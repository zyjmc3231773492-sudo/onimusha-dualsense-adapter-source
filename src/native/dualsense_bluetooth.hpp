#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

// Protocol references: daidr/dualsense-tester (MIT),
// awalol/dualsense-bt-haptics (MIT), Sony's hid-playstation driver.
// See legal/BLUETOOTH_THIRD_PARTY_NOTICES.txt.
namespace dualsense_bluetooth {
inline constexpr size_t ControlLength = 78;
inline constexpr size_t StreamLength = 398;
inline constexpr size_t SourceFrames = 512; // 10 2/3 ms at 48 kHz
inline constexpr size_t OpusFrames = 480;   // 45 kHz source, 48 kHz nominal
inline constexpr size_t OpusBytes = 200;
inline constexpr size_t HapticBytes = 64;  // 32 stereo samples at 3 kHz
inline constexpr uint8_t SpeakerVolume = 100; // same numeric setting, not measured acoustic equivalence
// Provisional -3 dB compensation for the observed louder Bluetooth speaker.
// Apply after the shared category mix/limiter and before Opus, exclusively to
// front speaker channels. Never scale the shared USB quad buffer or haptics.
inline constexpr float SpeakerOutputCompensationDb = -3.0f;
inline constexpr float SpeakerOutputScale = 0.707945784f; // 10^(-3/20)
inline constexpr size_t MaximumWindowsReportLength = 1024;
using ControlReport = std::array<uint8_t, ControlLength>;
using StreamReport = std::array<uint8_t, StreamLength>;

inline bool IsBluetoothCollection(size_t inputLength, size_t outputLength) {
    // Windows caps describe the longest report in the collection, rather than
    // the individual 0x31 / 0x36 packets. USB input reports are at most 64 bytes.
    return inputLength > 64 && outputLength >= StreamLength &&
        outputLength <= MaximumWindowsReportLength;
}

inline uint32_t OutputCrc(const uint8_t* data, size_t length) {
    uint32_t crc = 0xFFFFFFFFu;
    auto append = [&](uint8_t byte) {
        crc ^= byte;
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    };
    append(0xA2); // Bluetooth HID output transaction prefix
    for (size_t index = 0; index < length; ++index) append(data[index]);
    return ~crc;
}

template <size_t N> inline void Seal(std::array<uint8_t, N>& report) {
    const uint32_t crc = OutputCrc(report.data(), N - 4);
    for (unsigned byte = 0; byte < 4; ++byte)
        report[N - 4 + byte] = static_cast<uint8_t>(crc >> (byte * 8));
}

template <size_t N> inline ControlReport Control(
    const std::array<uint8_t, N>& logical, uint8_t sequence) {
    static_assert(N == 48);
    ControlReport report{};
    report[0] = 0x31;
    report[1] = static_cast<uint8_t>((sequence & 15u) << 4);
    report[2] = 0x10;
    std::copy(logical.begin() + 1, logical.end(), report.begin() + 3);
    Seal(report);
    return report;
}

inline std::array<uint8_t, HapticBytes> Haptics(const float* quad) {
    std::array<uint8_t, HapticBytes> result{};
    // A 16-tap box low-pass before decimation avoids directly aliasing the
    // 48 kHz waveform. Transport has signed 8-bit samples, not motor amplitude.
    for (size_t sample = 0; sample < 32; ++sample) {
        for (size_t channel = 0; channel < 2; ++channel) {
            float sum = 0.0f;
            for (size_t tap = 0; tap < 16; ++tap)
                sum += quad[(sample * 16 + tap) * 4 + channel + 2];
            const int value = static_cast<int>(std::lround(
                std::clamp(sum / 16.0f, -1.0f, 1.0f) * 127.0f));
            result[sample * 2 + channel] = static_cast<uint8_t>(value);
        }
    }
    return result;
}

inline std::array<float, OpusFrames * 2> Speaker(const float* quad) {
    std::array<float, OpusFrames * 2> result{};
    for (size_t frame = 0; frame < OpusFrames; ++frame) {
        const double position = static_cast<double>(frame) * 16.0 / 15.0;
        const size_t lower = static_cast<size_t>(position);
        const size_t upper = std::min(lower + 1, SourceFrames - 1);
        const float fraction = static_cast<float>(position - lower);
        for (size_t channel = 0; channel < 2; ++channel)
            result[frame * 2 + channel] = (quad[lower * 4 + channel] *
                (1.0f - fraction) + quad[upper * 4 + channel] * fraction) *
                SpeakerOutputScale;
    }
    return result;
}

inline StreamReport Stream(const std::array<uint8_t, OpusBytes>& opus,
    const std::array<uint8_t, HapticBytes>& pcm, uint8_t sequence,
    uint8_t counter, bool speakerEnabled = true) {
    StreamReport report{};
    report[0] = 0x36;
    report[1] = static_cast<uint8_t>((sequence & 15u) << 4);
    // Only audio fields are valid; streaming must not reset adaptive triggers,
    // rumble, lightbar, or LEDs sent in independent 0x31 control packets.
    report[2] = 0x90;
    report[3] = 0x3F;
    report[4] = 0xA0;
    report[9] = speakerEnabled ? SpeakerVolume : 0;
    report[11] = 0x09;
    report[67] = 0x91;
    report[68] = 0x07;
    report[69] = 0xFE;
    std::fill(report.begin() + 70, report.begin() + 75, uint8_t{0x40});
    report[75] = counter;
    report[76] = 0x93;
    report[77] = static_cast<uint8_t>(OpusBytes);
    std::copy(opus.begin(), opus.end(), report.begin() + 78);
    report[278] = 0x92;
    report[279] = static_cast<uint8_t>(HapticBytes);
    std::copy(pcm.begin(), pcm.end(), report.begin() + 280);
    Seal(report);
    return report;
}
} // namespace dualsense_bluetooth

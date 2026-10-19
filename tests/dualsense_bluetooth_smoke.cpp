#include <array>
#include <cmath>
#include <cstdio>
#include <opus.h>
#include "dualsense_bluetooth.hpp"
#include "dualsense_hid_report.hpp"

int main() {
    using namespace dualsense_bluetooth;
    auto fail = [](const char* message) { std::fprintf(stderr, "FAIL: %s\n", message); return 1; };
    auto crc = [](const auto& packet) {
        const size_t end = packet.size() - 4;
        uint32_t value = 0;
        for (unsigned byte = 0; byte < 4; ++byte)
            value |= static_cast<uint32_t>(packet[end + byte]) << (byte * 8);
        return value;
    };
    if (IsBluetoothCollection(64, 64) || IsBluetoothCollection(617, 64) ||
        IsBluetoothCollection(617, 2048) || !IsBluetoothCollection(617, 546))
        return fail("USB/Bluetooth collection classification");
    dualsense_hid_report::LogicalReport logical{};
    logical[0] = 2; logical[1] = 12; logical[11] = 5; logical[22] = 5;
    const auto control = Control(logical, 15);
    // Golden CRCs computed independently with Python's zlib.crc32, including
    // the 0xA2 transaction byte. They detect header/offset/prefix mistakes.
    if (control[13] != 5 || control[24] != 5 || crc(control) != 0xCA46F532u ||
        Control(logical, 16)[1] != 0)
        return fail("control report offsets, sequence wrap or golden CRC");
    std::array<uint8_t, OpusBytes> opus{};
    std::array<uint8_t, HapticBytes> pcm{};
    for (size_t i = 0; i < opus.size(); ++i) opus[i] = static_cast<uint8_t>(i);
    for (size_t i = 0; i < pcm.size(); ++i) pcm[i] = static_cast<uint8_t>(i);
    const auto stream = Stream(opus, pcm, 3, 254);
    if (crc(stream) != 0x7D231715u || stream[9] != 100 || stream[5] != 0 || stream[14] != 0 ||
        stream[25] != 0 || Stream(opus, pcm, 3, 254, false)[9] != 0)
        return fail("stream framing or audio packet changes trigger validity");

    std::array<float, SourceFrames * 4> quad{};
    for (size_t i = 0; i < SourceFrames; ++i) {
        quad[i * 4] = 0.35f * std::sin(static_cast<float>(i) * 0.04f);
        quad[i * 4 + 1] = quad[i * 4];
    }
    for (auto byte : Haptics(quad.data())) if (byte != 0) return fail("speaker leaks into haptics");
    quad.fill(0);
    for (size_t i = 0; i < SourceFrames; ++i) {
        quad[i * 4 + 2] = 0.5f;
        quad[i * 4 + 3] = -0.5f;
    }
    for (auto sample : Speaker(quad.data())) if (sample != 0) return fail("haptics leak into speaker");
    const auto haptics = Haptics(quad.data());
    for (size_t i = 0; i < HapticBytes; i += 2)
        if (haptics[i] != 64 || haptics[i + 1] != static_cast<uint8_t>(-64))
            return fail("signed stereo PCM mapping or gain");

    if (std::fabs(20.0f * std::log10(SpeakerOutputScale) -
        SpeakerOutputCompensationDb) > 0.0001f || SpeakerOutputScale >= 1.0f)
        return fail("speaker compensation must be the documented attenuation");
    // Exercise the actual Bluetooth extraction, not a duplicate mixer. Rear
    // channel PCM and the shared USB input must remain byte-for-byte unchanged.
    for (size_t i = 0; i < SourceFrames; ++i) {
        quad[i * 4] = 0.9f * std::sin(static_cast<float>(i) * 0.04f);
        quad[i * 4 + 1] = -0.25f + static_cast<float>(i) / 1024.0f;
    }
    const auto sourceBefore = quad;
    const auto pcmBefore = Haptics(quad.data());
    const auto compensated = Speaker(quad.data());
    for (size_t frame = 0; frame < OpusFrames; ++frame) {
        const double position = static_cast<double>(frame) * 16.0 / 15.0;
        const size_t lower = static_cast<size_t>(position);
        const size_t upper = std::min(lower + 1, SourceFrames - 1);
        const float fraction = static_cast<float>(position - lower);
        for (size_t channel = 0; channel < 2; ++channel) {
            const float unscaled = sourceBefore[lower * 4 + channel] *
                (1.0f - fraction) + sourceBefore[upper * 4 + channel] * fraction;
            if (std::fabs(compensated[frame * 2 + channel] -
                unscaled * SpeakerOutputScale) > 0.000001f)
                return fail("Bluetooth speaker-only compensation or channel mapping");
        }
    }
    if (quad != sourceBefore || Haptics(quad.data()) != pcmBefore)
        return fail("speaker compensation modifies shared USB input or haptics");
    quad.fill(0);
    for (auto sample : Speaker(quad.data()))
        if (sample != 0) return fail("speaker compensation must preserve digital silence");

    int error = OPUS_OK;
    OpusEncoder* encoder = opus_encoder_create(48000, 2, OPUS_APPLICATION_RESTRICTED_LOWDELAY, &error);
    OpusDecoder* decoder = opus_decoder_create(48000, 2, &error);
    if (encoder == nullptr || decoder == nullptr || error != OPUS_OK) return fail("Opus initialization");
    if (opus_encoder_ctl(encoder, OPUS_SET_BITRATE(160000)) != OPUS_OK ||
        opus_encoder_ctl(encoder, OPUS_SET_VBR(0)) != OPUS_OK ||
        opus_encoder_ctl(encoder, OPUS_SET_COMPLEXITY(1)) != OPUS_OK)
        return fail("Opus fixed-rate configuration");
    std::array<float, OpusFrames * 2> decoded{};
    double energy = 0;
    // Multiple frames verify valid Opus, constant payload size, and sound.
    for (size_t block = 0; block < 12; ++block) {
        for (size_t frame = 0; frame < SourceFrames; ++frame) {
            const float value = 0.3f * std::sin(static_cast<float>(frame + block * SourceFrames) * 0.04f);
            quad[frame * 4] = quad[frame * 4 + 1] = value;
        }
        const auto speaker = Speaker(quad.data());
        const int length = opus_encode_float(encoder, speaker.data(), 480, opus.data(), 200);
        if (length != 200 || opus_decode_float(decoder, opus.data(), length, decoded.data(), 480, 0) != 480)
            return fail("Opus frame encode/decode");
        for (float sample : decoded) energy += sample * sample;
    }
    opus_encoder_destroy(encoder);
    opus_decoder_destroy(decoder);
    if (energy < 10) return fail("speaker audio lost during codec round trip");
    std::puts("Bluetooth DEV: golden CRCs, trigger isolation, stereo PCM, speaker-only -3 dB compensation, unchanged USB source and Opus round trip passed.");
    return 0;
}

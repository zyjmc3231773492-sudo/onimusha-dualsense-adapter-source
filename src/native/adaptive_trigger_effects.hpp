#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace adaptive_trigger_effects {

constexpr size_t RightTriggerOffset = 0x0B;
constexpr size_t LeftTriggerOffset = 0x16;
constexpr size_t EffectBlockLength = 11;
constexpr uint8_t RightTriggerFlag = 0x04;
constexpr uint8_t LeftTriggerFlag = 0x08;
constexpr uint8_t TriggerEffectOff = 0x05;
constexpr uint8_t TriggerEffectVibration = 0x26;
constexpr uint8_t SoulAbsorbStartPosition = 7;
constexpr uint8_t SoulAbsorbAmplitude = 4;
constexpr uint8_t SoulAbsorbFrequencyHz = 40;

template <size_t Size>
void ClearEffectBlock(std::array<uint8_t, Size>& report, size_t offset) {
    if (offset + EffectBlockLength > report.size()) return;
    std::fill_n(report.begin() + offset, EffectBlockLength, uint8_t{0});
}

// DSX's simple Vibration mode applies one amplitude to every mechanical zone
// from startPosition through the end of trigger travel. The controller report
// stores the 1..8 amplitude as a packed 0..7 value for each active zone.
template <size_t Size>
bool EncodeDsxVibration(
    std::array<uint8_t, Size>& report,
    size_t offset,
    uint8_t startPosition,
    uint8_t amplitude,
    uint8_t frequencyHz) {
    if (offset + EffectBlockLength > report.size()) return false;

    ClearEffectBlock(report, offset);
    if (startPosition > 9 || amplitude < 1 || amplitude > 8 ||
        frequencyHz == 0) {
        report[offset] = TriggerEffectOff;
        return false;
    }

    uint16_t activeZones = 0;
    uint32_t strengthZones = 0;
    const uint32_t packedStrength = static_cast<uint32_t>(amplitude - 1u);
    for (uint8_t zone = startPosition; zone < 10; ++zone) {
        activeZones |= static_cast<uint16_t>(1u << zone);
        strengthZones |= packedStrength << (3u * zone);
    }

    report[offset + 0] = TriggerEffectVibration;
    report[offset + 1] = static_cast<uint8_t>(activeZones & 0xFFu);
    report[offset + 2] = static_cast<uint8_t>((activeZones >> 8u) & 0xFFu);
    report[offset + 3] = static_cast<uint8_t>(strengthZones & 0xFFu);
    report[offset + 4] = static_cast<uint8_t>((strengthZones >> 8u) & 0xFFu);
    report[offset + 5] = static_cast<uint8_t>((strengthZones >> 16u) & 0xFFu);
    report[offset + 6] = static_cast<uint8_t>((strengthZones >> 24u) & 0xFFu);
    report[offset + 9] = frequencyHz;
    return true;
}

template <size_t Size>
void DisableTrigger(
    std::array<uint8_t, Size>& report,
    uint8_t flag,
    size_t offset) {
    if (offset + EffectBlockLength > report.size()) return;
    report[1] |= flag;
    ClearEffectBlock(report, offset);
    report[offset] = TriggerEffectOff;
}

template <size_t Size>
std::array<uint8_t, Size> ApplyEnablement(
    const std::array<uint8_t, Size>& requested,
    bool leftEnabled,
    bool rightEnabled) {
    auto effective = requested;
    if (!rightEnabled) {
        DisableTrigger(effective, RightTriggerFlag, RightTriggerOffset);
    }
    if (!leftEnabled) {
        DisableTrigger(effective, LeftTriggerFlag, LeftTriggerOffset);
    }
    return effective;
}

template <size_t Size>
bool HasActiveEffect(const std::array<uint8_t, Size>& report) {
    const bool rightActive = (report[1] & RightTriggerFlag) != 0 &&
        report[RightTriggerOffset] != 0 &&
        report[RightTriggerOffset] != TriggerEffectOff;
    const bool leftActive = (report[1] & LeftTriggerFlag) != 0 &&
        report[LeftTriggerOffset] != 0 &&
        report[LeftTriggerOffset] != TriggerEffectOff;
    return rightActive || leftActive;
}

} // namespace adaptive_trigger_effects

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace captured_haptic_presets {

// These profiles are measurements of the three built-in PC vibration presets
// previously requested through PadVibrationManager.  Replaying the captured
// motor lanes in the helper preserves the audible-haptic result without
// constructing managed game objects or writing to the game's request queue.
enum class Preset : uint8_t {
    GetItem = 0,
    UseItem = 1,
    GenericInteract = 2,
};

struct Profile {
    float durationSeconds;
    std::array<float, 4> motorPower;
};

constexpr bool IsValid(uint8_t value) {
    return value <= static_cast<uint8_t>(Preset::GenericInteract);
}

constexpr Profile ProfileFor(Preset preset) {
    switch (preset) {
    case Preset::GetItem:
    case Preset::UseItem:
        return {0.100f, {0.100f, 0.0f, 0.0f, 0.0f}};
    case Preset::GenericInteract:
        return {0.150f, {0.200f, 0.200f, 0.0f, 0.0f}};
    }
    return {0.0f, {0.0f, 0.0f, 0.0f, 0.0f}};
}

inline float CalibrateMotorPower(float power) {
    return std::clamp(
        std::pow(std::clamp(power, 0.0f, 1.0f), 0.38f) * 1.08f,
        0.0f,
        1.0f);
}

} // namespace captured_haptic_presets

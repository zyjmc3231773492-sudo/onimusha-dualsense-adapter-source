#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace oni_transformation_haptics {

// Keep this distinct from wall_run_haptics::Message (0xE8). The helper uses a
// single-byte dispatcher, so duplicate IDs are consumed by the first branch.
constexpr uint8_t Message = 0xE6;
constexpr uint32_t SampleRate = 48000u;
constexpr uint32_t DurationFrames = 72000u; // 1.50 s

struct StereoSample { float left{}, right{}; };

inline double Smooth(double value) {
    value = std::clamp(value, 0.0, 1.0);
    return value * value * (3.0 - 2.0 * value);
}

inline StereoSample Sample(uint32_t frame) {
    if (frame >= DurationFrames) return {};
    constexpr double Pi = 3.14159265358979323846;
    constexpr double TwoPi = Pi * 2.0;
    const double t = static_cast<double>(frame) / SampleRate;

    // Button confirmation: a deep, rounded ignition rather than a UI click.
    const double ignitionAttack = Smooth(t / 0.018);
    const double ignition = ignitionAttack * std::exp(-t / 0.105);
    const double ignitionWave =
        0.76 * std::sin(TwoPi * 54.0 * t) +
        0.24 * std::sin(TwoPi * 91.0 * t + 0.18);

    // Energy gathers from alternating sides and climbs in pitch/pressure.
    const double gatherStart = Smooth((t - 0.075) / 0.16);
    const double gatherEnd = Smooth((0.94 - t) / 0.16);
    const double gather = gatherStart * gatherEnd;
    const double gatherProgress = std::clamp((t - 0.075) / 0.865, 0.0, 1.0);
    const double gatherPhase = TwoPi *
        (58.0 * t + 42.0 * t * t + 12.0 * t * t * t);
    const double gatherWave =
        0.70 * std::sin(gatherPhase) +
        0.30 * std::sin(TwoPi * (126.0 * t + 31.0 * t * t));
    const double lateral = std::sin(TwoPi * (2.35 * t + 0.35 * t * t));
    const double pressure = gather * (0.15 + 0.43 * gatherProgress);

    // Transformation lock: broad centre-weighted release, followed by a
    // short powered aura tail so completion feels final without buzzing on.
    const double burstDelta = t - 0.94;
    const double burstAttack = Smooth((burstDelta + 0.018) / 0.018);
    const double burst = burstDelta >= -0.018
        ? burstAttack * std::exp(-std::max(burstDelta, 0.0) / 0.155)
        : 0.0;
    const double burstWave =
        0.68 * std::cos(TwoPi * 47.0 * (burstDelta + 0.018)) +
        0.32 * std::sin(TwoPi * 88.0 * (burstDelta + 0.018));
    const double tail = t >= 1.03
        ? Smooth((t - 1.03) / 0.055) * Smooth((1.50 - t) / 0.25)
        : 0.0;
    const double tailWave =
        0.74 * std::sin(TwoPi * 76.0 * t) +
        0.26 * std::sin(TwoPi * 139.0 * t + 0.31);

    const double left =
        0.46 * ignition * ignitionWave +
        pressure * (0.78 + 0.22 * lateral) * gatherWave +
        1.02 * burst * burstWave + 0.20 * tail * tailWave;
    const double right =
        0.43 * ignition * ignitionWave +
        pressure * (0.78 - 0.22 * lateral) * gatherWave +
        1.02 * burst * burstWave + 0.22 * tail * tailWave;
    return {
        static_cast<float>(0.985 * std::tanh(left * 1.28)),
        static_cast<float>(0.985 * std::tanh(right * 1.28))};
}

} // namespace oni_transformation_haptics

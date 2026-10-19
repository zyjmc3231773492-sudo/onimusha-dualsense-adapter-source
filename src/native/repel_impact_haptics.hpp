#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace repel_impact_haptics {

constexpr uint32_t SampleRate = 48000;
constexpr uint32_t DurationFrames(bool projectile) {
    return projectile ? 9120u : 11520u;  // 190 / 240 ms, one decaying strike
}

struct StereoSample { float left; float right; };

// A single low, weighty metal strike. This intentionally shares no carrier
// with the old haptic_2 sample: the left actuator carries the 56 Hz mass while
// the right adds a slightly higher ring, both under one decaying envelope.
inline StereoSample Sample(uint32_t frame, bool projectile, float intensity) {
    const uint32_t frames = DurationFrames(projectile);
    if (frame >= frames || intensity <= 0.0f) return {};
    intensity = std::clamp(intensity, 0.0f, 2.0f);
    const double t = static_cast<double>(frame) / SampleRate;
    constexpr double TwoPi = 6.28318530717958647692;
    const double attackX = std::min(t / 0.0015, 1.0);
    const double attack = attackX * attackX * (3.0 - 2.0 * attackX);
    const double hold = projectile ? 0.026 : 0.044;
    const double decay = projectile ? 0.035 : 0.048;
    const double release = std::exp(-std::max(t - hold, 0.0) / decay);
    const double remaining = static_cast<double>(frames - 1 - frame) / SampleRate;
    const double fade = std::clamp(remaining / 0.020, 0.0, 1.0);
    const double envelope = attack * release * fade;

    const double baseFrequency = projectile ? 74.0 : 56.0;
    const double fall = projectile ? 18.0 : 12.0;
    const double bodyPhase = TwoPi * (baseFrequency * t - fall * t * t);
    const double ringPhase = TwoPi * ((projectile ? 148.0 : 116.0) * t -
        (projectile ? 42.0 : 31.0) * t * t);
    const double contact = std::exp(-t / (projectile ? 0.013 : 0.018));
    const double leftCarrier = 0.91 * std::sin(bodyPhase) +
        0.09 * contact * std::sin(ringPhase);
    const double rightCarrier = 0.84 * std::sin(bodyPhase + 0.055) +
        0.16 * contact * std::sin(ringPhase + 0.10);
    const double drive = 1.85 * intensity;
    return {
        static_cast<float>(envelope * 0.978 * std::tanh(leftCarrier * drive)),
        static_cast<float>(envelope * 0.978 * std::tanh(rightCarrier * drive))};
}

}  // namespace repel_impact_haptics

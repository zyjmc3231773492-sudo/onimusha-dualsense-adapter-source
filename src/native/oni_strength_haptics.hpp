#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace oni_strength_haptics {

constexpr uint32_t SampleRate = 48000u;
constexpr uint32_t FirstPalmFrame = 27840u;   // 0.58 s after action entry
constexpr uint32_t SecondPalmFrame = 43680u;  // 0.91 s after action entry
constexpr uint32_t DestroyDurationFrames = 58560u; // 1.22 s

struct StereoSample {
    float left;
    float right;
};

inline StereoSample SampleDestroy(uint32_t frame, float intensity = 1.0f) {
    if (frame >= DestroyDurationFrames || intensity <= 0.0f) {
        return {0.0f, 0.0f};
    }

    constexpr double Pi = 3.14159265358979323846;
    constexpr double TwoPi = Pi * 2.0;
    const double time = static_cast<double>(frame) /
        static_cast<double>(SampleRate);

    // Keep the existing wrist-lift texture short. The quiet gap after it is
    // deliberate: it prevents the two later palm contacts from feeling like
    // one continuous motor buzz.
    const double chargeAttack = std::clamp(time / 0.030, 0.0, 1.0);
    const double charge = time < 0.34
        ? chargeAttack * std::exp(-time / 0.150)
        : 0.0;
    const double chargeCarrier =
        0.72 * std::sin(TwoPi * 82.0 * time) +
        0.28 * std::sin(TwoPi * 137.0 * time);

    const auto palm = [time](double centerSeconds, bool leftLeading) {
        const double delta = time - centerSeconds;
        if (delta < -0.010 || delta > 0.280) {
            return StereoSample{0.0f, 0.0f};
        }

        // A fast contact edge followed by a dense low-frequency body and a
        // short recoil. The asymmetric cross-feed makes each palm readable
        // without turning the hit into a thin, high-pitched click.
        const double attack = std::clamp((delta + 0.010) / 0.010, 0.0, 1.0);
        const double decayTime = std::max(delta, 0.0);
        const double body = attack * std::exp(-decayTime / 0.090);
        const double recoil = delta >= 0.075
            ? std::exp(-(delta - 0.075) / 0.085)
            : 0.0;
        const double localTime = delta + 0.010;
        const double carrier =
            0.62 * std::cos(TwoPi * 58.0 * localTime) +
            0.27 * std::sin(TwoPi * 91.0 * localTime) +
            0.11 * std::sin(TwoPi * 151.0 * localTime);
        const double shaped = std::tanh(
            carrier * (2.85 * body + 0.72 * recoil));
        const float lead = static_cast<float>(0.985 * shaped);
        const float cross = static_cast<float>(0.315 * shaped);
        return leftLeading
            ? StereoSample{lead, cross}
            : StereoSample{cross, lead};
    };

    const auto first = palm(
        static_cast<double>(FirstPalmFrame) / SampleRate, true);
    const auto second = palm(
        static_cast<double>(SecondPalmFrame) / SampleRate, false);
    const double gain = std::clamp(static_cast<double>(intensity), 0.0, 2.0);
    const double left = gain * (
        0.285 * charge * chargeCarrier + first.left + second.left);
    const double right = gain * (
        0.165 * charge * chargeCarrier + first.right + second.right);

    return {
        static_cast<float>(0.985 * std::tanh(left * 1.65)),
        static_cast<float>(0.985 * std::tanh(right * 1.65))};
}

} // namespace oni_strength_haptics

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace oni_charge_release_haptics {

constexpr uint8_t Message = 0xE5;
constexpr uint32_t SampleRate = 48000u;
enum class Kind : uint8_t {
    None = 0,
    Light = 1,
    Heavy = 2,
    FinisherDraw = 3,
    FinisherSlash = 4
};
constexpr uint32_t Duration(Kind kind) {
    if (kind == Kind::FinisherDraw) return 7200u;    // 150 ms readable partial draw
    if (kind == Kind::FinisherSlash) return 124800u; // 2.6 s cut, wave and tree growth
    return kind == Kind::Heavy ? 24960u :
        (kind == Kind::Light ? 20160u : 0u);
}
struct StereoSample { float left{}, right{}; };
inline double Smooth(double value) {
    value = std::clamp(value, 0.0, 1.0);
    return value * value * (3.0 - 2.0 * value);
}
inline StereoSample Sample(uint32_t frame, Kind kind) {
    const uint32_t duration = Duration(kind);
    if (frame >= duration) return {};
    constexpr double Pi = 3.14159265358979323846;
    constexpr double TwoPi = Pi * 2.0;
    const bool heavy = kind == Kind::Heavy;
    const double time = static_cast<double>(frame) / SampleRate;
    const double seconds = static_cast<double>(duration) / SampleRate;
    const double progress = time / seconds;

    if (kind == Kind::FinisherDraw) {
        // One clearly readable partial-unsheathe snap, biased toward the hand
        // drawing the blade. Keep its carriers in the DualSense's useful
        // tactile band instead of relying on a quiet high-pitched steel tick.
        const double envelope = Smooth(time / 0.0020) * std::exp(-time / 0.068) *
            Smooth((seconds - time) / 0.028);
        const double steel =
            0.56 * std::sin(TwoPi * (62.0 * time + 26.0 * time * time)) +
            0.30 * std::sin(TwoPi * 116.0 * time + 0.31) +
            0.14 * std::sin(TwoPi * 184.0 * time + 0.78);
        const double catchTime = time - 0.055;
        const double catchEnvelope = catchTime > 0.0
            ? Smooth(catchTime / 0.0025) * std::exp(-catchTime / 0.052)
            : 0.0;
        const double catchWave =
            0.72 * std::sin(TwoPi * 74.0 * catchTime + 0.20) +
            0.28 * std::sin(TwoPi * 148.0 * catchTime + 0.64);
        const float left = static_cast<float>(
            1.05 * envelope * steel + 0.46 * catchEnvelope * catchWave);
        const float right = static_cast<float>(
            1.42 * envelope * steel + 0.62 * catchEnvelope * catchWave);
        return {0.985f * std::tanh(left * 1.22f),
            0.985f * std::tanh(right * 1.22f)};
    }

    if (kind == Kind::FinisherSlash) {
        // The game's native motor event marks the real cut, so this voice has
        // no lead-in. The first front is the blade itself; a wide pressure wave
        // and staggered ground-growth ridges then carry through the long visual
        // effect instead of collapsing into a short point vibration.
        const double cutEnvelope = Smooth(time / 0.0018) * std::exp(-time / 0.220);
        const double cut =
            0.66 * std::sin(TwoPi * (58.0 * time - 3.0 * time * time)) +
            0.23 * std::sin(TwoPi * 106.0 * time + 0.24) +
            0.11 * std::sin(TwoPi * 172.0 * time + 0.67);
        const double airEnvelope = Smooth((time - 0.012) / 0.010) *
            std::exp(-std::max(time - 0.012, 0.0) / 0.920) *
            Smooth((seconds - time) / 0.280);
        const double air =
            0.76 * std::sin(TwoPi * (86.0 * time + 34.0 * time * time)) +
            0.24 * std::sin(TwoPi * 172.0 * time + 0.58);
        const double leftPan = 0.54 + 0.46 * Smooth((0.70 - progress) / 0.58);
        const double rightPan = 0.54 + 0.46 * Smooth((progress - 0.02) / 0.58);
        const double impactTime = time - 0.105;
        const double impactEnvelope = impactTime > 0.0
            ? Smooth(impactTime / 0.003) * std::exp(-impactTime / 0.165)
            : 0.0;
        const double impact =
            0.72 * std::sin(TwoPi * 58.0 * impactTime) +
            0.20 * std::sin(TwoPi * 97.0 * impactTime + 0.44) +
            0.08 * std::sin(TwoPi * 157.0 * impactTime + 0.81);

        // Eight broad, partly overlapping eruptions follow the expanding cut.
        // Each has a dense body and recoil (not a single click), alternating
        // across the two actuators as the line of trees travels outwards.
        constexpr double growthCentres[] = {
            0.28, 0.52, 0.78, 1.05, 1.34, 1.64, 1.96, 2.24};
        constexpr double growthWeights[] = {
            1.00, 0.96, 0.91, 0.86, 0.80, 0.74, 0.67, 0.58};
        double growthLeft = 0.0;
        double growthRight = 0.0;
        for (size_t index = 0; index < 8; ++index) {
            const double delta = time - growthCentres[index];
            if (delta < -0.012 || delta > 0.42) continue;
            const double localTime = delta + 0.012;
            const double attack = Smooth(localTime / 0.012);
            const double decayTime = std::max(delta, 0.0);
            const double body = attack * std::exp(-decayTime / 0.155);
            const double recoil = delta >= 0.085
                ? std::exp(-(delta - 0.085) / 0.150)
                : 0.0;
            const double carrier =
                0.68 * std::cos(TwoPi * 58.0 * localTime) +
                0.23 * std::sin(TwoPi * 97.0 * localTime + 0.22) +
                0.09 * std::sin(TwoPi * 157.0 * localTime + 0.61);
            const double shaped = std::tanh(
                carrier * (2.20 * body + 0.62 * recoil)) * growthWeights[index];
            const bool leftLeading = (index & 1u) == 0u;
            growthLeft += shaped * (leftLeading ? 0.88 : 0.48);
            growthRight += shaped * (leftLeading ? 0.48 : 0.92);
        }
        const double groundBedEnvelope = Smooth((time - 0.075) / 0.050) *
            std::exp(-std::max(time - 0.075, 0.0) / 1.850) *
            Smooth((seconds - time) / 0.300);
        const double groundBed =
            0.72 * std::sin(TwoPi * 66.0 * time + 0.18) +
            0.28 * std::sin(TwoPi * 132.0 * time + 0.63);
        const float left = static_cast<float>(
            1.72 * cutEnvelope * cut + 1.18 * airEnvelope * air * leftPan +
            1.34 * impactEnvelope * impact +
            0.72 * groundBedEnvelope * groundBed + 0.94 * growthLeft);
        const float right = static_cast<float>(
            1.80 * cutEnvelope * cut + 1.26 * airEnvelope * air * rightPan +
            1.44 * impactEnvelope * impact +
            0.78 * groundBedEnvelope * groundBed + 0.98 * growthRight);
        return {0.99f * std::tanh(left * 1.12f),
            0.99f * std::tanh(right * 1.12f)};
    }

    // A rounded but unmistakable breakaway impulse ends the charge bed.
    const double breakaway = Smooth(time / 0.0035) * std::exp(-time / (heavy ? 0.105 : 0.078));
    const double breakBody =
        0.76 * std::sin(TwoPi * ((heavy ? 58.0 : 70.0) * time - 8.0 * time * time)) +
        0.24 * std::sin(TwoPi * ((heavy ? 103.0 : 121.0) * time) + 0.28);

    // The following blade-air sheet moves left to right and climbs in pitch.
    const double airEnvelope = Smooth((time - 0.014) / 0.020) *
        std::pow(std::sin(Pi * progress), 0.90) * Smooth((seconds - time) / 0.075);
    const double airPhase = TwoPi * ((heavy ? 132.0 : 154.0) * time +
        (heavy ? 92.0 : 116.0) * time * time);
    const double air = 0.82 * std::sin(airPhase) +
        0.18 * std::sin(TwoPi * ((heavy ? 246.0 : 282.0) * time) + 0.55);
    const double leftPan = 0.52 + 0.48 * Smooth((0.72 - progress) / 0.55);
    const double rightPan = 0.52 + 0.48 * Smooth((progress - 0.04) / 0.55);

    // A second pressure ridge keeps the heavy release from reading as a click.
    const double ridgeTime = time - (heavy ? 0.145 : 0.115);
    const double ridge = ridgeTime > 0.0
        ? Smooth(ridgeTime / 0.010) * std::exp(-ridgeTime / (heavy ? 0.090 : 0.065)) *
            std::sin(TwoPi * (heavy ? 76.0 : 88.0) * ridgeTime)
        : 0.0;
    const float left = static_cast<float>(
        (heavy ? 1.02 : 0.86) * breakaway * breakBody +
        (heavy ? 0.82 : 0.69) * airEnvelope * air * leftPan +
        (heavy ? 0.48 : 0.34) * ridge);
    const float right = static_cast<float>(
        (heavy ? 0.92 : 0.77) * breakaway * breakBody +
        (heavy ? 0.90 : 0.76) * airEnvelope * air * rightPan +
        (heavy ? 0.54 : 0.38) * ridge);
    return {0.98f * std::tanh(left / 0.98f), 0.98f * std::tanh(right / 0.98f)};
}

} // namespace oni_charge_release_haptics

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace bow_shot_haptics {

constexpr uint8_t Message = 0xE4;
constexpr uint32_t SampleRate = 48000u;

enum class Kind : uint8_t {
    Quick = 1,
    Charged = 2
};

constexpr uint32_t Duration(Kind kind) {
    return kind == Kind::Charged ? 13440u : 6240u; // 280 / 130 ms
}

struct StereoSample {
    float left{};
    float right{};
};

inline double Smooth(double value) {
    value = std::clamp(value, 0.0, 1.0);
    return value * value * (3.0 - 2.0 * value);
}

inline StereoSample Sample(uint32_t frame, Kind kind) {
    const uint32_t duration = Duration(kind);
    if (frame >= duration) return {};

    constexpr double Pi = 3.14159265358979323846;
    constexpr double TwoPi = Pi * 2.0;
    const bool charged = kind == Kind::Charged;
    const double time = static_cast<double>(frame) / SampleRate;
    const double seconds = static_cast<double>(duration) / SampleRate;
    const double progress = time / seconds;

    // A very fast string breakaway, biased slightly toward the right hand.
    const double snap = Smooth(time / (charged ? 0.0035 : 0.0025)) *
        std::exp(-time / (charged ? 0.072 : 0.041));
    const double snapWave =
        0.70 * std::sin(TwoPi * (charged ? 72.0 : 94.0) * time) +
        0.30 * std::sin(TwoPi * (charged ? 151.0 : 196.0) * time + 0.42);

    // The arrow leaving the bow is a short rising sheet, not a blunt impact.
    const double flightAttack = Smooth((time - 0.010) / 0.012);
    const double flightRelease = Smooth((seconds - time) /
        (charged ? 0.070 : 0.035));
    const double flightEnvelope = flightAttack * flightRelease *
        std::pow(std::sin(Pi * progress), charged ? 0.78 : 1.10);
    const double flightPhase = TwoPi * (
        (charged ? 118.0 : 154.0) * time +
        (charged ? 105.0 : 138.0) * time * time);
    const double flight =
        0.82 * std::sin(flightPhase) +
        0.18 * std::sin(TwoPi * (charged ? 245.0 : 306.0) * time + 0.65);

    // Charged shots retain a low bow-body recoil after the string has snapped.
    const double bodyTime = time - 0.022;
    const double body = charged && bodyTime > 0.0
        ? Smooth(bodyTime / 0.012) * std::exp(-bodyTime / 0.105) *
            std::sin(TwoPi * 57.0 * bodyTime)
        : 0.0;

    const double snapGain = charged ? 0.72 : 0.46;
    const double flightGain = charged ? 0.52 : 0.34;
    const float left = static_cast<float>(
        snapGain * snap * snapWave * 0.88 +
        flightGain * flightEnvelope * flight * 0.93 +
        0.38 * body);
    const float right = static_cast<float>(
        snapGain * snap * snapWave +
        flightGain * flightEnvelope * flight +
        0.43 * body);
    return {
        0.92f * std::tanh(left / 0.92f),
        0.92f * std::tanh(right / 0.92f)};
}

} // namespace bow_shot_haptics

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace black_soul_haptics {

constexpr uint8_t Profile = 4u;
constexpr uint32_t SampleRate = 48000u;
constexpr uint64_t EvidenceTimeoutMs = 420u;
constexpr uint32_t HoldFrames = 19200u;
constexpr float PullAmplitude = 0.60f;
constexpr uint32_t FinishFrames = 11520u; // 240 ms, one weighted settle
constexpr uint64_t FinishDurationMs = FinishFrames * 1000u / SampleRate;
constexpr float FinishReleaseSmoothing = 0.010f;
constexpr uint32_t FinishHoldFrames = 7680u; // 160 ms at full body strength
constexpr float FinishBodyGain = 1.25f; // compensate the short settling window

// Both enum layouts were checked against the installed game's TypeDB:
// NONE=0, ABSORB=1, FINISH=2. Completion ends the loop, then settles once.
constexpr bool IsAbsorbing(int32_t mainState, int32_t subState, bool paused) {
    return !paused && (mainState == 1 || subState == 1);
}

constexpr bool Completed(int32_t previousMain, int32_t previousSub,
    int32_t mainState, int32_t subState, bool paused, bool wasAbsorbing) {
    return wasAbsorbing && !paused && !IsAbsorbing(mainState, subState, false) &&
        ((previousMain == 1 && mainState == 2) ||
         (previousSub == 1 && subState == 2));
}

constexpr bool EvidenceFresh(uint64_t now, uint64_t observed) {
    return observed != 0 && now >= observed &&
        now - observed < EvidenceTimeoutMs;
}

struct StereoSample { float left; float right; };

inline StereoSample Sample(uint32_t frame, float level) {
    constexpr double TwoPi = 6.28318530717958647692;
    const double time = static_cast<double>(frame) / SampleRate;
    const double flow = 0.84 + 0.12 * std::sin(TwoPi * 2.3 * time) +
        0.04 * std::sin(TwoPi * 0.7 * time);
    const double wave = 0.72 * std::sin(TwoPi * 76.0 * time) +
        0.18 * std::sin(TwoPi * 129.0 * time) +
        0.10 * std::sin(TwoPi * 181.0 * time);
    const float pull = static_cast<float>(PullAmplitude * flow * wave) *
        std::clamp(level, 0.0f, 1.0f);
    return {pull, pull * 0.55f};
}

inline StereoSample FinishSample(uint32_t frame, float intensity) {
    if (frame >= FinishFrames || intensity <= 0.0f) return {};
    const double time = static_cast<double>(frame) / SampleRate;
    const double attackX = std::min(time / 0.004, 1.0);
    const double attack = attackX * attackX * (3.0 - 2.0 * attackX);
    const double releaseX = std::clamp(
        (static_cast<double>(frame) - FinishHoldFrames) /
        (FinishFrames - 1u - FinishHoldFrames), 0.0, 1.0);
    const double release = 1.0 - releaseX * releaseX * (3.0 - 2.0 * releaseX);
    // Reuse the accepted absorption carrier. Peak alone did not produce the
    // same felt weight with the previous 58 Hz / 45 ms decaying pulse.
    // Keep the energy present before rounding it off; do not extend the cue.
    const auto pull = Sample(frame, intensity);
    const float settle = pull.left * FinishBodyGain *
        static_cast<float>(attack * release);
    return {settle, settle * 0.82f};
}

} // namespace black_soul_haptics

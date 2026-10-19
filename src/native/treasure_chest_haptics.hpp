#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace treasure_chest_haptics {

enum class Stage : uint8_t {
    Open,
    Push,
    LidLand,
};

// The observed regular-chest lifecycle reaches OPENED 2508-2512 ms after
// OPENING, while the visible lid contact precedes that state. Fire the visual
// impact from the stable OPENING edge and retain OPENED only as a fallback.
inline constexpr uint64_t FallbackLidImpactDelayMs = 1700;
inline constexpr uint64_t LidImpactDedupeMs = 1200;
inline constexpr uint64_t OpenCueFallbackDelayMs = 900;
inline constexpr uint64_t PushCueDelayMs = 600;

struct Frame {
    float amplitude = 0.0f;
    float leftGain = 1.0f;
    float rightGain = 1.0f;
    float waveMixA = 1.0f;
    float waveMixB = 0.0f;
    float noiseMix = 0.0f;
    double frequencyA = 100.0;
    double frequencyB = 180.0;
    double modulationFrequency = 4.0;
};

inline constexpr float DurationSeconds(Stage stage) {
    switch (stage) {
    case Stage::Open:
        return 0.62f;
    case Stage::Push:
        // OPENED cancels this voice. This is only a safety ceiling for a
        // missing state transition and must outlive the measured 2.5 s lid
        // animation.
        return 3.20f;
    case Stage::LidLand:
        return 0.48f;
    }
    return 0.0f;
}

inline Frame Evaluate(Stage stage, float progress, float attack,
    float modulationWave) {
    progress = std::clamp(progress, 0.0f, 1.0f);
    attack = std::clamp(attack, 0.0f, 1.0f);
    Frame frame{};

    switch (stage) {
    case Stage::Open: {
        // The first physical stage is not a lock. The character lifts the
        // closed lid into a heavy half-open position. Model breakaway force,
        // rising load and a firm half-open stop before the separate push bed.
        const float breakaway = std::exp(
            -78.0f * (progress - 0.10f) * (progress - 0.10f));
        const float risingLoad = attack * (0.40f + 0.60f * progress);
        const float halfOpenStop = std::exp(
            -105.0f * (progress - 0.88f) * (progress - 0.88f));
        const float slowStrain = 0.88f +
            0.12f * std::fabs(modulationWave);
        frame.amplitude = slowStrain *
            (0.52f * breakaway + 0.58f * risingLoad +
             0.62f * halfOpenStop);
        frame.leftGain = 1.06f;
        frame.rightGain = 0.96f;
        frame.waveMixA = 0.72f;
        frame.waveMixB = 0.16f;
        frame.noiseMix = 0.12f;
        frame.frequencyA = 62.0 + 32.0 * progress;
        frame.frequencyB = 108.0 + 35.0 * progress;
        frame.modulationFrequency = 3.8;
        break;
    }
    case Stage::Push: {
        // Slowly engage a rough wooden hinge. Low carriers, asymmetric travel,
        // filtered grain and visible load modulation make this feel like a
        // lid being pushed rather than another latch/impact pulse.
        const float rise = 1.0f - std::exp(-5.2f * progress);
        const float slowLoad = 0.72f + 0.10f * std::sin(
            3.14159265358979323846f * std::min(progress / 0.82f, 1.0f));
        const float hingeGrain = 0.68f +
            0.32f * std::fabs(modulationWave);
        frame.amplitude = attack * rise * slowLoad * hingeGrain * 1.12f;
        frame.leftGain = 1.08f - 0.30f * progress;
        frame.rightGain = 0.68f + 0.34f * progress;
        frame.waveMixA = 0.54f;
        frame.waveMixB = 0.10f;
        frame.noiseMix = 0.36f;
        frame.frequencyA = 64.0 + 11.0 * progress +
            3.0 * modulationWave;
        frame.frequencyB = 108.0 + 15.0 * progress;
        frame.modulationFrequency = 3.4;
        break;
    }
    case Stage::LidLand: {
        // A wide low-mid thud, not a sub-50 Hz half-cycle. It retains a heavy
        // character while remaining inside the DualSense actuator's useful
        // tactile band, followed by two clearly smaller rebounds.
        const float impact = std::exp(
            -92.0f * (progress - 0.09f) * (progress - 0.09f));
        const float body = attack * std::exp(-4.2f * progress);
        const float rebound = std::exp(
            -175.0f * (progress - 0.43f) * (progress - 0.43f));
        const float settle = std::exp(
            -225.0f * (progress - 0.74f) * (progress - 0.74f));
        frame.amplitude = 1.02f * impact + 0.38f * body +
            0.42f * rebound + 0.22f * settle;
        frame.leftGain = 1.08f;
        frame.rightGain = 1.00f;
        frame.waveMixA = 0.80f;
        frame.waveMixB = 0.17f;
        frame.noiseMix = 0.03f;
        frame.frequencyA = 78.0 + 8.0 * (1.0f - progress);
        frame.frequencyB = 132.0 + 12.0 * progress;
        frame.modulationFrequency = 3.6;
        break;
    }
    }
    return frame;
}

} // namespace treasure_chest_haptics

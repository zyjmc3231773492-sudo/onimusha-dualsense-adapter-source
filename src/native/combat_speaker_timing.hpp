#pragma once

#include <algorithm>
#include <cstddef>
#include "combat_defense_feedback.hpp"

namespace combat_speaker_timing {

constexpr size_t SampleRate = 48000u;
constexpr size_t DeflectPlaybackFrames = 29760u; // 0.62 s
constexpr size_t DeflectFadeFrames = 6720u;      // final 0.14 s

constexpr size_t PlaybackFrames(bool deflect, size_t sourceFrames,
    combat_defense_feedback::DeflectSpeakerVariant variant =
        combat_defense_feedback::DeflectSpeakerVariant::Short) {
    return deflect && variant != combat_defense_feedback::DeflectSpeakerVariant::Slow
        ? std::min(sourceFrames, DeflectPlaybackFrames)
        : sourceFrames;
}

inline float Envelope(bool deflect, size_t frame, size_t playbackFrames) {
    if (frame >= playbackFrames) return 0.0f;
    if (!deflect || playbackFrames <= DeflectFadeFrames) return 1.0f;
    const size_t fadeStart = playbackFrames - DeflectFadeFrames;
    if (frame < fadeStart) return 1.0f;
    const float remaining = static_cast<float>(playbackFrames - frame) /
        static_cast<float>(DeflectFadeFrames);
    const float x = std::clamp(remaining, 0.0f, 1.0f);
    return x * x * (3.0f - 2.0f * x);
}

} // namespace combat_speaker_timing

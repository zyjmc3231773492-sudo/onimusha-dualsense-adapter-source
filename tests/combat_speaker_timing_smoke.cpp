#include <cmath>
#include <cstdio>

#include "combat_speaker_timing.hpp"

int main() {
    using namespace combat_speaker_timing;
    using combat_defense_feedback::DeflectSpeakerVariant;
    bool passed = true;

    if (PlaybackFrames(true, 84989u) != DeflectPlaybackFrames ||
        PlaybackFrames(true, 12000u) != 12000u ||
        PlaybackFrames(false, 84989u) != 84989u ||
        PlaybackFrames(true, 84989u, DeflectSpeakerVariant::Slow) != 84989u ||
        PlaybackFrames(true, 84989u, DeflectSpeakerVariant::ContactShort) != DeflectPlaybackFrames ||
        PlaybackFrames(true, 12000u, DeflectSpeakerVariant::Slow) != 12000u ||
        PlaybackFrames(false, 84989u, DeflectSpeakerVariant::Slow) != 84989u) {
        std::fprintf(stderr, "playback frame cap failed\n");
        passed = false;
    }

    // Slow mode must retain the separate original recording beyond the short
    // 0.62-s limit. It still fades once at its own end, with no looping.
    if (Envelope(true, DeflectPlaybackFrames, 84989u) != 1.0f ||
        Envelope(true, 84989u, 84989u) != 0.0f ||
        std::abs(Envelope(true, 84989u - DeflectFadeFrames / 2, 84989u)
            - 0.5f) > 0.0001f) {
        std::fprintf(stderr, "slow original recording truncated/fade failed\n");
        passed = false;
    }

    const size_t fadeStart = DeflectPlaybackFrames - DeflectFadeFrames;
    const float beforeFade = Envelope(
        true, fadeStart > 0 ? fadeStart - 1 : 0, DeflectPlaybackFrames);
    const float halfFade = Envelope(
        true, fadeStart + DeflectFadeFrames / 2, DeflectPlaybackFrames);
    const float finalFrame = Envelope(
        true, DeflectPlaybackFrames - 1, DeflectPlaybackFrames);
    const float afterEnd = Envelope(
        true, DeflectPlaybackFrames, DeflectPlaybackFrames);
    if (std::abs(beforeFade - 1.0f) > 0.0001f ||
        std::abs(halfFade - 0.5f) > 0.0001f ||
        finalFrame < 0.0f || finalFrame > 0.001f ||
        afterEnd != 0.0f ||
        Envelope(false, DeflectPlaybackFrames - 1,
            DeflectPlaybackFrames) != 1.0f) {
        std::fprintf(stderr,
            "fade envelope failed before=%.4f half=%.4f final=%.6f end=%.4f\n",
            beforeFade, halfFade, finalFrame, afterEnd);
        passed = false;
    }

    if (!passed) return 1;
    std::printf(
        "combat speaker timing passed: deflect=%.2fs fade=%.2fs\n",
        static_cast<double>(DeflectPlaybackFrames) / SampleRate,
        static_cast<double>(DeflectFadeFrames) / SampleRate);
    return 0;
}

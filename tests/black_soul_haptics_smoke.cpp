#include <algorithm>
#include <cmath>
#include <cstdio>
#include "black_soul_haptics.hpp"

using namespace black_soul_haptics;
static_assert(IsAbsorbing(1, 0, false));
static_assert(IsAbsorbing(0, 1, false));
static_assert(IsAbsorbing(2, 1, false));
static_assert(!IsAbsorbing(2, 2, false));
static_assert(!IsAbsorbing(0, 0, false));
static_assert(!IsAbsorbing(1, 1, true));
static_assert(!IsAbsorbing(-1, 3, false));
static_assert(EvidenceFresh(1419, 1000));
static_assert(!EvidenceFresh(1420, 1000));
static_assert(!EvidenceFresh(900, 1000));
static_assert(!EvidenceFresh(1000, 0));
static_assert(PullAmplitude == 0.60f); // accepted ongoing feel must not change
static_assert(FinishFrames == 11520u && FinishDurationMs == 240u);
static_assert(FinishHoldFrames == 7680u);
static_assert(PullAmplitude * FinishBodyGain < 1.0f);
static_assert(Completed(1, 0, 2, 0, false, true));
static_assert(Completed(0, 1, 0, 2, false, true));
static_assert(!Completed(1, 0, 0, 0, false, true)); // cancellation
static_assert(!Completed(1, 0, 2, 0, true, true)); // menu/pause
static_assert(!Completed(1, 0, 2, 0, false, false)); // watchdog/reset disarmed
static_assert(!Completed(2, 0, 2, 0, false, true)); // repeated FINISH
static_assert(!Completed(1, 2, 0, 2, false, true)); // stale other completion
static_assert(!Completed(1, 1, 2, 1, false, true)); // still absorbing

int main() {
    double leftEnergy = 0.0, rightEnergy = 0.0;
    float peak = 0.0f;
    // Every 100 ms of a four-second absorption must carry energy: no large
    // quiet gaps. The same full-length signal is kept by refresh messages.
    for (uint32_t block = 0; block < 40; ++block) {
        double energy = 0.0;
        for (uint32_t i = 0; i < 4800; ++i) {
            const auto sample = Sample(block * 4800 + i, 1.0f);
            const auto muted = Sample(block * 4800 + i, 0.0f);
            if (!std::isfinite(sample.left) || !std::isfinite(sample.right) ||
                std::abs(sample.left) > 0.601f || muted.left != 0.0f ||
                muted.right != 0.0f) return 1;
            energy += sample.left * sample.left;
            leftEnergy += sample.left * sample.left;
            rightEnergy += sample.right * sample.right;
            peak = std::max(peak, std::abs(sample.left));
        }
        if (energy < 90.0) {
            std::fprintf(stderr, "black soul quiet gap block=%u energy=%.3f\n",
                block, energy);
            return 1;
        }
    }
    if (leftEnergy < rightEnergy * 3.0 || peak < 0.56f) return 1;
    double openingEnergy = 0.0, endingEnergy = 0.0;
    double finishEnergy = 0.0, referenceEnergy = 0.0;
    float finishPeak = 0.0f;
    for (uint32_t frame = 0; frame <= FinishFrames; ++frame) {
        const auto sample = FinishSample(frame, 1.0f);
        const auto muted = FinishSample(frame, 0.0f);
        const auto maxLevel = FinishSample(frame, 5.0f);
        const auto reference = Sample(frame, 1.0f);
        if (!std::isfinite(sample.left) || !std::isfinite(sample.right) ||
            std::abs(sample.left) > 0.751f || muted.left != 0.0f ||
            muted.right != 0.0f || maxLevel.left != sample.left ||
            maxLevel.right != sample.right) return 1;
        finishPeak = std::max(finishPeak, std::abs(sample.left));
        finishEnergy += sample.left * sample.left;
        if (frame < FinishFrames) referenceEnergy += reference.left * reference.left;
        if (frame >= 1920u && frame <= FinishHoldFrames &&
            std::abs(sample.left - reference.left * FinishBodyGain) > 0.000001f)
            return 1; // shared carrier stays fully present through the hold
        if (frame < 2880u) openingEnergy += sample.left * sample.left;
        if (frame >= FinishFrames - 2880u) endingEnergy += sample.left * sample.left;
        if ((frame == 0 || frame >= FinishFrames - 1u) &&
            (sample.left != 0.0f || sample.right != 0.0f)) return 1;
    }
    const double rmsRatio = std::sqrt(finishEnergy / referenceEnergy);
    if (finishPeak < 0.70f || openingEnergy < endingEnergy * 3.0 ||
        rmsRatio < 1.0 || rmsRatio > 1.25) return 1;
    // Exercise the renderer's loop-to-finish crossfade at different carrier
    // phases. The default-strength transition must not hard clip or go silent.
    for (uint32_t phase = 0; phase < SampleRate; phase += 3000u) {
        float loopLevel = 1.0f;
        double transitionEnergy = 0.0;
        for (uint32_t frame = 0; frame < 4800u; ++frame) {
            loopLevel += (0.0f - loopLevel) * FinishReleaseSmoothing;
            const auto loop = Sample(phase + frame, loopLevel);
            const auto finish = FinishSample(frame, 1.0f);
            const float left = loop.left + finish.left;
            const float right = loop.right + finish.right;
            if (std::abs(left) >= 1.0f || std::abs(right) >= 1.0f) return 1;
            transitionEnergy += left * left + right * right;
        }
        if (transitionEnergy < 500.0) return 1;
    }
    std::printf("Black soul state, pause, cancellation, watchdog, sustained/finish waveform tests passed pull=%.3f finish=%.3f finish/pullRMS=%.3f hold=160ms duration=240ms\n", peak, finishPeak, rmsRatio);
    return 0;
}

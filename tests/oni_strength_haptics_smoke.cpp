#include <algorithm>
#include <cmath>
#include <cstdio>

#include "oni_strength_haptics.hpp"

namespace {

struct Energy {
    double left = 0.0;
    double right = 0.0;
    float peak = 0.0f;
};

Energy Measure(uint32_t firstFrame, uint32_t lastFrame) {
    Energy result{};
    for (uint32_t frame = firstFrame; frame < lastFrame; ++frame) {
        const auto sample = oni_strength_haptics::SampleDestroy(frame);
        result.left += static_cast<double>(sample.left) * sample.left;
        result.right += static_cast<double>(sample.right) * sample.right;
        result.peak = std::max(
            result.peak,
            std::max(std::abs(sample.left), std::abs(sample.right)));
    }
    return result;
}

} // namespace

int main() {
    using namespace oni_strength_haptics;
    bool passed = true;

    for (uint32_t frame = 0; frame < DestroyDurationFrames; ++frame) {
        const auto sample = SampleDestroy(frame);
        if (!std::isfinite(sample.left) || !std::isfinite(sample.right) ||
            std::abs(sample.left) > 0.986f ||
            std::abs(sample.right) > 0.986f) {
            std::fprintf(stderr, "invalid sample at frame %u\n", frame);
            passed = false;
            break;
        }
    }

    const auto charge = Measure(0u, 14400u);
    const auto quietGap = Measure(17280u, FirstPalmFrame - 960u);
    const auto firstPalm = Measure(
        FirstPalmFrame - 480u, FirstPalmFrame + 9600u);
    const auto secondPalm = Measure(
        SecondPalmFrame - 480u, SecondPalmFrame + 12000u);
    const auto afterEnd = SampleDestroy(DestroyDurationFrames + 1u);

    const double chargeEnergy = charge.left + charge.right;
    const double quietEnergy = quietGap.left + quietGap.right;
    if (chargeEnergy <= 1.0 || quietEnergy > 0.001) {
        std::fprintf(stderr, "charge/gap failed charge=%.3f gap=%.6f\n",
            chargeEnergy, quietEnergy);
        passed = false;
    }
    if (firstPalm.left <= firstPalm.right * 3.0 || firstPalm.peak < 0.80f) {
        std::fprintf(stderr,
            "first palm failed left=%.3f right=%.3f peak=%.3f\n",
            firstPalm.left, firstPalm.right, firstPalm.peak);
        passed = false;
    }
    if (secondPalm.right <= secondPalm.left * 3.0 ||
        secondPalm.peak < 0.80f) {
        std::fprintf(stderr,
            "second palm failed left=%.3f right=%.3f peak=%.3f\n",
            secondPalm.left, secondPalm.right, secondPalm.peak);
        passed = false;
    }
    if (afterEnd.left != 0.0f || afterEnd.right != 0.0f) {
        std::fprintf(stderr, "waveform continues past duration\n");
        passed = false;
    }

    if (!passed) return 1;
    std::printf(
        "oni strength haptics smoke passed: first=%.0f/%.0f second=%.0f/%.0f peaks=%.3f/%.3f\n",
        firstPalm.left, firstPalm.right,
        secondPalm.left, secondPalm.right,
        firstPalm.peak, secondPalm.peak);
    return 0;
}

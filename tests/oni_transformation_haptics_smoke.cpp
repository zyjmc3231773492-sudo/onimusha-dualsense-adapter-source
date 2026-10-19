#include "oni_transformation_haptics.hpp"

static_assert(oni_transformation_haptics::Message == 0xE6,
    "Oni transformation must keep its dedicated IPC message ID");

#include <algorithm>
#include <cmath>
#include <cstdio>

struct Energy { double left{}, right{}; float peak{}; };
static Energy Measure(uint32_t begin, uint32_t end) {
    Energy result{};
    for (uint32_t frame = begin; frame < end; ++frame) {
        const auto sample = oni_transformation_haptics::Sample(frame);
        if (!std::isfinite(sample.left) || !std::isfinite(sample.right) ||
            std::abs(sample.left) >= 0.986f || std::abs(sample.right) >= 0.986f) {
            std::fprintf(stderr, "invalid transformation sample at %u\n", frame);
            std::exit(1);
        }
        result.left += sample.left * sample.left;
        result.right += sample.right * sample.right;
        result.peak = std::max(result.peak, std::max(std::abs(sample.left), std::abs(sample.right)));
    }
    return result;
}
int main() {
    using namespace oni_transformation_haptics;
    const auto ignition = Measure(0u, 8640u);
    const auto gather = Measure(8640u, 42240u);
    const auto burst = Measure(44160u, 56640u);
    const auto tail = Measure(56640u, DurationFrames);
    if (ignition.peak < 0.25f || gather.left < 200.0 || gather.right < 200.0 ||
        burst.peak < 0.65f || tail.left < 10.0 || tail.right < 10.0 ||
        Sample(DurationFrames).left != 0.0f || Sample(DurationFrames).right != 0.0f) {
        std::fprintf(stderr, "oni transformation stages failed ignition=%.3f gather=%.1f/%.1f burst=%.3f tail=%.1f/%.1f\n",
            ignition.peak, gather.left, gather.right, burst.peak, tail.left, tail.right);
        return 1;
    }
    std::printf("oni transformation ignition/gather/burst/tail passed peak=%.3f/%.3f\n",
        ignition.peak, burst.peak);
}

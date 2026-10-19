#include <algorithm>
#include <cmath>
#include <cstdio>

#include "repel_impact_haptics.hpp"

int main() {
    using namespace repel_impact_haptics;
    bool passed = true;
    for (bool projectile : {false, true}) {
        double previousEnergy = 0.0;
        for (float intensity : {0.0f, 0.5f, 1.0f, 2.0f}) {
            double totalEnergy = 0.0;
            double openingEnergy = 0.0;
            double lateEnergy = 0.0;
            float peak = 0.0f;
            float previous = 0.0f;
            float maximumStep = 0.0f;
            for (uint32_t frame = 0; frame <= DurationFrames(projectile); ++frame) {
                const auto sample = Sample(frame, projectile, intensity);
                passed &= std::isfinite(sample.left) && std::isfinite(sample.right);
                passed &= std::abs(sample.left) <= 0.98f && std::abs(sample.right) <= 0.98f;
                const double energy = sample.left * sample.left + sample.right * sample.right;
                totalEnergy += energy;
                if (frame < SampleRate * 0.100) openingEnergy += energy;
                if (frame >= SampleRate * 0.160) lateEnergy += energy;
                peak = std::max(peak, std::max(std::abs(sample.left), std::abs(sample.right)));
                maximumStep = std::max(maximumStep, std::abs(sample.left - previous));
                previous = sample.left;
            }
            passed &= Sample(0, projectile, intensity).left == 0.0f;
            passed &= Sample(DurationFrames(projectile) - 1, projectile, intensity).left == 0.0f;
            passed &= Sample(DurationFrames(projectile), projectile, intensity).left == 0.0f;
            if (intensity == 0.0f) passed &= totalEnergy == 0.0;
            else {
                passed &= totalEnergy > previousEnergy;
                passed &= openingEnergy / totalEnergy > 0.86;
                passed &= lateEnergy / totalEnergy < 0.025;
                passed &= maximumStep < 0.080f;
            }
            if (intensity == 2.0f) passed &= peak > 0.95f;
            previousEnergy = totalEnergy;
            std::printf("Repel impact projectile=%d intensity=%.1f peak=%.4f opening=%.4f late=%.4f step=%.4f\n",
                projectile, intensity, peak, totalEnergy > 0 ? openingEnergy / totalEnergy : 0,
                totalEnergy > 0 ? lateEnergy / totalEnergy : 0, maximumStep);
        }
        passed &= Sample(100, projectile, -1.0f).left == 0.0f;
        passed &= Sample(100, projectile, 3.0f).left == Sample(100, projectile, 2.0f).left;
    }
    std::puts(passed ? "Single-impact Repel waveform tests passed." : "Single-impact Repel waveform tests FAILED.");
    return passed ? 0 : 1;
}

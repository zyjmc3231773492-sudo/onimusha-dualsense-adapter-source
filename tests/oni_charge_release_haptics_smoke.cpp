#include "oni_charge_release_haptics.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>

int main() {
    using namespace oni_charge_release_haptics;
    double lightEnergy = 0.0, heavyEnergy = 0.0;
    double drawEnergy = 0.0, slashEnergy = 0.0, slashLateEnergy = 0.0;
    float lightPeak = 0.0f, heavyPeak = 0.0f;
    float drawPeak = 0.0f, slashPeak = 0.0f;
    for (const auto kind : {Kind::Light, Kind::Heavy,
            Kind::FinisherDraw, Kind::FinisherSlash}) {
        double& energy = kind == Kind::Light ? lightEnergy :
            (kind == Kind::Heavy ? heavyEnergy :
                (kind == Kind::FinisherDraw ? drawEnergy : slashEnergy));
        float& peak = kind == Kind::Light ? lightPeak :
            (kind == Kind::Heavy ? heavyPeak :
                (kind == Kind::FinisherDraw ? drawPeak : slashPeak));
        for (uint32_t frame = 0; frame < Duration(kind); ++frame) {
            const auto sample = Sample(frame, kind);
            if (!std::isfinite(sample.left) || !std::isfinite(sample.right) ||
                std::abs(sample.left) >= 1.0f || std::abs(sample.right) >= 1.0f) return 1;
            peak = std::max({peak, std::abs(sample.left), std::abs(sample.right)});
            energy += sample.left * sample.left + sample.right * sample.right;
            if (kind == Kind::FinisherSlash && frame >= 24000u)
                slashLateEnergy += sample.left * sample.left + sample.right * sample.right;
        }
    }
    std::printf("Oni charge/release peak=%.3f/%.3f draw=%.3f slash=%.3f energy=%.0f/%.0f/%.0f/%.0f late=%.0f\n",
        lightPeak, heavyPeak, drawPeak, slashPeak,
        lightEnergy, heavyEnergy, drawEnergy, slashEnergy, slashLateEnergy);
    if (lightPeak < 0.65f || heavyPeak < 0.75f ||
        drawPeak < 0.75f || slashPeak < 0.90f ||
        lightEnergy < 2500.0 || heavyEnergy < lightEnergy * 1.25 ||
        Duration(Kind::FinisherSlash) < 120000u ||
        slashEnergy < heavyEnergy * 8.0 || slashLateEnergy < 25000.0) return 2;
    std::printf("Oni finisher stop/draw/slash staging passed\n");
}

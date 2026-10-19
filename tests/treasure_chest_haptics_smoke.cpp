#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

#include "treasure_chest_haptics.hpp"

namespace {
constexpr double Pi = 3.14159265358979323846;
constexpr double TwoPi = Pi * 2.0;
constexpr double SampleRate = 48000.0;

struct Measurement {
    float peakLeft = 0.0f;
    float peakRight = 0.0f;
    double rms = 0.0;
};

Measurement Measure(treasure_chest_haptics::Stage stage) {
    const uint32_t frames = static_cast<uint32_t>(
        treasure_chest_haptics::DurationSeconds(stage) * SampleRate);
    double phaseA = 0.0;
    double phaseB = 0.0;
    double modPhase = 0.0;
    double energy = 0.0;
    Measurement result{};
    for (uint32_t frameIndex = 0; frameIndex < frames; ++frameIndex) {
        const float progress = static_cast<float>(frameIndex) /
            static_cast<float>(std::max<uint32_t>(1, frames - 1));
        const float attackX = std::clamp(progress / 0.022f, 0.0f, 1.0f);
        const float attack = attackX * attackX * (3.0f - 2.0f * attackX);
        const float modWave = static_cast<float>(std::sin(modPhase));
        const auto parameters = treasure_chest_haptics::Evaluate(
            stage, progress, attack, modWave);
        const float wave = static_cast<float>(std::sin(phaseA)) *
                parameters.waveMixA +
            static_cast<float>(std::sin(phaseB)) * parameters.waveMixB;
        const float left = std::clamp(
            wave * parameters.amplitude * parameters.leftGain,
            -1.0f, 1.0f);
        const float right = std::clamp(
            wave * parameters.amplitude * parameters.rightGain,
            -1.0f, 1.0f);
        result.peakLeft = std::max(result.peakLeft, std::fabs(left));
        result.peakRight = std::max(result.peakRight, std::fabs(right));
        energy += static_cast<double>(left) * left +
            static_cast<double>(right) * right;
        phaseA = std::fmod(
            phaseA + TwoPi * parameters.frequencyA / SampleRate, TwoPi);
        phaseB = std::fmod(
            phaseB + TwoPi * parameters.frequencyB / SampleRate, TwoPi);
        modPhase = std::fmod(
            modPhase + TwoPi * parameters.modulationFrequency / SampleRate,
            TwoPi);
    }
    result.rms = std::sqrt(energy / static_cast<double>(frames * 2u));
    return result;
}

bool Near(float actual, float expected, float tolerance = 0.001f) {
    return std::fabs(actual - expected) <= tolerance;
}
} // namespace

int main() {
    using treasure_chest_haptics::Stage;
    const auto open = Measure(Stage::Open);
    const auto push = Measure(Stage::Push);
    const auto land = Measure(Stage::LidLand);

    bool ok = true;
    ok = ok && Near(treasure_chest_haptics::DurationSeconds(Stage::Open), 0.62f);
    ok = ok && treasure_chest_haptics::DurationSeconds(Stage::Push) >= 3.0f;
    ok = ok && Near(treasure_chest_haptics::DurationSeconds(Stage::LidLand), 0.48f);
    ok = ok && treasure_chest_haptics::FallbackLidImpactDelayMs == 1700;
    ok = ok && treasure_chest_haptics::LidImpactDedupeMs >= 1000;
    ok = ok && treasure_chest_haptics::OpenCueFallbackDelayMs == 900;
    ok = ok && treasure_chest_haptics::PushCueDelayMs == 600;
    ok = ok && open.rms >= 0.25 && open.peakRight >= 0.80f &&
        open.peakLeft >= 0.80f;
    ok = ok && push.rms >= 0.22 && push.peakLeft >= 0.38f;
    ok = ok && land.rms >= 0.24 && land.peakLeft >= 0.90f;

    const auto pushEarly = treasure_chest_haptics::Evaluate(
        Stage::Push, 0.20f, 1.0f, 0.0f);
    const auto pushLate = treasure_chest_haptics::Evaluate(
        Stage::Push, 0.75f, 1.0f, 0.0f);
    ok = ok && pushEarly.amplitude >= 0.25f && pushLate.amplitude >= 0.40f;

    const auto openCarrier = treasure_chest_haptics::Evaluate(
        Stage::Open, 0.10f, 1.0f, 0.0f);
    const auto landCarrier = treasure_chest_haptics::Evaluate(
        Stage::LidLand, 0.10f, 1.0f, 0.0f);
    ok = ok && openCarrier.frequencyA >= 60.0 &&
        openCarrier.frequencyA < 100.0;
    ok = ok && landCarrier.frequencyA >= 75.0;
    ok = ok && pushEarly.frequencyA < 80.0;
    ok = ok && pushEarly.noiseMix >= 0.30f;
    const auto liftStart = treasure_chest_haptics::Evaluate(
        Stage::Open, 0.10f, 1.0f, 0.0f);
    const auto halfOpen = treasure_chest_haptics::Evaluate(
        Stage::Open, 0.88f, 1.0f, 0.0f);
    ok = ok && liftStart.amplitude >= 0.55f;
    ok = ok && halfOpen.amplitude >= 0.90f;

    std::printf(
        "open peak=%.3f/%.3f rms=%.3f; push peak=%.3f/%.3f rms=%.3f; land peak=%.3f/%.3f rms=%.3f\n",
        open.peakLeft, open.peakRight, open.rms,
        push.peakLeft, push.peakRight, push.rms,
        land.peakLeft, land.peakRight, land.rms);
    return ok ? 0 : 1;
}

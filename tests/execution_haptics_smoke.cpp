#include "execution_haptics.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
using namespace execution_haptics;

static_assert(Classify("app.PlayerOneHandedAction.cFatalBlowSingle") == Kind::Thrust);
static_assert(Classify("app.PlayerOneHandedAction.cFatalBlowHoraizontalSlash") == Kind::Slash);
static_assert(Classify("app.PlayerOneHandedAction.cFatalBlowDiagonalSlash") == Kind::Slash);
static_assert(Classify("app.PlayerOneHandedAction.cFatalBlowArmSlash") == Kind::ArmCut);
static_assert(Classify("app.PlayerOneHandedAction.cFatalBlowForDownAttack") == Kind::Downed);
static_assert(Classify("app.PlayerOneHandedAction.cFatalBlowConst") == Kind::Const);
static_assert(Classify("app.PlayerOneHandedAction.cFatalBlowAdjust") == Kind::None);
static_assert(Classify("app.PlayerOneHandedAction.cQuickMoveBeforeFatalBlowForDownAdjust") == Kind::None);
static_assert(Classify("app.PlayerOneHandedAction.cBreakIssenQuick") == Kind::BreakIssen);
static_assert(Classify("app.PlayerOneHandedAction.cBreakIssenOniChange") == Kind::BreakIssen);
static_assert(Classify("app.PlayerOneHandedAction.cBlockIssenConst") == Kind::BlockIssen);
static_assert(Classify("app.PlayerOneHandedAction.cBlockIssenMultiple") == Kind::BlockIssen);
static_assert(Classify("app.PlayerTwoHandedAction.cBreakIssenComboStart") == Kind::BreakIssen);
static_assert(Classify("app.PlayerTwoHandedAction.cBreakIssenComboConst") == Kind::BlockIssen);
static_assert(Classify("app.PlayerTwoHandedAction.cBreakIssenComboGenericConst") == Kind::BlockIssen);
static_assert(Classify("app.PlayerOneHandedAction.cBreakIssenLongStart") == Kind::None);
static_assert(Classify("app.PlayerOneHandedAction.cBlockIssenAdjust") == Kind::None);
static_assert(Classify("app.PlayerOneHandedAction.cGimmickBreakIssen") == Kind::None);
static_assert(Classify("app.PlayerBasicAction.cBreakIssenToWeaponOff") == Kind::None);
static_assert(Classify("app.EnemyAction.cFatalBlowSingle") == Kind::None);
static_assert(MotionVariant({}) == 0u);
static_assert(MotionVariant({0x8A1B2C3D4E5F6071ULL, 14u, 1u, 0u, 2u}) < 4u);

static void Require(bool okay, const char* message) {
    if (!okay) { std::fprintf(stderr, "Execution haptics failed: %s\n", message); std::exit(1); }
}
static double Energy(Kind kind, double begin, double end, float* peak = nullptr,
    double* leftEnergy = nullptr, double* rightEnergy = nullptr,
    uint8_t motionVariant = 0u) {
    const uint32_t first = static_cast<uint32_t>(begin * SampleRate);
    const uint32_t last = std::min(Duration(kind), static_cast<uint32_t>(end * SampleRate));
    double energy = 0.0, left = 0.0, right = 0.0;
    for (uint32_t frame = first; frame < last; ++frame) {
        const auto sample = Sample(frame, kind, 1.0f, motionVariant);
        Require(std::isfinite(sample.left) && std::isfinite(sample.right), "finite output");
        Require(std::abs(sample.left) < 1.0f && std::abs(sample.right) < 1.0f, "headroom");
        if (peak) *peak = std::max(*peak, std::max(std::abs(sample.left), std::abs(sample.right)));
        left += sample.left * sample.left;
        right += sample.right * sample.right;
        energy += sample.left * sample.left + sample.right * sample.right;
    }
    if (leftEnergy) *leftEnergy = left;
    if (rightEnergy) *rightEnergy = right;
    return energy;
}
static double Difference(Kind kind, uint8_t firstVariant, uint8_t secondVariant) {
    double difference = 0.0;
    for (uint32_t frame = 0; frame < Duration(kind); ++frame) {
        const auto first = Sample(frame, kind, 1.0f, firstVariant);
        const auto second = Sample(frame, kind, 1.0f, secondVariant);
        difference += std::abs(first.left - second.left) +
            std::abs(first.right - second.right);
    }
    return difference;
}
static double RenderEnergy(Kind kind, uint8_t motionVariant, float intensity) {
    double energy = 0.0;
    for (uint32_t frame = 0; frame < Duration(kind); ++frame) {
        const auto sample = Sample(frame, kind, intensity, motionVariant);
        energy += sample.left * sample.left + sample.right * sample.right;
    }
    return energy;
}
int main() {
    std::array<double, 7> totals{};
    std::array<float, 7> peaks{};
    size_t index = 0;
    for (Kind kind : {Kind::Thrust, Kind::Slash, Kind::ArmCut, Kind::Downed,
             Kind::Const, Kind::BreakIssen, Kind::BlockIssen}) {
        Require(ProfileValue(kind) == 7u + index, "stable pipe profile value");
        Require(KindFromProfile(ProfileValue(kind)) == kind, "profile round trip");
        Require(SuppressionMs(kind) == Duration(kind) / 48u + 120u, "bounded isolation window");
        const bool immediateContact = kind == Kind::Const || kind == Kind::BlockIssen;
        if (!immediateContact && kind != Kind::BreakIssen)
            Require(Energy(kind, 0.0, 0.14) < 0.01, "no early button-like impact");
        const double first = immediateContact
            ? Energy(kind, 0.0, 0.25)
            : Energy(kind, 0.36, 0.72);
        const double finish = kind == Kind::BreakIssen
            ? Energy(kind, 0.70, 1.02)
            : (immediateContact
                ? Energy(kind, 0.80, 1.22)
                : Energy(kind, 1.02, 1.52));
        double left = 0.0, right = 0.0;
        totals[index] = Energy(kind, 0.0, 2.0, &peaks[index], &left, &right);
        Require(first > 420.0 && finish > 520.0, "two heavy animation-synchronised contact stages");
        Require(peaks[index] > 0.78f, "execution contact is clearly above the old weak profile");
        Require(Difference(kind, 0u, 3u) > 5000.0,
            "motion-id variants remain physically distinct");
        double weakestVariant = 1.0e30;
        double strongestVariant = 0.0;
        for (uint8_t variant = 0; variant < 4u; ++variant) {
            const double energy = RenderEnergy(kind, variant, 1.65f);
            weakestVariant = std::min(weakestVariant, energy);
            strongestVariant = std::max(strongestVariant, energy);
        }
        Require(strongestVariant / weakestVariant < 1.06,
            "motion-id variants have an equal strength baseline");
        if (kind == Kind::Slash) {
            std::printf("Execution slash lateral ratio=%.3f\n", right / left);
            Require(right / left > 0.72 && right / left < 1.18, "slash travels laterally");
        }
        if (kind == Kind::ArmCut) Require(left > right * 1.35, "arm cut has cutting-side bias");
        if (kind == Kind::Downed) Require(std::abs(left / right - 1.0) < 0.12, "downed impact is centred");
        if (kind == Kind::Const) Require(std::abs(left / right - 1.0) < 0.12, "const fallback is centred");
        if (kind == Kind::BreakIssen) Require(right / left > 0.72 && right / left < 1.14, "break Issen travels laterally");
        if (kind == Kind::BlockIssen) Require(std::abs(left / right - 1.0) < 0.12, "block Issen is centred");
        const auto ended = Sample(Duration(kind), kind);
        Require(ended.left == 0.0f && ended.right == 0.0f, "finite duration");
        ++index;
    }
    std::printf("Execution thrust/slash/arm/down/const/break/block classification, timing, direction, strength, isolation and headroom passed peak=%.3f/%.3f/%.3f/%.3f/%.3f/%.3f/%.3f energy=%.0f/%.0f/%.0f/%.0f/%.0f/%.0f/%.0f\n",
        peaks[0], peaks[1], peaks[2], peaks[3], peaks[4], peaks[5], peaks[6],
        totals[0], totals[1], totals[2], totals[3], totals[4], totals[5], totals[6]);
}

#include <cstdio>

#include "melee_repel_detection.hpp"
#include "combat_defense_feedback.hpp"

namespace {

bool TestDeflectEpisodes() {
    using combat_defense_feedback::DeflectEpisodeGate;
    bool passed = true;
    auto check = [&](bool result, const char* label) {
        if (!result) {
            std::fprintf(stderr, "Deflect episode gate failed: %s\n", label);
            passed = false;
        }
    };

    // Replay the actual 2026-09-18 14:49:48.264..14:49:49.282 callback burst.
    // The previous 450-ms emission cooldown admitted three separate sounds.
    DeflectEpisodeGate recorded;
    constexpr uint64_t RecordedOffsets[] = {
        0, 0, 74, 171, 269, 350, 451, 556, 651, 733, 837, 936, 1018};
    unsigned emitted = 0;
    for (const uint64_t offset : RecordedOffsets) {
        if (recorded.Observe(10000 + offset)) ++emitted;
    }
    check(emitted == 1, "recorded slow-motion burst emits exactly once");
    check(!recorded.Observe(11318), "quiet-window boundary remains same episode");
    check(recorded.Observe(11619), "independent next burst emits again");

    DeflectEpisodeGate prolonged;
    emitted = 0;
    for (uint64_t tick = 20000; tick <= 80000; tick += 100) {
        if (prolonged.Observe(tick)) ++emitted;
    }
    check(emitted == 1, "60 seconds of sustained callbacks never loop");

    DeflectEpisodeGate rapid;
    check(rapid.Observe(1000), "first independent impact");
    check(rapid.ConfirmSuccess(1190, 1180, 1), "accepted second defensive result");
    check(rapid.Observe(1200), "new result within quiet window is not lost");
    check(!rapid.ConfirmSuccess(1201, 1180, 1), "nested/base result duplicate");
    check(!rapid.Observe(1260), "second episode continuation is suppressed");
    check(rapid.ConfirmSuccess(1340, 1330, 2), "accepted third defensive result");
    check(rapid.Observe(1350), "rapid third independent impact");
    check(rapid.ConfirmSuccess(1390, 1380, 3), "new result only 50 ms later");
    check(rapid.Observe(1395), "no arbitrary cooldown blocks distinct impacts");
    check(!rapid.ConfirmSuccess(1400, 1380, 2), "older result cannot re-arm gate");
    check(!rapid.Observe(1450), "older-result duplicate does not cause replay");

    DeflectEpisodeGate insideOriginal;
    check(insideOriginal.Observe(1000), "sound resolves inside original call");
    check(insideOriginal.ConfirmSuccess(1020, 990, 1), "original result confirmation");
    check(!insideOriginal.Observe(1030), "post-hook must not re-arm same impact");

    DeflectEpisodeGate beforeSound;
    check(beforeSound.ConfirmSuccess(1000, 990, 1), "result resolves before sound");
    check(beforeSound.Observe(1060), "later original sound is still admitted");
    check(!beforeSound.Observe(1360), "ongoing burst remains suppressed");
    check(beforeSound.Observe(1661), "complete quiet interval admits new burst");

    DeflectEpisodeGate boundaries;
    check(boundaries.Observe(0), "zero timestamp first observation");
    check(!boundaries.Observe(300), "exact quiet-window boundary");
    check(boundaries.Observe(601), "strictly longer quiet window");
    check(!boundaries.Observe(600), "out-of-order observation ignored");
    check(!boundaries.ConfirmSuccess(590, 580, 1), "out-of-order result ignored");
    check(!boundaries.ConfirmSuccess(620, 630, 1), "invalid call interval ignored");
    check(!boundaries.ConfirmSuccess(620, 610, 0), "missing result identity ignored");
    check(!boundaries.Observe(901), "ignored events do not rewind observation");
    check(boundaries.Observe(1202), "gate recovers after complete quiet interval");
    return passed;
}
} // namespace

int main() {
    using melee_repel_detection::Evidence;
    using melee_repel_detection::ShouldEmit;
    constexpr Evidence confirmed{true, true, true, true, false};
    static_assert(ShouldEmit(confirmed));
    static_assert(!ShouldEmit({false, true, true, true, false}));
    static_assert(!ShouldEmit({true, false, true, true, false}));
    static_assert(!ShouldEmit({true, true, false, true, false}));
    static_assert(!ShouldEmit({true, true, true, false, false}));
    static_assert(!ShouldEmit({true, true, true, true, true}));
    static_assert(!ShouldEmit({}));
    using namespace combat_defense_feedback;
    static_assert(IsCinematicParryArgument(true, 2, true));
    static_assert(!IsCinematicParryArgument(true, 2, false));
    static_assert(!IsCinematicParryArgument(false, 2, true));
    static_assert(!IsCinematicParryArgument(true, 1, true));
    static_assert(!IsCinematicParryArgument(true, 0xFFFFFFFFu, true));
    static_assert(ClassifyDeflectSpeakerVariant(1992708532u, 3387009413u, true) ==
        DeflectSpeakerVariant::Slow);
    static_assert(ClassifyDeflectSpeakerVariant(1992708532u, 3387009413u) ==
        DeflectSpeakerVariant::ContactShort);
    static_assert(ClassifyDeflectSpeakerVariant(2383308271u, 3543035654u) ==
        DeflectSpeakerVariant::Short);
    static_assert(ClassifyDeflectSpeakerVariant(1992708532u, 3543035654u) ==
        DeflectSpeakerVariant::Short); // mismatched native pair is not assumed slow
    static_assert(ClassifyDeflectSpeakerVariant(0xFFFFFFFFu, 0xFFFFFFFFu) ==
        DeflectSpeakerVariant::Short);
    static_assert(DecodeDeflectSpeakerVariant(0) == DeflectSpeakerVariant::Short);
    static_assert(DecodeDeflectSpeakerVariant(1) == DeflectSpeakerVariant::Slow);
    static_assert(DecodeDeflectSpeakerVariant(2) == DeflectSpeakerVariant::ContactShort);
    static_assert(DecodeDeflectSpeakerVariant(255) == DeflectSpeakerVariant::Short);
    static_assert(UsesContactRecording(DeflectSpeakerVariant::ContactShort));
    static_assert(UsesContactRecording(DeflectSpeakerVariant::Slow));
    static_assert(!UsesContactRecording(DeflectSpeakerVariant::Short));
    static_assert(CanPromoteToCinematic(
        DeflectSpeakerVariant::ContactShort, DeflectSpeakerVariant::Slow));
    static_assert(!CanPromoteToCinematic(
        DeflectSpeakerVariant::Short, DeflectSpeakerVariant::Slow));
    static_assert(!CanPromoteToCinematic(
        DeflectSpeakerVariant::Slow, DeflectSpeakerVariant::Slow));
    static_assert(!CanPromoteToCinematic(
        DeflectSpeakerVariant::Slow, DeflectSpeakerVariant::ContactShort));
    static_assert(ClassifyNativeGrapple(NativeParry, true, false, 2383308271u) == Kind::Deflect);
    static_assert(ClassifyNativeGrapple(NativeParry, true, false, 1992708532u) == Kind::Deflect);
    static_assert(ClassifyNativeGrapple(NativeBlock, true, false, 3893406318u) == Kind::MeleeRepel);
    static_assert(ClassifyNativeGrapple(NativeParry, false, false, 1) == Kind::None);
    static_assert(ClassifyNativeGrapple(NativeParry, true, true, 1) == Kind::None);
    static_assert(ClassifyNativeGrapple(NativeParry, true, false, 0xFFFFFFFFu) == Kind::None);
    static_assert(ClassifyNativeGrapple(986178176u, true, false, 1) == Kind::None);
    static_assert(ClassifyNativeShell(true, false, 1, true) == Kind::ProjectileRepel);
    static_assert(ClassifyNativeShell(true, false, 1, false) == Kind::None);
    static_assert(ClassifyNativeShell(false, false, 1, true) == Kind::None);
    static_assert(ClassifyNativeShell(true, true, 1, true) == Kind::None);
    static_assert(ClassifyNativeShell(true, false, 0, true) == Kind::None);
    static_assert(RequiresNativeParrySound("app.PlayerOneHandedAction.cParryBlow"));
    static_assert(RequiresNativeParrySound("app.PlayerOneHandedAction.cParryDefend"));
    static_assert(RequiresNativeParrySound("app.PlayerOneHandedAction.cParryShell"));
    static_assert(!RequiresNativeParrySound("app.EnemyAction.cParryBlow"));
    static_assert(!RequiresNativeParrySound("app.PlayerOneHandedAction.cPreParry"));
    static_assert(PairedRepelSpeaker(HapticProfile(Kind::Deflect)) == 0);
    static_assert(PairedRepelSpeaker(HapticProfile(Kind::MeleeRepel)) == 12);
    static_assert(PairedRepelSpeaker(HapticProfile(Kind::ProjectileRepel)) == 13);
    static_assert(PairedRepelSpeaker(6) == 0);  // activation is not an impact sound
    static_assert(RepelImpactWindowActive(100, 100));
    static_assert(!RepelImpactWindowActive(101, 100));
    static_assert(!RepelImpactWindowActive(0, 0));
    static_assert(ShouldSuppressStandardRumble(50, 100, 1, 0));
    static_assert(ShouldSuppressStandardRumble(50, 100, 0, 1));
    static_assert(!ShouldSuppressStandardRumble(50, 100, 0, 0));
    static_assert(!ShouldSuppressStandardRumble(101, 100, 1, 1));
    if (!TestDeflectEpisodes()) return 1;
    std::puts("Deflect / melee Repel / projectile Repel routing smoke tests passed.");
    std::puts("Melee Repel success/input/pause gating smoke tests passed.");
    std::puts("Deflect slow-motion / rapid-next-impact / original-call-order episode tests passed.");
    return 0;
}

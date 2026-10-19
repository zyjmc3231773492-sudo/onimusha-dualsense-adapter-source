#include "normal_attack_haptics.hpp"
#include <cstdio>
#include <cstdlib>
using namespace normal_attack_haptics;
static_assert(ClassifyAction("app.PlayerOneHandedAction.cAttackWeak1") == Kind::Light);
static_assert(ClassifyAction("app.PlayerOneHandedAction.cAttackWeak3") == Kind::Light);
static_assert(ClassifyAction("app.PlayerOneHandedAction.cAttackWeakFromStrong2") == Kind::Light);
static_assert(ClassifyAction("app.PlayerOneHandedAction.cAttackStrong2") == Kind::Heavy);
static_assert(ClassifyAction("app.PlayerOneHandedAction.cAttackStrongFromWeak1") == Kind::Heavy);
static_assert(ClassifyAction("app.PlayerOneHandedAction.cAttackWeakCharge") == Kind::None);
static_assert(ClassifyAction("app.PlayerOneHandedAction.cAttackStrongCharge") == Kind::None);
static_assert(ClassifyChargeAction("app.PlayerOneHandedAction.cAttackWeakCharge") == Kind::ChargeLight);
static_assert(ClassifyChargeAction("app.PlayerOneHandedAction.cAttackStrongCharge") == Kind::ChargeHeavy);
static_assert(ClassifyChargeAction("app.PlayerOneHandedAction.cAttackStrong2") == Kind::None);
static_assert(BaseKind(Kind::ChargeLight) == Kind::Light);
static_assert(BaseKind(Kind::ChargeHeavy) == Kind::Heavy);
static_assert(ClassifyAction("app.PlayerOneHandedAction.cAttackAfterParry") == Kind::None);
static_assert(ClassifyAction("app.PlayerOneHandedAction.cAttackAfterJustDodge") == Kind::None);
static_assert(ClassifyAction("app.PlayerOneHandedAction.cAttackRepelled") == Kind::None);
static_assert(ClassifyAction("app.PlayerOneHandedAction.cSkill4_GroundAttack") == Kind::None);
static_assert(ClassifyAction("app.PlayerOneHandedAction.cCounterIssenMultipleAttack") == Kind::None);
static_assert(ClassifyAction("app.PlayerOneHandedAction.cFatalBlowForDownAttack") == Kind::None);
static_assert(ClassifyAction("app.Em100Action.cAttackWeak1") == Kind::None);
static_assert(!IsFightingSpiritState(false, 0));
static_assert(!IsFightingSpiritState(false, 1));
static_assert(!IsFightingSpiritState(true, 0));
static_assert(IsFightingSpiritState(true, 1));
static_assert(!IsFightingSpiritState(true, 2));
static_assert(!IsFightingSpiritState(true, 3));
static_assert(!IsFightingSpiritState(true, -999));
static_assert(IsEnhancedAttackState(true, 1, false, true));
static_assert(IsEnhancedAttackState(false, 0, true, true));
static_assert(IsEnhancedAttackState(true, 2, true, true));
static_assert(!IsEnhancedAttackState(false, 0, true, false));
static_assert(!IsEnhancedAttackState(false, 0, false, true));
static_assert(!IsEnhancedAttackState(true, 2, false, true));
static_assert(IsOniAttackState(true, true));
static_assert(!IsOniAttackState(true, false));
static_assert(IsOrdinaryAttackState(false, 0, false, false));
static_assert(IsOrdinaryAttackState(false, 0, true, false)); // stale Oni type after transformation ends
static_assert(IsOrdinaryAttackState(false, 0, false, true)); // captured post-battle effect residue
static_assert(!IsOrdinaryAttackState(true, 0, false, false));
static_assert(!IsOrdinaryAttackState(false, 1, false, false));
static_assert(!IsOrdinaryAttackState(false, 0, true, true));
static_assert(SwingLeadFrames(Kind::Light) == 10560u);
static_assert(SwingLeadFrames(Kind::Heavy) == 15360u);
static_assert(SwingLeadFrames("app.PlayerOneHandedAction.cAttackWeak3", Kind::Light) == 16320u);
static_assert(SwingLeadFrames("app.PlayerOneHandedAction.cAttackStrong1", Kind::Heavy) == 23040u);
static_assert(SwingLeadFrames("app.PlayerOneHandedAction.cAttackStrongFromWeak2", Kind::Heavy) == 7200u);
static_assert(SecondarySwingLeadFrames("app.PlayerOneHandedAction.cAttackWeakFromStrong1") == 28800u);
static_assert(SecondarySwingLeadFrames("app.PlayerOneHandedAction.cAttackStrongFromWeak1") == 26880u);
static_assert(SecondarySwingLeadFrames("app.PlayerOneHandedAction.cAttackWeak3") == 0u);

static void Require(bool okay, const char* message) {
    if (!okay) { std::fprintf(stderr, "Normal attack failed: %s\n", message); std::exit(1); }
}
static double Advance(Renderer& renderer, uint32_t frames, bool silent = false, float* peak = nullptr, Kind refresh = Kind::None) {
    double energy = 0.0;
    for (uint32_t frame = 0; frame < frames; ++frame) {
        if (refresh != Kind::None && frame % 4800u == 0) renderer.Refresh(refresh);
        const auto sample = renderer.Render();
        Require(std::isfinite(sample.left) && std::isfinite(sample.right), "finite samples");
        Require(std::abs(sample.left) < 1.0f && std::abs(sample.right) < 1.0f, "default headroom");
        if (silent) Require(sample.left == 0.0f && sample.right == 0.0f, "no unexpected cue");
        if (peak != nullptr) *peak = std::max(*peak, std::abs(sample.left));
        energy += sample.left * sample.left + sample.right * sample.right;
    }
    return energy;
}
static double BandPower(Kind kind, uint32_t low, uint32_t high) {
    // Same production waveform, Goertzel bins: check that empty swings no
    // longer carry the old low-frequency weight. This is not a feel test.
    double power = 0.0;
    for (uint32_t frequency = low; frequency <= high; frequency += 5u) {
        const double coefficient = 2.0 * std::cos(6.28318530717958647692 * frequency / SampleRate);
        double previous = 0.0, older = 0.0;
        for (uint32_t frame = 0; frame < Duration(kind); ++frame) {
            const double current = Sample(frame, kind).left + coefficient * previous - older;
            older = previous; previous = current;
        }
        power += std::max(0.0, previous * previous + older * older - coefficient * previous * older);
    }
    return power;
}
int main() {
    {
        ChargeTracker charge{};
        Require(charge.Observe(1000u, 10u, 100u, Kind::ChargeLight, false) == Operation::NoEvent,
            "inactive charge state stays silent");
        Require(charge.Observe(1010u, 10u, 100u, Kind::ChargeLight, true) == Operation::ChargeStart,
            "Oni light charge starts");
        Require(charge.Observe(1060u, 10u, 100u, Kind::ChargeLight, true) == Operation::NoEvent,
            "charge update does not restart");
        Require(charge.Observe(1120u, 10u, 100u, Kind::ChargeLight, true) == Operation::ChargeRefresh,
            "charge evidence refreshes");
        Require(charge.Observe(1200u, 10u, 100u, Kind::ChargeLight, true, true) == Operation::ChargeRelease,
            "actual attack phase releases charged swing");
        Require(charge.Release(1210u, 10u) == Kind::None,
            "action exit does not replay an already released swing");
        Require(charge.AcceptHit(1280u, 100u, 200u, 7u, true) == Kind::ChargeLight,
            "post-release owned damage retains charged hit");
        Require(charge.AcceptHit(1290u, 100u, 200u, 7u, true) == Kind::None,
            "charged hit deduplicates callbacks");
        Require(charge.AcceptHit(1950u, 100u, 201u, 8u, true) == Kind::None,
            "charged hit ticket expires");
    }
    {
        ChargeTracker ordinary{}, oni{};
        const bool normalForm = IsOrdinaryAttackState(false, 0, false, true);
        Require(normalForm,
            "post-battle non-Oni effect residue restores the ordinary-form gate");
        Require(ordinary.Observe(2000u, 20u, 100u, Kind::ChargeHeavy, normalForm) == Operation::ChargeStart,
            "ordinary heavy charge begins after the captured post-battle transition");
        Require(oni.Observe(2000u, 20u, 100u, Kind::ChargeHeavy,
            IsOniAttackState(false, false)) == Operation::NoEvent,
            "ordinary charge does not start the Oni tracker");
        Require(ordinary.Observe(2110u, 20u, 100u, Kind::ChargeHeavy, normalForm) == Operation::ChargeRefresh,
            "ordinary charge keeps held feedback alive");
        Require(ordinary.Observe(2200u, 20u, 100u, Kind::ChargeHeavy, normalForm, true) == Operation::ChargeRelease,
            "ordinary charged swing releases on attack phase");
        Require(ordinary.AcceptHit(2250u, 100u, 200u, 42u, true) == Kind::ChargeHeavy,
            "ordinary charged hit uses its own ownership ticket");
        Require(oni.AcceptHit(2250u, 100u, 200u, 42u, true) == Kind::None,
            "ordinary charged hit cannot leak into Oni feedback");
    }
    {
        Tracker spiritGate{};
        Require(spiritGate.Observe(1000u, 1u, 100u, Kind::Light,
            IsFightingSpiritState(false, 0)) == Operation::NoEvent,
            "ordinary state cannot start added swing haptic");
        Require(spiritGate.Observe(1010u, 1u, 100u, Kind::Light,
            IsFightingSpiritState(true, 1)) == Operation::Start,
            "captured active type1 state allows added swing even when PowerUp effect flag is zero");
        Require(spiritGate.Observe(1020u, 1u, 100u, Kind::Light,
            IsFightingSpiritState(true, 2)) == Operation::End,
            "changing to another active boost type ends the current swing ticket");
        Require(spiritGate.AcceptHit(1030u, 100u, 200u, 1u, true) == Kind::None,
            "hit cannot reuse ticket after spirit state ended");
        Require(spiritGate.Observe(1040u, 1u, 100u, Kind::Heavy,
            IsFightingSpiritState(true, 1)) == Operation::Start,
            "heavy attack allowed after fighting spirit resumes");
        Require(spiritGate.Observe(1050u, 1u, 100u, Kind::Heavy,
            IsFightingSpiritState(false, 1)) == Operation::End,
            "lingering type1 value alone is not an active state");
        Require(spiritGate.Observe(1060u, 2u, 100u, Kind::Heavy,
            IsEnhancedAttackState(false, 0, true, true)) == Operation::Start,
            "active Oni effect starts the same ordinary attack haptic path");
        Require(spiritGate.AcceptHit(1070u, 100u, 200u, 2u, true) == Kind::Heavy,
            "Oni ordinary attack ticket admits confirmed owned damage");
        Require(spiritGate.Observe(1080u, 2u, 100u, Kind::Heavy,
            IsEnhancedAttackState(false, 0, true, false)) == Operation::End,
            "stale Oni type without active effect ends the ticket");
    }
    Tracker tracker{};
    Require(tracker.Observe(1000u, 10u, 100u, Kind::Light, false) == Operation::NoEvent, "approach is not a swing");
    Require(tracker.Observe(1010u, 10u, 100u, Kind::Light, true) == Operation::Start, "real attack phase starts once");
    Require(tracker.Observe(1050u, 10u, 100u, Kind::Light, true) == Operation::NoEvent, "update does not restart swing");
    Require(tracker.Observe(1110u, 10u, 100u, Kind::Light, true) == Operation::Refresh, "phase evidence refresh");
    Require(tracker.AcceptHit(1120u, 99u, 200u, 1u, true) == Kind::None, "different attacker excluded");
    Require(tracker.AcceptHit(1120u, 100u, 100u, 1u, true) == Kind::None, "self hit excluded");
    Require(tracker.AcceptHit(1120u, 100u, 200u, 1u, false) == Kind::None, "zero damage or guard-only excluded");
    Require(tracker.AcceptHit(1120u, 100u, 200u, 1u, true) == Kind::Light, "confirmed owned light hit");
    Require(tracker.AcceptHit(1125u, 100u, 200u, 1u, true) == Kind::None, "duplicate hit callback excluded");
    Require(tracker.AcceptHit(1130u, 100u, 201u, 1u, true) == Kind::None, "same sweep crowd does not produce a pulse train");
    Require(tracker.AcceptHit(1320u, 100u, 200u, 1u, true) == Kind::None, "late duplicate excluded");
    Require(tracker.AcceptHit(1320u, 100u, 200u, 2u, true) == Kind::Light, "distinct later contact allowed");
    Require(tracker.AcceptHit(1410u, 100u, 200u, 3u, true) == Kind::None, "expired attack cannot claim damage");
    Require(tracker.Expire(1410u), "lost action evidence expires");
    Require(tracker.Observe(1500u, 11u, 100u, Kind::Heavy, true) == Operation::Start, "heavy starts after stale light");
    Require(tracker.Finish(10u) == Kind::None && tracker.Fresh(1510u), "old exit cannot cancel new combo");
    Require(tracker.AcceptHit(1520u, 100u, 200u, 1u, true) == Kind::Heavy, "heavy combo resets contact deduplication");
    Require(tracker.Finish(11u) == Kind::Heavy && !tracker.Fresh(1520u), "actual action exit clears hit ticket");
    Require(tracker.AcceptHit(1530u, 100u, 200u, 4u, true) == Kind::None, "no hit after cancellation");
    Require(tracker.Observe(1600u, 12u, 100u, Kind::None, true) == Operation::NoEvent, "skills do not create ordinary attack ticket");
    tracker.Observe(1700u, 13u, 100u, Kind::Light, true);
    tracker.Clear();
    Require(tracker.AcceptHit(1720u, 100u, 200u, 5u, true) == Kind::None, "pause/reset clears hit ownership");
    Renderer renderer{};
    Require(!renderer.Enabled(), "default off");
    renderer.Play(Kind::Light); renderer.Play(Kind::Heavy); renderer.Hit(Kind::Heavy);
    Advance(renderer, Duration(Kind::Heavy), true);
    renderer.SetEnabled(true);
    {
        Renderer chargeRenderer{};
        chargeRenderer.SetEnabled(true);
        chargeRenderer.StartCharge(Kind::ChargeHeavy);
        double early = 0.0, built = 0.0;
        float holdPeak = 0.0f, releasePeak = 0.0f;
        for (uint32_t frame = 0; frame < 57600u; ++frame) {
            if (frame % 4800u == 0) chargeRenderer.RefreshCharge(Kind::ChargeHeavy);
            const auto sample = chargeRenderer.Render();
            const double energy = sample.left * sample.left + sample.right * sample.right;
            if (frame < 9600u) early += energy;
            if (frame >= 38400u && frame < 48000u) built += energy;
            holdPeak = std::max(holdPeak, std::abs(sample.left));
        }
        Require(built > early * 1.35 && holdPeak > 0.25f,
            "held charge audibly builds instead of ticking once");
        chargeRenderer.ReleaseCharge(Kind::ChargeHeavy);
        const double release = Advance(chargeRenderer, ChargeReleaseDuration(Kind::ChargeHeavy),
            false, &releasePeak);
        Require(release > 1000.0 && releasePeak > holdPeak * 1.35f,
            "charged swing has a distinct release arc");
        chargeRenderer.Hit(Kind::ChargeHeavy);
        Require(Advance(chargeRenderer, 9600u) > 500.0,
            "charged enemy contact adds a thick impact");
        chargeRenderer.Stop();
        Advance(chargeRenderer, 480u, true);
    }
    {
        Renderer waves{};
        waves.SetEnabled(true);
        waves.PlayOrdinaryLightWave(false);
        const double small = Advance(waves, OrdinaryLightWaveDuration(false));
        Advance(waves, 13920u, true); // game sweep starts about 500 ms after release
        waves.PlayOrdinaryLightWave(true);
        const double large = Advance(waves, OrdinaryLightWaveDuration(true));
        Require(small > 100.0 && large > small * 2.0,
            "ordinary light charge has two distinct growing sword waves");
        waves.Stop();
        Advance(waves, 480u, true);
    }
    {
        Renderer impact{};
        impact.SetEnabled(true);
        impact.PlayOrdinaryHeavyDownstroke();
        const double stroke = Advance(impact, OrdinaryHeavyDownstrokeDuration);
        Advance(impact, 1440u, true); // sampled ground contact at about 430 ms
        impact.PlayOrdinaryHeavyGroundCrack();
        float crackPeak = 0.0f;
        const double crack = Advance(impact, OrdinaryHeavyGroundCrackDuration,
            false, &crackPeak);
        Require(stroke > 1000.0 && crack > 1500.0 && crackPeak > 0.65f,
            "ordinary heavy charge has an audible downstroke and sharp ground fracture");
        auto crackWindow = [](uint32_t begin, uint32_t end) {
            double left = 0.0, right = 0.0;
            for (uint32_t frame = begin; frame < end; ++frame) {
                const auto sample = OrdinaryHeavyGroundCrackSample(frame);
                left += sample.left * sample.left;
                right += sample.right * sample.right;
            }
            return std::pair{left, right};
        };
        const auto firstSplit = crackWindow(3600u, 4700u);
        const auto secondSplit = crackWindow(7400u, 8600u);
        const auto quietGap = crackWindow(5600u, 6500u);
        Require(firstSplit.first > firstSplit.second * 2.0 &&
                secondSplit.second > secondSplit.first * 2.0 &&
                firstSplit.first > quietGap.first * 2.0,
            "ground crack has separated left-right fracture transients");
        const auto finalBody = crackWindow(13000u, 19000u);
        const auto finalTail = crackWindow(19000u, 25000u);
        Require(finalBody.first + finalBody.second > 180.0 &&
                finalTail.first + finalTail.second > 40.0 &&
                finalBody.first + finalBody.second > finalTail.first + finalTail.second,
            "ground crack ends with a sustained, decaying body rather than faint taps");
        impact.Stop();
        Advance(impact, 480u, true);
    }
    renderer.Play(Kind::Light);
    Advance(renderer, SwingLeadFrames(Kind::Light), true);
    float lightPeak = 0.0f, heavyPeak = 0.0f;
    const double light = Advance(renderer, Duration(Kind::Light) - SwingLeadFrames(Kind::Light), false, &lightPeak, Kind::Light);
    Advance(renderer, 480u, true);
    Renderer heavyRenderer{};
    heavyRenderer.SetEnabled(true);
    heavyRenderer.Play(Kind::Heavy);
    Advance(heavyRenderer, SwingLeadFrames(Kind::Heavy), true, nullptr, Kind::Heavy);
    const double heavy = Advance(heavyRenderer, Duration(Kind::Heavy) - SwingLeadFrames(Kind::Heavy), false, &heavyPeak, Kind::Heavy);
    Require(lightPeak > 0.16f && lightPeak < 0.19f && heavyPeak > lightPeak * 1.25f && heavyPeak < 0.26f &&
        light < 350.0 && heavy < 900.0 && heavy > light * 2.0,
        "soft low-gain empty swings with distinct light/heavy sweeps");
    Advance(renderer, 480u, true);
    double minimumContactRatio = 1.0e9;
    for (const auto kind : {Kind::Light, Kind::Heavy}) {
        Require(BandPower(kind, 40u, 115u) < BandPower(kind, 140u, 320u) * 0.002,
            "air sweep has negligible low-frequency weight");
        renderer.Play(kind);
        Advance(renderer, SwingLeadFrames(kind), true, nullptr, kind);
        Advance(renderer, 2400u, false, nullptr, kind);
        for (uint32_t frame = SwingLeadFrames(kind) + 2400u; frame < Duration(kind) - 4800u; frame += 2400u) {
            const double window = Advance(renderer, 2400u, false, nullptr, kind);
            const double rms = std::sqrt(window / 4800.0);
            Require(rms > (kind == Kind::Heavy ? 0.010 : 0.008) && rms < 0.16,
                "each 50ms sweep window stays gentle and continuous, not one heavy click");
        }
        renderer.Stop();
        renderer.Play(kind);
        Advance(renderer, 9600u, false, nullptr, kind);
        renderer.End(); Advance(renderer, ReleaseFrames);
        Advance(renderer, Duration(Kind::Heavy), true); // early animation exit, no lingering hum
        renderer.Play(kind);
        Advance(renderer, 14400u + ReleaseFrames);
        Advance(renderer, Duration(Kind::Heavy), true); // dropped updates watchdog
        renderer.Stop();
        // Include observed real-game contact delays (~310-490 ms), not only
        // an artificially early hit before the air envelope reaches its peak.
        for (uint32_t hitAt : {7200u, 14880u, 18720u, 23520u}) {
            Renderer empty{}, contacted{};
            empty.SetEnabled(true); contacted.SetEnabled(true);
            empty.Play(kind); contacted.Play(kind);
            Advance(empty, hitAt, false, nullptr, kind);
            Advance(contacted, hitAt, false, nullptr, kind);
            contacted.Hit(kind);
            float contactPeak = 0.0f;
            const double noHitEnergy = Advance(empty, 9600u, false, nullptr, kind);
            const double hitEnergy = Advance(contacted, 9600u, false, &contactPeak, kind);
            const double ratio = hitEnergy / noHitEnergy;
            minimumContactRatio = std::min(minimumContactRatio, ratio);
            Require(ratio > 5.0 && contactPeak > (kind == Kind::Heavy ? heavyPeak : lightPeak) * 2.7f,
                "confirmed hit is clearly distinct from gentle air sweep at real contact times");
        }
        renderer.Hit(kind); Advance(renderer, 2400u);
        Require(Advance(renderer, 2400u) > 20.0, "contact stays thick beyond initial 50ms");
        renderer.Stop();
        renderer.Play(kind); Advance(renderer, 240u);
        renderer.Hit(kind);
        renderer.SetEnabled(false);
        Advance(renderer, 480u, true);
        renderer.Play(kind); renderer.Hit(kind); renderer.Refresh(kind); Advance(renderer, 480u, true);
        renderer.SetEnabled(true);
        Advance(renderer, 480u, true); // reenable does not replay disabled attacks
        renderer.Play(kind); renderer.Hit(kind); Advance(renderer, 240u);
        renderer.Stop(); Advance(renderer, 480u, true); // pause / reset
    }
    renderer.Play(Kind::None); Advance(renderer, 480u, true);
    {
        Renderer crossover{};
        crossover.SetEnabled(true);
        crossover.Play(Kind::Heavy, 10560u, 26880u);
        Advance(crossover, 10560u, true, nullptr, Kind::Heavy);
        Require(Advance(crossover, 4800u, false, nullptr, Kind::Heavy) > 1.0,
            "crossover first automatic cut renders");
        Advance(crossover, 11520u, false, nullptr, Kind::Heavy);
        Require(Advance(crossover, 4800u, false, nullptr, Kind::Heavy) > 1.0,
            "crossover second automatic cut renders within the same action");
    }
    for (uint32_t gap : {48u, 96u, 240u, 480u, 1440u, 4800u}) {
        renderer.Stop();
        for (uint32_t index = 0; index < 32u; ++index) {
            renderer.Play(index % 3u == 0 ? Kind::Light : Kind::Heavy);
            renderer.Hit(index % 3u == 0 ? Kind::Light : Kind::Heavy);
            Advance(renderer, gap);
        }
        Advance(renderer, Duration(Kind::Heavy));
        Advance(renderer, 480u, true); // no backlog after rapid combo triggers
    }
    Renderer phaseA{}, phaseB{};
    phaseA.SetEnabled(true); phaseB.SetEnabled(true); phaseA.Play(Kind::Heavy); phaseB.Play(Kind::Heavy);
    for (uint32_t frame = 0; frame < Duration(Kind::Heavy); ++frame) {
        if (frame % 4800u == 0) phaseA.Refresh(Kind::Heavy);
        if (frame % 7200u == 0) phaseB.Refresh(Kind::Heavy);
        const auto a = phaseA.Render(), b = phaseB.Render();
        Require(a.left == b.left && a.right == b.right, "refresh cadence never restarts waveform phase");
    }
    std::printf("Normal attack phase/owner/damage/dedup/exit gating, gentle high-mid air sweeps, distinct thick hits, refresh phase, renderer initialization/mute, pause, watchdog, combo/headroom tests passed peak=%.3f/%.3f energy=%.0f/%.0f minHitEnergyRatio=%.1f\n",
        lightPeak, heavyPeak, light, heavy, minimumContactRatio);
}

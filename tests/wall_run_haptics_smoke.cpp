#include "wall_run_haptics.hpp"
#include <cstdio>
#include <cstdlib>
#include <limits>

using namespace wall_run_haptics;
static_assert(ClassifyAction("app.PlayerBasicAction.cWallRunStartJump") == Phase::Takeoff);
static_assert(ClassifyAction("app.PlayerOneHandedAction.cWallRunStart") == Phase::Contact);
static_assert(ClassifyAction("app.PlayerOneHandedAction.cWallRunLoop") == Phase::Move);
static_assert(ClassifyAction("app.PlayerOneHandedAction.cWallRunJumpToWall") == Phase::Leap);
static_assert(ClassifyAction("app.PlayerOneHandedAction.cWallRunEnd") == Phase::GroundLeap);
static_assert(ClassifyAction("app.PlayerOneHandedAction.cWallRunRouteEnd") == Phase::GroundLeap);
static_assert(ClassifyAction("app.PlayerOneHandedAction.cWallRunJumpToGoal") == Phase::GroundLeap);
static_assert(ClassifyAction("app.PlayerOneHandedAction.cWallRunJumpToWallLand") == Phase::LandSmall);
static_assert(ClassifyAction("app.PlayerOneHandedAction.cWallRunRouteEndLandLarge") == Phase::LandLarge);
static_assert(ClassifyAction("app.PlayerOneHandedAction.cWallRunJumpToWallFall") == Phase::Fall);
static_assert(ClassifyAction("app.PlayerOneHandedDamageAction.cWallCrashBack") == Phase::None);
static_assert(ClassifyAction("app.PlayerOneHandedDamageAction.cWallRunDamageLand") == Phase::None);
static_assert(ClassifyAction("app.PlayerOneHandedAction.cWallRunGuard") == Phase::None);
static_assert(ClassifyAction("app.PlayerBasicAction.cLiftWallOpenLoop") == Phase::None);
static_assert(ClassifyAction("app.EnemyBaseAction.cWallRunLoop") == Phase::None);
static_assert(FromWallStyle(0) == Side::Left && FromWallStyle(1) == Side::Right);
static_assert(FromWallStyle(-1) == Side::Center && FromWallStyle(2) == Side::Center);
static_assert(IsFootContact(0, 0) && IsFootContact(2, 5));
static_assert(!IsFootContact(1, 0) && !IsFootContact(3, 0));
static_assert(!IsFootContact(2, -1) && !IsFootContact(2, 6));
static_assert(EvidenceFresh(1419, 1000) && !EvidenceFresh(1420, 1000));
static_assert(!EvidenceFresh(999, 1000) && !EvidenceFresh(1000, 0));
static_assert(SustainsFlight(Phase::Takeoff) && SustainsFlight(Phase::Leap));
static_assert(!SustainsFlight(Phase::Fall) && !SustainsFlight(Phase::GroundLeap));
static_assert(!SustainsFlight(Phase::Contact) && !SustainsFlight(Phase::LandSmall));
static_assert(static_cast<uint8_t>(Event::Detach) == 8 &&
    static_cast<uint8_t>(Event::AirMove) == 9 && static_cast<uint8_t>(Event::GroundLeap) == 10);
static_assert(ImpulseFrames(Event::GroundLeap) == 6720u);

static void Require(bool condition, const char* message) {
    if (!condition) { std::fprintf(stderr, "Wall Run test failed: %s\n", message); std::exit(1); }
}
static void Check(StereoSample sample) {
    Require(std::isfinite(sample.left) && std::isfinite(sample.right), "finite sample");
    Require(std::abs(sample.left) < 1.0f && std::abs(sample.right) < 1.0f,
        "isolated default-strength headroom");
}
static double Energy(StereoSample sample) {
    return sample.left * sample.left + sample.right * sample.right;
}
static double Advance(Renderer& renderer, uint32_t frames, bool silent = false) {
    double energy = 0.0;
    for (uint32_t i = 0; i < frames; ++i) {
        const auto sample = renderer.Render();
        Check(sample);
        if (silent) Require(sample.left == 0.0f && sample.right == 0.0f, "must stop completely");
        energy += Energy(sample);
    }
    return energy;
}
static double AdvanceRefreshed(Renderer& renderer, uint32_t frames, Event event) {
    double energy = 0.0;
    for (uint32_t i = 0; i < frames; ++i) {
        if (i > 0 && i % 7200u == 0) renderer.Apply(event, Side::Center, 1.0f);
        const auto sample = renderer.Render();
        Check(sample);
        energy += Energy(sample);
    }
    return energy;
}
// The 140 ms departure cue from the pre-glide implementation. Keep a golden
// reference so restoring the old feel is checked sample-for-sample, not only
// by requiring some output energy.
static StereoSample LegacyGroundSample(uint32_t frame, Side side) {
    constexpr double TwoPi = 6.28318530717958647692;
    constexpr uint32_t Duration = 6720u;
    if (frame >= Duration) return {};
    const double time = static_cast<double>(frame) / SampleRate;
    const double attackX = std::min(time / 0.004, 1.0);
    const double attack = attackX * attackX * (3.0 - 2.0 * attackX);
    const double decay = std::exp(-std::max(time - 0.018, 0.0) / 0.026);
    const double remaining = static_cast<double>(Duration - 1u - frame) / SampleRate;
    const double fade = std::clamp(remaining / 0.020, 0.0, 1.0);
    const double carrier = 0.90 * std::sin(TwoPi * (76.0 * time - 10.0 * time * time)) +
        0.10 * std::sin(TwoPi * 76.0 * 1.85 * time);
    return Directional(static_cast<float>(0.48 * attack * decay * fade *
        std::tanh(carrier * 1.75)), side);
}

static void TestMovingLandingTracker() {
    using Contact = LandingTracker::ContactResult;
    LandingTracker tracker{};
    Require(tracker.OfferContact(1000, 0, 0) == Contact::Ignored,
        "ordinary walking cannot arm landing");
    Require(!tracker.PublishFallback(2000), "no departure means no fallback");

    // Missing dedicated landing while movement is held: the first contact
    // wins once, replaces a footstep, and permits no subsequent walking hits.
    tracker.ObserveDeparture(Phase::GroundLeap, 1000);
    Require(tracker.OfferContact(1100, 0, 0) == Contact::Ignored, "ignore departure foot FX");
    Require(!tracker.PublishFallback(1700), "never land from elapsed time alone");
    Require(tracker.OfferContact(1800, 1, 0) == Contact::Ignored, "lift is not landing");
    Require(tracker.OfferContact(1800, 3, 0) == Contact::Ignored, "slide is not landing");
    Require(tracker.OfferContact(1800, 0, 6) == Contact::Ignored, "invalid foot is not landing");
    Require(tracker.OfferContact(1800, 0, 0) == Contact::Queued, "moving contact queues landing");
    Require(tracker.OfferContact(1805, 2, 1) == Contact::Pending, "second foot cannot queue twice");
    Require(!tracker.PublishFallback(1831), "give explicit landing entry priority");
    Require(tracker.PublishFallback(1832), "publish moving landing after contact");
    Require(!tracker.PublishFallback(1840), "publish only once");
    Require(tracker.OfferContact(1850, 0, 1) == Contact::Ignored,
        "continued movement does not repeat landing");
    Require(!tracker.AcceptExplicit(1870), "late explicit entry does not duplicate fallback");
    Require(!tracker.AcceptExplicit(2120), "same-flight late landing beyond debounce cannot repeat");
    tracker.ObserveDeparture(Phase::Fall, 2150);
    Require(tracker.OfferContact(2210, 0, 0) == Contact::Ignored,
        "late fall after fallback cannot rearm the same descent");

    // A later jump remains independent, including step rather than contact FX.
    tracker.ObserveDeparture(Phase::GroundLeap, 2200);
    Require(tracker.OfferContact(2499, 2, 1) == Contact::Queued, "next jump accepts step contact");
    Require(tracker.PublishFallback(2531), "next jump lands independently");
    tracker.ObserveDeparture(Phase::Takeoff, 2600);
    Require(tracker.AcceptExplicit(2800), "new approach clears previous fallback suppression");

    // If the game's dedicated small/large entry exists, it wins unchanged.
    tracker.Reset();
    tracker.ObserveDeparture(Phase::GroundLeap, 1000);
    Require(tracker.OfferContact(2100, 0, 0) == Contact::Queued, "contact before explicit");
    Require(tracker.AcceptExplicit(2110), "explicit small/large wins during preference window");
    Require(!tracker.PublishFallback(2132), "explicit cancels pending fallback");
    Require(!tracker.AcceptExplicit(2111), "derived/base landing pair deduplicated");
    Require(tracker.OfferContact(2140, 0, 1) == Contact::Ignored, "explicit consumes departure");

    // Fall after departure retains the original ticket. A direct fall gets its
    // own guarded ticket. Returning to a wall/new jump cancels stale tickets.
    tracker.Reset();
    tracker.ObserveDeparture(Phase::GroundLeap, 1000);
    tracker.ObserveDeparture(Phase::Fall, 1550);
    Require(tracker.OfferContact(1560, 0, 0) == Contact::Queued, "fall retains departure ticket");
    Require(tracker.PublishFallback(1592), "fall path lands");
    tracker.Reset();
    tracker.ObserveDeparture(Phase::Fall, 1000);
    Require(tracker.OfferContact(1059, 0, 0) == Contact::Ignored, "direct fall departure guard");
    Require(tracker.OfferContact(1060, 0, 0) == Contact::Queued, "direct fall ground contact");
    Require(tracker.PublishFallback(1092), "direct fall fallback");
    for (const auto phase : {Phase::Takeoff, Phase::Contact, Phase::Move, Phase::Leap}) {
        tracker.Reset();
        tracker.ObserveDeparture(Phase::GroundLeap, 1000);
        tracker.OfferContact(1800, 0, 0);
        tracker.ObserveDeparture(phase, 1810);
        Require(!tracker.PublishFallback(1832), "wall/new approach cancels old contact");
    }

    tracker.Reset();
    tracker.ObserveDeparture(Phase::GroundLeap, 1000);
    Require(tracker.OfferContact(3400, 0, 0) == Contact::Ignored, "departure ticket expires");
    Require(!tracker.PublishFallback(3432), "expired ticket cannot cause a delayed walking hit");
    tracker.ObserveDeparture(Phase::GroundLeap, 4000);
    Require(tracker.OfferContact(3999, 0, 0) == Contact::Ignored, "stale callback timestamp ignored");
    Require(!tracker.PublishFallback(3999), "stale polling timestamp ignored");
    Require(tracker.OfferContact(4800, 0, 0) == Contact::Queued,
        "stale timestamp must not erase newer departure");
    tracker.Reset(); // pause, menu, scene reset
    Require(!tracker.PublishFallback(4832), "pause clears queued landing");
    Require(tracker.OfferContact(4900, 0, 0) == Contact::Ignored, "no resume ghost landing");
}

int main() {
    TestMovingLandingTracker();
    Renderer renderer{};
    Advance(renderer, 480u, true);
    renderer.Apply(Event::Takeoff, Side::Center, 1.0f);
    double takeoff = 0.0;
    float takeoffPeak = 0.0f;
    for (uint32_t frame = 0; frame < ImpulseFrames(Event::Takeoff); ++frame) {
        const auto sample = renderer.Render();
        Check(sample);
        takeoff += Energy(sample);
        takeoffPeak = std::max(takeoffPeak, std::abs(sample.left));
    }
    Require(takeoff > 450.0 && takeoffPeak > 0.60f, "heavy takeoff push");
    Require(Advance(renderer, 4800u) > 40.0, "glide continues after launch impulse");
    Advance(renderer, HoldFrames + 2400u);
    Advance(renderer, 480u, true);

    Renderer left{}, right{}, withEarlyStep{};
    left.Apply(Event::Contact, Side::Left, 1.0f);
    right.Apply(Event::Contact, Side::Right, 1.0f);
    withEarlyStep.Apply(Event::Contact, Side::Left, 1.0f);
    withEarlyStep.Apply(Event::Step, Side::Right, 1.0f);
    double contact = 0.0;
    float peak = 0.0f;
    for (uint32_t frame = 0; frame < 4800u; ++frame) {
        const auto l = left.Render(), r = right.Render(), d = withEarlyStep.Render();
        Check(l); Check(r);
        Require(l.left == d.left && l.right == d.right, "initial foot/contact duplicate ignored");
        Require(std::abs(l.left - r.right) < 0.000001f &&
            std::abs(l.right - r.left) < 0.000001f, "wall side mirrors output");
        contact += Energy(l);
        peak = std::max(peak, std::abs(l.left));
    }
    Require(contact > 450.0 && peak > 0.60f, "wall contact weight preserved");

    // Contact -> detach -> loop must preserve its one-shot rather than cutting
    // it off when next-action enter and previous-action exit are adjacent.
    renderer.Stop();
    renderer.Apply(Event::Contact, Side::Left, 1.0f);
    Advance(renderer, 240u);
    renderer.Apply(Event::Detach, Side::Center, 0.0f);
    Require(Advance(renderer, 1200u) > 150.0, "detach preserves push impulse");
    renderer.Apply(Event::Move, Side::Left, 1.0f);

    // Different refresh schedules must produce exactly the same waveform.
    Renderer frequent{}, sparse{};
    for (uint32_t block = 0; block < 24u; ++block) {
        double energy = 0.0;
        for (uint32_t i = 0; i < 4800u; ++i) {
            const uint32_t frame = block * 4800u + i;
            if (frame % 4800u == 0) frequent.Apply(Event::Move, Side::Right, 1.0f);
            if (frame % 7200u == 0) sparse.Apply(Event::Move, Side::Right, 1.0f);
            const auto a = frequent.Render(), b = sparse.Render();
            Check(a); Check(b);
            Require(a.left == b.left && a.right == b.right, "refresh must not restart phase");
            energy += Energy(a);
        }
        Require(energy > 12.0, "along-wall texture has no 100 ms quiet gap");
    }
    Advance(sparse, HoldFrames + 2400u);
    Advance(sparse, 480u, true); // missing updates expire instead of lingering

    // Real foot contacts layer over texture; quick successive feet remain safe.
    renderer.Stop();
    renderer.Apply(Event::Contact, Side::Left, 1.0f);
    for (uint32_t frame = 0; frame < 24000u; ++frame) {
        if (frame % 7200u == 0) renderer.Apply(Event::Move, Side::Left, 1.0f);
        if (frame >= 3840u && frame % 3840u == 0)
            renderer.Apply(Event::Step, (frame / 3840u) % 2u == 0 ? Side::Left : Side::Right, 1.0f);
        Check(renderer.Render());
    }
    renderer.Apply(Event::Leap, Side::Left, 1.0f);
    Require(Advance(renderer, ImpulseFrames(Event::Leap)) > 450.0, "heavy leap cue");
    Require(Advance(renderer, 4800u) > 40.0, "airborne body without wall friction");
    renderer.Apply(Event::Detach, Side::Center, 0.0f);
    Advance(renderer, 2400u);
    Advance(renderer, 480u, true);

    // Air refreshes must not erase the launch impulse, restart the phase, or
    // leave gaps during an approach/transfer flight.
    Renderer airFrequent{}, airSparse{};
    airFrequent.Apply(Event::Leap, Side::Center, 1.0f);
    airSparse.Apply(Event::Leap, Side::Center, 1.0f);
    for (uint32_t block = 0; block < 12u; ++block) {
        double energy = 0.0;
        for (uint32_t i = 0; i < 4800u; ++i) {
            const uint32_t frame = block * 4800u + i;
            if (frame % 4800u == 0) airFrequent.Apply(Event::AirMove, Side::Center, 1.0f);
            if (frame % 7200u == 0) airSparse.Apply(Event::AirMove, Side::Center, 1.0f);
            const auto a = airFrequent.Render(), b = airSparse.Render();
            Check(a); Check(b);
            Require(a.left == b.left && a.right == b.right,
                "air refresh preserves launch and waveform phase");
            energy += Energy(a);
        }
        Require(energy > 40.0, "flight has no 100 ms quiet gap");
    }
    Advance(airSparse, HoldFrames + 2400u);
    Advance(airSparse, 480u, true);
    airFrequent.Apply(Event::LandSmall, Side::Center, 1.0f);
    Advance(airFrequent, ImpulseFrames(Event::LandSmall));
    Advance(airFrequent, 480u, true); // landing stops the airborne body

    // Leaving the wall for the ground restores the old 140 ms pulse exactly,
    // without starting a glide. Fall detaches an existing glide, and updates
    // cannot refresh it because Fall/GroundLeap are not sustained phases.
    for (const auto side : {Side::Left, Side::Right, Side::Center}) {
        Renderer ground{};
        ground.Apply(Event::GroundLeap, side, 1.0f);
        for (uint32_t frame = 0; frame < 6720u; ++frame) {
            const auto sample = ground.Render(), old = LegacyGroundSample(frame, side);
            Check(sample);
            Require(sample.left == old.left && sample.right == old.right,
                "ground departure matches pre-glide waveform exactly");
        }
        Advance(ground, 4800u, true);
    }
    Renderer fall{};
    fall.Apply(Event::Detach, Side::Center, 0.0f);
    Advance(fall, 480u, true); // walking off the wall must not start flight haptics
    fall.Apply(Event::Takeoff, Side::Center, 1.0f);
    Advance(fall, 14400u);
    fall.Apply(Event::Detach, Side::Center, 0.0f);
    Advance(fall, 2400u);
    Advance(fall, 4800u, true); // no sustained haptics during descent

    // Foot FX during flight must not accidentally layer grounded/wall steps.
    Renderer airOnly{}, airWithStep{};
    airOnly.Apply(Event::AirMove, Side::Center, 1.0f);
    airWithStep.Apply(Event::AirMove, Side::Center, 1.0f);
    Advance(airOnly, 4800u); Advance(airWithStep, 4800u);
    airWithStep.Apply(Event::Step, Side::Left, 1.0f);
    for (uint32_t i = 0; i < 4800u; ++i) {
        const auto a = airOnly.Render(), b = airWithStep.Render();
        Require(a.left == b.left && a.right == b.right, "no footsteps in flight");
    }

    // Replay timings observed in the real scene, not invented fixed lengths:
    // 390 ms approach, wall traverse, 504 ms transfer, 1106 ms exit to goal.
    renderer.Stop();
    renderer.Apply(Event::Takeoff, Side::Center, 1.0f);
    AdvanceRefreshed(renderer, 18720u, Event::AirMove);
    renderer.Apply(Event::Contact, Side::Left, 1.0f);
    AdvanceRefreshed(renderer, 52752u, Event::Move);
    renderer.Apply(Event::Move, Side::Left, 1.0f);
    AdvanceRefreshed(renderer, 17664u, Event::Move);
    renderer.Apply(Event::Leap, Side::Left, 1.0f);
    AdvanceRefreshed(renderer, 24192u, Event::AirMove);
    renderer.Apply(Event::LandSmall, Side::Center, 1.0f);
    Advance(renderer, 21216u);
    renderer.Apply(Event::GroundLeap, Side::Center, 1.0f);
    Advance(renderer, ImpulseFrames(Event::GroundLeap));
    Advance(renderer, 53088u - ImpulseFrames(Event::GroundLeap), true);
    renderer.Apply(Event::LandSmall, Side::Center, 1.0f);
    Advance(renderer, ImpulseFrames(Event::LandSmall));
    Advance(renderer, 480u, true);

    // Recontact must end the air layer and converge to unchanged wall output.
    Renderer recontact{}, referenceContact{};
    recontact.Apply(Event::AirMove, Side::Center, 1.0f);
    Advance(recontact, 4800u);
    recontact.Apply(Event::Contact, Side::Left, 1.0f);
    referenceContact.Apply(Event::Contact, Side::Left, 1.0f);
    Advance(recontact, 2400u); Advance(referenceContact, 2400u);
    for (uint32_t i = 0; i < 4800u; ++i) {
        const auto a = recontact.Render(), b = referenceContact.Render();
        Require(a.left == b.left && a.right == b.right, "recontact ends air layer");
    }

    Renderer small{}, large{};
    small.Apply(Event::LandSmall, Side::Center, 1.0f);
    large.Apply(Event::LandLarge, Side::Center, 1.0f);
    const double smallEnergy = Advance(small, ImpulseFrames(Event::LandSmall));
    const double largeEnergy = Advance(large, ImpulseFrames(Event::LandLarge));
    Require(largeEnergy > smallEnergy * 1.4, "large landing has greater weight");
    Advance(small, 480u, true); Advance(large, 480u, true);

    for (const auto event : {Event::Contact, Event::Takeoff, Event::Leap,
        Event::AirMove, Event::GroundLeap, Event::LandSmall, Event::LandLarge}) {
        renderer.Apply(event, Side::Left, 1.0f);
        Advance(renderer, 240u);
        renderer.Apply(Event::Stop, Side::Center, 0.0f);
        Advance(renderer, 480u, true); // pause/reset clears every layer immediately
        renderer.Apply(event, Side::Center, 0.0f);
        Advance(renderer, 480u, true);
    }
    renderer.Apply(Event::Move, Side::Left, std::numeric_limits<float>::quiet_NaN());
    renderer.Apply(Event::AirMove, Side::Center, std::numeric_limits<float>::quiet_NaN());
    Advance(renderer, 480u, true);
    std::printf("Wall Run moving/standing/fall landing fallback and deduplication, action/side/contact routing, heavy launch, transfer flight, legacy ground departure, silent descent, real timeline, texture/steps/landing, refresh, timeout, pause/mute tests passed launchPeak=%.3f contactPeak=%.3f landingEnergy=%.0f/%.0f\n",
        takeoffPeak, peak, smallEnergy, largeEnergy);
    return 0;
}

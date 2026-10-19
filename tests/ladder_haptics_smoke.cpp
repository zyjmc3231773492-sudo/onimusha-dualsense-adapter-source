#include "ladder_haptics.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>

using namespace ladder_haptics;

static_assert(ClassifyAction("app.PlayerBasicAction.cLadderClimbUpStart") ==
    Phase::Start);
static_assert(ClassifyAction("app.PlayerOneHandedAction.cLadderClimbUpLoopL") ==
    Phase::Climb);
static_assert(ClassifyAction("app.PlayerBasicAction.cLadderClimbDownFastLoopR") ==
    Phase::FastDown);
static_assert(ClassifyAction("app.PlayerOneHandedAction.cLadderClimbDownFastStopL") ==
    Phase::FastStop);
static_assert(ClassifyAction("app.PlayerBasicAction.cLadderClimbDownEndR") ==
    Phase::End);
static_assert(ClassifyAction("app.PlayerBasicAction.cLadderFallBase") ==
    Phase::Fall);
static_assert(ClassifyAction("app.PlayerBasicAction.cLadderIdleL") ==
    Phase::Idle);
static_assert(ClassifyAction("app.PlayerBasicAction.cLadderGuard") ==
    Phase::None);
static_assert(ClassifyAction("app.PlayerOneHandedDamageAction.cLadderClimbUpLoopL") ==
    Phase::None);
static_assert(ClassifyAction("app.EnemyBaseAction.cLadderClimbUpLoopL") ==
    Phase::None);
static_assert(SideFromAction("app.PlayerBasicAction.cLadderClimbUpLoopL") ==
    Side::Left);
static_assert(SideFromAction("app.PlayerBasicAction.cLadderClimbUpLoopR") ==
    Side::Right);
static_assert(SideFromAction("app.PlayerBasicAction.cLadderClimbStart") ==
    Side::Center);
static_assert(IsProfile(ClimbProfile) && IsProfile(FastSlideProfile));
static_assert(!IsProfile(0) && !IsProfile(7));
static_assert(EvidenceFresh(1419, 1000) && !EvidenceFresh(1420, 1000));
static_assert(!EvidenceFresh(999, 1000) && !EvidenceFresh(1000, 0));

static void Require(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "Ladder haptics test failed: %s\n", message);
        std::exit(1);
    }
}

static double Energy(StereoSample sample) {
    Require(std::isfinite(sample.left) && std::isfinite(sample.right),
        "finite sample");
    Require(std::abs(sample.left) < 1.0f && std::abs(sample.right) < 1.0f,
        "isolated default-strength headroom");
    return sample.left * sample.left + sample.right * sample.right;
}

static double Advance(Renderer& renderer, uint32_t frames, bool silent = false) {
    double energy = 0.0;
    for (uint32_t i = 0; i < frames; ++i) {
        const auto sample = renderer.Render();
        if (silent) Require(sample.left == 0.0f && sample.right == 0.0f,
            "must stop completely");
        energy += Energy(sample);
    }
    return energy;
}

int main() {
    Renderer left{};
    left.Apply(Profile::Climb, Operation::Start, Side::Left, 1.0f);
    left.Apply(Profile::Climb, Operation::Accent, Side::Left, 1.0f);
    const double climbEnergy = Advance(left, 9600u);
    Require(climbEnergy > 30.0, "softened climb texture and grip accent still carry energy");

    left.Stop();
    left.Apply(Profile::Climb, Operation::Start, Side::Left, 1.0f);
    left.Apply(Profile::Climb, Operation::Accent, Side::Left, 1.0f);
    Renderer right{};
    right.Apply(Profile::Climb, Operation::Start, Side::Right, 1.0f);
    right.Apply(Profile::Climb, Operation::Accent, Side::Right, 1.0f);
    for (uint32_t i = 0; i < 9600u; ++i) {
        const auto a = left.Render();
        const auto b = right.Render();
        Require(std::abs(a.left - b.right) < 0.000001f &&
            std::abs(a.right - b.left) < 0.000001f,
            "left/right grip texture mirrors output");
    }

    // Refresh extends the lease without restarting the carrier phase. This
    // models the bridge's 150 ms refresh schedule and guards against gaps.
    Renderer frequent{};
    Renderer sparse{};
    frequent.Apply(Profile::Climb, Operation::Start, Side::Center, 1.0f);
    sparse.Apply(Profile::Climb, Operation::Start, Side::Center, 1.0f);
    for (uint32_t frame = 0; frame < 28800u; ++frame) {
        if (frame > 0 && frame % 4800u == 0)
            frequent.Apply(Profile::Climb, Operation::Refresh, Side::Center, 1.0f);
        if (frame > 0 && frame % 7200u == 0)
            sparse.Apply(Profile::Climb, Operation::Refresh, Side::Center, 1.0f);
        const auto a = frequent.Render();
        const auto b = sparse.Render();
        Require(std::abs(a.left - b.left) < 0.000001f &&
            std::abs(a.right - b.right) < 0.000001f,
            "refresh preserves waveform phase");
    }
    Require(Advance(frequent, 4800u) > 10.0, "refreshed climb has no quiet gap");
    sparse.Apply(Profile::Climb, Operation::Stop, Side::Center, 0.0f);
    Advance(sparse, 480u, true);

    Renderer fast{};
    fast.Apply(Profile::FastSlide, Operation::Start, Side::Center, 1.0f);
    fast.Apply(Profile::FastSlide, Operation::Accent, Side::Center, 1.0f);
    const double fastEnergy = Advance(fast, 9600u);
    Require(fastEnergy > climbEnergy * 1.05,
        "fast slide has a stronger continuous texture");
    fast.Apply(Profile::FastSlide, Operation::Decelerate, Side::Center, 1.0f);
    Require(Advance(fast, 8640u) > 40.0, "fast slide deceleration cue");
    fast.Apply(Profile::FastSlide, Operation::Stop, Side::Center, 0.0f);
    Advance(fast, 480u, true);

    Renderer invalid{};
    invalid.Apply(Profile::Climb, Operation::Start, Side::Center,
        std::numeric_limits<float>::quiet_NaN());
    Advance(invalid, 480u, true);

    std::printf("Ladder action routing, side alternation, refresh lease, fast-slide texture, deceleration and stop tests passed climb=%.1f fast=%.1f\n",
        climbEnergy, fastEnergy);
    return 0;
}

#include "live_motor_state.hpp"
#include <cstdio>
#include <cstdlib>
#include <limits>
using namespace live_motor_state;
static void Require(bool condition, const char* message) {
    if (!condition) { std::fprintf(stderr, "Live motor failed: %s\n", message); std::exit(1); }
}
int main() {
    Sources sources;
    sources.Native(100, 0, 0.017f, 1000);
    sources.Native(200, 0, 0.5f, 1000);
    Require(sources.Resolve().power[0] == 0.5f, "native fallback works");
    sources.XInput(0, 0.5f, 0.4f, true);
    sources.Native(100, 0, 0.017f, 1100);
    sources.XInput(0, 0.0f, 0.0f, true);
    Require(sources.Resolve().kind == Kind::XInput && sources.Resolve().power[0] == 0,
        "canonical zero cannot be held up by stale native mirrors");
    sources.ExpireNative(100000);
    Require(sources.HasXInput(0) && sources.Resolve().power[0] == 0,
        "connected XInput authority is not an arbitrary short timeout");
    sources.XInput(0, 0.001f, 0.017f, true);
    sources.Native(300, 2, 0.025f, 100100);
    Require(sources.Resolve().power[0] == 0.001f && sources.Resolve().power[1] == 0.017f,
        "legitimate weak live output is NOT threshold filtered");
    Require(sources.Resolve().power[2] == 0.025f &&
        sources.Resolve().motorKind[2] == Kind::NativePad,
        "native trigger channels survive canonical body-motor routing");
    sources.XInput(1, 0.9f, 0.9f, false);
    Require(sources.Resolve().power[0] == 0.001f, "failed/disconnected writes excluded");
    sources.ExpireNative(999999);
    Require(sources.Resolve().power[1] == 0.017f, "valid sustained live output preserved");
    sources.RemoveXInput(0);
    Require(sources.Resolve().power[0] == 0 && sources.Resolve().kind == Kind::None,
        "disconnect removes authority and held state");
    sources.Native(400, 0, 0.017f, 1000000);
    sources.XInput(0, 0.4f, 0.4f, true);
    sources.RemoveXInput(0);
    Require(sources.Resolve().power[0] == 0, "disconnect cannot revive superseded native mirrors");
    sources.Native(100, 0, 0.017f, 1000000);
    sources.Native(100, 1, 0.4f, 1001999);
    sources.ExpireNative(1002000);
    Require(sources.Resolve().power[0] == 0 && sources.Resolve().power[1] == 0.4f,
        "refreshing another channel cannot keep a stale channel alive");
    for (uint64_t now = 1002000; now < 1020000; now += 100) {
        sources.Native(100, 0, 0.017f, now);
        sources.ExpireNative(now);
        Require(sources.Resolve().power[0] == 0.017f, "unchanged but refreshed native signal preserved");
    }
    sources.Clear();
    Require(sources.Resolve().power == std::array<float, 4>{}, "stop clears sender cache");
    for (uintptr_t source = 0; source < 40; ++source) sources.Native(source, 0, 0.5f, 2000000);
    Require(sources.NativeCount() == 16, "source table bounded");
    sources.ExpireNative(2002000);
    Require(sources.NativeCount() == 0, "expired source objects reclaimed");
    Require(ShouldSend(0.001f, 0, false), "zero edge bypasses delta compression");
    Require(ShouldSend(0, 0.001f, false), "weak start bypasses delta compression");
    Require(ShouldSend(0.017f, 0.017f, true), "unchanged state gets heartbeat");
    Require(!ShouldSend(0.1f, 0.1001f, false), "normal delta compression retained");
    TargetLease target;
    target.Set(0, 0.017f, 1000);
    target.Set(1, 0.001f, 3999);
    Require(target.Expire(4000) == 1 && target.Power()[0] == 0 && target.Power()[1] == 0.001f,
        "receiver timeout independent by channel without amplitude cutoff");
    target.Clear();
    for (uint64_t now = 5000; now < 30000; now += HeartbeatMs) {
        target.Set(0, 0.001f, now);
        Require(target.Expire(now + HeartbeatMs) == 0, "heartbeat preserves long weak signal");
    }
    target.Set(0, std::numeric_limits<float>::quiet_NaN(), 30000);
    Require(target.Power()[0] == 0, "nonfinite input sanitized");
    target.Clear();
    Require(target.Power() == std::array<float, 4>{}, "receiver stop clears all channels");
    std::puts("Live motor state smoke tests passed");
}

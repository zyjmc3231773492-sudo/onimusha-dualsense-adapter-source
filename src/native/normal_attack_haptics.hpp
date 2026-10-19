#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string_view>

namespace normal_attack_haptics {
constexpr uint8_t Message = 0xE7;
constexpr uint32_t SampleRate = 48000u;
enum class Kind : uint8_t { None, Light, Heavy, ChargeLight, ChargeHeavy };
enum class Operation : uint8_t {
    Cancel, Start, Refresh, End, Hit,
    ChargeStart, ChargeRefresh, ChargeRelease, ChargeCancel, ChargeHit,
    OrdinaryLightWaveSmall, OrdinaryLightWaveLarge,
    OrdinaryHeavyDownstroke, OrdinaryHeavyGroundCrack,
    NoEvent = 255
};
constexpr uint64_t EvidenceTimeoutMs = 300u;
constexpr uint32_t ReleaseFrames = 1680u; // 35 ms rounded action exit

// Actual gameplay capture: 气焰 has IsSoulBoost=1, BoostType=1 while
// _SoulBoostEffectTypePowerUp remains 0. Effect flags are not the state enum.
// Fail closed for missing/unknown types; activity alone also admits Oni.
constexpr int32_t FightingSpiritBoostType = 1;
constexpr bool IsFightingSpiritState(bool soulBoostActive, int32_t boostType) {
    return soulBoostActive && boostType == FightingSpiritBoostType;
}
// Oni transformation is exposed by a separate live effect flag and must not
// be inferred from an unverified BoostType number. Requiring both the Oni type
// flag and the active-effect flag excludes stale values between transitions.
constexpr bool IsEnhancedAttackState(bool soulBoostActive, int32_t boostType,
    bool oniEffectType, bool effectActive) {
    return IsFightingSpiritState(soulBoostActive, boostType) ||
        (oniEffectType && effectActive);
}
constexpr bool IsOniAttackState(bool oniEffectType, bool effectActive) {
    return oniEffectType && effectActive;
}
// The effect flags do not reset in a fixed order on every battle-exit path.
// Captured customer traces can return to an inactive type-0 boost while the
// generic active-effect flag is still true and the Oni type is already false.
// Treat only a confirmed active Oni pair as non-ordinary; this also keeps the
// existing stale-Oni-type/ended-effect transition valid. Require a resolved
// supporter before calling this helper.
constexpr bool IsOrdinaryAttackState(bool soulBoostActive, int32_t boostType,
    bool oniEffectType, bool effectActive) {
    const bool activeOni = oniEffectType && effectActive;
    return !soulBoostActive && boostType == 0 && !activeOni;
}
constexpr bool IsChargeKind(Kind kind) {
    return kind == Kind::ChargeLight || kind == Kind::ChargeHeavy;
}
constexpr bool IsHeavyKind(Kind kind) {
    return kind == Kind::Heavy || kind == Kind::ChargeHeavy;
}
constexpr Kind BaseKind(Kind kind) {
    if (kind == Kind::ChargeLight) return Kind::Light;
    if (kind == Kind::ChargeHeavy) return Kind::Heavy;
    return kind;
}

// Verified concrete TypeDB names: ordinary chains and light/heavy crossovers.
// Exact matching deliberately excludes skills, Issen, executions, counters,
// charge waiting states, enemy attacks and attack-repelled reactions.
constexpr Kind ClassifyAction(std::string_view name) {
    constexpr std::string_view Prefix = "app.PlayerOneHandedAction.";
    if (!name.starts_with(Prefix)) return Kind::None;
    name.remove_prefix(Prefix.size());
    if (name == "cAttackWeak1" || name == "cAttackWeak2" || name == "cAttackWeak3" ||
        name == "cAttackWeakFromStrong1" || name == "cAttackWeakFromStrong2") return Kind::Light;
    if (name == "cAttackStrong1" || name == "cAttackStrong2" || name == "cAttackStrong3" ||
        name == "cAttackStrongFromWeak1" || name == "cAttackStrongFromWeak2") return Kind::Heavy;
    return Kind::None;
}
constexpr Kind ClassifyChargeAction(std::string_view name) {
    constexpr std::string_view Prefix = "app.PlayerOneHandedAction.";
    if (!name.starts_with(Prefix)) return Kind::None;
    name.remove_prefix(Prefix.size());
    if (name == "cAttackWeakCharge") return Kind::ChargeLight;
    if (name == "cAttackStrongCharge") return Kind::ChargeHeavy;
    return Kind::None;
}
// ATTACK begins while the blade is still leaving its prepared pose. Runtime
// traces give very different hit times for the ten concrete motions, so use a
// per-motion wind-up instead of treating them like a controller-button timer.
constexpr uint32_t SwingLeadFrames(Kind kind) {
    return kind == Kind::Light ? 10560u : (kind == Kind::Heavy ? 15360u : 0u); // 220 / 320 ms fallback
}
constexpr uint32_t SwingLeadFrames(std::string_view name, Kind kind) {
    constexpr std::string_view Prefix = "app.PlayerOneHandedAction.";
    if (name.starts_with(Prefix)) name.remove_prefix(Prefix.size());
    if (name == "cAttackWeak1") return 10560u;             // 220 ms
    if (name == "cAttackWeak2") return 9600u;              // 200 ms
    if (name == "cAttackWeak3") return 16320u;             // 340 ms
    if (name == "cAttackWeakFromStrong1") return 9120u;    // 190 ms
    if (name == "cAttackWeakFromStrong2") return 6240u;    // 130 ms
    if (name == "cAttackStrong1") return 23040u;           // 480 ms
    if (name == "cAttackStrong2") return 14400u;           // 300 ms
    if (name == "cAttackStrong3") return 22080u;           // 460 ms
    if (name == "cAttackStrongFromWeak1") return 10560u;   // 220 ms
    if (name == "cAttackStrongFromWeak2") return 7200u;    // 150 ms
    return SwingLeadFrames(kind);
}
// The crossover actions each contain two visible automatic cuts inside one
// managed action instance.  Their second damage contacts were observed around
// 0.70-0.81 s, so schedule a second air arc before that contact instead of
// waiting for another action Start that the game never emits.
constexpr uint32_t SecondarySwingLeadFrames(std::string_view name) {
    constexpr std::string_view Prefix = "app.PlayerOneHandedAction.";
    if (name.starts_with(Prefix)) name.remove_prefix(Prefix.size());
    if (name == "cAttackWeakFromStrong1" || name == "cAttackWeakFromStrong2") return 28800u; // 600 ms
    if (name == "cAttackStrongFromWeak1") return 26880u; // 560 ms
    if (name == "cAttackStrongFromWeak2") return 29760u; // 620 ms
    return 0u;
}
constexpr uint32_t ActiveDuration(Kind kind) {
    return kind == Kind::Light ? 14400u : (kind == Kind::Heavy ? 19200u : 0u); // 300 / 400 ms arc
}
constexpr uint32_t Duration(Kind kind, uint32_t lead) {
    return ActiveDuration(kind) == 0 ? 0u : lead + ActiveDuration(kind);
}
constexpr uint32_t Duration(Kind kind) {
    return Duration(kind, SwingLeadFrames(kind));
}
constexpr uint32_t HitDuration(Kind kind) {
    if (kind == Kind::Light) return 13920u;
    if (kind == Kind::Heavy) return 18240u;
    if (kind == Kind::ChargeLight) return 16800u;
    if (kind == Kind::ChargeHeavy) return 21600u;
    return 0u;
}
struct StereoSample { float left{}, right{}; };
inline double Smooth(double x) { x = std::clamp(x, 0.0, 1.0); return x * x * (3.0 - 2.0 * x); }
inline StereoSample Sample(uint32_t frame, Kind kind, uint32_t lead) {
    const uint32_t duration = Duration(kind, lead);
    if (frame >= duration) return {};
    if (frame < lead || duration <= lead) return {};
    constexpr double TwoPi = 6.28318530717958647692;
    const bool heavy = kind == Kind::Heavy;
    const double time = static_cast<double>(frame - lead) / SampleRate;
    const double seconds = static_cast<double>(duration - lead) / SampleRate;
    const double progress = time / seconds;
    const double swell = 0.45 + 0.55 * std::pow(std::sin(3.141592653589793 * progress), 2.0);
    const double envelope = Smooth(time / (heavy ? 0.045 : 0.035)) * swell *
        Smooth((seconds - time) / (heavy ? 0.125 : 0.090));
    // Empty swings are a low-gain, soft high-mid sweep through air, not
    // an impact. No low-frequency mass component: cutting pressure belongs
    // only to the independently confirmed hit layer below.
    const double frequency = heavy ? 168.0 : 186.0;
    const double phase = TwoPi * (frequency * time + 52.0 * time * time / (2.0 * seconds));
    const double carrier = 0.82 * std::sin(phase) +
        0.18 * std::sin(TwoPi * ((heavy ? 238.0 : 262.0) * time +
            18.0 * time * time / (2.0 * seconds)) + 0.12);
    const float body = static_cast<float>((heavy ? 0.25 : 0.18) * envelope * carrier);
    const float lateral = static_cast<float>(0.77 + 0.18 * std::sin(3.141592653589793 * progress));
    return {body, body * lateral};
}
inline StereoSample Sample(uint32_t frame, Kind kind) {
    return Sample(frame, kind, SwingLeadFrames(kind));
}

inline StereoSample HitSample(uint32_t frame, Kind kind) {
    const uint32_t duration = HitDuration(kind);
    if (frame >= duration) return {};
    constexpr double TwoPi = 6.28318530717958647692;
    const bool heavy = IsHeavyKind(kind);
    const bool charged = IsChargeKind(kind);
    const double time = static_cast<double>(frame) / SampleRate;
    const double seconds = static_cast<double>(duration) / SampleRate;
    const double contact = Smooth(time / 0.009);
    const double pressure = std::exp(-std::max(time - (heavy ? 0.110 : 0.080), 0.0) /
        (heavy ? 0.082 : 0.060));
    const double fade = Smooth((seconds - time) / 0.060);
    const double body = std::sin(TwoPi * ((heavy ? 62.0 : 78.0) * time - 11.0 * time * time));
    const double drag = std::sin(TwoPi * ((heavy ? 136.0 : 162.0) * time + 9.0 * time * time));
    const double carrier = 0.70 * body + 0.30 * drag;
    const float value = static_cast<float>((charged ? (heavy ? 1.10 : 0.98) : (heavy ? 0.98 : 0.85)) *
        contact * pressure * fade *
        std::tanh(carrier * 1.55));
    return {value, value * (heavy ? 0.96f : 0.88f)};
}

constexpr uint32_t ChargeReleaseDuration(Kind kind) {
    return kind == Kind::ChargeLight ? 17760u :
        (kind == Kind::ChargeHeavy ? 23520u : 0u); // 370 / 490 ms
}
inline StereoSample ChargeHoldSample(uint32_t frame, Kind kind) {
    if (!IsChargeKind(kind)) return {};
    constexpr double TwoPi = 6.28318530717958647692;
    const bool heavy = IsHeavyKind(kind);
    const double time = static_cast<double>(frame) / SampleRate;
    const double build = Smooth(std::min(time / (heavy ? 0.82 : 0.62), 1.0));
    const double breath = 0.76 + 0.24 * std::sin(TwoPi * (1.55 * time + 0.11 * time * time));
    const double frequency = (heavy ? 54.0 : 63.0) + std::min(time, 1.2) * (heavy ? 15.0 : 19.0);
    const double phase = TwoPi * (frequency * time + (heavy ? 5.0 : 7.0) * time * time);
    const double texture = 0.72 * std::sin(phase) +
        0.28 * std::sin(TwoPi * ((heavy ? 116.0 : 132.0) * time) + 0.4);
    const float gain = static_cast<float>((heavy ? 0.36 : 0.29) * (0.30 + 0.70 * build) * breath);
    const float pan = static_cast<float>(0.10 * std::sin(TwoPi * 0.85 * time));
    return {gain * static_cast<float>(texture) * (1.0f + pan),
        gain * static_cast<float>(texture) * (1.0f - pan)};
}
inline StereoSample ChargeReleaseSample(uint32_t frame, Kind kind) {
    const uint32_t duration = ChargeReleaseDuration(kind);
    if (frame >= duration) return {};
    constexpr double Pi = 3.14159265358979323846;
    constexpr double TwoPi = Pi * 2.0;
    const bool heavy = IsHeavyKind(kind);
    const double time = static_cast<double>(frame) / SampleRate;
    const double seconds = static_cast<double>(duration) / SampleRate;
    const double progress = time / seconds;
    // A fast low-frequency break separates button release from the preceding
    // continuous charge. The air arc follows a few milliseconds later and
    // crosses the two actuators instead of feeling like more of the same hum.
    const double launch = Smooth(time / 0.004) * std::exp(-time / (heavy ? 0.095 : 0.072));
    const double arc = Smooth((time - 0.010) / 0.018) * std::pow(std::sin(Pi * progress), 1.10) *
        Smooth((seconds - time) / 0.085);
    const double low = std::sin(TwoPi * ((heavy ? 58.0 : 70.0) * time - 9.0 * time * time));
    const double air = std::sin(TwoPi * ((heavy ? 148.0 : 176.0) * time + 55.0 * time * time));
    const double leftSweep = 0.72 + 0.28 * Smooth((0.78 - progress) / 0.55);
    const double rightSweep = 0.72 + 0.28 * Smooth((progress - 0.05) / 0.55);
    const float left = static_cast<float>((heavy ? 0.94 : 0.78) * launch * low +
        (heavy ? 0.70 : 0.58) * arc * air * leftSweep);
    const float right = static_cast<float>((heavy ? 0.86 : 0.70) * launch * low +
        (heavy ? 0.78 : 0.65) * arc * air * rightSweep);
    return {left, right};
}

constexpr uint32_t OrdinaryLightWaveDuration(bool large) {
    return large ? 19200u : 10080u; // 400 / 210 ms
}
inline StereoSample OrdinaryLightWaveSample(uint32_t frame, bool large) {
    const uint32_t duration = OrdinaryLightWaveDuration(large);
    if (frame >= duration) return {};
    constexpr double TwoPi = 6.28318530717958647692;
    const double time = static_cast<double>(frame) / SampleRate;
    const double progress = static_cast<double>(frame) / duration;
    const double envelope = Smooth(time / (large ? 0.018 : 0.012)) *
        Smooth((1.0 - progress) / (large ? 0.27 : 0.32));
    const double rotation = std::sin(TwoPi * (large ? 3.4 : 4.2) * time);
    const double travel = Smooth(progress);
    const double pan = 0.20 + 0.46 * travel + 0.14 * rotation;
    const double air = std::sin(TwoPi * ((large ? 126.0 : 156.0) * time +
        (large ? 61.0 : 82.0) * time * time));
    const double body = std::sin(TwoPi * ((large ? 68.0 : 84.0) * time + 12.0 * time * time));
    const double wave = 0.70 * air + 0.30 * body;
    const double gain = (large ? 0.69 : 0.34) * envelope;
    return {static_cast<float>(gain * wave * (1.0 - pan)),
        static_cast<float>(gain * wave * (1.0 + pan))};
}

constexpr uint32_t OrdinaryHeavyDownstrokeDuration = 19200u; // 400 ms
constexpr uint32_t OrdinaryHeavyGroundCrackDuration = 31200u; // 650 ms
inline StereoSample OrdinaryHeavyDownstrokeSample(uint32_t frame) {
    if (frame >= OrdinaryHeavyDownstrokeDuration) return {};
    constexpr double TwoPi = 6.28318530717958647692;
    const double time = static_cast<double>(frame) / SampleRate;
    const double progress = static_cast<double>(frame) / OrdinaryHeavyDownstrokeDuration;
    const double attack = Smooth(time / 0.004);
    const double fall = Smooth((1.0 - progress) / 0.15);
    const double weight = 0.62 + 0.38 * Smooth(progress);
    const double air = std::sin(TwoPi * (172.0 * time - 58.0 * time * time));
    const double blade = std::sin(TwoPi * (95.0 * time - 19.0 * time * time));
    // The managed sweep event arrives roughly 0.44 s after the release.
    // Give the confirmed attack its first contact accent during the downward
    // motion, before the later sweep event extends it into visible fractures.
    const double contact = Smooth((time - 0.295) / 0.004) *
        std::exp(-std::max(time - 0.295, 0.0) / 0.035);
    const double thump = std::sin(TwoPi * (78.0 * time - 4.0 * time * time));
    const float value = static_cast<float>(0.78 * attack * fall * weight *
        (0.58 * air + 0.42 * blade) + 1.05 * contact * thump);
    return {value * static_cast<float>(1.08 - 0.15 * progress),
        value * static_cast<float>(0.85 + 0.23 * progress)};
}
inline StereoSample OrdinaryHeavyGroundCrackSample(uint32_t frame) {
    if (frame >= OrdinaryHeavyGroundCrackDuration) return {};
    constexpr double TwoPi = 6.28318530717958647692;
    const double time = static_cast<double>(frame) / SampleRate;
    const auto pulse = [time](double at, double decay) {
        const double elapsed = time - at;
        return elapsed < 0.0 ? 0.0 :
            Smooth(elapsed / 0.002) * std::exp(-elapsed / decay);
    };
    // Short separated fractures, alternating across the two actuators, feel
    // different from the earlier heavy contact. A continuous low rumble made
    // the first live version indistinct even though the event was delivered.
    const double opening = pulse(0.0, 0.038);
    const double leftSplit = pulse(0.065, 0.027);
    const double rightSplit = pulse(0.145, 0.030);
    const double wideSplit = pulse(0.235, 0.036);
    const double finalSplit = pulse(0.260, 0.135);
    const double low = std::sin(TwoPi * (80.0 * time - 4.0 * time * time));
    const double finalBody = 0.76 * std::sin(TwoPi * (76.0 * time - 5.0 * time * time)) +
        0.24 * std::sin(TwoPi * (113.0 * time - 3.0 * time * time) + 0.3);
    const double fracture = 0.68 * std::sin(TwoPi * (178.0 * time + 62.0 * time * time)) +
        0.32 * std::sin(TwoPi * (263.0 * time + 31.0 * time * time) + 0.7);
    return {
        static_cast<float>(0.64 * opening * low +
            (1.45 * leftSplit + 0.43 * rightSplit + 1.17 * wideSplit) * fracture +
            1.12 * finalSplit * finalBody),
        static_cast<float>(0.60 * opening * low +
            (0.40 * leftSplit + 1.46 * rightSplit + 1.14 * wideSplit) * fracture +
            1.08 * finalSplit * finalBody)
    };
}

// Shared with bridge/tests: only a fresh ordinary attack owned by the actual
// damage attacker can produce a hit. Repeated damage/audio callbacks coalesce.
class Tracker {
public:
    Operation Observe(uint64_t now, uintptr_t action, uintptr_t owner, Kind kind, bool attackPhase) {
        if (action == 0 || owner == 0 || kind == Kind::None) return Operation::NoEvent;
        if (!attackPhase) {
            if (action == m_action) { Clear(); return Operation::End; }
            return Operation::NoEvent;
        }
        const bool begin = action != m_action || kind != m_kind || !Fresh(now);
        if (begin) { Clear(); m_action = action; m_owner = owner; m_kind = kind; }
        m_seen = now;
        if (begin || now >= m_sent + 100u) {
            m_sent = now;
            return begin ? Operation::Start : Operation::Refresh;
        }
        return Operation::NoEvent;
    }
    Kind Finish(uintptr_t action) {
        if (action == 0 || action != m_action) return Kind::None;
        const auto kind = m_kind; Clear(); return kind;
    }
    Kind AcceptHit(uint64_t now, uintptr_t attacker, uintptr_t target, uint32_t uniqueId, bool damaged) {
        if (!Fresh(now) || attacker != m_owner || target == 0 || target == attacker || !damaged)
            return Kind::None;
        for (const auto& hit : m_hits) {
            if (hit.target == target && hit.id == uniqueId) return Kind::None;
        }
        m_hits[m_hitIndex++ % m_hits.size()] = {target, uniqueId};
        if (m_lastHit != 0 && now >= m_lastHit && now - m_lastHit < 90u) return Kind::None;
        m_lastHit = now;
        return m_kind;
    }
    bool Fresh(uint64_t now) const {
        return m_kind != Kind::None && now >= m_seen && now - m_seen < EvidenceTimeoutMs;
    }
    bool Expire(uint64_t now) {
        if (m_kind == Kind::None || now < m_seen || Fresh(now)) return false;
        Clear(); return true;
    }
    void Clear() { m_action = m_owner = 0; m_kind = Kind::None; m_seen = m_sent = m_lastHit = 0; m_hits = {}; m_hitIndex = 0; }
private:
    struct Hit { uintptr_t target{}; uint32_t id{}; };
    uintptr_t m_action{}, m_owner{};
    Kind m_kind{};
    uint64_t m_seen{}, m_sent{}, m_lastHit{};
    std::array<Hit, 16> m_hits{};
    uint32_t m_hitIndex{};
};

// Charge actions are waiting states rather than ordinary swing phases. Keep
// their ownership alive briefly after detailExit because the damage callback
// can arrive after the release action has already transitioned.
class ChargeTracker {
public:
    Operation Observe(uint64_t now, uintptr_t action, uintptr_t owner, Kind kind, bool oniActive,
        bool attackPhase = false) {
        if (action == 0 || owner == 0 || !IsChargeKind(kind)) return Operation::NoEvent;
        if (!oniActive) {
            if (action == m_action) { Clear(); return Operation::End; }
            return Operation::NoEvent;
        }
        const bool begin = action != m_action || kind != m_kind || (!m_released && !HoldingFresh(now));
        if (begin) { Clear(); m_action = action; m_owner = owner; m_kind = kind; }
        m_seen = now;
        if (attackPhase && !m_released) {
            m_released = true;
            return Operation::ChargeRelease;
        }
        if (m_released) return Operation::NoEvent;
        if (begin || now >= m_sent + 100u) {
            m_sent = now;
            return begin ? Operation::ChargeStart : Operation::ChargeRefresh;
        }
        return Operation::NoEvent;
    }
    Kind Release(uint64_t now, uintptr_t action) {
        if (action == 0 || action != m_action || !IsChargeKind(m_kind)) return Kind::None;
        if (m_released) return Kind::None;
        m_released = true;
        m_seen = now;
        return m_kind;
    }
    Kind AcceptHit(uint64_t now, uintptr_t attacker, uintptr_t target, uint32_t uniqueId, bool damaged) {
        if (!m_released || !ReleasedFresh(now) || attacker != m_owner || target == 0 ||
            target == attacker || !damaged) return Kind::None;
        for (const auto& hit : m_hits)
            if (hit.target == target && hit.id == uniqueId) return Kind::None;
        m_hits[m_hitIndex++ % m_hits.size()] = {target, uniqueId};
        if (m_lastHit != 0 && now >= m_lastHit && now - m_lastHit < 90u) return Kind::None;
        m_lastHit = now;
        return m_kind;
    }
    bool HoldingFresh(uint64_t now) const {
        return !m_released && IsChargeKind(m_kind) && now >= m_seen && now - m_seen < EvidenceTimeoutMs;
    }
    bool ReleasedFresh(uint64_t now) const {
        return m_released && IsChargeKind(m_kind) && now >= m_seen && now - m_seen < 700u;
    }
    void Clear() {
        m_action = m_owner = 0; m_kind = Kind::None; m_seen = m_sent = m_lastHit = 0;
        m_released = false; m_hits = {}; m_hitIndex = 0;
    }
private:
    struct Hit { uintptr_t target{}; uint32_t id{}; };
    uintptr_t m_action{}, m_owner{};
    Kind m_kind{};
    uint64_t m_seen{}, m_sent{}, m_lastHit{};
    bool m_released{};
    std::array<Hit, 16> m_hits{};
    uint32_t m_hitIndex{};
};

// This exact renderer is used by the helper and replayed by smoke tests.
// Refresh keeps the existing phase; hits add a thick cutting-contact layer.
class Renderer {
public:
    bool Enabled() const { return m_enabled; }
    void SetEnabled(bool enabled) { m_enabled = enabled; if (!enabled) Stop(); }
    void Stop() {
        m_voices = {}; m_hits = {}; m_hold = 0; m_rendered = 0;
        m_chargeKind = m_chargeReleaseKind = Kind::None;
        m_chargeFrame = m_chargeWatchdog = m_chargeReleaseFrame = m_chargeReleaseRemaining = 0;
        m_ordinaryLightSmallFrame = m_ordinaryLightSmallRemaining = 0;
        m_ordinaryLightLargeFrame = m_ordinaryLightLargeRemaining = 0;
        m_ordinaryHeavyDownstrokeFrame = m_ordinaryHeavyDownstrokeRemaining = 0;
        m_ordinaryHeavyGroundCrackFrame = m_ordinaryHeavyGroundCrackRemaining = 0;
    }
    uint8_t ConsumeRenderedFlags() { const auto flags = m_rendered; m_rendered = 0; return flags; }
    void Play(Kind kind, uint32_t lead = 0, uint32_t secondaryLead = 0) {
        if (!m_enabled || ActiveDuration(kind) == 0) return;
        if (lead == 0) lead = SwingLeadFrames(kind);
        StartVoice(m_voices, kind, Duration(kind, lead), lead, true);
        if (secondaryLead > lead)
            StartVoice(m_voices, kind, Duration(kind, secondaryLead), secondaryLead, false);
        m_hold = static_cast<uint32_t>(EvidenceTimeoutMs * SampleRate / 1000u);
    }
    void Refresh(Kind kind) {
        if (!m_enabled) return;
        for (const auto& voice : m_voices)
            if (voice.kind == kind && voice.remaining > 0 && voice.release == NoRelease)
                m_hold = static_cast<uint32_t>(EvidenceTimeoutMs * SampleRate / 1000u);
    }
    void End() {
        for (auto& voice : m_voices)
            if (voice.remaining > 0) voice.release = std::min(voice.release, ReleaseFrames);
        m_hold = 0;
    }
    void Hit(Kind kind) {
        if (m_enabled && HitDuration(kind) != 0) StartVoice(m_hits, kind, HitDuration(kind), 0u, true);
    }
    void StartCharge(Kind kind) {
        if (!m_enabled || !IsChargeKind(kind)) return;
        m_chargeKind = kind;
        m_chargeFrame = 0;
        m_chargeWatchdog = static_cast<uint32_t>(EvidenceTimeoutMs * SampleRate / 1000u);
        m_chargeReleaseKind = Kind::None;
        m_chargeReleaseFrame = m_chargeReleaseRemaining = 0;
    }
    void RefreshCharge(Kind kind) {
        if (!m_enabled || kind != m_chargeKind || !IsChargeKind(kind)) return;
        m_chargeWatchdog = static_cast<uint32_t>(EvidenceTimeoutMs * SampleRate / 1000u);
    }
    void ReleaseCharge(Kind kind) {
        if (!m_enabled || !IsChargeKind(kind)) return;
        m_chargeKind = Kind::None;
        m_chargeWatchdog = 0;
        m_chargeReleaseKind = kind;
        m_chargeReleaseFrame = 0;
        m_chargeReleaseRemaining = ChargeReleaseDuration(kind);
    }
    void CancelCharge() {
        m_chargeKind = m_chargeReleaseKind = Kind::None;
        m_chargeFrame = m_chargeWatchdog = m_chargeReleaseFrame = m_chargeReleaseRemaining = 0;
    }
    void PlayOrdinaryLightWave(bool large) {
        if (!m_enabled) return;
        if (large) {
            m_ordinaryLightLargeFrame = 0;
            m_ordinaryLightLargeRemaining = OrdinaryLightWaveDuration(true);
        } else {
            m_ordinaryLightSmallFrame = 0;
            m_ordinaryLightSmallRemaining = OrdinaryLightWaveDuration(false);
        }
    }
    void PlayOrdinaryHeavyDownstroke() {
        if (!m_enabled) return;
        m_ordinaryHeavyDownstrokeFrame = 0;
        m_ordinaryHeavyDownstrokeRemaining = OrdinaryHeavyDownstrokeDuration;
    }
    void PlayOrdinaryHeavyGroundCrack() {
        if (!m_enabled) return;
        m_ordinaryHeavyGroundCrackFrame = 0;
        m_ordinaryHeavyGroundCrackRemaining = OrdinaryHeavyGroundCrackDuration;
    }
    StereoSample Render() {
        if (m_hold > 0 && --m_hold == 0) End();
        StereoSample result{};
        Mix(m_voices, result, false, m_rendered);
        Mix(m_hits, result, true, m_rendered);
        if (IsChargeKind(m_chargeKind) && m_chargeWatchdog > 0) {
            const auto charge = ChargeHoldSample(m_chargeFrame++, m_chargeKind);
            result.left += charge.left;
            result.right += charge.right;
            --m_chargeWatchdog;
            if (m_chargeWatchdog == 0) m_chargeKind = Kind::None;
        }
        if (m_chargeReleaseRemaining > 0) {
            const auto release = ChargeReleaseSample(m_chargeReleaseFrame++, m_chargeReleaseKind);
            result.left += release.left;
            result.right += release.right;
            if (--m_chargeReleaseRemaining == 0) m_chargeReleaseKind = Kind::None;
        }
        if (m_ordinaryLightSmallRemaining > 0) {
            const auto wave = OrdinaryLightWaveSample(m_ordinaryLightSmallFrame++, false);
            result.left += wave.left;
            result.right += wave.right;
            --m_ordinaryLightSmallRemaining;
        }
        if (m_ordinaryLightLargeRemaining > 0) {
            const auto wave = OrdinaryLightWaveSample(m_ordinaryLightLargeFrame++, true);
            result.left += wave.left;
            result.right += wave.right;
            --m_ordinaryLightLargeRemaining;
        }
        if (m_ordinaryHeavyDownstrokeRemaining > 0) {
            const auto stroke = OrdinaryHeavyDownstrokeSample(m_ordinaryHeavyDownstrokeFrame++);
            result.left += stroke.left;
            result.right += stroke.right;
            --m_ordinaryHeavyDownstrokeRemaining;
        }
        if (m_ordinaryHeavyGroundCrackRemaining > 0) {
            const auto ground = OrdinaryHeavyGroundCrackSample(m_ordinaryHeavyGroundCrackFrame++);
            result.left += ground.left;
            result.right += ground.right;
            --m_ordinaryHeavyGroundCrackRemaining;
        }
        result.left = 0.98f * std::tanh(result.left / 0.98f);
        result.right = 0.98f * std::tanh(result.right / 0.98f);
        return result;
    }
private:
    struct Voice { Kind kind{}; uint32_t frame{}, remaining{}, release{}, lead{}; bool reported{}; };
    static constexpr uint32_t NoRelease = std::numeric_limits<uint32_t>::max();
    static void StartVoice(std::array<Voice, 2>& voices, Kind kind, uint32_t duration, uint32_t lead,
        bool releaseExisting) {
        auto* slot = &voices[0];
        if (releaseExisting) {
            for (auto& voice : voices)
                if (voice.remaining > 0) voice.release = std::min(voice.release, ReleaseFrames);
        }
        for (auto& voice : voices) {
            if (voice.remaining == 0) { slot = &voice; break; }
            if (voice.release < slot->release) slot = &voice;
        }
        *slot = {kind, 0u, duration, NoRelease, lead};
    }
    static void Mix(std::array<Voice, 2>& voices, StereoSample& result, bool hit, uint8_t& rendered) {
        for (auto& voice : voices) {
            if (voice.remaining == 0) continue;
            const auto sample = hit ? HitSample(voice.frame++, voice.kind) : Sample(voice.frame++, voice.kind, voice.lead);
            const float fade = voice.release <= ReleaseFrames ? static_cast<float>(Smooth(
                static_cast<double>(voice.release) / ReleaseFrames)) : 1.0f;
            result.left += sample.left * fade;
            result.right += sample.right * fade;
            if (!voice.reported && std::abs(sample.left * fade) > 0.001f) {
                voice.reported = true;
                rendered |= static_cast<uint8_t>(1u << ((hit ? 2u : 0u) + static_cast<unsigned>(voice.kind) - 1u));
            }
            --voice.remaining;
            if (voice.release <= ReleaseFrames && --voice.release == 0) voice.remaining = 0;
        }
    }
    std::array<Voice, 2> m_voices{};
    std::array<Voice, 2> m_hits{};
    uint32_t m_hold{};
    uint8_t m_rendered{};
    bool m_enabled = false;
    Kind m_chargeKind{}, m_chargeReleaseKind{};
    uint32_t m_chargeFrame{}, m_chargeWatchdog{}, m_chargeReleaseFrame{}, m_chargeReleaseRemaining{};
    uint32_t m_ordinaryLightSmallFrame{}, m_ordinaryLightSmallRemaining{};
    uint32_t m_ordinaryLightLargeFrame{}, m_ordinaryLightLargeRemaining{};
    uint32_t m_ordinaryHeavyDownstrokeFrame{}, m_ordinaryHeavyDownstrokeRemaining{};
    uint32_t m_ordinaryHeavyGroundCrackFrame{}, m_ordinaryHeavyGroundCrackRemaining{};
};
} // namespace normal_attack_haptics

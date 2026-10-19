#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string_view>

namespace wall_run_haptics {

constexpr uint8_t Message = 0xE8;
constexpr uint32_t SampleRate = 48000u;
constexpr uint32_t HoldFrames = 19200u;
constexpr uint64_t EvidenceTimeoutMs = 420u;
constexpr uint64_t RefreshIntervalMs = 150u;
enum class Event : uint8_t {
    Stop, Takeoff, Contact, Move, Leap, LandSmall, LandLarge, Step, Detach, AirMove, GroundLeap
};
enum class Side : uint8_t { Left, Right, Center };
enum class Phase : uint8_t { None, Takeoff, Contact, Move, Leap, LandSmall, LandLarge, Fall, GroundLeap };

// Concrete names and inheritance were read from this game's TypeDB. Do not
// match wall-crash damage, enemy actions, guards, or generic menu interactions.
constexpr Phase ClassifyAction(std::string_view name) {
    if (name == "app.PlayerBasicAction.cWallRunStartJump") return Phase::Takeoff;
    constexpr std::string_view Prefix = "app.PlayerOneHandedAction.";
    if (!name.starts_with(Prefix)) return Phase::None;
    name.remove_prefix(Prefix.size());
    if (name == "cWallRunStartJump") return Phase::Takeoff;
    if (name == "cWallRunStart") return Phase::Contact;
    if (name == "cWallRunLoop") return Phase::Move;
    if (name == "cWallRunJumpToWall") return Phase::Leap;
    if (name == "cWallRunEnd" || name == "cWallRunRouteEnd" ||
        name == "cWallRunJumpToGoal") return Phase::GroundLeap;
    if (name == "cWallRunJumpToGoalLandSmall" || name == "cWallRunRouteEndLandSmall" ||
        name == "cWallRunJumpToWallLand") return Phase::LandSmall;
    if (name == "cWallRunJumpToGoalLandLarge" || name == "cWallRunRouteEndLandLarge")
        return Phase::LandLarge;
    if (name == "cWallRunJumpToGoalFall" || name == "cWallRunJumpToWallFall" ||
        name == "cWallRunRouteEndFall") return Phase::Fall;
    return Phase::None;
}

constexpr bool AlongWall(Phase phase) {
    return phase == Phase::Contact || phase == Phase::Move;
}
constexpr bool SustainsFlight(Phase phase) {
    // Only approach/transfer flights sustain the enhanced glide feedback.
    // Departure to the ground and falling use the pre-glide behavior.
    return phase == Phase::Takeoff || phase == Phase::Leap;
}

// Verified enum ace.AceUtil.DIR2_LR: NONE=-1, L=0, R=1.
constexpr Side FromWallStyle(int32_t style) {
    return style == 0 ? Side::Left : (style == 1 ? Side::Right : Side::Center);
}
constexpr bool IsFootContact(int32_t step, int32_t foot) {
    return (step == 0 || step == 2) && foot >= 0 && foot <= 5;
}
constexpr bool EvidenceFresh(uint64_t now, uint64_t observed) {
    return observed != 0 && now >= observed && now - observed < EvidenceTimeoutMs;
}

// Holding movement can bypass cWallRun*Land and resume regular footsteps.
// Retain a bounded departure ticket, then use its first contact as a fallback.
// A brief arbitration window lets a real LandSmall/Large entry win unchanged.
class LandingTracker {
public:
    enum class ContactResult { Ignored, Queued, Pending };
    static constexpr uint64_t WindowMs = 2400u;
    static constexpr uint64_t PreferenceMs = 32u;
    static constexpr uint64_t DebounceMs = 240u;

    void Reset() { CancelPending(); m_lastLanding = m_fallbackDeparture = 0; }
    void ObserveDeparture(Phase phase, uint64_t now) {
        if (phase != Phase::GroundLeap && phase != Phase::Fall) {
            CancelPending();
            m_fallbackDeparture = 0;
            return;
        }
        if (phase == Phase::Fall && (Fresh(now) || FallbackFresh(now))) return;
        m_fallbackDeparture = 0;
        m_departure = now;
        m_minimumContactMs = phase == Phase::GroundLeap ? 250u : 60u;
        m_contact = 0;
    }
    bool AcceptExplicit(uint64_t now) {
        const bool alreadyLanded = FallbackFresh(now);
        CancelPending();
        if (alreadyLanded || (m_lastLanding != 0 && now >= m_lastLanding &&
            now - m_lastLanding < DebounceMs)) return false;
        m_lastLanding = now;
        return true;
    }
    ContactResult OfferContact(uint64_t now, int32_t step, int32_t foot) {
        if (!Fresh(now)) {
            if (now >= m_departure) CancelPending();
            return ContactResult::Ignored;
        }
        if (!IsFootContact(step, foot) || now - m_departure < m_minimumContactMs)
            return ContactResult::Ignored;
        if (m_contact != 0) return ContactResult::Pending;
        m_contact = now;
        return ContactResult::Queued;
    }
    bool PublishFallback(uint64_t now) {
        if (!Fresh(now)) {
            if (now >= m_departure) CancelPending();
            return false;
        }
        if (m_contact == 0 || now < m_contact || now - m_contact < PreferenceMs)
            return false;
        m_fallbackDeparture = m_departure;
        CancelPending();
        m_lastLanding = now;
        return true;
    }

private:
    bool Fresh(uint64_t now) const {
        return m_departure != 0 && now >= m_departure && now - m_departure < WindowMs;
    }
    bool FallbackFresh(uint64_t now) const {
        return m_fallbackDeparture != 0 && now >= m_fallbackDeparture &&
            now - m_fallbackDeparture < WindowMs;
    }
    void CancelPending() { m_departure = m_contact = 0; }
    uint64_t m_departure{}, m_contact{}, m_lastLanding{};
    uint64_t m_fallbackDeparture{};
    uint64_t m_minimumContactMs{};
};

constexpr uint32_t ImpulseFrames(Event event) {
    switch (event) {
    case Event::Takeoff: return 10080u;
    case Event::Contact: return 9600u;
    case Event::Leap: return 10560u;
    case Event::GroundLeap: return 6720u;
    case Event::LandSmall: return 9600u;
    case Event::LandLarge: return 13440u;
    case Event::Step: return 5280u;
    default: return 0;
    }
}

struct StereoSample { float left{}; float right{}; };
inline StereoSample Directional(float body, Side side, bool landing = false) {
    if (landing) return {body, body * 0.88f};
    if (side == Side::Left) return {body, body * 0.50f};
    if (side == Side::Right) return {body * 0.50f, body};
    return {body * 0.88f, body * 0.88f};
}

inline StereoSample TextureSample(uint32_t frame, Side side, float level) {
    constexpr double TwoPi = 6.28318530717958647692;
    const double time = static_cast<double>(frame) / SampleRate;
    const double traction = 0.84 + 0.11 * std::sin(TwoPi * 3.7 * time) +
        0.05 * std::sin(TwoPi * 8.1 * time);
    const double grain = 0.73 * std::sin(TwoPi * 82.0 * time) +
        0.17 * std::sin(TwoPi * 143.0 * time) +
        0.10 * std::sin(TwoPi * 207.0 * time);
    return Directional(static_cast<float>(0.20 * traction * grain) *
        std::clamp(level, 0.0f, 1.0f), side);
}

// A smooth, bilateral lift/glide body, separate from environmental wind and
// wall traction. Refreshes extend its lease without restarting its waveform.
inline StereoSample FlightSample(uint32_t frame, float level) {
    constexpr double TwoPi = 6.28318530717958647692;
    const double time = static_cast<double>(frame) / SampleRate;
    const double phase = TwoPi * (86.0 * time +
        12.0 * (time - 0.45 * (1.0 - std::exp(-time / 0.45))));
    const double carrier = 0.78 * std::sin(phase) +
        0.16 * std::sin(phase * 1.65) + 0.06 * std::sin(phase * 2.25);
    const double lift = 0.65 + 0.35 * std::exp(-time / 0.65);
    const double flow = 0.95 + 0.05 * std::sin(TwoPi * 1.2 * time);
    const float body = static_cast<float>(0.28 * lift * flow * carrier) *
        std::clamp(level, 0.0f, 1.0f);
    return {body, body * 0.86f};
}

inline StereoSample ImpulseSample(uint32_t frame, Event event, Side side, float intensity) {
    const uint32_t duration = ImpulseFrames(event);
    if (frame >= duration || intensity <= 0.0f) return {};
    constexpr double TwoPi = 6.28318530717958647692;
    const bool landing = event == Event::LandSmall || event == Event::LandLarge;
    const bool step = event == Event::Step;
    const bool contact = event == Event::Contact;
    const bool launch = event == Event::Takeoff || event == Event::Leap;
    const double time = static_cast<double>(frame) / SampleRate;
    const double attackX = std::min(time / 0.004, 1.0);
    const double attack = attackX * attackX * (3.0 - 2.0 * attackX);
    const double hold = landing ? 0.036 : (contact ? 0.042 : (launch ? 0.042 : 0.018));
    const double decayTime = landing ? 0.050 : (contact ? 0.045 : (launch ? 0.044 : 0.026));
    const double decay = std::exp(-std::max(time - hold, 0.0) / decayTime);
    const double remaining = static_cast<double>(duration - 1u - frame) / SampleRate;
    const double fade = std::clamp(remaining / 0.020, 0.0, 1.0);
    const double frequency = landing ? 72.0 : (step ? 84.0 : 76.0);
    const double carrier = 0.90 * std::sin(TwoPi * (frequency * time - 10.0 * time * time)) +
        0.10 * std::sin(TwoPi * frequency * 1.85 * time);
    const double amplitude = event == Event::LandLarge ? 0.90 :
        (landing ? 0.72 : (contact ? 0.82 : (step ? 0.52 :
            (event == Event::Takeoff ? 0.78 : (event == Event::Leap ? 0.76 : 0.48)))));
    const float body = static_cast<float>(amplitude * attack * decay * fade *
        std::tanh(carrier * 1.75 * std::clamp(intensity, 0.0f, 1.0f)));
    return Directional(body, side, landing);
}

// The helper uses this exact renderer; smoke tests replay its refresh, detach,
// pause/reset and timeout behavior rather than testing an unused waveform.
class Renderer {
public:
    void Stop() {
        m_target = m_level = 0.0f;
        m_hold = m_frame = 0;
        m_airTarget = m_airLevel = 0.0f;
        m_airHold = m_airFrame = 0;
        m_sinceContact = SampleRate;
        m_impulses = {};
    }

    void Apply(Event event, Side side, float intensity) {
        if (!std::isfinite(intensity)) return;
        intensity = std::clamp(intensity, 0.0f, 1.0f);
        if (event == Event::Stop) { Stop(); return; }
        if (event == Event::Detach) {
            m_target = 0.0f;
            m_hold = 0;
            m_airTarget = 0.0f;
            m_airHold = 0;
            return; // a phase exit must not erase a new leap/contact impulse
        }
        if (event == Event::Step) {
            if (m_target == 0.0f || m_sinceContact < 3840u) return;
        } else if (event != Event::Move && event != Event::AirMove) {
            m_impulses = {};
        }
        if (event == Event::Takeoff || event == Event::Leap || event == Event::AirMove) {
            if (event != Event::AirMove || m_airLevel == 0.0f) m_airFrame = 0;
            m_airTarget = intensity;
            m_airHold = HoldFrames;
        } else if (event != Event::Step) {
            m_airTarget = 0.0f;
            m_airHold = 0;
        }
        if (event == Event::Contact || event == Event::Move) {
            m_target = intensity;
            m_hold = HoldFrames;
            m_side = side;
            if (event == Event::Contact) {
                m_frame = 0;
                m_sinceContact = 0;
            }
        } else if (event != Event::Step) {
            m_target = 0.0f;
            m_hold = 0;
        }
        if (ImpulseFrames(event) == 0) return;
        auto* slot = &m_impulses[0];
        for (auto& candidate : m_impulses) {
            if (candidate.remaining == 0) { slot = &candidate; break; }
            if (candidate.remaining < slot->remaining) slot = &candidate;
        }
        *slot = {event, side, intensity, ImpulseFrames(event)};
    }

    StereoSample Render() {
        if (m_hold > 0) --m_hold;
        else m_target = 0.0f;
        m_level += (m_target - m_level) * (m_target > m_level ? 0.003f : 0.006f);
        if (m_target == 0.0f && m_level < 0.0001f) m_level = 0.0f;
        if (m_sinceContact < SampleRate) ++m_sinceContact;
        StereoSample result{};
        if (m_level > 0.0f) result = TextureSample(m_frame++, m_side, m_level);
        if (m_airHold > 0) --m_airHold;
        else m_airTarget = 0.0f;
        m_airLevel += (m_airTarget - m_airLevel) *
            (m_airTarget > m_airLevel ? 0.0025f : 0.012f);
        if (m_airTarget == 0.0f && m_airLevel < 0.0001f) m_airLevel = 0.0f;
        if (m_airLevel > 0.0f) {
            const auto flight = FlightSample(m_airFrame++, m_airLevel);
            result.left += flight.left;
            result.right += flight.right;
        }
        for (auto& impulse : m_impulses) {
            if (impulse.remaining == 0) continue;
            const auto sample = ImpulseSample(ImpulseFrames(impulse.event) -
                impulse.remaining, impulse.event, impulse.side, impulse.intensity);
            result.left += sample.left;
            result.right += sample.right;
            --impulse.remaining;
        }
        return result;
    }

private:
    struct Impulse {
        Event event{};
        Side side{Side::Center};
        float intensity{};
        uint32_t remaining{};
    };
    std::array<Impulse, 3> m_impulses{};
    float m_target{}, m_level{};
    uint32_t m_hold{}, m_frame{};
    float m_airTarget{}, m_airLevel{};
    uint32_t m_airHold{}, m_airFrame{};
    uint32_t m_sinceContact{SampleRate};
    Side m_side{Side::Center};
};

} // namespace wall_run_haptics

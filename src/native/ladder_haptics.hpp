#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string_view>

namespace ladder_haptics {

// These profiles travel through the existing continuous-player message. They
// are intentionally separate from the wall-run stream so a ladder transition
// can stop only its own texture and leave other authored feedback untouched.
constexpr uint8_t ClimbProfile = 5;
constexpr uint8_t FastSlideProfile = 6;
constexpr uint32_t SampleRate = 48000u;
constexpr uint32_t HoldFrames = 19200u; // 400 ms; bridge refreshes sooner.
constexpr uint64_t EvidenceTimeoutMs = 420u;
constexpr uint64_t RefreshIntervalMs = 150u;

enum class Profile : uint8_t {
    Climb = ClimbProfile,
    FastSlide = FastSlideProfile
};

enum class Operation : uint8_t {
    Stop = 0,
    Start = 1,
    Refresh = 2,
    Accent = 3,
    Decelerate = 4
};

enum class Side : uint8_t { Left, Right, Center };

enum class Phase : uint8_t {
    None,
    Start,
    Climb,
    FastUp,
    FastDown,
    End,
    FastStop,
    Idle,
    Fall,
    Land
};

constexpr bool IsPlayerAction(std::string_view name) {
    return name.starts_with("app.PlayerBasicAction.") ||
        name.starts_with("app.PlayerOneHandedAction.") ||
        name.starts_with("app.PlayerCommonAction.");
}

constexpr std::string_view ActionShortName(std::string_view name) {
    const size_t separator = name.find_last_of('.');
    return separator == std::string_view::npos
        ? name
        : name.substr(separator + 1);
}

constexpr Phase ClassifyAction(std::string_view name) {
    if (!IsPlayerAction(name)) return Phase::None;
    const auto action = ActionShortName(name);
    if (action == "cLadderClimbStart" ||
        action == "cLadderClimbUpStart" ||
        action == "cLadderClimbUpStartAir" ||
        action == "cLadderClimbDownStart" ||
        action == "cLadderClimbUpStartBase") return Phase::Start;
    if (action == "cLadderClimbUpLoopL" ||
        action == "cLadderClimbUpLoopR" ||
        action == "cLadderClimbDownLoopL" ||
        action == "cLadderClimbDownLoopR" ||
        action == "cLadderClimbLoop") return Phase::Climb;
    if (action == "cLadderClimbUpFastLoopL" ||
        action == "cLadderClimbUpFastLoopR" ||
        action == "cLadderClimbUpSuperFastLoopL" ||
        action == "cLadderClimbUpSuperFastLoopR") return Phase::FastUp;
    if (action == "cLadderClimbDownFastLoopL" ||
        action == "cLadderClimbDownFastLoopR") return Phase::FastDown;
    if (action == "cLadderClimbUpEndL" ||
        action == "cLadderClimbUpEndR" ||
        action == "cLadderClimbDownEndL" ||
        action == "cLadderClimbDownEndR" ||
        action == "cLadderClimbEnd") return Phase::End;
    if (action == "cLadderClimbDownFastEndL" ||
        action == "cLadderClimbDownFastEndR" ||
        action == "cLadderClimbDownFastStopL" ||
        action == "cLadderClimbDownFastStopR" ||
        action == "cLadderClimbFastStopL" ||
        action == "cLadderClimbFastStopR" ||
        action == "cLadderClimbFastStopBase") return Phase::FastStop;
    if (action == "cLadderIdle" || action == "cLadderIdleL" ||
        action == "cLadderIdleR") return Phase::Idle;
    if (action == "cLadderFallBase") return Phase::Fall;
    if (action == "cLadderFallLandBase") return Phase::Land;
    return Phase::None;
}

constexpr Side SideFromAction(std::string_view name) {
    const auto action = ActionShortName(name);
    if (action.ends_with('L')) return Side::Left;
    if (action.ends_with('R')) return Side::Right;
    return Side::Center;
}

constexpr bool Sustains(Phase phase) {
    return phase == Phase::Climb || phase == Phase::FastUp ||
        phase == Phase::FastDown;
}

constexpr bool IsFastDown(Phase phase) {
    return phase == Phase::FastDown;
}

constexpr bool IsProfile(uint8_t value) {
    return value == ClimbProfile || value == FastSlideProfile;
}

constexpr bool EvidenceFresh(uint64_t now, uint64_t observed) {
    return observed != 0 && now >= observed &&
        now - observed < EvidenceTimeoutMs;
}

struct StereoSample {
    float left{};
    float right{};
};

inline StereoSample Directional(float body, Side side) {
    if (side == Side::Left) return {body, body * 0.56f};
    if (side == Side::Right) return {body * 0.56f, body};
    return {body * 0.90f, body * 0.90f};
}

inline StereoSample BedSample(
    uint32_t frame,
    Profile profile,
    Side side,
    float level) {
    constexpr double TwoPi = 6.28318530717958647692;
    const double time = static_cast<double>(frame) / SampleRate;
    const bool fast = profile == Profile::FastSlide;
    const double carrierFrequency = fast ? 54.0 : 68.0;
    const double phase = TwoPi * (carrierFrequency * time +
        (fast ? 5.0 : 8.0) * time * time);
    const double carrier = 0.76 * std::sin(phase) +
        0.16 * std::sin(phase * (fast ? 1.61 : 1.73)) +
        0.08 * std::sin(phase * (fast ? 2.17 : 2.31));
    const double texture = fast
        ? 0.16 * std::sin(TwoPi * 117.0 * time) +
            0.08 * std::sin(TwoPi * 181.0 * time)
        : 0.11 * std::sin(TwoPi * 104.0 * time) +
            0.06 * std::sin(TwoPi * 157.0 * time);
    const double modulation = fast
        ? 0.86 + 0.14 * std::sin(TwoPi * 2.2 * time)
        : 0.73 + 0.27 * std::sin(TwoPi * 2.65 * time);
    // Ordinary climbing already receives a separate grip contact on every
    // hand change. Keep its bed understated so the two layers do not add up
    // to a heavy motor-like rumble. Fast downward sliding remains distinct.
    const float amplitude = fast ? 0.285f : 0.155f;
    const float body = static_cast<float>(amplitude * modulation *
        (carrier + texture) * std::clamp(level, 0.0f, 1.0f));
    return Directional(body, side);
}

inline uint32_t AccentFrames(Profile profile) {
    return static_cast<uint32_t>((profile == Profile::FastSlide ? 0.105 : 0.085) *
        SampleRate);
}

inline StereoSample AccentSample(
    uint32_t frame,
    Profile profile,
    Side side,
    float intensity) {
    const uint32_t duration = AccentFrames(profile);
    if (frame >= duration || intensity <= 0.0f) return {};
    constexpr double TwoPi = 6.28318530717958647692;
    const double time = static_cast<double>(frame) / SampleRate;
    const double attackX = std::min(time / 0.004, 1.0);
    const double attack = attackX * attackX * (3.0 - 2.0 * attackX);
    const double decay = std::exp(-time /
        (profile == Profile::FastSlide ? 0.042 : 0.034));
    const double frequency = profile == Profile::FastSlide ? 60.0 : 82.0;
    const double carrier = 0.90 * std::sin(TwoPi * frequency * time) +
        0.10 * std::sin(TwoPi * frequency * 1.85 * time);
    const float amplitude = profile == Profile::FastSlide ? 0.56f : 0.32f;
    const float body = static_cast<float>(amplitude * attack * decay *
        std::tanh(carrier * 1.65) * std::clamp(intensity, 0.0f, 1.0f));
    return Directional(body, side);
}

inline StereoSample BrakeSample(
    uint32_t frame,
    Profile profile,
    float intensity) {
    constexpr double TwoPi = 6.28318530717958647692;
    constexpr uint32_t Duration = 8640u; // 180 ms
    if (frame >= Duration || intensity <= 0.0f) return {};
    const double time = static_cast<double>(frame) / SampleRate;
    const double attackX = std::min(time / 0.008, 1.0);
    const double attack = attackX * attackX * (3.0 - 2.0 * attackX);
    const double decay = std::exp(-time / 0.072);
    const double frequency = profile == Profile::FastSlide ? 47.0 : 58.0;
    const double carrier = std::sin(TwoPi * frequency * time) * 0.88 +
        std::sin(TwoPi * frequency * 1.52 * time) * 0.12;
    const float amplitude = profile == Profile::FastSlide ? 0.46f : 0.26f;
    const float body = static_cast<float>(amplitude * attack * decay *
        std::tanh(carrier * 1.55) * std::clamp(intensity, 0.0f, 1.0f));
    return {body, body * 0.92f};
}

class Renderer {
public:
    void Stop() {
        m_target = m_level = 0.0f;
        m_hold = m_frame = 0;
        m_side = Side::Center;
        m_impulses = {};
    }

    void Apply(Profile profile, Operation operation, Side side, float intensity) {
        if (!std::isfinite(intensity)) return;
        intensity = std::clamp(intensity, 0.0f, 1.0f);
        if (operation == Operation::Stop) {
            Stop();
            return;
        }

        m_profile = profile;
        m_side = side;
        switch (operation) {
        case Operation::Start:
            m_frame = 0;
            m_target = intensity;
            m_hold = HoldFrames;
            break;
        case Operation::Refresh:
            m_target = intensity;
            m_hold = HoldFrames;
            break;
        case Operation::Accent:
            QueueImpulse(false, intensity);
            break;
        case Operation::Decelerate:
            m_target = 0.0f;
            m_hold = 0;
            QueueImpulse(true, intensity);
            break;
        default:
            break;
        }
    }

    StereoSample Render() {
        if (m_hold > 0) --m_hold;
        else m_target = 0.0f;
        const float smoothing = m_target > m_level ? 0.0045f : 0.014f;
        m_level += (m_target - m_level) * smoothing;
        if (m_target == 0.0f && m_level < 0.0001f) m_level = 0.0f;

        StereoSample result{};
        if (m_level > 0.0f) {
            result = BedSample(m_frame++, m_profile, m_side, m_level);
        }
        for (auto& impulse : m_impulses) {
            if (impulse.remaining == 0) continue;
            const uint32_t elapsed = impulse.total - impulse.remaining;
            const StereoSample sample = impulse.brake
                ? BrakeSample(elapsed, m_profile, impulse.intensity)
                : AccentSample(elapsed, m_profile, impulse.side, impulse.intensity);
            result.left += sample.left;
            result.right += sample.right;
            --impulse.remaining;
        }
        return result;
    }

private:
    struct Impulse {
        bool brake{};
        Side side{Side::Center};
        float intensity{};
        uint32_t total{};
        uint32_t remaining{};
    };

    void QueueImpulse(bool brake, float intensity) {
        const uint32_t frames = brake ? 8640u : AccentFrames(m_profile);
        auto* slot = &m_impulses[0];
        for (auto& candidate : m_impulses) {
            if (candidate.remaining == 0) {
                slot = &candidate;
                break;
            }
            if (candidate.remaining < slot->remaining) slot = &candidate;
        }
        *slot = {brake, m_side, intensity, frames, frames};
    }

    std::array<Impulse, 3> m_impulses{};
    Profile m_profile{Profile::Climb};
    Side m_side{Side::Center};
    float m_target{}, m_level{};
    uint32_t m_hold{}, m_frame{};
};

} // namespace ladder_haptics

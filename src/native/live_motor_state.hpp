#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <unordered_map>

// Only the generic, live PC motor mirror is governed here. Authored clips,
// timed motor pulses, footsteps, menu cues and continuous player textures
// retain their existing renderers and envelopes, including very weak samples.
namespace live_motor_state {
constexpr uint64_t NativeLeaseMs = 2000;
constexpr uint64_t HeartbeatMs = 250;
constexpr uint64_t ReceiverLeaseMs = 3000;
constexpr const char* Revision = "motor-state-r1";
enum class Kind { None, NativePad, XInput };
inline const char* Name(Kind kind) {
    return kind == Kind::XInput ? "xinput" :
        kind == Kind::NativePad ? "native-pad" : "none";
}
inline float Sanitize(float power) {
    return std::isfinite(power) ? std::clamp(power, 0.0f, 1.0f) : 0.0f;
}
inline bool ShouldSend(float previous, float power, bool heartbeat) {
    // A small delta must NEVER suppress a real stop or the first nonzero value.
    return heartbeat || previous < 0.0f || (previous == 0.0f) != (power == 0.0f) ||
        std::fabs(power - previous) >= 0.002f;
}
struct Snapshot {
    std::array<float, 4> power{};
    std::array<uint64_t, 4> tick{};
    std::array<bool, 4> observed{};
};
struct Mix {
    std::array<float, 4> power{};
    std::array<uintptr_t, 4> source{};
    std::array<Kind, 4> motorKind{};
    Kind kind = Kind::None;
};
class Sources {
public:
    void Native(uintptr_t device, size_t motor, float power, uint64_t now) {
        if (motor >= 4) return;
        if (m_native.size() >= 16 && !m_native.contains(device)) {
            ExpireNative(now);
            // Bound the table without clearing healthy sources wholesale.
            if (m_native.size() >= 16) return;
        }
        auto& value = m_native[device];
        value.power[motor] = Sanitize(power);
        value.tick[motor] = now;
        value.observed[motor] = true;
    }
    void XInput(uint32_t user, float left, float right, bool success) {
        if (user >= m_xinput.size()) return;
        if (m_connected[user] && !success) m_native.clear();
        m_connected[user] = success;
        m_xinput[user] = success ?
            std::array<float, 4>{Sanitize(left), Sanitize(right), 0.0f, 0.0f} :
            std::array<float, 4>{};
    }
    bool HasXInput(uint32_t user) const {
        return user < m_connected.size() && m_connected[user];
    }
    void RemoveXInput(uint32_t user) { XInput(user, 0.0f, 0.0f, false); }
    size_t ExpireNative(uint64_t now) {
        size_t expired = 0;
        for (auto entry = m_native.begin(); entry != m_native.end();) {
            bool alive = false;
            for (size_t motor = 0; motor < 4; ++motor) {
                auto& value = entry->second;
                if (!value.observed[motor]) continue;
                if (now - value.tick[motor] >= NativeLeaseMs) {
                    if (value.power[motor] > 0.0f) ++expired;
                    value.observed[motor] = false;
                    value.power[motor] = 0.0f;
                } else {
                    alive = true;
                }
            }
            if (!alive) entry = m_native.erase(entry); else ++entry;
        }
        return expired;
    }
    Mix Resolve() const {
        Mix mix{};
        // A SUCCESSFUL XInput write is the final game/Steam Input rumble.
        // A connected stream's zero is authoritative too: raw GamePadDevice
        // snapshots are a mirror, not another independent effect to max in.
        for (uint32_t user = 0; user < m_connected.size(); ++user) {
            if (!m_connected[user]) continue;
            mix.kind = Kind::XInput;
            for (size_t motor = 0; motor < 2; ++motor) {
                mix.motorKind[motor] = Kind::XInput;
                if (m_xinput[user][motor] > mix.power[motor]) {
                    mix.power[motor] = m_xinput[user][motor];
                    mix.source[motor] = user;
                }
            }
        }
        const bool canonical = mix.kind == Kind::XInput;
        if (!canonical && !m_native.empty()) mix.kind = Kind::NativePad;
        for (const auto& [device, value] : m_native) {
            // XInput has only two body motors. Retain independent native
            // trigger-motor channels rather than silently discarding them.
            for (size_t motor = canonical ? 2u : 0u; motor < 4; ++motor) {
                if (value.power[motor] > mix.power[motor]) {
                    mix.power[motor] = value.power[motor];
                    mix.source[motor] = device;
                    mix.motorKind[motor] = Kind::NativePad;
                }
            }
        }
        return mix;
    }
    size_t NativeCount() const { return m_native.size(); }
    void Clear() { m_native.clear(); m_connected.fill(false); m_xinput = {}; }
private:
    std::unordered_map<uintptr_t, Snapshot> m_native;
    std::array<std::array<float, 4>, 4> m_xinput{};
    std::array<bool, 4> m_connected{};
};

// Bridge heartbeats renew only this live target, not timed/authored effects.
// If the sender stalls or disappears, fade to zero via the existing mixer.
class TargetLease {
public:
    void Set(size_t motor, float power, uint64_t now) {
        if (motor >= 4) return;
        m_power[motor] = Sanitize(power);
        m_tick[motor] = now;
    }
    uint32_t Expire(uint64_t now) {
        uint32_t mask = 0;
        for (size_t motor = 0; motor < 4; ++motor) {
            if (m_power[motor] > 0.0f && now - m_tick[motor] >= ReceiverLeaseMs) {
                m_power[motor] = 0.0f;
                mask |= 1u << motor;
            }
        }
        return mask;
    }
    const std::array<float, 4>& Power() const { return m_power; }
    void Clear() { m_power.fill(0.0f); m_tick.fill(0); }
private:
    std::array<float, 4> m_power{};
    std::array<uint64_t, 4> m_tick{};
};
} // namespace live_motor_state

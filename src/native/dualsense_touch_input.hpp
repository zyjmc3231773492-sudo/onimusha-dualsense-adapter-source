#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace dualsense_touch_input {

constexpr uint32_t Left = 1;
constexpr uint32_t Right = 2;
constexpr uint16_t Width = 1920;
constexpr uint32_t FreshnessMs = 150;

struct SharedState {
    alignas(8) volatile long long packed = 0;
};

inline std::wstring MappingName(uint32_t gamePid) {
    return L"Local\\CodexOnimushaTouch_" + std::to_wstring(gamePid);
}

inline uint64_t Pack(uint32_t tick, uint32_t flags) {
    return (static_cast<uint64_t>(tick) << 32) | flags;
}

inline uint32_t FreshFlags(uint64_t packed, uint32_t now) {
    const uint32_t tick = static_cast<uint32_t>(packed >> 32);
    if (tick == 0 || now - tick > FreshnessMs) return 0;
    return static_cast<uint32_t>(packed) & (Left | Right);
}

struct Contact {
    bool active = false;
    uint8_t id = 0;
    uint16_t x = 0;
};

struct Contacts {
    bool valid = false;
    Contact points[2]{};
};

inline Contacts Parse(const uint8_t* data, size_t length) {
    Contacts result{};
    if (data == nullptr || length == 0) return result;
    const size_t base = data[0] == 0x01 && length >= 64 ? 1 :
        (data[0] == 0x31 && length >= 78 ? 2 : 0);
    if (base == 0 || length < base + 40) return result;
    // The two four-byte contacts begin 32 bytes into the common report.
    result.valid = true;
    for (size_t i = 0; i < 2; ++i) {
        const uint8_t* point = data + base + 32 + 4 * i;
        result.points[i].active = (point[0] & 0x80) == 0;
        result.points[i].id = point[0] & 0x7f;
        result.points[i].x = static_cast<uint16_t>(
            point[1] | ((point[2] & 0x0f) << 8));
        if (result.points[i].x >= Width) result.points[i].active = false;
    }
    return result;
}

// One finger owns its starting half until lifted. A second finger mutes the
// gesture, including after it lifts, to avoid an accidental L3/R3 chord.
class Gesture {
public:
    uint32_t Update(const Contacts& contacts) {
        if (!contacts.valid) return Reset();
        const int count = static_cast<int>(contacts.points[0].active) +
            static_cast<int>(contacts.points[1].active);
        if (count == 0) return Reset();
        if (count == 2) { blocked_ = true; return 0; }
        if (blocked_) return 0;
        const Contact& contact = contacts.points[contacts.points[0].active ? 0 : 1];
        if (!held_ || contact.id != id_) {
            held_ = true;
            id_ = contact.id;
            zone_ = contact.x < Width / 2 ? Left : Right;
        }
        return zone_;
    }

    uint32_t Reset() {
        held_ = false;
        blocked_ = false;
        zone_ = 0;
        return 0;
    }

private:
    bool held_ = false;
    bool blocked_ = false;
    uint8_t id_ = 0;
    uint32_t zone_ = 0;
};

} // namespace dualsense_touch_input

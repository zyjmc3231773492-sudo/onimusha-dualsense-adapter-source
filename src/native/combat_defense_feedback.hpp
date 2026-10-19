#pragma once

#include <cstdint>
#include <string_view>

namespace combat_defense_feedback {

constexpr uint32_t NativeParry = 1072545536u;
constexpr uint32_t NativeBlock = 1372241152u;

enum class Kind { None, Deflect, MeleeRepel, ProjectileRepel };

// Shared speaker-message byte 1. Cue 6 remains the same volume category;
// The native trigger selects the original recording; cinematic evidence
// selects its duration. The same PARRY trigger also occurs in normal combat.
enum class DeflectSpeakerVariant : uint8_t {
    Short = 0, Slow = 1, ContactShort = 2
};

// Mirrors the native enemy isGrappleParryLarge state query: category 2
// selects the currently resolved EnemyAction argument, not the BTable cache.
constexpr bool IsCinematicParryArgument(
    bool enemyPartner, uint32_t actionCategory, bool large) {
    return enemyPartner && actionCategory == 2 && large;
}

constexpr DeflectSpeakerVariant ClassifyDeflectSpeakerVariant(
    uint32_t trigger, uint32_t endTrigger, bool cinematicParry = false) {
    if (trigger == 1992708532u && endTrigger == 3387009413u) {
        return cinematicParry ? DeflectSpeakerVariant::Slow
                              : DeflectSpeakerVariant::ContactShort;
    }
    return DeflectSpeakerVariant::Short;
}

constexpr DeflectSpeakerVariant DecodeDeflectSpeakerVariant(uint8_t value) {
    return value == static_cast<uint8_t>(DeflectSpeakerVariant::Slow)
        ? DeflectSpeakerVariant::Slow
        : value == static_cast<uint8_t>(DeflectSpeakerVariant::ContactShort)
        ? DeflectSpeakerVariant::ContactShort : DeflectSpeakerVariant::Short;
}

constexpr bool UsesContactRecording(DeflectSpeakerVariant variant) {
    return variant == DeflectSpeakerVariant::Slow ||
        variant == DeflectSpeakerVariant::ContactShort;
}

constexpr bool CanPromoteToCinematic(DeflectSpeakerVariant current,
                                    DeflectSpeakerVariant requested) {
    return current == DeflectSpeakerVariant::ContactShort &&
        requested == DeflectSpeakerVariant::Slow;
}

// setGrapple(PARRY) is a sustained state callback, not one new impact per call.
// In slow motion the same state can report every frame for several seconds.
// Refresh the quiet-window timestamp on EVERY observation, including rejected
// duplicates: a cooldown measured from the last emitted cue would loop forever.
class DeflectEpisodeGate {
public:
    static constexpr uint64_t QuietWindowMs = 300;

    bool Observe(uint64_t now) {
        if (m_observed && now < m_lastObservation) return false;
        if (!m_observed || now - m_lastObservation > QuietWindowMs) {
            m_emitted = false;
        }
        m_observed = true;
        m_lastObservation = now;
        if (m_emitted) return false;
        m_emitted = true;
        m_lastEmission = now;
        return true;
    }

    // A separately accepted defensive result re-arms the next impact even if
    // it follows the previous one before the quiet window has elapsed. Do not
    // infer a fresh result from button input or a repeated action/state callback.
    bool ConfirmSuccess(uint64_t now, uint64_t callStarted, uint64_t resultId) {
        if (resultId == 0 || resultId <= m_lastConfirmedResult ||
            callStarted > now || (m_observed && now < m_lastObservation)) {
            return false;
        }
        // A native sound can resolve inside tryJustGuard's original call,
        // before its post-hook. In that order it already belongs to THIS result;
        // re-arming it here would cause another cue on the following frame.
        const bool emittedInsideCall = m_emitted &&
            m_lastEmission >= callStarted && m_lastEmission <= now;
        m_emitted = emittedInsideCall;
        m_observed = true;
        m_lastObservation = now;
        m_lastConfirmedResult = resultId;
        return true;
    }

private:
    bool m_observed = false;
    bool m_emitted = false;
    uint64_t m_lastObservation = 0;
    uint64_t m_lastEmission = 0;
    uint64_t m_lastConfirmedResult = 0;
};

constexpr Kind ClassifyNativeGrapple(uint32_t type, bool player, bool paused,
                                   uint32_t trigger) {
    if (!player || paused || trigger == 0 || trigger == 0xFFFFFFFFu) return Kind::None;
    if (type == NativeParry) return Kind::Deflect;
    if (type == NativeBlock) return Kind::MeleeRepel;
    return Kind::None;
}

constexpr Kind ClassifyNativeShell(bool player, bool paused, uint32_t trigger,
                                 bool recentRepelInput) {
    return player && !paused && recentRepelInput && trigger != 0 &&
        trigger != 0xFFFFFFFFu ? Kind::ProjectileRepel : Kind::None;
}

constexpr bool RequiresNativeParrySound(std::string_view action) {
    return action.starts_with("app.Player") &&
        (action.ends_with(".cParryBlow") || action.ends_with(".cParryDefend") ||
         action.ends_with(".cParryShell"));
}

constexpr uint8_t HapticProfile(Kind kind) {
    switch (kind) {
    case Kind::Deflect: return 4;
    case Kind::MeleeRepel: return 2;
    case Kind::ProjectileRepel: return 5;
    default: return 0;
    }
}

// Only the two actual Repel profiles pair a speaker message automatically.
// Deflect must retain its own separate original-audio cue, never cue 12/13.
constexpr uint8_t PairedRepelSpeaker(uint8_t hapticProfile) {
    return hapticProfile == 2 ? 12 : hapticProfile == 5 ? 13 : 0;
}

constexpr bool RepelImpactWindowActive(uint64_t now, uint64_t until) {
    return until != 0 && now <= until;
}

constexpr bool ShouldSuppressStandardRumble(
    uint64_t now,
    uint64_t until,
    uint16_t left,
    uint16_t right) {
    return RepelImpactWindowActive(now, until) && (left != 0 || right != 0);
}

}  // namespace combat_defense_feedback

#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string_view>

namespace execution_haptics {
constexpr uint32_t SampleRate = 48000u;
enum class Kind : uint8_t {
    None,
    Thrust,
    Slash,
    ArmCut,
    Downed,
    Const,
    BreakIssen,
    BlockIssen
};
struct StereoSample { float left{}, right{}; };
struct MotionSignature {
    uint64_t groupId{};
    uint32_t setId{};
    uint8_t layer{};
    uint8_t leftRight{};
    uint8_t connectType{};
};

// cFatalBlowSingle is an outer action shared by several concrete animation
// rows. Keep a stable, bounded variation key from the selected motion row so
// repeated use of the same execution remains consistent while genuinely
// different rows no longer collapse to an identical waveform.
constexpr uint8_t MotionVariant(MotionSignature signature) {
    if (signature.groupId == 0u && signature.setId == 0u) return 0u;
    uint64_t hash = signature.groupId ^
        (static_cast<uint64_t>(signature.setId) << 32u) ^
        (static_cast<uint64_t>(signature.layer) << 19u) ^
        (static_cast<uint64_t>(signature.leftRight) << 11u) ^
        (static_cast<uint64_t>(signature.connectType) << 3u);
    hash ^= hash >> 33u;
    hash *= 0xff51afd7ed558ccdULL;
    hash ^= hash >> 33u;
    return static_cast<uint8_t>(hash & 0x03u);
}

// Adjust/QuickMove/Start states only position the actors. Const is also used as
// the final observable player-side state by some execution rows, so it is a
// required fallback when no concrete Single/Slash/ArmCut/DownAttack entry
// follows. Break/Block Issen are separate execution chains used after Repel;
// classify only their confirmed contact/hold stages, never the similarly named
// gimmick, adjustment, run-up or weapon-off states.
constexpr Kind Classify(std::string_view name) {
    constexpr std::string_view OneHanded = "app.PlayerOneHandedAction.";
    constexpr std::string_view TwoHanded = "app.PlayerTwoHandedAction.";
    if (name.starts_with(OneHanded)) {
        name.remove_prefix(OneHanded.size());
        if (name == "cFatalBlowForDownAttack") return Kind::Downed;
        if (name == "cFatalBlowHoraizontalSlash" ||
            name == "cFatalBlowDiagonalSlash") return Kind::Slash;
        if (name == "cFatalBlowArmSlash") return Kind::ArmCut;
        if (name == "cFatalBlowSingle") return Kind::Thrust;
        if (name == "cFatalBlowConst") return Kind::Const;
        if (name == "cBreakIssen" ||
            name == "cBreakIssenQuick" ||
            name == "cBreakIssenOniChange" ||
            name == "cBreakIssenOniChangeQuick") return Kind::BreakIssen;
        if (name == "cBreakIssenLongConst" ||
            name == "cBlockIssenConst" ||
            name == "cBlockIssenMultiple") return Kind::BlockIssen;
        return Kind::None;
    }
    if (name.starts_with(TwoHanded)) {
        name.remove_prefix(TwoHanded.size());
        if (name == "cBreakIssenComboStart" ||
            name == "cBreakIssenComboNextTurn") return Kind::BreakIssen;
        if (name == "cBreakIssenComboConst" ||
            name == "cBreakIssenComboGenericConst") return Kind::BlockIssen;
    }
    return Kind::None;
}

constexpr uint8_t ProfileValue(Kind kind) {
    return kind == Kind::None ? 0u : static_cast<uint8_t>(6u + static_cast<uint8_t>(kind));
}
constexpr Kind KindFromProfile(uint8_t profile) {
    return profile >= 7u && profile <= 13u
        ? static_cast<Kind>(profile - 6u) : Kind::None;
}
constexpr uint32_t Duration(Kind kind) {
    switch (kind) {
    case Kind::Thrust: return 74400u; // 1.55 s
    case Kind::Slash: return 68160u;  // 1.42 s
    case Kind::ArmCut: return 75840u; // 1.58 s
    case Kind::Downed: return 82560u; // 1.72 s
    case Kind::Const: return 61440u;   // 1.28 s
    case Kind::BreakIssen: return 50400u; // 1.05 s
    case Kind::BlockIssen: return 60000u; // 1.25 s
    default: return 0u;
    }
}
constexpr uint32_t SuppressionMs(Kind kind) {
    return Duration(kind) == 0u ? 0u : Duration(kind) / 48u + 120u;
}
inline double Smooth(double x) {
    x = std::clamp(x, 0.0, 1.0);
    return x * x * (3.0 - 2.0 * x);
}
inline double Bell(double time, double center, double sharpness) {
    const double delta = time - center;
    return std::exp(-sharpness * delta * delta);
}
inline double Between(double time, double begin, double end, double edge) {
    return Smooth((time - begin) / edge) * Smooth((end - time) / edge);
}

// Timings are anchored to observed cFatalBlowSingle traffic: first game motor
// contact around +0.51 s and the finishing contact around +1.29 s. Each kind
// retains that animation rhythm but has a different direction and texture.
inline StereoSample Sample(
    uint32_t frame,
    Kind kind,
    float intensity = 1.0f,
    uint8_t motionVariant = 0u) {
    const uint32_t duration = Duration(kind);
    if (frame >= duration || kind == Kind::None) return {};
    constexpr double TwoPi = 6.28318530717958647692;
    const double time = static_cast<double>(frame) / SampleRate;
    const double seconds = static_cast<double>(duration) / SampleRate;
    const double endFade = Smooth((seconds - time) / 0.085);
    const double variant = static_cast<double>(motionVariant & 0x03u);
    // Do not move motion variants across actuator frequency bands. A small
    // numerical frequency change can feel disproportionately strong or weak on
    // DualSense even when RMS is identical. Variants differ by timing, pan and
    // harmonic phase instead, while sharing one calibrated carrier set.
    constexpr double frequencyScale = 1.0;
    const double timingShift = (variant - 1.5) * 0.006;
    const double texturePhase = variant * 0.41;
    double left = 0.0, right = 0.0;

    switch (kind) {
    case Kind::Thrust: {
        const double ready = Between(time, 0.15, 0.50, 0.095);
        const double plunge = Bell(time, 0.520 + timingShift, 205.0);
        const double pressure = Between(time, 0.525, 0.96, 0.060) *
            std::exp(-1.18 * std::max(0.0, time - 0.525));
        const double finish = Bell(time, 1.268 + timingShift, 190.0);
        const double wave =
            0.70 * std::sin(TwoPi * frequencyScale * (56.0 * time - 2.8 * time * time)) +
            0.22 * std::sin(TwoPi * frequencyScale * 103.0 * time) +
            0.08 * std::sin(TwoPi * frequencyScale * 174.0 * time);
        const double grain = std::sin(TwoPi * frequencyScale *
            (151.0 * time + 13.0 * time * time) + texturePhase);
        const double body = wave *
            (0.14 * ready + 1.22 * plunge + 0.53 * pressure + 1.34 * finish) +
            grain * (0.13 * pressure + 0.26 * finish);
        const double pan = (variant - 1.5) * 0.035;
        left = body * (1.01 - pan);
        right = body * (0.96 + pan);
        break;
    }
    case Kind::Slash: {
        const double sweep = Between(time, 0.31, 0.96, 0.095);
        const double edge = Bell(time, 0.510 + timingShift, 210.0);
        const double finish = Bell(time, 1.158 + timingShift, 205.0);
        const double progress = std::clamp((time - 0.35) / 0.58, 0.0, 1.0);
        const double wave =
            0.66 * std::sin(TwoPi * frequencyScale * (74.0 * time + 36.0 * time * time)) +
            0.34 * std::sin(TwoPi * frequencyScale *
                (153.0 * time + 19.0 * time * time) + texturePhase);
        const double body = wave * (0.49 * sweep + 1.15 * edge + 1.12 * finish);
        left = body * (1.15 - 0.48 * progress);
        right = body * (0.67 + 0.48 * progress);
        break;
    }
    case Kind::ArmCut: {
        const double bite = Bell(time, 0.525 + timingShift, 225.0);
        const double cut = Between(time, 0.53, 1.04, 0.055);
        const double tear = Bell(time, 1.145 + timingShift, 175.0);
        const double finish = Bell(time, 1.335 + timingShift, 230.0);
        const double rough = std::sin(TwoPi * frequencyScale *
            (68.0 * time + 9.0 * time * time)) *
            (0.78 + 0.22 * std::sin(TwoPi * 13.0 * time + texturePhase));
        const double edge = std::sin(TwoPi * frequencyScale *
            (142.0 * time + 25.0 * time * time) + texturePhase);
        const double body = rough *
            (1.18 * bite + 0.62 * cut + 0.94 * tear + 1.20 * finish) +
            edge * (0.24 * cut + 0.20 * tear);
        left = body * 1.04;
        right = body * 0.79;
        break;
    }
    case Kind::Downed: {
        const double fall = Between(time, 0.18, 0.80, 0.12) * Smooth((time - 0.18) / 0.34);
        const double first = Bell(time, 0.575 + timingShift, 190.0);
        const double impact = Bell(time, 1.275 + timingShift, 165.0);
        const double settle = Bell(time, 1.465 + timingShift, 145.0);
        const double low = std::sin(TwoPi * frequencyScale *
            (47.0 * time - 2.3 * time * time));
        const double crack = std::sin(TwoPi * frequencyScale *
            (105.0 * time + 6.0 * time * time) + texturePhase);
        const double body = low *
            (0.24 * fall + 0.94 * first + 1.42 * impact + 0.82 * settle) +
            crack * (0.24 * first + 0.34 * impact);
        left = body * 1.05;
        right = body * 1.02;
        break;
    }
    case Kind::Const: {
        // Observed cFatalBlowConst native contacts occur around +0.09 s and
        // +1.02 s. Start immediately with the lock-in impact, retain body
        // pressure through the held animation, then land the finishing cut.
        const double lock = Bell(time, 0.095 + timingShift * 0.35, 245.0);
        const double pressure = Between(time, 0.105, 0.86, 0.070) *
            std::exp(-0.52 * std::max(0.0, time - 0.105));
        const double finish = Bell(time, 1.020 + timingShift * 0.35, 185.0);
        const double settle = Bell(time, 1.125 + timingShift * 0.25, 260.0);
        const double low =
            0.76 * std::sin(TwoPi * (58.0 * time - 1.8 * time * time)) +
            0.24 * std::sin(TwoPi * 111.0 * time + texturePhase);
        const double edge = std::sin(TwoPi *
            (158.0 * time + 10.0 * time * time) + texturePhase);
        const double body = low *
            (1.12 * lock + 0.43 * pressure + 1.28 * finish + 0.58 * settle) +
            edge * (0.12 * pressure + 0.24 * finish);
        const double pan = (variant - 1.5) * 0.008;
        left = body * (1.00 - pan);
        right = body * (0.99 + pan);
        break;
    }
    case Kind::BreakIssen: {
        // cBreakIssenQuick's first native motor contact was observed at
        // +0.46 s. Build into that strike with a restrained blade-pressure
        // ramp, then carry the cut through instead of reducing it to a click.
        const double brace = Between(time, 0.12, 0.445, 0.075);
        const double contact = Bell(time, 0.463 + timingShift * 0.35, 225.0);
        const double cut = Between(time, 0.455, 0.790, 0.052) *
            std::exp(-0.75 * std::max(0.0, time - 0.455));
        const double release = Bell(time, 0.835 + timingShift * 0.30, 205.0);
        const double low =
            0.76 * std::sin(TwoPi * (57.0 * time - 2.0 * time * time)) +
            0.24 * std::sin(TwoPi * 112.0 * time + texturePhase);
        const double edge = std::sin(TwoPi *
            (166.0 * time + 15.0 * time * time) + texturePhase);
        const double body = low *
            (0.22 * brace + 1.42 * contact + 0.62 * cut + 0.88 * release) +
            edge * (0.28 * contact + 0.17 * cut + 0.20 * release);
        const double travel = std::clamp((time - 0.40) / 0.46, 0.0, 1.0);
        left = body * (1.13 - 0.27 * travel);
        right = body * (0.82 + 0.27 * travel);
        break;
    }
    case Kind::BlockIssen: {
        // cBlockIssenConst produced real contacts immediately after entry and
        // again near +0.97 s. Preserve both contacts around a firm held-blade
        // pressure bed so this final execution stage cannot feel like generic
        // XInput rumble.
        const double lock = Bell(time, 0.030 + timingShift * 0.22, 330.0);
        const double pressure = Between(time, 0.055, 0.82, 0.060) *
            std::exp(-0.38 * std::max(0.0, time - 0.055));
        const double finish = Bell(time, 0.974 + timingShift * 0.28, 195.0);
        const double settle = Bell(time, 1.095 + timingShift * 0.20, 235.0);
        const double low =
            0.79 * std::sin(TwoPi * (55.0 * time - 1.4 * time * time)) +
            0.21 * std::sin(TwoPi * 108.0 * time + texturePhase);
        const double metal = std::sin(TwoPi *
            (172.0 * time + 8.0 * time * time) + texturePhase);
        const double body = low *
            (1.24 * lock + 0.45 * pressure + 1.38 * finish + 0.62 * settle) +
            metal * (0.25 * lock + 0.12 * pressure + 0.27 * finish);
        left = body * 1.02;
        right = body * 0.98;
        break;
    }
    default:
        break;
    }

    // Equalise integrated strength without erasing each execution's shape.
    // Thrust/slash used materially less energy than arm-cut/downed in the first
    // authored pass; these gains bring their perceived baseline together.
    double equalisation = 1.0;
    switch (kind) {
    case Kind::Thrust: equalisation = 1.16; break;
    case Kind::Slash: equalisation = 1.17; break;
    case Kind::ArmCut: equalisation = 0.91; break;
    case Kind::Downed: equalisation = 0.78; break;
    case Kind::Const: equalisation = 1.00; break;
    case Kind::BreakIssen: equalisation = 1.11; break;
    case Kind::BlockIssen: equalisation = 0.95; break;
    default: break;
    }
    const double gain = std::clamp(static_cast<double>(intensity), 0.0, 2.0) *
        equalisation * endFade;
    return {
        static_cast<float>(0.97 * std::tanh(left * gain / 0.97)),
        static_cast<float>(0.97 * std::tanh(right * gain / 0.97))};
}
} // namespace execution_haptics

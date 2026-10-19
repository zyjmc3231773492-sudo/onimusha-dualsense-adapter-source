#define NOMINMAX
#include <Windows.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "captured_haptic_presets.hpp"
#include "adaptive_trigger_effects.hpp"
#include "product_runtime.hpp"

namespace {
struct RuntimeSandbox {
    std::filesystem::path root;
    std::wstring originalProgramData = product_runtime::detail::EnvironmentPath(L"ProgramData");
    std::wstring originalLocalData = product_runtime::detail::EnvironmentPath(L"LOCALAPPDATA");

    RuntimeSandbox() {
        wchar_t temporaryDirectory[MAX_PATH]{};
        wchar_t temporaryName[MAX_PATH]{};
        if (!GetTempPathW(MAX_PATH, temporaryDirectory) ||
            !GetTempFileNameW(temporaryDirectory, L"oni", 0, temporaryName))
            throw std::runtime_error("Cannot create runtime smoke sandbox");
        root = std::filesystem::absolute(temporaryName);
        if (root != (std::filesystem::absolute(temporaryDirectory) / root.filename()).lexically_normal() ||
            !root.filename().wstring().starts_with(L"oni"))
            throw std::runtime_error("Runtime smoke sandbox escaped temporary directory");
        DeleteFileW(root.c_str());
        std::filesystem::create_directory(root);
        std::filesystem::create_directory(root / L"local");
        std::filesystem::create_directory(root / L"local" / L"OnimushaPS5Adapter");
        SetEnvironmentVariableW(L"ProgramData", (root / L"machine").c_str());
        SetEnvironmentVariableW(L"LOCALAPPDATA", (root / L"local").c_str());
    }

    ~RuntimeSandbox() {
        SetEnvironmentVariableW(L"ProgramData", originalProgramData.empty() ? nullptr : originalProgramData.c_str());
        SetEnvironmentVariableW(L"LOCALAPPDATA", originalLocalData.empty() ? nullptr : originalLocalData.c_str());
        std::error_code error;
        if (std::filesystem::exists(root, error)) {
            for (const auto& entry : std::filesystem::recursive_directory_iterator(root, error))
                if (entry.is_regular_file()) SetFileAttributesW(entry.path().c_str(), FILE_ATTRIBUTE_NORMAL);
            std::filesystem::remove_all(root, error);
        }
    }

    void Write(const std::filesystem::path& path, const std::string& contents) {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream file(path, std::ios::binary);
        file << contents;
        if (!file) throw std::runtime_error("Cannot write runtime smoke fixture");
    }
};

bool CheckFreeRuntime() {
    RuntimeSandbox sandbox;
    const auto localSettings = sandbox.root / L"local" / L"OnimushaPS5Adapter" / L"settings.json";
    const auto machineDirectory = sandbox.root / L"machine" / L"OnimushaPS5Adapter";
    const std::array<std::wstring, 4> oldNames{
        L"license.dat", L"trial-state.bin", L"trial-status.json", L"device-seed.dat"};

    // A completely clean installation works without creating ProgramData or an identity.
    product_runtime::ProductRuntime clean;
    for (ULONGLONG elapsed = 0; elapsed <= 1200000; elapsed += 1000) {
        clean.Tick(true, elapsed);
        if (!clean.IsAllowed()) return false;
    }
    clean.Tick(false, 86400000);
    if (!clean.IsAllowed() || std::filesystem::exists(sandbox.root / L"machine")) return false;

    // Both corrupt and consumed records from commercial versions are inert.
    for (const bool expired : {false, true}) {
        std::array<std::string, 4> contents{
            "invalid activation", "unreadable old trial state",
            expired ? R"({"schemaVersion":1,"usedSeconds":600,"consumed":true})" : "{broken json",
            "invalid old device seed"};
        std::array<std::filesystem::file_time_type, 4> timestamps{};
        for (size_t index = 0; index < oldNames.size(); ++index) {
            const auto path = machineDirectory / oldNames[index];
            SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
            sandbox.Write(path, contents[index]);
            timestamps[index] = std::filesystem::last_write_time(path);
            SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_READONLY);
        }
        product_runtime::ProductRuntime runtime;
        for (ULONGLONG elapsed = 0; elapsed <= 1200000; elapsed += 1000) {
            runtime.Tick(true, elapsed);
            if (!runtime.IsAllowed()) return false;
        }
        runtime.Tick(false, 86400000);
        for (size_t index = 0; index < oldNames.size(); ++index) {
            const auto path = machineDirectory / oldNames[index];
            std::vector<uint8_t> data;
            if (!product_runtime::detail::ReadBinaryFile(path.wstring(), data) ||
                std::string(data.begin(), data.end()) != contents[index] ||
                std::filesystem::last_write_time(path) != timestamps[index]) return false;
        }
        if (std::distance(std::filesystem::directory_iterator(machineDirectory),
                          std::filesystem::directory_iterator()) != static_cast<std::ptrdiff_t>(oldNames.size())) return false;
    }

    // Even an inaccessible machine-data location cannot disable runtime features.
    const auto blocked = sandbox.root / L"blocked-machine-data";
    const std::string blockedContents = "This is a file, not a directory";
    sandbox.Write(blocked, blockedContents);
    SetEnvironmentVariableW(L"ProgramData", blocked.c_str());
    product_runtime::ProductRuntime inaccessible;
    inaccessible.Tick(true, 1200000);
    if (!inaccessible.IsAllowed() || std::filesystem::file_size(blocked) != blockedContents.size()) return false;

    // Removing licensing preserves the one-second polling and speaker-disable event.
    sandbox.Write(localSettings, R"({"SchemaVersion":13,"ControllerSpeakerEnabled":true,"HapticIntensityPercent":135})");
    product_runtime::ProductRuntime settingsRuntime;
    settingsRuntime.Tick(true, 0);
    sandbox.Write(localSettings, R"({"SchemaVersion":13,"ControllerSpeakerEnabled":false,"GauntletVoiceLanguage":"ja-JP","HapticIntensityPercent":150})");
    settingsRuntime.Tick(true, 500);
    if (!settingsRuntime.GetSettings().controllerSpeakerEnabled) return false;
    settingsRuntime.Tick(false, 1000);
    if (settingsRuntime.GetSettings().controllerSpeakerEnabled ||
        settingsRuntime.GetSettings().gauntletVoiceLanguage != product_runtime::GauntletVoiceLanguage::Japanese ||
        std::abs(settingsRuntime.GetSettings().hapticIntensity - 1.5f) > 0.001f ||
        !settingsRuntime.ConsumeControllerSpeakerJustDisabled() ||
        settingsRuntime.ConsumeControllerSpeakerJustDisabled()) return false;
    sandbox.Write(localSettings, R"({"SchemaVersion":13,"ControllerSpeakerEnabled":true})");
    settingsRuntime.Tick(false, 2000);
    return settingsRuntime.IsAllowed() && settingsRuntime.GetSettings().controllerSpeakerEnabled &&
        !settingsRuntime.ConsumeControllerSpeakerJustDisabled();
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc > 2 || (argc == 2 && std::wcscmp(argv[1], L"--settings-only") != 0 &&
                    std::wcscmp(argv[1], L"--free-runtime") != 0)) {
        std::fwprintf(stderr, L"usage: native_product_runtime_smoke [--settings-only|--free-runtime]\n");
        return 2;
    }
    const auto itemProfile = captured_haptic_presets::ProfileFor(
        captured_haptic_presets::Preset::GetItem);
    const auto useProfile = captured_haptic_presets::ProfileFor(
        captured_haptic_presets::Preset::UseItem);
    const auto interactProfile = captured_haptic_presets::ProfileFor(
        captured_haptic_presets::Preset::GenericInteract);
    if (itemProfile.durationSeconds != 0.100f ||
        itemProfile.motorPower[0] != 0.100f ||
        useProfile.durationSeconds != 0.100f ||
        useProfile.motorPower[0] != 0.100f ||
        interactProfile.durationSeconds != 0.150f ||
        interactProfile.motorPower[0] != 0.200f ||
        interactProfile.motorPower[1] != 0.200f ||
        captured_haptic_presets::IsValid(3) ||
        captured_haptic_presets::CalibrateMotorPower(0.200f) <=
            captured_haptic_presets::CalibrateMotorPower(0.100f)) {
        std::fwprintf(stderr, L"FAIL: captured native haptic profiles changed\n");
        return 7;
    }

    std::array<uint8_t, 48> absorbTrigger{};
    absorbTrigger[0] = 0x02;
    absorbTrigger[1] = adaptive_trigger_effects::LeftTriggerFlag |
        adaptive_trigger_effects::RightTriggerFlag;
    absorbTrigger[adaptive_trigger_effects::RightTriggerOffset] =
        adaptive_trigger_effects::TriggerEffectOff;
    if (!adaptive_trigger_effects::EncodeDsxVibration(
            absorbTrigger,
            adaptive_trigger_effects::LeftTriggerOffset,
            adaptive_trigger_effects::SoulAbsorbStartPosition,
            adaptive_trigger_effects::SoulAbsorbAmplitude,
            adaptive_trigger_effects::SoulAbsorbFrequencyHz) ||
        absorbTrigger[adaptive_trigger_effects::LeftTriggerOffset] != 0x26 ||
        absorbTrigger[adaptive_trigger_effects::LeftTriggerOffset + 1] != 0x80 ||
        absorbTrigger[adaptive_trigger_effects::LeftTriggerOffset + 2] != 0x03 ||
        absorbTrigger[adaptive_trigger_effects::LeftTriggerOffset + 5] != 0x60 ||
        absorbTrigger[adaptive_trigger_effects::LeftTriggerOffset + 6] != 0x1B ||
        absorbTrigger[adaptive_trigger_effects::LeftTriggerOffset + 9] !=
            adaptive_trigger_effects::SoulAbsorbFrequencyHz ||
        !adaptive_trigger_effects::HasActiveEffect(absorbTrigger)) {
        std::fwprintf(stderr, L"FAIL: DSX absorption trigger encoding changed\n");
        return 12;
    }
    const auto leftDisabled = adaptive_trigger_effects::ApplyEnablement(
        absorbTrigger, false, true);
    if (leftDisabled[adaptive_trigger_effects::LeftTriggerOffset] !=
            adaptive_trigger_effects::TriggerEffectOff ||
        adaptive_trigger_effects::HasActiveEffect(leftDisabled)) {
        std::fwprintf(stderr, L"FAIL: left trigger enablement filter changed\n");
        return 13;
    }
    std::array<uint8_t, 48> bowTrigger{};
    bowTrigger[0] = 0x02;
    bowTrigger[1] = adaptive_trigger_effects::LeftTriggerFlag |
        adaptive_trigger_effects::RightTriggerFlag;
    bowTrigger[adaptive_trigger_effects::LeftTriggerOffset] =
        adaptive_trigger_effects::TriggerEffectOff;
    adaptive_trigger_effects::EncodeDsxVibration(
        bowTrigger,
        adaptive_trigger_effects::RightTriggerOffset,
        3,
        4,
        30);
    const auto bowWithLeftDisabled = adaptive_trigger_effects::ApplyEnablement(
        bowTrigger, false, true);
    const auto bowWithRightDisabled = adaptive_trigger_effects::ApplyEnablement(
        bowTrigger, true, false);
    if (!adaptive_trigger_effects::HasActiveEffect(bowWithLeftDisabled) ||
        adaptive_trigger_effects::HasActiveEffect(bowWithRightDisabled) ||
        bowWithLeftDisabled[adaptive_trigger_effects::LeftTriggerOffset] !=
            adaptive_trigger_effects::TriggerEffectOff ||
        bowWithRightDisabled[adaptive_trigger_effects::RightTriggerOffset] !=
            adaptive_trigger_effects::TriggerEffectOff) {
        std::fwprintf(stderr, L"FAIL: independent trigger enablement changed\n");
        return 14;
    }

    const auto japaneseSettings = product_runtime::detail::ParseSettings(
        R"({"RainEnabled":false,"WindEnabled":true,"HapticIntensityPercent":150,"GuardHapticEnabled":false,"GuardHapticIntensityPercent":65,"ParryHapticEnabled":false,"ParryHapticIntensityPercent":135,"DeflectHapticEnabled":true,"DeflectHapticIntensityPercent":175,"IssenHapticEnabled":false,"IssenHapticIntensityPercent":250,"LeftAdaptiveTriggerEnabled":false,"RightAdaptiveTriggerEnabled":true,"ControllerSpeakerEnabled":false,"GauntletVoiceLanguage":"ja-JP","GauntletVoiceVolumePercent":125,"SoulSpeakerVolumePercent":-5,"ClashSpeakerVolumePercent":250,"GuardSpeakerVolumePercent":75,"DeflectSpeakerVolumePercent":80,"ExecutionSpeakerVolumePercent":90,"IssenSpeakerVolumePercent":110})");
    const auto fallbackSettings = product_runtime::detail::ParseSettings(
        R"({"GauntletVoiceLanguage":"unsupported"})");
    const auto versionEightCombatSettings =
        product_runtime::detail::ParseSettings(
            R"({"DeflectHapticEnabled":false,"DeflectHapticIntensityPercent":165})");
    if (japaneseSettings.rainEnabled || !japaneseSettings.windEnabled ||
        japaneseSettings.hapticIntensity != 1.50f ||
        japaneseSettings.guardHapticEnabled ||
        japaneseSettings.guardHapticIntensity != 0.65f ||
        japaneseSettings.parryHapticEnabled ||
        japaneseSettings.parryHapticIntensity != 1.35f ||
        !japaneseSettings.deflectHapticEnabled ||
        japaneseSettings.deflectHapticIntensity != 1.75f ||
        japaneseSettings.issenHapticEnabled ||
        japaneseSettings.issenHapticIntensity != 2.0f ||
        japaneseSettings.leftAdaptiveTriggerEnabled ||
        !japaneseSettings.rightAdaptiveTriggerEnabled ||
        japaneseSettings.controllerSpeakerEnabled ||
        japaneseSettings.gauntletVoiceLanguage !=
            product_runtime::GauntletVoiceLanguage::Japanese ||
        japaneseSettings.gauntletVoiceVolume != 1.25f ||
        japaneseSettings.soulSpeakerVolume != 0.0f ||
        japaneseSettings.clashSpeakerVolume != 2.0f ||
        japaneseSettings.guardSpeakerVolume != 0.75f ||
        japaneseSettings.deflectSpeakerVolume != 0.80f ||
        japaneseSettings.executionSpeakerVolume != 0.90f ||
        japaneseSettings.issenSpeakerVolume != 1.10f ||
        fallbackSettings.gauntletVoiceLanguage !=
            product_runtime::GauntletVoiceLanguage::Chinese ||
        !fallbackSettings.leftAdaptiveTriggerEnabled ||
        !fallbackSettings.rightAdaptiveTriggerEnabled ||
        fallbackSettings.hapticIntensity != 1.0f ||
        !fallbackSettings.guardHapticEnabled ||
        fallbackSettings.guardHapticIntensity != 1.0f ||
        !fallbackSettings.parryHapticEnabled ||
        fallbackSettings.parryHapticIntensity != 1.0f ||
        !fallbackSettings.deflectHapticEnabled ||
        fallbackSettings.deflectHapticIntensity != 1.0f ||
        !fallbackSettings.issenHapticEnabled ||
        fallbackSettings.issenHapticIntensity != 1.0f ||
        versionEightCombatSettings.parryHapticEnabled ||
        versionEightCombatSettings.parryHapticIntensity != 1.65f ||
        versionEightCombatSettings.deflectHapticEnabled ||
        versionEightCombatSettings.deflectHapticIntensity != 1.65f ||
        fallbackSettings.gauntletVoiceVolume != 1.0f ||
        fallbackSettings.soulSpeakerVolume != 1.0f) {
        std::fwprintf(stderr, L"FAIL: product settings parsing changed\n");
        return 8;
    }
    const auto normalAttackEnabled = product_runtime::detail::ParseSettings(
        R"({"SchemaVersion":10,"NormalAttackHapticEnabled":true,"HapticIntensityPercent":135,"GuardHapticEnabled":false})");
    const auto normalAttackDisabled = product_runtime::detail::ParseSettings(
        R"({"SchemaVersion":10,"NormalAttackHapticEnabled":false,"HapticIntensityPercent":135,"GuardHapticEnabled":false})");
    const auto oldNormalAttackSettings = product_runtime::detail::ParseSettings(
        R"({"SchemaVersion":9,"GuardHapticEnabled":true})");
    static_assert(product_runtime::NormalAttackHapticsAlwaysEnabled);
    if (std::abs(normalAttackEnabled.hapticIntensity - 1.35f) > 0.001f ||
        normalAttackDisabled.hapticIntensity != normalAttackEnabled.hapticIntensity ||
        normalAttackEnabled.guardHapticEnabled || normalAttackDisabled.guardHapticEnabled ||
        !oldNormalAttackSettings.guardHapticEnabled) {
        std::fwprintf(stderr, L"FAIL: obsolete normal attack toggle must be ignored without changing other preferences\n");
        return 15;
    }
    try {
        if (!CheckFreeRuntime()) {
            std::fwprintf(stderr, L"FAIL: free runtime must ignore old records, avoid machine-data writes, and keep settings polling\n");
            return 16;
        }
    } catch (const std::exception& exception) {
        std::fprintf(stderr, "FAIL: free runtime sandbox: %s\n", exception.what());
        return 17;
    }
    std::wprintf(L"Native free runtime, settings, trigger, and haptic smoke tests passed.\n");
    return 0;
}

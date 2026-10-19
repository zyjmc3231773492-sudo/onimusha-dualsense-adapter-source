#pragma once

#define NOMINMAX
#include <Windows.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

namespace product_runtime {

// Internal policy, not a user setting. Gameplay still requires fighting spirit.
constexpr bool NormalAttackHapticsAlwaysEnabled = true;
constexpr float GauntletSpeakerBaseline = 0.55f;
constexpr float SoulSpeakerBaseline = 0.60f;
constexpr float ParrySpeakerBaseline = 0.25f;
constexpr float CalibratedSpeakerScale(float volume, float baseline) {
    return std::clamp(volume, 0.0f, 2.0f) * baseline;
}

enum class GauntletVoiceLanguage : uint8_t {
    Chinese,
    Japanese,
};

struct Settings {
    bool rainEnabled = true;
    bool windEnabled = false;
    float hapticIntensity = 1.0f;
    bool guardHapticEnabled = true;
    float guardHapticIntensity = 1.0f;
    bool parryHapticEnabled = true;
    float parryHapticIntensity = 1.0f;
    bool deflectHapticEnabled = true;
    float deflectHapticIntensity = 1.0f;
    bool issenHapticEnabled = true;
    float issenHapticIntensity = 1.0f;
    bool leftAdaptiveTriggerEnabled = true;
    bool rightAdaptiveTriggerEnabled = true;
    bool controllerSpeakerEnabled = true;
    GauntletVoiceLanguage gauntletVoiceLanguage =
        GauntletVoiceLanguage::Chinese;
    float gauntletVoiceVolume = 1.0f;
    float soulSpeakerVolume = 1.0f;
    float clashSpeakerVolume = 1.0f;
    float guardSpeakerVolume = 1.0f;
    float parrySpeakerVolume = 1.0f;
    float deflectSpeakerVolume = 1.0f;
    float executionSpeakerVolume = 1.0f;
    float issenSpeakerVolume = 1.0f;
};

namespace detail {

inline std::wstring EnvironmentPath(const wchar_t* variable) {
    const DWORD required = GetEnvironmentVariableW(variable, nullptr, 0);
    if (required == 0) return {};
    std::wstring value(required, L'\0');
    const DWORD written = GetEnvironmentVariableW(variable, value.data(), required);
    if (written == 0 || written >= required) return {};
    value.resize(written);
    return value;
}

inline std::wstring LocalDataPath() {
    std::wstring root = EnvironmentPath(L"LOCALAPPDATA");
    if (root.empty()) return {};
    return root + L"\\OnimushaPS5Adapter";
}

inline bool ReadBinaryFile(const std::wstring& path, std::vector<uint8_t>& data) {
    data.clear();
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0 ||
        size.QuadPart > 1024 * 1024) {
        CloseHandle(file);
        return false;
    }
    data.resize(static_cast<size_t>(size.QuadPart));
    DWORD read = 0;
    const bool ok = data.empty() ||
        (ReadFile(file, data.data(), static_cast<DWORD>(data.size()), &read, nullptr) &&
         read == data.size());
    CloseHandle(file);
    if (!ok) data.clear();
    return ok;
}

inline bool JsonBoolean(const std::string& json, const char* property, bool fallback) {
    const std::string key = std::string("\"") + property + "\"";
    size_t position = json.find(key);
    if (position == std::string::npos) return fallback;
    position = json.find(':', position + key.size());
    if (position == std::string::npos) return fallback;
    ++position;
    while (position < json.size() && std::isspace(
        static_cast<unsigned char>(json[position]))) ++position;
    if (json.compare(position, 4, "true") == 0) return true;
    if (json.compare(position, 5, "false") == 0) return false;
    return fallback;
}

inline bool JsonStringEquals(
    const std::string& json,
    const char* property,
    const char* expected) {
    const std::string key = std::string("\"") + property + "\"";
    size_t position = json.find(key);
    if (position == std::string::npos) return false;
    position = json.find(':', position + key.size());
    if (position == std::string::npos) return false;
    ++position;
    while (position < json.size() && std::isspace(
        static_cast<unsigned char>(json[position]))) ++position;
    const std::string quoted = std::string("\"") + expected + "\"";
    return json.compare(position, quoted.size(), quoted) == 0;
}

inline float JsonNumber(
    const std::string& json,
    const char* property,
    float fallback) {
    const std::string key = std::string("\"") + property + "\"";
    size_t position = json.find(key);
    if (position == std::string::npos) return fallback;
    position = json.find(':', position + key.size());
    if (position == std::string::npos) return fallback;
    ++position;
    while (position < json.size() && std::isspace(
        static_cast<unsigned char>(json[position]))) ++position;
    char* end = nullptr;
    const float value = std::strtof(json.c_str() + position, &end);
    if (end == json.c_str() + position || !std::isfinite(value)) {
        return fallback;
    }
    return value;
}

inline float PercentScale(
    const std::string& json,
    const char* property) {
    return std::clamp(JsonNumber(json, property, 100.0f), 0.0f, 200.0f) /
        100.0f;
}

inline Settings ParseSettings(const std::string& json) {
    Settings settings;
    settings.rainEnabled = JsonBoolean(json, "RainEnabled", true);
    settings.windEnabled = JsonBoolean(json, "WindEnabled", false);
    settings.hapticIntensity = PercentScale(
        json, "HapticIntensityPercent");
    settings.guardHapticEnabled = JsonBoolean(
        json, "GuardHapticEnabled", true);
    settings.guardHapticIntensity = PercentScale(
        json, "GuardHapticIntensityPercent");
    settings.parryHapticEnabled = JsonBoolean(
        json,
        "ParryHapticEnabled",
        JsonBoolean(json, "DeflectHapticEnabled", true));
    settings.parryHapticIntensity = std::clamp(
        JsonNumber(
            json,
            "ParryHapticIntensityPercent",
            JsonNumber(json, "DeflectHapticIntensityPercent", 100.0f)),
        0.0f,
        200.0f) / 100.0f;
    settings.deflectHapticEnabled = JsonBoolean(
        json, "DeflectHapticEnabled", true);
    settings.deflectHapticIntensity = PercentScale(
        json, "DeflectHapticIntensityPercent");
    settings.issenHapticEnabled = JsonBoolean(
        json, "IssenHapticEnabled", true);
    settings.issenHapticIntensity = PercentScale(
        json, "IssenHapticIntensityPercent");
    settings.leftAdaptiveTriggerEnabled = JsonBoolean(
        json, "LeftAdaptiveTriggerEnabled", true);
    settings.rightAdaptiveTriggerEnabled = JsonBoolean(
        json, "RightAdaptiveTriggerEnabled", true);
    settings.controllerSpeakerEnabled = JsonBoolean(
        json, "ControllerSpeakerEnabled", true);
    if (JsonStringEquals(json, "GauntletVoiceLanguage", "ja-JP")) {
        settings.gauntletVoiceLanguage = GauntletVoiceLanguage::Japanese;
    }
    settings.gauntletVoiceVolume = PercentScale(
        json, "GauntletVoiceVolumePercent");
    settings.soulSpeakerVolume = PercentScale(
        json, "SoulSpeakerVolumePercent");
    settings.clashSpeakerVolume = PercentScale(
        json, "ClashSpeakerVolumePercent");
    settings.guardSpeakerVolume = PercentScale(
        json, "GuardSpeakerVolumePercent");
    settings.deflectSpeakerVolume = PercentScale(
        json, "DeflectSpeakerVolumePercent");
    settings.parrySpeakerVolume = std::clamp(JsonNumber(
        json, "ParrySpeakerVolumePercent", settings.deflectSpeakerVolume * 100.0f),
        0.0f, 200.0f) / 100.0f;
    settings.executionSpeakerVolume = PercentScale(
        json, "ExecutionSpeakerVolumePercent");
    settings.issenSpeakerVolume = PercentScale(
        json, "IssenSpeakerVolumePercent");
    if (JsonNumber(json, "SchemaVersion", 13.0f) < 13.0f) {
        const auto rebase = [](float volume, float baseline) {
            return std::clamp(std::round(volume * 100.0f / baseline) / 100.0f, 0.0f, 2.0f);
        };
        settings.gauntletVoiceVolume = rebase(settings.gauntletVoiceVolume, GauntletSpeakerBaseline);
        settings.soulSpeakerVolume = rebase(settings.soulSpeakerVolume, SoulSpeakerBaseline);
        settings.parrySpeakerVolume = rebase(settings.parrySpeakerVolume, ParrySpeakerBaseline);
    }
    return settings;
}

inline Settings LoadSettings() {
    Settings settings;
    const std::wstring directory = LocalDataPath();
    if (directory.empty()) return settings;
    std::vector<uint8_t> file;
    if (!ReadBinaryFile(directory + L"\\settings.json", file)) return settings;
    const std::string json(file.begin(), file.end());
    return ParseSettings(json);
}

} // namespace detail

class ProductRuntime {
public:
    ProductRuntime() : m_settings(detail::LoadSettings()) {}

    // Poll user settings independently of controller connection or elapsed play time.
    // Supplying a timestamp also allows deterministic checks of long-running sessions.
    void Tick(bool /*controllerConnected*/, ULONGLONG now = GetTickCount64()) {
        if (now >= m_nextSettingsTick) {
            const bool wasEnabled = m_settings.controllerSpeakerEnabled;
            m_settings = detail::LoadSettings();
            if (wasEnabled && !m_settings.controllerSpeakerEnabled)
                m_controllerSpeakerJustDisabled = true;
            m_nextSettingsTick = now + 1000;
        }
    }

    const Settings& GetSettings() const { return m_settings; }
    constexpr bool IsAllowed() const { return true; }

    bool ConsumeControllerSpeakerJustDisabled() {
        const bool value = m_controllerSpeakerJustDisabled;
        m_controllerSpeakerJustDisabled = false;
        return value;
    }

private:
    Settings m_settings;
    ULONGLONG m_nextSettingsTick = 0;
    bool m_controllerSpeakerJustDisabled = false;
};

} // namespace product_runtime

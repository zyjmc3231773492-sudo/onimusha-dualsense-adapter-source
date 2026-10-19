#define NOMINMAX
#include <Windows.h>
#include <audioclient.h>
#include <propsys.h>
#include <functiondiscoverykeys_devpkey.h>
#include <hidsdi.h>
#include <ks.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#include <setupapi.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include "adaptive_trigger_effects.hpp"
#include "black_soul_haptics.hpp"
#include "ladder_haptics.hpp"
#include "wall_run_haptics.hpp"
#include "normal_attack_haptics.hpp"
#include "execution_haptics.hpp"
#include "captured_haptic_presets.hpp"
#include "combat_speaker_timing.hpp"
#include "dualsense_hid_report.hpp"
#include "dualsense_touch_input.hpp"
#include "combat_defense_feedback.hpp"
#include "live_motor_state.hpp"
#include "oni_strength_haptics.hpp"
#include "repel_impact_haptics.hpp"
#include "product_runtime.hpp"
#include "title_haptics.hpp"
#include "treasure_chest_haptics.hpp"
#include "oni_transformation_haptics.hpp"
#include "oni_charge_release_haptics.hpp"
#include "bow_shot_haptics.hpp"
#ifdef ONIMUSHA_BLUETOOTH_ENABLED
#include "dualsense_bluetooth.hpp"
#include "speaker_preview_pipe.hpp"
#include "speaker_preview_settings.hpp"
#include <opus.h>
#endif

namespace {
constexpr uint16_t SonyVendorId = 0x054C;
constexpr uint16_t DualSenseProductId = 0x0CE6;
constexpr uint16_t DualSenseEdgeProductId = 0x0DF2;
constexpr size_t MessageLength = dualsense_hid_report::LogicalReportLength;
constexpr size_t RightTriggerOffset = 0x0B;
constexpr size_t LeftTriggerOffset = 0x16;
// The logical report includes the USB report id at byte 0.  The standard
// DualSense rumble fields therefore sit at bytes 3/4 (right/left) and use
// enableBits1 at byte 1.  This is the same control report path used by the
// controller's ordinary game rumble, independent of the audio-haptics stream.
constexpr size_t StandardRumbleRightOffset = 0x03;
constexpr size_t StandardRumbleLeftOffset = 0x04;
constexpr size_t StandardRumbleEnhancedOffset = 0x27;
constexpr uint8_t StandardRumbleAudioDisableBit = 0x02;
constexpr uint8_t StandardRumbleEnhancedBit = 0x04;
// A Bluetooth 0x31 rumble packet sets the audio-haptics disable bit. Chest
// feedback is authored as a 0x36 PCM voice, so sending both paths makes the
// physical result dependent on which packet the controller applies last.
constexpr bool UseTreasureChestStandardRumble = false;
constexpr uint8_t HapticPlayMessage = 0xF1;
constexpr uint8_t HapticStopAllMessage = 0xF2;
constexpr uint8_t HapticMotorPowerMessage = 0xF3;
constexpr uint8_t HapticMotorPulseMessage = 0xF4;
constexpr uint8_t HapticStopMotorMessage = 0xF5;
constexpr uint8_t HapticStopWaveformsMessage = 0xF6;
constexpr uint8_t HapticEnvironmentMessage = 0xF7;
constexpr uint8_t HapticFootstepMessage = 0xF8;
constexpr uint8_t HapticMapZoomMessage = 0xF9;
constexpr uint8_t HapticPlayerReactionMessage = 0xFA;
constexpr uint8_t ControllerSpeakerMessage = 0xFB;
constexpr uint8_t GauntletVoiceMessage = 0xFC;
constexpr uint8_t TitleHapticMessage = 0xFD;
constexpr uint8_t HapticEnvironmentInteractionMessage = 0xED;
constexpr uint8_t HapticLongPressMessage = 0xEE;
constexpr uint8_t HapticOniStrengthMessage = 0xEF;
constexpr uint8_t HapticCapturedPresetMessage = 0xEC;
constexpr uint8_t HapticCombatActionMessage = 0xE9;
constexpr uint8_t HapticContinuousPlayerMessage = 0xEA;
constexpr uint8_t HapticOniEyeMessage = 0xEB;
static_assert(oni_transformation_haptics::Message != wall_run_haptics::Message,
    "Oni transformation and wall-run IPC message IDs must be unique");
static_assert(oni_transformation_haptics::Message != normal_attack_haptics::Message,
    "Oni transformation and normal-attack IPC message IDs must be unique");
static_assert(oni_charge_release_haptics::Message != oni_transformation_haptics::Message &&
    oni_charge_release_haptics::Message != normal_attack_haptics::Message &&
    oni_charge_release_haptics::Message != wall_run_haptics::Message,
    "Oni charge release IPC message ID must be unique");
static_assert(bow_shot_haptics::Message != oni_charge_release_haptics::Message &&
    bow_shot_haptics::Message != oni_transformation_haptics::Message &&
    bow_shot_haptics::Message != normal_attack_haptics::Message &&
    bow_shot_haptics::Message != wall_run_haptics::Message,
    "Bow shot IPC message ID must be unique");
enum class PlayerHapticProfile : uint32_t {
    Hit = 1,
    Stab = 2,
    Collision = 3,
    Knockdown = 4,
    Grab = 5,
    Dodge = 6,
    JustDodge = 7,
    CinematicImpact = 8
};
enum class EnvironmentInteractionProfile : uint8_t {
    GenericSuccess = 0,
    Door = 1,
    Lever = 2,
    ElevatorStart = 3,
    ElevatorMove = 4,
    ElevatorStop = 5,
    TreasureChestOpen = 6,
    TreasureChestPush = 7,
    TreasureChestLidLand = 8
};
enum class ControllerSpeakerCue : uint32_t {
    None = 0,
    SoulStart = 1,
    SoulCollect = 2,
    SoulStop = 3,
    CombatClash = 4,
    CombatGuard = 5,
    CombatDeflect = 6,
    CombatExecution = 7,
    CombatClashHit = 8,
    CombatClashSuccess = 9,
    CombatGuardDirectional = 10,
    CombatIssen = 11,
    CombatParryBlade = 12,
    CombatParryProjectile = 13
};
enum class CombatHapticProfile : uint8_t {
    Guard = 1,
    Parry = 2,
    Issen = 3,
    Deflect = 4,
    ProjectileParry = 5,
    ParryActivation = 6,
    ExecutionThrust = 7,
    ExecutionSlash = 8,
    ExecutionArmCut = 9,
    ExecutionDowned = 10,
    ExecutionConst = 11,
    ExecutionBreakIssen = 12,
    ExecutionBlockIssen = 13
};
enum class ContinuousPlayerHaptic : uint8_t {
    SoulAbsorption = 1,
    Sprint = 2,
    SoulSearch = 3,
    BlackSoulAbsorption = black_soul_haptics::Profile,
    LadderClimb = ladder_haptics::ClimbProfile,
    LadderFastSlide = ladder_haptics::FastSlideProfile
};
enum class ContinuousHapticOperation : uint8_t {
    Stop = 0,
    Start = 1,
    Refresh = 2,
    Accent = 3,
    Decelerate = 4
};
constexpr wchar_t PipePrefix[] = L"\\\\.\\pipe\\CodexOnimushaDualSense_";

void Log(const wchar_t* message) {
    HANDLE file = CreateFileW(
        L"reframework\\data\\codex_dualsense_bridge_helper.log",
        FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) return;

    SYSTEMTIME time{};
    GetLocalTime(&time);
    wchar_t line[768]{};
    const int length = swprintf_s(
        line,
        L"%04u-%02u-%02u %02u:%02u:%02u.%03u %s\r\n",
        time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute,
        time.wSecond, time.wMilliseconds, message);
    if (length > 0) {
        DWORD written = 0;
        WriteFile(file, line, static_cast<DWORD>(length * sizeof(wchar_t)), &written, nullptr);
    }
    CloseHandle(file);
}

void LogFormat(const wchar_t* format, ...) {
    wchar_t message[512]{};
    va_list args;
    va_start(args, format);
    _vsnwprintf_s(message, std::size(message), _TRUNCATE, format, args);
    va_end(args);
    Log(message);
}

template <typename T>
void SafeRelease(T*& object) {
    if (object != nullptr) {
        object->Release();
        object = nullptr;
    }
}

bool GetSupportedControllerAttributes(HANDLE handle, HIDD_ATTRIBUTES& attributes) {
    attributes = {};
    attributes.Size = sizeof(attributes);
    return HidD_GetAttributes(handle, &attributes) &&
        attributes.VendorID == SonyVendorId &&
        (attributes.ProductID == DualSenseProductId ||
         attributes.ProductID == DualSenseEdgeProductId);
}

bool IsSupportedController(HANDLE handle) {
    HIDD_ATTRIBUTES attributes{};
    return GetSupportedControllerAttributes(handle, attributes);
}

struct ControllerConnection {
    HANDLE handle = INVALID_HANDLE_VALUE;
    uint16_t productId = 0;
    uint16_t inputReportLength = 0;
    uint16_t outputReportLength = 0;
    uint16_t featureReportLength = 0;
    bool bluetooth = false;
    bool bluetoothInputReady = false;
    bool bluetoothEnhancedReady = false;
    bool bluetoothControlReady = false;
    std::wstring devicePath;

    bool IsOpen() const { return handle != INVALID_HANDLE_VALUE; }
};

std::mutex g_controllerWriteMutex;
uint8_t g_bluetoothSequence = 0; // shared by control and audio writes

bool ReadBluetoothFeatureReport(
    ControllerConnection& controller,
    uint8_t reportId,
    size_t minimumLength,
    size_t* returnedLength = nullptr) {
    if (returnedLength != nullptr) *returnedLength = 0;
    // HidD_GetFeature requires the collection's complete feature-report
    // length, even when the requested report itself is shorter.
    const size_t featureLength = std::max<size_t>(controller.featureReportLength, minimumLength);
    if (featureLength == 0 || featureLength > static_cast<size_t>(std::numeric_limits<ULONG>::max())) {
        LogFormat(L"Bluetooth feature 0x%02X unavailable; invalid report length=%zu",
            reportId, featureLength);
        return false;
    }
    std::vector<uint8_t> feature(featureLength, 0);
    feature[0] = reportId;
    SetLastError(ERROR_SUCCESS);
    const BOOL success = HidD_GetFeature(
        controller.handle, feature.data(), static_cast<ULONG>(feature.size()));
    if (!success) {
        LogFormat(L"Bluetooth feature 0x%02X read failed length=%u error=%u",
            reportId, static_cast<unsigned>(feature.size()), GetLastError());
        return false;
    }
    if (returnedLength != nullptr) *returnedLength = feature.size();
    if (feature[0] != reportId) {
        LogFormat(L"Bluetooth feature 0x%02X returned unexpected id=0x%02X length=%u",
            reportId, feature[0], static_cast<unsigned>(feature.size()));
        return false;
    }
    if (reportId == 0x20 && feature.size() >= 46) {
        const uint16_t firmware = static_cast<uint16_t>(feature[44]) |
            (static_cast<uint16_t>(feature[45]) << 8);
        LogFormat(L"Bluetooth firmware feature read ok version=0x%04X", firmware);
    } else {
        LogFormat(L"Bluetooth feature 0x%02X read ok length=%u",
            reportId, static_cast<unsigned>(feature.size()));
    }
    return true;
}

bool EnableBluetoothEnhancedReports(ControllerConnection& controller) {
    if (!controller.bluetooth || controller.bluetoothEnhancedReady) return true;

    // DualSense Bluetooth collections can remain in the simple/DirectInput
    // mode even though they accept 0x31 output writes. Reading either of
    // these feature reports is the activation step used by SDL and the
    // controller then accepts the enhanced 0x31 rumble report.
    size_t serialLength = 0;
    size_t firmwareLength = 0;
    const bool serialOk = ReadBluetoothFeatureReport(controller, 0x09, 7, &serialLength);
    const bool firmwareOk = ReadBluetoothFeatureReport(controller, 0x20, 46, &firmwareLength);
    controller.bluetoothEnhancedReady = serialOk || firmwareOk;
    LogFormat(L"Bluetooth enhanced reports %s serial=%s firmware=%s serialLength=%u firmwareLength=%u",
        controller.bluetoothEnhancedReady ? L"enabled" : L"unavailable",
        serialOk ? L"ok" : L"failed",
        firmwareOk ? L"ok" : L"failed",
        static_cast<unsigned>(serialLength),
        static_cast<unsigned>(firmwareLength));
    return controller.bluetoothEnhancedReady;
}

bool PrimeBluetoothInput(ControllerConnection& controller) {
    if (!controller.bluetooth) return true;
    if (!controller.bluetoothInputReady) {
        std::vector<uint8_t> input(controller.inputReportLength, 0);
        OVERLAPPED operation{};
        operation.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (operation.hEvent == nullptr) return false;
        DWORD read = 0;
        BOOL success = ReadFile(controller.handle, input.data(), static_cast<DWORD>(input.size()), &read, &operation);
        DWORD error = success ? ERROR_SUCCESS : GetLastError();
        if (!success && error == ERROR_IO_PENDING) {
            const DWORD wait = WaitForSingleObject(operation.hEvent, 1500);
            if (wait == WAIT_OBJECT_0) {
                success = GetOverlappedResult(controller.handle, &operation, &read, FALSE);
                error = success ? ERROR_SUCCESS : GetLastError();
            } else {
                CancelIoEx(controller.handle, &operation);
                GetOverlappedResult(controller.handle, &operation, &read, TRUE);
                error = ERROR_TIMEOUT;
            }
        }
        CloseHandle(operation.hEvent);
        if (!success || read < 10 || (input[0] != 0x01 && input[0] != 0x31)) {
            LogFormat(L"Bluetooth input not established; output postponed read=%u error=%u", read, error);
            return false;
        }
        controller.bluetoothInputReady = true;
        LogFormat(L"Bluetooth input established report=0x%02X bytes=%u", input[0], read);
    }
    return EnableBluetoothEnhancedReports(controller);
}

bool WriteControllerPacket(ControllerConnection& controller,
    const uint8_t* packet, size_t packetLength) {
    // Windows requires the collection's maximum length. CRC stays at the
    // individual packet's end, before the zero padding stripped by HIDClass.
    std::vector<uint8_t> padded(controller.outputReportLength, 0);
    if (packetLength > padded.size()) return false;
    std::copy(packet, packet + packetLength, padded.begin());
    OVERLAPPED operation{};
    operation.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (operation.hEvent == nullptr) return false;
    DWORD written = 0;
    BOOL success = WriteFile(controller.handle, padded.data(),
        static_cast<DWORD>(padded.size()), &written, &operation);
    DWORD error = success ? ERROR_SUCCESS : GetLastError();
    if (!success && error == ERROR_IO_PENDING) {
        const DWORD writeTimeout = controller.bluetooth && !controller.bluetoothControlReady ? 1500 : 250;
        const DWORD wait = WaitForSingleObject(operation.hEvent, writeTimeout);
        if (wait != WAIT_OBJECT_0) {
            CancelIoEx(controller.handle, &operation);
            // The buffer and OVERLAPPED must live until cancellation completes.
            GetOverlappedResult(controller.handle, &operation, &written, TRUE);
            error = wait == WAIT_TIMEOUT ? ERROR_TIMEOUT : GetLastError();
        } else {
            success = GetOverlappedResult(controller.handle, &operation, &written, FALSE);
            error = success ? ERROR_SUCCESS : GetLastError();
        }
    }
    CloseHandle(operation.hEvent);
    if (!success || written != padded.size()) {
        LogFormat(L"HID write failed transport=%s report=0x%02X expected=%u written=%u error=%u",
            controller.bluetooth ? L"Bluetooth" : L"USB", packet[0],
            controller.outputReportLength, written, error);
        return false;
    }
    if (controller.bluetooth && packet[0] == 0x31) controller.bluetoothControlReady = true;
    return true;
}

const wchar_t* ControllerModelName(uint16_t productId) {
    return productId == DualSenseEdgeProductId ? L"DualSense Edge" : L"DualSense";
}

void CloseController(ControllerConnection& controller) {
    if (controller.IsOpen()) CloseHandle(controller.handle);
    controller = {};
}

bool OpenController(ControllerConnection& result, bool readOnly = false) {
    CloseController(result);
    GUID hidGuid{};
    HidD_GetHidGuid(&hidGuid);
    HDEVINFO deviceInfo = SetupDiGetClassDevsW(
        &hidGuid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (deviceInfo == INVALID_HANDLE_VALUE) return false;

    bool opened = false;
    for (DWORD index = 0;; ++index) {
        SP_DEVICE_INTERFACE_DATA interfaceData{};
        interfaceData.cbSize = sizeof(interfaceData);
        if (!SetupDiEnumDeviceInterfaces(deviceInfo, nullptr, &hidGuid, index, &interfaceData)) break;

        DWORD requiredSize = 0;
        SetupDiGetDeviceInterfaceDetailW(
            deviceInfo, &interfaceData, nullptr, 0, &requiredSize, nullptr);
        if (requiredSize < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W)) continue;

        std::wstring detailStorage;
        detailStorage.resize((requiredSize + sizeof(wchar_t) - 1) / sizeof(wchar_t));
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(detailStorage.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(
                deviceInfo, &interfaceData, detail, requiredSize, nullptr, nullptr)) continue;

        HANDLE handle = CreateFileW(
            detail->DevicePath, readOnly ? GENERIC_READ : (GENERIC_READ | GENERIC_WRITE),
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (handle == INVALID_HANDLE_VALUE) continue;
        HIDD_ATTRIBUTES attributes{};
        if (!GetSupportedControllerAttributes(handle, attributes)) {
            CloseHandle(handle);
            continue;
        }

        PHIDP_PREPARSED_DATA preparsed = nullptr;
        HIDP_CAPS caps{};
        const bool hasCaps = HidD_GetPreparsedData(handle, &preparsed) &&
            HidP_GetCaps(preparsed, &caps) == HIDP_STATUS_SUCCESS;
        if (preparsed != nullptr) HidD_FreePreparsedData(preparsed);
        bool bluetooth = false;
#ifdef ONIMUSHA_BLUETOOTH_ENABLED
        bluetooth = hasCaps && dualsense_bluetooth::IsBluetoothCollection(
            caps.InputReportByteLength, caps.OutputReportByteLength);
#endif
        if (!hasCaps || caps.UsagePage != 0x01 || caps.Usage != 0x05 ||
            (!bluetooth && !dualsense_hid_report::IsSupportedTransportLength(
                caps.OutputReportByteLength))) {
            CloseHandle(handle);
            continue;
        }
        if (opened && bluetooth) { CloseHandle(handle); continue; }
        CloseController(result);
        result.handle = handle;
        result.productId = attributes.ProductID;
        result.inputReportLength = caps.InputReportByteLength;
        result.outputReportLength = caps.OutputReportByteLength;
        result.featureReportLength = caps.FeatureReportByteLength;
        result.bluetooth = bluetooth;
        result.devicePath = detail->DevicePath;
        LogFormat(
            L"%s opened pid=0x%04X usagePage=0x%04X usage=0x%04X inputReport=%u outputReport=%u featureReport=%u",
            ControllerModelName(result.productId),
            result.productId,
            caps.UsagePage,
            caps.Usage,
            result.inputReportLength,
            result.outputReportLength,
            result.featureReportLength);
        opened = true;
        if (!bluetooth) break; // prefer USB if both transports are present
    }

    SetupDiDestroyDeviceInfoList(deviceInfo);
    if (opened) LogFormat(L"Controller transport=%s", result.bluetooth ? L"Bluetooth" : L"USB");
    return opened;
}

bool SendReport(
    ControllerConnection& controller,
    const std::array<uint8_t, MessageLength>& report,
    bool* reopened = nullptr) {
    if (reopened != nullptr) *reopened = false;
    if (!controller.IsOpen()) {
        if (!OpenController(controller)) {
            Log(L"DualSense unavailable");
            return false;
        }
        if (reopened != nullptr) *reopened = true;
    }

    if (!PrimeBluetoothInput(controller)) return false;

    dualsense_hid_report::TransportReport transportReport{};
    if (!controller.bluetooth && !dualsense_hid_report::PrepareTransportReport(
            report, controller.outputReportLength, transportReport)) {
        LogFormat(
            L"Unsupported HID output report length=%u pid=0x%04X",
            controller.outputReportLength,
            controller.productId);
        CloseController(controller);
        return false;
    }

    std::lock_guard writeLock{g_controllerWriteMutex};
    bool sent = false;
#ifdef ONIMUSHA_BLUETOOTH_ENABLED
    if (controller.bluetooth) {
        const auto packet = dualsense_bluetooth::Control(report, g_bluetoothSequence++);
        sent = WriteControllerPacket(controller, packet.data(), packet.size());
    } else
#endif
    {
        sent = WriteControllerPacket(controller, transportReport.data(), controller.outputReportLength);
    }
    if (!sent) {
        CloseController(controller);
        return false;
    }
    return true;
}

void ResetTriggers(ControllerConnection& controller) {
    std::array<uint8_t, MessageLength> report{};
    report[0] = 0x02;
    report[1] = 0x0C;
    report[RightTriggerOffset] = 0x05;
    report[LeftTriggerOffset] = 0x05;
    SendReport(controller, report);
}

bool ConfigureControllerSpeaker(ControllerConnection& controller, bool enabled) {
    if (!controller.IsOpen() && !OpenController(controller)) return false;
    // DualSense USB output report layout used by SDL:
    // bit 7 updates the audio route, bit 5 updates speaker volume, byte 7
    // selects the controller speaker when set to 0x30.
    std::array<uint8_t, MessageLength> report{};
    report[0] = 0x02;
    report[1] = 0x80 | 0x20;
    report[6] = enabled ? 100 : 0;
    report[8] = enabled ? (controller.bluetooth ? 0x09 : 0x30) : 0x00;
    return SendReport(controller, report);
}

struct HapticClip {
    std::vector<float> stereoSamples;
    size_t Frames() const { return stereoSamples.size() / 2; }
};

struct SpeakerClip {
    std::vector<float> monoSamples;
    size_t Frames() const { return monoSamples.size(); }
};

uint16_t ReadU16(const uint8_t* data) {
    uint16_t value = 0;
    std::memcpy(&value, data, sizeof(value));
    return value;
}

uint32_t ReadU32(const uint8_t* data) {
    uint32_t value = 0;
    std::memcpy(&value, data, sizeof(value));
    return value;
}

bool ReadWholeFile(const std::wstring& path, std::vector<uint8_t>& bytes) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 || size.QuadPart > 16 * 1024 * 1024) {
        CloseHandle(file);
        return false;
    }
    bytes.resize(static_cast<size_t>(size.QuadPart));
    DWORD read = 0;
    const BOOL ok = ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr);
    CloseHandle(file);
    return ok && read == bytes.size();
}

bool LoadHapticWav(const std::wstring& path, HapticClip& clip) {
    std::vector<uint8_t> bytes;
    if (!ReadWholeFile(path, bytes) || bytes.size() < 44 ||
        std::memcmp(bytes.data(), "RIFF", 4) != 0 ||
        std::memcmp(bytes.data() + 8, "WAVE", 4) != 0) return false;

    const uint8_t* formatData = nullptr;
    uint32_t formatSize = 0;
    const uint8_t* sampleData = nullptr;
    uint32_t sampleSize = 0;
    for (size_t offset = 12; offset + 8 <= bytes.size();) {
        const uint32_t chunkSize = ReadU32(bytes.data() + offset + 4);
        const size_t dataOffset = offset + 8;
        if (dataOffset + chunkSize > bytes.size()) return false;
        if (std::memcmp(bytes.data() + offset, "fmt ", 4) == 0) {
            formatData = bytes.data() + dataOffset;
            formatSize = chunkSize;
        } else if (std::memcmp(bytes.data() + offset, "data", 4) == 0) {
            sampleData = bytes.data() + dataOffset;
            sampleSize = chunkSize;
        }
        offset = dataOffset + chunkSize + (chunkSize & 1u);
    }
    if (formatData == nullptr || formatSize < 16 || sampleData == nullptr) return false;

    uint16_t formatTag = ReadU16(formatData);
    const uint16_t channels = ReadU16(formatData + 2);
    const uint32_t sampleRate = ReadU32(formatData + 4);
    const uint16_t blockAlign = ReadU16(formatData + 12);
    const uint16_t bitsPerSample = ReadU16(formatData + 14);
    if (formatTag == WAVE_FORMAT_EXTENSIBLE && formatSize >= 40) {
        GUID subFormat{};
        std::memcpy(&subFormat, formatData + 24, sizeof(subFormat));
        if (subFormat == KSDATAFORMAT_SUBTYPE_PCM) formatTag = WAVE_FORMAT_PCM;
        else if (subFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) formatTag = WAVE_FORMAT_IEEE_FLOAT;
    }
    if (channels != 2 || sampleRate != 48000 || blockAlign == 0 || sampleSize < blockAlign) {
        return false;
    }

    const size_t frames = sampleSize / blockAlign;
    clip.stereoSamples.resize(frames * 2);
    if (formatTag == WAVE_FORMAT_PCM && bitsPerSample == 16 && blockAlign >= 4) {
        for (size_t frame = 0; frame < frames; ++frame) {
            const uint8_t* source = sampleData + frame * blockAlign;
            int16_t left = 0;
            int16_t right = 0;
            std::memcpy(&left, source, sizeof(left));
            std::memcpy(&right, source + 2, sizeof(right));
            clip.stereoSamples[frame * 2] = static_cast<float>(left) / 32768.0f;
            clip.stereoSamples[frame * 2 + 1] = static_cast<float>(right) / 32768.0f;
        }
        return true;
    }
    if (formatTag == WAVE_FORMAT_IEEE_FLOAT && bitsPerSample == 32 && blockAlign >= 8) {
        for (size_t frame = 0; frame < frames; ++frame) {
            const uint8_t* source = sampleData + frame * blockAlign;
            std::memcpy(&clip.stereoSamples[frame * 2], source, sizeof(float));
            std::memcpy(&clip.stereoSamples[frame * 2 + 1], source + 4, sizeof(float));
        }
        return true;
    }
    clip.stereoSamples.clear();
    return false;
}

bool LoadSpeakerWav(const std::wstring& path, SpeakerClip& clip) {
    std::vector<uint8_t> bytes;
    if (!ReadWholeFile(path, bytes) || bytes.size() < 44 ||
        std::memcmp(bytes.data(), "RIFF", 4) != 0 ||
        std::memcmp(bytes.data() + 8, "WAVE", 4) != 0) return false;

    const uint8_t* formatData = nullptr;
    uint32_t formatSize = 0;
    const uint8_t* sampleData = nullptr;
    uint32_t sampleSize = 0;
    for (size_t offset = 12; offset + 8 <= bytes.size();) {
        const uint32_t chunkSize = ReadU32(bytes.data() + offset + 4);
        const size_t dataOffset = offset + 8;
        if (dataOffset + chunkSize > bytes.size()) return false;
        if (std::memcmp(bytes.data() + offset, "fmt ", 4) == 0) {
            formatData = bytes.data() + dataOffset;
            formatSize = chunkSize;
        } else if (std::memcmp(bytes.data() + offset, "data", 4) == 0) {
            sampleData = bytes.data() + dataOffset;
            sampleSize = chunkSize;
        }
        offset = dataOffset + chunkSize + (chunkSize & 1u);
    }
    if (formatData == nullptr || formatSize < 16 || sampleData == nullptr) return false;

    uint16_t formatTag = ReadU16(formatData);
    const uint16_t channels = ReadU16(formatData + 2);
    const uint32_t sampleRate = ReadU32(formatData + 4);
    const uint16_t blockAlign = ReadU16(formatData + 12);
    const uint16_t bitsPerSample = ReadU16(formatData + 14);
    if (formatTag == WAVE_FORMAT_EXTENSIBLE && formatSize >= 40) {
        GUID subFormat{};
        std::memcpy(&subFormat, formatData + 24, sizeof(subFormat));
        if (subFormat == KSDATAFORMAT_SUBTYPE_PCM) formatTag = WAVE_FORMAT_PCM;
        else if (subFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) {
            formatTag = WAVE_FORMAT_IEEE_FLOAT;
        }
    }
    if ((channels != 1 && channels != 2) || sampleRate != 48000 ||
        blockAlign == 0 || sampleSize < blockAlign) return false;

    const size_t frames = sampleSize / blockAlign;
    clip.monoSamples.resize(frames);
    if (formatTag == WAVE_FORMAT_PCM && bitsPerSample == 16 &&
        blockAlign >= channels * sizeof(int16_t)) {
        for (size_t frame = 0; frame < frames; ++frame) {
            const uint8_t* source = sampleData + frame * blockAlign;
            int16_t left = 0;
            int16_t right = 0;
            std::memcpy(&left, source, sizeof(left));
            if (channels == 2) std::memcpy(&right, source + 2, sizeof(right));
            else right = left;
            clip.monoSamples[frame] =
                (static_cast<float>(left) + static_cast<float>(right)) / 65536.0f;
        }
        return true;
    }
    if (formatTag == WAVE_FORMAT_IEEE_FLOAT && bitsPerSample == 32 &&
        blockAlign >= channels * sizeof(float)) {
        for (size_t frame = 0; frame < frames; ++frame) {
            const uint8_t* source = sampleData + frame * blockAlign;
            float left = 0.0f;
            float right = 0.0f;
            std::memcpy(&left, source, sizeof(left));
            if (channels == 2) std::memcpy(&right, source + 4, sizeof(right));
            else right = left;
            clip.monoSamples[frame] = (left + right) * 0.5f;
        }
        return true;
    }
    clip.monoSamples.clear();
    return false;
}

bool ContainsCaseInsensitive(const wchar_t* text, const wchar_t* fragment) {
    if (text == nullptr || fragment == nullptr) return false;
    const size_t textLength = wcslen(text);
    const size_t fragmentLength = wcslen(fragment);
    if (fragmentLength == 0 || textLength < fragmentLength) return false;
    for (size_t index = 0; index <= textLength - fragmentLength; ++index) {
        if (_wcsnicmp(text + index, fragment, fragmentLength) == 0) return true;
    }
    return false;
}

bool IsDualSenseRenderEndpointName(const wchar_t* name) {
    return ContainsCaseInsensitive(name, L"DualSense Wireless Controller") ||
        ContainsCaseInsensitive(name, L"DualSense Edge Wireless Controller");
}

HRESULT FindDualSenseRenderEndpoint(IMMDevice** result) {
    *result = nullptr;
    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDeviceCollection* collection = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumerator));
    if (FAILED(hr)) return hr;

    hr = enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection);
    if (FAILED(hr)) {
        SafeRelease(enumerator);
        return hr;
    }

    UINT count = 0;
    collection->GetCount(&count);
    for (UINT index = 0; index < count; ++index) {
        IMMDevice* device = nullptr;
        IPropertyStore* properties = nullptr;
        PROPVARIANT name{};
        PropVariantInit(&name);
        if (SUCCEEDED(collection->Item(index, &device)) &&
            SUCCEEDED(device->OpenPropertyStore(STGM_READ, &properties)) &&
            SUCCEEDED(properties->GetValue(PKEY_Device_FriendlyName, &name)) &&
            name.vt == VT_LPWSTR && name.pwszVal != nullptr &&
            IsDualSenseRenderEndpointName(name.pwszVal)) {
            LogFormat(L"DualSense audio endpoint selected name=\"%s\"", name.pwszVal);
            *result = device;
            (*result)->AddRef();
            PropVariantClear(&name);
            SafeRelease(properties);
            SafeRelease(device);
            break;
        }
        PropVariantClear(&name);
        SafeRelease(properties);
        SafeRelease(device);
    }

    SafeRelease(collection);
    SafeRelease(enumerator);
    return *result != nullptr ? S_OK : HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
}

struct FootstepProfile {
    float amplitude;
    float duration;
    double baseFrequency;
    double harmonicFrequency;
    float harmonicMix;
    float textureMix;
    float attackSeconds;
    float decayRate;
};

FootstepProfile ProfileForSurface(uint32_t surface) {
    switch (surface) {
    case 1: // Water00
        return {0.102f, 0.120f, 66.0, 139.0, 0.24f, 0.34f, 0.017f, 2.75f};
    case 2: // Grass00
    case 3: // Grass01
    case 5: // Leaves00
    case 6: // Ivy00
    case 21: // Moss00
        return {0.072f, 0.090f, 56.0, 91.0, 0.18f, 0.25f, 0.019f, 3.15f};
    case 4: // Bark00
    case 15: // Wood00
    case 16: // Wood01
        return {0.096f, 0.082f, 71.0, 124.0, 0.29f, 0.055f, 0.014f, 3.45f};
    case 7: // Soil00
        return {0.084f, 0.096f, 58.0, 88.0, 0.16f, 0.08f, 0.019f, 3.00f};
    case 8: // Mud00
        return {0.090f, 0.118f, 50.0, 76.0, 0.15f, 0.12f, 0.022f, 2.55f};
    case 9: // Tar
        return {0.086f, 0.105f, 53.0, 81.0, 0.17f, 0.08f, 0.021f, 2.75f};
    case 10: // Sand00
        return {0.074f, 0.104f, 52.0, 84.0, 0.16f, 0.15f, 0.021f, 2.85f};
    case 11: // gravel00
        return {0.090f, 0.088f, 69.0, 148.0, 0.25f, 0.29f, 0.017f, 3.20f};
    case 12: // Rock00
    case 17: // Stone00
        return {0.103f, 0.078f, 77.0, 128.0, 0.22f, 0.035f, 0.014f, 3.65f};
    case 13: // amber00
    case 14: // Cursed00
        return {0.101f, 0.083f, 82.0, 148.0, 0.27f, 0.075f, 0.014f, 3.45f};
    case 18: // Rooftile00
        return {0.103f, 0.074f, 86.0, 151.0, 0.27f, 0.045f, 0.013f, 3.75f};
    case 19: // Iron00
        return {0.094f, 0.066f, 93.0, 171.0, 0.31f, 0.025f, 0.012f, 4.00f};
    case 20: // Rug00
    case 22: // Tatami00
        return {0.066f, 0.100f, 47.0, 71.0, 0.13f, 0.035f, 0.023f, 2.85f};
    default:
        return {0.090f, 0.086f, 65.0, 106.0, 0.20f, 0.045f, 0.017f, 3.25f};
    }
}

struct FootstepVoice {
    FootstepProfile profile;
    uint32_t surface;
    uint32_t side;
    uint32_t stepType;
    float pace;
    uint32_t frame;
    uint32_t totalFrames;
    double basePhase;
    double harmonicPhase;
    float filteredNoise;
    uint32_t noiseState;
};

struct PlayerReactionVoice {
    PlayerHapticProfile profile;
    float intensity;
    uint32_t frame;
    uint32_t totalFrames;
    double phaseA;
    double phaseB;
    double modPhase;
    float filteredNoise;
    uint32_t noiseState;
};

struct CombatHapticVoice {
    CombatHapticProfile profile;
    float intensity;
    uint8_t motionVariant;
    uint32_t frame;
    uint32_t totalFrames;
    double phaseA;
    double phaseB;
};

struct EnvironmentInteractionVoice {
    EnvironmentInteractionProfile profile;
    float intensity;
    uint32_t frame;
    uint32_t totalFrames;
    double phaseA;
    double phaseB;
    double modPhase;
    float filteredNoise;
    uint32_t noiseState;
    float outputPeakLeft{};
    float outputPeakRight{};
    double outputEnergy{};
};

struct EnvironmentRenderDiagnostic {
    EnvironmentInteractionVoice voice;
    const wchar_t* outcome;
};

struct RainDropVoice {
    uint32_t frame{};
    uint32_t totalFrames{};
    float strength{};
    float pan{};
    double phase{};
    double frequency{};
};

constexpr bool IsTreasureChestProfile(EnvironmentInteractionProfile profile) {
    return profile >= EnvironmentInteractionProfile::TreasureChestOpen &&
        profile <= EnvironmentInteractionProfile::TreasureChestLidLand;
}

struct SpeakerCueVoice {
    ControllerSpeakerCue cue;
    uint32_t frame;
    uint32_t totalFrames;
    double phaseA;
    double phaseB;
    float filteredNoise;
    uint32_t noiseState;
    uint32_t delayFrames;
};

struct SpeakerFileVoice {
    size_t clipIndex;
    size_t frame;
    float gain;
    ControllerSpeakerCue cue;
    uint32_t delayFrames;
    bool submitted = false;
    combat_defense_feedback::DeflectSpeakerVariant deflectVariant =
        combat_defense_feedback::DeflectSpeakerVariant::Short;
};

struct CapturedPresetVoice {
    captured_haptic_presets::Preset preset;
    uint32_t frame;
    uint32_t totalFrames;
};

class AudioHaptics {
public:
    ~AudioHaptics() { Stop(); }

    bool Start(const std::wstring& dataDirectory) {
        if (m_thread.joinable()) return m_started.load();
        m_dataDirectory = dataDirectory;

        bool allLoaded = true;
        for (size_t index = 0; index < m_clips.size(); ++index) {
            const std::wstring path = dataDirectory + L"\\haptic_" +
                std::to_wstring(index + 1) + L".wav";
            if (!LoadHapticWav(path, m_clips[index])) {
                LogFormat(L"Failed to load haptic waveform: %s", path.c_str());
                allLoaded = false;
            }
        }
        if (!allLoaded) return false;

        constexpr std::array<const wchar_t*, 4> SpeakerFiles{
            L"soul_start.wav",
            L"soul_loop.wav",
            L"soul_collect.wav",
            L"soul_stop.wav",
        };
        bool speakerLoaded = true;
        for (size_t index = 0; index < m_speakerClips.size(); ++index) {
            const std::wstring path = dataDirectory + L"\\" + SpeakerFiles[index];
            if (!LoadSpeakerWav(path, m_speakerClips[index])) {
                LogFormat(L"Failed to load original soul speaker audio: %s", path.c_str());
                speakerLoaded = false;
            }
        }
        m_speakerAudioLoaded = speakerLoaded;
        Log(speakerLoaded
            ? L"Original soul speaker audio loaded from local game assets"
            : L"Original soul speaker audio unavailable; using synthesized fallback");

        constexpr std::array<const wchar_t*, 18> CombatSpeakerFiles{
            L"combat_clash_start.wav",
            L"combat_clash_hit_a.wav",
            L"combat_clash_hit_b.wav",
            L"combat_clash_hit_c.wav",
            L"combat_clash_success_metal.wav",
            L"combat_clash_success_layer.wav",
            L"combat_execution_issen_a.wav",
            L"combat_execution_issen_b.wav",
            L"combat_execution_break_issen.wav",
            L"combat_deflect_original.wav",
            L"combat_guard_main_a.wav",
            L"combat_guard_main_b.wav",
            L"combat_guard_main_c.wav",
            L"combat_guard_directional_a.wav",
            L"combat_guard_directional_b.wav",
            L"combat_guard_directional_c.wav",
            // Melee Repel: native BLOCK default trigger 3893406318,
            // hit_cmn event 1169882926, not the PARRY/Deflect event.
            // Projectile retains the already-tested speaker-band preset.
            L"combat_parry_blade_speaker.wav",
            L"combat_parry_projectile_speaker.wav",
        };
        bool combatSpeakerLoaded = true;
        for (size_t index = 0; index < CombatSpeakerFiles.size(); ++index) {
            const std::wstring path =
                dataDirectory + L"\\" + CombatSpeakerFiles[index];
            if (!LoadSpeakerWav(path, m_combatSpeakerClips[index])) {
                LogFormat(L"Failed to load original combat speaker audio: %s", path.c_str());
                combatSpeakerLoaded = false;
            }
        }
        m_combatSpeakerAudioLoaded = combatSpeakerLoaded;
        Log(combatSpeakerLoaded
            ? L"Original combat speaker audio loaded from local game assets"
            : L"Original combat speaker audio incomplete; using synthesized fallback");

        // Optional to existing combat audio: an absent new asset must not
        // break all other clips, nor silently substitute the WRONG short cue.
        m_slowDeflectSpeakerAudioLoaded = LoadSpeakerWav(
            dataDirectory + L"\\combat_deflect_slow_original.wav",
            m_combatSpeakerClips[18]);
        Log(m_slowDeflectSpeakerAudioLoaded
            ? L"Slow Deflect original audio loaded: hit_cmn event 3991122447"
            : L"Slow Deflect original audio missing; slow cue muted (no short substitute)");

        m_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        m_readyEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        m_bufferEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (m_stopEvent == nullptr || m_readyEvent == nullptr || m_bufferEvent == nullptr) {
            Log(L"Could not create haptics synchronization events");
            CloseEvents();
            return false;
        }
        m_thread = std::thread([this]() { ThreadMain(); });
        if (WaitForSingleObject(m_readyEvent, 5000) != WAIT_OBJECT_0 || !m_started.load()) {
            Log(L"DualSense haptics audio endpoint failed to start");
            Stop();
            return false;
        }
        Log(m_bluetoothMode.load()
            ? L"Bluetooth stream ready: stereo 3kHz PCM haptics + Opus speaker; equivalence unverified"
            : L"DualSense haptics audio ready: 4ch/48kHz float");
#ifdef ONIMUSHA_BLUETOOTH_ENABLED
        if (m_bluetoothMode.load()) {
            LogFormat(L"Bluetooth speaker-only compensation: %.2f dB scale=%.6f (provisional); USB/haptics unchanged",
                static_cast<double>(dualsense_bluetooth::SpeakerOutputCompensationDb),
                static_cast<double>(dualsense_bluetooth::SpeakerOutputScale));
        }
#endif
        return true;
    }

    void Stop() {
        if (m_stopEvent != nullptr) SetEvent(m_stopEvent);
        if (m_thread.joinable()) m_thread.join();
        m_started.store(false);
        StopAll();
        CloseEvents();
    }

    bool IsStarted() const {
        return m_started.load();
    }

    void Play(
        size_t oneBasedIndex,
        float leftGain,
        float rightGain,
        float delaySeconds = 0.0f) {
        if (!m_started.load() || oneBasedIndex == 0 || oneBasedIndex > m_clips.size()) return;
        const uint32_t delayFrames = static_cast<uint32_t>(std::clamp(
            delaySeconds * 48000.0f,
            0.0f,
            240000.0f));
        std::lock_guard lock{m_voiceMutex};
        if (m_voices.size() >= 16) m_voices.erase(m_voices.begin());
        m_voices.push_back({
            oneBasedIndex - 1,
            0,
            delayFrames,
            std::clamp(leftGain, 0.0f, 6.0f),
            std::clamp(rightGain, 0.0f, 6.0f)});
    }

    void PlayTitle(unsigned cue) {
        if (!m_started.load() || cue >= 5) return;
        std::lock_guard lock{m_voiceMutex};
        // Fade interrupted cues over 6 ms; never build an input backlog.
        for (auto& voice : m_titleVoices) voice.release = std::min(voice.release, 288u);
        if (m_titleVoices.size() >= 4) return;
        m_titleVoices.push_back({cue, 0, 0xFFFFFFFFu});
        LogFormat(L"Title haptic cue=%u duration=%.3f", cue, title_haptics::Duration(cue));
    }

    void StopAll(
        bool preserveCombatSpeakerVoices = false,
        bool preserveNormalAttack = false,
        bool preserveExecution = false,
        bool preserveTreasureChest = false) {
        std::lock_guard lock{m_voiceMutex};
        m_voices.clear();
        m_titleVoices.clear();
        m_liveMotorTargets.Clear();
        m_motorPower.fill(0.0f);
        m_rainTarget = 0.0f;
        m_windTarget = 0.0f;
        m_rainLevel = 0.0f;
        m_windLevel = 0.0f;
        m_rainDropCountdown = 0;
        for (auto& drop : m_rainDrops) drop = {};
        if (preserveTreasureChest) {
            m_environmentInteractions.erase(std::remove_if(
                m_environmentInteractions.begin(),
                m_environmentInteractions.end(),
                [](const EnvironmentInteractionVoice& voice) {
                    if (IsTreasureChestProfile(voice.profile)) return false;
                    return true;
                }), m_environmentInteractions.end());
        } else {
            for (const auto& voice : m_environmentInteractions) {
                if (IsTreasureChestProfile(voice.profile))
                    QueueEnvironmentDiagnosticLocked(voice, L"cancelled-stopAll");
            }
            m_environmentInteractions.clear();
        }
        m_elevatorTarget = 0.0f;
        m_elevatorLevel = 0.0f;
        m_elevatorHoldFrames = 0;
        m_mapZoomTarget = 0.0f;
        m_mapZoomLevel = 0.0f;
        m_mapZoomHoldFrames = 0;
        m_longPressTarget = 0.0f;
        m_longPressLevel = 0.0f;
        m_longPressProgress = 0.0f;
        m_longPressActive = false;
        m_longPressEquipmentProfile = false;
        m_longPressHoldFrames = 0;
        m_longPressStartFrames = 0;
        m_longPressCompleteFrames = 0;
        m_longPressCancelFrames = 0;
        m_oniStrengthFrames = 0;
        m_oniStrengthProfile = 0;
        m_oniEyeFrames = 0;
        m_oniTransformationFrames = 0;
        if (!preserveNormalAttack) {
            m_oniChargeReleaseFrames = 0;
            m_oniChargeReleaseDelayFrames = 0;
            m_oniChargeReleaseFrame = 0;
        }
        m_wallRunRenderer.Stop();
        m_ladderRenderer.Stop();
        if (!preserveNormalAttack) m_normalAttackRenderer.Stop();
        m_capturedPresetVoices.clear();
        if (!preserveTreasureChest) {
            for (auto& pulses : m_motorPulses) pulses.clear();
        }
        m_footsteps.clear();
        m_playerReactions.clear();
        if (preserveExecution) {
            m_combatHaptics.erase(std::remove_if(
                m_combatHaptics.begin(), m_combatHaptics.end(),
                [](const CombatHapticVoice& voice) {
                    return execution_haptics::KindFromProfile(
                        static_cast<uint8_t>(voice.profile)) ==
                        execution_haptics::Kind::None;
                }), m_combatHaptics.end());
        } else {
            m_combatHaptics.clear();
        }
        m_speakerCues.clear();
        m_speakerFileVoices.clear();
        // The game commonly calls stopAllVibration immediately after a short
        // impact event. Repel speaker cues intentionally start a few frames
        // after that event, so clearing them here makes a successfully queued
        // cue silent in gameplay even though isolated playback works. Keep
        // confirmed one-shot combat audio alive for game-originated haptic
        // stops; shutdown, licence loss and the speaker master switch still
        // use the default full clear.
        if (!preserveCombatSpeakerVoices) {
            m_combatSpeakerFileVoices.clear();
        }
        m_gauntletVoiceActive = false;
        m_gauntletVoiceFrame = 0;
        m_soulSpeakerTarget = 0.0f;
        m_soulSpeakerLevel = 0.0f;
        m_soulLoopFrame = 0;
        if (preserveTreasureChest) {
            Log(L"StopAll preserved recent treasure chest haptics");
        }
        m_soulHapticTarget = 0.0f;
        m_soulHapticLevel = 0.0f;
        m_soulHapticHoldFrames = 0;
        m_soulHapticStartFrames = 0;
        m_soulHapticAccentFrames = 0;
        m_blackSoulTarget = 0.0f;
        m_blackSoulLevel = 0.0f;
        m_blackSoulHoldFrames = 0;
        m_blackSoulFrame = 0;
        m_blackSoulFinishFrames = 0;
        m_blackSoulFinishIntensity = 0.0f;
        m_soulSearchTarget = 0.0f;
        m_soulSearchLevel = 0.0f;
        m_soulSearchHoldFrames = 0;
        m_soulSearchPhase = 0.0;
        m_soulSearchHarmonicPhase = 0.0;
        m_soulSearchModPhase = 0.0;
        m_sprintTarget = 0.0f;
        m_sprintLevel = 0.0f;
        m_sprintHoldFrames = 0;
        m_sprintStartFrames = 0;
        m_sprintPressFrames = 0;
        m_sprintPressIntensity = 0.0f;
        m_sprintPressIsSlowdown = false;
        m_sprintPressPhase = 0.0;
        m_sprintPressHarmonicPhase = 0.0;
    }

    void StopSpeakerAudio() {
        std::lock_guard lock{m_voiceMutex};
        m_speakerCues.clear();
        m_speakerFileVoices.clear();
        m_combatSpeakerFileVoices.clear();
        m_gauntletVoiceActive = false;
        m_gauntletVoiceFrame = 0;
        m_soulSpeakerTarget = 0.0f;
        m_soulSpeakerLevel = 0.0f;
        m_soulLoopFrame = 0;
    }

    void StopWaveforms() {
        std::lock_guard lock{m_voiceMutex};
        m_voices.clear();
    }

    void StopMotors(bool preserveTreasureChest = false) {
        std::lock_guard lock{m_voiceMutex};
        m_liveMotorTargets.Clear();
        m_motorPower.fill(0.0f);
        m_capturedPresetVoices.clear();
        if (!preserveTreasureChest) {
            for (auto& pulses : m_motorPulses) pulses.clear();
        } else {
            Log(L"StopMotors preserved recent treasure chest haptics");
        }
    }

    void SetMotorPower(size_t motor, float power) {
        if (!m_started.load() || motor >= 4) return;
        std::lock_guard lock{m_voiceMutex};
        if (power > 0.0f && ExecutionActiveLocked()) {
            m_suppressedExecutionMotorMessages.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (power > 0.0f && RepelImpactActiveLocked()) {
            m_suppressedRepelMotorMessages.fetch_add(1, std::memory_order_relaxed);
            LogFormat(L"Repel impact suppressed live motor=%zu power=%.3f", motor, power);
            return;
        }
        m_liveMotorTargets.Set(motor, power, GetTickCount64());
    }

    void SetEnvironment(float rainLevel, float windLevel) {
        if (!m_started.load()) return;
        std::lock_guard lock{m_voiceMutex};
        const float nextRain = std::clamp(rainLevel, 0.0f, 1.0f);
        m_rainTarget = nextRain;
        m_windTarget = std::clamp(windLevel, 0.0f, 1.0f);
        if (nextRain <= 0.0f) {
            // Menu/pause transitions must become silent immediately. Keeping
            // the old smoothing tail here was the reason rain continued under
            // the interface after the bridge had already sent zero.
            m_rainLevel = 0.0f;
            m_rainDropCountdown = 0;
            for (auto& drop : m_rainDrops) drop = {};
        }
    }

    void SetSpeakerVolumes(const product_runtime::Settings& settings) {
        std::lock_guard lock{m_voiceMutex};
        const bool normalAttackChanged = !m_normalAttackRenderer.Enabled();
        m_normalAttackRenderer.SetEnabled(product_runtime::NormalAttackHapticsAlwaysEnabled);
        if (normalAttackChanged)
            LogFormat(L"Normal attack haptics always enabled; gameplay gate=fighting-spirit-type1");
        m_gauntletVoiceVolume = product_runtime::CalibratedSpeakerScale(
            settings.gauntletVoiceVolume, product_runtime::GauntletSpeakerBaseline);
        m_soulSpeakerVolume = product_runtime::CalibratedSpeakerScale(
            settings.soulSpeakerVolume, product_runtime::SoulSpeakerBaseline);
        m_clashSpeakerVolume = std::clamp(
            settings.clashSpeakerVolume, 0.0f, 2.0f);
        m_guardSpeakerVolume = std::clamp(
            settings.guardSpeakerVolume, 0.0f, 2.0f);
        m_parrySpeakerVolume = product_runtime::CalibratedSpeakerScale(
            settings.parrySpeakerVolume, product_runtime::ParrySpeakerBaseline);
        m_deflectSpeakerVolume = std::clamp(
            settings.deflectSpeakerVolume, 0.0f, 2.0f);
        m_executionSpeakerVolume = std::clamp(
            settings.executionSpeakerVolume, 0.0f, 2.0f);
        m_issenSpeakerVolume = std::clamp(
            settings.issenSpeakerVolume, 0.0f, 2.0f);
    }

    void SetHapticIntensity(float intensity) {
        std::lock_guard lock{m_voiceMutex};
        m_hapticIntensity = std::clamp(intensity, 0.0f, 2.0f);
    }

    void PlayNormalAttack(normal_attack_haptics::Kind kind, normal_attack_haptics::Operation operation,
        uint32_t swingLeadFrames = 0u, uint32_t secondarySwingLeadFrames = 0u) {
        if (!m_started.load()) return;
        std::lock_guard lock{m_voiceMutex};
        using normal_attack_haptics::Operation;
        if (operation == Operation::Cancel) { m_normalAttackRenderer.Stop(); return; }
        if (!m_normalAttackRenderer.Enabled()) return;
        switch (operation) {
        case Operation::Start: m_normalAttackRenderer.Play(kind, swingLeadFrames, secondarySwingLeadFrames); break;
        case Operation::Refresh: m_normalAttackRenderer.Refresh(kind); break;
        case Operation::End: m_normalAttackRenderer.End(); break;
        case Operation::Hit: m_normalAttackRenderer.Hit(kind); break;
        case Operation::ChargeStart: m_normalAttackRenderer.StartCharge(kind); break;
        case Operation::ChargeRefresh: m_normalAttackRenderer.RefreshCharge(kind); break;
        case Operation::ChargeRelease: m_normalAttackRenderer.ReleaseCharge(kind); break;
        case Operation::ChargeCancel: m_normalAttackRenderer.CancelCharge(); break;
        case Operation::ChargeHit: m_normalAttackRenderer.Hit(kind); break;
        case Operation::OrdinaryLightWaveSmall: m_normalAttackRenderer.PlayOrdinaryLightWave(false); break;
        case Operation::OrdinaryLightWaveLarge: m_normalAttackRenderer.PlayOrdinaryLightWave(true); break;
        case Operation::OrdinaryHeavyDownstroke: m_normalAttackRenderer.PlayOrdinaryHeavyDownstroke(); break;
        case Operation::OrdinaryHeavyGroundCrack: m_normalAttackRenderer.PlayOrdinaryHeavyGroundCrack(); break;
        default: return;
        }
        if (operation != Operation::Refresh && operation != Operation::ChargeRefresh)
            LogFormat(L"Normal attack haptic kind=%u operation=%u leadFrames=%u secondaryLeadFrames=%u",
                static_cast<unsigned>(kind), static_cast<unsigned>(operation), swingLeadFrames, secondarySwingLeadFrames);
    }

    void SetWallRunHaptic(wall_run_haptics::Event event,
        wall_run_haptics::Side side, float intensity) {
        if (!m_started.load()) return;
        std::lock_guard lock{m_voiceMutex};
        m_wallRunRenderer.Apply(event, side, intensity);
        if (event != wall_run_haptics::Event::Move && event != wall_run_haptics::Event::AirMove) {
            LogFormat(L"Wall Run haptic event=%u side=%u intensity=%.2f",
                static_cast<unsigned>(event), static_cast<unsigned>(side), intensity);
        }
    }

    void SetContinuousPlayerHaptic(
        ContinuousPlayerHaptic profile,
        ContinuousHapticOperation operation,
        float intensity,
        ladder_haptics::Side side = ladder_haptics::Side::Center) {
        if (!m_started.load()) return;
        intensity = std::clamp(intensity, 0.0f, 1.0f);
        std::lock_guard lock{m_voiceMutex};

        if (profile == ContinuousPlayerHaptic::LadderClimb ||
            profile == ContinuousPlayerHaptic::LadderFastSlide) {
            if (operation == ContinuousHapticOperation::Stop) {
                m_ladderRenderer.Stop();
                Log(L"Ladder haptic stopped");
                return;
            }
            const auto ladderProfile = profile ==
                ContinuousPlayerHaptic::LadderFastSlide
                ? ladder_haptics::Profile::FastSlide
                : ladder_haptics::Profile::Climb;
            if (operation <= ContinuousHapticOperation::Decelerate) {
                m_ladderRenderer.Apply(
                    ladderProfile,
                    static_cast<ladder_haptics::Operation>(operation),
                    side,
                    intensity);
                if (operation != ContinuousHapticOperation::Refresh) {
                    LogFormat(L"Ladder haptic profile=%u operation=%u side=%u intensity=%.2f",
                        static_cast<unsigned>(ladderProfile),
                        static_cast<unsigned>(operation),
                        static_cast<unsigned>(side),
                        intensity);
                }
            }
            return;
        }

        if (profile == ContinuousPlayerHaptic::BlackSoulAbsorption) {
            if (operation == ContinuousHapticOperation::Stop) {
                m_blackSoulTarget = 0.0f;
                m_blackSoulHoldFrames = 0;
                m_blackSoulFinishFrames = 0;
                m_blackSoulFinishIntensity = 0.0f;
                Log(L"Black soul absorption haptic stopped");
                return;
            }
            if (operation == ContinuousHapticOperation::Accent) {
                m_blackSoulTarget = 0.0f;
                m_blackSoulHoldFrames = 0;
                m_blackSoulFinishFrames = black_soul_haptics::FinishFrames;
                m_blackSoulFinishIntensity = intensity;
                LogFormat(L"Black soul absorption completed settle=240ms intensity=%.2f",
                    intensity);
                return;
            }
            if (operation != ContinuousHapticOperation::Start &&
                operation != ContinuousHapticOperation::Refresh) return;
            m_blackSoulTarget = intensity;
            m_blackSoulHoldFrames = black_soul_haptics::HoldFrames;
            m_blackSoulFinishFrames = 0;
            m_blackSoulFinishIntensity = 0.0f;
            m_soulSearchTarget = 0.0f;
            m_soulSearchLevel = 0.0f;
            m_soulSearchHoldFrames = 0;
            if (operation == ContinuousHapticOperation::Start) {
                m_blackSoulFrame = 0;
                LogFormat(L"Black soul absorption haptic started intensity=%.2f",
                    intensity);
            }
            return;
        }

        if (profile == ContinuousPlayerHaptic::SoulAbsorption) {
            if (operation == ContinuousHapticOperation::Stop) {
                m_soulHapticTarget = 0.0f;
                m_soulHapticHoldFrames = 0;
                Log(L"Soul absorption haptic stopped");
                return;
            }
            if (operation == ContinuousHapticOperation::Accent) {
                if (m_soulHapticLevel > 0.01f ||
                    m_soulHapticTarget > 0.01f) {
                    m_soulHapticAccentFrames = 4800;
                }
                return;
            }

            m_soulHapticTarget = intensity;
            // A real soul stream always supersedes the empty-action texture.
            m_soulSearchTarget = 0.0f;
            m_soulSearchLevel = 0.0f;
            m_soulSearchHoldFrames = 0;
            // The bridge refreshes every 200 ms. Self-expiration prevents a
            // missed release callback from leaving a permanent vibration.
            m_soulHapticHoldFrames = 19200;
            if (operation == ContinuousHapticOperation::Start) {
                m_soulHapticStartFrames = 7200;
                m_soulHapticPhase = 0.0;
                m_soulHapticHarmonicPhase = 0.0;
                m_soulHapticModPhase = 0.0;
                LogFormat(
                    L"Soul absorption haptic started intensity=%.2f",
                    intensity);
            }
            return;
        }

        if (profile == ContinuousPlayerHaptic::SoulSearch) {
            if (operation == ContinuousHapticOperation::Stop) {
                m_soulSearchTarget = 0.0f;
                m_soulSearchHoldFrames = 0;
                Log(L"Empty soul action haptic stopped");
                return;
            }
            m_soulSearchTarget = intensity;
            // Bridge refreshes every 200 ms; expire safely if release is lost.
            m_soulSearchHoldFrames = 19200;
            if (operation == ContinuousHapticOperation::Start) {
                m_soulSearchPhase = 0.0;
                m_soulSearchHarmonicPhase = 0.0;
                m_soulSearchModPhase = 0.0;
                LogFormat(
                    L"Empty soul action haptic started intensity=%.2f",
                    intensity);
            }
            return;
        }

        if (profile == ContinuousPlayerHaptic::Sprint) {
            if (operation == ContinuousHapticOperation::Stop) {
                m_sprintTarget = 0.0f;
                m_sprintHoldFrames = 0;
                Log(L"Sprint haptic stopped");
                return;
            }
            if (operation == ContinuousHapticOperation::Accent) {
                // Keep the physical L3 acknowledgement independent from the
                // running bed. Use a broad low-frequency body rather than a
                // crisp tactile click, then fall into the continuous texture.
                m_sprintPressFrames = 10560;
                m_sprintPressIntensity = intensity;
                m_sprintPressIsSlowdown = false;
                m_sprintPressPhase = 0.0;
                m_sprintPressHarmonicPhase = 0.0;
                LogFormat(L"Sprint L3 accent intensity=%.2f", intensity);
                return;
            }
            if (operation == ContinuousHapticOperation::Decelerate) {
                // Pulling the stick back from sprint returns the character to
                // slow movement. Use a short, soft low-frequency braking fall
                // instead of replaying the heavy forward launch.
                m_sprintPressFrames = 5760;
                m_sprintPressIntensity = intensity;
                m_sprintPressIsSlowdown = true;
                m_sprintPressPhase = 0.0;
                m_sprintPressHarmonicPhase = 0.0;
                LogFormat(L"Sprint L3 slowdown intensity=%.2f", intensity);
                return;
            }

            m_sprintTarget = intensity;
            // The bridge refreshes active sprint state every 160 ms and owns
            // the cadence timeout. This wider helper margin prevents an audio
            // buffer or irregular animation step from dropping the bed early.
            m_sprintHoldFrames = 67200;
            if (operation == ContinuousHapticOperation::Start) {
                m_sprintStartFrames = 8640;
                m_sprintPhase = 0.0;
                m_sprintHarmonicPhase = 0.0;
                m_sprintModPhase = 0.0;
                LogFormat(L"Sprint haptic started intensity=%.2f", intensity);
            }
        }
    }

    void PlayEnvironmentInteraction(
        EnvironmentInteractionProfile profile,
        float intensity) {
        if (!m_started.load()) return;
        intensity = std::clamp(intensity, 0.0f, 1.0f);
        std::lock_guard lock{m_voiceMutex};

        if (profile == EnvironmentInteractionProfile::ElevatorMove) {
            m_elevatorTarget = intensity;
            // The bridge refreshes this from the actual elevator update state.
            // A short self-expiring hold prevents stuck vibration after a scene
            // transition, missed stop callback, or controller reconnect.
            m_elevatorHoldFrames = 9600;
            return;
        }

        float duration = 0.20f;
        switch (profile) {
        case EnvironmentInteractionProfile::GenericSuccess:
            duration = 0.20f;
            break;
        case EnvironmentInteractionProfile::Door:
            duration = 0.56f;
            break;
        case EnvironmentInteractionProfile::Lever:
            duration = 0.34f;
            break;
        case EnvironmentInteractionProfile::ElevatorStart:
            duration = 0.34f;
            m_elevatorTarget = intensity * 0.88f;
            m_elevatorHoldFrames = 14400;
            break;
        case EnvironmentInteractionProfile::ElevatorStop:
            duration = 0.40f;
            m_elevatorTarget = 0.0f;
            m_elevatorHoldFrames = 0;
            break;
        case EnvironmentInteractionProfile::TreasureChestOpen:
            duration = treasure_chest_haptics::DurationSeconds(
                treasure_chest_haptics::Stage::Open);
            break;
        case EnvironmentInteractionProfile::TreasureChestPush:
            duration = treasure_chest_haptics::DurationSeconds(
                treasure_chest_haptics::Stage::Push);
            break;
        case EnvironmentInteractionProfile::TreasureChestLidLand:
            duration = treasure_chest_haptics::DurationSeconds(
                treasure_chest_haptics::Stage::LidLand);
            break;
        default:
            return;
        }

        if (profile == EnvironmentInteractionProfile::TreasureChestOpen) {
            // A newly opened chest owns a fresh three-stage sequence. Do not
            // let a stale safety-timeout voice from another chest bleed in.
            m_environmentInteractions.erase(std::remove_if(
                m_environmentInteractions.begin(),
                m_environmentInteractions.end(),
                [](const EnvironmentInteractionVoice& voice) {
                    return IsTreasureChestProfile(voice.profile);
                }), m_environmentInteractions.end());
        } else if (profile ==
                EnvironmentInteractionProfile::TreasureChestLidLand) {
            // OPENED/LAND is the authoritative end of the sustained lid push.
            // Cancel it immediately before adding the physical impact.
            m_environmentInteractions.erase(std::remove_if(
                m_environmentInteractions.begin(),
                m_environmentInteractions.end(),
                [](const EnvironmentInteractionVoice& voice) {
                    return voice.profile ==
                        EnvironmentInteractionProfile::TreasureChestPush;
                }), m_environmentInteractions.end());
        }

        if (m_environmentInteractions.size() >= 12) {
            QueueEnvironmentDiagnosticLocked(m_environmentInteractions.front(), L"cancelled-capacity");
            m_environmentInteractions.erase(m_environmentInteractions.begin());
        }
        // The gameplay trace sends GenericInteract 140 ms after opening and
        // GetItem at the same instant as OPENED. Remove an earlier copy when
        // callback order is reversed; PlayCapturedPreset handles later copies.
        if (profile == EnvironmentInteractionProfile::TreasureChestOpen ||
            profile == EnvironmentInteractionProfile::TreasureChestLidLand) {
            const auto redundantPreset = profile == EnvironmentInteractionProfile::TreasureChestOpen
                ? captured_haptic_presets::Preset::GenericInteract
                : captured_haptic_presets::Preset::GetItem;
            m_capturedPresetVoices.erase(std::remove_if(
                m_capturedPresetVoices.begin(), m_capturedPresetVoices.end(),
                [redundantPreset](const CapturedPresetVoice& voice) {
                    return voice.preset == redundantPreset;
                }), m_capturedPresetVoices.end());
        }
        const uint32_t frames = static_cast<uint32_t>(duration * 48000.0f);
        m_environmentInteractions.push_back({
            profile,
            intensity,
            0,
            frames,
            0.0,
            0.0,
            0.0,
            0.0f,
            0x9E3779B9u ^
                (static_cast<uint32_t>(profile) * 2246822519u) ^
                static_cast<uint32_t>(GetTickCount64())});
        LogFormat(
            L"Environment interaction haptic queued profile=%u intensity=%.2f revision=chest-output-r2-pcm-only",
            static_cast<unsigned>(profile),
            intensity);
    }

    void SetMapZoom(float direction, float intensity) {
        if (!m_started.load()) return;
        std::lock_guard lock{m_voiceMutex};
        m_mapZoomDirection = direction >= 0.0f ? 1.0f : -1.0f;
        m_mapZoomTarget = std::clamp(intensity, 0.0f, 1.0f);
        // Bridge updates arrive once per rendered frame while the scale is
        // changing. The short hold bridges frame gaps and then releases
        // automatically when zooming stops or reaches an endpoint.
        m_mapZoomHoldFrames = 4320;
    }

    void SetLongPress(uint8_t operation, float progress, uint8_t profile) {
        if (!m_started.load()) return;
        std::lock_guard lock{m_voiceMutex};
        progress = std::clamp(progress, 0.0f, 1.0f);
        m_longPressEquipmentProfile = profile == 1;
        if (operation == 1) {
            const bool newlyActive = !m_longPressActive;
            m_longPressActive = true;
            m_longPressProgress = progress;
            // Maintain a faint contact at the start, then let actual circle
            // progress control both carrier pitch and strength.
            m_longPressTarget = m_longPressEquipmentProfile
                ? 0.24f + progress * 0.76f
                : 0.10f + progress * 0.90f;
            m_longPressHoldFrames = 4800;
            if (newlyActive) {
                m_longPressStartFrames = 2880;
                m_longPressCompleteFrames = 0;
                m_longPressCancelFrames = 0;
                m_longPressPhase = 0.0;
                m_longPressHarmonicPhase = 0.0;
                m_longPressModPhase = 0.0;
                m_longPressAccentPhase = 0.0;
                LogFormat(
                    L"Long press started progress=%.3f profile=%u",
                    progress,
                    static_cast<unsigned>(profile));
            }
        } else if (operation == 2) {
            m_longPressActive = false;
            m_longPressTarget = 0.0f;
            m_longPressHoldFrames = 0;
            m_longPressStartFrames = 0;
            m_longPressCancelFrames = 2880;
            m_longPressCompleteFrames = 0;
            m_longPressAccentPhase = 0.0;
            LogFormat(L"Long press cancelled progress=%.3f", progress);
        } else if (operation == 3) {
            m_longPressActive = false;
            m_longPressTarget = 0.0f;
            m_longPressProgress = 1.0f;
            m_longPressHoldFrames = 0;
            m_longPressStartFrames = 0;
            m_longPressCancelFrames = 0;
            m_longPressCompleteFrames = 7200;
            m_longPressAccentPhase = 0.0;
            Log(L"Long press completed");
        }
    }

    void PlayOniStrength(uint8_t profile) {
        if (!m_started.load()) return;
        std::lock_guard lock{m_voiceMutex};
        // Profile 0 is the destroy skill: keep the wrist lift and both later
        // palm contacts on one action-synchronised timeline. Profile 1 retains
        // the shorter barrier-break texture.
        m_oniStrengthProfile = profile == 1 ? 1 : 0;
        m_oniStrengthFrames = m_oniStrengthProfile == 1
            ? 24960
            : oni_strength_haptics::DestroyDurationFrames;
        m_oniStrengthPhase = 0.0;
        m_oniStrengthHarmonicPhase = 0.0;
        LogFormat(L"Oni Strength haptic started profile=%u",
            static_cast<unsigned>(m_oniStrengthProfile));
    }

    void PlayOniEye() {
        if (!m_started.load()) return;
        std::lock_guard lock{m_voiceMutex};
        // Three increasingly wide pulses move across the two actuators with a
        // slight stereo delay, followed by a soft return. The rising carrier
        // makes the result read as an outward sonar scan rather than a hit.
        // Keep the three expanding layers inside the visible yellow scan. The
        // audio-haptics transport adds a small presentation delay, so author
        // the tactile sequence at 820 ms and place the outer crest early.
        m_oniEyeFrames = 39360;
        m_oniEyePhase = 0.0;
        m_oniEyeHarmonicPhase = 0.0;
        m_oniEyeSweepPhase = 0.0;
        Log(L"Oni Eye sonar haptic started");
    }

    void PlayOniTransformation() {
        if (!m_started.load()) return;
        std::lock_guard lock{m_voiceMutex};
        m_oniTransformationFrames = oni_transformation_haptics::DurationFrames;
        Log(L"Oni transformation haptic started");
    }

    void PlayOniChargeRelease(oni_charge_release_haptics::Kind kind) {
        if (!m_started.load()) return;
        std::lock_guard lock{m_voiceMutex};
        // End the continuous held-charge bed first. This dedicated voice is
        // intentionally outside the normal-attack renderer so later ordinary
        // refresh/end/hit traffic cannot overwrite the release.
        m_normalAttackRenderer.CancelCharge();
        m_oniChargeReleaseKind = kind;
        m_oniChargeReleaseFrames = oni_charge_release_haptics::Duration(kind);
        m_oniChargeReleaseDelayFrames = kind == oni_charge_release_haptics::Kind::FinisherDraw
            ? 23040u : (kind == oni_charge_release_haptics::Kind::Heavy
                ? 8640u : (kind == oni_charge_release_haptics::Kind::Light
                    ? 5760u : 0u)); // draw: 480 ms into the black-screen silence
        m_oniChargeReleaseFrame = 0;
        LogFormat(L"Oni charge dedicated release armed kind=%u delayFrames=%u frames=%u",
            static_cast<unsigned>(kind), m_oniChargeReleaseDelayFrames,
            m_oniChargeReleaseFrames);
    }

    void PlayBowShot(bow_shot_haptics::Kind kind) {
        if (!m_started.load()) return;
        std::lock_guard lock{m_voiceMutex};
        m_bowShotKind = kind;
        m_bowShotFrames = bow_shot_haptics::Duration(kind);
        m_bowShotFrame = 0;
        LogFormat(L"Bow shot haptic started kind=%u frames=%u",
            static_cast<unsigned>(kind), m_bowShotFrames);
    }

    void PlayFootstep(
        uint32_t surface,
        uint32_t side,
        uint32_t stepType,
        float pace) {
        if (!m_started.load()) return;
        pace = std::clamp(pace, 0.0f, 1.0f);
        FootstepProfile profile = ProfileForSurface(surface);
        float duration = profile.duration * (1.0f - pace * 0.10f);
        if (stepType == 3) duration *= 1.42f;
        const uint32_t frames = static_cast<uint32_t>(std::clamp(
            duration * 48000.0f,
            1440.0f,
            9600.0f));
        std::lock_guard lock{m_voiceMutex};
        if (m_footsteps.size() >= 12) m_footsteps.erase(m_footsteps.begin());
        m_footsteps.push_back({
            profile,
            surface,
            side & 1u,
            stepType,
            pace,
            0,
            frames,
            0.0,
            0.0,
            0.0f,
            0xA341316Cu ^ (surface * 747796405u) ^ (side * 2891336453u) ^
                static_cast<uint32_t>(GetTickCount64())});
    }

    void PlayPlayerReaction(PlayerHapticProfile profile, float intensity,
        float durationHintSeconds = 0.0f) {
        if (!m_started.load()) return;
        float duration = 0.14f;
        switch (profile) {
        case PlayerHapticProfile::Hit: duration = 0.13f; break;
        case PlayerHapticProfile::Stab: duration = 0.19f; break;
        case PlayerHapticProfile::Collision: duration = 0.22f; break;
        case PlayerHapticProfile::Knockdown: duration = 0.38f; break;
        case PlayerHapticProfile::Grab: duration = 0.66f; break;
        case PlayerHapticProfile::Dodge: duration = 0.17f; break;
        case PlayerHapticProfile::JustDodge: duration = 0.25f; break;
        case PlayerHapticProfile::CinematicImpact:
            duration = std::isfinite(durationHintSeconds) && durationHintSeconds > 0.0f
                ? std::clamp(durationHintSeconds, 0.36f, 1.25f)
                : 0.36f;
            break;
        default: return;
        }
        const uint32_t frames = static_cast<uint32_t>(
            duration * 48000.0f);
        std::lock_guard lock{m_voiceMutex};
        if (profile == PlayerHapticProfile::Grab ||
            profile == PlayerHapticProfile::CinematicImpact) {
            // Replace an active voice of the same sustained/impact profile.
            // Overlap would clip and blur the intended rhythm.
            m_playerReactions.erase(std::remove_if(
                m_playerReactions.begin(),
                m_playerReactions.end(),
                [profile](const PlayerReactionVoice& reaction) {
                    return reaction.profile == profile;
                }), m_playerReactions.end());
        }
        if (m_playerReactions.size() >= 16) {
            m_playerReactions.erase(m_playerReactions.begin());
        }
        m_playerReactions.push_back({
            profile,
            std::clamp(intensity, 0.0f, 1.0f),
            0,
            frames,
            0.0,
            0.0,
            0.0,
            0.0f,
            0xC13FA9A9u ^
                (static_cast<uint32_t>(profile) * 2246822519u) ^
                static_cast<uint32_t>(GetTickCount64())});
    }

    void PlayCombatHaptic(
        CombatHapticProfile profile,
        float intensity,
        uint8_t motionVariant = 0u) {
        if (!m_started.load()) return;
        float duration = 0.30f;
        switch (profile) {
        case CombatHapticProfile::Guard: duration = 0.30f; break;
        case CombatHapticProfile::Parry: duration = 0.19f; break;
        case CombatHapticProfile::Issen: duration = 0.40f; break;
        case CombatHapticProfile::Deflect: duration = 0.34f; break;
        case CombatHapticProfile::ProjectileParry: duration = 0.16f; break;
        case CombatHapticProfile::ParryActivation: duration = 0.20f; break;
        default: {
            const auto executionKind = execution_haptics::KindFromProfile(
                static_cast<uint8_t>(profile));
            if (executionKind == execution_haptics::Kind::None) return;
            duration = static_cast<float>(execution_haptics::Duration(executionKind)) /
                48000.0f;
            break;
        }
        }

        std::lock_guard lock{m_voiceMutex};
        const bool repelImpact = profile == CombatHapticProfile::Parry ||
            profile == CombatHapticProfile::ProjectileParry;
        if (repelImpact) {
            // The PC game emits two fixed 0.4 motor pulses and sustained 0.4
            // motor power immediately after a successful Repel. They masked
            // every authored revision with the same generic rumble. Give the
            // exact success event an uncontaminated actuator window.
            m_voices.clear();
            m_capturedPresetVoices.clear();
            m_liveMotorTargets.Clear();
            m_motorPower.fill(0.0f);
            for (auto& pulses : m_motorPulses) pulses.clear();
        } else if (execution_haptics::KindFromProfile(
                static_cast<uint8_t>(profile)) != execution_haptics::Kind::None) {
            // Remove the generic PC motor bed before starting the authored
            // execution timeline. Subsequent motor traffic is isolated below.
            m_liveMotorTargets.Clear();
            m_motorPower.fill(0.0f);
            m_capturedPresetVoices.clear();
            for (auto& pulses : m_motorPulses) pulses.clear();
            m_suppressedExecutionMotorMessages.store(0, std::memory_order_relaxed);
        }
        // Concrete and inherited detailEnter hooks may resolve to the same
        // transition. Restart the matching profile instead of stacking it;
        // stacking makes a 200% setting clip and masks its designed shape.
        m_combatHaptics.erase(std::remove_if(
            m_combatHaptics.begin(),
            m_combatHaptics.end(),
            [profile](const CombatHapticVoice& voice) {
                const bool impact = profile == CombatHapticProfile::Parry ||
                    profile == CombatHapticProfile::ProjectileParry;
                const bool execution = execution_haptics::KindFromProfile(
                    static_cast<uint8_t>(profile)) != execution_haptics::Kind::None;
                return voice.profile == profile || (impact &&
                    (voice.profile == CombatHapticProfile::ParryActivation ||
                     voice.profile == CombatHapticProfile::Parry ||
                     voice.profile == CombatHapticProfile::ProjectileParry)) ||
                    (execution && execution_haptics::KindFromProfile(
                        static_cast<uint8_t>(voice.profile)) !=
                        execution_haptics::Kind::None);
            }), m_combatHaptics.end());
        if (m_combatHaptics.size() >= 8) {
            m_combatHaptics.erase(m_combatHaptics.begin());
        }
        m_combatHaptics.push_back({
            profile,
            std::clamp(intensity, 0.0f, 2.0f),
            static_cast<uint8_t>(motionVariant & 0x03u),
            0,
            profile == CombatHapticProfile::Parry || profile == CombatHapticProfile::ProjectileParry
                ? repel_impact_haptics::DurationFrames(profile == CombatHapticProfile::ProjectileParry)
                : static_cast<uint32_t>(duration * 48000.0f),
            0.0,
            0.0});
        LogFormat(
            L"Combat haptic profile=%u intensity=%d variant=%u",
            static_cast<unsigned>(profile),
            static_cast<int>(std::lround(intensity * 100.0f)),
            static_cast<unsigned>(motionVariant & 0x03u));
        if (profile == CombatHapticProfile::Parry || profile == CombatHapticProfile::ProjectileParry) {
            LogFormat(L"Combat repel single impact profile=%u durationMs=%u attackMs=2 layeredPulses=0",
                static_cast<unsigned>(profile),
                repel_impact_haptics::DurationFrames(profile == CombatHapticProfile::ProjectileParry) / 48u);
        }
    }

    void PlaySpeakerCue(ControllerSpeakerCue cue,
        combat_defense_feedback::DeflectSpeakerVariant variant =
            combat_defense_feedback::DeflectSpeakerVariant::Short) {
        if (!m_started.load()) return;
        float duration = 0.35f;
        if (cue == ControllerSpeakerCue::SoulCollect) duration = 0.52f;
        if (cue == ControllerSpeakerCue::SoulStop) duration = 0.30f;
        if (cue == ControllerSpeakerCue::CombatClash) duration = 0.24f;
        if (cue == ControllerSpeakerCue::CombatGuard) duration = 0.19f;
        if (cue == ControllerSpeakerCue::CombatGuardDirectional) duration = 0.24f;
        if (cue == ControllerSpeakerCue::CombatDeflect) duration = 0.30f;
        if (cue == ControllerSpeakerCue::CombatExecution) duration = 0.44f;
        if (cue == ControllerSpeakerCue::CombatClashHit) duration = 0.24f;
        if (cue == ControllerSpeakerCue::CombatClashSuccess) duration = 0.42f;
        if (cue == ControllerSpeakerCue::CombatIssen) duration = 0.44f;
        if (cue == ControllerSpeakerCue::CombatParryBlade) duration = 0.36f;
        if (cue == ControllerSpeakerCue::CombatParryProjectile) duration = 0.30f;
        std::lock_guard lock{m_voiceMutex};
        if (cue == ControllerSpeakerCue::SoulStart) {
            const bool wasActive = m_soulSpeakerTarget > 0.5f;
            m_soulSpeakerTarget = 1.0f;
            if (m_speakerAudioLoaded && !wasActive) {
                m_soulLoopFrame = 0;
                m_speakerFileVoices.push_back({
                    0, 0, 0.76f, ControllerSpeakerCue::SoulStart});
            }
        } else if (cue == ControllerSpeakerCue::SoulStop) {
            const bool wasActive = m_soulSpeakerTarget > 0.5f;
            m_soulSpeakerTarget = 0.0f;
            if (m_speakerAudioLoaded && wasActive) {
                m_speakerFileVoices.push_back({
                    3, 0, 0.74f, ControllerSpeakerCue::SoulStop});
            }
        } else if (cue == ControllerSpeakerCue::SoulCollect &&
                   m_speakerAudioLoaded) {
            if (m_speakerFileVoices.size() >= 16) {
                m_speakerFileVoices.erase(m_speakerFileVoices.begin());
            }
            m_speakerFileVoices.push_back({
                2, 0, 0.82f, ControllerSpeakerCue::SoulCollect});
        }
        const bool soulCue =
            cue >= ControllerSpeakerCue::SoulStart &&
            cue <= ControllerSpeakerCue::SoulStop;
        if (m_speakerAudioLoaded && soulCue) {
            return;
        }
        const bool combatCue =
            cue >= ControllerSpeakerCue::CombatClash &&
            cue <= ControllerSpeakerCue::CombatParryProjectile;
        if (cue == ControllerSpeakerCue::CombatDeflect &&
            combat_defense_feedback::UsesContactRecording(variant) &&
            (!m_slowDeflectSpeakerAudioLoaded || !m_combatSpeakerAudioLoaded)) {
            Log(L"Slow Deflect original cue unavailable; refusing short/synthetic substitute");
            return;
        }
        if (m_combatSpeakerAudioLoaded && combatCue) {
            if (cue == ControllerSpeakerCue::CombatDeflect) {
                // A NEW accepted impact can interrupt the previous tail, but
                // never stack several copies of the same Deflect clip. Keep
                // all other cues, gains, and original-audio playback unchanged.
                m_combatSpeakerFileVoices.erase(std::remove_if(
                    m_combatSpeakerFileVoices.begin(),
                    m_combatSpeakerFileVoices.end(),
                    [](const SpeakerFileVoice& voice) {
                        return voice.cue == ControllerSpeakerCue::CombatDeflect;
                    }), m_combatSpeakerFileVoices.end());
            }
            if (cue == ControllerSpeakerCue::CombatParryBlade ||
                cue == ControllerSpeakerCue::CombatParryProjectile) {
                // A successful Repel can follow a long directional-guard
                // sample by less than one second. Prioritize the confirmed
                // Repel layers so they are not masked by that older voice.
                m_combatSpeakerFileVoices.clear();
            }
            auto enqueueCombat = [&] (
                size_t clipIndex,
                float gain,
                float delaySeconds = 0.0f) {
                if (clipIndex >= m_combatSpeakerClips.size()) return;
                if (m_combatSpeakerFileVoices.size() >= 16) {
                    m_combatSpeakerFileVoices.erase(
                        m_combatSpeakerFileVoices.begin());
                }
                const uint32_t delayFrames = static_cast<uint32_t>(std::clamp(
                    delaySeconds * 48000.0f,
                    0.0f,
                    240000.0f));
                m_combatSpeakerFileVoices.push_back({
                    clipIndex, 0, gain, cue, delayFrames, false, variant});
                const size_t playbackFrames =
                    combat_speaker_timing::PlaybackFrames(
                        cue == ControllerSpeakerCue::CombatDeflect,
                        m_combatSpeakerClips[clipIndex].Frames(), variant);
                LogFormat(
                    L"Combat speaker file queued cue=%u clip=%zu gain=%.3f frames=%zu playback=%zu delay=%u variant=%u",
                    static_cast<unsigned>(cue),
                    clipIndex,
                    gain,
                    m_combatSpeakerClips[clipIndex].Frames(),
                    playbackFrames,
                    delayFrames, static_cast<unsigned>(variant));
            };
            const size_t hitVariant = 1 + (m_combatSpeakerVariant++ % 3);
            switch (cue) {
            case ControllerSpeakerCue::CombatClash:
                // The extracted DeadHeat start/hit assets are deliberately
                // quiet (about 0.10-0.12 peak), unlike the guard assets.
                // Per-cue gain brings them to the same perceived range without
                // raising dialogue, soul audio, guard, or deflect globally.
                enqueueCombat(0, 2.90f);
                break;
            case ControllerSpeakerCue::CombatClashHit:
                enqueueCombat(hitVariant, 3.00f);
                break;
            case ControllerSpeakerCue::CombatClashSuccess:
                enqueueCombat(4, 0.74f);
                enqueueCombat(5, 0.70f);
                break;
            case ControllerSpeakerCue::CombatGuard:
                // Exact hit_cmn event 1965748642: three randomized contact
                // samples used when cDamageGuard begins.
                enqueueCombat(10 + (m_combatSpeakerVariant++ % 3), 0.82f);
                break;
            case ControllerSpeakerCue::CombatGuardDirectional:
                // Exact hit_cmn event 4184067710: the three randomized
                // directional-force samples used by the follow-up state.
                enqueueCombat(13 + (m_combatSpeakerVariant++ % 3), 0.82f);
                break;
            case ControllerSpeakerCue::CombatDeflect:
                // Resolved native PARRY pair 1992708532 / 3387009413 uses
                // event 3991122447's separately decoded ORIGINAL recording.
                // Keep ordinary event 3469150555 short, never stretch it.
                enqueueCombat(combat_defense_feedback::UsesContactRecording(variant)
                    ? 18 : 9, 0.84f);
                break;
            case ControllerSpeakerCue::CombatParryBlade:
                // L1+Cross melee tests resolved the native BLOCK branch. Use
                // BlockSettings default 3893406318 / event 1169882926, not the
                // superficially named ParrySettings/deflect-style contact.
                // Weapon/enemy overrides are logged separately by the bridge.
                enqueueCombat(16, 1.25f);
                break;
            case ControllerSpeakerCue::CombatParryProjectile:
                // Start with the already-confirmed native blade Repel contact,
                // then retain the projectile return transient and short hit.
                // Staggering the latter two keeps all three layers readable
                // through the controller-speaker soft limiter.
                enqueueCombat(16, 1.25f);
                enqueueCombat(17, 2.60f, 0.080f);
                enqueueCombat(hitVariant, 1.80f, 0.110f);
                break;
            case ControllerSpeakerCue::CombatExecution:
                // Break Issen / enemy execution original event.
                enqueueCombat(8, 0.76f);
                break;
            case ControllerSpeakerCue::CombatIssen:
                // Counter/chain Issen original event has two source variants.
                enqueueCombat(6 + (m_combatSpeakerVariant++ % 2), 0.86f);
                break;
            default:
                break;
            }
            return;
        }
        if (cue == ControllerSpeakerCue::CombatDeflect) {
            m_speakerCues.erase(std::remove_if(
                m_speakerCues.begin(), m_speakerCues.end(),
                [](const SpeakerCueVoice& voice) {
                    return voice.cue == ControllerSpeakerCue::CombatDeflect;
                }), m_speakerCues.end());
        }
        if (m_speakerCues.size() >= 12) m_speakerCues.erase(m_speakerCues.begin());
        m_speakerCues.push_back({
            cue,
            0,
            static_cast<uint32_t>(duration * 48000.0f),
            0.0,
            0.0,
            0.0f,
            0x85EBCA6Bu ^
                (static_cast<uint32_t>(cue) * 2246822519u) ^
            static_cast<uint32_t>(GetTickCount64()),
            0});
    }

    void PromoteDeflectSpeakerToCinematic() {
        if (!m_started.load()) return;
        std::lock_guard lock{m_voiceMutex};
        for (auto& voice : m_combatSpeakerFileVoices) {
            if (voice.cue != ControllerSpeakerCue::CombatDeflect ||
                !combat_defense_feedback::CanPromoteToCinematic(
                    voice.deflectVariant,
                    combat_defense_feedback::DeflectSpeakerVariant::Slow) ||
                voice.clipIndex != 18 ||
                voice.frame >= combat_speaker_timing::DeflectPlaybackFrames) continue;
            voice.deflectVariant = combat_defense_feedback::DeflectSpeakerVariant::Slow;
            LogFormat(L"Cinematic Deflect promoted existing original voice at frame=%zu (no restart)",
                voice.frame);
        }
    }

    bool PlayGauntletVoice(
        uint32_t triggerId,
        product_runtime::GauntletVoiceLanguage language) {
        if (!m_started.load() || triggerId == 0 || m_dataDirectory.empty()) {
            return false;
        }
        SpeakerClip clip;
        const bool japanese =
            language == product_runtime::GauntletVoiceLanguage::Japanese;
        const std::wstring path = m_dataDirectory + L"\\gauntlet_voice\\" +
            (japanese ? L"ja-JP\\" : L"") + std::to_wstring(triggerId) +
            L".wav";
        if (!LoadSpeakerWav(path, clip) || clip.Frames() == 0) {
            LogFormat(
                L"Gauntlet voice file unavailable language=%s path=%s",
                japanese ? L"ja-JP" : L"zh-CN",
                path.c_str());
            return false;
        }
        const size_t frames = clip.Frames();
        {
            std::lock_guard lock{m_voiceMutex};
        m_gauntletVoiceClip = std::move(clip);
            m_gauntletVoiceLanguage = language;
            m_gauntletVoiceFrame = 0;
            m_gauntletVoiceActive = true;
        }
        LogFormat(
            L"Gauntlet voice started trigger=%u language=%s frames=%llu",
            triggerId,
            japanese ? L"ja-JP" : L"zh-CN",
            static_cast<unsigned long long>(frames));
        return true;
    }

    void StopGauntletVoice() {
        std::lock_guard lock{m_voiceMutex};
        m_gauntletVoiceActive = false;
        m_gauntletVoiceFrame = 0;
    }

    void StopGauntletVoiceOnLanguageChange(product_runtime::GauntletVoiceLanguage language) {
        std::lock_guard lock{m_voiceMutex};
        if (m_gauntletVoiceLanguage == language) return;
        m_gauntletVoiceActive = false;
        m_gauntletVoiceFrame = 0;
    }

    void PlayMotorPulse(
        size_t motor,
        float power,
        float duration,
        bool attenuate,
        int32_t easeType) {
        if (!m_started.load() || motor >= m_motorPulses.size()) return;
        const uint32_t frames = static_cast<uint32_t>(std::clamp(
            duration * 48000.0f,
            192.0f,
            240000.0f));
        // Lift quiet PC motor presets into the DualSense actuator's useful
        // range without collapsing all medium/heavy events to full scale.
        const float calibratedPower =
            captured_haptic_presets::CalibrateMotorPower(power);
        std::lock_guard lock{m_voiceMutex};
        if (ExecutionActiveLocked()) {
            m_suppressedExecutionMotorMessages.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (RepelImpactActiveLocked()) {
            m_suppressedRepelMotorMessages.fetch_add(1, std::memory_order_relaxed);
            LogFormat(L"Repel impact suppressed motor pulse=%zu power=%.3f duration=%.3f",
                motor, power, duration);
            return;
        }
        auto& pulses = m_motorPulses[motor];
        if (pulses.size() >= 24) pulses.erase(pulses.begin());
        pulses.push_back({calibratedPower, frames, frames, attenuate, easeType});
    }

    void PlayCapturedPreset(captured_haptic_presets::Preset preset) {
        if (!m_started.load()) return;
        const auto profile = captured_haptic_presets::ProfileFor(preset);
        const uint32_t frames = static_cast<uint32_t>(std::clamp(
            profile.durationSeconds * 48000.0f,
            192.0f,
            240000.0f));
        std::lock_guard lock{m_voiceMutex};
        const bool overlapsChest = std::any_of(
            m_environmentInteractions.begin(), m_environmentInteractions.end(),
            [preset](const EnvironmentInteractionVoice& voice) {
                return voice.frame < voice.totalFrames &&
                    ((preset == captured_haptic_presets::Preset::GenericInteract &&
                      voice.profile == EnvironmentInteractionProfile::TreasureChestOpen) ||
                     (preset == captured_haptic_presets::Preset::GetItem &&
                      voice.profile == EnvironmentInteractionProfile::TreasureChestLidLand));
            });
        if (overlapsChest) {
            ++m_suppressedChestPresets;
            LogFormat(L"Chest suppressed overlapping captured preset=%u",
                static_cast<unsigned>(preset));
            return;
        }
        if (m_capturedPresetVoices.size() >= 24) {
            m_capturedPresetVoices.erase(m_capturedPresetVoices.begin());
        }
        m_capturedPresetVoices.push_back({preset, 0, frames});
        LogFormat(
            L"Captured native preset replay=%u duration=%.3f",
            static_cast<unsigned>(preset),
            profile.durationSeconds);
    }

    uint32_t ConsumeCompletedCombatHaptics() {
        return m_completedCombatHapticMask.exchange(
            0,
            std::memory_order_acq_rel);
    }

    uint32_t ConsumeCompletedEnvironmentHaptics() {
        return m_completedEnvironmentHapticMask.exchange(0, std::memory_order_acq_rel);
    }

    uint32_t ConsumeSubmittedEnvironmentHaptics() {
        return m_submittedEnvironmentHapticMask.exchange(0, std::memory_order_acq_rel);
    }

    uint32_t ConsumeSuppressedChestPresets() {
        std::lock_guard lock{m_voiceMutex};
        return std::exchange(m_suppressedChestPresets, 0u);
    }

    void FlushEnvironmentDiagnostics() {
        std::vector<EnvironmentRenderDiagnostic> diagnostics;
        {
            std::lock_guard lock{m_voiceMutex};
            diagnostics.swap(m_environmentDiagnostics);
        }
        for (const auto& diagnostic : diagnostics) {
            const auto& voice = diagnostic.voice;
            const double rms = std::sqrt(voice.outputEnergy /
                (2.0 * std::max(1u, voice.frame)));
            LogFormat(L"Environment render %s profile=%u frames=%u/%u durationMs=%u postMasterPeak=%.4f/%.4f rms=%.4f transport=%s revision=chest-output-r2-pcm-only",
                diagnostic.outcome, static_cast<unsigned>(voice.profile),
                voice.frame, voice.totalFrames, voice.totalFrames / 48u,
                voice.outputPeakLeft, voice.outputPeakRight, rms,
                m_bluetoothMode.load() ? L"Bluetooth" : L"USB");
        }
    }

    uint32_t ConsumeSubmittedNormalAttackHaptics() {
        return m_submittedNormalAttackMask.exchange(0, std::memory_order_acq_rel);
    }

    uint32_t ConsumeSubmittedCombatSpeakers() {
        return m_submittedCombatSpeakerMask.exchange(0, std::memory_order_acq_rel);
    }

    uint32_t ConsumeCompletedCombatSpeakers() {
        return m_completedCombatSpeakerMask.exchange(0, std::memory_order_acq_rel);
    }

    uint32_t ConsumeSuppressedRepelMotorMessages() {
        return m_suppressedRepelMotorMessages.exchange(0, std::memory_order_acq_rel);
    }

    uint32_t ConsumeSuppressedExecutionMotorMessages() {
        return m_suppressedExecutionMotorMessages.exchange(
            0, std::memory_order_acq_rel);
    }

private:
    void QueueEnvironmentDiagnosticLocked(const EnvironmentInteractionVoice& voice,
        const wchar_t* outcome) {
        if (m_environmentDiagnostics.size() >= 32)
            m_environmentDiagnostics.erase(m_environmentDiagnostics.begin());
        m_environmentDiagnostics.push_back({voice, outcome});
    }

    bool ExecutionActiveLocked() const {
        return std::any_of(m_combatHaptics.begin(), m_combatHaptics.end(),
            [](const CombatHapticVoice& voice) {
                return voice.frame < voice.totalFrames &&
                    execution_haptics::KindFromProfile(
                        static_cast<uint8_t>(voice.profile)) !=
                        execution_haptics::Kind::None;
            });
    }

    bool RepelImpactActiveLocked() const {
        return std::any_of(m_combatHaptics.begin(), m_combatHaptics.end(),
            [](const CombatHapticVoice& voice) {
                return voice.frame < voice.totalFrames &&
                    (voice.profile == CombatHapticProfile::Parry ||
                     voice.profile == CombatHapticProfile::ProjectileParry);
            });
    }

    struct Voice {
        size_t clipIndex;
        size_t frame;
        uint32_t delayFrames;
        float leftGain;
        float rightGain;
    };
    struct MotorPulse {
        float power;
        uint32_t remainingFrames;
        uint32_t totalFrames;
        bool attenuate;
        int32_t easeType;
    };

    float SpeakerVolumeForCue(ControllerSpeakerCue cue) const {
        switch (cue) {
        case ControllerSpeakerCue::SoulStart:
        case ControllerSpeakerCue::SoulCollect:
        case ControllerSpeakerCue::SoulStop:
            return m_soulSpeakerVolume;
        case ControllerSpeakerCue::CombatClash:
        case ControllerSpeakerCue::CombatClashHit:
        case ControllerSpeakerCue::CombatClashSuccess:
            return m_clashSpeakerVolume;
        case ControllerSpeakerCue::CombatGuard:
        case ControllerSpeakerCue::CombatGuardDirectional:
            return m_guardSpeakerVolume;
        case ControllerSpeakerCue::CombatDeflect:
            return m_deflectSpeakerVolume;
        case ControllerSpeakerCue::CombatParryBlade:
        case ControllerSpeakerCue::CombatParryProjectile:
            return m_parrySpeakerVolume;
        case ControllerSpeakerCue::CombatExecution:
            return m_executionSpeakerVolume;
        case ControllerSpeakerCue::CombatIssen:
            return m_issenSpeakerVolume;
        default:
            return 1.0f;
        }
    }

    void CloseEvents() {
        if (m_stopEvent != nullptr) CloseHandle(m_stopEvent);
        if (m_readyEvent != nullptr) CloseHandle(m_readyEvent);
        if (m_bufferEvent != nullptr) CloseHandle(m_bufferEvent);
        m_stopEvent = nullptr;
        m_readyEvent = nullptr;
        m_bufferEvent = nullptr;
    }

    void ThreadMain() {
        const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        IMMDevice* device = nullptr;
        IAudioClient* audioClient = nullptr;
        IAudioRenderClient* renderClient = nullptr;
        bool audioStarted = false;
        bool bluetooth = false;
        ControllerConnection streamController;
        HANDLE streamTimer = nullptr;
        LARGE_INTEGER clockFrequency{}, nextDeadline{};
#ifdef ONIMUSHA_BLUETOOTH_ENABLED
        OpusEncoder* encoder = nullptr;
        uint8_t streamCounter = 0;
        unsigned streamPackets = 0, activeHapticPackets = 0, activeSpeakerPackets = 0;
        ULONGLONG nextStreamDiagnostic = GetTickCount64() + 5000;
        ULONGLONG maximumWriteMs = 0;
        std::array<float, dualsense_bluetooth::SourceFrames * 4> streamBuffer{};
#endif

        do {
            UINT32 bufferFrames = 0;
#ifdef ONIMUSHA_BLUETOOTH_ENABLED
            if (!OpenController(streamController)) break;
            bluetooth = streamController.bluetooth;
            m_bluetoothMode.store(bluetooth);
            if (bluetooth) {
                // Never start high-rate audio before input and a control write
                // establish the Windows Bluetooth HID channel.
                if (!ConfigureControllerSpeaker(streamController, true)) {
                    Log(L"Bluetooth control channel not ready; audio stream postponed");
                    break;
                }
                int error = OPUS_OK;
                encoder = opus_encoder_create(48000, 2, OPUS_APPLICATION_RESTRICTED_LOWDELAY, &error);
                if (encoder == nullptr || error != OPUS_OK) break;
                if (opus_encoder_ctl(encoder, OPUS_SET_BITRATE(160000)) != OPUS_OK ||
                    opus_encoder_ctl(encoder, OPUS_SET_VBR(0)) != OPUS_OK ||
                    opus_encoder_ctl(encoder, OPUS_SET_COMPLEXITY(1)) != OPUS_OK) break;
                streamTimer = CreateWaitableTimerExW(nullptr, nullptr,
                    CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
                if (streamTimer == nullptr)
                    streamTimer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
                if (streamTimer == nullptr || !QueryPerformanceFrequency(&clockFrequency)) break;
                QueryPerformanceCounter(&nextDeadline);
                bufferFrames = static_cast<UINT32>(dualsense_bluetooth::SourceFrames);
            } else {
                CloseController(streamController);
#endif
            HRESULT hr = FindDualSenseRenderEndpoint(&device);
            if (FAILED(hr)) {
                LogFormat(L"DualSense render endpoint not found hr=0x%08X", hr);
                break;
            }
            hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                reinterpret_cast<void**>(&audioClient));
            if (FAILED(hr)) {
                LogFormat(L"IAudioClient activation failed hr=0x%08X", hr);
                break;
            }

            WAVEFORMATEXTENSIBLE format{};
            format.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
            format.Format.nChannels = 4;
            format.Format.nSamplesPerSec = 48000;
            format.Format.wBitsPerSample = 32;
            format.Format.nBlockAlign = 4 * sizeof(float);
            format.Format.nAvgBytesPerSec = format.Format.nSamplesPerSec * format.Format.nBlockAlign;
            format.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
            format.Samples.wValidBitsPerSample = 32;
            format.dwChannelMask = KSAUDIO_SPEAKER_QUAD;
            format.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;

            hr = audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED,
                AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_NOPERSIST,
                1000000, 0, reinterpret_cast<WAVEFORMATEX*>(&format), nullptr);
            if (FAILED(hr)) {
                LogFormat(L"Haptics WASAPI initialize failed hr=0x%08X", hr);
                break;
            }
            hr = audioClient->SetEventHandle(m_bufferEvent);
            if (FAILED(hr)) {
                LogFormat(L"Haptics WASAPI SetEventHandle failed hr=0x%08X", hr);
                break;
            }
            hr = audioClient->GetService(__uuidof(IAudioRenderClient),
                reinterpret_cast<void**>(&renderClient));
            if (FAILED(hr)) {
                LogFormat(L"IAudioRenderClient acquisition failed hr=0x%08X", hr);
                break;
            }

            hr = audioClient->GetBufferSize(&bufferFrames);
            if (FAILED(hr)) break;
            BYTE* initial = nullptr;
            hr = renderClient->GetBuffer(bufferFrames, &initial);
            if (FAILED(hr)) break;
            renderClient->ReleaseBuffer(bufferFrames, AUDCLNT_BUFFERFLAGS_SILENT);
            hr = audioClient->Start();
            if (FAILED(hr)) {
                LogFormat(L"Haptics WASAPI Start failed hr=0x%08X", hr);
                break;
            }
            audioStarted = true;
#ifdef ONIMUSHA_BLUETOOTH_ENABLED
            }
#endif
            m_started.store(true);
            SetEvent(m_readyEvent);

            HANDLE waits[]{m_stopEvent, bluetooth ? streamTimer : m_bufferEvent};
            for (;;) {
#ifdef ONIMUSHA_BLUETOOTH_ENABLED
                if (bluetooth) {
                    LARGE_INTEGER now{}, due{};
                    QueryPerformanceCounter(&now);
                    due.QuadPart = -std::max<LONGLONG>(1,
                        (nextDeadline.QuadPart - now.QuadPart) * 10000000 / clockFrequency.QuadPart);
                    if (!SetWaitableTimer(streamTimer, &due, 0, nullptr, nullptr, FALSE)) break;
                }
#endif
                const DWORD wait = WaitForMultipleObjects(2, waits, FALSE, 1000);
                if (wait == WAIT_OBJECT_0) break;
                if (wait == WAIT_TIMEOUT) {
                    if (bluetooth) { Log(L"Bluetooth stream timer timeout"); break; }
                    UINT32 healthPadding = 0;
                    const HRESULT health =
                        audioClient->GetCurrentPadding(&healthPadding);
                    if (FAILED(health)) {
                        LogFormat(
                            L"DualSense haptics audio device lost while idle hr=0x%08X",
                            health);
                        break;
                    }
                    continue;
                }
                if (wait != WAIT_OBJECT_0 + 1) {
                    LogFormat(
                        L"DualSense haptics wait failed result=0x%08X",
                        wait);
                    break;
                }

                UINT32 available = bufferFrames;
                BYTE* raw = nullptr;
#ifdef ONIMUSHA_BLUETOOTH_ENABLED
                if (bluetooth) {
                    raw = reinterpret_cast<BYTE*>(streamBuffer.data());
                } else {
#endif
                UINT32 padding = 0;
                const HRESULT paddingResult =
                    audioClient->GetCurrentPadding(&padding);
                if (FAILED(paddingResult)) {
                    LogFormat(
                        L"DualSense haptics audio device lost hr=0x%08X",
                        paddingResult);
                    break;
                }
                if (padding >= bufferFrames) continue;
                available = bufferFrames - padding;
                const HRESULT bufferResult =
                    renderClient->GetBuffer(available, &raw);
                if (FAILED(bufferResult)) {
                    LogFormat(
                        L"DualSense haptics buffer acquisition failed hr=0x%08X",
                        bufferResult);
                    break;
                }
#ifdef ONIMUSHA_BLUETOOTH_ENABLED
                }
#endif

                float* output = reinterpret_cast<float*>(raw);
                std::unique_lock voiceLock{m_voiceMutex};
                const uint32_t expiredMotors = m_liveMotorTargets.Expire(GetTickCount64());
                if (expiredMotors != 0) LogFormat(
                    L"Live motor sender lease expired mask=0x%X revision=motor-state-r1", expiredMotors);
                constexpr double TwoPi = 6.28318530717958647692;
                constexpr double SampleRate = 48000.0;
                uint32_t normalAttackSubmitted = 0;
                uint32_t environmentSubmitted = 0;
                for (UINT32 frame = 0; frame < available; ++frame) {
                    float left = 0.0f;
                    float right = 0.0f;
                    float speaker = 0.0f;
                    float combatSpeaker = 0.0f;
                    std::array<float, 4> impactAccent{};
                    for (auto& voice : m_titleVoices) {
                        const auto sample = title_haptics::Sample(voice.cue, voice.frame / 48000.0f);
                        const float fade = voice.release < 288 ? voice.release / 288.0f : 1.0f;
                        left += sample[0] * fade;
                        right += sample[1] * fade;
                        ++voice.frame;
                        if (voice.release != 0xFFFFFFFFu && voice.release > 0) --voice.release;
                    }
                    for (Voice& voice : m_voices) {
                        if (voice.delayFrames > 0) {
                            --voice.delayFrames;
                            continue;
                        }
                        const HapticClip& clip = m_clips[voice.clipIndex];
                        if (voice.frame < clip.Frames()) {
                            left += clip.stereoSamples[voice.frame * 2] * voice.leftGain;
                            right += clip.stereoSamples[voice.frame * 2 + 1] * voice.rightGain;
                            ++voice.frame;
                        }
                    }

                    // Footsteps bypass the generic motor pulse path.  Their
                    // gradual attack removes the hard click/"glass" edge, and
                    // the game's collision material selects an organic carrier
                    // and decay profile for the ground under the player.
                    for (FootstepVoice& footstep : m_footsteps) {
                        if (footstep.frame >= footstep.totalFrames) continue;
                        const float attackFrames = std::max(
                            1.0f,
                            footstep.profile.attackSeconds *
                                static_cast<float>(SampleRate));
                        float envelope = 0.0f;
                        if (static_cast<float>(footstep.frame) < attackFrames) {
                            const float x = std::clamp(
                                static_cast<float>(footstep.frame) / attackFrames,
                                0.0f,
                                1.0f);
                            envelope = x * x * (3.0f - 2.0f * x);
                        } else {
                            const float decayProgress = std::clamp(
                                (static_cast<float>(footstep.frame) - attackFrames) /
                                    std::max(1.0f,
                                        static_cast<float>(footstep.totalFrames) -
                                            attackFrames),
                                0.0f,
                                1.0f);
                            envelope = std::exp(
                                -footstep.profile.decayRate * decayProgress);
                        }

                        footstep.noiseState =
                            footstep.noiseState * 1664525u + 1013904223u;
                        const float rawStepNoise =
                            static_cast<float>((footstep.noiseState >> 8) & 0xFFFFu) /
                                32767.5f - 1.0f;
                        footstep.filteredNoise +=
                            (rawStepNoise - footstep.filteredNoise) * 0.018f;
                        const float base = static_cast<float>(
                            std::sin(footstep.basePhase));
                        const float harmonic = static_cast<float>(
                            std::sin(footstep.harmonicPhase));
                        const float tonalMix =
                            1.0f - footstep.profile.harmonicMix;
                        const float wave =
                            base * tonalMix +
                            harmonic * footstep.profile.harmonicMix +
                            footstep.filteredNoise * footstep.profile.textureMix;
                        float amplitude = footstep.profile.amplitude *
                            (0.80f + 0.78f * footstep.pace);
                        if (footstep.stepType == 2) amplitude *= 0.90f;
                        if (footstep.stepType == 3) amplitude *= 0.53f;
                        amplitude *= envelope;

                        const float oppositeGain = 0.54f + 0.14f * footstep.pace;
                        const float leftGain = footstep.side == 0 ? 1.0f : oppositeGain;
                        const float rightGain = footstep.side == 1 ? 1.0f : oppositeGain;
                        left += wave * amplitude * leftGain;
                        right += wave * amplitude * rightGain;

                        const double gaitPitch = 1.0 - 0.11 * footstep.pace;
                        footstep.basePhase += TwoPi *
                            footstep.profile.baseFrequency * gaitPitch / SampleRate;
                        footstep.harmonicPhase += TwoPi *
                            footstep.profile.harmonicFrequency * gaitPitch / SampleRate;
                        if (footstep.basePhase >= TwoPi) footstep.basePhase -= TwoPi;
                        if (footstep.harmonicPhase >= TwoPi) {
                            footstep.harmonicPhase -= TwoPi;
                        }
                        ++footstep.frame;
                    }

                    const auto wallRun = m_wallRunRenderer.Render();
                    left += wallRun.left;
                    right += wallRun.right;
                    const auto ladder = m_ladderRenderer.Render();
                    left += ladder.left;
                    right += ladder.right;
                    const auto normalAttack = m_normalAttackRenderer.Render();
                    normalAttackSubmitted |= m_normalAttackRenderer.ConsumeRenderedFlags();
                    left += normalAttack.left;
                    right += normalAttack.right;
                    if (m_oniChargeReleaseFrames > 0) {
                        if (m_oniChargeReleaseDelayFrames > 0) {
                            --m_oniChargeReleaseDelayFrames;
                        } else {
                            const auto release = oni_charge_release_haptics::Sample(
                                m_oniChargeReleaseFrame++, m_oniChargeReleaseKind);
                            left += release.left;
                            right += release.right;
                            --m_oniChargeReleaseFrames;
                        }
                    }
                    if (m_bowShotFrames > 0) {
                        const auto shot = bow_shot_haptics::Sample(
                            m_bowShotFrame++, m_bowShotKind);
                        left += shot.left;
                        right += shot.right;
                        --m_bowShotFrames;
                    }
                    if (m_oniTransformationFrames > 0) {
                        const uint32_t transformationFrame = oni_transformation_haptics::DurationFrames -
                            m_oniTransformationFrames;
                        const auto transformation = oni_transformation_haptics::Sample(transformationFrame);
                        left += transformation.left;
                        right += transformation.right;
                        --m_oniTransformationFrames;
                    }

                    // Black souls have a separate state-driven lifecycle, not
                    // a stream of ordinary pickup-success events.
                    if (m_blackSoulHoldFrames > 0) --m_blackSoulHoldFrames;
                    else m_blackSoulTarget = 0.0f;
                    const float blackSoulSmoothing =
                        m_blackSoulTarget > m_blackSoulLevel ? 0.0020f :
                        (m_blackSoulFinishFrames > 0 ?
                            black_soul_haptics::FinishReleaseSmoothing : 0.0025f);
                    m_blackSoulLevel += (m_blackSoulTarget - m_blackSoulLevel) *
                        blackSoulSmoothing;
                    if (m_blackSoulTarget == 0.0f && m_blackSoulLevel < 0.0001f)
                        m_blackSoulLevel = 0.0f;
                    if (m_blackSoulLevel > 0.0f) {
                        const auto sample = black_soul_haptics::Sample(
                            m_blackSoulFrame++, m_blackSoulLevel);
                        left += sample.left;
                        right += sample.right;
                    }
                    if (m_blackSoulFinishFrames > 0) {
                        const auto sample = black_soul_haptics::FinishSample(
                            black_soul_haptics::FinishFrames - m_blackSoulFinishFrames,
                            m_blackSoulFinishIntensity);
                        left += sample.left;
                        right += sample.right;
                        --m_blackSoulFinishFrames;
                    }

                    // Soul absorption is a left-leading continuous pull. It
                    // complements the L2 adaptive-trigger effect rather than
                    // replacing it: the trigger keeps its mechanical texture,
                    // while the left actuator carries the flowing soul energy.
                    if (m_soulHapticHoldFrames > 0) {
                        --m_soulHapticHoldFrames;
                    } else {
                        m_soulHapticTarget = 0.0f;
                    }
                    const float soulHapticSmoothing =
                        m_soulHapticTarget > m_soulHapticLevel
                            ? 0.00185f
                            : 0.00048f;
                    m_soulHapticLevel +=
                        (m_soulHapticTarget - m_soulHapticLevel) *
                        soulHapticSmoothing;
                    if (m_soulHapticTarget == 0.0f &&
                        m_soulHapticLevel < 0.0001f) {
                        m_soulHapticLevel = 0.0f;
                    }
                    m_soulHapticNoiseState =
                        m_soulHapticNoiseState * 1664525u + 1013904223u;
                    const float rawSoulHapticNoise =
                        static_cast<float>(
                            (m_soulHapticNoiseState >> 8) & 0xFFFFu) /
                            32767.5f - 1.0f;
                    m_soulHapticNoiseLow +=
                        (rawSoulHapticNoise - m_soulHapticNoiseLow) * 0.017f;
                    const float soulHapticMod = static_cast<float>(
                        0.78 + 0.16 * std::sin(m_soulHapticModPhase) +
                        0.06 * std::sin(m_soulHapticModPhase * 0.43));
                    const float soulHapticWave = static_cast<float>(
                        0.68 * std::sin(m_soulHapticPhase) +
                        0.22 * std::sin(m_soulHapticHarmonicPhase)) +
                        m_soulHapticNoiseLow * 0.10f;
                    float soulHapticAmplitude =
                        0.148f * std::pow(m_soulHapticLevel, 0.82f) *
                        soulHapticMod;
                    if (m_soulHapticStartFrames > 0) {
                        const float t = 1.0f -
                            static_cast<float>(m_soulHapticStartFrames) /
                                7200.0f;
                        const float attack = std::clamp(
                            t / 0.08f, 0.0f, 1.0f);
                        soulHapticAmplitude +=
                            0.235f * attack * std::exp(-4.8f * t);
                        --m_soulHapticStartFrames;
                    }
                    if (m_soulHapticAccentFrames > 0) {
                        const float t = 1.0f -
                            static_cast<float>(m_soulHapticAccentFrames) /
                                4800.0f;
                        const float knot = std::exp(
                            -210.0f * (t - 0.20f) * (t - 0.20f));
                        const float settle = std::exp(
                            -95.0f * (t - 0.58f) * (t - 0.58f));
                        soulHapticAmplitude += 0.315f * knot + 0.125f * settle;
                        --m_soulHapticAccentFrames;
                    }
                    left += soulHapticWave * soulHapticAmplitude;
                    right += soulHapticWave * soulHapticAmplitude * 0.36f;
                    const double soulHapticFrequency =
                        72.0 + 18.0 * m_soulHapticLevel +
                        5.0 * std::sin(m_soulHapticModPhase);
                    m_soulHapticPhase +=
                        TwoPi * soulHapticFrequency / SampleRate;
                    m_soulHapticHarmonicPhase +=
                        TwoPi * (soulHapticFrequency * 1.72) / SampleRate;
                    m_soulHapticModPhase += TwoPi * 3.1 / SampleRate;
                    if (m_soulHapticPhase >= TwoPi) {
                        m_soulHapticPhase -= TwoPi;
                    }
                    if (m_soulHapticHarmonicPhase >= TwoPi) {
                        m_soulHapticHarmonicPhase -= TwoPi;
                    }
                    if (m_soulHapticModPhase >= TwoPi) {
                        m_soulHapticModPhase -= TwoPi;
                    }

                    // Holding the absorption action with no soul in range has
                    // a faint left-biased searching texture. It starts only
                    // after the bridge's no-soul grace period and is suppressed
                    // as soon as the real absorption stream becomes active.
                    if (m_soulSearchHoldFrames > 0) {
                        --m_soulSearchHoldFrames;
                    } else {
                        m_soulSearchTarget = 0.0f;
                    }
                    const float soulSearchSmoothing =
                        m_soulSearchTarget > m_soulSearchLevel
                            ? 0.00072f
                            : 0.00040f;
                    m_soulSearchLevel +=
                        (m_soulSearchTarget - m_soulSearchLevel) *
                        soulSearchSmoothing;
                    if (m_soulSearchTarget == 0.0f &&
                        m_soulSearchLevel < 0.0001f) {
                        m_soulSearchLevel = 0.0f;
                    }
                    const float soulSearchPulse = static_cast<float>(
                        0.70 + 0.30 *
                            (0.5 + 0.5 * std::sin(m_soulSearchModPhase)));
                    const float soulSearchWave = static_cast<float>(
                        0.86 * std::sin(m_soulSearchPhase) +
                        0.14 * std::sin(m_soulSearchHarmonicPhase));
                    const float soulSearchAmplitude =
                        0.038f * m_soulSearchLevel * soulSearchPulse;
                    left += soulSearchWave * soulSearchAmplitude;
                    right += soulSearchWave * soulSearchAmplitude * 0.28f;
                    const double soulSearchFrequency =
                        68.0 + 4.0 * std::sin(m_soulSearchModPhase * 0.71);
                    m_soulSearchPhase +=
                        TwoPi * soulSearchFrequency / SampleRate;
                    m_soulSearchHarmonicPhase +=
                        TwoPi * (soulSearchFrequency * 1.55) / SampleRate;
                    m_soulSearchModPhase += TwoPi * 1.35 / SampleRate;
                    if (m_soulSearchPhase >= TwoPi) {
                        m_soulSearchPhase -= TwoPi;
                    }
                    if (m_soulSearchHarmonicPhase >= TwoPi) {
                        m_soulSearchHarmonicPhase -= TwoPi;
                    }
                    if (m_soulSearchModPhase >= TwoPi) {
                        m_soulSearchModPhase -= TwoPi;
                    }

                    // Sprint adds a soft forward surge and a low continuous
                    // body/air texture. Surface-specific footsteps remain
                    // distinct on top, so stone, timber and earth do not lose
                    // their existing material identity while running.
                    if (m_sprintHoldFrames > 0) {
                        --m_sprintHoldFrames;
                    } else {
                        m_sprintTarget = 0.0f;
                    }
                    const float sprintSmoothing =
                        m_sprintTarget > m_sprintLevel ? 0.00072f : 0.00034f;
                    m_sprintLevel +=
                        (m_sprintTarget - m_sprintLevel) * sprintSmoothing;
                    if (m_sprintTarget == 0.0f && m_sprintLevel < 0.0001f) {
                        m_sprintLevel = 0.0f;
                    }
                    m_sprintNoiseState =
                        m_sprintNoiseState * 1664525u + 1013904223u;
                    const float rawSprintNoise = static_cast<float>(
                        (m_sprintNoiseState >> 8) & 0xFFFFu) / 32767.5f - 1.0f;
                    m_sprintNoiseLow +=
                        (rawSprintNoise - m_sprintNoiseLow) * 0.010f;
                    const float sprintPan = static_cast<float>(
                        std::sin(m_sprintModPhase));
                    const float sprintWave = static_cast<float>(
                        0.72 * std::sin(m_sprintPhase) +
                        0.20 * std::sin(m_sprintHarmonicPhase)) +
                        m_sprintNoiseLow * 0.08f;
                    float sprintAmplitude =
                        0.092f * std::pow(m_sprintLevel, 0.88f) *
                        (0.96f + 0.04f * sprintPan);
                    if (m_sprintStartFrames > 0) {
                        const float t = 1.0f -
                            static_cast<float>(m_sprintStartFrames) /
                                8640.0f;
                        const float attack = std::clamp(
                            t / 0.055f, 0.0f, 1.0f);
                        const float push = attack * std::exp(-5.2f * t);
                        const float secondStep = std::exp(
                            -125.0f * (t - 0.40f) * (t - 0.40f));
                        sprintAmplitude += 0.260f * push + 0.105f * secondStep;
                        --m_sprintStartFrames;
                    }
                    left += sprintWave * sprintAmplitude *
                        (0.94f - 0.03f * sprintPan);
                    right += sprintWave * sprintAmplitude *
                        (0.94f + 0.03f * sprintPan);
                    const double sprintFrequency =
                        58.0 + 24.0 * m_sprintLevel;
                    m_sprintPhase += TwoPi * sprintFrequency / SampleRate;
                    m_sprintHarmonicPhase +=
                        TwoPi * (sprintFrequency * 1.63) / SampleRate;
                    m_sprintModPhase +=
                        TwoPi * (2.5 + 0.9 * m_sprintLevel) / SampleRate;
                    if (m_sprintPhase >= TwoPi) m_sprintPhase -= TwoPi;
                    if (m_sprintHarmonicPhase >= TwoPi) {
                        m_sprintHarmonicPhase -= TwoPi;
                    }
                    if (m_sprintModPhase >= TwoPi) m_sprintModPhase -= TwoPi;

                    // A broad low-frequency forward surge makes the L3 press
                    // feel weighty instead of crisp. Its single front-loaded
                    // envelope decays smoothly into the continuous sprint bed.
                    if (m_sprintPressFrames > 0) {
                        const float totalFrames = m_sprintPressIsSlowdown
                            ? 5760.0f
                            : 10560.0f;
                        const float t = 1.0f -
                            static_cast<float>(m_sprintPressFrames) /
                                totalFrames;
                        const float attackX = std::clamp(
                            t / (m_sprintPressIsSlowdown ? 0.065f : 0.040f),
                            0.0f,
                            1.0f);
                        const float attack = attackX * attackX *
                            (3.0f - 2.0f * attackX);
                        const float drive = attack * std::exp(
                            -(m_sprintPressIsSlowdown ? 5.10f : 3.80f) * t);
                        const float settle = attack * std::exp(
                            -(m_sprintPressIsSlowdown ? 3.80f : 2.35f) * t);
                        const float pressWave = static_cast<float>(
                            0.90 * std::sin(m_sprintPressPhase) +
                            0.10 * std::sin(m_sprintPressHarmonicPhase));
                        const float pressAmplitude = m_sprintPressIntensity *
                            (m_sprintPressIsSlowdown
                                ? 0.430f * drive + 0.055f * settle
                                : 0.900f * drive + 0.105f * settle);
                        left += pressWave * pressAmplitude;
                        right += pressWave * pressAmplitude * 0.94f;

                        // Stay below the crisp-click band throughout the cue.
                        const double pressFrequency = m_sprintPressIsSlowdown
                            ? 64.0 - 10.0 * t
                            : 68.0 + 18.0 * t;
                        m_sprintPressPhase +=
                            TwoPi * pressFrequency / SampleRate;
                        m_sprintPressHarmonicPhase +=
                            TwoPi * (pressFrequency * 1.28) / SampleRate;
                        if (m_sprintPressPhase >= TwoPi) {
                            m_sprintPressPhase -= TwoPi;
                        }
                        if (m_sprintPressHarmonicPhase >= TwoPi) {
                            m_sprintPressHarmonicPhase -= TwoPi;
                        }
                        --m_sprintPressFrames;
                        if (m_sprintPressFrames == 0) {
                            m_sprintPressIntensity = 0.0f;
                            m_sprintPressIsSlowdown = false;
                        }
                    }

                    // Player reactions use purpose-built envelopes instead of
                    // merely scaling one rumble: a stab has a narrow tactile
                    // point, a collision has a broad low body, knockdown adds
                    // a delayed floor impact, grab alternates pressure across
                    // the actuators, and dodges sweep laterally.
                    for (PlayerReactionVoice& reaction : m_playerReactions) {
                        if (reaction.frame >= reaction.totalFrames) continue;
                        const float progress = std::clamp(
                            static_cast<float>(reaction.frame) /
                                std::max(1.0f,
                                    static_cast<float>(reaction.totalFrames - 1)),
                            0.0f,
                            1.0f);
                        const float attackX = std::clamp(
                            progress / 0.028f,
                            0.0f,
                            1.0f);
                        const float attack =
                            attackX * attackX * (3.0f - 2.0f * attackX);
                        reaction.noiseState =
                            reaction.noiseState * 1664525u + 1013904223u;
                        const float rawReactionNoise =
                            static_cast<float>(
                                (reaction.noiseState >> 8) & 0xFFFFu) /
                                32767.5f - 1.0f;
                        reaction.filteredNoise +=
                            (rawReactionNoise - reaction.filteredNoise) * 0.055f;

                        const float waveA = static_cast<float>(
                            std::sin(reaction.phaseA));
                        const float waveB = static_cast<float>(
                            std::sin(reaction.phaseB));
                        float wave = waveA;
                        float amplitude = 0.0f;
                        float leftGain = 1.0f;
                        float rightGain = 1.0f;
                        double frequencyA = 100.0;
                        double frequencyB = 180.0;
                        double modulationFrequency = 4.0;

                        switch (reaction.profile) {
                        case PlayerHapticProfile::Hit: {
                            const float envelope = attack *
                                std::exp(-5.6f * progress);
                            wave = waveA * 0.62f + waveB * 0.38f;
                            amplitude = 0.360f * reaction.intensity * envelope;
                            frequencyA = 118.0;
                            frequencyB = 238.0;
                            break;
                        }
                        case PlayerHapticProfile::Stab: {
                            const float needle = attack *
                                std::exp(-13.5f * progress);
                            const float body = attack *
                                std::exp(-4.2f * progress);
                            wave = waveB * needle + waveA * body * 0.48f;
                            amplitude = 0.430f * reaction.intensity;
                            leftGain = 0.82f;
                            rightGain = 1.0f;
                            frequencyA = 92.0;
                            frequencyB = 318.0;
                            break;
                        }
                        case PlayerHapticProfile::Collision: {
                            const float body = attack *
                                std::exp(-3.7f * progress);
                            const float rebound = std::exp(
                                -150.0f *
                                (progress - 0.24f) * (progress - 0.24f));
                            wave = waveA * 0.84f + waveB * 0.16f;
                            amplitude = 0.450f * reaction.intensity *
                                (body * 0.78f + rebound * 0.30f);
                            frequencyA = 61.0;
                            frequencyB = 104.0;
                            break;
                        }
                        case PlayerHapticProfile::CinematicImpact: {
                            const float elapsed =
                                static_cast<float>(reaction.frame) / 48000.0f;
                            const float duration =
                                static_cast<float>(reaction.totalFrames) / 48000.0f;
                            wave = waveA * 0.70f + waveB * 0.30f;
                            if (duration <= 0.37f) {
                                // Preserve the short impact the user accepted.
                                const float impact = attack *
                                    std::exp(-3.5f * progress);
                                const float rebound = std::exp(
                                    -150.0f *
                                    (progress - 0.23f) * (progress - 0.23f));
                                const float tail = attack *
                                    std::exp(-4.5f * progress);
                                amplitude = reaction.intensity *
                                    (0.92f * impact + 0.38f * rebound +
                                     0.12f * tail);
                            } else {
                                // A longer authored range sustains a low body
                                // and ends with a separate, decaying aftershock.
                                const float onsetX = std::clamp(
                                    elapsed / 0.008f, 0.0f, 1.0f);
                                const float onset = onsetX * onsetX *
                                    (3.0f - 2.0f * onsetX);
                                const float impact = onset *
                                    std::exp(-elapsed / 0.105f);
                                const float reboundTime =
                                    (elapsed - 0.085f) / 0.035f;
                                const float rebound = std::exp(
                                    -reboundTime * reboundTime);
                                const float longWeight = std::clamp(
                                    (duration - 0.36f) / 0.40f, 0.0f, 1.0f);
                                const float sustainInX = std::clamp(
                                    (elapsed - 0.095f) / 0.050f, 0.0f, 1.0f);
                                const float sustainOutX = std::clamp(
                                    (duration - 0.17f - elapsed) / 0.10f,
                                    0.0f, 1.0f);
                                const float sustain =
                                    sustainInX * sustainInX *
                                    (3.0f - 2.0f * sustainInX) *
                                    sustainOutX * sustainOutX *
                                    (3.0f - 2.0f * sustainOutX);
                                const float aftershockStart = duration - 0.18f;
                                const float aftershockTime =
                                    (elapsed - aftershockStart) / 0.050f;
                                const float aftershock = std::exp(
                                    -aftershockTime * aftershockTime);
                                const float tailX = std::clamp(
                                    (elapsed - aftershockStart) / 0.010f,
                                    0.0f, 1.0f);
                                const float tail = tailX * tailX *
                                    (3.0f - 2.0f * tailX) *
                                    std::exp(-std::max(0.0f,
                                        elapsed - aftershockStart) / 0.090f);
                                amplitude = reaction.intensity *
                                    (0.92f * impact + 0.27f * rebound +
                                     longWeight * (0.29f * sustain +
                                         0.37f * aftershock + 0.14f * tail));
                            }
                            // At 100% master intensity, a power-1.0 cue now
                            // peaks just below the actuator's full-scale
                            // sample range. Keep cue power in reaction.intensity
                            // so quieter cinematic events remain quieter.
                            amplitude *= duration <= 0.37f ? 1.23f : 1.36f;
                            frequencyA = 88.0 - 16.0 * progress;
                            frequencyB = 176.0 - 31.0 * progress;
                            break;
                        }
                        case PlayerHapticProfile::Knockdown: {
                            const float firstImpact = attack *
                                std::exp(-6.0f * progress);
                            float floorImpact = 0.0f;
                            if (progress > 0.40f) {
                                const float floorProgress = progress - 0.40f;
                                const float floorAttackX = std::clamp(
                                    floorProgress / 0.035f,
                                    0.0f,
                                    1.0f);
                                const float floorAttack = floorAttackX * floorAttackX *
                                    (3.0f - 2.0f * floorAttackX);
                                floorImpact = floorAttack *
                                    std::exp(-9.0f * floorProgress);
                            }
                            wave = waveA * 0.88f + waveB * 0.12f +
                                reaction.filteredNoise * floorImpact * 0.13f;
                            amplitude = 0.500f * reaction.intensity *
                                (firstImpact * 0.56f + floorImpact);
                            leftGain = 1.0f;
                            rightGain = 0.90f;
                            frequencyA = 48.0 + 24.0 * (1.0 - progress);
                            frequencyB = 96.0;
                            break;
                        }
                        case PlayerHapticProfile::Grab: {
                            const float onset = attack *
                                std::exp(-10.0f * progress);
                            const float window = std::pow(
                                std::max(0.0f, static_cast<float>(
                                    std::sin(3.14159265358979323846 * progress))),
                                0.42f);
                            const float clampWave = static_cast<float>(
                                0.72 + 0.28 * std::sin(reaction.modPhase));
                            const float release = std::exp(
                                -145.0f *
                                (progress - 0.84f) * (progress - 0.84f));
                            const float pan = static_cast<float>(
                                0.5 + 0.38 * std::sin(reaction.modPhase));
                            wave = waveA * 0.78f + waveB * 0.22f;
                            amplitude = reaction.intensity *
                                (0.470f * window * clampWave +
                                 0.240f * onset +
                                 0.100f * release);
                            leftGain = 0.58f + (1.0f - pan) * 0.72f;
                            rightGain = 0.58f + pan * 0.72f;
                            frequencyA = 67.0;
                            frequencyB = 111.0;
                            modulationFrequency = 6.2;
                            break;
                        }
                        case PlayerHapticProfile::Dodge: {
                            const float window = std::pow(
                                std::max(0.0f, static_cast<float>(
                                    std::sin(3.14159265358979323846 * progress))),
                                1.20f);
                            wave = waveA * 0.52f + waveB * 0.20f +
                                reaction.filteredNoise * 0.52f;
                            amplitude = 0.145f * reaction.intensity * window;
                            leftGain = 1.06f - progress * 0.50f;
                            rightGain = 0.56f + progress * 0.50f;
                            frequencyA = 148.0 + progress * 72.0;
                            frequencyB = 274.0;
                            break;
                        }
                        case PlayerHapticProfile::JustDodge: {
                            const float window = std::pow(
                                std::max(0.0f, static_cast<float>(
                                    std::sin(3.14159265358979323846 * progress))),
                                0.78f);
                            const float accent = std::exp(
                                -180.0f *
                                (progress - 0.18f) * (progress - 0.18f));
                            wave = waveA * 0.58f + waveB * 0.42f;
                            amplitude = 0.225f * reaction.intensity *
                                (window * 0.72f + accent * 0.48f);
                            leftGain = 1.10f - progress * 0.44f;
                            rightGain = 0.66f + progress * 0.44f;
                            frequencyA = 176.0 + progress * 86.0;
                            frequencyB = 336.0;
                            break;
                        }
                        default:
                            break;
                        }

                        left += wave * amplitude * leftGain;
                        right += wave * amplitude * rightGain;
                        reaction.phaseA += TwoPi * frequencyA / SampleRate;
                        reaction.phaseB += TwoPi * frequencyB / SampleRate;
                        reaction.modPhase +=
                            TwoPi * modulationFrequency / SampleRate;
                        if (reaction.phaseA >= TwoPi) reaction.phaseA -= TwoPi;
                        if (reaction.phaseB >= TwoPi) reaction.phaseB -= TwoPi;
                        if (reaction.modPhase >= TwoPi) reaction.modPhase -= TwoPi;
                        ++reaction.frame;
                    }

                    // Guard, deflect and Issen each use their own authored
                    // waveform. Their per-category intensity is applied here;
                    // the global haptic setting remains the final master gain.
                    for (CombatHapticVoice& combat : m_combatHaptics) {
                        if (combat.frame >= combat.totalFrames) continue;
                        if (combat.profile == CombatHapticProfile::Parry ||
                            combat.profile == CombatHapticProfile::ProjectileParry) {
                            const auto sample = repel_impact_haptics::Sample(
                                combat.frame,
                                combat.profile == CombatHapticProfile::ProjectileParry,
                                combat.intensity);
                            left += sample.left;
                            right += sample.right;
                            ++combat.frame;
                            continue;
                        }
                        const auto executionKind = execution_haptics::KindFromProfile(
                            static_cast<uint8_t>(combat.profile));
                        if (executionKind != execution_haptics::Kind::None) {
                            const auto sample = execution_haptics::Sample(
                                combat.frame,
                                executionKind,
                                combat.intensity,
                                combat.motionVariant);
                            left += sample.left;
                            right += sample.right;
                            ++combat.frame;
                            continue;
                        }
                        const float progress = std::clamp(
                            static_cast<float>(combat.frame) /
                                std::max(1.0f,
                                    static_cast<float>(combat.totalFrames - 1)),
                            0.0f,
                            1.0f);
                        const float attackX = std::clamp(
                            progress / 0.018f, 0.0f, 1.0f);
                        const float attack = attackX * attackX *
                            (3.0f - 2.0f * attackX);
                        const float waveA = static_cast<float>(
                            std::sin(combat.phaseA));
                        const float waveB = static_cast<float>(
                            std::sin(combat.phaseB));
                        float wave = waveA;
                        float amplitude = 0.0f;
                        float leftGain = 1.0f;
                        float rightGain = 1.0f;
                        double frequencyA = 80.0;
                        double frequencyB = 150.0;

                        switch (combat.profile) {
                        case CombatHapticProfile::Guard: {
                            // Make a blocked hit readable over the game's own
                            // rumble: one heavy body impact, then two lower
                            // bracing pulses as the weapon absorbs the force.
                            const float body = attack *
                                std::exp(-5.2f * progress);
                            const float brace = attack * std::exp(
                                -145.0f *
                                (progress - 0.29f) * (progress - 0.29f));
                            const float settle = attack * std::exp(
                                -190.0f *
                                (progress - 0.59f) * (progress - 0.59f));
                            wave = waveA * 0.92f + waveB * 0.08f;
                            amplitude = combat.intensity *
                                (0.92f * body + 0.74f * brace +
                                 0.42f * settle);
                            leftGain = 1.02f;
                            rightGain = 0.96f;
                            frequencyA = 58.0 - 8.0 * progress;
                            frequencyB = 88.0;
                            break;
                        }
                        case CombatHapticProfile::ParryActivation: {
                            // L1+X preparation gets a compact confirmation so
                            // the skill is perceptible even before a hit lands;
                            // success still receives the heavier profiles.
                            const float engage = attack *
                                std::exp(-5.2f * progress);
                            const float armed = attack * std::exp(
                                -155.0f *
                                (progress - 0.44f) * (progress - 0.44f));
                            wave = waveA * 0.92f + waveB * 0.08f;
                            amplitude = combat.intensity *
                                (0.74f * engage + 0.48f * armed);
                            leftGain = 0.98f;
                            rightGain = 0.98f;
                            frequencyA = 58.0;
                            frequencyB = 88.0;
                            break;
                        }
                        case CombatHapticProfile::Deflect: {
                            // Just Guard/deflect gathers pressure toward the
                            // centre before a single, clean release accent.
                            const float gather = attack * std::pow(
                                std::max(0.0f, static_cast<float>(
                                    std::sin(3.14159265358979323846 * progress))),
                                0.82f) * std::exp(-1.25f * progress);
                            const float release = attack * std::exp(
                                -210.0f *
                                (progress - 0.49f) * (progress - 0.49f));
                            const float confirm = attack * std::exp(
                                -250.0f *
                                (progress - 0.74f) * (progress - 0.74f));
                            wave = waveA * 0.76f + waveB * 0.24f;
                            amplitude = combat.intensity *
                                (0.48f * gather + 0.86f * release +
                                 0.38f * confirm);
                            leftGain = 1.16f - progress * 0.32f;
                            rightGain = 0.84f + progress * 0.32f;
                            frequencyA = 84.0 + 104.0 * progress;
                            frequencyB = 172.0 + 116.0 * progress;
                            break;
                        }
                        case CombatHapticProfile::Issen: {
                            // A decisive cut travels from left to right, with
                            // an impact near the opening and a short exit snap.
                            const float cut = attack * std::exp(
                                -390.0f *
                                (progress - 0.13f) * (progress - 0.13f));
                            const float sweep = attack * std::pow(
                                std::max(0.0f, static_cast<float>(
                                    std::sin(3.14159265358979323846 * progress))),
                                0.70f) * std::exp(-1.35f * progress);
                            const float exit = attack * std::exp(
                                -250.0f *
                                (progress - 0.66f) * (progress - 0.66f));
                            wave = waveA * 0.70f + waveB * 0.30f;
                            amplitude = combat.intensity *
                                (0.70f * cut + 0.28f * sweep + 0.27f * exit);
                            leftGain = 1.25f - progress * 0.66f;
                            rightGain = 0.59f + progress * 0.66f;
                            frequencyA = 112.0 + 178.0 * progress;
                            frequencyB = 228.0 + 142.0 * progress;
                            break;
                        }
                        default:
                            break;
                        }

                        left += wave * amplitude * leftGain;
                        right += wave * amplitude * rightGain;
                        combat.phaseA += TwoPi * frequencyA / SampleRate;
                        combat.phaseB += TwoPi * frequencyB / SampleRate;
                        if (combat.phaseA >= TwoPi) combat.phaseA -= TwoPi;
                        if (combat.phaseB >= TwoPi) combat.phaseB -= TwoPi;
                        ++combat.frame;
                    }

                    // Environment interactions are synthesized from the
                    // concrete gimmick state rather than from a generic input
                    // click. Doors carry a textured hinge/latch motion, levers
                    // build resistance before a snap, and elevator start/stop
                    // have distinct engagement and landing transients.
                    for (EnvironmentInteractionVoice& interaction :
                            m_environmentInteractions) {
                        if (interaction.frame >= interaction.totalFrames) continue;
                        const float progress = std::clamp(
                            static_cast<float>(interaction.frame) /
                                std::max(1.0f,
                                    static_cast<float>(interaction.totalFrames - 1)),
                            0.0f,
                            1.0f);
                        const float attackX = std::clamp(
                            progress / 0.022f, 0.0f, 1.0f);
                        const float attack =
                            attackX * attackX * (3.0f - 2.0f * attackX);
                        interaction.noiseState =
                            interaction.noiseState * 1664525u + 1013904223u;
                        const float rawInteractionNoise =
                            static_cast<float>(
                                (interaction.noiseState >> 8) & 0xFFFFu) /
                                32767.5f - 1.0f;
                        interaction.filteredNoise +=
                            (rawInteractionNoise - interaction.filteredNoise) *
                            0.026f;

                        const float waveA = static_cast<float>(
                            std::sin(interaction.phaseA));
                        const float waveB = static_cast<float>(
                            std::sin(interaction.phaseB));
                        const float modWave = static_cast<float>(
                            std::sin(interaction.modPhase));
                        float wave = waveA;
                        float amplitude = 0.0f;
                        float leftGain = 1.0f;
                        float rightGain = 1.0f;
                        double frequencyA = 92.0;
                        double frequencyB = 178.0;
                        double modulationFrequency = 5.0;

                        switch (interaction.profile) {
                        case EnvironmentInteractionProfile::GenericSuccess: {
                            const float body = attack * std::exp(-6.4f * progress);
                            const float lock = std::exp(
                                -260.0f * (progress - 0.30f) *
                                    (progress - 0.30f));
                            wave = waveA * 0.54f + waveB * 0.46f;
                            amplitude = interaction.intensity *
                                (0.300f * body + 0.355f * lock);
                            frequencyA = 126.0;
                            frequencyB = 268.0;
                            break;
                        }
                        case EnvironmentInteractionProfile::Door: {
                            const float hinge = attack * std::pow(
                                std::max(0.0f, static_cast<float>(
                                    std::sin(3.14159265358979323846 * progress))),
                                0.72f) * std::exp(-0.65f * progress);
                            const float release = std::exp(
                                -330.0f * (progress - 0.055f) *
                                    (progress - 0.055f));
                            wave = waveA * 0.68f + waveB * 0.20f +
                                interaction.filteredNoise *
                                    (0.12f + 0.10f * std::fabs(modWave));
                            amplitude = interaction.intensity *
                                (0.330f * hinge + 0.500f * release);
                            leftGain = 1.0f;
                            rightGain = 0.82f + 0.18f * progress;
                            frequencyA = 57.0 + 18.0 * progress;
                            frequencyB = 119.0 + 25.0 * progress;
                            modulationFrequency = 7.2;
                            break;
                        }
                        case EnvironmentInteractionProfile::Lever: {
                            const float resistance = attack * std::pow(
                                std::max(0.0f, static_cast<float>(
                                    std::sin(3.14159265358979323846 * progress))),
                                0.58f);
                            const float snap = std::exp(
                                -520.0f * (progress - 0.59f) *
                                    (progress - 0.59f));
                            const float detent = std::exp(
                                -360.0f * (progress - 0.84f) *
                                    (progress - 0.84f));
                            wave = waveA * 0.68f + waveB * 0.32f;
                            amplitude = interaction.intensity *
                                (0.220f * resistance + 0.610f * snap +
                                 0.310f * detent);
                            leftGain = 1.0f - 0.15f * progress;
                            rightGain = 0.82f + 0.18f * progress;
                            frequencyA = 72.0 + 62.0 * progress;
                            frequencyB = 174.0 + 48.0 * progress;
                            modulationFrequency = 8.5;
                            break;
                        }
                        case EnvironmentInteractionProfile::ElevatorStart: {
                            const float motorWake = attack *
                                std::exp(-3.0f * progress);
                            const float gearBite = std::exp(
                                -390.0f * (progress - 0.16f) *
                                    (progress - 0.16f));
                            const float takeLoad = std::exp(
                                -230.0f * (progress - 0.58f) *
                                    (progress - 0.58f));
                            wave = waveA * 0.78f + waveB * 0.16f +
                                interaction.filteredNoise * 0.10f;
                            amplitude = interaction.intensity *
                                (0.255f * motorWake + 0.525f * gearBite +
                                 0.350f * takeLoad);
                            frequencyA = 49.0 + 22.0 * progress;
                            frequencyB = 91.0;
                            modulationFrequency = 10.0;
                            break;
                        }
                        case EnvironmentInteractionProfile::ElevatorStop: {
                            const float landing = std::exp(
                                -430.0f * (progress - 0.075f) *
                                    (progress - 0.075f));
                            const float rebound = std::exp(
                                -180.0f * (progress - 0.42f) *
                                    (progress - 0.42f));
                            const float brakeLock = std::exp(
                                -330.0f * (progress - 0.74f) *
                                    (progress - 0.74f));
                            wave = waveA * 0.84f + waveB * 0.16f +
                                interaction.filteredNoise * 0.08f;
                            amplitude = interaction.intensity *
                                (0.680f * landing + 0.355f * rebound +
                                 0.390f * brakeLock);
                            leftGain = 1.0f;
                            rightGain = 0.92f;
                            frequencyA = 46.0 + 20.0 * (1.0f - progress);
                            frequencyB = 98.0;
                            modulationFrequency = 5.5;
                            break;
                        }
                        case EnvironmentInteractionProfile::TreasureChestOpen: {
                            const auto chestFrame = treasure_chest_haptics::Evaluate(
                                treasure_chest_haptics::Stage::Open,
                                progress, attack, modWave);
                            wave = waveA * chestFrame.waveMixA +
                                waveB * chestFrame.waveMixB +
                                interaction.filteredNoise * chestFrame.noiseMix;
                            amplitude = interaction.intensity * chestFrame.amplitude;
                            leftGain = chestFrame.leftGain;
                            rightGain = chestFrame.rightGain;
                            frequencyA = chestFrame.frequencyA;
                            frequencyB = chestFrame.frequencyB;
                            modulationFrequency = chestFrame.modulationFrequency;
                            break;
                        }
                        case EnvironmentInteractionProfile::TreasureChestPush: {
                            const auto chestFrame = treasure_chest_haptics::Evaluate(
                                treasure_chest_haptics::Stage::Push,
                                progress, attack, modWave);
                            wave = waveA * chestFrame.waveMixA +
                                waveB * chestFrame.waveMixB +
                                interaction.filteredNoise * chestFrame.noiseMix;
                            amplitude = interaction.intensity * chestFrame.amplitude;
                            leftGain = chestFrame.leftGain;
                            rightGain = chestFrame.rightGain;
                            frequencyA = chestFrame.frequencyA;
                            frequencyB = chestFrame.frequencyB;
                            modulationFrequency = chestFrame.modulationFrequency;
                            break;
                        }
                        case EnvironmentInteractionProfile::TreasureChestLidLand: {
                            const auto chestFrame = treasure_chest_haptics::Evaluate(
                                treasure_chest_haptics::Stage::LidLand,
                                progress, attack, modWave);
                            wave = waveA * chestFrame.waveMixA +
                                waveB * chestFrame.waveMixB +
                                interaction.filteredNoise * chestFrame.noiseMix;
                            amplitude = interaction.intensity * chestFrame.amplitude;
                            leftGain = chestFrame.leftGain;
                            rightGain = chestFrame.rightGain;
                            frequencyA = chestFrame.frequencyA;
                            frequencyB = chestFrame.frequencyB;
                            modulationFrequency = chestFrame.modulationFrequency;
                            break;
                        }
                        case EnvironmentInteractionProfile::ElevatorMove:
                            break;
                        }

                        const float interactionLeft = wave * amplitude * leftGain;
                        const float interactionRight = wave * amplitude * rightGain;
                        left += interactionLeft;
                        right += interactionRight;
                        const float outputLeft = std::clamp(interactionLeft * m_hapticIntensity, -1.0f, 1.0f);
                        const float outputRight = std::clamp(interactionRight * m_hapticIntensity, -1.0f, 1.0f);
                        interaction.outputPeakLeft = std::max(interaction.outputPeakLeft, std::fabs(outputLeft));
                        interaction.outputPeakRight = std::max(interaction.outputPeakRight, std::fabs(outputRight));
                        interaction.outputEnergy += static_cast<double>(outputLeft) * outputLeft +
                            static_cast<double>(outputRight) * outputRight;
                        if (std::fabs(outputLeft) > 0.0001f || std::fabs(outputRight) > 0.0001f)
                            environmentSubmitted |= 1u << static_cast<unsigned>(interaction.profile);
                        interaction.phaseA +=
                            TwoPi * frequencyA / SampleRate;
                        interaction.phaseB +=
                            TwoPi * frequencyB / SampleRate;
                        interaction.modPhase +=
                            TwoPi * modulationFrequency / SampleRate;
                        if (interaction.phaseA >= TwoPi) {
                            interaction.phaseA -= TwoPi;
                        }
                        if (interaction.phaseB >= TwoPi) {
                            interaction.phaseB -= TwoPi;
                        }
                        if (interaction.modPhase >= TwoPi) {
                            interaction.modPhase -= TwoPi;
                        }
                        ++interaction.frame;
                    }

                    // Elevator motion is a stateful, self-expiring mechanical
                    // bed. It stays present only while the player is actually
                    // inside the moving elevator and update keepalives arrive.
                    if (m_elevatorHoldFrames > 0) {
                        --m_elevatorHoldFrames;
                    } else {
                        m_elevatorTarget = 0.0f;
                    }
                    const float elevatorSmoothing =
                        m_elevatorTarget > m_elevatorLevel ? 0.00135f : 0.00055f;
                    m_elevatorLevel +=
                        (m_elevatorTarget - m_elevatorLevel) * elevatorSmoothing;
                    if (m_elevatorTarget == 0.0f &&
                        m_elevatorLevel < 0.0001f) {
                        m_elevatorLevel = 0.0f;
                    }
                    m_elevatorNoiseState =
                        m_elevatorNoiseState * 1664525u + 1013904223u;
                    const float rawElevatorNoise =
                        static_cast<float>(
                            (m_elevatorNoiseState >> 8) & 0xFFFFu) /
                            32767.5f - 1.0f;
                    m_elevatorNoiseLow +=
                        (rawElevatorNoise - m_elevatorNoiseLow) * 0.012f;
                    const float elevatorMod = static_cast<float>(
                        std::sin(m_elevatorModPhase));
                    const float elevatorWave = static_cast<float>(
                        0.69 * std::sin(m_elevatorPhase) +
                        0.23 * std::sin(m_elevatorHarmonicPhase)) +
                        m_elevatorNoiseLow * 0.16f;
                    const float elevatorAmplitude =
                        0.225f * std::pow(m_elevatorLevel, 0.82f) *
                        (0.84f + 0.16f * elevatorMod);
                    left += elevatorWave * elevatorAmplitude *
                        (0.95f - 0.05f * elevatorMod);
                    right += elevatorWave * elevatorAmplitude *
                        (0.95f + 0.05f * elevatorMod);
                    m_elevatorPhase += TwoPi * 54.0 / SampleRate;
                    m_elevatorHarmonicPhase += TwoPi * 93.0 / SampleRate;
                    m_elevatorModPhase += TwoPi * 4.4 / SampleRate;
                    if (m_elevatorPhase >= TwoPi) m_elevatorPhase -= TwoPi;
                    if (m_elevatorHarmonicPhase >= TwoPi) {
                        m_elevatorHarmonicPhase -= TwoPi;
                    }
                    if (m_elevatorModPhase >= TwoPi) {
                        m_elevatorModPhase -= TwoPi;
                    }

                    // The first two channels feed the controller's ordinary
                    // audio path; channels 3/4 remain dedicated to the two
                    // haptic actuators. Soul absorption uses isolated material
                    // from the game's own soul_pl bank, so speech, music and
                    // unrelated effects stay on the user's main output.
                    const float soulSmoothing = m_soulSpeakerTarget > m_soulSpeakerLevel
                        ? 0.00085f
                        : 0.00022f;
                    m_soulSpeakerLevel +=
                        (m_soulSpeakerTarget - m_soulSpeakerLevel) * soulSmoothing;
                    if (m_soulSpeakerTarget == 0.0f && m_soulSpeakerLevel < 0.0001f) {
                        m_soulSpeakerLevel = 0.0f;
                    }

                    if (m_speakerAudioLoaded) {
                        // These are decoded directly from the installed game's
                        // soul_pl Wwise bank. Event 3432052047 contains an
                        // authored intro and an exact loop region.
                        const SpeakerClip& loop = m_speakerClips[1];
                        if (m_soulSpeakerLevel > 0.0f && loop.Frames() > 0) {
                            const size_t loopStart = std::min<size_t>(107452, loop.Frames() - 1);
                            const size_t loopEnd = std::min<size_t>(599205, loop.Frames());
                            if (m_soulLoopFrame >= loopEnd) m_soulLoopFrame = loopStart;
                            speaker += loop.monoSamples[m_soulLoopFrame] *
                                m_soulSpeakerLevel * 0.70f *
                                m_soulSpeakerVolume;
                            ++m_soulLoopFrame;
                        }
                        for (SpeakerFileVoice& voice : m_speakerFileVoices) {
                            if (voice.clipIndex >= m_speakerClips.size()) continue;
                            const SpeakerClip& clip = m_speakerClips[voice.clipIndex];
                            if (voice.frame < clip.Frames()) {
                                speaker += clip.monoSamples[voice.frame] *
                                    voice.gain * SpeakerVolumeForCue(voice.cue);
                                ++voice.frame;
                            }
                        }
                    } else {
                        m_speakerNoiseState =
                            m_speakerNoiseState * 1664525u + 1013904223u;
                        const float soulRawNoise =
                            static_cast<float>((m_speakerNoiseState >> 8) & 0xFFFFu) /
                                32767.5f - 1.0f;
                        m_speakerNoiseLow +=
                            (soulRawNoise - m_speakerNoiseLow) * 0.032f;
                        const float soulAir = std::clamp(
                            (soulRawNoise - m_speakerNoiseLow) * 1.45f,
                            -1.0f,
                            1.0f);
                        const float soulBed = static_cast<float>(
                            0.22 * std::sin(m_soulSpeakerPhaseA) +
                            0.18 * std::sin(m_soulSpeakerPhaseB) +
                            0.12 * std::sin(m_soulSpeakerPhaseC) +
                            0.48 * soulAir);
                        const float soulBreath = static_cast<float>(
                            0.72 + 0.20 * std::sin(m_soulSpeakerModPhase) +
                            0.08 * std::sin(m_soulSpeakerModPhase * 0.37));
                        speaker += soulBed * m_soulSpeakerLevel * 0.092f *
                            soulBreath * m_soulSpeakerVolume;
                    }

                    if (m_combatSpeakerAudioLoaded) {
                        for (SpeakerFileVoice& voice :
                             m_combatSpeakerFileVoices) {
                            if (voice.clipIndex >=
                                m_combatSpeakerClips.size()) {
                                continue;
                            }
                            if (voice.delayFrames > 0) {
                                --voice.delayFrames;
                                continue;
                            }
                            const SpeakerClip& clip =
                                m_combatSpeakerClips[voice.clipIndex];
                            const bool deflect =
                                voice.cue == ControllerSpeakerCue::CombatDeflect;
                            const size_t playbackFrames =
                                combat_speaker_timing::PlaybackFrames(
                                    deflect, clip.Frames(), voice.deflectVariant);
                            if (voice.frame < playbackFrames) {
                                combatSpeaker += clip.monoSamples[voice.frame] *
                                    voice.gain * SpeakerVolumeForCue(voice.cue) *
                                    combat_speaker_timing::Envelope(
                                        deflect, voice.frame, playbackFrames);
                                ++voice.frame;
                            }
                        }
                    }

                    // Procedural event cues are independent of whether the
                    // original soul bank is available. Combat cues must still
                    // reach the front speaker channels when soul WAVs loaded.
                    for (SpeakerCueVoice& cue : m_speakerCues) {
                        if (cue.delayFrames > 0) {
                            --cue.delayFrames;
                            continue;
                        }
                        if (cue.frame >= cue.totalFrames) continue;
                        const float progress = static_cast<float>(cue.frame) /
                            static_cast<float>(std::max(1u, cue.totalFrames));
                        cue.noiseState = cue.noiseState * 1664525u + 1013904223u;
                        const float rawCueNoise =
                            static_cast<float>((cue.noiseState >> 8) & 0xFFFFu) /
                                32767.5f - 1.0f;
                        cue.filteredNoise +=
                            (rawCueNoise - cue.filteredNoise) * 0.080f;
                        float envelope = 0.0f;
                        float amplitude = 0.0f;
                        float toneMixA = 0.38f;
                        float toneMixB = 0.24f;
                        float noiseMix = 0.38f;
                        double frequencyA = 1300.0;
                        double frequencyB = 2200.0;
                        switch (cue.cue) {
                        case ControllerSpeakerCue::SoulStart: {
                            const float attack = std::clamp(progress / 0.12f, 0.0f, 1.0f);
                            const float release = std::clamp(
                                (1.0f - progress) / 0.24f, 0.0f, 1.0f);
                            const float smoothAttack = attack * attack * (3.0f - 2.0f * attack);
                            envelope = smoothAttack * std::sqrt(release);
                            amplitude = 0.205f * envelope;
                            frequencyA = 980.0 + progress * 1760.0;
                            frequencyB = 1640.0 + progress * 2380.0;
                            toneMixA = 0.31f;
                            toneMixB = 0.22f;
                            noiseMix = 0.47f;
                            break;
                        }
                        case ControllerSpeakerCue::SoulCollect: {
                            const float attack = std::clamp(progress / 0.055f, 0.0f, 1.0f);
                            const float primary = attack * std::exp(-3.9f * progress);
                            const float sparkleA = std::exp(
                                -115.0f * (progress - 0.28f) * (progress - 0.28f));
                            const float sparkleB = std::exp(
                                -150.0f * (progress - 0.51f) * (progress - 0.51f));
                            amplitude = 0.245f * primary +
                                0.070f * sparkleA + 0.048f * sparkleB;
                            frequencyA = 1480.0 + progress * 1580.0;
                            frequencyB = 2260.0 + progress * 2050.0;
                            toneMixA = 0.34f;
                            toneMixB = 0.28f;
                            noiseMix = 0.38f;
                            break;
                        }
                        case ControllerSpeakerCue::SoulStop: {
                            const float attack = std::clamp(progress / 0.10f, 0.0f, 1.0f);
                            envelope = attack * std::pow(1.0f - progress, 1.45f);
                            amplitude = 0.145f * envelope;
                            frequencyA = 2100.0 - progress * 920.0;
                            frequencyB = 3180.0 - progress * 1330.0;
                            toneMixA = 0.27f;
                            toneMixB = 0.18f;
                            noiseMix = 0.55f;
                            break;
                        }
                        case ControllerSpeakerCue::CombatClash:
                        case ControllerSpeakerCue::CombatClashHit: {
                            const float impact = std::exp(-25.0f * progress);
                            const float ring = std::exp(-6.2f * progress);
                            const float rebound = std::exp(
                                -190.0f * (progress - 0.23f) *
                                (progress - 0.23f));
                            amplitude = 0.28f * impact + 0.125f * ring +
                                0.070f * rebound;
                            frequencyA = 920.0 + progress * 250.0;
                            frequencyB = 2050.0 - progress * 340.0;
                            toneMixA = 0.38f;
                            toneMixB = 0.25f;
                            noiseMix = 0.37f;
                            break;
                        }
                        case ControllerSpeakerCue::CombatGuard:
                        case ControllerSpeakerCue::CombatGuardDirectional: {
                            const float attack = std::clamp(
                                progress / 0.025f, 0.0f, 1.0f);
                            amplitude = 0.34f * attack *
                                std::exp(-11.0f * progress);
                            frequencyA = 780.0 - progress * 90.0;
                            frequencyB = 1420.0 - progress * 210.0;
                            toneMixA = 0.35f;
                            toneMixB = 0.18f;
                            noiseMix = 0.47f;
                            break;
                        }
                        case ControllerSpeakerCue::CombatDeflect:
                        case ControllerSpeakerCue::CombatParryBlade:
                        case ControllerSpeakerCue::CombatParryProjectile: {
                            const float attack = std::clamp(
                                progress / 0.018f, 0.0f, 1.0f);
                            const float ring = attack * std::exp(-5.4f * progress);
                            const float sparkle = std::exp(
                                -155.0f * (progress - 0.31f) *
                                (progress - 0.31f));
                            amplitude = 0.285f * ring + 0.055f * sparkle;
                            frequencyA = 1650.0 + progress * 390.0;
                            frequencyB = 2760.0 + progress * 430.0;
                            toneMixA = 0.44f;
                            toneMixB = 0.31f;
                            noiseMix = 0.25f;
                            break;
                        }
                        case ControllerSpeakerCue::CombatExecution:
                        case ControllerSpeakerCue::CombatIssen:
                        case ControllerSpeakerCue::CombatClashSuccess: {
                            const float body = std::exp(-7.5f * progress);
                            const float slash = std::exp(
                                -75.0f * (progress - 0.14f) *
                                (progress - 0.14f));
                            const float finish = std::exp(
                                -135.0f * (progress - 0.48f) *
                                (progress - 0.48f));
                            amplitude = 0.13f * body + 0.245f * slash +
                                0.105f * finish;
                            frequencyA = 720.0 + progress * 360.0;
                            frequencyB = 1540.0 - progress * 420.0;
                            toneMixA = 0.29f;
                            toneMixB = 0.21f;
                            noiseMix = 0.50f;
                            break;
                        }
                        default:
                            break;
                        }
                        const float cueAir = std::clamp(
                            (rawCueNoise - cue.filteredNoise) * 1.35f,
                            -1.0f,
                            1.0f);
                        const float cueWave = static_cast<float>(
                            toneMixA * std::sin(cue.phaseA) +
                            toneMixB * std::sin(cue.phaseB) +
                            noiseMix * cueAir);
                        speaker += cueWave * amplitude *
                            SpeakerVolumeForCue(cue.cue);
                        cue.phaseA += TwoPi * frequencyA / SampleRate;
                        cue.phaseB += TwoPi * frequencyB / SampleRate;
                        if (cue.phaseA >= TwoPi) cue.phaseA -= TwoPi;
                        if (cue.phaseB >= TwoPi) cue.phaseB -= TwoPi;
                        ++cue.frame;
                    }

                    // Gauntlet dialogue has its own lazily loaded clip and is
                    // independent of the soul-effect fallback above.
                    if (m_gauntletVoiceActive) {
                        if (m_gauntletVoiceFrame < m_gauntletVoiceClip.Frames()) {
                            // Dialogue media is mastered much quieter than the
                            // soul cues (the probe peaks near -15 dBFS).  Bring
                            // it into the controller speaker's useful range;
                            // the final output limiter still prevents clipping.
                            speaker += m_gauntletVoiceClip.monoSamples[
                                m_gauntletVoiceFrame] * 2.60f *
                                m_gauntletVoiceVolume;
                            ++m_gauntletVoiceFrame;
                        } else {
                            m_gauntletVoiceActive = false;
                        }
                    }

                    // Replay the three measured built-in presets entirely on
                    // the helper side. Their motor lanes enter the same
                    // calibrated carrier, smoothing, harmonic and impact path
                    // as native rumble, but no managed game object is created.
                    std::array<float, 4> capturedPresetPower{};
                    for (CapturedPresetVoice& voice : m_capturedPresetVoices) {
                        if (voice.frame >= voice.totalFrames) continue;
                        const auto profile =
                            captured_haptic_presets::ProfileFor(voice.preset);
                        for (size_t motor = 0;
                             motor < capturedPresetPower.size();
                             ++motor) {
                            const float power =
                                captured_haptic_presets::CalibrateMotorPower(
                                    profile.motorPower[motor]);
                            capturedPresetPower[motor] = std::max(
                                capturedPresetPower[motor], power);
                            impactAccent[motor] = std::max(
                                impactAccent[motor],
                                power * std::exp(
                                    -static_cast<float>(voice.frame) / 720.0f));
                        }
                        ++voice.frame;
                    }

                    // Steam Input presents the DualSense as a virtual XInput pad,
                    // so the PC build normally emits only four motor-power lanes.
                    // Preserve their original envelopes, but turn them into tactile
                    // audio carriers for the DualSense's left/right actuators.
                    for (size_t motor = 0; motor < m_motorPower.size(); ++motor) {
                        float pulse = capturedPresetPower[motor];
                        for (MotorPulse& active : m_motorPulses[motor]) {
                            float envelope = 1.0f;
                            if (active.attenuate && active.totalFrames > 0) {
                                const float progress = 1.0f -
                                    static_cast<float>(active.remainingFrames) /
                                    static_cast<float>(active.totalFrames);
                                float eased = progress;
                                // The shipped presets use Linear (0) and
                                // InOutQuart (12). Preserve the authored curve.
                                if (active.easeType == 12) {
                                    eased = progress < 0.5f
                                        ? 8.0f * std::pow(progress, 4.0f)
                                        : 1.0f - std::pow(-2.0f * progress + 2.0f, 4.0f) / 2.0f;
                                }
                                envelope = 1.0f - std::clamp(eased, 0.0f, 1.0f);
                            }
                            pulse = std::max(pulse, active.power * envelope);
                            const uint32_t elapsedFrames =
                                active.totalFrames - active.remainingFrames;
                            impactAccent[motor] = std::max(
                                impactAccent[motor],
                                active.power *
                                    std::exp(-static_cast<float>(elapsedFrames) / 720.0f));
                            if (active.remainingFrames > 0) --active.remainingFrames;
                        }
                        auto& pulses = m_motorPulses[motor];
                        pulses.erase(std::remove_if(pulses.begin(), pulses.end(),
                            [](const MotorPulse& active) {
                                return active.remainingFrames == 0;
                            }), pulses.end());
                        const float target = std::max(m_liveMotorTargets.Power()[motor], pulse);
                        const float step = target > m_motorPower[motor] ? 0.014f : 0.0045f;
                        m_motorPower[motor] +=
                            (target - m_motorPower[motor]) * step;
                        if (target == 0.0f && m_motorPower[motor] < 0.0001f) {
                            m_motorPower[motor] = 0.0f;
                        }
                    }

                    const float lowAmplitude =
                        std::pow(m_motorPower[0], 0.68f) * 0.78f;
                    const float highAmplitude =
                        std::pow(m_motorPower[1], 0.72f) * 0.66f;
                    const float leftTriggerAmplitude =
                        std::pow(m_motorPower[2], 0.72f) * 0.72f;
                    const float rightTriggerAmplitude =
                        std::pow(m_motorPower[3], 0.72f) * 0.72f;

                    const float lowWave = static_cast<float>(
                        0.78 * std::sin(m_lowPhase) +
                        0.22 * std::sin(m_lowHarmonicPhase));
                    const float highWave = static_cast<float>(
                        0.82 * std::sin(m_highPhase) +
                        0.18 * std::sin(m_highHarmonicPhase));
                    const float triggerWave = static_cast<float>(
                        0.84 * std::sin(m_triggerPhase) +
                        0.16 * std::sin(m_triggerHarmonicPhase));
                    const float impactWave = static_cast<float>(
                        0.70 * std::sin(m_impactPhase) +
                        0.30 * std::sin(m_impactHarmonicPhase));

                    // Synthetic environmental bed. Rain is deliberately made
                    // from separated, spatialized droplet contacts rather than
                    // continuous noise: a permanent buzz reads like a motor,
                    // not water striking the character. Wind remains a slowly
                    // modulated low-frequency swell.
                    m_rainLevel += (m_rainTarget - m_rainLevel) * 0.00035f;
                    m_windLevel += (m_windTarget - m_windLevel) * 0.00012f;
                    float rainLeft = 0.0f;
                    float rainRight = 0.0f;
                    auto nextRainRandom = [this]() {
                        m_noiseState = m_noiseState * 1664525u + 1013904223u;
                        return static_cast<float>((m_noiseState >> 8) & 0xFFFFu) /
                            65535.0f;
                    };
                    if (m_rainLevel > 0.01f) {
                        if (m_rainDropCountdown > 0) --m_rainDropCountdown;
                        if (m_rainDropCountdown == 0) {
                            auto freeDrop = std::find_if(
                                m_rainDrops.begin(), m_rainDrops.end(),
                                [](const RainDropVoice& drop) {
                                    return drop.frame >= drop.totalFrames;
                                });
                            const float durationRandom = nextRainRandom();
                            const float strengthRandom = nextRainRandom();
                            const float panRandom = nextRainRandom();
                            const float pitchRandom = nextRainRandom();
                            const float clusterRandom = nextRainRandom();
                            if (freeDrop != m_rainDrops.end()) {
                                const bool heavyDrop = strengthRandom > 0.84f;
                                freeDrop->frame = 0;
                                freeDrop->totalFrames = static_cast<uint32_t>(
                                    SampleRate * (heavyDrop
                                        ? 0.034f + durationRandom * 0.018f
                                        : 0.018f + durationRandom * 0.020f));
                                freeDrop->strength =
                                    (0.135f + strengthRandom * 0.125f) *
                                    std::pow(m_rainLevel, 0.68f) *
                                    (heavyDrop ? 1.22f : 1.0f);
                                freeDrop->pan = panRandom * 1.50f - 0.75f;
                                freeDrop->phase = 0.0;
                                freeDrop->frequency =
                                    168.0 + pitchRandom * 146.0;
                            }
                            const float density = std::clamp(
                                m_rainLevel, 0.0f, 1.0f);
                            const float interval = clusterRandom < 0.18f
                                ? 0.030f + nextRainRandom() * 0.038f
                                : (0.105f - density * 0.044f) +
                                    nextRainRandom() * (0.145f - density * 0.060f);
                            m_rainDropCountdown = std::max<uint32_t>(
                                1u, static_cast<uint32_t>(SampleRate * interval));
                        }
                    }
                    for (auto& drop : m_rainDrops) {
                        if (drop.frame >= drop.totalFrames ||
                            drop.totalFrames == 0) continue;
                        const float t = static_cast<float>(drop.frame) /
                            static_cast<float>(drop.totalFrames);
                        const float attack = std::min(t / 0.065f, 1.0f);
                        const float decay = std::pow(1.0f - t, 2.40f);
                        const float envelope = attack * decay;
                        const float wave = static_cast<float>(
                            0.72 * std::sin(drop.phase) +
                            0.28 * std::sin(drop.phase * 1.91 + 0.24));
                        const float amplitude = wave * drop.strength * envelope;
                        const float leftGain = std::sqrt(
                            std::clamp((1.0f - drop.pan) * 0.5f, 0.0f, 1.0f));
                        const float rightGain = std::sqrt(
                            std::clamp((1.0f + drop.pan) * 0.5f, 0.0f, 1.0f));
                        rainLeft += amplitude * leftGain;
                        rainRight += amplitude * rightGain;
                        drop.phase += TwoPi * drop.frequency *
                            (1.0 - 0.26 * static_cast<double>(t)) / SampleRate;
                        if (drop.phase >= TwoPi) drop.phase -= TwoPi;
                        ++drop.frame;
                    }
                    const float windGust = std::clamp(static_cast<float>(
                        0.68 + 0.22 * std::sin(m_windModPhase) +
                        0.10 * std::sin(m_windSlowModPhase)), 0.32f, 1.0f);
                    const float windWave = static_cast<float>(
                        0.76 * std::sin(m_windPhase) +
                        0.24 * std::sin(m_windHarmonicPhase));
                    const float windAmplitude =
                        std::pow(m_windLevel, 1.12f) * 0.090f * windGust;

                    // Map zoom is a smooth, self-expiring tactile strip.  It
                    // avoids repeated motor clicks while the stick/trigger is
                    // held, and uses a slightly higher texture for zooming in
                    // than for zooming out so direction remains perceptible.
                    if (m_mapZoomHoldFrames > 0) --m_mapZoomHoldFrames;
                    const float mapZoomGoal = m_mapZoomHoldFrames > 0
                        ? m_mapZoomTarget
                        : 0.0f;
                    const float mapZoomSmoothing =
                        mapZoomGoal > m_mapZoomLevel ? 0.0024f : 0.00075f;
                    m_mapZoomLevel +=
                        (mapZoomGoal - m_mapZoomLevel) * mapZoomSmoothing;
                    if (mapZoomGoal == 0.0f && m_mapZoomLevel < 0.0001f) {
                        m_mapZoomLevel = 0.0f;
                    }
                    const float mapZoomWave = static_cast<float>(
                        0.76 * std::sin(m_mapZoomPhase) +
                        0.24 * std::sin(m_mapZoomHarmonicPhase));
                    const float mapZoomRipple = static_cast<float>(
                        0.91 + 0.09 * std::sin(m_mapZoomModPhase));
                    const float mapZoomAmplitude =
                        std::pow(m_mapZoomLevel, 0.80f) * 0.165f * mapZoomRipple;

                    // Long-press feedback follows the game's own hold-circle
                    // progress. A low contact texture grows in pitch and
                    // strength, while start/cancel/complete use separate
                    // envelopes so releasing early cannot feel successful.
                    if (m_longPressHoldFrames > 0) {
                        --m_longPressHoldFrames;
                    } else if (m_longPressActive) {
                        m_longPressActive = false;
                        m_longPressTarget = 0.0f;
                    }
                    const float longPressGoal = m_longPressActive
                        ? m_longPressTarget
                        : 0.0f;
                    const float longPressSmoothing =
                        longPressGoal > m_longPressLevel ? 0.00165f : 0.00072f;
                    m_longPressLevel +=
                        (longPressGoal - m_longPressLevel) * longPressSmoothing;
                    if (longPressGoal == 0.0f && m_longPressLevel < 0.0001f) {
                        m_longPressLevel = 0.0f;
                    }
                    const float longPressWave = static_cast<float>(
                        0.78 * std::sin(m_longPressPhase) +
                        0.22 * std::sin(m_longPressHarmonicPhase));
                    const float longPressRipple = static_cast<float>(
                        0.92 + 0.08 * std::sin(m_longPressModPhase));
                    const float longPressAmplitude = longPressRipple *
                        (0.046f * std::sqrt(m_longPressLevel) +
                         0.258f * std::pow(m_longPressLevel, 1.35f));
                    const float longPressProfileGain =
                        m_longPressEquipmentProfile ? 1.85f : 1.0f;
                    const float longPressAccentGain =
                        m_longPressEquipmentProfile ? 1.24f : 1.0f;

                    float longPressAccentAmplitude = 0.0f;
                    double longPressAccentFrequency = 176.0;
                    if (m_longPressStartFrames > 0) {
                        const float t = 1.0f -
                            static_cast<float>(m_longPressStartFrames) / 2880.0f;
                        const float attack = std::clamp(t / 0.12f, 0.0f, 1.0f);
                        longPressAccentAmplitude =
                            0.235f * attack * std::exp(-5.1f * t);
                        longPressAccentFrequency = 224.0 - 54.0 * t;
                        --m_longPressStartFrames;
                    } else if (m_longPressCancelFrames > 0) {
                        const float t = 1.0f -
                            static_cast<float>(m_longPressCancelFrames) / 2880.0f;
                        const float attack = std::clamp(t / 0.10f, 0.0f, 1.0f);
                        longPressAccentAmplitude =
                            0.175f * attack * std::exp(-4.2f * t);
                        longPressAccentFrequency = 150.0 - 74.0 * t;
                        --m_longPressCancelFrames;
                    } else if (m_longPressCompleteFrames > 0) {
                        const float t = 1.0f -
                            static_cast<float>(m_longPressCompleteFrames) / 7200.0f;
                        const float firstLock = std::exp(
                            -430.0f * (t - 0.075f) * (t - 0.075f));
                        const float secondLock = std::exp(
                            -260.0f * (t - 0.50f) * (t - 0.50f));
                        const float body = std::clamp(t / 0.055f, 0.0f, 1.0f) *
                            std::exp(-4.8f * t);
                        longPressAccentAmplitude =
                            0.455f * firstLock + 0.625f * secondLock +
                            0.095f * body;
                        longPressAccentFrequency = t < 0.34f ? 118.0 : 184.0;
                        --m_longPressCompleteFrames;
                    }
                    const float longPressAccentWave = static_cast<float>(
                        0.80 * std::sin(m_longPressAccentPhase) +
                        0.20 * std::sin(m_longPressAccentPhase * 2.0));

                    float oniStrengthLeft = 0.0f;
                    float oniStrengthRight = 0.0f;
                    double oniStrengthFrequency = 92.0;
                    if (m_oniStrengthFrames > 0) {
                        const float totalFrames = m_oniStrengthProfile == 1
                            ? 24960.0f
                            : static_cast<float>(
                                oni_strength_haptics::DestroyDurationFrames);
                        const float t = 1.0f -
                            static_cast<float>(m_oniStrengthFrames) /
                                totalFrames;
                        if (m_oniStrengthProfile == 1) {
                            const float wave = static_cast<float>(
                                0.76 * std::sin(m_oniStrengthPhase) +
                                0.24 * std::sin(m_oniStrengthHarmonicPhase));
                            const float strainAttack = std::clamp(
                                t / 0.075f, 0.0f, 1.0f);
                            const float strain = strainAttack *
                                std::exp(-2.8f * t);
                            const float crack = std::exp(
                                -420.0f * (t - 0.34f) * (t - 0.34f));
                            const float rupture = std::exp(
                                -620.0f * (t - 0.575f) * (t - 0.575f));
                            const float debris = std::exp(
                                -82.0f * (t - 0.76f) * (t - 0.76f));
                            oniStrengthLeft = wave * (
                                0.240f * strain + 0.485f * crack +
                                0.900f * rupture + 0.270f * debris);
                            oniStrengthRight = wave * (
                                0.195f * strain + 0.550f * crack +
                                0.810f * rupture + 0.315f * debris);
                            oniStrengthFrequency = t < 0.56f
                                ? 72.0 + 88.0 * (t / 0.56f)
                                : 126.0 - 52.0 * std::min(
                                    (t - 0.56f) / 0.44f, 1.0f);
                        } else {
                            const uint32_t hapticFrame =
                                oni_strength_haptics::DestroyDurationFrames -
                                m_oniStrengthFrames;
                            const auto sample =
                                oni_strength_haptics::SampleDestroy(
                                    hapticFrame);
                            oniStrengthLeft = sample.left;
                            oniStrengthRight = sample.right;
                        }
                        --m_oniStrengthFrames;
                    }

                    float oniEyeLeft = 0.0f;
                    float oniEyeRight = 0.0f;
                    double oniEyeFrequency = 72.0;
                    if (m_oniEyeFrames > 0) {
                        constexpr float totalFrames = 39360.0f;
                        const float t = 1.0f -
                            static_cast<float>(m_oniEyeFrames) / totalFrames;
                        const auto pulse = [t](float center, float sharpness) {
                            const float delta = t - center;
                            return std::exp(-sharpness * delta * delta);
                        };
                        const float attack = std::clamp(
                            t / 0.035f, 0.0f, 1.0f);
                        const float ignition = attack * std::exp(-10.5f * t);
                        const float innerLeft = pulse(0.12f, 260.0f);
                        const float middleLeft = pulse(0.34f, 190.0f);
                        const float outerLeft = pulse(0.56f, 150.0f);
                        const float innerRight = pulse(0.14f, 260.0f);
                        const float middleRight = pulse(0.365f, 190.0f);
                        const float outerRight = pulse(0.585f, 150.0f);
                        const float tail = t >= 0.56f
                            ? std::exp(-10.5f * (t - 0.56f))
                            : 0.0f;
                        const float ripple = static_cast<float>(
                            0.90 + 0.10 * std::sin(m_oniEyeSweepPhase));
                        const float leftAmplitude = ripple * (
                            0.090f * ignition + 0.240f * innerLeft +
                            0.380f * middleLeft + 0.540f * outerLeft +
                            0.100f * tail);
                        const float rightAmplitude = ripple * (
                            0.090f * ignition + 0.220f * innerRight +
                            0.400f * middleRight + 0.580f * outerRight +
                            0.110f * tail);
                        const float leftWave = static_cast<float>(
                            0.76 * std::sin(m_oniEyePhase) +
                            0.24 * std::sin(m_oniEyeHarmonicPhase));
                        const float rightWave = static_cast<float>(
                            0.76 * std::sin(m_oniEyePhase + 0.18) +
                            0.24 * std::sin(m_oniEyeHarmonicPhase + 0.31));
                        oniEyeLeft = leftWave * leftAmplitude;
                        oniEyeRight = rightWave * rightAmplitude;
                        oniEyeFrequency = t < 0.62f
                            ? 72.0 + 112.0 * (t / 0.62f)
                            : 184.0 - 92.0 * std::min(
                                (t - 0.62f) / 0.38f, 1.0f);
                        --m_oniEyeFrames;
                    }

                    left += lowWave * lowAmplitude * 0.88f +
                        highWave * highAmplitude * 0.66f +
                        triggerWave * leftTriggerAmplitude;
                    right += lowWave * lowAmplitude * 0.76f +
                        highWave * highAmplitude * 0.88f +
                        triggerWave * rightTriggerAmplitude;

                    // Add a short high-frequency leading edge to discrete PC
                    // motor requests. This makes sword hits, damage, UI/lever
                    // clicks and other short presets tactile without inflating
                    // their sustained body strength.
                    left += impactWave * (
                        impactAccent[0] * 0.34f +
                        impactAccent[1] * 0.50f +
                        impactAccent[2] * 0.55f +
                        impactAccent[3] * 0.18f);
                    right += impactWave * (
                        impactAccent[0] * 0.28f +
                        impactAccent[1] * 0.56f +
                        impactAccent[2] * 0.18f +
                        impactAccent[3] * 0.55f);
                    left += rainLeft + windWave * windAmplitude * 0.86f;
                    right += rainRight + windWave * windAmplitude;
                    const bool zoomingIn = m_mapZoomDirection > 0.0f;
                    left += mapZoomWave * mapZoomAmplitude *
                        (zoomingIn ? 0.86f : 1.0f);
                    right += mapZoomWave * mapZoomAmplitude *
                        (zoomingIn ? 1.0f : 0.86f);
                    left += longPressWave * longPressAmplitude *
                            longPressProfileGain * 0.96f +
                        longPressAccentWave * longPressAccentAmplitude *
                            longPressAccentGain;
                    right += longPressWave * longPressAmplitude *
                            longPressProfileGain +
                        longPressAccentWave * longPressAccentAmplitude *
                            longPressAccentGain;
                    left += oniStrengthLeft;
                    right += oniStrengthRight;
                    left += oniEyeLeft;
                    right += oniEyeRight;

                    auto advance = [](double& phase, double frequency) {
                        phase += TwoPi * frequency / SampleRate;
                        if (phase >= TwoPi) phase -= TwoPi;
                    };
                    advance(m_lowPhase, 82.0);
                    advance(m_lowHarmonicPhase, 131.0);
                    advance(m_highPhase, 168.0);
                    advance(m_highHarmonicPhase, 233.0);
                    advance(m_triggerPhase, 126.0);
                    advance(m_triggerHarmonicPhase, 206.0);
                    advance(m_impactPhase, 286.0);
                    advance(m_impactHarmonicPhase, 421.0);
                    advance(m_windPhase, 71.0);
                    advance(m_windHarmonicPhase, 109.0);
                    advance(m_windModPhase, 0.29);
                    advance(m_windSlowModPhase, 0.083);
                    advance(m_mapZoomPhase, zoomingIn ? 142.0 : 94.0);
                    advance(m_mapZoomHarmonicPhase, zoomingIn ? 224.0 : 151.0);
                    advance(m_mapZoomModPhase, 7.5);
                    advance(
                        m_longPressPhase,
                        92.0 + 94.0 * m_longPressProgress);
                    advance(
                        m_longPressHarmonicPhase,
                        151.0 + 119.0 * m_longPressProgress);
                    advance(
                        m_longPressModPhase,
                        6.5 + 5.5 * m_longPressProgress);
                    advance(m_longPressAccentPhase, longPressAccentFrequency);
                    advance(m_oniStrengthPhase, oniStrengthFrequency);
                    advance(m_oniStrengthHarmonicPhase,
                        oniStrengthFrequency * 1.73);
                    advance(m_oniEyePhase, oniEyeFrequency);
                    advance(m_oniEyeHarmonicPhase, oniEyeFrequency * 1.61);
                    advance(m_oniEyeSweepPhase, 5.4);
                    advance(m_soulSpeakerPhaseA, 1187.0);
                    advance(m_soulSpeakerPhaseB, 1811.0);
                    advance(m_soulSpeakerPhaseC, 2671.0);
                    advance(m_soulSpeakerModPhase, 1.35);

                    // Keep soul audio and gauntlet dialogue on their already
                    // tested limiter. Combat originals use a separate bus:
                    // their extracted RMS is much lower, so a soft-knee boost
                    // makes impacts audible without hard-clipping transients or
                    // changing the established voice/soul volume.
                    const float standardSpeakerOutput =
                        std::clamp(speaker, -0.42f, 0.42f);
                    const float combatSpeakerOutput = 0.82f * std::tanh(
                        combatSpeaker * 2.25f / 0.82f);
                    // Speaker routing is mono; equal front-channel levels keep
                    // it audible on firmware variants that select either side.
                    const float speakerOutput = std::clamp(
                        standardSpeakerOutput + combatSpeakerOutput,
                        -0.90f,
                        0.90f);
                    output[frame * 4] = speakerOutput;
                    output[frame * 4 + 1] = speakerOutput;
                    output[frame * 4 + 2] = std::clamp(
                        left * m_hapticIntensity, -1.0f, 1.0f);
                    output[frame * 4 + 3] = std::clamp(
                        right * m_hapticIntensity, -1.0f, 1.0f);
                }
                m_titleVoices.erase(std::remove_if(m_titleVoices.begin(), m_titleVoices.end(),
                    [](const TitleVoice& v) { return v.release == 0 || v.frame >= title_haptics::Duration(v.cue) * 48000.0f; }), m_titleVoices.end());
                m_voices.erase(std::remove_if(m_voices.begin(), m_voices.end(),
                    [this](const Voice& voice) {
                        return voice.frame >= m_clips[voice.clipIndex].Frames();
                    }), m_voices.end());
                m_footsteps.erase(std::remove_if(
                    m_footsteps.begin(),
                    m_footsteps.end(),
                    [](const FootstepVoice& footstep) {
                        return footstep.frame >= footstep.totalFrames;
                    }), m_footsteps.end());
                m_playerReactions.erase(std::remove_if(
                    m_playerReactions.begin(),
                    m_playerReactions.end(),
                    [](const PlayerReactionVoice& reaction) {
                        return reaction.frame >= reaction.totalFrames;
                    }), m_playerReactions.end());
                uint32_t completedCombatHapticMask = 0;
                for (const CombatHapticVoice& voice : m_combatHaptics) {
                    if (voice.frame >= voice.totalFrames) {
                        completedCombatHapticMask |= 1u << static_cast<unsigned>(voice.profile);
                    }
                }
                m_combatHaptics.erase(std::remove_if(
                    m_combatHaptics.begin(),
                    m_combatHaptics.end(),
                    [](const CombatHapticVoice& voice) {
                        return voice.frame >= voice.totalFrames;
                    }), m_combatHaptics.end());
                // Keep completion snapshots across the unlocked transport call.
                // A successfully queued/synthesized voice is not yet delivered.
                std::array<EnvironmentInteractionVoice, 12> completedEnvironment{};
                size_t completedEnvironmentCount = 0;
                for (const auto& voice : m_environmentInteractions) {
                    if (voice.frame >= voice.totalFrames)
                        completedEnvironment[completedEnvironmentCount++] = voice;
                }
                m_environmentInteractions.erase(std::remove_if(
                    m_environmentInteractions.begin(),
                    m_environmentInteractions.end(),
                    [](const EnvironmentInteractionVoice& interaction) {
                        return interaction.frame >= interaction.totalFrames;
                    }), m_environmentInteractions.end());
                m_capturedPresetVoices.erase(std::remove_if(
                    m_capturedPresetVoices.begin(),
                    m_capturedPresetVoices.end(),
                    [](const CapturedPresetVoice& voice) {
                        return voice.frame >= voice.totalFrames;
                    }), m_capturedPresetVoices.end());
                m_speakerCues.erase(std::remove_if(
                    m_speakerCues.begin(),
                    m_speakerCues.end(),
                    [](const SpeakerCueVoice& cue) {
                        return cue.frame >= cue.totalFrames;
                    }), m_speakerCues.end());
                m_speakerFileVoices.erase(std::remove_if(
                    m_speakerFileVoices.begin(),
                    m_speakerFileVoices.end(),
                    [this](const SpeakerFileVoice& voice) {
                        return voice.clipIndex >= m_speakerClips.size() ||
                            voice.frame >= m_speakerClips[voice.clipIndex].Frames();
                    }), m_speakerFileVoices.end());
                voiceLock.unlock();
                HRESULT releaseResult = S_OK;
#ifdef ONIMUSHA_BLUETOOTH_ENABLED
                if (bluetooth) {
                    const auto speaker = dualsense_bluetooth::Speaker(output);
                    const auto pcm = dualsense_bluetooth::Haptics(output);
                    std::array<uint8_t, dualsense_bluetooth::OpusBytes> opus{};
                    const int encoded = opus_encode_float(encoder, speaker.data(),
                        static_cast<int>(dualsense_bluetooth::OpusFrames), opus.data(),
                        static_cast<opus_int32>(opus.size()));
                    if (encoded != static_cast<int>(opus.size())) {
                        LogFormat(L"Bluetooth Opus encode failed size=%d", encoded);
                        break;
                    }
                    {
                        const ULONGLONG writeStart = GetTickCount64();
                        std::lock_guard writeLock{g_controllerWriteMutex};
                        const auto packet = dualsense_bluetooth::Stream(opus, pcm,
                            g_bluetoothSequence++, streamCounter++);
                        if (!WriteControllerPacket(streamController, packet.data(), packet.size()))
                            releaseResult = E_FAIL;
                        maximumWriteMs = std::max(maximumWriteMs, GetTickCount64() - writeStart);
                    }
                    if (SUCCEEDED(releaseResult)) {
                        ++streamPackets;
                        activeHapticPackets += std::any_of(pcm.begin(), pcm.end(), [](uint8_t value) { return value != 0; }) ? 1u : 0u;
                        activeSpeakerPackets += std::any_of(speaker.begin(), speaker.end(), [](float value) { return std::fabs(value) > 0.0001f; }) ? 1u : 0u;
                        if (GetTickCount64() >= nextStreamDiagnostic) {
                            LogFormat(L"Bluetooth stream accepted packets=%u hapticActive=%u speakerActive=%u maxWriteMs=%llu (transport only)",
                                streamPackets, activeHapticPackets, activeSpeakerPackets, maximumWriteMs);
                            streamPackets = activeHapticPackets = activeSpeakerPackets = 0;
                            maximumWriteMs = 0;
                            nextStreamDiagnostic = GetTickCount64() + 5000;
                        }
                    }
                    const LONGLONG interval = clockFrequency.QuadPart * 512 / 48000;
                    nextDeadline.QuadPart += interval;
                    LARGE_INTEGER now{};
                    QueryPerformanceCounter(&now);
                    if (now.QuadPart - nextDeadline.QuadPart > interval * 2) {
                        nextDeadline.QuadPart = now.QuadPart + interval;
                        Log(L"Bluetooth stream scheduling overrun; backlog skipped");
                    }
                } else
#endif
                {
                    releaseResult = renderClient->ReleaseBuffer(available, 0);
                }
                if (FAILED(releaseResult)) {
                    LogFormat(
                        L"DualSense haptics buffer release failed hr=0x%08X",
                        releaseResult);
                    break;
                }
                voiceLock.lock();
                m_submittedEnvironmentHapticMask.fetch_or(environmentSubmitted, std::memory_order_release);
                for (size_t index = 0; index < completedEnvironmentCount; ++index) {
                    const auto& voice = completedEnvironment[index];
                    m_completedEnvironmentHapticMask.fetch_or(
                        1u << static_cast<unsigned>(voice.profile), std::memory_order_release);
                    if (IsTreasureChestProfile(voice.profile))
                        QueueEnvironmentDiagnosticLocked(voice, L"completed-endpoint-accepted");
                }
                // Completion means the endpoint accepted the final haptic
                // buffer, not merely that synthesis advanced past its end.
                m_completedCombatHapticMask.fetch_or(
                    completedCombatHapticMask, std::memory_order_release);
                // Only report once each new swing/hit has nonzero samples and
                // WASAPI accepts the rear-channel buffer. Not a felt-strength test.
                if (m_hapticIntensity > 0.0f && normalAttackSubmitted != 0)
                    m_submittedNormalAttackMask.fetch_or(normalAttackSubmitted, std::memory_order_release);
                // Publish only after WASAPI accepts the front-channel buffer.
                // The pipe thread writes diagnostics so the audio thread never
                // performs file I/O for these short defense transients.
                for (SpeakerFileVoice& voice : m_combatSpeakerFileVoices) {
                    if ((voice.cue != ControllerSpeakerCue::CombatDeflect &&
                         voice.cue != ControllerSpeakerCue::CombatParryBlade &&
                         voice.cue != ControllerSpeakerCue::CombatParryProjectile) ||
                        voice.clipIndex >= m_combatSpeakerClips.size() ||
                        voice.frame == 0) continue;
                    const uint32_t bit = 1u << static_cast<unsigned>(voice.cue);
                    if (!voice.submitted) {
                        voice.submitted = true;
                        m_submittedCombatSpeakerMask.fetch_or(
                            bit, std::memory_order_release);
                    }
                    const bool deflect =
                        voice.cue == ControllerSpeakerCue::CombatDeflect;
                    const size_t playbackFrames =
                        combat_speaker_timing::PlaybackFrames(
                            deflect,
                            m_combatSpeakerClips[voice.clipIndex].Frames(),
                            voice.deflectVariant);
                    if (voice.frame >= playbackFrames) {
                        m_completedCombatSpeakerMask.fetch_or(
                            bit, std::memory_order_release);
                    }
                }
                m_combatSpeakerFileVoices.erase(std::remove_if(
                    m_combatSpeakerFileVoices.begin(),
                    m_combatSpeakerFileVoices.end(),
                    [this](const SpeakerFileVoice& voice) {
                        if (voice.clipIndex >=
                            m_combatSpeakerClips.size()) return true;
                        const bool deflect = voice.cue ==
                            ControllerSpeakerCue::CombatDeflect;
                        return voice.frame >=
                            combat_speaker_timing::PlaybackFrames(
                                deflect,
                                m_combatSpeakerClips[voice.clipIndex].Frames(),
                                voice.deflectVariant);
                    }), m_combatSpeakerFileVoices.end());
            }
        } while (false);

        if (!m_started.load()) SetEvent(m_readyEvent);
        if (audioStarted) audioClient->Stop();
#ifdef ONIMUSHA_BLUETOOTH_ENABLED
        if (bluetooth && streamPackets > 0)
            LogFormat(L"Bluetooth stream final accepted packets=%u hapticActive=%u speakerActive=%u maxWriteMs=%llu (transport only)",
                streamPackets, activeHapticPackets, activeSpeakerPackets, maximumWriteMs);
        if (encoder != nullptr) opus_encoder_destroy(encoder);
#endif
        if (streamTimer != nullptr) CloseHandle(streamTimer);
        CloseController(streamController);
        m_started.store(false);
        SafeRelease(renderClient);
        SafeRelease(audioClient);
        SafeRelease(device);
        if (SUCCEEDED(comResult)) CoUninitialize();
    }

    std::array<HapticClip, 5> m_clips;
    std::array<SpeakerClip, 4> m_speakerClips;
    std::array<SpeakerClip, 19> m_combatSpeakerClips;
    SpeakerClip m_gauntletVoiceClip;
    product_runtime::GauntletVoiceLanguage m_gauntletVoiceLanguage = product_runtime::GauntletVoiceLanguage::Chinese;
    struct TitleVoice { unsigned cue; unsigned frame; unsigned release; };
    std::vector<TitleVoice> m_titleVoices;
    std::vector<Voice> m_voices;
    std::vector<FootstepVoice> m_footsteps;
    std::vector<PlayerReactionVoice> m_playerReactions;
    std::vector<CombatHapticVoice> m_combatHaptics;
    std::vector<EnvironmentInteractionVoice> m_environmentInteractions;
    std::vector<EnvironmentRenderDiagnostic> m_environmentDiagnostics;
    uint32_t m_suppressedChestPresets{};
    std::vector<SpeakerCueVoice> m_speakerCues;
    std::vector<SpeakerFileVoice> m_speakerFileVoices;
    std::vector<SpeakerFileVoice> m_combatSpeakerFileVoices;
    std::vector<CapturedPresetVoice> m_capturedPresetVoices;
    live_motor_state::TargetLease m_liveMotorTargets;
    std::array<float, 4> m_motorPower{};
    std::array<std::vector<MotorPulse>, 4> m_motorPulses;
    double m_lowPhase = 0.0;
    double m_lowHarmonicPhase = 0.0;
    double m_highPhase = 0.0;
    double m_highHarmonicPhase = 0.0;
    double m_triggerPhase = 0.0;
    double m_triggerHarmonicPhase = 0.0;
    double m_impactPhase = 0.0;
    double m_impactHarmonicPhase = 0.0;
    double m_windPhase = 0.0;
    double m_windHarmonicPhase = 0.0;
    double m_windModPhase = 0.0;
    double m_windSlowModPhase = 0.0;
    double m_elevatorPhase = 0.0;
    double m_elevatorHarmonicPhase = 0.0;
    double m_elevatorModPhase = 0.0;
    double m_mapZoomPhase = 0.0;
    double m_mapZoomHarmonicPhase = 0.0;
    double m_mapZoomModPhase = 0.0;
    double m_longPressPhase = 0.0;
    double m_longPressHarmonicPhase = 0.0;
    double m_longPressModPhase = 0.0;
    double m_longPressAccentPhase = 0.0;
    double m_oniStrengthPhase = 0.0;
    double m_oniStrengthHarmonicPhase = 0.0;
    double m_oniEyePhase = 0.0;
    double m_oniEyeHarmonicPhase = 0.0;
    double m_oniEyeSweepPhase = 0.0;
    double m_soulHapticPhase = 0.0;
    double m_soulHapticHarmonicPhase = 0.0;
    double m_soulHapticModPhase = 0.0;
    double m_sprintPhase = 0.0;
    double m_sprintHarmonicPhase = 0.0;
    double m_sprintModPhase = 0.0;
    double m_soulSpeakerPhaseA = 0.0;
    double m_soulSpeakerPhaseB = 0.0;
    double m_soulSpeakerPhaseC = 0.0;
    double m_soulSpeakerModPhase = 0.0;
    float m_rainTarget = 0.0f;
    float m_windTarget = 0.0f;
    float m_rainLevel = 0.0f;
    float m_windLevel = 0.0f;
    std::array<RainDropVoice, 3> m_rainDrops{};
    uint32_t m_rainDropCountdown = 0;
    float m_elevatorTarget = 0.0f;
    float m_elevatorLevel = 0.0f;
    float m_elevatorNoiseLow = 0.0f;
    uint32_t m_elevatorHoldFrames = 0;
    uint32_t m_elevatorNoiseState = 0xD1B54A35u;
    float m_mapZoomTarget = 0.0f;
    float m_mapZoomLevel = 0.0f;
    float m_mapZoomDirection = 1.0f;
    uint32_t m_mapZoomHoldFrames = 0;
    float m_longPressTarget = 0.0f;
    float m_longPressLevel = 0.0f;
    float m_longPressProgress = 0.0f;
    bool m_longPressActive = false;
    bool m_longPressEquipmentProfile = false;
    uint32_t m_longPressHoldFrames = 0;
    uint32_t m_longPressStartFrames = 0;
    uint32_t m_longPressCompleteFrames = 0;
    uint32_t m_longPressCancelFrames = 0;
    uint32_t m_oniStrengthFrames = 0;
    uint8_t m_oniStrengthProfile = 0;
    uint32_t m_oniEyeFrames = 0;
    uint32_t m_oniTransformationFrames = 0;
    oni_charge_release_haptics::Kind m_oniChargeReleaseKind =
        oni_charge_release_haptics::Kind::Light;
    uint32_t m_oniChargeReleaseFrames = 0;
    uint32_t m_oniChargeReleaseDelayFrames = 0;
    uint32_t m_oniChargeReleaseFrame = 0;
    bow_shot_haptics::Kind m_bowShotKind = bow_shot_haptics::Kind::Quick;
    uint32_t m_bowShotFrames = 0;
    uint32_t m_bowShotFrame = 0;
    wall_run_haptics::Renderer m_wallRunRenderer{};
    ladder_haptics::Renderer m_ladderRenderer{};
    normal_attack_haptics::Renderer m_normalAttackRenderer{};
    float m_soulHapticTarget = 0.0f;
    float m_blackSoulTarget = 0.0f;
    float m_blackSoulLevel = 0.0f;
    uint32_t m_blackSoulHoldFrames = 0;
    uint32_t m_blackSoulFrame = 0;
    uint32_t m_blackSoulFinishFrames = 0;
    float m_blackSoulFinishIntensity = 0.0f;
    float m_soulHapticLevel = 0.0f;
    uint32_t m_soulHapticHoldFrames = 0;
    uint32_t m_soulHapticStartFrames = 0;
    uint32_t m_soulHapticAccentFrames = 0;
    float m_soulHapticNoiseLow = 0.0f;
    uint32_t m_soulHapticNoiseState = 0x6A09E667u;
    float m_soulSearchTarget = 0.0f;
    float m_soulSearchLevel = 0.0f;
    uint32_t m_soulSearchHoldFrames = 0;
    double m_soulSearchPhase = 0.0;
    double m_soulSearchHarmonicPhase = 0.0;
    double m_soulSearchModPhase = 0.0;
    float m_sprintTarget = 0.0f;
    float m_sprintLevel = 0.0f;
    uint32_t m_sprintHoldFrames = 0;
    uint32_t m_sprintStartFrames = 0;
    uint32_t m_sprintPressFrames = 0;
    float m_sprintPressIntensity = 0.0f;
    bool m_sprintPressIsSlowdown = false;
    double m_sprintPressPhase = 0.0;
    double m_sprintPressHarmonicPhase = 0.0;
    float m_sprintNoiseLow = 0.0f;
    uint32_t m_sprintNoiseState = 0xBB67AE85u;
    float m_soulSpeakerTarget = 0.0f;
    float m_soulSpeakerLevel = 0.0f;
    size_t m_soulLoopFrame = 0;
    size_t m_gauntletVoiceFrame = 0;
    bool m_speakerAudioLoaded = false;
    bool m_combatSpeakerAudioLoaded = false;
    bool m_slowDeflectSpeakerAudioLoaded = false;
    bool m_gauntletVoiceActive = false;
    float m_hapticIntensity = 1.0f;
    float m_gauntletVoiceVolume = 1.0f;
    float m_soulSpeakerVolume = 1.0f;
    float m_clashSpeakerVolume = 1.0f;
    float m_guardSpeakerVolume = 1.0f;
    float m_parrySpeakerVolume = 1.0f;
    float m_deflectSpeakerVolume = 1.0f;
    float m_executionSpeakerVolume = 1.0f;
    float m_issenSpeakerVolume = 1.0f;
    float m_speakerNoiseLow = 0.0f;
    uint32_t m_noiseState = 0x91E10DA5u;
    uint32_t m_speakerNoiseState = 0x243F6A88u;
    uint32_t m_combatSpeakerVariant = 0;
    std::mutex m_voiceMutex;
    std::wstring m_dataDirectory;
    std::atomic<bool> m_started{false};
    std::atomic<uint32_t> m_completedCombatHapticMask{0};
    std::atomic<uint32_t> m_completedEnvironmentHapticMask{0};
    std::atomic<uint32_t> m_submittedEnvironmentHapticMask{0};
    std::atomic<uint32_t> m_submittedNormalAttackMask{0};
    std::atomic<uint32_t> m_suppressedRepelMotorMessages{0};
    std::atomic<uint32_t> m_suppressedExecutionMotorMessages{0};
    std::atomic<uint32_t> m_submittedCombatSpeakerMask{0};
    std::atomic<uint32_t> m_completedCombatSpeakerMask{0};
    HANDLE m_stopEvent = nullptr;
    HANDLE m_readyEvent = nullptr;
    HANDLE m_bufferEvent = nullptr;
    std::thread m_thread;
    std::atomic<bool> m_bluetoothMode{false};
};

size_t ClipForTrigger(uint32_t triggerId) {
    switch (triggerId) {
    case 2988664619u: return 5;
    case 1889407330u: return 4;
    case 3915515893u: return 2;
    case 2872063316u: return 3;
    case 2030876089u: return 1;
    default: return 0;
    }
}

float GainMultiplierForClip(size_t clip) {
    // The short PC-bank impulses are authored well below the useful range of
    // the Windows DualSense audio endpoint (clip 5 is about -14 dB RMS).
    // Normalize each clip perceptually while keeping the final actuator stream
    // hard-limited to full scale in AudioHaptics::ThreadMain.
    switch (clip) {
    case 1: return 2.2f;
    case 2: return 1.1f;
    case 3: return 2.2f;
    case 4: return 2.0f;
    case 5: return 4.0f;
    default: return 1.0f;
    }
}
} // namespace

#ifdef ONIMUSHA_BLUETOOTH_ENABLED
uint32_t FindPreviewVoice(const std::wstring& dataDirectory, product_runtime::GauntletVoiceLanguage language) {
    const std::wstring directory = dataDirectory + L"\\gauntlet_voice\\" +
        (language == product_runtime::GauntletVoiceLanguage::Japanese ? L"ja-JP\\" : L"");
    WIN32_FIND_DATAW entry{};
    HANDLE search = FindFirstFileW((directory + L"*.wav").c_str(), &entry);
    if (search == INVALID_HANDLE_VALUE) return 0;
    uint32_t selected = 0;
    ULONGLONG smallest = ~ULONGLONG{0};
    do {
        if ((entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) continue;
        wchar_t* end = nullptr;
        const unsigned long long id = _wcstoui64(entry.cFileName, &end, 10);
        if (end == entry.cFileName || _wcsicmp(end, L".wav") != 0 || id == 0 || id > UINT32_MAX) continue;
        const ULONGLONG bytes = (static_cast<ULONGLONG>(entry.nFileSizeHigh) << 32) | entry.nFileSizeLow;
        if (bytes > 44 && bytes < smallest) { selected = static_cast<uint32_t>(id); smallest = bytes; }
    } while (FindNextFileW(search, &entry));
    FindClose(search);
    return selected;
}
#endif

void ReadTouchInputs(dualsense_touch_input::SharedState* shared,
    std::atomic<bool>& stop) {
    ControllerConnection inputController;
    dualsense_touch_input::Gesture gesture;
    uint32_t previous = 0;
    while (!stop.load(std::memory_order_relaxed)) {
        if (!inputController.IsOpen()) {
            if (!OpenController(inputController, true)) {
                gesture.Reset();
                InterlockedExchange64(&shared->packed,
                    static_cast<long long>(dualsense_touch_input::Pack(GetTickCount(), 0)));
                Sleep(250);
                continue;
            }
            LogFormat(L"Touch input connected transport=%s",
                inputController.bluetooth ? L"Bluetooth" : L"USB");
        }
        std::vector<uint8_t> input(inputController.inputReportLength, 0);
        OVERLAPPED operation{};
        operation.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (operation.hEvent == nullptr) break;
        DWORD read = 0;
        BOOL success = ReadFile(inputController.handle, input.data(),
            static_cast<DWORD>(input.size()), &read, &operation);
        DWORD error = success ? ERROR_SUCCESS : GetLastError();
        if (!success && error == ERROR_IO_PENDING) {
            if (WaitForSingleObject(operation.hEvent, 100) == WAIT_OBJECT_0) {
                success = GetOverlappedResult(inputController.handle,
                    &operation, &read, FALSE);
                error = success ? ERROR_SUCCESS : GetLastError();
            } else {
                CancelIoEx(inputController.handle, &operation);
                GetOverlappedResult(inputController.handle, &operation, &read, TRUE);
                error = ERROR_TIMEOUT;
            }
        }
        CloseHandle(operation.hEvent);
        if (!success) {
            if (error != ERROR_TIMEOUT) {
                gesture.Reset();
                CloseController(inputController);
                LogFormat(L"Touch input disconnected error=%u", error);
                Sleep(200);
            }
            InterlockedExchange64(&shared->packed,
                static_cast<long long>(dualsense_touch_input::Pack(GetTickCount(), 0)));
            previous = 0;
            continue;
        }
        const auto contacts = dualsense_touch_input::Parse(input.data(), read);
        const uint32_t flags = gesture.Update(contacts);
        InterlockedExchange64(&shared->packed,
            static_cast<long long>(dualsense_touch_input::Pack(GetTickCount(), flags)));
        if (flags != previous) {
            LogFormat(L"Touch gesture=%s", flags == dualsense_touch_input::Left
                ? L"left/L3" : flags == dualsense_touch_input::Right
                    ? L"right/R3" : L"released");
            previous = flags;
        }
    }
    InterlockedExchange64(&shared->packed,
        static_cast<long long>(dualsense_touch_input::Pack(GetTickCount(), 0)));
    CloseController(inputController);
}

int wmain(int argc, wchar_t** argv) {
#ifdef ONIMUSHA_BLUETOOTH_ENABLED
    if (argc == 2 && wcscmp(argv[1], L"--test-bluetooth-control") == 0) {
        ControllerConnection controller;
        if (!OpenController(controller)) return 10;
        if (!controller.bluetooth) { CloseController(controller); return 2; }
        std::array<uint8_t, MessageLength> report{};
        report[0] = 0x02;
        report[1] = 0x0C;
        report[RightTriggerOffset] = 0x05;
        report[LeftTriggerOffset] = 0x05;
        const ULONGLONG start = GetTickCount64();
        const bool sent = SendReport(controller, report);
        std::wprintf(L"Single Bluetooth 0x31 trigger-reset report: %s elapsed=%llu ms; no audio stream.\n",
            sent ? L"Windows write completed" : L"FAILED", GetTickCount64() - start);
        CloseController(controller);
        return sent ? 0 : 6;
    }
    if (argc == 3 && wcscmp(argv[1], L"--watch-controller") == 0) {
        const int seconds = _wtoi(argv[2]);
        if (seconds < 1 || seconds > 300) return 2;
        std::wprintf(L"READ-ONLY controller diagnostic for %d seconds; no output reports.\n", seconds);
        fflush(stdout);
        ControllerConnection controller;
        const ULONGLONG start = GetTickCount64();
        ULONGLONG nextSummary = start;
        unsigned packets = 0, simple = 0, enhanced = 0, errors = 0;
        while (GetTickCount64() - start < static_cast<ULONGLONG>(seconds) * 1000) {
            if (!controller.IsOpen()) {
                if (!OpenController(controller, true)) { Sleep(250); continue; }
                std::wprintf(L"t=%llu connected transport=%s input=%u output=%u\n",
                    GetTickCount64() - start, controller.bluetooth ? L"Bluetooth" : L"USB",
                    controller.inputReportLength, controller.outputReportLength);
                fflush(stdout);
            }
            std::vector<uint8_t> input(controller.inputReportLength, 0);
            OVERLAPPED operation{};
            operation.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (operation.hEvent == nullptr) { CloseController(controller); break; }
            DWORD read = 0;
            BOOL success = ReadFile(controller.handle, input.data(), static_cast<DWORD>(input.size()), &read, &operation);
            DWORD error = success ? ERROR_SUCCESS : GetLastError();
            if (!success && error == ERROR_IO_PENDING) {
                if (WaitForSingleObject(operation.hEvent, 250) == WAIT_OBJECT_0) {
                    success = GetOverlappedResult(controller.handle, &operation, &read, FALSE);
                    error = success ? ERROR_SUCCESS : GetLastError();
                } else {
                    CancelIoEx(controller.handle, &operation);
                    GetOverlappedResult(controller.handle, &operation, &read, TRUE);
                    error = ERROR_TIMEOUT;
                }
            }
            CloseHandle(operation.hEvent);
            if (success && read > 0) {
                ++packets;
                if (input[0] == 0x01) ++simple;
                if (input[0] == 0x31) ++enhanced;
            } else if (error != ERROR_TIMEOUT) {
                ++errors;
                std::wprintf(L"t=%llu input failed error=%u; reopening\n", GetTickCount64() - start, error);
                CloseController(controller);
                Sleep(250);
            }
            if (GetTickCount64() >= nextSummary) {
                std::wprintf(L"t=%llu packets=%u simple=%u enhanced=%u errors=%u\n",
                    GetTickCount64() - start, packets, simple, enhanced, errors);
                fflush(stdout);
                nextSummary = GetTickCount64() + 1000;
            }
        }
        CloseController(controller);
        std::wprintf(L"Finished: packets=%u simple=%u enhanced=%u errors=%u\n", packets, simple, enhanced, errors);
        return packets > 0 ? 0 : 10;
    }
    if (argc == 2 && wcscmp(argv[1], L"--probe-controller") == 0) {
        ControllerConnection controller;
        if (!OpenController(controller)) {
            std::wprintf(L"No compatible connected DualSense HID collection.\n");
            return 10;
        }
        std::wprintf(L"%s transport=%s input=%u output=%u\n",
            ControllerModelName(controller.productId), controller.bluetooth ? L"Bluetooth" : L"USB",
            controller.inputReportLength, controller.outputReportLength);
        CloseController(controller);
        return 0;
    }
#endif
    const std::wstring hapticsDirectory = L"reframework\\data\\codex_dualsense_haptics";

    if (argc == 3 && argv[1] != nullptr && wcscmp(argv[1], L"--test-haptic") == 0) {
        const int clip = _wtoi(argv[2]);
        AudioHaptics audio;
        if (clip < 1 || clip > 5 || !audio.Start(hapticsDirectory)) return 5;
        audio.Play(static_cast<size_t>(clip), 1.0f, 1.0f);
        Sleep(1800);
        return 0;
    }

    if (argc == 2 && argv[1] != nullptr &&
        wcscmp(argv[1], L"--test-environment-haptics") == 0) {
        ControllerConnection controller;
        if (!ConfigureControllerSpeaker(controller, true)) return 6;
        AudioHaptics audio;
        if (!audio.Start(hapticsDirectory)) {
            CloseController(controller);
            return 5;
        }
        Log(L"Environment comparison: known title-confirm cue then chest 6/7/8; endpoint acceptance is not tactile confirmation");
        std::wprintf(L"Reference: title confirm, then chest open / push / lid.\n");
        audio.PlayTitle(4);
        Sleep(1800);
        constexpr EnvironmentInteractionProfile Profiles[] = {
            EnvironmentInteractionProfile::TreasureChestOpen,
            EnvironmentInteractionProfile::TreasureChestPush,
            EnvironmentInteractionProfile::TreasureChestLidLand,
        };
        bool passed = true;
        for (const auto profile : Profiles) {
            LogFormat(L"Isolated environment haptic profile=%u",
                static_cast<unsigned>(profile));
            audio.PlayEnvironmentInteraction(profile, 1.0f);
            if (profile == EnvironmentInteractionProfile::TreasureChestOpen) {
                Sleep(140);
                audio.PlayCapturedPreset(captured_haptic_presets::Preset::GenericInteract);
            } else if (profile == EnvironmentInteractionProfile::TreasureChestLidLand) {
                audio.PlayCapturedPreset(captured_haptic_presets::Preset::GetItem);
            }
            const DWORD profileWaitMs = profile ==
                    EnvironmentInteractionProfile::TreasureChestPush
                ? static_cast<DWORD>(std::ceil(
                    treasure_chest_haptics::DurationSeconds(
                        treasure_chest_haptics::Stage::Push) * 1000.0f)) + 500u
                : 1800u;
            Sleep(profileWaitMs);
            audio.FlushEnvironmentDiagnostics();
            const uint32_t bit = 1u << static_cast<unsigned>(profile);
            const bool submitted = (audio.ConsumeSubmittedEnvironmentHaptics() & bit) != 0;
            const bool completed = (audio.ConsumeCompletedEnvironmentHaptics() & bit) != 0;
            const uint32_t suppressed = audio.ConsumeSuppressedChestPresets();
            const uint32_t expectedSuppressed = profile == EnvironmentInteractionProfile::TreasureChestPush ? 0u : 1u;
            passed = passed && submitted && completed && suppressed == expectedSuppressed;
            LogFormat(L"Isolated chest result profile=%u submitted=%d completed=%d suppressedPresets=%u",
                static_cast<unsigned>(profile), submitted ? 1 : 0, completed ? 1 : 0, suppressed);
            std::wprintf(L"profile=%u submitted=%d completed=%d suppressedPresets=%u\n",
                static_cast<unsigned>(profile), submitted ? 1 : 0, completed ? 1 : 0, suppressed);
        }
        audio.StopAll();
        audio.Stop();
        CloseController(controller);
        return passed ? 0 : 7;
    }

    if (argc == 2 && argv[1] != nullptr && wcscmp(argv[1], L"--test-speaker") == 0) {
        ControllerConnection controller;
        if (!ConfigureControllerSpeaker(controller, true)) return 6;
        AudioHaptics audio;
        if (!audio.Start(hapticsDirectory)) {
            ConfigureControllerSpeaker(controller, false);
            CloseController(controller);
            return 5;
        }
        audio.PlaySpeakerCue(ControllerSpeakerCue::SoulStart);
        Sleep(650);
        audio.PlaySpeakerCue(ControllerSpeakerCue::SoulCollect);
        Sleep(720);
        audio.PlaySpeakerCue(ControllerSpeakerCue::SoulStop);
        Sleep(500);
        audio.Stop();
        ConfigureControllerSpeaker(controller, false);
        CloseController(controller);
        return 0;
    }

    if (argc == 2 && argv[1] != nullptr &&
        wcscmp(argv[1], L"--test-combat-speaker") == 0) {
        ControllerConnection controller;
        if (!ConfigureControllerSpeaker(controller, true)) return 6;
        AudioHaptics audio;
        if (!audio.Start(hapticsDirectory)) {
            ConfigureControllerSpeaker(controller, false);
            CloseController(controller);
            return 5;
        }
        constexpr ControllerSpeakerCue Cues[] = {
            ControllerSpeakerCue::CombatClash,
            ControllerSpeakerCue::CombatClashHit,
            ControllerSpeakerCue::CombatClashSuccess,
            ControllerSpeakerCue::CombatGuard,
            ControllerSpeakerCue::CombatGuardDirectional,
            ControllerSpeakerCue::CombatDeflect,
            ControllerSpeakerCue::CombatParryBlade,
            ControllerSpeakerCue::CombatParryProjectile,
            ControllerSpeakerCue::CombatExecution,
            ControllerSpeakerCue::CombatIssen
        };
        for (ControllerSpeakerCue cue : Cues) {
            audio.PlaySpeakerCue(cue);
            Sleep(700);
        }
        audio.Stop();
        ConfigureControllerSpeaker(controller, false);
        CloseController(controller);
        return 0;
    }

    if (argc == 2 && argv[1] != nullptr && wcscmp(argv[1], L"--test-repel-haptic") == 0) {
        ControllerConnection controller;
        if (!ConfigureControllerSpeaker(controller, true)) return 6;
        AudioHaptics audio;
        if (!audio.Start(hapticsDirectory)) {
            ConfigureControllerSpeaker(controller, false);
            CloseController(controller);
            return 5;
        }
        bool passed = true;
        for (CombatHapticProfile profile : {CombatHapticProfile::Parry, CombatHapticProfile::ProjectileParry}) {
            LogFormat(L"Isolated single-impact Repel test profile=%u", static_cast<unsigned>(profile));
            audio.PlayCombatHaptic(profile, 2.0f);
            // Mirror the exact PC traffic observed after a successful Repel.
            // None of it may overwrite the authored impact window.
            audio.PlayMotorPulse(0, 0.4f, 0.12f, false, 0);
            audio.PlayMotorPulse(1, 0.4f, 0.12f, false, 0);
            audio.SetMotorPower(0, 0.4f);
            audio.SetMotorPower(1, 0.4f);
            Sleep(500);
            const bool completed = (audio.ConsumeCompletedCombatHaptics() &
                (1u << static_cast<unsigned>(profile))) != 0;
            const uint32_t suppressed = audio.ConsumeSuppressedRepelMotorMessages();
            passed &= completed && suppressed == 4;
            LogFormat(L"Isolated single-impact Repel profile=%u completed=%d suppressedMotors=%u",
                static_cast<unsigned>(profile), completed ? 1 : 0, suppressed);
        }
        audio.Stop();
        ConfigureControllerSpeaker(controller, false);
        CloseController(controller);
        return passed ? 0 : 7;
    }

    if (argc == 2 && argv[1] != nullptr &&
        wcscmp(argv[1], L"--test-execution-haptic") == 0) {
        ControllerConnection controller;
        if (!ConfigureControllerSpeaker(controller, true)) return 6;
        AudioHaptics audio;
        audio.SetHapticIntensity(1.0f);
        if (!audio.Start(hapticsDirectory)) {
            ConfigureControllerSpeaker(controller, false);
            CloseController(controller);
            return 5;
        }
        bool passed = true;
        constexpr CombatHapticProfile Profiles[] = {
            CombatHapticProfile::ExecutionThrust,
            CombatHapticProfile::ExecutionSlash,
            CombatHapticProfile::ExecutionArmCut,
            CombatHapticProfile::ExecutionDowned,
            CombatHapticProfile::ExecutionConst,
            CombatHapticProfile::ExecutionBreakIssen,
            CombatHapticProfile::ExecutionBlockIssen};
        uint8_t motionVariant = 0u;
        for (CombatHapticProfile profile : Profiles) {
            audio.PlayCombatHaptic(profile, 1.65f, motionVariant++);
            audio.StopAll(true, false, true); // ordinary game cleanup preserves it
            audio.PlayMotorPulse(0, 0.4f, 0.12f, false, 0);
            audio.PlayMotorPulse(1, 0.4f, 0.12f, false, 0);
            audio.SetMotorPower(0, 0.4f);
            audio.SetMotorPower(1, 0.4f);
            const auto kind = execution_haptics::KindFromProfile(
                static_cast<uint8_t>(profile));
            Sleep(execution_haptics::SuppressionMs(kind) + 80u);
            const bool completed = (audio.ConsumeCompletedCombatHaptics() &
                (1u << static_cast<unsigned>(profile))) != 0;
            const uint32_t suppressed =
                audio.ConsumeSuppressedExecutionMotorMessages();
            passed &= completed && suppressed == 4;
            LogFormat(
                L"Isolated execution profile=%u completed=%d suppressedMotors=%u",
                static_cast<unsigned>(profile), completed ? 1 : 0, suppressed);
        }
        audio.Stop();
        ConfigureControllerSpeaker(controller, false);
        CloseController(controller);
        return passed ? 0 : 7;
    }

    if (argc == 2 && argv[1] != nullptr && wcscmp(argv[1], L"--test-normal-attack-haptic") == 0) {
        using normal_attack_haptics::Kind;
        using normal_attack_haptics::Operation;
        ControllerConnection controller;
        if (!ConfigureControllerSpeaker(controller, true)) return 6;
        AudioHaptics audio;
        product_runtime::Settings settings{};
        audio.SetSpeakerVolumes(settings);
        audio.SetHapticIntensity(1.0f);
        if (!audio.Start(hapticsDirectory)) {
            ConfigureControllerSpeaker(controller, false); CloseController(controller); return 5;
        }
        bool passed = true;
        for (const auto kind : {Kind::Light, Kind::Heavy}) {
            for (bool hit : {false, true}) {
                audio.StopAll();
                audio.ConsumeSubmittedNormalAttackHaptics();
                audio.PlayNormalAttack(kind, Operation::Start);
                for (uint32_t step = 0; step < 8u; ++step) {
                    if (step == 1u) audio.StopAll(true, true); // reproduce game-originated cleanup
                    if (step == 2u && hit) audio.PlayNormalAttack(kind, Operation::Hit);
                    audio.PlayNormalAttack(kind, Operation::Refresh);
                    Sleep(100);
                }
                audio.PlayNormalAttack(kind, Operation::End);
                Sleep(80);
                const uint32_t submitted = audio.ConsumeSubmittedNormalAttackHaptics();
                const uint32_t swingBit = 1u << (static_cast<unsigned>(kind) - 1u);
                const uint32_t hitBit = swingBit << 2u;
                const bool valid = (submitted & swingBit) != 0 &&
                    (hit ? (submitted & hitBit) != 0 : (submitted & hitBit) == 0);
                passed &= valid;
                LogFormat(L"Isolated normal attack kind=%u hit=%d rearSubmitted=0x%X valid=%d gameStopPreserved=1",
                    static_cast<unsigned>(kind), hit ? 1 : 0, submitted, valid ? 1 : 0);
            }
        }
        audio.PlayNormalAttack(Kind::None, Operation::Cancel);
        Sleep(120);
        audio.ConsumeSubmittedNormalAttackHaptics();
        Sleep(120);
        const bool cancelledSilent = audio.ConsumeSubmittedNormalAttackHaptics() == 0;
        passed &= cancelledSilent;
        LogFormat(L"Isolated normal attack explicit cancel silent=%d", cancelledSilent ? 1 : 0);
        audio.Stop(); ConfigureControllerSpeaker(controller, false); CloseController(controller);
        return passed ? 0 : 7;
    }

    if (argc == 2 && argv[1] != nullptr &&
        (wcscmp(argv[1], L"--test-parry-speaker") == 0 ||
         wcscmp(argv[1], L"--test-defense-speaker") == 0)) {
        ControllerConnection controller;
        if (!ConfigureControllerSpeaker(controller, true)) return 6;
        AudioHaptics audio;
        if (!audio.Start(hapticsDirectory)) {
            ConfigureControllerSpeaker(controller, false);
            CloseController(controller);
            return 5;
        }
        bool deflectPassed = true;
        if (wcscmp(argv[1], L"--test-defense-speaker") == 0) {
            Log(L"Isolated defense speaker test: original deflect");
            audio.PlaySpeakerCue(ControllerSpeakerCue::CombatDeflect);
            Sleep(2200);
            const uint32_t deflectBit = 1u << static_cast<unsigned>(ControllerSpeakerCue::CombatDeflect);
            const bool submitted = (audio.ConsumeSubmittedCombatSpeakers() & deflectBit) != 0;
            const bool completed = (audio.ConsumeCompletedCombatSpeakers() & deflectBit) != 0;
            deflectPassed = submitted && completed;
            LogFormat(L"Isolated original deflect submitted=%d completed=%d",
                submitted ? 1 : 0, completed ? 1 : 0);
        }
        Log(L"Isolated parry speaker test: blade");
        audio.PlaySpeakerCue(ControllerSpeakerCue::CombatParryBlade);
        Sleep(2800);
        const uint32_t bladeBit = 1u << static_cast<unsigned>(ControllerSpeakerCue::CombatParryBlade);
        const bool bladeSubmitted = (audio.ConsumeSubmittedCombatSpeakers() & bladeBit) != 0;
        const bool bladeCompleted = (audio.ConsumeCompletedCombatSpeakers() & bladeBit) != 0;
        LogFormat(L"Isolated parry blade submitted=%d completed=%d",
            bladeSubmitted ? 1 : 0, bladeCompleted ? 1 : 0);
        Log(L"Isolated parry speaker test: projectile");
        audio.PlaySpeakerCue(ControllerSpeakerCue::CombatParryProjectile);
        Sleep(2800);
        const uint32_t projectileBit = 1u << static_cast<unsigned>(ControllerSpeakerCue::CombatParryProjectile);
        const bool projectileSubmitted = (audio.ConsumeSubmittedCombatSpeakers() & projectileBit) != 0;
        const bool projectileCompleted = (audio.ConsumeCompletedCombatSpeakers() & projectileBit) != 0;
        LogFormat(L"Isolated parry projectile submitted=%d completed=%d",
            projectileSubmitted ? 1 : 0, projectileCompleted ? 1 : 0);
        audio.Stop();
        ConfigureControllerSpeaker(controller, false);
        CloseController(controller);
        Log(L"Isolated parry speaker test completed");
        return deflectPassed && bladeSubmitted && bladeCompleted &&
            projectileSubmitted && projectileCompleted ? 0 : 7;
    }

    if (argc != 2 || argv[1] == nullptr ||
        wcsncmp(argv[1], PipePrefix, std::size(PipePrefix) - 1) != 0) return 2;

    const wchar_t* pipeName = argv[1];
    HANDLE pipe = CreateNamedPipeW(pipeName, PIPE_ACCESS_INBOUND,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT, 1,
        static_cast<DWORD>(MessageLength * 8), static_cast<DWORD>(MessageLength * 8),
        0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) {
        Log(L"CreateNamedPipeW failed");
        return 3;
    }

    const BOOL connected = ConnectNamedPipe(pipe, nullptr) ? TRUE :
        (GetLastError() == ERROR_PIPE_CONNECTED ? TRUE : FALSE);
    if (!connected) {
        Log(L"ConnectNamedPipe failed");
        CloseHandle(pipe);
        return 4;
    }

#ifdef ONIMUSHA_LOCAL_DEVELOPMENT
    Log(L"LOCAL DEVELOPMENT: product v2.2 / core v4.1; USB / BLUETOOTH");
#elif defined(ONIMUSHA_BLUETOOTH_ENABLED)
    Log(L"Product helper v2.2 USB / BLUETOOTH (bridge core v4.1) connected");
#else
    Log(L"Product helper v2.2 USB ONLY (bridge core v4.1) connected");
#endif
    Log(GetModuleHandleW(L"gameoverlayrenderer64.dll") != nullptr
        ? L"Steam overlay detected" : L"Steam overlay absent");
    const wchar_t* pidText = pipeName + std::size(PipePrefix) - 1;
    wchar_t* pidEnd = nullptr;
    const unsigned long gamePid = wcstoul(pidText, &pidEnd, 10);
    HANDLE touchMapping = nullptr;
    dualsense_touch_input::SharedState* touchState = nullptr;
    std::atomic<bool> touchStop{false};
    std::thread touchThread;
    if (gamePid != 0 && pidEnd != pidText && *pidEnd == L'\0') {
        const std::wstring mappingName = dualsense_touch_input::MappingName(gamePid);
        touchMapping = OpenFileMappingW(FILE_MAP_WRITE, FALSE, mappingName.c_str());
        if (touchMapping != nullptr) {
            touchState = static_cast<dualsense_touch_input::SharedState*>(
                MapViewOfFile(touchMapping, FILE_MAP_WRITE, 0, 0,
                    sizeof(dualsense_touch_input::SharedState)));
        }
    }
    if (touchState != nullptr) {
        touchThread = std::thread(ReadTouchInputs, touchState, std::ref(touchStop));
        Log(L"Touch input reader started (left=L3, right=R3)");
    } else {
        LogFormat(L"Touch input mapping unavailable error=%u", GetLastError());
    }
    ControllerConnection controller;
    if (OpenController(controller)) {
        Log(ConfigureControllerSpeaker(controller, true)
            ? L"DualSense controller speaker route enabled"
            : L"DualSense controller speaker route failed");
    } else {
        Log(L"DualSense unavailable at startup");
    }

    AudioHaptics audio;
    product_runtime::ProductRuntime product;
    audio.SetHapticIntensity(product.GetSettings().hapticIntensity);
    audio.SetSpeakerVolumes(product.GetSettings());
    Log(L"Free edition: enhancements available without activation or a time limit");
    bool audioRecoveryPending = false;
    ULONGLONG nextAudioRecoveryTick = 0;
    if (!audio.Start(hapticsDirectory)) {
        Log(L"Waveform haptics unavailable; adaptive triggers remain enabled");
        audioRecoveryPending = true;
        nextAudioRecoveryTick = GetTickCount64() + 750;
    }
    auto scheduleAudioRecovery = [&](const wchar_t* reason) {
        if (!audioRecoveryPending) Log(reason);
        audioRecoveryPending = true;
        audio.Stop();
        // HID normally returns before the USB audio endpoint. Give Windows a
        // short enumeration window, then retry until the endpoint is active.
        nextAudioRecoveryTick = GetTickCount64() + 300;
    };
    auto recoverAudioIfNeeded = [&]() {
        if (audio.IsStarted()) {
            audioRecoveryPending = false;
            return;
        }
        if (!audioRecoveryPending) {
            Log(L"DualSense haptics stream stopped; scheduling recovery");
            audioRecoveryPending = true;
            nextAudioRecoveryTick = GetTickCount64() + 300;
            return;
        }
        const ULONGLONG now = GetTickCount64();
        if (now < nextAudioRecoveryTick) return;

        audio.Stop();
        Log(L"Attempting DualSense haptics audio recovery");
        if (audio.Start(hapticsDirectory)) {
            Log(L"DualSense haptics audio recovered");
            audioRecoveryPending = false;
            nextAudioRecoveryTick = 0;
        } else {
            Log(L"DualSense haptics audio endpoint not ready; retrying");
            nextAudioRecoveryTick = GetTickCount64() + 1000;
        }
    };
    auto sendControllerReport = [&](const std::array<uint8_t, MessageLength>& report) {
        bool reopened = false;
        const bool sent = SendReport(controller, report, &reopened);
        if (reopened) {
            Log(ConfigureControllerSpeaker(controller, true)
                ? L"DualSense controller speaker route restored"
                : L"DualSense controller speaker route restore failed");
            scheduleAudioRecovery(
                L"DualSense HID reconnected; scheduling haptics audio recovery");
        }
        return sent;
    };
    ULONGLONG nextControllerHealthTick = 0;
    auto recoverControllerIfNeeded = [&]() {
        const ULONGLONG now = GetTickCount64();
        if (now < nextControllerHealthTick) return;
        nextControllerHealthTick = now + 500;

        if (controller.IsOpen()) {
            if (IsSupportedController(controller.handle)) return;
            CloseController(controller);
            Log(L"DualSense HID connection lost; waiting for reconnect");
        }

        if (!OpenController(controller)) return;
        Log(L"DualSense HID connection recovered");
        Log(ConfigureControllerSpeaker(controller, true)
            ? L"DualSense controller speaker route restored"
            : L"DualSense controller speaker route restore failed");
        scheduleAudioRecovery(
            L"DualSense HID recovered; scheduling haptics audio recovery");
    };

    std::array<uint8_t, MessageLength> activeTriggerReport{};
    std::array<uint8_t, MessageLength> requestedTriggerReport{};
    bool hasRequestedTriggerReport = false;
    bool keepTriggerEffect = false;
    ULONGLONG lastTriggerWrite = 0;
    uint8_t activeTreasureRumbleProfile = 0;
    uint8_t activeTreasureRumbleLeft = 0;
    uint8_t activeTreasureRumbleRight = 0;
    ULONGLONG treasureRumbleUntil = 0;
    ULONGLONG treasureRumbleNextRefresh = 0;
    auto sendTreasureChestRumbleReport = [&](uint8_t left, uint8_t right, bool enabled,
                                                bool logEvent = true) {
        // Start from the latest trigger report so a chest pulse cannot erase
        // an adaptive-trigger effect.  Standard rumble is deliberately sent
        // as a separate 0x31 control report; the Bluetooth 0x36 audio stream
        // never carries these fields.
        auto report = activeTriggerReport;
        report[0] = 0x02;
        report[1] &= static_cast<uint8_t>(~StandardRumbleAudioDisableBit);
        report[StandardRumbleRightOffset] = enabled ? right : 0;
        report[StandardRumbleLeftOffset] = enabled ? left : 0;
        report[StandardRumbleEnhancedOffset] = 0;
        if (enabled && (left != 0 || right != 0)) {
            // Current DualSense firmware advertises the enhanced rumble
            // emulation path.  Unlike legacy bit 0, this keeps the full byte
            // amplitude and is the format used by SDL for firmware >= 2.24.
            report[1] |= StandardRumbleAudioDisableBit;
            report[StandardRumbleEnhancedOffset] = StandardRumbleEnhancedBit;
        }
        const bool sent = sendControllerReport(report);
        if (logEvent) {
            LogFormat(
                L"treasureChestStandardRumble %s mode=enhanced left=%u right=%u reportBits=0x%02X enhanced=0x%02X write=%s",
                enabled ? L"start" : L"stop",
                static_cast<unsigned>(left),
                static_cast<unsigned>(right),
                static_cast<unsigned>(report[1]),
                static_cast<unsigned>(report[StandardRumbleEnhancedOffset]),
                sent ? L"ok" : L"failed");
        }
        return sent;
    };
    auto queueTreasureChestRumble = [&](uint8_t profile, float intensity) {
        if (profile < static_cast<uint8_t>(EnvironmentInteractionProfile::TreasureChestOpen) ||
            profile > static_cast<uint8_t>(EnvironmentInteractionProfile::TreasureChestLidLand)) {
            return;
        }
        if (!UseTreasureChestStandardRumble) {
            LogFormat(
                L"treasureChestStandardRumble skipped profile=%u mode=pcm-only revision=chest-output-r2",
                static_cast<unsigned>(profile));
            return;
        }
        intensity = std::clamp(intensity, 0.0f, 1.0f);
        float leftLevel = 0.0f;
        float rightLevel = 0.0f;
        uint32_t durationMs = 0;
        switch (static_cast<EnvironmentInteractionProfile>(profile)) {
        case EnvironmentInteractionProfile::TreasureChestOpen:
            leftLevel = 0.72f;
            rightLevel = 0.46f;
            durationMs = 90;
            break;
        case EnvironmentInteractionProfile::TreasureChestPush:
            leftLevel = 0.98f;
            rightLevel = 0.74f;
            durationMs = 420;
            break;
        case EnvironmentInteractionProfile::TreasureChestLidLand:
            leftLevel = 1.0f;
            rightLevel = 0.92f;
            durationMs = 170;
            break;
        default:
            return;
        }
        const auto ToByte = [intensity](float level) {
            return static_cast<uint8_t>(std::clamp(
                static_cast<int>(std::lround(level * intensity * 255.0f)),
                0,
                255));
        };
        activeTreasureRumbleProfile = profile;
        activeTreasureRumbleLeft = ToByte(leftLevel);
        activeTreasureRumbleRight = ToByte(rightLevel);
        const ULONGLONG now = GetTickCount64();
        treasureRumbleUntil = now + durationMs;
        treasureRumbleNextRefresh = now + 16;
        sendTreasureChestRumbleReport(
            activeTreasureRumbleLeft,
            activeTreasureRumbleRight,
            true);
        LogFormat(
            L"treasureChestStandardRumble profile=%u durationMs=%u",
            static_cast<unsigned>(profile),
            durationMs);
    };
    auto expireTreasureChestRumble = [&]() {
        if (activeTreasureRumbleProfile == 0 || GetTickCount64() < treasureRumbleUntil)
            return;
        const uint8_t profile = activeTreasureRumbleProfile;
        const bool sent = sendTreasureChestRumbleReport(0, 0, false);
        LogFormat(
            L"treasureChestStandardRumble expired profile=%u write=%s",
            static_cast<unsigned>(profile),
            sent ? L"ok" : L"failed");
        activeTreasureRumbleProfile = 0;
        activeTreasureRumbleLeft = 0;
        activeTreasureRumbleRight = 0;
        treasureRumbleUntil = 0;
        treasureRumbleNextRefresh = 0;
    };
    auto refreshTreasureChestRumble = [&]() {
        const ULONGLONG now = GetTickCount64();
        if (activeTreasureRumbleProfile == 0 || now >= treasureRumbleUntil ||
            now < treasureRumbleNextRefresh) {
            return;
        }
        const bool sent = sendTreasureChestRumbleReport(
            activeTreasureRumbleLeft,
            activeTreasureRumbleRight,
            true,
            false);
        if (!sent) {
            LogFormat(
                L"treasureChestStandardRumble refresh profile=%u write=failed",
                static_cast<unsigned>(activeTreasureRumbleProfile));
        }
        treasureRumbleNextRefresh = now + 16;
    };
    float latestRainLevel = 0.0f;
    float latestWindLevel = 0.0f;
    bool lastRainEnabled = product.GetSettings().rainEnabled;
    bool lastWindEnabled = product.GetSettings().windEnabled;
    bool lastControllerSpeakerEnabled =
        product.GetSettings().controllerSpeakerEnabled;
    auto lastGauntletVoiceLanguage =
        product.GetSettings().gauntletVoiceLanguage;
    auto lastSpeakerVolumeSettings = product.GetSettings();
    LogFormat(
        L"Adaptive triggers left=%s right=%s",
        lastSpeakerVolumeSettings.leftAdaptiveTriggerEnabled ? L"on" : L"off",
        lastSpeakerVolumeSettings.rightAdaptiveTriggerEnabled ? L"on" : L"off");
    if (!lastSpeakerVolumeSettings.leftAdaptiveTriggerEnabled ||
        !lastSpeakerVolumeSettings.rightAdaptiveTriggerEnabled) {
        std::array<uint8_t, MessageLength> disabledBaseReport{};
        disabledBaseReport[0] = 0x02;
        const auto disabledTriggerReport =
            adaptive_trigger_effects::ApplyEnablement(
                disabledBaseReport,
                lastSpeakerVolumeSettings.leftAdaptiveTriggerEnabled,
                lastSpeakerVolumeSettings.rightAdaptiveTriggerEnabled);
        sendControllerReport(disabledTriggerReport);
    }
    LogFormat(
        L"Product settings rain=%s wind=%s haptic=%d controllerSpeaker=%s voiceLanguage=%s volumes=voice:%d soul:%d clash:%d guard:%d deflect:%d execution:%d issen:%d",
        lastRainEnabled ? L"on" : L"off",
        lastWindEnabled ? L"on" : L"off",
        static_cast<int>(std::lround(lastSpeakerVolumeSettings.hapticIntensity * 100.0f)),
        lastControllerSpeakerEnabled ? L"on" : L"off",
        lastGauntletVoiceLanguage ==
                product_runtime::GauntletVoiceLanguage::Japanese
            ? L"ja-JP" : L"zh-CN",
        static_cast<int>(std::lround(lastSpeakerVolumeSettings.gauntletVoiceVolume * 100.0f)),
        static_cast<int>(std::lround(lastSpeakerVolumeSettings.soulSpeakerVolume * 100.0f)),
        static_cast<int>(std::lround(lastSpeakerVolumeSettings.clashSpeakerVolume * 100.0f)),
        static_cast<int>(std::lround(lastSpeakerVolumeSettings.guardSpeakerVolume * 100.0f)),
        static_cast<int>(std::lround(lastSpeakerVolumeSettings.deflectSpeakerVolume * 100.0f)),
        static_cast<int>(std::lround(lastSpeakerVolumeSettings.executionSpeakerVolume * 100.0f)),
        static_cast<int>(std::lround(lastSpeakerVolumeSettings.issenSpeakerVolume * 100.0f)));
    LogFormat(
        L"Combat haptic settings guard=%s/%d parry=%s/%d deflect=%s/%d issen=%s/%d",
        lastSpeakerVolumeSettings.guardHapticEnabled ? L"on" : L"off",
        static_cast<int>(std::lround(lastSpeakerVolumeSettings.guardHapticIntensity * 100.0f)),
        lastSpeakerVolumeSettings.parryHapticEnabled ? L"on" : L"off",
        static_cast<int>(std::lround(lastSpeakerVolumeSettings.parryHapticIntensity * 100.0f)),
        lastSpeakerVolumeSettings.deflectHapticEnabled ? L"on" : L"off",
        static_cast<int>(std::lround(lastSpeakerVolumeSettings.deflectHapticIntensity * 100.0f)),
        lastSpeakerVolumeSettings.issenHapticEnabled ? L"on" : L"off",
        static_cast<int>(std::lround(lastSpeakerVolumeSettings.issenHapticIntensity * 100.0f)));
    LogFormat(L"Normal attack haptics always enabled; gameplay gate=fighting-spirit-type1");
#ifdef ONIMUSHA_BLUETOOTH_ENABLED
    speaker_preview::Pipe preview(pipeName);
    Log(preview.IsAvailable() ? L"Speaker preview endpoint ready" : L"Speaker preview endpoint failed");
#endif
    for (;;) {
        recoverControllerIfNeeded();
        const bool controllerConnected = controller.IsOpen() &&
            IsSupportedController(controller.handle);
        product.Tick(controllerConnected);
        if (product.ConsumeControllerSpeakerJustDisabled()) {
            audio.StopSpeakerAudio();
            Log(L"Controller speaker disabled by product settings; all speaker audio stopped");
        }
        const auto& currentSettings = product.GetSettings();
        const bool adaptiveTriggerSettingsChanged =
            lastSpeakerVolumeSettings.leftAdaptiveTriggerEnabled !=
                currentSettings.leftAdaptiveTriggerEnabled ||
            lastSpeakerVolumeSettings.rightAdaptiveTriggerEnabled !=
                currentSettings.rightAdaptiveTriggerEnabled;
        if (lastRainEnabled != currentSettings.rainEnabled ||
            lastWindEnabled != currentSettings.windEnabled ||
            lastControllerSpeakerEnabled !=
                currentSettings.controllerSpeakerEnabled ||
            lastGauntletVoiceLanguage !=
                currentSettings.gauntletVoiceLanguage ||
            lastSpeakerVolumeSettings.hapticIntensity !=
                currentSettings.hapticIntensity ||
            lastSpeakerVolumeSettings.guardHapticEnabled !=
                currentSettings.guardHapticEnabled ||
            lastSpeakerVolumeSettings.guardHapticIntensity !=
                currentSettings.guardHapticIntensity ||
            lastSpeakerVolumeSettings.parryHapticEnabled !=
                currentSettings.parryHapticEnabled ||
            lastSpeakerVolumeSettings.parryHapticIntensity !=
                currentSettings.parryHapticIntensity ||
            lastSpeakerVolumeSettings.deflectHapticEnabled !=
                currentSettings.deflectHapticEnabled ||
            lastSpeakerVolumeSettings.deflectHapticIntensity !=
                currentSettings.deflectHapticIntensity ||
            lastSpeakerVolumeSettings.issenHapticEnabled !=
                currentSettings.issenHapticEnabled ||
            lastSpeakerVolumeSettings.issenHapticIntensity !=
                currentSettings.issenHapticIntensity ||
            lastSpeakerVolumeSettings.gauntletVoiceVolume !=
                currentSettings.gauntletVoiceVolume ||
            lastSpeakerVolumeSettings.soulSpeakerVolume !=
                currentSettings.soulSpeakerVolume ||
            lastSpeakerVolumeSettings.clashSpeakerVolume !=
                currentSettings.clashSpeakerVolume ||
            lastSpeakerVolumeSettings.guardSpeakerVolume !=
                currentSettings.guardSpeakerVolume ||
            lastSpeakerVolumeSettings.parrySpeakerVolume !=
                currentSettings.parrySpeakerVolume ||
            lastSpeakerVolumeSettings.deflectSpeakerVolume !=
                currentSettings.deflectSpeakerVolume ||
            lastSpeakerVolumeSettings.executionSpeakerVolume !=
                currentSettings.executionSpeakerVolume ||
            lastSpeakerVolumeSettings.issenSpeakerVolume !=
                currentSettings.issenSpeakerVolume ||
            adaptiveTriggerSettingsChanged) {
            if (lastGauntletVoiceLanguage !=
                currentSettings.gauntletVoiceLanguage) {
                audio.StopGauntletVoiceOnLanguageChange(currentSettings.gauntletVoiceLanguage);
            }
            audio.SetEnvironment(
                currentSettings.rainEnabled ? latestRainLevel : 0.0f,
                currentSettings.windEnabled ? latestWindLevel : 0.0f);
            audio.SetHapticIntensity(currentSettings.hapticIntensity);
            audio.SetSpeakerVolumes(currentSettings);
            if (adaptiveTriggerSettingsChanged) {
                if (hasRequestedTriggerReport) {
                    activeTriggerReport =
                        adaptive_trigger_effects::ApplyEnablement(
                            requestedTriggerReport,
                            currentSettings.leftAdaptiveTriggerEnabled,
                            currentSettings.rightAdaptiveTriggerEnabled);
                    sendControllerReport(activeTriggerReport);
                    keepTriggerEffect =
                        adaptive_trigger_effects::HasActiveEffect(
                            activeTriggerReport);
                    if (keepTriggerEffect) {
                        lastTriggerWrite = GetTickCount64();
                    }
                } else if (!currentSettings.leftAdaptiveTriggerEnabled ||
                    !currentSettings.rightAdaptiveTriggerEnabled) {
                    std::array<uint8_t, MessageLength> disabledBaseReport{};
                    disabledBaseReport[0] = 0x02;
                    activeTriggerReport =
                        adaptive_trigger_effects::ApplyEnablement(
                            disabledBaseReport,
                            currentSettings.leftAdaptiveTriggerEnabled,
                            currentSettings.rightAdaptiveTriggerEnabled);
                    sendControllerReport(activeTriggerReport);
                    keepTriggerEffect = false;
                } else {
                    activeTriggerReport.fill(0);
                    keepTriggerEffect = false;
                }
                LogFormat(
                    L"Adaptive trigger settings applied left=%s right=%s active=%s",
                    currentSettings.leftAdaptiveTriggerEnabled ? L"on" : L"off",
                    currentSettings.rightAdaptiveTriggerEnabled ? L"on" : L"off",
                    keepTriggerEffect ? L"yes" : L"no");
            }
            LogFormat(
                L"Product settings applied rain=%s wind=%s haptic=%d controllerSpeaker=%s voiceLanguage=%s volumes=voice:%d soul:%d clash:%d guard:%d parry:%d deflect:%d execution:%d issen:%d",
                currentSettings.rainEnabled ? L"on" : L"off",
                currentSettings.windEnabled ? L"on" : L"off",
                static_cast<int>(std::lround(currentSettings.hapticIntensity * 100.0f)),
                currentSettings.controllerSpeakerEnabled ? L"on" : L"off",
                currentSettings.gauntletVoiceLanguage ==
                        product_runtime::GauntletVoiceLanguage::Japanese
                    ? L"ja-JP" : L"zh-CN",
                static_cast<int>(std::lround(currentSettings.gauntletVoiceVolume * 100.0f)),
                static_cast<int>(std::lround(currentSettings.soulSpeakerVolume * 100.0f)),
                static_cast<int>(std::lround(currentSettings.clashSpeakerVolume * 100.0f)),
                static_cast<int>(std::lround(currentSettings.guardSpeakerVolume * 100.0f)),
                static_cast<int>(std::lround(currentSettings.parrySpeakerVolume * 100.0f)),
                static_cast<int>(std::lround(currentSettings.deflectSpeakerVolume * 100.0f)),
                static_cast<int>(std::lround(currentSettings.executionSpeakerVolume * 100.0f)),
                static_cast<int>(std::lround(currentSettings.issenSpeakerVolume * 100.0f)));
            LogFormat(
                L"Combat haptic settings applied guard=%s/%d parry=%s/%d deflect=%s/%d issen=%s/%d",
                currentSettings.guardHapticEnabled ? L"on" : L"off",
                static_cast<int>(std::lround(currentSettings.guardHapticIntensity * 100.0f)),
                currentSettings.parryHapticEnabled ? L"on" : L"off",
                static_cast<int>(std::lround(currentSettings.parryHapticIntensity * 100.0f)),
                currentSettings.deflectHapticEnabled ? L"on" : L"off",
                static_cast<int>(std::lround(currentSettings.deflectHapticIntensity * 100.0f)),
                currentSettings.issenHapticEnabled ? L"on" : L"off",
                static_cast<int>(std::lround(currentSettings.issenHapticIntensity * 100.0f)));
            lastRainEnabled = currentSettings.rainEnabled;
            lastWindEnabled = currentSettings.windEnabled;
            lastControllerSpeakerEnabled =
                currentSettings.controllerSpeakerEnabled;
            lastGauntletVoiceLanguage =
                currentSettings.gauntletVoiceLanguage;
            lastSpeakerVolumeSettings = currentSettings;
        }
        if (controllerConnected) recoverAudioIfNeeded();
        expireTreasureChestRumble();
        refreshTreasureChestRumble();
        audio.FlushEnvironmentDiagnostics();
        const uint32_t normalAttackSubmitted = audio.ConsumeSubmittedNormalAttackHaptics();
        for (uint32_t bit = 0; bit < 4u; ++bit)
            if ((normalAttackSubmitted & (1u << bit)) != 0)
                LogFormat(L"Normal attack rear buffer submitted kind=%u layer=%s", (bit % 2u) + 1u, bit >= 2u ? L"hit" : L"swing");
        const uint32_t completedCombatHaptics =
            audio.ConsumeCompletedCombatHaptics();
        const uint32_t submittedCombatSpeakers = audio.ConsumeSubmittedCombatSpeakers();
        const uint32_t completedCombatSpeakers = audio.ConsumeCompletedCombatSpeakers();
        constexpr ControllerSpeakerCue TrackedDefenseSpeakers[] = {
            ControllerSpeakerCue::CombatDeflect,
            ControllerSpeakerCue::CombatParryBlade,
            ControllerSpeakerCue::CombatParryProjectile};
        for (ControllerSpeakerCue trackedCue : TrackedDefenseSpeakers) {
            const unsigned cueValue = static_cast<unsigned>(trackedCue);
            if ((submittedCombatSpeakers & (1u << cueValue)) != 0) {
                LogFormat(L"Combat speaker buffer submitted cue=%u channels=front", cueValue);
            }
            if ((completedCombatSpeakers & (1u << cueValue)) != 0) {
                LogFormat(L"Combat speaker render completed cue=%u", cueValue);
            }
        }
        for (unsigned profileValue = static_cast<unsigned>(
                 CombatHapticProfile::Guard);
             profileValue <= static_cast<unsigned>(
                 CombatHapticProfile::ExecutionBlockIssen);
             ++profileValue) {
            if ((completedCombatHaptics & (1u << profileValue)) != 0) {
                LogFormat(
                    L"Combat haptic render completed profile=%u",
                    profileValue);
            }
        }
        DWORD available = 0;
#ifdef ONIMUSHA_BLUETOOTH_ENABLED
        preview.Poll([&](const speaker_preview::Request& request) {
            using speaker_preview::Status;
            if (!controllerConnected) return Status::Disconnected;
            const auto settings = speaker_preview::WithVolume(product_runtime::detail::LoadSettings(), request);
            if (!settings.controllerSpeakerEnabled) return Status::Muted;
            if (!audio.IsStarted()) return Status::AudioNotReady;
            audio.SetSpeakerVolumes(settings);
            if (request.cue == 14) {
                const uint32_t trigger = FindPreviewVoice(hapticsDirectory, settings.gauntletVoiceLanguage);
                if (trigger == 0 || !audio.PlayGauntletVoice(trigger, settings.gauntletVoiceLanguage))
                    return Status::AudioNotReady;
            } else {
                audio.PlaySpeakerCue(static_cast<ControllerSpeakerCue>(request.cue));
            }
            LogFormat(L"Speaker preview queued cue=%u volume=%u", request.cue, request.volume);
            return Status::Queued;
        });
#endif
        if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr)) break;
        if (available < MessageLength) {
            const ULONGLONG triggerRefreshInterval =
                controller.IsOpen() ? (controller.bluetooth ? 300 : 8) : 500;
            if (keepTriggerEffect &&
                GetTickCount64() - lastTriggerWrite >= triggerRefreshInterval) {
                sendControllerReport(activeTriggerReport);
                lastTriggerWrite = GetTickCount64();
            }
            Sleep(2);
            continue;
        }
        std::array<uint8_t, MessageLength> message{};
        DWORD read = 0;
        const BOOL ok = ReadFile(pipe, message.data(), static_cast<DWORD>(message.size()),
            &read, nullptr);
        if (!ok || read == 0) break;
        if (read != message.size()) continue;

        if (message[0] == 0x02) {
            requestedTriggerReport = message;
            hasRequestedTriggerReport = true;
            activeTriggerReport = adaptive_trigger_effects::ApplyEnablement(
                requestedTriggerReport,
                currentSettings.leftAdaptiveTriggerEnabled,
                currentSettings.rightAdaptiveTriggerEnabled);
            sendControllerReport(activeTriggerReport);
            keepTriggerEffect = adaptive_trigger_effects::HasActiveEffect(
                activeTriggerReport);
            if (keepTriggerEffect) {
                lastTriggerWrite = GetTickCount64();
            }
        } else if (message[0] == HapticPlayMessage) {
            uint32_t triggerId = 0;
            float gain = 1.0f;
            std::memcpy(&triggerId, message.data() + 4, sizeof(triggerId));
            std::memcpy(&gain, message.data() + 8, sizeof(gain));
            const size_t clip = ClipForTrigger(triggerId);
            if (clip != 0) {
                float rightGain = gain;
                if (message[1] >= 2) {
                    std::memcpy(&rightGain, message.data() + 12, sizeof(rightGain));
                }
                const float multiplier = GainMultiplierForClip(clip);
                const float effectiveLeft = gain * multiplier;
                const float effectiveRight = rightGain * multiplier;
                audio.Play(clip, effectiveLeft, effectiveRight);
                LogFormat(
                    L"Haptic trigger=%u clip=%zu left=%.3f right=%.3f effectiveL=%.3f effectiveR=%.3f",
                    triggerId,
                    clip,
                    gain,
                    rightGain,
                    effectiveLeft,
                    effectiveRight);
            } else {
                LogFormat(L"Unknown haptic trigger=%u", triggerId);
            }
        } else if (message[0] == HapticContinuousPlayerMessage) {
            const uint8_t profileValue = message[1];
            const uint8_t operationValue = message[2];
            float intensity = 0.0f;
            std::memcpy(&intensity, message.data() + 4, sizeof(intensity));
            const bool knownProfile =
                (profileValue >= static_cast<uint8_t>(
                    ContinuousPlayerHaptic::SoulAbsorption) &&
                 profileValue <= static_cast<uint8_t>(
                    ContinuousPlayerHaptic::BlackSoulAbsorption)) ||
                ladder_haptics::IsProfile(profileValue);
            const auto side = message[3] <= static_cast<uint8_t>(
                    ladder_haptics::Side::Center)
                ? static_cast<ladder_haptics::Side>(message[3])
                : ladder_haptics::Side::Center;
            if (knownProfile &&
                operationValue <= static_cast<uint8_t>(
                    ContinuousHapticOperation::Decelerate)) {
                audio.SetContinuousPlayerHaptic(
                    static_cast<ContinuousPlayerHaptic>(profileValue),
                    static_cast<ContinuousHapticOperation>(operationValue),
                    intensity,
                    side);
            } else {
                LogFormat(
                    L"Unknown continuous player haptic profile=%u operation=%u",
                    static_cast<unsigned>(profileValue),
                    static_cast<unsigned>(operationValue));
            }
        } else if (message[0] == HapticCombatActionMessage) {
            const uint8_t profileValue = message[1];
            const uint8_t motionVariant = static_cast<uint8_t>(message[2] & 0x03u);
            if (profileValue >= static_cast<uint8_t>(
                    CombatHapticProfile::Guard) &&
                profileValue <= static_cast<uint8_t>(
                    CombatHapticProfile::ExecutionBlockIssen)) {
                const auto profile = static_cast<CombatHapticProfile>(
                    profileValue);
                const auto& settings = product.GetSettings();
                bool enabled = false;
                float intensity = 1.0f;
                switch (profile) {
                case CombatHapticProfile::Guard:
                    enabled = settings.guardHapticEnabled;
                    intensity = settings.guardHapticIntensity;
                    break;
                case CombatHapticProfile::Parry:
                case CombatHapticProfile::ProjectileParry:
                case CombatHapticProfile::ParryActivation:
                    enabled = settings.parryHapticEnabled;
                    intensity = settings.parryHapticIntensity;
                    break;
                case CombatHapticProfile::Deflect:
                    enabled = settings.deflectHapticEnabled;
                    intensity = settings.deflectHapticIntensity;
                    break;
                case CombatHapticProfile::Issen:
                    enabled = settings.issenHapticEnabled;
                    intensity = settings.issenHapticIntensity;
                    break;
                case CombatHapticProfile::ExecutionThrust:
                case CombatHapticProfile::ExecutionSlash:
                case CombatHapticProfile::ExecutionArmCut:
                case CombatHapticProfile::ExecutionDowned:
                case CombatHapticProfile::ExecutionConst:
                case CombatHapticProfile::ExecutionBreakIssen:
                case CombatHapticProfile::ExecutionBlockIssen:
                    enabled = true;
                    intensity = 1.65f;
                    break;
                default:
                    break;
                }
                if (enabled && intensity > 0.0f) {
                    audio.PlayCombatHaptic(profile, intensity, motionVariant);
                    const uint8_t pairedRepelCue = combat_defense_feedback::PairedRepelSpeaker(
                        static_cast<uint8_t>(profile));
                    if (settings.controllerSpeakerEnabled && pairedRepelCue != 0) {
                        const auto parrySpeakerCue = static_cast<ControllerSpeakerCue>(pairedRepelCue);
                        audio.PlaySpeakerCue(parrySpeakerCue);
                        LogFormat(
                            L"Combat parry speaker paired profile=%u cue=%u",
                            static_cast<unsigned>(profile),
                            static_cast<unsigned>(parrySpeakerCue));
                    }
                    // Successful Repel has one self-contained impact waveform;
                    // do not dilute it with long WAVs or spaced motor pulses.
                    // Keep the existing preparation feedback unchanged.
                    if (profile == CombatHapticProfile::ParryActivation) {
                        audio.PlayMotorPulse(
                            0,
                            std::clamp(
                                intensity * 0.56f,
                                0.0f,
                                1.0f),
                            0.14f,
                            true,
                            0);
                        audio.PlayMotorPulse(
                            1,
                            std::clamp(
                                intensity * 0.44f,
                                0.0f,
                                1.0f),
                            0.10f,
                            true,
                            0);
                    }
                }
            } else {
                LogFormat(
                    L"Unknown combat haptic profile=%u",
                    static_cast<unsigned>(profileValue));
            }
        } else if (message[0] == HapticCapturedPresetMessage) {
            if (captured_haptic_presets::IsValid(message[1])) {
                audio.PlayCapturedPreset(
                    static_cast<captured_haptic_presets::Preset>(message[1]));
            } else {
                LogFormat(L"Unknown captured native preset=%u", message[1]);
            }
        } else if (message[0] == TitleHapticMessage) {
            audio.PlayTitle(message[1]);
        } else if (message[0] == HapticStopAllMessage) {
            audio.StopAll(
                true,
                message[1] == 1u,
                message[2] == 1u,
                message[3] == 1u);
        } else if (message[0] == HapticMotorPowerMessage) {
            uint32_t motor = 0;
            float power = 0.0f;
            std::memcpy(&motor, message.data() + 4, sizeof(motor));
            std::memcpy(&power, message.data() + 8, sizeof(power));
            if (motor < 4) audio.SetMotorPower(motor, power);
        } else if (message[0] == HapticMotorPulseMessage) {
            uint32_t motor = 0;
            float power = 0.0f;
            float duration = 0.0f;
            int32_t easeType = 0;
            std::memcpy(&motor, message.data() + 4, sizeof(motor));
            std::memcpy(&power, message.data() + 8, sizeof(power));
            std::memcpy(&duration, message.data() + 12, sizeof(duration));
            std::memcpy(&easeType, message.data() + 20, sizeof(easeType));
            if (motor < 4) {
                audio.PlayMotorPulse(
                    motor, power, duration, message[16] != 0, easeType);
            }
        } else if (message[0] == HapticStopMotorMessage) {
            audio.StopMotors(message[1] == 1u);
        } else if (message[0] == HapticStopWaveformsMessage) {
            audio.StopWaveforms();
        } else if (message[0] == HapticEnvironmentMessage) {
            float rainLevel = 0.0f;
            float windLevel = 0.0f;
            std::memcpy(&rainLevel, message.data() + 4, sizeof(rainLevel));
            std::memcpy(&windLevel, message.data() + 8, sizeof(windLevel));
            latestRainLevel = rainLevel;
            latestWindLevel = windLevel;
            const auto& settings = product.GetSettings();
            audio.SetEnvironment(
                settings.rainEnabled ? rainLevel : 0.0f,
                settings.windEnabled ? windLevel : 0.0f);
        } else if (message[0] == HapticFootstepMessage) {
            uint32_t surface = 0;
            uint32_t side = 0;
            uint32_t gait = 0;
            uint32_t stepType = 0;
            float pace = 0.0f;
            std::memcpy(&surface, message.data() + 4, sizeof(surface));
            std::memcpy(&side, message.data() + 8, sizeof(side));
            std::memcpy(&gait, message.data() + 12, sizeof(gait));
            std::memcpy(&stepType, message.data() + 16, sizeof(stepType));
            std::memcpy(&pace, message.data() + 20, sizeof(pace));
            audio.PlayFootstep(surface, side, stepType, pace);
            if (surface > 22) {
                LogFormat(
                    L"Unknown footstep surface=%u gait=%u pace=%.2f",
                    surface,
                    gait,
                    pace);
            }
        } else if (message[0] == HapticMapZoomMessage) {
            float direction = 1.0f;
            float intensity = 0.0f;
            std::memcpy(&direction, message.data() + 4, sizeof(direction));
            std::memcpy(&intensity, message.data() + 8, sizeof(intensity));
            audio.SetMapZoom(direction, intensity);
        } else if (message[0] == HapticEnvironmentInteractionMessage) {
            float intensity = 0.0f;
            std::memcpy(&intensity, message.data() + 4, sizeof(intensity));
            const uint8_t profile = message[1];
            if (profile <= static_cast<uint8_t>(
                    EnvironmentInteractionProfile::TreasureChestLidLand)) {
                audio.PlayEnvironmentInteraction(
                    static_cast<EnvironmentInteractionProfile>(profile),
                    intensity);
                if (profile >= static_cast<uint8_t>(
                        EnvironmentInteractionProfile::TreasureChestOpen)) {
                    queueTreasureChestRumble(profile, intensity);
                }
            }
        } else if (message[0] == HapticLongPressMessage) {
            float progress = 0.0f;
            std::memcpy(&progress, message.data() + 4, sizeof(progress));
            audio.SetLongPress(message[1], progress, message[2]);
        } else if (message[0] == HapticOniStrengthMessage) {
            audio.PlayOniStrength(message[1]);
        } else if (message[0] == normal_attack_haptics::Message) {
            if (message[2] <= static_cast<uint8_t>(normal_attack_haptics::Operation::OrdinaryHeavyGroundCrack) &&
                (message[2] == static_cast<uint8_t>(normal_attack_haptics::Operation::Cancel) ||
                 (message[1] >= static_cast<uint8_t>(normal_attack_haptics::Kind::Light) &&
                  message[1] <= static_cast<uint8_t>(normal_attack_haptics::Kind::ChargeHeavy)))) {
                uint32_t swingLeadFrames = 0u;
                uint32_t secondarySwingLeadFrames = 0u;
                std::memcpy(&swingLeadFrames, message.data() + 4, sizeof(swingLeadFrames));
                std::memcpy(&secondarySwingLeadFrames, message.data() + 8, sizeof(secondarySwingLeadFrames));
                audio.PlayNormalAttack(static_cast<normal_attack_haptics::Kind>(message[1]),
                    static_cast<normal_attack_haptics::Operation>(message[2]), swingLeadFrames,
                    secondarySwingLeadFrames);
            }
        } else if (message[0] == wall_run_haptics::Message) {
            float intensity = 0.0f;
            std::memcpy(&intensity, message.data() + 4, sizeof(intensity));
            if (message[1] <= static_cast<uint8_t>(wall_run_haptics::Event::GroundLeap) &&
                message[2] <= static_cast<uint8_t>(wall_run_haptics::Side::Center) &&
                std::isfinite(intensity)) {
                audio.SetWallRunHaptic(static_cast<wall_run_haptics::Event>(message[1]),
                    static_cast<wall_run_haptics::Side>(message[2]), intensity);
            }
        } else if (message[0] == HapticOniEyeMessage) {
            audio.PlayOniEye();
        } else if (message[0] == oni_transformation_haptics::Message) {
            audio.PlayOniTransformation();
        } else if (message[0] == oni_charge_release_haptics::Message) {
            if (message[1] >= static_cast<uint8_t>(oni_charge_release_haptics::Kind::Light) &&
                message[1] <= static_cast<uint8_t>(oni_charge_release_haptics::Kind::FinisherSlash)) {
                audio.PlayOniChargeRelease(
                    static_cast<oni_charge_release_haptics::Kind>(message[1]));
            }
        } else if (message[0] == bow_shot_haptics::Message) {
            if (message[1] >= static_cast<uint8_t>(bow_shot_haptics::Kind::Quick) &&
                message[1] <= static_cast<uint8_t>(bow_shot_haptics::Kind::Charged)) {
                audio.PlayBowShot(
                    static_cast<bow_shot_haptics::Kind>(message[1]));
            }
        } else if (message[0] == HapticPlayerReactionMessage) {
            uint32_t profile = 0;
            float intensity = 0.0f;
            float durationSeconds = 0.0f;
            std::memcpy(&profile, message.data() + 4, sizeof(profile));
            std::memcpy(&intensity, message.data() + 8, sizeof(intensity));
            std::memcpy(&durationSeconds, message.data() + 12, sizeof(durationSeconds));
            if (profile >= static_cast<uint32_t>(PlayerHapticProfile::Hit) &&
                profile <= static_cast<uint32_t>(PlayerHapticProfile::CinematicImpact)) {
                audio.PlayPlayerReaction(
                    static_cast<PlayerHapticProfile>(profile),
                    intensity,
                    durationSeconds);
            }
        } else if (message[0] == ControllerSpeakerMessage) {
            uint32_t cue = 0;
            std::memcpy(&cue, message.data() + 4, sizeof(cue));
            if (product.GetSettings().controllerSpeakerEnabled &&
                cue >= static_cast<uint32_t>(ControllerSpeakerCue::SoulStart) &&
                cue <= static_cast<uint32_t>(
                    ControllerSpeakerCue::CombatParryProjectile)) {
                const auto variant =
                    cue == static_cast<uint32_t>(ControllerSpeakerCue::CombatDeflect)
                    ? combat_defense_feedback::DecodeDeflectSpeakerVariant(message[1])
                    : combat_defense_feedback::DeflectSpeakerVariant::Short;
                if (message[2] == 0) {
                    audio.PlaySpeakerCue(static_cast<ControllerSpeakerCue>(cue), variant);
                    LogFormat(L"Controller speaker cue=%u variant=%u", cue,
                        static_cast<unsigned>(variant));
                } else if (message[2] == 1 &&
                    cue == static_cast<uint32_t>(ControllerSpeakerCue::CombatDeflect) &&
                    variant == combat_defense_feedback::DeflectSpeakerVariant::Slow) {
                    audio.PromoteDeflectSpeakerToCinematic();
                }
            }
        } else if (message[0] == GauntletVoiceMessage) {
            const uint8_t operation = message[1];
            uint32_t triggerId = 0;
            std::memcpy(&triggerId, message.data() + 4, sizeof(triggerId));
            if (operation == 1 &&
                product.GetSettings().controllerSpeakerEnabled) {
                audio.PlayGauntletVoice(
                    triggerId,
                    product.GetSettings().gauntletVoiceLanguage);
            } else if (operation == 2) {
                audio.StopGauntletVoice();
                Log(L"Gauntlet voice stopped");
            }
        }
    }

    touchStop.store(true, std::memory_order_relaxed);
    if (touchThread.joinable()) touchThread.join();
    if (touchState != nullptr) UnmapViewOfFile(touchState);
    if (touchMapping != nullptr) CloseHandle(touchMapping);
    audio.Stop();
    ResetTriggers(controller);
    ConfigureControllerSpeaker(controller, false);
    CloseController(controller);
    DisconnectNamedPipe(pipe);
    CloseHandle(pipe);
    Log(L"Helper stopped; haptics and triggers reset");
    return 0;
}

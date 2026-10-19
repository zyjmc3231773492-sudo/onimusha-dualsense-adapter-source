#define NOMINMAX
#include "speaker_preview_pipe.hpp"
#include "speaker_preview_settings.hpp"
#include <atomic>
#include <cstdio>
#include <stdexcept>
#include <thread>

void Require(bool okay, const char* message) {
    if (!okay) throw std::runtime_error(message);
}
HANDLE Client(const std::wstring& name) {
    const ULONGLONG deadline = GetTickCount64() + 2000;
    do {
        HANDLE client = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE,
            0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (client != INVALID_HANDLE_VALUE) return client;
        Sleep(5);
    } while (GetTickCount64() < deadline);
    throw std::runtime_error("client connect timeout");
}
bool Transfer(HANDLE pipe, uint8_t* data, DWORD size, bool write) {
    OVERLAPPED operation{};
    operation.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    DWORD count = 0;
    BOOL okay = write ? WriteFile(pipe, data, size, &count, &operation)
        : ReadFile(pipe, data, size, &count, &operation);
    if (!okay && GetLastError() == ERROR_IO_PENDING) {
        if (WaitForSingleObject(operation.hEvent, 2000) != WAIT_OBJECT_0) {
            CancelIoEx(pipe, &operation);
            GetOverlappedResult(pipe, &operation, &count, TRUE);
            okay = FALSE;
        } else okay = GetOverlappedResult(pipe, &operation, &count, FALSE);
    }
    CloseHandle(operation.hEvent);
    return okay && count == size;
}
uint8_t RoundTrip(const std::wstring& name, speaker_preview::Packet packet) {
    HANDLE client = Client(name);
    const bool written = Transfer(client, packet.data(), static_cast<DWORD>(packet.size()), true);
    // Deliberately postpone reading: a server that immediately disconnects
    // after its write would discard this acknowledgement.
    Sleep(30);
    speaker_preview::Packet response{};
    const bool read = Transfer(client, response.data(), static_cast<DWORD>(response.size()), false);
    CloseHandle(client);
    Require(written && read && response[0] == 'S' && response[1] == 'P' && response[2] == 1,
        "response was lost, invalid, or timed out");
    return response[3];
}
int main() {
    try {
        using namespace speaker_preview;
        const auto preset = product_runtime::detail::ParseSettings(R"({"SchemaVersion":12,"GauntletVoiceVolumePercent":55,"SoulSpeakerVolumePercent":60,"ParrySpeakerVolumePercent":25})");
        Require(preset.gauntletVoiceVolume == 1 && preset.soulSpeakerVolume == 1 && preset.parrySpeakerVolume == 1,
            "native screenshot calibration migration");
        Require(product_runtime::CalibratedSpeakerScale(1, product_runtime::GauntletSpeakerBaseline) == 0.55f &&
            product_runtime::CalibratedSpeakerScale(1, product_runtime::SoulSpeakerBaseline) == 0.60f &&
            product_runtime::CalibratedSpeakerScale(1, product_runtime::ParrySpeakerBaseline) == 0.25f &&
            product_runtime::CalibratedSpeakerScale(0, product_runtime::ParrySpeakerBaseline) == 0 &&
            product_runtime::CalibratedSpeakerScale(2, product_runtime::ParrySpeakerBaseline) == 0.5f,
            "new 100% calibration, mute and double-scale must use the production gain path");
        const auto legacy = product_runtime::detail::ParseSettings("{\"DeflectSpeakerVolumePercent\":165}");
        Require(legacy.parrySpeakerVolume == 1.65f, "native legacy Repel volume preservation");
        const auto split = product_runtime::detail::ParseSettings("{\"DeflectSpeakerVolumePercent\":165,\"ParrySpeakerVolumePercent\":0}");
        Require(split.deflectSpeakerVolume == 1.65f && split.parrySpeakerVolume == 0, "native Repel mute is independent");
        const auto boosted = product_runtime::detail::ParseSettings("{\"ParrySpeakerVolumePercent\":900}");
        Require(boosted.parrySpeakerVolume == 2, "native Repel volume bound");
        const uint8_t cues[]{2,4,5,6,7,11,12,13,14};
        const float product_runtime::Settings::* targets[]{
            &product_runtime::Settings::soulSpeakerVolume, &product_runtime::Settings::clashSpeakerVolume,
            &product_runtime::Settings::guardSpeakerVolume, &product_runtime::Settings::deflectSpeakerVolume,
            &product_runtime::Settings::executionSpeakerVolume, &product_runtime::Settings::issenSpeakerVolume,
            &product_runtime::Settings::parrySpeakerVolume, &product_runtime::Settings::parrySpeakerVolume,
            &product_runtime::Settings::gauntletVoiceVolume};
        for (size_t i=0; i<std::size(cues); ++i) {
            const auto preview = WithVolume({}, {cues[i], 0});
            Require(preview.*targets[i] == 0, "preview does not use its category's volume");
            if (cues[i] == 12 || cues[i] == 13)
                Require(preview.deflectSpeakerVolume == 1, "Repel preview changed Deflect");
            if (cues[i] == 6) Require(preview.parrySpeakerVolume == 1, "Deflect preview changed Repel");
        }
        const std::wstring base = L"\\\\.\\pipe\\CodexSpeakerSmoke_" + std::to_wstring(GetCurrentProcessId());
        const std::wstring name = base + L"_SpeakerPreview";
        Pipe server(base);
        Require(server.IsAvailable(), "native preview pipe creation");
        std::atomic_uint dispatched = 0;
        std::jthread worker([&](std::stop_token stop) {
            while (!stop.stop_requested()) {
                server.Poll([&](const Request&) { ++dispatched; return Status::Queued; });
                Sleep(1);
            }
        });
        for (auto cue : cues)
            Require(RoundTrip(name, {'S','P',1,cue,200,0,0,0}) == 1, "valid category rejected");
        Require(dispatched == std::size(cues), "missing or duplicate dispatch");
        Require(RoundTrip(name, {'S','P',1,12,201,0,0,0}) == 6, "out-of-range volume accepted");
        Require(RoundTrip(name, {'S','P',1,99,100,0,0,0}) == 6, "unknown cue accepted");
        Require(RoundTrip(name, {'X','P',1,12,100,0,0,0}) == 6, "invalid protocol accepted");
        Require(dispatched == std::size(cues), "malformed request dispatched audio");
        HANDLE partial = Client(name);
        uint8_t shortRequest[]{'S','P',1};
        Require(Transfer(partial, shortRequest, 3, true), "partial request write");
        CloseHandle(partial);
        Require(RoundTrip(name, {'S','P',1,12,0,0,0,0}) == 1, "pipe failed to recover after partial client disconnect");
        Require(dispatched == std::size(cues)+1, "partial request dispatched audio");
        std::puts("Speaker preview: native migration, category isolation, all cues, delayed ACK read, malformed input and reconnect passed.");
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL: %s\n", e.what());
        return 1;
    }
    return 0;
}

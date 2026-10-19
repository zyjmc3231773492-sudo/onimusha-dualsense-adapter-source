#pragma once
#include <Windows.h>
#include <array>
#include <cstdint>
#include <cstring>
#include <string>

// Development-only local IPC. The game helper owns the sole audio stream;
// this endpoint submits sound cues to its existing mixer from the main loop.
namespace speaker_preview {
enum class Status : uint8_t {
    Queued = 1, Muted = 2, Disconnected = 3,
    NotEntitled = 4, AudioNotReady = 5, InvalidRequest = 6
};
struct Request { uint8_t cue; uint32_t volume; };
using Packet = std::array<uint8_t, 8>;
inline bool IsValidCue(uint8_t cue) {
    switch (cue) {
    case 2: case 4: case 5: case 6: case 7:
    case 11: case 12: case 13: case 14: return true;
    default: return false;
    }
}
inline bool Decode(const Packet& packet, Request& request) {
    if (packet[0] != 'S' || packet[1] != 'P' || packet[2] != 1 ||
        !IsValidCue(packet[3])) return false;
    uint32_t volume = 0;
    for (unsigned i = 0; i < 4; ++i)
        volume |= static_cast<uint32_t>(packet[4 + i]) << (8 * i);
    if (volume > 200) return false;
    request = {packet[3], volume};
    return true;
}

class Pipe {
public:
    explicit Pipe(const std::wstring& gamePipeName) {
        m_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (m_event == nullptr) return;
        m_pipe = CreateNamedPipeW((gamePipeName + L"_SpeakerPreview").c_str(),
            PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
            1, 8, 8, 0, nullptr);
        if (m_pipe != INVALID_HANDLE_VALUE) Connect();
    }
    ~Pipe() {
        if (IsAvailable()) {
            CancelPending();
            DisconnectNamedPipe(m_pipe);
            CloseHandle(m_pipe);
        }
        if (m_event != nullptr) CloseHandle(m_event);
    }
    Pipe(const Pipe&) = delete;
    Pipe& operator=(const Pipe&) = delete;
    bool IsAvailable() const { return m_pipe != INVALID_HANDLE_VALUE; }

    template <typename Callback> void Poll(Callback&& callback) {
        if (!IsAvailable()) return;
        if (m_phase == Phase::WaitingDisconnect) {
            // DisconnectNamedPipe discards unread responses. Keep the
            // connection until the client reads its acknowledgement/closes.
            DWORD available = 0;
            if (!PeekNamedPipe(m_pipe, nullptr, 0, nullptr, &available, nullptr) ||
                GetTickCount64() >= m_deadline) Restart();
            return;
        }
        DWORD transferred = m_transferred;
        if (!m_immediate) {
            if (WaitForSingleObject(m_event, 0) != WAIT_OBJECT_0) {
                if (m_deadline != 0 && GetTickCount64() >= m_deadline) Restart();
                return;
            }
            const BOOL okay = GetOverlappedResult(m_pipe, &m_operation, &transferred, FALSE);
            m_pending = false;
            if (!okay) { Restart(); return; }
        }
        m_immediate = false;
        if (m_phase == Phase::Connecting) {
            Read();
        } else if (m_phase == Phase::Reading) {
            if (transferred != m_request.size()) { Restart(); return; }
            Request request{};
            const Status status = Decode(m_request, request)
                ? callback(request) : Status::InvalidRequest;
            m_response = {'S', 'P', 1, static_cast<uint8_t>(status),
                request.cue, static_cast<uint8_t>(request.volume), 0, 0};
            Write();
        } else {
            if (transferred != m_response.size()) { Restart(); return; }
            m_phase = Phase::WaitingDisconnect;
            m_deadline = GetTickCount64() + 1500;
        }
    }

private:
    enum class Phase { Connecting, Reading, Writing, WaitingDisconnect };
    void Prepare(Phase phase) {
        ResetEvent(m_event);
        m_operation = {};
        m_operation.hEvent = m_event;
        m_phase = phase;
        m_immediate = m_pending = false;
        m_transferred = 0;
        m_deadline = phase == Phase::Connecting ? 0 : GetTickCount64() + 1500;
    }
    void Connect() {
        Prepare(Phase::Connecting);
        const BOOL okay = ConnectNamedPipe(m_pipe, &m_operation);
        const DWORD error = okay ? ERROR_SUCCESS : GetLastError();
        if (okay || error == ERROR_PIPE_CONNECTED) m_immediate = true;
        else if (error == ERROR_IO_PENDING) m_pending = true;
        else { CloseHandle(m_pipe); m_pipe = INVALID_HANDLE_VALUE; }
    }
    void Read() {
        Prepare(Phase::Reading);
        m_request.fill(0);
        const BOOL okay = ReadFile(m_pipe, m_request.data(),
            static_cast<DWORD>(m_request.size()), &m_transferred, &m_operation);
        if (okay) m_immediate = true;
        else if (GetLastError() == ERROR_IO_PENDING) m_pending = true;
        else Restart();
    }
    void Write() {
        Prepare(Phase::Writing);
        const BOOL okay = WriteFile(m_pipe, m_response.data(),
            static_cast<DWORD>(m_response.size()), &m_transferred, &m_operation);
        if (okay) m_immediate = true;
        else if (GetLastError() == ERROR_IO_PENDING) m_pending = true;
        else Restart();
    }
    void CancelPending() {
        if (!m_pending) return;
        CancelIoEx(m_pipe, &m_operation);
        DWORD ignored = 0;
        GetOverlappedResult(m_pipe, &m_operation, &ignored, TRUE);
        m_pending = false;
    }
    void Restart() {
        CancelPending();
        DisconnectNamedPipe(m_pipe);
        Connect();
    }
    HANDLE m_pipe = INVALID_HANDLE_VALUE;
    HANDLE m_event = nullptr;
    OVERLAPPED m_operation{};
    Phase m_phase = Phase::Connecting;
    Packet m_request{}, m_response{};
    DWORD m_transferred = 0;
    ULONGLONG m_deadline = 0;
    bool m_pending = false, m_immediate = false;
};
} // namespace speaker_preview

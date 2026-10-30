// discord_ipc.cpp — Implementation of DiscordIpcClient
// Communicates with the Discord desktop client via Windows Named Pipe IPC.
// No external library dependencies.
//
// IMPORTANT: This file must be compiled with PrecompiledHeader=NotUsing
// in the ASI plugin project (it has its own WIN32_LEAN_AND_MEAN guard).

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "discord_ipc.h"

#include <cstdio>
#include <cstring>
#include <string>

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

namespace
{
    // Pack a uint32_t in little-endian byte order into dst[0..3].
    static void PackU32LE(uint8_t* dst, uint32_t value)
    {
        dst[0] = static_cast<uint8_t>(value & 0xFF);
        dst[1] = static_cast<uint8_t>((value >> 8) & 0xFF);
        dst[2] = static_cast<uint8_t>((value >> 16) & 0xFF);
        dst[3] = static_cast<uint8_t>((value >> 24) & 0xFF);
    }

    // Read a little-endian uint32_t from src[0..3].
    static uint32_t UnpackU32LE(const uint8_t* src)
    {
        return static_cast<uint32_t>(src[0])
             | (static_cast<uint32_t>(src[1]) << 8)
             | (static_cast<uint32_t>(src[2]) << 16)
             | (static_cast<uint32_t>(src[3]) << 24);
    }

    // Extract the value of a simple JSON string field by searching for
    // "key":"<value>" in the raw JSON text.  Returns an empty string if not found.
    static std::string ExtractJsonString(const std::string& json, const std::string& key)
    {
        const std::string searchFor = "\"" + key + "\":\"";
        const std::string::size_type keyPos = json.find(searchFor);
        if (keyPos == std::string::npos)
            return std::string();

        const std::string::size_type valueStart = keyPos + searchFor.size();
        std::string::size_type valueEnd = valueStart;
        while (valueEnd < json.size() && json[valueEnd] != '"')
        {
            if (json[valueEnd] == '\\')
                ++valueEnd; // skip escaped char
            ++valueEnd;
        }
        return json.substr(valueStart, valueEnd - valueStart);
    }

    // IPC opcodes
    const uint32_t OpcodeHandshake = 0;
    const uint32_t OpcodeFrame     = 1;
    const uint32_t OpcodeClose     = 2;
    const uint32_t OpcodePing      = 3;
    const uint32_t OpcodePong      = 4;

    // Maximum payload size we will accept from Discord (4 MiB safety cap)
    const uint32_t MaxPayloadBytes = 4u * 1024u * 1024u;
}

// ---------------------------------------------------------------------------
// DiscordIpcClient implementation
// ---------------------------------------------------------------------------

DiscordIpcClient::DiscordIpcClient()
    : m_pipe(INVALID_HANDLE_VALUE)
    , m_connected(0)
    , m_running(0)
    , m_nonceCounter(0)
    , m_readerThread(nullptr)
{
    m_callbacks = DiscordIpcCallbacks();
    InitializeCriticalSection(&m_writeLock);
}

DiscordIpcClient::~DiscordIpcClient()
{
    Shutdown();
    DeleteCriticalSection(&m_writeLock);
}

bool DiscordIpcClient::Initialize(const char* clientId, DiscordIpcCallbacks* callbacks)
{
    // Already connected — shut down first before reinitializing
    if (InterlockedCompareExchange(&m_connected, 0, 0) != 0)
        Shutdown();

    m_clientId = clientId ? clientId : "";
    if (callbacks)
        m_callbacks = *callbacks;
    else
        m_callbacks = DiscordIpcCallbacks();

    if (!ConnectAndHandshake())
        return false;

    // Start background reader thread
    InterlockedExchange(&m_running, 1);
    m_readerThread = CreateThread(nullptr, 0, ReaderThreadProc, this, 0, nullptr);
    if (m_readerThread == nullptr)
    {
        EnterCriticalSection(&m_writeLock);
        HANDLE pipeCopy = m_pipe;
        m_pipe = INVALID_HANDLE_VALUE;
        LeaveCriticalSection(&m_writeLock);
        CloseHandle(pipeCopy);
        InterlockedExchange(&m_connected, 0);
        InterlockedExchange(&m_running, 0);
        return false;
    }

    return true;
}

void DiscordIpcClient::Shutdown()
{
    // Signal the reader thread to stop
    InterlockedExchange(&m_running, 0);

    // Closing the pipe handle causes any blocking ReadFile in the reader thread
    // to return with an error, which lets the thread exit cleanly.
    EnterCriticalSection(&m_writeLock);
    HANDLE pipeCopy = m_pipe;
    m_pipe = INVALID_HANDLE_VALUE;
    LeaveCriticalSection(&m_writeLock);

    if (pipeCopy != INVALID_HANDLE_VALUE)
        CloseHandle(pipeCopy);

    InterlockedExchange(&m_connected, 0);

    if (m_readerThread != nullptr)
    {
        WaitForSingleObject(m_readerThread, 5000);
        CloseHandle(m_readerThread);
        m_readerThread = nullptr;
    }
}

bool DiscordIpcClient::IsConnected() const
{
    return InterlockedCompareExchange(
        const_cast<volatile LONG*>(&m_connected), 0, 0) != 0;
}

bool DiscordIpcClient::ConnectAndHandshake()
{
    for (int i = 0; i <= 9; ++i)
    {
        wchar_t pipeName[64];
        swprintf_s(pipeName, L"\\\\.\\pipe\\discord-ipc-%d", i);

        HANDLE pipe = CreateFileW(
            pipeName,
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);

        if (pipe == INVALID_HANDLE_VALUE)
        {
            const DWORD err = GetLastError();
            if (err == ERROR_PIPE_BUSY)
            {
                WaitNamedPipeW(pipeName, 500);
                pipe = CreateFileW(pipeName,
                    GENERIC_READ | GENERIC_WRITE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE,
                    nullptr, OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL, nullptr);
            }
            if (pipe == INVALID_HANDLE_VALUE)
                continue;
        }

        m_pipe = pipe;

        // Build and send handshake (opcode 0)
        char handshake[300];
        sprintf_s(handshake, sizeof(handshake),
            "{\"v\":1,\"client_id\":\"%s\"}", m_clientId.c_str());

        if (!SendFrame(OpcodeHandshake, std::string(handshake)))
        {
            EnterCriticalSection(&m_writeLock);
            HANDLE pipeCopy = m_pipe;
            m_pipe = INVALID_HANDLE_VALUE;
            LeaveCriticalSection(&m_writeLock);
            CloseHandle(pipeCopy);
            continue;
        }

        // Read the READY response synchronously (reader thread not started yet)
        uint8_t header[8] = {};
        if (!ReadExact(header, 8))
        {
            EnterCriticalSection(&m_writeLock);
            HANDLE pipeCopy = m_pipe;
            m_pipe = INVALID_HANDLE_VALUE;
            LeaveCriticalSection(&m_writeLock);
            CloseHandle(pipeCopy);
            continue;
        }

        const uint32_t opcode     = UnpackU32LE(header);
        const uint32_t payloadLen = UnpackU32LE(header + 4);

        if (payloadLen > MaxPayloadBytes)
        {
            EnterCriticalSection(&m_writeLock);
            HANDLE pipeCopy = m_pipe;
            m_pipe = INVALID_HANDLE_VALUE;
            LeaveCriticalSection(&m_writeLock);
            CloseHandle(pipeCopy);
            continue;
        }

        std::string payload(payloadLen, '\0');
        if (payloadLen > 0 && !ReadExact(&payload[0], payloadLen))
        {
            EnterCriticalSection(&m_writeLock);
            HANDLE pipeCopy = m_pipe;
            m_pipe = INVALID_HANDLE_VALUE;
            LeaveCriticalSection(&m_writeLock);
            CloseHandle(pipeCopy);
            continue;
        }

        // Expect opcode 1 (Frame) with evt=READY
        if (opcode == OpcodeFrame)
        {
            InterlockedExchange(&m_connected, 1);

            if (m_callbacks.onReady)
            {
                const std::string username = ExtractJsonString(payload, "username");
                m_callbacks.onReady(username.empty() ? nullptr : username.c_str());
            }
            return true;
        }

        // Unexpected — try next pipe
        EnterCriticalSection(&m_writeLock);
        HANDLE pipeCopy = m_pipe;
        m_pipe = INVALID_HANDLE_VALUE;
        LeaveCriticalSection(&m_writeLock);
        CloseHandle(pipeCopy);
    }

    return false;
}

bool DiscordIpcClient::SendFrame(uint32_t opcode, const std::string& json)
{
    // Build one contiguous buffer: [header 8 bytes][payload]
    const uint32_t payloadSize = static_cast<uint32_t>(json.size());
    std::string message(8 + payloadSize, '\0');

    uint8_t* header = reinterpret_cast<uint8_t*>(&message[0]);
    PackU32LE(header,     opcode);
    PackU32LE(header + 4, payloadSize);

    if (payloadSize > 0)
        std::memcpy(&message[8], json.data(), payloadSize);

    EnterCriticalSection(&m_writeLock);
    const HANDLE pipe = m_pipe;
    bool ok = false;
    if (pipe != INVALID_HANDLE_VALUE)
    {
        DWORD written = 0;
        ok = WriteFile(pipe, message.data(),
                       static_cast<DWORD>(message.size()), &written, nullptr) != FALSE
             && written == static_cast<DWORD>(message.size());
    }
    LeaveCriticalSection(&m_writeLock);

    if (!ok)
        InterlockedExchange(&m_connected, 0);

    return ok;
}

bool DiscordIpcClient::ReadExact(void* buffer, DWORD count)
{
    DWORD totalRead = 0;
    uint8_t* dst = static_cast<uint8_t*>(buffer);
    while (totalRead < count)
    {
        HANDLE pipe = m_pipe;
        if (pipe == INVALID_HANDLE_VALUE)
            return false;

        DWORD bytesRead = 0;
        if (!ReadFile(pipe, dst + totalRead, count - totalRead, &bytesRead, nullptr)
            || bytesRead == 0)
            return false;

        totalRead += bytesRead;
    }
    return true;
}

void DiscordIpcClient::SendPong(const std::string& payload)
{
    SendFrame(OpcodePong, payload);
}

bool DiscordIpcClient::SetActivity(const DiscordActivity& activity)
{
    // Increment nonce atomically
    const LONG nonce = InterlockedIncrement(&m_nonceCounter);
    char nonceStr[32];
    sprintf_s(nonceStr, sizeof(nonceStr), "drpc-%ld", static_cast<long>(nonce));

    // Build the "activity" object fields incrementally
    std::string activityJson = "{";
    bool firstField = true;

    // Lambda to append a JSON string field
    auto appendStrField = [&](const char* key, const std::string& value) {
        if (value.empty()) return;
        if (!firstField) activityJson += ",";
        activityJson += "\"";
        activityJson += key;
        activityJson += "\":\"";
        activityJson += JsonEscape(value);
        activityJson += "\"";
        firstField = false;
    };

    appendStrField("details", activity.details);
    appendStrField("state",   activity.state);

    // timestamps
    if (activity.startTimestamp != 0 || activity.endTimestamp != 0)
    {
        if (!firstField) activityJson += ",";
        activityJson += "\"timestamps\":{";
        bool firstTs = true;

        if (activity.startTimestamp != 0)
        {
            char buf[32];
            sprintf_s(buf, sizeof(buf), "%lld", static_cast<long long>(activity.startTimestamp));
            activityJson += "\"start\":";
            activityJson += buf;
            firstTs = false;
        }
        if (activity.endTimestamp != 0)
        {
            if (!firstTs) activityJson += ",";
            char buf[32];
            sprintf_s(buf, sizeof(buf), "%lld", static_cast<long long>(activity.endTimestamp));
            activityJson += "\"end\":";
            activityJson += buf;
        }
        activityJson += "}";
        firstField = false;
    }

    // assets
    const bool hasAsset = !activity.largeImageKey.empty() || !activity.largeImageText.empty()
                       || !activity.smallImageKey.empty() || !activity.smallImageText.empty();
    if (hasAsset)
    {
        if (!firstField) activityJson += ",";
        activityJson += "\"assets\":{";
        bool firstAsset = true;

        auto appendAsset = [&](const char* key, const std::string& value) {
            if (value.empty()) return;
            if (!firstAsset) activityJson += ",";
            activityJson += "\"";
            activityJson += key;
            activityJson += "\":\"";
            activityJson += JsonEscape(value);
            activityJson += "\"";
            firstAsset = false;
        };

        appendAsset("large_image", activity.largeImageKey);
        appendAsset("large_text",  activity.largeImageText);
        appendAsset("small_image", activity.smallImageKey);
        appendAsset("small_text",  activity.smallImageText);
        activityJson += "}";
        firstField = false;
    }

    // buttons (max 2)
    const bool hasBtn1 = !activity.button1Label.empty() && !activity.button1Url.empty();
    const bool hasBtn2 = !activity.button2Label.empty() && !activity.button2Url.empty();
    if (hasBtn1 || hasBtn2)
    {
        if (!firstField) activityJson += ",";
        activityJson += "\"buttons\":[";
        bool firstBtn = true;

        auto appendBtn = [&](const std::string& label, const std::string& url) {
            if (!firstBtn) activityJson += ",";
            activityJson += "{\"label\":\"";
            activityJson += JsonEscape(label);
            activityJson += "\",\"url\":\"";
            activityJson += JsonEscape(url);
            activityJson += "\"}";
            firstBtn = false;
        };

        if (hasBtn1) appendBtn(activity.button1Label, activity.button1Url);
        if (hasBtn2) appendBtn(activity.button2Label, activity.button2Url);
        activityJson += "]";
        firstField = false;
    }

    activityJson += "}";

    // Build full SET_ACTIVITY command
    char pidBuf[32];
    sprintf_s(pidBuf, sizeof(pidBuf), "%lu",
        static_cast<unsigned long>(GetCurrentProcessId()));

    std::string frame = "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":";
    frame += pidBuf;
    frame += ",\"activity\":";
    frame += activityJson;
    frame += "},\"nonce\":\"";
    frame += nonceStr;
    frame += "\"}";

    return SendFrame(OpcodeFrame, frame);
}

bool DiscordIpcClient::ClearActivity()
{
    const LONG nonce = InterlockedIncrement(&m_nonceCounter);
    char nonceStr[32];
    sprintf_s(nonceStr, sizeof(nonceStr), "drpc-%ld", static_cast<long>(nonce));

    char pidBuf[32];
    sprintf_s(pidBuf, sizeof(pidBuf), "%lu",
        static_cast<unsigned long>(GetCurrentProcessId()));

    std::string frame = "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":";
    frame += pidBuf;
    frame += ",\"activity\":{}},\"nonce\":\"";
    frame += nonceStr;
    frame += "\"}";

    return SendFrame(OpcodeFrame, frame);
}

// ---------------------------------------------------------------------------
// Background reader thread
// ---------------------------------------------------------------------------

DWORD WINAPI DiscordIpcClient::ReaderThreadProc(void* param)
{
    static_cast<DiscordIpcClient*>(param)->ReaderLoop();
    return 0;
}

void DiscordIpcClient::ReaderLoop()
{
    while (InterlockedCompareExchange(&m_running, 1, 1) != 0)
    {
        // Read 8-byte header
        uint8_t header[8] = {};
        if (!ReadExact(header, 8))
        {
            // Pipe error or closed — signal disconnect
            const LONG wasConnected = InterlockedExchange(&m_connected, 0);
            if (wasConnected != 0 && m_callbacks.onDisconnected)
                m_callbacks.onDisconnected(0, "Pipe read error or connection closed");
            break;
        }

        const uint32_t opcode     = UnpackU32LE(header);
        const uint32_t payloadLen = UnpackU32LE(header + 4);

        if (payloadLen > MaxPayloadBytes)
        {
            InterlockedExchange(&m_connected, 0);
            if (m_callbacks.onError)
                m_callbacks.onError(1, "Incoming payload exceeds size limit");
            break;
        }

        std::string payload(payloadLen, '\0');
        if (payloadLen > 0 && !ReadExact(&payload[0], payloadLen))
        {
            const LONG wasConnected = InterlockedExchange(&m_connected, 0);
            if (wasConnected != 0 && m_callbacks.onDisconnected)
                m_callbacks.onDisconnected(0, "Pipe read error on payload");
            break;
        }

        switch (opcode)
        {
        case OpcodeFrame:
            // Normal response frame — nothing to do for basic RPC use
            break;

        case OpcodeClose:
            InterlockedExchange(&m_connected, 0);
            if (m_callbacks.onDisconnected)
                m_callbacks.onDisconnected(0, "Discord sent Close opcode");
            return;

        case OpcodePing:
            // Discord sent a ping — reply with pong to keep the connection alive
            SendPong(payload);
            break;

        default:
            // Unknown opcode — ignore
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// JSON escape helper
// ---------------------------------------------------------------------------

std::string DiscordIpcClient::JsonEscape(const std::string& s)
{
    std::string out;
    out.reserve(s.size() + 8);
    for (size_t i = 0; i < s.size(); ++i)
    {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        switch (c)
        {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b";  break;
        case '\f': out += "\\f";  break;
        case '\n': out += "\\n";  break;
        case '\r': out += "\\r";  break;
        case '\t': out += "\\t";  break;
        default:
            if (c < 0x20)
            {
                char esc[8];
                sprintf_s(esc, sizeof(esc), "\\u%04x", static_cast<unsigned>(c));
                out += esc;
            }
            else
            {
                out += static_cast<char>(c);
            }
            break;
        }
    }
    return out;
}

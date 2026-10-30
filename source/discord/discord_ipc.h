// discord_ipc.h — Discord Rich Presence via Windows Named Pipe IPC
// No external library dependencies; communicates directly with the Discord desktop client.
//
// Protocol summary:
//   - Connect to \\.\pipe\discord-ipc-0 ... \\.\pipe\discord-ipc-9
//   - Each message = 8-byte header (opcode u32 LE + length u32 LE) + UTF-8 JSON payload
//   - Opcodes: 0=Handshake, 1=Frame, 2=Close, 3=Ping, 4=Pong
//   - After connecting, send Handshake immediately and read the READY response

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdint>
#include <string>

// ---------------------------------------------------------------------------
// Data types
// ---------------------------------------------------------------------------

struct DiscordActivity
{
    std::string details;         // First line of presence text (<=128 bytes)
    std::string state;           // Second line of presence text (<=128 bytes)
    std::string largeImageKey;   // Asset key for the large image (<=32 bytes) OR https:// URL
    std::string largeImageText;  // Hover text for the large image (<=128 bytes)
    std::string smallImageKey;   // Asset key for the small image (<=32 bytes) OR https:// URL
    std::string smallImageText;  // Hover text for the small image (<=128 bytes)
    std::string button1Label;    // Label for the first button (<=32 bytes)
    std::string button1Url;      // URL for the first button (must start with https://)
    std::string button2Label;    // Label for the second button (<=32 bytes)
    std::string button2Url;      // URL for the second button (must start with https://)
    int64_t     startTimestamp;  // Unix epoch seconds; 0 = omit
    int64_t     endTimestamp;    // Unix epoch seconds; 0 = omit

    DiscordActivity() : startTimestamp(0), endTimestamp(0) {}
};

struct DiscordIpcCallbacks
{
    // Called when Discord sends the READY event (connection established).
    // username: the logged-in Discord username (may be nullptr on parse failure).
    void (*onReady)(const char* username);

    // Called when the IPC connection is lost.
    void (*onDisconnected)(int code, const char* message);

    // Called when a protocol-level error occurs.
    void (*onError)(int code, const char* message);

    DiscordIpcCallbacks() : onReady(nullptr), onDisconnected(nullptr), onError(nullptr) {}
};

// ---------------------------------------------------------------------------
// Client class
// ---------------------------------------------------------------------------

class DiscordIpcClient
{
public:
    DiscordIpcClient();
    ~DiscordIpcClient();

    // Connects to the Discord named pipe, sends the handshake, and starts the
    // background reader thread.  Returns true on success.
    bool Initialize(const char* clientId, DiscordIpcCallbacks* callbacks);

    // Stops the reader thread, closes the pipe, and clears presence on the
    // Discord side (by dropping the connection).
    void Shutdown();

    // Sends a SET_ACTIVITY command to Discord.  Returns false if the send
    // fails; after a failure IsConnected() will return false and the caller
    // should call Initialize() again to reconnect.
    bool SetActivity(const DiscordActivity& activity);

    // Sends SET_ACTIVITY with an empty activity object to clear the presence.
    bool ClearActivity();

    // Returns true while the pipe connection is open.
    bool IsConnected() const;

private:
    // Disable copying
    DiscordIpcClient(const DiscordIpcClient&);
    DiscordIpcClient& operator=(const DiscordIpcClient&);

    // Tries to open one of the discord-ipc-N pipes and send the handshake.
    bool ConnectAndHandshake();

    // Sends raw bytes over the pipe (header + payload) as one atomic WriteFile.
    bool SendFrame(uint32_t opcode, const std::string& json);

    // Reads exactly 'count' bytes from the pipe into 'buffer'.
    bool ReadExact(void* buffer, DWORD count);

    // Background thread that reads incoming frames.
    static DWORD WINAPI ReaderThreadProc(void* param);
    void ReaderLoop();

    // Sends a Pong frame (opcode 4) in response to a Ping (opcode 3).
    void SendPong(const std::string& payload);

    // JSON helpers (static, no allocation)
    static std::string JsonEscape(const std::string& s);

    // State
    HANDLE              m_pipe;
    std::string         m_clientId;
    DiscordIpcCallbacks m_callbacks;
    volatile LONG       m_connected;   // 1 = connected, 0 = disconnected
    volatile LONG       m_running;     // 1 = reader thread should keep going

    // Nonce counter (incremented atomically)
    volatile LONG       m_nonceCounter;

    // Reader thread handle
    HANDLE              m_readerThread;

    // Mutex protecting m_pipe writes (reads happen only on the reader thread)
    CRITICAL_SECTION    m_writeLock;
};

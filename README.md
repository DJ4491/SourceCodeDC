# Discord Rich Presence — Direct IPC Edition

This repository contains two Visual Studio projects that publish Discord Rich Presence by communicating **directly** with the Discord desktop client over Windows Named Pipes.
The deprecated `discord-rpc.lib` / `discord-rpc.dll` dependency has been completely removed.
There are **no external libraries or DLLs to ship**.

---

## Projects

| Project | Output | Description |
|---|---|---|
| `DiscordPresenceTemplate` | `DiscordPresenceTemplate.exe` | Standalone Win32 GUI app that monitors a target process and publishes presence |
| `CodeChefPresence` | `CodeChefPresence.asi` | ASI plugin (loaded by [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader)) for the CodeChef application |

---

## Architecture

### Discord IPC protocol

The Discord desktop client exposes a named pipe at `\\.\pipe\discord-ipc-0` through `\\.\pipe\discord-ipc-9`.
Every message is framed as:

```
[opcode  : uint32 LE]  4 bytes
[length  : uint32 LE]  4 bytes
[payload : UTF-8 JSON] <length> bytes
```

| Opcode | Name      | Direction       |
|--------|-----------|-----------------|
| 0      | Handshake | client → Discord |
| 1      | Frame     | both            |
| 2      | Close     | both            |
| 3      | Ping      | Discord → client |
| 4      | Pong      | client → Discord |

1. **Connect** — open the first available pipe.
2. **Handshake** — send `{"v":1,"client_id":"<id>"}` (opcode 0).
3. **READY** — read the response frame; Discord replies with a READY event containing the logged-in user's name.
4. **SET_ACTIVITY** — send a Frame (opcode 1) with the presence JSON.
5. **Clear** — send SET_ACTIVITY with an empty `activity` object.
6. **Disconnect** — close the pipe handle; Discord clears presence automatically.

### Self-contained IPC implementation

| File | Purpose |
|---|---|
| `source/discord/discord_ipc.h` | Public API — `DiscordActivity`, `DiscordIpcCallbacks`, `DiscordIpcClient` |
| `source/discord/discord_ipc.cpp` | Full implementation (Windows API only, no third-party deps) |

`discord_ipc.cpp` is compiled directly into each project — no `.lib` file to manage.

---

## Building

Requirements:
- Visual Studio 2022 (v143 toolset)
- Windows 10 SDK
- x64 target only

Open `source/DiscordPresenceTemplate.vcxproj` or `source/CodeChefPresence.vcxproj` (or the solution file if present) in Visual Studio and build.

---

## DiscordPresenceTemplate — Setup

1. Go to the [Discord Developer Portal](https://discord.com/developers/applications) and create (or select) an application.
2. Note the **Application ID** (a long numeric string).
3. Optionally upload images under **Rich Presence → Art Assets** and copy their keys.
4. Run `DiscordPresenceTemplate.exe`.
5. Fill in the fields:
   - **Discord Application ID** *(required)* — the numeric ID from step 2.
   - **Target process** — e.g. `MyGame.exe`. Leave blank to show presence for any foreground window.
   - **Activity details** — first line of presence text (≤ 128 bytes).
   - **Activity state** — second line of presence text (≤ 128 bytes).
   - **Large / Small image asset key** — key from Developer Portal (≤ 32 bytes).
   - **Large / Small image hover text** — tooltip shown on hover (≤ 128 bytes).
   - **Button 1 / Button 2 label + URL** — up to two clickable buttons shown in the presence card. The URL **must start with `https://`** if a label is provided. Labels are ≤ 32 bytes; URLs are ≤ 512 bytes.
6. Click **Start presence**.
   - Switch to the target app; presence appears in Discord immediately.
   - Switch away; presence clears automatically.
7. **Save settings** writes the current values to `DiscordPresenceTemplate.ini` beside the exe.
   Settings are loaded automatically on next launch.
8. **Stop** disconnects from Discord and clears presence.

### Auto-reconnect

If Discord is not running when you click **Start**, the monitor thread retries the connection approximately every 3 seconds without any user action.

---

## CodeChefPresence — Setup

1. Build the project; the output is `CodeChefPresence.asi`.
2. Place `CodeChefPresence.asi` (and the Ultimate ASI Loader `dinput8.dll` or equivalent) in the CodeChef application directory.
3. Create `CodeChefPresence.ini` in the **same folder** as the `.asi` file:

```ini
[Discord]
ApplicationId=YOUR_APPLICATION_ID
LargeImageKey=codechef_logo
```

- `ApplicationId` — numeric Discord application ID *(required)*.
- `LargeImageKey` — image asset key (≤ 32 chars, defaults to `codechef_logo`).

4. Launch CodeChef; the plugin publishes presence when the CodeChef window is in the foreground and clears it when another window takes focus.

---

## INI format (DiscordPresenceTemplate)

```ini
[Discord]
ApplicationId=123456789012345678
TargetProcess=MyGame.exe
Details=Playing My Game
State=In the main menu
LargeImageKey=game_logo
LargeImageText=My Game
SmallImageKey=icon_idle
SmallImageText=Idle
Button1Label=Visit website
Button1Url=https://example.com
Button2Label=Join Discord
Button2Url=https://discord.gg/example
```

---

## License

See [LICENSE](LICENSE).

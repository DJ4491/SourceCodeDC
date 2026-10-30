# Universal Discord Rich Presence Template

`DiscordPresenceTemplate.exe` is a standalone x64 Windows GUI application. It publishes a configurable Discord Rich Presence to your profile and across Discord while any selected desktop application or game is in the foreground. It communicates directly with the local Discord desktop client via native Windows Named Pipes (`\\.\pipe\discord-ipc-0` .. `9`), completely replacing the deprecated `discord-rpc` library with zero external DLL or SDK dependencies.

## Key Features

- **Universal Process Monitoring**: Enter any `.exe` name (e.g. `hl2.exe`, `CodeChef.exe`, `game.exe`) or browse to it. Rich Presence automatically activates when the game/app is focused and clears when minimized/switched away.
- **Global / Any-App Mode**: Leave the target executable field blank to broadcast your custom presence while *any* non-Discord app is in the foreground.
- **Direct IPC Architecture**: Native 8-byte framed JSON-over-Named-Pipes protocol. No external `.dll` or `.lib` required.
- **Visible to Everyone**: Broadcasts presence via the Discord desktop client to Discord Gateway so that everyone on your friends list and shared servers can see your rich presence card.
- **Interactive Action Buttons**: Support for up to two custom clickable buttons (e.g., website, game invite, discord server link).
- **Auto-Reconnect**: Seamlessly detects Discord startup, shutdown, and restart, reconnecting automatically in the background.
- **Persistent Configuration**: Automatically loads and saves settings to `DiscordPresenceTemplate.ini` beside the executable.

## Setup & Usage

1. **Prerequisites**: Make sure the Discord desktop client is installed and running.
2. **Discord Developer Application**:
   - Go to the [Discord Developer Portal](https://discord.com/developers/applications).
   - Click **New Application** and give it the name you want displayed on Discord (e.g., `My Favorite Game`).
   - Copy the numeric **Application ID** (Client ID) from the **General Information** page.
   - *(Optional)* Under **Rich Presence -> Art Assets**, upload images for `LargeImageKey` and `SmallImageKey`. Alternatively, public `https://` direct image links can also be used.
3. **Run `DiscordPresenceTemplate.exe`**:
   - Paste the **Application ID**.
   - Set **Target process (.exe)** to the name of your game/executable (or click **Browse for app...**). Leave blank for all foreground apps.
   - Enter **Activity details** (first line of text) and **Activity state** (second line).
   - *(Optional)* Set image keys/URLs, hover tooltips, and button labels/URLs (`https://...`).
4. **Click "Start presence"**:
   - Settings are automatically saved to `DiscordPresenceTemplate.ini`.
   - The status bar will show `Connected to Discord as <username>`.
   - Whenever the target application window is focused, Discord displays your full Rich Presence card!
5. **Click "Stop"** to clear presence and disconnect from Discord IPC.

## Ensuring Presence is Visible to Others

For Discord to broadcast your presence to other users (and not just yourself):
1. In Discord, go to **User Settings -> Activity Privacy**.
2. Enable **"Display current activity as a status message"**.
3. Ensure your online status is set to **Online**, **Idle**, or **Do Not Disturb** (not **Invisible**).
4. If testing on specific servers, ensure server privacy allows activity sharing.

## Build from Source

Open `source/DiscordPresenceTemplate.vcxproj` in Visual Studio 2022 or build using MSBuild:
```powershell
MSBuild.exe source\DiscordPresenceTemplate.vcxproj /p:Configuration=Release /p:Platform=x64
```
The output binary will be created at `source/x64/Release/DiscordPresenceTemplate.exe`.

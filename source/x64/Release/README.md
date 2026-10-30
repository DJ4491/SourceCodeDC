# CodeChef Discord Presence — Direct IPC Edition

A Windows ASI plugin for the standalone CodeChef desktop app (or games using the Ultimate ASI Loader). It identifies the host process and publishes Discord Rich Presence via the native Discord IPC Named Pipe architecture whenever the application window is in the foreground.

## Architecture

This build uses the modern **Direct IPC Architecture**:
- Connects directly to `\\.\pipe\discord-ipc-0` through `discord-ipc-9`.
- Eliminates the deprecated `discord-rpc.lib` / `discord-rpc.dll` dependency.
- Self-contained, lightweight C++ implementation with zero external runtime libraries.
- Transmits presence data through the Discord desktop client to Discord's Gateway, making your activity visible to everyone across servers and direct messages.

## Requirements and setup

1. CodeChef (or your target game) is 64-bit. Use a matching 64-bit Ultimate ASI Loader. If the target application loads `VERSION.dll`, use the loader's [64-bit `version.dll` proxy](https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases), extracted beside the game executable.
2. Copy `CodeChefPresence.asi` and `CodeChefPresence.ini.example` beside the executable.
3. Rename the example to `CodeChefPresence.ini` and set `ApplicationId` to your numeric Discord Application ID from the [Discord Developer Portal](https://discord.com/developers/applications).
4. *(Optional)* Set `LargeImageKey` to an asset key uploaded in the Developer Portal, or a direct `https://` image link.
5. Launch the application and the Discord desktop client.

## Build

Open `source/CodeChefPresence.vcxproj` in Visual Studio 2022 and build the Release x64 configuration. The output is `CodeChefPresence.asi`.
```powershell
MSBuild.exe source\CodeChefPresence.vcxproj /p:Configuration=Release /p:Platform=x64
```

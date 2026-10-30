// CodeChefPresence.cpp
// ASI plugin: publishes Discord Rich Presence while CodeChef.exe is the
// foreground process, using the new direct Discord IPC protocol.
// Loaded by Ultimate ASI Loader (via version.dll proxy beside CodeChef.exe).
//
// No external SDK or .lib files are required — the IPC implementation is
// compiled directly into this DLL.

#include "pch.h"
#include "discord/discord_ipc.h"

#include <cctype>
#include <cstdio>
#include <ctime>
#include <cwchar>
#include <string>

// pch.h / framework.h already bring in <windows.h>

namespace
{
	HMODULE      pluginModule = nullptr;
	volatile LONG initialized = FALSE;
	volatile LONG running     = TRUE;

	// -------------------------------------------------------------------------
	// Callbacks from the IPC reader thread
	// -------------------------------------------------------------------------
	void OnDiscordReady(const char* username)
	{
		char output[256] = {};
		sprintf_s(output, sizeof(output),
			"CodeChef Presence: connected to Discord%s%s.\n",
			username ? " as " : "",
			username ? username : "");
		OutputDebugStringA(output);
	}

	void OnDiscordDisconnected(int errorCode, const char* message)
	{
		char output[256] = {};
		sprintf_s(output, sizeof(output),
			"CodeChef Presence: disconnected from Discord (%d): %s\n",
			errorCode, message ? message : "unknown error");
		OutputDebugStringA(output);
	}

	void OnDiscordError(int errorCode, const char* message)
	{
		char output[256] = {};
		sprintf_s(output, sizeof(output),
			"CodeChef Presence: Discord IPC error (%d): %s\n",
			errorCode, message ? message : "unknown error");
		OutputDebugStringA(output);
	}

	// -------------------------------------------------------------------------
	// Process / foreground detection
	// -------------------------------------------------------------------------
	bool IsCodeChefProcess()
	{
		wchar_t executablePath[MAX_PATH] = {};
		const DWORD length = GetModuleFileNameW(nullptr, executablePath, MAX_PATH);
		if (length == 0 || length >= MAX_PATH)
			return false;

		const wchar_t* executableName = wcsrchr(executablePath, L'\\');
		executableName = (executableName == nullptr) ? executablePath : executableName + 1;
		if (_wcsicmp(executableName, L"CodeChef.exe") != 0)
			return false;

		// Avoid matching renderer / GPU helper sub-processes
		return wcsstr(GetCommandLineW(), L"--type=") == nullptr;
	}

	bool IsCodeChefForeground()
	{
		const HWND window = GetForegroundWindow();
		if (window == nullptr)
			return false;

		DWORD foregroundProcessId = 0;
		GetWindowThreadProcessId(window, &foregroundProcessId);
		return foregroundProcessId == GetCurrentProcessId();
	}

	// -------------------------------------------------------------------------
	// INI config helpers
	// -------------------------------------------------------------------------
	bool GetConfigurationPath(std::string& path)
	{
		char pluginPath[MAX_PATH] = {};
		const DWORD length = GetModuleFileNameA(pluginModule, pluginPath, MAX_PATH);
		if (length == 0 || length >= MAX_PATH)
			return false;

		path.assign(pluginPath, length);
		const std::string::size_type sep = path.find_last_of("\\/");
		path.resize(sep == std::string::npos ? 0 : sep + 1);
		path += "CodeChefPresence.ini";
		return true;
	}

	bool IsNumericApplicationId(const char* applicationId)
	{
		if (applicationId[0] == '\0')
			return false;

		for (const auto* c = reinterpret_cast<const unsigned char*>(applicationId);
			*c != '\0'; ++c)
		{
			if (std::isdigit(*c) == 0)
				return false;
		}
		return true;
	}

	// -------------------------------------------------------------------------
	// Publish / clear presence helpers
	// -------------------------------------------------------------------------
	void UpdateCodeChefPresence(DiscordIpcClient& client,
		const char* imageKey, const char* imageText)
	{
		DiscordActivity activity = {};
		activity.details        = "Coding on CodeChef";
		activity.state          = "Practicing";
		activity.startTimestamp = static_cast<int64_t>(std::time(nullptr));
		if (imageKey && imageKey[0] != '\0')
		{
			activity.largeImageKey  = imageKey;
			activity.largeImageText = imageText && imageText[0] != '\0' ? imageText : "CodeChef";
		}
		client.SetActivity(activity);
	}

	// -------------------------------------------------------------------------
	// Presence monitor thread
	// -------------------------------------------------------------------------
	DWORD WINAPI RunPresence(void*)
	{
		if (!IsCodeChefProcess())
		{
			OutputDebugStringA("CodeChef Presence: refusing to run outside CodeChef.exe.\n");
			return 1;
		}

		std::string configurationPath;
		if (!GetConfigurationPath(configurationPath))
		{
			MessageBoxA(nullptr, "Could not determine the plugin path.",
				"CodeChef Presence", MB_ICONERROR);
			return 1;
		}

		// -- Read ApplicationId --
		char applicationId[64] = {};
		const DWORD applicationIdLength = GetPrivateProfileStringA(
			"Discord", "ApplicationId", "", applicationId, sizeof(applicationId),
			configurationPath.c_str());
		if (applicationIdLength == sizeof(applicationId) - 1
			|| !IsNumericApplicationId(applicationId))
		{
			MessageBoxA(nullptr,
				"Set Discord.ApplicationId in CodeChefPresence.ini to the numeric ID\n"
				"of a Discord application registered by its publisher.",
				"CodeChef Presence", MB_ICONERROR);
			return 1;
		}

		// -- Read LargeImageKey (optional) --
		char imageKey[64] = {};
		GetPrivateProfileStringA("Discord", "LargeImageKey", "codechef_logo",
			imageKey, sizeof(imageKey), configurationPath.c_str());

		// -- Read LargeImageText (optional) --
		char imageText[128] = {};
		GetPrivateProfileStringA("Discord", "LargeImageText", "CodeChef",
			imageText, sizeof(imageText), configurationPath.c_str());

		// -- Set up callbacks --
		DiscordIpcCallbacks callbacks = {};
		callbacks.onReady        = OnDiscordReady;
		callbacks.onDisconnected = OnDiscordDisconnected;
		callbacks.onError        = OnDiscordError;

		// -- Create IPC client --
		DiscordIpcClient client;
		client.Initialize(applicationId, &callbacks);

		bool wasCodeChefForeground = false;
		bool hasPublishedPresence  = false;
		int  reconnectTicks        = 0;

		while (InterlockedCompareExchange(&running, TRUE, TRUE) != FALSE)
		{
			// Auto-reconnect every ~5 s if disconnected
			if (!client.IsConnected())
			{
				++reconnectTicks;
				if (reconnectTicks >= 5)
				{
					reconnectTicks = 0;
					client.Initialize(applicationId, &callbacks);
				}
			}
			else
			{
				reconnectTicks = 0;
			}

			const bool isCodeChefForeground = IsCodeChefForeground();

			if (isCodeChefForeground != wasCodeChefForeground)
			{
				if (isCodeChefForeground && client.IsConnected())
				{
					UpdateCodeChefPresence(client, imageKey, imageText);
					hasPublishedPresence = true;
				}
				else if (hasPublishedPresence)
				{
					client.ClearActivity();
					hasPublishedPresence = false;
				}
				wasCodeChefForeground = isCodeChefForeground;
			}
			else if (isCodeChefForeground && !hasPublishedPresence && client.IsConnected())
			{
				// Re-publish after a reconnect
				UpdateCodeChefPresence(client, imageKey, imageText);
				hasPublishedPresence = true;
			}

			Sleep(1000);
		}

		if (hasPublishedPresence)
			client.ClearActivity();
		client.Shutdown();
		return 0;
	}
} // anonymous namespace

// ============================================================================
// DLL entry point
// ============================================================================
BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
	if (reason == DLL_PROCESS_ATTACH)
	{
		pluginModule = instance;
		DisableThreadLibraryCalls(instance);
	}
	else if (reason == DLL_PROCESS_DETACH)
	{
		InterlockedExchange(&running, FALSE);
	}

	return TRUE;
}

// Called by Ultimate ASI Loader after the host process is fully initialized.
extern "C" void InitializeASI()
{
	if (InterlockedCompareExchange(&initialized, TRUE, FALSE) != FALSE)
		return;

	HANDLE thread = CreateThread(nullptr, 0, RunPresence, nullptr, 0, nullptr);
	if (thread == nullptr)
	{
		InterlockedExchange(&initialized, FALSE);
		MessageBoxA(nullptr, "Could not start the CodeChef presence monitor.",
			"CodeChef Presence", MB_ICONERROR);
		return;
	}

	CloseHandle(thread);
}

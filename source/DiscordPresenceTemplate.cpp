// DiscordPresenceTemplate.cpp
// Universal Discord Rich Presence launcher — a standalone Win32 GUI.
// Monitors a user-selected foreground process and publishes Rich Presence via
// the Discord IPC Named Pipe protocol (no external SDK, no DLLs to ship).
//
// New in this version:
//   * Replaced deprecated discord-rpc.lib with direct IPC (discord_ipc.h/.cpp)
//   * Added Button 1 / Button 2 label + URL fields
//   * Auto-reconnect: re-initializes IPC when Discord restarts
//   * Expanded window to accommodate the additional controls

#include <windows.h>
#include <commdlg.h>

#include "discord/discord_ipc.h"

#include <cctype>
#include <ctime>
#include <string>
#include <vector>

#pragma comment(lib, "Comdlg32.lib")

namespace
{
	const wchar_t* const WindowClassName = L"DiscordPresenceTemplateWindow";
	const UINT StatusTimerId             = 1;
	const UINT StatusTimerIntervalMs     = 500;
	const UINT MaximumDiscordTextBytes   = 128;
	const UINT MaximumImageKeyBytes      = 32;
	const UINT MaximumButtonLabelBytes   = 32;
	const UINT MaximumButtonUrlBytes     = 512;
	const int  WindowWidth               = 700;
	const int  WindowHeight              = 820;

	// -------------------------------------------------------------------------
	// Control IDs
	// -------------------------------------------------------------------------
	enum ControlId
	{
		IdApplicationId  = 100,
		IdTargetProcess,
		IdDetails,
		IdState,
		IdLargeImageKey,
		IdLargeImageText,
		IdSmallImageKey,
		IdSmallImageText,
		IdButton1Label,
		IdButton1Url,
		IdButton2Label,
		IdButton2Url,
		IdBrowseProcess,
		IdStart,
		IdApply,
		IdStop,
		IdStatus
	};

	// -------------------------------------------------------------------------
	// Settings struct
	// -------------------------------------------------------------------------
	struct PresenceSettings
	{
		std::string applicationId;
		std::string targetProcess;
		std::string details;
		std::string state;
		std::string largeImageKey;
		std::string largeImageText;
		std::string smallImageKey;
		std::string smallImageText;
		std::string button1Label;
		std::string button1Url;
		std::string button2Label;
		std::string button2Url;
	};

	// -------------------------------------------------------------------------
	// Globals
	// -------------------------------------------------------------------------
	HINSTANCE applicationInstance    = nullptr;
	HWND      mainWindow             = nullptr;
	HWND      statusControl          = nullptr;
	HWND      applicationIdControl   = nullptr;
	HWND      targetProcessControl   = nullptr;
	HWND      detailsControl         = nullptr;
	HWND      stateControl           = nullptr;
	HWND      largeImageKeyControl   = nullptr;
	HWND      largeImageTextControl  = nullptr;
	HWND      smallImageKeyControl   = nullptr;
	HWND      smallImageTextControl  = nullptr;
	HWND      button1LabelControl    = nullptr;
	HWND      button1UrlControl      = nullptr;
	HWND      button2LabelControl    = nullptr;
	HWND      button2UrlControl      = nullptr;
	HWND      startButton            = nullptr;
	HWND      applyButton            = nullptr;
	HWND      stopButton             = nullptr;
	HFONT     uiFont                 = nullptr;
	HANDLE    monitorThread          = nullptr;
	volatile LONG monitorRunning     = FALSE;
	CRITICAL_SECTION sharedDataLock;
	PresenceSettings settings;
	std::wstring statusText          = L"Ready. Configure the application ID and optionally a target process.";
	std::wstring configurationPath;

	// -------------------------------------------------------------------------
	// String conversion helpers
	// -------------------------------------------------------------------------
	std::string WideToUtf8(const std::wstring& value)
	{
		if (value.empty())
			return std::string();

		const int required = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
			value.c_str(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
		if (required <= 0)
			return std::string();

		std::string result(required, '\0');
		if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.c_str(),
			static_cast<int>(value.size()), &result[0], required, nullptr, nullptr) != required)
			return std::string();

		return result;
	}

	std::wstring Utf8ToWide(const std::string& value)
	{
		if (value.empty())
			return std::wstring();

		const int required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
			value.c_str(), static_cast<int>(value.size()), nullptr, 0);
		if (required <= 0)
			return std::wstring();

		std::wstring result(required, L'\0');
		if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.c_str(),
			static_cast<int>(value.size()), &result[0], required) != required)
			return std::wstring();

		return result;
	}

	std::wstring GetControlText(HWND control)
	{
		const int length = GetWindowTextLengthW(control);
		if (length <= 0)
			return std::wstring();

		std::vector<wchar_t> buffer(static_cast<size_t>(length) + 1);
		const int copied = GetWindowTextW(control, &buffer[0], static_cast<int>(buffer.size()));
		return std::wstring(&buffer[0], copied);
	}

	void SetControlText(HWND control, const std::string& value)
	{
		SetWindowTextW(control, Utf8ToWide(value).c_str());
	}

	// -------------------------------------------------------------------------
	// Thread-safe status / settings accessors
	// -------------------------------------------------------------------------
	void SetStatus(const std::wstring& text)
	{
		EnterCriticalSection(&sharedDataLock);
		statusText = text;
		LeaveCriticalSection(&sharedDataLock);
	}

	std::wstring GetStatus()
	{
		EnterCriticalSection(&sharedDataLock);
		const std::wstring result = statusText;
		LeaveCriticalSection(&sharedDataLock);
		return result;
	}

	PresenceSettings GetSettings()
	{
		EnterCriticalSection(&sharedDataLock);
		const PresenceSettings result = settings;
		LeaveCriticalSection(&sharedDataLock);
		return result;
	}

	void SetSettings(const PresenceSettings& value)
	{
		EnterCriticalSection(&sharedDataLock);
		settings = value;
		LeaveCriticalSection(&sharedDataLock);
	}

	// -------------------------------------------------------------------------
	// Application directory
	// -------------------------------------------------------------------------
	std::wstring GetApplicationDirectory()
	{
		std::vector<wchar_t> buffer(32768);
		const DWORD length = GetModuleFileNameW(nullptr, &buffer[0], static_cast<DWORD>(buffer.size()));
		if (length == 0 || length >= buffer.size())
			return std::wstring();

		std::wstring path(&buffer[0], length);
		const std::wstring::size_type separator = path.find_last_of(L"\\/");
		path.resize(separator == std::wstring::npos ? 0 : separator + 1);
		return path;
	}

	// -------------------------------------------------------------------------
	// INI persistence
	// -------------------------------------------------------------------------
	std::string ReadIniValue(const wchar_t* key, const char* defaultValue)
	{
		std::vector<wchar_t> buffer(2048);
		const DWORD length = GetPrivateProfileStringW(L"Discord", key,
			Utf8ToWide(defaultValue).c_str(),
			&buffer[0], static_cast<DWORD>(buffer.size()), configurationPath.c_str());
		return WideToUtf8(std::wstring(&buffer[0], length));
	}

	void LoadSettings()
	{
		PresenceSettings loaded = {};
		loaded.applicationId  = ReadIniValue(L"ApplicationId",  "");
		loaded.targetProcess  = ReadIniValue(L"TargetProcess",  "");
		loaded.details        = ReadIniValue(L"Details",        "Using my app");
		loaded.state          = ReadIniValue(L"State",          "");
		loaded.largeImageKey  = ReadIniValue(L"LargeImageKey",  "");
		loaded.largeImageText = ReadIniValue(L"LargeImageText", "");
		loaded.smallImageKey  = ReadIniValue(L"SmallImageKey",  "");
		loaded.smallImageText = ReadIniValue(L"SmallImageText", "");
		loaded.button1Label   = ReadIniValue(L"Button1Label",   "");
		loaded.button1Url     = ReadIniValue(L"Button1Url",     "");
		loaded.button2Label   = ReadIniValue(L"Button2Label",   "");
		loaded.button2Url     = ReadIniValue(L"Button2Url",     "");
		SetSettings(loaded);

		SetControlText(applicationIdControl,  loaded.applicationId);
		SetControlText(targetProcessControl,  loaded.targetProcess);
		SetControlText(detailsControl,        loaded.details);
		SetControlText(stateControl,          loaded.state);
		SetControlText(largeImageKeyControl,  loaded.largeImageKey);
		SetControlText(largeImageTextControl, loaded.largeImageText);
		SetControlText(smallImageKeyControl,  loaded.smallImageKey);
		SetControlText(smallImageTextControl, loaded.smallImageText);
		SetControlText(button1LabelControl,   loaded.button1Label);
		SetControlText(button1UrlControl,     loaded.button1Url);
		SetControlText(button2LabelControl,   loaded.button2Label);
		SetControlText(button2UrlControl,     loaded.button2Url);
	}

	bool WriteIniValue(const wchar_t* key, const std::string& value)
	{
		return WritePrivateProfileStringW(L"Discord", key,
			Utf8ToWide(value).c_str(), configurationPath.c_str()) != FALSE;
	}

	bool SaveSettings(const PresenceSettings& value)
	{
		return WriteIniValue(L"ApplicationId",  value.applicationId)
			&& WriteIniValue(L"TargetProcess",  value.targetProcess)
			&& WriteIniValue(L"Details",        value.details)
			&& WriteIniValue(L"State",          value.state)
			&& WriteIniValue(L"LargeImageKey",  value.largeImageKey)
			&& WriteIniValue(L"LargeImageText", value.largeImageText)
			&& WriteIniValue(L"SmallImageKey",  value.smallImageKey)
			&& WriteIniValue(L"SmallImageText", value.smallImageText)
			&& WriteIniValue(L"Button1Label",   value.button1Label)
			&& WriteIniValue(L"Button1Url",     value.button1Url)
			&& WriteIniValue(L"Button2Label",   value.button2Label)
			&& WriteIniValue(L"Button2Url",     value.button2Url);
	}

	// -------------------------------------------------------------------------
	// Utility helpers
	// -------------------------------------------------------------------------
	std::wstring BaseName(const std::wstring& path)
	{
		const std::wstring::size_type sep = path.find_last_of(L"\\/");
		return sep == std::wstring::npos ? path : path.substr(sep + 1);
	}

	bool IsValidApplicationId(const std::string& value)
	{
		if (value.empty() || value.size() > 20)
			return false;
		for (const auto ch : value)
		{
			if (std::isdigit(static_cast<unsigned char>(ch)) == 0)
				return false;
		}
		return true;
	}

	bool FitsUtf8Bytes(const std::string& value, size_t maxBytes)
	{
		return value.size() <= maxBytes;
	}

	bool IsValidButtonUrl(const std::string& url)
	{
		if (url.empty()) return true; // empty is fine (button won't be shown)
		return url.rfind("https://", 0) == 0; // must start with https://
	}

	bool ValidateSettings(const PresenceSettings& value, std::wstring& error)
	{
		if (!IsValidApplicationId(value.applicationId))
		{
			error = L"Application ID must contain 1-20 digits. Create or select an application in the Discord Developer Portal.";
			return false;
		}

		if (!FitsUtf8Bytes(value.details, MaximumDiscordTextBytes)
			|| !FitsUtf8Bytes(value.state, MaximumDiscordTextBytes)
			|| !FitsUtf8Bytes(value.largeImageText, MaximumDiscordTextBytes)
			|| !FitsUtf8Bytes(value.smallImageText, MaximumDiscordTextBytes))
		{
			error = L"Activity text and image hover text must each be 128 UTF-8 bytes or fewer.";
			return false;
		}

		if (!FitsUtf8Bytes(value.largeImageKey, MaximumImageKeyBytes)
			|| !FitsUtf8Bytes(value.smallImageKey, MaximumImageKeyBytes))
		{
			error = L"Image asset keys must each be 32 UTF-8 bytes or fewer.";
			return false;
		}

		if (!FitsUtf8Bytes(value.button1Label, MaximumButtonLabelBytes)
			|| !FitsUtf8Bytes(value.button2Label, MaximumButtonLabelBytes))
		{
			error = L"Button labels must each be 32 UTF-8 bytes or fewer.";
			return false;
		}

		if (!FitsUtf8Bytes(value.button1Url, MaximumButtonUrlBytes)
			|| !FitsUtf8Bytes(value.button2Url, MaximumButtonUrlBytes))
		{
			error = L"Button URLs must each be 512 bytes or fewer.";
			return false;
		}

		// If a button label is set, a URL is required (and vice versa)
		if (!value.button1Label.empty() && value.button1Url.empty())
		{
			error = L"Button 1 URL is required when Button 1 Label is set.";
			return false;
		}
		if (!value.button1Url.empty() && value.button1Label.empty())
		{
			error = L"Button 1 Label is required when Button 1 URL is set.";
			return false;
		}
		if (!value.button2Label.empty() && value.button2Url.empty())
		{
			error = L"Button 2 URL is required when Button 2 Label is set.";
			return false;
		}
		if (!value.button2Url.empty() && value.button2Label.empty())
		{
			error = L"Button 2 Label is required when Button 2 URL is set.";
			return false;
		}

		if (!IsValidButtonUrl(value.button1Url))
		{
			error = L"Button 1 URL must start with https://.";
			return false;
		}
		if (!IsValidButtonUrl(value.button2Url))
		{
			error = L"Button 2 URL must start with https://.";
			return false;
		}

		if (!Utf8ToWide(value.targetProcess).empty()
			&& Utf8ToWide(value.targetProcess).find_first_of(L"\\/:|*?") != std::wstring::npos)
		{
			error = L"Enter the executable name only, for example game.exe, not a full path.";
			return false;
		}

		return true;
	}

	PresenceSettings ReadSettingsFromControls()
	{
		PresenceSettings value = {};
		value.applicationId  = WideToUtf8(GetControlText(applicationIdControl));
		value.targetProcess  = WideToUtf8(GetControlText(targetProcessControl));
		value.details        = WideToUtf8(GetControlText(detailsControl));
		value.state          = WideToUtf8(GetControlText(stateControl));
		value.largeImageKey  = WideToUtf8(GetControlText(largeImageKeyControl));
		value.largeImageText = WideToUtf8(GetControlText(largeImageTextControl));
		value.smallImageKey  = WideToUtf8(GetControlText(smallImageKeyControl));
		value.smallImageText = WideToUtf8(GetControlText(smallImageTextControl));
		value.button1Label   = WideToUtf8(GetControlText(button1LabelControl));
		value.button1Url     = WideToUtf8(GetControlText(button1UrlControl));
		value.button2Label   = WideToUtf8(GetControlText(button2LabelControl));
		value.button2Url     = WideToUtf8(GetControlText(button2UrlControl));
		return value;
	}

	void ShowError(const std::wstring& text)
	{
		MessageBoxW(mainWindow, text.c_str(), L"Discord Rich Presence", MB_OK | MB_ICONERROR);
	}

	// -------------------------------------------------------------------------
	// IPC callbacks (called from the reader thread — keep them thread-safe)
	// -------------------------------------------------------------------------
	void OnDiscordReady(const char* username)
	{
		wchar_t text[256] = {};
		swprintf_s(text, L"Connected to Discord%s%S. Waiting for the target app to be in the foreground.",
			username ? L" as " : L"",
			username ? username : "");
		SetStatus(text);
	}

	void OnDiscordDisconnected(int errorCode, const char* message)
	{
		wchar_t text[256] = {};
		swprintf_s(text, L"Discord disconnected (%d): %S", errorCode,
			message ? message : "unknown error");
		SetStatus(text);
	}

	void OnDiscordError(int errorCode, const char* message)
	{
		wchar_t text[256] = {};
		swprintf_s(text, L"Discord IPC error (%d): %S", errorCode,
			message ? message : "unknown error");
		SetStatus(text);
	}

	// -------------------------------------------------------------------------
	// Foreground process detection
	// -------------------------------------------------------------------------
	bool IsForegroundTarget(const std::string& targetProcess)
	{
		const HWND foregroundWindow = GetForegroundWindow();
		if (foregroundWindow == nullptr)
			return false;

		DWORD processId = 0;
		GetWindowThreadProcessId(foregroundWindow, &processId);
		if (processId == 0 || processId == GetCurrentProcessId())
			return false;

		HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
		if (process == nullptr)
			return false;

		std::vector<wchar_t> imagePath(32768);
		DWORD imagePathLength = static_cast<DWORD>(imagePath.size());
		const BOOL queried = QueryFullProcessImageNameW(process, 0, &imagePath[0], &imagePathLength);
		CloseHandle(process);
		if (!queried)
			return false;

		const std::wstring executableName = BaseName(std::wstring(&imagePath[0], imagePathLength));
		// Never match Discord itself
		if (_wcsicmp(executableName.c_str(), L"Discord.exe") == 0
			|| _wcsicmp(executableName.c_str(), L"DiscordPresenceTemplate.exe") == 0)
			return false;

		return targetProcess.empty()
			|| _wcsicmp(executableName.c_str(), Utf8ToWide(targetProcess).c_str()) == 0;
	}

	bool SamePresenceContent(const PresenceSettings& first, const PresenceSettings& second)
	{
		return first.details        == second.details
			&& first.state          == second.state
			&& first.largeImageKey  == second.largeImageKey
			&& first.largeImageText == second.largeImageText
			&& first.smallImageKey  == second.smallImageKey
			&& first.smallImageText == second.smallImageText
			&& first.button1Label   == second.button1Label
			&& first.button1Url     == second.button1Url
			&& first.button2Label   == second.button2Label
			&& first.button2Url     == second.button2Url;
	}

	// -------------------------------------------------------------------------
	// Monitor thread — runs the presence loop
	// -------------------------------------------------------------------------
	DWORD WINAPI MonitorPresence(void*)
	{
		DiscordIpcCallbacks callbacks = {};
		callbacks.onReady        = OnDiscordReady;
		callbacks.onDisconnected = OnDiscordDisconnected;
		callbacks.onError        = OnDiscordError;

		DiscordIpcClient client;
		PresenceSettings activeSettings = GetSettings();

		// Try to connect immediately
		bool connected = client.Initialize(activeSettings.applicationId.c_str(), &callbacks);
		if (connected)
			SetStatus(L"Started. Switch to the selected app to publish its activity.");
		else
			SetStatus(L"Started. Waiting for Discord to be running...");

		bool  wasForeground       = false;
		bool  hasPublishedPresence = false;
		int64_t startTimestamp    = 0;
		int   reconnectTicks      = 0;

		while (InterlockedCompareExchange(&monitorRunning, TRUE, TRUE) != FALSE)
		{
			PresenceSettings currentSettings = GetSettings();

			// Re-initialize when the application ID changes
			if (currentSettings.applicationId != activeSettings.applicationId)
			{
				client.Shutdown();
				hasPublishedPresence = false;
				wasForeground = false;
				connected = client.Initialize(currentSettings.applicationId.c_str(), &callbacks);
				SetStatus(connected
					? L"Discord application changed. Reconnecting..."
					: L"Discord application changed. Waiting for Discord...");
			}

			// Auto-reconnect (try every ~3 seconds = 6 ticks of 500 ms)
			if (!client.IsConnected())
			{
				++reconnectTicks;
				if (reconnectTicks >= 6)
				{
					reconnectTicks = 0;
					connected = client.Initialize(currentSettings.applicationId.c_str(), &callbacks);
					if (connected)
						SetStatus(L"Reconnected to Discord.");
				}
			}
			else
			{
				reconnectTicks = 0;
			}

			const bool isForeground = client.IsConnected() && IsForegroundTarget(currentSettings.targetProcess);

			if (isForeground)
			{
				if (!wasForeground)
					startTimestamp = static_cast<int64_t>(std::time(nullptr));

				if (!wasForeground || !hasPublishedPresence
					|| !SamePresenceContent(activeSettings, currentSettings))
				{
					DiscordActivity activity = {};
					activity.details        = currentSettings.details;
					activity.state          = currentSettings.state;
					activity.largeImageKey  = currentSettings.largeImageKey;
					activity.largeImageText = currentSettings.largeImageText;
					activity.smallImageKey  = currentSettings.smallImageKey;
					activity.smallImageText = currentSettings.smallImageText;
					activity.button1Label   = currentSettings.button1Label;
					activity.button1Url     = currentSettings.button1Url;
					activity.button2Label   = currentSettings.button2Label;
					activity.button2Url     = currentSettings.button2Url;
					activity.startTimestamp = startTimestamp;

					if (client.SetActivity(activity))
					{
						hasPublishedPresence = true;
						SetStatus(L"Presence active for the selected foreground app.");
					}
				}
			}
			else if (wasForeground || hasPublishedPresence)
			{
				client.ClearActivity();
				hasPublishedPresence = false;
				SetStatus(client.IsConnected()
					? L"Waiting for the selected app to be in the foreground."
					: L"Disconnected. Will reconnect automatically.");
			}

			wasForeground  = isForeground;
			activeSettings = currentSettings;

			Sleep(500);
		}

		if (hasPublishedPresence)
			client.ClearActivity();
		client.Shutdown();
		SetStatus(L"Stopped. Ready to start again.");
		return 0;
	}

	// -------------------------------------------------------------------------
	// Win32 control creation helpers
	// -------------------------------------------------------------------------
	HWND CreateChild(const wchar_t* className, const wchar_t* text, DWORD style,
		int x, int y, int width, int height, int identifier, HWND parent)
	{
		HWND control = CreateWindowExW(0, className, text, style,
			x, y, width, height, parent,
			reinterpret_cast<HMENU>(static_cast<INT_PTR>(identifier)),
			applicationInstance, nullptr);
		if (control != nullptr && uiFont != nullptr)
			SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(uiFont), TRUE);
		return control;
	}

	HWND CreateLabel(const wchar_t* text, int y)
	{
		return CreateChild(L"STATIC", text, WS_CHILD | WS_VISIBLE,
			24, y, 205, 22, 0, mainWindow);
	}

	HWND CreateEdit(const wchar_t* placeholder, int y, int identifier)
	{
		return CreateChild(L"EDIT", placeholder,
			WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL,
			235, y - 3, 435, 26, identifier, mainWindow);
	}

	void CreateControls()
	{
		uiFont = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
			DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
			CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

		// Description label
		CreateChild(L"STATIC",
			L"Configure a Discord application and the activity shown when your target app is in the foreground.",
			WS_CHILD | WS_VISIBLE, 24, 18, 650, 28, 0, mainWindow);

		// Row layout: label at x=24, edit at x=235
		int y = 58;
		const int rowStep = 39;

		CreateLabel(L"Discord Application ID *", y);
		applicationIdControl = CreateEdit(L"Numeric ID from the Discord Developer Portal", y, IdApplicationId);
		y += rowStep;

		CreateLabel(L"Target process (.exe)", y);
		targetProcessControl = CreateEdit(
			L"Optional, e.g. game.exe; blank = any foreground app", y, IdTargetProcess);
		y += rowStep;

		CreateLabel(L"Activity details", y);
		detailsControl = CreateEdit(L"First line of presence text", y, IdDetails);
		y += rowStep;

		CreateLabel(L"Activity state", y);
		stateControl = CreateEdit(L"Second line of presence text (optional)", y, IdState);
		y += rowStep;

		CreateLabel(L"Large image asset key", y);
		largeImageKeyControl = CreateEdit(
			L"Upload image in the Developer Portal first", y, IdLargeImageKey);
		y += rowStep;

		CreateLabel(L"Large image hover text", y);
		largeImageTextControl = CreateEdit(L"Tooltip on hover (optional)", y, IdLargeImageText);
		y += rowStep;

		CreateLabel(L"Small image asset key", y);
		smallImageKeyControl = CreateEdit(L"Optional small overlay image key", y, IdSmallImageKey);
		y += rowStep;

		CreateLabel(L"Small image hover text", y);
		smallImageTextControl = CreateEdit(L"Tooltip on hover (optional)", y, IdSmallImageText);
		y += rowStep;

		// --- Button rows ---
		CreateChild(L"STATIC", L"\u2500\u2500\u2500  Activity Buttons (optional)  \u2500\u2500\u2500",
			WS_CHILD | WS_VISIBLE, 24, y, 650, 18, 0, mainWindow);
		y += 22;

		CreateLabel(L"Button 1 label", y);
		button1LabelControl = CreateEdit(L"e.g. Visit Website (max 32 bytes)", y, IdButton1Label);
		y += rowStep;

		CreateLabel(L"Button 1 URL", y);
		button1UrlControl = CreateEdit(L"https://example.com", y, IdButton1Url);
		y += rowStep;

		CreateLabel(L"Button 2 label", y);
		button2LabelControl = CreateEdit(L"e.g. Join Server (max 32 bytes)", y, IdButton2Label);
		y += rowStep;

		CreateLabel(L"Button 2 URL", y);
		button2UrlControl = CreateEdit(L"https://example.com", y, IdButton2Url);
		y += rowStep;

		// Help text
		CreateChild(L"STATIC",
			L"Target is matched by executable name while its window is in front. "
			L"Image keys must already exist in the Discord application\u2019s Rich Presence assets. "
			L"Button URLs must start with https://.",
			WS_CHILD | WS_VISIBLE | SS_LEFT | SS_EDITCONTROL,
			24, y, 650, 42, 0, mainWindow);
		y += 50;

		// Action buttons
		CreateChild(L"BUTTON", L"Browse for app\u2026",
			WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
			24, y, 150, 32, IdBrowseProcess, mainWindow);

		startButton = CreateChild(L"BUTTON", L"Start presence",
			WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON | BS_DEFPUSHBUTTON,
			188, y, 145, 32, IdStart, mainWindow);

		applyButton = CreateChild(L"BUTTON", L"Save settings",
			WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
			345, y, 145, 32, IdApply, mainWindow);

		stopButton = CreateChild(L"BUTTON", L"Stop",
			WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
			502, y, 138, 32, IdStop, mainWindow);
		EnableWindow(stopButton, FALSE);
		y += 46;

		// Status label
		statusControl = CreateChild(L"STATIC", L"Ready.",
			WS_CHILD | WS_VISIBLE | SS_LEFT | SS_EDITCONTROL,
			24, y, 650, 54, IdStatus, mainWindow);

		LoadSettings();
	}

	// -------------------------------------------------------------------------
	// Settings apply / browse
	// -------------------------------------------------------------------------
	bool SaveAndApplySettings()
	{
		PresenceSettings value = ReadSettingsFromControls();
		std::wstring error;
		if (!ValidateSettings(value, error))
		{
			ShowError(error);
			return false;
		}

		if (!SaveSettings(value))
		{
			ShowError(L"Could not save the INI file beside this application. Check that the folder is writable.");
			return false;
		}

		SetSettings(value);
		return true;
	}

	void BrowseForProcess()
	{
		wchar_t filePath[32768] = {};
		OPENFILENAMEW dialog = {};
		dialog.lStructSize  = sizeof(dialog);
		dialog.hwndOwner    = mainWindow;
		dialog.lpstrFilter  = L"Applications (*.exe)\0*.exe\0All files (*.*)\0*.*\0\0";
		dialog.lpstrFile    = filePath;
		dialog.nMaxFile     = static_cast<DWORD>(sizeof(filePath) / sizeof(filePath[0]));
		dialog.Flags        = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
		dialog.lpstrTitle   = L"Select the app to watch";

		if (GetOpenFileNameW(&dialog))
			SetWindowTextW(targetProcessControl, BaseName(filePath).c_str());
	}

	// -------------------------------------------------------------------------
	// Monitor control
	// -------------------------------------------------------------------------
	void StopMonitor()
	{
		if (monitorThread == nullptr)
			return;

		InterlockedExchange(&monitorRunning, FALSE);
		WaitForSingleObject(monitorThread, INFINITE);
		CloseHandle(monitorThread);
		monitorThread = nullptr;
		EnableWindow(startButton, TRUE);
		EnableWindow(stopButton, FALSE);
	}

	void StartMonitor()
	{
		if (!SaveAndApplySettings())
			return;

		InterlockedExchange(&monitorRunning, TRUE);
		monitorThread = CreateThread(nullptr, 0, MonitorPresence, nullptr, 0, nullptr);
		if (monitorThread == nullptr)
		{
			InterlockedExchange(&monitorRunning, FALSE);
			ShowError(L"Could not start the presence monitor thread.");
			return;
		}

		EnableWindow(startButton, FALSE);
		EnableWindow(stopButton, TRUE);
	}

	// -------------------------------------------------------------------------
	// Window procedure
	// -------------------------------------------------------------------------
	LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
	{
		switch (message)
		{
		case WM_NCCREATE:
			mainWindow = window;
			return DefWindowProcW(window, message, wParam, lParam);

		case WM_CREATE:
			CreateControls();
			SetTimer(window, StatusTimerId, StatusTimerIntervalMs, nullptr);
			return 0;

		case WM_TIMER:
			if (wParam == StatusTimerId && statusControl != nullptr)
				SetWindowTextW(statusControl, GetStatus().c_str());
			return 0;

		case WM_COMMAND:
			switch (LOWORD(wParam))
			{
			case IdStart:
				StartMonitor();
				return 0;
			case IdApply:
				if (SaveAndApplySettings())
					SetStatus(L"Settings saved. Changes are active immediately.");
				return 0;
			case IdStop:
				StopMonitor();
				return 0;
			case IdBrowseProcess:
				BrowseForProcess();
				return 0;
			}
			break;

		case WM_CLOSE:
			StopMonitor();
			DestroyWindow(window);
			return 0;

		case WM_DESTROY:
			KillTimer(window, StatusTimerId);
			if (uiFont != nullptr)
			{
				DeleteObject(uiFont);
				uiFont = nullptr;
			}
			PostQuitMessage(0);
			return 0;
		}

		return DefWindowProcW(window, message, wParam, lParam);
	}
} // anonymous namespace

// ============================================================================
// Entry point
// ============================================================================
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand)
{
	applicationInstance = instance;
	InitializeCriticalSection(&sharedDataLock);

	const std::wstring applicationDirectory = GetApplicationDirectory();
	if (applicationDirectory.empty())
	{
		MessageBoxW(nullptr, L"Could not determine the application directory.",
			L"Discord Rich Presence", MB_OK | MB_ICONERROR);
		DeleteCriticalSection(&sharedDataLock);
		return 1;
	}
	configurationPath = applicationDirectory + L"DiscordPresenceTemplate.ini";

	// Register window class
	WNDCLASSEXW windowClass     = {};
	windowClass.cbSize          = sizeof(windowClass);
	windowClass.hInstance       = instance;
	windowClass.lpfnWndProc     = WindowProcedure;
	windowClass.lpszClassName   = WindowClassName;
	windowClass.hCursor         = LoadCursorW(nullptr, IDC_ARROW);
	windowClass.hIcon           = LoadIconW(nullptr, IDI_APPLICATION);
	windowClass.hbrBackground   = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
	if (RegisterClassExW(&windowClass) == 0)
	{
		MessageBoxW(nullptr, L"Could not register the application window.",
			L"Discord Rich Presence", MB_OK | MB_ICONERROR);
		DeleteCriticalSection(&sharedDataLock);
		return 1;
	}

	RECT windowRect = { 0, 0, WindowWidth, WindowHeight };
	AdjustWindowRect(&windowRect, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);
	mainWindow = CreateWindowExW(0, WindowClassName, L"Discord Rich Presence",
		WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
		CW_USEDEFAULT, CW_USEDEFAULT,
		windowRect.right - windowRect.left,
		windowRect.bottom - windowRect.top,
		nullptr, nullptr, instance, nullptr);
	if (mainWindow == nullptr)
	{
		MessageBoxW(nullptr, L"Could not create the application window.",
			L"Discord Rich Presence", MB_OK | MB_ICONERROR);
		DeleteCriticalSection(&sharedDataLock);
		return 1;
	}

	ShowWindow(mainWindow, showCommand);
	UpdateWindow(mainWindow);

	MSG message = {};
	while (GetMessageW(&message, nullptr, 0, 0) > 0)
	{
		TranslateMessage(&message);
		DispatchMessageW(&message);
	}

	DeleteCriticalSection(&sharedDataLock);
	return static_cast<int>(message.wParam);
}

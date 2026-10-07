// Settings (INI in %APPDATA%\Lull) and theme colors.
#include "app.h"
#include <shlobj.h>

Settings g_settings;

static std::wstring SettingsPath()
{
    PWSTR appdata = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appdata)))
    {
        dir = std::wstring(appdata) + L"\\Lull";
        CoTaskMemFree(appdata);
    }
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\settings.ini";
}

static int ReadInt(const wchar_t* key, int def, const std::wstring& path)
{
    return (int)GetPrivateProfileIntW(L"Lull", key, def, path.c_str());
}

static void WriteInt(const wchar_t* key, int v, const std::wstring& path)
{
    WritePrivateProfileStringW(L"Lull", key, std::to_wstring(v).c_str(), path.c_str());
}

void LoadSettings()
{
    auto path = SettingsPath();
    Settings& s = g_settings;
    s.minutes = ReadInt(L"Minutes", s.minutes, path);
    s.scene = ReadInt(L"Scene", s.scene, path);
    s.shuffle = ReadInt(L"Shuffle", s.shuffle, path) != 0;
    s.showCountdown = ReadInt(L"ShowCountdown", s.showCountdown, path) != 0;
    s.reminders = ReadInt(L"Reminders", s.reminders, path) != 0;
    s.chime = ReadInt(L"Chime", s.chime, path) != 0;
    s.earlyExit = ReadInt(L"EarlyExit", s.earlyExit, path) != 0;
    s.trayPromoted = ReadInt(L"TrayPromoted", s.trayPromoted, path) != 0;
    s.muteSounds = ReadInt(L"MuteSounds", s.muteSounds, path) != 0;
    s.gridSources = std::clamp(ReadInt(L"GridSources", s.gridSources, path), 1, 8);
    s.gridIntensity = std::clamp(ReadInt(L"GridIntensity", s.gridIntensity, path), 0, 100);
    s.gridPace = std::clamp(ReadInt(L"GridPace", s.gridPace, path), 0, 100);
    s.gridSurges = std::clamp(ReadInt(L"GridSurges", s.gridSurges, path), 0, 100);
    s.gridLook = std::clamp(ReadInt(L"GridLook", s.gridLook, path), 0, 2);
    {
        wchar_t buf[4096] = L"";
        GetPrivateProfileStringW(L"Lull", L"MutedDevices", L"", buf, 4096, path.c_str());
        s.mutedDevices = buf;
    }
    s.riddleSeed = (unsigned)ReadInt(L"RiddleSeed", (int)s.riddleSeed, path);
    s.riddleCursor = ReadInt(L"RiddleCursor", s.riddleCursor, path);
    if (s.minutes < 1) s.minutes = 1;
    if (s.minutes > 15) s.minutes = 15;
    if (s.scene < 0 || s.scene >= kSceneCount) s.scene = 0;
}

void SaveSettings()
{
    auto path = SettingsPath();
    const Settings& s = g_settings;
    WriteInt(L"Minutes", s.minutes, path);
    WriteInt(L"Scene", s.scene, path);
    WriteInt(L"Shuffle", s.shuffle, path);
    WriteInt(L"ShowCountdown", s.showCountdown, path);
    WriteInt(L"Reminders", s.reminders, path);
    WriteInt(L"Chime", s.chime, path);
    WriteInt(L"EarlyExit", s.earlyExit, path);
    WriteInt(L"TrayPromoted", s.trayPromoted, path);
    WriteInt(L"MuteSounds", s.muteSounds, path);
    WriteInt(L"GridSources", s.gridSources, path);
    WriteInt(L"GridIntensity", s.gridIntensity, path);
    WriteInt(L"GridPace", s.gridPace, path);
    WriteInt(L"GridSurges", s.gridSurges, path);
    WriteInt(L"GridLook", s.gridLook, path);
    WritePrivateProfileStringW(L"Lull", L"MutedDevices", s.mutedDevices.c_str(), path.c_str());
    WriteInt(L"RiddleSeed", (int)s.riddleSeed, path);
    WriteInt(L"RiddleCursor", s.riddleCursor, path);
}

// ------------------------------------------------------------------ autostart

static const wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

bool GetStartWithWindows()
{
    wchar_t buf[MAX_PATH * 2];
    DWORD size = sizeof(buf);
    return RegGetValueW(HKEY_CURRENT_USER, kRunKey, kAppName, RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS;
}

void SetStartWithWindows(bool on)
{
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) return;
    if (on)
    {
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring cmd = L"\"" + std::wstring(exe) + L"\"";
        RegSetValueExW(key, kAppName, 0, REG_SZ, (const BYTE*)cmd.c_str(), (DWORD)((cmd.size() + 1) * sizeof(wchar_t)));
    }
    else
    {
        RegDeleteValueW(key, kAppName);
    }
    RegCloseKey(key);
}

// ------------------------------------------------------------------ theme

static DWORD ReadPersonalize(const wchar_t* name, DWORD def)
{
    DWORD v = def, size = sizeof(v);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 name, RRF_RT_REG_DWORD, nullptr, &v, &size);
    return v;
}

bool TaskbarIsLight() { return ReadPersonalize(L"SystemUsesLightTheme", 0) != 0; }

bool AppsUseLightTheme()
{
    static ULONGLONG checked = 0;
    static bool light = false;
    ULONGLONG now = GetTickCount64();
    if (checked == 0 || now - checked > 2000)
    {
        light = ReadPersonalize(L"AppsUseLightTheme", 1) != 0;
        checked = now;
    }
    return light;
}

// Tray flyouts follow the taskbar ("system") theme, like the built-in ones.
Theme CurrentTheme() { return MakeTheme(!TaskbarIsLight()); }

Theme MakeTheme(bool dark)
{
    Theme t{};
    t.dark = dark;
    if (!t.dark)
    {
        t.card = Rgba(0xFAFAFC);
        t.cardBorder = Rgba(0x000000, 0.08f);
        t.text = Rgba(0x1B1B22);
        t.text2 = Rgba(0x5C5C6B);
        t.text3 = Rgba(0x8E8E9C);
        t.track = Rgba(0x1B1B33, 0.10f);
        t.hover = Rgba(0x1B1B33, 0.05f);
        t.pressed = Rgba(0x1B1B33, 0.09f);
        t.accent = Rgba(0x5E5CE6);
        t.accentHover = Rgba(0x6C6AEE);
        t.accentPressed = Rgba(0x5250D2);
        t.onAccent = Rgba(0xFFFFFF);
        t.toggleOff = Rgba(0x6B6B7A);
    }
    else
    {
        t.card = Rgba(0x24242A);
        t.cardBorder = Rgba(0xFFFFFF, 0.09f);
        t.text = Rgba(0xF3F3F7);
        t.text2 = Rgba(0xB0B0BE);
        t.text3 = Rgba(0x7E7E8C);
        t.track = Rgba(0xFFFFFF, 0.13f);
        t.hover = Rgba(0xFFFFFF, 0.06f);
        t.pressed = Rgba(0xFFFFFF, 0.10f);
        t.accent = Rgba(0x8B89FF);
        t.accentHover = Rgba(0x9C9AFF);
        t.accentPressed = Rgba(0x7A78F0);
        t.onAccent = Rgba(0x15142B);
        t.toggleOff = Rgba(0xA0A0AE);
    }
    return t;
}

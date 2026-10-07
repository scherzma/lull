// Lull â€” a quiet pause. Tray app entry point.
#include "app.h"
#include <shellapi.h>
#include <shlobj.h>
#include <windowsx.h>
#include <random>

HWND g_mainWnd = nullptr;
HINSTANCE g_hinst = nullptr;

int DevRenderScenes(const wchar_t* dir, int w, int h, float t);
int DevHookSelfTest(const wchar_t* outPath);
int DevSurgeTest(const wchar_t* outPath);
int DevSurgeFrames(const wchar_t* dir);

namespace
{
    const wchar_t kMainClass[] = L"Lull.Main";
    const wchar_t kMutexName[] = L"Local\\Lull.SingleInstance.5b0e2c";
    const UINT kTrayId = 1;
    const UINT_PTR kTimerIdle = 1, kTimerPromote = 2;

    UINT g_taskbarCreated = 0;
    HICON g_trayIcon = nullptr;
    bool g_devMode = false;
    int g_lastScene = -1;
    double g_lastGfxUse = 0;
    int g_promoteTries = 0;

    enum : UINT
    {
        ID_OPEN = 100, ID_COUNTDOWN, ID_REMINDERS, ID_CHIME, ID_EARLY, ID_STARTUP, ID_QUIT, ID_SHUFFLE, ID_PIN, ID_MUTE,
        ID_BEGIN = 200,   // + minutes
        ID_SCENE = 300,   // + scene index
    };

    // ------------------------------------------------------------ tray icon

    int TrayIconSize()
    {
        HWND taskbar = FindWindowW(L"Shell_TrayWnd", nullptr);
        UINT dpi = taskbar ? GetDpiForWindow(taskbar) : GetDpiForSystem();
        return GetSystemMetricsForDpi(SM_CXSMICON, dpi);
    }

    void UpdateTray(DWORD action)
    {
        HICON old = g_trayIcon;
        g_trayIcon = CreateTrayIcon(TrayIconSize(), TaskbarIsLight());
        NOTIFYICONDATAW nid{ sizeof(nid) };
        nid.hWnd = g_mainWnd;
        nid.uID = kTrayId;
        nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE | NIF_SHOWTIP;
        nid.uCallbackMessage = WM_APP_TRAY;
        nid.hIcon = g_trayIcon;
        wcscpy_s(nid.szTip, L"Lull \x2014 take a quiet pause");
        if (action == NIM_ADD)
        {
            if (!Shell_NotifyIconW(NIM_ADD, &nid)) Shell_NotifyIconW(NIM_MODIFY, &nid);
            nid.uVersion = NOTIFYICON_VERSION_4;
            Shell_NotifyIconW(NIM_SETVERSION, &nid);
        }
        else
        {
            Shell_NotifyIconW(NIM_MODIFY, &nid);
        }
        if (old) DestroyIcon(old);
    }

    void RemoveTray()
    {
        NOTIFYICONDATAW nid{ sizeof(nid) };
        nid.hWnd = g_mainWnd;
        nid.uID = kTrayId;
        Shell_NotifyIconW(NIM_DELETE, &nid);
    }

    POINT TrayAnchor()
    {
        NOTIFYICONIDENTIFIER id{ sizeof(id) };
        id.hWnd = g_mainWnd;
        id.uID = kTrayId;
        RECT r;
        if (SUCCEEDED(Shell_NotifyIconGetRect(&id, &r)) && r.right > r.left)
            return { (r.left + r.right) / 2, (r.top + r.bottom) / 2 };
        MONITORINFO mi{ sizeof(mi) };
        GetMonitorInfoW(MonitorFromPoint({ 0, 0 }, MONITOR_DEFAULTTOPRIMARY), &mi);
        return { mi.rcWork.right - 40, mi.rcWork.bottom };
    }

    // One-time hint: where the icon lives and how to keep it next to the clock.
    void ShowWelcomeHint()
    {
        NOTIFYICONDATAW nid{ sizeof(nid) };
        nid.hWnd = g_mainWnd;
        nid.uID = kTrayId;
        nid.uFlags = NIF_INFO;
        nid.dwInfoFlags = NIIF_USER | NIIF_LARGE_ICON | NIIF_NOSOUND;
        nid.hBalloonIcon = (HICON)LoadImageW(g_hinst, MAKEINTRESOURCEW(1), IMAGE_ICON, 64, 64, 0);
        wcscpy_s(nid.szInfoTitle, L"Lull is ready");
        wcscpy_s(nid.szInfo, L"Find it under the ^ arrow. Drag it next to the clock to keep it one click away.");
        Shell_NotifyIconW(NIM_MODIFY, &nid);
    }

    // Windows 11 keeps new tray icons in the overflow (^) menu. Mark ours "always show" once,
    // the same switch as Settings > Personalization > Taskbar > Other system tray icons.
    // Explorer picks this up the next time it starts (e.g. at sign-in). After that we never
    // touch it again, so the user's own choice wins.
    std::wstring ResolveShellPath(const std::wstring& p)
    {
        if (p.size() > 39 && p[0] == L'{' && p[37] == L'}')
        {
            GUID g;
            if (SUCCEEDED(CLSIDFromString(p.substr(0, 38).c_str(), &g)))
            {
                PWSTR base = nullptr;
                if (SUCCEEDED(SHGetKnownFolderPath(g, 0, nullptr, &base)))
                {
                    std::wstring r = std::wstring(base) + p.substr(38);
                    CoTaskMemFree(base);
                    return r;
                }
            }
        }
        return p;
    }

    bool PromoteTrayIcon()
    {
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        HKEY root;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Control Panel\\NotifyIconSettings", 0, KEY_READ, &root) != ERROR_SUCCESS)
            return true; // not Windows 11 style tray; nothing to do
        bool found = false;
        wchar_t name[256];
        for (DWORD i = 0;; i++)
        {
            DWORD len = 256;
            if (RegEnumKeyExW(root, i, name, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
            wchar_t path[1024];
            DWORD size = sizeof(path);
            if (RegGetValueW(root, name, L"ExecutablePath", RRF_RT_REG_SZ, nullptr, path, &size) != ERROR_SUCCESS) continue;
            if (_wcsicmp(ResolveShellPath(path).c_str(), exe) != 0) continue;
            HKEY k;
            if (RegOpenKeyExW(root, name, 0, KEY_SET_VALUE, &k) == ERROR_SUCCESS)
            {
                DWORD one = 1;
                RegSetValueExW(k, L"IsPromoted", 0, REG_DWORD, (const BYTE*)&one, sizeof(one));
                RegCloseKey(k);
                found = true;
            }
        }
        RegCloseKey(root);
        return found;
    }

    // ------------------------------------------------------------ menus

    void EnableDarkMenus()
    {
        HMODULE ux = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!ux) return;
        using SetModeFn = int(WINAPI*)(int);
        using FlushFn = void(WINAPI*)();
        auto setMode = (SetModeFn)GetProcAddress(ux, MAKEINTRESOURCEA(135));
        auto flush = (FlushFn)GetProcAddress(ux, MAKEINTRESOURCEA(136));
        if (setMode) setMode(1); // AllowDark: follow the system setting
        if (flush) flush();
    }

    void HandleCommand(UINT cmd)
    {
        auto& s = g_settings;
        if (cmd >= ID_BEGIN && cmd < ID_BEGIN + 100) { BeginLull((cmd - ID_BEGIN) * 60); return; }
        if (cmd >= ID_SCENE && cmd < ID_SCENE + kSceneCount) { s.scene = cmd - ID_SCENE; s.shuffle = false; SaveSettings(); return; }
        switch (cmd)
        {
        case ID_OPEN: flyout::Show(TrayAnchor()); break;
        case ID_SHUFFLE: s.shuffle = true; break;
        case ID_COUNTDOWN: s.showCountdown = !s.showCountdown; break;
        case ID_REMINDERS: s.reminders = !s.reminders; break;
        case ID_CHIME: s.chime = !s.chime; if (s.chime) PlayChime(); break;
        case ID_EARLY: s.earlyExit = !s.earlyExit; break;
        case ID_MUTE: s.muteSounds = !s.muteSounds; break;
        case ID_STARTUP: SetStartWithWindows(!GetStartWithWindows()); return;
        case ID_PIN: ShellExecuteW(nullptr, L"open", L"ms-settings:taskbar", nullptr, nullptr, SW_SHOWNORMAL); return;
        case ID_QUIT: DestroyWindow(g_mainWnd); return;
        default: return;
        }
        SaveSettings();
    }

    // ------------------------------------------------------------ commands from a second instance

    void RunCommand(const std::wstring& cmd)
    {
        if (cmd.rfind(L"start:", 0) == 0) BeginLull(std::max(5, _wtoi(cmd.c_str() + 6)));
        else flyout::Show(TrayAnchor());
    }

    LRESULT CALLBACK MainProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
    {
        if (msg == g_taskbarCreated && g_taskbarCreated)
        {
            UpdateTray(NIM_ADD);
            return 0;
        }
        switch (msg)
        {
        case WM_APP_TRAY:
            switch (LOWORD(lp))
            {
            case NIN_SELECT:
            case NIN_KEYSELECT:
                flyout::Toggle({ GET_X_LPARAM(wp), GET_Y_LPARAM(wp) });
                return 0;
            case WM_CONTEXTMENU:
                flyout::Hide(true);
                ShowTrayMenu(h, { GET_X_LPARAM(wp), GET_Y_LPARAM(wp) });
                return 0;
            }
            return 0;
        case WM_APP_BEGIN:
            BeginLull((int)wp);
            return 0;
        case WM_COPYDATA:
        {
            auto* cds = (COPYDATASTRUCT*)lp;
            if (cds->dwData == 0x4C554C4C && cds->lpData)
                RunCommand(std::wstring((const wchar_t*)cds->lpData, cds->cbData / sizeof(wchar_t)));
            return TRUE;
        }
        case WM_TIMER:
            if (wp == kTimerIdle)
            {
                // Free the GPU device when nothing has been on screen for a while.
                if (!flyout::Visible() && !overlay::Active() && gfx::Ready() && gfx::Now() - g_lastGfxUse > 45)
                {
                    gfx::Log(L"idle: releasing graphics");
                    flyout::ReleaseGraphics();
                    overlay::ReleaseGraphics();
                    gfx::Shutdown();
                }
            }
            else if (wp == kTimerPromote)
            {
                // Explorer creates our settings entry shortly after the icon appears.
                if (PromoteTrayIcon() || ++g_promoteTries > 20)
                {
                    KillTimer(h, kTimerPromote);
                    g_settings.trayPromoted = true;
                    SaveSettings();
                    ShowWelcomeHint();
                }
            }
            return 0;
        case WM_SETTINGCHANGE:
            if (lp && wcscmp((const wchar_t*)lp, L"ImmersiveColorSet") == 0) UpdateTray(NIM_MODIFY);
            return 0;
        case WM_DISPLAYCHANGE:
        case WM_DPICHANGED:
            UpdateTray(NIM_MODIFY);
            return 0;
        case WM_ENDSESSION:
            if (wp) audio::RestoreAll();
            return 0;
        case WM_DESTROY:
            audio::RestoreAll();
            RemoveTray();
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(h, msg, wp, lp);
    }

    bool SendToRunningInstance(const std::wstring& cmd)
    {
        HWND other = FindWindowW(kMainClass, nullptr);
        if (!other) return false;
        COPYDATASTRUCT cds{ 0x4C554C4C, (DWORD)(cmd.size() * sizeof(wchar_t)), (void*)cmd.c_str() };
        DWORD pid = 0;
        GetWindowThreadProcessId(other, &pid);
        AllowSetForegroundWindow(pid);
        SendMessageW(other, WM_COPYDATA, 0, (LPARAM)&cds);
        return true;
    }
}

void ShowTrayMenu(HWND owner, POINT pt)
{
    const auto& s = g_settings;
    auto flag = [](bool on) { return on ? MF_CHECKED : MF_UNCHECKED; };
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, ID_OPEN, L"Open Lull");
    SetMenuDefaultItem(m, ID_OPEN, FALSE);

    HMENU begin = CreatePopupMenu();
    for (int min : { 1, 2, 3, 5, 10, 15 })
    {
        wchar_t label[32];
        swprintf_s(label, min == 1 ? L"%d minute" : L"%d minutes", min);
        AppendMenuW(begin, MF_STRING | (min == s.minutes ? MF_DEFAULT : 0), ID_BEGIN + min, label);
    }
    AppendMenuW(m, MF_POPUP, (UINT_PTR)begin, L"Begin a lull");

    HMENU scenes = CreatePopupMenu();
    for (int i = 0; i < kSceneCount; i++) AppendMenuW(scenes, MF_STRING, ID_SCENE + i, kScenes[i].name);
    AppendMenuW(scenes, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(scenes, MF_STRING, ID_SHUFFLE, L"Shuffle");
    CheckMenuRadioItem(scenes, ID_SCENE, ID_SHUFFLE, s.shuffle ? ID_SHUFFLE : ID_SCENE + s.scene, MF_BYCOMMAND);
    AppendMenuW(m, MF_POPUP, (UINT_PTR)scenes, L"Scene");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | flag(s.showCountdown), ID_COUNTDOWN, L"Show countdown");
    AppendMenuW(m, MF_STRING | flag(s.reminders), ID_REMINDERS, L"Gentle reminders");
    AppendMenuW(m, MF_STRING | flag(s.chime), ID_CHIME, L"Chime when finished");
    AppendMenuW(m, MF_STRING | flag(s.muteSounds), ID_MUTE, L"Mute other sounds during a lull");
    AppendMenuW(m, MF_STRING | flag(s.earlyExit), ID_EARLY, L"Allow early exit (math challenge)");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | flag(GetStartWithWindows()), ID_STARTUP, L"Start with Windows");
    AppendMenuW(m, MF_STRING, ID_PIN, L"Pin icon next to the clock\x2026");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, ID_QUIT, L"Quit Lull");

    UINT align;
    if (owner == g_mainWnd)
    {
        MONITORINFO mi{ sizeof(mi) };
        GetMonitorInfoW(MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST), &mi);
        bool lowerHalf = pt.y > (mi.rcMonitor.top + mi.rcMonitor.bottom) / 2;
        align = (lowerHalf ? TPM_BOTTOMALIGN : TPM_TOPALIGN) |
                (GetSystemMetrics(SM_MENUDROPALIGNMENT) ? TPM_RIGHTALIGN : TPM_LEFTALIGN);
    }
    else
    {
        align = TPM_TOPALIGN | TPM_RIGHTALIGN; // from the flyout's "more" button
    }
    SetForegroundWindow(owner);
    UINT cmd = (UINT)TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY | align, pt.x, pt.y, owner, nullptr);
    PostMessageW(owner, WM_NULL, 0, 0);
    DestroyMenu(m);
    if (cmd) HandleCommand(cmd);
}

void BeginLull(int seconds)
{
    if (overlay::Active()) return;
    flyout::Hide(true);
    int scene = g_settings.scene;
    if (g_settings.shuffle)
    {
        static std::mt19937 rng{ (unsigned)GetTickCount64() };
        do scene = (int)(rng() % kSceneCount);
        while (scene == g_lastScene && kSceneCount > 1);
    }
    g_lastScene = scene;
    gfx::NewGridEnvironment(); // every lull starts from a fresh random layout (the screen fades in, so no jump shows)
    overlay::Begin(seconds, scene, g_devMode);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int)
{
    g_hinst = inst;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::wstring command = L"open";
    bool hasCommand = false;
    int startSeconds = 0;
    bool devDay = false, devNight = false; // --day / --night: dev renders use that Grid look
    for (int i = 1; i < argc; i++)
    {
        std::wstring a = argv[i];
        auto next = [&](const wchar_t* def) { return i + 1 < argc ? std::wstring(argv[++i]) : std::wstring(def); };
        if (a == L"--day") { devDay = true; continue; }
        if (a == L"--night") { devNight = true; continue; }
        if (a == L"--render-scenes")
        {
            std::wstring dir = next(L".");
            float t = i + 1 < argc ? (float)_wtof(argv[++i]) : 0.f;
            if (devDay) gfx::SetGridDay(1.f);
            return DevRenderScenes(dir.c_str(), 1600, 900, t);
        }
        if (a == L"--snap-flyout")
        {
            std::wstring path = next(L"flyout.png");
            bool tune = i + 1 < argc && wcscmp(argv[i + 1], L"tune") == 0;
            LoadSettings();
            if (devDay || devNight) { g_settings.gridLook = devDay ? 1 : 0; g_settings.scene = kSceneGrid; g_settings.shuffle = false; }
            return flyout::SnapshotPng(path.c_str(), tune) ? 0 : 1;
        }
        if (a == L"--export-icon") return ExportIco(next(L"lull.ico").c_str()) ? 0 : 1;
        if (a == L"--ink-test")
        {
            LoadSettings();
            g_settings.gridLook = devDay ? 1 : 0;
            return overlay::DevInkTest(next(L"ink.txt").c_str()) ? 0 : 1;
        }
        if (a == L"--snap-overlay" || a == L"--snap-text")
        {
            LoadSettings();
            g_settings.gridLook = devDay ? 1 : 0;
            g_settings.reminders = g_settings.showCountdown = true;
            return overlay::SnapshotScenes(next(L".").c_str(), a == L"--snap-text") ? 0 : 1;
        }
        if (a == L"--selftest-hook") return DevHookSelfTest(next(L"hooktest.txt").c_str());
        if (a == L"--surge-test") return DevSurgeTest(next(L"surges.txt").c_str());
        if (a == L"--surge-frames") return DevSurgeFrames(next(L".").c_str());
        if (a == L"--dev") g_devMode = true;
        else if (a == L"--start") { int m = _wtoi(next(L"5").c_str()); startSeconds = std::clamp(m, 1, 60) * 60; }
        else if (a == L"--start-seconds") startSeconds = std::max(5, _wtoi(next(L"10").c_str()));
    }
    LocalFree(argv);
    if (startSeconds) { command = L"start:" + std::to_wstring(startSeconds); hasCommand = true; }

    HANDLE mutex = CreateMutexW(nullptr, TRUE, kMutexName);
    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        SendToRunningInstance(command);
        return 0;
    }

    LoadSettings();
    gfx::NewGridEnvironment();
    if (!g_settings.mutedDevices.empty()) audio::RestoreAll(); // a previous run ended mid-lull
    EnableDarkMenus();

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc = MainProc;
    wc.hInstance = inst;
    wc.lpszClassName = kMainClass;
    RegisterClassExW(&wc);
    g_mainWnd = CreateWindowExW(WS_EX_TOOLWINDOW, kMainClass, L"Lull", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, inst, nullptr);
    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    ChangeWindowMessageFilterEx(g_mainWnd, g_taskbarCreated, MSGFLT_ALLOW, nullptr);
    ChangeWindowMessageFilterEx(g_mainWnd, WM_COPYDATA, MSGFLT_ALLOW, nullptr);

    UpdateTray(NIM_ADD);
    SetTimer(g_mainWnd, kTimerIdle, 15000, nullptr);
    if (!g_settings.trayPromoted) SetTimer(g_mainWnd, kTimerPromote, 500, nullptr);
    if (hasCommand) PostMessageW(g_mainWnd, WM_APP_BEGIN, startSeconds, 0);

    MSG msg;
    for (;;)
    {
        if (overlay::Active() || flyout::Visible())
        {
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
            {
                if (msg.message == WM_QUIT) goto done;
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            double t0 = gfx::Now();
            if (overlay::Active()) overlay::Frame();
            else if (flyout::Visible()) flyout::Frame();
            g_lastGfxUse = gfx::Now();
            if (g_lastGfxUse - t0 < 0.002) Sleep(4); // never spin if nothing paced the frame
        }
        else
        {
            BOOL r = GetMessageW(&msg, nullptr, 0, 0);
            if (r <= 0) break;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
done:
    flyout::ReleaseGraphics();
    gfx::Shutdown();
    if (g_trayIcon) DestroyIcon(g_trayIcon);
    if (mutex) { ReleaseMutex(mutex); CloseHandle(mutex); }
    CoUninitialize();
    return 0;
}

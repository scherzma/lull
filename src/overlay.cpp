// The lull itself: one topmost window per monitor, keyboard shield, countdown, prompts.
#include "app.h"
#include <dwmapi.h>
#include <d2d1effects.h>
#include <shellscalingapi.h>
#include <atomic>
#include <vector>
#include <random>
#include <cmath>
#include <DirectXPackedVector.h>

namespace
{
    // Text on the scene (prompt, countdown, hint, breathing cue) takes its colour from what is
    // behind it; see ProbeStart/ReadInk.
    enum { InkPrompt, InkCount, InkHint, InkCue, kInkRegions };
    struct InkStyle
    {
        D2D1_COLOR_F ink{ 1, 1, 1, 1 }, glow{ 0.02f, 0.03f, 0.07f, 1 };
        float glowA = 0.6f;   // soft glow hugging the letters
        float spot = 0.f;     // soft shade behind the whole line, only where contrast needs it
        float plate = 0.f;    // how much the scene right behind the line is blurred (busy patterns)
        bool light = true;    // light ink (dark scene) or dark ink (light scene)
    };
    // Where each text sits, as fractions of the screen (u0, v0, u1, v1).
    const float kInkRects[kInkRegions][4] = {
        { 0.24f, 0.46f, 0.76f, 0.54f },   // prompt
        { 0.44f, 0.845f, 0.56f, 0.915f }, // countdown and its bar
        { 0.32f, 0.94f, 0.68f, 0.97f },   // start hint
        { 0.38f, 0.775f, 0.62f, 0.815f }, // breathing cue
    };

    const wchar_t kClass[] = L"Lull.Overlay";
    const double kFadeIn = 1.4, kFadeOut = 1.6;
    const int kChallengeSteps = 3;          // correct answers in a row to end a longer lull early
    const int kShortLullSeconds = 5 * 60;   // up to this length, a single riddle is enough
    int g_steps = kChallengeSteps;
    const double kWrongCooldown = 3.0;

    // Gentle reminders, shown one at a time in a shuffled order.
    const wchar_t* kPrompts[] = {
#include "prompts.inc"
    };
    constexpr int kPromptCount = sizeof(kPrompts) / sizeof(kPrompts[0]);

    struct Screen
    {
        HWND hwnd = nullptr;
        RECT rc{};
        UINT dpi = 96;
        CompSurface surf;
        RenderTex scene;
        ComPtr<IDWriteTextFormat> fmtCount, fmtPrompt, fmtCue, fmtTitle, fmtProblem, fmtHint, fmtRiddle, fmtIcon, fmtGuide;
        float W = 0, H = 0; // DIPs
        bool shown = false;

        // Adaptive ink: a mipmapped copy of the scene, a small level of which is read back a few
        // times a second through two staging textures (never waiting on the GPU).
        ComPtr<ID3D11Texture2D> probe, probeRead[2];
        ComPtr<ID3D11ShaderResourceView> probeSrv;
        int probeSrcW = 0, probeSrcH = 0, probeW = 0, probeH = 0, probeNext = 0;
        UINT probeLevel = 0;
        bool probePending[2] = {};
        double probeAt = -1;
        InkStyle ink[kInkRegions], inkTarget[kInkRegions];
        bool inkSet = false;
        ComPtr<ID2D1Effect> plateBlur;
    };

    std::vector<Screen*> g_screens;
    bool g_active = false, g_dev = false, g_rebuild = false;
    double g_start = 0, g_end = 0, g_fadeOutStart = -1, g_sceneT0 = 0, g_lastEnforce = 0;
    bool g_cursorHidden = false;
    int g_scene = 0;
    uint32_t g_frame = 0;
    int g_promptOrder[kPromptCount];
    HANDLE g_watchdogStop = nullptr;
    std::mt19937 g_rng;
    int g_snapPrompt = -1; // dev snapshots: force this prompt fully visible

    // ---------------------------------------------------------------- early-exit math challenge
    // Deliberately a bit of a chore: press Esc, then solve three problems in a row: two quick
    // calculations and one short riddle. A wrong answer resets the streak and locks input for a
    // few seconds. The card stays open until Esc, so there's time to think.
    struct Riddle { const wchar_t* q; int answer; const wchar_t* why = nullptr; };
    const Riddle kRiddles[] = {
#include "riddles_a.inc"
#include "riddles_b.inc"
    };
    constexpr int kRiddleCount = sizeof(kRiddles) / sizeof(kRiddles[0]);
    enum { kAdd, kMulSmall, kMulTeen, kRiddle };

    struct Challenge
    {
        bool open = false;
        int solved = 0;
        int kinds[kChallengeSteps] = { kAdd, kMulSmall, kRiddle };
        std::wstring text;
        bool riddle = false;
        int answer = 0;
        std::wstring typed;
        std::wstring why;          // short guide shown after a wrong answer
        bool reveal = false;       // showing the solution of the last problem
        std::wstring revealTyped;  // what was typed
        double lastInput = 0, shakeStart = -10, cooldownUntil = 0, successAt = -10;
        float alpha = 0;
    } g_ch;

    int RandInt(int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(g_rng); }

    // Riddles come in a shuffled order that is saved, so none repeats until all have been seen.
    const Riddle& NextRiddle()
    {
        auto& s = g_settings;
        if (s.riddleSeed == 0 || s.riddleCursor < 0 || s.riddleCursor >= kRiddleCount)
        {
            s.riddleSeed = (unsigned)(GetTickCount64() & 0x7FFFFFFF) | 1u;
            s.riddleCursor = 0;
        }
        std::vector<int> order(kRiddleCount);
        for (int i = 0; i < kRiddleCount; i++) order[i] = i;
        std::shuffle(order.begin(), order.end(), std::mt19937(s.riddleSeed));
        const Riddle& r = kRiddles[order[s.riddleCursor++]];
        SaveSettings();
        return r;
    }

    // Two different calculations plus one riddle, in a random order.
    void ShuffleKinds()
    {
        int arith[3] = { kAdd, kMulSmall, kMulTeen };
        std::shuffle(std::begin(arith), std::end(arith), g_rng);
        g_ch.kinds[0] = arith[0];
        g_ch.kinds[1] = arith[1];
        g_ch.kinds[2] = kRiddle;
        std::shuffle(std::begin(g_ch.kinds), std::end(g_ch.kinds), g_rng);
        if (g_steps == 1) g_ch.kinds[0] = kRiddle; // short lulls: a single riddle
    }

    // Each calculation comes with a one-line guide (shown only if it's answered wrong).
    void NewProblem()
    {
        int a, b;
        wchar_t buf[48], why[160];
        g_ch.riddle = false;
        switch (g_ch.kinds[g_ch.solved % g_steps])
        {
        case kAdd:
            a = RandInt(128, 899); b = RandInt(128, 899); g_ch.answer = a + b;
            swprintf_s(buf, L"%d + %d", a, b);
            swprintf_s(why, L"Hundreds first: %d + %d = %d. Then %d + %d = %d. Together %d.",
                       a / 100 * 100, b / 100 * 100, a / 100 * 100 + b / 100 * 100, a % 100, b % 100, a % 100 + b % 100, a + b);
            break;
        case kMulSmall:
            a = RandInt(13, 49); b = RandInt(6, 9); g_ch.answer = a * b;
            swprintf_s(buf, L"%d \x00D7 %d", a, b);
            swprintf_s(why, L"Split %d into %d + %d: %d \x00D7 %d = %d, %d \x00D7 %d = %d, together %d.",
                       a, a / 10 * 10, a % 10, a / 10 * 10, b, a / 10 * 10 * b, a % 10, b, a % 10 * b, a * b);
            break;
        case kMulTeen:
            a = RandInt(21, 69); b = RandInt(12, 19); g_ch.answer = a * b;
            swprintf_s(buf, L"%d \x00D7 %d", a, b);
            swprintf_s(why, L"%d \x00D7 %d = %d \x00D7 10 + %d \x00D7 %d = %d + %d = %d.",
                       a, b, a, a, b - 10, a * 10, a * (b - 10), a * b);
            break;
        default:
        {
            const Riddle& r = NextRiddle();
            g_ch.text = r.q;
            g_ch.answer = r.answer;
            g_ch.why = r.why ? r.why : L"";
            g_ch.riddle = true;
            g_ch.typed.clear();
            return;
        }
        }
        g_ch.text = buf;
        g_ch.why = why;
        g_ch.typed.clear();
    }

    void OpenChallenge()
    {
        g_ch.open = true;
        g_ch.solved = 0;
        g_ch.reveal = false;
        ShuffleKinds();
        g_ch.lastInput = gfx::Now();
        g_ch.cooldownUntil = 0;
        NewProblem();
    }

    // Sound can be switched off/on mid-lull with M (independent of the saved preference).
    bool g_silenced = false;
    double g_soundToastAt = -100;

    void ToggleSilence()
    {
        g_silenced = !g_silenced;
        if (g_silenced) audio::MuteAll();
        else audio::RestoreAll();
        g_soundToastAt = gfx::Now();
    }

    void OnChallengeKey(UINT vk);

    // Every key pressed during a lull lands here (the system never sees it).
    void OnLullKey(UINT vk)
    {
        if (g_fadeOutStart >= 0) return;
        if (vk == 'M') { ToggleSilence(); return; }
        OnChallengeKey(vk);
    }

    void OnChallengeKey(UINT vk)
    {
        double now = gfx::Now();
        if (!g_settings.earlyExit || g_fadeOutStart >= 0) return;
        if (!g_ch.open)
        {
            if (vk == VK_ESCAPE) OpenChallenge();
            return;
        }
        g_ch.lastInput = now;
        if (vk == VK_ESCAPE) { g_ch.open = false; return; }
        if (now < g_ch.cooldownUntil) return;

        // Showing the solution of a wrong answer: read at your own pace, Enter for a new problem.
        if (g_ch.reveal)
        {
            if (vk == VK_RETURN)
            {
                g_ch.reveal = false;
                g_ch.solved = 0;
                ShuffleKinds();
                NewProblem();
            }
            return;
        }

        int digit = -1;
        if (vk >= '0' && vk <= '9') digit = vk - '0';
        else if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) digit = vk - VK_NUMPAD0;
        if (digit >= 0) { if (g_ch.typed.size() < 6) g_ch.typed += (wchar_t)(L'0' + digit); return; }
        if (vk == VK_BACK) { if (!g_ch.typed.empty()) g_ch.typed.pop_back(); return; }
        if (vk != VK_RETURN || g_ch.typed.empty()) return;

        if (_wtoi(g_ch.typed.c_str()) == g_ch.answer)
        {
            g_ch.solved++;
            g_ch.successAt = now;
            if (g_ch.solved >= g_steps) { g_ch.open = false; g_end = std::min(g_end, now); return; }
            NewProblem();
        }
        else
        {
            g_ch.reveal = true;
            g_ch.revealTyped = g_ch.typed;
            g_ch.typed.clear();
            g_ch.shakeStart = now;
            g_ch.cooldownUntil = now + kWrongCooldown;
        }
    }

    // ---------------------------------------------------------------- keyboard shield
    const UINT WM_APP_KEY = WM_APP + 40;
    HANDLE g_hookThread = nullptr;
    DWORD g_hookThreadId = 0;
    std::atomic<bool> g_blockKeys{ false };
    std::atomic<HWND> g_keySink{ nullptr }; // overlay window that receives blocked key presses
    bool g_downAtStart[256];
    bool g_blockedDown[256];

    bool IsPassThroughKey(DWORD vk)
    {
        switch (vk)
        {
        case VK_VOLUME_MUTE: case VK_VOLUME_DOWN: case VK_VOLUME_UP:
        case VK_MEDIA_NEXT_TRACK: case VK_MEDIA_PREV_TRACK: case VK_MEDIA_STOP: case VK_MEDIA_PLAY_PAUSE:
            return true;
        }
        return false;
    }

    LRESULT CALLBACK KeyboardProc(int code, WPARAM wp, LPARAM lp)
    {
        if (code != HC_ACTION) return CallNextHookEx(nullptr, code, wp, lp);

        // Ask the rest of the hook chain first. Synergy / Deskflow / Mouse Without Borders
        // swallow keys while you are controlling another machine. If one of them did, the
        // key is meant for that machine: let it go, don't block it.
        LRESULT next = CallNextHookEx(nullptr, code, wp, lp);
        if (next != 0) return next;

        auto* k = (KBDLLHOOKSTRUCT*)lp;
        DWORD vk = k->vkCode & 0xFF;
        bool up = (k->flags & LLKHF_UP) != 0;

        if (!g_blockKeys)
        {
            // Grace period after a lull: swallow releases of keys whose press we blocked,
            // so e.g. a lone Win key-up can't pop the Start menu.
            if (up && g_blockedDown[vk]) { g_blockedDown[vk] = false; return 1; }
            return 0;
        }

        if (up)
        {
            if (g_downAtStart[vk]) { g_downAtStart[vk] = false; return 0; } // held before the lull: release normally
            g_blockedDown[vk] = false;
            return 1;
        }
        if (IsPassThroughKey(vk)) return 0;
        g_blockedDown[vk] = true;
        // The system never sees the key, but the math challenge does.
        if (HWND sink = g_keySink) PostMessageW(sink, WM_APP_KEY, vk, 0);
        return 1;
    }

    DWORD WINAPI HookThread(LPVOID ready)
    {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
        MSG m;
        PeekMessageW(&m, nullptr, 0, 0, PM_NOREMOVE);
        HHOOK hook = SetWindowsHookExW(WH_KEYBOARD_LL, KeyboardProc, GetModuleHandleW(nullptr), 0);
        SetEvent((HANDLE)ready);
        while (GetMessageW(&m, nullptr, 0, 0) > 0) {}
        if (hook) UnhookWindowsHookEx(hook);
        return 0;
    }

    void StartShield()
    {
        for (int vk = 0; vk < 256; vk++)
        {
            g_downAtStart[vk] = (GetAsyncKeyState(vk) & 0x8000) != 0;
            g_blockedDown[vk] = false;
        }
        g_blockKeys = true;
        if (g_hookThread) return;
        HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        g_hookThread = CreateThread(nullptr, 0, HookThread, ready, 0, &g_hookThreadId);
        WaitForSingleObject(ready, 2000);
        CloseHandle(ready);
    }

    void CALLBACK StopShieldTimer(HWND, UINT, UINT_PTR id, DWORD)
    {
        KillTimer(nullptr, id);
        if (g_active || !g_hookThread) return;
        PostThreadMessageW(g_hookThreadId, WM_QUIT, 0, 0);
        WaitForSingleObject(g_hookThread, 1000);
        CloseHandle(g_hookThread);
        g_hookThread = nullptr;
    }

    void StopShield()
    {
        g_blockKeys = false;
        // Keep the hook a moment longer to absorb key releases (see grace period above).
        SetTimer(nullptr, 0, 1500, StopShieldTimer);
    }

    DWORD WINAPI Watchdog(LPVOID ms)
    {
        // Last-resort safety net: if anything ever hangs, the lock can't outlive its timer.
        if (WaitForSingleObject(g_watchdogStop, (DWORD)(UINT_PTR)ms) == WAIT_TIMEOUT)
        {
            audio::RestoreFromThread();
            ExitProcess(3);
        }
        return 0;
    }

    // ---------------------------------------------------------------- windows

    void ForceForeground(HWND h)
    {
        if (GetForegroundWindow() == h) return;
        if (SetForegroundWindow(h)) return;
        HWND fg = GetForegroundWindow();
        DWORD fgThread = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
        DWORD me = GetCurrentThreadId();
        if (fgThread && fgThread != me) AttachThreadInput(me, fgThread, TRUE);
        BringWindowToTop(h);
        SetForegroundWindow(h);
        if (fgThread && fgThread != me) AttachThreadInput(me, fgThread, FALSE);
    }

    bool IsOurs(HWND h)
    {
        for (auto* s : g_screens) if (s->hwnd == h) return true;
        return false;
    }

    LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
    {
        switch (msg)
        {
        case WM_SETCURSOR:
            // No cursor on this PC during a lull. (On machines reached through Synergy the
            // cursor is drawn by that machine, so it stays visible there.)
            SetCursor(nullptr);
            return TRUE;
        case WM_APP_KEY:
            OnLullKey((UINT)wp);
            return 0;
        case WM_MOUSEACTIVATE: return MA_ACTIVATE;
        case WM_ERASEBKGND: return 1;
        case WM_DISPLAYCHANGE: g_rebuild = true; return 0;
        case WM_CLOSE: return 0; // only we close these
        case WM_SYSCOMMAND:
            switch (wp & 0xFFF0) { case SC_CLOSE: case SC_KEYMENU: case SC_TASKLIST: case SC_SCREENSAVE: return 0; }
            break;
        case WM_KEYDOWN:
            // Only reached in --dev mode (no keyboard shield): route keys like the hook would.
            if (g_dev) OnLullKey((UINT)wp);
            return 0;
        case WM_SYSKEYDOWN: case WM_SYSKEYUP: case WM_SYSCHAR: return 0;
        }
        return DefWindowProcW(h, msg, wp, lp);
    }

    void RegisterClassOnce()
    {
        static bool done = false;
        if (done) return;
        WNDCLASSEXW wc{ sizeof(wc) };
        wc.lpfnWndProc = WndProc;
        wc.hInstance = g_hinst;
        wc.lpszClassName = kClass;
        wc.hCursor = nullptr;
        RegisterClassExW(&wc);
        done = true;
    }

    IDWriteTextFormat* MakeFormat(const wchar_t* family, DWRITE_FONT_WEIGHT weight, float size)
    {
        IDWriteTextFormat* f = nullptr;
        gfx::DWrite()->CreateTextFormat(family, nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                        size, L"en-us", &f);
        if (f)
        {
            f->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        }
        return f;
    }

    void MakeFormats(Screen* s)
    {
        float hd = s->H;
        const wchar_t* display = L"Segoe UI Variable Display";
        s->fmtCount.Attach(MakeFormat(display, DWRITE_FONT_WEIGHT_NORMAL, std::clamp(hd * 0.036f, 26.f, 60.f)));
        s->fmtPrompt.Attach(MakeFormat(display, DWRITE_FONT_WEIGHT_NORMAL, std::clamp(hd * 0.03f, 22.f, 46.f)));
        s->fmtCue.Attach(MakeFormat(display, DWRITE_FONT_WEIGHT_SEMI_LIGHT, std::clamp(hd * 0.026f, 20.f, 40.f)));
        s->fmtTitle.Attach(MakeFormat(L"Segoe UI Variable Text", DWRITE_FONT_WEIGHT_SEMI_BOLD, 15.f));
        s->fmtProblem.Attach(MakeFormat(display, DWRITE_FONT_WEIGHT_LIGHT, 44.f));
        s->fmtHint.Attach(MakeFormat(L"Segoe UI Variable Text", DWRITE_FONT_WEIGHT_NORMAL, 14.f));
        s->fmtRiddle.Attach(MakeFormat(L"Segoe UI Variable Text", DWRITE_FONT_WEIGHT_NORMAL, 19.f));
        s->fmtIcon.Attach(MakeFormat(L"Segoe Fluent Icons", DWRITE_FONT_WEIGHT_NORMAL, 16.f));
        s->fmtGuide.Attach(MakeFormat(L"Segoe UI Variable Text", DWRITE_FONT_WEIGHT_NORMAL, 15.5f));
        if (s->fmtGuide)
        {
            s->fmtGuide->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
            s->fmtGuide->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        }
        if (s->fmtRiddle)
        {
            s->fmtRiddle->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
            s->fmtRiddle->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
            s->fmtRiddle->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, 27.f, 21.f);
        }
    }

    BOOL CALLBACK AddMonitor(HMONITOR mon, HDC, LPRECT, LPARAM)
    {
        MONITORINFO mi{ sizeof(mi) };
        GetMonitorInfoW(mon, &mi);
        auto* s = new Screen();
        s->rc = mi.rcMonitor;
        UINT dx = 96, dy = 96;
        GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dx, &dy);
        s->dpi = dx;
        int w = s->rc.right - s->rc.left, h = s->rc.bottom - s->rc.top;
        s->W = w * 96.f / s->dpi;
        s->H = h * 96.f / s->dpi;
        MakeFormats(s);

        s->hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOREDIRECTIONBITMAP, kClass, L"Lull", WS_POPUP,
                                  s->rc.left, s->rc.top, w, h, nullptr, nullptr, g_hinst, nullptr);
        if (!s->hwnd || !s->surf.Create(s->hwnd, w, h))
        {
            if (s->hwnd) DestroyWindow(s->hwnd);
            delete s;
            return TRUE;
        }
        g_screens.push_back(s);
        return TRUE;
    }

    void CreateScreens()
    {
        RegisterClassOnce();
        EnumDisplayMonitors(nullptr, nullptr, AddMonitor, 0);
        g_keySink = g_screens.empty() ? nullptr : g_screens[0]->hwnd;
    }

    void DestroyScreens()
    {
        g_keySink = nullptr;
        for (auto* s : g_screens)
        {
            s->surf.Reset();
            s->scene.Reset();
            DestroyWindow(s->hwnd);
            delete s;
        }
        g_screens.clear();
    }

    // ---------------------------------------------------------------- drawing

    D2D1_COLOR_F WithA(D2D1_COLOR_F c, float a) { c.a = a; return c; }

    D2D1_COLOR_F MixC(D2D1_COLOR_F a, D2D1_COLOR_F b, float t)
    {
        return D2D1::ColorF(a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t);
    }
    D2D1_COLOR_F ScaleC(D2D1_COLOR_F c, float k) { return D2D1::ColorF(c.r * k, c.g * k, c.b * k, c.a); }

    // ---------------------------------------------------------------- adaptive ink
    // A few times a second the scene behind each line of text is measured. The text gets light or
    // dark ink, whichever stands out more against the worst spots behind it (the brightest for
    // light ink, the darkest for dark ink), faintly tinted with the scene's own colour, and only
    // as much glow and shade as it needs to stay readable.

    float Luminance(const float c[3]) // sRGB-encoded colour -> relative luminance
    {
        auto lin = [](float v) { return powf(std::clamp(v, 0.f, 1.f), 2.2f); };
        return 0.2126f * lin(c[0]) + 0.7152f * lin(c[1]) + 0.0722f * lin(c[2]);
    }
    float Contrast(float a, float b) { return (std::max(a, b) + 0.05f) / (std::min(a, b) + 0.05f); }

    InkStyle StyleFor(const float mean[3], float lumLo, float lumHi, bool wasLight)
    {
        float cLight = Contrast(0.86f, lumHi), cDark = Contrast(0.022f, lumLo);
        bool light = wasLight ? cLight * 1.3f >= cDark : cLight >= cDark * 1.3f; // no flip-flopping
        float mx = std::max({ mean[0], mean[1], mean[2], 0.02f });
        D2D1_COLOR_F hue = D2D1::ColorF(mean[0] / mx, mean[1] / mx, mean[2] / mx, 1.f); // the scene's colour, full brightness
        InkStyle st;
        st.light = light;
        if (light)
        {
            st.ink = MixC(D2D1::ColorF(1, 1, 1), hue, 0.10f);
            st.glow = MixC(D2D1::ColorF(0.02f, 0.025f, 0.06f), ScaleC(hue, 0.14f), 0.5f);
        }
        else
        {
            st.ink = MixC(D2D1::ColorF(0.10f, 0.09f, 0.18f), ScaleC(hue, 0.24f), 0.45f);
            st.glow = MixC(D2D1::ColorF(1, 1, 1), hue, 0.10f);
        }
        float c = light ? cLight : cDark;
        float need = std::clamp((7.f / c - 1.f) / 2.f, 0.f, 1.f); // 0 = plenty of contrast, 1 = far too little
        float busy = std::clamp((lumHi - lumLo - 0.08f) / 0.3f, 0.f, 1.f); // a pattern running through the letters
        // A busy pattern behind the letters is melted into its own average colour (the plate);
        // what contrast is still missing comes from a soft shade and a glow in the opposite tone.
        // A light glow around dark letters washes them out, so dark ink gets a fainter one.
        st.plate = std::clamp(busy * 1.4f + need * 0.5f, 0.f, 1.f);
        st.glowA = (light ? 0.38f : 0.2f) + 0.45f * need;
        st.spot = need;
        return st;
    }

    // Before the first measurement: the scene's known tone.
    InkStyle StaticStyle(bool lightBackground)
    {
        float grey[3] = { 0.5f, 0.5f, 0.5f };
        return lightBackground ? StyleFor(grey, 0.75f, 0.9f, false) : StyleFor(grey, 0.02f, 0.08f, true);
    }

    void ReadInk(Screen* s, const uint8_t* data, UINT pitch)
    {
        using DirectX::PackedVector::HALF;
        using DirectX::PackedVector::XMConvertHalfToFloat;
        std::vector<float> lums;
        for (int r = 0; r < kInkRegions; r++)
        {
            int x0 = std::clamp((int)(kInkRects[r][0] * s->probeW), 0, s->probeW - 1);
            int x1 = std::clamp((int)ceilf(kInkRects[r][2] * s->probeW), x0 + 1, s->probeW);
            int y0 = std::clamp((int)(kInkRects[r][1] * s->probeH), 0, s->probeH - 1);
            int y1 = std::clamp((int)ceilf(kInkRects[r][3] * s->probeH), y0 + 1, s->probeH);
            float mean[3] = {};
            lums.clear();
            for (int y = y0; y < y1; y++)
            {
                const HALF* row = (const HALF*)(data + (size_t)y * pitch);
                for (int x = x0; x < x1; x++)
                {
                    float c[3] = { XMConvertHalfToFloat(row[x * 4]), XMConvertHalfToFloat(row[x * 4 + 1]), XMConvertHalfToFloat(row[x * 4 + 2]) };
                    for (int k = 0; k < 3; k++) mean[k] += c[k];
                    lums.push_back(Luminance(c));
                }
            }
            for (float& m : mean) m /= (float)lums.size();
            std::sort(lums.begin(), lums.end());
            float lo = lums[lums.size() / 10], hi = lums[lums.size() * 9 / 10];
            s->inkTarget[r] = StyleFor(mean, lo, hi, s->inkSet ? s->ink[r].light : hi < 0.35f);
        }
        if (!s->inkSet)
        {
            for (int r = 0; r < kInkRegions; r++) s->ink[r] = s->inkTarget[r];
            s->inkSet = true;
        }
    }

    // Starts a measurement: copy the scene, shrink it with mipmaps, copy a ~100 px wide level out.
    void ProbeStart(Screen* s, double now)
    {
        auto& sc = s->scene;
        if (!sc.tex || s->probePending[s->probeNext]) return;
        auto dev = gfx::Device();
        auto ctx = gfx::Ctx();
        if (!s->probe || s->probeSrcW != sc.w || s->probeSrcH != sc.h)
        {
            s->probe.Reset(); s->probeSrv.Reset(); s->probeRead[0].Reset(); s->probeRead[1].Reset();
            s->probePending[0] = s->probePending[1] = false;
            D3D11_TEXTURE2D_DESC td{};
            td.Width = sc.w; td.Height = sc.h; td.MipLevels = 0; td.ArraySize = 1; td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            td.SampleDesc.Count = 1; td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            td.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
            if (FAILED(dev->CreateTexture2D(&td, nullptr, &s->probe))) return;
            D3D11_TEXTURE2D_DESC got{};
            s->probe->GetDesc(&got);
            if (FAILED(dev->CreateShaderResourceView(s->probe.Get(), nullptr, &s->probeSrv))) { s->probe.Reset(); return; }
            UINT level = 0;
            while (level + 1 < got.MipLevels && (sc.w >> (level + 1)) >= 96) level++;
            s->probeLevel = level;
            s->probeW = std::max(1, sc.w >> level);
            s->probeH = std::max(1, sc.h >> level);
            D3D11_TEXTURE2D_DESC rd{};
            rd.Width = s->probeW; rd.Height = s->probeH; rd.MipLevels = 1; rd.ArraySize = 1; rd.Format = td.Format;
            rd.SampleDesc.Count = 1; rd.Usage = D3D11_USAGE_STAGING; rd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            for (auto& r : s->probeRead)
                if (FAILED(dev->CreateTexture2D(&rd, nullptr, &r))) { s->probe.Reset(); return; }
            s->probeSrcW = sc.w; s->probeSrcH = sc.h;
        }
        s->probeAt = now;
        ctx->CopySubresourceRegion(s->probe.Get(), 0, 0, 0, 0, sc.tex.Get(), 0, nullptr);
        ctx->GenerateMips(s->probeSrv.Get());
        ctx->CopySubresourceRegion(s->probeRead[s->probeNext].Get(), 0, 0, 0, 0, s->probe.Get(), s->probeLevel, nullptr);
        s->probePending[s->probeNext] = true;
        s->probeNext ^= 1;
    }

    void ResetProbe(Screen* s)
    {
        s->probe.Reset(); s->probeSrv.Reset(); s->probeRead[0].Reset(); s->probeRead[1].Reset();
        s->probePending[0] = s->probePending[1] = false;
        s->probeSrcW = s->probeSrcH = 0;
    }

    // Picks up finished measurements (sync: wait for them, for dev snapshots).
    void ProbeCollect(Screen* s, bool sync)
    {
        for (int i = 0; i < 2; i++)
        {
            if (!s->probePending[i] || !s->probeRead[i]) continue;
            D3D11_MAPPED_SUBRESOURCE m{};
            HRESULT hr = gfx::Ctx()->Map(s->probeRead[i].Get(), 0, D3D11_MAP_READ, sync ? 0 : D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
            if (hr == DXGI_ERROR_WAS_STILL_DRAWING) continue;
            s->probePending[i] = false;
            if (FAILED(hr)) continue;
            ReadInk(s, (const uint8_t*)m.pData, m.RowPitch);
            gfx::Ctx()->Unmap(s->probeRead[i].Get(), 0);
        }
    }

    // Eases each line's ink toward its measured style (about a second), so nothing flickers.
    void EaseInk(Screen* s, float dt)
    {
        if (!s->inkSet) return;
        float k = 1.f - expf(-dt * 2.5f);
        for (int r = 0; r < kInkRegions; r++)
        {
            InkStyle& a = s->ink[r];
            const InkStyle& b = s->inkTarget[r];
            a.ink = MixC(a.ink, b.ink, k);
            a.glow = MixC(a.glow, b.glow, k);
            a.glowA += (b.glowA - a.glowA) * k;
            a.spot += (b.spot - a.spot) * k;
            a.plate += (b.plate - a.plate) * k;
            a.light = b.light;
        }
    }

    // Soft blurred shadow of a shape recorded by `record`, drawn into dc.
    template <typename Fn>
    void SoftShadow(ID2D1DeviceContext* dc, D2D1_COLOR_F color, float blur, Fn record)
    {
        auto rec = gfx::DC2();
        ComPtr<ID2D1CommandList> cl;
        if (!rec || FAILED(rec->CreateCommandList(&cl))) return;
        rec->SetTarget(cl.Get());
        rec->BeginDraw();
        ComPtr<ID2D1SolidColorBrush> black;
        rec->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 1), &black);
        record(rec, black.Get());
        rec->EndDraw();
        rec->SetTarget(nullptr);
        cl->Close();
        ComPtr<ID2D1Effect> fx;
        if (FAILED(dc->CreateEffect(CLSID_D2D1Shadow, &fx))) return;
        fx->SetInput(0, cl.Get());
        fx->SetValue(D2D1_SHADOW_PROP_BLUR_STANDARD_DEVIATION, blur);
        fx->SetValue(D2D1_SHADOW_PROP_COLOR, D2D1::Vector4F(color.r, color.g, color.b, color.a));
        dc->DrawImage(fx.Get());
    }

    ComPtr<IDWriteTextLayout> CenteredLayout(IDWriteTextFormat* fmt, const wchar_t* text, bool tabular)
    {
        ComPtr<IDWriteTextLayout> layout;
        gfx::DWrite()->CreateTextLayout(text, (UINT32)wcslen(text), fmt, 2000.f, 200.f, &layout);
        if (layout && tabular)
        {
            ComPtr<IDWriteTypography> typo;
            gfx::DWrite()->CreateTypography(&typo);
            typo->AddFontFeature({ DWRITE_FONT_FEATURE_TAG_TABULAR_FIGURES, 1 });
            layout->SetTypography(typo.Get(), { 0, (UINT32)wcslen(text) });
        }
        return layout;
    }

    // Centered text on a card.
    void DrawCentered(ID2D1DeviceContext* dc, IDWriteTextFormat* fmt, const wchar_t* text, float cx, float cy,
                      ID2D1SolidColorBrush* brush, bool tabular = false)
    {
        auto layout = CenteredLayout(fmt, text, tabular);
        if (layout) dc->DrawTextLayout(D2D1::Point2F(cx - 1000.f, cy - 100.f), layout.Get(), brush, D2D1_DRAW_TEXT_OPTIONS_NONE);
    }

    // Behind a line on a busy scene: the scene itself, blurred, in a feathered oval, so the pattern
    // melts into its own average colour right behind the letters (no box, no added colour).
    void DrawPlate(ID2D1DeviceContext* dc, Screen* s, float cx, float cy, float rx, float ry, float amount)
    {
        if (!s->scene.bmp || amount < 0.01f) return;
        if (!s->plateBlur && FAILED(dc->CreateEffect(CLSID_D2D1GaussianBlur, &s->plateBlur))) return;
        float px = s->scene.w / std::max(1.f, s->W); // scene pixels per DIP
        s->plateBlur->SetInput(0, s->scene.bmp.Get());
        s->plateBlur->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION, 10.f * px);
        s->plateBlur->SetValue(D2D1_GAUSSIANBLUR_PROP_BORDER_MODE, D2D1_BORDER_MODE_HARD);
        D2D1_GRADIENT_STOP stops[] = {
            { 0.0f, D2D1::ColorF(1, 1, 1, amount) },
            { 0.6f, D2D1::ColorF(1, 1, 1, amount) },
            { 1.0f, D2D1::ColorF(1, 1, 1, 0.f) },
        };
        ComPtr<ID2D1GradientStopCollection> coll;
        dc->CreateGradientStopCollection(stops, 3, &coll);
        ComPtr<ID2D1RadialGradientBrush> mask;
        dc->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties(D2D1::Point2F(cx, cy), D2D1::Point2F(0, 0), rx, ry),
                                      coll.Get(), &mask);
        if (!mask) return;
        dc->PushLayer(D2D1::LayerParameters1(D2D1::RectF(cx - rx, cy - ry, cx + rx, cy + ry), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                             D2D1::IdentityMatrix(), 1.f, mask.Get()), nullptr);
        // Effects work in context pixels whatever the bitmap's DPI, so size by the output's bounds.
        D2D1_MATRIX_3X2_F old;
        dc->GetTransform(&old);
        dc->SetTransform(D2D1::IdentityMatrix());
        ComPtr<ID2D1Image> image;
        s->plateBlur->GetOutput(&image);
        D2D1_RECT_F bounds{};
        dc->GetImageLocalBounds(image.Get(), &bounds);
        float iw = std::max(1.f, bounds.right - bounds.left), ih = std::max(1.f, bounds.bottom - bounds.top);
        dc->SetTransform(D2D1::Matrix3x2F::Scale(s->W / iw, s->H / ih) * old);
        dc->DrawImage(image.Get(), D2D1_INTERPOLATION_MODE_LINEAR);
        dc->SetTransform(old);
        dc->PopLayer();
    }

    // Centered text right on the scene, in its adaptive ink: on busy scenes the pattern behind
    // the line is melted away (plate), a soft shade adds what contrast is still missing, a glow
    // hugs the letters, then the letters. `below` extends the calm area under the line (the
    // countdown's bar).
    void DrawOnScene(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* brush, Screen* s, IDWriteTextFormat* fmt, const wchar_t* text,
                     float cx, float cy, const InkStyle& st, float alpha, bool tabular = false, float below = 0.f)
    {
        auto layout = CenteredLayout(fmt, text, tabular);
        if (!layout || alpha <= 0.002f) return;
        D2D1_POINT_2F origin = D2D1::Point2F(cx - 1000.f, cy - 100.f);
        float a = std::min(1.f, alpha / 0.85f);
        DWRITE_TEXT_METRICS m{};
        layout->GetMetrics(&m);
        float fs = fmt->GetFontSize();
        float pcy = cy + below / 2, prx = m.width / 2 + fs * 2.2f, pry = m.height / 2 + below / 2 + fs * 0.9f;
        DrawPlate(dc, s, cx, pcy, prx, pry, st.plate * a);
        if (st.spot > 0.01f)
        {
            D2D1_COLOR_F g = st.glow;
            D2D1_GRADIENT_STOP stops[] = {
                { 0.0f, D2D1::ColorF(g.r, g.g, g.b, 0.34f * a * st.spot) },
                { 0.6f, D2D1::ColorF(g.r, g.g, g.b, 0.22f * a * st.spot) },
                { 1.0f, D2D1::ColorF(g.r, g.g, g.b, 0.f) },
            };
            ComPtr<ID2D1GradientStopCollection> coll;
            dc->CreateGradientStopCollection(stops, 3, &coll);
            ComPtr<ID2D1RadialGradientBrush> spotBrush;
            dc->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties(D2D1::Point2F(cx, pcy), D2D1::Point2F(0, 0), prx, pry),
                                          coll.Get(), &spotBrush);
            if (spotBrush) dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, pcy), prx, pry), spotBrush.Get());
        }
        D2D1_COLOR_F shade = st.glow;
        shade.a = st.glowA * a;
        SoftShadow(dc, shade, std::max(3.f, fs * 0.16f), [&](ID2D1DeviceContext* rec, ID2D1Brush* b) {
            rec->DrawTextLayout(origin, layout.Get(), b, D2D1_DRAW_TEXT_OPTIONS_NONE);
        });
        D2D1_COLOR_F ink = st.ink;
        ink.a = std::min(1.f, alpha * (st.light ? 1.f : 1.12f)); // thin dark strokes need a little more
        brush->SetColor(ink);
        dc->DrawTextLayout(origin, layout.Get(), brush, D2D1_DRAW_TEXT_OPTIONS_NONE);
    }

    float TextWidth(IDWriteTextFormat* fmt, const std::wstring& t)
    {
        ComPtr<IDWriteTextLayout> l;
        gfx::DWrite()->CreateTextLayout(t.c_str(), (UINT32)t.size(), fmt, 4000.f, 200.f, &l);
        DWRITE_TEXT_METRICS m{};
        if (l) l->GetMetrics(&m);
        return m.widthIncludingTrailingWhitespace;
    }

    void DrawLeft(ID2D1DeviceContext* dc, IDWriteTextFormat* fmt, const std::wstring& t, float x, float cy, ID2D1SolidColorBrush* b)
    {
        ComPtr<IDWriteTextLayout> l;
        gfx::DWrite()->CreateTextLayout(t.c_str(), (UINT32)t.size(), fmt, 4000.f, 200.f, &l);
        if (!l) return;
        l->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        dc->DrawTextLayout(D2D1::Point2F(x, cy - 100.f), l.Get(), b);
    }

    // The "end early" card: a frosted panel with the current problem.
    void DrawChallenge(Screen* s, ID2D1DeviceContext* dc, ID2D1SolidColorBrush* b, double now, float op)
    {
        float a = g_ch.alpha * op;
        if (a < 0.01f) return;
        bool cooling = now < g_ch.cooldownUntil;
        bool reveal = g_ch.reveal;
        const D2D1_COLOR_F mint = Rgba(0x9FE7C8), coral = Rgba(0xFFB8A8);

        // Riddles (and solutions) get a wider card with wrapped text; calculations keep the big one-liner.
        float cw = (g_ch.riddle || reveal) ? 620.f : 460.f;
        ComPtr<IDWriteTextLayout> question, guide;
        float qh = 0, gh = 0;
        if (g_ch.riddle)
        {
            gfx::DWrite()->CreateTextLayout(g_ch.text.c_str(), (UINT32)g_ch.text.size(), s->fmtRiddle.Get(), cw - 80, 400, &question);
            DWRITE_TEXT_METRICS m{};
            if (question) question->GetMetrics(&m);
            qh = m.height;
        }
        if (reveal && !g_ch.why.empty())
        {
            gfx::DWrite()->CreateTextLayout(g_ch.why.c_str(), (UINT32)g_ch.why.size(), s->fmtGuide.Get(), cw - 80, 400, &guide);
            DWRITE_TEXT_METRICS m{};
            if (guide) guide->GetMetrics(&m);
            gh = m.height;
        }
        float body = g_ch.riddle ? qh + 22 + 64 : 64;          // question + answer row
        if (reveal) body += gh > 0 ? 12 + gh : 0;              // + the guide
        float chh = std::max(240.f, 92 + body + 56);

        float cx = s->W / 2;
        double ds = now - g_ch.shakeStart;
        float shake = ds < 0.7 ? sinf((float)ds * 44.f) * 14.f * expf(-(float)ds * 5.f) : 0.f;
        float lift = (1 - EaseOutCubic(g_ch.alpha)) * 12.f;
        float x0 = cx - cw / 2 + shake, y0 = s->H * 0.46f - chh / 2 + lift;
        cx += shake;

        b->SetColor(Rgba(0x0E1120, 0.84f * a));
        dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x0, y0, x0 + cw, y0 + chh), 20, 20), b);
        b->SetColor(Rgba(0xFFFFFF, 0.12f * a));
        dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x0 + 0.5f, y0 + 0.5f, x0 + cw - 0.5f, y0 + chh - 0.5f), 19.5f, 19.5f), b, 1.f);

        b->SetColor(reveal ? WithA(coral, 0.95f * a) : Rgba(0xFFFFFF, 0.8f * a));
        DrawCentered(dc, s->fmtTitle.Get(), reveal ? L"Not quite \x2014 here's how it works" : L"End this lull early?", cx, y0 + 34, b);

        // Streak dots (not for a single riddle, and not while showing a solution).
        for (int i = 0; i < g_steps && g_steps > 1 && !reveal; i++)
        {
            float dx = cx + (i - (g_steps - 1) / 2.f) * 18.f, dy = y0 + 60;
            bool done = i < g_ch.solved;
            float pulse = (done && i == g_ch.solved - 1) ? 1.f + 0.6f * expf(-(float)(now - g_ch.successAt) * 6.f) : 1.f;
            if (done)
            {
                b->SetColor(WithA(mint, 0.95f * a));
                dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(dx, dy), 4.5f * pulse, 4.5f * pulse), b);
            }
            else
            {
                b->SetColor(Rgba(0xFFFFFF, 0.32f * a));
                dc->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(dx, dy), 4.f, 4.f), b, 1.2f);
            }
        }

        bool caretOn = !reveal && fmod(now - g_ch.lastInput, 1.0) < 0.6;
        wchar_t answer[16];
        swprintf_s(answer, L"%d", g_ch.answer);
        float afterRow;
        if (g_ch.riddle)
        {
            // The riddle, then a centred answer slot (or the correct answer) under it.
            b->SetColor(Rgba(0xFFFFFF, (reveal ? 0.6f : 0.92f) * a));
            if (question) dc->DrawTextLayout(D2D1::Point2F(x0 + 40, y0 + 88), question.Get(), b);
            float py = y0 + 92 + qh + 22 + 30, slot = 180.f;
            if (reveal)
            {
                b->SetColor(WithA(mint, a));
                DrawCentered(dc, s->fmtProblem.Get(), answer, cx, py, b);
            }
            else
            {
                float tw = TextWidth(s->fmtProblem.Get(), g_ch.typed);
                b->SetColor(Rgba(0xFFFFFF, 0.95f * a));
                DrawLeft(dc, s->fmtProblem.Get(), g_ch.typed, cx - tw / 2, py, b);
                b->SetColor(Rgba(0xFFFFFF, 0.28f * a));
                dc->FillRectangle(D2D1::RectF(cx - slot / 2, py + 30, cx + slot / 2, py + 31.5f), b);
                if (caretOn)
                {
                    float cxr = cx + tw / 2 + 3;
                    b->SetColor(Rgba(0xFFFFFF, 0.85f * a));
                    dc->FillRectangle(D2D1::RectF(cxr, py - 22, cxr + 2, py + 22), b);
                }
            }
            afterRow = py + 34;
        }
        else
        {
            // "37 × 8 = 296": the slot while answering, the correct answer (mint) after a miss.
            std::wstring problem = g_ch.text + L" =";
            float wp = TextWidth(s->fmtProblem.Get(), problem);
            float slot = 130.f, gap = 18.f, total = wp + gap + slot;
            float px = cx - total / 2, py = y0 + 124;
            b->SetColor(Rgba(0xFFFFFF, (reveal ? 0.6f : 0.95f) * a));
            DrawLeft(dc, s->fmtProblem.Get(), problem, px, py, b);
            float ax = px + wp + gap;
            if (reveal)
            {
                b->SetColor(WithA(mint, a));
                DrawLeft(dc, s->fmtProblem.Get(), answer, ax, py, b);
            }
            else
            {
                b->SetColor(Rgba(0xFFFFFF, 0.95f * a));
                DrawLeft(dc, s->fmtProblem.Get(), g_ch.typed, ax, py, b);
                b->SetColor(Rgba(0xFFFFFF, 0.28f * a));
                dc->FillRectangle(D2D1::RectF(ax, py + 30, ax + slot, py + 31.5f), b);
                if (caretOn)
                {
                    float cxr = ax + TextWidth(s->fmtProblem.Get(), g_ch.typed) + 3;
                    b->SetColor(Rgba(0xFFFFFF, 0.85f * a));
                    dc->FillRectangle(D2D1::RectF(cxr, py - 22, cxr + 2, py + 22), b);
                }
            }
            afterRow = py + 34;
        }

        if (guide)
        {
            b->SetColor(Rgba(0xFFFFFF, 0.8f * a));
            dc->DrawTextLayout(D2D1::Point2F(x0 + 40, afterRow + 12), guide.Get(), b);
        }

        wchar_t msg[128];
        if (reveal)
        {
            if (cooling) swprintf_s(msg, L"You typed %s  \x00B7  a new problem in %d\x2026", g_ch.revealTyped.c_str(), (int)ceil(g_ch.cooldownUntil - now));
            else swprintf_s(msg, L"You typed %s  \x00B7  Enter for a new problem  \x00B7  Esc to go back", g_ch.revealTyped.c_str());
            b->SetColor(cooling ? WithA(coral, 0.9f * a) : Rgba(0xFFFFFF, 0.6f * a));
        }
        else
        {
            wcscpy_s(msg, g_steps == 1 ? L"Enter to check  \x00B7  Esc to go back" : L"Solve 3 in a row  \x00B7  Enter to check  \x00B7  Esc to go back");
            b->SetColor(Rgba(0xFFFFFF, 0.55f * a));
        }
        DrawCentered(dc, s->fmtHint.Get(), msg, cx, y0 + chh - 34, b);
    }

    // "Sound off" / "Sound on" pill after pressing M.
    void DrawSoundToast(Screen* s, ID2D1DeviceContext* dc, ID2D1SolidColorBrush* b, double now, float op)
    {
        double e = now - g_soundToastAt;
        if (e < 0 || e > 2.6) return;
        float a = Clamp01((float)std::min(e / 0.15, (2.6 - e) / 0.6)) * op;
        const wchar_t* label = g_silenced ? L"Sound off" : L"Sound on";
        const wchar_t* icon = g_silenced ? L"\xE74F" : L"\xE767";
        float tw = TextWidth(s->fmtHint.Get(), label);
        float w = 16 + 10 + tw + 40, h = 40, cx = s->W / 2, cy = s->H * 0.74f;
        float lift = (1 - EaseOutCubic(Clamp01((float)(e / 0.25)))) * 8.f;
        auto rr = D2D1::RoundedRect(D2D1::RectF(cx - w / 2, cy - h / 2 + lift, cx + w / 2, cy + h / 2 + lift), h / 2, h / 2);
        b->SetColor(Rgba(0x0E1120, 0.8f * a));
        dc->FillRoundedRectangle(rr, b);
        b->SetColor(Rgba(0xFFFFFF, 0.14f * a));
        dc->DrawRoundedRectangle(rr, b, 1.f);
        float x0 = cx - (16 + 10 + tw) / 2;
        b->SetColor(g_silenced ? Rgba(0xFFFFFF, 0.9f * a) : Rgba(0x9EF2D1, 0.95f * a));
        DrawLeft(dc, s->fmtIcon.Get(), icon, x0, cy + lift, b);
        b->SetColor(Rgba(0xFFFFFF, 0.92f * a));
        DrawLeft(dc, s->fmtHint.Get(), label, x0 + 26, cy + lift, b);
    }

    void DrawOverlay(Screen* s, ID2D1DeviceContext* dc, double now, float op)
    {
        SceneInfo info = kScenes[g_scene];
        if (g_scene == kSceneGrid && gfx::GridDay() > 0.5f) info.lightCenter = info.lightBottom = true; // day look
        // Each line's ink, measured from the scene behind it (the scene's known tone until then).
        auto style = [&](int region, bool lightBg) { return s->inkSet ? s->ink[region] : StaticStyle(lightBg); };
        ComPtr<ID2D1SolidColorBrush> brush;
        dc->CreateSolidColorBrush(Rgba(0xFFFFFF), &brush);
        float W = s->W, H = s->H, cx = W / 2;
        double elapsed = now - g_start, total = g_end - g_start;

        if (g_settings.showCountdown)
        {
            double remain = std::max(0.0, g_end - now);
            int secs = (int)ceil(remain - 1e-6);
            wchar_t buf[16];
            swprintf_s(buf, L"%d:%02d", secs / 60, secs % 60);
            float y = H * 0.875f;
            InkStyle st = style(InkCount, info.lightBottom);
            DrawOnScene(dc, brush.Get(), s, s->fmtCount.Get(), buf, cx, y, st, 0.92f * op, true, std::clamp(H * 0.036f, 26.f, 60.f) * 0.8f);

            float lw = 150.f, th = 2.f, ly = y + std::clamp(H * 0.036f, 26.f, 60.f) * 0.75f;
            float frac = (float)std::clamp(elapsed / std::max(total, 0.001), 0.0, 1.0);
            brush->SetColor(WithA(st.ink, 0.2f * op));
            dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(cx - lw / 2, ly, cx + lw / 2, ly + th), 1, 1), brush.Get());
            brush->SetColor(WithA(st.ink, 0.66f * op));
            dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(cx - lw / 2, ly, cx - lw / 2 + lw * frac, ly + th), 1, 1), brush.Get());
        }

        if (g_settings.reminders && g_ch.alpha < 0.5f)
        {
            if (g_scene == kSceneBreathe) // guide the rhythm
            {
                int phase; float pp;
                gfx::Breath(elapsed, &phase, &pp);
                static const wchar_t* cues[] = { L"Breathe in", L"Hold", L"Breathe out", L"" };
                const double lens[] = { 4.0, 1.5, 6.0, 1.0 };
                double inPhase = pp * lens[phase];
                float a = (float)std::min({ 1.0, inPhase / 0.7, (lens[phase] - inPhase) / 0.7 });
                if (phase == 1) a = 0.55f; // gentle "hold" between the two
                a = Clamp01(a);
                if (cues[phase][0])
                {
                    DrawOnScene(dc, brush.Get(), s, s->fmtCue.Get(), cues[phase], cx, H * 0.795f, style(InkCue, false), 0.86f * a * op);
                }
            }
            else
            {
                const double period = 24.0, first = 4.0;
                double pt = elapsed - first;
                if (pt >= 0)
                {
                    int idx = g_promptOrder[(int)(pt / period) % kPromptCount];
                    double u = fmod(pt, period);
                    float a = (float)std::clamp(std::min(u / 2.0, (20.0 - u) / 2.0), 0.0, 1.0);
                    a = EaseInOut(a);
                    if (g_snapPrompt >= 0) { idx = g_snapPrompt; a = 1.f; }
                    if (a > 0)
                    {
                        DrawOnScene(dc, brush.Get(), s, s->fmtPrompt.Get(), kPrompts[idx], cx, H * 0.5f, style(InkPrompt, info.lightCenter), 0.95f * a * op);
                    }
                }
            }
        }

        DrawChallenge(s, dc, brush.Get(), now, op);
        DrawSoundToast(s, dc, brush.Get(), now, op);

        // A brief hint at the start: the two keys that do something during a lull.
        {
            double e = now - g_start;
            float a = Clamp01((float)std::min((e - 1.5) / 0.8, (7.0 - e) / 1.2));
            if (a > 0 && g_fadeOutStart < 0)
            {
                DrawOnScene(dc, brush.Get(), s, s->fmtHint.Get(), g_settings.earlyExit ? L"M  sound on/off     \x00B7     Esc  end early" : L"M  sound on/off",
                            cx, H * 0.955f, style(InkHint, info.lightBottom), 0.85f * a * op);
            }
        }
    }

    float OpacityAt(double now)
    {
        float in = EaseInOut((float)((now - g_start) / kFadeIn));
        float out = g_fadeOutStart >= 0 ? 1.f - EaseInOut((float)((now - g_fadeOutStart) / kFadeOut)) : 1.f;
        return std::min(in, out);
    }

    void Finish()
    {
        g_active = false;
        DestroyScreens();
        StopShield();
        if (g_watchdogStop) { SetEvent(g_watchdogStop); CloseHandle(g_watchdogStop); g_watchdogStop = nullptr; }
        SetThreadExecutionState(ES_CONTINUOUS);
        audio::RestoreAll();
        if (g_cursorHidden) { ShowCursor(TRUE); g_cursorHidden = false; }
        g_ch.open = false;
        g_ch.alpha = 0;
        PostMessageW(g_mainWnd, WM_APP_LULL_DONE, 0, 0);
    }

    void RecreateGraphics()
    {
        for (auto* s : g_screens) { s->surf.Reset(); s->scene.Reset(); ResetProbe(s); }
        flyout::ReleaseGraphics();
        gfx::Shutdown();
        if (!gfx::Init()) return;
        for (auto* s : g_screens) s->surf.Create(s->hwnd, s->rc.right - s->rc.left, s->rc.bottom - s->rc.top);
    }
}

bool overlay::Active() { return g_active; }

bool overlay::Begin(int seconds, int scene, bool dev)
{
    if (g_active) return false;
    if (!gfx::Init()) return false;
    g_dev = dev;
    g_scene = scene;
    g_frame = 0;
    g_fadeOutStart = -1;
    g_rebuild = false;
    g_ch = Challenge{};
    g_start = gfx::Now();
    g_end = g_start + seconds;
    g_steps = seconds <= kShortLullSeconds ? 1 : kChallengeSteps;
    g_rng.seed((unsigned)GetTickCount64());
    g_sceneT0 = kScenes[scene].timeOffset + std::uniform_real_distribution<float>(0.f, 120.f)(g_rng);
    if (scene == kSceneBreathe) g_sceneT0 = 0; // breathing starts at the bottom of the breath
    for (int i = 0; i < kPromptCount; i++) g_promptOrder[i] = i;
    std::shuffle(g_promptOrder, g_promptOrder + kPromptCount, g_rng);

    CreateScreens();
    if (g_screens.empty()) return false;
    g_active = true;
    if (!g_cursorHidden) { ShowCursor(FALSE); g_cursorHidden = true; }

    if (!dev) StartShield();
    g_silenced = !dev && g_settings.muteSounds;
    g_soundToastAt = -100;
    if (g_silenced) audio::MuteAll();
    SetThreadExecutionState(ES_CONTINUOUS | ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED);
    g_watchdogStop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    CloseHandle(CreateThread(nullptr, 0, Watchdog, (LPVOID)(UINT_PTR)((seconds + 90) * 1000), 0, nullptr));

    Frame(); // first frame is fully transparent; windows appear and start catching input right away
    return true;
}

void overlay::Frame()
{
    if (!g_active) return;
    double now = gfx::Now();

    if (gfx::IsLost()) RecreateGraphics();
    if (g_rebuild)
    {
        g_rebuild = false;
        DestroyScreens();
        CreateScreens();
    }

    // Early-exit card: fade in/out. It stays open until Esc, however long the thinking takes.
    g_ch.alpha += ((g_ch.open ? 1.f : 0.f) - g_ch.alpha) * 0.2f;

    if (now >= g_end && g_fadeOutStart < 0)
    {
        g_fadeOutStart = now;
        audio::RestoreAll(); // sound back on first, so the chime is heard
        if (g_settings.chime) PlayChime();
    }
    if (g_fadeOutStart >= 0 && now - g_fadeOutStart >= kFadeOut) { Finish(); return; }

    // Stay in front: topmost, foreground.
    if (!g_dev && now - g_lastEnforce > 0.25 && !g_screens.empty())
    {
        g_lastEnforce = now;
        if (!IsOurs(GetForegroundWindow())) ForceForeground(g_screens[0]->hwnd);
        for (auto* s : g_screens)
            SetWindowPos(s->hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    }

    ApplyGridTuning();
    gfx::TickGrid(now);
    float op = OpacityAt(now);
    float t = (float)(g_sceneT0 + (now - g_start));
    float breath = gfx::Breath(now - g_start);
    auto dc = gfx::DC();
    g_frame++;
    static double lastInk = 0;
    float inkDt = (float)std::clamp(now - lastInk, 0.0, 0.1);
    lastInk = now;
    for (auto* s : g_screens)
    {
        int w = s->surf.w, h = s->surf.h;
        float rs = kScenes[g_scene].resScale;
        int sw = std::max(1, (int)(w * rs)), sh = std::max(1, (int)(h * rs));
        if (!s->scene.Ensure(sw, sh, DXGI_FORMAT_R16G16B16A16_FLOAT, true)) continue; // D2D blurs it behind busy text
        gfx::DrawScene(g_scene, s->scene.rtv.Get(), sw, sh, t, breath);
        gfx::Blit(s->scene.srv.Get(), s->surf.rtv.Get(), w, h, op, g_frame);
        ProbeCollect(s, false);
        if (now - s->probeAt > 0.2) ProbeStart(s, now);
        EaseInk(s, inkDt);

        dc->SetTarget(s->surf.bmp.Get());
        dc->SetDpi((float)s->dpi, (float)s->dpi);
        dc->BeginDraw();
        DrawOverlay(s, dc, now, op);
        if (dc->EndDraw() == D2DERR_RECREATE_TARGET) gfx::MarkLost();
        dc->SetTarget(nullptr);
        s->surf.Present(0);

        if (!s->shown)
        {
            s->shown = true;
            ShowWindow(s->hwnd, SW_SHOWNOACTIVATE);
        }
    }
    if (g_frame == 1 && !g_screens.empty())
    {
        // Activate the window on the monitor with the cursor (or the first one).
        POINT pt; GetCursorPos(&pt);
        HWND target = g_screens[0]->hwnd;
        for (auto* s : g_screens) if (PtInRect(&s->rc, pt)) target = s->hwnd;
        ForceForeground(target);
        SetCursor(nullptr); // hide right away, not only after the mouse moves
    }

    double before = gfx::Now();
    DwmFlush();
    if (gfx::Now() - before < 0.002) Sleep(8); // display off / locked session: don't spin
}

void overlay::ReleaseGraphics()
{
    for (auto* s : g_screens) { s->surf.Reset(); s->scene.Reset(); ResetProbe(s); }
}

// Dev: runs the live (non-waiting) ink measurement on an offscreen Grid for a second and logs it.
bool overlay::DevInkTest(const wchar_t* path)
{
    if (!gfx::Init()) return false;
    FILE* f = nullptr;
    _wfopen_s(&f, path, L"w");
    if (!f) return false;
    Screen s;
    s.dpi = 96; s.W = 1600; s.H = 900;
    g_scene = kSceneGrid;
    ApplyGridTuning();
    double t0 = gfx::Now();
    for (int frame = 0; frame < 60; frame++)
    {
        double now = gfx::Now();
        s.scene.Ensure(1600, 900, DXGI_FORMAT_R16G16B16A16_FLOAT, true);
        gfx::DrawScene(kSceneGrid, s.scene.rtv.Get(), 1600, 900, 50.f + (float)(now - t0), 0.f);
        ProbeCollect(&s, false);
        if (now - s.probeAt > 0.2) ProbeStart(&s, now);
        gfx::Ctx()->Flush();
        EaseInk(&s, 1 / 60.f);
        const InkStyle& p = s.ink[InkPrompt];
        fprintf(f, "frame %2d  t=%.2f  measured=%d  prompt ink=%s (%.2f %.2f %.2f) plate=%.2f spot=%.2f glow=%.2f\n", frame, now - t0,
                s.inkSet, p.light ? "light" : "dark", p.ink.r, p.ink.g, p.ink.b, p.plate, p.spot, p.glowA);
        Sleep(16);
    }
    fclose(f);
    return true;
}

// Dev: render every scene with countdown + a reminder to PNGs (checks text contrast without locking).
// plain: text only (reminder, countdown, start hint) at two moments per scene; otherwise one
// moment with the cards and toasts shown on some scenes.
bool overlay::SnapshotScenes(const wchar_t* dir, bool plain)
{
    if (!gfx::Init()) return false;
    // At the primary monitor's real size and scaling, so the text renders as it would there.
    HMONITOR mon = MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{ sizeof(mi) };
    GetMonitorInfoW(mon, &mi);
    UINT dpiX = 96, dpiY = 96;
    GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dpiX, &dpiY);
    const int w = mi.rcMonitor.right - mi.rcMonitor.left, h = mi.rcMonitor.bottom - mi.rcMonitor.top;
    Screen s;
    s.dpi = dpiX; s.W = w * 96.f / dpiX; s.H = h * 96.f / dpiX;
    MakeFormats(&s);

    D3D11_TEXTURE2D_DESC td{};
    td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1; td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> tex;
    if (FAILED(gfx::Device()->CreateTexture2D(&td, nullptr, &tex))) return false;
    ComPtr<ID3D11RenderTargetView> rtv;
    gfx::Device()->CreateRenderTargetView(tex.Get(), nullptr, &rtv);
    ComPtr<IDXGISurface> surf;
    tex.As(&surf);
    ComPtr<ID2D1Bitmap1> target;
    auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), (float)s.dpi, (float)s.dpi);
    gfx::DC()->CreateBitmapFromDxgiSurface(surf.Get(), &props, &target);

    double now = gfx::Now();
    ApplyGridTuning();
    for (int i = 0; i < kSceneCount; i++)
        for (int moment = 0; moment < (plain ? 2 : 1); moment++)
        {
            g_scene = i;
            g_start = now - 30; g_end = now + 270;
            g_snapPrompt = (i * 5 + moment * 11) % kPromptCount;
            g_silenced = !plain && i == 5;
            g_soundToastAt = !plain && i == 5 ? now - 0.8 : -100; // Silk: show the "Sound off" toast
            if (i == 4) g_start = now - 3.5;                       // Lanterns: show the start hint
            if (plain) g_start = now - 5.0;                        // the start hint and a reminder together
            g_ch = Challenge{};
            if (!plain && (i == 6 || i == 7)) // show the early-exit card: a riddle on one scene, a calculation on another
            {
                g_ch.open = true;
                g_ch.alpha = 1.f;
                g_ch.solved = 1;
                g_ch.riddle = i == 6;
                g_ch.text = i == 6 ? kRiddles[kRiddleCount / 2].q : L"47 \x00D7 13";
                g_ch.typed = L"42";
                // Show the solution card: the riddle on Ripple, the calculation on Grid.
                g_ch.reveal = true;
                g_ch.revealTyped = L"42";
                g_ch.typed.clear();
                g_ch.answer = i == 6 ? kRiddles[kRiddleCount / 2].answer : 611;
                g_ch.why = i == 6 ? (kRiddles[kRiddleCount / 2].why ? kRiddles[kRiddleCount / 2].why : L"") : L"47 \x00D7 13 = 47 \x00D7 10 + 47 \x00D7 3 = 470 + 141 = 611.";
                g_ch.cooldownUntil = 0;
            }
            int sw = (int)(w * kScenes[i].resScale), sh = (int)(h * kScenes[i].resScale);
            s.scene.Reset();
            ResetProbe(&s);
            s.scene.Ensure(sw, sh, DXGI_FORMAT_R16G16B16A16_FLOAT, true);
            float t = kScenes[i].timeOffset + 30.f + moment * 65.f;
            gfx::DrawScene(i, s.scene.rtv.Get(), sw, sh, t, gfx::Breath(30.0 - 2.0 + moment * 3.0));
            gfx::Blit(s.scene.srv.Get(), rtv.Get(), w, h, 1.f, 0);
            s.inkSet = false; // measure this scene afresh, no easing from the previous one
            ProbeStart(&s, now);
            ProbeCollect(&s, true);
            auto dc = gfx::DC();
            dc->SetTarget(target.Get());
            dc->SetDpi((float)s.dpi, (float)s.dpi);
            dc->BeginDraw();
            DrawOverlay(&s, dc, now, 1.f);
            dc->EndDraw();
            dc->SetTarget(nullptr);
            std::wstring path = std::wstring(dir) + L"\\overlay" + std::to_wstring(i) + L"_" + kScenes[i].name +
                                (plain ? (moment ? L"_b" : L"_a") : L"") + L".png";
            gfx::SaveTexturePng(tex.Get(), path.c_str());
        }
    g_snapPrompt = -1;
    return true;
}

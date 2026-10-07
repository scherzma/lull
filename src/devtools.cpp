// Developer helpers: render scenes to PNG for visual checks; hook-chain self test.
#include "gfx.h"
#include <string>
#include <algorithm>
#include <atomic>
#include <cstdio>

// ---------------------------------------------------------------- hook-chain self test
// Verifies the assumption Lull's keyboard shield relies on: a low-level hook that calls
// CallNextHookEx first sees whether a hook further down the chain (Synergy etc.) swallowed
// the key. Uses F24, which nothing reacts to.
namespace
{
    std::atomic<bool> g_eat{ false };
    std::atomic<LRESULT> g_seen{ -1 };
    HHOOK g_hookA = nullptr, g_hookB = nullptr;

    LRESULT CALLBACK ProcA(int code, WPARAM wp, LPARAM lp) // stands in for Synergy
    {
        auto* k = (KBDLLHOOKSTRUCT*)lp;
        if (code == HC_ACTION && k->vkCode == VK_F24 && g_eat) return 1;
        return CallNextHookEx(nullptr, code, wp, lp);
    }

    LRESULT CALLBACK ProcB(int code, WPARAM wp, LPARAM lp) // Lull's pattern
    {
        LRESULT next = CallNextHookEx(nullptr, code, wp, lp);
        auto* k = (KBDLLHOOKSTRUCT*)lp;
        if (code == HC_ACTION && k->vkCode == VK_F24 && wp == WM_KEYDOWN) g_seen = next;
        return next;
    }

    DWORD WINAPI HookThreadFn(LPVOID which)
    {
        MSG m;
        PeekMessageW(&m, nullptr, 0, 0, PM_NOREMOVE);
        HHOOK h = SetWindowsHookExW(WH_KEYBOARD_LL, which ? ProcB : ProcA, GetModuleHandleW(nullptr), 0);
        (which ? g_hookB : g_hookA) = h;
        while (GetMessageW(&m, nullptr, 0, 0) > 0) {}
        UnhookWindowsHookEx(h);
        return 0;
    }

    void PressF24()
    {
        INPUT in[2]{};
        in[0].type = in[1].type = INPUT_KEYBOARD;
        in[0].ki.wVk = in[1].ki.wVk = VK_F24;
        in[1].ki.dwFlags = KEYEVENTF_KEYUP;
        SendInput(2, in, sizeof(INPUT));
        MSG m;
        DWORD until = GetTickCount() + 300;
        while (GetTickCount() < until) { while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&m); Sleep(10); }
    }
}

int DevHookSelfTest(const wchar_t* outPath)
{
    DWORD ta, tb;
    HANDLE a = CreateThread(nullptr, 0, HookThreadFn, (LPVOID)0, 0, &ta);
    Sleep(150);
    HANDLE b = CreateThread(nullptr, 0, HookThreadFn, (LPVOID)1, 0, &tb); // installed later -> called first
    Sleep(150);

    g_eat = true; g_seen = -1; PressF24();
    LRESULT whenEaten = g_seen;
    g_eat = false; g_seen = -1; PressF24();
    LRESULT whenPassed = g_seen;

    PostThreadMessageW(ta, WM_QUIT, 0, 0);
    PostThreadMessageW(tb, WM_QUIT, 0, 0);
    WaitForSingleObject(a, 1000);
    WaitForSingleObject(b, 1000);

    FILE* f = nullptr;
    _wfopen_s(&f, outPath, L"w");
    if (f)
    {
        fprintf(f, "hooks installed: A=%d B=%d\n", g_hookA != nullptr, g_hookB != nullptr);
        fprintf(f, "downstream hook swallowed key -> CallNextHookEx returned %lld (expect nonzero)\n", (long long)whenEaten);
        fprintf(f, "downstream hook passed key    -> CallNextHookEx returned %lld (expect 0)\n", (long long)whenPassed);
        fprintf(f, "RESULT: %s\n", (whenEaten != 0 && whenEaten != -1 && whenPassed == 0) ? "PASS" : "FAIL");
        fclose(f);
    }
    return (whenEaten != 0 && whenEaten != -1 && whenPassed == 0) ? 0 : 1;
}

// Simulates Grid for 20 minutes in several random environments and reports how often its waves
// break (in real seconds between surges) and how fast the clock runs.
int DevSurgeTest(const wchar_t* outPath)
{
    FILE* f = nullptr;
    _wfopen_s(&f, outPath, L"w");
    if (!f) return 1;
    const int fps = 10, minutes = 20;
    for (float surges : { 0.f, 0.15f, 0.3f, 0.6f, 1.f })
    {
        gfx::SetGridTuning(5, 0.54f, 1.f, surges);
        int total = 0;
        double fast = 0, minSpeed = 1e9, maxSpeed = 0;
        std::string lines;
        for (float seed : { 0.f, 7.31f, 19.7f, 33.3f, 52.9f })
        {
            gfx::SetGridSeed(seed);
            gfx::PrimeGridAt(200.0);
            double t0 = 1000.0, prev = -1;
            gfx::TickGrid(t0);
            bool on = false;
            int hits = 0;
            std::string gaps;
            for (int i = 1; i <= fps * 60 * minutes; i++)
            {
                double now = t0 + (double)i / fps, before = gfx::GridClock();
                gfx::TickGrid(now);
                double speed = (gfx::GridClock() - before) * fps;
                minSpeed = std::min(minSpeed, speed);
                maxSpeed = std::max(maxSpeed, speed);
                float level = gfx::GridSurgeLevel();
                if (level > 0.3f) fast += 1.0 / fps;
                if (!on && level > 0.5f)
                {
                    on = true;
                    char b[32];
                    sprintf_s(b, "%s%.0f", hits ? " " : "", prev < 0 ? now - t0 : now - prev);
                    gaps += b;
                    prev = now;
                    hits++;
                }
                if (on && level < 0.1f) on = false;
            }
            total += hits;
            char b[64];
            sprintf_s(b, "  seed %4.1f: %2d surges, s between: ", seed, hits);
            lines += b + gaps + "\n";
        }
        fprintf(f, "surges=%.2f: %.1f surges per 10 min on average, speed %.2fx .. %.2fx, rushing %.0f%% of the time\n%s",
                surges, total / 5.0 / (minutes / 10.0), minSpeed, maxSpeed, 100.0 * fast / (5 * 60.0 * minutes), lines.c_str());
    }
    fclose(f);
    return 0;
}

// Finds the biggest breaking wave in 20 minutes of Grid time (surges at maximum, a random
// environment) and renders the moments around it to PNGs, for checking how it looks.
int DevSurgeFrames(const wchar_t* dir)
{
    if (!gfx::Init()) return 2;
    const int w = 1280, h = 720;
    gfx::SetGridTuning(5, 0.54f, 1.f, 1.f);
    gfx::SetGridSeed(7.31f);
    double best = 300;
    float bestPeak = -1e9f;
    for (double t = 300; t < 1500; t += 0.5)
    {
        float p = gfx::GridPeakAt(t);
        if (p > bestPeak) { bestPeak = p; best = t; }
    }
    RenderTex out, scene;
    int sw = (int)(w * kScenes[kSceneGrid].resScale), sh = (int)(h * kScenes[kSceneGrid].resScale);
    if (!out.Ensure(w, h, DXGI_FORMAT_B8G8R8A8_UNORM, false) || !scene.Ensure(sw, sh, DXGI_FORMAT_R16G16B16A16_FLOAT, false)) return 3;
    FILE* f = nullptr;
    _wfopen_s(&f, (std::wstring(dir) + L"\\peaks.txt").c_str(), L"w");
    for (float dt : { -8.f, -4.f, -2.f, -1.f, 0.f, 1.f, 2.f, 4.f, 8.f })
    {
        gfx::PrimeGridAt(best + dt);
        if (f) fprintf(f, "t%+.0f: highest wave %.2f (breaks at %.2f), surge level %.2f\n", dt, gfx::GridPeakAt(best + dt),
                       gfx::GridBreakHeight(), gfx::GridSurgeLevel());
        gfx::DrawScene(kSceneGrid, scene.rtv.Get(), sw, sh, 0.f, 0.f);
        gfx::Blit(scene.srv.Get(), out.rtv.Get(), w, h, 1.f, 0);
        wchar_t name[64];
        swprintf_s(name, L"\\surge_%+05.1f.png", dt);
        if (!gfx::SaveTexturePng(out.tex.Get(), (std::wstring(dir) + name).c_str())) return 4;
    }
    if (f) fclose(f);
    return 0;
}

int DevRenderScenes(const wchar_t* dir, int w, int h, float t)
{
    if (!gfx::Init()) return 2;
    RenderTex out;
    if (!out.Ensure(w, h, DXGI_FORMAT_B8G8R8A8_UNORM, false)) return 3;
    for (int i = 0; i < kSceneCount; i++)
    {
        RenderTex scene; // same reduced-resolution path the lock screen uses
        int sw = (int)(w * kScenes[i].resScale), sh = (int)(h * kScenes[i].resScale);
        if (!scene.Ensure(sw, sh, DXGI_FORMAT_R16G16B16A16_FLOAT, false)) return 3;
        float tt = kScenes[i].timeOffset + t + (i == 3 ? 3.0f : 0.f); // Breathe: show it mid in-breath
        gfx::DrawScene(i, scene.rtv.Get(), sw, sh, tt, gfx::Breath(tt));
        gfx::Blit(scene.srv.Get(), out.rtv.Get(), w, h, 1.f, 0);
        std::wstring path = std::wstring(dir) + L"\\scene" + std::to_wstring(i) + L"_" + kScenes[i].name + L".png";
        if (!gfx::SaveTexturePng(out.tex.Get(), path.c_str())) return 4;
    }
    return 0;
}

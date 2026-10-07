// Lull — a quiet pause. Shared declarations.
#pragma once
#include "gfx.h"
#include <algorithm>
#include <string>

constexpr wchar_t kAppName[] = L"Lull";
constexpr UINT WM_APP_TRAY = WM_APP + 1;      // tray icon callback
constexpr UINT WM_APP_BEGIN = WM_APP + 2;     // wParam = seconds
constexpr UINT WM_APP_LULL_DONE = WM_APP + 3; // overlay finished

// ------------------------------------------------------------------ settings
struct Settings
{
    int minutes = 5;            // 1..15
    int scene = 0;              // 0..kSceneCount-1
    bool shuffle = false;
    bool showCountdown = true;
    bool reminders = true;      // gentle prompts / breathing cues
    bool chime = true;
    bool earlyExit = true;      // Esc opens a math challenge (3 in a row)
    bool muteSounds = false;    // silence other sound while a lull runs
    int gridSources = 5;        // Grid tuning (hidden: double-click the Grid preview):
    int gridIntensity = 50;     //   wave sources 1..8, intensity 0..100 (flares, fold density),
    int gridPace = 50;          //   pace 0..100 (50 = normal)
    int gridSurges = 0;         //   surges 0..100: how low the breaking height is (how often superwaves come)
    int gridLook = 0;           //   0 = night, 1 = day, 2 = follow Windows' app theme
    std::wstring mutedDevices;  // outputs we muted (restored on the next start after a crash)
    bool trayPromoted = false;  // we already pinned the icon once
    unsigned riddleSeed = 0;    // riddles are served in a shuffled order that survives restarts,
    int riddleCursor = 0;       // so none repeats until all have been shown
};
extern Settings g_settings;
void LoadSettings();
void SaveSettings();
bool GetStartWithWindows();
void SetStartWithWindows(bool on);

// ------------------------------------------------------------------ theme
struct Theme
{
    bool dark;
    D2D1_COLOR_F card, cardBorder, text, text2, text3, track, hover, pressed,
                 accent, accentHover, accentPressed, onAccent, toggleOff;
};
Theme CurrentTheme();
Theme MakeTheme(bool dark);
bool TaskbarIsLight();
bool AppsUseLightTheme();       // Windows' app mode (cached for a couple of seconds)

// ------------------------------------------------------------------ art
// Monochrome tray glyph (sunset over still water).
HICON CreateTrayIcon(int size, bool lightTaskbar);
// Colored logo, drawn with D2D at (x, y) with given size in DIPs.
void DrawLogo(ID2D1RenderTarget* rt, float x, float y, float size);
bool ExportIco(const wchar_t* path);

// ------------------------------------------------------------------ flyout
namespace flyout
{
    void Toggle(POINT anchor);
    void Show(POINT anchor);
    void Hide(bool instant = false);
    bool Visible();
    bool Animating();
    void Frame();               // render one frame if visible
    void ReleaseGraphics();
    bool SnapshotPng(const wchar_t* path, bool tuneSheet); // dev
}

// ------------------------------------------------------------------ overlay (the lull itself)
namespace overlay
{
    bool Begin(int seconds, int scene, bool dev = false);
    bool Active();
    void Frame();
    void ReleaseGraphics();
    bool SnapshotScenes(const wchar_t* dir, bool plain = false); // dev; plain = text only, two moments
    bool DevInkTest(const wchar_t* path);                         // dev: live ink measurement, logged
}

// ------------------------------------------------------------------ audio
namespace audio
{
    void MuteAll();            // mute every active output that isn't muted yet
    void RestoreAll();         // unmute exactly those (also recovers after a crash)
    void RestoreFromThread();  // same, from a thread with its own COM apartment
}

// ------------------------------------------------------------------ misc
void PlayChime();
void ShowTrayMenu(HWND owner, POINT pt);
void BeginLull(int seconds);   // uses current settings
extern HWND g_mainWnd;
extern HINSTANCE g_hinst;

inline D2D1_COLOR_F Rgba(uint32_t rgb, float a = 1.f)
{
    return D2D1::ColorF(((rgb >> 16) & 255) / 255.f, ((rgb >> 8) & 255) / 255.f, (rgb & 255) / 255.f, a);
}
inline float Lerp(float a, float b, float t) { return a + (b - a) * t; }

// Pushes the saved Grid tuning to the renderer. Pace 0..100 maps to 0.5x..1x..1.8x speed.
inline void ApplyGridTuning()
{
    float s = g_settings.gridPace / 100.f;
    float pace = s < 0.5f ? Lerp(0.5f, 1.f, s * 2) : Lerp(1.f, 1.8f, (s - 0.5f) * 2);
    gfx::SetGridTuning(g_settings.gridSources, g_settings.gridIntensity / 100.f, pace, g_settings.gridSurges / 100.f);
    int look = g_settings.gridLook;
    gfx::SetGridDay(look == 1 || (look == 2 && AppsUseLightTheme()) ? 1.f : 0.f);
}
inline float Clamp01(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }
inline float EaseOutCubic(float t) { t = Clamp01(t); float u = 1 - t; return 1 - u * u * u; }
inline float EaseInOut(float t) { t = Clamp01(t); return t * t * (3 - 2 * t); }

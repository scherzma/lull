#pragma once
#include <windows.h>
#include <d3d11_1.h>
#include <dxgi1_3.h>
#include <d2d1_1.h>
#include <d2d1_1helper.h>
#include <dwrite.h>
#include <dcomp.h>
#include <wrl/client.h>
#include <cstdint>

using Microsoft::WRL::ComPtr;

constexpr int kSceneCount = 8;
constexpr int kSceneBreathe = 3;
constexpr int kSceneGrid = 7;

struct SceneInfo
{
    const wchar_t* name;
    bool lightCenter;   // background behind centered text is light -> use dark ink
    bool lightBottom;   // background behind the countdown is light -> use dark ink
    float timeOffset;   // where the scene "starts" so it looks settled immediately
    float resScale;     // render scale on the lock screen (soft scenes can render at half res)
    float textSpot;     // how much a busy background needs a soft shade behind text (0 = none)
};
extern const SceneInfo kScenes[kSceneCount];

// Offscreen render target (optionally also usable as a D2D bitmap).
struct RenderTex
{
    ComPtr<ID3D11Texture2D> tex;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID2D1Bitmap1> bmp;
    int w = 0, h = 0;
    bool Ensure(int w, int h, DXGI_FORMAT fmt, bool d2d);
    void Reset();
};

// Swap chain presented through DirectComposition (per-pixel alpha window).
struct CompSurface
{
    HWND hwnd = nullptr;
    ComPtr<IDXGISwapChain1> swap;
    ComPtr<IDCompositionTarget> target;
    ComPtr<IDCompositionVisual> visual;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID2D1Bitmap1> bmp;
    int w = 0, h = 0;
    bool Create(HWND hwnd, int w, int h);
    bool Resize(int w, int h);
    HRESULT Present(UINT sync);
    void Reset();
private:
    bool CreateViews();
};

namespace gfx
{
    // Device-independent factories (always available, cheap).
    ID2D1Factory1* D2DFactory();
    IDWriteFactory* DWrite();

    // Device-dependent resources; created lazily, released when idle.
    bool Init();
    void Shutdown();
    bool Ready();
    void MarkLost();
    bool IsLost();

    ID3D11Device* Device();
    ID3D11DeviceContext* Ctx();
    ID2D1DeviceContext* DC();
    ID2D1DeviceContext* DC2(); // for recording command lists while DC() is mid-draw
    IDCompositionDevice* DComp();

    // Grid tuning: number of wave sources (1..8), intensity (0..1), pace multiplier,
    // surges (0..1): how low the breaking height is, i.e. how often the waves pile up into a superwave.
    void SetGridTuning(int sources, float intensity, float pace, float surges);
    void NewGridEnvironment();     // random layout and phases for the singularities
    void SetGridSeed(float seed);  // 0 = the classic layout
    void SetGridDay(float day);    // 0 = night look, 1 = day look (eased over ~1 s while ticking)
    float GridDay();
    // Advances Grid's own clock (call once per rendered frame); it rushes while a giant breaks.
    void TickGrid(double now);
    double GridClock();
    float GridSurgeLevel();        // 0 calm .. 1 while the highest wave on screen has broken
    float GridPeakAt(double t);    // the highest wave on a 16:9 screen at Grid time t
    float GridBreakHeight();       // 0 = surges off
    void PrimeGridAt(double t);    // dev: jump Grid's clock to t (DrawScene then renders that moment)

    // originX/Y + normH reframe the scene (0 = centred, scaled to the canvas height).
    void DrawScene(int scene, ID3D11RenderTargetView* rtv, int w, int h, float time, float breath,
                   float originX = 0.f, float originY = 0.f, float normH = 0.f);
    void Blit(ID3D11ShaderResourceView* src, ID3D11RenderTargetView* dst, int w, int h, float opacity, uint32_t frame);

    bool SaveTexturePng(ID3D11Texture2D* bgra, const wchar_t* path);

    // Breathing rhythm: in 4s, hold 1.5s, out 6s, rest 1s.
    // Returns 0..1 (lung fullness); phase 0=in 1=hold 2=out 3=rest.
    float Breath(double t, int* phase = nullptr, float* phaseProgress = nullptr);

    double Now(); // seconds, monotonic
    void Log(const wchar_t* fmt, ...); // writes %TEMP%\lull-debug.log when LULL_DEBUG is set
}

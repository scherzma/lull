#include "gfx.h"
#include <wincodec.h>
#include <algorithm>
#include <random>
#include <cmath>
#include <cstdio>
#include <cstdarg>

#include "shaders/vs_full.h"
#include "shaders/ps_blit.h"
#include "shaders/ps_scene0.h"
#include "shaders/ps_scene1.h"
#include "shaders/ps_scene2.h"
#include "shaders/ps_scene3.h"
#include "shaders/ps_scene4.h"
#include "shaders/ps_scene5.h"
#include "shaders/ps_scene6.h"
#include "shaders/ps_scene7.h"
#include "shaders/ps_gridfield.h"

const SceneInfo kScenes[kSceneCount] = {
    { L"Aurora",   false, false,  40.f, 0.5f, 0.0f },
    { L"Drift",    true,  true,   20.f, 0.5f, 0.35f },
    { L"Tide",     true,  false,  10.f, 0.5f, 0.0f },
    { L"Breathe",  false, false,   0.f, 0.5f, 0.0f },
    { L"Lanterns", false, false,  60.f, 0.5f, 0.4f },
    { L"Silk",     true,  true,   30.f, 0.5f, 0.35f },
    { L"Ripple",   false, false,  12.f, 1.0f, 1.0f },
    { L"Grid",     false, false,  20.f, 1.0f, 1.0f },
};

namespace
{
    struct Params
    {
        float res[2];
        float time;
        float breath;
        float opacity;
        float frame;
        float seed;
        float pad;
        float origin[2];
        float normH;
        float pad2;
        float pad3[4]; // Grid: breaking height, surge level, day look, unused
    };

    ComPtr<ID2D1Factory1> g_d2dFactory;
    ComPtr<IDWriteFactory> g_dwrite;

    ComPtr<ID3D11Device> g_dev;
    ComPtr<ID3D11DeviceContext> g_ctx;
    ComPtr<IDXGIDevice1> g_dxgiDev;
    ComPtr<IDXGIFactory2> g_factory;
    ComPtr<ID2D1Device> g_d2dDev;
    ComPtr<ID2D1DeviceContext> g_dc;
    ComPtr<ID2D1DeviceContext> g_dc2; // records command lists while g_dc is drawing
    ComPtr<IDCompositionDevice> g_dcomp;

    ComPtr<ID3D11VertexShader> g_vs;
    ComPtr<ID3D11PixelShader> g_psBlit;
    ComPtr<ID3D11PixelShader> g_psScene[kSceneCount];
    ComPtr<ID3D11PixelShader> g_psGridField;

    // Grid computes its field once per square into a small texture; a few sizes are kept around
    // (lock screen, panel preview, panel backdrop).
    struct FieldTex { RenderTex rt; int w = 0, h = 0; unsigned long long used = 0; };
    FieldTex g_fieldTex[4];
    unsigned long long g_fieldClock = 0;
    float g_gridSources = 5, g_gridIntensity = 0.5f, g_gridPace = 1.f, g_gridSurges = 0.f;
    double g_gridClock = 100.0, g_gridLastTick = 0;
    float g_surgeLevel = 0;
    float g_gridSeed = 0; // Grid's random environment (see TickGrid)
    float g_gridDay = 0, g_gridDayTarget = 0; // Grid's look: 0 night .. 1 day, eased when it changes
    float BreakHeight();
    ComPtr<ID3D11Buffer> g_cb;
    ComPtr<ID3D11SamplerState> g_samp;
    bool g_lost = false;

    void SetCommonState(ID3D11RenderTargetView* rtv, int w, int h, const Params& p)
    {
        g_ctx->UpdateSubresource(g_cb.Get(), 0, nullptr, &p, 0, 0);
        D3D11_VIEWPORT vp{ 0, 0, (float)w, (float)h, 0, 1 };
        g_ctx->OMSetRenderTargets(1, &rtv, nullptr);
        g_ctx->RSSetViewports(1, &vp);
        g_ctx->RSSetState(nullptr);
        g_ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
        g_ctx->OMSetDepthStencilState(nullptr, 0);
        g_ctx->IASetInputLayout(nullptr);
        g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        g_ctx->VSSetShader(g_vs.Get(), nullptr, 0);
        g_ctx->PSSetConstantBuffers(0, 1, g_cb.GetAddressOf());
    }

    void Unbind()
    {
        ID3D11RenderTargetView* nullRtv = nullptr;
        ID3D11ShaderResourceView* nullSrv[2] = {};
        g_ctx->OMSetRenderTargets(1, &nullRtv, nullptr);
        g_ctx->PSSetShaderResources(0, 2, nullSrv);
    }
}

ID2D1Factory1* gfx::D2DFactory()
{
    if (!g_d2dFactory)
    {
        D2D1_FACTORY_OPTIONS opts{};
        D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &opts,
                          reinterpret_cast<void**>(g_d2dFactory.GetAddressOf()));
    }
    return g_d2dFactory.Get();
}

IDWriteFactory* gfx::DWrite()
{
    if (!g_dwrite)
        DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                            reinterpret_cast<IUnknown**>(g_dwrite.GetAddressOf()));
    return g_dwrite.Get();
}

bool gfx::Init()
{
    if (g_dev) return true;
    g_lost = false;

    D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, 2,
                                   D3D11_SDK_VERSION, &g_dev, nullptr, &g_ctx);
    if (FAILED(hr))
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, levels, 2,
                               D3D11_SDK_VERSION, &g_dev, nullptr, &g_ctx);
    if (FAILED(hr)) return false;

    g_dev.As(&g_dxgiDev);
    g_dxgiDev->SetMaximumFrameLatency(1);
    {
        ComPtr<IDXGIAdapter> adapter;
        g_dxgiDev->GetAdapter(&adapter);
        adapter->GetParent(IID_PPV_ARGS(&g_factory));
    }
    if (!g_factory) { Shutdown(); return false; }

    if (FAILED(D2DFactory()->CreateDevice(g_dxgiDev.Get(), &g_d2dDev)) ||
        FAILED(g_d2dDev->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &g_dc)))
    {
        Shutdown();
        return false;
    }
    g_dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    g_d2dDev->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &g_dc2);
    if (g_dc2) g_dc2->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);

    if (FAILED(DCompositionCreateDevice(g_dxgiDev.Get(), IID_PPV_ARGS(&g_dcomp)))) { Shutdown(); return false; }

    g_dev->CreateVertexShader(g_vs_full, sizeof(g_vs_full), nullptr, &g_vs);
    g_dev->CreatePixelShader(g_ps_blit, sizeof(g_ps_blit), nullptr, &g_psBlit);
    const struct { const BYTE* p; size_t n; } scenes[kSceneCount] = {
        { g_ps_scene0, sizeof(g_ps_scene0) }, { g_ps_scene1, sizeof(g_ps_scene1) },
        { g_ps_scene2, sizeof(g_ps_scene2) }, { g_ps_scene3, sizeof(g_ps_scene3) },
        { g_ps_scene4, sizeof(g_ps_scene4) }, { g_ps_scene5, sizeof(g_ps_scene5) },
        { g_ps_scene6, sizeof(g_ps_scene6) }, { g_ps_scene7, sizeof(g_ps_scene7) },
    };
    for (int i = 0; i < kSceneCount; i++)
        g_dev->CreatePixelShader(scenes[i].p, scenes[i].n, nullptr, &g_psScene[i]);
    g_dev->CreatePixelShader(g_ps_gridfield, sizeof(g_ps_gridfield), nullptr, &g_psGridField);

    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = sizeof(Params);
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    g_dev->CreateBuffer(&bd, nullptr, &g_cb);

    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    g_dev->CreateSamplerState(&sd, &g_samp);
    return true;
}

void gfx::Log(const wchar_t* fmt, ...)
{
    static int enabled = -1;
    if (enabled < 0) enabled = GetEnvironmentVariableW(L"LULL_DEBUG", nullptr, 0) > 0;
    if (!enabled) return;
    wchar_t msg[512];
    va_list ap;
    va_start(ap, fmt);
    vswprintf_s(msg, fmt, ap);
    va_end(ap);
    wchar_t path[MAX_PATH];
    GetTempPathW(MAX_PATH, path);
    wcscat_s(path, L"lull-debug.log");
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"a, ccs=UTF-8") == 0 && f)
    {
        fwprintf(f, L"%.3f %s\n", Now(), msg);
        fclose(f);
    }
}

void gfx::Shutdown()
{
    if (g_dev)
    {
        g_dev->AddRef();
        Log(L"shutdown: device refs before release = %lu", g_dev->Release());
    }
    if (g_ctx) { g_ctx->ClearState(); g_ctx->Flush(); }
    g_samp.Reset(); g_cb.Reset();
    for (auto& ps : g_psScene) ps.Reset();
    g_psGridField.Reset();
    for (auto& f : g_fieldTex) { f.rt.Reset(); f.w = f.h = 0; }
    g_psBlit.Reset(); g_vs.Reset();
    g_dcomp.Reset(); g_dc2.Reset(); g_dc.Reset(); g_d2dDev.Reset();
    g_factory.Reset(); g_dxgiDev.Reset(); g_ctx.Reset();
    if (g_dev)
    {
        ID3D11Device* raw = g_dev.Detach();
        ULONG left = raw->Release();
        Log(L"shutdown: device refs left = %lu (0 = freed)", left);
    }
}

bool gfx::Ready() { return g_dev != nullptr && !g_lost; }
void gfx::MarkLost() { g_lost = true; }
bool gfx::IsLost() { return g_lost; }
ID3D11Device* gfx::Device() { return g_dev.Get(); }
ID3D11DeviceContext* gfx::Ctx() { return g_ctx.Get(); }
ID2D1DeviceContext* gfx::DC() { return g_dc.Get(); }
ID2D1DeviceContext* gfx::DC2() { return g_dc2.Get(); }
IDCompositionDevice* gfx::DComp() { return g_dcomp.Get(); }

void gfx::DrawScene(int scene, ID3D11RenderTargetView* rtv, int w, int h, float time, float breath,
                     float originX, float originY, float normH)
{
    if (scene < 0 || scene >= kSceneCount) scene = 0;
    Params p{ { (float)w, (float)h }, time, breath, 1.f, 0.f, 0.f, 0.f, { originX, originY }, normH, 0.f };

    ID3D11ShaderResourceView* field = nullptr;
    if (scene == kSceneGrid)
    {
        p.seed = g_gridSources;
        p.pad = g_gridIntensity;
        p.pad2 = g_gridSeed;
        p.pad3[0] = BreakHeight();
        p.pad3[1] = g_surgeLevel; // the whole surface folds harder while a giant breaks
        p.pad3[2] = g_gridDay;
        if (g_gridLastTick > 0) p.time = (float)g_gridClock;
        // Pass 1: evaluate the (expensive) 3D field once per square, plus a one-square border.
        float nh = normH > 0 ? normH : (float)h;
        float cellPx = std::max(3.f, normH > 0 ? nh / 72.f : floorf(nh / 72.f));
        int fw = (int)ceilf(w / cellPx) + 2, fh = (int)ceilf(h / cellPx) + 2;
        FieldTex* ft = nullptr;
        for (auto& f : g_fieldTex) if (f.w == fw && f.h == fh) ft = &f;
        if (!ft)
        {
            ft = &g_fieldTex[0];
            for (auto& f : g_fieldTex) if (f.used < ft->used) ft = &f;
            ft->rt.Reset();
            if (!ft->rt.Ensure(fw, fh, DXGI_FORMAT_R16G16B16A16_FLOAT, false)) return;
            ft->w = fw; ft->h = fh;
        }
        ft->used = ++g_fieldClock;
        SetCommonState(ft->rt.rtv.Get(), fw, fh, p);
        g_ctx->PSSetShader(g_psGridField.Get(), nullptr, 0);
        g_ctx->Draw(3, 0);
        Unbind();
        field = ft->rt.srv.Get();
    }

    SetCommonState(rtv, w, h, p);
    g_ctx->PSSetShader(g_psScene[scene].Get(), nullptr, 0);
    if (field) g_ctx->PSSetShaderResources(1, 1, &field);
    g_ctx->Draw(3, 0);
    Unbind();
}

void gfx::Blit(ID3D11ShaderResourceView* src, ID3D11RenderTargetView* dst, int w, int h, float opacity, uint32_t frame)
{
    Params p{ { (float)w, (float)h }, 0.f, 0.f, opacity, (float)(frame % 4096), 0.f, 0.f, { 0.f, 0.f }, 0.f, 0.f };
    SetCommonState(dst, w, h, p);
    g_ctx->PSSetShader(g_psBlit.Get(), nullptr, 0);
    g_ctx->PSSetShaderResources(0, 1, &src);
    g_ctx->PSSetSamplers(0, 1, g_samp.GetAddressOf());
    g_ctx->Draw(3, 0);
    Unbind();
}

float gfx::Breath(double t, int* phase, float* pp)
{
    const double in = 4.0, hold = 1.5, out = 6.0, rest = 1.0;
    double x = fmod(t, in + hold + out + rest);
    auto ease = [](double v) { return 0.5 - 0.5 * cos(v * 3.14159265358979); };
    int ph; double prog; double v;
    if (x < in) { ph = 0; prog = x / in; v = ease(prog); }
    else if ((x -= in) < hold) { ph = 1; prog = x / hold; v = 1.0; }
    else if ((x -= hold) < out) { ph = 2; prog = x / out; v = 1.0 - ease(prog); }
    else { x -= out; ph = 3; prog = x / rest; v = 0.0; }
    if (phase) *phase = ph;
    if (pp) *pp = (float)prog;
    return (float)v;
}

double gfx::Now()
{
    static LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)freq.QuadPart;
}

// ------------------------------------------------------------------ RenderTex

bool RenderTex::Ensure(int nw, int nh, DXGI_FORMAT fmt, bool d2d)
{
    nw = nw < 1 ? 1 : nw;
    nh = nh < 1 ? 1 : nh;
    if (tex && w == nw && h == nh) return true;
    Reset();
    D3D11_TEXTURE2D_DESC td{};
    td.Width = nw; td.Height = nh; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = fmt; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    auto dev = gfx::Device();
    if (FAILED(dev->CreateTexture2D(&td, nullptr, &tex))) return false;
    dev->CreateRenderTargetView(tex.Get(), nullptr, &rtv);
    dev->CreateShaderResourceView(tex.Get(), nullptr, &srv);
    if (d2d)
    {
        ComPtr<IDXGISurface> surf;
        tex.As(&surf);
        auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE, D2D1::PixelFormat(fmt, D2D1_ALPHA_MODE_PREMULTIPLIED));
        gfx::DC()->CreateBitmapFromDxgiSurface(surf.Get(), &props, &bmp);
    }
    w = nw; h = nh;
    return true;
}

void RenderTex::Reset()
{
    bmp.Reset(); srv.Reset(); rtv.Reset(); tex.Reset();
    w = h = 0;
}

// ------------------------------------------------------------------ CompSurface

bool CompSurface::Create(HWND wnd, int nw, int nh)
{
    Reset();
    hwnd = wnd;
    w = nw < 1 ? 1 : nw;
    h = nh < 1 ? 1 : nh;

    ComPtr<IDXGIFactory2> factory;
    {
        ComPtr<IDXGIDevice> dxgi;
        gfx::Device()->QueryInterface(IID_PPV_ARGS(&dxgi));
        ComPtr<IDXGIAdapter> adapter;
        dxgi->GetAdapter(&adapter);
        adapter->GetParent(IID_PPV_ARGS(&factory));
    }
    DXGI_SWAP_CHAIN_DESC1 d{};
    d.Width = w; d.Height = h;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    d.BufferCount = 2;
    d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    d.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
    d.Scaling = DXGI_SCALING_STRETCH;
    if (FAILED(factory->CreateSwapChainForComposition(gfx::Device(), &d, nullptr, &swap))) return false;

    auto dcomp = gfx::DComp();
    if (FAILED(dcomp->CreateTargetForHwnd(hwnd, TRUE, &target))) return false;
    dcomp->CreateVisual(&visual);
    visual->SetContent(swap.Get());
    target->SetRoot(visual.Get());
    dcomp->Commit();
    return CreateViews();
}

bool CompSurface::CreateViews()
{
    ComPtr<ID3D11Texture2D> back;
    if (FAILED(swap->GetBuffer(0, IID_PPV_ARGS(&back)))) return false;
    gfx::Device()->CreateRenderTargetView(back.Get(), nullptr, &rtv);
    ComPtr<IDXGISurface> surf;
    back.As(&surf);
    auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    return SUCCEEDED(gfx::DC()->CreateBitmapFromDxgiSurface(surf.Get(), &props, &bmp));
}

bool CompSurface::Resize(int nw, int nh)
{
    nw = nw < 1 ? 1 : nw;
    nh = nh < 1 ? 1 : nh;
    if (!swap) return false;
    if (nw == w && nh == h) return true;
    gfx::DC()->SetTarget(nullptr);
    rtv.Reset(); bmp.Reset();
    gfx::Ctx()->ClearState();
    if (FAILED(swap->ResizeBuffers(2, nw, nh, DXGI_FORMAT_UNKNOWN, 0))) return false;
    w = nw; h = nh;
    return CreateViews();
}

HRESULT CompSurface::Present(UINT sync)
{
    if (!swap) return E_FAIL;
    HRESULT hr = swap->Present(sync, 0);
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) gfx::MarkLost();
    return hr;
}

void CompSurface::Reset()
{
    if (gfx::DC()) gfx::DC()->SetTarget(nullptr);
    bmp.Reset(); rtv.Reset();
    if (target) target->SetRoot(nullptr);
    visual.Reset(); target.Reset(); swap.Reset();
    if (gfx::DComp()) gfx::DComp()->Commit();
    w = h = 0;
}

// ------------------------------------------------------------------ PNG

bool gfx::SaveTexturePng(ID3D11Texture2D* src, const wchar_t* path)
{
    D3D11_TEXTURE2D_DESC td;
    src->GetDesc(&td);
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    td.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(g_dev->CreateTexture2D(&td, nullptr, &staging))) return false;
    g_ctx->CopyResource(staging.Get(), src);
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(g_ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) return false;

    bool ok = false;
    ComPtr<IWICImagingFactory> wic;
    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))))
    {
        ComPtr<IWICStream> stream;
        ComPtr<IWICBitmapEncoder> enc;
        ComPtr<IWICBitmapFrameEncode> frame;
        if (SUCCEEDED(wic->CreateStream(&stream)) &&
            SUCCEEDED(stream->InitializeFromFilename(path, GENERIC_WRITE)) &&
            SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) &&
            SUCCEEDED(enc->Initialize(stream.Get(), WICBitmapEncoderNoCache)) &&
            SUCCEEDED(enc->CreateNewFrame(&frame, nullptr)) &&
            SUCCEEDED(frame->Initialize(nullptr)))
        {
            frame->SetSize(td.Width, td.Height);
            WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppPBGRA;
            frame->SetPixelFormat(&fmt);
            ok = SUCCEEDED(frame->WritePixels(td.Height, m.RowPitch, m.RowPitch * td.Height, (BYTE*)m.pData)) &&
                 SUCCEEDED(frame->Commit()) && SUCCEEDED(enc->Commit());
        }
    }
    g_ctx->Unmap(staging.Get(), 0);
    return ok;
}

void gfx::SetGridTuning(int sources, float intensity, float pace, float surges)
{
    g_gridSources = (float)std::clamp(sources, 1, 8);
    g_gridIntensity = std::clamp(intensity, 0.f, 1.f);
    g_gridPace = std::clamp(pace, 0.2f, 3.f);
    g_gridSurges = std::clamp(surges, 0.f, 1.f);
}

// Superwaves are not a separate thing and nothing schedules them. They are Grid's own field
// (the same waves you always see) in the rare moments when several singularities' crests arrive
// at one spot together, usually during a flare, and pile up past the breaking height. The
// screen's highest wave is usually ~1.6·√N/2 high, reaches ~1.6·√N in 1% of moments, and goes
// past that rarely and unpredictably. Every motion in it is a sum of rhythms that never line up
// exactly, so such moments keep recurring, but never on a beat. Each lull starts from a random
// environment (gridSeed); seed 0 is the classic layout.
// The helpers below mirror gridSource/gridSlice/gridVolume in scenes.hlsl, so Grid's clock can
// follow the highest wave on screen: slow in calm stretches, rushing while a giant breaks.
namespace
{
    float Fr(float v) { return v - floorf(v); }
    float Hash11(float p) { p = Fr(p * .1031f); p *= p + 33.33f; p *= p + p; return Fr(p); }
    void Hash21(float p, float& ox, float& oy)
    {
        float x = Fr(p * .1031f), y = Fr(p * .1030f), z = Fr(p * .0973f);
        float d = x * (y + 33.33f) + y * (z + 33.33f) + z * (x + 33.33f);
        x += d; y += d; z += d;
        ox = Fr((x + y) * z);
        oy = Fr((x + z) * y);
    }

    float Mod289(float x) { return x - floorf(x * (1.f / 289.f)) * 289.f; }
    float Permute(float x) { return Mod289((x * 34.f + 1.f) * x); }
    // 3D simplex noise, as snoise3 in scenes.hlsl (Ashima Arts / Stefan Gustavson, MIT).
    float SNoise3(float vx, float vy, float vz)
    {
        const float C1 = 1.f / 6.f, C2 = 1.f / 3.f, n_ = 0.142857142857f;
        float s = (vx + vy + vz) * C2;
        float ix = floorf(vx + s), iy = floorf(vy + s), iz = floorf(vz + s);
        float t = (ix + iy + iz) * C1;
        float x0[3] = { vx - ix + t, vy - iy + t, vz - iz + t };
        float g[3] = { x0[0] >= x0[1] ? 1.f : 0.f, x0[1] >= x0[2] ? 1.f : 0.f, x0[2] >= x0[0] ? 1.f : 0.f };
        float i1[3] = { std::min(g[0], 1 - g[2]), std::min(g[1], 1 - g[0]), std::min(g[2], 1 - g[1]) };
        float i2[3] = { std::max(g[0], 1 - g[2]), std::max(g[1], 1 - g[0]), std::max(g[2], 1 - g[1]) };
        float X[4][3], off[4][3] = { { 0, 0, 0 }, { i1[0], i1[1], i1[2] }, { i2[0], i2[1], i2[2] }, { 1, 1, 1 } };
        for (int a = 0; a < 3; a++)
        {
            X[0][a] = x0[a];
            X[1][a] = x0[a] - i1[a] + C1;
            X[2][a] = x0[a] - i2[a] + C2;
            X[3][a] = x0[a] - 0.5f;
        }
        ix = Mod289(ix); iy = Mod289(iy); iz = Mod289(iz);
        float nsx = 2 * n_, nsy = 0.5f * n_ - 1, nsz = n_, sum = 0;
        for (int c = 0; c < 4; c++)
        {
            float p = Permute(Permute(Permute(iz + off[c][2]) + iy + off[c][1]) + ix + off[c][0]);
            float j = p - 49.f * floorf(p * nsz * nsz);
            float xq = floorf(j * nsz), yq = floorf(j - 7.f * xq);
            float gx = xq * nsx + nsy, gy = yq * nsx + nsy, gz = 1.f - fabsf(gx) - fabsf(gy);
            if (gz <= 0) { gx -= floorf(gx) * 2 + 1; gy -= floorf(gy) * 2 + 1; }
            float norm = 1.79284291400159f - 0.85373472095314f * (gx * gx + gy * gy + gz * gz);
            float m = std::max(0.6f - (X[c][0] * X[c][0] + X[c][1] * X[c][1] + X[c][2] * X[c][2]), 0.f);
            m *= m;
            sum += m * m * norm * (gx * X[c][0] + gy * X[c][1] + gz * X[c][2]);
        }
        return 42.f * sum;
    }

    // The height at which Grid's waves break (0 = never). Peaks scale with √N; lower = more often.
    float BreakScale() { return std::max(0.5f, sqrtf(g_gridSources / 5.f)); }
    float BreakHeight() { return g_gridSurges <= 0.001f ? 0.f : (3.5f - 0.9f * g_gridSurges) * BreakScale(); }

    // The highest wave on a 16:9 screen at Grid time t (a 32×18 sample of the field).
    float GridScreenPeak(double t)
    {
        const int n = (int)g_gridSources;
        const float I = g_gridIntensity, sd = g_gridSeed, aspect = 16.f / 9.f;
        float sx[8], sy[8], sz[8], amp[8];
        for (int k = 0; k < n; k++)
        {
            float fk = (float)k, hx, hy;
            Hash21(fk * 3.7f + 1.f + sd * 13.1f, hx, hy);
            sx[k] = (hx - 0.5f) * 1.7f + 0.28f * (float)sin(t * (0.025 + 0.008 * fk) + fk + sd * 3.1f);
            sy[k] = (hy - 0.5f) * 1.1f + 0.28f * (float)cos(t * (0.02 + 0.007 * fk) + 2 * fk + sd * 5.3f);
            sz[k] = (Hash11(fk * 5.3f + 2.f + sd * 7.7f) - 0.5f) * 0.9f + 0.28f * (float)sin(t * 0.016 + 3 * fk + sd * 2.3f);
            float f = std::clamp(0.5f + 0.5f * (float)sin(t * (0.05 + 0.013 * fk) + fk * 2.1f + sd * 1.9f), 0.f, 1.f);
            amp[k] = (0.8f + 0.5f * I) * (1.f + I * 2.4f * powf(f, 10.f));
        }
        float ry = 0.35f * (float)sin(t * 0.0075), rx = 0.45f * (float)sin(t * 0.0057 + 1.0), rz = (float)(t * 0.0035);
        float cz = cosf(rz), szn = sinf(rz), cx = cosf(rx), sxn = sinf(rx), cy = cosf(ry), syn = sinf(ry);
        float z0 = 0.22f * (float)sin(t * 0.011), nA = 0.35f + 0.6f * I, best = -1e9f;
        for (int j = 0; j < 18; j++)
            for (int i = 0; i < 32; i++)
            {
                // gridSlice
                float x = ((i + 0.5f) / 32.f - 0.5f) * aspect * 1.1f, y = ((j + 0.5f) / 18.f - 0.5f) * 1.1f, z = z0, u;
                u = x * cz - y * szn; y = x * szn + y * cz; x = u;
                u = y * cx - z * sxn; z = y * sxn + z * cx; y = u;
                u = x * cy + z * syn; z = -x * syn + z * cy; x = u;
                // gridVolume
                float F = 0;
                for (int k = 0; k < n; k++)
                {
                    float dx = x - sx[k], dy = y - sy[k], dz = z - sz[k], d = sqrtf(dx * dx + dy * dy + dz * dz);
                    F += amp[k] * (float)sin(d * (8.f + 1.6f * k) - t * (0.22 + 0.04 * k) + k * 1.7f + sd * 4.7f) / (1.f + 2.2f * d);
                }
                F += nA * SNoise3(x * 1.35f + sd * 17.3f, y * 1.35f + sd * 11.1f, z * 1.35f + (float)(t * 0.02));
                best = std::max(best, F);
            }
        return best;
    }
}

void gfx::NewGridEnvironment()
{
    static std::mt19937 rng{ std::random_device{}() };
    g_gridSeed = std::uniform_real_distribution<float>(1.f, 64.f)(rng);
}
void gfx::SetGridSeed(float seed) { g_gridSeed = seed; }
void gfx::SetGridDay(float day)
{
    g_gridDayTarget = std::clamp(day, 0.f, 1.f);
    if (g_gridLastTick <= 0) g_gridDay = g_gridDayTarget; // nothing ticking (dev renders): no fade
}
float gfx::GridDay() { return g_gridDay; }

namespace
{
    // Rises as the highest wave nears the breaking height, full once it has broken.
    float SurgeLevelAt(double t)
    {
        float T = BreakHeight();
        if (T <= 0) return 0;
        float b = BreakScale();
        float x = std::clamp((GridScreenPeak(t) - (T - 0.3f * b)) / (0.9f * b), 0.f, 1.f);
        return x * x * (3 - 2 * x);
    }
}

void gfx::TickGrid(double now)
{
    double dt = g_gridLastTick > 0 ? std::clamp(now - g_gridLastTick, 0.0, 0.1) : 0.0;
    g_gridLastTick = now;
    float s = g_gridSurges;
    g_gridDay += (g_gridDayTarget - g_gridDay) * (float)(1.0 - exp(-dt * 3.0));
    g_surgeLevel = SurgeLevelAt(g_gridClock);
    double calm = 1.0 - 0.4 * s, rush = 1.0 + 2.5 * sqrt(s);
    g_gridClock += dt * g_gridPace * (calm + (rush - calm) * g_surgeLevel);
}
void gfx::PrimeGridAt(double t)
{
    g_gridClock = t;
    g_gridLastTick = 1;
    g_surgeLevel = SurgeLevelAt(t);
}
double gfx::GridClock() { return g_gridClock; }
float gfx::GridSurgeLevel() { return g_surgeLevel; }
float gfx::GridPeakAt(double t) { return GridScreenPeak(t); }
float gfx::GridBreakHeight() { return BreakHeight(); }

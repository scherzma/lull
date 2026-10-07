// Tray glyph, colored logo, .ico export. All drawn with Direct2D so it is crisp at any DPI.
#include "app.h"
#include <wincodec.h>
#include <vector>
#include <cmath>

static ComPtr<IWICImagingFactory> Wic()
{
    static ComPtr<IWICImagingFactory> wic;
    if (!wic) CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
    return wic;
}

static void FillHalfDisc(ID2D1RenderTarget* rt, float cx, float cy, float r, ID2D1Brush* brush)
{
    ComPtr<ID2D1PathGeometry> geo;
    gfx::D2DFactory()->CreatePathGeometry(&geo);
    ComPtr<ID2D1GeometrySink> sink;
    geo->Open(&sink);
    sink->BeginFigure(D2D1::Point2F(cx - r, cy), D2D1_FIGURE_BEGIN_FILLED);
    sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(cx + r, cy), D2D1::SizeF(r, r), 0,
                                  D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    sink->Close();
    rt->FillGeometry(geo.Get(), brush);
}

static void Bar(ID2D1RenderTarget* rt, float x0, float x1, float y, float th, ID2D1Brush* brush)
{
    rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x0, y, x1, y + th), th / 2, th / 2), brush);
}

// Glyph on a 16-unit grid, snapped to whole pixels so 16/20/24 px stay sharp.
static void DrawGlyph(ID2D1RenderTarget* rt, int size, ID2D1Brush* brush)
{
    float s = size / 16.f;
    float th = std::max(1.f, std::round(s));
    float horizonY = std::round(10.f * s);
    float gap = std::max(1.f, std::round(s));
    float cx = size / 2.f;
    float r = std::round(4.6f * s * 2) / 2;
    FillHalfDisc(rt, cx, horizonY - gap, r, brush);
    Bar(rt, std::round(1.f * s), size - std::round(1.f * s), horizonY, th, brush);
    float y2 = horizonY + th + std::max(1.f, std::round(1.f * s));
    Bar(rt, std::round(4.f * s), size - std::round(4.f * s), y2, th, brush);
    float y3 = y2 + th + std::max(1.f, std::round(1.f * s));
    if (y3 + th <= size)
        Bar(rt, std::round(6.f * s), size - std::round(6.f * s), y3, th, brush);
}

void DrawLogo(ID2D1RenderTarget* rt, float x, float y, float size)
{
    D2D1_GRADIENT_STOP stops[] = {
        { 0.0f, Rgba(0xA79BF2) },
        { 0.55f, Rgba(0xE7A6CB) },
        { 1.0f, Rgba(0xF8BE9C) },
    };
    ComPtr<ID2D1GradientStopCollection> coll;
    rt->CreateGradientStopCollection(stops, 3, &coll);
    ComPtr<ID2D1LinearGradientBrush> grad;
    rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(x, y), D2D1::Point2F(x + size * 0.6f, y + size)),
                                  coll.Get(), &grad);
    float rad = size * 0.25f;
    rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x, y, x + size, y + size), rad, rad), grad.Get());

    ComPtr<ID2D1SolidColorBrush> white;
    rt->CreateSolidColorBrush(Rgba(0xFFFFFF, 0.95f), &white);
    float u = size / 100.f;
    float th = std::max(1.2f, 6.f * u);
    FillHalfDisc(rt, x + 50 * u, y + 57 * u, 21 * u, white.Get());
    Bar(rt, x + 18 * u, x + 82 * u, y + 61 * u, th, white.Get());
    white->SetOpacity(0.75f);
    Bar(rt, x + 31 * u, x + 69 * u, y + 61 * u + th * 2.0f, th, white.Get());
    white->SetOpacity(0.5f);
    Bar(rt, x + 41 * u, x + 59 * u, y + 61 * u + th * 4.0f, th, white.Get());
}

// Renders via D2D into a WIC bitmap; returns premultiplied BGRA pixels.
template <typename Fn>
static std::vector<uint32_t> RenderPixels(int size, Fn draw)
{
    std::vector<uint32_t> px;
    auto wic = Wic();
    ComPtr<IWICBitmap> bmp;
    if (!wic || FAILED(wic->CreateBitmap(size, size, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bmp))) return px;
    auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
    ComPtr<ID2D1RenderTarget> rt;
    if (FAILED(gfx::D2DFactory()->CreateWicBitmapRenderTarget(bmp.Get(), props, &rt))) return px;
    rt->BeginDraw();
    rt->Clear(D2D1::ColorF(0, 0, 0, 0));
    draw(rt.Get());
    rt->EndDraw();
    px.resize(size * size);
    bmp->CopyPixels(nullptr, size * 4, (UINT)(px.size() * 4), (BYTE*)px.data());
    return px;
}

static HICON IconFromPixels(const std::vector<uint32_t>& pre, int size)
{
    BITMAPV5HEADER bi{};
    bi.bV5Size = sizeof(bi);
    bi.bV5Width = size;
    bi.bV5Height = -size;
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000; bi.bV5GreenMask = 0x0000FF00; bi.bV5BlueMask = 0x000000FF; bi.bV5AlphaMask = 0xFF000000;
    void* bits = nullptr;
    HDC dc = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(dc, (BITMAPINFO*)&bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, dc);
    if (!color) return nullptr;
    auto* out = (uint32_t*)bits;
    for (size_t i = 0; i < pre.size(); i++)
    {
        uint32_t p = pre[i];
        uint32_t a = p >> 24;
        if (a == 0) { out[i] = 0; continue; }
        uint32_t r = ((p >> 16) & 255) * 255 / a, g = ((p >> 8) & 255) * 255 / a, b = (p & 255) * 255 / a;
        out[i] = (a << 24) | (std::min(r, 255u) << 16) | (std::min(g, 255u) << 8) | std::min(b, 255u);
    }
    HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);
    ICONINFO ii{ TRUE, 0, 0, mask, color };
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(mask);
    DeleteObject(color);
    return icon;
}

HICON CreateTrayIcon(int size, bool lightTaskbar)
{
    auto px = RenderPixels(size, [&](ID2D1RenderTarget* rt) {
        ComPtr<ID2D1SolidColorBrush> brush;
        rt->CreateSolidColorBrush(lightTaskbar ? Rgba(0x000000, 0.9f) : Rgba(0xFFFFFF), &brush);
        DrawGlyph(rt, size, brush.Get());
    });
    return px.empty() ? nullptr : IconFromPixels(px, size);
}

bool ExportIco(const wchar_t* path)
{
    const int sizes[] = { 16, 20, 24, 32, 40, 48, 64, 256 };
    std::vector<std::vector<BYTE>> pngs;
    auto wic = Wic();
    for (int size : sizes)
    {
        ComPtr<IWICBitmap> bmp;
        wic->CreateBitmap(size, size, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bmp);
        auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
        ComPtr<ID2D1RenderTarget> rt;
        if (FAILED(gfx::D2DFactory()->CreateWicBitmapRenderTarget(bmp.Get(), props, &rt))) return false;
        rt->BeginDraw();
        rt->Clear(D2D1::ColorF(0, 0, 0, 0));
        float pad = size <= 24 ? 0.f : size * 0.04f;
        DrawLogo(rt.Get(), pad, pad, size - pad * 2);
        rt->EndDraw();

        ComPtr<IStream> stream;
        CreateStreamOnHGlobal(nullptr, TRUE, &stream);
        ComPtr<IWICBitmapEncoder> enc;
        wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc);
        enc->Initialize(stream.Get(), WICBitmapEncoderNoCache);
        ComPtr<IWICBitmapFrameEncode> frame;
        enc->CreateNewFrame(&frame, nullptr);
        frame->Initialize(nullptr);
        frame->SetSize(size, size);
        WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
        frame->SetPixelFormat(&fmt);
        // Convert premultiplied -> straight alpha for the PNG.
        ComPtr<IWICFormatConverter> conv;
        wic->CreateFormatConverter(&conv);
        conv->Initialize(bmp.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom);
        frame->WriteSource(conv.Get(), nullptr);
        frame->Commit();
        enc->Commit();

        STATSTG st{};
        stream->Stat(&st, STATFLAG_NONAME);
        std::vector<BYTE> data((size_t)st.cbSize.QuadPart);
        LARGE_INTEGER zero{};
        stream->Seek(zero, STREAM_SEEK_SET, nullptr);
        ULONG read = 0;
        stream->Read(data.data(), (ULONG)data.size(), &read);
        pngs.push_back(std::move(data));
    }

    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD w;
    WORD header[3] = { 0, 1, (WORD)pngs.size() };
    WriteFile(f, header, sizeof(header), &w, nullptr);
    DWORD offset = 6 + 16 * (DWORD)pngs.size();
    for (size_t i = 0; i < pngs.size(); i++)
    {
        BYTE dim = sizes[i] >= 256 ? 0 : (BYTE)sizes[i];
        BYTE entry[16] = { dim, dim, 0, 0, 1, 0, 32, 0 };
        *(DWORD*)(entry + 8) = (DWORD)pngs[i].size();
        *(DWORD*)(entry + 12) = offset;
        WriteFile(f, entry, 16, &w, nullptr);
        offset += (DWORD)pngs[i].size();
    }
    for (auto& p : pngs) WriteFile(f, p.data(), (DWORD)p.size(), &w, nullptr);
    CloseHandle(f);
    return true;
}

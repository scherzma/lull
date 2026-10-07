// The tray flyout. A live preview of the chosen scene fills the top of the card and fades into
// a frosted copy of itself; below it sit the length, two options and Begin.
#include "app.h"
#include <d2d1_1helper.h>
#include <d2d1effects.h>
#include <shellscalingapi.h>
#include <windowsx.h>
#include <shellapi.h>
#include <cmath>

#pragma comment(lib, "dxguid.lib")

namespace
{
    // Layout in DIPs (card-local coordinates).
    const float CW = 360, P = 18, HeroH = 232;
    const float SegY = 252, SegH = 40, ToggleY = 304, ToggleH = 38, BeginY = 356, BeginH = 46;
    const float CHMain = 420, CHTune = 484, CHMax = CHTune; // the card grows while the tuning sheet is open
    const float ML = 16, MR = 16, MT = 10, MB = 12;          // room for the shadow
    const float kRadius = 16.f;
    const int kPresets[] = { 1, 2, 3, 5, 10, 15 };
    constexpr int kPresetCount = 6;

    enum Hit
    {
        HitNone = -1, HitMore = 0, HitPrev, HitNext, HitShuffle, HitToggle0, HitToggle1, HitToggle2, HitBegin,
        HitTune0, HitTune1, HitTune2, HitTune3, HitTuneReset, HitTuneDone, HitLook0, HitLook1, HitLook2,
        HitSeg0, HitDot0 = HitSeg0 + kPresetCount, HitHero = HitDot0 + kSceneCount, HitCount
    };

    // The card sits on a live copy of the scene, so everything on it is white glass.
    const D2D1_COLOR_F W1 = { 1, 1, 1, 0.96f }, W2 = { 1, 1, 1, 0.74f }, W3 = { 1, 1, 1, 0.52f };
    const D2D1_COLOR_F kInk = { 0.08f, 0.09f, 0.17f, 1.f }; // dark text on white controls

    const wchar_t kClass[] = L"Lull.Flyout";
    HWND g_hwnd = nullptr;
    CompSurface g_surf;
    UINT g_dpi = 96;
    float g_scale = 1.f;
    bool g_visible = false, g_closing = false, g_menuOpen = false, g_tracking = false;
    double g_animStart = 0, g_lastFrame = 0, g_appStart = 0;
    DWORD g_lastHideTick = 0;
    int g_hover = HitNone, g_pressed = HitNone;
    float g_hoverAnim[HitCount] = {};
    float g_heroHover = 0;      // chevrons fade in while the pointer is over the preview
    constexpr int kToggles = 3;
    float g_toggleAnim[kToggles] = { -1, -1, -1 };
    float g_segAnim = -1;       // sliding position of the selected-length pill (segment index)

    // Hidden Grid tuning sheet (double-click the Grid preview): a look switch, four slider tiles
    // (singularities, intensity, pace, surges) and Done.
    bool g_tuning = false;
    float g_tuneAnim = 0;       // 0 = normal controls, 1 = tuning sheet
    int g_dragTune = -1;        // slider tile being dragged
    float g_lookAnim = -1;      // sliding position of the selected-look pill
    const float TuneTitleY = 248, LookY = 270, LookH = 36, TileY = 316, TileH = 44, TileGap = 8;
    const float TuneDoneY = 424, TuneDoneH = 42;

    // The window is sized for the tall card; the card sits at its bottom (taskbar at the bottom)
    // or top, so growing for the tuning sheet extends it away from the taskbar.
    float g_cardH = CHMain;
    bool g_anchorBottom = true;
    float CardTop() { return MT + (g_anchorBottom ? CHMax - g_cardH : 0.f); }

    // What the preview shows: current scene and the one we're crossfading from.
    int g_show = -1, g_showPrev = -1;
    double g_switchAt = -10;
    int g_nameDir = 1;
    RenderTex g_hero[2], g_bg[2];
    ComPtr<ID2D1Effect> g_blur;
    ComPtr<ID2D1Bitmap1> g_shadow;
    float g_shadowScale = 0;

    struct Fonts
    {
        ComPtr<IDWriteTextFormat> brand, name, sub, meta, metaR, seg, pill, button, icon, iconSmall, foot, tileValue;
    } F;
    std::wstring g_iconFont;

    // ------------------------------------------------------------ helpers

    bool HasFont(const wchar_t* family)
    {
        ComPtr<IDWriteFontCollection> fc;
        gfx::DWrite()->GetSystemFontCollection(&fc);
        UINT32 idx; BOOL exists = FALSE;
        if (fc) fc->FindFamilyName(family, &idx, &exists);
        return exists != FALSE;
    }

    ComPtr<IDWriteTextFormat> Font(const wchar_t* family, float size, DWRITE_FONT_WEIGHT w = DWRITE_FONT_WEIGHT_NORMAL,
                                   DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING)
    {
        ComPtr<IDWriteTextFormat> f;
        gfx::DWrite()->CreateTextFormat(family, nullptr, w, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", &f);
        if (f)
        {
            f->SetTextAlignment(align);
            f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        }
        return f;
    }

    void CreateFonts()
    {
        if (F.brand) return;
        const wchar_t* text = HasFont(L"Segoe UI Variable Text") ? L"Segoe UI Variable Text" : L"Segoe UI";
        const wchar_t* disp = HasFont(L"Segoe UI Variable Display") ? L"Segoe UI Variable Display" : L"Segoe UI";
        g_iconFont = HasFont(L"Segoe Fluent Icons") ? L"Segoe Fluent Icons" : L"Segoe MDL2 Assets";
        F.brand = Font(text, 14, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        F.name = Font(disp, 28, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        F.sub = Font(text, 12.5f);
        F.meta = Font(text, 12.5f);
        F.metaR = Font(text, 12.5f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_TRAILING);
        F.seg = Font(text, 14, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
        F.pill = Font(text, 12.5f);
        F.button = Font(text, 15, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
        F.icon = Font(g_iconFont.c_str(), 16, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
        F.iconSmall = Font(g_iconFont.c_str(), 13, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
        F.foot = Font(text, 11.5f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
        F.tileValue = Font(text, 14, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    }

    D2D1_RECT_F R(float l, float t, float r, float b) { return D2D1::RectF(l, t, r, b); }

    D2D1_COLOR_F Mix(D2D1_COLOR_F a, D2D1_COLOR_F b, float t)
    {
        return D2D1::ColorF(Lerp(a.r, b.r, t), Lerp(a.g, b.g, t), Lerp(a.b, b.b, t), Lerp(a.a, b.a, t));
    }

    D2D1_COLOR_F WithAlpha(D2D1_COLOR_F c, float a) { c.a *= a; return c; }
    D2D1_COLOR_F White(float a) { return D2D1::ColorF(1, 1, 1, a); }

    float TextWidth(IDWriteTextFormat* fmt, const wchar_t* s)
    {
        ComPtr<IDWriteTextLayout> l;
        gfx::DWrite()->CreateTextLayout(s, (UINT32)wcslen(s), fmt, 1000, 100, &l);
        DWRITE_TEXT_METRICS m{};
        if (l) l->GetMetrics(&m);
        return m.widthIncludingTrailingWhitespace;
    }

    void Text(ID2D1DeviceContext* dc, IDWriteTextFormat* fmt, const wchar_t* s, D2D1_RECT_F r, D2D1_COLOR_F c, ID2D1SolidColorBrush* b)
    {
        b->SetColor(c);
        dc->DrawTextW(s, (UINT32)wcslen(s), fmt, r, b, D2D1_DRAW_TEXT_OPTIONS_NONE);
    }

    int PresetIndex(int minutes)
    {
        int best = 0;
        for (int i = 1; i < kPresetCount; i++)
            if (abs(kPresets[i] - minutes) < abs(kPresets[best] - minutes)) best = i;
        return best;
    }

    // ------------------------------------------------------------ layout

    D2D1_RECT_F SegRect(int i)
    {
        float w = (CW - 2 * P) / kPresetCount;
        return R(P + i * w, SegY, P + (i + 1) * w, SegY + SegH);
    }

    D2D1_RECT_F DotRect(int i)
    {
        float x = P + i * 14.f;
        return R(x - 3, HeroH - 34, x + 11, HeroH - 18);
    }

    D2D1_RECT_F RectOf(int id)
    {
        float tw = (CW - 2 * P - 2 * 8) / 3;
        switch (id)
        {
        case HitMore: return R(CW - 14 - 32, 12, CW - 14, 44);
        case HitPrev: return R(12, HeroH / 2 - 30, 46, HeroH / 2 + 4);
        case HitNext: return R(CW - 46, HeroH / 2 - 30, CW - 12, HeroH / 2 + 4);
        case HitShuffle: return R(CW - P - 36, HeroH - 52, CW - P, HeroH - 16);
        case HitToggle0: return R(P, ToggleY, P + tw, ToggleY + ToggleH);
        case HitToggle1: return R(P + tw + 8, ToggleY, P + 2 * tw + 8, ToggleY + ToggleH);
        case HitToggle2: return R(CW - P - tw, ToggleY, CW - P, ToggleY + ToggleH);
        case HitBegin: return R(P, BeginY, CW - P, BeginY + BeginH);
        case HitTune0: case HitTune1: case HitTune2: case HitTune3:
        {
            int i = id - HitTune0;
            float w = (CW - 2 * P - TileGap) / 2, x = P + (i % 2) * (w + TileGap), y = TileY + (i / 2) * (TileH + TileGap);
            return R(x, y, x + w, y + TileH);
        }
        case HitLook0: case HitLook1: case HitLook2:
        {
            float w = (CW - 2 * P) / 3, x = P + (id - HitLook0) * w;
            return R(x, LookY, x + w, LookY + LookH);
        }
        case HitTuneReset: return R(CW - P - 56, TuneTitleY - 4, CW - P, TuneTitleY + 22);
        case HitTuneDone: return R(P, TuneDoneY, CW - P, TuneDoneY + TuneDoneH);
        case HitHero: return R(0, 0, CW, HeroH);
        }
        if (id >= HitSeg0 && id < HitSeg0 + kPresetCount) return SegRect(id - HitSeg0);
        if (id >= HitDot0 && id < HitDot0 + kSceneCount) return DotRect(id - HitDot0);
        return R(0, 0, 0, 0);
    }

    int HitTest(float x, float y)
    {
        // Everything except the preview itself, which is the fallback for the top area.
        for (int id = 0; id < HitHero; id++)
        {
            if (id >= HitDot0 && id < HitDot0 + kSceneCount && g_settings.shuffle) continue;
            bool tuneId = id >= HitTune0 && id <= HitLook2;
            bool lowerId = id == HitToggle0 || id == HitToggle1 || id == HitToggle2 || id == HitBegin ||
                           (id >= HitSeg0 && id < HitSeg0 + kPresetCount);
            if (tuneId && !g_tuning) continue;
            if (lowerId && g_tuning) continue;
            auto r = RectOf(id);
            if (x >= r.left && x < r.right && y >= r.top && y < r.bottom) return id;
        }
        auto r = RectOf(HitHero);
        if (x >= r.left && x < r.right && y >= r.top && y < r.bottom) return HitHero;
        return HitNone;
    }

    // ------------------------------------------------------------ actions

    void SetPreset(int idx)
    {
        idx = std::clamp(idx, 0, kPresetCount - 1);
        if (kPresets[idx] == g_settings.minutes) return;
        g_settings.minutes = kPresets[idx];
        SaveSettings();
    }

    void StepScene(int dir)
    {
        auto& s = g_settings;
        s.scene = s.shuffle ? (g_show >= 0 ? g_show : s.scene) : (s.scene + dir + kSceneCount) % kSceneCount;
        s.shuffle = false;
        g_nameDir = dir;
        if (s.scene != kSceneGrid) g_tuning = false;
        SaveSettings();
    }

    void Activate(int id)
    {
        auto& s = g_settings;
        switch (id)
        {
        case HitMore:
        {
            auto r = RectOf(HitMore);
            POINT pt{ (LONG)((r.right + ML) * g_scale), (LONG)((r.bottom + CardTop() + 4) * g_scale) };
            ClientToScreen(g_hwnd, &pt);
            g_menuOpen = true;
            ShowTrayMenu(g_hwnd, pt);
            g_menuOpen = false;
            if (g_visible && !overlay::Active()) SetForegroundWindow(g_hwnd);
            return;
        }
        case HitPrev: StepScene(-1); return;
        case HitNext: StepScene(1); return;
        case HitShuffle: s.shuffle = !s.shuffle; break;
        case HitToggle0: s.showCountdown = !s.showCountdown; break;
        case HitToggle1: s.reminders = !s.reminders; break;
        case HitToggle2: s.muteSounds = !s.muteSounds; break;
        case HitBegin: BeginLull(s.minutes * 60); return;
        case HitTuneDone: g_tuning = false; return;
        case HitTuneReset: s.gridSources = 5; s.gridIntensity = 50; s.gridPace = 50; s.gridSurges = 0; break;
        case HitLook0: case HitLook1: case HitLook2: s.gridLook = id - HitLook0; break;
        default:
            if (id >= HitSeg0 && id < HitSeg0 + kPresetCount) { SetPreset(id - HitSeg0); return; }
            if (id >= HitDot0 && id < HitDot0 + kSceneCount)
            {
                int target = id - HitDot0;
                g_nameDir = target >= s.scene ? 1 : -1;
                s.scene = target;
                s.shuffle = false;
                if (target != kSceneGrid) g_tuning = false;
                break;
            }
            return;
        }
        SaveSettings();
    }

    // ------------------------------------------------------------ rendering

    void EnsureEffects(ID2D1DeviceContext* dc)
    {
        if (!g_blur) dc->CreateEffect(CLSID_D2D1GaussianBlur, &g_blur);
        if (g_shadow && g_shadowScale == g_scale) return;
        g_shadow.Reset();
        float W = CW + ML + MR, H = CHMain + MT + MB;
        auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), (float)g_dpi, (float)g_dpi);
        ComPtr<ID2D1Bitmap1> mask, out;
        D2D1_SIZE_U px = D2D1::SizeU((UINT32)ceilf(W * g_scale), (UINT32)ceilf(H * g_scale));
        if (FAILED(dc->CreateBitmap(px, nullptr, 0, &props, &mask)) || FAILED(dc->CreateBitmap(px, nullptr, 0, &props, &out))) return;

        ComPtr<ID2D1Image> old;
        dc->GetTarget(&old);
        ComPtr<ID2D1SolidColorBrush> b;
        dc->SetDpi((float)g_dpi, (float)g_dpi);
        dc->SetTarget(mask.Get());
        dc->BeginDraw();
        dc->Clear(D2D1::ColorF(0, 0, 0, 0));
        dc->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 1), &b);
        dc->FillRoundedRectangle(D2D1::RoundedRect(R(ML + 2, MT + 4, ML + CW - 2, MT + CHMain + 3), kRadius, kRadius), b.Get());
        dc->EndDraw();

        ComPtr<ID2D1Effect> shadow;
        dc->CreateEffect(CLSID_D2D1Shadow, &shadow);
        shadow->SetInput(0, mask.Get());
        shadow->SetValue(D2D1_SHADOW_PROP_BLUR_STANDARD_DEVIATION, 6.0f);
        shadow->SetValue(D2D1_SHADOW_PROP_COLOR, D2D1::Vector4F(0.02f, 0.02f, 0.06f, 0.42f));
        dc->SetTarget(out.Get());
        dc->BeginDraw();
        dc->Clear(D2D1::ColorF(0, 0, 0, 0));
        dc->DrawImage(shadow.Get());
        dc->EndDraw();
        dc->SetTarget(old.Get());
        g_shadow = out;
        g_shadowScale = g_scale;
    }

    // Renders the preview (crisp) and backdrop (small, to be blurred) for the shown scene,
    // plus the previous scene while crossfading.
    void RenderScenes(double now)
    {
        ApplyGridTuning();
        gfx::TickGrid(now);
        double t = now - g_appStart;
        int target = g_settings.shuffle ? (int)(t / 6.0) % kSceneCount : g_settings.scene;
        if (target != g_show)
        {
            g_showPrev = g_show;
            g_show = target;
            g_switchAt = g_showPrev < 0 ? -10 : now;
        }
        bool fading = now - g_switchAt < 0.6 && g_showPrev >= 0;
        int hw = (int)ceilf(CW * g_scale), hh = (int)ceilf(HeroH * g_scale);
        // The backdrop covers the whole card but uses the preview's framing, so below the
        // preview you see the same scene continuing (blurred), not a stretched copy.
        float bs = g_scale / 3;
        int bw = (int)ceilf(CW * bs), bh = (int)ceilf(CHMax * bs);
        int scenes[2] = { g_show, fading ? g_showPrev : -1 };
        for (int k = 0; k < 2; k++)
        {
            if (scenes[k] < 0) continue;
            float st = kScenes[scenes[k]].timeOffset + (float)t;
            float br = gfx::Breath(t);
            if (g_hero[k].Ensure(hw, hh, DXGI_FORMAT_B8G8R8A8_UNORM, true))
                gfx::DrawScene(scenes[k], g_hero[k].rtv.Get(), hw, hh, st, br);
            if (g_bg[k].Ensure(bw, bh, DXGI_FORMAT_B8G8R8A8_UNORM, true))
                gfx::DrawScene(scenes[k], g_bg[k].rtv.Get(), bw, bh, st, br, CW * bs / 2, HeroH * bs / 2, HeroH * bs);
        }
    }

    // Draws a bitmap (or its blurred version) stretched over rect, optionally faded.
    void DrawStretched(ID2D1DeviceContext* dc, ID2D1Bitmap1* bmp, D2D1_RECT_F rect, float alpha, bool blur)
    {
        if (!bmp) return;
        if (alpha < 0.999f)
            dc->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                                 D2D1::IdentityMatrix(), alpha), nullptr);
        if (blur && g_blur)
        {
            g_blur->SetInput(0, bmp);
            g_blur->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION, 2.2f);
            g_blur->SetValue(D2D1_GAUSSIANBLUR_PROP_BORDER_MODE, D2D1_BORDER_MODE_HARD);
            // Effects work in context pixels (bitmap DPI is ignored), so size by the effect's real bounds.
            D2D1_MATRIX_3X2_F old;
            dc->GetTransform(&old);
            dc->SetTransform(D2D1::IdentityMatrix());
            ComPtr<ID2D1Image> image;
            g_blur->GetOutput(&image);
            D2D1_RECT_F bounds{};
            dc->GetImageLocalBounds(image.Get(), &bounds);
            float iw = std::max(1.f, bounds.right - bounds.left), ih = std::max(1.f, bounds.bottom - bounds.top);
            dc->SetTransform(D2D1::Matrix3x2F::Scale((rect.right - rect.left) / iw, (rect.bottom - rect.top) / ih) *
                             D2D1::Matrix3x2F::Translation(rect.left, rect.top) * old);
            dc->DrawImage(image.Get(), D2D1_INTERPOLATION_MODE_LINEAR);
            dc->SetTransform(old);
        }
        else
        {
            dc->DrawBitmap(bmp, rect, 1.f, D2D1_INTERPOLATION_MODE_LINEAR);
        }
        if (alpha < 0.999f) dc->PopLayer();
    }

    ComPtr<ID2D1LinearGradientBrush> VGradient(ID2D1DeviceContext* dc, float y0, float y1,
                                               std::initializer_list<D2D1_GRADIENT_STOP> stops)
    {
        ComPtr<ID2D1GradientStopCollection> coll;
        dc->CreateGradientStopCollection(stops.begin(), (UINT32)stops.size(), &coll);
        ComPtr<ID2D1LinearGradientBrush> g;
        dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, y0), D2D1::Point2F(0, y1)), coll.Get(), &g);
        return g;
    }

    void GlassCircle(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* b, D2D1_RECT_F r, float hover, float alpha, bool on = false)
    {
        D2D1_POINT_2F c = D2D1::Point2F((r.left + r.right) / 2, (r.top + r.bottom) / 2);
        float rad = (r.right - r.left) / 2;
        b->SetColor(on ? White((0.92f + 0.06f * hover) * alpha) : White((0.14f + 0.12f * hover) * alpha));
        dc->FillEllipse(D2D1::Ellipse(c, rad, rad), b);
        if (!on)
        {
            b->SetColor(White(0.22f * alpha));
            dc->DrawEllipse(D2D1::Ellipse(c, rad - 0.5f, rad - 0.5f), b, 1.f);
        }
    }

    // The hidden "Tune Grid" sheet, in the same glass language as the normal controls: a header
    // row like "Length", a look switch like the length picker, four tiles that fill like a level
    // as you drag across them (or scroll over them), and a white Done like Begin.
    void DrawTuneSheet(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* b, float a)
    {
        const Settings& s = g_settings;
        float slide = (1 - a) * 10.f;
        dc->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                             D2D1::Matrix3x2F::Identity(), a), nullptr);
        D2D1_MATRIX_3X2_F old;
        dc->GetTransform(&old);
        dc->SetTransform(D2D1::Matrix3x2F::Translation(0, slide) * old);

        Text(dc, F.meta.Get(), L"Tune Grid", R(P, TuneTitleY, CW / 2, TuneTitleY + 18), W3, b);
        Text(dc, F.metaR.Get(), L"Reset", RectOf(HitTuneReset), Mix(W3, W1, g_hoverAnim[HitTuneReset]), b);

        // ---- look: night / day / auto
        {
            float rad = LookH / 2, segW = (CW - 2 * P) / 3;
            b->SetColor(White(0.08f));
            dc->FillRoundedRectangle(D2D1::RoundedRect(R(P, LookY, CW - P, LookY + LookH), rad, rad), b);
            b->SetColor(White(0.14f));
            dc->DrawRoundedRectangle(D2D1::RoundedRect(R(P + 0.5f, LookY + 0.5f, CW - P - 0.5f, LookY + LookH - 0.5f), rad - 0.5f, rad - 0.5f), b, 1.f);
            if (g_lookAnim < 0) g_lookAnim = (float)s.gridLook;
            for (int i = 0; i < 3; i++)
            {
                float h = g_hoverAnim[HitLook0 + i];
                if (h > 0.01f && i != s.gridLook)
                {
                    auto r = RectOf(HitLook0 + i);
                    b->SetColor(White(0.07f * h));
                    dc->FillRoundedRectangle(D2D1::RoundedRect(R(r.left + 3, r.top + 3, r.right - 3, r.bottom - 3), rad - 3, rad - 3), b);
                }
            }
            float px = P + g_lookAnim * segW;
            b->SetColor(White(0.96f));
            dc->FillRoundedRectangle(D2D1::RoundedRect(R(px + 3, LookY + 3, px + segW - 3, LookY + LookH - 3), rad - 3, rad - 3), b);
            const wchar_t* names[] = { L"Night", L"Day", L"Auto" };
            const wchar_t* icons[] = { L"\xE708", L"\xE706", L"\xE770" };
            for (int i = 0; i < 3; i++)
            {
                auto r = RectOf(HitLook0 + i);
                D2D1_COLOR_F fg = Mix(White(0.86f), kInk, Clamp01(1.f - fabsf(g_lookAnim - i)));
                float lw = TextWidth(F.pill.Get(), names[i]), x0 = (r.left + r.right) / 2 - (14 + 6 + lw) / 2;
                Text(dc, F.iconSmall.Get(), icons[i], R(x0, r.top, x0 + 14, r.bottom), fg, b);
                Text(dc, F.pill.Get(), names[i], R(x0 + 20, r.top, r.right, r.bottom - 1), fg, b);
            }
        }

        // ---- four level tiles
        auto word = [](int v, const wchar_t* const* names) { return names[std::min(4, v / 21)]; };
        static const wchar_t* intensity[] = { L"Calm", L"Gentle", L"Lively", L"Wild", L"Extreme" };
        static const wchar_t* pace[] = { L"Slow", L"Unhurried", L"Normal", L"Brisk", L"Fast" };
        wchar_t sources[16];
        swprintf_s(sources, L"%d", s.gridSources);
        const wchar_t* surges = s.gridSurges == 0 ? L"Never" : s.gridSurges <= 30 ? L"Rare" : s.gridSurges <= 60 ? L"Sometimes" : s.gridSurges <= 85 ? L"Often" : L"Frequent";
        const wchar_t* labels[] = { L"Singularities", L"Intensity", L"Pace", L"Surges" };
        const wchar_t* values[] = { sources, word(s.gridIntensity, intensity), word(s.gridPace, pace), surges };
        float fracs[] = { (s.gridSources - 1) / 7.f, s.gridIntensity / 100.f, s.gridPace / 100.f, s.gridSurges / 100.f };
        for (int i = 0; i < 4; i++)
        {
            auto r = RectOf(HitTune0 + i);
            float h = std::max(g_hoverAnim[HitTune0 + i], g_dragTune == i ? 1.f : 0.f);
            auto rr = D2D1::RoundedRect(r, 12, 12);
            b->SetColor(White(0.06f + 0.03f * h));
            dc->FillRoundedRectangle(rr, b);
            // The level: filled glass up to the value, with a bright edge.
            ComPtr<ID2D1RoundedRectangleGeometry> geo;
            gfx::D2DFactory()->CreateRoundedRectangleGeometry(rr, &geo);
            dc->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), geo.Get(), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                                 D2D1::IdentityMatrix(), 1.f), nullptr);
            float fx = Lerp(r.left, r.right, fracs[i]);
            b->SetColor(White(0.14f + 0.05f * h));
            dc->FillRectangle(R(r.left, r.top, fx, r.bottom), b);
            if (fx > r.left + 1 && fx < r.right - 1)
            {
                b->SetColor(White(0.42f + 0.3f * h));
                dc->FillRectangle(R(fx - 1, r.top, fx, r.bottom), b);
            }
            dc->PopLayer();
            b->SetColor(White(0.12f + 0.08f * h));
            dc->DrawRoundedRectangle(D2D1::RoundedRect(R(r.left + 0.5f, r.top + 0.5f, r.right - 0.5f, r.bottom - 0.5f), 11.5f, 11.5f), b, 1.f);
            Text(dc, F.meta.Get(), labels[i], R(r.left + 12, r.top + 5, r.right - 8, r.top + 21), W2, b);
            Text(dc, F.tileValue.Get(), values[i], R(r.left + 12, r.top + 21, r.right - 8, r.bottom - 5), W1, b);
        }

        // ---- done
        {
            auto r = RectOf(HitTuneDone);
            bool pressed = g_pressed == HitTuneDone && g_hover == HitTuneDone;
            b->SetColor(White(pressed ? 0.82f : 0.97f + 0.03f * g_hoverAnim[HitTuneDone]));
            dc->FillRoundedRectangle(D2D1::RoundedRect(r, TuneDoneH / 2, TuneDoneH / 2), b);
            Text(dc, F.button.Get(), L"Done", R(r.left, r.top, r.right, r.bottom - 1), kInk, b);
        }

        dc->SetTransform(old);
        dc->PopLayer();
    }

    void Draw(ID2D1DeviceContext* dc, float opacity, float yOff)
    {
        const Settings& s = g_settings;
        double now = gfx::Now();
        ComPtr<ID2D1SolidColorBrush> b;
        dc->CreateSolidColorBrush(W1, &b);

        bool layered = opacity < 0.999f;
        if (layered)
            dc->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                                 D2D1::IdentityMatrix(), opacity), nullptr);

        // The shadow is made for the short card; for the tall one its middle is stretched.
        if (g_shadow)
        {
            float W = CW + ML + MR, H = CHMain + MT + MB, y0 = CardTop() - MT + yOff, grow = g_cardH - CHMain, sy = MT + CHMain / 2;
            auto top = R(0, 0, W, sy), mid = R(0, sy, W, sy + 1), bottom = R(0, sy, W, H);
            dc->SetTransform(D2D1::IdentityMatrix());
            dc->DrawBitmap(g_shadow.Get(), R(0, y0, W, y0 + sy), 1.f, D2D1_INTERPOLATION_MODE_LINEAR, &top);
            if (grow > 0) dc->DrawBitmap(g_shadow.Get(), R(0, y0 + sy, W, y0 + sy + grow), 1.f, D2D1_INTERPOLATION_MODE_LINEAR, &mid);
            dc->DrawBitmap(g_shadow.Get(), R(0, y0 + sy + grow, W, y0 + H + grow), 1.f, D2D1_INTERPOLATION_MODE_LINEAR, &bottom);
        }
        dc->SetTransform(D2D1::Matrix3x2F::Translation(ML, CardTop() + yOff));

        float fade = Clamp01((float)((now - g_switchAt) / 0.6));
        bool fading = fade < 1 && g_showPrev >= 0;
        float fe = EaseInOut(fade);

        // ---- card surface (clipped to the rounded card)
        const float CH = g_cardH;
        auto cardRR = D2D1::RoundedRect(R(0, 0, CW, CH), kRadius, kRadius);
        ComPtr<ID2D1RoundedRectangleGeometry> cardGeo;
        gfx::D2DFactory()->CreateRoundedRectangleGeometry(cardRR, &cardGeo);
        dc->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), cardGeo.Get(), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                             D2D1::IdentityMatrix(), 1.f), nullptr);
        b->SetColor(Rgba(0x0B0E1C));
        dc->FillRectangle(R(0, 0, CW, CH), b.Get());

        // Frosted backdrop for the whole card, then a dusk tint for legibility.
        if (fading) DrawStretched(dc, g_bg[1].bmp.Get(), R(0, 0, CW, CHMax), 1.f, true);
        DrawStretched(dc, g_bg[0].bmp.Get(), R(0, 0, CW, CHMax), fading ? fe : 1.f, true);
        // Over Grid's pale day look a neutral tint turns grey, so it deepens into twilight violet.
        float dayTint = g_show == kSceneGrid ? gfx::GridDay() : 0.f;
        D2D1_COLOR_F dusk = Mix(D2D1::ColorF(0.04f, 0.05f, 0.11f, 1.f), D2D1::ColorF(0.13f, 0.10f, 0.27f, 1.f), dayTint);
        dc->FillRectangle(R(0, 0, CW, CH), VGradient(dc, 0, CH, {
            { 0.0f, WithAlpha(dusk, 0.40f + 0.08f * dayTint) },
            { 0.5f, WithAlpha(dusk, 0.56f + 0.16f * dayTint) },
            { 1.0f, WithAlpha(dusk, 0.70f + 0.14f * dayTint) } }).Get());

        // Crisp live preview, fading into the frost at its bottom edge.
        {
            auto mask = VGradient(dc, 0, HeroH, { { 0.0f, White(1) }, { 0.62f, White(1) }, { 1.0f, White(0) } });
            dc->PushLayer(D2D1::LayerParameters1(R(0, 0, CW, HeroH), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                                 D2D1::IdentityMatrix(), 1.f, mask.Get()), nullptr);
            if (fading) DrawStretched(dc, g_hero[1].bmp.Get(), R(0, 0, CW, HeroH), 1.f, false);
            DrawStretched(dc, g_hero[0].bmp.Get(), R(0, 0, CW, HeroH), fading ? fe : 1.f, false);
            dc->PopLayer();
        }
        // Soft scrims so the white type on the preview always reads.
        dc->FillRectangle(R(0, 0, CW, 64), VGradient(dc, 0, 64, { { 0.f, D2D1::ColorF(0, 0, 0.03f, 0.34f) }, { 1.f, D2D1::ColorF(0, 0, 0.03f, 0.f) } }).Get());
        dc->FillRectangle(R(0, HeroH - 110, CW, HeroH + 70), VGradient(dc, HeroH - 110, HeroH + 70, {
            { 0.f, D2D1::ColorF(0.02f, 0.03f, 0.08f, 0.f) }, { 0.6f, D2D1::ColorF(0.02f, 0.03f, 0.08f, 0.34f) },
            { 1.f, D2D1::ColorF(0.02f, 0.03f, 0.08f, 0.f) } }).Get());
        dc->PopLayer();

        // Glass edge.
        dc->DrawRoundedRectangle(D2D1::RoundedRect(R(0.5f, 0.5f, CW - 0.5f, CH - 0.5f), kRadius - 0.5f, kRadius - 0.5f),
                                 VGradient(dc, 0, CH, { { 0.0f, White(0.26f) }, { 0.3f, White(0.10f) }, { 1.0f, White(0.07f) } }).Get(), 1.f);

        // ---- on the preview
        DrawLogo(dc, 16, 14, 24);
        Text(dc, F.brand.Get(), L"Lull", R(48, 14, 160, 38), W1, b.Get());
        {
            auto r = RectOf(HitMore);
            float h = g_hoverAnim[HitMore];
            if (h > 0.01f) GlassCircle(dc, b.Get(), r, 0.f, h);
            Text(dc, F.icon.Get(), L"\xE712", r, W1, b.Get());
        }
        for (int i = 0; i < 2; i++)
        {
            int id = i == 0 ? HitPrev : HitNext;
            auto r = RectOf(id);
            float a = 0.35f + 0.65f * g_heroHover;
            GlassCircle(dc, b.Get(), r, g_hoverAnim[id], a);
            Text(dc, F.iconSmall.Get(), i == 0 ? L"\xE76B" : L"\xE76C", R(r.left, r.top, r.right, r.bottom), White(0.95f * a), b.Get());
        }

        // Scene name, sliding in from the side it came from.
        {
            float e = EaseOutCubic(fade);
            const wchar_t* name = s.shuffle ? L"Shuffle" : kScenes[std::max(0, g_show)].name;
            float y = HeroH - 80;
            if (fading && !s.shuffle && g_showPrev >= 0)
            {
                float dx = -g_nameDir * 18.f * e;
                Text(dc, F.name.Get(), kScenes[g_showPrev].name, R(P + dx, y, CW - 80 + dx, y + 40), WithAlpha(W1, 1 - e), b.Get());
                dx = g_nameDir * 18.f * (1 - e);
                Text(dc, F.name.Get(), name, R(P + dx, y, CW - 80 + dx, y + 40), WithAlpha(W1, e), b.Get());
            }
            else
            {
                Text(dc, F.name.Get(), name, R(P, y, CW - 80, y + 40), W1, b.Get());
            }
        }
        if (s.shuffle)
        {
            Text(dc, F.sub.Get(), L"A different scene every time", R(P, HeroH - 38, CW - 80, HeroH - 16), W2, b.Get());
        }
        else
        {
            for (int i = 0; i < kSceneCount; i++)
            {
                auto r = DotRect(i);
                float x = r.left + 3, cy = (r.top + r.bottom) / 2;
                bool on = i == s.scene;
                float h = g_hoverAnim[HitDot0 + i];
                b->SetColor(White(on ? 0.95f : 0.38f + 0.3f * h));
                if (on) dc->FillRoundedRectangle(D2D1::RoundedRect(R(x, cy - 3, x + 8, cy + 3), 3, 3), b.Get());
                else dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x + 3, cy), 3, 3), b.Get());
            }
        }
        {
            auto r = RectOf(HitShuffle);
            GlassCircle(dc, b.Get(), r, g_hoverAnim[HitShuffle], 1.f, s.shuffle);
            Text(dc, F.iconSmall.Get(), L"\xE8B1", r, s.shuffle ? kInk : W1, b.Get());
        }

        // ---- lower half: the normal controls, cross-fading with the hidden tuning sheet
        float tuneE = EaseInOut(g_tuneAnim);
        if (tuneE > 0.01f) DrawTuneSheet(dc, b.Get(), tuneE);
        float normalA = 1.f - tuneE;
        if (normalA > 0.01f)
        {
        if (normalA < 0.999f)
            dc->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                                 D2D1::IdentityMatrix(), normalA), nullptr);
        // ---- length (the selected pill spells out "min")
        {
            auto track = D2D1::RoundedRect(R(P, SegY, CW - P, SegY + SegH), SegH / 2, SegH / 2);
            b->SetColor(White(0.08f));
            dc->FillRoundedRectangle(track, b.Get());
            b->SetColor(White(0.14f));
            dc->DrawRoundedRectangle(D2D1::RoundedRect(R(P + 0.5f, SegY + 0.5f, CW - P - 0.5f, SegY + SegH - 0.5f), SegH / 2 - 0.5f, SegH / 2 - 0.5f), b.Get(), 1.f);

            int sel = PresetIndex(s.minutes);
            if (g_segAnim < 0) g_segAnim = (float)sel;
            float segW = (CW - 2 * P) / kPresetCount;
            for (int i = 0; i < kPresetCount; i++)
            {
                float h = g_hoverAnim[HitSeg0 + i];
                if (h > 0.01f && i != sel)
                {
                    auto r = SegRect(i);
                    b->SetColor(White(0.07f * h));
                    dc->FillRoundedRectangle(D2D1::RoundedRect(R(r.left + 3, r.top + 3, r.right - 3, r.bottom - 3), SegH / 2 - 3, SegH / 2 - 3), b.Get());
                }
            }
            float px = P + g_segAnim * segW;
            b->SetColor(White(0.96f));
            dc->FillRoundedRectangle(D2D1::RoundedRect(R(px + 3, SegY + 3, px + segW - 3, SegY + SegH - 3), SegH / 2 - 3, SegH / 2 - 3), b.Get());
            float minW = TextWidth(F.pill.Get(), L"min");
            for (int i = 0; i < kPresetCount; i++)
            {
                wchar_t label[8];
                swprintf_s(label, L"%d", kPresets[i]);
                float onPill = Clamp01(1.f - fabsf(g_segAnim - i));
                auto r = SegRect(i);
                float cx = (r.left + r.right) / 2 - onPill * (minW + 3) / 2, numW = TextWidth(F.seg.Get(), label);
                D2D1_COLOR_F fg = Mix(White(0.86f), kInk, onPill);
                Text(dc, F.seg.Get(), label, R(cx - 20, r.top, cx + 20, r.bottom), fg, b.Get());
                if (onPill > 0.01f)
                    Text(dc, F.pill.Get(), L"min", R(cx + numW / 2 + 3, r.top + 1, r.right, r.bottom), WithAlpha(fg, onPill * 0.75f), b.Get());
            }
        }

        // ---- options: three pill toggles
        {
            const wchar_t* labels[] = { L"Countdown", L"Reminders", L"Silence" };
            const wchar_t* icons[] = { L"\xE916", L"\xE8BD", L"\xE74F" };
            bool vals[] = { s.showCountdown, s.reminders, s.muteSounds };
            for (int i = 0; i < kToggles; i++)
            {
                int id = HitToggle0 + i;
                auto r = RectOf(id);
                if (g_toggleAnim[i] < 0) g_toggleAnim[i] = vals[i] ? 1.f : 0.f;
                float on = EaseInOut(g_toggleAnim[i]), h = g_hoverAnim[id];
                // Glass pill: brighter and outlined when on, with a small mint status light.
                auto rr = D2D1::RoundedRect(r, ToggleH / 2, ToggleH / 2);
                b->SetColor(White(Lerp(0.05f, 0.17f, on) + 0.06f * h));
                dc->FillRoundedRectangle(rr, b.Get());
                b->SetColor(White(Lerp(0.12f, 0.34f, on)));
                dc->DrawRoundedRectangle(D2D1::RoundedRect(R(r.left + 0.5f, r.top + 0.5f, r.right - 0.5f, r.bottom - 0.5f), ToggleH / 2 - 0.5f, ToggleH / 2 - 0.5f), b.Get(), 1.f);
                // When on, the label turns white and the icon lights up mint.
                D2D1_COLOR_F fg = Mix(W3, W1, on);
                D2D1_COLOR_F iconCol = Mix(W3, D2D1::ColorF(0.62f, 0.95f, 0.82f, 1.f), on);
                float lw = TextWidth(F.pill.Get(), labels[i]);
                float x0 = (r.left + r.right) / 2 - (16 + 7 + lw) / 2;
                Text(dc, F.iconSmall.Get(), icons[i], R(x0, r.top, x0 + 16, r.bottom), iconCol, b.Get());
                Text(dc, F.pill.Get(), labels[i], R(x0 + 23, r.top, r.right, r.bottom - 1), fg, b.Get());
            }
        }

        // ---- begin
        {
            auto r = RectOf(HitBegin);
            float h = g_hoverAnim[HitBegin];
            bool pressed = g_pressed == HitBegin && g_hover == HitBegin;
            b->SetColor(White(pressed ? 0.82f : 0.97f + 0.03f * h));
            dc->FillRoundedRectangle(D2D1::RoundedRect(r, BeginH / 2, BeginH / 2), b.Get());
            wchar_t label[64];
            swprintf_s(label, L"Begin  \x00B7  %d min", s.minutes);
            Text(dc, F.button.Get(), label, R(r.left, r.top, r.right, r.bottom - 1), kInk, b.Get());
        }
        if (normalA < 0.999f) dc->PopLayer();
        }

        dc->SetTransform(D2D1::IdentityMatrix());
        if (layered) dc->PopLayer();
    }

    void Animate(float dt)
    {
        float k = 1.f - expf(-dt * 14.f);
        for (int id = 0; id < HitCount; id++)
            g_hoverAnim[id] += ((g_hover == id ? 1.f : 0.f) - g_hoverAnim[id]) * k;
        bool overHero = g_hover == HitHero || g_hover == HitPrev || g_hover == HitNext || g_hover == HitShuffle ||
                        (g_hover >= HitDot0 && g_hover < HitDot0 + kSceneCount) || g_hover == HitMore;
        g_heroHover += ((overHero ? 1.f : 0.f) - g_heroHover) * k;
        g_tuneAnim += ((g_tuning ? 1.f : 0.f) - g_tuneAnim) * (1.f - expf(-dt * 12.f));
        g_cardH = Lerp(CHMain, CHTune, EaseInOut(g_tuneAnim));
        bool vals[] = { g_settings.showCountdown, g_settings.reminders, g_settings.muteSounds };
        for (int i = 0; i < kToggles; i++)
            if (g_toggleAnim[i] >= 0) g_toggleAnim[i] += ((vals[i] ? 1.f : 0.f) - g_toggleAnim[i]) * (1.f - expf(-dt * 16.f));
        float target = (float)PresetIndex(g_settings.minutes);
        if (g_segAnim < 0) g_segAnim = target;
        else g_segAnim += (target - g_segAnim) * (1.f - expf(-dt * 18.f));
        if (g_lookAnim < 0) g_lookAnim = (float)g_settings.gridLook;
        else g_lookAnim += (g_settings.gridLook - g_lookAnim) * (1.f - expf(-dt * 18.f));
    }

    // ------------------------------------------------------------ window

    void UpdateHover(int x, int y) { g_hover = HitTest(x / g_scale - ML, y / g_scale - CardTop()); }

    // Tile value (0..1) for a pointer x in window pixels: the tile's width maps to the range.
    float TuneValueAt(int row, int x)
    {
        auto r = RectOf(HitTune0 + row);
        return Clamp01((x / g_scale - ML - (r.left + 6)) / (r.right - r.left - 12));
    }

    void SetTune(int row, float v)
    {
        auto& s = g_settings;
        if (row == 0) s.gridSources = 1 + (int)std::lround(v * 7);
        else if (row == 1) s.gridIntensity = (int)std::lround(v * 100);
        else if (row == 2) s.gridPace = (int)std::lround(v * 100);
        else s.gridSurges = (int)std::lround(v * 100);
    }

    LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
    {
        switch (msg)
        {
        case WM_MOUSEMOVE:
            if (!g_tracking)
            {
                TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, h, 0 };
                TrackMouseEvent(&tme);
                g_tracking = true;
            }
            UpdateHover(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            if (g_dragTune >= 0) SetTune(g_dragTune, TuneValueAt(g_dragTune, GET_X_LPARAM(lp)));
            return 0;
        case WM_LBUTTONDBLCLK:
            // Hidden: double-clicking the Grid preview opens (or closes) the tuning sheet.
            UpdateHover(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            if (g_hover == HitHero && !g_settings.shuffle && g_settings.scene == kSceneGrid) g_tuning = !g_tuning;
            return 0;
        case WM_MOUSELEAVE:
            g_tracking = false;
            g_hover = HitNone;
            return 0;
        case WM_LBUTTONDOWN:
            UpdateHover(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            g_pressed = g_hover;
            SetCapture(h);
            if (g_hover >= HitTune0 && g_hover <= HitTune3)
            {
                g_dragTune = g_hover - HitTune0;
                SetTune(g_dragTune, TuneValueAt(g_dragTune, GET_X_LPARAM(lp)));
            }
            else if (g_hover == HitNone)
            {
                // Clicked outside the card (shadow margin): treat as dismiss.
                float cx = GET_X_LPARAM(lp) / g_scale - ML, cy = GET_Y_LPARAM(lp) / g_scale - CardTop();
                if (cx < 0 || cy < 0 || cx > CW || cy > g_cardH) { ReleaseCapture(); flyout::Hide(); }
            }
            return 0;
        case WM_LBUTTONUP:
        {
            ReleaseCapture();
            int pressed = g_pressed;
            g_pressed = HitNone;
            if (g_dragTune >= 0) { g_dragTune = -1; SaveSettings(); return 0; }
            UpdateHover(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            if (pressed != HitNone && pressed == g_hover) Activate(pressed);
            return 0;
        }
        case WM_MOUSEWHEEL:
        {
            int dir = GET_WHEEL_DELTA_WPARAM(wp) > 0 ? -1 : 1;
            POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            ScreenToClient(h, &pt);
            float cy = pt.y / g_scale - CardTop();
            UpdateHover(pt.x, pt.y);
            if (cy < HeroH) StepScene(dir);           // over the preview: browse scenes
            else if (g_tuning)                        // tuning: nudge the tile under the pointer
            {
                if (g_hover < HitTune0 || g_hover > HitTune3) return 0;
                auto& s = g_settings;
                int row = g_hover - HitTune0;
                if (row == 0) s.gridSources = std::clamp(s.gridSources - dir, 1, 8);
                else
                {
                    int& v = row == 1 ? s.gridIntensity : row == 2 ? s.gridPace : s.gridSurges;
                    v = std::clamp(v - 5 * dir, 0, 100);
                }
                SaveSettings();
            }
            else SetPreset(PresetIndex(g_settings.minutes) - dir); // elsewhere: length
            return 0;
        }
        case WM_KEYDOWN:
            switch (wp)
            {
            case VK_ESCAPE: if (g_tuning) g_tuning = false; else flyout::Hide(); return 0;
            case VK_RETURN: case VK_SPACE: BeginLull(g_settings.minutes * 60); return 0;
            case VK_LEFT: StepScene(-1); return 0;
            case VK_RIGHT: StepScene(1); return 0;
            case VK_UP: SetPreset(PresetIndex(g_settings.minutes) + 1); return 0;
            case VK_DOWN: SetPreset(PresetIndex(g_settings.minutes) - 1); return 0;
            case 'S': g_settings.shuffle = !g_settings.shuffle; SaveSettings(); return 0;
            }
            return 0;
        case WM_ACTIVATE:
            if (LOWORD(wp) == WA_INACTIVE && !g_menuOpen && g_visible && !g_closing)
            {
                g_lastHideTick = GetTickCount();
                flyout::Hide();
            }
            return 0;
        case WM_SETCURSOR:
            SetCursor(LoadCursorW(nullptr, IDC_ARROW));
            return TRUE;
        case WM_ERASEBKGND:
            return 1;
        case WM_DPICHANGED:
            return 0;
        }
        return DefWindowProcW(h, msg, wp, lp);
    }

    bool EnsureWindow()
    {
        if (g_hwnd) return true;
        WNDCLASSEXW wc{ sizeof(wc) };
        wc.lpfnWndProc = WndProc;
        wc.hInstance = g_hinst;
        wc.lpszClassName = kClass;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.style = CS_DBLCLKS;
        RegisterClassExW(&wc);
        g_hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOREDIRECTIONBITMAP, kClass, L"Lull",
                                 WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, g_hinst, nullptr);
        return g_hwnd != nullptr;
    }

    void RenderFrame(double now)
    {
        float dt = (float)std::clamp(now - g_lastFrame, 0.0, 0.1);
        g_lastFrame = now;
        Animate(dt);

        float a = Clamp01((float)((now - g_animStart) / (g_closing ? 0.12 : 0.26)));
        float opacity, yOff;
        if (g_closing) { opacity = 1 - EaseOutCubic(a); yOff = 8 * EaseOutCubic(a); }
        else { opacity = EaseOutCubic(a); yOff = 16 * (1 - EaseOutCubic(a)); }

        RenderScenes(now);
        auto dc = gfx::DC();
        EnsureEffects(dc); // draws offscreen, so it must happen outside BeginDraw/EndDraw
        dc->SetTarget(g_surf.bmp.Get());
        dc->SetDpi((float)g_dpi, (float)g_dpi);
        dc->BeginDraw();
        dc->Clear(D2D1::ColorF(0, 0, 0, 0));
        Draw(dc, opacity, yOff);
        if (dc->EndDraw() == D2DERR_RECREATE_TARGET) gfx::MarkLost();
        dc->SetTarget(nullptr);
        g_surf.Present(1);

        if (g_closing && a >= 1)
        {
            ShowWindow(g_hwnd, SW_HIDE);
            g_visible = false;
            g_closing = false;
        }
    }

    void ResetUiState()
    {
        g_hover = g_pressed = HitNone;
        for (auto& v : g_hoverAnim) v = 0;
        g_heroHover = 0;
        for (auto& v : g_toggleAnim) v = -1;
        g_segAnim = -1;
        g_lookAnim = -1;
        g_tuning = false;
        g_tuneAnim = 0;
        g_cardH = CHMain;
        g_dragTune = -1;
        g_show = g_showPrev = -1;
        g_switchAt = -10;
        // Lengths are presets now; snap older settings to the nearest one.
        g_settings.minutes = kPresets[PresetIndex(g_settings.minutes)];
    }
}

bool flyout::Visible() { return g_visible; }
bool flyout::Animating() { return g_visible; }

void flyout::Toggle(POINT anchor)
{
    if (g_visible && !g_closing) { Hide(); return; }
    // A click on the tray icon first deactivates (and closes) the flyout; don't reopen it.
    if (GetTickCount() - g_lastHideTick < 350) return;
    Show(anchor);
}

void flyout::Show(POINT anchor)
{
    if (overlay::Active()) return;
    if (!gfx::Init() || !EnsureWindow()) return;
    if (g_appStart == 0) g_appStart = gfx::Now();
    CreateFonts();

    HMONITOR mon = MonitorFromPoint(anchor, MONITOR_DEFAULTTONEAREST);
    UINT dx = 96, dy = 96;
    GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dx, &dy);
    g_dpi = dx;
    g_scale = g_dpi / 96.f;
    MONITORINFO mi{ sizeof(mi) };
    GetMonitorInfoW(mon, &mi);
    RECT work = mi.rcWork;

    int cardW = (int)lroundf(CW * g_scale), cardH = (int)lroundf(CHMax * g_scale), gap = (int)lroundf(12 * g_scale);
    int x = std::clamp((int)anchor.x - cardW / 2, (int)work.left + gap, (int)work.right - gap - cardW);
    APPBARDATA abd{ sizeof(abd) };
    bool taskbarTop = SHAppBarMessage(ABM_GETTASKBARPOS, &abd) && abd.uEdge == ABE_TOP &&
                      MonitorFromRect(&abd.rc, MONITOR_DEFAULTTONEAREST) == mon;
    int y = taskbarTop ? work.top + gap : work.bottom - gap - cardH;
    g_anchorBottom = !taskbarTop;

    int winW = (int)ceilf((CW + ML + MR) * g_scale), winH = (int)ceilf((CHMax + MT + MB) * g_scale);
    int winX = x - (int)lroundf(ML * g_scale), winY = y - (int)lroundf(MT * g_scale);
    SetWindowPos(g_hwnd, HWND_TOPMOST, winX, winY, winW, winH, SWP_NOACTIVATE);

    if (!g_surf.swap) { if (!g_surf.Create(g_hwnd, winW, winH)) return; }
    else g_surf.Resize(winW, winH);

    g_visible = true;
    g_closing = false;
    ResetUiState();
    g_animStart = g_lastFrame = gfx::Now();
    RenderFrame(g_animStart);
    ShowWindow(g_hwnd, SW_SHOW);
    SetForegroundWindow(g_hwnd);
}

void flyout::Hide(bool instant)
{
    if (!g_visible) return;
    if (GetCapture() == g_hwnd) ReleaseCapture();
    if (instant)
    {
        ShowWindow(g_hwnd, SW_HIDE);
        g_visible = false;
        g_closing = false;
        return;
    }
    if (!g_closing)
    {
        g_closing = true;
        g_animStart = gfx::Now();
    }
}

void flyout::Frame()
{
    if (!g_visible || !g_surf.swap) return;
    RenderFrame(gfx::Now());
}

void flyout::ReleaseGraphics()
{
    if (g_visible) Hide(true);
    g_surf.Reset();
    for (auto& t : g_hero) t.Reset();
    for (auto& t : g_bg) t.Reset();
    g_show = g_showPrev = -1;
    g_blur.Reset();
    g_shadow.Reset();
    g_shadowScale = 0;
}

bool flyout::SnapshotPng(const wchar_t* path, bool tuneSheet)
{
    if (!gfx::Init()) return false;
    CreateFonts();
    g_dpi = 192;
    g_scale = 2.f;
    g_appStart = gfx::Now() - 12.0;
    int w = (int)ceilf((CW + ML + MR) * g_scale), h = (int)ceilf((CHMax + MT + MB) * g_scale);

    D3D11_TEXTURE2D_DESC td{};
    td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1; td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> tex;
    if (FAILED(gfx::Device()->CreateTexture2D(&td, nullptr, &tex))) return false;
    ComPtr<IDXGISurface> surf;
    tex.As(&surf);
    ComPtr<ID2D1Bitmap1> target;
    auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 192, 192);
    gfx::DC()->CreateBitmapFromDxgiSurface(surf.Get(), &props, &target);

    ResetUiState();
    g_hover = HitHero; // show the chevrons as if the pointer were over the preview
    g_tuning = tuneSheet;
    g_tuneAnim = tuneSheet ? 1.f : 0.f;
    g_cardH = tuneSheet ? CHTune : CHMain;
    g_heroHover = 1;
    g_toggleAnim[0] = g_settings.showCountdown ? 1.f : 0.f;
    g_toggleAnim[1] = g_settings.reminders ? 1.f : 0.f;
    g_toggleAnim[2] = g_settings.muteSounds ? 1.f : 0.f;
    RenderScenes(gfx::Now());

    auto dc = gfx::DC();
    EnsureEffects(dc);
    dc->SetTarget(target.Get());
    dc->SetDpi(192, 192);
    dc->BeginDraw();
    dc->Clear(D2D1::ColorF(0, 0, 0, 0)); // transparent, shadow included
    Draw(dc, 1.f, 0.f);
    dc->EndDraw();
    dc->SetTarget(nullptr);
    return gfx::SaveTexturePng(tex.Get(), path);
}

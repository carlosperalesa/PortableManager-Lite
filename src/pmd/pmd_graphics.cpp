#include "pmd/pmd_graphics.h"

#include <objidl.h>
#include <shellapi.h>
#include <commctrl.h>
#include <commoncontrols.h>
#include <shlobj.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace std {
using std::min;
using std::max;
}
#include <gdiplus.h>

namespace pm::pmd::gfx {
namespace {

Gdiplus::Color ToGp(Color4 c) {
    return Gdiplus::Color(c.a, c.r, c.g, c.b);
}

void AddRoundedRect(Gdiplus::GraphicsPath& path, float x, float y, float w, float h, float r) {
    const float max_r = std::min(w, h) * 0.5f;
    const float radius = std::clamp(r, 0.5f, max_r);
    const float d = radius * 2.0f;
    path.Reset();
    path.AddArc(x, y, d, d, 180.0f, 90.0f);
    path.AddArc(x + w - d, y, d, d, 270.0f, 90.0f);
    path.AddArc(x + w - d, y + h - d, d, d, 0.0f, 90.0f);
    path.AddArc(x, y + h - d, d, d, 90.0f, 90.0f);
    path.CloseFigure();
}

std::unique_ptr<Gdiplus::FontFamily> CreateUiFontFamily() {
    auto family = std::make_unique<Gdiplus::FontFamily>(L"Segoe UI Variable Display");
    if (family->IsAvailable()) return family;
    family = std::make_unique<Gdiplus::FontFamily>(L"Segoe UI");
    if (family->IsAvailable()) return family;
    return std::unique_ptr<Gdiplus::FontFamily>(Gdiplus::FontFamily::GenericSansSerif()->Clone());
}

void ConfigureGraphics(Gdiplus::Graphics& g) {
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
    g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
    g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
}

HICON ExtractHighResIcon(const std::wstring& resolved_path, bool is_app, bool is_folder, int size) {
    if (resolved_path.empty()) return nullptr;
    if (is_app) {
        HICON icon = nullptr;
        if (::PrivateExtractIconsW(resolved_path.c_str(), 0, size, size, &icon, nullptr, 1, 0) > 0 && icon)
            return icon;
        ::ExtractIconExW(resolved_path.c_str(), 0, size >= 28 ? &icon : nullptr, size < 28 ? &icon : nullptr, 1);
        if (icon) return icon;
    }

    const DWORD attrs = is_folder ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
    SHFILEINFOW sfi{};
    if (size >= 32) {
        if (::SHGetFileInfoW(resolved_path.c_str(), attrs, &sfi, sizeof(sfi),
                             SHGFI_SYSICONINDEX | SHGFI_USEFILEATTRIBUTES)) {
            IImageList* image_list = nullptr;
            const int list_type = size >= 44 ? SHIL_EXTRALARGE : SHIL_LARGE;
            constexpr IID kIImageListIid = {0x46EB5926, 0x582E, 0x4017, {0x9F, 0xDF, 0xE8, 0x99, 0x8D, 0xAA, 0x09, 0x50}};
            if (SUCCEEDED(::SHGetImageList(list_type, kIImageListIid, reinterpret_cast<void**>(&image_list))) && image_list) {
                HICON icon = nullptr;
                image_list->GetIcon(sfi.iIcon, ILD_TRANSPARENT, &icon);
                image_list->Release();
                if (icon) return icon;
            }
        }
    }

    const UINT flags = SHGFI_ICON | SHGFI_USEFILEATTRIBUTES | (size >= 28 ? SHGFI_LARGEICON : SHGFI_SMALLICON);
    if (::SHGetFileInfoW(resolved_path.c_str(), attrs, &sfi, sizeof(sfi), flags))
        return sfi.hIcon;
    return nullptr;
}

void DrawIconAlphaAware(Gdiplus::Graphics& g, HICON icon, int x, int y, int size) {
    if (!icon) return;

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = size;
    bmi.bmiHeader.biHeight = -size;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    const HDC screen = ::GetDC(nullptr);
    const HDC memdc = ::CreateCompatibleDC(screen);
    const HBITMAP dib = ::CreateDIBSection(screen, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (screen) ::ReleaseDC(nullptr, screen);

    if (!memdc || !dib || !bits) {
        if (dib) ::DeleteObject(dib);
        if (memdc) ::DeleteDC(memdc);
        std::unique_ptr<Gdiplus::Bitmap> bmp(Gdiplus::Bitmap::FromHICON(icon));
        if (bmp) g.DrawImage(bmp.get(), x, y, size, size);
        return;
    }

    const HGDIOBJ old = ::SelectObject(memdc, dib);
    ::DrawIconEx(memdc, 0, 0, icon, size, size, 0, nullptr, DI_NORMAL);
    ::SelectObject(memdc, old);

    const auto* pixels = static_cast<const std::uint32_t*>(bits);
    const std::size_t count = static_cast<std::size_t>(size) * static_cast<std::size_t>(size);
    bool has_alpha = false;
    for (std::size_t i = 0; i < count; ++i) {
        if ((pixels[i] & 0xFF000000u) != 0) {
            has_alpha = true;
            break;
        }
    }

    if (has_alpha) {
        Gdiplus::Bitmap bmp(size, size, size * 4, PixelFormat32bppARGB, static_cast<BYTE*>(bits));
        g.DrawImage(&bmp, x, y, size, size);
    } else {
        std::unique_ptr<Gdiplus::Bitmap> bmp(Gdiplus::Bitmap::FromHICON(icon));
        if (bmp) g.DrawImage(bmp.get(), x, y, size, size);
    }

    ::DeleteObject(dib);
    ::DeleteDC(memdc);
}

} // namespace

GdiPlusSession::GdiPlusSession() noexcept {
    Gdiplus::GdiplusStartupInput input{};
    ok_ = Gdiplus::GdiplusStartup(&token_, &input, nullptr) == Gdiplus::Ok;
}

GdiPlusSession::~GdiPlusSession() {
    if (ok_) Gdiplus::GdiplusShutdown(token_);
}

Surface::~Surface() {
    Release();
}

void Surface::Release() {
    if (dc_ && old_bitmap_) ::SelectObject(dc_, old_bitmap_);
    if (bitmap_) ::DeleteObject(bitmap_);
    if (dc_) ::DeleteDC(dc_);
    dc_ = nullptr;
    bitmap_ = nullptr;
    old_bitmap_ = nullptr;
    bits_ = nullptr;
    width_ = 0;
    height_ = 0;
}

bool Surface::Resize(int width, int height) {
    if (width <= 0 || height <= 0) return false;
    if (width == width_ && height == height_ && dc_ && bits_) return true;
    Release();

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    const HDC screen = ::GetDC(nullptr);
    dc_ = ::CreateCompatibleDC(screen);
    bitmap_ = ::CreateDIBSection(screen, &bmi, DIB_RGB_COLORS, &bits_, nullptr, 0);
    if (screen) ::ReleaseDC(nullptr, screen);

    if (!dc_ || !bitmap_ || !bits_) {
        Release();
        return false;
    }
    old_bitmap_ = ::SelectObject(dc_, bitmap_);
    width_ = width;
    height_ = height;
    return true;
}

void Surface::Clear() {
    if (!bits_ || width_ <= 0 || height_ <= 0) return;
    std::fill_n(static_cast<std::uint32_t*>(bits_),
                static_cast<std::size_t>(width_) * static_cast<std::size_t>(height_), 0u);
}

void EnablePerMonitorDpiAwareness() {
    const auto user32 = ::GetModuleHandleW(L"user32.dll");
    if (!user32) return;
    using SetAwarenessFn = BOOL(WINAPI*)(HANDLE);
    const auto fn = reinterpret_cast<SetAwarenessFn>(
        reinterpret_cast<void*>(::GetProcAddress(user32, "SetProcessDpiAwarenessContext")));
    if (fn) {
        // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 = (HANDLE)-4
        fn(reinterpret_cast<HANDLE>(static_cast<INT_PTR>(-4)));
    }
}

UINT WindowDpi(HWND hwnd) {
    if (hwnd) {
        const auto user32 = ::GetModuleHandleW(L"user32.dll");
        using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
        const auto fn = user32
            ? reinterpret_cast<GetDpiForWindowFn>(reinterpret_cast<void*>(::GetProcAddress(user32, "GetDpiForWindow")))
            : nullptr;
        if (fn) {
            const UINT dpi = fn(hwnd);
            if (dpi >= 72) return dpi;
        }
        if (const HDC dc = ::GetDC(hwnd)) {
            const int log_y = ::GetDeviceCaps(dc, LOGPIXELSY);
            ::ReleaseDC(hwnd, dc);
            if (log_y >= 72) return static_cast<UINT>(log_y);
        }
    }
    return 96;
}

void DrawGlassCapsule(HDC hdc, const RECT& bounds, float corner_radius, std::uint8_t fill_alpha, UINT dpi) {
    Gdiplus::Graphics g(hdc);
    ConfigureGraphics(g);

    const float bx = static_cast<float>(bounds.left);
    const float by = static_cast<float>(bounds.top);
    const float bw = static_cast<float>(bounds.right - bounds.left);
    const float bh = static_cast<float>(bounds.bottom - bounds.top);

    // Multi-layer soft drop shadow outside the capsule
    const int shadow_layers = 6;
    for (int i = shadow_layers; i >= 1; --i) {
        const float spread = ScaleF(static_cast<float>(i) * 1.4f, dpi);
        const float y_off = ScaleF(static_cast<float>(i) * 0.45f, dpi);
        const std::uint8_t alpha = static_cast<std::uint8_t>(std::max(2, 22 - i * 3));
        Gdiplus::GraphicsPath sp;
        AddRoundedRect(sp, bx - spread * 0.5f, by - spread * 0.35f + y_off,
                       bw + spread, bh + spread * 0.9f, corner_radius + spread * 0.45f);
        Gdiplus::SolidBrush sb(Gdiplus::Color(alpha, 0, 0, 0));
        g.FillPath(&sb, &sp);
    }

    // Main Dark Obsidian Glass body
    Gdiplus::GraphicsPath body;
    AddRoundedRect(body, bx, by, bw, bh, corner_radius);

    Gdiplus::LinearGradientBrush bg(
        Gdiplus::PointF(bx, by), Gdiplus::PointF(bx, by + bh),
        Gdiplus::Color(fill_alpha, 23, 28, 38),
        Gdiplus::Color(fill_alpha, 14, 18, 25));
    g.FillPath(&bg, &body);

    // Subtle specular top highlight gradient
    Gdiplus::LinearGradientBrush rim(
        Gdiplus::PointF(bx, by), Gdiplus::PointF(bx, by + bh),
        Gdiplus::Color(46, 255, 255, 255),
        Gdiplus::Color(16, 255, 255, 255));
    Gdiplus::Pen inner_pen(&rim, 1.0f);
    Gdiplus::GraphicsPath inner;
    AddRoundedRect(inner, bx + 0.5f, by + 0.5f, bw - 1.0f, bh - 1.0f, std::max(1.0f, corner_radius - 0.5f));
    g.DrawPath(&inner_pen, &inner);

    // Crisp outer dark border for edge definition on light wallpapers
    Gdiplus::Pen outer_pen(Gdiplus::Color(140, 6, 8, 12), 1.0f);
    g.DrawPath(&outer_pen, &body);
}

void DrawCapsuleProgress(HDC hdc, const RECT& bounds, float corner_radius, float fraction, Color4 color) {
    Gdiplus::Graphics g(hdc);
    ConfigureGraphics(g);

    const float bx = static_cast<float>(bounds.left) + 1.0f;
    const float by = static_cast<float>(bounds.top) + 1.0f;
    const float bw = static_cast<float>(bounds.right - bounds.left) - 2.0f;
    const float bh = static_cast<float>(bounds.bottom - bounds.top) - 2.0f;
    if (bw <= 0.0f || bh <= 0.0f) return;

    Gdiplus::GraphicsPath clip;
    AddRoundedRect(clip, bx, by, bw, bh, std::max(1.0f, corner_radius - 1.0f));
    g.SetClip(&clip);

    const float clamped = std::clamp(fraction, 0.08f, 1.0f);
    const float pw = bw * clamped;
    Gdiplus::LinearGradientBrush fill(
        Gdiplus::PointF(bx, by), Gdiplus::PointF(bx + pw, by),
        Gdiplus::Color(static_cast<std::uint8_t>(color.a / 3), color.r, color.g, color.b),
        Gdiplus::Color(color.a, color.r, color.g, color.b));
    g.FillRectangle(&fill, bx, by, pw, bh);
}

void DrawGlowingLed(HDC hdc, float cx, float cy, float radius, Color4 color, bool hovered) {
    Gdiplus::Graphics g(hdc);
    ConfigureGraphics(g);

    // Outer radial glow
    const float glow_r = radius * (hovered ? 3.2f : 2.7f);
    Gdiplus::GraphicsPath glow_path;
    glow_path.AddEllipse(cx - glow_r, cy - glow_r, glow_r * 2.0f, glow_r * 2.0f);
    Gdiplus::PathGradientBrush pgb(&glow_path);
    pgb.SetCenterColor(Gdiplus::Color(hovered ? 165 : 130, color.r, color.g, color.b));
    Gdiplus::Color surround(0, color.r, color.g, color.b);
    int count = 1;
    pgb.SetSurroundColors(&surround, &count);
    g.FillPath(&pgb, &glow_path);

    // Core LED sphere
    Gdiplus::SolidBrush core(ToGp(color));
    g.FillEllipse(&core, cx - radius, cy - radius, radius * 2.0f, radius * 2.0f);

    // Tiny top specular glint
    Gdiplus::SolidBrush glint(Gdiplus::Color(115, 255, 255, 255));
    g.FillEllipse(&glint, cx - radius * 0.45f, cy - radius * 0.65f, radius * 0.7f, radius * 0.45f);
}

void DrawStatusShield(HDC hdc, float cx, float cy, float size, Color4 color) {
    Gdiplus::Graphics g(hdc);
    ConfigureGraphics(g);

    const float half = size * 0.5f;
    // Soft glow behind the shield
    const float glow_r = size * 0.95f;
    Gdiplus::GraphicsPath glow_path;
    glow_path.AddEllipse(cx - glow_r, cy - glow_r, glow_r * 2.0f, glow_r * 2.0f);
    Gdiplus::PathGradientBrush pgb(&glow_path);
    pgb.SetCenterColor(Gdiplus::Color(85, color.r, color.g, color.b));
    Gdiplus::Color surround(0, color.r, color.g, color.b);
    int count = 1;
    pgb.SetSurroundColors(&surround, &count);
    g.FillPath(&pgb, &glow_path);

    // Smooth shield path
    Gdiplus::GraphicsPath shield;
    const Gdiplus::PointF pts[] = {
        {cx, cy - half},
        {cx + half * 0.88f, cy - half * 0.68f},
        {cx + half * 0.80f, cy + half * 0.16f},
        {cx + half * 0.42f, cy + half * 0.68f},
        {cx, cy + half},
        {cx - half * 0.42f, cy + half * 0.68f},
        {cx - half * 0.80f, cy + half * 0.16f},
        {cx - half * 0.88f, cy - half * 0.68f},
    };
    shield.AddPolygon(pts, static_cast<int>(std::size(pts)));

    Gdiplus::LinearGradientBrush fill(
        Gdiplus::PointF(cx, cy - half), Gdiplus::PointF(cx, cy + half),
        Gdiplus::Color(255, static_cast<BYTE>(std::min(255, color.r + 25)),
                       static_cast<BYTE>(std::min(255, color.g + 25)),
                       static_cast<BYTE>(std::min(255, color.b + 25))),
        Gdiplus::Color(255, static_cast<BYTE>(color.r * 0.78f),
                       static_cast<BYTE>(color.g * 0.78f),
                       static_cast<BYTE>(color.b * 0.78f)));
    g.FillPath(&fill, &shield);

    Gdiplus::Pen rim(Gdiplus::Color(150, 255, 255, 255), 1.0f);
    g.DrawPath(&rim, &shield);
}

void DrawInstallIcon(HDC hdc, float cx, float cy, float size, Color4 color, bool hovered) {
    Gdiplus::Graphics g(hdc);
    ConfigureGraphics(g);

    const Color4 c{
        static_cast<std::uint8_t>(hovered ? std::min(255, color.r + 25) : color.r),
        static_cast<std::uint8_t>(hovered ? std::min(255, color.g + 25) : color.g),
        static_cast<std::uint8_t>(hovered ? std::min(255, color.b + 25) : color.b),
        255};
    Gdiplus::Pen pen(ToGp(c), std::max(1.5f, size * 0.12f));
    pen.SetStartCap(Gdiplus::LineCapRound);
    pen.SetEndCap(Gdiplus::LineCapRound);
    pen.SetLineJoin(Gdiplus::LineJoinRound);

    const float h = size * 0.44f;
    // Vertical shaft
    g.DrawLine(&pen, cx, cy - h, cx, cy + h * 0.45f);
    // Arrowhead
    g.DrawLine(&pen, cx - h * 0.55f, cy - h * 0.05f, cx, cy + h * 0.48f);
    g.DrawLine(&pen, cx + h * 0.55f, cy - h * 0.05f, cx, cy + h * 0.48f);
    // Tray bottom
    const Gdiplus::PointF tray[] = {
        {cx - h * 0.9f, cy + h * 0.45f},
        {cx - h * 0.9f, cy + h * 0.92f},
        {cx + h * 0.9f, cy + h * 0.92f},
        {cx + h * 0.9f, cy + h * 0.45f},
    };
    g.DrawLines(&pen, tray, 4);
}

void DrawSearchIcon(HDC hdc, float cx, float cy, float size, Color4 color) {
    Gdiplus::Graphics g(hdc);
    ConfigureGraphics(g);

    Gdiplus::Pen pen(ToGp(color), std::max(1.4f, size * 0.12f));
    pen.SetStartCap(Gdiplus::LineCapRound);
    pen.SetEndCap(Gdiplus::LineCapRound);

    const float r = size * 0.32f;
    const float ox = cx - size * 0.10f;
    const float oy = cy - size * 0.10f;
    g.DrawEllipse(&pen, ox - r, oy - r, r * 2.0f, r * 2.0f);
    g.DrawLine(&pen, ox + r * 0.72f, oy + r * 0.72f, cx + size * 0.42f, cy + size * 0.42f);
}

void DrawWebIcon(HDC hdc, float cx, float cy, float size, Color4 color, bool hovered) {
    Gdiplus::Graphics g(hdc);
    ConfigureGraphics(g);

    const Color4 c = hovered ? Color4{96, 165, 250, 255} : color;
    Gdiplus::Pen pen(ToGp(c), std::max(1.2f, size * 0.09f));
    const float r = size * 0.42f;
    g.DrawEllipse(&pen, cx - r, cy - r, r * 2.0f, r * 2.0f);
    g.DrawEllipse(&pen, cx - r * 0.45f, cy - r, r * 0.9f, r * 2.0f);
    g.DrawLine(&pen, cx - r, cy, cx + r, cy);
}

void DrawBackIcon(HDC hdc, float cx, float cy, float size, Color4 color, bool hovered) {
    Gdiplus::Graphics g(hdc);
    ConfigureGraphics(g);

    const Color4 c = hovered ? Color4{248, 250, 252, 255} : color;
    Gdiplus::Pen pen(ToGp(c), std::max(1.5f, size * 0.13f));
    pen.SetStartCap(Gdiplus::LineCapRound);
    pen.SetEndCap(Gdiplus::LineCapRound);
    pen.SetLineJoin(Gdiplus::LineJoinRound);

    const float h = size * 0.38f;
    g.DrawLine(&pen, cx + h, cy, cx - h, cy);
    g.DrawLine(&pen, cx - h * 0.15f, cy - h * 0.75f, cx - h, cy);
    g.DrawLine(&pen, cx - h * 0.15f, cy + h * 0.75f, cx - h, cy);
}

void DrawRoundedHighlight(HDC hdc, float x, float y, float w, float h, float radius, Color4 fill, Color4 border) {
    Gdiplus::Graphics g(hdc);
    ConfigureGraphics(g);
    Gdiplus::GraphicsPath path;
    AddRoundedRect(path, x, y, w, h, radius);
    if (fill.a > 0) {
        Gdiplus::SolidBrush sb(ToGp(fill));
        g.FillPath(&sb, &path);
    }
    if (border.a > 0) {
        Gdiplus::Pen pen(ToGp(border), 1.0f);
        g.DrawPath(&pen, &path);
    }
}

void DrawAccentBar(HDC hdc, float x, float y, float w, float h, Color4 color) {
    Gdiplus::Graphics g(hdc);
    ConfigureGraphics(g);
    Gdiplus::GraphicsPath path;
    AddRoundedRect(path, x, y, w, h, w * 0.5f);
    Gdiplus::SolidBrush sb(ToGp(color));
    g.FillPath(&sb, &path);
}

void DrawDivider(HDC hdc, float x1, float x2, float y, std::uint8_t alpha) {
    Gdiplus::Graphics g(hdc);
    Gdiplus::Pen pen(Gdiplus::Color(alpha, 255, 255, 255), 1.0f);
    g.DrawLine(&pen, x1, y, x2, y);
}

void DrawTextLine(HDC hdc, std::wstring_view text, const RECT& rect, float font_px, bool semibold,
                  Color4 color, int align_h) {
    if (text.empty()) return;
    Gdiplus::Graphics g(hdc);
    ConfigureGraphics(g);
    const auto family = CreateUiFontFamily();
    const Gdiplus::Font font(family.get(), font_px,
                             semibold ? Gdiplus::FontStyleBold : Gdiplus::FontStyleRegular,
                             Gdiplus::UnitPixel);
    Gdiplus::StringFormat fmt;
    fmt.SetAlignment(align_h < 0 ? Gdiplus::StringAlignmentNear :
                     align_h > 0 ? Gdiplus::StringAlignmentFar : Gdiplus::StringAlignmentCenter);
    fmt.SetLineAlignment(Gdiplus::StringAlignmentCenter);
    fmt.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
    fmt.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);
    Gdiplus::SolidBrush brush(ToGp(color));
    const Gdiplus::RectF layout(
        static_cast<float>(rect.left), static_cast<float>(rect.top),
        static_cast<float>(rect.right - rect.left), static_cast<float>(rect.bottom - rect.top));
    g.DrawString(text.data(), static_cast<int>(text.size()), &font, layout, &fmt, &brush);
}

void DrawGridLabel(HDC hdc, std::wstring_view text, const RECT& rect, float font_px, Color4 color) {
    if (text.empty()) return;
    Gdiplus::Graphics g(hdc);
    ConfigureGraphics(g);
    const auto family = CreateUiFontFamily();
    const Gdiplus::Font font(family.get(), font_px, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
    Gdiplus::StringFormat fmt;
    fmt.SetAlignment(Gdiplus::StringAlignmentCenter);
    fmt.SetLineAlignment(Gdiplus::StringAlignmentNear);
    fmt.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
    Gdiplus::SolidBrush brush(ToGp(color));
    const Gdiplus::RectF layout(
        static_cast<float>(rect.left), static_cast<float>(rect.top),
        static_cast<float>(rect.right - rect.left), static_cast<float>(rect.bottom - rect.top));
    g.DrawString(text.data(), static_cast<int>(text.size()), &font, layout, &fmt, &brush);
}

float MeasureTextWidth(HDC hdc, std::wstring_view text, float font_px, bool semibold) {
    if (text.empty()) return 0.0f;
    Gdiplus::Graphics g(hdc);
    ConfigureGraphics(g);
    const auto family = CreateUiFontFamily();
    const Gdiplus::Font font(family.get(), font_px,
                             semibold ? Gdiplus::FontStyleBold : Gdiplus::FontStyleRegular,
                             Gdiplus::UnitPixel);
    Gdiplus::StringFormat fmt(Gdiplus::StringFormat::GenericTypographic());
    fmt.SetFormatFlags(fmt.GetFormatFlags() | Gdiplus::StringFormatFlagsMeasureTrailingSpaces);
    Gdiplus::RectF box{};
    g.MeasureString(text.data(), static_cast<int>(text.size()), &font, Gdiplus::PointF(0.0f, 0.0f), &fmt, &box);
    return box.Width;
}

void DrawSearchText(HDC hdc, std::wstring_view typed, std::wstring_view ghost,
                    int sel_start, int sel_end, bool caret_visible, bool focused,
                    const RECT& rect, float font_px, UINT dpi) {
    Gdiplus::Graphics g(hdc);
    ConfigureGraphics(g);
    const auto family = CreateUiFontFamily();
    const Gdiplus::Font font(family.get(), font_px, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
    Gdiplus::StringFormat fmt(Gdiplus::StringFormat::GenericTypographic());
    fmt.SetFormatFlags(fmt.GetFormatFlags() | Gdiplus::StringFormatFlagsNoWrap |
                       Gdiplus::StringFormatFlagsMeasureTrailingSpaces);

    const float x0 = static_cast<float>(rect.left);
    const float max_w = static_cast<float>(rect.right - rect.left);
    const float y_mid = static_cast<float>(rect.top + rect.bottom) * 0.5f;
    const float text_h = font.GetHeight(&g);
    const float y0 = y_mid - text_h * 0.5f;

    const Gdiplus::RectF clip_rect(x0, static_cast<float>(rect.top), max_w, static_cast<float>(rect.bottom - rect.top));
    g.SetClip(clip_rect);

    auto measure = [&](std::wstring_view part) -> float {
        if (part.empty()) return 0.0f;
        Gdiplus::RectF box{};
        g.MeasureString(part.data(), static_cast<int>(part.size()), &font, Gdiplus::PointF(0.0f, 0.0f), &fmt, &box);
        return box.Width;
    };

    const float typed_w = measure(typed);
    const int n = static_cast<int>(typed.size());
    const int s0 = std::clamp(std::min(sel_start, sel_end), 0, n);
    const int s1 = std::clamp(std::max(sel_start, sel_end), 0, n);

    if (s1 > s0) {
        const float x_a = x0 + measure(typed.substr(0, static_cast<std::size_t>(s0)));
        const float x_b = x0 + measure(typed.substr(0, static_cast<std::size_t>(s1)));
        Gdiplus::SolidBrush sel_brush(Gdiplus::Color(115, 59, 130, 246));
        g.FillRectangle(&sel_brush, x_a, y0 - 1.0f, std::max(2.0f, x_b - x_a), text_h + 2.0f);
    }

    if (typed.empty()) {
        Gdiplus::SolidBrush placeholder(Gdiplus::Color(120, 148, 163, 184));
        const std::wstring_view hint = L"Buscar\u2026";
        g.DrawString(hint.data(), static_cast<int>(hint.size()), &font, Gdiplus::PointF(x0, y0), &fmt, &placeholder);
    } else {
        Gdiplus::SolidBrush white(Gdiplus::Color(255, 248, 250, 252));
        g.DrawString(typed.data(), static_cast<int>(typed.size()), &font, Gdiplus::PointF(x0, y0), &fmt, &white);

        if (!ghost.empty()) {
            Gdiplus::SolidBrush muted(Gdiplus::Color(160, 148, 163, 184));
            g.DrawString(ghost.data(), static_cast<int>(ghost.size()), &font,
                         Gdiplus::PointF(x0 + typed_w, y0), &fmt, &muted);
        }
    }

    if (focused && caret_visible) {
        const int caret_pos = std::clamp(sel_end, 0, n);
        const float cx = x0 + measure(typed.substr(0, static_cast<std::size_t>(caret_pos)));
        Gdiplus::Pen caret_pen(Gdiplus::Color(240, 248, 250, 252), ScaleF(1.5f, dpi));
        g.DrawLine(&caret_pen, cx, y0, cx, y0 + text_h);
    }
}

void DrawItemIcon(HDC hdc, const std::wstring& resolved_path, bool is_app, bool is_folder,
                  int x, int y, int size, bool has_privacy_dot, Color4 privacy_color,
                  bool is_running, UINT dpi) {
    Gdiplus::Graphics g(hdc);
    ConfigureGraphics(g);

    if (HICON icon = ExtractHighResIcon(resolved_path, is_app, is_folder, size)) {
        DrawIconAlphaAware(g, icon, x, y, size);
        ::DestroyIcon(icon);
    } else {
        // Fallback crisp vector icon when binary/path icon is unavailable
        const float pad = static_cast<float>(size) * 0.14f;
        Gdiplus::GraphicsPath fb;
        AddRoundedRect(fb, static_cast<float>(x) + pad, static_cast<float>(y) + pad,
                       static_cast<float>(size) - pad * 2.0f, static_cast<float>(size) - pad * 2.0f,
                       ScaleF(6.0f, dpi));
        Gdiplus::SolidBrush fill(Gdiplus::Color(200, 51, 65, 85));
        Gdiplus::Pen pen(Gdiplus::Color(180, 100, 116, 139), 1.0f);
        g.FillPath(&fill, &fb);
        g.DrawPath(&pen, &fb);
    }

    // Running app indicator (subtle emerald dot on top-right corner of the icon itself)
    if (is_running) {
        const float r = ScaleF(3.2f, dpi);
        const float rx = static_cast<float>(x + size) - r * 0.9f;
        const float ry = static_cast<float>(y) + r * 0.9f;
        Gdiplus::SolidBrush ring(Gdiplus::Color(235, 16, 20, 28));
        g.FillEllipse(&ring, rx - r - 1.5f, ry - r - 1.5f, (r + 1.5f) * 2.0f, (r + 1.5f) * 2.0f);
        Gdiplus::SolidBrush dot(Gdiplus::Color(255, 52, 211, 153));
        g.FillEllipse(&dot, rx - r, ry - r, r * 2.0f, r * 2.0f);
    }

    // Privacy dot directly on the bottom-right corner of the icon itself
    if (has_privacy_dot) {
        const float r = std::max(ScaleF(3.0f, dpi), static_cast<float>(size) * 0.135f);
        const float dx = static_cast<float>(x + size) - r * 0.85f;
        const float dy = static_cast<float>(y + size) - r * 0.85f;
        const float border = ScaleF(1.5f, dpi);

        Gdiplus::SolidBrush contrast_ring(Gdiplus::Color(245, 16, 20, 28));
        g.FillEllipse(&contrast_ring, dx - r - border, dy - r - border,
                      (r + border) * 2.0f, (r + border) * 2.0f);

        Gdiplus::SolidBrush dot_brush(ToGp(privacy_color));
        g.FillEllipse(&dot_brush, dx - r, dy - r, r * 2.0f, r * 2.0f);
    }
}

namespace {

struct GlassMenuState {
    const std::vector<MenuEntry>* items = nullptr;
    std::vector<RECT> item_rects;
    UINT dpi = 96;
    int shadow = 0;
    int width = 0;
    int height = 0;
    int hover_index = -1;
    int result_id = 0;
    bool done = false;
    bool tracking_mouse = false;
    Surface surface;

    void Render(HWND hwnd) {
        if (!surface.Resize(width, height)) return;
        surface.Clear();
        const HDC hdc = surface.hdc();
        const float radius = ScaleF(12.0f, dpi);
        const RECT bounds{shadow, shadow, width - shadow, height - shadow};
        const float cx = static_cast<float>(bounds.left);
        const float cw = static_cast<float>(bounds.right - bounds.left);

        DrawGlassCapsule(hdc, bounds, radius, 244, dpi);

        const float font_px = ScaleF(13.0f, dpi);
        const float pad_x = ScaleF(12.0f, dpi);
        for (std::size_t i = 0; i < items->size(); ++i) {
            const auto& entry = (*items)[i];
            const RECT& rc = item_rects[i];
            if (entry.separator) {
                const float sy = static_cast<float>(rc.top + rc.bottom) * 0.5f;
                DrawDivider(hdc, cx + pad_x, cx + cw - pad_x, sy, 26);
                continue;
            }
            const bool hovered = (static_cast<int>(i) == hover_index) && entry.enabled;
            if (hovered) {
                DrawRoundedHighlight(hdc,
                                     static_cast<float>(rc.left), static_cast<float>(rc.top),
                                     static_cast<float>(rc.right - rc.left),
                                     static_cast<float>(rc.bottom - rc.top),
                                     ScaleF(7.0f, dpi),
                                     Color4{59, 130, 246, 62}, Color4{96, 165, 250, 95});
            }
            RECT text_rc{rc.left + Scale(10, dpi), rc.top, rc.right - Scale(10, dpi), rc.bottom};
            const Color4 fg = entry.enabled ? Color4{241, 245, 249, 255}
                                            : Color4{148, 163, 184, 110};
            DrawTextLine(hdc, entry.label, text_rc, font_px, false, fg, -1);
        }

        RECT wnd_rc{};
        ::GetWindowRect(hwnd, &wnd_rc);
        POINT dst{wnd_rc.left, wnd_rc.top};
        SIZE sz{width, height};
        POINT src{0, 0};
        BLENDFUNCTION bf{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        ::UpdateLayeredWindow(hwnd, nullptr, &dst, &sz, hdc, &src, 0, &bf, ULW_ALPHA);
    }

    int HitTest(int x, int y) const {
        const POINT pt{x, y};
        for (std::size_t i = 0; i < items->size(); ++i) {
            if (!(*items)[i].separator && ::PtInRect(&item_rects[i], pt)) {
                return static_cast<int>(i);
            }
        }
        return -1;
    }

    void MoveSelection(int delta) {
        if (items->empty()) return;
        const int count = static_cast<int>(items->size());
        int cur = hover_index;
        for (int step = 0; step < count; ++step) {
            cur = (cur + delta + count) % count;
            if (!(*items)[static_cast<std::size_t>(cur)].separator &&
                (*items)[static_cast<std::size_t>(cur)].enabled) {
                hover_index = cur;
                return;
            }
        }
    }
};

LRESULT CALLBACK GlassMenuProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* state = reinterpret_cast<GlassMenuState*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        state = static_cast<GlassMenuState*>(cs->lpCreateParams);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
        return TRUE;
    }
    if (!state) return ::DefWindowProcW(hwnd, msg, wp, lp);

    switch (msg) {
        case WM_MOUSEMOVE: {
            if (!state->tracking_mouse) {
                TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
                ::TrackMouseEvent(&tme);
                state->tracking_mouse = true;
            }
            const int hit = state->HitTest(static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp)));
            if (hit != state->hover_index) {
                state->hover_index = hit;
                state->Render(hwnd);
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            state->tracking_mouse = false;
            if (state->hover_index != -1) {
                state->hover_index = -1;
                state->Render(hwnd);
            }
            return 0;
        case WM_LBUTTONUP:
        case WM_RBUTTONUP: {
            const int hit = state->HitTest(static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp)));
            if (hit >= 0) {
                const auto& entry = (*state->items)[static_cast<std::size_t>(hit)];
                if (entry.enabled) {
                    state->result_id = entry.id;
                    state->done = true;
                    ::PostMessageW(hwnd, WM_NULL, 0, 0);
                }
            } else {
                state->done = true;
                ::PostMessageW(hwnd, WM_NULL, 0, 0);
            }
            return 0;
        }
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE) {
                state->done = true;
                ::PostMessageW(hwnd, WM_NULL, 0, 0);
            } else if (wp == VK_RETURN) {
                if (state->hover_index >= 0) {
                    const auto& entry = (*state->items)[static_cast<std::size_t>(state->hover_index)];
                    if (entry.enabled) {
                        state->result_id = entry.id;
                        state->done = true;
                        ::PostMessageW(hwnd, WM_NULL, 0, 0);
                    }
                }
            } else if (wp == VK_DOWN) {
                state->MoveSelection(1);
                state->Render(hwnd);
            } else if (wp == VK_UP) {
                state->MoveSelection(-1);
                state->Render(hwnd);
            }
            return 0;
        case WM_ACTIVATE:
            if (LOWORD(wp) == WA_INACTIVE) {
                state->done = true;
                ::PostMessageW(hwnd, WM_NULL, 0, 0);
            }
            return 0;
        case WM_KILLFOCUS:
            state->done = true;
            ::PostMessageW(hwnd, WM_NULL, 0, 0);
            return 0;
        default:
            return ::DefWindowProcW(hwnd, msg, wp, lp);
    }
}

} // namespace

int ShowGlassMenu(HWND owner, POINT screen_pt, const std::vector<MenuEntry>& items, UINT dpi) {
    if (items.empty()) return 0;

    static const wchar_t* kClassName = L"PMDGlassMenuWindow";
    static bool registered = false;
    const HINSTANCE instance = ::GetModuleHandleW(nullptr);
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = GlassMenuProc;
        wc.hInstance = instance;
        wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kClassName;
        if (::RegisterClassExW(&wc) || ::GetLastError() == ERROR_CLASS_ALREADY_EXISTS) {
            registered = true;
        } else {
            return 0;
        }
    }

    GlassMenuState state;
    state.items = &items;
    state.dpi = dpi;
    state.shadow = Scale(10, dpi);

    const int pad_x = Scale(6, dpi);
    const int pad_y = Scale(6, dpi);
    const int item_h = Scale(30, dpi);
    const int sep_h = Scale(9, dpi);
    const float font_px = ScaleF(13.0f, dpi);

    HDC screen_dc = ::GetDC(nullptr);
    float max_text_w = 0.0f;
    for (const auto& entry : items) {
        if (!entry.separator) {
            max_text_w = std::max(max_text_w, MeasureTextWidth(screen_dc, entry.label, font_px, false));
        }
    }
    if (screen_dc) ::ReleaseDC(nullptr, screen_dc);

    const int content_w = std::clamp(
        static_cast<int>(std::ceil(max_text_w)) + Scale(38, dpi),
        Scale(164, dpi), Scale(340, dpi));

    int cur_y = state.shadow + pad_y;
    state.item_rects.reserve(items.size());
    for (const auto& entry : items) {
        const int h = entry.separator ? sep_h : item_h;
        state.item_rects.push_back(RECT{
            state.shadow + pad_x,
            cur_y,
            state.shadow + content_w - pad_x,
            cur_y + h
        });
        cur_y += h;
    }
    const int content_h = (cur_y - state.shadow) + pad_y;
    state.width = content_w + state.shadow * 2;
    state.height = content_h + state.shadow * 2;

    int x = screen_pt.x - state.shadow;
    int y = screen_pt.y - state.shadow;
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (::GetMonitorInfoW(::MonitorFromPoint(screen_pt, MONITOR_DEFAULTTONEAREST), &mi)) {
        const RECT& work = mi.rcWork;
        if (x + state.shadow + content_w > work.right) x = work.right - content_w - state.shadow;
        if (y + state.shadow + content_h > work.bottom) y = screen_pt.y - content_h - state.shadow;
        if (x < work.left - state.shadow) x = work.left - state.shadow;
        if (y < work.top - state.shadow) y = work.top - state.shadow;
    }

    HWND hwnd = ::CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        kClassName, L"", WS_POPUP,
        x, y, state.width, state.height,
        owner, nullptr, instance, &state);
    if (!hwnd) return 0;

    state.Render(hwnd);
    ::ShowWindow(hwnd, SW_SHOW);
    ::SetForegroundWindow(hwnd);
    ::SetFocus(hwnd);

    MSG msg{};
    while (!state.done && ::GetMessageW(&msg, nullptr, 0, 0) > 0) {
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }

    ::DestroyWindow(hwnd);
    if (owner && ::IsWindow(owner)) ::SetForegroundWindow(owner);
    return state.result_id;
}

} // namespace pm::pmd::gfx

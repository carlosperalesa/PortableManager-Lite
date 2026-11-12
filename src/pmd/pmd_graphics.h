#pragma once

#include <windows.h>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace pm::pmd::gfx {

// RAII wrapper for GDI+ startup and shutdown.
class GdiPlusSession {
public:
    GdiPlusSession() noexcept;
    ~GdiPlusSession();
    GdiPlusSession(const GdiPlusSession&) = delete;
    GdiPlusSession& operator=(const GdiPlusSession&) = delete;
    bool ok() const noexcept { return ok_; }

private:
    ULONG_PTR token_ = 0;
    bool ok_ = false;
};

// 32-bit premultiplied ARGB DIB buffer suitable for UpdateLayeredWindow.
class Surface {
public:
    Surface() = default;
    ~Surface();
    Surface(const Surface&) = delete;
    Surface& operator=(const Surface&) = delete;

    bool Resize(int width, int height);
    void Clear();
    HDC hdc() const noexcept { return dc_; }
    int width() const noexcept { return width_; }
    int height() const noexcept { return height_; }
    void* bits() const noexcept { return bits_; }

private:
    void Release();
    HDC dc_ = nullptr;
    HBITMAP bitmap_ = nullptr;
    HGDIOBJ old_bitmap_ = nullptr;
    void* bits_ = nullptr;
    int width_ = 0;
    int height_ = 0;
};

struct Color4 {
    std::uint8_t r;
    std::uint8_t g;
    std::uint8_t b;
    std::uint8_t a = 255;
};

// Enables Per-Monitor V2 DPI awareness when available on the host OS.
void EnablePerMonitorDpiAwareness();

// Queries effective window DPI (falls back to 96 if unavailable).
UINT WindowDpi(HWND hwnd);

// Scales a 96-DPI logical value to physical pixels.
inline int Scale(int px, UINT dpi) {
    return ::MulDiv(px, static_cast<int>(dpi), 96);
}

inline float ScaleF(float px, UINT dpi) {
    return px * (static_cast<float>(dpi) / 96.0f);
}

// Draws the outer soft shadow + dark obsidian glass capsule/panel + specular border.
void DrawGlassCapsule(HDC hdc, const RECT& bounds, float corner_radius, std::uint8_t fill_alpha, UINT dpi);

// Draws a progress fill clipped inside the capsule bounds.
void DrawCapsuleProgress(HDC hdc, const RECT& bounds, float corner_radius, float fraction, Color4 color);

// Draws a glowing status LED centered at (cx, cy).
void DrawGlowingLed(HDC hdc, float cx, float cy, float radius, Color4 color, bool hovered = false);

// Draws the heraldic PMG status shield centered at (cx, cy).
void DrawStatusShield(HDC hdc, float cx, float cy, float size, Color4 color);

// Draws a clean vector download-into-tray icon centered at (cx, cy).
void DrawInstallIcon(HDC hdc, float cx, float cy, float size, Color4 color, bool hovered);

// Draws a clean vector magnifying glass icon centered at (cx, cy).
void DrawSearchIcon(HDC hdc, float cx, float cy, float size, Color4 color);

// Draws a clean vector web globe icon centered at (cx, cy).
void DrawWebIcon(HDC hdc, float cx, float cy, float size, Color4 color, bool hovered);

// Draws a clean back arrow icon centered at (cx, cy).
void DrawBackIcon(HDC hdc, float cx, float cy, float size, Color4 color, bool hovered);

// Draws a rounded highlight pill/card (used for selected search rows or active hover).
void DrawRoundedHighlight(HDC hdc, float x, float y, float w, float h, float radius, Color4 fill, Color4 border);

// Draws a vertical accent bar on the left side of the active search row.
void DrawAccentBar(HDC hdc, float x, float y, float w, float h, Color4 color);

// Draws a thin horizontal separator line.
void DrawDivider(HDC hdc, float x1, float x2, float y, std::uint8_t alpha);

// Draws text with Segoe UI / ClearType-style antialiasing.
void DrawTextLine(HDC hdc, std::wstring_view text, const RECT& rect, float font_px, bool semibold,
                  Color4 color, int align_h /* -1 left, 0 center, 1 right */);

// Draws multi-line centered label (up to 2 lines with ellipsis) for desktop grid items.
void DrawGridLabel(HDC hdc, std::wstring_view text, const RECT& rect, float font_px, Color4 color);

// Draws the search input text, selection highlight, inline ghost autocomplete, and caret.
void DrawSearchText(HDC hdc, std::wstring_view typed, std::wstring_view ghost,
                    int sel_start, int sel_end, bool caret_visible, bool focused,
                    const RECT& rect, float font_px, UINT dpi);

// Measures a single line of text width in pixels.
float MeasureTextWidth(HDC hdc, std::wstring_view text, float font_px, bool semibold);

// Extracts a high-resolution icon for an executable or shell item and draws it at (x, y, size, size).
// When has_privacy_dot is true, draws the circular privacy dot directly on the bottom-right corner of the icon.
void DrawItemIcon(HDC hdc, const std::wstring& resolved_path, bool is_app, bool is_folder,
                  int x, int y, int size, bool has_privacy_dot, Color4 privacy_color,
                  bool is_running, UINT dpi);

// Entry in a custom Obsidian Glass popup menu.
struct MenuEntry {
    int id = 0;
    std::wstring label;
    bool enabled = true;
    bool separator = false;
};

// Shows a custom 32-bpp ARGB Obsidian Glass popup menu at screen_pt and returns the chosen item id (or 0 if cancelled).
int ShowGlassMenu(HWND owner, POINT screen_pt, const std::vector<MenuEntry>& items, UINT dpi);

} // namespace pm::pmd::gfx

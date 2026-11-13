#pragma once
#include <windows.h>
#include <cstdint>
#include <d2d1.h>
#include <dwrite.h>
#include <dxgi.h>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <wrl/client.h>

namespace dvr::panel {
using Microsoft::WRL::ComPtr;

// Layout units are DIPs (1/96 inch); the render target scales them to the window's DPI.
struct Rect {
    float left = 0, top = 0, right = 0, bottom = 0;
    float width() const { return right - left; }
    float height() const { return bottom - top; }
    float centerY() const { return (top + bottom) / 2; }
    bool contains(float x, float y) const { return x >= left && x < right && y >= top && y < bottom; }
    Rect inset(float dx, float dy) const { return {left + dx, top + dy, right - dx, bottom - dy}; }
    Rect inset(float d) const { return inset(d, d); }
};

// A tiling fill: cells of the given size in DIPs, each the background with a line on its right and
// bottom edge, anchored at an origin so neighbouring fills line up.
struct Grid {
    float cellWidth, cellHeight;
    uint32_t background, line;
    float originX, originY;
};

enum class Font { Title, Subtitle, Heading, Body, BodyBold, Small, Big, Count };
enum class Align { Leading, Center, Trailing };

class Renderer {
public:
    bool initialize();
    // GPU rendering, falling back to software. The panel redraws at display rate while anything
    // moves, which software rendering at high DPI cannot keep up with; a frame of this UI is a few
    // hundred small draws, negligible beside the game.
    bool attach(HWND window);
    void resize();
    void setDpi(float dpi);
    float dpi() const { return dpi_; }

    bool begin();
    // False when the target was lost; it is recreated on the next begin().
    bool end();
    // Blocks until the next vertical blank of the monitor showing the window, which paces
    // animation at the display's refresh rate. False if it could not wait.
    bool waitForVBlank();

    void clear(uint32_t color);
    void fill(const Rect& rect, uint32_t color, float radius = 0);
    void stroke(const Rect& rect, uint32_t color, float radius = 0, float width = 1);
    void ellipse(float cx, float cy, float rx, float ry, uint32_t color);
    void ellipseOutline(float cx, float cy, float rx, float ry, uint32_t color, float width);
    void line(float x0, float y0, float x1, float y1, uint32_t color, float width);
    // One draw for a whole connected line, far cheaper than a line per segment.
    void polyline(const std::vector<D2D1_POINT_2F>& points, uint32_t color, float width);
    // Single lines are vertically centered and end in an ellipsis; wrapped text starts at the top.
    void text(std::wstring_view content, const Rect& rect, Font font, uint32_t color, Align align = Align::Leading,
              bool wrap = false);
    float measureWidth(std::wstring_view content, Font font);
    float measureHeight(std::wstring_view content, Font font, float width);
    void fill(const Rect& rect, const Grid& grid, float radius = 0);
    void ellipse(float cx, float cy, float rx, float ry, const Grid& grid);
    // Axis-aligned clips only: they cost nothing, where a shaped clip (a layer) is composited on
    // the GPU every frame. Round corners are painted over with maskCorners() instead.
    void pushClip(const Rect& rect);
    void popClip();
    // Paints the area outside a rounded rectangle's corners, inside the rectangle, in the given color.
    void maskCorners(const Rect& rect, float radius, uint32_t color);

private:
    IDWriteTextFormat* format(Font font, Align align, bool wrap);
    // Text layouts are the expensive part of a frame, and almost all text is the same from one
    // frame to the next, so layouts are kept and reused.
    IDWriteTextLayout* layout(std::wstring_view content, Font font, Align align, bool wrap, float width, float height);
    ID2D1SolidColorBrush* brush(uint32_t color);
    ID2D1BitmapBrush* brush(const Grid& grid);

    ComPtr<ID2D1Factory> factory_;
    ComPtr<IDWriteFactory> write_;
    ComPtr<ID2D1HwndRenderTarget> target_;
    ComPtr<ID2D1SolidColorBrush> brush_;
    ComPtr<IDWriteTextFormat> formats_[static_cast<int>(Font::Count)];
    ComPtr<IDWriteInlineObject> ellipsis_[static_cast<int>(Font::Count)];
    std::unordered_map<std::wstring, ComPtr<IDWriteTextLayout>> layouts_;
    std::unordered_map<uint64_t, ComPtr<ID2D1BitmapBrush>> grids_; // by cell size in pixels and colors
    ComPtr<IDXGIFactory1> dxgi_;
    ComPtr<IDXGIOutput> output_;
    HMONITOR outputMonitor_ = nullptr;
    HWND window_ = nullptr;
    float dpi_ = 96;
};
} // namespace dvr::panel

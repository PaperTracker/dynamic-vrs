#include "panel_renderer.hpp"
#include <algorithm>
#include <cmath>

namespace dvr::panel {
namespace {
D2D1_COLOR_F color(uint32_t rgb) {
    return {((rgb >> 16) & 0xFF) / 255.0F, ((rgb >> 8) & 0xFF) / 255.0F, (rgb & 0xFF) / 255.0F, 1};
}
D2D1_RECT_F rect(const Rect& r) { return {r.left, r.top, r.right, r.bottom}; }

bool hasFamily(IDWriteFactory* factory, const wchar_t* name) {
    ComPtr<IDWriteFontCollection> fonts;
    UINT32 index = 0;
    BOOL exists = FALSE;
    return SUCCEEDED(factory->GetSystemFontCollection(&fonts)) &&
           SUCCEEDED(fonts->FindFamilyName(name, &index, &exists)) && exists;
}
} // namespace

bool Renderer::initialize() {
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory_.GetAddressOf())) ||
        FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown**>(write_.GetAddressOf()))))
        return false;
    const wchar_t* text = hasFamily(write_.Get(), L"Segoe UI") ? L"Segoe UI" : L"Arial";
    struct Style {
        const wchar_t* family;
        float size;
        DWRITE_FONT_WEIGHT weight;
    };
    const Style styles[] = {
        {text, 24, DWRITE_FONT_WEIGHT_SEMI_BOLD}, // Title
        {text, 13, DWRITE_FONT_WEIGHT_NORMAL},    // Subtitle
        {text, 14, DWRITE_FONT_WEIGHT_SEMI_BOLD}, // Heading
        {text, 13, DWRITE_FONT_WEIGHT_NORMAL},    // Body
        {text, 13, DWRITE_FONT_WEIGHT_SEMI_BOLD}, // BodyBold
        {text, 12, DWRITE_FONT_WEIGHT_NORMAL},    // Small
        {text, 22, DWRITE_FONT_WEIGHT_SEMI_BOLD}, // Big
    };
    for (int index = 0; index < static_cast<int>(Font::Count); ++index) {
        const auto& style = styles[index];
        if (FAILED(write_->CreateTextFormat(style.family, nullptr, style.weight, DWRITE_FONT_STYLE_NORMAL,
                                            DWRITE_FONT_STRETCH_NORMAL, style.size, L"", &formats_[index])) ||
            FAILED(write_->CreateEllipsisTrimmingSign(formats_[index].Get(), &ellipsis_[index])))
            return false;
    }
    return true;
}

bool Renderer::attach(HWND window) {
    window_ = window;
    dpi_ = static_cast<float>(GetDpiForWindow(window));
    return true;
}

void Renderer::resize() {
    RECT client{};
    GetClientRect(window_, &client);
    if (target_) target_->Resize({static_cast<UINT32>(client.right), static_cast<UINT32>(client.bottom)});
}

void Renderer::setDpi(float dpi) {
    dpi_ = dpi;
    grids_.clear();
    if (target_) target_->SetDpi(dpi, dpi);
}

bool Renderer::begin() {
    if (!target_) {
        RECT client{};
        GetClientRect(window_, &client);
        // Presents immediately: frames are paced by waitForVBlank(), not by blocking in EndDraw.
        const auto window = D2D1::HwndRenderTargetProperties(
            window_, {static_cast<UINT32>(client.right), static_cast<UINT32>(client.bottom)},
            D2D1_PRESENT_OPTIONS_IMMEDIATELY);
        for (const auto type : {D2D1_RENDER_TARGET_TYPE_HARDWARE, D2D1_RENDER_TARGET_TYPE_SOFTWARE}) {
            const auto properties = D2D1::RenderTargetProperties(
                type, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), dpi_, dpi_);
            if (SUCCEEDED(factory_->CreateHwndRenderTarget(properties, window, &target_)) &&
                SUCCEEDED(target_->CreateSolidColorBrush(color(0), &brush_)))
                break;
            target_.Reset();
        }
        if (!target_) return false;
        target_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);
    }
    target_->BeginDraw();
    return true;
}

bool Renderer::end() {
    if (target_->EndDraw() == D2DERR_RECREATE_TARGET) {
        brush_.Reset();
        grids_.clear();
        target_.Reset();
        return false;
    }
    return true;
}

bool Renderer::waitForVBlank() {
    const HMONITOR monitor = MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST);
    if (!output_ || monitor != outputMonitor_) {
        output_.Reset();
        outputMonitor_ = monitor;
        if (!dxgi_ && FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&dxgi_)))) return false;
        ComPtr<IDXGIAdapter1> adapter;
        for (UINT a = 0; !output_ && dxgi_->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; ++a) {
            ComPtr<IDXGIOutput> candidate;
            for (UINT o = 0; adapter->EnumOutputs(o, &candidate) != DXGI_ERROR_NOT_FOUND; ++o) {
                DXGI_OUTPUT_DESC description{};
                if (SUCCEEDED(candidate->GetDesc(&description)) && description.Monitor == monitor) {
                    output_ = candidate;
                    break;
                }
            }
        }
        if (!output_) {
            dxgi_.Reset(); // the adapter list may be stale after a display change; rebuild it next time
            return false;
        }
    }
    if (SUCCEEDED(output_->WaitForVBlank())) return true;
    output_.Reset();
    return false;
}

ID2D1SolidColorBrush* Renderer::brush(uint32_t rgb) {
    brush_->SetColor(color(rgb));
    return brush_.Get();
}

void Renderer::clear(uint32_t rgb) { target_->Clear(color(rgb)); }

void Renderer::fill(const Rect& r, uint32_t rgb, float radius) {
    if (radius > 0)
        target_->FillRoundedRectangle({rect(r), radius, radius}, brush(rgb));
    else
        target_->FillRectangle(rect(r), brush(rgb));
}

void Renderer::stroke(const Rect& r, uint32_t rgb, float radius, float width) {
    // Half-pixel inset keeps one-DIP borders crisp at 100% scale.
    const Rect inner = r.inset(width / 2);
    if (radius > 0)
        target_->DrawRoundedRectangle({rect(inner), radius, radius}, brush(rgb), width);
    else
        target_->DrawRectangle(rect(inner), brush(rgb), width);
}

void Renderer::ellipse(float cx, float cy, float rx, float ry, uint32_t rgb) {
    target_->FillEllipse({{cx, cy}, rx, ry}, brush(rgb));
}

void Renderer::ellipseOutline(float cx, float cy, float rx, float ry, uint32_t rgb, float width) {
    target_->DrawEllipse({{cx, cy}, rx, ry}, brush(rgb), width);
}

void Renderer::line(float x0, float y0, float x1, float y1, uint32_t rgb, float width) {
    target_->DrawLine({x0, y0}, {x1, y1}, brush(rgb), width);
}

void Renderer::polyline(const std::vector<D2D1_POINT_2F>& points, uint32_t rgb, float width) {
    if (points.size() < 2) return;
    ComPtr<ID2D1PathGeometry> path;
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(factory_->CreatePathGeometry(&path)) || FAILED(path->Open(&sink))) return;
    sink->BeginFigure(points.front(), D2D1_FIGURE_BEGIN_HOLLOW);
    sink->AddLines(points.data() + 1, static_cast<UINT32>(points.size() - 1));
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    if (SUCCEEDED(sink->Close())) target_->DrawGeometry(path.Get(), brush(rgb), width);
}

IDWriteTextFormat* Renderer::format(Font font, Align align, bool wrap) {
    const auto index = static_cast<int>(font);
    auto* textFormat = formats_[index].Get();
    textFormat->SetTextAlignment(align == Align::Leading  ? DWRITE_TEXT_ALIGNMENT_LEADING
                                 : align == Align::Center ? DWRITE_TEXT_ALIGNMENT_CENTER
                                                          : DWRITE_TEXT_ALIGNMENT_TRAILING);
    textFormat->SetParagraphAlignment(wrap ? DWRITE_PARAGRAPH_ALIGNMENT_NEAR : DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    textFormat->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
    const DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
    textFormat->SetTrimming(&trimming, ellipsis_[index].Get());
    return textFormat;
}

IDWriteTextLayout* Renderer::layout(std::wstring_view content, Font font, Align align, bool wrap, float width,
                                    float height) {
    std::wstring key(content);
    key += static_cast<wchar_t>(0xFFFF);
    const float parts[] = {static_cast<float>(font), static_cast<float>(align), wrap ? 1.F : 0.F, width, height};
    key.append(reinterpret_cast<const wchar_t*>(parts), sizeof(parts) / sizeof(wchar_t));
    if (const auto found = layouts_.find(key); found != layouts_.end()) return found->second.Get();
    ComPtr<IDWriteTextLayout> created;
    if (FAILED(write_->CreateTextLayout(content.data(), static_cast<UINT32>(content.size()), format(font, align, wrap),
                                        width, height, &created)))
        return nullptr;
    // Changing text (a live number) adds a layout per value; start over before the cache grows large.
    if (layouts_.size() >= 1024) layouts_.clear();
    return layouts_.emplace(std::move(key), std::move(created)).first->second.Get();
}

void Renderer::text(std::wstring_view content, const Rect& r, Font font, uint32_t rgb, Align align, bool wrap) {
    if (content.empty()) return;
    if (auto* textLayout = layout(content, font, align, wrap, r.width(), r.height()))
        target_->DrawTextLayout({r.left, r.top}, textLayout, brush(rgb), D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

float Renderer::measureWidth(std::wstring_view content, Font font) {
    DWRITE_TEXT_METRICS metrics{};
    auto* textLayout = layout(content, font, Align::Leading, false, 10000, 1000);
    return textLayout && SUCCEEDED(textLayout->GetMetrics(&metrics)) ? metrics.widthIncludingTrailingWhitespace : 0;
}

float Renderer::measureHeight(std::wstring_view content, Font font, float width) {
    DWRITE_TEXT_METRICS metrics{};
    auto* textLayout = layout(content, font, Align::Leading, true, width, 10000);
    return textLayout && SUCCEEDED(textLayout->GetMetrics(&metrics)) ? metrics.height : 0;
}

void Renderer::pushClip(const Rect& r) { target_->PushAxisAlignedClip(rect(r), D2D1_ANTIALIAS_MODE_ALIASED); }

void Renderer::popClip() { target_->PopAxisAlignedClip(); }

void Renderer::maskCorners(const Rect& r, float radius, uint32_t rgb) {
    ComPtr<ID2D1PathGeometry> path;
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(factory_->CreatePathGeometry(&path)) || FAILED(path->Open(&sink))) return;
    // Each corner: from the corner point along one edge, then the arc back to the other edge.
    const struct {
        float x, y, dx, dy;
    } corners[] = {
        {r.left, r.top, 1, 1}, {r.right, r.top, -1, 1}, {r.right, r.bottom, -1, -1}, {r.left, r.bottom, 1, -1}};
    for (const auto& corner : corners) {
        const bool clockwise = corner.dx * corner.dy > 0;
        sink->BeginFigure({corner.x, corner.y}, D2D1_FIGURE_BEGIN_FILLED);
        sink->AddLine({corner.x + corner.dx * radius, corner.y});
        sink->AddArc({{corner.x, corner.y + corner.dy * radius},
                      {radius, radius},
                      0,
                      clockwise ? D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE : D2D1_SWEEP_DIRECTION_CLOCKWISE,
                      D2D1_ARC_SIZE_SMALL});
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    }
    if (SUCCEEDED(sink->Close())) target_->FillGeometry(path.Get(), brush(rgb));
}

ID2D1BitmapBrush* Renderer::brush(const Grid& grid) {
    // One cell is drawn once in device pixels and tiled, so the lines stay one crisp pixel wide.
    const float scale = dpi_ / 96;
    const auto width = static_cast<uint32_t>(std::max(2.F, std::round(grid.cellWidth * scale)));
    const auto height = static_cast<uint32_t>(std::max(2.F, std::round(grid.cellHeight * scale)));
    const uint64_t key =
        (uint64_t{width} << 56) ^ (uint64_t{height} << 48) ^ (uint64_t{grid.background} << 24) ^ grid.line;
    auto& cached = grids_[key];
    if (!cached) {
        ComPtr<ID2D1BitmapRenderTarget> cell;
        ComPtr<ID2D1Bitmap> bitmap;
        const float line = std::max(1.F, std::round(scale)) / scale; // whole pixels, in DIPs
        const D2D1_SIZE_F size{static_cast<float>(width) / scale, static_cast<float>(height) / scale};
        if (FAILED(target_->CreateCompatibleRenderTarget(size, {width, height}, &cell))) return nullptr;
        cell->BeginDraw();
        cell->Clear(color(grid.background));
        ComPtr<ID2D1SolidColorBrush> ink;
        cell->CreateSolidColorBrush(color(grid.line), &ink);
        cell->FillRectangle({size.width - line, 0, size.width, size.height}, ink.Get());
        cell->FillRectangle({0, size.height - line, size.width, size.height}, ink.Get());
        if (FAILED(cell->EndDraw()) || FAILED(cell->GetBitmap(&bitmap)) ||
            FAILED(
                target_->CreateBitmapBrush(bitmap.Get(),
                                           D2D1::BitmapBrushProperties(D2D1_EXTEND_MODE_WRAP, D2D1_EXTEND_MODE_WRAP,
                                                                       D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR),
                                           &cached)))
            return nullptr;
    }
    // Snap the origin to a pixel so the tiling does not resample.
    const float x = std::round(grid.originX * scale) / scale, y = std::round(grid.originY * scale) / scale;
    cached->SetTransform(D2D1::Matrix3x2F::Translation(x, y));
    return cached.Get();
}

void Renderer::fill(const Rect& r, const Grid& grid, float radius) {
    auto* pattern = brush(grid);
    if (!pattern) return fill(r, grid.background, radius);
    if (radius > 0)
        target_->FillRoundedRectangle({rect(r), radius, radius}, pattern);
    else
        target_->FillRectangle(rect(r), pattern);
}

void Renderer::ellipse(float cx, float cy, float rx, float ry, const Grid& grid) {
    auto* pattern = brush(grid);
    if (!pattern) return ellipse(cx, cy, rx, ry, grid.background);
    target_->FillEllipse({{cx, cy}, rx, ry}, pattern);
}
} // namespace dvr::panel

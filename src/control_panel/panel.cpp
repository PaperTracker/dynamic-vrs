// Window, message handling and animation.
#include "panel.hpp"
#include "panel_common.hpp"
#include "panel_theme.hpp"

#include <cmath>
#include <cwchar>
#include <dwmapi.h>
#include <windowsx.h>

namespace dvr::panel {
using control::Result;

Panel::Panel(Options options) : options_(std::move(options)) {}

Panel::~Panel() {
    if (watched_) CloseHandle(watched_);
}

bool Panel::create(HINSTANCE instance) {
    strings_.load(options_.language);
    if (!renderer_.initialize()) return false;
    WNDCLASSEXW windowClass{sizeof(windowClass)};
    windowClass.lpfnWndProc = windowProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    windowClass.lpszClassName = WindowClass;
    if (!RegisterClassExW(&windowClass)) return false;
    if (options_.exitWithProcess && options_.processId) watched_ = OpenProcess(SYNCHRONIZE, FALSE, options_.processId);
    window_ = CreateWindowExW(0, WindowClass, strings_.get(L"Title").c_str(), WindowStyle, CW_USEDEFAULT, CW_USEDEFAULT,
                              900, 800, nullptr, nullptr, instance, this);
    if (!window_) return false;
    // Started by the game: show up without taking keyboard focus away from it.
    ShowWindow(window_, options_.exitWithProcess ? SW_SHOWNOACTIVATE : SW_SHOWNORMAL);
    return true;
}

LRESULT CALLBACK Panel::windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCCREATE) {
        auto* panel = static_cast<Panel*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        panel->window_ = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(panel));
    }
    auto* panel = reinterpret_cast<Panel*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    return panel ? panel->handle(message, wParam, lParam) : DefWindowProcW(window, message, wParam, lParam);
}

LRESULT Panel::handle(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE:
        renderer_.attach(window_);
        applyTheme();
        layout();
        fitWindow();
        SetTimer(window_, PollTimer, PollMs, nullptr);
        SetTimer(window_, ScanTimer, ScanMs, nullptr);
        scan();
        poll();
        return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT paintStruct;
        BeginPaint(window_, &paintStruct);
        paint();
        EndPaint(window_, &paintStruct);
        return 0;
    }
    case WM_SIZE:
        renderer_.resize();
        invalidate();
        return 0;
    case WM_DPICHANGED: {
        renderer_.setDpi(HIWORD(wParam));
        layout();
        const auto* suggested = reinterpret_cast<const RECT*>(lParam);
        RECT frame{0, 0, std::lround(layout_.width * scale()), std::lround(layout_.height * scale())};
        AdjustWindowRectExForDpi(&frame, WindowStyle, FALSE, 0, HIWORD(wParam));
        SetWindowPos(window_, nullptr, suggested->left, suggested->top, frame.right - frame.left,
                     frame.bottom - frame.top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_SETTINGCHANGE:
        if (lParam && std::wcscmp(reinterpret_cast<const wchar_t*>(lParam), L"ImmersiveColorSet") == 0) {
            applyTheme();
            invalidate();
        }
        return 0;
    case WM_TIMER:
        if (wParam == PollTimer) poll();
        if (wParam == ScanTimer) scan();
        if (wParam == SendTimer) flush();
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT) {
            const bool hand =
                hover_ != Control::None && enabled(hover_) && !(hover_ == Control::Preset && hoverIndex_ == 3);
            SetCursor(LoadCursorW(nullptr, hand ? IDC_HAND : IDC_ARROW));
            return TRUE;
        }
        break;
    case WM_MOUSEMOVE: mouseMove(GET_X_LPARAM(lParam) / scale(), GET_Y_LPARAM(lParam) / scale()); return 0;
    case WM_MOUSELEAVE:
        tracking_ = false;
        if (hover_ != Control::None && dragging_ == Control::None) {
            hover_ = Control::None;
            hoverIndex_ = -1;
            invalidate();
        }
        return 0;
    case WM_LBUTTONDOWN: mouseDown(GET_X_LPARAM(lParam) / scale(), GET_Y_LPARAM(lParam) / scale()); return 0;
    case WM_LBUTTONUP: mouseUp(); return 0;
    case WM_CAPTURECHANGED:
        if (reinterpret_cast<HWND>(lParam) != window_) {
            dragging_ = pressed_ = Control::None;
            invalidate();
        }
        return 0;
    case WM_MOUSEWHEEL: mouseWheel(mouseX_, mouseY_, GET_WHEEL_DELTA_WPARAM(wParam)); return 0;
    case WM_CLOSE: close(); return 0;
    case WM_DESTROY:
        KillTimer(window_, PollTimer);
        KillTimer(window_, ScanTimer);
        KillTimer(window_, SendTimer);
        PostQuitMessage(0);
        return 0;
    default: break;
    }
    return DefWindowProcW(window_, message, wParam, lParam);
}

void Panel::applyTheme() {
    dark_ = systemDark();
    palette_ = paletteFor(dark_);
    const BOOL dark = dark_;
    constexpr DWORD UseImmersiveDarkMode = 20; // DWMWA_USE_IMMERSIVE_DARK_MODE, Windows 10 20H1 and later
    DwmSetWindowAttribute(window_, UseImmersiveDarkMode, &dark, sizeof(dark));
}

void Panel::invalidate() {
    dirty_ = true;
    InvalidateRect(window_, nullptr, FALSE);
}

bool Panel::animating() const {
    if (!IsWindowVisible(window_) || IsIconic(window_)) return false;
    // A layer publishes every frame while its application renders, so a connected panel polls at
    // display rate; it only redraws when something changed.
    const bool streaming = haveStatus_ && GetTickCount64() - latest_.heartbeatMs < 1000;
    return dirty_ || moving_ || ab_.has_value() || streaming;
}

void Panel::frame() {
    poll();
    if (ab_) dirty_ = true; // the progress meter moves with time
    if (!dirty_ && !moving_) return;
    paint();
    ValidateRect(window_, nullptr);
}

float Panel::ease(uint32_t key, float target, float seconds, float epsilon) {
    const auto [entry, added] = eased_.try_emplace(key, target);
    float& value = entry->second;
    if (added || !std::isfinite(value)) return value = target;
    value += (target - value) * (1 - std::exp(-frameSeconds_ / seconds));
    if (std::fabs(target - value) <= epsilon)
        value = target;
    else
        moving_ = true;
    return value;
}

} // namespace dvr::panel

#pragma once
#include <windows.h>
#include "control_endpoint.hpp"
#include "panel_renderer.hpp"
#include "panel_strings.hpp"
#include "panel_theme.hpp"
#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace dvr::panel {
struct Options {
    uint32_t processId = 0; // preferred application
    bool exitWithProcess = false;
    std::filesystem::path config; // dynamic-vrs.ini for "Save as defaults"; empty disables saving
    std::wstring language = L"en_US";
};

// Everything the panel draws is custom: one window, one Direct2D target. While anything on screen
// moves (live gaze, an easing control, the trend line), the message loop calls frame() once per
// display refresh; otherwise the panel sleeps until a message arrives. Status comes from shared
// memory, so reading it never blocks.
class Panel {
public:
    explicit Panel(Options options);
    ~Panel();
    static constexpr const wchar_t* WindowClass = L"DynamicVRS.ControlPanel";
    bool create(HINSTANCE instance);
    // True while the next display refresh may need a new frame.
    bool animating() const;
    // Reads the latest status and redraws if anything visible changed or is still easing.
    void frame();
    bool waitForVBlank() { return renderer_.waitForVBlank(); }

private:
    enum class Control : uint8_t {
        None,
        Application,
        Enable,
        Mode,
        Preset,
        Ring1x1, // the four ring sliders, in tier order
        Ring2x1,
        Ring2x2,
        Ring4x2,
        Dither,
        Diagnostics,
        Display,
        Uniform,
        Measure,
        Restore,
        Save,
    };
    static Control ringControl(uint32_t ring) {
        return static_cast<Control>(static_cast<uint32_t>(Control::Ring1x1) + ring);
    }
    // The ring a slider edits, or -1 for any other control.
    static int ringOf(Control control) {
        const int ring = static_cast<int>(control) - static_cast<int>(Control::Ring1x1);
        return ring >= 0 && ring < static_cast<int>(control::RingCount) ? ring : -1;
    }
    enum class Tone : uint8_t { Idle, Good, Warn, Bad };
    struct Hit {
        Rect rect;
        Control control;
        int segments; // 0 for a plain control
    };
    struct Layout {
        float width = 0, height = 0;
        Rect title, subtitle, applicationLabel, application;
        Rect viewState;
        Rect settingsCard, enableTitle, enableSwitch, modeLabel, mode, presetLabel, preset;
        Rect regionsCard, regionsTitle;
        std::array<Rect, control::RingCount> ringLabel, ring, ringValue;
        Rect ditherLabel, dither, effective;
        Rect diagnosticsCard, diagnostics, displayLabel, display, uniformLabel, uniform;
        Rect viewCard, viewTitle, view, legendBar;
        std::array<Rect, control::TierCount> legend;
        Rect metricsCard, metricsTitle;
        std::array<Rect, 4> tiles;
        Rect measure, statusDot, message, restore, save, note;
        std::vector<Hit> hits;
    };
    // What the poll compares to decide whether a repaint is needed.
    struct Shown {
        control::Metrics metrics;
        Settings applied, settings;
        uint32_t width = 0, height = 0, connected = 0, tone = 0;
        bool operator==(const Shown&) const = default;
    };
    struct AbRun {
        uint32_t mode = 0;
        size_t phase = 0;
        uint64_t deadline = 0;
        std::vector<float> results[2];
    };

    static LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT handle(UINT message, WPARAM wParam, LPARAM lParam);

    // Layout and drawing.
    void layout();
    void fitWindow();
    void paint();
    // Eases a displayed value toward its target with a time constant in seconds; returns the value to
    // draw this frame. A key seen for the first time starts at its target.
    float ease(uint32_t key, float target, float seconds, float epsilon = .001F);
    void forget(uint32_t key) { eased_.erase(key); }
    void drawCard(const Rect& rect);
    void drawSegments(const Rect& rect, std::initializer_list<std::wstring_view> labels, int selected, Control control,
                      bool active, int inert = -1);
    // position: 0 to 1 along the track.
    void drawSlider(const Rect& track, const Rect& valueRect, Control control, float position,
                    const std::wstring& label, bool active);
    void drawButton(const Rect& rect, const std::wstring& label, Control control, bool accent, bool active);
    void drawChevron(float cx, float cy, bool up, uint32_t color);
    void drawView();
    // Each tier's shading blocks drawn to scale, colored from sharp to coarsest.
    Grid tierGrid(uint32_t tier) const;
    uint32_t tierColor(uint32_t tier) const;
    std::wstring effectiveText() const;
    void drawMetrics();
    void invalidate();
    void applyTheme();
    float scale() const { return renderer_.dpi() / 96; }
    std::pair<Control, int> hitTest(float x, float y) const;
    bool enabled(Control control) const;

    // Input.
    void mouseMove(float x, float y);
    void mouseDown(float x, float y);
    void mouseUp();
    void mouseWheel(float x, float y, int delta);
    void click(Control control, int index);
    void dragSlider(Control control, float x);
    void showApplicationMenu();

    // Control logic.
    void poll();
    void scan();
    void connect(uint32_t processId);
    void sync(const Settings& settings);
    void edit(const Patch& patch);
    void send(const Patch& patch);
    void flush();
    // Status lines from the poll; a notice (a save, an error) stays up for a few seconds instead.
    void status(std::wstring_view key, std::initializer_list<Strings::Argument> arguments = {});
    void notice(std::wstring_view key, std::initializer_list<Strings::Argument> arguments = {});
    // Sets one ring, moving the others just enough to keep them in order.
    void setRing(uint32_t ring, float degrees);
    void startMeasure();
    void startPhase();
    void stepMeasure(const Status& status);
    void finishMeasure(bool cancelled);
    void save();
    void restore();
    void close();

    Options options_;
    Strings strings_;
    Renderer renderer_;
    Palette palette_{};
    bool dark_ = false;
    HWND window_ = nullptr;
    HANDLE watched_ = nullptr;
    Layout layout_;
    float viewHeight_ = 250;

    Endpoint endpoint_;
    std::vector<Application> applications_;
    Status latest_{};
    bool haveStatus_ = false;
    Settings settings_{}; // what the controls show: the applied settings plus local edits in flight
    uint32_t lastMode_ = 2;
    Patch pending_;
    uint64_t waiting_ = 0;
    bool sendQueued_ = false;
    std::optional<AbRun> ab_;
    std::wstring abValue_, abDetail_;
    float abSaving_ = 0; // measured ms saved; colors the result
    // Scene GPU median sampled every GpuSampleMs, the last 30 s, for the trend line.
    std::vector<float> gpuHistory_;
    double lastGpuSample_ = 0;
    // Animation state: eased values, the time of the last frame, and whether anything is still moving.
    std::unordered_map<uint32_t, float> eased_;
    double lastFrame_ = 0;
    float frameSeconds_ = 0;
    bool dirty_ = true, moving_ = false;
    std::wstring message_;
    Tone tone_ = Tone::Idle;
    uint64_t holdUntil_ = 0;
    Shown shown_{};

    Control hover_ = Control::None, pressed_ = Control::None, dragging_ = Control::None;
    int hoverIndex_ = -1, pressedIndex_ = -1;
    float mouseX_ = 0, mouseY_ = 0;
    bool tracking_ = false, diagnosticsOpen_ = false;
};
} // namespace dvr::panel

// DynamicVRSPanel64.exe: desktop control panel for a running application with the layer loaded.
//   --pid <id>            application to control first
//   --exit-with-process   close when --pid exits, and do nothing if a panel is already open (used by the layer)
//   --config <ini>        dynamic-vrs.ini written by "Save as defaults"; default: ..\dynamic-vrs.ini
//   --lang <column>       column of control_panel_strings.tsv; default en_US
#include <windows.h>
#include "common/log.hpp"
#include "panel.hpp"
#include <cwchar>
#include <shellapi.h>

using namespace dvr::panel;

namespace {
Options parseArguments() {
    Options options;
    int count = 0;
    wchar_t** arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    for (int index = 1; arguments && index < count; ++index) {
        const std::wstring_view argument = arguments[index];
        const bool hasValue = index + 1 < count;
        if (argument == L"--pid" && hasValue)
            options.processId = static_cast<uint32_t>(std::wcstoul(arguments[++index], nullptr, 10));
        else if (argument == L"--config" && hasValue)
            options.config = arguments[++index];
        else if (argument == L"--lang" && hasValue)
            options.language = arguments[++index];
        else if (argument == L"--exit-with-process")
            options.exitWithProcess = true;
    }
    LocalFree(arguments);
    if (options.config.empty()) {
        // Installed layout: SteamVR\bin\dynamic-vrs_payload\DynamicVRSPanel64.exe next to SteamVR\bin\dynamic-vrs.ini.
        wchar_t executable[MAX_PATH]{};
        GetModuleFileNameW(nullptr, executable, MAX_PATH);
        const auto candidate = std::filesystem::path(executable).parent_path().parent_path() / L"dynamic-vrs.ini";
        std::error_code ignored;
        if (std::filesystem::is_regular_file(candidate, ignored)) options.config = candidate;
    }
    return options;
}
} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    // The layer starts the panel without a console, so failures go to the shared log folder.
    dvr::setLogComponent(L"control-panel");
    const Options options = parseArguments();
    HANDLE single = CreateMutexW(nullptr, FALSE, dvr::control::PanelMutexName);
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        // Opened by hand while a panel is up: bring that one forward instead.
        if (HWND existing = !options.exitWithProcess ? FindWindowW(Panel::WindowClass, nullptr) : nullptr) {
            ShowWindow(existing, SW_RESTORE);
            SetForegroundWindow(existing);
        }
        if (single) CloseHandle(single);
        return 0;
    }
    int exitCode = 0;
    {
        Panel panel(options);
        if (panel.create(instance)) {
            // Messages first; then, while anything moves, one frame per display refresh.
            MSG message{};
            while (message.message != WM_QUIT) {
                while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                    if (message.message == WM_QUIT) break;
                    TranslateMessage(&message);
                    DispatchMessageW(&message);
                }
                if (message.message == WM_QUIT) break;
                if (!panel.animating()) {
                    WaitMessage();
                    continue;
                }
                panel.frame();
                if (!panel.waitForVBlank()) Sleep(8);
            }
        } else {
            dvr::logMessage("stage=control-panel result=window-create-failed "
                            "reason=\"Direct2D/DirectWrite unavailable?\"");
            exitCode = 1;
        }
    }
    if (single) CloseHandle(single);
    return exitCode;
}

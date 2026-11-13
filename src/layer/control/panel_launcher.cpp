#include "panel_launcher.hpp"
#include <windows.h>
#include "common/ini_file.hpp"
#include "common/vrs_control_protocol.hpp"
#include <string>

namespace dvr::layer {
namespace {
std::wstring quote(const std::wstring& value) { return L"\"" + value + L"\""; }
} // namespace

const char* launchControlPanel(const std::filesystem::path& iniPath, const std::filesystem::path& layerDirectory) {
    if (IniFile(iniPath).integer(L"controlPanelAutoLaunch", 1) != 1) return "disabled";
    if (HANDLE running = OpenMutexW(SYNCHRONIZE, FALSE, dvr::control::PanelMutexName)) {
        CloseHandle(running);
        return "already-running";
    }
    const auto panel = layerDirectory / L"DynamicVRSPanel64.exe";
    std::error_code error;
    if (!std::filesystem::is_regular_file(panel, error)) return "panel-missing";

    std::wstring command = quote(panel.wstring()) + L" --pid " + std::to_wstring(GetCurrentProcessId()) +
                           L" --exit-with-process --config " + quote(iniPath.wstring());
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    const auto directory = layerDirectory.wstring();
    if (!CreateProcessW(panel.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NEW_PROCESS_GROUP, nullptr,
                        directory.c_str(), &startup, &process))
        return "create-process-failed";
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return "launched";
}
} // namespace dvr::layer

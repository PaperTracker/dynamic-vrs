#pragma once
#include <filesystem>

namespace dvr::layer {
// Starts the desktop control panel for this process unless one is already running.
// Reads controlPanelAutoLaunch from [loader] in iniPath; DynamicVRSPanel64.exe is expected
// next to the layer DLL. Returns a short reason for the log.
const char* launchControlPanel(const std::filesystem::path& iniPath, const std::filesystem::path& layerDirectory);
} // namespace dvr::layer

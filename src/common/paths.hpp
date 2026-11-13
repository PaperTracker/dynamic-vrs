#pragma once
#include <windows.h>

#include <filesystem>
#include <string>
#include <string_view>

// Module and folder lookups shared by every binary: the proxy, the layer and the control panel.

namespace dvr {

// The module (DLL or executable) this code is linked into, not the host process.
HMODULE thisModule();

// Full path of a loaded module; nullptr names the host executable. Empty on failure.
std::filesystem::path modulePath(HMODULE module);

// The folder holding this module.
std::filesystem::path thisModuleDirectory();

std::string wideToUtf8(std::wstring_view value);

// %LOCALAPPDATA%\OpenVRDynamicVRS\logs, or a logs folder next to this module when LOCALAPPDATA is unset.
// The folder name predates the project's current name and is kept so existing installs and logs stay put.
std::filesystem::path logDirectory();

} // namespace dvr

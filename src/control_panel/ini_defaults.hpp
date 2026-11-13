#pragma once
#include "common/vrs_control_protocol.hpp"
#include <filesystem>
#include <string>

namespace dvr::panel {
// Writes mode, radii and rates into [loader] of dynamic-vrs.ini, keeping comments, key order,
// BOM and line endings. The first save keeps the original as dynamic-vrs.ini.bak.
bool saveDefaults(const std::filesystem::path& path, const control::Settings& settings, std::wstring& error);
} // namespace dvr::panel

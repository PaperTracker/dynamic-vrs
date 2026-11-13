#pragma once
#include <windows.h>

#include <filesystem>
#include <string>

namespace dvr {

// Read access to the [loader] section of dynamic-vrs.ini, the one section the proxy, the layer and the OpenXR
// loader entry point read. A missing file or key yields the fallback.
class IniFile {
public:
    explicit IniFile(std::filesystem::path path) : path_(std::move(path)) {}

    const std::filesystem::path& path() const { return path_; }

    std::wstring text(const wchar_t* key, const wchar_t* fallback = L"") const;
    // GetPrivateProfileInt semantics: a leading integer, otherwise the fallback.
    int integer(const wchar_t* key, int fallback) const;
    // True when the value starts with 1.
    bool flag(const wchar_t* key) const;
    // A finite number clamped to [minimum, maximum]; the fallback (unclamped) for a missing or malformed value.
    double number(const wchar_t* key, double fallback, double minimum, double maximum) const;

private:
    std::filesystem::path path_;
};

} // namespace dvr

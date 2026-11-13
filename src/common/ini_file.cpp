#include "ini_file.hpp"

#include <algorithm>
#include <cmath>

namespace dvr {

namespace {
constexpr const wchar_t* Section = L"loader";
}

std::wstring IniFile::text(const wchar_t* key, const wchar_t* fallback) const {
    std::wstring value(32768, L'\0');
    const DWORD length =
        GetPrivateProfileStringW(Section, key, fallback, value.data(), static_cast<DWORD>(value.size()), path_.c_str());
    value.resize(length);
    return value;
}

int IniFile::integer(const wchar_t* key, int fallback) const {
    return static_cast<int>(GetPrivateProfileIntW(Section, key, fallback, path_.c_str()));
}

bool IniFile::flag(const wchar_t* key) const {
    const std::wstring value = text(key, L"0");
    return !value.empty() && value[0] == L'1';
}

double IniFile::number(const wchar_t* key, double fallback, double minimum, double maximum) const {
    const std::wstring value = text(key);
    if (value.empty()) {
        return fallback;
    }
    try {
        const double parsed = std::stod(value);
        return std::isnan(parsed) ? fallback : std::clamp(parsed, minimum, maximum);
    } catch (...) {
        return fallback;
    }
}

} // namespace dvr

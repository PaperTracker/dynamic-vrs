#include "ini_defaults.hpp"
#include <windows.h>
#include <array>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string_view>
#include <utility>
#include <vector>

namespace dvr::panel {
namespace {
std::string_view trim(std::string_view text) {
    const auto first = text.find_first_not_of(" \t");
    if (first == std::string_view::npos) return {};
    return text.substr(first, text.find_last_not_of(" \t") - first + 1);
}

std::string lower(std::string_view text) {
    std::string result(text);
    for (char& c : result)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return result;
}

std::wstring lastError(const wchar_t* action) {
    wchar_t* text = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
                   GetLastError(), 0, reinterpret_cast<wchar_t*>(&text), 0, nullptr);
    std::wstring message = std::wstring(action) + L": " + (text ? text : L"unknown error");
    LocalFree(text);
    while (!message.empty() && (message.back() == L'\n' || message.back() == L'\r')) message.pop_back();
    return message;
}

bool writeFile(const std::filesystem::path& path, const std::string& bytes) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    return file.write(bytes.data(), static_cast<std::streamsize>(bytes.size())) && file.flush();
}

std::string number(float value) {
    std::array<char, 32> text{};
    std::snprintf(text.data(), text.size(), "%.2f", value);
    return text.data();
}
} // namespace

bool saveDefaults(const std::filesystem::path& path, const control::Settings& settings, std::wstring& error) {
    static constexpr const char* ModeNames[] = {"off", "fixed", "gaze"};
    // The ring keys take precedence over the three-region keys of older files, which are left as they are.
    const std::pair<std::string_view, std::string> values[] = {
        {"nativeVrsMode", ModeNames[settings.mode <= 2 ? settings.mode : 0]},
        {"vrsRing1x1Degrees", number(settings.ringDegrees[control::Tier1x1])},
        {"vrsRing2x1Degrees", number(settings.ringDegrees[control::Tier2x1])},
        {"vrsRing2x2Degrees", number(settings.ringDegrees[control::Tier2x2])},
        {"vrsRing4x2Degrees", number(settings.ringDegrees[control::Tier4x2])},
        {"vrsRingDitherDegrees", number(settings.ditherDegrees)},
    };

    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = L"Cannot read " + path.wstring();
        return false;
    }
    const std::string raw((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    input.close();
    constexpr std::string_view Bom = "\xEF\xBB\xBF";
    const bool bom = raw.starts_with(Bom);
    const std::string_view text = std::string_view(raw).substr(bom ? Bom.size() : 0);
    const std::string_view newline = text.find("\r\n") != std::string_view::npos ? "\r\n" : "\n";

    std::vector<std::string> lines;
    for (size_t start = 0; start < text.size();) {
        size_t end = text.find('\n', start);
        if (end == std::string_view::npos) end = text.size();
        std::string_view line = text.substr(start, end - start);
        if (line.ends_with('\r')) line.remove_suffix(1);
        lines.emplace_back(line);
        start = end + 1;
    }

    std::string section;
    size_t loaderEnd = std::string::npos;
    bool inLoader = false, seen[std::size(values)] = {};
    for (size_t index = 0; index < lines.size(); ++index) {
        const auto stripped = trim(lines[index]);
        if (stripped.starts_with('[') && stripped.ends_with(']')) {
            if (inLoader && loaderEnd == std::string::npos) loaderEnd = index;
            section = lower(trim(stripped.substr(1, stripped.size() - 2)));
            inLoader = section == "loader";
            continue;
        }
        const auto equals = stripped.find('=');
        if (!inLoader || equals == std::string_view::npos || stripped.starts_with(';')) continue;
        const auto key = trim(stripped.substr(0, equals));
        for (size_t item = 0; item < std::size(values); ++item)
            if (key == values[item].first) {
                lines[index] = std::string(key) + "=" + values[item].second;
                seen[item] = true;
            }
    }
    if (inLoader && loaderEnd == std::string::npos) loaderEnd = lines.size();
    if (loaderEnd == std::string::npos) {
        lines.emplace_back("[loader]");
        loaderEnd = lines.size();
    }
    std::vector<std::string> missing;
    for (size_t item = 0; item < std::size(values); ++item)
        if (!seen[item]) missing.push_back(std::string(values[item].first) + "=" + values[item].second);
    lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(loaderEnd), missing.begin(), missing.end());

    std::string output = bom ? std::string(Bom) : std::string();
    for (const auto& line : lines) output += line + std::string(newline);

    auto backup = path;
    backup += L".bak";
    std::error_code ignored;
    if (!std::filesystem::exists(backup, ignored) && !writeFile(backup, raw)) {
        error = L"Cannot write " + backup.wstring();
        return false;
    }
    auto temporary = path;
    temporary += L".tmp";
    if (!writeFile(temporary, output)) {
        error = L"Cannot write " + temporary.wstring();
        return false;
    }
    if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        error = lastError(L"Cannot replace dynamic-vrs.ini");
        DeleteFileW(temporary.c_str());
        return false;
    }
    return true;
}
} // namespace dvr::panel

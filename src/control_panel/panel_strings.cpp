#include "panel_strings.hpp"
#include <windows.h>
#include "panel_resources.h"
#include <cmath>
#include <cwchar>
#include <iterator>
#include <vector>

namespace dvr::panel {
namespace {
std::wstring resourceText() {
    HRSRC resource = FindResourceW(nullptr, MAKEINTRESOURCEW(IDR_PANEL_STRINGS), RT_RCDATA);
    HGLOBAL loaded = resource ? LoadResource(nullptr, resource) : nullptr;
    const auto* bytes = loaded ? static_cast<const char*>(LockResource(loaded)) : nullptr;
    int size = bytes ? static_cast<int>(SizeofResource(nullptr, resource)) : 0;
    if (size >= 3 && bytes[0] == '\xEF' && bytes[1] == '\xBB' && bytes[2] == '\xBF') {
        bytes += 3;
        size -= 3;
    }
    if (size <= 0) return {};
    std::wstring text(MultiByteToWideChar(CP_UTF8, 0, bytes, size, nullptr, 0), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, bytes, size, text.data(), static_cast<int>(text.size()));
    return text;
}

std::vector<std::wstring_view> split(std::wstring_view text, wchar_t separator) {
    std::vector<std::wstring_view> parts;
    for (size_t start = 0;;) {
        const size_t end = text.find(separator, start);
        parts.push_back(text.substr(start, end == std::wstring_view::npos ? std::wstring_view::npos : end - start));
        if (end == std::wstring_view::npos) return parts;
        start = end + 1;
    }
}
} // namespace

void Strings::load(std::wstring_view language) {
    constexpr std::wstring_view Prefix = L"DynamicVRS/Control/";
    const std::wstring text = resourceText();
    size_t column = 0;
    bool header = true;
    for (auto line : split(text, L'\n')) {
        if (line.ends_with(L'\r')) line.remove_suffix(1);
        const auto parts = split(line, L'\t');
        if (header) {
            header = false;
            for (size_t index = 1; index < parts.size(); ++index)
                if (parts[index] == language || (!column && parts[index] == L"en_US")) column = index;
            continue;
        }
        if (column && parts.size() > column && parts[0].starts_with(Prefix))
            text_[std::wstring(parts[0].substr(Prefix.size()))] = parts[column];
    }
}

const std::wstring& Strings::get(std::wstring_view key) const {
    const auto found = text_.find(std::wstring(key));
    if (found != text_.end()) return found->second;
    missing_ = key;
    return missing_;
}

std::wstring Strings::format(std::wstring_view key, std::initializer_list<Argument> arguments) const {
    const std::wstring_view pattern = get(key);
    std::wstring result;
    for (size_t index = 0; index < pattern.size(); ++index) {
        const size_t close = pattern[index] == L'{' ? pattern.find(L'}', index) : std::wstring_view::npos;
        if (close == std::wstring_view::npos) {
            result += pattern[index];
            continue;
        }
        const auto field = pattern.substr(index + 1, close - index - 1);
        const auto colon = field.find(L':');
        const auto name = field.substr(0, colon);
        int precision = -1;
        if (colon != std::wstring_view::npos && field.size() > colon + 2 && field[colon + 1] == L'.')
            precision = field[colon + 2] - L'0';
        for (const auto& [argument, value] : arguments) {
            if (argument != name) continue;
            if (const auto* text = std::get_if<std::wstring>(&value)) {
                result += *text;
            } else {
                const double number = std::get<double>(value);
                wchar_t buffer[64];
                if (precision < 0) precision = number == std::floor(number) ? 0 : 2;
                std::swprintf(buffer, std::size(buffer), L"%.*f", precision, number);
                result += buffer;
            }
            break;
        }
        index = close;
    }
    return result;
}
} // namespace dvr::panel

#include "paths.hpp"

namespace dvr {

HMODULE thisModule() {
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&thisModule), &module);
    return module;
}

std::filesystem::path modulePath(HMODULE module) {
    std::wstring buffer(32768, L'\0');
    const DWORD length = GetModuleFileNameW(module, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) {
        return {};
    }
    buffer.resize(length);
    return std::filesystem::path(buffer);
}

std::filesystem::path thisModuleDirectory() { return modulePath(thisModule()).parent_path(); }

std::string wideToUtf8(std::wstring_view value) {
    if (value.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
                                         nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return "<utf8-conversion-failed>";
    }
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(),
                        size, nullptr, nullptr);
    return result;
}

std::filesystem::path logDirectory() {
    std::wstring localAppData(32768, L'\0');
    const DWORD length =
        GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData.data(), static_cast<DWORD>(localAppData.size()));
    if (length > 0 && length < localAppData.size()) {
        localAppData.resize(length);
        return std::filesystem::path(localAppData) / L"OpenVRDynamicVRS" / L"logs";
    }
    return thisModuleDirectory() / L"logs";
}

} // namespace dvr

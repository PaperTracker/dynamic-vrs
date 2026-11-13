#include "log.hpp"

#include "paths.hpp"

#include <cstdarg>
#include <cstdio>
#include <fstream>
#include <mutex>

namespace dvr {
namespace {

const wchar_t* g_component = L"dvr";
std::once_flag g_initOnce;
std::mutex g_mutex;
std::ofstream g_stream;

void openLog() {
    try {
        std::call_once(g_initOnce, [] {
            const auto directory = logDirectory();
            std::error_code error;
            std::filesystem::create_directories(directory, error);
            const auto processName = modulePath(nullptr).stem().wstring();
            const auto path = directory / (std::wstring(g_component) + L"-" + processName + L"-" +
                                           std::to_wstring(GetCurrentProcessId()) + L".log");
            g_stream.open(path, std::ios::out | std::ios::app);
        });
    } catch (...) {
        // Diagnostics must never affect the host application.
    }
}

} // namespace

void setLogComponent(const wchar_t* component) { g_component = component; }

void logMessage(const char* format, ...) {
    openLog();

    char message[4096]{};
    va_list args;
    va_start(args, format);
    vsnprintf_s(message, sizeof(message), _TRUNCATE, format, args);
    va_end(args);

    SYSTEMTIME time{};
    GetLocalTime(&time);
    char line[4608]{};
    snprintf(line, sizeof(line), "%04u-%02u-%02u %02u:%02u:%02u.%03u pid=%lu %s\n", time.wYear, time.wMonth, time.wDay,
             time.wHour, time.wMinute, time.wSecond, time.wMilliseconds, GetCurrentProcessId(), message);

    OutputDebugStringA(line);
    try {
        std::scoped_lock lock(g_mutex);
        if (g_stream.is_open()) {
            g_stream << line;
            g_stream.flush();
        }
    } catch (...) {
        // Fail open: forwarding the host's calls matters more than logging them.
    }
}

} // namespace dvr

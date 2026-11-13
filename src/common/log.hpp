#pragma once

namespace dvr {

// Names this module's log file, <component>-<process>-<pid>.log in logDirectory(). Call it before the first
// message; the string must outlive the module (use a literal).
void setLogComponent(const wchar_t* component);

// Appends one timestamped line to the log file and to OutputDebugString.
// Never throws: diagnostics must not affect the host application or SteamVR.
void logMessage(const char* format, ...);

} // namespace dvr

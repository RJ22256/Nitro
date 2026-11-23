#pragma once

#include <string>

namespace lsfgi {

void LogInit(const std::wstring& path);
void Log(const char* fmt, ...);
// Writes one line without waiting for the log lock (for the crash handler).
void LogNoWait(const char* text);
std::string Narrow(const std::wstring& s);

} // namespace lsfgi

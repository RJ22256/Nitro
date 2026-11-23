#pragma once

#include <string>

namespace lsfgi {

void LogInit(const std::wstring& path);
void Log(const char* fmt, ...);
std::string Narrow(const std::wstring& s);

} // namespace lsfgi

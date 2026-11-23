#include "log.h"

#include <windows.h>

#include <share.h>

#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace lsfgi {

namespace {
std::mutex g_logMutex;
FILE* g_log = nullptr;
} // namespace

void LogInit(const std::wstring& path) {
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (g_log) return;
    // Shared, so the log can be read while the game is still running.
    g_log = _wfsopen(path.c_str(), L"w", _SH_DENYNO);
}

void Log(const char* fmt, ...) {
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (!g_log) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    std::fprintf(g_log, "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list args;
    va_start(args, fmt);
    std::vfprintf(g_log, fmt, args);
    va_end(args);
    std::fputc('\n', g_log);
    std::fflush(g_log);
}

std::string Narrow(const std::wstring& s) {
    if (s.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                                      nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), n,
                        nullptr, nullptr);
    return out;
}

} // namespace lsfgi

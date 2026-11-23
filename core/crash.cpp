// Writes a short crash report to the log when the game hits a fatal CPU
// exception: what happened, where (module + offset, with Nitro function names
// when NitroHook.pdb is available), and the call stack. Nothing is changed:
// the game's own handlers still run afterwards.
#include "crash.h"

#include <windows.h>

#include <dbghelp.h>

#include <atomic>
#include <cstdio>
#include <string>

#include "log.h"

namespace lsfgi {

namespace {

std::atomic<int> g_reports{0};
std::wstring g_symbolPath;
bool g_symbolsReady = false;

using SymInitializeWFn = BOOL(WINAPI*)(HANDLE, PCWSTR, BOOL);
using SymSetOptionsFn = DWORD(WINAPI*)(DWORD);
using SymFromAddrFn = BOOL(WINAPI*)(HANDLE, DWORD64, PDWORD64, PSYMBOL_INFO);
using SymGetLineFromAddr64Fn = BOOL(WINAPI*)(HANDLE, DWORD64, PDWORD, PIMAGEHLP_LINE64);
SymFromAddrFn g_symFromAddr = nullptr;
SymGetLineFromAddr64Fn g_symGetLine = nullptr;

void InitSymbols() {
    if (g_symbolsReady) return;
    g_symbolsReady = true;
    HMODULE dbghelp = LoadLibraryExW(L"dbghelp.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!dbghelp) return;
    auto init = reinterpret_cast<SymInitializeWFn>(GetProcAddress(dbghelp, "SymInitializeW"));
    auto opts = reinterpret_cast<SymSetOptionsFn>(GetProcAddress(dbghelp, "SymSetOptions"));
    g_symFromAddr = reinterpret_cast<SymFromAddrFn>(GetProcAddress(dbghelp, "SymFromAddr"));
    g_symGetLine = reinterpret_cast<SymGetLineFromAddr64Fn>(GetProcAddress(dbghelp, "SymGetLineFromAddr64"));
    if (!init || !opts || !g_symFromAddr) {
        g_symFromAddr = nullptr;
        return;
    }
    opts(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES | SYMOPT_FAIL_CRITICAL_ERRORS |
         SYMOPT_NO_PROMPTS);
    if (!init(GetCurrentProcess(), g_symbolPath.empty() ? nullptr : g_symbolPath.c_str(), TRUE)) g_symFromAddr = nullptr;
}

const char* ExceptionName(DWORD code) {
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION: return "access violation";
    case EXCEPTION_ILLEGAL_INSTRUCTION: return "illegal instruction";
    case EXCEPTION_PRIV_INSTRUCTION: return "privileged instruction";
    case EXCEPTION_STACK_OVERFLOW: return "stack overflow";
    case EXCEPTION_INT_DIVIDE_BY_ZERO: return "divide by zero";
    case EXCEPTION_IN_PAGE_ERROR: return "in-page error";
    default: return nullptr;
    }
}

// "module+0x1234 (function + 0x12, file:line)"
void Describe(void* addr, char* out, size_t size) {
    HMODULE mod = nullptr;
    char name[MAX_PATH] = "?";
    uintptr_t offset = reinterpret_cast<uintptr_t>(addr);
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCWSTR>(addr), &mod) &&
        mod) {
        char path[MAX_PATH] = {};
        GetModuleFileNameA(mod, path, MAX_PATH);
        const char* slash = strrchr(path, '\\');
        snprintf(name, sizeof(name), "%s", slash ? slash + 1 : path);
        offset -= reinterpret_cast<uintptr_t>(mod);
    }
    int n = snprintf(out, size, "%s+0x%llx", name, static_cast<unsigned long long>(offset));
    if (g_symFromAddr && n > 0 && static_cast<size_t>(n) < size) {
        alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 256] = {};
        auto* sym = reinterpret_cast<SYMBOL_INFO*>(buf);
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 255;
        DWORD64 disp = 0;
        if (g_symFromAddr(GetCurrentProcess(), reinterpret_cast<DWORD64>(addr), &disp, sym)) {
            IMAGEHLP_LINE64 line{};
            line.SizeOfStruct = sizeof(line);
            DWORD ldisp = 0;
            if (g_symGetLine && g_symGetLine(GetCurrentProcess(), reinterpret_cast<DWORD64>(addr), &ldisp, &line)) {
                const char* file = strrchr(line.FileName, '\\');
                snprintf(out + n, size - n, " (%s + 0x%llx, %s:%lu)", sym->Name, static_cast<unsigned long long>(disp),
                         file ? file + 1 : line.FileName, line.LineNumber);
            } else {
                snprintf(out + n, size - n, " (%s + 0x%llx)", sym->Name, static_cast<unsigned long long>(disp));
            }
        }
    }
}

LONG CALLBACK OnException(EXCEPTION_POINTERS* ep) {
    const DWORD code = ep->ExceptionRecord->ExceptionCode;
    const char* what = ExceptionName(code);
    if (!what) return EXCEPTION_CONTINUE_SEARCH;
    // Games and drivers sometimes trigger and handle these on purpose, so
    // report each address once, and at most 10 in total.
    static void* seen[10] = {};
    void* addr = ep->ExceptionRecord->ExceptionAddress;
    const int n = g_reports.load();
    for (int i = 0; i < n && i < 10; ++i)
        if (seen[i] == addr) return EXCEPTION_CONTINUE_SEARCH;
    const int slot = g_reports.fetch_add(1);
    if (slot >= 10) return EXCEPTION_CONTINUE_SEARCH;
    seen[slot] = addr;

    InitSymbols();
    char line[1024];
    char where[600];
    Describe(ep->ExceptionRecord->ExceptionAddress, where, sizeof(where));
    if (code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2) {
        const ULONG_PTR kind = ep->ExceptionRecord->ExceptionInformation[0];
        snprintf(line, sizeof(line), "CRASH? %s %s address %p at %s (thread %lu)", what,
                 kind == 0 ? "reading" : kind == 1 ? "writing" : "executing",
                 reinterpret_cast<void*>(ep->ExceptionRecord->ExceptionInformation[1]), where, GetCurrentThreadId());
    } else {
        snprintf(line, sizeof(line), "CRASH? %s at %s (thread %lu)", what, where, GetCurrentThreadId());
    }
    LogNoWait(line);

    if (code != EXCEPTION_STACK_OVERFLOW) {
        void* frames[40];
        const USHORT count = RtlCaptureStackBackTrace(0, 40, frames, nullptr);
        bool past = false; // skip our handler and the exception dispatcher
        int written = 0;
        for (USHORT i = 0; i < count && written < 24; ++i) {
            if (!past) {
                HMODULE m = nullptr;
                GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   static_cast<LPCWSTR>(frames[i]), &m);
                if (m != GetModuleHandleW(L"ntdll.dll")) continue;
                past = true; // the first ntdll frame is the dispatcher; the faulting code follows it
                continue;
            }
            Describe(frames[i], where, sizeof(where));
            snprintf(line, sizeof(line), "    at %s", where);
            LogNoWait(line);
            ++written;
        }
    }
    LogNoWait("(If the game kept running, this was handled and can be ignored.)");
    return EXCEPTION_CONTINUE_SEARCH;
}

} // namespace

void InstallCrashLogger(const std::wstring& symbolPath) {
    g_symbolPath = symbolPath;
    AddVectoredExceptionHandler(0, &OnException); // last in line, after the game's own handlers
}

} // namespace lsfgi

#include "inject.h"

#include <psapi.h>

#include <algorithm>
#include <cstdint>

namespace lsfgi {

namespace {

HMODULE FindRemoteModule(HANDLE process, const std::wstring& path) {
    HMODULE mods[1024];
    DWORD needed = 0;
    if (!EnumProcessModulesEx(process, mods, sizeof(mods), &needed, LIST_MODULES_DEFAULT)) return nullptr;
    const DWORD count = (std::min)(static_cast<DWORD>(needed / sizeof(HMODULE)), static_cast<DWORD>(1024));
    for (DWORD i = 0; i < count; ++i) {
        wchar_t name[MAX_PATH];
        if (GetModuleFileNameExW(process, mods[i], name, MAX_PATH) && _wcsicmp(name, path.c_str()) == 0)
            return mods[i];
    }
    return nullptr;
}

bool RunRemote(HANDLE process, LPTHREAD_START_ROUTINE fn, void* arg, DWORD& exitCode) {
    HANDLE t = CreateRemoteThread(process, nullptr, 0, fn, arg, 0, nullptr);
    if (!t) return false;
    const DWORD wait = WaitForSingleObject(t, 30000);
    GetExitCodeThread(t, &exitCode);
    CloseHandle(t);
    return wait == WAIT_OBJECT_0;
}

std::wstring Parent(const std::wstring& p) {
    const size_t s = p.find_last_of(L"\\/");
    return s == std::wstring::npos ? L"." : p.substr(0, s);
}

} // namespace

LaunchResult LaunchInjected(const std::wstring& exe, const std::wstring& args, const std::wstring& dllPath) {
    LaunchResult result;
    wchar_t full[MAX_PATH];
    GetFullPathNameW(dllPath.c_str(), MAX_PATH, full, nullptr);
    const std::wstring dll = full;

    // Offset of Nitro_InitHooks inside the hook DLL (the same in every process).
    HMODULE local = LoadLibraryExW(dll.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
    const auto* initProc = local ? reinterpret_cast<const uint8_t*>(GetProcAddress(local, "Nitro_InitHooks")) : nullptr;
    const size_t initRva = initProc ? static_cast<size_t>(initProc - reinterpret_cast<const uint8_t*>(local)) : 0;
    if (local) FreeLibrary(local);

    std::wstring cmd = L"\"" + exe + L"\"";
    if (!args.empty()) cmd += L" " + args;
    const std::wstring workDir = Parent(exe);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED, nullptr, workDir.c_str(),
                        &si, &pi)) {
        result.error = GetLastError();
        return result;
    }
    result.started = true;

    const size_t bytes = (dll.size() + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(pi.hProcess, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (initRva && remote && WriteProcessMemory(pi.hProcess, remote, dll.c_str(), bytes, nullptr)) {
        auto loadLibrary =
            reinterpret_cast<LPTHREAD_START_ROUTINE>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"));
        DWORD code = 0;
        if (RunRemote(pi.hProcess, loadLibrary, remote, code) && code != 0) {
            if (HMODULE mod = FindRemoteModule(pi.hProcess, dll)) {
                auto init = reinterpret_cast<LPTHREAD_START_ROUTINE>(reinterpret_cast<uint8_t*>(mod) + initRva);
                result.injected = RunRemote(pi.hProcess, init, nullptr, code);
            }
        }
    }
    if (remote) VirtualFreeEx(pi.hProcess, remote, 0, MEM_RELEASE);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return result;
}

} // namespace lsfgi

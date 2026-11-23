// Entry point. The same DLL works two ways:
//  * copied into a game folder as dxgi.dll, d3d9.dll or version.dll
//    ("install" mode): it forwards that DLL's exports to the real System32
//    copy and hooks the game's rendering;
//  * injected into a game process by the launcher ("launch" mode).

#include <windows.h>

#include <MinHook.h>

#include <cwchar>
#include <mutex>
#include <string>

#include "../core/bridge.h"
#include "../core/log.h"
#include "../core/runtime.h"
#include "hooks.h"

struct ProxyEntry {
    const wchar_t* dll;
    const char* name;
};
#include "proxy_table.inc"

// Jump table used by the export thunks in proxy_thunks.asm.
extern "C" void* g_proxyProcs[];

// Lets the launcher recognise its own DLL when installing/removing.
extern "C" const char Nitro_Marker[] = "NITRO_PROXY_MARKER_v1";

namespace {

HRESULT WINAPI NotImplemented() {
    return E_NOTIMPL;
}

std::wstring OwnFileName(HMODULE module) {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(module, path, MAX_PATH);
    const wchar_t* slash = wcsrchr(path, L'\\');
    return slash ? slash + 1 : path;
}

bool ApiIs(const wchar_t* name) {
    return _wcsicmp(lsfgi::G().cfg.api.c_str(), name) == 0;
}

void DoInit() {
    using namespace lsfgi;
    if (!LoadRuntime(G().self, L"Nitro.log")) return;
    if (!ClaimProcess()) return;
    if (MH_Initialize() != MH_OK) Log("MinHook failed to initialise");

    const bool all = ApiIs(L"all");
    const bool autoMode = ApiIs(L"auto");
    if (all || autoMode || ApiIs(L"dxgi")) InstallDxgiHooks();
    if (all || ApiIs(L"d3d9") || (autoMode && GetModuleHandleW(L"d3d9.dll"))) InstallD3D9Hooks();
    if (all || ApiIs(L"opengl") || (autoMode && GetModuleHandleW(L"opengl32.dll"))) InstallGLHooks();
}

} // namespace

DWORD WINAPI lsfgi::InitHooksThread(LPVOID) {
    static std::once_flag once;
    std::call_once(once, DoInit);
    return 0;
}

// Called by the launcher through CreateRemoteThread so hooks are in place
// before the game's main thread is resumed.
extern "C" DWORD WINAPI Nitro_InitHooks(LPVOID param) {
    return lsfgi::InitHooksThread(param);
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(module);
    lsfgi::G().self = module;

    // If we are standing in for a system DLL, forward its exports.
    const std::wstring own = OwnFileName(module);
    HMODULE real = nullptr;
    for (size_t i = 0; i < kProxyCount; ++i) {
        void* proc = nullptr;
        if (_wcsicmp(kProxyEntries[i].dll, own.c_str()) == 0) {
            if (!real) real = lsfgi::SystemModule(kProxyEntries[i].dll);
            if (real) proc = reinterpret_cast<void*>(GetProcAddress(real, kProxyEntries[i].name));
        }
        g_proxyProcs[i] = proc ? proc : reinterpret_cast<void*>(&NotImplemented);
    }

    // Hook installation creates devices, which must not happen under the
    // loader lock, so do it on a separate thread.
    if (HANDLE t = CreateThread(nullptr, 0, lsfgi::InitHooksThread, nullptr, 0, nullptr))
        CloseHandle(t);
    return TRUE;
}

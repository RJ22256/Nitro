// Entry point. The same DLL works two ways:
//  * copied into a game folder as dxgi.dll ("install" mode): it forwards every
//    dxgi export to the real System32\dxgi.dll and hooks swap chains;
//  * injected into a game process by the launcher ("launch" mode).

#include <windows.h>

#include <cwchar>

#include "hooks.h"

// Jump table used by the export thunks in proxy_thunks.asm.
extern "C" void* g_dxgiProcs[];

// Lets the launcher recognise its own dxgi.dll when installing/removing.
extern "C" const char Nitro_Marker[] = "NITRO_PROXY_MARKER_v1";

namespace {

// Order must match the thunk indices in proxy_thunks.asm.
const char* const kDxgiExports[] = {
    "ApplyCompatResolutionQuirking",   // 0
    "CompatString",                    // 1
    "CompatValue",                     // 2
    "CreateDXGIFactory",               // 3
    "CreateDXGIFactory1",              // 4
    "CreateDXGIFactory2",              // 5
    "DXGID3D10CreateDevice",           // 6
    "DXGID3D10CreateLayeredDevice",    // 7
    "DXGID3D10GetLayeredDeviceSize",   // 8
    "DXGID3D10RegisterLayers",         // 9
    "DXGIDeclareAdapterRemovalSupport",// 10
    "DXGIDisableVBlankVirtualization", // 11
    "DXGIDumpJournal",                 // 12
    "DXGIGetDebugInterface1",          // 13
    "DXGIReportAdapterConfiguration",  // 14
    "PIXBeginCapture",                 // 15
    "PIXEndCapture",                   // 16
    "PIXGetCaptureState",              // 17
    "SetAppCompatStringPointer",       // 18
    "UpdateHMDEmulationStatus",        // 19
    "DXGIRevertToSxS",                 // 20
};

HMODULE g_realDxgi = nullptr;

HRESULT WINAPI NotImplemented() {
    return E_NOTIMPL;
}

} // namespace

// Called by the launcher through CreateRemoteThread so hooks are in place
// before the game's main thread is resumed.
extern "C" DWORD WINAPI Nitro_InitHooks(LPVOID param) {
    return lsfgi::InitHooksThread(param);
}

void* lsfgi::RealDxgiProc(const char* name) {
    return g_realDxgi ? reinterpret_cast<void*>(GetProcAddress(g_realDxgi, name)) : nullptr;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(module);
    lsfgi::G().self = module;

    wchar_t path[MAX_PATH] = {};
    const UINT n = GetSystemDirectoryW(path, MAX_PATH);
    if (n > 0 && n < MAX_PATH - 12) {
        wcscat_s(path, L"\\dxgi.dll");
        g_realDxgi = LoadLibraryW(path);
    }
    for (size_t i = 0; i < sizeof(kDxgiExports) / sizeof(kDxgiExports[0]); ++i) {
        FARPROC p = g_realDxgi ? GetProcAddress(g_realDxgi, kDxgiExports[i]) : nullptr;
        g_dxgiProcs[i] = p ? reinterpret_cast<void*>(p) : reinterpret_cast<void*>(&NotImplemented);
    }

    // Hook installation creates D3D devices, which must not happen under the
    // loader lock, so do it on a separate thread.
    if (HANDLE t = CreateThread(nullptr, 0, lsfgi::InitHooksThread, nullptr, 0, nullptr))
        CloseHandle(t);
    return TRUE;
}

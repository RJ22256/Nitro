#include "runtime.h"

#include <cwchar>

#include "log.h"

namespace lsfgi {

Globals& G() {
    static Globals g;
    return g;
}

bool LoadRuntime(HMODULE self, const wchar_t* logName) {
    auto& g = G();
    g.self = self;
    g.dir = ModuleDirectory(self);
    LogInit(g.dir + L"\\" + logName);

    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    Log("Nitro loaded into %s", Narrow(exe).c_str());

    LoadConfig(g.dir + L"\\" + kConfigFileName, g.cfg);
    if (g.cfg.losslessDll.empty()) g.cfg.losslessDll = g.dir + L"\\Lossless.dll";
    g.multiplier = g.cfg.multiplier;
    g.fgEnabled = g.cfg.enabled;

    int offset = -1;
    std::wstring err;
    if (!LoadLsfgShaders(g.cfg.losslessDll, g.cfg.shaderOffset, g.shaders, offset, err)) {
        Log("Shader load failed: %s", Narrow(err).c_str());
        Log("Frame generation disabled - the game will run normally");
        return false;
    }
    Log("Loaded %zu shaders from %s (resource offset %d)", g.shaders.size(),
        Narrow(g.cfg.losslessDll).c_str(), offset);
    if (g.cfg.performanceMode && g.shaders.find("p_alpha[0]") == g.shaders.end())
        Log("Performance-mode shaders not found; using quality mode");
    g.ready = true;
    return true;
}

bool PollHotkeys() {
    static bool prevToggle = false, prevCycle = false;
    auto& g = G();
    bool changed = false;

    const bool toggle = g.cfg.toggleKey && (GetAsyncKeyState(g.cfg.toggleKey) & 0x8000);
    if (toggle && !prevToggle) {
        const bool on = !g.fgEnabled.load();
        g.fgEnabled = on;
        Log("Frame generation %s", on ? "ON" : "OFF");
    }
    prevToggle = toggle;

    const bool cycle = g.cfg.cycleKey && (GetAsyncKeyState(g.cfg.cycleKey) & 0x8000);
    if (cycle && !prevCycle) {
        int m = g.multiplier.load() + 1;
        if (m > 4) m = 2;
        g.multiplier = m;
        changed = true;
        Log("Multiplier set to %dx", m);
    }
    prevCycle = cycle;
    return changed;
}

bool ClaimProcess() {
    wchar_t name[64];
    swprintf(name, 64, L"Local\\Nitro_%lu", GetCurrentProcessId());
    HANDLE mutex = CreateMutexW(nullptr, FALSE, name); // intentionally kept open
    if (!mutex) return true;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(mutex);
        Log("Another Nitro module already handles this process - staying idle");
        return false;
    }
    return true;
}

bool& InHook() {
    thread_local bool inHook = false;
    return inHook;
}

} // namespace lsfgi

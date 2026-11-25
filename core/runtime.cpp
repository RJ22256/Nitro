#include "runtime.h"

#include <cwchar>
#include <cwctype>

#include "crash.h"
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
    LoadConfig(g.dir + L"\\" + kConfigFileName, g.cfg);

    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const wchar_t* slash = wcsrchr(exe, L'\\');
    const std::wstring exeName = slash ? slash + 1 : exe;
    std::wstring lower = exeName;
    for (auto& c : lower) c = static_cast<wchar_t>(towlower(c));

    // Games often start helper programs from their own folder (crash
    // reporters, video encoders, launchers), and those load the installed
    // dxgi.dll too. Leave obvious helpers alone, and give any process that
    // isn't the game its own log so it can't overwrite the game's.
    const bool isGame = g.cfg.gameExe.empty() || _wcsicmp(g.cfg.gameExe.c_str(), exeName.c_str()) == 0;
    if (!isGame) {
        for (const wchar_t* helper : {L"crs-", L"crash", L"report", L"unins", L"setup", L"redist", L"cefprocess",
                                      L"webhelper", L"easyanticheat", L"battleye"})
            if (lower.find(helper) != std::wstring::npos) return false;
    }
    std::wstring log = g.dir + L"\\" + logName;
    if (!isGame) {
        const size_t dot = log.rfind(L'.');
        const std::wstring stem = exeName.substr(0, exeName.rfind(L'.'));
        log = log.substr(0, dot) + L"-" + stem + log.substr(dot);
    }
    LogInit(log);
    Log("Nitro loaded into %s", Narrow(exe).c_str());
    {
        // Crash reports name Nitro's functions if NitroHook.pdb is in the
        // Nitro folder (where Nitro.shaders is) or next to this DLL.
        std::wstring symbols = g.dir;
        const size_t slash = g.cfg.shaders.find_last_of(L"\\/");
        if (slash != std::wstring::npos) symbols += L";" + g.cfg.shaders.substr(0, slash);
        InstallCrashLogger(symbols);
    }
    if (g.cfg.losslessDll.empty()) g.cfg.losslessDll = g.dir + L"\\Lossless.dll";
    // Prefer the imported shader pack; fall back to Lossless.dll.
    std::wstring source = g.cfg.losslessDll;
    if (!g.cfg.shaders.empty() && GetFileAttributesW(g.cfg.shaders.c_str()) != INVALID_FILE_ATTRIBUTES)
        source = g.cfg.shaders;
    g.multiplier = g.cfg.multiplier;
    g.fgEnabled = g.cfg.enabled;

    int offset = -1;
    std::wstring err;
    if (!LoadLsfgShaders(source, g.cfg.shaderOffset, g.shaders, offset, err)) {
        Log("Shader load failed: %s", Narrow(err).c_str());
        Log("Frame generation disabled - the game will run normally");
        return false;
    }
    Log("Loaded %zu shaders from %s", g.shaders.size(), Narrow(source).c_str());
    (void)offset;
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
        BumpSettings();
        changed = true;
        Log("Multiplier set to %dx", m);
    }
    prevCycle = cycle;

    static bool prevMenu = false;
    const bool menu = g.cfg.menuKey && (GetAsyncKeyState(g.cfg.menuKey) & 0x8000);
    if (menu && !prevMenu) g.menuOpen = !g.menuOpen.load();
    prevMenu = menu;
    return changed;
}

void BumpSettings() {
    G().settingsVersion.fetch_add(1);
}

void NoteFrame(int generated, const char* api, UINT width, UINT height) {
    auto& g = G();
    g.apiName = api;
    g.width = width;
    g.height = height;
    static LARGE_INTEGER freq{}, start{};
    static int real = 0, gen = 0;
    if (!freq.QuadPart) {
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&start);
    }
    ++real;
    gen += generated;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const double secs = static_cast<double>(now.QuadPart - start.QuadPart) / static_cast<double>(freq.QuadPart);
    if (secs >= 0.5) {
        g.realFps = static_cast<float>(real / secs);
        g.outputFps = static_cast<float>((real + gen) / secs);
        real = gen = 0;
        start = now;
    }
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

namespace {

double Now() {
    static LARGE_INTEGER freq{};
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return static_cast<double>(t.QuadPart) / static_cast<double>(freq.QuadPart);
}

// Refresh rate of the monitor showing the window (re-read once a second).
float RefreshRate(HWND window) {
    static double lastCheck = -10.0;
    static float hz = 60.0f;
    const double now = Now();
    if (now - lastCheck < 1.0) return hz;
    lastCheck = now;
    HMONITOR mon = MonitorFromWindow(window ? window : GetForegroundWindow(), MONITOR_DEFAULTTOPRIMARY);
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    if (mon && GetMonitorInfoW(mon, &mi) && EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) &&
        dm.dmDisplayFrequency > 1)
        hz = static_cast<float>(dm.dmDisplayFrequency);
    return hz;
}

} // namespace

FramePlan PlanFrame(HWND window) {
    auto& g = G();
    const int maxMult = g.multiplier.load();
    FramePlan plan;
    const float hz = RefreshRate(window);
    g.refreshHz = hz;
    if (!g.cfg.dynamic) {
        plan.gens = maxMult - 1;
        plan.denom = maxMult;
        return plan;
    }

    // Dynamic: how many refreshes did this real frame span? Smooth the frame
    // time a little, aim just under the refresh rate (so we never hold the
    // game back), and carry the fraction over so e.g. 2.4x alternates 2x/3x.
    static double last = 0.0, smooth = 0.0, carry = 0.0;
    const double now = Now();
    double dt = last > 0.0 ? now - last : 0.0;
    last = now;
    if (dt <= 0.0 || dt > 0.25) { // first frame, or a hitch/loading screen
        smooth = 0.0;
        carry = 0.0;
        plan.gens = 0;
        plan.denom = 2;
        return plan;
    }
    smooth = smooth > 0.0 ? smooth * 0.85 + dt * 0.15 : dt;
    const double refreshes = smooth * hz * 0.98 + carry;
    int ticks = static_cast<int>(refreshes);
    carry = refreshes - ticks;
    if (ticks > maxMult) {
        ticks = maxMult;
        carry = 0.0;
    }
    if (ticks < 1) ticks = 1;
    plan.gens = ticks - 1;
    plan.denom = ticks < 2 ? 2 : ticks;
    return plan;
}

float FrameRateCap() {
    auto& g = G();
    if (g.cfg.fpsLimitMode == 2) return static_cast<float>(g.cfg.fpsLimit);
    if (g.cfg.fpsLimitMode == 1) {
        // Auto: real frames at refresh / multiplier, so every refresh gets a
        // new frame (real or generated) at an even pace.
        const float hz = g.refreshHz > 0.0f ? g.refreshHz : RefreshRate(nullptr);
        const int mult = g.fgEnabled ? g.multiplier.load() : 1;
        return hz / static_cast<float>(mult < 1 ? 1 : mult);
    }
    return 0.0f;
}

namespace {

// Sleeps until the given time: a high-resolution timer for most of the wait,
// then a short spin for the last half millisecond.
void WaitUntil(double target) {
    static HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                                 TIMER_ALL_ACCESS);
    double now = Now();
    const double sleepFor = target - now - 0.0005;
    if (sleepFor > 0.0) {
        if (timer) {
            LARGE_INTEGER due;
            due.QuadPart = -static_cast<LONGLONG>(sleepFor * 1e7); // 100 ns units, relative
            if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) WaitForSingleObject(timer, INFINITE);
        } else {
            Sleep(static_cast<DWORD>(sleepFor * 1000.0));
        }
    }
    while (Now() < target) YieldProcessor();
}

} // namespace

float LimitFrameRate() {
    static double next = 0.0;
    const float fps = FrameRateCap();
    if (fps <= 1.0f) {
        next = 0.0;
        return 0.0f;
    }
    const double interval = 1.0 / fps;
    double now = Now();
    if (next <= 0.0 || now - next > interval * 4.0) { // first frame, or after a hitch / loading
        next = now + interval;
        return fps;
    }
    if (now < next) {
        WaitUntil(next);
        now = Now();
    }
    next += interval;
    if (next < now) next = now; // running slower than the cap: don't try to catch up
    return fps;
}

bool& InHook() {
    thread_local bool inHook = false;
    return inHook;
}

} // namespace lsfgi

// State shared by every hook inside one Nitro DLL: settings, shaders, hotkeys.
#pragma once

#include <windows.h>

#include <atomic>
#include <string>

#include "../common/config.h"
#include "../common/shader_extract.h"

namespace lsfgi {

struct Globals {
    HMODULE self = nullptr;
    std::wstring dir;   // folder of the Nitro DLL
    Config cfg;
    ShaderBlobs shaders;
    bool ready = false; // settings + shaders loaded
    std::atomic<bool> fgEnabled{true};
    std::atomic<int> multiplier{2};
    std::atomic<bool> menuOpen{false};
    std::atomic<int> settingsVersion{0}; // bumped when anything needing a rebuild changes

    // Live status for the in-game menu
    const char* apiName = "";
    UINT width = 0, height = 0;
    float realFps = 0.0f, outputFps = 0.0f;
    float refreshHz = 0.0f;
};

Globals& G();

// Loads Nitro.ini and the shaders from Lossless.dll (next to the DLL unless
// the ini says otherwise). Opens <logName> in the DLL folder. Returns G().ready.
bool LoadRuntime(HMODULE self, const wchar_t* logName);

// Edge-triggered toggle/cycle hotkeys. Returns true if the multiplier changed.
bool PollHotkeys();

// Marks the pipeline settings as changed (multiplier, flow scale, mode, HDR).
void BumpSettings();

// How many frames to generate for the real frame being presented now, and
// at which in-between times ((i+1)/denom). Fixed mode: multiplier - 1.
// Dynamic mode: enough to fill the display refreshes this frame took.
struct FramePlan {
    int gens = 0;
    int denom = 2;
};
FramePlan PlanFrame(HWND window);

// Frame limiter for the game's real frames (Nitro.ini FpsLimitMode/FpsLimit).
// Call once per real frame, before presenting it. Returns the cap in fps
// (0 = off).
float LimitFrameRate();
// The cap LimitFrameRate would use right now (0 = off).
float FrameRateCap();

// Call once per real frame: generated = frames generated for it.
void NoteFrame(int generated, const char* api, UINT width, UINT height);

// Claims "this process is handled by Nitro". Only the first caller gets true,
// so two Nitro DLLs in one game never both generate frames.
bool ClaimProcess();

// Per-thread re-entrancy flag shared by all present hooks (a driver may present
// through DXGI from inside SwapBuffers, for example).
bool& InHook();

} // namespace lsfgi

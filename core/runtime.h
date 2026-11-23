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
};

Globals& G();

// Loads Nitro.ini and the shaders from Lossless.dll (next to the DLL unless
// the ini says otherwise). Opens <logName> in the DLL folder. Returns G().ready.
bool LoadRuntime(HMODULE self, const wchar_t* logName);

// Edge-triggered toggle/cycle hotkeys. Returns true if the multiplier changed.
bool PollHotkeys();

// Claims "this process is handled by Nitro". Only the first caller gets true,
// so two Nitro DLLs in one game never both generate frames.
bool ClaimProcess();

// Per-thread re-entrancy flag shared by all present hooks (a driver may present
// through DXGI from inside SwapBuffers, for example).
bool& InHook();

} // namespace lsfgi

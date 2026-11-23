#pragma once

#include <windows.h>

#include <atomic>

#include "../common/config.h"
#include "../common/shader_extract.h"

namespace lsfgi {

struct Globals {
    HMODULE self = nullptr;
    Config cfg;
    ShaderBlobs shaders;
    std::atomic<bool> fgEnabled{true};
    std::atomic<int> multiplier{2};
};

Globals& G();

// Thread entry: loads config + shaders and installs the swap chain hooks.
// Safe to call more than once (only the first call does anything).
DWORD WINAPI InitHooksThread(LPVOID);

// Address of an export in the real System32 dxgi.dll (set up in DllMain).
void* RealDxgiProc(const char* name);

} // namespace lsfgi

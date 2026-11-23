#pragma once

#include <windows.h>

#include <string>

namespace lsfgi {

// Settings shared by the launcher (writes them) and the hook DLL (reads them).
// Stored in Nitro.ini next to the hook DLL.
struct Config {
    std::wstring shaders;        // Nitro.shaders pack (preferred) ...
    std::wstring losslessDll;    // ... or the user's Lossless.dll
    int multiplier = 2;          // 2x, 3x or 4x
    float flowScale = 1.0f;      // 0.25 .. 1.0 (lower = faster, less accurate)
    bool performanceMode = false;
    int hdr = -1;                // -1 auto, 0 off, 1 on
    bool vsyncPacing = true;     // present every frame with vsync so the display paces them
    bool enabled = true;         // start with frame gen on
    int toggleKey = VK_END;      // in-game: turn frame gen on/off
    int cycleKey = VK_HOME;      // in-game: cycle 2x -> 3x -> 4x
    int menuKey = VK_INSERT;     // in-game: open/close the Nitro menu
    int shaderOffset = -1;       // -1 auto-detect
    bool probeD3D12 = true;      // create a throwaway D3D12 swap chain to find its vtable
    std::wstring api = L"auto"; // auto, dxgi (DX10/11/12), d3d9, opengl, all
};

constexpr wchar_t kConfigFileName[] = L"Nitro.ini";
constexpr wchar_t kConfigSection[] = L"Nitro";

int ClampMultiplier(int m);
float ClampFlowScale(float f);

void LoadConfig(const std::wstring& iniPath, Config& cfg);
bool SaveConfig(const std::wstring& iniPath, const Config& cfg);

// Folder containing the given module (nullptr = the current exe).
std::wstring ModuleDirectory(HMODULE module);

} // namespace lsfgi

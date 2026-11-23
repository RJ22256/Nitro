#pragma once

#include <windows.h>

namespace lsfgi {

// Thread entry: loads settings + shaders and installs the hooks for the
// configured graphics API. Only the first call does anything.
DWORD WINAPI InitHooksThread(LPVOID);

void InstallDxgiHooks();  // DX10 / DX11 / DX12
void InstallD3D9Hooks();  // DX9 / DX9Ex
void InstallGLHooks();    // OpenGL

// Creates a hidden 64x64 window for throwaway devices / swap chains.
HWND CreateDummyWindow();

} // namespace lsfgi

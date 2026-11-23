// Starts a game suspended, loads a Nitro hook DLL into it, runs the DLL's
// Nitro_InitHooks (so hooks are in place before the game runs), then resumes.
// The game and this process must be the same bitness.
#pragma once

#include <windows.h>

#include <string>

namespace lsfgi {

struct LaunchResult {
    bool started = false;
    bool injected = false;
    DWORD error = 0; // GetLastError() when the game could not be started
    HANDLE process = nullptr; // the game process (caller closes it), when started
};

LaunchResult LaunchInjected(const std::wstring& exe, const std::wstring& args, const std::wstring& dllPath);

// Exit codes used by NitroInject32.exe
constexpr DWORD kInjectOk = 0;
constexpr DWORD kInjectStartedOnly = 1;
constexpr DWORD kInjectNotStarted = 2;

} // namespace lsfgi

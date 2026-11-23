// NitroInject32.exe: the 64-bit Nitro app can't load a DLL into a 32-bit game,
// so it asks this small 32-bit helper to do it.
// Usage: NitroInject32.exe "<game.exe>" "<NitroHook32.dll>"   (game arguments in NITRO_ARGS)

#include <windows.h>
#include <shellapi.h>

#include <string>
#include <vector>

#include "../common/inject.h"

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv || argc < 3) return static_cast<int>(lsfgi::kInjectNotStarted);

    std::wstring args;
    const DWORD n = GetEnvironmentVariableW(L"NITRO_ARGS", nullptr, 0);
    if (n > 1) {
        std::vector<wchar_t> buf(n);
        GetEnvironmentVariableW(L"NITRO_ARGS", buf.data(), n);
        args = buf.data();
    }
    SetEnvironmentVariableW(L"NITRO_ARGS", nullptr); // don't leak into the game

    const lsfgi::LaunchResult r = lsfgi::LaunchInjected(argv[1], args, argv[2]);
    LocalFree(argv);
    if (r.process) CloseHandle(r.process);
    if (!r.started) {
        SetLastError(r.error);
        return static_cast<int>(lsfgi::kInjectNotStarted);
    }
    return static_cast<int>(r.injected ? lsfgi::kInjectOk : lsfgi::kInjectStartedOnly);
}

// Nitro launcher: pick a game, check Lossless.dll, choose settings,
// then either install the hook into the game folder (as dxgi.dll) or start the
// game with the hook injected.

#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <psapi.h>

#include <algorithm>
#include <cwchar>
#include <cwctype>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "../common/config.h"
#include "../common/shader_extract.h"

#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' " \
                        "version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

using namespace lsfgi;

namespace {

constexpr wchar_t kHookDll[] = L"NitroHook.dll";
constexpr wchar_t kProxyName[] = L"dxgi.dll";
constexpr wchar_t kBackupName[] = L"dxgi.dll.nitro-backup";
constexpr char kMarker[] = "NITRO_PROXY_MARKER_v1";

enum : int {
    ID_EXE = 100, ID_BROWSE, ID_EXEINFO, ID_ARGS, ID_LSSTATUS, ID_RECHECK, ID_MULT, ID_FLOW,
    ID_FLOWVAL, ID_PERF, ID_HDR, ID_VSYNC, ID_INSTALL, ID_UNINSTALL, ID_LAUNCH, ID_LOG,
};

HINSTANCE g_inst;
HWND g_wnd;
HFONT g_font;
int g_dpi = 96;
std::wstring g_dir;          // launcher folder
std::wstring g_losslessPath; // Lossless.dll that will be used
bool g_losslessOk = false;

int S(int v) { return MulDiv(v, g_dpi, 96); }
HWND Item(int id) { return GetDlgItem(g_wnd, id); }

std::wstring GetText(int id) {
    HWND h = Item(id);
    const int n = GetWindowTextLengthW(h);
    std::wstring s(static_cast<size_t>(n) + 1, L'\0');
    GetWindowTextW(h, s.data(), n + 1);
    s.resize(static_cast<size_t>(n));
    return s;
}

std::wstring Trim(std::wstring s) {
    while (!s.empty() && (iswspace(s.back()) || s.back() == L'"')) s.pop_back();
    size_t i = 0;
    while (i < s.size() && (iswspace(s[i]) || s[i] == L'"')) ++i;
    return s.substr(i);
}

bool FileExists(const std::wstring& p) {
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool DirExists(const std::wstring& p) {
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring Parent(const std::wstring& p) {
    const size_t s = p.find_last_of(L"\\/");
    return s == std::wstring::npos ? L"." : p.substr(0, s);
}

std::wstring ErrorText(DWORD code) {
    wchar_t* buf = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, code, 0, reinterpret_cast<LPWSTR>(&buf), 0, nullptr);
    std::wstring s = buf ? buf : L"";
    if (buf) LocalFree(buf);
    while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r')) s.pop_back();
    return s + L" (" + std::to_wstring(code) + L")";
}

void Log(const std::wstring& line) {
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t stamp[16];
    swprintf(stamp, 16, L"[%02d:%02d:%02d] ", t.wHour, t.wMinute, t.wSecond);
    HWND h = Item(ID_LOG);
    const int len = GetWindowTextLengthW(h);
    SendMessageW(h, EM_SETSEL, len, len);
    const std::wstring text = std::wstring(len ? L"\r\n" : L"") + stamp + line;
    SendMessageW(h, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(text.c_str()));
}

// ---------------------------------------------------------------------------
// Game executable analysis (PE headers + import tables)

struct ExeInfo {
    bool valid = false;
    bool x64 = false;
    std::vector<std::string> imports;
};

ExeInfo ReadExe(const std::wstring& path) {
    ExeInfo info;
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return info;
    LARGE_INTEGER size{};
    GetFileSizeEx(file, &size);
    HANDLE map = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    CloseHandle(file);
    if (!map) return info;
    const auto* base = static_cast<const uint8_t*>(MapViewOfFile(map, FILE_MAP_READ, 0, 0, 0));
    CloseHandle(map);
    if (!base) return info;
    const size_t n = static_cast<size_t>(size.QuadPart);

    auto in = [&](size_t off, size_t len) { return off <= n && len <= n - off; };
    do {
        if (!in(0, sizeof(IMAGE_DOS_HEADER))) break;
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) break;
        const size_t ntOff = static_cast<size_t>(dos->e_lfanew);
        if (!in(ntOff, sizeof(IMAGE_NT_HEADERS64))) break;
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + ntOff);
        if (nt->Signature != IMAGE_NT_SIGNATURE) break;
        info.valid = true;
        info.x64 = nt->FileHeader.Machine == IMAGE_FILE_MACHINE_AMD64 &&
                   nt->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC;
        if (!info.x64) break;

        const auto* sections = IMAGE_FIRST_SECTION(nt);
        const size_t secOff = reinterpret_cast<const uint8_t*>(sections) - base;
        const WORD nsec = nt->FileHeader.NumberOfSections;
        if (!in(secOff, nsec * sizeof(IMAGE_SECTION_HEADER))) break;
        auto rvaToOff = [&](DWORD rva) -> size_t {
            for (WORD i = 0; i < nsec; ++i) {
                const auto& s = sections[i];
                const DWORD span = std::max(s.Misc.VirtualSize, s.SizeOfRawData);
                if (rva >= s.VirtualAddress && rva < s.VirtualAddress + span)
                    return rva - s.VirtualAddress + s.PointerToRawData;
            }
            return SIZE_MAX;
        };
        auto readName = [&](DWORD rva) -> std::string {
            const size_t off = rvaToOff(rva);
            if (off == SIZE_MAX || off >= n) return {};
            std::string s;
            for (size_t i = off; i < n && base[i] && s.size() < 260; ++i)
                s.push_back(static_cast<char>(tolower(base[i])));
            return s;
        };

        const auto& dirs = nt->OptionalHeader.DataDirectory;
        const DWORD impRva = dirs[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
        if (impRva) {
            size_t off = rvaToOff(impRva);
            while (off != SIZE_MAX && in(off, sizeof(IMAGE_IMPORT_DESCRIPTOR))) {
                const auto* d = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + off);
                if (!d->Name) break;
                info.imports.push_back(readName(d->Name));
                off += sizeof(IMAGE_IMPORT_DESCRIPTOR);
            }
        }
        const DWORD delayRva = dirs[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT].VirtualAddress;
        if (delayRva) {
            size_t off = rvaToOff(delayRva);
            while (off != SIZE_MAX && in(off, sizeof(IMAGE_DELAYLOAD_DESCRIPTOR))) {
                const auto* d = reinterpret_cast<const IMAGE_DELAYLOAD_DESCRIPTOR*>(base + off);
                if (!d->DllNameRVA) break;
                DWORD nameRva = d->DllNameRVA;
                if (!(d->Attributes.AllAttributes & 1))
                    nameRva = static_cast<DWORD>(nameRva - nt->OptionalHeader.ImageBase);
                info.imports.push_back(readName(nameRva));
                off += sizeof(IMAGE_DELAYLOAD_DESCRIPTOR);
            }
        }
    } while (false);

    UnmapViewOfFile(base);
    return info;
}

// For Unreal-style games the top-level exe is just a stub that starts
// <Game>\Binaries\Win64\<Game>-Win64-Shipping.exe.
std::wstring FindShippingExe(const std::wstring& dir) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return {};
    std::wstring found;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
        const std::wstring bin = dir + L"\\" + fd.cFileName + L"\\Binaries\\Win64";
        WIN32_FIND_DATAW fe;
        HANDLE h2 = FindFirstFileW((bin + L"\\*Shipping.exe").c_str(), &fe);
        if (h2 != INVALID_HANDLE_VALUE) {
            found = bin + L"\\" + fe.cFileName;
            FindClose(h2);
            break;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return found;
}

void AnalyzeGame() {
    const std::wstring exe = Trim(GetText(ID_EXE));
    if (exe.empty()) {
        SetDlgItemTextW(g_wnd, ID_EXEINFO, L"Choose the game's .exe file.");
        return;
    }
    if (!FileExists(exe)) {
        SetDlgItemTextW(g_wnd, ID_EXEINFO, L"File not found.");
        return;
    }
    const ExeInfo info = ReadExe(exe);
    if (!info.valid) {
        SetDlgItemTextW(g_wnd, ID_EXEINFO, L"This doesn't look like a Windows program.");
        return;
    }
    if (!info.x64) {
        SetDlgItemTextW(g_wnd, ID_EXEINFO, L"32-bit game - not supported (64-bit games only).");
        return;
    }

    auto has = [&](const char* name) {
        return std::find(info.imports.begin(), info.imports.end(), name) != info.imports.end();
    };
    std::wstring api;
    const std::wstring dir = Parent(exe);
    if (has("d3d12.dll") || DirExists(dir + L"\\D3D12")) api += L"DirectX 12  ";
    if (has("d3d11.dll")) api += L"DirectX 11  ";
    if (has("vulkan-1.dll")) api += L"Vulkan (not supported - use DX11/DX12 mode in game)  ";
    if (has("opengl32.dll")) api += L"OpenGL (not supported)  ";
    if (has("d3d9.dll")) api += L"DirectX 9 (not supported)  ";
    if (api.empty()) api = L"loads its renderer dynamically (most DX11/DX12 games do)";

    std::wstring text = L"64-bit game. Renderer: " + api;
    const std::wstring shipping = FindShippingExe(dir);
    if (!shipping.empty() && _wcsicmp(shipping.c_str(), exe.c_str()) != 0)
        text += L"\r\nUnreal Engine stub? The real game exe is probably: " + shipping;
    SetDlgItemTextW(g_wnd, ID_EXEINFO, text.c_str());
}

// ---------------------------------------------------------------------------
// Lossless.dll

std::wstring SteamLosslessPath() {
    wchar_t steam[MAX_PATH] = {};
    DWORD size = sizeof(steam);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", RRF_RT_REG_SZ, nullptr,
                     steam, &size) != ERROR_SUCCESS)
        return {};
    std::wstring p = steam;
    std::replace(p.begin(), p.end(), L'/', L'\\');
    return p + L"\\steamapps\\common\\Lossless Scaling\\Lossless.dll";
}

void CheckLossless() {
    g_losslessOk = false;
    g_losslessPath = g_dir + L"\\Lossless.dll";
    std::wstring where = L"injector folder";
    if (!FileExists(g_losslessPath)) {
        const std::wstring steam = SteamLosslessPath();
        if (!steam.empty() && FileExists(steam)) {
            g_losslessPath = steam;
            where = L"your Steam install";
        }
    }

    DllReport report;
    std::wstring err;
    std::wstring status;
    if (!FileExists(g_losslessPath)) {
        status = L"Not found. Copy Lossless.dll from your Lossless Scaling install folder into:\r\n" + g_dir;
    } else if (!InspectLosslessDll(g_losslessPath, report, err)) {
        status = L"Problem: " + err;
    } else {
        g_losslessOk = true;
        status = L"OK (from " + where + L"): " + std::to_wstring(report.dxbc) + L" DirectX shaders, " +
                 L"performance mode " + (report.performanceSet ? L"available" : L"not available") + L".";
    }
    SetDlgItemTextW(g_wnd, ID_LSSTATUS, status.c_str());
    EnableWindow(Item(ID_PERF), !g_losslessOk || report.performanceSet);
}

// ---------------------------------------------------------------------------
// Settings

std::wstring IniPath() { return g_dir + L"\\" + kConfigFileName; }

Config ConfigFromUi() {
    Config c;
    LoadConfig(IniPath(), c); // keep advanced keys (hotkeys etc.) the user may have edited
    c.losslessDll = g_losslessPath;
    c.multiplier = static_cast<int>(SendMessageW(Item(ID_MULT), CB_GETCURSEL, 0, 0)) + 2;
    c.flowScale = static_cast<float>(SendMessageW(Item(ID_FLOW), TBM_GETPOS, 0, 0)) / 100.0f;
    c.performanceMode = IsDlgButtonChecked(g_wnd, ID_PERF) == BST_CHECKED;
    c.vsyncPacing = IsDlgButtonChecked(g_wnd, ID_VSYNC) == BST_CHECKED;
    const LRESULT hdr = SendMessageW(Item(ID_HDR), CB_GETCURSEL, 0, 0);
    c.hdr = hdr == 1 ? 1 : (hdr == 2 ? 0 : -1);
    return c;
}

void SaveUi() {
    SaveConfig(IniPath(), ConfigFromUi());
    WritePrivateProfileStringW(L"Launcher", L"GameExe", Trim(GetText(ID_EXE)).c_str(), IniPath().c_str());
    WritePrivateProfileStringW(L"Launcher", L"Arguments", GetText(ID_ARGS).c_str(), IniPath().c_str());
}

void UpdateFlowLabel() {
    const LRESULT v = SendMessageW(Item(ID_FLOW), TBM_GETPOS, 0, 0);
    SetDlgItemTextW(g_wnd, ID_FLOWVAL, (std::to_wstring(v) + L"%").c_str());
}

void LoadUi() {
    Config c;
    LoadConfig(IniPath(), c);
    wchar_t buf[2048] = {};
    GetPrivateProfileStringW(L"Launcher", L"GameExe", L"", buf, 2048, IniPath().c_str());
    SetDlgItemTextW(g_wnd, ID_EXE, buf);
    GetPrivateProfileStringW(L"Launcher", L"Arguments", L"", buf, 2048, IniPath().c_str());
    SetDlgItemTextW(g_wnd, ID_ARGS, buf);
    SendMessageW(Item(ID_MULT), CB_SETCURSEL, ClampMultiplier(c.multiplier) - 2, 0);
    SendMessageW(Item(ID_FLOW), TBM_SETPOS, TRUE, static_cast<LPARAM>(c.flowScale * 100.0f + 0.5f));
    CheckDlgButton(g_wnd, ID_PERF, c.performanceMode ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(g_wnd, ID_VSYNC, c.vsyncPacing ? BST_CHECKED : BST_UNCHECKED);
    SendMessageW(Item(ID_HDR), CB_SETCURSEL, c.hdr < 0 ? 0 : (c.hdr ? 1 : 2), 0);
    UpdateFlowLabel();
}

// ---------------------------------------------------------------------------
// Install / remove / launch

bool IsOurDll(const std::wstring& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    const std::vector<char> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    const size_t len = sizeof(kMarker) - 1;
    return std::search(data.begin(), data.end(), kMarker, kMarker + len) != data.end();
}

bool Preflight(std::wstring& exe) {
    exe = Trim(GetText(ID_EXE));
    if (exe.empty() || !FileExists(exe)) {
        Log(L"Choose the game's .exe first.");
        return false;
    }
    const ExeInfo info = ReadExe(exe);
    if (!info.valid || !info.x64) {
        Log(L"Only 64-bit games are supported.");
        return false;
    }
    CheckLossless();
    if (!g_losslessOk) {
        Log(L"Lossless.dll is missing or unusable - see the Lossless.dll box above.");
        return false;
    }
    if (!FileExists(g_dir + L"\\" + kHookDll)) {
        Log(std::wstring(kHookDll) + L" is missing from the injector folder.");
        return false;
    }
    return true;
}

void Install() {
    std::wstring exe;
    if (!Preflight(exe)) return;
    SaveUi();
    const std::wstring gameDir = Parent(exe);
    const std::wstring dst = gameDir + L"\\" + kProxyName;
    const std::wstring backup = gameDir + L"\\" + kBackupName;

    if (FileExists(dst) && !IsOurDll(dst)) {
        if (FileExists(backup)) {
            Log(L"The game folder already has a dxgi.dll and a backup from an earlier install. "
                L"Sort those out manually first.");
            return;
        }
        if (!MoveFileW(dst.c_str(), backup.c_str())) {
            Log(L"Could not rename the existing dxgi.dll: " + ErrorText(GetLastError()));
            return;
        }
        Log(L"The game already had a dxgi.dll (another mod such as ReShade?). "
            L"Renamed it to " + std::wstring(kBackupName) + L"; it is not loaded while frame gen is installed.");
    }

    if (!CopyFileW((g_dir + L"\\" + kHookDll).c_str(), dst.c_str(), FALSE)) {
        const DWORD e = GetLastError();
        Log(L"Copy failed: " + ErrorText(e));
        if (e == ERROR_ACCESS_DENIED) Log(L"Try running the injector as administrator.");
        return;
    }
    Config c = ConfigFromUi();
    if (!SaveConfig(gameDir + L"\\" + kConfigFileName, c)) {
        Log(L"Could not write the settings file into the game folder.");
        return;
    }
    Log(L"Installed into " + gameDir + L". Start the game normally (Steam, etc.).");
    Log(L"A log is written to Nitro.log in the game folder.");
}

void Uninstall() {
    const std::wstring exe = Trim(GetText(ID_EXE));
    if (exe.empty()) {
        Log(L"Choose the game's .exe first.");
        return;
    }
    const std::wstring gameDir = Parent(exe);
    const std::wstring dst = gameDir + L"\\" + kProxyName;
    const std::wstring backup = gameDir + L"\\" + kBackupName;

    bool changed = false;
    if (FileExists(dst)) {
        if (!IsOurDll(dst)) {
            Log(L"The dxgi.dll in the game folder isn't ours - left it alone.");
        } else if (!DeleteFileW(dst.c_str())) {
            Log(L"Could not delete dxgi.dll: " + ErrorText(GetLastError()) + L" (is the game still running?)");
            return;
        } else {
            changed = true;
        }
    }
    if (FileExists(backup) && !FileExists(dst)) {
        if (MoveFileW(backup.c_str(), dst.c_str())) {
            Log(L"Restored the game's original dxgi.dll.");
            changed = true;
        }
    }
    DeleteFileW((gameDir + L"\\" + kConfigFileName).c_str());
    DeleteFileW((gameDir + L"\\Nitro.log").c_str());
    Log(changed ? L"Removed from " + gameDir : L"Nothing to remove in " + gameDir);
}

HMODULE FindRemoteModule(HANDLE process, const std::wstring& path) {
    HMODULE mods[1024];
    DWORD needed = 0;
    if (!EnumProcessModulesEx(process, mods, sizeof(mods), &needed, LIST_MODULES_64BIT)) return nullptr;
    const DWORD count = std::min<DWORD>(needed / sizeof(HMODULE), 1024);
    for (DWORD i = 0; i < count; ++i) {
        wchar_t name[MAX_PATH];
        if (GetModuleFileNameExW(process, mods[i], name, MAX_PATH) && _wcsicmp(name, path.c_str()) == 0)
            return mods[i];
    }
    return nullptr;
}

bool RunRemote(HANDLE process, LPTHREAD_START_ROUTINE fn, void* arg, DWORD& exitCode) {
    HANDLE t = CreateRemoteThread(process, nullptr, 0, fn, arg, 0, nullptr);
    if (!t) return false;
    const DWORD wait = WaitForSingleObject(t, 30000);
    GetExitCodeThread(t, &exitCode);
    CloseHandle(t);
    return wait == WAIT_OBJECT_0;
}

void Launch() {
    std::wstring exe;
    if (!Preflight(exe)) return;
    SaveUi(); // the injected DLL reads Nitro.ini from the injector folder

    if (FileExists(Parent(exe) + L"\\" + kProxyName) && IsOurDll(Parent(exe) + L"\\" + kProxyName))
        Log(L"Note: frame gen is also installed in this game folder; only one copy will be active.");

    wchar_t full[MAX_PATH];
    GetFullPathNameW((g_dir + L"\\" + kHookDll).c_str(), MAX_PATH, full, nullptr);
    const std::wstring dll = full;

    // Offset of Nitro_InitHooks inside the hook DLL (same in every process).
    HMODULE local = LoadLibraryExW(dll.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
    const auto* initProc = local ? reinterpret_cast<const uint8_t*>(GetProcAddress(local, "Nitro_InitHooks")) : nullptr;
    const size_t initRva = initProc ? static_cast<size_t>(initProc - reinterpret_cast<const uint8_t*>(local)) : 0;
    if (local) FreeLibrary(local);
    if (!initRva) {
        Log(L"Could not read " + std::wstring(kHookDll) + L".");
        return;
    }

    std::wstring cmd = L"\"" + exe + L"\"";
    const std::wstring args = Trim(GetText(ID_ARGS));
    if (!args.empty()) cmd += L" " + args;
    const std::wstring workDir = Parent(exe);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED, nullptr,
                        workDir.c_str(), &si, &pi)) {
        const DWORD e = GetLastError();
        Log(L"Could not start the game: " + ErrorText(e));
        if (e == ERROR_ELEVATION_REQUIRED) Log(L"The game needs admin rights - run the injector as administrator.");
        return;
    }

    bool ok = false;
    const size_t bytes = (dll.size() + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(pi.hProcess, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (remote && WriteProcessMemory(pi.hProcess, remote, dll.c_str(), bytes, nullptr)) {
        auto loadLibrary = reinterpret_cast<LPTHREAD_START_ROUTINE>(
            GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"));
        DWORD code = 0;
        if (RunRemote(pi.hProcess, loadLibrary, remote, code) && code != 0) {
            if (HMODULE mod = FindRemoteModule(pi.hProcess, dll)) {
                auto init = reinterpret_cast<LPTHREAD_START_ROUTINE>(reinterpret_cast<uint8_t*>(mod) + initRva);
                ok = RunRemote(pi.hProcess, init, nullptr, code);
            }
        }
    }
    if (remote) VirtualFreeEx(pi.hProcess, remote, 0, MEM_RELEASE);

    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    if (ok) {
        Log(L"Game started with frame generation. Log: " + g_dir + L"\\Nitro.log");
        Log(L"If the game restarts itself through Steam, use 'Install into game folder' instead.");
    } else {
        Log(L"Game started, but injection failed (anti-cheat or permissions?). "
            L"Try 'Install into game folder' instead.");
    }
}

void Browse() {
    wchar_t file[MAX_PATH] = {};
    const std::wstring current = Trim(GetText(ID_EXE));
    wcsncpy_s(file, current.c_str(), _TRUNCATE);
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_wnd;
    ofn.lpstrFilter = L"Game executable (*.exe)\0*.exe\0All files\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = L"Choose the game's .exe";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameW(&ofn)) {
        SetDlgItemTextW(g_wnd, ID_EXE, file);
        AnalyzeGame();
        SaveUi();
    }
}

// ---------------------------------------------------------------------------
// Window

HWND Add(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id, DWORD ex = 0) {
    HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w), S(h), g_wnd,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), g_inst, nullptr);
    SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
    return c;
}

void CreateControls() {
    Add(L"STATIC", L"Game executable (.exe):", 0, 14, 12, 400, 18, -1);
    Add(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, 14, 32, 450, 24, ID_EXE, WS_EX_CLIENTEDGE);
    Add(L"BUTTON", L"Browse...", WS_TABSTOP | BS_PUSHBUTTON, 472, 31, 94, 26, ID_BROWSE);
    Add(L"STATIC", L"Choose the game's .exe file.", 0, 14, 62, 552, 36, ID_EXEINFO);
    Add(L"STATIC", L"Launch arguments (only used by 'Launch with injection'):", 0, 14, 102, 552, 18, -1);
    Add(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, 14, 122, 552, 24, ID_ARGS, WS_EX_CLIENTEDGE);

    Add(L"BUTTON", L"Lossless.dll", BS_GROUPBOX, 14, 156, 552, 68, -1);
    Add(L"STATIC", L"", 0, 26, 176, 430, 42, ID_LSSTATUS);
    Add(L"BUTTON", L"Re-check", WS_TABSTOP | BS_PUSHBUTTON, 466, 180, 88, 26, ID_RECHECK);

    Add(L"BUTTON", L"Frame generation", BS_GROUPBOX, 14, 232, 552, 170, -1);
    Add(L"STATIC", L"Multiplier:", 0, 26, 258, 90, 20, -1);
    HWND mult = Add(L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, 116, 254, 90, 200, ID_MULT);
    for (const wchar_t* s : {L"2x", L"3x", L"4x"}) SendMessageW(mult, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(s));
    Add(L"STATIC", L"Flow scale:", 0, 236, 258, 80, 20, -1);
    HWND flow = Add(TRACKBAR_CLASSW, L"", WS_TABSTOP | TBS_HORZ | TBS_NOTICKS, 312, 252, 186, 30, ID_FLOW);
    SendMessageW(flow, TBM_SETRANGE, TRUE, MAKELPARAM(25, 100));
    SendMessageW(flow, TBM_SETLINESIZE, 0, 5);
    SendMessageW(flow, TBM_SETPAGESIZE, 0, 10);
    Add(L"STATIC", L"100%", 0, 504, 258, 50, 20, ID_FLOWVAL);

    Add(L"BUTTON", L"Performance mode (faster, lower quality)", WS_TABSTOP | BS_AUTOCHECKBOX, 26, 290, 310, 22, ID_PERF);
    Add(L"STATIC", L"HDR:", 0, 352, 294, 40, 20, -1);
    HWND hdr = Add(L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, 394, 290, 110, 200, ID_HDR);
    for (const wchar_t* s : {L"Auto", L"On", L"Off"}) SendMessageW(hdr, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(s));
    Add(L"BUTTON", L"Pace generated frames with VSync (recommended)", WS_TABSTOP | BS_AUTOCHECKBOX, 26, 318, 500, 22, ID_VSYNC);
    Add(L"STATIC",
        L"In game: End = frame gen on/off, Home = cycle 2x / 3x / 4x.\r\n"
        L"Cap the game's FPS to refresh rate ÷ multiplier (e.g. 60 FPS on a 240 Hz screen for 4x).",
        0, 26, 346, 530, 48, -1);

    Add(L"BUTTON", L"Install into game folder", WS_TABSTOP | BS_PUSHBUTTON, 14, 414, 180, 32, ID_INSTALL);
    Add(L"BUTTON", L"Remove from game folder", WS_TABSTOP | BS_PUSHBUTTON, 204, 414, 180, 32, ID_UNINSTALL);
    Add(L"BUTTON", L"Launch with injection", WS_TABSTOP | BS_PUSHBUTTON, 394, 414, 172, 32, ID_LAUNCH);
    Add(L"EDIT", L"", WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL, 14, 458, 552, 150, ID_LOG,
        WS_EX_CLIENTEDGE);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        g_wnd = hwnd;
        CreateControls();
        LoadUi();
        CheckLossless();
        AnalyzeGame();
        Log(L"Ready. Single-player games only - never use this with online/anti-cheat games.");
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_BROWSE: Browse(); break;
        case ID_RECHECK: CheckLossless(); break;
        case ID_INSTALL: Install(); break;
        case ID_UNINSTALL: Uninstall(); break;
        case ID_LAUNCH: Launch(); break;
        case ID_EXE:
            if (HIWORD(wp) == EN_KILLFOCUS) AnalyzeGame();
            break;
        }
        return 0;
    case WM_HSCROLL:
        if (reinterpret_cast<HWND>(lp) == Item(ID_FLOW)) {
            // snap to 5% steps
            const LRESULT v = SendMessageW(Item(ID_FLOW), TBM_GETPOS, 0, 0);
            SendMessageW(Item(ID_FLOW), TBM_SETPOS, TRUE, ((v + 2) / 5) * 5);
            UpdateFlowLabel();
        }
        return 0;
    case WM_CLOSE:
        SaveUi();
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int show) {
    g_inst = inst;
    SetProcessDPIAware();
    HDC screen = GetDC(nullptr);
    g_dpi = GetDeviceCaps(screen, LOGPIXELSX);
    ReleaseDC(nullptr, screen);

    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_BAR_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);

    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof(ncm);
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    g_font = CreateFontIndirectW(&ncm.lfMessageFont);

    g_dir = ModuleDirectory(nullptr);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.hIcon = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON,
        GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR));
    wc.hIconSm = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
    wc.lpszClassName = L"NitroLauncher";
    RegisterClassExW(&wc);

    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT rc{0, 0, S(580), S(622)};
    AdjustWindowRect(&rc, style, FALSE);
    HWND wnd = CreateWindowExW(0, wc.lpszClassName, L"Nitro - frame generation for any game",
                               style, CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top,
                               nullptr, nullptr, inst, nullptr);
    if (!wnd) return 1;
    ShowWindow(wnd, show);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(wnd, &m)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    DeleteObject(g_font);
    return 0;
}

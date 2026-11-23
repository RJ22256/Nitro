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
constexpr wchar_t kVkDll[] = L"NitroVk.dll";
constexpr wchar_t kVkManifest[] = L"NitroVk.json";
constexpr wchar_t kBackupSuffix[] = L".nitro-backup";
constexpr const wchar_t* kProxyNames[] = {L"dxgi.dll", L"d3d9.dll", L"version.dll"};
constexpr wchar_t kVkLayersKey[] = L"Software\\Khronos\\Vulkan\\ImplicitLayers";

enum class Api { Auto, Dxgi, D3D9, OpenGL, Vulkan, Unknown };
constexpr char kMarker[] = "NITRO_PROXY_MARKER_v1";

enum : int {
    ID_EXE = 100, ID_BROWSE, ID_EXEINFO, ID_ARGS, ID_LSSTATUS, ID_RECHECK, ID_MULT, ID_FLOW,
    ID_FLOWVAL, ID_PERF, ID_HDR, ID_VSYNC, ID_INSTALL, ID_UNINSTALL, ID_LAUNCH, ID_LOG, ID_API,
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

// Imports of the exe plus the DLLs next to it (engines often keep the renderer
// in a DLL, e.g. UnityPlayer.dll).
std::vector<std::string> FolderImports(const std::wstring& exe) {
    std::vector<std::string> all = ReadExe(exe).imports;
    const std::wstring dir = Parent(exe);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*.dll").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return all;
    int scanned = 0;
    do {
        const ExeInfo dll = ReadExe(dir + L"\\" + fd.cFileName);
        all.insert(all.end(), dll.imports.begin(), dll.imports.end());
    } while (++scanned < 80 && FindNextFileW(h, &fd));
    FindClose(h);
    return all;
}

bool Has(const std::vector<std::string>& v, const char* name) {
    return std::find(v.begin(), v.end(), name) != v.end();
}

Api DetectApi(const std::wstring& exe, std::wstring& label) {
    const std::vector<std::string> imp = FolderImports(exe);
    const std::wstring dir = Parent(exe);
    const bool dx12 = Has(imp, "d3d12.dll") || DirExists(dir + L"\\D3D12");
    const bool dx11 = Has(imp, "d3d11.dll");
    const bool dx10 = Has(imp, "d3d10.dll") || Has(imp, "d3d10_1.dll");
    if (dx12 || dx11 || dx10 || Has(imp, "dxgi.dll")) {
        label = dx12 ? L"DirectX 12" : dx11 ? L"DirectX 11" : dx10 ? L"DirectX 10" : L"DirectX (DXGI)";
        return Api::Dxgi;
    }
    if (Has(imp, "d3d9.dll")) return label = L"DirectX 9", Api::D3D9;
    if (Has(imp, "opengl32.dll")) return label = L"OpenGL", Api::OpenGL;
    if (Has(imp, "vulkan-1.dll")) return label = L"Vulkan", Api::Vulkan;
    label = L"not detected (renderer loaded at run time - assuming DirectX 11/12)";
    return Api::Unknown;
}

// The API chosen in the drop-down, or the detected one for "Auto".
Api ChosenApi(const std::wstring& exe) {
    const LRESULT sel = SendMessageW(Item(ID_API), CB_GETCURSEL, 0, 0);
    switch (sel) {
    case 1: return Api::Dxgi;
    case 2: return Api::D3D9;
    case 3: return Api::OpenGL;
    case 4: return Api::Vulkan;
    default: break;
    }
    std::wstring label;
    const Api a = DetectApi(exe, label);
    return a == Api::Unknown ? Api::Dxgi : a;
}

const wchar_t* ApiName(Api a) {
    switch (a) {
    case Api::Dxgi: return L"DirectX 10/11/12";
    case Api::D3D9: return L"DirectX 9";
    case Api::OpenGL: return L"OpenGL";
    case Api::Vulkan: return L"Vulkan";
    default: return L"unknown";
    }
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
    std::wstring label;
    DetectApi(exe, label);
    std::wstring text = L"64-bit game. Renderer: " + label;
    const std::wstring shipping = FindShippingExe(Parent(exe));
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
    c.api = L"auto";
    return c;
}

void SaveUi() {
    SaveConfig(IniPath(), ConfigFromUi());
    WritePrivateProfileStringW(L"Launcher", L"GameExe", Trim(GetText(ID_EXE)).c_str(), IniPath().c_str());
    WritePrivateProfileStringW(L"Launcher", L"Arguments", GetText(ID_ARGS).c_str(), IniPath().c_str());
    WritePrivateProfileStringW(
        L"Launcher", L"ApiChoice",
        std::to_wstring(SendMessageW(Item(ID_API), CB_GETCURSEL, 0, 0)).c_str(), IniPath().c_str());
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
    SendMessageW(Item(ID_API), CB_SETCURSEL,
                 static_cast<WPARAM>(GetPrivateProfileIntW(L"Launcher", L"ApiChoice", 0, IniPath().c_str())), 0);
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

bool Preflight(std::wstring& exe, Api& api) {
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
    api = ChosenApi(exe);
    const wchar_t* need = api == Api::Vulkan ? kVkDll : kHookDll;
    if (!FileExists(g_dir + L"\\" + need)) {
        Log(std::wstring(need) + L" is missing from the Nitro folder.");
        return false;
    }
    if (api == Api::Vulkan && !FileExists(g_dir + L"\\" + kVkManifest)) {
        Log(std::wstring(kVkManifest) + L" is missing from the Nitro folder.");
        return false;
    }
    return true;
}

std::wstring Lower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), ::towlower);
    return s;
}

std::wstring ManifestPath() {
    wchar_t full[MAX_PATH];
    GetFullPathNameW((g_dir + L"\\" + kVkManifest).c_str(), MAX_PATH, full, nullptr);
    return full;
}

bool RegisterVulkanLayer() {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kVkLayersKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) !=
        ERROR_SUCCESS)
        return false;
    const DWORD zero = 0;
    const LONG r = RegSetValueExW(key, ManifestPath().c_str(), 0, REG_DWORD, reinterpret_cast<const BYTE*>(&zero),
                                  sizeof(zero));
    RegCloseKey(key);
    return r == ERROR_SUCCESS;
}

bool AnyVulkanGames() {
    wchar_t buf[4096] = {};
    GetPrivateProfileSectionW(L"VulkanGames", buf, 4096, IniPath().c_str());
    return buf[0] != 0;
}

void UnregisterVulkanLayerIfUnused() {
    if (AnyVulkanGames()) return;
    RegDeleteKeyValueW(HKEY_CURRENT_USER, kVkLayersKey, ManifestPath().c_str());
}

// Which system DLL name the hook should stand in for in this game's folder.
const wchar_t* ProxyFor(Api api, const std::wstring& exe) {
    if (api == Api::D3D9) return L"d3d9.dll";
    if (api == Api::OpenGL) {
        const std::vector<std::string> imp = FolderImports(exe);
        if (Has(imp, "version.dll")) return L"version.dll";
        if (Has(imp, "dxgi.dll") || Has(imp, "d3d11.dll")) return L"dxgi.dll";
        return nullptr;
    }
    return L"dxgi.dll";
}

const wchar_t* HookApiSetting(Api api) {
    switch (api) {
    case Api::D3D9: return L"d3d9";
    case Api::OpenGL: return L"opengl";
    default: return L"dxgi";
    }
}

// Removes every Nitro proxy from the game folder and restores backups.
bool RemoveProxies(const std::wstring& gameDir, bool& changed) {
    for (const wchar_t* name : kProxyNames) {
        const std::wstring dst = gameDir + L"\\" + name;
        const std::wstring backup = dst + kBackupSuffix;
        if (FileExists(dst) && IsOurDll(dst)) {
            if (!DeleteFileW(dst.c_str())) {
                Log(L"Could not delete " + std::wstring(name) + L": " + ErrorText(GetLastError()) +
                    L" (is the game still running?)");
                return false;
            }
            changed = true;
        }
        if (FileExists(backup) && !FileExists(dst) && MoveFileW(backup.c_str(), dst.c_str())) {
            Log(L"Restored the game's original " + std::wstring(name) + L".");
            changed = true;
        }
    }
    return true;
}

void Install() {
    std::wstring exe;
    Api api{};
    if (!Preflight(exe, api)) return;
    SaveUi();
    const std::wstring gameDir = Parent(exe);

    if (api == Api::Vulkan) {
        if (!RegisterVulkanLayer()) {
            Log(L"Could not register the Nitro Vulkan layer in the registry.");
            return;
        }
        WritePrivateProfileStringW(L"VulkanGames", Lower(exe).c_str(), L"1", IniPath().c_str());
        Log(L"Vulkan game enabled. Nothing was copied into the game folder; Nitro's Vulkan layer "
            L"switches on for this exe only. Start the game normally.");
        Log(L"Settings come from the Nitro folder; the log is NitroVk.log there.");
        return;
    }

    const wchar_t* proxy = ProxyFor(api, exe);
    if (!proxy) {
        Log(L"This OpenGL game doesn't load any DLL Nitro can stand in for. Use 'Launch with injection' instead.");
        return;
    }
    bool changed = false;
    if (!RemoveProxies(gameDir, changed)) return; // switching APIs: clear old installs first

    const std::wstring dst = gameDir + L"\\" + proxy;
    const std::wstring backup = dst + kBackupSuffix;
    if (FileExists(dst)) {
        if (FileExists(backup)) {
            Log(L"The game folder already has " + std::wstring(proxy) + L" and a backup of it. Sort those out first.");
            return;
        }
        if (!MoveFileW(dst.c_str(), backup.c_str())) {
            Log(L"Could not rename the existing " + std::wstring(proxy) + L": " + ErrorText(GetLastError()));
            return;
        }
        Log(L"The game already had a " + std::wstring(proxy) + L" (another mod?). Renamed it to " + proxy +
            kBackupSuffix + L"; Remove puts it back.");
    }
    if (!CopyFileW((g_dir + L"\\" + kHookDll).c_str(), dst.c_str(), FALSE)) {
        const DWORD e = GetLastError();
        Log(L"Copy failed: " + ErrorText(e));
        if (e == ERROR_ACCESS_DENIED) Log(L"Try running Nitro as administrator.");
        return;
    }
    Config c = ConfigFromUi();
    c.api = HookApiSetting(api);
    if (!SaveConfig(gameDir + L"\\" + kConfigFileName, c)) {
        Log(L"Could not write the settings file into the game folder.");
        return;
    }
    Log(std::wstring(L"Installed for ") + ApiName(api) + L" as " + proxy + L" in " + gameDir +
        L". Start the game normally (Steam, etc.).");
    Log(L"A log is written to Nitro.log in the game folder.");
}

void Uninstall() {
    const std::wstring exe = Trim(GetText(ID_EXE));
    if (exe.empty()) {
        Log(L"Choose the game's .exe first.");
        return;
    }
    const std::wstring gameDir = Parent(exe);
    bool changed = false;
    if (!RemoveProxies(gameDir, changed)) return;

    wchar_t flag[8] = {};
    if (GetPrivateProfileStringW(L"VulkanGames", Lower(exe).c_str(), L"", flag, 8, IniPath().c_str()) > 0) {
        WritePrivateProfileStringW(L"VulkanGames", Lower(exe).c_str(), nullptr, IniPath().c_str());
        UnregisterVulkanLayerIfUnused();
        changed = true;
    }
    DeleteFileW((gameDir + L"\\" + kConfigFileName).c_str());
    DeleteFileW((gameDir + L"\\Nitro.log").c_str());
    Log(changed ? L"Removed Nitro from " + gameDir : L"Nothing to remove for " + gameDir);
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
    Api api{};
    if (!Preflight(exe, api)) return;
    SaveUi(); // the injected DLL / Vulkan layer read Nitro.ini from the Nitro folder

    std::wstring cmd = L"\"" + exe + L"\"";
    const std::wstring args = Trim(GetText(ID_ARGS));
    if (!args.empty()) cmd += L" " + args;
    const std::wstring workDir = Parent(exe);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    if (api == Api::Vulkan) {
        if (!RegisterVulkanLayer()) {
            Log(L"Could not register the Nitro Vulkan layer in the registry.");
            return;
        }
        SetEnvironmentVariableW(L"NITRO_VULKAN", L"1"); // inherited by the game only
        const BOOL ok = CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr,
                                       workDir.c_str(), &si, &pi);
        SetEnvironmentVariableW(L"NITRO_VULKAN", nullptr);
        if (!ok) {
            Log(L"Could not start the game: " + ErrorText(GetLastError()));
            return;
        }
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        Log(L"Vulkan game started with Nitro's layer. Log: " + g_dir + L"\\NitroVk.log");
        return;
    }

    {
        Config c = ConfigFromUi();
        c.api = HookApiSetting(api);
        SaveConfig(IniPath(), c);
    }

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

    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED, nullptr,
                        workDir.c_str(), &si, &pi)) {
        const DWORD e = GetLastError();
        Log(L"Could not start the game: " + ErrorText(e));
        if (e == ERROR_ELEVATION_REQUIRED) Log(L"The game needs admin rights - run Nitro as administrator.");
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
        Log(std::wstring(L"Game started with Nitro (") + ApiName(api) + L"). Log: " + g_dir + L"\\Nitro.log");
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

    Add(L"BUTTON", L"Frame generation", BS_GROUPBOX, 14, 232, 552, 202, -1);
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
    Add(L"STATIC", L"Graphics API:", 0, 26, 352, 90, 20, -1);
    HWND api = Add(L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, 116, 348, 220, 200, ID_API);
    for (const wchar_t* s : {L"Auto-detect", L"DirectX 10 / 11 / 12", L"DirectX 9", L"OpenGL", L"Vulkan"})
        SendMessageW(api, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(s));
    SendMessageW(api, CB_SETCURSEL, 0, 0);
    Add(L"STATIC",
        L"In game: End = frame gen on/off, Home = cycle 2x / 3x / 4x.\r\n"
        L"Cap the game's FPS to refresh rate ÷ multiplier (e.g. 60 FPS on a 240 Hz screen for 4x).",
        0, 26, 380, 530, 48, -1);

    Add(L"BUTTON", L"Install into game folder", WS_TABSTOP | BS_PUSHBUTTON, 14, 446, 180, 32, ID_INSTALL);
    Add(L"BUTTON", L"Remove from game folder", WS_TABSTOP | BS_PUSHBUTTON, 204, 446, 180, 32, ID_UNINSTALL);
    Add(L"BUTTON", L"Launch with injection", WS_TABSTOP | BS_PUSHBUTTON, 394, 446, 172, 32, ID_LAUNCH);
    Add(L"EDIT", L"", WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL, 14, 490, 552, 150, ID_LOG,
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
        Log(L"Ready. Supports DirectX 9/10/11/12, OpenGL and Vulkan. Single-player games only - "
            L"never use this with online/anti-cheat games.");
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
    RECT rc{0, 0, S(580), S(654)};
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

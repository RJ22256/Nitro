// Nitro app: pick a game, check Lossless.dll, choose settings,
// then either install the hook into the game folder (as dxgi.dll) or start the
// game with the hook injected.

#include <windows.h>
#include <commdlg.h>
#include <d3d11.h>
#include <dwmapi.h>
#include <psapi.h>
#include <shellapi.h>
#include <wrl/client.h>

#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#include <algorithm>
#include <cwchar>
#include <cwctype>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "../common/config.h"
#include "../common/shader_extract.h"
#include "../core/nitro_ui.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

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

// Everything the window shows / edits.
struct UiState {
    char exe[1024] = {};
    char args[512] = {};
    int multiplier = 2;
    int flowPct = 100;
    bool performance = false;
    bool dynamic = false;
    bool vsync = true;
    int hdr = 0; // 0 auto, 1 on, 2 off
    int api = 0; // 0 auto, 1 DXGI, 2 D3D9, 3 OpenGL, 4 Vulkan
    std::string exeInfo = "Choose the game's .exe file.";
    std::string exeHint;
    std::string detected;
    std::string lossStatus;
    bool perfAvailable = true;
    bool shadersOutdated = false; // Lossless Scaling updated since the import
    std::vector<std::pair<ImVec4, std::string>> log;
    bool scrollLog = false;
};

UiState ui;
HWND g_wnd = nullptr;
std::wstring g_dir;          // Nitro folder
std::wstring g_losslessPath; // Lossless.dll the shaders were imported from
bool g_losslessOk = false;   // Nitro.shaders is present and valid

// First-run setup
struct FoundInstall {
    std::wstring path;
    bool ok = false;
    std::string status;
};
struct Wizard {
    bool active = false;
    int step = 0; // 0 welcome, 1 find + import, 2 done
    bool scanned = false;
    std::vector<FoundInstall> found;
    int selected = -1;
    std::string error;
    size_t imported = 0;
    bool performance = false;
} wiz;

std::wstring W(const char* utf8) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
    std::wstring w(static_cast<size_t>(n > 0 ? n - 1 : 0), L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, utf8, -1, w.data(), n);
    return w;
}

std::string U(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

void CopyTo(char* dst, size_t cap, const std::string& src) {
    strncpy_s(dst, cap, src.c_str(), _TRUNCATE);
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

void Log(const std::wstring& line, ImVec4 color = nitro_ui::kText) {
    SYSTEMTIME t;
    GetLocalTime(&t);
    char stamp[16];
    snprintf(stamp, sizeof(stamp), "%02d:%02d:%02d  ", t.wHour, t.wMinute, t.wSecond);
    ui.log.emplace_back(color, stamp + U(line));
    if (ui.log.size() > 200) ui.log.erase(ui.log.begin());
    ui.scrollLog = true;
}
void LogOk(const std::wstring& line) { Log(line, nitro_ui::kGood); }
void LogErr(const std::wstring& line) { Log(line, nitro_ui::kBad); }

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
    switch (ui.api) {
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
    ui.exeHint.clear();
    ui.detected.clear();
    const std::wstring exe = Trim(W(ui.exe));
    if (exe.empty()) {
        ui.exeInfo = "Choose the game's .exe file.";
        return;
    }
    if (!FileExists(exe)) {
        ui.exeInfo = "File not found.";
        return;
    }
    const ExeInfo info = ReadExe(exe);
    if (!info.valid) {
        ui.exeInfo = "This doesn't look like a Windows program.";
        return;
    }
    if (!info.x64) {
        ui.exeInfo = "32-bit game: not supported (64-bit games only).";
        return;
    }
    std::wstring label;
    DetectApi(exe, label);
    ui.exeInfo = "64-bit game";
    ui.detected = U(label);
    const std::wstring shipping = FindShippingExe(Parent(exe));
    if (!shipping.empty() && _wcsicmp(shipping.c_str(), exe.c_str()) != 0)
        ui.exeHint = U(shipping);
}

// ---------------------------------------------------------------------------
// Lossless.dll

std::wstring PackPath() {
    return g_dir + L"\\" + kShaderPackName;
}

std::wstring RegString(HKEY root, const wchar_t* key, const wchar_t* value) {
    wchar_t buf[MAX_PATH] = {};
    DWORD size = sizeof(buf);
    if (RegGetValueW(root, key, value, RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS) return {};
    std::wstring p = buf;
    std::replace(p.begin(), p.end(), L'/', L'\\');
    return p;
}

// Every Steam library folder on this PC (from libraryfolders.vdf).
std::vector<std::wstring> SteamLibraries() {
    std::vector<std::wstring> roots;
    for (const std::wstring& steam :
         {RegString(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath"),
          RegString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Valve\\Steam", L"InstallPath"),
          std::wstring(L"C:\\Program Files (x86)\\Steam")}) {
        if (steam.empty()) continue;
        roots.push_back(steam);
        std::ifstream vdf(steam + L"\\steamapps\\libraryfolders.vdf");
        std::string line;
        while (std::getline(vdf, line)) {
            const size_t key = line.find("\"path\"");
            if (key == std::string::npos) continue;
            const size_t a = line.find('"', key + 6);
            const size_t b = a == std::string::npos ? a : line.find('"', a + 1);
            if (b == std::string::npos) continue;
            std::string path = line.substr(a + 1, b - a - 1);
            std::string clean;
            for (size_t i = 0; i < path.size(); ++i) {
                if (path[i] == '\\' && i + 1 < path.size() && path[i + 1] == '\\') ++i;
                clean.push_back(path[i]);
            }
            roots.push_back(W(clean.c_str()));
        }
    }
    return roots;
}

void AddCandidate(std::vector<FoundInstall>& list, const std::wstring& dll) {
    if (!FileExists(dll)) return;
    for (const auto& f : list)
        if (_wcsicmp(f.path.c_str(), dll.c_str()) == 0) return;
    FoundInstall f;
    f.path = dll;
    DllReport report;
    std::wstring err;
    f.ok = InspectLosslessDll(dll, report, err);
    f.status = f.ok ? (std::to_string(report.dxbc) + " shaders" +
                       (report.performanceSet ? " \xC2\xB7 performance mode" : ""))
                    : U(err);
    list.push_back(f);
}

void ScanForLossless() {
    wiz.found.clear();
    wiz.selected = -1;
    wiz.error.clear();
    for (const std::wstring& lib : SteamLibraries())
        AddCandidate(wiz.found, lib + L"\\steamapps\\common\\Lossless Scaling\\Lossless.dll");
    AddCandidate(wiz.found, g_dir + L"\\Lossless.dll");
    for (size_t i = 0; i < wiz.found.size(); ++i)
        if (wiz.found[i].ok) {
            wiz.selected = static_cast<int>(i);
            break;
        }
    wiz.scanned = true;
}

void BrowseForLossless() {
    wchar_t file[MAX_PATH] = L"Lossless.dll";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_wnd;
    ofn.lpstrFilter = L"Lossless.dll\0Lossless.dll\0DLL files (*.dll)\0*.dll\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = L"Find Lossless.dll in your Lossless Scaling folder";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) return;
    AddCandidate(wiz.found, file);
    for (size_t i = 0; i < wiz.found.size(); ++i)
        if (_wcsicmp(wiz.found[i].path.c_str(), file) == 0) wiz.selected = static_cast<int>(i);
}

// Reads Nitro.shaders and updates the status shown in the app.
void CheckLossless() {
    PackInfo info;
    std::wstring err;
    g_losslessOk = LoadShaderPack(PackPath(), nullptr, info, err);
    g_losslessPath = info.source;
    ui.shadersOutdated = g_losslessOk && PackSourceChanged(info);
    if (g_losslessOk) {
        ui.lossStatus = "Imported from Lossless Scaling \xC2\xB7 " + std::to_string(info.shaderCount) +
                        " shaders \xC2\xB7 performance mode " + (info.performanceSet ? "available" : "unavailable");
        ui.perfAvailable = info.performanceSet;
    } else {
        ui.lossStatus = "Not set up yet. Run setup to import the frame-gen shaders from Lossless Scaling.";
        ui.perfAvailable = true;
    }
}

bool ImportShaders(const std::wstring& dll) {
    ShaderBlobs blobs;
    int offset = -1;
    std::wstring err;
    if (!LoadLsfgShaders(dll, -1, blobs, offset, err) || !SaveShaderPack(PackPath(), blobs, dll, err)) {
        wiz.error = U(err);
        return false;
    }
    wiz.imported = blobs.size();
    wiz.performance = blobs.find("p_alpha[0]") != blobs.end();
    CheckLossless();
    LogOk(L"Imported " + std::to_wstring(blobs.size()) + L" frame-gen shaders from " + dll);
    return g_losslessOk;
}

void StartSetup(int step) {
    wiz = Wizard{};
    wiz.active = true;
    wiz.step = step;
    if (step == 1) ScanForLossless();
}

// ---------------------------------------------------------------------------
// Settings

std::wstring IniPath() { return g_dir + L"\\" + kConfigFileName; }

Config ConfigFromUi() {
    Config c;
    LoadConfig(IniPath(), c); // keep advanced keys (hotkeys etc.) the user may have edited
    c.shaders = PackPath();
    c.losslessDll = g_losslessPath;
    c.multiplier = ClampMultiplier(ui.multiplier);
    c.flowScale = static_cast<float>(ui.flowPct) / 100.0f;
    c.performanceMode = ui.performance;
    c.dynamic = ui.dynamic;
    c.vsyncPacing = ui.vsync;
    c.hdr = ui.hdr == 1 ? 1 : (ui.hdr == 2 ? 0 : -1);
    c.api = L"auto";
    return c;
}

void SaveUi() {
    SaveConfig(IniPath(), ConfigFromUi());
    WritePrivateProfileStringW(L"Launcher", L"GameExe", Trim(W(ui.exe)).c_str(), IniPath().c_str());
    WritePrivateProfileStringW(L"Launcher", L"Arguments", W(ui.args).c_str(), IniPath().c_str());
    WritePrivateProfileStringW(L"Launcher", L"ApiChoice", std::to_wstring(ui.api).c_str(), IniPath().c_str());
}

void LoadUi() {
    Config c;
    LoadConfig(IniPath(), c);
    wchar_t buf[2048] = {};
    GetPrivateProfileStringW(L"Launcher", L"GameExe", L"", buf, 2048, IniPath().c_str());
    CopyTo(ui.exe, sizeof(ui.exe), U(buf));
    GetPrivateProfileStringW(L"Launcher", L"Arguments", L"", buf, 2048, IniPath().c_str());
    CopyTo(ui.args, sizeof(ui.args), U(buf));
    ui.api = static_cast<int>(GetPrivateProfileIntW(L"Launcher", L"ApiChoice", 0, IniPath().c_str()));
    if (ui.api < 0 || ui.api > 4) ui.api = 0;
    ui.multiplier = ClampMultiplier(c.multiplier);
    ui.flowPct = static_cast<int>(c.flowScale * 100.0f + 0.5f);
    ui.performance = c.performanceMode;
    ui.dynamic = c.dynamic;
    ui.vsync = c.vsyncPacing;
    ui.hdr = c.hdr < 0 ? 0 : (c.hdr ? 1 : 2);
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
    exe = Trim(W(ui.exe));
    if (exe.empty() || !FileExists(exe)) {
        LogErr(L"Choose the game's .exe first.");
        return false;
    }
    const ExeInfo info = ReadExe(exe);
    if (!info.valid || !info.x64) {
        LogErr(L"Only 64-bit games are supported.");
        return false;
    }
    CheckLossless();
    if (!g_losslessOk) {
        StartSetup(1);
        LogErr(L"Nitro isn't set up with Lossless Scaling yet - running setup.");
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
            LogErr(L"Could not register the Nitro Vulkan layer in the registry.");
            return;
        }
        WritePrivateProfileStringW(L"VulkanGames", Lower(exe).c_str(), L"1", IniPath().c_str());
        LogOk(L"Vulkan game enabled. Nothing was copied into the game folder; Nitro's Vulkan layer "
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
        LogErr(L"Copy failed: " + ErrorText(e));
        if (e == ERROR_ACCESS_DENIED) Log(L"Try running Nitro as administrator.");
        return;
    }
    Config c = ConfigFromUi();
    c.api = HookApiSetting(api);
    if (!SaveConfig(gameDir + L"\\" + kConfigFileName, c)) {
        Log(L"Could not write the settings file into the game folder.");
        return;
    }
    LogOk(std::wstring(L"Installed for ") + ApiName(api) + L" as " + proxy + L" in " + gameDir +
        L". Start the game normally (Steam, etc.).");
    Log(L"A log is written to Nitro.log in the game folder.");
}

void Uninstall() {
    const std::wstring exe = Trim(W(ui.exe));
    if (exe.empty()) {
        LogErr(L"Choose the game's .exe first.");
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
    if (changed) LogOk(L"Removed Nitro from " + gameDir); else Log(L"Nothing to remove for " + gameDir);
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
    const std::wstring args = Trim(W(ui.args));
    if (!args.empty()) cmd += L" " + args;
    const std::wstring workDir = Parent(exe);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    if (api == Api::Vulkan) {
        if (!RegisterVulkanLayer()) {
            LogErr(L"Could not register the Nitro Vulkan layer in the registry.");
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
        LogOk(L"Vulkan game started with Nitro's layer. Log: " + g_dir + L"\\NitroVk.log");
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
        LogOk(std::wstring(L"Game started with Nitro (") + ApiName(api) + L"). Log: " + g_dir + L"\\Nitro.log");
        Log(L"If the game restarts itself through Steam, use 'Install into game folder' instead.");
    } else {
        LogErr(L"Game started, but injection failed (anti-cheat or permissions?). "
            L"Try 'Install into game folder' instead.");
    }
}

void Browse() {
    wchar_t file[MAX_PATH] = {};
    const std::wstring current = Trim(W(ui.exe));
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
        CopyTo(ui.exe, sizeof(ui.exe), U(file));
        AnalyzeGame();
        SaveUi();
    }
}

// ---------------------------------------------------------------------------
// Window (Dear ImGui on D3D11)

using Microsoft::WRL::ComPtr;

ComPtr<ID3D11Device> g_device;
ComPtr<ID3D11DeviceContext> g_context;
ComPtr<IDXGISwapChain> g_swapChain;
ComPtr<ID3D11RenderTargetView> g_rtv;
nitro_ui::Fonts g_fonts;
float g_scale = 1.0f;
UINT g_resizeW = 0, g_resizeH = 0;

bool CreateDeviceD3D(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2,
                                               D3D11_SDK_VERSION, &sd, &g_swapChain, &g_device, nullptr, &g_context);
    if (FAILED(hr))
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2, D3D11_SDK_VERSION,
                                           &sd, &g_swapChain, &g_device, nullptr, &g_context);
    if (FAILED(hr)) return false;
    ComPtr<ID3D11Texture2D> back;
    g_swapChain->GetBuffer(0, IID_PPV_ARGS(&back));
    g_device->CreateRenderTargetView(back.Get(), nullptr, &g_rtv);
    return true;
}

void ResizeSwapChain() {
    g_rtv.Reset();
    g_swapChain->ResizeBuffers(0, g_resizeW, g_resizeH, DXGI_FORMAT_UNKNOWN, 0);
    ComPtr<ID3D11Texture2D> back;
    g_swapChain->GetBuffer(0, IID_PPV_ARGS(&back));
    g_device->CreateRenderTargetView(back.Get(), nullptr, &g_rtv);
    g_resizeW = g_resizeH = 0;
}

// Text with the logo's cyan -> pink gradient.
void GradientText(ImFont* font, const char* text) {
    ImGui::PushFont(font);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const ImVec2 size = ImGui::CalcTextSize(text);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const int v0 = dl->VtxBuffer.Size;
    dl->AddText(p, IM_COL32_WHITE, text);
    ImGui::ShadeVertsLinearColorGradientKeepAlpha(dl, v0, dl->VtxBuffer.Size, p, ImVec2(p.x + size.x, p.y),
                                                  nitro_ui::kCyan, nitro_ui::kPink);
    ImGui::Dummy(size);
    ImGui::PopFont();
}

void CardTitle(const char* title) {
    ImGui::PushFont(g_fonts.bold);
    ImGui::TextUnformatted(title);
    ImGui::PopFont();
}

bool BeginCard(const char* id, ImVec2 size) {
    ImGuiChildFlags flags = ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding;
    if (size.y <= 0) flags |= ImGuiChildFlags_AutoResizeY; // height 0 = fit contents
    return ImGui::BeginChild(id, size, flags);
}

void KeyRow(const char* key, const char* what) {
    const float y = ImGui::GetCursorPosY();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX());
    const float keyCol = 74 * g_scale;
    const float x0 = ImGui::GetCursorPosX();
    nitro_ui::Chip(key, ImVec4(0.0f, 0.898f, 1.0f, 1.0f));
    ImGui::SameLine(x0 + keyCol);
    ImGui::SetCursorPosY(y + 3.0f); // line up with the chip's text
    ImGui::TextUnformatted(what);
}

void CenteredText(const char* text, ImVec4 color, float wrap) {
    const ImVec2 size = ImGui::CalcTextSize(text, nullptr, false, wrap);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (ImGui::GetContentRegionAvail().x - size.x) * 0.5f);
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrap);
    ImGui::TextColored(color, "%s", text);
    ImGui::PopTextWrapPos();
}

void StepDots(int step) {
    const float s = g_scale;
    const float r = 4.5f * s, gap = 18 * s;
    const float w = gap * 2;
    ImVec2 p = ImGui::GetCursorScreenPos();
    p.x += (ImGui::GetContentRegionAvail().x - w) * 0.5f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (int i = 0; i < 3; ++i)
        dl->AddCircleFilled(ImVec2(p.x + gap * static_cast<float>(i), p.y + r), i == step ? r * 1.25f : r,
                            i == step ? nitro_ui::kCyan : IM_COL32(52, 60, 110, 255));
    ImGui::Dummy(ImVec2(0, r * 2 + 4 * s));
}

void DrawSetup() {
    const float s = g_scale;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("##setup", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleVar(2);

    const float cardW = (std::min)(600 * s, vp->WorkSize.x - 48 * s);
    const float cardH = 560 * s;
    ImGui::SetCursorPos(ImVec2((vp->WorkSize.x - cardW) * 0.5f, (std::max)(24 * s, (vp->WorkSize.y - cardH) * 0.5f)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(36 * s, 30 * s));
    ImGui::BeginChild("##setupcard", ImVec2(cardW, cardH), ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PopStyleVar();
    const float inner = ImGui::GetContentRegionAvail().x;
    StepDots(wiz.step);
    ImGui::Spacing();

    if (wiz.step == 0) {
        const float logo = 132 * s;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (inner - logo) * 0.5f);
        nitro_ui::DrawLogo(ImGui::GetWindowDrawList(), ImGui::GetCursorScreenPos(), logo);
        ImGui::Dummy(ImVec2(logo, logo));
        ImGui::Spacing();
        ImGui::PushFont(g_fonts.title);
        const char* title = "Welcome to Nitro";
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (inner - ImGui::CalcTextSize(title).x) * 0.5f);
        ImGui::PopFont();
        GradientText(g_fonts.title, title);
        ImGui::Spacing();
        CenteredText("Nitro adds Lossless Scaling's frame generation to your games, at up to 4x.", nitro_ui::kText,
                     inner);
        ImGui::Spacing();
        CenteredText("Setup imports the frame-gen shaders from your copy of Lossless Scaling once. After that "
                     "they're built into Nitro, so you never have to find Lossless.dll again.",
                     nitro_ui::kMuted, inner * 0.92f);
        ImGui::Spacing();
        ImGui::PushFont(g_fonts.caption);
        CenteredText("You need Lossless Scaling from Steam. Nothing is downloaded or uploaded.", nitro_ui::kMuted,
                     inner);
        ImGui::PopFont();
        ImGui::SetCursorPosY(cardH - 30 * s - 50 * s);
        ImGui::PushFont(g_fonts.bold);
        if (nitro_ui::AccentButton("Get started", ImVec2(inner, 50 * s))) StartSetup(1);
        ImGui::PopFont();
    } else if (wiz.step == 1) {
        GradientText(g_fonts.title, "Find Lossless Scaling");
        ImGui::TextColored(nitro_ui::kMuted, "Nitro looked in every Steam library on this PC.");
        ImGui::Spacing();
        const float listH = 210 * s;
        ImGui::BeginChild("##found", ImVec2(inner, listH), ImGuiChildFlags_Borders);
        if (wiz.found.empty()) {
            ImGui::TextColored(nitro_ui::kWarn, "Lossless Scaling wasn't found automatically.");
            ImGui::PushTextWrapPos(0);
            ImGui::TextColored(nitro_ui::kMuted,
                               "Install it from Steam and press Scan again, or use Browse to point at Lossless.dll "
                               "in its folder.");
            ImGui::PopTextWrapPos();
        }
        for (size_t i = 0; i < wiz.found.size(); ++i) {
            const FoundInstall& f = wiz.found[i];
            ImGui::PushID(static_cast<int>(i));
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const float rowH = 58 * s;
            const bool sel = wiz.selected == static_cast<int>(i);
            if (ImGui::InvisibleButton("##row", ImVec2(ImGui::GetContentRegionAvail().x, rowH)) && f.ok)
                wiz.selected = static_cast<int>(i);
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImVec2 q(p.x + ImGui::GetItemRectSize().x, p.y + rowH);
            dl->AddRectFilled(p, q, sel ? IM_COL32(122, 92, 255, 60) : (ImGui::IsItemHovered() ? IM_COL32(42, 49, 94, 255) : IM_COL32(29, 34, 66, 255)), 10 * s);
            if (sel) dl->AddRect(p, q, nitro_ui::kViolet, 10 * s, 0, 1.5f * s);
            ImGui::SetCursorScreenPos(ImVec2(p.x + 14 * s, p.y + 8 * s));
            ImGui::BeginGroup();
            {
                // "Steam library D:\Games\Steam" reads better than the whole path to the DLL
                const std::wstring tail = L"\\steamapps\\common\\Lossless Scaling\\Lossless.dll";
                std::wstring label = f.path;
                if (label.size() > tail.size() &&
                    _wcsicmp(label.c_str() + label.size() - tail.size(), tail.c_str()) == 0)
                    label = L"Steam library  " + label.substr(0, label.size() - tail.size());
                ImGui::PushClipRect(p, ImVec2(q.x - 10 * s, q.y), true);
                ImGui::TextUnformatted(U(label).c_str());
                ImGui::PopClipRect();
            }
            ImGui::PushFont(g_fonts.caption);
            ImGui::TextColored(f.ok ? nitro_ui::kGood : nitro_ui::kBad, "%s", f.status.c_str());
            ImGui::PopFont();
            ImGui::EndGroup();
            ImGui::SetCursorScreenPos(ImVec2(p.x, q.y + 6 * s));
            ImGui::Dummy(ImVec2(0, 0));
            ImGui::PopID();
        }
        ImGui::EndChild();
        if (nitro_ui::GhostButton("Browse...", ImVec2(130 * s, 0))) BrowseForLossless();
        ImGui::SameLine();
        if (nitro_ui::GhostButton("Scan again", ImVec2(130 * s, 0))) ScanForLossless();
        if (!wiz.error.empty()) {
            ImGui::PushTextWrapPos(0);
            ImGui::TextColored(nitro_ui::kBad, "%s", wiz.error.c_str());
            ImGui::PopTextWrapPos();
        }
        ImGui::SetCursorPosY(cardH - 30 * s - 50 * s);
        const bool canImport = wiz.selected >= 0 && wiz.found[static_cast<size_t>(wiz.selected)].ok;
        const float backW = 120 * s;
        if (nitro_ui::GhostButton("Back", ImVec2(backW, 50 * s))) wiz.step = 0;
        ImGui::SameLine();
        ImGui::BeginDisabled(!canImport);
        ImGui::PushFont(g_fonts.bold);
        if (nitro_ui::AccentButton("Import shaders", ImVec2(ImGui::GetContentRegionAvail().x, 50 * s)) && canImport &&
            ImportShaders(wiz.found[static_cast<size_t>(wiz.selected)].path))
            wiz.step = 2;
        ImGui::PopFont();
        ImGui::EndDisabled();
    } else {
        ImGui::Dummy(ImVec2(0, 20 * s));
        const float r = 46 * s;
        const ImVec2 c(ImGui::GetCursorScreenPos().x + inner * 0.5f, ImGui::GetCursorScreenPos().y + r);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddCircleFilled(c, r, IM_COL32(77, 224, 168, 40));
        dl->AddCircle(c, r, IM_COL32(77, 224, 168, 255), 0, 3 * s);
        const ImVec2 tick[] = {{c.x - 20 * s, c.y + 1 * s}, {c.x - 6 * s, c.y + 15 * s}, {c.x + 22 * s, c.y - 14 * s}};
        dl->AddPolyline(tick, 3, IM_COL32(77, 224, 168, 255), 0, 5 * s);
        ImGui::Dummy(ImVec2(0, r * 2 + 12 * s));
        ImGui::PushFont(g_fonts.title);
        const char* title = "You're all set";
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (inner - ImGui::CalcTextSize(title).x) * 0.5f);
        ImGui::PopFont();
        GradientText(g_fonts.title, title);
        ImGui::Spacing();
        char line[128];
        snprintf(line, sizeof(line), "%zu frame-gen shaders are now built into Nitro.", wiz.imported);
        CenteredText(line, nitro_ui::kText, inner);
        CenteredText(wiz.performance ? "Quality and performance modes are both available."
                                     : "Quality mode is available (this Lossless Scaling has no performance mode).",
                     nitro_ui::kMuted, inner);
        ImGui::Spacing();
        ImGui::PushFont(g_fonts.caption);
        CenteredText("If Lossless Scaling updates, Nitro will tell you so you can re-import.", nitro_ui::kMuted, inner);
        ImGui::PopFont();
        ImGui::SetCursorPosY(cardH - 30 * s - 50 * s);
        ImGui::PushFont(g_fonts.bold);
        if (nitro_ui::AccentButton("Open Nitro", ImVec2(inner, 50 * s))) wiz.active = false;
        ImGui::PopFont();
    }
    ImGui::EndChild();
    ImGui::End();
}

void DrawUi() {
    const float s = g_scale;
    if (wiz.active) {
        DrawSetup();
        return;
    }
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(24 * s, 20 * s));
    ImGui::Begin("##main", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImGui::PopStyleVar(3);
    bool changed = false;

    // ---- Header
    {
        const float logo = 96 * s;
        nitro_ui::DrawLogo(ImGui::GetWindowDrawList(), ImGui::GetCursorScreenPos(), logo);
        ImGui::Dummy(ImVec2(logo, logo));
        ImGui::SameLine(0, 18 * s);
        ImGui::BeginGroup();
        GradientText(g_fonts.title, "NITRO");
        ImGui::TextColored(nitro_ui::kMuted, "Frame generation for any game \xC2\xB7 powered by your Lossless Scaling");
        ImGui::PushFont(g_fonts.caption);
        const char* chips[] = {"DirectX 9", "DirectX 10", "DirectX 11", "DirectX 12", "OpenGL", "Vulkan"};
        for (int i = 0; i < 6; ++i) {
            if (i) ImGui::SameLine(0, 6 * s);
            nitro_ui::Chip(chips[i], ImVec4(0.478f, 0.361f, 1.0f, 1.0f));
        }
        ImGui::PopFont();
        ImGui::EndGroup();
    }
    ImGui::Spacing();

    const float avail = ImGui::GetContentRegionAvail().x;
    const float gap = ImGui::GetStyle().ItemSpacing.x * 1.5f;

    // ---- Game card
    if (BeginCard("##game", ImVec2(avail, 0))) {
        CardTitle("Game");
        const float bw = 110 * s;
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - bw - ImGui::GetStyle().ItemSpacing.x);
        ImGui::InputTextWithHint("##exe", "Choose the game's .exe", ui.exe, sizeof(ui.exe));
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            AnalyzeGame();
            changed = true;
        }
        ImGui::SameLine();
        if (nitro_ui::GhostButton("Browse...", ImVec2(bw, 0))) Browse();

        const bool good = ui.exeInfo == "64-bit game";
        nitro_ui::Chip(ui.exeInfo.c_str(), good ? nitro_ui::kGood : nitro_ui::kMuted);
        if (!ui.detected.empty()) {
            ImGui::SameLine();
            nitro_ui::Chip(("Renderer: " + ui.detected).c_str(), ImVec4(0.0f, 0.898f, 1.0f, 1.0f));
        }
        if (!ui.exeHint.empty()) {
            ImGui::TextColored(nitro_ui::kWarn, "Unreal Engine launcher stub? The real game is probably:");
            ImGui::TextColored(nitro_ui::kMuted, "%s", ui.exeHint.c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("Use this exe")) {
                CopyTo(ui.exe, sizeof(ui.exe), ui.exeHint);
                AnalyzeGame();
                changed = true;
            }
        }
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
        ImGui::InputTextWithHint("##args", "Launch arguments (optional, for Launch with injection)", ui.args,
                                 sizeof(ui.args));
        if (ImGui::IsItemDeactivatedAfterEdit()) changed = true;
    }
    ImGui::EndChild();
    ImGui::Spacing();

    // ---- Two columns
    const float colW = (avail - gap) * 0.5f;
    const float colH = 400 * s;
    if (BeginCard("##fg", ImVec2(colW, colH))) {
        CardTitle("Frame generation");
        const float toggleW = ImGui::GetFrameHeight() * 0.85f * 1.85f;
        nitro_ui::RowLabel("Mode");
        {
            const float pw = 100 * s;
            nitro_ui::SameLineRight(pw * 2 + 8 * s);
            if (nitro_ui::Pill("Fixed", !ui.dynamic, ImVec2(pw, ImGui::GetFrameHeight()))) {
                ui.dynamic = false;
                changed = true;
            }
            ImGui::SameLine(0, 8 * s);
            if (nitro_ui::Pill("Dynamic", ui.dynamic, ImVec2(pw, ImGui::GetFrameHeight()))) {
                ui.dynamic = true;
                changed = true;
            }
        }
        nitro_ui::RowLabel(ui.dynamic ? "Up to" : "Multiplier");
        {
            const float pw = 64 * s;
            nitro_ui::SameLineRight(pw * 3 + 8 * s * 2);
            for (int m = 2; m <= 4; ++m) {
                char label[8];
                snprintf(label, sizeof(label), "%dx", m);
                if (nitro_ui::Pill(label, ui.multiplier == m, ImVec2(pw, ImGui::GetFrameHeight()))) {
                    ui.multiplier = m;
                    changed = true;
                }
                if (m < 4) ImGui::SameLine(0, 8 * s);
            }
        }
        nitro_ui::RowLabel("Flow scale");
        {
            const float sw = 200 * s;
            nitro_ui::SameLineRight(sw);
            ImGui::SetNextItemWidth(sw);
            ImGui::SliderInt("##flow", &ui.flowPct, 25, 100, "%d%%");
            if (ImGui::IsItemDeactivatedAfterEdit()) changed = true;
        }
        ImGui::BeginDisabled(!ui.perfAvailable);
        nitro_ui::RowLabel("Performance mode");
        nitro_ui::SameLineRight(toggleW);
        changed |= nitro_ui::Toggle("##perf", &ui.performance);
        ImGui::EndDisabled();
        nitro_ui::RowLabel("VSync pacing", "(recommended)");
        nitro_ui::SameLineRight(toggleW);
        changed |= nitro_ui::Toggle("##vsync", &ui.vsync);
        nitro_ui::RowLabel("HDR");
        {
            const float cw = 200 * s;
            nitro_ui::SameLineRight(cw);
            ImGui::SetNextItemWidth(cw);
            const char* items[] = {"Auto", "On", "Off"};
            changed |= ImGui::Combo("##hdr", &ui.hdr, items, 3);
        }
        nitro_ui::RowLabel("Graphics API");
        {
            const float cw = 200 * s;
            nitro_ui::SameLineRight(cw);
            ImGui::SetNextItemWidth(cw);
            const char* items[] = {"Auto-detect", "DirectX 10 / 11 / 12", "DirectX 9", "OpenGL", "Vulkan"};
            changed |= ImGui::Combo("##api", &ui.api, items, 5);
        }
        ImGui::PushFont(g_fonts.caption);
        ImGui::PushTextWrapPos(0);
        if (ui.dynamic)
            ImGui::TextColored(nitro_ui::kMuted,
                               "Dynamic fills your monitor's refresh rate: it generates only as many frames as needed, "
                               "up to the multiplier.");
        else
            ImGui::TextColored(nitro_ui::kMuted, "Lower flow scale = faster. Try 50-75%% at 1440p / 4K.");
        ImGui::PopTextWrapPos();
        ImGui::PopFont();
    }
    ImGui::EndChild();
    ImGui::SameLine(0, gap);
    ImGui::BeginGroup();
    const float lossH = 176 * s;
    if (BeginCard("##loss", ImVec2(colW, lossH))) {
        CardTitle("Frame-gen shaders");
        ImGui::SameLine();
        if (!g_losslessOk)
            nitro_ui::Chip("Not set up", nitro_ui::kBad);
        else if (ui.shadersOutdated)
            nitro_ui::Chip("Update available", nitro_ui::kWarn);
        else
            nitro_ui::Chip("Built in", nitro_ui::kGood);
        ImGui::PushTextWrapPos(0);
        if (ui.shadersOutdated)
            ImGui::TextColored(nitro_ui::kWarn, "Lossless Scaling was updated since the import. Re-import to use the new version.");
        else
            ImGui::TextColored(nitro_ui::kMuted, "%s", ui.lossStatus.c_str());
        ImGui::PopTextWrapPos();
        if (g_losslessOk ? nitro_ui::GhostButton("Re-import", ImVec2(110 * s, 0))
                         : nitro_ui::AccentButton("Run setup", ImVec2(110 * s, ImGui::GetFrameHeight())))
            StartSetup(1);
        ImGui::SameLine();
        if (nitro_ui::GhostButton("Open Nitro folder", ImVec2(170 * s, 0)))
            ShellExecuteW(nullptr, L"open", g_dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    ImGui::EndChild();
    if (BeginCard("##keys", ImVec2(colW, colH - lossH - ImGui::GetStyle().ItemSpacing.y))) {
        CardTitle("In game");
        KeyRow("Insert", "Open the Nitro menu");
        KeyRow("End", "Frame generation on / off");
        KeyRow("Home", "Cycle 2x / 3x / 4x");
        ImGui::PushFont(g_fonts.caption);
        ImGui::PushTextWrapPos(0);
        if (ui.dynamic)
            ImGui::TextColored(nitro_ui::kMuted,
                               "Dynamic mode needs no FPS cap: Nitro tops up whatever the game renders to your "
                               "monitor's refresh rate.");
        else
            ImGui::TextColored(nitro_ui::kMuted,
                               "Cap the game at refresh rate \xC3\xB7 multiplier for the smoothest result "
                               "(e.g. 60 FPS on 240 Hz at 4x).");
        ImGui::PopTextWrapPos();
        ImGui::PopFont();
    }
    ImGui::EndChild();
    ImGui::EndGroup();
    ImGui::Spacing();

    // ---- Actions
    {
        const float h = 46 * s;
        const float w3 = (avail - gap * 2) / 3.0f;
        ImGui::PushFont(g_fonts.bold);
        if (nitro_ui::AccentButton("Install into game##install", ImVec2(w3, h))) Install();
        ImGui::SameLine(0, gap);
        if (nitro_ui::GhostButton("Launch with injection", ImVec2(w3, h))) Launch();
        ImGui::SameLine(0, gap);
        if (nitro_ui::GhostButton("Remove from game", ImVec2(w3, h))) Uninstall();
        ImGui::PopFont();
    }
    ImGui::Spacing();

    // ---- Log
    const float footer = ImGui::GetTextLineHeightWithSpacing() + 4 * s;
    if (BeginCard("##log", ImVec2(avail, (std::max)(90 * s, ImGui::GetContentRegionAvail().y - footer)))) {
        ImGui::PushFont(g_fonts.caption);
        for (const auto& [color, line] : ui.log) {
            ImGui::PushTextWrapPos(0);
            ImGui::TextColored(color, "%s", line.c_str());
            ImGui::PopTextWrapPos();
        }
        if (ui.scrollLog) {
            ImGui::SetScrollHereY(1.0f);
            ui.scrollLog = false;
        }
        ImGui::PopFont();
    }
    ImGui::EndChild();
    ImGui::PushFont(g_fonts.caption);
    ImGui::TextColored(nitro_ui::kMuted,
                       "Single-player games only \xC2\xB7 Not affiliated with Lossless Scaling \xC2\xB7 Logs: Nitro.log "
                       "(game folder) or NitroVk.log (Nitro folder)");
    ImGui::PopFont();

    if (changed) SaveUi();
    ImGui::End();
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return TRUE;
    switch (msg) {
    case WM_SIZE:
        if (wp != SIZE_MINIMIZED) {
            g_resizeW = LOWORD(lp);
            g_resizeH = HIWORD(lp);
        }
        return 0;
    case WM_GETMINMAXINFO: {
        auto* mm = reinterpret_cast<MINMAXINFO*>(lp);
        mm->ptMinTrackSize.x = static_cast<LONG>(860 * g_scale);
        mm->ptMinTrackSize.y = static_cast<LONG>(860 * g_scale);
        return 0;
    }
    case WM_SYSCOMMAND:
        if ((wp & 0xfff0) == SC_KEYMENU) return 0; // no Alt menu beep
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int show) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    g_dir = ModuleDirectory(nullptr);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_CLASSDC;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXICON),
                                             GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR));
    wc.hIconSm = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                               GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
    wc.lpszClassName = L"NitroApp";
    RegisterClassExW(&wc);

    const UINT dpi = GetDpiForSystem();
    g_scale = static_cast<float>(dpi) / 96.0f;
    RECT rc{0, 0, static_cast<LONG>(920 * g_scale), static_cast<LONG>(940 * g_scale)};
    AdjustWindowRectExForDpi(&rc, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);
    g_wnd = CreateWindowExW(0, wc.lpszClassName, L"Nitro", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                            rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr, inst, nullptr);
    if (!g_wnd) return 1;

    // Dark title bar matching the app (Windows 10 2004+ / 11)
    const BOOL dark = TRUE;
    DwmSetWindowAttribute(g_wnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark));
    const COLORREF caption = RGB(14, 17, 36);
    DwmSetWindowAttribute(g_wnd, 35 /*DWMWA_CAPTION_COLOR*/, &caption, sizeof(caption));

    if (!CreateDeviceD3D(g_wnd)) {
        MessageBoxW(nullptr, L"Nitro needs DirectX 11 to show its window.", L"Nitro", MB_ICONERROR);
        return 1;
    }
    ShowWindow(g_wnd, show);
    UpdateWindow(g_wnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    g_fonts = nitro_ui::LoadFonts(17.0f * g_scale);
    nitro_ui::ApplyStyle(g_scale);
    ImGui_ImplWin32_Init(g_wnd);
    ImGui_ImplDX11_Init(g_device.Get(), g_context.Get());

    LoadUi();
    CheckLossless();
    AnalyzeGame();
    if (!g_losslessOk) {
        StartSetup(0); // first run: set up with Lossless Scaling
    } else if (ui.shadersOutdated) {
        Log(L"Lossless Scaling was updated - use Re-import to pick up the new shaders.", nitro_ui::kWarn);
    }
    Log(L"Ready. Pick a game, choose a multiplier, then Install.");

    bool running = true;
    while (running) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) running = false;
        }
        if (!running) break;
        if (IsIconic(g_wnd)) {
            Sleep(50);
            continue;
        }
        if (g_resizeW && g_resizeH) ResizeSwapChain();

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        DrawUi();
        ImGui::Render();
        const float clear[4] = {0.055f, 0.067f, 0.141f, 1.0f};
        ID3D11RenderTargetView* rtv = g_rtv.Get();
        g_context->OMSetRenderTargets(1, &rtv, nullptr);
        g_context->ClearRenderTargetView(rtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_swapChain->Present(1, 0);
    }

    SaveUi();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    return 0;
}

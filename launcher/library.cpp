#include "library.h"

#include <d3d11.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cctype>
#include <cwctype>
#include <fstream>
#include <sstream>

namespace nitro_lib {

using Microsoft::WRL::ComPtr;

namespace {

std::wstring Lower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), ::towlower);
    return s;
}

bool Exists(const std::wstring& p) {
    return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool IsDir(const std::wstring& p) {
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string ReadFile(const std::wstring& path) {
    std::ifstream f(path, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Value of "key" "value" in a Steam .acf / .vdf file.
std::string VdfValue(const std::string& text, const char* key) {
    const std::string needle = std::string("\"") + key + "\"";
    size_t k = text.find(needle);
    while (k != std::string::npos) {
        const size_t a = text.find('"', k + needle.size());
        const size_t nl = text.find('\n', k);
        if (a != std::string::npos && (nl == std::string::npos || a < nl)) {
            const size_t b = text.find('"', a + 1);
            if (b != std::string::npos) return text.substr(a + 1, b - a - 1);
        }
        k = text.find(needle, k + 1);
    }
    return {};
}

// Value of "key": "value" in a simple JSON file (Epic manifests).
std::string JsonString(const std::string& text, const char* key) {
    const std::string needle = std::string("\"") + key + "\"";
    const size_t k = text.find(needle);
    if (k == std::string::npos) return {};
    size_t a = text.find('"', text.find(':', k + needle.size()) + 1);
    if (a == std::string::npos) return {};
    std::string out;
    for (size_t i = a + 1; i < text.size() && text[i] != '"'; ++i) {
        if (text[i] == '\\' && i + 1 < text.size()) ++i;
        out.push_back(text[i]);
    }
    return out;
}

std::wstring RegString(HKEY key, const wchar_t* value) {
    wchar_t buf[1024] = {};
    DWORD size = sizeof(buf);
    if (RegGetValueW(key, nullptr, value, RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS) return {};
    return buf;
}

bool Skipped(const std::wstring& appId, const std::wstring& name) {
    static const wchar_t* ids[] = {L"228980", L"1070560", L"1391110", L"1628350", L"1493710", L"250820", L"993090"};
    for (const wchar_t* id : ids)
        if (appId == id) return true;
    const std::wstring n = Lower(name);
    for (const wchar_t* w : {L"redistributable", L"proton", L"steam linux runtime", L"steamworks"})
        if (n.find(w) != std::wstring::npos) return true;
    return false;
}

std::wstring FindSteamCover(const std::wstring& steamRoot, const std::wstring& appId) {
    if (steamRoot.empty() || appId.empty()) return {};
    const std::wstring cache = steamRoot + L"\\appcache\\librarycache\\";
    for (const wchar_t* f : {L"_library_600x900.jpg", L"_library_600x900_2x.jpg", L"_header.jpg"})
        if (Exists(cache + appId + f)) return cache + appId + f;
    // Newer Steam keeps them in per-app folders (sometimes one level deeper).
    const std::wstring dir = cache + appId;
    if (!IsDir(dir)) return {};
    std::wstring header;
    std::vector<std::wstring> stack{dir};
    for (int depth = 0; depth < 3 && !stack.empty(); ++depth) {
        std::vector<std::wstring> next;
        for (const std::wstring& d : stack) {
            WIN32_FIND_DATAW fd;
            HANDLE h = FindFirstFileW((d + L"\\*").c_str(), &fd);
            if (h == INVALID_HANDLE_VALUE) continue;
            do {
                const std::wstring name = fd.cFileName;
                if (name == L"." || name == L"..") continue;
                const std::wstring full = d + L"\\" + name;
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                    next.push_back(full);
                } else {
                    const std::wstring l = Lower(name);
                    if (l == L"library_600x900.jpg" || l == L"library_600x900_2x.jpg") {
                        FindClose(h);
                        return full;
                    }
                    if (l == L"header.jpg") header = full;
                }
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        stack.swap(next);
    }
    return header;
}

std::string Alnum(const std::wstring& s) {
    std::string out;
    for (wchar_t c : s)
        if (iswalnum(c) && c < 128) out.push_back(static_cast<char>(tolower(static_cast<int>(c))));
    return out;
}

} // namespace

const char* StoreName(Store s) {
    switch (s) {
    case Store::Steam: return "Steam";
    case Store::Epic: return "Epic";
    default: return "GOG";
    }
}

std::vector<Game> FindGames(const std::vector<std::wstring>& steamLibraries, const std::wstring& steamRoot) {
    std::vector<Game> games;
    auto seen = [&](const std::wstring& folder) {
        const std::wstring l = Lower(folder);
        return std::any_of(games.begin(), games.end(), [&](const Game& g) { return Lower(g.folder) == l; });
    };

    // Steam
    for (const std::wstring& lib : steamLibraries) {
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((lib + L"\\steamapps\\appmanifest_*.acf").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            const std::string text = ReadFile(lib + L"\\steamapps\\" + fd.cFileName);
            Game g;
            g.appId = Widen(VdfValue(text, "appid"));
            g.name = Widen(VdfValue(text, "name"));
            const std::wstring dir = Widen(VdfValue(text, "installdir"));
            if (g.name.empty() || dir.empty() || Skipped(g.appId, g.name)) continue;
            g.folder = lib + L"\\steamapps\\common\\" + dir;
            if (!IsDir(g.folder) || seen(g.folder)) continue;
            g.store = Store::Steam;
            g.cover = FindSteamCover(steamRoot, g.appId);
            games.push_back(std::move(g));
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }

    // Epic Games
    wchar_t programData[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"ProgramData", programData, MAX_PATH)) {
        const std::wstring manifests = std::wstring(programData) + L"\\Epic\\EpicGamesLauncher\\Data\\Manifests";
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((manifests + L"\\*.item").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                const std::string text = ReadFile(manifests + L"\\" + fd.cFileName);
                Game g;
                g.name = Widen(JsonString(text, "DisplayName"));
                g.folder = Widen(JsonString(text, "InstallLocation"));
                const std::wstring launch = Widen(JsonString(text, "LaunchExecutable"));
                std::replace(g.folder.begin(), g.folder.end(), L'/', L'\\');
                if (g.name.empty() || !IsDir(g.folder) || seen(g.folder)) continue;
                if (!launch.empty()) {
                    std::wstring exe = g.folder + L"\\" + launch;
                    std::replace(exe.begin(), exe.end(), L'/', L'\\');
                    if (Exists(exe)) g.exe = exe;
                }
                g.store = Store::Epic;
                games.push_back(std::move(g));
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
    }

    // GOG Galaxy
    HKEY gog = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\GOG.com\\Games", 0, KEY_READ, &gog) == ERROR_SUCCESS) {
        wchar_t sub[256];
        for (DWORD i = 0;; ++i) {
            DWORD len = 256;
            if (RegEnumKeyExW(gog, i, sub, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
            HKEY k = nullptr;
            if (RegOpenKeyExW(gog, sub, 0, KEY_READ, &k) != ERROR_SUCCESS) continue;
            Game g;
            g.name = RegString(k, L"gameName");
            g.folder = RegString(k, L"path");
            const std::wstring exe = RegString(k, L"exe");
            RegCloseKey(k);
            if (g.name.empty() || !IsDir(g.folder) || seen(g.folder)) continue;
            if (Exists(exe)) g.exe = exe;
            g.store = Store::Gog;
            games.push_back(std::move(g));
        }
        RegCloseKey(gog);
    }

    std::sort(games.begin(), games.end(), [](const Game& a, const Game& b) { return Lower(a.name) < Lower(b.name); });
    return games;
}

std::wstring GuessMainExe(const std::wstring& folder, const std::wstring& name) {
    static const wchar_t* skip[] = {L"unins",     L"setup",        L"install",   L"redist",   L"vc_redist",
                                    L"vcredist",  L"dxsetup",      L"crash",     L"report",   L"easyanticheat",
                                    L"beservice", L"battleye",     L"prereq",    L"dotnet",   L"cefprocess",
                                    L"unitycrashhandler", L"pbsvc", L"touchup",  L"cleanup",  L"quicksfv",
                                    L"subprocess", L"webhelper",   L"vrserver",  L"7z",       L"python"};
    static const wchar_t* penalise[] = {L"launcher", L"server", L"editor", L"config", L"settings", L"benchmark",
                                        L"update", L"patch", L"helper", L"tool"};
    const std::string wantA = Alnum(name);
    const std::string wantB = Alnum(folder.substr(folder.find_last_of(L'\\') + 1));

    std::wstring best;
    double bestScore = -1e9;
    struct Dir {
        std::wstring path;
        int depth;
    };
    std::vector<Dir> stack{{folder, 0}};
    int files = 0;
    while (!stack.empty() && files < 600) {
        const Dir d = stack.back();
        stack.pop_back();
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((d.path + L"\\*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            const std::wstring fname = fd.cFileName;
            if (fname == L"." || fname == L"..") continue;
            const std::wstring full = d.path + L"\\" + fname;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (d.depth < 4) stack.push_back({full, d.depth + 1});
                continue;
            }
            ++files;
            const std::wstring l = Lower(fname);
            if (l.size() < 5 || l.substr(l.size() - 4) != L".exe") continue;
            if (std::any_of(std::begin(skip), std::end(skip),
                            [&](const wchar_t* s) { return l.find(s) != std::wstring::npos; }))
                continue;
            double score = 0;
            if (l.size() > 13 && l.substr(l.size() - 13) == L"-shipping.exe") score += 100;
            const std::wstring lp = Lower(d.path);
            if (lp.find(L"\\binaries\\win64") != std::wstring::npos) score += 30;
            if (lp.find(L"\\bin\\x64") != std::wstring::npos || lp.find(L"\\x64") != std::wstring::npos) score += 10;
            const std::string stem = Alnum(fname.substr(0, fname.size() - 4));
            if (!stem.empty() && (wantA.find(stem) != std::string::npos || stem.find(wantA.substr(0, 6)) == 0 ||
                                  wantB.find(stem) != std::string::npos))
                score += 35;
            for (const wchar_t* p : penalise)
                if (l.find(p) != std::wstring::npos) score -= 40;
            const double mb = static_cast<double>((static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow) /
                              (1024.0 * 1024.0);
            score += (std::min)(mb * 0.5, 40.0);
            score -= d.depth * 3.0;
            if (score > bestScore) {
                bestScore = score;
                best = full;
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return best;
}

bool HasAntiCheat(const std::wstring& folder, const std::wstring& exe) {
    static const wchar_t* markers[] = {L"easyanticheat", L"easyanticheat_eos", L"battleye", L"beservice",
                                       L"start_protected_game", L"eaanticheat", L"vgc", L"anticheatexpert",
                                       L"xigncode", L"nprotect", L"gameguard"};
    std::vector<std::wstring> dirs{folder};
    if (!exe.empty()) dirs.push_back(exe.substr(0, exe.find_last_of(L'\\')));
    for (int depth = 0; depth < 2; ++depth) {
        std::vector<std::wstring> next;
        for (const std::wstring& d : dirs) {
            WIN32_FIND_DATAW fd;
            HANDLE h = FindFirstFileW((d + L"\\*").c_str(), &fd);
            if (h == INVALID_HANDLE_VALUE) continue;
            do {
                const std::wstring l = Lower(fd.cFileName);
                if (l == L"." || l == L"..") continue;
                for (const wchar_t* m : markers)
                    if (l.rfind(m, 0) == 0) {
                        FindClose(h);
                        return true;
                    }
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) next.push_back(d + L"\\" + fd.cFileName);
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        dirs.swap(next);
    }
    return false;
}

ID3D11ShaderResourceView* LoadCover(ID3D11Device* device, const std::wstring& path, UINT maxWidth) {
    static ComPtr<IWICImagingFactory> factory;
    if (!factory && FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                            IID_PPV_ARGS(&factory))))
        return nullptr;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                  WICDecodeMetadataCacheOnDemand, &decoder)) ||
        FAILED(decoder->GetFrame(0, &frame)))
        return nullptr;
    UINT w = 0, h = 0;
    frame->GetSize(&w, &h);
    if (!w || !h) return nullptr;
    const UINT tw = (std::min)(w, maxWidth);
    const UINT th = (std::max)(1u, static_cast<UINT>(static_cast<uint64_t>(h) * tw / w));
    ComPtr<IWICBitmapScaler> scaler;
    ComPtr<IWICFormatConverter> conv;
    if (FAILED(factory->CreateBitmapScaler(&scaler)) ||
        FAILED(scaler->Initialize(frame.Get(), tw, th, WICBitmapInterpolationModeFant)) ||
        FAILED(factory->CreateFormatConverter(&conv)) ||
        FAILED(conv->Initialize(scaler.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                WICBitmapPaletteTypeCustom)))
        return nullptr;
    std::vector<uint8_t> px(static_cast<size_t>(tw) * th * 4);
    if (FAILED(conv->CopyPixels(nullptr, tw * 4, static_cast<UINT>(px.size()), px.data()))) return nullptr;

    D3D11_TEXTURE2D_DESC d{};
    d.Width = tw;
    d.Height = th;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_IMMUTABLE;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init{px.data(), tw * 4, 0};
    ComPtr<ID3D11Texture2D> tex;
    ID3D11ShaderResourceView* srv = nullptr;
    if (FAILED(device->CreateTexture2D(&d, &init, &tex)) ||
        FAILED(device->CreateShaderResourceView(tex.Get(), nullptr, &srv)))
        return nullptr;
    return srv;
}

} // namespace nitro_lib

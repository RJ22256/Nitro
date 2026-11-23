// Finds installed games (Steam, Epic Games, GOG) for Nitro's game library.
#pragma once

#include <windows.h>

#include <string>
#include <vector>

struct ID3D11Device;
struct ID3D11ShaderResourceView;

namespace nitro_lib {

enum class Store { Steam, Epic, Gog };

struct Game {
    std::wstring name;
    std::wstring folder;
    std::wstring exe;   // best guess at the game's main exe (may be empty until analysed)
    std::wstring cover; // cover art image (Steam only)
    std::wstring appId;
    Store store = Store::Steam;

    // Filled in by the background analysis
    bool analysed = false;
    bool antiCheat = false;
    bool x86 = false;
    std::string api;    // "DirectX 12", "Vulkan", ...
    std::string status; // "Installed", "Vulkan on", or empty

    // Cover texture (created on the UI thread)
    ID3D11ShaderResourceView* coverSrv = nullptr;
    bool coverTried = false;
};

const char* StoreName(Store s);

// Quick discovery from store manifests (no file system crawling).
std::vector<Game> FindGames(const std::vector<std::wstring>& steamLibraries, const std::wstring& steamRoot);

// Slower per-game work, safe to run on a background thread.
std::wstring GuessMainExe(const std::wstring& folder, const std::wstring& name);
bool HasAntiCheat(const std::wstring& folder, const std::wstring& exe);

// Decodes an image (JPG/PNG) into a small D3D11 texture. Call on the UI thread.
ID3D11ShaderResourceView* LoadCover(ID3D11Device* device, const std::wstring& path, UINT maxWidth);

} // namespace nitro_lib

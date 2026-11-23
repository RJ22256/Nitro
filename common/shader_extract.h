// Extracts the Lossless Scaling Frame Generation (LSFG 3.1) compute shaders
// from the user's own copy of Lossless.dll. Nothing from Lossless Scaling is
// shipped with this project.
#pragma once

#include <windows.h>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace lsfgi {

using ShaderBlobs = std::map<std::string, std::vector<uint8_t>>;

enum class BlobKind { DxbcSm5, DxbcDxil, Spirv, Other };

BlobKind ClassifyBlob(const uint8_t* data, size_t size);
const wchar_t* BlobKindName(BlobKind kind);

struct DllReport {
    int rcdataCount = 0;
    int dxbc = 0;
    int dxil = 0;
    int spirv = 0;
    int other = 0;
    int detectedOffset = -1;   // -1: no complete Shader Model 5 set was found
    bool performanceSet = false; // performance-mode shaders are present too
};

// Looks through Lossless.dll for a complete set of DXBC frame-gen shaders.
bool InspectLosslessDll(const std::wstring& path, DllReport& report, std::wstring& error);

// Loads every shader the frame generator needs, either from Lossless.dll or
// from a Nitro shader pack (see below). offset < 0 means auto-detect.
// Performance-mode shaders are loaded when present ("p_" prefix).
bool LoadLsfgShaders(const std::wstring& path, int offset, ShaderBlobs& out,
                     int& usedOffset, std::wstring& error);

// ---------------------------------------------------------------------------
// Shader pack: the shaders imported once from the user's Lossless.dll into a
// file next to Nitro.exe, so Lossless.dll never has to be found again.
// Created on the user's own PC only; never distribute it.

struct PackInfo {
    std::wstring source;       // Lossless.dll it was imported from
    uint64_t sourceSize = 0;
    uint64_t sourceTime = 0;   // last-write FILETIME
    uint64_t importedAt = 0;   // FILETIME
    size_t shaderCount = 0;
    bool performanceSet = false;
};

constexpr wchar_t kShaderPackName[] = L"Nitro.shaders";

bool IsShaderPack(const std::wstring& path);
bool SaveShaderPack(const std::wstring& path, const ShaderBlobs& blobs, const std::wstring& source,
                    std::wstring& error);
bool LoadShaderPack(const std::wstring& path, ShaderBlobs* blobs, PackInfo& info, std::wstring& error);
// True if the Lossless.dll the pack came from still exists and has changed.
bool PackSourceChanged(const PackInfo& info);

} // namespace lsfgi

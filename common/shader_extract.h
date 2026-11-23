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

// Loads every shader the frame generator needs. offset < 0 means auto-detect.
// Performance-mode shaders are loaded when present ("p_" prefix).
bool LoadLsfgShaders(const std::wstring& path, int offset, ShaderBlobs& out,
                     int& usedOffset, std::wstring& error);

} // namespace lsfgi

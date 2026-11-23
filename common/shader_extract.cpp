#include "shader_extract.h"

#include <cstring>

namespace lsfgi {

namespace {

struct ShaderId {
    const char* name;
    int id;
    bool performance;
};

// RT_RCDATA resource IDs of the Shader Model 5 (DXBC) LSFG 3.1 shaders in
// Lossless.dll. Mapping from lsfg-vk (MIT licence), from before it switched to
// the SPIR-V copies of the same shaders.
constexpr ShaderId kShaderIds[] = {
    {"mipmaps", 255, false},
    {"generate", 256, false},
    {"alpha[0]", 267, false}, {"alpha[1]", 268, false},
    {"alpha[2]", 269, false}, {"alpha[3]", 270, false},
    {"beta[0]", 275, false}, {"beta[1]", 276, false}, {"beta[2]", 277, false},
    {"beta[3]", 278, false}, {"beta[4]", 279, false},
    {"gamma[0]", 257, false}, {"gamma[1]", 259, false}, {"gamma[2]", 260, false},
    {"gamma[3]", 261, false}, {"gamma[4]", 262, false},
    {"delta[0]", 257, false}, {"delta[1]", 263, false}, {"delta[2]", 264, false},
    {"delta[3]", 265, false}, {"delta[4]", 266, false}, {"delta[5]", 258, false},
    {"delta[6]", 271, false}, {"delta[7]", 272, false}, {"delta[8]", 273, false},
    {"delta[9]", 274, false},

    {"p_alpha[0]", 290, true}, {"p_alpha[1]", 291, true},
    {"p_alpha[2]", 292, true}, {"p_alpha[3]", 293, true},
    {"p_beta[0]", 298, true}, {"p_beta[1]", 299, true}, {"p_beta[2]", 300, true},
    {"p_beta[3]", 301, true}, {"p_beta[4]", 302, true},
    {"p_gamma[0]", 280, true}, {"p_gamma[1]", 282, true}, {"p_gamma[2]", 283, true},
    {"p_gamma[3]", 284, true}, {"p_gamma[4]", 285, true},
    {"p_delta[0]", 280, true}, {"p_delta[1]", 286, true}, {"p_delta[2]", 287, true},
    {"p_delta[3]", 288, true}, {"p_delta[4]", 289, true}, {"p_delta[5]", 281, true},
    {"p_delta[6]", 294, true}, {"p_delta[7]", 295, true}, {"p_delta[8]", 296, true},
    {"p_delta[9]", 297, true},
};

// Lossless.dll stores several copies of the shader set in blocks of 49 IDs.
// Try the known block positions until one is all DXBC/SM5.
constexpr int kCandidateOffsets[] = {0, 49, 98, 147, 196, 245};

const LPCWSTR kRcData = MAKEINTRESOURCEW(10); // RT_RCDATA

struct DataModule {
    HMODULE handle = nullptr;
    explicit DataModule(const std::wstring& path) {
        handle = LoadLibraryExW(path.c_str(), nullptr,
                                LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    }
    ~DataModule() {
        if (handle) FreeLibrary(handle);
    }
    DataModule(const DataModule&) = delete;
    DataModule& operator=(const DataModule&) = delete;
};

bool GetResource(HMODULE module, int id, const uint8_t*& data, size_t& size) {
    HRSRC res = FindResourceW(module, MAKEINTRESOURCEW(id), kRcData);
    if (!res) return false;
    HGLOBAL loaded = LoadResource(module, res);
    if (!loaded) return false;
    data = static_cast<const uint8_t*>(LockResource(loaded));
    size = SizeofResource(module, res);
    return data != nullptr && size > 0;
}

bool IsSm5(HMODULE module, int id) {
    const uint8_t* data = nullptr;
    size_t size = 0;
    if (!GetResource(module, id, data, size)) return false;
    return ClassifyBlob(data, size) == BlobKind::DxbcSm5;
}

bool SetComplete(HMODULE module, int offset, bool performance) {
    for (const auto& s : kShaderIds) {
        if (s.performance != performance) continue;
        if (!IsSm5(module, s.id + offset)) return false;
    }
    return true;
}

int DetectOffset(HMODULE module) {
    for (int offset : kCandidateOffsets)
        if (SetComplete(module, offset, false)) return offset;
    return -1;
}

uint32_t ReadU32(const uint8_t* p) {
    uint32_t v;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

} // namespace

BlobKind ClassifyBlob(const uint8_t* data, size_t size) {
    if (size >= 4 && ReadU32(data) == 0x07230203u) return BlobKind::Spirv;
    if (size < 32 || std::memcmp(data, "DXBC", 4) != 0) return BlobKind::Other;

    const uint32_t chunkCount = ReadU32(data + 28);
    if (32ull + 4ull * chunkCount > size) return BlobKind::Other;

    bool dxil = false;
    for (uint32_t i = 0; i < chunkCount; ++i) {
        const uint32_t offset = ReadU32(data + 32 + 4 * i);
        if (static_cast<size_t>(offset) + 4 > size) continue;
        const uint8_t* fourcc = data + offset;
        if (std::memcmp(fourcc, "SHEX", 4) == 0 || std::memcmp(fourcc, "SHDR", 4) == 0)
            return BlobKind::DxbcSm5;
        if (std::memcmp(fourcc, "DXIL", 4) == 0) dxil = true;
    }
    return dxil ? BlobKind::DxbcDxil : BlobKind::Other;
}

const wchar_t* BlobKindName(BlobKind kind) {
    switch (kind) {
    case BlobKind::DxbcSm5: return L"DXBC (SM5)";
    case BlobKind::DxbcDxil: return L"DXIL";
    case BlobKind::Spirv: return L"SPIR-V";
    default: return L"other";
    }
}

bool InspectLosslessDll(const std::wstring& path, DllReport& report, std::wstring& error) {
    report = DllReport{};
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        error = L"Lossless.dll not found";
        return false;
    }
    DataModule module(path);
    if (!module.handle) {
        error = L"Could not open Lossless.dll (error " + std::to_wstring(GetLastError()) + L")";
        return false;
    }

    struct Ctx {
        DllReport* report;
    } ctx{&report};

    EnumResourceNamesW(
        module.handle, kRcData,
        [](HMODULE mod, LPCWSTR, LPWSTR name, LONG_PTR param) -> BOOL {
            auto* c = reinterpret_cast<Ctx*>(param);
            c->report->rcdataCount++;
            HRSRC res = FindResourceW(mod, name, MAKEINTRESOURCEW(10));
            if (!res) return TRUE;
            HGLOBAL loaded = LoadResource(mod, res);
            const auto* data = loaded ? static_cast<const uint8_t*>(LockResource(loaded)) : nullptr;
            const size_t size = SizeofResource(mod, res);
            if (!data) return TRUE;
            switch (ClassifyBlob(data, size)) {
            case BlobKind::DxbcSm5: c->report->dxbc++; break;
            case BlobKind::DxbcDxil: c->report->dxil++; break;
            case BlobKind::Spirv: c->report->spirv++; break;
            default: c->report->other++; break;
            }
            return TRUE;
        },
        reinterpret_cast<LONG_PTR>(&ctx));

    report.detectedOffset = DetectOffset(module.handle);
    if (report.detectedOffset < 0) {
        error = L"No complete set of DirectX 11 frame-gen shaders found. "
                L"Is this Lossless.dll from an up-to-date Lossless Scaling install?";
        return false;
    }
    report.performanceSet = SetComplete(module.handle, report.detectedOffset, true);
    return true;
}

bool LoadLsfgShaders(const std::wstring& path, int offset, ShaderBlobs& out,
                     int& usedOffset, std::wstring& error) {
    out.clear();
    DataModule module(path);
    if (!module.handle) {
        error = L"Could not open " + path + L" (error " + std::to_wstring(GetLastError()) + L")";
        return false;
    }

    if (offset < 0) offset = DetectOffset(module.handle);
    if (offset < 0 || !SetComplete(module.handle, offset, false)) {
        error = L"Lossless.dll does not contain the expected DXBC frame-gen shaders";
        return false;
    }
    usedOffset = offset;
    const bool perf = SetComplete(module.handle, offset, true);

    for (const auto& s : kShaderIds) {
        if (s.performance && !perf) continue;
        const uint8_t* data = nullptr;
        size_t size = 0;
        if (!GetResource(module.handle, s.id + offset, data, size)) {
            error = L"Missing shader resource " + std::to_wstring(s.id + offset);
            return false;
        }
        out[s.name] = std::vector<uint8_t>(data, data + size);
    }
    return true;
}

} // namespace lsfgi

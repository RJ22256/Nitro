#include "shader_extract.h"

#include <cstring>
#include <fstream>

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

struct InspectCtx {
    DllReport* report;
};

BOOL CALLBACK CountResource(HMODULE mod, LPCWSTR, LPWSTR name, LONG_PTR param) {
    auto* c = reinterpret_cast<InspectCtx*>(param);
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

    InspectCtx ctx{&report};

    EnumResourceNamesW(module.handle, kRcData, &CountResource, reinterpret_cast<LONG_PTR>(&ctx));

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
    if (IsShaderPack(path)) {
        PackInfo info;
        usedOffset = -1;
        return LoadShaderPack(path, &out, info, error);
    }
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

// ---------------------------------------------------------------------------
// Shader pack
//
// "NITROSH1" | u32 version | u16 n, wchar source[n] | u64 size | u64 time |
// u64 importedAt | u32 count | count x (u16 n, char name[n], u32 n, data[n])

namespace {

constexpr char kPackMagic[8] = {'N', 'I', 'T', 'R', 'O', 'S', 'H', '1'};
constexpr uint32_t kPackVersion = 1;

bool FileStamp(const std::wstring& path, uint64_t& size, uint64_t& time) {
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a)) return false;
    size = (static_cast<uint64_t>(a.nFileSizeHigh) << 32) | a.nFileSizeLow;
    time = (static_cast<uint64_t>(a.ftLastWriteTime.dwHighDateTime) << 32) | a.ftLastWriteTime.dwLowDateTime;
    return true;
}

template <typename T>
void Put(std::ofstream& f, T v) {
    f.write(reinterpret_cast<const char*>(&v), sizeof(v));
}

template <typename T>
bool Get(std::ifstream& f, T& v) {
    return static_cast<bool>(f.read(reinterpret_cast<char*>(&v), sizeof(v)));
}

} // namespace

bool IsShaderPack(const std::wstring& path) {
    std::ifstream f(path, std::ios::binary);
    char magic[8] = {};
    return f.read(magic, 8) && std::memcmp(magic, kPackMagic, 8) == 0;
}

bool SaveShaderPack(const std::wstring& path, const ShaderBlobs& blobs, const std::wstring& source,
                    std::wstring& error) {
    const std::wstring tmp = path + L".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) {
            error = L"Could not write " + path;
            return false;
        }
        uint64_t size = 0, time = 0;
        FileStamp(source, size, time);
        FILETIME now{};
        GetSystemTimeAsFileTime(&now);
        f.write(kPackMagic, 8);
        Put(f, kPackVersion);
        Put(f, static_cast<uint16_t>(source.size()));
        f.write(reinterpret_cast<const char*>(source.data()), static_cast<std::streamsize>(source.size() * 2));
        Put(f, size);
        Put(f, time);
        Put(f, (static_cast<uint64_t>(now.dwHighDateTime) << 32) | now.dwLowDateTime);
        Put(f, static_cast<uint32_t>(blobs.size()));
        for (const auto& [name, data] : blobs) {
            Put(f, static_cast<uint16_t>(name.size()));
            f.write(name.data(), static_cast<std::streamsize>(name.size()));
            Put(f, static_cast<uint32_t>(data.size()));
            f.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        }
        if (!f) {
            error = L"Could not write " + path;
            return false;
        }
    }
    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(tmp.c_str());
        error = L"Could not replace " + path;
        return false;
    }
    return true;
}

bool LoadShaderPack(const std::wstring& path, ShaderBlobs* blobs, PackInfo& info, std::wstring& error) {
    info = PackInfo{};
    std::ifstream f(path, std::ios::binary);
    char magic[8] = {};
    uint32_t version = 0;
    if (!f || !f.read(magic, 8) || std::memcmp(magic, kPackMagic, 8) != 0 || !Get(f, version) ||
        version != kPackVersion) {
        error = L"Not a Nitro shader pack (or from a different Nitro version)";
        return false;
    }
    uint16_t n = 0;
    if (!Get(f, n)) return error = L"Shader pack is damaged", false;
    info.source.resize(n);
    f.read(reinterpret_cast<char*>(info.source.data()), static_cast<std::streamsize>(n) * 2);
    uint32_t count = 0;
    if (!Get(f, info.sourceSize) || !Get(f, info.sourceTime) || !Get(f, info.importedAt) || !Get(f, count) ||
        count > 1024)
        return error = L"Shader pack is damaged", false;

    ShaderBlobs local;
    for (uint32_t i = 0; i < count; ++i) {
        uint16_t nameLen = 0;
        uint32_t size = 0;
        if (!Get(f, nameLen)) return error = L"Shader pack is damaged", false;
        std::string name(nameLen, '\0');
        f.read(name.data(), nameLen);
        if (!Get(f, size) || size > (64u << 20)) return error = L"Shader pack is damaged", false;
        std::vector<uint8_t> data(size);
        if (!f.read(reinterpret_cast<char*>(data.data()), size)) return error = L"Shader pack is damaged", false;
        if (ClassifyBlob(data.data(), data.size()) != BlobKind::DxbcSm5)
            return error = L"Shader pack contains an unexpected shader", false;
        local[name] = std::move(data);
    }
    // Everything the quality pipeline needs must be present.
    for (const auto& s : kShaderIds)
        if (!s.performance && local.find(s.name) == local.end())
            return error = L"Shader pack is incomplete - re-import from Lossless Scaling", false;

    info.shaderCount = local.size();
    info.performanceSet = local.find("p_alpha[0]") != local.end();
    if (blobs) *blobs = std::move(local);
    return true;
}

bool PackSourceChanged(const PackInfo& info) {
    uint64_t size = 0, time = 0;
    if (info.source.empty() || !FileStamp(info.source, size, time)) return false;
    return size != info.sourceSize || time != info.sourceTime;
}

} // namespace lsfgi

// D3D11 port of the LSFG 3.1 frame-generation pipeline.
//
// The pass structure (which shader runs with which inputs/outputs, image sizes,
// constant-buffer contents) follows lsfg-vk's Vulkan implementation (MIT).
// The shaders are Lossless Scaling's own DXBC compute shaders, read from the
// user's Lossless.dll at runtime, so they run on D3D11 without translation.
#pragma once

#include <d3d11_1.h>
#include <wrl/client.h>

#include <array>
#include <cstdint>
#include <initializer_list>
#include <map>
#include <string>
#include <vector>

#include "../common/shader_extract.h"

namespace lsfgi {

using Microsoft::WRL::ComPtr;

struct Tex {
    ComPtr<ID3D11Texture2D> tex;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11UnorderedAccessView> uav;
    UINT w = 0;
    UINT h = 0;
};

struct FrameGenParams {
    UINT width = 0;
    UINT height = 0;
    DXGI_FORMAT backbufferFormat = DXGI_FORMAT_UNKNOWN;
    int multiplier = 2;     // total frames per real frame (2..4)
    float flowScale = 1.0f; // 0.25..1.0
    bool performance = false;
    bool hdr = false;
};

bool IsSupportedBackbufferFormat(DXGI_FORMAT f);
bool IsSrgbFormat(DXGI_FORMAT f);
DXGI_FORMAT OutputFormatFor(DXGI_FORMAT backbuffer);
bool IsHdrFormat(DXGI_FORMAT f);
// Typed, non-sRGB view of a back buffer format (what Nitro's textures use).
DXGI_FORMAT ViewFormat(DXGI_FORMAT f);
// Typeless family of a back buffer format (for copies between views).
DXGI_FORMAT TypelessFormat(DXGI_FORMAT f);

class FrameGen {
public:
    FrameGen() = default;
    FrameGen(const FrameGen&) = delete;
    FrameGen& operator=(const FrameGen&) = delete;

    bool Init(ID3D11Device* device, const FrameGenParams& params, const ShaderBlobs& shaders,
              std::string& error);

    // Copies the finished real frame out of the back buffer and runs the
    // pipeline. Generates `gens` frames at times (i+1)/denom between the
    // previous and this real frame (gens < 0: the full multiplier). Returns how
    // many generated frames are ready (0 while warming up).
    int Process(ID3D11DeviceContext* ctx, ID3D11Resource* backbuffer, int gens = -1, int denom = 0);
    int MaxGenerated() const { return gens_; }

    const Tex& Generated(int i) const { return outs_[static_cast<size_t>(i)]; }
    // Copy of the real frame captured by the last Process() call.
    ID3D11Texture2D* CurrentFrame() const { return frames_[(frameIdx_ + 1) % 2].tex.Get(); }
    DXGI_FORMAT OutputFormat() const { return outFormat_; }
    const FrameGenParams& Params() const { return p_; }
    void Reset() { frameIdx_ = 0; }

private:
    using List = std::vector<const Tex*>;

    struct Alpha {
        const Tex* in = nullptr;
        std::vector<Tex> t1, t2, t3;
        std::array<std::vector<Tex>, 3> out;
    };
    struct Gamma {
        const Tex* in2 = nullptr;
        const Tex* opt = nullptr;
        std::vector<Tex> t1, t2;
        Tex out;
    };
    struct Delta {
        const Tex* in2 = nullptr;
        const Tex* opt1 = nullptr;
        const Tex* opt2 = nullptr;
        const Tex* opt3 = nullptr;
        std::vector<Tex> t1, t2;
        Tex out1, out2;
    };

    bool MakeTex(Tex& t, UINT w, UINT h, DXGI_FORMAT viewFormat, bool uav,
                 DXGI_FORMAT resourceFormat = DXGI_FORMAT_UNKNOWN);
    bool MakeTexArray(std::vector<Tex>& v, size_t count, UINT w, UINT h, DXGI_FORMAT fmt);
    ComPtr<ID3D11Buffer> MakeConstants(float timestamp, bool firstIter, bool firstIterS);
    ID3D11ComputeShader* S(const char* name) const;
    ID3D11Buffer* Cb(int pass, int kind) const;

    void Run(ID3D11DeviceContext* c, ID3D11ComputeShader* cs, ID3D11Buffer* cb,
             std::initializer_list<ID3D11SamplerState*> samplers, const List& srvs,
             const List& uavs, UINT gx, UINT gy);

    void RunMipmaps(ID3D11DeviceContext* c, uint64_t fc);
    void RunAlpha(ID3D11DeviceContext* c, Alpha& a, uint64_t fc);
    void RunBeta(ID3D11DeviceContext* c, uint64_t fc);
    void RunGamma(ID3D11DeviceContext* c, int i, int pass, uint64_t fc);
    void RunDelta(ID3D11DeviceContext* c, int j, int pass, uint64_t fc);
    void RunGenerate(ID3D11DeviceContext* c, int pass, uint64_t fc);

    ComPtr<ID3D11Device> dev_;
    FrameGenParams p_;
    DXGI_FORMAT outFormat_ = DXGI_FORMAT_R8G8B8A8_UNORM;
    int gens_ = 1;
    size_t C_ = 4;  // feature channels: 4 (quality) / 2 (performance)
    size_t A1_ = 2; // alpha half-res temporaries
    size_t T1_ = 4; // gamma/delta temp1 count
    size_t D_ = 2;  // delta second-half temporaries

    std::map<std::string, ComPtr<ID3D11ComputeShader>> cs_;
    ComPtr<ID3D11SamplerState> sBlack_, sWhite_, sClamp_;
    ComPtr<ID3D11Buffer> cbDefault_, cbBeta_;
    // [denominator 2..4][pass][plain, firstIter, firstIterS]; timestamp = (pass+1)/denominator
    std::array<std::array<std::array<ComPtr<ID3D11Buffer>, 3>, 3>, 3> cbPass_;
    int denom_ = 2;

    std::array<Tex, 2> frames_;
    std::array<Tex, 7> mips_;
    std::array<Alpha, 7> alpha_;
    std::vector<Tex> bt1_, bt2_, bout_;
    std::array<Gamma, 7> gamma_;
    std::array<Delta, 3> delta_;
    std::vector<Tex> outs_;

    uint64_t frameIdx_ = 0;
};

} // namespace lsfgi

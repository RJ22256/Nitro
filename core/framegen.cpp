#include "framegen.h"

#include <algorithm>
#include <cstring>

namespace lsfgi {

namespace {

// Matches the constant buffer the LSFG shaders read (register b0).
struct Constants {
    UINT inputOffset[2];
    UINT firstIter;
    UINT firstIterS;
    UINT advancedColorKind;
    UINT hdrSupport;
    float resolutionInvScale;
    float timestamp;
    float uiThreshold;
    UINT pad[3];
};
static_assert(sizeof(Constants) == 48, "constant buffer layout");

UINT Half(UINT v) { return std::max(1u, (v + 1) >> 1); }
UINT Shr(UINT v, int s) { return std::max(1u, v >> s); }
UINT Groups(UINT v, int shift) { return (v + (1u << shift) - 1) >> shift; }

} // namespace

DXGI_FORMAT TypelessFormat(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_TYPELESS;
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_TYPELESS;
    case DXGI_FORMAT_B8G8R8X8_UNORM:
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8X8_TYPELESS;
    case DXGI_FORMAT_R10G10B10A2_UNORM: return DXGI_FORMAT_R10G10B10A2_TYPELESS;
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return DXGI_FORMAT_R16G16B16A16_TYPELESS;
    default: return f;
    }
}

// Raw (non-sRGB) view so the shaders see the same encoded values a screen
// capture would.
DXGI_FORMAT ViewFormat(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_B8G8R8X8_UNORM:
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8X8_UNORM;
    default: return f;
    }
}

bool IsSupportedBackbufferFormat(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8X8_UNORM:
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return true;
    default: return false;
    }
}

bool IsSrgbFormat(DXGI_FORMAT f) {
    return f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
           f == DXGI_FORMAT_B8G8R8X8_UNORM_SRGB;
}

bool IsHdrFormat(DXGI_FORMAT f) {
    return f == DXGI_FORMAT_R16G16B16A16_FLOAT;
}

DXGI_FORMAT OutputFormatFor(DXGI_FORMAT bb) {
    if (bb == DXGI_FORMAT_R16G16B16A16_FLOAT) return DXGI_FORMAT_R16G16B16A16_FLOAT;
    if (bb == DXGI_FORMAT_R10G10B10A2_UNORM) return DXGI_FORMAT_R10G10B10A2_UNORM;
    return DXGI_FORMAT_R8G8B8A8_UNORM;
}

bool FrameGen::MakeTex(Tex& t, UINT w, UINT h, DXGI_FORMAT viewFormat, bool uav,
                       DXGI_FORMAT resourceFormat) {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = w;
    desc.Height = h;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = resourceFormat != DXGI_FORMAT_UNKNOWN ? resourceFormat : viewFormat;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | (uav ? D3D11_BIND_UNORDERED_ACCESS : 0);
    if (FAILED(dev_->CreateTexture2D(&desc, nullptr, &t.tex))) return false;

    D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = viewFormat;
    srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srv.Texture2D.MipLevels = 1;
    if (FAILED(dev_->CreateShaderResourceView(t.tex.Get(), &srv, &t.srv))) return false;

    if (uav) {
        D3D11_UNORDERED_ACCESS_VIEW_DESC u{};
        u.Format = viewFormat;
        u.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
        if (FAILED(dev_->CreateUnorderedAccessView(t.tex.Get(), &u, &t.uav))) return false;
    }
    t.w = w;
    t.h = h;
    return true;
}

bool FrameGen::MakeTexArray(std::vector<Tex>& v, size_t count, UINT w, UINT h, DXGI_FORMAT fmt) {
    v.clear();
    v.resize(count);
    for (auto& t : v)
        if (!MakeTex(t, w, h, fmt, true)) return false;
    return true;
}

ComPtr<ID3D11Buffer> FrameGen::MakeConstants(float timestamp, bool firstIter, bool firstIterS) {
    Constants c{};
    c.firstIter = firstIter ? 1u : 0u;
    c.firstIterS = firstIterS ? 1u : 0u;
    c.advancedColorKind = p_.hdr ? 2u : 0u;
    c.hdrSupport = p_.hdr ? 1u : 0u;
    c.resolutionInvScale = 1.0f / p_.flowScale;
    c.timestamp = timestamp;
    c.uiThreshold = 0.5f;

    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = sizeof(Constants);
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA init{&c, 0, 0};
    ComPtr<ID3D11Buffer> buf;
    dev_->CreateBuffer(&desc, &init, &buf);
    return buf;
}

ID3D11Buffer* FrameGen::Cb(int pass, int kind) const {
    return cbPass_[static_cast<size_t>(denom_ - 2)][static_cast<size_t>(pass)][static_cast<size_t>(kind)].Get();
}

ID3D11ComputeShader* FrameGen::S(const char* name) const {
    std::string key = name;
    if (p_.performance && key != "mipmaps" && key != "generate") key = "p_" + key;
    auto it = cs_.find(key);
    return it == cs_.end() ? nullptr : it->second.Get();
}

bool FrameGen::Init(ID3D11Device* device, const FrameGenParams& params,
                    const ShaderBlobs& shaders, std::string& error) {
    dev_ = device;
    p_ = params;
    gens_ = params.multiplier - 1;
    frameIdx_ = 0;
    outFormat_ = OutputFormatFor(params.backbufferFormat);

    if (p_.performance && shaders.find("p_alpha[0]") == shaders.end()) {
        p_.performance = false; // this Lossless.dll has no performance-mode set
    }
    C_ = p_.performance ? 2 : 4;
    A1_ = p_.performance ? 1 : 2;
    T1_ = p_.performance ? 3 : 4;
    D_ = p_.performance ? 1 : 2;

    // Compute shaders
    cs_.clear();
    for (const auto& [name, blob] : shaders) {
        const bool isPerf = name.rfind("p_", 0) == 0;
        if (isPerf != p_.performance && name != "mipmaps" && name != "generate") continue;
        ComPtr<ID3D11ComputeShader> cs;
        if (FAILED(dev_->CreateComputeShader(blob.data(), blob.size(), nullptr, &cs))) {
            error = "CreateComputeShader failed for " + name;
            return false;
        }
        cs_[name] = cs;
    }

    // Samplers: border black, border white, clamp-to-edge (all linear)
    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_BORDER;
    sd.MaxAnisotropy = 1;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(dev_->CreateSamplerState(&sd, &sBlack_))) return error = "sampler", false;
    sd.BorderColor[0] = sd.BorderColor[1] = sd.BorderColor[2] = sd.BorderColor[3] = 1.0f;
    if (FAILED(dev_->CreateSamplerState(&sd, &sWhite_))) return error = "sampler", false;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.BorderColor[0] = sd.BorderColor[1] = sd.BorderColor[2] = sd.BorderColor[3] = 0.0f;
    if (FAILED(dev_->CreateSamplerState(&sd, &sClamp_))) return error = "sampler", false;

    // Constant buffers
    cbDefault_ = MakeConstants(0.0f, false, false);
    cbBeta_ = MakeConstants(0.5f, false, false);
    for (int d = 2; d <= 4; ++d)
        for (int p = 0; p < d - 1; ++p) {
            const float t = static_cast<float>(p + 1) / static_cast<float>(d);
            cbPass_[d - 2][p][0] = MakeConstants(t, false, false);
            cbPass_[d - 2][p][1] = MakeConstants(t, true, false);
            cbPass_[d - 2][p][2] = MakeConstants(t, false, true);
        }
    if (!cbDefault_ || !cbBeta_) return error = "constant buffer", false;

    const UINT W = p_.width, H = p_.height;
    const DXGI_FORMAT RGBA8 = DXGI_FORMAT_R8G8B8A8_UNORM;
    const DXGI_FORMAT R8 = DXGI_FORMAT_R8_UNORM;
    const DXGI_FORMAT F16 = DXGI_FORMAT_R16G16B16A16_FLOAT;

    // Two copies of the real frames (current / previous)
    for (auto& f : frames_)
        if (!MakeTex(f, W, H, ViewFormat(p_.backbufferFormat), false,
                     TypelessFormat(p_.backbufferFormat)))
            return error = "frame copy texture", false;

    // Luma mip chain at flow resolution
    const UINT fw = std::max(1u, static_cast<UINT>(static_cast<float>(W) * p_.flowScale));
    const UINT fh = std::max(1u, static_cast<UINT>(static_cast<float>(H) * p_.flowScale));
    for (int i = 0; i < 7; ++i)
        if (!MakeTex(mips_[i], Shr(fw, i), Shr(fh, i), R8, true)) return error = "mips", false;

    // Alpha: feature extraction per mip level
    for (int i = 0; i < 7; ++i) {
        Alpha& a = alpha_[i];
        a.in = &mips_[i];
        const UINT hw = Half(a.in->w), hh = Half(a.in->h);
        const UINT qw = Half(hw), qh = Half(hh);
        bool ok = MakeTexArray(a.t1, A1_, hw, hh, RGBA8) && MakeTexArray(a.t2, A1_, hw, hh, RGBA8) &&
                  MakeTexArray(a.t3, C_, qw, qh, RGBA8);
        for (auto& o : a.out) ok = ok && MakeTexArray(o, C_, qw, qh, RGBA8);
        if (!ok) return error = "alpha textures", false;
    }

    // Beta: scene-change / confidence pyramid
    {
        const UINT ew = alpha_[0].t3[0].w, eh = alpha_[0].t3[0].h;
        bool ok = MakeTexArray(bt1_, 2, ew, eh, RGBA8) && MakeTexArray(bt2_, 2, ew, eh, RGBA8);
        bout_.clear();
        bout_.resize(6);
        for (int i = 0; i < 6 && ok; ++i) ok = MakeTex(bout_[i], Shr(ew, i), Shr(eh, i), R8, true);
        if (!ok) return error = "beta textures", false;
    }

    // Gamma: coarse-to-fine motion estimation
    for (int i = 0; i < 7; ++i) {
        Gamma& g = gamma_[i];
        const Tex& ref = alpha_[6 - i].t3[0];
        g.in2 = &bout_[static_cast<size_t>(std::min(6 - i, 5))];
        g.opt = i == 0 ? nullptr : &gamma_[i - 1].out;
        if (!MakeTexArray(g.t1, T1_, ref.w, ref.h, RGBA8) ||
            !MakeTexArray(g.t2, C_, ref.w, ref.h, RGBA8) || !MakeTex(g.out, ref.w, ref.h, F16, true))
            return error = "gamma textures", false;
    }

    // Delta: refinement on the three finest levels
    for (int j = 0; j < 3; ++j) {
        const int i = j + 4;
        Delta& d = delta_[j];
        const Tex& ref = alpha_[6 - i].t3[0];
        d.in2 = &bout_[static_cast<size_t>(6 - i)];
        d.opt1 = i == 4 ? nullptr : &gamma_[i - 1].out;
        d.opt2 = j == 0 ? nullptr : &delta_[j - 1].out1;
        d.opt3 = j == 0 ? nullptr : &delta_[j - 1].out2;
        if (!MakeTexArray(d.t1, T1_, ref.w, ref.h, RGBA8) ||
            !MakeTexArray(d.t2, C_, ref.w, ref.h, RGBA8) ||
            !MakeTex(d.out1, ref.w, ref.h, F16, true) || !MakeTex(d.out2, ref.w, ref.h, F16, true))
            return error = "delta textures", false;
    }

    // Generated frames
    if (!MakeTexArray(outs_, static_cast<size_t>(gens_), W, H, outFormat_))
        return error = "output textures", false;

    // Make sure every shader we will dispatch exists.
    const char* needed[] = {"mipmaps", "generate", "alpha[0]", "alpha[1]", "alpha[2]", "alpha[3]",
                            "beta[0]", "beta[1]", "beta[2]", "beta[3]", "beta[4]",
                            "gamma[0]", "gamma[1]", "gamma[2]", "gamma[3]", "gamma[4]",
                            "delta[0]", "delta[1]", "delta[2]", "delta[3]", "delta[4]",
                            "delta[5]", "delta[6]", "delta[7]", "delta[8]", "delta[9]"};
    for (const char* n : needed)
        if (!S(n)) return error = std::string("missing shader ") + n, false;

    return true;
}

void FrameGen::Run(ID3D11DeviceContext* c, ID3D11ComputeShader* cs, ID3D11Buffer* cb,
                   std::initializer_list<ID3D11SamplerState*> samplers, const List& srvs,
                   const List& uavs, UINT gx, UINT gy) {
    ID3D11ShaderResourceView* srvArr[16] = {};
    ID3D11UnorderedAccessView* uavArr[8] = {};

    // Unbind everything first so nothing is bound as SRV and UAV at once.
    c->CSSetUnorderedAccessViews(0, 8, uavArr, nullptr);
    c->CSSetShaderResources(0, 16, srvArr);

    const UINT nSrv = static_cast<UINT>(std::min<size_t>(srvs.size(), 16));
    const UINT nUav = static_cast<UINT>(std::min<size_t>(uavs.size(), 8));
    for (UINT i = 0; i < nSrv; ++i) srvArr[i] = srvs[i] ? srvs[i]->srv.Get() : nullptr;
    for (UINT i = 0; i < nUav; ++i) uavArr[i] = uavs[i] ? uavs[i]->uav.Get() : nullptr;

    ID3D11SamplerState* smp[4] = {};
    UINT nSmp = 0;
    for (ID3D11SamplerState* s : samplers)
        if (nSmp < 4) smp[nSmp++] = s;

    c->CSSetShader(cs, nullptr, 0);
    if (cb) c->CSSetConstantBuffers(0, 1, &cb);
    if (nSmp) c->CSSetSamplers(0, nSmp, smp);
    if (nSrv) c->CSSetShaderResources(0, nSrv, srvArr);
    if (nUav) c->CSSetUnorderedAccessViews(0, nUav, uavArr, nullptr);
    c->Dispatch(std::max(1u, gx), std::max(1u, gy), 1);
}

namespace {
void Add(std::vector<const Tex*>& l, const std::vector<Tex>& v, size_t n) {
    for (size_t i = 0; i < n && i < v.size(); ++i) l.push_back(&v[i]);
}
} // namespace

void FrameGen::RunMipmaps(ID3D11DeviceContext* c, uint64_t fc) {
    List u;
    for (auto& m : mips_) u.push_back(&m);
    Run(c, S("mipmaps"), cbDefault_.Get(), {sBlack_.Get()}, {&frames_[fc % 2]}, u,
        Groups(mips_[0].w, 6), Groups(mips_[0].h, 6));
}

void FrameGen::RunAlpha(ID3D11DeviceContext* c, Alpha& a, uint64_t fc) {
    const UINT hx = Groups(a.t1[0].w, 3), hy = Groups(a.t1[0].h, 3);
    const UINT qx = Groups(a.t3[0].w, 3), qy = Groups(a.t3[0].h, 3);
    ID3D11SamplerState* s = sBlack_.Get();
    List in, out;

    out.clear(); Add(out, a.t1, A1_);
    Run(c, S("alpha[0]"), nullptr, {s}, {a.in}, out, hx, hy);

    in.clear(); Add(in, a.t1, A1_);
    out.clear(); Add(out, a.t2, A1_);
    Run(c, S("alpha[1]"), nullptr, {s}, in, out, hx, hy);

    in.clear(); Add(in, a.t2, A1_);
    out.clear(); Add(out, a.t3, C_);
    Run(c, S("alpha[2]"), nullptr, {s}, in, out, qx, qy);

    in.clear(); Add(in, a.t3, C_);
    out.clear(); Add(out, a.out[fc % 3], C_);
    Run(c, S("alpha[3]"), nullptr, {s}, in, out, qx, qy);
}

void FrameGen::RunBeta(ID3D11DeviceContext* c, uint64_t fc) {
    const auto& in = alpha_[0].out;
    const size_t k = fc % 3;
    const UINT ew = bt1_[0].w, eh = bt1_[0].h;
    const UINT gx = Groups(ew, 3), gy = Groups(eh, 3);
    List s, u;

    Add(s, in[(k + 1) % 3], C_);
    Add(s, in[(k + 2) % 3], C_);
    Add(s, in[k], C_);
    Add(u, bt1_, 2);
    Run(c, S("beta[0]"), nullptr, {sWhite_.Get()}, s, u, gx, gy);

    s.clear(); u.clear(); Add(s, bt1_, 2); Add(u, bt2_, 2);
    Run(c, S("beta[1]"), nullptr, {sBlack_.Get()}, s, u, gx, gy);

    s.clear(); u.clear(); Add(s, bt2_, 2); Add(u, bt1_, 2);
    Run(c, S("beta[2]"), nullptr, {sBlack_.Get()}, s, u, gx, gy);

    s.clear(); u.clear(); Add(s, bt1_, 2); Add(u, bt2_, 2);
    Run(c, S("beta[3]"), nullptr, {sBlack_.Get()}, s, u, gx, gy);

    s.clear(); u.clear(); Add(s, bt2_, 2); Add(u, bout_, 6);
    Run(c, S("beta[4]"), cbBeta_.Get(), {sBlack_.Get()}, s, u, Groups(ew, 5), Groups(eh, 5));
}

void FrameGen::RunGamma(ID3D11DeviceContext* c, int i, int pass, uint64_t fc) {
    Gamma& g = gamma_[i];
    const Alpha& a = alpha_[6 - i];
    ID3D11Buffer* cb = Cb(pass, g.opt ? 0 : 1);
    const UINT gx = Groups(g.t1[0].w, 3), gy = Groups(g.t1[0].h, 3);
    List s, u;

    Add(s, a.out[(fc + 2) % 3], C_);
    Add(s, a.out[fc % 3], C_);
    s.push_back(g.opt);
    Add(u, g.t1, 3);
    Run(c, S("gamma[0]"), cb, {sWhite_.Get(), sClamp_.Get()}, s, u, gx, gy);

    s.clear(); u.clear(); Add(s, g.t1, 3); Add(u, g.t2, C_);
    Run(c, S("gamma[1]"), nullptr, {sBlack_.Get()}, s, u, gx, gy);

    s.clear(); u.clear(); Add(s, g.t2, C_); Add(u, g.t1, C_);
    Run(c, S("gamma[2]"), nullptr, {sBlack_.Get()}, s, u, gx, gy);

    s.clear(); u.clear(); Add(s, g.t1, C_); Add(u, g.t2, C_);
    Run(c, S("gamma[3]"), nullptr, {sBlack_.Get()}, s, u, gx, gy);

    s.clear(); u.clear();
    Add(s, g.t2, C_);
    s.push_back(g.opt);
    s.push_back(g.in2);
    u.push_back(&g.out);
    Run(c, S("gamma[4]"), cb, {sBlack_.Get(), sClamp_.Get()}, s, u, gx, gy);
}

void FrameGen::RunDelta(ID3D11DeviceContext* c, int j, int pass, uint64_t fc) {
    const int i = j + 4;
    Delta& d = delta_[j];
    const Alpha& a = alpha_[6 - i];
    ID3D11Buffer* cb = Cb(pass, d.opt1 ? 0 : 2);
    const UINT gx = Groups(d.t1[0].w, 3), gy = Groups(d.t1[0].h, 3);
    List s, u;

    Add(s, a.out[(fc + 2) % 3], C_);
    Add(s, a.out[fc % 3], C_);
    s.push_back(d.opt1);
    Add(u, d.t1, 3);
    Run(c, S("delta[0]"), cb, {sWhite_.Get(), sClamp_.Get()}, s, u, gx, gy);

    s.clear(); u.clear(); Add(s, d.t1, 3); Add(u, d.t2, C_);
    Run(c, S("delta[1]"), nullptr, {sBlack_.Get()}, s, u, gx, gy);

    s.clear(); u.clear(); Add(s, d.t2, C_); Add(u, d.t1, C_);
    Run(c, S("delta[2]"), nullptr, {sBlack_.Get()}, s, u, gx, gy);

    s.clear(); u.clear(); Add(s, d.t1, C_); Add(u, d.t2, C_);
    Run(c, S("delta[3]"), nullptr, {sBlack_.Get()}, s, u, gx, gy);

    s.clear(); u.clear();
    Add(s, d.t2, C_);
    s.push_back(d.opt1);
    s.push_back(d.in2);
    u.push_back(&d.out1);
    Run(c, S("delta[4]"), cb, {sBlack_.Get(), sClamp_.Get()}, s, u, gx, gy);

    s.clear(); u.clear();
    Add(s, a.out[(fc + 2) % 3], C_);
    Add(s, a.out[fc % 3], C_);
    s.push_back(d.opt1);
    s.push_back(d.opt2);
    Add(u, d.t2, D_);
    Run(c, S("delta[5]"), cb, {sWhite_.Get(), sClamp_.Get()}, s, u, gx, gy);

    s.clear(); u.clear(); Add(s, d.t2, D_); Add(u, d.t1, D_);
    Run(c, S("delta[6]"), nullptr, {sBlack_.Get()}, s, u, gx, gy);

    s.clear(); u.clear(); Add(s, d.t1, D_); Add(u, d.t2, D_);
    Run(c, S("delta[7]"), nullptr, {sBlack_.Get()}, s, u, gx, gy);

    s.clear(); u.clear(); Add(s, d.t2, D_); Add(u, d.t1, D_);
    Run(c, S("delta[8]"), nullptr, {sBlack_.Get()}, s, u, gx, gy);

    s.clear(); u.clear();
    Add(s, d.t1, D_);
    s.push_back(d.opt3);
    u.push_back(&d.out2);
    Run(c, S("delta[9]"), cb, {sBlack_.Get(), sClamp_.Get()}, s, u, gx, gy);
}

void FrameGen::RunGenerate(ID3D11DeviceContext* c, int pass, uint64_t fc) {
    const Tex* prev = &frames_[(fc + 1) % 2];
    const Tex* cur = &frames_[fc % 2];
    List s = {prev, cur, &gamma_[6].out, &delta_[2].out1, &delta_[2].out2};
    List u = {&outs_[static_cast<size_t>(pass)]};
    Run(c, S("generate"), Cb(pass, 0), {sBlack_.Get(), sClamp_.Get()}, s, u,
        Groups(p_.width, 4), Groups(p_.height, 4));
}

int FrameGen::Process(ID3D11DeviceContext* c, ID3D11Resource* backbuffer, int gens, int denom) {
    const uint64_t fc = frameIdx_;
    if (gens < 0 || gens > gens_) gens = gens_;
    if (denom < gens + 1) denom = gens + 1;
    denom_ = denom < 2 ? 2 : (denom > 4 ? 4 : denom);
    if (gens > denom_ - 1) gens = denom_ - 1;
    c->CopyResource(frames_[fc % 2].tex.Get(), backbuffer);

    RunMipmaps(c, fc);
    for (int i = 6; i >= 0; --i) RunAlpha(c, alpha_[i], fc);
    RunBeta(c, fc);
    for (int pass = 0; pass < gens; ++pass) {
        for (int i = 0; i < 7; ++i) {
            RunGamma(c, i, pass, fc);
            if (i >= 4) RunDelta(c, i - 4, pass, fc);
        }
        RunGenerate(c, pass, fc);
    }

    ID3D11ShaderResourceView* nullSrv[16] = {};
    ID3D11UnorderedAccessView* nullUav[8] = {};
    c->CSSetShaderResources(0, 16, nullSrv);
    c->CSSetUnorderedAccessViews(0, 8, nullUav, nullptr);
    c->CSSetShader(nullptr, nullptr, 0);

    frameIdx_++;
    // The motion pipeline keeps a 3-frame history; wait until it is filled.
    return fc >= 2 ? gens : 0;
}

} // namespace lsfgi

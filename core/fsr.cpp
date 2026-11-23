#include "fsr.h"

#include <d3dcompiler.h>

#include <cmath>
#include <cstdint>
#include <cstring>

#define A_CPU 1
#include <ffx_a.h>
#include <ffx_fsr1.h>

#include "log.h"
#include "runtime.h"

namespace lsfgi {

using Microsoft::WRL::ComPtr;

namespace {

struct Resource {
    const char* data = nullptr;
    UINT size = 0;
};

Resource LoadText(const char* name) {
    Resource r;
    HMODULE m = G().self;
    HRSRC res = FindResourceA(m, name, MAKEINTRESOURCEA(10));
    if (!res) return r;
    HGLOBAL h = LoadResource(m, res);
    r.data = h ? static_cast<const char*>(LockResource(h)) : nullptr;
    r.size = r.data ? SizeofResource(m, res) : 0;
    return r;
}

// Serves the embedded ffx_a.h / ffx_fsr1.h to the HLSL compiler.
class EmbeddedIncludes : public ID3DInclude {
public:
    HRESULT STDMETHODCALLTYPE Open(D3D_INCLUDE_TYPE, LPCSTR name, LPCVOID, LPCVOID* data, UINT* bytes) override {
        Resource r;
        if (_stricmp(name, "ffx_a.h") == 0) r = LoadText("FFX_A_H");
        if (_stricmp(name, "ffx_fsr1.h") == 0) r = LoadText("FFX_FSR1_H");
        if (!r.data) return E_FAIL;
        *data = r.data;
        *bytes = r.size;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Close(LPCVOID) override { return S_OK; }
};

ComPtr<ID3D11ComputeShader> Compile(ID3D11Device* dev, pD3DCompile compile, bool easu, std::string& error) {
    const Resource src = LoadText("NITRO_FSR_HLSL");
    if (!src.data) {
        error = "FSR shader source missing from the Nitro DLL";
        return nullptr;
    }
    const D3D_SHADER_MACRO macros[] = {{"SAMPLE_SLOW_FALLBACK", "1"},
                                       {"SAMPLE_BILINEAR", "0"},
                                       {"SAMPLE_EASU", easu ? "1" : "0"},
                                       {"SAMPLE_RCAS", easu ? "0" : "1"},
                                       {"WIDTH", "64"},
                                       {"HEIGHT", "1"},
                                       {"DEPTH", "1"},
                                       {nullptr, nullptr}};
    EmbeddedIncludes includes;
    ComPtr<ID3DBlob> code, errors;
    if (FAILED(compile(src.data, src.size, "fsr_pass.hlsl", macros, &includes, "mainCS", "cs_5_0",
                       D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors))) {
        error = std::string("FSR shader compile failed: ") +
                (errors ? static_cast<const char*>(errors->GetBufferPointer()) : "unknown error");
        return nullptr;
    }
    ComPtr<ID3D11ComputeShader> cs;
    if (FAILED(dev->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &cs))) {
        error = "FSR shader creation failed";
        return nullptr;
    }
    return cs;
}

struct Constants {
    AU1 c0[4], c1[4], c2[4], c3[4];
    AU1 sample[4];
};
static_assert(sizeof(Constants) == 80, "FSR constant buffer layout");

} // namespace

bool MakeTexture(ID3D11Device* dev, Tex& t, UINT w, UINT h, DXGI_FORMAT viewFormat, UINT bind,
                 DXGI_FORMAT resourceFormat) {
    t = Tex{};
    D3D11_TEXTURE2D_DESC d{};
    d.Width = w;
    d.Height = h;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = resourceFormat != DXGI_FORMAT_UNKNOWN ? resourceFormat : viewFormat;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = bind;
    if (FAILED(dev->CreateTexture2D(&d, nullptr, &t.tex))) return false;
    if (bind & D3D11_BIND_SHADER_RESOURCE) {
        D3D11_SHADER_RESOURCE_VIEW_DESC s{};
        s.Format = viewFormat;
        s.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        s.Texture2D.MipLevels = 1;
        if (FAILED(dev->CreateShaderResourceView(t.tex.Get(), &s, &t.srv))) return false;
    }
    if (bind & D3D11_BIND_UNORDERED_ACCESS) {
        D3D11_UNORDERED_ACCESS_VIEW_DESC u{};
        u.Format = viewFormat;
        u.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
        if (FAILED(dev->CreateUnorderedAccessView(t.tex.Get(), &u, &t.uav))) return false;
    }
    t.w = w;
    t.h = h;
    return true;
}

bool Fsr::Init(ID3D11Device* device, std::string& error) {
    dev_ = device;
    HMODULE compiler = LoadLibraryW(L"d3dcompiler_47.dll");
    auto compile = compiler ? reinterpret_cast<pD3DCompile>(GetProcAddress(compiler, "D3DCompile")) : nullptr;
    if (!compile) {
        error = "d3dcompiler_47.dll not available";
        return false;
    }
    easu_ = Compile(device, compile, true, error);
    rcas_ = easu_ ? Compile(device, compile, false, error) : nullptr;
    if (!easu_ || !rcas_) return false;

    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxAnisotropy = 1;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = sizeof(Constants);
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(dev_->CreateSamplerState(&sd, &sampler_)) || FAILED(dev_->CreateBuffer(&bd, nullptr, &cb_))) {
        error = "FSR resources";
        return false;
    }
    std::string blitErr;
    blit_.Init(device, blitErr);
    return true;
}

bool Fsr::Ensure(DXGI_FORMAT srcFormat, UINT sw, UINT sh, DXGI_FORMAT dstFormat, UINT dw, UINT dh) {
    if (src_.tex && srcFormat == srcFormat_ && sw == sw_ && sh == sh_ && dstFormat == dstFormat_ && dw == dw_ &&
        dh == dh_)
        return true;
    const UINT rw = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    if (!MakeTexture(dev_.Get(), src_, sw, sh, ViewFormat(srcFormat), D3D11_BIND_SHADER_RESOURCE,
                     TypelessFormat(srcFormat)) ||
        !MakeTexture(dev_.Get(), mid_, dw, dh, DXGI_FORMAT_R16G16B16A16_FLOAT, rw) ||
        !MakeTexture(dev_.Get(), out_, dw, dh, OutputFormatFor(dstFormat), rw)) {
        src_ = Tex{};
        return false;
    }
    srcFormat_ = srcFormat;
    dstFormat_ = dstFormat;
    sw_ = sw;
    sh_ = sh;
    dw_ = dw;
    dh_ = dh;
    return true;
}

bool Fsr::Apply(ID3D11DeviceContext* c, ID3D11Resource* src, DXGI_FORMAT srcFormat, UINT sw, UINT sh,
                ID3D11Resource* dst, DXGI_FORMAT dstFormat, UINT dw, UINT dh, bool upscale, float sharpness) {
    if (!easu_ || !Ensure(srcFormat, sw, sh, dstFormat, dw, dh)) return false;
    c->CopyResource(src_.tex.Get(), src);

    ID3D11ShaderResourceView* nullSrv = nullptr;
    ID3D11UnorderedAccessView* nullUav = nullptr;
    ID3D11SamplerState* smp = sampler_.Get();
    ID3D11Buffer* cb = cb_.Get();
    c->CSSetSamplers(0, 1, &smp);
    c->CSSetConstantBuffers(0, 1, &cb);
    const UINT gx = (dw + 15) / 16, gy = (dh + 15) / 16;

    const Tex* rcasInput = &src_;
    if (upscale) {
        Constants k{};
        FsrEasuCon(k.c0, k.c1, k.c2, k.c3, static_cast<AF1>(sw), static_cast<AF1>(sh), static_cast<AF1>(sw),
                   static_cast<AF1>(sh), static_cast<AF1>(dw), static_cast<AF1>(dh));
        c->UpdateSubresource(cb_.Get(), 0, nullptr, &k, 0, 0);
        ID3D11ShaderResourceView* s = src_.srv.Get();
        ID3D11UnorderedAccessView* u = mid_.uav.Get();
        c->CSSetShader(easu_.Get(), nullptr, 0);
        c->CSSetShaderResources(0, 1, &s);
        c->CSSetUnorderedAccessViews(0, 1, &u, nullptr);
        c->Dispatch(gx, gy, 1);
        c->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
        c->CSSetShaderResources(0, 1, &nullSrv);
        rcasInput = &mid_;
    }

    Constants k{};
    const float stops = 2.0f * (1.0f - (sharpness < 0 ? 0 : (sharpness > 1 ? 1 : sharpness)));
    FsrRcasCon(k.c0, stops);
    c->UpdateSubresource(cb_.Get(), 0, nullptr, &k, 0, 0);
    ID3D11ShaderResourceView* s = rcasInput->srv.Get();
    ID3D11UnorderedAccessView* u = out_.uav.Get();
    c->CSSetShader(rcas_.Get(), nullptr, 0);
    c->CSSetShaderResources(0, 1, &s);
    c->CSSetUnorderedAccessViews(0, 1, &u, nullptr);
    c->Dispatch(gx, gy, 1);
    c->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
    c->CSSetShaderResources(0, 1, &nullSrv);
    c->CSSetShader(nullptr, nullptr, 0);

    if (OutputFormatFor(dstFormat) == dstFormat)
        c->CopyResource(dst, out_.tex.Get());
    else
        blit_.Blit(c, out_.srv.Get(), dst, dw, dh, IsSrgbFormat(dstFormat));
    return true;
}

} // namespace lsfgi

#include "blit.h"

#include <d3dcompiler.h>

#include <cstring>

namespace lsfgi {

namespace {

const char kBlitHlsl[] = R"(
Texture2D<float4> src : register(t0);
cbuffer Params : register(b0) { uint srgbDecode; uint3 pad; };

float4 VSMain(uint id : SV_VertexID) : SV_Position {
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}

float3 ToLinear(float3 c) {
    return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}

float4 PSMain(float4 pos : SV_Position) : SV_Target {
    float4 c = src.Load(int3(pos.xy, 0));
    if (srgbDecode) c.rgb = ToLinear(c.rgb);
    return float4(c.rgb, 1);
}
)";

Microsoft::WRL::ComPtr<ID3DBlob> Compile(pD3DCompile compile, const char* entry,
                                         const char* target, std::string& error) {
    Microsoft::WRL::ComPtr<ID3DBlob> code, errors;
    const HRESULT hr = compile(kBlitHlsl, std::strlen(kBlitHlsl), "nitro_blit", nullptr, nullptr,
                               entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    if (FAILED(hr)) {
        error = "blit shader compile failed";
        if (errors) error += ": " + std::string(static_cast<const char*>(errors->GetBufferPointer()));
        return nullptr;
    }
    return code;
}

} // namespace

bool Blitter::Init(ID3D11Device* device, std::string& error) {
    dev_ = device;
    HMODULE compiler = LoadLibraryW(L"d3dcompiler_47.dll");
    if (!compiler) {
        error = "d3dcompiler_47.dll not available";
        return false;
    }
    auto compile = reinterpret_cast<pD3DCompile>(GetProcAddress(compiler, "D3DCompile"));
    if (!compile) {
        error = "D3DCompile not found";
        return false;
    }

    auto vs = Compile(compile, "VSMain", "vs_5_0", error);
    auto ps = Compile(compile, "PSMain", "ps_5_0", error);
    if (!vs || !ps) return false;
    if (FAILED(dev_->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &vs_)) ||
        FAILED(dev_->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &ps_))) {
        error = "blit shader creation failed";
        return false;
    }

    UINT data[4] = {0, 0, 0, 0};
    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = sizeof(data);
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA init{data, 0, 0};
    if (FAILED(dev_->CreateBuffer(&desc, &init, &cbPlain_))) return error = "blit cb", false;
    data[0] = 1;
    if (FAILED(dev_->CreateBuffer(&desc, &init, &cbDecode_))) return error = "blit cb", false;
    return true;
}

void Blitter::Blit(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* src, ID3D11Resource* dst,
                   UINT width, UINT height, bool srgbDecode) {
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv;
    if (FAILED(dev_->CreateRenderTargetView(dst, nullptr, &rtv))) return;

    D3D11_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f};
    ID3D11Buffer* cb = srgbDecode ? cbDecode_.Get() : cbPlain_.Get();
    ID3D11RenderTargetView* rtvs[] = {rtv.Get()};

    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(vs_.Get(), nullptr, 0);
    ctx->PSSetShader(ps_.Get(), nullptr, 0);
    ctx->PSSetConstantBuffers(0, 1, &cb);
    ctx->PSSetShaderResources(0, 1, &src);
    ctx->RSSetState(nullptr);
    ctx->RSSetViewports(1, &vp);
    ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
    ctx->OMSetDepthStencilState(nullptr, 0);
    ctx->OMSetRenderTargets(1, rtvs, nullptr);
    ctx->Draw(3, 0);

    ID3D11ShaderResourceView* nullSrv = nullptr;
    ctx->PSSetShaderResources(0, 1, &nullSrv);
    ctx->OMSetRenderTargets(0, nullptr, nullptr);
}

} // namespace lsfgi

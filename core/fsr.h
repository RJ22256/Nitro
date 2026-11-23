// AMD FidelityFX Super Resolution 1 (EASU upscaling + RCAS sharpening) on D3D11.
// The official FSR 1 shaders (MIT) are embedded in the Nitro DLL and compiled
// at run time.
#pragma once

#include <d3d11.h>
#include <wrl/client.h>

#include <string>

#include "blit.h"
#include "framegen.h"

namespace lsfgi {

class Fsr {
public:
    bool Init(ID3D11Device* device, std::string& error);

    // Reads src (sw x sh) and writes the result into dst (dw x dh).
    // upscale: EASU to the destination size, then RCAS; otherwise RCAS only
    // (src and dst the same size; src may be dst). sharpness: 0 (soft) .. 1 (sharpest).
    bool Apply(ID3D11DeviceContext* ctx, ID3D11Resource* src, DXGI_FORMAT srcFormat, UINT sw, UINT sh,
               ID3D11Resource* dst, DXGI_FORMAT dstFormat, UINT dw, UINT dh, bool upscale, float sharpness);

private:
    bool Ensure(DXGI_FORMAT srcFormat, UINT sw, UINT sh, DXGI_FORMAT dstFormat, UINT dw, UINT dh);

    Microsoft::WRL::ComPtr<ID3D11Device> dev_;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> easu_, rcas_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> cb_;
    Tex src_, mid_, out_;
    DXGI_FORMAT srcFormat_ = DXGI_FORMAT_UNKNOWN, dstFormat_ = DXGI_FORMAT_UNKNOWN;
    UINT sw_ = 0, sh_ = 0, dw_ = 0, dh_ = 0;
    Blitter blit_;
};

// Creates a texture + views (helper shared with the FSR code).
bool MakeTexture(ID3D11Device* dev, Tex& t, UINT w, UINT h, DXGI_FORMAT viewFormat, UINT bind,
                 DXGI_FORMAT resourceFormat = DXGI_FORMAT_UNKNOWN);

} // namespace lsfgi

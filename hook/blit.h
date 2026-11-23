#pragma once

#include <d3d11.h>
#include <wrl/client.h>

#include <string>

namespace lsfgi {

// Fullscreen-triangle copy of a texture into a back buffer, used when the
// generated frame's format differs from the swap chain's (e.g. BGRA vs RGBA).
class Blitter {
public:
    bool Init(ID3D11Device* device, std::string& error);
    void Blit(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* src, ID3D11Resource* dst,
              UINT width, UINT height, bool srgbDecode);

private:
    Microsoft::WRL::ComPtr<ID3D11Device> dev_;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vs_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> ps_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> cbPlain_, cbDecode_;
};

} // namespace lsfgi

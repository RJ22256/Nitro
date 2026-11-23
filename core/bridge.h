// Runs frame generation on Nitro's own D3D11 device, for games whose graphics
// API can't run our D3D11 pipeline directly (DX9, DX10, OpenGL, Vulkan).
//
// The game side copies its finished frame into Input(), calls Run(), then
// copies each Output(k) back to its back buffer. Input/outputs can be shared
// with other APIs (legacy or NT handles), or filled/read through the CPU.
#pragma once

#include <d3d11.h>
#include <wrl/client.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "blit.h"
#include "framegen.h"

namespace lsfgi {

enum class ShareMode { None, Legacy, NtHandle };

struct BridgeParams {
    const LUID* adapter = nullptr; // nullptr = default adapter
    UINT vendorId = 0;             // used when no LUID is known (0 = any)
    UINT width = 0;
    UINT height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_B8G8R8A8_UNORM; // typed, non-sRGB
    ShareMode share = ShareMode::None;
    bool renderTargetOutputs = false; // extra bind flag for GL interop
};

class Bridge {
public:
    Bridge() = default;
    Bridge(const Bridge&) = delete;
    Bridge& operator=(const Bridge&) = delete;

    bool Init(const BridgeParams& p, std::string& error);
    bool Matches(UINT w, UINT h, DXGI_FORMAT f, int multiplier) const;

    ID3D11Device* Device() const { return dev_.Get(); }
    ID3D11Texture2D* Input() const { return in_.Get(); }
    ID3D11Texture2D* Output(int k) const { return outs_[static_cast<size_t>(k)].Get(); }
    HANDLE InputHandle() const { return inHandle_; }
    HANDLE OutputHandle(int k) const { return outHandles_[static_cast<size_t>(k)]; }
    int MaxOutputs() const { return static_cast<int>(outs_.size()); }

    // Generates frames from Input() into Output(0..n-1) (when generate is
    // true), and if overlay is set draws it on every output and on
    // RealOutput() (a copy of Input()). Waits for the GPU.
    using OverlayFn = std::function<void(ID3D11DeviceContext*, ID3D11Texture2D*)>;
    int Run(bool generate = true, const OverlayFn& overlay = {}, int gens = -1, int denom = 0);
    ID3D11Texture2D* RealOutput() const { return real_.Get(); }
    HANDLE RealOutputHandle() const { return realHandle_; }
    ID3D11DeviceContext* Context() const { return ctx_.Get(); }
    void Reset();

    // CPU path
    void Upload(const void* data, UINT pitch);
    bool Download(int k, void* dst, UINT dstPitch, UINT bytesPerRow, UINT rows); // k = -1: RealOutput()

    ~Bridge();

private:
    bool MakeShared(Microsoft::WRL::ComPtr<ID3D11Texture2D>& tex, HANDLE& handle, UINT bind,
                    std::string& error);
    void WaitIdle();

    BridgeParams p_;
    int mult_ = 0;
    int version_ = -1;
    Microsoft::WRL::ComPtr<ID3D11Device> dev_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> ctx_;
    Microsoft::WRL::ComPtr<ID3D11Query> done_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> in_;
    std::vector<Microsoft::WRL::ComPtr<ID3D11Texture2D>> outs_;
    std::vector<Microsoft::WRL::ComPtr<ID3D11Texture2D>> staging_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> real_, realStaging_;
    HANDLE inHandle_ = nullptr;
    HANDLE realHandle_ = nullptr;
    std::vector<HANDLE> outHandles_;
    std::unique_ptr<FrameGen> fg_;
    Blitter blit_;
    bool blitReady_ = false;
};

// Finds a DXGI adapter by LUID or vendor id and creates a D3D11 device on it.
bool CreateD3D11DeviceOn(const LUID* luid, UINT vendorId, Microsoft::WRL::ComPtr<ID3D11Device>& dev,
                         Microsoft::WRL::ComPtr<ID3D11DeviceContext>& ctx);

// System32 copy of a DLL, loaded once.
HMODULE SystemModule(const wchar_t* name);

} // namespace lsfgi

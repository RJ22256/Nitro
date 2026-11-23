#include "bridge.h"

#include <dxgi1_2.h>

#include <cstring>

#include "log.h"
#include "runtime.h"

namespace lsfgi {

using Microsoft::WRL::ComPtr;

HMODULE SystemModule(const wchar_t* name) {
    wchar_t path[MAX_PATH] = {};
    const UINT n = GetSystemDirectoryW(path, MAX_PATH);
    if (n == 0 || n + wcslen(name) + 2 >= MAX_PATH) return nullptr;
    wcscat_s(path, L"\\");
    wcscat_s(path, name);
    return LoadLibraryW(path);
}

bool CreateD3D11DeviceOn(const LUID* luid, UINT vendorId, ComPtr<ID3D11Device>& dev,
                         ComPtr<ID3D11DeviceContext>& ctx) {
    HMODULE dxgi = SystemModule(L"dxgi.dll");
    HMODULE d3d11 = SystemModule(L"d3d11.dll");
    if (!dxgi || !d3d11) return false;
    using CreateFactory1Fn = HRESULT(WINAPI*)(REFIID, void**);
    auto createFactory = reinterpret_cast<CreateFactory1Fn>(GetProcAddress(dxgi, "CreateDXGIFactory1"));
    auto createDevice = reinterpret_cast<PFN_D3D11_CREATE_DEVICE>(GetProcAddress(d3d11, "D3D11CreateDevice"));
    if (!createFactory || !createDevice) return false;

    ComPtr<IDXGIAdapter1> chosen;
    ComPtr<IDXGIFactory1> factory;
    if (SUCCEEDED(createFactory(IID_PPV_ARGS(&factory)))) {
        ComPtr<IDXGIAdapter1> a;
        for (UINT i = 0; factory->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; ++i, a.Reset()) {
            DXGI_ADAPTER_DESC1 d{};
            a->GetDesc1(&d);
            if (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
            const bool luidMatch = luid && d.AdapterLuid.LowPart == luid->LowPart &&
                                   d.AdapterLuid.HighPart == luid->HighPart;
            const bool vendorMatch = !luid && vendorId && d.VendorId == vendorId;
            if (luidMatch || vendorMatch) {
                chosen = a;
                break;
            }
        }
    }

    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    HRESULT hr = createDevice(chosen.Get(), chosen ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
                              nullptr, flags, levels, 2, D3D11_SDK_VERSION, &dev, nullptr, &ctx);
    if (FAILED(hr))
        hr = createDevice(chosen.Get(), chosen ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
                          nullptr, flags, levels + 1, 1, D3D11_SDK_VERSION, &dev, nullptr, &ctx);
    return SUCCEEDED(hr);
}

Bridge::~Bridge() {
    if (p_.share == ShareMode::NtHandle) {
        if (inHandle_) CloseHandle(inHandle_);
        if (realHandle_) CloseHandle(realHandle_);
        for (HANDLE h : outHandles_)
            if (h) CloseHandle(h);
    }
}

bool Bridge::MakeShared(ComPtr<ID3D11Texture2D>& tex, HANDLE& handle, UINT bind, std::string& error) {
    D3D11_TEXTURE2D_DESC d{};
    d.Width = p_.width;
    d.Height = p_.height;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = p_.format;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = bind;
    if (p_.share == ShareMode::Legacy) d.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
    if (p_.share == ShareMode::NtHandle)
        d.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
    if (FAILED(dev_->CreateTexture2D(&d, nullptr, &tex))) {
        error = "bridge texture creation failed";
        return false;
    }
    handle = nullptr;
    if (p_.share == ShareMode::Legacy) {
        ComPtr<IDXGIResource> r;
        if (FAILED(tex.As(&r)) || FAILED(r->GetSharedHandle(&handle))) {
            error = "GetSharedHandle failed";
            return false;
        }
    } else if (p_.share == ShareMode::NtHandle) {
        ComPtr<IDXGIResource1> r;
        if (FAILED(tex.As(&r)) ||
            FAILED(r->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                                         nullptr, &handle))) {
            error = "CreateSharedHandle failed";
            return false;
        }
    }
    return true;
}

bool Bridge::Init(const BridgeParams& p, std::string& error) {
    auto& g = G();
    p_ = p;
    mult_ = g.multiplier.load();
    version_ = g.settingsVersion.load();
    if (!CreateD3D11DeviceOn(p.adapter, p.vendorId, dev_, ctx_)) {
        error = "could not create Nitro's D3D11 device";
        return false;
    }

    FrameGenParams fp;
    fp.width = p.width;
    fp.height = p.height;
    fp.backbufferFormat = p.format;
    fp.multiplier = mult_;
    fp.flowScale = g.cfg.flowScale;
    fp.performance = g.cfg.performanceMode;
    fp.hdr = g.cfg.hdr < 0 ? IsHdrFormat(p.format) : g.cfg.hdr != 0;
    fg_ = std::make_unique<FrameGen>();
    if (!fg_->Init(dev_.Get(), fp, g.shaders, error)) return false;

    std::string blitErr;
    blitReady_ = blit_.Init(dev_.Get(), blitErr);
    if (fg_->OutputFormat() != p.format && !blitReady_) {
        error = "blitter unavailable: " + blitErr;
        return false;
    }

    const UINT bind = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    if (!MakeShared(in_, inHandle_, bind, error)) return false;
    if (!MakeShared(real_, realHandle_, bind, error)) return false;
    const int n = mult_ - 1;
    outs_.assign(static_cast<size_t>(n), nullptr);
    outHandles_.assign(static_cast<size_t>(n), nullptr);
    staging_.assign(static_cast<size_t>(n), nullptr);
    for (int k = 0; k < n; ++k)
        if (!MakeShared(outs_[k], outHandles_[k], bind, error)) return false;

    D3D11_QUERY_DESC q{D3D11_QUERY_EVENT, 0};
    if (FAILED(dev_->CreateQuery(&q, &done_))) {
        error = "query creation failed";
        return false;
    }
    Log("Bridge ready: %ux%u format %d, %dx", p.width, p.height, static_cast<int>(p.format), mult_);
    return true;
}

bool Bridge::Matches(UINT w, UINT h, DXGI_FORMAT f, int multiplier) const {
    return fg_ && w == p_.width && h == p_.height && f == p_.format && multiplier == mult_ &&
           version_ == G().settingsVersion.load();
}

void Bridge::WaitIdle() {
    ctx_->End(done_.Get());
    ctx_->Flush();
    BOOL finished = FALSE;
    while (ctx_->GetData(done_.Get(), &finished, sizeof(finished), 0) == S_FALSE) SwitchToThread();
}

int Bridge::Run(bool generate, const OverlayFn& overlay, int gens, int denom) {
    if (!fg_) return 0;
    if (!generate) fg_->Reset();
    const int n = generate ? fg_->Process(ctx_.Get(), in_.Get(), gens, denom) : 0;
    for (int k = 0; k < n; ++k) {
        const Tex& t = fg_->Generated(k);
        if (fg_->OutputFormat() == p_.format)
            ctx_->CopyResource(outs_[k].Get(), t.tex.Get());
        else
            blit_.Blit(ctx_.Get(), t.srv.Get(), outs_[k].Get(), p_.width, p_.height, false);
        if (overlay) overlay(ctx_.Get(), outs_[k].Get());
    }
    if (overlay) {
        ctx_->CopyResource(real_.Get(), in_.Get());
        overlay(ctx_.Get(), real_.Get());
    }
    WaitIdle();
    return n;
}

void Bridge::Reset() {
    if (fg_) fg_->Reset();
}

void Bridge::Upload(const void* data, UINT pitch) {
    ctx_->UpdateSubresource(in_.Get(), 0, nullptr, data, pitch, 0);
}

bool Bridge::Download(int k, void* dst, UINT dstPitch, UINT bytesPerRow, UINT rows) {
    auto& st = k < 0 ? realStaging_ : staging_[static_cast<size_t>(k)];
    ID3D11Texture2D* src = k < 0 ? real_.Get() : outs_[static_cast<size_t>(k)].Get();
    if (!st) {
        D3D11_TEXTURE2D_DESC d{};
        src->GetDesc(&d);
        d.Usage = D3D11_USAGE_STAGING;
        d.BindFlags = 0;
        d.MiscFlags = 0;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(dev_->CreateTexture2D(&d, nullptr, &st))) return false;
    }
    ctx_->CopyResource(st.Get(), src);
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(ctx_->Map(st.Get(), 0, D3D11_MAP_READ, 0, &m))) return false;
    const UINT row = bytesPerRow < m.RowPitch ? bytesPerRow : m.RowPitch;
    for (UINT y = 0; y < rows; ++y)
        std::memcpy(static_cast<uint8_t*>(dst) + static_cast<size_t>(y) * dstPitch,
                    static_cast<const uint8_t*>(m.pData) + static_cast<size_t>(y) * m.RowPitch, row);
    ctx_->Unmap(st.Get(), 0);
    return true;
}

} // namespace lsfgi

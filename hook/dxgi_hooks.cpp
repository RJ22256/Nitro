// DXGI swap chain hooks (DX10 / DX11 / DX12): every time the game presents a
// real frame, run LSFG on it, present the generated in-between frames, then
// present the real frame.

#include "hooks.h"

#include <d3d10_1.h> // must precede anything that pulls in d3d10.h
#include <d3d11_1.h>
#include <d3d11on12.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <cstdio>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "../core/blit.h"
#include "../core/bridge.h"
#include "../core/framegen.h"
#include "../core/log.h"
#include "../core/runtime.h"

namespace lsfgi {

using Microsoft::WRL::ComPtr;

namespace {

// ---------------------------------------------------------------------------
// vtable bookkeeping

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT,
                                               const DXGI_PRESENT_PARAMETERS*);
using ResizeBuffersFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT,
                                                    DXGI_FORMAT, UINT);
using ResizeBuffers1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain3*, UINT, UINT, UINT,
                                                     DXGI_FORMAT, UINT, const UINT*,
                                                     IUnknown* const*);

// IDXGISwapChain / IDXGISwapChain1 / IDXGISwapChain3 vtable slots
constexpr int kSlotPresent = 8;
constexpr int kSlotResizeBuffers = 13;
constexpr int kSlotPresent1 = 22;
constexpr int kSlotResizeBuffers1 = 39;

struct VtHooks {
    void** vt = nullptr;
    PresentFn present = nullptr;
    Present1Fn present1 = nullptr;
    ResizeBuffersFn resize = nullptr;
    ResizeBuffers1Fn resize1 = nullptr;
};

constexpr int kMaxVtables = 8;
VtHooks g_vt[kMaxVtables];
std::atomic<int> g_vtCount{0};

const VtHooks* FindVt(void* obj) {
    void** vt = *reinterpret_cast<void***>(obj);
    const int n = g_vtCount.load();
    for (int i = 0; i < n; ++i)
        if (g_vt[i].vt == vt) return &g_vt[i];
    return &g_vt[0];
}

// ---------------------------------------------------------------------------

struct PresentCall {
    IDXGISwapChain* sc;
    UINT sync;
    UINT flags;
    const DXGI_PRESENT_PARAMETERS* params;
    const VtHooks* vt;
    bool present1;

    HRESULT Real(UINT s, UINT f) const {
        if (present1)
            return vt->present1(static_cast<IDXGISwapChain1*>(sc), s, f, params);
        return vt->present(sc, s, f);
    }
    HRESULT Generated(UINT s, UINT f) const {
        if (present1) {
            DXGI_PRESENT_PARAMETERS none{};
            return vt->present1(static_cast<IDXGISwapChain1*>(sc), s, f, &none);
        }
        return vt->present(sc, s, f);
    }
};

class SwapState {
public:
    bool failed = false;

    bool Init(IDXGISwapChain* sc);
    bool SameDevice(IDXGISwapChain* sc) const;
    HRESULT Present(const PresentCall& call);
    void ReleaseBuffers();

private:
    HRESULT PresentD3D10(const PresentCall& call, const DXGI_SWAP_CHAIN_DESC& desc);
    bool EnsureBridge10(UINT w, UINT h, DXGI_FORMAT fmt);
    void Wait10();

    bool EnsureFrameGen(const DXGI_SWAP_CHAIN_DESC& desc);
    bool BeginFrame(IDXGISwapChain* sc, ComPtr<ID3D11Texture2D>& bb);
    void EndFrame();
    void WriteGenerated(int i, ID3D11Texture2D* bb);

    bool d3d12_ = false;
    bool d3d10_ = false;
    IUnknown* deviceKey_ = nullptr;

    // D3D10 games: frame gen runs on Nitro's own D3D11 device (shared textures)
    ComPtr<ID3D10Device> dev10_;
    ComPtr<ID3D10Query> query10_;
    LUID luid_{};
    std::unique_ptr<Bridge> bridge_;
    ComPtr<ID3D10Texture2D> in10_;
    std::vector<ComPtr<ID3D10Texture2D>> out10_;
    ComPtr<ID3D11Device> dev_;
    ComPtr<ID3D11DeviceContext> ctx_;

    // D3D11 games: we borrow the immediate context with our own state object
    ComPtr<ID3D11DeviceContext1> ctx1_;
    ComPtr<ID3DDeviceContextState> ourState_, savedState_;

    // D3D12 games: D3D11On12 on the game's own command queue
    ComPtr<ID3D12CommandQueue> queue_;
    ComPtr<ID3D11On12Device> on12_;
    std::unordered_map<ID3D12Resource*, ComPtr<ID3D11Texture2D>> wrapped_;
    ID3D11Texture2D* acquired_ = nullptr;

    std::unique_ptr<FrameGen> fg_;
    Blitter blit_;
    bool blitReady_ = false;
    DXGI_FORMAT bbFormat_ = DXGI_FORMAT_UNKNOWN;
    UINT w_ = 0, h_ = 0;
    int mult_ = 0;
    bool warnedFormat_ = false;
};

bool SwapState::Init(IDXGISwapChain* sc) {
    ComPtr<ID3D11Device> d11;
    if (SUCCEEDED(sc->GetDevice(IID_PPV_ARGS(&d11)))) {
        if (d11->GetFeatureLevel() < D3D_FEATURE_LEVEL_11_0) {
            Log("D3D11 device below feature level 11_0 - compute shaders unavailable, skipping");
            return false;
        }
        ComPtr<ID3D11Device1> d1;
        if (FAILED(d11.As(&d1))) {
            Log("ID3D11Device1 not available - skipping");
            return false;
        }
        dev_ = d11;
        deviceKey_ = d11.Get();
        dev_->GetImmediateContext(&ctx_);
        if (FAILED(ctx_.As(&ctx1_))) {
            Log("ID3D11DeviceContext1 not available - skipping");
            return false;
        }
        const UINT flags = (d11->GetCreationFlags() & D3D11_CREATE_DEVICE_SINGLETHREADED)
                               ? D3D11_1_CREATE_DEVICE_CONTEXT_STATE_SINGLETHREADED
                               : 0;
        D3D_FEATURE_LEVEL fl = d11->GetFeatureLevel();
        if (FAILED(d1->CreateDeviceContextState(flags, &fl, 1, D3D11_SDK_VERSION,
                                                __uuidof(ID3D11Device1), nullptr, &ourState_))) {
            Log("CreateDeviceContextState failed - skipping");
            return false;
        }
        Log("Attached to a D3D11 swap chain");
    } else if (ComPtr<ID3D10Device> d10; SUCCEEDED(sc->GetDevice(IID_PPV_ARGS(&d10)))) {
        dev10_ = d10;
        deviceKey_ = d10.Get();
        d3d10_ = true;
        ComPtr<IDXGIDevice> dxgiDev;
        ComPtr<IDXGIAdapter> adapter;
        DXGI_ADAPTER_DESC ad{};
        if (SUCCEEDED(d10.As(&dxgiDev)) && SUCCEEDED(dxgiDev->GetAdapter(&adapter)) &&
            SUCCEEDED(adapter->GetDesc(&ad)))
            luid_ = ad.AdapterLuid;
        D3D10_QUERY_DESC qd{D3D10_QUERY_EVENT, 0};
        if (FAILED(d10->CreateQuery(&qd, &query10_))) {
            Log("D3D10 query creation failed - skipping");
            return false;
        }
        Log("Attached to a D3D10 swap chain (frame gen on a separate D3D11 device)");
        return true;
    } else {
        if (FAILED(sc->GetDevice(IID_PPV_ARGS(&queue_)))) {
            Log("Swap chain is neither D3D11 nor D3D12 - skipping");
            return false;
        }
        ComPtr<ID3D12Device> d12;
        if (FAILED(queue_->GetDevice(IID_PPV_ARGS(&d12)))) return false;

        HMODULE d3d11 = GetModuleHandleW(L"d3d11.dll");
        if (!d3d11) d3d11 = LoadLibraryW(L"d3d11.dll");
        auto create = d3d11 ? reinterpret_cast<PFN_D3D11ON12_CREATE_DEVICE>(
                                  GetProcAddress(d3d11, "D3D11On12CreateDevice"))
                            : nullptr;
        if (!create) {
            Log("D3D11On12CreateDevice not available - skipping");
            return false;
        }
        IUnknown* queues[] = {queue_.Get()};
        D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
        if (FAILED(create(d12.Get(), D3D11_CREATE_DEVICE_BGRA_SUPPORT, &fl, 1, queues, 1, 0,
                          &dev_, &ctx_, nullptr)) ||
            FAILED(dev_.As(&on12_))) {
            Log("D3D11On12CreateDevice failed - skipping");
            return false;
        }
        d3d12_ = true;
        deviceKey_ = queue_.Get();
        Log("Attached to a D3D12 swap chain (via D3D11On12)");
    }

    std::string err;
    blitReady_ = blit_.Init(dev_.Get(), err);
    if (!blitReady_) Log("Blitter unavailable (%s); only RGBA/10-bit/FP16 swap chains will work", err.c_str());
    return true;
}

bool SwapState::SameDevice(IDXGISwapChain* sc) const {
    if (failed) return true;
    if (d3d10_) {
        ComPtr<ID3D10Device> d;
        return SUCCEEDED(sc->GetDevice(IID_PPV_ARGS(&d))) && d.Get() == deviceKey_;
    }
    if (d3d12_) {
        ComPtr<ID3D12CommandQueue> q;
        return SUCCEEDED(sc->GetDevice(IID_PPV_ARGS(&q))) && q.Get() == deviceKey_;
    }
    ComPtr<ID3D11Device> d;
    return SUCCEEDED(sc->GetDevice(IID_PPV_ARGS(&d))) && d.Get() == deviceKey_;
}

bool SwapState::EnsureFrameGen(const DXGI_SWAP_CHAIN_DESC& d) {
    auto& g = G();
    const int mult = g.multiplier.load();
    const UINT w = d.BufferDesc.Width, h = d.BufferDesc.Height;
    const DXGI_FORMAT fmt = d.BufferDesc.Format;
    if (fg_ && w == w_ && h == h_ && fmt == bbFormat_ && mult == mult_) return true;

    fg_.reset();
    FrameGenParams p;
    p.width = w;
    p.height = h;
    p.backbufferFormat = fmt;
    p.multiplier = mult;
    p.flowScale = g.cfg.flowScale;
    p.performance = g.cfg.performanceMode;
    p.hdr = g.cfg.hdr < 0 ? IsHdrFormat(fmt) : g.cfg.hdr != 0;

    if (OutputFormatFor(fmt) != fmt && !blitReady_) {
        Log("Back buffer format %d needs the blitter, which failed to initialise", static_cast<int>(fmt));
        return false;
    }

    auto fg = std::make_unique<FrameGen>();
    std::string err;
    if (!fg->Init(dev_.Get(), p, g.shaders, err)) {
        Log("Frame generation setup failed: %s", err.c_str());
        return false;
    }
    fg_ = std::move(fg);
    w_ = w;
    h_ = h;
    bbFormat_ = fmt;
    mult_ = mult;
    Log("Frame generation ready: %ux%u, format %d, %dx, flow scale %.2f, %s mode, HDR %s", w, h,
        static_cast<int>(fmt), mult, p.flowScale, fg_->Params().performance ? "performance" : "quality",
        p.hdr ? "on" : "off");
    return true;
}

bool SwapState::BeginFrame(IDXGISwapChain* sc, ComPtr<ID3D11Texture2D>& bb) {
    bb.Reset();
    if (!d3d12_) {
        if (FAILED(sc->GetBuffer(0, IID_PPV_ARGS(&bb)))) return false;
        ctx1_->SwapDeviceContextState(ourState_.Get(), &savedState_);
        return true;
    }

    ComPtr<IDXGISwapChain3> sc3;
    if (FAILED(sc->QueryInterface(IID_PPV_ARGS(&sc3)))) return false;
    const UINT idx = sc3->GetCurrentBackBufferIndex();
    ComPtr<ID3D12Resource> res;
    if (FAILED(sc->GetBuffer(idx, IID_PPV_ARGS(&res)))) return false;

    auto it = wrapped_.find(res.Get());
    if (it == wrapped_.end()) {
        D3D11_RESOURCE_FLAGS flags{};
        flags.BindFlags = D3D11_BIND_RENDER_TARGET;
        ComPtr<ID3D11Texture2D> tex;
        if (FAILED(on12_->CreateWrappedResource(res.Get(), &flags, D3D12_RESOURCE_STATE_PRESENT,
                                                D3D12_RESOURCE_STATE_PRESENT, IID_PPV_ARGS(&tex))))
            return false;
        it = wrapped_.emplace(res.Get(), tex).first;
    }
    ID3D11Resource* r = it->second.Get();
    on12_->AcquireWrappedResources(&r, 1);
    acquired_ = it->second.Get();
    bb = it->second;
    return true;
}

void SwapState::EndFrame() {
    if (!d3d12_) {
        if (savedState_) {
            ctx1_->SwapDeviceContextState(savedState_.Get(), nullptr);
            savedState_.Reset();
        }
        return;
    }
    if (acquired_) {
        ID3D11Resource* r = acquired_;
        on12_->ReleaseWrappedResources(&r, 1);
        acquired_ = nullptr;
    }
    ctx_->Flush();
}

void SwapState::WriteGenerated(int i, ID3D11Texture2D* bb) {
    const Tex& t = fg_->Generated(i);
    if (fg_->OutputFormat() == bbFormat_)
        ctx_->CopyResource(bb, t.tex.Get());
    else
        blit_.Blit(ctx_.Get(), t.srv.Get(), bb, w_, h_, IsSrgbFormat(bbFormat_));
}

void SwapState::ReleaseBuffers() {
    if (bridge_) bridge_->Reset();
    if (d3d12_ && ctx_) {
        wrapped_.clear();
        ctx_->ClearState();
        ctx_->Flush();
    }
    if (fg_) fg_->Reset();
}

void SwapState::Wait10() {
    query10_->End();
    dev10_->Flush();
    BOOL done = FALSE;
    while (query10_->GetData(&done, sizeof(done), 0) == S_FALSE) SwitchToThread();
}

bool SwapState::EnsureBridge10(UINT w, UINT h, DXGI_FORMAT fmt) {
    const DXGI_FORMAT view = ViewFormat(fmt);
    if (bridge_ && bridge_->Matches(w, h, view, G().multiplier.load())) return true;
    in10_.Reset();
    out10_.clear();
    bridge_ = std::make_unique<Bridge>();
    BridgeParams p;
    p.adapter = &luid_;
    p.width = w;
    p.height = h;
    p.format = view;
    p.share = ShareMode::Legacy;
    std::string err;
    if (!bridge_->Init(p, err)) {
        Log("D3D10 frame generation setup failed: %s", err.c_str());
        bridge_.reset();
        return false;
    }
    if (FAILED(dev10_->OpenSharedResource(bridge_->InputHandle(), IID_PPV_ARGS(&in10_)))) {
        Log("D3D10 could not open the shared input texture");
        bridge_.reset();
        return false;
    }
    out10_.resize(static_cast<size_t>(bridge_->MaxOutputs()));
    for (int k = 0; k < bridge_->MaxOutputs(); ++k) {
        if (FAILED(dev10_->OpenSharedResource(bridge_->OutputHandle(k), IID_PPV_ARGS(&out10_[k])))) {
            Log("D3D10 could not open a shared output texture");
            bridge_.reset();
            return false;
        }
    }
    return true;
}

HRESULT SwapState::PresentD3D10(const PresentCall& call, const DXGI_SWAP_CHAIN_DESC& desc) {
    if (!EnsureBridge10(desc.BufferDesc.Width, desc.BufferDesc.Height, desc.BufferDesc.Format)) {
        failed = true;
        return call.Real(call.sync, call.flags);
    }
    UINT sync = call.sync, flags = call.flags;
    if (G().cfg.vsyncPacing) {
        if (sync == 0) sync = 1;
        flags &= ~static_cast<UINT>(DXGI_PRESENT_ALLOW_TEARING);
    }
    const UINT genFlags = flags & ~static_cast<UINT>(DXGI_PRESENT_DO_NOT_WAIT | DXGI_PRESENT_RESTART |
                                                     DXGI_PRESENT_DO_NOT_SEQUENCE);

    ComPtr<ID3D10Texture2D> bb;
    if (FAILED(call.sc->GetBuffer(0, IID_PPV_ARGS(&bb)))) return call.Real(call.sync, call.flags);
    dev10_->CopyResource(in10_.Get(), bb.Get());
    Wait10();

    const int n = bridge_->Run();
    for (int k = 0; k < n; ++k) {
        dev10_->CopyResource(bb.Get(), out10_[k].Get());
        bb.Reset();
        const HRESULT hr = call.Generated(sync, genFlags);
        if (FAILED(hr)) return hr;
        if (FAILED(call.sc->GetBuffer(0, IID_PPV_ARGS(&bb)))) return call.Real(sync, flags);
    }
    if (n > 0) dev10_->CopyResource(bb.Get(), in10_.Get());
    bb.Reset();
    return call.Real(sync, flags);
}

HRESULT SwapState::Present(const PresentCall& call) {
    auto& g = G();
    if (failed || !g.fgEnabled.load()) {
        if (fg_) fg_->Reset();
        if (bridge_) bridge_->Reset();
        return call.Real(call.sync, call.flags);
    }

    DXGI_SWAP_CHAIN_DESC desc{};
    if (FAILED(call.sc->GetDesc(&desc))) return call.Real(call.sync, call.flags);
    if (desc.SampleDesc.Count > 1 || !IsSupportedBackbufferFormat(desc.BufferDesc.Format)) {
        if (!warnedFormat_) {
            Log("Unsupported back buffer (format %d, %u samples) - passing through",
                static_cast<int>(desc.BufferDesc.Format), desc.SampleDesc.Count);
            warnedFormat_ = true;
        }
        return call.Real(call.sync, call.flags);
    }
    if (d3d10_) return PresentD3D10(call, desc);
    if (!EnsureFrameGen(desc)) {
        failed = true;
        return call.Real(call.sync, call.flags);
    }

    // With vsync pacing every present waits for its own refresh, so the
    // generated frames are spread evenly and the game is naturally held to
    // refresh / multiplier real fps.
    UINT sync = call.sync, flags = call.flags;
    if (g.cfg.vsyncPacing) {
        if (sync == 0) sync = 1;
        flags &= ~static_cast<UINT>(DXGI_PRESENT_ALLOW_TEARING);
    }
    const UINT genFlags = flags & ~static_cast<UINT>(DXGI_PRESENT_DO_NOT_WAIT | DXGI_PRESENT_RESTART |
                                                     DXGI_PRESENT_DO_NOT_SEQUENCE);

    ComPtr<ID3D11Texture2D> bb;
    if (!BeginFrame(call.sc, bb)) return call.Real(call.sync, call.flags);

    const int n = fg_->Process(ctx_.Get(), bb.Get());
    for (int i = 0; i < n; ++i) {
        WriteGenerated(i, bb.Get());
        EndFrame();
        bb.Reset();
        const HRESULT hr = call.Generated(sync, genFlags);
        if (FAILED(hr)) return hr;
        if (!BeginFrame(call.sc, bb)) return call.Real(sync, flags);
    }
    if (n > 0) ctx_->CopyResource(bb.Get(), fg_->CurrentFrame()); // put the real frame back
    EndFrame();
    bb.Reset();
    return call.Real(sync, flags);
}

std::mutex g_stateMutex;
std::unordered_map<IDXGISwapChain*, std::unique_ptr<SwapState>> g_states;

HRESULT HandlePresent(const PresentCall& call) {
    if (InHook() || (call.flags & DXGI_PRESENT_TEST)) return call.Real(call.sync, call.flags);
    InHook() = true;
    PollHotkeys();

    SwapState* state = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        auto& slot = g_states[call.sc];
        if (slot && !slot->SameDevice(call.sc)) slot.reset();
        if (!slot) {
            slot = std::make_unique<SwapState>();
            if (!slot->Init(call.sc)) slot->failed = true;
        }
        state = slot.get();
    }
    const HRESULT hr = state->Present(call);
    InHook() = false;
    return hr;
}

void OnResize(IDXGISwapChain* sc) {
    std::lock_guard<std::mutex> lock(g_stateMutex);
    auto it = g_states.find(sc);
    if (it != g_states.end() && it->second) it->second->ReleaseBuffers();
}

HRESULT STDMETHODCALLTYPE HookPresent(IDXGISwapChain* sc, UINT sync, UINT flags) {
    return HandlePresent({sc, sync, flags, nullptr, FindVt(sc), false});
}

HRESULT STDMETHODCALLTYPE HookPresent1(IDXGISwapChain1* sc, UINT sync, UINT flags,
                                       const DXGI_PRESENT_PARAMETERS* params) {
    return HandlePresent({sc, sync, flags, params, FindVt(sc), true});
}

HRESULT STDMETHODCALLTYPE HookResizeBuffers(IDXGISwapChain* sc, UINT count, UINT w, UINT h,
                                            DXGI_FORMAT fmt, UINT flags) {
    OnResize(sc);
    return FindVt(sc)->resize(sc, count, w, h, fmt, flags);
}

HRESULT STDMETHODCALLTYPE HookResizeBuffers1(IDXGISwapChain3* sc, UINT count, UINT w, UINT h,
                                             DXGI_FORMAT fmt, UINT flags, const UINT* mask,
                                             IUnknown* const* queues) {
    OnResize(sc);
    return FindVt(sc)->resize1(sc, count, w, h, fmt, flags, mask, queues);
}

// ---------------------------------------------------------------------------
// Installing the hooks

void WriteSlot(void** vt, int idx, void* fn) {
    DWORD old = 0;
    if (!VirtualProtect(&vt[idx], sizeof(void*), PAGE_READWRITE, &old)) return;
    vt[idx] = fn;
    VirtualProtect(&vt[idx], sizeof(void*), old, &old);
}

void HookSwapChain(IDXGISwapChain* sc) {
    void** vt = *reinterpret_cast<void***>(sc);
    const int n = g_vtCount.load();
    for (int i = 0; i < n; ++i)
        if (g_vt[i].vt == vt) return;
    if (n >= kMaxVtables) return;

    ComPtr<IDXGISwapChain1> sc1;
    ComPtr<IDXGISwapChain3> sc3;
    const bool has1 = SUCCEEDED(sc->QueryInterface(IID_PPV_ARGS(&sc1)));
    const bool has3 = SUCCEEDED(sc->QueryInterface(IID_PPV_ARGS(&sc3)));

    // Record the originals and publish them before any slot is redirected.
    VtHooks& h = g_vt[n];
    h.vt = vt;
    h.present = reinterpret_cast<PresentFn>(vt[kSlotPresent]);
    h.resize = reinterpret_cast<ResizeBuffersFn>(vt[kSlotResizeBuffers]);
    if (has1) h.present1 = reinterpret_cast<Present1Fn>(vt[kSlotPresent1]);
    if (has3) h.resize1 = reinterpret_cast<ResizeBuffers1Fn>(vt[kSlotResizeBuffers1]);
    g_vtCount = n + 1;

    WriteSlot(vt, kSlotPresent, reinterpret_cast<void*>(&HookPresent));
    WriteSlot(vt, kSlotResizeBuffers, reinterpret_cast<void*>(&HookResizeBuffers));
    if (has1) WriteSlot(vt, kSlotPresent1, reinterpret_cast<void*>(&HookPresent1));
    if (has3) WriteSlot(vt, kSlotResizeBuffers1, reinterpret_cast<void*>(&HookResizeBuffers1));
    Log("Hooked swap chain vtable %p (Present1: %s, ResizeBuffers1: %s)", static_cast<void*>(vt),
        has1 ? "yes" : "no", has3 ? "yes" : "no");
}

void HookFromDevice(IDXGIFactory2* factory, IUnknown* deviceOrQueue, bool legacyToo) {
    if (HWND wnd = CreateDummyWindow()) {
        DXGI_SWAP_CHAIN_DESC1 d{};
        d.Width = 64;
        d.Height = 64;
        d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        d.BufferCount = 2;
        d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        ComPtr<IDXGISwapChain1> sc;
        if (SUCCEEDED(factory->CreateSwapChainForHwnd(deviceOrQueue, wnd, &d, nullptr, nullptr, &sc)))
            HookSwapChain(sc.Get());
        sc.Reset();
        DestroyWindow(wnd);
    }
    if (!legacyToo) return;
    if (HWND wnd = CreateDummyWindow()) {
        DXGI_SWAP_CHAIN_DESC d{};
        d.BufferDesc.Width = 64;
        d.BufferDesc.Height = 64;
        d.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        d.BufferCount = 1;
        d.OutputWindow = wnd;
        d.Windowed = TRUE;
        d.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        ComPtr<IDXGISwapChain> sc;
        if (SUCCEEDED(factory->CreateSwapChain(deviceOrQueue, &d, &sc))) HookSwapChain(sc.Get());
        sc.Reset();
        DestroyWindow(wnd);
    }
}

} // namespace

HWND CreateDummyWindow() {
    static ATOM cls = 0;
    if (!cls) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = G().self;
        wc.lpszClassName = L"NitroDummy";
        cls = RegisterClassExW(&wc);
    }
    return CreateWindowExW(0, L"NitroDummy", L"", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr,
                           nullptr, G().self, nullptr);
}

void InstallDxgiHooks() {
    using CreateFactory1Fn = HRESULT(WINAPI*)(REFIID, void**);
    HMODULE dxgi = SystemModule(L"dxgi.dll");
    auto createFactory =
        dxgi ? reinterpret_cast<CreateFactory1Fn>(GetProcAddress(dxgi, "CreateDXGIFactory1")) : nullptr;
    if (!createFactory) {
        Log("CreateDXGIFactory1 not found in system dxgi.dll");
        return;
    }
    ComPtr<IDXGIFactory2> factory;
    if (FAILED(createFactory(IID_PPV_ARGS(&factory)))) {
        Log("Could not create a DXGI factory");
        return;
    }

    HMODULE d3d11 = SystemModule(L"d3d11.dll");
    auto createDevice =
        d3d11 ? reinterpret_cast<PFN_D3D11_CREATE_DEVICE>(GetProcAddress(d3d11, "D3D11CreateDevice"))
              : nullptr;
    if (createDevice) {
        ComPtr<ID3D11Device> dev;
        HRESULT hr = createDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                  D3D11_SDK_VERSION, &dev, nullptr, nullptr);
        if (FAILED(hr))
            hr = createDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
                              D3D11_SDK_VERSION, &dev, nullptr, nullptr);
        if (SUCCEEDED(hr)) HookFromDevice(factory.Get(), dev.Get(), true);
    }

    if (G().cfg.probeD3D12) {
        if (HMODULE d3d12 = SystemModule(L"d3d12.dll")) {
            auto create12 =
                reinterpret_cast<PFN_D3D12_CREATE_DEVICE>(GetProcAddress(d3d12, "D3D12CreateDevice"));
            ComPtr<ID3D12Device> d12;
            if (create12 && SUCCEEDED(create12(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&d12)))) {
                D3D12_COMMAND_QUEUE_DESC qd{};
                qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
                ComPtr<ID3D12CommandQueue> q;
                if (SUCCEEDED(d12->CreateCommandQueue(&qd, IID_PPV_ARGS(&q))))
                    HookFromDevice(factory.Get(), q.Get(), false);
            }
        }
    }

    Log("%d swap chain vtable(s) hooked", g_vtCount.load());
}

} // namespace lsfgi

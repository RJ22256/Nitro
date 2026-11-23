// DirectX 9 / 9Ex hooks. D3D9 can't share textures with D3D11 (plain D3D9 has
// no shared handles at all), so frames go through system memory: copy the back
// buffer out, run frame gen on Nitro's D3D11 device, copy the results back.
// DX9 games are light enough that this costs little.

#include <d3d9.h>
#include <wrl/client.h>

#include <MinHook.h>

#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "../core/bridge.h"
#include "../core/log.h"
#include "../core/overlay.h"
#include "../core/runtime.h"
#include "hooks.h"

namespace lsfgi {

namespace {

using Microsoft::WRL::ComPtr;

// IDirect3DDevice9 / IDirect3DDevice9Ex / IDirect3DSwapChain9 vtable slots
constexpr int kDevReset = 16;
constexpr int kDevPresent = 17;
constexpr int kDevPresentEx = 121;
constexpr int kDevResetEx = 132;
constexpr int kScPresent = 3;

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND,
                                              const RGNDATA*);
using PresentExFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9Ex*, const RECT*, const RECT*, HWND,
                                                const RGNDATA*, DWORD);
using ScPresentFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DSwapChain9*, const RECT*, const RECT*, HWND,
                                                const RGNDATA*, DWORD);
using ResetFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
using ResetExFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9Ex*, D3DPRESENT_PARAMETERS*,
                                              D3DDISPLAYMODEEX*);

// [0] = plain D3D9 device class, [1] = D3D9Ex device class (may be the same code)
PresentFn g_present[2] = {};
ResetFn g_reset[2] = {};
ScPresentFn g_scPresent[2] = {};
PresentExFn g_presentEx = nullptr;
ResetExFn g_resetEx = nullptr;

class D9State {
public:
    HRESULT Present(IDirect3DDevice9* dev, const std::function<HRESULT(IDirect3DSurface9**)>& getBB,
                    const std::function<HRESULT()>& present);
    void Release();

private:
    bool Ensure(IDirect3DDevice9* dev, const D3DSURFACE_DESC& d);

    ComPtr<IDirect3DSurface9> copy_;   // real frame (D3DPOOL_DEFAULT render target)
    ComPtr<IDirect3DSurface9> sysIn_;  // system-memory readback of the real frame
    ComPtr<IDirect3DSurface9> sysOut_; // system-memory upload of a generated frame
    ComPtr<IDirect3DSurface9> gpuOut_; // D3DPOOL_DEFAULT copy of a generated frame
    std::unique_ptr<Bridge> bridge_;
    UINT w_ = 0, h_ = 0;
    D3DFORMAT fmt_ = D3DFMT_UNKNOWN;
    bool failed_ = false;
    bool warned_ = false;
};

std::mutex g_mutex;
std::unordered_map<IDirect3DDevice9*, std::unique_ptr<D9State>> g_states;

bool D9State::Ensure(IDirect3DDevice9* dev, const D3DSURFACE_DESC& d) {
    const int mult = G().multiplier.load();
    if (bridge_ && d.Width == w_ && d.Height == h_ && d.Format == fmt_ &&
        bridge_->Matches(d.Width, d.Height, DXGI_FORMAT_B8G8R8A8_UNORM, mult))
        return true;
    Release();

    LUID luid{};
    bool haveLuid = false;
    ComPtr<IDirect3D9> d3d;
    D3DDEVICE_CREATION_PARAMETERS cp{};
    ComPtr<IDirect3D9Ex> d3dEx;
    if (SUCCEEDED(dev->GetDirect3D(&d3d)) && SUCCEEDED(dev->GetCreationParameters(&cp)) &&
        SUCCEEDED(d3d.As(&d3dEx)) && SUCCEEDED(d3dEx->GetAdapterLUID(cp.AdapterOrdinal, &luid)))
        haveLuid = true;

    bridge_ = std::make_unique<Bridge>();
    BridgeParams p;
    p.adapter = haveLuid ? &luid : nullptr;
    p.width = d.Width;
    p.height = d.Height;
    p.format = DXGI_FORMAT_B8G8R8A8_UNORM; // same memory layout as D3DFMT_A8R8G8B8 / X8R8G8B8
    p.share = ShareMode::None;
    std::string err;
    if (!bridge_->Init(p, err)) {
        Log("DX9 frame generation setup failed: %s", err.c_str());
        bridge_.reset();
        return false;
    }
    if (FAILED(dev->CreateRenderTarget(d.Width, d.Height, d.Format, D3DMULTISAMPLE_NONE, 0, FALSE,
                                       &copy_, nullptr)) ||
        FAILED(dev->CreateOffscreenPlainSurface(d.Width, d.Height, d.Format, D3DPOOL_SYSTEMMEM, &sysIn_,
                                                nullptr)) ||
        FAILED(dev->CreateOffscreenPlainSurface(d.Width, d.Height, d.Format, D3DPOOL_SYSTEMMEM, &sysOut_,
                                                nullptr)) ||
        FAILED(dev->CreateOffscreenPlainSurface(d.Width, d.Height, d.Format, D3DPOOL_DEFAULT, &gpuOut_,
                                                nullptr))) {
        Log("DX9 surface creation failed");
        Release();
        return false;
    }
    w_ = d.Width;
    h_ = d.Height;
    fmt_ = d.Format;
    Log("DX9 frame generation ready: %ux%u, %dx", w_, h_, mult);
    return true;
}

void D9State::Release() {
    copy_.Reset();
    sysIn_.Reset();
    sysOut_.Reset();
    gpuOut_.Reset();
    bridge_.reset();
}

HRESULT D9State::Present(IDirect3DDevice9* dev, const std::function<HRESULT(IDirect3DSurface9**)>& getBB,
                         const std::function<HRESULT()>& present) {
    const bool generate = G().fgEnabled.load();
    const bool menu = OverlayVisible();
    if (!generate && bridge_) bridge_->Reset();
    if (failed_ || (!generate && !menu)) {
        NoteFrame(0, "DirectX 9", w_, h_);
        return present();
    }

    ComPtr<IDirect3DSurface9> bb;
    D3DSURFACE_DESC d{};
    if (FAILED(getBB(&bb)) || FAILED(bb->GetDesc(&d))) return present();
    if (d.Format != D3DFMT_X8R8G8B8 && d.Format != D3DFMT_A8R8G8B8) {
        if (!warned_) Log("DX9 back buffer format %d not supported - passing through", static_cast<int>(d.Format));
        warned_ = true;
        return present();
    }
    if (!Ensure(dev, d)) {
        failed_ = true;
        return present();
    }

    // Real frame -> system memory -> Nitro's D3D11 device
    if (FAILED(dev->StretchRect(bb.Get(), nullptr, copy_.Get(), nullptr, D3DTEXF_NONE)) ||
        FAILED(dev->GetRenderTargetData(copy_.Get(), sysIn_.Get())))
        return present();
    D3DLOCKED_RECT lr{};
    if (FAILED(sysIn_->LockRect(&lr, nullptr, D3DLOCK_READONLY))) return present();
    bridge_->Upload(lr.pBits, static_cast<UINT>(lr.Pitch));
    sysIn_->UnlockRect();

    bool drawMenu = false;
    if (menu) {
        D3DDEVICE_CREATION_PARAMETERS cp{};
        HWND wnd = SUCCEEDED(dev->GetCreationParameters(&cp)) && cp.hFocusWindow ? cp.hFocusWindow : FindMainWindow();
        drawMenu = OverlayBegin(bridge_->Device(), bridge_->Context(), wnd, w_, h_);
    }
    const int n = drawMenu ? bridge_->Run(generate, [](ID3D11DeviceContext* c, ID3D11Texture2D* t) { OverlayDraw(c, t); })
                           : bridge_->Run(generate);
    NoteFrame(n, "DirectX 9", w_, h_);
    for (int k = 0; k < n; ++k) {
        if (FAILED(sysOut_->LockRect(&lr, nullptr, 0))) break;
        const bool ok = bridge_->Download(k, lr.pBits, static_cast<UINT>(lr.Pitch), w_ * 4, h_);
        sysOut_->UnlockRect();
        if (!ok || FAILED(dev->UpdateSurface(sysOut_.Get(), nullptr, gpuOut_.Get(), nullptr))) break;

        bb.Reset();
        if (FAILED(getBB(&bb))) break;
        dev->StretchRect(gpuOut_.Get(), nullptr, bb.Get(), nullptr, D3DTEXF_NONE);
        bb.Reset();
        const HRESULT hr = present();
        if (FAILED(hr)) return hr;
    }

    // Put the real frame back (with the menu on it, if open) and present it
    bb.Reset();
    if (drawMenu) {
        if (SUCCEEDED(sysOut_->LockRect(&lr, nullptr, 0))) {
            const bool ok = bridge_->Download(-1, lr.pBits, static_cast<UINT>(lr.Pitch), w_ * 4, h_);
            sysOut_->UnlockRect();
            if (ok && SUCCEEDED(dev->UpdateSurface(sysOut_.Get(), nullptr, gpuOut_.Get(), nullptr)) &&
                SUCCEEDED(getBB(&bb)))
                dev->StretchRect(gpuOut_.Get(), nullptr, bb.Get(), nullptr, D3DTEXF_NONE);
        }
    } else if (n > 0 && SUCCEEDED(getBB(&bb))) {
        dev->StretchRect(copy_.Get(), nullptr, bb.Get(), nullptr, D3DTEXF_NONE);
    }
    bb.Reset();
    return present();
}

D9State* StateFor(IDirect3DDevice9* dev) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto& slot = g_states[dev];
    if (!slot) slot = std::make_unique<D9State>();
    return slot.get();
}

HRESULT Handle(IDirect3DDevice9* dev, const std::function<HRESULT(IDirect3DSurface9**)>& getBB,
               const std::function<HRESULT()>& present) {
    if (InHook()) return present();
    InHook() = true;
    PollHotkeys();
    const HRESULT hr = StateFor(dev)->Present(dev, getBB, present);
    InHook() = false;
    return hr;
}

void OnReset(IDirect3DDevice9* dev) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_states.find(dev);
    if (it != g_states.end() && it->second) it->second->Release(); // D3DPOOL_DEFAULT must go first
}

template <int I>
HRESULT STDMETHODCALLTYPE HkPresent(IDirect3DDevice9* dev, const RECT* src, const RECT* dst, HWND wnd,
                                    const RGNDATA* dirty) {
    return Handle(
        dev,
        [&](IDirect3DSurface9** bb) { return dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, bb); },
        [&] { return g_present[I](dev, src, dst, wnd, dirty); });
}

HRESULT STDMETHODCALLTYPE HkPresentEx(IDirect3DDevice9Ex* dev, const RECT* src, const RECT* dst, HWND wnd,
                                      const RGNDATA* dirty, DWORD flags) {
    return Handle(
        dev,
        [&](IDirect3DSurface9** bb) { return dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, bb); },
        [&] { return g_presentEx(dev, src, dst, wnd, dirty, flags); });
}

template <int I>
HRESULT STDMETHODCALLTYPE HkScPresent(IDirect3DSwapChain9* sc, const RECT* src, const RECT* dst, HWND wnd,
                                      const RGNDATA* dirty, DWORD flags) {
    ComPtr<IDirect3DDevice9> dev;
    if (FAILED(sc->GetDevice(&dev))) return g_scPresent[I](sc, src, dst, wnd, dirty, flags);
    return Handle(
        dev.Get(), [&](IDirect3DSurface9** bb) { return sc->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, bb); },
        [&] { return g_scPresent[I](sc, src, dst, wnd, dirty, flags); });
}

template <int I>
HRESULT STDMETHODCALLTYPE HkReset(IDirect3DDevice9* dev, D3DPRESENT_PARAMETERS* pp) {
    OnReset(dev);
    return g_reset[I](dev, pp);
}

HRESULT STDMETHODCALLTYPE HkResetEx(IDirect3DDevice9Ex* dev, D3DPRESENT_PARAMETERS* pp, D3DDISPLAYMODEEX* mode) {
    OnReset(dev);
    return g_resetEx(dev, pp, mode);
}

template <typename Fn>
bool Hook(void* target, void* detour, Fn* original) {
    const MH_STATUS st = MH_CreateHook(target, detour, reinterpret_cast<void**>(original));
    if (st == MH_ERROR_ALREADY_CREATED) return false; // same code as an earlier vtable
    if (st != MH_OK) {
        Log("MinHook could not hook a D3D9 function (%d)", static_cast<int>(st));
        return false;
    }
    MH_EnableHook(target);
    return true;
}

template <int I>
void HookDevice(IDirect3DDevice9* dev) {
    void** vt = *reinterpret_cast<void***>(dev);
    Hook(vt[kDevPresent], reinterpret_cast<void*>(&HkPresent<I>), &g_present[I]);
    Hook(vt[kDevReset], reinterpret_cast<void*>(&HkReset<I>), &g_reset[I]);
    ComPtr<IDirect3DSwapChain9> sc;
    if (SUCCEEDED(dev->GetSwapChain(0, &sc))) {
        void** svt = *reinterpret_cast<void***>(sc.Get());
        Hook(svt[kScPresent], reinterpret_cast<void*>(&HkScPresent<I>), &g_scPresent[I]);
    }
}

} // namespace

void InstallD3D9Hooks() {
    HMODULE d3d9 = SystemModule(L"d3d9.dll");
    if (!d3d9) {
        Log("System d3d9.dll not found");
        return;
    }
    using Create9Fn = IDirect3D9*(WINAPI*)(UINT);
    using Create9ExFn = HRESULT(WINAPI*)(UINT, IDirect3D9Ex**);
    auto create9 = reinterpret_cast<Create9Fn>(GetProcAddress(d3d9, "Direct3DCreate9"));
    auto create9Ex = reinterpret_cast<Create9ExFn>(GetProcAddress(d3d9, "Direct3DCreate9Ex"));

    HWND wnd = CreateDummyWindow();
    if (!wnd) return;
    D3DPRESENT_PARAMETERS pp{};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat = D3DFMT_UNKNOWN;
    pp.hDeviceWindow = wnd;
    const DWORD flags = D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_DISABLE_DRIVER_MANAGEMENT;

    int hooked = 0;
    if (create9) {
        ComPtr<IDirect3D9> d3d;
        d3d.Attach(create9(D3D_SDK_VERSION));
        ComPtr<IDirect3DDevice9> dev;
        if (d3d && SUCCEEDED(d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, wnd, flags, &pp, &dev))) {
            HookDevice<0>(dev.Get());
            ++hooked;
        }
    }
    if (create9Ex) {
        ComPtr<IDirect3D9Ex> d3d;
        ComPtr<IDirect3DDevice9Ex> dev;
        if (SUCCEEDED(create9Ex(D3D_SDK_VERSION, &d3d)) &&
            SUCCEEDED(d3d->CreateDeviceEx(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, wnd, flags, &pp, nullptr, &dev))) {
            HookDevice<1>(dev.Get());
            void** vt = *reinterpret_cast<void***>(dev.Get());
            Hook(vt[kDevPresentEx], reinterpret_cast<void*>(&HkPresentEx), &g_presentEx);
            Hook(vt[kDevResetEx], reinterpret_cast<void*>(&HkResetEx), &g_resetEx);
            ++hooked;
        }
    }
    DestroyWindow(wnd);
    Log("D3D9 hooks installed (%d device type(s))", hooked);
}

} // namespace lsfgi

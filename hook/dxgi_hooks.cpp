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

#include <MinHook.h>

#include <cstdio>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "../core/blit.h"
#include "../core/bridge.h"
#include "../core/framegen.h"
#include "../core/fsr.h"
#include "../core/log.h"
#include "../core/overlay.h"
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
using GetBufferFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, REFIID, void**);
using GetDescFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, DXGI_SWAP_CHAIN_DESC*);
using GetDesc1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, DXGI_SWAP_CHAIN_DESC1*);

// IDXGISwapChain / IDXGISwapChain1 / IDXGISwapChain3 vtable slots
constexpr int kSlotPresent = 8;
constexpr int kSlotGetBuffer = 9;
constexpr int kSlotGetDesc = 12;
constexpr int kSlotResizeBuffers = 13;
constexpr int kSlotGetDesc1 = 18;
constexpr int kSlotPresent1 = 22;
constexpr int kSlotResizeBuffers1 = 39;
constexpr int kSlotExecuteCommandLists = 10; // ID3D12CommandQueue

// Hooks are inline (MinHook) on the swap chain's implementation functions,
// so they catch every call, even through vtable copies made by overlays or
// wrappers. Each distinct Present implementation gets its own group; the
// function pointers below are the trampolines that call the original code.
struct VtHooks {
    void* target = nullptr; // the Present implementation this group is for
    PresentFn present = nullptr;
    Present1Fn present1 = nullptr;
    ResizeBuffersFn resize = nullptr;
    ResizeBuffers1Fn resize1 = nullptr;
    GetBufferFn getBuffer = nullptr;
    GetDescFn getDesc = nullptr;
    GetDesc1Fn getDesc1 = nullptr;
};

constexpr int kMaxVtables = 8;
VtHooks g_vt[kMaxVtables];
std::atomic<int> g_vtCount{0};

// The originals to call for this object. Objects whose vtable we don't
// recognise (a wrapper's) are called through their own vtable; that ends in
// our hook, which passes straight through because InHook() is set.
VtHooks FindVt(void* obj) {
    void** vt = *reinterpret_cast<void***>(obj);
    const int n = g_vtCount.load();
    for (int i = 0; i < n; ++i)
        if (g_vt[i].target == vt[kSlotPresent]) return g_vt[i];
    VtHooks d;
    d.present = reinterpret_cast<PresentFn>(vt[kSlotPresent]);
    d.present1 = reinterpret_cast<Present1Fn>(vt[kSlotPresent1]);
    d.resize = reinterpret_cast<ResizeBuffersFn>(vt[kSlotResizeBuffers]);
    d.resize1 = reinterpret_cast<ResizeBuffers1Fn>(vt[kSlotResizeBuffers1]);
    d.getBuffer = reinterpret_cast<GetBufferFn>(vt[kSlotGetBuffer]);
    d.getDesc = reinterpret_cast<GetDescFn>(vt[kSlotGetDesc]);
    d.getDesc1 = reinterpret_cast<GetDesc1Fn>(vt[kSlotGetDesc1]);
    return d;
}

// ---------------------------------------------------------------------------
// D3D12: a swap chain can't tell us which command queue it presents on, and
// D3D11On12 needs exactly that queue. So we note the queue when the game
// creates the swap chain (factory hooks), and as a fallback remember the
// direct queues the game submits work on.

std::mutex g_queueMutex;
// Never destroyed (see g_states below).
auto& g_swapQueues = *new std::unordered_map<IDXGISwapChain*, ComPtr<ID3D12CommandQueue>>();

constexpr int kMaxSeenQueues = 16;
std::atomic<ID3D12CommandQueue*> g_seenQueues[kMaxSeenQueues];

void RememberSwapQueue(IUnknown* swapChain, IUnknown* deviceOrQueue) {
    if (!swapChain || !deviceOrQueue) return;
    ComPtr<ID3D12CommandQueue> q;
    if (FAILED(deviceOrQueue->QueryInterface(IID_PPV_ARGS(&q)))) return;
    ComPtr<IDXGISwapChain> sc;
    if (FAILED(swapChain->QueryInterface(IID_PPV_ARGS(&sc)))) return;
    std::lock_guard<std::mutex> lock(g_queueMutex);
    g_swapQueues[sc.Get()] = q;
}

ComPtr<ID3D12CommandQueue> QueueForSwapChain(IDXGISwapChain* sc) {
    {
        std::lock_guard<std::mutex> lock(g_queueMutex);
        auto it = g_swapQueues.find(sc);
        if (it != g_swapQueues.end()) return it->second;
    }
    // Fallback: a direct queue on the swap chain's device that the game uses.
    ComPtr<ID3D12Device> dev;
    if (FAILED(sc->GetDevice(IID_PPV_ARGS(&dev)))) return nullptr;
    for (auto& slot : g_seenQueues) {
        ID3D12CommandQueue* q = slot.load();
        if (!q) break;
        ComPtr<ID3D12Device> qd;
        if (q->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_DIRECT && SUCCEEDED(q->GetDevice(IID_PPV_ARGS(&qd))) &&
            qd.Get() == dev.Get())
            return q;
    }
    return nullptr;
}

using ExecuteCommandListsFn = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
ExecuteCommandListsFn g_origExecute = nullptr;

void STDMETHODCALLTYPE HookExecuteCommandLists(ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* lists) {
    for (auto& slot : g_seenQueues) {
        ID3D12CommandQueue* cur = slot.load(std::memory_order_relaxed);
        if (cur == q) break;
        if (!cur) {
            ID3D12CommandQueue* expected = nullptr;
            if (slot.compare_exchange_strong(expected, q)) {
                q->AddRef(); // kept for the life of the process
                break;
            }
            if (expected == q) break;
        }
    }
    g_origExecute(q, n, lists);
}

// ---------------------------------------------------------------------------
// D3D12 diagnostics (Diagnostics=1 in Nitro.ini): DRED records what the GPU
// was doing, so a device removal can be traced to the game or to Nitro.

void EnableDred() {
    using GetDebugInterfaceFn = HRESULT(WINAPI*)(REFIID, void**);
    HMODULE d3d12 = LoadLibraryW(L"d3d12.dll");
    auto get = d3d12 ? reinterpret_cast<GetDebugInterfaceFn>(GetProcAddress(d3d12, "D3D12GetDebugInterface")) : nullptr;
    ComPtr<ID3D12DeviceRemovedExtendedDataSettings> dred;
    if (!get || FAILED(get(IID_PPV_ARGS(&dred)))) {
        Log("Diagnostics: DRED isn't available");
        return;
    }
    dred->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
    dred->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
    Log("Diagnostics: D3D12 DRED breadcrumbs and page-fault tracking on");
}

const char* DredName(const char* a, const wchar_t* w, std::string& tmp) {
    if (a && *a) return a;
    if (w && *w) {
        tmp = Narrow(w);
        return tmp.c_str();
    }
    return "(unnamed)";
}

void LogDred(ID3D12Device* device) {
    ComPtr<ID3D12DeviceRemovedExtendedData1> dred;
    if (!device || FAILED(device->QueryInterface(IID_PPV_ARGS(&dred)))) {
        Log("DRED: no data (turn on Diagnostics=1 in Nitro.ini and restart the game)");
        return;
    }
    D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 crumbs{};
    if (SUCCEEDED(dred->GetAutoBreadcrumbsOutput1(&crumbs))) {
        int shown = 0;
        std::string t1, t2;
        for (const D3D12_AUTO_BREADCRUMB_NODE1* n = crumbs.pHeadAutoBreadcrumbNode; n && shown < 12; n = n->pNext) {
            const UINT done = n->pLastBreadcrumbValue ? *n->pLastBreadcrumbValue : 0;
            if (done >= n->BreadcrumbCount) continue; // finished on the GPU
            const int op = done < n->BreadcrumbCount && n->pCommandHistory ? n->pCommandHistory[done] : -1;
            Log("DRED: unfinished command list '%s' on queue '%s': %u of %u ops done, next op %d",
                DredName(n->pCommandListDebugNameA, n->pCommandListDebugNameW, t1),
                DredName(n->pCommandQueueDebugNameA, n->pCommandQueueDebugNameW, t2), done, n->BreadcrumbCount, op);
            ++shown;
        }
        if (!shown) Log("DRED: no unfinished command lists recorded");
    }
    D3D12_DRED_PAGE_FAULT_OUTPUT1 pf{};
    if (SUCCEEDED(dred->GetPageFaultAllocationOutput1(&pf)) && pf.PageFaultVA) {
        Log("DRED: GPU page fault at VA 0x%llx", static_cast<unsigned long long>(pf.PageFaultVA));
        std::string t;
        int k = 0;
        for (const D3D12_DRED_ALLOCATION_NODE1* a = pf.pHeadExistingAllocationNode; a && k < 6; a = a->pNext, ++k)
            Log("DRED:   live allocation there: '%s' (type %d)", DredName(a->ObjectNameA, a->ObjectNameW, t),
                static_cast<int>(a->AllocationType));
        k = 0;
        for (const D3D12_DRED_ALLOCATION_NODE1* a = pf.pHeadRecentFreedAllocationNode; a && k < 6; a = a->pNext, ++k)
            Log("DRED:   recently freed there: '%s' (type %d)", DredName(a->ObjectNameA, a->ObjectNameW, t),
                static_cast<int>(a->AllocationType));
    } else {
        Log("DRED: no GPU page fault recorded");
    }
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
    bool retry_ = false; // Init failed only because the D3D12 queue wasn't known yet
    bool loggedFirstGen_ = false;

    bool Init(IDXGISwapChain* sc);
    bool SameDevice(IDXGISwapChain* sc) const;
    HRESULT Present(const PresentCall& call);
    void ReleaseBuffers();

    // FSR 1 upscaling: the game renders into stand-in ("proxy") back buffers
    // at the size it asked for, and Nitro upscales them into the real,
    // window-sized buffers.
    bool Scaling() const { return scaling_; }
    HRESULT GetProxy(IDXGISwapChain* sc, UINT idx, REFIID riid, void** out);
    void AdjustSize(UINT& w, UINT& h) const {
        if (scaling_ && gameW_ && gameH_) {
            w = gameW_;
            h = gameH_;
        }
    }
    void OnGameResize(UINT w, UINT h, HWND wnd);

private:
    HRESULT PresentD3D10(const PresentCall& call, const DXGI_SWAP_CHAIN_DESC& desc);
    bool EnsureFsr();
    void EnsureRealSize(IDXGISwapChain* sc, DXGI_SWAP_CHAIN_DESC& desc);
    ID3D11Texture2D* ProxyForFrame(IDXGISwapChain* sc);
    void ScalePass(IDXGISwapChain* sc, ID3D11Texture2D* bb, const DXGI_SWAP_CHAIN_DESC& desc);
    bool EnsureBridge10(UINT w, UINT h, DXGI_FORMAT fmt);
    void Wait10();
    // D3D12 without D3D11On12: frame gen runs on Nitro's own D3D11 device and
    // frames move through shared textures, copied on the game's queue.
    HRESULT PresentD3D12(const PresentCall& call, const DXGI_SWAP_CHAIN_DESC& desc);
    bool EnsureBridge12(UINT w, UINT h, DXGI_FORMAT fmt);
    bool InitCopier12();
    // Records barriers + CopyResource on the game's queue. Shared textures sit
    // in COMMON; back buffers in PRESENT (also 0). wait = block the CPU until done.
    bool Copy12(ID3D12Resource* dst, ID3D12Resource* src, bool wait);
    void Wait12(UINT64 value);
    ComPtr<ID3D12Resource> CurrentBackBuffer12(IDXGISwapChain* sc);

    bool EnsureFrameGen(const DXGI_SWAP_CHAIN_DESC& desc);
    bool BeginFrame(IDXGISwapChain* sc, ComPtr<ID3D11Texture2D>& bb);
    void EndFrame();
    void WriteGenerated(int i, ID3D11Texture2D* bb);
    std::string DeviceRemovedReason() {
        HRESULT r = S_OK;
        if (queue_) {
            ComPtr<ID3D12Device> d;
            if (SUCCEEDED(queue_->GetDevice(IID_PPV_ARGS(&d)))) r = d->GetDeviceRemovedReason();
        } else if (dev_) {
            r = dev_->GetDeviceRemovedReason();
        }
        if (r == S_OK) return {};
        char buf[48];
        snprintf(buf, sizeof(buf), ", device removed: 0x%08X", static_cast<unsigned>(r));
        return buf;
    }

    bool d3d12_ = false;
    bool d3d10_ = false;
    bool fgFailed_ = false;
    IUnknown* deviceKey_ = nullptr;
    VtHooks vt_;

    // FSR 1
    Fsr fsr_;
    bool fsrTried_ = false, fsrReady_ = false;
    bool scaling_ = false;
    UINT gameW_ = 0, gameH_ = 0;
    ComPtr<ID3D11Texture2D> proxy11_;
    std::vector<ComPtr<ID3D12Resource>> proxy12_;
    std::unordered_map<ID3D12Resource*, ComPtr<ID3D11Texture2D>> wrappedProxy_;
    ID3D11Texture2D* acquiredProxy_ = nullptr;

    // D3D10 games: frame gen runs on Nitro's own D3D11 device (shared textures)
    ComPtr<ID3D10Device> dev10_;
    ComPtr<ID3D10Query> query10_;
    LUID luid_{};
    std::unique_ptr<Bridge> bridge_;
    ComPtr<ID3D10Texture2D> in10_, real10_;
    std::vector<ComPtr<ID3D10Texture2D>> out10_;
    bool bridged12_ = false;
    ComPtr<ID3D12Device> d12_;
    ComPtr<ID3D12Resource> in12_, real12_;
    std::vector<ComPtr<ID3D12Resource>> out12_;
    static constexpr int kAllocators = 6;
    ComPtr<ID3D12CommandAllocator> alloc12_[kAllocators];
    UINT64 allocDone12_[kAllocators] = {};
    int nextAlloc12_ = 0;
    ComPtr<ID3D12GraphicsCommandList> list12_;
    ComPtr<ID3D12Fence> fence12_;
    UINT64 fenceValue12_ = 0;
    HANDLE fenceEvent12_ = nullptr;
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
    UINT w_ = 0, h_ = 0;     // current back buffer size
    UINT fgW_ = 0, fgH_ = 0; // size the frame generator was built for
    int mult_ = 0;
    int version_ = -1;
    bool warnedFormat_ = false;
};

bool SwapState::Init(IDXGISwapChain* sc) {
    vt_ = FindVt(sc);
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
        ComPtr<ID3D12Device> d12;
        if (FAILED(sc->GetDevice(IID_PPV_ARGS(&d12)))) {
            Log("Swap chain is neither D3D11 nor D3D12 - skipping");
            return false;
        }
        queue_ = QueueForSwapChain(sc);
        if (!queue_) {
            // Try again on a later frame: the game may not have submitted work yet.
            Log("D3D12 swap chain: its command queue isn't known yet");
            retry_ = true;
            return false;
        }

        // Default: Nitro's own D3D11 device + shared textures. D3D11On12 on the
        // game's queue is faster but some games/drivers remove the device when
        // it's used around their present (TLOU Part II: 0x887A002B). FSR
        // upscaling needs D3D11On12.
        if (!G().cfg.d3d11On12 && G().cfg.fsrMode != 2) {
            d12_ = d12;
            luid_ = d12->GetAdapterLuid();
            if (!InitCopier12()) return false;
            d3d12_ = true;
            bridged12_ = true;
            deviceKey_ = queue_.Get();
            Log("Attached to a D3D12 swap chain (frame gen on Nitro's own D3D11 device)");
            DXGI_SWAP_CHAIN_DESC d{};
            if (SUCCEEDED(FindVt(sc).getDesc(sc, &d)))
                Log("Swap chain: %u buffers, swap effect %d, flags 0x%X, %s", d.BufferCount,
                    static_cast<int>(d.SwapEffect), d.Flags, d.Windowed ? "windowed/borderless" : "exclusive fullscreen");
            if (GetModuleHandleW(L"nvngx_dlssg.dll"))
                Log("Note: DLSS Frame Generation is loaded in this game. Turn it off in the game's settings, or Nitro "
                    "and DLSS FG will both present frames and the game can crash.");
            return true;
        }

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
        {
            DXGI_SWAP_CHAIN_DESC d{};
            if (SUCCEEDED(FindVt(sc).getDesc(sc, &d)))
                Log("Swap chain: %u buffers, swap effect %d, flags 0x%X, %s", d.BufferCount,
                    static_cast<int>(d.SwapEffect), d.Flags, d.Windowed ? "windowed/borderless" : "exclusive fullscreen");
        }
        if (GetModuleHandleW(L"sl.pcl.dll") || GetModuleHandleW(L"sl.reflex.dll"))
            Log("Note: NVIDIA Reflex (Streamline) is loaded. If the game crashes with frame generation on, set "
                "Reflex to Off in the game's settings.");
        // Game frame generation on top of Nitro's makes two presenters fight
        // over the same swap chain.
        if (GetModuleHandleW(L"nvngx_dlssg.dll"))
            Log("Note: DLSS Frame Generation is loaded in this game. Turn it off in the game's settings, or Nitro "
                "and DLSS FG will both present frames and the game can crash.");
    }

    std::string err;
    blitReady_ = blit_.Init(dev_.Get(), err);
    scaling_ = G().cfg.fsrMode == 2 && EnsureFsr();
    if (scaling_) Log("FSR 1 upscaling on for this swap chain");
    if (!blitReady_) Log("Blitter unavailable (%s); only RGBA/10-bit/FP16 swap chains will work", err.c_str());
    return true;
}

bool SwapState::SameDevice(IDXGISwapChain* sc) const {
    if (failed) return true;
    if (d3d10_) {
        ComPtr<ID3D10Device> d;
        return SUCCEEDED(sc->GetDevice(IID_PPV_ARGS(&d))) && d.Get() == deviceKey_;
    }
    if (d3d12_) return QueueForSwapChain(sc).Get() == deviceKey_;
    ComPtr<ID3D11Device> d;
    return SUCCEEDED(sc->GetDevice(IID_PPV_ARGS(&d))) && d.Get() == deviceKey_;
}

bool SwapState::EnsureFrameGen(const DXGI_SWAP_CHAIN_DESC& d) {
    auto& g = G();
    const int mult = g.multiplier.load();
    const UINT w = d.BufferDesc.Width, h = d.BufferDesc.Height;
    const DXGI_FORMAT fmt = d.BufferDesc.Format;
    const int version = g.settingsVersion.load();
    if (fg_ && w == fgW_ && h == fgH_ && fmt == bbFormat_ && mult == mult_ && version == version_) return true;

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
    fgW_ = w;
    fgH_ = h;
    version_ = version;
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
        if (FAILED(vt_.getBuffer(sc, 0, IID_PPV_ARGS(&bb)))) return false;
        ctx1_->SwapDeviceContextState(ourState_.Get(), &savedState_);
        return true;
    }

    ComPtr<IDXGISwapChain3> sc3;
    if (FAILED(sc->QueryInterface(IID_PPV_ARGS(&sc3)))) return false;
    const UINT idx = sc3->GetCurrentBackBufferIndex();
    ComPtr<ID3D12Resource> res;
    if (FAILED(vt_.getBuffer(sc, idx, IID_PPV_ARGS(&res)))) return false;

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
    wrappedProxy_.clear();
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
    real10_.Reset();
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
    if (FAILED(dev10_->OpenSharedResource(bridge_->InputHandle(), IID_PPV_ARGS(&in10_))) ||
        FAILED(dev10_->OpenSharedResource(bridge_->RealOutputHandle(), IID_PPV_ARGS(&real10_)))) {
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
    const bool generate = G().fgEnabled.load();
    const bool menu = OverlayVisible();
    w_ = desc.BufferDesc.Width;
    h_ = desc.BufferDesc.Height;
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
    if (FAILED(vt_.getBuffer(call.sc, 0, IID_PPV_ARGS(&bb)))) return call.Real(call.sync, call.flags);
    dev10_->CopyResource(in10_.Get(), bb.Get());
    Wait10();

    bool drawMenu = false;
    if (menu)
        drawMenu = OverlayBegin(bridge_->Device(), bridge_->Context(), desc.OutputWindow, w_, h_);
    const FramePlan plan = PlanFrame(desc.OutputWindow);
    const int n = bridge_->Run(generate, drawMenu ? Bridge::OverlayFn([](ID3D11DeviceContext* c, ID3D11Texture2D* t) { OverlayDraw(c, t); }) : Bridge::OverlayFn{},
                               plan.gens, plan.denom);
    NoteFrame(n, "DirectX 10", w_, h_);
    for (int k = 0; k < n; ++k) {
        dev10_->CopyResource(bb.Get(), out10_[k].Get());
        bb.Reset();
        const HRESULT hr = call.Generated(sync, genFlags);
        if (FAILED(hr)) {
            Log("Presenting a generated frame failed (0x%08X) - frame generation off for this game window",
                static_cast<unsigned>(hr));
            fgFailed_ = true;
            return call.Real(call.sync, call.flags);
        }
        if (FAILED(vt_.getBuffer(call.sc, 0, IID_PPV_ARGS(&bb)))) return call.Real(sync, flags);
    }
    if (bridge_->RealOutputUsed())
        dev10_->CopyResource(bb.Get(), real10_.Get());
    else if (n > 0)
        dev10_->CopyResource(bb.Get(), in10_.Get());
    bb.Reset();
    return call.Real(sync, flags);
}

bool SwapState::InitCopier12() {
    const D3D12_COMMAND_LIST_TYPE type = queue_->GetDesc().Type;
    for (auto& a : alloc12_)
        if (FAILED(d12_->CreateCommandAllocator(type, IID_PPV_ARGS(&a)))) {
            Log("D3D12: could not create a command allocator");
            return false;
        }
    if (FAILED(d12_->CreateCommandList(0, type, alloc12_[0].Get(), nullptr, IID_PPV_ARGS(&list12_))) ||
        FAILED(list12_->Close()) || FAILED(d12_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence12_)))) {
        Log("D3D12: could not create a command list / fence");
        return false;
    }
    fenceEvent12_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    list12_->SetName(L"Nitro copy");
    return fenceEvent12_ != nullptr;
}

void SwapState::Wait12(UINT64 value) {
    if (fence12_->GetCompletedValue() >= value) return;
    if (SUCCEEDED(fence12_->SetEventOnCompletion(value, fenceEvent12_))) WaitForSingleObject(fenceEvent12_, 5000);
}

bool SwapState::Copy12(ID3D12Resource* dst, ID3D12Resource* src, bool wait) {
    const int a = nextAlloc12_;
    nextAlloc12_ = (nextAlloc12_ + 1) % kAllocators;
    Wait12(allocDone12_[a]); // the allocator's previous commands must be finished
    if (FAILED(alloc12_[a]->Reset()) || FAILED(list12_->Reset(alloc12_[a].Get(), nullptr))) return false;
    D3D12_RESOURCE_BARRIER b[2] = {};
    b[0].Type = b[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b[0].Transition.pResource = src;
    b[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON; // == PRESENT
    b[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b[1].Transition.pResource = dst;
    b[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    b[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    list12_->ResourceBarrier(2, b);
    list12_->CopyResource(dst, src);
    std::swap(b[0].Transition.StateBefore, b[0].Transition.StateAfter);
    std::swap(b[1].Transition.StateBefore, b[1].Transition.StateAfter);
    list12_->ResourceBarrier(2, b);
    if (FAILED(list12_->Close())) return false;
    ID3D12CommandList* lists[] = {list12_.Get()};
    queue_->ExecuteCommandLists(1, lists);
    queue_->Signal(fence12_.Get(), ++fenceValue12_);
    allocDone12_[a] = fenceValue12_;
    if (wait) Wait12(fenceValue12_);
    return true;
}

ComPtr<ID3D12Resource> SwapState::CurrentBackBuffer12(IDXGISwapChain* sc) {
    ComPtr<IDXGISwapChain3> sc3;
    ComPtr<ID3D12Resource> res;
    if (SUCCEEDED(sc->QueryInterface(IID_PPV_ARGS(&sc3))))
        vt_.getBuffer(sc, sc3->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&res));
    return res;
}

bool SwapState::EnsureBridge12(UINT w, UINT h, DXGI_FORMAT fmt) {
    const DXGI_FORMAT view = ViewFormat(fmt);
    if (bridge_ && bridge_->Matches(w, h, view, G().multiplier.load())) return true;
    Wait12(fenceValue12_);
    in12_.Reset();
    real12_.Reset();
    out12_.clear();
    bridge_ = std::make_unique<Bridge>();
    BridgeParams p;
    p.adapter = &luid_;
    p.width = w;
    p.height = h;
    p.format = view;
    p.share = ShareMode::NtHandle;
    std::string err;
    if (!bridge_->Init(p, err)) {
        Log("D3D12 frame generation setup failed: %s", err.c_str());
        bridge_.reset();
        return false;
    }
    bool ok = SUCCEEDED(d12_->OpenSharedHandle(bridge_->InputHandle(), IID_PPV_ARGS(&in12_))) &&
              SUCCEEDED(d12_->OpenSharedHandle(bridge_->RealOutputHandle(), IID_PPV_ARGS(&real12_)));
    out12_.resize(static_cast<size_t>(bridge_->MaxOutputs()));
    for (int k = 0; ok && k < bridge_->MaxOutputs(); ++k)
        ok = SUCCEEDED(d12_->OpenSharedHandle(bridge_->OutputHandle(k), IID_PPV_ARGS(&out12_[k])));
    if (!ok) {
        Log("D3D12 could not open Nitro's shared textures");
        bridge_.reset();
        return false;
    }
    Log("Frame generation ready: %ux%u, format %d, %dx (D3D12 via shared textures)", w, h, static_cast<int>(fmt),
        G().multiplier.load());
    return true;
}

HRESULT SwapState::PresentD3D12(const PresentCall& call, const DXGI_SWAP_CHAIN_DESC& desc) {
    const bool generate = G().fgEnabled.load() && !fgFailed_;
    const bool menu = OverlayVisible();
    w_ = desc.BufferDesc.Width;
    h_ = desc.BufferDesc.Height;
    if (!EnsureBridge12(w_, h_, desc.BufferDesc.Format)) {
        failed = true;
        return call.Real(call.sync, call.flags);
    }
    UINT sync = call.sync, flags = call.flags;
    if (G().cfg.vsyncPacing && generate) {
        if (sync == 0) sync = 1;
        flags &= ~static_cast<UINT>(DXGI_PRESENT_ALLOW_TEARING);
    }
    const UINT genFlags = flags & ~static_cast<UINT>(DXGI_PRESENT_DO_NOT_WAIT | DXGI_PRESENT_RESTART |
                                                     DXGI_PRESENT_DO_NOT_SEQUENCE);

    ComPtr<ID3D12Resource> bb = CurrentBackBuffer12(call.sc);
    if (!bb || !Copy12(in12_.Get(), bb.Get(), true)) return call.Real(call.sync, call.flags);
    bb.Reset();

    bool drawMenu = false;
    if (menu) drawMenu = OverlayBegin(bridge_->Device(), bridge_->Context(), desc.OutputWindow, w_, h_);
    const FramePlan plan = PlanFrame(desc.OutputWindow);
    const int n = bridge_->Run(generate,
                               drawMenu ? Bridge::OverlayFn([](ID3D11DeviceContext* c, ID3D11Texture2D* t) {
                                   OverlayDraw(c, t);
                               })
                                        : Bridge::OverlayFn{},
                               plan.gens, plan.denom);
    NoteFrame(n, "DirectX 12", w_, h_);
    for (int k = 0; k < n; ++k) {
        bb = CurrentBackBuffer12(call.sc);
        if (!bb || !Copy12(bb.Get(), out12_[k].Get(), false)) return call.Real(sync, flags);
        bb.Reset();
        const HRESULT hr = call.Generated(sync, genFlags);
        if (FAILED(hr)) {
            Log("Presenting a generated frame failed (0x%08X%s) - frame generation off for this game window",
                static_cast<unsigned>(hr), DeviceRemovedReason().c_str());
            LogDred(d12_.Get());
            fgFailed_ = true;
            return call.Real(call.sync, call.flags);
        }
        if (!loggedFirstGen_) {
            loggedFirstGen_ = true;
            Log("First generated frame shown");
        }
    }
    // After generated frames the current back buffer is a different one, so
    // the real frame (or the sharpened / menu version) goes back in.
    if (bridge_->RealOutputUsed() || n > 0) {
        bb = CurrentBackBuffer12(call.sc);
        if (bb) Copy12(bb.Get(), bridge_->RealOutputUsed() ? real12_.Get() : in12_.Get(), false);
        bb.Reset();
    }
    return call.Real(sync, flags);
}

bool SwapState::EnsureFsr() {
    if (!fsrTried_) {
        fsrTried_ = true;
        std::string err;
        fsrReady_ = fsr_.Init(dev_.Get(), err);
        if (!fsrReady_) Log("FSR 1 unavailable: %s", err.c_str());
    }
    return fsrReady_;
}

HRESULT SwapState::GetProxy(IDXGISwapChain* sc, UINT idx, REFIID riid, void** out) {
    DXGI_SWAP_CHAIN_DESC rd{};
    vt_.getDesc(sc, &rd);
    if (rd.SampleDesc.Count > 1 || !IsSupportedBackbufferFormat(rd.BufferDesc.Format)) {
        Log("FSR upscaling needs a plain RGBA/BGRA/10-bit/FP16 swap chain - off for this game");
        scaling_ = false;
        return vt_.getBuffer(sc, idx, riid, out);
    }
    if (!gameW_ || !gameH_) {
        gameW_ = rd.BufferDesc.Width;
        gameH_ = rd.BufferDesc.Height;
    }
    if (!d3d12_) {
        if (!proxy11_) {
            D3D11_TEXTURE2D_DESC d{};
            d.Width = gameW_;
            d.Height = gameH_;
            d.MipLevels = 1;
            d.ArraySize = 1;
            d.Format = rd.BufferDesc.Format;
            d.SampleDesc.Count = 1;
            d.Usage = D3D11_USAGE_DEFAULT;
            d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            if (FAILED(dev_->CreateTexture2D(&d, nullptr, &proxy11_))) {
                scaling_ = false;
                return vt_.getBuffer(sc, idx, riid, out);
            }
        }
        return proxy11_->QueryInterface(riid, out);
    }
    ComPtr<ID3D12Device> d12;
    if (FAILED(queue_->GetDevice(IID_PPV_ARGS(&d12)))) return vt_.getBuffer(sc, idx, riid, out);
    if (proxy12_.size() < rd.BufferCount) proxy12_.resize(rd.BufferCount);
    if (idx >= proxy12_.size()) return DXGI_ERROR_INVALID_CALL;
    if (!proxy12_[idx]) {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width = gameW_;
        d.Height = gameH_;
        d.DepthOrArraySize = 1;
        d.MipLevels = 1;
        d.Format = rd.BufferDesc.Format;
        d.SampleDesc.Count = 1;
        d.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        if (FAILED(d12->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_PRESENT,
                                                nullptr, IID_PPV_ARGS(&proxy12_[idx]))))
            return E_OUTOFMEMORY;
    }
    return proxy12_[idx]->QueryInterface(riid, out);
}

void SwapState::OnGameResize(UINT w, UINT h, HWND wnd) {
    proxy11_.Reset();
    wrappedProxy_.clear();
    proxy12_.clear();
    RECT rc{};
    GetClientRect(wnd, &rc);
    gameW_ = w ? w : static_cast<UINT>(rc.right - rc.left);
    gameH_ = h ? h : static_cast<UINT>(rc.bottom - rc.top);
}

// The real buffers follow the window; the game only ever sees the proxies.
void SwapState::EnsureRealSize(IDXGISwapChain* sc, DXGI_SWAP_CHAIN_DESC& desc) {
    RECT rc{};
    if (!desc.Windowed || !GetClientRect(desc.OutputWindow, &rc)) return;
    const UINT cw = static_cast<UINT>(rc.right - rc.left), ch = static_cast<UINT>(rc.bottom - rc.top);
    if (!cw || !ch || (cw == desc.BufferDesc.Width && ch == desc.BufferDesc.Height)) return;
    wrapped_.clear();
    if (d3d12_ && ctx_) {
        ctx_->ClearState();
        ctx_->Flush();
    }
    if (SUCCEEDED(vt_.resize(sc, 0, 0, 0, DXGI_FORMAT_UNKNOWN, desc.Flags))) {
        vt_.getDesc(sc, &desc);
        Log("FSR 1: game renders %ux%u, upscaling to %ux%u", gameW_, gameH_, desc.BufferDesc.Width,
            desc.BufferDesc.Height);
    }
}

ID3D11Texture2D* SwapState::ProxyForFrame(IDXGISwapChain* sc) {
    if (!d3d12_) return proxy11_.Get();
    ComPtr<IDXGISwapChain3> sc3;
    if (FAILED(sc->QueryInterface(IID_PPV_ARGS(&sc3)))) return nullptr;
    const UINT idx = sc3->GetCurrentBackBufferIndex();
    if (idx >= proxy12_.size() || !proxy12_[idx]) return nullptr;
    ID3D12Resource* res = proxy12_[idx].Get();
    auto it = wrappedProxy_.find(res);
    if (it == wrappedProxy_.end()) {
        D3D11_RESOURCE_FLAGS flags{};
        flags.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        ComPtr<ID3D11Texture2D> tex;
        if (FAILED(on12_->CreateWrappedResource(res, &flags, D3D12_RESOURCE_STATE_PRESENT,
                                                D3D12_RESOURCE_STATE_PRESENT, IID_PPV_ARGS(&tex))))
            return nullptr;
        it = wrappedProxy_.emplace(res, tex).first;
    }
    ID3D11Resource* r = it->second.Get();
    on12_->AcquireWrappedResources(&r, 1);
    acquiredProxy_ = it->second.Get();
    return acquiredProxy_;
}

void SwapState::ScalePass(IDXGISwapChain* sc, ID3D11Texture2D* bb, const DXGI_SWAP_CHAIN_DESC& desc) {
    ID3D11Texture2D* proxy = ProxyForFrame(sc);
    if (!proxy) return;
    D3D11_TEXTURE2D_DESC pd{};
    proxy->GetDesc(&pd);
    const UINT w = desc.BufferDesc.Width, h = desc.BufferDesc.Height;
    const bool same = pd.Width == w && pd.Height == h;
    if (!fsr_.Apply(ctx_.Get(), proxy, pd.Format, pd.Width, pd.Height, bb, desc.BufferDesc.Format, w, h, !same,
                    G().cfg.fsrSharpness) &&
        same)
        ctx_->CopyResource(bb, proxy);
    if (acquiredProxy_) {
        ID3D11Resource* r = acquiredProxy_;
        on12_->ReleaseWrappedResources(&r, 1);
        acquiredProxy_ = nullptr;
    }
}

HRESULT SwapState::Present(const PresentCall& call) {
    auto& g = G();
    if (!g.fgEnabled.load()) {
        if (fg_) fg_->Reset();
        if (bridge_) bridge_->Reset();
    }
    const bool generate = g.fgEnabled.load() && !fgFailed_;
    const bool menu = OverlayVisible();
    const bool sharpen = g.cfg.fsrMode >= 1;
    if (failed || (!scaling_ && !generate && !menu && !sharpen)) {
        NoteFrame(0, d3d12_ ? "DirectX 12" : d3d10_ ? "DirectX 10" : "DirectX 11", w_, h_);
        return call.Real(call.sync, call.flags);
    }

    DXGI_SWAP_CHAIN_DESC desc{};
    if (FAILED(vt_.getDesc(call.sc, &desc))) return call.Real(call.sync, call.flags);
    if (desc.SampleDesc.Count > 1 || !IsSupportedBackbufferFormat(desc.BufferDesc.Format)) {
        if (!warnedFormat_) {
            Log("Unsupported back buffer (format %d, %u samples) - passing through",
                static_cast<int>(desc.BufferDesc.Format), desc.SampleDesc.Count);
            warnedFormat_ = true;
        }
        return call.Real(call.sync, call.flags);
    }
    if (d3d10_) return PresentD3D10(call, desc);
    if (bridged12_) return PresentD3D12(call, desc);
    if (scaling_) EnsureRealSize(call.sc, desc);
    w_ = desc.BufferDesc.Width;
    h_ = desc.BufferDesc.Height;
    bool gen = generate;
    if (gen && !EnsureFrameGen(desc)) {
        fgFailed_ = true;
        gen = false;
    }

    // With vsync pacing every present waits for its own refresh, so the
    // generated frames are spread evenly and the game is naturally held to
    // refresh / multiplier real fps.
    UINT sync = call.sync, flags = call.flags;
    if (g.cfg.vsyncPacing && gen) {
        if (sync == 0) sync = 1;
        flags &= ~static_cast<UINT>(DXGI_PRESENT_ALLOW_TEARING);
    }
    const UINT genFlags = flags & ~static_cast<UINT>(DXGI_PRESENT_DO_NOT_WAIT | DXGI_PRESENT_RESTART |
                                                     DXGI_PRESENT_DO_NOT_SEQUENCE);

    ComPtr<ID3D11Texture2D> bb;
    if (!BeginFrame(call.sc, bb)) return call.Real(call.sync, call.flags);

    // FSR 1 first, so frame generation works on the finished, full-size image.
    if (scaling_)
        ScalePass(call.sc, bb.Get(), desc);
    else if (sharpen && EnsureFsr())
        fsr_.Apply(ctx_.Get(), bb.Get(), desc.BufferDesc.Format, w_, h_, bb.Get(), desc.BufferDesc.Format, w_, h_,
                   false, g.cfg.fsrSharpness);

    const FramePlan plan = PlanFrame(desc.OutputWindow);
    const int n = gen && fg_ ? fg_->Process(ctx_.Get(), bb.Get(), plan.gens, plan.denom) : 0;
    const char* api = d3d12_ ? "DirectX 12" : "DirectX 11";
    NoteFrame(n, api, w_, h_);
    const bool drawMenu = menu && OverlayBegin(dev_.Get(), ctx_.Get(), desc.OutputWindow, w_, h_);
    for (int i = 0; i < n; ++i) {
        WriteGenerated(i, bb.Get());
        if (drawMenu) OverlayDraw(ctx_.Get(), bb.Get());
        EndFrame();
        bb.Reset();
        const HRESULT hr = call.Generated(sync, genFlags);
        if (FAILED(hr)) {
            // Never hand the game an error it didn't cause: switch frame gen
            // off for this swap chain and show the real frame as usual.
            Log("Presenting a generated frame failed (0x%08X%s) - frame generation off for this game window",
                static_cast<unsigned>(hr), DeviceRemovedReason().c_str());
            if (queue_) {
                ComPtr<ID3D12Device> d;
                if (SUCCEEDED(queue_->GetDevice(IID_PPV_ARGS(&d)))) LogDred(d.Get());
            }
            fgFailed_ = true;
            return call.Real(call.sync, call.flags);
        }
        if (!loggedFirstGen_) {
            loggedFirstGen_ = true;
            Log("First generated frame shown");
        }
        if (!BeginFrame(call.sc, bb)) return call.Real(sync, flags);
    }
    if (n > 0) ctx_->CopyResource(bb.Get(), fg_->CurrentFrame()); // put the real frame back
    if (drawMenu) OverlayDraw(ctx_.Get(), bb.Get());
    EndFrame();
    bb.Reset();
    return call.Real(sync, flags);
}

std::mutex g_stateMutex;
// Never destroyed: releasing D3D objects during process exit (under the
// loader lock) can hang or crash the game as it closes.
auto& g_states = *new std::unordered_map<IDXGISwapChain*, std::unique_ptr<SwapState>>();

SwapState* StateFor(IDXGISwapChain* sc) {
    std::lock_guard<std::mutex> lock(g_stateMutex);
    auto& slot = g_states[sc];
    if (slot && !slot->Scaling() && !slot->SameDevice(sc)) slot.reset();
    static int retries = 0;
    if (slot && slot->retry_ && slot->failed && retries < 600 && QueueForSwapChain(sc)) {
        ++retries;
        slot.reset();
    }
    if (!slot) {
        slot = std::make_unique<SwapState>();
        if (!slot->Init(sc)) slot->failed = true;
    }
    return slot.get();
}

SwapState* FindState(IDXGISwapChain* sc) {
    std::lock_guard<std::mutex> lock(g_stateMutex);
    auto it = g_states.find(sc);
    return it == g_states.end() ? nullptr : it->second.get();
}

HRESULT HandlePresent(const PresentCall& call) {
    static bool loggedCall = false;
    if (!loggedCall && !InHook() && !(call.flags & DXGI_PRESENT_TEST)) {
        loggedCall = true;
        Log("Game presents with sync interval %u, flags 0x%X%s", call.sync, call.flags, call.present1 ? " (Present1)" : "");
    }
    if (InHook() || (call.flags & DXGI_PRESENT_TEST)) return call.Real(call.sync, call.flags);
    InHook() = true;
    PollHotkeys();
    LimitFrameRate();
    SwapState* state = StateFor(call.sc);
    const HRESULT hr = state->Present(call);
    InHook() = false;
    return hr;
}

// Returns true if the swap chain is upscaling (then the real buffers are
// resized to the window and the game's size goes to the proxies).
bool OnResize(IDXGISwapChain* sc, UINT w, UINT h) {
    SwapState* st = FindState(sc);
    if (!st) return false;
    st->ReleaseBuffers();
    if (!st->Scaling()) return false;
    DXGI_SWAP_CHAIN_DESC rd{};
    FindVt(sc).getDesc(sc, &rd);
    st->OnGameResize(w, h, rd.OutputWindow);
    return true;
}

// One set of hook functions per group I; each calls group I's trampolines.
template <int I>
HRESULT STDMETHODCALLTYPE HookGetBuffer(IDXGISwapChain* sc, UINT idx, REFIID riid, void** out) {
    if (!InHook() && G().cfg.fsrMode == 2 && G().ready) {
        SwapState* st = StateFor(sc);
        if (st && st->Scaling()) return st->GetProxy(sc, idx, riid, out);
    }
    return g_vt[I].getBuffer(sc, idx, riid, out);
}

template <int I>
HRESULT STDMETHODCALLTYPE HookGetDesc(IDXGISwapChain* sc, DXGI_SWAP_CHAIN_DESC* d) {
    const HRESULT hr = g_vt[I].getDesc(sc, d);
    if (SUCCEEDED(hr) && d && !InHook())
        if (SwapState* st = FindState(sc)) st->AdjustSize(d->BufferDesc.Width, d->BufferDesc.Height);
    return hr;
}

template <int I>
HRESULT STDMETHODCALLTYPE HookGetDesc1(IDXGISwapChain1* sc, DXGI_SWAP_CHAIN_DESC1* d) {
    const HRESULT hr = g_vt[I].getDesc1(sc, d);
    if (SUCCEEDED(hr) && d && !InHook())
        if (SwapState* st = FindState(sc)) st->AdjustSize(d->Width, d->Height);
    return hr;
}

template <int I>
HRESULT STDMETHODCALLTYPE HookPresent(IDXGISwapChain* sc, UINT sync, UINT flags) {
    return HandlePresent({sc, sync, flags, nullptr, &g_vt[I], false});
}

template <int I>
HRESULT STDMETHODCALLTYPE HookPresent1(IDXGISwapChain1* sc, UINT sync, UINT flags,
                                       const DXGI_PRESENT_PARAMETERS* params) {
    return HandlePresent({sc, sync, flags, params, &g_vt[I], true});
}

template <int I>
HRESULT STDMETHODCALLTYPE HookResizeBuffers(IDXGISwapChain* sc, UINT count, UINT w, UINT h,
                                            DXGI_FORMAT fmt, UINT flags) {
    if (!InHook() && OnResize(sc, w, h)) w = h = 0; // real buffers follow the window
    return g_vt[I].resize(sc, count, w, h, fmt, flags);
}

template <int I>
HRESULT STDMETHODCALLTYPE HookResizeBuffers1(IDXGISwapChain3* sc, UINT count, UINT w, UINT h,
                                             DXGI_FORMAT fmt, UINT flags, const UINT* mask,
                                             IUnknown* const* queues) {
    if (!InHook() && OnResize(sc, w, h)) w = h = 0;
    return g_vt[I].resize1(sc, count, w, h, fmt, flags, mask, queues);
}

struct GroupHooks {
    void* present;
    void* present1;
    void* resize;
    void* resize1;
    void* getBuffer;
    void* getDesc;
    void* getDesc1;
};

template <int I>
constexpr GroupHooks MakeGroup() {
    return {reinterpret_cast<void*>(&HookPresent<I>),       reinterpret_cast<void*>(&HookPresent1<I>),
            reinterpret_cast<void*>(&HookResizeBuffers<I>), reinterpret_cast<void*>(&HookResizeBuffers1<I>),
            reinterpret_cast<void*>(&HookGetBuffer<I>),     reinterpret_cast<void*>(&HookGetDesc<I>),
            reinterpret_cast<void*>(&HookGetDesc1<I>)};
}

const GroupHooks kGroups[kMaxVtables] = {MakeGroup<0>(), MakeGroup<1>(), MakeGroup<2>(), MakeGroup<3>(),
                                         MakeGroup<4>(), MakeGroup<5>(), MakeGroup<6>(), MakeGroup<7>()};

// ---------------------------------------------------------------------------
// Installing the hooks

// Every function we've detoured, so a second group that shares an
// implementation (e.g. GetBuffer) reuses the first group's trampoline.
struct DetourEntry {
    void* target;
    void* trampoline;
};
std::mutex g_detourMutex;
DetourEntry g_detours[64];
int g_detourCount = 0;

// Detours target with hook (once) and stores the pointer that calls the
// original in *orig before the detour goes live.
template <typename Fn>
bool Detour(void* target, void* hook, Fn* orig) {
    std::lock_guard<std::mutex> lock(g_detourMutex);
    for (int i = 0; i < g_detourCount; ++i)
        if (g_detours[i].target == target) {
            *orig = reinterpret_cast<Fn>(g_detours[i].trampoline);
            return true;
        }
    if (g_detourCount >= 64) return false;
    if (MH_CreateHook(target, hook, reinterpret_cast<void**>(orig)) != MH_OK) {
        Log("Could not hook function %p", target);
        return false;
    }
    if (MH_EnableHook(target) != MH_OK) {
        MH_RemoveHook(target);
        Log("Could not enable the hook on %p", target);
        return false;
    }
    g_detours[g_detourCount++] = {target, reinterpret_cast<void*>(*orig)};
    return true;
}

void HookSwapChain(IDXGISwapChain* sc) {
    void** vt = *reinterpret_cast<void***>(sc);
    void* presentTarget = vt[kSlotPresent];
    const int n = g_vtCount.load();
    for (int i = 0; i < n; ++i)
        if (g_vt[i].target == presentTarget) return;
    if (n >= kMaxVtables) return;

    ComPtr<IDXGISwapChain1> sc1;
    ComPtr<IDXGISwapChain3> sc3;
    const bool has1 = SUCCEEDED(sc->QueryInterface(IID_PPV_ARGS(&sc1)));
    const bool has3 = SUCCEEDED(sc->QueryInterface(IID_PPV_ARGS(&sc3)));

    // Fill in every trampoline before the group is published (a detour is
    // live as soon as it's created).
    VtHooks& h = g_vt[n];
    const GroupHooks& fn = kGroups[n];
    h.target = presentTarget;
    bool ok = Detour(vt[kSlotGetBuffer], fn.getBuffer, &h.getBuffer);
    ok &= Detour(vt[kSlotGetDesc], fn.getDesc, &h.getDesc);
    ok &= Detour(vt[kSlotResizeBuffers], fn.resize, &h.resize);
    if (has1) Detour(vt[kSlotGetDesc1], fn.getDesc1, &h.getDesc1);
    if (has3) Detour(vt[kSlotResizeBuffers1], fn.resize1, &h.resize1);
    if (has1) Detour(vt[kSlotPresent1], fn.present1, &h.present1);
    ok &= Detour(presentTarget, fn.present, &h.present);
    if (!ok) {
        Log("Hooking the swap chain failed");
        return;
    }
    g_vtCount = n + 1;
    Log("Hooked swap chain functions (Present %p, Present1: %s, ResizeBuffers1: %s)", presentTarget,
        has1 ? "yes" : "no", has3 ? "yes" : "no");
}

// IDXGIFactory / IDXGIFactory2 vtable slots
constexpr int kSlotCreateSwapChain = 10;
constexpr int kSlotCreateForHwnd = 15;
constexpr int kSlotCreateForCoreWindow = 16;
constexpr int kSlotCreateForComposition = 24;

using CreateSwapChainFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*,
                                                      IDXGISwapChain**);
using CreateForHwndFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*,
                                                    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*,
                                                    IDXGISwapChain1**);
using CreateForCoreWindowFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, IUnknown*,
                                                          const DXGI_SWAP_CHAIN_DESC1*, IDXGIOutput*,
                                                          IDXGISwapChain1**);
using CreateForCompositionFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, const DXGI_SWAP_CHAIN_DESC1*,
                                                           IDXGIOutput*, IDXGISwapChain1**);

CreateSwapChainFn g_origCreate = nullptr;
CreateForHwndFn g_origForHwnd = nullptr;
CreateForCoreWindowFn g_origForCoreWindow = nullptr;
CreateForCompositionFn g_origForComposition = nullptr;

void OnSwapChainCreated(IUnknown* deviceOrQueue, IUnknown* swapChain) {
    if (InHook()) return; // Nitro's own probe swap chains
    RememberSwapQueue(swapChain, deviceOrQueue);
    static std::atomic<int> logged{0};
    if (logged.fetch_add(1) < 8) {
        ComPtr<ID3D12CommandQueue> q;
        Log("Game created a swap chain (%s)",
            deviceOrQueue && SUCCEEDED(deviceOrQueue->QueryInterface(IID_PPV_ARGS(&q))) ? "Direct3D 12" : "Direct3D 10/11");
    }
    ComPtr<IDXGISwapChain> sc;
    if (swapChain && SUCCEEDED(swapChain->QueryInterface(IID_PPV_ARGS(&sc)))) HookSwapChain(sc.Get());
}

HRESULT STDMETHODCALLTYPE HookCreateSwapChain(IDXGIFactory* f, IUnknown* dev, DXGI_SWAP_CHAIN_DESC* d,
                                              IDXGISwapChain** out) {
    const HRESULT hr = g_origCreate(f, dev, d, out);
    if (SUCCEEDED(hr) && out && *out) OnSwapChainCreated(dev, *out);
    return hr;
}

HRESULT STDMETHODCALLTYPE HookCreateForHwnd(IDXGIFactory2* f, IUnknown* dev, HWND wnd, const DXGI_SWAP_CHAIN_DESC1* d,
                                            const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs, IDXGIOutput* o,
                                            IDXGISwapChain1** out) {
    const HRESULT hr = g_origForHwnd(f, dev, wnd, d, fs, o, out);
    if (SUCCEEDED(hr) && out && *out) OnSwapChainCreated(dev, *out);
    return hr;
}

HRESULT STDMETHODCALLTYPE HookCreateForCoreWindow(IDXGIFactory2* f, IUnknown* dev, IUnknown* wnd,
                                                  const DXGI_SWAP_CHAIN_DESC1* d, IDXGIOutput* o,
                                                  IDXGISwapChain1** out) {
    const HRESULT hr = g_origForCoreWindow(f, dev, wnd, d, o, out);
    if (SUCCEEDED(hr) && out && *out) OnSwapChainCreated(dev, *out);
    return hr;
}

HRESULT STDMETHODCALLTYPE HookCreateForComposition(IDXGIFactory2* f, IUnknown* dev, const DXGI_SWAP_CHAIN_DESC1* d,
                                                   IDXGIOutput* o, IDXGISwapChain1** out) {
    const HRESULT hr = g_origForComposition(f, dev, d, o, out);
    if (SUCCEEDED(hr) && out && *out) OnSwapChainCreated(dev, *out);
    return hr;
}

void HookFactory(IDXGIFactory2* factory) {
    if (g_origForHwnd) return;
    void** vt = *reinterpret_cast<void***>(factory);
    Detour(vt[kSlotCreateSwapChain], reinterpret_cast<void*>(&HookCreateSwapChain), &g_origCreate);
    Detour(vt[kSlotCreateForCoreWindow], reinterpret_cast<void*>(&HookCreateForCoreWindow), &g_origForCoreWindow);
    Detour(vt[kSlotCreateForComposition], reinterpret_cast<void*>(&HookCreateForComposition), &g_origForComposition);
    Detour(vt[kSlotCreateForHwnd], reinterpret_cast<void*>(&HookCreateForHwnd), &g_origForHwnd);
    Log("Hooked DXGI factory (CreateSwapChain: %s)", g_origCreate && g_origForHwnd ? "yes" : "partly");
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
    if (G().cfg.diagnostics) EnableDred();
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
                if (SUCCEEDED(d12->CreateCommandQueue(&qd, IID_PPV_ARGS(&q)))) {
                    HookFromDevice(factory.Get(), q.Get(), false);
                    void** qvt = *reinterpret_cast<void***>(q.Get());
                    if (!g_origExecute)
                        Detour(qvt[kSlotExecuteCommandLists], reinterpret_cast<void*>(&HookExecuteCommandLists),
                               &g_origExecute);
                }
            }
        }
    }

    // Last, so Nitro's own probe swap chains above don't go through it.
    HookFactory(factory.Get());
    Log("%d swap chain implementation(s) hooked", g_vtCount.load());
}

} // namespace lsfgi

// OpenGL hooks. SwapBuffers / wglSwapBuffers are intercepted; frames are
// shared with Nitro's D3D11 device through WGL_NV_DX_interop2 (supported by
// NVIDIA, AMD and Intel drivers on Windows), so nothing goes through the CPU.

#include <windows.h>

#include <MinHook.h>

#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "../core/bridge.h"
#include "../core/log.h"
#include "../core/overlay.h"
#include "../core/runtime.h"
#include "hooks.h"

namespace lsfgi {

namespace {

using GLenum = unsigned int;
using GLuint = unsigned int;
using GLint = int;
using GLsizei = int;
using GLbitfield = unsigned int;
using GLboolean = unsigned char;
using GLubyte = unsigned char;

constexpr GLenum GL_TEXTURE_2D = 0x0DE1;
constexpr GLenum GL_SCISSOR_TEST = 0x0C11;
constexpr GLenum GL_VENDOR = 0x1F00;
constexpr GLenum GL_NEAREST = 0x2600;
constexpr GLbitfield GL_COLOR_BUFFER_BIT = 0x4000;
constexpr GLenum GL_READ_FRAMEBUFFER = 0x8CA8;
constexpr GLenum GL_DRAW_FRAMEBUFFER = 0x8CA9;
constexpr GLenum GL_DRAW_FRAMEBUFFER_BINDING = 0x8CA6;
constexpr GLenum GL_READ_FRAMEBUFFER_BINDING = 0x8CAA;
constexpr GLenum GL_COLOR_ATTACHMENT0 = 0x8CE0;
constexpr GLenum GL_FRAMEBUFFER_COMPLETE = 0x8CD5;
constexpr GLenum GL_FRAMEBUFFER_SRGB = 0x8DB9;
constexpr GLenum WGL_ACCESS_READ_WRITE_NV = 0x0001;

struct GLFuncs {
    HGLRC(WINAPI* wglGetCurrentContext)() = nullptr;
    PROC(WINAPI* wglGetProcAddress)(LPCSTR) = nullptr;
    void(WINAPI* glGetIntegerv)(GLenum, GLint*) = nullptr;
    const GLubyte*(WINAPI* glGetString)(GLenum) = nullptr;
    GLboolean(WINAPI* glIsEnabled)(GLenum) = nullptr;
    void(WINAPI* glEnable)(GLenum) = nullptr;
    void(WINAPI* glDisable)(GLenum) = nullptr;
    void(WINAPI* glGenTextures)(GLsizei, GLuint*) = nullptr;
    void(WINAPI* glDeleteTextures)(GLsizei, const GLuint*) = nullptr;
    // GL 3.0
    void(WINAPI* glGenFramebuffers)(GLsizei, GLuint*) = nullptr;
    void(WINAPI* glDeleteFramebuffers)(GLsizei, const GLuint*) = nullptr;
    void(WINAPI* glBindFramebuffer)(GLenum, GLuint) = nullptr;
    void(WINAPI* glFramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint) = nullptr;
    GLenum(WINAPI* glCheckFramebufferStatus)(GLenum) = nullptr;
    void(WINAPI* glBlitFramebuffer)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield,
                                    GLenum) = nullptr;
    // WGL_NV_DX_interop
    HANDLE(WINAPI* wglDXOpenDeviceNV)(void*) = nullptr;
    BOOL(WINAPI* wglDXCloseDeviceNV)(HANDLE) = nullptr;
    HANDLE(WINAPI* wglDXRegisterObjectNV)(HANDLE, void*, GLuint, GLenum, GLenum) = nullptr;
    BOOL(WINAPI* wglDXUnregisterObjectNV)(HANDLE, HANDLE) = nullptr;
    BOOL(WINAPI* wglDXLockObjectsNV)(HANDLE, GLint, HANDLE*) = nullptr;
    BOOL(WINAPI* wglDXUnlockObjectsNV)(HANDLE, GLint, HANDLE*) = nullptr;
};

GLFuncs gl;
bool g_glBaseLoaded = false;

template <typename T>
void LoadExport(HMODULE m, T& fn, const char* name) {
    fn = reinterpret_cast<T>(GetProcAddress(m, name));
}

template <typename T>
bool LoadExt(T& fn, const char* name) {
    PROC p = gl.wglGetProcAddress(name);
    const auto v = reinterpret_cast<intptr_t>(p);
    fn = (v == 0 || v == 1 || v == 2 || v == 3 || v == -1) ? nullptr : reinterpret_cast<T>(p);
    return fn != nullptr;
}

bool LoadBase() {
    if (g_glBaseLoaded) return gl.wglGetCurrentContext != nullptr;
    g_glBaseLoaded = true;
    HMODULE m = GetModuleHandleW(L"opengl32.dll");
    if (!m) return false;
    LoadExport(m, gl.wglGetCurrentContext, "wglGetCurrentContext");
    LoadExport(m, gl.wglGetProcAddress, "wglGetProcAddress");
    LoadExport(m, gl.glGetIntegerv, "glGetIntegerv");
    LoadExport(m, gl.glGetString, "glGetString");
    LoadExport(m, gl.glIsEnabled, "glIsEnabled");
    LoadExport(m, gl.glEnable, "glEnable");
    LoadExport(m, gl.glDisable, "glDisable");
    LoadExport(m, gl.glGenTextures, "glGenTextures");
    LoadExport(m, gl.glDeleteTextures, "glDeleteTextures");
    return gl.wglGetCurrentContext && gl.wglGetProcAddress && gl.glGetIntegerv && gl.glGetString &&
           gl.glIsEnabled && gl.glEnable && gl.glDisable && gl.glGenTextures && gl.glDeleteTextures;
}

// Extension functions are per context in theory; in practice the same for a
// given driver, so load them once with the first context we see.
bool LoadContextFuncs() {
    static int state = 0; // 0 untried, 1 ok, -1 failed
    if (state) return state > 0;
    bool ok = LoadExt(gl.glGenFramebuffers, "glGenFramebuffers") &&
              LoadExt(gl.glDeleteFramebuffers, "glDeleteFramebuffers") &&
              LoadExt(gl.glBindFramebuffer, "glBindFramebuffer") &&
              LoadExt(gl.glFramebufferTexture2D, "glFramebufferTexture2D") &&
              LoadExt(gl.glCheckFramebufferStatus, "glCheckFramebufferStatus") &&
              LoadExt(gl.glBlitFramebuffer, "glBlitFramebuffer");
    if (!ok) Log("OpenGL 3.0 framebuffer functions not available - OpenGL frame gen disabled");
    const bool interop = LoadExt(gl.wglDXOpenDeviceNV, "wglDXOpenDeviceNV") &&
                         LoadExt(gl.wglDXCloseDeviceNV, "wglDXCloseDeviceNV") &&
                         LoadExt(gl.wglDXRegisterObjectNV, "wglDXRegisterObjectNV") &&
                         LoadExt(gl.wglDXUnregisterObjectNV, "wglDXUnregisterObjectNV") &&
                         LoadExt(gl.wglDXLockObjectsNV, "wglDXLockObjectsNV") &&
                         LoadExt(gl.wglDXUnlockObjectsNV, "wglDXUnlockObjectsNV");
    if (ok && !interop) Log("WGL_NV_DX_interop2 not supported by this driver - OpenGL frame gen disabled");
    state = ok && interop ? 1 : -1;
    return state > 0;
}

UINT VendorFromGL() {
    const auto* v = reinterpret_cast<const char*>(gl.glGetString(GL_VENDOR));
    if (!v) return 0;
    if (strstr(v, "NVIDIA")) return 0x10DE;
    if (strstr(v, "AMD") || strstr(v, "ATI")) return 0x1002;
    if (strstr(v, "Intel")) return 0x8086;
    return 0;
}

class GLState {
public:
    ~GLState() { Release(); }
    BOOL Swap(HDC dc, BOOL(WINAPI* orig)(HDC));

private:
    bool Ensure(UINT w, UINT h);
    void Release();
    void Blit(GLuint readFbo, GLuint drawFbo) const {
        gl.glBindFramebuffer(GL_READ_FRAMEBUFFER, readFbo);
        gl.glBindFramebuffer(GL_DRAW_FRAMEBUFFER, drawFbo);
        // Flipped both ways, so the shared texture is upright for D3D11 (the menu
        // is drawn on it) and the picture comes back the right way round.
        gl.glBlitFramebuffer(0, 0, static_cast<GLint>(w_), static_cast<GLint>(h_), 0, static_cast<GLint>(h_),
                             static_cast<GLint>(w_), 0, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    }

    std::unique_ptr<Bridge> bridge_;
    HANDLE interop_ = nullptr;
    GLuint texIn_ = 0, fboIn_ = 0;
    HANDLE objIn_ = nullptr;
    std::vector<GLuint> texOut_, fboOut_;
    std::vector<HANDLE> objOut_;
    GLuint texReal_ = 0, fboReal_ = 0;
    HANDLE objReal_ = nullptr;
    UINT w_ = 0, h_ = 0;
    bool failed_ = false;
};

void GLState::Release() {
    if (interop_) {
        if (objIn_) gl.wglDXUnregisterObjectNV(interop_, objIn_);
        if (objReal_) gl.wglDXUnregisterObjectNV(interop_, objReal_);
        for (HANDLE h : objOut_)
            if (h) gl.wglDXUnregisterObjectNV(interop_, h);
        gl.wglDXCloseDeviceNV(interop_);
    }
    if (fboIn_) gl.glDeleteFramebuffers(1, &fboIn_);
    if (fboReal_) gl.glDeleteFramebuffers(1, &fboReal_);
    if (texReal_) gl.glDeleteTextures(1, &texReal_);
    if (!fboOut_.empty()) gl.glDeleteFramebuffers(static_cast<GLsizei>(fboOut_.size()), fboOut_.data());
    if (texIn_) gl.glDeleteTextures(1, &texIn_);
    if (!texOut_.empty()) gl.glDeleteTextures(static_cast<GLsizei>(texOut_.size()), texOut_.data());
    interop_ = objIn_ = objReal_ = nullptr;
    texIn_ = fboIn_ = texReal_ = fboReal_ = 0;
    texOut_.clear();
    fboOut_.clear();
    objOut_.clear();
    bridge_.reset();
}

bool GLState::Ensure(UINT w, UINT h) {
    if (bridge_ && bridge_->Matches(w, h, DXGI_FORMAT_R8G8B8A8_UNORM, G().multiplier.load())) return true;
    Release();

    bridge_ = std::make_unique<Bridge>();
    BridgeParams p;
    p.vendorId = VendorFromGL();
    p.width = w;
    p.height = h;
    p.format = DXGI_FORMAT_R8G8B8A8_UNORM;
    std::string err;
    if (!bridge_->Init(p, err)) {
        Log("OpenGL frame generation setup failed: %s", err.c_str());
        return false;
    }
    interop_ = gl.wglDXOpenDeviceNV(bridge_->Device());
    if (!interop_) {
        Log("wglDXOpenDeviceNV failed (GPU mismatch?)");
        return false;
    }

    GLint prevRead = 0, prevDraw = 0;
    gl.glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prevRead);
    gl.glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prevDraw);

    auto wrap = [&](ID3D11Texture2D* tex, GLuint& glTex, GLuint& fbo, HANDLE& obj) {
        gl.glGenTextures(1, &glTex);
        obj = gl.wglDXRegisterObjectNV(interop_, tex, glTex, GL_TEXTURE_2D, WGL_ACCESS_READ_WRITE_NV);
        if (!obj) return false;
        gl.glGenFramebuffers(1, &fbo);
        gl.wglDXLockObjectsNV(interop_, 1, &obj);
        gl.glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
        gl.glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, glTex, 0);
        const bool complete = gl.glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
        gl.wglDXUnlockObjectsNV(interop_, 1, &obj);
        return complete;
    };

    bool ok = wrap(bridge_->Input(), texIn_, fboIn_, objIn_) &&
              wrap(bridge_->RealOutput(), texReal_, fboReal_, objReal_);
    const size_t n = static_cast<size_t>(bridge_->MaxOutputs());
    texOut_.assign(n, 0);
    fboOut_.assign(n, 0);
    objOut_.assign(n, nullptr);
    for (size_t k = 0; k < n && ok; ++k) ok = wrap(bridge_->Output(static_cast<int>(k)), texOut_[k], fboOut_[k], objOut_[k]);

    gl.glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(prevRead));
    gl.glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(prevDraw));
    if (!ok) {
        Log("Could not share textures between OpenGL and D3D11");
        return false;
    }
    w_ = w;
    h_ = h;
    Log("OpenGL frame generation ready: %ux%u, %dx", w, h, G().multiplier.load());
    return true;
}

BOOL GLState::Swap(HDC dc, BOOL(WINAPI* orig)(HDC)) {
    const bool generate = G().fgEnabled.load();
    const bool menu = OverlayVisible();
    if (!generate && bridge_) bridge_->Reset();
    if (failed_ || (!generate && !menu && G().cfg.fsrMode == 0)) {
        NoteFrame(0, "OpenGL", w_, h_);
        return orig(dc);
    }
    RECT rc{};
    HWND wnd = WindowFromDC(dc);
    if (!wnd || !GetClientRect(wnd, &rc) || rc.right <= 0 || rc.bottom <= 0) return orig(dc);
    if (!Ensure(static_cast<UINT>(rc.right), static_cast<UINT>(rc.bottom))) {
        Release();
        failed_ = true;
        return orig(dc);
    }

    // Save the bits of GL state we touch
    GLint prevRead = 0, prevDraw = 0;
    gl.glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prevRead);
    gl.glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prevDraw);
    const bool scissor = gl.glIsEnabled(GL_SCISSOR_TEST) != 0;
    const bool srgb = gl.glIsEnabled(GL_FRAMEBUFFER_SRGB) != 0;
    auto prepare = [&] {
        gl.glDisable(GL_SCISSOR_TEST);
        gl.glDisable(GL_FRAMEBUFFER_SRGB);
    };
    auto restore = [&] {
        gl.glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(prevRead));
        gl.glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(prevDraw));
        if (scissor) gl.glEnable(GL_SCISSOR_TEST);
        if (srgb) gl.glEnable(GL_FRAMEBUFFER_SRGB);
    };

    // Real frame -> shared texture
    prepare();
    gl.wglDXLockObjectsNV(interop_, 1, &objIn_);
    Blit(0, fboIn_);
    gl.wglDXUnlockObjectsNV(interop_, 1, &objIn_);
    restore();

    const bool drawMenu = menu && OverlayBegin(bridge_->Device(), bridge_->Context(), wnd, w_, h_);
    const FramePlan plan = PlanFrame(wnd);
    const int n = bridge_->Run(generate, drawMenu ? Bridge::OverlayFn([](ID3D11DeviceContext* c, ID3D11Texture2D* t) { OverlayDraw(c, t); }) : Bridge::OverlayFn{},
                               plan.gens, plan.denom);
    NoteFrame(n, "OpenGL", w_, h_);
    for (int k = 0; k < n; ++k) {
        prepare();
        gl.wglDXLockObjectsNV(interop_, 1, &objOut_[k]);
        Blit(fboOut_[k], 0);
        gl.wglDXUnlockObjectsNV(interop_, 1, &objOut_[k]);
        restore();
        orig(dc);
    }
    const bool useReal = bridge_->RealOutputUsed();
    if (useReal || n > 0) {
        HANDLE obj = useReal ? objReal_ : objIn_;
        prepare();
        gl.wglDXLockObjectsNV(interop_, 1, &obj);
        Blit(useReal ? fboReal_ : fboIn_, 0);
        gl.wglDXUnlockObjectsNV(interop_, 1, &obj);
        restore();
    }
    return orig(dc);
}

std::mutex g_mutex;
// Never destroyed (see dxgi_hooks.cpp).
auto& g_states = *new std::unordered_map<HGLRC, std::unique_ptr<GLState>>();

using SwapFn = BOOL(WINAPI*)(HDC);
SwapFn g_gdiSwap = nullptr;
SwapFn g_wglSwap = nullptr;

BOOL HandleSwap(HDC dc, SwapFn orig) {
    if (InHook() || !LoadBase()) return orig(dc);
    HGLRC rc = gl.wglGetCurrentContext();
    if (!rc || !LoadContextFuncs()) return orig(dc);

    InHook() = true;
    PollHotkeys();
    GLState* state = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto& slot = g_states[rc];
        if (!slot) slot = std::make_unique<GLState>();
        state = slot.get();
    }
    const BOOL r = state->Swap(dc, orig);
    InHook() = false;
    return r;
}

BOOL WINAPI HkGdiSwapBuffers(HDC dc) {
    return HandleSwap(dc, g_gdiSwap);
}

BOOL WINAPI HkWglSwapBuffers(HDC dc) {
    return HandleSwap(dc, g_wglSwap);
}

} // namespace

void InstallGLHooks() {
    // Make sure opengl32 is present so wglSwapBuffers can be hooked up front.
    LoadLibraryW(L"opengl32.dll");
    int hooked = 0;
    if (MH_CreateHookApi(L"gdi32", "SwapBuffers", reinterpret_cast<void*>(&HkGdiSwapBuffers),
                         reinterpret_cast<void**>(&g_gdiSwap)) == MH_OK)
        ++hooked;
    if (MH_CreateHookApi(L"opengl32", "wglSwapBuffers", reinterpret_cast<void*>(&HkWglSwapBuffers),
                         reinterpret_cast<void**>(&g_wglSwap)) == MH_OK)
        ++hooked;
    MH_EnableHook(MH_ALL_HOOKS);
    Log("OpenGL hooks installed (%d)", hooked);
}

} // namespace lsfgi

#include "overlay.h"

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_internal.h>
#include <imgui_impl_win32.h>
#include <wrl/client.h>

#include <MinHook.h>

#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <mutex>

#include "log.h"
#include "nitro_ui.h"
#include "runtime.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace lsfgi {

namespace {

using Microsoft::WRL::ComPtr;

std::recursive_mutex g_ui; // ImGui is single-threaded; the window thread and the render thread both touch it
ImGuiContext* g_ctx = nullptr;
nitro_ui::Fonts g_fonts;
float g_scale = 1.0f;
ComPtr<ID3D11Device> g_dev;
HWND g_hwnd = nullptr;
WNDPROC g_origProc = nullptr;
ImDrawData* g_drawData = nullptr;
ULONGLONG g_lastBegin = 0;
ULONGLONG g_savedAt = 0;

using SetCursorPosFn = BOOL(WINAPI*)(int, int);
using ClipCursorFn = BOOL(WINAPI*)(const RECT*);
SetCursorPosFn g_setCursorPos = nullptr;
ClipCursorFn g_clipCursor = nullptr;

// While the menu is open, stop the game from re-centring or trapping the cursor.
BOOL WINAPI HkSetCursorPos(int x, int y) {
    if (G().menuOpen.load()) return TRUE;
    return g_setCursorPos(x, y);
}

BOOL WINAPI HkClipCursor(const RECT* r) {
    if (G().menuOpen.load()) return TRUE;
    return g_clipCursor(r);
}

void InstallCursorHooks() {
    const MH_STATUS init = MH_Initialize();
    if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) return;
    MH_CreateHookApi(L"user32", "SetCursorPos", reinterpret_cast<void*>(&HkSetCursorPos),
                     reinterpret_cast<void**>(&g_setCursorPos));
    MH_CreateHookApi(L"user32", "ClipCursor", reinterpret_cast<void*>(&HkClipCursor),
                     reinterpret_cast<void**>(&g_clipCursor));
    MH_EnableHook(MH_ALL_HOOKS);
}

bool IsInputMessage(UINT m) {
    return (m >= WM_MOUSEFIRST && m <= WM_MOUSELAST) || (m >= WM_KEYFIRST && m <= WM_KEYLAST && m != WM_SYSKEYDOWN &&
                                                         m != WM_SYSKEYUP) ||
           m == WM_INPUT || m == WM_SETCURSOR;
}

LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (G().menuOpen.load()) {
        {
            std::lock_guard<std::recursive_mutex> lock(g_ui);
            if (g_ctx) {
                ImGui::SetCurrentContext(g_ctx);
                ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp);
            }
        }
        if (IsInputMessage(msg)) {
            if (msg == WM_SETCURSOR) {
                SetCursor(nullptr); // ImGui draws its own cursor
                return TRUE;
            }
            return msg == WM_INPUT ? DefWindowProcW(hwnd, msg, wp, lp) : 0;
        }
    }
    return CallWindowProcW(g_origProc, hwnd, msg, wp, lp);
}

void AttachWindow(HWND hwnd) {
    if (hwnd == g_hwnd) return;
    if (g_hwnd) {
        ImGui_ImplWin32_Shutdown();
        if (g_origProc && IsWindow(g_hwnd))
            SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_origProc));
    }
    g_hwnd = hwnd;
    ImGui_ImplWin32_Init(hwnd);
    g_origProc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&OverlayWndProc)));
}

void AttachDevice(ID3D11Device* dev, ID3D11DeviceContext* ctx) {
    if (dev == g_dev.Get()) return;
    if (g_dev) ImGui_ImplDX11_Shutdown();
    g_dev = dev;
    ImGui_ImplDX11_Init(dev, ctx);
}

// ImGui lays out in window-client coordinates; stretch to the back buffer.
void FitDrawData(ImDrawData* dd, UINT w, UINT h) {
    if (!dd || dd->DisplaySize.x <= 0 || dd->DisplaySize.y <= 0) return;
    const ImVec2 s(static_cast<float>(w) / dd->DisplaySize.x, static_cast<float>(h) / dd->DisplaySize.y);
    if (s.x == 1.0f && s.y == 1.0f) return;
    for (ImDrawList* list : dd->CmdLists)
        for (ImDrawVert& v : list->VtxBuffer) {
            v.pos.x *= s.x;
            v.pos.y *= s.y;
        }
    dd->ScaleClipRects(s);
    dd->DisplaySize = ImVec2(static_cast<float>(w), static_cast<float>(h));
}

const char* KeyName(int vk) {
    static char buf[32];
    const UINT sc = MapVirtualKeyA(static_cast<UINT>(vk), MAPVK_VK_TO_VSC);
    LONG lparam = static_cast<LONG>(sc << 16);
    if (vk == VK_INSERT || vk == VK_HOME || vk == VK_END || vk == VK_PRIOR || vk == VK_NEXT || vk == VK_DELETE)
        lparam |= 1 << 24; // extended key, so we get "Insert" rather than "Num 0"
    if (!GetKeyNameTextA(lparam, buf, sizeof(buf))) snprintf(buf, sizeof(buf), "0x%02X", vk);
    return buf;
}

void BuildMenu() {
    auto& g = G();
    const float s = g_scale;
    ImGui::SetNextWindowPos(ImVec2(28 * s, 28 * s), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(420 * s, 0), ImGuiCond_Always);
    ImGui::Begin("##nitro", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize);

    // Header
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float logo = 40 * s;
    nitro_ui::DrawLogo(ImGui::GetWindowDrawList(), p, logo);
    ImGui::Dummy(ImVec2(logo, logo));
    ImGui::SameLine(0, 12 * s);
    ImGui::BeginGroup();
    ImGui::PushFont(g_fonts.bold);
    {
        const ImVec2 tp = ImGui::GetCursorScreenPos();
        const ImVec2 ts = ImGui::CalcTextSize("NITRO");
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const int v0 = dl->VtxBuffer.Size;
        dl->AddText(tp, IM_COL32_WHITE, "NITRO");
        ImGui::ShadeVertsLinearColorGradientKeepAlpha(dl, v0, dl->VtxBuffer.Size, tp, ImVec2(tp.x + ts.x, tp.y),
                                                      nitro_ui::kCyan, nitro_ui::kPink);
        ImGui::Dummy(ts);
    }
    ImGui::PopFont();
    ImGui::PushFont(g_fonts.caption);
    ImGui::TextColored(nitro_ui::kMuted, "%s  \xC2\xB7  %ux%u", g.apiName, g.width, g.height);
    ImGui::PopFont();
    ImGui::EndGroup();
    ImGui::SameLine();
    {
        char closeHint[48];
        snprintf(closeHint, sizeof(closeHint), "%s to close", KeyName(g.cfg.menuKey));
        ImGui::PushFont(g_fonts.caption);
        const float w = ImGui::CalcTextSize(closeHint).x + 20 * s;
        nitro_ui::SameLineRight(w);
        nitro_ui::Chip(closeHint, nitro_ui::kMuted);
        ImGui::PopFont();
    }

    // Live FPS
    ImGui::Spacing();
    {
        const ImVec2 b0 = ImGui::GetCursorScreenPos();
        const float bw = ImGui::GetContentRegionAvail().x;
        const float bh = 58 * s;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(b0, ImVec2(b0.x + bw, b0.y + bh), IM_COL32(22, 26, 51, 255), 12 * s);
        char real[32], out[32];
        snprintf(real, sizeof(real), "%.0f", g.realFps);
        snprintf(out, sizeof(out), "%.0f", g.fgEnabled.load() ? g.outputFps : g.realFps);
        ImGui::SetCursorScreenPos(ImVec2(b0.x + 16 * s, b0.y + 8 * s));
        ImGui::BeginGroup();
        ImGui::PushFont(g_fonts.caption);
        ImGui::TextColored(nitro_ui::kMuted, "Rendered");
        ImGui::PopFont();
        ImGui::PushFont(g_fonts.bold);
        ImGui::Text("%s fps", real);
        ImGui::PopFont();
        ImGui::EndGroup();
        ImGui::SetCursorScreenPos(ImVec2(b0.x + bw * 0.5f, b0.y + 8 * s));
        ImGui::BeginGroup();
        ImGui::PushFont(g_fonts.caption);
        ImGui::TextColored(nitro_ui::kMuted, "On screen");
        ImGui::PopFont();
        ImGui::PushFont(g_fonts.bold);
        ImGui::TextColored(ImVec4(0.0f, 0.898f, 1.0f, 1.0f), "%s fps", out);
        ImGui::PopFont();
        ImGui::EndGroup();
        ImGui::SetCursorScreenPos(ImVec2(b0.x, b0.y + bh));
        ImGui::Dummy(ImVec2(bw, 4 * s));
    }

    // Controls
    const float toggleW = ImGui::GetFrameHeight() * 0.85f * 1.85f;
    bool enabled = g.fgEnabled.load();
    char hint[48];
    snprintf(hint, sizeof(hint), "(%s)", KeyName(g.cfg.toggleKey));
    nitro_ui::RowLabel("Frame generation", hint);
    nitro_ui::SameLineRight(toggleW);
    if (nitro_ui::Toggle("##fg", &enabled)) g.fgEnabled = enabled;

    snprintf(hint, sizeof(hint), "(%s)", KeyName(g.cfg.cycleKey));
    nitro_ui::RowLabel("Multiplier", hint);
    {
        const float pw = 56 * s, ph = ImGui::GetFrameHeight();
        nitro_ui::SameLineRight(pw * 3 + 8 * s * 2);
        for (int m = 2; m <= 4; ++m) {
            char label[8];
            snprintf(label, sizeof(label), "%dx", m);
            if (nitro_ui::Pill(label, g.multiplier.load() == m, ImVec2(pw, ph)) && g.multiplier.load() != m) {
                g.multiplier = m;
                BumpSettings();
            }
            if (m < 4) ImGui::SameLine(0, 8 * s);
        }
    }

    nitro_ui::RowLabel("Flow scale");
    {
        const float sw = 190 * s;
        nitro_ui::SameLineRight(sw);
        ImGui::SetNextItemWidth(sw);
        int pct = static_cast<int>(g.cfg.flowScale * 100.0f + 0.5f);
        if (ImGui::SliderInt("##flow", &pct, 25, 100, "%d%%")) g.cfg.flowScale = static_cast<float>(pct) / 100.0f;
        if (ImGui::IsItemDeactivatedAfterEdit()) BumpSettings();
    }

    bool perf = g.cfg.performanceMode;
    nitro_ui::RowLabel("Performance mode");
    nitro_ui::SameLineRight(toggleW);
    if (nitro_ui::Toggle("##perf", &perf)) {
        g.cfg.performanceMode = perf;
        BumpSettings();
    }

    bool vsync = g.cfg.vsyncPacing;
    nitro_ui::RowLabel("VSync pacing");
    nitro_ui::SameLineRight(toggleW);
    if (nitro_ui::Toggle("##vsync", &vsync)) g.cfg.vsyncPacing = vsync;

    nitro_ui::RowLabel("HDR");
    {
        const float cw = 120 * s;
        nitro_ui::SameLineRight(cw);
        ImGui::SetNextItemWidth(cw);
        const char* items[] = {"Auto", "On", "Off"};
        int sel = g.cfg.hdr < 0 ? 0 : (g.cfg.hdr ? 1 : 2);
        if (ImGui::Combo("##hdr", &sel, items, 3)) {
            g.cfg.hdr = sel == 0 ? -1 : (sel == 1 ? 1 : 0);
            BumpSettings();
        }
    }

    ImGui::Spacing();
    const float bw = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
    if (nitro_ui::AccentButton("Save as default", ImVec2(bw, 36 * s))) {
        Config c = g.cfg;
        c.multiplier = g.multiplier.load();
        c.enabled = g.fgEnabled.load();
        if (SaveConfig(g.dir + L"\\" + kConfigFileName, c)) g_savedAt = GetTickCount64();
    }
    ImGui::SameLine();
    if (nitro_ui::GhostButton("Close", ImVec2(bw, 36 * s))) g.menuOpen = false;

    ImGui::PushFont(g_fonts.caption);
    if (g_savedAt && GetTickCount64() - g_savedAt < 2500)
        ImGui::TextColored(nitro_ui::kGood, "Saved. These settings load next time.");
    else
        ImGui::TextColored(nitro_ui::kMuted, "Changes apply instantly. Save to keep them.");
    ImGui::PopFont();
    ImGui::End();
}

} // namespace

bool OverlayVisible() {
    return G().menuOpen.load();
}

HWND FindMainWindow() {
    struct Ctx {
        DWORD pid;
        HWND best;
        LONG area;
    } c{GetCurrentProcessId(), nullptr, 0};
    EnumWindows(
        [](HWND h, LPARAM lp) -> BOOL {
            auto* c = reinterpret_cast<Ctx*>(lp);
            DWORD pid = 0;
            GetWindowThreadProcessId(h, &pid);
            if (pid != c->pid || !IsWindowVisible(h) || GetWindow(h, GW_OWNER)) return TRUE;
            RECT r{};
            GetClientRect(h, &r);
            const LONG area = (r.right - r.left) * (r.bottom - r.top);
            if (area > c->area) {
                c->area = area;
                c->best = h;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&c));
    return c.best;
}

bool OverlayBegin(ID3D11Device* dev, ID3D11DeviceContext* ctx, HWND hwnd, UINT width, UINT height) {
    std::lock_guard<std::recursive_mutex> lock(g_ui);
    g_drawData = nullptr;
    if (!dev || !ctx || !hwnd || !width || !height) return false;

    if (!g_ctx) {
        IMGUI_CHECKVERSION();
        g_ctx = ImGui::CreateContext();
        ImGui::SetCurrentContext(g_ctx);
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr; // never write imgui.ini into the game folder
        io.LogFilename = nullptr;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
        io.MouseDrawCursor = true;
        RECT rc{};
        GetClientRect(hwnd, &rc);
        g_scale = std::clamp(static_cast<float>(rc.bottom - rc.top) / 1080.0f, 0.75f, 2.5f);
        g_fonts = nitro_ui::LoadFonts(18.0f * g_scale);
        nitro_ui::ApplyStyle(g_scale, 0.96f);
        InstallCursorHooks();
        Log("In-game menu ready");
    }
    ImGui::SetCurrentContext(g_ctx);
    AttachDevice(dev, ctx);
    AttachWindow(hwnd);

    const ULONGLONG now = GetTickCount64();
    if (now - g_lastBegin > 250 && g_clipCursor) g_clipCursor(nullptr); // menu just opened: free the cursor
    g_lastBegin = now;

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    BuildMenu();
    ImGui::Render();
    g_drawData = ImGui::GetDrawData();
    FitDrawData(g_drawData, width, height);
    return g_drawData != nullptr;
}

void OverlayDraw(ID3D11DeviceContext* ctx, ID3D11Resource* target) {
    std::lock_guard<std::recursive_mutex> lock(g_ui);
    if (!g_drawData || !g_dev || !target) return;
    ImGui::SetCurrentContext(g_ctx);
    ComPtr<ID3D11RenderTargetView> rtv;
    if (FAILED(g_dev->CreateRenderTargetView(target, nullptr, &rtv))) return;
    ID3D11RenderTargetView* rtvs[] = {rtv.Get()};
    ctx->OMSetRenderTargets(1, rtvs, nullptr);
    ImGui_ImplDX11_RenderDrawData(g_drawData);
    ctx->OMSetRenderTargets(0, nullptr, nullptr);
}

} // namespace lsfgi

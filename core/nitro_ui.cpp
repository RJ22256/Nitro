#include "nitro_ui.h"

#include <imgui_internal.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <cstdlib>
#include <sys/stat.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace nitro_ui {

namespace {

std::vector<std::string> g_fontDirs; // extra folders from AddFontDir, searched first

#ifdef _WIN32
std::string Utf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring Wide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

bool Exists(const std::string& p) {
    const DWORD a = GetFileAttributesW(Wide(p).c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// Folder of the module this code is linked into (Nitro.exe or the hook DLL).
std::string ModuleDir() {
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&ModuleDir), &self);
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(self, path, MAX_PATH);
    std::wstring p = path;
    const size_t slash = p.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::string(".") : Utf8(p.substr(0, slash));
}

// Where fonts are looked for: AddFontDir folders, a Fonts folder next to
// Nitro, fonts installed for this user, then the Windows font folder.
std::vector<std::string> FontDirs() {
    std::vector<std::string> dirs;
    for (const std::string& d : g_fontDirs) {
        dirs.push_back(d + "\\Fonts");
        dirs.push_back(d);
    }
    const std::string mod = ModuleDir();
    dirs.push_back(mod + "\\Fonts");
    dirs.push_back(mod);
    wchar_t buf[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH))
        dirs.push_back(Utf8(buf) + "\\Microsoft\\Windows\\Fonts");
    GetWindowsDirectoryW(buf, MAX_PATH);
    dirs.push_back(Utf8(buf) + "\\Fonts");
    return dirs;
}
const char kSep = '\\';
#else
// Non-Windows builds (UI previews only): fonts come from NITRO_FONT_DIR.
bool Exists(const std::string& p) {
    struct stat st {};
    return stat(p.c_str(), &st) == 0;
}

std::vector<std::string> FontDirs() {
    std::vector<std::string> dirs = g_fontDirs;
    const char* dir = std::getenv("NITRO_FONT_DIR");
    dirs.push_back(dir ? dir : ".");
    return dirs;
}
const char kSep = '/';
#endif

std::string FindFont(const char* file) {
    for (const std::string& d : FontDirs()) {
        const std::string p = d + kSep + file;
        if (Exists(p)) return p;
    }
    return {};
}

const ImWchar kRanges[] = {0x0020, 0x00FF, 0x2013, 0x2026, 0x2190, 0x2193, 0x00D7, 0x00D7, 0};

ImFont* Load(const char* file, float px) {
    const std::string path = FindFont(file);
    if (path.empty()) return nullptr;
    ImFontConfig cfg;
    cfg.OversampleH = 2;
    cfg.OversampleV = 2;
    return ImGui::GetIO().Fonts->AddFontFromFileTTF(path.c_str(), px, &cfg, kRanges);
}

ImU32 Mix(ImU32 a, ImU32 b, float t) {
    const ImVec4 x = ImGui::ColorConvertU32ToFloat4(a), y = ImGui::ColorConvertU32ToFloat4(b);
    return ImGui::ColorConvertFloat4ToU32(ImVec4(x.x + (y.x - x.x) * t, x.y + (y.y - x.y) * t,
                                                 x.z + (y.z - x.z) * t, x.w + (y.w - x.w) * t));
}

} // namespace

void AddFontDir(const std::string& utf8Dir) {
    if (!utf8Dir.empty()) g_fontDirs.push_back(utf8Dir);
}

// Source Sans 3 (shipped in Nitro's Fonts folder, SIL Open Font License),
// otherwise Segoe UI, otherwise ImGui's built-in font. Source Sans runs a
// tall line height, so sizes stay as they are.
Fonts LoadFonts(float px) {
    Fonts f;
    const float k = 1.0f;
    f.body = Load("SourceSans3-Regular.ttf", px * k);
    f.shipped = f.body != nullptr;
    if (!f.body) f.body = Load("segoeui.ttf", px);
    if (!f.body) f.body = ImGui::GetIO().Fonts->AddFontDefault();
    f.bold = f.shipped ? Load("SourceSans3-Semibold.ttf", px * k) : Load("seguisb.ttf", px);
    if (!f.bold) f.bold = f.body;
    f.title = f.shipped ? Load("SourceSans3-Light.ttf", px * k * 2.4f) : Load("segoeuil.ttf", px * 2.4f);
    if (!f.title) f.title = f.bold;
    f.caption = f.shipped ? Load("SourceSans3-Regular.ttf", px * k * 0.82f) : Load("segoeui.ttf", px * 0.82f);
    if (!f.caption) f.caption = f.body;
    f.tab = f.shipped ? Load("SourceSans3-Semibold.ttf", px * k * 1.35f) : Load("seguisb.ttf", px * 1.35f);
    if (!f.tab) f.tab = f.bold;
    ImGui::GetIO().FontDefault = f.body;
    return f;
}

void ApplyStyle(float scale, float windowAlpha) {
    ImGuiStyle& s = ImGui::GetStyle();
    s = ImGuiStyle();
    s.WindowRounding = 20.0f;
    s.ChildRounding = 18.0f;
    s.FrameRounding = 12.0f;
    s.PopupRounding = 12.0f;
    s.GrabRounding = 12.0f;
    s.ScrollbarRounding = 12.0f;
    s.TabRounding = 10.0f;
    s.WindowBorderSize = 1.0f;
    s.ChildBorderSize = 1.0f;
    s.PopupBorderSize = 1.0f;
    s.FrameBorderSize = 0.0f;
    s.WindowPadding = ImVec2(22, 18);
    s.FramePadding = ImVec2(14, 8);
    s.ItemSpacing = ImVec2(12, 9);
    s.ItemInnerSpacing = ImVec2(8, 6);
    s.GrabMinSize = 20.0f;
    s.ScrollbarSize = 10.0f;
    s.ScaleAllSizes(scale);

    auto white = [](float a) { return ImVec4(1.0f, 1.0f, 1.0f, a); };
    ImVec4* c = s.Colors;
    const ImVec4 bg(0.031f, 0.078f, 0.200f, windowAlpha);
    const ImVec4 blue(0.0f, 0.439f, 0.820f, 1.0f);
    c[ImGuiCol_Text] = kText;
    c[ImGuiCol_TextDisabled] = kMuted;
    c[ImGuiCol_WindowBg] = bg;
    c[ImGuiCol_ChildBg] = white(0.055f);
    c[ImGuiCol_PopupBg] = ImVec4(0.043f, 0.102f, 0.239f, 0.98f);
    c[ImGuiCol_Border] = white(0.10f);
    c[ImGuiCol_FrameBg] = white(0.08f);
    c[ImGuiCol_FrameBgHovered] = white(0.13f);
    c[ImGuiCol_FrameBgActive] = white(0.17f);
    c[ImGuiCol_TitleBg] = bg;
    c[ImGuiCol_TitleBgActive] = bg;
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab] = white(0.18f);
    c[ImGuiCol_ScrollbarGrabHovered] = white(0.28f);
    c[ImGuiCol_ScrollbarGrabActive] = white(0.36f);
    c[ImGuiCol_CheckMark] = kText;
    c[ImGuiCol_SliderGrab] = kText;
    c[ImGuiCol_SliderGrabActive] = ImVec4(0.361f, 0.667f, 1.0f, 1.0f);
    c[ImGuiCol_Button] = white(0.10f);
    c[ImGuiCol_ButtonHovered] = white(0.16f);
    c[ImGuiCol_ButtonActive] = white(0.22f);
    c[ImGuiCol_Header] = ImVec4(blue.x, blue.y, blue.z, 0.55f);
    c[ImGuiCol_HeaderHovered] = ImVec4(blue.x, blue.y, blue.z, 0.75f);
    c[ImGuiCol_HeaderActive] = blue;
    c[ImGuiCol_Separator] = white(0.10f);
    c[ImGuiCol_NavHighlight] = kText;
    c[ImGuiCol_TextSelectedBg] = ImVec4(blue.x, blue.y, blue.z, 0.55f);
}

namespace {
// Logo geometry in 512-unit coordinates (same as tools/make_logo.py)
constexpr float kSlant = 0.20f;
const ImVec2 kN[] = {{136, 384}, {136, 128}, {204, 128}, {308, 282}, {308, 128},
                     {376, 128}, {376, 384}, {308, 384}, {204, 230}, {204, 384}};
struct SpeedLine {
    float y, x0, alpha;
};
const SpeedLine kLines[] = {{200, 66, 0.55f}, {256, 34, 0.85f}, {312, 66, 0.55f}};

void Gradient(ImDrawList* dl, int v0, ImVec2 a, ImVec2 b, ImU32 ca, ImU32 cb) {
    ImGui::ShadeVertsLinearColorGradientKeepAlpha(dl, v0, dl->VtxBuffer.Size, a, b, ca, cb);
}
} // namespace

void DrawLogo(ImDrawList* dl, ImVec2 pos, float size) {
    const float k = size / 512.0f;
    const ImVec2 end(pos.x + size, pos.y + size);
    int v0 = dl->VtxBuffer.Size;
    dl->AddRectFilled(pos, end, IM_COL32_WHITE, 112.0f * k);
    Gradient(dl, v0, pos, ImVec2(pos.x, end.y), IM_COL32(31, 140, 235, 255), IM_COL32(0, 46, 125, 255));
    for (const SpeedLine& l : kLines) {
        const float x1 = 136 + (256 - l.y) * kSlant - 22;
        dl->AddRectFilled(ImVec2(pos.x + l.x0 * k, pos.y + (l.y - 10) * k), ImVec2(pos.x + x1 * k, pos.y + (l.y + 10) * k),
                          IM_COL32(255, 255, 255, static_cast<int>(255 * l.alpha)), 10 * k);
    }
    ImVec2 pts[10];
    auto shape = [&](float dx, float dy) {
        for (int i = 0; i < 10; ++i)
            pts[i] = ImVec2(pos.x + (kN[i].x + (256 - kN[i].y) * kSlant + dx) * k, pos.y + (kN[i].y + dy) * k);
    };
    shape(8, 12);
    dl->AddConcavePolyFilled(pts, 10, IM_COL32(0, 18, 60, 70));
    shape(0, 0);
    dl->AddConcavePolyFilled(pts, 10, IM_COL32_WHITE);
}

void DrawBackdrop(ImDrawList* dl, ImVec2 min, ImVec2 max) {
    const float w = max.x - min.x, h = max.y - min.y;
    int v0 = dl->VtxBuffer.Size;
    dl->AddRectFilled(min, max, IM_COL32_WHITE);
    Gradient(dl, v0, min, ImVec2(min.x, max.y), IM_COL32(9, 30, 82, 255), IM_COL32(3, 8, 24, 255));
    // Soft light waves: one triangle strip per wave, bright in the middle and
    // fading out above and below (no overlapping slices, so no seams).
    struct Wave {
        float amp, offset, width, alpha, phase, freq;
    };
    const Wave waves[] = {{0.05f, 0.56f, 0.070f, 0.10f, 0.0f, 2.0f},
                          {0.07f, 0.63f, 0.030f, 0.16f, 1.1f, 2.3f},
                          {0.045f, 0.69f, 0.014f, 0.24f, 2.3f, 1.8f}};
    const int segments = 96;
    const ImVec2 uv = dl->_Data->TexUvWhitePixel;
    for (const Wave& wv : waves) {
        const ImU32 on = IM_COL32(130, 190, 255, static_cast<int>(255 * wv.alpha));
        const ImU32 off = IM_COL32(130, 190, 255, 0);
        const float half = h * wv.width;
        dl->PrimReserve(segments * 12, (segments + 1) * 3);
        const ImDrawIdx base = static_cast<ImDrawIdx>(dl->_VtxCurrentIdx);
        for (int i = 0; i <= segments; ++i) {
            const float t = static_cast<float>(i) / segments;
            const float x = min.x + w * t;
            const float y = min.y + h * (wv.offset + wv.amp * sinf(t * 3.14159f * wv.freq + wv.phase));
            dl->PrimWriteVtx(ImVec2(x, y - half), uv, off);
            dl->PrimWriteVtx(ImVec2(x, y), uv, on);
            dl->PrimWriteVtx(ImVec2(x, y + half), uv, off);
        }
        for (int i = 0; i < segments; ++i) {
            const ImDrawIdx a0 = static_cast<ImDrawIdx>(base + i * 3), b0 = static_cast<ImDrawIdx>(a0 + 3);
            for (int k = 0; k < 2; ++k) {
                const ImDrawIdx a = static_cast<ImDrawIdx>(a0 + k), b = static_cast<ImDrawIdx>(b0 + k);
                dl->PrimWriteIdx(a);
                dl->PrimWriteIdx(b);
                dl->PrimWriteIdx(static_cast<ImDrawIdx>(b + 1));
                dl->PrimWriteIdx(a);
                dl->PrimWriteIdx(static_cast<ImDrawIdx>(b + 1));
                dl->PrimWriteIdx(static_cast<ImDrawIdx>(a + 1));
            }
        }
    }
}

void DrawFocus(ImDrawList* dl, ImVec2 min, ImVec2 max, float rounding) {
    for (int i = 3; i >= 1; --i) {
        const float g = static_cast<float>(i) * 2.0f * ImGui::GetStyle().FramePadding.x / 14.0f;
        dl->AddRect(ImVec2(min.x - g, min.y - g), ImVec2(max.x + g, max.y + g),
                    IM_COL32(150, 200, 255, 30 / i + 10), rounding + g, 0, 2.0f);
    }
    dl->AddRect(min, max, kFocus, rounding, 0, 2.0f);
}

bool NavTab(const char* label, bool selected) {
    ImGui::PushID(label);
    const ImVec2 ts = ImGui::CalcTextSize(label);
    const float pad = ImGui::GetStyle().FramePadding.y;
    const ImVec2 size(ts.x, ts.y + pad * 2.0f);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton("##tab", size);
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddText(p, selected ? IM_COL32_WHITE : (hovered ? IM_COL32(220, 230, 255, 255) : IM_COL32(150, 165, 200, 255)),
                label);
    if (selected) {
        const float bh = (std::max)(2.0f, pad * 0.35f);
        dl->AddRectFilled(ImVec2(p.x, p.y + size.y - bh), ImVec2(p.x + size.x, p.y + size.y), IM_COL32_WHITE, bh);
    }
    ImGui::PopID();
    return clicked;
}

bool Pill(const char* label, bool selected, ImVec2 size) {
    ImGui::PushID(label);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton("##pill", size);
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 q(p.x + size.x, p.y + size.y);
    const float r = size.y * 0.5f;
    if (selected) {
        dl->AddRectFilled(p, q, IM_COL32_WHITE, r);
        if (hovered) DrawFocus(dl, p, q, r);
    } else {
        dl->AddRectFilled(p, q, hovered ? IM_COL32(255, 255, 255, 40) : IM_COL32(255, 255, 255, 20), r);
    }
    const ImVec2 ts = ImGui::CalcTextSize(label);
    dl->AddText(ImVec2(p.x + (size.x - ts.x) * 0.5f, p.y + (size.y - ts.y) * 0.5f),
                selected ? kNavy : ImGui::GetColorU32(ImGuiCol_Text), label);
    ImGui::PopID();
    return clicked;
}

bool Toggle(const char* id, bool* value) {
    const float h = ImGui::GetFrameHeight() * 0.85f;
    const float w = h * 1.85f;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton(id, ImVec2(w, h));
    if (clicked) *value = !*value;
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 q(p.x + w, p.y + h);
    if (*value)
        dl->AddRectFilled(p, q, hovered ? Mix(kBlue, IM_COL32_WHITE, 0.12f) : kBlue, h * 0.5f);
    else
        dl->AddRectFilled(p, q, hovered ? IM_COL32(255, 255, 255, 56) : IM_COL32(255, 255, 255, 38), h * 0.5f);
    const float knob = h * 0.5f - 3.0f;
    const float cx = *value ? q.x - h * 0.5f : p.x + h * 0.5f;
    dl->AddCircleFilled(ImVec2(cx, p.y + h * 0.5f), knob, IM_COL32_WHITE);
    return clicked;
}

void RowLabel(const char* label, const char* hint) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    if (hint) {
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
        ImGui::TextUnformatted(hint);
        ImGui::PopStyleColor();
    }
}

void SameLineRight(float controlWidth) {
    ImGui::SameLine();
    const float x = ImGui::GetWindowContentRegionMax().x - controlWidth;
    if (x > ImGui::GetCursorPosX()) ImGui::SetCursorPosX(x);
}

void Chip(const char* text, ImVec4 color) {
    const ImVec2 ts = ImGui::CalcTextSize(text);
    const ImVec2 pad(10, 3);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const ImVec2 size(ts.x + pad.x * 2, ts.y + pad.y * 2);
    ImGui::Dummy(size);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec4 bg = color;
    bg.w = 0.14f;
    dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), ImGui::ColorConvertFloat4ToU32(bg), size.y * 0.5f);
    dl->AddText(ImVec2(p.x + pad.x, p.y + pad.y), ImGui::ColorConvertFloat4ToU32(color), text);
}

// Primary action: a white button with dark text, glowing when hovered.
bool AccentButton(const char* label, ImVec2 size) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton(label, size);
    const bool hovered = ImGui::IsItemHovered();
    const bool held = ImGui::IsItemActive();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 q(p.x + size.x, p.y + size.y);
    const float r = size.y * 0.5f;
    dl->AddRectFilled(p, q, held ? IM_COL32(215, 225, 245, 255) : IM_COL32_WHITE, r);
    if (hovered) DrawFocus(dl, p, q, r);
    const char* end = ImGui::FindRenderedTextEnd(label);
    const ImVec2 ts = ImGui::CalcTextSize(label, end);
    dl->AddText(ImVec2(p.x + (size.x - ts.x) * 0.5f, p.y + (size.y - ts.y) * 0.5f), kNavy, label, end);
    return clicked;
}

// Secondary action: translucent glass button with white text.
bool GhostButton(const char* label, ImVec2 size) {
    if (size.y <= 0) size.y = ImGui::GetFrameHeight();
    const char* end = ImGui::FindRenderedTextEnd(label);
    const ImVec2 ts = ImGui::CalcTextSize(label, end);
    if (size.x <= 0) size.x = ts.x + ImGui::GetStyle().FramePadding.x * 2;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton(label, size);
    const bool hovered = ImGui::IsItemHovered();
    const bool held = ImGui::IsItemActive();
    const bool disabled = (ImGui::GetItemFlags() & ImGuiItemFlags_Disabled) != 0;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 q(p.x + size.x, p.y + size.y);
    const float r = size.y * 0.5f;
    dl->AddRectFilled(p, q, IM_COL32(255, 255, 255, held ? 60 : hovered ? 44 : 26), r);
    if (hovered && !disabled) DrawFocus(dl, p, q, r);
    dl->AddText(ImVec2(p.x + (size.x - ts.x) * 0.5f, p.y + (size.y - ts.y) * 0.5f),
                ImGui::GetColorU32(ImGuiCol_Text), label, end);
    return clicked;
}

} // namespace nitro_ui

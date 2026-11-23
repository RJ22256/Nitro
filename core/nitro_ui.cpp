#include "nitro_ui.h"

#include <imgui_internal.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <cstdlib>
#include <sys/stat.h>
#endif

#include <algorithm>
#include <cstdio>
#include <string>

namespace nitro_ui {

namespace {

#ifdef _WIN32
std::string FontPath(const char* file) {
    char dir[MAX_PATH] = {};
    GetWindowsDirectoryA(dir, MAX_PATH);
    return std::string(dir) + "\\Fonts\\" + file;
}

bool Exists(const std::string& p) {
    return GetFileAttributesA(p.c_str()) != INVALID_FILE_ATTRIBUTES;
}
#else
// Non-Windows builds (UI previews only): fonts come from NITRO_FONT_DIR.
std::string FontPath(const char* file) {
    const char* dir = std::getenv("NITRO_FONT_DIR");
    return std::string(dir ? dir : ".") + "/" + file;
}

bool Exists(const std::string& p) {
    struct stat st {};
    return stat(p.c_str(), &st) == 0;
}
#endif

ImFont* Load(const char* file, float px) {
    ImGuiIO& io = ImGui::GetIO();
    const std::string path = FontPath(file);
    if (!Exists(path)) return nullptr;
    ImFontConfig cfg;
    cfg.OversampleH = 2;
    cfg.OversampleV = 2;
    static const ImWchar ranges[] = {0x0020, 0x00FF, 0x2013, 0x2026, 0x2190, 0x2193, 0x00D7, 0x00D7, 0};
    return io.Fonts->AddFontFromFileTTF(path.c_str(), px, &cfg, ranges);
}

ImU32 Mix(ImU32 a, ImU32 b, float t) {
    const ImVec4 x = ImGui::ColorConvertU32ToFloat4(a), y = ImGui::ColorConvertU32ToFloat4(b);
    return ImGui::ColorConvertFloat4ToU32(ImVec4(x.x + (y.x - x.x) * t, x.y + (y.y - x.y) * t,
                                                 x.z + (y.z - x.z) * t, x.w + (y.w - x.w) * t));
}

} // namespace

Fonts LoadFonts(float px) {
    Fonts f;
    f.body = Load("segoeui.ttf", px);
    if (!f.body) f.body = ImGui::GetIO().Fonts->AddFontDefault();
    f.bold = Load("seguisb.ttf", px);
    if (!f.bold) f.bold = f.body;
    f.title = Load("segoeuib.ttf", px * 2.4f);
    if (!f.title) f.title = f.bold;
    f.small = Load("segoeui.ttf", px * 0.82f);
    if (!f.small) f.small = f.body;
    ImGui::GetIO().FontDefault = f.body;
    return f;
}

void ApplyStyle(float scale, float windowAlpha) {
    ImGuiStyle& s = ImGui::GetStyle();
    s = ImGuiStyle();
    s.WindowRounding = 16.0f;
    s.ChildRounding = 14.0f;
    s.FrameRounding = 10.0f;
    s.PopupRounding = 10.0f;
    s.GrabRounding = 10.0f;
    s.ScrollbarRounding = 10.0f;
    s.TabRounding = 8.0f;
    s.WindowBorderSize = 1.0f;
    s.ChildBorderSize = 1.0f;
    s.FrameBorderSize = 0.0f;
    s.WindowPadding = ImVec2(18, 16);
    s.FramePadding = ImVec2(12, 8);
    s.ItemSpacing = ImVec2(10, 10);
    s.ItemInnerSpacing = ImVec2(8, 6);
    s.GrabMinSize = 18.0f;
    s.ScrollbarSize = 12.0f;
    s.ScaleAllSizes(scale);

    ImVec4* c = s.Colors;
    const ImVec4 bg(0.055f, 0.067f, 0.141f, windowAlpha);
    const ImVec4 panel(0.086f, 0.102f, 0.200f, 1.0f);
    const ImVec4 panel2(0.114f, 0.133f, 0.259f, 1.0f);
    const ImVec4 border(0.165f, 0.188f, 0.376f, 1.0f);
    const ImVec4 violet(0.478f, 0.361f, 1.000f, 1.0f);
    c[ImGuiCol_Text] = kText;
    c[ImGuiCol_TextDisabled] = kMuted;
    c[ImGuiCol_WindowBg] = bg;
    c[ImGuiCol_ChildBg] = panel;
    c[ImGuiCol_PopupBg] = panel2;
    c[ImGuiCol_Border] = border;
    c[ImGuiCol_FrameBg] = panel2;
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.145f, 0.169f, 0.322f, 1.0f);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.176f, 0.204f, 0.380f, 1.0f);
    c[ImGuiCol_TitleBg] = bg;
    c[ImGuiCol_TitleBgActive] = bg;
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab] = border;
    c[ImGuiCol_CheckMark] = violet;
    c[ImGuiCol_SliderGrab] = violet;
    c[ImGuiCol_SliderGrabActive] = ImVec4(0.0f, 0.898f, 1.0f, 1.0f);
    c[ImGuiCol_Button] = panel2;
    c[ImGuiCol_ButtonHovered] = ImVec4(0.165f, 0.192f, 0.369f, 1.0f);
    c[ImGuiCol_ButtonActive] = ImVec4(0.200f, 0.231f, 0.431f, 1.0f);
    c[ImGuiCol_Header] = ImVec4(0.478f, 0.361f, 1.0f, 0.35f);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.478f, 0.361f, 1.0f, 0.50f);
    c[ImGuiCol_HeaderActive] = ImVec4(0.478f, 0.361f, 1.0f, 0.65f);
    c[ImGuiCol_Separator] = border;
    c[ImGuiCol_NavHighlight] = ImVec4(0.0f, 0.898f, 1.0f, 1.0f);
    c[ImGuiCol_TextSelectedBg] = ImVec4(0.478f, 0.361f, 1.0f, 0.45f);
}

void DrawLogo(ImDrawList* dl, ImVec2 pos, float size) {
    const float k = size / 512.0f;
    const ImVec2 end(pos.x + size, pos.y + size);
    dl->AddRectFilled(pos, end, IM_COL32(27, 18, 64, 255), 116.0f * k);
    dl->AddRectFilled(pos, end, IM_COL32(91, 59, 255, 40), 116.0f * k);

    static const ImVec2 bolt[] = {{300, 52}, {128, 296}, {238, 296}, {202, 460}, {384, 208}, {272, 208}};
    auto shape = [&](float dx, ImVec2* out) {
        for (int i = 0; i < 6; ++i) out[i] = ImVec2(pos.x + (bolt[i].x + 30 + dx) * k, pos.y + bolt[i].y * k);
    };
    ImVec2 pts[6];
    dl->PushClipRect(pos, end, true);
    const float ghostAlpha[] = {0.12f, 0.22f, 0.38f};
    for (int g = 0; g < 3; ++g) {
        shape(-96.0f + 32.0f * static_cast<float>(g), pts);
        dl->AddConcavePolyFilled(pts, 6, IM_COL32(0, 229, 255, static_cast<int>(255 * ghostAlpha[g])));
    }
    shape(0, pts);
    const int v0 = dl->VtxBuffer.Size;
    dl->AddConcavePolyFilled(pts, 6, IM_COL32_WHITE);
    ImGui::ShadeVertsLinearColorGradientKeepAlpha(dl, v0, dl->VtxBuffer.Size, ImVec2(pos.x, pos.y + 52 * k),
                                                  ImVec2(pos.x, pos.y + 460 * k), kCyan, kPink);
    dl->AddPolyline(pts, 6, IM_COL32(255, 255, 255, 220), ImDrawFlags_Closed, (std::max)(1.0f, 6.0f * k));
    dl->PopClipRect();
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
        const int v0 = dl->VtxBuffer.Size;
        dl->AddRectFilled(p, q, IM_COL32_WHITE, r);
        ImGui::ShadeVertsLinearColorGradientKeepAlpha(dl, v0, dl->VtxBuffer.Size, p, q, kCyan, kViolet);
    } else {
        dl->AddRectFilled(p, q, hovered ? IM_COL32(42, 49, 94, 255) : IM_COL32(29, 34, 66, 255), r);
        dl->AddRect(p, q, IM_COL32(42, 48, 96, 255), r);
    }
    const ImVec2 ts = ImGui::CalcTextSize(label);
    dl->AddText(ImVec2(p.x + (size.x - ts.x) * 0.5f, p.y + (size.y - ts.y) * 0.5f),
                selected ? IM_COL32(10, 12, 30, 255) : ImGui::GetColorU32(ImGuiCol_Text), label);
    ImGui::PopID();
    return clicked;
}

bool Toggle(const char* id, bool* value) {
    const float h = ImGui::GetFrameHeight() * 0.85f;
    const float w = h * 1.85f;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton(id, ImVec2(w, h));
    if (clicked) *value = !*value;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 q(p.x + w, p.y + h);
    if (*value) {
        const int v0 = dl->VtxBuffer.Size;
        dl->AddRectFilled(p, q, IM_COL32_WHITE, h * 0.5f);
        ImGui::ShadeVertsLinearColorGradientKeepAlpha(dl, v0, dl->VtxBuffer.Size, p, q, kCyan, kViolet);
    } else {
        dl->AddRectFilled(p, q, ImGui::IsItemHovered() ? IM_COL32(52, 60, 110, 255) : IM_COL32(42, 48, 96, 255),
                          h * 0.5f);
    }
    const float knob = h * 0.5f - 3.0f;
    const float cx = *value ? q.x - h * 0.5f : p.x + h * 0.5f;
    dl->AddCircleFilled(ImVec2(cx, p.y + h * 0.5f), knob, IM_COL32(255, 255, 255, 255));
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
    bg.w = 0.16f;
    dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), ImGui::ColorConvertFloat4ToU32(bg), size.y * 0.5f);
    dl->AddText(ImVec2(p.x + pad.x, p.y + pad.y), ImGui::ColorConvertFloat4ToU32(color), text);
}

bool AccentButton(const char* label, ImVec2 size) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton(label, size);
    const bool hovered = ImGui::IsItemHovered();
    const bool held = ImGui::IsItemActive();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 q(p.x + size.x, p.y + size.y);
    const int v0 = dl->VtxBuffer.Size;
    dl->AddRectFilled(p, q, IM_COL32_WHITE, ImGui::GetStyle().FrameRounding);
    const float t = held ? 0.25f : (hovered ? 0.12f : 0.0f);
    ImGui::ShadeVertsLinearColorGradientKeepAlpha(dl, v0, dl->VtxBuffer.Size, p, q, Mix(kViolet, IM_COL32_WHITE, t),
                                                  Mix(kPink, IM_COL32_WHITE, t));
    const char* end = ImGui::FindRenderedTextEnd(label);
    const ImVec2 ts = ImGui::CalcTextSize(label, end);
    dl->AddText(ImVec2(p.x + (size.x - ts.x) * 0.5f, p.y + (size.y - ts.y) * 0.5f), IM_COL32_WHITE, label, end);
    return clicked;
}

bool GhostButton(const char* label, ImVec2 size) {
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
    const bool clicked = ImGui::Button(label, size);
    ImGui::PopStyleVar();
    return clicked;
}

} // namespace nitro_ui

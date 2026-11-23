// Nitro's look, shared by the app window and the in-game menu (Dear ImGui).
#pragma once

#include <imgui.h>

#include <string>

namespace nitro_ui {

// Palette: PlayStation-style deep blues, white type, one bright blue accent.
constexpr ImU32 kBlue = IM_COL32(0, 112, 209, 255);      // accent (toggles, highlights)
constexpr ImU32 kBlueLight = IM_COL32(92, 170, 255, 255); // links, active details
constexpr ImU32 kBlueDeep = IM_COL32(0, 46, 125, 255);
constexpr ImU32 kNavy = IM_COL32(8, 20, 52, 255);         // text on white
constexpr ImU32 kFocus = IM_COL32(255, 255, 255, 255);    // focus ring / selected
// Older names, kept so existing code reads naturally
constexpr ImU32 kCyan = kBlueLight;
constexpr ImU32 kViolet = kBlue;
constexpr ImU32 kPink = kBlueDeep;
constexpr ImVec4 kText = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
constexpr ImVec4 kMuted = ImVec4(0.663f, 0.722f, 0.851f, 1.0f);
constexpr ImVec4 kGood = ImVec4(0.408f, 0.878f, 0.651f, 1.0f);
constexpr ImVec4 kBad = ImVec4(1.000f, 0.443f, 0.490f, 1.0f);
constexpr ImVec4 kWarn = ImVec4(1.000f, 0.788f, 0.388f, 1.0f);

struct Fonts {
    ImFont* body = nullptr;
    ImFont* bold = nullptr;
    ImFont* title = nullptr; // large, light weight (PlayStation-style headings)
    ImFont* caption = nullptr;
    ImFont* tab = nullptr; // top navigation (Game / Library)
    bool shipped = false; // Nitro's own font (Source Sans 3) was found
};

// Adds a folder to search for fonts (its Fonts subfolder is searched too).
void AddFontDir(const std::string& utf8Dir);

// Loads Source Sans 3 from Nitro's Fonts folder, else Segoe UI, else ImGui's
// built-in font.
Fonts LoadFonts(float px);
void ApplyStyle(float scale, float windowAlpha = 1.0f);

// The Nitro logo: blue tile, speed lines, italic N (tools/make_logo.py).
void DrawLogo(ImDrawList* dl, ImVec2 pos, float size);
// Deep blue background with soft light waves, filling [min, max].
void DrawBackdrop(ImDrawList* dl, ImVec2 min, ImVec2 max);
// White glow ring around a focused/selected item.
void DrawFocus(ImDrawList* dl, ImVec2 min, ImVec2 max, float rounding);

// Widgets
// Top navigation tab: plain text, the selected one white with a bar under it.
bool NavTab(const char* label, bool selected);
bool Pill(const char* label, bool selected, ImVec2 size);
bool Toggle(const char* id, bool* value);
// Label on the left, control placed at the right edge of the current region.
void RowLabel(const char* label, const char* hint = nullptr);
void SameLineRight(float controlWidth);
void Chip(const char* text, ImVec4 color);
bool AccentButton(const char* label, ImVec2 size);
bool GhostButton(const char* label, ImVec2 size);

} // namespace nitro_ui

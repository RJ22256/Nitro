// Nitro's look, shared by the app window and the in-game menu (Dear ImGui).
#pragma once

#include <imgui.h>

#include <string>

namespace nitro_ui {

// Palette (matches the logo)
constexpr ImU32 kCyan = IM_COL32(0, 229, 255, 255);
constexpr ImU32 kViolet = IM_COL32(122, 92, 255, 255);
constexpr ImU32 kPink = IM_COL32(255, 46, 154, 255);
constexpr ImVec4 kText = ImVec4(0.914f, 0.922f, 1.000f, 1.0f);
constexpr ImVec4 kMuted = ImVec4(0.573f, 0.596f, 0.761f, 1.0f);
constexpr ImVec4 kGood = ImVec4(0.302f, 0.878f, 0.659f, 1.0f);
constexpr ImVec4 kBad = ImVec4(1.000f, 0.420f, 0.502f, 1.0f);
constexpr ImVec4 kWarn = ImVec4(1.000f, 0.780f, 0.360f, 1.0f);

struct Fonts {
    ImFont* body = nullptr;
    ImFont* bold = nullptr;
    ImFont* title = nullptr;
    ImFont* caption = nullptr;
    bool sst = false; // Sony's SST typeface was found and is in use
};

// Adds a folder to search for fonts (its Fonts subfolder is searched too).
void AddFontDir(const std::string& utf8Dir);

// Loads SST if its .ttf files are found (a Fonts folder next to Nitro, or
// installed in Windows), else Segoe UI, else ImGui's built-in font.
Fonts LoadFonts(float px);
void ApplyStyle(float scale, float windowAlpha = 1.0f);

// The Nitro logo: tile, trailing ghost bolts, gradient bolt.
void DrawLogo(ImDrawList* dl, ImVec2 pos, float size);

// Widgets
bool Pill(const char* label, bool selected, ImVec2 size);
bool Toggle(const char* id, bool* value);
// Label on the left, control placed at the right edge of the current region.
void RowLabel(const char* label, const char* hint = nullptr);
void SameLineRight(float controlWidth);
void Chip(const char* text, ImVec4 color);
bool AccentButton(const char* label, ImVec2 size);
bool GhostButton(const char* label, ImVec2 size);

} // namespace nitro_ui

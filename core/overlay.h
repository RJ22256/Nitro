// The in-game Nitro menu (Insert by default), drawn with Dear ImGui through a
// D3D11 device: the game's own (DX11), D3D11On12 (DX12) or Nitro's bridge
// device (DX9/10, OpenGL, Vulkan). It is drawn after frame generation, so the
// menu never ends up in the frames LSFG interpolates.
#pragma once

#include <d3d11.h>

namespace lsfgi {

bool OverlayVisible();

// Builds the menu for this real frame. width/height = the target's size.
// Returns false if the overlay can't draw this frame.
bool OverlayBegin(ID3D11Device* dev, ID3D11DeviceContext* ctx, HWND hwnd, UINT width, UINT height);

// Draws the menu built by OverlayBegin onto a render-target-capable texture.
void OverlayDraw(ID3D11DeviceContext* ctx, ID3D11Resource* target);

// Largest visible top-level window of this process (for APIs that don't tell us).
HWND FindMainWindow();

} // namespace lsfgi

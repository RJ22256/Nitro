<div align="center">

<img src="assets/nitro-logo.png" alt="Nitro logo" width="160">

# Nitro

**Frame generation for any DirectX 9 / 10 / 11 / 12, OpenGL or Vulkan game: 2x, 3x or 4x, injected straight into the game.**

Powered by the frame-generation algorithm from your own copy of [Lossless Scaling](https://store.steampowered.com/app/993090/Lossless_Scaling/).

[![Build](https://github.com/RJ22256/Nitro/actions/workflows/build.yml/badge.svg)](https://github.com/RJ22256/Nitro/actions/workflows/build.yml)
[![Release](https://img.shields.io/github/v/release/RJ22256/Nitro?include_prereleases&label=release)](https://github.com/RJ22256/Nitro/releases)
![Platform](https://img.shields.io/badge/platform-Windows%20x64-0078d4)
![API](https://img.shields.io/badge/API-DX9%20%7C%20DX10%20%7C%20DX11%20%7C%20DX12%20%7C%20OpenGL%20%7C%20Vulkan-76b900)

[**Download**](https://github.com/RJ22256/Nitro/releases/latest) · [How to use](#-how-to-use) · [Troubleshooting](#-troubleshooting) · [Building](#-building)

</div>

---

<p align="center">
  <img src="assets/screenshot-app.png" alt="The Nitro app" width="49%">
  <img src="assets/screenshot-ingame.png" alt="The in-game Nitro menu" width="49%">
</p>

## ✨ Features

- **2x / 3x / 4x frame generation** in 64-bit **DirectX 9, 10, 11, 12, OpenGL and Vulkan** games
- **Detects the game's graphics API automatically**, or you can pick it yourself
- **Runs inside the game**: no window capture, no borderless-window requirement, no overlay app running alongside
- **Two ways to load it:** install it once (works with Steam launches), or launch the game with Nitro injected
- **In-game menu** (<kbd>Insert</kbd>): change frame gen, the multiplier, flow scale, performance mode, VSync pacing and HDR live, see rendered and on-screen FPS, and save the settings as your default
- **Live hotkeys:** <kbd>End</kbd> toggles frame gen, <kbd>Home</kbd> cycles 2x → 3x → 4x
- **Quality & performance modes**, adjustable flow scale, HDR support
- **Leaves the game's rendering state untouched:** each API gets its own path (see [How it works](#-how-it-works))
- **Removes cleanly**, and backs up any DLL it replaces (e.g. ReShade's `dxgi.dll`), then restores it

> [!IMPORTANT]
> Nitro ships **none** of Lossless Scaling's code or shaders. It reads them at runtime from the `Lossless.dll` in **your** Lossless Scaling install, so you need to own Lossless Scaling on Steam.

> [!WARNING]
> **Single-player games only.** Injecting into games with anti-cheat can get you banned.

## 🚀 How to use

1. Download **`Nitro-x64.zip`** from [Releases](https://github.com/RJ22256/Nitro/releases) and unzip it.
2. Copy **`Lossless.dll`** from `Steam\steamapps\common\Lossless Scaling\` into the Nitro folder.
3. Run **`Nitro.exe`** and **Browse...** to the game's `.exe`.
   For Unreal Engine games, pick `...\Binaries\Win64\*-Shipping.exe` (Nitro suggests it if it finds one).
4. Choose a **multiplier** and, if you like, a **flow scale** (100% at 1080p, 50–75% at 1440p/4K).
5. Leave **Graphics API** on *Auto-detect* unless the game has several renderers (e.g. a DX12/Vulkan toggle). In that case, pick the one you play with.
6. Then either:
   - **Install** (recommended), then start the game the normal way; or
   - **Launch with injection**, which leaves nothing behind.

What **Install** does for each API:

| API | What Install does |
|---|---|
| DirectX 10 / 11 / 12 | Copies the hook into the game folder as `dxgi.dll` |
| DirectX 9 | Copies the hook into the game folder as `d3d9.dll` |
| OpenGL | Copies the hook in as `version.dll` (or `dxgi.dll`) if the game loads one; otherwise use **Launch with injection** |
| Vulkan | Registers Nitro's Vulkan layer and switches it on **for that exe only**. Nothing is copied into the game folder. |

### In game

| Key | Does |
|---|---|
| <kbd>Insert</kbd> | Opens the Nitro menu. While it's open, the game doesn't receive your mouse and keyboard input. |
| <kbd>End</kbd> | Turns frame generation on/off |
| <kbd>Home</kbd> | Cycles 2x → 3x → 4x |

The menu is drawn *after* frame generation, so it never gets smeared into the generated frames. **Save as default** writes your changes to `Nitro.ini`.

### Tips for smooth results

| Tip | Why |
|---|---|
| Keep **"Pace generated frames with VSync"** on | Each frame gets its own refresh, so a 240 Hz screen at 4x shows 60 real + 180 generated frames |
| Cap the game at **refresh rate ÷ multiplier** | Gives even frame pacing (e.g. 60 FPS on 240 Hz for 4x) |
| Have **≥ 40–60 real FPS** before generating frames | Like any frame generation, it adds about one frame of latency and needs good input frames |

## ⚙️ Settings

Nitro writes `Nitro.ini` next to the hook. That's the game folder for an install, or the Nitro folder for launch mode.

```ini
[Nitro]
LosslessDll=C:\...\Lossless.dll
Multiplier=2          ; 2-4
FlowScale=1.00        ; 0.25-1.00
PerformanceMode=0
HDR=-1                ; -1 auto, 0 off, 1 on
VSyncPacing=1
EnabledAtStart=1
ToggleKey=0x23        ; virtual-key code (End)
CycleKey=0x24         ; (Home)
MenuKey=0x2D          ; (Insert) opens the in-game menu
ShaderOffset=-1       ; -1 auto-detect
ProbeD3D12=1          ; set 0 if a DX12 game won't start with Nitro
Api=auto              ; dxgi, d3d9, opengl, all (written by Install)
```

Vulkan games are listed under `[VulkanGames]` in the `Nitro.ini` next to `Nitro.exe`. Their settings come from that file too.

## 🛠 Troubleshooting

Check **`Nitro.log`** first. It's written next to the hook and records whether the shaders loaded, which graphics API was detected, and the resolution and format.

| Problem | Try |
|---|---|
| Game won't start after installing | **Remove from game folder**, then use **Launch with injection**. For DX12 games, try `ProbeD3D12=0`. |
| Log says *Shader load failed* | Update Lossless Scaling and copy a fresh `Lossless.dll`. |
| Log says *Unsupported back buffer* | The game uses an unusual swap-chain format or an MSAA back buffer. |
| Vulkan game: nothing happens | Check `NitroVk.log` in the Nitro folder. If there isn't one, the game isn't enabled (run **Install** again) or another Vulkan layer is in the way. |
| OpenGL game: log says *WGL_NV_DX_interop2 not supported* | The GPU driver can't share textures between OpenGL and D3D11. Update your driver. |
| DX9 game: generated frames flash or tear | The game presents without VSync. Turn VSync on in the game. |
| No log file at all | The hook never loaded. Usual causes: wrong exe (Unreal stub), a Vulkan/OpenGL game, or anti-cheat. |
| *Access denied* when installing | The game is in a protected folder, so run Nitro as administrator. |

Still stuck? [Open an issue](https://github.com/RJ22256/Nitro/issues) and attach `Nitro.log`.

## 🔬 How it works

```
 game renders frame N ──► Present() ──► Nitro hook
                                          │
                     copy frame N out of the back buffer
                                          │
       LSFG 3.1 compute pipeline (Lossless Scaling's DXBC shaders):
       mip pyramid → feature extraction → 7-level motion estimation
                 → refinement → synthesis of 1-3 in-between frames
                                          │
        present generated frames ──► restore + present real frame N
```

| API | Hook | How frames move |
|---|---|---|
| **DX11** | DXGI `Present` / `ResizeBuffers` | Runs on the game's own device, inside a separate `ID3DDeviceContextState` |
| **DX12** | DXGI `Present` / `ResizeBuffers` | D3D11On12 on the game's own command queue, with the back buffers wrapped |
| **DX10** | DXGI `Present` | Shared textures with Nitro's own D3D11 device |
| **DX9 / 9Ex** | `IDirect3DDevice9::Present` / `PresentEx` / `Reset` (MinHook) | Through system memory to Nitro's D3D11 device (DX9 can't share GPU textures) |
| **OpenGL** | `SwapBuffers` / `wglSwapBuffers` (MinHook) | `WGL_NV_DX_interop2` texture sharing with Nitro's D3D11 device |
| **Vulkan** | Implicit Vulkan layer (`NitroVk.dll`), active only for enabled games | D3D11 textures imported with `VK_KHR_external_memory_win32`; extra `vkAcquireNextImageKHR` + `vkQueuePresentKHR` per generated frame |

- **Shaders:** Lossless Scaling's own Shader Model 5 compute shaders, loaded from `Lossless.dll` and run natively, with no translation layer.
- **Pipeline:** the pass layout follows [lsfg-vk](https://github.com/PancakeTAS/lsfg-vk) v1 (MIT); see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## 🧱 Building

Requires **Visual Studio 2022** ("Desktop development with C++") and **CMake**.

```bat
build.bat
```

The output is in `build\dist`: `Nitro.exe`, `NitroHook.dll`, `NitroVk.dll` and `NitroVk.json`. CMake downloads MinHook and the Vulkan headers automatically. Every push is also built by GitHub Actions, and pushing a `v*` tag publishes a release with `Nitro-x64.zip` attached.

## 📋 Limitations

- 64-bit games only.
- DX9 frames make a round trip through system memory. That's fine at 1080p but costs more at 4K.
- OpenGL needs a driver with `WGL_NV_DX_interop2` (current NVIDIA, AMD and Intel drivers have it), and Vulkan needs `VK_KHR_external_memory_win32`.
- The in-game menu can only block mouse and keyboard input that the game reads through normal window messages or raw input. Games that read DirectInput directly may still react while it's open.
- MSAA back buffers aren't supported (rare in modern games).

## Credits

- Frame-generation algorithm and shaders: **Lossless Scaling** by THS. Nitro is not affiliated with or endorsed by its developer.
- Pipeline reference: **lsfg-vk** by PancakeTAS (MIT).
- UI: **Dear ImGui** by Omar Cornut (MIT). Function hooking: **MinHook** by Tsuda Kageyu (BSD 2-Clause). Vulkan headers by **Khronos** (Apache 2.0).

## Nitro 0.2 — Game Library, AMD FSR 1, 32-bit games 🚀

Adds Lossless Scaling's frame generation to **DirectX 9, 10, 11, 12, OpenGL and Vulkan** single-player games at **2x, 3x or 4x**, or in **Dynamic** mode, which tops your frame rate up to your monitor's refresh rate. It runs inside the game's own rendering, so there's no window capture and no overlay.

### New in this version
- **Game Library:** a new tab lists your Steam, Epic Games and GOG Galaxy games with cover art, the detected graphics API and whether Nitro is installed. Hover a game to install or remove Nitro in one click. Games that ship anti-cheat are flagged and blocked.
- **AMD FSR 1:** RCAS sharpening in every API, and EASU upscaling in DirectX 11/12 games (run the game borderless/windowed at a lower resolution). Available in the app and in the in-game menu.
- **32-bit games:** Nitro now handles 32-bit DirectX 9/10/11, OpenGL and Vulkan games too.
- **Installer:** `Nitro-Setup.exe` with Start menu/desktop shortcuts and a clean uninstaller.
- FSR 3 isn't included: it needs the game's motion vectors and depth, which only the engine has. See the README for details.

### Install
1. Download **Nitro-Setup.exe** below and run it (no admin needed). For a portable copy, use **Nitro-x64.zip** instead and keep all its files together.
2. Run Nitro. The first-run setup finds [Lossless Scaling](https://store.steampowered.com/app/993090/Lossless_Scaling/) on your PC (you need to own it) and builds its frame-gen shaders into Nitro.
3. Pick your game in the **Library** (or browse to its `.exe`), choose a multiplier and click **Install**.
4. Start the game. Press **Insert** for the Nitro menu, **End** to toggle frame gen and **Home** to cycle 2x/3x/4x.

### Heads-up
- This is an early **pre-release** and hasn't been tested on many games yet. If something breaks, open an issue and attach `Nitro.log` from the game folder.
- Single-player games only. Don't use it with anything that has anti-cheat.
- Not affiliated with Lossless Scaling, its developer, or AMD.

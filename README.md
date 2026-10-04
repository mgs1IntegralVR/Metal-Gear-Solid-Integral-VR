# Metal Gear Solid Integral VR

A VR mod for the **GOG PC release of Metal Gear Solid (Integral)**. It hooks the original `mgsi.exe` and renders the game in true stereo 3D, with head tracking, 6DOF leaning, motion controls and Snake's-eye cutscenes, using OpenXR.

> Early, actively developed, and tested on a Meta Quest 2 over Virtual Desktop. Other headsets and runtimes are untested.

## Install (players)

You need:
- The **GOG** version of Metal Gear Solid, untouched
- A 32-bit-capable OpenXR runtime: **Virtual Desktop** (tested) or SteamVR (untested)
- The **Microsoft Visual C++ Redistributable (x86)**

Steps:
1. Open the game folder (the one containing `mgsi.exe`, for GOG Galaxy usually `C:\Program Files (x86)\GOG Galaxy\Games\Metal Gear Solid\`).
2. **Back up the game's own `dinput.dll`** (rename it to `dinput.dll.bak`). The mod replaces it.
3. Copy these three files from the release into that folder:
   - `dinput.dll` (the mod)
   - `openxr_loader.dll` (32-bit Khronos OpenXR loader)
   - `mgs1_vr_config.ini` (settings; every option is documented inside)
4. Start your VR runtime, then launch the game.

To uninstall, delete those three files and rename `dinput.dll.bak` back to `dinput.dll`.

## Controls (defaults)

- **R3 double-press**: switch between VR and the original game on a virtual screen
- **R3 hold**: recentre
- **L3 hold**: in-headset button remapping on your left wrist
- Everything else is listed and remappable in `[controls]` of `mgs1_vr_config.ini`.

## Build (developers)

- Visual Studio 2026 (toolset v145), **Win32 / Release** configuration. `mgsi.exe` is 32-bit; an x64 build will not work.
- Open `mgs1-vr-mod.slnx`. The output is `dinput.dll`.
- Note: the project's post-build step copies `dinput.dll` into the default GOG Galaxy install path. Edit or remove it under *Build Events* if your game lives elsewhere.

Third-party code included: [MinHook](third-party/minhook/LICENSE.txt) (BSD 2-Clause) and the [OpenXR SDK headers](third-party/openxr/LICENSE) (Apache-2.0).

## Legal

This project contains no game code or assets. You must own Metal Gear Solid on GOG. Metal Gear Solid is a trademark of Konami; this project is not affiliated with or endorsed by Konami or GOG.

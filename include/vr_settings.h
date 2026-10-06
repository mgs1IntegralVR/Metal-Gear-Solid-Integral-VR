#pragma once

// ---------------------------------------------------------------------------
// IN-GAME VR SETTINGS PANEL (2026-10-05).
//
// Hold L3 (left stick click) about a second -- in gameplay, in the pause menu,
// on the title screen, anywhere -- and a settings panel floats in front of you.
// It is the end-user face of mgs1_vr_config.ini: every option on it is a key
// in the ini, read from the file when the panel opens and written back to the
// file when you change it, so nothing needs editing by hand and nothing needs
// a relaunch for the options marked live.
//
//   Left/right trigger ........ previous / next tab
//   Either stick up/down ...... pick an option
//   Either stick left/right ... change it (A also steps it / toggles it)
//   Y ......................... put the highlighted option back to its default
//   B or L3 click ............. close
//
// While it is open nothing reaches the game (same rule as the old remap
// panel, which the BUTTONS tab replaces). It is drawn as its own OpenXR quad
// layer, so it shows over the VR view AND over the virtual screen (pause menu,
// codec, title), unlike the wrist HUD which is VR-view only.
//
// Live vs restart: each option says which module re-reads it. Live options
// take effect a moment after you stop changing them (writes are batched so
// the 160 KB ini is not rewritten on every stick tick). Options tagged
// RESTART are saved now and used on the next launch -- they size swapchains
// or install hooks at startup and cannot change under a running session.
//
// Threading: everything here runs on the XR frame thread (vr_input.cpp's
// UpdateOpenXRInput and vr_injection.cpp's layer build are both called from
// XrFrameThreadProc). VrSettingsIsOpen() is atomic and safe from anywhere.
// ---------------------------------------------------------------------------

#include <d3d11.h>
#include <openxr/openxr.h>
#include <vector>
#include <windows.h>

struct VrSettingsInput {
    float sx = 0.0f, sy = 0.0f;          // the stronger of the two sticks
    bool a = false, b = false, y = false;
    bool lTrig = false, rTrig = false;
};

bool VrSettingsIsOpen();
void VrSettingsOpen();
// Saves anything still pending, then closes.
void VrSettingsClose();
// Once per XR frame while open. Consumes the input (nothing goes to the game).
void VrSettingsUpdate(const VrSettingsInput& in, DWORD nowTick);

bool InitVrSettingsPanel(XrSession session, XrSpace playSpace, ID3D11Device* device, ID3D11DeviceContext* context);
void ShutdownVrSettingsPanel();
// Appends the panel's quad while open. headPose = this frame's view pose in
// the play space (the panel is placed in front of it when it opens).
void BuildVrSettingsLayers(const XrPosef& headPose, std::vector<const XrCompositionLayerBaseHeader*>& outLayers);

// --- live re-read hooks implemented by the owning modules ------------------
// vr_input.cpp: [input] [controls] [melee] [weapons] (same as an L3 tap).
void ReloadInputConfigLive();
// vr_injection.cpp: sharpening, edge smoothing, frame pacing, virtual screen size.
void ReloadDisplayLiveSettings();
// camera_write_hook.cpp: cutscene view options, Snake's-eyes stick options, [scope].
// (declared again in camera_write_hook.h)
void ReloadCameraHookLiveSettings();

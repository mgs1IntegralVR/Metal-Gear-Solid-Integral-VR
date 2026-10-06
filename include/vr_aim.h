#pragma once

// ---------------------------------------------------------------------------
// MOTION AIM: controller pose publishing + the OpenXR aim laser.
//
// Two things live here, and they are deliberately together because they must
// agree by SHARED DATA rather than by parallel arithmetic:
//
//   1. The right controller's AIM pose, published once per XR frame.
//   2. A row of soft dots drawn along that same pose as OpenXR quad layers.
//
// Why quad layers rather than geometry in the scene: the laser then lives
// purely in XR space. It is correct in both eyes for free, needs no engine
// hook, no game-space projection, and nothing in MGS1's software rasteriser
// can clip or z-fight it. It also costs no game frames -- the compositor
// draws it, not the 30fps engine.
//
// The laser is ALSO the calibration instrument, and that is its real job
// right now. There is no confirmed bullet-direction address in MGS1 yet, so
// until there is, the flow is: fire at a wall, look at where the holes land
// versus where the dots point, and move the trim until they agree. Once they
// agree, the dots ARE the aim, and any later engine-side aim write can be
// validated against them instead of against a feeling.
//
// GRIP vs AIM pose: the runtime's grip pose runs along the controller handle
// and reads tens of degrees low as a pointing ray -- fine for placing a hand
// model, wrong for aiming. The aim pose is "where this controller points",
// which is what a gun wants. vr_input.cpp already had a grip pose; this adds
// aim alongside it rather than replacing it, because a future weapon model
// will want grip back.
// ---------------------------------------------------------------------------

#include <d3d11.h>
#include <openxr/openxr.h>
#include <vector>
#include <cstdint>

// --- published by vr_input.cpp, once per XR frame ---------------------------

// Store the right controller's aim pose. valid=false while that controller is
// not tracked (asleep, out of view, session unfocused) -- consumers must fall
// back to the game's own aim rather than freezing on a stale pose.
void PublishControllerAim(const XrPosef& aimPose, bool valid);

// The raw pose, in the XR play space. XR FRAME THREAD ONLY.
bool GetControllerAimPose(XrPosef* out);

// The same direction as yaw/pitch in radians, published atomically so the
// GAME thread (camera_write_hook.cpp) can read it for the heading-drive
// experiment without touching XR types or racing the frame thread.
// Trim from the ini is ALREADY APPLIED -- so the angles here and the laser
// dots below describe the same ray by construction, which is the whole point.
bool GetControllerAimAnglesRad(float* yawRad, float* pitchRad);

// --- the laser --------------------------------------------------------------

// Read [aim] from mgs1_vr_config.ini. Safe to call before a session exists.
void LoadAimConfig();

// Create the dot swapchain. Call once the session and D3D11 device are up;
// returns false and disables the laser for the run on failure (fail-soft --
// a missing laser must never cost you the headset image).
bool InitAimLaser(XrSession session, XrSpace playSpace, ID3D11Device* device,
                  ID3D11DeviceContext* context);

// Release everything. Safe to call twice, and on a dead session.
void ShutdownAimLaser();

// Build this frame's laser layer and append it to `outLayers`. The pointer
// appended remains valid until the next call, which is exactly the lifetime
// xrEndFrame needs. Appends nothing when the laser is off, the controller is
// untracked, or init failed.
//
// STATUS 2026-08-14, READ THIS BEFORE TOUCHING ANYTHING HERE. Two headset
// sessions have shown a permanent frame-rate collapse (30 fps -> 3-15 fps,
// surviving a return to third person) that happens with the laser on and does
// NOT happen with it off. It was first blamed on the six quad layers; that was
// wrong -- the single-layer version below collapses too. The one confirmed
// fact is that `laser_enabled=0` is clean and `laser_enabled=1` is not.
//
// The fingerprint is a CONSTANT ~325 ms worst-case DirectDraw Lock() on the
// game thread, in every PERF sample for minutes, with one directly observed
// `lock=321.24ms` on a single capture. A constant means a timeout, not load.
//
// `laser_submit_layer=0` exists to finish the bisection: it runs every bit of
// the swapchain work and skips only the xrEndFrame submission.
//   - still collapses  -> the cost is our swapchain traffic (acquire /
//                         xrWaitSwapchainImage(INFINITE) / UpdateSubresource
//                         on the shared immediate context)
//   - holds 30 fps     -> the cost is the composition layer itself, and one
//                         is already too many for VDXR here
//
// EXACTLY ONE LAYER, ALWAYS. The first version submitted one quad per dot,
// and six alpha-blended quads over two 2496x2688 projection views was enough
// to saturate the Virtual Desktop compositor on a Quest 2. Once it fell
// behind, the game's DirectDraw Lock() started blocking on a busy GPU and the
// process never recovered -- 30 fps to 3 fps, permanently, surviving even a
// return to third person. Confirmed by bisection on 2026-08-14: with the
// laser off the same build held 30.0 fps and 151/151 captured flips for three
// and a half minutes across six FPV transitions.
//
// So the beam is now DRAWN INTO one head-billboarded quad's texture instead
// of being made of quads. The visual is identical; the layer count is 1.
// Do not go back to one-layer-per-dot.
//
// headPose billboards the quad and provides the viewpoint the dots are
// projected from -- position matters now, not just orientation.
void BuildAimLaserLayers(XrTime displayTime,
                         const XrPosef& headPose,
                         std::vector<const XrCompositionLayerBaseHeader*>& outLayers);

// Is the laser configured on? (Cheap; for logging and gating.)
bool IsAimLaserEnabled();

// --- wrist HUD (vr_aim.cpp, 2026-09-23) --------------------------------------
// LIFE/O2 + equipped item on the left wrist, equipped weapon on the right; the
// game's own selection lists move onto the wrist while a grip is held. See the
// block comment in vr_aim.cpp.
void PublishControllerGrips(const XrPosef& left, bool leftValid, const XrPosef& right, bool rightValid,
                            bool leftGripHeld, bool rightGripHeld);
void LoadWristHudConfig();
// Boss / Meryl / other LIFE bars on the left wrist: hooks menu_draw_bar
// (mgsi.exe+68DA6). Call once after MH_Initialize().
bool InstallWristBarHook();
// Captions / codec text on the virtual screen: hooks the PC port's hi-res text
// renderer (mgsi.exe+24020). Call once after MH_Initialize().
bool InstallGameTextHook();
// True while the codec is open (MenuWork state +0x2A == 4, mgsi.exe+325FEA).
bool IsCodecOpen();
bool InitWristHud(XrSession session, XrSpace playSpace, ID3D11Device* device, ID3D11DeviceContext* context);
void ShutdownWristHud();
// XR thread, once per NEW game frame, BEFORE it is uploaded: copies the HUD
// crops for the wrists and (optionally) blanks the corner boxes in the view.
void WristHudOnNewFrame(uint8_t* rgba, int width, int height);
void BuildWristHudLayers(XrTime displayTime, const XrPosef& headPose,
                         std::vector<const XrCompositionLayerBaseHeader*>& outLayers);
// Wrist quick-select (XR thread, from vr_input): holding a grip opens a compact
// list on that wrist; that hand's stick scrolls it; release equips (a quick tap
// toggles the last one, like the game). ownsLeft/ownsRight: the wrist is using
// that grip + stick this frame, so vr_input must not pass them to the game.
void WristSelectInput(bool leftGrip, bool rightGrip, float lx, float ly, float rx, float ry,
                      bool* ownsLeft, bool* ownsRight);

// --- controls & weapons pass (2026-09-29) -----------------------------------

// The head pose from the most recent XR frame (set in BuildAimLaserLayers).
// vr_input's punch / chokehold gestures measure the hands against it.
bool GetLastHeadPose(XrPosef* out);

// IN-HEADSET BUTTON REMAP PANEL. vr_input.cpp owns the remap state and hands a
// snapshot here once per frame; the left wrist shows it (always, not only when
// you look at it) while `open` is true.
struct RemapPanelView {
    bool open = false;
    int  count = 0;                 // rows in use (<= 12)
    int  sel = 0;                   // highlighted row
    bool changed = false;           // unsaved edits -> title says so
    char input[12][12] = {};        // "R TRIGGER"
    char action[12][16] = {};       // "ACTION"
};
void SetRemapPanel(const RemapPanelView& view);

// Left-handed mode: forwards to motion_aim (gun in the left hand, both hand
// models kept on their own controllers).
void SetLeftHandedHands(bool leftHanded);

// Close a wrist quick-select list without equipping (0 = items, 1 = weapons).
void WristSelectCancel(int side);

// A while the item list is open on the left wrist: use the highlighted item
// through the game's own item-window routine. True if a use was queued.
bool WristSelectUseHighlighted();

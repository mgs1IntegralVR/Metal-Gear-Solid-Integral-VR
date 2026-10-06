#pragma once

// ---------------------------------------------------------------------------
// MOTION AIM -- bullets leave your right hand and go where it points.
//
// MGS1's first-person gun fires along the CAMERA, and the camera is the head.
// This decouples them WITHOUT touching the camera at all: it intercepts the
// moment a bullet object is created and rewrites the matrix the bullet is
// built from (muzzle position + orientation) to the controller's pose, mapped
// into game space.
//
// Where it hooks, and why it is safe to hook blind:
//   Okajima\bullet.c has three constructors (task roster: VA 0x5FDA0C,
//   0x5FE57C, 0x5FE6AB). Each allocates its object with
//       push 0x170 / push 5 / call 0x40A30C          (GV_NewActor)
//   BEFORE it reads its MATRIX argument. We redirect only that call's rel32
//   to a thunk, which edits the matrix in place and then jumps on to the real
//   allocator. The signature (the two pushes + a call that resolves to
//   0x40A30C) is verified per site; a mismatch installs nothing.
//
// What it does NOT know yet, and measures instead of guessing:
//   - which stack argument is the matrix      -> found by shape (orthonormal
//     3x3 at 4096 scale, translation within player_radius of the eye)
//   - which row/column of it is "forward"     -> the one that matches the
//     camera look on your first two shots (FPV fires along the camera)
//   - the game camera's right/up handedness   -> correlated from your own
//     head movement while in first person, on the XR thread
// Until all three are settled it PASSES THROUGH and says so in the log.
// ---------------------------------------------------------------------------

#include <cstdint>
#include <openxr/openxr.h>

// [motion_aim] in mgs1_vr_config.ini. Called by InstallMotionAimHooks.
void LoadMotionAimConfig();

// Patch the bullet constructors' allocator calls. Safe to call once, early,
// next to InstallCameraWriteHook. Fail-soft: returns false and changes
// nothing if no site verifies.
bool InstallMotionAimHooks();
void RemoveMotionAimHooks();

// XR FRAME THREAD, once per frame. `trimmedAim` is the right controller's aim
// pose with the [aim] trim already composed on -- the same ray the laser draws,
// so the laser and the bullets agree by construction.
void MotionAimPublishXrFrame(const XrPosef& headPose, const XrPosef& trimmedAim,
                             bool aimValid);

// XR FRAME THREAD, once per eye, after that eye's projection FOV is decided.
// Describes where the game picture actually lands in the headset, so a point
// the laser covers is mapped to the game direction DRAWN under it -- not to
// the raw physical angle, which differs whenever the submitted frustum is
// wider than the rendered one (submitted_hfov_deg=0 does exactly that).
//   submittedFov      : the fov handed to xrEndFrame for this eye
//   sub*              : the imageRect handed to xrEndFrame (pixels)
//   draw*             : where the game picture sits inside that texture
//   visFracX/Y        : fraction of the rendered frame still visible after crop
void MotionAimPublishEyeImage(unsigned eye, const XrFovf& submittedFov,
                              int32_t subX, int32_t subY, int32_t subW, int32_t subH,
                              int32_t drawX, int32_t drawY, int32_t drawW, int32_t drawH,
                              float visFracX, float visFracY);

// INPUT THREAD, every poll: the physical right trigger. Used only to tell the
// player's shots from an enemy's (fire_window_ms).
void MotionAimNoteFireButton(bool down);

// XR thread (vr_injection.cpp), each time a new game image first reaches the
// headset: how old it is (ms since the game frame that drew it), the game's
// frame period and the headset's display period. The held gun is predicted by
// this measured amount when [motion_aim] hand_predict_ms=-1.
void MotionAimNoteImageTiming(float firstShowAgeMs, float gamePeriodMs, float displayPeriodMs);

// True while the game's own weapon model is being driven by the controller
// (gun in hand). Motion aim's bullet redirect stands down while this holds,
// because the weapon then fires from the gun you are holding by itself.
bool IsGunInHandActive();

// XR thread, once per frame: the LEFT controller's grip pose, for Snake's left
// hand ([hands] in the ini). Called from PublishControllerGrips.
void MotionAimPublishLeftGrip(const XrPosef& grip, bool valid);
// Left-handed mode: the RIGHT controller's aim pose, for Snake's right hand
// model (the gun / MotionAimPublishXrFrame then carries the LEFT aim pose).
void MotionAimPublishRightHandAim(const XrPosef& aim, bool valid);
void MotionAimSetLeftHanded(bool leftHanded);
// Rumble requests, taken (and cleared) once per XR frame by vr_input.cpp.
// Shot: 0 none, 1 bullet, 2 missile (Nikita / Stinger), 3 throw.
// Knock: bit 0 = right hand, bit 1 = left hand.
int MotionAimTakeShotRumble();
int MotionAimTakeKnockRumble();

// True once installed, enabled and fully calibrated (i.e. shots are being
// redirected, not just observed).
bool IsMotionAimLive();

// Uses an item (ration, medicine, diazepam...) through the game's own
// item-window routine, on the game thread at Snake's next act. XR thread safe.
void MotionAimRequestItemUse(int itemId);

// True while that hand (0 = left, 1 = right) is resting against a wall, as the
// hands' collision sees it this frame.
bool MotionAimHandTouchingWall(int side);

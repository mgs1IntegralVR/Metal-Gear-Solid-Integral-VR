#pragma once
#include <cstdint>

// ---------------------------------------------------------------------------
// In-frame camera hooks.
//
// TWO detours inside the game's own per-frame camera update:
//
//   RVA 0xE1AF8  rotation  -- yaw / pitch / roll   (593FC8 / 593FCA / 593FCC)
//   RVA 0xE1B58  position  -- X / Y / Z            (593FA0 / 593FA2 / 593FA4)
//
// Both run on the game's own thread, once per game frame, after the game has
// finished computing its own value and before the frame renders. That is the
// only place a write survives -- the game rebuilds the whole camera state
// every frame, so anything written from another thread at another rate loses.
//
// Confirmed axis mapping (by experiment, not by the community table's labels,
// which are transposed):
//   593FC8 = pitch (vertical), 593FCA = yaw (horizontal), 593FCC = roll
// ---------------------------------------------------------------------------

// Called from the OpenXR frame thread whenever a fresh head pose is available.
// Lock-free; safe at full HMD rate.
//   *Rad  : head orientation, radians, OpenXR Y-up YXZ convention
//   pos*  : head position in metres, OpenXR play space (X right, Y up, Z back)
void PublishHeadPose(float yawRad, float pitchRad, float rollRad,
    float posX, float posY, float posZ);

// Current head yaw relative to the reference pose, in radians, with the
// invert_head_yaw setting already applied. Used to make stick movement
// follow where you are looking. Returns false when there is no active
// reference (not in FPV, no pose yet, hook disabled) -- callers should leave
// input untouched in that case.
bool GetHeadYawOffsetRadians(float* outRad);

// Drop the reference pose. The next hooked frame re-pins "where the head is
// now" to "where the game camera is now", so the current head direction
// becomes forward and the current head position becomes centre. Happens
// automatically on every entry into FPV.
void RequestCameraRecenter();

// Installs both detours. Verifies the expected opcode bytes at each site and
// refuses to patch anything on mismatch, so a wrong game build is a clean
// no-op rather than silent corruption. Returns true if the ROTATION hook
// installed (the position hook is optional and reported separately).
bool InstallCameraWriteHook();
void RemoveCameraWriteHook();

// Runtime on/off. The detours stay installed either way -- when disabled they
// reproduce the original instructions exactly.
void SetCameraWriteHookEnabled(bool enabled);

bool IsCameraWriteHookInstalled();
bool IsCameraWriteHookEnabled();
bool IsPositionHookInstalled();

// ---------------------------------------------------------------------------
// THE VIEW COMMIT HOOK -- layer 3, RVA 0x53C47
// ---------------------------------------------------------------------------
// A third detour, and the one that finally reaches cutscenes.
//
// Established 2026-08-23 by disassembling mgsi.exe (claude/view_pipeline_map.md
// has the derivation). rotate2 at 0x593FC8 -- what the two hooks above write --
// is the FIRST of three camera layers:
//
//   LAYER 1  0x593FA0 pos / 0x593FC8 rot
//      | copied at 0x53D4B, but ONLY when [0x593FF9] & 3 == 0
//   LAYER 2  0x593FE0 pos / 0x593FF0 rot / 0x593FE8 target
//      | committed at 0x54477
//   LAYER 3  0x593F60 FROM / 0x593F68 TO / 0x593F7C fov
//      | consumed at 0x1C22, which builds the view matrix
//
// A cutscene runs a camera script that writes layer 2 directly. The copy at
// 0x53D4B never happens, so layer 1 is writable, readable and connected to
// nothing -- which is precisely what the 2026-08-23 yaw pulse measured, and
// why every previous attempt at cutscene VR failed with the writes landing
// perfectly.
//
// Layer 3 is a LOOK-AT PAIR, not an euler triple: direction is TO - FROM.
// Whatever writes those two points owns the camera, unconditionally.
//
// The hook is additive on the game's own committed direction -- read FROM and
// TO, rotate the direction they describe by the head delta, write TO back --
// so the director's framing is preserved, you look around inside the shot, and
// a shot change is followed for free with no re-anchoring heuristic.
//
// Independent corroboration: the community cheat table (PC MGS1 i.CT.xml)
// carries two undescribed entries at exactly 0x593F60 and 0x593F68, and its
// cutscene free-camera hotkey works by setting bit 7 of 0x32279F, which makes
// the whole camera task return at 0x53BCA and leaves layer 3 unowned. Someone
// found this by hand in 2021 and never wrote down what it was.
bool IsViewCommitHookInstalled();

// True while the view commit hook is writing layer 3 this frame. Distinct from
// IsCutsceneVrActive(), which reports the mod's cutscene DETECTION -- this one
// reports whether the camera is actually being driven.
bool IsViewCommitDriving();

// ---------------------------------------------------------------------------
// SNAKE'S POINT OF VIEW IN CUTSCENES
// ---------------------------------------------------------------------------
// True while the cutscene camera's EYE is being placed at Snake's head instead
// of at the position the scene's director chose. The shot's DIRECTION is still
// the game's -- it pans, zooms and cuts exactly as authored; only the point it
// is anchored to moves. R3 double-press leaves all of it for the native
// virtual screen, which is the "watch it as Konami intended" escape hatch.
//
// THE ACTOR IS TAKEN, NOT DERIVED. The position detour at mgsi.exe+E1B58 sits
// inside the game's own first-person camera routine, where `esi` is the actor
// the engine was handed -- ebx was set to esi+0x20 at +E1A74 and esi is
// callee-saved. The detour publishes that register before any of the mod's own
// gates, and the cutscene path reads it.
//
// The earlier derivation through the global at mgsi.exe+334228 (stored by the
// game at +E174B, read back by its own accessors at +DC89A/+DC8A6) was refuted
// by its own probe on 2026-08-24: distTO put that object 13000-17000 units from
// the shot's look-at target, its position never travelled more than ~200 units,
// and [obj+0x9C] read back as 0xF38A06C4 -- not a pointer. It is kept only as a
// selectable fallback (cutscene_snake_pov_actor_source), off by default.
//
// Field layout, unchanged and still correct:
//
//     [obj+0x20] int16 x   [obj+0x24] int16 z     world position
//     [obj+0x28] int16 pitch, [obj+0x2A] yaw      facing
//     *(int16*)(*(void**)(obj+0x9C) + 0x288)      HEAD height
//
// The head field is not a guess: +E1AB4 in the game's own first-person routine
// copies the actor position, then overwrites Y with exactly that value before
// handing it to the camera. Reading the same field means the mod inherits the
// engine's eye height for free, in every stance, without a magic constant.
//
// But the engine puts TWO guards around that read, and the first build of this
// feature copied the read without them:
//
//     +E1AA2  test byte [obj+0x895],8 / jne   -- refuse the head height
//     +E1AC5  cmp word [obj+0xA26],2 / add Y,0x140
//
// Without the first guard the field is read in exactly the states where the
// engine declines to, and returns whatever that memory holds -- measured at
// +31108 and -25482 on consecutive frames, which threw the camera fifteen
// metres out of the room on every shot change. Both guards are now honoured,
// and a reading further than cutscene_snake_pov_eye_max_delta from the actor's
// feet is refused on top of them, falling back to the last separation that was
// accepted. Heads do not move a room's width between frames.
//
// The actor pointer itself is sound, and the constant value it returns is not
// the tell it looks like: 0x900F00 is inside .data's uninitialised tail, i.e.
// the game's static allocation arena, so a fixed address for the player is
// expected rather than suspicious. The rotation words corroborate it -- the
// yaw field carries high bits because the engine masks them off AFTER copying
// (+E1AEF: and word [993FCA],0xFFF), and masking the same way yields angles in
// range.
//
// WHICH OBJECT, NOT WHICH FIELD (2026-08-25). The field sweep answered: the
// actor holds exactly one position, mirrored into +0x044 and +0xA60 by the
// camera routine, and there is no second field that keeps moving when that
// routine stops. And [obj+0x20] is a genuine position after all -- +40241F
// forwards it to +4022EC, which READS it (movsx eax,[ebx]/[ebx+2]/[ebx+4]) as
// a point to look up in a zone table and writes obj+0x848 instead; +5CD367
// calls the same function with the cutscene camera's own position in that
// argument. So the coordinate the probe reports is real. It just does not
// belong to anyone the scene is filming.
//
// That leaves one candidate explanation, and the engine hands us the tool to
// test it. Every object is a node in an intrusive doubly-linked list whose
// heads live in one table -- 0x40A2AF computes head = 0x6BFC98 + class*0x44,
// then [head]=obj, obj+0x4=head, obj+0x0=old head -- and the player is
// allocated with class 5 (+1E1701: push 0xA74 / push 5 / call 0x40A30C), the
// class 211 of the binary's 468 allocation sites use. So the whole cast can be
// walked from an address the engine computes for itself. Actors[] logs every
// node's position and its distance to the shot's look-at target; source mode 3
// puts the eye on the nearest node. If nothing is near that target, actors and
// camera are not in one coordinate space, which is a different bug entirely.
bool IsCutsceneSnakePovActive();

// True while a cutscene is playing in Snake's-eyes mode but the current shot
// has no Snake in it (cutscene_pov_no_snake=1). vr_injection.cpp shows those
// frames on the world-locked virtual screen, the same one native mode and the
// PC menus use; render twice stands by for them. Goes false within ~250 ms of
// the demo ending.
bool IsCutsceneScreenFallbackActive();

// Authoritative "are we in first-person view right now" for the rest of the
// mod. This is the DEBOUNCED state the rotation hook maintains from the game's
// own flag at 0x324898 -- not a guess tracked from our own button presses,
// which drifts out of sync (the logs show fpv_look_active disagreeing with the
// real flag after a double-toggle). Use this for anything that must change
// behaviour in FPV.
bool IsFpvActive();

// True only while the third-person VR "diorama" mode is actually driving the
// camera: false when third_person_vr=0, false in first person, and false for
// the brief guard window after a room cut. Anything that treats "not in FPV"
// as "this is flat 2D UI" must consult this too -- see the 2D-UI framing in
// vr_injection.cpp, which would otherwise shrink the diorama onto a small
// virtual screen.
bool IsThirdPersonVrActive();

// Gameplay Snake's eyes (2026-10-02): the game took the camera out of first
// person during play (elevator, ladder, scripted shot) and the view is being
// rebuilt from Snake's head joint. Also folded into IsThirdPersonVrActive().
bool IsGameplayPovActive();
// Gameplay POV stick steering (2026-10-02): the yaw you are looking along and
// the game camera's own yaw (its stick is relative to that). False when POV is
// not drawing or gameplay_pov_stick_follows_view=0.
bool GetGameplayPovStickYaw(float* viewYaw, float* gameYaw);
// Snake's eyes is drawing Snake in a vehicle (jeep, REX top).
bool IsGameplayPovInVehicle();
// Wall press: hold the game out of first person on purpose (the maintenance
// loop waits) while Snake's eyes draws, anchored at anchorYaw if given.
void SetFirstPersonHold(bool on, bool haveAnchor, float anchorYaw);
bool IsFirstPersonHeld();
// Right stick turns Snake's eyes (REX top by default): the input thread adds yaw.
bool GameplayPovWantsStickTurn();
void AddGameplayPovYaw(float rad);
// The game's own stick frame (pad origin 0x6C03A4): stick UP moves Snake along this yaw.
bool GetGamePadFrameYaw(float* out);
// Wall press: the yaw you were facing when X went down.
bool GetFirstPersonHoldAnchor(float* out);
// Snake's eyes view pair actually drawn (FROM/TO), fresh within 150 ms.
bool GetGameplayPovPair(int16_t from[3], int16_t to[3]);
// REX top: the snake18 task and the DG_OBJS of Snake's body Snake's eyes is using.
bool GetGameplayPovRexBody(uint32_t* obj, uint32_t* objs);

// Render-only hides, applied by motion_aim.cpp around the DG frame actors:
// slot 0 = Snake's body during gameplay Snake's eyes, slot 1 = Snake's demo
// model during cutscene Snake's eyes. 0 = nothing to hide this frame.
uint32_t GetRenderHideObjs(int slot);

// True while cutscene Snake's eyes is drawing and the ini asks for the demo's
// black letterbox bars (Takabe\cinema.c) to be removed.
bool WantDemoLetterboxHidden();
bool WantDemoBottomBarHidden();
// Cutscene Snake's eyes is drawing this frame (demo running, VR mode).
bool IsCutscenePovDrawing();
// [camera_hook] cutscene_pov_subtitle_panel: captions shown on their own panel.
bool IsSubtitlePanelEnabled();

// ---------------------------------------------------------------------------
// CUTSCENE VR
// ---------------------------------------------------------------------------
// Called once per PRESENTED frame, from the DirectDraw Flip hook, on the
// game's own thread. It has to live there rather than in the camera hooks
// because its whole job is to keep working when those hooks stop -- which is
// precisely what cutscenes were suspected of doing, and what this measures.
//
// Three things happen per call, in order:
//   1. Read and edge-log the three candidate state bytes (0x391A0C modal mode,
//      0x32279F "Cutscene control", 0x593FD0 "Camera control"). This runs even
//      with the feature switched off -- it is the Cheat Engine hand-test the
//      handoff has been asking for, run automatically from normal play.
//   2. Measure whether the game's camera update is still running, by watching
//      a counter the rotation hook bumps every time it executes.
//   3. If a cutscene is detected AND the camera update has stopped, drive
//      rotate2 from here (path B). If the camera update is alive, do nothing
//      and let the in-frame hooks handle it (path A).
void CutsceneVrFlipTick(unsigned long long flipCount);

// True while either cutscene-VR path is driving the camera. Anything that
// treats "not in first person" as "this is flat 2D UI" must consult this as
// well as IsThirdPersonVrActive() -- a cutscene rendered in VR is a scene you
// are inside, not a menu to be pushed back onto a small virtual screen.
bool IsCutsceneVrActive();

// True only while the flip-time fallback is doing the writing, i.e. the game's
// camera update is confirmed stopped. The two paths differ in what they can
// do -- notably the fallback is always mono -- so callers may care which.
bool IsCutsceneVrFallbackDriving();

// Alternate-eye stereo. The game re-rasterises from wherever we put the
// camera, so nudging it half an IPD each way on alternate frames produces two
// genuinely different viewpoints -- real geometric disparity at every depth,
// with no extra rendering. Returns 0 (left), 1 (right), or -1 when stereo is
// off and the frame is mono.
int GetCurrentStereoEye();

// Half the stereo baseline in game units (stereo_ipd_mm / 2 x position_scale),
// negated when stereo_swap_eyes=1. Left eye = -value along screen-right.
float GetStereoHalfIpdUnits();
// position_scale: game units per real metre.
float GetPositionScaleUnitsPerMetre();

// ---------------------------------------------------------------------------
// VIEW MODE -- the mod's own, and the authority for the whole mod
// ---------------------------------------------------------------------------
// Two states, chosen by the player with R3 and by nothing else:
//
//   NATIVE  the game exactly as shipped. We write no camera, no FOV, no
//           player heading; stereo is off and the picture is presented on a
//           world-locked virtual screen. The mod is a viewer, not a driver.
//
//   VR      everything the mod does: head-driven camera, 6DOF, alternate-eye
//           stereo, wide FOV, aim laser.
//
// WHY THIS EXISTS, AND WHY IT IS NOT THE GAME'S FPV FLAG. Until 2026-08-23 the
// mod had no mode of its own -- it rode on MGS1's first-person flag at
// 0x324898 and asked for it by synthesising the X key. The game owns that flag
// and revokes it constantly: every cutscene, codec call, vent and most room
// transitions. The mod's response was to guess whether the player still wanted
// first person and re-request it on a three-attempt budget, which produced the
// "FPV auto-restore: gave up after 3 attempts" failure in every session log --
// after which first person was not restored at all, because giving up also
// cleared the guessed intent.
//
// Intent is not something to infer. The player pressed a button; that is the
// intent, it is stored here, and it survives everything the game does to its
// own flag. Re-asserting first person becomes maintenance of a known state
// rather than a guess with a retry budget, and there is no state in which the
// mod concludes the player has stopped wanting what they asked for.
//
// Lock-free; read from the game thread every frame and from the XR frame
// thread, written from the input thread.
bool IsVrViewModeActive();

// Flip the mode. Returns the NEW state (true == VR). Called from the R3
// double-press handler.
bool ToggleVrViewMode();

// Set it outright, for startup defaults and for anything that needs to force a
// known state. Logs the transition.
void SetVrViewMode(bool vrOn);

// Native mode's virtual screen is pinned in the world, so it needs an explicit
// "put it back in front of me" -- posted here by the input layer, consumed by
// the XR frame thread. Also posted automatically on entering native mode.
void RequestVirtualScreenRecenter();
bool ConsumeVirtualScreenRecenter();

// True when the game's own first-person flag is off but the mod's VR mode is
// on -- i.e. we want first person and do not currently have it. The restore
// maintenance in vr_injection.cpp uses this together with the two gates below.
bool VrModeWantsFirstPerson();

// Is now a sane moment to ask the game for first person? False while the
// camera update is stalled (a cutscene, a menu, a loading pause) or while a
// modal UI owns the screen -- exactly the states in which the old retry budget
// was burned on requests the game could never honour. Every "gave up after 3
// attempts" in the 2026-08-23 log was three attempts fired into one of these.
bool FirstPersonRequestIsPlausible();

// ---------------------------------------------------------------------------
// FRAME VIEW RECORD -- which head pose each captured game frame was drawn from
// (added 2026-09-22, the "choppy when looking around / same view twice" fix)
// ---------------------------------------------------------------------------
// The compositor can only hold the world still while your head moves if every
// image it is handed is labelled with the head pose that image was RENDERED
// from. Until now the label was "whatever the headset pose is on the XR frame
// that happens to be copying the picture", re-applied on every XR frame. So
// the newest image was always claimed to be current, nothing was ever
// reprojected, and the world was glued to your face between game frames and
// then jumped -- in each eye at a different moment. That is the judder when
// looking around and the doubled image.
//
// The record travels with the pixels: published by the rotation hook, picked
// up ONCE by the flip that presents that frame (same game thread, same frame),
// handed to the XR thread alongside the RGBA buffer.
struct ViewPoseF {
    float qx = 0, qy = 0, qz = 0, qw = 1;   // OpenXR orientation
    float px = 0, py = 0, pz = 0;           // OpenXR play-space position, metres
};

struct FrameViewRecord {
    bool      valid = false;       // this frame's camera was driven from a head pose
    ViewPoseF eye[2];              // the two eye poses sampled with that head pose
    float     yawEq = 0;           // head yaw/pitch/roll (rad, YXZ) the IMAGE corresponds to
    float     pitchEq = 0;
    float     rollEq = 0;
    int32_t   baseYaw = 0;         // the game's own yaw/pitch that frame (12-bit units),
    int32_t   basePitch = 0;       //   i.e. where stick turning / scripts had it
    int       kYaw = 1;            // head rad -> game units sign (invert_head_*)
    int       kPitch = 1;
    int       clip = 0;            // clip_distance the frame was projected with (0 = unknown)
    unsigned long long tick = 0;   // GetTickCount64 when recorded
    long long qpc = 0;             // QueryPerformanceCounter when recorded (precise; tick is ~15 ms coarse)
    bool      pov = false;         // written by gameplay Snake's eyes (view build), not the rotation hook
};

// Called from the OpenXR frame thread with the SAME sample PublishHeadPose got,
// plus both eye poses. Consistent snapshot; the rotation hook reads angles and
// eye poses together from it.
void PublishHeadViews(float yawRad, float pitchRad, float rollRad,
                      const ViewPoseF& leftEye, const ViewPoseF& rightEye);

// GAME THREAD, at flip: hands over the record for the frame being presented,
// exactly once. Invalid if the rotation hook did not drive this frame's camera.
FrameViewRecord ConsumeFrameViewRecord();

// Any thread: the most recent valid record from the rotation hook, if it is
// younger than maxAgeMs. Motion aim uses its head pose to place the hand
// relative to the camera the game actually drew with.
bool GetLatestFrameViewRecord(FrameViewRecord* out, unsigned maxAgeMs);

// Position hook (position_frame=2) records each camera position it writes and
// the eye part of the offset in it. Motion aim looks up layer-3 FROM here to get
// back to the head centre exactly. False if FROM matches no recent write.
void RecordCameraEyeOffset(const int32_t cam[3], const int32_t eyeOff[3]);
bool LookupEyeOffsetForCamera(const int32_t cam[3], int32_t outEye[3]);

// Body follows head ([camera_hook] body_follows_head). vr_input publishes
// whether the move stick is pushed; while it is (mode 1), Snake's facing is
// turned to where you look. vr_input skips its own stick rotation when enabled.
void PublishMoveStickActive(bool active);
bool IsBodyFollowsHeadEnabled();

// ---------------------------------------------------------------------------
// WEAPONS AND CONTROLS PASS (2026-09-29)
// ---------------------------------------------------------------------------

// PSG1 raise-to-eye scope ([scope] in the ini). vr_aim.cpp decides, on the XR
// thread, when the rifle is up at your eye; the camera hook then writes the
// scope's zoomed clip distance instead of fov_clip_distance, render twice and
// alternate-eye stereo drop the eye offset (a scope is one eye), and the
// headset layer is claimed wider than it was drawn, which is what magnifies.
void SetScopeViewActive(bool active);
bool IsScopeViewActive();
bool IsScopeFeatureEnabled();
// clip_distance written while scoped (read from [scope] zoom_clip_distance).
int  GetScopeClipDistance();
// Horizontal angle, in degrees, the scoped picture is shown across.
float GetScopeDisplayDeg();
// Distances, in metres, for vr_aim's raise/lower test (with hysteresis).
float GetScopeRaiseDistanceM();
float GetScopeLowerDistanceM();
void LoadScopeConfig();

// Snake's position ([actor+0x20], int16 x,y,z) from the actor the engine
// handed its first-person camera routine. False before first person has run
// or if the read fails. Any thread; guarded reads.
bool GetPlayerActorPosition(int16_t out[3]);

// True if an object whose registered source name contains `nameSubstring`
// (e.g. "rmissile") is alive in class list `cls` (0..15), or in any class when
// cls < 0. Guarded reads; safe from any thread, may be wrong for one poll while
// the game is mid-way through creating or freeing that object.
bool IsGameTaskAlive(const char* nameSubstring, int cls);

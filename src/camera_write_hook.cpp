#include <windows.h>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <atomic>
#include <string>

#include "../include/camera_write_hook.h"
#include "../include/debug_logging.h"
#include "../include/render_twice.h"

// ===========================================================================
// Verified against the shipped mgsi.exe (GOG "MGS1 Integral"). ImageBase
// 0x400000, no .reloc section, so it can never be rebased -- every RVA below
// is also the runtime offset from GetModuleHandle(nullptr).
//
// ROTATION SITE, RVA 0xE1AE4..0xE1AFD:
//   4e1ae4:  8b 46 28                 mov  eax,[esi+0x28]     ; actor rotate2
//   4e1ae7:  a3 c8 3f 99 00           mov  ds:0x993fc8,eax    ; DWORD: yaw+pitch
//   4e1aec:  8b 46 2c                 mov  eax,[esi+0x2c]
//   4e1aef:  66 81 25 ca 3f 99 00     and  word ds:0x993fca,0xfff
//            ff 0f
//   4e1af8:  a3 cc 3f 99 00           mov  ds:0x993fcc,eax    ; <-- HOOK (5 bytes)
//   4e1afd:  e8 2e 1d 00 00           call 0x4e3830           ; resume
//
// Hooking the LAST store means our write lands after the game's own yaw/pitch
// store AND after its 12-bit mask, with nothing left in the block to clobber
// it -- and it hands us roll in the same place.
//
// POSITION SITE, RVA 0xE1B36..0xE1B5D:
//   4e1b36:  6a 03 / 57 / 68 a0 3f 99 00
//   4e1b3e:  e8 02 28 f6 ff           call 0x444345           ; GV_NearExp4PV(993FA0, edi, 3)
//   4e1b46:  eb 10                    jmp  0x4e1b58
//   4e1b48:  8b 45 f8                 mov  eax,[ebp-0x8]
//   4e1b4b:  a3 a0 3f 99 00           mov  ds:0x993fa0,eax    ; camera X (+Y in high half)
//   4e1b50:  8b 45 fc                 mov  eax,[ebp-0x4]
//   4e1b53:  a3 a4 3f 99 00           mov  ds:0x993fa4,eax    ; camera Z
//   4e1b58:  8b 03                    mov  eax,[ebx]          ; <-- HOOK (2 bytes)
//   4e1b5a:  8b 5b 04                 mov  ebx,[ebx+0x4]      ; <-- +3 = exactly 5
//   4e1b5d:  a3 b0 42 99 00           mov  ds:0x9942b0,eax    ; resume (clean boundary)
//
// Both branches of the position update converge at 0xE1B58, so that is the
// first point where camera position is final for the frame. Writing position
// from the ROTATION hook would be useless -- the game overwrites it 90 bytes
// later. The two stolen instructions clobber eax and ebx, so the detour
// replays them itself before resuming.
//
// Each replaced region is exactly 5 bytes -- exactly a jmp rel32 -- so both
// are clean whole-instruction swaps with no trampolines and no partial
// instruction hazards. That is why this is hand-rolled rather than MinHook,
// whose detours assume a function entry point with a callable convention.
// ===========================================================================

// File scope (not the anonymous namespace): referenced from inline assembly,
// which MSVC resolves most reliably for plain file statics.
static uintptr_t g_rotResumeAddr = 0;
static uintptr_t g_posResumeAddr = 0;
static uintptr_t g_vcResumeAddr = 0;
static uintptr_t g_vbResumeAddr = 0;
static uintptr_t g_dcResumeAddr = 0;
static uintptr_t g_xformResumeAddr = 0;

namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kTwoPi = 6.28318530717958647692f;
constexpr float kUnitsPerTurn = 4096.0f;   // PSX 12-bit angle: 4096 == 360 deg

// --- code sites -------------------------------------------------------------
constexpr uintptr_t kRotHookRva = 0x000E1AF8;
constexpr uintptr_t kRotResumeRva = 0x000E1AFD;
const uint8_t kRotHookExpected[5] = { 0xA3, 0xCC, 0x3F, 0x99, 0x00 };

constexpr uintptr_t kRot2StoreRva = 0x000E1AE7;              // sanity check only
const uint8_t kRot2StoreExpected[5] = { 0xA3, 0xC8, 0x3F, 0x99, 0x00 };

constexpr uintptr_t kPosHookRva = 0x000E1B58;
constexpr uintptr_t kPosResumeRva = 0x000E1B5D;
const uint8_t kPosHookExpected[5] = { 0x8B, 0x03, 0x8B, 0x5B, 0x04 };

// --- THE VIEW COMMIT HOOK (layer 3) -----------------------------------------
// Established by static analysis of mgsi.exe on 2026-08-23; see
// claude/view_pipeline_map.md for the full derivation. The short version:
//
// rotate2 (0x593FC8) is NOT the camera. It is the first of three layers:
//
//   LAYER 1  0x593FA0 pos / 0x593FC8 rot     <- the two hooks above
//      | copied to layer 2 at 0x53D4B, but ONLY when [0x593FF9] & 3 == 0
//   LAYER 2  0x593FE0 pos / 0x593FF0 rot / 0x593FE8 target
//      | committed to layer 3 at 0x54477
//   LAYER 3  0x593F60 FROM / 0x593F68 TO / 0x593F7C fov
//      | consumed at 0x1C22, which builds the actual view matrix
//
// During a cutscene the game runs a camera script that writes layer 2
// directly, so the copy at 0x53D4B never happens and layer 1 is dead --
// writable, readable, and connected to nothing. That is exactly what the
// 2026-08-23 yaw-pulse experiment measured: the writes landed perfectly and
// the picture did not move.
//
// This site is the last instruction before FROM and TO are pushed into the
// view-matrix builder:
//
//   53C41  push dword [0x593F7C]      ; fov
//   53C47  mov  esi, 0x6BC36C         ; <-- HERE, 5 bytes
//   53C4C  push 0x593F68              ; TO
//   53C51  push 0x593F60              ; FROM
//   53C56  push esi
//   53C57  call 1C22                  ; build the view matrix
//
// Both of the game's camera paths converge on it and the modal-state
// shortcut at 0x53BD7 still reaches it, so it runs on essentially every
// rendered 3D frame -- gameplay, cutscene, codec, everything. The single
// exception is [0x32279F] & 0x80, which returns from the whole camera task at
// 0x53BCA; in that state layer 3 is not refreshed by anyone, so a value we
// wrote earlier simply persists rather than being overwritten.
//
// esi must be reloaded AFTER the register restore -- the pushes below it and
// the call itself use it. Same shape as the position detour.
constexpr uintptr_t kViewCommitHookRva = 0x00053C47;
constexpr uintptr_t kViewCommitResumeRva = 0x00053C4C;
const uint8_t kViewCommitExpected[5] = { 0xBE, 0x6C, 0xC3, 0x6B, 0x00 };

// Layer 3. Three int16 each, X / Y / Z, same axis convention and same units as
// layer 1 (the copy chain preserves them), so every scale and sign the mod
// already calibrated carries over unchanged.
constexpr uintptr_t kViewFromRva = 0x593F60;   // eye point
constexpr uintptr_t kViewToRva = 0x593F68;   // look-at point
constexpr uintptr_t kViewTwistRva = 0x593F78;   // int32, roll about the view axis
constexpr uintptr_t kViewFovRva = 0x593F7C;   // int32, the fov actually pushed
                                              // into the view builder. 0x54477
                                              // fills it from 0x594000 each
                                              // frame, so writing 0x594000 does
                                              // propagate -- but only while the
                                              // camera task is running.
constexpr uintptr_t kSceneDistRva = 0x593FFC;   // int32 eye->target distance

// --- THE VIEW BUILD HOOK (the chokepoint) -----------------------------------
// 2026-08-23, second result. The layer-3 hook above is correct about WHAT the
// renderer reads and wrong about WHEN it runs. During a cutscene MGS1 sets bit
// 7 of 0x32279F itself, and the very first instruction of the camera task is
//
//     53BC2  test byte [0x32279F], 0x80
//     53BCA  jne  53C81          ; return
//
// so the whole task -- including the layer-3 commit and the +53C47 site 133
// bytes into it -- is skipped. The session log proves it: across the 2m35s the
// mod detected as a cutscene, the +53C47 body ran about 60 times instead of
// about 4,500, and 0x32279F went to 0x80 five flips before the detection and
// back to 0x00 on the flip the detection ended. **This is the exact exception
// written into claude/view_pipeline_map.md and then not weighed.**
//
// So the game does not use one camera path. It uses at least 26 -- that is how
// many call sites the view-matrix builder at 0x1C22 has -- and +53C47 is only
// the gameplay one. Rather than guess which of the other 25 a cutscene uses,
// hook the thing they all funnel into.
//
//     1C22  push ebp / mov ebp,esp / sub esp,0x30
//           args: [esp+4] view struct, [esp+8] FROM*, [esp+0C] TO*, [esp+10] fov
//
// Six bytes, so the detour is jmp rel32 + one nop, and the stub replays the
// prologue before resuming at 0x1C28. The body gets the caller's own FROM/TO
// pointers, whichever buffer this particular path chose, and logs the RETURN
// ADDRESS -- which identifies the live path by RVA instead of by inference.
constexpr uintptr_t kViewBuildHookRva = 0x00001C22;
constexpr uintptr_t kViewBuildResumeRva = 0x00001C28;
const uint8_t kViewBuildExpected[6] = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x30 };

// The Snake probe. 0x593FA0 is the base of GM_SnakeCameraWork (0x593FD0 is
// documented as that struct + 0x30, and 0x593FD0 - 0x593FA0 == 0x30), so it is
// the CAMERA, not Snake's actor -- the game lerps it toward a target at
// 0x4E1B39, and the mod has been shoving half an IPD through it every frame
// for weeks without Snake twitching.
//
// But it is not the same thing in both view modes, and that is the useful part.
// Cross-referencing the 2026-08-23 logs:
//
//   first person   0x593FA0 = (-4842,2998,-18470)   layer3 FROM = (-4841,2998,-18533)
//   third person   0x593FA0 = (-1111,1498,16762)    layer3 TO   = (-1111,1348,16762)
//
// In FPV it is the eye (matching FROM to within our own 63-unit IPD offset).
// In third person it is the camera's LOOK-AT TARGET -- X and Z identical to TO,
// Y off by 150 -- which is confirmed structurally by 0x45395A copying it into
// the third-person target at 0x593FA8 whenever [0x6BED20] > 0.
//
// A look-at target that tracks Snake is Snake's world position for free, about
// 150 units below his head, with no GM_PlayerPosition hunt. That address blocks
// Snake's-POV cutscenes, third-person variant (b) and strafing simultaneously.
//
// THE OPEN QUESTION IS WHETHER IT KEEPS TRACKING HIM DURING A CUTSCENE, when a
// scripted camera owns the framing. This probe answers it by measurement rather
// than by inference -- which is the whole lesson of the last two builds.
constexpr uintptr_t kTpvTargetRva = 0x593FA8;   // third-person camera target
constexpr uintptr_t kFpvBranchRva = 0x594002;   // ==1 selects the camera-script path

// --- THE CUTSCENE CAMERA ----------------------------------------------------
// 2026-08-23, third result, and the one that explains every previous failure.
//
// Sniper Wolf's death scene (Snowfield A2) is unambiguously an in-engine 3D
// cutscene -- characters, camera cuts, dialogue -- so something must build a
// view. It is not the camera task, and it is not the view-matrix builder.
//
// EXACTLY ONE instruction in the whole binary sets bit 7 of 0x32279F:
//
//     1CD0A2  or byte [0x32279F], 0x80
//
// and 1,058 bytes later, in the same subsystem:
//
//     1CD292  push ebp / mov ebp,esp / sub esp,0x2C     <- the function
//     1CD299  mov ebx, [ebp+0x0C]                       <- arg1 = camera script state
//     ...
//     1CD4C4  mov ax,[ebx+0x08] -> 0x593F60   FROM x    <- writes LAYER 3 DIRECTLY
//             mov ax,[ebx+0x0A] -> 0x593F62   FROM y
//             mov ax,[ebx+0x0C] -> 0x593F64   FROM z
//             mov ax,[ebx+0x0E] -> 0x593F68   TO   x
//             mov ax,[ebx+0x10] -> 0x593F6A   TO   y
//             mov ax,[ebx+0x12] -> 0x593F6C   TO   z
//     1CD541  call sqrt / 1CD54C call atan2             <- and then computes the
//     1CD56A  mov ax,[ebx+0x08]                            view angles INLINE,
//                                                          re-reading the STRUCT
//
// So during a cutscene the game switches its own camera task off, writes layer
// 3 from script data, and derives the view rotation itself -- without ever
// calling 0x1C22. That is why the chokepoint hook logged exactly one caller
// (+53C5C, the gameplay path) across a 65-second scene, and why neither of the
// other two hooks ever ran.
//
// AND IT IS WHY WE HOOK THE STRUCT, NOT THE GLOBALS. The angle maths at 1CD56A
// re-reads [ebx+0x08] rather than 0x593F60, so overwriting the globals after
// the copy would move the eye and leave the rotation pointing the old way.
// Rewriting the SOURCE before the function consumes it keeps the position, the
// globals and the derived rotation consistent -- one edit, one truth.
constexpr uintptr_t kDemoCamHookRva = 0x001CD292;
constexpr uintptr_t kDemoCamResumeRva = 0x001CD298;
const uint8_t kDemoCamExpected[6] = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x2C };
constexpr uintptr_t kDemoCamFromOffset = 0x08;   // int16 x/y/z within the struct
constexpr uintptr_t kDemoCamToOffset = 0x0E;

// --- THE DEMO'S OWN CAST LIST ----------------------------------------------
// A correction, and an embarrassing one. This hook has always captured
// stack[2] -- arg1, `ebx`, the SCRIPT CHUNK -- and called it "the state". It
// has never once looked at arg0. And arg0, `edi`, is the SCENE object, which
// is where the cast lives:
//
//   0x5CD292(scene, chunk):
//     [chunk+0x18] = count, [chunk+0x1C] = array of 0x34-byte script records
//     for each record: find a node in the list at scene+0x38 whose [node+0x14]
//     equals record[0]; if absent, malloc(0x78), zero it, link it, and
//     rep movsd the whole 0x34-byte record into node+0x14. Mark survivors,
//     free the rest, then run 0x5CEBD2 per node.
//
// So the demo publishes its cast, per shot, in a list hanging off a pointer
// this hook has been handed on every single call. Node layout, from the copy:
//
//   +0x00 prev   +0x04 NEXT   +0x08 alive mark   +0x0C resource handle
//   +0x14 id     +0x18 command (1..0x4A)
//   +0x1C SVECTOR rotation    +0x22 SVECTOR position
//   +0x48, +0x4C written per-frame by 0x5CEBD2
//
// The head is the sentinel at scene+0x38 and the first node is [scene+0x3C];
// 0x5CD439 walks it with `esi = [esi+4]` until it comes back to the head.
constexpr uintptr_t kDemoCastHeadOffset = 0x38;
constexpr uintptr_t kDemoCastFirstOffset = 0x3C;
constexpr uintptr_t kDemoNodeNextOffset = 0x04;
constexpr uintptr_t kDemoNodeAliveOffset = 0x08;
constexpr uintptr_t kDemoNodeResOffset = 0x0C;
constexpr uintptr_t kDemoNodeIdOffset = 0x14;
constexpr uintptr_t kDemoNodeCmdOffset = 0x18;
constexpr uintptr_t kDemoNodeRotOffset = 0x1C;
constexpr uintptr_t kDemoNodePosOffset = 0x22;

// --- THE GTE TRANSFORM SETTER ----------------------------------------------
// 0x407ADA(SVECTOR* translation, SVECTOR* rotation) is this engine's
// SetRotMatrix + SetTransMatrix. It calls 0x44C620 (RotMatrix) on arg1, then
// copies the resulting matrix into the GTE register block at 0x993E40 and
// writes arg0's three shorts into TRX/TRY/TRZ at 0x993E54/58/5C. 0x44AE10
// (ReadRotMatrix) reads that same block back out, which is how the MATRIX
// layout was confirmed.
//
// **424 call sites.** It is the universal per-object transform. NOTHING gets
// drawn without going through it. That is what makes it the right instrument:
// after two weeks of asking memory where the characters are, this asks the
// renderer, and the renderer cannot decline to answer -- if a character is on
// screen, its translation passed through here this frame.
//
// The caller's return address is the other half of the answer. Every RVA maps
// to a task whose name the binary still carries (see claude/task_roster.md),
// so a translation is not an anonymous triple any more: it comes labelled with
// the code that drew it.
constexpr uintptr_t kXformHookRva = 0x00007ADA;   // 0x407ADA
constexpr uintptr_t kXformResumeRva = 0x00007AE0;
const uint8_t kXformExpected[6] = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x20 };

// --- SNAKE HIMSELF ---------------------------------------------------------
// The player actor object, and the reason a cutscene can be watched from his
// eyes rather than from the director's tripod.
//
// Found by disassembly, 2026-08-24, not by scanning. RVA +1E1701 allocates a
// 0xA74-byte object (`push 0xA74 / push 5 / call 0x40A30C`) and RVA +E174B
// stores the pointer:  mov ds:[0x734228], esi.  Two accessors at +DC89A and
// +DC8A6 read it straight back as "the player", confirming what it is.
//
// The same `esi` is what the first-person camera routine walks at +E1A67 to
// build layer 1 -- which is why the field offsets below map one-for-one onto
// the layer-1 globals the mod has been writing for weeks:
//
//     [obj+0x20] int16  ->  0x593FA0   X
//     [obj+0x24] int16  ->  0x593FA4   Z
//     [obj+0x28] dword  ->  0x593FC8   pitch / yaw
//     [obj+0x2C] dword  ->  0x593FCC   roll
//
// The EYE HEIGHT is the interesting one. It is NOT [obj+0x22] (that is the
// actor's base Y). At +E1AB4 the game does
//
//     mov ecx,[esi+0x9C] / mov cx,[ecx+0x288] / mov [ebp-6],cx
//
// i.e. it replaces the Y it just copied with a value read through the model
// pointer at +0x9C. That is Snake's head, and substituting it is precisely how
// MGS1 builds first person. So the mod does not have to invent an eye offset:
// the game already computed one, and we read the same field it does.
//
// WHY THIS IS THE ANSWER FOR CUTSCENES. 0x593FA0 goes stale during a cutscene
// because the CAMERA TASK stops (bit 7 of 0x32279F), not because Snake stops.
// The player object is Snake, the demo animates him, and nothing about the
// camera bypass touches it. That last step is the one claim here that is not
// provable from the disassembly alone -- so the hook logs these fields
// unconditionally every session, driven or not.
constexpr uintptr_t kPlayerObjPtrRva = 0x334228;  // ptr -> player actor
constexpr uintptr_t kPlayerPosOffset = 0x20;      // int16 x, y, z
constexpr uintptr_t kPlayerRotOffset = 0x28;      // int16 pitch, yaw, roll
constexpr uintptr_t kPlayerModelPtrOffset = 0x9C; // ptr -> model/anim state
constexpr uintptr_t kPlayerEyeYOffset = 0x288;    // int16, inside that struct
// The two guards the engine puts around that head read, which the first
// attempt at this feature ignored. +E1AA2 refuses the model head height when
// bit 3 of [obj+0x895] is set; +E1AC5 adds 0x140 to the eye Y when the stance
// word at [obj+0xA26] is 2. Reading the head field in the states where the
// engine declines to is what produced eyeY = +31108 and threw the cutscene
// camera fifteen metres out of the room.
constexpr uintptr_t kPlayerFlagsOffset = 0x895;   // uint8, bit 3 = no head read
constexpr uint8_t   kPlayerNoHeadBit = 0x08;
constexpr uintptr_t kPlayerStanceOffset = 0xA26;  // int16, 2 == the adjusted stance
constexpr int32_t   kStanceEyeAdjust = 0x140;

inline int32_t Abs32(int32_t v) { return v < 0 ? -v : v; }

// --- data addresses (GM_SnakeCameraWork, module-relative) -------------------
// Mapping confirmed IN GAME by isolation testing, and matching PSX SVECTOR
// convention (vx=pitch, vy=yaw, vz=roll). The community Cheat Engine table's
// "RY"/"RX" labels are transposed.
constexpr uintptr_t kPitchOffset = 0x593FC8;  // rotate2.vx -- VERTICAL
constexpr uintptr_t kYawOffset = 0x593FCA;  // rotate2.vy -- HORIZONTAL
constexpr uintptr_t kRollOffset = 0x593FCC;  // rotate2.vz
constexpr uintptr_t kPosXOffset = 0x593FA0;
constexpr uintptr_t kPosYOffset = 0x593FA2;
constexpr uintptr_t kPosZOffset = 0x593FA4;
constexpr uintptr_t kFpvFlagOffset = 0x324898;

// FIELD OF VIEW. Community table (PC MGS1 i.CT.xml) entry 91 labels
// 0x00994000 as "Zoom", a 2-byte value. Module base is 0x400000, so RVA
// 0x594000 -- sitting inside the camera block, right after the position and
// rotation family at 0x593FA0..0x593FCC and immediately before the FPV flag
// the same table puts at 0x594002.
//
// The decomp (handoff doc section 8) says FOV on this engine is the GTE
// projection distance, `clip_distance`, a short defaulting to 320, with
// hfov = 2*atan(160 / clip_distance). 320 -> 53.1 deg, 160 -> 90 deg.
//
// "A 2-byte camera-block field someone labelled Zoom" and "a short called
// clip_distance" are very likely the same thing, but that is a hypothesis
// until the logged value comes back 320. The code below LOGS what it finds
// before it ever writes, so the read confirms or kills the theory even if
// the write is left disabled.
constexpr uintptr_t kFovClipDistanceRva = 0x594000;

// --- state ------------------------------------------------------------------
uintptr_t g_moduleBase = 0;
uint8_t* g_rotHookSite = nullptr;
uint8_t* g_posHookSite = nullptr;
uint8_t  g_rotOriginal[5] = {};
uint8_t  g_posOriginal[5] = {};
bool g_rotInstalled = false;
bool g_posInstalled = false;

uint8_t* g_vcHookSite = nullptr;
uint8_t  g_vcOriginal[5] = {};
bool     g_vcInstalled = false;

uint8_t* g_dcHookSite = nullptr;
uint8_t  g_dcOriginal[6] = {};
bool     g_dcInstalled = false;

uint8_t* g_xformHookSite = nullptr;
uint8_t  g_xformOriginal[6] = {};
bool     g_xformInstalled = false;
std::atomic<unsigned long long> g_dcFrames{ 0 };
std::atomic<bool> g_dcDriving{ false };
int  g_dcMode = 1;      // 0 observe, 1 cutscenes only, 2 always
int  g_dcLogLines = 60;

// --- Snake POV -------------------------------------------------------------
int  g_povMode = 1;        // 0 off (director's camera), 1 on (Snake's eyes)
int  g_povEyeOffset = 0;   // extra game units added to the eye Y (Y is DOWN)
int  g_povFallbackEye = 0; // used only if the head-bone read fails; 0 = base Y
int  g_povEyeMaxDelta = 3000; // how far the head may sit from the feet, in
                              // game units, before the reading is refused
int  g_povProbeLines = 240;
int  g_povProbeEvery = 15;
std::atomic<bool> g_povActive{ false };

// --- SNAKE'S EYES IN CUTSCENES (2026-09-28) --------------------------------
// The demo system keeps its whole cast in ONE place, and the cutscene hook has
// been holding a pointer to it all along. FoxdieTeam decomp, kojo/demoexec.c,
// confirmed instruction-for-instruction against mgsi.exe:
//
//   +1CD292  FrameRunDemo(MGSDEMOACT* act, DMO_DAT* data)   <- our detour
//   +1CECDA  ShowScene(act, DMO_ADJ* adjust)                  one per model
//
//   act+0x30  DMO_DEF* header     header+0x10 n_models, header+0x18 DMO_MDL[]
//   act+0x34  DEMO_MODEL[]        stride 0x1A4; CONTROL at +0 (mov +0, rot +8),
//                                 OBJECT at +0x7C (DG_OBJS* objs at +0x7C)
//   DMO_MDL   stride 0x14         type +0, flag +4, filename +0xC (GV_StrCode)
//   DMO_DAT   eye +8, center +0xE, roll +0x14, clip +0x16,
//             n_adjusts +0x20 (short), adjust +0x24 (offset until converted)
//   DMO_ADJ   stride 0x18         type +0, visible +4, rot +6, pos +0xC,
//                                 n_rots +0x12, rots +0x14 (offset until converted)
//   DG_OBJS   world.t +0x14, n_models +0x2E, objs[] +0x48 stride 0x5C
//             -> joint 6 (Snake's head, the same joint the game's own
//                first-person code reads at +E1AB4) world.t at +0x284
//
// "snake" (GV_StrCode 0x992D) is Snake's model in the demo files checked
// (s0201a0, s0302a0). Other outfits get matched by DG_DEF pointer against the
// last player object the first-person routine handed us, and by the ini list.
// Earlier builds found "no character objects during a cutscene" because demo
// characters are not tasks: they are these DEMO_MODEL entries.
int   g_csView = 1;               // cutscene_view: 0 director camera + head look, 1 Snake's eyes
int   g_csPovNoSnake = 2;         // cutscene_pov_no_snake: 0 director camera, 1 virtual screen, 2 stay in Snake's eyes
int   g_csPovHoldRadius = 9000;   // mode 2: keep his last eye while the shot stays within this of him
int   g_csPovForward = 100;       // eye pushed this far in front of the head joint, game units
int   g_csPovFallbackHead = 700;  // head height above the model origin until a real read lands
int   g_csPovCutUnits = 1500;     // Snake jumping further than this in one frame = new shot, re-face
int   g_csPovYawOffsetDeg = 0;    // escape hatch if the start facing is consistently off
bool  g_csPovInvertPitch = false;
bool  g_csPovPosition = true;
bool  g_csPovStereo = true;
bool  g_csPovFov = true;
int   g_csPovAbsentFrames = 3;    // debounce before a Snake-less shot goes to the screen
int   g_csPovLogLines = 120;
int   g_csPovLogEvery = 30;
uint16_t g_csPovHashes[16] = {};
int   g_csPovHashCount = 0;

// --- SNAKE'S EYES IN GAMEPLAY (2026-10-02) ----------------------------------
// Elevators, ladders and a few other set pieces take the camera away from
// first person: the game switches to a third-person shot (or a scripted
// first-person camera that ignores rotate2), and VR collapsed back to a flat
// view until it was over. The player object is alive through all of them, so
// the view is rebuilt from Snake's own head joint at the one place every
// camera path ends -- the view-matrix builder, gameplay caller +53C5C -- with
// the head turning it, exactly like Snake's eyes in cutscenes.
int   g_gpPov = 1;                // gameplay_pov: 0 off, 1 on
int   g_gpPovForward = 80;        // eye pushed this far in front of the head joint
int   g_gpPovVehBack = 250;       // in a vehicle (jeep gun): eye this far BEHIND the head ...
int   g_gpPovVehUp = 120;         // ... and this far above it, so the gun is not in your face
// Rappel (2026-10-04): on the rope Snake is not sna_init at all -- he is
// chara\rope\rope.c, a separate task with its own copy of his model. Found the
// same way as a vehicle, but the eye sits at his head like on foot.
int   g_gpPovRope = 1;            // gameplay_pov_rope: 0 off, 1 Snake's eyes on the rope
int   g_gpPovRopeForward = 40;    // gameplay_pov_rope_forward_units: eye in front of the head (face is near the wall)
// Ocelot's torture room (2026-10-04): strapped to the rack Snake is not
// sna_init either -- he is chara\torture\torture.c (0x904 bytes, ctor 4F5B03),
// and in the cell sne_03c.c (0x800). Same story as the rope: no live
// sna_init, so Snake's eyes stood down ("no live player object") and the
// game's own camera showed the whole torture and the scripted shots around it.
int   g_gpPovTorture = 1;           // gameplay_pov_torture: 0 off, 1 Snake's eyes on the rack / in the cell
int   g_gpPovTortureForward = 80;   // gameplay_pov_torture_forward_units: eye in front of the head
int   g_gpPovTortureYawOffsetDeg = 0; // gameplay_pov_torture_yaw_offset_deg: added to his body facing (180 if you start facing backwards)
bool  g_gpPovFollowBody = true;   // the view turns when Snake turns (ladder, elevator)
bool  g_gpPovHideBody = true;     // hide Snake's own body for the renderer, like first person
bool  g_gpPovFov = true;          // use the mod's lens (fov_clip_distance)
bool  g_gpPovPosition = true;     // room-scale lean on top
int   g_gpPovLogLines = 60;
int   g_gpPovLogEvery = 30;
std::atomic<unsigned long long> g_gpPovTick{ 0 };   // last frame gameplay POV drew
// Stick steering in gameplay POV (2026-10-02, REX top): the game's own
// third-person stick is relative to ITS camera, which is nowhere near where
// you look. Published each POV frame: the view yaw you see and the game
// camera's yaw, both ratan2(dx, dz) of FROM->TO.
std::atomic<float> g_gpPovViewYaw{ 0.0f };
std::atomic<float> g_gpPovGameYaw{ 0.0f };
std::atomic<bool>  g_gpPovGameYawOk{ false };
int   g_gpPovStickFollowsView = 1;  // gameplay_pov_stick_follows_view: 0 off, 1 REX top + wall press, 2 always
// Snake in a vehicle (jeep, REX top): last tick the POV saw him there. The
// game's own first person in a vehicle is built from the on-foot object's
// stale spot (the "under the map" view), so the POV keeps the vehicle even
// when the game's FPV flag goes up, and first-person maintenance waits.
std::atomic<unsigned long long> g_gpPovVehicleTick{ 0 };
std::atomic<int> g_gpPovVehicleKind{ 0 };   // 1 jeep / tank / other, 2 REX top (snake18), 3 rappel rope (rope.c), 4 torture rack / cell (torture\\*.c)
// Round 10: the hands on top of REX need the view Snake's eyes drew (layer 3
// is the game's own camera again by the time the actors run) and the body.
std::atomic<uint64_t> g_gpPovPairA{ 0 }, g_gpPovPairB{ 0 };   // FROM xyz + TO x / TO yz packed
std::atomic<unsigned long long> g_gpPovPairTick{ 0 };
std::atomic<uint32_t> g_gpPovVehObj{ 0 }, g_gpPovVehObjs{ 0 };
// Right-stick turning inside Snake's eyes (2026-10-02 round 7, REX top: "like a
// normal VR session -- left stick moves, right stick turns"). The input thread
// adds radians here; the POV folds them into its base yaw every frame.
std::atomic<float> g_gpPovYawAdd{ 0.0f };
int   g_gpPovRightStickTurn = 1;    // gameplay_pov_right_stick_turn: 0 off, 1 REX top, 2 any Snake's eyes moment
// REX top: Snake turns to face where you look while the move stick is idle
// (so punches go where you are looking, like first person). Writes rot.vy and
// turn.vy of his CONTROL (+0x2A / +0x6E), only after checking that its mov
// (+0x20) sits on his model.
int   g_gpPovRexFaceView = 1;       // gameplay_pov_rex_face_view
// First-person HOLD (wall press, 2026-10-02): the input side takes the game
// out of first person on purpose so MGS1 can do its own wall press, and asks
// the maintenance loop not to put it straight back. Snake's eyes draws the
// view meanwhile, anchored to where you were looking (not his body, which
// turns his back to the wall).
std::atomic<bool>  g_fpvHold{ false };
std::atomic<bool>  g_fpvHoldAnchorPending{ false };
std::atomic<float> g_fpvHoldAnchorYaw{ 0.0f };
// Render-only hides: [0] Snake's body during gameplay POV, [1] Snake's demo
// model during cutscene POV. motion_aim.cpp applies them around the DG frame
// actors only, so no game actor ever sees the flag changed.
std::atomic<uint32_t> g_renderHideObjs[2] = { {0}, {0} };
std::atomic<unsigned long long> g_renderHideTick[2] = { {0}, {0} };
bool  g_csPovHideSnake = true;    // cutscene_pov_hide_snake
int   g_csLetterbox = 0;
bool  g_csSubPanel = true;       // cutscene_pov_subtitle_panel: captions lifted onto their own panel          // cutscene_pov_letterbox: 0 remove the demo's black bars in Snake's eyes, 1 keep
std::atomic<uint32_t> g_playerModelDef{ 0 };
std::atomic<bool> g_csScreenFallback{ false };
std::atomic<unsigned long long> g_csDemoTick{ 0 };

inline uint16_t GvStrCode(const char* s) {
    uint16_t id = 0;
    for (const unsigned char* p = (const unsigned char*)s; *p; ++p) {
        id = (uint16_t)((id << 5) | (id >> 11));
        id = (uint16_t)(id + *p);
    }
    return id;
}

// --- the player actor, captured from the engine's own register -------------
// Published by the position detour at +E1B58, where `esi` is the actor the
// first-person camera routine was handed. Read by the cutscene path, which
// runs on the same thread but at a time when that routine is not running.
// Lock-free and single-word: a torn read is impossible on x86 for an aligned
// 32-bit value, and a stale one is harmless -- the actor does not move house.
std::atomic<uint32_t> g_playerActor{ 0 };
std::atomic<unsigned> g_playerActorSeen{ 0 };
int g_povActorSource = 1;   // 0 = the +334228 global, 1 = captured only, 2 = captured then global

// --- the field sweep -------------------------------------------------------
// A search with a KNOWN ANSWER, which is why it is worth a build.
//
// (Read with the correction below: [actor+0x20] IS a position field. +40241F
// forwards it to +4022EC, which reads it -- `movsx eax,[ebx]/[ebx+2]/[ebx+4]`
// -- as the point to look up in a zone table, and writes obj+0x848 instead.
// +5CD367 calls the same function with the cutscene camera's own position in
// that argument. The sweep below was built on the opposite belief and its
// result stands anyway: the actor has no second position field.)
//
// But in FPV we know the right answer exactly: 0x593FA0 IS Snake's eye, and
// our position detour fires one instruction after the game writes it. So we
// can sweep the whole 0xA74-byte actor for an int16 triple whose X and Z match
// 0x593FA0, and let the object tell us which offset holds the real position.
// Anything that matches and is NOT 0x20 is a candidate for a field that
// persists when the camera routine stops.
constexpr int kFieldSlots = 14;
int32_t  g_fieldOff[kFieldSlots] = {};
int      g_fieldCount = 0;
int      g_fieldScan = 1;
int      g_fieldScanLines = 24;
int      g_fieldScanEvery = 120;

// The actor-list walk's tunables. They live up here with the other config
// globals, NOT beside EnumerateActors where the rest of that feature sits: the
// ini loader is ~1100 lines above the walk itself and assigns every one of
// them, and this is a single translation unit with no forward declarations.
// Declaring them next to the code that uses them cost a build with 17 errors.

// --- the demo heap sweep ---------------------------------------------------
// Every pointer-based hunt for Snake has now failed for the same reason, and
// the 2026-08-31 log finally states it plainly: cap= and glob= are the SAME
// address and BOTH are dead (rej=2, model=00000000) for essentially the whole
// scene. The player object does not exist during this cutscene. It is built
// afterwards -- the log captures 008FF400 only once the scene ends. The demo
// draws its cast from its own data.
//
// So stop chasing the player pointer and search where the characters actually
// are, with the one known answer this scene hands us for free: the director is
// LOOKING AT them. TO is on or near a character every frame.
//
// WHY THE FIRST SWEEP FOUND NOTHING (2026-09-01, second run). Every hit came
// back at d=0 -- dozens of EXACT copies of TO scattered through the heap, at a
// 0x180 stride matching the per-frame camera records. The sweep found the
// camera's own look-at echoed through the script's interpolation buffers, and
// no character anywhere near it.
//
// Because it could not see them. It read int16 triples only. On PSX the camera
// script stores SVECTOR (int16) -- which is why TO matched to the unit -- but
// an object's world translation lives in a MATRIX: `short m[3][3]; long t[3];`
// The rotation is int16; **the translation is int32**, at offset 0x14 of the
// matrix (0x18 into a GsCOORDINATE2). Characters were never going to appear in
// a 16-bit sweep. That is a format mistake, not a "they are not there".
//
// So the sweep now runs both widths, skips the d==0 echoes of TO by default,
// and covers 1 MB instead of 256 KB (the first run could only read 32-47 of 64
// blocks, so half the window was unmapped anyway).
//
// The +1CD292 hook already holds the demo's script chunk (02C2xxxx). This
// sweeps a window of that heap for int16 (x,y,z) triples lying within
// demo_scan_radius of TO and reports the closest. It is the same
// search-with-a-known-answer that found the actor fields, aimed at the right
// target this time: not "which field of the player" but "what, anywhere in the
// demo's memory, is standing where the camera is pointed".
int g_demoScan = 1;
int g_demoScanEvery = 60;
int g_demoScanLines = 24;
int g_demoScanWindowKb = 1024;  // bytes swept, centred on the script chunk
int g_demoScanRadius = 4000;    // ~2 metres at 2048 units/m
int g_demoScanHits = 10;
int g_demoScanInt32 = 1;        // ALSO sweep 32-bit triples -- see below
int g_demoScanSkipExact = 1;    // drop d==0: those are copies of TO itself
std::atomic<uint32_t> g_demoState{ 0 };

// --- the actor-SIGNATURE scan ----------------------------------------------
// WHY THE COORDINATE SWEEP ABOVE IS BEING RETIRED. It ran at both widths and
// returned nothing at 32 bits at all; every /16 hit was either an exact echo
// of TO (up to 215 per dump) or one of two values repeated at dozens of
// addresses, plus ladders that step smoothly frame to frame -- interpolation
// buffers for the camera path, not bodies. And it could only read 32-90 of its
// 256 blocks: the window was mostly unmapped, so "no hits" mostly meant "did
// not look". A coordinate search also has to be told the answer before it can
// find it, and the whole difficulty here is that Snake's cutscene position is
// the unknown.
//
// So stop searching for a NUMBER and search for a SHAPE. Whatever animates
// Snake during a demo is an actor, and this binary's actors have a layout we
// already know cold, having read the engine's own accessors for it: a model
// pointer at +0x9C (+E1AF8 reads the head through it), an int16 world position
// at +0x20 (+4022EC reads exactly those three shorts as a world point), a
// flags byte at +0x895 (+E1AA2 tests bit 3 there) and a stance word at +0xA26
// (+E1AC5 tests it for 2). Four fields, spread over 2.6 KB, with independent
// constraints -- that is a signature, and a signature is selective enough to
// sweep without knowing where Snake is.
//
// It also fixes the coverage problem properly. Instead of a fixed window
// around the script chunk, VirtualQuery walks the address space and the scan
// visits every committed, readable, private region up to a byte budget. No
// more counting unreadable blocks.
int g_actorSigScan = 1;
int g_actorSigEvery = 60;
int g_actorSigLines = 24;
int g_actorSigHits = 12;
int g_actorSigRadius = 24000;   // ~12 m: wide, because TO frames a shot, not a body
int g_actorSigBudgetMb = 96;    // hard cap on bytes examined per sweep
int g_actorSigRequireFlags = 0; // 1 = also demand [+0x895] != 0

// --- the task-NAME walk ----------------------------------------------------
// THE THING THAT ENDS THE GUESSING. Every object in this game is created by
// 0x40A30C(class, size) and then registered by 0x40A347(obj, init, update,
// name) -- and `name` is a POINTER TO THE ORIGINAL SOURCE FILE PATH, which the
// shipped binary still carries. The player's is the literal string
// "C:\mgs\source\chara\snake\sna_init.c". 477 registrations, 426 distinct
// files, still in .rdata.
//
// So the header of EVERY object is known exactly, from 0x40A2AF and 0x40A347:
//   +0x00 next   +0x04 prev   +0x08 init fn   +0x0C update fn
//   +0x10 free fn   +0x14 NAME   +0x18 = 0   +0x1C = 0
// and 0x40A2AF links it at the FRONT of the list whose head is
// 0x6BFC98 + class*0x44.
//
// That makes the walk self-validating for the first time in this hunt. If the
// names come back as "C:\mgs\source\..." the walk is right; if they come back
// as garbage it is wrong, and there is no third case to talk myself into.
//
// AND IT EXPLAINS THE CLASS-5 FAILURE. Class 5 is the player's class, but the
// cutscene cast is not class 5:
//   class 5  chara\snake\sna_init.c (0xA74) -- the PLAYER, and scene props
//   class 4  animal\zako*, animal\meryl*, animal\ninja, animal\liquid,
//            animal\snake18 (0x940), Okajima\11b_demo, Takabe\mg_demo1
//   class 6  Koba\demo\demomngr.c, Takabe\democame.c, chara\meryl07b
//   class 7  chara\others\intr_cam.c, chara\psyco
// The characters a demo animates are CLASS 4. I walked class 5 and concluded
// there was no cast. There was; it was one list over.
constexpr uintptr_t kTaskInitOffset = 0x08;
constexpr uintptr_t kTaskUpdateOffset = 0x0C;
constexpr uintptr_t kTaskNameOffset = 0x14;

// The transform log.
//
// THE FIRST VERSION THREW THE ANSWER AWAY. It kept one slot per caller and
// overwrote it on every call, so with `+4ED96` drawing 120 objects a frame it
// reported object 120 and discarded the other 119. The character was in there;
// the logger binned it.
//
// The translations are WORLD SPACE -- proved, not assumed: caller +C48C
// reported (-1500,0,24250), which is exactly the [+0x20] of the
// `Thing\door.c` task at 008FF3A0 in the same log. Two independent readings of
// one door agreeing to the unit.
//
// So rank by distance to TO instead. The director is looking AT a character,
// so the drawn object nearest the look-at point is the character -- and the
// caller RVA still names the code that drew it.
struct XformSlot {
    uint32_t callerRva;
    int32_t  x, y, z;
    int32_t  dist;
    uint32_t count;
};
constexpr int kXformSlots = 16;
// Per-caller call counts, kept separately now that the slots rank by distance.
struct XformCaller { uint32_t rva; uint32_t count; };
constexpr int kXformCallers = 12;
XformCaller g_xformCallers[kXformCallers] = {};
std::atomic<int> g_xformCallerCount{ 0 };
std::atomic<int32_t> g_xformToX{ 0 }, g_xformToY{ 0 }, g_xformToZ{ 0 };
XformSlot g_xformSlots[kXformSlots] = {};
std::atomic<int> g_xformUsed{ 0 };
std::atomic<uint32_t> g_xformCalls{ 0 };
int g_xformScan = 1;
int g_xformEvery = 30;
int g_xformLines = 40;

int g_demoCastScan = 1;
int g_demoCastEvery = 30;
int g_demoCastLines = 60;
int g_demoCastMax = 32;
std::atomic<uint32_t> g_demoScene{ 0 };

int g_taskScan = 1;
int g_taskScanEvery = 60;
int g_taskScanLines = 40;
int g_taskScanMax = 64;         // nodes per class before giving up
int g_taskScanClassMask = 0xFFFF;
std::atomic<uint32_t> g_snakeTask{ 0 };

int g_classScan = 1;
int g_classScanEvery = 60;
int g_classScanLines = 20;

int g_actorListScan = 1;
int g_actorListClass = 5;
int g_actorListEvery = 60;
int g_actorListLines = 30;
int g_actorListMax = 48;
std::atomic<uint32_t> g_listActor{ 0 };
std::atomic<int32_t>  g_listActorDist{ 0 };
std::atomic<int>      g_listActorCount{ 0 };

inline void NotePlayerActor(uint32_t p) {
    // No memory reads here -- this runs inside a detour on the game's own
    // thread, before any of the mod's gates, and must stay a few instructions.
    // A user-space heap or static-arena pointer, aligned. Anything else is a
    // register we misread and is refused rather than published.
    if (p < 0x00400000u || p >= 0x7FFF0000u || (p & 3u) != 0u) return;
    g_playerActor.store(p, std::memory_order_relaxed);
    g_playerActorSeen.fetch_add(1, std::memory_order_relaxed);
}

uint8_t* g_vbHookSite = nullptr;
uint8_t  g_vbOriginal[6] = {};
bool     g_vbInstalled = false;
std::atomic<unsigned long long> g_vbFrames{ 0 };
std::atomic<bool> g_vbDriving{ false };
// Set by the layer-3 hook when it wrote, CONSUMED by the view-build hook.
// 0x53C47 always runs before 0x1C22 inside one frame, so an exchange here
// answers "did layer 3 already apply the head delta to this very pair?".
// A plain sticky flag would latch true and suppress the build hook forever the
// moment the camera task stopped running -- the exact state this is all for.
std::atomic<bool> g_vcDroveThisBuild{ false };

// mode: 0 = observe only, 1 = drive during cutscenes only, 2 = drive always
int  g_vbMode = 1;
// When non-zero, only drive when the caller's return address matches this RVA.
// Zero drives on every path -- correct while we do not yet know which path a
// cutscene uses, and the thing to narrow once the log says.
unsigned g_vbCallerRva = 0;
int  g_vbLogCallers = 24;
bool g_vbSnakeProbe = true;
int  g_vbSnakeEvery = 30;
int  g_vbSnakeLines = 240;

// Distinct caller return addresses seen, so the log names each path once
// instead of once per frame. Game thread only.
constexpr int kVbMaxCallers = 32;
unsigned g_vbSeenCallers[kVbMaxCallers] = { 0 };
int      g_vbSeenCount = 0;

// --- view-commit hook state (game thread only, except the atomics) ----------
// This hook keeps its OWN reference pose. It cannot borrow the rotation
// hook's: during a cutscene the rotation hook does not run at all, so
// g_haveReference is stale or absent exactly when this one needs it most.
bool  g_vcHaveRef = false;
float g_vcRefHeadYaw = 0.0f, g_vcRefHeadPitch = 0.0f, g_vcRefHeadRoll = 0.0f;
float g_vcRefHeadPosX = 0.0f, g_vcRefHeadPosY = 0.0f, g_vcRefHeadPosZ = 0.0f;
std::atomic<bool> g_vcRecenter{ false };
std::atomic<bool> g_vcDriving{ false };   // did we write layer 3 last frame?
std::atomic<unsigned long long> g_vcFrames{ 0 };

// mode: 0 = observe only (read and log layer 3, write nothing)
//       1 = drive during cutscenes only (default)
//       2 = drive always
int  g_vcMode = 1;
bool g_vcRotation = true;
bool g_vcPosition = true;
bool g_vcStereo = true;
bool g_vcFov = false;
int  g_vcRotationPercent = 100;
int  g_vcMinDist = 256;
int  g_vcDefaultDist = 1000;
int  g_vcLogFrames = 240;

std::atomic<bool> g_enabled{ false };
std::atomic<bool> g_havePose{ false };
std::atomic<bool> g_recenterRequested{ false };

std::atomic<float> g_headYaw{ 0.0f };
std::atomic<float> g_headPitch{ 0.0f };
std::atomic<float> g_headRoll{ 0.0f };
std::atomic<float> g_headPosX{ 0.0f };
std::atomic<float> g_headPosY{ 0.0f };
std::atomic<float> g_headPosZ{ 0.0f };

// Head yaw offset from reference, published for the movement rotation in
// dllmain.cpp. Written by the rotation hook, read by the input hook.
std::atomic<float> g_yawOffsetRad{ 0.0f };
std::atomic<bool>  g_yawOffsetValid{ false };

// Reference pose. Only touched inside the hooks, which are single-threaded on
// the game's thread.
bool  g_haveReference = false;
bool  g_lastFpvState = false;
// FPV flag debounce. The flag at 0x324898 is noisy: entering FPV produces
// three transitions inside ~200ms, and it briefly drops on its own mid-play.
// Every transition used to drop the reference pose, which snaps the view back
// to the game's angle -- that is a large part of "rotating feels wonky".
// We now require the raw flag to hold its new value for N consecutive hooked
// frames before believing it.
int   g_fpvPendingFrames = 0;
float g_refHeadYaw = 0.0f, g_refHeadPitch = 0.0f, g_refHeadRoll = 0.0f;
float g_refHeadPosX = 0.0f, g_refHeadPosY = 0.0f, g_refHeadPosZ = 0.0f;
int32_t g_refGameYaw = 0, g_refGamePitch = 0, g_refGameRoll = 0;

// Position offset we applied last frame, so it can be un-applied before the
// game's own position update runs. See the note in the rotation hook body.
int32_t g_prevPosOffX = 0, g_prevPosOffY = 0, g_prevPosOffZ = 0;
bool    g_havePrevPosOffset = false;

// --- tunables (mgs1_vr_config.ini, [camera_hook]) ---------------------------
bool g_writeYaw = true;
bool g_writePitch = true;
bool g_writeRoll = false;
// Inverts now apply to the HEAD axes, BEFORE routing -- so the names finally
// mean what they say. (They used to be applied after the swap, which made
// "invert_yaw" flip the vertical. That footgun is gone.)
bool g_invertHeadYaw = true;
bool g_invertHeadPitch = true;
bool g_invertHeadRoll = false;
bool g_swapYawPitch = true;
// Add the head delta ON TOP of the game's own live camera angle each frame,
// instead of pinning to the angle captured at FPV entry. See the note in the
// rotation body -- this is what lets stick-turning still work.
bool g_additiveRotation = true;
// BODY FOLLOWS HEAD (2026-09-23, [camera_hook] body_follows_head).
// In first person the stick walks Snake along HIS facing, and the head only
// turns the camera on top of it -- so looking left and pushing forward walks
// you straight on. 1 = while the move stick is pushed, Snake's facing is
// turned to where you are looking (rot.vy [actor+0x2A] and turn.vy
// [actor+0x6E], CONTROL layout from the decomp: mov +0x20, rot +0x28, step
// +0x64, turn +0x6C) and the head reference is re-anchored by the same angle,
// so the VIEW does not move at all -- only the body does. 2 = always (the body
// tracks your head even standing still). 0 = off (turn with the right stick).
int  g_bodyFollowsHead = 1;
std::atomic<bool> g_moveStickActive{ false };
int32_t g_absorbAccum = 0;     // total yaw units handed from head to body (keeps the frame record's
                               // baseYaw continuous, so the stereo stick-turn correction ignores it)
int  g_fpvDebounceFrames = 6;      // consecutive frames before a flip is real
int  g_logFrames = 240;

bool  g_positionEnabled = false;
float g_positionScale = 1000.0f;   // game units per metre -- NEEDS CALIBRATION
int   g_maxPositionOffset = 400;   // clamp, game units, per axis
bool  g_invertPosX = false;
bool  g_invertPosY = true;         // PSX Y is normally DOWN
bool  g_invertPosZ = false;
bool  g_rotatePositionWithView = true;
float g_positionYawOffsetRad = 0.0f;
// Mirror the horizontal lean AND reverse the yaw together -- the only way to
// express an XR-to-game handedness change. See the long comment at the
// rotation site; do not "simplify" this into another invert_position_* flag.
bool  g_positionHandednessFlip = false;
// POSITION FRAME (2026-09-23). 2 = map head offsets and the stereo eye offset
// through the game camera's OWN basis, read out of the view-matrix builder
// (DG_LookAt at mgsi.exe+1C22) rather than guessed with flip/offset flags.
// 0 = the legacy invert/handedness/yaw-offset path, kept for A/B.
int   g_positionFrame = 2;
// Diagnostic: ignore the head entirely and shove the camera left/right by a
// fixed number of game units, flipping every 60 hooked frames. Non-zero makes
// the view visibly rock side to side IF position writes reach the renderer at
// all. This separates "the hook does nothing" from "the scale is too small".
int   g_positionTestUnits = 0;

// --- alternate-eye stereo ----------------------------------------------------
// The mod has been sending one mono image to both eyes with a uniform pixel
// shift. A uniform shift is not stereo: it moves near and far pixels by the
// same amount, so there is zero disparity and therefore zero depth. It is the
// reason the picture reads as flat no matter how well head tracking works.
//
// This does it properly, and cheaply, using machinery we already own. The game
// re-rasterises the whole scene from wherever we put the camera -- that is what
// makes leaning produce true parallax. So we nudge the camera half an IPD left
// on one game frame and half an IPD right on the next, tag each captured frame
// with the eye it belongs to, and hold one image per eye. Both eyes then show
// genuinely different viewpoints: real geometric disparity, correct at every
// depth, with NO extra rendering cost.
//
// The honest trade: at the engine's 30 fps cap each eye refreshes at 15 Hz, and
// during fast motion the two eyes show slightly different moments in time. For
// MGS1's pacing that may be fine; it is a toggle so it can be judged rather
// than argued about.
bool  g_stereoAlternate = false;
int   g_stereoIpdMm = 64;      // interpupillary distance, millimetres
bool  g_stereoSwapEyes = false;  // if depth reads inverted / eyes feel crossed
int   g_stereoEye = 0;       // which eye THIS game frame is being rendered for
// -1 = stereo off (mono), 0 = left, 1 = right, 2 = HOLD (write this frame to
// both eyes). Read by the capture, on another thread.
std::atomic<int> g_publishedEye{ -1 };

// WHEN the eye was last published, and how long a published eye stays valid.
//
// This exists because of a real, reproduced bug. The eye tag is advanced from
// the ROTATION HOOK, which only runs while the game is running its own camera
// update. During a cutscene, a codec call or the pause menu the game stops
// calling it -- so the last eye value just SAT THERE, and every frame captured
// for the whole scene got tagged with it. One eye received the entire
// cutscene; the other kept showing the last frame of gameplay from before it
// started. Exactly the "cutscene in one eye, old gameplay in the other"
// symptom.
//
// Fix: the tag has a shelf life. If the game thread has not refreshed it
// within this window, the reader treats the frame as MONO, which is the
// correct answer for anything the camera update is not driving.
std::atomic<unsigned long long> g_eyePublishTick{ 0 };
int g_stereoEyeTimeoutMs = 120;   // ~4 game frames at 30fps

// --- motion hold -------------------------------------------------------------
// Alternate-eye stereo's one real weakness: the two eyes hold frames from
// different moments. Standing still that is invisible, because nothing moved
// between them. TURNING, it is very visible -- the eyes differ by however far
// the view rotated in 33ms, which is far larger than an IPD and reads as
// flicker or rivalry rather than depth.
//
// So while the view is turning quickly we stop alternating and feed both eyes
// the same newest frame. Depth disappears for the duration, which nobody
// notices mid-turn, and comes back as soon as you settle. This is the standard
// trade for this technique and it costs nothing.
int   g_stereoMotionDegPerFrame = 2;   // above this, hold mono
int   g_stereoHoldFrames = 10;  // and stay held this long after
int   g_stereoHoldCounter = 0;
int32_t g_prevFrameYawUnits = -1;

// The three gaps the original yaw-only detector had, and why each one ghosted:
//
//  (a) PITCH was never measured. A vertical head swing separates the two eyes
//      in time exactly as far as a horizontal one does, and never armed the
//      hold, so it ghosted at full strength with no ini value able to help.
//  (b) TRANSLATION was never measured, and this is the big one. Walking
//      changes no angle at all. Every frame of every walk therefore kept
//      alternating with the two eyes 33 ms apart -- the "ghosting while moving
//      the player character" half of the report, structurally unfixable from
//      the ini.
//  (c) The detector was REACTIVE: it compared the game's angle to last
//      frame's, so the hold armed on the frame AFTER the one that had already
//      shipped a mismatched pair. Every swing leaked its first frame. The head
//      pose LEADS the game camera (we write the camera from it), so arming off
//      HMD angular velocity closes that frame.
int   g_stereoMotionUnitsPerFrame = 8;   // translation threshold, game units/frame
int32_t g_prevFramePitchUnits = 0;
int32_t g_prevGamePosX = 0, g_prevGamePosY = 0, g_prevGamePosZ = 0;
bool    g_havePrevGamePos = false;
float   g_prevMotionHeadYaw = 0.0f, g_prevMotionHeadPitch = 0.0f;
bool    g_haveMotionHeadRef = false;

// Alternate-eye stereo splits the game's frame rate between your eyes. At 30
// fps that is 15 Hz each, which is the deal we accepted. But menus, codec
// calls and heavy scenes (water) run far slower, and 9 fps would mean 4.5 Hz
// per eye -- unusable, and probably a real part of why menus feel awful. So
// below a floor we stop alternating entirely and both eyes track the game.
int   g_stereoMinFps = 24;

// --- player heading ---------------------------------------------------------
// Found by static analysis of mgsi.exe, from the goldmine table's FPV routine.
//
// At mgsi.exe+53D86, on entering first-person view, the game does:
//     movswl 0x993fca,%eax     ; the camera yaw -- the one we hook
//     add    $0x800,%eax       ; +180 degrees (the camera looks AT Snake)
//     call   0x40ac17
// and 0x40AC17 is a two-instruction setter:
//     mov 0x4(%esp),%eax
//     mov %eax,0x6c03a4        ; <-- a single global heading
//     ret
// with a matching getter at 0x40AC21, and eleven callers across the camera
// mode routines.
//
// So 0x6C03A4 (RVA 0x2C03A4) is the game's current heading frame, captured
// ONCE when the view mode changes. That is almost certainly the thing behind
// both open complaints: movement staying locked to where the camera was at FPV
// entry, and gunfire going where Snake faces rather than where you look --
// because the game keeps using a heading from the moment you pressed the
// button, while we move the camera every frame afterwards.
//
// Writing it every frame should make the game's own notion of "forward" track
// your head. This is a hypothesis from reading the binary, not something
// confirmed in play -- if Snake spins or movement goes strange, set
// write_player_heading=0 and tell me what it did.
constexpr uintptr_t kPlayerHeadingRva = 0x2C03A4;
bool  g_writePlayerHeading = true;
int   g_playerHeadingOffsetUnits = 2048;   // 0x800 = 180 degrees, as the game uses

// --- FOV ---------------------------------------------------------------
// 0 = observe only (read and log, never write). Any other value is written
// to clip_distance every frame. Writing EVERY frame is deliberate and
// necessary: the field is per-camera and script-overridable, so a one-shot
// poke would be undone at the next room, cutscene or camera change.
int  g_fovClipDistance = 0;
bool g_fovFirstPersonOnly = true;

// --- PSG1 raise-to-eye scope ([scope], 2026-09-29) ---------------------------
// Set from the XR thread (vr_aim.cpp) while the rifle is up at your eye. The
// FOV write below then writes the scope's own zoomed clip distance, and the
// per-eye offset is dropped: a scope is one eye, and a magnified picture with a
// full IPD between the eyes would be double the disparity the brain expects.
std::atomic<bool> g_scopeActive{ false };
bool  g_scopeEnabled = true;
int   g_scopeClip = 1100;        // clip_distance while scoped (320 = stock 53 deg)
float g_scopeDisplayDeg = 34.0f; // horizontal angle the scoped picture is shown across
float g_scopeRaiseM = 0.35f;     // controller within this of your eye (and lined up) -> scope up
float g_scopeLowerM = 0.45f;     // and it stays up until further than this
LARGE_INTEGER g_lastFrameQpc{};
bool  g_haveLastFrameQpc = false;
bool  g_lowFpsMonoActive = false;

// ===========================================================================
// THIRD-PERSON VR -- the "diorama" comfort mode (NEW 2026-08-16)
// ===========================================================================
// Medium-term goal from the handoff. If a player dislikes first person, they
// play MGS1 as a diorama: standing in the room, watching Snake, with head
// tracking giving a real 3D view of the scene instead of a flat frame on a
// screen.
//
// Almost all of it was already built and simply gated off. Everything below
// this point in the rotation and position hooks -- head rotation, positional
// 6DOF, alternate-eye stereo -- was behind `if (!fpv) return;`. This is the
// "(a) you are the security camera" variant from the handoff: MGS1 keeps its
// own scripted per-room camera POSITION, and we add head rotation and a 6DOF
// offset ON TOP of it, with stereo on. It is deliberately NOT the free
// over-the-shoulder variant, which needs Snake's world position -- an address
// nobody has confirmed yet (see the strafing section of the handoff; same
// hunt, same blocker).
//
// THREE THINGS THIS HAS TO GET RIGHT, all of them called out in the handoff:
//
// 1. write_player_heading MUST NOT run outside first person. In FPV that
//    global is the movement/aim frame and driving it from the head is the
//    whole point. In third person MGS1 uses it for Snake's own facing, so
//    feeding head yaw into it makes Snake spin on the spot as you look
//    around. Hard-disabled below, not left to the ini.
//
// 2. rotate2 is an INTERPOLATION TARGET, and outside FPV the game pulls
//    against writes to it much harder than it does in first person. This may
//    simply lose. `0x593FD0` ("Camera control") is the suspected switch that
//    hands camera authority over and would fix it, and is still an untested
//    CE hand-test -- so this mode ships with a rotation strength knob and an
//    honest expectation that it may fight back until that test is done.
//
// 3. MGS1 CUTS HARD BETWEEN ROOMS. Every cut teleports the viewpoint, which
//    in VR is genuinely unpleasant, and it is the main thing that could sink
//    this mode. Detected below by watching the game's own camera position for
//    a jump larger than third_person_cut_units in a single frame; on a cut we
//    drop the reference pose and suppress our own offsets for a few frames so
//    the view re-anchors to the new scripted camera cleanly instead of
//    dragging the previous room's offset across the boundary.
//
// Ships OFF. Nothing about the confirmed-working first-person path changes
// when third_person_vr=0: every gate below reduces to exactly its previous
// form, because `tpv` is false and `fpv || tpv` is `fpv`.
bool g_thirdPersonVr = false;
bool g_thirdPersonRotation = true;
bool g_thirdPersonPosition = true;
bool g_thirdPersonStereo = true;
bool g_thirdPersonFov = false;
int  g_thirdPersonRotationPercent = 100;
int  g_thirdPersonCutUnits = 400;
int  g_thirdPersonCutHoldFrames = 12;

// Live third-person state.
bool    g_tpvActive = false;          // read by IsThirdPersonVrActive()
int     g_tpvCutHoldCounter = 0;      // frames left suppressing our writes after a cut
bool    g_tpvHavePrevCamPos = false;
int32_t g_tpvPrevCamX = 0, g_tpvPrevCamY = 0, g_tpvPrevCamZ = 0;

// ===========================================================================
// CUTSCENE VR -- first person during cutscenes (NEW 2026-08-22)
// ===========================================================================
// The handoff's "Medium-term goal: cutscenes fully in first person", and the
// one item on it with a genuine structural unknown in the way.
//
// THE BLOCKER, STATED HONESTLY BEFORE ANY OF THIS RUNS. Section 3 of the
// handoff records that "cutscenes, codec calls and the pause menu all stop the
// rotation hook" -- that is the whole reason g_eyePublishTick and its expiry
// exist. Both of this mod's camera writes live INSIDE the game's own camera
// update. If that function does not run during a cutscene, then no amount of
// gating gets us a camera write, because the code we would gate never
// executes. Nobody has ever measured whether that is true for real cutscenes
// specifically, as opposed to codec calls and menus, which is what the eye-tag
// expiry was actually observed firing on.
//
// So this ships as TWO things at once, and the log says which one is carrying:
//
//   PATH A -- IN-FRAME. If the rotation hook does keep running through
//   cutscenes, this is the same machinery third-person VR already uses,
//   pointed at cutscene state instead of at "not in first person". Head
//   rotation added on top of the scripted cutscene camera, real 6DOF, real
//   stereo. Correct, cheap, and preferred whenever it is available.
//
//   PATH B -- FLIP-TIME FALLBACK. If the hook IS stalled, we still have one
//   thing that keeps running on the game's own thread every single frame:
//   IDirectDrawSurface7::Flip. The 2026-08-22 depth probe proved that
//   conclusively -- HookedPresent never fires all session while Flip fires
//   continuously. Writing rotate2 from there is NOT the cross-thread write
//   this project banned. It is the same thread, once per frame; only the point
//   in the frame differs. The write lands after the frame that was just
//   presented and is picked up by the next one.
//
// PATH B IS ABSOLUTE, NOT ADDITIVE, AND THAT IS NOT A STYLE CHOICE. The
// in-frame path reads the angle the game just wrote and adds the head delta on
// top, which is safe because the game recomputes that value from the camera
// actor every frame. During a stall the game is NOT recomputing it -- so
// "read live, add delta" would read back OUR OWN last write and compound it
// every frame until the view spins. Path B therefore pins an anchor when it
// engages and writes anchor+delta absolutely. When the game moves the cutscene
// camera itself (a cut to a new shot, a scripted pan) the live value diverges
// from what we last wrote by more than cutscene_reanchor_units, and we
// re-anchor to it -- which is also what makes shot changes work rather than
// fighting us.
//
// PATH B DOES NOT DO STEREO. The alternate-eye tag has to be advanced in the
// same frame the camera offset is applied and before the pixels are captured;
// at flip time that ordering is not available and a wrong tag routes whole
// scenes into one eye, which is a bug this project has already shipped once.
// Mono during a fallback-driven cutscene is the honest answer.
//
// DETECTION. Three candidate signals, none of them previously tested against a
// real cutscene, so all three are read and edge-logged every session whether
// or not they are used:
//
//   0x32279F  "Cutscene control", byte, from the community table -- its author
//             bound hotkeys writing 0x00 and 0x80, so 0x80 is presumed to be
//             one flag in a bitfield.
//   0x593FD0  "Camera control", 2 bytes, GM_SnakeCameraWork + 0x30, the
//             table's hotkey sets it to 1. Long suspected to be the switch
//             that hands camera authority over, which would serve cutscene
//             FPV, third-person VR and retiring the NOP patches all at once.
//   0x391A0C  the PAUSE byte, CONFIRMED 2026-08-22 to be a MODE ENUM --
//             0 gameplay, 1 pause menu, 4 inventory. Its cutscene value, if it
//             has one, has never been observed. Used here to EXCLUDE modal UI
//             from cutscene detection regardless of which mode is selected.
//
// This is the CE hand-test the handoff has been asking for since 2026-08-15,
// done automatically from a normal play session instead of by hand.
constexpr uintptr_t kCutsceneControlRva = 0x32279F;   // byte
constexpr uintptr_t kCameraControlRva = 0x593FD0;     // 2 bytes
constexpr uintptr_t kModalStateRva = 0x391A0C;        // byte, mode enum

// 0 = off, 1 = auto (control byte OR hook stall), 2 = control byte only,
// 3 = hook stall only, 4 = force (anything not FPV and not modal UI).
int  g_cutsceneDetectMode = 0;
int  g_cutsceneControlMask = 0x80;
bool g_cutsceneRotation = true;
int  g_cutsceneRotationPercent = 100;
bool g_cutscenePosition = true;
bool g_cutsceneStereo = true;
bool g_cutsceneFov = false;
int  g_cutsceneCutUnits = 400;
int  g_cutsceneCutHoldFrames = 8;
int  g_cutsceneStallFlips = 6;
bool g_cutsceneFlipFallback = true;
bool g_cutsceneFallbackPosition = false;
int  g_cutsceneReanchorUnits = 32;
int  g_cutsceneCameraControlWrite = -1;   // -1 = observe only
int  g_cutsceneControlWrite = -1;         // -1 = observe only

// Liveness counter. Bumped once per rotation-hook body. The flip tick watches
// it: if it has not moved in cutscene_stall_flips presented frames, the game's
// camera update has stopped and path A is unavailable. This is the measurement
// the whole feature is gated on, and it costs one increment per frame.
std::atomic<unsigned long long> g_rotHookRuns{ 0 };
std::atomic<unsigned long long> g_rotHookTick{ 0 };   // GetTickCount64 of the last rotation-hook run

// ---- FRAME VIEW RECORD state (2026-09-22) ---------------------------------
// See FrameViewRecord in camera_write_hook.h for the why. Two small locked
// structs, touched a handful of times per frame; SRW locks rather than
// atomics because each is a multi-field snapshot that must never be torn.
struct HeadViewSnap {
    bool      have = false;
    float     yaw = 0, pitch = 0, roll = 0;
    ViewPoseF eye[2];
};
SRWLOCK         g_headViewLock = SRWLOCK_INIT;
HeadViewSnap    g_headViewSnap;          // XR thread writes, rotation hook reads

SRWLOCK         g_frameRecLock = SRWLOCK_INIT;
FrameViewRecord g_frameRec;              // written by the rotation hook, this frame
unsigned long long g_frameRecRun = 0;    // g_rotHookRuns value it belongs to
FrameViewRecord g_lastValidRec;          // newest valid one, for motion aim
bool            g_frameRecordEnabled = true;   // [camera_hook] frame_view_record

std::atomic<bool> g_csvDetected{ false };      // written by the flip tick
std::atomic<bool> g_csvActive{ false };        // path A is driving right now
std::atomic<bool> g_csvFallbackDriving{ false };  // path B is driving right now

// Published by the flip tick purely so FirstPersonRequestIsPlausible() can ask
// "is the game in a state where it could possibly honour an FPV request?"
// without duplicating the tick's debounce and stall measurement. Both are
// covered by the same staleness gate as the flags above.
std::atomic<bool> g_csvHookStalled{ false };
std::atomic<int>  g_csvModalState{ -1 };       // 0 gameplay, 1 pause, 4 inventory, -1 unread

// WHEN the flip tick last ran. Same shape, and the same reason, as
// g_eyePublishTick: every one of the three flags above is written ONLY by the
// tick, and the tick lives inside `if (SUCCEEDED(hr))` in the Flip hook. A
// lost DirectDraw surface -- an alt-tab, a display mode change -- makes Flip
// start failing while the process, the OpenXR loop and everything else keeps
// running, and all three flags then latch at whatever they last were. Latched
// mid-cutscene that means the 2D-UI framing stays suppressed and the FPV
// auto-restore stays deferred indefinitely. A stale answer is worth exactly
// nothing here, so past this window callers are told "no".
std::atomic<unsigned long long> g_csvTickStamp{ 0 };
constexpr unsigned long long kCsvTickStaleMs = 1000;

// Path A live state (game thread, inside the hooks).
bool g_csvInFrame = false;

// Path B live state (game thread, inside Flip).
bool    g_csvFbHaveAnchor = false;
int32_t g_csvFbAnchorYaw = 0, g_csvFbAnchorPitch = 0;
int32_t g_csvFbAnchorPosX = 0, g_csvFbAnchorPosY = 0, g_csvFbAnchorPosZ = 0;
float   g_csvFbRefHeadYaw = 0.0f, g_csvFbRefHeadPitch = 0.0f;
float   g_csvFbRefHeadPosX = 0.0f, g_csvFbRefHeadPosY = 0.0f, g_csvFbRefHeadPosZ = 0.0f;
int32_t g_csvFbLastWroteYaw = 0, g_csvFbLastWrotePitch = 0;
bool    g_csvFbHaveLastWrote = false;
// Position-side fallback state. File scope rather than function statics
// specifically so CutsceneFallbackReset() can actually clear it -- as function
// statics these survived between cutscenes, and the anchor carried the last
// frame's head-lean offset into the next scene as a permanent bias.
bool    g_csvFbHaveLastPos = false;
int32_t g_csvFbLastWroteX = 0, g_csvFbLastWroteY = 0, g_csvFbLastWroteZ = 0;

// Camera-authority experiment: what the two bytes read before we first wrote
// them, so they can be put back exactly once when the cutscene ends.
bool g_csvSavedCameraControl = false;
int16_t g_csvOriginalCameraControl = 0;
bool g_csvSavedCutsceneControl = false;
uint8_t g_csvOriginalCutsceneControl = 0;

// ---------------------------------------------------------------------------
// VIEW MODE (see the long note in camera_write_hook.h)
// ---------------------------------------------------------------------------
// The mod's own authoritative state. NOT the game's FPV flag, and never
// inferred from it. Read on the game thread every frame, written from the
// input thread on an R3 double-press.
std::atomic<bool> g_vrViewMode{ true };
std::atomic<bool> g_virtualScreenRecenter{ false };

// Has first person ever been ESTABLISHED this session -- either by the player
// pressing R3, or by the game putting us there on its own?
//
// Without this the maintenance loop starts firing synthetic X keypresses the
// moment the process is up, because VR mode is on by default and the game is
// obviously not in first person during the title sequence. The modal byte is
// no defence: the 2026-08-23 log reads `modal(391A0C)=0 (gameplay)` at flip #1,
// long before the player is controlling anything, so "modal says gameplay"
// cannot distinguish the intro from the game.
//
// Arming on first establishment reproduces exactly the old contract -- press
// R3 once and the mod keeps you there -- while removing the part that was
// broken, which was giving up.
std::atomic<bool> g_fpvMaintenanceArmed{ false };

// Startup default, from the ini. Shipping VR-on preserves the behaviour every
// previous session had; native-on-start is there for anyone who wants the game
// to look untouched until they ask for VR.
bool g_vrViewModeDefault = true;

// --- handing the field of view back -----------------------------------------
// fov_clip_distance is written every frame while VR mode drives, and the game
// does NOT rewrite it every frame -- it sets it per camera, on transitions.
// (The session log catches exactly that: "FOV observed: reads 320" on entering
// first person, then 240 for every frame after our first write.) So simply
// ceasing to write on entering native mode would leave OUR wide lens in place
// until the next room change, and "the game exactly as shipped" would ship with
// a 67-degree lens where the game asked for 53.
//
// The stock value is therefore captured opportunistically: any frame where the
// field reads something other than what we write is a frame the game set it,
// and that value is worth keeping. Restored exactly once, on the game thread,
// the first frame after the mode goes native.
//
// It can be one room out of date if the player switches modes without the game
// having touched the field since the last transition. That is both rare and
// self-correcting -- the game reassigns it at the very next camera change --
// and a slightly stale stock value is a far smaller deviation than a lens the
// game never chose at all.
int16_t g_fovOriginal = 0;
bool    g_haveFovOriginal = false;
std::atomic<bool> g_fovRestorePending{ false };

// ---------------------------------------------------------------------------
// CUTSCENE PROBE (2026-08-23) -- is rotate2 even read during a cutscene?
// ---------------------------------------------------------------------------
// The 2026-08-23 session settled one half of the cutscene question and opened
// the other. PATH B's writes LAND: every CsvFb line in mgs1_vr_test.log shows
// the value we wrote coming back as the next frame's "live was", to the unit,
// for a sixty-second scene. Nothing overwrites them. And yet the picture did
// not move at all -- the player's report was "the game on a screen like
// normal", for every cutscene in the session.
//
// Writes landing plus a still picture leaves exactly one conclusion available:
// during a cutscene, NOTHING READS rotate2. The most likely mechanism is that
// the routine our hooks live inside (0xE1AF8) is not only the writer of
// rotate2 but also the thing that turns it into the view transform the
// rasteriser uses. Stop the routine and rotate2 becomes a dead input -- still
// writable, still readable, connected to nothing. That is consistent with PATH
// A working (routine runs, transform rebuilt, our write included) and PATH B
// not (routine stopped, transform frozen at the shot's value).
//
// It is a conclusion by elimination, though, and this project's own working
// practices say to measure rather than reason when the measurement is cheap.
// So:
//
//   THE PULSE. While PATH B is driving, add a large offset to the yaw we
//   write, and turn that offset on and off on a fixed period. If rotate2 is
//   live, the cutscene camera swings a quarter turn and back, once a second,
//   unmissably. If it is inert, nothing happens and the scene plays normally.
//   Either way the answer arrives in the first cutscene of the session and
//   needs no interpretation of a log.
//
//   Deliberately large and deliberately periodic. A small offset is
//   indistinguishable from head tracking; a constant one is indistinguishable
//   from a wrong anchor. A metronome is neither.
//
//   THE SCAN. If rotate2 is inert, the next question is what the cutscene
//   renderer reads INSTEAD -- quite possibly a separate camera struct, since
//   MGS1 has more than one. So while a cutscene is running, sample a window of
//   memory around the known camera block and count which 16-bit words change.
//   A word that moves when the cutscene camera moves, and is not one of the
//   six we already know, is the next thing to try writing.
int  g_csvProbePulseUnits = 1024;    // 1024 = quarter turn. 0 disables.
int  g_csvProbePulseMs = 1000;
bool g_csvProbeScan = true;
uintptr_t g_csvProbeScanBase = 0x593E00;
int  g_csvProbeScanBytes = 1024;     // 512 x 16-bit words
int  g_csvProbeScanEvery = 4;        // sample every Nth flip while in a cutscene

// Pulse state (game thread, inside Flip).
bool g_csvProbePulseOn = false;
unsigned long long g_csvProbePulseEdgeMs = 0;

// Scan state (game thread, inside Flip). Fixed capacity so there is no
// allocation on the game thread, ever.
constexpr int kCsvScanMaxWords = 512;
uint16_t g_csvScanPrev[kCsvScanMaxWords] = { 0 };
uint32_t g_csvScanHits[kCsvScanMaxWords] = { 0 };
bool     g_csvScanHavePrev = false;
int      g_csvScanWords = 0;
unsigned long long g_csvScanSamples = 0;

std::string GetGameIniPath() {
    char exePath[MAX_PATH] = { 0 };
    GetModuleFileNameA(NULL, exePath, MAX_PATH);
    std::string dir(exePath);
    auto slash = dir.find_last_of("\\/");
    if (slash != std::string::npos) dir = dir.substr(0, slash);
    return dir + "\\mgs1_vr_config.ini";
}

// See the matching comment in vr_input.cpp's copy of this helper for why it
// exists: a 2026-08-15 report of edited ini values not taking effect even on
// a fresh (non-mid-session) launch. Logs which file was actually opened and
// when it was last modified, so a mismatch against "when I saved my edit" is
// immediately visible in the log instead of guessed at.
static void LogIniFileState(const std::string& path) {
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &fad)) {
        DebugLogger::LogFormat("Ini file NOT FOUND at %s -- every setting loaded below is a hardcoded "
            "default, not read from your edits", path.c_str());
        return;
    }
    SYSTEMTIME utc{}, local{};
    FileTimeToSystemTime(&fad.ftLastWriteTime, &utc);
    SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local);
    DebugLogger::LogFormat("Ini file actually opened: %s (last modified %04d-%02d-%02d %02d:%02d:%02d "
        "local time -- if that is NOT when you last saved your edit, this process is reading a different "
        "copy of the file than the one you're editing)",
        path.c_str(), local.wYear, local.wMonth, local.wDay, local.wHour, local.wMinute, local.wSecond);
}

void LoadCameraHookConfig() {
    const std::string ini = GetGameIniPath();
    LogIniFileState(ini);
    auto I = [&](const char* k, int d) { return GetPrivateProfileIntA("camera_hook", k, d, ini.c_str()); };

    g_writeYaw = I("write_yaw", 1) != 0;
    g_writePitch = I("write_pitch", 1) != 0;
    g_writeRoll = I("write_roll", 0) != 0;
    g_invertHeadYaw = I("invert_head_yaw", 1) != 0;
    g_invertHeadPitch = I("invert_head_pitch", 1) != 0;
    g_invertHeadRoll = I("invert_head_roll", 0) != 0;
    g_swapYawPitch = I("swap_yaw_pitch", 1) != 0;
    g_additiveRotation = I("additive_rotation", 1) != 0;
    g_bodyFollowsHead = I("body_follows_head", 1);
    if (g_bodyFollowsHead < 0 || g_bodyFollowsHead > 2) g_bodyFollowsHead = 1;
    DebugLogger::LogFormat("Body follows head: %d (%s)", g_bodyFollowsHead,
        g_bodyFollowsHead == 0 ? "off -- Snake turns only with the stick" :
        g_bodyFollowsHead == 1 ? "while moving -- push the stick and Snake walks where you look" :
                                 "always -- Snake's facing tracks your head at all times");
    g_fpvDebounceFrames = I("fpv_debounce_frames", 6);
    if (g_fpvDebounceFrames < 1) g_fpvDebounceFrames = 1;
    g_logFrames = I("log_frames", 240);

    g_positionEnabled = I("position_tracking", 0) != 0;
    g_positionScale = (float)I("position_scale", 1000);
    g_maxPositionOffset = I("max_position_offset", 400);
    g_invertPosX = I("invert_position_x", 0) != 0;
    g_invertPosY = I("invert_position_y", 1) != 0;
    g_invertPosZ = I("invert_position_z", 0) != 0;
    g_rotatePositionWithView = I("rotate_position_with_view", 1) != 0;
    g_positionYawOffsetRad = (float)I("position_yaw_offset_deg", 0) * (kPi / 180.0f);
    g_positionHandednessFlip = I("position_handedness_flip", 0) != 0;
    g_positionFrame = I("position_frame", 2);
    DebugLogger::LogFormat(
        "Position frame: %s",
        g_positionFrame == 2
            ? "2 -- from the game camera basis (right = (-cos yaw, 0, sin yaw), forward = (sin yaw, 0, cos yaw), "
              "world Y up), as read out of DG_LookAt. invert_position_*, position_handedness_flip and "
              "position_yaw_offset_deg are IGNORED in this mode."
            : "0 -- legacy flags (invert_position_*, handedness_flip, yaw_offset)");
    g_positionTestUnits = I("position_test_oscillate", 0);
    g_stereoAlternate = I("stereo_alternate_eyes", 0) != 0;
    g_stereoIpdMm = I("stereo_ipd_mm", 64);
    if (g_stereoIpdMm < 0)   g_stereoIpdMm = 0;
    if (g_stereoIpdMm > 120) g_stereoIpdMm = 120;
    g_stereoSwapEyes = I("stereo_swap_eyes", 0) != 0;
    g_stereoMotionDegPerFrame = I("stereo_motion_hold_deg", 2);
    if (g_stereoMotionDegPerFrame < 0) g_stereoMotionDegPerFrame = 0;
    g_stereoHoldFrames = I("stereo_hold_frames", 10);
    if (g_stereoHoldFrames < 0) g_stereoHoldFrames = 0;
    g_stereoMotionUnitsPerFrame = I("stereo_motion_hold_units", 8);
    if (g_stereoMotionUnitsPerFrame < 0) g_stereoMotionUnitsPerFrame = 0;
    g_stereoEyeTimeoutMs = I("stereo_eye_timeout_ms", 120);
    if (g_stereoEyeTimeoutMs < 33) g_stereoEyeTimeoutMs = 33;
    g_frameRecordEnabled = I("frame_view_record", 1) != 0;
    DebugLogger::LogFormat("Frame view record: %s",
        g_frameRecordEnabled
            ? "ON -- each captured frame carries the head pose it was drawn from, so the "
              "compositor can hold the world still between game frames"
            : "OFF -- legacy: frames are labelled with whatever the headset pose is when copied");
    g_stereoMinFps = I("stereo_min_fps", 24);
    if (g_stereoMinFps < 0) g_stereoMinFps = 0;
    g_writePlayerHeading = I("write_player_heading", 1) != 0;
    g_playerHeadingOffsetUnits = I("player_heading_offset_units", 2048);

    g_fovClipDistance = I("fov_clip_distance", 0);
    if (g_fovClipDistance != 0) {
        // Below ~90 the frustum gets absurd and the 1998 renderer starts
        // showing you rooms it never finished building. Above 320 is narrower
        // than stock, which nobody wants but is harmless.
        if (g_fovClipDistance < 90)   g_fovClipDistance = 90;
        if (g_fovClipDistance > 1000) g_fovClipDistance = 1000;
    }
    g_fovFirstPersonOnly = I("fov_first_person_only", 1) != 0;
    {
        const double hfov = 2.0 * std::atan(160.0 / (double)(g_fovClipDistance ? g_fovClipDistance : 320))
                          * 180.0 / 3.14159265358979323846;
        DebugLogger::LogFormat(
            "FOV: clip_distance=%d (%s) -> hfov %.1f deg | first_person_only=%d | field at mgsi.exe+%X",
            g_fovClipDistance,
            g_fovClipDistance ? "WRITING every frame" : "observe only, not writing",
            hfov, g_fovFirstPersonOnly ? 1 : 0, (unsigned)kFovClipDistanceRva);
    }
    DebugLogger::LogFormat(
        "Player heading: write=%d offset=%d units (target mgsi.exe+%X, found by static analysis of the FPV routine)",
        g_writePlayerHeading ? 1 : 0, g_playerHeadingOffsetUnits, (unsigned)kPlayerHeadingRva);

    // --- third-person VR ("diorama") ---------------------------------------
    g_thirdPersonVr = I("third_person_vr", 0) != 0;
    g_thirdPersonRotation = I("third_person_rotation", 1) != 0;
    g_thirdPersonPosition = I("third_person_position", 1) != 0;
    g_thirdPersonStereo = I("third_person_stereo", 1) != 0;
    g_thirdPersonFov = I("third_person_fov", 0) != 0;
    g_thirdPersonRotationPercent = I("third_person_rotation_percent", 100);
    if (g_thirdPersonRotationPercent < 0)   g_thirdPersonRotationPercent = 0;
    if (g_thirdPersonRotationPercent > 100) g_thirdPersonRotationPercent = 100;
    g_thirdPersonCutUnits = I("third_person_cut_units", 400);
    if (g_thirdPersonCutUnits < 0) g_thirdPersonCutUnits = 0;
    g_thirdPersonCutHoldFrames = I("third_person_cut_hold_frames", 12);
    if (g_thirdPersonCutHoldFrames < 0)  g_thirdPersonCutHoldFrames = 0;
    if (g_thirdPersonCutHoldFrames > 120) g_thirdPersonCutHoldFrames = 120;
    if (g_thirdPersonVr) {
        DebugLogger::LogFormat(
            "Third-person VR (diorama): ON -- rotation=%d (%d%%) position=%d stereo=%d fov=%d | "
            "room-cut guard: jumps > %d units suppress our writes for %d frames and re-anchor",
            g_thirdPersonRotation ? 1 : 0, g_thirdPersonRotationPercent,
            g_thirdPersonPosition ? 1 : 0, g_thirdPersonStereo ? 1 : 0,
            g_thirdPersonFov ? 1 : 0, g_thirdPersonCutUnits, g_thirdPersonCutHoldFrames);
        DebugLogger::Log(
            "Third-person VR: write_player_heading is force-disabled outside first person regardless "
            "of the ini -- driving that global from head yaw makes Snake spin on the spot. Also note "
            "rotate2 is an interpolation TARGET here and the game pulls against writes to it harder "
            "than it does in FPV; if the view feels rubber-banded, that is the known cause and the "
            "0x593FD0 'Camera control' hand-test is the thing that would fix it, not a tuning knob.");
    }

    // --- cutscene VR --------------------------------------------------------
    g_cutsceneDetectMode = I("cutscene_detect_mode", 0);
    if (g_cutsceneDetectMode < 0) g_cutsceneDetectMode = 0;
    if (g_cutsceneDetectMode > 4) g_cutsceneDetectMode = 4;
    g_cutsceneControlMask = I("cutscene_control_mask", 0x80);
    if (g_cutsceneControlMask < 0)   g_cutsceneControlMask = 0;
    if (g_cutsceneControlMask > 255) g_cutsceneControlMask = 255;
    g_cutsceneRotation = I("cutscene_rotation", 1) != 0;
    g_cutsceneRotationPercent = I("cutscene_rotation_percent", 100);
    if (g_cutsceneRotationPercent < 0)   g_cutsceneRotationPercent = 0;
    if (g_cutsceneRotationPercent > 100) g_cutsceneRotationPercent = 100;
    g_cutscenePosition = I("cutscene_position", 1) != 0;
    g_cutsceneStereo = I("cutscene_stereo", 1) != 0;
    g_cutsceneFov = I("cutscene_fov", 0) != 0;
    g_cutsceneStallFlips = I("cutscene_stall_flips", 6);
    if (g_cutsceneStallFlips < 2)   g_cutsceneStallFlips = 2;
    if (g_cutsceneStallFlips > 240) g_cutsceneStallFlips = 240;
    g_cutsceneFlipFallback = I("cutscene_flip_fallback", 1) != 0;
    g_cutsceneFallbackPosition = I("cutscene_flip_fallback_position", 0) != 0;
    g_cutsceneCutUnits = I("cutscene_cut_units", 400);
    if (g_cutsceneCutUnits < 0) g_cutsceneCutUnits = 0;
    g_cutsceneCutHoldFrames = I("cutscene_cut_hold_frames", 8);
    if (g_cutsceneCutHoldFrames < 0)   g_cutsceneCutHoldFrames = 0;
    if (g_cutsceneCutHoldFrames > 120) g_cutsceneCutHoldFrames = 120;
    // RAISED 32 -> 200 on 2026-08-23. 32 units is 2.8 degrees, which is inside
    // the noise of a scripted pan: the session log has a re-pin fire on a
    // 35-unit move, and re-pinning throws away the player's head offset. Real
    // shot changes in the same log measured 74, 983, 1042 and 2112 units, so
    // 200 sits clear of the noise and well under every genuine cut.
    g_cutsceneReanchorUnits = I("cutscene_reanchor_units", 200);
    if (g_cutsceneReanchorUnits < 1)    g_cutsceneReanchorUnits = 1;
    if (g_cutsceneReanchorUnits > 2048) g_cutsceneReanchorUnits = 2048;
    g_cutsceneCameraControlWrite = I("cutscene_camera_control", -1);
    g_cutsceneControlWrite = I("cutscene_control_write", -1);

    // --- view mode ----------------------------------------------------------
    g_vrViewModeDefault = I("start_in_vr_mode", 1) != 0;
    g_vrViewMode.store(g_vrViewModeDefault, std::memory_order_relaxed);
    DebugLogger::LogFormat(
        "View mode: starting in %s. R3 double-press switches. In NATIVE the mod writes NOTHING to the "
        "game -- no camera, no FOV, no player heading, no stereo -- and the picture is shown on a "
        "world-locked virtual screen. In VR the mod drives the camera as it always has.",
        g_vrViewModeDefault ? "VR" : "NATIVE");

    // --- cutscene probe (2026-08-23) ----------------------------------------
    g_csvProbePulseUnits = I("cutscene_probe_pulse_units", 1024);
    if (g_csvProbePulseUnits < 0)    g_csvProbePulseUnits = 0;
    if (g_csvProbePulseUnits > 2048) g_csvProbePulseUnits = 2048;
    g_csvProbePulseMs = I("cutscene_probe_pulse_ms", 1000);
    if (g_csvProbePulseMs < 200)   g_csvProbePulseMs = 200;
    if (g_csvProbePulseMs > 10000) g_csvProbePulseMs = 10000;
    g_csvProbeScan = I("cutscene_probe_scan", 1) != 0;
    g_csvProbeScanBase = (uintptr_t)(unsigned)I("cutscene_probe_scan_base", 0x593E00);
    g_csvProbeScanBytes = I("cutscene_probe_scan_bytes", 1024);
    if (g_csvProbeScanBytes < 32) g_csvProbeScanBytes = 32;
    if (g_csvProbeScanBytes > kCsvScanMaxWords * 2) g_csvProbeScanBytes = kCsvScanMaxWords * 2;
    g_csvScanWords = g_csvProbeScanBytes / 2;
    g_csvProbeScanEvery = I("cutscene_probe_scan_every", 4);
    if (g_csvProbeScanEvery < 1)   g_csvProbeScanEvery = 1;
    if (g_csvProbeScanEvery > 120) g_csvProbeScanEvery = 120;

    if (g_csvProbePulseUnits > 0) {
        DebugLogger::LogFormat(
            "Cutscene probe: PULSE ON -- while the flip-time fallback is driving, %d units (%.0f deg) "
            "of extra yaw are switched on and off every %d ms. THIS IS A TEST AND IT IS MEANT TO BE "
            "OBVIOUS: if the cutscene camera swings back and forth, rotate2 is still read during "
            "cutscenes and the fallback can work. If the scene plays perfectly normally, rotate2 is "
            "inert while the camera update is stopped -- which is the answer we expect, and it means "
            "the cutscene renderer reads something else. Set cutscene_probe_pulse_units=0 to stop it.",
            g_csvProbePulseUnits, (double)g_csvProbePulseUnits * 360.0 / 4096.0, g_csvProbePulseMs);
    }
    else {
        DebugLogger::Log("Cutscene probe: pulse OFF (cutscene_probe_pulse_units=0)");
    }
    DebugLogger::LogFormat(
        "Cutscene probe: scan=%d over mgsi.exe+%X..%X (%d 16-bit words, every %d flips). Looks for the "
        "camera the cutscene renderer actually reads, by counting which words move while a scene plays.",
        g_csvProbeScan ? 1 : 0, (unsigned)g_csvProbeScanBase,
        (unsigned)(g_csvProbeScanBase + (uintptr_t)g_csvProbeScanBytes),
        g_csvScanWords, g_csvProbeScanEvery);

    {
        static const char* kModeNames[5] = {
            "OFF (observe only -- the two candidate bytes are still read and logged)",
            "AUTO (cutscene control byte OR a stalled camera update)",
            "CONTROL BYTE only (mgsi.exe+32279F & mask)",
            "HOOK STALL only (camera update stopped while frames keep flipping)",
            "FORCE (anything that is not first person and not modal UI)"
        };
        DebugLogger::LogFormat(
            "Cutscene VR: detect_mode=%d -- %s | control_mask=0x%02X | stall after %d flips",
            g_cutsceneDetectMode, kModeNames[g_cutsceneDetectMode],
            (unsigned)g_cutsceneControlMask, g_cutsceneStallFlips);
        DebugLogger::LogFormat(
            "Cutscene VR: rotation=%d (%d%%) position=%d stereo=%d fov=%d | flip fallback=%d "
            "(position through fallback=%d, re-anchor at %d units)",
            g_cutsceneRotation ? 1 : 0, g_cutsceneRotationPercent,
            g_cutscenePosition ? 1 : 0, g_cutsceneStereo ? 1 : 0, g_cutsceneFov ? 1 : 0,
            g_cutsceneFlipFallback ? 1 : 0, g_cutsceneFallbackPosition ? 1 : 0,
            g_cutsceneReanchorUnits);
        DebugLogger::LogFormat(
            "Cutscene VR: camera authority -- mgsi.exe+593FD0 (\"Camera control\") %s, "
            "mgsi.exe+32279F (\"Cutscene control\") %s. Both are READ and edge-logged every session "
            "regardless; this is the Cheat Engine hand-test the handoff has been asking for, run "
            "automatically.",
            (g_cutsceneCameraControlWrite < 0) ? "observe only"
                : "WILL BE WRITTEN while a cutscene is active",
            (g_cutsceneControlWrite < 0) ? "observe only"
                : "WILL BE WRITTEN while a cutscene is active");
        if (g_cutsceneCameraControlWrite >= 0) {
            DebugLogger::LogFormat("Cutscene VR: will write %d to mgsi.exe+593FD0 during cutscenes "
                "and restore the pre-cutscene value on exit", g_cutsceneCameraControlWrite);
        }
        if (g_cutsceneControlWrite >= 0) {
            DebugLogger::LogFormat("Cutscene VR: will write %d to mgsi.exe+32279F during cutscenes "
                "and restore the pre-cutscene value on exit", g_cutsceneControlWrite);
        }
    }

    DebugLogger::LogFormat(
        "Camera hook config: write y=%d p=%d r=%d | invert head y=%d p=%d r=%d | swap=%d log=%d",
        g_writeYaw ? 1 : 0, g_writePitch ? 1 : 0, g_writeRoll ? 1 : 0,
        g_invertHeadYaw ? 1 : 0, g_invertHeadPitch ? 1 : 0, g_invertHeadRoll ? 1 : 0,
        g_swapYawPitch ? 1 : 0, g_logFrames);
    DebugLogger::LogFormat("Camera hook: additive_rotation=%d fpv_debounce_frames=%d",
        g_additiveRotation ? 1 : 0, g_fpvDebounceFrames);
    if (g_positionTestUnits != 0) {
        DebugLogger::LogFormat(
            "Position tracking: DIAGNOSTIC OSCILLATION ACTIVE at +/-%d units -- head position ignored",
            g_positionTestUnits);
    }
    DebugLogger::LogFormat(
        "Stereo: alternate_eyes=%d ipd=%dmm swap_eyes=%d (offset per eye = %.1f units at scale %.0f)",
        g_stereoAlternate ? 1 : 0, g_stereoIpdMm, g_stereoSwapEyes ? 1 : 0,
        (g_stereoIpdMm / 2000.0f) * g_positionScale, g_positionScale);
    DebugLogger::LogFormat(
        "Stereo motion hold: %d deg/frame (rotation, head-led + game yaw/pitch) | %d units/frame "
        "(translation) | hold %d frames | floor %d fps",
        g_stereoMotionDegPerFrame, g_stereoMotionUnitsPerFrame, g_stereoHoldFrames, g_stereoMinFps);
    DebugLogger::LogFormat(
        "Position tracking: enabled=%d scale=%.0f units/m clamp=%d invert x=%d y=%d z=%d rotate=%d yaw_off=%.0fdeg",
        g_positionEnabled ? 1 : 0, g_positionScale, g_maxPositionOffset,
        g_invertPosX ? 1 : 0, g_invertPosY ? 1 : 0, g_invertPosZ ? 1 : 0,
        g_rotatePositionWithView ? 1 : 0, g_positionYawOffsetRad * 180.0f / kPi);
    DebugLogger::LogFormat("Position frame: handedness_flip=%d (0 = pure rotation, 1 = mirrored Z + reversed yaw)",
        g_positionHandednessFlip ? 1 : 0);

    // --- view commit (layer 3) ----------------------------------------------
    g_vcMode = I("view_commit_mode", 1);
    if (g_vcMode < 0) g_vcMode = 0;
    if (g_vcMode > 2) g_vcMode = 2;
    g_vcRotation = I("view_commit_rotation", 1) != 0;
    g_vcPosition = I("view_commit_position", 1) != 0;
    g_vcStereo = I("view_commit_stereo", 1) != 0;
    g_vcFov = I("view_commit_fov", 0) != 0;
    g_vcRotationPercent = I("view_commit_rotation_percent", 100);
    if (g_vcRotationPercent < 0)   g_vcRotationPercent = 0;
    if (g_vcRotationPercent > 200) g_vcRotationPercent = 200;
    g_vcMinDist = I("view_commit_min_dist", 256);
    if (g_vcMinDist < 16) g_vcMinDist = 16;
    g_vcDefaultDist = I("view_commit_default_dist", 1000);
    if (g_vcDefaultDist < g_vcMinDist) g_vcDefaultDist = g_vcMinDist;
    g_vcLogFrames = I("view_commit_log_frames", 240);
    DebugLogger::LogFormat(
        "View commit (layer 3, +53C47): mode=%d (%s) rotation=%d(%d%%) position=%d stereo=%d fov=%d "
        "min_dist=%d default_dist=%d",
        g_vcMode,
        g_vcMode == 0 ? "observe only" : (g_vcMode == 1 ? "cutscenes only" : "always"),
        g_vcRotation ? 1 : 0, g_vcRotationPercent, g_vcPosition ? 1 : 0,
        g_vcStereo ? 1 : 0, g_vcFov ? 1 : 0, g_vcMinDist, g_vcDefaultDist);

    g_vbMode = I("view_build_mode", 1);
    if (g_vbMode < 0) g_vbMode = 0;
    if (g_vbMode > 2) g_vbMode = 2;
    g_vbCallerRva = (unsigned)I("view_build_caller_rva", 0);
    g_vbLogCallers = I("view_build_log_callers", 24);
    g_vbSnakeProbe = I("view_build_snake_probe", 1) != 0;
    g_vbSnakeEvery = I("view_build_snake_probe_every", 30);
    if (g_vbSnakeEvery < 1) g_vbSnakeEvery = 1;
    g_vbSnakeLines = I("view_build_snake_probe_lines", 240);
    g_dcMode = I("demo_camera_mode", 1);
    if (g_dcMode < 0) g_dcMode = 0;
    if (g_dcMode > 2) g_dcMode = 2;
    g_dcLogLines = I("demo_camera_log_lines", 60);
    DebugLogger::LogFormat(
        "View build (chokepoint, +1C22): mode=%d (%s) caller_filter=%s. This is the function every "
        "camera path in the game calls to turn a from/to pair into a view matrix -- 26 call sites, "
        "of which +53C47 is only the gameplay one. It runs when the camera task does not.",
        g_vbMode,
        g_vbMode == 0 ? "observe only" : (g_vbMode == 1 ? "cutscenes only" : "always"),
        g_vbCallerRva ? "set" : "any caller");
    DebugLogger::LogFormat(
        "Snake probe: %d (every %d builds, %d lines max). Writes Snake?[] lines to "
        "mgs1_vr_test.log -- NOT the debug log -- comparing mgsi.exe+593FA0 against the from/to "
        "pair each camera path hands the renderer. Answers whether 593FA0 keeps tracking Snake "
        "while a scripted cutscene camera owns the framing.",
        g_vbSnakeProbe ? 1 : 0, g_vbSnakeEvery, g_vbSnakeLines);
    DebugLogger::LogFormat(
        "Cutscene camera (+1CD292): mode=%d (%s). THIS is the one that runs during a cutscene. The "
        "game sets bit 7 of 32279F (only instruction that does so is +1CD0A2), which switches its "
        "own camera task off, then this function writes layer 3 from script data at +1CD4C4 and "
        "derives the view angles inline -- never calling +1C22. We rewrite its SOURCE struct, "
        "because the angle maths re-reads the struct rather than the globals.",
        g_dcMode,
        g_dcMode == 0 ? "observe only" : (g_dcMode == 1 ? "cutscenes only" : "always"));

    g_povMode = I("cutscene_snake_pov", 1) != 0 ? 1 : 0;

    g_csView = I("cutscene_view", 1) != 0 ? 1 : 0;
    g_csPovNoSnake = I("cutscene_pov_no_snake", 2);
    if (g_csPovNoSnake < 0 || g_csPovNoSnake > 2) g_csPovNoSnake = 2;
    g_csPovHoldRadius = I("cutscene_pov_hold_radius_units", 9000);
    g_csPovForward = I("cutscene_pov_forward_units", 100);
    if (g_csPovForward < 0) g_csPovForward = 0;
    if (g_csPovForward > 600) g_csPovForward = 600;
    g_csPovFallbackHead = I("cutscene_pov_fallback_head_units", 700);
    g_csPovCutUnits = I("cutscene_pov_cut_units", 1500);
    if (g_csPovCutUnits < 200) g_csPovCutUnits = 200;
    g_csPovYawOffsetDeg = I("cutscene_pov_yaw_offset_deg", 0);
    g_csPovInvertPitch = I("cutscene_pov_invert_pitch", 0) != 0;
    g_csPovPosition = I("cutscene_pov_position", 1) != 0;
    g_csPovStereo = I("cutscene_pov_stereo", 1) != 0;
    g_csPovFov = I("cutscene_pov_fov", 1) != 0;
    g_csPovAbsentFrames = I("cutscene_pov_absent_frames", 3);
    if (g_csPovAbsentFrames < 1) g_csPovAbsentFrames = 1;
    g_csPovLogLines = I("cutscene_pov_log_lines", 120);
    g_csPovHideSnake = I("cutscene_pov_hide_snake", 1) != 0;
    g_csLetterbox = I("cutscene_pov_letterbox", 0);
    g_csSubPanel = I("cutscene_pov_subtitle_panel", 1) != 0;
    g_gpPov = I("gameplay_pov", 1) != 0 ? 1 : 0;
    g_gpPovForward = I("gameplay_pov_forward_units", 80);
    if (g_gpPovForward < 0) g_gpPovForward = 0;
    if (g_gpPovForward > 600) g_gpPovForward = 600;
    g_gpPovFollowBody = I("gameplay_pov_follow_body", 1) != 0;
    g_gpPovVehBack = I("gameplay_pov_vehicle_back_units", 250);
    g_gpPovVehUp = I("gameplay_pov_vehicle_up_units", 120);
    g_gpPovRope = I("gameplay_pov_rope", 1) != 0 ? 1 : 0;
    g_gpPovRopeForward = I("gameplay_pov_rope_forward_units", 40);
    if (g_gpPovRopeForward < 0) g_gpPovRopeForward = 0;
    if (g_gpPovRopeForward > 600) g_gpPovRopeForward = 600;
    g_gpPovTorture = I("gameplay_pov_torture", 1) != 0 ? 1 : 0;
    g_gpPovTortureForward = I("gameplay_pov_torture_forward_units", 80);
    if (g_gpPovTortureForward < 0) g_gpPovTortureForward = 0;
    if (g_gpPovTortureForward > 600) g_gpPovTortureForward = 600;
    g_gpPovTortureYawOffsetDeg = I("gameplay_pov_torture_yaw_offset_deg", 0);
    g_gpPovHideBody = I("gameplay_pov_hide_body", 1) != 0;
    g_gpPovFov = I("gameplay_pov_fov", 1) != 0;
    g_gpPovPosition = I("gameplay_pov_position", 1) != 0;
    g_gpPovLogLines = I("gameplay_pov_log_lines", 60);
    g_gpPovStickFollowsView = I("gameplay_pov_stick_follows_view", 1);
    g_gpPovRightStickTurn = I("gameplay_pov_right_stick_turn", 1);
    g_gpPovRexFaceView = I("gameplay_pov_rex_face_view", 1);
    g_gpPovLogEvery = I("gameplay_pov_log_every", 30);
    if (g_gpPovLogEvery < 1) g_gpPovLogEvery = 1;
    DebugLogger::LogFormat(
        "Gameplay Snake's eyes: %s -- when the game takes the camera out of first person during play "
        "(elevator, ladder, scripted shots) the view is rebuilt from Snake's head joint and your head "
        "turns it. forward=%d follow_body=%d hide_body=%d fov=%d position=%d. Cutscenes: hide Snake's "
        "own model=%d, demo letterbox bars %s. GpPov[] lines in mgs1_vr_test.log.",
        g_gpPov ? "ON" : "off", g_gpPovForward, g_gpPovFollowBody ? 1 : 0, g_gpPovHideBody ? 1 : 0,
        g_gpPovFov ? 1 : 0, g_gpPovPosition ? 1 : 0, g_csPovHideSnake ? 1 : 0,
        g_csLetterbox ? "kept" : "removed");
    g_csPovLogEvery = I("cutscene_pov_log_every", 30);
    if (g_csPovLogEvery < 1) g_csPovLogEvery = 1;
    {
        // Snake's model names. "snake" always; anything else from the ini,
        // as names ("sne_nude") or raw hashes ("0x992D"), comma separated.
        g_csPovHashCount = 0;
        g_csPovHashes[g_csPovHashCount++] = GvStrCode("snake");
        char buf[512] = {};
        GetPrivateProfileStringA("camera_hook", "cutscene_pov_snake_models", "", buf, sizeof(buf), ini.c_str());
        char* ctx = nullptr;
        for (char* tok = strtok_s(buf, ", \t", &ctx); tok && g_csPovHashCount < 16;
             tok = strtok_s(nullptr, ", \t", &ctx)) {
            uint16_t h = 0;
            if ((tok[0] == '0') && (tok[1] == 'x' || tok[1] == 'X')) h = (uint16_t)strtoul(tok, nullptr, 16);
            else h = GvStrCode(tok);
            if (h != 0) g_csPovHashes[g_csPovHashCount++] = h;
        }
        char list[160] = {};
        int w = 0;
        for (int i = 0; i < g_csPovHashCount; ++i) {
            const int n = _snprintf_s(list + w, sizeof(list) - w, _TRUNCATE, "%s%04X", i ? " " : "", g_csPovHashes[i]);
            if (n > 0) w += n;
        }
        DebugLogger::LogFormat(
            "Cutscene view: %s. Snake's eyes reads him straight out of the demo system's own cast "
            "(MGSDEMOACT+0x34, one DEMO_MODEL per character, posed each frame by ShowScene at "
            "+1CECDA) -- head joint 6, the joint the game's first-person code uses. Only YOUR head "
            "turns the view; you start facing where Snake faces and re-face only when the scene "
            "moves him somewhere new (> %d units). Shots without Snake: %s. forward=%d fov=%s "
            "stereo=%d position=%d. Snake model hashes: %s (+ whatever outfit the player object was "
            "last wearing, matched by DG_DEF). Pov[] lines in mgs1_vr_test.log, and a Demo model "
            "table in the debug log at the start of every cutscene.",
            g_csView ? "SNAKE'S EYES" : "director camera + head look",
            g_csPovCutUnits, g_csPovNoSnake == 1 ? "virtual screen" : (g_csPovNoSnake == 2 ? "Snake's last eye while the shot stays near him, else the director camera" : "director camera"),
            g_csPovForward, g_csPovFov ? "mod lens (fov_clip_distance)" : "scene's own",
            g_csPovStereo ? 1 : 0, g_csPovPosition ? 1 : 0, list);
    }
    g_povEyeOffset = I("cutscene_snake_pov_eye_offset", 0);
    g_povFallbackEye = I("cutscene_snake_pov_fallback_eye", 0);
    // Default 1 -- captured ONLY. If nothing has been captured yet, Snake POV
    // simply does not engage and the cutscene plays on the director's camera,
    // which is the behaviour that already worked. Falling back to a pointer
    // that has been shown to be wrong would trade a known-good picture for a
    // known-bad one, which is not a fallback.
    // Default 0 now: the sweep did its job and answered. It found +0x020,
    // +0x044 and +0xA60 -- three copies of one value, all written by the same
    // camera routine, with +0xA60 reading (0,0,0) through the whole cutscene.
    // The actor does not carry a second position field. Turn it back on only
    // to repeat that measurement.
    g_fieldScan = I("snake_field_scan", 0) != 0 ? 1 : 0;
    g_fieldScanLines = I("snake_field_scan_lines", 24);
    g_fieldScanEvery = I("snake_field_scan_every", 120);
    if (g_fieldScanEvery < 1) g_fieldScanEvery = 1;
    DebugLogger::LogFormat(
        "Snake field sweep: %d (every %d camera frames, %d lines max). Writes Fields[] and Cand[] "
        "lines to mgs1_vr_test.log. In FIRST PERSON, 0x593FA0 IS Snake's eye and the position "
        "detour fires one instruction after the game writes it -- so the sweep asks the 0xA74-byte "
        "actor object which of ITS fields currently equals that value, as an int16 (x,_,z) pair. "
        "CORRECTION (fourth run): [actor+0x20] is NOT a destination. +E1A79 calls +40241F with "
        "(obj+0x20, obj+0x848); +40241F forwards the same pair to +4022EC, which READS arg0 as a "
        "position -- `movsx eax,[ebx]`, `[ebx+2]`, `[ebx+4]` -- to look the point up in a zone "
        "table, and WRITES arg1. +5CD367 calls the same function with the cutscene camera's own "
        "position as arg0, which settles the direction. So +0x20 is a genuine world position "
        "field and the ~(150,550,-570) it reports during a cutscene is a real coordinate of "
        "SOMETHING -- just, apparently, not of the character the scene is filming. Cand[] reads "
        "offset the sweep found back during the cutscene. The one still moving, and still in the "
        "shot's coordinate range, is Snake. A search with a known answer, not another guess.",
        g_fieldScan, g_fieldScanEvery, g_fieldScanLines);

    g_demoScan = I("demo_heap_scan", 1) != 0 ? 1 : 0;
    g_demoScanEvery = I("demo_heap_scan_every", 60);
    if (g_demoScanEvery < 1) g_demoScanEvery = 1;
    g_demoScanLines = I("demo_heap_scan_lines", 24);
    g_demoScanWindowKb = I("demo_heap_scan_window_kb", 1024);
    g_demoScanInt32 = I("demo_heap_scan_int32", 1) != 0 ? 1 : 0;
    g_demoScanSkipExact = I("demo_heap_scan_skip_exact", 1) != 0 ? 1 : 0;
    g_demoScanRadius = I("demo_heap_scan_radius", 4000);
    g_demoScanHits = I("demo_heap_scan_hits", 10);
    DebugLogger::LogFormat(
        "Demo heap sweep: %d (every %d cutscene frames, %d dumps max, %d KB window, radius %d, "
        "%d hits). The 2026-08-31 log ended the pointer hunt: cap= and glob= are the SAME address "
        "and BOTH fail the liveness gate (rej=2, model=00000000) for essentially the whole scene. "
        "The player object DOES NOT EXIST during this cutscene -- it is constructed afterwards, "
        "which is why 008FF400 only appears once the scene ends. The demo animates its cast from "
        "its own data. So this stops chasing the player pointer and searches where the characters "
        "actually are, using the one known answer the scene gives away for free: the director is "
        "LOOKING at them, so TO is on or near a character every frame. The sweep walks a window of "
        "the demo heap around the script chunk the +1CD292 hook already holds (02C2xxxx) and "
        "reports every int16 (x,y,z) triple within the radius of TO, closest first. Demo[] lines "
        "in mgs1_vr_test.log. A cluster of hits at a STABLE OFFSET from the chunk, tracking the "
        "shot across cuts, is the cast; the one nearest TO on a Snake-facing shot is him.",
        g_demoScan, g_demoScanEvery, g_demoScanLines, g_demoScanWindowKb,
        g_demoScanRadius, g_demoScanHits);

    g_xformScan = I("transform_scan", 1) != 0 ? 1 : 0;
    g_xformEvery = I("transform_scan_every", 30);
    if (g_xformEvery < 1) g_xformEvery = 1;
    g_xformLines = I("transform_scan_lines", 40);
    DebugLogger::LogFormat(
        "Transform log: %d (every %d cutscene frames, %d dumps max). ASK THE RENDERER, NOT THE "
        "HEAP. 0x407ADA(SVECTOR* translation, SVECTOR* rotation) is this engine's SetRotMatrix + "
        "SetTransMatrix: it runs RotMatrix on arg1 and writes arg0's three shorts into the GTE's "
        "TRX/TRY/TRZ at 0x993E54. There are 424 call sites -- it is the universal per-object "
        "transform, and nothing reaches the screen without passing through it. So if a character "
        "is visible during the demo, its translation went through this function on that frame, "
        "whether or not any object, task or cast node exists for it. Each Xform[] line groups the "
        "translations by CALLER RVA, which names the code that drew them (see task_roster.md). "
        "That is the difference between an anonymous coordinate triple and an identification.",
        g_xformScan, g_xformEvery, g_xformLines);

    g_demoCastScan = I("demo_cast_scan", 1) != 0 ? 1 : 0;
    g_demoCastEvery = I("demo_cast_scan_every", 30);
    if (g_demoCastEvery < 1) g_demoCastEvery = 1;
    g_demoCastLines = I("demo_cast_scan_lines", 60);
    g_demoCastMax = I("demo_cast_scan_max", 32);
    DebugLogger::LogFormat(
        "Demo cast walk: %d (every %d cutscene frames, %d dumps max, %d nodes). THE CORRECTION "
        "THAT MATTERS. The +1CD292 hook has always captured arg1 -- the script CHUNK -- and never "
        "looked at arg0, which is the SCENE object. 0x5CD292(scene, chunk) mark-and-sweeps the "
        "shot's cast into a list at scene+0x38: one malloc(0x78) node per scripted element, with "
        "the 0x34-byte script record copied in at +0x14 (id +0x14, command +0x18, SVECTOR rotation "
        "+0x1C, SVECTOR position +0x22). The demo has been handing us its cast on every call for "
        "six builds. Cast[] lines in mgs1_vr_test.log walk it. Commands in 1..0x4A and positions "
        "in the shot's coordinate range prove the walk; a node whose +0x22 tracks the character "
        "the camera is framing is the one Snake's POV should read.",
        g_demoCastScan, g_demoCastEvery, g_demoCastLines, g_demoCastMax);

    g_taskScan = I("task_name_scan", 1) != 0 ? 1 : 0;
    g_taskScanEvery = I("task_name_scan_every", 60);
    if (g_taskScanEvery < 1) g_taskScanEvery = 1;
    g_taskScanLines = I("task_name_scan_lines", 40);
    g_taskScanMax = I("task_name_scan_max", 64);
    g_taskScanClassMask = I("task_name_scan_class_mask", 0xFFFF);
    DebugLogger::LogFormat(
        "Task name walk: %d (every %d cutscene frames, %d dumps max, %d nodes per class, "
        "class mask %04X). The shipped binary still carries its ORIGINAL SOURCE PATHS: "
        "0x40A347(obj, init, update, name) stores a pointer at [obj+0x14] to strings like "
        "\"C:\\mgs\\source\\chara\\snake\\sna_init.c\" -- 477 registrations, 426 distinct files. "
        "So every live object can name itself, and this walk is self-validating: readable "
        "source paths mean the list traversal is correct, garbage means it is not, and there is "
        "no third answer. It also explains why the class-5 walk found no cast -- class 5 is the "
        "PLAYER's class; the demo's characters (animal\\zako*, animal\\meryl*, animal\\ninja, "
        "animal\\snake18) are CLASS 4. Task[] lines in mgs1_vr_test.log list every live object "
        "as class, address, name and [+0x20] triple.",
        g_taskScan, g_taskScanEvery, g_taskScanLines, g_taskScanMax, (unsigned)g_taskScanClassMask);

    g_actorSigScan = I("actor_signature_scan", 1) != 0 ? 1 : 0;
    g_actorSigEvery = I("actor_signature_scan_every", 60);
    if (g_actorSigEvery < 1) g_actorSigEvery = 1;
    g_actorSigLines = I("actor_signature_scan_lines", 24);
    g_actorSigHits = I("actor_signature_scan_hits", 12);
    g_actorSigRadius = I("actor_signature_scan_radius", 24000);
    g_actorSigBudgetMb = I("actor_signature_scan_budget_mb", 96);
    g_actorSigRequireFlags = I("actor_signature_scan_require_flags", 0) != 0 ? 1 : 0;
    DebugLogger::LogFormat(
        "Actor signature scan: %d (every %d cutscene frames, %d dumps max, %d hits, radius %d, "
        "%d MB budget, require flags %d). This replaces the coordinate sweep, which returned no "
        "32-bit hits at all and whose 16-bit hits were echoes of TO, repeated constants and "
        "smooth interpolation ladders -- camera-path buffers, not bodies -- while reading only "
        "32-90 of its 256 blocks. Rather than looking for a number, this looks for the SHAPE of "
        "an actor: a model pointer at +0x9C, an int16 world position at +0x20, a flags byte at "
        "+0x895 and a stance word at +0xA26, all four of which the engine's own accessors read. "
        "VirtualQuery walks every committed private region instead of a fixed window, so "
        "coverage is no longer a matter of luck. Sig[] lines in mgs1_vr_test.log list every "
        "object-shaped block whose position is within the radius of the shot's look-at point, "
        "closest first, with its model pointer and flags. A row that PERSISTS across frames and "
        "MOVES is a live actor; the one the camera keeps framing on Snake-facing shots is him.",
        g_actorSigScan, g_actorSigEvery, g_actorSigLines, g_actorSigHits, g_actorSigRadius,
        g_actorSigBudgetMb, g_actorSigRequireFlags);

    g_classScan = I("class_list_scan", 0) != 0 ? 1 : 0;
    g_classScanEvery = I("class_list_scan_every", 60);
    if (g_classScanEvery < 1) g_classScanEvery = 1;
    g_classScanLines = I("class_list_scan_lines", 20);
    DebugLogger::LogFormat(
        "Class list scan: %d (every %d camera frames, %d lines max). Walks ALL SIXTEEN object "
        "lists at 0x6BFC98 + class*0x44 and reports which one contains the actor pointer the "
        "engine handed us at +E1B58. The 2026-08-25 log made this necessary: class 5 has 21 "
        "nodes and the player is not among them, and the nodes it does have are map furniture "
        "-- an arithmetic run of (-16432,143,1), (-17744,143,1), (-19056,143,1) stepping -1312, "
        "plus round numbers like (20250,1500,3000). Exactly one list should contain the pointer; "
        "that list is the cast, and the Class[] line names it.",
        g_classScan, g_classScanEvery, g_classScanLines);

    g_actorListScan = I("actor_list_scan", 0) != 0 ? 1 : 0;
    g_actorListClass = I("actor_list_class", 5);
    if (g_actorListClass < 0 || g_actorListClass > 15) g_actorListClass = 5;
    g_actorListEvery = I("actor_list_every", 60);
    if (g_actorListEvery < 1) g_actorListEvery = 1;
    g_actorListLines = I("actor_list_lines", 30);
    g_actorListMax = I("actor_list_max", 48);
    if (g_actorListMax < 1) g_actorListMax = 1;
    DebugLogger::LogFormat(
        "Actor list walk: %d (class %d, every %d camera frames, %d dumps max, %d nodes max). "
        "The engine keeps every object in an intrusive doubly-linked list and keeps the list "
        "heads in one table: 0x40A2AF does `imul eax,[class],0x44 / add eax,0x6BFC98`, sets "
        "[head]=obj, obj+0x4=head, obj+0x0=old head. The player is allocated at +1E1701 with "
        "class 5 and size 0xA74, and 211 of the binary's 468 allocation sites use class 5 -- it "
        "is the game-object class. So the whole cast can be walked from a head address the "
        "engine computes itself, with nothing guessed. Actors[] lines in mgs1_vr_test.log list "
        "every node's [+0x20] position, its [+0x9A8] state and its distance to the shot's "
        "look-at target. This replaces the field sweep, which asked which FIELD of one object "
        "holds a position; during a demo the real question is which OBJECT is Snake, because "
        "the scene may be animating a different one than the first-person camera was built "
        "from. If no node is near the look-at target, actors and camera are not in one "
        "coordinate space and that is the next thing to fix instead.",
        g_actorListScan, g_actorListClass, g_actorListEvery, g_actorListLines, g_actorListMax);

    g_povActorSource = I("cutscene_snake_pov_actor_source", 2);
    if (g_povActorSource < 0 || g_povActorSource > 4) g_povActorSource = 2;
    g_povEyeMaxDelta = I("cutscene_snake_pov_eye_max_delta", 3000);
    if (g_povEyeMaxDelta < 1) g_povEyeMaxDelta = 1;
    g_povProbeLines = I("cutscene_snake_pov_probe_lines", 240);
    g_povProbeEvery = I("cutscene_snake_pov_probe_every", 15);
    if (g_povProbeEvery < 1) g_povProbeEvery = 1;
    DebugLogger::LogFormat(
        "Snake POV in cutscenes: %d. actor_source=%d (0=+334228 global, 1=captured, 2=captured "
        "then global, 3=list node then captured, 4=global then captured). EVERY candidate now "
        "passes a LIVENESS GATE before it is allowed to drive: [obj+0x9C] must be a plausible "
        "pointer, because the engine dereferences that field itself at +E1AB4. The 2026-08-26 "
        "log is why -- for most of a cutscene the captured object reads [obj+0x9C]=00000000 and "
        "f895=00, i.e. CLEARED, with occasional frames where it is rebuilt. That flicker between "
        "a live object and a dead one is what put the view alternately at Snake and back on the "
        "director's camera. The default is now 2: captured first, then the +334228 global, first "
        "one that is actually live. The global matters because +E174B writes it at CONSTRUCTION "
        "time, which happens during the scene, whereas the capture site is the first-person "
        "camera routine, which does not run then -- so across a room change the capture is "
        "frozen on the previous room's player and only the global can see the new one. "
        "THE ACTOR IS TAKEN, NOT DERIVED: the position detour at +E1B58 "
        "publishes `esi`, which is the actor the game's own first-person camera routine was "
        "handed at that instant (ebx was set to esi+0x20 at +E1A74 and esi is callee-saved). "
        "The 2026-08-24 derivation through the global at +334228 was refuted by its own probe -- "
        "distTO put that object 13000-17000 units from the shot's look-at target, its position "
        "never travelled more than ~200 units, and [obj+0x9C] read back as 0xF38A06C4, which is "
        "not a pointer. Snake's EYE then moves to [obj+0x20/+0x24] with head height from "
        "*(int16*)(*(void**)(obj+0x9C) + 0x288), the field +E1AB4 substitutes. The shot's "
        "DIRECTION is left alone, so the scene still pans and cuts as directed. "
        "The head read now carries the engine's OWN two guards, which the first build did not: "
        "it is refused when bit 3 of [obj+0x895] is set (+E1AA2) and 0x140 is added to the eye "
        "when the stance word [obj+0xA26] is 2 (+E1AC5). On top of those, a reading further than "
        "eye_max_delta=%d units from Snake's feet is rejected outright and the last accepted "
        "head/feet separation is reused -- a head does not move a room's width between frames, "
        "and the unguarded read is what put the eye 31000 units up last build. "
        "eye_offset=%d fallback_eye=%d (the seed separation, used only before any reading has "
        "been accepted). R3 double-press still leaves all of this behind for the native virtual "
        "screen. Snake[] probe lines go to mgs1_vr_test.log every %d calls, %d max.",
        g_povMode, g_povActorSource, g_povEyeMaxDelta, g_povEyeOffset, g_povFallbackEye,
        g_povProbeEvery, g_povProbeLines);
    LoadScopeConfig();
}


inline float NormalizeRad(float a) {
    while (a > kPi)  a -= kTwoPi;
    while (a < -kPi) a += kTwoPi;
    return a;
}

// Radians -> signed 12-bit engine units. No +PI offset: the original code
// added PI before scaling, which mapped "head straight ahead" onto 2048
// (== 180 degrees) and made every target antipodal to the game's neutral.
inline int32_t RadToUnits(float rad) {
    return (int32_t)((rad / kTwoPi) * kUnitsPerTurn);
}

inline int32_t WrapUnits(int32_t v) {
    v %= 4096;
    if (v < 0) v += 4096;
    return v;
}

inline int32_t Clamp(int32_t v, int32_t lo, int32_t hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

// --- SEH leaf helpers -------------------------------------------------------
// MSVC rejects __try in any function it considers to require object unwinding
// (C2712), so every guarded access lives in its own tiny function containing
// nothing but the access. Mirrors SafeRead16/SafeWrite16 in vr_injection.cpp.
bool SafeRead32(uintptr_t a, int32_t* o) {
    __try { *o = *(volatile int32_t*)a; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool SafeRead16(uintptr_t a, int16_t* o) {
    __try { *o = *(volatile int16_t*)a; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool SafeWrite32(uintptr_t a, uint32_t v) {
    __try { *(volatile uint32_t*)a = v; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool SafeWrite16(uintptr_t a, int16_t v) {
    __try { *(volatile int16_t*)a = v; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool SafeRead8(uintptr_t a, uint8_t* o) {
    __try { *o = *(volatile uint8_t*)a; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool SafeWrite8(uintptr_t a, uint8_t v) {
    __try { *(volatile uint8_t*)a = v; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// --- the field sweep, run from the position detour (FPV only) --------------
// See the comment on g_fieldOff. This asks the actor object which of its
// fields currently equals the value the game just wrote to 0x593FA0, which in
// first person IS Snake. A search with a known answer beats another guess.
//
// Cost: one pass over 0xA74 bytes, every g_fieldScanEvery calls, capped at
// g_fieldScanLines log lines. It runs on the game thread, so it is bounded on
// purpose rather than left to run every frame.
void ScanActorFields(uintptr_t obj) {
    if (!g_fieldScan || g_moduleBase == 0) return;
    static int  s_lines = 0;
    static unsigned s_tick = 0;
    if (s_lines >= g_fieldScanLines) return;
    if ((s_tick++ % (unsigned)(g_fieldScanEvery < 1 ? 1 : g_fieldScanEvery)) != 0) return;

    int16_t tx = 0, tz = 0;
    if (!SafeRead16(g_moduleBase + kPosXOffset, &tx)) return;
    if (!SafeRead16(g_moduleBase + kPosZOffset, &tz)) return;
    // A target of (0,0) matches everything zeroed. Useless, and it would fill
    // the slots with noise, so refuse rather than report a false positive.
    if (tx == 0 && tz == 0) return;

    g_fieldCount = 0;
    char hits[512];
    int  used = 0;
    hits[0] = '\0';
    for (int off = 0; off + 4 <= 0xA74 && g_fieldCount < kFieldSlots; off += 2) {
        int16_t x = 0, z = 0;
        if (!SafeRead16(obj + (unsigned)off + 0, &x)) continue;
        if (!SafeRead16(obj + (unsigned)off + 4, &z)) continue;
        if (x != tx || z != tz) continue;
        int16_t y = 0;
        SafeRead16(obj + (unsigned)off + 2, &y);
        g_fieldOff[g_fieldCount++] = off;
        const int n = _snprintf_s(hits + used, sizeof(hits) - used, _TRUNCATE,
            "%s+0x%03X(y=%d)", used ? " " : "", off, (int)y);
        if (n > 0) used += n;
    }

    s_lines++;
    DebugLogger::TestLogFormat(
        "Fields[%d]: obj=%08X target 593FA0=(x=%d z=%d) -> %d match%s: %s",
        s_lines, (unsigned)obj, (int)tx, (int)tz, g_fieldCount,
        g_fieldCount == 1 ? "" : "es",
        g_fieldCount ? hits : "(none -- the actor does not store its own world position as int16 x,_,z)");
}

bool SafeReadBytes(uint8_t* dst, const uint8_t* src, size_t n) {
    __try {
        for (size_t i = 0; i < n; ++i) dst[i] = src[i];
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

inline bool ReadFpvFlag() {
    int32_t v = 0;
    if (!SafeRead32(g_moduleBase + kFpvFlagOffset, &v)) return false;
    return v != 0;
}

} // namespace

// In-headset VR Settings panel (2026-10-05): re-read ONLY the end-user options
// it offers. LoadCameraHookConfig() itself is not re-run live -- it also resets
// diagnostic scan state and the Snake model hash list the game thread reads.
// Defined OUTSIDE the anonymous namespace (vr_settings.cpp links to it).
// Each of these is a plain int/bool the game thread reads per frame, so a
// change lands on the next game frame.
void ReloadCameraHookLiveSettings() {
    const std::string ini = GetGameIniPath();
    auto I = [&](const char* k, int d) { return GetPrivateProfileIntA("camera_hook", k, d, ini.c_str()); };
    g_csView = I("cutscene_view", 1) != 0 ? 1 : 0;
    int bf = I("body_follows_head", 1);
    if (bf < 0 || bf > 2) bf = 1;
    g_bodyFollowsHead = bf;
    int ns = I("cutscene_pov_no_snake", 2);
    if (ns < 0 || ns > 2) ns = 2;
    g_csPovNoSnake = ns;
    g_csLetterbox = I("cutscene_pov_letterbox", 0);
    g_csSubPanel = I("cutscene_pov_subtitle_panel", 1) != 0;
    g_gpPovStickFollowsView = I("gameplay_pov_stick_follows_view", 1);
    g_gpPovRightStickTurn = I("gameplay_pov_right_stick_turn", 1);
    DebugLogger::LogFormat("Camera live settings: body_follows_head=%d", g_bodyFollowsHead);
    DebugLogger::LogFormat("Camera live settings: cutscene_view=%d no_snake=%d letterbox=%d caption_panel=%d "
        "stick_follows_view=%d right_stick_turn=%d",
        g_csView, g_csPovNoSnake, g_csLetterbox, g_csSubPanel ? 1 : 0, g_gpPovStickFollowsView, g_gpPovRightStickTurn);
    LoadScopeConfig();
}

// ===========================================================================
// ROTATION hook body. Runs on the game's thread, once per game frame.
// gameRollDword is what the replaced instruction was about to store into
// 0x993FCC. We always perform that store first, so behaviour is identical to
// stock when the override is off, and the vector's 4th component is preserved.
// ===========================================================================
extern "C" void __cdecl Mgs1CameraRotWriteBody(uint32_t gameRollDword) {
    // LIVENESS. Bumped unconditionally, before any gate, because the whole
    // cutscene-VR question is "does this function still get called during a
    // cutscene". The flip tick compares this against presented frames; if it
    // stops moving while frames keep flipping, path A is unavailable and the
    // fallback takes over. One relaxed increment per game frame.
    g_rotHookRuns.fetch_add(1, std::memory_order_relaxed);
    g_rotHookTick.store(GetTickCount64(), std::memory_order_relaxed);

    if (!SafeWrite32(g_moduleBase + kRollOffset, gameRollDword)) return;

    // ---- UN-APPLY last frame's position offset -------------------------
    // Critical, and the reason the first version of the position hook
    // misbehaved. One branch of the game's position update is
    // GV_NearExp4PV(993FA0, edi, 3) -- an INTERPOLATOR that reads the current
    // position and steps a quarter of the way toward its target. Adding our
    // offset on top of the live value every frame therefore feeds our own
    // offset back into the interpolation, converging on roughly 4x the
    // intended displacement rather than settling at 1x.
    //
    // This hook (RVA 0xE1AF8) runs EARLIER in the same function than the
    // position update (0xE1B36..0xE1B58), so removing the offset here hands
    // the game back its own unmodified value before it computes the next one.
    // The position hook then re-applies cleanly. Correct for both the
    // interpolated and the direct-write branch.
    if (g_havePrevPosOffset) {
        int16_t px = 0, py = 0, pz = 0;
        if (SafeRead16(g_moduleBase + kPosXOffset, &px) &&
            SafeRead16(g_moduleBase + kPosYOffset, &py) &&
            SafeRead16(g_moduleBase + kPosZOffset, &pz)) {
            SafeWrite16(g_moduleBase + kPosXOffset, (int16_t)((int32_t)px - g_prevPosOffX));
            SafeWrite16(g_moduleBase + kPosYOffset, (int16_t)((int32_t)py - g_prevPosOffY));
            SafeWrite16(g_moduleBase + kPosZOffset, (int16_t)((int32_t)pz - g_prevPosOffZ));
        }
        g_havePrevPosOffset = false;
        g_prevPosOffX = g_prevPosOffY = g_prevPosOffZ = 0;
    }

    if (!g_enabled.load(std::memory_order_relaxed)) return;

    // ---- DEBOUNCED FPV state ----------------------------------------------
    // The raw flag flickers. Only act on a transition once the new value has
    // survived g_fpvDebounceFrames consecutive frames; until then keep using
    // the state we already believe, so a 2-frame glitch no longer throws away
    // the reference pose and snaps the view.
    const bool fpvRaw = ReadFpvFlag();
    if (fpvRaw != g_lastFpvState) {
        if (++g_fpvPendingFrames >= g_fpvDebounceFrames) {
            g_lastFpvState = fpvRaw;
            g_fpvPendingFrames = 0;
            g_haveReference = false;        // recentre on every real FPV entry
            g_yawOffsetValid.store(false, std::memory_order_relaxed);
            // First person has now been established at least once, so the
            // maintenance loop is allowed to put us back when the game takes
            // it away. See the note at g_fpvMaintenanceArmed.
            if (fpvRaw) g_fpvMaintenanceArmed.store(true, std::memory_order_relaxed);
            DebugLogger::LogFormat("Camera hook: FPV state -> %d (reference dropped)", fpvRaw ? 1 : 0);
        }
    }
    else if (g_fpvPendingFrames != 0) {
        DebugLogger::LogFormat("Camera hook: FPV glitch absorbed after %d frame(s)", g_fpvPendingFrames);
        g_fpvPendingFrames = 0;
    }

    const bool fpv = g_lastFpvState;

    // ---- NON-FIRST-PERSON VR CAMERA MODES, once per game frame -------------
    // Two modes live here -- the third-person "diorama" and cutscene VR -- and
    // they deliberately share ONE cut guard, because they have exactly the
    // same problem: MGS1 teleports its camera between rooms, and cutscenes
    // teleport it again between shots. An instant viewpoint jump is genuinely
    // unpleasant in a headset either way.
    //
    // PRECEDENCE: CUTSCENE VR WINS. Both modes engage on "not in first
    // person", so during a cutscene with third_person_vr=1 they would
    // otherwise both be driving rotate2 in the same frame. That is the "one
    // writer per value per frame" rule from the handoff's working practices,
    // and it is the rule this project has broken most often.
    //
    // With cutscene_detect_mode=0 and third_person_vr=0 this whole block is
    // two cheap falses and every gate below reduces to exactly its previous
    // first-person-only form.
    bool tpv = false;
    bool csv = false;
    const bool hookEnabled = g_enabled.load(std::memory_order_relaxed);
    // VIEW MODE IS THE OUTERMOST GATE. In native mode every one of the three VR
    // camera modes is off, which makes `vrCam` false, which returns out of this
    // function before a single write -- no rotation, no FOV, no player heading,
    // no stereo tag. The un-apply block above has already run by this point, so
    // the frame native mode is entered on also hands back the last position
    // offset. That is the whole of "the mod writes nothing": one flag, checked
    // where every mode is decided, rather than a check bolted onto each write.
    const bool vrMode = g_vrViewMode.load(std::memory_order_relaxed);

    // Hand the field of view back on the way out. Runs here, above every
    // return below it, because native mode's whole point is that the frames
    // after the switch look like the game -- one of them being wide-angle
    // would be a visible seam. Once, on the game's own thread, then never
    // again until the next time VR mode writes the field.
    if (!vrMode && g_fovRestorePending.exchange(false, std::memory_order_relaxed)) {
        if (g_haveFovOriginal && g_fovClipDistance != 0) {
            if (SafeWrite16(g_moduleBase + kFovClipDistanceRva, g_fovOriginal)) {
                DebugLogger::LogFormat(
                    "View mode: field of view handed back to the game (clip_distance %d -> %d).",
                    g_fovClipDistance, (int)g_fovOriginal);
            }
        }
    }

    // PATH A stands down when the view commit hook owns the cutscene camera.
    // Both would apply the same head delta -- PATH A into layer 1, which the
    // copy at 0x53D4B feeds into layer 3, and the view commit hook into layer 3
    // directly -- so leaving both on doubles every head movement in exactly the
    // scenes where the game happens to still be copying layer 1. One writer per
    // value per frame; the layers do not make that rule negotiable.
    const bool vcOwnsCutscene = (g_vcInstalled && g_vcMode != 0);
    const bool wantCutscene = vrMode && (g_cutsceneDetectMode != 0) && !fpv && hookEnabled &&
        !vcOwnsCutscene && g_csvDetected.load(std::memory_order_relaxed);
    const bool wantDiorama = vrMode && g_thirdPersonVr && !fpv && hookEnabled && !wantCutscene;

    if (wantCutscene || wantDiorama) {
        const int32_t cutUnits = wantCutscene ? g_cutsceneCutUnits : g_thirdPersonCutUnits;
        const int32_t cutHold = wantCutscene ? g_cutsceneCutHoldFrames : g_thirdPersonCutHoldFrames;
        // ROOM-CUT DETECTION. MGS1's fixed cameras cut hard between rooms and
        // every cut is an instant viewpoint teleport. Read the game's OWN
        // camera position -- this runs after the un-apply block above, so the
        // value here is the game's, not ours -- and treat a large single-frame
        // jump as a cut.
        //
        // Manhattan distance rather than Euclidean deliberately: this is a
        // threshold test, it runs every frame on the game's thread, and a
        // sqrt buys nothing. 400 units is ~20 cm at the measured
        // position_scale of ~2048 units/metre, i.e. ~6 m/s of camera travel,
        // which no scripted pan does but every room change exceeds by orders
        // of magnitude.
        int16_t cx = 0, cy = 0, cz = 0;
        if (SafeRead16(g_moduleBase + kPosXOffset, &cx) &&
            SafeRead16(g_moduleBase + kPosYOffset, &cy) &&
            SafeRead16(g_moduleBase + kPosZOffset, &cz)) {
            if (g_tpvHavePrevCamPos && cutUnits > 0) {
                int32_t dx = (int32_t)cx - g_tpvPrevCamX;
                int32_t dy = (int32_t)cy - g_tpvPrevCamY;
                int32_t dz = (int32_t)cz - g_tpvPrevCamZ;
                if (dx < 0) dx = -dx;
                if (dy < 0) dy = -dy;
                if (dz < 0) dz = -dz;
                const int32_t jump = dx + dy + dz;
                if (jump > cutUnits) {
                    g_tpvCutHoldCounter = cutHold;
                    // Drop the reference so the next frame re-anchors head
                    // rotation to the NEW scripted camera. Without this the
                    // old room's head-vs-camera offset is carried across the
                    // cut and the new room starts off-axis by however far you
                    // had turned in the previous one.
                    g_haveReference = false;
                    g_yawOffsetValid.store(false, std::memory_order_relaxed);
                    static int cutLog = 0;
                    if (cutLog < 12) {
                        cutLog++;
                        DebugLogger::LogFormat(
                            "%s: %s cut detected (camera jumped %d units in one frame, "
                            "threshold %d) -- suppressing our writes for %d frames and re-anchoring",
                            wantCutscene ? "Cutscene VR" : "Third-person VR",
                            wantCutscene ? "shot" : "room",
                            jump, cutUnits, cutHold);
                    }
                }
            }
            g_tpvPrevCamX = cx; g_tpvPrevCamY = cy; g_tpvPrevCamZ = cz;
            g_tpvHavePrevCamPos = true;
        }

        if (g_tpvCutHoldCounter > 0) {
            g_tpvCutHoldCounter--;   // stay stock for this frame
        }
        else if (wantCutscene) {
            csv = true;
        }
        else {
            tpv = true;
        }
    }
    else {
        g_tpvHavePrevCamPos = false;
        g_tpvCutHoldCounter = 0;
    }
    g_tpvActive = tpv;
    g_csvInFrame = csv;
    // OWNERSHIP, NOT WRITING. `csv` means "cutscene VR is writing this frame";
    // wantCutscene means "cutscene VR owns this scene", which stays true
    // through the few frames after a shot cut when the guard is deliberately
    // holding our writes back. The 2D-UI framing and the FPV auto-restore have
    // to key off OWNERSHIP -- keyed off the write flag they would flick the
    // scene onto the distant virtual screen and back on every single shot
    // change, and MGS1 cutscenes cut constantly.
    g_csvActive.store(wantCutscene, std::memory_order_relaxed);

    {
        static bool lastTpv = false;
        if (tpv != lastTpv) {
            lastTpv = tpv;
            DebugLogger::LogFormat("Third-person VR: %s", tpv ? "ENGAGED (diorama view)" : "disengaged");
        }
        static bool lastCsv = false;
        if (csv != lastCsv) {
            lastCsv = csv;
            DebugLogger::LogFormat(
                "Cutscene VR: %s -- PATH A (in-frame). The game's camera update IS still running "
                "during this scene, which is the good case: head rotation, 6DOF and stereo all use "
                "the normal confirmed-working code path.",
                csv ? "ENGAGED" : "disengaged");
        }
    }

    // From here on, "the VR camera is driving" means any of the three modes --
    // and only ever while the mod is in VR mode.
    const bool vrCam = vrMode && (fpv || tpv || csv);

    // ---- advance the stereo eye, once per game frame -----------------------
    // Done here because the rotation hook runs earlier in the same function
    // than the position hook, so the eye is settled before the position write
    // that actually applies the offset, and before the capture that tags the
    // resulting pixels.
    if (g_stereoAlternate && vrMode &&
        (fpv || (tpv && g_thirdPersonStereo) || (csv && g_cutsceneStereo)) &&
        g_enabled.load(std::memory_order_relaxed)) {
        // ---- MOTION HOLD: three independent detectors ----------------------
        // Any one of them arming holds mono for g_stereoHoldFrames. See the
        // note at the state block for what each covers and what the original
        // yaw-only version missed.
        bool  moving = false;
        float worstDeg = 0.0f;
        int32_t worstUnits = 0;

        // (a) HEAD angular velocity, off the published HMD pose. This LEADS
        //     the game camera -- the head has already moved by the time this
        //     frame's camera write happens -- so it arms the hold on the first
        //     frame of a swing rather than the second. It sees pitch too.
        if (g_havePose.load(std::memory_order_relaxed)) {
            const float hy = g_headYaw.load(std::memory_order_relaxed);
            const float hp = g_headPitch.load(std::memory_order_relaxed);
            if (g_haveMotionHeadRef) {
                const float dy = NormalizeRad(hy - g_prevMotionHeadYaw);
                const float dp = NormalizeRad(hp - g_prevMotionHeadPitch);
                const float deg = std::sqrt(dy * dy + dp * dp) * (180.0f / kPi);
                if (deg > worstDeg) worstDeg = deg;
                if (deg >= (float)g_stereoMotionDegPerFrame) moving = true;
            }
            g_prevMotionHeadYaw = hy;
            g_prevMotionHeadPitch = hp;
            g_haveMotionHeadRef = true;
        }

        // (b) The GAME's own view angles, yaw AND pitch. Catches stick turning
        //     and scripted camera moves, which the head detector cannot see.
        int16_t liveYaw = 0, livePitch = 0;
        if (SafeRead16(g_moduleBase + kYawOffset, &liveYaw) &&
            SafeRead16(g_moduleBase + kPitchOffset, &livePitch)) {
            const int32_t yawNow = (int32_t)liveYaw & 0x0FFF;
            const int32_t pitchNow = (int32_t)livePitch & 0x0FFF;
            if (g_prevFrameYawUnits >= 0) {
                int32_t dy = yawNow - g_prevFrameYawUnits;
                if (dy > 2048)  dy -= 4096;      // shortest way round
                if (dy < -2048) dy += 4096;
                int32_t dp = pitchNow - g_prevFramePitchUnits;
                if (dp > 2048)  dp -= 4096;
                if (dp < -2048) dp += 4096;
                const float deg = std::sqrt((float)(dy * dy + dp * dp))
                    * (360.0f / kUnitsPerTurn);
                if (deg > worstDeg) worstDeg = deg;
                if (deg >= (float)g_stereoMotionDegPerFrame) moving = true;
            }
            g_prevFrameYawUnits = yawNow;
            g_prevFramePitchUnits = pitchNow;
        }

        // (c) TRANSLATION -- the gap no ini value could close. Sampled HERE,
        //     at the top of the frame, which matters: last frame's eye/lean
        //     offset was un-applied a few lines above, so this reads the
        //     GAME's own camera position and not ours. Without that ordering
        //     the alternating IPD offset would itself look like motion every
        //     single frame and hold mono forever.
        int16_t gpx = 0, gpy = 0, gpz = 0;
        if (SafeRead16(g_moduleBase + kPosXOffset, &gpx) &&
            SafeRead16(g_moduleBase + kPosYOffset, &gpy) &&
            SafeRead16(g_moduleBase + kPosZOffset, &gpz)) {
            if (g_havePrevGamePos) {
                const int32_t ddx = (int32_t)gpx - g_prevGamePosX;
                const int32_t ddy = (int32_t)gpy - g_prevGamePosY;
                const int32_t ddz = (int32_t)gpz - g_prevGamePosZ;
                const float dist = std::sqrt((float)(ddx * ddx + ddy * ddy + ddz * ddz));
                if ((int32_t)dist > worstUnits) worstUnits = (int32_t)dist;
                if (g_stereoMotionUnitsPerFrame > 0 &&
                    dist >= (float)g_stereoMotionUnitsPerFrame) {
                    moving = true;
                }
            }
            g_prevGamePosX = gpx; g_prevGamePosY = gpy; g_prevGamePosZ = gpz;
            g_havePrevGamePos = true;
        }

        if (moving) g_stereoHoldCounter = g_stereoHoldFrames;

        // Heartbeat, so the hold can be judged from the log instead of by
        // feel: if the ghost is still there while this says MOVING, the cause
        // is not the hold and the next suspect is the per-eye pose tagging.
        static int holdLog = 0;
        if (holdLog < g_logFrames || (holdLog % 120) == 0) {
            DebugLogger::TestLogFormat(
                "Hold[%d]: %s (worst %.2f deg/frame vs %d, %d units/frame vs %d) counter=%d",
                holdLog, moving ? "MOVING -> mono" : "settled -> stereo",
                worstDeg, g_stereoMotionDegPerFrame,
                worstUnits, g_stereoMotionUnitsPerFrame,
                g_stereoHoldCounter);
        }
        holdLog++;

        // ---- frame-rate floor ---------------------------------------------
        // Measured on the game's own frame cadence, since this hook runs once
        // per game frame. Smoothed, because a single slow frame is normal.
        //
        // TWO BUGS FIXED HERE, both visible in the first headset log:
        //
        // (1) POISONED BY THE GAP. This block only runs inside the stereo
        //     branch, so it stops updating whenever you leave FPV. On the next
        //     FPV entry the first measured interval was the whole time you had
        //     spent outside it -- seconds -- and one sample of 3000 ms drags
        //     the EMA to ~330 ms. Result: "SUSPENDED at 0.4 fps" logged at the
        //     exact moment you entered first person, every single time, forcing
        //     mono for the first second of FPV. An interval far longer than a
        //     plausible frame is a GAP, not a slow frame, and must be dropped
        //     rather than averaged.
        //
        // (2) FLAPPING ON THE THRESHOLD. The log shows "SUSPENDED at 23.9" then
        //     "resumed at 25.4" within 16 ms. One threshold with no hysteresis
        //     oscillates whenever the true rate sits near the floor.
        {
            LARGE_INTEGER now{}, freq{};
            QueryPerformanceCounter(&now);
            QueryPerformanceFrequency(&freq);
            if (g_haveLastFrameQpc && freq.QuadPart > 0) {
                const double ms = (double)(now.QuadPart - g_lastFrameQpc.QuadPart) * 1000.0 / (double)freq.QuadPart;
                static double smoothedMs = 33.0;

                // 200 ms is 6 game frames. Nothing that is merely "a slow
                // frame" takes that long; anything that does is a gap (mode
                // change, load, menu) and feeding it to the average is what
                // produced the phantom 0.4 fps readings.
                const bool plausibleFrame = (ms > 0.0 && ms < 200.0);
                if (plausibleFrame) {
                    smoothedMs = smoothedMs * 0.9 + ms * 0.1;
                }

                const double fps = (smoothedMs > 0.0) ? (1000.0 / smoothedMs) : 0.0;

                // Hysteresis: drop out below the floor, come back only once
                // clearly above it.
                if (g_stereoMinFps > 0 && plausibleFrame) {
                    const double lowMark = (double)g_stereoMinFps;
                    const double highMark = (double)g_stereoMinFps + 4.0;
                    const bool low = g_lowFpsMonoActive ? (fps < highMark) : (fps < lowMark);
                    if (low != g_lowFpsMonoActive) {
                        g_lowFpsMonoActive = low;
                        DebugLogger::LogFormat(
                            "Stereo: %s at %.1f fps (floor %d) -- %s",
                            low ? "SUSPENDED" : "resumed", fps, g_stereoMinFps,
                            low ? "too slow to split frames between eyes, both eyes now track the game"
                                : "frame rate recovered");
                    }
                }
            }
            g_lastFrameQpc = now;
            g_haveLastFrameQpc = true;
        }

        if (IsRenderTwiceLive()) {
            // RENDER TWICE is drawing BOTH eyes of this frame from the head
            // centre and applies the eye offset itself, on the view matrix.
            // Publishing HOLD here makes every offset site below (position
            // hook, view commit) leave the camera centred -- one eye offset,
            // applied once. If pairs stop arriving, this lapses within
            // ~150 ms and alternate-eye stereo resumes by itself.
            g_publishedEye.store(2, std::memory_order_relaxed);
        }
        else if (g_lowFpsMonoActive) {
            g_publishedEye.store(2, std::memory_order_relaxed);   // both eyes
        }
        else if (g_stereoHoldCounter > 0) {
            g_stereoHoldCounter--;
            g_publishedEye.store(2, std::memory_order_relaxed);   // both eyes
        }
        else {
            g_stereoEye ^= 1;
            g_publishedEye.store(g_stereoEye, std::memory_order_relaxed);
        }
        // Stamp the tag as fresh. GetCurrentStereoEye() expires it if the game
        // stops calling us -- see the note at g_eyePublishTick.
        g_eyePublishTick.store(GetTickCount64(), std::memory_order_relaxed);
    }
    else {
        g_publishedEye.store(-1, std::memory_order_relaxed);
        g_eyePublishTick.store(GetTickCount64(), std::memory_order_relaxed);
        g_prevFrameYawUnits = -1;
        g_prevFramePitchUnits = 0;
        g_havePrevGamePos = false;
        g_haveMotionHeadRef = false;
        g_stereoHoldCounter = 0;
        // Drop the frame-pacing reference too. Keeping it meant the next FPV
        // entry measured the whole time spent outside FPV as one "frame".
        g_haveLastFrameQpc = false;
        g_lowFpsMonoActive = false;
    }

    // Was `if (!fpv) return;` -- the single gate that kept every VR camera
    // behaviour first-person-only. Third-person VR is the same machinery from
    // here down, so this widens rather than duplicates. Note the reference
    // capture below has to run for BOTH modes even when third_person_rotation
    // is off, because the POSITION hook depends on g_haveReference -- so the
    // rotation-strength knob is applied to the deltas further down, not here.
    if (!vrCam) return;
    if (!g_havePose.load(std::memory_order_relaxed)) return;

    // ONE consistent head sample for everything this frame does with it: the
    // camera write below AND the frame view record that tells the compositor
    // which pose these pixels came from. Reading the three atomics separately
    // could mix two XR frames' worth of pose; the snapshot cannot.
    HeadViewSnap headSnap;
    AcquireSRWLockShared(&g_headViewLock);
    headSnap = g_headViewSnap;
    ReleaseSRWLockShared(&g_headViewLock);
    const float hYaw = headSnap.have ? headSnap.yaw : g_headYaw.load(std::memory_order_relaxed);
    const float hPitch = headSnap.have ? headSnap.pitch : g_headPitch.load(std::memory_order_relaxed);
    const float hRoll = headSnap.have ? headSnap.roll : g_headRoll.load(std::memory_order_relaxed);

    const uintptr_t aPitch = g_moduleBase + kPitchOffset;
    const uintptr_t aYaw = g_moduleBase + kYawOffset;
    const uintptr_t aRoll = g_moduleBase + kRollOffset;

    // --- (re)capture the reference ------------------------------------------
    // Pins "where the head is now" to "where the game camera is now", then
    // applies head DELTAS from there. Removes any need to know the engine's
    // neutral value or zero direction, and gives recentring for free.
    if (!g_haveReference || g_recenterRequested.exchange(false, std::memory_order_relaxed)) {
        int16_t cp = 0, cy = 0, cr = 0;
        if (!SafeRead16(aPitch, &cp) || !SafeRead16(aYaw, &cy) || !SafeRead16(aRoll, &cr)) return;
        g_refGamePitch = (int32_t)cp & 0x0FFF;
        g_refGameYaw = (int32_t)cy & 0x0FFF;
        g_refGameRoll = (int32_t)cr & 0x0FFF;
        g_refHeadYaw = hYaw;  g_refHeadPitch = hPitch;  g_refHeadRoll = hRoll;
        g_refHeadPosX = g_headPosX.load(std::memory_order_relaxed);
        g_refHeadPosY = g_headPosY.load(std::memory_order_relaxed);
        g_refHeadPosZ = g_headPosZ.load(std::memory_order_relaxed);
        g_haveReference = true;
        g_yawOffsetRad.store(0.0f, std::memory_order_relaxed);
        // Only ever valid in first person -- see the longer note at the other
        // store site below for why third person must not publish this.
        g_yawOffsetValid.store(fpv, std::memory_order_relaxed);

        DebugLogger::LogFormat(
            "Camera hook: reference captured. head(y=%.4f p=%.4f r=%.4f pos=%.3f,%.3f,%.3f) game(pitch=%d yaw=%d roll=%d)",
            hYaw, hPitch, hRoll, g_refHeadPosX, g_refHeadPosY, g_refHeadPosZ,
            g_refGamePitch, g_refGameYaw, g_refGameRoll);
        return;                              // let this frame keep the game's value
    }

    // --- head deltas, inverted in HEAD space (names mean what they say) -----
    float dYawRad = NormalizeRad(hYaw - g_refHeadYaw);
    float dPitchRad = NormalizeRad(hPitch - g_refHeadPitch);
    float dRollRad = NormalizeRad(hRoll - g_refHeadRoll);

    // --- third-person rotation strength -------------------------------------
    // In the diorama view you are watching a scene from a fixed camera, so
    // 1:1 head rotation is not automatically what you want: MGS1's scripted
    // cameras are already framed, and a full-strength turn walks the framing
    // off the subject. Scaling the deltas here (rather than gating the write)
    // keeps the reference capture, the position hook and the stereo path all
    // running normally at any strength, including 0.
    //
    // third_person_rotation=0 is exactly third_person_rotation_percent=0 --
    // head rotation contributes nothing while positional 6DOF and stereo
    // still do, which is a genuinely useful mode on its own: the scene gets
    // real depth and real parallax without the camera ever leaving the
    // framing the game chose.
    //
    // Cutscene VR takes the same treatment for a related but not identical
    // reason: a cutscene is DIRECTED. Every shot is composed, and turning your
    // head 1:1 walks the framing off whatever the scene wants you to be
    // looking at. cutscene_rotation_percent below 100 keeps the director's
    // framing dominant while still letting you look around inside the shot,
    // which is probably the version most people actually want. 100 is shipped
    // because it is the honest default to judge from.
    if (tpv) {
        const float strength = g_thirdPersonRotation
            ? ((float)g_thirdPersonRotationPercent / 100.0f) : 0.0f;
        dYawRad *= strength;
        dPitchRad *= strength;
        dRollRad *= strength;
    }
    else if (csv) {
        const float strength = g_cutsceneRotation
            ? ((float)g_cutsceneRotationPercent / 100.0f) : 0.0f;
        dYawRad *= strength;
        dPitchRad *= strength;
        dRollRad *= strength;
    }
    // The head orientation the picture we are about to request actually
    // corresponds to -- taken here, after the strength scaling and before the
    // axis inversions, so it is in the same head space as the XR pose. An axis
    // we do not write is rendered at the game's own angle, which by
    // construction is the reference head angle.
    const float eqYawRad = g_refHeadYaw + (g_writeYaw ? dYawRad : 0.0f);
    const float eqPitchRad = g_refHeadPitch + (g_writePitch ? dPitchRad : 0.0f);
    const float eqRollRad = g_refHeadRoll + (g_writeRoll ? dRollRad : 0.0f);

    if (g_invertHeadYaw)   dYawRad = -dYawRad;
    if (g_invertHeadPitch) dPitchRad = -dPitchRad;
    if (g_invertHeadRoll)  dRollRad = -dRollRad;

    // Publish for the movement rotation in vr_input.cpp.
    //
    // FIRST PERSON ONLY, and this matters as much as the write_player_heading
    // gate below. vr_input.cpp's RotateStickByHeadYaw() rotates the analog
    // stick vector by whatever this publishes, and its own FPV check
    // (g_lookOnRightStick && IsFpvActive()) does not fire in third person --
    // so publishing a valid offset here would rotate Snake's walking
    // direction by your head yaw while you look around the diorama. Same
    // class of bug as Snake spinning on the spot, arriving through a
    // different door.
    //
    // Invalidating rather than just not-storing is deliberate: a stale
    // offset left valid from the last time first person was active would keep
    // steering movement with a frozen angle.
    if (fpv) {
        g_yawOffsetRad.store(dYawRad, std::memory_order_relaxed);
        g_yawOffsetValid.store(true, std::memory_order_relaxed);
    }
    else {
        g_yawOffsetValid.store(false, std::memory_order_relaxed);
    }

    int32_t dYaw = RadToUnits(dYawRad);
    int32_t dPitch = RadToUnits(dPitchRad);
    const int32_t dRoll = RadToUnits(dRollRad);

    // Routing. Confirmed: head yaw -> 593FCA, head pitch -> 593FC8.
    // swap_yaw_pitch=0 sends them the other way, kept only as insurance.
    if (!g_swapYawPitch) { const int32_t t = dYaw; dYaw = dPitch; dPitch = t; }

    // ---- BASE ANGLE --------------------------------------------------------
    // ADDITIVE (default): read the angle the game just wrote THIS frame and add
    // the head delta on top. Anchoring to the angle captured at FPV entry --
    // what this used to do -- meant every yaw change the game made afterwards
    // (stick turning, scripted camera moves) was overwritten by our pinned
    // value. Symptom: the radar's FOV cone rotates with the stick but the view
    // does not follow, because the radar reads Snake's facing while the render
    // camera reads 593FCA, which we were holding fixed.
    //
    // Safe to do here, unlike position: at 0xE1AE7 the game does
    // `mov ds:0x993fc8,eax` straight from the camera actor -- a direct copy
    // with no read-back -- so our addition cannot feed into its own input the
    // way it did through GV_NearExp4PV on the position path.
    int32_t baseYaw = g_refGameYaw;
    int32_t basePitch = g_refGamePitch;
    int32_t baseRoll = g_refGameRoll;
    if (g_additiveRotation) {
        int16_t ly = 0, lp = 0, lr = 0;
        if (SafeRead16(aYaw, &ly) && SafeRead16(aPitch, &lp) && SafeRead16(aRoll, &lr)) {
            baseYaw = (int32_t)ly & 0x0FFF;
            basePitch = (int32_t)lp & 0x0FFF;
            baseRoll = (int32_t)lr & 0x0FFF;
        }
    }

    const int32_t yawUnits = WrapUnits(baseYaw + dYaw);
    const int32_t absorbAccumForRecord = g_absorbAccum;

    // ---- BODY FOLLOWS HEAD ---------------------------------------------------
    // Hand the head's yaw offset to Snake's body: add it to his facing (rot and
    // turn together, so a stick turn already in progress is preserved), and
    // move the head reference by the same amount. The camera written below is
    // unchanged this frame (base + d), and from next frame the game's own base
    // IS base + d with d back near zero -- the view never moves, the body does.
    // Guard: only when the actor's facing is exactly the camera base we just
    // read, i.e. the pointer and field are what we think they are.
    if (g_bodyFollowsHead != 0 && fpv && g_writeYaw && g_swapYawPitch && g_additiveRotation && dYaw != 0 &&
        (g_bodyFollowsHead == 2 || g_moveStickActive.load(std::memory_order_relaxed))) {
        const uint32_t actor = g_playerActor.load(std::memory_order_relaxed);
        int16_t rotY = 0, turnY = 0;
        if (actor && SafeRead16((uintptr_t)actor + 0x2A, &rotY) && SafeRead16((uintptr_t)actor + 0x6E, &turnY)) {
            if (((int32_t)rotY & 0x0FFF) == baseYaw) {
                SafeWrite16((uintptr_t)actor + 0x2A, (int16_t)(((int32_t)rotY & ~0x0FFF) | WrapUnits((int32_t)rotY + dYaw)));
                SafeWrite16((uintptr_t)actor + 0x6E, (int16_t)(((int32_t)turnY & ~0x0FFF) | WrapUnits((int32_t)turnY + dYaw)));
                g_refHeadYaw = hYaw;
                g_absorbAccum = WrapUnits(g_absorbAccum + dYaw);
                static int bodyLog = 0;
                if (bodyLog < 5 || (bodyLog % 600) == 0) {
                    DebugLogger::LogFormat("Body follows head[%d]: Snake facing %d -> %d (head offset %d units handed to the body)",
                        bodyLog, (int)((int32_t)rotY & 0x0FFF), WrapUnits((int32_t)rotY + dYaw), dYaw);
                }
                bodyLog++;
            }
            else {
                static int mismatchLog = 0;
                if (mismatchLog < 5) {
                    mismatchLog++;
                    DebugLogger::LogFormat("Body follows head: skipped -- actor %08X facing %d != camera base %d "
                        "(not the actor the camera was built from this frame)", actor, (int)((int32_t)rotY & 0x0FFF), baseYaw);
                }
            }
        }
    }
    const int32_t pitchUnits = WrapUnits(basePitch + dPitch);
    const int32_t rollUnits = WrapUnits(baseRoll + dRoll);

    // Keep the game's heading frame in step with the camera we just wrote, so
    // its idea of "forward" is where you are actually looking rather than where
    // the camera happened to point when FPV was entered.
    //
    // HARD-GATED ON FPV, deliberately, and NOT on vrCam. Outside first person
    // this global is Snake's own movement/facing frame, so feeding head yaw
    // into it makes him spin on the spot as you look around the diorama --
    // the single most predictable way third-person VR could go wrong, called
    // out in the handoff's risk list before any of this was written. Not left
    // to the ini because there is no configuration in which it is correct.
    if (g_writePlayerHeading && g_writeYaw && fpv) {
        const int32_t heading = WrapUnits(yawUnits + g_playerHeadingOffsetUnits);
        SafeWrite32(g_moduleBase + kPlayerHeadingRva, (uint32_t)heading);
        static int headLog = 0;
        if (headLog < 3) {
            headLog++;
            DebugLogger::LogFormat("Player heading: wrote %d (camera yaw %d + %d)",
                heading, yawUnits, g_playerHeadingOffsetUnits);
        }
    }

    if (g_writeYaw)   SafeWrite16(aYaw, (int16_t)yawUnits);
    if (g_writePitch) SafeWrite16(aPitch, (int16_t)pitchUnits);
    if (g_writeRoll)  SafeWrite16(aRoll, (int16_t)rollUnits);

    // ---- FRAME VIEW RECORD --------------------------------------------------
    // "The pixels this frame produces were drawn from THIS head pose." Picked
    // up by the flip that presents them (ConsumeFrameViewRecord) and carried to
    // the XR thread with the RGBA, where it becomes the pose the layer is
    // submitted with. Only with the normal yaw->593FCA routing; the legacy
    // swapped routing has no well-defined head equivalent, so it records
    // nothing and the submission falls back to the old behaviour.
    // Gameplay Snake's eyes owns the record while it is drawing: the image is
    // built from the head joint at the view-build, not from rotate2.
    if (g_frameRecordEnabled && headSnap.have && g_swapYawPitch &&
        GetTickCount64() - g_gpPovTick.load(std::memory_order_relaxed) > 60) {
        FrameViewRecord r;
        r.valid = true;
        r.eye[0] = headSnap.eye[0];
        r.eye[1] = headSnap.eye[1];
        r.yawEq = eqYawRad;
        r.pitchEq = eqPitchRad;
        r.rollEq = eqRollRad;
        r.baseYaw = WrapUnits(baseYaw - absorbAccumForRecord);   // continuous across body-follows-head
        r.basePitch = basePitch;
        r.kYaw = g_invertHeadYaw ? -1 : 1;
        r.kPitch = g_invertHeadPitch ? -1 : 1;
        r.clip = 0;                        // filled at flip, from what was really used
        r.tick = GetTickCount64();
        { LARGE_INTEGER q; QueryPerformanceCounter(&q); r.qpc = q.QuadPart; }
        AcquireSRWLockExclusive(&g_frameRecLock);
        g_frameRec = r;
        g_frameRecRun = g_rotHookRuns.load(std::memory_order_relaxed);
        g_lastValidRec = r;
        ReleaseSRWLockExclusive(&g_frameRecLock);

        static int recLog = 0;
        if (recLog < 3 || (recLog % 1800) == 0) {
            DebugLogger::LogFormat(
                "Frame view record[%d]: image = head yaw %.1f pitch %.1f roll %.1f deg "
                "(game base yaw %d pitch %d) -- the pose this frame's layer is submitted with",
                recLog, eqYawRad * 57.2958f, eqPitchRad * 57.2958f, eqRollRad * 57.2958f,
                baseYaw, basePitch);
        }
        recLog++;
    }

    // ---- FIELD OF VIEW -----------------------------------------------------
    // Read first, always, whether or not we write. The first few reads are the
    // whole experiment: if this field is clip_distance it will say 320.
    {
        const uintptr_t aFov = g_moduleBase + kFovClipDistanceRva;
        int16_t fovNow = 0;
        const bool gotFov = SafeRead16(aFov, &fovNow);

        static int fovObserveLog = 0;
        if (gotFov && fovObserveLog < 5) {
            fovObserveLog++;
            const double hfov = (fovNow > 0)
                ? 2.0 * std::atan(160.0 / (double)fovNow) * 180.0 / 3.14159265358979323846
                : 0.0;
            DebugLogger::LogFormat(
                "FOV observed: mgsi.exe+%X reads %d%s -- implied hfov %.1f deg",
                (unsigned)kFovClipDistanceRva, (int)fovNow,
                (fovNow == 320) ? "  <== 320, THIS IS clip_distance" : "",
                hfov);
        }

        // Write every frame, from inside the game's own frame, for the same
        // reason the rotation write lives here: anything we poke from outside
        // gets overwritten by the engine's next camera update. Per-camera and
        // script-overridable means one write is never enough.
        // third_person_fov extends the FOV write into the diorama view. Worth
        // having as its own switch rather than folding into
        // fov_first_person_only: the handoff's point that "a diorama seen
        // through a 53 deg letterbox is a screen; the same diorama at 90 deg
        // is a room" argues FOR widening here, while the standing warning
        // that a wide lens on fixed third-person cameras mostly reveals level
        // seams argues against. Ships off; try it once the mode itself works.

        // KEEP THE STOCK VALUE. Anything that is not our own value is one the
        // game set, and native mode needs it to hand the lens back. Guarded on
        // gotFov so a failed read never poisons the saved value with a zero.
        // While the scope is up the field holds OUR zoom, never the stock lens.
        const bool scoped = g_scopeEnabled && g_scopeActive.load(std::memory_order_relaxed) && g_lastFpvState;
        if (gotFov && g_fovClipDistance != 0 && fovNow != (int16_t)g_fovClipDistance &&
            fovNow != (int16_t)g_scopeClip) {
            g_fovOriginal = fovNow;
            g_haveFovOriginal = true;
        }

        if (scoped) {
            // PSG1 at your eye: write the scope's zoom instead of the wide
            // lens. rifle.c's own zoom (+63FBAA, add a third per frame while
            // the button is held) is simply overwritten -- the zoom is ours,
            // and it is there as soon as the rifle is at your eye.
            SafeWrite16(aFov, (int16_t)g_scopeClip);
            static int scopeLog = 0;
            if (scopeLog < 6) {
                scopeLog++;
                DebugLogger::LogFormat("Scope: wrote clip_distance=%d (%.1f deg drawn, shown across %.0f deg)",
                    g_scopeClip, 2.0 * std::atan(160.0 / (double)g_scopeClip) * 180.0 / 3.14159265358979323846,
                    g_scopeDisplayDeg);
            }
        }
        else if (g_fovClipDistance != 0 &&
            (!g_fovFirstPersonOnly || g_lastFpvState || (tpv && g_thirdPersonFov)
             || (csv && g_cutsceneFov))) {
            SafeWrite16(aFov, (int16_t)g_fovClipDistance);
            static int fovWriteLog = 0;
            if (fovWriteLog < 3) {
                fovWriteLog++;
                DebugLogger::LogFormat("FOV: wrote clip_distance=%d (was %d)",
                    g_fovClipDistance, (int)fovNow);
            }
        }
    }

    static int rotLog = 0;
    if (rotLog < g_logFrames) {
        DebugLogger::TestLogFormat(
            "Rot[%d]: d(yaw=%d pitch=%d) base(yaw=%d pitch=%d) -> wrote(yaw=%d pitch=%d)",
            rotLog, dYaw, dPitch, baseYaw, basePitch, yawUnits, pitchUnits);
    }
    rotLog++;
}

// ===========================================================================
// POSITION hook body. Runs later in the same frame, after the game has
// finalised camera position, so our offset is the last word.
//
// Unlike rotation this is ADDITIVE ON TOP OF THE LIVE VALUE rather than
// anchored to a reference -- the game legitimately moves the camera as Snake
// moves, and we only want to add the head's offset from its reference.
//
// Because MGS1 rasterises the whole scene in software from this camera
// position, moving it produces REAL geometric parallax: leaning past a
// doorframe genuinely reveals what is behind it. This is not reprojection.
// ===========================================================================
extern "C" void __cdecl Mgs1CameraPosWriteBody(uint32_t actorPtr) {
    // --- THE PLAYER ACTOR, TAKEN RATHER THAN DERIVED -----------------------
    // FIRST, before every gate. This is not part of the position write; it is
    // the one place in the process where the player object identifies itself,
    // and it must not be silenced by native mode, by position_tracking=0, or by
    // anything else that could itself be the thing under test.
    //
    // At +E1B58 the game is mid-way through building the first-person camera
    // out of an actor it was handed. `esi` IS that actor -- `ebx` was set to
    // `esi+0x20` at +E1A74 and esi is callee-saved, so nothing has touched it.
    // Reading it here is not an inference about which global holds the player;
    // it is the pointer the engine is using, at the moment it is using it.
    //
    // This replaces the 2026-08-24 derivation through the global at +334228,
    // which the Snake[] probe refuted: distTO showed the object 13000-17000
    // units from the shot's own look-at target, its position never travelled
    // more than ~200 units, and [obj+0x9C] read back as 0xF38A06C4 -- not a
    // pointer at all. That global holds SOMETHING the constructor at +1E1701
    // made; it is not the actor this routine is handed.
    NotePlayerActor(actorPtr);
    // Snake's model definition, so the cutscene can recognise him in any
    // outfit: a demo model built from the same cached .kmd shares this DG_DEF
    // pointer (GV_GetCache hands back the one loaded copy). [actor+0x9C] is
    // the OBJECT's DG_OBJS*, [objs+0x24] its DG_DEF*.
    if (actorPtr >= 0x00400000u && actorPtr < 0x7FFF0000u) {
        int32_t objs = 0, def = 0;
        if (SafeRead32((uintptr_t)actorPtr + 0x9C, &objs) &&
            (uint32_t)objs >= 0x00400000u && (uint32_t)objs < 0x7FFF0000u &&
            SafeRead32((uintptr_t)(uint32_t)objs + 0x24, &def) &&
            (uint32_t)def >= 0x00400000u && (uint32_t)def < 0x7FFF0000u) {
            g_playerModelDef.store((uint32_t)def, std::memory_order_relaxed);
        }
    }
    // Also before every gate. The sweep only means anything while this routine
    // is running, which is exactly when the mod might otherwise be switched off.
    if (actorPtr >= 0x00400000u && actorPtr < 0x7FFF0000u) ScanActorFields(actorPtr);
    static uint32_t s_lastLoggedActor = 0;
    {
        const uint32_t now = g_playerActor.load(std::memory_order_relaxed);
        if (now != s_lastLoggedActor) {
            s_lastLoggedActor = now;
            DebugLogger::LogFormat(
                "Player actor CAPTURED: %08X (esi at mgsi.exe+E1B58, raw arg %08X). This is the "
                "object the game's own first-person camera routine is using this frame, not a "
                "guess about which global holds it. Snake POV in cutscenes reads position from "
                "[actor+0x20/+0x22/+0x24] through this pointer. A NEW value here on a room change "
                "is expected; a value that changes every frame would mean esi is not what we think.",
                now, actorPtr);
        }
    }

    // Loud about why it does nothing. Leaning has never been observed working,
    // and silence here is indistinguishable from the detour never firing, so
    // the first few skips now say which gate closed.
    static int posBail = 0;
    // Stereo needs this hook even when positional tracking is switched off --
    // the eye offset is applied through the same write.
    if (!g_positionEnabled && !g_stereoAlternate) {
        if (posBail < 5) { DebugLogger::LogFormat("Pos hook: skipped -- position_tracking=0 in the ini"); posBail++; }
        return;
    }
    if (!g_enabled.load(std::memory_order_relaxed)) {
        if (posBail < 5) { DebugLogger::LogFormat("Pos hook: skipped -- camera hook disabled"); posBail++; }
        return;
    }
    // VIEW MODE, and it has to be checked HERE rather than relied on through
    // the flags below. The gate further down passes on `g_lastFpvState` alone,
    // and that flag is a fact about MGS1, not about the mod: switching to
    // native while the game happens to be in first person leaves it true. The
    // rotation hook, meanwhile, has already returned before capturing a
    // reference -- so g_haveReference is still true from the last VR frame and
    // does not stop anything either. Without this line, native mode would keep
    // writing a 6DOF position offset into a game it is supposed to be leaving
    // completely alone, which is the exact promise the mode makes.
    if (!g_vrViewMode.load(std::memory_order_relaxed)) {
        if (posBail < 5) { DebugLogger::LogFormat("Pos hook: skipped -- native view mode"); posBail++; }
        return;
    }
    if (!g_havePose.load(std::memory_order_relaxed)) {
        if (posBail < 5) { DebugLogger::LogFormat("Pos hook: skipped -- no head pose published yet"); posBail++; }
        return;
    }
    if (!g_haveReference) {
        if (posBail < 5) { DebugLogger::LogFormat("Pos hook: skipped -- no reference pose (not in FPV?)"); posBail++; }
        return;
    }
    // Debounced state, set by the rotation hook earlier in this same frame --
    // as is g_tpvActive, which is why this can trust both without re-reading
    // anything. Third-person VR gets the same positional 6DOF and the same
    // per-eye stereo offset through this hook; the room-cut guard has already
    // cleared g_tpvActive for the duration of a cut, so no offset is applied
    // across a viewpoint teleport.
    if (!g_lastFpvState &&
        !(g_tpvActive && (g_thirdPersonPosition || g_thirdPersonStereo)) &&
        !(g_csvInFrame && (g_cutscenePosition || g_cutsceneStereo))) {
        if (posBail < 5) {
            DebugLogger::LogFormat("Pos hook: skipped -- not in first person, and neither third-person "
                "VR nor cutscene VR is driving");
            posBail++;
        }
        return;
    }

    // Head offset from reference, in metres, OpenXR play space.
    float dx = 0.0f, dy = 0.0f, dz = 0.0f;
    if (g_positionEnabled &&
        (!g_tpvActive || g_thirdPersonPosition) &&
        (!g_csvInFrame || g_cutscenePosition)) {
        dx = g_headPosX.load(std::memory_order_relaxed) - g_refHeadPosX;
        dy = g_headPosY.load(std::memory_order_relaxed) - g_refHeadPosY;
        dz = g_headPosZ.load(std::memory_order_relaxed) - g_refHeadPosZ;
    }

    // ---- eye offset --------------------------------------------------------
    // Half an IPD sideways, in HEAD space, so it goes through the same view
    // rotation as leaning does and therefore stays lateral to wherever you are
    // actually looking. Applied before the axis inverts on purpose: if head X
    // is mirrored into the game's world, the eyes have to mirror with it or the
    // stereo comes out swapped.
    // position_frame=2 adds its own eye offset along HEAD-right below. Adding
    // this one as well put the eyes ~2x IPD apart with a fore/aft skew that
    // depended on head yaw (fixed 2026-09-23: the leftover gun/near double image).
    if (g_stereoAlternate && g_positionFrame != 2 && !g_scopeActive.load(std::memory_order_relaxed)) {
        const int eye = g_publishedEye.load(std::memory_order_relaxed);
        if (eye == 0 || eye == 1) {
            float half = (float)g_stereoIpdMm / 2000.0f;   // mm -> metres, halved
            if (g_stereoSwapEyes) half = -half;
            dx += (eye == 0) ? -half : half;
        }
        // eye == 2 is the motion hold: no offset, both eyes take this frame.
    }

    // ---- POSITION FRAME 2: the game camera's own basis ---------------------
    // Derived from the binary, not tuned. DG_LookAt (mgsi.exe+1C22) builds the
    // view as rows r, u, f with
    //     f = TO - FROM,  r = (0,-4096,0) x f = (-fz, 0, fx),  u = f x r
    // so for a camera yaw Y (the engine gets yaw back as ratan2(x, z) at +4546D9):
    //     forward = ( sin Y, 0, cos Y)     screen right = (-cos Y, 0, sin Y)
    // and u = f x r comes out as (0,-1,0) for a level camera: screen-DOWN is
    // world -Y, i.e. the world is Y-UP. (Corroborated: the first-person camera
    // sits at y = +1497 over a floor at 0 -- a 1.5 m eye at 1000 units/metre --
    // and lying down RAISES the eye by 0x140 at +E1AC5, which only makes sense
    // with Y up.)
    //
    // What the legacy path got wrong, worked through with those vectors: it
    // added the half-IPD to PLAY-SPACE X, then rotated by the camera yaw with
    // a mirror and a 180. A pure "right" offset came out as (-cos Y, 0, -sin Y)
    // against the true (-cos Y, 0, sin Y): correct facing +/-Z, eyes SWAPPED
    // facing +/-X, and at 45 degrees the two eyes sat one in front of the
    // other. That heading-dependent mess is the "crossing your eyes" double
    // image on anything near -- the gun included -- and the same transform is
    // why leaning never felt right. invert_position_y=1 on a Y-up world also
    // meant raising your head lowered the camera.
    //
    // Frame 2: take the head offset in HEAD-LOCAL terms (right / up / back,
    // from the current head yaw), add the eye offset along head-right, and lay
    // that onto the camera's right / up / forward. No flags to get wrong.
    if (g_positionFrame == 2 && g_positionTestUnits == 0) {
        float lx = 0.0f, ly = dy, lz = 0.0f;
        {
            const float h = g_headYaw.load(std::memory_order_relaxed);
            const float ch = std::cos(h), sh = std::sin(h);
            // OpenXR yaw about +Y: head-right = (cos h, 0, -sin h), head-back = (sin h, 0, cos h)
            lx = dx * ch - dz * sh;
            lz = dx * sh + dz * ch;
        }
        const float lxLean = lx;   // head-centre (lean only), for motion aim's gun anchor
        if (g_stereoAlternate && !g_scopeActive.load(std::memory_order_relaxed)) {
            const int eye = g_publishedEye.load(std::memory_order_relaxed);
            if (eye == 0 || eye == 1) {
                float half = (float)g_stereoIpdMm / 2000.0f;
                if (g_stereoSwapEyes) half = -half;
                lx += (eye == 0) ? -half : half;
            }
        }
        int16_t yawRaw = 0;
        if (!SafeRead16(g_moduleBase + kYawOffset, &yawRaw)) return;
        const float Y = ((float)((int32_t)yawRaw & 0x0FFF) / kUnitsPerTurn) * kTwoPi;
        const float cY = std::cos(Y), sY = std::sin(Y);
        // game = right*lx + forward*(-lz) + up*ly
        const float gxm = -cY * lx + sY * (-lz);
        const float gzm =  sY * lx + cY * (-lz);
        const float gym = ly;
        const int32_t lim = g_maxPositionOffset;
        const int32_t ox = Clamp((int32_t)std::lround(gxm * g_positionScale), -lim, lim);
        const int32_t oy = Clamp((int32_t)std::lround(gym * g_positionScale), -lim, lim);
        const int32_t oz = Clamp((int32_t)std::lround(gzm * g_positionScale), -lim, lim);

        int16_t gx = 0, gy = 0, gz = 0;
        if (!SafeRead16(g_moduleBase + kPosXOffset, &gx) ||
            !SafeRead16(g_moduleBase + kPosYOffset, &gy) ||
            !SafeRead16(g_moduleBase + kPosZOffset, &gz)) return;
        SafeWrite16(g_moduleBase + kPosXOffset, (int16_t)((int32_t)gx + ox));
        SafeWrite16(g_moduleBase + kPosYOffset, (int16_t)((int32_t)gy + oy));
        SafeWrite16(g_moduleBase + kPosZOffset, (int16_t)((int32_t)gz + oz));
        g_prevPosOffX = ox; g_prevPosOffY = oy; g_prevPosOffZ = oz;
        g_havePrevPosOffset = true;
        {
            // Eye part of the offset = total - lean-only, in the same rounded
            // units that were written, so motion aim can recover the head centre
            // exactly from whichever eye's camera the gun is being built against.
            const float gxl = -cY * lxLean + sY * (-lz);
            const float gzl =  sY * lxLean + cY * (-lz);
            const int32_t lxo = Clamp((int32_t)std::lround(gxl * g_positionScale), -lim, lim);
            const int32_t lzo = Clamp((int32_t)std::lround(gzl * g_positionScale), -lim, lim);
            const int32_t cam[3] = { (int32_t)(int16_t)((int32_t)gx + ox),
                                     (int32_t)(int16_t)((int32_t)gy + oy),
                                     (int32_t)(int16_t)((int32_t)gz + oz) };
            const int32_t eyeOff[3] = { ox - lxo, 0, oz - lzo };
            RecordCameraEyeOffset(cam, eyeOff);
        }

        static int posLog2 = 0;
        if (posLog2 < g_logFrames || (posLog2 % 120) == 0) {
            DebugLogger::TestLogFormat(
                "PosCam[%d]: eye=%d yaw=%d | head-local right=%.3f up=%.3f back=%.3f m -> offset(%d,%d,%d) "
                "| game was(%d,%d,%d)",
                posLog2, g_publishedEye.load(std::memory_order_relaxed), (int)((int32_t)yawRaw & 0x0FFF),
                lx, ly, lz, ox, oy, oz, (int)gx, (int)gy, (int)gz);
        }
        posLog2++;
        return;
    }

    // Diagnostic override: forget the head, just rock the camera side to side.
    static int oscFrame = 0;
    bool oscillating = false;
    if (g_positionTestUnits != 0) {
        oscillating = true;
        oscFrame++;
        dx = ((oscFrame / 60) % 2 == 0) ? 1.0f : -1.0f;
        dy = 0.0f;
        dz = 0.0f;
    }

    // --- Axis inversion, in HEAD space, BEFORE the view rotation ------------
    // The ordering matters and it was wrong. These used to be applied to the
    // final world-space offset, which meant invert_position_x negated the
    // world X component of an already-rotated vector: it only behaved like
    // "invert left/right" while you happened to be facing along the world X
    // axis, and silently became "invert forward/back" once you turned 90
    // degrees. Same class of footgun as the rotation inverts before they were
    // moved into head space. Applied here, the names mean what they say
    // regardless of which way you are facing.
    const float headDx = dx, headDz = dz;   // kept for the log
    if (g_invertPosX) dx = -dx;
    if (g_invertPosY) dy = -dy;          // PSX Y is normally DOWN
    if (g_invertPosZ) dz = -dz;

    // Rotate the horizontal offset into the game's world frame using the live
    // camera yaw, so leaning is relative to where you are looking rather than
    // to a fixed world axis.
    if (g_rotatePositionWithView) {
        int16_t yawRaw = 0;
        if (SafeRead16(g_moduleBase + kYawOffset, &yawRaw)) {
            float camYaw = ((float)((int32_t)yawRaw & 0x0FFF) / kUnitsPerTurn) * kTwoPi
                + g_positionYawOffsetRad;

            // --- HANDEDNESS, and why this is not just another invert flag ---
            //
            // Settled the hard way on 2026-08-15, after three headset sessions
            // spent flipping invert_position_x/z and getting three different
            // flavours of wrong. The measured Pos[] lines finally made the
            // logic decidable:
            //
            //   x=0 z=0 (pure rotation)        -> REVERSED, clearly
            //   x=1 z=1 (= pure rotation +180) -> not reversed, but still off
            //
            // Those two are the ONLY rigid rotations reachable from the old
            // ini, because negating both horizontal axes IS a 180 degree yaw
            // and negating one is a mirror. Both rotations being wrong is not
            // a tuning failure -- it is a proof. If no rotation works, the
            // transform we need is not a rotation: the XR play space and the
            // game's world axes are OPPOSITE-HANDED, and converting between
            // them needs a reflection.
            //
            // A reflection cannot be expressed as an axis flip on its own.
            // Negating one axis while leaving the rotation direction alone is
            // exactly the heading-dependent mirror that produced the original
            // "correct facing one way, wrong facing another, difficult to
            // describe" report. The mirror and the rotation direction have to
            // flip TOGETHER, which is what this does: mirror Z, and run the
            // yaw backwards.
            //
            // With this flag, the four candidate frames are finally all
            // reachable, and exactly one of them is correct:
            //   flip=0 offset=0    pure rotation                 (tested: reversed)
            //   flip=0 offset=180  rotation + 180  (= old x=1,z=1) (tested: off)
            //   flip=1 offset=0    reflected                     <-- try first
            //   flip=1 offset=180  reflected + 180               <-- then this
            if (g_positionHandednessFlip) {
                dz = -dz;
                camYaw = -camYaw;
            }

            const float c = std::cos(camYaw), s = std::sin(camYaw);
            const float rx = dx * c + dz * s;
            const float rz = -dx * s + dz * c;
            dx = rx; dz = rz;
        }
    }

    const float scale = oscillating ? (float)g_positionTestUnits : g_positionScale;
    int32_t ox = (int32_t)(dx * scale);
    int32_t oy = (int32_t)(dy * scale);
    int32_t oz = (int32_t)(dz * scale);

    // The oscillation test deliberately bypasses the clamp -- its whole job is
    // to be unmistakable.
    if (!oscillating) {
        const int32_t lim = g_maxPositionOffset;
        ox = Clamp(ox, -lim, lim);
        oy = Clamp(oy, -lim, lim);
        oz = Clamp(oz, -lim, lim);
    }

    int16_t gx = 0, gy = 0, gz = 0;
    if (!SafeRead16(g_moduleBase + kPosXOffset, &gx) ||
        !SafeRead16(g_moduleBase + kPosYOffset, &gy) ||
        !SafeRead16(g_moduleBase + kPosZOffset, &gz)) return;

    SafeWrite16(g_moduleBase + kPosXOffset, (int16_t)((int32_t)gx + ox));
    SafeWrite16(g_moduleBase + kPosYOffset, (int16_t)((int32_t)gy + oy));
    SafeWrite16(g_moduleBase + kPosZOffset, (int16_t)((int32_t)gz + oz));

    // Remember exactly what we added so the rotation hook can remove it at
    // the top of the next frame, before the game interpolates from it.
    g_prevPosOffX = ox; g_prevPosOffY = oy; g_prevPosOffZ = oz;
    g_havePrevPosOffset = true;

    // Log the opening burst, then keep a slow heartbeat going so the numbers
    // are still there minutes into a session when leaning is actually tried.
    static int posLog = 0;
    if (posLog < g_logFrames || (posLog % 120) == 0) {
        DebugLogger::TestLogFormat(
            "Pos[%d]%s: headRaw(x=%.3f z=%.3f m) -> world(x=%.3f z=%.3f) -> offset(%d,%d,%d) | game was(%d,%d,%d) -> now(%d,%d,%d)",
            posLog, oscillating ? " OSC" : "", headDx, headDz, dx, dz, ox, oy, oz,
            (int)gx, (int)gy, (int)gz,
            (int)(int16_t)((int32_t)gx + ox), (int)(int16_t)((int32_t)gy + oy),
            (int)(int16_t)((int32_t)gz + oz));
    }
    posLog++;
}

// ===========================================================================
// VIEW COMMIT BODY -- layer 3, at RVA 0x53C47.
//
// Runs once per rendered 3D frame, on the game's thread, after the game has
// finished deciding where its camera is and immediately before that decision
// is turned into a view matrix. Unlike the two hooks above it, this one is
// reached during cutscenes.
//
// It is ADDITIVE on the game's own committed direction: read FROM and TO,
// measure the direction the shot is pointing, rotate that direction by how
// far the head has turned since the reference pose, write TO back. The
// director's framing is preserved and you look around inside it. A shot
// change moves FROM and TO together, so it is followed for free with no
// re-anchoring logic at all -- the thing PATH A needed 200 units of
// hysteresis to approximate.
// ===========================================================================
namespace {

struct ViewPoint { int32_t x, y, z; };

// ABSOLUTE addresses, not RVAs -- the view-build hook receives pointers the
// live camera path chose, which are not always the layer-3 globals.
bool ReadViewPoint(uintptr_t addr, ViewPoint* out) {
    int16_t x = 0, y = 0, z = 0;
    if (!SafeRead16(addr + 0, &x)) return false;
    if (!SafeRead16(addr + 2, &y)) return false;
    if (!SafeRead16(addr + 4, &z)) return false;
    out->x = x; out->y = y; out->z = z;
    return true;
}

bool WriteViewPoint(uintptr_t addr, const ViewPoint& p) {
    const bool a = SafeWrite16(addr + 0, (int16_t)Clamp(p.x, -32768, 32767));
    const bool b = SafeWrite16(addr + 2, (int16_t)Clamp(p.y, -32768, 32767));
    const bool c = SafeWrite16(addr + 4, (int16_t)Clamp(p.z, -32768, 32767));
    return a && b && c;
}

// True while this hook owns the cutscene camera. When it does, the older
// cutscene paths must stand down: layer 1's head delta flows into layer 3
// through the copy at 0x53D4B, so applying the delta again here would double
// every head movement. One writer per value per frame, expressed across two
// layers instead of one.

// --- Snake's head, read the same way the game reads it ---------------------
// Everything here is guarded: the player object does not exist in menus or
// between rooms, and the model pointer is null for a few frames after a load.
// A failure is reported, never assumed away -- a POV camera silently falling
// back to (0,0,0) would look exactly like the hook not running, and this
// project has already spent three builds on that ambiguity.
// --- the engine's own object list ------------------------------------------
//
// Every game object is a node in an intrusive doubly-linked list, and the list
// heads live in one table. From the allocator:
//
//   +1E1701  push 0xA74 / push 5 / call 0x40A30C      the player: class 5
//   0x40A30C -> 0x40B296 (malloc) -> 0x40A2AF (link)
//   0x40A2AF  imul eax,[class],0x44 / add eax,0x6BFC98   <-- the head table
//             [head] = obj ; obj->[+4] = head ; obj->[+0] = old head
//
// So the list head for class N is 0x6BFC98 + N*0x44, node +0x0 is next and
// +0x4 is prev, and class 5 is the class the player is allocated with (211 of
// the 468 allocation sites in the binary use it -- it is the game-object
// class). Walking it needs no pointer we had to guess.
//
// This is what the field sweep should have been. Rather than asking "which
// field of THIS object holds a position", it asks "which OBJECT is Snake" --
// and during a cutscene that is the real question, because the demo may well
// be animating a different object than the one the first-person camera was
// built from before the scene started.
constexpr uintptr_t kEntityHeadTableRva = 0x2BFC98;  // 0x6BFC98 absolute
constexpr unsigned  kEntityHeadStride = 0x44;
constexpr uintptr_t kEntityNextOffset = 0x00;
constexpr uintptr_t kEntityStateOffset = 0x9A8;      // the +DC89A accessor's field



// Sweeps the demo heap for int16 triples near the shot's look-at point.
// Reads in blocks and scans the copy: one SafeReadBytes per 4 KB instead of
// one guarded read per candidate, which is what makes a 256 KB sweep cheap
// enough to run inside the game's own frame.
void DemoScanNearTarget(const ViewPoint& to, unsigned long long frame) {
    if (!g_demoScan) return;
    static int s_lines = 0;
    if (s_lines >= g_demoScanLines) return;
    const unsigned every = (unsigned)(g_demoScanEvery < 1 ? 1 : g_demoScanEvery);
    if ((frame % every) != 0) return;
    const uint32_t st = g_demoState.load(std::memory_order_relaxed);
    if (st == 0) return;

    const uint32_t window = (uint32_t)(g_demoScanWindowKb < 4 ? 4 : g_demoScanWindowKb) * 1024u;
    uint32_t base = (st > window / 2u) ? (st - window / 2u) : 0u;
    base &= ~0xFFFu;

    struct Hit { uint32_t addr; int x, y, z, d, bits; };
    unsigned exact = 0;
    Hit best[16];
    int nbest = 0;
    const int wantHits = (g_demoScanHits < 1) ? 1 : (g_demoScanHits > 16 ? 16 : g_demoScanHits);
    const double radius = (double)(g_demoScanRadius < 1 ? 1 : g_demoScanRadius);

    static uint8_t buf[4096 + 8];
    unsigned blocks = 0, readable = 0;
    for (uint32_t off = 0; off + 4096 <= window; off += 4096) {
        const uint32_t blockAddr = base + off;
        ++blocks;
        if (!SafeReadBytes(buf, (const uint8_t*)(uintptr_t)blockAddr, 4096)) continue;
        ++readable;
        // Two passes: 16-bit triples (SVECTOR) and 32-bit triples (VECTOR).
        // The 32-bit pass is the one that matters -- see the comment above.
        for (int pass = 0; pass < (g_demoScanInt32 ? 2 : 1); ++pass) {
            const int step = pass ? 4 : 2;
            const int span = pass ? 12 : 6;
            for (int i = 0; i + span <= 4096; i += step) {
                int x, y, z;
                if (pass) {
                    x = (int)(uint32_t)(buf[i]     | (buf[i+1] << 8) | (buf[i+2]  << 16) | (buf[i+3]  << 24));
                    y = (int)(uint32_t)(buf[i + 4] | (buf[i+5] << 8) | (buf[i+6]  << 16) | (buf[i+7]  << 24));
                    z = (int)(uint32_t)(buf[i + 8] | (buf[i+9] << 8) | (buf[i+10] << 16) | (buf[i+11] << 24));
                } else {
                    x = (int)(int16_t)(buf[i]     | (buf[i + 1] << 8));
                    y = (int)(int16_t)(buf[i + 2] | (buf[i + 3] << 8));
                    z = (int)(int16_t)(buf[i + 4] | (buf[i + 5] << 8));
                }
                if (x == 0 && y == 0 && z == 0) continue;
                const double dx = (double)(to.x - x);
                const double dy = (double)(to.y - y);
                const double dz = (double)(to.z - z);
                const double d2 = dx * dx + dy * dy + dz * dz;
                if (d2 > radius * radius) continue;
                const int d = (int)sqrt(d2);
                // d == 0 is TO echoed back at us, not a body standing on it.
                if (g_demoScanSkipExact && d == 0) { ++exact; continue; }
                int slot = nbest;
                while (slot > 0 && best[slot - 1].d > d) { if (slot < wantHits) best[slot] = best[slot - 1]; --slot; }
                if (slot < wantHits) {
                    best[slot].addr = blockAddr + (uint32_t)i;
                    best[slot].x = x; best[slot].y = y; best[slot].z = z; best[slot].d = d;
                    best[slot].bits = pass ? 32 : 16;
                    if (nbest < wantHits) ++nbest;
                }
            }
        }
    }

    s_lines++;
    DebugLogger::TestLogFormat(
        "Demo[%llu]: chunk=%08X swept %08X..%08X (%u/%u blocks readable) TO=(%d,%d,%d) r=%d "
        "-> %d hit%s, %u exact copies of TO skipped",
        frame, st, base, base + window, readable, blocks,
        to.x, to.y, to.z, g_demoScanRadius, nbest, nbest == 1 ? "" : "s", exact);
    char line[900];
    int used = 0;
    line[0] = '\0';
    for (int i = 0; i < nbest; ++i) {
        const int n = _snprintf_s(line + used, sizeof(line) - used, _TRUNCATE,
            "%s%08X/%d=(%d,%d,%d)d=%d", used ? "  " : "",
            best[i].addr, best[i].bits, best[i].x, best[i].y, best[i].z, best[i].d);
        if (n > 0) used += n;
        if ((i + 1) % 4 == 0) {
            DebugLogger::TestLogFormat("  Demo: %s", line);
            used = 0; line[0] = '\0';
        }
    }
    if (used > 0) DebugLogger::TestLogFormat("  Demo: %s", line);
}

// Sweeps every committed private region for OBJECTS SHAPED LIKE ACTORS.
//
// The four-field signature and why each field is in it:
//   +0x9C   model pointer   -- must be a plausible 4-aligned heap/static
//                              address. This is the cheap first test, and it
//                              rejects almost everything on its own.
//   +0x20   int16 x,y,z     -- must be non-zero and within the radius of TO.
//                              +4022EC reads exactly these three shorts and
//                              hands them to the zone table, so they are a
//                              world point by the engine's own use of them.
//   +0x895  flags byte      -- reported always; only required when the ini
//                              says so, because a demo actor may legitimately
//                              carry zero there.
//   +0xA26  stance word     -- reported, never required: it is the field that
//                              decides the eye adjust, so it is worth seeing.
//
// Region walking: VirtualQuery from 0x00400000 up. MEM_COMMIT, not
// PAGE_NOACCESS/GUARD, not MEM_IMAGE (the actors are allocated, not linked).
// Each region is read in 64 KB chunks overlapping by one object span so a
// candidate straddling a chunk boundary is not lost, and the whole sweep stops
// at the byte budget so a frame can never be eaten by an enormous mapping.
void ScanActorSignatures(const ViewPoint& to, unsigned long long frame) {
    if (!g_actorSigScan) return;
    static int s_lines = 0;
    if (s_lines >= g_actorSigLines) return;
    const unsigned every = (unsigned)(g_actorSigEvery < 1 ? 1 : g_actorSigEvery);
    if ((frame % every) != 0) return;

    struct Hit { uint32_t addr, model; int x, y, z, d, flags, stance; };
    Hit best[16];
    int nbest = 0;
    const int wantHits = (g_actorSigHits < 1) ? 1 : (g_actorSigHits > 16 ? 16 : g_actorSigHits);
    const double radius = (double)(g_actorSigRadius < 1 ? 1 : g_actorSigRadius);

    // 0xA28 is the span the signature needs: the stance word ends there.
    const uint32_t kSpan = 0xA28u;
    static uint8_t chunk[65536 + 0xA28];
    const uint32_t kStep = 65536u;

    uint64_t budget = (uint64_t)(g_actorSigBudgetMb < 1 ? 1 : g_actorSigBudgetMb) * 1024u * 1024u;
    uint64_t swept = 0;
    unsigned regions = 0, examined = 0, shaped = 0;

    uintptr_t addr = 0x00400000u;
    MEMORY_BASIC_INFORMATION mbi;
    while (addr < 0x7FFF0000u && swept < budget) {
        if (VirtualQuery((LPCVOID)addr, &mbi, sizeof(mbi)) != sizeof(mbi)) break;
        const uintptr_t rb = (uintptr_t)mbi.BaseAddress;
        const uintptr_t re = rb + (uintptr_t)mbi.RegionSize;
        const DWORD prot = mbi.Protect;
        const bool readable =
            (mbi.State == MEM_COMMIT) &&
            !(prot & (PAGE_NOACCESS | PAGE_GUARD)) &&
            (prot & (PAGE_READWRITE | PAGE_READONLY | PAGE_WRITECOPY |
                     PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
        addr = re;
        if (!readable || mbi.Type == MEM_IMAGE) continue;
        if ((uintptr_t)mbi.RegionSize < kSpan) continue;
        ++regions;

        for (uintptr_t cur = rb; cur + kSpan <= re && swept < budget; cur += kStep) {
            uint32_t len = (uint32_t)((re - cur) > (uintptr_t)(kStep + kSpan)
                                      ? (kStep + kSpan) : (re - cur));
            if (!SafeReadBytes(chunk, (const uint8_t*)cur, len)) continue;
            swept += len;
            for (uint32_t i = 0; i + kSpan <= len; i += 4) {
                ++examined;
                const uint32_t model = (uint32_t)(chunk[i + 0x9C] | (chunk[i + 0x9D] << 8) |
                                                  (chunk[i + 0x9E] << 16) | (chunk[i + 0x9F] << 24));
                if (model < 0x00400000u || model >= 0x7FFF0000u || (model & 3u) != 0u) continue;
                const int x = (int)(int16_t)(chunk[i + 0x20] | (chunk[i + 0x21] << 8));
                const int y = (int)(int16_t)(chunk[i + 0x22] | (chunk[i + 0x23] << 8));
                const int z = (int)(int16_t)(chunk[i + 0x24] | (chunk[i + 0x25] << 8));
                if (x == 0 && y == 0 && z == 0) continue;
                const int flags = (int)chunk[i + 0x895];
                if (g_actorSigRequireFlags && flags == 0) continue;
                ++shaped;
                const double dx = (double)(to.x - x);
                const double dy = (double)(to.y - y);
                const double dz = (double)(to.z - z);
                const double d2 = dx * dx + dy * dy + dz * dz;
                if (d2 > radius * radius) continue;
                const int d = (int)sqrt(d2);
                int slot = nbest;
                while (slot > 0 && best[slot - 1].d > d) { if (slot < wantHits) best[slot] = best[slot - 1]; --slot; }
                if (slot < wantHits) {
                    best[slot].addr = (uint32_t)(cur + i);
                    best[slot].model = model;
                    best[slot].x = x; best[slot].y = y; best[slot].z = z; best[slot].d = d;
                    best[slot].flags = flags;
                    best[slot].stance = (int)(int16_t)(chunk[i + 0xA26] | (chunk[i + 0xA27] << 8));
                    if (nbest < wantHits) ++nbest;
                }
            }
        }
    }

    s_lines++;
    DebugLogger::TestLogFormat(
        "Sig[%llu]: %u regions, %llu KB swept, %u candidates, %u object-shaped, "
        "TO=(%d,%d,%d) r=%d -> %d in range",
        frame, regions, (unsigned long long)(swept / 1024u), examined, shaped,
        to.x, to.y, to.z, g_actorSigRadius, nbest);
    char line[900];
    int used = 0;
    line[0] = '\0';
    for (int i = 0; i < nbest; ++i) {
        const int n = _snprintf_s(line + used, sizeof(line) - used, _TRUNCATE,
            "%s%08X=(%d,%d,%d)d=%d m=%08X f=%02X st=%d", used ? "  " : "",
            best[i].addr, best[i].x, best[i].y, best[i].z, best[i].d,
            best[i].model, (unsigned)best[i].flags, best[i].stance);
        if (n > 0) used += n;
        if ((i + 1) % 2 == 0) {
            DebugLogger::TestLogFormat("  Sig: %s", line);
            used = 0; line[0] = '\0';
        }
    }
    if (used > 0) DebugLogger::TestLogFormat("  Sig: %s", line);
}

// Walks the demo's per-shot cast list off the scene object.
//
// This is the walk that should have been written six builds ago, because the
// hook was already holding the pointer it needs. Head sentinel at scene+0x38,
// first node at scene+0x3C, next at [node+0x04], exactly as 0x5CD439 does it.
//
// Self-validating like the task walk: the command word at +0x18 must land in
// 1..0x4A (the jump table at 0x5CEA65 has 0x4A entries), and the positions must
// look like room coordinates. If the commands come back as noise, the walk is
// wrong and nothing that follows is worth reading.
void EnumerateDemoCast(uint32_t scene, unsigned long long frame) {
    if (!g_demoCastScan || scene == 0) return;
    static int s_lines = 0;
    if (s_lines >= g_demoCastLines) return;
    const unsigned every = (unsigned)(g_demoCastEvery < 1 ? 1 : g_demoCastEvery);
    if ((frame % every) != 0) return;
    if (scene < 0x00400000u || scene >= 0x7FFF0000u || (scene & 3u) != 0u) return;

    const uint32_t head = scene + (uint32_t)kDemoCastHeadOffset;
    int32_t first = 0;
    if (!SafeRead32((uintptr_t)scene + kDemoCastFirstOffset, &first)) return;
    s_lines++;

    const int maxNodes = (g_demoCastMax < 1) ? 1 : (g_demoCastMax > 64 ? 64 : g_demoCastMax);
    uint32_t node = (uint32_t)first;
    int n = 0, sane = 0, used = 0;
    char line[900];
    line[0] = '\0';
    while (node != 0 && node != head && n < maxNodes) {
        if (node < 0x00400000u || node >= 0x7FFF0000u || (node & 3u) != 0u) break;
        int32_t id = 0, cmd = 0;
        int16_t rx = 0, ry = 0, rz = 0, px = 0, py = 0, pz = 0;
        SafeRead32((uintptr_t)node + kDemoNodeIdOffset, &id);
        SafeRead32((uintptr_t)node + kDemoNodeCmdOffset, &cmd);
        SafeRead16((uintptr_t)node + kDemoNodeRotOffset + 0, &rx);
        SafeRead16((uintptr_t)node + kDemoNodeRotOffset + 2, &ry);
        SafeRead16((uintptr_t)node + kDemoNodeRotOffset + 4, &rz);
        SafeRead16((uintptr_t)node + kDemoNodePosOffset + 0, &px);
        SafeRead16((uintptr_t)node + kDemoNodePosOffset + 2, &py);
        SafeRead16((uintptr_t)node + kDemoNodePosOffset + 4, &pz);
        if (cmd >= 1 && cmd <= 0x4A) ++sane;
        const int w = _snprintf_s(line + used, sizeof(line) - used, _TRUNCATE,
            "%s%08X id=%08X cmd=%02X pos=(%d,%d,%d) rot=(%d,%d,%d)",
            used ? " | " : "", node, (unsigned)id, (unsigned)cmd,
            (int)px, (int)py, (int)pz, (int)rx, (int)ry, (int)rz);
        if (w > 0) used += w;
        if (used > 640) {
            DebugLogger::TestLogFormat("  Cast: %s", line);
            used = 0; line[0] = '\0';
        }
        int32_t next = 0;
        if (!SafeRead32((uintptr_t)node + kDemoNodeNextOffset, &next)) break;
        node = (uint32_t)next;
        ++n;
    }
    DebugLogger::TestLogFormat(
        "Cast[%llu]: scene=%08X head=%08X -> %d node%s, %d with a valid command%s",
        frame, scene, head, n, n == 1 ? "" : "s", sane,
        (n > 0 && sane == 0) ? " (NONE -- this walk is wrong, stop reading here)" : "");
    if (used > 0) DebugLogger::TestLogFormat("  Cast: %s", line);
}

// Reads [node+0x14] as a source-path string and shortens it to the last two
// path components -- "snake\sna_init.c" rather than the whole C:\mgs\source
// prefix, which is the same on every line and would blow the log budget.
// Returns false when the pointer or the bytes are not a plausible path, which
// is the signal that the walk has left the list.
bool ReadTaskName(uint32_t node, char* out, size_t cap) {
    out[0] = '\0';
    int32_t p = 0;
    if (!SafeRead32((uintptr_t)node + kTaskNameOffset, &p)) return false;
    const uint32_t s = (uint32_t)p;
    if (s < 0x00400000u || s >= 0x7FFF0000u) return false;
    char buf[80];
    if (!SafeReadBytes((uint8_t*)buf, (const uint8_t*)(uintptr_t)s, sizeof(buf))) return false;
    buf[sizeof(buf) - 1] = '\0';
    size_t n = 0;
    while (n < sizeof(buf) - 1 && buf[n] != '\0') {
        const unsigned char c = (unsigned char)buf[n];
        if (c < 0x20 || c > 0x7E) return false;   // not text -> not a name
        ++n;
    }
    if (n < 4) return false;
    // Keep the last two components.
    int slashes = 0; size_t start = 0;
    for (size_t i = n; i > 0; --i) {
        if (buf[i - 1] == '\\' || buf[i - 1] == '/') {
            if (++slashes == 2) { start = i; break; }
        }
    }
    size_t j = 0;
    for (size_t i = start; i < n && j + 1 < cap; ++i) out[j++] = buf[i];
    out[j] = '\0';
    return j > 0;
}

// Walks every class list and asks each object its own name.
//
// Head for class c is 0x6BFC98 + c*0x44 and is a circular sentinel: 0x40A2AF
// inserts at the front, writing [head]=obj, [obj]=oldFirst, [obj+4]=head. So
// next is [node+0x00] and the walk ends when it comes back to the head.
//
// The point is the name. A line that reads "snake\sna_init.c" or "zako\zako.c"
// is proof the traversal is sound AND tells us who the object is, which is the
// question the last six builds were all circling. The [+0x20] triple is logged
// beside it: for the player that offset is a world position (+4022EC reads it
// as one), and whether it is also one for a class-4 character is exactly what
// these lines will show.
void EnumerateTasks(unsigned long long frame) {
    if (!g_taskScan || g_moduleBase == 0) return;
    static int s_lines = 0;
    if (s_lines >= g_taskScanLines) return;
    const unsigned every = (unsigned)(g_taskScanEvery < 1 ? 1 : g_taskScanEvery);
    if ((frame % every) != 0) return;
    s_lines++;

    const uintptr_t tableLo = g_moduleBase + kEntityHeadTableRva;
    const uintptr_t tableHi = tableLo + 16u * kEntityHeadStride;
    const uint32_t cap = g_playerActor.load(std::memory_order_relaxed);
    const int maxNodes = (g_taskScanMax < 1) ? 1 : (g_taskScanMax > 256 ? 256 : g_taskScanMax);

    int total = 0, named = 0;
    uint32_t snake = 0;
    DebugLogger::TestLogFormat("Task[%llu]: walking 16 class lists, cap=%08X", frame, cap);

    char line[900];
    for (int cls = 0; cls < 16; ++cls) {
        if ((g_taskScanClassMask & (1 << cls)) == 0) continue;
        const uintptr_t head = tableLo + (unsigned)cls * kEntityHeadStride;
        int32_t first = 0;
        if (!SafeRead32(head, &first)) continue;
        uint32_t node = (uint32_t)first;
        int n = 0, used = 0;
        line[0] = '\0';
        while (node != 0 && node != (uint32_t)head && n < maxNodes) {
            if (node < 0x00400000u || node >= 0x7FFF0000u || (node & 3u) != 0u) break;
            if ((uintptr_t)node >= tableLo && (uintptr_t)node < tableHi) break;
            char nm[48];
            const bool ok = ReadTaskName(node, nm, sizeof(nm));
            if (ok) ++named;
            int16_t x = 0, y = 0, z = 0;
            SafeRead16((uintptr_t)node + kPlayerPosOffset + 0, &x);
            SafeRead16((uintptr_t)node + kPlayerPosOffset + 2, &y);
            SafeRead16((uintptr_t)node + kPlayerPosOffset + 4, &z);
            if (ok && snake == 0) {
                // "snake", "sna_", "sne" -- the three spellings the source tree
                // uses for him. Recorded, not yet driven from.
                for (const char* p = nm; *p; ++p) {
                    if ((p[0] == 's' && p[1] == 'n' && (p[2] == 'a' || p[2] == 'e')) ||
                        (p[0] == 's' && p[1] == 'n' && p[2] == 'k')) { snake = node; break; }
                }
            }
            const int w = _snprintf_s(line + used, sizeof(line) - used, _TRUNCATE,
                "%s%08X=%s(%d,%d,%d)", used ? " " : "",
                node, ok ? nm : "<unnamed>", (int)x, (int)y, (int)z);
            if (w > 0) used += w;
            if (used > 700) {
                DebugLogger::TestLogFormat("  Task c%d: %s", cls, line);
                used = 0; line[0] = '\0';
            }
            int32_t next = 0;
            if (!SafeRead32((uintptr_t)node + kEntityNextOffset, &next)) break;
            node = (uint32_t)next;
            ++n; ++total;
        }
        if (used > 0) DebugLogger::TestLogFormat("  Task c%d: %s", cls, line);
    }
    g_snakeTask.store(snake, std::memory_order_relaxed);
    DebugLogger::TestLogFormat(
        "Task[%llu]: %d nodes, %d named%s -- names readable means the walk is CORRECT",
        frame, total, named, named ? "" : " (NONE -- the traversal is wrong, not the theory)");
}

// Which list is the player actually IN?
//
// The first walk answered a question I had not asked. Class 5 has 21 nodes and
// the captured player object (0x00900F00) is NOT one of them -- the chain runs
// 00901B40, 008FF730, 008FF620, ... and never reaches it. Their positions are
// not a cast either: an arithmetic run of (-16432,143,1), (-17744,143,1),
// (-19056,143,1) stepping -1312 with identical Y and Z, plus a scatter of
// (20250,1500,3000)-style round numbers. That is map or zone furniture, not
// characters. So class 5 is a game-object class but it is not the player's,
// and the +1E1701 allocation I read the 5 from belongs to something else.
//
// Rather than guess again, ask all sixteen. This walks every class list, counts
// it, and says which one contains the pointer the engine handed us at +E1B58.
// That is a search whose answer is checkable: exactly one list should contain
// it, and that list is the cast.
void EnumerateClassLists(unsigned long long frame) {
    if (!g_classScan || g_moduleBase == 0) return;
    static int s_lines = 0;
    if (s_lines >= g_classScanLines) return;
    const unsigned every = (unsigned)(g_classScanEvery < 1 ? 1 : g_classScanEvery);
    if ((frame % every) != 0) return;
    s_lines++;

    const uint32_t want = g_playerActor.load(std::memory_order_relaxed);
    const uintptr_t tableLo = g_moduleBase + kEntityHeadTableRva;
    const uintptr_t tableHi = tableLo + 16u * kEntityHeadStride;

    char line[900];
    int  used = 0;
    line[0] = '\0';
    int foundClass = -1;
    int foundIndex = -1;

    for (int cls = 0; cls < 16; ++cls) {
        const uintptr_t head = tableLo + (unsigned)cls * kEntityHeadStride;
        int32_t first = 0;
        if (!SafeRead32(head, &first)) continue;
        uint32_t node = (uint32_t)first;
        int n = 0, hit = -1;
        while (node != 0 && node != (uint32_t)head && n < 256) {
            if (node < 0x00400000u || node >= 0x7FFF0000u || (node & 3u) != 0u) break;
            if ((uintptr_t)node >= tableLo && (uintptr_t)node < tableHi) break;
            if (want != 0 && node == want) hit = n;
            int32_t next = 0;
            if (!SafeRead32(node + kEntityNextOffset, &next)) break;
            node = (uint32_t)next;
            ++n;
        }
        if (hit >= 0) { foundClass = cls; foundIndex = hit; }
        const int w = _snprintf_s(line + used, sizeof(line) - used, _TRUNCATE,
            "%s%d:%d%s", used ? " " : "", cls, n, hit >= 0 ? "*" : "");
        if (w > 0) used += w;
    }
    DebugLogger::TestLogFormat(
        "Class[%llu]: cap=%08X counts(class:nodes, * = contains cap) %s | cap is in class %d at index %d",
        frame, want, line, foundClass, foundIndex);
}

// Walks the class list, logs every node's position, and publishes the node
// closest to the shot's look-at target. "Nearest the thing the director is
// pointing at" is not a proof that a node is Snake -- but if NO node is near
// it, that is a proof that the actors and the camera are not in one coordinate
// space, and that is the fork the whole feature has been stuck on.
void EnumerateActors(const ViewPoint& to, unsigned long long frame) {
    if (!g_actorListScan || g_moduleBase == 0) return;
    static int s_lines = 0;
    if (s_lines >= g_actorListLines) return;
    const unsigned every = (unsigned)(g_actorListEvery < 1 ? 1 : g_actorListEvery);
    if ((frame % every) != 0) return;

    const uintptr_t head = g_moduleBase + kEntityHeadTableRva +
        (unsigned)g_actorListClass * kEntityHeadStride;
    int32_t first = 0;
    if (!SafeRead32(head, &first)) return;

    uint32_t node = (uint32_t)first;
    uint32_t best = 0;
    double   bestD = 1e18;
    int      n = 0, shown = 0;
    char     chunk[900];
    int      used = 0;
    chunk[0] = '\0';
    s_lines++;
    DebugLogger::TestLogFormat("Actors[%llu]: class=%d head=%08X first=%08X TO=(%d,%d,%d)",
        frame, g_actorListClass, (unsigned)head, (unsigned)first, to.x, to.y, to.z);

    // The head TABLE, not just this class's slot. The first walk ran off the
    // end of the chain into 0x6BFDCC and dutifully reported it as an actor at
    // (6976,144,0) -- which is the head pointer 0x00901B40 read as two int16s.
    // A node inside the table is the list terminating, not a game object.
    const uintptr_t tableLo = g_moduleBase + kEntityHeadTableRva;
    const uintptr_t tableHi = tableLo + 16u * kEntityHeadStride;

    while (node != 0 && node != (uint32_t)head && n < g_actorListMax) {
        if (node < 0x00400000u || node >= 0x7FFF0000u || (node & 3u) != 0u) break;
        if ((uintptr_t)node >= tableLo && (uintptr_t)node < tableHi) break;
        int16_t x = 0, y = 0, z = 0;
        const bool okp =
            SafeRead16(node + kPlayerPosOffset + 0, &x) &&
            SafeRead16(node + kPlayerPosOffset + 2, &y) &&
            SafeRead16(node + kPlayerPosOffset + 4, &z);
        if (okp) {
            const double dx = (double)(to.x - (int)x);
            const double dy = (double)(to.y - (int)y);
            const double dz = (double)(to.z - (int)z);
            const double d = sqrt(dx * dx + dy * dy + dz * dz);
            const bool zeroed = (x == 0 && y == 0 && z == 0);
            if (!zeroed && d < bestD) { bestD = d; best = node; }
            int32_t st = 0; SafeRead32(node + kEntityStateOffset, &st);
            const int w = _snprintf_s(chunk + used, sizeof(chunk) - used, _TRUNCATE,
                "%s%08X(%d,%d,%d)d=%d s=%X", used ? "  " : "",
                node, (int)x, (int)y, (int)z, (int)d, (unsigned)st);
            if (w > 0) used += w;
            if (++shown % 5 == 0) {
                DebugLogger::TestLogFormat("  Act: %s", chunk);
                used = 0; chunk[0] = '\0';
            }
        }
        int32_t next = 0;
        if (!SafeRead32(node + kEntityNextOffset, &next)) break;
        node = (uint32_t)next;
        ++n;
    }
    if (used > 0) DebugLogger::TestLogFormat("  Act: %s", chunk);

    g_listActor.store(best, std::memory_order_relaxed);
    g_listActorDist.store((int32_t)(bestD > 1e9 ? -1 : (int)bestD), std::memory_order_relaxed);
    g_listActorCount.store(n, std::memory_order_relaxed);
    DebugLogger::TestLogFormat("  Act: walked=%d nearestTO=%08X d=%d cap=%08X",
        n, best, (bestD > 1e9 ? -1 : (int)bestD),
        g_playerActor.load(std::memory_order_relaxed));
}

struct SnakeState {
    bool     havePtr = false;
    bool     fromCapture = false;
    bool     fromList = false;
    bool     fromGlobal = false;
    bool     plausible = false;   // [obj+0x9C] still looks like a pointer
    int      rejected = 0;        // candidates the gate threw out this frame
    uint32_t capPtr = 0;          // what the +E1B58 capture holds
    uint32_t globPtr = 0;         // what mgsi.exe+334228 holds
    uint32_t objPtr = 0;
    bool     havePos = false;
    ViewPoint pos{};        // [obj+0x20/22/24] -- base, feet
    bool     haveEye = false;   // eyeY is usable (either read or calibrated)
    bool     eyeFromModel = false;
    int32_t  eyeY = 0;      // final world Y for the eye
    int32_t  eyeRaw = 0;    // what [model+0x288] actually said, before vetting
    bool     eyeRejected = false;
    int32_t  eyeDelta = 0;  // eyeY - pos.y, the number that gets calibrated
    uint32_t modelPtr = 0;
    uint8_t  flags895 = 0;
    int16_t  stance = 0;
    bool     haveRot = false;
    int16_t  pitch = 0, yaw = 0;
};

// The last eye/base separation we were willing to believe. Snake's head does
// not change height between one frame and the next, so a single good reading
// carries the camera through every frame where the model pointer is stale --
// which is most of a cutscene. Seeded from the config, then earned.
int32_t g_eyeDeltaCal = 0;
bool    g_eyeDeltaCalValid = false;

// Is the object at this address still an actor?
//
// The 2026-08-25 run caught the captured object being torn down mid-cutscene:
// f895 walking C7 -> 00 -> D8 -> D5 -> D3, the position teleporting thousands
// of units per shot, and the model pointer reading back as FF3C011B, F252FE27,
// FF2200EF -- values that are not addresses. The 2026-08-26 run, with the
// first version of this gate in place, showed the other half of the same
// story: for most of the scene [obj+0x9C] is 00000000 and f895 is 00. The
// memory is not merely reused, it is CLEARED and occasionally rebuilt -- which
// is why the POV engaged on a handful of frames and not the rest, and why the
// picture flickered between Snake's eyes and the director's camera.
//
// The test is the one field that must be a pointer if this is an actor: the
// model/anim struct at +0x9C, which the engine dereferences itself at +E1AB4.
bool ActorLooksLive(uint32_t obj) {
    if (obj < 0x00400000u || obj >= 0x7FFF0000u || (obj & 3u) != 0u) return false;
    int32_t probe = 0;
    if (!SafeRead32((uintptr_t)obj + kPlayerModelPtrOffset, &probe)) return false;
    const uint32_t m = (uint32_t)probe;
    return m >= 0x00400000u && m < 0x7FFF0000u && (m & 3u) == 0u;
}

SnakeState ReadSnake() {
    SnakeState s;
    if (g_moduleBase == 0) return s;

    // The captured actor first. It is not a derivation -- it is the pointer
    // the engine's own first-person routine was holding at +E1B58, taken from
    // esi as it ran. The +334228 global is kept only as a fallback and only
    // because switching sources is one ini line if this turns out wrong too.
    // Both candidates are read every frame, and the global is logged whether or
    // not it is used -- because the 2026-08-25 run raised a question only it
    // can answer. The capture site is the FIRST-PERSON camera routine, which
    // does not run during a cutscene; so a capture taken before a room change
    // is frozen on the PREVIOUS room's player. The global at +334228 is written
    // by +E174B at construction time, which DOES happen during the scene. If
    // the demo builds the new room's Snake mid-cutscene, only the global sees
    // it. The debug log is consistent with exactly that: a new capture,
    // 008FF400, appeared only after the scene ended and first person resumed.
    uint32_t cap = g_playerActor.load(std::memory_order_relaxed);
    uint32_t glob = 0;
    { int32_t g = 0;
      if (SafeRead32(g_moduleBase + kPlayerObjPtrRva, &g) && g != 0) glob = (uint32_t)g; }
    s.capPtr = cap;
    s.globPtr = glob;

    // Candidates in the order this mode prefers them. The PLAUSIBILITY GATE
    // then picks the first one that is still an actor, rather than the first
    // one that is merely non-null. That is the difference that matters now:
    // the captured object spends most of the cutscene zeroed or reused, and
    // "non-null" was never the question.
    uint32_t cand[3] = { 0, 0, 0 };
    int nc = 0;
    switch (g_povActorSource) {
    case 0:  cand[nc++] = glob; break;
    case 1:  cand[nc++] = cap;  break;
    case 3:  cand[nc++] = g_listActor.load(std::memory_order_relaxed);
             cand[nc++] = cap; break;
    case 4:  cand[nc++] = glob; cand[nc++] = cap; break;
    default: cand[nc++] = cap;  cand[nc++] = glob; break;   // 2
    }

    uint32_t ptr = 0;
    for (int i = 0; i < nc && ptr == 0; ++i) {
        if (cand[i] == 0) continue;
        if (!ActorLooksLive(cand[i])) { s.rejected++; continue; }
        ptr = cand[i];
    }
    // Nothing survived. Keep the FIRST candidate for the log line so the probe
    // still reports what it saw, but leave havePos false so nothing drives.
    if (ptr == 0) {
        for (int i = 0; i < nc; ++i) if (cand[i]) { s.objPtr = cand[i]; s.havePtr = true; break; }
        if (s.havePtr) ReadViewPoint((uintptr_t)s.objPtr + kPlayerPosOffset, &s.pos);
        return s;
    }
    if (ptr == cap)  s.fromCapture = true;
    if (ptr == glob && ptr != cap) s.fromGlobal = true;
    if (g_povActorSource == 3 && ptr == g_listActor.load(std::memory_order_relaxed)) s.fromList = true;
    s.havePtr = true;
    s.objPtr = ptr;

    // A different actor means the head/feet separation we learned belongs to a
    // different body. Forget it rather than carry it across.
    static uint32_t s_calActor = 0;
    if (ptr != s_calActor) { s_calActor = ptr; g_eyeDeltaCalValid = false; g_eyeDeltaCal = 0; }

    const uintptr_t obj = (uintptr_t)(uint32_t)ptr;
    s.havePos = ReadViewPoint(obj + kPlayerPosOffset, &s.pos);

    // --- is this still an actor at all? ------------------------------------
    // The 2026-08-25 log caught the object being FREED AND REUSED part-way
    // through a cutscene. Up to Snake[345] it read f895=C7 and a steady
    // ~(150,550,-570). From Snake[360] on: f895=00, then D8/D5/D4/D3, pos
    // jumping (23050,0,-564) -> (19055,0,1438) -> (28181,0,-1712) ->
    // (-26941,0,32), and the model pointer coming back as FF3C011B, F252FE27,
    // FF2200EF -- values that are not addresses. That is not Snake moving.
    // That is our pointer dangling into memory the game has handed to
    // something else, and it is exactly the reported symptom: the camera jumps
    // somewhere new on every shot change, because every shot change reuses
    // that memory differently.
    //
    // The cheap, honest test is the one field we know must be a pointer: the
    // model/anim struct at +0x9C, which the engine itself dereferences at
    // +E1AB4. If that is not a plausible user-space address, this object is
    // not an actor any more and nothing read from it means anything.
    s.plausible = true;   // ActorLooksLive already vetted this candidate

    int16_t p = 0, y = 0;
    if (SafeRead16(obj + kPlayerRotOffset + 0, &p) &&
        SafeRead16(obj + kPlayerRotOffset + 2, &y)) {
        // The game masks the yaw word to 12 bits AFTER copying it out
        // (+E1AEF: and word [993FCA],0xFFF), so the raw field legitimately
        // carries high bits. Mask here or every angle looks like garbage.
        s.haveRot = true; s.pitch = p; s.yaw = (int16_t)(y & 0x0FFF);
    }

    if (!s.havePos) return s;

    // --- eye height, with the engine's own two guards ----------------------
    // +E1AA2  test byte [esi+0x895],8 / jne  -> skip the head substitution
    // +E1AB4  ecx = [esi+0x9C]; cx = [ecx+0x288]; -> replaces Y outright
    // +E1AC5  cmp word [esi+0xA26],2 / add word Y,0x140
    //
    // The first guard is the one that was missing. Without it we read
    // [model+0x288] in exactly the states where the engine itself declines
    // to, which is how eyeY came back as +31108 and threw the camera 15
    // metres into the ceiling.
    int32_t eyeY = s.pos.y;
    bool    fromModel = false;

    uint8_t f = 0;
    const bool haveFlags = SafeRead8(obj + kPlayerFlagsOffset, &f);
    if (haveFlags) s.flags895 = f;

    int32_t model = 0;
    if (SafeRead32(obj + kPlayerModelPtrOffset, &model) && model != 0) {
        s.modelPtr = (uint32_t)model;
        const bool engineWouldUseIt = haveFlags && ((f & kPlayerNoHeadBit) == 0);
        int16_t eye = 0;
        if (engineWouldUseIt &&
            SafeRead16((uintptr_t)(uint32_t)model + kPlayerEyeYOffset, &eye)) {
            s.eyeRaw = eye;
            // Second gate, ours: a head is never further from the feet than
            // a body length. Anything outside that band is a stale or
            // repurposed field, not a head, and is refused rather than flown to.
            if (Abs32(eye - s.pos.y) <= g_povEyeMaxDelta) {
                eyeY = eye;
                fromModel = true;
                g_eyeDeltaCal = eye - s.pos.y;
                g_eyeDeltaCalValid = true;
            } else {
                s.eyeRejected = true;
            }
        }
    }

    if (!fromModel) {
        eyeY = s.pos.y + (g_eyeDeltaCalValid ? g_eyeDeltaCal : g_povFallbackEye);
    }

    int16_t stance = 0;
    if (SafeRead16(obj + kPlayerStanceOffset, &stance)) {
        s.stance = stance;
        if (stance == 2) eyeY += kStanceEyeAdjust;   // +E1ACF, verbatim
    }

    s.haveEye = true;
    s.eyeFromModel = fromModel;
    s.eyeY = eyeY;
    s.eyeDelta = eyeY - s.pos.y;
    return s;
}
} // namespace

// ---------------------------------------------------------------------------
// The shared head-look maths, factored out so both view hooks use one copy.
// Takes the pair to rotate as ABSOLUTE addresses, because the view-build hook
// receives pointers chosen by whichever camera path is live rather than the
// fixed layer-3 globals.
//
// Returns true if it wrote this frame -- false on the frame that pins the
// reference, and on a failed write.
// ---------------------------------------------------------------------------
//
// `overrideFrom`, when non-null, relocates the EYE without touching the shot's
// direction: the yaw/pitch/length are still derived from the game's own
// (from -> to) vector, and only the point they are anchored to changes. That
// is what "watch the cutscene from Snake's eyes" means -- the director still
// frames and cuts the scene, you are just standing where Snake is standing.
// It also forces FROM to be written, which the offset-only path skips.
static bool DriveViewPair(uintptr_t fromAddr, uintptr_t toAddr,
    const ViewPoint& from, const ViewPoint& to,
    unsigned long long frame, const char* who, int ownerId, ViewPoint* outWrittenTo,
    const ViewPoint* overrideFrom = nullptr) {
    const float hYaw = g_headYaw.load(std::memory_order_relaxed);
    const float hPitch = g_headPitch.load(std::memory_order_relaxed);
    const float hRoll = g_headRoll.load(std::memory_order_relaxed);
    const float hPosX = g_headPosX.load(std::memory_order_relaxed);
    const float hPosY = g_headPosY.load(std::memory_order_relaxed);
    const float hPosZ = g_headPosZ.load(std::memory_order_relaxed);

    // --- reference pose -----------------------------------------------------
    // Its own, not the rotation hook's: during a cutscene that hook does not
    // run, so g_haveReference is stale exactly when this one matters.
    // Both hooks share one reference pose. Handing the camera from one to the
    // other without re-pinning would apply a delta measured against a pose that
    // belonged to a different owner -- a snap of the whole accumulated offset at
    // exactly the moment a cutscene starts or ends.
    static int s_refOwner = -1;
    const bool ownerChanged = (s_refOwner != ownerId);
    if (!g_vcHaveRef || ownerChanged ||
        g_vcRecenter.exchange(false, std::memory_order_relaxed)) {
        s_refOwner = ownerId;
        g_vcRefHeadYaw = hYaw; g_vcRefHeadPitch = hPitch; g_vcRefHeadRoll = hRoll;
        g_vcRefHeadPosX = hPosX; g_vcRefHeadPosY = hPosY; g_vcRefHeadPosZ = hPosZ;
        g_vcHaveRef = true;
        DebugLogger::LogFormat(
            "View commit (%s): reference pinned at frame %llu. head(y=%.3f p=%.3f) "
            "shot from=(%d,%d,%d) to=(%d,%d,%d)",
            who, frame, hYaw, hPitch, from.x, from.y, from.z, to.x, to.y, to.z);
        return false;                 // let this frame keep the game's framing
    }

    // --- head deltas --------------------------------------------------------
    // swap_yaw_pitch is deliberately NOT consulted. That flag exists because
    // layer 1 is a raw SVECTOR whose component order had to be established by
    // experiment; here we are working in a real 3D frame built from FROM and
    // TO, where yaw and pitch are defined by the geometry rather than by which
    // int16 they happen to live in.
    float dYawRad = NormalizeRad(hYaw - g_vcRefHeadYaw);
    float dPitchRad = NormalizeRad(hPitch - g_vcRefHeadPitch);
    float dRollRad = NormalizeRad(hRoll - g_vcRefHeadRoll);
    if (g_invertHeadYaw)   dYawRad = -dYawRad;
    if (g_invertHeadPitch) dPitchRad = -dPitchRad;
    if (g_invertHeadRoll)  dRollRad = -dRollRad;

    const float strength = g_vcRotation ? ((float)g_vcRotationPercent / 100.0f) : 0.0f;
    dYawRad *= strength;
    dPitchRad *= strength;
    dRollRad *= strength;

    // --- the shot's own direction -------------------------------------------
    // Game frame: X right, Y DOWN, Z forward. Y-down is why pitch uses -dy;
    // it is the same convention invert_position_y=1 encodes for layer 1.
    const float dx = (float)(to.x - from.x);
    const float dy = (float)(to.y - from.y);
    const float dz = (float)(to.z - from.z);

    float len = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (len < (float)g_vcMinDist) {
        // Degenerate shot (FROM == TO happens between scenes). Fall back to
        // the engine's own eye->target distance, then to the configured one.
        int32_t sceneDist = 0;
        if (SafeRead32(g_moduleBase + kSceneDistRva, &sceneDist) &&
            sceneDist >= g_vcMinDist && sceneDist <= 30000) {
            len = (float)sceneDist;
        }
        else {
            len = (float)g_vcDefaultDist;
        }
    }

    const float horiz = std::sqrt(dx * dx + dz * dz);

    // A near-vertical shot -- looking straight down a shaft, or straight up at
    // a gantry -- has no horizontal component, so atan2(dx, dz) collapses to
    // atan2(0, 0) == 0 and the yaw we would apply the head delta to is not the
    // yaw the shot actually has. Reusing the last good yaw keeps the view from
    // snapping to due north for the frames it takes the camera to tilt back.
    static float s_lastGoodYaw = 0.0f;
    static bool  s_haveGoodYaw = false;
    float yaw;
    if (horiz > 1.0f) {
        yaw = std::atan2(dx, dz);
        s_lastGoodYaw = yaw;
        s_haveGoodYaw = true;
    }
    else {
        yaw = s_haveGoodYaw ? s_lastGoodYaw : 0.0f;
    }
    float pitch = std::atan2(-dy, horiz);

    yaw = NormalizeRad(yaw + dYawRad);
    pitch += dPitchRad;

    // Never let the look direction reach straight up or down: at the poles the
    // yaw term stops meaning anything and the view snaps.
    const float kPitchLimit = 1.4835f;             // 85 degrees
    if (pitch > kPitchLimit)  pitch = kPitchLimit;
    if (pitch < -kPitchLimit) pitch = -kPitchLimit;

    const float cp = std::cos(pitch), sp = std::sin(pitch);
    const float sy = std::sin(yaw), cy = std::cos(yaw);

    // Unit basis in the game's frame, for the new look direction.
    const float fwdX = cp * sy, fwdY = -sp, fwdZ = cp * cy;
    const float rgtX = cy, rgtZ = -sy;             // right is horizontal by construction

    // --- 6DOF and stereo, both as an offset applied to FROM and TO together --
    // Moving both points by the same vector translates the camera without
    // touching where it points, which is exactly what leaning and an IPD
    // offset are.
    float offX = 0.0f, offY = 0.0f, offZ = 0.0f;

    if (g_vcPosition && g_positionEnabled) {
        // OpenXR play space: X right, Y up, Z BACK. Forward is -Z.
        const float right = hPosX - g_vcRefHeadPosX;
        const float up = hPosY - g_vcRefHeadPosY;
        const float fwd = -(hPosZ - g_vcRefHeadPosZ);
        float ox = (right * rgtX + fwd * fwdX) * g_positionScale;
        float oz = (right * rgtZ + fwd * fwdZ) * g_positionScale;
        float oy = -up * g_positionScale;          // game Y is down
        if (g_invertPosX) ox = -ox;
        if (g_invertPosY) oy = -oy;
        if (g_invertPosZ) oz = -oz;
        offX += (float)Clamp((int32_t)ox, -g_maxPositionOffset, g_maxPositionOffset);
        offY += (float)Clamp((int32_t)oy, -g_maxPositionOffset, g_maxPositionOffset);
        offZ += (float)Clamp((int32_t)oz, -g_maxPositionOffset, g_maxPositionOffset);
    }

    // Alternate-eye stereo. The eye tag is normally advanced by the rotation
    // hook, which does not run here -- so when it has gone quiet this hook
    // advances the tag itself, otherwise cutscenes would silently be mono for
    // the same reason PATH B always was.
    // Render twice draws both eyes itself (see render_twice.h), so this path
    // must neither advance the tag nor offset the camera while it is live.
    if (g_vcStereo && g_stereoAlternate && IsRenderTwiceLive()) {
        g_publishedEye.store(2, std::memory_order_relaxed);
        g_eyePublishTick.store(GetTickCount64(), std::memory_order_relaxed);
    }
    else if (g_vcStereo && g_stereoAlternate) {
        static unsigned long long s_lastRotRuns = 0;
        static int s_vcEye = 0;
        const unsigned long long rotRuns = g_rotHookRuns.load(std::memory_order_relaxed);
        if (rotRuns == s_lastRotRuns) {
            s_vcEye ^= 1;
            g_publishedEye.store(s_vcEye, std::memory_order_relaxed);
            g_eyePublishTick.store(GetTickCount64(), std::memory_order_relaxed);
        }
        s_lastRotRuns = rotRuns;

        const int eye = g_publishedEye.load(std::memory_order_relaxed);
        if (eye == 0 || eye == 1) {
            float half = ((float)g_stereoIpdMm / 2000.0f) * g_positionScale;
            if (g_stereoSwapEyes) half = -half;
            const float s = (eye == 0) ? -half : half;
            offX += rgtX * s;
            offZ += rgtZ * s;
        }
    }

    const ViewPoint& base = overrideFrom ? *overrideFrom : from;
    ViewPoint newFrom{ base.x + (int32_t)offX, base.y + (int32_t)offY, base.z + (int32_t)offZ };
    ViewPoint newTo{
        newFrom.x + (int32_t)(fwdX * len),
        newFrom.y + (int32_t)(fwdY * len),
        newFrom.z + (int32_t)(fwdZ * len)
    };

    const bool wroteFrom = (overrideFrom || offX != 0.0f || offY != 0.0f || offZ != 0.0f)
        ? WriteViewPoint(fromAddr, newFrom) : true;
    const bool wroteTo = WriteViewPoint(toAddr, newTo);

    if (g_writeRoll && dRollRad != 0.0f) {
        SafeWrite32(g_moduleBase + kViewTwistRva, (uint32_t)RadToUnits(dRollRad));
    }

    // FOV, written at the point of use rather than at the source. 0x54477 does
    // copy 0x594000 into 0x593F7C each frame, so the mod's existing write to
    // 0x594000 genuinely propagates -- but only while the camera task runs.
    // Writing here as well covers the frames where it does not, and costs one
    // guarded store. Off by default: the source write is sufficient in every
    // state observed so far, and two writers for one value is what this whole
    // session has been about avoiding.
    if (g_vcFov && g_fovClipDistance != 0) {
        SafeWrite32(g_moduleBase + kViewFovRva, (uint32_t)g_fovClipDistance);
    }

    if (outWrittenTo) *outWrittenTo = newTo;

    // --- instrument the OUTPUT, not the mechanism ---------------------------
    // Degrees, not raw int16, and the game's direction alongside ours. If the
    // picture does not move, this line says immediately whether the maths was
    // wrong or the write never reached the renderer -- the exact question the
    // 2026-08-23 pulse could not answer.
    static int s_driveLog = 0;
    if (s_driveLog < g_vcLogFrames && (frame % 15) == 0) {
        s_driveLog++;
        const float gameYawDeg = std::atan2(dx, dz) * 180.0f / kPi;
        const float gamePitchDeg = std::atan2(-dy, horiz) * 180.0f / kPi;
        int16_t backTo = 0;
        const bool readBack = SafeRead16(toAddr, &backTo);
        DebugLogger::TestLogFormat(
            "View[%s %llu]: shot(yaw=%.1f pitch=%.1f len=%.0f) head(dyaw=%.1f dpitch=%.1f) "
            "-> wrote(yaw=%.1f pitch=%.1f) pov=%d from=(%d,%d,%d) to=(%d,%d,%d) "
            "readback_to_x=%d(%s) wf=%d wt=%d",
            who, frame, gameYawDeg, gamePitchDeg, len,
            dYawRad * 180.0f / kPi, dPitchRad * 180.0f / kPi,
            yaw * 180.0f / kPi, pitch * 180.0f / kPi,
            overrideFrom ? 1 : 0,
            newFrom.x, newFrom.y, newFrom.z, newTo.x, newTo.y, newTo.z,
            readBack ? (int)backTo : -1,
            (readBack && backTo == (int16_t)Clamp(newTo.x, -32768, 32767)) ? "OK" : "MISMATCH",
            wroteFrom ? 1 : 0, wroteTo ? 1 : 0);
    }
    return wroteTo;
}


extern "C" void __cdecl Mgs1ViewCommitBody() {
    if (g_moduleBase == 0) return;

    const unsigned long long frame =
        g_vcFrames.fetch_add(1, std::memory_order_relaxed) + 1;

    ViewPoint from{}, to{};
    if (!ReadViewPoint(g_moduleBase + kViewFromRva, &from) ||
        !ReadViewPoint(g_moduleBase + kViewToRva, &to)) {
        static int badRead = 0;
        if (badRead < 5) {
            badRead++;
            DebugLogger::LogFormat("View commit: layer 3 unreadable at frame %llu -- standing down", frame);
        }
        g_vcDriving.store(false, std::memory_order_relaxed);
        return;
    }

    // Did the camera task early-out via [0x32279F] & 0x80? If it did, 0x54477
    // never ran and what we are looking at is our OWN write from last frame
    // rather than the game's intent. Worth knowing about; it is the state the
    // community cheat table's T hotkey deliberately induces.
    static ViewPoint s_lastWrittenTo{}; static bool s_haveLastWritten = false;
    static int s_persistLog = 0;
    if (s_haveLastWritten && s_persistLog < 6 &&
        to.x == s_lastWrittenTo.x && to.y == s_lastWrittenTo.y && to.z == s_lastWrittenTo.z) {
        s_persistLog++;
        DebugLogger::LogFormat(
            "View commit: layer 3 still holds OUR value at frame %llu -- the game's camera task "
            "did not refresh it (cutscene control bit 7, or a modal state). Not an error.", frame);
    }

    const bool vrMode = g_vrViewMode.load(std::memory_order_relaxed);
    const bool hookEnabled = g_enabled.load(std::memory_order_relaxed);
    const bool havePose = g_havePose.load(std::memory_order_relaxed);

    // Observe-only, native view mode, hook off, or no head pose yet: read and
    // log, write nothing. Native mode in particular must be the game exactly
    // as shipped -- that is the whole contract of the R3 toggle.
    bool drive = (g_vcMode != 0) && vrMode && hookEnabled && havePose;
    if (drive && g_vcMode == 1) {
        drive = g_csvDetected.load(std::memory_order_relaxed) ||
            g_csvActive.load(std::memory_order_relaxed);
    }

    static int s_observeLog = 0;
    if (!drive) {
        if (s_observeLog < g_vcLogFrames && (frame % 60) == 0) {
            s_observeLog++;
            DebugLogger::LogFormat(
                "View commit: observing frame %llu. from=(%d,%d,%d) to=(%d,%d,%d) "
                "[vr=%d enabled=%d pose=%d mode=%d cutscene=%d]",
                frame, from.x, from.y, from.z, to.x, to.y, to.z,
                vrMode ? 1 : 0, hookEnabled ? 1 : 0, havePose ? 1 : 0, g_vcMode,
                g_csvDetected.load(std::memory_order_relaxed) ? 1 : 0);
        }
        g_vcDriving.store(false, std::memory_order_relaxed);
        g_vcDroveThisBuild.store(false, std::memory_order_relaxed);
        g_vcHaveRef = false;      // re-pin on the next frame we do drive
        s_haveLastWritten = false;
        return;
    }

    ViewPoint written{};
    const bool wrote = DriveViewPair(g_moduleBase + kViewFromRva, g_moduleBase + kViewToRva,
        from, to, frame, "L3", 0, &written);
    s_lastWrittenTo = written;
    s_haveLastWritten = wrote;
    g_vcDriving.store(wrote, std::memory_order_relaxed);
    // OWNED, not "wrote" (2026-10-04, rappel log): on the frame this hook pins
    // its reference it writes nothing, so the view-build hook (+53C5C, the same
    // pair) took the frame under its own owner id -- which re-pinned the shared
    // reference, which made this hook re-pin the next frame, and so on. 2,000+
    // "reference pinned" lines, and head look never applied once: a flat,
    // fixed third-person picture. The layer-3 hook claiming the build whenever
    // it is driving at all keeps one owner, so the pin happens once.
    g_vcDroveThisBuild.store(true, std::memory_order_relaxed);
}


// ===========================================================================
// SNAKE'S EYES IN GAMEPLAY -- called from the view-build body for the gameplay
// camera (+53C5C, FROM/TO = layer 3 at 0x593F60/68).
//
// Engages only when the game has taken the camera OUT of first person while
// the player is still playing: the camera task is running (not a cutscene),
// no menu or inventory is open, VR mode is on and not handed over to the
// virtual screen (wall press, Nikita), and either the first-person flag is
// off or the first-person camera is being driven by a script that ignores
// rotate2 (0x593FF9 & 3 -- the elevator ride's fixed first-person shot).
//
// The player object is alive through all of these, so the eye is Snake's
// head joint (DG_OBJS joint 6 world translation, written by his own act this
// frame) nudged in front of the face; the facing is his body yaw plus your
// head. Mono, like cutscene Snake's eyes without render twice; the frame view
// record keeps it world-locked and FOV-exact.
// ===========================================================================
static bool GpPovNameIsSnake(uint32_t obj) {
    int32_t name = 0;
    if (!SafeRead32((uintptr_t)obj + 0x14, &name)) return false;
    if ((uint32_t)name < 0x00400000u || (uint32_t)name >= 0x7FFF0000u) return false;
    char buf[48] = {};
    if (!SafeReadBytes((uint8_t*)buf, (const uint8_t*)(uintptr_t)(uint32_t)name, sizeof(buf) - 1)) return false;
    buf[sizeof(buf) - 1] = 0;
    return strstr(buf, "sna_init") != nullptr;
}

// Is this task still LINKED in the game's live task lists? Freed tasks keep
// their memory (name and all) readable for a while after a level change.
static bool GpPovTaskLinked(uint32_t want) {
    const uintptr_t lo = g_moduleBase + kEntityHeadTableRva;
    const uintptr_t hi = lo + 16u * kEntityHeadStride;
    for (int c = 0; c < 16; ++c) {
        const uintptr_t head = lo + (unsigned)c * kEntityHeadStride;
        int32_t first = 0;
        if (!SafeRead32(head, &first)) continue;
        uint32_t node = (uint32_t)first;
        for (int n = 0; n < 512 && node != 0 && node != (uint32_t)head; ++n) {
            if (node < 0x00400000u || node >= 0x7FFF0000u || (node & 3u) != 0u) break;
            if ((uintptr_t)node >= lo && (uintptr_t)node < hi) break;
            if (node == want) return true;
            int32_t next = 0;
            if (!SafeRead32((uintptr_t)node, &next)) break;
            node = (uint32_t)next;
        }
    }
    return false;
}

// Is an on-foot Snake (sna_init, linked in the live task lists) the player
// right now? The torture tasks only stand in for him when he is NOT -- the
// small sne_03c helper (0x38 bytes, same source name) may well live beside a
// walking Snake in the cell. Cached for 300 ms (a list walk each time).
static bool GpPovLiveSnaInit() {
    static bool s_live = false;
    static unsigned long long s_tick = 0;
    const unsigned long long t = GetTickCount64();
    if (s_tick && t - s_tick < 300) return s_live;
    s_tick = t;
    int32_t obj = 0;
    s_live = SafeRead32(g_moduleBase + kPlayerObjPtrRva, &obj) &&
        (uint32_t)obj >= 0x00400000u && (uint32_t)obj < 0x7FFF0000u &&
        GpPovNameIsSnake((uint32_t)obj) && GpPovTaskLinked((uint32_t)obj);
    return s_live;
}

// Diagnostic for the next place Snake's eyes drops out: which class-5 tasks
// (Snake's class -- sna_init, rope.c, torture.c all live there) exist at the
// moment no player object could be found. Short names, a few lines per run.
static void GpPovLogClass5Names(const char* why) {
    static int s_lines = 0;
    static unsigned long long s_last = 0;
    const unsigned long long t = GetTickCount64();
    if (s_lines >= 6 || (s_last && t - s_last < 5000)) return;
    s_last = t;
    s_lines++;
    std::string out;
    const uintptr_t lo = g_moduleBase + kEntityHeadTableRva;
    const uintptr_t hi = lo + 16u * kEntityHeadStride;
    const uintptr_t head = lo + 5u * kEntityHeadStride;
    int32_t first = 0;
    int count = 0;
    if (SafeRead32(head, &first)) {
        uint32_t node = (uint32_t)first;
        for (int n = 0; n < 96 && node != 0 && node != (uint32_t)head; ++n) {
            if (node < 0x00400000u || node >= 0x7FFF0000u || (node & 3u) != 0u) break;
            if ((uintptr_t)node >= lo && (uintptr_t)node < hi) break;
            char nm[64] = {};
            if (ReadTaskName(node, nm, sizeof(nm))) {
                const char* b = std::strrchr(nm, '\\');
                b = b ? b + 1 : nm;
                if (out.find(b) == std::string::npos) { if (!out.empty()) out += ' '; out += b; }
            }
            count++;
            int32_t next = 0;
            if (!SafeRead32((uintptr_t)node, &next)) break;
            node = (uint32_t)next;
        }
    }
    DebugLogger::LogFormat("Gameplay Snake's eyes: %s -- class-5 tasks right now (%d): %s", why, count,
        out.empty() ? "(none readable)" : out.c_str());
}

// Snake in a vehicle -- or on the rappel rope (rope\rope.c, kind 3): a task
// whose source-file name says so, and the DG_OBJS inside it that has a posed
// head joint. Cached; re-validated by name.
static bool GpPovFindVehicleSnake(uint32_t* outObj, uint32_t* outObjs) {
    static uint32_t s_obj = 0, s_objs = 0;
    static unsigned long long s_lastScan = 0;
    static const char* const kNames[] = { "jeep_sne", "pkjp_sne", "sne17a", "snake18", "rope\\rope.c",
                                          "torture\\torture.c", "torture\\sne_03c.c" };
    auto isRopeName = [](const char* nm) { return std::strstr(nm, "rope\\rope.c") != nullptr; };
    auto isTortureName = [](const char* nm) {
        return std::strstr(nm, "torture\\torture.c") != nullptr || std::strstr(nm, "torture\\sne_03c.c") != nullptr;
    };
    auto nameOk = [&](uint32_t node) {
        char nm[64];
        if (!ReadTaskName(node, nm, sizeof(nm))) return false;
        for (const char* n : kNames) {
            if (!std::strstr(nm, n)) continue;
            if (isRopeName(nm) && !g_gpPovRope) return false;
            if (isTortureName(nm)) {
                if (!g_gpPovTorture) return false;
                // sne_03c.c registers TWO tasks under one name (task roster):
                // Snake (0x800, init 5047A7 / update 504605) and a 0x38-byte
                // helper (504B0A / 504963). Only the big one carries his model.
                int32_t f8 = 0, fc = 0;
                SafeRead32((uintptr_t)node + 0x08, &f8);
                SafeRead32((uintptr_t)node + 0x0C, &fc);
                const uint32_t h1 = (uint32_t)(g_moduleBase + 0x104B0A), h2 = (uint32_t)(g_moduleBase + 0x104963);
                if ((uint32_t)f8 == h1 || (uint32_t)f8 == h2 || (uint32_t)fc == h1 || (uint32_t)fc == h2) return false;
                // Only when no walking Snake is the player.
                if (GpPovLiveSnaInit()) return false;
            }
            return true;
        }
        return false;
    };
    auto objsOk = [](uint32_t p) {
        if (p < 0x00400000u || p >= 0x7FFF0000u || (p & 3u)) return false;
        int16_t n = 0; int32_t def = 0, rx = 0, ry = 0, rz = 0, hx = 0, hy = 0, hz = 0;
        if (!SafeRead16((uintptr_t)p + 0x2E, &n) || n < 8 || n > 40) return false;
        if (!SafeRead32((uintptr_t)p + 0x24, &def) || (uint32_t)def < 0x00400000u || (uint32_t)def >= 0x7FFF0000u) return false;
        if (!SafeRead32((uintptr_t)p + 0x14, &rx) || !SafeRead32((uintptr_t)p + 0x18, &ry) || !SafeRead32((uintptr_t)p + 0x1C, &rz) ||
            !SafeRead32((uintptr_t)p + 0x284, &hx) || !SafeRead32((uintptr_t)p + 0x288, &hy) || !SafeRead32((uintptr_t)p + 0x28C, &hz))
            return false;
        if (hx == 0 && hy == 0 && hz == 0) return false;
        return std::abs(hx - rx) < 2500 && std::abs(hy - ry) < 2500 && std::abs(hz - rz) < 2500;
    };
    // 2026-10-02 round 8: a cached vehicle Snake outlived its level -- loading
    // another save left the REX-top task's memory readable, name and all, and
    // the view was built from it ("VR mode takes me outside of the map"). A
    // cached task now has to be found LINKED in the live task lists again
    // (checked every 300 ms), or it is dropped.
    static unsigned long long s_verified = 0;
    if (s_obj && nameOk(s_obj) && objsOk(s_objs)) {
        const unsigned long long t = GetTickCount64();
        if (t - s_verified < 300) { *outObj = s_obj; *outObjs = s_objs; return true; }
        if (GpPovTaskLinked(s_obj)) { s_verified = t; *outObj = s_obj; *outObjs = s_objs; return true; }
        DebugLogger::LogFormat("Gameplay Snake's eyes: vehicle task %08X is no longer in the game's task lists "
            "(level changed) -- dropped", s_obj);
        s_obj = s_objs = 0;
        g_gpPovVehicleKind.store(0, std::memory_order_relaxed);
        g_gpPovVehicleTick.store(0, std::memory_order_relaxed);
        return false;
    }
    if (s_obj) {
        // The task ended (rope.c is deleted when Snake lands) or its model
        // stopped looking posed. Drop the kind so nothing downstream keeps
        // treating him as riding.
        g_gpPovVehicleKind.store(0, std::memory_order_relaxed);
    }
    s_obj = s_objs = 0;
    const unsigned long long now = GetTickCount64();
    // A scan is cheap, but not every frame. 250 ms, not a second: the rope
    // task appears the moment Snake steps off the roof, and until it is found
    // the view would sit on whatever the game shows.
    if (now - s_lastScan < 250) return false;
    s_lastScan = now;
    const uintptr_t tableLo = g_moduleBase + kEntityHeadTableRva;
    const uintptr_t tableHi = tableLo + 16u * kEntityHeadStride;
    const uint32_t playerDef = g_playerModelDef.load(std::memory_order_relaxed);
    for (int c = 0; c < 16 && !s_obj; ++c) {
        const uintptr_t head = tableLo + (unsigned)c * kEntityHeadStride;
        int32_t first = 0;
        if (!SafeRead32(head, &first)) continue;
        uint32_t node = (uint32_t)first;
        for (int n = 0; n < 96 && node != 0 && node != (uint32_t)head; ++n) {
            if (node < 0x00400000u || node >= 0x7FFF0000u || (node & 3u) != 0u) break;
            if ((uintptr_t)node >= tableLo && (uintptr_t)node < tableHi) break;
            if (nameOk(node)) {
                char nm[64] = {};
                ReadTaskName(node, nm, sizeof(nm));
                const bool rope = isRopeName(nm);
                const bool torture = isTortureName(nm);
                // rope.c is a 0x1098-byte task (task roster); torture.c 0x904,
                // sne_03c.c 0x800; vehicles fit in 0x800.
                const uint32_t scanEnd = rope ? 0x1090u
                                       : torture ? (std::strstr(nm, "torture.c") ? 0x900u : 0x7FCu)
                                       : 0x800u;
                const bool mostJoints = rope || torture;
                // Prefer the DG_OBJS built from Snake's own model; the first
                // plausible one (the jeep, the gun) only as a last resort. On
                // the rope the fallback is the candidate with the MOST joints
                // (Snake's skeleton, not a short run of rope segments).
                uint32_t best = 0, any = 0;
                int16_t anyN = 0;
                int found = 0;
                for (uint32_t off = 0x20; off < scanEnd && !best; off += 4) {
                    int32_t p = 0;
                    if (!SafeRead32((uintptr_t)node + off, &p) || !objsOk((uint32_t)p)) continue;
                    int32_t def = 0;
                    int16_t nj = 0;
                    SafeRead32((uintptr_t)(uint32_t)p + 0x24, &def);
                    SafeRead16((uintptr_t)(uint32_t)p + 0x2E, &nj);
                    if (mostJoints) found++;
                    static int s_candLog = 0;
                    if (mostJoints && s_candLog < 24) {
                        s_candLog++;
                        DebugLogger::LogFormat("Gameplay Snake's eyes: %s task %08X candidate model at +0x%X = %08X "
                            "(%d joints, def %08X%s)", rope ? "rope" : "torture", node, off, (uint32_t)p, (int)nj,
                            (uint32_t)def, (playerDef != 0 && (uint32_t)def == playerDef) ? " = Snake's model" : "");
                    }
                    if (playerDef != 0 && (uint32_t)def == playerDef) best = (uint32_t)p;
                    else if (!any || (mostJoints && nj > anyN)) { any = (uint32_t)p; anyN = nj; }
                }
                if (!best) best = any;
                if (best) {
                    s_obj = node; s_objs = best;
                    s_verified = GetTickCount64();
                    g_gpPovVehicleKind.store(rope ? 3 : torture ? 4 : (std::strstr(nm, "snake18") ? 2 : 1),
                        std::memory_order_relaxed);
                    DebugLogger::LogFormat("Gameplay Snake's eyes: Snake is %s -- task %08X (%s), model %08X",
                        rope ? "on the rappel rope" : torture ? "held by Ocelot (torture rack / cell)" : "in a vehicle",
                        node, nm, best);
                    break;
                }
                else if (mostJoints) {
                    static int s_ropeMiss = 0;
                    if (s_ropeMiss < 6) {
                        s_ropeMiss++;
                        DebugLogger::LogFormat("Gameplay Snake's eyes: %s task %08X found but no posed model "
                            "inside it yet (%d candidates)", rope ? "rope" : "torture", node, found);
                    }
                }
            }
            int32_t next = 0;
            if (!SafeRead32((uintptr_t)node, &next)) break;
            node = (uint32_t)next;
        }
    }
    if (!s_obj) return false;
    *outObj = s_obj; *outObjs = s_objs;
    return true;
}

static bool DriveGameplayPov(unsigned callerRva, uint32_t fromPtr, uint32_t toPtr, uint32_t* stack,
                             unsigned long long frame) {
    static bool s_active = false;
    static unsigned long long s_lastTick = 0;
    static float s_refYaw = 0.0f, s_anchorYaw = 0.0f;
    static float s_refPosX = 0.0f, s_refPosY = 0.0f, s_refPosZ = 0.0f;
    static float s_userYaw = 0.0f;
    static int s_logLines = 0;
    static const char* s_lastWhyNot = nullptr;

    auto standDown = [&](const char* why) {
        if (s_active) {
            s_active = false;
            DebugLogger::LogFormat("Gameplay Snake's eyes: OFF (%s) at view frame %llu", why, frame);
        }
        s_lastWhyNot = why;
        return false;
    };

    if (!g_gpPov) return false;
    // Layer 3 (the gameplay camera, +53C5C, and any set-piece camera that
    // commits through the same pair) -- and, round 12, the SCRIPTED camera task
    // at +226DC9 (view built at +226DE3 from its own pair at work+0x20/+0x28,
    // into the same main view 0x6BC36C, after the gameplay camera). That one
    // runs the in-engine scenes the demo system does not: the escape start
    // with Meryl ("Hurry!!", "I'll drive!"), the truck, and so on. It was left
    // alone, so those scenes showed the game's own shot (flat) with Snake
    // hidden by the game. Now they are rebuilt from Snake's head too.
    const bool scripted = (callerRva == 0x226DE3u);
    static unsigned long long s_scriptedTick = 0;
    if (scripted) s_scriptedTick = GetTickCount64();
    const bool scriptedRecent = s_scriptedTick && GetTickCount64() - s_scriptedTick < 250;
    if (!scripted && (fromPtr != (uint32_t)(g_moduleBase + 0x593F60) ||
        toPtr != (uint32_t)(g_moduleBase + 0x593F68))) return false;
    {
        static bool s_scriptLogged = false;
        if (scripted && !s_scriptLogged) {
            s_scriptLogged = true;
            DebugLogger::Log("Gameplay Snake's eyes: the game's scripted-scene camera (+226DE3) is running -- "
                "rebuilding it from Snake's head as well.");
        }
    }
    if (!g_vrViewMode.load(std::memory_order_relaxed) || !g_enabled.load(std::memory_order_relaxed) ||
        !g_havePose.load(std::memory_order_relaxed)) return standDown("native mode / no headset pose");
    if (g_thirdPersonVr) return standDown("third_person_vr owns the third-person camera");
    // A REAL cutscene runs the demo (FrameRunDemo, +1CD292, stamps
    // g_csDemoTick every frame). Some set pieces stop the first-person camera
    // routine for the whole fight with no demo at all -- Liquid on top of REX,
    // the jeep escape -- and the stall detector calls those "cutscene" too
    // (2026-10-02 log: camera update STALLED for 900+ flips, modal 0, no Demo
    // model table). Those are gameplay: Snake's eyes takes them.
    const bool demoRunning = GetTickCount64() - g_csDemoTick.load(std::memory_order_relaxed) < 500;
    if (g_csScreenFallback.load(std::memory_order_relaxed) ||
        (g_csvDetected.load(std::memory_order_relaxed) && demoRunning))
        return standDown("cutscene");
    // Modal 2 shows up through the jeep escape, flickering with 0 (2026-10-02
    // log) -- not a menu. Standing down on it flipped the view between Snake's
    // eyes and the game camera every few frames. Only pause (1) and the
    // inventory (4) are menus.
    {
        const int modal = g_csvModalState.load(std::memory_order_relaxed);
        if (modal == 1 || modal == 4) return standDown("menu / inventory");
    }

    uint8_t scriptCam = 0;
    SafeReadBytes(&scriptCam, (const uint8_t*)(g_moduleBase + 0x593FF9), 1);
    // First person only "works" if the game's first-person camera routine is
    // actually running: on top of REX the game's FPV flag is 1 for the whole
    // fight while that routine never runs (2026-10-02 log, 20:02:19..20:04:21:
    // fpv_flag=1, camera update STALLED), so rotate2 drives nothing.
    const bool rotAlive = GetTickCount64() - g_rotHookTick.load(std::memory_order_relaxed) < 150;
    const bool fpvWorks = g_lastFpvState && rotAlive && (scriptCam & 3) == 0;
    if (fpvWorks) {
        // In a vehicle the game's own first person is built from the on-foot
        // object's stale spot -- the jeep's "under the map" view (2026-10-02,
        // twice; R3 out and back in fixed it). If Snake on foot is not the
        // player right now, keep Snake's eyes on the vehicle regardless.
        // (2026-10-02 round 8: in the escape jeep 0x334228 still pointed at a
        // leftover sna_init from the REX fight, up at y=11693 -- "the camera
        // is way above the map". The vehicle is checked first now.)
        uint32_t vo = 0, vobjs = 0;
        const bool inVehicle = GpPovFindVehicleSnake(&vo, &vobjs);
        // A scripted scene camera overrides first person after it; keep
        // Snake's eyes up for it (and do not flip OFF/ON for the gameplay
        // camera's own call in the same frame).
        if (!inVehicle && !scripted) {
            if (scriptedRecent && s_active) return false;
            return standDown("first person");
        }
    }
    // The view-commit hook drives "cutscenes" with head look on the director's
    // pair. Without a demo it is one of the set pieces above, and this
    // replaces its pair outright, so its write is simply superseded.
    if (g_vcDroveThisBuild.load(std::memory_order_relaxed) && demoRunning)
        return standDown("view commit hook drove this frame");

    // --- the player object -----------------------------------------------------
    // Snake on foot is chara\snake\sna_init.c, pointed to by 0x334228. In a
    // vehicle he is a different object (Takabe\jeep_sne, pkjp_sne, ...): found
    // by name in the task lists, its DG_OBJS located by shape (and by his
    // model definition when it matches). Those have no known facing field, so
    // the view starts facing the way the game's own camera looks.
    int32_t obj = 0, objs = 0;
    bool onFoot = true;
    int16_t nJoints = 0, ax = 0, ay = 0, az = 0, rotY = 0;
    int32_t hx = 0, hy = 0, hz = 0;
    uint32_t vehObj = 0, vehObjs = 0;
    if (GpPovFindVehicleSnake(&vehObj, &vehObjs)) {
        // A riding Snake (jeep, REX top) wins over whatever 0x334228 holds.
        onFoot = false;
        obj = (int32_t)vehObj; objs = (int32_t)vehObjs;
        g_gpPovVehicleTick.store(GetTickCount64(), std::memory_order_relaxed);
    }
    else if (!SafeRead32(g_moduleBase + kPlayerObjPtrRva, &obj) ||
        (uint32_t)obj < 0x00400000u || (uint32_t)obj >= 0x7FFF0000u || !GpPovNameIsSnake((uint32_t)obj) ||
        !SafeRead32((uintptr_t)(uint32_t)obj + 0x9C, &objs) ||
        (uint32_t)objs < 0x00400000u || (uint32_t)objs >= 0x7FFF0000u) {
        GpPovLogClass5Names("no live player object");
        return standDown("no live player object (sna_init / *_sne)");
    }
    // 2026-10-04 (rappel): stepping onto the rope frees sna_init, but its
    // memory stays readable -- name and all -- so for a moment the view was
    // built from a dead Snake somewhere else entirely (y=-28896, log 14:16:57).
    // Same cure as the vehicle cache: it has to be linked in the live lists.
    if (onFoot) {
        static uint32_t s_linkObj = 0;
        static unsigned long long s_linkTick = 0;
        const unsigned long long t = GetTickCount64();
        if ((uint32_t)obj != s_linkObj || t - s_linkTick >= 300) {
            if (!GpPovTaskLinked((uint32_t)obj)) {
                s_linkObj = 0;
                return standDown("player object freed (no longer in the game's task lists)");
            }
            s_linkObj = (uint32_t)obj;
            s_linkTick = t;
        }
    }
    const bool rope = !onFoot && g_gpPovVehicleKind.load(std::memory_order_relaxed) == 3;
    const bool torture = !onFoot && g_gpPovVehicleKind.load(std::memory_order_relaxed) == 4;
    const bool eyeAtHead = onFoot || rope || torture;   // rope / rack: at his head, as far as the eye goes
    if (!SafeRead16((uintptr_t)(uint32_t)objs + 0x2E, &nJoints) || nJoints <= 6 ||
        !SafeRead32((uintptr_t)(uint32_t)objs + 0x284, &hx) || !SafeRead32((uintptr_t)(uint32_t)objs + 0x288, &hy) ||
        !SafeRead32((uintptr_t)(uint32_t)objs + 0x28C, &hz))
        return standDown("player head unreadable");
    if (onFoot) {
        if (!SafeRead16((uintptr_t)(uint32_t)obj + 0x20, &ax) || !SafeRead16((uintptr_t)(uint32_t)obj + 0x22, &ay) ||
            !SafeRead16((uintptr_t)(uint32_t)obj + 0x24, &az) || !SafeRead16((uintptr_t)(uint32_t)obj + 0x2A, &rotY))
            return standDown("player head unreadable");
    }
    else {
        int32_t rx = 0, ry = 0, rz = 0;      // model root world translation
        if (!SafeRead32((uintptr_t)(uint32_t)objs + 0x14, &rx) || !SafeRead32((uintptr_t)(uint32_t)objs + 0x18, &ry) ||
            !SafeRead32((uintptr_t)(uint32_t)objs + 0x1C, &rz))
            return standDown("player head unreadable");
        ax = (int16_t)rx; ay = (int16_t)ry; az = (int16_t)rz;
    }
    // A head sits within a couple of metres of the body's origin.
    if (std::abs(hx - ax) > 2500 || std::abs(hy - ay) > 2500 || std::abs(hz - az) > 2500 ||
        (hx == 0 && hy == 0 && hz == 0))
        return standDown("head joint not posed");

    float bodyYaw = NormalizeRad(((float)(((int)rotY) & 0xFFF)) * (2.0f * kPi / 4096.0f) +
        (float)g_csPovYawOffsetDeg * kPi / 180.0f);
    // Facing from the model's own root matrix (DG_OBJS.world at +0, 3x3
    // int16, 4096 = 1). Model forward is +Z, so world forward is column 2:
    // (m02, m12, m22) -- yaw = ratan2(m02, m22), the same convention as rotY.
    auto rootYaw = [&](float* out) {
        int16_t m02 = 0, m22 = 0;
        if (!SafeRead16((uintptr_t)(uint32_t)objs + 0x04, &m02) || !SafeRead16((uintptr_t)(uint32_t)objs + 0x10, &m22))
            return false;
        if (std::abs((int)m02) + std::abs((int)m22) < 1024) return false;   // lying flat / not a rotation
        *out = std::atan2((float)m02, (float)m22);
        return true;
    };
    if (onFoot) {
        // One-time check that the matrix reading agrees with sna_init's rotY
        // (it decides where the torture view starts).
        static int s_rootCheck = 0;
        float ry = 0.0f;
        if (s_rootCheck < 3 && rootYaw(&ry)) {
            s_rootCheck++;
            DebugLogger::LogFormat("Gameplay Snake's eyes: root-matrix facing check -- matrix %.1f deg vs rotY %.1f deg "
                "(should match; the torture rack uses the matrix)", ry * 180.0f / kPi,
                ((float)(((int)rotY) & 0xFFF)) * 360.0f / 4096.0f);
        }
    }
    if (!onFoot) {
        ViewPoint gf{}, gt{};
        float ry = 0.0f;
        if (torture && rootYaw(&ry))
            bodyYaw = NormalizeRad(ry + (float)g_gpPovTortureYawOffsetDeg * kPi / 180.0f);
        else if (ReadViewPoint((uintptr_t)fromPtr, &gf) && ReadViewPoint((uintptr_t)toPtr, &gt) &&
            (gt.x != gf.x || gt.z != gf.z))
            bodyYaw = std::atan2((float)(gt.x - gf.x), (float)(gt.z - gf.z));
    }

    // --- head pose, one consistent XR sample ------------------------------------
    HeadViewSnap snap;
    AcquireSRWLockShared(&g_headViewLock);
    snap = g_headViewSnap;
    ReleaseSRWLockShared(&g_headViewLock);
    if (!snap.have) return standDown("no head snapshot yet");
    const float hYaw = snap.yaw;
    float hPitch = snap.pitch;
    const float hPosX = (snap.eye[0].px + snap.eye[1].px) * 0.5f;
    const float hPosY = (snap.eye[0].py + snap.eye[1].py) * 0.5f;
    const float hPosZ = (snap.eye[0].pz + snap.eye[1].pz) * 0.5f;
    if (g_csPovInvertPitch) hPitch = -hPitch;

    const unsigned long long nowTick = GetTickCount64();
    const bool recenter = s_active && g_vcRecenter.exchange(false, std::memory_order_relaxed);
    static uint32_t s_lastObj = 0;
    const bool newObj = (uint32_t)obj != s_lastObj;
    s_lastObj = (uint32_t)obj;
    if (!s_active || nowTick - s_lastTick > 400 || recenter || newObj) {
        s_refYaw = hYaw;
        s_anchorYaw = bodyYaw;
        s_userYaw = 0.0f;
        g_gpPovYawAdd.store(0.0f, std::memory_order_relaxed);
        if (g_fpvHold.load(std::memory_order_relaxed) &&
            g_fpvHoldAnchorPending.exchange(false, std::memory_order_relaxed)) {
            s_anchorYaw = g_fpvHoldAnchorYaw.load(std::memory_order_relaxed);
            DebugLogger::LogFormat("Gameplay Snake's eyes: wall press -- view kept where you were looking "
                "(%.1f deg), not where Snake's body turns.", s_anchorYaw * 180.0f / kPi);
        }
        s_refPosX = hPosX; s_refPosY = hPosY; s_refPosZ = hPosZ;
        DebugLogger::LogFormat(
            "Gameplay Snake's eyes: ON at view frame %llu (%s; fpv flag %d, script camera %d) -- Snake at "
            "(%d,%d,%d) facing %.1f deg, head at (%d,%d,%d).", frame,
            recenter ? "recentre" : (s_lastWhyNot ? s_lastWhyNot : "start"),
            g_lastFpvState ? 1 : 0, (int)(scriptCam & 3), ax, ay, az, bodyYaw * 180.0f / kPi, hx, hy, hz);
    }
    s_active = true;
    s_lastTick = nowTick;

    const bool holdView = g_fpvHold.load(std::memory_order_relaxed);
    s_userYaw = NormalizeRad(s_userYaw + g_gpPovYawAdd.exchange(0.0f, std::memory_order_relaxed));
    const float baseYaw = NormalizeRad(((g_gpPovFollowBody && onFoot && !holdView) ? bodyYaw : s_anchorYaw) + s_userYaw);
    const float yaw = NormalizeRad(baseYaw + NormalizeRad(hYaw - s_refYaw));
    float pitch = hPitch;
    const float kPitchLimit = 1.4835f;              // 85 degrees
    if (pitch > kPitchLimit)  pitch = kPitchLimit;
    if (pitch < -kPitchLimit) pitch = -kPitchLimit;
    const float cp = std::cos(pitch), sp = std::sin(pitch);
    const float sy = std::sin(yaw), cy = std::cos(yaw);
    const float fwdX = cp * sy, fwdY = sp, fwdZ = cp * cy;

    // --- 6DOF lean, measured in the room and laid onto the view's base yaw -----
    float ox = 0.0f, oy = 0.0f, oz = 0.0f;
    if (g_gpPovPosition && g_positionEnabled) {
        const float dxr = hPosX - s_refPosX, dyr = hPosY - s_refPosY, dzr = hPosZ - s_refPosZ;
        const float fx = -std::sin(s_refYaw), fz = -std::cos(s_refYaw);
        const float rx = std::cos(s_refYaw), rz = -std::sin(s_refYaw);
        const float dF = dxr * fx + dzr * fz;
        const float dR = dxr * rx + dzr * rz;
        const float ay2 = std::sin(baseYaw), ac2 = std::cos(baseYaw);
        const float lim = (float)g_maxPositionOffset;
        auto clampf = [lim](float v) { return v > lim ? lim : (v < -lim ? -lim : v); };
        ox = clampf((dF * ay2 + dR * -ac2) * g_positionScale);
        oz = clampf((dF * ac2 + dR * ay2) * g_positionScale);
        oy = clampf(dyr * g_positionScale);
    }

    // On foot: just in front of the face. In a vehicle: behind and above the
    // head, along the base yaw (not the head yaw, so looking around does not
    // swing the eye), so the mounted gun sits out in front of you.
    const float bsy = std::sin(baseYaw), bcy = std::cos(baseYaw);
    // On the rope: at the head like on foot, but nearer it -- his face is a
    // hand's width from the wall.
    const float push = onFoot  ? (float)g_gpPovForward
                     : rope    ? (float)g_gpPovRopeForward
                     : torture ? (float)g_gpPovTortureForward
                               : -(float)g_gpPovVehBack;
    const float lift = eyeAtHead ? 0.0f : (float)g_gpPovVehUp;
    const ViewPoint newFrom{
        (int32_t)std::lround((float)hx + (eyeAtHead ? sy : bsy) * push + ox),
        (int32_t)std::lround((float)hy + lift + oy),
        (int32_t)std::lround((float)hz + (eyeAtHead ? cy : bcy) * push + oz) };
    const ViewPoint newTo{
        newFrom.x + (int32_t)std::lround(fwdX * 1024.0f),
        newFrom.y + (int32_t)std::lround(fwdY * 1024.0f),
        newFrom.z + (int32_t)std::lround(fwdZ * 1024.0f) };
    ViewPoint gameFrom{}, gameTo{};
    ReadViewPoint((uintptr_t)fromPtr, &gameFrom);
    ReadViewPoint((uintptr_t)toPtr, &gameTo);
    const bool wrote = WriteViewPoint((uintptr_t)fromPtr, newFrom) && WriteViewPoint((uintptr_t)toPtr, newTo);
    if (!wrote) return standDown("layer-3 write failed");
    g_gpPovViewYaw.store(yaw, std::memory_order_relaxed);
    {
        auto u16 = [](int32_t v) { return (uint64_t)(uint16_t)(int16_t)v; };
        g_gpPovPairA.store(u16(newFrom.x) | (u16(newFrom.y) << 16) | (u16(newFrom.z) << 32) | (u16(newTo.x) << 48),
            std::memory_order_relaxed);
        g_gpPovPairB.store(u16(newTo.y) | (u16(newTo.z) << 16), std::memory_order_relaxed);
        g_gpPovPairTick.store(GetTickCount64(), std::memory_order_relaxed);
        g_gpPovVehObj.store(onFoot ? 0u : (uint32_t)obj, std::memory_order_relaxed);
        g_gpPovVehObjs.store(onFoot ? 0u : (uint32_t)objs, std::memory_order_relaxed);
    }
    if (!onFoot && g_gpPovRexFaceView && g_gpPovVehicleKind.load(std::memory_order_relaxed) == 2 &&
        !g_moveStickActive.load(std::memory_order_relaxed)) {
        static int s_ctlState = 0;      // 0 unknown, 1 verified, -1 refused
        static uint32_t s_ctlObj = 0;
        if ((uint32_t)obj != s_ctlObj) { s_ctlObj = (uint32_t)obj; s_ctlState = 0; }
        if (s_ctlState == 0) {
            int16_t mx = 0, mz = 0;
            const bool ok = SafeRead16((uintptr_t)(uint32_t)obj + 0x20, &mx) && SafeRead16((uintptr_t)(uint32_t)obj + 0x24, &mz) &&
                std::abs((int)mx - (int)ax) < 300 && std::abs((int)mz - (int)az) < 300;
            s_ctlState = ok ? 1 : -1;
            DebugLogger::LogFormat("REX top: Snake's CONTROL at +0x20 %s (mov %d,%d vs model %d,%d) -- %s", ok ? "matches" : "does NOT match",
                mx, mz, ax, az, ok ? "he turns to face where you look while the left stick is idle"
                                   : "not touching his facing");
        }
        if (s_ctlState == 1) {
            const int16_t u = (int16_t)(((int)std::lround(yaw * 4096.0f / (2.0f * kPi))) & 0xFFF);
            SafeWrite16((uintptr_t)(uint32_t)obj + 0x2A, u);
            SafeWrite16((uintptr_t)(uint32_t)obj + 0x6E, u);
        }
    }
    if (gameTo.x != gameFrom.x || gameTo.z != gameFrom.z) {
        g_gpPovGameYaw.store(std::atan2((float)(gameTo.x - gameFrom.x), (float)(gameTo.z - gameFrom.z)),
            std::memory_order_relaxed);
        g_gpPovGameYawOk.store(true, std::memory_order_relaxed);
    }

    int lens = 0;
    {
        int32_t argFov = 0;
        SafeRead32((uintptr_t)(stack + 4), &argFov);
        lens = argFov;
    }
    if (g_gpPovFov && g_fovClipDistance > 0) {
        // The builder takes the lens as its 4th argument; layer 3's fov field
        // is what the flip reads back as "the lens this frame used".
        SafeWrite32((uintptr_t)(stack + 4), (uint32_t)g_fovClipDistance);
        SafeWrite32(g_moduleBase + kViewFovRva, (uint32_t)g_fovClipDistance);
        lens = g_fovClipDistance;
    }

    // Both eyes get this image (unless render twice is drawing a true pair
    // around it, which it does on its own when first person is live).
    g_publishedEye.store(2, std::memory_order_relaxed);
    g_eyePublishTick.store(nowTick, std::memory_order_relaxed);

    if (g_frameRecordEnabled) {
        FrameViewRecord r;
        r.valid = true;
        r.eye[0] = snap.eye[0];
        r.eye[1] = snap.eye[1];
        r.yawEq = snap.yaw;
        r.pitchEq = g_csPovInvertPitch ? -pitch : pitch;
        r.rollEq = 0.0f;
        r.baseYaw = 0;
        r.basePitch = 0;
        r.kYaw = 1;
        r.kPitch = 1;
        r.clip = (lens >= 64 && lens <= 4000) ? lens : 0;
        r.tick = nowTick;
        { LARGE_INTEGER q; QueryPerformanceCounter(&q); r.qpc = q.QuadPart; }
        r.pov = true;
        AcquireSRWLockExclusive(&g_frameRecLock);
        g_frameRec = r;
        g_frameRecRun = g_rotHookRuns.load(std::memory_order_relaxed);
        g_lastValidRec = r;
        ReleaseSRWLockExclusive(&g_frameRecLock);
    }

    g_gpPovTick.store(nowTick, std::memory_order_relaxed);
    if (g_gpPovHideBody) {
        g_renderHideObjs[0].store((uint32_t)objs, std::memory_order_relaxed);
        g_renderHideTick[0].store(nowTick, std::memory_order_relaxed);
    }

    if (s_logLines < g_gpPovLogLines && (frame % (unsigned)g_gpPovLogEvery) == 0) {
        s_logLines++;
        DebugLogger::TestLogFormat(
            "GpPov[%llu]: snake=(%d,%d,%d) head=(%d,%d,%d) body=%.1fdeg | view yaw=%.1f pitch=%.1f lean=(%d,%d,%d) "
            "lens=%d | FROM=(%d,%d,%d) TO=(%d,%d,%d) | game was (%d,%d,%d)->(%d,%d,%d) fpv=%d script=%d pad=%.1f %s",
            frame, ax, ay, az, hx, hy, hz, bodyYaw * 180.0f / kPi, yaw * 180.0f / kPi, pitch * 180.0f / kPi,
            (int)ox, (int)oy, (int)oz, lens, newFrom.x, newFrom.y, newFrom.z, newTo.x, newTo.y, newTo.z,
            gameFrom.x, gameFrom.y, gameFrom.z, gameTo.x, gameTo.y, gameTo.z,
            g_lastFpvState ? 1 : 0, (int)(scriptCam & 3), [&]() { float pf = 0; return GetGamePadFrameYaw(&pf) ? pf * 180.0f / kPi : -999.0f; }(),
            onFoot ? "foot" : (rope ? "rope" : (torture ? "torture" : "vehicle")));
    }
    return true;
}

// ===========================================================================
// VIEW BUILD BODY -- the chokepoint, at RVA 0x1C22.
//
// Every camera path in the game ends here. `stack` points at the untouched
// call frame: [0] return address, [1] view struct, [2] FROM*, [3] TO*,
// [4] fov. FROM and TO are three int16 each, same axis convention and units
// as everything else in this file.
// ===========================================================================
extern "C" void __cdecl Mgs1ViewBuildBody(uint32_t* stack) {
    if (g_moduleBase == 0 || stack == nullptr) return;

    const unsigned long long frame =
        g_vbFrames.fetch_add(1, std::memory_order_relaxed) + 1;

    uint32_t ret = 0, fromPtr = 0, toPtr = 0;
    if (!SafeRead32((uintptr_t)(stack + 0), (int32_t*)&ret)) return;
    if (!SafeRead32((uintptr_t)(stack + 2), (int32_t*)&fromPtr)) return;
    if (!SafeRead32((uintptr_t)(stack + 3), (int32_t*)&toPtr)) return;
    if (fromPtr == 0 || toPtr == 0) return;

    const unsigned callerRva =
        (ret > (uint32_t)g_moduleBase) ? (unsigned)(ret - (uint32_t)g_moduleBase) : 0;

    // --- name each camera path once ----------------------------------------
    // The single most useful line this build produces. 26 call sites reach
    // here; this says which ones actually run, and when. A caller that appears
    // only while a cutscene is playing IS the cutscene camera path.
    bool isNewCaller = false;
    if (g_vbSeenCount < kVbMaxCallers) {
        isNewCaller = true;
        for (int i = 0; i < g_vbSeenCount; ++i) {
            if (g_vbSeenCallers[i] == callerRva) { isNewCaller = false; break; }
        }
        if (isNewCaller) {
            g_vbSeenCallers[g_vbSeenCount++] = callerRva;
            if (g_vbSeenCount <= g_vbLogCallers) {
                DebugLogger::LogFormat(
                    "View build: NEW camera path -- called from mgsi.exe+%X (frame %llu). "
                    "from=%08X to=%08X | fpv=%d cutscene=%d modal=%d",
                    callerRva, frame, fromPtr, toPtr,
                    g_lastFpvState ? 1 : 0,
                    g_csvDetected.load(std::memory_order_relaxed) ? 1 : 0,
                    g_csvModalState.load(std::memory_order_relaxed));
            }
        }
    }

    // --- THE SNAKE PROBE -----------------------------------------------------
    // Logged in BOTH gameplay and cutscenes, in one format, so the two are
    // directly comparable. The question is not "does 593FA0 move" -- it always
    // moves -- but "does it move with SNAKE or with the CAMERA". Printing it
    // next to the from/to pair this path just handed the renderer answers that
    // in one glance: if 593FA0 sits on the pair, it is the camera; if it wanders
    // independently while the pair holds a composed shot, it is Snake.
    {
        static int s_snakeLog = 0;
        static unsigned long long s_lastProbeFrame = 0;
        if (g_vbSnakeProbe && s_snakeLog < g_vbSnakeLines &&
            (frame - s_lastProbeFrame) >= (unsigned long long)g_vbSnakeEvery) {
            s_lastProbeFrame = frame;
            s_snakeLog++;
            ViewPoint camWork{}, tpvTgt{}, pFrom{}, pTo{};
            const bool okA = ReadViewPoint(g_moduleBase + kPosXOffset, &camWork);
            const bool okB = ReadViewPoint(g_moduleBase + kTpvTargetRva, &tpvTgt);
            const bool okC = ReadViewPoint((uintptr_t)fromPtr, &pFrom);
            const bool okD = ReadViewPoint((uintptr_t)toPtr, &pTo);
            int16_t branch = 0;
            const bool okE = SafeRead16(g_moduleBase + kFpvBranchRva, &branch);
            DebugLogger::TestLogFormat(
                "Snake?[%llu] cs=%d fpv=%d 594002=%d caller=+%X | "
                "593FA0=(%d,%d,%d)%s 593FA8=(%d,%d,%d)%s | "
                "path from=(%d,%d,%d)%s to=(%d,%d,%d)%s | dFROM=(%d,%d,%d) dTO=(%d,%d,%d)",
                frame,
                g_csvDetected.load(std::memory_order_relaxed) ? 1 : 0,
                g_lastFpvState ? 1 : 0,
                okE ? (int)branch : -1,
                callerRva,
                camWork.x, camWork.y, camWork.z, okA ? "" : "?",
                tpvTgt.x, tpvTgt.y, tpvTgt.z, okB ? "" : "?",
                pFrom.x, pFrom.y, pFrom.z, okC ? "" : "?",
                pTo.x, pTo.y, pTo.z, okD ? "" : "?",
                // 593FA0 minus each end of the pair. A near-zero row is the
                // camera; a large, drifting row is something else -- and
                // something else, in this struct, is Snake.
                camWork.x - pFrom.x, camWork.y - pFrom.y, camWork.z - pFrom.z,
                camWork.x - pTo.x, camWork.y - pTo.y, camWork.z - pTo.z);
        }
    }

    if (DriveGameplayPov(callerRva, fromPtr, toPtr, stack, frame)) {
        g_vcDroveThisBuild.store(false, std::memory_order_relaxed);
        return;
    }

    // --- PSG1 LENS, at the point of use (2026-10-04) -------------------------
    // Test: "the scope is small with no zoom; randomly it is large and zoomed,
    // very briefly". Log: the scope wrote clip_distance 1100, but the frames
    // were drawn at 1466 -- rifle.c adds a third to the lens every frame while
    // the PSG1 is out (scope down: 120 became 160 the same way). The headset
    // only magnifies a frame whose lens is exactly the scope's (1100), so the
    // 1466 frames went out at their true 12.5 degrees -- a small, unmagnified
    // picture -- and only the odd frame that escaped the +1/3 was shown big.
    // The lens handed to the view builder is the last word (the chanl's clip,
    // the sky, the frame record and the headset claim all follow it), so it is
    // set here, after rifle.c: scope up = the scope's zoom, PSG1 down = the
    // mod's normal first-person lens. First person, VR mode, layer-3 pair only.
    if (g_scopeEnabled && g_lastFpvState && fromPtr == (uint32_t)(g_moduleBase + 0x593F60) &&
        g_vrViewMode.load(std::memory_order_relaxed) && g_enabled.load(std::memory_order_relaxed)) {
        int16_t weaponId = -1;
        SafeRead16(g_moduleBase + 0x38E7FC, &weaponId);              // GM_CurrentWeaponId
        int want = 0;
        if (weaponId == 9 && g_scopeActive.load(std::memory_order_relaxed)) want = g_scopeClip;
        else if (weaponId == 9 && g_fovClipDistance > 0) want = g_fovClipDistance;
        if (want > 0) {
            int32_t argFov = 0;
            SafeRead32((uintptr_t)(stack + 4), &argFov);
            if (argFov != want) {
                SafeWrite32((uintptr_t)(stack + 4), (uint32_t)want);
                SafeWrite32(g_moduleBase + kViewFovRva, (uint32_t)want);
                static int s_psgLensLog = 0;
                if (s_psgLensLog < 8) {
                    s_psgLensLog++;
                    DebugLogger::LogFormat("PSG1 lens: the game handed the view builder %d, set to %d (%s)",
                        (int)argFov, want, want == g_scopeClip ? "scope up -- zoomed and magnified"
                                                                 : "scope down -- normal first-person lens");
                }
            }
        }
    }

    const bool vrMode = g_vrViewMode.load(std::memory_order_relaxed);
    const bool hookEnabled = g_enabled.load(std::memory_order_relaxed);
    const bool havePose = g_havePose.load(std::memory_order_relaxed);

    bool drive = (g_vbMode != 0) && vrMode && hookEnabled && havePose;
    if (drive && g_vbMode == 1) {
        drive = g_csvDetected.load(std::memory_order_relaxed);
    }
    // Never fight the layer-3 hook: if that one is driving this frame, the
    // pair we are looking at already carries its write. One writer per value
    // per frame, across two hooks in the same call chain.
    if (g_vcDroveThisBuild.exchange(false, std::memory_order_relaxed)) drive = false;
    if (drive && g_vbCallerRva != 0 && callerRva != g_vbCallerRva) drive = false;

    if (!drive) {
        g_vbDriving.store(false, std::memory_order_relaxed);
        return;
    }

    ViewPoint from{}, to{};
    if (!ReadViewPoint((uintptr_t)fromPtr, &from) || !ReadViewPoint((uintptr_t)toPtr, &to)) {
        static int badRead = 0;
        if (badRead < 5) {
            badRead++;
            DebugLogger::LogFormat("View build: caller +%X handed us unreadable points -- skipping",
                callerRva);
        }
        g_vbDriving.store(false, std::memory_order_relaxed);
        return;
    }

    char who[16];
    _snprintf_s(who, sizeof(who), _TRUNCATE, "VB+%X", callerRva);
    const bool wrote = DriveViewPair((uintptr_t)fromPtr, (uintptr_t)toPtr,
        from, to, frame, who, 1, nullptr);
    g_vbDriving.store(wrote, std::memory_order_relaxed);
}


// ===========================================================================
// TRANSFORM BODY -- RVA 0x7ADA. Runs for EVERY object the engine draws.
//
// Hot path. It does nothing at all unless a cutscene is in progress and the
// log is enabled, and even then it is a bounded linear scan of 28 slots plus
// one guarded 6-byte read. Anything heavier here would cost frames.
// ===========================================================================
extern "C" void __cdecl Mgs1XformBody(uint32_t* stack) {
    if (!g_xformScan || stack == nullptr) return;
    if (!g_csvDetected.load(std::memory_order_relaxed)) return;

    uint32_t ret = 0, trans = 0;
    if (!SafeRead32((uintptr_t)(stack + 0), (int32_t*)&ret)) return;
    if (!SafeRead32((uintptr_t)(stack + 1), (int32_t*)&trans)) return;
    if (trans < 0x00400000u || trans >= 0x7FFF0000u) return;

    uint8_t v[6];
    if (!SafeReadBytes(v, (const uint8_t*)(uintptr_t)trans, 6)) return;
    const int32_t x = (int32_t)(int16_t)(v[0] | (v[1] << 8));
    const int32_t y = (int32_t)(int16_t)(v[2] | (v[3] << 8));
    const int32_t z = (int32_t)(int16_t)(v[4] | (v[5] << 8));

    const uint32_t rva = (ret > (uint32_t)g_moduleBase) ? (ret - (uint32_t)g_moduleBase) : 0u;
    g_xformCalls.fetch_add(1, std::memory_order_relaxed);

    // Per-caller tally, so the census of "who drew how many" survives.
    {
        const int nc = g_xformCallerCount.load(std::memory_order_relaxed);
        int i = 0;
        for (; i < nc && i < kXformCallers; ++i) {
            if (g_xformCallers[i].rva == rva) { g_xformCallers[i].count++; break; }
        }
        if (i == nc && nc < kXformCallers) {
            g_xformCallers[nc].rva = rva;
            g_xformCallers[nc].count = 1;
            g_xformCallerCount.store(nc + 1, std::memory_order_relaxed);
        }
    }

    // Rank by distance to the shot's look-at point. Integer maths only: this
    // runs hundreds of times a frame.
    const int32_t dx = g_xformToX.load(std::memory_order_relaxed) - x;
    const int32_t dy = g_xformToY.load(std::memory_order_relaxed) - y;
    const int32_t dz = g_xformToZ.load(std::memory_order_relaxed) - z;
    const int64_t d2 = (int64_t)dx * dx + (int64_t)dy * dy + (int64_t)dz * dz;
    int32_t d = 0;
    { int64_t r = 0, bit = (int64_t)1 << 30;      // integer sqrt
      int64_t n = d2;
      while (bit > n) bit >>= 2;
      while (bit) { if (n >= r + bit) { n -= r + bit; r = (r >> 1) + bit; } else r >>= 1; bit >>= 2; }
      d = (int32_t)r; }

    int used = g_xformUsed.load(std::memory_order_relaxed);
    if (used >= kXformSlots && d >= g_xformSlots[kXformSlots - 1].dist) return;
    int slot = (used < kXformSlots) ? used : (kXformSlots - 1);
    while (slot > 0 && g_xformSlots[slot - 1].dist > d) {
        g_xformSlots[slot] = g_xformSlots[slot - 1];
        --slot;
    }
    g_xformSlots[slot].callerRva = rva;
    g_xformSlots[slot].x = x; g_xformSlots[slot].y = y; g_xformSlots[slot].z = z;
    g_xformSlots[slot].dist = d;
    g_xformSlots[slot].count = 1;
    if (used < kXformSlots) g_xformUsed.store(used + 1, std::memory_order_relaxed);
}

// Dumps and clears the transform table. Called from the cutscene camera body,
// which runs once per shot frame, so each dump covers one frame's drawing.
static void ReportTransforms(const ViewPoint& from, const ViewPoint& to, unsigned long long frame) {
    if (!g_xformScan) return;
    static int s_lines = 0;
    const unsigned every = (unsigned)(g_xformEvery < 1 ? 1 : g_xformEvery);
    if ((frame % every) != 0) return;
    const int used = g_xformUsed.load(std::memory_order_relaxed);
    if (s_lines >= g_xformLines) { g_xformUsed.store(0, std::memory_order_relaxed); return; }
    s_lines++;

    const int nc = g_xformCallerCount.load(std::memory_order_relaxed);
    char who[400];
    int ww = 0;
    who[0] = '\0';
    for (int i = 0; i < nc && i < kXformCallers; ++i) {
        const int n = _snprintf_s(who + ww, sizeof(who) - ww, _TRUNCATE,
            "%s+%X x%u", ww ? " " : "", g_xformCallers[i].rva, g_xformCallers[i].count);
        if (n > 0) ww += n;
    }
    DebugLogger::TestLogFormat(
        "Xform[%llu]: %u calls | callers: %s | FROM=(%d,%d,%d) TO=(%d,%d,%d) -- nearest TO first",
        frame, g_xformCalls.load(std::memory_order_relaxed), who,
        from.x, from.y, from.z, to.x, to.y, to.z);
    char line[900];
    int w = 0;
    line[0] = '\0';
    for (int i = 0; i < used && i < kXformSlots; ++i) {
        const int n = _snprintf_s(line + w, sizeof(line) - w, _TRUNCATE,
            "%sd=%d +%X (%d,%d,%d)", w ? "  " : "",
            g_xformSlots[i].dist, g_xformSlots[i].callerRva,
            g_xformSlots[i].x, g_xformSlots[i].y, g_xformSlots[i].z);
        if (n > 0) w += n;
        if ((i + 1) % 4 == 0) { DebugLogger::TestLogFormat("  Xform: %s", line); w = 0; line[0] = '\0'; }
    }
    if (w > 0) DebugLogger::TestLogFormat("  Xform: %s", line);
    g_xformUsed.store(0, std::memory_order_relaxed);
    g_xformCalls.store(0, std::memory_order_relaxed);
    g_xformCallerCount.store(0, std::memory_order_relaxed);
}

// ===========================================================================
// SNAKE'S EYES -- the cutscene seen from Snake's head, not the director's.
//
// Called at the top of FrameRunDemo (+1CD292), before the game reads this
// frame's DMO_DAT. Everything it needs is in the two arguments:
//
//   * which demo model is Snake     -> act->header->models[i].filename
//   * where the scene puts him NOW  -> this frame's DMO_ADJ for that model
//   * where his head is relative to that point -> last frame's joint-6 world
//     matrix, which ShowScene/GM_ActObject computed one frame ago
//
// and then it rewrites the frame's eye, look-at, roll and lens before the
// game builds the view from them. The director's framing is discarded on
// purpose: only the player's head turns the view (no forced rotation), you
// start facing where Snake faces, and you re-face only when the scene moves
// him somewhere new -- which is already a hard cut, so nothing is lost.
//
// WORLD AXES, derived rather than assumed: MGS1's world is Y-UP and
// right-handed (sna_init.c lifts the eye by +320 when Snake is lying down;
// FrameRunDemo builds the camera with a 180-degree roll, which is what maps a
// Y-up world onto the PSX's Y-down screen, and mirrors X with it). So a view
// yaw a = ratan2(dx, dz) looks along (sin a, 0, cos a), screen-right is
// (-cos a, 0, sin a), and OpenXR yaw/pitch map on with the same signs.
//
// Returns true when it took the frame (Snake's eyes, or the virtual screen for
// a Snake-less shot); false hands the frame to the director-camera path.
// ===========================================================================
static inline bool PovPlausible(uint32_t p) { return p >= 0x00400000u && p < 0x7FFF0000u; }

static bool DriveSnakePov(uint32_t scene, uint32_t data, unsigned long long frame) {
    g_csDemoTick.store(GetTickCount64(), std::memory_order_relaxed);

    if (!g_vrViewMode.load(std::memory_order_relaxed) ||
        !g_enabled.load(std::memory_order_relaxed) ||
        !g_havePose.load(std::memory_order_relaxed) ||
        !PovPlausible(scene) || !PovPlausible(data)) {
        g_csScreenFallback.store(false, std::memory_order_relaxed);
        g_povActive.store(false, std::memory_order_relaxed);
        return false;
    }

    // --- per-demo state ------------------------------------------------------
    static uint32_t s_header = 0;
    static int   s_snakeIndex = -1, s_snakeType = -1;
    static bool  s_seen = false, s_haveOff = false, s_haveAnchor = false, s_haveLast = false;
    static int   s_absent = 0;
    static int32_t s_offX = 0, s_offY = 0, s_offZ = 0;
    static int32_t s_lastX = 0, s_lastY = 0, s_lastZ = 0;
    static float s_lastBodyYaw = 0.0f;
    static float s_anchorYaw = 0.0f, s_refYaw = 0.0f;
    static float s_refPosX = 0.0f, s_refPosY = 0.0f, s_refPosZ = 0.0f;
    static int   s_logLines = 0;

    int32_t header = 0, models = 0, nModels = 0, mdl = 0;
    if (!SafeRead32((uintptr_t)scene + 0x30, &header) || !PovPlausible((uint32_t)header) ||
        !SafeRead32((uintptr_t)scene + 0x34, &models) || !PovPlausible((uint32_t)models) ||
        !SafeRead32((uintptr_t)(uint32_t)header + 0x10, &nModels) ||
        !SafeRead32((uintptr_t)(uint32_t)header + 0x18, &mdl) || !PovPlausible((uint32_t)mdl) ||
        nModels < 0 || nModels > 64) {
        static int s_bad = 0;
        if (s_bad < 5) {
            s_bad++;
            DebugLogger::LogFormat("Snake's eyes: demo header unreadable (scene=%08X header=%08X models=%08X "
                "n=%d) -- this frame goes to the %s.", scene, (uint32_t)header, (uint32_t)models, nModels,
                g_csPovNoSnake == 1 ? "virtual screen" : "director camera");
        }
        nModels = 0;
    }

    // A new demo: find Snake in its cast, once, and say who is in it.
    if ((uint32_t)header != s_header) {
        s_header = (uint32_t)header;
        s_snakeIndex = -1; s_snakeType = -1;
        s_seen = false; s_haveOff = false; s_haveAnchor = false; s_haveLast = false; s_absent = 0;
        const uint32_t playerDef = g_playerModelDef.load(std::memory_order_relaxed);
        char tbl[700] = {};
        int w = 0;
        for (int i = 0; i < nModels; ++i) {
            int32_t type = 0, file = 0, objs = 0, def = 0;
            SafeRead32((uintptr_t)(uint32_t)mdl + i * 0x14 + 0x0, &type);
            SafeRead32((uintptr_t)(uint32_t)mdl + i * 0x14 + 0xC, &file);
            SafeRead32((uintptr_t)(uint32_t)models + i * 0x1A4 + 0x7C, &objs);
            if (PovPlausible((uint32_t)objs)) SafeRead32((uintptr_t)(uint32_t)objs + 0x24, &def);
            const uint16_t h = (uint16_t)(file & 0xFFFF);
            bool byName = false;
            for (int k = 0; k < g_csPovHashCount; ++k) if (g_csPovHashes[k] == h) byName = true;
            const bool byDef = playerDef != 0 && (uint32_t)def == playerDef;
            if ((byName || byDef) && s_snakeIndex < 0) { s_snakeIndex = i; s_snakeType = type; }
            const int n = _snprintf_s(tbl + w, sizeof(tbl) - w, _TRUNCATE, "%s[%d] type=%d model=%04X%s%s",
                w ? " " : "", i, type, h, byName ? " SNAKE(name)" : "", byDef ? " SNAKE(outfit)" : "");
            if (n > 0) w += n;
        }
        DebugLogger::LogFormat("Demo model table (header %08X, %d models, player DG_DEF %08X): %s -> %s",
            s_header, nModels, playerDef, nModels ? tbl : "(none)",
            s_snakeIndex >= 0 ? "Snake FOUND, cutscene seen through his eyes"
                              : (g_csPovNoSnake == 1 ? "no Snake in this scene, virtual screen"
                                                : "no Snake in this scene, director camera"));
    }

    // --- this frame's pose for Snake -----------------------------------------
    bool present = false;
    int16_t px = 0, py = 0, pz = 0, rotY = 0, rot0Y = 0;
    uint32_t objs = 0;
    if (s_snakeIndex >= 0) {
        int16_t nAdj = 0;
        int32_t adj = 0;
        if (SafeRead16((uintptr_t)data + 0x20, &nAdj) && SafeRead32((uintptr_t)data + 0x24, &adj) &&
            nAdj > 0 && nAdj < 128) {
            uint32_t a = (uint32_t)adj;
            if (a < 0x40000u) a += data;          // the game converts this offset a few instructions later
            for (int i = 0; i < nAdj && PovPlausible(a); ++i) {
                const uintptr_t e = (uintptr_t)a + (uintptr_t)i * 0x18;
                int32_t type = 0;
                if (!SafeRead32(e, &type)) break;
                if (type != s_snakeType) continue;
                int16_t vis = 0;
                SafeRead16(e + 0x4, &vis);
                if (vis != 0 && SafeRead16(e + 0xC, &px) && SafeRead16(e + 0xE, &py) &&
                    SafeRead16(e + 0x10, &pz) && SafeRead16(e + 0x8, &rotY)) {
                    present = true;
                    // Joint 0's yaw rides on top of the model's own; the demo
                    // code (magic_calc) adds the two to get the facing.
                    int16_t nRots = 0;
                    int32_t rots = 0;
                    if (SafeRead16(e + 0x12, &nRots) && nRots > 0 && SafeRead32(e + 0x14, &rots)) {
                        uint32_t r = (uint32_t)rots;
                        if (r < 0x40000u) r += (uint32_t)e;
                        if (PovPlausible(r)) SafeRead16((uintptr_t)r + 2, &rot0Y);
                    }
                }
                break;
            }
        }
        int32_t o = 0;
        if (SafeRead32((uintptr_t)(uint32_t)models + s_snakeIndex * 0x1A4 + 0x7C, &o) && PovPlausible((uint32_t)o))
            objs = (uint32_t)o;
    }

    if (!present) {
        ++s_absent;
        // A brief gap inside a Snake scene (a one-frame hide, a cut) holds the
        // last eye rather than flashing the screen up for a frame.
        bool hold = s_seen && s_haveLast && s_absent < g_csPovAbsentFrames;
        // cutscene_pov_no_snake=2 (2026-10-02, "can we have them be first
        // person like the rest?"): Snake is in this scene but not drawn in
        // this shot (the director cut to whoever he is talking to). He is
        // still standing where he was, so stay behind his eyes -- as long as
        // the shot is looking somewhere near him, not at another room.
        if (!hold && g_csPovNoSnake == 2 && s_seen && s_haveLast) {
            ViewPoint dTo{};
            if (ReadViewPoint((uintptr_t)data + kDemoCamToOffset, &dTo)) {
                const float qx = (float)(dTo.x - s_lastX), qy = (float)(dTo.y - s_lastY), qz = (float)(dTo.z - s_lastZ);
                hold = std::sqrt(qx * qx + qy * qy + qz * qz) < (float)g_csPovHoldRadius;
            }
            static unsigned long long s_holdLogHeader = 0;
            if (s_holdLogHeader != (unsigned long long)s_header + 1) {
                s_holdLogHeader = (unsigned long long)s_header + 1;
                DebugLogger::LogFormat("Snake's eyes: a shot without Snake in it -- %s", hold
                    ? "staying behind his eyes where he stands (cutscene_pov_no_snake=2)"
                    : "the shot is somewhere else entirely, so the director's camera with head look");
            }
        }
        if (!hold) {
            g_povActive.store(false, std::memory_order_relaxed);
            g_dcDriving.store(false, std::memory_order_relaxed);
            if (g_csPovNoSnake == 1) {
                g_csScreenFallback.store(true, std::memory_order_relaxed);
                return true;                      // nothing written: the flat screen shows the directed shot
            }
            g_csScreenFallback.store(false, std::memory_order_relaxed);
            return false;                         // director camera + head look
        }
        px = (int16_t)s_lastX; py = (int16_t)s_lastY; pz = (int16_t)s_lastZ;
    }
    else {
        s_absent = 0;
    }

    // --- head offset: last frame's joint 6 minus last frame's model origin ----
    bool headFromJoint = false;
    if (present && objs != 0) {
        int16_t nJoints = 0;
        int32_t rx = 0, ry = 0, rz = 0, hx = 0, hy = 0, hz = 0;
        if (SafeRead16((uintptr_t)objs + 0x2E, &nJoints) && nJoints > 6 &&
            SafeRead32((uintptr_t)objs + 0x14, &rx) && SafeRead32((uintptr_t)objs + 0x18, &ry) &&
            SafeRead32((uintptr_t)objs + 0x1C, &rz) &&
            SafeRead32((uintptr_t)objs + 0x284, &hx) && SafeRead32((uintptr_t)objs + 0x288, &hy) &&
            SafeRead32((uintptr_t)objs + 0x28C, &hz)) {
            const int32_t ox = hx - rx, oy = hy - ry, oz = hz - rz;
            // A head sits within a couple of metres of its own origin; the
            // matrix of a model that has not been posed yet reads as zeros.
            const bool sane = std::abs(ox) < 2500 && std::abs(oy) < 2500 && std::abs(oz) < 2500 &&
                (ox != 0 || oy != 0 || oz != 0) && (rx != 0 || ry != 0 || rz != 0);
            if (sane) { s_offX = ox; s_offY = oy; s_offZ = oz; s_haveOff = true; headFromJoint = true; }
        }
    }
    const int32_t offX = s_haveOff ? s_offX : 0;
    const int32_t offY = s_haveOff ? s_offY : g_csPovFallbackHead;
    const int32_t offZ = s_haveOff ? s_offZ : 0;

    const float bodyYaw = NormalizeRad(((float)(((int)rotY + (int)rot0Y) & 0xFFF)) * (2.0f * kPi / 4096.0f) +
        (float)g_csPovYawOffsetDeg * kPi / 180.0f);

    // --- head pose -------------------------------------------------------------
    // One consistent XR sample: angles AND both eye poses from the same instant.
    // The frame view record published below tells the compositor "this image
    // was drawn from exactly this head pose", so the camera has to be built
    // from that same sample or the world-lock would be off by the difference.
    HeadViewSnap snap;
    AcquireSRWLockShared(&g_headViewLock);
    snap = g_headViewSnap;
    ReleaseSRWLockShared(&g_headViewLock);
    const float hYaw = snap.have ? snap.yaw : g_headYaw.load(std::memory_order_relaxed);
    float hPitch = snap.have ? snap.pitch : g_headPitch.load(std::memory_order_relaxed);
    const float hPosX = snap.have ? (snap.eye[0].px + snap.eye[1].px) * 0.5f : g_headPosX.load(std::memory_order_relaxed);
    const float hPosY = snap.have ? (snap.eye[0].py + snap.eye[1].py) * 0.5f : g_headPosY.load(std::memory_order_relaxed);
    const float hPosZ = snap.have ? (snap.eye[0].pz + snap.eye[1].pz) * 0.5f : g_headPosZ.load(std::memory_order_relaxed);
    if (g_csPovInvertPitch) hPitch = -hPitch;

    // Face Snake's way at the start of the scene, after a big move, and on
    // an R3-hold recentre. Between those, only the player's head turns.
    bool jumped = false;
    if (present && s_haveLast) {
        const float jx = (float)(px - s_lastX), jy = (float)(py - s_lastY), jz = (float)(pz - s_lastZ);
        jumped = std::sqrt(jx * jx + jy * jy + jz * jz) > (float)g_csPovCutUnits;
    }
    const bool recenter = g_vcRecenter.exchange(false, std::memory_order_relaxed);
    if (!s_haveAnchor || jumped || recenter) {
        s_anchorYaw = present ? bodyYaw : s_lastBodyYaw;
        s_refYaw = hYaw;
        s_refPosX = hPosX; s_refPosY = hPosY; s_refPosZ = hPosZ;
        s_haveAnchor = true;
        DebugLogger::LogFormat("Snake's eyes: facing pinned at frame %llu (%s) -- Snake at (%d,%d,%d) facing "
            "%.1f deg, head offset (%d,%d,%d) from %s.", frame,
            jumped ? "Snake moved to a new spot" : (recenter ? "recentre" : "scene start"),
            px, py, pz, s_anchorYaw * 180.0f / kPi, offX, offY, offZ,
            s_haveOff ? "his head joint" : "the fallback height");
    }
    if (present) {
        s_seen = true;
        s_lastX = px; s_lastY = py; s_lastZ = pz;
        s_lastBodyYaw = bodyYaw;
        s_haveLast = true;
    }

    const float yaw = NormalizeRad(s_anchorYaw + NormalizeRad(hYaw - s_refYaw));
    float pitch = hPitch;
    const float kPitchLimit = 1.4835f;              // 85 degrees
    if (pitch > kPitchLimit)  pitch = kPitchLimit;
    if (pitch < -kPitchLimit) pitch = -kPitchLimit;
    const float cp = std::cos(pitch), sp = std::sin(pitch);
    const float sy = std::sin(yaw), cy = std::cos(yaw);
    const float fwdX = cp * sy, fwdY = sp, fwdZ = cp * cy;
    const float rgtX = -cy, rgtZ = sy;

    // --- 6DOF: the lean measured in the room, laid onto Snake's frame ---------
    float ox = 0.0f, oy = 0.0f, oz = 0.0f;
    if (g_csPovPosition && g_positionEnabled) {
        const float dxr = hPosX - s_refPosX, dyr = hPosY - s_refPosY, dzr = hPosZ - s_refPosZ;
        // OpenXR: Y up, forward is -Z, positive yaw turns left.
        const float fx = -std::sin(s_refYaw), fz = -std::cos(s_refYaw);
        const float rx = std::cos(s_refYaw), rz = -std::sin(s_refYaw);
        const float dF = dxr * fx + dzr * fz;
        const float dR = dxr * rx + dzr * rz;
        const float ay = std::sin(s_anchorYaw), ac = std::cos(s_anchorYaw);
        const float lim = (float)g_maxPositionOffset;
        auto clampf = [lim](float v) { return v > lim ? lim : (v < -lim ? -lim : v); };
        ox = clampf((dF * ay + dR * -ac) * g_positionScale);
        oz = clampf((dF * ac + dR * ay) * g_positionScale);
        oy = clampf(dyr * g_positionScale);
    }

    // --- stereo, exactly as the director path does it -------------------------
    float sx = 0.0f, sz = 0.0f;
    if (g_csPovStereo && g_stereoAlternate && IsRenderTwiceLive()) {
        g_publishedEye.store(2, std::memory_order_relaxed);
        g_eyePublishTick.store(GetTickCount64(), std::memory_order_relaxed);
    }
    else if (g_csPovStereo && g_stereoAlternate) {
        static unsigned long long s_lastRotRuns = 0;
        static int s_eye = 0;
        const unsigned long long rotRuns = g_rotHookRuns.load(std::memory_order_relaxed);
        if (rotRuns == s_lastRotRuns) {
            s_eye ^= 1;
            g_publishedEye.store(s_eye, std::memory_order_relaxed);
            g_eyePublishTick.store(GetTickCount64(), std::memory_order_relaxed);
        }
        s_lastRotRuns = rotRuns;
        const int eye = g_publishedEye.load(std::memory_order_relaxed);
        if (eye == 0 || eye == 1) {
            float half = ((float)g_stereoIpdMm / 2000.0f) * g_positionScale;
            if (g_stereoSwapEyes) half = -half;
            const float k = (eye == 0) ? -half : half;
            sx = rgtX * k; sz = rgtZ * k;
        }
    }

    // --- the eye: head joint, nudged out in front of the face ------------------
    const float push = (float)g_csPovForward;
    const float ex = (float)(px + offX) + sy * push + ox + sx;
    const float ey = (float)(py + offY) + oy;
    const float ez = (float)(pz + offZ) + cy * push + oz + sz;

    ViewPoint shotFrom{}, shotTo{};
    ReadViewPoint((uintptr_t)data + kDemoCamFromOffset, &shotFrom);
    ReadViewPoint((uintptr_t)data + kDemoCamToOffset, &shotTo);

    const ViewPoint newFrom{ (int32_t)std::lround(ex), (int32_t)std::lround(ey), (int32_t)std::lround(ez) };
    const ViewPoint newTo{
        newFrom.x + (int32_t)std::lround(fwdX * 1024.0f),
        newFrom.y + (int32_t)std::lround(fwdY * 1024.0f),
        newFrom.z + (int32_t)std::lround(fwdZ * 1024.0f) };

    const bool wrote = WriteViewPoint((uintptr_t)data + kDemoCamFromOffset, newFrom) &&
        WriteViewPoint((uintptr_t)data + kDemoCamToOffset, newTo);
    SafeWrite16((uintptr_t)data + 0x14, 0);                     // no director's dutch angle
    int16_t lens = 0;
    if (g_csPovFov && g_fovClipDistance > 0) {
        // The scene zooms by changing its lens; inside someone's head a zoom is
        // a lurch, and the headset's projection is built for the mod's lens.
        SafeWrite16((uintptr_t)data + 0x16, (int16_t)g_fovClipDistance);
        lens = (int16_t)g_fovClipDistance;
    }
    else {
        SafeRead16((uintptr_t)data + 0x16, &lens);
    }

    // ---- FRAME VIEW RECORD: the world lock -------------------------------
    // 2026-09-28 first test: "distorted when looking in each direction, things
    // in the distance warp a bit". The rotation hook is what normally records
    // the head pose each frame was drawn from, and it does not run during a
    // cutscene -- so these frames went out with NO record: the layer was
    // tagged with the live head pose instead of the render pose (no timewarp
    // correction) and claimed the runtime's whole asymmetric eye frustum
    // instead of the 106-degree lens actually rendered (the image stretched
    // and sheared as you turned). Exactly the swim the world-lock work fixed
    // for gameplay. Publish the same record here: the head sample the camera
    // was built from, no roll (the image is drawn level), and the lens used.
    if (wrote && g_frameRecordEnabled && snap.have) {
        FrameViewRecord r;
        r.valid = true;
        r.eye[0] = snap.eye[0];
        r.eye[1] = snap.eye[1];
        r.yawEq = snap.yaw;
        r.pitchEq = pitch;           // the pitch actually rendered (clamped as drawn)
        r.rollEq = 0.0f;
        r.baseYaw = 0;               // no stick turning inside a cutscene
        r.basePitch = 0;
        r.kYaw = 1;
        r.kPitch = 1;
        r.clip = (lens >= 64 && lens <= 4000) ? (int)lens : 0;
        r.tick = GetTickCount64();
        { LARGE_INTEGER q; QueryPerformanceCounter(&q); r.qpc = q.QuadPart; }
        AcquireSRWLockExclusive(&g_frameRecLock);
        g_frameRec = r;
        g_frameRecRun = g_rotHookRuns.load(std::memory_order_relaxed);
        g_lastValidRec = r;
        ReleaseSRWLockExclusive(&g_frameRecLock);
        static int s_recLog = 0;
        if (s_recLog < 2) {
            s_recLog++;
            DebugLogger::LogFormat("Snake's eyes: frames now carry their render pose (head yaw %.1f pitch %.1f, "
                "lens %d) -- world-locked and FOV-exact like first person.",
                snap.yaw * 57.2958f, r.pitchEq * 57.2958f, r.clip);
        }
    }

    g_csScreenFallback.store(false, std::memory_order_relaxed);
    g_povActive.store(wrote, std::memory_order_relaxed);
    g_dcDriving.store(wrote, std::memory_order_relaxed);
    if (wrote && g_csPovHideSnake && objs != 0) {
        // Snake's own head, shoulders and arms sit right on top of the camera
        // and slice through whoever he is standing next to. Hidden for the
        // renderer only (motion_aim.cpp, around the DG frame actors).
        g_renderHideObjs[1].store(objs, std::memory_order_relaxed);
        g_renderHideTick[1].store(GetTickCount64(), std::memory_order_relaxed);
    }

    if (s_logLines < g_csPovLogLines && (frame % (unsigned)g_csPovLogEvery) == 0) {
        s_logLines++;
        DebugLogger::TestLogFormat(
            "Pov[%llu]: %s snake[%d] type=%d origin=(%d,%d,%d) rotY=%d+%d body=%.1fdeg head=(%d,%d,%d)%s | "
            "view yaw=%.1f pitch=%.1f lean=(%d,%d,%d) | FROM=(%d,%d,%d) TO=(%d,%d,%d) | director was "
            "(%d,%d,%d)->(%d,%d,%d) | wrote=%d",
            frame, present ? "LIVE" : "HOLD", s_snakeIndex, s_snakeType, px, py, pz, rotY, rot0Y,
            bodyYaw * 180.0f / kPi, offX, offY, offZ, headFromJoint ? "" : (s_haveOff ? " (last)" : " (fallback)"),
            yaw * 180.0f / kPi, pitch * 180.0f / kPi, (int)ox, (int)oy, (int)oz,
            newFrom.x, newFrom.y, newFrom.z, newTo.x, newTo.y, newTo.z,
            shotFrom.x, shotFrom.y, shotFrom.z, shotTo.x, shotTo.y, shotTo.z, wrote ? 1 : 0);
    }
    return true;
}

// ===========================================================================
// CUTSCENE CAMERA BODY -- RVA 0x1CD292, the camera that actually runs.
//
// `stack`: [0] return address, [1] arg0, [2] arg1 == the camera script state.
// FROM is three int16 at arg1+0x08, TO at arg1+0x0E. Rewriting them here, at
// the top of the function, means the layer-3 copy at +1CD4C4 AND the inline
// angle derivation at +1CD56A both see the same edited values.
// ===========================================================================
extern "C" void __cdecl Mgs1DemoCamBody(uint32_t* stack) {
    if (g_moduleBase == 0 || stack == nullptr) return;

    const unsigned long long frame =
        g_dcFrames.fetch_add(1, std::memory_order_relaxed) + 1;

    uint32_t ret = 0, statePtr = 0, scenePtr = 0;
    if (!SafeRead32((uintptr_t)(stack + 0), (int32_t*)&ret)) return;
    // arg0 -- the SCENE. Six builds went past this without reading it. It is
    // the object whose +0x38 list holds the shot's cast.
    SafeRead32((uintptr_t)(stack + 1), (int32_t*)&scenePtr);
    if (!SafeRead32((uintptr_t)(stack + 2), (int32_t*)&statePtr)) return;
    if (statePtr == 0) return;
    g_demoScene.store(scenePtr, std::memory_order_relaxed);

    const uintptr_t fromAddr = (uintptr_t)statePtr + kDemoCamFromOffset;
    const uintptr_t toAddr = (uintptr_t)statePtr + kDemoCamToOffset;

    ViewPoint from{}, to{};
    const bool ok = ReadViewPoint(fromAddr, &from) && ReadViewPoint(toAddr, &to);

    // Instrument first, unconditionally. Whether this function runs at all
    // during a cutscene is the question three builds have failed to answer, so
    // the answer must not depend on any gate.
    static int s_log = 0;
    if (s_log < g_dcLogLines) {
        s_log++;
        DebugLogger::LogFormat(
            "Cutscene camera: +1CD292 ran (call %llu, from +%X) scene=%08X chunk=%08X "
            "FROM=(%d,%d,%d) TO=(%d,%d,%d)%s | cs=%d fpv=%d",
            frame,
            (ret > (uint32_t)g_moduleBase) ? (unsigned)(ret - (uint32_t)g_moduleBase) : 0u,
            scenePtr, statePtr, from.x, from.y, from.z, to.x, to.y, to.z, ok ? "" : " (unreadable)",
            g_csvDetected.load(std::memory_order_relaxed) ? 1 : 0,
            g_lastFpvState ? 1 : 0);
    }
    if (!ok) return;

    // Snake's eyes. When it takes the frame nothing below runs: the director
    // path, the old actor-pointer POV and all the cast-hunting diagnostics are
    // for cutscene_view=0 and for scenes it hands back.
    if (g_csView == 1 && g_dcMode != 0 && DriveSnakePov(scenePtr, statePtr, frame)) return;
    g_csScreenFallback.store(false, std::memory_order_relaxed);

    // --- Snake's point of view ---------------------------------------------
    // Read him and log it BEFORE the drive gate, deliberately. Whether the
    // actor keeps moving while the camera task is switched off is the one link
    // in this chain the disassembly cannot settle, and the answer must not
    // depend on VR mode, on the cutscene detector, or on anything else that
    // could be the thing that is wrong. Same discipline as the +1CD292 line
    // above, for the same reason.
    // Walk the engine's object list first: it publishes the nearest-to-target
    // node that source mode 3 then reads.
    g_demoState.store(statePtr, std::memory_order_relaxed);
    DemoScanNearTarget(to, frame);
    // Dump the frame just drawn (ranked against the TO it was drawn for), then
    // publish this frame's TO for the draws that are about to happen.
    ReportTransforms(from, to, frame);
    g_xformToX.store(to.x, std::memory_order_relaxed);
    g_xformToY.store(to.y, std::memory_order_relaxed);
    g_xformToZ.store(to.z, std::memory_order_relaxed);
    EnumerateDemoCast(scenePtr, frame);
    EnumerateTasks(frame);
    ScanActorSignatures(to, frame);
    EnumerateClassLists(frame);
    EnumerateActors(to, frame);

    const SnakeState snake = ReadSnake();

    ViewPoint eye{};
    bool havePov = false;
    // An all-zero read is refused rather than trusted. A freed or not-yet-built
    // object reads as zeros through a valid page, and (0,0,0) is a real world
    // coordinate the camera would happily fly to -- which on screen is
    // indistinguishable from the hook being broken.
    const bool zeroed = snake.havePos &&
        snake.pos.x == 0 && snake.pos.y == 0 && snake.pos.z == 0;
    if (g_povMode && snake.havePos && !zeroed) {
        eye.x = snake.pos.x;
        eye.z = snake.pos.z;
        // Game Y is DOWN, so the head is at a SMALLER Y than the feet. Prefer
        // the game's own head-bone value; the configured fallback only exists
        // for the frames where the model pointer has not been built yet.
        eye.y = snake.eyeY + g_povEyeOffset;
        havePov = true;
    }

    static int s_povLog = 0;
    if (s_povLog < g_povProbeLines && (frame % (unsigned)g_povProbeEvery) == 0) {
        s_povLog++;
        // distSnakeToTo is the line that settles whether Snake and the shot are
        // even in the same coordinate space. If the director is framing Snake,
        // the look-at TARGET lands on or near him; a number in the hundreds
        // confirms the space, a number in the tens of thousands refutes the
        // whole actor derivation and nothing downstream is worth debugging.
        const double dx = (double)(to.x - snake.pos.x);
        const double dy = (double)(to.y - snake.pos.y);
        const double dz = (double)(to.z - snake.pos.z);
        const int    distToTarget = (int)sqrt(dx * dx + dy * dy + dz * dz);
        DebugLogger::TestLogFormat(
            "Snake[%llu]: obj=%08X%s%s cap=%08X glob=%08X rej=%d model=%08X f895=%02X "
            "stance=%d pos=(%d,%d,%d)%s | "
            "eyeY=%d d=%d src=%s raw=%d%s | rot(p=%d y=%d)%s | "
            "shot from=(%d,%d,%d) to=(%d,%d,%d) distTO=%d | pov=%d dEYE=(%d,%d,%d)",
            frame, snake.objPtr, snake.havePtr ? "" : " (NO PTR)",
            snake.fromList ? " LIST" : (snake.fromCapture ? " CAP"
                : (snake.fromGlobal ? " GLOB" : " NONE")),
            snake.capPtr, snake.globPtr, snake.rejected,
            snake.modelPtr, (unsigned)snake.flags895, (int)snake.stance,
            snake.pos.x, snake.pos.y, snake.pos.z,
            snake.plausible ? (snake.havePos ? "" : " (unread)") : " DANGLING",
            snake.eyeY, snake.eyeDelta,
            snake.eyeFromModel ? "model" : (g_eyeDeltaCalValid ? "cal" : "seed"),
            snake.eyeRaw, snake.eyeRejected ? " REJECTED" : "",
            snake.pitch, snake.yaw, snake.haveRot ? "" : " (unread)",
            from.x, from.y, from.z, to.x, to.y, to.z, distToTarget,
            havePov ? 1 : 0,
            havePov ? (eye.x - from.x) : 0,
            havePov ? (eye.y - from.y) : 0,
            havePov ? (eye.z - from.z) : 0);

        // And the candidates the FPV sweep found, read back HERE -- during the
        // cutscene, when the camera routine is stopped. The one that is still
        // moving, and still in the shot's coordinate range, is Snake.
        if (g_fieldCount > 0 && snake.havePtr) {
            char vals[512];
            int  used = 0;
            vals[0] = '\0';
            for (int i = 0; i < g_fieldCount; ++i) {
                int16_t x = 0, y = 0, z = 0;
                const uintptr_t base = (uintptr_t)snake.objPtr + (unsigned)g_fieldOff[i];
                SafeRead16(base + 0, &x); SafeRead16(base + 2, &y); SafeRead16(base + 4, &z);
                const int n = _snprintf_s(vals + used, sizeof(vals) - used, _TRUNCATE,
                    "%s+0x%03X=(%d,%d,%d)", used ? " " : "", g_fieldOff[i], (int)x, (int)y, (int)z);
                if (n > 0) used += n;
            }
            DebugLogger::TestLogFormat("Cand[%llu]: %s", frame, vals);
        }
    }

    bool drive = (g_dcMode != 0) &&
        g_vrViewMode.load(std::memory_order_relaxed) &&
        g_enabled.load(std::memory_order_relaxed) &&
        g_havePose.load(std::memory_order_relaxed);
    if (drive && g_dcMode == 1) {
        drive = g_csvDetected.load(std::memory_order_relaxed);
    }
    if (!drive) {
        g_dcDriving.store(false, std::memory_order_relaxed);
        g_povActive.store(false, std::memory_order_relaxed);
        return;
    }

    g_povActive.store(havePov, std::memory_order_relaxed);

    const bool wrote = DriveViewPair(fromAddr, toAddr, from, to, frame, "CUT", 2, nullptr,
        havePov ? &eye : nullptr);
    g_dcDriving.store(wrote, std::memory_order_relaxed);
}

// ===========================================================================
// The detours. Reached by jmp, never call -- there is no return address on
// the stack, so neither may execute a `ret`. Both save everything (the game
// keeps using esi/ebx/edi/ebp immediately afterwards), 16-byte align the
// stack before entering C++, restore, and jump back.
// ===========================================================================
#ifdef _M_IX86
extern "C" void __cdecl Mgs1CameraRotWriteBody(uint32_t gameRollDword);
extern "C" void __cdecl Mgs1CameraPosWriteBody(uint32_t actorPtr);
extern "C" void __cdecl Mgs1ViewCommitBody();
extern "C" void __cdecl Mgs1ViewBuildBody(uint32_t* stack);
extern "C" void __cdecl Mgs1DemoCamBody(uint32_t* stack);

static __declspec(naked) void Mgs1DemoCamDetour() {
    __asm {
        pushfd
        pushad
        lea  eax, [esp + 36]
        mov  ebp, esp
        and  esp, 0FFFFFFF0h
        sub  esp, 16
        mov  [esp], eax
        call Mgs1DemoCamBody
        mov  esp, ebp
        popad
        popfd

        push ebp
        mov  ebp, esp
        sub  esp, 2Ch
        jmp  dword ptr [g_dcResumeAddr]
    }
}

extern "C" void __cdecl Mgs1XformBody(uint32_t* stack);

static __declspec(naked) void Mgs1XformDetour() {
    __asm {
        pushfd
        pushad
        lea  eax, [esp + 36]
        mov  ebp, esp
        and  esp, 0FFFFFFF0h
        sub  esp, 16
        mov  [esp], eax
        call Mgs1XformBody
        mov  esp, ebp
        popad
        popfd

        push ebp
        mov  ebp, esp
        sub  esp, 20h
        jmp  dword ptr [g_xformResumeAddr]
    }
}

static __declspec(naked) void Mgs1ViewBuildDetour() {
    __asm {
        pushfd
        pushad
        lea  eax, [esp + 36]            // pushfd(4) + pushad(32): the original esp,
                                        // pointing at the return address
        mov  ebp, esp
        and  esp, 0FFFFFFF0h
        sub  esp, 16
        mov  [esp], eax
        call Mgs1ViewBuildBody
        mov  esp, ebp
        popad
        popfd

        // Replay the prologue we overwrote, then resume past it.
        push ebp
        mov  ebp, esp
        sub  esp, 30h
        jmp  dword ptr [g_vbResumeAddr]
    }
}

static __declspec(naked) void Mgs1ViewCommitDetour() {
    __asm {
        pushfd
        pushad
        mov  ebp, esp
        and  esp, 0FFFFFFF0h
        sub  esp, 16
        call Mgs1ViewCommitBody
        mov  esp, ebp
        popad
        popfd

        // Replay the instruction we replaced. It loads esi, which the game
        // pushes as the view-struct argument four instructions later, so it
        // must run AFTER popad has put the old esi back.
        mov  esi, 06BC36Ch
        jmp  dword ptr [g_vcResumeAddr]
    }
}

static __declspec(naked) void Mgs1CameraRotWriteDetour() {
    __asm {
        pushfd
        pushad
        mov  ebp, esp
        and  esp, 0FFFFFFF0h
        sub  esp, 16
        mov  [esp], eax                 // the DWORD the game was about to store
        call Mgs1CameraRotWriteBody
        mov  esp, ebp
        popad
        popfd
        jmp  dword ptr [g_rotResumeAddr]
    }
}

static __declspec(naked) void Mgs1CameraPosWriteDetour() {
    __asm {
        pushfd
        pushad
        mov  ebp, esp
        and  esp, 0FFFFFFF0h
        sub  esp, 16
        // esi is the player actor this routine was handed -- see the comment
        // at the top of Mgs1CameraPosWriteBody. Passed as arg0 in the aligned
        // frame; esp is restored from ebp below, so cdecl cleanup is moot.
        mov  [esp], esi
        call Mgs1CameraPosWriteBody
        mov  esp, ebp
        popad
        popfd

        // Replay the two instructions we replaced. They load eax and ebx,
        // which the game uses immediately after, so they must run AFTER the
        // register restore above.
        mov  eax, [ebx]
        mov  ebx, [ebx + 4]
        jmp  dword ptr [g_posResumeAddr]
    }
}
#endif

// ===========================================================================
// Public API
// ===========================================================================

void PublishHeadPose(float yawRad, float pitchRad, float rollRad,
    float posX, float posY, float posZ) {
    g_headYaw.store(yawRad, std::memory_order_relaxed);
    g_headPitch.store(pitchRad, std::memory_order_relaxed);
    g_headRoll.store(rollRad, std::memory_order_relaxed);
    g_headPosX.store(posX, std::memory_order_relaxed);
    g_headPosY.store(posY, std::memory_order_relaxed);
    g_headPosZ.store(posZ, std::memory_order_relaxed);
    g_havePose.store(true, std::memory_order_relaxed);
}

void PublishHeadViews(float yawRad, float pitchRad, float rollRad,
                      const ViewPoseF& leftEye, const ViewPoseF& rightEye) {
    AcquireSRWLockExclusive(&g_headViewLock);
    g_headViewSnap.have = true;
    g_headViewSnap.yaw = yawRad;
    g_headViewSnap.pitch = pitchRad;
    g_headViewSnap.roll = rollRad;
    g_headViewSnap.eye[0] = leftEye;
    g_headViewSnap.eye[1] = rightEye;
    ReleaseSRWLockExclusive(&g_headViewLock);
}

FrameViewRecord ConsumeFrameViewRecord() {
    FrameViewRecord out;
    AcquireSRWLockShared(&g_frameRecLock);
    // A gameplay Snake's eyes record is written at the view build, and the
    // rotation hook may run AFTER that in the same frame (2026-10-02 test: the
    // run-count check threw every one of them away, the frames went out with
    // no pose and the runtime's whole frustum -- the "weird FOV" distortion).
    // Its own freshness is the test instead, and it is handed over once.
    const bool povRec = g_frameRec.valid && g_frameRec.pov &&
        GetTickCount64() - g_frameRec.tick < 60;
    const bool current = g_frameRec.valid &&
        (g_frameRec.pov ? povRec : g_frameRecRun == g_rotHookRuns.load(std::memory_order_relaxed));
    if (current) out = g_frameRec;
    ReleaseSRWLockShared(&g_frameRecLock);
    if (!current) return FrameViewRecord{};

    // Same expiry as the eye tag: if the rotation hook has stopped running
    // (menu, codec, stalled cutscene) the record is not about these pixels.
    const unsigned long long now = GetTickCount64();
    if (out.tick == 0 || (now - out.tick) > (unsigned long long)g_stereoEyeTimeoutMs) {
        return FrameViewRecord{};
    }

    // The projection distance this frame was ACTUALLY drawn with: layer 3's
    // fov, the value pushed into the view-matrix builder. Falls back to the
    // clip_distance field, and to 0 (unknown) if neither reads sanely.
    // A cutscene record already knows its lens: the demo projects with the
    // clip in its own frame data, and layer 3's fov is stale while the camera
    // task is switched off.
    if (out.clip > 0) return out;
    int32_t clip32 = 0;
    int clip = 0;
    if (SafeRead32(g_moduleBase + kViewFovRva, &clip32) && clip32 >= 64 && clip32 <= 4000) {
        clip = (int)clip32;
    }
    else {
        int16_t clip16 = 0;
        if (SafeRead16(g_moduleBase + kFovClipDistanceRva, &clip16) && clip16 >= 64 && clip16 <= 4000)
            clip = (int)clip16;
    }
    out.clip = clip;
    return out;
}

// ---- camera -> eye-offset history (for motion aim's gun anchor) ----------
// The gun is built against layer-3 FROM, which is whichever eye's camera the
// game is holding at that moment -- not necessarily the eye the XR thread
// published last. Matching FROM against the cameras actually written tells us
// exactly which eye offset it carries, whatever the timing.
namespace {
struct CamEyeEntry { int32_t cam[3]; int32_t eye[3]; unsigned long long tick; };
CamEyeEntry g_camEye[6] = {};
int g_camEyeNext = 0;
SRWLOCK g_camEyeLock = SRWLOCK_INIT;
}

void RecordCameraEyeOffset(const int32_t cam[3], const int32_t eyeOff[3]) {
    AcquireSRWLockExclusive(&g_camEyeLock);
    CamEyeEntry& e = g_camEye[g_camEyeNext];
    for (int i = 0; i < 3; ++i) { e.cam[i] = cam[i]; e.eye[i] = eyeOff[i]; }
    e.tick = GetTickCount64();
    g_camEyeNext = (g_camEyeNext + 1) % 6;
    ReleaseSRWLockExclusive(&g_camEyeLock);
}

bool LookupEyeOffsetForCamera(const int32_t cam[3], int32_t outEye[3]) {
    const unsigned long long now = GetTickCount64();
    bool found = false;
    unsigned long long best = 0;
    AcquireSRWLockShared(&g_camEyeLock);
    for (const CamEyeEntry& e : g_camEye) {
        if (e.tick == 0 || now - e.tick > 250) continue;
        if (std::abs(e.cam[0] - cam[0]) > 1 || std::abs(e.cam[1] - cam[1]) > 1 ||
            std::abs(e.cam[2] - cam[2]) > 1) continue;
        if (!found || e.tick >= best) {
            best = e.tick; found = true;
            for (int i = 0; i < 3; ++i) outEye[i] = e.eye[i];
        }
    }
    ReleaseSRWLockShared(&g_camEyeLock);
    return found;
}

bool GetLatestFrameViewRecord(FrameViewRecord* out, unsigned maxAgeMs) {
    if (!out) return false;
    AcquireSRWLockShared(&g_frameRecLock);
    const FrameViewRecord r = g_lastValidRec;
    ReleaseSRWLockShared(&g_frameRecLock);
    if (!r.valid || r.tick == 0) return false;
    if ((GetTickCount64() - r.tick) > (unsigned long long)maxAgeMs) return false;
    *out = r;
    return true;
}

bool GetHeadYawOffsetRadians(float* outRad) {
    if (!outRad) return false;
    if (!g_enabled.load(std::memory_order_relaxed)) return false;
    if (!g_yawOffsetValid.load(std::memory_order_relaxed)) return false;
    *outRad = g_yawOffsetRad.load(std::memory_order_relaxed);
    return true;
}

void RequestCameraRecenter() {
    g_recenterRequested.store(true, std::memory_order_relaxed);
    // The view commit hook keeps its own reference, so it needs its own
    // request. Recentring that moved the gameplay camera but left a cutscene
    // pointing the old way would be a confusing half-fix.
    g_vcRecenter.store(true, std::memory_order_relaxed);
}

bool IsViewCommitHookInstalled() { return g_vcInstalled; }
bool IsViewCommitDriving() { return g_vcDriving.load(std::memory_order_relaxed); }
// FrameRunDemo stops being called the instant a demo ends, so every flag it
// owns is only believed while it has run in the last few frames.
static bool DemoRecentlyRan() {
    const unsigned long long t = g_csDemoTick.load(std::memory_order_relaxed);
    return t != 0 && (GetTickCount64() - t) < 250;
}
bool IsCutsceneSnakePovActive() {
    return DemoRecentlyRan() && g_povActive.load(std::memory_order_relaxed);
}
bool IsCutsceneScreenFallbackActive() {
    return DemoRecentlyRan() && g_vrViewMode.load(std::memory_order_relaxed) &&
        g_csScreenFallback.load(std::memory_order_relaxed);
}

void SetCameraWriteHookEnabled(bool enabled) {
    g_enabled.store(enabled, std::memory_order_relaxed);
    DebugLogger::LogFormat("Camera hook: %s", enabled ? "ENABLED" : "disabled");
}

bool IsCameraWriteHookInstalled() { return g_rotInstalled; }
bool IsPositionHookInstalled() { return g_posInstalled; }
bool IsCameraWriteHookEnabled() { return g_enabled.load(std::memory_order_relaxed); }

// ===========================================================================
// CUTSCENE VR -- the per-presented-frame tick, called from the DirectDraw
// Flip hook in ddraw_hook.cpp.
//
// WHY HERE AND NOT IN THE CAMERA HOOK. Everything this function does has to
// keep working in exactly the situation where the camera hook does not run.
// That is the entire point. Flip is the one thing confirmed (2026-08-22 depth
// probe) to fire continuously on the game's own thread for the whole session,
// so it is where the measurement and the fallback both live.
//
// Free of C++ objects on purpose: the Safe* helpers it calls contain the SEH,
// and MSVC rejects __try in anything needing unwinding (C2712).
// ===========================================================================
namespace {

// Signed shortest-way-round difference between two 12-bit engine angles.
inline int32_t AngleDeltaUnits(int32_t a, int32_t b) {
    int32_t d = (a - b) % 4096;
    if (d > 2048)  d -= 4096;
    if (d < -2048) d += 4096;
    return d;
}

// Head delta from the fallback's own reference pose, in engine units, with the
// same invert / swap / strength treatment the in-frame path applies. Kept
// separate rather than shared because the in-frame path also has to deal with
// the additive base angle and the yaw-offset publication, neither of which
// applies here.
void CutsceneFallbackHeadDelta(int32_t* outYaw, int32_t* outPitch) {
    const float hYaw = g_headYaw.load(std::memory_order_relaxed);
    const float hPitch = g_headPitch.load(std::memory_order_relaxed);

    float dYawRad = NormalizeRad(hYaw - g_csvFbRefHeadYaw);
    float dPitchRad = NormalizeRad(hPitch - g_csvFbRefHeadPitch);

    const float strength = g_cutsceneRotation
        ? ((float)g_cutsceneRotationPercent / 100.0f) : 0.0f;
    dYawRad *= strength;
    dPitchRad *= strength;

    if (g_invertHeadYaw)   dYawRad = -dYawRad;
    if (g_invertHeadPitch) dPitchRad = -dPitchRad;

    int32_t dYaw = RadToUnits(dYawRad);
    int32_t dPitch = RadToUnits(dPitchRad);
    if (!g_swapYawPitch) { const int32_t t = dYaw; dYaw = dPitch; dPitch = t; }

    *outYaw = dYaw;
    *outPitch = dPitch;
}

void CutsceneFallbackPinReference() {
    g_csvFbRefHeadYaw = g_headYaw.load(std::memory_order_relaxed);
    g_csvFbRefHeadPitch = g_headPitch.load(std::memory_order_relaxed);
    g_csvFbRefHeadPosX = g_headPosX.load(std::memory_order_relaxed);
    g_csvFbRefHeadPosY = g_headPosY.load(std::memory_order_relaxed);
    g_csvFbRefHeadPosZ = g_headPosZ.load(std::memory_order_relaxed);
}

void CutsceneFallbackReset() {
    g_csvFbHaveAnchor = false;
    g_csvFbHaveLastWrote = false;
    g_csvFbHaveLastPos = false;
}

// ---------------------------------------------------------------------------
// PROBE SCAN -- which words move while a cutscene plays?
// ---------------------------------------------------------------------------
// If the pulse shows that rotate2 is inert during cutscenes, the immediate
// next question is what the cutscene renderer reads instead. This answers it
// the cheap way: watch a window of memory around the camera block and count how
// often each 16-bit word changes while a scene is running.
//
// Two properties make the output readable rather than noise:
//
//   The window is small and centred on what we already understand. The camera
//   block we know (position at 0x593FA0, rotation at 0x593FC8, the "camera
//   control" word at 0x593FD0, clip distance at 0x594000) all sit inside the
//   default range, so the report comes with its own calibration: those words
//   MUST appear with high hit counts, and if they do not, the scan is broken
//   rather than the theory.
//
//   It counts changes rather than logging them. A cutscene is thousands of
//   frames; per-change logging would be a flood and would do file I/O on the
//   game's thread. One report per scene, capped, is enough to point at an
//   address, and pointing at an address is all this needs to do.
//
// Sampled on the game thread inside Flip. No allocation, fixed-size arrays.
void CutsceneProbeScanReset() {
    g_csvScanHavePrev = false;
    g_csvScanSamples = 0;
    for (int i = 0; i < kCsvScanMaxWords; ++i) g_csvScanHits[i] = 0;
}

void CutsceneProbeScanSample() {
    if (g_moduleBase == 0 || g_csvScanWords <= 0) return;
    const int words = (g_csvScanWords > kCsvScanMaxWords) ? kCsvScanMaxWords : g_csvScanWords;

    for (int i = 0; i < words; ++i) {
        int16_t v = 0;
        if (!SafeRead16(g_moduleBase + g_csvProbeScanBase + (uintptr_t)(i * 2), &v)) {
            // An unreadable word is not a changed word. Bail on the whole
            // sample rather than half-counting: a partially-read sample would
            // silently bias the hit counts toward the readable end of the
            // window, which is exactly the kind of quiet distortion that makes
            // a diagnostic worse than useless.
            //
            // And drop the baseline on the way out. The loop above has already
            // overwritten prev[0..i-1] with this sample's values while leaving
            // prev[i..] holding the previous one, so the array is now half of
            // each; comparing the next sample against that would manufacture
            // changes at the boundary. Re-baselining costs one sample.
            g_csvScanHavePrev = false;
            return;
        }
        const uint16_t u = (uint16_t)v;
        if (g_csvScanHavePrev && u != g_csvScanPrev[i]) {
            if (g_csvScanHits[i] < 0xFFFFFFFFu) g_csvScanHits[i]++;
        }
        g_csvScanPrev[i] = u;
    }
    g_csvScanHavePrev = true;
    if (g_csvScanSamples < 0xFFFFFFFFFFFFFFFFull) g_csvScanSamples++;
}

void CutsceneProbeScanReport() {
    if (!g_csvProbeScan || g_csvScanSamples < 4) return;

    // Report budget for the whole session. Every scene would otherwise print
    // this, and MGS1 has a lot of scenes.
    static int reports = 0;
    if (reports >= 6) return;
    reports++;

    const int words = (g_csvScanWords > kCsvScanMaxWords) ? kCsvScanMaxWords : g_csvScanWords;

    // Top 24 by hit count, selected by repeated linear scan. O(24*words) on a
    // 512-word window, once per cutscene -- cheaper than a sort, and no
    // allocation.
    constexpr int kTop = 24;
    int   bestIdx[kTop];
    uint32_t bestHit[kTop];
    int found = 0;
    for (int slot = 0; slot < kTop; ++slot) {
        int   pick = -1;
        uint32_t pickHits = 0;
        for (int i = 0; i < words; ++i) {
            if (g_csvScanHits[i] == 0) continue;
            bool taken = false;
            for (int j = 0; j < found; ++j) { if (bestIdx[j] == i) { taken = true; break; } }
            if (taken) continue;
            if (g_csvScanHits[i] > pickHits) { pickHits = g_csvScanHits[i]; pick = i; }
        }
        if (pick < 0) break;
        bestIdx[found] = pick;
        bestHit[found] = pickHits;
        found++;
    }

    DebugLogger::LogFormat(
        "Cutscene probe scan: %llu samples over that scene. The %d most-changed 16-bit words in "
        "mgsi.exe+%X..%X follow. Known landmarks that SHOULD appear: 593FA0/A2/A4 (camera position), "
        "593FC8 (pitch) 593FCA (yaw) 593FCC (roll), 593FD0 (\"camera control\"), 594000 (clip "
        "distance). Anything ELSE with a high count that moves when the cutscene camera moves is a "
        "candidate for the camera the cutscene renderer actually reads.",
        g_csvScanSamples, found,
        (unsigned)(g_csvProbeScanBase), (unsigned)(g_csvProbeScanBase + (uintptr_t)(words * 2)));

    for (int i = 0; i < found; ++i) {
        const uintptr_t rva = g_csvProbeScanBase + (uintptr_t)(bestIdx[i] * 2);
        const uint16_t last = g_csvScanPrev[bestIdx[i]];
        DebugLogger::LogFormat(
            "Cutscene probe scan:   mgsi.exe+%X  changed %u/%llu samples  last=%d (0x%04X)%s",
            (unsigned)rva, bestHit[i], g_csvScanSamples,
            (int)(int16_t)last, (unsigned)last,
            (rva == 0x593FA0) ? "  [known: camera pos X]" :
            (rva == 0x593FA2) ? "  [known: camera pos Y]" :
            (rva == 0x593FA4) ? "  [known: camera pos Z]" :
            (rva == 0x593FC8) ? "  [known: pitch -- WE WRITE THIS]" :
            (rva == 0x593FCA) ? "  [known: yaw -- WE WRITE THIS]" :
            (rva == 0x593FCC) ? "  [known: roll]" :
            (rva == 0x593FD0) ? "  [known: camera control]" :
            (rva == 0x594000) ? "  [known: clip distance]" : "");
    }
}

} // namespace

void CutsceneVrFlipTick(unsigned long long flipCount) {
    if (g_moduleBase == 0) return;
    g_csvTickStamp.store(GetTickCount64(), std::memory_order_relaxed);

    // ---- read every candidate signal, always -------------------------------
    // Read even with cutscene_detect_mode=0. These four lines ARE the Cheat
    // Engine hand-test the handoff has been requesting since 2026-08-15, and
    // they cost four guarded loads per presented frame. A session played
    // normally with the feature switched off still comes back with the answer.
    uint8_t modalRaw = 0;   const bool modalOk = SafeRead8(g_moduleBase + kModalStateRva, &modalRaw);
    uint8_t ctlRaw = 0;     const bool ctlOk = SafeRead8(g_moduleBase + kCutsceneControlRva, &ctlRaw);
    int16_t camCtlRaw = 0;  const bool camCtlOk = SafeRead16(g_moduleBase + kCameraControlRva, &camCtlRaw);
    // DEBOUNCED HERE TOO, and not for tidiness. The flag at 0x324898 is
    // documented as noisy -- entering first person produces three transitions
    // in ~200 ms and it drops on its own mid-play, which is the entire reason
    // the rotation hook debounces it. This tick cannot borrow that debounced
    // state, because the rotation hook is exactly the thing that may have
    // stopped, so it keeps its own. Without this, a single flickered frame
    // would be enough to declare a cutscene and start writing the camera.
    static bool fpvDebounced = false;
    static int  fpvPending = 0;
    {
        const bool raw = ReadFpvFlag();
        if (raw != fpvDebounced) {
            if (++fpvPending >= g_fpvDebounceFrames) { fpvDebounced = raw; fpvPending = 0; }
        }
        else {
            fpvPending = 0;
        }
    }
    const bool fpvRaw = fpvDebounced;

    // ---- camera-update liveness --------------------------------------------
    static unsigned long long lastHookRuns = 0;
    static int    flipsSinceHookRun = 0;
    static bool   haveHookRunsRef = false;
    const unsigned long long hookRuns = g_rotHookRuns.load(std::memory_order_relaxed);
    if (!haveHookRunsRef) { lastHookRuns = hookRuns; haveHookRunsRef = true; }
    if (hookRuns != lastHookRuns) { lastHookRuns = hookRuns; flipsSinceHookRun = 0; }
    else if (flipsSinceHookRun < 100000) { flipsSinceHookRun++; }
    const bool hookStalled = (flipsSinceHookRun >= g_cutsceneStallFlips);

    // Publish for the first-person maintenance in vr_injection.cpp. Written
    // unconditionally, including with cutscene detection off, because "can the
    // game take an FPV request right now" is a question about the game, not
    // about the cutscene feature.
    g_csvHookStalled.store(hookStalled, std::memory_order_relaxed);
    g_csvModalState.store(modalOk ? (int)modalRaw : -1, std::memory_order_relaxed);

    // PATH A CANNOT POSSIBLY BE RUNNING WHILE THE HOOK IS STALLED, so clear
    // the flag that gates the position hook rather than leaving it at whatever
    // the hook last wrote before it stopped.
    //
    // Ownership (g_csvActive) is deliberately NOT cleared here. Whether a
    // cutscene is being rendered in VR and whether anything is currently
    // WRITING the camera are different questions, and the 2D-UI framing wants
    // the first one. Clearing ownership on a stall would fade a cutscene onto
    // the distant virtual screen the moment the camera update paused --
    // including the whole of a scene played with cutscene_flip_fallback=0,
    // which is a supported setting.
    if (hookStalled) {
        g_csvInFrame = false;
    }

    // ---- edge logging, capped ----------------------------------------------
    // Every transition of all three bytes, tagged with the FPV flag and
    // whether the camera update is alive at that moment. This is what turns
    // "what do 0x593FD0 and 0x32279F actually do" from a question into a
    // grep. Values other than the ones already known (modal 0/1/4) are called
    // out explicitly, because an unseen value appearing exactly when a
    // cutscene starts is the single most useful thing this can find.
    //
    // SEPARATE BUDGETS, AND A CHATTER DETECTOR, because this project has now
    // twice had a fixed log budget eaten in milliseconds by whatever the
    // noisiest untriaged traffic happened to be (startup colorfills, then the
    // screen-wipe self-blits). 0x593FD0 is only SUSPECTED to be a mode flag;
    // it sits at GM_SnakeCameraWork + 0x30, right next to fields the engine
    // rewrites every single frame, so it is entirely possible it changes
    // constantly. If it does, it would swallow the whole budget and hide the
    // two bytes that actually matter. So it gets its own small budget and a
    // detector that switches it off and says why -- which is itself a useful
    // finding: a value that changes every frame is not a mode switch.
    // THREE INDEPENDENT BUDGETS, one per signal. A single shared budget was
    // the original design and it was wrong in exactly the way this project has
    // now been burned by twice: the two "maybe it's a mode flag" bytes are the
    // ones most likely to turn out to be per-frame traffic, and sharing a
    // budget with them means their noise buries the modal/FPV/stall
    // transitions -- which are the three signals the feature is actually gated
    // on. Separate budgets make that impossible by construction, rather than
    // relying on a chatter detector firing in time to save them.
    {
        static int lastModal = -2, lastFpv = -1, lastStall = -1;
        static int changeLog = 0;
        const int modalNow = modalOk ? (int)modalRaw : -1;
        const int fpvNow = fpvRaw ? 1 : 0;
        const int stallNow = hookStalled ? 1 : 0;

        if (modalNow != lastModal || fpvNow != lastFpv || stallNow != lastStall) {
            if (changeLog < 150) {
                changeLog++;
                const char* modalName =
                    (modalNow == 0) ? "gameplay" :
                    (modalNow == 1) ? "PAUSE MENU" :
                    (modalNow == 4) ? "inventory" :
                    (modalNow < 0) ? "unreadable" : "UNKNOWN VALUE -- worth knowing what this is";
                DebugLogger::LogFormat(
                    "Cutscene probe @flip #%llu: modal(391A0C)=%d (%s) | fpv_flag=%d | camera update "
                    "%s (%d flips since its last run) | cutscene_control(32279F)=0x%02X | "
                    "camera_control(593FD0)=%d",
                    flipCount, modalNow, modalName, fpvNow,
                    hookStalled ? "STALLED" : "alive", flipsSinceHookRun,
                    (unsigned)(ctlOk ? ctlRaw : 0), camCtlOk ? (int)camCtlRaw : -1);
            }
            lastModal = modalNow; lastFpv = fpvNow; lastStall = stallNow;
        }
    }

    // Both of the candidate bytes get the same treatment: their own small
    // budget, and a chatter detector that reports "this changes every frame,
    // so it is not a mode switch" -- which is itself a real finding and kills
    // a lead that has been sitting open in the handoff for a week.
    {
        static int  lastCtl = -0x7FFF, lastCamCtl = -0x7FFF;
        static int  ctlLog = 0, camCtlLog = 0;
        static int  ctlChanges = 0, camCtlChanges = 0;
        static bool ctlChatty = false, camCtlChatty = false;
        const int ctlNow = ctlOk ? (int)ctlRaw : -1;
        const int camCtlNow = camCtlOk ? (int)camCtlRaw : -0x7FFE;

        if (ctlNow != lastCtl) {
            lastCtl = ctlNow;
            ctlChanges++;
            if (!ctlChatty && ctlChanges > 40) {
                ctlChatty = true;
                DebugLogger::LogFormat(
                    "Cutscene probe: mgsi.exe+32279F (\"Cutscene control\") has already changed %d "
                    "times by flip #%llu -- that is a busy bitfield, not a scene flag. Further "
                    "changes are not logged. cutscene_detect_mode=2 is unlikely to be useful.",
                    ctlChanges, flipCount);
            }
            if (!ctlChatty && ctlLog < 60) {
                ctlLog++;
                DebugLogger::LogFormat(
                    "Cutscene probe: cutscene_control(32279F) -> 0x%02X at flip #%llu "
                    "(fpv=%d modal=%d camera update %s)",
                    (unsigned)(ctlNow < 0 ? 0 : ctlNow), flipCount, fpvRaw ? 1 : 0,
                    modalOk ? (int)modalRaw : -1, hookStalled ? "STALLED" : "alive");
            }
        }

        if (camCtlNow != lastCamCtl) {
            lastCamCtl = camCtlNow;
            camCtlChanges++;
            if (!camCtlChatty && camCtlChanges > 40) {
                camCtlChatty = true;
                DebugLogger::LogFormat(
                    "Cutscene probe: mgsi.exe+593FD0 (\"Camera control\") has already changed %d times "
                    "by flip #%llu. That is per-frame traffic, not a mode switch -- it sits at "
                    "GM_SnakeCameraWork + 0x30, next to fields the engine rewrites every frame, so the "
                    "community table's label is probably wrong and writing 1 to it will not hand "
                    "camera authority over. Further changes are not logged.",
                    camCtlChanges, flipCount);
            }
            if (!camCtlChatty && camCtlLog < 40) {
                camCtlLog++;
                DebugLogger::LogFormat(
                    "Cutscene probe: camera_control(593FD0) -> %d at flip #%llu (fpv=%d modal=%d "
                    "camera update %s)",
                    camCtlNow, flipCount, fpvRaw ? 1 : 0, modalOk ? (int)modalRaw : -1,
                    hookStalled ? "STALLED" : "alive");
            }
        }
    }

    // A stall is worth its own loud line the first few times, because it is
    // the answer to the one question that decides whether this feature can
    // work the good way or only the fallback way.
    {
        static bool lastStalled = false;
        static int stallLog = 0;
        if (hookStalled != lastStalled) {
            lastStalled = hookStalled;
            if (stallLog < 20) {
                stallLog++;
                DebugLogger::LogFormat(
                    "Cutscene VR: the game's camera update %s at flip #%llu (fpv_flag=%d, "
                    "modal=%d). %s",
                    hookStalled ? "STOPPED" : "resumed", flipCount, fpvRaw ? 1 : 0,
                    modalOk ? (int)modalRaw : -1,
                    hookStalled
                        ? "While it is stopped neither camera hook can run, so path A is unavailable "
                          "and the flip-time fallback is the only way to move the camera."
                        : "Path A is available again -- the fallback stands down.");
            }
        }
    }

    if (g_cutsceneDetectMode == 0) {
        g_csvDetected.store(false, std::memory_order_relaxed);
        g_csvActive.store(false, std::memory_order_relaxed);
        g_csvFallbackDriving.store(false, std::memory_order_relaxed);
        CutsceneFallbackReset();
        return;
    }

    // ---- the verdict --------------------------------------------------------
    // Modal UI is excluded from every mode. The pause menu and the inventory
    // screen also stop the camera update, so a stall-based detector would
    // otherwise call them cutscenes and start driving the camera behind a
    // menu, which is both wrong and the sort of thing that is very confusing
    // to debug from a headset.
    const bool modalUi = modalOk && (modalRaw != 0);
    const bool controlSays = ctlOk && ((ctlRaw & (uint8_t)g_cutsceneControlMask) != 0);

    bool cutscene = false;
    if (!fpvRaw && !modalUi && g_enabled.load(std::memory_order_relaxed)) {
        switch (g_cutsceneDetectMode) {
        case 1: cutscene = controlSays || hookStalled; break;
        case 2: cutscene = controlSays; break;
        case 3: cutscene = hookStalled; break;
        case 4: cutscene = true; break;
        default: cutscene = false; break;
        }
    }
    g_csvDetected.store(cutscene, std::memory_order_relaxed);
    // g_csvActive is normally maintained by the rotation hook, which is
    // exactly the thing that may have stopped. Clearing it here as well means
    // "the cutscene ended while the camera update was still stopped" (a pause
    // menu opened straight out of a scene, say) cannot leave a stale true
    // behind that keeps the 2D-UI framing switched off.
    if (!cutscene) g_csvActive.store(false, std::memory_order_relaxed);

    {
        static bool lastCutscene = false;
        static int verdictLog = 0;
        if (cutscene != lastCutscene) {
            lastCutscene = cutscene;
            if (verdictLog < 60) {
                verdictLog++;
                DebugLogger::LogFormat(
                    "Cutscene VR: cutscene %s at flip #%llu (mode=%d, control byte says %d, "
                    "camera update stalled %d)",
                    cutscene ? "DETECTED" : "ended", flipCount, g_cutsceneDetectMode,
                    controlSays ? 1 : 0, hookStalled ? 1 : 0);
            }
            if (!cutscene) CutsceneProbeScanReport();
            CutsceneProbeScanReset();
        }
    }

    // ---- THE PROBE SCAN ----------------------------------------------------
    // Runs in BOTH view modes and whether or not anything is driving: it is
    // pure observation, and the most interesting version of it is the one taken
    // while the mod is writing nothing at all, because then every word that
    // moves belongs to the game.
    if (cutscene && g_csvProbeScan) {
        static int scanPhase = 0;
        if (++scanPhase >= g_csvProbeScanEvery) {
            scanPhase = 0;
            CutsceneProbeScanSample();
        }
    }

    // ---- camera authority experiment ---------------------------------------
    // Read-first, write-only-if-asked, restore-on-exit. Both of these are
    // hypotheses from a community table, so neither is written by default and
    // both are put back exactly as they were found the moment the cutscene
    // ends -- leaving "Camera control" latched at 1 into normal gameplay is
    // precisely the kind of thing that would look like a completely unrelated
    // bug three sessions later.
    // VR-mode gated like every other write. Note the ELSE branch is deliberately
    // NOT gated: if the player switches to native mid-cutscene while an
    // authority byte is being held, the restore has to still happen, or a value
    // the mod forced would be left latched into the game with nothing left
    // running to put it back.
    if (cutscene && g_vrViewMode.load(std::memory_order_relaxed)) {
        if (g_cutsceneCameraControlWrite >= 0) {
            if (!g_csvSavedCameraControl && camCtlOk) {
                g_csvOriginalCameraControl = camCtlRaw;
                g_csvSavedCameraControl = true;
                DebugLogger::LogFormat(
                    "Cutscene VR: taking camera authority -- mgsi.exe+593FD0 was %d, writing %d "
                    "every frame until the cutscene ends",
                    (int)camCtlRaw, g_cutsceneCameraControlWrite);
            }
            SafeWrite16(g_moduleBase + kCameraControlRva, (int16_t)g_cutsceneCameraControlWrite);
        }
        if (g_cutsceneControlWrite >= 0) {
            if (!g_csvSavedCutsceneControl && ctlOk) {
                g_csvOriginalCutsceneControl = ctlRaw;
                g_csvSavedCutsceneControl = true;
                DebugLogger::LogFormat(
                    "Cutscene VR: mgsi.exe+32279F was 0x%02X, writing %d every frame until the "
                    "cutscene ends", (unsigned)ctlRaw, g_cutsceneControlWrite);
            }
            SafeWrite8(g_moduleBase + kCutsceneControlRva, (uint8_t)g_cutsceneControlWrite);
        }
    }
    else {
        if (g_csvSavedCameraControl) {
            SafeWrite16(g_moduleBase + kCameraControlRva, g_csvOriginalCameraControl);
            g_csvSavedCameraControl = false;
            DebugLogger::LogFormat("Cutscene VR: restored mgsi.exe+593FD0 to %d",
                (int)g_csvOriginalCameraControl);
        }
        if (g_csvSavedCutsceneControl) {
            SafeWrite8(g_moduleBase + kCutsceneControlRva, g_csvOriginalCutsceneControl);
            g_csvSavedCutsceneControl = false;
            DebugLogger::LogFormat("Cutscene VR: restored mgsi.exe+32279F to 0x%02X",
                (unsigned)g_csvOriginalCutsceneControl);
        }
    }

    // ---- PATH B: flip-time fallback ----------------------------------------
    // Runs ONLY while path A cannot. The two are mutually exclusive by
    // construction -- one writer per value per frame -- and the gate is the
    // measured stall, not a guess.
    //
    // Detection above runs in BOTH view modes on purpose -- it is
    // instrumentation and costs four guarded loads -- but writing is VR-mode
    // only, like every other write in this file. That split is also what makes
    // the probe scan useful in native mode: with nothing of ours touching the
    // camera block, every word it sees move belongs to the game.
    //
    // 2026-08-23: PATH B IS NOW DEAD BY MEASUREMENT, not by suspicion. The yaw
    // pulse it carried proved that during a cutscene nothing reads rotate2 --
    // the writes landed perfectly and the picture did not move. Static analysis
    // then explained why: the cutscene camera script writes layer 2 directly,
    // so the copy at 0x53D4B never runs and layer 1 is disconnected. The view
    // commit hook at 0x53C47 does the job PATH B was invented for, one layer
    // lower, and does it in stereo. The code stays for now, gated off, because
    // it is the control case if the new hook ever fails to install.
    const bool vcOwnsCutscene = (g_vcInstalled && g_vcMode != 0);
    const bool wantFallback =
        cutscene && hookStalled && g_cutsceneFlipFallback && g_cutsceneRotation &&
        !vcOwnsCutscene &&
        g_vrViewMode.load(std::memory_order_relaxed) &&
        g_havePose.load(std::memory_order_relaxed) &&
        g_enabled.load(std::memory_order_relaxed);

    if (!wantFallback) {
        if (g_csvFallbackDriving.exchange(false, std::memory_order_relaxed)) {
            // NOTE, because the obvious worry here is wrong and it cost a
            // review round to establish that. It looks as though the in-frame
            // path would re-apply the head delta on top of the value the
            // fallback left behind, since it is additive on the live angle.
            // It cannot: the game's own rotate2 store lives at 0xE1AE7, and
            // our rotation hook is at 0xE1AF8 -- seventeen bytes LATER in the
            // same function. On any frame the camera update runs, the game has
            // already overwritten whatever the fallback left before we read
            // the live value, so the additive base is always the game's.
            // Nothing needs re-anchoring here, and forcing a re-anchor would
            // itself snap the view by the whole accumulated head offset.
            DebugLogger::Log("Cutscene VR: flip-time fallback stood down");
        }
        CutsceneFallbackReset();
        return;
    }

    int16_t liveYaw = 0, livePitch = 0;
    if (!SafeRead16(g_moduleBase + kYawOffset, &liveYaw) ||
        !SafeRead16(g_moduleBase + kPitchOffset, &livePitch)) {
        return;
    }
    const int32_t nowYaw = (int32_t)liveYaw & 0x0FFF;
    const int32_t nowPitch = (int32_t)livePitch & 0x0FFF;

    if (!g_csvFbHaveAnchor) {
        g_csvFbAnchorYaw = nowYaw;
        g_csvFbAnchorPitch = nowPitch;
        g_csvFbHaveAnchor = true;
        g_csvFbHaveLastWrote = false;

        // RE-PIN THE HEAD REFERENCE ONLY ON FIRST ENGAGEMENT. Zeroing the head
        // offset is what makes the fallback take over seamlessly the first
        // time. It is the wrong thing to do on the OTHER route into this
        // branch -- a failed camera write, which clears the anchor so it can
        // re-derive from the live value -- because re-pinning there throws
        // away however far the player had turned their head and snaps the view
        // back by exactly that much, every frame the failure persists. Note a
        // partial failure (yaw landed, pitch did not) means the camera has
        // already moved, so this is reachable with the view mid-flight.
        if (!g_csvFallbackDriving.exchange(true, std::memory_order_relaxed)) {
            CutsceneFallbackPinReference();
            // BUDGETED, even though this is an edge. A persistent write
            // failure stands the fallback all the way down every frame, and
            // each stand-down makes the next frame look like a fresh
            // engagement -- so an unbudgeted banner here would print six lines
            // per frame at 30 fps, doing file I/O on the game's thread, for as
            // long as the fault lasted. The pin stays unconditional; only the
            // logging is capped.
            static int engageLog = 0;
            if (engageLog < 10) {
                engageLog++;
                DebugLogger::LogFormat(
                    "Cutscene VR: ENGAGED via PATH B (flip-time fallback) at flip #%llu. The camera "
                    "update is stopped, so rotate2 is being written from the DirectDraw Flip hook -- "
                    "same thread, once per presented frame, one frame later than the in-frame path. "
                    "Stereo stays MONO here by design; the eye tag cannot be advanced correctly at "
                    "this point in the frame. Anchor pinned at yaw=%d pitch=%d.",
                    flipCount, nowYaw, nowPitch);
            }
        }
    }
    else if (g_csvFbHaveLastWrote) {
        // ANCHOR FOLLOWS THE GAME. Whatever the game itself did to the value
        // since our last write is exactly (live - lastWrote), so adding that
        // to the anchor tracks scripted pans at any speed and survives shot
        // changes, while a game that touched nothing contributes zero and our
        // own write cannot compound into itself.
        const int32_t gameDYaw = AngleDeltaUnits(nowYaw, g_csvFbLastWroteYaw);
        const int32_t gameDPitch = AngleDeltaUnits(nowPitch, g_csvFbLastWrotePitch);
        g_csvFbAnchorYaw = WrapUnits(g_csvFbAnchorYaw + gameDYaw);
        g_csvFbAnchorPitch = WrapUnits(g_csvFbAnchorPitch + gameDPitch);

        // A jump this large is a cut to a new shot, not a pan. Re-pin the head
        // reference so the angle you happened to be looking at during the last
        // shot is not carried into the new one -- the same reasoning as the
        // room-cut guard in the in-frame path.
        const int32_t jump = (gameDYaw < 0 ? -gameDYaw : gameDYaw)
                           + (gameDPitch < 0 ? -gameDPitch : gameDPitch);
        if (jump > g_cutsceneReanchorUnits) {
            CutsceneFallbackPinReference();
            static int shotLog = 0;
            if (shotLog < 20) {
                shotLog++;
                DebugLogger::LogFormat(
                    "Cutscene VR (fallback): shot change -- the game moved the camera %d units in "
                    "one frame (threshold %d), re-pinning the head reference to the new shot",
                    jump, g_cutsceneReanchorUnits);
            }
        }
    }

    int32_t dYaw = 0, dPitch = 0;
    CutsceneFallbackHeadDelta(&dYaw, &dPitch);

    // ---- THE PROBE PULSE ---------------------------------------------------
    // Added to the yaw we write, switched on and off on a fixed period, and
    // folded in BEFORE the write so that g_csvFbLastWroteYaw records the value
    // including it. That ordering is what stops the pulse from being mistaken
    // for the game moving the camera: next frame reads back exactly what we
    // wrote, the game-delta is zero, and the anchor does not drift by 1024
    // units every time the pulse flips. Recording the un-pulsed value instead
    // would walk the anchor a quarter turn per period.
    int32_t probeYaw = 0;
    if (g_csvProbePulseUnits > 0) {
        const unsigned long long nowMs = GetTickCount64();
        if (g_csvProbePulseEdgeMs == 0 ||
            (nowMs - g_csvProbePulseEdgeMs) >= (unsigned long long)g_csvProbePulseMs) {
            g_csvProbePulseEdgeMs = nowMs;
            g_csvProbePulseOn = !g_csvProbePulseOn;
            static int pulseLog = 0;
            if (pulseLog < 12) {
                pulseLog++;
                DebugLogger::LogFormat(
                    "Cutscene probe: pulse %s (%+d units on the written yaw). If the picture is "
                    "swinging in time with these lines, rotate2 IS read during cutscenes. If the "
                    "scene looks completely normal, it is not, and the fallback can never work as "
                    "designed -- the cutscene renderer is reading some other camera.",
                    g_csvProbePulseOn ? "ON " : "OFF", g_csvProbePulseOn ? g_csvProbePulseUnits : 0);
            }
        }
        if (g_csvProbePulseOn) probeYaw = g_csvProbePulseUnits;
    }

    // Two yaws, deliberately. writeYawHonest is where the head is actually
    // pointing; writeYaw is that plus the probe. The position block below
    // rotates the head-lean offset into camera space and must use the honest
    // one -- rotating a lean by a yaw the player's head is not at would send
    // leaning sideways in a direction that changes every time the pulse flips.
    const int32_t writeYawHonest = WrapUnits(g_csvFbAnchorYaw + dYaw);
    const int32_t writeYaw = WrapUnits(writeYawHonest + probeYaw);
    const int32_t writePitch = WrapUnits(g_csvFbAnchorPitch + dPitch);

    // The write's success is NOT optional bookkeeping. The anchor-follows-the-
    // game scheme derives the game's own motion from (live - lastWrote), so
    // recording a write that did not land makes the next frame read our own
    // delta back as if the game had moved the camera by it -- and the anchor
    // then walks by the head offset every frame, which is precisely the
    // self-compounding runaway this scheme exists to prevent. See the failure
    // branch below for why the recovery is a full stand-down rather than
    // anything cheaper.
    const bool yawOk = !g_writeYaw ||
        SafeWrite16(g_moduleBase + kYawOffset, (int16_t)writeYaw);
    const bool pitchOk = !g_writePitch ||
        SafeWrite16(g_moduleBase + kPitchOffset, (int16_t)writePitch);

    if (!(yawOk && pitchOk)) {
        // FULL STAND-DOWN, not a partial recovery, and the reasoning is worth
        // keeping because two cheaper-looking fixes are both wrong.
        //
        // Nothing but us writes rotate2 while the camera update is stalled, so
        // the live value IS our last write. Clearing only the last-wrote record
        // freezes the anchor at a stale value. Clearing the anchor as well is
        // worse: the next frame sets anchor = live = (anchor + delta), and
        // because the head reference is deliberately NOT re-pinned outside
        // first engagement, the same delta is added again -- A+d, A+2d, A+3d,
        // the exact self-compounding runaway this scheme exists to prevent,
        // now reachable through the recovery path itself. It is reachable in
        // practice because yaw and pitch succeed or fail independently.
        //
        // Standing all the way down costs one re-centre on what is by then a
        // genuine memory fault, and re-engages next frame through the
        // first-engagement path with the anchor and the head reference pinned
        // together -- the only state this scheme is correct in.
        CutsceneFallbackReset();
        g_csvFallbackDriving.store(false, std::memory_order_relaxed);
        static int failLog = 0;
        if (failLog < 5) {
            failLog++;
            DebugLogger::Log("Cutscene VR (fallback): a camera write failed -- standing the fallback "
                "all the way down so the anchor cannot compound, and re-engaging cleanly next frame. "
                "If this repeats, something else is unmapping the game's camera block.");
        }
        return;
    }

    g_csvFbLastWroteYaw = g_writeYaw ? writeYaw : nowYaw;
    g_csvFbLastWrotePitch = g_writePitch ? writePitch : nowPitch;
    g_csvFbHaveLastWrote = true;

    // FOV, for the same reason it is written every frame in the in-frame path:
    // the field is per-camera and script-overridable, and a cutscene changes
    // cameras constantly.
    if (g_cutsceneFov && g_fovClipDistance != 0) {
        SafeWrite16(g_moduleBase + kFovClipDistanceRva, (int16_t)g_fovClipDistance);
    }

    // ---- position through the fallback, OFF by default ---------------------
    // The in-frame position write is only correct because the rotation hook
    // un-applies last frame's offset before the game's own interpolator
    // (GV_NearExp4PV) reads the value back. At flip time that ordering does
    // not exist. During a genuine stall the interpolator is not running
    // either, so the same anchor-follows-the-game scheme used for rotation is
    // sound -- but "the interpolator is definitely not running" is an
    // assumption, not a measurement, so this ships off. Turn it on once the
    // rotation half is confirmed working and see whether leaning during a
    // cutscene behaves or drifts.
    if (g_cutsceneFallbackPosition && g_positionEnabled && g_cutscenePosition) {
        int16_t gx = 0, gy = 0, gz = 0;
        if (SafeRead16(g_moduleBase + kPosXOffset, &gx) &&
            SafeRead16(g_moduleBase + kPosYOffset, &gy) &&
            SafeRead16(g_moduleBase + kPosZOffset, &gz)) {

            if (!g_csvFbHaveLastPos) {
                g_csvFbAnchorPosX = gx; g_csvFbAnchorPosY = gy; g_csvFbAnchorPosZ = gz;
                g_csvFbHaveLastPos = true;
            }
            else {
                g_csvFbAnchorPosX += (int32_t)gx - g_csvFbLastWroteX;
                g_csvFbAnchorPosY += (int32_t)gy - g_csvFbLastWroteY;
                g_csvFbAnchorPosZ += (int32_t)gz - g_csvFbLastWroteZ;
            }

            float dx = g_headPosX.load(std::memory_order_relaxed) - g_csvFbRefHeadPosX;
            float dy = g_headPosY.load(std::memory_order_relaxed) - g_csvFbRefHeadPosY;
            float dz = g_headPosZ.load(std::memory_order_relaxed) - g_csvFbRefHeadPosZ;
            if (g_invertPosX) dx = -dx;
            if (g_invertPosY) dy = -dy;
            if (g_invertPosZ) dz = -dz;

            if (g_rotatePositionWithView) {
                float camYaw = ((float)writeYawHonest / kUnitsPerTurn) * kTwoPi + g_positionYawOffsetRad;
                if (g_positionHandednessFlip) { dz = -dz; camYaw = -camYaw; }
                const float c = std::cos(camYaw), s = std::sin(camYaw);
                const float rx = dx * c + dz * s;
                const float rz = -dx * s + dz * c;
                dx = rx; dz = rz;
            }

            const int32_t lim = g_maxPositionOffset;
            const int32_t ox = Clamp((int32_t)(dx * g_positionScale), -lim, lim);
            const int32_t oy = Clamp((int32_t)(dy * g_positionScale), -lim, lim);
            const int32_t oz = Clamp((int32_t)(dz * g_positionScale), -lim, lim);

            const int32_t wx = g_csvFbAnchorPosX + ox;
            const int32_t wy = g_csvFbAnchorPosY + oy;
            const int32_t wz = g_csvFbAnchorPosZ + oz;
            const bool posOk =
                SafeWrite16(g_moduleBase + kPosXOffset, (int16_t)wx) &&
                SafeWrite16(g_moduleBase + kPosYOffset, (int16_t)wy) &&
                SafeWrite16(g_moduleBase + kPosZOffset, (int16_t)wz);
            if (posOk) {
                g_csvFbLastWroteX = wx; g_csvFbLastWroteY = wy; g_csvFbLastWroteZ = wz;
            }
            else {
                g_csvFbHaveLastPos = false;   // same reasoning as the rotation write above
            }
        }
    }

    // Path B is mono, always. Publishing -1 here rather than leaving the tag
    // to expire on its own is the difference between "both eyes get the
    // cutscene" and "one eye holds the last frame of gameplay for a minute".
    g_publishedEye.store(-1, std::memory_order_relaxed);
    g_eyePublishTick.store(GetTickCount64(), std::memory_order_relaxed);

    static int fbLog = 0;
    if (fbLog < g_logFrames || (fbLog % 120) == 0) {
        DebugLogger::TestLogFormat(
            "CsvFb[%d]: anchor(yaw=%d pitch=%d) + head(yaw=%d pitch=%d) + probe(yaw=%d) "
            "-> wrote(yaw=%d pitch=%d) | live was(yaw=%d pitch=%d)",
            fbLog, g_csvFbAnchorYaw, g_csvFbAnchorPitch, dYaw, dPitch, probeYaw,
            writeYaw, writePitch, nowYaw, nowPitch);
    }
    fbLog++;
}

// Shared gate for both public cutscene accessors, so the two cannot drift.
// Every cutscene flag is written only by the flip tick, and the flip tick sits
// inside `if (SUCCEEDED(hr))` in the Flip hook -- a lost DirectDraw surface
// makes it stop while everything else keeps running, and the flags then latch
// at whatever they last were. See the note at g_csvTickStamp.
static bool CutsceneStateIsFresh() {
    if (!g_enabled.load(std::memory_order_relaxed)) return false;

    const unsigned long long stamped = g_csvTickStamp.load(std::memory_order_relaxed);
    if (stamped == 0) return false;

    static bool loggedStale = false;
    if ((GetTickCount64() - stamped) > kCsvTickStaleMs) {
        if (!loggedStale) {
            loggedStale = true;
            DebugLogger::Log("Cutscene VR: the flip tick has stopped running for over a second "
                "(the DirectDraw Flip hook is no longer succeeding -- lost surface, alt-tab, "
                "display mode change, or a very long load). Cutscene state is reported inactive "
                "rather than left latched at whatever it last was.");
        }
        return false;
    }
    // Reset the latch when it comes back, so one transient gap early in a
    // session cannot silence a genuinely stuck one later. Same reasoning as
    // the auto-restore deferral's log latches in vr_injection.cpp.
    loggedStale = false;
    return true;
}

// True while cutscene VR owns the frame. Anything that treats "not in first
// person" as "this is flat 2D UI" has to consult this, exactly as it already
// has to consult IsThirdPersonVrActive() -- a cutscene being rendered in VR is
// a scene you are standing inside, not a menu to be shrunk onto a distant
// virtual screen.
bool IsCutsceneVrActive() {
    // DETECTION, not "something is writing right now". The flip tick maintains
    // g_csvDetected authoritatively every presented frame whether or not
    // either path is driving, which is exactly what a caller asking "is this
    // frame a cutscene we are rendering in VR" needs. Keyed off the writing
    // flags instead, this would go false during a shot-cut hold, during a
    // stall with the fallback disabled, and for any frame the head pose
    // dropped out -- flicking the scene onto a distant 2D screen and back each
    // time.
    // Snake's eyes counts from its very first frame, before the stall
    // detector has had its six flips to notice the cutscene.
    if (IsCutsceneSnakePovActive()) return true;
    if (!CutsceneStateIsFresh()) return false;
    return g_csvDetected.load(std::memory_order_relaxed) ||
        g_csvActive.load(std::memory_order_relaxed) ||
        g_csvFallbackDriving.load(std::memory_order_relaxed);
}

// True only while the flip-time fallback is the one doing the writing, i.e.
// the game's camera update is confirmed stopped. Separate from the above
// because the two paths have different capabilities -- notably the fallback
// is always mono -- and callers may care which is live.
bool IsCutsceneVrFallbackDriving() {
    if (!CutsceneStateIsFresh()) return false;
    return g_csvFallbackDriving.load(std::memory_order_relaxed);
}

// Debounced FPV state, maintained by the rotation hook from the game's own
// flag. Reading a plain bool written on the game thread from the input thread
// is benign here: it is a single aligned bool, and a one-frame-stale answer
// only costs one frame of control routing.
//
// DELIBERATELY NOT GATED ON VIEW MODE. This answers "is MGS1 in first person",
// which stays a fact about the game whichever mode the mod is in. Callers that
// mean "is the mod driving a first-person VR camera" want
// IsFpvActive() && IsVrViewModeActive(), and the two are different questions --
// conflating them here would make the mode maintenance below unable to see the
// state it exists to maintain.
bool IsFpvActive() {
    return g_enabled.load(std::memory_order_relaxed) && g_lastFpvState;
}

// ---------------------------------------------------------------------------
// VIEW MODE
// ---------------------------------------------------------------------------
bool IsVrViewModeActive() {
    return g_vrViewMode.load(std::memory_order_relaxed);
}

void SetVrViewMode(bool vrOn) {
    const bool was = g_vrViewMode.exchange(vrOn, std::memory_order_relaxed);
    if (was == vrOn) return;

    if (!vrOn) {
        // Leaving VR. Everything the mod was holding has to be handed back in
        // the same breath, not left to decay:
        //
        //  - The head reference, so that returning to VR re-pins to wherever
        //    the game's camera is then rather than snapping by however far the
        //    head moved while native mode was on.
        //  - The stick-rotation offset, which vr_input.cpp applies to movement
        //    whenever it is valid. Left valid, native mode would keep steering
        //    Snake by a frozen head angle -- the same class of bug as the
        //    diorama making him spin on the spot.
        //  - The stereo eye tag, so the capture path stops alternating and
        //    sends every frame to both eyes.
        //
        // The position offset does NOT need un-applying here. The rotation
        // hook un-applies the previous frame's offset before its mode gate,
        // unconditionally, so the frame after this call hands the game back its
        // own position whether or not we ever write again.
        //
        // THE RE-PIN GOES THROUGH g_recenterRequested, NOT g_haveReference.
        // This function runs on the INPUT thread; g_haveReference is a plain
        // bool owned by the game thread inside the rotation hook, and writing
        // it from here is a straightforward data race on a variable the hook
        // both reads and writes in the same breath. g_recenterRequested is an
        // atomic that exists for exactly this purpose and is consumed with an
        // exchange, so the hand-off is single-owner in both directions.
        g_recenterRequested.store(true, std::memory_order_relaxed);
        g_yawOffsetValid.store(false, std::memory_order_relaxed);
        g_publishedEye.store(-1, std::memory_order_relaxed);
        g_eyePublishTick.store(GetTickCount64(), std::memory_order_relaxed);
        g_fovRestorePending.store(true, std::memory_order_relaxed);
        RequestVirtualScreenRecenter();
    }
    else {
        // Entering VR. Re-pin on the next hooked frame, same atomic hand-off.
        g_recenterRequested.store(true, std::memory_order_relaxed);
        // An explicit R3 into VR mode is the player establishing first person
        // just as surely as the game granting it, so arm the maintenance loop
        // here too. Otherwise the very first R3 of a session -- the one where
        // the game refuses because a scene is still playing -- would toggle the
        // mode, fail to enter first person, and leave nothing watching for the
        // moment it becomes possible.
        g_fpvMaintenanceArmed.store(true, std::memory_order_relaxed);
    }

    DebugLogger::LogFormat(
        "View mode: %s. %s",
        vrOn ? "VR" : "NATIVE",
        vrOn ? "The mod is driving the camera again; head tracking, stereo and the wide FOV are back."
             : "The mod now writes nothing to the game. The picture is on a world-locked virtual "
               "screen -- turn your head and it stays where it is. R3 double-press to come back.");
}

bool ToggleVrViewMode() {
    const bool next = !g_vrViewMode.load(std::memory_order_relaxed);
    SetVrViewMode(next);
    return next;
}

void RequestVirtualScreenRecenter() {
    g_virtualScreenRecenter.store(true, std::memory_order_relaxed);
}

bool ConsumeVirtualScreenRecenter() {
    return g_virtualScreenRecenter.exchange(false, std::memory_order_relaxed);
}

bool VrModeWantsFirstPerson() {
    // A wall press holds first person off on purpose; a vehicle's own first
    // person is the broken "under the map" one -- Snake's eyes covers both.
    if (g_fpvHold.load(std::memory_order_relaxed)) return false;
    {
        const unsigned long long v = g_gpPovVehicleTick.load(std::memory_order_relaxed);
        if (v != 0 && GetTickCount64() - v < 1500) return false;
    }
    return g_vrViewMode.load(std::memory_order_relaxed) &&
        g_enabled.load(std::memory_order_relaxed) &&
        g_fpvMaintenanceArmed.load(std::memory_order_relaxed) &&
        !g_lastFpvState;
}

// The game's own stick frame: GV pad origin 0x6C03A4 (set by every camera mode
// through +40AC17, read by the pad code at +40A966: dir = table + origin; the
// table gives UP = 2048, RIGHT = 1024, DOWN = 0, LEFT = 3072). So pushing UP
// moves Snake along yaw (origin + 2048), in the same units as his facing.
// 2026-10-02 round 8: the camera's layer-3 direction is NOT always that frame
// (REX top uses another camera mode), which is why movement fought you.
bool GetGamePadFrameYaw(float* out) {
    int32_t o = 0;
    if (!out || !g_moduleBase || !SafeRead32(g_moduleBase + kPlayerHeadingRva, &o)) return false;
    *out = NormalizeRad((float)((o + 2048) & 0xFFF) * (2.0f * kPi / 4096.0f));
    return true;
}

bool GetFirstPersonHoldAnchor(float* out) {
    if (!out || !g_fpvHold.load(std::memory_order_relaxed)) return false;
    *out = g_fpvHoldAnchorYaw.load(std::memory_order_relaxed);
    return true;
}

bool GetGameplayPovStickYaw(float* viewYaw, float* gameYaw) {
    if (!g_gpPovStickFollowsView || !IsGameplayPovActive() || !g_gpPovGameYawOk.load(std::memory_order_relaxed))
        return false;
    // 1 (default): only where it was asked for -- the REX-top fight and the
    // wall press. Ladders keep the game's own up/down, and the jeep gun is
    // left as it was. 2: every Snake's-eyes moment.
    if (g_gpPovStickFollowsView == 1 && !g_fpvHold.load(std::memory_order_relaxed) &&
        !(IsGameplayPovInVehicle() && g_gpPovVehicleKind.load(std::memory_order_relaxed) == 2))
        return false;
    if (viewYaw) *viewYaw = g_gpPovViewYaw.load(std::memory_order_relaxed);
    // Wall press: "into the wall" and "along it" are fixed by where you were
    // facing when you pressed X -- looking around must not swing them.
    if (viewYaw && g_fpvHold.load(std::memory_order_relaxed)) *viewYaw = g_fpvHoldAnchorYaw.load(std::memory_order_relaxed);
    if (gameYaw) {
        float pf = 0.0f;
        *gameYaw = GetGamePadFrameYaw(&pf) ? pf : g_gpPovGameYaw.load(std::memory_order_relaxed);
    }
    return true;
}

bool GameplayPovWantsStickTurn() {
    if (!g_gpPovRightStickTurn || !IsGameplayPovActive() || g_fpvHold.load(std::memory_order_relaxed)) return false;
    if (g_gpPovRightStickTurn == 2) return true;
    // Round 10: any vehicle (REX top AND the jeep escape -- "can we have my
    // ability to move the camera back during the jeep escape"). Not the rappel
    // rope: only your head turns the view there.
    return IsGameplayPovInVehicle() && g_gpPovVehicleKind.load(std::memory_order_relaxed) != 3 &&
        g_gpPovVehicleKind.load(std::memory_order_relaxed) != 4;
}

bool GetGameplayPovPair(int16_t from[3], int16_t to[3]) {
    const unsigned long long t = g_gpPovPairTick.load(std::memory_order_relaxed);
    if (!t || GetTickCount64() - t > 150 || !IsGameplayPovActive()) return false;
    const uint64_t a = g_gpPovPairA.load(std::memory_order_relaxed), b = g_gpPovPairB.load(std::memory_order_relaxed);
    from[0] = (int16_t)(a & 0xFFFF); from[1] = (int16_t)((a >> 16) & 0xFFFF); from[2] = (int16_t)((a >> 32) & 0xFFFF);
    to[0] = (int16_t)((a >> 48) & 0xFFFF); to[1] = (int16_t)(b & 0xFFFF); to[2] = (int16_t)((b >> 16) & 0xFFFF);
    return true;
}

bool GetGameplayPovRexBody(uint32_t* obj, uint32_t* objs) {
    if (!IsGameplayPovInVehicle() || g_gpPovVehicleKind.load(std::memory_order_relaxed) != 2) return false;
    const uint32_t o = g_gpPovVehObj.load(std::memory_order_relaxed), b = g_gpPovVehObjs.load(std::memory_order_relaxed);
    if (!o || !b) return false;
    if (obj) *obj = o;
    if (objs) *objs = b;
    return true;
}

void AddGameplayPovYaw(float rad) {
    float cur = g_gpPovYawAdd.load(std::memory_order_relaxed);
    while (!g_gpPovYawAdd.compare_exchange_weak(cur, cur + rad, std::memory_order_relaxed)) {}
}

void SetFirstPersonHold(bool on, bool haveAnchor, float anchorYaw) {
    if (on) {
        g_fpvHoldAnchorYaw.store(anchorYaw, std::memory_order_relaxed);
        g_fpvHoldAnchorPending.store(haveAnchor, std::memory_order_relaxed);
    }
    else g_fpvHoldAnchorPending.store(false, std::memory_order_relaxed);
    g_fpvHold.store(on, std::memory_order_relaxed);
}

bool IsFirstPersonHeld() { return g_fpvHold.load(std::memory_order_relaxed); }

bool IsGameplayPovInVehicle() {
    const unsigned long long v = g_gpPovVehicleTick.load(std::memory_order_relaxed);
    return v != 0 && GetTickCount64() - v < 500 && IsGameplayPovActive();
}

// Is now a moment the game could actually honour a first-person request?
//
// This replaces a three-attempt retry budget, and it replaces it for a
// measured reason. In the 2026-08-23 log both "gave up after 3 attempts" events
// fired all three attempts while `camera update STALLED (1504 flips)` and
// `(1592 flips)` were being logged alongside them -- the game was mid-cutscene
// with its camera update stopped, and could not have taken the request under
// any circumstances. The budget was not being spent on a game that refused;
// it was being spent on moments that were never candidates.
//
// So: no budget, and no attempts outside these three conditions. A request is
// plausible when the camera update is running (so the game is simulating), the
// modal byte says plain gameplay (no pause menu, no inventory), and no
// cutscene is detected. Outside that we simply wait, indefinitely and
// silently, because the mode is a stored fact and waiting costs nothing.
bool FirstPersonRequestIsPlausible() {
    if (!CutsceneStateIsFresh()) {
        // No fresh measurement means no informed answer. Say no: the cost of
        // waiting is a delay, the cost of guessing yes is another burst of
        // synthetic keypresses into a game that cannot take them.
        return false;
    }
    if (g_csvHookStalled.load(std::memory_order_relaxed)) return false;
    if (g_csvModalState.load(std::memory_order_relaxed) != 0) return false;
    if (g_csvDetected.load(std::memory_order_relaxed)) return false;
    return true;
}

// True only while the diorama view is actually driving the camera -- so it is
// false when third_person_vr=0, false in first person, and false for the few
// frames after a room cut while the cut guard is holding. Same unsynchronised
// single-bool read as IsFpvActive above, for the same reason.
//
// vr_injection.cpp uses this to decide that a frame is "gameplay" rather than
// "2D UI": without it, the diorama view would be treated as a menu and pushed
// back on a small virtual screen, which is precisely the opposite of what the
// mode is for.
bool IsThirdPersonVrActive() {
    // Gameplay Snake's eyes counts: it is a head-driven 3D view, not 2D UI,
    // so the image must not be shrunk onto the "menu" framing.
    return g_enabled.load(std::memory_order_relaxed) && (g_tpvActive || IsGameplayPovActive());
}

bool IsGameplayPovActive() {
    const unsigned long long t = g_gpPovTick.load(std::memory_order_relaxed);
    return t != 0 && GetTickCount64() - t < 250;
}

uint32_t GetRenderHideObjs(int slot) {
    if (slot < 0 || slot > 1) return 0;
    const unsigned long long t = g_renderHideTick[slot].load(std::memory_order_relaxed);
    if (t == 0 || GetTickCount64() - t > 150) return 0;
    if (slot == 1 && !g_povActive.load(std::memory_order_relaxed)) return 0;
    return g_renderHideObjs[slot].load(std::memory_order_relaxed);
}

bool IsCutscenePovDrawing() {
    return g_povActive.load(std::memory_order_relaxed) && g_vrViewMode.load(std::memory_order_relaxed) &&
           GetTickCount64() - g_csDemoTick.load(std::memory_order_relaxed) < 250;
}

bool IsSubtitlePanelEnabled() { return g_csSubPanel; }

// With the subtitle panel on, the demo's BOTTOM bar stays in the game's frame:
// the captions are drawn on it, and the panel lifts that strip out (and the
// strip is then painted out of the main view). The top bar always goes.
bool WantDemoBottomBarHidden() { return WantDemoLetterboxHidden() && !g_csSubPanel; }

bool WantDemoLetterboxHidden() {
    return g_csLetterbox == 0 && g_povActive.load(std::memory_order_relaxed) &&
           g_vrViewMode.load(std::memory_order_relaxed) &&
           GetTickCount64() - g_csDemoTick.load(std::memory_order_relaxed) < 250;
}

// Half the eye separation in game units, sign-corrected for stereo_swap_eyes:
// the LEFT eye sits at -value along the camera's screen-right axis, the right
// eye at +value. Same numbers the alternate-eye path uses, so render twice and
// alternate-eye stereo produce the same baseline.
// Game units per real metre (position_scale). Render twice uses it to turn the
// headset's own IPD into a camera baseline.
float GetPositionScaleUnitsPerMetre() {
    return g_positionScale;
}

float GetStereoHalfIpdUnits() {
    float half = ((float)g_stereoIpdMm / 2000.0f) * g_positionScale;
    if (g_stereoSwapEyes) half = -half;
    return half;
}

// Which eye the frame currently being rendered belongs to. -1 when
// alternate-eye stereo is off or inactive, in which case the frame is mono and
// goes to both eyes as before.
int GetCurrentStereoEye() {
    const int eye = g_publishedEye.load(std::memory_order_relaxed);
    if (eye != 0 && eye != 1) return eye;   // mono / hold need no freshness test

    // EXPIRY. A left/right tag is only meaningful while the game's camera
    // update is actually running. Cutscenes, codec calls and the pause menu
    // all stop it, and a stale tag then routes every captured frame of the
    // whole scene into ONE eye while the other holds pre-cutscene gameplay.
    // Past the timeout we report mono, which sends the frame to both eyes.
    const unsigned long long stamped = g_eyePublishTick.load(std::memory_order_relaxed);
    const unsigned long long now = GetTickCount64();
    if (stamped != 0 && (now - stamped) > (unsigned long long)g_stereoEyeTimeoutMs) {
        static bool loggedExpiry = false;
        if (!loggedExpiry) {
            loggedExpiry = true;
            DebugLogger::LogFormat(
                "Stereo: eye tag EXPIRED after %llu ms (timeout %d) -- the game's camera update "
                "has stopped, so frames are being treated as mono and sent to both eyes. "
                "This is the cutscene/codec path.",
                now - stamped, g_stereoEyeTimeoutMs);
        }
        return -1;
    }
    return eye;
}

#ifdef _M_IX86
namespace {
// Writes a 5-byte jmp rel32 over an exactly-5-byte region, after verifying it
// matches what we expect. Returns false and touches nothing on mismatch.
bool InstallJmp5(uintptr_t siteRva, const uint8_t* expected, void* detour,
    uint8_t* outOriginal, uint8_t** outSite, const char* label) {
    uint8_t* site = (uint8_t*)(g_moduleBase + siteRva);
    uint8_t got[5] = {};
    if (!SafeReadBytes(got, site, 5)) {
        DebugLogger::LogFormat("Camera hook (%s): site +%X unreadable -- not installing", label, (unsigned)siteRva);
        return false;
    }
    if (memcmp(got, expected, 5) != 0) {
        DebugLogger::LogFormat(
            "Camera hook (%s): SIGNATURE MISMATCH at +%X -- got %02X %02X %02X %02X %02X, "
            "expected %02X %02X %02X %02X %02X. Not installing.",
            label, (unsigned)siteRva, got[0], got[1], got[2], got[3], got[4],
            expected[0], expected[1], expected[2], expected[3], expected[4]);
        return false;
    }
    memcpy(outOriginal, got, 5);

    DWORD oldProtect = 0;
    if (!VirtualProtect(site, 5, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        DebugLogger::LogFormat("Camera hook (%s): VirtualProtect failed", label);
        return false;
    }
    const intptr_t rel = (intptr_t)detour - (intptr_t)site - 5;
    site[0] = 0xE9;
    memcpy(site + 1, &rel, 4);
    DWORD ignored = 0;
    VirtualProtect(site, 5, oldProtect, &ignored);
    FlushInstructionCache(GetCurrentProcess(), site, 5);

    *outSite = site;
    DebugLogger::LogFormat("Camera hook (%s) INSTALLED at %p (mgsi.exe+%X)", label, (void*)site, (unsigned)siteRva);
    return true;
}
// Same, for a site whose first instruction boundary lands at 6 bytes rather
// than 5: jmp rel32 plus one nop, so no instruction is left half-overwritten.
bool InstallJmp6(uintptr_t siteRva, const uint8_t* expected, void* detour,
    uint8_t* outOriginal, uint8_t** outSite, const char* label) {
    uint8_t* site = (uint8_t*)(g_moduleBase + siteRva);
    uint8_t got[6] = {};
    if (!SafeReadBytes(got, site, 6)) {
        DebugLogger::LogFormat("Camera hook (%s): site +%X unreadable -- not installing", label, (unsigned)siteRva);
        return false;
    }
    if (memcmp(got, expected, 6) != 0) {
        DebugLogger::LogFormat(
            "Camera hook (%s): SIGNATURE MISMATCH at +%X -- got %02X %02X %02X %02X %02X %02X, "
            "expected %02X %02X %02X %02X %02X %02X. Not installing.",
            label, (unsigned)siteRva, got[0], got[1], got[2], got[3], got[4], got[5],
            expected[0], expected[1], expected[2], expected[3], expected[4], expected[5]);
        return false;
    }
    memcpy(outOriginal, got, 6);

    DWORD oldProtect = 0;
    if (!VirtualProtect(site, 6, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        DebugLogger::LogFormat("Camera hook (%s): VirtualProtect failed", label);
        return false;
    }
    const intptr_t rel = (intptr_t)detour - (intptr_t)site - 5;
    site[0] = 0xE9;
    memcpy(site + 1, &rel, 4);
    site[5] = 0x90;
    DWORD ignored = 0;
    VirtualProtect(site, 6, oldProtect, &ignored);
    FlushInstructionCache(GetCurrentProcess(), site, 6);

    *outSite = site;
    DebugLogger::LogFormat("Camera hook (%s) INSTALLED at %p (mgsi.exe+%X)", label, (void*)site, (unsigned)siteRva);
    return true;
}
} // namespace
#endif

bool InstallCameraWriteHook() {
#ifndef _M_IX86
    DebugLogger::Log("Camera hook: NOT installed -- x86-only, and this is not a 32-bit build. "
        "mgsi.exe is 32-bit; the project must be built for Win32.");
    return false;
#else
    if (g_rotInstalled) return true;

    LoadCameraHookConfig();

    g_moduleBase = (uintptr_t)GetModuleHandleA(nullptr);
    if (!g_moduleBase) {
        DebugLogger::Log("Camera hook: GetModuleHandle(nullptr) failed");
        return false;
    }
    if (g_moduleBase != 0x00400000) {
        DebugLogger::LogFormat("Camera hook: NOTE -- module base is %p, not the expected 0x00400000", (void*)g_moduleBase);
    }

    // Sanity check the rotate2 store as well; if that doesn't match either,
    // this is a different binary and we do nothing at all.
    uint8_t rot2[5] = {};
    if (!SafeReadBytes(rot2, (uint8_t*)(g_moduleBase + kRot2StoreRva), 5) ||
        memcmp(rot2, kRot2StoreExpected, 5) != 0) {
        DebugLogger::Log("Camera hook: rotate2 store signature mismatch -- wrong game build. Nothing installed.");
        return false;
    }

    g_rotResumeAddr = g_moduleBase + kRotResumeRva;
    g_posResumeAddr = g_moduleBase + kPosResumeRva;
    g_vcResumeAddr = g_moduleBase + kViewCommitResumeRva;
    g_vbResumeAddr = g_moduleBase + kViewBuildResumeRva;
    g_dcResumeAddr = g_moduleBase + kDemoCamResumeRva;
    g_xformResumeAddr = g_moduleBase + kXformResumeRva;

    g_rotInstalled = InstallJmp5(kRotHookRva, kRotHookExpected,
        (void*)&Mgs1CameraRotWriteDetour, g_rotOriginal, &g_rotHookSite, "rotation");

    // Position is optional -- rotation still works without it.
    g_posInstalled = InstallJmp5(kPosHookRva, kPosHookExpected,
        (void*)&Mgs1CameraPosWriteDetour, g_posOriginal, &g_posHookSite, "position");

    // View commit is independent of both. It is installed even when the other
    // two fail, because it is the only one that works during a cutscene and it
    // shares no state with them beyond the head pose.
    g_vcInstalled = InstallJmp5(kViewCommitHookRva, kViewCommitExpected,
        (void*)&Mgs1ViewCommitDetour, g_vcOriginal, &g_vcHookSite, "view commit");
    if (g_vcInstalled) {
        DebugLogger::LogFormat(
            "View commit hook live at mgsi.exe+%X. This is layer 3 -- FROM 0x%X / TO 0x%X, read "
            "directly by the view-matrix builder at +1C22. It runs during cutscenes, which is the "
            "whole point: rotate2 does not.",
            (unsigned)kViewCommitHookRva, (unsigned)kViewFromRva, (unsigned)kViewToRva);
        if (g_vcMode != 0) {
            DebugLogger::Log("Cutscene VR: PATH A and PATH B stand down -- the view commit hook owns "
                "the cutscene camera now. One writer per value per frame.");
        }
        if (g_vcMode == 2 && g_rotInstalled) {
            // Say this plainly rather than let it be discovered as "head
            // tracking feels twice as fast". At mode 2 this hook rotates layer
            // 3 on every frame, while the rotation hook keeps rotating layer 1
            // by the same delta -- and in first person the copy at 0x53D4B
            // carries layer 1 forward into layer 3, so the two compose.
            DebugLogger::Log(
                "View commit: WARNING -- view_commit_mode=2 with the rotation hook installed. "
                "In first person BOTH will apply the head delta (layer 1 flows into layer 3 via "
                "+53D4B), so head movement will read roughly twice as fast. Set write_yaw=0 and "
                "write_pitch=0, or go back to view_commit_mode=1.");
        }
    }
    else {
        DebugLogger::Log("View commit hook NOT installed -- cutscene head tracking will not work. "
            "The older rotate2 paths stay in charge.");
    }

    // The chokepoint. Installed last and independently of everything above --
    // it is the only hook that runs when the game switches its camera task off.
    g_vbInstalled = InstallJmp6(kViewBuildHookRva, kViewBuildExpected,
        (void*)&Mgs1ViewBuildDetour, g_vbOriginal, &g_vbHookSite, "view build");
    if (g_vbInstalled) {
        DebugLogger::Log(
            "View build hook live at mgsi.exe+1C22 -- the view-matrix builder every camera path in "
            "the game calls. Watch for 'View build: NEW camera path' lines: each one names a "
            "different camera by the RVA that called it, and the one that appears only during a "
            "cutscene is the path the +53C47 hook never sees.");
    }
    else {
        DebugLogger::Log("View build hook NOT installed -- cutscene head tracking cannot work, "
            "because the layer-3 site is skipped entirely while a cutscene is playing.");
    }

    g_dcInstalled = InstallJmp6(kDemoCamHookRva, kDemoCamExpected,
        (void*)&Mgs1DemoCamDetour, g_dcOriginal, &g_dcHookSite, "cutscene camera");
    if (g_dcInstalled) {
        DebugLogger::Log("Cutscene camera hook live at mgsi.exe+1CD292. Watch for 'Cutscene camera: "
            "+1CD292 ran' lines -- if they appear during Sniper Wolf's death scene, this is the "
            "camera, and it is ours.");
    }

    if (g_xformScan) {
        g_xformInstalled = InstallJmp6(kXformHookRva, kXformExpected,
            (void*)&Mgs1XformDetour, g_xformOriginal, &g_xformHookSite, "GTE transform");
        if (g_xformInstalled) {
            DebugLogger::Log("Transform hook live at mgsi.exe+7ADA -- the engine's SetRotMatrix + "
                "SetTransMatrix, 424 call sites, the universal per-object transform. During a "
                "cutscene every translation it is handed is recorded and grouped by caller RVA, "
                "and dumped as Xform[] lines. Nothing on screen can avoid this function, so this "
                "is the one measurement that cannot come back empty because it looked in the "
                "wrong place. Set transform_scan=0 if it costs frames.");
        }
    }

    if (g_rotInstalled || g_vcInstalled || g_vbInstalled || g_dcInstalled) {
        DebugLogger::LogFormat(
            "Head tracking active. Rotation hook=%d, position hook=%d, view commit hook=%d, "
            
            "view build hook=%d, position_tracking=%d.",
            g_rotInstalled ? 1 : 0, g_posInstalled ? 1 : 0, g_vcInstalled ? 1 : 0,
            g_vbInstalled ? 1 : 0, g_positionEnabled ? 1 : 0);
    }
    return g_rotInstalled || g_vcInstalled || g_vbInstalled;
#endif
}

void RemoveCameraWriteHook() {
    auto restoreN = [](uint8_t* site, const uint8_t* orig, bool& flag, size_t n) {
        if (!flag || !site) return;
        DWORD oldProtect = 0;
        if (VirtualProtect(site, n, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            memcpy(site, orig, n);
            DWORD ignored = 0;
            VirtualProtect(site, n, oldProtect, &ignored);
            FlushInstructionCache(GetCurrentProcess(), site, n);
        }
        flag = false;
    };
    auto restore = [&](uint8_t* site, const uint8_t* orig, bool& flag) {
        restoreN(site, orig, flag, 5);
    };
    restoreN(g_dcHookSite, g_dcOriginal, g_dcInstalled, 6);
    restoreN(g_xformHookSite, g_xformOriginal, g_xformInstalled, 6);
    restoreN(g_vbHookSite, g_vbOriginal, g_vbInstalled, 6);
    restore(g_vcHookSite, g_vcOriginal, g_vcInstalled);
    restore(g_posHookSite, g_posOriginal, g_posInstalled);
    restore(g_rotHookSite, g_rotOriginal, g_rotInstalled);
    DebugLogger::Log("Camera hooks removed (original bytes restored)");
}

// ---- body follows head: input side ------------------------------------------
void PublishMoveStickActive(bool active) { g_moveStickActive.store(active, std::memory_order_relaxed); }
bool IsBodyFollowsHeadEnabled() { return g_bodyFollowsHead != 0; }


// ===========================================================================
// WEAPONS AND CONTROLS PASS (2026-09-29): scope state, Snake's position, and a
// by-name liveness test for game objects.
// ===========================================================================
void SetScopeViewActive(bool active) {
    const bool was = g_scopeActive.exchange(active && g_scopeEnabled, std::memory_order_relaxed);
    if (was != (active && g_scopeEnabled)) {
        DebugLogger::LogFormat("Scope: %s", active ? "UP -- rifle at your eye, zoomed, aimed by your head"
                                                   : "down -- rifle back in your hand");
    }
}
bool IsScopeViewActive() { return g_scopeActive.load(std::memory_order_relaxed); }
bool IsScopeFeatureEnabled() { return g_scopeEnabled; }
int GetScopeClipDistance() { return g_scopeClip; }
float GetScopeDisplayDeg() { return g_scopeDisplayDeg; }
float GetScopeRaiseDistanceM() { return g_scopeRaiseM; }
float GetScopeLowerDistanceM() { return g_scopeLowerM; }

void LoadScopeConfig() {
    char exe[MAX_PATH] = { 0 };
    GetModuleFileNameA(NULL, exe, MAX_PATH);
    std::string ini(exe);
    const auto sl = ini.find_last_of("\\/");
    if (sl != std::string::npos) ini = ini.substr(0, sl);
    ini += "\\mgs1_vr_config.ini";
    auto I = [&](const char* k, int d) { return GetPrivateProfileIntA("scope", k, d, ini.c_str()); };
    g_scopeEnabled = I("psg1_raise_to_eye", 1) != 0;
    g_scopeClip = I("zoom_clip_distance", 1100);
    if (g_scopeClip < 200) g_scopeClip = 200;
    if (g_scopeClip > 4000) g_scopeClip = 4000;
    g_scopeDisplayDeg = (float)I("display_deg", 34);
    if (g_scopeDisplayDeg < 8.0f) g_scopeDisplayDeg = 8.0f;
    if (g_scopeDisplayDeg > 100.0f) g_scopeDisplayDeg = 100.0f;
    g_scopeRaiseM = (float)I("raise_distance_cm", 35) / 100.0f;
    g_scopeLowerM = (float)I("lower_distance_cm", 45) / 100.0f;
    if (g_scopeLowerM < g_scopeRaiseM + 0.02f) g_scopeLowerM = g_scopeRaiseM + 0.02f;
    const double drawn = 2.0 * std::atan(160.0 / (double)g_scopeClip) * 180.0 / 3.14159265358979323846;
    DebugLogger::LogFormat("Scope (PSG1 raise to eye): %s | zoom clip %d = %.1f deg drawn, shown across %.0f deg "
        "(= %.1fx magnification) | up within %.0f cm of your eye, down past %.0f cm",
        g_scopeEnabled ? "ON" : "OFF", g_scopeClip, drawn, g_scopeDisplayDeg,
        std::tan(g_scopeDisplayDeg * 0.5 * 3.14159265358979323846 / 180.0) /
            std::tan(drawn * 0.5 * 3.14159265358979323846 / 180.0),
        g_scopeRaiseM * 100.0f, g_scopeLowerM * 100.0f);
}

bool GetPlayerActorPosition(int16_t out[3]) {
    const uint32_t actor = g_playerActor.load(std::memory_order_relaxed);
    if (!actor) return false;
    return SafeRead16((uintptr_t)actor + kPlayerPosOffset + 0, &out[0]) &&
           SafeRead16((uintptr_t)actor + kPlayerPosOffset + 2, &out[1]) &&
           SafeRead16((uintptr_t)actor + kPlayerPosOffset + 4, &out[2]);
}

bool IsGameTaskAlive(const char* nameSubstring, int cls) {
    if (!nameSubstring || !*nameSubstring) return false;
    const uintptr_t base = (uintptr_t)GetModuleHandleA(nullptr);
    const uintptr_t tableLo = base + kEntityHeadTableRva;
    const uintptr_t tableHi = tableLo + 16u * kEntityHeadStride;
    const int c0 = cls < 0 ? 0 : cls, c1 = cls < 0 ? 15 : cls;
    for (int c = c0; c <= c1 && c < 16; ++c) {
        const uintptr_t head = tableLo + (unsigned)c * kEntityHeadStride;
        int32_t first = 0;
        if (!SafeRead32(head, &first)) continue;
        uint32_t node = (uint32_t)first;
        for (int n = 0; n < 96 && node != 0 && node != (uint32_t)head; ++n) {
            if (node < 0x00400000u || node >= 0x7FFF0000u || (node & 3u) != 0u) break;
            if ((uintptr_t)node >= tableLo && (uintptr_t)node < tableHi) break;
            char nm[48];
            if (ReadTaskName(node, nm, sizeof(nm)) && std::strstr(nm, nameSubstring)) return true;
            int32_t next = 0;
            if (!SafeRead32((uintptr_t)node, &next)) break;
            node = (uint32_t)next;
        }
    }
    return false;
}

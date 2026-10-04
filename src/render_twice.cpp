#include <windows.h>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include "MinHook.h"
#include "../include/render_twice.h"
#include "../include/camera_write_hook.h"
#include "../include/ddraw_hook.h"
#include "../include/debug_logging.h"
#include "../include/gpu_eye_capture.h"

// ===========================================================================
// RENDER TWICE -- see render_twice.h for the frame structure this relies on.
//
// Frame N, last actor (DG_EndFrame hook):
//   1. snapshot DG_Chanls[1].ot[slot]           (257 dwords, pre-sort)
//   2. save the channel's centre eye matrices   (set this frame by the game's
//                                                camera code + our hooks)
//   3. shift them to the LEFT eye, run the game's own DG_SortChanlSystem
//   4. put the centre matrices back
// Frame N+1, first actor (DG_DrawChanlSystem hook), right after the present:
//   5. DrawOTag(slot)            -> left eye in the back buffer -> read back
//   6. restore the OT snapshot, shift to the RIGHT eye, sort again
//      (GV_Clock forced to `slot`: the prim units index packs[] by GV_Clock,
//       which has already toggled by now)
//   7. DrawOTag(slot)            -> right eye in the back buffer -> read back
//   8. publish the pair; restore the centre matrices and GV_Clock
//
// Why the OT snapshot is a complete undo: every sort unit only PUSHES packets
// onto the heads of OT buckets (addPrim: p->tag = *ot; *ot = p). Restoring the
// 257 bucket heads orphans everything the first sort added and leaves every
// primitive the actors linked in directly (HUD, effects) exactly as it was.
//
// Why moving the matrix is the whole eye offset: DG_MakeCameraMatrix (+1C22)
// builds eye.m columns = (right, up, forward) and eye_inv = eye^T with
// eye_inv.t = -eye^T * FROM. Moving FROM by s along screen-right changes
// eye_inv.t.x by exactly -s and nothing else; eye.t moves by s * right.
// ===========================================================================

namespace {

// ---- addresses (RVAs) -------------------------------------------------------
constexpr uintptr_t kDrawChanlSystemRva = 0x001619;  // void DG_DrawChanlSystem(int which)
constexpr uintptr_t kEndFrameRva        = 0x001C15;  // void DG_EndFrame(void)
constexpr uintptr_t kSortChanlSystemRva = 0x00171C;  // void DG_SortChanlSystem(int which)
constexpr uintptr_t kDrawOTagRva        = 0x0103B0;  // void DrawOTag(u_long* ot)
constexpr uintptr_t kChanl0Env1Rva      = 0x2BC1EC;  // &DG_Chanls[0].env1[0], 0x40 per slot
constexpr uintptr_t kChanl1Rva          = 0x2BC36C;  // &DG_Chanls[1]
constexpr uintptr_t kGvClockRva         = 0x391A08;  // int GV_Clock
constexpr uintptr_t kHikituriRva        = 0x2BB950;  // int DG_HikituriFlag
constexpr uintptr_t kChanlStopRva       = 0x2BECE8;  // stop_chanl_system_flag
constexpr uintptr_t kDrawSkipOnceRva    = 0x2FC718;  // PC: DrawOTag skips once when == 1
constexpr uintptr_t kDrawSuppressRva    = 0x2FC720;  // PC: DrawOTag skips while != 0
constexpr uintptr_t kBackBufferRva      = 0x2FC738;  // IDirectDrawSurface7* back buffer
constexpr uintptr_t kSoftwareModeRva    = 0x2FC794;  // PC: 1 = software renderer
constexpr uintptr_t kSceneOpenRva       = 0x2FC724;  // PC: 1 while a D3D scene is open
constexpr uintptr_t kD3DDeviceRva       = 0x2FC74C;  // IDirect3DDevice7*
constexpr uintptr_t kWindowedRva        = 0x250D14;  // PC: 1 = presents by Blt instead of Flip

// Expected first bytes at each hooked/called site. Nothing installs unless
// all of them match -- a different build is a clean no-op.
struct Sig { uintptr_t rva; uint8_t bytes[12]; int n; const char* what; };
const Sig kSigs[] = {
    { kDrawChanlSystemRva, { 0x8B,0x44,0x24,0x04, 0xC1,0xE0,0x06, 0x05,0xEC,0xC1,0x6B,0x00 }, 12, "DG_DrawChanlSystem" },
    { kEndFrameRva,        { 0xFF,0x35,0x08,0x1A,0x79,0x00, 0xE8,0xFC,0xFA,0xFF,0xFF, 0x59 }, 12, "DG_EndFrame" },
    { kSortChanlSystemRva, { 0x83,0x3D,0xE8,0xEC,0x6B,0x00,0x00, 0x56, 0x75,0x3F }, 10, "DG_SortChanlSystem" },
    { kDrawOTagRva,        { 0x55, 0x8B,0xEC, 0x6A,0xFF, 0x68,0x60,0xA3,0x64,0x00 }, 10, "DrawOTag" },
};

// ---- PSX types as laid out in mgsi.exe -------------------------------------
#pragma pack(push, 4)
struct PsxMatrix {
    int16_t m[3][3];
    int16_t pad;
    int32_t t[3];
};
struct PsxChanlHead {           // leading part of DG_CHANL, +0x00 .. +0x58
    uint32_t* ot[2];            // +0x00
    int16_t   ot_size;          // +0x08
    int16_t   link;             // +0x0A
    int16_t   dblbuf;           // +0x0C
    int16_t   dirty;            // +0x0E
    PsxMatrix eye_inv;          // +0x10
    PsxMatrix eye;              // +0x30
    int16_t   screen;           // +0x50
    int16_t   queue_size;       // +0x52
    int16_t   prim_index;       // +0x54
    int16_t   objs_index;       // +0x56
};
#pragma pack(pop)
static_assert(sizeof(PsxMatrix) == 0x20, "PSX MATRIX is 32 bytes");
static_assert(offsetof(PsxChanlHead, eye_inv) == 0x10, "eye_inv at +0x10");
static_assert(offsetof(PsxChanlHead, eye) == 0x30, "eye at +0x30");
static_assert(offsetof(PsxChanlHead, screen) == 0x50, "screen at +0x50");

using EndFrameFn  = void(__cdecl*)();
using IntArgFn    = void(__cdecl*)(int);
using DrawOTagFn  = void(__cdecl*)(void*);

uintptr_t   g_base = 0;
EndFrameFn  g_origEndFrame = nullptr;
IntArgFn    g_origDrawChanl = nullptr;
IntArgFn    g_sortChanlSystem = nullptr;   // called directly, never hooked
DrawOTagFn  g_drawOTag = nullptr;          // called directly, never hooked
bool        g_installed = false;

// ---- config ([render_twice]) -----------------------------------------------
bool g_cfgEnabled = true;       // enabled
int  g_cfgScope = 1;            // 1 = first person, 2 = any head-driven VR camera, 3 = every 3D frame in VR mode
bool g_cfgLog = true;           // log_perf
bool g_cfgIpdFromHeadset = true; // ipd_from_headset
double g_cfgMaxExtraMs = 14.0;   // max_extra_ms: budget for the second eye, per game frame
int  g_cfgCooldownMs = 3000;     // cooldown_ms: first back-off when over budget (doubles, capped)
int  g_cfgMaxCooldownMs = 30000; // max_cooldown_ms
int  g_cfgMaxSkips = 3;          // max_frame_skips: late or skipped game frames per second before backing off
double g_cfgLateFrameMs = 40.0;
double g_cfgLateBlameMs = 18.0;  // late_frames_need_extra_ms: late frames only count while our average extra cost is at least this
int    g_govIgnoredLate = 0;     // late frames not blamed on us (per perf window)  // late_frame_ms: a game frame slower than this (30 fps = 33.3 ms) counts as late
constexpr int kMaxOtEntries = 1025;
// capture = gpu | flip. gpu: copy each eye on the GPU out of the wrapper's
// render target (no extra presents), verified against the flip path on the
// first frames and dropped for the session if it does not match.
bool g_cfgCaptureGpu = true;
bool g_cfgGpuDeferred = true;    // gpu_readback = deferred (at the end of the game frame) | immediate
bool g_cfgGpuAllFrames = true;   // gpu_capture_all_frames: once verified, ordinary frames use the GPU copy too

// ---- per-frame state (game thread only) ------------------------------------
struct Pending {
    bool      valid = false;
    int       slot = 0;
    int       otCount = 0;
    uint32_t  preOt[kMaxOtEntries];
    PsxMatrix centreInv;
    PsxMatrix centreEye;
    float     half = 0.0f;
    bool      ipdFromHeadset = false;
    FrameViewRecord rec;
};
Pending g_pending;

RtEyeImage g_eyeBuf[2];

// ---- GPU eye capture state (game thread) -----------------------------------
enum GpuState { kGpuOff = 0, kGpuWaiting, kGpuVerifying, kGpuTrusted, kGpuFailed };
int  g_gpuState = kGpuOff;
int  g_gpuWaitFrames = 0, g_gpuVerifyPass = 0, g_gpuVerifyFail = 0, g_gpuRunFails = 0;
RtEyeImage g_gpuBuf[2];
struct GpuPendingPair {
    bool valid = false;
    FrameViewRecord rec;
    double drawSideMs = 0.0;
    float half = 0.0f;
    bool ipdFromHeadset = false;
};
GpuPendingPair g_gpuPend;
// An ordinary (not drawn-twice) frame copied on the GPU, read back at the end
// of the game frame. At 960p this replaces a Lock of the wrapper's primary
// that the 2026-09-26 log measured at 7-12 ms normally and up to ~1 s under
// load -- stalling the game AND the headset.
struct GpuPendingMono { bool valid = false; int eye = -1; FrameViewRecord rec; };
GpuPendingMono g_gpuMono;
RtEyeImage g_gpuMonoBuf;
int g_gpuMonoFrames = 0, g_gpuMonoFails = 0;
double g_gpuMonoMs = 0.0;
bool g_pathGpuThisWindow = false;   // for the RT PERF label
std::atomic<unsigned long long> g_liveTick{ 0 };

// perf accumulators
LARGE_INTEGER g_qpcFreq{};
double g_accSort = 0, g_accDraw = 0, g_accLock = 0, g_accConv = 0;
int    g_accPairs = 0, g_accFrames = 0, g_accDropped = 0, g_accLate = 0;
LARGE_INTEGER g_perfWindowStart{};
unsigned long long g_pairsTotal = 0;

// ---- governor (game thread only) -------------------------------------------
// COMFORT FIRST: a steady 30 fps with alternate-eye stereo is far easier to
// play than true stereo that stutters. Every pair is timed end to end; if the
// running average goes over budget, or the game starts dropping frames, render
// twice pauses for a cool-down and alternate-eye stereo takes over by itself
// (IsRenderTwiceLive() goes false 150 ms later). It retries after the
// cool-down; a quick relapse doubles the cool-down, a long clean run resets it.
double             g_govEma = 0.0;          // ms, smoothed extra cost per pair
int                g_govSamples = 0;        // pairs since (re)start
unsigned long long g_govPausedUntil = 0;    // GetTickCount64
unsigned long long g_govResumedAt = 0;
unsigned long long g_govSkipWindowStart = 0;
int                g_govSkipsInWindow = 0;
int                g_govCooldown = 0;       // current back-off, ms
int                g_govTrips = 0;
LARGE_INTEGER      g_govLastEndFrame{};     // previous DG_EndFrame, for the game's real frame interval
bool               g_govLastWasPair = false; // the previous game frame was drawn twice
bool               g_govWasWanted = false;  // rising edge -> fresh warm-up
constexpr int      kGovWarmupPairs = 8;     // ~0.25 s: never judged on extra ms

inline double MsBetween(const LARGE_INTEGER& a, const LARGE_INTEGER& b) {
    return (double)(b.QuadPart - a.QuadPart) * 1000.0 / (double)g_qpcFreq.QuadPart;
}
inline LARGE_INTEGER Now() { LARGE_INTEGER v; QueryPerformanceCounter(&v); return v; }

inline PsxChanlHead* Chanl1() { return reinterpret_cast<PsxChanlHead*>(g_base + kChanl1Rva); }
inline int32_t& GvClock() { return *reinterpret_cast<int32_t*>(g_base + kGvClockRva); }
inline int32_t Read32(uintptr_t rva) { return *reinterpret_cast<volatile int32_t*>(g_base + rva); }

// HARDWARE MODE: the game's present (+422210) runs EndScene -> Flip ->
// colour-fill the new back buffer -> frame limiter -> BeginScene, so DrawOTag
// always draws inside an open scene. Our extra presents do the same minus the
// limiter (calling the game's present would sleep a whole frame each time).
using D3DDevMethod = HRESULT(__stdcall*)(void*);
bool CallDevice(int slot) {
    void* dev = *reinterpret_cast<void**>(g_base + kD3DDeviceRva);
    if (!dev) return false;
    void** vtbl = *reinterpret_cast<void***>(dev);
    return SUCCEEDED(reinterpret_cast<D3DDevMethod>(vtbl[slot])(dev));
}
bool EndSceneIfOpen() {
    if (Read32(kSceneOpenRva) == 0) return false;
    return CallDevice(6);                                   // EndScene
}
void ReopenScene(bool wasOpen) {
    if (!wasOpen) return;
    if (!CallDevice(5)) {                                   // BeginScene
        *reinterpret_cast<volatile int32_t*>(g_base + kSceneOpenRva) = 0;
        static int logged = 0;
        if (logged < 5) { logged++; DebugLogger::Log("Render twice: BeginScene failed -- scene left closed for the game's own present"); }
    }
}

// Present what is in the back buffer and read it back off the primary.
bool PresentEye(RtEyeImage& out, double* flipMs, double* lockMs,
                double* convMs, bool clearAfter) {
    const bool wasOpen = EndSceneIfOpen();
    const bool ok = RtFlipAndCapturePrimary(out, flipMs, lockMs, convMs);
    if (clearAfter) RtClearSurfaceBlack(*reinterpret_cast<void**>(g_base + kBackBufferRva));
    ReopenScene(wasOpen);
    return ok;
}

std::string IniPath() {
    char exePath[MAX_PATH] = { 0 };
    GetModuleFileNameA(NULL, exePath, MAX_PATH);
    std::string dir(exePath);
    const size_t slash = dir.find_last_of("\\/");
    if (slash != std::string::npos) dir = dir.substr(0, slash);
    return dir + "\\mgs1_vr_config.ini";
}

void LoadConfig() {
    const std::string ini = IniPath();
    g_cfgEnabled = GetPrivateProfileIntA("render_twice", "enabled", 1, ini.c_str()) != 0;
    g_cfgScope = GetPrivateProfileIntA("render_twice", "scope", 1, ini.c_str());
    if (g_cfgScope < 1 || g_cfgScope > 3) g_cfgScope = 1;
    g_cfgLog = GetPrivateProfileIntA("render_twice", "log_perf", 1, ini.c_str()) != 0;
    g_cfgIpdFromHeadset = GetPrivateProfileIntA("render_twice", "ipd_from_headset", 1, ini.c_str()) != 0;
    g_cfgMaxExtraMs = (double)GetPrivateProfileIntA("render_twice", "max_extra_ms", 24, ini.c_str());
    if (g_cfgMaxExtraMs < 0.0) g_cfgMaxExtraMs = 0.0;          // 0 = governor off
    g_cfgCooldownMs = GetPrivateProfileIntA("render_twice", "cooldown_ms", 3000, ini.c_str());
    if (g_cfgCooldownMs < 500) g_cfgCooldownMs = 500;
    g_cfgMaxCooldownMs = GetPrivateProfileIntA("render_twice", "max_cooldown_ms", 8000, ini.c_str());
    g_cfgLateBlameMs = (double)GetPrivateProfileIntA("render_twice", "late_frames_need_extra_ms", 18, ini.c_str());
    if (g_cfgMaxCooldownMs < g_cfgCooldownMs) g_cfgMaxCooldownMs = g_cfgCooldownMs;
    g_cfgMaxSkips = GetPrivateProfileIntA("render_twice", "max_frame_skips", 3, ini.c_str());
    if (g_cfgMaxSkips < 1) g_cfgMaxSkips = 1;
    char buf[32] = {};
    GetPrivateProfileStringA("render_twice", "capture", "gpu", buf, sizeof(buf), ini.c_str());
    g_cfgCaptureGpu = _stricmp(buf, "flip") != 0;
    GetPrivateProfileStringA("render_twice", "gpu_readback", "deferred", buf, sizeof(buf), ini.c_str());
    g_cfgGpuDeferred = _stricmp(buf, "immediate") != 0;
    GpuEye::LoadConfig(ini.c_str());
    g_cfgGpuAllFrames = GetPrivateProfileIntA("render_twice", "gpu_capture_all_frames", 1, ini.c_str()) != 0;
    g_gpuState = g_cfgCaptureGpu ? kGpuWaiting : kGpuOff;
    g_cfgLateFrameMs = (double)GetPrivateProfileIntA("render_twice", "late_frame_ms", 40, ini.c_str());
    if (g_cfgLateFrameMs < 34.0) g_cfgLateFrameMs = 34.0;
    g_govCooldown = g_cfgCooldownMs;
}

// Why a frame was not rendered twice, logged on change only.
void NoteSkip(const char* why) {
    static const char* last = nullptr;
    if (why != last) {
        last = why;
        DebugLogger::LogFormat("Render twice: %s", why);
    }
}

// Cutscenes run at the game's own 24 fps demo pace (RT PERF reads exactly
// 24.0 in every scene of the 2026-10-02 log, with or without render twice).
// A 41.7 ms frame then looked "late" against the 40 ms gameplay threshold, so
// every drawn pair counted against us and the governor kept dropping the
// scene to alternate-eye stereo -- 12 images per eye per second. Judge a
// cutscene frame against the cutscene's own cadence instead, and give the
// second eye the extra slack that slower cadence really has.
bool GovCutscenePace() { return IsCutsceneVrActive(); }
double GovLateFrameMs() {
    return GovCutscenePace() ? (g_cfgLateFrameMs > 52.0 ? g_cfgLateFrameMs : 52.0) : g_cfgLateFrameMs;
}
double GovMaxExtraMs() {
    return GovCutscenePace() ? g_cfgMaxExtraMs * 1.25 : g_cfgMaxExtraMs;
}

void GovTrip(const char* reason, double value) {
    if (g_cfgMaxExtraMs <= 0.0) return;
    const unsigned long long now = GetTickCount64();
    // Relapse within 10 s of resuming -> longer back-off; otherwise start over.
    if (g_govResumedAt != 0 && now - g_govResumedAt < 10000) {
        g_govCooldown = (g_govCooldown * 2 > g_cfgMaxCooldownMs) ? g_cfgMaxCooldownMs : g_govCooldown * 2;
    } else {
        g_govCooldown = g_cfgCooldownMs;
    }
    g_govPausedUntil = now + (unsigned long long)g_govCooldown;
    g_govTrips++;
    DebugLogger::LogFormat("Render twice: over budget (%s %.1f) -- alternate-eye stereo for %.1f s (pause #%d)",
                           reason, value, g_govCooldown / 1000.0, g_govTrips);
}

void GovNotePair(double extraMs) {
    if (g_cfgMaxExtraMs <= 0.0) return;
    g_govSamples++;
    // The first pairs after entering first person or resuming carry one-off
    // costs (the FOV switch, texture uploads, driver warm-up -- the 2026-09-24
    // log had one 39 ms draw on pair #3 and nothing like it after): seed the
    // average but never trip on extra ms. Late game frames still count.
    if (g_govSamples == 1) { g_govEma = extraMs; return; }
    if (g_govSamples <= kGovWarmupPairs) { g_govEma = 0.5 * (g_govEma + extraMs); return; }
    g_govEma = 0.85 * g_govEma + 0.15 * extraMs;
    // Safety ceiling only. The real test is whether the game is still on
    // time (GovNoteFrameInterval): time we spend inside the frame limiter's
    // slack costs nothing -- the 2026-09-24 log held 30.0 fps with zero
    // skips at ~20 ms extra.
    if (g_govEma > GovMaxExtraMs()) { GovTrip("average extra ms", g_govEma); return; }
    if (extraMs > 2.5 * GovMaxExtraMs()) { GovTrip("single pair ms", extraMs); return; }
    // A long clean run forgives earlier relapses.
    if (g_govResumedAt != 0 && GetTickCount64() - g_govResumedAt > 30000) g_govCooldown = g_cfgCooldownMs;
}

// A game frame that was dropped (the game's own frame-skip) or arrived late.
void GovNoteLateFrame(const char* what) {
    if (g_cfgMaxExtraMs <= 0.0) return;
    // Only blame render twice when it is actually expensive. At ~12 ms for the
    // second eye (GPU path, RTX 3070, 960p) a late frame is the game's own
    // hiccup -- a room load, a burst of effects -- and pausing true stereo for
    // it just drops the player into alternate-eye for nothing (2026-09-27 log:
    // 10 such pauses, ~2 minutes lost). On slower hardware, where the second
    // eye really does cost a lot, this still bites.
    if (g_govEma < g_cfgLateBlameMs) {
        g_govIgnoredLate++;
        return;
    }
    const unsigned long long now = GetTickCount64();
    if (now - g_govSkipWindowStart > 1000) { g_govSkipWindowStart = now; g_govSkipsInWindow = 0; }
    if (++g_govSkipsInWindow >= g_cfgMaxSkips) {
        GovTrip(what, (double)g_govSkipsInWindow);
        g_govSkipsInWindow = 0;
    }
}
void GovNoteGameSkip() { GovNoteLateFrame("game frame-skips/late frames in 1 s"); }

// Called at every DG_EndFrame. The game paces itself to 30 fps with its own
// limiter, so a frame that follows a drawn-twice frame and still arrives on
// time means the second eye fitted in the slack and cost nothing.
void GovNoteFrameInterval() {
    const LARGE_INTEGER now = Now();
    if (g_govLastEndFrame.QuadPart != 0 && g_govLastWasPair) {
        const double ms = MsBetween(g_govLastEndFrame, now);
        const double lateMs = GovLateFrameMs();
        if (ms > lateMs) GovNoteLateFrame("late game frames in 1 s");
        g_accLate += (ms > lateMs) ? 1 : 0;
    }
    g_govLastEndFrame = now;
    g_govLastWasPair = false;
}

// True while the governor has render twice paused.
bool GovPaused() {
    if (g_govPausedUntil == 0) return false;
    const unsigned long long now = GetTickCount64();
    if (now < g_govPausedUntil) return true;
    g_govPausedUntil = 0;
    g_govResumedAt = now;
    g_govSamples = 0;
    g_govEma = 0.0;
    g_govSkipsInWindow = 0;
    DebugLogger::Log("Render twice: cool-down over -- trying both eyes again");
    return false;
}

bool WantedThisFrame(const char** why) {
    if (!g_cfgEnabled) { *why = "off (enabled=0)"; return false; }
    if (!IsVrViewModeActive()) { *why = "standing by -- native (virtual screen) mode"; return false; }
    if (IsCutsceneScreenFallbackActive()) { *why = "standing by -- cutscene shot on the virtual screen (no Snake in it)"; return false; }
    // Gameplay Snake's eyes (elevator, ladder, scripted shots) is first person
    // as far as the player is concerned: both eyes, every frame, like normal play.
    const bool fpv = IsFpvActive() || IsGameplayPovActive();
    const bool anyVrCam = fpv || IsThirdPersonVrActive() || IsCutsceneVrActive();
    if (g_cfgScope == 1 && !fpv) { *why = "standing by -- not in first person (scope=1)"; return false; }
    if (g_cfgScope == 2 && !anyVrCam) { *why = "standing by -- camera not head-driven (scope=2)"; return false; }
    if (GetStereoHalfIpdUnits() == 0.0f) { *why = "standing by -- stereo_ipd_mm is 0"; return false; }
    if (Read32(kSoftwareModeRva) != 0 || Read32(kWindowedRva) != 0) {
        *why = "standing by -- game is in software/windowed present mode (only the hardware Flip path is supported)";
        return false;
    }
    if (GovPaused()) { *why = "paused by the frame-time governor -- alternate-eye stereo meanwhile"; return false; }
    *why = "ACTIVE -- both eyes drawn every game frame";
    return true;
}

// Moves a copy of the centre matrices to one eye and writes it into the channel.
void ApplyEye(PsxChanlHead* c, const PsxMatrix& centreInv, const PsxMatrix& centreEye, float s) {
    PsxMatrix inv = centreInv;
    PsxMatrix eye = centreEye;
    const int32_t si = (int32_t)std::lround(s);
    inv.t[0] -= si;
    for (int i = 0; i < 3; ++i) {
        eye.t[i] += (int32_t)std::lround((double)eye.m[i][0] * (double)s / 4096.0);
    }
    c->eye_inv = inv;
    c->eye = eye;
}

// ---- SKY LOCK (2026-09-28: heliport sky "turns with the headset") -----------
// The outdoor sky is not geometry. Thing\sphere.c (act +23E24E) and
// Thing\sphere2.c (act +23DCCE) draw it as a grid of 2D POLY_FT4 tiles whose
// scroll comes from one helper each (+23E511 / +23DF8F), fed the channel's
// eye matrix (DG_Chanls[1].eye, 0x6BC39C):
//   yaw   = ratan2(eye.m[0][2], eye.m[2][2]) & 0xFFF        (+40B612)
//   out.x = W - W*yaw/4096 - 1          W = sky width   (sphere 0x78A988, sphere2 0x78A974)
//   out.y = -285*fy/sqrt(1-fy^2) - base  fy = eye.m[1][2] (base 0x78A98A / 0x78A976)
// So one full turn scrolls the sky by W pixels and pitch by a fixed 285 --
// numbers tuned by eye for the stock 53-degree lens. The world on screen moves
// at `clip` pixels per radian, and at the mod's wide FOV that no longer
// matches: the sky slid with your head instead of staying put.
// Fix: take over the two calls and scroll by the clip distance actually in use
// (DG_Chanls[1] +0x50). Pitch: clip*tan, exactly as the world projects.
// Yaw: clip px per radian, accumulated from frame-to-frame yaw changes so the
// scroll never jumps (v1 rounded to whole panorama repeats per turn -- at clip
// 120 and W 2048 that rounded to the stock rate and changed nothing).
// VR view mode only; native mode is stock.
constexpr uintptr_t kSkyEyeRva = 0x2BC39C;   // DG_Chanls[1].eye
constexpr uintptr_t kRatan2Rva = 0x00B612;
struct SkySite {
    const char* name; uintptr_t callRva; uintptr_t targetRva; uintptr_t widthRva; uintptr_t baseRva; bool widthSigned;
    bool installed; unsigned logged;
    bool have; int lastYaw; int lastW; int lastClip; double accumPx;
};
SkySite g_skySites[2] = {
    { "sphere.c",  0x23E260, 0x23E511, 0x38A988, 0x38A98A, true,  false, 0, false, 0, 0, 0, 0.0 },
    { "sphere2.c", 0x23DCE0, 0x23DF8F, 0x38A974, 0x38A976, false, false, 0, false, 0, 0, 0, 0.0 },
};
bool g_skyLockEnabled = true;

using SkyFn = void(__cdecl*)(const int16_t* eye, int16_t* out);
using Ratan2Fn = int(__cdecl*)(const int16_t* v);

void SkyAngles(int site, const int16_t* eye, int16_t* out) {
    SkySite& S = g_skySites[site];
    reinterpret_cast<SkyFn>(g_base + S.targetRva)(eye, out);          // stock result first
    if (!g_skyLockEnabled || !IsVrViewModeActive()) { S.have = false; return; }
    const int clip = *reinterpret_cast<const int16_t*>(g_base + kChanl1Rva + 0x50);
    int W = S.widthSigned ? (int)*reinterpret_cast<const int16_t*>(g_base + S.widthRva)
                          : (int)*reinterpret_cast<const uint16_t*>(g_base + S.widthRva);
    if (clip < 40 || clip > 4000 || W <= 0) return;
    const int base = *reinterpret_cast<const int16_t*>(g_base + S.baseRva);

    // yaw exactly as the game computes it
    int16_t v[3] = { eye[2], 0, eye[8] };                              // m[0][2], m[2][2]
    const int yaw = reinterpret_cast<Ratan2Fn>(g_base + kRatan2Rva)(v) & 0xFFF;
    // Scroll by the world's own rate, clip px per radian, ACCUMULATED from
    // yaw changes. At the wide FOV one turn is less than one panorama width,
    // so a direct yaw->scroll mapping would jump at north. Accumulating keeps
    // it continuous (the panorama tiles seamlessly over W); the only cost is
    // that after a full turn the clouds sit elsewhere, which nobody can see.
    const double pxPerUnit = (double)clip * 6.283185307179586 / 4096.0;
    if (!S.have || S.lastW != W) {
        S.have = true; S.lastW = W;
        S.accumPx = (double)W * (double)yaw / 4096.0;                   // start exactly where stock is
    }
    else {
        const int delta = ((yaw - S.lastYaw + 2048) & 0xFFF) - 2048;
        S.accumPx += (double)delta * pxPerUnit;
        S.accumPx = std::fmod(S.accumPx, (double)W);
    }
    S.lastYaw = yaw;
    double m = std::fmod(S.accumPx, (double)W);
    if (m < 0.0) m += (double)W;
    int x = W - 1 - (int)m;
    if (x < 0) x = 0;
    if (x > W - 1) x = W - 1;
    out[0] = (int16_t)x;

    // pitch: clip * tan(pitch), the world's own projection
    const int fy = eye[5];                                               // m[1][2]
    double c = 16777216.0 - (double)fy * fy;
    if (c < 1.0) c = 1.0;
    const double y = -(double)clip * (double)fy / std::sqrt(c);
    out[1] = (int16_t)(std::lround(y) - base);

    if (clip != S.lastClip && S.logged < 12) {
        ++S.logged;
        S.lastClip = clip;
        DebugLogger::LogFormat("Sky lock (%s): sky width %d px, clip %d -> scrolling %.2f px per degree of turn "
            "(stock %.2f), pitch scale %d (stock 285). The sky should now hold still in the world.",
            S.name, W, clip, pxPerUnit * 4096.0 / 360.0, (double)W / 360.0, clip);
    }
}
void __cdecl SkyAngles0(const int16_t* eye, int16_t* out) { SkyAngles(0, eye, out); }
void __cdecl SkyAngles1(const int16_t* eye, int16_t* out) { SkyAngles(1, eye, out); }

void InstallSkyLockHooks() {
    char exe[MAX_PATH] = { 0 };
    GetModuleFileNameA(NULL, exe, MAX_PATH);
    std::string ini(exe);
    const auto sl = ini.find_last_of("\\/");
    if (sl != std::string::npos) ini = ini.substr(0, sl);
    ini += "\\mgs1_vr_config.ini";
    g_skyLockEnabled = GetPrivateProfileIntA("sky_lock", "enabled", 1, ini.c_str()) != 0;
    if (!g_skyLockEnabled) { DebugLogger::Log("Sky lock: disabled in ini ([sky_lock] enabled=0) -- stock sky scroll."); return; }
    void* fns[2] = { reinterpret_cast<void*>(&SkyAngles0), reinterpret_cast<void*>(&SkyAngles1) };
    for (int i = 0; i < 2; ++i) {
        SkySite& S = g_skySites[i];
        uint8_t* call = reinterpret_cast<uint8_t*>(g_base + S.callRva);
        int32_t rel = 0;
        if (call[0] != 0xE8) { DebugLogger::LogFormat("Sky lock (%s): no call at +%X -- not installed", S.name, (unsigned)S.callRva); continue; }
        std::memcpy(&rel, call + 1, 4);
        if ((uintptr_t)(call + 5) + rel != g_base + S.targetRva) {
            DebugLogger::LogFormat("Sky lock (%s): call at +%X does not target +%X -- not installed", S.name,
                (unsigned)S.callRva, (unsigned)S.targetRva);
            continue;
        }
        DWORD old = 0;
        if (!VirtualProtect(call, 5, PAGE_EXECUTE_READWRITE, &old)) continue;
        const int32_t nrel = (int32_t)((intptr_t)fns[i] - (intptr_t)(call + 5));
        std::memcpy(call + 1, &nrel, 4);
        DWORD ignored = 0;
        VirtualProtect(call, 5, old, &ignored);
        FlushInstructionCache(GetCurrentProcess(), call, 5);
        S.installed = true;
        DebugLogger::LogFormat("Sky lock (%s): INSTALLED at +%X", S.name, (unsigned)S.callRva);
    }
}

// ---- SORT-ONLY PRIM DEPTHS (fixed 2026-09-28: heliport sky over the buildings,
// right eye only) ---------------------------------------------------------------
// The sort unit (+340A -> +34C6) places each DG_PRIM packet by reading its
// DEPTH out of the low 16 bits of the packet's own tag word, and then
// OVERWRITES that tag with the OT link. Ordinary prims get a fresh depth from
// the prim unit (+41A5) on every sort -- but that unit skips prims whose type
// has 0x100 or 0x800 set ([prim+0x24] & 0x900, +4041A5..+40423C), while the
// sort unit still sorts the 0x800 ones. Their depth is written once, by their
// own actor. So the LEFT sort consumed it, and the RIGHT sort read the left
// sort's link pointer as a depth and dropped those packets into a random
// bucket: at the heliport the sky landed in front of the distant buildings.
// Fix: save every sorted prim's packet tags before the left sort and put them
// back before the right sort, which is exactly the state the game sorted from.
//   chanl: +0x52 end index, +0x54 first prim index, +0x58 DG_PRIM** queue
//   prim : +0x24 type, +0x2A packet count, +0x30 packet stride, +0x40 packs[2]
constexpr int kMaxPrimTags = 65536;
struct PrimTag { uint32_t* at; uint32_t val; };
PrimTag g_primTags[kMaxPrimTags];
int g_primTagCount = 0;
bool g_primTagOverflow = false;
unsigned g_primTagRestores = 0;

// SEH only: no objects with destructors in here.
int SnapshotPrimTagsRaw(uintptr_t chanl, int which) {
    int n = 0;
    g_primTagOverflow = false;
    __try {
        const int end = *(int16_t*)(chanl + 0x52);
        const int start = *(int16_t*)(chanl + 0x54);
        uint32_t** queue = *(uint32_t***)(chanl + 0x58);
        if (!queue || end - start <= 0 || end - start > 4096) return 0;
        for (int i = start; i < end; ++i) {
            const uintptr_t prim = (uintptr_t)queue[i];
            if (!prim) continue;
            if (*(uint8_t*)(prim + 0x25) & 1) continue;          // the sort unit skips these too
            const int count = *(int16_t*)(prim + 0x2A);
            const int stride = *(int16_t*)(prim + 0x30);
            uintptr_t pk = *(uintptr_t*)(prim + 0x40 + (uintptr_t)which * 4);
            if (!pk || count <= 0 || stride <= 0) continue;
            for (int k = 0; k < count; ++k, pk += (uintptr_t)stride) {
                if (n >= kMaxPrimTags) { g_primTagOverflow = true; return n; }
                g_primTags[n].at = (uint32_t*)pk;
                g_primTags[n].val = *(uint32_t*)pk;
                ++n;
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
    return n;
}
bool RestorePrimTagsRaw(int n) {
    __try {
        for (int i = 0; i < n; ++i) *g_primTags[i].at = g_primTags[i].val;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return true;
}

const char* GpuStateName(int s) {
    switch (s) {
    case kGpuOff: return "off (capture=flip)";
    case kGpuWaiting: return "waiting";
    case kGpuVerifying: return "verifying against the flip path";
    case kGpuTrusted: return "ACTIVE";
    default: return "failed -- flip path for this session";
    }
}

void GpuFail(const char* why) {
    if (g_gpuState == kGpuFailed) return;
    g_gpuState = kGpuFailed;
    g_gpuPend.valid = false;
    GpuEye::ReleaseAll();
    DebugLogger::LogFormat("Render twice: GPU eye capture OFF for this session -- %s. Using the flip path (2 extra presents per frame) instead. [%s]",
        why, GpuEye::Describe());
}

// 16x12 grid of mean luminance (0..255) over a whole eye image, flip-path
// (raw 5:6:5 or RGBA) or GPU (BGRA). Resolution-independent, so a 640x480
// read-back and a 1280x960 GPU copy of the same picture compare directly.
bool GridLuma(const RtEyeImage& im, float grid[12][16], double* mean) {
    const int W = im.width, H = im.height;
    if (W < 32 || H < 24) return false;
    const bool r16 = !im.raw16.empty();
    if (r16 ? im.raw16.size() < (size_t)W * H : im.rgba.size() < (size_t)W * H * 4) return false;
    double total = 0.0;
    for (int gy = 0; gy < 12; ++gy) for (int gx = 0; gx < 16; ++gx) {
        double acc = 0.0;
        for (int sy = 0; sy < 4; ++sy) for (int sx = 0; sx < 4; ++sx) {
            const int x = (int)(((gx * 4 + sx) + 0.5) * W / 64.0);
            const int y = (int)(((gy * 4 + sy) + 0.5) * H / 48.0);
            double r, g, b;
            if (r16) {
                const uint16_t v = im.raw16[(size_t)y * W + x];
                r = ((v >> 11) & 31) * 255.0 / 31.0; g = ((v >> 5) & 63) * 255.0 / 63.0; b = (v & 31) * 255.0 / 31.0;
            }
            else {
                const uint8_t* p = &im.rgba[((size_t)y * W + x) * 4];
                if (im.bgra) { b = p[0]; g = p[1]; r = p[2]; } else { r = p[0]; g = p[1]; b = p[2]; }
            }
            acc += 0.299 * r + 0.587 * g + 0.114 * b;
        }
        grid[gy][gx] = (float)(acc / 16.0);
        total += grid[gy][gx];
    }
    *mean = total / 192.0;
    return true;
}

// Does the GPU copy show the same picture the flip path read back?
bool GpuMatches(const RtEyeImage& gpu, const RtEyeImage& ref, double* mad, double* gpuMean, double* refMean) {
    float a[12][16], b[12][16];
    if (!GridLuma(gpu, a, gpuMean) || !GridLuma(ref, b, refMean)) { *mad = 999.0; return false; }
    double sum = 0.0;
    for (int y = 0; y < 12; ++y) for (int x = 0; x < 16; ++x) sum += std::fabs((double)a[y][x] - (double)b[y][x]);
    *mad = sum / 192.0;
    if (*refMean > 10.0 && *gpuMean < 3.0) return false;   // black copy of a lit scene
    return *mad <= 14.0;
}

// Reads both GPU eyes back and publishes them (GPU path, trusted).
void GpuFinishPair() {
    if (!g_gpuPend.valid) return;
    g_gpuPend.valid = false;
    double rb0 = 0, rb1 = 0;
    const bool ok = GpuEye::Readback(0, g_gpuBuf[0], &rb0) && GpuEye::Readback(1, g_gpuBuf[1], &rb1);
    if (!ok) {
        if (++g_gpuRunFails >= 3) GpuFail("read-back failed 3 times in a row");
        return;
    }
    g_gpuRunFails = 0;
    const int32_t w = g_gpuBuf[0].width, h = g_gpuBuf[0].height;
    PublishRenderTwicePair(g_gpuBuf[0], g_gpuBuf[1], g_gpuPend.rec, false);
    g_liveTick.store(GetTickCount64(), std::memory_order_relaxed);
    g_pairsTotal++;
    g_accPairs++;
    g_accLock += rb0 + rb1;
    GovNotePair(g_gpuPend.drawSideMs + rb0 + rb1);
    static int logged = 0;
    if (logged < 3) {
        logged++;
        DebugLogger::LogFormat("Render twice: GPU pair published %dx%d | our side %.2f ms + read-back %.2f+%.2f ms | half-IPD %.1f units | view record %s | IPD %s",
            w, h, g_gpuPend.drawSideMs, rb0, rb1, g_gpuPend.half, g_gpuPend.rec.valid ? "valid" : "none",
            g_gpuPend.ipdFromHeadset ? "from headset" : "from ini");
    }
}

void GpuFinishMono() {
    if (!g_gpuMono.valid) return;
    g_gpuMono.valid = false;
    double rb = 0.0;
    if (!GpuEye::Readback(0, g_gpuMonoBuf, &rb)) {
        if (++g_gpuMonoFails >= 5) {
            g_cfgGpuAllFrames = false;
            DebugLogger::Log("Render twice: GPU capture of ordinary frames failed 5 times in a row -- back to reading the primary for those");
        }
        return;
    }
    g_gpuMonoFails = 0;
    PublishGpuMonoFrame(g_gpuMonoBuf, g_gpuMono.eye, g_gpuMono.rec);
    g_gpuMonoFrames++;
    g_gpuMonoMs += rb;
    static bool logged = false;
    if (!logged) {
        logged = true;
        DebugLogger::LogFormat("Render twice: ordinary frames now captured on the GPU too (%dx%d, read-back %.2f ms) -- no more locking the primary",
            g_gpuMonoBuf.width, g_gpuMonoBuf.height, rb);
    }
}

// After the game's own DrawOTag of an ordinary frame: copy it on the GPU.
void MaybeGpuMonoCapture() {
    if (!g_cfgGpuAllFrames || g_gpuState != kGpuTrusted) return;
    if (GpuEye::Ready(nullptr) != 1) return;
    const bool wasOpen = EndSceneIfOpen();
    const bool ok = GpuEye::CopyEye(0);
    ReopenScene(wasOpen);
    if (!ok) return;
    g_gpuMono.valid = true;
    GetDrawnFrameTag(&g_gpuMono.eye, &g_gpuMono.rec);
}

void MaybeLogPerf() {
    if (!g_cfgLog) return;
    const LARGE_INTEGER now = Now();
    if (g_perfWindowStart.QuadPart == 0) { g_perfWindowStart = now; return; }
    const double elapsed = MsBetween(g_perfWindowStart, now);
    if (elapsed < 5000.0) return;
    const double fps = g_accFrames * 1000.0 / elapsed;
    const int n = g_accPairs > 0 ? g_accPairs : 1;
    const double extra = (g_accSort + g_accDraw + g_accLock + g_accConv) / n;
    DebugLogger::LogFormat(
        "RT PERF: game %.1f fps | %d/%d frames rendered twice | path %s | per pair: 2nd sort %.2f ms, 2nd draw %.2f ms, "
        "%s %.2f ms + copy %.2f ms (both eyes) | extra %.2f ms of the %.1f ms frame | "
        "game frame-skips %d",
        fps, g_accPairs, g_accFrames, g_pathGpuThisWindow ? "GPU" : "flip",
        g_accSort / n, g_accDraw / n,
        g_pathGpuThisWindow ? "GPU copies + read-back" : "2 extra flips + read-back locks",
        g_accLock / n, g_accConv / n,
        extra, fps > 0 ? 1000.0 / fps : 0.0, g_accDropped);
    if (g_gpuState != kGpuOff)
        DebugLogger::LogFormat("RT GPU CAPTURE: %s | %s | ordinary frames via GPU: %d (read-back avg %.2f ms)",
            GpuStateName(g_gpuState), GpuEye::Describe(), g_gpuMonoFrames, g_gpuMonoFrames ? g_gpuMonoMs / g_gpuMonoFrames : 0.0);
    g_gpuMonoFrames = 0; g_gpuMonoMs = 0.0;
    g_pathGpuThisWindow = false;
    if (g_cfgMaxExtraMs > 0.0)
        DebugLogger::LogFormat("RT GOVERNOR: avg extra %.1f ms (ceiling %.0f) | late frames (>%.0f ms) after a pair: %d (%d not blamed on us: our cost under %.0f ms) | %s | pauses so far %d",
            g_govEma, g_cfgMaxExtraMs, g_cfgLateFrameMs, g_accLate, g_govIgnoredLate, g_cfgLateBlameMs, g_govPausedUntil ? "PAUSED" : "running", g_govTrips);
    g_govIgnoredLate = 0;
    g_accSort = g_accDraw = g_accLock = g_accConv = 0;
    g_accPairs = g_accFrames = g_accDropped = g_accLate = 0;
    g_perfWindowStart = now;
}

// ---- hooks -----------------------------------------------------------------

void __cdecl HookedEndFrame() {
    // GPU path: the pair drawn at the start of this frame has had the whole
    // game-logic phase to finish on the GPU; reading it back now costs little.
    if (g_gpuPend.valid) GpuFinishPair();
    if (g_gpuMono.valid) GpuFinishMono();
    g_pending.valid = false;
    g_accFrames++;
    GovNoteFrameInterval();

    const char* why = nullptr;
    const bool wanted = WantedThisFrame(&why);
    if (wanted && !g_govWasWanted) { g_govSamples = 0; g_govEma = 0.0; }   // fresh warm-up on (re)entry
    g_govWasWanted = wanted;
    const int slot = GvClock();
    PsxChanlHead* c = Chanl1();
    const int otCount = (c->ot_size >= 0 && c->ot_size <= 10) ? (1 << c->ot_size) + 1 : 0;

    if (!wanted || Read32(kHikituriRva) != 0 || Read32(kChanlStopRva) != 0 ||
        (slot != 0 && slot != 1) || otCount <= 0 || otCount > kMaxOtEntries || !c->ot[slot]) {
        if (!wanted) NoteSkip(why);
        g_origEndFrame();
        MaybeLogPerf();
        return;
    }
    NoteSkip(why);

    g_pending.slot = slot;
    g_pending.otCount = otCount;
    std::memcpy(g_pending.preOt, c->ot[slot], (size_t)otCount * sizeof(uint32_t));
    g_primTagCount = SnapshotPrimTagsRaw((uintptr_t)c, slot);
    {
        static bool logged = false, loggedBad = false;
        if (g_primTagCount < 0 && !loggedBad) {
            loggedBad = true;
            DebugLogger::Log("Render twice: prim depth snapshot faulted -- right eye may mis-sort sort-only prims (sky).");
        }
        else if (g_primTagOverflow && !loggedBad) {
            loggedBad = true;
            DebugLogger::LogFormat("Render twice: prim depth snapshot hit its %d-packet cap -- the rest keep the old behaviour.", kMaxPrimTags);
        }
        else if (!logged && g_primTagCount > 0) {
            logged = true;
            DebugLogger::LogFormat("Render twice: saving %d prim packet depths per frame so the right eye sorts the sky "
                "and other sort-only prims from the same depths as the left.", g_primTagCount);
        }
    }
    g_pending.centreInv = c->eye_inv;
    g_pending.centreEye = c->eye;
    // The rotation hook ran earlier in this same frame, so this is the head
    // pose THESE pixels are drawn from -- exact, not one frame stale.
    g_pending.rec = ConsumeFrameViewRecord();
    g_pending.half = GetStereoHalfIpdUnits();
    g_pending.ipdFromHeadset = false;
    // COMFORT: use the headset's own eye separation, from the same eye poses
    // the compositor will present these images with. A baseline that does
    // not match the one the runtime reprojects with reads as the world being
    // the wrong size (and shears when you turn). Sanity band 45..85 mm; the
    // ini value is the fallback.
    if (g_cfgIpdFromHeadset && g_pending.rec.valid) {
        const ViewPoseF& l = g_pending.rec.eye[0];
        const ViewPoseF& r = g_pending.rec.eye[1];
        const float dx = r.px - l.px, dy = r.py - l.py, dz = r.pz - l.pz;
        const float ipdM = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (ipdM > 0.045f && ipdM < 0.085f) {
            const float half = 0.5f * ipdM * GetPositionScaleUnitsPerMetre();
            g_pending.half = (g_pending.half < 0.0f) ? -half : half;   // keeps stereo_swap_eyes
            g_pending.ipdFromHeadset = true;
            static float s_loggedMm = -1.0f;
            if (std::fabs(ipdM * 1000.0f - s_loggedMm) > 1.0f) {
                s_loggedMm = ipdM * 1000.0f;
                DebugLogger::LogFormat("Render twice: IPD from headset %.1f mm -> half-baseline %.1f game units",
                                       s_loggedMm, half);
            }
        }
    }

    // PSG1 scope up: one picture for both eyes. A scope is monocular, and the
    // scoped frame is shown magnified, so a full IPD would read as doubled.
    if (IsScopeViewActive()) g_pending.half = 0.0f;

    ApplyEye(c, g_pending.centreInv, g_pending.centreEye, -g_pending.half);   // LEFT
    g_origEndFrame();                                                          // the game's own sort
    c->eye_inv = g_pending.centreInv;
    c->eye = g_pending.centreEye;
    g_pending.valid = true;
    MaybeLogPerf();
}

void __cdecl HookedDrawChanl(int slot) {
    if (g_pending.valid && (Read32(kDrawSkipOnceRva) == 1 || Read32(kDrawSuppressRva) != 0)) {
        g_accDropped++;   // the game's own frame-skip: this frame is not drawn at all
        GovNoteGameSkip();
    }
    if (!g_pending.valid || slot != g_pending.slot ||
        Read32(kDrawSkipOnceRva) == 1 || Read32(kDrawSuppressRva) != 0) {
        const bool drawn = Read32(kDrawSkipOnceRva) != 1 && Read32(kDrawSuppressRva) == 0;
        g_pending.valid = false;
        g_origDrawChanl(slot);
        if (drawn) MaybeGpuMonoCapture();
        return;
    }
    g_pending.valid = false;

    void* env = reinterpret_cast<void*>(g_base + kChanl0Env1Rva + (uintptr_t)slot * 0x40);

    // Which capture path this frame takes.
    bool gpuReady = false;
    if (g_gpuState == kGpuWaiting || g_gpuState == kGpuVerifying || g_gpuState == kGpuTrusted) {
        const char* gwhy = nullptr;
        const int rdy = GpuEye::Ready(&gwhy);
        gpuReady = rdy == 1;
        static const char* lastWhy = nullptr;
        if (gwhy != lastWhy) { lastWhy = gwhy; DebugLogger::LogFormat("Render twice: GPU eye capture -- %s", gwhy); }
        if (rdy < 0) GpuFail(gwhy);
        else if (rdy == 0 && g_gpuState != kGpuTrusted && ++g_gpuWaitFrames > 300) GpuFail(gwhy);
        else if (g_gpuState == kGpuWaiting) {
            g_gpuState = kGpuVerifying;
            DebugLogger::Log("Render twice: GPU eye capture -- checking the first frames against the flip path");
        }
    }
    const bool gpuRun = gpuReady && g_gpuState == kGpuTrusted;
    const bool gpuVerify = gpuReady && g_gpuState == kGpuVerifying;
    void* backBuffer = *reinterpret_cast<void**>(g_base + kBackBufferRva);

    PsxChanlHead* c = Chanl1();
    auto sortRight = [&]() {
        // RIGHT: undo the sort, move the eye, sort again.
        const PsxMatrix keepInv = c->eye_inv;
        const PsxMatrix keepEye = c->eye;
        const int32_t keepClock = GvClock();
        std::memcpy(c->ot[slot], g_pending.preOt, (size_t)g_pending.otCount * sizeof(uint32_t));
        // Put back the prim depths the left sort overwrote with OT links.
        if (g_primTagCount > 0 && RestorePrimTagsRaw(g_primTagCount)) ++g_primTagRestores;
        ApplyEye(c, g_pending.centreInv, g_pending.centreEye, +g_pending.half);
        GvClock() = slot;
        g_sortChanlSystem(slot);
        GvClock() = keepClock;
        c->eye_inv = keepInv;
        c->eye = keepEye;
    };

    // ======================= GPU PATH (verified) =============================
    // Each eye is copied on the GPU straight out of the wrapper's render target
    // after its draw; nothing is presented. The game's own Flip next frame
    // shows the right eye on the monitor, and the copies are read back at the
    // end of this game frame (gpu_readback=deferred) or right now.
    if (gpuRun) {
        g_origDrawChanl(slot);                                // LEFT
        const LARGE_INTEGER tStart = Now();
        bool wasOpen = EndSceneIfOpen();
        const bool c0 = GpuEye::CopyEye(0);
        RtClearSurfaceBlack(backBuffer);
        ReopenScene(wasOpen);
        const LARGE_INTEGER t0 = Now();
        sortRight();
        const LARGE_INTEGER t1 = Now();
        g_drawOTag(env);                                      // RIGHT
        const LARGE_INTEGER t2 = Now();
        wasOpen = EndSceneIfOpen();
        const bool c1 = GpuEye::CopyEye(1);
        ReopenScene(wasOpen);
        if (!c0 || !c1) {
            if (++g_gpuRunFails >= 3) GpuFail("GPU copy failed 3 times in a row");
            return;
        }
        g_pathGpuThisWindow = true;
        g_accSort += MsBetween(t0, t1);
        g_accDraw += MsBetween(t1, t2);
        g_gpuPend.valid = true;
        g_gpuPend.rec = g_pending.rec;
        g_gpuPend.half = g_pending.half;
        g_gpuPend.ipdFromHeadset = g_pending.ipdFromHeadset;
        g_gpuPend.drawSideMs = MsBetween(tStart, Now());
        g_govLastWasPair = true;
        if (!g_cfgGpuDeferred) GpuFinishPair();
        return;
    }

    // ======================= FLIP PATH ======================================
    // LEFT: the game's own draw of the OT sorted at DG_EndFrame, presented and
    // read back right away. The back buffer is then cleared, as the game's own
    // present would, ready for the right eye. While the GPU path is being
    // verified, each eye is ALSO copied on the GPU just before its present so
    // the two can be compared.
    const long drawsBefore = gpuVerify ? GpuEye::DrawCallCount() : 0;
    g_origDrawChanl(slot);
    const LARGE_INTEGER tStart = Now();   // everything after the game's own draw is ours
    long drawsLeft = 0;
    bool vc0 = false, vc1 = false;
    if (gpuVerify) {
        const bool wasOpen = EndSceneIfOpen();
        drawsLeft = GpuEye::DrawCallCount() - drawsBefore;
        vc0 = GpuEye::CopyEye(0);
        ReopenScene(wasOpen);
    }
    int32_t w0 = 0, h0 = 0, w1 = 0, h1 = 0;
    double flip0 = 0, lock0 = 0, conv0 = 0, flip1 = 0, lock1 = 0, conv1 = 0;
    const bool ok0 = PresentEye(g_eyeBuf[0], &flip0, &lock0, &conv0, true);
    w0 = g_eyeBuf[0].width; h0 = g_eyeBuf[0].height;
    if (!ok0) {
        static int failLog = 0;
        if (failLog < 10) { failLog++; DebugLogger::Log("Render twice: left-eye present/read-back failed -- frame stays mono"); }
        return;
    }

    const LARGE_INTEGER t0 = Now();
    sortRight();
    const LARGE_INTEGER t1 = Now();
    g_drawOTag(env);
    const LARGE_INTEGER t2 = Now();
    if (gpuVerify) {
        const bool wasOpen = EndSceneIfOpen();
        vc1 = GpuEye::CopyEye(1);
        ReopenScene(wasOpen);
    }

    // No clear afterwards: the game's own present at the start of next frame
    // flips once more and colour-fills the back buffer itself.
    const bool ok1 = PresentEye(g_eyeBuf[1], &flip1, &lock1, &conv1, false);
    w1 = g_eyeBuf[1].width; h1 = g_eyeBuf[1].height;
    if (!ok1 || w0 != w1 || h0 != h1) {
        static int failLog = 0;
        if (failLog < 10) { failLog++; DebugLogger::LogFormat("Render twice: right-eye present/read-back failed (%dx%d vs %dx%d) -- frame stays mono", w0, h0, w1, h1); }
        return;
    }

    // GPU verification: read the GPU copies back now and compare them with
    // what the flip path just read off the primary. Three matching frames ->
    // the GPU path takes over; three mismatches -> flip path for the session.
    if (gpuVerify) {
        double rb0 = 0, rb1 = 0, mad0 = 999, mad1 = 999, gm0 = 0, rm0 = 0, gm1 = 0, rm1 = 0;
        const bool rbOk = vc0 && vc1 && GpuEye::Readback(0, g_gpuBuf[0], &rb0) && GpuEye::Readback(1, g_gpuBuf[1], &rb1);
        const bool match = rbOk && GpuMatches(g_gpuBuf[0], g_eyeBuf[0], &mad0, &gm0, &rm0) &&
                                   GpuMatches(g_gpuBuf[1], g_eyeBuf[1], &mad1, &gm1, &rm1);
        DebugLogger::LogFormat(
            "Render twice: GPU check %s | wrapper draw calls during the left DrawOTag: %ld | copies %d/%d read-back %s (%.2f+%.2f ms) | "
            "left: diff %.1f (GPU mean %.1f, flip mean %.1f) right: diff %.1f (GPU %.1f, flip %.1f) | flip %dx%d GPU %dx%d | %s",
            match ? "MATCH" : "mismatch", drawsLeft, vc0 ? 1 : 0, vc1 ? 1 : 0, rbOk ? "ok" : "FAILED", rb0, rb1,
            mad0, gm0, rm0, mad1, gm1, rm1, w0, h0, g_gpuBuf[0].width, g_gpuBuf[0].height, GpuEye::Describe());
        if (match) {
            if (++g_gpuVerifyPass >= 3) {
                g_gpuState = kGpuTrusted;
                g_govSamples = 0; g_govEma = 0.0;   // new path, fresh warm-up
                DebugLogger::LogFormat("Render twice: GPU eye capture VERIFIED -- switching to it (no extra presents, %dx%d per eye, read-back %s)",
                    g_gpuBuf[0].width, g_gpuBuf[0].height, g_cfgGpuDeferred ? "deferred to the end of the game frame" : "immediate");
            }
        }
        else if (++g_gpuVerifyFail >= 3) {
            GpuFail(drawsLeft == 0 ? "the copies do not match the real picture, and the wrapper made no draw calls during DrawOTag (it defers drawing to its Flip)"
                                   : "the copies do not match the real picture");
        }
    }

    PublishRenderTwicePair(g_eyeBuf[0], g_eyeBuf[1], g_pending.rec);
    g_liveTick.store(GetTickCount64(), std::memory_order_relaxed);
    g_pairsTotal++;

    g_accPairs++;
    g_accSort += MsBetween(t0, t1);
    g_accDraw += MsBetween(t1, t2);
    g_accLock += lock0 + lock1 + flip0 + flip1;
    g_accConv += conv0 + conv1;
    GovNotePair(MsBetween(tStart, Now()));
    g_govLastWasPair = true;

    if (g_pairsTotal <= 3) {
        DebugLogger::LogFormat(
            "Render twice: pair #%llu published %dx%d | half-IPD %.1f units | view record %s | "
            "sort %.2f ms, draw %.2f ms, flip %.2f+%.2f ms, lock %.2f+%.2f ms, copy %.2f+%.2f ms | IPD %s",
            g_pairsTotal, w0, h0, g_pending.half, g_pending.rec.valid ? "valid" : "none",
            MsBetween(t0, t1), MsBetween(t1, t2), flip0, flip1, lock0, lock1, conv0, conv1,
            g_pending.ipdFromHeadset ? "from headset" : "from ini");
    }
}

bool CheckSignatures() {
    bool ok = true;
    for (const Sig& sg : kSigs) {
        const uint8_t* p = reinterpret_cast<const uint8_t*>(g_base + sg.rva);
        if (std::memcmp(p, sg.bytes, (size_t)sg.n) != 0) {
            char got[3 * 12 + 1] = {};
            for (int i = 0; i < sg.n; ++i) sprintf_s(got + i * 3, 4, "%02X ", p[i]);
            DebugLogger::LogFormat("Render twice: SIGNATURE MISMATCH at mgsi.exe+%X (%s): got %s-- not installing",
                (unsigned)sg.rva, sg.what, got);
            ok = false;
        }
    }
    return ok;
}

}  // namespace

bool IsRenderTwiceLive() {
    const unsigned long long t = g_liveTick.load(std::memory_order_relaxed);
    return t != 0 && (GetTickCount64() - t) < 150;
}

bool InstallRenderTwiceHooks() {
#ifndef _M_IX86
    DebugLogger::Log("Render twice: not available in a 64-bit build");
    return false;
#else
    if (g_installed) return true;
    QueryPerformanceFrequency(&g_qpcFreq);
    LoadConfig();
    g_base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
    InstallSkyLockHooks();   // independent of render twice; installs even when it is disabled

    DebugLogger::LogFormat("Render twice (v6): capture=%s gpu_readback=%s | enabled=%d scope=%d (%s) log_perf=%d ipd_from_headset=%d | "
        "governor: late_frame_ms=%.0f max_frame_skips=%d max_extra_ms=%.0f%s cooldown_ms=%d max_cooldown_ms=%d",
        g_cfgCaptureGpu ? "gpu (verified against the flip path first)" : "flip", g_cfgGpuDeferred ? "deferred" : "immediate",
        g_cfgEnabled ? 1 : 0, g_cfgScope,
        g_cfgScope == 1 ? "first person only" : g_cfgScope == 2 ? "any head-driven VR camera" : "every 3D frame in VR mode",
        g_cfgLog ? 1 : 0, g_cfgIpdFromHeadset ? 1 : 0,
        g_cfgLateFrameMs, g_cfgMaxSkips, g_cfgMaxExtraMs, g_cfgMaxExtraMs <= 0.0 ? " (OFF)" : "", g_cfgCooldownMs, g_cfgMaxCooldownMs);
    if (!g_cfgEnabled) {
        DebugLogger::Log("Render twice: disabled in ini -- hooks not installed, alternate-eye stereo unchanged");
        return false;
    }
    if (!CheckSignatures()) return false;

    g_sortChanlSystem = reinterpret_cast<IntArgFn>(g_base + kSortChanlSystemRva);
    g_drawOTag = reinterpret_cast<DrawOTagFn>(g_base + kDrawOTagRva);

    LPVOID endFrame = reinterpret_cast<LPVOID>(g_base + kEndFrameRva);
    LPVOID drawChanl = reinterpret_cast<LPVOID>(g_base + kDrawChanlSystemRva);
    MH_STATUS s1 = MH_CreateHook(endFrame, reinterpret_cast<LPVOID>(&HookedEndFrame),
                                 reinterpret_cast<LPVOID*>(&g_origEndFrame));
    MH_STATUS s2 = MH_CreateHook(drawChanl, reinterpret_cast<LPVOID>(&HookedDrawChanl),
                                 reinterpret_cast<LPVOID*>(&g_origDrawChanl));
    if (s1 != MH_OK || s2 != MH_OK) {
        DebugLogger::LogFormat("Render twice: MH_CreateHook failed (EndFrame %d, DrawChanl %d) -- not installed", s1, s2);
        if (s1 == MH_OK) MH_RemoveHook(endFrame);
        if (s2 == MH_OK) MH_RemoveHook(drawChanl);
        return false;
    }
    // DrawChanl first: an EndFrame that runs without its partner would leave
    // the OT sorted for the left eye only, which is harmless (the game just
    // draws the left eye), but there is no reason to allow even that.
    s2 = MH_EnableHook(drawChanl);
    s1 = (s2 == MH_OK) ? MH_EnableHook(endFrame) : MH_UNKNOWN;
    if (s1 != MH_OK || s2 != MH_OK) {
        DebugLogger::LogFormat("Render twice: MH_EnableHook failed (EndFrame %d, DrawChanl %d) -- not installed", s1, s2);
        MH_DisableHook(drawChanl);
        MH_RemoveHook(endFrame);
        MH_RemoveHook(drawChanl);
        return false;
    }
    g_installed = true;
    DebugLogger::LogFormat(
        "Render twice: INSTALLED -- DG_EndFrame (+%X) and DG_DrawChanlSystem (+%X) hooked, sort +%X, "
        "DrawOTag +%X, renderer=%s",
        (unsigned)kEndFrameRva, (unsigned)kDrawChanlSystemRva, (unsigned)kSortChanlSystemRva,
        (unsigned)kDrawOTagRva, Read32(kSoftwareModeRva) ? "software" : "hardware (or not yet chosen)");
    return true;
#endif
}

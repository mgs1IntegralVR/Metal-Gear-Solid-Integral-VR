#include <windows.h>
#include <ddraw.h>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <intrin.h>
#include <vector>
#include <string>
#include <cstring>
#include <cstdio>
#include "MinHook.h"
#include "../include/debug_logging.h"
#include "../include/ddraw_hook.h"
#include "../include/camera_write_hook.h"

// Direct DirectDraw surface capture, take 2. Earlier attempts assumed this
// path was fundamentally unreliable against GOG's DirectDraw-to-D3D9
// wrapper, based on capture never producing visible pixels. Since then we've
// confirmed empirically (via d3d9_capture_hook.cpp's generic call logger)
// that the wrapper's IDirect3DDevice9 object is only used once at startup
// (Clear/SetRenderTarget/UpdateSurface/StretchRect fire exactly once, then
// never again) while DirectDrawSurface7::Flip fires continuously for the
// entire session -- meaning the wrapper's real per-frame rendering almost
// certainly still flows through the DirectDraw surface path, and the
// earlier failure was likely a capture bug, not a wrong-layer problem. This
// version adds detailed diagnostics (exact HRESULT, pixel format, and a raw
// byte sample) on every branch so a renewed failure tells us why instead of
// just "no frame yet".

using DirectDrawCreate_t = HRESULT(WINAPI*)(GUID*, LPVOID*, IUnknown*);
using DirectDrawCreateEx_t = HRESULT(WINAPI*)(GUID*, LPVOID*, REFIID, IUnknown*);
using DD7_CreateSurface_t = HRESULT(WINAPI*)(IDirectDraw7*, LPDDSURFACEDESC2, LPDIRECTDRAWSURFACE7*, IUnknown*);
using DDS7_Flip_t = HRESULT(WINAPI*)(IDirectDrawSurface7*, LPDIRECTDRAWSURFACE7, DWORD);
using DDS7_Blt_t = HRESULT(WINAPI*)(IDirectDrawSurface7*, LPRECT, LPDIRECTDRAWSURFACE7, LPRECT, DWORD, LPDDBLTFX);
using DDS7_BltFast_t = HRESULT(WINAPI*)(IDirectDrawSurface7*, DWORD, DWORD, LPDIRECTDRAWSURFACE7, LPRECT, DWORD);
using DDS7_GetDC_t = HRESULT(WINAPI*)(IDirectDrawSurface7*, HDC*);
using DDS7_ReleaseDC_t = HRESULT(WINAPI*)(IDirectDrawSurface7*, HDC);
using DDS7_Unlock_t = HRESULT(WINAPI*)(IDirectDrawSurface7*, LPRECT);

static DirectDrawCreate_t g_OriginalDirectDrawCreate = nullptr;
static DirectDrawCreateEx_t g_OriginalDirectDrawCreateEx = nullptr;
static DD7_CreateSurface_t g_OriginalDD7CreateSurface = nullptr;
static DDS7_Flip_t g_OriginalDDS7Flip = nullptr;
static DDS7_Blt_t g_OriginalDDS7Blt = nullptr;
static DDS7_BltFast_t g_OriginalDDS7BltFast = nullptr;
static DDS7_GetDC_t g_OriginalDDS7GetDC = nullptr;
static DDS7_ReleaseDC_t g_OriginalDDS7ReleaseDC = nullptr;
static DDS7_Unlock_t g_OriginalDDS7Unlock = nullptr;

static VRFrameTickFn g_OnFrameTick = nullptr;
static bool g_DD7Hooked = false;
static bool g_FlipHooked = false;
static bool g_BltHooked = false;
static bool g_BltFastHooked = false;
static bool g_MenuHooksInstalled = false;

// ---- PC-PORT MENUS (pause / LOAD GAME / SAVE GAME / options), 2026-09-23 ---
// Found by disassembly, after weeks of looking in the wrong layer. The PC
// port's own menus are NOT drawn into the software back buffer the 3D game
// renders into. They are drawn straight onto the PRIMARY surface with GDI:
//
//   +4313BD ("LOAD GAME"):  primary->Blt(COLORFILL) ; primary->GetDC ;
//                           SetBkMode / SetTextColor / SelectObject ;
//                           TextOutA ; primary->ReleaseDC
//
// (primary = [0x6FC734], created after the "Creating primary surface..."
// message; [0x6FC738] is the "back buffer for software rendering".) The whole
// menu system at +42D6xx..+438xxx works the same way, and while it is open the
// game presents nothing -- it is sitting in a Windows-style menu loop.
//
// Our capture only ever ran right after Flip, i.e. BEFORE any of that GDI
// drawing, and during the menu there are no flips at all. So the headset kept
// the last gameplay frame while the monitor -- which shows the primary --
// showed the menu. Now every draw that reaches the primary outside a
// present (ReleaseDC, a colour fill, a foreign Unlock) triggers a capture of
// the primary as it actually is, and while such drawing is recent the
// after-Flip capture stands down so a clean frame cannot overwrite the menu.
static IDirectDrawSurface7* g_primarySurface = nullptr;
static bool   g_menuCaptureEnabled = true;       // [capture] menu_capture
static int    g_menuHoldMs = 400;                // [capture] menu_hold_ms
static int    g_menuMinIntervalMs = 20;          // [capture] menu_min_interval_ms
static int    g_stallWatchdogMs = 2000;          // [capture] stall_watchdog_ms (0 = off)
static std::mutex g_primaryCapMutex;             // serialises every primary capture
static std::atomic<bool>     g_offFlipDirty{ false };
static std::atomic<long long> g_lastOffFlipDrawMs{ -1000000 };
static std::atomic<long long> g_lastPresentMs{ 0 };
static std::atomic<long long> g_lastCaptureMs{ 0 };
static std::atomic<int>       g_dcOutstanding{ 0 };
static std::atomic<bool>      g_menuWorkerStarted{ false };
static thread_local bool      t_inOwnCapture = false;
static long long g_offFlipCaptures = 0;

// --- pause-menu investigation: Blt/BltFast logging -------------------------
// The primary surface only ever receives full-frame Flip()s during normal
// gameplay -- that path is already hooked and captured above. If the pause
// menu (or codec call, or any other overlay) is instead drawn as a partial
// Blt/BltFast onto a surface, that draw would currently be invisible to us:
// we would only ever see whatever was already in the framebuffer from the
// last Flip. This does not attempt to capture the blitted pixels (that is a
// separate, larger change); it only logs WHERE and FROM WHAT each blit
// draws, cheaply, so that turning the pause menu on/off and diffing the log
// tells us definitively whether the menu goes through this path, and if so,
// what its destination rect and source surface look like -- enough to turn
// a future capture attempt into a targeted one instead of "log everything".
static int g_bltLogCount = 0;
static int g_bltFastLogCount = 0;
// Only the first N calls per session are logged in full; Blt/BltFast can be
// called many times per frame by normal rendering (e.g. layered UI, HUD
// elements the software renderer composites this way), and we don't want to
// flood the log or add per-call overhead once the shape of the traffic is
// established. Bump the cap in the ini if a longer capture window is needed.
//
// RAISED 200 -> 600, 2026-08-16. Two separate floods have now each eaten the
// entire 200-call budget before the pause menu was ever opened: the startup
// colorfill burst (fixed 2026-08-15) and the screen-wipe self-blits (fixed
// today). Both are filtered out now, so 600 is not about absorbing noise --
// it is headroom so that a third, unanticipated flood cannot silently cost
// another test session. The two filters are the real fix; this is insurance.
static int g_bltLogCap = 600;
static int g_bltFastLogCap = 600;

static std::mutex g_captureMutex;
static std::vector<uint8_t> g_capturedRgba;
static int32_t g_capturedWidth = 0;
static int32_t g_capturedHeight = 0;
static bool g_hasCapturedFrame = false;
// Which eye this captured frame was rendered for, when alternate-eye stereo is
// running: 0 = left, 1 = right, -1 = mono. Tagged at capture time on the game
// thread, so it always matches the camera offset that produced the pixels.
static int g_capturedEye = -1;
// The head pose the frame was drawn from, taken at the same moment as the eye
// tag and for the same reason: only here, on the game thread at flip, is it
// certain which camera produced these pixels.
static FrameViewRecord g_capturedViewRec;
// The wrist-HUD pass already ran on this frame (converter thread).
static bool g_capturedHudDone = false;
static bool g_lastHandedHudDone = false;

// RENDER TWICE (2026-09-24): a true stereo pair waiting to be handed out, one
// eye per GetLatestDdrawFrameRgba() call. Guarded by g_captureMutex.
struct RtQueuedFrame {
	RtEyeImage img;
	int eye = -1;
	FrameViewRecord rec;
};
static RtQueuedFrame g_rtQueue[2];
static int g_rtQueueHead = 0;
static int g_rtQueueCount = 0;
// When the last pair was published. While pairs are flowing the after-Flip
// capture stands down: it would read the SECOND eye's image back off the
// primary a frame later and send it to both eyes as mono.
static std::atomic<long long> g_rtLastPairMs{ -1000000 };
// Last pair that the flip path presented itself (skip_game_flip keys off this;
// GPU-captured pairs present nothing, so the game's own Flip must still run).
static std::atomic<long long> g_rtLastOwnPresentMs{ -1000000 };
// [render_twice] skip_game_flip: while pairs are flowing, the game's own Flip
// at the start of each frame only re-shows the left eye on the monitor -- the
// headset already has both. Skipping it saves a present through the wrapper.
static bool g_rtSkipGameFlip = true;
static long long g_rtSkippedGameFlips = 0;

// ---- WHICH FRAME IS ON THE PRIMARY AFTER A PRESENT (2026-09-24) -----------
// Static analysis of DG_StartFrame (+1ADE hw / +1B60 sw): the present happens
// in PutDispEnv at the START of a frame, BEFORE that frame's DrawOTag. So the
// image a present shows was drawn at the start of the PREVIOUS frame, from the
// camera the rotation hook set one frame before that. Reading the eye tag and
// the view record at present time labelled every captured image with the
// NEXT frame's eye and head pose: 33 ms stale, and -- with alternate-eye
// stereo -- the other eye. [capture] tag_delay_frames=1 (default) holds the
// tag back one present so it matches the pixels. Render twice is exact either
// way; this is for every frame it does not cover (menus, third-person VR,
// cutscenes, and alternate-eye stereo as a fallback).
struct PresentTag { int eye = -1; FrameViewRecord rec; };
static int g_tagDelayFrames = 1;
static PresentTag g_prevPresentTag;
static PresentTag g_thisPresentTag;
static void AdvancePresentTag() {
	PresentTag cur;
	cur.eye = GetCurrentStereoEye();
	cur.rec = ConsumeFrameViewRecord();
	if (g_tagDelayFrames > 0) {
		g_thisPresentTag = g_prevPresentTag;
		g_prevPresentTag = cur;
	}
	else {
		g_thisPresentTag = cur;
	}
}

static int g_captureAttemptCount = 0;
static int g_captureSuccessCount = 0;
// Bumped once per NEW frame in the shared buffer. The VR thread runs at HMD
// rate (72 Hz) while the game produces at most 30 -- and far fewer than that
// once the throttle below engages -- so without this it re-copies, re-scales
// and re-uploads the same pixels several times over for every real frame. That
// work lands on the CPU at exactly the moments we can least afford it.
static uint64_t g_captureSequence = 0;

// --- adaptive capture throttle ----------------------------------------------
// Measured, not theorised: in menus, codec calls, cutscenes and underwater the
// PERF lines read
//     game 8.1 fps (123.7 ms/frame) | our capture 108.56 ms (88% of it)
//     | lock avg 107.97 | convert avg 0.60
// while healthy gameplay reads
//     game 30.0 fps (33.3 ms/frame) | our capture 4.29 ms (13%) | lock avg 3.85
//
// Conversion is flat at ~0.5 ms in both. The entire difference is Lock(). The
// primary surface is VIDEOMEMORY|3DDEVICE, so Lock() has to drain the GPU
// pipeline and read the framebuffer back over the bus; when the GPU has a
// deep queue (lots of overdraw -- water, full-screen video, layered menus)
// that drain is the whole frame. We are not measuring the slowdown, we ARE
// the slowdown.
//
// So: watch the rolling Lock cost and, when it goes bad, stop capturing every
// flip. At 1-in-N the game gets N-1 frames where the CPU never blocks on the
// GPU, and the headset keeps showing the last captured frame -- which in a
// codec call or a menu is nearly static anyway. Trading capture rate for game
// rate is the right trade in exactly the places this triggers.
//
// It is also the decisive experiment: if the game speeds up when we capture
// less, the cause is definitively ours.
//
// ---------------------------------------------------------------------------
// SESSION 2 CORRECTION -- THIS MECHANISM WAS BLINDING ONE EYE.
//
// The comment that used to sit in HookedDDSurface7Flip claimed stereo was safe
// because the alternate-eye path falls back to mono below stereo_min_fps. That
// was wrong, and the first headset log proves it.
//
// The eye alternates once per GAME FRAME: L, R, L, R. Capturing "1 flip in 2"
// samples every other frame -- which against a period-2 alternation means it
// samples THE SAME EYE every single time. One eye simply stops receiving new
// frames for as long as the throttle is engaged, while the other keeps
// updating. The log showed four consecutive `tagged eye=0 (left)` and not one
// `eye=1`.
//
// It was also firing constantly: budget 8 ms against a measured average lock of
// 3-5 ms, flapping between 1-in-1 and 1-in-2 about twice a second, on noise.
//
// Two fixes, both below:
//   * the default budget is now 20 ms, well clear of the real average, so the
//     throttle only engages when something is genuinely wrong;
//   * the skip count is forced ODD while stereo is live. Odd counts still
//     visit both eyes (1-in-3 against L,R,L,R alternates fine); only EVEN
//     counts alias. This makes the throttle harmless rather than merely rare.
// ---------------------------------------------------------------------------
static bool   g_captureThrottleEnabled = true;
static double g_captureMaxLockMs = 20.0;  // above this rolling average, start skipping
static int    g_captureMaxSkip = 8;       // never capture less often than 1 flip in 8
static double g_lockAvgMs = 0.0;          // exponential moving average of Lock() cost
static bool   g_haveLockAvg = false;
static int    g_captureSkipN = 1;         // capture 1 flip in N (1 = every flip)
static int    g_flipsSinceCapture = 0;
static long long g_flipCount = 0;         // every flip, captured or not
static long long g_skippedCaptures = 0;
static bool g_lockNoWait = true;          // ini capture/lock_nowait
static int  g_lockNoWaitMaxBusy = 4;      // force one blocking lock after this many busy replies

static void LoadCaptureConfig() {
	char exePath[MAX_PATH] = { 0 };
	GetModuleFileNameA(NULL, exePath, MAX_PATH);
	std::string dir(exePath);
	auto slash = dir.find_last_of("\\/");
	if (slash != std::string::npos) dir = dir.substr(0, slash);
	const std::string ini = dir + "\\mgs1_vr_config.ini";

	g_captureThrottleEnabled = GetPrivateProfileIntA("capture", "adaptive_throttle", 1, ini.c_str()) != 0;
	g_captureMaxLockMs = (double)GetPrivateProfileIntA("capture", "max_lock_ms", 20, ini.c_str());
	g_captureMaxSkip = GetPrivateProfileIntA("capture", "max_skip", 8, ini.c_str());
	if (g_captureMaxLockMs < 1.0) g_captureMaxLockMs = 1.0;
	if (g_captureMaxSkip < 1) g_captureMaxSkip = 1;
	if (g_captureMaxSkip > 30) g_captureMaxSkip = 30;
	g_lockNoWait = GetPrivateProfileIntA("capture", "lock_nowait", 1, ini.c_str()) != 0;
	g_lockNoWaitMaxBusy = GetPrivateProfileIntA("capture", "lock_nowait_max_busy", 4, ini.c_str());
	if (g_lockNoWaitMaxBusy < 1)  g_lockNoWaitMaxBusy = 1;
	if (g_lockNoWaitMaxBusy > 60) g_lockNoWaitMaxBusy = 60;

	// Pause-menu investigation: how many Blt/BltFast calls to log in full
	// before going quiet. 0 disables logging for that call entirely (the
	// hook still installs and passes through, it just never logs) -- useful
	// once the investigation is done and this becomes noise.
	g_bltLogCap = GetPrivateProfileIntA("capture", "blt_log_cap", 600, ini.c_str());
	g_bltFastLogCap = GetPrivateProfileIntA("capture", "bltfast_log_cap", 600, ini.c_str());
	if (g_bltLogCap < 0) g_bltLogCap = 0;
	if (g_bltFastLogCap < 0) g_bltFastLogCap = 0;

	g_tagDelayFrames = GetPrivateProfileIntA("capture", "tag_delay_frames", 1, ini.c_str()) != 0 ? 1 : 0;
	g_rtSkipGameFlip = GetPrivateProfileIntA("render_twice", "skip_game_flip", 1, ini.c_str()) != 0;
	g_menuCaptureEnabled = GetPrivateProfileIntA("capture", "menu_capture", 1, ini.c_str()) != 0;
	g_menuHoldMs = GetPrivateProfileIntA("capture", "menu_hold_ms", 400, ini.c_str());
	g_menuMinIntervalMs = GetPrivateProfileIntA("capture", "menu_min_interval_ms", 20, ini.c_str());
	g_stallWatchdogMs = GetPrivateProfileIntA("capture", "stall_watchdog_ms", 2000, ini.c_str());
	if (g_menuHoldMs < 50) g_menuHoldMs = 50;
	if (g_menuMinIntervalMs < 0) g_menuMinIntervalMs = 0;
	DebugLogger::LogFormat("Menu capture: %s (hold %d ms, min interval %d ms)",
		g_menuCaptureEnabled ? "ON -- PC-port menus drawn onto the primary with GDI reach the headset"
		                     : "OFF -- pause/load menus will only show on the monitor",
		g_menuHoldMs, g_menuMinIntervalMs);

	DebugLogger::LogFormat(
		"Capture config: adaptive_throttle=%d max_lock_ms=%.0f max_skip=%d lock_nowait=%d lock_nowait_max_busy=%d blt_log_cap=%d bltfast_log_cap=%d",
		g_captureThrottleEnabled ? 1 : 0, g_captureMaxLockMs, g_captureMaxSkip,
		g_lockNoWait ? 1 : 0, g_lockNoWaitMaxBusy, g_bltLogCap, g_bltFastLogCap);
	DebugLogger::LogFormat(
		"Capture tags: tag_delay_frames=%d (%s) | render twice skip_game_flip=%d",
		g_tagDelayFrames, g_tagDelayFrames ? "eye tag + head pose taken from the frame the pixels were DRAWN in"
		                                   : "legacy: tag read at present time, one frame ahead of the pixels",
		g_rtSkipGameFlip ? 1 : 0);
}

// Called after every successful Lock with that Lock's cost. Keeps a smoothed
// average and moves the skip level toward "how many multiples of the budget
// we're over", one step at a time in each direction so it can't oscillate.
static void UpdateCaptureThrottle(double lockMs) {
	if (!g_haveLockAvg) {
		g_lockAvgMs = lockMs;
		g_haveLockAvg = true;
	}
	else {
		g_lockAvgMs = g_lockAvgMs * 0.85 + lockMs * 0.15;
	}

	if (!g_captureThrottleEnabled) {
		g_captureSkipN = 1;
		return;
	}

	const int previous = g_captureSkipN;

	if (g_lockAvgMs > g_captureMaxLockMs) {
		int wanted = (int)(g_lockAvgMs / g_captureMaxLockMs) + 1;
		// One step per capture in EITHER direction, so it settles instead of
		// jumping between extremes as the average crosses the budget.
		if (wanted > g_captureSkipN + 1) wanted = g_captureSkipN + 1;
		if (wanted < g_captureSkipN - 1) wanted = g_captureSkipN - 1;
		if (wanted > g_captureMaxSkip)   wanted = g_captureMaxSkip;
		if (wanted < 1)                  wanted = 1;
		g_captureSkipN = wanted;
	}
	else if (g_lockAvgMs < g_captureMaxLockMs * 0.5 && g_captureSkipN > 1) {
		g_captureSkipN--;   // recover gently once the scene is cheap again
	}

	// ---- NEVER LET THE SKIP COUNT ALIAS THE STEREO EYE ---------------------
	// The eye alternates L,R,L,R once per game frame. Capturing 1 flip in 2
	// therefore samples the SAME EYE every time, forever -- one eye simply
	// stops receiving frames for as long as the throttle is engaged. Every
	// EVEN skip count has this property; every ODD one still visits both eyes.
	//
	// Rounding UP to odd rather than down keeps the throttle's intent (it
	// wanted to capture less often, and it still does).
	//
	// GetCurrentStereoEye() returns -1 when stereo is off OR when the tag has
	// expired because the game's camera update stopped -- in both of those
	// cases there is no alternation to alias with, so the throttle is free to
	// use any count it likes.
	if (g_captureSkipN > 1 && (g_captureSkipN % 2) == 0 && GetCurrentStereoEye() != -1) {
		const int odd = (g_captureSkipN + 1 <= g_captureMaxSkip)
			? g_captureSkipN + 1
			: g_captureSkipN - 1;
		static int loggedAliasFix = 0;
		if (loggedAliasFix < 3) {
			loggedAliasFix++;
			DebugLogger::LogFormat(
				"Capture throttle: skip %d would alias the alternate-eye pattern and starve one eye "
				"-- using %d instead", g_captureSkipN, odd);
		}
		g_captureSkipN = odd;
		if (g_captureSkipN < 1) g_captureSkipN = 1;
	}

	if (g_captureSkipN != previous) {
		DebugLogger::LogFormat(
			"Capture throttle: %s -- lock avg %.1f ms (budget %.0f), now capturing 1 flip in %d",
			(g_captureSkipN > previous) ? "BACKING OFF" : "recovering",
			g_lockAvgMs, g_captureMaxLockMs, g_captureSkipN);
	}
}

static bool PatchVTable(void* obj, int index, void* hook, void** original) {
	if (!obj) return false;
	void*** pVTableObj = reinterpret_cast<void***>(obj);
	void** vtbl = *pVTableObj;
	if (!vtbl) return false;

	DWORD oldProtect = 0;
	if (!VirtualProtect(&vtbl[index], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) {
		return false;
	}

	if (original && *original == nullptr) {
		*original = vtbl[index];
	}
	vtbl[index] = hook;

	DWORD ignore = 0;
	VirtualProtect(&vtbl[index], sizeof(void*), oldProtect, &ignore);
	return true;
}

static inline uint8_t ExtractChannel8(uint32_t pixel, uint32_t mask, unsigned long shift, uint32_t maxValue) {
	if (mask == 0 || maxValue == 0) {
		return 0;
	}
	uint32_t value = (pixel & mask) >> shift;
	return static_cast<uint8_t>((value * 255u) / maxValue);
}

// --- timing -----------------------------------------------------------------
static LARGE_INTEGER QpcFreq() {
	static LARGE_INTEGER f = [] { LARGE_INTEGER v{}; QueryPerformanceFrequency(&v); return v; }();
	return f;
}
static LARGE_INTEGER QpcNow() {
	LARGE_INTEGER v{};
	QueryPerformanceCounter(&v);
	return v;
}
static double QpcMillisSince(const LARGE_INTEGER& start) {
	const LARGE_INTEGER now = QpcNow();
	const LARGE_INTEGER f = QpcFreq();
	if (f.QuadPart == 0) return 0.0;
	return (double)(now.QuadPart - start.QuadPart) * 1000.0 / (double)f.QuadPart;
}

// --- 16-bit -> RGBA lookup table --------------------------------------------
// 65,536 entries x 4 bytes = 256 KB, built once per pixel-format. Every
// 16-bit source value maps to a finished RGBA pixel, so the per-pixel
// conversion work drops to a single indexed load.
struct Rgb16Lut {
	std::vector<uint32_t> table;
	uint32_t rMask = 0, gMask = 0, bMask = 0;
	bool built = false;
};
static Rgb16Lut g_rgb16Lut;

static void EnsureRgb16Lut(uint32_t rMask, uint32_t gMask, uint32_t bMask,
	unsigned long rShift, uint32_t rMax,
	unsigned long gShift, uint32_t gMax,
	unsigned long bShift, uint32_t bMax) {
	if (g_rgb16Lut.built && g_rgb16Lut.rMask == rMask &&
		g_rgb16Lut.gMask == gMask && g_rgb16Lut.bMask == bMask) {
		return;
	}
	g_rgb16Lut.table.resize(65536);
	for (uint32_t p = 0; p < 65536u; ++p) {
		const uint32_t r = ExtractChannel8(p, rMask, rShift, rMax);
		const uint32_t g = ExtractChannel8(p, gMask, gShift, gMax);
		const uint32_t b = ExtractChannel8(p, bMask, bShift, bMax);
		// Little-endian byte order matches the old dstPixel[0..3] = R,G,B,255.
		g_rgb16Lut.table[p] = r | (g << 8) | (b << 16) | 0xFF000000u;
	}
	g_rgb16Lut.rMask = rMask;
	g_rgb16Lut.gMask = gMask;
	g_rgb16Lut.bMask = bMask;
	g_rgb16Lut.built = true;
	DebugLogger::LogFormat(
		"Capture: built 16-bit RGBA lookup table (R=0x%08X G=0x%08X B=0x%08X) -- per-pixel conversion is now a single table read",
		rMask, gMask, bMask);
}

static void MaskToShiftAndMax(uint32_t mask, unsigned long& outShift, uint32_t& outMax) {
	outShift = 0;
	outMax = 0;
	if (mask == 0) {
		return;
	}
	while (((mask >> outShift) & 1u) == 0) {
		outShift++;
	}
	outMax = mask >> outShift;
}

// Captures the just-flipped primary surface with full diagnostics on every
// branch, so a failure tells us exactly what happened rather than just "no
// frame yet".
// mode: 0 = the normal after-Flip capture (eye-tagged, may block),
//       1 = off-flip capture on the game thread (menus drawn straight onto the
//           primary with GDI -- always mono, no view record),
//       2 = off-flip capture from the menu flush worker thread: mono, and it
//           must NEVER block (the game thread may be about to touch the surface).
static void CaptureFlippedSurfaceToSharedBuffer(IDirectDrawSurface7* surface, int mode = 0) {
	if (!surface) {
		return;
	}

	g_captureAttemptCount++;
	bool logThisAttempt = (g_captureAttemptCount <= 5) || (g_captureAttemptCount % 120 == 0);

	DDSURFACEDESC2 desc{};
	desc.dwSize = sizeof(desc);
	// Time the Lock itself, separately from the conversion.
	//
	// This surface lives in VIDEO memory (caps say VIDEOMEMORY|3DDEVICE), so
	// Lock() is not a cheap pointer handout -- it has to drain the GPU pipeline
	// and pull the framebuffer back across the bus into CPU-readable memory.
	// That cost scales with pixel count and is invisible in the conversion
	// timer, which only starts after Lock returns. If the Lock is the expensive
	// half, then the resolution problem is OUR capture method, not the game.
	//
	// Second lever, ahead of the throttle: DDLOCK_WAIT is what turns "read the
	// framebuffer" into "block this thread until the GPU is finished". Without
	// it, a busy surface returns DDERR_WASSTILLDRAWING immediately and we
	// simply don't get a frame this flip -- which costs us a capture and costs
	// the game nothing. That is the correct default: the game's frame rate is
	// worth more than our capture rate.
	//
	// The safety net matters, though. If the wrapper never reports the surface
	// as free we would capture nothing at all and the headset would show a
	// frozen image forever, so after a few consecutive busy replies we take one
	// blocking lock to guarantee the picture keeps moving. Worst case this
	// degrades to "block once every few frames" -- still far better than
	// blocking on every one.
	static int s_consecutiveBusy = 0;
	const bool forceBlocking = (mode != 2) && (!g_lockNoWait || (s_consecutiveBusy >= g_lockNoWaitMaxBusy));

	const LARGE_INTEGER lockStart = QpcNow();
	DWORD lockFlags = DDLOCK_READONLY | DDLOCK_NOSYSLOCK;
	if (forceBlocking) lockFlags |= DDLOCK_WAIT;
	HRESULT hr = surface->Lock(nullptr, &desc, lockFlags, nullptr);
	double lockMs = QpcMillisSince(lockStart);

	if (hr == DDERR_WASSTILLDRAWING || hr == DDERR_SURFACEBUSY) {
		s_consecutiveBusy++;
		if (s_consecutiveBusy == 1 || s_consecutiveBusy % 300 == 0) {
			DebugLogger::LogFormat(
				"Capture: surface still drawing, skipped this flip without blocking (%d in a row; "
				"a blocking lock is forced after %d)", s_consecutiveBusy, g_lockNoWaitMaxBusy);
		}
		// A skipped capture is a fast frame for the game, so don't let it drag
		// the Lock average down and hide a genuinely expensive scene.
		return;
	}

	if (FAILED(hr)) {
		if (logThisAttempt) {
			DebugLogger::LogFormat("Capture attempt #%d: IDirectDrawSurface7::Lock failed: 0x%08X%s",
				g_captureAttemptCount, (unsigned)hr,
				forceBlocking ? "" : " (non-blocking attempt)");
		}
		if (!forceBlocking && mode != 2) {
			// Some unexpected error from the non-blocking path -- retry once
			// the old way so an unusual wrapper can't cost us capture entirely.
			desc = DDSURFACEDESC2{};
			desc.dwSize = sizeof(desc);
			const LARGE_INTEGER retryStart = QpcNow();
			hr = surface->Lock(nullptr, &desc, DDLOCK_WAIT | DDLOCK_READONLY | DDLOCK_NOSYSLOCK, nullptr);
			lockMs = QpcMillisSince(retryStart);
			if (FAILED(hr)) {
				if (g_lockNoWait) {
					DebugLogger::Log("Capture: non-blocking lock is not usable on this wrapper -- falling back to blocking locks");
					g_lockNoWait = false;
				}
				return;
			}
		}
		else {
			return;
		}
	}

	s_consecutiveBusy = 0;

	if (mode == 0) UpdateCaptureThrottle(lockMs);

	const DDPIXELFORMAT& pf = desc.ddpfPixelFormat;
	int32_t width = static_cast<int32_t>(desc.dwWidth);
	int32_t height = static_cast<int32_t>(desc.dwHeight);
	LONG pitch = desc.lPitch;

	if (logThisAttempt) {
		DebugLogger::LogFormat(
			"Capture attempt #%d: Lock OK. %dx%d pitch=%ld pfFlags=0x%08X bpp=%u masks R=0x%08X G=0x%08X B=0x%08X A=0x%08X lpSurface=%p",
			g_captureAttemptCount, width, height, pitch,
			(unsigned)pf.dwFlags, (unsigned)pf.dwRGBBitCount,
			(unsigned)pf.dwRBitMask, (unsigned)pf.dwGBitMask, (unsigned)pf.dwBBitMask, (unsigned)pf.dwRGBAlphaBitMask,
			desc.lpSurface);

		// ---- SOFTWARE vs HARDWARE, decided by the surface itself -----------
		// The surface caps settle the argument that "no BeginScene fired" only
		// hinted at. A Direct3D7 render target lives in VIDEOMEMORY and carries
		// DDSCAPS_3DDEVICE; the game's software rasteriser draws into a
		// SYSTEMMEMORY surface. This is the flag, not an inference.
		{
			const DWORD caps = desc.ddsCaps.dwCaps;
			DebugLogger::LogFormat(
				"Capture attempt #%d: surface caps=0x%08X [%s%s%s%s%s%s] -> VERDICT: %s",
				g_captureAttemptCount, (unsigned)caps,
				(caps & DDSCAPS_SYSTEMMEMORY) ? "SYSTEMMEMORY " : "",
				(caps & DDSCAPS_VIDEOMEMORY) ? "VIDEOMEMORY " : "",
				(caps & DDSCAPS_3DDEVICE) ? "3DDEVICE " : "",
				(caps & DDSCAPS_PRIMARYSURFACE) ? "PRIMARY " : "",
				(caps & DDSCAPS_BACKBUFFER) ? "BACKBUFFER " : "",
				(caps & DDSCAPS_OFFSCREENPLAIN) ? "OFFSCREENPLAIN " : "",
				(caps & DDSCAPS_3DDEVICE)
					? "HARDWARE (Direct3D render target)"
					: ((caps & DDSCAPS_SYSTEMMEMORY) ? "SOFTWARE (system-memory surface)"
													 : "UNCLEAR -- video memory but not a 3D render target"));
		}

		if (desc.lpSurface && width > 0 && height > 0) {
			const uint8_t* p = reinterpret_cast<const uint8_t*>(desc.lpSurface);
			// Sample the first 16 bytes and the pixel at the center of the
			// image, so we can tell a genuinely blank/black surface (all
			// zero bytes) apart from a format-misinterpretation (non-zero
			// bytes that just don't decode to what we expect).
			char sampleHex[16 * 3 + 1] = {};
			for (int i = 0; i < 16 && i < pitch; ++i) {
				sprintf_s(sampleHex + i * 3, 4, "%02X ", p[i]);
			}
			size_t centerOffset = (size_t)(height / 2) * (size_t)pitch + (size_t)(width / 2) * (pf.dwRGBBitCount / 8u);
			uint32_t centerPixel = 0;
			if (pf.dwRGBBitCount == 32) centerPixel = *reinterpret_cast<const uint32_t*>(p + centerOffset);
			else if (pf.dwRGBBitCount == 16) centerPixel = *reinterpret_cast<const uint16_t*>(p + centerOffset);
			DebugLogger::LogFormat("Capture attempt #%d: first 16 bytes = %s | center pixel raw = 0x%08X", g_captureAttemptCount, sampleHex, centerPixel);
		}
	}

	bool supported = (pf.dwFlags & DDPF_RGB) != 0 &&
		(pf.dwRGBBitCount == 32 || pf.dwRGBBitCount == 24 || pf.dwRGBBitCount == 16) &&
		width > 0 && height > 0 && desc.lpSurface != nullptr;

	if (!supported) {
		if (logThisAttempt) {
			DebugLogger::LogFormat("Capture attempt #%d: format not supported (flags=0x%08X bpp=%u) or zero dimensions", g_captureAttemptCount, (unsigned)pf.dwFlags, (unsigned)pf.dwRGBBitCount);
		}
		surface->Unlock(nullptr);
		return;
	}

	unsigned long rShift, gShift, bShift;
	uint32_t rMax, gMax, bMax;
	MaskToShiftAndMax(pf.dwRBitMask, rShift, rMax);
	MaskToShiftAndMax(pf.dwGBitMask, gShift, gMax);
	MaskToShiftAndMax(pf.dwBBitMask, bShift, bMax);

	// -----------------------------------------------------------------------
	// Scratch buffer, reused across frames.
	//
	// This used to allocate a fresh std::vector every single frame. At
	// 1280x960 that is a 4.9 MB heap allocation, zero-fill and free per frame,
	// on the GAME's thread -- it lands directly in the game's frame time. We
	// now keep one buffer and SWAP it with the published one, so after the
	// first frame we are handed the previous buffer back and never allocate
	// again at a steady resolution.
	// -----------------------------------------------------------------------
	static std::vector<uint8_t> s_scratch;
	const size_t needBytes = static_cast<size_t>(width) * static_cast<size_t>(height) * 4u;
	if (s_scratch.size() != needBytes) s_scratch.resize(needBytes);

	const uint8_t* srcBase = reinterpret_cast<const uint8_t*>(desc.lpSurface);
	uint32_t bytesPerPixel = pf.dwRGBBitCount / 8u;

	const LARGE_INTEGER convStart = QpcNow();
	uint32_t nonZeroPixelCount = 0;
	bool countedNonZero = false;

	if (bytesPerPixel == 2) {
		// --- 16-bit fast path -----------------------------------------------
		// The old code did three ExtractChannel8() calls per pixel -- mask,
		// shift, then a widening multiply-divide to scale the channel to 0..255.
		// That is ~10 ops per pixel, and it was measured at 10 ms for 640x480
		// and 43 ms for 1280x960 (exactly 4x, i.e. purely per-pixel bound).
		//
		// A 16-bit source has only 65,536 possible values, so the entire
		// conversion collapses into one 256 KB lookup table built once. The
		// inner loop becomes a load, a table index and a 32-bit store.
		EnsureRgb16Lut(pf.dwRBitMask, pf.dwGBitMask, pf.dwBBitMask,
			rShift, rMax, gShift, gMax, bShift, bMax);
		const uint32_t* lut = g_rgb16Lut.table.data();

		for (int32_t y = 0; y < height; ++y) {
			const uint16_t* srcRow = reinterpret_cast<const uint16_t*>(
				srcBase + static_cast<size_t>(y) * static_cast<size_t>(pitch));
			uint32_t* dstRow = reinterpret_cast<uint32_t*>(
				s_scratch.data() + static_cast<size_t>(y) * static_cast<size_t>(width) * 4u);
			for (int32_t x = 0; x < width; ++x) {
				dstRow[x] = lut[srcRow[x]];
			}
		}

		// The non-zero percentage is a diagnostic only. Counting it per pixel
		// on every frame costs a branch per pixel forever to serve a line that
		// prints once every 120 frames, so it now runs only when we log.
		if (logThisAttempt) {
			countedNonZero = true;
			for (int32_t y = 0; y < height; ++y) {
				const uint16_t* srcRow = reinterpret_cast<const uint16_t*>(
					srcBase + static_cast<size_t>(y) * static_cast<size_t>(pitch));
				for (int32_t x = 0; x < width; ++x) {
					if (srcRow[x] != 0) nonZeroPixelCount++;
				}
			}
		}
	}
	else {
		// --- generic 24/32-bit path (unchanged) ------------------------------
		countedNonZero = true;
		for (int32_t y = 0; y < height; ++y) {
			const uint8_t* srcRow = srcBase + static_cast<size_t>(y) * static_cast<size_t>(pitch);
			uint8_t* dstRow = s_scratch.data() + static_cast<size_t>(y) * static_cast<size_t>(width) * 4u;

			for (int32_t x = 0; x < width; ++x) {
				uint32_t pixel = 0;
				const uint8_t* srcPixel = srcRow + static_cast<size_t>(x) * bytesPerPixel;
				if (bytesPerPixel == 4) {
					pixel = *reinterpret_cast<const uint32_t*>(srcPixel);
				}
				else {
					pixel = srcPixel[0] | (srcPixel[1] << 8) | (srcPixel[2] << 16);
				}

				if (pixel != 0) {
					nonZeroPixelCount++;
				}

				uint8_t* dstPixel = dstRow + static_cast<size_t>(x) * 4u;
				dstPixel[0] = ExtractChannel8(pixel, pf.dwRBitMask, rShift, rMax);
				dstPixel[1] = ExtractChannel8(pixel, pf.dwGBitMask, gShift, gMax);
				dstPixel[2] = ExtractChannel8(pixel, pf.dwBBitMask, bShift, bMax);
				dstPixel[3] = 255;
			}
		}
	}

	const double convMs = QpcMillisSince(convStart);

	surface->Unlock(nullptr);

	{
		std::lock_guard<std::mutex> lock(g_captureMutex);
		// swap, not move: we get the previous buffer back as next frame's
		// scratch, which is why the steady state allocates nothing.
		g_capturedRgba.swap(s_scratch);
		g_capturedHudDone = false;
		g_capturedWidth = width;
		g_capturedHeight = height;
		// The eye this frame belongs to. GetCurrentStereoEye() now EXPIRES a
		// stale tag: during a cutscene, codec call or the pause menu the game
		// stops running its camera update, so the tag stops being refreshed
		// and would otherwise route the whole scene into one eye. Past the
		// timeout it reports -1 (mono) and the frame goes to both eyes.
		if (mode == 0) {
			g_capturedEye = g_thisPresentTag.eye;
			g_capturedViewRec = g_thisPresentTag.rec;
		}
		else {
			// A PC-port menu drawn onto the primary with GDI: a flat 2D picture,
			// identical for both eyes and drawn from no camera at all.
			g_capturedEye = -1;
			g_capturedViewRec = FrameViewRecord{};
		}
		g_hasCapturedFrame = true;
		g_captureSequence++;
	}

	g_captureSuccessCount++;

	// ---- rolling performance summary ---------------------------------------
	// One line every few seconds giving the game's ACTUAL frame rate alongside
	// what our capture cost. Until now frame rate had to be inferred by
	// counting log lines and doing arithmetic, which made every "it's slow
	// here" report a research task. This makes underwater, menus, codec and
	// cutscenes directly comparable, and separates the game's cost from ours.
	//
	// NOTE: game frame rate is counted from FLIPS, not from captures. Once the
	// throttle starts skipping, captures no longer happen once per game frame,
	// and counting them would report the game as slower exactly when the
	// throttle is making it faster -- i.e. it would hide its own fix.
	{
		static LARGE_INTEGER windowStart = QpcNow();
		static long long windowStartFlips = 0;
		static int    windowCaptures = 0;
		static double windowLockMs = 0.0;
		static double windowConvMs = 0.0;
		static double windowWorstLock = 0.0;

		windowCaptures++;
		windowLockMs += lockMs;
		windowConvMs += convMs;
		if (lockMs > windowWorstLock) windowWorstLock = lockMs;

		const double elapsedMs = QpcMillisSince(windowStart);
		const long long windowFlips = g_flipCount - windowStartFlips;
		if (elapsedMs >= 5000.0 && windowFlips > 0) {
			const double fps = (double)windowFlips * 1000.0 / elapsedMs;
			const double frameMs = elapsedMs / (double)windowFlips;
			// Our cost per GAME frame: total capture time spread over every
			// flip in the window, so the percentage stays comparable whether
			// we captured all of them or one in eight.
			const double ourMs = (windowLockMs + windowConvMs) / (double)windowFlips;
			DebugLogger::LogFormat(
				"PERF: game %.1f fps (%.1f ms/frame) | our capture %.2f ms/frame (%.0f%% of it) "
				"| lock avg %.2f worst %.2f | convert avg %.2f | %dx%d | captured %lld/%lld flips (1-in-%d)",
				fps, frameMs, ourMs, (ourMs / frameMs) * 100.0,
				windowLockMs / (double)windowCaptures, windowWorstLock,
				windowConvMs / (double)windowCaptures, width, height,
				(long long)windowCaptures, windowFlips, g_captureSkipN);
			windowStart = QpcNow();
			windowStartFlips = g_flipCount;
			windowCaptures = 0;
			windowLockMs = 0.0;
			windowConvMs = 0.0;
			windowWorstLock = 0.0;
		}
	}

	if (logThisAttempt) {
		double nonZeroPct = countedNonZero
			? (double)nonZeroPixelCount / (double)((size_t)width * (size_t)height) * 100.0
			: -1.0;
		DebugLogger::LogFormat(
			"Capture attempt #%d: SUCCESS. %.1f%% of pixels non-zero (successCount=%d) lock=%.2fms convert=%.2fms total=%.2fms path=%s",
			g_captureAttemptCount, nonZeroPct, g_captureSuccessCount, lockMs, convMs, lockMs + convMs,
			(bytesPerPixel == 2) ? "LUT16" : "generic");
	}
}


// ===========================================================================
// PC-port menus: capture the primary when the game draws on it directly.
// See the block comment at g_primarySurface for why this exists.
// ===========================================================================
static long long NowMs() {
	return (long long)(QpcNow().QuadPart * 1000 / QpcFreq().QuadPart);
}

// Serialised, flagged as our own (so the Unlock hook ignores our Unlock), and
// timestamped. Every capture of the primary goes through here or through the
// after-Flip path, which takes the same mutex.
static void CapturePrimaryOffFlip(int mode) {
	IDirectDrawSurface7* prim = g_primarySurface;
	if (!prim) return;
	std::unique_lock<std::mutex> lk(g_primaryCapMutex, std::defer_lock);
	if (mode == 2) { if (!lk.try_lock()) return; }
	else lk.lock();
	t_inOwnCapture = true;
	const uint64_t before = GetDdrawFrameSequence();
	CaptureFlippedSurfaceToSharedBuffer(prim, mode);
	t_inOwnCapture = false;
	if (GetDdrawFrameSequence() != before) {
		g_offFlipDirty = false;
		g_lastCaptureMs = NowMs();
		g_offFlipCaptures++;
		if (g_offFlipCaptures <= 3 || (g_offFlipCaptures % 500) == 0) {
			DebugLogger::LogFormat("Menu capture #%lld: primary captured outside a present (%s)",
				g_offFlipCaptures, mode == 2 ? "idle flush" : "game thread");
		}
	}
}

// Something was drawn onto the primary outside a present. `what` is for the
// log only; `caller` is the game's return address, so the first few lines say
// WHICH menu routine did it.
static void NotePrimaryDrawnOffFlip(const char* what, void* caller) {
	if (!g_menuCaptureEnabled) return;
	const long long now = NowMs();
	const bool wasActive = (now - g_lastOffFlipDrawMs.load() < g_menuHoldMs) ||
		(g_lastOffFlipDrawMs.load() > g_lastPresentMs.load());
	g_lastOffFlipDrawMs = now;
	g_offFlipDirty = true;

	static int logged = 0;
	if (!wasActive || logged < 6) {
		if (logged < 40) {
			logged++;
			DebugLogger::LogFormat("Menu overlay: %s on the PRIMARY outside a present, caller mgsi.exe+%X%s",
				what, (unsigned)((uintptr_t)caller - (uintptr_t)GetModuleHandleA(nullptr)),
				wasActive ? "" : " -- menu overlay now ACTIVE (headset shows the primary as drawn)");
		}
	}

	// Only capture here when the game is NOT presenting (a PC menu loop). While
	// it is presenting, the capture happens just before the next present
	// instead, which sees the whole composite rather than half a redraw.
	if (now - g_lastPresentMs.load() > 100 && now - g_lastCaptureMs.load() >= g_menuMinIntervalMs) {
		CapturePrimaryOffFlip(1);
	}
}

// Menus redraw in bursts (header, then each line), and a burst can end inside
// the min interval above. This picks up the last one once drawing goes quiet.
// Never blocks, never touches the surface while the game holds a DC on it.
static DWORD WINAPI MenuFlushWorker(LPVOID) {
	for (;;) {
		Sleep(25);
		if (!g_menuCaptureEnabled || !g_offFlipDirty.load()) continue;
		const long long now = NowMs();
		if (now - g_lastOffFlipDrawMs.load() < 50) continue;       // still drawing
		if (now - g_lastPresentMs.load() < 150) continue;          // presenting: handled in-line
		if (g_dcOutstanding.load() > 0) continue;
		CapturePrimaryOffFlip(2);
	}
}

// ===========================================================================
// Stall watchdog (round 6). The game hung twice in testing; the log showed the
// present stream simply stopping. This thread notices when no present has
// happened for stall_watchdog_ms (default 2000) while no PC menu is up, and
// logs WHY it might be: whether the game window still has focus (the PC port
// pauses when it loses focus -- e.g. the Virtual Desktop streamer or another
// window grabbing it), and WHERE the game thread is sitting (EIP + a short
// EBP-chain backtrace, resolved to module+offset). The thread is suspended
// only for the few microseconds it takes to copy its registers/stack; no
// logging or allocation happens while it is suspended.
// ===========================================================================
static std::atomic<DWORD> g_gameThreadId{ 0 };

static void DescribeAddress(uintptr_t a, char* out, size_t n) {
	HMODULE m = nullptr;
	if (a && GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		reinterpret_cast<LPCSTR>(a), &m) && m) {
		char path[MAX_PATH] = {};
		GetModuleFileNameA(m, path, MAX_PATH);
		const char* base = strrchr(path, '\\');
		base = base ? base + 1 : path;
		_snprintf_s(out, n, _TRUNCATE, "%s+%X", base, (unsigned)(a - (uintptr_t)m));
	}
	else {
		_snprintf_s(out, n, _TRUNCATE, "%p", (void*)a);
	}
}

struct FindGameWnd { DWORD pid; HWND best; };
static BOOL CALLBACK EnumGameWindows(HWND h, LPARAM lp) {
	FindGameWnd* f = reinterpret_cast<FindGameWnd*>(lp);
	DWORD pid = 0;
	GetWindowThreadProcessId(h, &pid);
	if (pid == f->pid && IsWindowVisible(h) && !GetWindow(h, GW_OWNER)) { f->best = h; return FALSE; }
	return TRUE;
}

static void LogStallSnapshot(long long stalledMs) {
	// --- focus -----------------------------------------------------------
	HWND fg = GetForegroundWindow();
	DWORD fgPid = 0;
	if (fg) GetWindowThreadProcessId(fg, &fgPid);
	char fgTitle[128] = {};
	if (fg) GetWindowTextA(fg, fgTitle, sizeof(fgTitle));
	char fgExe[MAX_PATH] = "?";
	if (fgPid) {
		HANDLE hp = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, fgPid);
		if (hp) {
			DWORD sz = MAX_PATH;
			char full[MAX_PATH] = {};
			if (QueryFullProcessImageNameA(hp, 0, full, &sz)) {
				const char* b = strrchr(full, '\\');
				strncpy_s(fgExe, b ? b + 1 : full, _TRUNCATE);
			}
			CloseHandle(hp);
		}
	}
	FindGameWnd f{ GetCurrentProcessId(), nullptr };
	EnumWindows(EnumGameWindows, reinterpret_cast<LPARAM>(&f));
	const bool gameFg = fgPid == GetCurrentProcessId();
	DebugLogger::LogFormat("STALL: no game present for %lld ms (menu overlay not active). Foreground window: \"%s\" (%s, pid %lu)%s | game window %p iconic=%d",
		stalledMs, fgTitle, fgExe, (unsigned long)fgPid,
		gameFg ? " = THE GAME" : " = NOT the game (the PC port pauses when it loses focus -- click back into the game / keep the VD streamer from taking focus)",
		(void*)f.best, f.best ? (IsIconic(f.best) ? 1 : 0) : -1);

	// --- where is the game thread? -------------------------------------------
	const DWORD tid = g_gameThreadId.load();
	if (!tid) return;
	HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
	if (!th) { DebugLogger::LogFormat("STALL: OpenThread(%lu) failed (%lu)", (unsigned long)tid, GetLastError()); return; }
	uintptr_t pcs[10] = {};
	int npc = 0;
	bool gotCtx = false;
	if (SuspendThread(th) != (DWORD)-1) {
		CONTEXT ctx{};
		ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
		if (GetThreadContext(th, &ctx)) {
			gotCtx = true;
#if defined(_M_IX86)
			uintptr_t pc = ctx.Eip, fp = ctx.Ebp;
#else
			uintptr_t pc = (uintptr_t)ctx.Rip, fp = (uintptr_t)ctx.Rbp;
#endif
			pcs[npc++] = pc;
			// EBP chain: [fp] = previous fp, [fp+ptr] = return address. Reads go
			// through ReadProcessMemory so a bad pointer cannot fault us.
			for (int i = 0; i < 9 && fp; ++i) {
				uintptr_t frame[2] = {};
				SIZE_T got = 0;
				if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(fp), frame, sizeof(frame), &got) || got != sizeof(frame)) break;
				if (!frame[1]) break;
				pcs[npc++] = frame[1];
				if (frame[0] <= fp) break;   // stack grows down; frames must go up
				fp = frame[0];
			}
		}
		ResumeThread(th);
	}
	CloseHandle(th);
	if (!gotCtx) { DebugLogger::Log("STALL: could not read the game thread's context"); return; }
	std::string line = "STALL: game thread ";
	line += std::to_string((unsigned long)tid);
	line += " is at";
	for (int i = 0; i < npc; ++i) {
		char d[96];
		DescribeAddress(pcs[i], d, sizeof(d));
		line += i == 0 ? " " : " <- ";
		line += d;
	}
	DebugLogger::Log(line.c_str());
}

static DWORD WINAPI StallWatchdog(LPVOID) {
	bool stalled = false;
	long long stallStart = 0, nextLog = 0;
	int snapshots = 0;
	for (;;) {
		Sleep(250);
		if (g_stallWatchdogMs <= 0) continue;
		const long long last = g_lastPresentMs.load();
		if (last == 0) continue;                                  // game not presenting yet
		const long long now = NowMs();
		const long long gap = now - last;
		if (!stalled) {
			if (gap >= g_stallWatchdogMs && !IsDdrawMenuOverlayActive()) {
				stalled = true;
				stallStart = last;
				nextLog = now;
			}
		}
		else if (gap < g_stallWatchdogMs) {
			DebugLogger::LogFormat("STALL over: presents resumed after %lld ms", last - stallStart);
			stalled = false;
			continue;
		}
		if (stalled && now >= nextLog && snapshots < 40) {
			snapshots++;
			LogStallSnapshot(gap);
			nextLog = now + 10000;   // then every 10 s while it lasts
		}
	}
}

static void EnsureStallWatchdog() {
	{
		static std::atomic<bool> watchdogStarted{ false };
		if (watchdogStarted.load(std::memory_order_relaxed)) return;
		bool exp = false;
		if (watchdogStarted.compare_exchange_strong(exp, true)) {
			HANDLE hw = CreateThread(nullptr, 0, StallWatchdog, nullptr, 0, nullptr);
			if (hw) CloseHandle(hw);
			DebugLogger::LogFormat("Stall watchdog: %s (stall_watchdog_ms=%d) -- logs focus + game-thread location if presents stop",
				g_stallWatchdogMs > 0 ? "ON" : "OFF", g_stallWatchdogMs);
		}
	}
}

static void EnsureMenuFlushWorker() {
	bool expected = false;
	if (!g_menuWorkerStarted.compare_exchange_strong(expected, true)) return;
	HANDLE h = CreateThread(nullptr, 0, MenuFlushWorker, nullptr, 0, nullptr);
	if (h) CloseHandle(h);
}

bool IsDdrawMenuOverlayActive() {
	if (!g_menuCaptureEnabled) return false;
	const long long last = g_lastOffFlipDrawMs.load();
	return (last > g_lastPresentMs.load()) || (NowMs() - last < g_menuHoldMs);
}

// Called by both presents (Flip and a Blt onto the primary) BEFORE the real
// call: if a menu has been drawn onto the front buffer since our last capture,
// take it now -- after the present it is gone.
static void BeforePresent(IDirectDrawSurface7* prim) {
	if (g_menuCaptureEnabled && prim && prim == g_primarySurface && g_offFlipDirty.load()) {
		CapturePrimaryOffFlip(1);
	}
}

static HRESULT WINAPI HookedDDSurface7GetDC(IDirectDrawSurface7* self, HDC* outDc) {
	HRESULT hr = g_OriginalDDS7GetDC ? g_OriginalDDS7GetDC(self, outDc) : DDERR_GENERIC;
	if (SUCCEEDED(hr) && self == g_primarySurface) g_dcOutstanding++;
	return hr;
}

static HRESULT WINAPI HookedDDSurface7ReleaseDC(IDirectDrawSurface7* self, HDC dc) {
	HRESULT hr = g_OriginalDDS7ReleaseDC ? g_OriginalDDS7ReleaseDC(self, dc) : DDERR_GENERIC;
	if (self == g_primarySurface && g_primarySurface) {
		if (g_dcOutstanding.load() > 0) g_dcOutstanding--;
		if (SUCCEEDED(hr)) NotePrimaryDrawnOffFlip("GDI draw (ReleaseDC)", _ReturnAddress());
	}
	return hr;
}

static HRESULT WINAPI HookedDDSurface7Unlock(IDirectDrawSurface7* self, LPRECT rect) {
	HRESULT hr = g_OriginalDDS7Unlock ? g_OriginalDDS7Unlock(self, rect) : DDERR_GENERIC;
	// Only while the game is NOT presenting. Every PC menu draw found in the
	// binary is GDI (GetDC/ReleaseDC on the primary); a raw Lock/Unlock of the
	// primary while frames are still flipping is something else, and must not
	// be able to switch gameplay onto the menu screen.
	if (!t_inOwnCapture && SUCCEEDED(hr) && self == g_primarySurface && g_primarySurface &&
		NowMs() - g_lastPresentMs.load() > 150) {
		NotePrimaryDrawnOffFlip("direct pixel write (Unlock)", _ReturnAddress());
	}
	return hr;
}

bool GetLatestDdrawFrameRgba(std::vector<uint8_t>& outRgba, int32_t& outWidth, int32_t& outHeight, int* outEye,
                             FrameViewRecord* outViewRec) {
	std::lock_guard<std::mutex> lock(g_captureMutex);
	// RENDER TWICE: a published stereo pair is handed out ONE EYE PER CALL,
	// left then right, and becomes "the latest frame" as it goes. The consumer
	// calls again while GetPendingDdrawFrameCount() > 0, so both eyes of a
	// pair land in the same XR frame.
	if (g_rtQueueCount > 0) {
		RtQueuedFrame& q = g_rtQueue[g_rtQueueHead];
		const int32_t w = q.img.width, h = q.img.height;
		if (!q.img.raw16.empty() && g_rgb16Lut.built) {
			// 16-bit -> RGBA here, on the XR thread, not on the game thread:
			// the game thread only copied 600 KB while it held the lock.
			const size_t n = (size_t)w * (size_t)h;
			if (g_capturedRgba.size() != n * 4u) g_capturedRgba.resize(n * 4u);
			const uint32_t* lut = g_rgb16Lut.table.data();
			uint32_t* dst = reinterpret_cast<uint32_t*>(g_capturedRgba.data());
			const uint16_t* src = q.img.raw16.data();
			for (size_t i = 0; i < n; ++i) dst[i] = lut[src[i]];
		}
		else {
			g_capturedRgba.swap(q.img.rgba);
			if (q.img.bgra) {
				// GPU eye capture hands over B,G,R,x: swizzle here, on the XR
				// thread, so the game thread never touches a pixel.
				const size_t n = (size_t)w * (size_t)h;
				uint8_t* p = g_capturedRgba.data();
				if (g_capturedRgba.size() >= n * 4u) {
					for (size_t i = 0; i < n; ++i, p += 4) { const uint8_t b = p[0]; p[0] = p[2]; p[2] = b; p[3] = 255; }
				}
			}
		}
		g_capturedWidth = w;
		g_capturedHeight = h;
		g_capturedHudDone = q.img.hudDone;
		q.img.hudDone = false;
		g_capturedEye = q.eye;
		g_capturedViewRec = q.rec;
		g_hasCapturedFrame = true;
		g_rtQueueHead++;
		g_rtQueueCount--;
	}
	if (!g_hasCapturedFrame) {
		return false;
	}
	outRgba = g_capturedRgba;
	g_lastHandedHudDone = g_capturedHudDone;
	outWidth = g_capturedWidth;
	outHeight = g_capturedHeight;
	if (outEye) *outEye = g_capturedEye;
	if (outViewRec) *outViewRec = g_capturedViewRec;
	return true;
}

bool LastDdrawFrameHudDone() {
	std::lock_guard<std::mutex> lock(g_captureMutex);
	return g_lastHandedHudDone;
}

uint64_t GetDdrawFrameSequence() {
	std::lock_guard<std::mutex> lock(g_captureMutex);
	return g_captureSequence;
}

int GetPendingDdrawFrameCount() {
	std::lock_guard<std::mutex> lock(g_captureMutex);
	return g_rtQueueCount;
}

// ===========================================================================
// RENDER TWICE support (2026-09-24). See render_twice.cpp.
// ===========================================================================

// Reads a surface the game has just drawn into (the software/hardware BACK
// buffer, not the primary) into tightly packed RGBA8. Always a blocking lock:
// the next DrawOTag overwrites these pixels, so there is no "try next flip".
bool CaptureSurfaceRgbaBlocking(void* surfaceVoid, std::vector<uint8_t>& out, int32_t& outW, int32_t& outH,
                                double* outLockMs, double* outConvMs) {
	IDirectDrawSurface7* surface = reinterpret_cast<IDirectDrawSurface7*>(surfaceVoid);
	if (!surface) return false;

	DDSURFACEDESC2 desc{};
	desc.dwSize = sizeof(desc);
	const LARGE_INTEGER lockStart = QpcNow();
	HRESULT hr = surface->Lock(nullptr, &desc, DDLOCK_WAIT | DDLOCK_READONLY | DDLOCK_NOSYSLOCK, nullptr);
	const double lockMs = QpcMillisSince(lockStart);
	if (outLockMs) *outLockMs = lockMs;
	if (FAILED(hr)) {
		static int failLog = 0;
		if (failLog < 10) {
			failLog++;
			DebugLogger::LogFormat("Render twice: back-buffer Lock failed 0x%08X", (unsigned)hr);
		}
		return false;
	}

	const DDPIXELFORMAT& pf = desc.ddpfPixelFormat;
	const int32_t width = static_cast<int32_t>(desc.dwWidth);
	const int32_t height = static_cast<int32_t>(desc.dwHeight);
	const LONG pitch = desc.lPitch;
	const uint32_t bpp = pf.dwRGBBitCount;
	const bool supported = (pf.dwFlags & DDPF_RGB) != 0 && (bpp == 16 || bpp == 24 || bpp == 32) &&
		width > 0 && height > 0 && desc.lpSurface != nullptr;
	if (!supported) {
		surface->Unlock(nullptr);
		static bool logged = false;
		if (!logged) {
			logged = true;
			DebugLogger::LogFormat("Render twice: back buffer format unsupported (flags=0x%08X bpp=%u %dx%d)",
				(unsigned)pf.dwFlags, (unsigned)bpp, width, height);
		}
		return false;
	}

	unsigned long rShift, gShift, bShift;
	uint32_t rMax, gMax, bMax;
	MaskToShiftAndMax(pf.dwRBitMask, rShift, rMax);
	MaskToShiftAndMax(pf.dwGBitMask, gShift, gMax);
	MaskToShiftAndMax(pf.dwBBitMask, bShift, bMax);

	const size_t needBytes = static_cast<size_t>(width) * static_cast<size_t>(height) * 4u;
	if (out.size() != needBytes) out.resize(needBytes);
	const uint8_t* srcBase = reinterpret_cast<const uint8_t*>(desc.lpSurface);

	const LARGE_INTEGER convStart = QpcNow();
	if (bpp == 16) {
		EnsureRgb16Lut(pf.dwRBitMask, pf.dwGBitMask, pf.dwBBitMask, rShift, rMax, gShift, gMax, bShift, bMax);
		const uint32_t* lut = g_rgb16Lut.table.data();
		for (int32_t y = 0; y < height; ++y) {
			const uint16_t* srcRow = reinterpret_cast<const uint16_t*>(srcBase + static_cast<size_t>(y) * static_cast<size_t>(pitch));
			uint32_t* dstRow = reinterpret_cast<uint32_t*>(out.data() + static_cast<size_t>(y) * static_cast<size_t>(width) * 4u);
			for (int32_t x = 0; x < width; ++x) dstRow[x] = lut[srcRow[x]];
		}
	}
	else {
		const uint32_t bytesPerPixel = bpp / 8u;
		for (int32_t y = 0; y < height; ++y) {
			const uint8_t* srcRow = srcBase + static_cast<size_t>(y) * static_cast<size_t>(pitch);
			uint8_t* dstRow = out.data() + static_cast<size_t>(y) * static_cast<size_t>(width) * 4u;
			for (int32_t x = 0; x < width; ++x) {
				const uint8_t* sp = srcRow + static_cast<size_t>(x) * bytesPerPixel;
				const uint32_t pixel = (bytesPerPixel == 4) ? *reinterpret_cast<const uint32_t*>(sp)
				                                            : (uint32_t)(sp[0] | (sp[1] << 8) | (sp[2] << 16));
				uint8_t* dp = dstRow + static_cast<size_t>(x) * 4u;
				dp[0] = ExtractChannel8(pixel, pf.dwRBitMask, rShift, rMax);
				dp[1] = ExtractChannel8(pixel, pf.dwGBitMask, gShift, gMax);
				dp[2] = ExtractChannel8(pixel, pf.dwBBitMask, bShift, bMax);
				dp[3] = 255;
			}
		}
	}
	if (outConvMs) *outConvMs = QpcMillisSince(convStart);
	surface->Unlock(nullptr);
	outW = width;
	outH = height;
	return true;
}

// RENDER TWICE, second design (2026-09-24 evening). The first build read the
// BACK buffer with Lock() straight after DrawOTag and got black -- and the
// monitor went black too. GOG's DirectDraw->D3D9 wrapper keeps the 3D image
// in its own render target and only resolves it at Flip; a Lock on the back
// buffer before that sees a stale system-memory copy, and the Unlock then
// pushes that copy back over the real picture. The one read-back this
// project has ever had working is the primary right after a Flip, so this
// does exactly that: present the eye with the ORIGINAL Flip (none of our
// per-flip bookkeeping, no cutscene tick, no frame count), then lock the
// primary. Hardware flip path only; the caller checks the mode.
// Lock + tight copy of a 16-bit surface (no per-pixel work on the game
// thread). Falls back to the full RGBA conversion for any other format.
static bool CaptureSurfaceForPair(IDirectDrawSurface7* surface, RtEyeImage& out, double* outLockMs, double* outCopyMs) {
	DDSURFACEDESC2 desc{};
	desc.dwSize = sizeof(desc);
	const LARGE_INTEGER lockStart = QpcNow();
	HRESULT hr = surface->Lock(nullptr, &desc, DDLOCK_WAIT | DDLOCK_READONLY | DDLOCK_NOSYSLOCK, nullptr);
	if (outLockMs) *outLockMs = QpcMillisSince(lockStart);
	if (FAILED(hr)) return false;
	const DDPIXELFORMAT& pf = desc.ddpfPixelFormat;
	const int32_t w = (int32_t)desc.dwWidth, h = (int32_t)desc.dwHeight;
	if (pf.dwRGBBitCount != 16 || !(pf.dwFlags & DDPF_RGB) || !desc.lpSurface || w <= 0 || h <= 0) {
		surface->Unlock(nullptr);
		out.raw16.clear();
		out.bgra = false;
		return CaptureSurfaceRgbaBlocking(surface, out.rgba, out.width, out.height, outLockMs, outCopyMs);
	}
	unsigned long rS, gS, bS; uint32_t rM, gM, bM;
	MaskToShiftAndMax(pf.dwRBitMask, rS, rM);
	MaskToShiftAndMax(pf.dwGBitMask, gS, gM);
	MaskToShiftAndMax(pf.dwBBitMask, bS, bM);
	// Built on the game thread (once), read-only everywhere after.
	EnsureRgb16Lut(pf.dwRBitMask, pf.dwGBitMask, pf.dwBBitMask, rS, rM, gS, gM, bS, bM);
	const LARGE_INTEGER copyStart = QpcNow();
	const size_t n = (size_t)w * (size_t)h;
	if (out.raw16.size() != n) out.raw16.resize(n);
	const uint8_t* src = reinterpret_cast<const uint8_t*>(desc.lpSurface);
	for (int32_t y = 0; y < h; ++y) {
		memcpy(out.raw16.data() + (size_t)y * (size_t)w, src + (size_t)y * (size_t)desc.lPitch, (size_t)w * 2u);
	}
	if (outCopyMs) *outCopyMs = QpcMillisSince(copyStart);
	surface->Unlock(nullptr);
	out.width = w;
	out.height = h;
	out.bgra = false;
	return true;
}

bool RtFlipAndCapturePrimary(RtEyeImage& out, double* outFlipMs, double* outLockMs, double* outCopyMs) {
	IDirectDrawSurface7* prim = g_primarySurface;
	if (!prim || !g_OriginalDDS7Flip) return false;
	const LARGE_INTEGER flipStart = QpcNow();
	const HRESULT hr = g_OriginalDDS7Flip(prim, nullptr, DDFLIP_WAIT);
	if (outFlipMs) *outFlipMs = QpcMillisSince(flipStart);
	if (FAILED(hr)) {
		static int failLog = 0;
		if (failLog < 10) { failLog++; DebugLogger::LogFormat("Render twice: our Flip failed 0x%08X", (unsigned)hr); }
		return false;
	}
	g_lastPresentMs = NowMs();
	std::lock_guard<std::mutex> capLock(g_primaryCapMutex);
	t_inOwnCapture = true;
	const bool ok = CaptureSurfaceForPair(prim, out, outLockMs, outCopyMs);
	t_inOwnCapture = false;
	return ok;
}

// What the game's own present does to the new back buffer right after its
// Flip (+4226B4): a black colour fill. Needed after our extra Flip, because
// the back buffer now holds an older frame and DrawOTag does not clear it.
void RtClearSurfaceBlack(void* surfaceVoid) {
	IDirectDrawSurface7* s = reinterpret_cast<IDirectDrawSurface7*>(surfaceVoid);
	if (!s) return;
	DDBLTFX fx{};
	fx.dwSize = sizeof(fx);
	fx.dwFillColor = 0;
	s->Blt(nullptr, nullptr, nullptr, DDBLT_COLORFILL | DDBLT_WAIT, &fx);
}

// ---- PAIR CONVERTER THREAD (2026-09-26) -------------------------------------
// At 960p each eye is 1280x960: turning it into RGBA (the 16-bit LUT for the
// flip path, the BGRA swizzle for the GPU path) is ~1.2 million pixels per eye.
// Done on the XR thread -- as it was -- both eyes land in ONE compositor frame
// and blow its 11-14 ms budget every game frame: the headset drops a frame
// three times a second and the view seems to be forever catching up. So a
// published pair goes to this thread first; the XR thread only ever sees
// finished RGBA and does nothing but copy and upload.
static std::mutex g_rtConvMutex;
static std::condition_variable g_rtConvCv;
static RtEyeImage g_rtStage[2];
static FrameViewRecord g_rtStageRec;
static bool g_rtStageReady = false;
static bool g_rtStageMono = false;     // a single GPU-captured frame, not a pair
static int  g_rtStageEye = -1;         // its eye tag (mono frames only)
static std::once_flag g_rtConvOnce;

// Wrist HUD (vr_aim.cpp): crops the item/weapon/radar boxes for the wrists and
// hides them in the picture. Per-pixel work at 1280x960 -- so it runs HERE, on
// the converter thread, not on the XR thread (2026-09-27 log: the XR thread
// spent 50-60 ms per compositor frame taking a 960p image in, i.e. 13 Hz).
void WristHudOnNewFrame(uint8_t* rgba, int width, int height);

// ---- F9 SNAPSHOT (2026-09-27, red-shading investigation) --------------------
// Press F9 on the PC keyboard: the next frame the headset receives is saved as
// BMPs in <game folder>\VR_SNAPSHOTS, at three points of the pipeline:
//   snap_NNN_<eye>_1_captured.bmp  straight off the GPU / primary copy
//   snap_NNN_<eye>_2_final.bmp     after our own processing (wrist-HUD masks)
//   snap_NNN_monitor.bmp           the game window as it is on the desktop
// Comparing them says in one look which stage adds a colour the game did not
// draw: 1 vs monitor = the copy, 2 vs 1 = our processing, both clean while the
// headset is not = the compositor side (vr_injection / runtime).
static std::atomic<int> g_snapPending{ 0 };   // frames still to save for this press
static int  g_snapIndex = 0;
static bool g_snapKeyDown = false;
static char g_snapDir[MAX_PATH] = "";

static const char* SnapDir() {
	if (!g_snapDir[0]) {
		char exe[MAX_PATH] = {};
		GetModuleFileNameA(NULL, exe, MAX_PATH);
		char* slash = strrchr(exe, '\\');
		if (slash) *slash = 0;
		_snprintf_s(g_snapDir, sizeof(g_snapDir), _TRUNCATE, "%s\\VR_SNAPSHOTS", exe);
		CreateDirectoryA(g_snapDir, nullptr);
	}
	return g_snapDir;
}

// Writes a top-down 32-bit BMP. src is RGBA unless srcIsBgra.
static bool SnapWriteBmp(const char* path, const uint8_t* src, int w, int h, int srcPitch, bool srcIsBgra) {
	if (!src || w <= 0 || h <= 0) return false;
	FILE* f = nullptr;
	if (fopen_s(&f, path, "wb") != 0 || !f) return false;
	BITMAPFILEHEADER fh{};
	BITMAPINFOHEADER ih{};
	const DWORD imgBytes = (DWORD)w * (DWORD)h * 4u;
	fh.bfType = 0x4D42;
	fh.bfOffBits = sizeof(fh) + sizeof(ih);
	fh.bfSize = fh.bfOffBits + imgBytes;
	ih.biSize = sizeof(ih);
	ih.biWidth = w;
	ih.biHeight = -h;            // top-down
	ih.biPlanes = 1;
	ih.biBitCount = 32;
	ih.biCompression = BI_RGB;
	ih.biSizeImage = imgBytes;
	fwrite(&fh, sizeof(fh), 1, f);
	fwrite(&ih, sizeof(ih), 1, f);
	std::vector<uint8_t> row((size_t)w * 4u);
	for (int y = 0; y < h; ++y) {
		const uint8_t* s = src + (size_t)y * (size_t)srcPitch;
		for (int x = 0; x < w; ++x) {
			const uint8_t* p = s + (size_t)x * 4u;
			uint8_t* o = row.data() + (size_t)x * 4u;
			if (srcIsBgra) { o[0] = p[0]; o[1] = p[1]; o[2] = p[2]; }
			else           { o[0] = p[2]; o[1] = p[1]; o[2] = p[0]; }
			o[3] = 255;
		}
		fwrite(row.data(), 1, row.size(), f);
	}
	fclose(f);
	return true;
}

// The game window as the desktop shows it (composited screen pixels).
static void SnapGameWindow(const char* path) {
	FindGameWnd fw{ GetCurrentProcessId(), nullptr };
	EnumWindows(EnumGameWindows, reinterpret_cast<LPARAM>(&fw));
	if (!fw.best) { DebugLogger::Log("Snapshot: game window not found -- no monitor image"); return; }
	RECT rc{};
	GetClientRect(fw.best, &rc);
	POINT tl{ 0, 0 };
	ClientToScreen(fw.best, &tl);
	const int w = rc.right - rc.left, h = rc.bottom - rc.top;
	if (w <= 0 || h <= 0) { DebugLogger::Log("Snapshot: game window has no client area (minimised?) -- no monitor image"); return; }
	HDC scr = GetDC(nullptr);
	HDC mem = CreateCompatibleDC(scr);
	BITMAPINFO bi{};
	bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
	bi.bmiHeader.biWidth = w;
	bi.bmiHeader.biHeight = -h;
	bi.bmiHeader.biPlanes = 1;
	bi.bmiHeader.biBitCount = 32;
	bi.bmiHeader.biCompression = BI_RGB;
	void* bits = nullptr;
	HBITMAP bmp = CreateDIBSection(scr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
	if (bmp && bits) {
		HGDIOBJ old = SelectObject(mem, bmp);
		BitBlt(mem, 0, 0, w, h, scr, tl.x, tl.y, SRCCOPY | CAPTUREBLT);
		GdiFlush();
		SnapWriteBmp(path, static_cast<const uint8_t*>(bits), w, h, w * 4, true);
		SelectObject(mem, old);
		DebugLogger::LogFormat("Snapshot: monitor image %dx%d -> %s%s", w, h, path,
			GetForegroundWindow() == fw.best ? "" : " (game window was NOT in front -- anything covering it is in the picture)");
	}
	if (bmp) DeleteObject(bmp);
	DeleteDC(mem);
	ReleaseDC(nullptr, scr);
}

// Polled from the converter thread (it wakes for every frame the game hands
// over, ~30 times a second). Edge-triggered.
static void SnapPollKey() {
	const bool down = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
	if (down && !g_snapKeyDown && g_snapPending.load() == 0) {
		g_snapIndex++;
		g_snapPending.store(1);
		DebugLogger::LogFormat("Snapshot #%d requested (F9) -- saving the next frame into %s", g_snapIndex, SnapDir());
	}
	g_snapKeyDown = down;
}

static void SnapEyePath(char* out, size_t n, const char* eyeName, const char* stage) {
	_snprintf_s(out, n, _TRUNCATE, "%s\\snap_%03d_%s_%s.bmp", SnapDir(), g_snapIndex, eyeName, stage);
}

static void ConvertEyeToRgba(RtEyeImage& im, const char* snapCapturedPath = nullptr) {
	const size_t n = (size_t)(im.width > 0 ? im.width : 0) * (size_t)(im.height > 0 ? im.height : 0);
	if (n == 0) return;
	if (!im.raw16.empty()) {
		if (!g_rgb16Lut.built || im.raw16.size() < n) { im.rgba.clear(); return; }
		if (im.rgba.size() != n * 4u) im.rgba.resize(n * 4u);
		const uint32_t* lut = g_rgb16Lut.table.data();
		uint32_t* dst = reinterpret_cast<uint32_t*>(im.rgba.data());
		const uint16_t* src = im.raw16.data();
		for (size_t i = 0; i < n; ++i) dst[i] = lut[src[i]];
		im.raw16.clear();
	}
	else if (im.bgra) {
		if (im.rgba.size() < n * 4u) { im.rgba.clear(); return; }
		uint8_t* p = im.rgba.data();
		for (size_t i = 0; i < n; ++i, p += 4) { const uint8_t t = p[0]; p[0] = p[2]; p[2] = t; p[3] = 255; }
	}
	im.bgra = false;
	const size_t need = (size_t)im.width * (size_t)im.height * 4u;
	if (snapCapturedPath && im.rgba.size() >= need && need)
		SnapWriteBmp(snapCapturedPath, im.rgba.data(), im.width, im.height, im.width * 4, false);
	if (im.rgba.size() >= need && need) { WristHudOnNewFrame(im.rgba.data(), im.width, im.height); im.hudDone = true; }
}

static void RtConverterLoop() {
	RtEyeImage work[2];
	FrameViewRecord rec;
	bool mono = false;
	int monoEye = -1;
	double accMs = 0.0, maxMs = 0.0;
	int pairs = 0;
	long long windowStart = NowMs();
	for (;;) {
		{
			std::unique_lock<std::mutex> lk(g_rtConvMutex);
			g_rtConvCv.wait(lk, [] { return g_rtStageReady; });
			std::swap(work[0], g_rtStage[0]);
			std::swap(work[1], g_rtStage[1]);
			rec = g_rtStageRec;
			mono = g_rtStageMono;
			monoEye = g_rtStageEye;
			g_rtStageReady = false;
		}
		SnapPollKey();
		const bool snapNow = g_snapPending.load() > 0;
		char snapA[MAX_PATH] = {}, snapB[MAX_PATH] = {};
		if (snapNow) {
			g_snapPending.store(0);
			char mon[MAX_PATH] = {};
			_snprintf_s(mon, sizeof(mon), _TRUNCATE, "%s\\snap_%03d_monitor.bmp", SnapDir(), g_snapIndex);
			SnapGameWindow(mon);   // first, so it is as close in time as possible
			DebugLogger::LogFormat("Snapshot #%d: %s frame %dx%d | view mode %s | first person %d | render twice pair %d",
				g_snapIndex, mono ? "single" : "stereo-pair", (int)work[0].width, (int)work[0].height,
				IsVrViewModeActive() ? "VR" : "NATIVE", IsFpvActive() ? 1 : 0, mono ? 0 : 1);
		}
		if (mono) {
			// One GPU-captured game frame (alternate-eye stereo, third person,
			// cutscenes...): becomes "the latest frame", exactly as the
			// after-Flip capture would have published it.
			if (snapNow) SnapEyePath(snapA, sizeof(snapA), "single", "1_captured");
			ConvertEyeToRgba(work[0], snapNow ? snapA : nullptr);
			const size_t n = (size_t)work[0].width * (size_t)work[0].height * 4u;
			if (n == 0 || work[0].rgba.size() < n) continue;
			if (snapNow) {
				SnapEyePath(snapB, sizeof(snapB), "single", "2_final");
				SnapWriteBmp(snapB, work[0].rgba.data(), work[0].width, work[0].height, work[0].width * 4, false);
				DebugLogger::LogFormat("Snapshot #%d saved (single frame, eye tag %d)", g_snapIndex, monoEye);
			}
			std::lock_guard<std::mutex> lock(g_captureMutex);
			g_capturedRgba.swap(work[0].rgba);
			g_capturedWidth = work[0].width;
			g_capturedHeight = work[0].height;
			g_capturedEye = monoEye;
			g_capturedViewRec = rec;
			g_capturedHudDone = work[0].hudDone;
			work[0].hudDone = false;
			g_hasCapturedFrame = true;
			g_rtQueueCount = 0;   // anything older is stale now
			g_captureSequence++;
			continue;
		}
		const LARGE_INTEGER t0 = QpcNow();
		char snapC[MAX_PATH] = {};
		if (snapNow) {
			SnapEyePath(snapA, sizeof(snapA), "left", "1_captured");
			SnapEyePath(snapC, sizeof(snapC), "right", "1_captured");
		}
		ConvertEyeToRgba(work[0], snapNow ? snapA : nullptr);
		ConvertEyeToRgba(work[1], snapNow ? snapC : nullptr);
		if (snapNow) {
			for (int e = 0; e < 2; ++e) {
				const size_t ne = (size_t)work[e].width * (size_t)work[e].height * 4u;
				if (!ne || work[e].rgba.size() < ne) continue;
				SnapEyePath(snapB, sizeof(snapB), e == 0 ? "left" : "right", "2_final");
				SnapWriteBmp(snapB, work[e].rgba.data(), work[e].width, work[e].height, work[e].width * 4, false);
			}
			DebugLogger::LogFormat("Snapshot #%d saved (stereo pair)", g_snapIndex);
		}
		const double ms = QpcMillisSince(t0);
		const size_t n0 = (size_t)work[0].width * (size_t)work[0].height * 4u;
		if (n0 == 0 || work[0].rgba.size() < n0 || work[1].rgba.size() < n0 ||
			work[0].width != work[1].width || work[0].height != work[1].height) continue;
		const int32_t w = work[0].width, h = work[0].height;
		{
			std::lock_guard<std::mutex> lock(g_captureMutex);
			std::swap(g_rtQueue[0].img, work[0]);
			g_rtQueue[0].eye = 0; g_rtQueue[0].rec = rec;
			std::swap(g_rtQueue[1].img, work[1]);
			g_rtQueue[1].eye = 1; g_rtQueue[1].rec = rec;
			g_rtQueueHead = 0;
			g_rtQueueCount = 2;
			g_captureSequence += 2;
		}
		accMs += ms; if (ms > maxMs) maxMs = ms; pairs++;
		const long long now = NowMs();
		if (now - windowStart >= 5000) {
			DebugLogger::LogFormat("RT CONVERT: %d pairs in 5 s at %dx%d per eye | RGBA conversion (off the XR thread) avg %.2f ms, worst %.2f ms per pair",
				pairs, (int)w, (int)h, pairs ? accMs / pairs : 0.0, maxMs);
			accMs = 0.0; maxMs = 0.0; pairs = 0; windowStart = now;
		}
	}
}

// Hands a finished left/right pair to the headset (via the converter thread).
// Buffers are SWAPPED in, so the caller gets older storage back and nothing
// reallocates in the steady state. The newest pair always wins, and the two
// eyes of a pair are never mixed with another pair's.
static void StartConverterOnce() {
	std::call_once(g_rtConvOnce, [] {
		std::thread(RtConverterLoop).detach();
		DebugLogger::Log("Render twice: pair converter thread started (RGBA conversion off the XR and game threads)");
		DebugLogger::Log("Snapshot: press F9 on the PC keyboard to save the next headset frame (as captured, as processed) plus the monitor image into the game folder's VR_SNAPSHOTS");
	});
}

void PublishGpuMonoFrame(RtEyeImage& img, int eye, const FrameViewRecord& rec) {
	StartConverterOnce();
	{
		std::lock_guard<std::mutex> lk(g_rtConvMutex);
		std::swap(g_rtStage[0], img);
		g_rtStageRec = rec;
		g_rtStageMono = true;
		g_rtStageEye = eye;
		g_rtStageReady = true;
	}
	g_rtConvCv.notify_one();
	// Stands the after-Flip primary read-back down, exactly as a pair does.
	g_rtLastPairMs = NowMs();
}

bool GetDrawnFrameTag(int* eye, FrameViewRecord* rec) {
	// AdvancePresentTag ran at this frame's present, BEFORE its DrawOTag: the
	// tag it computed ("cur") describes the camera the upcoming draw uses. With
	// tag_delay_frames > 0 that is kept in g_prevPresentTag.
	const PresentTag& t = g_tagDelayFrames > 0 ? g_prevPresentTag : g_thisPresentTag;
	if (eye) *eye = t.eye;
	if (rec) *rec = t.rec;
	return true;
}

void PublishRenderTwicePair(RtEyeImage& left, RtEyeImage& right, const FrameViewRecord& rec, bool presentedByUs) {
	StartConverterOnce();
	{
		std::lock_guard<std::mutex> lk(g_rtConvMutex);
		std::swap(g_rtStage[0], left);
		std::swap(g_rtStage[1], right);
		g_rtStageRec = rec;
		g_rtStageMono = false;
		g_rtStageReady = true;
	}
	g_rtConvCv.notify_one();
	g_rtLastPairMs = NowMs();
	if (presentedByUs) g_rtLastOwnPresentMs = NowMs();
}

// Read-only probe of the community table's undescribed PAUSE byte.
//
// Deliberately its own function rather than an inline __try inside the flip
// hook: MSVC refuses structured exception handling in any function that needs
// object unwinding (C2712), and the flip hook is exactly the kind of place
// where a future edit adds a std::string or a lock guard and breaks the build
// in a way that looks unrelated to this. Same shape as SafeRead16 in
// camera_write_hook.cpp, for the same reason.
//
// Returns the byte's value, or -1 if the address could not be read.
static int ReadPauseByteSafe() {
	static uintptr_t moduleBase = 0;
	if (moduleBase == 0) {
		moduleBase = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
	}
	if (moduleBase == 0) return -1;

	constexpr uintptr_t kPauseByteRva = 0x391A0C;
	__try {
		return *reinterpret_cast<volatile unsigned char*>(moduleBase + kPauseByteRva);
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		return -1;
	}
}

static HRESULT WINAPI HookedDDSurface7Flip(IDirectDrawSurface7* self, LPDIRECTDRAWSURFACE7 targetOverride, DWORD flags) {
	if (!g_primarySurface) g_primarySurface = self;
	g_gameThreadId = GetCurrentThreadId();
	EnsureStallWatchdog();
	BeforePresent(self);
	// Render twice has already presented both eyes of the frame on the back
	// buffer's way through; the game's own flip would only show the left eye
	// again. 60 ms = within the last game frame or two, so the moment pairs
	// stop the game's flips come straight back.
	const bool rtOwnsPresent = g_rtSkipGameFlip && (NowMs() - g_rtLastOwnPresentMs.load() < 60);
	HRESULT hr;
	if (rtOwnsPresent) {
		hr = DD_OK;
		if (++g_rtSkippedGameFlips == 1) {
			DebugLogger::Log("Render twice: game's own Flip skipped while pairs are flowing (skip_game_flip=1)");
		}
	}
	else {
		hr = g_OriginalDDS7Flip ? g_OriginalDDS7Flip(self, targetOverride, flags) : DDERR_GENERIC;
	}
	AdvancePresentTag();
	const bool menuOverlayRecent = g_menuCaptureEnabled &&
		(NowMs() - g_lastOffFlipDrawMs.load() < g_menuHoldMs);
	g_lastPresentMs = NowMs();

	// `self` is the primary surface's stable COM identity; DirectDraw swaps
	// the underlying video memory pages under the hood on Flip(), so right
	// after the real Flip() returns, `self` holds the frame that was just
	// presented.
	if (SUCCEEDED(hr)) {
		g_flipCount++;

		// ---- PAUSE-STATE MONITOR (NEW 2026-08-16) --------------------------
		// The community Cheat Engine table has a bare, undescribed `PAUSE`
		// byte at mgsi.exe+391A0C. It has never been tested, and the handoff
		// has been asking for exactly this test for days.
		//
		// Two reasons it earns a per-frame read here, on the flip hook:
		//
		//  1. It TIMESTAMPS the pause. The whole pause-menu investigation has
		//     been hampered by not knowing which Blt calls happened while
		//     paused -- the pause state has only ever been inferred backwards
		//     from `Stereo: eye tag EXPIRED after N ms`, which is a timeout
		//     guess. With a state-change line in the same log, the relevant
		//     Blt calls become findable by timestamp instead of by sifting.
		//  2. It TESTS the byte itself. If this flips cleanly 0 <-> non-zero
		//     exactly when the player pauses, that is a confirmed pause flag
		//     and a far better signal than the timeout. If it never moves, or
		//     moves at unrelated times, the table entry is mislabelled and we
		//     stop treating it as a lead.
		//
		// READ-ONLY, edge-triggered, SEH-guarded. It never writes, and an
		// unreadable address degrades to "unknown" rather than taking the
		// frame hook down with it.
		{
			const int pauseNow = ReadPauseByteSafe();

			static int  lastPause = -2;
			static int  changeLog = 0;
			if (pauseNow != lastPause && changeLog < 60) {
				changeLog++;
				DebugLogger::LogFormat(
					"PAUSE byte (mgsi.exe+391A0C): %d -> %d at flip #%llu. If this tracks the pause "
					"menu opening and closing, it is a real pause flag and the Blt calls logged "
					"around this timestamp are the ones worth reading.",
					lastPause, pauseNow, (unsigned long long)g_flipCount);
				lastPause = pauseNow;
			}
			else if (pauseNow != lastPause) {
				lastPause = pauseNow;   // keep tracking silently once the log budget is spent
			}
		}

		// ---- CUTSCENE VR TICK (NEW 2026-08-22) -----------------------------
		// Runs here, and only here, for one reason: this is the last place in
		// the mod that is guaranteed to still be executing on the game's own
		// thread when the game's camera update has stopped. The 2026-08-22
		// depth probe settled that -- HookedPresent never fires for a whole
		// session while this hook fires continuously.
		//
		// It reads the cutscene/camera-control/modal state bytes, measures
		// whether the camera hooks are still alive, and (only when they are
		// not) writes the camera itself. When the hooks ARE alive it writes
		// nothing at all, so there is never more than one writer per value per
		// frame. Cheap and inert with cutscene_detect_mode=0, which is the
		// shipped default -- it still does the reading and logging, because
		// that is the measurement the feature is waiting on.
		//
		// Placed AFTER the capture below would be wrong: the camera value this
		// may write is for the NEXT frame the game renders, and putting it
		// before the capture keeps the ordering easy to reason about.
		CutsceneVrFlipTick((unsigned long long)g_flipCount);

		// Adaptive throttle. When Lock() has been expensive we deliberately let
		// flips go by uncaptured, so the game's CPU thread isn't forced to wait
		// on the GPU every single frame. The headset keeps displaying the last
		// captured frame in the meantime.
		//
		// The claim that used to live here -- that stereo was safe because the
		// alternate-eye path falls back to mono below stereo_min_fps -- WAS
		// WRONG and cost one eye its image for as long as the throttle was on.
		// See the correction block at the throttle globals. The skip count is
		// now forced odd while stereo is live, which is what actually makes
		// this safe.
		bool doCapture = true;
		if (g_captureThrottleEnabled && g_captureSkipN > 1) {
			doCapture = (g_flipsSinceCapture >= g_captureSkipN - 1);
		}

		// A PC menu is being drawn onto the front buffer after each present:
		// a clean after-Flip capture would replace it with the frame beneath,
		// and the headset would flicker between the two. BeforePresent() has
		// already captured the composite.
		if (menuOverlayRecent) doCapture = false;

		// Render twice owns the headset image while it is producing pairs.
		// The primary now holds the right eye of the PREVIOUS frame; capturing
		// it here would overwrite a true pair with a stale mono frame.
		if (NowMs() - g_rtLastPairMs.load() < 150) doCapture = false;

		if (doCapture) {
			g_flipsSinceCapture = 0;
			std::lock_guard<std::mutex> capLock(g_primaryCapMutex);
			t_inOwnCapture = true;
			CaptureFlippedSurfaceToSharedBuffer(self);
			t_inOwnCapture = false;
			g_lastCaptureMs = NowMs();
		}
		else {
			g_flipsSinceCapture++;
			g_skippedCaptures++;
		}
	}

	if (g_OnFrameTick) {
		g_OnFrameTick();
	}

	return hr;
}

static bool GameIsWindowed() {
	// [0x6FC794] selects the game's present path at +42253D: non-zero ->
	// Blt(primary <- back buffer) every frame (windowed), zero -> Flip.
	__try { return *reinterpret_cast<volatile int*>((uintptr_t)GetModuleHandleA(nullptr) + 0x2FC794) != 0; }
	__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static HRESULT BltOriginalAndCapture(IDirectDrawSurface7* self, LPRECT destRect, LPDIRECTDRAWSURFACE7 srcSurface,
                                     LPRECT srcRect, DWORD flags, LPDDBLTFX bltFx, void* caller) {
	const bool ontoPrimary = g_menuCaptureEnabled && g_primarySurface && self == g_primarySurface;
	if (!ontoPrimary) {
		return g_OriginalDDS7Blt ? g_OriginalDDS7Blt(self, destRect, srcSurface, srcRect, flags, bltFx) : DDERR_GENERIC;
	}
	const bool isFill = (flags & DDBLT_COLORFILL) != 0;
	const bool windowedPresent = !isFill && srcSurface && srcSurface != self && GameIsWindowed();
	if (windowedPresent) {
		// Windowed mode presents with this Blt instead of Flip: same rules as Flip.
		g_gameThreadId = GetCurrentThreadId();
		BeforePresent(self);
		HRESULT hr = g_OriginalDDS7Blt ? g_OriginalDDS7Blt(self, destRect, srcSurface, srcRect, flags, bltFx) : DDERR_GENERIC;
		AdvancePresentTag();
		const bool menuOverlayRecent = (NowMs() - g_lastOffFlipDrawMs.load() < g_menuHoldMs);
		g_lastPresentMs = NowMs();
		if (SUCCEEDED(hr) && !menuOverlayRecent) {
			std::lock_guard<std::mutex> capLock(g_primaryCapMutex);
			t_inOwnCapture = true;
			CaptureFlippedSurfaceToSharedBuffer(self);
			t_inOwnCapture = false;
			g_lastCaptureMs = NowMs();
		}
		return hr;
	}
	HRESULT hr = g_OriginalDDS7Blt ? g_OriginalDDS7Blt(self, destRect, srcSurface, srcRect, flags, bltFx) : DDERR_GENERIC;
	if (SUCCEEDED(hr)) NotePrimaryDrawnOffFlip(isFill ? "colour fill (Blt)" : "Blt", caller);
	return hr;
}

static HRESULT WINAPI HookedDDSurface7Blt(IDirectDrawSurface7* self, LPRECT destRect, LPDIRECTDRAWSURFACE7 srcSurface, LPRECT srcRect, DWORD flags, LPDDBLTFX bltFx) {
	// FIXED 2026-08-15: DDBLT_COLORFILL calls are solid-color surface clears
	// (dest=NULL srcSurface=NULL every time), not real image blits -- and the
	// game fires ~150 of them in a single burst during startup texture init,
	// well before a user ever gets far enough to test the pause menu. That
	// burst alone was blowing through the entire g_bltLogCap budget, so a
	// later, actually-interesting Blt (e.g. a pause menu draw) never got
	// logged at all. Colorfills are counted separately and don't touch the
	// real cap, so it stays available for real image copies.
	if (flags & DDBLT_COLORFILL) {
		static int colorFillCount = 0;
		colorFillCount++;
		if (colorFillCount == 1 || (colorFillCount % 100) == 0) {
			DebugLogger::LogFormat(
				"Blt: DDBLT_COLORFILL #%d (surface clear, not a real blit -- excluded from blt_log_cap)",
				colorFillCount);
		}
	} else if (srcSurface != nullptr && srcSurface == self) {
		// FIXED 2026-08-16: the SECOND thing that ate the whole budget.
		//
		// The 2026-08-16 log finally captured 200 non-colorfill Blt calls --
		// and every one of them was the game blitting a surface ONTO ITSELF in
		// 320x4 and 320x2 horizontal strips, sweeping down and back up across
		// both halves of a 640-wide surface. That is MGS1's own screen-wipe /
		// transition effect, not the pause menu. It fired at 14:21:51, spent
		// the entire cap in about 15 milliseconds, and the pause menu opened
		// later with no budget left -- exactly the same failure mode the
		// colorfill filter above was written to fix, arriving through a
		// different door.
		//
		// A blit whose source surface IS its destination cannot be a menu
		// being composited in from somewhere else; it can only be the surface
		// rearranging its own pixels. So these are counted separately and
		// sparsely logged, leaving the real cap for blits that bring in
		// content from another surface -- which is what the pause menu would
		// have to be.
		static int selfBltCount = 0;
		selfBltCount++;
		if (selfBltCount == 1 || (selfBltCount % 200) == 0) {
			DebugLogger::LogFormat(
				"Blt: self-to-self #%d (screen wipe / transition, source surface == destination -- "
				"excluded from blt_log_cap)",
				selfBltCount);
		}
	} else if (g_bltLogCount < g_bltLogCap) {
		g_bltLogCount++;
		char destStr[64] = "NULL";
		char srcStr[64] = "NULL";
		if (destRect) {
			sprintf_s(destStr, sizeof(destStr), "(%ld,%ld)-(%ld,%ld) %ldx%ld",
				destRect->left, destRect->top, destRect->right, destRect->bottom,
				destRect->right - destRect->left, destRect->bottom - destRect->top);
		}
		if (srcRect) {
			sprintf_s(srcStr, sizeof(srcStr), "(%ld,%ld)-(%ld,%ld) %ldx%ld",
				srcRect->left, srcRect->top, srcRect->right, srcRect->bottom,
				srcRect->right - srcRect->left, srcRect->bottom - srcRect->top);
		}
		DebugLogger::LogFormat(
			"Blt #%d: self=%p dest=%s srcSurface=%p srcRect=%s flags=0x%08X%s",
			g_bltLogCount, (void*)self, destStr, (void*)srcSurface, srcStr, (unsigned)flags,
			(g_bltLogCount == g_bltLogCap) ? " -- log cap reached, further Blt calls will not be logged" : "");
	}
	return BltOriginalAndCapture(self, destRect, srcSurface, srcRect, flags, bltFx, _ReturnAddress());
}

static HRESULT WINAPI HookedDDSurface7BltFast(IDirectDrawSurface7* self, DWORD dstX, DWORD dstY, LPDIRECTDRAWSURFACE7 srcSurface, LPRECT srcRect, DWORD flags) {
	if (g_bltFastLogCount < g_bltFastLogCap) {
		g_bltFastLogCount++;
		char srcStr[64] = "NULL";
		if (srcRect) {
			sprintf_s(srcStr, sizeof(srcStr), "(%ld,%ld)-(%ld,%ld) %ldx%ld",
				srcRect->left, srcRect->top, srcRect->right, srcRect->bottom,
				srcRect->right - srcRect->left, srcRect->bottom - srcRect->top);
		}
		DebugLogger::LogFormat(
			"BltFast #%d: self=%p dstX=%lu dstY=%lu srcSurface=%p srcRect=%s flags=0x%08X%s",
			g_bltFastLogCount, (void*)self, (unsigned long)dstX, (unsigned long)dstY,
			(void*)srcSurface, srcStr, (unsigned)flags,
			(g_bltFastLogCount == g_bltFastLogCap) ? " -- log cap reached, further BltFast calls will not be logged" : "");
	}
	return g_OriginalDDS7BltFast ? g_OriginalDDS7BltFast(self, dstX, dstY, srcSurface, srcRect, flags) : DDERR_GENERIC;
}

static HRESULT WINAPI HookedDD7CreateSurface(IDirectDraw7* self, LPDDSURFACEDESC2 desc, LPDIRECTDRAWSURFACE7* outSurface, IUnknown* outer) {
	HRESULT hr = g_OriginalDD7CreateSurface ? g_OriginalDD7CreateSurface(self, desc, outSurface, outer) : DDERR_GENERIC;

	// ---- surface census -----------------------------------------------------
	// Every surface the game asks for, once each. The reason this matters is
	// the DEPTH BUFFER: per-pixel depth is what turns one rendered frame into a
	// genuine stereo pair without rendering twice. Now that we know the game
	// renders through Direct3D, a Z-buffer should exist -- and if it is created
	// here, we can find it. DDSCAPS_ZBUFFER is what to look for.
	if (SUCCEEDED(hr) && desc) {
		static int surfLog = 0;
		if (surfLog < 40) {
			surfLog++;
			const DWORD caps = desc->ddsCaps.dwCaps;
			const DWORD caps2 = desc->ddsCaps.dwCaps2;
			const bool isZ = (caps & DDSCAPS_ZBUFFER) != 0;
			DebugLogger::LogFormat(
				"Surface created #%d: %ux%u caps=0x%08X caps2=0x%08X [%s%s%s%s%s%s%s%s] bpp=%u zbits=%u%s",
				surfLog,
				(desc->dwFlags & DDSD_WIDTH) ? desc->dwWidth : 0,
				(desc->dwFlags & DDSD_HEIGHT) ? desc->dwHeight : 0,
				(unsigned)caps, (unsigned)caps2,
				(caps & DDSCAPS_PRIMARYSURFACE) ? "PRIMARY " : "",
				(caps & DDSCAPS_BACKBUFFER) ? "BACKBUFFER " : "",
				(caps & DDSCAPS_OFFSCREENPLAIN) ? "OFFSCREEN " : "",
				(caps & DDSCAPS_TEXTURE) ? "TEXTURE " : "",
				(caps & DDSCAPS_3DDEVICE) ? "3DDEVICE " : "",
				(caps & DDSCAPS_ZBUFFER) ? "ZBUFFER " : "",
				(caps & DDSCAPS_VIDEOMEMORY) ? "VIDMEM " : "",
				(caps & DDSCAPS_SYSTEMMEMORY) ? "SYSMEM " : "",
				(desc->dwFlags & DDSD_PIXELFORMAT) ? desc->ddpfPixelFormat.dwRGBBitCount : 0,
				(desc->dwFlags & DDSD_PIXELFORMAT) ? desc->ddpfPixelFormat.dwZBufferBitDepth : 0,
				isZ ? "   <<-- DEPTH BUFFER, this is the one that matters for stereo" : "");
		}
	}

	if (SUCCEEDED(hr) && desc && outSurface && *outSurface &&
		(desc->ddsCaps.dwCaps & DDSCAPS_PRIMARYSURFACE)) {
		g_primarySurface = *outSurface;
		DebugLogger::LogFormat("Primary surface identified: %p (menus drawn onto it with GDI are captured from here)",
			(void*)g_primarySurface);
	}
	if (SUCCEEDED(hr) && outSurface && *outSurface && !g_MenuHooksInstalled && g_menuCaptureEnabled) {
		// IDirectDrawSurface7 vtable: GetDC=17, ReleaseDC=26, Unlock=32.
		const bool a = PatchVTable(*outSurface, 17, reinterpret_cast<void*>(&HookedDDSurface7GetDC), reinterpret_cast<void**>(&g_OriginalDDS7GetDC));
		const bool b = PatchVTable(*outSurface, 26, reinterpret_cast<void*>(&HookedDDSurface7ReleaseDC), reinterpret_cast<void**>(&g_OriginalDDS7ReleaseDC));
		const bool c = PatchVTable(*outSurface, 32, reinterpret_cast<void*>(&HookedDDSurface7Unlock), reinterpret_cast<void**>(&g_OriginalDDS7Unlock));
		g_MenuHooksInstalled = true;
		EnsureMenuFlushWorker();
		DebugLogger::LogFormat("Menu capture hooks: GetDC=%d ReleaseDC=%d Unlock=%d (PC-port pause/load/save/options menus)",
			a ? 1 : 0, b ? 1 : 0, c ? 1 : 0);
	}
	if (SUCCEEDED(hr) && outSurface && *outSurface && !g_FlipHooked) {
		if (PatchVTable(*outSurface, 11, reinterpret_cast<void*>(&HookedDDSurface7Flip), reinterpret_cast<void**>(&g_OriginalDDS7Flip))) {
			g_FlipHooked = true;
			DebugLogger::Log("DirectDrawSurface7::Flip hook installed");
		}
	}

	// Pause-menu investigation: hook Blt/BltFast the same way, on the same
	// vtable-per-class assumption already proven correct for Flip above.
	// Standard IDirectDrawSurface7 vtable order: QueryInterface=0, AddRef=1,
	// Release=2, AddAttachedSurface=3, AddOverlayDirtyRect=4, Blt=5,
	// BltBatch=6, BltFast=7, ... Flip=11 (matches what's already confirmed
	// working two blocks up).
	if (SUCCEEDED(hr) && outSurface && *outSurface && !g_BltHooked) {
		if (PatchVTable(*outSurface, 5, reinterpret_cast<void*>(&HookedDDSurface7Blt), reinterpret_cast<void**>(&g_OriginalDDS7Blt))) {
			g_BltHooked = true;
			DebugLogger::Log("DirectDrawSurface7::Blt hook installed (pause-menu investigation)");
		}
	}
	if (SUCCEEDED(hr) && outSurface && *outSurface && !g_BltFastHooked) {
		if (PatchVTable(*outSurface, 7, reinterpret_cast<void*>(&HookedDDSurface7BltFast), reinterpret_cast<void**>(&g_OriginalDDS7BltFast))) {
			g_BltFastHooked = true;
			DebugLogger::Log("DirectDrawSurface7::BltFast hook installed (pause-menu investigation)");
		}
	}

	return hr;
}

// Shared by both entry points: once we have any IDirectDraw7*, wire up the
// CreateSurface hook the same way regardless of which factory function
// produced it.
static void TryHookCreateSurface(IDirectDraw7* dd7) {
	if (!dd7 || g_DD7Hooked) {
		return;
	}
	if (PatchVTable(dd7, 6, reinterpret_cast<void*>(&HookedDD7CreateSurface), reinterpret_cast<void**>(&g_OriginalDD7CreateSurface))) {
		g_DD7Hooked = true;
		DebugLogger::Log("IDirectDraw7::CreateSurface hook installed");
	}
}

// ---------------------------------------------------------------------------
// SELF-SEEDING THE CreateSurface HOOK -- fixes a startup race we had been
// winning by luck for weeks, and lost three times running on 2026-08-15.
//
// THE BUG. Until now the ONLY way we ever got a CreateSurface hook was to
// intercept the game's call to DirectDrawCreate/Ex and patch the vtable of the
// IDirectDraw7 it handed back. That call happens ONCE, within the first few
// hundred milliseconds of process start -- and our injection thread is racing
// it. We resolve ddraw.dll, MinHook two exports, and only then are we armed.
// In the 2026-08-15 logs that arming completed 455 ms after DLL attach on the
// failing runs and 390 ms on the one that worked. The game got there first
// twice, so `HookedDirectDrawCreateEx called` never appeared, no surface was
// ever hooked, `haveCapturedMonoFrame` stayed 0 forever, and the headset got
// the RED/GREEN fallback for the whole session. Nothing recovers from this,
// because the one call we needed to see already happened.
//
// THE FIX, and why it fully closes the race rather than narrowing it. A COM
// object's vtable is per-CLASS, not per-instance: every IDirectDraw7 the
// wrapper hands out points at the same static function table. So we do not
// need the GAME's object at all. We create our OWN throwaway IDirectDraw7,
// patch slot 6 (CreateSurface) in the table it shares with the game's object,
// and release it. From that moment CreateSurface is hooked for everyone.
//
// The reason that is a complete fix and not a smaller race: the game creates
// its DirectDraw object almost immediately, but it does not create the PRIMARY
// SURFACE until seconds later -- 2.4 s after DirectDrawCreateEx in the run that
// worked, while it loads. We only ever needed to be armed before the surface,
// not before the factory call. Seeding buys us that entire window.
//
// Fail-soft on purpose: if creating our own object fails, or the wrapper turns
// out to use per-instance vtables, we log it and fall straight back to the old
// intercept-the-factory-call path, which is still installed and still works
// whenever we win the race. This can only add coverage.
// Own function because MSVC refuses __try in a function that needs object
// unwinding (C2712), and the seed function below builds std::strings for its
// log lines.
static IDirectDraw7* ReadGameDirectDrawPointer() {
	__try {
		return *reinterpret_cast<IDirectDraw7* volatile*>((uintptr_t)GetModuleHandleA(nullptr) + 0x2FC730);
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		return nullptr;
	}
}

static void SeedCreateSurfaceHookFromOwnObject() {
	if (g_DD7Hooked) {
		return; // the game beat us to it and we already caught it -- nothing to do
	}

	// ---- LATE ARRIVAL: use the GAME'S object, never make a second one ------
	// FOUND 2026-09-26 with mgs1_vr_launch.log. When our hooks arrive after
	// the game has already created its DirectDraw object, creating a second
	// IDirectDraw7 through GOG's DDraw->D3D9 wrapper (and releasing it) breaks
	// the wrapper's window setup: the game window is left HIDDEN (visible=0,
	// never restyled to its 1920x1080 popup), so it can never take focus. The
	// PC port then runs unfocused -- no sound, no input -- and nothing shows on
	// the monitor, while the headset still gets frames. 3 of 3 launches fit:
	// seed before the game's DirectDrawCreateEx = fine, seed after = broken.
	//
	// The game keeps its IDirectDraw7 at mgsi.exe+2FC730 (static disassembly:
	// +41F315 pushes 0x6FC730 as the out-pointer of DirectDrawCreateEx, and
	// +41F3B1 reads it back for QueryInterface). If it is already there, patch
	// CreateSurface through the game's own object: no creation, no Release,
	// no footprint in the wrapper at all.
	IDirectDraw7* gameDD = ReadGameDirectDrawPointer();
	if (gameDD) {
		TryHookCreateSurface(gameDD);
		DebugLogger::LogFormat("Seed: the game ALREADY created its DirectDraw (mgsi.exe+2FC730 = %p) -- CreateSurface %s through the GAME'S object; no object of our own created",
			(void*)gameDD, g_DD7Hooked ? "hooked" : "NOT hooked");
		return;
	}
	if (!g_OriginalDirectDrawCreateEx) {
		DebugLogger::Log("Seed: no DirectDrawCreateEx trampoline, cannot self-seed the CreateSurface hook");
		return;
	}

	// Call the TRAMPOLINE, not the export -- going through our own hook here
	// would re-enter this logic and log a phantom "HookedDirectDrawCreateEx
	// called" that would be very confusing in the log six months from now.
	IDirectDraw7* seedDD = nullptr;
	HRESULT hr = g_OriginalDirectDrawCreateEx(nullptr, reinterpret_cast<LPVOID*>(&seedDD), IID_IDirectDraw7, nullptr);
	if (FAILED(hr) || !seedDD) {
		DebugLogger::LogFormat("Seed: could not create our own IDirectDraw7 (0x%08X) -- falling back to intercepting the game's factory call", (unsigned)hr);
		return;
	}

	TryHookCreateSurface(seedDD);

	// The vtable patch lives in the wrapper's shared static table, so it
	// survives this Release. We deliberately hold no reference: an extra
	// IDirectDraw7 alive for the whole session could change the wrapper's
	// device/cooperative-level bookkeeping, and we want zero behavioural
	// footprint from a diagnostic object.
	seedDD->Release();

	if (g_DD7Hooked) {
		DebugLogger::Log("Seed: CreateSurface hooked from our OWN IDirectDraw7 -- the startup race with the game's DirectDrawCreate call no longer matters");
	}
	else {
		DebugLogger::Log("WARNING: Seed: vtable patch did not take. This wrapper may use per-instance vtables; we are back to relying on catching the game's factory call, so a slow start can still produce a RED/GREEN session.");
	}
}

static HRESULT WINAPI HookedDirectDrawCreateEx(GUID* guid, LPVOID* outDD, REFIID iid, IUnknown* outer) {
	DebugLogger::Log("HookedDirectDrawCreateEx called");
	HRESULT hr = g_OriginalDirectDrawCreateEx ? g_OriginalDirectDrawCreateEx(guid, outDD, iid, outer) : DDERR_GENERIC;

	if (SUCCEEDED(hr) && outDD && *outDD && iid == IID_IDirectDraw7) {
		TryHookCreateSurface(reinterpret_cast<IDirectDraw7*>(*outDD));
	}

	return hr;
}

// MGS1 (built well before DirectDrawCreateEx was the norm) almost certainly
// calls the original DirectDrawCreate, which only ever hands back an
// IDirectDraw (the DX3-era interface) -- not IDirectDraw7 directly. We
// QueryInterface our way up to IDirectDraw7 ourselves so we can reuse the
// exact same CreateSurface/Flip hook path either way.
static HRESULT WINAPI HookedDirectDrawCreate(GUID* guid, LPVOID* outDD, IUnknown* outer) {
	DebugLogger::Log("HookedDirectDrawCreate called");
	HRESULT hr = g_OriginalDirectDrawCreate ? g_OriginalDirectDrawCreate(guid, outDD, outer) : DDERR_GENERIC;

	if (SUCCEEDED(hr) && outDD && *outDD) {
		IUnknown* unk = reinterpret_cast<IUnknown*>(*outDD);
		IDirectDraw7* dd7 = nullptr;
		HRESULT qiHr = unk->QueryInterface(IID_IDirectDraw7, reinterpret_cast<void**>(&dd7));
		if (SUCCEEDED(qiHr) && dd7) {
			DebugLogger::Log("DirectDrawCreate -> QueryInterface(IDirectDraw7) succeeded");
			TryHookCreateSurface(dd7);
			dd7->Release(); // vtable patch lives on the shared COM object; we don't need to keep our own ref
		}
		else {
			DebugLogger::LogFormat("WARNING: DirectDrawCreate succeeded but QueryInterface(IDirectDraw7) failed: 0x%08X", (unsigned)qiHr);
		}
	}

	return hr;
}

bool InstallDdrawHooks(VRFrameTickFn onFrameTick) {
	g_OnFrameTick = onFrameTick;
	LoadCaptureConfig();

	HMODULE hDdraw = GetModuleHandleA("ddraw.dll");
	if (!hDdraw) {
		hDdraw = LoadLibraryA("ddraw.dll");
	}

	if (!hDdraw) {
		DebugLogger::Log("ERROR: Could not load ddraw.dll");
		return false;
	}

	char resolvedPath[MAX_PATH] = {};
	if (GetModuleFileNameA(hDdraw, resolvedPath, MAX_PATH)) {
		DebugLogger::LogFormat("ddraw.dll resolved to: %s", resolvedPath);
	}
	else {
		DebugLogger::Log("WARNING: GetModuleFileNameA failed for the resolved ddraw.dll handle");
	}

	auto pDirectDrawCreate = reinterpret_cast<DirectDrawCreate_t>(GetProcAddress(hDdraw, "DirectDrawCreate"));
	if (pDirectDrawCreate) {
		MH_STATUS s = MH_CreateHook(reinterpret_cast<LPVOID>(pDirectDrawCreate), reinterpret_cast<LPVOID>(&HookedDirectDrawCreate), reinterpret_cast<LPVOID*>(&g_OriginalDirectDrawCreate));
		if (s == MH_OK || s == MH_ERROR_ALREADY_CREATED) {
			s = MH_EnableHook(reinterpret_cast<LPVOID>(pDirectDrawCreate));
			if (s == MH_OK || s == MH_ERROR_ENABLED) {
				DebugLogger::Log("DirectDrawCreate hook installed");
			}
			else {
				DebugLogger::LogFormat("ERROR: MH_EnableHook(DirectDrawCreate) failed: %d", s);
			}
		}
		else {
			DebugLogger::LogFormat("ERROR: MH_CreateHook(DirectDrawCreate) failed: %d", s);
		}
	}
	else {
		DebugLogger::Log("WARNING: Could not locate DirectDrawCreate export (older DirectDraw entry point) -- only DirectDrawCreateEx will be hooked");
	}

	auto pDirectDrawCreateEx = reinterpret_cast<DirectDrawCreateEx_t>(GetProcAddress(hDdraw, "DirectDrawCreateEx"));
	if (!pDirectDrawCreateEx) {
		DebugLogger::Log("ERROR: Could not locate DirectDrawCreateEx export");
		return false;
	}

	MH_STATUS s = MH_CreateHook(reinterpret_cast<LPVOID>(pDirectDrawCreateEx), reinterpret_cast<LPVOID>(&HookedDirectDrawCreateEx), reinterpret_cast<LPVOID*>(&g_OriginalDirectDrawCreateEx));
	if (s != MH_OK && s != MH_ERROR_ALREADY_CREATED) {
		DebugLogger::LogFormat("ERROR: MH_CreateHook(DirectDrawCreateEx) failed: %d", s);
		return false;
	}

	s = MH_EnableHook(reinterpret_cast<LPVOID>(pDirectDrawCreateEx));
	if (s != MH_OK && s != MH_ERROR_ENABLED) {
		DebugLogger::LogFormat("ERROR: MH_EnableHook(DirectDrawCreateEx) failed: %d", s);
		return false;
	}

	DebugLogger::Log("DirectDrawCreateEx hook installed");

	// Both factory hooks are live. Now stop depending on them: patch
	// CreateSurface via an object of our own so that even if the game already
	// made its IDirectDraw7 before we got here, we still see the primary
	// surface when it is created seconds later. See the long comment above.
	SeedCreateSurfaceHookFromOwnObject();

	return true;
}

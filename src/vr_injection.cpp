#include <windows.h>
#include <cstdint>
#include <cmath>
#include <string>
#include <cstring>
#include <vector>
#include <mutex>
#include <atomic>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <xinput.h>
#define XR_USE_GRAPHICS_API_D3D11
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include "MinHook.h"
#include "../include/debug_logging.h"
#include "../include/ddraw_hook.h"
#include "../include/d3d9_capture_hook.h"
#include "../include/camera_write_hook.h"
#include "../include/vr_aim.h"
#include "../include/motion_aim.h"
#include "../include/render_twice.h"

#pragma comment(lib, "xinput9_1_0.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")

#define PI 3.14159265359f

// CONFIRMED REAL, from a working community Cheat Engine table for MGS1
// Integral PC (mgsi.exe) -- verified by that table's own live free-camera
// Lua script, which reads/writes these exact addresses every frame to move
// the camera. All fields are 2 bytes (int16), NOT 4 bytes like the earlier
// placeholder guessed -- position and rotation are separate, non-contiguous
// static addresses (not one packed struct), each independently confirmed.
// Expressed as module-relative offsets (added to GetMgsiModuleBase() at use
// time) rather than hardcoded absolute addresses, consistent with the rest
// of this file's convention -- the source table's own absolute addresses
// (e.g. 0x00993FA0) and module-relative form (mgsi.exe+593FA0) agree exactly
// assuming the standard 0x00400000 base, which matches every other
// confirmed address found in this project so far.
static constexpr uintptr_t kCamPosXOffset = 0x593FA0;
static constexpr uintptr_t kCamPosYOffset = 0x593FA2;
static constexpr uintptr_t kCamPosZOffset = 0x593FA4;
static constexpr uintptr_t kCamRotYOffset = 0x593FC8; // yaw, per the source table's "RY" label
static constexpr uintptr_t kCamRotXOffset = 0x593FCA; // pitch, per the source table's "RX" label

// SECOND block, same GM_SnakeCameraWork struct as the primary block above,
// just further-in fields -- confirmed via real FoxdieTeam decompilation
// source + offset math (base=593FA0=position/offset0x00):
//   593FA8 = target, 593FB0 = rotate (NOT a separate FPV struct as
//   originally guessed -- rotate is a real field the game's own camera
//   logic reads FROM as an interpolation source toward rotate2/593FC8,
//   which is our real, correct write target). Declarations kept for
//   reference/future use (e.g. writing position/target directly once we
//   have Snake's real position source) but NOT written to for rotation
//   anymore -- see the real-write block below.
static constexpr uintptr_t kFpvCamPosXOffset = 0x593FA8; // = GM_SnakeCameraWork.target.vx
static constexpr uintptr_t kFpvCamPosYOffset = 0x593FAA; // = GM_SnakeCameraWork.target.vy
static constexpr uintptr_t kFpvCamPosZOffset = 0x593FAC; // = GM_SnakeCameraWork.target.vz
static constexpr uintptr_t kFpvCamRotYOffset = 0x593FB0; // = GM_SnakeCameraWork.rotate.vx (NOT rotate2 -- do not write here)
static constexpr uintptr_t kFpvCamRotXOffset = 0x593FB2; // = GM_SnakeCameraWork.rotate.vy (NOT rotate2 -- do not write here)
static constexpr uintptr_t kFpvCamZoomOffset = 0x594000;
static constexpr uintptr_t kFpvFlagByteOffset = 0x594002; // single byte, distinct from our own 0x324898 flag

// Real, confirmed built-in debug free-camera toggle (separate from our own
// gameplay FPV flag at 0x324898) -- "FREE CAMERA (built-in)" in the source
// table. Not currently used by the mod, but confirmed real and available
// for a future dedicated free-cam mode if wanted.
static constexpr uintptr_t kBuiltinFreeCameraOffset = 0x37C934;

struct MGS_SVECTOR { int32_t x, y, z; };
struct MGS_SROTATION { int16_t pitch, yaw, roll; };

// --- Live candidate diagnostics (title bar) --------------------------------
// Real, module-relative offsets narrowed down via Cheat Engine (2-byte int
// scan, alternating Changed/Unchanged while turning the FPV camera). None of
// these are confirmed as camera yaw/pitch yet -- this just surfaces their
// live values in the window title so they can be watched without tabbing
// into Cheat Engine. Update this list as candidates are ruled in/out.
struct CandidateOffset {
    const char* label;
    uintptr_t offset;
};

static const CandidateOffset g_candidateOffsets[] = {
    { "A", 0x51BCD8 },
    { "B", 0x51BDAA },
    { "C", 0x51BEAE },
    { "D", 0x51C04E },
};
static constexpr int kCandidateCount = sizeof(g_candidateOffsets) / sizeof(g_candidateOffsets[0]);

// --- Rotation candidate write-test (in-process, replaces the Lua script) --
// Same 116 addresses from tonight's Cheat Engine Increased/Decreased +
// Unchanged scan (module-relative, so these are permanent -- see project
// notes). The Cheat Engine console proved too unstable under sustained
// scripted read/write activity to finish testing all 116 (crashed at
// varying points -- #3, #8, etc. -- not tied to any specific address, so
// likely a CE-console-internal issue, not something in the test itself).
// Running the identical test natively inside our own already-injected DLL
// sidesteps that entirely: no separate process, no IPC, just direct reads/
// writes protected by the same SEH pattern already proven throughout this
// file (title bar diagnostics, FPV flag read/write, etc.).
static const uint32_t g_rotationCandidateOffsets[] = {
    0x4FBACE, 0x4FD00E, 0x4FE54E,
    0x4FFA8E, 0x4FFA96, 0x4FFA9E, 0x4FFAA6, 0x4FFAB6, 0x4FFABE, 0x4FFAC6, 0x4FFACE,
    0x500B32, 0x500B88, 0x500BE4, 0x500C40, 0x500D4C, 0x500DB0, 0x500E0C, 0x500E5E, 0x500E68,
    0x592056, 0x592072, 0x592076, 0x592084, 0x59209C, 0x5920BC, 0x5920C8, 0x5920CC, 0x5920D8, 0x5920DC,
    0x5920F0, 0x5920F4, 0x5920FC, 0x592100, 0x592104, 0x592108, 0x59211C, 0x592120, 0x59216C,
    0x5921C4, 0x5921D0, 0x5921D4, 0x5921D8, 0x5921DC, 0x5921E0, 0x5921E8, 0x5921EC, 0x5921F0, 0x5921F4,
    0x5921F8, 0x5921FC, 0x592200, 0x592204, 0x592208, 0x59220C, 0x592210, 0x592214, 0x592220, 0x592224, 0x59222C,
    0x5922AA, 0x5922AE, 0x5922B2, 0x5922B6, 0x5922BA, 0x5922BE, 0x5922CA, 0x5922CE, 0x5922DA, 0x5922DE,
    0x5922F2, 0x5922F6, 0x5922FE, 0x592302, 0x592306, 0x59230A, 0x59231E, 0x592322, 0x59236E,
    0x5923C6, 0x5923D2, 0x5923D6, 0x5923DA, 0x5923DE, 0x5923E2, 0x5923EA, 0x5923EE, 0x5923F2, 0x5923F6,
    0x5923FA, 0x5923FE, 0x592402, 0x592406, 0x59240A, 0x59240E, 0x592412, 0x592416, 0x59241A, 0x59241E,
    0x592422, 0x592426, 0x59242E, 0x592472, 0x592476, 0x59247A, 0x59247E, 0x592482,
    0x592552, 0x592556, 0x592592, 0x593F6C, 0x593FEC,
};
static constexpr int kRotationCandidateCount = sizeof(g_rotationCandidateOffsets) / sizeof(g_rotationCandidateOffsets[0]);

static uintptr_t GetMgsiModuleBase();

static bool g_rotationTestRequested = false;  // set true to trigger a run (see key hook below)
static bool g_rotationTestRunning = false;

static int16_t SafeRead16(uintptr_t addr) {
    __try {
        return *reinterpret_cast<volatile int16_t*>(addr);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0x7FFF; // sentinel: unreadable
    }
}

static bool SafeWrite16(uintptr_t addr, int16_t value) {
    __try {
        *reinterpret_cast<volatile int16_t*>(addr) = value;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Patches out the specific game instructions that overwrite our camera
// rotation writes every frame -- taken directly, byte-for-byte, from the
// reference community Cheat Engine table's own confirmed-working Lua
// script (writeBytes(0x0044437E, NOP x3) and writeBytes(0x0045495F, NOP x6)).
// Applied ONCE (not every frame -- these are one-time code patches, not
// live data), the first time the real camera write path activates.
static bool g_cameraNopPatchesApplied = false;

static bool PatchCodeBytes(uintptr_t addr, const uint8_t* bytes, size_t count) {
    DWORD oldProtect = 0;
    if (!VirtualProtect(reinterpret_cast<LPVOID>(addr), count, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        return false;
    }
    __try {
        memcpy(reinterpret_cast<void*>(addr), bytes, count);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        DWORD ignore = 0;
        VirtualProtect(reinterpret_cast<LPVOID>(addr), count, oldProtect, &ignore);
        return false;
    }
    DWORD ignore = 0;
    VirtualProtect(reinterpret_cast<LPVOID>(addr), count, oldProtect, &ignore);
    return true;
}

// Set from mgs1_vr_config.ini [openxr] enable_camera_nop_patches.
// DEFAULT OFF, and it should stay off -- see the block comment below.
static bool g_enableCameraNopPatches = false;

// ###########################################################################
// # ALL OF THESE PATCH ADDRESSES WERE WRONG. Verified against the real
// # mgsi.exe binary.
// #
// # mgsi.exe has ImageBase 0x400000 and no .reloc section, so it always loads
// # at 0x400000 and these constants -- which are added to the module base --
// # must be RVAs. Five of the seven were absolute VMAs copied verbatim out of
// # the community table's Lua (writeBytes(0x0044437E, ...)) and out of Cheat
// # Engine's disassembly view, i.e. 0x400000 too large:
// #
// #   patch    was          lands in            correct RVA   bytes there
// #   1        0x0044437E   .data (uninit)      0x4437E       66 89 01      OK
// #   3        0x00053A3A   .text  (correct)    --            66 89 11      OK
// #   4        0x00053A5B   .text  (correct)    --            66 89 51 02   OK
// #   5        0x004E1AE7   .data (uninit)      0xE1AE7       A3 C8 3F 99 00
// #   5b       0x004E1AEF   .data (uninit)      0xE1AEF       66 81 25 ...
// #   6        0x00453B95   .data (uninit)      0x53B95       66 89 35 ...
// #   7        0x00453BB7   .data (uninit)      0x53BB7       66 C7 05 ...
// #
// # So only patches 3 and 4 ever ran. The other five spent every session
// # writing 0x90 bytes into committed-but-uninitialised game globals -- 34
// # bytes of live state corruption per run, while cheerfully logging "OK",
// # because PatchCodeBytes only reports whether the write succeeded.
// #
// # The addresses below are now the VERIFIED-correct RVAs. They are left in
// # place for reference and experimentation only. The whole mechanism is
// # DISABLED BY DEFAULT and is no longer needed, because the camera rotation
// # is now written from inside the game's own frame (see camera_write_hook.cpp)
// # rather than fought for from another thread.
// #
// # Two specific warnings if you ever do turn this on:
// #  * Patch 1 (RVA 0x4437E) is the store inside GV_NearExp4PV at RVA 0x44345
// #    -- the game's GENERIC exponential interpolator, called all over the
// #    engine (including with 0x993FA0 for camera position 80 bytes later).
// #    NOPping it breaks every interpolated value in the game, not just the
// #    camera. The community table got away with it because its free camera
// #    did not care what else broke.
// #  * Patch 5b removes the game's own `and word [993FCA],0xFFF`. That mask is
// #    what makes pitch a well-defined unsigned 12-bit value. Removing it
// #    changes the meaning of every value written to that field.
// ###########################################################################
static void ApplyCameraNopPatchesOnce(uintptr_t base) {
    if (g_cameraNopPatchesApplied) {
        return;
    }

    if (!g_enableCameraNopPatches) {
        static bool loggedSkip = false;
        if (!loggedSkip) {
            loggedSkip = true;
            DebugLogger::Log("Camera NOP patches: SKIPPED (disabled by default). "
                "Five of the original seven addresses were absolute VMAs used as RVAs and were "
                "corrupting game .data; the in-frame camera hook replaces this mechanism entirely.");
        }
        g_cameraNopPatchesApplied = true; // don't re-check every frame
        return;
    }

    // Patch 1: the store inside GV_NearExp4PV (RVA 0x44345), the game's
    // generic interpolator. DO NOT ENABLE -- see the warning above.
    static constexpr uintptr_t kPatch1Offset = 0x0004437E;
    static const uint8_t kNop3[3] = { 0x90, 0x90, 0x90 };

    // Patches 3 and 4: found later, directly in the reference table's own
    // Auto Assembler scripts (DISABLE_FREE_CAM3/4) -- these disable a
    // rotation CLAMPING routine (bounds-checks against limits stored at
    // 594034-594048). This is very likely the actual cause of the
    // "recognizes direction then reverts to zero" symptom: our written
    // absolute head-angle value would look "out of range" to whatever
    // limits this clamp expects for the normal (non-free) camera, and get
    // snapped back. Both instructions are "mov [ecx+offset],dx" (4 bytes
    // each, matching the exact byte patterns from the table's own aobscan).
    static constexpr uintptr_t kPatch3Offset = 0x00053A3A;
    static constexpr uintptr_t kPatch4Offset = 0x00053A5B;
    static const uint8_t kNop4[4] = { 0x90, 0x90, 0x90, 0x90 };

    // Patch 5 (and 5b): found via direct Cheat Engine "find out what
    // accesses" trace on 593FC8/593FCA -- a fifth writer, in a different
    // code region entirely (4E1xxx, distinct from the 4437E/5495F/53Axxx
    // cluster patches 1-4 came from). Two instructions:
    //   4E1AE7: A3 C83F9900          -> mov [593FC8],eax        (5 bytes)
    //   4E1AEF: 66 81 25 CA3F9900 FF0F -> and word ptr [593FCA],0FFF (10 bytes)
    // The second instruction masks pitch to its low 12 bits every time this
    // runs -- confirmed real via direct disassembly, not inferred. Patching
    // BOTH: the write (so this path stops overwriting our value with
    // whatever's in EAX from elsewhere) and the mask (so our own write,
    // wherever it lands afterward in frame order, doesn't get truncated
    // by this specific instruction either).
    static constexpr uintptr_t kPatch5Offset = 0x000E1AE7;  // CORRECTED (was 0x004E1AE7 = absolute VMA)
    static constexpr uintptr_t kPatch5bOffset = 0x000E1AEF; // CORRECTED (was 0x004E1AEF = absolute VMA)
    static const uint8_t kNop5[5] = { 0x90, 0x90, 0x90, 0x90, 0x90 };
    static const uint8_t kNop10[10] = { 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 };

    // Patches 6 and 7 (NEW, this session): found via a live write-trace on
    // rotate2 specifically DURING a codec call / brief in-engine cutscene --
    // two genuinely new writer instructions never seen in any prior
    // gameplay trace, almost certainly the codec/cutscene system's own
    // scripted "reset camera to default facing" logic:
    //   453B95: 66 89 35 C83F9900       -> mov [593FC8],si          (7 bytes)
    //           writes the SI register (not EAX like patch 5) to yaw.
    //   453BB7: 66 C7 05 CA3F9900 0008  -> mov word ptr [593FCA],0800 (9 bytes)
    //           writes a HARDCODED IMMEDIATE 0x0800 (2048, the confirmed
    //           "centered/neutral" value from the reference table's own
    //           trackbar reset logic) to pitch -- strong confirmation this
    //           is a scripted reset-to-default, not a live camera read.
    // These fire specifically during codec/cutscene transitions and were
    // never touched by patches 1-5b (all found during normal gameplay
    // testing only) -- a strong candidate for the actual cause of the
    // camera fighting/flicker seen specifically during codec calls and
    // in-engine cutscenes.
    static constexpr uintptr_t kPatch6Offset = 0x00053B95;  // CORRECTED (was 0x00453B95 = absolute VMA)
    static constexpr uintptr_t kPatch7Offset = 0x00053BB7;  // CORRECTED (was 0x00453BB7 = absolute VMA)
    static const uint8_t kNop7[7] = { 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 };
    static const uint8_t kNop9[9] = { 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 };

    // Patch 1 is deliberately NEVER applied, even when patches are enabled:
    // its real target is inside the game's shared interpolator. Reported as
    // OK so the aggregate flag below stays meaningful.
    (void)kPatch1Offset;
    bool ok1 = true;
    // Patch 3's real original instruction is "66 89 11" (mov [ecx],dx) --
    // 3 bytes, not 2. Using kNop3 here (matching the real instruction
    // length), not the table's own newmem code shape (which relies on its
    // own jmp/label structure we're not replicating) -- a straightforward
    // fixed-length NOP of the actual instruction bytes, consistent with
    // how patches 1 and 2 were already done.
    bool ok3 = PatchCodeBytes(base + kPatch3Offset, kNop3, sizeof(kNop3));
    bool ok4 = PatchCodeBytes(base + kPatch4Offset, kNop4, sizeof(kNop4));
    bool ok5 = PatchCodeBytes(base + kPatch5Offset, kNop5, sizeof(kNop5));
    bool ok5b = PatchCodeBytes(base + kPatch5bOffset, kNop10, sizeof(kNop10));
    bool ok6 = PatchCodeBytes(base + kPatch6Offset, kNop7, sizeof(kNop7));
    bool ok7 = PatchCodeBytes(base + kPatch7Offset, kNop9, sizeof(kNop9));

    DebugLogger::LogFormat("Camera NOP patches: patch1=%s patch3=%s patch4=%s patch5=%s patch5b=%s patch6=%s patch7=%s",
        ok1 ? "OK" : "FAILED", ok3 ? "OK" : "FAILED", ok4 ? "OK" : "FAILED",
        ok5 ? "OK" : "FAILED", ok5b ? "OK" : "FAILED", ok6 ? "OK" : "FAILED", ok7 ? "OK" : "FAILED");

    g_cameraNopPatchesApplied = (ok1 && ok3 && ok4 && ok5 && ok5b && ok6 && ok7);
}

// --- Passive watch mode -----------------------------------------------
// Focused follow-up to the write-test: these specific offsets showed
// GENUINE movement in the last run (read-back value differed from what we
// wrote AND from the original -- meaning the game was actively recalculating
// them, not just rejecting our write and holding static). Everything else
// from the 112-candidate list reverted to the exact same static original
// every time and is considered ruled out.
//
// This mode does NOT write anything -- just logs the live value once a
// second, so you can turn the camera deliberately (no cutscene, no test-
// value interference) and see which of these actually tracks your view
// smoothly, versus just being some other live-but-unrelated game value.
static const uint32_t g_watchOffsets[] = {
    // Cluster A: steady small-step drift across consecutive reads --
    // exactly the shape a rotation value takes during continuous motion
    0x592472, 0x592476, 0x59247A, 0x59247E, 0x592482, 0x592552, 0x592556,
    // Cluster B: also showed real movement, worth a look even if less clean
    0x5922AA, 0x5922AE, 0x5922B2, 0x5922B6, 0x5922BA, 0x5922BE,
    0x5922CA, 0x5922CE, 0x5922DA, 0x5922DE,
    0x5922F2, 0x5922F6, 0x5922FE, 0x592302, 0x592306, 0x59230A,
    0x59231E, 0x592322,
    // A couple of the wider single-value movers, lower priority but cheap to include
    0x592072, 0x592076, 0x592084, 0x5920F4, 0x5920FC, 0x592100, 0x592104, 0x592108, 0x59211C, 0x592120,
};
static constexpr int kWatchCount = sizeof(g_watchOffsets) / sizeof(g_watchOffsets[0]);

static bool g_watchModeRequested = false;
static bool g_watchModeRunning = false;

static DWORD WINAPI WatchModeThreadProc(LPVOID) {
    g_watchModeRunning = true;
    DebugLogger::LogFormat("Watch mode starting -- see separate log file: %s", DebugLogger::GetTestLogPath().c_str());
    DebugLogger::TestLogFormat("=== WATCH MODE: %d candidates, logging live values once/sec for 60 seconds. Turn the camera deliberately (no cutscene!) and watch which columns move smoothly with your view. Press F10 again to stop early. ===", kWatchCount);

    uintptr_t base = GetMgsiModuleBase();

    // Header row so the columns are easy to scan
    std::string header = "time     | ";
    for (int i = 0; i < kWatchCount; ++i) {
        char buf[16];
        sprintf_s(buf, "%X", g_watchOffsets[i]);
        header += buf;
        header += " | ";
    }
    DebugLogger::TestLog(header);

    for (int tick = 0; tick < 60 && g_watchModeRunning; ++tick) {
        std::string line;
        for (int i = 0; i < kWatchCount; ++i) {
            int16_t v = SafeRead16(base + g_watchOffsets[i]);
            char buf[16];
            sprintf_s(buf, "%d", (int)v);
            line += buf;
            line += " | ";
        }
        DebugLogger::TestLog(line);
        Sleep(1000);
    }

    DebugLogger::TestLog("=== WATCH MODE: DONE (or stopped). Look for a column that ramped smoothly and consistently while you were turning, ideally wrapping around at the top/bottom of its range. ===");
    DebugLogger::Log("Watch mode: DONE.");
    g_watchModeRunning = false;
    g_watchModeRequested = false;
    return 0;
}

static void StartWatchModeIfRequested() {
    if (!g_watchModeRequested || g_watchModeRunning) {
        return;
    }
    HANDLE h = CreateThread(nullptr, 0, WatchModeThreadProc, nullptr, 0, nullptr);
    if (h) {
        CloseHandle(h);
    }
    else {
        g_watchModeRequested = false;
        DebugLogger::Log("Watch mode: failed to create thread");
    }
}

// --- Fast, narrow watch mode -------------------------------------------
// Focused on just the two most structurally distinct candidates from the
// first watch-mode run (592072, 592076 -- these held steady at low, round
// values (0/256/512/2048/2560) for multi-second stretches then jumped,
// unlike everything else which jittered every single sample -- a "discrete
// jump" shape is a real, different signature worth a closer look at higher
// resolution). Samples 10x/sec instead of 1x/sec, for 30 seconds, to reveal
// their true shape rather than just seeing possibly-aliased snapshots.
static const uint32_t g_fastWatchOffsets[] = { 0x592072, 0x592076 };
static constexpr int kFastWatchCount = sizeof(g_fastWatchOffsets) / sizeof(g_fastWatchOffsets[0]);

static bool g_fastWatchRequested = false;
static bool g_fastWatchRunning = false;

static DWORD WINAPI FastWatchThreadProc(LPVOID) {
    g_fastWatchRunning = true;
    DebugLogger::LogFormat("Fast watch mode starting -- see separate log file: %s", DebugLogger::GetTestLogPath().c_str());
    DebugLogger::TestLog("=== FAST WATCH MODE: 592072 and 592076, sampling 10x/sec for 30 seconds. Turn smoothly and continuously the whole time. Press F11 again to stop early. ===");

    uintptr_t base = GetMgsiModuleBase();
    DebugLogger::TestLog("tick | 592072 | 592076 |");

    for (int tick = 0; tick < 300 && g_fastWatchRunning; ++tick) {
        int16_t v0 = SafeRead16(base + g_fastWatchOffsets[0]);
        int16_t v1 = SafeRead16(base + g_fastWatchOffsets[1]);
        DebugLogger::TestLogFormat("%d | %d | %d |", tick, (int)v0, (int)v1);
        Sleep(100);
    }

    DebugLogger::TestLog("=== FAST WATCH MODE: DONE (or stopped). ===");
    DebugLogger::Log("Fast watch mode: DONE.");
    g_fastWatchRunning = false;
    g_fastWatchRequested = false;
    return 0;
}

static void StartFastWatchIfRequested() {
    if (!g_fastWatchRequested || g_fastWatchRunning) {
        return;
    }
    HANDLE h = CreateThread(nullptr, 0, FastWatchThreadProc, nullptr, 0, nullptr);
    if (h) {
        CloseHandle(h);
    }
    else {
        g_fastWatchRequested = false;
        DebugLogger::Log("Fast watch mode: failed to create thread");
    }
}

// Runs the full write-test sequence on a background thread (so it doesn't
// block the render/tick thread): for each candidate, write a test value,
// hold briefly, restore the original. Watch the game and note which index
// number's write produces a visible camera snap -- same idea as the Lua
// script, just running natively instead.
static DWORD WINAPI RotationCandidateTestThreadProc(LPVOID) {
    g_rotationTestRunning = true;
    DebugLogger::LogFormat("Rotation candidate test starting -- see separate log file: %s", DebugLogger::GetTestLogPath().c_str());
    DebugLogger::TestLogFormat("Rotation candidate test: starting, %d candidates, base=0x%p", kRotationCandidateCount, (void*)GetMgsiModuleBase());

    uintptr_t base = GetMgsiModuleBase();
    constexpr int16_t kTestValue = 16384;
    constexpr DWORD kHoldMs = 2000;

    for (int i = 0; i < kRotationCandidateCount; ++i) {
        uintptr_t addr = base + g_rotationCandidateOffsets[i];
        int16_t original = SafeRead16(addr);

        DebugLogger::TestLogFormat(">>> [%d/%d] mgsi.exe+%X: original=%d, writing %d (watch the game NOW) <<<",
            i + 1, kRotationCandidateCount, g_rotationCandidateOffsets[i], original, kTestValue);

        bool wrote = SafeWrite16(addr, kTestValue);
        if (!wrote) {
            DebugLogger::TestLog("    -> write FAILED (unmapped/protected memory)");
            continue;
        }

        Sleep(kHoldMs);

        int16_t readBack = SafeRead16(addr);
        if (readBack == kTestValue) {
            DebugLogger::TestLogFormat("    -> value STUCK at %d", (int)readBack);
        }
        else {
            DebugLogger::TestLogFormat("    -> value REVERTED to %d", (int)readBack);
        }

        SafeWrite16(addr, original); // restore before moving on
        Sleep(300);
    }

    DebugLogger::TestLog("Rotation candidate test: DONE. Scroll this file for which index number lined up with a visible camera snap.");
    DebugLogger::Log("Rotation candidate test: DONE.");
    g_rotationTestRunning = false;
    g_rotationTestRequested = false;
    return 0;
}

static void StartRotationCandidateTestIfRequested() {
    if (!g_rotationTestRequested || g_rotationTestRunning) {
        return;
    }
    HANDLE h = CreateThread(nullptr, 0, RotationCandidateTestThreadProc, nullptr, 0, nullptr);
    if (h) {
        CloseHandle(h);
    }
    else {
        g_rotationTestRequested = false;
        DebugLogger::Log("Rotation candidate test: failed to create thread");
    }
}

static uintptr_t GetMgsiModuleBase() {
    static uintptr_t cachedBase = 0;
    if (cachedBase != 0) {
        return cachedBase;
    }
    HMODULE hMod = GetModuleHandleA(nullptr); // main exe module of this process
    cachedBase = reinterpret_cast<uintptr_t>(hMod);
    return cachedBase;
}

// --- CONFIRMED (not placeholder) -- found via Cheat Engine, cross-session
// stable static address, verified: writing 1 here visibly switches the
// camera toward FPV (game's own input-validation immediately reverts it
// back to 0 the next frame if the real double-tap gesture wasn't detected,
// which is why this needs to be force-written every frame rather than set
// once). See ddraw_hook.cpp/session notes for the write-trace confirming this.
static constexpr uintptr_t kFpvFlagOffset = 0x324898;

static bool ReadRealFpvFlag() {
    uintptr_t addr = GetMgsiModuleBase() + kFpvFlagOffset;
    __try {
        return *reinterpret_cast<volatile int32_t*>(addr) != 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// NOTE: an earlier version of this force-write directly into game memory
// every frame (see git history / prior session notes if needed). That
// caused the game's own FPV gesture-validation logic to fight the forced
// value, producing a visible flicker where FPV kept trying to engage but
// never stuck. Replaced with TriggerFpvDoubleTap() (vr_input.cpp), which
// sends the real key-tap gesture once at startup instead -- see
// VRTickFromRenderer for where it's called.

XrInstance g_xrInstance = XR_NULL_HANDLE;
XrSession  g_xrSession = XR_NULL_HANDLE;
XrSpace    g_xrPlaySpace = XR_NULL_HANDLE;
XrSystemId g_xrSystemId = XR_NULL_SYSTEM_ID;

static ID3D11Device* g_xrD3DDevice = nullptr;
static ID3D11DeviceContext* g_xrD3DContext = nullptr;
static bool g_xrSessionReady = false;
static bool g_xrSessionRunning = false;
static bool g_xrInputInitialized = false;
static std::atomic<bool> g_xrFrameThreadRunning = false;
static HANDLE g_xrFrameThreadHandle = nullptr;
static std::atomic<bool> g_runtimeTickThreadRunning = false;
static HANDLE g_runtimeTickThreadHandle = nullptr;
static std::mutex g_hmdLookMutex;
static float g_hmdAccumDeltaX = 0.0f;
static float g_hmdAccumDeltaY = 0.0f;

struct XrEyeRenderTarget {
    XrSwapchain swapchain = XR_NULL_HANDLE;
    int32_t width = 0;
    int32_t height = 0;
    std::vector<XrSwapchainImageD3D11KHR> images;
    std::vector<ID3D11RenderTargetView*> rtvs;
};

static XrEyeRenderTarget g_xrEyeTargets[2];
static std::vector<XrViewConfigurationView> g_xrViewConfigViews;
static std::vector<XrView> g_xrViews;
static std::vector<XrCompositionLayerProjectionView> g_xrProjectionViews;
static XrCompositionLayerProjection g_xrProjectionLayer{ XR_TYPE_COMPOSITION_LAYER_PROJECTION };
static DXGI_FORMAT g_xrSwapchainFormat = DXGI_FORMAT_UNKNOWN;
// COLOUR (2026-09-27, the "red shading" bug). OpenXR treats a *_UNORM swapchain
// as LINEAR light and encodes it to sRGB for the display. The game's pixels are
// already sRGB, so that encoding lifted every dark tone massively: MGS1's
// near-black walls and ceilings carry faint red in their texel noise, and the
// lift turned it into visible red shading -- in the headset only, never on the
// monitor. F9 snapshots proved our copies match the monitor; simulating the
// linear->sRGB lift on those copies reproduces the red exactly. Fix: declare
// the swapchain *_SRGB (so the bytes are taken as sRGB, i.e. as-is) and render
// into it through a non-sRGB view so the bytes are written unchanged.
static bool g_srgbSwapchain = true;   // [openxr] srgb_swapchain
static bool g_xrRenderTargetsReady = false;
static ID3D11Texture2D* g_xrMonoCaptureTexture = nullptr;
// Alternate-eye stereo keeps one image per eye. Each is refreshed only on the
// game frames rendered from that eye's camera position, so both eyes hold a
// genuinely different viewpoint rather than the same picture shifted sideways.
static ID3D11Texture2D* g_xrEyeCaptureTexture[2] = { nullptr, nullptr };
static bool g_haveEyeCapture[2] = { false, false };
static int32_t g_captureWidth = 0;
static int32_t g_captureHeight = 0;
static std::vector<uint8_t> g_captureRgba;     // destination buffer, sized to the eye render target (CPU fallback path only)
static std::vector<uint8_t> g_capturedSourceRgba; // scratch buffer for the raw frame pulled from d3d9_capture_hook
static int g_stereoParallaxPixels = 24;

// --- GPU bilinear resample --------------------------------------------------
// Replaces the CPU nearest-neighbor upscale (ResampleNearestInto/ResampleNearest,
// still below for the ini-toggled fallback) with a full-screen textured quad
// drawn into the eye/mono capture textures using a linear sampler. The raw
// game frame is uploaded once per frame into a small shader-resource-only
// source texture at ITS native resolution (e.g. 640x480 -- a few hundred KB,
// cheap); the GPU then does the up-filtering for free as part of the sample,
// instead of the CPU doing ~4.7 million scalar reads/writes with no filtering
// at all. Toggle: [openxr] gpu_bilinear_resample (default 1).
static bool g_gpuBilinearResample = true;
static ID3D11RenderTargetView* g_xrMonoCaptureRTV = nullptr;
static ID3D11RenderTargetView* g_xrEyeCaptureRTV[2] = { nullptr, nullptr };
static ID3D11Texture2D* g_xrSourceCaptureTexture = nullptr;
static ID3D11ShaderResourceView* g_xrSourceCaptureSRV = nullptr;
static int32_t g_sourceCaptureWidth = 0;
static int32_t g_sourceCaptureHeight = 0;
static ID3D11VertexShader* g_blitVS = nullptr;
static ID3D11PixelShader* g_blitPS = nullptr;
static ID3D11SamplerState* g_blitLinearSampler = nullptr;
static ID3D11RasterizerState* g_blitRasterState = nullptr;
static ID3D11BlendState* g_blitBlendState = nullptr;
static ID3D11DepthStencilState* g_blitDepthState = nullptr;
static ID3D11Buffer* g_blitCropCB = nullptr;
static bool g_blitPipelineReady = false;

// --- HUD magnification insets ------------------------------------------------
// Short-term goal, 2026-08-15: MGS1's HUD (radar, life gauge, weapon/ammo,
// stance icon) is drawn at native 4:3 res and reads small once stretched
// across a headset's much wider FOV. Rather than guess at fixed pixel
// coordinates from a screenshot we don't have, every region is an
// ini-configurable rectangle in the SOURCE frame (fractions 0..1, so it is
// resolution-independent) magnified into a destination rectangle placed
// relative to the drawn image (also fractions 0..1, so it tracks
// preserve_aspect/image_scale_percent letterboxing automatically). This
// reuses the exact GPU blit pipeline shipped for the bilinear resample (test
// 3) -- same shaders, same sampler, just re-pointed at a cropped source UV
// rect and drawn a second time on top without clearing. GPU path only: if
// gpu_bilinear_resample=0 or the pipeline failed to come up, HUD magnify
// silently does nothing (logged once) rather than trying to reimplement
// cropping in the CPU nearest-neighbor fallback.
struct HudMagnifyRegion {
    float srcX = 0.0f, srcY = 0.0f, srcW = 0.0f, srcH = 0.0f; // fraction of source frame
    float dstX = 0.0f, dstY = 0.0f, dstW = 0.0f, dstH = 0.0f; // fraction of the drawn rect
};
static bool g_hudMagnifyEnabled = false;
static std::vector<HudMagnifyRegion> g_hudMagnifyRegions;

// --- per-eye pose tagging ----------------------------------------------------
// Alternate-eye stereo means one eye's image is always a game frame (~33 ms)
// old. Submitting BOTH views with the live head pose tells the compositor the
// stale image is current, so it reprojects neither -- and the old eye visibly
// drags behind the fresh one through every swing. That is the ghost.
//
// The fix is the discipline every alternate-eye implementation needs: stamp
// each projection view with the head pose ITS image was actually rendered
// from. The runtime then knows exactly how stale that eye is and timewarps it
// forward to the current pose for free, which is what lets stereo survive
// motion instead of having to be traded away for mono.
//
// ROTATION ONLY. Timewarp has no depth buffer from us and cannot re-parallax a
// stale image, so translation staleness (walking) still has to be handled by
// the motion hold in camera_write_hook.cpp. The two fixes cover different
// halves of the same problem; neither replaces the other.
static XrPosef g_eyeSubmitPose[2] = {};
static bool    g_haveEyeSubmitPose[2] = { false, false };
static bool    g_stereoPoseTagging = true;

// --- per-eye FRAME VIEW RECORDS (2026-09-22) ---------------------------------
// What the pose tagging above was always meant to be, done at the right place.
//
// THE BUG IT FIXES. CaptureGameFrameToMonoTexture runs on THIS thread, once
// per XR frame (72-90 Hz), and re-copies the newest game frame every time. The
// old latch took g_xrViews -- the headset pose for THE XR FRAME DOING THE
// COPY -- and re-applied it on every one of those copies. So the newest image
// was always labelled "current" and the compositor never reprojected it: the
// world rode along with your head between game frames and then snapped, at
// 15 Hz per eye, each eye snapping at a different moment. That is "choppy
// when looking around" and "the same view twice with an offset". The motion
// controls inherited it, because the picture under the laser was never where
// the compositor believed it was.
//
// The record comes from the game thread instead, attached to the pixels at
// flip: the head pose the rotation hook actually steered that frame's camera
// with. It does not change however many XR frames re-show the image, which is
// what lets the runtime's timewarp carry a stale eye forward correctly.
static FrameViewRecord g_eyeRec[2];      // the record behind each eye's current image
static FrameViewRecord g_latestRec;      // newest valid record captured (either eye)
static bool g_fovClaimFromFrame = true;  // [openxr] fov_claim_from_frame
static bool g_stickTurnCorrection = true;// [openxr] stereo_stick_turn_correction

// q = yaw(Y) * pitch(X) * roll(Z): the exact inverse of QuaternionToYawPitchRoll
// below, and the same order the game applies its camera angles in.
static XrQuaternionf QuatFromYawPitchRoll(float yaw, float pitch, float roll) {
    const float cy = std::cos(yaw * 0.5f), sy = std::sin(yaw * 0.5f);
    const float cp = std::cos(pitch * 0.5f), sp = std::sin(pitch * 0.5f);
    const float cr = std::cos(roll * 0.5f), sr = std::sin(roll * 0.5f);
    // qy * qx
    const float ax = cy * sp, ay = sy * cp, az = -sy * sp, aw = cy * cp;
    // (qy*qx) * qz
    XrQuaternionf q;
    q.x = ax * cr + ay * sr;
    q.y = ay * cr - ax * sr;
    q.z = aw * sr + az * cr;
    q.w = aw * cr - az * sr;
    return q;
}

static XrVector3f QuatRotate(const XrQuaternionf& q, const XrVector3f& v) {
    // v' = v + 2w(u x v) + 2(u x (u x v)), u = (x,y,z)
    const float tx = 2.0f * (q.y * v.z - q.z * v.y);
    const float ty = 2.0f * (q.z * v.x - q.x * v.z);
    const float tz = 2.0f * (q.x * v.y - q.y * v.x);
    XrVector3f r;
    r.x = v.x + q.w * tx + (q.y * tz - q.z * ty);
    r.y = v.y + q.w * ty + (q.z * tx - q.x * tz);
    r.z = v.z + q.w * tz + (q.x * ty - q.y * tx);
    return r;
}

static ViewPoseF ToViewPoseF(const XrPosef& p) {
    ViewPoseF v;
    v.qx = p.orientation.x; v.qy = p.orientation.y; v.qz = p.orientation.z; v.qw = p.orientation.w;
    v.px = p.position.x; v.py = p.position.y; v.pz = p.position.z;
    return v;
}

// The pose eye `eye`'s image is submitted with.
//
// Orientation: the head orientation that image was drawn for, rebuilt from the
// record. Two things differ from "the headset pose at that moment", both on
// purpose:
//   - ROLL is the reference roll, because the game camera has no roll (unless
//     write_roll=1). Labelling a level picture with a tilted head made the
//     horizon tilt WITH your head; now the compositor keeps it level.
//   - STICK TURNING. The game's own base yaw moves when you turn with the
//     stick, which rotates the world under you. The newer eye already shows
//     that; the older eye still shows the world where it was a frame ago. The
//     correction re-labels the older eye by exactly the base yaw that has
//     happened since, so the compositor rotates it into agreement instead of
//     showing two worlds a few degrees apart.
// Position: the sampled eye position, carried round the head centre by the
// same re-labelling so the two eyes stay a proper stereo pair.
static XrPosef PoseFromRecord(const FrameViewRecord& r, int eye) {
    float yaw = r.yawEq, pitch = r.pitchEq;
    if (g_stickTurnCorrection && g_latestRec.valid) {
        auto wrap = [](int32_t u) { u %= 4096; if (u > 2048) u -= 4096; if (u < -2048) u += 4096; return u; };
        const int32_t dY = wrap(r.baseYaw - g_latestRec.baseYaw);
        const int32_t dP = wrap(r.basePitch - g_latestRec.basePitch);
        // More than 45 degrees in one frame is a cut or a mode change, not a
        // turn; leave that eye alone for the frame it takes to refresh.
        if (dY > -512 && dY < 512) yaw += (float)r.kYaw * (float)dY * (2.0f * PI / 4096.0f);
        if (dP > -512 && dP < 512) pitch += (float)r.kPitch * (float)dP * (2.0f * PI / 4096.0f);
    }
    const XrQuaternionf qTag = QuatFromYawPitchRoll(yaw, pitch, r.rollEq);

    const ViewPoseF& e0 = r.eye[0];
    const ViewPoseF& e1 = r.eye[1];
    const ViewPoseF& me = r.eye[eye & 1];
    const XrVector3f centre{ (e0.px + e1.px) * 0.5f, (e0.py + e1.py) * 0.5f, (e0.pz + e1.pz) * 0.5f };
    const XrQuaternionf qSampleInv{ -me.qx, -me.qy, -me.qz, me.qw };
    const XrVector3f offW{ me.px - centre.x, me.py - centre.y, me.pz - centre.z };
    const XrVector3f offL = QuatRotate(qSampleInv, offW);
    const XrVector3f offT = QuatRotate(qTag, offL);

    XrPosef p;
    p.orientation = qTag;
    p.position = { centre.x + offT.x, centre.y + offT.y, centre.z + offT.z };
    return p;
}

// --- FOV-exact submission ----------------------------------------------------
// The game renders a fixed ~4:3 frustum. We letterbox it into a square-ish eye
// buffer and then tag the layer with the RUNTIME's per-eye FOV -- which claims
// that 4:3 picture fills the headset's whole canted, asymmetric frustum. It
// does not, and that mismatch is a pure magnification error: a prime suspect
// for the world reading wrong-sized, and entirely separate from position_scale
// (which sets the stereo baseline).
//
// Set submitted_hfov_deg to the game's real horizontal render FOV and the
// layer is instead tagged with a SYMMETRIC frustum built from exactly that,
// with the sub-image narrowed to the pixels actually drawn. 0 = off, previous
// behaviour. The tell for a bad claim is that YAW warps (the world shears as
// you turn) while pitch stays clean.
static float   g_submittedHfovDeg = 0.0f;
static int32_t g_drawRectX = 0, g_drawRectY = 0;
static int32_t g_drawRectW = 0, g_drawRectH = 0;

// --- image framing -----------------------------------------------------------
// The game renders 4:3 (640x480, aspect 1.333). The per-eye swapchain is
// 2496x2688 (aspect 0.928). Stretching one to the other -- which is what this
// did -- makes everything 44% too tall, across the whole headset FOV. That is a
// large part of why the picture reads as looming and uncomfortably close.
static bool g_preserveAspect = true;
// Shrink the fitted image inside the eye, as a percentage. 100 = as large as
// aspect-correct fitting allows. Lower values push the picture away from your
// face, which is what menus and codec calls need.
static int  g_imageScalePercent = 100;

// --- CROP-TO-FILL (NEW 2026-08-16, short-term goal) --------------------------
// "Remove the letterbox bars." Aspect-correct fitting leaves black bands
// because the SOURCE is 4:3 (1.333 wide) and the Quest 2's per-eye buffer is
// TALLER than it is wide (2496x2688 = 0.929). Fitting inside matches width
// and leaves ~15% of the eye buffer black above and below.
//
// The honest thing to say about filling it: on this headset the bars are
// vertical space, so filling them means scaling the picture up until it is
// tall enough -- and then throwing away the sides. At 100% fill the visible
// horizontal fraction of the game's frame is dstAspect/srcAspect = 0.929 /
// 1.333 = 0.696, i.e. THIRTY PERCENT OF YOUR HORIZONTAL VIEW IS CUT OFF.
// That is why this is not simply "on": at the stock 53.1 deg render FOV it
// would leave 38 deg of usable horizontal FOV, which is worse than the
// letterbox it removed. It only becomes a good trade once fov_clip_distance
// has been pushed wide, which is why the handoff listed this as blocked on
// the FOV work (now shipped at 240 / 67.4 deg -> 49.8 deg cropped at 100%).
//
// So this is a PERCENTAGE, not a switch: 0 = today's letterbox, 100 = full
// bleed, and everything between is a partial crop. The startup log prints the
// resulting effective horizontal FOV next to the rendered one so the trade is
// a number instead of a feeling.
static int g_cropToFillPercent = 0;

// The fraction of the source frame actually visible after cropping, published
// for the FOV claim. Cropping the sides changes what the layer is showing, so
// a claim computed from the RENDERED fov would over-claim by exactly this
// factor and shear the world as you turn -- the same failure mode the
// submitted_hfov_deg comment warns about. 1.0 = nothing cropped.
static float g_visibleSrcFracX = 1.0f;
static float g_visibleSrcFracY = 1.0f;

// The image-scale shrink actually applied to the drawn rect this frame, 0..1.
//
// This has to reach the FOV claim or image_scale_percent silently stops
// working the moment submitted_hfov_deg is non-zero: the claim is built from
// the drawn rect's ASPECT, which scaling preserves, and the sub-image is
// narrowed to the drawn rect -- so the content would subtend exactly the same
// angle no matter how small the rect got. Claiming a proportionally smaller
// frustum is what makes the picture genuinely sit further away, which is the
// entire point of the knob and of ui_image_scale_percent built on top of it.
static float g_drawScaleFactor = 1.0f;

// --- 2D UI DISTANCE (NEW 2026-08-16, short-term goal) ------------------------
// "Push menus, codec and 2D UI further back." Those are flat images with text
// on them; gameplay is a world. They want different framing, and until now
// both got image_scale_percent.
//
// When the game is NOT in first person -- menus, codec calls, cutscenes, the
// briefing screens -- the framing switches to ui_image_scale_percent and
// ui_crop_to_fill_percent instead. A smaller scale is literally a smaller
// virtual screen with black around it, which is what makes text comfortable
// to read in a headset, and cropping is forced off there by default because
// cutting the sides off a menu can cut off the menu.
//
// The switch is blended over ui_transition_frames rather than snapped: MGS1
// enters codec calls and menus abruptly, and an instant change of image size
// in a headset reads as the world lurching toward or away from you.
static bool g_uiFramingEnabled = true;
static int  g_uiImageScalePercent = 80;
static int  g_uiCropToFillPercent = 0;
static int  g_uiTransitionFrames = 8;
// 0.0 = fully gameplay framing, 1.0 = fully UI framing. Moves by
// 1/g_uiTransitionFrames per captured frame toward whichever the current
// state calls for.
static float g_uiFramingBlend = 0.0f;

// --- NATIVE MODE: the world-locked virtual screen ----------------------------
// In VR mode the game image is painted into the eye buffers and submitted as a
// projection layer, so it is head-locked by construction: turn your head and
// the picture comes with you, because it IS your eyes. That is right for VR and
// wrong for "the game as it shipped, on a screen" -- a screen that follows your
// head is a blindfold with a picture on it.
//
// So native mode submits the same rendered image as a QUAD layer instead, at a
// fixed pose in the play space. Turn your head and the screen stays where it
// is, like a monitor on a desk.
//
// IT REUSES THE EYE-0 SWAPCHAIN rather than creating its own, and that is a
// deliberate choice with history behind it. vr_aim.h documents a permanent
// frame-rate collapse (30 fps -> 3-15 fps, surviving a return to third person,
// with a constant ~325 ms DirectDraw Lock() as its fingerprint) traced to quad
// layer traffic saturating the Virtual Desktop compositor on this exact Quest 2
// setup, and its conclusion is "EXACTLY ONE LAYER, ALWAYS". A second swapchain
// plus a second set of acquire/wait/release calls per frame is the shape of the
// thing that caused it. Reusing eye 0 means native mode does strictly LESS work
// than VR mode: one eye rendered instead of two, one quad layer submitted
// instead of a two-view projection layer. If anything it should be faster.
static bool  g_nativeScreenEnabled = true;
// [openxr] menu_as_screen: the PC port's pause / LOAD / SAVE / options menus
// (captured off the primary by ddraw_hook.cpp) are flat 2D pictures full of
// text. Spread across a 106-degree head-locked view they are unreadable at the
// edges, so while one is open it goes on the same world-locked virtual screen
// native mode uses, placed in front of you when the menu opens.
static bool  g_menuAsScreen = true;
// [openxr] codec_as_screen: while the codec is open (in VR mode) the picture
// goes onto the same world-locked virtual screen, placed in front of you when
// the call opens, and the VR view returns the moment it closes.
// codec_exit_hold_ms keeps the screen that much longer after it closes (0 =
// switch back on the very frame the game closes the codec).
static bool  g_codecAsScreen = true;
static int   g_codecExitHoldMs = 0;
// Codec open now, or closed less than codec_exit_hold_ms ago. Logs each edge.
static bool CodecScreenWanted() {
    if (!g_codecAsScreen) return false;
    static bool s_prev = false;
    static ULONGLONG s_closedAt = 0;
    const bool open = IsCodecOpen();
    if (open != s_prev) {
        s_prev = open;
        if (!open) s_closedAt = GetTickCount64();
        DebugLogger::LogFormat("Codec %s", open ? "OPEN -- showing it on the virtual screen in front of you"
                                                 : "closed -- back to the VR view");
    }
    if (open) return true;
    return g_codecExitHoldMs > 0 && s_closedAt && GetTickCount64() - s_closedAt < (ULONGLONG)g_codecExitHoldMs;
}
// [openxr] sharpen_percent (0..100): contrast-limited sharpening in the blit.
static float g_sharpenAmount = 0.40f;
// [openxr] fallback_debug_colors: before the first game frame arrives each eye
// used to be cleared RED (left) / GREEN (right) -- a diagnostic for "capture is
// not working". It is still logged either way; the colours are now opt-in.
static bool  g_fallbackDebugColors = false;
static float g_nativeScreenDistanceM = 2.5f;
static float g_nativeScreenWidthM = 3.2f;
static bool  g_nativeScreenLockRoll = true;
static XrCompositionLayerQuad g_nativeScreenQuad{ XR_TYPE_COMPOSITION_LAYER_QUAD };
static XrPosef g_nativeScreenPose{};
static bool    g_nativeScreenPosed = false;

extern void UpdateFallbackXInput();
extern void StartFallbackInputThread();
extern bool IsFpvLookActive();
extern void TriggerFpvDoubleTap();
// When the USER last asked for an FPV toggle (R3 double-press or the startup
// auto-trigger), as a GetTickCount() stamp. 0 = never. The auto-restore below
// uses this to tell "the player chose to leave first person" apart from "the
// game yanked the camera away from them", which are indistinguishable from the
// FPV flag alone.
extern DWORD GetLastFpvToggleRequestTick();
extern void InitOpenXRInput();
extern void UpdateOpenXRInput(XrTime predictedTime);

static float g_fallbackLookSensitivity = 14.0f;
static bool g_overlayEnabled = true;
static bool g_enableOpenXrSession = false;
static bool g_enableHmdLook = false;
static bool g_enableRealCameraWrite = false; // defaults off -- see mgs1_vr_config.ini comment
static bool g_useIncrementalCameraWrite = true; // new test: small per-frame steps instead of absolute jumps
static int16_t g_maxCameraStepPerFrame = 40; // ~3.5 degrees/frame at 90fps -- tune if too slow/fast
static float g_hmdLookSensitivityYaw = 900.0f;
static float g_hmdLookSensitivityPitch = 900.0f;
static bool g_forceFpvOnStart = false; // safe default: see mgs1_vr_config.ini comment for why this defaults off
static bool g_useCameraWriteHook = true;  // in-frame camera rotation hook (camera_write_hook.cpp)
static bool g_haveSeenHealthyCapture = false; // set once a capture with real (non-negligible) content is seen
static bool g_autoFpvTriggered = false;
static DWORD g_readyConditionsMetTick = 0;

// --- first-person maintenance ------------------------------------------------
// MGS1 drops you out of first person on its own, repeatedly: every cutscene,
// every codec call, climbing out of a vent, most room transitions. The first
// headset session logged seven spontaneous `REAL FPV FLAG CHANGED: now=0`
// events in ten minutes, each one requiring another manual R3 double-press.
//
// The mod's answer used to be to REMEMBER that the player probably wanted first
// person and put them back on a retry budget. Since 2026-08-23 there is nothing
// to remember: the player picks a view mode with R3, the camera hook owns it,
// and this loop just keeps MGS1's flag in agreement with that choice. See the
// long note at the maintenance loop itself for why the guessing and the budget
// both had to go, and what in the logs said so.
//
// g_fpvRestoreDelayMs is now the poll interval between requests rather than a
// one-shot settle delay. There is no attempt cap and no "give up" state.
static bool  g_fpvAutoRestore = true;
static int   g_fpvRestoreDelayMs = 1200;   // gap between requests
static std::atomic<bool> g_fpvRestoreInFlight{ false };

// TriggerFpvDoubleTap() sleeps between the two taps, so it must never run on
// the render thread -- 85 ms of stall per restore would be a visible hitch.
static DWORD WINAPI FpvRestoreThreadProc(LPVOID) {
    TriggerFpvDoubleTap();
    g_fpvRestoreInFlight.store(false);
    return 0;
}

// Fires the auto-FPV double-tap once both the OpenXR session is genuinely
// running/focused (g_xrSessionRunning, set true on entering FOCUSED -- see
// PumpOpenXrEvents) AND we've seen at least one real, non-black captured
// frame (the pixel pipeline itself is confirmed up), PLUS a minimum
// wall-clock settle delay after those conditions are first met.
//
// The settle delay exists because our readiness checks alone weren't
// sufficient -- confirmed by direct comparison in a real session log: the
// auto-trigger fired within ~130ms of FOCUSED being reached and visibly did
// nothing, while a manual R3 double-tap ~18 seconds later (after capture had
// been flowing steadily and the session had been comfortably FOCUSED for a
// while) worked immediately. So "session running + one healthy capture"
// happens well before the game's own input/gesture handling is actually
// ready to process a double-tap, even though it's a reasonable-looking
// readiness signal on our side. Starting with a conservative 5s settle
// delay; increase if it's still unreliable, since an unnecessarily long
// wait is a much smaller cost than another silent failure to diagnose.
static constexpr DWORD kFpvSettleDelayMs = 5000;

static void TriggerAutoFpvIfNeeded() {
    // ...and not while the mod is in native mode. force_fpv_on_start means
    // "start me in first person"; with start_in_vr_mode=0 the player has asked
    // for the opposite, and the mode is the more specific instruction.
    if (!g_forceFpvOnStart || g_autoFpvTriggered || !g_haveSeenHealthyCapture ||
        !g_xrSessionRunning || !IsVrViewModeActive()) {
        return;
    }

    DWORD now = GetTickCount();
    if (g_readyConditionsMetTick == 0) {
        g_readyConditionsMetTick = now;
        DebugLogger::LogFormat("Auto-FPV readiness conditions met -- waiting %lums before firing (settle delay for the game's own input handling to catch up)", kFpvSettleDelayMs);
        return;
    }

    if ((now - g_readyConditionsMetTick) < kFpvSettleDelayMs) {
        return;
    }

    g_autoFpvTriggered = true;
    TriggerFpvDoubleTap();
}
static volatile LONG g_RenderHookSeen = 0;
static int g_FrameCount = 0;
static std::string g_iniPath;

static void ApplyFallbackCameraFromXInput();
static void ApplyHmdLookIfAvailable();
static void PumpOpenXrEvents();
static void TryInitializeOpenXRSession();
static void StartXrFrameThreadIfNeeded();
static void StartRuntimeTickThread();
static bool InitializeOpenXrRenderTargets();
static void CleanupOpenXrRenderTargets();
static bool EnsureMonoCaptureResources(int32_t width, int32_t height);
static bool CaptureGameFrameToMonoTexture();
static void BlitMonoTextureToEyeSwapchain(uint32_t eye, ID3D11Texture2D* dstTexture);

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

static void LoadRuntimeConfig() {
    char modulePath[MAX_PATH] = { 0 };
    GetModuleFileNameA(NULL, modulePath, MAX_PATH);

    std::string gameDir(modulePath);
    auto slash = gameDir.find_last_of("\\/");
    if (slash != std::string::npos) {
        gameDir = gameDir.substr(0, slash);
    }

    g_iniPath = gameDir + "\\mgs1_vr_config.ini";
    LogIniFileState(g_iniPath);

    g_fallbackLookSensitivity = (float)GetPrivateProfileIntA("input", "fallback_look_sensitivity", 14, g_iniPath.c_str());
    g_overlayEnabled = GetPrivateProfileIntA("overlay", "enabled", 1, g_iniPath.c_str()) != 0;
    g_enableOpenXrSession = GetPrivateProfileIntA("openxr", "enable_session", 1, g_iniPath.c_str()) != 0;
    g_enableHmdLook = GetPrivateProfileIntA("openxr", "hmd_look_enabled", 1, g_iniPath.c_str()) != 0;
    g_srgbSwapchain = GetPrivateProfileIntA("openxr", "srgb_swapchain", 1, g_iniPath.c_str()) != 0;
    g_enableRealCameraWrite = GetPrivateProfileIntA("openxr", "enable_real_camera_write", 0, g_iniPath.c_str()) != 0;
    g_useIncrementalCameraWrite = GetPrivateProfileIntA("openxr", "use_incremental_camera_write", 1, g_iniPath.c_str()) != 0;
    g_maxCameraStepPerFrame = (int16_t)GetPrivateProfileIntA("openxr", "max_camera_step_per_frame", 40, g_iniPath.c_str());
    g_hmdLookSensitivityYaw = (float)GetPrivateProfileIntA("openxr", "hmd_look_sensitivity_yaw", 900, g_iniPath.c_str());
    g_hmdLookSensitivityPitch = (float)GetPrivateProfileIntA("openxr", "hmd_look_sensitivity_pitch", 900, g_iniPath.c_str());
    g_forceFpvOnStart = GetPrivateProfileIntA("openxr", "force_fpv_on_start", 0, g_iniPath.c_str()) != 0;
    g_useCameraWriteHook = GetPrivateProfileIntA("openxr", "use_camera_write_hook", 1, g_iniPath.c_str()) != 0;
    g_preserveAspect = GetPrivateProfileIntA("openxr", "preserve_aspect", 1, g_iniPath.c_str()) != 0;
    g_imageScalePercent = GetPrivateProfileIntA("openxr", "image_scale_percent", 100, g_iniPath.c_str());
    if (g_imageScalePercent < 20)  g_imageScalePercent = 20;
    if (g_imageScalePercent > 100) g_imageScalePercent = 100;
    g_gpuBilinearResample = GetPrivateProfileIntA("openxr", "gpu_bilinear_resample", 1, g_iniPath.c_str()) != 0;
    DebugLogger::LogFormat("Image resample: %s", g_gpuBilinearResample ?
        "GPU bilinear (linear-filtered quad draw)" : "CPU nearest-neighbor (legacy fallback)");
    g_enableCameraNopPatches = GetPrivateProfileIntA("openxr", "enable_camera_nop_patches", 0, g_iniPath.c_str()) != 0;

    // --- crop-to-fill + separate 2D-UI framing (NEW 2026-08-16) -------------
    g_cropToFillPercent = GetPrivateProfileIntA("openxr", "crop_to_fill_percent", 0, g_iniPath.c_str());
    if (g_cropToFillPercent < 0)   g_cropToFillPercent = 0;
    if (g_cropToFillPercent > 100) g_cropToFillPercent = 100;

    g_uiFramingEnabled = GetPrivateProfileIntA("openxr", "ui_framing_enabled", 1, g_iniPath.c_str()) != 0;
    g_uiImageScalePercent = GetPrivateProfileIntA("openxr", "ui_image_scale_percent", 80, g_iniPath.c_str());
    if (g_uiImageScalePercent < 20)  g_uiImageScalePercent = 20;
    if (g_uiImageScalePercent > 100) g_uiImageScalePercent = 100;
    g_uiCropToFillPercent = GetPrivateProfileIntA("openxr", "ui_crop_to_fill_percent", 0, g_iniPath.c_str());
    if (g_uiCropToFillPercent < 0)   g_uiCropToFillPercent = 0;
    if (g_uiCropToFillPercent > 100) g_uiCropToFillPercent = 100;
    g_uiTransitionFrames = GetPrivateProfileIntA("openxr", "ui_transition_frames", 8, g_iniPath.c_str());
    if (g_uiTransitionFrames < 1)   g_uiTransitionFrames = 1;
    if (g_uiTransitionFrames > 120) g_uiTransitionFrames = 120;

    {
        // Spell out the actual cost of the requested crop at the FOV we are
        // actually rendering, in degrees, at load time -- so this is a
        // decision made against a number rather than discovered in the
        // headset. The source aspect is not known yet here (it comes from the
        // first captured frame) so 4:3 is assumed for the estimate; the
        // per-frame "Image framing:" line below reports the real one.
        const int clip = GetPrivateProfileIntA("camera_hook", "fov_clip_distance", 0, g_iniPath.c_str());
        const int effectiveClip = (clip != 0) ? clip : 320;
        const double renderedHfov = 2.0 * std::atan(160.0 / (double)effectiveClip) * 180.0 / PI;
        // Estimated visible fraction at 100% fill for a 4:3 source in a
        // 0.929-aspect eye buffer. Recomputed exactly, per frame, below.
        const double fillFrac = 0.929 / 1.3333;
        const double t = (double)g_cropToFillPercent / 100.0;
        const double visible = 1.0 - t * (1.0 - fillFrac);
        const double croppedHfov =
            2.0 * std::atan(std::tan(renderedHfov * 0.5 * PI / 180.0) * visible) * 180.0 / PI;
        DebugLogger::LogFormat(
            "Crop-to-fill: %d%% (0=letterbox, 100=full bleed). At fov_clip_distance=%d the game renders "
            "%.1f deg horizontally; this crop leaves roughly %.1f deg visible. Letterbox bars shrink by "
            "the same proportion. If that number is smaller than you want, lower fov_clip_distance "
            "(wider render) before raising this.",
            g_cropToFillPercent, effectiveClip, renderedHfov, croppedHfov);
        DebugLogger::LogFormat(
            "2D UI framing: enabled=%d ui_image_scale_percent=%d ui_crop_to_fill_percent=%d "
            "transition=%d frames (applied whenever first person is NOT active -- menus, codec, "
            "cutscenes, briefings)",
            g_uiFramingEnabled ? 1 : 0, g_uiImageScalePercent, g_uiCropToFillPercent,
            g_uiTransitionFrames);
    }

    // --- native mode's world-locked virtual screen ---------------------------
    g_nativeScreenEnabled = GetPrivateProfileIntA("openxr", "native_screen_enabled", 1, g_iniPath.c_str()) != 0;
    g_menuAsScreen = GetPrivateProfileIntA("openxr", "menu_as_screen", 1, g_iniPath.c_str()) != 0;
    g_codecAsScreen = GetPrivateProfileIntA("openxr", "codec_as_screen", 1, g_iniPath.c_str()) != 0;
    g_codecExitHoldMs = GetPrivateProfileIntA("openxr", "codec_exit_hold_ms", 0, g_iniPath.c_str());
    if (g_codecExitHoldMs < 0) g_codecExitHoldMs = 0;
    if (g_codecExitHoldMs > 2000) g_codecExitHoldMs = 2000;
    DebugLogger::LogFormat("Codec on the virtual screen: %d (exit hold %d ms)", g_codecAsScreen ? 1 : 0, g_codecExitHoldMs);
    g_fallbackDebugColors = GetPrivateProfileIntA("openxr", "fallback_debug_colors", 0, g_iniPath.c_str()) != 0;
    {
        int sp = (int)GetPrivateProfileIntA("openxr", "sharpen_percent", 40, g_iniPath.c_str());
        if (sp < 0) sp = 0;
        if (sp > 100) sp = 100;
        g_sharpenAmount = (float)sp / 100.0f;
        DebugLogger::LogFormat("Image sharpening: sharpen_percent=%d (0 = off). For the clearest picture also set the "
            "game's GRAPHICS OPTIONS to 1024x768 + High resolution textures.", sp);
    }
    DebugLogger::LogFormat("PC menus on the virtual screen: %d | no-frame fallback: %s",
        g_menuAsScreen ? 1 : 0,
        g_fallbackDebugColors ? "RED/GREEN debug colours" : "black (set fallback_debug_colors=1 for RED/GREEN)");
    g_nativeScreenDistanceM =
        (float)GetPrivateProfileIntA("openxr", "native_screen_distance_cm", 250, g_iniPath.c_str()) / 100.0f;
    if (g_nativeScreenDistanceM < 0.5f)  g_nativeScreenDistanceM = 0.5f;
    if (g_nativeScreenDistanceM > 15.0f) g_nativeScreenDistanceM = 15.0f;
    g_nativeScreenWidthM =
        (float)GetPrivateProfileIntA("openxr", "native_screen_width_cm", 320, g_iniPath.c_str()) / 100.0f;
    if (g_nativeScreenWidthM < 0.3f)  g_nativeScreenWidthM = 0.3f;
    if (g_nativeScreenWidthM > 30.0f) g_nativeScreenWidthM = 30.0f;
    g_nativeScreenLockRoll = GetPrivateProfileIntA("openxr", "native_screen_lock_roll", 1, g_iniPath.c_str()) != 0;
    {
        // The apparent size, which is the number anyone actually wants when
        // choosing these two. 3.2 m wide at 2.5 m away subtends about 65
        // degrees -- roughly a very large TV from a sofa.
        const double hdeg = 2.0 * std::atan((g_nativeScreenWidthM * 0.5) / g_nativeScreenDistanceM)
                          * 180.0 / PI;
        DebugLogger::LogFormat(
            "Native-mode virtual screen: enabled=%d, %.2f m wide at %.2f m (subtends %.0f deg), "
            "upright=%d. World-locked: it stays put when you turn your head. Recentred on entering "
            "native mode. If enabled=0, native mode falls back to the head-locked projection layer.",
            g_nativeScreenEnabled ? 1 : 0, g_nativeScreenWidthM, g_nativeScreenDistanceM, hdeg,
            g_nativeScreenLockRoll ? 1 : 0);
    }

    // --- HUD magnification insets -------------------------------------------
    // Short-term goal, 2026-08-15: shrink-to-headset-FOV makes MGS1's HUD
    // hard to read. Each region is defined by two rectangles, both as
    // fractions 0..1 (so this works at any capture/eye resolution without
    // retuning): "src" crops a corner of the game's native frame (where a
    // HUD element lives), "dst" places the magnified result somewhere on
    // the drawn image. Region count is read explicitly rather than probed,
    // so a bad/missing key just yields 0 regions instead of silently
    // stopping partway through a longer list.
    //
    // No default regions are shipped -- we don't have a reference screenshot
    // of MGS1's actual HUD layout to hardcode correct coordinates from, and a
    // wrong guess magnifying empty background would be worse than nothing.
    // The ini ships this OFF with commented example region keys and
    // instructions for sizing them against a screenshot from an actual play
    // session (see mgs1_vr_config.ini [hud_magnify]).
    g_hudMagnifyEnabled = GetPrivateProfileIntA("hud_magnify", "enabled", 0, g_iniPath.c_str()) != 0;
    g_hudMagnifyRegions.clear();
    int hudRegionCount = GetPrivateProfileIntA("hud_magnify", "region_count", 0, g_iniPath.c_str());
    if (hudRegionCount < 0) hudRegionCount = 0;
    if (hudRegionCount > 8) hudRegionCount = 8; // sanity cap, not a real limit anyone should hit
    for (int i = 1; i <= hudRegionCount; ++i) {
        char srcKey[32], dstKey[32];
        sprintf_s(srcKey, sizeof(srcKey), "region%d_src", i);
        sprintf_s(dstKey, sizeof(dstKey), "region%d_dst", i);
        char srcBuf[128] = {}, dstBuf[128] = {};
        GetPrivateProfileStringA("hud_magnify", srcKey, "", srcBuf, sizeof(srcBuf), g_iniPath.c_str());
        GetPrivateProfileStringA("hud_magnify", dstKey, "", dstBuf, sizeof(dstBuf), g_iniPath.c_str());
        HudMagnifyRegion region{};
        int srcParsed = sscanf_s(srcBuf, "%f,%f,%f,%f", &region.srcX, &region.srcY, &region.srcW, &region.srcH);
        int dstParsed = sscanf_s(dstBuf, "%f,%f,%f,%f", &region.dstX, &region.dstY, &region.dstW, &region.dstH);
        if (srcParsed != 4 || dstParsed != 4 || region.srcW <= 0.0f || region.srcH <= 0.0f ||
            region.dstW <= 0.0f || region.dstH <= 0.0f) {
            DebugLogger::LogFormat(
                "HUD magnify: region%d malformed or incomplete (src='%s' dst='%s') -- skipped. "
                "Expected 'x,y,w,h' fractions (0..1) for both %s and %s.",
                i, srcBuf, dstBuf, srcKey, dstKey);
            continue;
        }
        g_hudMagnifyRegions.push_back(region);
        DebugLogger::LogFormat(
            "HUD magnify: region%d src=(%.3f,%.3f,%.3f,%.3f) dst=(%.3f,%.3f,%.3f,%.3f)",
            i, region.srcX, region.srcY, region.srcW, region.srcH,
            region.dstX, region.dstY, region.dstW, region.dstH);
    }
    DebugLogger::LogFormat("HUD magnify: enabled=%d regions_loaded=%zu (GPU-path only; no effect if "
        "gpu_bilinear_resample=0 or the GPU blit pipeline failed to come up)",
        g_hudMagnifyEnabled ? 1 : 0, g_hudMagnifyRegions.size());

    // --- first-person maintenance -------------------------------------------
    g_fpvAutoRestore = GetPrivateProfileIntA("openxr", "fpv_auto_restore", 1, g_iniPath.c_str()) != 0;
    g_fpvRestoreDelayMs = GetPrivateProfileIntA("openxr", "fpv_restore_delay_ms", 1200, g_iniPath.c_str());
    if (g_fpvRestoreDelayMs < 200) g_fpvRestoreDelayMs = 200;
    DebugLogger::LogFormat(
        "First-person maintenance: enabled=%d, one request every %d ms while VR mode is on and the "
        "game is not in first person, and ONLY while the game could accept one (camera update "
        "running, no menu, no cutscene). No retry limit and no give-up state -- fpv_restore_max_retries "
        "and fpv_user_toggle_grace_ms are obsolete and ignored.",
        g_fpvAutoRestore ? 1 : 0, g_fpvRestoreDelayMs);

    // --- stereo submission -------------------------------------------------
    g_stereoPoseTagging = GetPrivateProfileIntA("openxr", "stereo_pose_tagging", 1, g_iniPath.c_str()) != 0;
    g_fovClaimFromFrame = GetPrivateProfileIntA("openxr", "fov_claim_from_frame", 1, g_iniPath.c_str()) != 0;
    g_stickTurnCorrection = GetPrivateProfileIntA("openxr", "stereo_stick_turn_correction", 1, g_iniPath.c_str()) != 0;
    DebugLogger::LogFormat(
        "World lock: fov_claim_from_frame=%d (%s) stereo_stick_turn_correction=%d",
        g_fovClaimFromFrame ? 1 : 0,
        g_fovClaimFromFrame ? "head-driven frames claim exactly the FOV the game drew them with"
                            : "OFF -- head-driven frames use submitted_hfov_deg / runtime FOV, the world will swim",
        g_stickTurnCorrection ? 1 : 0);
    {
        const int raw = GetPrivateProfileIntA("openxr", "submitted_hfov_deg", 0, g_iniPath.c_str());
        if (raw < 0) {
            // AUTO: derive from the FOV we are actually driving the game to
            // render. hfov = 2*atan(160 / clip_distance) -- the same formula
            // camera_write_hook.cpp uses, read from the same ini key, so the
            // claim and the render cannot drift apart by hand-editing one and
            // forgetting the other. That drift is exactly the bug this avoids.
            const int clip = GetPrivateProfileIntA("camera_hook", "fov_clip_distance", 0, g_iniPath.c_str());
            const int effectiveClip = (clip != 0) ? clip : 320;
            g_submittedHfovDeg = (float)(2.0 * std::atan(160.0 / (double)effectiveClip) * 180.0 / PI);
            DebugLogger::LogFormat(
                "Submitted FOV: AUTO from fov_clip_distance=%d -> claiming %.1f deg",
                effectiveClip, g_submittedHfovDeg);
        } else {
            g_submittedHfovDeg = (float)raw;
        }
        if (g_submittedHfovDeg < 0.0f || g_submittedHfovDeg > 179.0f) g_submittedHfovDeg = 0.0f;
    }
    DebugLogger::LogFormat("Stereo submission: pose_tagging=%d submitted_hfov_deg=%.0f (%s)",
        g_stereoPoseTagging ? 1 : 0, g_submittedHfovDeg,
        g_submittedHfovDeg > 0.0f ? "symmetric frustum claim built from the rendered FOV"
                                  : "runtime per-eye FOV claim (legacy)");

    LoadAimConfig();
    LoadWristHudConfig();
    SetCameraWriteHookEnabled(g_useCameraWriteHook);

    DebugLogger::LogFormat("Config loaded: ini=%s, fallback_look_sensitivity=%.1f, overlay=%d, openxr_enable_session=%d, hmd_look_enabled=%d yaw_sens=%.1f pitch_sens=%.1f force_fpv_on_start=%d enable_real_camera_write=%d use_incremental_camera_write=%d max_camera_step_per_frame=%d",
        g_iniPath.c_str(),
        g_fallbackLookSensitivity,
        g_overlayEnabled ? 1 : 0,
        g_enableOpenXrSession ? 1 : 0,
        g_enableHmdLook ? 1 : 0,
        g_hmdLookSensitivityYaw,
        g_hmdLookSensitivityPitch,
        g_forceFpvOnStart ? 1 : 0,
        g_enableRealCameraWrite ? 1 : 0,
        g_useIncrementalCameraWrite ? 1 : 0,
        (int)g_maxCameraStepPerFrame);
}

static void UpdateOverlayWindowTitle(int frame) {
    if (!g_overlayEnabled || frame % 15 != 0) {
        return;
    }

    HWND hwnd = GetForegroundWindow();
    if (!hwnd) {
        return;
    }

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != GetCurrentProcessId()) {
        return;
    }

    uintptr_t base = GetMgsiModuleBase();

    char candidatesStr[160] = {};
    int written = 0;
    for (int i = 0; i < kCandidateCount && written < (int)sizeof(candidatesStr) - 20; ++i) {
        uintptr_t addr = base + g_candidateOffsets[i].offset;
        int16_t value = 0;
        __try {
            value = *reinterpret_cast<volatile int16_t*>(addr);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            value = -1; // unreadable -- shows as -1 rather than crashing the title update
        }
        written += sprintf_s(candidatesStr + written, sizeof(candidatesStr) - written,
            "%s=%d ", g_candidateOffsets[i].label, (int)value);
    }

    char title[256];
    sprintf_s(title, "Metal Gear Solid [XR:%s] [Frame:%d] [%s]",
        (g_xrSessionRunning ? "ACTIVE" : "FALLBACK"), frame, candidatesStr);
    SetWindowTextA(hwnd, title);
}

void VRTickFromRenderer() {
    g_FrameCount++;

    if (InterlockedCompareExchange(&g_RenderHookSeen, 1, 0) == 0) {
        DebugLogger::Log("Renderer tick observed (internal hook or Flip hook)");
    }

    UpdateOverlayWindowTitle(g_FrameCount);

    // Auto-trigger native FPV mode once both the OpenXR session is FOCUSED
    // and a real (non-black) capture has been confirmed -- see
    // TriggerAutoFpvIfNeeded's comment for why this replaced a fixed
    // frame-count guess. Called here too (not just from the FOCUSED event
    // handler) as a retry path: if FOCUSED fires before the first healthy
    // capture arrives, this catches it on a later tick once capture health
    // is confirmed.
    TriggerAutoFpvIfNeeded();

    // Independent, edge-triggered monitor of the REAL FPV memory flag --
    // decoupled from R3/keyboard detection entirely, so we can see exactly
    // when the game's own state actually changes regardless of what our
    // input code thinks happened.
    {
        static bool lastKnownFpvState = false;
        static bool haveLastKnownFpvState = false;
        bool currentFpvState = ReadRealFpvFlag();
        if (!haveLastKnownFpvState || currentFpvState != lastKnownFpvState) {
            DebugLogger::LogFormat("REAL FPV FLAG CHANGED: now=%d (was=%d)", currentFpvState ? 1 : 0, lastKnownFpvState ? 1 : 0);
            lastKnownFpvState = currentFpvState;
            haveLastKnownFpvState = true;
        }
    }

    // ---- FIRST-PERSON MAINTENANCE (rewritten 2026-08-23) --------------------
    // Was "FPV auto-restore": infer from the game's flag whether the player
    // probably still wanted first person, then re-request it on a three-attempt
    // budget. Both halves are now gone, and both for reasons the session logs
    // made concrete.
    //
    // THE INFERENCE IS GONE because there is nothing left to infer. The player
    // chooses the view mode with R3 and it is stored in the camera hook; this
    // loop's only job is to make MGS1's own first-person flag agree with a
    // decision that has already been made. "Did they mean to leave first
    // person?" was always a guess, and the grace-window heuristic that answered
    // it could not tell a deliberate exit from the game revoking the flag half
    // a second after an unrelated toggle.
    //
    // THE BUDGET IS GONE because it was being spent on impossible moments. In
    // the 2026-08-23 log both "gave up after 3 attempts" events fired all three
    // attempts while the log simultaneously recorded `camera update STALLED
    // (1504 flips)` and `(1592 flips)` -- the game was mid-cutscene with its
    // camera update stopped and could not have honoured a request under any
    // circumstances. Worse, giving up also cleared the remembered intent, so
    // the one thing the player definitely wanted was abandoned precisely
    // because the mod had spent its retries on moments that were never
    // candidates.
    //
    // So: ask only when the game could plausibly say yes
    // (FirstPersonRequestIsPlausible() -- camera update running, no modal UI,
    // no cutscene), and otherwise wait, indefinitely and quietly. Waiting costs
    // nothing because the mode is a stored fact rather than a decaying guess.
    if (g_fpvAutoRestore) {
        const DWORD now = GetTickCount();

        // Both VrModeWantsFirstPerson() and FirstPersonRequestIsPlausible()
        // require the camera hook, because both are built on state the hook
        // maintains. With use_camera_write_hook=0 this loop is therefore a
        // silent no-op -- and silence is the one thing a disabled feature must
        // not be, so say it once.
        static bool loggedNoHook = false;
        if (!IsCameraWriteHookEnabled() && !loggedNoHook) {
            loggedNoHook = true;
            DebugLogger::Log(
                "First-person maintenance: inactive -- the camera write hook is off "
                "(use_camera_write_hook=0), so there is no debounced first-person signal and no "
                "cutscene/stall measurement to decide when a request could land. R3 still toggles "
                "the view mode; nothing will re-request first person when the game takes it away.");
        }

        static bool  outageOpen = false;      // VR mode on, first person missing
        static DWORD nextAttemptTick = 0;
        static int   attempts = 0;
        static bool  loggedWaiting = false;

        const bool wants = VrModeWantsFirstPerson();

        if (!wants) {
            if (outageOpen) {
                outageOpen = false;
                // Two very different reasons to stop wanting it, and saying
                // "back" for both would put a false success in the log every
                // time the player simply chose native mode mid-outage.
                if (attempts > 0) {
                    if (IsFpvActive()) {
                        DebugLogger::LogFormat(
                            "First person: back, after %d request(s). VR mode never stopped wanting it.",
                            attempts);
                    }
                    else {
                        DebugLogger::LogFormat(
                            "First person: stopped asking after %d request(s) -- not because the game "
                            "refused, but because it is no longer wanted (native mode, or the camera "
                            "hook went away).", attempts);
                    }
                }
                attempts = 0;
                loggedWaiting = false;
            }
            nextAttemptTick = 0;
        }
        else {
            if (!outageOpen) {
                outageOpen = true;
                attempts = 0;
                loggedWaiting = false;
                nextAttemptTick = now + (DWORD)g_fpvRestoreDelayMs;
                DebugLogger::LogFormat(
                    "First person: VR mode is on but the game is not in first person (cutscene, vent, "
                    "room change or menu). Will ask for it back as soon as the game is in a state that "
                    "can accept the request -- no retry limit, because the mode is what you chose, not "
                    "something to be guessed at.");
            }

            if (!g_fpvRestoreInFlight.load() && nextAttemptTick != 0 &&
                (long)(now - nextAttemptTick) >= 0) {

                // DON'T RACE THE PLAYER'S OWN TOGGLE. R3 sends a double-tap
                // and returns immediately; the game's flag takes a moment to
                // move and our debounce takes six frames more to believe it.
                // A maintenance request landing inside that window would send a
                // SECOND double-tap and toggle first person straight back off
                // -- turning a working button press into a no-op that looks
                // like the mod fighting itself.
                const DWORD lastUserToggle = GetLastFpvToggleRequestTick();
                if (lastUserToggle != 0 && (now - lastUserToggle) < 2000u) {
                    nextAttemptTick = lastUserToggle + 2000u;
                }
                else if (!FirstPersonRequestIsPlausible()) {
                    // Not a failed attempt -- a moment that was never a
                    // candidate. Re-check soon and count nothing.
                    nextAttemptTick = now + 250;
                    if (!loggedWaiting) {
                        loggedWaiting = true;
                        DebugLogger::Log(
                            "First person: holding the request -- the game's camera update is stopped "
                            "or a menu owns the screen, so it cannot take one right now. This is a wait, "
                            "not a failure, and nothing is being spent.");
                    }
                }
                else {
                    loggedWaiting = false;
                    attempts++;
                    nextAttemptTick = now + (DWORD)g_fpvRestoreDelayMs;
                    g_fpvRestoreInFlight.store(true);
                    // First three, then every twentieth. A genuinely stuck
                    // state should stay visible in the log without becoming
                    // the log.
                    if (attempts <= 3 || (attempts % 20) == 0) {
                        DebugLogger::LogFormat(
                            "First person: requesting it back (attempt %d, no limit).", attempts);
                    }
                    HANDLE h = CreateThread(nullptr, 0, FpvRestoreThreadProc, nullptr, 0, nullptr);
                    if (h) {
                        CloseHandle(h);
                    }
                    else {
                        g_fpvRestoreInFlight.store(false);
                        DebugLogger::Log("First person: failed to create the request thread");
                    }
                }
            }
        }
    }


    // F9 USED TO start the August rotation candidate WRITE-test here. Removed
    // 2026-09-28: F9 is now the snapshot key (ddraw_hook.cpp), and pressing it
    // also fired this test, which wrote 16384 into mgsi.exe+4FBACE (live game
    // data) and crashed the game at the heliport. The test itself is obsolete
    // -- the camera addresses were settled by disassembly -- so it is no
    // longer reachable from any key.

    // F10: passive watch mode for the focused candidate cluster (no writes,
    // just logs live values once/sec). Press again while running to stop early.
    static bool f10WasDown = false;
    bool f10IsDown = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
    if (f10IsDown && !f10WasDown) {
        if (g_watchModeRunning) {
            DebugLogger::Log("F10 pressed: stopping watch mode early");
            g_watchModeRunning = false;
        }
        else {
            DebugLogger::Log("F10 pressed: requesting watch mode");
            g_watchModeRequested = true;
        }
    }
    f10WasDown = f10IsDown;
    StartWatchModeIfRequested();

    // F11: fast, narrow watch mode (just 2 candidates, 10x/sec). Press again while running to stop early.
    static bool f11WasDown = false;
    bool f11IsDown = (GetAsyncKeyState(VK_F11) & 0x8000) != 0;
    if (f11IsDown && !f11WasDown) {
        if (g_fastWatchRunning) {
            DebugLogger::Log("F11 pressed: stopping fast watch mode early");
            g_fastWatchRunning = false;
        }
        else {
            DebugLogger::Log("F11 pressed: requesting fast watch mode");
            g_fastWatchRequested = true;
        }
    }
    f11WasDown = f11IsDown;
    StartFastWatchIfRequested();

    if (g_enableOpenXrSession) {
        TryInitializeOpenXRSession();
        PumpOpenXrEvents();
        StartXrFrameThreadIfNeeded();
    }

    if (!g_enableOpenXrSession || !g_xrSessionReady || !g_xrSessionRunning) {
        ApplyFallbackCameraFromXInput();
        return;
    }

    ApplyHmdLookIfAvailable();
}

static void ApplyFallbackCameraFromXInput() {
    static bool warned = false;
    if (!warned) {
        DebugLogger::Log("Fallback memory camera override disabled; using native FPV mouse-look path");
        warned = true;
    }
}

static void ApplyHmdLookIfAvailable() {
    // Prefer the real, confirmed game-memory FPV flag over the mod's own
    // guessed local state (IsFpvLookActive(), which just assumes its sent
    // double-tap worked). Falls back to the guessed flag only if the real
    // read is unavailable for some reason.
    bool fpvActive = g_forceFpvOnStart ? true : ReadRealFpvFlag();
    if (!g_enableHmdLook || !fpvActive) {
        return;
    }

    float dx = 0.0f;
    float dy = 0.0f;
    {
        std::lock_guard<std::mutex> lock(g_hmdLookMutex);
        dx = g_hmdAccumDeltaX;
        dy = g_hmdAccumDeltaY;
        g_hmdAccumDeltaX = 0.0f;
        g_hmdAccumDeltaY = 0.0f;
    }

    if (dx == 0.0f && dy == 0.0f) {
        return;
    }

    INPUT input = { 0 };
    input.type = INPUT_MOUSE;
    input.mi.dx = (LONG)dx;
    input.mi.dy = (LONG)dy;
    input.mi.dwFlags = MOUSEEVENTF_MOVE;
    SendInput(1, &input, sizeof(INPUT));
}


void ConvertXrPoseToMGS(const XrPosef& xrPose, MGS_SVECTOR& outPos, MGS_SROTATION& outRot) {
    outPos.x = static_cast<int32_t>(xrPose.position.x * 1000.0f);
    outPos.y = static_cast<int32_t>(xrPose.position.y * 1000.0f);
    outPos.z = static_cast<int32_t>(xrPose.position.z * -1000.0f);

    float sinr_cosp = 2 * (xrPose.orientation.w * xrPose.orientation.x + xrPose.orientation.y * xrPose.orientation.z);
    float cosr_cosp = 1 - 2 * (xrPose.orientation.x * xrPose.orientation.x + xrPose.orientation.y * xrPose.orientation.y);
    float roll = std::atan2(sinr_cosp, cosr_cosp);

    float sinp = 2 * (xrPose.orientation.w * xrPose.orientation.y - xrPose.orientation.z * xrPose.orientation.x);
    float pitch = (std::abs(sinp) >= 1) ? std::copysign(PI / 2, sinp) : std::asin(sinp);

    float siny_cosp = 2 * (xrPose.orientation.w * xrPose.orientation.z + xrPose.orientation.x * xrPose.orientation.y);
    float cosy_cosp = 1 - 2 * (xrPose.orientation.y * xrPose.orientation.y + xrPose.orientation.z * xrPose.orientation.z);
    float yaw = std::atan2(siny_cosp, cosy_cosp);

    auto RadToPSX = [](float rad) -> int16_t {
        float normalized = rad + PI;
        return static_cast<int16_t>((normalized / (2 * PI)) * 4096.0f) % 4096;
        };

    outRot.pitch = RadToPSX(pitch);
    outRot.yaw = RadToPSX(yaw);
    outRot.roll = RadToPSX(roll);
}

// FOV address not yet confirmed (not present in the discovered community
// table) -- this function is disabled rather than writing through the old
// placeholder address, which would corrupt random memory. Camera
// position/rotation (the actually-needed piece for 6DOF) ARE confirmed real
// now; FOV correction can be revisited separately once/if a real address is found.
//
// NOTE: not being able to CHANGE the game's render FOV does not stop us
// CLAIMING it correctly on the submitted layer -- see submitted_hfov_deg.
// Those are different problems and only the second one needs an address.
void ApplyFrustumCorrection(const XrFovf& fov) {
    static bool warned = false;
    if (!warned) {
        DebugLogger::Log("ApplyFrustumCorrection: disabled -- no confirmed real FOV address yet (unrelated to the now-confirmed camera position/rotation addresses)");
        warned = true;
    }
}

typedef void(__cdecl* MGS_RenderFrame_t)();
MGS_RenderFrame_t Original_RenderFrame = nullptr;

void __cdecl Hooked_RenderFrame() {
    VRTickFromRenderer();

    if (Original_RenderFrame) {
        Original_RenderFrame();
    }
}

static DWORD WINAPI HookWatchdogThread(LPVOID) {
    Sleep(5000);
    if (InterlockedCompareExchange(&g_RenderHookSeen, 0, 0) == 0) {
        DebugLogger::Log("WARNING: Render hook never fired in first 5 seconds. Running synthetic runtime tick fallback.");
    }
    else {
        DebugLogger::Log("Hook watchdog: render hook is active.");
    }
    return 0;
}

static bool FileExistsA(const std::string& path) {
    DWORD attrs = GetFileAttributesA(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

static std::string ReadRegistryString(HKEY hive, const char* subKey, const char* valueName) {
    char buffer[1024] = { 0 };
    DWORD type = 0;
    DWORD size = sizeof(buffer);
    LONG rc = RegGetValueA(hive, subKey, valueName, RRF_RT_REG_SZ, &type, buffer, &size);
    if (rc != ERROR_SUCCESS) {
        return std::string();
    }
    return std::string(buffer);
}

static std::string ReplaceSuffix(const std::string& source, const std::string& oldSuffix, const std::string& newSuffix) {
    if (source.size() >= oldSuffix.size() && source.compare(source.size() - oldSuffix.size(), oldSuffix.size(), oldSuffix) == 0) {
        return source.substr(0, source.size() - oldSuffix.size()) + newSuffix;
    }
    return std::string();
}

static void AutoConfigureOpenXRRuntime() {
    char forcedPath[1024] = { 0 };
    if (!g_iniPath.empty()) {
        GetPrivateProfileStringA("openxr", "runtime_json", "", forcedPath, sizeof(forcedPath), g_iniPath.c_str());
    }

    if (forcedPath[0] != '\0') {
        std::string configured(forcedPath);
        if (FileExistsA(configured)) {
            SetEnvironmentVariableA("XR_RUNTIME_JSON", configured.c_str());
            DebugLogger::LogFormat("OpenXR runtime forced from config: %s", configured.c_str());
            return;
        }
        DebugLogger::LogFormat("Configured openxr.runtime_json not found: %s", configured.c_str());
    }

    std::string runtime32 = ReadRegistryString(HKEY_LOCAL_MACHINE, "SOFTWARE\\WOW6432Node\\Khronos\\OpenXR\\1", "ActiveRuntime");
    if (!runtime32.empty() && FileExistsA(runtime32)) {
        SetEnvironmentVariableA("XR_RUNTIME_JSON", runtime32.c_str());
        DebugLogger::LogFormat("OpenXR runtime from 32-bit registry: %s", runtime32.c_str());
        return;
    }

    std::string runtime64 = ReadRegistryString(HKEY_LOCAL_MACHINE, "SOFTWARE\\Khronos\\OpenXR\\1", "ActiveRuntime");
    if (!runtime64.empty()) {
        std::string candidate = ReplaceSuffix(runtime64, "steamxr_win64.json", "steamxr_win32.json");
        if (!candidate.empty() && FileExistsA(candidate)) {
            SetEnvironmentVariableA("XR_RUNTIME_JSON", candidate.c_str());
            DebugLogger::LogFormat("OpenXR runtime inferred from 64-bit SteamVR key: %s", candidate.c_str());
            return;
        }

        candidate = ReplaceSuffix(runtime64, "virtualdesktop-openxr.json", "virtualdesktop-openxr-32.json");
        if (!candidate.empty() && FileExistsA(candidate)) {
            SetEnvironmentVariableA("XR_RUNTIME_JSON", candidate.c_str());
            DebugLogger::LogFormat("OpenXR runtime inferred from 64-bit Virtual Desktop key: %s", candidate.c_str());
            return;
        }
    }

    std::vector<std::string> commonPaths = {
        "C:\\Program Files (x86)\\Steam\\steamapps\\common\\SteamVR\\steamxr_win32.json",
        "C:\\Program Files\\Virtual Desktop Streamer\\OpenXR\\virtualdesktop-openxr-32.json"
    };

    for (const auto& path : commonPaths) {
        if (FileExistsA(path)) {
            SetEnvironmentVariableA("XR_RUNTIME_JSON", path.c_str());
            DebugLogger::LogFormat("OpenXR runtime selected from common path: %s", path.c_str());
            return;
        }
    }

    DebugLogger::Log("OpenXR runtime auto-config could not find a valid 32-bit runtime JSON");
}

static bool CreateOpenXrD3D11Device() {
    if (g_xrD3DDevice) {
        return true;
    }

    D3D_FEATURE_LEVEL featureLevels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0
    };

    D3D_FEATURE_LEVEL actualLevel = D3D_FEATURE_LEVEL_10_0;
    HRESULT hr = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        0,
        featureLevels,
        ARRAYSIZE(featureLevels),
        D3D11_SDK_VERSION,
        &g_xrD3DDevice,
        &actualLevel,
        &g_xrD3DContext);

    if (FAILED(hr)) {
        DebugLogger::LogFormat("D3D11CreateDevice failed: 0x%08X", (unsigned)hr);
        return false;
    }

    DebugLogger::LogFormat("D3D11 device created for OpenXR. FeatureLevel=0x%X", (unsigned)actualLevel);
    return true;
}

static bool IsTypelessFormat(DXGI_FORMAT format) {
    return format == DXGI_FORMAT_R8G8B8A8_TYPELESS ||
        format == DXGI_FORMAT_B8G8R8A8_TYPELESS ||
        format == DXGI_FORMAT_B8G8R8X8_TYPELESS;
}

// The non-sRGB twin of an sRGB format (for render-target views that must
// write bytes unchanged). Anything else comes back as it is.
static DXGI_FORMAT NonSrgbView(DXGI_FORMAT f) {
    if (f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) return DXGI_FORMAT_R8G8B8A8_UNORM;
    if (f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) return DXGI_FORMAT_B8G8R8A8_UNORM;
    if (f == DXGI_FORMAT_B8G8R8X8_UNORM_SRGB) return DXGI_FORMAT_B8G8R8X8_UNORM;
    return f;
}

static DXGI_FORMAT SelectSwapchainFormat(const std::vector<int64_t>& runtimeFormats) {
    const DXGI_FORMAT srgbFirst[] = {
        DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
        DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,
        DXGI_FORMAT_R8G8B8A8_UNORM,
        DXGI_FORMAT_B8G8R8A8_UNORM
    };
    const DXGI_FORMAT unormFirst[] = {
        DXGI_FORMAT_R8G8B8A8_UNORM,
        DXGI_FORMAT_B8G8R8A8_UNORM,
        DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
        DXGI_FORMAT_B8G8R8A8_UNORM_SRGB
    };
    const DXGI_FORMAT* preferredFormats = g_srgbSwapchain ? srgbFirst : unormFirst;

    for (int k = 0; k < 4; ++k) {
        const DXGI_FORMAT preferred = preferredFormats[k];
        for (int64_t runtimeFormat : runtimeFormats) {
            if (runtimeFormat == (int64_t)preferred) {
                return preferred;
            }
        }
    }

    if (!runtimeFormats.empty()) {
        return (DXGI_FORMAT)runtimeFormats[0];
    }

    return DXGI_FORMAT_UNKNOWN;
}

static bool EnsureMonoCaptureResources(int32_t width, int32_t height) {
    if (!g_xrD3DDevice || width <= 0 || height <= 0) {
        return false;
    }

    if (g_captureWidth == width && g_captureHeight == height && g_xrMonoCaptureTexture != nullptr) {
        return true;
    }

    if (g_xrMonoCaptureRTV) { g_xrMonoCaptureRTV->Release(); g_xrMonoCaptureRTV = nullptr; }
    if (g_xrMonoCaptureTexture) {
        g_xrMonoCaptureTexture->Release();
        g_xrMonoCaptureTexture = nullptr;
    }
    for (int e = 0; e < 2; ++e) {
        if (g_xrEyeCaptureRTV[e]) { g_xrEyeCaptureRTV[e]->Release(); g_xrEyeCaptureRTV[e] = nullptr; }
        if (g_xrEyeCaptureTexture[e]) { g_xrEyeCaptureTexture[e]->Release(); g_xrEyeCaptureTexture[e] = nullptr; }
        g_haveEyeCapture[e] = false;
        g_haveEyeSubmitPose[e] = false;
        g_eyeRec[e] = FrameViewRecord{};
    }

    g_captureWidth = width;
    g_captureHeight = height;
    g_captureRgba.assign((size_t)width * (size_t)height * 4u, 0);

    // Keep this texture in a concrete, UpdateSubresource-safe typed format
    // (R8G8B8A8_UNORM) rather than trying to match the swapchain's TYPELESS
    // format directly -- D3D11 requires a fully-typed format to know the
    // pixel layout for UpdateSubresource, which this capture path relies on
    // every frame. CopySubresourceRegion between R8G8B8A8_UNORM and
    // R8G8B8A8_TYPELESS is standard, documented-compatible D3D11 behavior
    // (same bit layout, just missing a type on the destination) -- the
    // earlier "src fmt=28 dst fmt=27" log line is expected and not
    // inherently wrong on its own; if the intermittent black screen recurs
    // after this session's apparent fix (Virtual Desktop/headset restart),
    // the real cause is more likely something session/driver-level than
    // this specific format pairing, which is a supported copy path.
    //
    // BindFlags now include RENDER_TARGET alongside SHADER_RESOURCE: the GPU
    // bilinear resample path draws a filtered full-screen quad directly into
    // these textures (see GpuResampleInto below), which needs an RTV. The
    // CPU nearest-neighbor fallback (gpu_bilinear_resample=0) still uses
    // these same textures via UpdateSubresource, which does not care about
    // the extra bind flag.
    D3D11_TEXTURE2D_DESC captureDesc{};
    captureDesc.Width = (UINT)width;
    captureDesc.Height = (UINT)height;
    captureDesc.MipLevels = 1;
    captureDesc.ArraySize = 1;
    captureDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    captureDesc.SampleDesc.Count = 1;
    captureDesc.Usage = D3D11_USAGE_DEFAULT;
    captureDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;

    for (int e = 0; e < 2; ++e) {
        if (g_xrEyeCaptureTexture[e]) { g_xrEyeCaptureTexture[e]->Release(); g_xrEyeCaptureTexture[e] = nullptr; }
        g_haveEyeCapture[e] = false;
        HRESULT hrEye = g_xrD3DDevice->CreateTexture2D(&captureDesc, nullptr, &g_xrEyeCaptureTexture[e]);
        if (SUCCEEDED(hrEye) && g_xrEyeCaptureTexture[e]) {
            g_xrD3DDevice->CreateRenderTargetView(g_xrEyeCaptureTexture[e], nullptr, &g_xrEyeCaptureRTV[e]);
        }
    }

    HRESULT hr = g_xrD3DDevice->CreateTexture2D(&captureDesc, nullptr, &g_xrMonoCaptureTexture);
    if (FAILED(hr)) {
        DebugLogger::LogFormat("CreateTexture2D for mono capture failed: 0x%08X", (unsigned)hr);
        return false;
    }
    g_xrD3DDevice->CreateRenderTargetView(g_xrMonoCaptureTexture, nullptr, &g_xrMonoCaptureRTV);

    return true;
}

// --- GPU bilinear resample pipeline -----------------------------------------
// A minimal, input-layout-free blit: the vertex shader generates a full-
// screen triangle from SV_VertexID alone (no vertex/index buffers needed),
// and the pixel shader samples the source texture with a linear sampler.
// Compiled once, lazily, the first time it is needed; reused for every draw
// thereafter. Rendering into a sub-rectangle of the destination (for
// letterboxing) is done purely with the D3D11 viewport -- the shader always
// draws "full screen" and the viewport clips it to the letterboxed area.
static bool EnsureBlitPipeline() {
    if (g_blitPipelineReady) return true;
    if (!g_xrD3DDevice) return false;

    static const char* kBlitShaderSrc =
        // uvOffset/uvScale let one draw sample a CROPPED sub-rectangle of the
        // source texture instead of the whole thing -- that's what turns this
        // same pipeline into a magnified HUD inset. Default (0,0)/(1,1) is the
        // identity mapping the original full-frame resample always used, so
        // every existing call site is unaffected unless it explicitly sets a
        // crop region via SetBlitCropRegion() below.
        "cbuffer CropParams : register(b0) {\n"
        "    float2 uvOffset;\n"
        "    float2 uvScale;\n"
        "    float  sharpen;\n"
        "    float3 pad0;\n"
        "}\n"
        "struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };\n"
        "VSOut VSMain(uint id : SV_VertexID) {\n"
        "    VSOut o;\n"
        "    float2 uv = float2((id << 1) & 2, id & 2);\n"
        "    o.pos = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.0, 1.0);\n"
        "    o.uv = uvOffset + uv * uvScale;\n"
        "    return o;\n"
        "}\n"
        "Texture2D srcTex : register(t0);\n"
        "SamplerState linearSampler : register(s0);\n"
        // Round 6: optional contrast-limited unsharp mask in SOURCE texel
        // space (the 640x480/1024x768 game frame is upscaled ~3x per eye, so
        // plain bilinear looks soft). Result is clamped to the local min/max
        // so edges get crisper without bright halos. sharpen=0 -> plain blit.
        "float4 PSMain(VSOut i) : SV_TARGET {\n"
        "    float4 c = srcTex.Sample(linearSampler, i.uv);\n"
        "    float tw, th; srcTex.GetDimensions(tw, th);\n"
        "    float2 d = float2(1.0 / tw, 1.0 / th);\n"
        "    float3 n = srcTex.Sample(linearSampler, i.uv + float2(0, -d.y)).rgb;\n"
        "    float3 s = srcTex.Sample(linearSampler, i.uv + float2(0,  d.y)).rgb;\n"
        "    float3 e = srcTex.Sample(linearSampler, i.uv + float2( d.x, 0)).rgb;\n"
        "    float3 w = srcTex.Sample(linearSampler, i.uv + float2(-d.x, 0)).rgb;\n"
        "    float3 mn = min(c.rgb, min(min(n, s), min(e, w)));\n"
        "    float3 mx = max(c.rgb, max(max(n, s), max(e, w)));\n"
        "    float3 r = c.rgb + sharpen * (c.rgb - (n + s + e + w) * 0.25) * 2.0;\n"
        "    return float4(clamp(r, mn, mx), c.a);\n"
        "}\n";

    UINT compileFlags = 0;
#ifdef _DEBUG
    compileFlags |= D3DCOMPILE_DEBUG;
#endif

    ID3DBlob* vsBlob = nullptr;
    ID3DBlob* psBlob = nullptr;
    ID3DBlob* errBlob = nullptr;

    HRESULT hr = D3DCompile(kBlitShaderSrc, strlen(kBlitShaderSrc), "vr_injection_blit.hlsl",
        nullptr, nullptr, "VSMain", "vs_4_0", compileFlags, 0, &vsBlob, &errBlob);
    if (FAILED(hr)) {
        DebugLogger::LogFormat("Blit VS compile failed: 0x%08X%s", (unsigned)hr,
            errBlob ? (const char*)errBlob->GetBufferPointer() : "");
        if (errBlob) errBlob->Release();
        if (vsBlob) vsBlob->Release();
        return false;
    }
    if (errBlob) { errBlob->Release(); errBlob = nullptr; }

    hr = D3DCompile(kBlitShaderSrc, strlen(kBlitShaderSrc), "vr_injection_blit.hlsl",
        nullptr, nullptr, "PSMain", "ps_4_0", compileFlags, 0, &psBlob, &errBlob);
    if (FAILED(hr)) {
        DebugLogger::LogFormat("Blit PS compile failed: 0x%08X%s", (unsigned)hr,
            errBlob ? (const char*)errBlob->GetBufferPointer() : "");
        if (errBlob) errBlob->Release();
        vsBlob->Release();
        if (psBlob) psBlob->Release();
        return false;
    }
    if (errBlob) { errBlob->Release(); errBlob = nullptr; }

    hr = g_xrD3DDevice->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &g_blitVS);
    HRESULT hr2 = g_xrD3DDevice->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &g_blitPS);
    vsBlob->Release();
    psBlob->Release();
    if (FAILED(hr) || FAILED(hr2)) {
        DebugLogger::LogFormat("Blit shader creation failed: vs=0x%08X ps=0x%08X", (unsigned)hr, (unsigned)hr2);
        return false;
    }

    D3D11_SAMPLER_DESC sampDesc{};
    sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sampDesc.MaxLOD = D3D11_FLOAT32_MAX;
    g_xrD3DDevice->CreateSamplerState(&sampDesc, &g_blitLinearSampler);

    D3D11_RASTERIZER_DESC rastDesc{};
    rastDesc.FillMode = D3D11_FILL_SOLID;
    rastDesc.CullMode = D3D11_CULL_NONE;
    rastDesc.DepthClipEnable = TRUE;
    g_xrD3DDevice->CreateRasterizerState(&rastDesc, &g_blitRasterState);

    D3D11_BLEND_DESC blendDesc{};
    blendDesc.RenderTarget[0].BlendEnable = FALSE;
    blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    g_xrD3DDevice->CreateBlendState(&blendDesc, &g_blitBlendState);

    D3D11_DEPTH_STENCIL_DESC depthDesc{};
    depthDesc.DepthEnable = FALSE;
    depthDesc.StencilEnable = FALSE;
    g_xrD3DDevice->CreateDepthStencilState(&depthDesc, &g_blitDepthState);

    if (!g_blitLinearSampler || !g_blitRasterState || !g_blitBlendState || !g_blitDepthState) {
        DebugLogger::Log("Blit pipeline state object creation failed (sampler/raster/blend/depth)");
        return false;
    }

    // Crop constant buffer (uvOffset.xy, uvScale.xy = 4 floats, 16 bytes --
    // already the minimum D3D11 constant-buffer size, no padding needed).
    // DYNAMIC + CPU_ACCESS_WRITE so SetBlitCropRegion() can Map/Unmap it once
    // per draw call cheaply, rather than recreating a buffer every frame.
    D3D11_BUFFER_DESC cbDesc{};
    cbDesc.ByteWidth = sizeof(float) * 8;   // + sharpen, padded to 32 bytes
    cbDesc.Usage = D3D11_USAGE_DYNAMIC;
    cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    HRESULT cbHr = g_xrD3DDevice->CreateBuffer(&cbDesc, nullptr, &g_blitCropCB);
    if (FAILED(cbHr) || !g_blitCropCB) {
        DebugLogger::LogFormat("Blit crop constant buffer creation failed: 0x%08X", (unsigned)cbHr);
        return false;
    }

    g_blitPipelineReady = true;
    DebugLogger::Log("GPU bilinear blit pipeline compiled and ready");
    return true;
}

// Points the blit pipeline's next draw at a sub-rectangle of the source
// texture (uvOffset/uvScale, both in 0..1 source-UV space) instead of the
// whole frame. Call with (0,0,1,1) to restore the identity mapping used by
// the plain full-frame resample.
static void SetBlitCropRegion(float uvOffsetX, float uvOffsetY, float uvScaleX, float uvScaleY) {
    if (!g_blitCropCB || !g_xrD3DContext) return;
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (SUCCEEDED(g_xrD3DContext->Map(g_blitCropCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        float* f = reinterpret_cast<float*>(mapped.pData);
        f[0] = uvOffsetX; f[1] = uvOffsetY; f[2] = uvScaleX; f[3] = uvScaleY;
        f[4] = g_sharpenAmount; f[5] = f[6] = f[7] = 0.0f;
        g_xrD3DContext->Unmap(g_blitCropCB, 0);
    }
    ID3D11Buffer* cbs[1] = { g_blitCropCB };
    g_xrD3DContext->VSSetConstantBuffers(0, 1, cbs);
    g_xrD3DContext->PSSetConstantBuffers(0, 1, cbs);
}

// (Re)creates the small shader-resource-only texture the raw captured game
// frame is uploaded into at ITS NATIVE resolution, ahead of the GPU blit
// sampling it with linear filtering. Cheap to recreate on the rare occasions
// the game's capture resolution actually changes (e.g. FPV vs. menus).
static bool EnsureSourceCaptureTexture(int32_t srcW, int32_t srcH) {
    if (!g_xrD3DDevice || srcW <= 0 || srcH <= 0) return false;
    if (g_sourceCaptureWidth == srcW && g_sourceCaptureHeight == srcH && g_xrSourceCaptureTexture != nullptr) {
        return true;
    }

    if (g_xrSourceCaptureSRV) { g_xrSourceCaptureSRV->Release(); g_xrSourceCaptureSRV = nullptr; }
    if (g_xrSourceCaptureTexture) { g_xrSourceCaptureTexture->Release(); g_xrSourceCaptureTexture = nullptr; }

    D3D11_TEXTURE2D_DESC srcDesc{};
    srcDesc.Width = (UINT)srcW;
    srcDesc.Height = (UINT)srcH;
    srcDesc.MipLevels = 1;
    srcDesc.ArraySize = 1;
    srcDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srcDesc.SampleDesc.Count = 1;
    srcDesc.Usage = D3D11_USAGE_DEFAULT;
    srcDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    HRESULT hr = g_xrD3DDevice->CreateTexture2D(&srcDesc, nullptr, &g_xrSourceCaptureTexture);
    if (FAILED(hr) || !g_xrSourceCaptureTexture) {
        DebugLogger::LogFormat("CreateTexture2D for GPU resample source failed: 0x%08X", (unsigned)hr);
        return false;
    }
    hr = g_xrD3DDevice->CreateShaderResourceView(g_xrSourceCaptureTexture, nullptr, &g_xrSourceCaptureSRV);
    if (FAILED(hr) || !g_xrSourceCaptureSRV) {
        DebugLogger::LogFormat("CreateShaderResourceView for GPU resample source failed: 0x%08X", (unsigned)hr);
        return false;
    }

    g_sourceCaptureWidth = srcW;
    g_sourceCaptureHeight = srcH;
    return true;
}

// Draws the (already-uploaded) source capture texture into dstRTV, filling
// exactly the sub-rectangle [offX,offY,rectW,rectH] of a dstFullW x dstFullH
// destination with a linearly-filtered full-screen-triangle blit. When
// clearFull is set the whole destination is cleared to black first (the
// letterbox surround), matching the CPU path's "black the surround only when
// the geometry actually changes" behavior.
static void GpuResampleInto(ID3D11RenderTargetView* dstRTV, int32_t dstFullW, int32_t dstFullH,
                             int32_t offX, int32_t offY, int32_t rectW, int32_t rectH, bool clearFull) {
    if (!dstRTV || !g_xrD3DContext || !g_xrSourceCaptureSRV) return;

    if (clearFull) {
        const float black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
        g_xrD3DContext->ClearRenderTargetView(dstRTV, black);
    }

    D3D11_VIEWPORT vp{};
    vp.TopLeftX = (float)offX;
    vp.TopLeftY = (float)offY;
    vp.Width = (float)rectW;
    vp.Height = (float)rectH;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;

    ID3D11RenderTargetView* rtvs[1] = { dstRTV };
    g_xrD3DContext->OMSetRenderTargets(1, rtvs, nullptr);
    g_xrD3DContext->RSSetViewports(1, &vp);
    g_xrD3DContext->RSSetState(g_blitRasterState);
    g_xrD3DContext->OMSetBlendState(g_blitBlendState, nullptr, 0xFFFFFFFF);
    g_xrD3DContext->OMSetDepthStencilState(g_blitDepthState, 0);
    g_xrD3DContext->IASetInputLayout(nullptr);
    g_xrD3DContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_xrD3DContext->VSSetShader(g_blitVS, nullptr, 0);
    g_xrD3DContext->PSSetShader(g_blitPS, nullptr, 0);
    // Plain full-frame resample always samples the whole source texture --
    // identity crop, so this is unaffected by whatever HUD-magnify insets
    // may have run before or after it in the same frame.
    SetBlitCropRegion(0.0f, 0.0f, 1.0f, 1.0f);
    ID3D11ShaderResourceView* srvs[1] = { g_xrSourceCaptureSRV };
    g_xrD3DContext->PSSetShaderResources(0, 1, srvs);
    ID3D11SamplerState* samplers[1] = { g_blitLinearSampler };
    g_xrD3DContext->PSSetSamplers(0, 1, samplers);
    g_xrD3DContext->Draw(3, 0);

    // Leave shader resource slot 0 unbound afterward so the same texture is
    // never simultaneously bound as an SRV input and (elsewhere) implicitly
    // treated as a render target -- cheap insurance against D3D11 debug-layer
    // warnings/hazard clears on some drivers.
    ID3D11ShaderResourceView* nullSrv[1] = { nullptr };
    g_xrD3DContext->PSSetShaderResources(0, 1, nullSrv);
}

// Same draw as GpuResampleInto, but samples a CROPPED sub-rectangle of the
// source texture (srcUvX/Y/W/H, fractions 0..1 of the source frame) instead
// of the whole thing, magnified to fill [offX,offY,rectW,rectH] of the
// destination. This is the HUD magnification inset primitive: a small
// corner of the game's native frame (where the radar/life/weapon HUD lives)
// drawn again, larger, on top of the already-drawn full frame. clearFull is
// almost always false here -- clearing would erase the full-frame draw this
// is meant to sit on top of.
static void GpuResampleCropInto(ID3D11RenderTargetView* dstRTV, int32_t dstFullW, int32_t dstFullH,
                                 int32_t offX, int32_t offY, int32_t rectW, int32_t rectH,
                                 float srcUvX, float srcUvY, float srcUvW, float srcUvH,
                                 bool clearFull) {
    if (!dstRTV || !g_xrD3DContext || !g_xrSourceCaptureSRV) return;
    if (rectW <= 0 || rectH <= 0) return;

    if (clearFull) {
        const float black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
        g_xrD3DContext->ClearRenderTargetView(dstRTV, black);
    }

    D3D11_VIEWPORT vp{};
    vp.TopLeftX = (float)offX;
    vp.TopLeftY = (float)offY;
    vp.Width = (float)rectW;
    vp.Height = (float)rectH;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;

    ID3D11RenderTargetView* rtvs[1] = { dstRTV };
    g_xrD3DContext->OMSetRenderTargets(1, rtvs, nullptr);
    g_xrD3DContext->RSSetViewports(1, &vp);
    g_xrD3DContext->RSSetState(g_blitRasterState);
    g_xrD3DContext->OMSetBlendState(g_blitBlendState, nullptr, 0xFFFFFFFF);
    g_xrD3DContext->OMSetDepthStencilState(g_blitDepthState, 0);
    g_xrD3DContext->IASetInputLayout(nullptr);
    g_xrD3DContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_xrD3DContext->VSSetShader(g_blitVS, nullptr, 0);
    g_xrD3DContext->PSSetShader(g_blitPS, nullptr, 0);
    SetBlitCropRegion(srcUvX, srcUvY, srcUvW, srcUvH);
    ID3D11ShaderResourceView* srvs[1] = { g_xrSourceCaptureSRV };
    g_xrD3DContext->PSSetShaderResources(0, 1, srvs);
    ID3D11SamplerState* samplers[1] = { g_blitLinearSampler };
    g_xrD3DContext->PSSetSamplers(0, 1, samplers);
    g_xrD3DContext->Draw(3, 0);

    ID3D11ShaderResourceView* nullSrv[1] = { nullptr };
    g_xrD3DContext->PSSetShaderResources(0, 1, nullSrv);
}

// Cheap nearest-neighbor resample from the raw DirectDraw surface capture
// (whatever resolution the game is actually rendering at) into the
// destination buffer sized to match the OpenXR eye render target. This is a
// "flat screen shown in a headset" resize, not anything perspective-correct
// -- fine for getting a visible image; not a substitute for real per-eye
// rendering.
// Nearest-neighbour resample into a SUB-RECTANGLE of a larger destination,
// leaving everything outside it untouched (the caller blacks the surround once
// per rect change rather than every frame -- a 27 MB memset per frame is not
// free at headset rates).
static void ResampleNearestInto(const uint8_t* src, int32_t srcW, int32_t srcH,
                                uint8_t* dst, int32_t dstStrideW,
                                int32_t offX, int32_t offY,
                                int32_t rectW, int32_t rectH) {
    if (rectW <= 0 || rectH <= 0 || srcW <= 0 || srcH <= 0) return;
    for (int32_t y = 0; y < rectH; ++y) {
        int32_t srcY = (int32_t)(((int64_t)y * srcH) / rectH);
        if (srcY >= srcH) srcY = srcH - 1;
        const uint8_t* srcRow = src + (size_t)srcY * (size_t)srcW * 4u;
        uint8_t* dstRow = dst + ((size_t)(y + offY) * (size_t)dstStrideW + (size_t)offX) * 4u;
        for (int32_t x = 0; x < rectW; ++x) {
            int32_t srcX = (int32_t)(((int64_t)x * srcW) / rectW);
            if (srcX >= srcW) srcX = srcW - 1;
            memcpy(dstRow + (size_t)x * 4u, srcRow + (size_t)srcX * 4u, 4u);
        }
    }
}

// Same as ResampleNearestInto but sampling only a sub-rectangle of the source
// (srcUv*, fractions 0..1), so the CPU fallback can do crop-to-fill too. Kept
// as a separate function rather than adding parameters to the one above,
// because that one is the long-standing confirmed-working path and there is
// no reason to touch it. With identity UVs the two are equivalent.
static void ResampleNearestCropInto(const uint8_t* src, int32_t srcW, int32_t srcH,
                                    uint8_t* dst, int32_t dstStrideW,
                                    int32_t offX, int32_t offY,
                                    int32_t rectW, int32_t rectH,
                                    float srcUvX, float srcUvY, float srcUvW, float srcUvH) {
    if (rectW <= 0 || rectH <= 0 || srcW <= 0 || srcH <= 0) return;

    const int32_t cropX = (int32_t)(srcUvX * (float)srcW + 0.5f);
    const int32_t cropY = (int32_t)(srcUvY * (float)srcH + 0.5f);
    int32_t cropW = (int32_t)(srcUvW * (float)srcW + 0.5f);
    int32_t cropH = (int32_t)(srcUvH * (float)srcH + 0.5f);
    if (cropW < 1) cropW = 1;
    if (cropH < 1) cropH = 1;
    if (cropX + cropW > srcW) cropW = srcW - cropX;
    if (cropY + cropH > srcH) cropH = srcH - cropY;
    if (cropW < 1 || cropH < 1) return;

    for (int32_t y = 0; y < rectH; ++y) {
        int32_t srcY = cropY + (int32_t)(((int64_t)y * cropH) / rectH);
        if (srcY >= srcH) srcY = srcH - 1;
        const uint8_t* srcRow = src + (size_t)srcY * (size_t)srcW * 4u;
        uint8_t* dstRow = dst + ((size_t)(y + offY) * (size_t)dstStrideW + (size_t)offX) * 4u;
        for (int32_t x = 0; x < rectW; ++x) {
            int32_t srcX = cropX + (int32_t)(((int64_t)x * cropW) / rectW);
            if (srcX >= srcW) srcX = srcW - 1;
            memcpy(dstRow + (size_t)x * 4u, srcRow + (size_t)srcX * 4u, 4u);
        }
    }
}

static void ResampleNearest(const uint8_t* src, int32_t srcW, int32_t srcH,
                             uint8_t* dst, int32_t dstW, int32_t dstH) {
    for (int32_t y = 0; y < dstH; ++y) {
        int32_t srcY = (y * srcH) / dstH;
        if (srcY >= srcH) srcY = srcH - 1;
        const uint8_t* srcRow = src + (size_t)srcY * srcW * 4u;
        uint8_t* dstRow = dst + (size_t)y * dstW * 4u;
        for (int32_t x = 0; x < dstW; ++x) {
            int32_t srcX = (x * srcW) / dstW;
            if (srcX >= srcW) srcX = srcW - 1;
            memcpy(dstRow + (size_t)x * 4u, srcRow + (size_t)srcX * 4u, 4u);
        }
    }
}

static bool CaptureGameFrameToMonoTexture() {
    if (!g_xrMonoCaptureTexture || g_captureWidth <= 0 || g_captureHeight <= 0) {
        return false;
    }

    // Skip everything below when the game hasn't produced a new frame since the
    // last upload. This runs on the OpenXR frame thread at headset rate (72 Hz)
    // while the game caps out at 30 -- so most calls were re-copying a ~1.2 MB
    // buffer, rescaling ~4.7 million pixels and re-uploading a texture that had
    // not changed a single byte. The textures keep their contents, so returning
    // early here is invisible in the headset.
    static uint64_t s_lastUploadedSequence = 0;
    static bool     s_haveUploadedOnce = false;
    static int32_t  s_lastCaptureW = -1, s_lastCaptureH = -1;
    const uint64_t thisSequence = GetDdrawFrameSequence();
    // A render-twice pair publishes two frames at once and bumps the sequence
    // by two; the second eye is still queued after the first upload, so the
    // "nothing new" shortcut must not fire while anything is pending.
    if (s_haveUploadedOnce && thisSequence != 0 && thisSequence == s_lastUploadedSequence &&
        s_lastCaptureW == g_captureWidth && s_lastCaptureH == g_captureHeight &&
        GetPendingDdrawFrameCount() == 0) {
        return true;
    }

    int32_t srcW = 0, srcH = 0;
    int capturedEye = -1;

    // CAPTURE SOURCE. 0 = auto (DirectDraw, D3D9 only if DirectDraw has
    // nothing), 1 = force DirectDraw, 2 = force D3D9 backbuffer.
    //
    // This exists to find the pause menu. Confirmed 2026-08-14: during a pause
    // the game keeps flipping and our DirectDraw capture keeps succeeding with
    // 90%+ non-zero pixels -- so the earlier "the menu uses Blt, so capture
    // never fires" theory is WRONG. Capture fires; the menu simply is not in
    // the pixels we get. The D3D9 backbuffer is the other place those pixels
    // could be, and in auto mode it is never consulted because DirectDraw
    // never fails. Forcing source=2 is the test.
    static int s_captureSource = -1;
    if (s_captureSource < 0) {
        s_captureSource = GetPrivateProfileIntA("capture", "source", 0, g_iniPath.c_str());
        if (s_captureSource < 0 || s_captureSource > 2) s_captureSource = 0;
        DebugLogger::LogFormat("Capture source: %d (%s)", s_captureSource,
            s_captureSource == 2 ? "FORCED D3D9 backbuffer" :
            s_captureSource == 1 ? "FORCED DirectDraw" : "auto -- DirectDraw, D3D9 as fallback");
    }

    bool gotFrame = false;
    static bool loggedSource = false;
    FrameViewRecord frameRec;   // stays invalid for the D3D9 path, which has no record

    if (s_captureSource != 2) {
        gotFrame = GetLatestDdrawFrameRgba(g_capturedSourceRgba, srcW, srcH, &capturedEye, &frameRec);
        if (gotFrame && !loggedSource) {
            DebugLogger::Log("CaptureGameFrameToMonoTexture: using DirectDraw surface capture as frame source");
            loggedSource = true;
        }
    }

    if (!gotFrame && s_captureSource != 1) {
        // The D3D9 path has no eye tag -- it does not know about our stereo
        // alternation -- so anything from here is mono and goes to both eyes.
        capturedEye = -1;
        gotFrame = GetLatestD3D9FrameRgba(g_capturedSourceRgba, srcW, srcH);
        if (gotFrame && !loggedSource) {
            DebugLogger::Log("CaptureGameFrameToMonoTexture: using D3D9 backbuffer capture as frame source");
            loggedSource = true;
        }
    }

    if (!gotFrame || srcW <= 0 || srcH <= 0) {
        static bool loggedNoFrame = false;
        if (!loggedNoFrame) {
            DebugLogger::Log("CaptureGameFrameToMonoTexture: no frame captured yet from either DirectDraw or D3D9 "
                "(check for 'Capture attempt' lines from ddraw_hook.cpp or 'Direct3D9 backbuffer capture active' above in the log)");
            loggedNoFrame = true;
        }
        return false;
    }

    // Wrist HUD: take the weapon/item box crops for the wrists from the fresh
    // frame, and (by default) blank those corner boxes in the view itself --
    // they live on your wrists now. No-op outside first-person gameplay.
    if (s_captureSource != 2 && (int)g_capturedSourceRgba.size() >= srcW * srcH * 4 &&
        !LastDdrawFrameHudDone()) {   // render-twice / GPU frames: already done on the converter thread
        WristHudOnNewFrame(g_capturedSourceRgba.data(), srcW, srcH);
    }

    // With alternate-eye stereo the frame belongs to one eye, so it updates
    // that eye's texture and leaves the other holding its own last viewpoint.
    // A frame belongs to one eye ONLY when it was rendered from that eye's
    // camera offset. Everything else -- motion hold, and every frame captured
    // while stereo is inactive (menus, codec, cutscenes, anything outside
    // first-person view) -- is mono and must go to BOTH eyes.
    //
    // This was a bug: mono frames were being written to a texture that nothing
    // displays, so once stereo had started, the eyes kept showing their last
    // stereo pair while the real content updated somewhere invisible. That is
    // why cutscenes looked like they were happening in one eye.
    ID3D11Texture2D* uploadTarget = g_xrMonoCaptureTexture;
    ID3D11RenderTargetView* uploadTargetRTV = g_xrMonoCaptureRTV;
    bool uploadBothEyes = (capturedEye != 0 && capturedEye != 1);
    if (capturedEye == 0 || capturedEye == 1) {
        if (g_xrEyeCaptureTexture[capturedEye]) {
            uploadTarget = g_xrEyeCaptureTexture[capturedEye];
            uploadTargetRTV = g_xrEyeCaptureRTV[capturedEye];
            g_haveEyeCapture[capturedEye] = true;
        }
        // Latch the head pose this image belongs to NOW, while it is fresh.
        // Read at capture time rather than at submit time -- by submit the
        // pose has moved on, and pretending otherwise is precisely the lie
        // that makes the stale eye drag.
        //
        // With a frame view record this is exact and stable across however
        // many XR frames re-show the image; without one (legacy, or the
        // record switched off) it is the old approximation.
        g_eyeRec[capturedEye] = frameRec;
        if (frameRec.valid) g_latestRec = frameRec;
        if (g_stereoPoseTagging && (size_t)capturedEye < g_xrViews.size()) {
            g_eyeSubmitPose[capturedEye] = g_xrViews[capturedEye].pose;
            g_haveEyeSubmitPose[capturedEye] = true;
        }
        static int eyeTagLog = 0;
        if (eyeTagLog < 4) {
            eyeTagLog++;
            DebugLogger::LogFormat("Stereo: captured frame tagged eye=%d (%s) -- holding one image per eye",
                capturedEye, capturedEye == 0 ? "left" : "right");
        }
    }

    if (uploadBothEyes && frameRec.valid) g_latestRec = frameRec;
    {
        static int recUseLog = 0;
        static bool lastValid = false;
        if (recUseLog < 12 && frameRec.valid != lastValid) {
            recUseLog++;
            lastValid = frameRec.valid;
            DebugLogger::LogFormat(
                "World lock: frames now %s (eye tag %d, clip %d)",
                frameRec.valid ? "carry their render pose -- submitted with it, FOV claimed from the frame"
                               : "have NO render pose (not head-driven) -- legacy submission",
                capturedEye, frameRec.clip);
        }
    }
    if (uploadBothEyes) {
        for (int e = 0; e < 2; ++e) {
            if (g_xrEyeCaptureTexture[e]) g_haveEyeCapture[e] = true;
            // A mono frame is current in both eyes, so both get this pose.
            g_eyeRec[e] = frameRec;
            if (g_stereoPoseTagging && (size_t)e < g_xrViews.size()) {
                g_eyeSubmitPose[e] = g_xrViews[e].pose;
                g_haveEyeSubmitPose[e] = true;
            }
        }
    }

    if (srcW == g_captureWidth && srcH == g_captureHeight) {
        // No letterbox: the drawn rect is the whole eye buffer.
        g_drawRectX = 0; g_drawRectY = 0;
        g_drawRectW = g_captureWidth; g_drawRectH = g_captureHeight;
        g_visibleSrcFracX = 1.0f;
        g_visibleSrcFracY = 1.0f;
        g_drawScaleFactor = 1.0f;
        if (uploadBothEyes) {
            for (int e = 0; e < 2; ++e) {
                if (g_xrEyeCaptureTexture[e])
                    g_xrD3DContext->UpdateSubresource(g_xrEyeCaptureTexture[e], 0, nullptr, g_capturedSourceRgba.data(), (UINT)(g_captureWidth * 4), 0);
            }
        }
        g_xrD3DContext->UpdateSubresource(uploadTarget, 0, nullptr, g_capturedSourceRgba.data(), (UINT)(g_captureWidth * 4), 0);
    }
    else {
        // --- 2D UI framing blend --------------------------------------------
        // Menus, codec calls and cutscenes are flat pictures with text on
        // them and want to sit further away than gameplay does. IsFpvActive()
        // is the debounced first-person state from the camera hook -- the
        // same signal the FPV auto-restore trusts -- so this follows the game
        // rather than guessing from pixels. Blended over
        // ui_transition_frames because MGS1 cuts to a codec call instantly
        // and an instant change of image size reads as the world lurching.
        {
            // Three conditions, and the third is the non-obvious one.
            //
            // NOT just !IsFpvActive(): the third-person diorama mode is
            // gameplay-in-a-room, not a flat menu, and shrinking it onto a
            // small virtual screen would defeat the entire point of it.
            //
            // AND IsCameraWriteHookEnabled(): IsFpvActive() returns
            // `g_enabled && g_lastFpvState`, where g_enabled is the CAMERA
            // WRITE HOOK's enable flag -- so with use_camera_write_hook=0
            // (a supported A/B setting this ini documents) it is false
            // FOREVER, and without this guard all of gameplay would silently
            // render at ui_image_scale_percent on a shrunken screen. In that
            // configuration we have no reliable first-person signal at all,
            // so the honest answer is to leave framing alone rather than
            // guess.
            //
            // AND NOT IsCutsceneVrActive(): same argument again, one step
            // further. A cutscene being rendered in VR -- head rotation, 6DOF,
            // stereo -- is a scene you are standing inside, and shrinking it
            // onto a distant virtual screen is exactly what the cutscene-VR
            // feature exists to stop happening. Note this is the ONE place
            // where cutscene VR changes behaviour outside camera_write_hook,
            // and it has to be here rather than folded into
            // IsThirdPersonVrActive(), which would make that function's name
            // a lie for every other caller.
            const bool haveFpvSignal = IsCameraWriteHookEnabled();
            // NATIVE MODE OVERRIDES ALL OF THIS. The 2D-UI framing exists to
            // push menus back onto a small virtual screen inside a head-locked
            // projection layer. Native mode's whole picture is already ON a
            // screen -- a real one, world-locked, whose size is set in metres by
            // native_screen_width_cm -- so shrinking the image inside it as
            // well would just letterbox the screen's own contents and make the
            // game smaller for no reason. Force the full-size framing and let
            // the quad's physical size be the only thing that decides how big
            // the picture looks.
            const bool nativeMode = !IsVrViewModeActive() ||
                (g_menuAsScreen && g_nativeScreenEnabled && IsDdrawMenuOverlayActive()) ||
                (g_nativeScreenEnabled && CodecScreenWanted()) ||
                (g_nativeScreenEnabled && IsCutsceneScreenFallbackActive());
            const float target = nativeMode ? 0.0f :
                ((g_uiFramingEnabled && haveFpvSignal &&
                 !IsFpvActive() && !IsThirdPersonVrActive() &&
                 !IsCutsceneVrActive()) ? 1.0f : 0.0f);
            static bool loggedNoFpvSignal = false;
            if (g_uiFramingEnabled && !haveFpvSignal && !loggedNoFpvSignal) {
                loggedNoFpvSignal = true;
                DebugLogger::Log("2D UI framing: disabled at runtime -- use_camera_write_hook=0 means there "
                    "is no trustworthy first-person signal to switch on, so every frame keeps the gameplay "
                    "framing (image_scale_percent).");
            }
            const float step = 1.0f / (float)g_uiTransitionFrames;
            if (g_uiFramingBlend < target) {
                g_uiFramingBlend += step;
                if (g_uiFramingBlend > target) g_uiFramingBlend = target;
            }
            else if (g_uiFramingBlend > target) {
                g_uiFramingBlend -= step;
                if (g_uiFramingBlend < target) g_uiFramingBlend = target;
            }
        }
        const float blend = g_uiFramingBlend;
        const float effScalePercent =
            (float)g_imageScalePercent + blend * (float)(g_uiImageScalePercent - g_imageScalePercent);
        const float effCropPercent =
            (float)g_cropToFillPercent + blend * (float)(g_uiCropToFillPercent - g_cropToFillPercent);

        // --- how much of the source survives the crop ------------------------
        // Fit-inside uses all of the source and leaves bars. Fill uses all of
        // the destination and throws source away along whichever axis is
        // oversupplied. Both are the same construction with a different
        // visible fraction, so interpolate the FRACTION rather than trying to
        // interpolate two different rect formulas.
        //
        //   srcAspect > dstAspect (the Quest 2 case: 1.333 vs 0.929) -> the
        //     source is relatively too WIDE, bars are top/bottom, filling
        //     crops the SIDES, and the drawn width is the full eye width in
        //     both extremes.
        //   srcAspect < dstAspect -> mirror image: bars left/right, filling
        //     crops top and bottom, drawn height is the full eye height.
        const double srcAspect = (double)srcW / (double)srcH;
        const double dstAspect = (double)g_captureWidth / (double)g_captureHeight;
        const double t = (double)effCropPercent / 100.0;

        double visFracX = 1.0, visFracY = 1.0;
        int32_t rectW = g_captureWidth;
        int32_t rectH = g_captureHeight;

        if (g_preserveAspect) {
            if (srcAspect >= dstAspect) {
                const double fillFrac = dstAspect / srcAspect;      // <= 1
                visFracX = 1.0 - t * (1.0 - fillFrac);
                visFracY = 1.0;
                rectW = g_captureWidth;
                rectH = (int32_t)((double)g_captureWidth / (srcAspect * visFracX) + 0.5);
                // Overflow guard. Unreachable by construction (at t=1 this
                // lands exactly on g_captureHeight and t<1 lands below it) but
                // kept, and kept ASPECT-CORRECT: clamping one axis alone would
                // stretch the picture, which is the exact distortion
                // preserve_aspect exists to prevent. Recompute the other axis
                // from the clamped one instead.
                if (rectH > g_captureHeight) {
                    rectH = g_captureHeight;
                    rectW = (int32_t)((double)g_captureHeight * srcAspect * visFracX + 0.5);
                    if (rectW > g_captureWidth) rectW = g_captureWidth;
                }
            }
            else {
                const double fillFrac = srcAspect / dstAspect;      // <= 1
                visFracY = 1.0 - t * (1.0 - fillFrac);
                visFracX = 1.0;
                rectH = g_captureHeight;
                rectW = (int32_t)((double)g_captureHeight * srcAspect / visFracY + 0.5);
                if (rectW > g_captureWidth) {
                    rectW = g_captureWidth;
                    rectH = (int32_t)((double)g_captureWidth * visFracY / srcAspect + 0.5);
                    if (rectH > g_captureHeight) rectH = g_captureHeight;
                }
            }
        }
        else if (effCropPercent > 0.5f) {
            // preserve_aspect=0 stretches the source to fill the eye buffer
            // outright, so there is no letterbox to remove and nothing for
            // crop-to-fill to do. Say so once rather than leaving the key
            // looking broken.
            static bool loggedCropIgnored = false;
            if (!loggedCropIgnored) {
                loggedCropIgnored = true;
                DebugLogger::Log("Crop-to-fill: IGNORED because preserve_aspect=0. With aspect preservation "
                    "off the frame is already stretched to fill the whole eye buffer, so there are no "
                    "letterbox bars to crop away. Set preserve_aspect=1 to use crop_to_fill_percent.");
            }
        }

        float appliedScale = 1.0f;
        if (effScalePercent < 99.5f) {
            appliedScale = effScalePercent / 100.0f;
            rectW = (int32_t)((double)rectW * (double)appliedScale + 0.5);
            rectH = (int32_t)((double)rectH * (double)appliedScale + 0.5);
        }
        if (rectW < 1) rectW = 1;
        if (rectH < 1) rectH = 1;
        const int32_t offX = (g_captureWidth - rectW) / 2;
        const int32_t offY = (g_captureHeight - rectH) / 2;

        // Source sub-rectangle, centred, in 0..1 UV space. Identity when
        // nothing is cropped, which is what every pre-existing call site
        // expects, so crop_to_fill_percent=0 reproduces the old behaviour
        // exactly rather than approximately.
        const float srcUvW = (float)visFracX;
        const float srcUvH = (float)visFracY;
        const float srcUvX = (float)((1.0 - visFracX) * 0.5);
        const float srcUvY = (float)((1.0 - visFracY) * 0.5);

        // Publish the drawn rect for the submission: with submitted_hfov_deg
        // set, the layer's sub-image is narrowed to exactly these pixels, so
        // the claimed frustum describes the picture and not the letterbox.
        g_drawRectX = offX; g_drawRectY = offY;
        g_drawRectW = rectW; g_drawRectH = rectH;
        // ...and how much of the rendered frustum those pixels actually
        // contain, so the FOV claim can be narrowed by the same amount. A
        // claim that ignores the crop over-claims by 1/visFrac and shears the
        // world as you turn.
        g_visibleSrcFracX = (float)visFracX;
        g_visibleSrcFracY = (float)visFracY;
        // ...and how much the picture was deliberately shrunk, so the claim
        // shrinks with it and the image really does move away from your face.
        g_drawScaleFactor = appliedScale;

        // Black the surround only when the letterbox geometry actually
        // changes -- and then only on each destination's own next redraw, see
        // the note below.
        static bool s_eyeNeedsClear[2] = { false, false };
        static bool s_monoNeedsClear = false;
        static int32_t lastRectW = -1, lastRectH = -1, lastOffX = -1, lastOffY = -1;
        if (rectW != lastRectW || rectH != lastRectH || offX != lastOffX || offY != lastOffY) {
            // CPU fallback path: one shared staging buffer, so blacking it
            // here covers every destination at once. The GPU path needs the
            // per-destination latches below because each destination is a
            // separate texture redrawn on its own schedule.
            if (!g_captureRgba.empty()) memset(g_captureRgba.data(), 0, g_captureRgba.size());
            // EVERY destination is MARKED for clearing on a geometry change,
            // not just the one this frame happens to draw into -- but each is
            // actually cleared only on the frame it is next redrawn.
            //
            // Both halves of that matter. Marking all three is needed because
            // in alternate-eye stereo a frame draws ONE eye, so clearing only
            // that eye leaves the other ringed with the previous, larger
            // frame's pixels. Deferring the clear to the redraw is needed
            // because doing it eagerly blacks an eye that is NOT being
            // redrawn this frame, and the eyes then alternate black for the
            // whole transition -- which is worse than the ring it fixes.
            //
            // Both bugs are invisible while the rect essentially never moves.
            // ui_framing_enabled moves it on every menu and codec transition,
            // which is what brought them into range.
            s_eyeNeedsClear[0] = s_eyeNeedsClear[1] = true;
            s_monoNeedsClear = true;
            lastRectW = rectW; lastRectH = rectH; lastOffX = offX; lastOffY = offY;
            DebugLogger::LogFormat(
                "Image framing: source %dx%d (aspect %.3f) -> eye %dx%d (aspect %.3f); drawing %dx%d at +%d,+%d "
                "[preserve_aspect=%d scale=%.0f%% crop_to_fill=%.0f%% visible_src=%.3fx%.3f ui_blend=%.2f "
                "gpu_bilinear_resample=%d]",
                srcW, srcH, (double)srcW / (double)srcH,
                g_captureWidth, g_captureHeight, (double)g_captureWidth / (double)g_captureHeight,
                rectW, rectH, offX, offY,
                g_preserveAspect ? 1 : 0, effScalePercent, effCropPercent,
                visFracX, visFracY, blend, g_gpuBilinearResample ? 1 : 0);
        }

        // Only take the GPU path when everything it needs is actually up --
        // pipeline compiled, source texture ready, AND a render-target view
        // for wherever this frame is going. Anything short of "all of the
        // above" falls through to the CPU path in full, rather than mixing
        // a GPU-drawn eye with a CPU-drawn mono target (or vice versa) in
        // the same frame.
        bool gpuPathReady = g_gpuBilinearResample && uploadTargetRTV &&
            EnsureBlitPipeline() && EnsureSourceCaptureTexture(srcW, srcH);

        if (gpuPathReady) {
            // One small upload of the raw source frame at its native
            // resolution (e.g. 640x480 = ~1.2 MB), then the GPU samples it
            // with bilinear filtering into each destination sub-rectangle --
            // no CPU-side per-pixel loop at all.
            g_xrD3DContext->UpdateSubresource(g_xrSourceCaptureTexture, 0, nullptr,
                g_capturedSourceRgba.data(), (UINT)(srcW * 4), 0);

            // Was GpuResampleInto (which hardcodes an identity source UV
            // rect). The crop variant is the same draw with the sampled
            // sub-rectangle made explicit, and at crop_to_fill_percent=0 the
            // UVs ARE (0,0,1,1), so this is bit-identical to the old path
            // until the crop is actually turned on.
            if (uploadBothEyes) {
                for (int e = 0; e < 2; ++e) {
                    if (g_xrEyeCaptureRTV[e]) {
                        GpuResampleCropInto(g_xrEyeCaptureRTV[e], g_captureWidth, g_captureHeight,
                                            offX, offY, rectW, rectH,
                                            srcUvX, srcUvY, srcUvW, srcUvH, s_eyeNeedsClear[e]);
                        s_eyeNeedsClear[e] = false;
                    }
                }
            }
            // Consume this destination's own pending clear, whichever it is.
            // uploadTargetRTV is the mono RTV on a mono frame and the tagged
            // eye's RTV on a stereo frame (see where uploadTarget is chosen
            // above), so the flag has to be selected the same way rather than
            // assumed.
            {
                bool* pendingClear = &s_monoNeedsClear;
                if ((capturedEye == 0 || capturedEye == 1) &&
                    uploadTargetRTV == g_xrEyeCaptureRTV[capturedEye]) {
                    pendingClear = &s_eyeNeedsClear[capturedEye];
                }
                GpuResampleCropInto(uploadTargetRTV, g_captureWidth, g_captureHeight,
                                    offX, offY, rectW, rectH,
                                    srcUvX, srcUvY, srcUvW, srcUvH, *pendingClear);
                *pendingClear = false;
            }

            // --- HUD magnification insets ---------------------------------
            // Drawn AFTER the full-frame resample above, on top of it, never
            // clearing (clearFull=false) -- each region is a small crop of
            // the source frame sampled again, larger, at a position relative
            // to the just-drawn image rect so it tracks preserve_aspect /
            // image_scale_percent letterboxing automatically.
            //
            // Note the deliberate asymmetry with crop-to-fill: region src
            // rects stay in FULL source space, unaffected by
            // crop_to_fill_percent. That is the useful behaviour -- crop-to-
            // fill cuts the sides off, which is exactly where MGS1 puts the
            // radar and the life gauge, so a magnify region is how you get a
            // HUD element back after cropping it away. The dst rects DO ride
            // on the (possibly cropped) drawn rect, so the inset still lands
            // where you asked on screen.
            if (g_hudMagnifyEnabled && !g_hudMagnifyRegions.empty()) {
                static bool loggedHudMagnifyActive = false;
                if (!loggedHudMagnifyActive) {
                    loggedHudMagnifyActive = true;
                    DebugLogger::LogFormat("HUD magnify: drawing %zu region(s) this frame onward",
                        g_hudMagnifyRegions.size());
                }
                for (const HudMagnifyRegion& region : g_hudMagnifyRegions) {
                    const int32_t insetOffX = offX + (int32_t)(region.dstX * (float)rectW);
                    const int32_t insetOffY = offY + (int32_t)(region.dstY * (float)rectH);
                    const int32_t insetW = (int32_t)(region.dstW * (float)rectW);
                    const int32_t insetH = (int32_t)(region.dstH * (float)rectH);
                    if (insetW <= 0 || insetH <= 0) continue;

                    if (uploadBothEyes) {
                        for (int e = 0; e < 2; ++e) {
                            if (g_xrEyeCaptureRTV[e]) {
                                GpuResampleCropInto(g_xrEyeCaptureRTV[e], g_captureWidth, g_captureHeight,
                                    insetOffX, insetOffY, insetW, insetH,
                                    region.srcX, region.srcY, region.srcW, region.srcH, false);
                            }
                        }
                    }
                    GpuResampleCropInto(uploadTargetRTV, g_captureWidth, g_captureHeight,
                        insetOffX, insetOffY, insetW, insetH,
                        region.srcX, region.srcY, region.srcW, region.srcH, false);
                }
            }
        }
        else {
            // CPU nearest-neighbor fallback -- either gpu_bilinear_resample=0
            // in the ini, or the GPU pipeline/textures failed to come up
            // (compile error, low-end GPU without vs_4_0/ps_4_0, etc). Keeps
            // the mod working even if the shader path can't run.
            if (g_hudMagnifyEnabled && !g_hudMagnifyRegions.empty()) {
                static bool loggedHudMagnifySkipped = false;
                if (!loggedHudMagnifySkipped) {
                    loggedHudMagnifySkipped = true;
                    DebugLogger::Log("HUD magnify: enabled in ini but the GPU blit path is not active "
                        "(gpu_bilinear_resample=0 or pipeline setup failed) -- insets need the GPU path, skipping");
                }
            }
            ResampleNearestCropInto(g_capturedSourceRgba.data(), srcW, srcH,
                                    g_captureRgba.data(), g_captureWidth,
                                    offX, offY, rectW, rectH,
                                    srcUvX, srcUvY, srcUvW, srcUvH);
            if (uploadBothEyes) {
                for (int e = 0; e < 2; ++e) {
                    if (g_xrEyeCaptureTexture[e])
                        g_xrD3DContext->UpdateSubresource(g_xrEyeCaptureTexture[e], 0, nullptr, g_captureRgba.data(), (UINT)(g_captureWidth * 4), 0);
                }
            }
            g_xrD3DContext->UpdateSubresource(uploadTarget, 0, nullptr, g_captureRgba.data(), (UINT)(g_captureWidth * 4), 0);
        }
    }

    // Cheap spot check (not a full scan) for "this frame actually has real
    // content, not just black" -- feeds TriggerAutoFpvIfNeeded()'s readiness
    // gate. Sampling a sparse set of bytes is enough to distinguish a truly
    // blank capture from a real one without the cost of scanning every pixel
    // every frame.
    if (!g_haveSeenHealthyCapture) {
        const uint8_t* buf = g_capturedSourceRgba.data();
        size_t bufSize = g_capturedSourceRgba.size();
        size_t nonZeroSamples = 0;
        constexpr size_t kSampleCount = 64;
        for (size_t i = 0; i < kSampleCount && bufSize > 0; ++i) {
            size_t offset = (bufSize / kSampleCount) * i;
            if (offset < bufSize && buf[offset] != 0) {
                nonZeroSamples++;
            }
        }
        if (nonZeroSamples > kSampleCount / 4) { // >25% of samples non-zero
            g_haveSeenHealthyCapture = true;
            DebugLogger::Log("First healthy (non-black) capture confirmed -- auto-FPV can now fire on next FOCUSED event");
        }
    }

    // Commit the skip state only now that the upload has actually happened, so
    // a frame we bailed out of can never be recorded as delivered.
    s_lastUploadedSequence = thisSequence;
    s_haveUploadedOnce = true;
    s_lastCaptureW = g_captureWidth;
    s_lastCaptureH = g_captureHeight;

    return true;
}

static void BlitMonoTextureToEyeSwapchain(uint32_t eye, ID3D11Texture2D* dstTexture) {
    if (!dstTexture || !g_xrMonoCaptureTexture || g_captureWidth <= 0 || g_captureHeight <= 0) {
        return;
    }

    // Real stereo: this eye's own viewpoint, if we have one.
    ID3D11Texture2D* srcTexture = g_xrMonoCaptureTexture;
    bool realStereo = false;
    if (eye < 2 && g_haveEyeCapture[0] && g_haveEyeCapture[1] && g_xrEyeCaptureTexture[eye]) {
        srcTexture = g_xrEyeCaptureTexture[eye];
        realStereo = true;
    }

    // The uniform pixel shift is a fake. It translates near and far pixels
    // alike, so it contributes no disparity and no depth -- it only ever
    // shifted the whole picture sideways. Once each eye has its own genuine
    // viewpoint the shift is not just unnecessary, it would corrupt the real
    // disparity, so it stands down.
    int shift = realStereo ? 0 : g_stereoParallaxPixels;
    if (shift < 0) shift = 0;
    if (shift > g_captureWidth / 4) shift = g_captureWidth / 4;

    static bool loggedRealStereo = false;
    if (realStereo && !loggedRealStereo) {
        loggedRealStereo = true;
        DebugLogger::Log("Stereo: REAL per-eye viewpoints active -- uniform pixel shift disabled");
    }

    D3D11_BOX srcBox{};
    srcBox.top = 0;
    srcBox.bottom = (UINT)g_captureHeight;
    srcBox.front = 0;
    srcBox.back = 1;

    UINT dstX = 0;
    if (eye == 0) {
        srcBox.left = 0;
        srcBox.right = (UINT)(g_captureWidth - shift);
        dstX = (UINT)shift;
    }
    else {
        srcBox.left = (UINT)shift;
        srcBox.right = (UINT)g_captureWidth;
        dstX = 0;
    }

    g_xrD3DContext->CopySubresourceRegion(dstTexture, 0, dstX, 0, 0, srcTexture, 0, &srcBox);
}

static void CleanupOpenXrRenderTargets() {
    ShutdownAimLaser();
    ShutdownWristHud();

    if (g_xrMonoCaptureRTV) { g_xrMonoCaptureRTV->Release(); g_xrMonoCaptureRTV = nullptr; }
    if (g_xrMonoCaptureTexture) {
        g_xrMonoCaptureTexture->Release();
        g_xrMonoCaptureTexture = nullptr;
    }
    for (int e = 0; e < 2; ++e) {
        if (g_xrEyeCaptureRTV[e]) { g_xrEyeCaptureRTV[e]->Release(); g_xrEyeCaptureRTV[e] = nullptr; }
        if (g_xrEyeCaptureTexture[e]) { g_xrEyeCaptureTexture[e]->Release(); g_xrEyeCaptureTexture[e] = nullptr; }
    }

    // GPU resample source texture/SRV and pipeline state objects are tied to
    // g_xrD3DDevice, which this teardown may be the last reference to (a new
    // session may bring up a different device) -- release and reset so the
    // next session's EnsureBlitPipeline()/EnsureSourceCaptureTexture() start
    // clean rather than reusing objects from a dead device.
    if (g_xrSourceCaptureSRV) { g_xrSourceCaptureSRV->Release(); g_xrSourceCaptureSRV = nullptr; }
    if (g_xrSourceCaptureTexture) { g_xrSourceCaptureTexture->Release(); g_xrSourceCaptureTexture = nullptr; }
    g_sourceCaptureWidth = 0;
    g_sourceCaptureHeight = 0;
    if (g_blitVS) { g_blitVS->Release(); g_blitVS = nullptr; }
    if (g_blitPS) { g_blitPS->Release(); g_blitPS = nullptr; }
    if (g_blitLinearSampler) { g_blitLinearSampler->Release(); g_blitLinearSampler = nullptr; }
    if (g_blitRasterState) { g_blitRasterState->Release(); g_blitRasterState = nullptr; }
    if (g_blitBlendState) { g_blitBlendState->Release(); g_blitBlendState = nullptr; }
    if (g_blitDepthState) { g_blitDepthState->Release(); g_blitDepthState = nullptr; }
    if (g_blitCropCB) { g_blitCropCB->Release(); g_blitCropCB = nullptr; }
    g_blitPipelineReady = false;

    g_captureWidth = 0;
    g_captureHeight = 0;
    g_captureRgba.clear();
    g_capturedSourceRgba.clear();
    g_haveEyeSubmitPose[0] = g_haveEyeSubmitPose[1] = false;
    g_eyeRec[0] = g_eyeRec[1] = g_latestRec = FrameViewRecord{};
    g_drawRectX = g_drawRectY = g_drawRectW = g_drawRectH = 0;

    for (auto& eye : g_xrEyeTargets) {
        for (auto* rtv : eye.rtvs) {
            if (rtv) {
                rtv->Release();
            }
        }
        eye.rtvs.clear();
        eye.images.clear();

        if (eye.swapchain != XR_NULL_HANDLE && g_xrSession != XR_NULL_HANDLE) {
            xrDestroySwapchain(eye.swapchain);
        }

        eye.swapchain = XR_NULL_HANDLE;
        eye.width = 0;
        eye.height = 0;
    }

    g_xrViewConfigViews.clear();
    g_xrViews.clear();
    g_xrProjectionViews.clear();
    g_xrProjectionLayer = { XR_TYPE_COMPOSITION_LAYER_PROJECTION };
    g_xrSwapchainFormat = DXGI_FORMAT_UNKNOWN;
    g_xrRenderTargetsReady = false;
}

static bool InitializeOpenXrRenderTargets() {
    if (g_xrSession == XR_NULL_HANDLE || g_xrSystemId == XR_NULL_SYSTEM_ID || !g_xrD3DDevice) {
        return false;
    }

    CleanupOpenXrRenderTargets();

    uint32_t formatCount = 0;
    XrResult xr = xrEnumerateSwapchainFormats(g_xrSession, 0, &formatCount, nullptr);
    if (xr != XR_SUCCESS || formatCount == 0) {
        DebugLogger::LogFormat("xrEnumerateSwapchainFormats failed: %d (count=%u)", (int)xr, formatCount);
        return false;
    }

    std::vector<int64_t> formats(formatCount);
    xr = xrEnumerateSwapchainFormats(g_xrSession, formatCount, &formatCount, formats.data());
    if (xr != XR_SUCCESS) {
        DebugLogger::LogFormat("xrEnumerateSwapchainFormats(list) failed: %d", (int)xr);
        return false;
    }

    g_xrSwapchainFormat = SelectSwapchainFormat(formats);
    if (g_xrSwapchainFormat == DXGI_FORMAT_UNKNOWN) {
        DebugLogger::Log("No supported OpenXR swapchain format selected");
        return false;
    }

    uint32_t viewCount = 0;
    xr = xrEnumerateViewConfigurationViews(
        g_xrInstance,
        g_xrSystemId,
        XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
        0,
        &viewCount,
        nullptr);
    if (xr != XR_SUCCESS || viewCount < 2) {
        DebugLogger::LogFormat("xrEnumerateViewConfigurationViews(count) failed: %d (viewCount=%u)", (int)xr, viewCount);
        return false;
    }

    g_xrViewConfigViews.resize(viewCount, { XR_TYPE_VIEW_CONFIGURATION_VIEW });
    xr = xrEnumerateViewConfigurationViews(
        g_xrInstance,
        g_xrSystemId,
        XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
        viewCount,
        &viewCount,
        g_xrViewConfigViews.data());
    if (xr != XR_SUCCESS) {
        DebugLogger::LogFormat("xrEnumerateViewConfigurationViews(list) failed: %d", (int)xr);
        return false;
    }

    g_xrViews.resize(viewCount, { XR_TYPE_VIEW });
    g_xrProjectionViews.resize(viewCount, { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW });

    // EYE RESOLUTION SCALE.
    //
    // The runtime's "recommended" size is sized for an app that renders real
    // geometry. We are upscaling a 640x480 PSX framebuffer, so at the Quest 2's
    // recommended 2496x2688 we were paying for a 4x linear upscale that carries
    // no extra detail whatsoever -- and paying for it in the compositor and in
    // the wireless encoder, every frame.
    //
    // That cost is not academic. It is why VDXR could not absorb a single extra
    // composition layer for the aim laser (see the Traps section of the handoff
    // doc): submitting the laser quad collapsed the frame rate permanently,
    // while running the identical code path WITHOUT submitting the layer was
    // clean. The compositor had no headroom, and this is where it went.
    //
    // 50% linear = 25% of the pixels. Against a 640x480 source that is still a
    // ~2x upscale, so there is nothing to lose visually.
    int eyeScalePercent = GetPrivateProfileIntA(
        "openxr", "eye_resolution_percent", 50, g_iniPath.c_str());
    if (eyeScalePercent < 25)  eyeScalePercent = 25;
    if (eyeScalePercent > 100) eyeScalePercent = 100;

    for (uint32_t eye = 0; eye < viewCount && eye < 2; ++eye) {
        XrEyeRenderTarget& target = g_xrEyeTargets[eye];
        const int32_t recW = (int32_t)g_xrViewConfigViews[eye].recommendedImageRectWidth;
        const int32_t recH = (int32_t)g_xrViewConfigViews[eye].recommendedImageRectHeight;

        // Keep it even -- odd swapchain dimensions upset some encoders.
        target.width  = ((recW * eyeScalePercent / 100) + 1) & ~1;
        target.height = ((recH * eyeScalePercent / 100) + 1) & ~1;
        if (target.width  < 2) target.width  = 2;
        if (target.height < 2) target.height = 2;

        if (eye == 0) {
            DebugLogger::LogFormat(
                "Eye resolution: runtime recommends %dx%d, submitting %dx%d "
                "(eye_resolution_percent=%d). The game frame (640x480 or 1024x768) is still upscaled; sharpen_percent offsets the softness.",
                recW, recH, target.width, target.height, eyeScalePercent);
        }

        XrSwapchainCreateInfo swapchainInfo{ XR_TYPE_SWAPCHAIN_CREATE_INFO };
        swapchainInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
        swapchainInfo.format = (int64_t)g_xrSwapchainFormat;
        swapchainInfo.sampleCount = g_xrViewConfigViews[eye].recommendedSwapchainSampleCount;
        swapchainInfo.width = target.width;
        swapchainInfo.height = target.height;
        swapchainInfo.faceCount = 1;
        swapchainInfo.arraySize = 1;
        swapchainInfo.mipCount = 1;

        xr = xrCreateSwapchain(g_xrSession, &swapchainInfo, &target.swapchain);
        if (xr != XR_SUCCESS) {
            DebugLogger::LogFormat("xrCreateSwapchain(eye=%u) failed: %d", eye, (int)xr);
            return false;
        }

        uint32_t imageCount = 0;
        xr = xrEnumerateSwapchainImages(target.swapchain, 0, &imageCount, nullptr);
        if (xr != XR_SUCCESS || imageCount == 0) {
            DebugLogger::LogFormat("xrEnumerateSwapchainImages(count, eye=%u) failed: %d count=%u", eye, (int)xr, imageCount);
            return false;
        }

        target.images.resize(imageCount, { XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR });
        xr = xrEnumerateSwapchainImages(
            target.swapchain,
            imageCount,
            &imageCount,
            reinterpret_cast<XrSwapchainImageBaseHeader*>(target.images.data()));
        if (xr != XR_SUCCESS) {
            DebugLogger::LogFormat("xrEnumerateSwapchainImages(list, eye=%u) failed: %d", eye, (int)xr);
            return false;
        }

        target.rtvs.resize(imageCount, nullptr);
        for (uint32_t i = 0; i < imageCount; ++i) {
            ID3D11Texture2D* texture = target.images[i].texture;
            if (!texture) {
                DebugLogger::LogFormat("OpenXR swapchain image texture null (eye=%u image=%u)", eye, i);
                return false;
            }

            D3D11_TEXTURE2D_DESC texDesc{};
            texture->GetDesc(&texDesc);

            D3D11_RENDER_TARGET_VIEW_DESC rtvDesc{};
            // Typeless image (the normal case): view it as the NON-sRGB twin
            // of the swapchain format, so our already-sRGB pixels are written
            // byte for byte and the runtime reads them as sRGB. An image that
            // is itself an sRGB format cannot be viewed that way; D3D11 would
            // then re-encode on write and darks would lift again -- logged.
            rtvDesc.Format = IsTypelessFormat(texDesc.Format) ? NonSrgbView(g_xrSwapchainFormat) : texDesc.Format;
            if (eye == 0 && i == 0) {
                const bool srgbDecl = NonSrgbView(g_xrSwapchainFormat) != g_xrSwapchainFormat;
                DebugLogger::LogFormat("Colour: swapchain declared fmt %d (%s), image fmt %d, render view fmt %d -> %s",
                    (int)g_xrSwapchainFormat, srgbDecl ? "sRGB" : "UNORM = linear to the runtime",
                    (int)texDesc.Format, (int)rtvDesc.Format,
                    !srgbDecl ? "the runtime will LIFT dark tones (set [openxr] srgb_swapchain=1)"
                    : (rtvDesc.Format == g_xrSwapchainFormat ? "WARNING: image is not typeless, writes get sRGB-encoded (darks lifted)"
                                                             : "game colours pass through unchanged"));
            }
            if (texDesc.ArraySize > 1) {
                rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
                rtvDesc.Texture2DArray.MipSlice = 0;
                rtvDesc.Texture2DArray.FirstArraySlice = 0;
                rtvDesc.Texture2DArray.ArraySize = 1;
            }
            else {
                rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
                rtvDesc.Texture2D.MipSlice = 0;
            }

            HRESULT hr = g_xrD3DDevice->CreateRenderTargetView(texture, &rtvDesc, &target.rtvs[i]);
            if (FAILED(hr)) {
                DebugLogger::LogFormat("CreateRenderTargetView failed (eye=%u image=%u): 0x%08X texFmt=%d rtvFmt=%d array=%u", eye, i, (unsigned)hr, (int)texDesc.Format, (int)rtvDesc.Format, texDesc.ArraySize);
                return false;
            }
        }

        g_xrProjectionViews[eye].subImage.swapchain = target.swapchain;
        g_xrProjectionViews[eye].subImage.imageRect.offset = { 0, 0 };
        g_xrProjectionViews[eye].subImage.imageRect.extent = { target.width, target.height };
        g_xrProjectionViews[eye].subImage.imageArrayIndex = 0;
    }

    g_xrProjectionLayer.space = g_xrPlaySpace;
    g_xrProjectionLayer.viewCount = (uint32_t)g_xrProjectionViews.size();
    g_xrProjectionLayer.views = g_xrProjectionViews.data();

    if (!EnsureMonoCaptureResources(g_xrEyeTargets[0].width, g_xrEyeTargets[0].height)) {
        DebugLogger::Log("WARNING: Mono capture resources unavailable; using debug eye colors");
    }

    g_xrRenderTargetsReady = true;

    // Fail-soft: a laser that cannot initialise logs and stands down. It must
    // never be able to cost you the headset image.
    if (IsAimLaserEnabled()) {
        InitAimLaser(g_xrSession, g_xrPlaySpace, g_xrD3DDevice, g_xrD3DContext);
    }
    InitWristHud(g_xrSession, g_xrPlaySpace, g_xrD3DDevice, g_xrD3DContext);

    DebugLogger::LogFormat(
        "OpenXR projection rendering initialized: format=%d left=%dx%d right=%dx%d",
        (int)g_xrSwapchainFormat,
        g_xrEyeTargets[0].width,
        g_xrEyeTargets[0].height,
        g_xrEyeTargets[1].width,
        g_xrEyeTargets[1].height);

    return true;
}

static const char* DescribeSessionState(XrSessionState state) {
    switch (state) {
    case XR_SESSION_STATE_UNKNOWN: return "UNKNOWN";
    case XR_SESSION_STATE_IDLE: return "IDLE";
    case XR_SESSION_STATE_READY: return "READY";
    case XR_SESSION_STATE_SYNCHRONIZED: return "SYNCHRONIZED";
    case XR_SESSION_STATE_VISIBLE: return "VISIBLE";
    case XR_SESSION_STATE_FOCUSED: return "FOCUSED";
    case XR_SESSION_STATE_STOPPING: return "STOPPING";
    case XR_SESSION_STATE_LOSS_PENDING: return "LOSS_PENDING";
    case XR_SESSION_STATE_EXITING: return "EXITING";
    default: return "UNRECOGNIZED";
    }
}

static void PumpOpenXrEvents() {
    if (g_xrInstance == XR_NULL_HANDLE) {
        return;
    }

    while (true) {
        XrEventDataBuffer eventData{ XR_TYPE_EVENT_DATA_BUFFER };
        XrResult poll = xrPollEvent(g_xrInstance, &eventData);
        if (poll == XR_EVENT_UNAVAILABLE) {
            break;
        }

        if (poll != XR_SUCCESS) {
            DebugLogger::LogFormat("xrPollEvent failed: %d", (int)poll);
            break;
        }

        if (eventData.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            auto* changed = reinterpret_cast<XrEventDataSessionStateChanged*>(&eventData);
            DebugLogger::LogFormat("OpenXR session state changed: %d (%s)", (int)changed->state, DescribeSessionState(changed->state));

            if (changed->state == XR_SESSION_STATE_READY && g_xrSession != XR_NULL_HANDLE && !g_xrSessionRunning) {
                XrSessionBeginInfo beginInfo{ XR_TYPE_SESSION_BEGIN_INFO };
                beginInfo.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                XrResult br = xrBeginSession(g_xrSession, &beginInfo);
                if (br == XR_SUCCESS) {
                    g_xrSessionRunning = true;
                    DebugLogger::Log("xrBeginSession succeeded");

                    if (!g_xrInputInitialized) {
                        InitOpenXRInput();
                        g_xrInputInitialized = true;
                        DebugLogger::Log("OpenXR action input initialized");
                    }
                }
                else {
                    DebugLogger::LogFormat("xrBeginSession failed: %d", (int)br);
                }
            }
            else if (changed->state == XR_SESSION_STATE_STOPPING && g_xrSessionRunning) {
                xrEndSession(g_xrSession);
                g_xrSessionRunning = false;
                CleanupOpenXrRenderTargets();
                DebugLogger::Log("xrEndSession called (STOPPING)");
            }
        }
    }
}

static void QuaternionToYawPitch(const XrQuaternionf& q, float& yawRad, float& pitchRad) {
    float sinp = 2.0f * (q.w * q.y - q.z * q.x);
    pitchRad = (std::abs(sinp) >= 1.0f) ? std::copysign(3.14159265359f * 0.5f, sinp) : std::asin(sinp);

    float siny_cosp = 2.0f * (q.w * q.z + q.x * q.y);
    float cosy_cosp = 1.0f - 2.0f * (q.y * q.y + q.z * q.z);
    yawRad = std::atan2(siny_cosp, cosy_cosp);
}

// CORRECTED EULER EXTRACTION.
//
// QuaternionToYawPitch() above uses the aerospace/Z-up (ZYX) convention:
//     pitch = asin(2(wy - zx)),  yaw = atan2(2(wz + xy), 1 - 2(y^2 + z^2))
// OpenXR is right-handed with **Y up** and -Z forward, so that decomposition
// is simply wrong for this data. Proven against real captured quaternions:
//
//   a pure 60 deg head YAW    -> old code reports yaw =  0.0, pitch = 60.0
//   a pure 40 deg head PITCH  -> old code reports yaw =  0.0, pitch =  0.0
//
// i.e. the old "pitch" is actually true yaw, and the old "yaw" is very nearly
// blind to head pitch altogether. That is exactly the observed symptom set:
// horizontal look roughly working, vertical barely moving ("slight up, slight
// down") and feeling inverted, and rotation generally feeling coupled/odd.
// It went unnoticed for so long because the old mouse-emulation path only ever
// used frame-to-frame DELTAS, where the error is far less visible; driving
// absolute angles exposes it immediately.
//
// Correct decomposition for OpenXR, YXZ order (yaw about Y, then pitch about
// X, then roll about Z) -- the natural order for a head pose:
static void QuaternionToYawPitchRoll(const XrQuaternionf& q, float& yawRad, float& pitchRad, float& rollRad) {
    const float x = q.x;
    const float y = q.y;
    const float z = q.z;
    const float w = q.w;

    float sinPitch = 2.0f * (w * x - y * z);
    if (sinPitch > 1.0f)  sinPitch = 1.0f;
    if (sinPitch < -1.0f) sinPitch = -1.0f;
    pitchRad = std::asin(sinPitch);

    yawRad = std::atan2(2.0f * (w * y + x * z), 1.0f - 2.0f * (x * x + y * y));
    rollRad = std::atan2(2.0f * (w * z + x * y), 1.0f - 2.0f * (x * x + z * z));
}

static float NormalizeAngleDelta(float delta) {
    const float pi = 3.14159265359f;
    while (delta > pi) delta -= 2.0f * pi;
    while (delta < -pi) delta += 2.0f * pi;
    return delta;
}

// Put the native-mode virtual screen in front of wherever the player is looking
// now, and leave it there.
//
// YAW ONLY, and the roll/pitch are dropped deliberately. Pinning the screen to
// the full head orientation would hang it at whatever angle your head happened
// to be at when you pressed the button -- tilted, or aimed at the floor -- and
// then leave it there for the rest of the session. A screen is a thing in a
// room: it stands upright, at eye height, facing you. Only which WALL it is on
// should depend on where you were looking.
//
// (native_screen_lock_roll=0 exists for anyone who disagrees, e.g. playing
// lying down, where an upright screen is the wrong answer.)
static void PinNativeScreenTo(const XrPosef& headPose) {
    float yaw = 0.0f, pitch = 0.0f, roll = 0.0f;
    QuaternionToYawPitchRoll(headPose.orientation, yaw, pitch, roll);

    if (g_nativeScreenLockRoll) {
        // Upright, facing the same compass direction the head was.
        const float half = yaw * 0.5f;
        g_nativeScreenPose.orientation.x = 0.0f;
        g_nativeScreenPose.orientation.y = std::sin(half);
        g_nativeScreenPose.orientation.z = 0.0f;
        g_nativeScreenPose.orientation.w = std::cos(half);
    }
    else {
        g_nativeScreenPose.orientation = headPose.orientation;
    }

    // OpenXR is right-handed, -Z forward. Placing the screen along the head's
    // own forward vector rather than the yaw-only one would put it above or
    // below eye level whenever the head was tilted at the moment of pinning,
    // which is the same "wherever you happened to be looking" problem the
    // orientation above avoids -- so this uses the flattened direction too.
    const float fx = -std::sin(yaw);
    const float fz = -std::cos(yaw);
    g_nativeScreenPose.position.x = headPose.position.x + fx * g_nativeScreenDistanceM;
    g_nativeScreenPose.position.y = headPose.position.y;
    g_nativeScreenPose.position.z = headPose.position.z + fz * g_nativeScreenDistanceM;
    g_nativeScreenPosed = true;
}

static DWORD WINAPI XrFrameThreadProc(LPVOID) {
    DebugLogger::Log("OpenXR frame thread started");
    bool haveLast = false;
    float lastYaw = 0.0f;
    float lastPitch = 0.0f;

    while (g_xrFrameThreadRunning.load()) {
        if (!g_xrSessionRunning || g_xrSession == XR_NULL_HANDLE || g_xrPlaySpace == XR_NULL_HANDLE) {
            Sleep(5);
            continue;
        }

        XrFrameWaitInfo waitInfo{ XR_TYPE_FRAME_WAIT_INFO };
        XrFrameState frameState{ XR_TYPE_FRAME_STATE };
        XrResult waitResult = xrWaitFrame(g_xrSession, &waitInfo, &frameState);
        if (waitResult != XR_SUCCESS) {
            Sleep(1);
            continue;
        }

        XrFrameBeginInfo beginInfo{ XR_TYPE_FRAME_BEGIN_INFO };
        if (xrBeginFrame(g_xrSession, &beginInfo) != XR_SUCCESS) {
            Sleep(1);
            continue;
        }

        UpdateOpenXRInput(frameState.predictedDisplayTime);

        XrViewLocateInfo locateInfo{ XR_TYPE_VIEW_LOCATE_INFO };
        locateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        locateInfo.displayTime = frameState.predictedDisplayTime;
        locateInfo.space = g_xrPlaySpace;

        XrViewState viewState{ XR_TYPE_VIEW_STATE };
        uint32_t viewCountOutput = 0;
        uint32_t configuredViewCount = (uint32_t)g_xrViews.size();
        XrResult locateResult = XR_ERROR_RUNTIME_FAILURE;
        if (configuredViewCount > 0) {
            locateResult = xrLocateViews(g_xrSession, &locateInfo, &viewState, configuredViewCount, &viewCountOutput, g_xrViews.data());
        }

        // xrLocateViews can return XR_SUCCESS while the runtime is telling
        // us, via these flags, that it did NOT actually have valid tracking
        // data for this call -- in which case it hands back a default
        // (usually identity) pose rather than an error. This was found to
        // be the actual cause of head movement having no effect: the
        // quaternion was confirmed static/identity across an entire test
        // session despite active head movement, and this flag check was
        // missing entirely.
        bool orientationValid = (viewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) != 0;
        bool positionValid = (viewState.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) != 0;

        static int viewStateLogCount = 0;
        if (locateResult == XR_SUCCESS && (viewStateLogCount < 5 || viewStateLogCount % 120 == 0)) {
            DebugLogger::LogFormat("xrLocateViews [tick %d]: viewStateFlags=0x%llX orientationValid=%d positionValid=%d",
                viewStateLogCount, (unsigned long long)viewState.viewStateFlags, orientationValid ? 1 : 0, positionValid ? 1 : 0);
            viewStateLogCount++;
        }

        if (locateResult == XR_SUCCESS && viewCountOutput > 0 && g_enableHmdLook && orientationValid) {
            float yaw = 0.0f;
            float pitch = 0.0f;
            float roll = 0.0f;
            QuaternionToYawPitchRoll(g_xrViews[0].pose.orientation, yaw, pitch, roll);

            // Hand the fresh head pose (orientation AND position) to the
            // in-frame camera hooks.
            // This thread NO LONGER writes camera memory itself when the hook
            // is in use -- it only publishes. The actual write happens on the
            // game's own thread inside its camera update, which is the only
            // place it can survive: the game rewrites the whole rotate2 vector
            // once per game frame from the camera actor, so any writer running
            // at a different rate on a different thread always loses.
            PublishHeadPose(yaw, pitch, roll,
                g_xrViews[0].pose.position.x,
                g_xrViews[0].pose.position.y,
                g_xrViews[0].pose.position.z);
            // The same sample, with both eye poses, as one snapshot. The
            // rotation hook steers the camera from it and attaches it to the
            // frame, so the frame can later be submitted with the pose it was
            // really drawn from (see g_eyeRec).
            if (viewCountOutput >= 2) {
                PublishHeadViews(yaw, pitch, roll,
                    ToViewPoseF(g_xrViews[0].pose), ToViewPoseF(g_xrViews[1].pose));
            }

            // REAL 6DOF WRITE: using the confirmed real rotation addresses
            // AND the confirmed NOP code patches, both taken directly from
            // the reference community Cheat Engine table's own working Lua
            // script (not just the static address list -- the script is
            // what actually proved these addresses work, and it does two
            // things the static entries alone don't show: it patches out
            // the specific game code that overwrites these bytes every
            // frame, via writeBytes(0x0044437E, NOP x3) and
            // writeBytes(0x0045495F, NOP x6). Without these patches, the
            // game's own camera-update code overwrites our rotation writes
            // before they're ever rendered -- confirmed by extensive
            // testing this session: writes verified landing correctly via
            // read-back, yet zero visible effect, exactly the symptom two
            // un-patched competing writers would produce.
            // LEGACY PATH. Superseded by the in-frame hook. Kept only so the old
            // behaviour can still be reproduced for comparison by setting
            // use_camera_write_hook=0 in the ini. While the hook is active this
            // stays false, so there is never more than one writer.
            bool realWriteActiveThisFrame = g_enableRealCameraWrite
                && !IsCameraWriteHookEnabled()
                && ReadRealFpvFlag();

            if (realWriteActiveThisFrame) {
                uintptr_t base = GetMgsiModuleBase();

                ApplyCameraNopPatchesOnce(base);

                // RadToPSX conversion: CONFIRMED via the FoxdieTeam PS1
                // decompilation's real source code (source/game/camera.h
                // area): "SVECTOR rot; // current orientation (4096 = 2Pi
                // rad)" and "SVECTOR turn; // rotation vector (4096 = 2Pi
                // rad)" -- both comments explicit and unambiguous, plus
                // GM_SnakeCamera.rotate2.vy = 2048 appearing repeatedly
                // across the codebase specifically to mean "180 degrees"
                // (half of 4096). This is authoritative, real source code,
                // not inference -- confirms BOTH axes use the same uniform
                // 12-bit (0-4095) range. An earlier version of this
                // function tried asymmetric scaling based on reading the
                // reference Cheat Engine table's UI trackbar reset values
                // (RX defaulting to 2048, RY to 1024) as implying different
                // ranges -- that was a misread of the UI (RY=1024 more
                // likely reflects that tool's chosen default VIEWING angle,
                // not a smaller total range) and is reverted here now that
                // real source code settles it.
                auto RadToPSX12 = [](float rad) -> int16_t {
                    float normalized = rad + PI;
                    while (normalized < 0.0f) normalized += 2.0f * PI;
                    while (normalized >= 2.0f * PI) normalized -= 2.0f * PI;
                    return static_cast<int16_t>((normalized / (2.0f * PI)) * 4096.0f) & 0x0FFF;
                    };

                int16_t yawTarget = RadToPSX12(yaw);
                int16_t pitchTarget = RadToPSX12(pitch);

                // INCREMENTAL DELTA WRITE (new test): rather than writing an
                // absolute target angle every frame, read the CURRENT live
                // value and step a small amount toward the target -- mimicking
                // how real mouse-look actually feeds this address (small,
                // continuous per-frame deltas), which was confirmed clean and
                // flicker-free via direct Cheat Engine trace, unlike both our
                // own absolute writes and manual absolute edits in Cheat
                // Engine itself (both of which showed the same
                // recognize-then-revert flicker). Theory: some per-frame
                // logic may expect/tolerate small steps but reject/revert
                // large jumps.
                int16_t yawVal, pitchVal;
                if (g_useIncrementalCameraWrite) {
                    int16_t currentYaw = SafeRead16(base + kCamRotYOffset) & 0x0FFF;
                    int16_t currentPitch = SafeRead16(base + kCamRotXOffset) & 0x0FFF;

                    auto StepToward = [](int16_t current, int16_t target, int16_t maxStep) -> int16_t {
                        // Shortest-path delta on a 12-bit wraparound circle (0-4095)
                        int32_t diff = (int32_t)target - (int32_t)current;
                        if (diff > 2048) diff -= 4096;
                        if (diff < -2048) diff += 4096;
                        if (diff > maxStep) diff = maxStep;
                        if (diff < -maxStep) diff = -maxStep;
                        int32_t result = (int32_t)current + diff;
                        while (result < 0) result += 4096;
                        while (result >= 4096) result -= 4096;
                        return (int16_t)result;
                        };

                    // DENSE DIAGNOSTIC (every frame, first 300 ticks only,
                    // to test whether "current" genuinely accumulates
                    // frame-to-frame or keeps getting reset toward zero by
                    // something else between our writes -- the coarse
                    // every-120-ticks logging couldn't distinguish these.
                    // Writes to the separate low-noise test log, not the
                    // main debug log.
                    static int denseLogCount = 0;
                    if (denseLogCount < 300) {
                        DebugLogger::TestLogFormat("Dense[%d]: currentYaw=%d yawTarget=%d | currentPitch=%d pitchTarget=%d",
                            denseLogCount, (int)currentYaw, (int)yawTarget, (int)currentPitch, (int)pitchTarget);
                        denseLogCount++;
                    }

                    yawVal = StepToward(currentYaw, yawTarget, g_maxCameraStepPerFrame);
                    pitchVal = StepToward(currentPitch, pitchTarget, g_maxCameraStepPerFrame);
                }
                else {
                    yawVal = yawTarget;
                    pitchVal = pitchTarget;
                }

                bool wroteYaw = SafeWrite16(base + kCamRotYOffset, yawVal);
                bool wrotePitch = SafeWrite16(base + kCamRotXOffset, pitchVal);

                // ALSO write to the second, newly-discovered camera block
                // (uppercase X/Y/Z/Rotate, next to its own FPV byte flag) --
                // testing the theory that THIS is the block the game
                // actually renders from for first-person view, while the
                // block above may be a free-camera-only value.
                // NOTE (new this session): reading patch 2's own script
                // MAJOR CORRECTION (confirmed via real FoxdieTeam PS1
                // decompilation source, not inference): GM_SnakeCameraWork
                // has TWO rotation fields -- rotate (offset +0x10) and
                // rotate2 (offset +0x28). Real camera-update code
                // (camera_act_helper_helper_8002F008, InterpIntoSubject)
                // proves the actually-rendered camera's rotation comes from
                // rotate2 via GV_NearExp4PV(&GM_Camera.rotate,
                // &GM_SnakeCamera.rotate2, 3) -- an INTERPOLATION, not an
                // instant copy, smoothing GM_Camera.rotate TOWARD rotate2
                // over time. Computing real offsets from our confirmed
                // position address (593FA0 = struct offset 0x00):
                //   rotate  (+0x10) = 593FB0  <- this is what we'd been
                //                                writing here (WRONG target,
                //                                actively fights the game's
                //                                own interpolation source)
                //   rotate2 (+0x28) = 593FC8  <- kCamRotYOffset, our
                //                                ORIGINAL address from the
                //                                very start of this whole
                //                                investigation, which was
                //                                right all along
                // STOPPING the write to 593FB0/593FB2 here entirely -- it's
                // a different, real field the game itself both reads AND
                // writes as part of its own smoothing, and our writing to
                // it was very likely the actual source of the
                // "recognizes direction then reverts" symptom throughout
                // this whole session, not a scale or patch issue.


                // Read back immediately after writing, to check whether
                // something (game logic, or our own free-cam flag not
                // actually taking effect) overwrites the value again before
                // the next frame -- this is the direct test of "is the write
                // actually sticking through to when the game renders,"
                // which we've never explicitly verified before.
                int16_t yawReadBack = SafeRead16(base + kCamRotYOffset);
                int16_t pitchReadBack = SafeRead16(base + kCamRotXOffset);

                // BURST SAMPLING: user reported a "recognizes direction then
                // reverts" flicker after the NOP patches -- this samples the
                // same address rapidly over the next ~80ms (finer than our
                // normal once-per-tick logging) to see if a revert happens
                // WITHIN this window, which per-tick logging alone would
                // completely miss. Only runs for the first few real-write
                // frames to avoid spamming the log every frame.
                static int burstSampleRuns = 0;
                if (burstSampleRuns < 3) {
                    burstSampleRuns++;
                    std::string burstLine = "Burst sample yaw: ";
                    for (int i = 0; i < 8; ++i) {
                        int16_t sample = SafeRead16(base + kCamRotYOffset);
                        char buf[16];
                        sprintf_s(buf, "%d ", (int)sample);
                        burstLine += buf;
                        Sleep(10);
                    }
                    DebugLogger::TestLog(burstLine);
                }

                if (!wroteYaw || !wrotePitch) {
                    static bool loggedFail = false;
                    if (!loggedFail) {
                        DebugLogger::Log("Real camera write: failed to write rotation (unmapped/protected memory) -- disabling for this session");
                        loggedFail = true;
                        g_enableRealCameraWrite = false;
                        realWriteActiveThisFrame = false;
                    }
                }

                // Periodic (not just first-frame) diagnostic, so ongoing
                // per-frame behavior can actually be verified in the log --
                // a one-shot log line couldn't tell us whether later frames
                // were still writing correctly, which mattered for
                // diagnosing the "only stick worked" report.
                static int realWriteLogCount = 0;
                if (realWriteLogCount < 5 || realWriteLogCount % 120 == 0) {
                    // Split into separate calls (rather than one call mixing
                    // int16_t and float args) to rule out any variadic
                    // argument-passing ambiguity as the cause of a garbled
                    // pitch value seen in an earlier test run.
                    DebugLogger::LogFormat("Real camera write [tick %d]: yawVal=%d pitchVal=%d | READBACK yaw=%d pitch=%d | nopPatchesApplied=%d",
                        realWriteLogCount, (int)yawVal, (int)pitchVal, (int)yawReadBack, (int)pitchReadBack, g_cameraNopPatchesApplied ? 1 : 0);
                    DebugLogger::LogFormat("Real camera write [tick %d]: yaw_rad=%.4f pitch_rad=%.4f",
                        realWriteLogCount, yaw, pitch);
                    DebugLogger::LogFormat("Real camera write [tick %d]: raw quat x=%.4f y=%.4f z=%.4f w=%.4f",
                        realWriteLogCount, g_xrViews[0].pose.orientation.x, g_xrViews[0].pose.orientation.y,
                        g_xrViews[0].pose.orientation.z, g_xrViews[0].pose.orientation.w);
                }
                realWriteLogCount++;
            }

            // IMPORTANT: the old mouse-emulation path is now SKIPPED
            // entirely whenever the real write is active this frame. Both
            // paths writing related rotation state in the same frame (one
            // directly, one indirectly via the game's own mouse-input
            // handling) was the most likely reason a prior test appeared to
            // show only the controller stick affecting the camera --
            // whichever path's effect landed last silently won, with no way
            // to tell which from the logs at the time. Only one path should
            // ever be authoritative per frame.
            if (!realWriteActiveThisFrame && haveLast) {
                float dYaw = NormalizeAngleDelta(yaw - lastYaw);
                float dPitch = NormalizeAngleDelta(pitch - lastPitch);

                float mouseDx = dYaw * g_hmdLookSensitivityYaw;
                float mouseDy = -dPitch * g_hmdLookSensitivityPitch;

                std::lock_guard<std::mutex> lock(g_hmdLookMutex);
                g_hmdAccumDeltaX += mouseDx;
                g_hmdAccumDeltaY += mouseDy;
            }

            lastYaw = yaw;
            lastPitch = pitch;
            haveLast = true;
        }

        bool submittedProjectionLayer = false;
        bool submittedNativeScreen = false;
        // Sampled ONCE per frame and used for every decision below. Read fresh
        // at each use site it could change mid-frame -- the input thread can
        // flip it at any instant -- and this frame would render one eye but
        // submit a two-view projection layer, or pin a screen pose it then does
        // not use, or hang the aim laser over a virtual screen.
        const bool vrModeFrame = IsVrViewModeActive();
        // A cutscene shot without Snake in it (Snake's-eyes mode) uses the same
        // screen: there is no head to be inside, so it is watched, not entered.
        const bool cutsceneScreenFrame = vrModeFrame && g_nativeScreenEnabled &&
                                         IsCutsceneScreenFallbackActive();
        // The codec: same screen, from the frame it opens to the frame it closes.
        const bool codecScreenFrame = vrModeFrame && g_nativeScreenEnabled && CodecScreenWanted();
        const bool menuScreenFrame = vrModeFrame && g_nativeScreenEnabled &&
                                     ((g_menuAsScreen && IsDdrawMenuOverlayActive()) || cutsceneScreenFrame ||
                                      codecScreenFrame);
        const bool nativeScreenFrame = (!vrModeFrame || menuScreenFrame) && g_nativeScreenEnabled;
        static bool s_prevMenuScreen = false;
        const bool menuScreenOpened = menuScreenFrame && !s_prevMenuScreen;
        if (menuScreenFrame != s_prevMenuScreen) {
            static bool s_prevCutsceneScreen = false;
            const bool labelCutscene = menuScreenFrame ? cutsceneScreenFrame : s_prevCutsceneScreen;
            s_prevCutsceneScreen = cutsceneScreenFrame;
            DebugLogger::LogFormat("%s %s -- %s", labelCutscene ? "Cutscene shot without Snake" :
                (codecScreenFrame || (!menuScreenFrame && IsCodecOpen())) ? "Codec screen" : "PC menu",
                menuScreenFrame ? "OPEN" : "closed",
                menuScreenFrame ? "showing it on a virtual screen in front of you"
                                : "back to the VR view");
        }
        s_prevMenuScreen = menuScreenFrame;

        if (g_xrRenderTargetsReady && frameState.shouldRender && locateResult == XR_SUCCESS && viewCountOutput > 0 && !g_xrProjectionViews.empty()) {
            uint32_t viewCountToRender = viewCountOutput;
            if (viewCountToRender > (uint32_t)g_xrProjectionViews.size()) {
                viewCountToRender = (uint32_t)g_xrProjectionViews.size();
            }
            if (viewCountToRender > 2) {
                viewCountToRender = 2;
            }
            // One eye is all a mono screen needs, and rendering the second
            // would be identical pixels thrown away.
            if (nativeScreenFrame) {
                viewCountToRender = 1;
            }

            bool haveCapturedMonoFrame = false;
            LARGE_INTEGER xrCapT0, xrCapT1, xrQpcF;
            QueryPerformanceCounter(&xrCapT0);
            if (EnsureMonoCaptureResources(g_xrEyeTargets[0].width, g_xrEyeTargets[0].height)) {
                haveCapturedMonoFrame = CaptureGameFrameToMonoTexture();
                // Render twice hands a stereo pair out one eye per call. Take
                // the rest of it now so both eyes of one game frame are shown
                // in the same XR frame (never left from one frame, right from
                // another).
                for (int extra = 0; extra < 2 && GetPendingDdrawFrameCount() > 0; ++extra) {
                    haveCapturedMonoFrame = CaptureGameFrameToMonoTexture() || haveCapturedMonoFrame;
                }
            }
            // XR PERF: how long this compositor frame spent taking the game's
            // image in (copy, wrist HUD crops, upload, resample). The whole XR
            // frame budget is 11-14 ms at 90/72 Hz; this must stay well inside it.
            {
                QueryPerformanceCounter(&xrCapT1);
                QueryPerformanceFrequency(&xrQpcF);
                const double capMs = (double)(xrCapT1.QuadPart - xrCapT0.QuadPart) * 1000.0 / (double)xrQpcF.QuadPart;
                static double sAcc = 0.0, sMax = 0.0;
                static int sFrames = 0, sOver4 = 0, sOver8 = 0;
                static ULONGLONG sStart = 0;
                static LARGE_INTEGER sLastLoop{};
                static double sGapMax = 0.0;
                static int sGapLate = 0;
                if (sLastLoop.QuadPart != 0) {
                    const double gap = (double)(xrCapT0.QuadPart - sLastLoop.QuadPart) * 1000.0 / (double)xrQpcF.QuadPart;
                    const double period = frameState.predictedDisplayPeriod > 0 ? (double)frameState.predictedDisplayPeriod / 1.0e6 : 13.9;
                    if (gap > sGapMax) sGapMax = gap;
                    if (gap > period * 1.5) sGapLate++;
                }
                sLastLoop = xrCapT0;
                sAcc += capMs; if (capMs > sMax) sMax = capMs;
                sFrames++; if (capMs > 4.0) sOver4++; if (capMs > 8.0) sOver8++;
                const ULONGLONG nowTick = GetTickCount64();
                if (sStart == 0) sStart = nowTick;
                if (nowTick - sStart >= 5000) {
                    DebugLogger::LogFormat("XR PERF: %d compositor frames in 5 s (%.1f Hz) | taking the game image in: avg %.2f ms, worst %.2f ms, %d frames over 4 ms, %d over 8 ms | "
                        "frame-to-frame worst %.1f ms, %d frames later than 1.5x the display period | source %dx%d",
                        sFrames, sFrames / ((nowTick - sStart) / 1000.0), sAcc / (sFrames ? sFrames : 1), sMax, sOver4, sOver8,
                        sGapMax, sGapLate, (int)g_sourceCaptureWidth, (int)g_sourceCaptureHeight);
                    sAcc = 0.0; sMax = 0.0; sFrames = 0; sOver4 = 0; sOver8 = 0; sGapMax = 0.0; sGapLate = 0; sStart = nowTick;
                }
            }

            static int frameLoopLogCount = 0;
            if (frameLoopLogCount < 5 || frameLoopLogCount % 180 == 0) {
                DebugLogger::LogFormat("Frame loop #%d: haveCapturedMonoFrame=%d viewCountToRender=%u g_xrMonoCaptureTexture=%p captureWH=%dx%d",
                    frameLoopLogCount, haveCapturedMonoFrame ? 1 : 0, viewCountToRender, (void*)g_xrMonoCaptureTexture, g_captureWidth, g_captureHeight);
            }
            frameLoopLogCount++;

            for (uint32_t eye = 0; eye < viewCountToRender; ++eye) {
                XrEyeRenderTarget& target = g_xrEyeTargets[eye];
                if (target.swapchain == XR_NULL_HANDLE || target.images.empty()) {
                    continue;
                }

                uint32_t imageIndex = 0;
                XrSwapchainImageAcquireInfo acquireInfo{ XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
                if (xrAcquireSwapchainImage(target.swapchain, &acquireInfo, &imageIndex) != XR_SUCCESS) {
                    continue;
                }

                XrSwapchainImageWaitInfo waitImageInfo{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
                waitImageInfo.timeout = XR_INFINITE_DURATION;
                if (xrWaitSwapchainImage(target.swapchain, &waitImageInfo) == XR_SUCCESS && imageIndex < target.rtvs.size()) {
                    ID3D11RenderTargetView* rtv = target.rtvs[imageIndex];
                    ID3D11Texture2D* dstTexture = target.images[imageIndex].texture;
                    if (rtv) {
                        FLOAT clearColor[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
                        g_xrD3DContext->OMSetRenderTargets(1, &rtv, nullptr);
                        g_xrD3DContext->ClearRenderTargetView(rtv, clearColor);

                        static int blitLogCount = 0;
                        bool logThisBlit = (blitLogCount < 5) || (blitLogCount % 180 == 0);
                        blitLogCount++;

                        if (haveCapturedMonoFrame && dstTexture) {
                            if (logThisBlit) {
                                D3D11_TEXTURE2D_DESC srcDesc{};
                                if (g_xrMonoCaptureTexture) g_xrMonoCaptureTexture->GetDesc(&srcDesc);
                                D3D11_TEXTURE2D_DESC dstDesc{};
                                dstTexture->GetDesc(&dstDesc);
                                DebugLogger::LogFormat(
                                    "Blit#%d eye=%u: src(mono capture)=%ux%u fmt=%d bind=0x%X | dst(swapchain img)=%ux%u fmt=%d bind=0x%X imageIndex=%u",
                                    blitLogCount, eye,
                                    srcDesc.Width, srcDesc.Height, (int)srcDesc.Format, (unsigned)srcDesc.BindFlags,
                                    dstDesc.Width, dstDesc.Height, (int)dstDesc.Format, (unsigned)dstDesc.BindFlags,
                                    imageIndex);
                            }
                            BlitMonoTextureToEyeSwapchain(eye, dstTexture);
                            if (logThisBlit) {
                                DebugLogger::LogFormat("Blit#%d eye=%u: CopySubresourceRegion issued (no error return available from this D3D11 call -- errors surface via debug layer or TDR only)", blitLogCount, eye);
                            }
                        }
                        else {
                            // Fallback debug colors if desktop capture is unavailable.
                            FLOAT debugColor[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
                            if (g_fallbackDebugColors) {
                                debugColor[0] = eye == 0 ? 1.0f : 0.0f;
                                debugColor[1] = eye == 0 ? 0.0f : 1.0f;
                            }
                            g_xrD3DContext->ClearRenderTargetView(rtv, debugColor);
                            if (logThisBlit) {
                                DebugLogger::LogFormat("Blit#%d eye=%u: no game frame yet, showing the %s fallback (haveCapturedMonoFrame=%d dstTexture=%p)", blitLogCount, eye, g_fallbackDebugColors ? "RED/GREEN" : "black", haveCapturedMonoFrame ? 1 : 0, (void*)dstTexture);
                            }
                        }
                    }
                    else {
                        static bool loggedNullRtv = false;
                        if (!loggedNullRtv) {
                            DebugLogger::LogFormat("WARNING: rtv is null for eye=%u imageIndex=%u -- nothing drawn to this swapchain image at all", eye, imageIndex);
                            loggedNullRtv = true;
                        }
                    }
                }
                else {
                    static bool loggedWaitFail = false;
                    if (!loggedWaitFail) {
                        DebugLogger::LogFormat("WARNING: xrWaitSwapchainImage failed or imageIndex out of range (imageIndex=%u rtvs.size=%zu)", imageIndex, target.rtvs.size());
                        loggedWaitFail = true;
                    }
                }

                XrSwapchainImageReleaseInfo releaseInfo{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
                xrReleaseSwapchainImage(target.swapchain, &releaseInfo);

                // ---- POSE ------------------------------------------------
                // The pose THIS eye's image was actually rendered from, not
                // the live one. That is what lets the runtime timewarp a
                // stale eye forward instead of compositing it as if current,
                // which is the whole fix for the one-eye drag.
                const FrameViewRecord& eyeRec = g_eyeRec[eye];
                const bool recUsable = g_stereoPoseTagging && eyeRec.valid && haveCapturedMonoFrame;
                if (recUsable) {
                    g_xrProjectionViews[eye].pose = PoseFromRecord(eyeRec, (int)eye);
                }
                else if (g_stereoPoseTagging && g_haveEyeSubmitPose[eye]) {
                    g_xrProjectionViews[eye].pose = g_eyeSubmitPose[eye];
                }
                else {
                    g_xrProjectionViews[eye].pose = g_xrViews[eye].pose;
                }

                g_xrProjectionViews[eye].subImage.swapchain = target.swapchain;
                g_xrProjectionViews[eye].subImage.imageArrayIndex = 0;

                // ---- FOV -------------------------------------------------
                // Either claim what we actually rendered (symmetric frustum
                // from submitted_hfov_deg, sub-image narrowed to the drawn
                // pixels), or fall back to the legacy claim that the 4:3
                // picture fills the runtime's whole asymmetric eye frustum.
                // WORLD LOCK needs the claim to be EXACT. With the legacy
                // claim the game's ~67 deg picture was stretched across the
                // headset's ~100 deg eye, so a 10 deg head turn moved the
                // drawn world ~15 deg: it swam against your head, and no
                // amount of pose tagging can hold still a picture that is
                // magnified. A head-driven frame now claims exactly the frustum
                // it was projected with, read from the frame itself (so it
                // follows fov_clip_distance, room overrides and FPV entry).
                float claimHfovDeg = g_submittedHfovDeg;
                if (g_fovClaimFromFrame && recUsable && eyeRec.clip > 0) {
                    claimHfovDeg = (float)(2.0 * std::atan(160.0 / (double)eyeRec.clip) * 180.0 / PI);
                }
                // PSG1 SCOPE (2026-09-29). A frame drawn at the scope's zoom is
                // CLAIMED wider than it was drawn -- that difference is the
                // magnification, exactly as a real scope shows a narrow slice of
                // the world across a bigger angle. Keyed on the frame's own clip
                // so a frame drawn before or after the zoom is never magnified.
                if (IsScopeFeatureEnabled() && recUsable && eyeRec.clip == GetScopeClipDistance()) {
                    claimHfovDeg = GetScopeDisplayDeg();
                }
                if (claimHfovDeg > 0.0f && g_drawRectW > 0 && g_drawRectH > 0) {
                    // CROP-AWARE (2026-08-16). submitted_hfov_deg describes
                    // the frustum the GAME RENDERED. Crop-to-fill throws part
                    // of that frustum away before it reaches the layer, so
                    // claiming the full angle over the cropped image
                    // over-claims by exactly 1/visibleFrac -- the world would
                    // shear as you turn, which is the precise failure this
                    // whole symmetric-claim mechanism exists to avoid.
                    // Narrowing the claim by the visible fraction is a
                    // tangent-space scale, not an angle scale, because the
                    // crop is linear in the projection plane, not in angle.
                    //
                    // At crop_to_fill_percent=0 the fractions are 1.0 and
                    // this reduces exactly to the previous expression.
                    float fx = g_visibleSrcFracX;
                    float fy = g_visibleSrcFracY;
                    if (fx <= 0.0f || fx > 1.0f) fx = 1.0f;
                    if (fy <= 0.0f || fy > 1.0f) fy = 1.0f;

                    // image_scale_percent / ui_image_scale_percent shrink the
                    // drawn rect in both axes. Without folding that in here,
                    // those knobs do NOTHING once submitted_hfov_deg is set:
                    // the sub-image narrows to the smaller rect and the claim
                    // is built from its (unchanged) aspect, so the picture
                    // subtends exactly the same angle as before. Scaling the
                    // claim by the same factor is what actually pushes the
                    // image away -- and it scales both axes together, so the
                    // aspect relationship below is untouched.
                    float scale = g_drawScaleFactor;
                    if (scale <= 0.0f || scale > 1.0f) scale = 1.0f;

                    const float halfHFull = claimHfovDeg * 0.5f * (PI / 180.0f);
                    const float halfH = std::atan(std::tan(halfHFull) * fx * scale);
                    // Vertical needs NO separate crop term. The drawn rect's
                    // aspect is the CROPPED source's aspect, so
                    //   tan(halfV) = tan(halfH) / drawnAspect
                    // already carries the crop through correctly in both
                    // directions. Worked through for the horizontal-crop case
                    // (drawnAspect = srcAspect*fx, halfH scaled by fx, so the
                    // fx cancels and vertical is untouched -- which is right,
                    // cropping the sides does not change vertical FOV) and for
                    // the vertical-crop case (drawnAspect = srcAspect/fy,
                    // halfH unscaled, so vertical narrows by fy -- also right).
                    // image_scale_percent scales width and height together and
                    // therefore does not enter here at all.
                    const float aspect = (float)g_drawRectW / (float)g_drawRectH;
                    const float halfV = std::atan(std::tan(halfH) / aspect);
                    XrFovf claimed{};
                    claimed.angleLeft = -halfH;
                    claimed.angleRight = halfH;
                    claimed.angleUp = halfV;
                    claimed.angleDown = -halfV;
                    g_xrProjectionViews[eye].fov = claimed;
                    g_xrProjectionViews[eye].subImage.imageRect.offset = { g_drawRectX, g_drawRectY };
                    g_xrProjectionViews[eye].subImage.imageRect.extent = { g_drawRectW, g_drawRectH };

                    // Re-logged whenever the claim actually changes, not just
                    // twice at startup: the claim now moves with the 2D-UI
                    // framing blend and with crop_to_fill_percent, so a
                    // two-shot log would only ever show the startup value and
                    // silently hide every later change.
                    static float lastClaimH = -1.0f, lastClaimV = -1.0f;
                    const float claimHDeg = halfH * 2.0f * 180.0f / PI;
                    const float claimVDeg = halfV * 2.0f * 180.0f / PI;
                    if (std::abs(claimHDeg - lastClaimH) > 0.5f ||
                        std::abs(claimVDeg - lastClaimV) > 0.5f) {
                        lastClaimH = claimHDeg;
                        lastClaimV = claimVDeg;
                        DebugLogger::LogFormat(
                            "FOV claim: symmetric %.1f deg H x %.1f deg V over rect %dx%d at +%d,+%d "
                            "(rendered %.1f deg H, visible fraction %.3fx%.3f after crop, image scale %.2f)",
                            claimHDeg, claimVDeg,
                            g_drawRectW, g_drawRectH, g_drawRectX, g_drawRectY,
                            claimHfovDeg, fx, fy, scale);
                    }
                }
                else {
                    g_xrProjectionViews[eye].fov = g_xrViews[eye].fov;
                    g_xrProjectionViews[eye].subImage.imageRect.offset = { 0, 0 };
                    g_xrProjectionViews[eye].subImage.imageRect.extent = { target.width, target.height };
                }

                // Tell motion aim where the game picture actually lands in this
                // eye, so the gun and the bullets line up with what is DRAWN
                // under the laser whichever FOV branch ran above.
                MotionAimPublishEyeImage(eye, g_xrProjectionViews[eye].fov,
                    g_xrProjectionViews[eye].subImage.imageRect.offset.x,
                    g_xrProjectionViews[eye].subImage.imageRect.offset.y,
                    g_xrProjectionViews[eye].subImage.imageRect.extent.width,
                    g_xrProjectionViews[eye].subImage.imageRect.extent.height,
                    g_drawRectX, g_drawRectY, g_drawRectW, g_drawRectH,
                    g_visibleSrcFracX, g_visibleSrcFracY);
            }

            if (nativeScreenFrame) {
                // ---- NATIVE MODE: the world-locked screen -------------------
                // Same pixels the projection path would have shown, submitted
                // as a quad at a fixed pose instead of as your eyes.
                //
                // The swapchain check is not ceremony. The eye loop above
                // `continue`s past a null swapchain, so reaching here with
                // nothing to point at is possible -- and a quad layer whose
                // subImage names XR_NULL_HANDLE is undefined behaviour in the
                // runtime rather than a tidy no-op. Submitting no layer at all
                // costs one black frame, which is the cheap failure.
                //
                // Note this deliberately does NOT fall through to the
                // projection branch: mixing the two on alternate frames would
                // flip the picture between a world-locked screen and a
                // head-locked one, which is far more unpleasant than a dropped
                // frame.
                const XrEyeRenderTarget& src = g_xrEyeTargets[0];
                if (src.swapchain != XR_NULL_HANDLE) {

                    // Recentre on request (entering native mode posts one) and on
                    // the first frame we ever need a pose.
                    if (ConsumeVirtualScreenRecenter() || !g_nativeScreenPosed || menuScreenOpened) {
                        PinNativeScreenTo(g_xrViews[0].pose);
                        DebugLogger::LogFormat(
                            "Native screen: placed at (%.2f, %.2f, %.2f) facing where you were looking. "
                            "It stays there now -- turn your head and it will not follow.",
                            g_nativeScreenPose.position.x, g_nativeScreenPose.position.y,
                            g_nativeScreenPose.position.z);
                    }

                    // Show exactly the rect the game image was drawn into, so the
                    // letterbox bars the eye buffer needs never reach the quad.
                    // Falling back to the whole buffer if the framing pass has not
                    // run yet costs one frame of black border, which is better than
                    // a zero-extent sub-image (undefined behaviour in the runtime).
                    int32_t rx = g_drawRectX, ry = g_drawRectY;
                    int32_t rw = g_drawRectW, rh = g_drawRectH;
                    if (rw <= 0 || rh <= 0 ||
                        rx < 0 || ry < 0 ||
                        rx + rw > src.width || ry + rh > src.height) {
                        rx = 0; ry = 0; rw = src.width; rh = src.height;
                    }

                    g_nativeScreenQuad.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
                    g_nativeScreenQuad.next = nullptr;
                    g_nativeScreenQuad.layerFlags = 0;   // opaque; nothing behind it
                    g_nativeScreenQuad.space = g_xrPlaySpace;
                    g_nativeScreenQuad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                    g_nativeScreenQuad.subImage.swapchain = src.swapchain;
                    g_nativeScreenQuad.subImage.imageRect.offset = { rx, ry };
                    g_nativeScreenQuad.subImage.imageRect.extent = { rw, rh };
                    g_nativeScreenQuad.subImage.imageArrayIndex = 0;
                    g_nativeScreenQuad.pose = g_nativeScreenPose;
                    // Height from the drawn rect's aspect, so a 4:3 game is a 4:3
                    // screen whatever the eye buffer's shape happens to be.
                    g_nativeScreenQuad.size.width = g_nativeScreenWidthM;
                    g_nativeScreenQuad.size.height =
                        g_nativeScreenWidthM * ((float)rh / (float)rw);
                    submittedNativeScreen = true;

                    static bool loggedNativeSubmit = false;
                    if (!loggedNativeSubmit) {
                        loggedNativeSubmit = true;
                        DebugLogger::LogFormat(
                            "Native screen: submitting a quad layer (%dx%d from the eye-0 swapchain, "
                            "%.2f x %.2f m). One eye rendered, one layer submitted -- strictly less GPU "
                            "and compositor work than VR mode's two-view projection layer.",
                            rw, rh, g_nativeScreenQuad.size.width, g_nativeScreenQuad.size.height);
                    }
                }
            }
            else {
                g_xrProjectionLayer.space = g_xrPlaySpace;
                g_xrProjectionLayer.viewCount = viewCountToRender;
                g_xrProjectionLayer.views = g_xrProjectionViews.data();
                submittedProjectionLayer = true;
            }
        }

        XrFrameEndInfo endInfo{ XR_TYPE_FRAME_END_INFO };
        endInfo.displayTime = frameState.predictedDisplayTime;
        endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;

        // A vector now, because the laser appends a variable number of quads.
        // static so the storage trivially outlives this scope -- xrEndFrame
        // reads through these pointers before returning, but keeping them
        // alive by construction is cheaper than reasoning about it.
        static std::vector<const XrCompositionLayerBaseHeader*> layers;
        layers.clear();

        if (submittedProjectionLayer) {
            layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&g_xrProjectionLayer));
            static bool loggedProjectionSubmit = false;
            if (!loggedProjectionSubmit) {
                DebugLogger::Log("OpenXR projection layer submission active (alternate-eye stereo with per-eye pose tagging; debug colors on capture fallback)");
                loggedProjectionSubmit = true;
            }
        }
        else if (submittedNativeScreen) {
            layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&g_nativeScreenQuad));
        }

        // The laser goes on TOP, and deliberately also submits when there is
        // no projection layer at all: "no game image but the dots are there"
        // localises a capture failure to the capture path in one glance.
        //
        // NOT IN NATIVE MODE, though. The laser is an aiming aid for a camera
        // the mod is driving; floating it over a virtual screen would point it
        // at a picture rather than at anything in the world, and it would sit
        // in front of the screen at a completely unrelated depth. Native mode
        // means the game as it shipped, and the game as it shipped has no
        // laser.
        if (locateResult == XR_SUCCESS && viewCountOutput > 0 && vrModeFrame && !menuScreenFrame) {
            BuildAimLaserLayers(frameState.predictedDisplayTime,
                                g_xrViews[0].pose, layers);
            // Wrist HUD (LIFE on the left wrist, weapon on the right). Same
            // rules as the laser: VR mode only, never over a menu screen.
            BuildWristHudLayers(frameState.predictedDisplayTime, g_xrViews[0].pose, layers);
        }

        endInfo.layerCount = (uint32_t)layers.size();
        endInfo.layers = layers.empty() ? nullptr : layers.data();

        const XrResult endResult = xrEndFrame(g_xrSession, &endInfo);
        if (endResult != XR_SUCCESS) {
            static int endFailLogs = 0;
            if (endFailLogs < 10) {
                endFailLogs++;
                DebugLogger::LogFormat("xrEndFrame failed: %d", (int)endResult);
            }
        }
    }

    DebugLogger::Log("OpenXR frame thread stopped");
    return 0;
}

static void StartXrFrameThreadIfNeeded() {
    if (!g_enableOpenXrSession || !g_xrSessionReady || !g_xrSessionRunning) {
        return;
    }

    if (g_xrFrameThreadRunning.load()) {
        return;
    }

    g_xrFrameThreadRunning = true;
    g_xrFrameThreadHandle = CreateThread(nullptr, 0, XrFrameThreadProc, nullptr, 0, nullptr);
    if (!g_xrFrameThreadHandle) {
        g_xrFrameThreadRunning = false;
        DebugLogger::Log("ERROR: Failed to create OpenXR frame thread");
    }
    else {
        DebugLogger::Log("OpenXR frame thread created");
    }
}

void ShutdownVrRuntime() {
    // Stop overriding first, then restore the original instruction bytes, so
    // the game's own camera update is intact again before anything else winds
    // down.
    SetCameraWriteHookEnabled(false);
    RemoveCameraWriteHook();

    g_runtimeTickThreadRunning = false;
    if (g_runtimeTickThreadHandle) {
        WaitForSingleObject(g_runtimeTickThreadHandle, 1000);
        CloseHandle(g_runtimeTickThreadHandle);
        g_runtimeTickThreadHandle = nullptr;
    }

    g_xrFrameThreadRunning = false;
    if (g_xrFrameThreadHandle) {
        WaitForSingleObject(g_xrFrameThreadHandle, 1000);
        CloseHandle(g_xrFrameThreadHandle);
        g_xrFrameThreadHandle = nullptr;
    }

    CleanupOpenXrRenderTargets();
}

static DWORD WINAPI RuntimeTickThreadProc(LPVOID) {
    DebugLogger::Log("Runtime tick fallback thread started");
    while (g_runtimeTickThreadRunning.load()) {
        if (InterlockedCompareExchange(&g_RenderHookSeen, 0, 0) == 0) {
            VRTickFromRenderer();
            Sleep(8);
        }
        else {
            Sleep(50);
        }
    }
    DebugLogger::Log("Runtime tick fallback thread stopped");
    return 0;
}

static void StartRuntimeTickThread() {
    if (g_runtimeTickThreadRunning.load()) {
        return;
    }

    g_runtimeTickThreadRunning = true;
    g_runtimeTickThreadHandle = CreateThread(nullptr, 0, RuntimeTickThreadProc, nullptr, 0, nullptr);
    if (!g_runtimeTickThreadHandle) {
        g_runtimeTickThreadRunning = false;
        DebugLogger::Log("ERROR: Failed to create runtime tick fallback thread");
    }
}

static void TryInitializeOpenXRSession() {
    if (!g_enableOpenXrSession) {
        return;
    }

    if (g_xrInstance == XR_NULL_HANDLE || g_xrSessionReady) {
        return;
    }

    if (g_xrSession != XR_NULL_HANDLE && g_xrPlaySpace != XR_NULL_HANDLE) {
        if (!g_xrRenderTargetsReady) {
            if (!InitializeOpenXrRenderTargets()) {
                DebugLogger::Log("WARNING: Projection render target init failed; continuing in input-only OpenXR mode");
            }
        }
        g_xrSessionReady = true;
        return;
    }

    XrSystemGetInfo systemInfo{ XR_TYPE_SYSTEM_GET_INFO };
    systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;

    XrResult result = xrGetSystem(g_xrInstance, &systemInfo, &g_xrSystemId);
    if (result != XR_SUCCESS) {
        DebugLogger::LogFormat("xrGetSystem failed: %d", (int)result);
        return;
    }

    DebugLogger::LogFormat("xrGetSystem succeeded. SystemId=%llu", (unsigned long long)g_xrSystemId);

    PFN_xrGetD3D11GraphicsRequirementsKHR pfnGetRequirements = nullptr;
    result = xrGetInstanceProcAddr(g_xrInstance, "xrGetD3D11GraphicsRequirementsKHR", (PFN_xrVoidFunction*)&pfnGetRequirements);
    if (result != XR_SUCCESS || !pfnGetRequirements) {
        DebugLogger::LogFormat("xrGetInstanceProcAddr(xrGetD3D11GraphicsRequirementsKHR) failed: %d", (int)result);
        return;
    }

    if (!CreateOpenXrD3D11Device()) {
        return;
    }

    XrGraphicsRequirementsD3D11KHR requirements{ XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR };
    result = pfnGetRequirements(g_xrInstance, g_xrSystemId, &requirements);
    if (result != XR_SUCCESS) {
        DebugLogger::LogFormat("xrGetD3D11GraphicsRequirementsKHR failed: %d", (int)result);
        return;
    }

    XrGraphicsBindingD3D11KHR graphicsBinding{ XR_TYPE_GRAPHICS_BINDING_D3D11_KHR };
    graphicsBinding.device = g_xrD3DDevice;

    XrSessionCreateInfo sessionCreateInfo{ XR_TYPE_SESSION_CREATE_INFO };
    sessionCreateInfo.next = &graphicsBinding;
    sessionCreateInfo.systemId = g_xrSystemId;

    result = xrCreateSession(g_xrInstance, &sessionCreateInfo, &g_xrSession);
    if (result != XR_SUCCESS) {
        DebugLogger::LogFormat("xrCreateSession failed: %d", (int)result);
        return;
    }

    XrReferenceSpaceCreateInfo spaceInfo{ XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
    spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    spaceInfo.poseInReferenceSpace.orientation.w = 1.0f;

    result = xrCreateReferenceSpace(g_xrSession, &spaceInfo, &g_xrPlaySpace);
    if (result != XR_SUCCESS) {
        DebugLogger::LogFormat("xrCreateReferenceSpace failed: %d", (int)result);
        if (g_xrSession != XR_NULL_HANDLE) {
            xrDestroySession(g_xrSession);
            g_xrSession = XR_NULL_HANDLE;
        }
        return;
    }

    if (!InitializeOpenXrRenderTargets()) {
        DebugLogger::Log("WARNING: Failed to initialize OpenXR projection render targets; continuing with input-only OpenXR mode");
    }

    g_xrSessionReady = true;
    DebugLogger::Log("OpenXR session and reference space created successfully");
}

static void TryInitializeOpenXRInstance() {
    AutoConfigureOpenXRRuntime();

    char effectiveRuntime[1024] = { 0 };
    GetEnvironmentVariableA("XR_RUNTIME_JSON", effectiveRuntime, sizeof(effectiveRuntime));
    if (effectiveRuntime[0] != '\0') {
        DebugLogger::LogFormat("XR_RUNTIME_JSON in process: %s", effectiveRuntime);
    }
    else {
        DebugLogger::Log("XR_RUNTIME_JSON in process: <not set>");
    }

    const char* enabledExts[] = {
        XR_KHR_D3D11_ENABLE_EXTENSION_NAME
    };

    XrInstanceCreateInfo createInfo{ XR_TYPE_INSTANCE_CREATE_INFO };
    createInfo.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
    strcpy_s(createInfo.applicationInfo.applicationName, "MGS1_VR_Mod");
    createInfo.applicationInfo.applicationVersion = 1;
    strcpy_s(createInfo.applicationInfo.engineName, "MGS1_Internal");
    createInfo.applicationInfo.engineVersion = 1;
    createInfo.enabledExtensionCount = 1;
    createInfo.enabledExtensionNames = enabledExts;

    XrResult result = xrCreateInstance(&createInfo, &g_xrInstance);
    if (result == XR_SUCCESS) {
        DebugLogger::Log("OpenXR instance created successfully");
    }
    else {
        DebugLogger::LogFormat("OpenXR instance creation failed: %d", (int)result);
    }
}

DWORD WINAPI VRMainThread(LPVOID lpReserved) {
    DebugLogger::Log("VRMainThread: Starting VR injection...");

    // Hook installation happens FIRST, before any config loading or OpenXR
    // instance/session setup -- those do real disk I/O and can take several
    // hundred ms, which is more than enough time for the game to have
    // already called DirectDrawCreate/Direct3DCreate9 and captured an
    // unhooked function pointer before we ever got a chance to patch it.
    if (MH_Initialize() != MH_OK) {
        DebugLogger::Log("ERROR: MinHook initialization failed!");
        return FALSE;
    }
    DebugLogger::Log("MinHook initialized successfully");

    // ---- DIRECTDRAW / D3D9 HOOKS FIRST (moved 2026-09-26) -----------------
    // These MUST be armed before the game calls DirectDrawCreateEx, which it
    // does ~600 ms after launch. Over the last month the camera, motion-aim,
    // render-twice and wrist-HUD installs all landed ABOVE these, and together
    // they pushed the DirectDraw hooks out to ~550-680 ms -- right on top of
    // the game's call. Losing that race is what left the game window hidden
    // and unfocused (see SeedCreateSurfaceHookFromOwnObject). They only resolve
    // two exports and MinHook them, so they now go first, a few ms in.
    bool d3d9HookInstalled = InstallD3D9CaptureHook(&VRTickFromRenderer);
    if (!d3d9HookInstalled) {
        DebugLogger::Log("WARNING: Direct3D9 capture hook install failed; frame tick and pixel capture will rely on other paths");
    }
    bool ddrawHookInstalled = InstallDdrawHooks(&VRTickFromRenderer);

    // In-frame camera rotation hook. Installed early, before the game has a
    // chance to run its camera update, and independent of MinHook (it is a
    // hand-written 5-byte jmp over an exactly-5-byte instruction -- see
    // camera_write_hook.cpp for why MinHook is the wrong tool for a
    // mid-function hook). Verifies the target opcode bytes before patching
    // and is a clean no-op if they do not match.
    if (!InstallCameraWriteHook()) {
        DebugLogger::Log("WARNING: in-frame camera hook not installed -- head rotation will not drive the camera. "
            "Check the signature-mismatch line above.");
    }

    // Motion aim + gun in hand. Independent of the camera hooks: it patches
    // the three bullet constructors' allocator calls and the actor dispatcher
    // at +A216, each signature-verified, and installs nothing on a mismatch.
    InstallMotionAimHooks();

    // Render twice: true stereo, both eyes drawn from the same game frame.
    // Hooks DG_EndFrame (+1C15) and DG_DrawChanlSystem (+1619) with MinHook,
    // signature-verified; installs nothing on a mismatch or with
    // [render_twice] enabled=0, and alternate-eye stereo carries on as before.
    InstallRenderTwiceHooks();

    // Boss / Meryl life bars -> left wrist (hooks menu_draw_bar, signature-checked).
    InstallWristBarHook();

    // Captions and codec text on the virtual screen (hooks the PC port's
    // hi-res text renderer, signature-checked).
    InstallGameTextHook();

    // Real pixel capture (Direct3DCreate9 -> CreateDevice -> Present) and the
    // DirectDraw hooks were installed at the top of this function -- see there.
    if (!ddrawHookInstalled) {
        DebugLogger::Log("WARNING: DirectDraw hook install failed; falling back to internal hook attempt");

        uintptr_t RenderDispatchAddr = 0x0045A120;
        DebugLogger::LogFormat("Attempting to hook RenderFrame at 0x%p", RenderDispatchAddr);

        MH_STATUS status = MH_CreateHook(
            (LPVOID)RenderDispatchAddr,
            &Hooked_RenderFrame,
            reinterpret_cast<LPVOID*>(&Original_RenderFrame)
        );

        if (status != MH_OK) {
            DebugLogger::LogFormat("ERROR: MH_CreateHook failed with status: %d", status);
        }
        else {
            DebugLogger::Log("MH_CreateHook: Success");
            status = MH_EnableHook((LPVOID)RenderDispatchAddr);
            if (status != MH_OK) {
                DebugLogger::LogFormat("ERROR: MH_EnableHook failed with status: %d", status);
            }
            else {
                DebugLogger::Log("MH_EnableHook: Success - Camera hook is now active!");
            }
        }
    }
    else {
        DebugLogger::Log("Using DirectDraw Flip hook as secondary frame tick source (D3D9 Present hook is primary)");
    }

    // Config/OpenXR setup happens AFTER hooks are in place.
    LoadRuntimeConfig();
    StartFallbackInputThread();
    TryInitializeOpenXRInstance();
    if (g_enableOpenXrSession) {
        TryInitializeOpenXRSession();
    }
    {
        uintptr_t base = GetMgsiModuleBase();
        int16_t px = SafeRead16(base + kCamPosXOffset);
        int16_t py = SafeRead16(base + kCamPosYOffset);
        int16_t pz = SafeRead16(base + kCamPosZOffset);
        int16_t ry = SafeRead16(base + kCamRotYOffset);
        int16_t rx = SafeRead16(base + kCamRotXOffset);
        DebugLogger::LogFormat("Confirmed real camera address check: pos=(%d,%d,%d) rot(y,x)=(%d,%d) [mgsi.exe+593FA0 family]",
            (int)px, (int)py, (int)pz, (int)ry, (int)rx);
    }

    if (!g_enableOpenXrSession) {
        DebugLogger::Log("OpenXR session init disabled by config (openxr.enable_session=0); running fallback controller mode.");
    }
    else {
        DebugLogger::Log("OpenXR session initialization enabled; if session cannot run, fallback controller mode remains active.");
    }

    StartRuntimeTickThread();

    HANDLE watchdog = CreateThread(nullptr, 0, HookWatchdogThread, nullptr, 0, nullptr);
    if (watchdog) {
        CloseHandle(watchdog);
    }

    DebugLogger::Log("VRMainThread: Injection complete. Running indefinitely.");

    return TRUE;
}

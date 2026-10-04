#include <windows.h>
#include <cmath>
#include <cstdio>   // sprintf_s, used by the keyboard-synthesis coverage log
#include <string>
#include <atomic>
#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <xinput.h>
#include <openxr/openxr.h>
#include "../include/debug_logging.h"
#include "../include/camera_write_hook.h"
#include "../include/vr_aim.h"
#include "../include/motion_aim.h"

// From dllmain.cpp: injects a key state directly into the game's own
// DirectInput keyboard GetDeviceState() read, bypassing SendInput() entirely
// (confirmed necessary -- SendInput()-based synthetic taps, in both
// scan-code and virtual-key forms, were verified reaching the correctly-
// focused game window without ever triggering FPV mode, while the same
// physical key on a real keyboard works instantly; the game reads keyboard
// state via DirectInput's buffered device polling, a path SendInput doesn't
// reach on this title).
extern void InjectKeyboardKeyState(BYTE dikCode, bool isDown);

// Converts a VK_* code to the DIK_* scan code DirectInput's keyboard buffer
// actually uses -- these are two different numbering systems, so
// g_nativeFpvToggleVk (a VK code) can't be used directly as a DIK index.
// MapVirtualKeyA(vk, MAPVK_VK_TO_VSC) returns the same scan code value
// DIK_* constants are defined as, so this conversion is exact.
static BYTE VkToDik(WORD vk) {
    return (BYTE)MapVirtualKeyA(vk, MAPVK_VK_TO_VSC);
}

#pragma comment(lib, "xinput9_1_0.lib")

extern XrInstance g_xrInstance;
extern XrSession  g_xrSession;
extern XrSpace    g_xrPlaySpace;

XrActionSet g_actionSet;
XrAction    g_poseAction;
XrAction    g_triggerAction;
XrAction    g_gripAction;
XrAction    g_thumbstickAction;
XrAction    g_thumbstickRightAction;
// Previously-dead inputs. Four physical buttons on the Touch controllers
// reached the game as nothing at all, which is why MGS1 could not be played
// without remapping first -- there simply were not enough bound inputs.
XrAction    g_leftTriggerAction;
XrAction    g_leftGripAction;
XrAction    g_xButtonAction;
XrAction    g_yButtonAction;
XrAction    g_radioAction;      // left menu button -> Select / Radio
XrAction    g_r3ClickAction;
XrAction    g_confirmAction;
XrAction    g_backAction;
// Left thumbstick click. Not bound to any game function -- free to use as a
// mod-level hotkey. Reloads [controls]/[input] from mgs1_vr_config.ini live,
// so button remaps can be tested without quitting to desktop and relaunching
// the game (LoadFallbackInputConfig() previously only ran once, at startup).
XrAction    g_l3ClickAction;
// Controller rumble (2026-09-29): tells you a punch / grab / wall press / scope registered.
XrAction    g_hapticAction = XR_NULL_HANDLE;
XrPath      g_leftHapticPath = XR_NULL_PATH, g_rightHapticPath = XR_NULL_PATH;

// The runtime's AIM pose for the right hand -- "where this controller points".
// Distinct from g_poseAction above, which is the GRIP pose (along the handle).
// Both are kept: aim drives the laser and any future engine aim write, grip is
// what a rendered weapon model will need. Grip reads tens of degrees low as a
// pointing ray, which is why a gun must not use it.
XrAction    g_aimPoseAction;
XrSpace     g_rightAimSpace;

XrSpace     g_rightHandSpace;
XrSpace     g_leftHandSpace;
XrPath      g_rightHandPath;
XrPath      g_leftHandPath;

struct PoseFilter {
    XrVector3f position = { 0, 0, 0 };
    XrQuaternionf orientation = { 0, 0, 0, 1 };
    float alpha = 0.6f;

    void Update(const XrPosef& rawPose) {
        position.x = position.x + alpha * (rawPose.position.x - position.x);
        position.y = position.y + alpha * (rawPose.position.y - position.y);
        position.z = position.z + alpha * (rawPose.position.z - position.z);
        orientation = rawPose.orientation;
    }
};

PoseFilter g_rightHandFilter;
PoseFilter g_leftHandFilter;

static float g_fallbackMouseLookSensitivity = 16.0f;
static float g_fallbackMouseDeadzone = 0.2f;
static bool g_fallbackMouseLookEnabled = false;
static bool g_fpvLookActive = false;
static WORD g_nativeFpvToggleVk = 'X'; // confirmed via direct test: this is the real in-game FPV toggle key, not V
static bool g_movementFollowsHead = true;
static bool g_invertMovementRotation = false;
static bool g_lookOnRightStick = true;
static bool g_invertLookStick = false;

// ---------------------------------------------------------------------------
// BUTTON MAPPING, [controls] in the ini -- BY ACTION NAME (2026-09-29).
//
// WHY THIS CHANGED. The old [controls] held "game button numbers" and wrote
// them straight into the DirectInput joystick state. But a joystick button
// number means nothing on its own: MGS1 decides what each joystick button does
// from ITS OWN controller settings, which it saves in mgs.cfg and keeps in
// memory as one table (read out of mgsi.exe, 2026-09-29):
//
//   0x4331D4 loads mgs.cfg: 14 bytes (keyboard), 4 dwords, then 14 dwords
//            into 0x6571F4 -- one per row of the Controller options screen:
//     [0] First person view  [1] Weapon  [2] Action  [3] Crawl
//     [4] Switch item  [5] Inventory items  [6] Switch weapon
//     [7] Inventory weapon  [8] Select / Radio  [9..12] Move L/R/U/D
//     [13] Menu / Pause
//   Each entry is an input code: 0x00-0x1F = joystick button, 0x20-0x2F =
//   axis, 0x30-0x37 = hat direction, 0xFF = unassigned (0x432C60 onwards is
//   the screen that writes them; 0x42C4D5 is a reader).
//
// On this PC mgs.cfg reads FPV=3, Weapon=2, Action=1, Crawl=0, items=6,
// weapons=7, Select=8, Pause=9 -- the classic PlayStation layout -- while the
// old ini assumed 0=Weapon, 1=FPV, 2=Crawl, 3=Action. So "Action" (3) was
// really sending First person view, the trigger (0) was sending Crawl, and so
// on. Nothing was unmapped; everything was shifted.
//
// NOW: the ini names an ACTION ("action", "crawl", ...). Every frame the mod
// looks that action up in the game's own table and presses whichever joystick
// button the game has assigned to it. Change the game's controller settings
// and the VR controls still mean what they say. An action the game has not put
// on a joystick button goes out as its keyboard key instead; Codec and Pause
// always go out as Tab / Esc, the route proven in the 2026-08-16 session.
//
// Old numeric values are still accepted and read with the meaning the old ini
// comment gave them, so an old ini keeps doing what its author intended.
// ---------------------------------------------------------------------------
constexpr int kUnbound = -1;
constexpr int kMaxGameButton = 31;

enum GameAction {
    ACT_NONE = -1,
    ACT_FPV = 0, ACT_WEAPON = 1, ACT_ACTION = 2, ACT_CRAWL = 3,
    ACT_SWITCH_ITEM = 4, ACT_ITEMS = 5, ACT_SWITCH_WEAPON = 6, ACT_WEAPONS = 7,
    ACT_CODEC = 8, ACT_PAUSE = 13,
    ACT_WALL = 20      // the mod's own: wall press (not a game button)
};

struct ActionInfo { int act; const char* key; const char* label; WORD vk; };
// Order = the order the in-headset remap panel cycles through.
static const ActionInfo kActions[] = {
    { ACT_NONE,          "none",          "NONE",          0 },
    { ACT_WEAPON,        "weapon",        "WEAPON",        VK_CONTROL },
    { ACT_ACTION,        "action",        "ACTION",        VK_SPACE },
    { ACT_CRAWL,         "crawl",         "CRAWL",         VK_SHIFT },
    { ACT_FPV,           "first_person",  "FIRST PERSON",  'X' },
    { ACT_SWITCH_ITEM,   "switch_item",   "SWITCH ITEM",   'Q' },
    { ACT_ITEMS,         "items",         "ITEMS",         'A' },
    { ACT_SWITCH_WEAPON, "switch_weapon", "SWITCH WEAPON", 'W' },
    { ACT_WEAPONS,       "weapons",       "WEAPONS",       'S' },
    { ACT_CODEC,         "codec",         "CODEC",         VK_TAB },
    { ACT_PAUSE,         "pause",         "PAUSE",         VK_ESCAPE },
    { ACT_WALL,          "wall_press",    "WALL PRESS",    0 },
};
static constexpr int kActionCount = (int)(sizeof(kActions) / sizeof(kActions[0]));

static int ActionIndex(int act) {
    for (int i = 0; i < kActionCount; ++i) if (kActions[i].act == act) return i;
    return 0;
}
static const char* ActionLabel(int act) { return kActions[ActionIndex(act)].label; }
static const char* ActionKey(int act) { return kActions[ActionIndex(act)].key; }

// The nine physical inputs that carry game actions.
enum PhysInput { IN_RTRIG, IN_RGRIP, IN_A, IN_B, IN_X, IN_Y, IN_LTRIG, IN_LGRIP, IN_LMENU, IN_COUNT };
struct PhysInfo { const char* iniKey; const char* label; int def; };
static const PhysInfo kPhys[IN_COUNT] = {
    { "right_trigger", "R TRIG", ACT_WEAPON },
    { "right_grip",    "R GRIP", ACT_WEAPONS },
    { "a_button",      "A",      ACT_ACTION },
    { "b_button",      "B",      ACT_PAUSE },
    { "x_button",      "X",      ACT_CODEC },
    { "y_button",      "Y",      ACT_PAUSE },
    { "left_trigger",  "L TRIG", ACT_CRAWL },
    { "left_grip",     "L GRIP", ACT_ITEMS },
    { "left_menu",     "MENU",   ACT_CODEC },
};
static int g_bind[IN_COUNT] = { ACT_WEAPON, ACT_WEAPONS, ACT_ACTION, ACT_PAUSE, ACT_CODEC,
                                ACT_PAUSE, ACT_CRAWL, ACT_ITEMS, ACT_CODEC };

// Menu keyboard synthesis. Enter on confirm, Escape on back. Space is
// deliberately NOT sent any more -- Space is the game's Action button, so
// synthesising it fired a second gameplay action on every confirm.
static bool g_menuKeysEnabled = true;
// 0 = never fall back to the keyboard (joystick only). Codec and Pause are
// still sent as keys: that is their only proven route.
static bool g_keyboardSynthEnabled = true;

// The game's own controller table (0x6571F4, 14 int32).
static constexpr uintptr_t kPadConfigRva = 0x2571F4;
static bool ReadPadConfig(int32_t out[14]) {
    static uintptr_t base = 0;
    if (!base) base = (uintptr_t)GetModuleHandleA(nullptr);
    __try {
        for (int i = 0; i < 14; ++i) out[i] = *reinterpret_cast<volatile int32_t*>(base + kPadConfigRva + i * 4);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static int ReadGameByteOr(uintptr_t rva, int fallback) {
    static uintptr_t base = 0;
    if (!base) base = (uintptr_t)GetModuleHandleA(nullptr);
    __try { return (int)*reinterpret_cast<volatile uint8_t*>(base + rva); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return fallback; }
}
static int ReadGameShortOr(uintptr_t rva, int fallback) {
    static uintptr_t base = 0;
    if (!base) base = (uintptr_t)GetModuleHandleA(nullptr);
    __try { return (int)*reinterpret_cast<volatile int16_t*>(base + rva); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return fallback; }
}
static int ReadGameIntOr(uintptr_t rva, int fallback) {
    static uintptr_t base = 0;
    if (!base) base = (uintptr_t)GetModuleHandleA(nullptr);
    __try { return (int)*reinterpret_cast<volatile int32_t*>(base + rva); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return fallback; }
}
// Non-zero while one of the PC port's own GDI menus (title, pause, load/save,
// options) is running. Written at +2BC16/+2C25E/+2D70D, read by the pad code.
static constexpr uintptr_t kPcMenuActiveRva = 0x31D17C;
static int CurrentWeaponId() { return ReadGameShortOr(0x38E7FC, -99); }   // GM_CurrentWeaponId, -1 = none
static int ModalState() { return ReadGameByteOr(0x391A0C, 0); }            // 0 gameplay, 1 pause, 4 inventory

static void DescribeInputCode(int32_t code, char* out, size_t cap) {
    if (code >= 0 && code < 0x20) sprintf_s(out, cap, "button %d", (int)code);
    else if (code >= 0x20 && code < 0x30) sprintf_s(out, cap, "axis 0x%02X", (unsigned)code);
    else if (code >= 0x30 && code < 0x38) sprintf_s(out, cap, "hat 0x%02X", (unsigned)code);
    else sprintf_s(out, cap, "unassigned");
}

// Logs the game's own table whenever it changes (first read included), so the
// log always says which joystick button each action is going out on.
static void LogPadConfigIfChanged(const int32_t cfg[14]) {
    static int32_t last[14];
    static bool have = false;
    if (have && std::memcmp(last, cfg, sizeof(last)) == 0) return;
    std::memcpy(last, cfg, sizeof(last));
    have = true;
    static const char* kRow[14] = { "First person", "Weapon", "Action", "Crawl", "Switch item", "Items",
        "Switch weapon", "Weapons", "Select/Radio", "Move L", "Move R", "Move U", "Move D", "Pause" };
    std::string line;
    for (int i = 0; i < 14; ++i) {
        char d[32], b[64];
        DescribeInputCode(cfg[i], d, sizeof(d));
        sprintf_s(b, "%s=%s%s", kRow[i], d, i < 13 ? ", " : "");
        line += b;
    }
    DebugLogger::LogFormat("Game controller settings (mgs.cfg, read live from mgsi.exe+2571F4): %s", line.c_str());
}

// Every VK this file may hold down on the game's behalf, EXCLUDING the four
// arrow keys (those are owned by SetOpenXrDirectionalKey and must not be
// double-driven from here).
//
// Routing all synthesis through one desired-state table is what makes this
// safe: two inputs can ask for the same key (B and Y are both Pause, and the
// B button also sends Escape as a menu key), so an edge-triggered "send on
// change" per source would release a key while another source still wants it
// held. Computing one wanted-state per key per frame and diffing that against
// what is currently held removes the whole class of problem by construction.
static const WORD kSynthTrackedVk[] = {
    VK_RETURN, VK_ESCAPE, VK_TAB, VK_CONTROL, VK_SHIFT, VK_SPACE,
    'Q', 'W', 'A', 'S', 'X'
};
static constexpr int kSynthTrackedCount =
    (int)(sizeof(kSynthTrackedVk) / sizeof(kSynthTrackedVk[0]));
static bool g_synthVkHeld[kSynthTrackedCount] = {};

// Bit N set = the game's button N is down this frame.
static std::atomic<uint32_t> g_gameButtonMask{ 0 };

static inline void SetGameButton(uint32_t& mask, int button, bool pressed) {
    if (!pressed || button < 0 || button > kMaxGameButton) return;
    mask |= (1u << button);
}

static int g_fpvDoubleTapMs = 85;

// When the USER last asked for an FPV toggle, as a GetTickCount() stamp.
//
// vr_injection.cpp's FPV auto-restore reads this. MGS1 drops you out of first
// person constantly on its own -- cutscenes, codec calls, vents, room changes
// -- and the auto-restore puts you back. But it must never fight a player who
// deliberately chose third person, and from the FPV flag alone those two cases
// are identical. This is the one extra fact that separates them: a drop
// shortly after a request is the player's doing and is left alone; a drop with
// nothing behind it is the game's and gets restored.
//
// Deliberately NOT stamped by TriggerFpvDoubleTap(), which is what the
// auto-restore itself calls -- stamping it would make every restore look like
// a user request and clear the very intent that triggered it.
static DWORD g_lastFpvToggleRequestTick = 0;

DWORD GetLastFpvToggleRequestTick() { return g_lastFpvToggleRequestTick; }

static std::atomic<bool> g_hasVirtualPadState = false;

// Read by dllmain.cpp's DirectInput translation.
bool GetInjectedGameButtons(uint32_t* outMask) {
    if (!outMask) return false;
    if (!g_hasVirtualPadState.load(std::memory_order_relaxed)) return false;
    *outMask = g_gameButtonMask.load(std::memory_order_relaxed);
    return true;
}

static XINPUT_STATE g_virtualPadState{};
static CRITICAL_SECTION g_virtualPadLock;
static std::atomic<bool> g_virtualPadLockInit = false;
static DWORD g_lastR3ToggleTick = 0;
// When R3 went down, for the hold-to-recentre gesture below. 0 = not held.
static DWORD g_r3DownTick = 0;
// A hold this long is a recentre rather than half of a double-press. Comfortably
// longer than any double-press (whose two presses must land within 450 ms of
// each other) and comfortably shorter than "did this button even register".
static constexpr DWORD kR3HoldRecenterMs = 700;
static bool g_openXrUpDown = false;
static bool g_openXrDownDown = false;
static bool g_openXrLeftDown = false;
static bool g_openXrRightDown = false;

void SendVirtualKey(WORD vKey, bool isPressed);

static const char* DescribeXrSyncResult(XrResult result) {
    switch (result) {
    case XR_SUCCESS:
        return "XR_SUCCESS";
    case XR_SESSION_NOT_FOCUSED:
        return "XR_SESSION_NOT_FOCUSED";
    default:
        return "OTHER_XR_RESULT";
    }
}

static void SetOpenXrDirectionalKey(WORD vKey, bool pressed, bool& cachedState) {
    if (pressed == cachedState) {
        return;
    }

    SendVirtualKey(vKey, pressed);
    cachedState = pressed;
}

static void ReleaseOpenXrDirectionalKeys() {
    SetOpenXrDirectionalKey(VK_UP, false, g_openXrUpDown);
    SetOpenXrDirectionalKey(VK_DOWN, false, g_openXrDownDown);
    SetOpenXrDirectionalKey(VK_LEFT, false, g_openXrLeftDown);
    SetOpenXrDirectionalKey(VK_RIGHT, false, g_openXrRightDown);
}

// The single owner of every synthesised (non-arrow) keypress.
//
// `wantVk` lists every key some input wants held this frame (0-terminated or
// n entries), on top of the menu confirm/back keys. Only the differences
// against what is currently held are sent. Called every frame, including with
// nothing wanted on session loss, so a key can never be left stuck down.
static void ApplySynthesizedKeyboard(const WORD* wantVk, int n, bool wantEnter, bool wantEscape) {
    bool want[kSynthTrackedCount] = {};

    auto Want = [&](WORD vk) {
        if (vk == 0) return;
        for (int i = 0; i < kSynthTrackedCount; ++i) {
            if (kSynthTrackedVk[i] == vk) { want[i] = true; return; }
        }
    };

    if (wantEnter)  Want(VK_RETURN);
    if (wantEscape) Want(VK_ESCAPE);
    for (int i = 0; i < n; ++i) Want(wantVk[i]);

    for (int i = 0; i < kSynthTrackedCount; ++i) {
        if (want[i] == g_synthVkHeld[i]) continue;
        SendVirtualKey(kSynthTrackedVk[i], want[i]);
        g_synthVkHeld[i] = want[i];

        // First few transitions per key are logged by name, so the log
        // answers "was Codec actually sent?" directly. Capped per key.
        static int transitionLog[kSynthTrackedCount] = {};
        if (transitionLog[i] < 4) {
            transitionLog[i]++;
            const char* which = "menu key";
            for (int a = 1; a < kActionCount; ++a) {
                if (kActions[a].vk == kSynthTrackedVk[i]) { which = kActions[a].label; break; }
            }
            DebugLogger::LogFormat("Synth key: VK=%u (%s) -> %s",
                (unsigned)kSynthTrackedVk[i], which, want[i] ? "DOWN" : "UP");
        }
    }
}

static void ReleaseSynthesizedKeyboard() {
    ApplySynthesizedKeyboard(nullptr, 0, false, false);
}

// Presses one game ACTION: the joystick button the game's own table assigns
// to it, else its keyboard key. Codec and Pause always go out as keys.
struct ActionOut { uint32_t mask = 0; WORD vk[24]; int nvk = 0; };
static void PressAction(ActionOut& o, int act, const int32_t cfg[14], bool haveCfg) {
    if (act < 0 || act == ACT_WALL) return;
    auto key = [&](WORD vk) { if (vk && o.nvk < 24) o.vk[o.nvk++] = vk; };
    const WORD vk = kActions[ActionIndex(act)].vk;
    if (act == ACT_CODEC || act == ACT_PAUSE) { key(vk); return; }
    const int32_t code = (haveCfg && act < 14) ? cfg[act] : -1;
    if (code >= 0 && code <= kMaxGameButton) { o.mask |= (1u << code); return; }
    if (g_keyboardSynthEnabled) key(vk);
}

static std::string GetGameIniPath() {
    char exePath[MAX_PATH] = { 0 };
    GetModuleFileNameA(NULL, exePath, MAX_PATH);
    std::string dir(exePath);
    auto slash = dir.find_last_of("\\/");
    if (slash != std::string::npos) {
        dir = dir.substr(0, slash);
    }
    return dir + "\\mgs1_vr_config.ini";
}

// Reported 2026-08-15: edited the ini, launched fresh, new control mapping
// still didn't take effect -- NOT a mid-session-only reload problem (that's
// what L3 fixes above), this was a cold launch. Path resolution itself is
// consistent (this function, camera_write_hook.cpp's copy, and
// vr_injection.cpp's inline version all compute the identical exe-relative
// path), so the leading suspects are: (a) Windows UAC file virtualization --
// the game installs under Program Files (x86), a protected location, and an
// old unmanifested 32-bit exe running unelevated there can have file reads
// silently redirected to a per-user shadow copy under
// %LocalAppData%\VirtualStore\... which stops tracking the real file the
// moment the two diverge (e.g. one of you -- the text editor or the game --
// is running elevated and the other isn't); or (b) a typo'd ini key name,
// which GetPrivateProfileIntA fails silently on by returning the default,
// no error. This log line answers which: compare its timestamp against when
// you actually saved your edit. Matches -> not (a). If the parsed button
// values a few lines below also don't match what you set, check for (b).
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

// ---- motion melee, wall press, Nikita (2026-09-29), [melee] / [input] / [weapons]
static bool  g_meleePunch = true;
static float g_punchSpeed = 2.0f;     // m/s along your facing
static float g_punchReach = 0.18f;    // m in front of your head
static bool  g_meleeChoke = true;
static float g_chokeHands = 0.30f;    // m apart at most
static float g_snapSpeed = 1.5f;      // m/s jerk while choking = extra press
static int   g_grabAction = 1;         // grip while that hand rests on a wall = ACTION (ladders, elevator buttons)
static int   g_grabActionMs = 120;
static int   g_wallPressMode = 3;      // 0 off, 1 hold right grip + stick, 2 automatic (push into a wall), 3 Snake's eyes (button / back into a wall)
static int   g_wallAutoBack = 1;       // mode 3: backing into a wall for wall_press_ms starts it too
static int   g_wallLeaveMs = 900;      // (unused since round 7: releasing X ends it)
static int   g_wallInFront = 0;
static bool  g_codecGesture = true;    // left grip with the left hand at your left ear = Codec
static float g_codecEarM = 0.17f;        // mode 3: 0 = the wall is behind you (back to it), 1 = in front
static float g_povTurnDegPerSec = 150.0f;  // right-stick turning inside Snake's eyes (REX top)
static bool  g_hapticsEnabled = true;
static int   g_chokeHoldMs = 350;
static int   g_wallPressMs = 450;
static int   g_wallPressUnits = 30;
static int   g_wallPressReleaseMs = 700;
static bool  g_nikitaScreen = true;

static void LoadFallbackInputConfig() {
    std::string ini = GetGameIniPath();
    LogIniFileState(ini);
    g_fallbackMouseLookSensitivity = (float)GetPrivateProfileIntA("input", "fallback_mouse_look_sensitivity", 16, ini.c_str());
    g_fallbackMouseDeadzone = (float)GetPrivateProfileIntA("input", "fallback_mouse_look_deadzone_percent", 20, ini.c_str()) / 100.0f;
    g_fallbackMouseLookEnabled = GetPrivateProfileIntA("input", "fallback_mouse_look_enabled", 0, ini.c_str()) != 0;
    g_nativeFpvToggleVk = (WORD)GetPrivateProfileIntA("input", "native_fpv_toggle_vk", 'X', ini.c_str());
    g_fpvDoubleTapMs = GetPrivateProfileIntA("input", "native_fpv_double_tap_ms", 85, ini.c_str());
    g_movementFollowsHead = GetPrivateProfileIntA("input", "movement_follows_head", 1, ini.c_str()) != 0;
    g_invertMovementRotation = GetPrivateProfileIntA("input", "invert_movement_rotation", 0, ini.c_str()) != 0;
    g_lookOnRightStick = GetPrivateProfileIntA("input", "look_on_right_stick", 1, ini.c_str()) != 0;
    g_invertLookStick = GetPrivateProfileIntA("input", "invert_look_stick", 0, ini.c_str()) != 0;
    DebugLogger::LogFormat("Look config: look_on_right_stick=%d invert_look_stick=%d",
        g_lookOnRightStick ? 1 : 0, g_invertLookStick ? 1 : 0);

    // ---- [controls]: action names ------------------------------------------
    // Legacy numbers, read with the meaning the OLD ini comment gave them
    // (0 Weapon, 1 FPV, 2 Crawl, 3 Action, 4 Switch item, 5 Switch weapon,
    // 6 Items, 7 Weapons, 10 Codec, 11 Pause).
    auto Legacy = [](int n) -> int {
        switch (n) {
        case -1: return ACT_NONE;   case 0: return ACT_WEAPON;  case 1: return ACT_FPV;
        case 2: return ACT_CRAWL;   case 3: return ACT_ACTION;  case 4: return ACT_SWITCH_ITEM;
        case 5: return ACT_SWITCH_WEAPON; case 6: return ACT_ITEMS; case 7: return ACT_WEAPONS;
        case 10: return ACT_CODEC;  case 11: return ACT_PAUSE;
        default: return -99;
        }
    };
    auto ParseAction = [&](const char* s, int def, const char* key) -> int {
        char t[40] = {};
        size_t j = 0;
        for (const char* c = s; *c && j + 1 < sizeof(t); ++c) {
            if (*c == ' ' || *c == '\t' || *c == ';') { if (j) break; else continue; }
            t[j++] = (char)((*c >= 'A' && *c <= 'Z') ? *c + 32 : (*c == '-' ? '_' : *c));
        }
        if (!t[0]) return def;
        if ((t[0] >= '0' && t[0] <= '9') || t[0] == '-') {
            const int a = Legacy(atoi(t));
            if (a == -99) {
                DebugLogger::LogFormat("[controls] %s=%s: not a known old button number -- using the default (%s)", key, t, ActionKey(def));
                return def;
            }
            DebugLogger::LogFormat("[controls] %s=%s is an OLD button number, read as \"%s\". Write the name to be explicit.", key, t, ActionKey(a));
            return a;
        }
        for (int i = 0; i < kActionCount; ++i) if (std::strcmp(t, kActions[i].key) == 0) return kActions[i].act;
        // A few friendly aliases.
        if (!std::strcmp(t, "fpv") || !std::strcmp(t, "first_person_view")) return ACT_FPV;
        if (!std::strcmp(t, "radio") || !std::strcmp(t, "select")) return ACT_CODEC;
        if (!std::strcmp(t, "menu") || !std::strcmp(t, "start")) return ACT_PAUSE;
        if (!std::strcmp(t, "fire") || !std::strcmp(t, "shoot")) return ACT_WEAPON;
        if (!std::strcmp(t, "crouch")) return ACT_CRAWL;
        if (!std::strcmp(t, "wall") || !std::strcmp(t, "shimmy") || !std::strcmp(t, "wallpress")) return ACT_WALL;
        if (!std::strcmp(t, "inventory_items") || !std::strcmp(t, "item_menu")) return ACT_ITEMS;
        if (!std::strcmp(t, "inventory_weapons") || !std::strcmp(t, "weapon_menu") || !std::strcmp(t, "inventory_weapon")) return ACT_WEAPONS;
        DebugLogger::LogFormat("[controls] %s=%s: unknown action -- using the default (%s). Known: none weapon action crawl "
            "first_person switch_item items switch_weapon weapons codec pause wall_press", key, t, ActionKey(def));
        return def;
    };
    for (int i = 0; i < IN_COUNT; ++i) {
        char v[64] = {};
        GetPrivateProfileStringA("controls", kPhys[i].iniKey, "", v, sizeof(v), ini.c_str());
        g_bind[i] = ParseAction(v, kPhys[i].def, kPhys[i].iniKey);
    }
    g_menuKeysEnabled = GetPrivateProfileIntA("controls", "menu_keys_enabled", 1, ini.c_str()) != 0;
    g_keyboardSynthEnabled = GetPrivateProfileIntA("controls", "keyboard_synth_enabled", 1, ini.c_str()) != 0;
    {
        std::string m;
        for (int i = 0; i < IN_COUNT; ++i) {
            m += kPhys[i].label; m += "="; m += ActionKey(g_bind[i]); if (i + 1 < IN_COUNT) m += " ";
        }
        DebugLogger::LogFormat("Controls (by action): %s | menu keys (A=Enter, B=Esc)=%d | keyboard fallback=%d. "
            "Each action is pressed on whichever joystick button the game's own controller settings give it "
            "(see the 'Game controller settings' line); Codec and Pause go out as Tab / Esc. Hold L3 ~1 s in the "
            "headset to remap on your left wrist.", m.c_str(), g_menuKeysEnabled ? 1 : 0, g_keyboardSynthEnabled ? 1 : 0);
    }

    // ---- [melee] -----------------------------------------------------------
    g_meleePunch = GetPrivateProfileIntA("melee", "punch", 1, ini.c_str()) != 0;
    g_punchSpeed = (float)GetPrivateProfileIntA("melee", "punch_speed_cms", 200, ini.c_str()) / 100.0f;
    g_punchReach = (float)GetPrivateProfileIntA("melee", "punch_reach_cm", 18, ini.c_str()) / 100.0f;
    g_meleeChoke = GetPrivateProfileIntA("melee", "chokehold", 1, ini.c_str()) != 0;
    g_chokeHands = (float)GetPrivateProfileIntA("melee", "choke_hands_apart_cm", 30, ini.c_str()) / 100.0f;
    g_snapSpeed = (float)GetPrivateProfileIntA("melee", "snap_speed_cms", 150, ini.c_str()) / 100.0f;
    DebugLogger::LogFormat("Melee: punch=%d (hand forward faster than %.1f m/s, %.0f cm out = Action) | chokehold=%d "
        "(unarmed, both hands together within %.0f cm in front of you = hold Weapon; a jerk faster than %.1f m/s = "
        "extra Weapon press). The buttons still work as well.",
        g_meleePunch ? 1 : 0, g_punchSpeed, g_punchReach * 100.0f, g_meleeChoke ? 1 : 0, g_chokeHands * 100.0f, g_snapSpeed);

    // ---- wall press / Nikita ------------------------------------------------
    g_wallPressMode = GetPrivateProfileIntA("input", "wall_press_mode", 3, ini.c_str());
    g_wallAutoBack = GetPrivateProfileIntA("input", "wall_press_back_into_wall", 1, ini.c_str());
    g_wallLeaveMs = GetPrivateProfileIntA("input", "wall_press_leave_ms", 900, ini.c_str());
    if (g_wallLeaveMs < 200) g_wallLeaveMs = 200;
    g_wallInFront = GetPrivateProfileIntA("input", "wall_press_wall_in_front", 0, ini.c_str());
    g_codecGesture = GetPrivateProfileIntA("input", "codec_ear_gesture", 1, ini.c_str()) != 0;
    g_codecEarM = (float)GetPrivateProfileIntA("input", "codec_ear_cm", 17, ini.c_str()) / 100.0f;
    g_povTurnDegPerSec = (float)GetPrivateProfileIntA("input", "pov_turn_deg_per_s", 150, ini.c_str());
    g_grabAction = GetPrivateProfileIntA("input", "grab_action", 1, ini.c_str());
    g_grabActionMs = GetPrivateProfileIntA("input", "grab_action_ms", 120, ini.c_str());
    if (g_grabActionMs < 40) g_grabActionMs = 40;
    DebugLogger::LogFormat("Grab: %s -- squeeze a grip while that hand rests on a wall (the hands' own collision) "
        "and the game gets ACTION for %d ms: climb a ladder, press an elevator button. A grip in open air still "
        "opens the wrist list / wall press as before.", g_grabAction ? "ON" : "off", g_grabActionMs);
    g_wallPressMs = GetPrivateProfileIntA("input", "wall_press_ms", 450, ini.c_str());
    g_wallPressUnits = GetPrivateProfileIntA("input", "wall_press_units", 30, ini.c_str());
    g_wallPressReleaseMs = GetPrivateProfileIntA("input", "wall_press_release_ms", 700, ini.c_str());
    g_nikitaScreen = GetPrivateProfileIntA("weapons", "nikita_virtual_screen", 1, ini.c_str()) != 0;
    g_hapticsEnabled = GetPrivateProfileIntA("melee", "rumble", 1, ini.c_str()) != 0;
    g_chokeHoldMs = GetPrivateProfileIntA("melee", "choke_hold_ms", 350, ini.c_str());
    DebugLogger::LogFormat("Wall press mode=%d (0 off, 1 = hold RIGHT GRIP and move the left stick, 2 = automatic when "
        "pushing into a wall %d ms / %d units, stick released %d ms, 3 = HOLD the WALL PRESS button: Snake flattens "
        "against the wall %s you, right stick left/right sidles, release to stop) | Nikita on the "
        "virtual screen=%d | rumble=%d",
        g_wallPressMode, g_wallPressMs, g_wallPressUnits, g_wallPressReleaseMs, g_wallInFront ? "in front of" : "behind",
        g_nikitaScreen ? 1 : 0, g_hapticsEnabled ? 1 : 0);
    DebugLogger::LogFormat("Movement config: follows_head=%d invert_rotation=%d",
        g_movementFollowsHead ? 1 : 0, g_invertMovementRotation ? 1 : 0);

    DebugLogger::LogFormat(
        "Fallback input config: mouse_look_enabled=%d sensitivity=%.1f deadzone=%.2f fpv_vk=%u fpv_double_tap_ms=%d",
        g_fallbackMouseLookEnabled ? 1 : 0,
        g_fallbackMouseLookSensitivity,
        g_fallbackMouseDeadzone,
        (unsigned)g_nativeFpvToggleVk,
        g_fpvDoubleTapMs);
}

static void SendVirtualMouseMove(LONG dx, LONG dy) {
    if (dx == 0 && dy == 0) {
        return;
    }

    INPUT input = { 0 };
    input.type = INPUT_MOUSE;
    input.mi.dx = dx;
    input.mi.dy = dy;
    input.mi.dwFlags = MOUSEEVENTF_MOVE;
    SendInput(1, &input, sizeof(INPUT));
}

static void SendKeyTap(WORD vKey) {
    WORD scan = (WORD)MapVirtualKeyA(vKey, MAPVK_VK_TO_VSC);
    DWORD extendedFlag = 0;

    if (vKey == VK_UP || vKey == VK_DOWN || vKey == VK_LEFT || vKey == VK_RIGHT ||
        vKey == VK_RCONTROL || vKey == VK_RMENU || vKey == VK_INSERT || vKey == VK_DELETE ||
        vKey == VK_HOME || vKey == VK_END || vKey == VK_PRIOR || vKey == VK_NEXT) {
        extendedFlag = KEYEVENTF_EXTENDEDKEY;
    }

    // Send BOTH scan-code and virtual-key forms of the same physical key.
    // A real hardware keypress produces a scan code that Windows resolves
    // to a virtual key; SendInput can emit either representation, and which
    // one a given application actually reacts to depends on how it reads
    // input (raw WM_KEYDOWN via wParam=vKey vs low-level scan-code hooks
    // vs DirectInput buffered scan-code polling). Sending both covers each
    // case without needing to know which this game uses. This does send
    // two logically-identical "key down" events in a row for one physical
    // tap, which is harmless for a simple toggle-on-press gesture like this.
    INPUT downScan = { 0 };
    downScan.type = INPUT_KEYBOARD;
    downScan.ki.wVk = 0;
    downScan.ki.wScan = scan;
    downScan.ki.dwFlags = KEYEVENTF_SCANCODE | extendedFlag;

    INPUT upScan = downScan;
    upScan.ki.dwFlags = KEYEVENTF_SCANCODE | extendedFlag | KEYEVENTF_KEYUP;

    INPUT downVk = { 0 };
    downVk.type = INPUT_KEYBOARD;
    downVk.ki.wVk = vKey;
    downVk.ki.wScan = 0;
    downVk.ki.dwFlags = 0;

    INPUT upVk = downVk;
    upVk.ki.dwFlags = KEYEVENTF_KEYUP;

    INPUT arr[4] = { downScan, downVk, upScan, upVk };
    SendInput(4, arr, sizeof(INPUT));
}

// Presses then releases the configured FPV toggle key via direct
// DirectInput keyboard buffer injection (see InjectKeyboardKeyState in
// dllmain.cpp) rather than SendInput -- SendInput was confirmed reaching
// the correctly-focused game window without ever triggering FPV, while a
// real physical keypress works instantly, indicating the game reads
// keyboard state through DirectInput's buffered polling rather than the
// message-queue/low-level-hook path SendInput primarily targets.
static void InjectFpvKeyTap() {
    BYTE dik = VkToDik(g_nativeFpvToggleVk);
    InjectKeyboardKeyState(dik, true);
    Sleep(50); // held long enough to be seen by at least one real poll
    InjectKeyboardKeyState(dik, false);
}

// SendInput()'s keyboard events are delivered to whichever window has real
// OS-level keyboard focus at that moment -- NOT necessarily the game, even
// though the call itself always "succeeds". If the game's window isn't
// actually foreground/focused (very plausible when driving everything
// through a headset via Virtual Desktop rather than looking at the desktop
// directly), a synthetic key-tap can silently go nowhere while callers have
// no way to tell. This forces real focus onto the game's own window
// immediately before sending, and logs whether that actually worked.
static bool EnsureGameWindowFocused() {
    DWORD gamePid = GetCurrentProcessId();
    HWND gameHwnd = nullptr;

    // Find a top-level window belonging to this process (the game's own
    // window, not our headless DLL).
    HWND hwnd = GetForegroundWindow();
    if (hwnd) {
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (pid == gamePid) {
            gameHwnd = hwnd; // already focused, nothing to do
        }
    }

    if (!gameHwnd) {
        // Foreground window isn't ours -- search for one that is.
        struct FindContext { DWORD pid; HWND found; };
        FindContext ctx{ gamePid, nullptr };
        EnumWindows([](HWND candidate, LPARAM lparam) -> BOOL {
            auto* ctx = reinterpret_cast<FindContext*>(lparam);
            DWORD pid = 0;
            GetWindowThreadProcessId(candidate, &pid);
            if (pid == ctx->pid && IsWindowVisible(candidate)) {
                ctx->found = candidate;
                return FALSE; // stop enumerating
            }
            return TRUE;
            }, reinterpret_cast<LPARAM>(&ctx));
        gameHwnd = ctx.found;

        if (gameHwnd) {
            SetForegroundWindow(gameHwnd);
            SetFocus(gameHwnd);
        }
    }

    if (!gameHwnd) {
        DebugLogger::Log("EnsureGameWindowFocused: could not find any visible window belonging to this process");
        return false;
    }

    HWND actualForeground = GetForegroundWindow();
    DWORD actualPid = 0;
    if (actualForeground) {
        GetWindowThreadProcessId(actualForeground, &actualPid);
    }
    bool focused = (actualPid == gamePid);
    if (!focused) {
        DebugLogger::LogFormat("EnsureGameWindowFocused: SetForegroundWindow did not take effect (foreground pid=%lu, our pid=%lu) -- "
            "Windows sometimes blocks a background process from stealing focus; synthetic key input may not reach the game",
            actualPid, gamePid);
    }
    return focused;
}

// ---------------------------------------------------------------------------
// HEAD-RELATIVE MOVEMENT
//
// MGS1 locks its movement frame to the camera as it was when you entered
// first-person view. We move the CAMERA with the head but leave Snake's body
// facing alone, so without this the stick keeps pushing him in the pre-FPV
// direction regardless of where you are looking.
//
// This rotates the stick vector by the head-yaw offset AT THE SOURCE, before
// anything downstream consumes it. That matters: an earlier attempt rotated
// the vector inside the DirectInput mapper, which had no effect, because
// actual walking is driven by the ARROW KEYS synthesised from this same stick
// a few lines below -- and those were being derived from the unrotated value.
// Rotating here covers the directional keys and the virtual pad axes both.
//
// No memory writes and no new addresses. It composes exactly, because the
// reference pose is captured at FPV entry -- the same instant the game locks
// its movement frame.
// ---------------------------------------------------------------------------
static void RotateStickByHeadYaw(float& x, float& y) {
    if (!g_movementFollowsHead) return;
    // Snake's eyes outside first person (REX top, jeep, ladder, wall press):
    // the game reads the stick relative to ITS camera, which looks at Snake
    // from somewhere else. Turn the stick from the view you see into that
    // camera's frame, so up = where you look, left/right = your left/right.
    // (2026-10-02 REX: "left/right on the right stick, up/down on the left".)
    if (IsGameplayPovActive()) {
        float view = 0.0f, game = 0.0f;
        if (x == 0.0f && y == 0.0f) return;
        if (!GetGameplayPovStickYaw(&view, &game)) return;
        float d = view - game;
        if (g_invertMovementRotation) d = -d;
        const float c = std::cos(d), s = std::sin(d);
        const float nx = x * c - y * s;
        const float ny = x * s + y * c;
        x = nx; y = ny;
        static int s_povLog = 0;
        if (s_povLog < 6) {
            s_povLog++;
            DebugLogger::LogFormat("Snake's eyes stick: view %.1f deg, game camera %.1f deg -> stick turned %.1f deg",
                view * 57.2958f, game * 57.2958f, d * 57.2958f);
        }
        return;
    }
    // Twin-stick mode owns turning. Auto-turn and manual turn both drive the
    // same channel (VK_LEFT/VK_RIGHT), so leaving both on means walking drags
    // your aim away from wherever you deliberately pointed it.
    if (g_lookOnRightStick && IsFpvActive()) return;
    // The camera hook is turning Snake's body to your head instead; rotating
    // the stick as well would steer twice.
    if (IsBodyFollowsHeadEnabled() && IsFpvActive()) return;
    if (x == 0.0f && y == 0.0f) return;

    float yawRad = 0.0f;
    if (!GetHeadYawOffsetRadians(&yawRad)) return;   // not in FPV / no reference
    if (g_invertMovementRotation) yawRad = -yawRad;

    const float c = std::cos(yawRad);
    const float s = std::sin(yawRad);
    const float nx = x * c - y * s;
    const float ny = x * s + y * c;
    x = nx;
    y = ny;

    static int logCount = 0;
    if (logCount < 20) {
        DebugLogger::LogFormat("Head-relative movement: yaw_off=%.3f rad -> stick(%.2f, %.2f)", yawRad, x, y);
        logCount++;
    }
}

void ApplyRadialDeadzone(float& x, float& y, float deadzone) {
    float mag = std::sqrt(x * x + y * y);
    if (mag < deadzone) {
        x = 0.0f;
        y = 0.0f;
    }
    else {
        float rescale = (mag - deadzone) / (1.0f - deadzone);
        x = (x / mag) * rescale;
        y = (y / mag) * rescale;
    }
}

void SendVirtualKey(WORD vKey, bool isPressed) {
    INPUT input = { 0 };
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = 0;
    input.ki.wScan = (WORD)MapVirtualKeyA(vKey, MAPVK_VK_TO_VSC);
    input.ki.dwFlags = KEYEVENTF_SCANCODE;

    if (vKey == VK_UP || vKey == VK_DOWN || vKey == VK_LEFT || vKey == VK_RIGHT ||
        vKey == VK_RCONTROL || vKey == VK_RMENU || vKey == VK_INSERT || vKey == VK_DELETE ||
        vKey == VK_HOME || vKey == VK_END || vKey == VK_PRIOR || vKey == VK_NEXT) {
        input.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    }

    if (!isPressed) {
        input.ki.dwFlags |= KEYEVENTF_KEYUP;
    }

    SendInput(1, &input, sizeof(INPUT));
}

void SendVirtualMouseClick(DWORD flags) {
    INPUT input = { 0 };
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = flags;
    SendInput(1, &input, sizeof(INPUT));
}

void InitOpenXRInput() {
    XrActionSetCreateInfo actionSetInfo{ XR_TYPE_ACTION_SET_CREATE_INFO };
    strcpy_s(actionSetInfo.actionSetName, "mgs1_vr_gameplay");
    strcpy_s(actionSetInfo.localizedActionSetName, "MGS1 VR Gameplay");
    XrResult xr = xrCreateActionSet(g_xrInstance, &actionSetInfo, &g_actionSet);
    if (xr != XR_SUCCESS) {
        DebugLogger::LogFormat("ERROR: xrCreateActionSet failed: %d", (int)xr);
        g_actionSet = XR_NULL_HANDLE;
        return;
    }

    xrStringToPath(g_xrInstance, "/user/hand/left", &g_leftHandPath);
    xrStringToPath(g_xrInstance, "/user/hand/right", &g_rightHandPath);
    XrPath handPaths[2] = { g_leftHandPath, g_rightHandPath };

    auto CreateActionChecked = [&](XrActionType type, const char* name, const char* localized, XrAction* outAction) {
        XrActionCreateInfo actionInfo{ XR_TYPE_ACTION_CREATE_INFO };
        actionInfo.actionType = type;
        actionInfo.countSubactionPaths = 2;
        actionInfo.subactionPaths = handPaths;
        strcpy_s(actionInfo.actionName, name);
        strcpy_s(actionInfo.localizedActionName, localized);
        XrResult cr = xrCreateAction(g_actionSet, &actionInfo, outAction);
        if (cr != XR_SUCCESS) {
            DebugLogger::LogFormat("ERROR: xrCreateAction(%s) failed: %d", name, (int)cr);
            *outAction = XR_NULL_HANDLE;
        }
    };

    CreateActionChecked(XR_ACTION_TYPE_POSE_INPUT, "hand_pose", "Hand Pose", &g_poseAction);
    CreateActionChecked(XR_ACTION_TYPE_POSE_INPUT, "aim_pose", "Aim Pose", &g_aimPoseAction);
    CreateActionChecked(XR_ACTION_TYPE_BOOLEAN_INPUT, "trigger_click", "Trigger Click", &g_triggerAction);
    CreateActionChecked(XR_ACTION_TYPE_BOOLEAN_INPUT, "grip_click", "Grip Click", &g_gripAction);
    CreateActionChecked(XR_ACTION_TYPE_VECTOR2F_INPUT, "thumbstick_move", "Thumbstick Move", &g_thumbstickAction);
    CreateActionChecked(XR_ACTION_TYPE_VECTOR2F_INPUT, "thumbstick_look", "Thumbstick Look", &g_thumbstickRightAction);
    CreateActionChecked(XR_ACTION_TYPE_BOOLEAN_INPUT, "left_trigger", "Left Trigger", &g_leftTriggerAction);
    CreateActionChecked(XR_ACTION_TYPE_BOOLEAN_INPUT, "left_grip", "Left Grip", &g_leftGripAction);
    CreateActionChecked(XR_ACTION_TYPE_BOOLEAN_INPUT, "x_click", "X Button", &g_xButtonAction);
    CreateActionChecked(XR_ACTION_TYPE_BOOLEAN_INPUT, "y_click", "Y Button", &g_yButtonAction);
    CreateActionChecked(XR_ACTION_TYPE_BOOLEAN_INPUT, "radio_click", "Radio", &g_radioAction);
    CreateActionChecked(XR_ACTION_TYPE_BOOLEAN_INPUT, "r3_click", "R3 Click", &g_r3ClickAction);
    CreateActionChecked(XR_ACTION_TYPE_BOOLEAN_INPUT, "confirm_click", "Confirm Click", &g_confirmAction);
    CreateActionChecked(XR_ACTION_TYPE_BOOLEAN_INPUT, "back_click", "Back Click", &g_backAction);
    CreateActionChecked(XR_ACTION_TYPE_BOOLEAN_INPUT, "l3_click", "L3 Click (reload config)", &g_l3ClickAction);
    CreateActionChecked(XR_ACTION_TYPE_VIBRATION_OUTPUT, "haptic", "Rumble", &g_hapticAction);
    xrStringToPath(g_xrInstance, "/user/hand/left/output/haptic", &g_leftHapticPath);
    xrStringToPath(g_xrInstance, "/user/hand/right/output/haptic", &g_rightHapticPath);

    XrPath oculusProfilePath, valveProfilePath, viveProfilePath, wmrProfilePath;
    XrPath triggerValuePath, triggerClickPath, squeezeClickPath, thumbstickPath, trackpadPath, posePath, leftPosePath;
    XrPath thumbstickRightPath;
    XrPath leftTriggerPath, leftSqueezePath, xClickPath, yClickPath;
    XrPath r3Path, confirmAPath, confirmTriggerPath, backBPath, backMenuPath;
    XrPath aimPosePath;
    XrPath squeezeValuePath;
    XrPath l3Path;

    xrStringToPath(g_xrInstance, "/interaction_profiles/oculus/touch_controller", &oculusProfilePath);
    xrStringToPath(g_xrInstance, "/interaction_profiles/valve/index_controller", &valveProfilePath);
    xrStringToPath(g_xrInstance, "/interaction_profiles/htc/vive_controller", &viveProfilePath);
    xrStringToPath(g_xrInstance, "/interaction_profiles/microsoft/motion_controller", &wmrProfilePath);

    xrStringToPath(g_xrInstance, "/user/hand/right/input/trigger/value", &triggerValuePath);
    xrStringToPath(g_xrInstance, "/user/hand/right/input/trigger/click", &triggerClickPath);
    xrStringToPath(g_xrInstance, "/user/hand/right/input/squeeze/click", &squeezeClickPath);
    // The Index has squeeze/value and squeeze/force but NO squeeze/click. The
    // whole valve binding set was being rejected because of it -- the first
    // headset log shows `xrSuggestInteractionProfileBindings(valve) failed: -22`
    // (XR_ERROR_PATH_UNSUPPORTED), which means an Index user was getting NO
    // bindings at all. Binding a boolean action to a float path is legal; the
    // runtime applies its own threshold, exactly as we already rely on for
    // Touch's left grip below.
    xrStringToPath(g_xrInstance, "/user/hand/right/input/squeeze/value", &squeezeValuePath);
    xrStringToPath(g_xrInstance, "/user/hand/left/input/thumbstick", &thumbstickPath);
    xrStringToPath(g_xrInstance, "/user/hand/right/input/thumbstick", &thumbstickRightPath);
    // Touch's grip is an analogue squeeze, not a click. Binding a boolean
    // action to a float path is legal -- the runtime applies its own threshold.
    // (The right grip stays on squeeze/click because that combination is
    // already confirmed working in the logs; no reason to risk it.)
    xrStringToPath(g_xrInstance, "/user/hand/left/input/trigger/value", &leftTriggerPath);
    xrStringToPath(g_xrInstance, "/user/hand/left/input/squeeze/value", &leftSqueezePath);
    xrStringToPath(g_xrInstance, "/user/hand/left/input/x/click", &xClickPath);
    xrStringToPath(g_xrInstance, "/user/hand/left/input/y/click", &yClickPath);
    xrStringToPath(g_xrInstance, "/user/hand/left/input/trackpad", &trackpadPath);
    xrStringToPath(g_xrInstance, "/user/hand/right/input/grip/pose", &posePath);
    // The LEFT grip pose was never bound, so g_leftHandSpace never located.
    // The wrist HUD (LIFE on the left wrist) needs it. Standard path on every
    // profile below.
    xrStringToPath(g_xrInstance, "/user/hand/left/input/grip/pose", &leftPosePath);
    // "Where this controller points" -- the aim pose. Defined for every
    // interaction profile below, and the correct source for a gun ray. The
    // grip pose above runs along the handle and reads tens of degrees low.
    xrStringToPath(g_xrInstance, "/user/hand/right/input/aim/pose", &aimPosePath);
    xrStringToPath(g_xrInstance, "/user/hand/right/input/thumbstick/click", &r3Path);
    xrStringToPath(g_xrInstance, "/user/hand/left/input/thumbstick/click", &l3Path);
    xrStringToPath(g_xrInstance, "/user/hand/right/input/a/click", &confirmAPath);
    xrStringToPath(g_xrInstance, "/user/hand/right/input/trigger/click", &confirmTriggerPath);
    xrStringToPath(g_xrInstance, "/user/hand/right/input/b/click", &backBPath);
    xrStringToPath(g_xrInstance, "/user/hand/left/input/menu/click", &backMenuPath);

    XrActionSuggestedBinding oculusBindings[] = {
        {g_triggerAction, triggerClickPath},
        {g_gripAction, squeezeClickPath},
        {g_thumbstickAction, thumbstickPath},
        {g_thumbstickRightAction, thumbstickRightPath},
        {g_poseAction, posePath},
        {g_poseAction, leftPosePath},
        {g_aimPoseAction, aimPosePath},
        {g_r3ClickAction, r3Path},
        // Confirm now lives on the A button, not the trigger. It used to share
        // the trigger, which is fine in a menu and wrong in gameplay: the
        // trigger is the Weapon button, and confirm also synthesises a
        // keypress, so one squeeze fired two different game actions.
        {g_confirmAction, confirmAPath},
        {g_backAction, backBPath},
        {g_leftTriggerAction, leftTriggerPath},
        {g_leftGripAction, leftSqueezePath},
        {g_xButtonAction, xClickPath},
        {g_yButtonAction, yClickPath},
        {g_radioAction, backMenuPath},
        {g_l3ClickAction, l3Path},
        {g_hapticAction, g_leftHapticPath},
        {g_hapticAction, g_rightHapticPath}
    };

    XrInteractionProfileSuggestedBinding suggested{ XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
    suggested.interactionProfile = oculusProfilePath;
    suggested.suggestedBindings = oculusBindings;
    suggested.countSuggestedBindings = (uint32_t)(sizeof(oculusBindings) / sizeof(oculusBindings[0]));
    xr = xrSuggestInteractionProfileBindings(g_xrInstance, &suggested);
    if (xr != XR_SUCCESS) {
        DebugLogger::LogFormat("WARNING: xrSuggestInteractionProfileBindings(oculus) failed: %d", (int)xr);
    }

    XrActionSuggestedBinding valveBindings[] = {
        {g_triggerAction, triggerClickPath},
        {g_gripAction, squeezeValuePath},   // Index has no squeeze/click -- see note above
        {g_thumbstickAction, thumbstickPath},
        {g_thumbstickRightAction, thumbstickRightPath},
        {g_poseAction, posePath},
        {g_poseAction, leftPosePath},
        {g_aimPoseAction, aimPosePath},
        {g_r3ClickAction, r3Path},
        {g_confirmAction, confirmTriggerPath},
        {g_backAction, backMenuPath},
        {g_l3ClickAction, l3Path},
        {g_hapticAction, g_leftHapticPath},
        {g_hapticAction, g_rightHapticPath}
    };

    suggested.interactionProfile = valveProfilePath;
    suggested.suggestedBindings = valveBindings;
    suggested.countSuggestedBindings = (uint32_t)(sizeof(valveBindings) / sizeof(valveBindings[0]));
    xr = xrSuggestInteractionProfileBindings(g_xrInstance, &suggested);
    if (xr != XR_SUCCESS) {
        DebugLogger::LogFormat("WARNING: xrSuggestInteractionProfileBindings(valve) failed: %d", (int)xr);
    }

    XrActionSuggestedBinding viveBindings[] = {
        {g_triggerAction, triggerClickPath},
        {g_gripAction, squeezeClickPath},
        {g_thumbstickAction, trackpadPath},
        {g_poseAction, posePath},
        {g_poseAction, leftPosePath},
        {g_aimPoseAction, aimPosePath},
        {g_confirmAction, triggerClickPath},
        {g_backAction, backMenuPath},
        {g_hapticAction, g_leftHapticPath},
        {g_hapticAction, g_rightHapticPath}
    };

    suggested.interactionProfile = viveProfilePath;
    suggested.suggestedBindings = viveBindings;
    suggested.countSuggestedBindings = (uint32_t)(sizeof(viveBindings) / sizeof(viveBindings[0]));
    xr = xrSuggestInteractionProfileBindings(g_xrInstance, &suggested);
    if (xr != XR_SUCCESS) {
        DebugLogger::LogFormat("WARNING: xrSuggestInteractionProfileBindings(vive) failed: %d", (int)xr);
    }

    XrActionSuggestedBinding wmrBindings[] = {
        {g_triggerAction, triggerClickPath},
        {g_gripAction, squeezeClickPath},
        {g_thumbstickAction, thumbstickPath},
        {g_thumbstickRightAction, thumbstickRightPath},
        {g_poseAction, posePath},
        {g_poseAction, leftPosePath},
        {g_aimPoseAction, aimPosePath},
        {g_confirmAction, triggerClickPath},
        {g_backAction, backMenuPath},
        {g_l3ClickAction, l3Path},
        {g_hapticAction, g_leftHapticPath},
        {g_hapticAction, g_rightHapticPath}
    };

    suggested.interactionProfile = wmrProfilePath;
    suggested.suggestedBindings = wmrBindings;
    suggested.countSuggestedBindings = (uint32_t)(sizeof(wmrBindings) / sizeof(wmrBindings[0]));
    xr = xrSuggestInteractionProfileBindings(g_xrInstance, &suggested);
    if (xr != XR_SUCCESS) {
        DebugLogger::LogFormat("WARNING: xrSuggestInteractionProfileBindings(wmr) failed: %d", (int)xr);
    }

    XrActionSpaceCreateInfo spaceInfo{ XR_TYPE_ACTION_SPACE_CREATE_INFO };
    spaceInfo.action = g_poseAction;
    spaceInfo.poseInActionSpace.orientation.w = 1.0f;

    spaceInfo.subactionPath = g_rightHandPath;
    xr = xrCreateActionSpace(g_xrSession, &spaceInfo, &g_rightHandSpace);
    if (xr != XR_SUCCESS) {
        DebugLogger::LogFormat("WARNING: xrCreateActionSpace(right) failed: %d", (int)xr);
    }

    spaceInfo.subactionPath = g_leftHandPath;
    xr = xrCreateActionSpace(g_xrSession, &spaceInfo, &g_leftHandSpace);
    if (xr != XR_SUCCESS) {
        DebugLogger::LogFormat("WARNING: xrCreateActionSpace(left) failed: %d", (int)xr);
    }

    // Right-hand AIM space. Separate action, so it needs its own space.
    XrActionSpaceCreateInfo aimSpaceInfo{ XR_TYPE_ACTION_SPACE_CREATE_INFO };
    aimSpaceInfo.action = g_aimPoseAction;
    aimSpaceInfo.poseInActionSpace.orientation.w = 1.0f;
    aimSpaceInfo.subactionPath = g_rightHandPath;
    xr = xrCreateActionSpace(g_xrSession, &aimSpaceInfo, &g_rightAimSpace);
    if (xr != XR_SUCCESS) {
        DebugLogger::LogFormat("WARNING: xrCreateActionSpace(right aim) failed: %d -- laser and motion aim disabled", (int)xr);
        g_rightAimSpace = XR_NULL_HANDLE;
    }
    else {
        DebugLogger::Log("Aim: right-hand aim action space created");
    }

    XrSessionActionSetsAttachInfo attachInfo{ XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO };
    attachInfo.countActionSets = 1;
    attachInfo.actionSets = &g_actionSet;
    xr = xrAttachSessionActionSets(g_xrSession, &attachInfo);
    if (xr != XR_SUCCESS) {
        DebugLogger::LogFormat("ERROR: xrAttachSessionActionSets failed: %d", (int)xr);
    }
}

// ===========================================================================
// CONTROLS & WEAPONS PASS (2026-09-29): in-headset remap, motion melee, wall
// press assist, Nikita on the virtual screen.
// ===========================================================================

// ---- temporary hand-back to the game's own view --------------------------------
// Two situations want the game's own camera for a moment, shown on the
// world-locked virtual screen: a Nikita missile in flight (its camera is the
// missile's nose -- driving it with your head is the wrong idea) and Snake
// flattened against a wall (MGS1 only does that in third person). Both enter
// native mode and give VR mode back by themselves. An R3 double-press cancels
// it and the player's choice wins.
enum { TEMP_NONE = 0, TEMP_NIKITA = 1, TEMP_WALL = 2 };

// Controller rumble. hand 0 = left, 1 = right, 2 = both.
static void Buzz(int hand, float amplitude, int ms) {
    if (g_hapticAction == XR_NULL_HANDLE || g_xrSession == XR_NULL_HANDLE || !g_hapticsEnabled) return;
    for (int h = 0; h < 2; ++h) {
        if (hand != 2 && hand != h) continue;
        XrHapticActionInfo info{ XR_TYPE_HAPTIC_ACTION_INFO };
        info.action = g_hapticAction;
        info.subactionPath = h ? g_rightHandPath : g_leftHandPath;
        XrHapticVibration v{ XR_TYPE_HAPTIC_VIBRATION };
        v.amplitude = amplitude;
        v.duration = (XrDuration)ms * 1000000LL;
        v.frequency = XR_FREQUENCY_UNSPECIFIED;
        xrApplyHapticFeedback(g_xrSession, &info, reinterpret_cast<const XrHapticBaseHeader*>(&v));
    }
}
static int g_tempNative = TEMP_NONE;

// ---- wall press steering (2026-10-02) ------------------------------------------
// In the wall press the game's third-person camera takes over, and its stick is
// relative to THAT camera -- which looks at Snake from somewhere else entirely,
// so "push toward the wall" was a guess (blast furnace: never flattened). The
// stick is turned back into the frame Snake was facing when you grabbed the
// wall: up = into the wall, left/right = sidle along it, whatever the camera.
static bool  g_wallSteerValid = false;
static float g_wallFacingRad = 0.0f;
static bool ReadLayer3Yaw(float* out) {
    const int fx = ReadGameShortOr(0x593F60, 0), fz = ReadGameShortOr(0x593F64, 0);
    const int tx = ReadGameShortOr(0x593F68, 0), tz = ReadGameShortOr(0x593F6C, 0);
    if (tx == fx && tz == fz) return false;
    *out = std::atan2((float)(tx - fx), (float)(tz - fz));
    return true;
}
static void CaptureWallFacing() {
    g_wallSteerValid = false;
    const int obj = ReadGameIntOr(0x334228, 0);
    if (obj < 0x00400000 || obj >= 0x7FFF0000) return;
    const int rot = ReadGameShortOr((uintptr_t)obj + 0x2A - (uintptr_t)GetModuleHandleA(nullptr), -1);
    if (rot < -32768) return;
    g_wallFacingRad = (float)(rot & 0xFFF) * (6.28318530718f / 4096.0f);
    g_wallSteerValid = true;
    DebugLogger::LogFormat("Wall press: stick steered from Snake's facing %.1f deg (up = into the wall)",
        g_wallFacingRad * 57.2958f);
}
static void SteerStickForWallPress(float& x, float& y) {
    if (!g_wallSteerValid) return;
    float cam = 0.0f;
    if (!ReadLayer3Yaw(&cam)) return;
    // World direction in Snake's (entry) frame, re-expressed in the camera's.
    const float d = g_wallFacingRad - cam;
    const float c = std::cos(d), s = std::sin(d);
    const float nx = x * c - y * s;
    const float ny = x * s + y * c;
    x = nx; y = ny;
}

static void EnterTempNative(int reason, bool leaveGameFpv) {
    if (g_tempNative != TEMP_NONE || !IsVrViewModeActive()) return;
    if (reason == TEMP_WALL) CaptureWallFacing();
    g_tempNative = reason;
    SetVrViewMode(false);
    if (leaveGameFpv && IsFpvActive()) {
        SendKeyTap(g_nativeFpvToggleVk);
        Sleep(g_fpvDoubleTapMs);
        SendKeyTap(g_nativeFpvToggleVk);
    }
}
static void LeaveTempNative() {
    if (g_tempNative == TEMP_NONE) return;
    g_tempNative = TEMP_NONE;
    if (!IsVrViewModeActive()) SetVrViewMode(true);   // also re-arms first-person maintenance
}

// ---- Nikita: missile in flight -> virtual screen ------------------------------
static void NikitaUpdate(DWORD now) {
    static DWORD lastPoll = 0, lastAlive = 0;
    if (now - lastPoll < 100) return;
    lastPoll = now;
    const bool alive = g_nikitaScreen && CurrentWeaponId() == 3 /* NIKITA */ && IsGameTaskAlive("rmissile", -1);
    if (alive) {
        lastAlive = now;
        if (g_tempNative == TEMP_NONE && IsVrViewModeActive()) {
            DebugLogger::Log("Nikita: missile launched -- showing the missile's camera on the virtual screen "
                "(steer with the stick as in the original). VR comes back when it is gone.");
            EnterTempNative(TEMP_NIKITA, false);
        }
    }
    else if (g_tempNative == TEMP_NIKITA && now - lastAlive > 500) {
        DebugLogger::Log("Nikita: missile gone -- back to VR.");
        LeaveTempNative();
    }
}

// ---- wall press assist -------------------------------------------------------
// MGS1 flattens Snake against a wall only in its own third-person view. In
// first person, pushing into a wall just stops him -- which is why the Blast
// Furnace's wall-hugging section could not be done. When the stick pushes
// forward and Snake has not moved for wall_press_ms, hand the view to the game
// (virtual screen) and take first person back once the stick is let go.
// ---- wall press, GRIP mode (default, 2026-09-29 second pass) ------------------
// The auto-detect below fired once and then fought you. Asked for instead:
// HOLD THE RIGHT GRIP and move the left stick -> the game's own view on the
// virtual screen, where MGS1 does its real wall press and lets you sidle along
// the wall with the stick. Let go of the grip -> first person again.
// Returns true while it owns the right grip (so the weapon list and the
// grip's own action stay out of it).
static bool WallGripUpdate(bool rightGrip, float stickMag, DWORD now, bool handOnWall) {
    static DWORD releaseStart = 0;
    if (g_tempNative == TEMP_WALL) {
        if (!rightGrip) {
            if (!releaseStart) releaseStart = now ? now : 1;
            else if (now - releaseStart >= 120) {
                releaseStart = 0;
                DebugLogger::Log("Wall press: right grip released -- back to first person.");
                Buzz(1, 0.3f, 40);
                LeaveTempNative();
                return false;
            }
        }
        else releaseStart = 0;
        return true;
    }
    releaseStart = 0;
    if (g_wallPressMode != 1 || !rightGrip || stickMag < 0.5f || g_tempNative != TEMP_NONE ||
        !IsVrViewModeActive() || !IsFpvActive() || ModalState() != 0) return false;
    // Only when Snake is actually up against a wall (2026-10-02: it fired in
    // open rooms while walking with the weapon list held). 0x734210 is the
    // game's own wall-contact state, written by Snake's collision at +E3C65..
    // +E3E07 (0 = none, 1/2/4 = touching, by side) and read by his idle state
    // at +DE156 to start the wall press itself.
    // The right hand gripping the wall is evidence enough on its own.
    if (!handOnWall && ReadGameIntOr(0x334210, 0) == 0) return false;
    WristSelectCancel(1);
    DebugLogger::Log("Wall press: right grip + stick -- the game's own view on the virtual screen. Push toward the wall "
        "to flatten against it and sidle along it; release the grip to go back to first person.");
    Buzz(1, 0.5f, 60);
    EnterTempNative(TEMP_WALL, true);
    return true;
}

static void WallPressUpdate(float stickX, float stickY, bool blocked, DWORD now) {
    static DWORD pushStart = 0, releaseStart = 0;
    static int16_t anchor[3] = {};
    const float mag = std::sqrt(stickX * stickX + stickY * stickY);

    if (g_tempNative == TEMP_WALL && g_wallPressMode == 2) {
        if (mag < 0.3f) {
            if (!releaseStart) releaseStart = now;
            else if (now - releaseStart >= (DWORD)g_wallPressReleaseMs) {
                DebugLogger::Log("Wall press: stick released -- back to first person.");
                releaseStart = 0;
                LeaveTempNative();
            }
        }
        else releaseStart = 0;
        return;
    }
    releaseStart = 0;
    if (g_wallPressMode != 2 || blocked || g_tempNative != TEMP_NONE || !IsVrViewModeActive() || !IsFpvActive() ||
        ModalState() != 0 || stickY < 0.7f) {
        pushStart = 0;
        return;
    }
    int16_t p[3];
    if (!GetPlayerActorPosition(p)) { pushStart = 0; return; }
    if (!pushStart) { pushStart = now; anchor[0] = p[0]; anchor[1] = p[1]; anchor[2] = p[2]; return; }
    const float dx = (float)(p[0] - anchor[0]), dz = (float)(p[2] - anchor[2]);
    if (std::sqrt(dx * dx + dz * dz) > (float)g_wallPressUnits) {
        pushStart = now; anchor[0] = p[0]; anchor[1] = p[1]; anchor[2] = p[2];
        return;
    }
    if (now - pushStart >= (DWORD)g_wallPressMs) {
        DebugLogger::LogFormat("Wall press: pushing into something for %lu ms without moving (Snake at %d,%d,%d) -- "
            "handing the view to the game so he flattens against it. Keep pushing toward the wall on the screen; "
            "let go of the stick to return to first person.", (unsigned long)(now - pushStart), p[0], p[1], p[2]);
        pushStart = 0;
        EnterTempNative(TEMP_WALL, true);
    }
}

// ---- wall press, SNAKE'S EYES mode (3, default from 2026-10-02 round 6) -------
// Asked for: "turn around, put my back against a wall in game and shimmy across
// it left or right ... perhaps a different button to activate it?"
// MGS1 only flattens Snake against a wall outside first person. So: take the
// game out of first person (the maintenance loop is told to wait), and keep
// you behind Snake's eyes while the game does its real wall press -- the view
// stays where you were looking, the stick is turned into your view (down =
// back into the wall behind you, left/right = sidle). Starts on the WALL PRESS
// button (X by default), or by backing into a wall for wall_press_ms. Ends on
// the button again, or once Snake has been off the wall for wall_press_leave_ms.
static bool g_wallHold = false;
static void WallHoldExit(const char* why) {
    if (!g_wallHold) return;
    g_wallHold = false;
    SetFirstPersonHold(false, false, 0.0f);
    Buzz(2, 0.3f, 40);
    DebugLogger::LogFormat("Wall press: %s -- back to first person.", why);
}
static void WallHoldUpdate(bool btnDown, bool btnEdge, DWORD now) {
    static DWORD s_since = 0;
    const int contact = ReadGameIntOr(0x334210, 0);
    {
        static int s_lastContact = -1, s_contactLogs = 0;
        if (contact != s_lastContact && s_contactLogs < 40) {
            s_contactLogs++;
            DebugLogger::LogFormat("Wall contact (game 0x734210): %d -> %d (fpv=%d, Snake's eyes=%d, wall press=%d)",
                s_lastContact, contact, IsFpvActive() ? 1 : 0, IsGameplayPovActive() ? 1 : 0, g_wallHold ? 1 : 0);
        }
        s_lastContact = contact;
    }
    if (g_wallHold) {
        if (!IsVrViewModeActive()) { WallHoldExit("native mode chosen"); return; }
        if (!btnDown) { WallHoldExit("X released"); return; }
        // Round 9: once (22:17:06) the double-tap did not take the game out of
        // first person at all and the stick turned/walked him instead. Ask
        // again every 500 ms, up to three more times.
        {
            static DWORD s_lastAsk = 0;
            static int s_asks = 0;
            if (now - s_since < 200) { s_lastAsk = now; s_asks = 0; }
            else if (IsFpvActive() && s_asks < 3 && now - s_lastAsk >= 500) {
                s_lastAsk = now; s_asks++;
                DebugLogger::LogFormat("Wall press: game still in first person -- asking again (%d)", s_asks);
                SendKeyTap(g_nativeFpvToggleVk);
                Sleep(g_fpvDoubleTapMs);
                SendKeyTap(g_nativeFpvToggleVk);
            }
        }
        if (now - s_since > 2500 && !IsGameplayPovActive() && !IsFpvActive()) {
            // Snake's eyes is not drawing (gameplay_pov=0, or a scene took
            // over): do not leave you on the game's camera.
            WallHoldExit("Snake's eyes is not drawing (gameplay_pov off?)");
        }
        return;
    }
    if (g_wallPressMode != 3 || !btnEdge) return;
    const bool eligible = IsVrViewModeActive() && IsFpvActive() && !IsGameplayPovActive() && ModalState() == 0 &&
                          !IsCutsceneVrActive() && g_tempNative == TEMP_NONE && !IsScopeViewActive();
    if (!eligible) {
        DebugLogger::LogFormat("Wall press: X ignored -- only from normal first person (vr=%d fpv=%d snake's eyes=%d "
            "modal=%d cutscene=%d scope=%d)", IsVrViewModeActive() ? 1 : 0, IsFpvActive() ? 1 : 0,
            IsGameplayPovActive() ? 1 : 0, ModalState(), IsCutsceneVrActive() ? 1 : 0, IsScopeViewActive() ? 1 : 0);
        return;
    }
    float yaw = 0.0f;
    const bool haveYaw = ReadLayer3Yaw(&yaw);
    g_wallHold = true;
    s_since = now;
    SetFirstPersonHold(true, haveYaw, yaw);
    WristSelectCancel(0);
    WristSelectCancel(1);
    Buzz(2, 0.5f, 60);
    DebugLogger::LogFormat("Wall press: X held (wall contact %d) -- the game takes first person off so it can flatten "
        "Snake against the wall %s you; you stay behind his eyes. RIGHT STICK left/right = sidle. Let go of X to "
        "return.", contact, g_wallInFront ? "in front of" : "behind");
    SendKeyTap(g_nativeFpvToggleVk);
    Sleep(g_fpvDoubleTapMs);
    SendKeyTap(g_nativeFpvToggleVk);
}

// While X is held: the left stick is the wall's. Nothing pushed on the right
// stick = keep pushing into the wall (that is what makes MGS1 flatten him and
// keep him there); right stick left/right = sidle that way. The left stick
// and right-stick turning do nothing meanwhile.
// 2026-10-02 round 9 -- rebuilt from the decompilation (FoxdieTeam
// mgs_reversing, chara/snake/sna_init.c), which describes exactly this:
//   * Every frame Snake's collision gives the direction INTO the wall he is
//     touching, dword_800ABBD0 (here 0x734204, -1 = no wall), and compares the
//     pad direction with it (diff = dir - wall, 4096 = 360 deg):
//        |diff| <= 256 (22.5 deg)         -> state 1  "pushing into it"
//        256 < |diff| <= 896 (79 deg)     -> state 4 (diff > 0) / 2 (diff < 0)
//        anything else                    -> 0
//     That state is dword_800ABBC4 = 0x734210 (the "wall contact" we logged).
//   * Standing or walking with state 1 (3 frames when walking) -> he flattens
//     against the wall (sna_anim_wall_idle), facing wall - 180 deg.
//   * Flattened: state 2 or 4 -> sidle (sna_anim_wall_move) that way; state 1
//     -> stand still on the wall. A pad direction more than 79 deg off the
//     wall (e.g. straight ALONG it) is state 0, which is why pushing along the
//     wall made him step off and run.
//   * The pad direction is the d-pad table + the pad origin 0x6C03A4, so a
//     d-pad / arrow-key push can only point in 45-degree steps from it.
// So: no sidle input -> the 45-degree step closest to straight into the wall;
// right stick -> the step closest to 50 deg off the wall toward your side
// (always inside 22.5..79). Until the game is out of first person nothing
// is sent (in first person the same stick turns him).
static int WrapUnits4096(int a) { a %= 4096; if (a < 0) a += 4096; return a; }
static int DiffUnits(int a, int b) { int d = WrapUnits4096(a - b); return d > 2048 ? d - 4096 : d; }
static bool WallHoldStick(float& lx, float& ly, float rx) {
    if (!g_wallHold) return false;
    lx = 0.0f; ly = 0.0f;
    if (IsFpvActive()) return true;                      // still leaving first person: hold still
    const int origin = ReadGameIntOr(0x2C03A4, 0);
    const int pad = WrapUnits4096(origin + 2048);         // where stick UP goes
    float anchorRad = 0.0f;
    const bool haveAnchor = GetFirstPersonHoldAnchor(&anchorRad);
    const int anchor = WrapUnits4096((int)std::lround(anchorRad * 4096.0f / 6.28318531f));
    const int wall = ReadGameIntOr(0x334204, -1);
    const int state = ReadGameIntOr(0x334210, 0);
    // Into the wall: the game's own wall direction when he is touching one,
    // else straight behind (or ahead of) the way you faced when X went down.
    const int into = (wall >= 0 && wall < 4096) ? wall
                   : WrapUnits4096(anchor + (g_wallInFront ? 0 : 2048));
    int want = into;
    const char* what = "into the wall";
    if (std::fabs(rx) >= 0.3f) {
        // Your right is anchor - 1024; take the side of the wall closer to it.
        const int yourRight = WrapUnits4096(anchor + (rx > 0 ? -1024 : 1024));
        const int sideA = WrapUnits4096(into + 1024), sideB = WrapUnits4096(into - 1024);
        const int sign = std::abs(DiffUnits(sideA, yourRight)) <= std::abs(DiffUnits(sideB, yourRight)) ? 1 : -1;
        want = WrapUnits4096(into + sign * 576);           // ~50 deg off the wall: state 4 / 2
        what = rx > 0 ? "sidle right" : "sidle left";
    }
    (void)haveAnchor;
    // Nearest of the eight 45-degree steps the pad can make.
    const int k = WrapUnits4096((int)std::lround((double)DiffUnits(pad, want) / 512.0) * 512);
    // k = clockwise stick angle (units) from UP; the game moves along pad - k.
    const float th = (float)k * (6.28318531f / 4096.0f);
    lx = std::sin(th); ly = std::cos(th);
    if (std::fabs(lx) < 0.01f) lx = 0.0f;
    if (std::fabs(ly) < 0.01f) ly = 0.0f;
    // Diagonals must still trip both arrow keys (> 0.5 each).
    if (lx != 0.0f && ly != 0.0f) { lx = lx > 0 ? 0.85f : -0.85f; ly = ly > 0 ? 0.85f : -0.85f; }
    static const char* s_lastWhat = nullptr;
    static int s_lastWall = -2, s_logs = 0;
    if ((what != s_lastWhat || (wall >= 0) != (s_lastWall >= 0)) && s_logs < 40) {
        s_logs++;
        DebugLogger::LogFormat("Wall press stick: %s | wall dir %d (%s), wanted %d, pad up %d, sent %d -> diff %d, "
            "state now %d | stick (%.2f, %.2f) | game pad dir %d",
            what, wall, wall >= 0 ? "touching" : "no wall yet, using your facing", want, pad,
            WrapUnits4096(pad - k), DiffUnits(WrapUnits4096(pad - k), into), state, lx, ly, ReadGameIntOr(0x33422C, -1));
    }
    s_lastWhat = what; s_lastWall = wall;
    return true;
}

// ---- motion melee ------------------------------------------------------------
// PUNCH: a hand thrown forward (along your facing) faster than punch_speed and
// at least punch_reach out presses Action once -- the game's own punch; keep
// punching and its own punch-punch-kick combo follows. CHOKEHOLD (unarmed):
// both hands brought together in front of you holds Weapon, which is the
// game's grab; a sharp jerk while holding presses it again, the game's own
// "squeeze" presses. Both are pose-only, so nothing is taken off the buttons.
struct HandTrk { bool have = false; float p[3] = {}; float v[3] = {}; LONGLONG q = 0; bool armed = true; };
static HandTrk g_hand[2];

static void QRotV(const XrQuaternionf& q, const float v[3], float o[3]) {
    const float x = q.x, y = q.y, z = q.z, w = q.w;
    const float tx = 2.0f * (y * v[2] - z * v[1]), ty = 2.0f * (z * v[0] - x * v[2]), tz = 2.0f * (x * v[1] - y * v[0]);
    o[0] = v[0] + w * tx + (y * tz - z * ty);
    o[1] = v[1] + w * ty + (z * tx - x * tz);
    o[2] = v[2] + w * tz + (x * ty - y * tx);
}

static void TrackHand(int i, const XrPosef& pose, bool valid) {
    HandTrk& h = g_hand[i];
    if (!valid) { h.have = false; return; }
    LARGE_INTEGER qpc, f; QueryPerformanceCounter(&qpc); QueryPerformanceFrequency(&f);
    const float p[3] = { pose.position.x, pose.position.y, pose.position.z };
    if (h.have) {
        const float dt = (float)((double)(qpc.QuadPart - h.q) / (double)f.QuadPart);
        if (dt > 0.002f && dt < 0.1f) {
            const float a = dt / (0.025f + dt);
            for (int k = 0; k < 3; ++k) h.v[k] += ((p[k] - h.p[k]) / dt - h.v[k]) * a;
        }
    }
    else { h.v[0] = h.v[1] = h.v[2] = 0.0f; }
    for (int k = 0; k < 3; ++k) h.p[k] = p[k];
    h.q = qpc.QuadPart;
    h.have = true;
}

static void MeleeUpdate(bool gate, bool lTrig, bool rTrig, DWORD now, ActionOut& out,
                        const int32_t cfg[14], bool haveCfg) {
    static DWORD punchUntil = 0, lastPunch = 0;
    static DWORD chokeSince = 0, chokeLost = 0, gapUntil = 0, lastSnap = 0;
    static bool choking = false;

    XrPosef head;
    if (!gate || !GetLastHeadPose(&head)) {
        choking = false; chokeSince = 0;
        if (now < punchUntil) PressAction(out, ACT_ACTION, cfg, haveCfg);
        return;
    }
    const float z[3] = { 0.0f, 0.0f, -1.0f };
    float fwd[3];
    QRotV(head.orientation, z, fwd);
    fwd[1] = 0.0f;
    const float fl = std::sqrt(fwd[0] * fwd[0] + fwd[2] * fwd[2]);
    if (fl < 1e-3f) return;
    fwd[0] /= fl; fwd[2] /= fl;
    const float hp[3] = { head.position.x, head.position.y, head.position.z };

    // ---- punch ------------------------------------------------------------
    if (g_meleePunch) {
        const bool trig[2] = { lTrig, rTrig };
        for (int i = 0; i < 2; ++i) {
            HandTrk& h = g_hand[i];
            if (!h.have) continue;
            const float speed = std::sqrt(h.v[0] * h.v[0] + h.v[1] * h.v[1] + h.v[2] * h.v[2]);
            const float vf = h.v[0] * fwd[0] + h.v[2] * fwd[2];
            const float reach = (h.p[0] - hp[0]) * fwd[0] + (h.p[2] - hp[2]) * fwd[2];
            if (vf < 0.6f) h.armed = true;
            if (h.armed && !choking && !trig[i] && vf > g_punchSpeed && vf > 0.7f * speed && reach > g_punchReach &&
                now - lastPunch > 150) {
                h.armed = false;
                lastPunch = now;
                punchUntil = now + 70;
                Buzz(i, 0.7f, 45);   // you felt it register
                static int logged = 0;
                if (logged++ < 8) DebugLogger::LogFormat("Melee: %s punch (%.1f m/s, %.0f cm out) -> Action",
                    i ? "right" : "left", vf, reach * 100.0f);
            }
        }
    }
    if (now < punchUntil) PressAction(out, ACT_ACTION, cfg, haveCfg);

    // ---- chokehold (unarmed only) -----------------------------------------------
    bool pose = false;
    if (g_meleeChoke && CurrentWeaponId() == -1 && g_hand[0].have && g_hand[1].have) {
        const HandTrk& L = g_hand[0];
        const HandTrk& R = g_hand[1];
        const float d[3] = { L.p[0] - R.p[0], L.p[1] - R.p[1], L.p[2] - R.p[2] };
        const float apart = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        auto front = [&](const HandTrk& h) {
            const float reach = (h.p[0] - hp[0]) * fwd[0] + (h.p[2] - hp[2]) * fwd[2];
            const float below = hp[1] - h.p[1];
            return reach > 0.12f && reach < 0.50f && below > 0.10f && below < 0.70f;
        };
        auto still = [](const HandTrk& h) {
            return std::sqrt(h.v[0] * h.v[0] + h.v[1] * h.v[1] + h.v[2] * h.v[2]) < 0.8f;
        };
        // 2026-09-29 (second pass): boxing held both fists together in front of
        // you, so the grab kept engaging mid-combo and holding Weapon through
        // the punches. Now the hands must be close to your chest, near each
        // other and STILL, for choke_hold_ms -- and a punch cancels it.
        pose = apart < g_chokeHands && front(L) && front(R) && !lTrig && !rTrig &&
               (choking || (still(L) && still(R))) && now - lastPunch > 600;
    }
    if (pose) {
        chokeLost = 0;
        if (!chokeSince) chokeSince = now;
        if (!choking && now - chokeSince >= (DWORD)g_chokeHoldMs) {
            choking = true;
            DebugLogger::Log("Melee: chokehold pose -- holding Weapon (the game's grab)");
            Buzz(2, 0.4f, 120);
        }
    }
    else if (choking) {
        if (!chokeLost) chokeLost = now;
        if (now - chokeLost > 150) { choking = false; chokeSince = 0; DebugLogger::Log("Melee: chokehold released"); }
    }
    else chokeSince = 0;

    if (choking) {
        float jerk = 0.0f;
        for (int i = 0; i < 2; ++i) {
            const HandTrk& h = g_hand[i];
            jerk = (std::max)(jerk, std::sqrt(h.v[0] * h.v[0] + h.v[1] * h.v[1] + h.v[2] * h.v[2]));
        }
        if (jerk > g_snapSpeed && now - lastSnap > 250 && now - chokeSince > (DWORD)g_chokeHoldMs + 300) {
            lastSnap = now;
            Buzz(2, 0.8f, 60);
            gapUntil = now + 60;   // let go for a moment, then press again: one more press
            static int logged = 0;
            if (logged++ < 8) DebugLogger::LogFormat("Melee: jerk while choking (%.1f m/s) -> Weapon pressed again", jerk);
        }
        if (now >= gapUntil) PressAction(out, ACT_WEAPON, cfg, haveCfg);
    }
}

// ---- in-headset remap --------------------------------------------------------
// Hold L3 about a second: a panel on your left wrist lists the nine buttons and
// what each does. Press a button (or stick up/down) to pick it, stick
// left/right to change it, click L3 to save to mgs1_vr_config.ini and close.
// While it is open nothing reaches the game.
static bool g_remapOpen = false;
static int  g_remapSel = 0;
static int  g_remapBind[IN_COUNT];
static bool g_remapChanged = false;

static void PublishRemapPanel() {
    RemapPanelView v;
    v.open = g_remapOpen;
    v.count = IN_COUNT;
    v.sel = g_remapSel;
    v.changed = g_remapChanged;
    for (int i = 0; i < IN_COUNT; ++i) {
        strcpy_s(v.input[i], kPhys[i].label);
        strcpy_s(v.action[i], ActionLabel(g_remapBind[i]));
    }
    SetRemapPanel(v);
}

static void RemapOpen() {
    for (int i = 0; i < IN_COUNT; ++i) g_remapBind[i] = g_bind[i];
    g_remapSel = 0;
    g_remapChanged = false;
    g_remapOpen = true;
    DebugLogger::Log("Remap: panel OPEN on the left wrist (press a button or stick up/down to pick, "
        "stick left/right to change, L3 to save and close)");
    PublishRemapPanel();
}

static void LoadFallbackInputConfig();
static void RemapSaveAndClose() {
    g_remapOpen = false;
    if (g_remapChanged) {
        const std::string ini = GetGameIniPath();
        int failed = 0;
        for (int i = 0; i < IN_COUNT; ++i) {
            if (!WritePrivateProfileStringA("controls", kPhys[i].iniKey, ActionKey(g_remapBind[i]), ini.c_str())) ++failed;
        }
        WritePrivateProfileStringA(nullptr, nullptr, nullptr, ini.c_str());   // flush the ini cache
        if (failed) {
            DebugLogger::LogFormat("Remap: could NOT write %d key(s) to %s (error %lu) -- using the new mapping for "
                "this session only", failed, ini.c_str(), GetLastError());
            for (int i = 0; i < IN_COUNT; ++i) g_bind[i] = g_remapBind[i];
        }
        else {
            DebugLogger::LogFormat("Remap: saved to %s", ini.c_str());
            LoadFallbackInputConfig();
        }
    }
    else DebugLogger::Log("Remap: closed, nothing changed");
    PublishRemapPanel();
}

static void RemapUpdate(const bool down[IN_COUNT], float sx, float sy, DWORD now) {
    static bool prev[IN_COUNT] = {};
    static DWORD lastV = 0, lastH = 0;
    static int vdir = 0, hdir = 0;
    for (int i = 0; i < IN_COUNT; ++i) {
        if (down[i] && !prev[i]) g_remapSel = i;   // "press the button you want to change"
        prev[i] = down[i];
    }
    const int nv = sy > 0.6f ? -1 : (sy < -0.6f ? 1 : 0);
    const int nh = (std::fabs(sx) > std::fabs(sy)) ? (sx > 0.6f ? 1 : (sx < -0.6f ? -1 : 0)) : 0;
    if (nv != 0 && (nv != vdir || now - lastV >= 250)) {
        g_remapSel = (g_remapSel + nv + IN_COUNT) % IN_COUNT;
        lastV = now;
    }
    vdir = nv;
    if (nh != 0 && (nh != hdir || now - lastH >= 300)) {
        const int k = (ActionIndex(g_remapBind[g_remapSel]) + nh + kActionCount) % kActionCount;
        g_remapBind[g_remapSel] = kActions[k].act;
        g_remapChanged = true;
        lastH = now;
    }
    hdir = nh;
    PublishRemapPanel();
}

void UpdateOpenXRInput(XrTime predictedTime) {
    if (g_xrSession == XR_NULL_HANDLE || g_actionSet == XR_NULL_HANDLE) {
        g_hasVirtualPadState = false;
        ReleaseOpenXrDirectionalKeys();
        ReleaseSynthesizedKeyboard();
        return;
    }

    XrActiveActionSet activeSet{};
    activeSet.actionSet = g_actionSet;
    activeSet.subactionPath = XR_NULL_PATH;

    XrActionsSyncInfo syncInfo{ XR_TYPE_ACTIONS_SYNC_INFO };
    syncInfo.countActiveActionSets = 1;
    syncInfo.activeActionSets = &activeSet;
    XrResult syncResult = xrSyncActions(g_xrSession, &syncInfo);
    static bool wasNotFocused = false;
    if (syncResult != XR_SUCCESS) {
        static bool loggedSyncFail = false;

        if (syncResult == XR_SESSION_NOT_FOCUSED) {
            if (!wasNotFocused) {
                DebugLogger::LogFormat("OpenXR input paused: xrSyncActions=%d (%s). Reason: runtime session is not focused (dashboard/desktop focus).", (int)syncResult, DescribeXrSyncResult(syncResult));
                wasNotFocused = true;
            }
        }
        else if (!loggedSyncFail) {
            DebugLogger::LogFormat("ERROR: xrSyncActions failed: %d (%s). Input injection disabled until sync recovers.", (int)syncResult, DescribeXrSyncResult(syncResult));
            loggedSyncFail = true;
        }

        g_hasVirtualPadState = false;
        ReleaseOpenXrDirectionalKeys();
        ReleaseSynthesizedKeyboard();
        // The controller is not reporting while unfocused, so the aim ray must
        // be marked invalid rather than left frozen at its last value.
        {
            XrPosef dummy{};
            dummy.orientation.w = 1.0f;
            PublishControllerAim(dummy, false);
        }
        return;
    }

    if (wasNotFocused) {
        DebugLogger::Log("OpenXR input resumed: xrSyncActions recovered after focus returned");
        wasNotFocused = false;
    }

    const DWORD nowTick = GetTickCount();

    // ---- read every input up front ------------------------------------------
    // (2026-09-29: all reads moved here so the remap panel and the gestures see
    // one consistent snapshot. Each query sets its OWN subactionPath -- the
    // 2026-08-15 A/B bug was one query inheriting the previous one's hand.)
    XrActionStateGetInfo getInfo{ XR_TYPE_ACTION_STATE_GET_INFO };
    auto GetBool = [&](XrAction a, XrPath hand, XrActionStateBoolean& st) -> XrResult {
        getInfo.action = a; getInfo.subactionPath = hand;
        return xrGetActionStateBoolean(g_xrSession, &getInfo, &st);
    };

    XrActionStateVector2f stickState{ XR_TYPE_ACTION_STATE_VECTOR2F };
    getInfo.action = g_thumbstickAction; getInfo.subactionPath = g_leftHandPath;
    XrResult stickResult = xrGetActionStateVector2f(g_xrSession, &getInfo, &stickState);

    XrActionStateVector2f lookState{ XR_TYPE_ACTION_STATE_VECTOR2F };
    getInfo.action = g_thumbstickRightAction; getInfo.subactionPath = g_rightHandPath;
    xrGetActionStateVector2f(g_xrSession, &getInfo, &lookState);

    XrActionStateBoolean triggerState{ XR_TYPE_ACTION_STATE_BOOLEAN }, gripState{ XR_TYPE_ACTION_STATE_BOOLEAN };
    XrActionStateBoolean lTrigState{ XR_TYPE_ACTION_STATE_BOOLEAN }, lGripState{ XR_TYPE_ACTION_STATE_BOOLEAN };
    XrActionStateBoolean xState{ XR_TYPE_ACTION_STATE_BOOLEAN }, yState{ XR_TYPE_ACTION_STATE_BOOLEAN };
    XrActionStateBoolean radioState{ XR_TYPE_ACTION_STATE_BOOLEAN }, r3State{ XR_TYPE_ACTION_STATE_BOOLEAN };
    XrActionStateBoolean l3State{ XR_TYPE_ACTION_STATE_BOOLEAN };
    XrActionStateBoolean confirmState{ XR_TYPE_ACTION_STATE_BOOLEAN }, backState{ XR_TYPE_ACTION_STATE_BOOLEAN };
    XrResult triggerResult = GetBool(g_triggerAction, g_rightHandPath, triggerState);
    XrResult gripResult = GetBool(g_gripAction, g_rightHandPath, gripState);
    GetBool(g_leftTriggerAction, g_leftHandPath, lTrigState);
    GetBool(g_leftGripAction, g_leftHandPath, lGripState);
    GetBool(g_xButtonAction, g_leftHandPath, xState);
    GetBool(g_yButtonAction, g_leftHandPath, yState);
    GetBool(g_radioAction, g_leftHandPath, radioState);
    XrResult r3Result = GetBool(g_r3ClickAction, g_rightHandPath, r3State);
    GetBool(g_l3ClickAction, g_leftHandPath, l3State);
    XrResult confirmResult = GetBool(g_confirmAction, g_rightHandPath, confirmState);
    XrResult backResult = GetBool(g_backAction, g_rightHandPath, backState);

    auto Down = [](const XrActionStateBoolean& s) { return s.isActive && s.currentState; };
    bool physDown[IN_COUNT] = {};
    physDown[IN_RTRIG] = Down(triggerState);
    physDown[IN_RGRIP] = Down(gripState);
    physDown[IN_A] = Down(confirmState);
    physDown[IN_B] = Down(backState);
    physDown[IN_X] = Down(xState);
    physDown[IN_Y] = Down(yState);
    physDown[IN_LTRIG] = Down(lTrigState);
    physDown[IN_LGRIP] = Down(lGripState);
    physDown[IN_LMENU] = Down(radioState);

    // Hands, located now so the gestures below can use this frame's poses.
    XrSpaceLocation rightHandLoc{ XR_TYPE_SPACE_LOCATION };
    xrLocateSpace(g_rightHandSpace, g_xrPlaySpace, predictedTime, &rightHandLoc);
    if (rightHandLoc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) g_rightHandFilter.Update(rightHandLoc.pose);
    XrSpaceLocation leftHandLoc{ XR_TYPE_SPACE_LOCATION };
    xrLocateSpace(g_leftHandSpace, g_xrPlaySpace, predictedTime, &leftHandLoc);
    if (leftHandLoc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) g_leftHandFilter.Update(leftHandLoc.pose);
    const XrSpaceLocationFlags needPose = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
    const bool leftValid = (leftHandLoc.locationFlags & needPose) == needPose;
    const bool rightValid = (rightHandLoc.locationFlags & needPose) == needPose;
    TrackHand(0, leftHandLoc.pose, leftValid);
    TrackHand(1, rightHandLoc.pose, rightValid);

    // The game's own controller settings, read live.
    int32_t padCfg[14];
    const bool haveCfg = ReadPadConfig(padCfg);
    if (haveCfg) LogPadConfigIfChanged(padCfg);

    // ---- L3: tap = reload the ini, hold ~1 s = remap panel ----------------------
    // Tap still re-reads [controls]/[input] (as since 2026-08-15). Holding it
    // opens the remap panel; with the panel open, a tap saves and closes.
    {
        static DWORD l3Down = 0;
        static bool l3Consumed = false;
        const bool l3 = Down(l3State);
        if (l3 && !l3Down) { l3Down = nowTick ? nowTick : 1; l3Consumed = false; }
        if (l3 && !l3Consumed && !g_remapOpen && nowTick - l3Down >= 900) {
            l3Consumed = true;
            RemapOpen();
        }
        if (!l3 && l3Down) {
            if (!l3Consumed) {
                if (g_remapOpen) RemapSaveAndClose();
                else {
                    LoadFallbackInputConfig();
                    DebugLogger::Log("L3: live-reloaded [controls]/[input] from mgs1_vr_config.ini");
                }
            }
            l3Down = 0;
        }
    }

    XINPUT_STATE virtualState{};
    virtualState.dwPacketNumber = GetTickCount();
    ActionOut out;
    bool wantEnter = false, wantEscape = false;

    if (g_remapOpen) {
        // Nothing reaches the game while the panel is up.
        RemapUpdate(physDown, stickState.isActive ? stickState.currentState.x : 0.0f,
                    stickState.isActive ? stickState.currentState.y : 0.0f, nowTick);
        ReleaseOpenXrDirectionalKeys();
        PublishMoveStickActive(false);
        bool noGrip = false;
        WristSelectInput(false, false, 0, 0, 0, 0, &noGrip, &noGrip);
    }
    else {
        // ---- WRIST QUICK-SELECT (2026-09-23) ---------------------------------
        // Holding a grip opens a compact item (left) / weapon (right) list on
        // that wrist; that hand's stick scrolls it and releasing the grip
        // equips. While it owns a grip, neither the grip nor that stick reaches
        // the game.
        // Wall press (hold right grip + move): decided BEFORE the wrist list,
        // so a grip that becomes a wall press never equips anything.
        float stickMag = 0.0f;
        if (stickState.isActive)
            stickMag = std::sqrt(stickState.currentState.x * stickState.currentState.x +
                                 stickState.currentState.y * stickState.currentState.y);
        // ---- GRAB (2026-10-02, round 3): grip with that hand on a wall --------
        // Decided on the press, before the wall press and the wrist list see
        // the grip, and it owns that grip until it is released:
        //   * RIGHT hand on the wall + grip, then push the left stick -> wall
        //     press (flatten and shimmy), like grabbing the wall to slide along.
        //   * otherwise -> ACTION once (ladder, elevator button): on a quick
        //     squeeze when it is let go, or after 250 ms of holding still.
        // ---- CODEC GESTURE (round 10): left grip with the left hand at your left
        // ear -> CODEC, like holding a radio to your ear. (X is the wall press
        // now; the left MENU button is still Codec too.)
        static bool s_codecOwns = false;
        static DWORD s_codecUntil = 0;
        {
            static bool prevL = false;
            const bool lg = physDown[IN_LGRIP];
            if (lg && !prevL && g_codecGesture && IsVrViewModeActive() && ModalState() == 0 && g_hand[0].have) {
                XrPosef head;
                if (GetLastHeadPose(&head)) {
                    const float earLocal[3] = { -0.08f, -0.02f, 0.03f };   // left of, a touch below and behind the eyes
                    float ear[3];
                    QRotV(head.orientation, earLocal, ear);
                    ear[0] += head.position.x; ear[1] += head.position.y; ear[2] += head.position.z;
                    const float dx = g_hand[0].p[0] - ear[0], dy = g_hand[0].p[1] - ear[1], dz = g_hand[0].p[2] - ear[2];
                    const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
                    static int s_codecLog = 0;
                    if (d <= g_codecEarM) {
                        s_codecOwns = true;
                        s_codecUntil = nowTick + 150;
                        Buzz(0, 0.6f, 60);
                        if (s_codecLog < 10) { s_codecLog++; DebugLogger::LogFormat("Codec: left hand at your ear (%.0f cm) + grip -> CODEC", d * 100.0f); }
                    }
                    else if (s_codecLog < 10 && d <= g_codecEarM * 2.0f) {
                        s_codecLog++;
                        DebugLogger::LogFormat("Codec: left grip %.0f cm from your ear -- not close enough (codec_ear_cm=%.0f)", d * 100.0f, g_codecEarM * 100.0f);
                    }
                }
            }
            if (!lg) s_codecOwns = false;
            prevL = lg;
        }
        static bool s_grabOwns[2] = { false, false };       // 0 = left, 1 = right
        static bool s_grabFired[2] = { false, false };
        static DWORD s_grabSince[2] = { 0, 0 };
        static DWORD s_grabUntil = 0;
        bool grabStartsWallPress = false;
        {
            static bool prevGrip[2] = { false, false };
            const bool grip[2] = { physDown[IN_LGRIP], physDown[IN_RGRIP] };
            const bool grabGate = g_grabAction && IsVrViewModeActive() && IsFpvActive() &&
                                  !IsCutsceneVrActive() && ModalState() == 0 && !IsScopeViewActive();
            auto fireAction = [&](int h) {
                if (s_grabFired[h]) return;
                s_grabFired[h] = true;
                s_grabUntil = nowTick + (DWORD)g_grabActionMs;
                Buzz(h, 0.6f, 40);
                static int grabLog = 0;
                if (grabLog < 8) {
                    grabLog++;
                    DebugLogger::LogFormat("Grab: %s hand on a wall + grip -> ACTION", h ? "right" : "left");
                }
            };
            for (int h = 0; h < 2; ++h) {
                if (grip[h] && !prevGrip[h] && grabGate && !(h == 0 && s_codecOwns) && MotionAimHandTouchingWall(h)) {
                    s_grabOwns[h] = true;
                    s_grabFired[h] = false;
                    s_grabSince[h] = nowTick;
                    Buzz(h, 0.25f, 25);
                }
                if (s_grabOwns[h]) {
                    if (!grip[h]) {
                        if (nowTick - s_grabSince[h] < 250) fireAction(h);   // quick squeeze
                        s_grabOwns[h] = false;
                    }
                    else if (h == 1 && !s_grabFired[h] && g_wallPressMode == 1 && stickMag >= 0.5f) {
                        // Grabbed the wall and pushed the stick: shimmy.
                        s_grabOwns[h] = false;
                        grabStartsWallPress = true;
                        DebugLogger::Log("Grab: right hand on the wall + stick -> wall press");
                    }
                    else if (nowTick - s_grabSince[h] >= 250) fireAction(h);
                }
                prevGrip[h] = grip[h];
            }
        }
        const bool gripL = physDown[IN_LGRIP] && !s_grabOwns[0] && !s_codecOwns;
        const bool gripR = physDown[IN_RGRIP] && !s_grabOwns[1];

        const bool wallOwnsRightGrip = WallGripUpdate(gripR, stickMag, nowTick, grabStartsWallPress);

        bool wristOwnsLeft = false, wristOwnsRight = false;
        WristSelectInput(gripL, gripR && !wallOwnsRightGrip,
                         stickState.isActive ? stickState.currentState.x : 0.0f,
                         stickState.isActive ? stickState.currentState.y : 0.0f,
                         lookState.isActive ? lookState.currentState.x : 0.0f,
                         lookState.isActive ? lookState.currentState.y : 0.0f,
                         &wristOwnsLeft, &wristOwnsRight);
        if (wristOwnsLeft)  { stickState.currentState.x = 0.0f; stickState.currentState.y = 0.0f; }
        if (wristOwnsRight) { lookState.currentState.x = 0.0f;  lookState.currentState.y = 0.0f; }

        // ---- WALL PRESS (mode 3): hold X ------------------------------------------
        static bool s_wallDirect = false;      // the stick below is already in the game's frame
        s_wallDirect = false;
        {
            static bool s_wallBtnPrev = false;
            bool wallBtn = false;
            for (int i = 0; i < IN_COUNT; ++i) if (physDown[i] && g_bind[i] == ACT_WALL) wallBtn = true;
            const bool edge = wallBtn && !s_wallBtnPrev;
            s_wallBtnPrev = wallBtn;
            WallHoldUpdate(wallBtn, edge, nowTick);
            float rx = lookState.isActive ? lookState.currentState.x : 0.0f;
            if (g_invertLookStick) rx = -rx;
            float wx = 0.0f, wy = 0.0f;
            if (WallHoldStick(wx, wy, rx)) {
                s_wallDirect = true;
                stickState.isActive = true;
                stickState.currentState.x = wx; stickState.currentState.y = wy;
                lookState.currentState.x = 0.0f; lookState.currentState.y = 0.0f;
            }
        }

        // ---- RIGHT STICK TURNS SNAKE'S EYES (REX top, round 7) ------------------
        // "Like a normal VR session: left stick moves, right stick turns." The
        // game's first-person turning does not exist in that fight, so the
        // right stick turns the view itself, and the left stick (turned into
        // your view) moves Snake where you look.
        {
            static ULONGLONG s_lastTurn = 0;
            const ULONGLONG t = GetTickCount64();
            float dt = s_lastTurn ? (float)(t - s_lastTurn) / 1000.0f : 0.0f;
            if (dt > 0.05f) dt = 0.05f;
            s_lastTurn = t;
            if (g_lookOnRightStick && lookState.isActive && !g_wallHold && GameplayPovWantsStickTurn()) {
                float tx = lookState.currentState.x;
                if (g_invertLookStick) tx = -tx;
                const float a = std::fabs(tx);
                if (a > 0.25f) {
                    const float k = (a - 0.25f) / 0.75f * (tx > 0 ? 1.0f : -1.0f);
                    AddGameplayPovYaw(-k * g_povTurnDegPerSec * (3.14159265f / 180.0f) * dt);
                    static int s_turnLog = 0;
                    if (s_turnLog < 3) { s_turnLog++; DebugLogger::Log("Snake's eyes: right stick turning the view (REX top)"); }
                }
            }
        }

        // Twin-stick applies ONLY in first-person view. Outside FPV the left
        // stick keeps full 8-way movement.
        // ... and only while the game's first-person camera is what you see:
        // on REX the game's FPV flag stays 1 while Snake's eyes draws a
        // third-person fight, and twin-stick sent the right stick to turning.
        const bool twinStick = g_lookOnRightStick && IsFpvActive() && !IsGameplayPovActive() && !g_wallHold;

        float lookX = 0.0f;
        if (twinStick && lookState.isActive) {
            float lx = lookState.currentState.x;
            float ly = lookState.currentState.y;
            ApplyRadialDeadzone(lx, ly, 0.2f);
            lookX = g_invertLookStick ? -lx : lx;
            virtualState.Gamepad.sThumbRX = (SHORT)(lx * 32767.0f);
            virtualState.Gamepad.sThumbRY = (SHORT)(ly * 32767.0f);
        }

        PublishMoveStickActive(false);
        float rawX = 0.0f, rawY = 0.0f;
        if (stickState.isActive) {
            float x = stickState.currentState.x;
            float y = stickState.currentState.y;
            ApplyRadialDeadzone(x, y, 0.2f);
            rawX = x; rawY = y;
            PublishMoveStickActive(std::sqrt(x * x + y * y) > 0.3f);
            if (!s_wallDirect) RotateStickByHeadYaw(x, y);
            if (g_tempNative == TEMP_WALL) SteerStickForWallPress(x, y);

            virtualState.Gamepad.sThumbLX = (SHORT)(x * 32767.0f);
            virtualState.Gamepad.sThumbLY = (SHORT)(y * 32767.0f);

            SetOpenXrDirectionalKey(VK_UP, y > 0.5f, g_openXrUpDown);
            SetOpenXrDirectionalKey(VK_DOWN, y < -0.5f, g_openXrDownDown);
            const float turnX = twinStick ? lookX : x;
            SetOpenXrDirectionalKey(VK_RIGHT, turnX > 0.5f, g_openXrRightDown);
            SetOpenXrDirectionalKey(VK_LEFT, turnX < -0.5f, g_openXrLeftDown);
        }
        else if (twinStick) {
            SetOpenXrDirectionalKey(VK_UP, false, g_openXrUpDown);
            SetOpenXrDirectionalKey(VK_DOWN, false, g_openXrDownDown);
            SetOpenXrDirectionalKey(VK_RIGHT, lookX > 0.5f, g_openXrRightDown);
            SetOpenXrDirectionalKey(VK_LEFT, lookX < -0.5f, g_openXrLeftDown);
        }
        else {
            ReleaseOpenXrDirectionalKeys();
        }

        static bool lastTwin = false;
        if (twinStick != lastTwin) {
            DebugLogger::LogFormat("Twin-stick %s (right stick turns, left stick moves, auto-turn %s)",
                twinStick ? "ON" : "OFF", twinStick ? "suspended" : "restored");
            lastTwin = twinStick;
        }

        // ---- wall press / Nikita: hand the view to the game when it needs it ----
        WallPressUpdate(rawX, rawY, wristOwnsLeft, nowTick);
        NikitaUpdate(nowTick);

        // ---- buttons -> game actions ---------------------------------------------
        // ---- item use from the wrist (2026-10-02) -----------------------------------
        // With the ITEM list open on the left wrist, A uses the highlighted
        // item (ration etc.), like ACTION in the game's own item window. A is
        // then the wrist's for as long as it is held: no punch, no Enter.
        static bool s_aPrev = false, s_aOwnedByWrist = false;
        if (physDown[IN_A] && !s_aPrev && wristOwnsLeft) { WristSelectUseHighlighted(); s_aOwnedByWrist = true; }
        if (!physDown[IN_A]) s_aOwnedByWrist = false;
        s_aPrev = physDown[IN_A];

        for (int i = 0; i < IN_COUNT; ++i) {
            if (!physDown[i]) continue;
            if (i == IN_RGRIP && (wristOwnsRight || wallOwnsRightGrip)) continue;
            if (i == IN_LGRIP && wristOwnsLeft) continue;
            if (i == IN_A && s_aOwnedByWrist) continue;
            if (i == IN_LGRIP && s_grabOwns[0]) continue;
            if (i == IN_LGRIP && s_codecOwns) continue;
            if (i == IN_RGRIP && s_grabOwns[1]) continue;
            PressAction(out, g_bind[i], padCfg, haveCfg);
        }
        if ((int)(s_grabUntil - nowTick) > 0) PressAction(out, ACT_ACTION, padCfg, haveCfg);
        if ((int)(s_codecUntil - nowTick) > 0) PressAction(out, ACT_CODEC, padCfg, haveCfg);
        // Motion aim: tells the player's shots apart from an enemy's.
        MotionAimNoteFireButton(physDown[IN_RTRIG]);

        // ---- motion melee -----------------------------------------------------------
        // Gameplay Snake's eyes counts as first person here: the REX-top fist
        // fight runs with the game's camera routine stalled (reads as a
        // "cutscene" to the detector) and is exactly where punches matter.
        const bool meleeGate = IsVrViewModeActive() &&
                               ((IsFpvActive() && !IsCutsceneVrActive()) || IsGameplayPovActive()) &&
                               ModalState() == 0 && !wristOwnsLeft && !wristOwnsRight && !IsScopeViewActive();
        MeleeUpdate(meleeGate, physDown[IN_LTRIG], physDown[IN_RTRIG], nowTick, out, padCfg, haveCfg);

        // First press of each input names the action and the route it took, so
        // the mapping can be checked from the log alone.
        {
            static bool seen[IN_COUNT] = {};
            for (int i = 0; i < IN_COUNT; ++i) {
                if (!physDown[i] || seen[i]) continue;
                seen[i] = true;
                const int act = g_bind[i];
                char route[48] = "nothing";
                if (act == ACT_WALL) sprintf_s(route, "the mod (wall press)");
                else if (act == ACT_CODEC || act == ACT_PAUSE) sprintf_s(route, "keyboard key");
                else if (act >= 0) {
                    const int32_t code = (haveCfg && act < 14) ? padCfg[act] : -1;
                    if (code >= 0 && code <= kMaxGameButton) sprintf_s(route, "joystick button %d", (int)code);
                    else sprintf_s(route, g_keyboardSynthEnabled ? "keyboard key (not on the pad in the game's settings)" : "nothing (unassigned in the game)");
                }
                DebugLogger::LogFormat("Button first press: %s -> %s -> %s", kPhys[i].label, ActionLabel(act), route);
            }
        }

        wantEnter = g_menuKeysEnabled && physDown[IN_A] && !s_aOwnedByWrist;
        wantEscape = g_menuKeysEnabled && physDown[IN_B];

        // ---- PC menu double-confirm fix (2026-10-02) -----------------------------
        // Disassembly, mgsi.exe+2C826..+2C87C: while a PC (GDI) menu is open
        // ([0x71D17C] != 0) the game turns a press of the joystick button
        // assigned to ACTION into an Enter keypress itself (sets the key byte at
        // 0x9AD88D). We were ALSO sending a real Enter for the same A press, so
        // the menu consumed Enter, then got the game's own Enter a frame later:
        // one press, two confirms. When A is already pressing the ACTION
        // joystick button and a PC menu is open, the game's own path is enough.
        if (wantEnter && haveCfg && ReadGameIntOr(kPcMenuActiveRva, 0) != 0) {
            const int32_t actCode = padCfg[ACT_ACTION];
            if (actCode >= 0 && actCode <= kMaxGameButton && (out.mask & (1u << actCode))) {
                wantEnter = false;
                static int logged = 0;
                if (logged < 3) {
                    ++logged;
                    DebugLogger::LogFormat("PC menu: A presses joystick button %d (ACTION) and the game turns that "
                        "into Enter itself -- not sending a second Enter (fixes the double confirm)", (int)actCode);
                }
            }
        }
    }

    g_gameButtonMask.store(out.mask, std::memory_order_relaxed);

    if (confirmState.changedSinceLastSync) {
        DebugLogger::LogFormat("Confirm (A) state CHANGED: currentState=%d isActive=%d", confirmState.currentState ? 1 : 0, confirmState.isActive ? 1 : 0);
    }
    if (backState.changedSinceLastSync) {
        DebugLogger::LogFormat("Back (B) state CHANGED: currentState=%d isActive=%d", backState.currentState ? 1 : 0, backState.isActive ? 1 : 0);
    }

    // ---- ALL synthesised keyboard output, in one place ------------------------
    ApplySynthesizedKeyboard(out.vk, out.nvk, wantEnter, wantEscape);

    if (r3State.changedSinceLastSync) {
        DebugLogger::LogFormat("R3 state CHANGED: currentState=%d isActive=%d", r3State.currentState ? 1 : 0, r3State.isActive ? 1 : 0);
    }

    // ---- R3 HELD = RECENTRE ----------------------------------------------------
    // Handled on RELEASE so it can be told apart from a press.
    if (r3State.changedSinceLastSync && !r3State.currentState) {
        const DWORD now = GetTickCount();
        if (g_r3DownTick != 0 && (now - g_r3DownTick) >= kR3HoldRecenterMs) {
            if (IsVrViewModeActive()) {
                RequestCameraRecenter();
                DebugLogger::Log("R3 held: recentred the VR camera -- where you are looking is now forward.");
            }
            else {
                RequestVirtualScreenRecenter();
                DebugLogger::Log("R3 held: recentred the virtual screen -- it has been moved back in "
                    "front of you and pinned there.");
            }
            g_lastR3ToggleTick = 0;   // a hold is not half of a double-press
        }
        g_r3DownTick = 0;
    }

    if (r3State.changedSinceLastSync && r3State.currentState) {
        DWORD now = GetTickCount();
        g_r3DownTick = now;
        DebugLogger::LogFormat("R3 press registered at tick=%lu (last press tick=%lu, delta=%lu)", now, g_lastR3ToggleTick, g_lastR3ToggleTick != 0 ? (now - g_lastR3ToggleTick) : 0);
        if (g_lastR3ToggleTick != 0 && (now - g_lastR3ToggleTick) <= 450) {
            // ---- R3 DOUBLE-PRESS: THE VIEW SWITCH --------------------------
            // Flip the mod's own mode, then bring the game into line with it.
            // An explicit switch also cancels a temporary hand-back (Nikita /
            // wall press): the player's choice wins.
            g_tempNative = TEMP_NONE;
            g_lastFpvToggleRequestTick = now;   // the player asked for this
            const bool vrNow = ToggleVrViewMode();

            const bool gameFpv = IsFpvActive();
            const bool needToggle = (vrNow != gameFpv);
            if (needToggle) {
                SendKeyTap(g_nativeFpvToggleVk);
                Sleep(g_fpvDoubleTapMs);
                SendKeyTap(g_nativeFpvToggleVk);
            }
            g_fpvLookActive = vrNow;
            virtualState.Gamepad.wButtons |= XINPUT_GAMEPAD_RIGHT_THUMB;
            DebugLogger::LogFormat(
                "R3 double-press: view mode -> %s. Game's own FPV flag was %d; %s",
                vrNow ? "VR" : "NATIVE", gameFpv ? 1 : 0,
                needToggle
                    ? "sent the in-game first-person toggle to match."
                    : "already matches, no in-game toggle sent.");
            g_lastR3ToggleTick = 0;
        }
        else {
            g_lastR3ToggleTick = now;
        }
    }

    static DWORD lastInputLogTick = 0;
    if (nowTick - lastInputLogTick > 1500) {
        DebugLogger::LogFormat("OpenXR action state: stick_active=%d x=%.2f y=%.2f trigger_active=%d trigger=%d grip_active=%d grip=%d confirm_active=%d confirm=%d back_active=%d back=%d r3_active=%d r3=%d | mask=%08X | results stick=%d trig=%d grip=%d conf=%d back=%d r3=%d",
            stickState.isActive ? 1 : 0, stickState.currentState.x, stickState.currentState.y,
            triggerState.isActive ? 1 : 0, triggerState.currentState ? 1 : 0,
            gripState.isActive ? 1 : 0, gripState.currentState ? 1 : 0,
            confirmState.isActive ? 1 : 0, confirmState.currentState ? 1 : 0,
            backState.isActive ? 1 : 0, backState.currentState ? 1 : 0,
            r3State.isActive ? 1 : 0, r3State.currentState ? 1 : 0, out.mask,
            (int)stickResult, (int)triggerResult, (int)gripResult, (int)confirmResult, (int)backResult, (int)r3Result);
        lastInputLogTick = nowTick;
    }

    // GetInjectedGameButtons() -- and therefore EVERY game button dllmain.cpp
    // writes into the DirectInput state -- is gated on this flag; every input
    // counts, so a left-hand press alone keeps the virtual pad alive.
    bool hasAnyOpenXrActivity =
        stickState.isActive || triggerState.isActive || gripState.isActive ||
        lTrigState.isActive || lGripState.isActive || xState.isActive ||
        yState.isActive || radioState.isActive || confirmState.isActive ||
        backState.isActive || r3State.isActive;

    if (!g_virtualPadLockInit.load()) {
        InitializeCriticalSection(&g_virtualPadLock);
        g_virtualPadLockInit = true;
    }

    EnterCriticalSection(&g_virtualPadLock);
    g_virtualPadState = virtualState;
    LeaveCriticalSection(&g_virtualPadLock);

    bool previousInjected = g_hasVirtualPadState.load();
    g_hasVirtualPadState = hasAnyOpenXrActivity;

    if (previousInjected != hasAnyOpenXrActivity) {
        DebugLogger::LogFormat("OpenXR virtual pad availability changed: injected_state=%d (stick=%d trig=%d grip=%d confirm=%d back=%d r3=%d)",
            hasAnyOpenXrActivity ? 1 : 0, stickState.isActive ? 1 : 0, triggerState.isActive ? 1 : 0,
            gripState.isActive ? 1 : 0, confirmState.isActive ? 1 : 0, backState.isActive ? 1 : 0,
            r3State.isActive ? 1 : 0);
    }

    // ---- publish the right controller's AIM pose ---------------------------
    // Located at the SAME predictedTime as the head pose. Deliberately NOT
    // filtered: smoothing an aim ray only adds lag. BOTH validity bits needed.
    if (g_rightAimSpace != XR_NULL_HANDLE) {
        XrSpaceLocation aimLoc{ XR_TYPE_SPACE_LOCATION };
        xrLocateSpace(g_rightAimSpace, g_xrPlaySpace, predictedTime, &aimLoc);
        const bool aimValid =
            (aimLoc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) &&
            (aimLoc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT);

        static bool loggedFirstInvalid = false;
        if (!aimValid && !loggedFirstInvalid) {
            loggedFirstInvalid = true;
            DebugLogger::LogFormat(
                "Aim: aim pose located but NOT valid (locationFlags=0x%llX) -- the right controller is "
                "not being tracked. The laser stays hidden until it is.",
                (unsigned long long)aimLoc.locationFlags);
        }

        PublishControllerAim(aimLoc.pose, aimValid);
    }
    else {
        XrPosef dummy{};
        dummy.orientation.w = 1.0f;
        PublishControllerAim(dummy, false);
    }
    PublishControllerGrips(leftHandLoc.pose, leftValid, rightHandLoc.pose, rightValid,
                           physDown[IN_LGRIP] && !g_remapOpen, physDown[IN_RGRIP] && !g_remapOpen && g_tempNative != TEMP_WALL);
}

void UpdateFallbackXInput() {
    static bool logged = false;
    static bool wDown = false;
    static bool aDown = false;
    static bool sDown = false;
    static bool dDown = false;
    static bool lmbDown = false;
    static bool rmbDown = false;
    static bool selectDown = false;
    static bool backDown = false;
    static bool fpvToggleDown = false;

    XINPUT_STATE state{};
    if (XInputGetState(0, &state) != ERROR_SUCCESS) {
        return;
    }

    if (!logged) {
        DebugLogger::Log("Fallback input active: XInput controller 0 detected");
        logged = true;
    }

    auto normalize = [](SHORT v) -> float {
        if (v >= 0) return (float)v / 32767.0f;
        return (float)v / 32768.0f;
    };

    float lx = normalize(state.Gamepad.sThumbLX);
    float ly = normalize(state.Gamepad.sThumbLY);

    if (std::abs(lx) < g_fallbackMouseDeadzone) lx = 0.0f;
    if (std::abs(ly) < g_fallbackMouseDeadzone) ly = 0.0f;
    RotateStickByHeadYaw(lx, ly);

    bool w = ly > 0.4f;
    bool s = ly < -0.4f;
    bool d = lx > 0.4f;
    bool a = lx < -0.4f;

    if (w != wDown) { SendVirtualKey(VK_UP, w); wDown = w; }
    if (s != sDown) { SendVirtualKey(VK_DOWN, s); sDown = s; }
    if (d != dDown) { SendVirtualKey(VK_RIGHT, d); dDown = d; }
    if (a != aDown) { SendVirtualKey(VK_LEFT, a); aDown = a; }

    bool lmb = state.Gamepad.bRightTrigger > 40;
    bool rmb = (state.Gamepad.wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER) != 0;

    if (lmb != lmbDown) {
        SendVirtualMouseClick(lmb ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP);
        lmbDown = lmb;
    }

    if (rmb != rmbDown) {
        SendVirtualMouseClick(rmb ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP);
        rmbDown = rmb;
    }

    bool selectPressed = (state.Gamepad.wButtons & XINPUT_GAMEPAD_A) != 0;
    if (selectPressed != selectDown) {
        SendVirtualKey(VK_RETURN, selectPressed);
        SendVirtualKey(VK_SPACE, selectPressed);
        selectDown = selectPressed;
    }

    bool backPressed = (state.Gamepad.wButtons & XINPUT_GAMEPAD_B) != 0;
    if (backPressed != backDown) {
        SendVirtualKey(VK_ESCAPE, backPressed);
        SendVirtualKey(VK_BACK, backPressed);
        backDown = backPressed;
    }

    bool togglePressed = (state.Gamepad.wButtons & XINPUT_GAMEPAD_RIGHT_THUMB) != 0;
    if (togglePressed && !fpvToggleDown) {
        g_lastFpvToggleRequestTick = GetTickCount();   // the player asked for this
        SendKeyTap(g_nativeFpvToggleVk);
        Sleep(g_fpvDoubleTapMs);
        SendKeyTap(g_nativeFpvToggleVk);
        g_fpvLookActive = !g_fpvLookActive;
        DebugLogger::LogFormat("Fallback FPV double-tap sent (VK=%u, delay=%dms), fpv_look_active=%d", (unsigned)g_nativeFpvToggleVk, g_fpvDoubleTapMs, g_fpvLookActive ? 1 : 0);
    }
    fpvToggleDown = togglePressed;

    if (g_fallbackMouseLookEnabled && g_fpvLookActive) {
        float rx = normalize(state.Gamepad.sThumbRX);
        float ry = normalize(state.Gamepad.sThumbRY);

        if (std::abs(rx) < g_fallbackMouseDeadzone) rx = 0.0f;
        if (std::abs(ry) < g_fallbackMouseDeadzone) ry = 0.0f;

        LONG dx = (LONG)(rx * g_fallbackMouseLookSensitivity);
        LONG dy = (LONG)(-ry * g_fallbackMouseLookSensitivity);
        SendVirtualMouseMove(dx, dy);
    }
}

static DWORD WINAPI FallbackInputThreadProc(LPVOID) {
    DebugLogger::Log("Fallback input thread started");
    while (true) {
        UpdateFallbackXInput();
        Sleep(8);
    }
    return 0;
}

bool GetInjectedXInputState(XINPUT_STATE* outState) {
    if (!outState || !g_hasVirtualPadState.load() || !g_virtualPadLockInit.load()) {
        return false;
    }

    EnterCriticalSection(&g_virtualPadLock);
    *outState = g_virtualPadState;
    LeaveCriticalSection(&g_virtualPadLock);
    return true;
}

bool IsFpvLookActive() {
    return g_fpvLookActive;
}

// Sends the SAME real key-tap sequence the game's own input handling
// expects for its native FPV toggle gesture (two taps within
// g_fpvDoubleTapMs), rather than writing directly into game memory. Safer
// than a raw memory force-write: goes through the game's real input path,
// so whatever validation logic it does internally (gesture timing, input
// source checks, etc.) is satisfied the normal way. Blocking (uses Sleep
// for the inter-tap delay) -- call from a background/init thread, not a
// latency-sensitive per-frame path.
//
// NOTE: this deliberately does NOT stamp g_lastFpvToggleRequestTick. It is
// what the FPV auto-restore calls, and stamping it would make every restore
// look like a user request -- which would clear the very intent that caused
// the restore, so first person would be given back once and then abandoned.
void TriggerFpvDoubleTap() {
    bool focused = EnsureGameWindowFocused();
    if (!focused) {
        DebugLogger::Log("TriggerFpvDoubleTap: game window is not focused -- sending anyway, but this key-tap will likely go nowhere (see EnsureGameWindowFocused log above for why)");
    }

    // Back to SendKeyTap (SendInput-based): the DirectInput keyboard
    // injection path was never actually active in any session (the
    // keyboard device hook's GUID_SysKeyboard branch never fires -- this
    // game apparently doesn't create a DirectInput keyboard device through
    // the hooked CreateDevice/CreateDeviceEx at all). The real root cause
    // of FPV never engaging was confirmed to be a wrong key (V, VK=86)
    // instead of the actual in-game-bound key (X, VK=88) -- SendInput
    // itself was always correctly delivering whatever key we asked for.
    SendKeyTap(g_nativeFpvToggleVk);
    Sleep(g_fpvDoubleTapMs);
    SendKeyTap(g_nativeFpvToggleVk);

    if (focused) {
        g_fpvLookActive = !g_fpvLookActive;
    }
    // else: don't flip the tracked flag if focus wasn't confirmed -- the
    // real game state almost certainly didn't change either.

    DebugLogger::LogFormat("Auto-triggered FPV double-tap at startup via SendInput (VK=%u delay=%dms), window_focused=%d, fpv_look_active=%d",
        (unsigned)g_nativeFpvToggleVk, g_fpvDoubleTapMs, focused ? 1 : 0, g_fpvLookActive ? 1 : 0);
}

void StartFallbackInputThread() {
    static bool started = false;
    if (started) {
        return;
    }

    LoadFallbackInputConfig();

    HANDLE hThread = CreateThread(nullptr, 0, FallbackInputThreadProc, nullptr, 0, nullptr);
    if (hThread) {
        CloseHandle(hThread);
        started = true;
    }
    else {
        DebugLogger::Log("ERROR: Failed to start fallback input thread");
    }
}

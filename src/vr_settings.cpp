#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <d3d11.h>
#define XR_USE_GRAPHICS_API_D3D11
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include "../include/vr_settings.h"
#include "../include/vr_aim.h"
#include "../include/motion_aim.h"
#include "../include/camera_write_hook.h"
#include "../include/debug_logging.h"

// See vr_settings.h for what this is. This file owns: the option table, the
// panel's state and navigation, writing the ini, asking the owning modules to
// re-read it, and drawing the panel into its own swapchain.

void VrSettingsEnsureLaser();   // below: builds the laser swapchain if it was off at startup

namespace {

// ===========================================================================
// The option table
// ===========================================================================

// Which module re-reads an option once it is written.
enum : uint32_t {
    AP_INPUT   = 1u << 0,   // vr_input.cpp      LoadFallbackInputConfig
    AP_AIM     = 1u << 1,   // vr_aim.cpp        LoadAimConfig (+ laser init if it was off)
    AP_WRIST   = 1u << 2,   // vr_aim.cpp        LoadWristHudConfig ([wrist_hud] + [subtitles])
    AP_MOTION  = 1u << 3,   // motion_aim.cpp    LoadMotionAimConfig ([motion_aim] + [hands])
    AP_CAMERA  = 1u << 4,   // camera_write_hook ReloadCameraHookLiveSettings (+ [scope])
    AP_DISPLAY = 1u << 5,   // vr_injection.cpp  ReloadDisplayLiveSettings
    AP_RESTART = 1u << 8,   // saved now, used on the next launch
};

enum Kind { K_BOOL, K_INT, K_ENUM, K_ENUM_STR };

struct Opt { int v; const char* s; const char* label; };

struct Item {
    const char* label;
    const char* section;
    const char* key;
    Kind kind;
    int def;                 // K_BOOL / K_INT / K_ENUM: the shipped value
    const char* defStr;      // K_ENUM_STR: the shipped value
    int lo, hi, step;        // K_INT
    int div, decimals;       // K_INT display: value / div with N decimals
    const char* unit;        // K_INT display suffix
    const char* zeroLabel;   // K_INT: shown instead of 0 (e.g. "ALWAYS")
    const Opt* opts; int nOpts;
    uint32_t apply;
    const char* help;
};

#define N_OPTS(a) a, (int)(sizeof(a) / sizeof(a[0]))
#define BOOL_ITEM(lbl, sec, key, def, ap, help) \
    { lbl, sec, key, K_BOOL, def, nullptr, 0, 1, 1, 1, 0, "", nullptr, nullptr, 0, ap, help }
#define INT_ITEM(lbl, sec, key, def, lo, hi, step, div, dec, unit, zero, ap, help) \
    { lbl, sec, key, K_INT, def, nullptr, lo, hi, step, div, dec, unit, zero, nullptr, 0, ap, help }
#define ENUM_ITEM(lbl, sec, key, def, opts, ap, help) \
    { lbl, sec, key, K_ENUM, def, nullptr, 0, 0, 1, 1, 0, "", nullptr, N_OPTS(opts), ap, help }
#define ACTION_ITEM(lbl, key, defStr) \
    { lbl, "controls", key, K_ENUM_STR, 0, defStr, 0, 0, 1, 1, 0, "", nullptr, N_OPTS(kActionOpts), AP_INPUT, \
      "WHAT THIS BUTTON DOES IN THE GAME. SAME NAMES AS [CONTROLS] IN THE INI." }

const Opt kCutsceneView[] = { { 0, nullptr, "DIRECTOR CAMERA" }, { 1, nullptr, "SNAKE'S EYES" } };
const Opt kNoSnake[]      = { { 0, nullptr, "DIRECTOR CAMERA" }, { 1, nullptr, "VIRTUAL SCREEN" }, { 2, nullptr, "STAY IN HIS EYES" } };
const Opt kBars[]         = { { 0, nullptr, "REMOVED" }, { 1, nullptr, "KEPT" } };
const Opt kPacing[]       = { { 0, nullptr, "OFF" }, { 1, nullptr, "TIMING" }, { 2, nullptr, "EVEN (LOCKED)" } };
const Opt kStickTurn[]    = { { 0, nullptr, "OFF" }, { 1, nullptr, "REX + JEEP" }, { 2, nullptr, "EVERYWHERE" } };
const Opt kBodyFollow[]   = { { 0, nullptr, "OFF" }, { 1, nullptr, "WHILE MOVING" }, { 2, nullptr, "ALWAYS" } };
const Opt kQuickSel[]     = { { 0, nullptr, "GAME MENU" }, { 1, nullptr, "WRIST LIST" } };
const Opt kWallPress[]    = { { 0, nullptr, "OFF" }, { 1, nullptr, "GRIP + STICK" }, { 2, nullptr, "AUTOMATIC" }, { 3, nullptr, "HOLD BUTTON" } };
const Opt kCapHeight[]    = { { 480, nullptr, "480 (FASTEST)" }, { 600, nullptr, "600" }, { 768, nullptr, "768" }, { 960, nullptr, "960 (SHARPEST)" } };
// Same keys vr_input.cpp's [controls] parser accepts.
const Opt kActionOpts[] = {
    { 0, "none", "NONE" }, { 0, "weapon", "WEAPON" }, { 0, "action", "ACTION" }, { 0, "crawl", "CRAWL" },
    { 0, "first_person", "FIRST PERSON" }, { 0, "switch_item", "SWITCH ITEM" }, { 0, "items", "ITEMS" },
    { 0, "switch_weapon", "SWITCH WEAPON" }, { 0, "weapons", "WEAPONS" }, { 0, "codec", "CODEC" },
    { 0, "pause", "PAUSE" }, { 0, "wall_press", "WALL PRESS" },
};

// Defaults below are the values the shipped mgs1_vr_config.ini carries.
const Item kView[] = {
    ENUM_ITEM("CUTSCENES", "camera_hook", "cutscene_view", 1, kCutsceneView, AP_CAMERA,
        "SNAKE'S EYES: NO CUTS OR PANS, ONLY YOUR HEAD TURNS THE VIEW. DIRECTOR: THE GAME'S OWN CAMERA."),
    ENUM_ITEM("SHOTS WITHOUT SNAKE", "camera_hook", "cutscene_pov_no_snake", 2, kNoSnake, AP_CAMERA,
        "WHEN A CUTSCENE CUTS TO SOMEWHERE SNAKE ISN'T: SHOW THE GAME'S CAMERA, THE FLAT SCREEN, OR STAY WHERE SNAKE IS."),
    BOOL_ITEM("CAPTION PANEL", "camera_hook", "cutscene_pov_subtitle_panel", 1, AP_CAMERA,
        "LIFT CUTSCENE CAPTIONS ONTO A PANEL JUST BELOW YOUR GAZE SO THEY CAN BE READ."),
    ENUM_ITEM("CINEMA BARS", "camera_hook", "cutscene_pov_letterbox", 0, kBars, AP_CAMERA,
        "THE BLACK BARS OF CUTSCENES IN SNAKE'S EYES. THE VIRTUAL SCREEN ALWAYS KEEPS THEM."),
    INT_ITEM("SHARPENING", "openxr", "sharpen_percent", 25, 0, 100, 5, 1, 0, "%", "OFF", AP_DISPLAY,
        "RESTORES EDGE CONTRAST LOST STRETCHING THE GAME IMAGE ACROSS EACH EYE. 30-60 IS A GOOD RANGE."),
    INT_ITEM("EDGE SMOOTHING", "openxr", "post_aa_percent", 100, 0, 100, 10, 1, 0, "%", "OFF", AP_DISPLAY,
        "ANTI-ALIASING ON JAGGED POLYGON EDGES. A PICTURE FILTER ONLY: THE GAME IS UNTOUCHED."),
    ENUM_ITEM("FRAME PACING", "openxr", "frame_pacing", 2, kPacing, AP_DISPLAY,
        "EVEN: EVERY GAME IMAGE SHOWN FOR THE SAME NUMBER OF HEADSET FRAMES. SMOOTHEST AT 90 HZ."),
    INT_ITEM("SCREEN DISTANCE", "openxr", "native_screen_distance_cm", 250, 100, 800, 25, 100, 2, " M", nullptr, AP_DISPLAY,
        "HOW FAR AWAY THE VIRTUAL SCREEN (MENUS, CODEC, FLAT MODE) FLOATS."),
    INT_ITEM("SCREEN WIDTH", "openxr", "native_screen_width_cm", 320, 100, 1000, 20, 100, 1, " M", nullptr, AP_DISPLAY,
        "HOW WIDE THE VIRTUAL SCREEN IS. 3.2 M AT 2.5 M IS A VERY LARGE TV FROM A SOFA."),
};

const Item kComfort[] = {
    BOOL_ITEM("LEFT-HANDED", "input", "left_handed", 0, AP_INPUT,
        "GUN IN YOUR LEFT HAND: LEFT TRIGGER FIRES, A/B SWAP WITH X/Y, MOVE ON THE RIGHT STICK, TURN ON THE LEFT."),
    ENUM_ITEM("WALK WHERE YOU LOOK", "camera_hook", "body_follows_head", 1, kBodyFollow, AP_CAMERA,
        "IN FIRST PERSON SNAKE TURNS TO FACE WHERE YOU LOOK, SO PUSHING FORWARD WALKS THAT WAY. OFF = TURN WITH THE STICK ONLY."),
    BOOL_ITEM("TURN WITH RIGHT STICK", "input", "look_on_right_stick", 1, AP_INPUT,
        "ON: THE RIGHT STICK TURNS, THE LEFT STICK ONLY WALKS FORWARD AND BACK. OFF: LEFT STICK LEFT/RIGHT TURNS SNAKE, AS IN THE ORIGINAL GAME."),
    BOOL_ITEM("INVERT TURN STICK", "input", "invert_look_stick", 0, AP_INPUT,
        "REVERSES THE RIGHT STICK'S TURN DIRECTION."),
    ENUM_ITEM("STICK TURNING ON REX", "camera_hook", "gameplay_pov_right_stick_turn", 1, kStickTurn, AP_CAMERA,
        "ON TOP OF METAL GEAR REX AND IN THE JEEP THE GAME CANNOT TURN YOUR VIEW. THIS LETS THE TURN STICK DO IT."),
    INT_ITEM("REX TURN SPEED", "input", "pov_turn_deg_per_s", 150, 45, 360, 15, 1, 0, " DEG/S", nullptr, AP_INPUT,
        "HOW FAST THE TURN STICK TURNS YOU ON TOP OF REX AND IN THE JEEP."),
    BOOL_ITEM("CONTROLLER RUMBLE", "melee", "rumble", 1, AP_INPUT,
        "A BUZZ WHEN A PUNCH, GRAB, WALL PRESS OR SCOPE REGISTERS."),
};

const Item kHud[] = {
    INT_ITEM("WRIST PANEL SIZE", "wrist_hud", "width_mm", 90, 50, 200, 10, 1, 0, " MM", nullptr, AP_WRIST,
        "WIDTH OF THE LIFE AND WEAPON PANELS ON YOUR WRISTS."),
    INT_ITEM("SHOW WHEN LOOKED AT", "wrist_hud", "gaze_cone_deg", 28, 0, 60, 2, 1, 0, " DEG", "ALWAYS", AP_WRIST,
        "A WRIST PANEL APPEARS WHEN YOU LOOK WITHIN THIS ANGLE OF IT."),
    BOOL_ITEM("RADAR ON WRIST", "wrist_hud", "show_radar", 1, AP_WRIST,
        "THE SOLITON RADAR ON YOUR LEFT WRIST INSTEAD OF IN YOUR VIEW."),
    BOOL_ITEM("BOSS LIFE ON WRIST", "wrist_hud", "show_boss_life", 1, AP_WRIST,
        "BOSS LIFE BARS ON YOUR LEFT WRIST."),
    ENUM_ITEM("QUICK SELECT", "wrist_hud", "select_on_wrist", 1, kQuickSel, AP_WRIST,
        "HOLD A GRIP: A COMPACT LIST ON THE WRIST, OR THE GAME'S OWN SCROLLING MENU."),
    INT_ITEM("LIFE BAR LENGTH", "wrist_hud", "life_bar_scale_percent", 125, 50, 250, 25, 1, 0, "%", nullptr, AP_WRIST,
        "SCALE OF THE LIFE BAR ON THE WRIST. IT GROWS WITH SNAKE'S MAX LIFE."),
    BOOL_ITEM("AIM LASER", "aim", "laser_enabled", 1, AP_AIM,
        "DOTS ALONG WHERE YOUR RIGHT HAND POINTS: WHERE BULLETS GO."),
    INT_ITEM("LASER DOT SIZE", "aim", "laser_size_tenth_deg", 15, 3, 40, 1, 10, 1, " DEG", nullptr, AP_AIM,
        "SIZE OF THE AIM LASER DOTS."),
    INT_ITEM("CAPTION DISTANCE", "subtitles", "panel_distance_cm", 110, 50, 250, 10, 100, 1, " M", nullptr, AP_WRIST,
        "HOW FAR AWAY THE CUTSCENE CAPTION PANEL SITS."),
    INT_ITEM("CAPTION WIDTH", "subtitles", "panel_width_cm", 80, 30, 200, 10, 100, 1, " M", nullptr, AP_WRIST,
        "HOW WIDE THE CUTSCENE CAPTION PANEL IS."),
    BOOL_ITEM("TEXT ON VIRTUAL SCREEN", "subtitles", "draw_on_screen", 1, AP_WRIST,
        "DRAW THE GAME'S CAPTIONS AND CODEC TEXT INTO THE VIRTUAL SCREEN PICTURE."),
};

const Item kHands[] = {
    BOOL_ITEM("PUNCH GESTURE", "melee", "punch", 1, AP_INPUT,
        "THROW A REAL PUNCH TO PUNCH. THE BUTTON STILL WORKS TOO."),
    INT_ITEM("PUNCH SPEED NEEDED", "melee", "punch_speed_cms", 200, 100, 400, 25, 100, 2, " M/S", nullptr, AP_INPUT,
        "HOW FAST YOUR HAND MUST MOVE FORWARD TO COUNT AS A PUNCH."),
    BOOL_ITEM("CHOKEHOLD GESTURE", "melee", "chokehold", 1, AP_INPUT,
        "UNARMED, BOTH HANDS TOGETHER IN FRONT OF YOU GRABS. JERK THEM TO SNAP."),
    BOOL_ITEM("CODEC AT YOUR EAR", "input", "codec_ear_gesture", 1, AP_INPUT,
        "LEFT GRIP WITH YOUR LEFT HAND AT YOUR LEFT EAR OPENS THE CODEC."),
    BOOL_ITEM("GRAB WALLS + LADDERS", "input", "grab_action", 1, AP_INPUT,
        "SQUEEZE A GRIP WITH THAT HAND ON A WALL: CLIMB LADDERS, PRESS BUTTONS."),
    ENUM_ITEM("WALL PRESS", "input", "wall_press_mode", 3, kWallPress, AP_INPUT,
        "HOLD BUTTON: HOLD WALL PRESS, SNAKE FLATTENS, RIGHT STICK SIDLES. RELEASE TO STOP."),
    BOOL_ITEM("PSG1 RAISE TO EYE", "scope", "psg1_raise_to_eye", 1, AP_CAMERA,
        "THE SNIPER SCOPE OPENS ONLY WHEN YOU LIFT THE RIFLE TO YOUR FACE."),
    BOOL_ITEM("GUN IN YOUR HAND", "motion_aim", "gun_in_hand", 1, AP_MOTION,
        "GUNS ARE HELD IN YOUR RIGHT HAND AND FIRE WHERE IT POINTS."),
    BOOL_ITEM("THROW BY SWINGING", "motion_aim", "throw_swing", 1, AP_MOTION,
        "A REAL THROWING MOTION SETS GRENADE DIRECTION AND STRENGTH."),
    BOOL_ITEM("HAND SMOOTHING", "motion_aim", "hand_filter", 1, AP_MOTION,
        "STEADIES THE GUN AND HANDS WHILE YOUR HAND IS STILL. NO LAG WHEN IT MOVES."),
    BOOL_ITEM("FULL BODY (TEST)", "hands", "full_body", 0, AP_MOTION,
        "EXPERIMENTAL: DRAW SNAKE'S WHOLE BODY, ARMS REACHING TO YOUR HANDS. OFF = JUST HANDS."),
    BOOL_ITEM("SHOW FOREARMS", "hands", "show_forearms", 0, AP_MOTION,
        "DRAW SNAKE'S FOREARMS BEHIND YOUR HANDS (HANDS-ONLY MODE)."),
    BOOL_ITEM("HANDS STOP AT WALLS", "hands", "wall_collision", 1, AP_MOTION,
        "YOUR HANDS AND GUN STOP AT WALLS INSTEAD OF PASSING THROUGH."),
    BOOL_ITEM("KNOCK ON WALLS", "hands", "knock", 1, AP_MOTION,
        "RAP A WALL WITH YOUR HAND TO KNOCK, LIKE SNAKE'S KNOCK. GUARDS HEAR IT."),
    BOOL_ITEM("NIKITA ON SCREEN", "weapons", "nikita_virtual_screen", 1, AP_INPUT,
        "STEER NIKITA MISSILES ON THE VIRTUAL SCREEN."),
};

const Item kButtons[] = {
    ACTION_ITEM("RIGHT TRIGGER", "right_trigger", "weapon"),
    ACTION_ITEM("RIGHT GRIP", "right_grip", "switch_weapon"),
    ACTION_ITEM("A", "a_button", "action"),
    ACTION_ITEM("B", "b_button", "first_person"),
    ACTION_ITEM("X", "x_button", "wall_press"),
    ACTION_ITEM("Y", "y_button", "pause"),
    ACTION_ITEM("LEFT TRIGGER", "left_trigger", "crawl"),
    ACTION_ITEM("LEFT GRIP", "left_grip", "items"),
    ACTION_ITEM("MENU", "left_menu", "codec"),
};

const Item kSystem[] = {
    BOOL_ITEM("START IN VR VIEW", "camera_hook", "start_in_vr_mode", 1, AP_RESTART,
        "OFF: THE GAME STARTS ON THE FLAT VIRTUAL SCREEN. R3 DOUBLE-PRESS SWITCHES ANY TIME."),
    INT_ITEM("EYE RESOLUTION", "openxr", "eye_resolution_percent", 50, 30, 100, 5, 1, 0, "%", nullptr, AP_RESTART,
        "RESOLUTION SENT TO THE HEADSET, AS A SHARE OF WHAT IT RECOMMENDS. HIGHER COSTS MORE GPU."),
    ENUM_ITEM("CAPTURE HEIGHT", "render_twice", "gpu_capture_height", 960, kCapHeight, AP_RESTART,
        "SHARPNESS OF THE GAME PICTURE IN THE HEADSET. LOWER IS LIGHTER ON OLDER GRAPHICS CARDS."),
    BOOL_ITEM("VR HANDS", "hands", "enabled", 1, AP_RESTART,
        "SNAKE'S HANDS (AND BODY) FOLLOW YOUR CONTROLLERS."),
    BOOL_ITEM("WRIST HUD", "wrist_hud", "enabled", 1, AP_RESTART,
        "LIFE, WEAPON, ITEM AND RADAR ON YOUR WRISTS."),
};

struct Page { const char* name; const Item* items; int count; };
const Page kPages[] = {
    { "VIEW",    kView,    (int)(sizeof(kView) / sizeof(kView[0])) },
    { "COMFORT", kComfort, (int)(sizeof(kComfort) / sizeof(kComfort[0])) },
    { "HUD",     kHud,     (int)(sizeof(kHud) / sizeof(kHud[0])) },
    { "HANDS",   kHands,   (int)(sizeof(kHands) / sizeof(kHands[0])) },
    { "BUTTONS", kButtons, (int)(sizeof(kButtons) / sizeof(kButtons[0])) },
    { "SYSTEM",  kSystem,  (int)(sizeof(kSystem) / sizeof(kSystem[0])) },
};
constexpr int kPageCount = (int)(sizeof(kPages) / sizeof(kPages[0]));
constexpr int kMaxItems = 16;

// ===========================================================================
// State (XR frame thread)
// ===========================================================================

std::atomic<bool> s_open{ false };
int  s_page = 0;
int  s_sel[kPageCount] = {};
int  s_scroll[kPageCount] = {};
// Current values. K_ENUM_STR keeps the option index, or -1 for a value the
// table does not know (a legacy number or alias): shown raw, kept untouched.
int  s_val[kPageCount][kMaxItems] = {};
char s_raw[kPageCount][kMaxItems][24] = {};
bool s_dirty[kPageCount][kMaxItems] = {};
uint32_t s_pendingApply = 0;
DWORD s_lastChange = 0;
bool s_restartChanged = false;
bool s_armed = false;           // inputs held when the panel opened are ignored until released
bool s_placed = false;
XrPosef s_pose{};
// Status line after a save.
DWORD s_statusTick = 0;
bool  s_statusOk = true;
int   s_statusCount = 0;

std::string IniPath() {
    char exe[MAX_PATH] = { 0 };
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    std::string p(exe);
    const auto sl = p.find_last_of("\\/");
    if (sl != std::string::npos) p = p.substr(0, sl);
    return p + "\\mgs1_vr_config.ini";
}

int OptIndexByValue(const Item& it, int v) {
    for (int i = 0; i < it.nOpts; ++i) if (it.opts[i].v == v) return i;
    return -1;
}
int OptIndexByKey(const Item& it, const char* s) {
    for (int i = 0; i < it.nOpts; ++i) if (it.opts[i].s && _stricmp(it.opts[i].s, s) == 0) return i;
    return -1;
}

void ReadAllFromIni() {
    const std::string ini = IniPath();
    for (int p = 0; p < kPageCount; ++p) {
        for (int i = 0; i < kPages[p].count && i < kMaxItems; ++i) {
            const Item& it = kPages[p].items[i];
            s_dirty[p][i] = false;
            if (it.kind == K_ENUM_STR) {
                char buf[64] = {};
                GetPrivateProfileStringA(it.section, it.key, it.defStr, buf, sizeof(buf), ini.c_str());
                // Value only: cut a trailing inline comment / whitespace.
                char t[24] = {};
                size_t j = 0;
                for (const char* c = buf; *c && j + 1 < sizeof(t); ++c) {
                    if (*c == ' ' || *c == '\t' || *c == ';') { if (j) break; else continue; }
                    t[j++] = *c;
                }
                if (!t[0]) strcpy_s(t, it.defStr);
                strcpy_s(s_raw[p][i], t);
                s_val[p][i] = OptIndexByKey(it, t);
            }
            else {
                s_val[p][i] = (int)GetPrivateProfileIntA(it.section, it.key, it.def, ini.c_str());
            }
        }
    }
}

// Re-read by the owning modules. XR frame thread, which is also where every
// one of these is safe to call (see vr_settings.h).
void ApplyPending() {
    const uint32_t ap = s_pendingApply;
    s_pendingApply = 0;
    if (ap & AP_INPUT)   ReloadInputConfigLive();
    if (ap & AP_AIM)     LoadAimConfig();
    if (ap & AP_WRIST)   LoadWristHudConfig();
    if (ap & AP_MOTION)  LoadMotionAimConfig();
    if (ap & AP_CAMERA)  ReloadCameraHookLiveSettings();
    if (ap & AP_DISPLAY) ReloadDisplayLiveSettings();
    if (ap & AP_AIM)     VrSettingsEnsureLaser();
    if (ap & ~AP_RESTART)
        DebugLogger::LogFormat("VR Settings: live-applied (modules 0x%02X)", (unsigned)(ap & 0xFF));
}

void Flush() {
    const std::string ini = IniPath();
    int written = 0, failed = 0;
    for (int p = 0; p < kPageCount; ++p) {
        for (int i = 0; i < kPages[p].count && i < kMaxItems; ++i) {
            if (!s_dirty[p][i]) continue;
            const Item& it = kPages[p].items[i];
            char v[32] = {};
            if (it.kind == K_ENUM_STR) {
                const int k = s_val[p][i];
                strcpy_s(v, (k >= 0 && k < it.nOpts) ? it.opts[k].s : s_raw[p][i]);
            }
            else sprintf_s(v, "%d", s_val[p][i]);
            if (WritePrivateProfileStringA(it.section, it.key, v, ini.c_str())) {
                ++written;
                DebugLogger::LogFormat("VR Settings: [%s] %s=%s", it.section, it.key, v);
            }
            else {
                ++failed;
                DebugLogger::LogFormat("VR Settings: could NOT write [%s] %s=%s to %s (error %lu) -- is the game "
                    "folder read-only, or the ini open elsewhere?", it.section, it.key, v, ini.c_str(), GetLastError());
            }
            s_dirty[p][i] = false;
        }
    }
    if (!written && !failed) { s_pendingApply = 0; return; }
    WritePrivateProfileStringA(nullptr, nullptr, nullptr, ini.c_str());   // flush the profile cache
    {
        // The game lives under Program Files, and an old unmanifested 32-bit
        // exe there gets its writes silently redirected to a per-user copy
        // under VirtualStore. The game keeps reading that copy, so the panel
        // still works -- but hand edits to the real file would then be ignored.
        static bool s_vsLogged = false;
        char local[MAX_PATH] = {};
        if (!s_vsLogged && GetEnvironmentVariableA("LOCALAPPDATA", local, MAX_PATH) && ini.size() > 2 && ini[1] == ':') {
            const std::string vs = std::string(local) + "\\VirtualStore" + ini.substr(2);
            if (GetFileAttributesA(vs.c_str()) != INVALID_FILE_ATTRIBUTES) {
                s_vsLogged = true;
                DebugLogger::LogFormat("VR Settings: NOTE -- Windows is redirecting this game's ini writes to %s "
                    "(the game folder is write-protected). The game reads that copy from now on: edit THAT file by "
                    "hand, or delete it to go back to the one in the game folder.", vs.c_str());
            }
        }
    }
    ApplyPending();
    s_statusTick = GetTickCount() ? GetTickCount() : 1;
    s_statusOk = failed == 0;
    s_statusCount = written;
}

void Change(int dir) {
    const Page& pg = kPages[s_page];
    const int i = s_sel[s_page];
    if (i < 0 || i >= pg.count) return;
    const Item& it = pg.items[i];
    int& v = s_val[s_page][i];
    const int before = v;
    switch (it.kind) {
    case K_BOOL:
        v = v ? 0 : 1;
        break;
    case K_INT: {
        // Off the step grid (hand-edited 27 with step 5): go to the next grid
        // value in that direction, 30 or 25, rather than to 32 / 22.
        int nv;
        const int off = v - it.lo;
        if (off < 0) nv = it.lo;
        else if (it.step > 1 && off % it.step != 0) nv = it.lo + (off / it.step + (dir > 0 ? 1 : 0)) * it.step;
        else nv = v + dir * it.step;
        v = (std::max)(it.lo, (std::min)(it.hi, nv));
        break;
    }
    case K_ENUM: {
        int k = OptIndexByValue(it, v);
        k = (k < 0) ? 0 : (k + dir + it.nOpts) % it.nOpts;
        v = it.opts[k].v;
        break;
    }
    case K_ENUM_STR: {
        int k = v;
        k = (k < 0) ? 0 : (k + dir + it.nOpts) % it.nOpts;
        v = k;
        break;
    }
    }
    if (v == before) return;
    s_dirty[s_page][i] = true;
    s_pendingApply |= it.apply;
    if (it.apply & AP_RESTART) s_restartChanged = true;
    s_lastChange = GetTickCount() ? GetTickCount() : 1;
}

void ResetHighlighted() {
    const Page& pg = kPages[s_page];
    const int i = s_sel[s_page];
    if (i < 0 || i >= pg.count) return;
    const Item& it = pg.items[i];
    const int d = (it.kind == K_ENUM_STR) ? OptIndexByKey(it, it.defStr) : it.def;
    if (s_val[s_page][i] == d) return;
    s_val[s_page][i] = d;
    s_dirty[s_page][i] = true;
    s_pendingApply |= it.apply;
    if (it.apply & AP_RESTART) s_restartChanged = true;
    s_lastChange = GetTickCount() ? GetTickCount() : 1;
}

void FormatValue(const Item& it, int p, int i, char* out, size_t cap) {
    const int v = s_val[p][i];
    switch (it.kind) {
    case K_BOOL: sprintf_s(out, cap, "%s", v ? "ON" : "OFF"); break;
    case K_INT:
        if (v == 0 && it.zeroLabel) { sprintf_s(out, cap, "%s", it.zeroLabel); break; }
        if (it.div <= 1) sprintf_s(out, cap, "%d%s", v, it.unit);
        else sprintf_s(out, cap, "%.*f%s", it.decimals, (double)v / it.div, it.unit);
        break;
    case K_ENUM: {
        const int k = OptIndexByValue(it, v);
        if (k >= 0) sprintf_s(out, cap, "%s", it.opts[k].label);
        else sprintf_s(out, cap, "%d", v);
        break;
    }
    case K_ENUM_STR:
        if (v >= 0 && v < it.nOpts) sprintf_s(out, cap, "%s", it.opts[v].label);
        else sprintf_s(out, cap, "%s", s_raw[p][i]);
        break;
    }
}

bool IsDefault(const Item& it, int p, int i) {
    if (it.kind == K_ENUM_STR) return s_val[p][i] == OptIndexByKey(it, it.defStr);
    return s_val[p][i] == it.def;
}

// ===========================================================================
// Drawing
// ===========================================================================

constexpr int kW = 768, kH = 512;
constexpr int kRowY = 84, kRowH = 28, kRows = 12;
std::vector<uint8_t> s_px;
XrSession   s_session = XR_NULL_HANDLE;
XrSpace     s_space = XR_NULL_HANDLE;
ID3D11Device* s_device = nullptr;
ID3D11DeviceContext* s_context = nullptr;
XrSwapchain s_swapchain = XR_NULL_HANDLE;
std::vector<XrSwapchainImageD3D11KHR> s_images;
bool s_ready = false;
bool s_haveReleased = false;
uint64_t s_lastSig = 0;
DWORD s_lastRender = 0;
XrCompositionLayerQuad s_quad{ XR_TYPE_COMPOSITION_LAYER_QUAD };
float s_widthM = 0.90f, s_distM = 1.00f, s_dropM = 0.10f;

// 5x7 font, rows top to bottom, '1' = lit.
const char* Glyph(char c) {
    switch (c) {
    case 'A': return "01110100011000111111100011000110001";
    case 'B': return "11110100011000111110100011000111110";
    case 'C': return "01110100011000010000100001000101110";
    case 'D': return "11110100011000110001100011000111110";
    case 'E': return "11111100001000011110100001000011111";
    case 'F': return "11111100001000011110100001000010000";
    case 'G': return "01110100011000010111100011000101111";
    case 'H': return "10001100011000111111100011000110001";
    case 'I': return "01110001000010000100001000010001110";
    case 'J': return "00111000100001000010000101001001100";
    case 'K': return "10001100101010011000101001001010001";
    case 'L': return "10000100001000010000100001000011111";
    case 'M': return "10001110111010110101100011000110001";
    case 'N': return "10001110011010110011100011000110001";
    case 'O': return "01110100011000110001100011000101110";
    case 'P': return "11110100011000111110100001000010000";
    case 'Q': return "01110100011000110001101011001001101";
    case 'R': return "11110100011000111110101001001010001";
    case 'S': return "01111100001000001110000010000111110";
    case 'T': return "11111001000010000100001000010000100";
    case 'U': return "10001100011000110001100011000101110";
    case 'V': return "10001100011000110001100010101000100";
    case 'W': return "10001100011000110101101011010101010";
    case 'X': return "10001100010101000100010101000110001";
    case 'Y': return "10001100010101000100001000010000100";
    case 'Z': return "11111000010001000100010001000011111";
    case '0': return "01110100011001110101110011000101110";
    case '1': return "00100011000010000100001000010001110";
    case '2': return "01110100010000100010001000100011111";
    case '3': return "11111000100010000010000011000101110";
    case '4': return "00010001100101010010111110001000010";
    case '5': return "11111100001111000001000011000101110";
    case '6': return "00110010001000011110100011000101110";
    case '7': return "11111000010001000100010000100001000";
    case '8': return "01110100011000101110100011000101110";
    case '9': return "01110100011000101111000010001001100";
    case '.': return "00000000000000000000000000110001100";
    case '-': return "00000000000000011111000000000000000";
    case '/': return "00000000010001000100010001000000000";
    case ':': return "00000011000110000000011000110000000";
    case '%': return "11000110010001000100010001001100011";
    case '+': return "00000001000010011111001000010000000";
    case '<': return "00010001000100010000010000010000010";
    case '>': return "01000001000001000001000100010001000";
    case '(': return "00010001000100001000010000010000010";
    case ')': return "01000001000001000010000100010001000";
    case '\'': return "00100001000100000000000000000000000";
    case '*': return "00000001001010101110101010010000000";
    case '!': return "00100001000010000100001000000000100";
    case '?': return "01110100010000100010001000000000100";
    case ',': return "00000000000000000000011000010001000";
    case '=': return "00000000001111100000111110000000000";
    case '[': return "01110010000100001000010000100001110";
    case ']': return "01110000100001000010000100001001110";
    case '_': return "00000000000000000000000000000011111";
    case '&': return "01100100101010001000101011001001101";
    default:  return nullptr;
    }
}

void Px(int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if (x < 0 || y < 0 || x >= kW || y >= kH) return;
    uint8_t* p = &s_px[((size_t)y * kW + x) * 4u];
    p[0] = r; p[1] = g; p[2] = b; p[3] = a;
}
void Fill(int x, int y, int w, int h, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    for (int yy = y; yy < y + h; ++yy) for (int xx = x; xx < x + w; ++xx) Px(xx, yy, r, g, b, a);
}
int TextW(const char* s, int scale) { return (int)std::strlen(s) * 6 * scale; }
int Text(int x, int y, const char* s, int scale, uint8_t r, uint8_t g, uint8_t b) {
    for (int pass = 0; pass < 2; ++pass) {
        int cx = x;
        for (const char* c = s; *c; ++c) {
            const char ch = (*c >= 'a' && *c <= 'z') ? (char)(*c - 32) : *c;
            const char* rows = Glyph(ch);
            if (rows) {
                for (int gy = 0; gy < 7; ++gy) for (int gx = 0; gx < 5; ++gx) {
                    if (rows[gy * 5 + gx] != '1') continue;
                    if (pass == 0) Fill(cx + gx * scale - 1, y + gy * scale - 1, scale + 2, scale + 2, 0, 0, 0, 255);
                    else           Fill(cx + gx * scale, y + gy * scale, scale, scale, r, g, b, 255);
                }
            }
            cx += 6 * scale;
        }
        if (pass == 1) return cx;
    }
    return x;
}
// Word-wrapped text; returns the y after the last line.
int TextWrapped(int x, int y, int maxW, const char* s, int scale, int maxLines, uint8_t r, uint8_t g, uint8_t b) {
    const int perLine = (std::max)(1, maxW / (6 * scale));
    const size_t n = std::strlen(s);
    size_t pos = 0;
    for (int line = 0; line < maxLines && pos < n; ++line) {
        while (pos < n && s[pos] == ' ') ++pos;
        size_t end = (std::min)(n, pos + (size_t)perLine);
        if (end < n) {
            size_t cut = end;
            while (cut > pos && s[cut] != ' ') --cut;
            if (cut > pos) end = cut;
        }
        char buf[160] = {};
        const size_t len = (std::min)(end - pos, sizeof(buf) - 1);
        memcpy(buf, s + pos, len);
        Text(x, y, buf, scale, r, g, b);
        y += 9 * scale;
        pos = end;
    }
    return y;
}

uint64_t Render() {
    std::fill(s_px.begin(), s_px.end(), (uint8_t)0);
    uint64_t sig = 1469598103934665603ull;
    auto mix = [&](uint64_t v) { sig ^= v; sig *= 1099511628211ull; };
    const DWORD now = GetTickCount();

    // Body and frame.
    Fill(0, 0, kW, kH, 8, 12, 10, 228);
    Fill(0, 0, kW, 3, 90, 200, 120, 255); Fill(0, kH - 3, kW, 3, 90, 200, 120, 255);
    Fill(0, 0, 3, kH, 90, 200, 120, 255); Fill(kW - 3, 0, 3, kH, 90, 200, 120, 255);

    Text(16, 12, "VR SETTINGS", 3, 255, 210, 90);
    {
        bool anyDirty = false;
        for (int p = 0; p < kPageCount; ++p) for (int i = 0; i < kPages[p].count; ++i) anyDirty |= s_dirty[p][i];
        const char* st = nullptr;
        uint8_t r = 200, g = 200, b = 200;
        if (anyDirty) { st = "SAVING..."; r = 255; g = 210; b = 90; }
        else if (s_statusTick && now - s_statusTick < 2500) {
            if (s_statusOk) { st = "SAVED"; r = 120; g = 230; b = 140; }
            else { st = "SAVE FAILED - SEE LOG"; r = 255; g = 90; b = 80; }
        }
        if (st) Text(kW - 16 - TextW(st, 2), 18, st, 2, r, g, b);
        mix(anyDirty ? 3 : 4); mix(st ? (uint64_t)st[0] + (s_statusOk ? 0 : 99) : 0);
    }

    // Tabs.
    const int tabW = (kW - 16) / kPageCount;
    for (int p = 0; p < kPageCount; ++p) {
        const int x = 8 + p * tabW;
        const bool on = p == s_page;
        Fill(x + 2, 46, tabW - 4, 28, on ? 40 : 22, on ? 110 : 34, on ? 70 : 28, 245);
        const int tw = TextW(kPages[p].name, 2);
        Text(x + (tabW - tw) / 2, 53, kPages[p].name, 2, on ? 255 : 170, on ? 255 : 190, on ? 255 : 170);
    }
    mix(0x7A0 + (uint64_t)s_page);

    // Rows.
    const Page& pg = kPages[s_page];
    int& scroll = s_scroll[s_page];
    const int sel = s_sel[s_page];
    if (sel < scroll) scroll = sel;
    if (sel >= scroll + kRows) scroll = sel - kRows + 1;
    for (int r = 0; r < kRows; ++r) {
        const int i = scroll + r;
        if (i >= pg.count) break;
        const Item& it = pg.items[i];
        const int y = kRowY + r * kRowH;
        const bool hi = i == sel;
        if (hi) Fill(8, y - 4, kW - 16, kRowH - 2, 40, 110, 70, 235);
        int lx = Text(20, y + 2, it.label, 2, hi ? 255 : 210, hi ? 255 : 210, hi ? 255 : 210);
        if (it.apply & AP_RESTART) Text(lx + 8, y + 6, "RESTART", 1, 255, 170, 60);
        char v[48] = {};
        FormatValue(it, s_page, i, v, sizeof(v));
        const bool def = IsDefault(it, s_page, i);
        const int vx = 470;
        if (hi) {
            Text(vx - 24, y + 2, "<", 2, 255, 210, 90);
            Text(kW - 36, y + 2, ">", 2, 255, 210, 90);
        }
        Text(vx, y + 2, v, 2, def ? 170 : 255, def ? 210 : 230, def ? 255 : 120);
        mix((uint64_t)i * 131 + (uint64_t)(s_val[s_page][i] + 100000) + (hi ? 7 : 0));
    }
    // Scroll hints.
    if (scroll > 0) Text(kW / 2 - 6, kRowY - 12, "-", 1, 200, 200, 200);
    if (scroll + kRows < pg.count) {
        char more[24];
        sprintf_s(more, "%d MORE", pg.count - (scroll + kRows));
        Text(kW / 2 - TextW(more, 1) / 2, kRowY + kRows * kRowH - 6, more, 1, 200, 200, 200);
    }
    mix((uint64_t)scroll);

    // Help for the highlighted option, then the controls.
    Fill(8, 420, kW - 16, 2, 90, 200, 120, 200);
    if (sel >= 0 && sel < pg.count) {
        const Item& it = pg.items[sel];
        char help[200];
        if (it.apply & AP_RESTART) sprintf_s(help, "%s (USED NEXT LAUNCH)", it.help);
        else sprintf_s(help, "%s", it.help);
        TextWrapped(16, 428, kW - 32, help, 2, 3, 200, 220, 255);
    }
    const char* hint = s_restartChanged
        ? "RESTART THE GAME FOR OPTIONS MARKED RESTART.   B: CLOSE"
        : "TRIGGERS: TAB   STICK: PICK / CHANGE   Y: DEFAULT   B: CLOSE";
    Text(16, 490, hint, 2, 255, s_restartChanged ? 170 : 210, s_restartChanged ? 60 : 90);
    mix(s_restartChanged ? 11 : 12);
    return sig;
}

void QuatRotate(const XrQuaternionf& q, const float v[3], float out[3]) {
    const float x = q.x, y = q.y, z = q.z, w = q.w;
    const float tx = 2.0f * (y * v[2] - z * v[1]);
    const float ty = 2.0f * (z * v[0] - x * v[2]);
    const float tz = 2.0f * (x * v[1] - y * v[0]);
    out[0] = v[0] + w * tx + (y * tz - z * ty);
    out[1] = v[1] + w * ty + (z * tx - x * tz);
    out[2] = v[2] + w * tz + (x * ty - y * tx);
}

} // namespace

// The aim laser only builds its swapchain at startup when [aim] laser_enabled
// was on. Turning it on from the panel creates it now.
void VrSettingsEnsureLaser() {
    if (IsAimLaserEnabled() && s_session != XR_NULL_HANDLE && s_device && s_context)
        InitAimLaser(s_session, s_space, s_device, s_context);   // no-op when already ready
}

// ===========================================================================
// Public
// ===========================================================================

bool VrSettingsIsOpen() { return s_open.load(); }

void VrSettingsOpen() {
    if (s_open.load()) return;
    ReadAllFromIni();
    s_restartChanged = false;
    s_pendingApply = 0;
    s_armed = false;
    s_placed = false;
    s_statusTick = 0;
    s_lastChange = 0;
    s_open.store(true);
    DebugLogger::LogFormat("VR Settings: OPEN (values read from %s)%s", IniPath().c_str(),
        s_ready ? "" : " -- WARNING: the panel's swapchain is not up, so it cannot be seen; B or L3 closes it");
}

void VrSettingsClose() {
    if (!s_open.load()) return;
    Flush();
    s_open.store(false);
    DebugLogger::Log(s_restartChanged
        ? "VR Settings: closed (some changes need a game restart)"
        : "VR Settings: closed");
}

void VrSettingsUpdate(const VrSettingsInput& in, DWORD now) {
    if (!s_open.load()) return;
    static bool pa = false, pb = false, py = false, plt = false, prt = false;
    static DWORD lastV = 0, lastH = 0, holdV = 0, holdH = 0;
    static int vdir = 0, hdir = 0;

    const int nv = in.sy > 0.6f ? -1 : (in.sy < -0.6f ? 1 : 0);
    const int nh = (std::fabs(in.sx) > std::fabs(in.sy)) ? (in.sx > 0.6f ? 1 : (in.sx < -0.6f ? -1 : 0)) : 0;

    if (!s_armed) {
        // Wait until whatever opened the panel is let go.
        if (!in.a && !in.b && !in.y && !in.lTrig && !in.rTrig && nv == 0 && nh == 0) s_armed = true;
        pa = in.a; pb = in.b; py = in.y; plt = in.lTrig; prt = in.rTrig; vdir = nv; hdir = nh;
        return;
    }

    if (in.b && !pb) { pb = true; VrSettingsClose(); return; }
    if (in.lTrig && !plt) s_page = (s_page + kPageCount - 1) % kPageCount;
    if (in.rTrig && !prt) s_page = (s_page + 1) % kPageCount;
    const int count = kPages[s_page].count;
    int& sel = s_sel[s_page];
    if (sel >= count) sel = count - 1;

    // Stick: one step on the push, then repeat while held.
    if (nv != 0) {
        if (nv != vdir) { holdV = now; lastV = now; sel = (sel + nv + count) % count; }
        else if (now - holdV >= 380 && now - lastV >= 110) { lastV = now; sel = (std::max)(0, (std::min)(count - 1, sel + nv)); }
    }
    vdir = nv;
    if (nh != 0) {
        if (nh != hdir) { holdH = now; lastH = now; Change(nh); }
        else if (now - holdH >= 420 && now - lastH >= 90) { lastH = now; Change(nh); }
    }
    hdir = nh;
    if (in.a && !pa) Change(1);
    if (in.y && !py) ResetHighlighted();

    pa = in.a; pb = in.b; py = in.y; plt = in.lTrig; prt = in.rTrig;

    // Batch the writes: save once you stop changing things for a moment.
    if (s_lastChange && now - s_lastChange >= 300) {
        s_lastChange = 0;
        Flush();
    }
}

bool InitVrSettingsPanel(XrSession session, XrSpace playSpace, ID3D11Device* device, ID3D11DeviceContext* context) {
    if (s_ready) return true;
    if (session == XR_NULL_HANDLE || playSpace == XR_NULL_HANDLE || !device || !context) return false;
    s_session = session; s_space = playSpace; s_device = device; s_context = context;
    {
        const std::string ini = IniPath();
        s_widthM = (float)GetPrivateProfileIntA("vr_settings", "panel_width_cm", 90, ini.c_str()) / 100.0f;
        s_distM = (float)GetPrivateProfileIntA("vr_settings", "panel_distance_cm", 100, ini.c_str()) / 100.0f;
        s_dropM = (float)GetPrivateProfileIntA("vr_settings", "panel_below_eyes_cm", 10, ini.c_str()) / 100.0f;
        s_widthM = (std::max)(0.3f, (std::min)(3.0f, s_widthM));
        s_distM = (std::max)(0.4f, (std::min)(4.0f, s_distM));
        s_dropM = (std::max)(-1.0f, (std::min)(1.0f, s_dropM));
    }
    XrSwapchainCreateInfo info{ XR_TYPE_SWAPCHAIN_CREATE_INFO };
    info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    info.format = (int64_t)DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;   // same as the wrist HUD: bytes are sRGB
    info.sampleCount = 1; info.width = kW; info.height = kH;
    info.faceCount = 1; info.arraySize = 1; info.mipCount = 1;
    XrResult xr = xrCreateSwapchain(session, &info, &s_swapchain);
    if (xr != XR_SUCCESS) {
        info.format = (int64_t)DXGI_FORMAT_R8G8B8A8_UNORM;
        xr = xrCreateSwapchain(session, &info, &s_swapchain);
    }
    if (xr != XR_SUCCESS) {
        DebugLogger::LogFormat("VR Settings: xrCreateSwapchain failed (%d) -- the in-headset settings panel is "
            "unavailable this run (the ini still works)", (int)xr);
        s_swapchain = XR_NULL_HANDLE;
        return false;
    }
    uint32_t count = 0;
    xrEnumerateSwapchainImages(s_swapchain, 0, &count, nullptr);
    s_images.assign(count, { XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR });
    if (count == 0 || xrEnumerateSwapchainImages(s_swapchain, count, &count,
            reinterpret_cast<XrSwapchainImageBaseHeader*>(s_images.data())) != XR_SUCCESS) {
        DebugLogger::Log("VR Settings: could not enumerate swapchain images -- panel unavailable");
        ShutdownVrSettingsPanel();
        return false;
    }
    s_px.assign((size_t)kW * kH * 4u, 0);
    s_ready = true;
    s_haveReleased = false;
    DebugLogger::LogFormat("VR Settings: panel ready (%dx%d, %.2f m wide at %.2f m). Hold L3 ~1 s to open it.",
        kW, kH, s_widthM, s_distM);
    return true;
}

void ShutdownVrSettingsPanel() {
    if (s_swapchain != XR_NULL_HANDLE) xrDestroySwapchain(s_swapchain);
    s_swapchain = XR_NULL_HANDLE;
    s_images.clear();
    s_ready = false; s_haveReleased = false; s_placed = false;
    s_session = XR_NULL_HANDLE; s_space = XR_NULL_HANDLE; s_device = nullptr; s_context = nullptr;
}

void BuildVrSettingsLayers(const XrPosef& headPose, std::vector<const XrCompositionLayerBaseHeader*>& outLayers) {
    if (!s_open.load() || !s_ready) return;

    if (!s_placed) {
        // In front of where you face, level (yaw only), a little below eye height.
        const float fz[3] = { 0, 0, -1 };
        float f[3];
        QuatRotate(headPose.orientation, fz, f);
        const float yaw = std::atan2(-f[0], -f[2]);
        s_pose.position.x = headPose.position.x - std::sin(yaw) * s_distM;
        s_pose.position.y = headPose.position.y - s_dropM;
        s_pose.position.z = headPose.position.z - std::cos(yaw) * s_distM;
        s_pose.orientation.x = 0.0f; s_pose.orientation.z = 0.0f;
        s_pose.orientation.y = std::sin(yaw * 0.5f);
        s_pose.orientation.w = std::cos(yaw * 0.5f);
        s_placed = true;
    }

    const DWORD now = GetTickCount();
    if (now - s_lastRender >= 33 || !s_haveReleased) {
        s_lastRender = now;
        const uint64_t sig = Render();
        if (sig != s_lastSig || !s_haveReleased) {
            uint32_t idx = 0;
            XrSwapchainImageAcquireInfo acq{ XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
            if (xrAcquireSwapchainImage(s_swapchain, &acq, &idx) == XR_SUCCESS) {
                XrSwapchainImageWaitInfo wait{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
                wait.timeout = 50000000;   // 50 ms; never hang the frame thread on a menu
                bool ok = false;
                if (xrWaitSwapchainImage(s_swapchain, &wait) == XR_SUCCESS && idx < s_images.size() && s_images[idx].texture) {
                    s_context->UpdateSubresource(s_images[idx].texture, 0, nullptr, s_px.data(), (UINT)(kW * 4), 0);
                    ok = true;
                }
                XrSwapchainImageReleaseInfo rel{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
                if (xrReleaseSwapchainImage(s_swapchain, &rel) == XR_SUCCESS && ok) { s_haveReleased = true; s_lastSig = sig; }
            }
        }
    }
    if (!s_haveReleased) return;

    XrCompositionLayerQuad& q = s_quad;
    q = XrCompositionLayerQuad{ XR_TYPE_COMPOSITION_LAYER_QUAD };
    q.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT | XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
    q.space = s_space;
    q.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    q.subImage.swapchain = s_swapchain;
    q.subImage.imageRect.offset = { 0, 0 };
    q.subImage.imageRect.extent = { kW, kH };
    q.subImage.imageArrayIndex = 0;
    q.pose = s_pose;
    q.size.width = s_widthM;
    q.size.height = s_widthM * (float)kH / (float)kW;
    outLayers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&q));
}

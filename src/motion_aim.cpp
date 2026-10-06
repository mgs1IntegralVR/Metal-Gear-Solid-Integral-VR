#include <windows.h>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <atomic>
#include <string>
#include <openxr/openxr.h>

#include "../include/motion_aim.h"
#include "../include/crash_report.h"
#include "../include/camera_write_hook.h"
#include "../include/debug_logging.h"

// See motion_aim.h for the design. Everything here is self-contained: its own
// config, its own guarded reads, its own call-site patch. The rest of the mod
// only feeds it (head/aim pose, eye image placement, trigger state).

// ---------------------------------------------------------------------------
// Addresses (RVAs; mgsi.exe has no .reloc and loads at 0x400000)
// ---------------------------------------------------------------------------
namespace {

constexpr uintptr_t kAllocRva     = 0x0A30C;   // 0x40A30C, GV_NewActor(class, size)
constexpr uintptr_t kViewFromRva  = 0x593F60;  // layer 3 FROM, int16 x,y,z   (99%)
constexpr uintptr_t kViewToRva    = 0x593F68;  // layer 3 TO,   int16 x,y,z   (99%)
constexpr uintptr_t kViewFovRva   = 0x593F7C;  // clip distance in use        (95%)
constexpr uintptr_t kClipDistRva  = 0x594000;  // clip distance source        (99%)

// Okajima\bullet.c constructor sites, from claude_task_roster.md (VA - 0x400000).
// The roster's address lands on or next to the allocator call; we search a
// small window for the exact byte pattern rather than trusting it to the byte.
struct BulletSite {
    const char* label;
    uintptr_t   rosterRva;
    int         kind;               // 0 = MATRIX arg (bullets, missiles), 1 = thrown object (NewTenage)
    int         extraWindowMs;      // weapons that fire on trigger RELEASE get a longer window
    uint8_t     push[7];            // push <size> / push <class> right before the call
    uint8_t*    call = nullptr;     // the E8 byte
    int32_t     origRel = 0;
    uintptr_t   retAddr = 0;        // call + 5, what the thunk sees on the stack
    bool        installed = false;
    unsigned    hits = 0;
};
BulletSite g_sites[] = {
    { "bullet.c #1", 0x1FDA0C, 0, 0, { 0x68, 0x70, 0x01, 0x00, 0x00, 0x6A, 0x05 } },
    { "bullet.c #2", 0x1FE57C, 0, 0, { 0x68, 0x70, 0x01, 0x00, 0x00, 0x6A, 0x05 } },
    { "bullet.c #3", 0x1FE6AB, 0, 0, { 0x68, 0x70, 0x01, 0x00, 0x00, 0x6A, 0x05 } },
    // Nikita: NewRMissile(MATRIX*, side) -- rcm.c builds the matrix from Snake's
    // heading and hand joint, not from the gun model. Verified 2026-09-23:
    // push 0x328 / push 6 / call 0x40A30C at 0x43DB32.
    { "rmissile.c (Nikita)", 0x03DB39, 0, 600, { 0x68, 0x28, 0x03, 0x00, 0x00, 0x6A, 0x06 } },
    // Stinger: NewAMissile(MATRIX*, side) -- aam.c builds it from the camera.
    // push 0x160 / push 6 / call 0x40A30C at 0x5A09D4.
    { "amissile.c (Stinger)", 0x1A09DB, 0, 600, { 0x68, 0x60, 0x01, 0x00, 0x00, 0x6A, 0x06 } },
    // NewTenage (0x5A1135): the player's grenade / stun / chaff throw. Verified
    // 2026-09-23 in mgsi.exe: push 0x124 / push 5 / call 0x40A30C at 0x5A1144,
    // before it reads pos ([ebp+8]) and step ([ebp+0xC]). Its only player
    // caller is weapon\grenade.c at 0x64011C.
    { "tenage.c (throw)", 0x1A114B, 1, 600, { 0x68, 0x24, 0x01, 0x00, 0x00, 0x6A, 0x05 } },
};
constexpr int kSiteCount = (int)(sizeof(g_sites) / sizeof(g_sites[0]));


constexpr float kPi = 3.14159265358979323846f;

// --- config -----------------------------------------------------------------
bool  g_cfgEnabled = true;
bool  g_cfgMuzzleFromHand = true;
float g_cfgMaxReachM = 0.60f;
float g_cfgPlayerRadius = 2500.0f;   // game units, eye -> muzzle
int   g_cfgFireWindowMs = 400;
int   g_cfgRightSign = 0;            // 0 auto, +1/-1 forced
int   g_cfgUpSign = 0;
// Decomp (okajima/bullet.c GetResources): bullet_step.vy = -speed, rotated by
// the matrix -> the bullet flies along the matrix's LOCAL -Y, i.e. -column 1.
// Pinned by default; -1 re-enables the shot-based auto-calibration.
int   g_cfgForwardVector = 1;        // -1 auto, 0..2 column, 3..5 row
int   g_cfgForwardSign = -1;         // 0 auto
bool  g_cfgDisplayMapping = true;
int   g_cfgLogShots = 40;
int   g_cfgLogMisses = 6;
float g_cfgUnitsPerMetre = 2048.0f;  // [camera_hook] position_scale

// --- gun in hand ([motion_aim] gun_*) --------------------------------------
bool  g_cfgGunInHand = true;
bool  g_cfgGunAllFirearms = true;    // PSG1 / Nikita / Stinger too
bool  g_cfgStingerHeadAim = true;    // [motion_aim] stinger_head_aim
bool  g_cfgThrowAim = true;          // grenades/stun/chaff leave your hand along the laser
bool  g_cfgGunThrowables = true;     // show the grenade/canister in your hand
float g_cfgThrowSpeed = 1.0f;
int   g_cfgHandPredictMs = -1;       // extrapolate the hand this far ahead; -1 = measured (auto)
int   g_cfgHandPredictPercent = 80;  // how much of that horizon to actually predict
// Hand smoothing (2026-10-04): a One Euro filter on the controller pose that
// the HELD gun and both hands are drawn from. The aim laser keeps the raw pose.
bool  g_cfgHandFilter = true;
float g_cfgHandFilterMinCutHz = 1.5f;   // smoothing when the hand is still (lower = smoother, more lag)
float g_cfgHandFilterPosBeta = 12.0f;   // Hz added per m/s of hand speed (higher = less lag when moving)
float g_cfgHandFilterRotBeta = 2.0f;    // Hz added per rad/s of turning
float g_cfgHandFilterDCutHz = 5.0f;     // smoothing of the velocity used for prediction
// Stillness gate (2026-10-04, round 2): prediction (and the filter-lag
// compensation) fades out as the hand slows, so a hand at rest is drawn at its
// smoothed position with NO extrapolation. Below the "still" speed nothing is
// predicted; above the "moving" speed all of it is.
float g_cfgStillMps = 0.03f, g_cfgMovingMps = 0.15f;      // hand speed, m/s
float g_cfgStillRads = 0.15f, g_cfgMovingRads = 0.80f;    // hand turn rate, rad/s
bool  g_cfgHandJitterLog = true;                          // HandJitter lines
// Measured on the XR thread (vr_injection.cpp): how old a game image is when
// it first reaches your eyes, the game frame period and the display period.
std::atomic<float> g_imgAgeMs{ 0.0f };
std::atomic<float> g_imgPeriodMs{ 33.3f };
std::atomic<float> g_dispPeriodMs{ 11.1f };
bool  g_cfgThrowSwing = true;        // a real throwing swing sets the throw's direction and speed
float g_cfgSwingMinMps = 1.2f;       // slower than this = an aimed throw instead
float g_cfgSwingGain = 1.0f;
bool  g_cfgModelProbe = true;
float g_cfgGunRollDeg = 0.0f;
float g_cfgGunOffset[3] = { 0, 0, 0 };  // gun-local game units: moves the model in your hand
int   g_cfgGunEyeCenterSign = 1;     // undo the per-eye stereo nudge (see BuildHandMatrix)
float g_cfgHalfIpdM = 0.032f;
bool  g_cfgStereoSwap = false;

uintptr_t g_base = 0;
bool g_anyInstalled = false;
bool g_dispatchInstalled = false;
uint8_t* g_dispatchSite = nullptr;
uint8_t  g_dispatchOrig[7] = {};
std::atomic<LONGLONG> g_gunActiveQpc{ 0 };
std::atomic<bool> g_heldFiresFromModel{ false };

// --- published from the XR thread ------------------------------------------
SRWLOCK g_lock = SRWLOCK_INIT;
struct AimSnap {
    bool     valid = false;
    float    dir[3] = { 0, 0, -1 };  // controller forward, HEAD-LOCAL (x right, y up, z back)
    float    pos[3] = { 0, 0, 0 };   // controller origin minus head, HEAD-LOCAL, metres
    float    up[3] = { 0, 1, 0 };    // controller up, HEAD-LOCAL
    float    vel[3] = { 0, 0, 0 };   // controller linear velocity, HEAD-LOCAL, m/s (smoothed)
    LONGLONG qpc = 0;
    // previous sample, for extrapolation
    bool     havePrev = false;
    float    pdir[3] = { 0, 0, -1 }, ppos[3] = { 0, 0, 0 }, pup[3] = { 0, 1, 0 };
    LONGLONG pqpc = 0;
    // The same controller in the PLAY SPACE (2026-09-22). The head-local copy
    // above is relative to the headset pose of the XR frame that sampled it;
    // the game camera it gets mapped onto was steered from an EARLIER head
    // pose. Any head movement in between moved the gun and the bullets by
    // exactly that much -- turn your head and the gun swam off your hand, tilt
    // it and the gun rolled. Keeping play-space values lets the consumer
    // re-express the hand against the head pose the game really drew with.
    bool     haveWorld = false;
    float    wdir[3] = { 0, 0, -1 }, wpos[3] = { 0, 0, 0 }, wup[3] = { 0, 1, 0 }, wvel[3] = { 0, 0, 0 };
    float    pwdir[3] = { 0, 0, -1 }, pwpos[3] = { 0, 0, 0 }, pwup[3] = { 0, 1, 0 };
    // FILTERED play-space pose (One Euro), its smoothed rates of change, and
    // the lag the filter itself introduced (s), for the held gun and hands.
    bool     haveFilt = false;
    float    fwpos[3] = { 0, 0, 0 }, fwdir[3] = { 0, 0, -1 }, fwup[3] = { 0, 1, 0 };
    float    vpos[3] = { 0, 0, 0 }, vdir[3] = { 0, 0, 0 }, vup[3] = { 0, 0, 0 };
    float    lagPos = 0.0f, lagRot = 0.0f;
    // head pose at publish, to place the hand when no render record exists
    float    hpos[3] = { 0, 0, 0 };
    XrQuaternionf hq{ 0, 0, 0, 1 };
} g_snap;
bool g_cfgRenderHeadFrame = true;    // [motion_aim] hand_relative_to_render_pose
// --- left controller snapshot (published from the XR thread) ----------------
AimSnap  g_snapL;
struct VelTrackT { bool have = false; float pos[3]{}; LONGLONG qpc = 0; float vel[3]{}; };
VelTrackT g_velTrackL;
// --- left-handed mode (2026-10-05) ------------------------------------------
// g_snap (the gun, bullets, laser) then comes from the LEFT controller's aim
// pose, so Snake's RIGHT hand needs its own source: the right controller's aim
// pose, published here. With it, both hand models keep exactly the controller
// pose + calibration they have in right-handed mode -- left mesh on the left
// grip, right mesh on the right aim -- and only the gun changes hands.
bool      g_leftHanded = false;
AimSnap   g_snapRH;
VelTrackT g_velTrackRH;
XrPosef  g_lastHead{};
bool     g_haveHead = false;
// play-space velocity tracking (XR thread only)
struct { bool have = false; float pos[3]{}; LONGLONG qpc = 0; float vel[3]{}; } g_velTrack;
struct EyeImage {
    bool     valid = false;
    float    dL = -1, dR = 1, dU = 1, dD = -1;  // tangents of the picture's edges as displayed
    float    fx = 1, fy = 1;
    LONGLONG qpc = 0;
} g_eyeImg[2];

std::atomic<LONGLONG> g_lastFireQpc{ 0 };
// Rumble requests for vr_input.cpp (XR thread), which owns the haptics.
// Shot: 0 none, 1 bullet, 2 missile, 3 throw (the strongest wins until taken).
// Knock: bit 0 = right hand, bit 1 = left hand.
std::atomic<int> g_shotRumble{ 0 };
std::atomic<int> g_knockRumble{ 0 };

// --- calibration -------------------------------------------------------------
std::atomic<int> g_rightSign{ 0 };   // resolved sign, 0 = not yet
std::atomic<int> g_upSign{ 0 };
int  g_fwdIndex = -1;                // game thread only
int  g_fwdSign = 0;
int  g_fwdVoteIdx = -1, g_fwdVoteSign = 0, g_fwdVoteStreak = 0;

// XR-thread calibration scratch
struct CalRef { bool have = false; XrQuaternionf q{}; float F[3]{}; LONGLONG qpc = 0; } g_cal;
int g_votesX = 0, g_countX = 0, g_votesY = 0, g_countY = 0;

int g_shotLogs = 0, g_missLogs = 0;
LONGLONG g_qpcFreq = 0;

// ---------------------------------------------------------------------------
// small helpers
// ---------------------------------------------------------------------------
LONGLONG NowQpc() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }
double MsSince(LONGLONG then) {
    if (then == 0 || g_qpcFreq == 0) return 1e9;
    return (double)(NowQpc() - then) * 1000.0 / (double)g_qpcFreq;
}

// SEH-only functions: nothing with a destructor may live in these.
bool SafeRead(void* dst, const void* src, size_t n) {
    __try { memcpy(dst, src, n); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool SafeWrite(void* dst, const void* src, size_t n) {
    __try { memcpy(dst, src, n); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

struct V3 { float x, y, z; };
inline V3 Add(V3 a, V3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
inline V3 Sub(V3 a, V3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
inline V3 Mul(V3 a, float s) { return { a.x * s, a.y * s, a.z * s }; }
inline float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V3 Cross(V3 a, V3 b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
inline float Len(V3 a) { return std::sqrt(Dot(a, a)); }
inline bool Norm(V3& a) { const float l = Len(a); if (l < 1e-6f) return false; a = Mul(a, 1.0f / l); return true; }

V3 QRot(const XrQuaternionf& q, V3 v) {
    const V3 u{ q.x, q.y, q.z };
    const float s = q.w;
    return Add(Add(Mul(u, 2.0f * Dot(u, v)), Mul(v, s * s - Dot(u, u))), Mul(Cross(u, v), 2.0f * s));
}
XrQuaternionf QInv(XrQuaternionf q) { q.x = -q.x; q.y = -q.y; q.z = -q.z; return q; }

// ---------------------------------------------------------------------------
// ONE EURO FILTER (Casiez et al. 2012), per 3-vector. Low cutoff when the hand
// is still (kills tracking tremor), cutoff rising with speed (no lag when you
// move). Its smoothed derivative is what the gun is predicted with -- not the
// difference of the last two raw samples, which multiplied tracking noise by
// the prediction horizon over one XR frame (~5x) and was the gun's jitter.
// XR thread only.
// ---------------------------------------------------------------------------
struct Euro3 {
    bool  have = false;
    V3    x{ 0, 0, 0 };     // filtered value
    V3    dx{ 0, 0, 0 };    // filtered rate of change, per second
    float lag = 0.0f;       // 1/(2 pi fc) of the last step: the ramp lag it introduced, s
};
inline float EuroAlpha(float cutHz, float dt) {
    const float tau = 1.0f / (2.0f * kPi * cutHz);
    return 1.0f / (1.0f + tau / dt);
}
V3 EuroStep(Euro3& f, V3 raw, float dt, float minCut, float beta, float dCut) {
    if (!f.have || !(dt > 0.0005f) || dt > 0.2f) {
        f.have = true; f.x = raw; f.dx = V3{ 0, 0, 0 }; f.lag = 0.0f;
        return raw;
    }
    const V3 rawDx = Mul(Sub(raw, f.x), 1.0f / dt);
    f.dx = Add(f.dx, Mul(Sub(rawDx, f.dx), EuroAlpha(dCut, dt)));
    const float cut = minCut + beta * Len(f.dx);
    f.x = Add(f.x, Mul(Sub(raw, f.x), EuroAlpha(cut, dt)));
    f.lag = 1.0f / (2.0f * kPi * cut);
    return f.x;
}
struct HandEuro { Euro3 pos, dir, up; LONGLONG qpc = 0; };
HandEuro g_euroR, g_euroL, g_euroRH;

// Runs the filter on one play-space controller sample and stores the result in
// the snapshot (caller holds g_lock exclusively).
void FilterIntoSnap(HandEuro& h, AimSnap& s, V3 pW, V3 dW, V3 uW, const XrPosef& head, LONGLONG nowQ) {
    const float dt = h.qpc ? (float)((double)(nowQ - h.qpc) / (double)g_qpcFreq) : 0.0f;
    h.qpc = nowQ;
    const float mc = g_cfgHandFilterMinCutHz, dc = g_cfgHandFilterDCutHz;
    const V3 fp = EuroStep(h.pos, pW, dt, mc, g_cfgHandFilterPosBeta, dc);
    V3 fd = EuroStep(h.dir, dW, dt, mc, g_cfgHandFilterRotBeta, dc);
    V3 fu = EuroStep(h.up, uW, dt, mc, g_cfgHandFilterRotBeta, dc);
    if (!Norm(fd)) fd = dW;
    if (!Norm(fu)) fu = uW;
    auto put = [](float* d, V3 v) { d[0] = v.x; d[1] = v.y; d[2] = v.z; };
    put(s.fwpos, fp); put(s.fwdir, fd); put(s.fwup, fu);
    put(s.vpos, h.pos.dx); put(s.vdir, h.dir.dx); put(s.vup, h.up.dx);
    s.lagPos = h.pos.lag;
    s.lagRot = h.dir.lag;
    s.hpos[0] = head.position.x; s.hpos[1] = head.position.y; s.hpos[2] = head.position.z;
    s.hq = head.orientation;
    s.haveFilt = true;
}

// yaw(Y) * pitch(X) * roll(Z) -- the same convention as the head angles.
XrQuaternionf QFromYpr(float yaw, float pitch, float roll) {
    const float cy = std::cos(yaw * 0.5f), sy = std::sin(yaw * 0.5f);
    const float cp = std::cos(pitch * 0.5f), sp = std::sin(pitch * 0.5f);
    const float cr = std::cos(roll * 0.5f), sr = std::sin(roll * 0.5f);
    const float ax = cy * sp, ay = sy * cp, az = -sy * sp, aw = cy * cp;
    XrQuaternionf q;
    q.x = ax * cr + ay * sr;
    q.y = ay * cr - ax * sr;
    q.z = aw * sr + az * cr;
    q.w = aw * cr - az * sr;
    return q;
}

// Re-express a snapshot's hand relative to the head pose the game ACTUALLY
// drew its current camera from (the rotation hook's frame view record), rather
// than the headset pose of whichever XR frame sampled the controller. With the
// record's roll being the camera's (level) roll, tilting your head no longer
// rolls the gun either. Leaves the snapshot untouched if there is no fresh
// record (cutscenes, menus, record switched off) -- the old behaviour.
bool ToRenderHeadFrame(AimSnap& s) {
    if (!g_cfgRenderHeadFrame || !s.valid || !s.haveWorld) return false;
    FrameViewRecord r;
    if (!GetLatestFrameViewRecord(&r, 150)) return false;
    const XrQuaternionf inv = QInv(QFromYpr(r.yawEq, r.pitchEq, r.rollEq));
    const V3 c{ (r.eye[0].px + r.eye[1].px) * 0.5f, (r.eye[0].py + r.eye[1].py) * 0.5f,
                (r.eye[0].pz + r.eye[1].pz) * 0.5f };
    auto put = [](float* d, V3 v) { d[0] = v.x; d[1] = v.y; d[2] = v.z; };
    auto get = [](const float* a) { return V3{ a[0], a[1], a[2] }; };
    put(s.dir, QRot(inv, get(s.wdir)));
    put(s.up,  QRot(inv, get(s.wup)));
    put(s.pos, QRot(inv, Sub(get(s.wpos), c)));
    put(s.vel, QRot(inv, get(s.wvel)));
    if (s.havePrev) {
        put(s.pdir, QRot(inv, get(s.pwdir)));
        put(s.pup,  QRot(inv, get(s.pwup)));
        put(s.ppos, QRot(inv, Sub(get(s.pwpos), c)));
    }
    return true;
}

bool ReadV16(uintptr_t rva, V3* out) {
    int16_t v[3];
    if (!g_base || !SafeRead(v, (const void*)(g_base + rva), sizeof(v))) return false;
    *out = { (float)v[0], (float)v[1], (float)v[2] };
    return true;
}

// The camera basis. F is where the game is looking; R0/U0 are built against
// world Y without knowing whether Y is up or down or which way the frame is
// handed -- the calibrated signs decide which way "right" and "up" really are.
bool CameraBasis(V3 from, V3 to, V3* F, V3* R0, V3* U0) {
    V3 f = Sub(to, from);
    if (!Norm(f)) return false;
    V3 r = Cross(f, V3{ 0, 1, 0 });
    if (!Norm(r)) return false;           // looking straight along Y
    *F = f; *R0 = r; *U0 = Cross(r, f);
    return true;
}

int ReadClipDistance() {
    int16_t c = 0;
    if (g_base && SafeRead(&c, (const void*)(g_base + kViewFovRva), 2) && c >= 60 && c <= 4000) return c;
    if (g_base && SafeRead(&c, (const void*)(g_base + kClipDistRva), 2) && c >= 60 && c <= 4000) return c;
    return 320;
}

bool GameplayGate() {
    return IsFpvActive() && IsVrViewModeActive() && !IsCutsceneVrActive();
}

std::string IniPath() {
    char exe[MAX_PATH] = { 0 };
    GetModuleFileNameA(NULL, exe, MAX_PATH);
    std::string d(exe);
    const auto s = d.find_last_of("\\/");
    if (s != std::string::npos) d = d.substr(0, s);
    return d + "\\mgs1_vr_config.ini";
}

// ---------------------------------------------------------------------------
// The game's MATRIX: short m[3][3], 2 bytes pad, long t[3] at +0x14.
// ---------------------------------------------------------------------------
#pragma pack(push, 1)
struct GameMatrix { int16_t m[3][3]; int16_t pad; int32_t t[3]; };
#pragma pack(pop)
static_assert(sizeof(GameMatrix) == 32, "PSX MATRIX layout");

// k 0..2 = column k, 3..5 = row k-3. Unnormalised (4096 scale).
V3 MatVec(const GameMatrix& M, int k) {
    if (k < 3) return { (float)M.m[0][k], (float)M.m[1][k], (float)M.m[2][k] };
    const int r = k - 3;
    return { (float)M.m[r][0], (float)M.m[r][1], (float)M.m[r][2] };
}
void SetMatVec(GameMatrix& M, int k, V3 v) {
    auto c16 = [](float f) -> int16_t {
        long r = std::lround(f);
        if (r > 32767) r = 32767;
        if (r < -32768) r = -32768;
        return (int16_t)r;
    };
    if (k < 3) { M.m[0][k] = c16(v.x); M.m[1][k] = c16(v.y); M.m[2][k] = c16(v.z); }
    else { const int r = k - 3; M.m[r][0] = c16(v.x); M.m[r][1] = c16(v.y); M.m[r][2] = c16(v.z); }
}

bool LooksLikeRotation(const GameMatrix& M) {
    for (int i = 0; i < 3; ++i) {
        const float l = Len(MatVec(M, 3 + i)) / 4096.0f;
        if (l < 0.88f || l > 1.12f) return false;
    }
    for (int i = 0; i < 3; ++i)
        for (int j = i + 1; j < 3; ++j)
            if (std::fabs(Dot(MatVec(M, 3 + i), MatVec(M, 3 + j))) / (4096.0f * 4096.0f) > 0.12f)
                return false;
    return true;
}

// Rotate v by the minimal rotation taking unit O onto unit D.
V3 RotateMinimal(V3 v, V3 O, V3 D, V3 fallbackAxis) {
    const V3 k = Cross(O, D);
    const float s2 = Dot(k, k);
    const float c = Dot(O, D);
    if (s2 < 1e-10f) {
        if (c > 0.0f) return v;
        // 180 degrees about an axis perpendicular to O
        V3 a = fallbackAxis; Norm(a);
        return Sub(Mul(a, 2.0f * Dot(a, v)), v);
    }
    return Add(Add(Mul(v, c), Cross(k, v)), Mul(k, Dot(k, v) * (1.0f - c) / s2));
}

// Displayed tangent -> game tangent, per axis, averaged over fresh eyes.
void DisplayMapping(int clip, float* ax, float* bx, float* ay, float* by) {
    *ax = 1; *bx = 0; *ay = 1; *by = 0;
    if (!g_cfgDisplayMapping) return;
    float sax = 0, sbx = 0, say = 0, sby = 0; int n = 0;
    AcquireSRWLockShared(&g_lock);
    for (int e = 0; e < 2; ++e) {
        const EyeImage& E = g_eyeImg[e];
        if (!E.valid || MsSince(E.qpc) > 1000.0) continue;
        const float gxh = (160.0f / (float)clip) * E.fx;
        const float gyh = (120.0f / (float)clip) * E.fy;
        const float wx = E.dR - E.dL, wy = E.dU - E.dD;
        if (wx < 1e-4f || wy < 1e-4f) continue;
        const float a1 = 2.0f * gxh / wx, a2 = 2.0f * gyh / wy;
        sax += a1; sbx += -gxh - a1 * E.dL;
        say += a2; sby += -gyh - a2 * E.dD;
        ++n;
    }
    ReleaseSRWLockShared(&g_lock);
    if (n == 0) return;
    *ax = sax / n; *bx = sbx / n; *ay = say / n; *by = sby / n;
}

// Head-local (metres or unit direction) -> game-space offset from the eye.
// Linear, so lines map to lines: the bullet's path lands on the picture
// exactly where the laser does, for whatever frustum the image is shown at.
V3 HeadToGame(V3 h, V3 F, V3 R, V3 U, float ax, float bx, float ay, float by) {
    const float depth = -h.z;
    const float ex = ax * h.x + bx * depth;
    const float ey = ay * h.y + by * depth;
    return Add(Add(Mul(R, ex), Mul(U, ey)), Mul(F, depth));
}

bool BuildHandMatrix(GameMatrix* M, float* reachM);   // gun section, below
static V3 HeadCentreFromEye(V3 from, V3 R);            // gun section, below
extern int g_throwLogs;

const char* VecName(int k) {
    static const char* n[6] = { "col0", "col1", "col2", "row0", "row1", "row2" };
    return (k >= 0 && k < 6) ? n[k] : "?";
}

} // namespace

// ===========================================================================
// XR thread
// ===========================================================================
void MotionAimPublishXrFrame(const XrPosef& head, const XrPosef& aim, bool aimValid) {
    if (!g_anyInstalled) return;
    AcquireSRWLockExclusive(&g_lock);
    g_lastHead = head; g_haveHead = true;
    ReleaseSRWLockExclusive(&g_lock);

    const XrQuaternionf inv = QInv(head.orientation);
    if (aimValid) {
        const V3 dW = QRot(aim.orientation, V3{ 0, 0, -1 });
        const V3 pW = { aim.position.x - head.position.x,
                        aim.position.y - head.position.y,
                        aim.position.z - head.position.z };
        const V3 dL = QRot(inv, dW), pL = QRot(inv, pW);
        const V3 uL = QRot(inv, QRot(aim.orientation, V3{ 0, 1, 0 }));
        // Hand velocity in the play space, lightly smoothed (~30 ms), then
        // expressed in the head frame like everything else.
        const LONGLONG nowQ = NowQpc();
        if (g_velTrack.have) {
            const float dt = (float)((double)(nowQ - g_velTrack.qpc) / (double)g_qpcFreq);
            if (dt > 0.002f && dt < 0.2f) {
                const float a = dt / (0.03f + dt);
                const float raw[3] = { (aim.position.x - g_velTrack.pos[0]) / dt,
                                       (aim.position.y - g_velTrack.pos[1]) / dt,
                                       (aim.position.z - g_velTrack.pos[2]) / dt };
                for (int i = 0; i < 3; ++i) g_velTrack.vel[i] += (raw[i] - g_velTrack.vel[i]) * a;
            }
        }
        g_velTrack.have = true; g_velTrack.qpc = nowQ;
        g_velTrack.pos[0] = aim.position.x; g_velTrack.pos[1] = aim.position.y; g_velTrack.pos[2] = aim.position.z;
        const V3 vL = QRot(inv, V3{ g_velTrack.vel[0], g_velTrack.vel[1], g_velTrack.vel[2] });
        const V3 uW = QRot(aim.orientation, V3{ 0, 1, 0 });
        AcquireSRWLockExclusive(&g_lock);
        if (g_snap.valid) {
            g_snap.havePrev = true;
            for (int i = 0; i < 3; ++i) { g_snap.pdir[i] = g_snap.dir[i]; g_snap.ppos[i] = g_snap.pos[i]; g_snap.pup[i] = g_snap.up[i]; }
            for (int i = 0; i < 3; ++i) { g_snap.pwdir[i] = g_snap.wdir[i]; g_snap.pwpos[i] = g_snap.wpos[i]; g_snap.pwup[i] = g_snap.wup[i]; }
            g_snap.pqpc = g_snap.qpc;
        }
        g_snap.haveWorld = true;
        g_snap.wdir[0] = dW.x; g_snap.wdir[1] = dW.y; g_snap.wdir[2] = dW.z;
        g_snap.wup[0] = uW.x;  g_snap.wup[1] = uW.y;  g_snap.wup[2] = uW.z;
        g_snap.wpos[0] = aim.position.x; g_snap.wpos[1] = aim.position.y; g_snap.wpos[2] = aim.position.z;
        g_snap.wvel[0] = g_velTrack.vel[0]; g_snap.wvel[1] = g_velTrack.vel[1]; g_snap.wvel[2] = g_velTrack.vel[2];
        g_snap.vel[0] = vL.x; g_snap.vel[1] = vL.y; g_snap.vel[2] = vL.z;
        g_snap.valid = true;
        g_snap.up[0] = uL.x; g_snap.up[1] = uL.y; g_snap.up[2] = uL.z;
        g_snap.dir[0] = dL.x; g_snap.dir[1] = dL.y; g_snap.dir[2] = dL.z;
        g_snap.pos[0] = pL.x; g_snap.pos[1] = pL.y; g_snap.pos[2] = pL.z;
        g_snap.qpc = NowQpc();
        FilterIntoSnap(g_euroR, g_snap, V3{ aim.position.x, aim.position.y, aim.position.z }, dW, uW, head, nowQ);
        ReleaseSRWLockExclusive(&g_lock);
    }
    else {
        AcquireSRWLockExclusive(&g_lock);
        g_snap.valid = false;
        g_snap.havePrev = false;
        g_snap.haveWorld = false;
        g_snap.haveFilt = false;
        ReleaseSRWLockExclusive(&g_lock);
        g_velTrack.have = false;
        g_euroR = HandEuro{};
    }

    // ---- handedness calibration --------------------------------------------
    // The camera IS the head in first person, so how the game's look vector
    // moves when the head turns right/up tells us which way the game frame's
    // right/up point. Measured, not assumed -- the lean work cost two wrong
    // answers by assuming.
    if (g_rightSign.load() != 0 && g_upSign.load() != 0) return;
    if (!GameplayGate()) { g_cal.have = false; return; }

    V3 from, to, F, R0, U0;
    if (!ReadV16(kViewFromRva, &from) || !ReadV16(kViewToRva, &to)) return;
    if (!CameraBasis(from, to, &F, &R0, &U0)) return;

    const LONGLONG now = NowQpc();
    if (!g_cal.have) {
        g_cal.have = true; g_cal.q = head.orientation;
        g_cal.F[0] = F.x; g_cal.F[1] = F.y; g_cal.F[2] = F.z; g_cal.qpc = now;
        return;
    }
    if (MsSince(g_cal.qpc) < 200.0) return;

    const V3 Fref{ g_cal.F[0], g_cal.F[1], g_cal.F[2] };
    V3 Fr, Rr, Ur;
    const bool okRef = CameraBasis(V3{ 0, 0, 0 }, Fref, &Fr, &Rr, &Ur);
    const V3 headNow = QRot(head.orientation, V3{ 0, 0, -1 });
    const V3 l = QRot(QInv(g_cal.q), headNow);   // x right, y up, in the old head frame
    if (okRef) {
        const float gx = Dot(F, Rr), gy = Dot(F, Ur);
        if (std::fabs(l.x) > 0.03f && std::fabs(gx) > 0.01f) { g_votesX += (l.x * gx > 0) ? 1 : -1; ++g_countX; }
        if (std::fabs(l.y) > 0.03f && std::fabs(gy) > 0.01f) { g_votesY += (l.y * gy > 0) ? 1 : -1; ++g_countY; }
    }
    g_cal.q = head.orientation;
    g_cal.F[0] = F.x; g_cal.F[1] = F.y; g_cal.F[2] = F.z; g_cal.qpc = now;

    auto settle = [](std::atomic<int>& sign, int votes, int count, const char* axis) {
        if (sign.load() != 0 || count < 12) return;
        if (std::abs(votes) * 4 < count * 3) return;          // need 75% agreement
        const int s = votes > 0 ? 1 : -1;
        sign.store(s);
        DebugLogger::LogFormat("Motion aim: CALIBRATED %s_sign=%+d (%d of %d head movements agreed). "
            "Put %s_sign=%d in [motion_aim] to skip this next launch.",
            axis, s, (count + std::abs(votes)) / 2, count, axis, s);
    };
    settle(g_rightSign, g_votesX, g_countX, "right");
    settle(g_upSign, g_votesY, g_countY, "up");
}

void MotionAimPublishEyeImage(unsigned eye, const XrFovf& fov,
                              int32_t subX, int32_t subY, int32_t subW, int32_t subH,
                              int32_t drawX, int32_t drawY, int32_t drawW, int32_t drawH,
                              float visFracX, float visFracY) {
    if (!g_anyInstalled || eye > 1 || subW <= 0 || subH <= 0 || drawW <= 0 || drawH <= 0) return;
    const float tl = std::tan(fov.angleLeft), tr = std::tan(fov.angleRight);
    const float tu = std::tan(fov.angleUp), td = std::tan(fov.angleDown);
    const float u0 = (float)(drawX - subX) / (float)subW, u1 = (float)(drawX + drawW - subX) / (float)subW;
    const float v0 = (float)(drawY - subY) / (float)subH, v1 = (float)(drawY + drawH - subY) / (float)subH;

    EyeImage e;
    e.valid = true;
    e.dL = tl + (tr - tl) * u0;  e.dR = tl + (tr - tl) * u1;
    e.dU = tu + (td - tu) * v0;  e.dD = tu + (td - tu) * v1;
    e.fx = (visFracX > 0.0f && visFracX <= 1.0f) ? visFracX : 1.0f;
    e.fy = (visFracY > 0.0f && visFracY <= 1.0f) ? visFracY : 1.0f;
    e.qpc = NowQpc();

    AcquireSRWLockExclusive(&g_lock);
    const EyeImage prev = g_eyeImg[eye];
    g_eyeImg[eye] = e;
    ReleaseSRWLockExclusive(&g_lock);

    if (!prev.valid || std::fabs(prev.dR - e.dR) > 0.02f || std::fabs(prev.dL - e.dL) > 0.02f) {
        const int clip = ReadClipDistance();
        DebugLogger::LogFormat("Motion aim: eye %u picture spans tan L%+.3f R%+.3f U%+.3f D%+.3f "
            "(game renders +-%.3f x +-%.3f at clip %d) -> horizontal gain %.2f",
            eye, e.dL, e.dR, e.dU, e.dD, 160.0f / clip * e.fx, 120.0f / clip * e.fy, clip,
            (320.0f / clip * e.fx) / (e.dR - e.dL));
    }
}

int MotionAimTakeShotRumble() { return g_shotRumble.exchange(0, std::memory_order_relaxed); }
int MotionAimTakeKnockRumble() { return g_knockRumble.exchange(0, std::memory_order_relaxed); }

void MotionAimNoteFireButton(bool down) {
    if (down) g_lastFireQpc.store(NowQpc(), std::memory_order_relaxed);
}

bool IsMotionAimLive() {
    return g_anyInstalled && g_cfgEnabled && g_rightSign.load() != 0 &&
           g_upSign.load() != 0 && g_fwdIndex >= 0;
}

// ===========================================================================
// Game thread: a bullet is about to be allocated.
// `stack[0]` is the return address into the constructor; everything above it
// is the constructor's frame and then its caller's.
// ===========================================================================
extern "C" void __cdecl MotionAimAllocBody(uint32_t* stack) {
    uint32_t ret = 0;
    if (!SafeRead(&ret, stack, 4)) return;
    int site = -1;
    for (int i = 0; i < kSiteCount; ++i) if (g_sites[i].installed && g_sites[i].retAddr == ret) site = i;
    if (site < 0) return;
    g_sites[site].hits++;

    // ---- rumble on the player's own shot (2026-10-05) ----------------------
    // Fire button pressed within the window AND the projectile starts nearEye
    // your eye (an enemy firing while you hold the trigger starts elsewhere).
    // Checked here, before anything below can stand down, so it works with
    // the gun in your hand, the scope, the Stinger and throws alike.
    if (GameplayGate()) {
        const bool throwSite = g_sites[site].kind == 1;
        const double win = (double)g_cfgFireWindowMs + g_sites[site].extraWindowMs + (throwSite ? 600.0 : 0.0);
        const bool pressed = g_cfgFireWindowMs <= 0 ||
            MsSince(g_lastFireQpc.load(std::memory_order_relaxed)) <= win;
        V3 eye{}, look{};
        if (pressed && ReadV16(kViewFromRva, &eye) && ReadV16(kViewToRva, &look)) {
            bool nearEye = false;
            if (throwSite) {
                uint32_t posP = 0; int16_t pos[3];
                if (SafeRead(&posP, stack + 8, 4) && posP && SafeRead(pos, (const void*)(uintptr_t)posP, 6))
                    nearEye = Len(Sub(V3{ (float)pos[0], (float)pos[1], (float)pos[2] }, eye)) <= g_cfgPlayerRadius;
            }
            else {
                for (int i = 1; i < 192 && !nearEye; ++i) {
                    uint32_t p = 0;
                    if (!SafeRead(&p, stack + i, 4)) break;
                    if (p < 0x10000 || p > 0x7FFE0000 || (p & 1)) continue;
                    GameMatrix c;
                    if (!SafeRead(&c, (const void*)(uintptr_t)p, sizeof(c)) || !LooksLikeRotation(c)) continue;
                    nearEye = Len(Sub(V3{ (float)c.t[0], (float)c.t[1], (float)c.t[2] }, eye)) <= g_cfgPlayerRadius;
                }
            }
            if (nearEye) {
                const int kind = throwSite ? 3 : (g_sites[site].extraWindowMs > 0 ? 2 : 1);
                int cur = g_shotRumble.load(std::memory_order_relaxed);
                while (kind > cur && !g_shotRumble.compare_exchange_weak(cur, kind)) {}
            }
        }
    }

    // ---- thrown objects: NewTenage(SVECTOR* pos, SVECTOR* step, ...) --------
    // Stack at this point (0x5A1135 has an ebp frame and pushes ebx/esi/edi):
    //   [1]=5 [2]=0x124 [3]=edi [4]=esi [5]=ebx [6]=ebp [7]=ret [8]=pos [9]=step
    if (g_sites[site].kind == 1) {
        if (!g_cfgEnabled || !g_cfgThrowAim || !GameplayGate()) return;
        const bool fired = (g_cfgFireWindowMs <= 0) ||
            MsSince(g_lastFireQpc.load(std::memory_order_relaxed)) <= (double)g_cfgFireWindowMs + 600.0;
        if (!fired) return;   // throws release on trigger UP, hence the longer window
        uint32_t cls = 0, size = 0, posP = 0, stepP = 0;
        if (!SafeRead(&cls, stack + 1, 4) || !SafeRead(&size, stack + 2, 4) || cls != 5 || size != 0x124) return;
        if (!SafeRead(&posP, stack + 8, 4) || !SafeRead(&stepP, stack + 9, 4) || !posP || !stepP) return;
        int16_t pos[3], step[3];
        if (!SafeRead(pos, (const void*)(uintptr_t)posP, 6) || !SafeRead(step, (const void*)(uintptr_t)stepP, 6)) return;

        V3 from, to, F, R0, U0;
        if (!ReadV16(kViewFromRva, &from) || !ReadV16(kViewToRva, &to)) return;
        if (!CameraBasis(from, to, &F, &R0, &U0)) return;
        if (Len(Sub(V3{ (float)pos[0], (float)pos[1], (float)pos[2] }, from)) > g_cfgPlayerRadius) return;

        GameMatrix H{};
        float reach = 0.0f;
        if (!BuildHandMatrix(&H, &reach)) return;         // also enforces calibration + fresh pose
        // Aim = the hand matrix's forward (local -Y), the same ray as the laser.
        V3 D{ -(float)H.m[0][1], -(float)H.m[1][1], -(float)H.m[2][1] };
        if (!Norm(D)) return;

        // Keep the game's lob: rotate its throw velocity so that its FLAT heading
        // lands on your aim. Pointing ahead reproduces the original throw exactly;
        // pointing up lofts it, pointing down rolls it.
        const V3 S{ (float)step[0], (float)step[1], (float)step[2] };
        V3 Hs{ S.x, 0.0f, S.z };
        if (!Norm(Hs)) return;
        V3 S2 = Mul(RotateMinimal(S, Hs, D, U0), g_cfgThrowSpeed);

        // SWING. If the hand was actually moving when the trigger let go, the
        // throw follows the swing: its direction, and a speed scaled from it.
        // The game's own throw (~200 units/frame) is taken as a brisk 5 m/s
        // overarm, so a gentle underarm lobs short and a hard throw goes far,
        // capped at twice the game's range.
        const char* mode = "aimed";
        if (g_cfgThrowSwing) {
            AimSnap sw;
            AcquireSRWLockShared(&g_lock);
            sw = g_snap;
            ReleaseSRWLockShared(&g_lock);
            ToRenderHeadFrame(sw);
            const V3 vL{ sw.vel[0], sw.vel[1], sw.vel[2] };
            const float speed = Len(vL);
            if (speed >= g_cfgSwingMinMps) {
                const int sx = g_rightSign.load(), sy = g_upSign.load();
                const int clip = ReadClipDistance();
                float ax, bx, ay, by;
                DisplayMapping(clip, &ax, &bx, &ay, &by);
                V3 Dg = HeadToGame(vL, F, Mul(R0, (float)sx), Mul(U0, (float)sy), ax, bx, ay, by);
                if (Norm(Dg)) {
                    float mag = Len(S) * (speed / 5.0f) * g_cfgSwingGain * g_cfgThrowSpeed;
                    const float cap = Len(S) * 2.0f;
                    if (mag > cap) mag = cap;
                    S2 = Mul(Dg, mag);
                    mode = "SWING";
                }
            }
        }
        auto c16 = [](float f) -> int16_t { long r = std::lround(f); if (r > 32767) r = 32767; if (r < -32768) r = -32768; return (int16_t)r; };
        const int16_t newStep[3] = { c16(S2.x), c16(S2.y), c16(S2.z) };
        const int16_t newPos[3] = { c16((float)H.t[0]), c16((float)H.t[1]), c16((float)H.t[2]) };
        const bool ok = SafeWrite((void*)(uintptr_t)stepP, newStep, 6) && SafeWrite((void*)(uintptr_t)posP, newPos, 6);
        if (g_throwLogs < 20) {
            ++g_throwLogs;
            DebugLogger::LogFormat("Throw[%d] %s %s: from (%d,%d,%d) -> hand (%d,%d,%d), velocity (%d,%d,%d) -> (%d,%d,%d)",
                g_throwLogs, mode, ok ? "REDIRECTED" : "WRITE FAILED", pos[0], pos[1], pos[2],
                newPos[0], newPos[1], newPos[2], step[0], step[1], step[2], newStep[0], newStep[1], newStep[2]);
        }
        return;
    }

    if (!g_cfgEnabled || !GameplayGate()) return;
    // STINGER: aimed with your HEAD (2026-09-29, second pass). Leaving aam.c's
    // matrix alone did NOT fire where you look -- whatever it builds the
    // missile from is not the head-driven camera -- so the launch matrix is now
    // rewritten to your head's forward, like the scoped PSG1, below. After ~14
    // frames amissile.c (+5A0D3A) steers toward the lock target in [0x791DF4]
    // if the game has one within 90 degrees; that is the Stinger's homing and
    // is left alone. The MAim line says whether a lock existed.
    const bool stinger = std::strstr(g_sites[site].label, "Stinger") != nullptr;
    const bool stingerHead = stinger && g_cfgStingerHeadAim;
    // With the gun in your hand, the weapon already builds its bullet from the
    // gun you are holding (objs->world). Redirecting again would only move the
    // muzzle from the barrel back to the controller tip.
    if (IsGunInHandActive() && g_heldFiresFromModel.load(std::memory_order_relaxed)) {
        static bool said = false;
        if (!said) { said = true; DebugLogger::Log("Motion aim: held SOCOM/FAMAS fires from its own barrel -- redirect standing down for it."); }
        return;
    }

    V3 from, to, F, R0, U0;
    if (!ReadV16(kViewFromRva, &from) || !ReadV16(kViewToRva, &to)) return;
    if (!CameraBasis(from, to, &F, &R0, &U0)) return;
    from = HeadCentreFromEye(from, Mul(R0, (float)g_rightSign.load()));   // muzzle from the head centre, not one eye

    const bool playerFired = (g_cfgFireWindowMs <= 0) ||
        MsSince(g_lastFireQpc.load(std::memory_order_relaxed)) <= (double)(g_cfgFireWindowMs + g_sites[site].extraWindowMs);

    // ---- find the MATRIX argument by shape ---------------------------------
    // Any stack dword that points at an orthonormal 4096-scale 3x3 whose
    // translation is within player_radius of the eye. Lowest stack slot wins:
    // that is the innermost frame, i.e. the constructor's own argument.
    GameMatrix M{};
    uintptr_t mAddr = 0;
    int slot = -1, matches = 0;
    constexpr int kScanDwords = 192;
    for (int i = 1; i < kScanDwords; ++i) {
        uint32_t p = 0;
        if (!SafeRead(&p, stack + i, 4)) break;
        if (p < 0x10000 || p > 0x7FFE0000 || (p & 1)) continue;
        GameMatrix c;
        if (!SafeRead(&c, (const void*)(uintptr_t)p, sizeof(c))) continue;
        if (!LooksLikeRotation(c)) continue;
        const V3 t{ (float)c.t[0], (float)c.t[1], (float)c.t[2] };
        if (Len(Sub(t, from)) > g_cfgPlayerRadius) continue;
        ++matches;
        if (slot < 0) { M = c; mAddr = p; slot = i; }
    }

    if (slot < 0) {
        if (playerFired && g_missLogs < g_cfgLogMisses) {
            ++g_missLogs;
            DebugLogger::LogFormat("MAim miss [%s]: player fired but no matrix-shaped argument within %.0f "
                "units of the eye (%.0f,%.0f,%.0f). First stack dwords follow:",
                g_sites[site].label, g_cfgPlayerRadius, from.x, from.y, from.z);
            for (int i = 1; i <= 12; ++i) {
                uint32_t p = 0; SafeRead(&p, stack + i, 4);
                int16_t w[8] = {};
                const bool ok = (p >= 0x10000 && p < 0x7FFE0000) && SafeRead(w, (const void*)(uintptr_t)p, sizeof(w));
                DebugLogger::LogFormat("  [esp+%02X] %08X -> %s %d %d %d %d %d %d %d %d", i * 4, p,
                    ok ? "" : "(unreadable)", w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]);
            }
        }
        return;
    }

    // ---- which of its vectors is "forward" ---------------------------------
    float dots[6];
    for (int k = 0; k < 6; ++k) { V3 v = MatVec(M, k); Norm(v); dots[k] = Dot(v, F); }

    if (g_fwdIndex < 0) {
        if (g_cfgForwardVector >= 0 && g_cfgForwardVector < 6) {
            g_fwdIndex = g_cfgForwardVector;
            g_fwdSign = g_cfgForwardSign != 0 ? (g_cfgForwardSign > 0 ? 1 : -1) : (dots[g_fwdIndex] >= 0 ? 1 : -1);
        }
        // Verify the pinned axis against the first real player shot: it should
        // read close to +1 if FPV fires along the camera, as the handoff says.
        static bool verified[16] = {};
        if (g_fwdIndex >= 0 && playerFired && site < 16 && !verified[site]) {
            verified[site] = true;
            DebugLogger::LogFormat("MAim verify [%s]: pinned forward %s%s reads %+.2f against the camera "
                "(all: c0 %+.2f c1 %+.2f c2 %+.2f r0 %+.2f r1 %+.2f r2 %+.2f). Expect ~+1.",
                g_sites[site].label, g_fwdSign > 0 ? "+" : "-", VecName(g_fwdIndex),
                dots[g_fwdIndex] * (float)g_fwdSign, dots[0], dots[1], dots[2], dots[3], dots[4], dots[5]);
        }
        else if (playerFired) {
            int best = 0, second = -1;
            for (int k = 1; k < 6; ++k) if (std::fabs(dots[k]) > std::fabs(dots[best])) best = k;
            for (int k = 0; k < 6; ++k) if (k != best && (second < 0 || std::fabs(dots[k]) > std::fabs(dots[second]))) second = k;
            // A row and a column can both match when the matrix is near-symmetric
            // (e.g. facing along an axis); only a clear winner counts as a vote.
            const bool clear = std::fabs(dots[best]) > 0.80f &&
                               std::fabs(dots[best]) - std::fabs(dots[second]) > 0.25f;
            const int sgn = dots[best] >= 0 ? 1 : -1;
            DebugLogger::LogFormat("MAim calibrate [%s] slot esp+%02X @%08X: dots c0 %+.2f c1 %+.2f c2 %+.2f "
                "r0 %+.2f r1 %+.2f r2 %+.2f -> %s%s", g_sites[site].label, slot * 4, (unsigned)mAddr,
                dots[0], dots[1], dots[2], dots[3], dots[4], dots[5],
                clear ? (sgn > 0 ? "+" : "-") : "", clear ? VecName(best) : "no clear winner");
            if (clear) {
                if (best == g_fwdVoteIdx && sgn == g_fwdVoteSign) ++g_fwdVoteStreak;
                else { g_fwdVoteIdx = best; g_fwdVoteSign = sgn; g_fwdVoteStreak = 1; }
                if (g_fwdVoteStreak >= 2) {
                    g_fwdIndex = best; g_fwdSign = sgn;
                    DebugLogger::LogFormat("Motion aim: CALIBRATED forward = %s%s. Put forward_vector=%d "
                        "forward_sign=%d in [motion_aim] to skip this next launch.",
                        sgn > 0 ? "+" : "-", VecName(best), best, sgn);
                }
            }
        }
        if (g_fwdIndex < 0) return;
    }

    V3 O = MatVec(M, g_fwdIndex);
    Norm(O);
    O = Mul(O, (float)g_fwdSign);

    // Only the player's own shots: near the eye (already), pointing roughly
    // where the camera looks (an enemy's shot at you points back at you),
    // and the trigger pulled recently.
    if ((!stingerHead && Dot(O, F) < 0.34f) || !playerFired) return;

    const int sx = g_rightSign.load(), sy = g_upSign.load();
    if (sx == 0 || sy == 0) {
        static int nagged = 0;
        if (nagged++ < 3)
            DebugLogger::Log("Motion aim: shot passed through unchanged -- still calibrating right/up. "
                "Look around (left/right AND up/down) in first person for a few seconds.");
        return;
    }

    AimSnap s;
    AcquireSRWLockShared(&g_lock);
    s = g_snap;
    ReleaseSRWLockShared(&g_lock);
    ToRenderHeadFrame(s);
    if (!s.valid || MsSince(s.qpc) > 150.0) return;   // controller asleep / not tracked

    // PSG1 SCOPE UP: the shot goes where you are looking -- straight down the
    // middle of the scope picture -- and leaves from between your eyes.
    {
        int16_t wid = -1;
        SafeRead(&wid, (const void*)(g_base + 0x38E7FC), 2);   // GM_CurrentWeaponId (9 = PSG1)
        if (stingerHead) {
            // Head forward; launched from your right shoulder.
            s.dir[0] = 0.0f; s.dir[1] = 0.0f; s.dir[2] = -1.0f;
            s.pos[0] = 0.12f; s.pos[1] = -0.10f; s.pos[2] = 0.0f;
            uint32_t lock = 0;
            SafeRead(&lock, (const void*)(g_base + 0x391DF4), 4);
            int16_t lp[3] = {};
            if (lock) SafeRead(lp, (const void*)(uintptr_t)(lock + 8), 6);
            static int said = 0;
            if (said++ < 6) DebugLogger::LogFormat("Motion aim: Stinger launched along your head. Lock target %s%08X (%d,%d,%d) -- "
                "with a lock the missile homes on it after ~0.5 s, without one it flies straight.",
                lock ? "" : "NONE ", lock, lp[0], lp[1], lp[2]);
        }
        else if (IsScopeViewActive() && wid == 9) {
            s.dir[0] = 0.0f; s.dir[1] = 0.0f; s.dir[2] = -1.0f;
            s.pos[0] = 0.0f; s.pos[1] = 0.0f; s.pos[2] = 0.0f;
            static int said = 0;
            if (said++ < 3) DebugLogger::Log("Motion aim: PSG1 fired through the scope -- aimed by your head.");
        }
    }

    const int clip = ReadClipDistance();
    float ax, bx, ay, by;
    DisplayMapping(clip, &ax, &bx, &ay, &by);
    const V3 R = Mul(R0, (float)sx), U = Mul(U0, (float)sy);

    V3 D = HeadToGame(V3{ s.dir[0], s.dir[1], s.dir[2] }, F, R, U, ax, bx, ay, by);
    if (!Norm(D)) return;

    // ---- rewrite: rotate every vector of the forward's kind by O -> D -------
    const int base = g_fwdIndex < 3 ? 0 : 3;
    GameMatrix out = M;
    for (int k = base; k < base + 3; ++k) SetMatVec(out, k, RotateMinimal(MatVec(M, k), O, D, U));

    V3 muzzle{ (float)M.t[0], (float)M.t[1], (float)M.t[2] };
    if (g_cfgMuzzleFromHand) {
        V3 p{ s.pos[0], s.pos[1], s.pos[2] };
        const float reach = Len(p);
        if (reach > g_cfgMaxReachM && reach > 1e-4f) p = Mul(p, g_cfgMaxReachM / reach);
        muzzle = Add(from, Mul(HeadToGame(p, F, R, U, ax, bx, ay, by), g_cfgUnitsPerMetre));
        out.t[0] = (int32_t)std::lround(muzzle.x);
        out.t[1] = (int32_t)std::lround(muzzle.y);
        out.t[2] = (int32_t)std::lround(muzzle.z);
    }

    // Only the rotation and translation; the pad word is left as found.
    const bool wrote = SafeWrite((void*)mAddr, out.m, sizeof(out.m)) &&
                       SafeWrite((void*)(mAddr + 0x14), out.t, sizeof(out.t));

    if (g_shotLogs < g_cfgLogShots) {
        ++g_shotLogs;
        const float offDeg = std::acos(std::fmax(-1.0f, std::fmin(1.0f, Dot(O, D)))) * 180.0f / kPi;
        DebugLogger::LogFormat("MAim[%d] %s %s: slot esp+%02X @%08X, game aim -> hand aim turned %.1f deg, "
            "muzzle (%d,%d,%d) -> (%d,%d,%d), map gain x%.2f y%.2f, %d candidate(s)%s",
            g_shotLogs, g_sites[site].label, wrote ? "REDIRECTED" : "WRITE FAILED",
            slot * 4, (unsigned)mAddr, offDeg,
            M.t[0], M.t[1], M.t[2], out.t[0], out.t[1], out.t[2], ax, ay, matches,
            matches > 1 ? " (used the innermost)" : "");
    }
}


// ===========================================================================
// GUN IN HAND -- the game draws its own weapon model at your controller.
//
// Everything below was read out of mgsi.exe (2026-09-22) and the FoxdieTeam
// decomp, not guessed:
//
//   0x40A1BF  GV_ExecActorSystem. Walks classes 0..8, and for every actor:
//             40A216  push [ebp-0x14]    ; FF 75 EC   the actor
//             40A219  call [ebp-0x10]    ; FF 55 F0   its act function
//             40A21C  pop  ecx           ; 59
//             Those 7 bytes become `call thunk` + 2 nops. The thunk makes the
//             same call and then runs a post-act hook -- so we run on the game
//             thread, right after a weapon's own update, before the renderer
//             (class 8) draws anything. No list walking, no cross-thread writes.
//   Weapon work (weapon\socom.c et al., class 6):
//             +0x20 DG_OBJS* (the gun model)   +0x48 root OBJECT* (Snake)
//             +0x4C unit (Snake's hand joint)
//   DG_OBJS:  +0x00 world MATRIX   +0x20 MATRIX* root   +0x28 flag (0x80 = hidden)
//   0x404271  if (objs->root) objs->world = *objs->root   -- every frame, at render
//   0x45011B  GM_ConfigObjectRoot: root = &parent->objs->objs[unit].world
//             (parent->objs + 0x48 + unit*0x5C)
//   socom Act 0x640FB3: in first person Snake's body is hidden, so the gun is
//             hidden with it ([objs+0x28] |= 0x80). Nothing else stops it
//             drawing.
//
// So: after the weapon's act, point objs->root at a MATRIX we own and clear
// the hidden bit. The engine renders its own gun -- real mesh, real textures,
// real lighting, and walls correctly in front of it -- wherever we put it. And
// because the weapon builds its bullet from objs->world (socom: DG_SetPos(world)
// / DG_MovePos(muzzle 20,-370,60) / NewBullet), shots leave the muzzle of the
// gun you are holding with no further help.
//
// Stateless on purpose: to hand the gun back we REBUILD Snake's hand pointer
// from the weapon's own fields rather than remembering one, so a weapon
// switch, a room change or a pause can never leave a stale pointer behind.
// ===========================================================================
namespace {

constexpr uintptr_t kDispatchRva = 0x0A216;
const uint8_t kDispatchExpected[7] = { 0xFF, 0x75, 0xEC, 0xFF, 0x55, 0xF0, 0x59 };

// group: 0 = SOCOM/FAMAS, 1 = other firearms, 2 = throwables.
// Offsets are per weapon because grenade.c lays its work out differently
// (actor, root_ctrl +0x20, root_obj +0x24, object +0x28, unit +0x4C,
// flags +0x50, pos +0x54, time +0x5C, type +0x60 -- read at 0x63FF71..0x63FFD3).
struct WeaponAct {
    uint32_t actRva; const char* name; int group;
    uint32_t objsOff, rootObjOff, unitOff, timeOff;   // timeOff 0 = none
    bool firesFromModel;   // shot built from objs->world (so the held gun already aims it)
    unsigned logged;
};
WeaponAct g_weapons[] = {
    // SOCOM/FAMAS(+MP5): socom.c/famas.c fire from DG_SetPos(objs->world).
    // PSG1: rifle.c builds its bullet from Snake's heading + hand joint.
    // Nikita/Stinger: rcm.c/aam.c build their missile from heading/camera.
    // For the last three the held model does NOT aim the shot, so the
    // constructor hooks above do it instead.
    { 0x240FB3, "SOCOM",   0, 0x20, 0x48, 0x4C, 0,    true,  0 },
    { 0x240CDC, "FAMAS",   0, 0x20, 0x48, 0x4C, 0,    true,  0 },
    { 0x23FB13, "PSG1",    1, 0x20, 0x48, 0x4C, 0,    false, 0 },
    { 0x2408D5, "Nikita",  1, 0x20, 0x48, 0x4C, 0,    false, 0 },
    { 0x24064A, "Stinger", 1, 0x20, 0x48, 0x4C, 0,    false, 0 },
    { 0x23FF5A, "Grenade/Stun/Chaff", 2, 0x28, 0x24, 0x4C, 0x5C, false, 0 },
};
constexpr int kWeaponCount = (int)(sizeof(g_weapons) / sizeof(g_weapons[0]));

// GM_CurrentWeaponId and the ammo array (CE table + decomp order).
constexpr uintptr_t kCurWeaponRva = 0x38E7FC, kAmmoArrayRva = 0x38E802;

constexpr uint32_t kObjsRoot = 0x20, kObjsFlag = 0x28, kObjsFirstObj = 0x48, kObjSize = 0x5C;
constexpr uint32_t kHiddenBit = 0x80;

// The matrix the gun hangs from. DG copies it into objs->world at render time,
// on the game thread -- the same thread that writes it -- so no tearing.
GameMatrix g_handMtx{};

bool ReadU32(uint32_t a, uint32_t* v) { return SafeRead(v, (const void*)(uintptr_t)a, 4); }
bool WriteU32(uint32_t a, uint32_t v) { return SafeWrite((void*)(uintptr_t)a, &v, 4); }

// What objs->root normally is: Snake's hand joint.
bool OriginalRoot(const WeaponAct& W, uint32_t work, uint32_t* out) {
    uint32_t rootObj = 0, unit = 0, parentObjs = 0;
    if (!ReadU32(work + W.rootObjOff, &rootObj) || !rootObj) return false;
    if (!ReadU32(work + W.unitOff, &unit) || unit > 64) return false;
    if (!ReadU32(rootObj, &parentObjs) || !parentObjs) return false;
    *out = parentObjs + kObjsFirstObj + unit * kObjSize;
    return true;
}

// Controller pose -> the gun's world matrix. Gun-local axes, from the decomp:
// the bullet leaves along local -Y and the muzzle sits at (20,-370,60), i.e.
// the barrel runs down -Y with the bore slightly +Z of the grip -- so +Z is
// the top of the gun. The matrix maps local -> world, so column 1 = -forward
// and column 2 = up.
// FROM is one eye's camera. Recover the head centre: first by exact lookup of
// the eye offset the position hook wrote into THAT camera (timing-proof), else
// the legacy guess from the current stereo eye (can pick the wrong eye when the
// gun is built against the previous frame's camera -> +/-IPD error = double gun).
static unsigned g_eyeLookupHit = 0, g_eyeLookupMiss = 0;
static V3 HeadCentreFromEye(V3 from, V3 R) {
    const int32_t fi[3] = { (int32_t)std::lround(from.x), (int32_t)std::lround(from.y), (int32_t)std::lround(from.z) };
    int32_t e[3] = { 0, 0, 0 };
    V3 out = from;
    if (LookupEyeOffsetForCamera(fi, e)) {
        ++g_eyeLookupHit;
        out = Sub(from, V3{ (float)e[0], (float)e[1], (float)e[2] });
    }
    else {
        ++g_eyeLookupMiss;
        if (g_cfgGunEyeCenterSign != 0) {
            const int eye = GetCurrentStereoEye();
            if (eye == 0 || eye == 1) {
                float half = g_cfgHalfIpdM * g_cfgUnitsPerMetre * (float)g_cfgGunEyeCenterSign;
                if (g_cfgStereoSwap) half = -half;
                out = Add(from, Mul(R, eye == 0 ? half : -half));   // left eye sits left of centre
            }
        }
    }
    const unsigned n = g_eyeLookupHit + g_eyeLookupMiss;
    if (n == 30 || n == 300 || (n % 3000) == 0) {
        DebugLogger::LogFormat("GunAnchor: head centre from exact eye lookup %u / %u (miss -> legacy eye guess). "
            "last eye offset (%d,%d,%d)", g_eyeLookupHit, n, e[0], e[1], e[2]);
    }
    return out;
}

// How far ahead of the newest controller sample the held gun has to be drawn:
// the measured age of a game image when it first reaches your eyes, plus half
// of the time it then stays on screen, scaled by hand_predict_percent. A fixed
// hand_predict_ms (>= 0) overrides the measurement. 45 ms until measured.
double HandPredictHorizonMs() {
    double h;
    if (g_cfgHandPredictMs >= 0) h = (double)g_cfgHandPredictMs;
    else {
        const float age = g_imgAgeMs.load(std::memory_order_relaxed);
        if (age <= 0.0f) h = 45.0;
        else {
            const float hold = g_imgPeriodMs.load(std::memory_order_relaxed) - g_dispPeriodMs.load(std::memory_order_relaxed);
            h = (double)age + (hold > 0.0f ? 0.5 * hold : 0.0);
        }
        h *= (double)g_cfgHandPredictPercent / 100.0;
    }
    if (h < 0.0) h = 0.0;
    if (h > 120.0) h = 120.0;
    return h;
}

// Controller snapshot -> a world MATRIX in the gun/hand joint's local frame
// (local -Y = forward, +Z = up; the frame Snake's hand joint and every weapon
// model share, which is why the gun hangs from part 4 with no extra rotation).
// Shared by the gun, the right hand and the left hand.
//   gunOffset : add g_cfgGunOffset (the gun only -- the hand stays on the joint)
//   rollDeg / rotDeg : extra rotation, degrees (roll about the barrel; rotDeg is
//                      pitch/yaw/roll applied in the hand's own frame)
//   headOut   : the head centre FROM was nudged away from, for collision rays
bool BuildControllerMatrix(AimSnap s, bool gunOffset, float rollDeg, const float* rotDeg,
                           GameMatrix* M, float* reachM, V3* headOut, V3* fwdOut = nullptr) {
    const int sx = g_rightSign.load(), sy = g_upSign.load();
    if (sx == 0 || sy == 0) return false;

    if (!s.valid || MsSince(s.qpc) > 150.0) return false;

    // SMOOTHED + PREDICTED (2026-10-04, hand_filter=1). The filtered pose is
    // carried forward along its smoothed velocity by the prediction horizon
    // PLUS the lag the filter itself introduced, so smoothing costs no extra
    // drag while the hand moves, and tracking tremor is not amplified.
    bool smoothed = false;
    if (g_cfgHandFilter && s.haveFilt && s.haveWorld) {
        const float aheadS = (float)((MsSince(s.qpc) + HandPredictHorizonMs()) / 1000.0);
        auto get = [](const float* a) { return V3{ a[0], a[1], a[2] }; };
        auto put = [](float* d, V3 v) { d[0] = v.x; d[1] = v.y; d[2] = v.z; };
        // Fade prediction out as the hand comes to rest: extrapolating a
        // velocity that is only tracking noise is what made a still hand shake.
        auto ramp = [](float v, float lo, float hi) {
            if (hi <= lo) return v > lo ? 1.0f : 0.0f;
            float t = (v - lo) / (hi - lo);
            return t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t * t * (3.0f - 2.0f * t));
        };
        const float kPos = ramp(Len(get(s.vpos)), g_cfgStillMps, g_cfgMovingMps);
        const float kRot = ramp(Len(get(s.vdir)), g_cfgStillRads, g_cfgMovingRads);
        V3 dp = Mul(get(s.vpos), (aheadS + s.lagPos) * kPos);
        const float dpl = Len(dp);
        if (dpl > 0.15f) dp = Mul(dp, 0.15f / dpl);
        auto lead = [&](const float* x, const float* v) {
            V3 d = Mul(get(v), (aheadS + s.lagRot) * kRot);
            const float l = Len(d);
            if (l > 0.6f) d = Mul(d, 0.6f / l);
            V3 r = Add(get(x), d);
            if (!Norm(r)) r = get(x);
            return r;
        };
        const V3 wp = Add(get(s.fwpos), dp);
        const V3 wd = lead(s.fwdir, s.vdir);
        const V3 wu = lead(s.fwup, s.vup);
        put(s.wpos, wp); put(s.wdir, wd); put(s.wup, wu);
        s.havePrev = false;
        if (!ToRenderHeadFrame(s)) {
            // No render record: relative to the head pose at publish time.
            const XrQuaternionf inv = QInv(s.hq);
            put(s.dir, QRot(inv, wd));
            put(s.up, QRot(inv, wu));
            put(s.pos, QRot(inv, Sub(wp, get(s.hpos))));
        }
        smoothed = true;
        static int s_smoothLog = 0;
        if (s_smoothLog < 3 || (s_smoothLog % 900) == 0) {
            DebugLogger::LogFormat("Hand smoothing: predicting %.0f ms ahead (+%.0f ms filter lag), image age %.0f ms, "
                "game frame %.1f ms, headset frame %.1f ms | hand speed %.2f m/s -> prediction %.0f%% (pos) %.0f%% (turn)",
                aheadS * 1000.0f, s.lagPos * 1000.0f, g_imgAgeMs.load(), g_imgPeriodMs.load(), g_dispPeriodMs.load(),
                Len(get(s.vpos)), kPos * 100.0f, kRot * 100.0f);
        }
        s_smoothLog++;
    }
    else {
        ToRenderHeadFrame(s);
    }

    // PREDICTION (legacy, hand_filter=0). The game draws this matrix into a frame that reaches your
    // eyes roughly one game frame plus capture plus one XR frame later
    // (~45 ms). Extrapolating the hand by that much -- from the last two
    // samples, so rotation AND position -- puts the gun where your hand will
    // be when you see it, instead of where it was. Clamped: a lost sample
    // can never fling it.
    if (!smoothed && g_cfgHandPredictMs != 0 && s.havePrev) {
        const double span = (double)(s.qpc - s.pqpc) * 1000.0 / (double)g_qpcFreq;
        if (span > 2.0 && span < 60.0) {
            double ahead = MsSince(s.qpc) + HandPredictHorizonMs();
            if (ahead > 100.0) ahead = 100.0;
            const float k = (float)(ahead / span);
            for (int i = 0; i < 3; ++i) {
                s.dir[i] += (s.dir[i] - s.pdir[i]) * k;
                s.up[i]  += (s.up[i]  - s.pup[i])  * k;
                float d = (s.pos[i] - s.ppos[i]) * k;
                if (d > 0.15f) d = 0.15f;
                if (d < -0.15f) d = -0.15f;
                s.pos[i] += d;
            }
        }
    }

    V3 from, to, F, R0, U0;
    {
        // Snake's eyes (REX top): the pair it actually drew. Layer 3 holds the
        // game's own camera again by the time the actors run.
        int16_t pf[3], pt[3];
        if (GetGameplayPovPair(pf, pt)) {
            from = { (float)pf[0], (float)pf[1], (float)pf[2] };
            to = { (float)pt[0], (float)pt[1], (float)pt[2] };
        }
        else if (!ReadV16(kViewFromRva, &from) || !ReadV16(kViewToRva, &to)) return false;
    }
    if (!CameraBasis(from, to, &F, &R0, &U0)) return false;
    const V3 R = Mul(R0, (float)sx), U = Mul(U0, (float)sy);

    // FROM is the EYE, and in alternate-eye stereo it is half an IPD to one
    // side on every frame. Anchoring the gun to it would carry the gun along
    // with each eye and give it zero disparity -- a flat cut-out at infinity.
    // Undo the nudge so the gun hangs off the head centre like the world does.
    from = HeadCentreFromEye(from, R);
    if (headOut) *headOut = from;
    if (fwdOut) *fwdOut = F;

    const int clip = ReadClipDistance();
    float ax, bx, ay, by;
    DisplayMapping(clip, &ax, &bx, &ay, &by);

    V3 fwd = HeadToGame(V3{ s.dir[0], s.dir[1], s.dir[2] }, F, R, U, ax, bx, ay, by);
    V3 up  = HeadToGame(V3{ s.up[0],  s.up[1],  s.up[2]  }, F, R, U, ax, bx, ay, by);
    if (!Norm(fwd)) return false;
    up = Sub(up, Mul(fwd, Dot(up, fwd)));
    if (!Norm(up)) return false;
    if (rotDeg && (rotDeg[0] != 0.0f || rotDeg[1] != 0.0f)) {
        // pitch about the hand's side axis, then yaw about its up axis
        V3 side = Cross(fwd, up);
        const float p = rotDeg[0] * kPi / 180.0f;
        const V3 f1 = Add(Mul(fwd, std::cos(p)), Mul(up, std::sin(p)));
        const V3 u1 = Sub(Mul(up, std::cos(p)), Mul(fwd, std::sin(p)));
        fwd = f1; up = u1; side = Cross(fwd, up);
        const float y = rotDeg[1] * kPi / 180.0f;
        fwd = Add(Mul(fwd, std::cos(y)), Mul(side, std::sin(y)));
        Norm(fwd);
        up = Sub(up, Mul(fwd, Dot(up, fwd)));
        if (!Norm(up)) return false;
    }
    const float rollTotal = rollDeg + (rotDeg ? rotDeg[2] : 0.0f);
    if (rollTotal != 0.0f) {
        const float r = rollTotal * kPi / 180.0f;
        const V3 side = Cross(fwd, up);
        up = Add(Mul(up, std::cos(r)), Mul(side, std::sin(r)));
    }

    const V3 c1 = Mul(fwd, -1.0f);   // local -Y = forward
    const V3 c2 = up;                // local +Z = top of the gun
    const V3 c0 = Cross(c1, c2);     // proper rotation: det = +1
    for (int i = 0; i < 3; ++i) {
        const float col[3][3] = { { c0.x, c0.y, c0.z }, { c1.x, c1.y, c1.z }, { c2.x, c2.y, c2.z } };
        for (int j = 0; j < 3; ++j) M->m[i][j] = (int16_t)std::lround(col[j][i] * 4096.0f);
    }
    M->pad = 0;

    V3 p{ s.pos[0], s.pos[1], s.pos[2] };
    const float reach = Len(p);
    if (reachM) *reachM = reach;
    if (reach > g_cfgMaxReachM && reach > 1e-4f) p = Mul(p, g_cfgMaxReachM / reach);
    V3 t = Add(from, Mul(HeadToGame(p, F, R, U, ax, bx, ay, by), g_cfgUnitsPerMetre));
    // Grip offset, in the gun's own axes.
    if (gunOffset)
        t = Add(t, Add(Add(Mul(c0, g_cfgGunOffset[0]), Mul(c1, g_cfgGunOffset[1])), Mul(c2, g_cfgGunOffset[2])));
    M->t[0] = (int32_t)std::lround(t.x);
    M->t[1] = (int32_t)std::lround(t.y);
    M->t[2] = (int32_t)std::lround(t.z);
    return true;
}

// The gun's matrix. Prefers the one the hands pass built this game frame
// (Snake acts before his weapon), because that one has had wall collision
// applied; falls back to building it here exactly as before.
bool HandsFreshGunMatrix(GameMatrix* M, float* reachM);   // hands section, below
bool BuildHandMatrix(GameMatrix* M, float* reachM) {
    if (HandsFreshGunMatrix(M, reachM)) return true;
    AimSnap s;
    AcquireSRWLockShared(&g_lock);
    s = g_snap;
    ReleaseSRWLockShared(&g_lock);
    return BuildControllerMatrix(s, true, g_cfgGunRollDeg, nullptr, M, reachM, nullptr);
}

int g_gunLogs = 0;

// MODEL PROBE -- groundwork for drawing the held weapon at headset rate.
// DG_OBJS +0x24 is DG_DEF*; DG_DEF = { n_models, n_x_models, bbox[6], DG_MDL
// models[] at +0x20 }, each DG_MDL 0x58 bytes (fmt_kmd.h). One log block per
// weapon type, so the next session proves (or refutes) that the PC port keeps
// the PSX KMD layout before a renderer is built on it.
// BODY PROBE -- groundwork for a floating left hand. Snake's body is one
// DG_OBJS whose objs[i] (0x5C each from +0x48) are the body parts; each part's
// DG_MDL carries its parent index at +0x2C. Logging the parent chain, sizes and
// where each part sits relative to the waist identifies the left hand
// unambiguously (the leaf of the arm chain on the opposite side to the
// weapon's joint), without guessing an index.
void ProbeSnakeBody(uint32_t bodyObjs, int weaponUnit) {
    int16_t n = 0;
    if (!SafeRead(&n, (const void*)(uintptr_t)(bodyObjs + 0x2E), 2) || n <= 0 || n > 40) return;
    int32_t t0[3] = {};
    SafeRead(t0, (const void*)(uintptr_t)(bodyObjs + 0x48 + 0x14), sizeof(t0));
    DebugLogger::LogFormat("Body: Snake DG_OBJS %08X has %d parts; weapon hangs on part %d. Positions relative to part 0:",
        bodyObjs, (int)n, weaponUnit);
    for (int i = 0; i < n; ++i) {
        const uint32_t obj = bodyObjs + 0x48 + (uint32_t)i * 0x5C;
        uint32_t mdl = 0; int32_t t[3] = {};
        ReadU32(obj + 0x40, &mdl);
        SafeRead(t, (const void*)(uintptr_t)(obj + 0x14), sizeof(t));
        int32_t parent = -99, faces = 0, verts = 0;
        if (mdl) {
            SafeRead(&faces, (const void*)(uintptr_t)(mdl + 0x04), 4);
            SafeRead(&parent, (const void*)(uintptr_t)(mdl + 0x2C), 4);
            SafeRead(&verts, (const void*)(uintptr_t)(mdl + 0x34), 4);
        }
        DebugLogger::LogFormat("  part %2d parent %3d faces %4d verts %4d rel(%6d,%6d,%6d)%s", i, (int)parent,
            (int)faces, (int)verts, t[0] - t0[0], t[1] - t0[1], t[2] - t0[2], i == weaponUnit ? "  <- weapon" : "");
    }
}

void ProbeWeaponModel(const WeaponAct& W, uint32_t objs) {
    uint32_t def = 0;
    int16_t nObjModels = 0;
    if (!ReadU32(objs + 0x24, &def) || !def) return;
    SafeRead(&nObjModels, (const void*)(uintptr_t)(objs + 0x2E), 2);
    int32_t hdr[8] = {};
    if (!SafeRead(hdr, (const void*)(uintptr_t)def, sizeof(hdr))) return;
    DebugLogger::LogFormat("Model[%s]: DG_DEF %08X n_models=%d n_x=%d bbox(%d,%d,%d)-(%d,%d,%d) | objs n_models=%d",
        W.name, def, hdr[0], hdr[1], hdr[2], hdr[3], hdr[4], hdr[5], hdr[6], hdr[7], (int)nObjModels);
    const int n = (hdr[0] > 0 && hdr[0] <= 8) ? hdr[0] : 0;
    for (int m = 0; m < n; ++m) {
        const uint32_t mdl = def + 0x20 + (uint32_t)m * 0x58;
        uint32_t f[22] = {};
        if (!SafeRead(f, (const void*)(uintptr_t)mdl, sizeof(f))) break;
        // f: 0 flag 1 n_faces 2-10 bbox/offset 11 parent 12 extend 13 n_verts 14 verts 15 vindices
        //    16 n_norms 17 norms 18 nindices 19 uvs 20 texids 21 pad
        int16_t v[12] = {}; uint8_t vi[8] = {}; uint16_t tex[4] = {}; uint8_t uv[8] = {};
        SafeRead(v, (const void*)(uintptr_t)f[14], sizeof(v));
        SafeRead(vi, (const void*)(uintptr_t)f[15], sizeof(vi));
        SafeRead(tex, (const void*)(uintptr_t)f[20], sizeof(tex));
        SafeRead(uv, (const void*)(uintptr_t)f[19], sizeof(uv));
        uint32_t objModel = 0;
        ReadU32(objs + 0x48 + (uint32_t)m * 0x5C + 0x40, &objModel);
        DebugLogger::LogFormat("  mdl%d @%08X flag=%08X faces=%d verts=%d norms=%d parent=%d extend=%d off(%d,%d,%d) "
            "| v0(%d,%d,%d) v1(%d,%d,%d) v2(%d,%d,%d) | idx %u %u %u %u %u %u %u %u | uv %u %u %u %u | tex %04X %04X | obj.model=%08X%s",
            m, mdl, f[0], (int)f[1], (int)f[13], (int)f[16], (int)f[11], (int)f[12], (int)f[8], (int)f[9], (int)f[10],
            v[0], v[1], v[2], v[4], v[5], v[6], v[8], v[9], v[10],
            vi[0], vi[1], vi[2], vi[3], vi[4], vi[5], vi[6], vi[7], uv[0], uv[1], uv[2], uv[3], tex[0], tex[1],
            objModel, objModel == mdl ? " (MATCH)" : "");
    }
}
// ===========================================================================
// FLOATING HANDS + WALL TOUCH (2026-09-25)
//
// Snake's OWN hands, drawn by the game at your controllers -- the same trick as
// the gun in hand, applied to his body model. Everything below was read out of
// mgsi.exe, not guessed:
//
//   Snake's body is ONE DG_OBJS, reached as [sna work + 0x9C] (the same field
//   the head-height read has always used). The body probe (2026-09-24 log)
//   listed its 16 parts: 2->3->4 is the right arm ending in the hand the gun
//   hangs on (part 4), 7->8->9 the left arm ending in the matching hand
//   (part 9, same 22 faces / 28 verts as part 4), 5->6 neck and head.
//
//   At render, 0x4066F2 builds each part's WORLD matrix into a scratch array
//   at 0x991E80 + part*0x20 (0x406B97, from the animation), then
//   `push ebp / push ebx / call 0x406906` at 0x406740 turns those into screen
//   matrices. We retarget THAT call: our wrapper overwrites the scratch world
//   of the two hand parts with the controller poses, collapses every other
//   part to a point inside the hand (a zero rotation draws nothing), and then
//   calls 0x406906 as before. The body's hidden bit (DG_OBJS +0x28 & 0x80, set
//   by Snake in first person) is lifted only while this is running.
//
//   Hand local frame == gun local frame (-Y forward, +Z up): the gun's root is
//   literally &objs[4].world in the stock game. So the right hand sits exactly
//   where it grips the gun, with no calibration of its own.
//
// Wall touch uses the collision the BULLETS use (okajima/bullet.c, 0x5FE41D):
//   map = GM_GetMap(GM_CurrentMap)          0x44F79E(mask), mask at 0x9942A0
//   hit = HZD_LineTest(map->hzd, &from, &to, 0xF, 4)   0x5D8EFB
//   HZD_GetHitPoint(&out)                   0x5D9BC2
// A line from your head to each hand; if it crosses a wall or floor, the hand
// stops on the surface. The gun gets a second line along its barrel and slides
// back instead of poking through.
//
// Knock: touching a wall with some speed plays a sound at the contact point
// and raises the game's own noise event -- the three globals every enemy reads
// (power 0x995364, length 0x995340, position 0x995328), written with the exact
// "only if louder" rule the game inlines at every noise site (e.g. Snake's
// footsteps at 0x4E1143: power 4, length 2).
// ===========================================================================

constexpr uintptr_t kSnaActRva        = 0x0E1755;  // chara\snake\sna_init.c act
constexpr uintptr_t kScreenCallRva    = 0x006740;  // push ebp / push ebx / call 0x406906
constexpr uintptr_t kScreenObjsRva    = 0x006906;
const uint8_t kScreenCallExpected[7]  = { 0x55, 0x53, 0xE8, 0xBF, 0x01, 0x00, 0x00 };
constexpr uintptr_t kPartWorldScratch = 0x591E80;  // 0x991E80: part world matrices, 0x20 each
constexpr uintptr_t kCurMapRva        = 0x5942A0;  // GM_CurrentMap (mask)
constexpr uintptr_t kGetMapRva        = 0x04F79E;
constexpr uintptr_t kHzdLineTestRva   = 0x1D8EFB;
constexpr uintptr_t kHzdHitPointRva   = 0x1D9BC2;
constexpr uintptr_t kSeSetRva         = 0x04F8E6;  // GM_SeSet(SVECTOR* pos, int id)
constexpr uintptr_t kNoisePowerRva    = 0x595364;
constexpr uintptr_t kNoiseLengthRva   = 0x595340;
constexpr uintptr_t kNoisePosRva      = 0x595328;
// The game's own knock (sna_knock at +DED6F): after its wall ray it reads the
// hit surface's attribute word (+1D9B86 returns int16 at +591EA8), skips the
// knock entirely when (attr & 0x700) >= 0x400, and otherwise looks the sound up
// in the stage's material table: byte[+3248B8 + ((attr >> 8) & 3) * 3]
// (+54FFD). Stage scripts fill that table; the boot default is 0x37. A tiny
// actor (+114F7C) then plays it 6 frames later with GM_SeSetMode(pos, id, 1)
// (+4FDA3) and raises noise 100 / length 16 at the player.
constexpr uintptr_t kHitAttrRva       = 0x591EA8;
constexpr uintptr_t kMaterialSeRva    = 0x3248B8;
constexpr uintptr_t kSeSetModeRva     = 0x04FDA3;
constexpr uint32_t  kSnaBodyObjsOff   = 0x9C;
const uint8_t kHzdSig[6]  = { 0x55, 0x8B, 0xEC, 0x51, 0x51, 0xA1 };
const uint8_t kSeSig[6]   = { 0x55, 0x8B, 0xEC, 0x51, 0x51, 0x8B };
const uint8_t kSeModeSig[8] = { 0x55, 0x8B, 0xEC, 0x51, 0x51, 0x8B, 0x4D, 0x08 };
const uint8_t kMapSig[6]  = { 0x8B, 0x0D, 0xB4, 0x29, 0x72, 0x00 };

// --- [hands] config ----------------------------------------------------------
bool  h_enabled = true, h_collision = true, h_gunCollision = true, h_knock = true, h_forearms = false;
int   h_partR = 4, h_partL = 9, h_foreR = 3, h_foreL = 8;
float h_rotR[3] = { 0, 0, 0 }, h_rotL[3] = { 0, 0, 0 };
float h_offR[3] = { 0, 0, 0 }, h_offL[3] = { 0, 0, 0 };   // game units, hand-local x/y/z
float h_margin = 30.0f;          // stop this far short of the surface (game units)
float h_gunLength = 420.0f;      // grip to muzzle, game units (socom muzzle is at -370)
float h_knockSpeed = 0.6f;       // m/s at the moment of contact
int   h_knockCooldownMs = 300;
int   h_knockSound = -1;         // -1 = the game's own choice: the wall's material sound
int   h_knockPower = 100, h_knockLength = 16;   // what the game's knock raises
int16_t h_lastHitAttr = 0;       // surface attribute of the most recent LineHit
int   h_logLines = 40;

// --- [hands] full body + arm IK (2026-10-04) ----------------------------------
// full_body=1: instead of folding Snake's body into his hands, draw all of it
// in its animated pose, hide only the head and neck (the camera sits inside
// them), and bend each arm with two-bone IK so the shoulder (from the
// animation) reaches the hand (at your controller). Render scratch only --
// game logic never sees any of it, exactly like the hands.
bool  h_fullBody = false;
int   h_hideParts[8] = { 5, 6, -1, -1, -1, -1, -1, -1 };   // head + neck by default
int   h_hidePartsN = 2;
float h_elbowOut = 0.5f;         // elbow pole: 1.0 toward the feet + this much outward
int   h_ikLogLines = 12;
int   h_parent[40];              // part -> parent part, filled by ValidateBody
int   h_upR = -1, h_upL = -1;    // upper-arm parts (parents of the forearms)
unsigned h_ikFrames = 0, h_ikOverReach = 0;
int   h_ikLogs = 0;

bool  h_hooked = false, h_readOk = false, h_disabledRuntime = false;
uint8_t* h_callSite = nullptr;
int32_t  h_callOrigRel = 0;


// --- per-game-frame result, game thread only ---------------------------------
struct HandsFrame {
    bool active = false;
    uint32_t bodyObjs = 0;
    int nParts = 0;
    bool haveR = false, haveL = false;
    GameMatrix R{}, L{};
    bool gunValid = false; GameMatrix gun{}; float gunReach = 0;
    bool haveHead = false; V3 head{}, fwd{};   // head point + view forward the hands were built from
    LONGLONG qpc = 0;
} g_hf;
LONGLONG h_seenQpc = 0;          // wrapper last saw the body (render reached it)
LONGLONG h_unhideQpc = 0;
LONGLONG h_anyScreenQpc = 0;     // wrapper ran at all (the game is rendering)
uint8_t  h_leftSide[40] = {};    // 1 = part collapses onto the LEFT hand's wrist
uint32_t h_unhidObjs = 0;
bool h_unhidWasHidden = true;   // did WE clear its hidden bit (first person) or was it visible anyway (REX top)?
uint32_t h_validatedObjs = 0;
bool     h_contact[2] = { false, false };
std::atomic<unsigned long long> h_touchTick[2] = { {0}, {0} };   // [0] right, [1] left: last frame the hand met a wall
LONGLONG h_lastKnockQpc[2] = { 0, 0 };
int      h_probeIdx = 0;
int      h_logs = 0;
unsigned h_contacts = 0, h_knocks = 0, h_gunPushes = 0;

const int kKnockProbe[] = { 0x2C, 0x2F, 0x31, 0x09, 0x04, 0x33, 0x13, 0x34, 0x35, 0x38, 0x39 };
constexpr int kKnockProbeN = (int)(sizeof(kKnockProbe) / sizeof(kKnockProbe[0]));

bool VerifyBytes(uintptr_t rva, const uint8_t* sig, size_t n) {
    uint8_t got[16] = {};
    return n <= sizeof(got) && SafeRead(got, (const void*)(g_base + rva), n) && memcmp(got, sig, n) == 0;
}

// --- float matrix helpers (4096 = 1.0) ----------------------------------------
struct FM { float r[3][3]; float t[3]; };
FM ToFM(const GameMatrix& g) {
    FM f;
    for (int i = 0; i < 3; ++i) { for (int j = 0; j < 3; ++j) f.r[i][j] = g.m[i][j] / 4096.0f; f.t[i] = (float)g.t[i]; }
    return f;
}
GameMatrix ToGM(const FM& f) {
    GameMatrix g{};
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            float v = f.r[i][j] * 4096.0f;
            if (v > 32767.f) v = 32767.f; if (v < -32768.f) v = -32768.f;
            g.m[i][j] = (int16_t)std::lround(v);
        }
        g.t[i] = (int32_t)std::lround(f.t[i]);
    }
    return g;
}
// a * b  (rotation and translation, world = parent * local)
FM Compose(const FM& a, const FM& b) {
    FM o;
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) o.r[i][j] = a.r[i][0] * b.r[0][j] + a.r[i][1] * b.r[1][j] + a.r[i][2] * b.r[2][j];
        o.t[i] = a.t[i] + a.r[i][0] * b.t[0] + a.r[i][1] * b.t[1] + a.r[i][2] * b.t[2];
    }
    return o;
}
FM InverseRigid(const FM& a) {
    FM o;
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) o.r[i][j] = a.r[j][i];
    for (int i = 0; i < 3; ++i) o.t[i] = -(o.r[i][0] * a.t[0] + o.r[i][1] * a.t[1] + o.r[i][2] * a.t[2]);
    return o;
}
V3 MatCol(const GameMatrix& g, int c) { return V3{ g.m[0][c] / 4096.0f, g.m[1][c] / 4096.0f, g.m[2][c] / 4096.0f }; }
V3 MatT(const GameMatrix& g) { return V3{ (float)g.t[0], (float)g.t[1], (float)g.t[2] }; }

// ---------------------------------------------------------------------------
// HAND JITTER MEASUREMENT (2026-10-04). While your right hand is still, a
// still hand should give a still gun IN THE GAME WORLD (the world is what the
// headset holds steady). Per game frame, frame-to-frame movement is measured
// at four points of the chain and logged as RMS every ~4 s of stillness:
//   controller : the raw tracked pose (what the headset reports)
//   filtered   : after the One Euro filter
//   gun        : the matrix the game draws the gun with, in game-world mm/deg
//   camera     : the head centre the gun hangs from (moves with your head)
// gun >> filtered means the jitter is added AFTER the filter (head/camera
// mapping); filtered ~ controller means the filter is not doing its job.
// ---------------------------------------------------------------------------
struct JitStat { double ss = 0, mx = 0; int n = 0;
    void Add(double v) { ss += v * v; if (v > mx) mx = v; ++n; }
    double Rms() const { return n ? std::sqrt(ss / n) : 0.0; }
    void Clear() { ss = mx = 0; n = 0; } };
struct {
    bool have = false;
    V3 raw{}, rawDir{}, filt{}, filtDir{}, gun{}, gunDir{}, cam{};
    JitStat raw_mm, raw_deg, filt_mm, filt_deg, gun_mm, gun_deg, cam_mm;
    int lines = 0;
} g_jit;
inline double AngDeg(V3 a, V3 b) {
    const float c = Dot(a, b) / std::fmax(1e-6f, Len(a) * Len(b));
    return std::acos(std::fmax(-1.0f, std::fmin(1.0f, c))) * 180.0 / kPi;
}
void HandJitterSample(const AimSnap& s, const GameMatrix& G, V3 cam) {
    if (!g_cfgHandJitterLog || !s.haveFilt) return;
    auto get = [](const float* a) { return V3{ a[0], a[1], a[2] }; };
    const bool still = Len(get(s.vpos)) < g_cfgStillMps && Len(get(s.vdir)) < g_cfgStillRads;
    const V3 raw = get(s.wpos), rawDir = get(s.wdir), filt = get(s.fwpos), filtDir = get(s.fwdir);
    const V3 gun = MatT(G), gunDir = Mul(MatCol(G, 1), -1.0f);
    const float mmPerUnit = 1000.0f / g_cfgUnitsPerMetre;
    if (still && g_jit.have) {
        g_jit.raw_mm.Add(Len(Sub(raw, g_jit.raw)) * 1000.0);
        g_jit.raw_deg.Add(AngDeg(rawDir, g_jit.rawDir));
        g_jit.filt_mm.Add(Len(Sub(filt, g_jit.filt)) * 1000.0);
        g_jit.filt_deg.Add(AngDeg(filtDir, g_jit.filtDir));
        g_jit.gun_mm.Add(Len(Sub(gun, g_jit.gun)) * mmPerUnit);
        g_jit.gun_deg.Add(AngDeg(gunDir, g_jit.gunDir));
        g_jit.cam_mm.Add(Len(Sub(cam, g_jit.cam)) * mmPerUnit);
        if (g_jit.gun_mm.n >= 120 && g_jit.lines < 60) {
            ++g_jit.lines;
            DebugLogger::LogFormat("HandJitter (hand still, %d frames, per game frame RMS / worst): "
                "controller %.2f/%.2f mm %.3f/%.3f deg | filtered %.2f/%.2f mm %.3f/%.3f deg | "
                "gun in game world %.2f/%.2f mm %.3f/%.3f deg | camera (head) %.2f/%.2f mm",
                g_jit.gun_mm.n,
                g_jit.raw_mm.Rms(), g_jit.raw_mm.mx, g_jit.raw_deg.Rms(), g_jit.raw_deg.mx,
                g_jit.filt_mm.Rms(), g_jit.filt_mm.mx, g_jit.filt_deg.Rms(), g_jit.filt_deg.mx,
                g_jit.gun_mm.Rms(), g_jit.gun_mm.mx, g_jit.gun_deg.Rms(), g_jit.gun_deg.mx,
                g_jit.cam_mm.Rms(), g_jit.cam_mm.mx);
            g_jit.raw_mm.Clear(); g_jit.raw_deg.Clear(); g_jit.filt_mm.Clear(); g_jit.filt_deg.Clear();
            g_jit.gun_mm.Clear(); g_jit.gun_deg.Clear(); g_jit.cam_mm.Clear();
        }
    }
    g_jit.have = true;
    g_jit.raw = raw; g_jit.rawDir = rawDir; g_jit.filt = filt; g_jit.filtDir = filtDir;
    g_jit.gun = gun; g_jit.gunDir = gunDir; g_jit.cam = cam;
}
void SetT(GameMatrix& g, V3 t) { g.t[0] = (int32_t)std::lround(t.x); g.t[1] = (int32_t)std::lround(t.y); g.t[2] = (int32_t)std::lround(t.z); }
int16_t Clamp16(float v) { if (v > 32767.f) v = 32767.f; if (v < -32768.f) v = -32768.f; return (int16_t)std::lround(v); }

// --- engine calls (game thread, act phase only) --------------------------------
struct SV { int16_t x, y, z, pad; };
typedef uint32_t(__cdecl* GetMapFn)(uint32_t mask);
typedef int(__cdecl* HzdTestFn)(uint32_t hzd, SV* from, SV* to, int flags, int mask);
typedef void(__cdecl* HzdHitFn)(SV* out);
typedef void(__cdecl* SeSetFn)(SV* pos, int id);
typedef void(__cdecl* SeSetModeFn)(SV* pos, int id, int mode);

int CallHzd(uint32_t hzd, SV* a, SV* b) {
    __try { return ((HzdTestFn)(g_base + kHzdLineTestRva))(hzd, a, b, 0xF, 4); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}
bool CallHit(SV* out) {
    __try { ((HzdHitFn)(g_base + kHzdHitPointRva))(out); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
uint32_t CurrentHzd() {
    uint32_t mask = 0, map = 0, hzd = 0;
    if (!ReadU32((uint32_t)(g_base + kCurMapRva), &mask) || !mask) return 0;
    __try { map = ((GetMapFn)(g_base + kGetMapRva))(mask); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    if (!map || !ReadU32(map + 8, &hzd)) return 0;
    return hzd;
}
// First surface on the segment a->b, or false.
bool LineHit(uint32_t hzd, V3 a, V3 b, V3* hit) {
    SV sa{ Clamp16(a.x), Clamp16(a.y), Clamp16(a.z), 0 }, sb{ Clamp16(b.x), Clamp16(b.y), Clamp16(b.z), 0 };
    const int r = CallHzd(hzd, &sa, &sb);
    if (r == -1) { h_disabledRuntime = true; DebugLogger::Log("Hands: HZD line test faulted -- wall collision off for this session."); return false; }
    if (r == 0) return false;
    SV h{};
    if (!CallHit(&h)) return false;
    h_lastHitAttr = 0;
    SafeRead(&h_lastHitAttr, (const void*)(g_base + kHitAttrRva), 2);
    *hit = V3{ (float)h.x, (float)h.y, (float)h.z };
    return true;
}

// SEH lives in its own function: MSVC refuses __try anywhere a std::string
// temporary (DebugLogger::Log) also lives (C2712).
bool CallSeSet(SV* pos, int id) {
    __try { ((SeSetFn)(g_base + kSeSetRva))(pos, id); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool CallSeSetMode(SV* pos, int id, int mode) {
    __try { ((SeSetModeFn)(g_base + kSeSetModeRva))(pos, id, mode); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// knock_sound_id: -1 = the game's own (wall material), -2 = old probe cycle,
// anything else = that id.
void DoKnock(V3 p, int hand, int16_t attr) {
    SV pos{ Clamp16(p.x), Clamp16(p.y), Clamp16(p.z), 0 };
    int id = h_knockSound;
    const bool material = (id == -1);
    if (material) {
        if ((attr & 0x700) >= 0x400) return;          // the game makes no knock on these surfaces
        uint8_t b = 0;
        SafeRead(&b, (const void*)(g_base + kMaterialSeRva + (uintptr_t)(((attr >> 8) & 3) * 3)), 1);
        if (!b) return;                                // the game skips id 0 too
        id = b;
    }
    else if (id < 0) id = kKnockProbe[(h_probeIdx++) % kKnockProbeN];
    const bool ok = material ? CallSeSetMode(&pos, id, 1) : CallSeSet(&pos, id);
    if (!ok) { h_knock = false; DebugLogger::Log("Hands: sound call faulted -- knock off."); return; }
    // GM_SetNoise, exactly as the game inlines it: only if at least as loud.
    int32_t power = 0, length = 0;
    SafeRead(&power, (const void*)(g_base + kNoisePowerRva), 4);
    SafeRead(&length, (const void*)(g_base + kNoiseLengthRva), 4);
    if (power < h_knockPower || (power == h_knockPower && length <= h_knockLength)) {
        const int32_t p2 = h_knockPower, l2 = h_knockLength;
        SafeWrite((void*)(g_base + kNoisePowerRva), &p2, 4);
        SafeWrite((void*)(g_base + kNoiseLengthRva), &l2, 4);
        SafeWrite((void*)(g_base + kNoisePosRva), &pos, 8);
    }
    ++h_knocks;
    g_knockRumble.fetch_or(hand ? 2 : 1, std::memory_order_relaxed);   // vr_input buzzes that hand
    if (h_knocks <= 30)
        DebugLogger::LogFormat("Knock[%u]: %s hand at (%d,%d,%d) surface 0x%04X sound id 0x%02X%s, noise power %d length %d",
            h_knocks, hand ? "left" : "right", pos.x, pos.y, pos.z, (unsigned)(uint16_t)attr, id,
            material ? " (the wall's own knock sound)" : (h_knockSound == -2 ? " (PROBE)" : ""),
            h_knockPower, h_knockLength);
}

// Head -> hand. If the line crosses a surface, pull the hand back onto it.
// Returns true while touching.
bool ClampToWorld(uint32_t hzd, V3 head, GameMatrix& M) {
    const V3 p = MatT(M);
    V3 d = Sub(p, head);
    const float len = Len(d);
    if (len < 1.0f || !Norm(d)) return false;
    // A touch is "at the surface or a little beyond": test slightly past the
    // hand so resting the hand ON the wall still counts as contact.
    const V3 probe = Add(p, Mul(d, h_margin));
    V3 hit;
    if (!LineHit(hzd, head, probe, &hit)) return false;
    float along = Dot(Sub(hit, head), d) - h_margin;
    if (along < 0.0f) along = 0.0f;
    if (along < len) SetT(M, Add(head, Mul(d, along)));
    return true;
}

// The body objs Snake draws, validated once per pointer: part count sane and
// the two arm chains exactly as the probe found them. Anything else and the
// hands stay off -- a wrong part list would show the wrong limbs.
bool ValidateBody(uint32_t objs, int* nOut) {
    int16_t n = 0;
    if (!SafeRead(&n, (const void*)(uintptr_t)(objs + 0x2E), 2) || n < 10 || n > 40) return false;
    *nOut = n;
    if (objs == h_validatedObjs) return true;
    static uint32_t rejected = 0;
    if (objs == rejected) return false;
    auto parentOf = [&](int i) -> int32_t {
        uint32_t mdl = 0; int32_t par = -99;
        if (i < 0 || i >= n) return -99;
        if (!ReadU32(objs + 0x48 + (uint32_t)i * 0x5C + 0x40, &mdl) || !mdl) return -99;
        SafeRead(&par, (const void*)(uintptr_t)(mdl + 0x2C), 4);
        return par;
    };
    const bool ok = parentOf(h_partR) == h_foreR && parentOf(h_partL) == h_foreL &&
                    parentOf(h_foreR) >= 0 && parentOf(h_foreL) >= 0;
    DebugLogger::LogFormat("Hands: Snake body %08X, %d parts. Right hand part %d (parent %d), left hand part %d "
        "(parent %d) -- %s", objs, (int)n, h_partR, parentOf(h_partR), h_partL, parentOf(h_partL),
        ok ? "arm chains MATCH, hands armed" : "MISMATCH, hands stay off (set hands_right_part/left_part)");
    if (!ok) { rejected = objs; return false; }
    for (int i = 0; i < 40; ++i) h_parent[i] = (i < n) ? parentOf(i) : -99;
    h_upR = parentOf(h_foreR); h_upL = parentOf(h_foreL);
    {
        char tree[256] = {}; int len = 0;
        for (int i = 0; i < n && len < 240; ++i)
            len += sprintf_s(tree + len, sizeof(tree) - len, "%d<%d ", i, (int)h_parent[i]);
        DebugLogger::LogFormat("FullBody: part tree (part<parent): %s| arms R %d->%d->%d, L %d->%d->%d | full_body=%d",
            tree, h_upR, h_foreR, h_partR, h_upL, h_foreL, h_partL, h_fullBody ? 1 : 0);
    }
    // Split the collapsed body between the two wrists. Parts that belong only
    // to the left arm (ancestors of the left hand that are not ancestors of the
    // right hand) sit on the left wrist; everything else on the right wrist.
    // Seam polygons then only ever join points that sit together, so nothing
    // stretches between the hands.
    memset(h_leftSide, 0, sizeof(h_leftSide));
    {
        bool rightAnc[40] = {};
        for (int i = h_partR, g = 0; i >= 0 && i < n && g < 40; i = parentOf(i), ++g) rightAnc[i] = true;
        for (int i = h_partL, g = 0; i >= 0 && i < n && g < 40; i = parentOf(i), ++g) {
            if (rightAnc[i]) break;
            h_leftSide[i] = 1;
        }
    }
    h_validatedObjs = objs;
    return true;
}

void ApplyHandOffset(GameMatrix& M, const float* off) {
    if (off[0] == 0 && off[1] == 0 && off[2] == 0) return;
    SetT(M, Add(MatT(M), Add(Add(Mul(MatCol(M, 0), off[0]), Mul(MatCol(M, 1), off[1])), Mul(MatCol(M, 2), off[2]))));
}

// Game thread, right after Snake's own act (before his weapon's, before render).
void HandsSnakePost(uint32_t work, uint32_t bodyOverride = 0) {
    // Scope up: no hands -- at the scope's zoom a hand 15 cm from your eye
    // would fill the lens. Round 10: also on top of REX (Snake's eyes, the
    // body passed in), where the game has no first person at all.
    const bool povBody = bodyOverride != 0;
    const bool gate = g_cfgEnabled && !IsScopeViewActive() &&
        (povBody ? (IsVrViewModeActive() && IsGameplayPovActive()) : GameplayGate());
    // (Round 11: NOT gated on IsCutsceneVrActive() -- on top of REX the game's
    // camera routine is stopped for the whole fight, so the stall detector
    // reads it as a "cutscene" all along: log 23:06, body armed, hands never
    // drawn. Snake's eyes already stands down for real cutscenes.)
    g_hf.active = false;
    g_hf.gunValid = false;

    uint32_t objs = bodyOverride;
    if (!objs) ReadU32(work + kSnaBodyObjsOff, &objs);
    int n = 0;
    const bool bodyOk = objs && ValidateBody(objs, &n);

    if (gate && (h_enabled || h_gunCollision || h_collision)) {
        // sG = the gun (and bullets). sR / sL = Snake's right / left hand
        // models. Right-handed: the gun IS the right hand. Left-handed: the
        // gun follows the left controller and travels with the LEFT hand,
        // while the right hand model takes the right controller's aim pose.
        AimSnap sG, sR, sL;
        AcquireSRWLockShared(&g_lock);
        sG = g_snap; sL = g_snapL;
        sR = g_leftHanded ? g_snapRH : g_snap;
        ReleaseSRWLockShared(&g_lock);
        const int gunHand = g_leftHanded ? 1 : 0;   // index into hands[] below

        V3 head{}, fwd{};
        GameMatrix R{}, L{};
        float reachR = 0, reachL = 0;
        const bool haveR = BuildControllerMatrix(sR, false, 0.0f, h_rotR, &R, &reachR, &head, &fwd);
        V3 headL{}, fwdL{};
        const bool haveL = BuildControllerMatrix(sL, false, 0.0f, h_rotL, &L, &reachL, &headL, &fwdL);
        if (!haveR && haveL) { head = headL; fwd = fwdL; }
        if (haveR) ApplyHandOffset(R, h_offR);
        if (haveL) ApplyHandOffset(L, h_offL);

        // The gun, built as it always was, then carried along by any
        // correction the right hand receives.
        GameMatrix G{}; float gReach = 0;
        const bool haveG = BuildControllerMatrix(sG, true, g_cfgGunRollDeg, nullptr, &G, &gReach, nullptr);
        if (haveG && haveR && !g_leftHanded) HandJitterSample(sG, G, head);

        const uint32_t hzd = (h_collision && !h_disabledRuntime) ? CurrentHzd() : 0;
        if (hzd) {
            const LONGLONG now = NowQpc();
            GameMatrix* hands[2] = { haveR ? &R : nullptr, haveL ? &L : nullptr };
            const AimSnap* snaps[2] = { &sR, &sL };
            for (int h = 0; h < 2; ++h) {
                if (!hands[h]) { h_contact[h] = false; continue; }
                const V3 before = MatT(*hands[h]);
                const bool touch = ClampToWorld(hzd, head, *hands[h]);
                const int16_t attr = h_lastHitAttr;
                if (touch) { ++h_contacts; h_touchTick[h].store(GetTickCount64(), std::memory_order_relaxed); }
                if (h == gunHand && haveG) SetT(G, Add(MatT(G), Sub(MatT(*hands[h]), before)));
                if (touch && !h_contact[h] && h_knock) {
                    const float* v = snaps[h]->wvel;
                    const float speed = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
                    const double since = (double)(now - h_lastKnockQpc[h]) * 1000.0 / (double)g_qpcFreq;
                    if (speed >= h_knockSpeed && since > h_knockCooldownMs) {
                        h_lastKnockQpc[h] = now;
                        DoKnock(MatT(*hands[h]), h, attr);
                    }
                }
                h_contact[h] = touch;
            }
            // Barrel: grip -> muzzle. If it crosses a wall, slide gun and hand
            // back along the barrel so the muzzle rests on the surface.
            if (h_gunCollision && haveG && IsGunInHandActive()) {
                const V3 fwd = Mul(MatCol(G, 1), -1.0f);
                const V3 grip = MatT(G);
                const V3 muzzle = Add(grip, Mul(fwd, h_gunLength));
                V3 hit;
                if (LineHit(hzd, grip, Add(muzzle, Mul(fwd, h_margin)), &hit)) {
                    const float along = Dot(Sub(hit, grip), fwd);
                    const float back = h_gunLength + h_margin - along;
                    if (back > 0.0f) {
                        const V3 shift = Mul(fwd, -back);
                        SetT(G, Add(grip, shift));
                        if (gunHand == 0 && haveR) SetT(R, Add(MatT(R), shift));
                        if (gunHand == 1 && haveL) SetT(L, Add(MatT(L), shift));
                        ++h_gunPushes;
                    }
                }
            }
            static unsigned lastRep = 0;
            if (h_contacts / 300 != lastRep) {
                lastRep = h_contacts / 300;
                DebugLogger::LogFormat("Hands: %u frames touching a surface, %u knocks, %u gun push-backs so far",
                    h_contacts, h_knocks, h_gunPushes);
            }
        }

        if (haveG) { g_hf.gunValid = true; g_hf.gun = G; g_hf.gunReach = gReach; }
        g_hf.haveR = haveR; g_hf.haveL = haveL;
        g_hf.R = R; g_hf.L = L;
        g_hf.haveHead = haveR || haveL; g_hf.head = head; g_hf.fwd = fwd;
        g_hf.bodyObjs = objs; g_hf.nParts = n;
        g_hf.active = h_hooked && h_enabled && bodyOk && (haveR || haveL);
        g_hf.qpc = NowQpc();

        if (g_hf.active && h_logs < h_logLines && (h_logs < 3 || (GetTickCount() % 1000) < 40)) {
            ++h_logs;
            DebugLogger::LogFormat("Hands[%d]: right %s (%d,%d,%d) left %s (%d,%d,%d) head (%.0f,%.0f,%.0f)", h_logs,
                haveR ? "ok" : "--", R.t[0], R.t[1], R.t[2], haveL ? "ok" : "--", L.t[0], L.t[1], L.t[2],
                head.x, head.y, head.z);
        }
    }

    // Visibility. Lift the "hidden in first person" bit only while the hands
    // are live, and put it back the moment they are not. Safety net: if the
    // renderer never reaches the body after we unhid it, give up for the
    // session rather than risk showing Snake's whole body around the camera.
    if (!objs) return;
    uint32_t flag = 0;
    if (!ReadU32(objs + kObjsFlag, &flag)) return;
    if (g_hf.active && !h_disabledRuntime) {
        if (!(flag & kHiddenBit) && povBody && h_unhidObjs != objs) {
            // Already visible (third person): just claim it, so the render-
            // only hide of Snake's eyes leaves it to the hands.
            h_unhidObjs = objs; h_unhideQpc = NowQpc(); h_unhidWasHidden = false;
            DebugLogger::LogFormat("Hands: on top of REX -- Snake's body %08X (%d parts) drawn as your hands", objs, n);
        }
        if (flag & kHiddenBit) {
            WriteU32(objs + kObjsFlag, flag & ~kHiddenBit);
            if (h_unhidObjs != objs) { h_unhidObjs = objs; h_unhideQpc = NowQpc(); h_unhidWasHidden = true; }
        }
        // Judge in FRAMES, not milliseconds (fixed 2026-09-26). The old test
        // was "no body seen for 500 ms while the renderer ran in the last
        // 100 ms", and ONE long game-thread stall tripped it: entering first
        // person starts render twice + GPU eye capture, the game thread paused
        // ~700 ms, the first screen pass after it came >100 ms after the act
        // and so did not count as seeing the body, and the very next act
        // switched the hands off for the whole session -- 0.7 s after they had
        // demonstrably worked ("render reached Snake's body" one frame
        // earlier). Now: count acts after which a screen pass ran but never
        // reached the body; only a long unbroken run of those is a failure.
        // Pauses and loading screens run no screen pass, so they count nothing.
        static LONGLONG s_lastJudgeQpc = 0;
        static int s_missedFrames = 0;
        static uint32_t s_judgedObjs = 0;
        if (s_judgedObjs != h_unhidObjs) { s_judgedObjs = h_unhidObjs; s_missedFrames = 0; }
        if (h_unhidObjs == objs && MsSince(h_unhideQpc) > 500.0 && s_lastJudgeQpc != 0) {
            const bool rendered = h_anyScreenQpc > s_lastJudgeQpc;
            const bool reached = h_seenQpc > s_lastJudgeQpc;
            if (reached) s_missedFrames = 0;
            else if (rendered) ++s_missedFrames;
        }
        s_lastJudgeQpc = NowQpc();
        if (h_unhidObjs == objs && s_missedFrames >= 45) {
            h_disabledRuntime = true; g_hf.active = false;
            WriteU32(objs + kObjsFlag, flag | kHiddenBit);
            DebugLogger::Log("Hands: the renderer never reached Snake's body through the hooked path -- hands "
                "OFF for this session and the body re-hidden. Please send mgs1_vr_debug.log.");
        }
    }
    else if (h_unhidObjs == objs) {
        if (h_unhidWasHidden && IsFpvActive()) WriteU32(objs + kObjsFlag, flag | kHiddenBit);
        h_unhidObjs = 0;
    }
}

bool HandsFreshGunMatrix(GameMatrix* M, float* reachM) {
    if (!g_hf.gunValid || MsSince(g_hf.qpc) > 25.0) return false;
    *M = g_hf.gun;
    if (reachM) *reachM = g_hf.gunReach;
    return true;
}

// Render: rewrite the body's part world matrices just before they become
// screen matrices. Runs once per render pass (twice per frame with render
// twice), always on the game thread.
// --- FULL BODY: two-bone arm IK in render scratch (2026-10-04, v2 2026-10-05) --
// v2, after the first headset test:
//  * "left elbow all twisted": each bone used to be re-aimed by its own
//    minimal rotation, so upper arm and forearm picked different twists and
//    the elbow seam wrung itself. Now both bones are placed by TWO vectors --
//    the bone axis and the elbow's hinge axis -- so the elbow bends exactly as
//    the model was built to bend. The hinge is learned from the animation
//    itself (whenever the stock pose bends an elbow, its axis is read in each
//    part's own frame), so no model axes are assumed.
//  * "the body is ahead of me when walking" (the cigarette effect: the body is
//    this frame's Snake, the eye is last frame's) and "clipping into his
//    body": the whole body is now anchored horizontally to the same head point
//    the hands hang from, with the neck body_back_cm behind it. Render only.
GameMatrix FromCols(V3 c0, V3 c1, V3 c2, V3 t) {
    GameMatrix g{};
    const V3 c[3] = { c0, c1, c2 };
    for (int k = 0; k < 3; ++k) {
        g.m[0][k] = Clamp16(c[k].x * 4096.0f); g.m[1][k] = Clamp16(c[k].y * 4096.0f); g.m[2][k] = Clamp16(c[k].z * 4096.0f);
    }
    SetT(g, t);
    return g;
}
// world -> part-local direction: R^T v
V3 ToLocal(const GameMatrix& g, V3 v) { return V3{ Dot(MatCol(g, 0), v), Dot(MatCol(g, 1), v), Dot(MatCol(g, 2), v) }; }
V3 RotAxis(V3 v, V3 a, float ang) {   // Rodrigues, a unit
    const float c = std::cos(ang), s = std::sin(ang);
    return Add(Add(Mul(v, c), Mul(Cross(a, v), s)), Mul(a, Dot(a, v) * (1.0f - c)));
}

// Minimal re-aim (fallback until a hinge has been learned).
GameMatrix AimPart(const GameMatrix& src, V3 from, V3 to, V3 newOrigin) {
    GameMatrix g = src;
    if (Norm(from) && Norm(to)) {
        for (int c = 0; c < 3; ++c) {
            const V3 v = RotateMinimal(MatCol(src, c), from, to, V3{ 0, 1, 0 });
            g.m[0][c] = Clamp16(v.x * 4096.0f); g.m[1][c] = Clamp16(v.y * 4096.0f); g.m[2][c] = Clamp16(v.z * 4096.0f);
        }
    }
    SetT(g, newOrigin);
    return g;
}

// Place a part so its local bone axis bL lands on world b1 and its local hinge
// hL on world n1. P = [b1 n1 b1xn1] * [bL hL bLxhL]^T.
bool AlignPart(V3 bL, V3 hL, V3 b1, V3 n1, V3 origin, GameMatrix* out) {
    if (!Norm(bL) || !Norm(b1)) return false;
    hL = Sub(hL, Mul(bL, Dot(hL, bL))); if (!Norm(hL)) return false;
    n1 = Sub(n1, Mul(b1, Dot(n1, b1))); if (!Norm(n1)) return false;
    const V3 zL = Cross(bL, hL), z1 = Cross(b1, n1);
    // column k of P = b1*bL[k] + n1*hL[k] + z1*zL[k]
    const float bl[3] = { bL.x, bL.y, bL.z }, hl[3] = { hL.x, hL.y, hL.z }, zl[3] = { zL.x, zL.y, zL.z };
    V3 col[3];
    for (int k = 0; k < 3; ++k) col[k] = Add(Add(Mul(b1, bl[k]), Mul(n1, hl[k])), Mul(z1, zl[k]));
    *out = FromCols(col[0], col[1], col[2], origin);
    return true;
}

struct ArmHinge { bool learned = false; V3 up{}, fore{}; unsigned samples = 0; };
ArmHinge h_hinge[2];
float h_twistShare = 0.5f;       // [hands] forearm_twist_percent
float h_bodyBack = 300.0f;       // [hands] body_back_cm, converted to game units
float h_bodyDown = 0.0f;         // [hands] body_down_cm
bool  h_bodyAnchor = true;       // [hands] body_follow_head
int   h_fwdSign = 0;             // body forward = sign * cross(up, out), locked once
unsigned h_anchorSkips = 0;

// One arm: shoulder S (animated), target wrist T (controller hand).
bool SolveArm(const GameMatrix* W, GameMatrix* out, int up, int fore, int hand, V3 T, V3 pole, int side,
              const GameMatrix& handNew) {
    const V3 S = MatT(W[up]), E0 = MatT(W[fore]), H0 = MatT(W[hand]);
    const V3 u0 = Sub(E0, S), f0 = Sub(H0, E0);
    const float a = Len(u0), b = Len(f0);
    if (a < 1.0f || b < 1.0f) return false;

    // Learn the elbow hinge whenever the animation bends this elbow.
    {
        V3 n0 = Cross(u0, f0);
        const float sinBend = Len(n0) / (a * b);
        if (sinBend > 0.26f && Norm(n0)) {          // > ~15 degrees
            ArmHinge& hg = h_hinge[side];
            const V3 lu = ToLocal(W[up], n0), lf = ToLocal(W[fore], n0);
            if (!hg.learned) { hg.up = lu; hg.fore = lf; hg.learned = true;
                DebugLogger::LogFormat("FullBody: %s elbow hinge learned from the animation (bend %.0f deg)",
                    side ? "left" : "right", std::asin(std::fmin(1.0f, sinBend)) * 180.0f / kPi); }
            else { hg.up = Add(Mul(hg.up, 0.95f), Mul(lu, 0.05f)); hg.fore = Add(Mul(hg.fore, 0.95f), Mul(lf, 0.05f));
                   Norm(hg.up); Norm(hg.fore); }
            ++hg.samples;
        }
    }

    V3 d = Sub(T, S);
    float L = Len(d);
    if (!Norm(d)) return false;
    V3 p = Sub(pole, Mul(d, Dot(pole, d)));
    if (!Norm(p)) { p = Cross(d, V3{ 0, 1, 0 }); if (!Norm(p)) p = V3{ 1, 0, 0 }; }
    V3 E;
    if (L >= a + b - 0.5f) {
        E = Add(S, Mul(d, L * a / (a + b)));        // straight, gap shared along the arm
        ++h_ikOverReach;
    }
    else {
        const float minL = std::fabs(a - b) + 1.0f;
        if (L < minL) L = minL;
        const float x = (a * a - b * b + L * L) / (2.0f * L);
        const float h = std::sqrt(std::fmax(0.0f, a * a - x * x));
        E = Add(Add(S, Mul(d, x)), Mul(p, h));
    }
    // Hinge of the solved arm. cross(upper, fore) = -h*L*cross(d, p), i.e.
    // along cross(p, d) -- defined even when the arm is straight.
    const V3 n1 = Cross(p, d);

    const ArmHinge& hg = h_hinge[side];
    bool aligned = false;
    if (hg.learned) {
        GameMatrix U{}, F{};
        aligned = AlignPart(ToLocal(W[up], u0), hg.up, Sub(E, S), n1, S, &U) &&
                  AlignPart(ToLocal(W[fore], f0), hg.fore, Sub(T, E), n1, E, &F);
        if (aligned) {
            // Share the hand's roll with the forearm: twist it about its own
            // axis by part of the gap between the hand you hold and the hand
            // the forearm would carry.
            if (h_twistShare != 0.0f) {
                V3 ax = Sub(T, E);
                if (Norm(ax)) {
                    // D = Hheld * Hcarried^T, Hcarried = Fnew * Wf^T * Wh;
                    // apply D to a vector perpendicular to the forearm.
                    V3 perp = n1; perp = Sub(perp, Mul(ax, Dot(perp, ax))); Norm(perp);
                    const V3 inHandOld = ToLocal(W[hand], Add(Add(Mul(MatCol(W[fore], 0), ToLocal(F, perp).x),
                        Mul(MatCol(W[fore], 1), ToLocal(F, perp).y)), Mul(MatCol(W[fore], 2), ToLocal(F, perp).z)));
                    const V3 actual = Add(Add(Mul(MatCol(handNew, 0), inHandOld.x), Mul(MatCol(handNew, 1), inHandOld.y)),
                        Mul(MatCol(handNew, 2), inHandOld.z));
                    V3 ap = Sub(actual, Mul(ax, Dot(actual, ax)));
                    if (Norm(ap)) {
                        const float ang = std::atan2(Dot(Cross(perp, ap), ax), Dot(perp, ap)) * h_twistShare;
                        F = FromCols(RotAxis(MatCol(F, 0), ax, ang), RotAxis(MatCol(F, 1), ax, ang),
                                     RotAxis(MatCol(F, 2), ax, ang), E);
                    }
                }
            }
            out[up] = U; out[fore] = F;
        }
    }
    if (!aligned) {
        out[up] = AimPart(W[up], u0, Sub(E, S), S);
        out[fore] = AimPart(W[fore], f0, Sub(T, E), E);
    }
    if (h_ikLogs < h_ikLogLines && (h_ikFrames % 90) == 0) {
        ++h_ikLogs;
        DebugLogger::LogFormat("FullBody[%u] %s arm: upper %.0f forearm %.0f (reach %.0f) | shoulder (%.0f,%.0f,%.0f) "
            "-> hand (%.0f,%.0f,%.0f) dist %.0f%s | elbow (%.0f,%.0f,%.0f) %s | over-reach frames so far %u",
            h_ikFrames, side ? "left" : "right", a, b, a + b, S.x, S.y, S.z, T.x, T.y, T.z, Len(Sub(T, S)),
            Len(Sub(T, S)) > a + b ? " OUT OF REACH" : "", E.x, E.y, E.z,
            aligned ? "hinge-aligned" : "minimal (hinge not learned yet)", h_ikOverReach);
    }
    return true;
}

// Whole body in its animated pose; anchored under the head; head + neck
// folded away; arms solved.
void FullBodyApply(const GameMatrix* Win, GameMatrix* out, int n) {
    GameMatrix W[40];
    for (int i = 0; i < n; ++i) W[i] = Win[i];
    ++h_ikFrames;

    const bool armsOk = h_upR >= 0 && h_upR < n && h_upL >= 0 && h_upL < n &&
                        h_foreR < n && h_foreL < n && h_partR < n && h_partL < n;

    // Body frame from the animation itself: "up" is the root part to the
    // middle of the shoulders, "out" is shoulder to shoulder.
    int root = 0;
    for (int i = 0; i < n; ++i) if (h_parent[i] < 0) { root = i; break; }
    V3 up{ 0, 1, 0 }, outR{ 1, 0, 0 };
    V3 neck = MatT(W[root]);
    if (armsOk) {
        const V3 sR = MatT(W[h_upR]), sL = MatT(W[h_upL]);
        neck = Mul(Add(sR, sL), 0.5f);
        up = Sub(neck, MatT(W[root]));
        if (!Norm(up)) up = V3{ 0, 1, 0 };
        outR = Sub(sR, sL);
        outR = Sub(outR, Mul(up, Dot(outR, up)));
        if (!Norm(outR)) outR = V3{ 1, 0, 0 };
    }
    if (h_hidePartsN > 0 && h_hideParts[0] >= 0 && h_hideParts[0] < n) neck = MatT(W[h_hideParts[0]]);

    // Anchor: neck body_back_cm behind the head point the hands hang from,
    // horizontally (x/z); height stays the animation's (minus body_down_cm).
    if (h_bodyAnchor && g_hf.haveHead && armsOk) {
        V3 c = Cross(up, outR); c.y = 0.0f;
        V3 fh = g_hf.fwd; fh.y = 0.0f;
        if (Norm(c) && Norm(fh)) {
            if (h_fwdSign == 0 && std::fabs(Dot(c, fh)) > 0.8f) {
                h_fwdSign = Dot(c, fh) > 0 ? 1 : -1;
                DebugLogger::LogFormat("FullBody: body forward locked (sign %d) -- neck kept %.0f units behind your head",
                    h_fwdSign, h_bodyBack);
            }
            const V3 bodyFwd = Mul(c, (float)(h_fwdSign ? h_fwdSign : (Dot(c, fh) > 0 ? 1 : -1)));
            const V3 target = Sub(g_hf.head, Mul(bodyFwd, h_bodyBack));
            V3 delta{ target.x - neck.x, -h_bodyDown, target.z - neck.z };
            const float dl = std::sqrt(delta.x * delta.x + delta.z * delta.z);
            if (dl < 1500.0f) {
                for (int i = 0; i < n; ++i) SetT(W[i], Add(MatT(W[i]), delta));
                neck = Add(neck, delta);
            }
            else if (++h_anchorSkips <= 5)
                DebugLogger::LogFormat("FullBody: anchor skipped -- body %.0f units from your head (camera elsewhere?)", dl);
            if (h_ikLogs < h_ikLogLines && (h_ikFrames % 90) == 45) {
                ++h_ikLogs;
                DebugLogger::LogFormat("FullBody[%u] anchor: moved body by (%.0f,%.0f,%.0f) = %.0f units horizontally "
                    "(animation vs head lead)", h_ikFrames, delta.x, delta.y, delta.z, dl);
            }
        }
    }

    for (int i = 0; i < n; ++i) out[i] = W[i];

    // Head and neck: every vertex onto the base of the neck, a point.
    if (h_hidePartsN > 0 && h_hideParts[0] >= 0 && h_hideParts[0] < n) {
        GameMatrix z{};
        SetT(z, MatT(W[h_hideParts[0]]));
        for (int k = 0; k < h_hidePartsN; ++k)
            if (h_hideParts[k] >= 0 && h_hideParts[k] < n) out[h_hideParts[k]] = z;
    }
    if (!armsOk) return;
    const V3 down = Mul(up, -1.0f);

    // A hand with no tracking keeps its animated arm and hand.
    if (g_hf.haveR) {
        out[h_partR] = g_hf.R;
        SolveArm(W, out, h_upR, h_foreR, h_partR, MatT(g_hf.R), Add(down, Mul(outR, h_elbowOut)), 0, g_hf.R);
    }
    if (g_hf.haveL) {
        out[h_partL] = g_hf.L;
        SolveArm(W, out, h_upL, h_foreL, h_partL, MatT(g_hf.L), Add(down, Mul(outR, -h_elbowOut)), 1, g_hf.L);
    }
}

void HandsApplyToScratch(uint32_t objs, int n) {
    GameMatrix* scratch = (GameMatrix*)(g_base + kPartWorldScratch);
    if (n > 40) n = 40;
    GameMatrix W[40];
    if (!SafeRead(W, scratch, sizeof(GameMatrix) * (size_t)n)) return;
    if (h_fullBody) {
        GameMatrix fb[40];
        FullBodyApply(W, fb, n);
        (void)objs;
        SafeWrite(scratch, fb, sizeof(GameMatrix) * (size_t)n);   // scratch ONLY, never obj+0
        return;
    }
    const V3 anchorR = g_hf.haveR ? MatT(g_hf.R) : MatT(g_hf.L);
    const V3 anchorL = g_hf.haveL ? MatT(g_hf.L) : MatT(g_hf.R);
    GameMatrix out[40];
    for (int i = 0; i < n; ++i) {
        GameMatrix m{};
        // Zero rotation: every vertex of the part lands on one point, the
        // wrist of the hand whose side it belongs to.
        SetT(m, h_leftSide[i] ? anchorL : anchorR);
        out[i] = m;
    }
    if (g_hf.haveR && h_partR < n) out[h_partR] = g_hf.R;
    if (g_hf.haveL && h_partL < n) out[h_partL] = g_hf.L;
    if (h_forearms) {
        // Keep each forearm where the current animation holds it relative to
        // its hand: F' = H' * inverse(H) * F.
        const int pairs[2][2] = { { h_partR, h_foreR }, { h_partL, h_foreL } };
        const bool have[2] = { g_hf.haveR, g_hf.haveL };
        const GameMatrix* Hn[2] = { &g_hf.R, &g_hf.L };
        for (int k = 0; k < 2; ++k) {
            const int hp = pairs[k][0], fp = pairs[k][1];
            if (!have[k] || hp >= n || fp >= n || fp < 0) continue;
            const FM local = Compose(InverseRigid(ToFM(W[hp])), ToFM(W[fp]));
            out[fp] = ToGM(Compose(ToFM(*Hn[k]), local));
        }
    }
    // Scratch ONLY. The parts' own world matrices (obj+0) must stay untouched:
    // the first-person camera reads Snake's eye height from the head part
    // there, and writing the collapsed pose into it dropped the camera (and
    // then Snake) through the floor.
    (void)objs;
    SafeWrite(scratch, out, sizeof(GameMatrix) * (size_t)n);
}

int g_throwLogs = 0;

// ---------------------------------------------------------------------------
// KEEP SNAKE "HIDDEN" WHILE THE OTHER ACTORS RUN (fixed 2026-09-27)
//
// Snake's equipment decides first person by mirroring his body's hidden bit,
// read out of mgsi.exe:
//   gasmask.c act 0x5BC58A: mask objs hidden = [[w+0x48]]+0x28 & 0x80, and the
//                           first-person mask overlay (gmsight, 0x5BB23E) is
//                           only created while that bit is set
//   goggle.c  act 0x628F1E: goggle objs hidden = the same bit
//   box.c     act 0x5BC718: bit set -> box model hidden + the inside-the-box
//                           first-person view (0x44313D, 0xE2A9); bit clear ->
//                           box model shown (around the camera, so unseen)
// The hands lift that bit so the renderer draws the body -- and every one of
// those acts then decided Snake was in third person: the NVG/gas mask head was
// drawn around the camera ("floating head") and the box's first-person view
// never started.
//
// So the bit is lifted only for the RENDERER. Before every actor's act (except
// the two DG frame actors, which sort and draw) it goes back on; after the act
// it comes off again. Every game actor sees exactly what the stock game shows
// it in first person; only the draw sees the hands.
constexpr uintptr_t kDgStartFrameActRva = 0x001234;   // registered at +11C8, calls DG_StartFrame +1C02
constexpr uintptr_t kDgEndFrameActRva   = 0x0012ED;   // registered at +11E6 (class 8), jmp DG_EndFrame +1C15
uint32_t h_actRehidObjs = 0;                          // body we re-hid for the act in progress
unsigned h_actRehides = 0;

bool HandsBodyUnhiddenNow() {
    return g_hf.active && !h_disabledRuntime && h_unhidObjs && h_unhidObjs == g_hf.bodyObjs &&
           MsSince(g_hf.qpc) < 250.0;
}

// ---------------------------------------------------------------------------
// RENDER-ONLY HIDES + DEMO LETTERBOX + ITEM USE (2026-10-02)
// ---------------------------------------------------------------------------
// Render-only hide: Snake's body during gameplay Snake's eyes, Snake's demo
// model during cutscene Snake's eyes. Set just before each DG frame actor
// (the two that sort and draw) and cleared right after it, so no game actor
// ever sees the flag changed -- the same rule the hands follow.
uint32_t rh_hid[2] = { 0, 0 };

void RenderHidesPre() {
    for (int slot = 0; slot < 2; ++slot) {
        rh_hid[slot] = 0;
        const uint32_t objs = GetRenderHideObjs(slot);
        if (!objs || objs < 0x00400000u || objs >= 0x7FFF0000u) continue;
        if (slot == 0 && HandsBodyUnhiddenNow() && objs == h_unhidObjs) continue;   // the hands own it
        uint32_t flag = 0;
        if (!ReadU32(objs + kObjsFlag, &flag) || (flag & kHiddenBit)) continue;
        if (WriteU32(objs + kObjsFlag, flag | kHiddenBit)) {
            rh_hid[slot] = objs;
            static int s_log[2] = { 0, 0 };
            if (s_log[slot] < 2) {
                s_log[slot]++;
                DebugLogger::LogFormat("Snake's eyes: Snake's own %s hidden for the renderer only (DG_OBJS %08X)",
                    slot == 0 ? "body" : "cutscene model", objs);
            }
        }
    }
}

void RenderHidesPost() {
    for (int slot = 0; slot < 2; ++slot) {
        const uint32_t objs = rh_hid[slot];
        if (!objs) continue;
        rh_hid[slot] = 0;
        uint32_t flag = 0;
        if (ReadU32(objs + kObjsFlag, &flag) && (flag & kHiddenBit))
            WriteU32(objs + kObjsFlag, flag & ~kHiddenBit);
    }
}

// Demo letterbox (Takabe\cinema.c, act +2259C2). Its init (+225BEB) builds
// the bars once into a buffer at work+0x30: per OT buffer (stride 0x48) a
// POLY_G4 top bar at +0x10 (y 0..24) and bottom bar at +0x34 (y 224..184),
// plus solid TILEs at +0xA0/+0xC0 (top, h 24) and +0xB0/+0xD0 (bottom, h 40).
// Hiding collapses them to zero height; showing writes the stock values back.
// Only values that are one of the two known states are ever touched.
constexpr uintptr_t kCinemaActRva = 0x2259C2;

// PSG1 scope overlay (2026-10-02). Equipping the PSG1 makes Weapon\rfsight.c
// spawn Thing\sight.c overlays (act +431B5) tied to the flag word 0x78A9A4 --
// the game's full-screen scope reticle, drawn at once in first person. The
// mod's own "raise to eye" scope is the only one wanted, so while it is DOWN
// the overlay is kept off the screen: sight.c skips linking its prims when
// GM_PlayerStatus has 0x04000000 (byte 0x9942AB bit 0x04, tested at +433E3
// and +43436). That bit is set for the sight's own act only and put back
// straight after, so nothing else in the game ever sees it.
constexpr uintptr_t kSightActRva = 0x0431B5;
constexpr uintptr_t kRfSightFlagRva = 0x38A9A4;
constexpr uintptr_t kPlayerStatusB3Rva = 0x5942AB;
int8_t   ps_sightSaved = -1;      // original byte while we hold it, else -1
unsigned ps_sightHidden = 0;
bool PsgSightWantHidden(uint32_t work) {
    uint32_t flagPtr = 0;
    if (!ReadU32(work + 0x24, &flagPtr) || flagPtr != (uint32_t)(g_base + kRfSightFlagRva)) return false;
    return IsVrViewModeActive() && IsFpvActive() && !IsScopeViewActive() && !IsCutsceneVrActive();
}
void CinemaBars(uint32_t work, bool hide, bool hideBottom) {
    static int s_logged = 0;
    static bool s_lastHide = false;
    uint32_t buf = 0;
    if (!ReadU32(work + 0x30, &buf) || buf < 0x00400000u || buf >= 0x7FFF0000u) return;
    auto set16 = [](uint32_t a, int16_t shown, int16_t hidden, bool h) {
        int16_t v = 0;
        if (!SafeRead(&v, (const void*)(uintptr_t)a, 2)) return;
        if (v != shown && v != hidden) return;
        const int16_t want = h ? hidden : shown;
        if (v != want) SafeWrite((void*)(uintptr_t)a, &want, 2);
    };
    for (int k = 0; k < 2; ++k) {
        const uint32_t top = buf + k * 0x48 + 0x10, bot = buf + k * 0x48 + 0x34;
        set16(top + 0x1A, 0x18, 0x00, hide); set16(top + 0x22, 0x18, 0x00, hide);
        set16(bot + 0x0A, 0xE0, 0xB8, hideBottom); set16(bot + 0x12, 0xE0, 0xB8, hideBottom);
        set16(buf + 0xA0 + k * 0x20 + 0x0E, 0x18, 0x00, hide);
        set16(buf + 0xB0 + k * 0x20 + 0x0E, 0x28, 0x00, hideBottom);
    }
    if (hide != s_lastHide && s_logged < 6) {
        s_logged++;
        DebugLogger::LogFormat("Cutscene letterbox: demo black bars %s (cinema task %08X)",
            hide ? "REMOVED while you watch through Snake's eyes" : "restored", work);
    }
    s_lastHide = hide;
}

// Item use from the wrist list (2026-10-02). Disassembly: the item window's
// "use" is +6A916(list, press) -- called from the open item menu (+6A2A9)
// with the pad press; bit 0x20 (circle = ACTION) uses the selected entry:
// ration / medicine / diazepam effects, LIFE clamp, sound, count -1. It reads
// only list+4 (index) and list+0x20+index*8 (item id, count), so a one-entry
// list built here is everything it needs. Called on the game thread, right
// after Snake's act, never from the XR thread.
constexpr uintptr_t kItemUseRva = 0x06A916;
constexpr uintptr_t kLinkvarRva = 0x38E7E0;
std::atomic<int> g_itemUseReq{ 0 };     // item id + 1, 0 = none
bool g_itemUseVerified = false, g_itemUseChecked = false;
int16_t g_itemUseList[0x30 / 2] = {};

bool CallItemUse(void* list, uint32_t press) {
    typedef void(__cdecl* ItemUseFn)(void*, uint32_t);
    __try { reinterpret_cast<ItemUseFn>(g_base + kItemUseRva)(list, press); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Cigarettes (Equip\tabako.c, 2026-10-02 round 6): "the ciggie was a bit
// ahead of my head when running/walking". The cigarette is drawn from this
// frame's head joint, the first-person eye from where Snake was a frame
// earlier, so at a run it leads you by one frame of movement. At draw time
// its part matrix is pulled back by Snake's last per-frame step times
// cigarette_lag_percent/100 ([motion_aim], 0 = off, negative pushes it forward).
// LIFE drain is the game's own: tabako's act takes 1 LIFE every 64 frames
// (about 2 s) while LIFE > 1 -- slow by design; logged so it can be checked.
uint32_t cg_objs = 0;
LONGLONG cg_qpc = 0;
int32_t  sn_prev[3] = {}, sn_delta[3] = {};
bool     sn_havePrev = false;
float    g_cfgCigLag = 1.0f;
void SnakeStepTrack(uint32_t work) {
    int16_t p[3] = {};
    if (!SafeRead(p, (const void*)(uintptr_t)(work + 0x20), 6)) { sn_havePrev = false; return; }
    if (sn_havePrev) {
        int32_t d[3] = { p[0] - sn_prev[0], p[1] - sn_prev[1], p[2] - sn_prev[2] };
        const bool jump = std::abs(d[0]) > 400 || std::abs(d[1]) > 400 || std::abs(d[2]) > 400;
        for (int k = 0; k < 3; ++k) sn_delta[k] = jump ? 0 : d[k];
    }
    for (int k = 0; k < 3; ++k) sn_prev[k] = p[k];
    sn_havePrev = true;
}
void CigarettePost(uint32_t work) {
    uint32_t objs = 0;
    if (work && ReadU32(work + 0x20, &objs) && objs >= 0x00400000u && objs < 0x7FFF0000u) {
        cg_objs = objs;
        cg_qpc = NowQpc();
    }
    static DWORD s_lastLog = 0;
    static int s_logs = 0;
    const DWORD now = GetTickCount();
    if (s_logs < 30 && now - s_lastLog > 15000) {
        s_lastLog = now;
        s_logs++;
        int16_t life = 0, lifeMax = 0;
        SafeRead(&life, (const void*)(g_base + 0x38E7E0 + 11 * 2), 2);
        SafeRead(&lifeMax, (const void*)(g_base + 0x38E7E0 + 12 * 2), 2);
        DebugLogger::LogFormat("Cigarettes: lit, LIFE %d / %d (the game takes 1 every 64 frames while above 1)",
            (int)life, (int)lifeMax);
    }
}
bool CigaretteShiftNow(uint32_t objs) {
    return g_cfgCigLag != 0.0f && objs == cg_objs && cg_objs != 0 && MsSince(cg_qpc) < 250.0 &&
           IsVrViewModeActive() && IsFpvActive() && !IsGameplayPovActive() && !IsCutsceneVrActive();
}

void ItemUseOnGameThread() {
    const int req = g_itemUseReq.exchange(0, std::memory_order_relaxed);
    if (req <= 0 || !g_base) return;
    const int id = req - 1;
    if (!g_itemUseChecked) {
        g_itemUseChecked = true;
        static const uint8_t kSig[9] = { 0xF6, 0x44, 0x24, 0x08, 0x20, 0x56, 0x57, 0x0F, 0x84 };
        uint8_t got[9] = {};
        g_itemUseVerified = SafeRead(got, (const void*)(g_base + kItemUseRva), 9) && memcmp(got, kSig, 9) == 0;
        DebugLogger::LogFormat("Item use: game's item-window use routine at mgsi.exe+6A916 %s",
            g_itemUseVerified ? "verified" : "SIGNATURE MISMATCH -- item use from the wrist disabled");
    }
    if (!g_itemUseVerified || id < 0 || id >= 24) return;
    int16_t count = -1, lifeBefore = 0, lifeAfter = 0;
    SafeRead(&count, (const void*)(g_base + kLinkvarRva + (37 + id) * 2), 2);
    SafeRead(&lifeBefore, (const void*)(g_base + kLinkvarRva + 11 * 2), 2);
    if (count <= 0) {
        DebugLogger::LogFormat("Item use: item %d has none left (count %d) -- nothing used", id, (int)count);
        return;
    }
    memset(g_itemUseList, 0, sizeof(g_itemUseList));
    g_itemUseList[0x20 / 2] = (int16_t)id;     // list+0x20: id
    g_itemUseList[0x22 / 2] = count;           // list+0x22: count; list+4 index = 0
    const bool ok = CallItemUse(g_itemUseList, 0x20);
    int16_t countAfter = -1;
    SafeRead(&countAfter, (const void*)(g_base + kLinkvarRva + (37 + id) * 2), 2);
    SafeRead(&lifeAfter, (const void*)(g_base + kLinkvarRva + 11 * 2), 2);
    DebugLogger::LogFormat("Item use: item %d used through the game's own routine%s -- count %d -> %d, LIFE %d -> %d",
        id, ok ? "" : " (FAULTED)", (int)count, (int)countAfter, (int)lifeBefore, (int)lifeAfter);
}

} // namespace

// Runs before EVERY actor's act, on the game thread.
// Crash report breadcrumbs (2026-10-05). The game actor whose act is running
// right now (0 between acts), so a crash inside the game's own code can name
// the actor it happened in. Two plain stores per act -- nothing else.
volatile uint32_t g_crashActFn = 0, g_crashActWork = 0;

extern "C" void __cdecl MotionAimActPre(uint32_t fn, uint32_t work) {
    g_crashActFn = fn; g_crashActWork = work;
    h_actRehidObjs = 0;
    if (fn == (uint32_t)(g_base + kDgStartFrameActRva) || fn == (uint32_t)(g_base + kDgEndFrameActRva))
        RenderHidesPre();
    else if (fn == (uint32_t)(g_base + kCinemaActRva) && work)
        CinemaBars(work, WantDemoLetterboxHidden(), WantDemoBottomBarHidden());
    else if (fn == (uint32_t)(g_base + kSightActRva) && work && PsgSightWantHidden(work)) {
        uint8_t b = 0;
        if (SafeRead(&b, (const void*)(g_base + kPlayerStatusB3Rva), 1) && !(b & 0x04)) {
            const uint8_t nb = (uint8_t)(b | 0x04);
            if (SafeWrite((void*)(g_base + kPlayerStatusB3Rva), &nb, 1)) {
                ps_sightSaved = (int8_t)b;
                if (++ps_sightHidden == 1)
                    DebugLogger::Log("PSG1: the game's own scope overlay is kept off the screen until you raise "
                        "the rifle to your eye (renderer-side only)");
            }
        }
    }
    if (!HandsBodyUnhiddenNow()) return;
    if (fn == (uint32_t)(g_base + kDgStartFrameActRva) || fn == (uint32_t)(g_base + kDgEndFrameActRva)) return;
    // Cigarettes (Equip\tabako.c, act +1BC914) hide the cigarette whenever the
    // body reads hidden -- the stock first-person rule. In VR the hands make
    // the body visible to the renderer anyway, so let the cigarette see that
    // too: it is drawn at Snake's mouth, just under your view, smoke and all.
    if (fn == (uint32_t)(g_base + 0x1BC914)) return;
    if (!h_unhidWasHidden) return;    // REX top: the body is visible to the game anyway
    uint32_t flag = 0;
    if (!ReadU32(h_unhidObjs + kObjsFlag, &flag) || (flag & kHiddenBit)) return;
    if (!WriteU32(h_unhidObjs + kObjsFlag, flag | kHiddenBit)) return;
    h_actRehidObjs = h_unhidObjs;
    if (++h_actRehides == 1)
        DebugLogger::Log("Hands: Snake's body now reads as hidden to every actor (gas mask, goggles, box see "
            "first person as in the stock game); only the renderer sees it unhidden.");
}

// Runs after EVERY actor's act, on the game thread. Must stay cheap: one
// compare against five addresses for everything that is not a gun.
extern "C" void __cdecl MotionAimActPost(uint32_t fn, uint32_t work) {
    g_crashActFn = 0; g_crashActWork = 0;
    if (ps_sightSaved != -1 && fn == (uint32_t)(g_base + kSightActRva)) {
        uint8_t cur = 0;
        if (SafeRead(&cur, (const void*)(g_base + kPlayerStatusB3Rva), 1)) {
            const uint8_t restored = (uint8_t)((cur & ~0x04) | ((uint8_t)ps_sightSaved & 0x04));
            SafeWrite((void*)(g_base + kPlayerStatusB3Rva), &restored, 1);
        }
        ps_sightSaved = -1;
    }
    if (fn == (uint32_t)(g_base + kDgStartFrameActRva) || fn == (uint32_t)(g_base + kDgEndFrameActRva))
        RenderHidesPost();
    // Undo the pre-act re-hide first, so the renderer (and HandsSnakePost's
    // own bookkeeping) sees the body exactly as the hands left it.
    if (h_actRehidObjs) {
        const uint32_t body = h_actRehidObjs;
        h_actRehidObjs = 0;
        uint32_t flag = 0;
        if (HandsBodyUnhiddenNow() && body == h_unhidObjs && ReadU32(body + kObjsFlag, &flag) && (flag & kHiddenBit))
            WriteU32(body + kObjsFlag, flag & ~kHiddenBit);
    }
    if (fn == (uint32_t)(g_base + 0x1BC914)) { CigarettePost(work); return; }
    {
        uint32_t rexObj = 0, rexObjs = 0;
        if (work && GetGameplayPovRexBody(&rexObj, &rexObjs) && work == rexObj) {   // Snake on top of REX
            HandsSnakePost(work, rexObjs);
            return;
        }
    }
    if (fn == (uint32_t)(g_base + kSnaActRva)) {   // Snake himself: hands + wall touch
        if (work) SnakeStepTrack(work);
        ItemUseOnGameThread();
        if (work) HandsSnakePost(work);
        return;
    }
    int w = -1;
    for (int i = 0; i < kWeaponCount; ++i)
        if (fn == (uint32_t)(g_base + g_weapons[i].actRva)) { w = i; break; }
    if (w < 0 || !work) return;
    WeaponAct& W = g_weapons[w];

    uint32_t objs = 0;
    if (!ReadU32(work + W.objsOff, &objs) || !objs) return;
    uint32_t root = 0, flag = 0;
    if (!ReadU32(objs + kObjsRoot, &root) || !ReadU32(objs + kObjsFlag, &flag)) return;
    const uint32_t ours = (uint32_t)(uintptr_t)&g_handMtx;

    const bool allowed = g_cfgEnabled && g_cfgGunInHand &&
        (W.group == 0 || (W.group == 1 && g_cfgGunAllFirearms) || (W.group == 2 && g_cfgGunThrowables));
    float reach = 0.0f;
    // PSG1 at your eye (scope up): the rifle is not drawn in your hand -- it
    // would sit across the scope picture. Hand it back to Snake, whose first-
    // person model the game already hides.
    const bool scopedRifle = IsScopeViewActive() && std::strcmp(W.name, "PSG1") == 0;
    const bool drive = allowed && !scopedRifle && GameplayGate() && BuildHandMatrix(&g_handMtx, &reach);

    if (drive) {
        if (root != ours) {
            // First frame for this weapon object: prove the rebuild is right
            // before trusting it to give the gun back later.
            uint32_t rebuilt = 0;
            const bool ok = OriginalRoot(W, work, &rebuilt) && rebuilt == root;
            if (W.logged < 4) {
                ++W.logged;
                DebugLogger::LogFormat("Gun in hand: %s (work %08X, model %08X) -- hand joint %08X, rebuilt %08X %s",
                    W.name, work, objs, root, rebuilt, ok ? "MATCH, taking the gun" : "MISMATCH, leaving it alone");
            }
            if (!ok) return;
            WriteU32(objs + kObjsRoot, ours);
            if (g_cfgModelProbe && W.logged <= 1) {
                ProbeWeaponModel(W, objs);
                static bool bodyDone = false;
                uint32_t rootObj = 0, unit = 0, bodyObjs = 0;
                if (!bodyDone && ReadU32(work + W.rootObjOff, &rootObj) && rootObj &&
                    ReadU32(work + W.unitOff, &unit) && ReadU32(rootObj, &bodyObjs) && bodyObjs) {
                    bodyDone = true;
                    ProbeSnakeBody(bodyObjs, (int)unit);
                }
            }
        }
        // Throwables are also hidden for a moment after each throw (time > 120,
        // grenade.c) and when you are out of them; keep those, drop only the
        // "Snake is invisible in first person" reason.
        bool showIt = true;
        if (W.timeOff) {
            int32_t t = 0; int16_t wid = -1, ammo = 0;
            SafeRead(&t, (const void*)(uintptr_t)(work + W.timeOff), 4);
            SafeRead(&wid, (const void*)(g_base + kCurWeaponRva), 2);
            if (wid >= 0 && wid < 16) SafeRead(&ammo, (const void*)(g_base + kAmmoArrayRva + wid * 2), 2);
            showIt = (t <= 120) && (ammo > 0);
        }
        if (showIt && (flag & kHiddenBit)) WriteU32(objs + kObjsFlag, flag & ~kHiddenBit);
        g_gunActiveQpc.store(NowQpc(), std::memory_order_relaxed);
        g_heldFiresFromModel.store(W.firesFromModel, std::memory_order_relaxed);

        if (g_gunLogs < 12 && (g_gunLogs < 3 || (GetTickCount() / 1000) % 20 == 0)) {
            static DWORD lastTick = 0;
            if (GetTickCount() - lastTick > 900) {
                lastTick = GetTickCount();
                ++g_gunLogs;
                DebugLogger::LogFormat("Gun[%d] %s at (%d,%d,%d) reach %.2fm fwd(%d,%d,%d)", g_gunLogs, W.name,
                    g_handMtx.t[0], g_handMtx.t[1], g_handMtx.t[2], reach,
                    -g_handMtx.m[0][1], -g_handMtx.m[1][1], -g_handMtx.m[2][1]);
            }
        }
    }
    else if (root == ours) {
        // Hand it back to Snake. The weapon's own act already chose visibility
        // for this frame, so only the pointer needs restoring.
        uint32_t rebuilt = 0;
        if (OriginalRoot(W, work, &rebuilt)) WriteU32(objs + kObjsRoot, rebuilt);
    }
}

// Render-time wrapper around 0x406906 (DG screen matrices for one DG_OBJS).
// Same cdecl signature, so the original call site's pushes feed it unchanged.
extern "C" void __cdecl HandsScreenObjsWrap(uint32_t objs, int n) {
    h_anyScreenQpc = NowQpc();
    // 1000 ms, not 100: after a game-thread stall the first screen pass can
    // arrive long after the act that published the pose. The pose still
    // belongs to this body (the act has not run again), and skipping it would
    // draw Snake's whole unhidden body around the camera for that frame.
    if (g_hf.active && objs == g_hf.bodyObjs && n == g_hf.nParts && MsSince(g_hf.qpc) < 1000.0) {
        h_seenQpc = NowQpc();
        HandsApplyToScratch(objs, n);
        static bool logged = false;
        if (!logged) {
            logged = true;
            DebugLogger::LogFormat("Hands: render reached Snake's body (%d parts) -- his own hands are now drawn "
                "at your controllers.", n);
        }
    }
    if (n > 0 && n <= 4 && CigaretteShiftNow(objs)) {
        GameMatrix* scratch = (GameMatrix*)(g_base + kPartWorldScratch);
        for (int i = 0; i < n; ++i)
            for (int k = 0; k < 3; ++k)
                scratch[i].t[k] -= (int32_t)std::lround((float)sn_delta[k] * g_cfgCigLag);
        static bool s_logged = false;
        if (!s_logged) {
            s_logged = true;
            DebugLogger::LogFormat("Cigarettes: drawn %.1f frame(s) of movement back, level with your eye "
                "(cigarette_lag_percent)", g_cfgCigLag);
        }
    }
    ((void(__cdecl*)(uint32_t, int))(g_base + kScreenObjsRva))(objs, n);
}

// XR thread. The left controller's GRIP pose, stored exactly like the right
// aim snapshot (play space + head-local), so the same builder can place it.
static void PublishHandPoseInto(AimSnap& snap, VelTrackT& vt, HandEuro& eu, const XrPosef& grip, bool valid) {
    if (!g_anyInstalled) return;
    AcquireSRWLockExclusive(&g_lock);
    const bool haveHead = g_haveHead;
    const XrPosef head = g_lastHead;
    ReleaseSRWLockExclusive(&g_lock);
    if (!valid || !haveHead) {
        AcquireSRWLockExclusive(&g_lock);
        snap.valid = false; snap.havePrev = false; snap.haveWorld = false; snap.haveFilt = false;
        ReleaseSRWLockExclusive(&g_lock);
        vt.have = false;
        eu = HandEuro{};
        return;
    }
    const XrQuaternionf inv = QInv(head.orientation);
    const V3 dW = QRot(grip.orientation, V3{ 0, 0, -1 });
    const V3 uW = QRot(grip.orientation, V3{ 0, 1, 0 });
    const V3 pW = { grip.position.x - head.position.x, grip.position.y - head.position.y,
                    grip.position.z - head.position.z };
    const LONGLONG nowQ = NowQpc();
    if (vt.have) {
        const float dt = (float)((double)(nowQ - vt.qpc) / (double)g_qpcFreq);
        if (dt > 0.002f && dt < 0.2f) {
            const float a = dt / (0.03f + dt);
            const float raw[3] = { (grip.position.x - vt.pos[0]) / dt,
                                   (grip.position.y - vt.pos[1]) / dt,
                                   (grip.position.z - vt.pos[2]) / dt };
            for (int i = 0; i < 3; ++i) vt.vel[i] += (raw[i] - vt.vel[i]) * a;
        }
    }
    vt.have = true; vt.qpc = nowQ;
    vt.pos[0] = grip.position.x; vt.pos[1] = grip.position.y; vt.pos[2] = grip.position.z;
    const V3 dL = QRot(inv, dW), pL = QRot(inv, pW), uL = QRot(inv, uW);
    const V3 vL = QRot(inv, V3{ vt.vel[0], vt.vel[1], vt.vel[2] });
    AcquireSRWLockExclusive(&g_lock);
    AimSnap& s = snap;
    if (s.valid) {
        s.havePrev = true;
        for (int i = 0; i < 3; ++i) { s.pdir[i] = s.dir[i]; s.ppos[i] = s.pos[i]; s.pup[i] = s.up[i]; }
        for (int i = 0; i < 3; ++i) { s.pwdir[i] = s.wdir[i]; s.pwpos[i] = s.wpos[i]; s.pwup[i] = s.wup[i]; }
        s.pqpc = s.qpc;
    }
    s.haveWorld = true;
    s.wdir[0] = dW.x; s.wdir[1] = dW.y; s.wdir[2] = dW.z;
    s.wup[0] = uW.x;  s.wup[1] = uW.y;  s.wup[2] = uW.z;
    s.wpos[0] = grip.position.x; s.wpos[1] = grip.position.y; s.wpos[2] = grip.position.z;
    s.wvel[0] = vt.vel[0]; s.wvel[1] = vt.vel[1]; s.wvel[2] = vt.vel[2];
    s.dir[0] = dL.x; s.dir[1] = dL.y; s.dir[2] = dL.z;
    s.up[0] = uL.x;  s.up[1] = uL.y;  s.up[2] = uL.z;
    s.pos[0] = pL.x; s.pos[1] = pL.y; s.pos[2] = pL.z;
    s.vel[0] = vL.x; s.vel[1] = vL.y; s.vel[2] = vL.z;
    s.valid = true;
    s.qpc = nowQ;
    FilterIntoSnap(eu, s, V3{ grip.position.x, grip.position.y, grip.position.z }, dW, uW, head, nowQ);
    ReleaseSRWLockExclusive(&g_lock);
}

void MotionAimPublishLeftGrip(const XrPosef& grip, bool valid) {
    PublishHandPoseInto(g_snapL, g_velTrackL, g_euroL, grip, valid);
}

void MotionAimPublishRightHandAim(const XrPosef& aim, bool valid) {
    PublishHandPoseInto(g_snapRH, g_velTrackRH, g_euroRH, aim, valid);
}

void MotionAimSetLeftHanded(bool leftHanded) {
    if (g_leftHanded != leftHanded)
        DebugLogger::LogFormat("Hands: %s", leftHanded
            ? "LEFT-HANDED -- the gun follows your left hand; Snake's hands stay on their own controllers"
            : "right-handed");
    g_leftHanded = leftHanded;
}

bool IsGunInHandActive() {
    return MsSince(g_gunActiveQpc.load(std::memory_order_relaxed)) < 250.0;
}

void MotionAimNoteImageTiming(float firstShowAgeMs, float gamePeriodMs, float displayPeriodMs) {
    // Lightly smoothed: one late image must not yank the gun forward.
    const float prev = g_imgAgeMs.load(std::memory_order_relaxed);
    g_imgAgeMs.store(prev <= 0.0f ? firstShowAgeMs : prev + 0.1f * (firstShowAgeMs - prev), std::memory_order_relaxed);
    g_imgPeriodMs.store(gamePeriodMs, std::memory_order_relaxed);
    g_dispPeriodMs.store(displayPeriodMs, std::memory_order_relaxed);
}

#ifdef _M_IX86
static __declspec(naked) void MotionAimDispatchThunk() {
    __asm {
        // Pre-act hook (re-hides Snake's body for everything but the renderer).
        pushfd
        pushad
        mov  eax, [ebp - 10h]           // act fn
        mov  ecx, [ebp - 14h]           // actor
        mov  esi, esp
        and  esp, 0FFFFFFF0h
        sub  esp, 16
        mov  [esp], eax
        mov  [esp + 4], ecx
        call MotionAimActPre
        mov  esp, esi
        popad
        popfd

        // Replays: push [ebp-0x14] / call [ebp-0x10] / pop ecx
        push dword ptr [ebp - 14h]
        call dword ptr [ebp - 10h]
        pop  ecx

        pushfd
        pushad
        mov  eax, [ebp - 10h]           // act fn  (ebp is still the loop's frame)
        mov  ecx, [ebp - 14h]           // actor
        mov  esi, esp
        and  esp, 0FFFFFFF0h
        sub  esp, 16
        mov  [esp], eax
        mov  [esp + 4], ecx
        call MotionAimActPost
        mov  esp, esi
        popad
        popfd
        ret                              // back to 0x40A21D
    }
}
#endif

// ===========================================================================
// Install
// ===========================================================================
#ifdef _M_IX86
static uintptr_t g_maAllocTarget = 0;

static __declspec(naked) void MotionAimAllocThunk() {
    __asm {
        pushfd
        pushad
        lea  eax, [esp + 36]            // original esp: the return address
        mov  ebp, esp
        and  esp, 0FFFFFFF0h
        sub  esp, 16
        mov  [esp], eax
        call MotionAimAllocBody
        mov  esp, ebp
        popad
        popfd
        jmp  dword ptr [g_maAllocTarget] // the real allocator; it returns to the ctor
    }
}
#endif

void LoadMotionAimConfig() {
    const std::string ini = IniPath();
    auto I = [&](const char* k, int d) { return (int)GetPrivateProfileIntA("motion_aim", k, d, ini.c_str()); };
    g_cfgEnabled = I("enabled", 1) != 0;
    g_cfgMuzzleFromHand = I("muzzle_from_hand", 1) != 0;
    g_cfgMaxReachM = (float)I("max_reach_cm", 60) / 100.0f;
    g_cfgPlayerRadius = (float)I("player_radius", 2500);
    g_cfgFireWindowMs = I("fire_window_ms", 400);
    g_cfgRightSign = I("right_sign", 0);
    g_cfgUpSign = I("up_sign", 0);
    g_cfgForwardVector = I("forward_vector", 1);
    g_cfgForwardSign = I("forward_sign", -1);
    g_cfgDisplayMapping = I("display_mapping", 1) != 0;
    g_cfgLogShots = I("log_shots", 40);
    g_cfgLogMisses = I("log_misses", 6);
    g_cfgUnitsPerMetre = (float)GetPrivateProfileIntA("camera_hook", "position_scale", 2048, ini.c_str());
    if (g_cfgUnitsPerMetre < 100.0f) g_cfgUnitsPerMetre = 2048.0f;

    g_cfgGunInHand = I("gun_in_hand", 1) != 0;
    g_cfgGunAllFirearms = I("gun_all_firearms", 1) != 0;
    g_cfgStingerHeadAim = I("stinger_head_aim", 1) != 0;
    g_cfgGunRollDeg = (float)I("gun_roll_deg", 0);
    g_cfgGunOffset[0] = (float)I("gun_offset_x", 0);
    g_cfgGunOffset[1] = (float)I("gun_offset_y", 0);
    g_cfgGunOffset[2] = (float)I("gun_offset_z", 0);
    g_cfgGunEyeCenterSign = I("gun_eye_center_sign", 1);
    g_cfgThrowAim = I("throw_aim", 1) != 0;
    g_cfgGunThrowables = I("gun_throwables", 1) != 0;
    g_cfgThrowSpeed = (float)I("throw_speed_percent", 100) / 100.0f;
    g_cfgRenderHeadFrame = I("hand_relative_to_render_pose", 1) != 0;
    g_cfgHandPredictMs = I("hand_predict_ms", -1);
    if (g_cfgHandPredictMs < -1) g_cfgHandPredictMs = -1;
    if (g_cfgHandPredictMs > 100) g_cfgHandPredictMs = 100;
    g_cfgHandPredictPercent = I("hand_predict_percent", 80);
    if (g_cfgHandPredictPercent < 0) g_cfgHandPredictPercent = 0;
    if (g_cfgHandPredictPercent > 150) g_cfgHandPredictPercent = 150;
    g_cfgHandFilter = I("hand_filter", 1) != 0;
    {
        auto F = [&](const char* k, float d, float lo, float hi) {
            char buf[32] = {};
            GetPrivateProfileStringA("motion_aim", k, "", buf, sizeof(buf), ini.c_str());
            float v = buf[0] ? (float)atof(buf) : d;
            if (!(v >= lo)) v = lo;
            if (v > hi) v = hi;
            return v;
        };
        g_cfgHandFilterMinCutHz = F("hand_filter_min_cutoff_hz", 1.5f, 0.2f, 30.0f);
        g_cfgHandFilterPosBeta = F("hand_filter_pos_beta", 12.0f, 0.0f, 200.0f);
        g_cfgHandFilterRotBeta = F("hand_filter_rot_beta", 2.0f, 0.0f, 50.0f);
        g_cfgHandFilterDCutHz = F("hand_filter_velocity_cutoff_hz", 5.0f, 0.5f, 30.0f);
        g_cfgStillMps = F("hand_still_cms", 3.0f, 0.0f, 100.0f) / 100.0f;
        g_cfgMovingMps = F("hand_moving_cms", 15.0f, 0.0f, 300.0f) / 100.0f;
        if (g_cfgMovingMps < g_cfgStillMps) g_cfgMovingMps = g_cfgStillMps;
        g_cfgStillRads = F("hand_still_deg_per_s", 9.0f, 0.0f, 360.0f) * kPi / 180.0f;
        g_cfgMovingRads = F("hand_moving_deg_per_s", 45.0f, 0.0f, 720.0f) * kPi / 180.0f;
        if (g_cfgMovingRads < g_cfgStillRads) g_cfgMovingRads = g_cfgStillRads;
        g_cfgHandJitterLog = I("hand_jitter_log", 1) != 0;
    }
    DebugLogger::LogFormat("Hand smoothing: hand_filter=%d (min cutoff %.1f Hz, beta pos %.1f rot %.1f, velocity cutoff %.1f Hz) | "
        "prediction %s at %d%%", g_cfgHandFilter ? 1 : 0, g_cfgHandFilterMinCutHz, g_cfgHandFilterPosBeta,
        g_cfgHandFilterRotBeta, g_cfgHandFilterDCutHz,
        g_cfgHandPredictMs < 0 ? "MEASURED (image age + half its time on screen)" : "fixed hand_predict_ms",
        g_cfgHandPredictPercent);
    g_cfgThrowSwing = I("throw_swing", 1) != 0;
    g_cfgSwingMinMps = (float)I("throw_swing_min_cms", 120) / 100.0f;
    g_cfgSwingGain = (float)I("throw_swing_gain_percent", 100) / 100.0f;
    g_cfgModelProbe = I("model_probe", 1) != 0;
    g_cfgCigLag = (float)I("cigarette_lag_percent", 100) / 100.0f;
    if (g_cfgThrowSpeed < 0.25f) g_cfgThrowSpeed = 0.25f;
    if (g_cfgThrowSpeed > 3.0f) g_cfgThrowSpeed = 3.0f;
    g_cfgHalfIpdM = (float)GetPrivateProfileIntA("camera_hook", "stereo_ipd_mm", 64, ini.c_str()) / 2000.0f;
    g_cfgStereoSwap = GetPrivateProfileIntA("camera_hook", "stereo_swap_eyes", 0, ini.c_str()) != 0;
    DebugLogger::LogFormat("Gun in hand config: enabled=%d all_firearms=%d roll=%.0f offset(%.0f,%.0f,%.0f) "
        "eye_center_sign=%d half_ipd=%.3fm throw_aim=%d gun_throwables=%d throw_speed=%.0f%% "
        "hand_predict=%dms throw_swing=%d (min %.2f m/s, gain %.0f%%) model_probe=%d hand_relative_to_render_pose=%d",
        g_cfgGunInHand ? 1 : 0, g_cfgGunAllFirearms ? 1 : 0, g_cfgGunRollDeg,
        g_cfgGunOffset[0], g_cfgGunOffset[1], g_cfgGunOffset[2], g_cfgGunEyeCenterSign, g_cfgHalfIpdM,
        g_cfgThrowAim ? 1 : 0, g_cfgGunThrowables ? 1 : 0, g_cfgThrowSpeed * 100.0f,
        g_cfgHandPredictMs, g_cfgThrowSwing ? 1 : 0, g_cfgSwingMinMps, g_cfgSwingGain * 100.0f, g_cfgModelProbe ? 1 : 0,
        g_cfgRenderHeadFrame ? 1 : 0);

    // ---- [hands] -----------------------------------------------------------
    {
        const char* H = "hands";
        auto HI = [&](const char* k, int d) { return (int)GetPrivateProfileIntA(H, k, d, ini.c_str()); };
        auto HF3 = [&](const char* k, float* out) {
            char buf[96] = {};
            GetPrivateProfileStringA(H, k, "", buf, sizeof(buf), ini.c_str());
            float v[3];
            if (buf[0] && sscanf_s(buf, "%f,%f,%f", &v[0], &v[1], &v[2]) == 3) { out[0] = v[0]; out[1] = v[1]; out[2] = v[2]; }
        };
        h_enabled = HI("enabled", 1) != 0;
        h_forearms = HI("show_forearms", 0) != 0;
        h_partR = HI("right_hand_part", 4);  h_foreR = HI("right_forearm_part", 3);
        h_partL = HI("left_hand_part", 9);   h_foreL = HI("left_forearm_part", 8);
        HF3("right_rotation_deg", h_rotR);
        HF3("left_rotation_deg", h_rotL);
        HF3("right_offset", h_offR);
        HF3("left_offset", h_offL);
        h_collision = HI("wall_collision", 1) != 0;
        h_gunCollision = HI("gun_collision", 1) != 0;
        h_margin = (float)HI("surface_margin", 30);
        h_gunLength = (float)HI("gun_length", 420);
        h_knock = HI("knock", 1) != 0;
        h_knockSpeed = (float)HI("knock_min_speed_cms", 60) / 100.0f;
        h_knockCooldownMs = HI("knock_cooldown_ms", 300);
        h_knockSound = HI("knock_sound_id", -1);
        h_knockPower = HI("knock_noise_power", 100);
        h_knockLength = HI("knock_noise_length", 16);
        h_logLines = HI("log_lines", 40);
        h_fullBody = HI("full_body", 0) != 0;   // experimental: off unless asked for
        {
            char buf[96] = {};
            GetPrivateProfileStringA(H, "full_body_hide_parts", "5,6", buf, sizeof(buf), ini.c_str());
            h_hidePartsN = 0;
            for (char* p = buf; *p && h_hidePartsN < 8; ) {
                char* end = nullptr;
                const long v = strtol(p, &end, 10);
                if (end == p) { ++p; continue; }
                h_hideParts[h_hidePartsN++] = (int)v;
                p = end;
            }
        }
        h_elbowOut = (float)HI("elbow_out_percent", 50) / 100.0f;
        h_ikLogLines = HI("full_body_log_lines", 12);
        h_twistShare = (float)HI("forearm_twist_percent", 50) / 100.0f;
        h_bodyAnchor = HI("body_follow_head", 1) != 0;
        h_bodyBack = (float)HI("body_back_cm", 15) * g_cfgUnitsPerMetre / 100.0f;
        h_bodyDown = (float)HI("body_down_cm", 0) * g_cfgUnitsPerMetre / 100.0f;
        DebugLogger::LogFormat("FullBody config: full_body=%d hide %d part(s) starting %d, elbow_out=%.0f%% "
            "forearm_twist=%.0f%% body_follow_head=%d back %.0f units down %.0f units",
            h_fullBody ? 1 : 0, h_hidePartsN, h_hidePartsN ? h_hideParts[0] : -1, h_elbowOut * 100.0f,
            h_twistShare * 100.0f, h_bodyAnchor ? 1 : 0, h_bodyBack, h_bodyDown);
        DebugLogger::LogFormat("Hands config: enabled=%d forearms=%d parts R%d/L%d (forearms %d/%d) rotR(%.0f,%.0f,%.0f) "
            "rotL(%.0f,%.0f,%.0f) | wall_collision=%d gun_collision=%d margin=%.0f gun_length=%.0f | knock=%d "
            "min %.2f m/s cooldown %dms sound %s noise %d/%d",
            h_enabled ? 1 : 0, h_forearms ? 1 : 0, h_partR, h_partL, h_foreR, h_foreL,
            h_rotR[0], h_rotR[1], h_rotR[2], h_rotL[0], h_rotL[1], h_rotL[2],
            h_collision ? 1 : 0, h_gunCollision ? 1 : 0, h_margin, h_gunLength, h_knock ? 1 : 0,
            h_knockSpeed, h_knockCooldownMs, h_knockSound < 0 ? "PROBE (cycles candidates)" : "fixed",
            h_knockPower, h_knockLength);
    }

    if (g_cfgRightSign != 0) g_rightSign.store(g_cfgRightSign > 0 ? 1 : -1);
    if (g_cfgUpSign != 0) g_upSign.store(g_cfgUpSign > 0 ? 1 : -1);

    DebugLogger::LogFormat("Motion aim config: enabled=%d muzzle_from_hand=%d max_reach=%.2fm "
        "player_radius=%.0f fire_window=%dms right_sign=%d up_sign=%d forward_vector=%d "
        "display_mapping=%d units/m=%.0f",
        g_cfgEnabled ? 1 : 0, g_cfgMuzzleFromHand ? 1 : 0, g_cfgMaxReachM, g_cfgPlayerRadius,
        g_cfgFireWindowMs, g_cfgRightSign, g_cfgUpSign, g_cfgForwardVector,
        g_cfgDisplayMapping ? 1 : 0, g_cfgUnitsPerMetre);
}

bool InstallMotionAimHooks() {
#ifndef _M_IX86
    DebugLogger::Log("Motion aim: NOT installed -- x86 only.");
    return false;
#else
    if (g_anyInstalled) return true;
    LARGE_INTEGER f; QueryPerformanceFrequency(&f); g_qpcFreq = f.QuadPart;
    LoadMotionAimConfig();
    if (!g_cfgEnabled) { DebugLogger::Log("Motion aim: disabled in ini -- nothing patched."); return false; }

    g_base = (uintptr_t)GetModuleHandleA(nullptr);
    if (!g_base) return false;
    g_maAllocTarget = g_base + kAllocRva;

    for (int i = 0; i < kSiteCount; ++i) {
        BulletSite& S = g_sites[i];
        uint8_t win[64];
        uint8_t* start = (uint8_t*)(g_base + S.rosterRva - 24);
        if (!SafeRead(win, start, sizeof(win))) {
            DebugLogger::LogFormat("Motion aim (%s): +%X unreadable -- skipped", S.label, (unsigned)S.rosterRva);
            continue;
        }
        int found = -1, count = 0;
        for (int o = 7; o + 5 <= (int)sizeof(win); ++o) {
            if (win[o] != 0xE8) continue;
            int32_t rel; memcpy(&rel, win + o + 1, 4);
            const uintptr_t target = (uintptr_t)(start + o + 5) + rel;
            if (target != g_maAllocTarget) continue;
            if (memcmp(win + o - 7, S.push, 7) != 0) continue;
            ++count; found = o;
        }
        if (count != 1) {
            DebugLogger::LogFormat("Motion aim (%s): %s near +%X -- NOT installing this site",
                S.label, count == 0 ? "no matching 'push size / push class / call 0x40A30C'" : "ambiguous match",
                (unsigned)S.rosterRva);
            continue;
        }
        uint8_t* call = start + found;
        memcpy(&S.origRel, call + 1, 4);
        DWORD old = 0;
        if (!VirtualProtect(call, 5, PAGE_EXECUTE_READWRITE, &old)) continue;
        const int32_t rel = (int32_t)((intptr_t)&MotionAimAllocThunk - (intptr_t)(call + 5));
        memcpy(call + 1, &rel, 4);
        DWORD ignored = 0;
        VirtualProtect(call, 5, old, &ignored);
        FlushInstructionCache(GetCurrentProcess(), call, 5);
        S.call = call; S.retAddr = (uintptr_t)(call + 5); S.installed = true;
        g_anyInstalled = true;
        DebugLogger::LogFormat("Motion aim (%s) INSTALLED: allocator call at mgsi.exe+%X redirected",
            S.label, (unsigned)((uintptr_t)call - g_base));
    }

    // Actor dispatcher, for the gun in hand AND for Snake's hands / wall touch.
    if (g_cfgGunInHand || h_enabled || h_collision || h_gunCollision) {
        uint8_t* site = (uint8_t*)(g_base + kDispatchRva);
        uint8_t got[7] = {};
        if (SafeRead(got, site, 7) && memcmp(got, kDispatchExpected, 7) == 0) {
            DWORD old = 0;
            if (VirtualProtect(site, 7, PAGE_EXECUTE_READWRITE, &old)) {
                memcpy(g_dispatchOrig, got, 7);
                const int32_t rel = (int32_t)((intptr_t)&MotionAimDispatchThunk - (intptr_t)(site + 5));
                site[0] = 0xE8; memcpy(site + 1, &rel, 4); site[5] = 0x90; site[6] = 0x90;
                DWORD ignored = 0;
                VirtualProtect(site, 7, old, &ignored);
                FlushInstructionCache(GetCurrentProcess(), site, 7);
                g_dispatchSite = site; g_dispatchInstalled = true; g_anyInstalled = true;
                DebugLogger::Log("Gun in hand: actor dispatcher hooked at mgsi.exe+A216 -- the game will draw "
                    "its own weapon at your right controller in first person.");
            }
        }
        else {
            DebugLogger::LogFormat("Gun in hand: dispatcher signature MISMATCH at +A216 (got %02X %02X %02X %02X "
                "%02X %02X %02X) -- not installed.", got[0], got[1], got[2], got[3], got[4], got[5], got[6]);
        }
    }

    // Engine functions the wall touch calls. Verified, not trusted.
    if (h_collision || h_gunCollision || h_knock) {
        const bool hzdOk = VerifyBytes(kHzdLineTestRva, kHzdSig, sizeof(kHzdSig)) &&
                           VerifyBytes(kGetMapRva, kMapSig, sizeof(kMapSig));
        const bool seOk = VerifyBytes(kSeSetRva, kSeSig, sizeof(kSeSig)) &&
                          VerifyBytes(kSeSetModeRva, kSeModeSig, sizeof(kSeModeSig));
        if (!hzdOk) { h_collision = false; h_gunCollision = false; h_knock = false; }
        if (!seOk) h_knock = false;
        DebugLogger::LogFormat("Wall touch: collision functions %s, sound function %s -- wall_collision=%d "
            "gun_collision=%d knock=%d", hzdOk ? "VERIFIED" : "MISMATCH", seOk ? "VERIFIED" : "MISMATCH",
            h_collision ? 1 : 0, h_gunCollision ? 1 : 0, h_knock ? 1 : 0);
    }

    // Render-time hook for Snake's hands: retarget the rel32 of
    // `call 0x406906` at +6742 to our same-signature wrapper.
    if (h_enabled && g_dispatchInstalled) {
        uint8_t* site = (uint8_t*)(g_base + kScreenCallRva);
        uint8_t got[7] = {};
        if (SafeRead(got, site, 7) && memcmp(got, kScreenCallExpected, 7) == 0) {
            DWORD old = 0;
            if (VirtualProtect(site + 2, 5, PAGE_EXECUTE_READWRITE, &old)) {
                memcpy(&h_callOrigRel, site + 3, 4);
                const int32_t rel = (int32_t)((intptr_t)&HandsScreenObjsWrap - (intptr_t)(site + 7));
                memcpy(site + 3, &rel, 4);
                DWORD ignored = 0;
                VirtualProtect(site + 2, 5, old, &ignored);
                FlushInstructionCache(GetCurrentProcess(), site + 2, 5);
                h_callSite = site; h_hooked = true;
                DebugLogger::Log("Hands: render hook INSTALLED at mgsi.exe+6742 (DG screen matrices). Snake's own "
                    "hands will be drawn at your controllers in first person.");
            }
        }
        else {
            DebugLogger::LogFormat("Hands: render call signature MISMATCH at +6740 (got %02X %02X %02X %02X %02X %02X "
                "%02X) -- hands not installed.", got[0], got[1], got[2], got[3], got[4], got[5], got[6]);
        }
    }
    else if (h_enabled) {
        DebugLogger::Log("Hands: not installed -- they need the actor dispatcher hook, which did not install.");
    }

    if (g_anyInstalled)
        DebugLogger::Log("Motion aim: armed. Calibrates itself -- look around in first person, then fire "
            "twice with the controller pointing roughly where you look. Watch for 'CALIBRATED' lines.");
    else
        DebugLogger::Log("Motion aim: no bullet constructor verified -- shots stay on the camera.");
    return g_anyInstalled;
#endif
}

void RemoveMotionAimHooks() {
    for (int i = 0; i < kSiteCount; ++i) {
        BulletSite& S = g_sites[i];
        if (!S.installed || !S.call) continue;
        DWORD old = 0;
        if (VirtualProtect(S.call, 5, PAGE_EXECUTE_READWRITE, &old)) {
            memcpy(S.call + 1, &S.origRel, 4);
            DWORD ignored = 0;
            VirtualProtect(S.call, 5, old, &ignored);
            FlushInstructionCache(GetCurrentProcess(), S.call, 5);
        }
        S.installed = false;
    }
    if (g_dispatchInstalled && g_dispatchSite) {
        DWORD old = 0;
        if (VirtualProtect(g_dispatchSite, 7, PAGE_EXECUTE_READWRITE, &old)) {
            memcpy(g_dispatchSite, g_dispatchOrig, 7);
            DWORD ignored = 0;
            VirtualProtect(g_dispatchSite, 7, old, &ignored);
            FlushInstructionCache(GetCurrentProcess(), g_dispatchSite, 7);
        }
        g_dispatchInstalled = false;
    }
    if (h_hooked && h_callSite) {
        DWORD old = 0;
        if (VirtualProtect(h_callSite + 2, 5, PAGE_EXECUTE_READWRITE, &old)) {
            memcpy(h_callSite + 3, &h_callOrigRel, 4);
            DWORD ignored = 0;
            VirtualProtect(h_callSite + 2, 5, old, &ignored);
            FlushInstructionCache(GetCurrentProcess(), h_callSite + 2, 5);
        }
        h_hooked = false;
    }
    g_anyInstalled = false;
}

// XR thread: ask the game thread to use an item through the game's own
// item-window routine on the next Snake act.
void MotionAimRequestItemUse(int itemId) {
    if (itemId < 0 || itemId >= 24) return;
    g_itemUseReq.store(itemId + 1, std::memory_order_relaxed);
}

// XR thread: is this hand (0 = left, 1 = right) resting against a wall right
// now, as the hands' own collision sees it? Used for "grab" (grip on a wall =
// ACTION: ladders, elevator buttons).
bool MotionAimHandTouchingWall(int side) {
    const int h = (side == 1) ? 0 : 1;
    const unsigned long long t = h_touchTick[h].load(std::memory_order_relaxed);
    return t != 0 && GetTickCount64() - t < 150;
}

// Crash handler (dllmain.cpp): the DG_OBJS pointers this file writes to, and
// how long ago each was last confirmed, read without locks or allocation.
// Called from inside an exception handler, so it only copies plain values.
extern "C" void MotionAimCrashSnapshot(MotionAimCrashInfo* out) {
    if (!out) return;
    out->actFn = g_crashActFn;
    out->actWork = g_crashActWork;
    out->handsActive = g_hf.active ? 1 : 0;
    out->handsBodyObjs = g_hf.bodyObjs;
    out->handsNParts = g_hf.nParts;
    out->handsAgeMs = g_hf.qpc ? MsSince(g_hf.qpc) : -1.0;
    out->unhidObjs = h_unhidObjs;
    out->unhidAgeMs = h_unhideQpc ? MsSince(h_unhideQpc) : -1.0;
    out->renderHid0 = rh_hid[0];
    out->renderHid1 = rh_hid[1];
    out->actRehidObjs = h_actRehidObjs;
}

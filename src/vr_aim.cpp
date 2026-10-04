#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <atomic>
#include <string>
#include <vector>
#include <d3d11.h>
#define XR_USE_GRAPHICS_API_D3D11
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include "../include/vr_aim.h"
#include "../include/motion_aim.h"
#include "../include/debug_logging.h"
#include "../include/camera_write_hook.h"
#include <mutex>
#pragma comment(lib, "gdi32.lib")

// ===========================================================================
// See vr_aim.h for what this is and why it is a compositor layer rather than
// scene geometry. This file is deliberately self-contained: it owns its own
// swapchain and its own config, and the only things it needs from the rest of
// the mod are one publish call per frame from vr_input.cpp and one append call
// per frame from vr_injection.cpp's xrEndFrame.
// ===========================================================================

namespace {

constexpr float kPi = 3.14159265358979323846f;

// --- config, [aim] in mgs1_vr_config.ini ------------------------------------
bool  g_laserEnabled = true;
int   g_laserDots = 6;
float g_laserNearM = 0.30f;   // first dot, metres from the controller
float g_laserFarM = 6.00f;    // last dot
float g_laserSizeDeg = 0.7f;  // angular diameter, so the beam reads evenly
float g_aimPitchTrimDeg = 0.0f;
float g_aimYawTrimDeg = 0.0f;

// How many dots the beam may have. This is NOT a layer budget any more -- the
// whole beam is drawn into a single quad's texture, so dots are free. It only
// bounds the per-frame draw loop.
//
// It used to be a layer budget, and that nearly killed the mod: six quad
// layers over two 2496x2688 projection views saturated VDXR's compositor on a
// Quest 2, and once that happened the game's DirectDraw Lock() blocked on a
// busy GPU forever -- a permanent 30 -> 3 fps collapse that survived leaving
// first person. See the note in vr_aim.h.
constexpr int kMaxDots = 12;

// --- published controller aim ------------------------------------------------
// The XrPosef is written and read on the XR frame thread only. The angles are
// atomics because the GAME thread reads them.
XrPosef g_aimPose{};
bool    g_aimPoseValid = false;
std::atomic<float> g_aimYawRad{ 0.0f };
std::atomic<float> g_aimPitchRad{ 0.0f };
std::atomic<bool>  g_aimAnglesValid{ false };

// --- laser resources ---------------------------------------------------------
XrSession   g_session = XR_NULL_HANDLE;
XrSpace     g_space = XR_NULL_HANDLE;
ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_context = nullptr;

XrSwapchain g_dotSwapchain = XR_NULL_HANDLE;
std::vector<XrSwapchainImageD3D11KHR> g_dotImages;
bool g_laserReady = false;

// The beam canvas. One texture, redrawn each frame, presented as one quad.
// 256 square is plenty: a dot is a handful of texels across and the canvas
// only ever holds a thin row of them.
constexpr int32_t kBeamPx = 256;

// Where the quad hangs relative to the head, and how much of the field it
// spans. The quad is a PROJECTION SCREEN, not an object -- the dots' real 3D
// positions are projected onto it from the head, so the beam still reads as a
// ray receding into the scene even though it is painted on a flat plane at a
// fixed distance. Anything outside this cone simply falls off the canvas.
constexpr float kQuadDistM = 2.0f;
constexpr float kQuadHalfFovDeg = 50.0f;

// Diagnostic: do all the swapchain work but never submit the layer. See
// BuildAimLaserLayers.
bool g_laserSubmitLayer = true;

// Scratch, reused every frame so the per-frame cost is a memset and a few
// small circles rather than an allocation.
std::vector<uint8_t> g_beamPixels;

// Storage the layer points into. Must outlive the xrEndFrame call, which it
// does because it is file-scope.
XrCompositionLayerQuad g_beamQuad{};

std::string GetGameIniPath() {
    char exePath[MAX_PATH] = { 0 };
    GetModuleFileNameA(NULL, exePath, MAX_PATH);
    std::string dir(exePath);
    auto slash = dir.find_last_of("\\/");
    if (slash != std::string::npos) dir = dir.substr(0, slash);
    return dir + "\\mgs1_vr_config.ini";
}

// --- tiny quaternion helpers -------------------------------------------------
XrQuaternionf QuatMul(const XrQuaternionf& a, const XrQuaternionf& b) {
    XrQuaternionf r;
    r.w = a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z;
    r.x = a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y;
    r.y = a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x;
    r.z = a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w;
    return r;
}

XrQuaternionf QuatFromAxisAngle(float ax, float ay, float az, float rad) {
    const float h = rad * 0.5f;
    const float s = std::sin(h);
    XrQuaternionf q;
    q.x = ax * s; q.y = ay * s; q.z = az * s; q.w = std::cos(h);
    return q;
}

// Rotate v by q.
void QuatRotate(const XrQuaternionf& q, const float v[3], float out[3]) {
    const float ux = q.x, uy = q.y, uz = q.z, s = q.w;
    const float dot = ux * v[0] + uy * v[1] + uz * v[2];
    const float cx = uy * v[2] - uz * v[1];
    const float cy = uz * v[0] - ux * v[2];
    const float cz = ux * v[1] - uy * v[0];
    out[0] = 2.0f * dot * ux + (s * s - (ux * ux + uy * uy + uz * uz)) * v[0] + 2.0f * s * cx;
    out[1] = 2.0f * dot * uy + (s * s - (ux * ux + uy * uy + uz * uz)) * v[1] + 2.0f * s * cy;
    out[2] = 2.0f * dot * uz + (s * s - (ux * ux + uy * uy + uz * uz)) * v[2] + 2.0f * s * cz;
}

// The aim orientation with the ini trim composed on. Trim is applied in the
// CONTROLLER's own frame (yaw about its up, pitch about its right), which is
// what makes "nudge the dot onto the bullet hole" behave like a sight
// adjustment rather than a world-space rotation.
XrQuaternionf TrimmedAimOrientation(const XrQuaternionf& raw) {
    XrQuaternionf q = raw;
    if (g_aimYawTrimDeg != 0.0f) {
        q = QuatMul(q, QuatFromAxisAngle(0.0f, 1.0f, 0.0f, g_aimYawTrimDeg * kPi / 180.0f));
    }
    if (g_aimPitchTrimDeg != 0.0f) {
        q = QuatMul(q, QuatFromAxisAngle(1.0f, 0.0f, 0.0f, g_aimPitchTrimDeg * kPi / 180.0f));
    }
    return q;
}

// OpenXR forward is -Z.
void AimForward(const XrQuaternionf& q, float out[3]) {
    const float fwd[3] = { 0.0f, 0.0f, -1.0f };
    QuatRotate(q, fwd, out);
}

// YXZ decomposition, matching QuaternionToYawPitchRoll in vr_injection.cpp.
// Using the SAME convention matters: the game-thread consumer converts these
// straight into the engine's 12-bit angles alongside head angles produced the
// same way, and a second convention here would silently disagree.
void QuatToYawPitch(const XrQuaternionf& q, float& yawRad, float& pitchRad) {
    float sinPitch = 2.0f * (q.w * q.x - q.y * q.z);
    if (sinPitch > 1.0f)  sinPitch = 1.0f;
    if (sinPitch < -1.0f) sinPitch = -1.0f;
    pitchRad = std::asin(sinPitch);
    yawRad = std::atan2(2.0f * (q.w * q.y + q.x * q.z),
                        1.0f - 2.0f * (q.x * q.x + q.y * q.y));
}

// Fill one 64x64 RGBA image with a soft round dot: opaque core, alpha falling
// to zero at the rim. Premultiplied is deliberately NOT used -- the layer is
// submitted with the unpremultiplied-alpha flag, which is the simpler contract
// and what every runtime handles identically.
void StampDot(std::vector<uint8_t>& rgba, float cx, float cy, float rOuter,
              float intensity) {
    if (rOuter < 0.75f) rOuter = 0.75f;
    const float rCore = rOuter * 0.45f;
    const int x0 = (int)std::floor(cx - rOuter), x1 = (int)std::ceil(cx + rOuter);
    const int y0 = (int)std::floor(cy - rOuter), y1 = (int)std::ceil(cy + rOuter);
    for (int y = y0; y <= y1; ++y) {
        if (y < 0 || y >= kBeamPx) continue;
        for (int x = x0; x <= x1; ++x) {
            if (x < 0 || x >= kBeamPx) continue;
            const float dx = (float)x - cx, dy = (float)y - cy;
            const float d = std::sqrt(dx * dx + dy * dy);
            float a = 0.0f;
            if (d <= rCore) a = 1.0f;
            else if (d < rOuter) a = 1.0f - (d - rCore) / (rOuter - rCore);
            if (a <= 0.0f) continue;
            a = a * a * intensity;           // squared falloff reads cleaner
            if (a <= 0.0f) continue;
            uint8_t* p = &rgba[((size_t)y * kBeamPx + x) * 4u];
            const uint8_t newA = (uint8_t)(a * 255.0f + 0.5f);
            // Dots may overlap near the muzzle; keep the strongest rather than
            // summing, so an overlap does not read as a bright blob.
            if (newA <= p[3]) continue;
            p[0] = 255; p[1] = 90; p[2] = 60; // warm red, MGS-ish laser sight
            p[3] = newA;
        }
    }
}

} // namespace

// ===========================================================================
// Publishing
// ===========================================================================

void PublishControllerAim(const XrPosef& aimPose, bool valid) {
    g_aimPoseValid = valid;
    if (!valid) {
        g_aimAnglesValid.store(false, std::memory_order_relaxed);
        return;
    }
    g_aimPose = aimPose;

    // Publish the TRIMMED direction, not the raw one, so every consumer --
    // laser, a future engine aim write, the log -- describes one ray.
    const XrQuaternionf trimmed = TrimmedAimOrientation(aimPose.orientation);
    float yaw = 0.0f, pitch = 0.0f;
    QuatToYawPitch(trimmed, yaw, pitch);
    g_aimYawRad.store(yaw, std::memory_order_relaxed);
    g_aimPitchRad.store(pitch, std::memory_order_relaxed);
    g_aimAnglesValid.store(true, std::memory_order_relaxed);

    static int logCount = 0;
    if (logCount < 3) {
        logCount++;
        DebugLogger::LogFormat(
            "Aim: controller aim pose live -- pos(%.3f, %.3f, %.3f) yaw=%.1f deg pitch=%.1f deg "
            "(trim yaw %+.1f pitch %+.1f)",
            aimPose.position.x, aimPose.position.y, aimPose.position.z,
            yaw * 180.0f / kPi, pitch * 180.0f / kPi,
            g_aimYawTrimDeg, g_aimPitchTrimDeg);
    }
}

bool GetControllerAimPose(XrPosef* out) {
    if (!out || !g_aimPoseValid) return false;
    *out = g_aimPose;
    return true;
}

bool GetControllerAimAnglesRad(float* yawRad, float* pitchRad) {
    if (!g_aimAnglesValid.load(std::memory_order_relaxed)) return false;
    if (yawRad)   *yawRad = g_aimYawRad.load(std::memory_order_relaxed);
    if (pitchRad) *pitchRad = g_aimPitchRad.load(std::memory_order_relaxed);
    return true;
}

// ===========================================================================
// Config
// ===========================================================================

void LoadAimConfig() {
    const std::string ini = GetGameIniPath();
    auto I = [&](const char* k, int d) { return GetPrivateProfileIntA("aim", k, d, ini.c_str()); };

    g_laserEnabled = I("laser_enabled", 1) != 0;
    g_laserSubmitLayer = I("laser_submit_layer", 1) != 0;
    g_laserDots = I("laser_dots", 6);
    if (g_laserDots < 1) g_laserDots = 1;
    if (g_laserDots > kMaxDots) g_laserDots = kMaxDots;
    // Distances and trim are in whole units/tenths via the ini's int-only API.
    g_laserNearM = (float)I("laser_near_cm", 30) / 100.0f;
    g_laserFarM = (float)I("laser_far_cm", 600) / 100.0f;
    if (g_laserFarM <= g_laserNearM) g_laserFarM = g_laserNearM + 0.5f;
    g_laserSizeDeg = (float)I("laser_size_tenth_deg", 7) / 10.0f;
    if (g_laserSizeDeg < 0.1f) g_laserSizeDeg = 0.1f;
    g_aimYawTrimDeg = (float)I("aim_yaw_trim_tenth_deg", 0) / 10.0f;
    g_aimPitchTrimDeg = (float)I("aim_pitch_trim_tenth_deg", 0) / 10.0f;

    DebugLogger::LogFormat(
        "Aim laser config: enabled=%d submit_layer=%d dots=%d near=%.2fm far=%.2fm "
        "size=%.1fdeg trim(yaw %+.1f, pitch %+.1f)",
        g_laserEnabled ? 1 : 0, g_laserSubmitLayer ? 1 : 0, g_laserDots,
        g_laserNearM, g_laserFarM,
        g_laserSizeDeg, g_aimYawTrimDeg, g_aimPitchTrimDeg);
}

bool IsAimLaserEnabled() { return g_laserEnabled; }

// ===========================================================================
// Laser resources
// ===========================================================================

bool InitAimLaser(XrSession session, XrSpace playSpace, ID3D11Device* device,
                  ID3D11DeviceContext* context) {
    if (!g_laserEnabled) return false;
    if (g_laserReady) return true;
    if (session == XR_NULL_HANDLE || playSpace == XR_NULL_HANDLE || !device || !context) {
        return false;
    }

    g_session = session;
    g_space = playSpace;
    g_device = device;
    g_context = context;

    XrSwapchainCreateInfo info{ XR_TYPE_SWAPCHAIN_CREATE_INFO };
    info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    info.format = (int64_t)DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;   // bytes are sRGB; UNORM would be read as linear (washed-out)
    info.sampleCount = 1;
    info.width = kBeamPx;
    info.height = kBeamPx;
    info.faceCount = 1;
    info.arraySize = 1;
    info.mipCount = 1;

    XrResult xr = xrCreateSwapchain(session, &info, &g_dotSwapchain);
    if (xr != XR_SUCCESS) {
        info.format = (int64_t)DXGI_FORMAT_R8G8B8A8_UNORM;
        xr = xrCreateSwapchain(session, &info, &g_dotSwapchain);
    }
    if (xr != XR_SUCCESS) {
        DebugLogger::LogFormat("Aim laser: xrCreateSwapchain failed (%d) -- laser disabled for this run", (int)xr);
        g_dotSwapchain = XR_NULL_HANDLE;
        g_laserEnabled = false;
        return false;
    }

    uint32_t count = 0;
    xrEnumerateSwapchainImages(g_dotSwapchain, 0, &count, nullptr);
    if (count == 0) {
        DebugLogger::Log("Aim laser: swapchain reported zero images -- laser disabled");
        ShutdownAimLaser();
        g_laserEnabled = false;
        return false;
    }
    g_dotImages.resize(count, { XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR });
    xr = xrEnumerateSwapchainImages(g_dotSwapchain, count, &count,
        reinterpret_cast<XrSwapchainImageBaseHeader*>(g_dotImages.data()));
    if (xr != XR_SUCCESS) {
        DebugLogger::LogFormat("Aim laser: xrEnumerateSwapchainImages failed (%d) -- laser disabled", (int)xr);
        ShutdownAimLaser();
        g_laserEnabled = false;
        return false;
    }

    g_beamPixels.assign((size_t)kBeamPx * kBeamPx * 4u, 0);
    g_laserReady = true;
    DebugLogger::LogFormat(
        "Aim laser: ready (%u swapchain images, %dx%d beam canvas, ONE composition layer)",
        count, kBeamPx, kBeamPx);
    return true;
}

void ShutdownAimLaser() {
    // Only destroy through a live session -- destroying a swapchain whose
    // session has already gone is undefined, and session teardown takes its
    // swapchains with it anyway.
    if (g_dotSwapchain != XR_NULL_HANDLE && g_session != XR_NULL_HANDLE) {
        xrDestroySwapchain(g_dotSwapchain);
    }
    g_dotSwapchain = XR_NULL_HANDLE;
    g_dotImages.clear();
    g_beamPixels.clear();
    g_laserReady = false;
    g_session = XR_NULL_HANDLE;
    g_space = XR_NULL_HANDLE;
    g_device = nullptr;
    g_context = nullptr;
}

// ===========================================================================
// Per-frame layer build
// ===========================================================================

namespace {
XrPosef g_lastHeadPose{};
bool    g_haveHeadPose = false;
SRWLOCK g_headPoseLock = SRWLOCK_INIT;

// GM_CurrentWeaponId (linkvarbuf[14]). Own SEH helper: MSVC refuses __try in a
// function that also needs object unwinding (C2712).
int ReadCurrentWeaponId() {
    static uintptr_t base = 0;
    if (!base) base = (uintptr_t)GetModuleHandleA(nullptr);
    __try { return (int)*reinterpret_cast<volatile int16_t*>(base + 0x38E7FC); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}

// PSG1 RAISE TO EYE (2026-09-29). The rifle is a normal gun in your hand until
// the controller comes up to your face; then the scope is up -- zoomed,
// magnified, both eyes one picture, and the shot goes where you look.
// Hysteresis so it does not flicker at the edge.
void UpdateScopeState(const XrPosef& head, const XrPosef& aim, bool aimValid) {
    // 2026-09-29 (second pass): the first test never raised the scope. The
    // test was "controller tip within 20 cm of the eye", but with a rifle
    // shouldered the trigger hand sits 25-35 cm from the eye -- it could not
    // happen. What actually distinguishes "looking down the scope" is the LINE:
    // the eye sits behind the controller, on (or just above) its barrel axis,
    // and you are facing the way it points. Distance is only a loose limit.
    const int weapon = ReadCurrentWeaponId();
    const bool ctx = IsScopeFeatureEnabled() && aimValid && IsVrViewModeActive() && IsFpvActive() &&
                     !IsCutsceneVrActive() && weapon == 9 /* PSG1 */;
    bool want = false;
    float d = 0.0f, behind = 0.0f, facing = 0.0f;
    if (aimValid) {
        const float te[3] = { head.position.x - aim.position.x, head.position.y - aim.position.y,
                              head.position.z - aim.position.z };
        d = std::sqrt(te[0] * te[0] + te[1] * te[1] + te[2] * te[2]);
        float f[3];
        AimForward(TrimmedAimOrientation(aim.orientation), f);
        if (d > 1e-3f) behind = -(te[0] * f[0] + te[1] * f[1] + te[2] * f[2]) / d;   // 1 = eye straight behind the barrel
        const float z[3] = { 0.0f, 0.0f, -1.0f };
        float hf[3];
        QuatRotate(head.orientation, z, hf);
        facing = hf[0] * f[0] + hf[1] * f[1] + hf[2] * f[2];                          // 1 = looking the way it points
        if (ctx) {
            const bool up = IsScopeViewActive();
            const float maxD = up ? GetScopeLowerDistanceM() : GetScopeRaiseDistanceM();
            want = d < maxD && behind > (up ? 0.70f : 0.82f) && facing > (up ? 0.70f : 0.82f);
        }
    }
    if (want != IsScopeViewActive()) SetScopeViewActive(want);

    // Tuning line, once a second while the PSG1 is out (50 lines max), so the
    // thresholds can be set from what your own shouldered pose measures.
    static ULONGLONG last = 0;
    static int lines = 0;
    if (weapon == 9 && lines < 50 && GetTickCount64() - last > 1000) {
        last = GetTickCount64();
        ++lines;
        DebugLogger::LogFormat("Scope check: dist %.0f cm (up < %.0f) | eye behind barrel %.2f (up > 0.82) | facing along "
            "barrel %.2f (up > 0.82) | fpv=%d vr=%d -> %s",
            d * 100.0f, GetScopeRaiseDistanceM() * 100.0f, behind, facing, IsFpvActive() ? 1 : 0,
            IsVrViewModeActive() ? 1 : 0, IsScopeViewActive() ? "SCOPE UP" : "down");
    }
}
} // namespace

bool GetLastHeadPose(XrPosef* out) {
    if (!out) return false;
    AcquireSRWLockShared(&g_headPoseLock);
    const bool ok = g_haveHeadPose;
    *out = g_lastHeadPose;
    ReleaseSRWLockShared(&g_headPoseLock);
    return ok;
}

void BuildAimLaserLayers(XrTime displayTime,
                         const XrPosef& headPose,
                         std::vector<const XrCompositionLayerBaseHeader*>& outLayers) {
    (void)displayTime;   // the quad is placed in the play space, not timed
    AcquireSRWLockExclusive(&g_headPoseLock);
    g_lastHeadPose = headPose; g_haveHeadPose = true;
    ReleaseSRWLockExclusive(&g_headPoseLock);
    UpdateScopeState(headPose, g_aimPose, g_aimPoseValid);

    // Motion aim / gun in hand gets the head and the TRIMMED aim pose every VR
    // frame -- before the laser's own early-outs, so it works with the laser
    // off. Same ray the dots are drawn along, by construction.
    {
        XrPosef trimmed = g_aimPose;
        trimmed.orientation = TrimmedAimOrientation(g_aimPose.orientation);
        MotionAimPublishXrFrame(headPose, trimmed, g_aimPoseValid);
    }

    if (!g_laserEnabled || !g_laserReady || !g_aimPoseValid) return;
    // No laser while the scope is up (you aim with the reticle) or with the
    // Stinger (aimed with your head, through its own sight).
    if (IsScopeViewActive() || ReadCurrentWeaponId() == 4) return;
    if (g_beamPixels.size() != (size_t)kBeamPx * kBeamPx * 4u) return;

    // ---- where the beam's dots are, in the world ---------------------------
    const XrQuaternionf aimQ = TrimmedAimOrientation(g_aimPose.orientation);
    float fwd[3];
    AimForward(aimQ, fwd);

    const float step = (g_laserDots > 1)
        ? (g_laserFarM - g_laserNearM) / (float)(g_laserDots - 1)
        : 0.0f;

    // ---- project them onto the quad's plane --------------------------------
    // The quad is a screen hanging kQuadDistM in front of the head, facing it.
    // Projecting each dot's true 3D position onto that screen from the head's
    // eyepoint reproduces exactly what one-quad-per-dot drew, because a
    // perspective projection is what the compositor was doing anyway. The
    // difference is that we do it once, on the CPU, into one texture.
    const float halfExtentM = kQuadDistM * std::tan(kQuadHalfFovDeg * kPi / 180.0f);
    const float pxPerM = (float)kBeamPx * 0.5f / halfExtentM;

    // A dot of constant angular size lands at a constant texel size on a plane
    // at fixed distance -- so the "even row of dots that does not shrink into
    // the distance" property survives the change for free.
    const float dotRadiusPx =
        kQuadDistM * std::tan(g_laserSizeDeg * 0.5f * kPi / 180.0f) * pxPerM;

    // Inverse of the head rotation, to take world offsets into head space.
    XrQuaternionf headInv = headPose.orientation;
    headInv.x = -headInv.x; headInv.y = -headInv.y; headInv.z = -headInv.z;

    std::fill(g_beamPixels.begin(), g_beamPixels.end(), (uint8_t)0);
    int drawn = 0;

    for (int i = 0; i < g_laserDots; ++i) {
        const float d = g_laserNearM + step * (float)i;

        const float world[3] = {
            g_aimPose.position.x + fwd[0] * d - headPose.position.x,
            g_aimPose.position.y + fwd[1] * d - headPose.position.y,
            g_aimPose.position.z + fwd[2] * d - headPose.position.z,
        };
        float local[3];
        QuatRotate(headInv, world, local);

        // OpenXR looks down -Z. Anything at or behind the head plane has no
        // projection; drop it rather than letting it mirror to the far side.
        if (local[2] > -0.05f) continue;

        const float invZ = kQuadDistM / -local[2];
        const float px = ((local[0] * invZ) * pxPerM) + (float)kBeamPx * 0.5f;
        // Texture rows run downward, world Y runs upward.
        const float py = (float)kBeamPx * 0.5f - ((local[1] * invZ) * pxPerM);

        // Fade the far end slightly so the beam reads as having direction.
        const float t = (g_laserDots > 1) ? (float)i / (float)(g_laserDots - 1) : 0.0f;
        StampDot(g_beamPixels, px, py, dotRadiusPx, 1.0f - 0.35f * t);
        drawn++;
    }

    if (drawn == 0) return;   // beam entirely behind or outside the canvas

    // DIAGNOSTIC SPLIT (laser_submit_layer=0). Everything above and below runs
    // -- the CPU rasterise, the acquire/wait/release, the UpdateSubresource --
    // but the layer is never handed to xrEndFrame. That separates "our
    // swapchain traffic costs something" from "an extra composition layer
    // costs something", which two headset sessions have failed to tell apart.
    // See the note in vr_aim.h.

    // ---- upload and submit, as ONE layer -----------------------------------
    uint32_t imageIndex = 0;
    XrSwapchainImageAcquireInfo acq{ XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
    if (xrAcquireSwapchainImage(g_dotSwapchain, &acq, &imageIndex) != XR_SUCCESS) return;

    bool uploaded = false;
    XrSwapchainImageWaitInfo wait{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
    wait.timeout = XR_INFINITE_DURATION;
    if (xrWaitSwapchainImage(g_dotSwapchain, &wait) == XR_SUCCESS &&
        imageIndex < g_dotImages.size() && g_dotImages[imageIndex].texture) {
        g_context->UpdateSubresource(g_dotImages[imageIndex].texture, 0, nullptr,
            g_beamPixels.data(), (UINT)(kBeamPx * 4), 0);
        uploaded = true;
    }
    XrSwapchainImageReleaseInfo rel{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
    xrReleaseSwapchainImage(g_dotSwapchain, &rel);
    if (!uploaded) return;

    g_beamQuad = XrCompositionLayerQuad{ XR_TYPE_COMPOSITION_LAYER_QUAD };
    g_beamQuad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT |
                            XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
    g_beamQuad.space = g_space;
    g_beamQuad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    g_beamQuad.subImage.swapchain = g_dotSwapchain;
    g_beamQuad.subImage.imageRect.offset = { 0, 0 };
    g_beamQuad.subImage.imageRect.extent = { kBeamPx, kBeamPx };
    g_beamQuad.subImage.imageArrayIndex = 0;

    float quadFwd[3];
    const float fz[3] = { 0.0f, 0.0f, -1.0f };
    QuatRotate(headPose.orientation, fz, quadFwd);
    g_beamQuad.pose.position.x = headPose.position.x + quadFwd[0] * kQuadDistM;
    g_beamQuad.pose.position.y = headPose.position.y + quadFwd[1] * kQuadDistM;
    g_beamQuad.pose.position.z = headPose.position.z + quadFwd[2] * kQuadDistM;
    g_beamQuad.pose.orientation = headPose.orientation;
    g_beamQuad.size.width = halfExtentM * 2.0f;
    g_beamQuad.size.height = halfExtentM * 2.0f;

    if (g_laserSubmitLayer) {
        outLayers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&g_beamQuad));
    }

    static bool loggedOnce = false;
    if (!loggedOnce) {
        loggedOnce = true;
        DebugLogger::LogFormat(
            "Aim laser: beam drawn into a single quad layer (%d dots, %.1f px radius, "
            "quad %.2fm ahead spanning %.0f deg) -- submit_layer=%d",
            g_laserDots, dotRadiusPx, kQuadDistM, kQuadHalfFovDeg * 2.0f,
            g_laserSubmitLayer ? 1 : 0);
        if (!g_laserSubmitLayer) {
            DebugLogger::Log(
                "Aim laser: DIAGNOSTIC MODE -- swapchain is being acquired, written and "
                "released every frame, but the layer is NOT submitted. You will see no "
                "beam. If the frame rate holds, the cost is the composition layer; if it "
                "still collapses, the cost is our swapchain traffic.");
        }
        if (dotRadiusPx < 1.5f) {
            DebugLogger::LogFormat(
                "Aim laser: dot radius is only %.1f px -- the beam will be nearly "
                "invisible. Raise laser_size_tenth_deg (try 15) or the canvas.",
                dotRadiusPx);
        }
    }
}

// ===========================================================================
// WRIST HUD (2026-09-23)
//
// LIFE (and O2) on the left wrist, the equipped weapon on the right, the
// equipped item under LIFE -- like a watch you glance at. While a grip is held
// (the game's own weapon / item selection is open) the panel on that wrist
// becomes a window onto the selection instead, so it is readable without
// looking at the far corners of a 106-degree view.
//
// No new art. Two sources, both the game's own:
//   * LIFE / O2 / names are read from the live linkvar block (mgsi.exe+38E7E0,
//     the same block motion_aim already reads the current weapon from) and
//     drawn with a tiny built-in pixel font.
//   * The weapon / item BOXES and the selection lists are CROPPED from the
//     captured game frame -- the real icons and ammo counters, just moved.
//     Positions from the FoxdieTeam decomp: PANEL_CONF {16,184} (items) and
//     {256,184} (weapons) in 320x240, box x-4..x+47, y..y+29; doubled for the
//     PC's 640x480. All four crop rects are ini-tunable fractions.
//
// ONE swapchain (512x256: left half = left wrist, right half = right wrist),
// two small quads. Re-uploaded only when something changed, at most ~30 Hz.
// ===========================================================================
#include "../include/ddraw_hook.h"
#include "../include/camera_write_hook.h"
#include <mutex>
#include "MinHook.h"

namespace {

// ---- config [wrist_hud] ----------------------------------------------------
bool  w_enabled = true;
float w_widthM = 0.09f;
float w_offL[3] = { -0.010f, 0.040f, 0.100f };   // grip space, metres (+X right, +Y up, +Z back toward elbow)
float w_offR[3] = {  0.010f, 0.040f, 0.100f };
float w_rotL[3] = { -60.0f, 0.0f, 0.0f };          // pitch, yaw, roll (deg) in grip space
float w_rotR[3] = { -60.0f, 0.0f, 0.0f };
// 2026-10-02: where the panels mount. 1 = the BACK OF THE WRIST, like a watch:
// twist the forearm (back of the wrist toward your eyes) to read it instead of
// lifting the arm. 0 = the old spot over the back of the hand.
// LIFE bars drawn at the game's own proportions (2026-10-02): the game draws
// every life bar max/8 pixels long on its 320-wide screen (decomp menu/life.c),
// so Snake's starts short and grows after each boss, and a boss's is as long
// as its own max. 1 game pixel = w_barScale panel pixels (panel is 256 wide).
bool  w_barFollowsMax = true;
float w_barScale = 1.25f;
int   w_mount = 1;
int   w_mountSide[2] = { 0, 0 };    // per wrist, info panel: 0 top of the hand, 1 back of wrist, 2 inside of wrist
int   w_listMount = 2;              // where an OPEN item / weapon list goes (2026-10-02 round 5: inside of the wrist)
// Inside of the wrist (palm side), like a watch worn face-in: turn the palm
// up toward you to read it.
float w_inOffL[3] = {  0.030f, 0.010f, 0.070f };
float w_inOffR[3] = { -0.030f, 0.010f, 0.070f };
float w_inRotL[3] = { 0.0f, 0.0f, 0.0f };
float w_inRotR[3] = { 0.0f, 0.0f, 0.0f };
float w_backOffL[3] = { -0.030f, 0.010f, 0.070f };  // grip space, metres (round 3: closer to the hand)
float w_backOffR[3] = {  0.030f, 0.010f, 0.070f };
// Trim on top of the watch-face orientation (pitch, yaw, roll). Pitch +45
// tilts the face from straight out of the back of the wrist halfway toward the
// thumb side, so a small turn of the forearm brings it to your eyes.
float w_backRotL[3] = { 45.0f, 0.0f, 0.0f };
float w_backRotR[3] = { 45.0f, 0.0f, 0.0f };
float w_gazeConeDeg = 28.0f;                       // show when the panel is this close to where you look (0 = always)
bool  w_maskBoxesInView = true;                    // hide the corner boxes in the main view (they are on your wrists now)
bool  w_maskSelectionInView = false;               // game-menu mode only: hide the game's selection strip while a grip is held
bool  w_hideLifeInView = true;                     // hide the game's own LIFE / O2 bars (they are on the left wrist)
bool  w_showOtherBars = true;                      // boss / Meryl / other LIFE bars on the left wrist too
bool  w_hideOtherBarsInView = true;                // and hide those in the view
int   w_hideStyle = 1;                             // 1 = fill from the surrounding picture, 0 = black
bool  w_cropBoxes = true;
float w_itemBox[4]   = { 0.030f, 0.755f, 0.180f, 0.140f };   // x, y, w, h fractions of the frame
float w_weaponBox[4] = { 0.780f, 0.755f, 0.180f, 0.140f };
// The game's own scrolling selection (decomp PANEL_CONF, 320x240): the item
// strip grows right from (16,184) by 56 px per slot and up by 40; the weapon
// strip mirrors it from (256,184). These cover the visible part of the scroll.
float w_itemSel[4]   = { 0.030f, 0.420f, 0.530f, 0.480f };
float w_weaponSel[4] = { 0.430f, 0.420f, 0.540f, 0.480f };
float w_life[4]      = { -1.0f, 0.0f, 0.0f, 0.0f };   // x<0 = derive from max LIFE (bar at 16,16; width max/8)
// Radar: decomp menu/radar.c draws it into a 69x52 clip at (235,16) of 320x240
// (plus its frame), i.e. the top-right corner.
float w_radar[4]     = { 0.722f, 0.050f, 0.244f, 0.258f };
bool  w_showRadar = true;
bool  w_hideRadarInView = true;
bool  w_selectOnWrist = true;      // 1 = compact wrist list (default); 0 = the game's own menu, cropped onto the wrist
int   w_logFrames = 3;

// ---- published grips (XR frame thread) -----------------------------------
XrPosef w_grip[2]{};
bool    w_gripValid[2] = { false, false };
bool    w_gripHeld[2] = { false, false };

// ---- resources ------------------------------------------------------------
XrSession   w_session = XR_NULL_HANDLE;
XrSpace     w_space = XR_NULL_HANDLE;
ID3D11DeviceContext* w_context = nullptr;
XrSwapchain w_swapchain = XR_NULL_HANDLE;
std::vector<XrSwapchainImageD3D11KHR> w_images;
bool w_ready = false;
bool w_haveReleased = false;
constexpr int kWristW = 512, kWristH = 320, kPanel = 256;
// Rows kPanel..kWristH-1 (512x64) hold the cutscene subtitle strip (2026-10-02).
constexpr int kSubY = kPanel, kSubH = kWristH - kPanel;
std::vector<uint8_t> w_pixels;       // 512x256 RGBA
int w_panelH[2] = { 160, 112 };      // used height of each half
XrCompositionLayerQuad w_quad[2]{};

// ---- frame crops (copied on the XR thread when a new game frame arrives) ----
struct Crop { std::vector<uint8_t> rgba; int w = 0, h = 0; };
Crop w_crop[5];
// --- CUTSCENE SUBTITLES (2026-10-02) ---------------------------------------
// In Snake's eyes the captions sit on the demo's bottom bar, at the very
// bottom edge of the lens where nobody can read them. The strip is lifted out
// of each frame onto its own panel just below your gaze (and painted out of
// the main view), shown only while it has text on it.
Crop     w_subCrop;
bool     w_subHave = false;
ULONGLONG w_subFromBitmap = 0;   // last time the caption came from the game's bitmap (frame crop then stays out)
ULONGLONG w_subLastText = 0;
float    w_subRect[4] = { 0.0f, 184.0f / 240.0f, 1.0f, 40.0f / 240.0f };
float    w_subWidthM = 0.80f, w_subDistM = 1.10f, w_subDownDeg = 20.0f;
XrCompositionLayerQuad w_subQuad{ XR_TYPE_COMPOSITION_LAYER_QUAD };
bool SubtitlesShowing() {
    return IsSubtitlePanelEnabled() && IsVrViewModeActive() && w_subLastText != 0 &&
           GetTickCount64() - w_subLastText < 400;
}
// --- CAPTIONS FROM THE GAME ITSELF (2026-10-02 round 8) -----------------------
// "Subtitles never appear in VR, but they do in the plain GOG game." They ARE
// drawn -- by chara\others\jimaku (+462A3D) as a sprite centred at y = 204 of
// the 240-line screen (or 112 in its second mode). In VR that is 35-40 degrees
// below the centre of the lens: past the bottom edge of what the headset
// shows, so they were never readable anywhere, gameplay or cutscene.
// The jimaku state says exactly when and where a caption is up:
//   0x733820 byte  1 = a caption is built and being drawn
//   0x733824 short x (centred: (320 - width) / 2)
//   0x733826 short y (top)       0x73382A short height
//   0x78E7E5 bit 0x40 = the game is not drawing captions right now
// That rectangle is lifted onto the caption panel below your gaze and painted
// out of the view, in first person, Snake's eyes and cutscenes alike.
bool ReadGameShortSafe(uintptr_t rva, int16_t* out);
bool GameCaptionRect(float r[4]) {
    int16_t flag = 0, x = 0, y = 0, hgt = 0, st = 0;
    if (!ReadGameShortSafe(0x333820, &flag) || (flag & 0xFF) != 1) return false;
    if (ReadGameShortSafe(0x38E7E4, &st) && (((uint16_t)st >> 8) & 0x40)) return false;
    if (!ReadGameShortSafe(0x333824, &x) || !ReadGameShortSafe(0x333826, &y) || !ReadGameShortSafe(0x33382A, &hgt))
        return false;
    if (x < 0 || x >= 158 || y < 0 || y >= 236 || hgt < 6 || hgt > 120) return false;
    const float pad = 3.0f;
    float x0 = (float)x - pad, y0 = (float)y - pad, x1 = 320.0f - (float)x + pad, y1 = (float)(y + hgt) + pad;
    if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0; if (x1 > 320) x1 = 320; if (y1 > 240) y1 = 240;
    r[0] = x0 / 320.0f; r[1] = y0 / 240.0f; r[2] = (x1 - x0) / 320.0f; r[3] = (y1 - y0) / 240.0f;
    return true;
}

// --- CAPTION STRAIGHT FROM THE GAME'S TEXT BITMAP (round 9) -------------------
// The frame-capture route found the caption pixels in only a few frames (log
// 22:17-22:20: the panel showed once). The caption is not needed from the
// frame at all: menu\jimaku.c renders the line with font_print_string into
// its own 4-bit bitmap (the KCB at 0x73382C, decompiled in mgs_reversing
// source/font/font.c) and only then uploads it to VRAM. That bitmap is read
// here and drawn on the panel directly -- crisp, and independent of render
// twice, alternate-eye stereo or what the eye images contain.
//   KCB +0x07 max_width (byte)   +0x14 buffer (pixels, 4bpp, low nibble first)
//   KCB +0x18 row (bytes/line)   +0x1E max_height   +0x28 cbuffer (16 x RGB555)
bool SafeCopyAbs(void* dst, uintptr_t src, size_t n) {
    __try { std::memcpy(dst, reinterpret_cast<const void*>(src), n); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// Round 10: the "line of pixels". The PC port does NOT draw caption pixels into
// that buffer: with its high-resolution text switched on (0x6FC7AC set by
// jimaku around the print, +462C8A) font_draw_string (+45AB2D) writes TEXT
// RUNS there instead (+4242CB / +42431F), which its renderer later draws with
// a Windows font -- and the captured eye images never contain them. Each run:
//   [0] count  [1] x  [2] y  [3] advance  [4] colour  [5..6] flags  [7..] ASCII
// and the next run follows at count + 7; a count of 0 ends the list. So the
// caption is read as text here and typeset onto the panel with GDI.
bool GameCaptionLines(std::string lines[3], int* nLines, uint32_t* sigOut) {
    static uintptr_t base = 0;
    if (!base) base = (uintptr_t)GetModuleHandleA(nullptr);
    float r[4];
    if (!GameCaptionRect(r)) return false;
    const uintptr_t kcb = base + 0x33382C;
    uint32_t buf = 0; int16_t row = 0, height = 0;
    if (!SafeCopyAbs(&buf, kcb + 0x14, 4) || !SafeCopyAbs(&row, kcb + 0x18, 2) || !SafeCopyAbs(&height, kcb + 0x1C, 2))
        return false;
    if (buf < 0x00400000u || buf >= 0x7FFF0000u) return false;
    size_t cap = (row > 0 && height > 0) ? (size_t)row * (size_t)height : 0;
    if (cap < 64) cap = 64;
    if (cap > 16384) cap = 16384;
    static std::vector<uint8_t> b;
    b.resize(cap);
    if (!SafeCopyAbs(b.data(), buf, cap)) return false;
    int ys[3] = { -1, -1, -1 };
    *nLines = 0;
    uint32_t sig = 2166136261u;
    size_t p = 0;
    for (int guard = 0; guard < 64 && p + 7 < cap; ++guard) {
        const int n = b[p];
        if (n == 0 || n == 0xFF || p + 7 + (size_t)n > cap) break;
        const int y = b[p + 2];
        int li = -1;
        for (int k = 0; k < *nLines; ++k) if (ys[k] == y) li = k;
        if (li < 0) {
            if (*nLines >= 3) break;
            li = (*nLines)++;
            ys[li] = y;
            lines[li].clear();
        }
        for (int i = 0; i < n; ++i) {
            const uint8_t c = b[p + 7 + i];
            if (c >= 0x20 && c < 0x7F) { lines[li] += (char)c; sig = (sig ^ c) * 16777619u; }
        }
        sig = (sig ^ (uint32_t)(y + 1000)) * 16777619u;
        p += 7 + (size_t)n;
    }
    for (int i = 0; i < *nLines; ++i)
        for (int j = i + 1; j < *nLines; ++j)
            if (ys[j] < ys[i]) { std::swap(ys[i], ys[j]); std::swap(lines[i], lines[j]); }
    bool any = false;
    for (int i = 0; i < *nLines; ++i) if (!lines[i].empty()) any = true;
    if (!any) return false;
    if (sigOut) *sigOut = sig;
    return true;
}

// Typesets the lines into a 512 x 64 RGBA crop (white, dark outline) with GDI.
bool TypesetCaption(const std::string lines[3], int nLines, Crop& out) {
    const int W = 512, H = 64;
    HDC dc = CreateCompatibleDC(nullptr);
    if (!dc) return false;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = W; bi.bmiHeader.biHeight = -H;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp || !bits) { if (bmp) DeleteObject(bmp); DeleteDC(dc); return false; }
    HGDIOBJ oldBmp = SelectObject(dc, bmp);
    std::memset(bits, 0, (size_t)W * H * 4u);
    const int lineH = nLines <= 1 ? 40 : (nLines == 2 ? 30 : 20);
    int fontPx = lineH - 4;
    HFONT font = nullptr;
    for (; fontPx >= 12; fontPx -= 2) {               // shrink until the widest line fits
        if (font) DeleteObject(font);
        font = CreateFontA(-fontPx, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, ANSI_CHARSET, OUT_DEFAULT_PRECIS,
                           CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, "Arial");
        if (!font) break;
        SelectObject(dc, font);
        int widest = 0;
        for (int i = 0; i < nLines; ++i) {
            SIZE sz{};
            GetTextExtentPoint32A(dc, lines[i].c_str(), (int)lines[i].size(), &sz);
            if (sz.cx > widest) widest = sz.cx;
        }
        if (widest <= W - 12) break;
    }
    if (font) SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    const int top = (H - lineH * nLines) / 2;
    for (int pass = 0; pass < 2; ++pass) {
        SetTextColor(dc, pass == 0 ? RGB(1, 1, 1) : RGB(255, 255, 255));
        for (int i = 0; i < nLines; ++i) {
            SIZE sz{};
            GetTextExtentPoint32A(dc, lines[i].c_str(), (int)lines[i].size(), &sz);
            const int x = (W - sz.cx) / 2, y = top + i * lineH + (lineH - sz.cy) / 2;
            if (pass == 0) {
                for (int dy = -2; dy <= 2; ++dy) for (int dx = -2; dx <= 2; ++dx)
                    if (dx || dy) TextOutA(dc, x + dx, y + dy, lines[i].c_str(), (int)lines[i].size());
            }
            else TextOutA(dc, x, y, lines[i].c_str(), (int)lines[i].size());
        }
    }
    GdiFlush();
    out.w = W; out.h = H;
    out.rgba.resize((size_t)W * H * 4u);
    const uint8_t* src = (const uint8_t*)bits;
    for (size_t i = 0; i < (size_t)W * H; ++i) {
        out.rgba[i * 4 + 0] = src[i * 4 + 2];
        out.rgba[i * 4 + 1] = src[i * 4 + 1];
        out.rgba[i * 4 + 2] = src[i * 4 + 0];
        out.rgba[i * 4 + 3] = 255;
    }
    SelectObject(dc, oldBmp);
    if (font) DeleteObject(font);
    DeleteObject(bmp);
    DeleteDC(dc);
    return true;
}

bool GameCaptionBitmap(Crop& out, uint32_t* sigOut) {
    std::string lines[3];
    int n = 0;
    uint32_t sig = 0;
    if (!GameCaptionLines(lines, &n, &sig)) return false;
    static uint32_t s_lastSig = 0;
    static Crop s_last;
    if (sig != s_lastSig || s_last.w == 0) {
        if (!TypesetCaption(lines, n, s_last)) return false;
        s_lastSig = sig;
        static int s_log = 0;
        if (s_log < 40) {
            s_log++;
            DebugLogger::LogFormat("Captions: \"%s%s%s%s%s\"", lines[0].c_str(), n > 1 ? " / " : "",
                n > 1 ? lines[1].c_str() : "", n > 2 ? " / " : "", n > 2 ? lines[2].c_str() : "");
        }
    }
    out = s_last;
    if (sigOut) *sigOut = sig;
    return true;
}                      // 0 item box, 1 weapon box, 2 item sel, 3 weapon sel, 4 radar
// --- THE GAME'S HI-RES TEXT ON THE VIRTUAL SCREEN (2026-10-03) ---------------
// "Subtitles show in the VR session, but on the virtual screen they never
// appear in cutscenes or codec calls." The PC port does not draw its text into
// the 3D picture. Every hi-res string (captions, codec lines, ...) is a "text"
// primitive in the draw list, rendered by mgsi.exe+24020:
//     EndScene ; backbuffer->GetDC ; SelectObject([0x6FC7E8] font) ;
//     SetMapMode(1) ; SetBkMode(TRANSPARENT) ; SetTextColor ;
//     for each node: TextOutA(dc, node.x + vtx.x, node.y + vtx.y, node.text)
//     ReleaseDC ; BeginScene
// i.e. GDI onto the wrapper's back buffer, which the eye capture (the render
// target the 3D draws went into) never contains. In the VR view the caption
// panel reads the jimaku state instead; everywhere else -- the native virtual
// screen, PC menus, codec / cutscenes on the 2D frame -- the text was simply
// missing. So +24020 is hooked: each call's strings are recorded with their
// final position and colour, and typeset into the captured frame here.
//   node: +0 x (int, back-buffer px)  +4 y  +0x10 char[256]  +0x114 next
//   vtx : +0 x (float)  +4 y (float)  +0x10 B  +0x11 G  +0x12 R (doubled,
//         clamped, on the hardware path: [0x6FC794] != 0 || [0x650D30] != 0)
//   [0x650D28] float: resolution scale (back buffer = 320 x 240 times it)
using GameTextFn = void(__cdecl*)(void* list, void* vtx, int flag);
GameTextFn g_origGameText = nullptr;
constexpr uintptr_t kGameTextRva = 0x024020;
bool g_gtDrawOnScreen = true;            // [subtitles] draw_on_screen
struct GameTextItem { float x = 0, y = 0; uint32_t rgb = 0; char s[96] = {}; ULONGLONG tick = 0; };
constexpr int kMaxGameText = 48;
GameTextItem g_gt[kMaxGameText];
std::mutex   g_gtMutex;
std::atomic<long> g_gtCalls{ 0 };

// SEH only: no C++ objects with destructors in here.
int CollectGameTextRuns(void* list, const void* vtx, GameTextItem* out, int maxOut) {
    int n = 0;
    __try {
        const uintptr_t base = (uintptr_t)GetModuleHandleA(nullptr);
        const uint8_t* v = reinterpret_cast<const uint8_t*>(vtx);
        const float vx = *reinterpret_cast<const float*>(v + 0), vy = *reinterpret_cast<const float*>(v + 4);
        int r = v[0x12], g = v[0x11], b = v[0x10];
        const bool hw = *reinterpret_cast<const int32_t*>(base + 0x2FC794) != 0 ||
                        *reinterpret_cast<const int32_t*>(base + 0x250D30) != 0;
        if (hw) { r = (std::min)(255, r * 2); g = (std::min)(255, g * 2); b = (std::min)(255, b * 2); }
        const uint8_t* node = reinterpret_cast<const uint8_t*>(list);
        for (int guard = 0; node && guard < 32 && n < maxOut; ++guard) {
            GameTextItem& it = out[n];
            it.x = (float)*reinterpret_cast<const int32_t*>(node + 0) + vx;
            it.y = (float)*reinterpret_cast<const int32_t*>(node + 4) + vy;
            it.rgb = (uint32_t)r | ((uint32_t)g << 8) | ((uint32_t)b << 16);
            int k = 0;
            for (int i = 0; i < 255 && k < (int)sizeof(it.s) - 1; ++i) {
                const uint8_t c = node[0x10 + i];
                if (!c) break;
                it.s[k++] = (c >= 0x20 && c != 0x7F) ? (char)c : ' ';
            }
            it.s[k] = 0;
            if (k > 0) ++n;
            node = *reinterpret_cast<uint8_t* const*>(node + 0x114);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    return n;
}

void StoreGameText(const GameTextItem* in, int n) {
    const ULONGLONG now = GetTickCount64();
    std::lock_guard<std::mutex> lk(g_gtMutex);
    for (int i = 0; i < n; ++i) {
        int slot = -1, oldest = 0;
        for (int k = 0; k < kMaxGameText; ++k) {
            GameTextItem& e = g_gt[k];
            if (e.tick && e.x == in[i].x && e.y == in[i].y && std::strcmp(e.s, in[i].s) == 0) { slot = k; break; }
            if (g_gt[k].tick < g_gt[oldest].tick) oldest = k;
        }
        if (slot < 0) {
            slot = oldest;
            static int s_log = 0;
            if (s_log < 30) {
                s_log++;
                DebugLogger::LogFormat("Game text: \"%s\" at (%.0f, %.0f) colour %06X -- drawn by the PC port with GDI, "
                    "added to the picture whenever the caption panel is not taking it", in[i].s, in[i].x, in[i].y, (unsigned)in[i].rgb);
            }
        }
        g_gt[slot] = in[i];
        g_gt[slot].tick = now;
    }
}

void __cdecl HookedGameText(void* list, void* vtx, int flag) {
    g_gtCalls++;
    if (g_gtDrawOnScreen && list && vtx) {
        GameTextItem tmp[16];
        const int n = CollectGameTextRuns(list, vtx, tmp, 16);
        if (n > 0) StoreGameText(tmp, n);
    }
    g_origGameText(list, vtx, flag);
}

// One string rendered to a coverage mask (white on black), cached by text and
// font height; the colour is applied when it is blended into the frame.
struct GameTextMask { std::string s; int fontH = 0; int w = 0, h = 0; std::vector<uint8_t> a; ULONGLONG used = 0; };
std::mutex g_gtDrawMutex;
GameTextMask g_gtCache[40];
HDC     g_gtDc = nullptr;
HFONT   g_gtFont = nullptr;
int     g_gtFontH = 0;
char    g_gtFace[LF_FACESIZE] = {};

bool ReadGameFontSafe(HFONT* out) {
    __try { *out = *reinterpret_cast<HFONT*>((uintptr_t)GetModuleHandleA(nullptr) + 0x2FC7E8); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool ReadGameScaleSafe(float* out) {
    __try { *out = *reinterpret_cast<float*>((uintptr_t)GetModuleHandleA(nullptr) + 0x250D28); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// The game's own font (its LOGFONT), at the frame's size.
bool EnsureGameTextFont(float frameScale, float gameScale) {
    LOGFONTA lf{};
    HFONT gf = nullptr;
    bool haveGame = ReadGameFontSafe(&gf) && gf && GetObjectA(gf, sizeof(lf), &lf) == sizeof(lf);
    if (!haveGame) {
        std::memset(&lf, 0, sizeof(lf));
        lf.lfHeight = -(LONG)std::lround(11.0f * gameScale);
        lf.lfWeight = FW_NORMAL;
        lf.lfCharSet = ANSI_CHARSET;
        std::memcpy(lf.lfFaceName, "Arial", 6);
    }
    int hgt = (int)std::lround((float)lf.lfHeight * frameScale);
    if (hgt == 0) hgt = -12;
    if (g_gtFont && hgt == g_gtFontH && std::strncmp(g_gtFace, lf.lfFaceName, LF_FACESIZE) == 0) return true;
    lf.lfHeight = hgt;
    lf.lfWidth = (LONG)std::lround((float)lf.lfWidth * frameScale);
    lf.lfQuality = ANTIALIASED_QUALITY;
    HFONT f = CreateFontIndirectA(&lf);
    if (!f) return false;
    if (g_gtFont) DeleteObject(g_gtFont);
    g_gtFont = f;
    g_gtFontH = hgt;
    std::memcpy(g_gtFace, lf.lfFaceName, LF_FACESIZE);
    for (GameTextMask& m : g_gtCache) { m.s.clear(); m.w = m.h = 0; }
    static int s_log = 0;
    if (s_log < 4) {
        s_log++;
        DebugLogger::LogFormat("Game text: font \"%s\" height %d for the frame (%s, game scale %.2f, frame scale %.2f)",
            lf.lfFaceName, hgt, haveGame ? "the game's own font" : "fallback Arial", gameScale, frameScale);
    }
    return true;
}

const GameTextMask* GameTextMaskFor(const char* s) {
    GameTextMask* lru = &g_gtCache[0];
    for (GameTextMask& m : g_gtCache) {
        if (m.w && m.fontH == g_gtFontH && m.s == s) { m.used = GetTickCount64(); return &m; }
        if (m.used < lru->used) lru = &m;
    }
    if (!g_gtDc) g_gtDc = CreateCompatibleDC(nullptr);
    if (!g_gtDc) return nullptr;
    HGDIOBJ oldF = SelectObject(g_gtDc, g_gtFont);
    const int len = (int)std::strlen(s);
    SIZE sz{};
    GetTextExtentPoint32A(g_gtDc, s, len, &sz);
    const int W = (std::min)((int)sz.cx + 4, 4096), H = (std::min)((int)sz.cy + 2, 512);
    if (W <= 4 || H <= 2) { SelectObject(g_gtDc, oldF); return nullptr; }
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = W; bi.bmiHeader.biHeight = -H;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(g_gtDc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp || !bits) { if (bmp) DeleteObject(bmp); SelectObject(g_gtDc, oldF); return nullptr; }
    HGDIOBJ oldB = SelectObject(g_gtDc, bmp);
    std::memset(bits, 0, (size_t)W * H * 4u);
    SetBkMode(g_gtDc, TRANSPARENT);
    SetTextAlign(g_gtDc, TA_LEFT | TA_TOP);
    SetTextColor(g_gtDc, RGB(255, 255, 255));
    TextOutA(g_gtDc, 0, 0, s, len);
    GdiFlush();
    lru->s = s;
    lru->fontH = g_gtFontH;
    lru->w = W; lru->h = H;
    lru->a.resize((size_t)W * H);
    const uint8_t* src = (const uint8_t*)bits;
    for (size_t i = 0; i < (size_t)W * H; ++i) lru->a[i] = (std::max)(src[i * 4 + 1], (std::max)(src[i * 4 + 0], src[i * 4 + 2]));
    lru->used = GetTickCount64();
    SelectObject(g_gtDc, oldB);
    SelectObject(g_gtDc, oldF);
    DeleteObject(bmp);
    return lru;
}

// Typesets every string the game drew in the last ~120 ms into the frame, at
// the place and colour the game drew it on the monitor.
void DrawGameTextIntoFrame(uint8_t* rgba, int w, int h) {
    GameTextItem items[kMaxGameText];
    int n = 0;
    {
        const ULONGLONG now = GetTickCount64();
        std::lock_guard<std::mutex> lk(g_gtMutex);
        for (int k = 0; k < kMaxGameText; ++k)
            if (g_gt[k].tick && now - g_gt[k].tick < 120) items[n++] = g_gt[k];
    }
    if (!n) return;
    float gs = 0.0f;
    if (!ReadGameScaleSafe(&gs) || !(gs >= 0.5f && gs <= 16.0f)) gs = (float)w / 320.0f;
    const float fx = (float)w / (320.0f * gs), fy = (float)h / (240.0f * gs);
    std::lock_guard<std::mutex> lk(g_gtDrawMutex);
    if (!EnsureGameTextFont(fy, gs)) return;
    for (int i = 0; i < n; ++i) {
        const GameTextMask* m = GameTextMaskFor(items[i].s);
        if (!m) continue;
        const int x0 = (int)std::lround(items[i].x * fx), y0 = (int)std::lround(items[i].y * fy);
        const uint32_t c = items[i].rgb;
        const int cr = c & 0xFF, cg = (c >> 8) & 0xFF, cb = (c >> 16) & 0xFF;
        for (int y = 0; y < m->h; ++y) {
            const int py = y0 + y;
            if (py < 0 || py >= h) continue;
            const uint8_t* a = &m->a[(size_t)y * m->w];
            uint8_t* d = rgba + ((size_t)py * w) * 4u;
            for (int x = 0; x < m->w; ++x) {
                const int px = x0 + x;
                const int al = a[x];
                if (!al || px < 0 || px >= w) continue;
                uint8_t* p = d + (size_t)px * 4u;
                p[0] = (uint8_t)((p[0] * (255 - al) + cr * al) / 255);
                p[1] = (uint8_t)((p[1] * (255 - al) + cg * al) / 255);
                p[2] = (uint8_t)((p[2] * (255 - al) + cb * al) / 255);
            }
        }
    }
    static int s_log = 0;
    if (s_log < 3) {
        s_log++;
        DebugLogger::LogFormat("Game text: %d string(s) typeset into the %dx%d picture (game scale %.2f)", n, w, h, gs);
    }
}

bool w_haveCrops = false;
uint64_t w_cropSeq = 0;
// WristHudOnNewFrame now runs on ddraw_hook's converter thread (2026-09-27),
// while RenderPanels runs on the XR thread: the crops are shared under this.
std::mutex w_cropMutex;

// ---- in-headset remap panel (vr_input.cpp owns the state) --------------------
RemapPanelView w_remap;
std::mutex w_remapMutex;

// ---- 5x7 pixel font ---------------------------------------------------------
const char* GlyphRows(char c) {
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
    default:  return nullptr;
    }
}

void PutPx(int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if (x < 0 || y < 0 || x >= kWristW || y >= kWristH) return;
    uint8_t* p = &w_pixels[((size_t)y * kWristW + x) * 4u];
    p[0] = r; p[1] = g; p[2] = b; p[3] = a;
}

void HudFill(int x, int y, int w, int h, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    for (int yy = y; yy < y + h; ++yy) for (int xx = x; xx < x + w; ++xx) PutPx(xx, yy, r, g, b, a);
}

// Text with a 1-px dark outline so it reads over anything. Returns the end x.
int HudText(int x, int y, const char* s, int scale, uint8_t r, uint8_t g, uint8_t b) {
    for (int pass = 0; pass < 2; ++pass) {
        int cx = x;
        for (const char* c = s; *c; ++c) {
            char ch = (*c >= 'a' && *c <= 'z') ? (char)(*c - 32) : *c;
            const char* rows = GlyphRows(ch);
            if (rows) {
                for (int gy = 0; gy < 7; ++gy) for (int gx = 0; gx < 5; ++gx) {
                    if (rows[gy * 5 + gx] != '1') continue;
                    if (pass == 0) HudFill(cx + gx * scale - 1, y + gy * scale - 1, scale + 2, scale + 2, 0, 0, 0, 255);
                    else           HudFill(cx + gx * scale, y + gy * scale, scale, scale, r, g, b, 255);
                }
            }
            cx += 6 * scale;
        }
        if (pass == 1) return cx;
    }
    return x;
}

// Bilinear blit of a crop into the canvas, fitted into (x, y, maxW, maxH).
// Returns the height used.
int BlitCrop(const Crop& c, int x, int y, int maxW, int maxH) {
    if (c.w <= 1 || c.h <= 1) return 0;
    float s = (std::min)((float)maxW / (float)c.w, (float)maxH / (float)c.h);
    const int dw = (std::max)(1, (int)(c.w * s)), dh = (std::max)(1, (int)(c.h * s));
    for (int yy = 0; yy < dh; ++yy) {
        const float sy = ((float)yy + 0.5f) / s - 0.5f;
        const int y0 = (std::max)(0, (std::min)(c.h - 1, (int)std::floor(sy))), y1 = (std::min)(c.h - 1, y0 + 1);
        const float fy = (std::max)(0.0f, (std::min)(1.0f, sy - (float)y0));
        for (int xx = 0; xx < dw; ++xx) {
            const float sx = ((float)xx + 0.5f) / s - 0.5f;
            const int x0 = (std::max)(0, (std::min)(c.w - 1, (int)std::floor(sx))), x1 = (std::min)(c.w - 1, x0 + 1);
            const float fx = (std::max)(0.0f, (std::min)(1.0f, sx - (float)x0));
            uint8_t out[3];
            for (int k = 0; k < 3; ++k) {
                const float a = c.rgba[((size_t)y0 * c.w + x0) * 4 + k] * (1 - fx) + c.rgba[((size_t)y0 * c.w + x1) * 4 + k] * fx;
                const float b = c.rgba[((size_t)y1 * c.w + x0) * 4 + k] * (1 - fx) + c.rgba[((size_t)y1 * c.w + x1) * 4 + k] * fx;
                out[k] = (uint8_t)(a * (1 - fy) + b * fy + 0.5f);
            }
            PutPx(x + xx, y + yy, out[0], out[1], out[2], 235);
        }
    }
    return dh;
}

// ---- game state -------------------------------------------------------------
struct GameHud { int vit = 0, vitMax = 0, o2 = 1024, weapon = -1, item = -1, ammo = 0, ammoMax = 0, itemCount = 0, modal = 0; bool ok = false; };

bool ReadGameShortSafe(uintptr_t rva, int16_t* out) {
    static uintptr_t base = 0;
    if (!base) base = (uintptr_t)GetModuleHandleA(nullptr);
    __try { *out = *reinterpret_cast<volatile int16_t*>(base + rva); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool ReadGameByteSafe(uintptr_t rva, uint8_t* out) {
    static uintptr_t base = 0;
    if (!base) base = (uintptr_t)GetModuleHandleA(nullptr);
    __try { *out = *reinterpret_cast<volatile uint8_t*>(base + rva); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

constexpr uintptr_t kLinkvar = 0x38E7E0;   // linkvarbuf[0]; [14] = 0x38E7FC is motion_aim's kCurWeaponRva
GameHud ReadGameHud() {
    GameHud h;
    int16_t v = 0;
    auto lv = [&](int idx, int* out) { if (ReadGameShortSafe(kLinkvar + idx * 2, &v)) { *out = v; return true; } return false; };
    h.ok = lv(11, &h.vit) && lv(12, &h.vitMax) && lv(14, &h.weapon) && lv(15, &h.item);
    if (h.weapon >= 0 && h.weapon < 10) { lv(17 + h.weapon, &h.ammo); lv(27 + h.weapon, &h.ammoMax); }
    if (h.item >= 0 && h.item < 24) lv(37 + h.item, &h.itemCount);
    if (ReadGameShortSafe(0x595348, &v)) h.o2 = v;        // community table "O2", 1024 = full
    uint8_t m = 0;
    if (ReadGameByteSafe(0x391A0C, &m)) h.modal = m;      // 0 gameplay, 1 pause, 4 inventory
    return h;
}

const char* kWeaponNames[10] = { "SOCOM", "FA-MAS", "GRENADE", "NIKITA", "STINGER", "CLAYMORE", "C4", "STUN.G", "CHAFF.G", "PSG1" };
const char* kItemNames[24] = { "CIGS", "SCOPE", "C.BOX A", "C.BOX B", "C.BOX C", "N.V.G", "THERM.G", "GASMASK",
    "B.ARMOR", "KETCHUP", "STEALTH", "BANDANA", "CAMERA", "RATION", "MEDICINE", "DIAZEPAM", "PAL KEY", "CARD",
    "TIMER.B", "MINE.D", "DISC", "ROPE", "SCARF", "SUPPR." };

void ReadFloatsSection(const std::string& ini, const char* section, const char* key, float* out, int n);
void ReadFloats(const std::string& ini, const char* key, float* out, int n) {
    ReadFloatsSection(ini, "wrist_hud", key, out, n);
}
void ReadFloatsSection(const std::string& ini, const char* section, const char* key, float* out, int n) {
    char buf[128] = {};
    GetPrivateProfileStringA(section, key, "", buf, sizeof(buf), ini.c_str());
    if (!buf[0]) return;
    float t[4] = {};
    int got = 0;
    char* p = buf;
    while (got < n && *p) {
        char* end = nullptr;
        const float f = std::strtof(p, &end);
        if (end == p) break;
        t[got++] = f;
        p = end;
        while (*p == ',' || *p == ' ') ++p;
    }
    if (got == n) for (int i = 0; i < n; ++i) out[i] = t[i];
}

bool HudContextActive(const GameHud& h) {
    return w_enabled && IsVrViewModeActive() && (IsFpvActive() || IsThirdPersonVrActive()) &&
           (!IsCutsceneVrActive() || IsGameplayPovActive()) &&   // REX / jeep: stalled camera but still play
           !IsDdrawMenuOverlayActive() && h.ok && (h.modal == 0 || h.modal == 4);
}

void CopyCrop(const uint8_t* rgba, int w, int h, const float r[4], Crop& c) {
    int x0 = (int)(r[0] * w), y0 = (int)(r[1] * h);
    int cw = (int)(r[2] * w), ch = (int)(r[3] * h);
    x0 = (std::max)(0, (std::min)(w - 1, x0)); y0 = (std::max)(0, (std::min)(h - 1, y0));
    cw = (std::max)(1, (std::min)(w - x0, cw)); ch = (std::max)(1, (std::min)(h - y0, ch));
    c.w = cw; c.h = ch;
    c.rgba.resize((size_t)cw * ch * 4u);
    for (int y = 0; y < ch; ++y)
        std::memcpy(&c.rgba[(size_t)y * cw * 4u], rgba + ((size_t)(y0 + y) * w + x0) * 4u, (size_t)cw * 4u);
}

// Hides a HUD rectangle. Style 0 paints it black (old behaviour); style 1
// fills it from the picture around it: each pixel is an inverse-square-
// distance blend of the (blurred) rows/columns just outside the four edges,
// so the patch continues the surrounding colours instead of leaving a hole.
// Edges that touch the frame border are simply not used.
void BlurLine(std::vector<float>& v, int n, int radius) {
    if (n <= 1) return;
    std::vector<float> t(v);
    for (int i = 0; i < n; ++i)
        for (int c = 0; c < 3; ++c) {
            float acc = 0; int cnt = 0;
            for (int k = -radius; k <= radius; ++k) { int j = i + k; if (j < 0 || j >= n) continue; acc += t[(size_t)j * 3 + c]; ++cnt; }
            v[(size_t)i * 3 + c] = acc / (float)(std::max)(1, cnt);
        }
}

void MaskRect(uint8_t* rgba, int w, int h, const float r[4]) {
    int x0 = (int)(r[0] * w), y0 = (int)(r[1] * h);
    int x1 = (int)((r[0] + r[2]) * w), y1 = (int)((r[1] + r[3]) * h);
    x0 = (std::max)(0, x0); y0 = (std::max)(0, y0); x1 = (std::min)(w, x1); y1 = (std::min)(h, y1);
    if (x1 <= x0 || y1 <= y0) return;
    const bool haveL = x0 > 1, haveR = x1 < w - 1, haveT = y0 > 1, haveB = y1 < h - 1;
    if (w_hideStyle == 0 || !(haveL || haveR || haveT || haveB)) {
        for (int y = y0; y < y1; ++y) {
            uint8_t* p = rgba + ((size_t)y * w + x0) * 4u;
            for (int x = x0; x < x1; ++x, p += 4) { p[0] = 0; p[1] = 0; p[2] = 0; p[3] = 255; }
        }
        return;
    }
    const int rw = x1 - x0, rh = y1 - y0;
    // Sample 2 px outside the edge (the HUD frame often has a 1 px border).
    auto px = [&](int x, int y, float* o) {
        x = (std::max)(0, (std::min)(w - 1, x)); y = (std::max)(0, (std::min)(h - 1, y));
        const uint8_t* p = rgba + ((size_t)y * w + x) * 4u;
        o[0] = p[0]; o[1] = p[1]; o[2] = p[2];
    };
    std::vector<float> L((size_t)rh * 3), R((size_t)rh * 3), T((size_t)rw * 3), B((size_t)rw * 3);
    for (int y = 0; y < rh; ++y) { px(x0 - 2, y0 + y, &L[(size_t)y * 3]); px(x1 + 1, y0 + y, &R[(size_t)y * 3]); }
    for (int x = 0; x < rw; ++x) { px(x0 + x, y0 - 2, &T[(size_t)x * 3]); px(x0 + x, y1 + 1, &B[(size_t)x * 3]); }
    const int rad = 3;
    BlurLine(L, rh, rad); BlurLine(R, rh, rad); BlurLine(T, rw, rad); BlurLine(B, rw, rad);
    for (int y = 0; y < rh; ++y) {
        uint8_t* p = rgba + ((size_t)(y0 + y) * w + x0) * 4u;
        const float dt = (float)(y + 1), db = (float)(rh - y);
        const float wt = haveT ? 1.0f / (dt * dt) : 0.0f, wb = haveB ? 1.0f / (db * db) : 0.0f;
        for (int x = 0; x < rw; ++x, p += 4) {
            const float dl = (float)(x + 1), dr = (float)(rw - x);
            const float wl = haveL ? 1.0f / (dl * dl) : 0.0f, wr = haveR ? 1.0f / (dr * dr) : 0.0f;
            const float sum = wl + wr + wt + wb;
            for (int c = 0; c < 3; ++c) {
                const float v = (wl * L[(size_t)y * 3 + c] + wr * R[(size_t)y * 3 + c] +
                                 wt * T[(size_t)x * 3 + c] + wb * B[(size_t)x * 3 + c]) / sum;
                p[c] = (uint8_t)(std::max)(0.0f, (std::min)(255.0f, v + 0.5f));
            }
            p[3] = 255;
        }
    }
}

// ---- OTHER LIFE BARS: bosses, Meryl, anyone else (2026-09-25) --------------
// Every life bar the game draws -- Snake's LIFE and O2, a boss's, Meryl's when
// she is with you -- goes through one function: menu_draw_bar(prim, x, y,
// rest, now, max, conf) (decomp menu/life.c; PC mgsi.exe+68DA6, found from the
// "LIFE" bar config it is called with at +691A2). conf->name is the caption
// the game prints ("OCELOT", "SNIPER WOLF", "MERYL", ...), and conf holds the
// bar's own colours. Hooking it hands us every bar with its name, value and
// screen position, whoever draws it -- no per-boss addresses. Snake's own
// LIFE / O2 are skipped (the wrist already reads those from the linkvar block).
struct OtherBar {
    char     name[24];
    int      now = 0, max = 0, x = 0, y = 0;
    uint8_t  rgb[3] = { 200, 60, 60 };
    ULONGLONG tick = 0;
    uintptr_t conf = 0;
};
constexpr int kMaxOtherBars = 4;
std::mutex w_barMutex;
OtherBar   w_bars[kMaxOtherBars];
using MenuDrawBarFn = void(__cdecl*)(void*, long, long, long, long, long, void*);
MenuDrawBarFn w_origDrawBar = nullptr;
constexpr uintptr_t kMenuDrawBarRva = 0x068DA6;
constexpr uintptr_t kLifeConfRva = 0x2757F0, kO2ConfRva = 0x275800;
constexpr uintptr_t kGameStatusRva = 0x32279C;   // GM_GameStatus; menu_draw_bar returns early on & 0x80020400

// SEH on its own: no objects to unwind here.
bool ReadBarConf(uintptr_t conf, char name[24], uint8_t rgb[3]) {
    __try {
        const char* n = *reinterpret_cast<const char* const*>(conf);
        int i = 0;
        for (; i < 23 && n && n[i]; ++i) name[i] = n[i];
        name[i] = 0;
        const uint8_t* c = reinterpret_cast<const uint8_t*>(conf + 4);
        // right-hand (brighter) end of the game's own gradient
        rgb[0] = c[3]; rgb[1] = c[4]; rgb[2] = c[5];
        return i > 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void __cdecl HookedMenuDrawBar(void* prim, long x, long y, long rest, long now, long max, void* confArg) {
    static const uintptr_t base = (uintptr_t)GetModuleHandleA(nullptr);
    const uintptr_t conf = (uintptr_t)confArg & 0xBFFFFFFFu;   // the PC build tags bit 30, as the function itself masks
    uint8_t st[4] = {};
    int16_t dummy = 0; (void)dummy;
    uint32_t status = 0;
    bool haveStatus = ReadGameByteSafe(kGameStatusRva, &st[0]) && ReadGameByteSafe(kGameStatusRva + 1, &st[1]) &&
                      ReadGameByteSafe(kGameStatusRva + 2, &st[2]) && ReadGameByteSafe(kGameStatusRva + 3, &st[3]);
    if (haveStatus) status = (uint32_t)st[0] | ((uint32_t)st[1] << 8) | ((uint32_t)st[2] << 16) | ((uint32_t)st[3] << 24);
    if (conf && conf != base + kLifeConfRva && conf != base + kO2ConfRva && max > 0 && !(status & 0x80020400u)) {
        OtherBar b;
        if (ReadBarConf(conf, b.name, b.rgb) && std::strcmp(b.name, "LIFE") != 0 && std::strcmp(b.name, "O2") != 0) {
            b.now = (int)now; b.max = (int)max; b.x = (int)x; b.y = (int)y;
            b.tick = GetTickCount64(); b.conf = conf;
            std::lock_guard<std::mutex> lk(w_barMutex);
            int slot = -1, oldest = 0;
            for (int i = 0; i < kMaxOtherBars; ++i) {
                if (w_bars[i].conf == conf) { slot = i; break; }
                if (w_bars[i].tick < w_bars[oldest].tick) oldest = i;
            }
            if (slot < 0) {
                slot = oldest;
                DebugLogger::LogFormat("Wrist HUD: life bar \"%s\" %d/%d at (%ld,%ld) -- shown on the left wrist", b.name, b.now, b.max, x, y);
            }
            w_bars[slot] = b;
        }
    }
    w_origDrawBar(prim, x, y, rest, now, max, confArg);
}

// Bars drawn within the last half second, oldest first.
int ActiveOtherBars(OtherBar out[kMaxOtherBars]) {
    std::lock_guard<std::mutex> lk(w_barMutex);
    const ULONGLONG now = GetTickCount64();
    int n = 0;
    for (int i = 0; i < kMaxOtherBars; ++i)
        if (w_bars[i].tick && now - w_bars[i].tick < 500) out[n++] = w_bars[i];
    std::sort(out, out + n, [](const OtherBar& a, const OtherBar& b) { return a.y < b.y; });
    return n;
}

// Where the game draws LIFE (and O2 under it): decomp menu/life.c puts the
// bar at (16,16) of 320x240, width = max LIFE / 8, O2 row 12 px lower, with
// the "LIFE" caption just above. Padded so the frame and caption go too.
void LifeRect(const GameHud& g, float out[4]) {
    if (w_life[0] >= 0.0f) { for (int i = 0; i < 4; ++i) out[i] = w_life[i]; return; }
    int maxLife = g.vitMax > 0 ? g.vitMax : 1024;
    if (maxLife > 2048) maxLife = 2048;
    const float barW = (float)maxLife / 8.0f;
    const float x0 = 10.0f, y0 = 6.0f, x1 = 16.0f + barW + 8.0f, y1 = 16.0f + 12.0f + 14.0f;
    out[0] = x0 / 320.0f; out[1] = y0 / 240.0f;
    out[2] = (std::min)(1.0f - out[0], (x1 - x0) / 320.0f); out[3] = (y1 - y0) / 240.0f;
}

XrQuaternionf EulerDeg(const float pyr[3]) {
    const float d = kPi / 180.0f;
    XrQuaternionf q = QuatFromAxisAngle(0, 1, 0, pyr[1] * d);
    q = QuatMul(q, QuatFromAxisAngle(1, 0, 0, pyr[0] * d));
    q = QuatMul(q, QuatFromAxisAngle(0, 0, 1, pyr[2] * d));
    return q;
}

void DrawBar(int x, int y, int w, int h, float frac, uint8_t r, uint8_t g, uint8_t b) {
    frac = (std::max)(0.0f, (std::min)(1.0f, frac));
    HudFill(x - 1, y - 1, w + 2, h + 2, 200, 200, 200, 255);
    HudFill(x, y, w, h, 20, 20, 20, 230);
    HudFill(x, y, (int)(w * frac), h, r, g, b, 255);
}

// ---- wrist quick-select state (XR thread) -----------------------------------
struct WristList {
    bool open = false;
    bool navigated = false;
    bool used = false;       // an item was used from this list -> releasing changes nothing
    ULONGLONG openedAt = 0;
    ULONGLONG lastStep = 0;
    int  stepDir = 0;
    int  ids[26];            // -1 = NONE (unequip), else weapon / item id
    int  count = 0;
    int  sel = 0;
    int  lastEquipped = -1;  // for the quick tap toggle
};
WristList w_list[2];         // 0 = items (left grip), 1 = weapons (right grip)

bool WriteGameShortSafe(uintptr_t rva, int16_t v) {
    static uintptr_t base = 0;
    if (!base) base = (uintptr_t)GetModuleHandleA(nullptr);
    __try { *reinterpret_cast<volatile int16_t*>(base + rva) = v; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Owned = linkvar count/ammo >= 0 (-1 means not in the inventory).
void BuildWristList(int side, int equipped) {
    WristList& L = w_list[side];
    L.count = 0;
    L.ids[L.count++] = -1;
    const int n = side == 1 ? 10 : 24;
    const int baseIdx = side == 1 ? 17 : 37;
    for (int i = 0; i < n && L.count < 26; ++i) {
        int16_t v = -1;
        if (ReadGameShortSafe(kLinkvar + (baseIdx + i) * 2, &v) && v >= 0) L.ids[L.count++] = i;
    }
    L.sel = 0;
    for (int k = 0; k < L.count; ++k) if (L.ids[k] == equipped) L.sel = k;
}

void EquipFromWrist(int side, int id) {
    WriteGameShortSafe(kLinkvar + (side == 1 ? 14 : 15) * 2, (int16_t)id);
    DebugLogger::LogFormat("Wrist select: %s -> %s", side == 1 ? "weapon" : "item",
        id < 0 ? "NONE" : (side == 1 ? kWeaponNames[id] : kItemNames[id]));
}

void DrawWristList(int X, int side, const GameHud& h) {
    const WristList& L = w_list[side];
    const int rowH = 20, maxRows = 11;
    int first = L.sel - maxRows / 2;
    if (first > L.count - maxRows) first = L.count - maxRows;
    if (first < 0) first = 0;
    const int rows = (std::min)(maxRows, L.count - first);
    const int H = 26 + rows * rowH + 4;
    HudFill(X, 0, kPanel, H, 0, 0, 0, 170);
    HudText(X + 8, 6, side == 1 ? "WEAPON" : "ITEM", 2, 120, 200, 255);
    for (int r = 0; r < rows; ++r) {
        const int k = first + r;
        const int id = L.ids[k];
        const int y = 26 + r * rowH;
        const bool hi = (k == L.sel);
        if (hi) HudFill(X + 2, y - 2, kPanel - 4, rowH, 40, 110, 70, 230);
        char line[40];
        int16_t v = 0;
        if (id < 0) sprintf_s(line, "NONE");
        else if (side == 1) {
            ReadGameShortSafe(kLinkvar + (17 + id) * 2, &v);
            sprintf_s(line, "%s %d", kWeaponNames[id], (int)v);
        }
        else {
            ReadGameShortSafe(kLinkvar + (37 + id) * 2, &v);
            if (id == 17) sprintf_s(line, "CARD LV%d", (int)v);
            else if (id == 13 || id == 14 || id == 15) sprintf_s(line, "%s %d", kItemNames[id], (int)v);
            else sprintf_s(line, "%s", kItemNames[id]);
        }
        const bool equipped = (side == 1 ? h.weapon : h.item) == id;
        HudText(X + 10, y, line, 2, hi ? 255 : 210, hi ? 255 : 210, equipped ? 120 : (hi ? 255 : 210));
    }
    w_panelH[side] = (std::min)(kPanel, H);
}

// The button remap panel, on the left half. Row = physical input, value = the
// game action it sends. Caller holds w_remapMutex.
void DrawRemapPanel(int X) {
    const RemapPanelView& R = w_remap;
    const int rowH = 18;
    const int H = 24 + R.count * rowH + 30;
    HudFill(X, 0, kPanel, (std::min)(kPanel, H), 0, 0, 0, 205);
    HudText(X + 6, 5, R.changed ? "REMAP *" : "REMAP", 2, 255, 210, 90);
    for (int r = 0; r < R.count; ++r) {
        const int y = 24 + r * rowH;
        const bool hi = (r == R.sel);
        if (hi) HudFill(X + 2, y - 2, kPanel - 4, rowH, 40, 110, 70, 235);
        HudText(X + 6, y, R.input[r], 2, 170, 210, 255);
        HudText(X + 90, y, R.action[r], 2, 255, 255, hi ? 150 : 230);
    }
    const int yh = 24 + R.count * rowH + 4;
    HudText(X + 6, yh, "STICK UP/DOWN: PICK   LEFT/RIGHT: CHANGE", 1, 200, 200, 200);
    HudText(X + 6, yh + 11, "OR PRESS A BUTTON TO PICK IT", 1, 200, 200, 200);
    HudText(X + 6, yh + 22, "L3 CLICK: SAVE AND CLOSE", 1, 255, 210, 90);
    w_panelH[0] = (std::min)(kPanel, H + 4);
}

// Redraw both halves into w_pixels. Returns a cheap signature so an unchanged
// picture is not re-uploaded.
uint64_t RenderPanels(const GameHud& h) {
    std::fill(w_pixels.begin(), w_pixels.end(), (uint8_t)0);
    uint64_t sig = 1469598103934665603ull;
    auto mix = [&](uint64_t v) { sig ^= v; sig *= 1099511628211ull; };

    if (w_subHave && SubtitlesShowing()) {
        HudFill(0, kSubY, kWristW, kSubH, 0, 0, 0, 220);
        {
            // centred, aspect kept
            const float sc = (w_subCrop.w > 1 && w_subCrop.h > 1)
                ? (std::min)((float)kWristW / (float)w_subCrop.w, (float)kSubH / (float)w_subCrop.h) : 1.0f;
            const int dw = (int)(w_subCrop.w * sc), dh = (int)(w_subCrop.h * sc);
            BlitCrop(w_subCrop, (std::max)(0, (kWristW - dw) / 2), kSubY + (std::max)(0, (kSubH - dh) / 2), kWristW, kSubH);
        }
        mix(0x5B7); mix(w_cropSeq);
    }

    // ---- LEFT: LIFE, O2, item --------------------------------------------
    {
        const int X = 0;
        const bool sel = !w_list[0].open && w_gripHeld[0] && w_haveCrops && !w_selectOnWrist;
        bool remapOpen = false;
        {
            std::lock_guard<std::mutex> lk(w_remapMutex);
            remapOpen = w_remap.open;
            if (remapOpen) {
                DrawRemapPanel(X);
                mix(0xC0 + (uint64_t)w_remap.sel); mix((uint64_t)w_remap.count); mix(w_remap.changed ? 7 : 8);
                for (int r = 0; r < w_remap.count; ++r)
                    for (const char* c = w_remap.action[r]; *c; ++c) mix((uint64_t)(unsigned char)*c);
            }
        }
        if (remapOpen) {
            // drawn above
        }
        else if (w_list[0].open) {
            DrawWristList(X, 0, h);
            mix(0xA0 + (uint64_t)w_list[0].sel); mix((uint64_t)w_list[0].count); mix((uint64_t)(h.item + 1));
        }
        else if (sel) {
            HudFill(X, 0, kPanel, kPanel, 0, 0, 0, 150);
            const int used = BlitCrop(w_crop[2], X + 4, 4, kPanel - 8, kPanel - 8);
            w_panelH[0] = (std::min)(kPanel, used + 8);
            mix(w_cropSeq); mix(2);
        }
        else {
            HudFill(X, 0, kPanel, kPanel, 0, 0, 0, 150);
            HudText(X + 8, 8, "LIFE", 2, 255, 255, 255);
            const float lf = h.vitMax > 0 ? (float)h.vit / (float)h.vitMax : 0.0f;
            const bool low = lf < 0.25f;
            // The game's proportions: max/8 game pixels, so the bar grows as
            // Snake's max LIFE does. O2 shares LIFE's length, as in the game.
            const int lifeFullW = kPanel - 76;
            int lifeW = lifeFullW;
            if (w_barFollowsMax && h.vitMax > 0)
                lifeW = (std::max)(12, (std::min)(lifeFullW, (int)((float)h.vitMax / 8.0f * w_barScale + 0.5f)));
            DrawBar(X + 64, 8, lifeW, 14, lf, low ? 230 : 40, low ? 50 : 170, low ? 40 : 230);
            int y = 30;
            if (h.o2 < 1024) {
                HudText(X + 8, y, "O2", 2, 255, 255, 255);
                DrawBar(X + 64, y, lifeW, 14, (float)h.o2 / 1024.0f, 60, 200, 255);
                y += 22;
            }
            // Bosses, Meryl, anyone else whose LIFE the game is showing:
            // their own caption and colours, full panel width.
            if (w_showOtherBars) {
                OtherBar ob[kMaxOtherBars];
                const int nb = ActiveOtherBars(ob);
                for (int i = 0; i < nb && y < kPanel - 60; ++i) {
                    y += 2;
                    HudText(X + 8, y, ob[i].name, 2, ob[i].rgb[0], ob[i].rgb[1], ob[i].rgb[2]);
                    y += 18;
                    const float f = ob[i].max > 0 ? (float)ob[i].now / (float)ob[i].max : 0.0f;
                    int bw = kPanel - 16;
                    if (w_barFollowsMax)
                        bw = (std::max)(12, (std::min)(kPanel - 16, (int)((float)ob[i].max / 8.0f * w_barScale + 0.5f)));
                    DrawBar(X + 8, y, bw, 12, f, ob[i].rgb[0], ob[i].rgb[1], ob[i].rgb[2]);
                    y += 18;
                    mix((uint64_t)ob[i].now); mix((uint64_t)ob[i].max); mix((uint64_t)ob[i].conf);
                }
                mix((uint64_t)nb);
            }
            y += 4;
            if (w_showRadar && w_haveCrops) {
                // Centred under LIFE; fitted to 108 px high (less when boss
                // bars need the room).
                const Crop& rc = w_crop[4];
                const int radarH = (std::max)(48, (std::min)(108, kPanel - y - 80));
                const float s = (std::min)((float)(kPanel - 16) / (float)(std::max)(1, rc.w), (float)radarH / (float)(std::max)(1, rc.h));
                const int dw = (int)(rc.w * s);
                y += BlitCrop(rc, X + (kPanel - dw) / 2, y, kPanel - 16, radarH) + 6;
                mix(w_cropSeq);
            }
            if (w_cropBoxes && w_haveCrops && h.item >= 0) {
                y += BlitCrop(w_crop[0], X + 8, y, kPanel - 16, (std::min)(72, kPanel - y - 4));
                mix(w_cropSeq);
            }
            else if (h.item >= 0 && h.item < 24) {
                char line[32];
                if (h.itemCount > 0 && (h.item == 13 || h.item == 14 || h.item == 15)) sprintf_s(line, "%s %d", kItemNames[h.item], h.itemCount);
                else if (h.item == 17) sprintf_s(line, "CARD LV%d", h.itemCount);
                else sprintf_s(line, "%s", kItemNames[h.item]);
                HudText(X + 8, y, line, 2, 230, 230, 230);
                y += 18;
            }
            w_panelH[0] = (std::max)(40, (std::min)(kPanel, y + 6));
            mix((uint64_t)h.vit); mix((uint64_t)h.vitMax); mix((uint64_t)h.o2); mix((uint64_t)(h.item + 1)); mix((uint64_t)h.itemCount);
        }
        mix(sel ? 11 : 12);
    }

    // ---- RIGHT: weapon -----------------------------------------------------
    {
        const int X = kPanel;
        const bool sel = !w_list[1].open && w_gripHeld[1] && w_haveCrops && !w_selectOnWrist;
        if (w_list[1].open) {
            DrawWristList(X, 1, h);
            mix(0xB0 + (uint64_t)w_list[1].sel); mix((uint64_t)w_list[1].count); mix((uint64_t)(h.weapon + 1));
        }
        else if (sel) {
            HudFill(X, 0, kPanel, kPanel, 0, 0, 0, 150);
            const int used = BlitCrop(w_crop[3], X + 4, 4, kPanel - 8, kPanel - 8);
            w_panelH[1] = (std::min)(kPanel, used + 8);
            mix(w_cropSeq); mix(3);
        }
        else if (h.weapon >= 0) {
            HudFill(X, 0, kPanel, 112, 0, 0, 0, 150);
            int y = 6;
            if (w_cropBoxes && w_haveCrops) {
                y += BlitCrop(w_crop[1], X + 8, y, kPanel - 16, 100);
                mix(w_cropSeq);
            }
            else if (h.weapon < 10) {
                char line[32];
                sprintf_s(line, "%s %d", kWeaponNames[h.weapon], h.ammo);
                HudText(X + 8, y + 4, line, 2, 230, 230, 230);
                y += 24;
            }
            w_panelH[1] = (std::max)(24, (std::min)(112, y + 6));
            mix((uint64_t)(h.weapon + 1)); mix((uint64_t)h.ammo);
        }
        else {
            w_panelH[1] = 0;
        }
        mix(sel ? 21 : 22);
    }
    return sig;
}

bool GazeShows(int side, const XrPosef& head, const XrPosef& quadPose) {
    if (w_gripHeld[side]) return true;                     // selection open: always show it
    if (side == 0) { std::lock_guard<std::mutex> lk(w_remapMutex); if (w_remap.open) return true; }
    if (w_gazeConeDeg <= 0.0f) return true;
    const float d[3] = { quadPose.position.x - head.position.x, quadPose.position.y - head.position.y,
                         quadPose.position.z - head.position.z };
    const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (len < 1e-3f) return true;
    float fwd[3];
    const float fz[3] = { 0, 0, -1 };
    QuatRotate(head.orientation, fz, fwd);
    const float c = (fwd[0] * d[0] + fwd[1] * d[1] + fwd[2] * d[2]) / len;
    // Hysteresis: once shown, keep it until 8 degrees outside the cone.
    static bool shown[2] = { false, false };
    const float inCos = std::cos(w_gazeConeDeg * kPi / 180.0f);
    const float outCos = std::cos((w_gazeConeDeg + 8.0f) * kPi / 180.0f);
    shown[side] = shown[side] ? (c > outCos) : (c > inCos);
    // And it has to face you at least a little -- the back of a quad is invisible anyway.
    float n[3];
    const float pz[3] = { 0, 0, 1 };
    QuatRotate(quadPose.orientation, pz, n);
    const float facing = -(n[0] * d[0] + n[1] * d[1] + n[2] * d[2]) / len;
    return shown[side] && facing > 0.15f;
}

} // namespace

void PublishControllerGrips(const XrPosef& left, bool leftValid, const XrPosef& right, bool rightValid,
                            bool leftGripHeld, bool rightGripHeld) {
    w_grip[0] = left;  w_gripValid[0] = leftValid;
    w_grip[1] = right; w_gripValid[1] = rightValid;
    w_gripHeld[0] = leftGripHeld; w_gripHeld[1] = rightGripHeld;
    MotionAimPublishLeftGrip(left, leftValid);   // Snake's left hand ([hands])
}

void LoadWristHudConfig() {
    const std::string ini = GetGameIniPath();
    const char* S = "wrist_hud";
    w_enabled = GetPrivateProfileIntA(S, "enabled", 1, ini.c_str()) != 0;
    w_widthM = (float)GetPrivateProfileIntA(S, "width_mm", 90, ini.c_str()) / 1000.0f;
    if (w_widthM < 0.03f) w_widthM = 0.03f;
    if (w_widthM > 0.40f) w_widthM = 0.40f;
    float cm[3];
    cm[0] = w_offL[0] * 100; cm[1] = w_offL[1] * 100; cm[2] = w_offL[2] * 100;
    ReadFloats(ini, "left_offset_cm", cm, 3);  for (int i = 0; i < 3; ++i) w_offL[i] = cm[i] / 100.0f;
    cm[0] = w_offR[0] * 100; cm[1] = w_offR[1] * 100; cm[2] = w_offR[2] * 100;
    ReadFloats(ini, "right_offset_cm", cm, 3); for (int i = 0; i < 3; ++i) w_offR[i] = cm[i] / 100.0f;
    ReadFloats(ini, "left_rotation_deg", w_rotL, 3);
    ReadFloats(ini, "right_rotation_deg", w_rotR, 3);
    w_mount = GetPrivateProfileIntA(S, "mount", 1, ini.c_str());
    w_mountSide[0] = GetPrivateProfileIntA(S, "left_mount", 0, ini.c_str());
    w_listMount = GetPrivateProfileIntA(S, "list_mount", 2, ini.c_str());
    {
        const char* T = "subtitles";
        ReadFloatsSection(ini, T, "strip", w_subRect, 4);
        w_subWidthM = (float)GetPrivateProfileIntA(T, "panel_width_cm", 80, ini.c_str()) / 100.0f;
        w_subDistM = (float)GetPrivateProfileIntA(T, "panel_distance_cm", 110, ini.c_str()) / 100.0f;
        w_subDownDeg = (float)GetPrivateProfileIntA(T, "panel_down_deg", 20, ini.c_str());
        g_gtDrawOnScreen = GetPrivateProfileIntA(T, "draw_on_screen", 1, ini.c_str()) != 0;
    }
    w_mountSide[1] = GetPrivateProfileIntA(S, "right_mount", 0, ini.c_str());
    cm[0] = w_inOffL[0] * 100; cm[1] = w_inOffL[1] * 100; cm[2] = w_inOffL[2] * 100;
    ReadFloats(ini, "inside_left_offset_cm", cm, 3);  for (int i = 0; i < 3; ++i) w_inOffL[i] = cm[i] / 100.0f;
    cm[0] = w_inOffR[0] * 100; cm[1] = w_inOffR[1] * 100; cm[2] = w_inOffR[2] * 100;
    ReadFloats(ini, "inside_right_offset_cm", cm, 3); for (int i = 0; i < 3; ++i) w_inOffR[i] = cm[i] / 100.0f;
    ReadFloats(ini, "inside_left_rotation_deg", w_inRotL, 3);
    ReadFloats(ini, "inside_right_rotation_deg", w_inRotR, 3);
    w_barFollowsMax = GetPrivateProfileIntA(S, "life_bar_follows_max", 1, ini.c_str()) != 0;
    w_barScale = (float)GetPrivateProfileIntA(S, "life_bar_scale_percent", 125, ini.c_str()) / 100.0f;
    if (w_barScale < 0.25f) w_barScale = 0.25f;
    if (w_barScale > 4.0f) w_barScale = 4.0f;
    cm[0] = w_backOffL[0] * 100; cm[1] = w_backOffL[1] * 100; cm[2] = w_backOffL[2] * 100;
    ReadFloats(ini, "back_left_offset_cm", cm, 3);  for (int i = 0; i < 3; ++i) w_backOffL[i] = cm[i] / 100.0f;
    cm[0] = w_backOffR[0] * 100; cm[1] = w_backOffR[1] * 100; cm[2] = w_backOffR[2] * 100;
    ReadFloats(ini, "back_right_offset_cm", cm, 3); for (int i = 0; i < 3; ++i) w_backOffR[i] = cm[i] / 100.0f;
    ReadFloats(ini, "back_left_rotation_deg", w_backRotL, 3);
    ReadFloats(ini, "back_right_rotation_deg", w_backRotR, 3);
    DebugLogger::LogFormat("Wrist HUD mount: %s", w_mount == 1
        ? "BACK OF THE WRIST -- twist your forearm to read it, like checking a watch"
        : "over the back of the hand (old)");
    w_gazeConeDeg = (float)GetPrivateProfileIntA(S, "gaze_cone_deg", 28, ini.c_str());
    w_maskBoxesInView = GetPrivateProfileIntA(S, "hide_boxes_in_view", 1, ini.c_str()) != 0;
    w_maskSelectionInView = GetPrivateProfileIntA(S, "hide_selection_in_view", 0, ini.c_str()) != 0;
    w_hideLifeInView = GetPrivateProfileIntA(S, "hide_life_in_view", 1, ini.c_str()) != 0;
    w_showOtherBars = GetPrivateProfileIntA(S, "show_boss_life", 1, ini.c_str()) != 0;
    w_hideOtherBarsInView = GetPrivateProfileIntA(S, "hide_boss_life_in_view", 1, ini.c_str()) != 0;
    w_hideStyle = GetPrivateProfileIntA(S, "hide_style", 1, ini.c_str()) != 0 ? 1 : 0;
    ReadFloats(ini, "life", w_life, 4);
    w_cropBoxes = GetPrivateProfileIntA(S, "show_game_boxes", 1, ini.c_str()) != 0;
    ReadFloats(ini, "item_box", w_itemBox, 4);
    ReadFloats(ini, "weapon_box", w_weaponBox, 4);
    ReadFloats(ini, "item_selection", w_itemSel, 4);
    ReadFloats(ini, "weapon_selection", w_weaponSel, 4);
    ReadFloats(ini, "radar", w_radar, 4);
    w_showRadar = GetPrivateProfileIntA(S, "show_radar", 1, ini.c_str()) != 0;
    w_hideRadarInView = GetPrivateProfileIntA(S, "hide_radar_in_view", 1, ini.c_str()) != 0;
    w_selectOnWrist = GetPrivateProfileIntA(S, "select_on_wrist", 1, ini.c_str()) != 0;
    DebugLogger::LogFormat("Wrist HUD: hide LIFE in view=%d | hide style=%s | selection strips item (%.3f,%.3f,%.3f,%.3f) weapon (%.3f,%.3f,%.3f,%.3f) | wrist menu=%s",
        w_hideLifeInView ? 1 : 0, w_hideStyle ? "fill from surroundings" : "black",
        w_itemSel[0], w_itemSel[1], w_itemSel[2], w_itemSel[3], w_weaponSel[0], w_weaponSel[1], w_weaponSel[2], w_weaponSel[3],
        w_selectOnWrist ? "compact list" : "game's own scrolling menu");
    DebugLogger::LogFormat("Wrist HUD: radar on left wrist=%d (hidden in view=%d) | quick-select on wrist=%d "
        "(hold grip, stick up/down, release to equip; quick tap = toggle last)",
        w_showRadar ? 1 : 0, w_hideRadarInView ? 1 : 0, w_selectOnWrist ? 1 : 0);
    DebugLogger::LogFormat(
        "Wrist HUD: %s | width %.0f mm | gaze cone %.0f deg | hide boxes in view=%d, selection=%d | game boxes on wrist=%d | "
        "item box (%.3f,%.3f,%.3f,%.3f) weapon box (%.3f,%.3f,%.3f,%.3f)",
        w_enabled ? "ON (LIFE left wrist, weapon right wrist)" : "OFF", w_widthM * 1000.0f, w_gazeConeDeg,
        w_maskBoxesInView ? 1 : 0, w_maskSelectionInView ? 1 : 0, w_cropBoxes ? 1 : 0,
        w_itemBox[0], w_itemBox[1], w_itemBox[2], w_itemBox[3],
        w_weaponBox[0], w_weaponBox[1], w_weaponBox[2], w_weaponBox[3]);
}

bool InitWristHud(XrSession session, XrSpace playSpace, ID3D11Device* device, ID3D11DeviceContext* context) {
    if (!w_enabled) return false;
    if (w_ready) return true;
    if (session == XR_NULL_HANDLE || playSpace == XR_NULL_HANDLE || !device || !context) return false;
    w_session = session; w_space = playSpace; w_context = context;
    XrSwapchainCreateInfo info{ XR_TYPE_SWAPCHAIN_CREATE_INFO };
    info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    // sRGB: the wrist panels carry cropped game pixels, which are already sRGB.
    // A UNORM swapchain is read as linear and lifts the darks (the same fault
    // as the 2026-09-27 red shading in the main view). UNORM only as fallback.
    info.format = (int64_t)DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    info.sampleCount = 1; info.width = kWristW; info.height = kWristH;
    info.faceCount = 1; info.arraySize = 1; info.mipCount = 1;
    XrResult wxr = xrCreateSwapchain(session, &info, &w_swapchain);
    if (wxr != XR_SUCCESS) {
        info.format = (int64_t)DXGI_FORMAT_R8G8B8A8_UNORM;
        wxr = xrCreateSwapchain(session, &info, &w_swapchain);
    }
    if (wxr != XR_SUCCESS) {
        DebugLogger::Log("Wrist HUD: xrCreateSwapchain failed -- wrist HUD off for this run");
        w_swapchain = XR_NULL_HANDLE; w_enabled = false; return false;
    }
    uint32_t count = 0;
    xrEnumerateSwapchainImages(w_swapchain, 0, &count, nullptr);
    w_images.assign(count, { XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR });
    if (count == 0 || xrEnumerateSwapchainImages(w_swapchain, count, &count,
            reinterpret_cast<XrSwapchainImageBaseHeader*>(w_images.data())) != XR_SUCCESS) {
        DebugLogger::Log("Wrist HUD: could not enumerate swapchain images -- off");
        ShutdownWristHud(); w_enabled = false; return false;
    }
    w_pixels.assign((size_t)kWristW * kWristH * 4u, 0);
    w_ready = true;
    w_haveReleased = false;
    DebugLogger::LogFormat("Wrist HUD: ready (%u images, %dx%d, two small quads on the grip poses)", count, kWristW, kWristH);
    return true;
}

void ShutdownWristHud() {
    if (w_swapchain != XR_NULL_HANDLE && w_session != XR_NULL_HANDLE) xrDestroySwapchain(w_swapchain);
    w_swapchain = XR_NULL_HANDLE;
    w_images.clear();
    w_ready = false; w_haveReleased = false;
    w_session = XR_NULL_HANDLE; w_space = XR_NULL_HANDLE; w_context = nullptr;
}

// PSG1 scope picture: black outside a round lens, and a fine crosshair. Drawn
// into the captured frame, so it is exactly where the zoomed picture is.
static void DrawScopeOverlay(uint8_t* rgba, int w, int h) {
    const float cx = w * 0.5f, cy = h * 0.5f;
    const float r = h * 0.5f, r2 = r * r;
    for (int y = 0; y < h; ++y) {
        const float dy = (float)y + 0.5f - cy;
        uint8_t* row = rgba + (size_t)y * w * 4u;
        for (int x = 0; x < w; ++x) {
            const float dx = (float)x + 0.5f - cx;
            const float d2 = dx * dx + dy * dy;
            if (d2 > r2) { row[x * 4 + 0] = row[x * 4 + 1] = row[x * 4 + 2] = 0; }
            else if (d2 > r2 * 0.90f) {   // soft dark rim
                const float k = (r2 - d2) / (r2 * 0.10f);
                for (int c = 0; c < 3; ++c) row[x * 4 + c] = (uint8_t)(row[x * 4 + c] * k);
            }
        }
    }
    const int t = (std::max)(1, h / 240);          // line thickness
    const int gap = (int)(r * 0.06f), len = (int)(r * 0.92f);
    auto px = [&](int x, int y) {
        if (x < 0 || y < 0 || x >= w || y >= h) return;
        uint8_t* p = rgba + ((size_t)y * w + x) * 4u; p[0] = p[1] = p[2] = 0;
    };
    const int icx = (int)cx, icy = (int)cy;
    for (int i = gap; i < len; ++i) for (int k = -t; k <= t; ++k) {
        const int thick = (i > len / 2) ? 2 : 1;   // heavier posts toward the rim
        for (int q = 0; q < thick; ++q) {
            px(icx + i, icy + k * thick + q); px(icx - i, icy + k * thick + q);
            px(icx + k * thick + q, icy + i); px(icx + k * thick + q, icy - i);
        }
    }
    for (int y = -t; y <= t; ++y) for (int x = -t; x <= t; ++x) {   // red centre dot
        if (icx + x < 0 || icy + y < 0 || icx + x >= w || icy + y >= h) continue;
        uint8_t* p = rgba + ((size_t)(icy + y) * w + icx + x) * 4u; p[0] = 230; p[1] = 30; p[2] = 30;
    }
}

void WristHudOnNewFrame(uint8_t* rgba, int w, int h) {
    if (rgba && w >= 64 && h >= 64 && IsScopeViewActive()) DrawScopeOverlay(rgba, w, h);
    // The codec is shown on the virtual screen (vr_injection.cpp), never as the
    // VR view, even though the game's first-person flag can stay up under it.
    const bool vrView = IsVrViewModeActive() && !IsCodecOpen() &&
                        (IsFpvActive() || IsGameplayPovActive() || IsCutscenePovDrawing());
    // The PC port's hi-res text (captions, codec lines) is GDI on the back
    // buffer and never in the captured picture. Wherever the caption panel is
    // not taking it -- the native virtual screen, the codec and cutscenes on
    // the 2D frame, the panel switched off -- typeset it into the frame at the
    // place the game drew it on the monitor (2026-10-03).
    const bool panelTakesText = w_enabled && w_ready && IsSubtitlePanelEnabled() && vrView;
    if (rgba && w >= 64 && h >= 64 && g_gtDrawOnScreen && g_origGameText && !panelTakesText)
        DrawGameTextIntoFrame(rgba, w, h);
    if (rgba && w >= 64 && h >= 64 && w_enabled && IsSubtitlePanelEnabled() && vrView) {
        float gr[4];
        const bool game = GameCaptionRect(gr);
        static int s_capLog = 0;
        static bool s_lastGame = false;
        static int s_capFrames = 0, s_capText = 0, s_capMaxBright = 0;
        if (game != s_lastGame && s_capLog < 40) {
            s_capLog++;
            if (!game && s_capFrames)
                DebugLogger::LogFormat("Captions: last caption -- %d frames seen, %d with text found in the frame "
                    "(max %d bright pixels, frame %dx%d)", s_capFrames, s_capText, s_capMaxBright, w, h);
            s_capFrames = s_capText = s_capMaxBright = 0;
            int16_t x = 0, y = 0, hh = 0;
            ReadGameShortSafe(0x333824, &x); ReadGameShortSafe(0x333826, &y); ReadGameShortSafe(0x33382A, &hh);
            DebugLogger::LogFormat("Captions: game caption %s (x %d y %d h %d, fpv=%d snake's eyes=%d cutscene=%d)",
                game ? "UP -- lifted onto the panel below your gaze" : "gone", x, y, hh,
                IsFpvActive() ? 1 : 0, IsGameplayPovActive() ? 1 : 0, IsCutscenePovDrawing() ? 1 : 0);
        }
        s_lastGame = game;
        const float* rect = game ? gr : w_subRect;
        if (game || IsCutscenePovDrawing()) {
            std::lock_guard<std::mutex> lk(w_cropMutex);
            static Crop s_frameCap;
            CopyCrop(rgba, w, h, rect, s_frameCap);
            size_t bright = 0;
            const size_t n = (size_t)s_frameCap.w * s_frameCap.h;
            for (size_t i = 0; i < n; ++i) {
                const uint8_t* p = &s_frameCap.rgba[i * 4u];
                if (p[0] > 140 || p[1] > 140 || p[2] > 140) ++bright;
            }
            // The game's flag is the authority; the old bright-pixel test is
            // only the fallback for a demo strip with no flag. Painted out of
            // the view ONLY when it is going onto the panel -- never erased
            // without being shown.
            const bool text = game ? (bright > 0) : (n && bright * 1000u >= n * 2u && bright * 4u < n);
            if (game) {
                s_capFrames++;
                if (text) s_capText++;
                if ((int)bright > s_capMaxBright) s_capMaxBright = (int)bright;
            }
            const bool fromBitmap = GetTickCount64() - w_subFromBitmap < 500;
            if (text && !fromBitmap) {
                w_subCrop = s_frameCap;
                w_subLastText = GetTickCount64();
                w_subHave = true;
                w_cropSeq++;
            }
            if (text || (game && fromBitmap)) MaskRect(rgba, w, h, rect);
        }
    }
    if (!w_enabled || !rgba || w < 64 || h < 64) return;
    const GameHud g = ReadGameHud();
    static bool loggedRead = false;
    if (!loggedRead && g.ok) {
        loggedRead = true;
        int16_t alt = 0;
        ReadGameShortSafe(0x323876, &alt);
        DebugLogger::LogFormat("Wrist HUD: LIFE %d/%d (community table's LIFE copy reads %d), weapon %d ammo %d/%d, item %d x%d, O2 %d, modal %d",
            g.vit, g.vitMax, (int)alt, g.weapon, g.ammo, g.ammoMax, g.item, g.itemCount, g.o2, g.modal);
    }
    if (!w_ready || !HudContextActive(g)) return;   // no panels -> leave the view untouched
    // Crops first (the wrists want the real pixels), then hide in the view.
    {
        std::lock_guard<std::mutex> lk(w_cropMutex);
        CopyCrop(rgba, w, h, w_itemBox, w_crop[0]);
        CopyCrop(rgba, w, h, w_weaponBox, w_crop[1]);
        CopyCrop(rgba, w, h, w_itemSel, w_crop[2]);
        CopyCrop(rgba, w, h, w_weaponSel, w_crop[3]);
        CopyCrop(rgba, w, h, w_radar, w_crop[4]);
        w_haveCrops = true;
        w_cropSeq++;
    }
    if (w_maskBoxesInView) {
        if (!w_gripHeld[0] || w_selectOnWrist) MaskRect(rgba, w, h, w_itemBox);
        if (!w_gripHeld[1] || w_selectOnWrist) MaskRect(rgba, w, h, w_weaponBox);
    }
    if (w_showRadar && w_hideRadarInView) MaskRect(rgba, w, h, w_radar);
    if (w_hideLifeInView) { float lr[4]; LifeRect(g, lr); MaskRect(rgba, w, h, lr); }
    if (w_showOtherBars && w_hideOtherBarsInView) {
        // Same padding as Snake's LIFE: caption above, frame around. Game
        // coordinates are the PSX 320x240 screen.
        OtherBar ob[kMaxOtherBars];
        const int nb = ActiveOtherBars(ob);
        for (int i = 0; i < nb; ++i) {
            const float x0 = (float)ob[i].x - 6.0f, y0 = (float)ob[i].y - 10.0f;
            const float x1 = (float)ob[i].x + (float)ob[i].max / 8.0f + 8.0f, y1 = (float)ob[i].y + 14.0f;
            float r[4] = { x0 / 320.0f, y0 / 240.0f, (x1 - x0) / 320.0f, (y1 - y0) / 240.0f };
            if (r[0] < 0.0f) { r[2] += r[0]; r[0] = 0.0f; }
            if (r[1] < 0.0f) { r[3] += r[1]; r[1] = 0.0f; }
            if (r[0] + r[2] > 1.0f) r[2] = 1.0f - r[0];
            if (r[1] + r[3] > 1.0f) r[3] = 1.0f - r[1];
            if (r[2] > 0.01f && r[3] > 0.01f) MaskRect(rgba, w, h, r);
        }
    }
    if (w_maskSelectionInView && !w_selectOnWrist) {
        if (w_gripHeld[0]) MaskRect(rgba, w, h, w_itemSel);
        if (w_gripHeld[1]) MaskRect(rgba, w, h, w_weaponSel);
    }
}

void BuildWristHudLayers(XrTime displayTime, const XrPosef& headPose,
                         std::vector<const XrCompositionLayerBaseHeader*>& outLayers) {
    (void)displayTime;
    if (!w_enabled || !w_ready) return;
    const GameHud g = ReadGameHud();
    bool remapOpen = false;
    { std::lock_guard<std::mutex> lk(w_remapMutex); remapOpen = w_remap.open; }
    const bool hudCtx = HudContextActive(g);
    // Captions straight from the game's text bitmap, polled here so the panel
    // never depends on what the captured frames contain.
    if (IsSubtitlePanelEnabled() && IsVrViewModeActive() && !IsCodecOpen() &&
        (IsFpvActive() || IsGameplayPovActive() || IsCutscenePovDrawing())) {
        static Crop s_cap;
        static uint32_t s_lastSig = 0;
        uint32_t sig = 0;
        if (GameCaptionBitmap(s_cap, &sig)) {
            std::lock_guard<std::mutex> lk(w_cropMutex);
            if (sig != s_lastSig || !w_subHave) {
                w_subCrop = s_cap;
                w_cropSeq++;
                s_lastSig = sig;
                static int s_bmLog = 0;
                if (s_bmLog < 12) {
                    s_bmLog++;
                    DebugLogger::LogFormat("Captions: caption text read from the game's own bitmap (%dx%d) -> panel", s_cap.w, s_cap.h);
                }
            }
            w_subHave = true;
            w_subLastText = GetTickCount64();
            w_subFromBitmap = GetTickCount64();
        }
    }
    const bool subs = SubtitlesShowing();
    if (!hudCtx && !remapOpen && !subs) return;

    // Re-render at most ~30 Hz, and upload only if the picture changed.
    static ULONGLONG lastRender = 0;
    static uint64_t lastSig = 0;
    const ULONGLONG now = GetTickCount64();
    if (now - lastRender >= 33 || !w_haveReleased) {
        lastRender = now;
        uint64_t sig;
        {
            std::lock_guard<std::mutex> lk(w_cropMutex);
            sig = RenderPanels(g);
        }
        if (sig != lastSig || !w_haveReleased) {
            uint32_t idx = 0;
            XrSwapchainImageAcquireInfo acq{ XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
            if (xrAcquireSwapchainImage(w_swapchain, &acq, &idx) == XR_SUCCESS) {
                XrSwapchainImageWaitInfo wait{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
                wait.timeout = 50000000;   // 50 ms; never hang the frame thread on a HUD
                bool ok = false;
                if (xrWaitSwapchainImage(w_swapchain, &wait) == XR_SUCCESS && idx < w_images.size() && w_images[idx].texture) {
                    w_context->UpdateSubresource(w_images[idx].texture, 0, nullptr, w_pixels.data(), (UINT)(kWristW * 4), 0);
                    ok = true;
                }
                XrSwapchainImageReleaseInfo rel{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
                if (xrReleaseSwapchainImage(w_swapchain, &rel) == XR_SUCCESS && ok) { w_haveReleased = true; lastSig = sig; }
            }
        }
    }
    if (!w_haveReleased) return;

    if (subs) {
        // Head-locked, yaw only: a strip of text just below where you look.
        const XrQuaternionf& o = headPose.orientation;
        const float fz[3] = { 0, 0, -1 };
        float f[3];
        QuatRotate(o, fz, f);
        const float yaw = std::atan2(-f[0], -f[2]);
        const float down = w_subDownDeg * kPi / 180.0f;
        XrPosef p;
        p.position.x = headPose.position.x - std::sin(yaw) * std::cos(down) * w_subDistM;
        p.position.y = headPose.position.y - std::sin(down) * w_subDistM;
        p.position.z = headPose.position.z - std::cos(yaw) * std::cos(down) * w_subDistM;
        p.orientation = QuatMul(QuatFromAxisAngle(0, 1, 0, yaw), QuatFromAxisAngle(1, 0, 0, -down));   // tilted up to face you
        XrCompositionLayerQuad& q = w_subQuad;
        q = XrCompositionLayerQuad{ XR_TYPE_COMPOSITION_LAYER_QUAD };
        q.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT | XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
        q.space = w_space;
        q.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        q.subImage.swapchain = w_swapchain;
        q.subImage.imageRect.offset = { 0, kSubY };
        q.subImage.imageRect.extent = { kWristW, kSubH };
        q.subImage.imageArrayIndex = 0;
        q.pose = p;
        q.size.width = w_subWidthM;
        q.size.height = w_subWidthM * (float)kSubH / (float)kWristW;
        outLayers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&q));
        static int subLog = 0;
        if (subLog < 2) { subLog++; DebugLogger::Log("Subtitles: caption strip shown on its own panel below your gaze"); }
    }
    if (!hudCtx && !remapOpen) return;

    static int logged = 0;
    for (int side = 0; side < 2; ++side) {
        if (!w_gripValid[side] || w_panelH[side] <= 0) continue;
        // Info (LIFE / radar / boss LIFE / item box on the left, weapon and
        // ammo on the right) sits on its mount; a list you are scrolling
        // moves to the list mount (inside of the wrist) while it is open.
        const bool listOpen = w_list[side].open || (w_gripHeld[side] && !w_selectOnWrist);
        const int mnt = listOpen ? w_listMount : w_mountSide[side];
        const bool back = (mnt == 1), inside = (mnt == 2);
        const float* off = inside ? (side == 0 ? w_inOffL : w_inOffR)
                         : back ? (side == 0 ? w_backOffL : w_backOffR) : (side == 0 ? w_offL : w_offR);
        const float* rot = inside ? (side == 0 ? w_inRotL : w_inRotR)
                         : back ? (side == 0 ? w_backRotL : w_backRotR) : (side == 0 ? w_rotL : w_rotR);
        XrPosef pose;
        float o[3];
        QuatRotate(w_grip[side].orientation, off, o);
        pose.position.x = w_grip[side].position.x + o[0];
        pose.position.y = w_grip[side].position.y + o[1];
        pose.position.z = w_grip[side].position.z + o[2];
        if (inside) {
            // Face out of the palm side of the wrist: -X (right hand) / +X
            // (left) in grip space; panel "up" runs toward the fingers, so it
            // reads upright with the palm turned up toward you.
            XrQuaternionf face;
            face.w = 0.5f;
            if (side == 1) { face.x = -0.5f; face.y = -0.5f; face.z = 0.5f; }
            else           { face.x = -0.5f; face.y = 0.5f;  face.z = -0.5f; }
            pose.orientation = QuatMul(QuatMul(w_grip[side].orientation, face), EulerDeg(rot));
        }
        else if (back) {
            // Watch face. OpenXR grip space: -Z along the fingers, +Y up the
            // thumb side, and the back of the hand faces -X (left) / +X (right).
            // The face looks out of the back of the wrist; reading it with the
            // forearm turned palm-down, "right" on the panel runs toward the
            // fingers on the left wrist (toward the elbow on the right) and
            // "up" points away from you -- 180-degree turns about (1,0,-+1).
            const float h = 0.70710678f;
            XrQuaternionf face;
            face.w = 0.0f; face.y = 0.0f; face.x = h; face.z = (side == 0) ? -h : h;
            pose.orientation = QuatMul(QuatMul(w_grip[side].orientation, face), EulerDeg(rot));
        }
        else {
            pose.orientation = QuatMul(w_grip[side].orientation, EulerDeg(rot));
        }
        if (!GazeShows(side, headPose, pose)) continue;

        const bool sel = w_gripHeld[side];
        // The game's own selection list (crop) gets a bigger window; the
        // compact wrist list stays close to watch size.
        float width = w_list[side].open ? w_widthM * 1.3f : (sel ? w_widthM * 1.6f : w_widthM);
        if (side == 0 && remapOpen) width = w_widthM * 2.0f;   // the remap panel: readable, not watch-sized
        XrCompositionLayerQuad& q = w_quad[side];
        q = XrCompositionLayerQuad{ XR_TYPE_COMPOSITION_LAYER_QUAD };
        q.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT | XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
        q.space = w_space;
        q.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        q.subImage.swapchain = w_swapchain;
        q.subImage.imageRect.offset = { side * kPanel, 0 };
        q.subImage.imageRect.extent = { kPanel, w_panelH[side] };
        q.subImage.imageArrayIndex = 0;
        q.pose = pose;
        q.size.width = width;
        q.size.height = width * (float)w_panelH[side] / (float)kPanel;
        outLayers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&q));
        if (logged < w_logFrames) {
            logged++;
            DebugLogger::LogFormat("Wrist HUD: %s panel shown at (%.2f,%.2f,%.2f), %.0fx%.0f mm%s | LIFE %d/%d weapon %d item %d",
                side == 0 ? "left" : "right", pose.position.x, pose.position.y, pose.position.z,
                q.size.width * 1000.0f, q.size.height * 1000.0f, sel ? " (selection window)" : "",
                g.vit, g.vitMax, g.weapon, g.item);
        }
    }
}

// ---- wrist quick-select: input -------------------------------------------------
// XR thread (vr_input.cpp). Returns via out-params whether the wrist owns each
// grip this frame (so the game must not see that grip, nor that hand's stick).
// Close a wrist list WITHOUT equipping anything (the grip is being used for
// something else -- the wall press).
void WristSelectCancel(int side) {
    if (side < 0 || side > 1) return;
    if (w_list[side].open) DebugLogger::LogFormat("Wrist select: %s list cancelled", side == 1 ? "weapon" : "item");
    w_list[side].open = false;
}

// A pressed while the ITEM list is open on the wrist: use the highlighted
// item (ration, medicine, diazepam...) the way the game's own item window does
// when you press ACTION in it. The use itself runs on the game thread
// (motion_aim.cpp) through the game's own routine, so every rule it applies --
// LIFE already full, frozen ration, counts -- is the game's. The list stays
// open so you can see the count drop.
bool WristSelectUseHighlighted() {
    if (!w_enabled || !w_ready || !w_selectOnWrist) return false;
    WristList& L = w_list[0];
    if (!L.open || L.count <= 0 || L.sel < 0 || L.sel >= L.count) return false;
    const int id = L.ids[L.sel];
    if (id < 0 || id >= 24) return false;
    int16_t count = -1;
    ReadGameShortSafe(kLinkvar + (37 + id) * 2, &count);
    MotionAimRequestItemUse(id);
    L.used = true;               // releasing the grip afterwards leaves the equipment alone
    DebugLogger::LogFormat("Wrist select: A -> use %s (have %d)", kItemNames[id], (int)count);
    return true;
}

void WristSelectInput(bool leftGrip, bool rightGrip, float lx, float ly, float rx, float ry,
                      bool* ownsLeft, bool* ownsRight) {
    *ownsLeft = false; *ownsRight = false;
    if (!w_enabled || !w_ready || !w_selectOnWrist) {
        w_list[0].open = w_list[1].open = false;
        return;
    }
    const GameHud g = ReadGameHud();
    const bool ctx = HudContextActive(g) && g.modal == 0;
    const ULONGLONG now = GetTickCount64();
    const bool grip[2] = { leftGrip, rightGrip };
    const float sx[2] = { lx, rx }, sy[2] = { ly, ry };
    for (int side = 0; side < 2; ++side) {
        WristList& L = w_list[side];
        const int equipped = side == 1 ? g.weapon : g.item;
        if (!L.open) {
            // Only a press that STARTS in gameplay opens the list; a grip
            // already held when gameplay resumes keeps going to the game.
            static bool prev[2] = { false, false };
            const bool pressed = grip[side] && !prev[side];
            prev[side] = grip[side];
            if (pressed && ctx) {
                L.open = true; L.navigated = false; L.used = false; L.openedAt = now; L.lastStep = 0; L.stepDir = 0;
                BuildWristList(side, equipped);
                static int openLog = 0;
                if (openLog < 6) { openLog++;
                    DebugLogger::LogFormat("Wrist select: %s list opened (%d entries, equipped %d)",
                        side == 1 ? "weapon" : "item", L.count - 1, equipped); }
            }
            if (!L.open) continue;
        }
        if (side == 0) *ownsLeft = true; else *ownsRight = true;

        if (grip[side]) {
            // Navigate: stick up = previous, down = next; repeats while held.
            const float v = (std::fabs(sy[side]) >= std::fabs(sx[side])) ? -sy[side] : sx[side];
            const int dir = v > 0.6f ? 1 : (v < -0.6f ? -1 : 0);
            if (dir != 0 && L.count > 0 && (dir != L.stepDir || now - L.lastStep >= 170)) {
                L.sel = (L.sel + dir + L.count) % L.count;   // first flick steps at once, holding repeats
                L.lastStep = now;
                L.navigated = true;
            }
            L.stepDir = dir;
            continue;
        }

        // Released: equip. A quick tap with no scrolling toggles, like the game.
        L.open = false;
        if (!ctx || L.used) continue;
        const bool tap = !L.navigated && (now - L.openedAt) < 300;
        if (tap) {
            if (equipped >= 0) { L.lastEquipped = equipped; EquipFromWrist(side, -1); }
            else if (L.lastEquipped >= 0) {
                int16_t v = -1;
                const int baseIdx = side == 1 ? 17 : 37;
                if (ReadGameShortSafe(kLinkvar + (baseIdx + L.lastEquipped) * 2, &v) && v >= 0)
                    EquipFromWrist(side, L.lastEquipped);
            }
        }
        else if (L.count > 0) {
            const int id = L.ids[L.sel];
            if (id != equipped) {
                if (equipped >= 0) L.lastEquipped = equipped;
                EquipFromWrist(side, id);
            }
        }
    }
}

// Hooks menu_draw_bar so boss / Meryl life bars reach the left wrist. Call
// once after MH_Initialize(). Signature-checked; a mismatch is a clean no-op.
bool InstallWristBarHook() {
    const uintptr_t base = (uintptr_t)GetModuleHandleA(nullptr);
    static const uint8_t kSig[12] = { 0x55, 0x8B,0xEC, 0x83,0xEC,0x1C, 0x8B,0x4D,0x20, 0x56, 0x8B,0xF1 };
    void* target = reinterpret_cast<void*>(base + kMenuDrawBarRva);
    if (std::memcmp(target, kSig, sizeof(kSig)) != 0) {
        DebugLogger::Log("Wrist HUD: menu_draw_bar signature mismatch at mgsi.exe+68DA6 -- boss / Meryl life on the wrist disabled");
        return false;
    }
    MH_STATUS st = MH_CreateHook(target, reinterpret_cast<void*>(&HookedMenuDrawBar), reinterpret_cast<void**>(&w_origDrawBar));
    if (st == MH_OK) st = MH_EnableHook(target);
    DebugLogger::LogFormat("Wrist HUD: life-bar hook at mgsi.exe+68DA6 %s (show_boss_life=%d, hide in view=%d)",
        st == MH_OK ? "INSTALLED -- boss and Meryl LIFE go to the left wrist" : "FAILED", w_showOtherBars ? 1 : 0, w_hideOtherBarsInView ? 1 : 0);
    return st == MH_OK;
}

// --- CODEC ON THE VIRTUAL SCREEN (2026-10-03) --------------------------------
// MenuWork (the menu task, menuman.c) lives at mgsi.exe+325FC0 (its init at
// +59991 is called with esi = 0x725FC0 from +598AD). Its state byte +0x2A is
// what the menu modules switch on: 1 = item menu (+6984D), 2 = weapon menu
// (+6A1E1), 4 = CODEC (radio update +63763 sets it on SELECT / an incoming
// call, together with GV_PauseLevel |= 1, and clears it to 0 only once the
// close animation has finished, field 0x20C == 0x14).
bool IsCodecOpen() {
    uint8_t st = 0;
    return ReadGameByteSafe(0x325FEA, &st) && st == 4;
}

// Hooks the PC port's hi-res text renderer (mgsi.exe+24020) so captions and
// codec text reach the virtual screen. Call once after MH_Initialize().
bool InstallGameTextHook() {
    const uintptr_t base = (uintptr_t)GetModuleHandleA(nullptr);
    static const uint8_t kSig[16] = { 0x55, 0x8B,0xEC, 0x81,0xEC,0x80,0x00,0x00,0x00, 0x53, 0x56, 0x8B,0x75,0x08, 0x33,0xDB };
    void* target = reinterpret_cast<void*>(base + kGameTextRva);
    if (std::memcmp(target, kSig, sizeof(kSig)) != 0) {
        DebugLogger::Log("Game text: signature mismatch at mgsi.exe+24020 -- captions / codec text will not reach the virtual screen");
        return false;
    }
    MH_STATUS st = MH_CreateHook(target, reinterpret_cast<void*>(&HookedGameText), reinterpret_cast<void**>(&g_origGameText));
    if (st == MH_OK) st = MH_EnableHook(target);
    if (st != MH_OK) g_origGameText = nullptr;
    DebugLogger::LogFormat("Game text: hi-res text hook at mgsi.exe+24020 %s (draw_on_screen=%d)",
        st == MH_OK ? "INSTALLED -- captions and codec text are drawn into the picture on the virtual screen" : "FAILED",
        g_gtDrawOnScreen ? 1 : 0);
    return st == MH_OK;
}

void SetRemapPanel(const RemapPanelView& view) {
    std::lock_guard<std::mutex> lk(w_remapMutex);
    w_remap = view;
}

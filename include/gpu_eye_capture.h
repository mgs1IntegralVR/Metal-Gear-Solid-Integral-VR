#pragma once
#include <cstdint>

struct RtEyeImage;

// ---------------------------------------------------------------------------
// GPU EYE CAPTURE (render twice, 2026-09-25)
//
// The flip path grabs each eye by PRESENTING it (an extra DirectDraw Flip
// through GOG's wrapper) and locking the primary -- two extra presents per
// game frame, a 16-bit 640x480 read-back, and most of render twice's cost.
//
// This path copies each eye on the GPU instead, straight out of the render
// target GOG's wrapper draws into on its Direct3D 9 device, into two private
// render targets (resolved from MSAA and scaled to gpu_capture_height, up to
// 1280x960), then reads them back once per frame. No extra presents; the
// game's own Flip shows the right eye on the monitor as normal.
//
// Everything here runs on the game thread, and only after it has SEEN the
// wrapper make its Direct3D 9 calls on that same thread (a threaded wrapper
// would make our calls unsafe -- then this stays off). render_twice.cpp checks
// the result against the flip path for the first few frames and falls back
// to the flip path for the session if the pictures do not match.
// ---------------------------------------------------------------------------
namespace GpuEye {

// Reads [render_twice] gpu_* keys. Call once from InstallRenderTwiceHooks.
void LoadConfig(const char* iniPath);

// 1 = ready: the device is known, the safety hooks are in, and the wrapper's
// Direct3D 9 calls come from THIS thread with no other thread using the device
// in the last second. 0 = not right now (try again later). -1 = never this
// session. Installs the hooks on first call. `why` gets a short reason.
int Ready(const char** why);

// Snapshot of the wrapper's Direct3D 9 draw-call counter (for "did the wrapper
// actually draw during DrawOTag, or does it defer to Flip?").
long DrawCallCount();

// Copies the currently bound render target into eye 0 or 1 (GPU only, no wait).
bool CopyEye(int eye);

// Reads eye 0 or 1 back into `out` (BGRA in out.rgba, out.bgra = true).
// Blocks until the GPU has finished the copy.
bool Readback(int eye, RtEyeImage& out, double* ms);

// Releases every Direct3D resource (device Reset / shutdown).
void ReleaseAll();

// One-line description of the source render target and output size, for logs.
const char* Describe();

}  // namespace GpuEye

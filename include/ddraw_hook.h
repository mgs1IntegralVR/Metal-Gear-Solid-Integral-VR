#pragma once
#include <cstdint>
#include <vector>

using VRFrameTickFn = void(*)();

bool InstallDdrawHooks(VRFrameTickFn onFrameTick);

// Copies the most recently flipped DirectDraw primary-surface frame into
// outRgba as tightly-packed, top-down RGBA8 pixels. Returns false if no
// frame has been captured yet. See ddraw_hook.cpp for capture details and
// diagnostic logging.
// outEye receives which eye the frame was rendered for when alternate-eye
// stereo is active (0 = left, 1 = right, -1 = mono). Optional.
// outViewRec (optional) receives the head pose that frame was drawn from --
// see FrameViewRecord in camera_write_hook.h. Invalid when the frame's camera
// was not head-driven (menus, codec, native mode, stalled cutscenes).
struct FrameViewRecord;
bool GetLatestDdrawFrameRgba(std::vector<uint8_t>& outRgba, int32_t& outWidth, int32_t& outHeight,
                             int* outEye = nullptr, FrameViewRecord* outViewRec = nullptr);

// Increments once per genuinely new captured frame. The VR frame loop runs far
// faster than the game produces frames, so this lets it skip the copy, the
// rescale and the texture upload when nothing has changed. 0 means no frame
// from this path yet (e.g. the D3D9 fallback is the source instead).
uint64_t GetDdrawFrameSequence();
// True when the frame most recently handed out by GetLatestDdrawFrameRgba has
// already had the wrist-HUD crops and masks applied (on the converter thread),
// so the XR thread must not do it again.
bool LastDdrawFrameHudDone();

// True while the PC port's own menus (pause / LOAD GAME / SAVE GAME / options)
// are being drawn straight onto the primary with GDI. The frame the headset
// gets then is a flat 2D menu (mono, no view record), and vr_injection shows it
// on the world-locked virtual screen rather than across your whole view.
bool IsDdrawMenuOverlayActive();

// ---- RENDER TWICE (2026-09-24), see render_twice.h -------------------------
// Frames of a stereo pair still waiting to be handed out by
// GetLatestDdrawFrameRgba (0, 1 or 2). The consumer keeps calling while this is
// non-zero so both eyes of a pair are uploaded together.
int GetPendingDdrawFrameCount();

// Blocking Lock + RGBA8 conversion of any DirectDraw surface (an
// IDirectDrawSurface7*, passed as void* to keep ddraw.h out of this header).
bool CaptureSurfaceRgbaBlocking(void* surface, std::vector<uint8_t>& out, int32_t& outW, int32_t& outH,
                                double* outLockMs, double* outConvMs);

// One eye of a render-twice pair. A 16-bit primary is kept as raw pixels and
// converted to RGBA on the XR thread when it is handed out; anything else is
// converted at capture time into rgba.
struct RtEyeImage {
	std::vector<uint8_t>  rgba;
	std::vector<uint16_t> raw16;
	int32_t width = 0, height = 0;
	bool bgra = false;   // rgba actually holds B,G,R,x (GPU eye capture); swizzled off the XR thread
	bool hudDone = false; // wrist-HUD crops/masks already applied (converter thread)
};

// Publishes a true left/right pair (both drawn from the same game frame).
// Images are swapped in, not copied.
// presentedByUs: the flip path presented both eyes itself, so the game's own
// next Flip is redundant (skip_game_flip). The GPU path presents nothing and
// passes false -- the game's own Flip is then the only present and must run.
// GPU capture of an ordinary (single) game frame: becomes the latest frame,
// tagged with `eye` (-1 = mono) and its render pose. Replaces the after-Flip
// read-back of the primary while these keep coming.
void PublishGpuMonoFrame(RtEyeImage& img, int eye, const FrameViewRecord& rec);
// Eye tag and pose of the frame being drawn right now (after this frame's
// present, before/while its DrawOTag runs).
bool GetDrawnFrameTag(int* eye, FrameViewRecord* rec);
void PublishRenderTwicePair(RtEyeImage& left, RtEyeImage& right, const FrameViewRecord& rec,
                            bool presentedByUs = true);

// Render twice: present the back buffer with the game's ORIGINAL Flip (no hook
// bookkeeping) and read the primary back -- the one read-back path proven to
// work through GOG's wrapper. Hardware flip mode only.
bool RtFlipAndCapturePrimary(RtEyeImage& out, double* outFlipMs, double* outLockMs, double* outCopyMs);
// Black colour fill, as the game's own present does after its Flip.
void RtClearSurfaceBlack(void* surface);

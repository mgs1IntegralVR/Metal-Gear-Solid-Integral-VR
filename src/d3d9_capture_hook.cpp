#include <windows.h>
#include <d3d9.h>
#include <cstring>   // memcpy, used by the depth-stencil probe
#include <mutex>
#include <vector>
#include "MinHook.h"
#include "../include/debug_logging.h"
#include "../include/d3d9_capture_hook.h"

#pragma comment(lib, "d3d9.lib")

using Direct3DCreate9_t = IDirect3D9*(WINAPI*)(UINT);
using IDirect3D9_CreateDevice_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
using IDirect3DDevice9_Present_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
using IDirect3DSwapChain9_Present_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DSwapChain9*, const RECT*, const RECT*, HWND, const RGNDATA*, DWORD);
using IDirect3DDevice9Ex_PresentEx_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9Ex*, const RECT*, const RECT*, HWND, const RGNDATA*, DWORD);

static Direct3DCreate9_t g_OriginalDirect3DCreate9 = nullptr;
static IDirect3D9_CreateDevice_t g_OriginalCreateDevice = nullptr;
static IDirect3DDevice9_Present_t g_OriginalPresent = nullptr;
static IDirect3DSwapChain9_Present_t g_OriginalSwapChainPresent = nullptr;
static IDirect3DDevice9Ex_PresentEx_t g_OriginalPresentEx = nullptr;

static VRFrameTickFn g_OnFrameTick = nullptr;
static IDirect3DDevice9* g_lastHookedDevice = nullptr;
void* GetWrapperD3D9Device() { return g_lastHookedDevice; }

static std::mutex g_captureMutex;
static std::vector<uint8_t> g_capturedRgba;
static int32_t g_capturedWidth = 0;
static int32_t g_capturedHeight = 0;
static bool g_hasCapturedFrame = false;

// Reusable system-memory surface the backbuffer gets copied into every
// frame; recreated only when size/format actually change.
static IDirect3DSurface9* g_sysMemSurface = nullptr;
static UINT g_sysMemWidth = 0;
static UINT g_sysMemHeight = 0;
static D3DFORMAT g_sysMemFormat = D3DFMT_UNKNOWN;

static bool g_loggedGetBackBufferFailure = false;
static bool g_loggedCreateSurfaceFailure = false;
static bool g_loggedGetRenderTargetDataFailure = false;
static bool g_loggedLockFailure = false;
static bool g_loggedUnsupportedFormat = false;
static bool g_loggedFirstCaptureSuccess = false;

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

// Handles the two formats that make up the overwhelming majority of D3D9
// backbuffers (X8R8G8B8 / A8R8G8B8). Logs and bails on anything else so we
// find out rather than silently showing corrupted color.
static bool ConvertRowToRgba(const uint8_t* src, uint8_t* dst, UINT pixels, D3DFORMAT format) {
	if (format == D3DFMT_X8R8G8B8 || format == D3DFMT_A8R8G8B8) {
		for (UINT x = 0; x < pixels; ++x) {
			const uint8_t* s = src + (size_t)x * 4u;
			uint8_t* d = dst + (size_t)x * 4u;
			d[0] = s[2]; // R
			d[1] = s[1]; // G
			d[2] = s[0]; // B
			d[3] = (format == D3DFMT_A8R8G8B8) ? s[3] : 255;
		}
		return true;
	}
	return false;
}

static void CapturePresentedFrame(IDirect3DDevice9* device) {
	if (!device) return;

	IDirect3DSurface9* backBuffer = nullptr;
	HRESULT hr = device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer);
	if (FAILED(hr) || !backBuffer) {
		if (!g_loggedGetBackBufferFailure) {
			DebugLogger::LogFormat("ERROR: IDirect3DDevice9::GetBackBuffer failed: 0x%08X", (unsigned)hr);
			g_loggedGetBackBufferFailure = true;
		}
		return;
	}

	D3DSURFACE_DESC desc{};
	backBuffer->GetDesc(&desc);

	if (!g_sysMemSurface || g_sysMemWidth != desc.Width || g_sysMemHeight != desc.Height || g_sysMemFormat != desc.Format) {
		if (g_sysMemSurface) {
			g_sysMemSurface->Release();
			g_sysMemSurface = nullptr;
		}
		hr = device->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format, D3DPOOL_SYSTEMMEM, &g_sysMemSurface, nullptr);
		if (FAILED(hr) || !g_sysMemSurface) {
			if (!g_loggedCreateSurfaceFailure) {
				DebugLogger::LogFormat("ERROR: CreateOffscreenPlainSurface failed: 0x%08X (fmt=%d %ux%u)", (unsigned)hr, (int)desc.Format, desc.Width, desc.Height);
				g_loggedCreateSurfaceFailure = true;
			}
			backBuffer->Release();
			return;
		}
		g_sysMemWidth = desc.Width;
		g_sysMemHeight = desc.Height;
		g_sysMemFormat = desc.Format;
	}

	hr = device->GetRenderTargetData(backBuffer, g_sysMemSurface);
	backBuffer->Release();
	if (FAILED(hr)) {
		if (!g_loggedGetRenderTargetDataFailure) {
			DebugLogger::LogFormat("ERROR: IDirect3DDevice9::GetRenderTargetData failed: 0x%08X", (unsigned)hr);
			g_loggedGetRenderTargetDataFailure = true;
		}
		return;
	}

	D3DLOCKED_RECT locked{};
	hr = g_sysMemSurface->LockRect(&locked, nullptr, D3DLOCK_READONLY);
	if (FAILED(hr)) {
		if (!g_loggedLockFailure) {
			DebugLogger::LogFormat("ERROR: IDirect3DSurface9::LockRect failed: 0x%08X", (unsigned)hr);
			g_loggedLockFailure = true;
		}
		return;
	}

	std::vector<uint8_t> converted((size_t)desc.Width * (size_t)desc.Height * 4u);
	const uint8_t* srcBase = reinterpret_cast<const uint8_t*>(locked.pBits);
	bool ok = true;
	for (UINT y = 0; y < desc.Height && ok; ++y) {
		const uint8_t* srcRow = srcBase + (size_t)y * (size_t)locked.Pitch;
		uint8_t* dstRow = converted.data() + (size_t)y * (size_t)desc.Width * 4u;
		ok = ConvertRowToRgba(srcRow, dstRow, desc.Width, desc.Format);
	}

	g_sysMemSurface->UnlockRect();

	if (!ok) {
		if (!g_loggedUnsupportedFormat) {
			DebugLogger::LogFormat("WARNING: Backbuffer format %d not handled by capture (expected X8R8G8B8/A8R8G8B8)", (int)desc.Format);
			g_loggedUnsupportedFormat = true;
		}
		return;
	}

	{
		std::lock_guard<std::mutex> lock(g_captureMutex);
		g_capturedRgba = std::move(converted);
		g_capturedWidth = (int32_t)desc.Width;
		g_capturedHeight = (int32_t)desc.Height;
		g_hasCapturedFrame = true;
	}

	if (!g_loggedFirstCaptureSuccess) {
		DebugLogger::LogFormat("Direct3D9 backbuffer capture active: %ux%u fmt=%d", desc.Width, desc.Height, (int)desc.Format);
		g_loggedFirstCaptureSuccess = true;
	}
}

static HRESULT STDMETHODCALLTYPE HookedPresent(IDirect3DDevice9* self, const RECT* srcRect, const RECT* destRect, HWND destWindowOverride, const RGNDATA* dirtyRegion);

bool GetLatestD3D9FrameRgba(std::vector<uint8_t>& outRgba, int32_t& outWidth, int32_t& outHeight) {
	// Lightweight periodic status log: with multiple IDirect3D9Device9
	// instances potentially hooked (see HookedCreateDevice), the useful
	// signal is simply whether frames are actually arriving from any of
	// them -- not whether one specific device's vtable slot still matches,
	// since an earlier hooked-but-discarded device's slot getting reset is
	// now an expected, harmless event rather than something to warn about.
	static DWORD lastStatusTick = 0;
	DWORD now = GetTickCount();
	if ((now - lastStatusTick) > 3000) {
		lastStatusTick = now;
		std::lock_guard<std::mutex> statusLock(g_captureMutex);
		DebugLogger::LogFormat("D3D9 capture status: hasCapturedFrame=%d lastHookedDevice=%p", g_hasCapturedFrame ? 1 : 0, (void*)g_lastHookedDevice);
	}

	std::lock_guard<std::mutex> lock(g_captureMutex);
	if (!g_hasCapturedFrame) {
		return false;
	}
	outRgba = g_capturedRgba;
	outWidth = g_capturedWidth;
	outHeight = g_capturedHeight;
	return true;
}

static HRESULT STDMETHODCALLTYPE HookedSwapChainPresent(IDirect3DSwapChain9* self, const RECT* srcRect, const RECT* destRect, HWND destWindowOverride, const RGNDATA* dirtyRegion, DWORD flags) {
	static bool loggedFirstCall = false;
	if (!loggedFirstCall) {
		DebugLogger::LogFormat("HookedSwapChainPresent called for the first time (swapchain=%p)", (void*)self);
		loggedFirstCall = true;
	}

	IDirect3DDevice9* owningDevice = nullptr;
	if (SUCCEEDED(self->GetDevice(&owningDevice)) && owningDevice) {
		CapturePresentedFrame(owningDevice);
		owningDevice->Release();
	}

	if (g_OnFrameTick) {
		g_OnFrameTick();
	}

	return g_OriginalSwapChainPresent ? g_OriginalSwapChainPresent(self, srcRect, destRect, destWindowOverride, dirtyRegion, flags) : D3DERR_INVALIDCALL;
}

static HRESULT STDMETHODCALLTYPE HookedPresentEx(IDirect3DDevice9Ex* self, const RECT* srcRect, const RECT* destRect, HWND destWindowOverride, const RGNDATA* dirtyRegion, DWORD flags) {
	static bool loggedFirstCall = false;
	if (!loggedFirstCall) {
		DebugLogger::LogFormat("HookedPresentEx called for the first time (deviceEx=%p)", (void*)self);
		loggedFirstCall = true;
	}

	// IDirect3DDevice9Ex derives from IDirect3DDevice9, so the same pointer
	// is valid as an IDirect3DDevice9* for our existing capture logic.
	CapturePresentedFrame(reinterpret_cast<IDirect3DDevice9*>(self));

	if (g_OnFrameTick) {
		g_OnFrameTick();
	}

	return g_OriginalPresentEx ? g_OriginalPresentEx(self, srcRect, destRect, destWindowOverride, dirtyRegion, flags) : D3DERR_INVALIDCALL;
}

// --- Targeted call loggers ---------------------------------------------
// Covers the IDirect3DDevice9 methods most likely to be where the wrapper
// actually does its per-frame work, in case none of Present/SwapChain
// Present/PresentEx are it (all three ruled out empirically at this point).
// Unlike a generic variadic passthrough (unsafe across mixed calling
// conventions/argument types), each of these uses the method's real,
// correctly-typed signature, so calling through to the original is safe.
// Each logs its name exactly once, then behaves identically to the original.

using BeginScene_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*);
using EndScene_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*);
using Clear_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, DWORD, const D3DRECT*, DWORD, D3DCOLOR, float, DWORD);
using SetRenderTarget_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, DWORD, IDirect3DSurface9*);
using StretchRect_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, IDirect3DSurface9*, const RECT*, IDirect3DSurface9*, const RECT*, D3DTEXTUREFILTERTYPE);
using UpdateSurface_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, IDirect3DSurface9*, const RECT*, IDirect3DSurface9*, const POINT*);

static BeginScene_t g_OrigBeginScene = nullptr;
static EndScene_t g_OrigEndScene = nullptr;
static Clear_t g_OrigClear = nullptr;
static SetRenderTarget_t g_OrigSetRenderTarget = nullptr;
static StretchRect_t g_OrigStretchRect = nullptr;
static UpdateSurface_t g_OrigUpdateSurface = nullptr;

#define LOG_ONCE(name) do { static bool fired = false; if (!fired) { fired = true; DebugLogger::Log("Device method called: " name); } } while (0)

static HRESULT STDMETHODCALLTYPE Logged_BeginScene(IDirect3DDevice9* self) {
	LOG_ONCE("BeginScene");
	return g_OrigBeginScene ? g_OrigBeginScene(self) : D3DERR_INVALIDCALL;
}
static HRESULT STDMETHODCALLTYPE Logged_EndScene(IDirect3DDevice9* self) {
	LOG_ONCE("EndScene");
	return g_OrigEndScene ? g_OrigEndScene(self) : D3DERR_INVALIDCALL;
}
static HRESULT STDMETHODCALLTYPE Logged_Clear(IDirect3DDevice9* self, DWORD count, const D3DRECT* rects, DWORD flags, D3DCOLOR color, float z, DWORD stencil) {
	LOG_ONCE("Clear");
	return g_OrigClear ? g_OrigClear(self, count, rects, flags, color, z, stencil) : D3DERR_INVALIDCALL;
}
static HRESULT STDMETHODCALLTYPE Logged_SetRenderTarget(IDirect3DDevice9* self, DWORD index, IDirect3DSurface9* rt) {
	LOG_ONCE("SetRenderTarget");
	return g_OrigSetRenderTarget ? g_OrigSetRenderTarget(self, index, rt) : D3DERR_INVALIDCALL;
}
static HRESULT STDMETHODCALLTYPE Logged_StretchRect(IDirect3DDevice9* self, IDirect3DSurface9* src, const RECT* srcRect, IDirect3DSurface9* dst, const RECT* dstRect, D3DTEXTUREFILTERTYPE filter) {
	LOG_ONCE("StretchRect");
	return g_OrigStretchRect ? g_OrigStretchRect(self, src, srcRect, dst, dstRect, filter) : D3DERR_INVALIDCALL;
}
static HRESULT STDMETHODCALLTYPE Logged_UpdateSurface(IDirect3DDevice9* self, IDirect3DSurface9* src, const RECT* srcRect, IDirect3DSurface9* dst, const POINT* dstPoint) {
	LOG_ONCE("UpdateSurface");
	return g_OrigUpdateSurface ? g_OrigUpdateSurface(self, src, srcRect, dst, dstPoint) : D3DERR_INVALIDCALL;
}

static void InstallGenericCallLogger(IDirect3DDevice9* device) {
	if (!device) return;
	// Verified indices only (cross-checked against d3d9.h method order).
	// DrawPrimitive/DrawIndexedPrimitive deliberately omitted: their exact
	// slot depends on counting through several SetXxx/GetXxx state methods
	// I don't have full confidence in without the header in front of me,
	// and a wrong index here would silently patch some unrelated method's
	// vtable slot instead -- worse than just not covering this one.
	PatchVTable(device, 41, reinterpret_cast<void*>(&Logged_BeginScene), reinterpret_cast<void**>(&g_OrigBeginScene));
	PatchVTable(device, 42, reinterpret_cast<void*>(&Logged_EndScene), reinterpret_cast<void**>(&g_OrigEndScene));
	PatchVTable(device, 43, reinterpret_cast<void*>(&Logged_Clear), reinterpret_cast<void**>(&g_OrigClear));
	PatchVTable(device, 37, reinterpret_cast<void*>(&Logged_SetRenderTarget), reinterpret_cast<void**>(&g_OrigSetRenderTarget));
	PatchVTable(device, 34, reinterpret_cast<void*>(&Logged_StretchRect), reinterpret_cast<void**>(&g_OrigStretchRect));
	PatchVTable(device, 30, reinterpret_cast<void*>(&Logged_UpdateSurface), reinterpret_cast<void**>(&g_OrigUpdateSurface));
	DebugLogger::LogFormat("Generic call logger installed (device=%p)", (void*)device);
}

// Readable names for the handful of D3DFORMAT values that matter to the depth
// probe. Anything unrecognised falls through to its numeric value, which is
// still enough to look up.
static const char* DescribeD3DFormat(D3DFORMAT fmt) {
	switch ((int)fmt) {
	case 0:  return "NONE";
	case 75: return "D3DFMT_D24S8 (NOT lockable)";
	case 77: return "D3DFMT_D24X8 (NOT lockable)";
	case 79: return "D3DFMT_D24X4S4 (NOT lockable)";
	case 70: return "D3DFMT_D16_LOCKABLE (LOCKABLE)";
	case 71: return "D3DFMT_D32 (NOT lockable)";
	case 73: return "D3DFMT_D15S1 (NOT lockable)";
	case 80: return "D3DFMT_D16 (NOT lockable)";
	case 82: return "D3DFMT_D32F_LOCKABLE (LOCKABLE)";
	case 83: return "D3DFMT_D24FS8 (NOT lockable)";
	case 21: return "D3DFMT_A8R8G8B8";
	case 22: return "D3DFMT_X8R8G8B8";
	case 23: return "D3DFMT_R5G6B5";
	default: return "<see numeric value>";
	}
}

// ---- DEPTH REPROJECTION FEASIBILITY, PART 2 (NEW 2026-08-16) --------------
// Runs ONCE, on the first Present, and never again. Answers the question the
// CreateDevice log opens: is there actually a depth-stencil surface bound to
// this device, what is it, and can we read it?
//
// Read-only and fail-soft by construction: every call is checked, the surface
// is released on every path, and a failed lock is reported as a RESULT, not
// treated as an error. Most D3D9 depth formats genuinely cannot be locked --
// a clean D3DERR_INVALIDCALL here is a real answer about the hardware path,
// not a bug in this code, and the log says so explicitly so nobody spends a
// session "fixing" it.
static void ProbeDepthStencilOnce(IDirect3DDevice9* device) {
	static bool probed = false;
	if (probed || !device) return;
	probed = true;

	IDirect3DSurface9* depth = nullptr;
	HRESULT hr = device->GetDepthStencilSurface(&depth);

	if (FAILED(hr) || !depth) {
		DebugLogger::LogFormat(
			"DEPTH PROBE (Present): GetDepthStencilSurface returned 0x%08X, surface=%p -- NO depth "
			"buffer is bound to this device. Combined with the CreateDevice line above, this means "
			"the D3D9 path carries no per-pixel scene depth at all: the wrapper is presenting an "
			"already-flattened 2D image. Depth-based reprojection (handoff route (a), ~55%%) is NOT "
			"reachable this way -- drop it to near zero and put the remaining hope on route (b), "
			"rendering twice per frame.",
			(unsigned)hr, (void*)depth);
		if (depth) depth->Release();
		return;
	}

	D3DSURFACE_DESC desc{};
	HRESULT descHr = depth->GetDesc(&desc);
	if (FAILED(descHr)) {
		DebugLogger::LogFormat("DEPTH PROBE (Present): depth surface exists (%p) but GetDesc failed: 0x%08X",
			(void*)depth, (unsigned)descHr);
		depth->Release();
		return;
	}

	DebugLogger::LogFormat(
		"DEPTH PROBE (Present): depth-stencil surface FOUND. %ux%u fmt=%d (%s) pool=%d usage=0x%08X "
		"multisample=%d quality=%lu type=%d",
		desc.Width, desc.Height, (int)desc.Format, DescribeD3DFormat(desc.Format),
		(int)desc.Pool, (unsigned)desc.Usage, (int)desc.MultiSampleType,
		(unsigned long)desc.MultiSampleQuality, (int)desc.Type);

	// The decisive test. D3DLOCK_READONLY so we can never modify what the
	// game is about to render against, even by accident.
	D3DLOCKED_RECT locked{};
	HRESULT lockHr = depth->LockRect(&locked, nullptr, D3DLOCK_READONLY);
	if (SUCCEEDED(lockHr)) {
		// Sample a few raw values from the middle row purely to confirm the
		// data is real rather than a zeroed staging allocation.
		unsigned first = 0, mid = 0;
		if (locked.pBits && locked.Pitch > 0) {
			const unsigned char* row =
				static_cast<const unsigned char*>(locked.pBits) + (size_t)(desc.Height / 2) * (size_t)locked.Pitch;
			memcpy(&first, row, sizeof(first));
			memcpy(&mid, row + (size_t)(locked.Pitch / 2) - 2, sizeof(mid));
		}
		DebugLogger::LogFormat(
			"DEPTH PROBE: LockRect SUCCEEDED (pitch=%d). Centre-row samples: 0x%08X 0x%08X. "
			"This is the good outcome -- the depth buffer is directly readable, so depth-based "
			"reprojection is feasible without a shader resolve. Raise handoff route (a) well above 55%%.",
			locked.Pitch, first, mid);
		depth->UnlockRect();
	}
	else {
		DebugLogger::LogFormat(
			"DEPTH PROBE: LockRect FAILED: 0x%08X. THIS IS EXPECTED AND IS NOT A BUG -- almost no "
			"driver allows locking a non-lockable depth format (see the format name above). It does "
			"NOT kill depth reprojection: it means the depth would have to be resolved through a "
			"shader into a readable render target first (INTZ/RAWZ-style), which is more work but is "
			"a solved technique. The surface EXISTING is the part that matters.",
			(unsigned)lockHr);
	}

	depth->Release();
}

static HRESULT STDMETHODCALLTYPE HookedPresent(IDirect3DDevice9* self, const RECT* srcRect, const RECT* destRect, HWND destWindowOverride, const RGNDATA* dirtyRegion) {
	static bool loggedFirstCall = false;
	if (!loggedFirstCall) {
		DebugLogger::LogFormat("HookedPresent called for the first time (device=%p)", (void*)self);
		loggedFirstCall = true;
	}

	// One-shot, read-only, before the capture so the depth surface is still
	// whatever the game last rendered against.
	ProbeDepthStencilOnce(self);

	// Capture BEFORE the real Present(): under the common DISCARD swap
	// effect, backbuffer contents become undefined the moment Present()
	// returns, so this is the last safe point to read it.
	CapturePresentedFrame(self);

	if (g_OnFrameTick) {
		g_OnFrameTick();
	}

	return g_OriginalPresent ? g_OriginalPresent(self, srcRect, destRect, destWindowOverride, dirtyRegion) : D3DERR_INVALIDCALL;
}

static HRESULT STDMETHODCALLTYPE HookedCreateDevice(IDirect3D9* self, UINT adapter, D3DDEVTYPE deviceType, HWND hFocusWindow, DWORD behaviorFlags, D3DPRESENT_PARAMETERS* presentParams, IDirect3DDevice9** ppReturnedDeviceInterface) {
	HRESULT hr = g_OriginalCreateDevice ? g_OriginalCreateDevice(self, adapter, deviceType, hFocusWindow, behaviorFlags, presentParams, ppReturnedDeviceInterface) : D3DERR_INVALIDCALL;

	DebugLogger::LogFormat("IDirect3D9::CreateDevice called: hr=0x%08X behaviorFlags=0x%08X%s windowed=%d backbuffer=%ux%u fmt=%d",
		(unsigned)hr, (unsigned)behaviorFlags, (behaviorFlags & D3DCREATE_MULTITHREADED) ? " (MULTITHREADED)" : "",
		presentParams ? presentParams->Windowed : -1,
		presentParams ? presentParams->BackBufferWidth : 0,
		presentParams ? presentParams->BackBufferHeight : 0,
		presentParams ? (int)presentParams->BackBufferFormat : -1);

	// ---- DEPTH REPROJECTION FEASIBILITY, PART 1 (NEW 2026-08-16) ----------
	// This is the cheapest decisive evidence in the whole project for whether
	// depth-based reprojection (handoff §5, ~55%) is possible at all, and it
	// costs one log line because these fields are already being handed to us.
	//
	// Depth reprojection needs the scene's Z-buffer. If the ddraw wrapper
	// creates its device with EnableAutoDepthStencil=FALSE and
	// AutoDepthStencilFormat=0, then there is no depth-stencil surface
	// attached to this device at all -- which would mean the wrapper is only
	// compositing an already-finished 2D image, there is no per-pixel depth
	// anywhere in the D3D9 path, and route (a) is dead without needing any
	// further work. That would be a genuinely useful answer: it would move
	// the remaining hope entirely onto route (b), making the engine render
	// twice per frame (~30%).
	//
	// If instead auto depth-stencil IS enabled, the format tells us
	// immediately whether we can ever read it: D3DFMT_D16_LOCKABLE (80) and
	// D3DFMT_D32F_LOCKABLE (82) can be locked directly; D24S8 (75), D24X8
	// (77), D16 (80 is lockable, 80 vs 81 differ) and friends cannot be
	// locked on essentially any driver, and GetRenderTargetData does not
	// work on depth surfaces either. A non-lockable format does NOT kill the
	// idea -- it means the depth has to be resolved through a shader into a
	// readable render target -- but it decides how expensive route (a) is
	// before a line of reprojection code gets written.
	if (presentParams) {
		DebugLogger::LogFormat(
			"DEPTH PROBE (CreateDevice): EnableAutoDepthStencil=%d AutoDepthStencilFormat=%d (%s) | "
			"MultiSampleType=%d quality=%lu | SwapEffect=%d BackBufferCount=%u | flags=0x%08X",
			presentParams->EnableAutoDepthStencil ? 1 : 0,
			(int)presentParams->AutoDepthStencilFormat,
			DescribeD3DFormat(presentParams->AutoDepthStencilFormat),
			(int)presentParams->MultiSampleType,
			(unsigned long)presentParams->MultiSampleQuality,
			(int)presentParams->SwapEffect,
			presentParams->BackBufferCount,
			(unsigned)presentParams->Flags);
		if (!presentParams->EnableAutoDepthStencil) {
			DebugLogger::Log(
				"DEPTH PROBE: EnableAutoDepthStencil is FALSE -- this device was created with NO "
				"depth-stencil surface. If GetDepthStencilSurface at Present also returns nothing, "
				"there is no scene Z-buffer in the D3D9 path at all and depth-based reprojection "
				"(handoff route (a)) is not possible from here. That would leave only route (b), "
				"making the engine render twice per frame.");
		}
	}

	if (SUCCEEDED(hr) && ppReturnedDeviceInterface && *ppReturnedDeviceInterface) {
		void** vtbl = *reinterpret_cast<void***>(*ppReturnedDeviceInterface);
		void* presentSlotAddr = vtbl ? vtbl[17] : nullptr;
		HMODULE ownerModule = nullptr;
		char ownerPath[MAX_PATH] = {};
		if (presentSlotAddr && GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (LPCSTR)presentSlotAddr, &ownerModule) && ownerModule) {
			GetModuleFileNameA(ownerModule, ownerPath, MAX_PATH);
		}
		DebugLogger::LogFormat("Device vtable[17] (Present) code lives in: %s (addr=%p)", ownerPath[0] ? ownerPath : "<unknown module>", presentSlotAddr);

		// As with CreateDevice above: no "only once" guard. Each device
		// instance the wrapper creates gets its own Present hook, since we
		// now know (from log evidence) that an earlier hooked device's
		// vtable slot can get silently reset -- almost certainly because
		// that device gets torn down and replaced by a second, real one.
		void* originalPresentForThisDevice = nullptr;
		if (PatchVTable(*ppReturnedDeviceInterface, 17, reinterpret_cast<void*>(&HookedPresent), &originalPresentForThisDevice)) {
			g_lastHookedDevice = *ppReturnedDeviceInterface;
			if (!g_OriginalPresent) {
				g_OriginalPresent = reinterpret_cast<IDirect3DDevice9_Present_t>(originalPresentForThisDevice);
			}
			DebugLogger::LogFormat("IDirect3DDevice9::Present hook installed (device=%p)", (void*)*ppReturnedDeviceInterface);

			void** vtblAfter = *reinterpret_cast<void***>(*ppReturnedDeviceInterface);
			DebugLogger::LogFormat("Read-back check: vtbl[17] now = %p (expected HookedPresent = %p) -> %s",
				vtblAfter ? vtblAfter[17] : nullptr,
				reinterpret_cast<void*>(&HookedPresent),
				(vtblAfter && vtblAfter[17] == reinterpret_cast<void*>(&HookedPresent)) ? "MATCH" : "MISMATCH -- something else owns this vtable slot");
		}

		// Also hook IDirect3DSwapChain9::Present (slot 3: QueryInterface,
		// AddRef, Release, Present), in case the wrapper presents via an
		// explicit swap chain object rather than calling the device's own
		// Present -- this is a real, known alternate path in D3D9 and would
		// explain a correctly-hooked, still-hooked device that nonetheless
		// never has its Present entered.
		IDirect3DSwapChain9* swapChain = nullptr;
		if (SUCCEEDED((*ppReturnedDeviceInterface)->GetSwapChain(0, &swapChain)) && swapChain) {
			void** scVtbl = *reinterpret_cast<void***>(swapChain);
			void* scPresentSlotAddr = scVtbl ? scVtbl[3] : nullptr;
			HMODULE scOwnerModule = nullptr;
			char scOwnerPath[MAX_PATH] = {};
			if (scPresentSlotAddr && GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (LPCSTR)scPresentSlotAddr, &scOwnerModule) && scOwnerModule) {
				GetModuleFileNameA(scOwnerModule, scOwnerPath, MAX_PATH);
			}
			DebugLogger::LogFormat("SwapChain vtable[3] (Present) code lives in: %s (addr=%p)", scOwnerPath[0] ? scOwnerPath : "<unknown module>", scPresentSlotAddr);

			void* originalSwapChainPresent = nullptr;
			if (PatchVTable(swapChain, 3, reinterpret_cast<void*>(&HookedSwapChainPresent), &originalSwapChainPresent)) {
				if (!g_OriginalSwapChainPresent) {
					g_OriginalSwapChainPresent = reinterpret_cast<IDirect3DSwapChain9_Present_t>(originalSwapChainPresent);
				}
				DebugLogger::LogFormat("IDirect3DSwapChain9::Present hook installed (swapchain=%p)", (void*)swapChain);
			}
			swapChain->Release();
		}

		// Also probe for IDirect3DDevice9Ex and hook PresentEx (slot 121:
		// the full IDirect3DDevice9 vtable [0..116] plus five Ex-only
		// methods [117..121], where PresentEx is the last). GOG's wrapper
		// has strong reasons to use the Ex device internally (flip-model
		// swap chains, tighter vsync/timing control) even though it only
		// ever hands the game a plain IDirect3DDevice9 -- if so, actual
		// presentation goes through PresentEx and neither hook above would
		// ever be entered, exactly what we've observed.
		IDirect3DDevice9Ex* deviceEx = nullptr;
		HRESULT qiHr = (*ppReturnedDeviceInterface)->QueryInterface(IID_IDirect3DDevice9Ex, reinterpret_cast<void**>(&deviceEx));
		if (SUCCEEDED(qiHr) && deviceEx) {
			DebugLogger::LogFormat("QueryInterface(IDirect3DDevice9Ex) succeeded (deviceEx=%p) -- this device is Ex-capable", (void*)deviceEx);

			void** exVtbl = *reinterpret_cast<void***>(deviceEx);
			void* presentExSlotAddr = exVtbl ? exVtbl[121] : nullptr;
			HMODULE exOwnerModule = nullptr;
			char exOwnerPath[MAX_PATH] = {};
			if (presentExSlotAddr && GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (LPCSTR)presentExSlotAddr, &exOwnerModule) && exOwnerModule) {
				GetModuleFileNameA(exOwnerModule, exOwnerPath, MAX_PATH);
			}
			DebugLogger::LogFormat("Device vtable[121] (PresentEx) code lives in: %s (addr=%p)", exOwnerPath[0] ? exOwnerPath : "<unknown module>", presentExSlotAddr);

			void* originalPresentEx = nullptr;
			if (PatchVTable(deviceEx, 121, reinterpret_cast<void*>(&HookedPresentEx), &originalPresentEx)) {
				if (!g_OriginalPresentEx) {
					g_OriginalPresentEx = reinterpret_cast<IDirect3DDevice9Ex_PresentEx_t>(originalPresentEx);
				}
				DebugLogger::LogFormat("IDirect3DDevice9Ex::PresentEx hook installed (deviceEx=%p)", (void*)deviceEx);
			}
			deviceEx->Release();
		}
		else {
			DebugLogger::LogFormat("QueryInterface(IDirect3DDevice9Ex) failed: 0x%08X -- this device is not Ex-capable, PresentEx is not the path here", (unsigned)qiHr);
		}

		InstallGenericCallLogger(*ppReturnedDeviceInterface);
	}

	return hr;
}

static IDirect3D9* WINAPI HookedDirect3DCreate9(UINT sdkVersion) {
	IDirect3D9* d3d9 = g_OriginalDirect3DCreate9 ? g_OriginalDirect3DCreate9(sdkVersion) : nullptr;

	DebugLogger::LogFormat("Direct3DCreate9 called: sdkVersion=%u result=%p", sdkVersion, (void*)d3d9);

	// Deliberately no "only hook once" guard here: the wrapper has been
	// observed calling Direct3DCreate9 more than once (likely a throwaway
	// caps-probe instance followed by the real one), and each returned
	// object has its own independent vtable -- so each needs its own patch,
	// even if a previous instance was already hooked.
	if (d3d9) {
		void* originalCreateDeviceForThisInstance = nullptr;
		if (PatchVTable(d3d9, 16, reinterpret_cast<void*>(&HookedCreateDevice), &originalCreateDeviceForThisInstance)) {
			DebugLogger::LogFormat("IDirect3D9::CreateDevice hook installed (factory=%p)", (void*)d3d9);
			// Only the very first captured original is kept -- see note by
			// g_OriginalCreateDevice's declaration on why one shared
			// trampoline is fine even with multiple hooked instances.
			if (!g_OriginalCreateDevice) {
				g_OriginalCreateDevice = reinterpret_cast<IDirect3D9_CreateDevice_t>(originalCreateDeviceForThisInstance);
			}
		}
	}

	return d3d9;
}

bool InstallD3D9CaptureHook(VRFrameTickFn onFrameTick) {
	g_OnFrameTick = onFrameTick;

	HMODULE hD3D9 = GetModuleHandleA("d3d9.dll");
	if (!hD3D9) {
		hD3D9 = LoadLibraryA("d3d9.dll");
	}
	if (!hD3D9) {
		DebugLogger::Log("ERROR: Could not load d3d9.dll");
		return false;
	}

	char resolvedPath[MAX_PATH] = {};
	if (GetModuleFileNameA(hD3D9, resolvedPath, MAX_PATH)) {
		DebugLogger::LogFormat("d3d9.dll resolved to: %s", resolvedPath);
	}

	auto pDirect3DCreate9 = reinterpret_cast<Direct3DCreate9_t>(GetProcAddress(hD3D9, "Direct3DCreate9"));
	if (!pDirect3DCreate9) {
		DebugLogger::Log("ERROR: Could not locate Direct3DCreate9 export");
		return false;
	}

	MH_STATUS s = MH_CreateHook(reinterpret_cast<LPVOID>(pDirect3DCreate9), reinterpret_cast<LPVOID>(&HookedDirect3DCreate9), reinterpret_cast<LPVOID*>(&g_OriginalDirect3DCreate9));
	if (s != MH_OK && s != MH_ERROR_ALREADY_CREATED) {
		DebugLogger::LogFormat("ERROR: MH_CreateHook(Direct3DCreate9) failed: %d", s);
		return false;
	}

	s = MH_EnableHook(reinterpret_cast<LPVOID>(pDirect3DCreate9));
	if (s != MH_OK && s != MH_ERROR_ENABLED) {
		DebugLogger::LogFormat("ERROR: MH_EnableHook(Direct3DCreate9) failed: %d", s);
		return false;
	}

	DebugLogger::Log("Direct3DCreate9 hook installed");
	return true;
}

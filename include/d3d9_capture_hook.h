#pragma once
#include <cstdint>
#include <vector>

using VRFrameTickFn = void(*)();

// This GOG release's DDRAW.dll is GOG's own "Direct1-7 wrapper": it exposes
// the classic DirectDraw7 interfaces MGS1 calls, but every frame is actually
// rendered through a real Direct3D 9 device internally (confirmed via
// `dumpbin /imports` on DDRAW.dll -- it imports d3d9.dll!Direct3DCreate9).
// IDirectDrawSurface7::Lock() on the wrapper's surfaces does not reliably
// return real pixel data, which is why the earlier DirectDraw-level capture
// (ddraw_hook.cpp) never worked here.
//
// This hooks the real system d3d9.dll!Direct3DCreate9 directly (it's a
// normal, unwrapped Microsoft DLL, so this is a standard, reliable hook
// point), then chains into IDirect3D9::CreateDevice and finally
// IDirect3DDevice9::Present on whatever objects the wrapper creates through
// that call -- giving us the actual D3D9 device driving rendering, which the
// wrapper never exposes directly.
bool InstallD3D9CaptureHook(VRFrameTickFn onFrameTick);

// Copies the most recently presented frame into outRgba as tightly-packed,
// top-down RGBA8 pixels (outWidth * outHeight * 4 bytes). Captured from the
// real backbuffer via GetRenderTargetData() into a lockable system-memory
// surface, immediately before each real Present() call (backbuffer contents
// are undefined after Present() returns under the common DISCARD swap
// effect, so this is the last safe moment to read it).
//
// Returns false if no frame has been captured yet.
bool GetLatestD3D9FrameRgba(std::vector<uint8_t>& outRgba, int32_t& outWidth, int32_t& outHeight);

// The IDirect3DDevice9* GOG's wrapper created (as void* to keep d3d9.h out of
// this header), or nullptr before CreateDevice. Used by gpu_eye_capture.
void* GetWrapperD3D9Device();

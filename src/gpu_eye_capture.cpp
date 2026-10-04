#include <windows.h>
#include <d3d9.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>
#include "MinHook.h"
#include "../include/gpu_eye_capture.h"
#include "../include/ddraw_hook.h"
#include "../include/d3d9_capture_hook.h"
#include "../include/debug_logging.h"

// See gpu_eye_capture.h. All device calls here happen on the game thread,
// and only once the wrapper's own Direct3D 9 calls have been observed there.

namespace {

using ResetFn     = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
using PresentFn   = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
using SceneFn     = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*);
using DrawPFn     = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT);
using DrawIPFn    = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
using DrawPUPFn   = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, const void*, UINT);
using SetRtFn     = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, DWORD, IDirect3DSurface9*);
using DrawIPUPFn  = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT, UINT, const void*, D3DFORMAT, const void*, UINT);

ResetFn    o_Reset = nullptr;
PresentFn  o_Present = nullptr;
SceneFn    o_Begin = nullptr;
DrawPFn    o_DrawP = nullptr;
DrawIPFn   o_DrawIP = nullptr;
DrawPUPFn  o_DrawPUP = nullptr;
DrawIPUPFn o_DrawIPUP = nullptr;
SetRtFn    o_SetRt = nullptr;

std::atomic<long>  g_nDraw{ 0 }, g_nBegin{ 0 }, g_nPresent{ 0 }, g_nReset{ 0 };
std::atomic<DWORD> g_d3dThread{ 0 };        // thread the wrapper's D3D9 calls come from
std::atomic<unsigned long long> g_lastForeignTick{ 0 };  // last call from any OTHER thread
std::atomic<long>  g_foreignCalls{ 0 };

std::recursive_mutex g_res;                 // resources vs Reset
// The render target the wrapper's most recent DRAW went into (AddRef'd). The
// currently bound target after the game's EndScene may already be the swap
// chain's back buffer again; the one that was actually drawn to is the eye.
IDirect3DSurface9* g_drawnRt = nullptr;
std::atomic<bool>  g_rtDirty{ true };
IDirect3DDevice9*  g_dev = nullptr;
IDirect3DSurface9* g_resolve = nullptr;     // same size as the source, no MSAA
IDirect3DSurface9* g_eyeRt[2] = { nullptr, nullptr };
IDirect3DSurface9* g_sys[2] = { nullptr, nullptr };
bool               g_eyeCopied[2] = { false, false };
UINT g_srcW = 0, g_srcH = 0, g_outW = 0, g_outH = 0;
D3DFORMAT g_srcFmt = D3DFMT_UNKNOWN, g_outFmt = D3DFMT_UNKNOWN;
D3DMULTISAMPLE_TYPE g_srcMs = D3DMULTISAMPLE_NONE;
RECT g_srcRect{};
char g_desc[256] = "not set up";

bool g_hooksTried = false, g_hooksOk = false;
int  g_cfgHeight = 960;
int  g_cfgMaxWidth = 1280;

void NoteDrawTarget(IDirect3DDevice9* d) {
    if (!g_rtDirty.exchange(false)) return;
    IDirect3DSurface9* rt = nullptr;
    if (FAILED(d->GetRenderTarget(0, &rt)) || !rt) return;
    std::lock_guard<std::recursive_mutex> lk(g_res);
    if (g_drawnRt) g_drawnRt->Release();
    g_drawnRt = rt;   // keeps GetRenderTarget's reference
}

// True when the call comes from the wrapper's main (game) thread. Calls from
// any other thread -- seen 2026-09-26 while a PC-port menu was open -- only
// make the GPU path stand aside while they are recent (see Ready()).
bool NoteThread(const char* what) {
    const DWORD me = GetCurrentThreadId();
    DWORD prev = g_d3dThread.load(std::memory_order_relaxed);
    if (prev == 0) { g_d3dThread.compare_exchange_strong(prev, me); return true; }
    if (prev == me) return true;
    g_lastForeignTick.store(GetTickCount64(), std::memory_order_relaxed);
    const long n = ++g_foreignCalls;
    if (n <= 3 || (n & (n - 1)) == 0) {
        DebugLogger::LogFormat("GPU eye capture: %s from thread %lu (the wrapper's main thread is %lu) -- call #%ld from another thread; GPU copies pause while these are recent",
            what, (unsigned long)me, (unsigned long)prev, n);
    }
    return false;
}

HRESULT STDMETHODCALLTYPE H_Reset(IDirect3DDevice9* d, D3DPRESENT_PARAMETERS* pp) {
    g_nReset++;
    GpuEye::ReleaseAll();   // DEFAULT-pool surfaces must be gone or Reset fails
    {
        std::lock_guard<std::recursive_mutex> lk(g_res);
        if (g_drawnRt) { g_drawnRt->Release(); g_drawnRt = nullptr; }   // our reference would block Reset too
        g_rtDirty = true;
    }
    static int logged = 0;
    if (logged < 5) { logged++; DebugLogger::Log("GPU eye capture: device Reset -- our surfaces released, rebuilt on next use"); }
    return o_Reset(d, pp);
}
HRESULT STDMETHODCALLTYPE H_Present(IDirect3DDevice9* d, const RECT* a, const RECT* b, HWND w, const RGNDATA* r) {
    g_nPresent++; NoteThread("Present");
    return o_Present(d, a, b, w, r);
}
HRESULT STDMETHODCALLTYPE H_Begin(IDirect3DDevice9* d) {
    g_nBegin++; NoteThread("BeginScene");
    return o_Begin(d);
}
HRESULT STDMETHODCALLTYPE H_DrawP(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, UINT a, UINT b) {
    g_nDraw++; if (NoteThread("a draw call")) NoteDrawTarget(d);
    return o_DrawP(d, t, a, b);
}
HRESULT STDMETHODCALLTYPE H_DrawIP(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, INT a, UINT b, UINT c, UINT e, UINT f) {
    g_nDraw++; if (NoteThread("a draw call")) NoteDrawTarget(d);
    return o_DrawIP(d, t, a, b, c, e, f);
}
HRESULT STDMETHODCALLTYPE H_DrawPUP(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, UINT a, const void* v, UINT s) {
    g_nDraw++; if (NoteThread("a draw call")) NoteDrawTarget(d);
    return o_DrawPUP(d, t, a, v, s);
}
HRESULT STDMETHODCALLTYPE H_DrawIPUP(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, UINT a, UINT b, UINT c, const void* i, D3DFORMAT f, const void* v, UINT s) {
    g_nDraw++; if (NoteThread("a draw call")) NoteDrawTarget(d);
    return o_DrawIPUP(d, t, a, b, c, i, f, v, s);
}

HRESULT STDMETHODCALLTYPE H_SetRt(IDirect3DDevice9* d, DWORD idx, IDirect3DSurface9* rt) {
    if (idx == 0) g_rtDirty = true;
    return o_SetRt(d, idx, rt);
}

// Code hooks (not vtable patches): the D3D9 runtime swaps a device's vtable
// pointer after start-up, which is why the vtable-patched Present/BeginScene
// in d3d9_capture_hook never fired. The code behind the CURRENT vtable is
// what the wrapper calls, so hook that.
bool HookSlot(void** vtbl, int slot, void* det, void** orig, const char* name) {
    void* target = vtbl[slot];
    MH_STATUS s = MH_CreateHook(target, det, orig);
    if (s == MH_OK) s = MH_EnableHook(target);
    if (s != MH_OK) {
        DebugLogger::LogFormat("GPU eye capture: could not hook %s (slot %d at %p): MinHook %d", name, slot, target, (int)s);
        return false;
    }
    return true;
}

void InstallHooksOnce() {
    if (g_hooksTried || !g_dev) return;
    g_hooksTried = true;
    void** vtbl = *reinterpret_cast<void***>(g_dev);
    // Reset is the safety one: without it a Reset would fail on our surfaces.
    const bool reset = HookSlot(vtbl, 16, (void*)&H_Reset, (void**)&o_Reset, "Reset");
    const bool pres  = HookSlot(vtbl, 17, (void*)&H_Present, (void**)&o_Present, "Present");
    const bool begin = HookSlot(vtbl, 41, (void*)&H_Begin, (void**)&o_Begin, "BeginScene");
    const bool setRt = HookSlot(vtbl, 37, (void*)&H_SetRt, (void**)&o_SetRt, "SetRenderTarget");
    int draws = 0;
    draws += HookSlot(vtbl, 81, (void*)&H_DrawP, (void**)&o_DrawP, "DrawPrimitive") ? 1 : 0;
    draws += HookSlot(vtbl, 82, (void*)&H_DrawIP, (void**)&o_DrawIP, "DrawIndexedPrimitive") ? 1 : 0;
    draws += HookSlot(vtbl, 83, (void*)&H_DrawPUP, (void**)&o_DrawPUP, "DrawPrimitiveUP") ? 1 : 0;
    draws += HookSlot(vtbl, 84, (void*)&H_DrawIPUP, (void**)&o_DrawIPUP, "DrawIndexedPrimitiveUP") ? 1 : 0;
    g_hooksOk = reset && (pres || begin || draws > 0);
    DebugLogger::LogFormat("GPU eye capture: device %p, code hooks Reset=%d Present=%d BeginScene=%d SetRenderTarget=%d draw calls=%d/4 -> %s",
        (void*)g_dev, reset, pres, begin, setRt, draws, g_hooksOk ? "ok" : "NOT SAFE, staying on the flip path");
}

void ReleaseSurface(IDirect3DSurface9*& s) { if (s) { s->Release(); s = nullptr; } }

bool Is32(D3DFORMAT f) { return f == D3DFMT_X8R8G8B8 || f == D3DFMT_A8R8G8B8; }

// (Re)creates our surfaces for a source render target of this shape.
bool EnsureResources(const D3DSURFACE_DESC& d) {
    if (g_eyeRt[0] && d.Width == g_srcW && d.Height == g_srcH && d.Format == g_srcFmt && d.MultiSampleType == g_srcMs)
        return true;
    GpuEye::ReleaseAll();
    g_srcW = d.Width; g_srcH = d.Height; g_srcFmt = d.Format; g_srcMs = d.MultiSampleType;

    // Source crop: a 16:9 target with the 4:3 picture pillar-boxed in it
    // (the wrapper's scaling=fit) -> take the centre 4:3.
    UINT cw = g_srcW, ch = g_srcH;
    if ((double)cw / (double)ch > 1.40) cw = (UINT)((double)ch * 4.0 / 3.0 + 0.5);
    g_srcRect.left = (LONG)((g_srcW - cw) / 2); g_srcRect.top = (LONG)((g_srcH - ch) / 2);
    g_srcRect.right = g_srcRect.left + (LONG)cw; g_srcRect.bottom = g_srcRect.top + (LONG)ch;

    g_outH = ch < (UINT)g_cfgHeight ? ch : (UINT)g_cfgHeight;
    g_outW = (UINT)((double)g_outH * (double)cw / (double)ch + 0.5) & ~1u;
    if (g_outW > (UINT)g_cfgMaxWidth) {
        g_outW = (UINT)g_cfgMaxWidth & ~1u;
        g_outH = (UINT)((double)g_outW * (double)ch / (double)cw + 0.5);
    }

    // Output as 32-bit when the driver can convert on the GPU; otherwise keep
    // the source format and convert on the CPU at read-back.
    g_outFmt = g_srcFmt;
    if (!Is32(g_srcFmt)) {
        IDirect3D9* d3d = nullptr;
        if (SUCCEEDED(g_dev->GetDirect3D(&d3d)) && d3d) {
            D3DDEVICE_CREATION_PARAMETERS cp{};
            g_dev->GetCreationParameters(&cp);
            if (SUCCEEDED(d3d->CheckDeviceFormatConversion(cp.AdapterOrdinal, cp.DeviceType, g_srcFmt, D3DFMT_X8R8G8B8)))
                g_outFmt = D3DFMT_X8R8G8B8;
            d3d->Release();
        }
    }
    if (!Is32(g_outFmt) && g_outFmt != D3DFMT_R5G6B5) {
        sprintf_s(g_desc, "source %ux%u fmt %d: unsupported format", g_srcW, g_srcH, (int)g_srcFmt);
        return false;
    }

    HRESULT hr = S_OK;
    if (g_srcMs != D3DMULTISAMPLE_NONE) {
        hr = g_dev->CreateRenderTarget(g_srcW, g_srcH, g_srcFmt, D3DMULTISAMPLE_NONE, 0, FALSE, &g_resolve, nullptr);
        if (FAILED(hr)) { sprintf_s(g_desc, "CreateRenderTarget(resolve %ux%u) failed 0x%08X", g_srcW, g_srcH, (unsigned)hr); GpuEye::ReleaseAll(); return false; }
    }
    for (int e = 0; e < 2; ++e) {
        hr = g_dev->CreateRenderTarget(g_outW, g_outH, g_outFmt, D3DMULTISAMPLE_NONE, 0, FALSE, &g_eyeRt[e], nullptr);
        if (SUCCEEDED(hr)) hr = g_dev->CreateOffscreenPlainSurface(g_outW, g_outH, g_outFmt, D3DPOOL_SYSTEMMEM, &g_sys[e], nullptr);
        if (FAILED(hr)) { sprintf_s(g_desc, "creating %ux%u eye surfaces failed 0x%08X", g_outW, g_outH, (unsigned)hr); GpuEye::ReleaseAll(); return false; }
    }
    // ReleaseAll zeroed the shape; put it back now that everything exists.
    g_srcW = d.Width; g_srcH = d.Height; g_srcFmt = d.Format; g_srcMs = d.MultiSampleType;
    sprintf_s(g_desc, "source RT %ux%u fmt %d MSAA %d usage 0x%X -> crop %ldx%ld at %ld,%ld -> %ux%u fmt %d%s",
        g_srcW, g_srcH, (int)g_srcFmt, (int)g_srcMs, (unsigned)d.Usage,
        g_srcRect.right - g_srcRect.left, g_srcRect.bottom - g_srcRect.top, g_srcRect.left, g_srcRect.top,
        g_outW, g_outH, (int)g_outFmt, g_srcMs != D3DMULTISAMPLE_NONE ? " (MSAA resolved first)" : "");
    DebugLogger::LogFormat("GPU eye capture: %s", g_desc);
    return true;
}

}  // namespace

namespace GpuEye {

void LoadConfig(const char* ini) {
    g_cfgHeight = GetPrivateProfileIntA("render_twice", "gpu_capture_height", 960, ini);
    if (g_cfgHeight < 240) g_cfgHeight = 240;
    if (g_cfgHeight > 1440) g_cfgHeight = 1440;
    g_cfgMaxWidth = (g_cfgHeight * 4 + 2) / 3;
}

int Ready(const char** why) {
    const char* dummy = nullptr;
    if (!why) why = &dummy;
    if (!g_dev) g_dev = reinterpret_cast<IDirect3DDevice9*>(GetWrapperD3D9Device());
    if (!g_dev) { *why = "no Direct3D 9 device seen yet"; return 0; }
    InstallHooksOnce();
    if (!g_hooksOk) { *why = "could not hook the device safely"; return -1; }
    const DWORD t = g_d3dThread.load();
    if (t == 0) { *why = "waiting to see the wrapper's Direct3D 9 calls"; return 0; }
    if (t != GetCurrentThreadId()) { *why = "the wrapper draws on a thread other than the game's -- not safe to share its device"; return -1; }
    const unsigned long long f = g_lastForeignTick.load();
    if (f != 0 && GetTickCount64() - f < 1000) { *why = "another thread used the device in the last second (a PC menu?) -- flip path meanwhile"; return 0; }
    *why = "ready";
    return 1;
}

long DrawCallCount() { return g_nDraw.load(std::memory_order_relaxed); }

bool CopyEye(int eye) {
    if (eye < 0 || eye > 1 || !g_dev) return false;
    std::lock_guard<std::recursive_mutex> lk(g_res);
    g_eyeCopied[eye] = false;
    IDirect3DSurface9* rt = nullptr;
    HRESULT hr = S_OK;
    const bool fromDraw = g_drawnRt != nullptr;
    if (fromDraw) { rt = g_drawnRt; rt->AddRef(); }
    else hr = g_dev->GetRenderTarget(0, &rt);
    static int lastFromDraw = -1;
    if (lastFromDraw != (fromDraw ? 1 : 0)) {
        lastFromDraw = fromDraw ? 1 : 0;
        DebugLogger::LogFormat("GPU eye capture: copying from %s", fromDraw ? "the render target of the wrapper's last draw call"
                                                                           : "the currently bound render target (no draw call seen yet)");
    }
    if (FAILED(hr) || !rt) {
        static int l = 0; if (l < 5) { l++; DebugLogger::LogFormat("GPU eye capture: GetRenderTarget failed 0x%08X", (unsigned)hr); }
        return false;
    }
    D3DSURFACE_DESC d{};
    rt->GetDesc(&d);
    bool ok = EnsureResources(d);
    if (ok) {
        IDirect3DSurface9* src = rt;
        if (g_resolve) {
            hr = g_dev->StretchRect(rt, nullptr, g_resolve, nullptr, D3DTEXF_NONE);
            src = g_resolve;
            ok = SUCCEEDED(hr);
        }
        if (ok) {
            hr = g_dev->StretchRect(src, &g_srcRect, g_eyeRt[eye], nullptr, D3DTEXF_LINEAR);
            ok = SUCCEEDED(hr);
        }
        if (!ok) {
            static int l = 0; if (l < 5) { l++; DebugLogger::LogFormat("GPU eye capture: StretchRect failed 0x%08X (%s)", (unsigned)hr, g_desc); }
        }
    }
    else {
        static int l = 0; if (l < 5) { l++; DebugLogger::LogFormat("GPU eye capture: set-up failed -- %s", g_desc); }
    }
    rt->Release();
    g_eyeCopied[eye] = ok;
    return ok;
}

bool Readback(int eye, RtEyeImage& out, double* ms) {
    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t0);
    std::lock_guard<std::recursive_mutex> lk(g_res);
    if (eye < 0 || eye > 1 || !g_dev || !g_eyeCopied[eye] || !g_eyeRt[eye] || !g_sys[eye]) return false;
    HRESULT hr = g_dev->GetRenderTargetData(g_eyeRt[eye], g_sys[eye]);
    if (FAILED(hr)) {
        static int l = 0; if (l < 5) { l++; DebugLogger::LogFormat("GPU eye capture: GetRenderTargetData failed 0x%08X", (unsigned)hr); }
        return false;
    }
    D3DLOCKED_RECT lr{};
    hr = g_sys[eye]->LockRect(&lr, nullptr, D3DLOCK_READONLY);
    if (FAILED(hr) || !lr.pBits) {
        static int l = 0; if (l < 5) { l++; DebugLogger::LogFormat("GPU eye capture: LockRect failed 0x%08X", (unsigned)hr); }
        return false;
    }
    const size_t n = (size_t)g_outW * g_outH;
    if (out.rgba.size() != n * 4u) out.rgba.resize(n * 4u);
    out.raw16.clear();
    const uint8_t* src = static_cast<const uint8_t*>(lr.pBits);
    if (Is32(g_outFmt)) {
        for (UINT y = 0; y < g_outH; ++y)
            memcpy(out.rgba.data() + (size_t)y * g_outW * 4u, src + (size_t)y * (size_t)lr.Pitch, (size_t)g_outW * 4u);
    }
    else {   // R5G6B5 -> BGRA
        for (UINT y = 0; y < g_outH; ++y) {
            const uint16_t* s = reinterpret_cast<const uint16_t*>(src + (size_t)y * (size_t)lr.Pitch);
            uint8_t* o = out.rgba.data() + (size_t)y * g_outW * 4u;
            for (UINT x = 0; x < g_outW; ++x, o += 4) {
                const uint16_t v = s[x];
                const uint8_t r = (uint8_t)((v >> 11) & 31), g = (uint8_t)((v >> 5) & 63), b = (uint8_t)(v & 31);
                o[0] = (uint8_t)((b << 3) | (b >> 2)); o[1] = (uint8_t)((g << 2) | (g >> 4));
                o[2] = (uint8_t)((r << 3) | (r >> 2)); o[3] = 255;
            }
        }
    }
    g_sys[eye]->UnlockRect();
    out.width = (int32_t)g_outW;
    out.height = (int32_t)g_outH;
    out.bgra = true;
    QueryPerformanceCounter(&t1);
    if (ms) *ms = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart;
    return true;
}

void ReleaseAll() {
    std::lock_guard<std::recursive_mutex> lk(g_res);
    ReleaseSurface(g_resolve);
    for (int e = 0; e < 2; ++e) { ReleaseSurface(g_eyeRt[e]); ReleaseSurface(g_sys[e]); g_eyeCopied[e] = false; }
    g_srcW = g_srcH = 0; g_srcFmt = D3DFMT_UNKNOWN; g_srcMs = D3DMULTISAMPLE_NONE;
}

const char* Describe() { return g_desc; }

}  // namespace GpuEye

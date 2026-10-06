#include <windows.h>
#define INITGUID // so GUID_SysKeyboard (used below) is actually defined, not just declared
#include <dinput.h>
#include <xinput.h>
#include <string>
#include <cstring>
#include <cstdio>
#include <cstdint>
#include <cstdarg>
#include <cstddef>
#include "../include/debug_logging.h"

extern bool GetInjectedXInputState(XINPUT_STATE* outState);
// Bit N = the game's button N is pressed, using MGS1's own 0-indexed
// numbering from its controller config screen. Populated by vr_input.cpp from
// the [controls] ini section. Returns false when there is no virtual pad, in
// which case the legacy XInput-derived mapping below applies unchanged.
extern bool GetInjectedGameButtons(uint32_t* outMask);

#pragma comment(lib, "xinput9_1_0.lib")

// Ensure undecorated exports for x86 import resolution.
#ifdef _M_IX86
#pragma comment(linker, "/EXPORT:DirectInputCreateA=_DirectInputCreateA@16")
#pragma comment(linker, "/EXPORT:DirectInputCreateEx=_DirectInputCreateEx@20")
#else
#pragma comment(linker, "/EXPORT:DirectInputCreateA")
#pragma comment(linker, "/EXPORT:DirectInputCreateEx")
#endif

// Function signatures for the real dinput.dll exports used by mgsi.exe
typedef HRESULT(WINAPI* DirectInputCreateA_t)(HINSTANCE, DWORD, LPVOID*, LPUNKNOWN);
typedef HRESULT(WINAPI* DirectInputCreateEx_t)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);

DirectInputCreateA_t Original_DirectInputCreateA = nullptr;
DirectInputCreateEx_t Original_DirectInputCreateEx = nullptr;
HMODULE g_RealDInputModule = nullptr;

using DI_CreateDevice_t = HRESULT(STDMETHODCALLTYPE*)(LPVOID, REFGUID, LPDIRECTINPUTDEVICEA*, LPUNKNOWN);
using DI_CreateDeviceEx_t = HRESULT(STDMETHODCALLTYPE*)(LPVOID, REFGUID, REFIID, LPVOID*, LPUNKNOWN);
using DIDevice_GetDeviceState_t = HRESULT(STDMETHODCALLTYPE*)(LPVOID, DWORD, LPVOID);

static DI_CreateDevice_t g_OriginalCreateDevice = nullptr;
static DI_CreateDeviceEx_t g_OriginalCreateDeviceEx = nullptr;
static DIDevice_GetDeviceState_t g_OriginalGetDeviceState = nullptr;
static DIDevice_GetDeviceState_t g_OriginalKeyboardGetDeviceState = nullptr;
static bool g_CreateDeviceHooked = false;
static bool g_CreateDeviceExHooked = false;
static bool g_GetDeviceStateHooked = false;
static bool g_KeyboardGetDeviceStateHooked = false;

// Synthetic keyboard key injection, for games that read keyboard state via
// DirectInput's buffered GetDeviceState() rather than the normal Win32
// message queue -- SendInput()-based synthetic key events don't reach this
// path at all on some titles, since DirectInput reads straight from the
// keyboard driver's device buffer rather than the message queue SendInput
// primarily targets. This lets other code (e.g. the FPV auto-double-tap)
// force a specific DIK_* key to appear pressed for exactly one or more
// GetDeviceState() calls, guaranteed to be seen by whatever's actually
// reading it, since it's injected into the game's own read path directly.
static CRITICAL_SECTION g_injectedKeyLock;
static bool g_injectedKeyLockInitialized = false;
static BYTE g_injectedKeyCode = 0; // 0 = none pending
static bool g_injectedKeyDown = false;

static void EnsureInjectedKeyLockInit() {
    if (!g_injectedKeyLockInitialized) {
        InitializeCriticalSection(&g_injectedKeyLock);
        g_injectedKeyLockInitialized = true;
    }
}

// Called by other code (vr_input.cpp) to request that subsequent keyboard
// GetDeviceState() calls report the given DIK_* code as pressed (or
// released). Not queued/buffered -- just sets the state for subsequent
// reads until explicitly changed, so callers should call this to press,
// wait however long they want the "key" held, then call again to release.
void InjectKeyboardKeyState(BYTE dikCode, bool isDown) {
    EnsureInjectedKeyLockInit();
    EnterCriticalSection(&g_injectedKeyLock);
    g_injectedKeyCode = dikCode;
    g_injectedKeyDown = isDown;
    LeaveCriticalSection(&g_injectedKeyLock);
}

static bool PatchComVTable(void* comObj, int methodIndex, void* hookFn, void** originalFn) {
    if (!comObj) {
        return false;
    }

    void*** pObj = reinterpret_cast<void***>(comObj);
    void** vtbl = *pObj;
    if (!vtbl) {
        return false;
    }

    DWORD oldProtect = 0;
    if (!VirtualProtect(&vtbl[methodIndex], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) {
        return false;
    }

    if (originalFn && *originalFn == nullptr) {
        *originalFn = vtbl[methodIndex];
    }

    vtbl[methodIndex] = hookFn;

    DWORD ignore = 0;
    VirtualProtect(&vtbl[methodIndex], sizeof(void*), oldProtect, &ignore);
    return true;
}

static LONG NormalizeAxisToMgsRange(SHORT v) {
    float n = (v >= 0) ? ((float)v / 32767.0f) : ((float)v / 32768.0f);
    if (n > -0.22f && n < 0.22f) {
        return 0;
    }
    return (LONG)(n * 1000.0f);
}

static DWORD BuildPovFromDpad(const XINPUT_STATE& state) {
    bool up = (state.Gamepad.wButtons & XINPUT_GAMEPAD_DPAD_UP) != 0;
    bool down = (state.Gamepad.wButtons & XINPUT_GAMEPAD_DPAD_DOWN) != 0;
    bool left = (state.Gamepad.wButtons & XINPUT_GAMEPAD_DPAD_LEFT) != 0;
    bool right = (state.Gamepad.wButtons & XINPUT_GAMEPAD_DPAD_RIGHT) != 0;

    if (up && right) return 4500;
    if (right && down) return 13500;
    if (down && left) return 22500;
    if (left && up) return 31500;
    if (up) return 0;
    if (right) return 9000;
    if (down) return 18000;
    if (left) return 27000;
    return 0xFFFFFFFF;
}

// ===========================================================================
// DIRECTINPUT PROBE (2026-10-05)
//
// WHY: a player on a Quest 3 with no gamepad plugged in found that Action,
// Weapon, Crawl and First person did nothing, while movement (arrow keys),
// Codec/Pause (Tab/Esc) and everything the mod does itself worked. The same
// bug was reproduced here by unplugging the wired pad: MGS1 only opens and
// polls a DirectInput joystick when Windows has one, and the mod's pad
// buttons are written into that joystick's GetDeviceState() buffer. No pad,
// no joystick read, no buttons.
//
// The fix is a virtual joystick of our own that the game finds and opens as
// if it were a real pad. To build it, every question the game asks a real
// joystick has to be answered the same way -- so this probe wraps the real
// device and logs every DirectInput call the game makes: which interface
// version, how it enumerates, the data format it sets, the axis ranges and
// dead zones it asks for, capability/info queries, acquire and poll pattern.
//
// Pure pass-through: every hook calls the real function and only logs. All
// lines start with [DIPROBE] (grep for it) and every kind of line is capped,
// so a session adds a few hundred lines at most, mostly at startup.
//
// One deliberate behaviour change, a safety one: DirectInput uses ONE device
// vtable for keyboards, mice and joysticks, so HookedGetDeviceState also sees
// keyboard and mouse reads. It now leaves any device created as
// GUID_SysKeyboard or GUID_SysMouse untouched instead of writing pad state
// into a 256-byte keyboard buffer. Joystick behaviour is unchanged.
// ===========================================================================

enum ProbeDevKind { KIND_UNKNOWN = 0, KIND_KEYBOARD, KIND_MOUSE, KIND_GAMECTRL, KIND_VIRTUAL };

static const char* ProbeKindName(int k) {
    switch (k) {
    case KIND_KEYBOARD: return "keyboard";
    case KIND_MOUSE:    return "mouse";
    case KIND_GAMECTRL: return "game controller";
    case KIND_VIRTUAL:  return "virtual pad";
    default:            return "unregistered";
    }
}

struct ProbeDev {
    void* self;
    int id;
    int kind;
    GUID guid;
    DWORD polls;
    DWORD firstPollTick;
    DWORD lastCb;
    bool haveRaw;
    uint32_t rawButtons;
    DWORD rawPov;
    LONG rawAxes[6];
    DWORD lastAxisLogTick;
};

static constexpr int kMaxProbeDevs = 16;
static ProbeDev g_probeDevs[kMaxProbeDevs];
static int g_probeDevCount = 0;
static int g_probeNextId = 1;
static SRWLOCK g_probeLock = SRWLOCK_INIT;

// One entry per COM vtable we have patched (device vtables and IDirectInput
// vtables alike), holding the real function for each slot we replaced.
struct ProbeVtbl {
    void** vtbl;
    void* orig[32];
    bool v2;            // Poll (slot 25) exists and is hooked
    bool deviceHooks;   // device slots hooked
    bool ifaceHooks;    // IDirectInput slots hooked
};
static constexpr int kMaxProbeVtbls = 12;
static ProbeVtbl g_probeVtbls[kMaxProbeVtbls];
static int g_probeVtblCount = 0;

// Per-kind log caps.
static volatile LONG g_pcQI = 0, g_pcCaps = 0, g_pcEnumObj = 0, g_pcGetProp = 0, g_pcSetProp = 0,
    g_pcAcquire = 0, g_pcAcquireFail = 0, g_pcUnacquire = 0, g_pcGetData = 0, g_pcSetFormat = 0,
    g_pcSetEvent = 0, g_pcCoop = 0, g_pcObjInfo = 0, g_pcDevInfo = 0, g_pcPoll = 0, g_pcPollFail = 0,
    g_pcEnumDev = 0, g_pcDevStatus = 0, g_pcRaw = 0, g_pcFirstPoll = 0, g_pcStateFail = 0;

static bool ProbeUnderCap(volatile LONG* counter, LONG cap) {
    return InterlockedIncrement(counter) <= cap;
}

static void ProbeGuidName(const GUID* g, char* out, size_t cap) {
    if (!g) { strcpy_s(out, cap, "(null)"); return; }
    struct Named { const GUID* g; const char* n; };
    static const Named kNames[] = {
        { &GUID_SysKeyboard, "GUID_SysKeyboard" }, { &GUID_SysMouse, "GUID_SysMouse" },
        { &GUID_Joystick, "GUID_Joystick" },
        { &GUID_XAxis, "X" }, { &GUID_YAxis, "Y" }, { &GUID_ZAxis, "Z" },
        { &GUID_RxAxis, "Rx" }, { &GUID_RyAxis, "Ry" }, { &GUID_RzAxis, "Rz" },
        { &GUID_Slider, "Slider" }, { &GUID_Button, "Button" }, { &GUID_Key, "Key" },
        { &GUID_POV, "POV" }, { &GUID_Unknown, "Unknown" },
        { &IID_IDirectInputA, "IID_IDirectInputA" }, { &IID_IDirectInputW, "IID_IDirectInputW" },
        { &IID_IDirectInput2A, "IID_IDirectInput2A" }, { &IID_IDirectInput2W, "IID_IDirectInput2W" },
        { &IID_IDirectInput7A, "IID_IDirectInput7A" }, { &IID_IDirectInput7W, "IID_IDirectInput7W" },
        { &IID_IDirectInputDeviceA, "IID_IDirectInputDeviceA" }, { &IID_IDirectInputDeviceW, "IID_IDirectInputDeviceW" },
        { &IID_IDirectInputDevice2A, "IID_IDirectInputDevice2A" }, { &IID_IDirectInputDevice2W, "IID_IDirectInputDevice2W" },
        { &IID_IDirectInputDevice7A, "IID_IDirectInputDevice7A" }, { &IID_IDirectInputDevice7W, "IID_IDirectInputDevice7W" },
    };
    for (const auto& n : kNames) {
        if (IsEqualGUID(*g, *n.g)) { strcpy_s(out, cap, n.n); return; }
    }
    sprintf_s(out, cap, "{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
        (unsigned long)g->Data1, g->Data2, g->Data3, g->Data4[0], g->Data4[1], g->Data4[2],
        g->Data4[3], g->Data4[4], g->Data4[5], g->Data4[6], g->Data4[7]);
}

static bool ProbeIsDevice2Plus(const IID* riid) {
    return riid && (IsEqualGUID(*riid, IID_IDirectInputDevice2A) || IsEqualGUID(*riid, IID_IDirectInputDevice2W) ||
                    IsEqualGUID(*riid, IID_IDirectInputDevice7A) || IsEqualGUID(*riid, IID_IDirectInputDevice7W));
}

// DirectInput 7 device-type codes, spelled out here because the Windows SDK's
// dinput.h only defines DIDEVTYPE_* when DIRECTINPUT_VERSION <= 0x0700, and
// this file builds with the SDK default (0x0800). MGS1 itself is a DirectInput
// 7 game, so these are the values its calls actually carry.
static constexpr DWORD kDi7DevTypeDevice   = 1;
static constexpr DWORD kDi7DevTypeMouse    = 2;
static constexpr DWORD kDi7DevTypeKeyboard = 3;
static constexpr DWORD kDi7DevTypeJoystick = 4;
static constexpr DWORD kDi7DevTypeHid      = 0x00010000;

static const char* ProbeDevTypeName(DWORD devType) {
    switch (devType & 0xFF) {
    case kDi7DevTypeDevice:   return "device";
    case kDi7DevTypeMouse:    return "mouse";
    case kDi7DevTypeKeyboard: return "keyboard";
    case kDi7DevTypeJoystick: return "joystick";
    default:                  return "other";
    }
}

static ProbeVtbl* ProbeFindVtblLocked(void** vtbl) {
    for (int i = 0; i < g_probeVtblCount; ++i) {
        if (g_probeVtbls[i].vtbl == vtbl) return &g_probeVtbls[i];
    }
    return nullptr;
}

// The real function behind a slot we patched, for the object's own vtable.
static void* ProbeOriginal(void* self, int slot) {
    if (!self || slot < 0 || slot >= 32) return nullptr;
    void** vtbl = *reinterpret_cast<void***>(self);
    void* fn = nullptr;
    AcquireSRWLockShared(&g_probeLock);
    if (ProbeVtbl* pv = ProbeFindVtblLocked(vtbl)) fn = pv->orig[slot];
    ReleaseSRWLockShared(&g_probeLock);
    return fn;
}

static ProbeDev* ProbeFindDevLocked(void* self) {
    for (int i = 0; i < g_probeDevCount; ++i) {
        if (g_probeDevs[i].self == self) return &g_probeDevs[i];
    }
    return nullptr;
}

static ProbeDev* ProbeAddDevLocked(void* self, int kind, const GUID* guid) {
    ProbeDev* d = ProbeFindDevLocked(self);
    if (d) {
        if (d->kind == KIND_UNKNOWN) d->kind = kind;
        return d;
    }
    if (g_probeDevCount >= kMaxProbeDevs) return nullptr;
    d = &g_probeDevs[g_probeDevCount++];
    std::memset(d, 0, sizeof(*d));
    d->self = self;
    d->id = g_probeNextId++;
    d->kind = kind;
    if (guid) d->guid = *guid;
    return d;
}

static int ProbeKindOf(void* self) {
    int kind = KIND_UNKNOWN;
    AcquireSRWLockShared(&g_probeLock);
    if (ProbeDev* d = ProbeFindDevLocked(self)) kind = d->kind;
    ReleaseSRWLockShared(&g_probeLock);
    return kind;
}

static void ProbeTag(void* self, char* out, size_t cap) {
    int id = 0, kind = KIND_UNKNOWN;
    AcquireSRWLockShared(&g_probeLock);
    if (ProbeDev* d = ProbeFindDevLocked(self)) { id = d->id; kind = d->kind; }
    ReleaseSRWLockShared(&g_probeLock);
    if (id) sprintf_s(out, cap, "dev#%d %s", id, ProbeKindName(kind));
    else sprintf_s(out, cap, "dev? %p", self);
}

static bool ProbePatchSlotLocked(ProbeVtbl* pv, int slot, void* hook) {
    void** entry = &pv->vtbl[slot];
    if (*entry == hook) return true;
    DWORD oldProtect = 0;
    if (!VirtualProtect(entry, sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) return false;
    if (!pv->orig[slot]) pv->orig[slot] = *entry;
    *entry = hook;
    DWORD ignore = 0;
    VirtualProtect(entry, sizeof(void*), oldProtect, &ignore);
    return true;
}

static ProbeVtbl* ProbeVtblEntryLocked(void* obj) {
    void** vtbl = *reinterpret_cast<void***>(obj);
    if (!vtbl) return nullptr;
    ProbeVtbl* pv = ProbeFindVtblLocked(vtbl);
    if (pv) return pv;
    if (g_probeVtblCount >= kMaxProbeVtbls) return nullptr;
    pv = &g_probeVtbls[g_probeVtblCount++];
    std::memset(pv, 0, sizeof(*pv));
    pv->vtbl = vtbl;
    return pv;
}

// ---- forward declarations (defined further down) ---------------------------
static HRESULT STDMETHODCALLTYPE HookedGetDeviceState(LPVOID self, DWORD cbData, LPVOID lpvData);
static HRESULT STDMETHODCALLTYPE HookedKeyboardGetDeviceState(LPVOID self, DWORD cbData, LPVOID lpvData);
static void ProbeHookDeviceVtbl(void* dev, bool v2, int kind);
static int VirtualPadMode();
static bool VirtualPadIsGuid(const GUID* g);
static BOOL VirtualPadOffer(LPDIENUMDEVICESCALLBACKA cb, LPVOID ref, LONG call);

// ---- device method hooks ----------------------------------------------------
using PrQI_t        = HRESULT(STDMETHODCALLTYPE*)(void*, REFIID, void**);
using PrCaps_t      = HRESULT(STDMETHODCALLTYPE*)(void*, LPDIDEVCAPS);
using PrEnumObj_t   = HRESULT(STDMETHODCALLTYPE*)(void*, LPDIENUMDEVICEOBJECTSCALLBACKA, LPVOID, DWORD);
using PrProp_t      = HRESULT(STDMETHODCALLTYPE*)(void*, const void*, LPDIPROPHEADER);
using PrNoArg_t     = HRESULT(STDMETHODCALLTYPE*)(void*);
using PrGetData_t   = HRESULT(STDMETHODCALLTYPE*)(void*, DWORD, LPDIDEVICEOBJECTDATA, LPDWORD, DWORD);
using PrFormat_t    = HRESULT(STDMETHODCALLTYPE*)(void*, LPCDIDATAFORMAT);
using PrEvent_t     = HRESULT(STDMETHODCALLTYPE*)(void*, HANDLE);
using PrCoop_t      = HRESULT(STDMETHODCALLTYPE*)(void*, HWND, DWORD);
using PrObjInfo_t   = HRESULT(STDMETHODCALLTYPE*)(void*, LPDIDEVICEOBJECTINSTANCEA, DWORD, DWORD);
using PrDevInfo_t   = HRESULT(STDMETHODCALLTYPE*)(void*, LPDIDEVICEINSTANCEA);
using PrEnumDev_t   = HRESULT(STDMETHODCALLTYPE*)(void*, DWORD, LPDIENUMDEVICESCALLBACKA, LPVOID, DWORD);
using PrDevStatus_t = HRESULT(STDMETHODCALLTYPE*)(void*, const GUID*);

static HRESULT STDMETHODCALLTYPE ProbeQI(void* self, REFIID riid, void** ppv) {
    auto o = reinterpret_cast<PrQI_t>(ProbeOriginal(self, 0));
    if (!o) return E_NOINTERFACE;
    HRESULT hr = o(self, riid, ppv);
    void* out = (SUCCEEDED(hr) && ppv) ? *ppv : nullptr;
    int kind = ProbeKindOf(self);
    if (out && out != self) {
        GUID g{};
        AcquireSRWLockExclusive(&g_probeLock);
        if (ProbeDev* parent = ProbeFindDevLocked(self)) g = parent->guid;
        ProbeAddDevLocked(out, kind, &g);
        ReleaseSRWLockExclusive(&g_probeLock);
    }
    if (out && ProbeIsDevice2Plus(&riid)) ProbeHookDeviceVtbl(out, true, kind);
    if (ProbeUnderCap(&g_pcQI, 16)) {
        char tag[48], name[64];
        ProbeTag(self, tag, sizeof(tag));
        ProbeGuidName(&riid, name, sizeof(name));
        DebugLogger::LogFormat("[DIPROBE] QueryInterface %s: %s -> hr=0x%08lX %s%p", tag, name,
            (unsigned long)hr, out == self ? "(same object) " : "", out);
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE ProbeGetCapabilities(void* self, LPDIDEVCAPS caps) {
    auto o = reinterpret_cast<PrCaps_t>(ProbeOriginal(self, 3));
    HRESULT hr = o ? o(self, caps) : DIERR_GENERIC;
    if (ProbeUnderCap(&g_pcCaps, 12)) {
        char tag[48];
        ProbeTag(self, tag, sizeof(tag));
        if (SUCCEEDED(hr) && caps) {
            DebugLogger::LogFormat("[DIPROBE] GetCapabilities %s: dwSize=%lu flags=0x%lX devType=0x%08lX (%s) axes=%lu buttons=%lu povs=%lu",
                tag, caps->dwSize, caps->dwFlags, caps->dwDevType, ProbeDevTypeName(caps->dwDevType),
                caps->dwAxes, caps->dwButtons, caps->dwPOVs);
        } else {
            DebugLogger::LogFormat("[DIPROBE] GetCapabilities %s: hr=0x%08lX (dwSize asked %lu)", tag,
                (unsigned long)hr, caps ? caps->dwSize : 0);
        }
    }
    return hr;
}

struct ProbeEnumObjCtx { LPDIENUMDEVICEOBJECTSCALLBACKA cb; LPVOID ref; int n; char tag[48]; };

static BOOL CALLBACK ProbeEnumObjCb(LPCDIDEVICEOBJECTINSTANCEA inst, LPVOID pv) {
    auto* c = static_cast<ProbeEnumObjCtx*>(pv);
    BOOL r = c->cb ? c->cb(inst, c->ref) : DIENUM_STOP;
    if (inst && ++c->n <= 48) {
        char g[64];
        ProbeGuidName(&inst->guidType, g, sizeof(g));
        DebugLogger::LogFormat("[DIPROBE]   object %d of %s: %s ofs=%lu type=0x%08lX flags=0x%lX name='%.40s' -> %s",
            c->n, c->tag, g, inst->dwOfs, inst->dwType, inst->dwFlags, inst->tszName,
            r == DIENUM_CONTINUE ? "continue" : "STOP");
    }
    return r;
}

static HRESULT STDMETHODCALLTYPE ProbeEnumObjects(void* self, LPDIENUMDEVICEOBJECTSCALLBACKA cb, LPVOID ref, DWORD flags) {
    auto o = reinterpret_cast<PrEnumObj_t>(ProbeOriginal(self, 4));
    if (!o) return DIERR_GENERIC;
    if (!ProbeUnderCap(&g_pcEnumObj, 6)) return o(self, cb, ref, flags);
    ProbeEnumObjCtx ctx{ cb, ref, 0, {} };
    ProbeTag(self, ctx.tag, sizeof(ctx.tag));
    DebugLogger::LogFormat("[DIPROBE] EnumObjects %s: flags=0x%08lX", ctx.tag, flags);
    HRESULT hr = o(self, ProbeEnumObjCb, &ctx, flags);
    DebugLogger::LogFormat("[DIPROBE] EnumObjects %s: hr=0x%08lX after %d object(s)", ctx.tag, (unsigned long)hr, ctx.n);
    return hr;
}

static const char* ProbePropName(uintptr_t id) {
    switch (id) {
    case 1: return "BUFFERSIZE"; case 2: return "AXISMODE"; case 3: return "GRANULARITY";
    case 4: return "RANGE"; case 5: return "DEADZONE"; case 6: return "SATURATION";
    case 7: return "FFGAIN"; case 8: return "FFLOAD"; case 9: return "AUTOCENTER";
    case 10: return "CALIBRATIONMODE"; case 11: return "CALIBRATION"; case 12: return "GUIDANDPATH";
    case 13: return "INSTANCENAME"; case 14: return "PRODUCTNAME"; case 15: return "JOYSTICKID";
    case 16: return "GETPORTDISPLAYNAME"; case 18: return "PHYSICALRANGE"; case 19: return "LOGICALRANGE";
    case 20: return "KEYNAME"; case 21: return "CPOINTS"; case 22: return "APPDATA";
    case 23: return "SCANCODE"; case 24: return "VIDPID"; case 25: return "USERNAME"; case 26: return "TYPENAME";
    default: return nullptr;
    }
}

static void ProbeDescribeProp(const void* prop, const DIPROPHEADER* h, bool withValue, char* out, size_t cap) {
    char name[64];
    uintptr_t id = reinterpret_cast<uintptr_t>(prop);
    if (id <= 0xFFFF) {
        const char* n = ProbePropName(id);
        if (n) sprintf_s(name, sizeof(name), "DIPROP_%s", n);
        else sprintf_s(name, sizeof(name), "DIPROP_#%u", (unsigned)id);
    } else {
        ProbeGuidName(static_cast<const GUID*>(prop), name, sizeof(name));
    }
    if (!h) { sprintf_s(out, cap, "%s (no header)", name); return; }
    static const char* kHow[] = { "device", "by offset", "by id", "by usage" };
    const char* how = h->dwHow < 4 ? kHow[h->dwHow] : "?";
    char value[160] = "";
    if (withValue) {
        if ((id == 4 || id == 18 || id == 19) && h->dwSize >= sizeof(DIPROPRANGE)) {
            auto* r = reinterpret_cast<const DIPROPRANGE*>(h);
            sprintf_s(value, sizeof(value), " = %ld..%ld", r->lMin, r->lMax);
        } else if ((id == 13 || id == 14 || id == 20 || id == 25 || id == 26) && h->dwSize >= sizeof(DIPROPSTRING)) {
            auto* s = reinterpret_cast<const DIPROPSTRING*>(h);
            char narrow[96] = "";
            WideCharToMultiByte(CP_UTF8, 0, s->wsz, -1, narrow, sizeof(narrow) - 1, nullptr, nullptr);
            sprintf_s(value, sizeof(value), " = '%s'", narrow);
        } else if (id != 11 && id != 12 && h->dwSize >= sizeof(DIPROPDWORD)) {
            auto* d = reinterpret_cast<const DIPROPDWORD*>(h);
            sprintf_s(value, sizeof(value), " = %lu", d->dwData);
        }
    }
    sprintf_s(out, cap, "%s obj=%lu (%s) size=%lu%s", name, h->dwObj, how, h->dwSize, value);
}

static HRESULT STDMETHODCALLTYPE ProbeGetProperty(void* self, const void* prop, LPDIPROPHEADER h) {
    auto o = reinterpret_cast<PrProp_t>(ProbeOriginal(self, 5));
    HRESULT hr = o ? o(self, prop, h) : DIERR_GENERIC;
    if (ProbeUnderCap(&g_pcGetProp, 32)) {
        char tag[48], desc[320];
        ProbeTag(self, tag, sizeof(tag));
        ProbeDescribeProp(prop, h, SUCCEEDED(hr), desc, sizeof(desc));
        DebugLogger::LogFormat("[DIPROBE] GetProperty %s: %s -> hr=0x%08lX", tag, desc, (unsigned long)hr);
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE ProbeSetProperty(void* self, const void* prop, LPDIPROPHEADER h) {
    auto o = reinterpret_cast<PrProp_t>(ProbeOriginal(self, 6));
    char desc[320] = "";
    bool log = ProbeUnderCap(&g_pcSetProp, 32);
    if (log) ProbeDescribeProp(prop, h, true, desc, sizeof(desc));   // value as the game asked for it
    HRESULT hr = o ? o(self, prop, h) : DIERR_GENERIC;
    if (log) {
        char tag[48];
        ProbeTag(self, tag, sizeof(tag));
        DebugLogger::LogFormat("[DIPROBE] SetProperty %s: %s -> hr=0x%08lX", tag, desc, (unsigned long)hr);
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE ProbeAcquire(void* self) {
    auto o = reinterpret_cast<PrNoArg_t>(ProbeOriginal(self, 7));
    HRESULT hr = o ? o(self) : DIERR_GENERIC;
    if (ProbeUnderCap(&g_pcAcquire, 8) || (FAILED(hr) && ProbeUnderCap(&g_pcAcquireFail, 8))) {
        char tag[48];
        ProbeTag(self, tag, sizeof(tag));
        DebugLogger::LogFormat("[DIPROBE] Acquire %s -> hr=0x%08lX", tag, (unsigned long)hr);
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE ProbeUnacquire(void* self) {
    auto o = reinterpret_cast<PrNoArg_t>(ProbeOriginal(self, 8));
    HRESULT hr = o ? o(self) : DIERR_GENERIC;
    if (ProbeUnderCap(&g_pcUnacquire, 8)) {
        char tag[48];
        ProbeTag(self, tag, sizeof(tag));
        DebugLogger::LogFormat("[DIPROBE] Unacquire %s -> hr=0x%08lX", tag, (unsigned long)hr);
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE ProbeGetDeviceData(void* self, DWORD cbObj, LPDIDEVICEOBJECTDATA data, LPDWORD inOut, DWORD flags) {
    auto o = reinterpret_cast<PrGetData_t>(ProbeOriginal(self, 10));
    DWORD asked = inOut ? *inOut : 0;
    HRESULT hr = o ? o(self, cbObj, data, inOut, flags) : DIERR_GENERIC;
    if (ProbeUnderCap(&g_pcGetData, 6)) {
        char tag[48];
        ProbeTag(self, tag, sizeof(tag));
        DebugLogger::LogFormat("[DIPROBE] GetDeviceData %s: cbObjectData=%lu asked=%lu got=%lu flags=0x%lX -> hr=0x%08lX",
            tag, cbObj, asked, inOut ? *inOut : 0, flags, (unsigned long)hr);
    }
    return hr;
}

static const char* ProbeFormatHint(DWORD dataSize) {
    switch (dataSize) {
    case 16:  return "same size as c_dfDIMouse";
    case 20:  return "same size as c_dfDIMouse2";
    case 80:  return "same size as c_dfDIJoystick";
    case 256: return "same size as c_dfDIKeyboard";
    case 272: return "same size as c_dfDIJoystick2";
    default:  return "custom size";
    }
}

static HRESULT STDMETHODCALLTYPE ProbeSetDataFormat(void* self, LPCDIDATAFORMAT f) {
    auto o = reinterpret_cast<PrFormat_t>(ProbeOriginal(self, 11));
    HRESULT hr = o ? o(self, f) : DIERR_GENERIC;
    LONG n = InterlockedIncrement(&g_pcSetFormat);
    if (n <= 12) {
        char tag[48];
        ProbeTag(self, tag, sizeof(tag));
        if (!f) {
            DebugLogger::LogFormat("[DIPROBE] SetDataFormat %s: (null) -> hr=0x%08lX", tag, (unsigned long)hr);
            return hr;
        }
        DebugLogger::LogFormat("[DIPROBE] SetDataFormat %s: dwSize=%lu objSize=%lu flags=0x%lX (%s) dataSize=%lu (%s) numObjs=%lu -> hr=0x%08lX",
            tag, f->dwSize, f->dwObjSize, f->dwFlags,
            (f->dwFlags & DIDF_RELAXIS) ? "relative axes" : "absolute axes",
            f->dwDataSize, ProbeFormatHint(f->dwDataSize), f->dwNumObjs, (unsigned long)hr);
        // Full object list for the first few formats: offset, guid, type, flags.
        if (n <= 4 && f->rgodf) {
            const DWORD count = f->dwNumObjs < 48 ? f->dwNumObjs : 48;
            std::string line;
            for (DWORD i = 0; i < count; ++i) {
                const DIOBJECTDATAFORMAT& od = f->rgodf[i];
                char g[64], item[112];
                if (od.pguid) ProbeGuidName(od.pguid, g, sizeof(g)); else strcpy_s(g, sizeof(g), "any");
                sprintf_s(item, sizeof(item), "%lu:%s/t%08lX/f%lX  ", od.dwOfs, g, od.dwType, od.dwFlags);
                line += item;
                if ((i % 4) == 3 || i + 1 == count) {
                    DebugLogger::LogFormat("[DIPROBE]   format objs %lu-%lu: %s", i - (i % 4), i, line.c_str());
                    line.clear();
                }
            }
        }
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE ProbeSetEventNotification(void* self, HANDLE ev) {
    auto o = reinterpret_cast<PrEvent_t>(ProbeOriginal(self, 12));
    HRESULT hr = o ? o(self, ev) : DIERR_GENERIC;
    if (ProbeUnderCap(&g_pcSetEvent, 4)) {
        char tag[48];
        ProbeTag(self, tag, sizeof(tag));
        DebugLogger::LogFormat("[DIPROBE] SetEventNotification %s: event=%p -> hr=0x%08lX", tag, ev, (unsigned long)hr);
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE ProbeSetCooperativeLevel(void* self, HWND hwnd, DWORD flags) {
    auto o = reinterpret_cast<PrCoop_t>(ProbeOriginal(self, 13));
    HRESULT hr = o ? o(self, hwnd, flags) : DIERR_GENERIC;
    if (ProbeUnderCap(&g_pcCoop, 12)) {
        char tag[48];
        ProbeTag(self, tag, sizeof(tag));
        DebugLogger::LogFormat("[DIPROBE] SetCooperativeLevel %s: hwnd=%p flags=0x%lX (%s%s%s%s%s) -> hr=0x%08lX",
            tag, hwnd, flags,
            (flags & DISCL_EXCLUSIVE) ? "exclusive " : "", (flags & DISCL_NONEXCLUSIVE) ? "nonexclusive " : "",
            (flags & DISCL_FOREGROUND) ? "foreground " : "", (flags & DISCL_BACKGROUND) ? "background " : "",
            (flags & DISCL_NOWINKEY) ? "nowinkey" : "", (unsigned long)hr);
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE ProbeGetObjectInfo(void* self, LPDIDEVICEOBJECTINSTANCEA info, DWORD obj, DWORD how) {
    auto o = reinterpret_cast<PrObjInfo_t>(ProbeOriginal(self, 14));
    HRESULT hr = o ? o(self, info, obj, how) : DIERR_GENERIC;
    if (ProbeUnderCap(&g_pcObjInfo, 16)) {
        char tag[48], g[64] = "-";
        ProbeTag(self, tag, sizeof(tag));
        if (SUCCEEDED(hr) && info) ProbeGuidName(&info->guidType, g, sizeof(g));
        DebugLogger::LogFormat("[DIPROBE] GetObjectInfo %s: obj=%lu how=%lu -> hr=0x%08lX %s ofs=%lu type=0x%08lX name='%.40s'",
            tag, obj, how, (unsigned long)hr, g,
            (SUCCEEDED(hr) && info) ? info->dwOfs : 0, (SUCCEEDED(hr) && info) ? info->dwType : 0,
            (SUCCEEDED(hr) && info) ? info->tszName : "");
    }
    return hr;
}

static void ProbeLogInstance(const char* prefix, const DIDEVICEINSTANCEA* inst, const char* suffix) {
    char gi[64], gp[64];
    ProbeGuidName(&inst->guidInstance, gi, sizeof(gi));
    ProbeGuidName(&inst->guidProduct, gp, sizeof(gp));
    DebugLogger::LogFormat("[DIPROBE] %s instance=%s product=%s devType=0x%08lX (%s%s) dwSize=%lu name='%.48s' productName='%.48s'%s",
        prefix, gi, gp, inst->dwDevType, ProbeDevTypeName(inst->dwDevType),
        (inst->dwDevType & kDi7DevTypeHid) ? ", HID" : "", inst->dwSize, inst->tszInstanceName, inst->tszProductName, suffix);
}

static HRESULT STDMETHODCALLTYPE ProbeGetDeviceInfo(void* self, LPDIDEVICEINSTANCEA info) {
    auto o = reinterpret_cast<PrDevInfo_t>(ProbeOriginal(self, 15));
    HRESULT hr = o ? o(self, info) : DIERR_GENERIC;
    if (ProbeUnderCap(&g_pcDevInfo, 8)) {
        char tag[48], prefix[96];
        ProbeTag(self, tag, sizeof(tag));
        sprintf_s(prefix, sizeof(prefix), "GetDeviceInfo %s:", tag);
        if (SUCCEEDED(hr) && info) ProbeLogInstance(prefix, info, "");
        else DebugLogger::LogFormat("[DIPROBE] %s hr=0x%08lX", prefix, (unsigned long)hr);
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE ProbePoll(void* self) {
    auto o = reinterpret_cast<PrNoArg_t>(ProbeOriginal(self, 25));
    HRESULT hr = o ? o(self) : DIERR_GENERIC;
    if (ProbeUnderCap(&g_pcPoll, 4) || (FAILED(hr) && ProbeUnderCap(&g_pcPollFail, 8))) {
        char tag[48];
        ProbeTag(self, tag, sizeof(tag));
        DebugLogger::LogFormat("[DIPROBE] Poll %s -> hr=0x%08lX", tag, (unsigned long)hr);
    }
    return hr;
}

// ---- IDirectInput hooks -----------------------------------------------------
struct ProbeEnumDevCtx { LPDIENUMDEVICESCALLBACKA cb; LPVOID ref; int n; int joysticks; LONG call; bool log; bool stopped; };

static BOOL CALLBACK ProbeEnumDevCb(LPCDIDEVICEINSTANCEA inst, LPVOID pv) {
    auto* c = static_cast<ProbeEnumDevCtx*>(pv);
    BOOL r = c->cb ? c->cb(inst, c->ref) : DIENUM_STOP;
    ++c->n;
    if (inst && (inst->dwDevType & 0xFF) == kDi7DevTypeJoystick) ++c->joysticks;
    if (r != DIENUM_CONTINUE) c->stopped = true;
    if (inst && c->log) {
        char prefix[64];
        sprintf_s(prefix, sizeof(prefix), "  EnumDevices call %ld, device %d:", c->call, c->n);
        ProbeLogInstance(prefix, inst, r == DIENUM_CONTINUE ? " -> game says continue" : " -> game says STOP");
    }
    return r;
}

// Also where the VIRTUAL JOYSTICK enters the game (see the VirtualPad section
// further down): MGS1 enumerates attached joysticks once at startup, opens
// the first one it is shown and stops. virtual_joystick=1 shows it the
// virtual pad only when Windows reported no joystick at all; =2 shows the
// virtual pad first, ahead of any real one.
static HRESULT STDMETHODCALLTYPE ProbeEnumDevices(void* self, DWORD devType, LPDIENUMDEVICESCALLBACKA cb, LPVOID ref, DWORD flags) {
    auto o = reinterpret_cast<PrEnumDev_t>(ProbeOriginal(self, 4));
    if (!o) return DIERR_GENERIC;
    const LONG call = InterlockedIncrement(&g_pcEnumDev);
    const bool log = call <= 8;
    const int mode = VirtualPadMode();
    const bool wantsJoysticks = cb && (devType == 0 || (devType & 0xFF) == kDi7DevTypeJoystick) &&
                                !(flags & DIEDFL_FORCEFEEDBACK);
    if (log) {
        DebugLogger::LogFormat("[DIPROBE] EnumDevices call %ld: devType=%lu (%s) flags=0x%lX%s", call, devType,
            devType == 0 ? "all" : ProbeDevTypeName(devType), flags,
            (flags & DIEDFL_ATTACHEDONLY) ? " (attached only)" : "");
    }

    if (wantsJoysticks && mode == 2) {
        if (VirtualPadOffer(cb, ref, call) != DIENUM_CONTINUE) {
            if (log) DebugLogger::LogFormat("[DIPROBE] EnumDevices call %ld: the game took the virtual pad -- real devices not offered (virtual_joystick=2)", call);
            return DI_OK;
        }
    }

    ProbeEnumDevCtx ctx{ cb, ref, 0, 0, call, log, false };
    HRESULT hr = o(self, devType, ProbeEnumDevCb, &ctx, flags);
    const bool offerVirtual = SUCCEEDED(hr) && wantsJoysticks && mode == 1 && ctx.joysticks == 0 && !ctx.stopped;
    if (log) {
        DebugLogger::LogFormat("[DIPROBE] EnumDevices call %ld: hr=0x%08lX, %d device(s) reported%s", call,
            (unsigned long)hr, ctx.n,
            offerVirtual ? " -- no real joystick: offering the virtual pad" :
            (wantsJoysticks && ctx.joysticks == 0 && mode == 0) ? " -- NONE, and virtual_joystick=0: the game will have no joystick" : "");
    }
    if (offerVirtual) VirtualPadOffer(cb, ref, call);
    return hr;
}

static HRESULT STDMETHODCALLTYPE ProbeGetDeviceStatus(void* self, const GUID* g) {
    auto o = reinterpret_cast<PrDevStatus_t>(ProbeOriginal(self, 5));
    HRESULT hr;
    if (VirtualPadMode() != 0 && VirtualPadIsGuid(g)) hr = DI_OK;   // the virtual pad is always attached
    else hr = o ? o(self, g) : DIERR_GENERIC;
    if (ProbeUnderCap(&g_pcDevStatus, 12)) {
        char name[64];
        ProbeGuidName(g, name, sizeof(name));
        DebugLogger::LogFormat("[DIPROBE] GetDeviceStatus %s -> hr=0x%08lX (%s)", name, (unsigned long)hr,
            hr == DI_OK ? "attached" : hr == DI_NOTATTACHED ? "NOT attached" : "error");
    }
    return hr;
}

// ---- installers ---------------------------------------------------------------
static void ProbeHookInterfaceVtbl(void* di) {
    if (!di) return;
    bool fresh = false;
    void** vtbl = nullptr;
    AcquireSRWLockExclusive(&g_probeLock);
    if (ProbeVtbl* pv = ProbeVtblEntryLocked(di)) {
        vtbl = pv->vtbl;
        if (!pv->ifaceHooks) {
            ProbePatchSlotLocked(pv, 4, reinterpret_cast<void*>(&ProbeEnumDevices));
            ProbePatchSlotLocked(pv, 5, reinterpret_cast<void*>(&ProbeGetDeviceStatus));
            pv->ifaceHooks = true;
            fresh = true;
        }
    }
    ReleaseSRWLockExclusive(&g_probeLock);
    if (fresh) DebugLogger::LogFormat("[DIPROBE] IDirectInput vtable %p: EnumDevices + GetDeviceStatus wrapped", vtbl);
}

static void ProbeHookDeviceVtbl(void* dev, bool v2, int kind) {
    if (!dev) return;
    bool fresh = false, addedPoll = false, addedState = false;
    void** vtbl = nullptr;
    AcquireSRWLockExclusive(&g_probeLock);
    if (ProbeVtbl* pv = ProbeVtblEntryLocked(dev)) {
        vtbl = pv->vtbl;
        if (!pv->deviceHooks) {
            ProbePatchSlotLocked(pv, 0, reinterpret_cast<void*>(&ProbeQI));
            ProbePatchSlotLocked(pv, 3, reinterpret_cast<void*>(&ProbeGetCapabilities));
            ProbePatchSlotLocked(pv, 4, reinterpret_cast<void*>(&ProbeEnumObjects));
            ProbePatchSlotLocked(pv, 5, reinterpret_cast<void*>(&ProbeGetProperty));
            ProbePatchSlotLocked(pv, 6, reinterpret_cast<void*>(&ProbeSetProperty));
            ProbePatchSlotLocked(pv, 7, reinterpret_cast<void*>(&ProbeAcquire));
            ProbePatchSlotLocked(pv, 8, reinterpret_cast<void*>(&ProbeUnacquire));
            ProbePatchSlotLocked(pv, 10, reinterpret_cast<void*>(&ProbeGetDeviceData));
            ProbePatchSlotLocked(pv, 11, reinterpret_cast<void*>(&ProbeSetDataFormat));
            ProbePatchSlotLocked(pv, 12, reinterpret_cast<void*>(&ProbeSetEventNotification));
            ProbePatchSlotLocked(pv, 13, reinterpret_cast<void*>(&ProbeSetCooperativeLevel));
            ProbePatchSlotLocked(pv, 14, reinterpret_cast<void*>(&ProbeGetObjectInfo));
            ProbePatchSlotLocked(pv, 15, reinterpret_cast<void*>(&ProbeGetDeviceInfo));
            pv->deviceHooks = true;
            fresh = true;
        }
        // GetDeviceState on a game-controller vtable nobody has hooked yet
        // (e.g. a separate vtable handed out by QueryInterface).
        void* cur = pv->vtbl[9];
        if (kind == KIND_GAMECTRL && cur != reinterpret_cast<void*>(&HookedGetDeviceState) &&
            cur != reinterpret_cast<void*>(&HookedKeyboardGetDeviceState)) {
            addedState = ProbePatchSlotLocked(pv, 9, reinterpret_cast<void*>(&HookedGetDeviceState));
        }
        if (v2 && !pv->v2) {
            ProbePatchSlotLocked(pv, 25, reinterpret_cast<void*>(&ProbePoll));
            pv->v2 = true;
            addedPoll = true;
        }
    }
    ReleaseSRWLockExclusive(&g_probeLock);
    if (fresh) DebugLogger::LogFormat("[DIPROBE] device vtable %p: methods wrapped for logging", vtbl);
    if (addedState) DebugLogger::LogFormat("[DIPROBE] device vtable %p: GetDeviceState mapper added (vtable not seen before)", vtbl);
    if (addedPoll) DebugLogger::LogFormat("[DIPROBE] device vtable %p: Poll wrapped (Device2+ interface)", vtbl);
}

static void ProbeOnCreateDevice(const char* via, const GUID* rguid, const IID* riid, HRESULT hr, void* dev) {
    char g[64], i[64] = "";
    ProbeGuidName(rguid, g, sizeof(g));
    if (riid) ProbeGuidName(riid, i, sizeof(i));
    int kind = KIND_GAMECTRL;
    if (rguid && IsEqualGUID(*rguid, GUID_SysKeyboard)) kind = KIND_KEYBOARD;
    else if (rguid && IsEqualGUID(*rguid, GUID_SysMouse)) kind = KIND_MOUSE;
    int id = 0;
    if (SUCCEEDED(hr) && dev) {
        AcquireSRWLockExclusive(&g_probeLock);
        if (ProbeDev* d = ProbeAddDevLocked(dev, kind, rguid)) id = d->id;
        ReleaseSRWLockExclusive(&g_probeLock);
    }
    DebugLogger::LogFormat("[DIPROBE] %s(%s%s%s) -> hr=0x%08lX device=%p -> dev#%d %s", via, g, riid ? ", " : "", i,
        (unsigned long)hr, dev, id, ProbeKindName(kind));
    if (SUCCEEDED(hr) && dev) ProbeHookDeviceVtbl(dev, ProbeIsDevice2Plus(riid), kind);
}

// Called from HookedGetDeviceState right after the real read, BEFORE the
// mapper writes anything -- so the raw lines show what the real pad reported.
// Returns the device's kind.
static int ProbeOnGetDeviceState(void* self, DWORD cb, const void* data, HRESULT hr) {
    const DWORD now = GetTickCount();
    int kind = KIND_UNKNOWN, id = 0;
    bool logFirst = false, logMilestone = false, logRaw = false;
    DWORD polls = 0, elapsed = 0;
    uint32_t buttons = 0;
    DWORD pov = 0xFFFFFFFF;
    LONG axes[6] = {};

    const bool joyBuf = SUCCEEDED(hr) && data && cb >= sizeof(DIJOYSTATE);
    if (joyBuf) {
        auto* js = static_cast<const DIJOYSTATE*>(data);
        for (int b = 0; b < 32; ++b) if (js->rgbButtons[b] & 0x80) buttons |= (1u << b);
        pov = js->rgdwPOV[0];
        axes[0] = js->lX; axes[1] = js->lY; axes[2] = js->lZ;
        axes[3] = js->lRx; axes[4] = js->lRy; axes[5] = js->lRz;
    }

    AcquireSRWLockExclusive(&g_probeLock);
    ProbeDev* d = ProbeFindDevLocked(self);
    if (!d) d = ProbeAddDevLocked(self, KIND_UNKNOWN, nullptr);
    if (d) {
        kind = d->kind;
        id = d->id;
        if (d->polls == 0) d->firstPollTick = now;
        ++d->polls;
        polls = d->polls;
        elapsed = now - d->firstPollTick;
        if (d->lastCb != cb) { d->lastCb = cb; logFirst = true; }
        logMilestone = (polls == 100 || polls == 1000 || polls == 10000 || polls == 100000);
        if (joyBuf && kind != KIND_KEYBOARD && kind != KIND_MOUSE) {
            bool buttonsChanged = !d->haveRaw || buttons != d->rawButtons || pov != d->rawPov;
            bool axesMoved = false;
            for (int a = 0; a < 6; ++a) {
                LONG delta = axes[a] - d->rawAxes[a];
                if (delta > 2000 || delta < -2000) axesMoved = true;
            }
            if (axesMoved && !buttonsChanged && now - d->lastAxisLogTick < 250) axesMoved = false;
            if (buttonsChanged || axesMoved) {
                logRaw = true;
                d->haveRaw = true;
                d->rawButtons = buttons;
                d->rawPov = pov;
                std::memcpy(d->rawAxes, axes, sizeof(axes));
                d->lastAxisLogTick = now;
            }
        }
    }
    ReleaseSRWLockExclusive(&g_probeLock);

    if (logFirst && ProbeUnderCap(&g_pcFirstPoll, 24)) {
        DebugLogger::LogFormat("[DIPROBE] GetDeviceState dev#%d %s: first read with cbData=%lu (%s) -> hr=0x%08lX%s",
            id, ProbeKindName(kind), cb, ProbeFormatHint(cb), (unsigned long)hr,
            cb >= sizeof(DIJOYSTATE) ? "" : " -- smaller than a joystick state, the pad mapper will not write to it");
    }
    if (logMilestone) {
        DebugLogger::LogFormat("[DIPROBE] GetDeviceState dev#%d %s: %lu reads in %lu ms (~%.0f per second), cbData=%lu",
            id, ProbeKindName(kind), polls, elapsed, elapsed ? polls * 1000.0 / elapsed : 0.0, cb);
    }
    if (FAILED(hr) && ProbeUnderCap(&g_pcStateFail, 8)) {
        DebugLogger::LogFormat("[DIPROBE] GetDeviceState dev#%d %s: hr=0x%08lX (0x8007000C = not acquired, 0x8007001E = input lost)",
            id, ProbeKindName(kind), (unsigned long)hr);
    }
    if (logRaw && ProbeUnderCap(&g_pcRaw, 80)) {
        DebugLogger::LogFormat("[DIPROBE] raw dev#%d: X=%ld Y=%ld Z=%ld Rx=%ld Ry=%ld Rz=%ld POV=%ld buttons=%08lX",
            id, axes[0], axes[1], axes[2], axes[3], axes[4], axes[5],
            pov == 0xFFFFFFFF ? -1L : (long)pov, (unsigned long)buttons);
    }
    return kind;
}

static bool MapPadIntoJoyState(void* self, DWORD cbData, LPVOID lpvData);

static HRESULT STDMETHODCALLTYPE HookedGetDeviceState(LPVOID self, DWORD cbData, LPVOID lpvData) {
    // The real function for THIS object's vtable (the probe can add this hook
    // to a vtable QueryInterface hands out); falls back to the one saved when
    // the hook was first installed.
    auto original = reinterpret_cast<DIDevice_GetDeviceState_t>(ProbeOriginal(self, 9));
    if (!original) original = g_OriginalGetDeviceState;
    HRESULT hr = original ? original(self, cbData, lpvData) : DIERR_GENERIC;
    const int kind = ProbeOnGetDeviceState(self, cbData, lpvData, hr);
    if (FAILED(hr) || !lpvData) {
        return hr;
    }

    // Keyboards and mice share DirectInput's device vtable, so their reads land
    // here too. Never write pad state into their buffers.
    if (kind == KIND_KEYBOARD || kind == KIND_MOUSE) {
        return hr;
    }

    // Smaller than DIJOYSTATE = not a joystick read; nothing below applies.
    // (Checked first so the "state mapper active" line below can no longer
    // claim success for a device it never writes to -- the 2026-10-05
    // no-gamepad report logged it while no pad buttons were reaching the game.)
    if (cbData < sizeof(DIJOYSTATE)) {
        return hr;
    }

    MapPadIntoJoyState(self, cbData, lpvData);
    return hr;
}

// Writes the VR controllers (or, with no headset input, physical XInput pad 0)
// into a joystick-format state buffer of at least sizeof(DIJOYSTATE). Shared
// by the real-joystick hook above and the virtual pad below, so both feed the
// game identically. Returns false, leaving the buffer as it was, when there
// is no input source at all.
static bool MapPadIntoJoyState(void* self, DWORD cbData, LPVOID lpvData) {
    XINPUT_STATE xi{};
    bool hasInjectedState = GetInjectedXInputState(&xi);
    if (!hasInjectedState && XInputGetState(0, &xi) != ERROR_SUCCESS) {
        static bool loggedPassthrough = false;
        if (!loggedPassthrough) {
            DebugLogger::Log("DirectInput mapper passthrough: no OpenXR virtual pad and no physical XInput pad; preserving native DI state");
            loggedPassthrough = true;
        }
        return false;
    }

    static bool loggedInjected = false;
    static bool loggedPhysical = false;
    if (hasInjectedState && !loggedInjected) {
        char tag[48];
        ProbeTag(self, tag, sizeof(tag));
        DebugLogger::LogFormat("DirectInput state mapper active: feeding legacy DI state from OpenXR virtual gamepad (%s, cbData=%lu)", tag, cbData);
        loggedInjected = true;
    }
    if (!hasInjectedState && !loggedPhysical) {
        char tag[48];
        ProbeTag(self, tag, sizeof(tag));
        DebugLogger::LogFormat("DirectInput state mapper active: feeding legacy DI state from physical XInput controller 0 (%s, cbData=%lu)", tag, cbData);
        loggedPhysical = true;
    }

    LONG lx = NormalizeAxisToMgsRange(xi.Gamepad.sThumbLX);
    LONG ly = NormalizeAxisToMgsRange((SHORT)-xi.Gamepad.sThumbLY);
    DWORD pov = BuildPovFromDpad(xi);

    if (cbData >= sizeof(DIJOYSTATE2)) {
        auto* js = reinterpret_cast<DIJOYSTATE2*>(lpvData);
        js->lX = lx;
        js->lY = ly;
        js->lZ = 0;
        js->lRx = 0;
        js->lRy = 0;
        js->lRz = 0;
        js->rglSlider[0] = 0;
        js->rglSlider[1] = 0;
        js->rgdwPOV[0] = pov;
        js->rgdwPOV[1] = 0xFFFFFFFF;
        js->rgdwPOV[2] = 0xFFFFFFFF;
        js->rgdwPOV[3] = 0xFFFFFFFF;

        js->rgbButtons[0] = (xi.Gamepad.wButtons & XINPUT_GAMEPAD_A) ? 0x80 : 0;
        js->rgbButtons[1] = (xi.Gamepad.wButtons & XINPUT_GAMEPAD_B) ? 0x80 : 0;
        js->rgbButtons[2] = (xi.Gamepad.wButtons & XINPUT_GAMEPAD_X) ? 0x80 : 0;
        js->rgbButtons[3] = (xi.Gamepad.wButtons & XINPUT_GAMEPAD_Y) ? 0x80 : 0;
        js->rgbButtons[4] = (xi.Gamepad.wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER) ? 0x80 : 0;
        js->rgbButtons[5] = (xi.Gamepad.wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER) ? 0x80 : 0;
        js->rgbButtons[6] = (xi.Gamepad.bLeftTrigger > 40) ? 0x80 : 0;
        js->rgbButtons[7] = (xi.Gamepad.bRightTrigger > 40) ? 0x80 : 0;

        // The virtual pad addresses the game's buttons directly, which is the
        // only way to reach 8-11 -- Select/Radio and Menu/Pause live up there
        // and no XInput bit maps to them.
        uint32_t gameButtons = 0;
        if (GetInjectedGameButtons(&gameButtons)) {
            // 32: the game's controller settings can put an action on any
            // joystick button 0..31 (input codes 0x00-0x1F, mgsi.exe+432C60).
            for (int b = 0; b < 32; ++b) {
                js->rgbButtons[b] = (gameButtons & (1u << b)) ? 0x80 : 0;
            }
        }
    }
    else if (cbData >= sizeof(DIJOYSTATE)) {
        auto* js = reinterpret_cast<DIJOYSTATE*>(lpvData);
        js->lX = lx;
        js->lY = ly;
        js->lZ = 0;
        js->lRx = 0;
        js->lRy = 0;
        js->lRz = 0;
        js->rgbButtons[0] = (xi.Gamepad.wButtons & XINPUT_GAMEPAD_A) ? 0x80 : 0;
        js->rgbButtons[1] = (xi.Gamepad.wButtons & XINPUT_GAMEPAD_B) ? 0x80 : 0;
        js->rgbButtons[2] = (xi.Gamepad.wButtons & XINPUT_GAMEPAD_X) ? 0x80 : 0;
        js->rgbButtons[3] = (xi.Gamepad.wButtons & XINPUT_GAMEPAD_Y) ? 0x80 : 0;

        // DIJOYSTATE only carries 32 buttons but the game asks for a handful;
        // fill what this struct has from the same direct mapping.
        uint32_t gameButtons = 0;
        if (GetInjectedGameButtons(&gameButtons)) {
            // 32: the game's controller settings can put an action on any
            // joystick button 0..31 (input codes 0x00-0x1F, mgsi.exe+432C60).
            for (int b = 0; b < 32; ++b) {
                js->rgbButtons[b] = (gameButtons & (1u << b)) ? 0x80 : 0;
            }
        }
    }

    return true;
}

// Standard DirectInput keyboard GetDeviceState() format: a 256-byte array,
// one byte per DIK_* code, high bit (0x80) set = pressed. We only ever
// touch the single byte for whatever key is currently injected, leaving
// every other byte exactly as the real device reported it -- so real
// keyboard input keeps working normally alongside injected keys.
static HRESULT STDMETHODCALLTYPE HookedKeyboardGetDeviceState(LPVOID self, DWORD cbData, LPVOID lpvData) {
    HRESULT hr = g_OriginalKeyboardGetDeviceState ? g_OriginalKeyboardGetDeviceState(self, cbData, lpvData) : DIERR_GENERIC;
    if (FAILED(hr) || !lpvData || cbData < 256) {
        return hr;
    }

    EnsureInjectedKeyLockInit();
    EnterCriticalSection(&g_injectedKeyLock);
    BYTE code = g_injectedKeyCode;
    bool down = g_injectedKeyDown;
    LeaveCriticalSection(&g_injectedKeyLock);

    if (code != 0) {
        reinterpret_cast<BYTE*>(lpvData)[code] = down ? 0x80 : 0x00;
    }

    return hr;
}

// ===========================================================================
// VIRTUAL JOYSTICK (2026-10-05)
//
// MGS1 reads its pad ONLY through a DirectInput joystick, and only opens one
// if Windows lists one at startup. With no gamepad plugged in it opens none
// (DIPROBE log, 2026-10-05: "EnumDevices ... 0 device(s)", then only the
// mouse is created), so every action the mod presses as a pad button --
// Action, Weapon, Crawl, First person -- went nowhere. Only keyboard keys and
// the mod's own features worked. A wired pad hid this on the dev machine.
//
// This is a DirectInput joystick of our own. The game is shown it during its
// startup enumeration, opens it exactly as it opened the wired pad, and reads
// it every frame; GetDeviceState is filled by the same MapPadIntoJoyState the
// real-pad hook uses. So without a controller the game behaves as it does
// with one.
//
// It answers the calls the same game made on a real Xbox pad (DIPROBE log,
// wired pad connected): CreateDeviceEx as IID_IDirectInputDevice2A,
// GetDeviceInfo, SetDataFormat(c_dfDIJoystick, 80 bytes), SetCooperativeLevel
// (exclusive | foreground), GetCapabilities (5 axes, 16 buttons, 1 POV),
// EnumObjects for axes (setting DIPROP_RANGE -1000..1000 on each by offset)
// and for buttons, Acquire, then Poll + GetDeviceState every frame. The rest
// of the interface is implemented in the plain way DirectInput would for a
// pad with no force feedback.
//
// [input] virtual_joystick in mgs1_vr_config.ini:
//   1 = only when Windows reports no joystick (default)
//   2 = always, ahead of any real pad (for testing with a pad plugged in)
//   0 = off
// ===========================================================================

// Made-up instance GUID: identifies the virtual pad in CreateDevice calls.
static const GUID kVirtualPadInstance =
    { 0x4D475331, 0x5652, 0x4A6F, { 0x79, 0x50, 0x61, 0x64, 0x00, 0x00, 0x00, 0x01 } };
// Product GUID in DirectInput's "PIDVID" form, as an Xbox 360 pad (045E:028E).
static const GUID kVirtualPadProduct =
    { 0x028E045E, 0x0000, 0x0000, { 0x00, 0x00, 0x50, 0x49, 0x44, 0x56, 0x49, 0x44 } };
static const char kVirtualPadName[] = "MGS1 VR Controller";
static constexpr DWORD kVirtualPadDevType = 0x00010404;   // HID | gamepad | joystick, as the wired pad reported

static int VirtualPadMode() {
    static int mode = -1;
    if (mode < 0) {
        char exePath[MAX_PATH] = { 0 };
        GetModuleFileNameA(NULL, exePath, MAX_PATH);
        std::string dir(exePath);
        auto slash = dir.find_last_of("\\/");
        if (slash != std::string::npos) dir = dir.substr(0, slash);
        const std::string ini = dir + "\\mgs1_vr_config.ini";
        int m = (int)GetPrivateProfileIntA("input", "virtual_joystick", 1, ini.c_str());
        if (m < 0 || m > 2) m = 1;
        mode = m;
        DebugLogger::LogFormat("Virtual joystick: virtual_joystick=%d (%s)", mode,
            mode == 0 ? "off -- with no gamepad plugged in, pad buttons will not reach the game" :
            mode == 1 ? "used only when Windows reports no joystick" :
                        "always used, ahead of any real pad");
    }
    return mode;
}

// [input] virtual_joystick_stick: 1 = the virtual pad also carries the left
// stick as analog axes. Default 0: movement goes only through the arrow keys.
static bool VirtualPadSendsStick() {
    static int v = -1;
    if (v < 0) {
        char exePath[MAX_PATH] = { 0 };
        GetModuleFileNameA(NULL, exePath, MAX_PATH);
        std::string dir(exePath);
        auto slash = dir.find_last_of("\\/");
        if (slash != std::string::npos) dir = dir.substr(0, slash);
        v = GetPrivateProfileIntA("input", "virtual_joystick_stick", 0, (dir + "\\mgs1_vr_config.ini").c_str()) ? 1 : 0;
        DebugLogger::LogFormat("Virtual joystick: virtual_joystick_stick=%d (%s)", v,
            v ? "left stick also sent as pad axes" : "buttons only; movement stays on the arrow keys");
    }
    return v == 1;
}

static bool VirtualPadIsGuid(const GUID* g) {
    return g && IsEqualGUID(*g, kVirtualPadInstance);
}

struct VirtualPadObject { const GUID* guid; DWORD ofs; DWORD type; DWORD flags; const char* name; };

static constexpr int kVirtualPadButtons = 16;

// Objects in c_dfDIJoystick layout: 5 axes, 1 POV, 16 buttons.
static int VirtualPadObjectAt(int index, VirtualPadObject& out, char* nameBuf, size_t nameCap) {
    static const VirtualPadObject kAxesAndPov[] = {
        { &GUID_XAxis,  0, DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE(0), DIDOI_ASPECTPOSITION, "X Axis" },
        { &GUID_YAxis,  4, DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE(1), DIDOI_ASPECTPOSITION, "Y Axis" },
        { &GUID_ZAxis,  8, DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE(2), DIDOI_ASPECTPOSITION, "Z Axis" },
        { &GUID_RxAxis, 12, DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE(3), DIDOI_ASPECTPOSITION, "X Rotation" },
        { &GUID_RyAxis, 16, DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE(4), DIDOI_ASPECTPOSITION, "Y Rotation" },
        { &GUID_POV,    32, DIDFT_POV | DIDFT_MAKEINSTANCE(0), 0, "Hat Switch" },
    };
    constexpr int kFixed = (int)(sizeof(kAxesAndPov) / sizeof(kAxesAndPov[0]));
    if (index < 0) return 0;
    if (index < kFixed) { out = kAxesAndPov[index]; return 1; }
    const int b = index - kFixed;
    if (b >= kVirtualPadButtons) return 0;
    sprintf_s(nameBuf, nameCap, "Button %d", b);
    out = { &GUID_Button, (DWORD)(48 + b), (DWORD)(DIDFT_PSHBUTTON | DIDFT_MAKEINSTANCE(b)), 0, nameBuf };
    return 1;
}

static bool VirtualPadTypeMatches(DWORD objType, DWORD flags) {
    if (flags & (DIDFT_FFACTUATOR | DIDFT_FFEFFECTTRIGGER | DIDFT_OUTPUT | DIDFT_COLLECTION | DIDFT_NODATA)) return false;
    const DWORD wanted = flags & 0xFF;   // DIDFT_ALL == 0
    return wanted == 0 || (objType & wanted) != 0;
}

static void VirtualPadFillObject(const VirtualPadObject& o, DIDEVICEOBJECTINSTANCEA& inst) {
    std::memset(&inst, 0, sizeof(inst));
    inst.dwSize = sizeof(inst);
    inst.guidType = *o.guid;
    inst.dwOfs = o.ofs;
    inst.dwType = o.type;
    inst.dwFlags = o.flags;
    strcpy_s(inst.tszName, sizeof(inst.tszName), o.name);
}

static void VirtualPadFillInstance(DIDEVICEINSTANCEA& inst) {
    std::memset(&inst, 0, sizeof(inst));
    inst.dwSize = sizeof(inst);
    inst.guidInstance = kVirtualPadInstance;
    inst.guidProduct = kVirtualPadProduct;
    inst.dwDevType = kVirtualPadDevType;
    strcpy_s(inst.tszInstanceName, sizeof(inst.tszInstanceName), kVirtualPadName);
    strcpy_s(inst.tszProductName, sizeof(inst.tszProductName), kVirtualPadName);
    inst.wUsagePage = 0x01;   // generic desktop
    inst.wUsage = 0x05;       // game pad
}

// Copies our full-size struct into the caller's, honouring the dwSize the
// caller declared (DirectInput 3 structs are a shorter prefix of the same).
template <typename T>
static HRESULT VirtualPadCopyOut(const T& full, T* out, DWORD minSize) {
    if (!out || out->dwSize < minSize || out->dwSize > sizeof(T)) return DIERR_INVALIDPARAM;
    const DWORD size = out->dwSize;
    std::memcpy(reinterpret_cast<BYTE*>(out) + sizeof(DWORD), reinterpret_cast<const BYTE*>(&full) + sizeof(DWORD),
        size - sizeof(DWORD));
    return DI_OK;
}

// Sizes of the DirectInput 3 versions of these structs: each is the full
// struct cut off where the DirectInput 5 fields begin.
static constexpr DWORD kDevCapsDx3Size     = offsetof(DIDEVCAPS, dwFFSamplePeriod);              // 24
static constexpr DWORD kObjInstDx3Size     = offsetof(DIDEVICEOBJECTINSTANCEA, dwFFMaxForce);    // 292
static constexpr DWORD kDevInstDx3Size     = offsetof(DIDEVICEINSTANCEA, guidFFDriver);          // 560
static constexpr DWORD kObjDataDx3Size     = 16;   // DIDEVICEOBJECTDATA without DirectInput 8's uAppData

static volatile LONG g_vpLogProp = 0, g_vpLogMisc = 0;

class VirtualPad final : public IDirectInputDevice7A {
public:
    // ---- IUnknown ------------------------------------------------------------
    STDMETHODIMP QueryInterface(REFIID riid, LPVOID* ppv) override {
        if (!ppv) return E_POINTER;
        if (IsEqualGUID(riid, __uuidof(IUnknown)) || IsEqualGUID(riid, IID_IDirectInputDeviceA) ||
            IsEqualGUID(riid, IID_IDirectInputDevice2A) || IsEqualGUID(riid, IID_IDirectInputDevice7A)) {
            *ppv = this;
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        if (ProbeUnderCap(&g_vpLogMisc, 24)) {
            char name[64];
            ProbeGuidName(&riid, name, sizeof(name));
            DebugLogger::LogFormat("Virtual pad: QueryInterface(%s) refused", name);
        }
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return (ULONG)InterlockedIncrement(&m_ref); }
    STDMETHODIMP_(ULONG) Release() override {
        LONG r = InterlockedDecrement(&m_ref);
        if (r <= 0) { m_ref = 0; m_acquired = false; }   // kept alive: one instance for the whole session
        return (ULONG)(r < 0 ? 0 : r);
    }

    // ---- IDirectInputDeviceA ---------------------------------------------------
    STDMETHODIMP GetCapabilities(LPDIDEVCAPS caps) override {
        if (!caps || caps->dwSize < kDevCapsDx3Size || caps->dwSize > sizeof(DIDEVCAPS)) return DIERR_INVALIDPARAM;
        DIDEVCAPS full{};
        full.dwSize = sizeof(full);
        full.dwFlags = DIDC_ATTACHED | DIDC_EMULATED;   // what the wired pad reported (0x5)
        full.dwDevType = kVirtualPadDevType;
        full.dwAxes = 5;
        full.dwButtons = kVirtualPadButtons;
        full.dwPOVs = 1;
        return VirtualPadCopyOut(full, caps, kDevCapsDx3Size);
    }

    STDMETHODIMP EnumObjects(LPDIENUMDEVICEOBJECTSCALLBACKA cb, LPVOID ref, DWORD flags) override {
        if (!cb) return DIERR_INVALIDPARAM;
        int count = 0;
        for (int i = 0;; ++i) {
            VirtualPadObject o;
            char nameBuf[32];
            if (!VirtualPadObjectAt(i, o, nameBuf, sizeof(nameBuf))) break;
            if (!VirtualPadTypeMatches(o.type, flags)) continue;
            DIDEVICEOBJECTINSTANCEA inst;
            VirtualPadFillObject(o, inst);
            ++count;
            if (cb(&inst, ref) != DIENUM_CONTINUE) break;
        }
        if (ProbeUnderCap(&g_vpLogMisc, 24)) {
            DebugLogger::LogFormat("Virtual pad: EnumObjects(flags=0x%08lX) -> %d object(s)", flags, count);
        }
        return DI_OK;
    }

    STDMETHODIMP GetProperty(REFGUID prop, LPDIPROPHEADER h) override {
        const uintptr_t id = reinterpret_cast<uintptr_t>(&prop);
        if (!h || h->dwHeaderSize != sizeof(DIPROPHEADER)) return DIERR_INVALIDPARAM;
        HRESULT hr = DIERR_UNSUPPORTED;
        int axis = AxisFor(h);
        switch (id) {
        case 1:   // BUFFERSIZE
            hr = SetDword(h, m_bufferSize); break;
        case 2:   // AXISMODE
            hr = SetDword(h, DIPROPAXISMODE_ABS); break;
        case 3:   // GRANULARITY
            hr = SetDword(h, 1); break;
        case 4:   // RANGE
            if (h->dwSize != sizeof(DIPROPRANGE)) { hr = DIERR_INVALIDPARAM; break; }
            if (h->dwHow != DIPH_DEVICE && axis < 0) { hr = DIERR_OBJECTNOTFOUND; break; }
            reinterpret_cast<DIPROPRANGE*>(h)->lMin = m_min[axis < 0 ? 0 : axis];
            reinterpret_cast<DIPROPRANGE*>(h)->lMax = m_max[axis < 0 ? 0 : axis];
            hr = DI_OK; break;
        case 5:   // DEADZONE
            hr = SetDword(h, m_deadzone[axis < 0 ? 0 : axis]); break;
        case 6:   // SATURATION
            hr = SetDword(h, m_saturation[axis < 0 ? 0 : axis]); break;
        case 9:   // AUTOCENTER
            hr = SetDword(h, DIPROPAUTOCENTER_OFF); break;
        case 13:  // INSTANCENAME
        case 14:  // PRODUCTNAME
            if (h->dwSize != sizeof(DIPROPSTRING)) { hr = DIERR_INVALIDPARAM; break; }
            MultiByteToWideChar(CP_ACP, 0, kVirtualPadName, -1, reinterpret_cast<DIPROPSTRING*>(h)->wsz, MAX_PATH);
            hr = DI_OK; break;
        case 15:  // JOYSTICKID
            hr = SetDword(h, 0); break;
        case 24:  // VIDPID
            hr = SetDword(h, 0x028E045E); break;
        default:
            break;
        }
        if (ProbeUnderCap(&g_vpLogProp, 24)) {
            DebugLogger::LogFormat("Virtual pad: GetProperty(#%u, obj=%lu how=%lu) -> hr=0x%08lX",
                (unsigned)id, h->dwObj, h->dwHow, (unsigned long)hr);
        }
        return hr;
    }

    STDMETHODIMP SetProperty(REFGUID prop, LPCDIPROPHEADER h) override {
        const uintptr_t id = reinterpret_cast<uintptr_t>(&prop);
        if (!h || h->dwHeaderSize != sizeof(DIPROPHEADER)) return DIERR_INVALIDPARAM;
        HRESULT hr = DI_PROPNOEFFECT;
        const int axis = AxisFor(h);
        auto forAxes = [&](auto fn) -> HRESULT {
            if (h->dwHow == DIPH_DEVICE) { for (int a = 0; a < 5; ++a) fn(a); return DI_OK; }
            if (axis < 0) return DIERR_OBJECTNOTFOUND;
            fn(axis);
            return DI_OK;
        };
        switch (id) {
        case 1:   // BUFFERSIZE
            if (h->dwSize != sizeof(DIPROPDWORD)) { hr = DIERR_INVALIDPARAM; break; }
            m_bufferSize = reinterpret_cast<const DIPROPDWORD*>(h)->dwData;
            hr = DI_OK; break;
        case 4: { // RANGE
            if (h->dwSize != sizeof(DIPROPRANGE)) { hr = DIERR_INVALIDPARAM; break; }
            auto* r = reinterpret_cast<const DIPROPRANGE*>(h);
            if (r->lMin >= r->lMax) { hr = DIERR_INVALIDPARAM; break; }
            hr = forAxes([&](int a) { m_min[a] = r->lMin; m_max[a] = r->lMax; });
            if (r->lMin != -1000 || r->lMax != 1000) {
                DebugLogger::LogFormat("Virtual pad: WARNING the game asked for axis range %ld..%ld; the pad mapper writes -1000..1000",
                    r->lMin, r->lMax);
            }
            break;
        }
        case 5:   // DEADZONE
        case 6: { // SATURATION
            if (h->dwSize != sizeof(DIPROPDWORD)) { hr = DIERR_INVALIDPARAM; break; }
            const DWORD v = reinterpret_cast<const DIPROPDWORD*>(h)->dwData;
            if (v > 10000) { hr = DIERR_INVALIDPARAM; break; }
            hr = forAxes([&](int a) { if (id == 5) m_deadzone[a] = v; else m_saturation[a] = v; });
            break;
        }
        case 2:   // AXISMODE
        case 9:   // AUTOCENTER
        case 10:  // CALIBRATIONMODE
            hr = DI_OK; break;
        default:
            break;    // anything else: accepted, no effect
        }
        if (ProbeUnderCap(&g_vpLogProp, 24)) {
            DebugLogger::LogFormat("Virtual pad: SetProperty(#%u, obj=%lu how=%lu) -> hr=0x%08lX",
                (unsigned)id, h->dwObj, h->dwHow, (unsigned long)hr);
        }
        return hr;
    }

    STDMETHODIMP Acquire() override {
        const bool was = m_acquired;
        m_acquired = true;
        if (!was && ProbeUnderCap(&g_vpLogMisc, 24)) DebugLogger::Log("Virtual pad: acquired");
        return was ? DI_NOEFFECT : DI_OK;
    }
    STDMETHODIMP Unacquire() override {
        const bool was = m_acquired;
        m_acquired = false;
        if (was && ProbeUnderCap(&g_vpLogMisc, 24)) DebugLogger::Log("Virtual pad: unacquired");
        return was ? DI_OK : DI_NOEFFECT;
    }

    STDMETHODIMP GetDeviceState(DWORD cb, LPVOID data) override {
        if (!data || (cb != sizeof(DIJOYSTATE) && cb != sizeof(DIJOYSTATE2))) {
            ProbeOnGetDeviceState(this, cb, nullptr, DIERR_INVALIDPARAM);
            return DIERR_INVALIDPARAM;
        }
        if (!m_acquired) {
            ProbeOnGetDeviceState(this, cb, nullptr, DIERR_NOTACQUIRED);
            return DIERR_NOTACQUIRED;
        }
        // Neutral pad: every axis centred in the range the game set, no hat,
        // no buttons. Then the same mapper the real-pad hook uses.
        std::memset(data, 0, cb);
        auto* js = static_cast<DIJOYSTATE*>(data);
        LONG* axes[5] = { &js->lX, &js->lY, &js->lZ, &js->lRx, &js->lRy };
        for (int a = 0; a < 5; ++a) *axes[a] = m_min[a] + (m_max[a] - m_min[a]) / 2;
        for (int p = 0; p < 4; ++p) js->rgdwPOV[p] = 0xFFFFFFFF;
        MapPadIntoJoyState(this, cb, data);
        // Buttons only, by default. Movement already reaches the game as arrow
        // keys from vr_input.cpp, which apply twin-stick (left/right turns via
        // the RIGHT stick in first person) and head-relative steering. The
        // mapper also writes the raw left stick into lX/lY, so through the
        // virtual pad the game got movement twice (2026-10-05 test: menus
        // skipped options, and first person suddenly strafed left/right with
        // the left stick). Re-centre the axes so the keys are the only route.
        if (!VirtualPadSendsStick()) {
            for (int a = 0; a < 5; ++a) *axes[a] = m_min[a] + (m_max[a] - m_min[a]) / 2;
        }
        // Logged AFTER mapping, so the probe's raw lines show what the game read.
        ProbeOnGetDeviceState(this, cb, data, DI_OK);
        return DI_OK;
    }

    STDMETHODIMP GetDeviceData(DWORD cbObjectData, LPDIDEVICEOBJECTDATA, LPDWORD inOut, DWORD) override {
        if (!inOut || (cbObjectData != kObjDataDx3Size && cbObjectData != sizeof(DIDEVICEOBJECTDATA))) return DIERR_INVALIDPARAM;
        if (!m_acquired) return DIERR_NOTACQUIRED;
        if (m_bufferSize == 0) return DIERR_NOTBUFFERED;
        *inOut = 0;   // no buffered events: the game reads state, not events
        return DI_OK;
    }

    STDMETHODIMP SetDataFormat(LPCDIDATAFORMAT f) override {
        if (!f) return DIERR_INVALIDPARAM;
        if (m_acquired) return DIERR_ACQUIRED;
        const bool standard = f->dwDataSize == sizeof(DIJOYSTATE) || f->dwDataSize == sizeof(DIJOYSTATE2);
        DebugLogger::LogFormat("Virtual pad: SetDataFormat(dataSize=%lu, %lu objects)%s", f->dwDataSize, f->dwNumObjs,
            standard ? "" : " -- WARNING not a standard joystick format; the virtual pad only fills c_dfDIJoystick/c_dfDIJoystick2 layouts");
        return DI_OK;
    }

    STDMETHODIMP SetEventNotification(HANDLE) override { return DI_OK; }   // never signalled: the game polls

    STDMETHODIMP SetCooperativeLevel(HWND, DWORD) override { return DI_OK; }

    STDMETHODIMP GetObjectInfo(LPDIDEVICEOBJECTINSTANCEA info, DWORD obj, DWORD how) override {
        for (int i = 0;; ++i) {
            VirtualPadObject o;
            char nameBuf[32];
            if (!VirtualPadObjectAt(i, o, nameBuf, sizeof(nameBuf))) break;
            bool hit = false;
            if (how == DIPH_BYOFFSET) hit = (o.ofs == obj);
            else if (how == DIPH_BYID) hit = (DIDFT_GETINSTANCE(o.type) == DIDFT_GETINSTANCE(obj)) && (o.type & obj & 0xFF);
            if (!hit) continue;
            DIDEVICEOBJECTINSTANCEA full;
            VirtualPadFillObject(o, full);
            return VirtualPadCopyOut(full, info, kObjInstDx3Size);
        }
        return DIERR_OBJECTNOTFOUND;
    }

    STDMETHODIMP GetDeviceInfo(LPDIDEVICEINSTANCEA info) override {
        DIDEVICEINSTANCEA full;
        VirtualPadFillInstance(full);
        return VirtualPadCopyOut(full, info, kDevInstDx3Size);
    }

    STDMETHODIMP RunControlPanel(HWND, DWORD) override { return DI_OK; }
    STDMETHODIMP Initialize(HINSTANCE, DWORD, REFGUID g) override {
        return IsEqualGUID(g, kVirtualPadInstance) ? DI_OK : DIERR_DEVICENOTREG;
    }

    // ---- IDirectInputDevice2A: no force feedback -------------------------------
    STDMETHODIMP CreateEffect(REFGUID, LPCDIEFFECT, LPDIRECTINPUTEFFECT* out, LPUNKNOWN) override {
        if (out) *out = nullptr;
        return DIERR_UNSUPPORTED;
    }
    STDMETHODIMP EnumEffects(LPDIENUMEFFECTSCALLBACKA, LPVOID, DWORD) override { return DI_OK; }
    STDMETHODIMP GetEffectInfo(LPDIEFFECTINFOA, REFGUID) override { return DIERR_DEVICENOTREG; }
    STDMETHODIMP GetForceFeedbackState(LPDWORD) override { return DIERR_UNSUPPORTED; }
    STDMETHODIMP SendForceFeedbackCommand(DWORD) override { return DIERR_UNSUPPORTED; }
    STDMETHODIMP EnumCreatedEffectObjects(LPDIENUMCREATEDEFFECTOBJECTSCALLBACK, LPVOID, DWORD) override { return DI_OK; }
    STDMETHODIMP Escape(LPDIEFFESCAPE) override { return DIERR_UNSUPPORTED; }
    STDMETHODIMP Poll() override { return m_acquired ? DI_NOEFFECT : DIERR_NOTACQUIRED; }   // the wired pad answered 1 too
    STDMETHODIMP SendDeviceData(DWORD, LPCDIDEVICEOBJECTDATA, LPDWORD inOut, DWORD) override {
        if (inOut) *inOut = 0;
        return DIERR_UNSUPPORTED;
    }

    // ---- IDirectInputDevice7A ----------------------------------------------------
    STDMETHODIMP EnumEffectsInFile(LPCSTR, LPDIENUMEFFECTSINFILECALLBACK, LPVOID, DWORD) override { return DI_OK; }
    STDMETHODIMP WriteEffectToFile(LPCSTR, DWORD, LPDIFILEEFFECT, DWORD) override { return DIERR_UNSUPPORTED; }

private:
    // Axis index (0..4) a property header points at, or -1.
    int AxisFor(LPCDIPROPHEADER h) const {
        for (int a = 0; a < 5; ++a) {
            const DWORD ofs = (DWORD)(a * 4);
            if (h->dwHow == DIPH_BYOFFSET && h->dwObj == ofs) return a;
            if (h->dwHow == DIPH_BYID && DIDFT_GETINSTANCE(h->dwObj) == (DWORD)a && (h->dwObj & DIDFT_AXIS)) return a;
        }
        return -1;
    }
    static HRESULT SetDword(LPDIPROPHEADER h, DWORD v) {
        if (h->dwSize != sizeof(DIPROPDWORD)) return DIERR_INVALIDPARAM;
        reinterpret_cast<DIPROPDWORD*>(h)->dwData = v;
        return DI_OK;
    }

    volatile LONG m_ref = 0;
    volatile bool m_acquired = false;
    DWORD m_bufferSize = 0;
    LONG m_min[5] = { -1000, -1000, -1000, -1000, -1000 };
    LONG m_max[5] = { 1000, 1000, 1000, 1000, 1000 };
    DWORD m_deadzone[5] = {};
    DWORD m_saturation[5] = { 10000, 10000, 10000, 10000, 10000 };
};

static VirtualPad* VirtualPadInstance() {
    static VirtualPad* pad = nullptr;
    if (!pad) {
        pad = new VirtualPad();
        AcquireSRWLockExclusive(&g_probeLock);
        ProbeAddDevLocked(pad, KIND_VIRTUAL, &kVirtualPadInstance);
        ReleaseSRWLockExclusive(&g_probeLock);
    }
    return pad;
}

// Shows the game the virtual pad in its EnumDevices callback.
static BOOL VirtualPadOffer(LPDIENUMDEVICESCALLBACKA cb, LPVOID ref, LONG call) {
    if (!cb) return DIENUM_CONTINUE;
    DIDEVICEINSTANCEA inst;
    VirtualPadFillInstance(inst);
    BOOL r = cb(&inst, ref);
    DebugLogger::LogFormat("Virtual joystick: offered to the game (EnumDevices call %ld) -> game says %s", call,
        r == DIENUM_CONTINUE ? "continue (not taken)" : "STOP (taken)");
    return r;
}

// CreateDevice / CreateDeviceEx for the virtual pad's GUID. Returns true when
// it handled the call (hr set).
static bool VirtualPadCreate(REFGUID rguid, const IID* riid, void** out, HRESULT* hr) {
    if (!VirtualPadIsGuid(&rguid) || VirtualPadMode() == 0) return false;
    if (!out) { *hr = DIERR_INVALIDPARAM; return true; }
    *out = nullptr;
    if (riid && !(IsEqualGUID(*riid, IID_IDirectInputDeviceA) || IsEqualGUID(*riid, IID_IDirectInputDevice2A) ||
                  IsEqualGUID(*riid, IID_IDirectInputDevice7A))) {
        char name[64];
        ProbeGuidName(riid, name, sizeof(name));
        DebugLogger::LogFormat("Virtual joystick: CreateDeviceEx asked for %s -- not supported", name);
        *hr = DIERR_NOINTERFACE;
        return true;
    }
    VirtualPad* pad = VirtualPadInstance();
    pad->AddRef();
    *out = static_cast<IDirectInputDevice7A*>(pad);
    char name[64] = "IDirectInputDeviceA";
    if (riid) ProbeGuidName(riid, name, sizeof(name));
    DebugLogger::LogFormat("Virtual joystick: the game opened it as %s -- pad buttons now reach the game without a controller plugged in", name);
    *hr = DI_OK;
    return true;
}

static void TryHookDeviceState(LPVOID outDevice) {
    if (!outDevice || g_GetDeviceStateHooked) {
        return;
    }

    if (PatchComVTable(outDevice, 9, reinterpret_cast<void*>(&HookedGetDeviceState), reinterpret_cast<void**>(&g_OriginalGetDeviceState))) {
        g_GetDeviceStateHooked = true;
        DebugLogger::Log("DirectInput device GetDeviceState hook installed (menu axis sanitizer active)");
    }
}

static void TryHookKeyboardDeviceState(LPVOID outDevice) {
    if (!outDevice || g_KeyboardGetDeviceStateHooked) {
        return;
    }

    if (PatchComVTable(outDevice, 9, reinterpret_cast<void*>(&HookedKeyboardGetDeviceState), reinterpret_cast<void**>(&g_OriginalKeyboardGetDeviceState))) {
        g_KeyboardGetDeviceStateHooked = true;
        DebugLogger::Log("DirectInput KEYBOARD device GetDeviceState hook installed (synthetic key injection available)");
    }
}

static HRESULT STDMETHODCALLTYPE HookedCreateDevice(LPVOID self, REFGUID rguid, LPDIRECTINPUTDEVICEA* outDevice, LPUNKNOWN outer) {
    HRESULT vhr = DI_OK;
    if (VirtualPadCreate(rguid, nullptr, reinterpret_cast<void**>(outDevice), &vhr)) {
        return vhr;
    }

    HRESULT hr = g_OriginalCreateDevice ? g_OriginalCreateDevice(self, rguid, outDevice, outer) : DIERR_GENERIC;

    if (SUCCEEDED(hr) && outDevice && *outDevice) {
        if (IsEqualGUID(rguid, GUID_SysKeyboard)) {
            DebugLogger::Log("DirectInput CreateDevice: GUID_SysKeyboard requested");
            TryHookKeyboardDeviceState(*outDevice);
        }
        else {
            TryHookDeviceState(*outDevice);
        }
    }

    // AFTER the GetDeviceState hook above, so it is the hook that saves the
    // real function -- never the probe's copy of the hook itself.
    ProbeOnCreateDevice("CreateDevice", &rguid, nullptr, hr, (SUCCEEDED(hr) && outDevice) ? *outDevice : nullptr);

    return hr;
}

static HRESULT STDMETHODCALLTYPE HookedCreateDeviceEx(LPVOID self, REFGUID rguid, REFIID riid, LPVOID* outDevice, LPUNKNOWN outer) {
    HRESULT vhr = DI_OK;
    if (VirtualPadCreate(rguid, &riid, outDevice, &vhr)) {
        return vhr;
    }

    HRESULT hr = g_OriginalCreateDeviceEx ? g_OriginalCreateDeviceEx(self, rguid, riid, outDevice, outer) : DIERR_GENERIC;

    if (SUCCEEDED(hr) && outDevice && *outDevice) {
        if (IsEqualGUID(rguid, GUID_SysKeyboard)) {
            DebugLogger::Log("DirectInput CreateDeviceEx: GUID_SysKeyboard requested");
            TryHookKeyboardDeviceState(*outDevice);
        }
        else {
            TryHookDeviceState(*outDevice);
        }
    }

    // AFTER the GetDeviceState hook above, so it is the hook that saves the
    // real function -- never the probe's copy of the hook itself.
    ProbeOnCreateDevice("CreateDeviceEx", &rguid, &riid, hr, (SUCCEEDED(hr) && outDevice) ? *outDevice : nullptr);

    return hr;
}

static void TryHookDirectInputInterface(LPVOID pInterface, REFIID riid) {
    if (!pInterface) {
        return;
    }

    if (!g_CreateDeviceHooked) {
        if (PatchComVTable(pInterface, 3, reinterpret_cast<void*>(&HookedCreateDevice), reinterpret_cast<void**>(&g_OriginalCreateDevice))) {
            g_CreateDeviceHooked = true;
            DebugLogger::Log("DirectInput CreateDevice hook installed");
        }
    }

    if (!g_CreateDeviceExHooked) {
        if (PatchComVTable(pInterface, 9, reinterpret_cast<void*>(&HookedCreateDeviceEx), reinterpret_cast<void**>(&g_OriginalCreateDeviceEx))) {
            g_CreateDeviceExHooked = true;
            DebugLogger::Log("DirectInput CreateDeviceEx hook installed");
        }
    }

    if (!(g_CreateDeviceHooked || g_CreateDeviceExHooked)) {
        DebugLogger::LogFormat("WARNING: Could not hook DirectInput interface methods for riid path");
    }

    ProbeHookInterfaceVtbl(pInterface);
}

static void EnsureRealDInputLoaded() {
    if (g_RealDInputModule) {
        return;
    }

    char sysPath[MAX_PATH] = { 0 };
    GetSystemDirectoryA(sysPath, MAX_PATH);
    std::string realDInputPath = std::string(sysPath) + "\\dinput.dll";

    g_RealDInputModule = LoadLibraryA(realDInputPath.c_str());
    if (!g_RealDInputModule) {
        return;
    }

    Original_DirectInputCreateA = (DirectInputCreateA_t)GetProcAddress(g_RealDInputModule, "DirectInputCreateA");
    Original_DirectInputCreateEx = (DirectInputCreateEx_t)GetProcAddress(g_RealDInputModule, "DirectInputCreateEx");
}

extern "C" HRESULT WINAPI DirectInputCreateA(HINSTANCE hinst, DWORD dwVersion, LPVOID* ppDI, LPUNKNOWN punkOuter) {
    EnsureRealDInputLoaded();

    if (Original_DirectInputCreateA) {
        HRESULT hr = Original_DirectInputCreateA(hinst, dwVersion, ppDI, punkOuter);
        // Logged only: the version-1 IDirectInputA vtable has no CreateDeviceEx
        // slot, so TryHookDirectInputInterface is not safe to run on it. If this
        // line shows up, the game opens devices through a path we do not hook.
        DebugLogger::LogFormat("[DIPROBE] DirectInputCreateA(version=0x%04lX) -> hr=0x%08lX interface=%p (NOT hooked)",
            dwVersion, (unsigned long)hr, (ppDI && SUCCEEDED(hr)) ? *ppDI : nullptr);
        return hr;
    }

    return E_FAIL;
}

extern "C" HRESULT WINAPI DirectInputCreateEx(HINSTANCE hinst, DWORD dwVersion, REFIID riid, LPVOID* ppvOut, LPUNKNOWN punkOuter) {
    EnsureRealDInputLoaded();

    if (Original_DirectInputCreateEx) {
        HRESULT hr = Original_DirectInputCreateEx(hinst, dwVersion, riid, ppvOut, punkOuter);
        {
            char name[64];
            ProbeGuidName(&riid, name, sizeof(name));
            DebugLogger::LogFormat("[DIPROBE] DirectInputCreateEx(version=0x%04lX, %s) -> hr=0x%08lX interface=%p",
                dwVersion, name, (unsigned long)hr, (ppvOut && SUCCEEDED(hr)) ? *ppvOut : nullptr);
        }
        if (SUCCEEDED(hr) && ppvOut && *ppvOut) {
            TryHookDirectInputInterface(*ppvOut, riid);
        }
        return hr;
    }

    return E_FAIL;
}

// Reference to the main VR functions declared in vr_injection.cpp
extern DWORD WINAPI VRMainThread(LPVOID lpReserved);
extern void ShutdownVrRuntime();

static void WriteBootMarker() {
    char exePath[MAX_PATH] = { 0 };
    GetModuleFileNameA(NULL, exePath, MAX_PATH);
    std::string dir(exePath);
    auto slash = dir.find_last_of("\\/");
    if (slash != std::string::npos) {
        dir = dir.substr(0, slash);
    }

    std::string markerPath = dir + "\\mgs1_vr_boot.txt";
    HANDLE hFile = CreateFileA(markerPath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        return;
    }

    const char* marker = "MGS1 VR DLL loaded\r\n";
    DWORD written = 0;
    WriteFile(hFile, marker, (DWORD)strlen(marker), &written, nullptr);
    CloseHandle(hFile);
}

// ===========================================================================
// CRASH REPORT (2026-10-05)
//
// The Psycho Mantis crash (knocking Meryl out) died with nothing in the log:
// Windows only said "mgsi.exe+61C7, access violation". This writes what is
// needed to name the cause the next time anything crashes:
//
//   mgs1_vr_crash.log  -- fault address as module+offset, registers, the code
//                         bytes, the EBP call chain, return addresses on the
//                         stack, which game actor was running, and the DG_OBJS
//                         pointers the mod was writing to (with any register
//                         that points into one of them flagged)
//   mgs1_vr_crash.dmp  -- a minidump with the game's data sections (the GV
//                         heaps and the DG queues live there), for offline
//                         inspection
//
// Two entry points, one report: a vectored handler sees a fault inside
// mgsi.exe first (before anything else can swallow or replace the crash
// path), and the unhandled-exception filter catches every other crash. The
// vectored handler never handles anything -- it writes the report once and
// lets the exception carry on exactly as before. Faults inside this DLL are
// left to the filter, because the mod's own SafeRead/SafeWrite fault on
// purpose under __try and those must not be reported.
// ===========================================================================
#include <dbghelp.h>
#include "../include/crash_report.h"

bool IsFpvActive();
bool IsGameplayPovActive();
bool IsCutscenePovDrawing();
uint32_t GetRenderHideObjs(int slot);

static volatile LONG g_crashVehReports = 0, g_crashUefWritten = 0;
static uintptr_t g_gameBase = 0, g_gameEnd = 0, g_modBase = 0, g_modEnd = 0;
static char g_crashDir[MAX_PATH] = { 0 };

static bool CrashRead(uintptr_t addr, void* out, size_t n) {
    SIZE_T got = 0;
    return addr && ReadProcessMemory(GetCurrentProcess(), (LPCVOID)addr, out, n, &got) && got == n;
}

static void CrashModuleRange(HMODULE h, uintptr_t* base, uintptr_t* end) {
    *base = (uintptr_t)h; *end = 0;
    if (!h) return;
    IMAGE_DOS_HEADER dos{};
    IMAGE_NT_HEADERS32 nt{};
    if (CrashRead(*base, &dos, sizeof(dos)) && dos.e_magic == IMAGE_DOS_SIGNATURE &&
        CrashRead(*base + dos.e_lfanew, &nt, sizeof(nt)) && nt.Signature == IMAGE_NT_SIGNATURE)
        *end = *base + nt.OptionalHeader.SizeOfImage;
}

// "mgsi.exe+61C7" style name for any address.
static void CrashAddrName(uintptr_t a, char* out, size_t cap) {
    HMODULE m = nullptr;
    if (a && GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                (LPCSTR)a, &m) && m) {
        char path[MAX_PATH] = { 0 };
        GetModuleFileNameA(m, path, MAX_PATH);
        const char* name = strrchr(path, '\\');
        name = name ? name + 1 : path;
        _snprintf_s(out, cap, _TRUNCATE, "%s+%X", name, (unsigned)(a - (uintptr_t)m));
    } else {
        _snprintf_s(out, cap, _TRUNCATE, "%08X", (unsigned)a);
    }
}

struct CrashOut {
    HANDLE f = INVALID_HANDLE_VALUE;
    void line(const char* fmt, ...) {
        if (f == INVALID_HANDLE_VALUE) return;
        char buf[1024];
        va_list ap;
        va_start(ap, fmt);
        int n = _vsnprintf_s(buf, sizeof(buf) - 2, _TRUNCATE, fmt, ap);
        va_end(ap);
        if (n < 0) n = (int)strlen(buf);
        buf[n++] = '\r'; buf[n++] = '\n';
        DWORD w = 0;
        WriteFile(f, buf, (DWORD)n, &w, nullptr);
    }
};

static const char* CrashCodeName(DWORD c) {
    switch (c) {
    case EXCEPTION_ACCESS_VIOLATION:      return "ACCESS VIOLATION";
    case EXCEPTION_ILLEGAL_INSTRUCTION:   return "ILLEGAL INSTRUCTION";
    case EXCEPTION_INT_DIVIDE_BY_ZERO:    return "INTEGER DIVIDE BY ZERO";
    case EXCEPTION_STACK_OVERFLOW:        return "STACK OVERFLOW";
    case EXCEPTION_PRIV_INSTRUCTION:      return "PRIVILEGED INSTRUCTION";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "ARRAY BOUNDS";
    case EXCEPTION_IN_PAGE_ERROR:         return "IN PAGE ERROR";
    case 0xC0000409:                      return "STACK BUFFER OVERRUN / FAST FAIL";
    case 0xC0000374:                      return "HEAP CORRUPTION";
    default:                              return "exception";
    }
}

// Which of the mod's DG_OBJS pointers (if any) does this value point into?
// A DG_OBJS is 0x48 bytes of header plus 0x5C per model, Snake has 16 models.
static const char* CrashWhose(uint32_t v, const MotionAimCrashInfo& s, uint32_t rh0, uint32_t rh1) {
    struct { uint32_t p; const char* what; } t[] = {
        { s.handsBodyObjs, "Snake's body (hands / full body)" },
        { s.unhidObjs,     "the body the hands unhide" },
        { s.renderHid0,    "render-hidden body (Snake's eyes)" },
        { s.renderHid1,    "render-hidden demo model" },
        { rh0,             "render-hide slot 0 candidate" },
        { rh1,             "render-hide slot 1 candidate" },
        { s.actRehidObjs,  "body re-hidden for this act" },
    };
    for (auto& e : t)
        if (e.p && v >= e.p && v < e.p + 0x48 + 40 * 0x5C) return e.what;
    return nullptr;
}

static void WriteCrashReport(EXCEPTION_POINTERS* ep, const char* via, bool withDump) {
    if (!ep || !ep->ExceptionRecord || !ep->ContextRecord) return;

    const EXCEPTION_RECORD& er = *ep->ExceptionRecord;
    const CONTEXT& c = *ep->ContextRecord;
    char path[MAX_PATH + 32];
    _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\mgs1_vr_crash.log", g_crashDir);
    CrashOut o;
    o.f = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

    SYSTEMTIME st{};
    GetLocalTime(&st);
    char where[160];
    CrashAddrName((uintptr_t)er.ExceptionAddress, where, sizeof(where));
    o.line("========================================");
    o.line("[%04d-%02d-%02d %02d:%02d:%02d.%03d] CRASH: %s (%08X) at %s -- via %s, thread %u",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
        CrashCodeName(er.ExceptionCode), (unsigned)er.ExceptionCode, where, via, (unsigned)GetCurrentThreadId());
    o.line("Build stamp: %s %s", __DATE__, __TIME__);
    if (er.ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er.NumberParameters >= 2) {
        const ULONG_PTR kind = er.ExceptionInformation[0];
        o.line("  tried to %s address %08X", kind == 0 ? "READ" : kind == 1 ? "WRITE" : "EXECUTE",
            (unsigned)er.ExceptionInformation[1]);
    }
#ifdef _M_IX86
    o.line("  EAX=%08X EBX=%08X ECX=%08X EDX=%08X ESI=%08X EDI=%08X",
        (unsigned)c.Eax, (unsigned)c.Ebx, (unsigned)c.Ecx, (unsigned)c.Edx, (unsigned)c.Esi, (unsigned)c.Edi);
    o.line("  EBP=%08X ESP=%08X EIP=%08X EFLAGS=%08X",
        (unsigned)c.Ebp, (unsigned)c.Esp, (unsigned)c.Eip, (unsigned)c.EFlags);
    {
        uint8_t code[16] = {};
        if (CrashRead(c.Eip, code, sizeof(code)))
            o.line("  code at EIP: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
                code[0], code[1], code[2], code[3], code[4], code[5], code[6], code[7],
                code[8], code[9], code[10], code[11], code[12], code[13], code[14], code[15]);
    }

    // --- what the mod was doing ----------------------------------------------
    MotionAimCrashInfo s{};
    MotionAimCrashSnapshot(&s);
    const uint32_t rh0 = GetRenderHideObjs(0), rh1 = GetRenderHideObjs(1);
    char actName[160] = "(between acts)";
    if (s.actFn) CrashAddrName(s.actFn, actName, sizeof(actName));
    char actPath[64] = { 0 };
    if (s.actWork) {
        uint32_t namePtr = 0;
        if (CrashRead(s.actWork + 0x14, &namePtr, 4) && namePtr) CrashRead(namePtr, actPath, sizeof(actPath) - 1);
        actPath[sizeof(actPath) - 1] = 0;
        for (char* p = actPath; *p; ++p) if ((unsigned char)*p < 0x20 || (unsigned char)*p > 0x7E) { *p = 0; break; }
    }
    o.line("Game actor running: act %s work %08X %s", actName, (unsigned)s.actWork, actPath);
    o.line("Mod state: fpv=%d gameplay Snake's eyes=%d cutscene Snake's eyes drawing=%d",
        IsFpvActive() ? 1 : 0, IsGameplayPovActive() ? 1 : 0, IsCutscenePovDrawing() ? 1 : 0);
    o.line("  hands: active=%d body DG_OBJS %08X (%d parts) confirmed %.0f ms ago | unhid %08X (set %.0f ms ago)",
        s.handsActive, (unsigned)s.handsBodyObjs, s.handsNParts, s.handsAgeMs, (unsigned)s.unhidObjs, s.unhidAgeMs);
    o.line("  render-only hides in force: %08X %08X | slot candidates %08X %08X | act re-hide %08X",
        (unsigned)s.renderHid0, (unsigned)s.renderHid1, (unsigned)rh0, (unsigned)rh1, (unsigned)s.actRehidObjs);
    {
        const struct { const char* n; uint32_t v; } regs[] = {
            { "EAX", c.Eax }, { "EBX", c.Ebx }, { "ECX", c.Ecx }, { "EDX", c.Edx },
            { "ESI", c.Esi }, { "EDI", c.Edi }, { "EBP", c.Ebp } };
        for (auto& r : regs)
            if (const char* w = CrashWhose(r.v, s, rh0, rh1))
                o.line("  !! %s=%08X points into %s", r.n, (unsigned)r.v, w);
    }

    // --- the DG render pass that crashed on 2026-10-04 (mgsi.exe+6168..61E4) --
    // Per queued DG_OBJS (bound_mode +0x32 set), per model (bound +0x4C set),
    // it clears every prim of packs[idx] (+0x54). Name the object it was on.
    if (g_gameBase && c.Eip >= g_gameBase + 0x6168 && c.Eip < g_gameBase + 0x61E5) {
        uint32_t cursor = 0, objs = 0, idx = 0;
        CrashRead(c.Ebp - 8, &cursor, 4);
        if (cursor) CrashRead(cursor - 4, &objs, 4);
        CrashRead(c.Ebp + 0xC, &idx, 4);
        uint32_t flag = 0, def = 0; int16_t nModels = 0, bound = 0;
        CrashRead(objs + 0x28, &flag, 4); CrashRead(objs + 0x24, &def, 4);
        CrashRead(objs + 0x2E, &nModels, 2); CrashRead(objs + 0x32, &bound, 2);
        o.line("DG prim-clear pass: buffer %u | DG_OBJS %08X flag=%08X def=%08X n_models=%d bound_mode=%d %s",
            (unsigned)idx, (unsigned)objs, (unsigned)flag, (unsigned)def, (int)nModels, (int)bound,
            CrashWhose(objs, s, rh0, rh1) ? "<- ONE OF THE MOD'S OBJECTS" : "");
        const uint32_t obj = (uint32_t)c.Ecx;
        int16_t ob = 0, np = 0; uint32_t ext = 0, p0 = 0, p1 = 0;
        CrashRead(obj + 0x4C, &ob, 2); CrashRead(obj + 0x52, &np, 2); CrashRead(obj + 0x48, &ext, 4);
        CrashRead(obj + 0x54, &p0, 4); CrashRead(obj + 0x58, &p1, 4);
        o.line("  model DG_OBJ %08X (index %d): bound=%d n_prims=%d extend=%08X packs[0]=%08X packs[1]=%08X",
            (unsigned)obj, objs ? (int)((obj - (objs + 0x48)) / 0x5C) : -1, (int)ob, (int)np,
            (unsigned)ext, (unsigned)p0, (unsigned)p1);
    }

    // --- call chain -------------------------------------------------------------
    {
        uint32_t ebp = c.Ebp;
        for (int i = 0; i < 20 && ebp; ++i) {
            uint32_t frame[2] = {};
            if (!CrashRead(ebp, frame, 8)) break;
            char n[160];
            CrashAddrName(frame[1], n, sizeof(n));
            o.line("  frame %2d: EBP %08X returns to %s", i, (unsigned)ebp, n);
            if (frame[0] <= ebp) break;
            ebp = frame[0];
        }
    }
    {
        uint32_t stack[192] = {};
        int shown = 0;
        if (CrashRead(c.Esp, stack, sizeof(stack))) {
            for (int i = 0; i < 192 && shown < 32; ++i) {
                const uint32_t v = stack[i];
                const bool inGame = v >= g_gameBase + 0x1000 && v < g_gameBase + 0x24A000;
                const bool inMod = g_modEnd && v >= g_modBase && v < g_modEnd;
                if (!inGame && !inMod) continue;
                char n[160];
                CrashAddrName(v, n, sizeof(n));
                o.line("  stack ESP+%03X: %s", i * 4, n);
                ++shown;
            }
        }
    }
#endif
    o.line(withDump ? "Minidump: mgs1_vr_crash.dmp (same folder)"
                    : "(first sight of the fault; if the game kept running, something handled it -- not a crash)");
    if (o.f != INVALID_HANDLE_VALUE) { FlushFileBuffers(o.f); CloseHandle(o.f); }
    DebugLogger::LogFormat("CRASH: %s at %s -- details in mgs1_vr_crash.log", CrashCodeName(er.ExceptionCode), where);

    // --- minidump ------------------------------------------------------------------
    if (!withDump) return;
    typedef BOOL(WINAPI* MiniDumpWriteDump_t)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE,
        PMINIDUMP_EXCEPTION_INFORMATION, PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);
    if (HMODULE dbg = LoadLibraryA("dbghelp.dll")) {
        auto write = (MiniDumpWriteDump_t)GetProcAddress(dbg, "MiniDumpWriteDump");
        char dmp[MAX_PATH + 32];
        _snprintf_s(dmp, sizeof(dmp), _TRUNCATE, "%s\\mgs1_vr_crash.dmp", g_crashDir);
        HANDLE df = CreateFileA(dmp, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (write && df != INVALID_HANDLE_VALUE) {
            MINIDUMP_EXCEPTION_INFORMATION mei{};
            mei.ThreadId = GetCurrentThreadId();
            mei.ExceptionPointers = ep;
            mei.ClientPointers = FALSE;
            write(GetCurrentProcess(), GetCurrentProcessId(), df,
                (MINIDUMP_TYPE)(MiniDumpWithDataSegs | MiniDumpWithIndirectlyReferencedMemory |
                                MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules),
                &mei, nullptr, nullptr);
        }
        if (df != INVALID_HANDLE_VALUE) CloseHandle(df);
    }
}

static LONG CALLBACK CrashVectoredHandler(EXCEPTION_POINTERS* ep) {
    if (!ep || !ep->ExceptionRecord) return EXCEPTION_CONTINUE_SEARCH;
    const DWORD code = ep->ExceptionRecord->ExceptionCode;
    const bool fatalKind = code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_ILLEGAL_INSTRUCTION ||
                           code == EXCEPTION_INT_DIVIDE_BY_ZERO || code == EXCEPTION_PRIV_INSTRUCTION ||
                           code == EXCEPTION_STACK_OVERFLOW;
    const uintptr_t at = (uintptr_t)ep->ExceptionRecord->ExceptionAddress;
    // Text only, at most 3 times: a fault the game (or a guarded call into the
    // game) handles itself must not use up the report for the real crash.
    if (fatalKind && g_gameEnd && at >= g_gameBase && at < g_gameEnd && InterlockedIncrement(&g_crashVehReports) <= 3)
        WriteCrashReport(ep, "vectored handler (fault inside the game's code)", false);
    return EXCEPTION_CONTINUE_SEARCH;   // never handled here: the game crashes exactly as before
}

static LONG WINAPI CrashUnhandledFilter(EXCEPTION_POINTERS* ep) {
    if (InterlockedExchange(&g_crashUefWritten, 1) == 0)
        WriteCrashReport(ep, "unhandled-exception filter -- the crash itself", true);
    return EXCEPTION_CONTINUE_SEARCH;   // let Windows Error Reporting carry on as before
}

static void InstallCrashReport(HMODULE self) {
    GetModuleFileNameA(nullptr, g_crashDir, MAX_PATH);
    if (char* s = strrchr(g_crashDir, '\\')) *s = 0;
    CrashModuleRange(GetModuleHandleA(nullptr), &g_gameBase, &g_gameEnd);
    CrashModuleRange(self, &g_modBase, &g_modEnd);
    AddVectoredExceptionHandler(1, CrashVectoredHandler);
    SetUnhandledExceptionFilter(CrashUnhandledFilter);
    DebugLogger::LogFormat("Crash report: armed (mgs1_vr_crash.log + mgs1_vr_crash.dmp on a crash) | game %08X-%08X",
        (unsigned)g_gameBase, (unsigned)g_gameEnd);
}

// 2. DLL Entry Point
BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    if (ul_reason_for_call == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);

        WriteBootMarker();

        // Initialize logging
        DebugLogger::Initialize();
        DebugLogger::Log("DLL_PROCESS_ATTACH: MGS1 VR Mod loading...");
#ifdef _DEBUG
        const char* buildConfig = "DEBUG (slow: switch Visual Studio to Release for play)";
#else
        const char* buildConfig = "Release";
#endif
        DebugLogger::LogFormat("Build stamp: %s %s | arch=%s | config=%s", __DATE__, __TIME__, (sizeof(void*) == 8) ? "x64" : "x86", buildConfig);
        DebugLogger::LogFormat("Log file: %s", DebugLogger::GetLogPath().c_str());
        InstallCrashReport(hModule);

        // Launch the OpenXR/MinHook injection on a separate thread.
        // We do this so we don't stall mgsi.exe's boot sequence.
        DebugLogger::Log("Creating VR injection thread...");
        CreateThread(nullptr, 0, VRMainThread, hModule, 0, nullptr);

        DebugLogger::Log("DLL_PROCESS_ATTACH: Completed");
    } else if (ul_reason_for_call == DLL_PROCESS_DETACH) {
        DebugLogger::Log("DLL_PROCESS_DETACH: Shutting down...");
        ShutdownVrRuntime();
        DebugLogger::Shutdown();
    }
    return TRUE;
}

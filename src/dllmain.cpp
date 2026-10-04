#include <windows.h>
#define INITGUID // so GUID_SysKeyboard (used below) is actually defined, not just declared
#include <dinput.h>
#include <xinput.h>
#include <string>
#include <cstring>
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

static HRESULT STDMETHODCALLTYPE HookedGetDeviceState(LPVOID self, DWORD cbData, LPVOID lpvData) {
    HRESULT hr = g_OriginalGetDeviceState ? g_OriginalGetDeviceState(self, cbData, lpvData) : DIERR_GENERIC;
    if (FAILED(hr) || !lpvData) {
        return hr;
    }

    XINPUT_STATE xi{};
    bool hasInjectedState = GetInjectedXInputState(&xi);
    if (!hasInjectedState && XInputGetState(0, &xi) != ERROR_SUCCESS) {
        static bool loggedPassthrough = false;
        if (!loggedPassthrough) {
            DebugLogger::Log("DirectInput mapper passthrough: no OpenXR virtual pad and no physical XInput pad; preserving native DI state");
            loggedPassthrough = true;
        }
        return hr;
    }

    static bool loggedInjected = false;
    static bool loggedPhysical = false;
    if (hasInjectedState && !loggedInjected) {
        DebugLogger::Log("DirectInput state mapper active: feeding legacy DI state from OpenXR virtual gamepad");
        loggedInjected = true;
    }
    if (!hasInjectedState && !loggedPhysical) {
        DebugLogger::Log("DirectInput state mapper active: feeding legacy DI state from physical XInput controller 0");
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

    return hr;
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

    return hr;
}

static HRESULT STDMETHODCALLTYPE HookedCreateDeviceEx(LPVOID self, REFGUID rguid, REFIID riid, LPVOID* outDevice, LPUNKNOWN outer) {
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
        return Original_DirectInputCreateA(hinst, dwVersion, ppDI, punkOuter);
    }

    return E_FAIL;
}

extern "C" HRESULT WINAPI DirectInputCreateEx(HINSTANCE hinst, DWORD dwVersion, REFIID riid, LPVOID* ppvOut, LPUNKNOWN punkOuter) {
    EnsureRealDInputLoaded();

    if (Original_DirectInputCreateEx) {
        HRESULT hr = Original_DirectInputCreateEx(hinst, dwVersion, riid, ppvOut, punkOuter);
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

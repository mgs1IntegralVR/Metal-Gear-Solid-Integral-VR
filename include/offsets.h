#pragma once
#include <cstdint>

// ========================================================================
// MGS1 (mgsi.exe) Memory Offsets & Signatures
// ========================================================================
namespace MGSConfig {
    // Base address offset expectations for the static GOG executable payload
    constexpr uintptr_t EXE_BASE = 0x00400000;

    // Function hooks
    constexpr uintptr_t FUNC_RENDER_FRAME = 0x0045A120; // Master frame dispatcher
    constexpr uintptr_t FUNC_CAMERA_UPDATE = 0x0048B500; // Camera calculation routine

    // Static global memory pointers (Fallback references if pointer chains shift)
    constexpr uintptr_t ADDR_GLOBAL_CAMERA = 0x006ABC10;
    constexpr uintptr_t ADDR_GAME_STATUS = 0x007C1204; // Bitmask for cutscenes / demo mode
}
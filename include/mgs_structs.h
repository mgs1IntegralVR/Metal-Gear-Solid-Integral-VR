
#pragma once
#include <cstdint>

#pragma pack(push, 1)

// MGS1 / PSX Vector representation (Fixed-point millimeters)
struct MGS_SVECTOR {
    int32_t x;
    int32_t y;
    int32_t z;
};

// MGS1 / PSX Rotation representation (12-bit rotational units: 0 to 4095)
struct MGS_SROTATION {
    int16_t pitch;
    int16_t yaw;
    int16_t roll;
    int16_t pad; // Alignment padding for structural sizing
};

// Core Camera Actor Layout (Based on Konami's internal camera struct design)
struct MGS_CameraActor {
    int32_t       field_0x00;
    int32_t       actorState;
    MGS_SVECTOR   position;      // World position coordinates
    MGS_SROTATION rotation;      // View angles
    int32_t       screenDistance;// PSX screen distance 'h' (acts as FOV scalar)
};

#pragma pack(pop)
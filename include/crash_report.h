#pragma once
#include <cstdint>

// Crash report (2026-10-05). dllmain.cpp installs the handler; motion_aim.cpp
// fills in the DG_OBJS pointers it writes to, so a crash inside the game's
// renderer can be checked against them. Plain values only: this is read from
// inside an exception handler.
struct MotionAimCrashInfo {
    uint32_t actFn, actWork;            // game actor whose act was running (0 = between acts)
    int      handsActive;
    uint32_t handsBodyObjs;             // Snake's body DG_OBJS the hands draw on
    int      handsNParts;
    double   handsAgeMs;                // since Snake's act last confirmed it (-1 never)
    uint32_t unhidObjs;                 // body whose hidden bit the hands clear
    double   unhidAgeMs;                // since that pointer was last set (-1 never)
    uint32_t renderHid0, renderHid1;    // render-only hides in force right now
    uint32_t actRehidObjs;              // body re-hidden for the act in progress
};

extern "C" void MotionAimCrashSnapshot(MotionAimCrashInfo* out);

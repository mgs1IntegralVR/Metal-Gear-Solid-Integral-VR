#pragma once
#include <cstdint>

// ---------------------------------------------------------------------------
// RENDER TWICE -- true stereo, both eyes from the same game frame.
//
// MGS1 builds its 3D scene in two halves (FoxdieTeam decomp, libdg/):
//
//   DG_EndFrame   (last actor of frame N)   DG_SortChanlSystem(GV_Clock)
//                 runs the 7 channel units (screen, bound, trans, shade, prim,
//                 divide, sort) over DG_Chanls[1], transforming every queued
//                 object with the channel's eye matrix into packets + an OT.
//   DG_StartFrame (first actor of N+1)      PutDispEnv  -> present (Flip)
//                                           DG_DrawChanlSystem(slot N)
//                                           -> DrawOTag: draw that OT into
//                                              the back buffer, synchronously.
//
// PC addresses (mgsi.exe, static disassembly 2026-09-24):
//   DG_EndFrame          +1C15   push [GV_Clock] / call +171C
//   DG_SortChanlSystem   +171C   7 units from the table at +2500E0
//   DG_DrawChanlSystem   +1619   DrawOTag(&DG_Chanls[0].env1[which])
//   DrawOTag             +103B0
//   DG_Chanls[1]         +2BC36C (eye_inv +0x10, eye +0x30, ot[2] +0x00)
//   GV_Clock             +391A08
//
// So a second eye costs: restore the OT to its pre-sort state, move the
// channel's eye matrix sideways by one IPD, re-run the sort, and DrawOTag
// again. Each image is read back from the back buffer straight after its own
// draw and published to the headset as a true left/right pair.
//
// The game's own camera code is untouched: the camera hooks stop alternating
// the eye (IsRenderTwiceLive()) and render from the head centre; the eye
// offset is applied here, on the matrix, for both eyes.
// ---------------------------------------------------------------------------

// Call once, after MH_Initialize(). Signature-checks every site and installs
// nothing unless all of them match.
bool InstallRenderTwiceHooks();

// True while render-twice has produced a stereo pair recently (~150 ms). The
// camera hooks use it to stop their own alternate-eye offset, so the eye
// separation is never applied twice. False -> alternate-eye stereo resumes
// on its own.
bool IsRenderTwiceLive();

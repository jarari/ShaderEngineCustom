#pragma once

#include <atomic>
#include <cstdint>

// HiZCull — Hi-Z-based occlusion culling for the BSGeometry::OnVisible path.
//
// What it hooks (one engine hook, no JMP14):
//   `BSGeometry::OnVisible @ 0x141bb11e0` (OG 1.10.163), vtable slot 55
//   (NiAVObject_vtbl::OnVisible at byte offset 440, confirmed via PDB).
//   Vtable base for BSGeometry = 0x141bb1830.
//
// Why this hook site:
//   `OnVisible` is the *post-frustum-cull, pre-pass-enrolment* gate. When
//   `BSCullingProcess::Process` (from `BSShaderUtil::AccumulateSceneArray ×3`
//   inside `DrawWorld::MainAccum`) passes the frustum test on a BSGeometry,
//   it dispatches through this vtable slot to enrol the geometry into the
//   active BSShaderAccumulator's pass lists. Returning early here causes the
//   geometry to be silently skipped — engine state stays self-consistent
//   because we're inserting at the existing polymorphic dispatch point.
//
//   The other cull path (`BSCullingGroup::Process` + `AccumulatePasses`,
//   walking BSCuller arenas) bypasses this vtable; that's a v2 target —
//   see notes in HiZCull.cpp.
//
// What "occluded" means here:
//   Hi-Z pyramid built from previous frame's main depth.  For a BSGeometry's
//   world-space bound sphere we (a) project the center to view space, (b)
//   compute screen-space AABB radius from radius / viewZ, (c) pick the Hi-Z
//   mip whose texel covers that AABB, (d) sample the mip (max of four
//   surrounding texels), (e) compare against the sphere's *nearest* clip-space
//   Z. Occluded ⇔ sphere's near Z is *behind* the farthest visible depth in
//   the region (i.e. every pixel in the region is occupied by something
//   closer than my object's nearest point).
//
// Modes:
//   off     — vtable not patched; zero per-call cost.
//   measure — vtable patched, original always called. Counts OnVisible call
//             rate, unique geometry pointers, would-cull rate (for tuning).
//             Use this to decide whether `cull` mode is worth turning on.
//   cull    — vtable patched, occluded geometries are skipped. Hi-Z resources
//             are lazily allocated on first frame.
//
// Per-frame latency:
//   Hi-Z is built from the *current* main depth at MainAccum entry. The depth
//   target at that moment holds frame N-1's depth (this frame's G-buffer pass
//   hasn't run yet). So we cull frame N against frame N-1's occluders — the
//   standard 1-frame Hi-Z staleness. Combined with the GPU→CPU readback ring
//   buffer (2 frames), the effective staleness is 2 frames. Acceptable for
//   gameplay; insufficient for cinematic camera cuts (mitigated by the
//   `g_invalidateThisFrame` flag, which forces every geometry to pass).
//
// What ISN'T done in v1:
//   - BSCullingGroup::Process / BSCuller-arena path (see file-top comment in
//     HiZCull.cpp). Roughly 60-70% of OnVisible-equivalent enrolments per
//     frame likely flow through that path. Measure mode reports both rates so
//     we can quantify the gap.
//   - Shadow cascade Hi-Z (Phase 5 in FO4PerfPipelineRE.md §17.7).
//   - Guard band / first-frame-visible override. Falsely culled-this-frame
//     geometries appear next frame.
//   - Skinned / fading / particle special-casing. The vtable patch hits all
//     BSGeometry subclasses; visual artifacts on those may need exclusions.
//
// INI gate: `HIZ_CULL_MODE = off | measure | cull`. Default off.

namespace HiZCull {

enum class Mode : std::uint8_t {
    Off     = 0,
    Measure = 1,
    Cull    = 2,
};

extern std::atomic<Mode> g_mode;

// Install the BSGeometry vtable patch (idempotent). Returns true on success.
// Called from main.cpp's plugin-load path BEFORE the renderer warms up — the
// patch is a memory write and does not touch D3D11 state.
bool Initialize();

// Releases GPU resources. Called from F4SEPlugin_Release.
void Shutdown();

// Called from PhaseTelemetry's HookedMainAccum at the start of every
// DrawWorld::MainAccum invocation. Builds Hi-Z from current main depth,
// snapshots the world camera VP matrix, and rotates the readback ring.
// Cheap when Mode == Off.
void OnMainAccumEnter();

// Called from PhaseTelemetry's HookedMainAccum immediately after the
// original MainAccum returns. Clears the "inside main accum" gate so
// OnVisible calls from non-world contexts (shadow path, 3D UI) become
// passthrough — they shouldn't be culled against the world camera's Hi-Z.
void OnMainAccumExit();

// Force every BSGeometry visible for the next N frames (used on camera cut /
// teleport to avoid popping when the Hi-Z is from a stale viewpoint).
// Implementation reserves this for future work; v1 always treats Hi-Z as
// valid. Exposed for completeness.
void InvalidateForFrames(unsigned n);

}  // namespace HiZCull

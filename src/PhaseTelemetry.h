#pragma once

#include <atomic>
#include <cstdint>

// PhaseTelemetry — per-phase timing + draw-count instrumentation for the
// G-buffer pass.
//
// `FinishAccumulating_Standard_PreResolveDepth` (and its Post sibling) is a
// flat sequence of ~10 sub-calls. RenderDoc shows the parent costs ~16 ms in
// Boston but there is no per-sub-call breakdown anywhere — we don't know
// whether RenderOpaqueDecals owns 5 ms or 0.5 ms. That's the question this
// module answers.
//
// What we measure, per logging window, per `{Pre|Post}` × `{subPhase}`:
//   - call count
//   - draw count (via OnDraw, called from HookedBSBatchRendererDraw)
//   - total ns
//   - max ns (single-call worst case)
//
// Sub-phases (mapped to known hook targets):
//   - OpaqueDecals      — BSShaderAccumulator::RenderOpaqueDecals
//   - AllBatches        — BSShaderAccumulator::RenderAllBatches (its inner
//                          13-group loop fires through BSBatchRenderer; all
//                          attribute here)
//   - BlendedDecals     — BSShaderAccumulator::RenderBlendedDecals
//   - SortAlphaPasses   — BSBatchRenderer::SortAlphaPasses
//   - GG[g]             — BSShaderAccumulator::RenderGeometryGroup, binned by
//                          the `group` argument (0..19)
//   - <total>           — the outer Pre/Post wrapper's own wall time
//   - <unaccounted>     — computed at log time as total − Σ children. Picks up
//                          the direct `BSBatchRenderer::RenderBatches(group=7)`
//                          call inside PreResolveDepth and any inlined helpers
//                          we don't separately hook.
//
// Each child hook checks the TLS phase counter and only attributes if invoked
// from within Pre or Post. Sub-functions called from outside (e.g. the
// shadow path, immediate-mode menu rendering) are passed through to the
// original without telemetry.
//
// INI gate: `PHASE_TELEMETRY_MODE = off | on`. Default off — zero impact.

namespace PhaseTelemetry {

enum class Mode {
    Off,
    On,
};

extern std::atomic<Mode> g_mode;

// Force-install the DrawWorld wrappers even when telemetry logging is off.
// Rendering-side features use these wrappers as cheap phase context only.
void RequireHooks();

// Cheap render-thread phase predicates used by renderer hooks.
bool IsInRenderPreUI();
bool IsInDeferredPrePass();

// Called from Plugin.cpp's HookedBSBatchRendererDraw. Cheap when mode==Off
// (single relaxed atomic load + branch). Attributes a draw to the currently
// active sub-phase bucket (or to the Frame total if just inside Render_PreUI
// but outside any tracked sub-phase).
void OnDraw();

// Called from the D3D11 immediate-context Draw* hooks. This is the ground-truth
// API draw-call counter; OnDraw is the higher-level BSBatchRenderer counter.
void OnD3DDraw();

// Called from BSBatchRenderer::RenderCommandBufferPassesImpl. This captures
// Bethesda's pre-recorded command-buffer draw path, which bypasses
// BSBatchRenderer::Draw and may also bypass our immediate-context hooks.
void OnCommandBufferDraw();

// Install the DrawWorld:: hooks. Hooks are installed if PHASE_TELEMETRY_MODE is
// `on` OR if any piggy-back consumer (LightSorter) is enabled — see
// PhaseTelemetry.cpp::Initialize. Safe to call multiple times. Returns false
// if any individual hook install fails; partial-install state is logged.
bool Initialize();

}  // namespace PhaseTelemetry

#pragma once

#include <atomic>
#include <cstdint>

// InstancingProbe — measures the per-frame distribution of (shader, material,
// geometry) tuples at the BSBatchRenderer::Draw level so we can decide
// whether the engine's 4150 G-buffer draws contain enough same-mesh runs to
// be worth auto-instancing through the existing BSInstanceGroupPass path.
//
// This is the FIRST phase of "Path A". Phase 2 (the actual instancing
// coalesce) only makes sense if the data here shows a meaningful fraction of
// consecutive draws that could be merged.
//
// What we measure (per draw, accumulated per logging window):
//   - Total draws seen.
//   - Times shader unchanged from previous draw.
//   - Times shader AND material unchanged from previous draw.
//   - Times shader AND material AND geometry unchanged ("instance candidate"
//     — same mesh redrawn back-to-back, almost certainly with a different
//     transform; this is the win bucket).
//   - Run-length stats for the instance-candidate bucket: total run-length
//     accumulated across the window, peak run length seen.
//
// Implementation: no hooks of our own — Plugin.cpp's existing
// HookedBSBatchRendererDraw calls InstancingProbe::OnDraw(pass) at the top of
// every per-pass draw. When mode==Off, OnDraw bails after a single relaxed
// atomic load.
//
// Mode is INI-gated via INSTANCING_PROBE_MODE in ShaderEngine.ini:
//   off  - no work (default)
//   on   - record and log every kLogIntervalCalls draws / kLogIntervalSecs
//          seconds, whichever comes first.
namespace InstancingProbe {

enum class Mode {
    Off,
    On,
};

extern std::atomic<Mode> g_mode;

// Called from Plugin.cpp's HookedBSBatchRendererDraw before the original.
// Cheap when mode==Off (single relaxed atomic load + branch). The argument
// is an opaque pointer to BSRenderPassLayout — that struct lives in
// Plugin.cpp's anonymous namespace so we take const void* and byte-grab
// the three offsets we need (offsets 0x08/0x10/0x18; verified by the
// static_asserts in Plugin.cpp).
void OnDraw(const void* pass);

// Lifecycle (mirrors LightCullPolicy / ShadowCullCache pattern).
void Initialize();

}  // namespace InstancingProbe

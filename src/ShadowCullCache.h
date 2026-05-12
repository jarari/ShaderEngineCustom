#pragma once

#include <atomic>
#include <cstdint>

// ShadowCullCache — instruments per-frame shadow-scene work to find and
// eventually cache the expensive cull/submit paths that produce the 8.3ms
// shadow atlas pass in our Boston RenderDoc capture (2280 draws across 5
// shadow viewports; sun cascade 0 alone = 1905 draws).
//
// First attempt hooked ShadowSceneNode::OnVisible @ 0x14280eda0 thinking it
// was the per-frame cull walker. It is NOT: that function's address only
// appears in ONE dispatch table at 0x143095310 and is not invoked by any
// active per-frame code path in worldspace. The hook installed cleanly but
// never fired during gameplay.
//
// Current target: ShadowSceneNode::ProcessAllQueuedLights (static, OG @
// 0x1428104d0, REL::ID 1421093). It IS on the per-frame path —
// DrawWorld::LightUpdate @ 0x14284ddd0 (the world-render light-update
// entry) calls it unconditionally once per frame. It loops the 5
// BSShaderManager::St shadow nodes applying queued AddQueuedLight /
// RemoveQueuedLight ops and calling UpdateLightList. This is NOT itself
// the heavy cull cost — that's deeper, likely in UpdateLightList and the
// BSShaderUtil::AccumulateScene / RenderSceneDeferred chain called from
// the renderer. We hook here first as a "proof of life" + camera-coherence
// measurement waypoint; once we confirm telemetry, we'll move the hook to
// the real cull cost site (TBD via callchain mapping).
//
// Phases (only phase 1 ships in this commit):
//
//   1. Measure  — wraps the original, computes an input-state hash, and
//                 records how often that hash would repeat frame-to-frame
//                 (both exact and within camera-motion tolerance), plus how
//                 long each call takes. No behavior change.
//
//   2. Cache    — TBD. On exact hash match, skip the original by restoring
//                 a snapshot of the mutations OnVisible would have made
//                 (shared-map state, recursive NiAVObject::Cull effects).
//
//   3. Tolerance cache — TBD. Reuse Phase 2 snapshot when camera/sun motion
//                 is within a configured epsilon.
//
// Self-contained: owns its REL::Relocation, gateway template, hook function,
// and INI gate. Plugin.cpp does not need to know about the hook details.
// Lifecycle mirrors LightCullPolicy / LightTracker — Initialize() at plugin
// load reads the INI gate and patches if enabled; Shutdown() is a no-op (the
// gateway/trampoline live for the lifetime of the process).
//
// Default INI mode is "off" so the hook is never installed on default builds.
namespace ShadowCullCache {

enum class Mode {
    Off,      // Hook not installed. Zero impact.
    Measure,  // Hook installed; calls original; records hash/timing telemetry.
    Cache,    // Hook installed; on combined-match (content hash + camera within
              // tolerance) returns WITHOUT calling the original — the previous
              // frame's atlas slice persists on the GPU. RISK: if the engine
              // clears the shadow depth target upstream of this function
              // (before calling it), skipping leaves an empty slice and
              // shadows visibly disappear that frame. Toggle this on briefly
              // first to verify visual correctness before relying on it.
};

// Resolved from ShaderEngine.ini key SHADOW_CULL_CACHE_MODE (off/measure).
// Defaults to Off. Read once during Initialize().
extern std::atomic<Mode> g_mode;

// Periodic stats snapshot. Reset each time the periodic log fires so the
// reported numbers describe the last logging window, not the whole session.
struct WindowStats {
    std::uint64_t calls            = 0;  // total OnVisible invocations
    std::uint64_t exactHashHits    = 0;  // would have hit a strict-match cache
    std::uint64_t loosePosHits     = 0;  // camera pos delta < kLoosePosEpsilon
    std::uint64_t looseRotHits     = 0;  // camera rot delta within kLooseRotDotEpsilon
    double        totalMicros      = 0.0;
    double        maxMicros        = 0.0;
};

// Install the hook if mode != Off. Safe to call multiple times; subsequent
// calls are no-ops. Returns false only if the INI requested install but the
// gateway patch failed; logs the reason via REX::WARN.
bool Initialize();

// Reads the latest snapshot atomically. Intended for a future debug HUD.
WindowStats GetStats();

}  // namespace ShadowCullCache

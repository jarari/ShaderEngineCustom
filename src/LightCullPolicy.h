#pragma once

#include <atomic>
#include <cmath>
#include <limits>

// Per-shader-rule cull-bound scaler. Hooks BSLight::TestFrustumCull (engine
// function — not D3D11 vtable, so the ENB/ReShade Draw-wrap problem that
// LightTracker works around does not apply here). On entry, walks the active
// ShaderDefinitions; if any active rule with a compiled replacement declares
// a `lightCullRadiusScaleValue`, multiplies the BSLight's bound radius
// (geometry[+0x138]) by that float's current value before letting the engine's
// cull run, then restores the source field on the way out. The engine's cached
// cull sphere at geometry[+0xBC] is deliberately LEFT at the scaled value so
// downstream consumers see the boosted reach.
//
// Module shape mirrors LightTracker.{h,cpp}: atomic gate, inline header entry,
// slow path in the .cpp. Lifecycle is Initialize() at plugin load and
// Shutdown() at plugin release; no per-frame Tick needed because cull is
// event-driven.
namespace LightCullPolicy {

// Sentinel returned by OnTestFrustumCullEnter when no scaling was applied
// (no active rule, scale ≈ 1.0, or geometry pointer missing). OnLeave checks
// this with isnan and short-circuits.
constexpr float kNoScaleSaved = std::numeric_limits<float>::quiet_NaN();

// True between Initialize and Shutdown. Hot path bails out cheaply when off.
extern std::atomic<bool> g_isActive;

void Initialize();
void Shutdown();

// Resolve the currently-active radius scale. Walks g_shaderDefinitions for
// the first PS rule that is active=true, has a compiled replacement PS, and
// declares a lightCullRadiusScaleValue that resolves to either a ShaderValue
// float or a GpuScalar probe. Returns 1.0 when no rule is in effect, so
// callers can unconditionally multiply by the return without a guard.
float GetCurrentScale();

// Slow paths — see LightCullPolicy.cpp.
float OnTestFrustumCullEnterImpl(void* light);
void  OnTestFrustumCullLeaveImpl(void* light, float savedRadius);

// Inline fast paths called from HookedBSLightTestFrustumCull in Plugin.cpp.
// The Enter call returns the vanilla radius that should be passed back to
// Leave so the source field can be restored after the engine's cull runs.
// NaN means "no scaling was applied; Leave is a no-op".
inline float OnTestFrustumCullEnter(void* light)
{
    if (g_isActive.load(std::memory_order_relaxed)) {
        return OnTestFrustumCullEnterImpl(light);
    }
    return kNoScaleSaved;
}

inline void OnTestFrustumCullLeave(void* light, float savedRadius)
{
    if (!std::isnan(savedRadius)) {
        OnTestFrustumCullLeaveImpl(light, savedRadius);
    }
}

}  // namespace LightCullPolicy

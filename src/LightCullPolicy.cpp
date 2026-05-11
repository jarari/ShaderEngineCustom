#include <PCH.h>
#include "LightCullPolicy.h"

#include <Global.h>
#include <GpuScalar.h>
#include <Plugin.h>

namespace LightCullPolicy {

namespace {

// Verified via IDA against Fallout4.exe OG 1.10.163:
//   BSLight::TestFrustumCull at 0x14285ACB0 reads the source bound radius
//   from `*(float*)([BSLight + 0xB8] + 0x138)` (instruction 0x14285AE41)
//   and copies it into the cached cull sphere at +0xBC (0x14285AE67).
// See TiledLightingReconstruction.md and the disassembly captured in chat.
constexpr std::size_t kBSLightGeometryOffset     = 0xB8;
constexpr std::size_t kGeometryBoundRadiusOffset = 0x138;

float* GetBoundRadiusPtr(void* light)
{
    if (!light) {
        return nullptr;
    }
    auto* geometry = *reinterpret_cast<void**>(
        static_cast<std::byte*>(light) + kBSLightGeometryOffset);
    if (!geometry) {
        return nullptr;
    }
    return reinterpret_cast<float*>(
        static_cast<std::byte*>(geometry) + kGeometryBoundRadiusOffset);
}

// Look up a Float ShaderValue by id. Walks g_shaderSettings.GetFloatShaderValues();
// the vector is small (one entry per Values.ini float across all loaded mods),
// so linear search is fine. Returns nullptr if not found or wrong type.
ShaderValue* ResolveFloatValueByName(const std::string& id)
{
    if (id.empty()) {
        return nullptr;
    }
    auto& values = g_shaderSettings.GetFloatShaderValues();
    for (auto* v : values) {
        if (v && v->type == ShaderValue::Type::Float && v->id == id) {
            return v;
        }
    }
    return nullptr;
}

// Walk active shader definitions and return the *first* float scale that's
// currently in effect, i.e. a rule with active=true, type=Pixel, a non-empty
// lightCullRadiusScaleValue, AND a compiled replacement pixel shader. The
// "compiled replacement" check is what makes the policy follow the rule's
// actual effect: a rule that's loaded but hasn't compiled (or that failed to
// compile) doesn't actually replace the engine's PS, so its declared scale
// shouldn't take effect either.
//
// Lazily resolves the value name on first hit and caches the result on the
// ShaderDefinition. The lookup checks the ShaderValue pool first (Values.ini
// floats — they're the cheap zero-lag path) and falls back to the GpuScalar
// pool (HLSL function probes — they have a 1-frame readback lag). If neither
// matches yet (Values.ini still loading or probe not yet registered), the
// rule is skipped and resolution retries on the next cull call.
//
// Returns 1.0f when no rule is currently in effect — caller treats that as
// "no scaling".
float FindActiveScale()
{
    std::shared_lock lock(g_shaderDefinitions.mutex);
    for (auto* def : g_shaderDefinitions.definitions) {
        if (!def || !def->active) continue;
        if (def->type != ShaderType::Pixel) continue;
        if (def->lightCullRadiusScaleValue.empty()) continue;
        // Only count the rule as "in effect" if its replacement PS got past
        // CreatePixelShader successfully. A rule whose replacement is still
        // pending (or failed to compile) is dormant — we don't want to scale
        // the cull bound for lights whose PS is still the engine's vanilla.
        if (def->loadedPixelShader == nullptr) continue;
        // Lazy resolve. We mutate fields on the definition under a shared_lock:
        // safe because shared_lock protects the vector structure, not the
        // elements, and the writes are idempotent (multiple threads write the
        // same resolved pointer or the same logged-flag value).
        if (!def->lightCullRadiusScaleResolved && !def->lightCullRadiusScaleGpuRef) {
            if (auto* sv = ResolveFloatValueByName(def->lightCullRadiusScaleValue)) {
                def->lightCullRadiusScaleResolved = sv;
                REX::INFO("LightCullPolicy: definition '{}' bound to Values.ini float '{}'",
                          def->id, def->lightCullRadiusScaleValue);
            } else if (auto* gpu = GpuScalar::GetValuePtr(def->lightCullRadiusScaleValue)) {
                def->lightCullRadiusScaleGpuRef = gpu;
                REX::INFO("LightCullPolicy: definition '{}' bound to gpuScalar probe '{}'",
                          def->id, def->lightCullRadiusScaleValue);
            } else {
                if (!def->lightCullRadiusScaleResolveLogged) {
                    REX::WARN("LightCullPolicy: definition '{}' references '{}' — no matching "
                              "Values.ini float or [gpuScalar:NAME] block; cull scaling skipped",
                              def->id, def->lightCullRadiusScaleValue);
                    def->lightCullRadiusScaleResolveLogged = true;
                }
                continue;
            }
        }
        if (def->lightCullRadiusScaleResolved) {
            return def->lightCullRadiusScaleResolved->current.f;
        }
        if (def->lightCullRadiusScaleGpuRef) {
            return *def->lightCullRadiusScaleGpuRef;
        }
    }
    return 1.0f;
}

}  // anonymous namespace

std::atomic<bool> g_isActive{ false };

void Initialize()
{
    g_isActive.store(true, std::memory_order_release);
    REX::INFO("LightCullPolicy: initialized — scaling active when any rule "
              "declares lightCullRadiusScaleValue and has a compiled replacement");
}

void Shutdown()
{
    g_isActive.store(false, std::memory_order_release);
}

float GetCurrentScale()
{
    if (!g_isActive.load(std::memory_order_relaxed)) return 1.0f;
    return FindActiveScale();
}

float OnTestFrustumCullEnterImpl(void* light)
{
    const float scale = FindActiveScale();
    // Treat scale very close to 1.0 as a no-op so we don't pointlessly
    // touch the field. Non-positive scale would zero or invert the cull
    // bound — reject as a misconfiguration. Non-finite (NaN/Inf from a
    // misbehaving HLSL probe) — likewise reject.
    if (!std::isfinite(scale) || scale <= 0.0f || std::abs(scale - 1.0f) < 1e-4f) {
        return kNoScaleSaved;
    }
    float* radiusPtr = GetBoundRadiusPtr(light);
    if (!radiusPtr) {
        return kNoScaleSaved;
    }
    // Diagnostic: log the FIRST time we successfully apply a scale, and
    // periodically thereafter.
    static std::atomic<std::uint64_t> s_appliedCalls{ 0 };
    const auto callIdx = s_appliedCalls.fetch_add(1, std::memory_order_relaxed);
    if (callIdx == 0 || (callIdx % 30000) == 0) {
        REX::INFO("LightCullPolicy: applying scale={:.4f} (cull call #{})", scale, callIdx);
    }
    const float saved = *radiusPtr;
    *radiusPtr = saved * scale;
    return saved;
}

void OnTestFrustumCullLeaveImpl(void* light, float savedRadius)
{
    // Restore the vanilla radius so shadow projection, fade-distance, and any
    // other consumer that reads geometry[+0x138] outside of our cull / volume-
    // mesh hook windows sees the engine's original value. The cached cull
    // sphere at geometry[+0xBC] stays scaled because TestFrustumCull wrote
    // it from the scaled source during its run; that's the only place we
    // *want* the scale to persist, since downstream cull-sphere consumers
    // benefit from the boosted reach.
    if (float* radiusPtr = GetBoundRadiusPtr(light)) {
        *radiusPtr = savedRadius;
    }
}

}  // namespace LightCullPolicy

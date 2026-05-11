#include <PCH.h>
#include "GpuScalar.h"

#include <Global.h>
#include <Plugin.h>

#include <array>
#include <atomic>
#include <format>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <vector>

#include <d3d11.h>
#include <d3dcompiler.h>

// Same forward decls CustomPass.cpp uses for the small INI helpers defined in
// main.cpp's TU. Avoids dragging main.cpp's static helpers into a header.
extern std::string RemoveInlineComment(const std::string& line);
extern std::pair<std::string, std::string> GetKeyValueFromLine(const std::string& line);

namespace GpuScalar {

namespace {

inline std::string RemoveAllWS(std::string s)
{
    s.erase(std::remove_if(s.begin(), s.end(),
        [](unsigned char c) { return std::isspace(c); }), s.end());
    return s;
}

constexpr UINT kReadbackRingSize = 2;  // 1-frame lag; bump to 3 for more headroom.

struct Probe {
    Spec spec;
    // GPU state — created lazily on first OnFramePresent. nullptr means
    // "not yet attempted" (compile is retried up to kCompileRetryLimit times,
    // then we latch failed). compileFailed=true means "do not retry".
    REX::W32::ID3D11ComputeShader*       cs        = nullptr;
    REX::W32::ID3D11Buffer*              outputBuf = nullptr;
    REX::W32::ID3D11UnorderedAccessView* uav       = nullptr;
    std::array<REX::W32::ID3D11Buffer*, kReadbackRingSize> staging{};
    UINT          writeIdx       = 0;
    bool          compileFailed  = false;
    int           compileAttempts = 0;
    // Stable storage for the readback value. Initialized to 1.0 so consumers
    // that cache the pointer see a sensible default before the first
    // successful readback completes.
    float         value = 1.0f;
    bool          valid = false;     // true once we've successfully mapped at least once
    std::uint64_t framesDispatched = 0;

    void Release()
    {
        if (uav)       { uav->Release();       uav = nullptr; }
        if (outputBuf) { outputBuf->Release(); outputBuf = nullptr; }
        for (auto& s : staging) {
            if (s) { s->Release(); s = nullptr; }
        }
        if (cs) { cs->Release(); cs = nullptr; }
    }
};

constexpr int kCompileRetryLimit = 5;

std::shared_mutex                    g_mutex;
std::vector<std::unique_ptr<Probe>>  g_probes;     // owned probes, indexed by registration order

// Synthesize the wrapper HLSL for a probe. Same header + define injections as
// customPass uses (CustomPass.cpp:593..604) so the user's #include compiles
// identically. Entry-point name is fixed.
std::string BuildProbeSource(const Spec& spec)
{
    std::string source = GetCommonShaderHeaderHLSLTop();
    source += GetCommonShaderHeaderHLSLBottom();
    for (auto* sv : g_shaderSettings.GetFloatShaderValues()) {
        if (!sv) continue;
        source += std::format("#define {} GFXModularFloats[{}]{}\n", sv->id, sv->bufferIndex / 4,
            std::array<std::string_view, 4>{ ".x", ".y", ".z", ".w" }[sv->bufferIndex % 4]);
    }
    for (auto* sv : g_shaderSettings.GetIntShaderValues()) {
        if (!sv) continue;
        source += std::format("#define {} GFXModularInts[{}]{}\n", sv->id, sv->bufferIndex / 4,
            std::array<std::string_view, 4>{ ".x", ".y", ".z", ".w" }[sv->bufferIndex % 4]);
    }
    for (auto* sv : g_shaderSettings.GetBoolShaderValues()) {
        if (!sv) continue;
        source += std::format("#define {} (GFXModularBools[{}]{} != 0)\n", sv->id, sv->bufferIndex / 4,
            std::array<std::string_view, 4>{ ".x", ".y", ".z", ".w" }[sv->bufferIndex % 4]);
    }
    source += "\n";
    source += std::format("#include \"{}\"\n\n", spec.includePath);
    source += "RWStructuredBuffer<float> _GpuScalarOut : register(u0);\n\n";
    source += "[numthreads(1,1,1)]\n";
    source += "void GpuScalarMain()\n";
    source += "{\n";
    source += std::format("    _GpuScalarOut[0] = {}();\n", spec.function);
    source += "}\n";
    return source;
}

// Compile + create UAV resources + staging ring. Mirrors CustomPass's
// EnsureCompiled flow (cache lookup → D3DCompile → CreateComputeShader) and
// reuses ShaderCache so repeated launches don't re-pay D3DCompile cost.
bool EnsureCompiled(Probe& probe)
{
    if (probe.cs && probe.uav && probe.outputBuf) return true;
    if (probe.compileFailed) return false;
    if (!g_rendererData || !g_rendererData->device) return false;
    if (++probe.compileAttempts > kCompileRetryLimit) {
        probe.compileFailed = true;
        REX::WARN("GpuScalar[{}]: compile retry limit exceeded; giving up", probe.spec.name);
        return false;
    }

    const std::string source = BuildProbeSource(probe.spec);
    constexpr uint32_t kCompileFlags = D3DCOMPILE_OPTIMIZATION_LEVEL3;
    const char* profile = "cs_5_0";
    const char* entry   = "GpuScalarMain";

    ID3DBlob* blob = nullptr;
    const std::string cacheKey = ShaderCache::ComputeKey({
        .assembledSource = source,
        .profile         = profile,
        .entry           = entry,
        .flags           = kCompileFlags,
    });
    if (ShaderCache::TryLoad(cacheKey, &blob)) {
        REX::INFO("GpuScalar[{}]: cache HIT ({} bytes)", probe.spec.name, blob->GetBufferSize());
    } else {
        ID3DBlob* errBlob = nullptr;
        auto* includer = new ShaderIncludeHandler();
        HRESULT hr = D3DCompile(source.c_str(), source.size(), probe.spec.name.c_str(),
                                nullptr, includer, entry, profile,
                                kCompileFlags, 0, &blob, &errBlob);
        delete includer;
        if (FAILED(hr)) {
            if (errBlob) {
                REX::WARN("GpuScalar[{}]: D3DCompile failed: {}",
                          probe.spec.name,
                          static_cast<const char*>(errBlob->GetBufferPointer()));
                errBlob->Release();
            } else {
                REX::WARN("GpuScalar[{}]: D3DCompile failed (no errBlob), hr=0x{:08X}",
                          probe.spec.name, static_cast<std::uint32_t>(hr));
            }
            probe.compileFailed = true;
            return false;
        }
        if (errBlob) errBlob->Release();
        ShaderCache::Store(cacheKey, blob);
        REX::INFO("GpuScalar[{}]: compiled ({} bytes)", probe.spec.name, blob->GetBufferSize());
    }

    // Build the CS. Set the create-replacement flag so the CreatePixelShader/
    // CreateComputeShader hooks (when they fire) don't treat our blob as an
    // engine shader candidate.
    ::g_isCreatingReplacementShader = true;
    HRESULT hr = g_rendererData->device->CreateComputeShader(
        blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &probe.cs);
    ::g_isCreatingReplacementShader = false;
    blob->Release();
    if (FAILED(hr) || !probe.cs) {
        REX::WARN("GpuScalar[{}]: CreateComputeShader failed, hr=0x{:08X}",
                  probe.spec.name, static_cast<std::uint32_t>(hr));
        probe.compileFailed = true;
        return false;
    }

    // Output buffer (RWStructuredBuffer<float> with one element).
    REX::W32::D3D11_BUFFER_DESC od{};
    od.byteWidth           = sizeof(float);
    od.usage               = REX::W32::D3D11_USAGE_DEFAULT;
    od.bindFlags           = REX::W32::D3D11_BIND_UNORDERED_ACCESS;
    od.cpuAccessFlags      = 0;
    od.miscFlags           = REX::W32::D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    od.structureByteStride = sizeof(float);
    hr = g_rendererData->device->CreateBuffer(&od, nullptr, &probe.outputBuf);
    if (FAILED(hr) || !probe.outputBuf) {
        REX::WARN("GpuScalar[{}]: CreateBuffer(output) failed, hr=0x{:08X}",
                  probe.spec.name, static_cast<std::uint32_t>(hr));
        probe.Release();
        probe.compileFailed = true;
        return false;
    }

    REX::W32::D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
    ud.format              = REX::W32::DXGI_FORMAT_UNKNOWN;  // structured → UNKNOWN
    ud.viewDimension       = REX::W32::D3D11_UAV_DIMENSION_BUFFER;
    ud.buffer.firstElement = 0;
    ud.buffer.numElements  = 1;
    ud.buffer.flags        = 0;
    hr = g_rendererData->device->CreateUnorderedAccessView(probe.outputBuf, &ud, &probe.uav);
    if (FAILED(hr) || !probe.uav) {
        REX::WARN("GpuScalar[{}]: CreateUnorderedAccessView failed, hr=0x{:08X}",
                  probe.spec.name, static_cast<std::uint32_t>(hr));
        probe.Release();
        probe.compileFailed = true;
        return false;
    }

    // Staging ring (one buffer per ring slot — STAGING+CPU_READ).
    for (UINT i = 0; i < kReadbackRingSize; ++i) {
        REX::W32::D3D11_BUFFER_DESC sd{};
        sd.byteWidth           = sizeof(float);
        sd.usage               = REX::W32::D3D11_USAGE_STAGING;
        sd.bindFlags           = 0;
        sd.cpuAccessFlags      = REX::W32::D3D11_CPU_ACCESS_READ;
        sd.miscFlags           = 0;
        sd.structureByteStride = 0;
        hr = g_rendererData->device->CreateBuffer(&sd, nullptr, &probe.staging[i]);
        if (FAILED(hr) || !probe.staging[i]) {
            REX::WARN("GpuScalar[{}]: CreateBuffer(staging[{}]) failed, hr=0x{:08X}",
                      probe.spec.name, i, static_cast<std::uint32_t>(hr));
            probe.Release();
            probe.compileFailed = true;
            return false;
        }
    }

    REX::INFO("GpuScalar[{}]: probe ready (function='{}', include='{}')",
              probe.spec.name, probe.spec.function, probe.spec.includePath);
    return true;
}

// Dispatch the probe, copy UAV → staging[writeIdx], map staging[1-writeIdx]
// (= the previous frame's result) and store the float in probe.value.
//
// The synthesized HLSL uses GFXInjected (cb / SRV) and the vu_* modular value
// SRVs the same way regular PS replacements do. The engine binds those to the
// PS pipeline via BindCustomShaderResources, but compute is a separate
// pipeline with separate binding tables — without CSSetShaderResources here
// the probe reads zeros from those slots and (for the user's curve) bails out
// at the `if (!vu_DirectLightEnabled) return 1.0;` early-out.
//
// Saves and restores both the CS shader and the SRV / UAV slots we touch.
void TickProbe(Probe& probe, REX::W32::ID3D11DeviceContext* ctx)
{
    if (!EnsureCompiled(probe)) return;

    // The four modular SRV slots are configurable in ShaderEngine.ini, so the
    // indices aren't known at compile time. Use them to size dynamic save/
    // restore buffers below.
    const UINT srvSlots[] = {
        CUSTOMBUFFER_SLOT,
        MODULAR_FLOATS_SLOT,
        MODULAR_INTS_SLOT,
        MODULAR_BOOLS_SLOT,
    };
    REX::W32::ID3D11ShaderResourceView* const srvValues[] = {
        g_customSRV,
        g_modularFloatsSRV,
        g_modularIntsSRV,
        g_modularBoolsSRV,
    };
    constexpr UINT kSrvCount = sizeof(srvSlots) / sizeof(srvSlots[0]);

    // Snapshot the CS state we're about to mutate. We touch:
    //   - CS shader
    //   - CS UAV slot 0
    //   - CS SRV slots {CUSTOMBUFFER, MODULAR_FLOATS, MODULAR_INTS, MODULAR_BOOLS}
    REX::W32::ID3D11ComputeShader*       prevCs       = nullptr;
    REX::W32::ID3D11ClassInstance*       prevInst[16] = {};
    UINT                                 prevInstCount = 16;
    REX::W32::ID3D11UnorderedAccessView* prevUav      = nullptr;
    REX::W32::ID3D11ShaderResourceView*  prevSrvs[kSrvCount] = {};
    ctx->CSGetShader(&prevCs, prevInst, &prevInstCount);
    ctx->CSGetUnorderedAccessViews(0, 1, &prevUav);
    for (UINT i = 0; i < kSrvCount; ++i) {
        ctx->CSGetShaderResources(srvSlots[i], 1, &prevSrvs[i]);
    }

    // Bind injected resources on the CS pipeline so the synthesized HLSL sees
    // the same GFXInjected + vu_* values as regular PS replacements.
    for (UINT i = 0; i < kSrvCount; ++i) {
        if (srvValues[i]) {
            ctx->CSSetShaderResources(srvSlots[i], 1, &srvValues[i]);
        }
    }

    // Bind + dispatch.
    UINT initialCount = 0;
    ctx->CSSetShader(probe.cs, nullptr, 0);
    ctx->CSSetUnorderedAccessViews(0, 1, &probe.uav, &initialCount);
    ctx->Dispatch(1, 1, 1);

    // Copy UAV → this frame's staging slot.
    ctx->CopyResource(probe.staging[probe.writeIdx], probe.outputBuf);

    // Map the *other* slot — that holds the result the engine wrote N frames
    // ago and which the GPU has had plenty of time to publish. With ring
    // size 2 the lag is exactly 1 frame after the first kReadbackRingSize
    // ticks have passed.
    ++probe.framesDispatched;
    if (probe.framesDispatched >= kReadbackRingSize) {
        const UINT readIdx = (probe.writeIdx + 1) % kReadbackRingSize;
        REX::W32::D3D11_MAPPED_SUBRESOURCE mapped{};
        const HRESULT hr = ctx->Map(probe.staging[readIdx], 0,
                                    REX::W32::D3D11_MAP_READ, 0, &mapped);
        if (SUCCEEDED(hr) && mapped.data) {
            probe.value = *static_cast<const float*>(mapped.data);
            probe.valid = true;
            ctx->Unmap(probe.staging[readIdx], 0);
        }
    }
    probe.writeIdx = (probe.writeIdx + 1) % kReadbackRingSize;

    // Restore prior CS state.
    ctx->CSSetShader(prevCs, prevInst, prevInstCount);
    UINT restoreInit = 0;
    ctx->CSSetUnorderedAccessViews(0, 1, &prevUav, &restoreInit);
    for (UINT i = 0; i < kSrvCount; ++i) {
        ctx->CSSetShaderResources(srvSlots[i], 1, &prevSrvs[i]);
    }

    if (prevCs)  prevCs->Release();
    if (prevUav) prevUav->Release();
    for (UINT i = 0; i < prevInstCount; ++i) {
        if (prevInst[i]) prevInst[i]->Release();
    }
    for (UINT i = 0; i < kSrvCount; ++i) {
        if (prevSrvs[i]) prevSrvs[i]->Release();
    }
}

}  // anonymous namespace

void Initialize()
{
    REX::INFO("GpuScalar: initialized");
}

void Shutdown()
{
    Reset();
}

void Reset()
{
    std::unique_lock lock(g_mutex);
    for (auto& p : g_probes) {
        if (p) p->Release();
    }
    g_probes.clear();
}

bool IsGpuScalarSection(const std::string& sectionName)
{
    return sectionName.rfind("gpuScalar:", 0) == 0;
}

bool ParseSection(const std::string& name,
                  std::ifstream&     file,
                  const std::string& endTag,
                  const std::string& folderName)
{
    Spec spec;
    spec.name       = name;
    spec.folderName = folderName;

    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == ';') continue;
        std::string clean = RemoveAllWS(RemoveInlineComment(line));
        if (clean.empty()) continue;
        if (ToLower(clean) == ToLower(endTag)) break;
        if (clean[0] == '[') {
            REX::WARN("GpuScalar[{}]: new section before {} — block malformed", name, endTag);
            return false;
        }

        auto [key, value] = GetKeyValueFromLine(clean);
        if (key.empty() || value.empty()) continue;
        const auto lowerKey = ToLower(key);
        if (lowerKey == "include")       spec.includePath = value;
        else if (lowerKey == "function") spec.function    = value;
        else {
            REX::WARN("GpuScalar[{}]: unknown key '{}'", name, key);
        }
    }

    if (spec.includePath.empty() || spec.function.empty()) {
        REX::WARN("GpuScalar[{}]: missing 'include' or 'function' — block ignored", name);
        return false;
    }

    std::unique_lock lock(g_mutex);
    // Reject duplicate names so consumers caching the value pointer don't get
    // surprised by a later registration silently shadowing the first.
    for (const auto& p : g_probes) {
        if (p && p->spec.name == spec.name) {
            REX::WARN("GpuScalar[{}]: duplicate registration; ignoring later entry", spec.name);
            return false;
        }
    }
    auto probe = std::make_unique<Probe>();
    probe->spec = std::move(spec);
    REX::INFO("GpuScalar[{}]: registered (include='{}', function='{}', folder='{}')",
              probe->spec.name, probe->spec.includePath, probe->spec.function, probe->spec.folderName);
    g_probes.push_back(std::move(probe));
    return true;
}

void OnFramePresent(REX::W32::ID3D11DeviceContext* context)
{
    if (!context) return;
    // Shared lock here would be enough for the iteration itself, but TickProbe
    // mutates probe state. Take a shared lock since the vector itself isn't
    // resized except on Reset/ParseSection, and treat the per-probe state as
    // owned by the render thread (the only thread that calls OnFramePresent).
    std::shared_lock lock(g_mutex);
    for (auto& p : g_probes) {
        if (p) TickProbe(*p, context);
    }
}

const float* GetValuePtr(const std::string& name)
{
    std::shared_lock lock(g_mutex);
    for (auto& p : g_probes) {
        if (p && p->spec.name == name) {
            // Returning a raw pointer that outlives the lock is safe because:
            // - Probe is heap-allocated and held in unique_ptr; the float lives
            //   inside the Probe.
            // - Reset() clears g_probes and Releases probes, BUT consumers are
            //   expected to drop cached pointers when Reset() is called (in
            //   practice this is handled by LightCullPolicy's per-rule resolver
            //   getting cleared at the same hot-reload point).
            return &p->value;
        }
    }
    return nullptr;
}

}  // namespace GpuScalar

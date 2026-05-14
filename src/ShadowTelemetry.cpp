#include <PCH.h>
#include "ShadowTelemetry.h"
#include "PhaseTelemetry.h"
#include "Global.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <limits>
#include <mutex>
#include <unordered_map>

namespace ShadowTelemetry {

std::atomic<Mode> g_mode{ Mode::Off };

namespace {

using RenderShadowMap_t = void (*)(void* light, void* shadowMapData);
using RenderScene_t = void (*)(void* camera, void* accumulator, bool flag);

RenderShadowMap_t s_origRenderShadowMap = nullptr;
RenderScene_t     s_origRenderScene = nullptr;
bool              s_installed = false;

REL::Relocation<std::uintptr_t> ptr_ShadowDirectionalRender{ REL::ID{ 871921, 2319335 } };
REL::Relocation<std::uintptr_t> ptr_RenderShadowMap{ REL::ID{ 1144068, 2319309 } };

constexpr std::uintptr_t kDirectionalRenderShadowMapCallOffsetOG = 0x48;  // 0x1428CA758 - 0x1428CA710
constexpr std::uintptr_t kRenderSceneCallOffsetOG = 0xFE;                 // 0x1428C98FE - 0x1428C9800

constexpr std::uintptr_t kDirectionalReturnOffsetOG = 0x4D;               // 0x1428CA75D - 0x1428CA710
constexpr std::uint32_t kDirectionalCacheMapSlot = 1;

constexpr double kLogIntervalSecs = 2.0;
constexpr std::size_t kMaxLoggedKeys = 8;
constexpr std::size_t kMaxLoggedWorkKeys = 6;
constexpr std::size_t kWorkKindCount = 3;

enum class LightKind : std::uint8_t {
    Unknown,
    Directional,
    Parabolic,
    Frustum,
};

const char* LightKindName(LightKind kind) noexcept
{
    switch (kind) {
    case LightKind::Directional: return "directional";
    case LightKind::Parabolic:   return "parabolic";
    case LightKind::Frustum:     return "frustum";
    default:                     return "unknown";
    }
}

const char* WorkKindName(WorkKind kind) noexcept
{
    switch (kind) {
    case WorkKind::CommandBufferReplay: return "cmdReplay";
    case WorkKind::ImmediatePass:       return "immediate";
    case WorkKind::BuildCommandBuffer:  return "buildCB";
    default:                            return "unknown";
    }
}

LightKind ClassifyCaller(void* returnAddress) noexcept
{
    const auto ra = reinterpret_cast<std::uintptr_t>(returnAddress);
    if (ra == ptr_ShadowDirectionalRender.address() + kDirectionalReturnOffsetOG) return LightKind::Directional;
    return LightKind::Unknown;
}

template <class T>
T ReadField(void* base, std::size_t offset, T fallback = {}) noexcept
{
    if (!base) return fallback;
    T value{};
    std::memcpy(&value, static_cast<std::byte*>(base) + offset, sizeof(T));
    return value;
}

template <class T>
void WriteField(void* base, std::size_t offset, const T& value) noexcept
{
    if (!base) {
        return;
    }
    std::memcpy(static_cast<std::byte*>(base) + offset, &value, sizeof(T));
}

std::uint64_t HashBytes(const void* data, std::size_t size) noexcept
{
    if (!data || size == 0) {
        return 0;
    }

    constexpr std::uint64_t kFnvOffset = 14695981039346656037ull;
    constexpr std::uint64_t kFnvPrime = 1099511628211ull;
    std::uint64_t hash = kFnvOffset;
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    for (std::size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= kFnvPrime;
    }
    return hash;
}

std::int32_t QuantizeFloat(float value, float step) noexcept
{
    if (!std::isfinite(value) || step <= 0.0f) {
        return (std::numeric_limits<std::int32_t>::min)();
    }

    const float q = std::round(value / step);
    if (q < static_cast<float>((std::numeric_limits<std::int32_t>::min)() + 1)) {
        return (std::numeric_limits<std::int32_t>::min)() + 1;
    }
    if (q > static_cast<float>((std::numeric_limits<std::int32_t>::max)())) {
        return (std::numeric_limits<std::int32_t>::max)();
    }
    return static_cast<std::int32_t>(q);
}

std::uint64_t ShadowCameraSignature(void* camera) noexcept
{
    if (!camera) {
        return 0;
    }

    // RE::NiCamera: worldToCam @ 0x120, viewFrustum/min/far/port through 0x193.
    // Keep this conservative: if the directional shadow camera moves or rotates
    // enough for the static depth projection to visibly shift, rebuild instead
    // of reusing stale depth. Tiny quantization absorbs float noise only.
    constexpr std::size_t kWorldToCamOffset = 0x120;
    constexpr std::size_t kFrustumOffset = 0x160;
    constexpr std::size_t kFrustumFloatCount = 9;  // NiFrustum + minNear + maxFarNearRatio
    constexpr float kRotationStep = 0.0010f;
    constexpr float kTranslationStep = 8.0f;
    constexpr float kFrustumStep = 4.0f;

    std::array<std::int32_t, 16 + kFrustumFloatCount> quantized{};
    const auto* matrix = reinterpret_cast<const float*>(static_cast<const std::byte*>(camera) + kWorldToCamOffset);
    for (std::size_t i = 0; i < 16; ++i) {
        const float v = matrix[i];
        const float step = std::abs(v) <= 4.0f ? kRotationStep : kTranslationStep;
        quantized[i] = QuantizeFloat(v, step);
    }

    const auto* frustum = reinterpret_cast<const float*>(static_cast<const std::byte*>(camera) + kFrustumOffset);
    for (std::size_t i = 0; i < kFrustumFloatCount; ++i) {
        quantized[16 + i] = QuantizeFloat(frustum[i], kFrustumStep);
    }

    return HashBytes(quantized.data(), quantized.size() * sizeof(quantized[0]));
}

std::uint64_t DominantLightDirectionSignature() noexcept
{
    if (g_customBufferData.g_SunValid <= 0.0f) {
        return 0;
    }

    const float x = g_customBufferData.g_SunDirX;
    const float y = g_customBufferData.g_SunDirY;
    const float z = g_customBufferData.g_SunDirZ;
    const float lenSq = x * x + y * y + z * z;
    if (!std::isfinite(lenSq) || lenSq <= 1.0e-6f) {
        return 0;
    }

    constexpr float kSunDirectionStep = 0.005f;
    const float invLen = 1.0f / std::sqrt(lenSq);
    const std::array<std::int32_t, 3> quantized{
        QuantizeFloat(x * invLen, kSunDirectionStep),
        QuantizeFloat(y * invLen, kSunDirectionStep),
        QuantizeFloat(z * invLen, kSunDirectionStep),
    };
    return HashBytes(quantized.data(), quantized.size() * sizeof(quantized[0]));
}

std::uint32_t ShadowMapDataIndex(void* light, void* shadowMapData) noexcept
{
    if (!light || !shadowMapData) {
        return (std::numeric_limits<std::uint32_t>::max)();
    }

    constexpr std::size_t kShadowMapDataArrayOffset = 0x198;
    constexpr std::size_t kShadowMapDataStride = 0xF0;
    auto* base = ReadField<std::byte*>(light, kShadowMapDataArrayOffset);
    auto* cur = static_cast<std::byte*>(shadowMapData);
    if (!base || cur < base) {
        return (std::numeric_limits<std::uint32_t>::max)();
    }

    const auto delta = static_cast<std::uintptr_t>(cur - base);
    if ((delta % kShadowMapDataStride) != 0) {
        return (std::numeric_limits<std::uint32_t>::max)();
    }
    return static_cast<std::uint32_t>(delta / kShadowMapDataStride);
}

struct ViewportSig {
    std::uint32_t d0 = 0;
    std::uint32_t d4 = 0;
    std::uint32_t d8 = 0;
    std::uint32_t dc = 0;

    bool operator==(const ViewportSig& o) const noexcept
    {
        return d0 == o.d0 && d4 == o.d4 && d8 == o.d8 && dc == o.dc;
    }
};

struct Key {
    void* light = nullptr;
    void* accumulator = nullptr;
    std::uint64_t cameraSig = 0;
    std::uint64_t dominantLightSig = 0;
    std::uint32_t depthTarget = 0;
    std::uint32_t mapSlot = 0;
    ViewportSig viewport{};
    LightKind kind = LightKind::Unknown;

    bool operator==(const Key& o) const noexcept
    {
        return light == o.light &&
               accumulator == o.accumulator &&
               cameraSig == o.cameraSig &&
               dominantLightSig == o.dominantLightSig &&
               depthTarget == o.depthTarget &&
               mapSlot == o.mapSlot &&
               viewport == o.viewport &&
               kind == o.kind;
    }
};

struct KeyHash {
    std::size_t operator()(const Key& k) const noexcept
    {
        std::size_t h = std::hash<void*>{}(k.light);
        auto mix = [&h](std::size_t v) {
            h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
        };
        mix(std::hash<void*>{}(k.accumulator));
        mix(std::hash<std::uint64_t>{}(k.cameraSig));
        mix(std::hash<std::uint64_t>{}(k.dominantLightSig));
        mix(std::hash<std::uint32_t>{}(k.depthTarget));
        mix(std::hash<std::uint32_t>{}(k.mapSlot));
        mix(std::hash<std::uint32_t>{}(k.viewport.d0));
        mix(std::hash<std::uint32_t>{}(k.viewport.d4));
        mix(std::hash<std::uint32_t>{}(k.viewport.d8));
        mix(std::hash<std::uint32_t>{}(k.viewport.dc));
        mix(std::hash<std::uint8_t>{}(static_cast<std::uint8_t>(k.kind)));
        return h;
    }
};

REX::W32::ID3D11DepthStencilView* ActiveShadowDSV(const Key& key) noexcept
{
    if (!g_rendererData ||
        key.depthTarget >= static_cast<std::uint32_t>(std::size(g_rendererData->depthStencilTargets)) ||
        key.mapSlot >= 4) {
        return nullptr;
    }

    return g_rendererData->depthStencilTargets[key.depthTarget].dsView[key.mapSlot];
}

struct Aggregate {
    std::uint64_t calls = 0;
    std::uint64_t totalNs = 0;
    std::uint64_t maxNs = 0;
    std::uint64_t bsDraws = 0;
    std::uint64_t d3dDraws = 0;
    std::uint64_t cmdBufDraws = 0;
    std::uint64_t renderSceneCalls = 0;
    std::uint64_t renderSceneNs = 0;
    std::uint64_t deferredLightsCalls = 0;
};

struct ShadowCacheState {
    Key key{};
    Key pendingBuildKey{};
    void* cullingProcess = nullptr;
    std::uint32_t skipsSinceUpdate = 0;
    std::uint32_t stableKeyHits = 0;
    std::uint32_t pendingBuildKeyHits = 0;
    std::uint32_t lastDynamicMaxSkip = 0;
    std::uint64_t eligible = 0;
    std::uint64_t updates = 0;
    std::uint64_t skips = 0;
    std::uint64_t invalidations = 0;
    std::uint64_t hits = 0;
    std::uint64_t misses = 0;
    std::uint64_t staticBuilds = 0;
    std::uint64_t restoreFailures = 0;
    std::uint64_t captured = 0;
    std::uint64_t emptyStaticBuilds = 0;
    std::uint64_t staticKept = 0;
    std::uint64_t staticSkipped = 0;
    std::uint64_t overlayKept = 0;
    std::uint64_t overlaySkipped = 0;
    std::uint64_t shadowMapOrMaskHookCalls = 0;
    std::uint64_t shadowMapOrMaskHookActiveCalls = 0;
    std::array<std::uint64_t, 4> shadowMapOrMaskHookCallsByPass{};
    std::array<std::uint64_t, 4> shadowMapOrMaskHookActiveCallsByPass{};
    bool valid = false;
};

enum class ShadowCachePass : std::uint8_t {
    None,
    Passthrough,
    StaticBuild,
    DynamicOverlay,
};

struct StaticDepthCache {
    REX::W32::ID3D11Texture2D* texture = nullptr;
    REX::W32::D3D11_TEXTURE2D_DESC desc{};
    std::uint64_t validSubresources = 0;
    bool valid = false;
};

struct WorkAggregate {
    std::uint64_t calls = 0;
    std::uint64_t totalNs = 0;
    std::uint64_t maxNs = 0;
    std::uint64_t bsDraws = 0;
    std::uint64_t d3dDraws = 0;
    std::uint64_t cmdBufDraws = 0;
};

struct WorkKey {
    Key shadow{};
    WorkKind kind = WorkKind::CommandBufferReplay;
    void* owner = nullptr;
    void* head = nullptr;
    void* cbData = nullptr;
    void* commandBuffer = nullptr;
    void* geometry = nullptr;
    void* shader = nullptr;
    void* shaderProperty = nullptr;
    std::uint32_t techniqueID = 0;
    std::int32_t passGroupIdx = -1;
    std::uint32_t subIdx = 0;
    std::uint32_t chainLen = 0;
    std::uint32_t srvCount = 0;
    std::uint32_t cbRecordCount = 0;
    std::uint32_t cbDrawCount = 0;
    std::uint32_t cbNonZeroDrawCount = 0;
    std::uint32_t cbMaxDrawCount = 0;
    std::uint32_t cbSrvRecordCount = 0;
    std::uint32_t cbMaxSrvRecordCount = 0;
    bool allowAlpha = false;

    bool operator==(const WorkKey& o) const noexcept
    {
        return shadow == o.shadow &&
               kind == o.kind &&
               owner == o.owner &&
               head == o.head &&
               cbData == o.cbData &&
               commandBuffer == o.commandBuffer &&
               geometry == o.geometry &&
               shader == o.shader &&
               shaderProperty == o.shaderProperty &&
               techniqueID == o.techniqueID &&
               passGroupIdx == o.passGroupIdx &&
               subIdx == o.subIdx &&
               chainLen == o.chainLen &&
               srvCount == o.srvCount &&
               cbRecordCount == o.cbRecordCount &&
               cbDrawCount == o.cbDrawCount &&
               cbNonZeroDrawCount == o.cbNonZeroDrawCount &&
               cbMaxDrawCount == o.cbMaxDrawCount &&
               cbSrvRecordCount == o.cbSrvRecordCount &&
               cbMaxSrvRecordCount == o.cbMaxSrvRecordCount &&
               allowAlpha == o.allowAlpha;
    }
};

struct WorkKeyHash {
    std::size_t operator()(const WorkKey& k) const noexcept
    {
        std::size_t h = KeyHash{}(k.shadow);
        auto mix = [&h](std::size_t v) {
            h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
        };
        mix(std::hash<std::uint8_t>{}(static_cast<std::uint8_t>(k.kind)));
        mix(std::hash<void*>{}(k.owner));
        mix(std::hash<void*>{}(k.head));
        mix(std::hash<void*>{}(k.cbData));
        mix(std::hash<void*>{}(k.commandBuffer));
        mix(std::hash<void*>{}(k.geometry));
        mix(std::hash<void*>{}(k.shader));
        mix(std::hash<void*>{}(k.shaderProperty));
        mix(std::hash<std::uint32_t>{}(k.techniqueID));
        mix(std::hash<std::int32_t>{}(k.passGroupIdx));
        mix(std::hash<std::uint32_t>{}(k.subIdx));
        mix(std::hash<std::uint32_t>{}(k.chainLen));
        mix(std::hash<std::uint32_t>{}(k.srvCount));
        mix(std::hash<std::uint32_t>{}(k.cbRecordCount));
        mix(std::hash<std::uint32_t>{}(k.cbDrawCount));
        mix(std::hash<std::uint32_t>{}(k.cbNonZeroDrawCount));
        mix(std::hash<std::uint32_t>{}(k.cbMaxDrawCount));
        mix(std::hash<std::uint32_t>{}(k.cbSrvRecordCount));
        mix(std::hash<std::uint32_t>{}(k.cbMaxSrvRecordCount));
        mix(std::hash<bool>{}(k.allowAlpha));
        return h;
    }
};

struct Scope {
    Key key{};
    void* shadowMapData = nullptr;
    void* camera = nullptr;
    void* cullingProcess = nullptr;
    std::uint32_t shadowMapDataIndex = (std::numeric_limits<std::uint32_t>::max)();
    std::chrono::steady_clock::time_point start{};
    std::uint64_t bsDraws = 0;
    std::uint64_t d3dDraws = 0;
    std::uint64_t cmdBufDraws = 0;
    std::uint64_t renderSceneCalls = 0;
    std::uint64_t renderSceneNs = 0;
    std::array<WorkAggregate, kWorkKindCount> work{};
    bool inDeferredLights = false;
};

thread_local std::vector<Scope> tl_scopes;

struct WorkScope {
    WorkKey key{};
    std::chrono::steady_clock::time_point start{};
    std::uint64_t bsDraws = 0;
    std::uint64_t d3dDraws = 0;
    std::uint64_t cmdBufDraws = 0;
};

thread_local std::vector<WorkScope> tl_workScopes;
Aggregate s_total;
std::unordered_map<Key, Aggregate, KeyHash> s_byKey;
std::array<WorkAggregate, kWorkKindCount> s_workTotals{};
std::unordered_map<WorkKey, WorkAggregate, WorkKeyHash> s_workByKey;
std::chrono::steady_clock::time_point s_lastLogTime;
std::chrono::steady_clock::time_point s_lastCacheLogTime;
ShadowCacheState s_directionalMapSlot1Cache;
StaticDepthCache s_staticDepthCache;
std::vector<REX::W32::ID3D11DepthStencilView*> s_staticBuildDSVs;
thread_local ShadowCachePass tl_shadowCachePass = ShadowCachePass::None;
std::atomic<ShadowCachePass> g_registrationShadowCachePass{ ShadowCachePass::None };
std::atomic_bool g_registrationShadowCacheActive{ false };
std::atomic_uint32_t g_registrationStaticKeepCount{ 0 };
std::atomic<void*> g_registrationShadowCacheAccumulator{ nullptr };
std::atomic_bool g_shadowCacheTargetActive{ false };
std::atomic<std::uint32_t> g_shadowCacheTargetMapSlot{ (std::numeric_limits<std::uint32_t>::max)() };
std::atomic<std::uint32_t> g_shadowCacheTargetDepthTarget{ (std::numeric_limits<std::uint32_t>::max)() };

class ScopedOMUnbind
{
public:
    explicit ScopedOMUnbind(REX::W32::ID3D11DeviceContext* context) :
        context_(context)
    {
        if (!context_) {
            return;
        }

        context_->OMGetRenderTargets(kMaxRTVs, rtvs_.data(), &dsv_);
        context_->OMSetRenderTargets(0, nullptr, nullptr);
    }

    ~ScopedOMUnbind()
    {
        if (!context_) {
            return;
        }

        context_->OMSetRenderTargets(kMaxRTVs, rtvs_.data(), dsv_);
        for (auto*& rtv : rtvs_) {
            if (rtv) {
                rtv->Release();
                rtv = nullptr;
            }
        }
        if (dsv_) {
            dsv_->Release();
            dsv_ = nullptr;
        }
    }

    ScopedOMUnbind(const ScopedOMUnbind&) = delete;
    ScopedOMUnbind& operator=(const ScopedOMUnbind&) = delete;

private:
    static constexpr std::uint32_t kMaxRTVs = 8;
    REX::W32::ID3D11DeviceContext* context_ = nullptr;
    std::array<REX::W32::ID3D11RenderTargetView*, kMaxRTVs> rtvs_{};
    REX::W32::ID3D11DepthStencilView* dsv_ = nullptr;
};

std::size_t WorkIndex(WorkKind kind) noexcept
{
    return static_cast<std::size_t>(kind) < kWorkKindCount ? static_cast<std::size_t>(kind) : 0;
}

void AddToWorkAggregate(WorkAggregate& dst, std::uint64_t ns, std::uint64_t bsDraws, std::uint64_t d3dDraws, std::uint64_t cmdBufDraws)
{
    ++dst.calls;
    dst.totalNs += ns;
    if (ns > dst.maxNs) {
        dst.maxNs = ns;
    }
    dst.bsDraws += bsDraws;
    dst.d3dDraws += d3dDraws;
    dst.cmdBufDraws += cmdBufDraws;
}

void AddToAggregate(Aggregate& dst, const Scope& scope, std::uint64_t ns)
{
    ++dst.calls;
    dst.totalNs += ns;
    if (ns > dst.maxNs) {
        dst.maxNs = ns;
    }
    dst.bsDraws += scope.bsDraws;
    dst.d3dDraws += scope.d3dDraws;
    dst.cmdBufDraws += scope.cmdBufDraws;
    dst.renderSceneCalls += scope.renderSceneCalls;
    dst.renderSceneNs += scope.renderSceneNs;
    if (scope.inDeferredLights) {
        ++dst.deferredLightsCalls;
    }
}

void ResetWindow()
{
    s_total = {};
    s_byKey.clear();
    s_workTotals = {};
    s_workByKey.clear();
    s_directionalMapSlot1Cache.updates = 0;
    s_directionalMapSlot1Cache.skips = 0;
    s_directionalMapSlot1Cache.invalidations = 0;
    s_directionalMapSlot1Cache.hits = 0;
    s_directionalMapSlot1Cache.misses = 0;
    s_directionalMapSlot1Cache.staticBuilds = 0;
    s_directionalMapSlot1Cache.restoreFailures = 0;
    s_directionalMapSlot1Cache.captured = 0;
    s_directionalMapSlot1Cache.emptyStaticBuilds = 0;
    s_directionalMapSlot1Cache.staticKept = 0;
    s_directionalMapSlot1Cache.staticSkipped = 0;
    s_directionalMapSlot1Cache.overlayKept = 0;
    s_directionalMapSlot1Cache.overlaySkipped = 0;
    s_directionalMapSlot1Cache.shadowMapOrMaskHookCalls = 0;
    s_directionalMapSlot1Cache.shadowMapOrMaskHookActiveCalls = 0;
    s_directionalMapSlot1Cache.shadowMapOrMaskHookCallsByPass = {};
    s_directionalMapSlot1Cache.shadowMapOrMaskHookActiveCallsByPass = {};
}

void SetRegistrationShadowCachePass(ShadowCachePass pass) noexcept;
void IncrementRegistrationCounter(std::uint64_t& counter) noexcept;
std::uint32_t ShadowCacheDynamicMaxSkips(bool stableCamera) noexcept;

std::size_t PassIndex(ShadowCachePass pass) noexcept
{
    const auto idx = static_cast<std::size_t>(pass);
    return idx < 4 ? idx : 0;
}

const char* PassShortName(ShadowCachePass pass) noexcept
{
    switch (pass) {
    case ShadowCachePass::None: return "n";
    case ShadowCachePass::Passthrough: return "p";
    case ShadowCachePass::StaticBuild: return "s";
    case ShadowCachePass::DynamicOverlay: return "d";
    default: return "?";
    }
}

bool IsRegistrationFilteringPass(ShadowCachePass pass) noexcept
{
    return pass == ShadowCachePass::StaticBuild ||
           pass == ShadowCachePass::DynamicOverlay;
}

bool RegistrationAccumulatorMatches(void* accumulator) noexcept
{
    void* expected = g_registrationShadowCacheAccumulator.load(std::memory_order_acquire);
    return expected == nullptr || expected == accumulator;
}

ShadowCachePass CurrentResolvedRegistrationPass() noexcept
{
    return g_registrationShadowCachePass.load(std::memory_order_acquire);
}

void ReleaseStaticDepthCache() noexcept
{
    if (s_staticDepthCache.texture) {
        s_staticDepthCache.texture->Release();
        s_staticDepthCache.texture = nullptr;
    }
    s_staticDepthCache.desc = {};
    s_staticDepthCache.validSubresources = 0;
    s_staticDepthCache.valid = false;
}

void ReleaseStaticBuildDSVs() noexcept
{
    for (auto* dsv : s_staticBuildDSVs) {
        if (dsv) {
            dsv->Release();
        }
    }
    s_staticBuildDSVs.clear();
}

void RememberStaticBuildDSV(REX::W32::ID3D11DepthStencilView* dsv) noexcept
{
    if (!dsv) {
        return;
    }

    for (auto* existing : s_staticBuildDSVs) {
        if (existing == dsv) {
            return;
        }
    }

    dsv->AddRef();
    s_staticBuildDSVs.push_back(dsv);
}

bool SameTextureDesc(const REX::W32::D3D11_TEXTURE2D_DESC& a,
                     const REX::W32::D3D11_TEXTURE2D_DESC& b) noexcept
{
    return std::memcmp(&a, &b, sizeof(a)) == 0;
}

bool GetDSVSubresource(const REX::W32::D3D11_DEPTH_STENCIL_VIEW_DESC& dsvDesc,
                       const REX::W32::D3D11_TEXTURE2D_DESC& texDesc,
                       std::uint32_t& outSubresource) noexcept
{
    switch (dsvDesc.viewDimension) {
    case REX::W32::D3D11_DSV_DIMENSION_TEXTURE2D:
    case REX::W32::D3D11_DSV_DIMENSION_TEXTURE2DMS:
        outSubresource = dsvDesc.viewDimension == REX::W32::D3D11_DSV_DIMENSION_TEXTURE2D ?
            dsvDesc.texture2D.mipSlice :
            0;
        return outSubresource < texDesc.mipLevels * texDesc.arraySize;
    case REX::W32::D3D11_DSV_DIMENSION_TEXTURE2DARRAY:
        if (dsvDesc.texture2DArray.arraySize != 1) {
            return false;
        }
        outSubresource = dsvDesc.texture2DArray.mipSlice +
                         dsvDesc.texture2DArray.firstArraySlice * texDesc.mipLevels;
        return outSubresource < texDesc.mipLevels * texDesc.arraySize;
    case REX::W32::D3D11_DSV_DIMENSION_TEXTURE2DMSARRAY:
        if (dsvDesc.texture2DMSArray.arraySize != 1) {
            return false;
        }
        outSubresource = dsvDesc.texture2DMSArray.firstArraySlice * texDesc.mipLevels;
        return outSubresource < texDesc.mipLevels * texDesc.arraySize;
    default:
        return false;
    }
}

bool QueryTextureFromDSV(REX::W32::ID3D11DepthStencilView* dsv,
                         REX::W32::ID3D11Texture2D** outTexture,
                         REX::W32::D3D11_DEPTH_STENCIL_VIEW_DESC* outDSVDesc,
                         std::uint32_t* outSubresource)
{
    if (!dsv || !outTexture) {
        return false;
    }

    REX::W32::ID3D11Resource* resource = nullptr;
    dsv->GetResource(&resource);
    if (!resource) {
        return false;
    }

    REX::W32::ID3D11Texture2D* texture = nullptr;
    const HRESULT qi = resource->QueryInterface(
        REX::W32::IID_ID3D11Texture2D,
        reinterpret_cast<void**>(&texture));
    resource->Release();
    if (FAILED(qi) || !texture) {
        return false;
    }

    REX::W32::D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
    dsv->GetDesc(&dsvDesc);
    REX::W32::D3D11_TEXTURE2D_DESC texDesc{};
    texture->GetDesc(&texDesc);

    std::uint32_t subresource = 0;
    if (!GetDSVSubresource(dsvDesc, texDesc, subresource)) {
        texture->Release();
        return false;
    }

    if (outDSVDesc) {
        *outDSVDesc = dsvDesc;
    }
    if (outSubresource) {
        *outSubresource = subresource;
    }
    *outTexture = texture;
    return true;
}

bool EnsureStaticDepthCacheTexture(REX::W32::ID3D11Texture2D* activeTexture)
{
    if (!activeTexture || !g_rendererData || !g_rendererData->device) {
        return false;
    }

    REX::W32::D3D11_TEXTURE2D_DESC desc{};
    activeTexture->GetDesc(&desc);
    desc.usage = REX::W32::D3D11_USAGE_DEFAULT;
    desc.cpuAccessFlags = 0;

    if (s_staticDepthCache.texture && SameTextureDesc(s_staticDepthCache.desc, desc)) {
        return true;
    }

    ReleaseStaticDepthCache();
    auto* device = g_rendererData->device;
    HRESULT hr = device->CreateTexture2D(&desc, nullptr, &s_staticDepthCache.texture);
    if (FAILED(hr) || !s_staticDepthCache.texture) {
        REX::WARN("ShadowCache: CreateTexture2D static depth cache failed 0x{:08X}", static_cast<unsigned>(hr));
        return false;
    }

    s_staticDepthCache.desc = desc;
    s_staticDepthCache.validSubresources = 0;
    s_staticDepthCache.valid = false;
    return true;
}

bool IsCacheableSubresource(std::uint32_t subresource) noexcept
{
    return subresource < 64;
}

std::uint64_t SubresourceMask(std::uint32_t subresource) noexcept
{
    return IsCacheableSubresource(subresource) ? (1ull << subresource) : 0ull;
}

bool CopyDSVToStaticCache(REX::W32::ID3D11DeviceContext* context,
                          REX::W32::ID3D11DepthStencilView* dsv)
{
    if (!context || !dsv) {
        return false;
    }

    REX::W32::ID3D11Texture2D* activeTexture = nullptr;
    std::uint32_t subresource = 0;
    if (!QueryTextureFromDSV(dsv, &activeTexture, nullptr, &subresource)) {
        return false;
    }
    if (!IsCacheableSubresource(subresource)) {
        activeTexture->Release();
        return false;
    }

    const bool ok = EnsureStaticDepthCacheTexture(activeTexture);
    if (ok) {
        ScopedOMUnbind unbind(context);
        context->CopySubresourceRegion(
            s_staticDepthCache.texture, subresource, 0, 0, 0,
            activeTexture, subresource, nullptr);
        s_staticDepthCache.validSubresources |= SubresourceMask(subresource);
        s_staticDepthCache.valid = true;
    }
    activeTexture->Release();
    return ok;
}

bool RestoreStaticCacheToDSV(REX::W32::ID3D11DeviceContext* context,
                             REX::W32::ID3D11DepthStencilView* dsv)
{
    if (!context || !dsv || !s_staticDepthCache.texture || !s_staticDepthCache.valid) {
        return false;
    }

    REX::W32::ID3D11Texture2D* activeTexture = nullptr;
    std::uint32_t subresource = 0;
    if (!QueryTextureFromDSV(dsv, &activeTexture, nullptr, &subresource)) {
        return false;
    }

    REX::W32::D3D11_TEXTURE2D_DESC activeDesc{};
    activeTexture->GetDesc(&activeDesc);
    if (!SameTextureDesc(activeDesc, s_staticDepthCache.desc) ||
        (s_staticDepthCache.validSubresources & SubresourceMask(subresource)) == 0) {
        activeTexture->Release();
        return false;
    }

    {
        ScopedOMUnbind unbind(context);
        context->CopySubresourceRegion(
            activeTexture, subresource, 0, 0, 0,
            s_staticDepthCache.texture, subresource, nullptr);
    }
    activeTexture->Release();
    return true;
}

bool CaptureStaticCacheFromBuildDSV()
{
    auto* context = g_rendererData ? g_rendererData->context : nullptr;
    bool ok = context && !s_staticBuildDSVs.empty();
    if (ok) {
        s_staticDepthCache.validSubresources = 0;
        s_staticDepthCache.valid = false;
        for (auto* dsv : s_staticBuildDSVs) {
            if (!CopyDSVToStaticCache(context, dsv)) {
                ok = false;
                break;
            }
        }
    }
    ReleaseStaticBuildDSVs();
    if (ok) {
        ++s_directionalMapSlot1Cache.captured;
    }
    return ok;
}

bool IsClose(std::uint32_t a, std::uint32_t b, std::uint32_t tolerance) noexcept
{
    return a > b ? (a - b) <= tolerance : (b - a) <= tolerance;
}

bool ViewportClose(const ViewportSig& a, const ViewportSig& b) noexcept
{
    constexpr std::uint32_t kViewportTolerance = 4;
    return IsClose(a.d0, b.d0, kViewportTolerance) &&
           IsClose(a.d4, b.d4, kViewportTolerance) &&
           IsClose(a.d8, b.d8, kViewportTolerance) &&
           IsClose(a.dc, b.dc, kViewportTolerance);
}

bool CacheKeyStable(const Key& cached, const Key& current) noexcept
{
    return cached.cameraSig == current.cameraSig &&
           cached.dominantLightSig == current.dominantLightSig &&
           cached.depthTarget == current.depthTarget &&
           cached.mapSlot == current.mapSlot &&
           cached.kind == current.kind &&
           ViewportClose(cached.viewport, current.viewport);
}

std::uint32_t ShadowCacheDynamicMaxSkips(bool stableCamera) noexcept
{
    if (!stableCamera) {
        return 1u;
    }

    const std::uint32_t configured = (std::max)(1u, SHADOW_CACHE_DIRECTIONAL_MAPSLOT1_MAX_SKIP);
    return (std::min<std::uint32_t>)(8u, configured);
}

void MaybeLogShadowCache(
    const Scope& scope,
    const char* decision,
    bool cacheEligible,
    bool cacheHit,
    bool preCacheValid,
    bool preDepthValid,
    bool preStableKey)
{
    if (!SHADOW_CACHE_DIRECTIONAL_MAPSLOT1_ON) {
        return;
    }

    auto& cache = s_directionalMapSlot1Cache;
    const bool hasActivity =
        cache.eligible != 0 ||
        cache.hits != 0 ||
        cache.misses != 0 ||
        cache.staticBuilds != 0 ||
        cache.captured != 0 ||
        cache.emptyStaticBuilds != 0 ||
        cache.restoreFailures != 0 ||
        cache.shadowMapOrMaskHookCalls != 0 ||
        cache.invalidations != 0;
    if (!hasActivity) {
        return;
    }

    const ShadowCachePass registrationPass =
        g_registrationShadowCachePass.load(std::memory_order_acquire);
    const bool registrationActive =
        g_registrationShadowCacheActive.load(std::memory_order_acquire);
    const auto cachedKeyHash = static_cast<std::uint64_t>(KeyHash{}(cache.key));
    const auto currentKeyHash = static_cast<std::uint64_t>(KeyHash{}(scope.key));
    const bool keyLightEq = cache.key.light == scope.key.light;
    const bool keyCameraEq = cache.key.cameraSig == scope.key.cameraSig;
    const bool keySunEq = cache.key.dominantLightSig == scope.key.dominantLightSig;
    const bool keyDepthEq = cache.key.depthTarget == scope.key.depthTarget;
    const bool keySlotEq = cache.key.mapSlot == scope.key.mapSlot;
    const bool keyKindEq = cache.key.kind == scope.key.kind;
    const bool keyViewportEq = ViewportClose(cache.key.viewport, scope.key.viewport);

    REX::INFO(
        "ShadowCache[directional idx={} mapSlot={} targetSlot={}]: decision={} eligibleNow={} hitNow={} "
        "pre(valid/depth/stable)={}/{}/{} "
        "post(valid/depth/stableHits/skips)={}/{}/{}/{} "
        "key(cur/cache)={:#x}/{:#x} keyEq(l/cam/sun/depth/slot/kind/vp)={}/{}/{}/{}/{}/{}/{} viewport=({},{},{},{}) "
        "dsv(validMask={:#x}) pass={}/{} "
        "eligible={} hits={} misses={} staticBuilds={} captures={} "
        "emptyStatic={} restoreFailures={} staticKeep={} staticSkip={} overlayKeep={} overlaySkip={} "
        "regSM={}/{} smPass(n/p/s/d={}/{}/{}/{}, active={}/{}/{}/{}) "
        "updates={} skips={} invalidations={}",
        scope.shadowMapDataIndex,
        scope.key.mapSlot,
        kDirectionalCacheMapSlot,
        decision ? decision : "?",
        cacheEligible,
        cacheHit,
        preCacheValid,
        preDepthValid,
        preStableKey,
        cache.valid,
        s_staticDepthCache.valid,
        cache.stableKeyHits,
        cache.skipsSinceUpdate,
        currentKeyHash,
        cachedKeyHash,
        keyLightEq,
        keyCameraEq,
        keySunEq,
        keyDepthEq,
        keySlotEq,
        keyKindEq,
        keyViewportEq,
        scope.key.viewport.d0,
        scope.key.viewport.d4,
        scope.key.viewport.d8,
        scope.key.viewport.dc,
        s_staticDepthCache.validSubresources,
        PassShortName(registrationPass),
        registrationActive,
        cache.eligible,
        cache.hits,
        cache.misses,
        cache.staticBuilds,
        cache.captured,
        cache.emptyStaticBuilds,
        cache.restoreFailures,
        cache.staticKept,
        cache.staticSkipped,
        cache.overlayKept,
        cache.overlaySkipped,
        cache.shadowMapOrMaskHookActiveCalls,
        cache.shadowMapOrMaskHookCalls,
        cache.shadowMapOrMaskHookCallsByPass[PassIndex(ShadowCachePass::None)],
        cache.shadowMapOrMaskHookCallsByPass[PassIndex(ShadowCachePass::Passthrough)],
        cache.shadowMapOrMaskHookCallsByPass[PassIndex(ShadowCachePass::StaticBuild)],
        cache.shadowMapOrMaskHookCallsByPass[PassIndex(ShadowCachePass::DynamicOverlay)],
        cache.shadowMapOrMaskHookActiveCallsByPass[PassIndex(ShadowCachePass::None)],
        cache.shadowMapOrMaskHookActiveCallsByPass[PassIndex(ShadowCachePass::Passthrough)],
        cache.shadowMapOrMaskHookActiveCallsByPass[PassIndex(ShadowCachePass::StaticBuild)],
        cache.shadowMapOrMaskHookActiveCallsByPass[PassIndex(ShadowCachePass::DynamicOverlay)],
        cache.updates,
        cache.skips,
        cache.invalidations);
    cache.eligible = 0;
    cache.updates = 0;
    cache.skips = 0;
    cache.invalidations = 0;
    cache.hits = 0;
    cache.misses = 0;
    cache.staticBuilds = 0;
    cache.restoreFailures = 0;
    cache.captured = 0;
    cache.emptyStaticBuilds = 0;
    cache.staticKept = 0;
    cache.staticSkipped = 0;
    cache.overlayKept = 0;
    cache.overlaySkipped = 0;
    cache.shadowMapOrMaskHookCalls = 0;
    cache.shadowMapOrMaskHookActiveCalls = 0;
    cache.shadowMapOrMaskHookCallsByPass = {};
    cache.shadowMapOrMaskHookActiveCallsByPass = {};
}

bool IsDirectionalMapSlot1CacheEligible(const Scope& scope) noexcept
{
    return SHADOW_CACHE_DIRECTIONAL_MAPSLOT1_ON &&
           SHADOW_CACHE_DIRECTIONAL_MAPSLOT1_MAX_SKIP != 0 &&
           scope.key.kind == LightKind::Directional &&
           scope.key.mapSlot == kDirectionalCacheMapSlot &&
           scope.key.light != nullptr &&
           scope.key.accumulator != nullptr;
}

bool DirectionalMapSlot1CacheHit(const Scope& scope) noexcept
{
    auto& cache = s_directionalMapSlot1Cache;
    const bool stableKey = CacheKeyStable(cache.key, scope.key);
    if (!cache.valid ||
        !s_staticDepthCache.valid) {
        cache.stableKeyHits = 0;
        cache.lastDynamicMaxSkip = ShadowCacheDynamicMaxSkips(false);
        return false;
    }

    if (!stableKey) {
        ++cache.invalidations;
        cache.valid = false;
        cache.skipsSinceUpdate = 0;
        cache.stableKeyHits = 0;
        cache.lastDynamicMaxSkip = ShadowCacheDynamicMaxSkips(false);
        ReleaseStaticDepthCache();
        return false;
    }

    const bool stableCamera = cache.stableKeyHits >= 2;
    const std::uint32_t dynamicMaxSkips = ShadowCacheDynamicMaxSkips(stableCamera);
    cache.lastDynamicMaxSkip = dynamicMaxSkips;
    if (!stableCamera && cache.skipsSinceUpdate >= dynamicMaxSkips) {
        cache.skipsSinceUpdate = 0;
        cache.stableKeyHits = 0;
        return false;
    }

    ++cache.skipsSinceUpdate;
    ++cache.stableKeyHits;
    ++cache.skips;
    ++cache.hits;
    return true;
}

bool DirectionalMapSlot1StaticBuildReady(const Scope& scope) noexcept
{
    auto& cache = s_directionalMapSlot1Cache;
    if (cache.valid &&
        s_staticDepthCache.valid &&
        CacheKeyStable(cache.key, scope.key)) {
        cache.pendingBuildKey = {};
        cache.pendingBuildKeyHits = 0;
        return true;
    }

    if (!CacheKeyStable(cache.pendingBuildKey, scope.key)) {
        cache.pendingBuildKey = scope.key;
        cache.pendingBuildKeyHits = 1;
        return false;
    }

    if (cache.pendingBuildKeyHits < 3) {
        ++cache.pendingBuildKeyHits;
    }
    return cache.pendingBuildKeyHits >= 2;
}

void MarkDirectionalMapSlot1CacheUpdated(const Scope& scope) noexcept
{
    if (!SHADOW_CACHE_DIRECTIONAL_MAPSLOT1_ON ||
        scope.key.kind != LightKind::Directional ||
        scope.key.mapSlot != kDirectionalCacheMapSlot) {
        return;
    }

    ++s_directionalMapSlot1Cache.updates;
}

void MaybeLog()
{
    const auto now = std::chrono::steady_clock::now();
    const double secs = std::chrono::duration<double>(now - s_lastLogTime).count();
    if (secs < kLogIntervalSecs || s_total.calls == 0) return;
    s_lastLogTime = now;

    REX::INFO(
        "ShadowTelemetry[RenderShadowMap]: calls={} over {:.2f}s ({:.1f}/s) "
        "totalMs/s={:.2f} avgUs={:.1f} maxUs={:.1f} bsDraws/call={:.1f} "
        "cmdBuf/call={:.1f} d3dDraws/call={:.1f} renderSceneMs/s={:.2f} inDeferredLights={}/{}",
        s_total.calls, secs, static_cast<double>(s_total.calls) / secs,
        (s_total.totalNs / 1'000'000.0) / secs,
        (s_total.totalNs / 1000.0) / static_cast<double>(s_total.calls),
        s_total.maxNs / 1000.0,
        static_cast<double>(s_total.bsDraws) / static_cast<double>(s_total.calls),
        static_cast<double>(s_total.cmdBufDraws) / static_cast<double>(s_total.calls),
        static_cast<double>(s_total.d3dDraws) / static_cast<double>(s_total.calls),
        (s_total.renderSceneNs / 1'000'000.0) / secs,
        s_total.deferredLightsCalls, s_total.calls);

    std::vector<std::pair<Key, Aggregate>> rows;
    rows.reserve(s_byKey.size());
    for (const auto& item : s_byKey) {
        rows.push_back(item);
    }
    std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) {
        return a.second.totalNs > b.second.totalNs;
    });

    const std::size_t n = rows.size() < kMaxLoggedKeys ? rows.size() : kMaxLoggedKeys;
    for (std::size_t i = 0; i < n; ++i) {
        const auto& [key, b] = rows[i];
        REX::INFO(
            "  Shadow[{}] light={} accum={} depthTarget={} mapSlot={} viewport=({},{},{},{}) "
            "calls={} totalMs/s={:.2f} avgUs={:.1f} d3dDraws/call={:.1f} bsDraws/call={:.1f}",
            LightKindName(key.kind),
            key.light,
            key.accumulator,
            key.depthTarget,
            key.mapSlot,
            key.viewport.d0, key.viewport.d4, key.viewport.d8, key.viewport.dc,
            b.calls,
            (b.totalNs / 1'000'000.0) / secs,
            (b.totalNs / 1000.0) / static_cast<double>(b.calls),
            static_cast<double>(b.d3dDraws) / static_cast<double>(b.calls),
            static_cast<double>(b.bsDraws) / static_cast<double>(b.calls));
    }

    REX::INFO(
        "ShadowTelemetry[Work]: replay calls={} ms/s={:.2f} d3d/call={:.1f}; "
        "immediate calls={} ms/s={:.2f} d3d/call={:.1f}; buildCB calls={} ms/s={:.2f}",
        s_workTotals[WorkIndex(WorkKind::CommandBufferReplay)].calls,
        (s_workTotals[WorkIndex(WorkKind::CommandBufferReplay)].totalNs / 1'000'000.0) / secs,
        s_workTotals[WorkIndex(WorkKind::CommandBufferReplay)].calls
            ? static_cast<double>(s_workTotals[WorkIndex(WorkKind::CommandBufferReplay)].d3dDraws) /
                  static_cast<double>(s_workTotals[WorkIndex(WorkKind::CommandBufferReplay)].calls)
            : 0.0,
        s_workTotals[WorkIndex(WorkKind::ImmediatePass)].calls,
        (s_workTotals[WorkIndex(WorkKind::ImmediatePass)].totalNs / 1'000'000.0) / secs,
        s_workTotals[WorkIndex(WorkKind::ImmediatePass)].calls
            ? static_cast<double>(s_workTotals[WorkIndex(WorkKind::ImmediatePass)].d3dDraws) /
                  static_cast<double>(s_workTotals[WorkIndex(WorkKind::ImmediatePass)].calls)
            : 0.0,
        s_workTotals[WorkIndex(WorkKind::BuildCommandBuffer)].calls,
        (s_workTotals[WorkIndex(WorkKind::BuildCommandBuffer)].totalNs / 1'000'000.0) / secs);

    std::vector<std::pair<WorkKey, WorkAggregate>> workRows;
    workRows.reserve(s_workByKey.size());
    for (const auto& item : s_workByKey) {
        workRows.push_back(item);
    }
    std::sort(workRows.begin(), workRows.end(), [](const auto& a, const auto& b) {
        return a.second.totalNs > b.second.totalNs;
    });

    const std::size_t workN = workRows.size() < kMaxLoggedWorkKeys ? workRows.size() : kMaxLoggedWorkKeys;
    for (std::size_t i = 0; i < workN; ++i) {
        const auto& [key, b] = workRows[i];
        REX::INFO(
            "  ShadowWork[{}] light={} mapSlot={} group={} sub={} chain={} rec={} recDraws={} nonZeroDraws={} maxDraw={} recSrv={} maxSrv={} "
            "geom={} shader={} prop={} tech=0x{:08X} cbData={} firstRec={} "
            "calls={} totalMs/s={:.2f} avgUs={:.1f} d3d/call={:.1f}",
            WorkKindName(key.kind),
            key.shadow.light,
            key.shadow.mapSlot,
            key.passGroupIdx,
            key.subIdx,
            key.chainLen,
            key.cbRecordCount,
            key.cbDrawCount,
            key.cbNonZeroDrawCount,
            key.cbMaxDrawCount,
            key.cbSrvRecordCount,
            key.cbMaxSrvRecordCount,
            key.geometry,
            key.shader,
            key.shaderProperty,
            key.techniqueID,
            key.cbData,
            key.commandBuffer,
            b.calls,
            (b.totalNs / 1'000'000.0) / secs,
            (b.totalNs / 1000.0) / static_cast<double>(b.calls),
            b.calls ? static_cast<double>(b.d3dDraws) / static_cast<double>(b.calls) : 0.0);
    }

    ResetWindow();
}

void HookedRenderScene(void* camera, void* accumulator, bool flag)
{
    if (tl_scopes.empty()) {
        s_origRenderScene(camera, accumulator, flag);
        return;
    }

    const auto t0 = std::chrono::steady_clock::now();
    s_origRenderScene(camera, accumulator, flag);
    const auto t1 = std::chrono::steady_clock::now();
    const auto ns = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());

    if (g_mode.load(std::memory_order_relaxed) == Mode::On) {
        auto& scope = tl_scopes.back();
        ++scope.renderSceneCalls;
        scope.renderSceneNs += ns;
    }
}

void SetRegistrationShadowCachePass(ShadowCachePass pass) noexcept
{
    const bool active = pass == ShadowCachePass::StaticBuild ||
                        pass == ShadowCachePass::DynamicOverlay;
    if (pass == ShadowCachePass::StaticBuild) {
        g_registrationStaticKeepCount.store(0, std::memory_order_relaxed);
    }
    g_registrationShadowCachePass.store(pass, std::memory_order_release);
    g_registrationShadowCacheActive.store(active, std::memory_order_release);
}

class ScopedShadowCacheAccumulator
{
public:
    explicit ScopedShadowCacheAccumulator(const Scope& scope) noexcept :
        previousThreadPass_(tl_shadowCachePass),
        previousAccumulator_(g_registrationShadowCacheAccumulator.load(std::memory_order_acquire)),
        previousTargetActive_(g_shadowCacheTargetActive.load(std::memory_order_acquire)),
        previousTargetMapSlot_(g_shadowCacheTargetMapSlot.load(std::memory_order_acquire)),
        previousTargetDepthTarget_(g_shadowCacheTargetDepthTarget.load(std::memory_order_acquire))
    {
        g_registrationShadowCacheAccumulator.store(scope.key.accumulator, std::memory_order_release);
        g_shadowCacheTargetMapSlot.store(scope.key.mapSlot, std::memory_order_release);
        g_shadowCacheTargetDepthTarget.store(scope.key.depthTarget, std::memory_order_release);
        g_shadowCacheTargetActive.store(true, std::memory_order_release);
    }

    ~ScopedShadowCacheAccumulator()
    {
        tl_shadowCachePass = previousThreadPass_;
        g_registrationShadowCacheAccumulator.store(previousAccumulator_, std::memory_order_release);
        g_shadowCacheTargetMapSlot.store(previousTargetMapSlot_, std::memory_order_release);
        g_shadowCacheTargetDepthTarget.store(previousTargetDepthTarget_, std::memory_order_release);
        g_shadowCacheTargetActive.store(previousTargetActive_, std::memory_order_release);
        SetRegistrationShadowCachePass(previousThreadPass_);
    }

    ScopedShadowCacheAccumulator(const ScopedShadowCacheAccumulator&) = delete;
    ScopedShadowCacheAccumulator& operator=(const ScopedShadowCacheAccumulator&) = delete;

private:
    ShadowCachePass previousThreadPass_ = ShadowCachePass::None;
    void* previousAccumulator_ = nullptr;
    bool previousTargetActive_ = false;
    std::uint32_t previousTargetMapSlot_ = (std::numeric_limits<std::uint32_t>::max)();
    std::uint32_t previousTargetDepthTarget_ = (std::numeric_limits<std::uint32_t>::max)();
};

void IncrementRegistrationCounter(std::uint64_t& counter) noexcept
{
    std::atomic_ref<std::uint64_t>(counter).fetch_add(1, std::memory_order_relaxed);
}

void BeginShadowCachePass(ShadowCachePass pass) noexcept
{
    tl_shadowCachePass = pass;
    SetRegistrationShadowCachePass(pass);
}

bool RunOriginalRenderShadowMap(void* light, void* shadowMapData)
{
    if (!s_origRenderShadowMap || !light || !shadowMapData) {
        return false;
    }

    s_origRenderShadowMap(light, shadowMapData);
    return true;
}

bool RunCustomDirectionalMapSlot1ShadowMap(void* light,
                                           void* shadowMapData,
                                           const Scope& scope,
                                           bool buildStatic,
                                           bool& outStaticCaptured)
{
    outStaticCaptured = false;

    if (buildStatic) {
        ReleaseStaticBuildDSVs();
        RememberStaticBuildDSV(ActiveShadowDSV(scope.key));

        BeginShadowCachePass(ShadowCachePass::StaticBuild);
        if (!RunOriginalRenderShadowMap(light, shadowMapData)) {
            return false;
        }

        const std::uint32_t kept = g_registrationStaticKeepCount.load(std::memory_order_acquire);
        const bool hasStaticRegistrations = kept != 0;
        if (hasStaticRegistrations && CaptureStaticCacheFromBuildDSV()) {
            s_directionalMapSlot1Cache.valid = true;
            s_directionalMapSlot1Cache.key = scope.key;
            s_directionalMapSlot1Cache.skipsSinceUpdate = 0;
            s_directionalMapSlot1Cache.stableKeyHits = 0;
            s_directionalMapSlot1Cache.pendingBuildKey = {};
            s_directionalMapSlot1Cache.pendingBuildKeyHits = 0;
            s_directionalMapSlot1Cache.lastDynamicMaxSkip = ShadowCacheDynamicMaxSkips(false);
            outStaticCaptured = true;
        } else {
            s_directionalMapSlot1Cache.valid = false;
            ReleaseStaticDepthCache();
            ReleaseStaticBuildDSVs();
            if (!hasStaticRegistrations) {
                ++s_directionalMapSlot1Cache.emptyStaticBuilds;
            }

            // Static cache build failed. Re-render the same shadow map without
            // filtering so the visible frame is complete.
            BeginShadowCachePass(ShadowCachePass::Passthrough);
            RunOriginalRenderShadowMap(light, shadowMapData);
            return true;
        }
    } else {
        if (!s_staticDepthCache.valid) {
            s_directionalMapSlot1Cache.valid = false;
            BeginShadowCachePass(ShadowCachePass::Passthrough);
            if (!RunOriginalRenderShadowMap(light, shadowMapData)) {
                return false;
            }
            return true;
        }
    }

    // DynamicOverlay uses the vanilla RenderShadowMap lifetime. The clear hook
    // restores the cached static depth into the just-bound DSV and suppresses
    // the original clear before dynamic/unknown casters are registered.
    BeginShadowCachePass(ShadowCachePass::DynamicOverlay);
    if (!RunOriginalRenderShadowMap(light, shadowMapData)) {
        return false;
    }

    return true;
}

void HookedRenderShadowMap(void* light, void* shadowMapData)
{
    const bool telemetryOn = g_mode.load(std::memory_order_relaxed) == Mode::On;
    if (!telemetryOn && !SHADOW_CACHE_DIRECTIONAL_MAPSLOT1_ON) {
        s_origRenderShadowMap(light, shadowMapData);
        return;
    }

    Scope scope;
    scope.shadowMapData = shadowMapData;
    scope.key.light = light;
    scope.key.kind = ClassifyCaller(_ReturnAddress());
    scope.shadowMapDataIndex = ShadowMapDataIndex(light, shadowMapData);
    scope.camera = ReadField<void*>(shadowMapData, 0x40);
    scope.key.cameraSig = ShadowCameraSignature(scope.camera);
    scope.key.dominantLightSig = DominantLightDirectionSignature();
    scope.key.accumulator = ReadField<void*>(shadowMapData, 0x48);
    scope.key.depthTarget = ReadField<std::uint32_t>(shadowMapData, 0x50);
    scope.key.mapSlot = ReadField<std::uint32_t>(shadowMapData, 0x54);
    scope.key.viewport.d0 = ReadField<std::uint32_t>(shadowMapData, 0xD0);
    scope.key.viewport.d4 = ReadField<std::uint32_t>(shadowMapData, 0xD4);
    scope.key.viewport.d8 = ReadField<std::uint32_t>(shadowMapData, 0xD8);
    scope.key.viewport.dc = ReadField<std::uint32_t>(shadowMapData, 0xDC);
    scope.cullingProcess = ReadField<void*>(shadowMapData, 0xE0);
    scope.inDeferredLights = PhaseTelemetry::IsInDeferredLightsImpl();
    scope.start = std::chrono::steady_clock::now();

    auto& cache = s_directionalMapSlot1Cache;
    const bool preCacheValid = cache.valid;
    const bool preDepthValid = s_staticDepthCache.valid;
    const bool preStableKey = CacheKeyStable(cache.key, scope.key);
    const bool cacheEligible = IsDirectionalMapSlot1CacheEligible(scope);
    const bool cacheHit = cacheEligible && DirectionalMapSlot1CacheHit(scope);
    if (cacheEligible) {
        ++s_directionalMapSlot1Cache.eligible;
    }

    tl_scopes.push_back(scope);
    ScopedShadowCacheAccumulator accumulatorScope(scope);

    const char* decision = "unknown";
    const bool staticBuildReady = cacheEligible && !cacheHit && DirectionalMapSlot1StaticBuildReady(scope);
    if (cacheEligible && !cacheHit && staticBuildReady) {
        decision = "rebuild-overlay";
        ++s_directionalMapSlot1Cache.misses;
        ++s_directionalMapSlot1Cache.staticBuilds;

        bool captured = false;
        if (!RunCustomDirectionalMapSlot1ShadowMap(light, shadowMapData, scope, true, captured)) {
            BeginShadowCachePass(ShadowCachePass::Passthrough);
            RunOriginalRenderShadowMap(light, shadowMapData);
        }
    } else if (cacheEligible && !cacheHit) {
        decision = "passthrough-unstable";
        ++s_directionalMapSlot1Cache.misses;
        BeginShadowCachePass(ShadowCachePass::Passthrough);
        RunOriginalRenderShadowMap(light, shadowMapData);
    } else if (cacheEligible && cacheHit) {
        decision = "dynamic-overlay";
        bool captured = false;
        if (!RunCustomDirectionalMapSlot1ShadowMap(light, shadowMapData, scope, false, captured)) {
            BeginShadowCachePass(ShadowCachePass::Passthrough);
            RunOriginalRenderShadowMap(light, shadowMapData);
        }
    } else {
        decision = "passthrough";
        BeginShadowCachePass(ShadowCachePass::Passthrough);
        RunOriginalRenderShadowMap(light, shadowMapData);
    }

    Scope finished = tl_scopes.back();

    if (telemetryOn) {
        const auto t1 = std::chrono::steady_clock::now();
        const auto ns = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - finished.start).count());

        AddToAggregate(s_total, finished, ns);
        AddToAggregate(s_byKey[finished.key], finished, ns);
    }
    if (!cacheEligible) {
        MarkDirectionalMapSlot1CacheUpdated(finished);
    }
    MaybeLogShadowCache(
        finished,
        decision,
        cacheEligible,
        cacheHit,
        preCacheValid,
        preDepthValid,
        preStableKey);
    if (telemetryOn) {
        MaybeLog();
    }

    tl_scopes.pop_back();
    tl_shadowCachePass = ShadowCachePass::None;
    SetRegistrationShadowCachePass(ShadowCachePass::None);
}

bool PatchCall(const char* label, std::uintptr_t callSite, void* hook, std::uintptr_t* outOriginal)
{
    if (callSite < 0x140000000ull) {
        REX::WARN("ShadowTelemetry::Initialize: {} call site failed to resolve ({:#x})", label, callSite);
        return false;
    }
    REL::Relocation<std::uintptr_t> rel{ callSite };
    const auto original = rel.write_call<5>(hook);
    if (outOriginal && !*outOriginal) {
        *outOriginal = original;
    }
    REX::INFO("ShadowTelemetry::Initialize: patched {} call site @ {:#x} -> original {:#x}",
              label, callSite, original);
    return true;
}

}  // anonymous namespace

void OnBSDraw()
{
    if (g_mode.load(std::memory_order_relaxed) != Mode::On || tl_scopes.empty()) return;
    ++tl_scopes.back().bsDraws;
    if (!tl_workScopes.empty()) {
        ++tl_workScopes.back().bsDraws;
    }
}

void OnD3DDraw()
{
    if (g_mode.load(std::memory_order_relaxed) != Mode::On || tl_scopes.empty()) return;
    ++tl_scopes.back().d3dDraws;
    if (!tl_workScopes.empty()) {
        ++tl_workScopes.back().d3dDraws;
    }
}

void OnCommandBufferDraw()
{
    if (g_mode.load(std::memory_order_relaxed) != Mode::On || tl_scopes.empty()) return;
    ++tl_scopes.back().cmdBufDraws;
    if (!tl_workScopes.empty()) {
        ++tl_workScopes.back().cmdBufDraws;
    }
}

bool BeginShadowWork(WorkKind kind, const WorkTarget& target)
{
    if (g_mode.load(std::memory_order_relaxed) != Mode::On || tl_scopes.empty()) {
        return false;
    }

    const auto& shadow = tl_scopes.back().key;
    WorkScope scope;
    scope.key.shadow = shadow;
    scope.key.kind = kind;
    scope.key.owner = target.owner;
    scope.key.head = target.head;
    scope.key.cbData = target.cbData;
    scope.key.commandBuffer = target.commandBuffer;
    scope.key.geometry = target.geometry;
    scope.key.shader = target.shader;
    scope.key.shaderProperty = target.shaderProperty;
    scope.key.techniqueID = target.techniqueID;
    scope.key.passGroupIdx = target.passGroupIdx;
    scope.key.subIdx = target.subIdx;
    scope.key.chainLen = target.chainLen;
    scope.key.srvCount = target.srvCount;
    scope.key.cbRecordCount = target.cbRecordCount;
    scope.key.cbDrawCount = target.cbDrawCount;
    scope.key.cbNonZeroDrawCount = target.cbNonZeroDrawCount;
    scope.key.cbMaxDrawCount = target.cbMaxDrawCount;
    scope.key.cbSrvRecordCount = target.cbSrvRecordCount;
    scope.key.cbMaxSrvRecordCount = target.cbMaxSrvRecordCount;
    scope.key.allowAlpha = target.allowAlpha;
    scope.start = std::chrono::steady_clock::now();
    tl_workScopes.push_back(scope);
    return true;
}

void EndShadowWork(WorkKind kind)
{
    if (g_mode.load(std::memory_order_relaxed) != Mode::On || tl_scopes.empty() || tl_workScopes.empty()) {
        return;
    }

    WorkScope scope = tl_workScopes.back();
    tl_workScopes.pop_back();
    if (scope.key.kind != kind) {
        return;
    }

    const auto t1 = std::chrono::steady_clock::now();
    const auto ns = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - scope.start).count());

    const auto idx = WorkIndex(scope.key.kind);
    AddToWorkAggregate(s_workTotals[idx], ns, scope.bsDraws, scope.d3dDraws, scope.cmdBufDraws);
    AddToWorkAggregate(s_workByKey[scope.key], ns, scope.bsDraws, scope.d3dDraws, scope.cmdBufDraws);
    AddToWorkAggregate(tl_scopes.back().work[idx], ns, scope.bsDraws, scope.d3dDraws, scope.cmdBufDraws);
}

bool IsInShadowMap()
{
    return !tl_scopes.empty();
}

bool IsDirectionalMapSlot1Shadow()
{
    return g_shadowCacheTargetActive.load(std::memory_order_acquire) &&
           g_shadowCacheTargetMapSlot.load(std::memory_order_acquire) == kDirectionalCacheMapSlot;
}

bool IsShadowCacheActiveForCurrentShadow()
{
    return SHADOW_CACHE_DIRECTIONAL_MAPSLOT1_ON &&
           IsDirectionalMapSlot1Shadow();
}

bool IsShadowCacheStaticBuildPass()
{
    return IsShadowCacheActiveForCurrentShadow() &&
           CurrentResolvedRegistrationPass() == ShadowCachePass::StaticBuild;
}

bool IsShadowCacheDynamicOverlayPass()
{
    return IsShadowCacheActiveForCurrentShadow() &&
           CurrentResolvedRegistrationPass() == ShadowCachePass::DynamicOverlay;
}

bool IsShadowCacheRegistrationFilterActive(void* accumulator, void* geometry)
{
    (void)geometry;
    return g_registrationShadowCacheActive.load(std::memory_order_acquire) &&
           RegistrationAccumulatorMatches(accumulator);
}

void NoteShadowCacheShadowMapOrMaskHook(bool active)
{
    const ShadowCachePass pass = CurrentResolvedRegistrationPass();
    const std::size_t passIdx = PassIndex(pass);
    IncrementRegistrationCounter(s_directionalMapSlot1Cache.shadowMapOrMaskHookCalls);
    IncrementRegistrationCounter(s_directionalMapSlot1Cache.shadowMapOrMaskHookCallsByPass[passIdx]);
    if (active) {
        IncrementRegistrationCounter(s_directionalMapSlot1Cache.shadowMapOrMaskHookActiveCalls);
        IncrementRegistrationCounter(s_directionalMapSlot1Cache.shadowMapOrMaskHookActiveCallsByPass[passIdx]);
    }
}

void NoteShadowCacheShadowMapOrMaskHookDetail(bool active, void* accumulator)
{
    NoteShadowCacheShadowMapOrMaskHook(active);
    (void)accumulator;
}

void NoteShadowCacheRenderPassSplit(bool kept, bool isStaticCaster)
{
    if (!SHADOW_CACHE_DIRECTIONAL_MAPSLOT1_ON ||
        !g_shadowCacheTargetActive.load(std::memory_order_acquire) ||
        g_shadowCacheTargetMapSlot.load(std::memory_order_acquire) != kDirectionalCacheMapSlot) {
        return;
    }

    switch (CurrentResolvedRegistrationPass()) {
    case ShadowCachePass::StaticBuild:
        if (kept && isStaticCaster) {
            IncrementRegistrationCounter(s_directionalMapSlot1Cache.staticKept);
            g_registrationStaticKeepCount.fetch_add(1, std::memory_order_relaxed);
        } else {
            IncrementRegistrationCounter(s_directionalMapSlot1Cache.staticSkipped);
        }
        break;
    case ShadowCachePass::DynamicOverlay:
        if (kept) {
            IncrementRegistrationCounter(s_directionalMapSlot1Cache.overlayKept);
        } else {
            IncrementRegistrationCounter(s_directionalMapSlot1Cache.overlaySkipped);
        }
        break;
    default:
        break;
    }
}

void ResetShadowCacheState()
{
    ReleaseStaticDepthCache();
    ReleaseStaticBuildDSVs();
    s_directionalMapSlot1Cache.valid = false;
    s_directionalMapSlot1Cache.skipsSinceUpdate = 0;
    s_directionalMapSlot1Cache.stableKeyHits = 0;
    s_directionalMapSlot1Cache.pendingBuildKey = {};
    s_directionalMapSlot1Cache.pendingBuildKeyHits = 0;
    s_directionalMapSlot1Cache.lastDynamicMaxSkip = 0;
    s_directionalMapSlot1Cache.key = {};
    g_registrationShadowCacheAccumulator.store(nullptr, std::memory_order_release);
    g_shadowCacheTargetActive.store(false, std::memory_order_release);
    g_shadowCacheTargetMapSlot.store((std::numeric_limits<std::uint32_t>::max)(), std::memory_order_release);
    g_shadowCacheTargetDepthTarget.store((std::numeric_limits<std::uint32_t>::max)(), std::memory_order_release);
    SetRegistrationShadowCachePass(ShadowCachePass::None);
    ResetWindow();
}

bool HandleShadowCacheClearDepthStencilView(
    REX::W32::ID3D11DeviceContext* context,
    REX::W32::ID3D11DepthStencilView* dsv,
    std::uint32_t,
    float,
    std::uint8_t)
{
    if (!SHADOW_CACHE_DIRECTIONAL_MAPSLOT1_ON ||
        !g_shadowCacheTargetActive.load(std::memory_order_acquire) ||
        g_shadowCacheTargetMapSlot.load(std::memory_order_acquire) != kDirectionalCacheMapSlot) {
        return false;
    }

    if (CurrentResolvedRegistrationPass() == ShadowCachePass::StaticBuild) {
        RememberStaticBuildDSV(dsv);
        return false;
    }

    if (CurrentResolvedRegistrationPass() != ShadowCachePass::DynamicOverlay) {
        return false;
    }

    const bool restored = RestoreStaticCacheToDSV(context, dsv);
    if (!restored) {
        ++s_directionalMapSlot1Cache.restoreFailures;
        s_directionalMapSlot1Cache.valid = false;
        ReleaseStaticDepthCache();
        SetRegistrationShadowCachePass(ShadowCachePass::Passthrough);
        return false;
    }

    return true;
}

bool Initialize()
{
    if (s_installed) {
        REX::INFO("ShadowTelemetry::Initialize: already installed; skipping");
        return true;
    }

    if (g_mode.load(std::memory_order_relaxed) != Mode::On &&
        !SHADOW_CACHE_DIRECTIONAL_MAPSLOT1_ON) {
        REX::INFO("ShadowTelemetry::Initialize: mode=off and shadow cache=off; hooks skipped");
        return true;
    }

    if (REX::FModule::GetRuntimeIndex() != REX::FModule::Runtime::kOG) {
        REX::WARN("ShadowTelemetry::Initialize: hooks skipped; call-site offsets are verified for OG runtime only");
        return false;
    }

    bool ok = true;
    std::uintptr_t originalShadow = 0;
    std::uintptr_t originalScene = 0;

    const bool needsRenderSceneHook =
        g_mode.load(std::memory_order_relaxed) == Mode::On ||
        SHADOW_CACHE_DIRECTIONAL_MAPSLOT1_ON;
    if (needsRenderSceneHook) {
        const auto renderShadowMapAddr = ptr_RenderShadowMap.address();
        ok &= PatchCall("BSShadowLight::RenderShadowMap -> BSShaderUtil::RenderScene",
                        renderShadowMapAddr + kRenderSceneCallOffsetOG,
                        reinterpret_cast<void*>(&HookedRenderScene),
                        &originalScene);
        s_origRenderScene = reinterpret_cast<RenderScene_t>(originalScene);
    }

    ok &= PatchCall("BSShadowDirectionalLight::Render -> RenderShadowMap",
                    ptr_ShadowDirectionalRender.address() + kDirectionalRenderShadowMapCallOffsetOG,
                    reinterpret_cast<void*>(&HookedRenderShadowMap),
                    &originalShadow);
    s_origRenderShadowMap = reinterpret_cast<RenderShadowMap_t>(originalShadow);

    if (!s_origRenderShadowMap ||
        (needsRenderSceneHook && !s_origRenderScene)) {
        REX::WARN("ShadowTelemetry::Initialize: original function capture failed (RenderShadowMap={}, RenderScene={}, telemetryMode={})",
                  reinterpret_cast<void*>(s_origRenderShadowMap),
                  reinterpret_cast<void*>(s_origRenderScene),
                  static_cast<int>(g_mode.load(std::memory_order_relaxed)));
        return false;
    }

    s_lastLogTime = std::chrono::steady_clock::now();
    s_installed = ok;
    REX::INFO("ShadowTelemetry::Initialize: hooks {} logging every {:.1f}s",
              ok ? "installed" : "partially installed", kLogIntervalSecs);
    return ok;
}

}  // namespace ShadowTelemetry

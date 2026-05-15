#include <Global.h>
#include <PCH.h>
#include <PassOcclusion.h>
#include <PhaseTelemetry.h>
#include <Plugin.h>
#include <ShaderResources.h>

namespace PassOcclusion
{
struct PassOcclusionState
{
    REX::W32::ID3D11Query* query = nullptr;
    bool pending = false;
    std::uint8_t hiddenStreak = 0;
    std::uint8_t skippedSinceRefresh = 0;
    std::uint64_t lastTouchedFrame = 0;
};

namespace
{

    enum class PassOcclusionDomain : std::uint8_t
    {
        None,
        MainGBuffer,
        Shadow
    };

    struct PassOcclusionKey
    {
        RE::BSGeometry* geometry;
        RE::BSShader* shader;
        RE::BSShaderProperty* shaderProperty;
        std::uint32_t techniqueID;
        std::uint32_t group;
        std::uint32_t viewportSig;
        PassOcclusionDomain domain;

        bool operator==(const PassOcclusionKey& rhs) const noexcept
        {
            return geometry == rhs.geometry &&
                   shader == rhs.shader &&
                   shaderProperty == rhs.shaderProperty &&
                   techniqueID == rhs.techniqueID &&
                   group == rhs.group &&
                   viewportSig == rhs.viewportSig &&
                   domain == rhs.domain;
        }
    };

    struct PassOcclusionKeyHash
    {
        std::size_t operator()(const PassOcclusionKey& k) const noexcept
        {
            std::size_t h = std::hash<void*>{}(k.geometry);
            auto mix = [&h](std::size_t v) {
                h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
            };
            mix(std::hash<void*>{}(k.shader));
            mix(std::hash<void*>{}(k.shaderProperty));
            mix(std::hash<std::uint32_t>{}(k.techniqueID));
            mix(std::hash<std::uint32_t>{}(k.group));
            mix(std::hash<std::uint32_t>{}(k.viewportSig));
            mix(std::hash<std::uint8_t>{}(static_cast<std::uint8_t>(k.domain)));
            return h;
        }
    };

    struct PassOcclusionEarlyGeometryState
    {
        std::uint8_t hiddenStreak = 0;
        std::uint8_t skippedSinceRefresh = 0;
        std::uint64_t lastTouchedFrame = 0;
    };

    struct CameraOcclusionSnapshot
    {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        float dx = 0.0f;
        float dy = 0.0f;
        float dz = 1.0f;
    };

    using PassOcclusionArenaGatePatch = ArenaGatePatch;

    constexpr std::size_t kPassOcclusionMaxEntries = 32768;
    constexpr std::uint32_t kPassOcclusionMainQueryBudget = 384;
    constexpr std::uint32_t kPassOcclusionShadowQueryBudget = 192;
    constexpr std::uint32_t kPassOcclusionMainSkipBudget = 2048;
    constexpr std::uint32_t kPassOcclusionShadowSkipBudget = 512;
    constexpr std::uint8_t kPassOcclusionMainHiddenFrames = 2;
    constexpr std::uint8_t kPassOcclusionShadowHiddenFrames = 3;
    constexpr std::uint8_t kPassOcclusionMainMaxSkips = 4;
    constexpr std::uint8_t kPassOcclusionShadowMaxSkips = 1;
    constexpr std::uint64_t kPassOcclusionStaleFrames = 600;
    constexpr std::uint32_t kPassOcclusionPrunePeriod = 120;
    constexpr std::uint64_t kPassOcclusionTelemetryPeriod = 300;
    constexpr std::size_t kCullerBlockStride = 0x3A70;
    constexpr std::size_t kCullerBlockObjectArrayOffset = 0x2060;
    constexpr std::size_t kCullerBlockGateArrayOffset = 0x3060;
    constexpr std::size_t kCullerBlockGateStride = 5;
    constexpr std::size_t kCullerBlockCountOffset = 0x3A68;
    constexpr std::size_t kCullerBlockNextOffset = 0x3A70;
    constexpr std::size_t kCullerArenaHeadOffset = 0x00;
    constexpr std::size_t kCullerArenaFirstBlockOffset = 0x10;
    constexpr std::size_t kCullerArenaCurrentOffset = 0x20;
    constexpr std::size_t kCullerArenaTailOffset = 0x28;
    constexpr std::size_t kNiAVObjectTypeOffset = 0x158;
    constexpr bool kPassOcclusionArenaGateEnabled = false;

    enum class PassOcclusionGeometryReject : std::uint8_t
    {
        None,
        NullOrSkinned,
        Type,
        StaticClass,
        Radius,
        NearCamera,
        ActorTagged
    };

    enum class PassOcclusionResolveResult : std::uint8_t
    {
        None,
        Pending,
        Hidden,
        Visible
    };

    struct PassOcclusionTelemetry
    {
        std::uint64_t attempts = 0;
        std::uint64_t commandAttempts = 0;
        std::uint64_t immediateAttempts = 0;
        std::uint64_t disabled = 0;
        std::uint64_t badInput = 0;
        std::uint64_t noDomain = 0;
        std::uint64_t noDomainOtherDepth = 0;
        std::uint64_t noDomainMainNoRT = 0;
        std::uint64_t noDomainMainNotDeferredPrePass = 0;
        std::uint64_t noDomainMainGroup = 0;
        std::uint64_t noDomainShadowHasRT = 0;
        std::uint64_t rejectAlpha = 0;
        std::uint64_t rejectEmpty = 0;
        std::uint64_t rejectTooLong = 0;
        std::uint64_t rejectGeomNullOrSkinned = 0;
        std::uint64_t rejectGeomType = 0;
        std::uint64_t rejectGeomRadius = 0;
        std::uint64_t rejectGeomNearCamera = 0;
        std::uint64_t rejectGeomActorTagged = 0;
        std::uint64_t eligibleMain = 0;
        std::uint64_t eligibleShadow = 0;
        std::uint64_t queryBudgetDenied = 0;
        std::uint64_t cacheFull = 0;
        std::uint64_t noDevice = 0;
        std::uint64_t createQueryFailed = 0;
        std::uint64_t stateCreated = 0;
        std::uint64_t queryBegun = 0;
        std::uint64_t queryEnded = 0;
        std::uint64_t queryPending = 0;
        std::uint64_t resolvedZero = 0;
        std::uint64_t resolvedVisible = 0;
        std::uint64_t skipMain = 0;
        std::uint64_t skipShadow = 0;
        std::uint64_t earlySkipMain = 0;
        std::uint64_t earlyNoPhase = 0;
        std::uint64_t earlyCameraUnstable = 0;
        std::uint64_t earlyNoCache = 0;
        std::uint64_t earlyHiddenTooLow = 0;
        std::uint64_t earlyMaxSkips = 0;
        std::uint64_t earlyBudgetDenied = 0;
        std::uint64_t arenaCalls = 0;
        std::uint64_t arenaBlocks = 0;
        std::uint64_t arenaEntries = 0;
        std::uint64_t arenaGateNonZero = 0;
        std::uint64_t arenaCandidates = 0;
        std::uint64_t arenaPatched = 0;
        std::uint64_t arenaRejectType = 0;
        std::uint64_t arenaRejectNoObject = 0;
        std::uint64_t cameraStableFrames = 0;
        std::uint64_t cameraUnstableFrames = 0;
        std::uint64_t cameraInvalidFrames = 0;
        std::uint64_t pruned = 0;
        std::array<std::uint64_t, 32> noDomainByGroup{};
        std::array<std::uint64_t, 32> mainByGroup{};
        std::array<std::uint64_t, 32> shadowByGroup{};
    };

    std::unordered_map<PassOcclusionKey, PassOcclusionState, PassOcclusionKeyHash> g_passOcclusionCache;
    std::unordered_map<RE::BSGeometry*, PassOcclusionEarlyGeometryState> g_passOcclusionEarlyGeometryCache;
    PassOcclusionTelemetry g_passOcclusionTelemetry;
    std::uint64_t g_passOcclusionFrame = 0;
    std::uint64_t g_passOcclusionLastTelemetryFrame = 0;
    std::uint32_t g_passOcclusionMainQueries = 0;
    std::uint32_t g_passOcclusionShadowQueries = 0;
    std::uint32_t g_passOcclusionMainSkips = 0;
    std::uint32_t g_passOcclusionShadowSkips = 0;
    bool g_passOcclusionCameraStable = false;
    bool g_passOcclusionHaveCamera = false;
    CameraOcclusionSnapshot g_passOcclusionPrevCamera;

    bool IsMainGBufferGroup(unsigned int group)
    {
        switch (group) {
            case 0:
            case 1:
            case 4:
            case 7:
            case 8:
            case 9:
            case 10:
            case 14:
                return true;
            default:
                return false;
        }
    }

    bool IsStaticGeometryType(std::uint8_t type)
    {
        switch (type) {
            case 3:   // BSTriShape
            case 5:   // BSMeshLODTriShape / merge-style static geometry
            case 6:   // BSMultiIndexTriShape
            case 7:   // static tri variant
            case 15:  // BSCombinedTriShape / precombined geometry
                return true;
            default:
                return false;
        }
    }

    bool IsActorTaggedGeometry(RE::BSGeometry* geometry)
    {
        return IsActorDrawTaggedGeometry_Internal(geometry);
    }

    void CountPassOcclusionGeometryReject(PassOcclusionGeometryReject reject)
    {
        switch (reject) {
            case PassOcclusionGeometryReject::NullOrSkinned:
                ++g_passOcclusionTelemetry.rejectGeomNullOrSkinned;
                break;
            case PassOcclusionGeometryReject::Type:
                ++g_passOcclusionTelemetry.rejectGeomType;
                break;
            case PassOcclusionGeometryReject::StaticClass:
                ++g_passOcclusionTelemetry.rejectGeomType;
                break;
            case PassOcclusionGeometryReject::Radius:
                ++g_passOcclusionTelemetry.rejectGeomRadius;
                break;
            case PassOcclusionGeometryReject::NearCamera:
                ++g_passOcclusionTelemetry.rejectGeomNearCamera;
                break;
            case PassOcclusionGeometryReject::ActorTagged:
                ++g_passOcclusionTelemetry.rejectGeomActorTagged;
                break;
            default:
                break;
        }
    }

    bool IsGeometryEligibleForPassOcclusion(
        RE::BSGeometry* geometry,
        PassOcclusionDomain domain,
        PassOcclusionGeometryReject* reject = nullptr)
    {
        if (!geometry || geometry->skinInstance) {
            if (reject) *reject = PassOcclusionGeometryReject::NullOrSkinned;
            return false;
        }
        if (!IsStaticGeometryType(geometry->type)) {
            if (reject) *reject = PassOcclusionGeometryReject::Type;
            return false;
        }
        if (domain == PassOcclusionDomain::MainGBuffer &&
            !IsPrecombineShadowGeometry_Internal(geometry)) {
            if (reject) *reject = PassOcclusionGeometryReject::StaticClass;
            return false;
        }

        const float radius = geometry->worldBound.fRadius;
        if (!(radius > 0.0f) || !std::isfinite(radius)) {
            if (reject) *reject = PassOcclusionGeometryReject::Radius;
            return false;
        }

        const float minRadius = domain == PassOcclusionDomain::Shadow ? 256.0f : 192.0f;
        const float maxRadius = domain == PassOcclusionDomain::Shadow ? 24000.0f : 36000.0f;
        if (radius < minRadius || radius > maxRadius) {
            if (reject) *reject = PassOcclusionGeometryReject::Radius;
            return false;
        }

        const auto& c = geometry->worldBound.center;
        const float dx = c.x - g_customBufferData.camX;
        const float dy = c.y - g_customBufferData.camY;
        const float dz = c.z - g_customBufferData.camZ;
        const float distSq = dx * dx + dy * dy + dz * dz;
        const float nearLimit = domain == PassOcclusionDomain::Shadow ? 768.0f : 384.0f;
        if (distSq < nearLimit * nearLimit) {
            if (reject) *reject = PassOcclusionGeometryReject::NearCamera;
            return false;
        }

        if (IsActorTaggedGeometry(geometry)) {
            if (reject) *reject = PassOcclusionGeometryReject::ActorTagged;
            return false;
        }

        if (reject) *reject = PassOcclusionGeometryReject::None;
        return true;
    }

    bool IsPassChainEligibleForOcclusion(BSRenderPassLayout* head, bool allowAlpha, PassOcclusionDomain domain)
    {
        if (!head) {
            ++g_passOcclusionTelemetry.rejectEmpty;
            return false;
        }
        if (allowAlpha) {
            ++g_passOcclusionTelemetry.rejectAlpha;
            return false;
        }

        unsigned int count = 0;
        for (auto* pass = head; pass && count < 128; pass = pass->passGroupNext, ++count) {
            PassOcclusionGeometryReject reject = PassOcclusionGeometryReject::None;
            if (!IsGeometryEligibleForPassOcclusion(pass->geometry, domain, &reject)) {
                CountPassOcclusionGeometryReject(reject);
                return false;
            }
        }
        if (count == 0) {
            ++g_passOcclusionTelemetry.rejectEmpty;
            return false;
        }
        if (count >= 128) {
            ++g_passOcclusionTelemetry.rejectTooLong;
            return false;
        }
        return true;
    }

    PassOcclusionDomain GetCurrentPassOcclusionDomain(unsigned int group)
    {
        const UINT depthTarget = ShaderResources::GetCurrentDepthTargetIndex();
        const bool hasRT = ShaderResources::HasCurrentRenderTarget();

        if (depthTarget == static_cast<UINT>(ShaderResources::MAIN_DEPTHSTENCIL_TARGET) &&
            hasRT &&
            IsMainGBufferGroup(group)) {
            return PassOcclusionDomain::MainGBuffer;
        }

        if (depthTarget == static_cast<UINT>(ShaderResources::DepthStencilTarget::kShadowMap) && !hasRT) {
            return PassOcclusionDomain::Shadow;
        }

        return PassOcclusionDomain::None;
    }

    std::string FormatPassOcclusionGroupCounts(const std::array<std::uint64_t, 32>& counts)
    {
        std::ostringstream ss;
        bool first = true;
        for (std::size_t i = 0; i < counts.size(); ++i) {
            if (counts[i] == 0) {
                continue;
            }
            if (!first) {
                ss << ',';
            }
            ss << i << ':' << counts[i];
            first = false;
        }
        return first ? "-" : ss.str();
    }

    std::uint32_t CaptureViewportSignature(REX::W32::ID3D11DeviceContext* context, PassOcclusionDomain domain)
    {
        if (!context || domain != PassOcclusionDomain::Shadow) {
            return 0;
        }

        std::uint32_t count = 1;
        REX::W32::D3D11_VIEWPORT vp{};
        context->RSGetViewports(&count, &vp);
        if (count == 0) {
            return 0;
        }

        const auto q = [](float v) -> std::uint32_t {
            return static_cast<std::uint32_t>((std::max)(0, static_cast<int>(v + 0.5f)));
        };

        std::uint32_t h = q(vp.topLeftX) & 0x3ffu;
        h |= (q(vp.topLeftY) & 0x3ffu) << 10;
        h ^= (q(vp.width) & 0xfffu) << 4;
        h ^= (q(vp.height) & 0xfffu) << 16;
        return h;
    }

    BSRenderPassLayout* GetRenderBatchNodeHead(void* batchRenderer, int group, unsigned int subIdx)
    {
        return GetRenderBatchNodeHead_Internal(batchRenderer, group, subIdx);
    }

    PassOcclusionResolveResult ResolvePassOcclusionQuery(REX::W32::ID3D11DeviceContext* context, PassOcclusionState& state)
    {
        if (!context || !state.query || !state.pending) {
            return PassOcclusionResolveResult::None;
        }

        std::uint64_t samples = 0;
        const HRESULT hr = context->GetData(
            state.query,
            &samples,
            sizeof(samples),
            REX::W32::D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if (hr != S_OK) {
            ++g_passOcclusionTelemetry.queryPending;
            return PassOcclusionResolveResult::Pending;
        }

        state.pending = false;
        if (samples == 0) {
            ++g_passOcclusionTelemetry.resolvedZero;
            if (state.hiddenStreak < 255) {
                ++state.hiddenStreak;
            }
            return PassOcclusionResolveResult::Hidden;
        } else {
            ++g_passOcclusionTelemetry.resolvedVisible;
            state.hiddenStreak = 0;
            state.skippedSinceRefresh = 0;
            return PassOcclusionResolveResult::Visible;
        }
    }

    PassOcclusionState* FindOrCreatePassOcclusionState(const PassOcclusionKey& key, bool allowCreate)
    {
        auto it = g_passOcclusionCache.find(key);
        if (it != g_passOcclusionCache.end()) {
            return &it->second;
        }
        if (!allowCreate) {
            return nullptr;
        }
        if (g_passOcclusionCache.size() >= kPassOcclusionMaxEntries) {
            ++g_passOcclusionTelemetry.cacheFull;
            return nullptr;
        }
        if (!g_rendererData || !g_rendererData->device) {
            ++g_passOcclusionTelemetry.noDevice;
            return nullptr;
        }

        PassOcclusionState state;
        REX::W32::D3D11_QUERY_DESC desc{};
        desc.query = REX::W32::D3D11_QUERY_OCCLUSION;
        desc.miscFlags = 0;
        if (FAILED(g_rendererData->device->CreateQuery(&desc, &state.query)) || !state.query) {
            ++g_passOcclusionTelemetry.createQueryFailed;
            return nullptr;
        }

        auto [newIt, inserted] = g_passOcclusionCache.emplace(key, state);
        if (!inserted) {
            if (state.query) {
                state.query->Release();
            }
        } else {
            ++g_passOcclusionTelemetry.stateCreated;
        }
        return &newIt->second;
    }

    bool ConsumePassOcclusionSkipBudget(PassOcclusionDomain domain);

    void SyncPassOcclusionEarlyGeometryState(
        RE::BSGeometry* geometry,
        PassOcclusionDomain domain,
        const PassOcclusionState& state,
        PassOcclusionResolveResult result)
    {
        if (!geometry || domain != PassOcclusionDomain::MainGBuffer) {
            return;
        }
        if (result != PassOcclusionResolveResult::Hidden &&
            result != PassOcclusionResolveResult::Visible) {
            return;
        }

        auto& early = g_passOcclusionEarlyGeometryCache[geometry];
        early.hiddenStreak = state.hiddenStreak;
        early.skippedSinceRefresh = 0;
        early.lastTouchedFrame = g_passOcclusionFrame;
    }

    bool ShouldEarlyCullRegisterObjectStandardImpl(RE::BSGeometry* geometry)
    {
        if (!PASS_LEVEL_OCCLUSION_ON || !geometry) {
            return false;
        }
        if (!PhaseTelemetry::IsInMainAccum()) {
            ++g_passOcclusionTelemetry.earlyNoPhase;
            return false;
        }
        if (!g_passOcclusionCameraStable) {
            ++g_passOcclusionTelemetry.earlyCameraUnstable;
            return false;
        }

        PassOcclusionGeometryReject reject = PassOcclusionGeometryReject::None;
        if (!IsGeometryEligibleForPassOcclusion(geometry, PassOcclusionDomain::MainGBuffer, &reject)) {
            CountPassOcclusionGeometryReject(reject);
            return false;
        }

        auto it = g_passOcclusionEarlyGeometryCache.find(geometry);
        if (it == g_passOcclusionEarlyGeometryCache.end()) {
            ++g_passOcclusionTelemetry.earlyNoCache;
            return false;
        }

        auto& early = it->second;
        if (early.hiddenStreak < kPassOcclusionMainHiddenFrames) {
            ++g_passOcclusionTelemetry.earlyHiddenTooLow;
            return false;
        }
        if (early.skippedSinceRefresh >= kPassOcclusionMainMaxSkips) {
            ++g_passOcclusionTelemetry.earlyMaxSkips;
            return false;
        }
        if (!ConsumePassOcclusionSkipBudget(PassOcclusionDomain::MainGBuffer)) {
            ++g_passOcclusionTelemetry.earlyBudgetDenied;
            return false;
        }

        ++early.skippedSinceRefresh;
        early.lastTouchedFrame = g_passOcclusionFrame;
        ++g_passOcclusionTelemetry.skipMain;
        ++g_passOcclusionTelemetry.earlySkipMain;
        return true;
    }

    std::uintptr_t ReadArenaPtr(std::uintptr_t base, std::size_t offset)
    {
        return base ? *reinterpret_cast<std::uintptr_t*>(base + offset) : 0;
    }

    std::uintptr_t ResolveCullerArenaEnd(std::uintptr_t arena)
    {
        const auto current = ReadArenaPtr(arena, kCullerArenaCurrentOffset);
        const auto firstBlock = ReadArenaPtr(arena, kCullerArenaFirstBlockOffset);
        if (firstBlock && current >= firstBlock + kCullerBlockNextOffset) {
            const auto next = ReadArenaPtr(firstBlock, kCullerBlockNextOffset);
            if (next) {
                return next;
            }
        }
        return current;
    }

    std::uintptr_t ResolveCullerArenaStart(std::uintptr_t arena)
    {
        const auto current = ReadArenaPtr(arena, kCullerArenaCurrentOffset);
        const auto tail = ReadArenaPtr(arena, kCullerArenaTailOffset);
        if (tail == current) {
            return ResolveCullerArenaEnd(arena);
        }
        return ReadArenaPtr(arena, kCullerArenaHeadOffset) ? tail : 0;
    }

    void PatchPassOcclusionArenaGates(void* arena, std::vector<PassOcclusionArenaGatePatch>& patches)
    {
        if constexpr (!kPassOcclusionArenaGateEnabled) {
            (void)arena;
            (void)patches;
            return;
        }

        ++g_passOcclusionTelemetry.arenaCalls;
        if (!PASS_LEVEL_OCCLUSION_ON ||
            !PhaseTelemetry::IsInMainAccum() ||
            !g_passOcclusionCameraStable ||
            !arena) {
            return;
        }

        const auto arenaBase = reinterpret_cast<std::uintptr_t>(arena);
        const auto endBlock = ResolveCullerArenaEnd(arenaBase);
        std::uintptr_t block = ResolveCullerArenaStart(arenaBase);
        for (std::uint32_t blockGuard = 0; block && block != endBlock && blockGuard < 4096; ++blockGuard) {
            ++g_passOcclusionTelemetry.arenaBlocks;
            const auto count = *reinterpret_cast<std::uint32_t*>(block + kCullerBlockCountOffset);
            const auto safeCount = (std::min)(count, 1024u);
            g_passOcclusionTelemetry.arenaEntries += safeCount;
            for (std::uint32_t i = 0; i < safeCount; ++i) {
                auto* gate = reinterpret_cast<std::uint16_t*>(
                    block + kCullerBlockGateArrayOffset + static_cast<std::size_t>(i) * kCullerBlockGateStride);
                if (*gate == 0) {
                    continue;
                }
                ++g_passOcclusionTelemetry.arenaGateNonZero;

                const auto object = *reinterpret_cast<std::uintptr_t*>(
                    block + kCullerBlockObjectArrayOffset + static_cast<std::size_t>(i) * sizeof(void*));
                if (!object) {
                    ++g_passOcclusionTelemetry.arenaRejectNoObject;
                    continue;
                }

                const auto type = *reinterpret_cast<std::uint8_t*>(object + kNiAVObjectTypeOffset);
                if (!IsStaticGeometryType(type)) {
                    ++g_passOcclusionTelemetry.arenaRejectType;
                    continue;
                }

                ++g_passOcclusionTelemetry.arenaCandidates;
                auto* geometry = reinterpret_cast<RE::BSGeometry*>(object);
                if (!ShouldEarlyCullRegisterObjectStandardImpl(geometry)) {
                    continue;
                }

                patches.push_back({ gate, *gate });
                *gate = 0;
                ++g_passOcclusionTelemetry.arenaPatched;
            }

            block = ReadArenaPtr(block, kCullerBlockNextOffset);
        }
    }

    void RestorePassOcclusionArenaGates(std::vector<PassOcclusionArenaGatePatch>& patches)
    {
        for (const auto& patch : patches) {
            if (patch.gate) {
                *patch.gate = patch.original;
            }
        }
        patches.clear();
    }

    bool ConsumePassOcclusionSkipBudget(PassOcclusionDomain domain)
    {
        if (domain == PassOcclusionDomain::Shadow) {
            if (g_passOcclusionShadowSkips >= kPassOcclusionShadowSkipBudget) {
                return false;
            }
            ++g_passOcclusionShadowSkips;
            return true;
        }

        if (g_passOcclusionMainSkips >= kPassOcclusionMainSkipBudget) {
            return false;
        }
        ++g_passOcclusionMainSkips;
        return true;
    }

    bool ConsumePassOcclusionQueryBudget(PassOcclusionDomain domain)
    {
        if (domain == PassOcclusionDomain::Shadow) {
            if (g_passOcclusionShadowQueries >= kPassOcclusionShadowQueryBudget) {
                return false;
            }
            ++g_passOcclusionShadowQueries;
            return true;
        }

        if (g_passOcclusionMainQueries >= kPassOcclusionMainQueryBudget) {
            return false;
        }
        ++g_passOcclusionMainQueries;
        return true;
    }

    Decision BeginPassOcclusionDecision(
        REX::W32::ID3D11DeviceContext* context,
        void* batchRenderer,
        BSRenderPassLayout* head,
        unsigned int group,
        bool allowAlpha,
        bool commandBufferPath)
    {
        Decision decision;
        if (!PASS_LEVEL_OCCLUSION_ON) {
            return decision;
        }

        ++g_passOcclusionTelemetry.attempts;
        if (commandBufferPath) {
            ++g_passOcclusionTelemetry.commandAttempts;
        } else {
            ++g_passOcclusionTelemetry.immediateAttempts;
        }

        if (!context || !batchRenderer || !head || !IsRenderBatchesActive_Internal()) {
            ++g_passOcclusionTelemetry.badInput;
            return decision;
        }

        const UINT depthTarget = ShaderResources::GetCurrentDepthTargetIndex();
        const bool hasRT = ShaderResources::HasCurrentRenderTarget();
        const bool inDeferredPrePass = PhaseTelemetry::IsInDeferredPrePass();
        const bool mainGroup = IsMainGBufferGroup(group);
        const PassOcclusionDomain domain = GetCurrentPassOcclusionDomain(group);
        if (domain == PassOcclusionDomain::None) {
            ++g_passOcclusionTelemetry.noDomain;
            if (group < g_passOcclusionTelemetry.noDomainByGroup.size()) {
                ++g_passOcclusionTelemetry.noDomainByGroup[group];
            }
            if (depthTarget == static_cast<UINT>(ShaderResources::MAIN_DEPTHSTENCIL_TARGET)) {
                if (!hasRT) {
                    ++g_passOcclusionTelemetry.noDomainMainNoRT;
                }
                if (!inDeferredPrePass) {
                    ++g_passOcclusionTelemetry.noDomainMainNotDeferredPrePass;
                }
                if (!mainGroup) {
                    ++g_passOcclusionTelemetry.noDomainMainGroup;
                }
            } else if (depthTarget == static_cast<UINT>(ShaderResources::DepthStencilTarget::kShadowMap)) {
                if (hasRT) {
                    ++g_passOcclusionTelemetry.noDomainShadowHasRT;
                }
            } else {
                ++g_passOcclusionTelemetry.noDomainOtherDepth;
            }
            return decision;
        }
        if (!IsPassChainEligibleForOcclusion(head, allowAlpha, domain)) {
            return decision;
        }
        if (domain == PassOcclusionDomain::Shadow) {
            ++g_passOcclusionTelemetry.eligibleShadow;
            if (group < g_passOcclusionTelemetry.shadowByGroup.size()) {
                ++g_passOcclusionTelemetry.shadowByGroup[group];
            }
        } else {
            ++g_passOcclusionTelemetry.eligibleMain;
            if (group < g_passOcclusionTelemetry.mainByGroup.size()) {
                ++g_passOcclusionTelemetry.mainByGroup[group];
            }
        }

        PassOcclusionKey key{
            head->geometry,
            head->shader,
            head->shaderProperty,
            head->techniqueID,
            group,
            CaptureViewportSignature(context, domain),
            domain
        };

        bool queryBudgetAlreadyConsumed = false;
        auto* state = FindOrCreatePassOcclusionState(key, false);
        if (!state) {
            if (!ConsumePassOcclusionQueryBudget(domain)) {
                ++g_passOcclusionTelemetry.queryBudgetDenied;
                return decision;
            }
            queryBudgetAlreadyConsumed = true;
            state = FindOrCreatePassOcclusionState(key, true);
        }
        if (!state) {
            return decision;
        }

        state->lastTouchedFrame = g_passOcclusionFrame;
        const auto resolveResult = ResolvePassOcclusionQuery(context, *state);
        SyncPassOcclusionEarlyGeometryState(key.geometry, domain, *state, resolveResult);

        const std::uint8_t requiredHidden =
            domain == PassOcclusionDomain::Shadow ? kPassOcclusionShadowHiddenFrames : kPassOcclusionMainHiddenFrames;
        const std::uint8_t maxSkips =
            domain == PassOcclusionDomain::Shadow ? kPassOcclusionShadowMaxSkips : kPassOcclusionMainMaxSkips;

        if (g_passOcclusionCameraStable &&
            state->hiddenStreak >= requiredHidden &&
            state->skippedSinceRefresh < maxSkips &&
            ConsumePassOcclusionSkipBudget(domain)) {
            ++state->skippedSinceRefresh;
            if (domain == PassOcclusionDomain::Shadow) {
                ++g_passOcclusionTelemetry.skipShadow;
            } else {
                ++g_passOcclusionTelemetry.skipMain;
            }
            decision.state = state;
            decision.skip = true;
            return decision;
        }

        state->skippedSinceRefresh = 0;
        if (!state->pending) {
            const bool haveQueryBudget = queryBudgetAlreadyConsumed || ConsumePassOcclusionQueryBudget(domain);
            if (haveQueryBudget) {
                context->Begin(state->query);
                ++g_passOcclusionTelemetry.queryBegun;
                decision.queryActive = true;
            } else {
                ++g_passOcclusionTelemetry.queryBudgetDenied;
            }
        }

        decision.state = state;
        return decision;
    }

    void EndPassOcclusionDecision(REX::W32::ID3D11DeviceContext* context, Decision& decision)
    {
        if (!context || !decision.state || !decision.queryActive || !decision.state->query) {
            return;
        }

        context->End(decision.state->query);
        ++g_passOcclusionTelemetry.queryEnded;
        decision.state->pending = true;
        decision.queryActive = false;
    }

    void MaybeLogPassOcclusionTelemetry()
    {
        if (g_passOcclusionFrame - g_passOcclusionLastTelemetryFrame < kPassOcclusionTelemetryPeriod) {
            return;
        }

        g_passOcclusionTelemetry = {};
        g_passOcclusionLastTelemetryFrame = g_passOcclusionFrame;
    }

    void PassOcclusionOnFramePresent()
    {
        ++g_passOcclusionFrame;
        g_passOcclusionMainQueries = 0;
        g_passOcclusionShadowQueries = 0;
        g_passOcclusionMainSkips = 0;
        g_passOcclusionShadowSkips = 0;

        if (!PASS_LEVEL_OCCLUSION_ON) {
            g_passOcclusionCameraStable = false;
            g_passOcclusionHaveCamera = false;
            MaybeLogPassOcclusionTelemetry();
            return;
        }

        if (!CUSTOMBUFFER_ON ||
            !std::isfinite(g_customBufferData.camX) ||
            !std::isfinite(g_customBufferData.camY) ||
            !std::isfinite(g_customBufferData.camZ) ||
            !std::isfinite(g_customBufferData.viewDirX) ||
            !std::isfinite(g_customBufferData.viewDirY) ||
            !std::isfinite(g_customBufferData.viewDirZ)) {
            g_passOcclusionCameraStable = false;
            g_passOcclusionHaveCamera = false;
            ++g_passOcclusionTelemetry.cameraInvalidFrames;
            MaybeLogPassOcclusionTelemetry();
            return;
        }

        CameraOcclusionSnapshot cur{
            g_customBufferData.camX,
            g_customBufferData.camY,
            g_customBufferData.camZ,
            g_customBufferData.viewDirX,
            g_customBufferData.viewDirY,
            g_customBufferData.viewDirZ
        };

        if (g_passOcclusionHaveCamera) {
            const float px = cur.x - g_passOcclusionPrevCamera.x;
            const float py = cur.y - g_passOcclusionPrevCamera.y;
            const float pz = cur.z - g_passOcclusionPrevCamera.z;
            const float posSq = px * px + py * py + pz * pz;
            const float dot = cur.dx * g_passOcclusionPrevCamera.dx +
                              cur.dy * g_passOcclusionPrevCamera.dy +
                              cur.dz * g_passOcclusionPrevCamera.dz;
            g_passOcclusionCameraStable = posSq < 16.0f && dot > 0.9994f;
        } else {
            g_passOcclusionCameraStable = false;
            g_passOcclusionHaveCamera = true;
        }
        if (g_passOcclusionCameraStable) {
            ++g_passOcclusionTelemetry.cameraStableFrames;
        } else {
            ++g_passOcclusionTelemetry.cameraUnstableFrames;
        }
        g_passOcclusionPrevCamera = cur;

        if ((g_passOcclusionFrame % kPassOcclusionPrunePeriod) == 0) {
            for (auto it = g_passOcclusionCache.begin(); it != g_passOcclusionCache.end();) {
                if (g_passOcclusionFrame > it->second.lastTouchedFrame &&
                    g_passOcclusionFrame - it->second.lastTouchedFrame > kPassOcclusionStaleFrames) {
                    if (it->second.query) {
                        it->second.query->Release();
                    }
                    ++g_passOcclusionTelemetry.pruned;
                    it = g_passOcclusionCache.erase(it);
                } else {
                    ++it;
                }
            }
            for (auto it = g_passOcclusionEarlyGeometryCache.begin(); it != g_passOcclusionEarlyGeometryCache.end();) {
                if (g_passOcclusionFrame > it->second.lastTouchedFrame &&
                    g_passOcclusionFrame - it->second.lastTouchedFrame > kPassOcclusionStaleFrames) {
                    it = g_passOcclusionEarlyGeometryCache.erase(it);
                } else {
                    ++it;
                }
            }
        }

        MaybeLogPassOcclusionTelemetry();
    }

    void ShutdownPassOcclusionCache()
    {
        for (auto& [key, state] : g_passOcclusionCache) {
            if (state.query) {
                state.query->Release();
                state.query = nullptr;
            }
        }
        g_passOcclusionCache.clear();
        g_passOcclusionEarlyGeometryCache.clear();
    }

} // namespace

Decision BeginDecision(
    REX::W32::ID3D11DeviceContext* context,
    void* batchRenderer,
    BSRenderPassLayout* head,
    unsigned int group,
    bool allowAlpha,
    bool commandBufferPath)
{
    return BeginPassOcclusionDecision(context, batchRenderer, head, group, allowAlpha, commandBufferPath);
}

void EndDecision(REX::W32::ID3D11DeviceContext* context, Decision& decision)
{
    EndPassOcclusionDecision(context, decision);
}

bool ShouldEarlyCullRegisterObjectStandard(RE::BSGeometry* geometry)
{
    return ShouldEarlyCullRegisterObjectStandardImpl(geometry);
}

void PatchArenaGates(void* arena, std::vector<ArenaGatePatch>& patches)
{
    PatchPassOcclusionArenaGates(arena, patches);
}

void RestoreArenaGates(std::vector<ArenaGatePatch>& patches)
{
    RestorePassOcclusionArenaGates(patches);
}

void OnFramePresent()
{
    PassOcclusionOnFramePresent();
}

void ShutdownCache()
{
    ShutdownPassOcclusionCache();
}
} // namespace PassOcclusion

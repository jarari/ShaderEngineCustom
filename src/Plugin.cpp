#include <Global.h>
#include <PCH.h>
#include <CustomPass.h>
#include <GpuScalar.h>
#include <PhaseTelemetry.h>
#include <ShadowTelemetry.h>
#include <LightCullPolicy.h>
#include <LightTracker.h>
#include <RenderTargets.h>
#include <d3d11.h>

#include <optional>

namespace RE
{
    class BSLODTriShape :
        public BSTriShape
    {
    public:
        static constexpr auto Ni_RTTI{ Ni_RTTI::BSLODTriShape };
    };

    class BSLODMultiIndexTriShape :
        public BSTriShape
    {
    public:
        static constexpr auto Ni_RTTI{ Ni_RTTI::BSLODMultiIndexTriShape };
    };

    class BSMeshLODTriShape :
        public BSTriShape
    {
    public:
        static constexpr auto Ni_RTTI{ Ni_RTTI::BSMeshLODTriShape };
    };

    class BSPackedCombinedGeomDataExtra :
        public NiExtraData
    {
    public:
        static constexpr auto Ni_RTTI{ Ni_RTTI::BSPackedCombinedGeomDataExtra };
    };

    class BSPackedCombinedSharedGeomDataExtra :
        public NiExtraData
    {
    public:
        static constexpr auto Ni_RTTI{ Ni_RTTI::BSPackedCombinedSharedGeomDataExtra };
    };
}

// --- Variables ---
REL::Relocation<uintptr_t> ptr_D3D11CreateDeviceAndSwapChainCall{ REL::ID{ 224250, 4492363 } };
constexpr std::uint32_t kCallOffsetOG = 0x419;
constexpr std::uint32_t kCallOffsetAE = 0x410;
typedef HRESULT (*FnD3D11CreateDeviceAndSwapChain)(IDXGIAdapter*,
    D3D_DRIVER_TYPE,
    HMODULE, UINT,
    const D3D_FEATURE_LEVEL*,
    UINT,
    UINT,
    const DXGI_SWAP_CHAIN_DESC*,
    IDXGISwapChain**,
    ID3D11Device**,
    D3D_FEATURE_LEVEL*,
    ID3D11DeviceContext**);
FnD3D11CreateDeviceAndSwapChain D3D11CreateDeviceAndSwapChain_Orig;
typedef HRESULT (*FnD3D11Present)(IDXGISwapChain*, UINT, UINT);
FnD3D11Present D3D11Present_Orig;
REL::Relocation<uintptr_t> ptr_ClipCursor{ REL::ID{ 641385, 4823626 } };
typedef BOOL (*FnClipCursor)(const RECT*);
FnClipCursor ClipCursor_Orig;

// --- Variables ---

// Global Singletons
RE::BSGraphics::RendererData* g_rendererData = nullptr;
RECT g_windowRect;
HWND g_outputWindow = nullptr;
RE::PlayerCharacter* g_player = nullptr;
RE::ActorValue* g_actorValueInfo = nullptr;
RE::Sky* g_sky = nullptr;
// Global Shader DB
ShaderDB g_ShaderDB = {};
// Global trackers of current shader
int g_currentTextureDSIndices[128] = { -1 }; 
// Tell MyCreatePixelShader to skip analysing the shader when creating replacement shaders to avoid infinite recursion
thread_local bool g_isCreatingReplacementShader = false;
// Last original (pre-replacement) pixel shader observed in MyPSSetShader.
// Used by the actor-tag debug logger to attribute draws to a shader UID.
// Externally visible for CustomPass.cpp's BeforeDrawForMatchedDef trigger,
// which fires from MyDraw* hooks and needs to identify the currently bound
// original (pre-replacement) PS to look up its matched ShaderDefinition.
std::atomic<REX::W32::ID3D11PixelShader*> g_currentOriginalPixelShader{ nullptr };
// Global custom buffer data structure instance for updating CB13
GFXBoosterAccessData g_customBufferData = {};
DrawTagData g_drawTagData = {};
// Global custom resource to pass data to shaders
REX::W32::ID3D11Buffer* g_customSRVBuffer = nullptr;
REX::W32::ID3D11ShaderResourceView* g_customSRV = nullptr;
REX::W32::ID3D11Buffer* g_drawTagSRVBuffer = nullptr;
REX::W32::ID3D11ShaderResourceView* g_drawTagSRV = nullptr;
REX::W32::ID3D11Buffer* g_modularFloatsSRVBuffer = nullptr;
REX::W32::ID3D11ShaderResourceView* g_modularFloatsSRV = nullptr;
REX::W32::ID3D11Buffer* g_modularIntsSRVBuffer = nullptr;
REX::W32::ID3D11ShaderResourceView* g_modularIntsSRV = nullptr;
REX::W32::ID3D11Buffer* g_modularBoolsSRVBuffer = nullptr;
REX::W32::ID3D11ShaderResourceView* g_modularBoolsSRV = nullptr;
static constexpr UINT DEPTHBUFFER_SLOT = 30;
enum class DepthStencilTarget : UINT
{
    kMainOtherOther = 0,
    kMainOther = 1,
    kMain = 2,
    kMainCopy = 3,
    kMainCopyCopy = 4,
    kShadowMap = 8,
    kCount = 13
};
static constexpr auto MAIN_DEPTHSTENCIL_TARGET = DepthStencilTarget::kMain;
static constexpr UINT DEPTHSTENCIL_TARGET_COUNT = static_cast<UINT>(DepthStencilTarget::kCount);
static std::atomic<UINT> g_currentDepthTargetIndex{ DEPTHSTENCIL_TARGET_COUNT };
static std::atomic<UINT> g_currentRenderTargetCount{ 0 };
static std::atomic<bool> g_currentHasRenderTarget{ false };
// Global depth buffer SRV for shaders to read depth when DEPTHBUFFER_ON is enabled
REX::W32::ID3D11ShaderResourceView* g_depthSRV = nullptr;
static bool g_activeReplacementPixelShader = false;
// Exposed via Global.h so CustomPass.cpp can guard against recursion in
// MyPSSetShader while the registry rebinds injected resources during a pass.
thread_local bool g_bindingInjectedPixelResources = false;
static thread_local std::uint32_t g_commandBufferReplayDepth = 0;
static REX::W32::ID3D11ShaderResourceView* g_lastSceneDepthSRV = nullptr;
struct alignas(16) ModularFloat4 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 0.0f;
};
struct alignas(16) ModularInt4 {
    int32_t x = 0;
    int32_t y = 0;
    int32_t z = 0;
    int32_t w = 0;
};
struct alignas(16) ModularUInt4 {
    uint32_t x = 0;
    uint32_t y = 0;
    uint32_t z = 0;
    uint32_t w = 0;
};
static std::vector<ModularFloat4> g_modularFloatData(1);
static std::vector<ModularInt4> g_modularIntData(1);
static std::vector<ModularUInt4> g_modularBoolData(1);
static UINT g_modularFloatElementCount = 0;
static UINT g_modularIntElementCount = 0;
static UINT g_modularBoolElementCount = 0;

static DirectX::XMFLOAT3 ColorFromPackedRGB(std::uint32_t color)
{
    const float inv255 = 1.0f / 255.0f;
    return DirectX::XMFLOAT3(
        static_cast<float>((color >> 16) & 0xFF) * inv255,
        static_cast<float>((color >> 8) & 0xFF) * inv255,
        static_cast<float>(color & 0xFF) * inv255);
}

static const RE::INTERIOR_DATA* GetInteriorDataForDominantLight(RE::TESObjectCELL* cell)
{
    if (!cell || !cell->IsInterior()) {
        return nullptr;
    }

    if (cell->cellDataInterior) {
        return cell->cellDataInterior;
    }

    return cell->lightingTemplate ? std::addressof(cell->lightingTemplate->data) : nullptr;
}

static DirectX::XMFLOAT3 DirectionFromInteriorAngles(std::uint32_t directionalXY, std::uint32_t directionalZ)
{
    const float yaw = static_cast<float>(directionalXY & 0xFF) * (DirectX::XM_2PI / 255.0f);
    const float pitch = (static_cast<float>(directionalZ & 0xFF) / 255.0f - 0.5f) * DirectX::XM_PI;
    const float cosPitch = std::cos(pitch);

    return DirectX::XMFLOAT3(
        std::cos(yaw) * cosPitch,
        std::sin(yaw) * cosPitch,
        std::sin(pitch));
}

static DirectX::XMFLOAT3 DirectionFromNodeForward(const RE::NiAVObject* object)
{
    if (!object) {
        return DirectX::XMFLOAT3(0.0f, 0.0f, 0.0f);
    }

    const auto& row = object->world.rotate[1];
    const float lenSq = row.x * row.x + row.y * row.y + row.z * row.z;
    if (lenSq <= 1.0e-6f) {
        return DirectX::XMFLOAT3(0.0f, 0.0f, 0.0f);
    }

    const float invLen = 1.0f / std::sqrt(lenSq);
    return DirectX::XMFLOAT3(row.x * invLen, row.y * invLen, row.z * invLen);
}

// Ring of distinct buffer+SRV pairs so each draw gets its own backing.
// F4 batches draws into BSGraphics command queues; with a single shared
// buffer, all queued draws would resolve to whatever value was Mapped last
// before flush — appearing as stale tags.
static constexpr UINT DRAWTAG_RING_SIZE = 1024;
static std::array<REX::W32::ID3D11Buffer*, DRAWTAG_RING_SIZE> g_drawTagRingBuffers{};
static std::array<REX::W32::ID3D11ShaderResourceView*, DRAWTAG_RING_SIZE> g_drawTagRingSRVs{};
static std::atomic<UINT> g_drawTagRingCursor{ 0 };

// --- Static per-tag resources for the command-buffer-recording path ---
//
// BSShader::BuildCommandBuffer hook records one of these wrappers per pass.
// The recorded wrapper pointer is captured into the command buffer and
// dereferenced at replay time (potentially many frames later, on whatever
// thread the engine flushes), so the SRV must be a long-lived immutable
// resource — ring slots would race because they get reused per-draw.
//
// Layout matches what ProcessCommandBuffer reads for kind=0:
//   +0x00  unused
//   +0x08  ID3D11ShaderResourceView*  (passed as ppSRVs to PSSetShaderResources)
//   +0x30  fence count, must be 0 to skip priority/wait branches
struct alignas(16) FakeStructuredResource
{
    void*                                    pad00 = nullptr;
    std::atomic<REX::W32::ID3D11ShaderResourceView*> srv = nullptr;
    char                                     pad10[0x20] = {};
    std::uint32_t                            fenceCount = 0;
    char                                     pad34[12] = {};
};
static_assert(sizeof(FakeStructuredResource) == 64);
static_assert(sizeof(std::atomic<REX::W32::ID3D11ShaderResourceView*>) == sizeof(REX::W32::ID3D11ShaderResourceView*));
static_assert(offsetof(FakeStructuredResource, srv) == 0x08);
static_assert(offsetof(FakeStructuredResource, fenceCount) == 0x30);

// Recorded entry layout that ProcessCommandBuffer iterates as a 16-byte
// table after the textures section in each command buffer.
struct CommandBufferShaderResource
{
    void*           resourceWrapper;  // +0x00
    std::uint8_t    slot;             // +0x08
    std::uint8_t    stage;            // +0x09 (0=VS, 1=PS)
    std::uint8_t    kind;             // +0x0A (0 = wrapper+0x08 path)
    std::uint8_t    pad0B = 0;
    std::uint32_t   pad0C = 0;
};
static_assert(sizeof(CommandBufferShaderResource) == 16);

static REX::W32::ID3D11Buffer*              g_actorTagBuffer = nullptr;
static REX::W32::ID3D11ShaderResourceView*  g_actorTagSRV = nullptr;
static REX::W32::ID3D11Buffer*              g_unknownTagBuffer = nullptr;
static REX::W32::ID3D11ShaderResourceView*  g_unknownTagSRV = nullptr;
// Head/facegen variant: same materialTag as kActor (1) but with isHead = 1
// in the structured buffer so PS code can branch on facegen vs. equipment.
static REX::W32::ID3D11Buffer*              g_actorHeadTagBuffer = nullptr;
static REX::W32::ID3D11ShaderResourceView*  g_actorHeadTagSRV = nullptr;
static FakeStructuredResource               g_actorTagWrapper{};
static FakeStructuredResource               g_unknownTagWrapper{};
static FakeStructuredResource               g_actorHeadTagWrapper{};
static std::unordered_map<RE::BSGeometry*, std::unique_ptr<FakeStructuredResource>> g_commandBufferDrawTagWrappers;
static std::shared_mutex                    g_commandBufferDrawTagWrappersLock;
static std::mutex                           g_tagWrapperInitMutex;
static bool                                 g_tagWrapperResourcesReady = false;
// Global frame counter for slow updates
static uint32_t g_frameTick = 0;      // frame counter
// Global player values
static float    g_healthPerc = 1.0f; // value sampled 30 frames ago
static float    g_lastRad = 0.0f;   // value sampled 30 frames ago
static float    g_radDmg = 0.0f;    // calculated value send to the shader
static bool     g_haveRadBaseline = false;
// Global wind values
static float    g_windSpeed = 0.0f;     // value sampled 30 frames ago
static float    g_windAngle = 0.0f;     // value sampled 30 frames ago
static float    g_windTurbulence = 0.0f;// value sampled 30 frames ago
// Global combat flag
static bool     g_inCombat = false;    // value sampled 30 frames ago
// Global interior flag
static bool     g_inInterior = false; // value sampled 30 frames ago

static REX::W32::ID3D11ShaderResourceView* UpdateDrawTagBuffer(REX::W32::ID3D11DeviceContext* context, float materialTag, float isHead);
static void BindDrawTagForCurrentDraw(REX::W32::ID3D11DeviceContext* context, bool force = false);
static void EnsureD3DDrawHooksPresent();
// Forward decl: HookedBSBatchRendererDraw (defined further up the file via
// hook namespace) needs to re-publish injected resources after a customPass
// fires, but BindInjectedPixelShaderResources is defined later.
static void BindInjectedPixelShaderResources(REX::W32::ID3D11DeviceContext* context);

namespace
{
    enum class DrawMaterialTag : std::uint32_t
    {
        kUnknown = 0,
        kActor = 1
    };

    // Wrapper-side tag used to pick which static SRV a per-geometry wrapper
    // points at. kActor and kActorHead both seed materialTag = kActor; they
    // differ only in the isHead flag carried through the structured buffer.
    enum class WrapperTag : std::uint8_t
    {
        kUnknown,
        kActor,
        kActorHead
    };

    struct BSRenderPassLayout
    {
        BSRenderPassLayout* next;                  // 00
        RE::BSShader* shader;                      // 08
        RE::BSShaderProperty* shaderProperty;      // 10
        RE::BSGeometry* geometry;                  // 18
        std::byte pad20[0x28 - 0x20];              // 20
        void* commandBuffer;                       // 28
        std::byte pad30[0x38 - 0x30];              // 30
        BSRenderPassLayout* listNext;              // 38
        BSRenderPassLayout* passGroupNext;         // 40
        std::uint32_t techniqueID;                 // 48
    };
    static_assert(offsetof(BSRenderPassLayout, shaderProperty) == 0x10);
    static_assert(offsetof(BSRenderPassLayout, geometry) == 0x18);
    static_assert(offsetof(BSRenderPassLayout, commandBuffer) == 0x28);
    static_assert(offsetof(BSRenderPassLayout, listNext) == 0x38);
    static_assert(offsetof(BSRenderPassLayout, passGroupNext) == 0x40);
    static_assert(offsetof(BSRenderPassLayout, techniqueID) == 0x48);

    std::uint32_t CountPassChain(BSRenderPassLayout* head)
    {
        std::uint32_t count = 0;
        for (auto* pass = head; pass && count < 512; pass = pass->passGroupNext) {
            ++count;
        }
        return count;
    }

    bool IsShadowWorkTelemetryActive() noexcept
    {
        return ShadowTelemetry::g_mode.load(std::memory_order_relaxed) == ShadowTelemetry::Mode::On &&
               ShadowTelemetry::IsInShadowMap();
    }

    void DecodeCommandBufferData(void* cbData, ShadowTelemetry::WorkTarget& target)
    {
        if (!cbData) {
            return;
        }

        auto** records = static_cast<void**>(cbData);
        constexpr std::uint32_t kMaxRecords = 4096;
        for (std::uint32_t i = 0; i < kMaxRecords; ++i) {
            void* record = records[i];
            if (!record) {
                break;
            }

            if (!target.commandBuffer) {
                target.commandBuffer = record;
            }

            auto* bytes = static_cast<std::uint8_t*>(record);
            const auto drawCount = static_cast<std::uint32_t>(bytes[0x04]);
            const auto srvRecordCount = static_cast<std::uint32_t>(bytes[0x06]);
            auto* drawEntries = reinterpret_cast<const std::uint32_t*>(bytes + 0x3C);
            ++target.cbRecordCount;
            target.cbDrawCount += drawCount;
            target.cbSrvRecordCount += srvRecordCount;
            for (std::uint32_t j = 0; j < drawCount; ++j) {
                if (drawEntries[j] != 0) {
                    ++target.cbNonZeroDrawCount;
                }
            }
            if (drawCount > target.cbMaxDrawCount) {
                target.cbMaxDrawCount = drawCount;
            }
            if (srvRecordCount > target.cbMaxSrvRecordCount) {
                target.cbMaxSrvRecordCount = srvRecordCount;
            }
        }
    }

    ShadowTelemetry::WorkTarget MakeShadowPassWorkTarget(
        void* owner,
        BSRenderPassLayout* head,
        void* cbData,
        std::int32_t passGroupIdx,
        std::uint32_t subIdx,
        bool allowAlpha)
    {
        ShadowTelemetry::WorkTarget target{};
        target.owner = owner;
        target.head = head;
        target.cbData = cbData;
        target.commandBuffer = nullptr;
        target.geometry = head ? static_cast<void*>(head->geometry) : nullptr;
        target.shader = head ? static_cast<void*>(head->shader) : nullptr;
        target.shaderProperty = head ? static_cast<void*>(head->shaderProperty) : nullptr;
        target.techniqueID = head ? head->techniqueID : 0;
        target.passGroupIdx = passGroupIdx;
        target.subIdx = subIdx;
        target.chainLen = CountPassChain(head);
        target.allowAlpha = allowAlpha;
        target.commandBuffer = head ? head->commandBuffer : nullptr;
        return target;
    }

    class ScopedShadowWork
    {
    public:
        ScopedShadowWork(ShadowTelemetry::WorkKind kind, const ShadowTelemetry::WorkTarget& target) :
            kind_(kind),
            active_(ShadowTelemetry::BeginShadowWork(kind, target))
        {}

        ~ScopedShadowWork()
        {
            if (active_) {
                ShadowTelemetry::EndShadowWork(kind_);
            }
        }

        ScopedShadowWork(const ScopedShadowWork&) = delete;
        ScopedShadowWork& operator=(const ScopedShadowWork&) = delete;

    private:
        ShadowTelemetry::WorkKind kind_;
        bool active_;
    };

    thread_local ShadowTelemetry::WorkTarget tl_processCommandBufferTarget{};
    thread_local bool tl_processCommandBufferTargetActive = false;
    std::shared_mutex g_shadowGeometryClassLock;
    std::unordered_map<RE::BSGeometry*, bool> g_shadowGeometryIsPrecombine;
    std::unordered_map<RE::NiAVObject*, bool> g_shadowObjectHasPackedCombinedExtra;
    std::unordered_map<RE::NiAVObject*, bool> g_shadowObjectHasDynamicBSXFlags;
    struct ShadowPassClassification
    {
        RE::BSGeometry* geometry = nullptr;
        std::uint8_t flags = 0;
    };
    std::shared_mutex g_shadowPassClassLock;
    std::unordered_map<BSRenderPassLayout*, ShadowPassClassification> g_shadowPassClassifications;

    constexpr std::uint8_t kShadowPassClassStatic = 1u << 0;
    constexpr std::uint32_t kBSXAnimated = 0x1;
    constexpr std::uint32_t kBSXHavok = 0x2;
    constexpr std::uint32_t kBSXRagdoll = 0x4;
    constexpr std::uint32_t kBSXDynamic = 0x40;      // 64 decimal in NIF tools
    constexpr std::uint32_t kBSXArticulated = 0x80;
    constexpr std::uint32_t kShadowDynamicBSXMask =
        kBSXAnimated | kBSXHavok | kBSXRagdoll | kBSXDynamic | kBSXArticulated;
    constexpr std::uint32_t kMaxShadowParentScan = 32;
    constexpr std::size_t kMaxShadowGeometryClassCache = 262144;
    constexpr std::size_t kMaxShadowPassClassCache = 262144;

    std::uint32_t GetBSXFlags(RE::NiObjectNET* object, std::uint32_t mask)
    {
        if (!object) {
            return 0;
        }

        using GetFlags_t = std::uint32_t (*)(RE::NiObjectNET*, std::uint32_t);
        static REL::Relocation<GetFlags_t> getFlags{ REL::ID{ 1036263, 0 } };  // BSXFlags::GetFlags, FO4 1.10.163
        return getFlags(object, mask);
    }

    bool IsPackedCombinedExtraData(RE::NiExtraData* extra)
    {
        if (!extra) {
            return false;
        }

        if (netimmerse_cast<RE::BSPackedCombinedGeomDataExtra*>(extra) ||
            netimmerse_cast<RE::BSPackedCombinedSharedGeomDataExtra*>(extra)) {
            return true;
        }

        static const RE::BSFixedString packedGeomName("BSPackedCombinedGeomDataExtra");
        static const RE::BSFixedString packedSharedName("BSPackedCombinedSharedGeomDataExtra");
        return extra->name == packedGeomName || extra->name == packedSharedName;
    }

    bool ComputePackedCombinedExtra(RE::NiAVObject* object)
    {
        if (!object) {
            return false;
        }

        auto* extras = object->extra;
        if (!extras) {
            return false;
        }

        for (auto it = extras->begin(); it != extras->end(); ++it) {
            if (IsPackedCombinedExtraData(*it)) {
                return true;
            }
        }
        return false;
    }

    bool HasPackedCombinedExtra(RE::NiAVObject* object)
    {
        if (!object) {
            return false;
        }

        {
            std::shared_lock lock(g_shadowGeometryClassLock);
            auto it = g_shadowObjectHasPackedCombinedExtra.find(object);
            if (it != g_shadowObjectHasPackedCombinedExtra.end()) {
                return it->second;
            }
        }

        const bool hasExtra = ComputePackedCombinedExtra(object);
        std::unique_lock lock(g_shadowGeometryClassLock);
        g_shadowObjectHasPackedCombinedExtra[object] = hasExtra;
        return hasExtra;
    }

    bool ComputeDynamicBSXFlags(RE::NiAVObject* object)
    {
        if (object && object->controllers) {
            return true;
        }
        return GetBSXFlags(object, kShadowDynamicBSXMask) != 0;
    }

    bool HasDynamicBSXFlags(RE::NiAVObject* object)
    {
        if (!object) {
            return false;
        }

        {
            std::shared_lock lock(g_shadowGeometryClassLock);
            auto it = g_shadowObjectHasDynamicBSXFlags.find(object);
            if (it != g_shadowObjectHasDynamicBSXFlags.end()) {
                return it->second;
            }
        }

        const bool hasDynamicFlags = ComputeDynamicBSXFlags(object);
        if (hasDynamicFlags) {
            std::unique_lock lock(g_shadowGeometryClassLock);
            g_shadowObjectHasDynamicBSXFlags[object] = true;
        }
        return hasDynamicFlags;
    }

    bool HasPackedCombinedExtraInChain(RE::NiAVObject* object)
    {
        for (std::uint32_t depth = 0; object && depth < kMaxShadowParentScan; ++depth, object = object->parent) {
            if (HasPackedCombinedExtra(object)) {
                return true;
            }
        }
        return false;
    }

    bool HasDynamicBSXFlagsInChain(RE::NiAVObject* object)
    {
        for (std::uint32_t depth = 0; object && depth < kMaxShadowParentScan; ++depth, object = object->parent) {
            if (HasDynamicBSXFlags(object)) {
                return true;
            }
        }
        return false;
    }

    bool IsBroadShadowStaticGeometryType(RE::BSGeometry* geometry)
    {
        if (!geometry) {
            return false;
        }

        if (netimmerse_cast<RE::BSTriShape*>(geometry) ||
            netimmerse_cast<RE::BSLODTriShape*>(geometry) ||
            netimmerse_cast<RE::BSLODMultiIndexTriShape*>(geometry) ||
            netimmerse_cast<RE::BSMeshLODTriShape*>(geometry)) {
            return true;
        }

        switch (geometry->type) {
        case 3:   // BSTriShape fallback
        case 5:   // BSMeshLODTriShape fallback
        case 6:   // BSLODMultiIndexTriShape fallback
        case 7:   // BSLODTriShape fallback
            return true;
        default:
            return false;
        }
    }

    bool IsShadowStaticCandidate(RE::BSGeometry* geometry)
    {
        if (!geometry || geometry->skinInstance) {
            return false;
        }

        if (geometry->type == 15 ||
            geometry->IsBSCombinedTriShape() != nullptr ||
            HasPackedCombinedExtraInChain(geometry)) {
            return true;
        }

        return IsBroadShadowStaticGeometryType(geometry);
    }

    bool ComputePrecombineShadowGeometry(RE::BSGeometry* geometry)
    {
        if (!IsShadowStaticCandidate(geometry)) {
            return false;
        }

        // Static shape class is necessary but not sufficient: animated,
        // Havok, articulated, ragdoll, or controller-driven objects must stay
        // in the live dynamic overlay even if their mesh type looks static.
        if (HasDynamicBSXFlagsInChain(geometry)) {
            return false;
        }

        return true;
    }

    bool IsPrecombineShadowGeometry(RE::BSGeometry* geometry)
    {
        if (!geometry) {
            return false;
        }

        {
            std::shared_lock lock(g_shadowGeometryClassLock);
            const auto it = g_shadowGeometryIsPrecombine.find(geometry);
            if (it != g_shadowGeometryIsPrecombine.end()) {
                return it->second;
            }
        }

        const bool isPrecombine = ComputePrecombineShadowGeometry(geometry);
        {
            std::unique_lock lock(g_shadowGeometryClassLock);
            if (g_shadowGeometryIsPrecombine.size() >= kMaxShadowGeometryClassCache) {
                g_shadowGeometryIsPrecombine.clear();
            }
            g_shadowGeometryIsPrecombine[geometry] = isPrecombine;
        }
        return isPrecombine;
    }

    bool TryGetShadowPassClassification(BSRenderPassLayout* pass, bool& isPrecombine)
    {
        if (!pass) {
            return false;
        }

        std::shared_lock lock(g_shadowPassClassLock);
        const auto it = g_shadowPassClassifications.find(pass);
        if (it == g_shadowPassClassifications.end() ||
            it->second.geometry != pass->geometry) {
            return false;
        }

        isPrecombine = (it->second.flags & kShadowPassClassStatic) != 0;
        return true;
    }

    void RememberShadowPassClassification(BSRenderPassLayout* pass, bool isPrecombine)
    {
        if (!pass) {
            return;
        }

        std::unique_lock lock(g_shadowPassClassLock);
        if (g_shadowPassClassifications.size() >= kMaxShadowPassClassCache) {
            g_shadowPassClassifications.clear();
        }
        g_shadowPassClassifications[pass] = {
            pass->geometry,
            static_cast<std::uint8_t>(isPrecombine ? kShadowPassClassStatic : 0)
        };
    }

    bool ClassifyShadowPass(BSRenderPassLayout* pass)
    {
        if (!pass) {
            return false;
        }

        bool isPrecombine = false;
        if (TryGetShadowPassClassification(pass, isPrecombine)) {
            return isPrecombine;
        }

        isPrecombine = IsPrecombineShadowGeometry(pass->geometry);
        RememberShadowPassClassification(pass, isPrecombine);
        return isPrecombine;
    }

    bool ShouldKeepShadowPassForCurrentSplit(BSRenderPassLayout* pass)
    {
        if (!SHADOW_CACHE_DIRECTIONAL_MAPSLOT1_ON ||
            !ShadowTelemetry::IsShadowCacheActiveForCurrentShadow() ||
            !pass) {
            return true;
        }

        const bool isStaticCaster = ClassifyShadowPass(pass);
        if (ShadowTelemetry::IsShadowCacheStaticBuildPass()) {
            ShadowTelemetry::NoteShadowCacheRenderPassSplit(isStaticCaster, isStaticCaster);
            return isStaticCaster;
        }
        if (ShadowTelemetry::IsShadowCacheDynamicOverlayPass()) {
            const bool keep = !isStaticCaster;
            ShadowTelemetry::NoteShadowCacheRenderPassSplit(keep, isStaticCaster);
            return keep;
        }
        return true;
    }

    constexpr unsigned int kShadowCacheBatchPassGroup = 1;
    constexpr std::uintptr_t kInvalidBatchNode = 0xFFFFu;
    constexpr std::size_t kBatchRendererGroupStride = 0x18;
    constexpr std::size_t kBatchRendererGroupArrayOffset = 0x08;
    constexpr std::size_t kBatchRendererGroupHeadOffset = 0x2E0;
    constexpr std::size_t kBatchRendererGroupTailOffset = 0x2E4;
    constexpr std::size_t kBatchRendererNodeStride = 0x10;
    constexpr std::size_t kBatchRendererNodeHeadOffset = 0x00;
    constexpr std::size_t kBatchRendererNodeTechniqueOffset = 0x08;
    constexpr std::size_t kBatchRendererNodeNextOffset = 0x0C;
    constexpr std::size_t kCommandBufferWriteCursorOffset = 0x10000;
    constexpr std::size_t kCommandBufferFrameOffset = 0x10010;
    constexpr std::size_t kMaxCommandBufferRecords = 8192;

    bool IsShadowCacheRenderSplitActive() noexcept
    {
        return SHADOW_CACHE_DIRECTIONAL_MAPSLOT1_ON &&
               ShadowTelemetry::IsShadowCacheActiveForCurrentShadow() &&
               (ShadowTelemetry::IsShadowCacheStaticBuildPass() ||
                ShadowTelemetry::IsShadowCacheDynamicOverlayPass());
    }

    bool ShouldSplitRenderBatchesCall(unsigned int passGroupIdx, unsigned int filter) noexcept
    {
        if (!IsShadowCacheRenderSplitActive()) {
            return false;
        }

        // FinishAccumulating_ShadowMapOrMask consumes accumulator group 1, then
        // RenderGeometryGroup(group=9). The latter can render a nested batch
        // renderer across groups 0..12 with filter=9.
        return passGroupIdx == kShadowCacheBatchPassGroup || filter == 9;
    }

    std::uintptr_t GetRenderBatchNodeAddress(void* batchRenderer, int group, unsigned int subIdx)
    {
        if (!batchRenderer || group < 0 || group >= 32 || subIdx >= 0xFFFFu) {
            return 0;
        }

        const auto base = reinterpret_cast<std::uintptr_t>(batchRenderer);
        const auto nodeArray = *reinterpret_cast<std::uintptr_t*>(
            base + kBatchRendererGroupStride * static_cast<unsigned int>(group) +
            kBatchRendererGroupArrayOffset);
        if (!nodeArray) {
            return 0;
        }

        return nodeArray + kBatchRendererNodeStride * subIdx;
    }

    BSRenderPassLayout* GetRenderBatchNodeHeadRaw(std::uintptr_t node)
    {
        return node ? *reinterpret_cast<BSRenderPassLayout**>(node + kBatchRendererNodeHeadOffset) : nullptr;
    }

    void SetRenderBatchNodeHeadRaw(std::uintptr_t node, BSRenderPassLayout* head)
    {
        if (node) {
            *reinterpret_cast<BSRenderPassLayout**>(node + kBatchRendererNodeHeadOffset) = head;
        }
    }

    std::uint32_t GetRenderBatchNodeTechniqueRaw(std::uintptr_t node)
    {
        return node ? *reinterpret_cast<std::uint32_t*>(node + kBatchRendererNodeTechniqueOffset) : 0;
    }

    void InvalidateCommandBufferFrame(void* cbData)
    {
        if (cbData) {
            *reinterpret_cast<std::uint32_t*>(static_cast<std::byte*>(cbData) + kCommandBufferFrameOffset) = 0xFFFFFFFFu;
        }
    }

    class ScopedStaticRenderBatchesRestore
    {
    public:
        ScopedStaticRenderBatchesRestore(void* batchRenderer, unsigned int group) :
            batchRenderer_(reinterpret_cast<std::uintptr_t>(batchRenderer)),
            group_(group)
        {
            if (!batchRenderer_ ||
                group_ != kShadowCacheBatchPassGroup ||
                !ShadowTelemetry::IsShadowCacheStaticBuildPass()) {
                return;
            }

            const auto nodeArray = *reinterpret_cast<std::uintptr_t*>(
                batchRenderer_ + kBatchRendererGroupStride * group_ + kBatchRendererGroupArrayOffset);
            if (!nodeArray) {
                return;
            }

            originalHead_ = *reinterpret_cast<std::uint32_t*>(
                batchRenderer_ + kBatchRendererGroupHeadOffset + group_ * sizeof(std::uint64_t));
            originalTail_ = *reinterpret_cast<std::uint32_t*>(
                batchRenderer_ + kBatchRendererGroupTailOffset + group_ * sizeof(std::uint64_t));

            std::uint32_t nodeIndex = originalHead_;
            for (std::size_t count = 0; nodeIndex != kInvalidBatchNode && count < 65536; ++count) {
                const std::uintptr_t node = nodeArray + kBatchRendererNodeStride * nodeIndex;
                nodes_.push_back({ node, GetRenderBatchNodeHeadRaw(node) });
                nodeIndex = *reinterpret_cast<std::uint16_t*>(node + kBatchRendererNodeNextOffset);
            }

            active_ = true;
        }

        ~ScopedStaticRenderBatchesRestore()
        {
            if (!active_) {
                return;
            }

            *reinterpret_cast<std::uint32_t*>(
                batchRenderer_ + kBatchRendererGroupHeadOffset + group_ * sizeof(std::uint64_t)) = originalHead_;
            *reinterpret_cast<std::uint32_t*>(
                batchRenderer_ + kBatchRendererGroupTailOffset + group_ * sizeof(std::uint64_t)) = originalTail_;

            for (const auto& node : nodes_) {
                SetRenderBatchNodeHeadRaw(node.address, node.head);
            }
        }

        ScopedStaticRenderBatchesRestore(const ScopedStaticRenderBatchesRestore&) = delete;
        ScopedStaticRenderBatchesRestore& operator=(const ScopedStaticRenderBatchesRestore&) = delete;

    private:
        struct NodeHead
        {
            std::uintptr_t address = 0;
            BSRenderPassLayout* head = nullptr;
        };

        std::uintptr_t batchRenderer_ = 0;
        unsigned int group_ = 0;
        std::uint32_t originalHead_ = 0xFFFFu;
        std::uint32_t originalTail_ = 0xFFFFu;
        bool active_ = false;
        std::vector<NodeHead> nodes_;
    };

    enum class ShadowSplitPhase : std::uint8_t
    {
        None,
        StaticBuild,
        DynamicOverlay
    };

    constexpr std::size_t kGeometryGroupPersistentHeadOffset = 0x08;
    constexpr std::size_t kGeometryGroupFlagsOffset = 0x20;

    struct alignas(16) CommandBufferPassesDataSidecar
    {
        std::array<void*, kMaxCommandBufferRecords> records{};
        void** writeCursor = records.data();
        std::uint64_t pad10008 = 0;
        std::uint32_t frame = 0xFFFFFFFFu;
        std::uint32_t pad10014 = 0;
    };
    static_assert(offsetof(CommandBufferPassesDataSidecar, records) == 0x0);
    static_assert(offsetof(CommandBufferPassesDataSidecar, writeCursor) == kCommandBufferWriteCursorOffset);
    static_assert(offsetof(CommandBufferPassesDataSidecar, frame) == kCommandBufferFrameOffset);

    struct ShadowSplitChain
    {
        BSRenderPassLayout* head = nullptr;
        BSRenderPassLayout* tail = nullptr;
        std::vector<BSRenderPassLayout*> passes;
        std::vector<void*> commandBuffers;
    };

    struct ShadowSplitChains
    {
        ShadowSplitChain staticChain;
        ShadowSplitChain dynamicChain;
    };

    struct ActiveCommandBufferSelection
    {
        std::uintptr_t node = 0;
        BSRenderPassLayout* head = nullptr;
        std::uint32_t techniqueID = 0;
        std::vector<void*> commandBuffers;
    };

    struct CommandBufferSidecarCacheEntry
    {
        void* originalCbData = nullptr;
        std::uintptr_t node = 0;
        ShadowSplitPhase phase = ShadowSplitPhase::None;
        std::unique_ptr<CommandBufferPassesDataSidecar> data;
        std::vector<void*> selectedCommandBuffers;
        void** sourceWriteCursor = nullptr;
        std::uint32_t sourceFrame = 0xFFFFFFFFu;
    };

    struct PassGroupState
    {
        std::uint32_t head = 0xFFFFu;
        std::uint32_t tail = 0xFFFFu;
    };

    struct NodeState
    {
        std::uintptr_t address = 0;
        BSRenderPassLayout* head = nullptr;
        std::uint16_t next = 0xFFFFu;
    };

    struct PassLinkState
    {
        BSRenderPassLayout* pass = nullptr;
        BSRenderPassLayout* next = nullptr;
    };

    thread_local std::uint32_t tl_finishLevelShadowSplitDepth = 0;
    thread_local std::vector<ActiveCommandBufferSelection> tl_shadowCommandSelections;
    thread_local std::vector<CommandBufferSidecarCacheEntry> tl_commandBufferSidecars;
    constexpr std::size_t kMaxShadowCommandBufferSidecars = 256;

    ShadowSplitPhase CurrentShadowSplitPhase() noexcept
    {
        if (ShadowTelemetry::IsShadowCacheStaticBuildPass()) {
            return ShadowSplitPhase::StaticBuild;
        }
        if (ShadowTelemetry::IsShadowCacheDynamicOverlayPass()) {
            return ShadowSplitPhase::DynamicOverlay;
        }
        return ShadowSplitPhase::None;
    }

    bool IsFinishLevelShadowSplitActive() noexcept
    {
        return tl_finishLevelShadowSplitDepth != 0;
    }

    class ScopedFinishLevelShadowSplit
    {
    public:
        ScopedFinishLevelShadowSplit()
        {
            ++tl_finishLevelShadowSplitDepth;
        }

        ~ScopedFinishLevelShadowSplit()
        {
            --tl_finishLevelShadowSplitDepth;
        }

        ScopedFinishLevelShadowSplit(const ScopedFinishLevelShadowSplit&) = delete;
        ScopedFinishLevelShadowSplit& operator=(const ScopedFinishLevelShadowSplit&) = delete;
    };

    void AddPassToShadowSplitChain(ShadowSplitChain& chain, BSRenderPassLayout* pass)
    {
        if (!pass) {
            return;
        }

        if (!chain.head) {
            chain.head = pass;
        }
        chain.tail = pass;
        chain.passes.push_back(pass);
        auto* commandBufferRecord = reinterpret_cast<void*>(pass->next);
        if (!commandBufferRecord) {
            commandBufferRecord = pass->commandBuffer;
        }
        if (commandBufferRecord) {
            chain.commandBuffers.push_back(commandBufferRecord);
        }
    }

    ShadowSplitChains BuildShadowSplitChains(BSRenderPassLayout* head)
    {
        ShadowSplitChains chains{};
        const auto phase = CurrentShadowSplitPhase();
        if (phase == ShadowSplitPhase::None || !head) {
            return chains;
        }

        std::uint32_t count = 0;
        for (auto* pass = head; pass && count < 65536; pass = pass->passGroupNext, ++count) {
            const bool isStaticCaster = ClassifyShadowPass(pass);
            if (phase == ShadowSplitPhase::StaticBuild) {
                ShadowTelemetry::NoteShadowCacheRenderPassSplit(isStaticCaster, isStaticCaster);
            } else if (phase == ShadowSplitPhase::DynamicOverlay) {
                ShadowTelemetry::NoteShadowCacheRenderPassSplit(!isStaticCaster, isStaticCaster);
            }

            if (isStaticCaster) {
                AddPassToShadowSplitChain(chains.staticChain, pass);
            } else {
                AddPassToShadowSplitChain(chains.dynamicChain, pass);
            }
        }
        return chains;
    }

    const ShadowSplitChain& SelectShadowSplitChain(const ShadowSplitChains& chains) noexcept
    {
        return ShadowTelemetry::IsShadowCacheStaticBuildPass() ? chains.staticChain : chains.dynamicChain;
    }

    void ExposeShadowSplitChain(const ShadowSplitChain& chain, std::vector<PassLinkState>& originalLinks)
    {
        for (std::size_t i = 0; i < chain.passes.size(); ++i) {
            auto* pass = chain.passes[i];
            if (!pass) {
                continue;
            }
            originalLinks.push_back({ pass, pass->passGroupNext });
            pass->passGroupNext = (i + 1 < chain.passes.size()) ? chain.passes[i + 1] : nullptr;
        }
    }

    void RestorePassLinks(std::vector<PassLinkState>& originalLinks)
    {
        for (const auto& link : originalLinks) {
            if (link.pass) {
                link.pass->passGroupNext = link.next;
            }
        }
        originalLinks.clear();
    }

    std::uintptr_t GetBatchRendererNodeArray(std::uintptr_t batchRenderer, unsigned int group)
    {
        if (!batchRenderer || group >= 32) {
            return 0;
        }
        return *reinterpret_cast<std::uintptr_t*>(
            batchRenderer + kBatchRendererGroupStride * group + kBatchRendererGroupArrayOffset);
    }

    PassGroupState ReadBatchGroupState(std::uintptr_t batchRenderer, unsigned int group)
    {
        PassGroupState state{};
        if (!batchRenderer || group >= 32) {
            return state;
        }
        state.head = *reinterpret_cast<std::uint32_t*>(
            batchRenderer + kBatchRendererGroupHeadOffset + group * sizeof(std::uint64_t));
        state.tail = *reinterpret_cast<std::uint32_t*>(
            batchRenderer + kBatchRendererGroupTailOffset + group * sizeof(std::uint64_t));
        return state;
    }

    void WriteBatchGroupState(std::uintptr_t batchRenderer, unsigned int group, PassGroupState state)
    {
        if (!batchRenderer || group >= 32) {
            return;
        }
        *reinterpret_cast<std::uint32_t*>(
            batchRenderer + kBatchRendererGroupHeadOffset + group * sizeof(std::uint64_t)) = state.head;
        *reinterpret_cast<std::uint32_t*>(
            batchRenderer + kBatchRendererGroupTailOffset + group * sizeof(std::uint64_t)) = state.tail;
    }

    std::size_t GetCommandBufferRecordCount(void* cbData)
    {
        if (!cbData) {
            return 0;
        }

        auto** records = static_cast<void**>(cbData);
        auto** cursor = *reinterpret_cast<void***>(
            static_cast<std::byte*>(cbData) + kCommandBufferWriteCursorOffset);
        const auto recordsAddr = reinterpret_cast<std::uintptr_t>(records);
        const auto cursorAddr = reinterpret_cast<std::uintptr_t>(cursor);
        const auto maxCursorAddr = recordsAddr + kMaxCommandBufferRecords * sizeof(void*);
        if (cursorAddr >= recordsAddr &&
            cursorAddr <= maxCursorAddr &&
            ((cursorAddr - recordsAddr) % sizeof(void*)) == 0) {
            return static_cast<std::size_t>((cursorAddr - recordsAddr) / sizeof(void*));
        }

        std::size_t count = 0;
        while (count < kMaxCommandBufferRecords && records[count]) {
            ++count;
        }
        return count;
    }

    bool ContainsPointer(const std::vector<void*>& values, void* needle)
    {
        return std::find(values.begin(), values.end(), needle) != values.end();
    }

    CommandBufferSidecarCacheEntry& GetCommandBufferSidecarEntry(
        void* cbData,
        std::uintptr_t node,
        ShadowSplitPhase phase)
    {
        for (auto& entry : tl_commandBufferSidecars) {
            if (entry.originalCbData == cbData &&
                entry.node == node &&
                entry.phase == phase) {
                return entry;
            }
        }

        if (tl_commandBufferSidecars.size() >= kMaxShadowCommandBufferSidecars) {
            tl_commandBufferSidecars.clear();
        }

        auto& entry = tl_commandBufferSidecars.emplace_back();
        entry.originalCbData = cbData;
        entry.node = node;
        entry.phase = phase;
        entry.data = std::make_unique<CommandBufferPassesDataSidecar>();
        return entry;
    }

    ActiveCommandBufferSelection* FindActiveCommandBufferSelection(std::uintptr_t node)
    {
        if (!node) {
            return nullptr;
        }

        for (auto it = tl_shadowCommandSelections.rbegin(); it != tl_shadowCommandSelections.rend(); ++it) {
            if (it->node == node) {
                return std::addressof(*it);
            }
        }
        return nullptr;
    }

    std::vector<void*> CollectCommandBufferRecords(BSRenderPassLayout* head)
    {
        std::vector<void*> records;
        for (auto* pass = head; pass && records.size() < 65536; pass = pass->passGroupNext) {
            auto* commandBufferRecord = reinterpret_cast<void*>(pass->next);
            if (!commandBufferRecord) {
                commandBufferRecord = pass->commandBuffer;
            }
            if (commandBufferRecord) {
                records.push_back(commandBufferRecord);
            }
        }
        return records;
    }

    void* PrepareShadowCommandBufferSidecar(
        void* cbData,
        const ActiveCommandBufferSelection& selection,
        ShadowSplitPhase phase)
    {
        if (!cbData || selection.commandBuffers.empty() || phase == ShadowSplitPhase::None) {
            return nullptr;
        }

        auto& entry = GetCommandBufferSidecarEntry(cbData, selection.node, phase);
        if (!entry.data) {
            entry.data = std::make_unique<CommandBufferPassesDataSidecar>();
        }

        auto** sourceRecords = static_cast<void**>(cbData);
        auto** sourceCursor = *reinterpret_cast<void***>(
            static_cast<std::byte*>(cbData) + kCommandBufferWriteCursorOffset);
        const auto sourceFrame = *reinterpret_cast<std::uint32_t*>(
            static_cast<std::byte*>(cbData) + kCommandBufferFrameOffset);
        const bool needsRebuild =
            entry.sourceWriteCursor != sourceCursor ||
            entry.sourceFrame != sourceFrame ||
            entry.selectedCommandBuffers != selection.commandBuffers;

        if (needsRebuild) {
            std::fill(entry.data->records.begin(), entry.data->records.end(), nullptr);
            const std::size_t sourceCount = GetCommandBufferRecordCount(cbData);
            std::size_t keptCount = 0;
            std::vector<void*> keptRecords;
            keptRecords.reserve(selection.commandBuffers.size());

            for (std::size_t i = 0; i < sourceCount && keptCount < kMaxCommandBufferRecords; ++i) {
                void* record = sourceRecords[i];
                if (record && ContainsPointer(selection.commandBuffers, record)) {
                    entry.data->records[keptCount++] = record;
                    keptRecords.push_back(record);
                }
            }

            for (void* commandBuffer : selection.commandBuffers) {
                if (commandBuffer && !ContainsPointer(keptRecords, commandBuffer)) {
                    return nullptr;
                }
            }
            if (keptCount == 0) {
                return nullptr;
            }

            entry.data->writeCursor = entry.data->records.data() + keptCount;
            if (keptCount < kMaxCommandBufferRecords) {
                entry.data->records[keptCount] = nullptr;
            }
            entry.selectedCommandBuffers = selection.commandBuffers;
            entry.sourceWriteCursor = sourceCursor;
            entry.sourceFrame = sourceFrame;
        }

        entry.data->frame = sourceFrame;
        return entry.data.get();
    }

    class ScopedShadowBatchGroupExposure
    {
    public:
        ScopedShadowBatchGroupExposure(void* batchRenderer, unsigned int group) :
            batchRenderer_(reinterpret_cast<std::uintptr_t>(batchRenderer)),
            group_(group)
        {
            if (!IsShadowCacheRenderSplitActive() ||
                !batchRenderer_) {
                return;
            }

            nodeArray_ = GetBatchRendererNodeArray(batchRenderer_, group_);
            if (!nodeArray_) {
                return;
            }

            active_ = true;
            originalGroup_ = ReadBatchGroupState(batchRenderer_, group_);
            const bool clearAfterRender = *reinterpret_cast<std::uint8_t*>(batchRenderer_ + 0x350) != 0;
            restore_ = ShadowTelemetry::IsShadowCacheStaticBuildPass() || !clearAfterRender;
            std::uint32_t filteredHead = 0xFFFFu;
            std::uint32_t filteredTail = 0xFFFFu;

            std::uint32_t nodeIndex = originalGroup_.head;
            for (std::size_t count = 0; nodeIndex != kInvalidBatchNode && count < 65536; ++count) {
                const std::uintptr_t node = nodeArray_ + kBatchRendererNodeStride * nodeIndex;
                const auto originalHead = GetRenderBatchNodeHeadRaw(node);
                const auto originalNext = *reinterpret_cast<std::uint16_t*>(node + kBatchRendererNodeNextOffset);
                originalNodes_.push_back({ node, originalHead, originalNext });

                const auto chains = BuildShadowSplitChains(originalHead);
                const auto& selected = SelectShadowSplitChain(chains);
                ExposeShadowSplitChain(selected, originalPassLinks_);
                SetRenderBatchNodeHeadRaw(node, selected.head);

                if (selected.head) {
                    if (filteredHead == 0xFFFFu) {
                        filteredHead = nodeIndex;
                    } else if (filteredTail != 0xFFFFu) {
                        const std::uintptr_t tailNode = nodeArray_ + kBatchRendererNodeStride * filteredTail;
                        *reinterpret_cast<std::uint16_t*>(tailNode + kBatchRendererNodeNextOffset) =
                            static_cast<std::uint16_t>(nodeIndex);
                    }
                    filteredTail = nodeIndex;
                    *reinterpret_cast<std::uint16_t*>(node + kBatchRendererNodeNextOffset) = 0xFFFFu;

                    tl_shadowCommandSelections.push_back({
                        node,
                        selected.head,
                        GetRenderBatchNodeTechniqueRaw(node),
                        selected.commandBuffers
                    });
                    ++commandSelectionCount_;
                } else if (!restore_ && clearAfterRender) {
                    SetRenderBatchNodeHeadRaw(node, nullptr);
                }

                nodeIndex = originalNext;
            }

            WriteBatchGroupState(batchRenderer_, group_, { filteredHead, filteredTail });
            empty_ = filteredHead == 0xFFFFu;
        }

        ~ScopedShadowBatchGroupExposure()
        {
            if (!active_) {
                return;
            }

            RestorePassLinks(originalPassLinks_);
            if (commandSelectionCount_ <= tl_shadowCommandSelections.size()) {
                tl_shadowCommandSelections.resize(tl_shadowCommandSelections.size() - commandSelectionCount_);
            } else {
                tl_shadowCommandSelections.clear();
            }

            if (!restore_) {
                for (const auto& node : originalNodes_) {
                    *reinterpret_cast<std::uint16_t*>(node.address + kBatchRendererNodeNextOffset) = node.next;
                }
                return;
            }

            WriteBatchGroupState(batchRenderer_, group_, originalGroup_);
            for (const auto& node : originalNodes_) {
                SetRenderBatchNodeHeadRaw(node.address, node.head);
                *reinterpret_cast<std::uint16_t*>(node.address + kBatchRendererNodeNextOffset) = node.next;
            }
        }

        bool Active() const noexcept { return active_; }
        bool Empty() const noexcept { return active_ && empty_; }

        ScopedShadowBatchGroupExposure(const ScopedShadowBatchGroupExposure&) = delete;
        ScopedShadowBatchGroupExposure& operator=(const ScopedShadowBatchGroupExposure&) = delete;

    private:
        std::uintptr_t batchRenderer_ = 0;
        unsigned int group_ = 0;
        std::uintptr_t nodeArray_ = 0;
        PassGroupState originalGroup_{};
        std::vector<NodeState> originalNodes_;
        std::vector<PassLinkState> originalPassLinks_;
        std::size_t commandSelectionCount_ = 0;
        bool active_ = false;
        bool restore_ = false;
        bool empty_ = false;
    };

    class ScopedPersistentShadowListExposure
    {
    public:
        explicit ScopedPersistentShadowListExposure(void* persistentPassList) :
            list_(static_cast<BSRenderPassLayout**>(persistentPassList))
        {
            if (!list_ || !IsShadowCacheRenderSplitActive()) {
                return;
            }

            active_ = true;
            restore_ = ShadowTelemetry::IsShadowCacheStaticBuildPass();
            originalHead_ = list_[0];
            originalTail_ = list_[1];

            const auto chains = BuildShadowSplitChains(originalHead_);
            const auto& selected = SelectShadowSplitChain(chains);
            ExposeShadowSplitChain(selected, originalPassLinks_);
            list_[0] = selected.head;
            list_[1] = selected.tail;
        }

        ~ScopedPersistentShadowListExposure()
        {
            if (!active_) {
                return;
            }

            RestorePassLinks(originalPassLinks_);
            if (restore_ && list_) {
                list_[0] = originalHead_;
                list_[1] = originalTail_;
            }
        }

        bool Active() const noexcept { return active_; }

        ScopedPersistentShadowListExposure(const ScopedPersistentShadowListExposure&) = delete;
        ScopedPersistentShadowListExposure& operator=(const ScopedPersistentShadowListExposure&) = delete;

    private:
        BSRenderPassLayout** list_ = nullptr;
        BSRenderPassLayout* originalHead_ = nullptr;
        BSRenderPassLayout* originalTail_ = nullptr;
        std::vector<PassLinkState> originalPassLinks_;
        bool active_ = false;
        bool restore_ = false;
    };

    class ScopedShadowPassChainFilter
    {
    public:
        explicit ScopedShadowPassChainFilter(BSRenderPassLayout* head)
        {
            if (IsFinishLevelShadowSplitActive() ||
                !SHADOW_CACHE_DIRECTIONAL_MAPSLOT1_ON ||
                !ShadowTelemetry::IsShadowCacheActiveForCurrentShadow() ||
                (!ShadowTelemetry::IsShadowCacheStaticBuildPass() &&
                 !ShadowTelemetry::IsShadowCacheDynamicOverlayPass())) {
                filteredHead_ = head;
                return;
            }

            BSRenderPassLayout* tail = nullptr;
            for (auto* pass = head; pass && originalLinks_.size() < 65536; pass = pass->passGroupNext) {
                originalLinks_.push_back({ pass, pass->passGroupNext });
                if (!ShouldKeepShadowPassForCurrentSplit(pass)) {
                    continue;
                }

                if (!filteredHead_) {
                    filteredHead_ = pass;
                }
                if (tail) {
                    tail->passGroupNext = pass;
                }
                tail = pass;
            }

            if (tail) {
                tail->passGroupNext = nullptr;
            }
        }

        ~ScopedShadowPassChainFilter()
        {
            for (const auto& link : originalLinks_) {
                if (link.pass) {
                    link.pass->passGroupNext = link.next;
                }
            }
        }

        BSRenderPassLayout* Head() const noexcept
        {
            return filteredHead_;
        }

        ScopedShadowPassChainFilter(const ScopedShadowPassChainFilter&) = delete;
        ScopedShadowPassChainFilter& operator=(const ScopedShadowPassChainFilter&) = delete;

    private:
        struct Link
        {
            BSRenderPassLayout* pass = nullptr;
            BSRenderPassLayout* next = nullptr;
        };

        BSRenderPassLayout* filteredHead_ = nullptr;
        std::vector<Link> originalLinks_;
    };

    class ScopedCommandBufferPassesFilter
    {
    public:
        ScopedCommandBufferPassesFilter(void* batchRenderer, int group, void* cbData, unsigned int subIdx) :
            cbData_(cbData),
            records_(static_cast<void**>(cbData)),
            node_(GetRenderBatchNodeAddress(batchRenderer, group, subIdx))
        {
            if (IsFinishLevelShadowSplitActive() ||
                !IsShadowCacheRenderSplitActive() ||
                group != static_cast<int>(kShadowCacheBatchPassGroup) ||
                !cbData_ ||
                !records_ ||
                !node_) {
                return;
            }

            active_ = true;
            restoreFrame_ = ShadowTelemetry::IsShadowCacheStaticBuildPass();
            originalFrame_ = *reinterpret_cast<std::uint32_t*>(
                static_cast<std::byte*>(cbData_) + kCommandBufferFrameOffset);
            originalNodeHead_ = GetRenderBatchNodeHeadRaw(node_);
            techniqueID_ = GetRenderBatchNodeTechniqueRaw(node_);
            writeCursorSlot_ = reinterpret_cast<void***>(
                static_cast<std::byte*>(cbData_) + kCommandBufferWriteCursorOffset);
            originalWriteCursor_ = *writeCursorSlot_;

            const std::size_t recordCount = GetOriginalRecordCount();
            originalRecords_.assign(records_, records_ + recordCount);
            if (recordCount < kMaxCommandBufferRecords) {
                terminatorSlot_ = records_ + recordCount;
                originalTerminator_ = *terminatorSlot_;
            }

            BSRenderPassLayout* filteredHead = nullptr;
            BSRenderPassLayout* filteredTail = nullptr;
            std::vector<void*> keptCommandBuffers;
            for (auto* pass = originalNodeHead_; pass && originalLinks_.size() < 65536; pass = pass->passGroupNext) {
                originalLinks_.push_back({ pass, pass->passGroupNext });
                if (!ShouldKeepShadowPassForCurrentSplit(pass)) {
                    continue;
                }

                if (!filteredHead) {
                    filteredHead = pass;
                }
                if (filteredTail) {
                    filteredTail->passGroupNext = pass;
                }
                filteredTail = pass;
                if (pass->commandBuffer) {
                    keptCommandBuffers.push_back(pass->commandBuffer);
                }
            }

            if (filteredTail) {
                filteredTail->passGroupNext = nullptr;
            }
            filteredHead_ = filteredHead;
            SetRenderBatchNodeHeadRaw(node_, filteredHead_);

            for (void* record : originalRecords_) {
                if (Contains(keptCommandBuffers, record)) {
                    records_[keptRecordCount_++] = record;
                }
            }
            *writeCursorSlot_ = records_ + keptRecordCount_;
            if (keptRecordCount_ < kMaxCommandBufferRecords) {
                records_[keptRecordCount_] = nullptr;
            }

            for (void* record : keptCommandBuffers) {
                if (!Contains(originalRecords_, record)) {
                    needsImmediateFallback_ = true;
                    break;
                }
            }
        }

        ~ScopedCommandBufferPassesFilter()
        {
            if (!active_) {
                return;
            }

            for (const auto& link : originalLinks_) {
                if (link.pass) {
                    link.pass->passGroupNext = link.next;
                }
            }
            SetRenderBatchNodeHeadRaw(node_, originalNodeHead_);

            for (std::size_t i = 0; i < originalRecords_.size(); ++i) {
                records_[i] = originalRecords_[i];
            }
            if (terminatorSlot_) {
                *terminatorSlot_ = originalTerminator_;
            }
            if (writeCursorSlot_) {
                *writeCursorSlot_ = originalWriteCursor_;
            }
            if (restoreFrame_) {
                *reinterpret_cast<std::uint32_t*>(
                    static_cast<std::byte*>(cbData_) + kCommandBufferFrameOffset) = originalFrame_;
            }
        }

        bool Active() const noexcept { return active_; }
        BSRenderPassLayout* Head() const noexcept { return filteredHead_; }
        std::uint32_t TechniqueID() const noexcept { return techniqueID_; }
        bool NeedsImmediateFallback() const noexcept
        {
            return active_ && filteredHead_ && (keptRecordCount_ == 0 || needsImmediateFallback_);
        }

        ScopedCommandBufferPassesFilter(const ScopedCommandBufferPassesFilter&) = delete;
        ScopedCommandBufferPassesFilter& operator=(const ScopedCommandBufferPassesFilter&) = delete;

    private:
        static bool Contains(const std::vector<void*>& records, void* needle)
        {
            return std::find(records.begin(), records.end(), needle) != records.end();
        }

        std::size_t GetOriginalRecordCount() const
        {
            const auto recordsAddr = reinterpret_cast<std::uintptr_t>(records_);
            const auto cursorAddr = reinterpret_cast<std::uintptr_t>(originalWriteCursor_);
            const auto maxCursorAddr = recordsAddr + kMaxCommandBufferRecords * sizeof(void*);
            if (cursorAddr >= recordsAddr && cursorAddr <= maxCursorAddr &&
                ((cursorAddr - recordsAddr) % sizeof(void*)) == 0) {
                return static_cast<std::size_t>((cursorAddr - recordsAddr) / sizeof(void*));
            }

            std::size_t count = 0;
            while (count < kMaxCommandBufferRecords && records_[count]) {
                ++count;
            }
            return count;
        }

        struct Link
        {
            BSRenderPassLayout* pass = nullptr;
            BSRenderPassLayout* next = nullptr;
        };

        void* cbData_ = nullptr;
        void** records_ = nullptr;
        void*** writeCursorSlot_ = nullptr;
        void** originalWriteCursor_ = nullptr;
        void** terminatorSlot_ = nullptr;
        void* originalTerminator_ = nullptr;
        std::uintptr_t node_ = 0;
        BSRenderPassLayout* originalNodeHead_ = nullptr;
        BSRenderPassLayout* filteredHead_ = nullptr;
        std::uint32_t techniqueID_ = 0;
        std::uint32_t originalFrame_ = 0xFFFFFFFFu;
        std::size_t keptRecordCount_ = 0;
        bool active_ = false;
        bool restoreFrame_ = false;
        bool needsImmediateFallback_ = false;
        std::vector<void*> originalRecords_;
        std::vector<Link> originalLinks_;
    };

    class ScopedPersistentShadowPassListFilter
    {
    public:
        explicit ScopedPersistentShadowPassListFilter(void* persistentPassList) :
            list_(static_cast<BSRenderPassLayout**>(persistentPassList))
        {
            if (!list_) {
                return;
            }

            originalHead_ = list_[0];
            originalTail_ = list_[1];

            if (IsFinishLevelShadowSplitActive() ||
                !SHADOW_CACHE_DIRECTIONAL_MAPSLOT1_ON ||
                !ShadowTelemetry::IsShadowCacheActiveForCurrentShadow() ||
                (!ShadowTelemetry::IsShadowCacheStaticBuildPass() &&
                 !ShadowTelemetry::IsShadowCacheDynamicOverlayPass())) {
                return;
            }

            active_ = true;
            restoreListHead_ = ShadowTelemetry::IsShadowCacheStaticBuildPass();

            BSRenderPassLayout* filteredHead = nullptr;
            BSRenderPassLayout* filteredTail = nullptr;
            for (auto* pass = originalHead_; pass && originalLinks_.size() < 65536; pass = pass->passGroupNext) {
                originalLinks_.push_back({ pass, pass->passGroupNext });
                if (!ShouldKeepShadowPassForCurrentSplit(pass)) {
                    continue;
                }

                if (!filteredHead) {
                    filteredHead = pass;
                }
                if (filteredTail) {
                    filteredTail->passGroupNext = pass;
                }
                filteredTail = pass;
            }

            if (filteredTail) {
                filteredTail->passGroupNext = nullptr;
            }
            list_[0] = filteredHead;
            list_[1] = filteredTail;
        }

        ~ScopedPersistentShadowPassListFilter()
        {
            for (const auto& link : originalLinks_) {
                if (link.pass) {
                    link.pass->passGroupNext = link.next;
                }
            }

            if (active_ && restoreListHead_ && list_) {
                list_[0] = originalHead_;
                list_[1] = originalTail_;
            }
        }

        bool Empty() const noexcept
        {
            return list_ && list_[0] == nullptr;
        }

        ScopedPersistentShadowPassListFilter(const ScopedPersistentShadowPassListFilter&) = delete;
        ScopedPersistentShadowPassListFilter& operator=(const ScopedPersistentShadowPassListFilter&) = delete;

    private:
        struct Link
        {
            BSRenderPassLayout* pass = nullptr;
            BSRenderPassLayout* next = nullptr;
        };

        BSRenderPassLayout** list_ = nullptr;
        BSRenderPassLayout* originalHead_ = nullptr;
        BSRenderPassLayout* originalTail_ = nullptr;
        bool active_ = false;
        bool restoreListHead_ = false;
        std::vector<Link> originalLinks_;
    };

    class ScopedProcessCommandBufferTarget
    {
    public:
        explicit ScopedProcessCommandBufferTarget(const ShadowTelemetry::WorkTarget& target) :
            previous_(tl_processCommandBufferTarget),
            wasActive_(tl_processCommandBufferTargetActive)
        {
            tl_processCommandBufferTarget = target;
            tl_processCommandBufferTargetActive = true;
        }

        ~ScopedProcessCommandBufferTarget()
        {
            tl_processCommandBufferTarget = previous_;
            tl_processCommandBufferTargetActive = wasActive_;
        }

        ScopedProcessCommandBufferTarget(const ScopedProcessCommandBufferTarget&) = delete;
        ScopedProcessCommandBufferTarget& operator=(const ScopedProcessCommandBufferTarget&) = delete;

    private:
        ShadowTelemetry::WorkTarget previous_{};
        bool wasActive_ = false;
    };

    using BSBatchRendererDraw_t = void (*)(BSRenderPassLayout* pass, std::uintptr_t unk2, std::uintptr_t unk3, RE::BSGraphics::DynamicTriShapeDrawData* dynamicDrawData);
    BSBatchRendererDraw_t OriginalBSBatchRendererDraw = nullptr;
    REL::Relocation<std::uintptr_t> ptr_BSBatchRendererDraw{ REL::ID{ 1152191, 2318696 } };

    using BSShaderSetupGeometry_t = void (*)(RE::BSShader* shader, BSRenderPassLayout* pass);
    BSShaderSetupGeometry_t OriginalBSLightingShaderSetupGeometry = nullptr;
    BSShaderSetupGeometry_t OriginalBSEffectShaderSetupGeometry = nullptr;

    using BSShaderRestoreGeometry_t = void (*)(RE::BSShader* shader, BSRenderPassLayout* pass);
    BSShaderRestoreGeometry_t OriginalBSLightingShaderRestoreGeometry = nullptr;
    BSShaderRestoreGeometry_t OriginalBSEffectShaderRestoreGeometry = nullptr;

    using BipedAnimAttachSkinnedObject_t = RE::NiAVObject* (*)(RE::BipedAnim* biped, RE::NiNode* destinationRoot, RE::NiNode* sourceRoot, RE::BIPED_OBJECT bipedObject, bool firstPerson);
    BipedAnimAttachSkinnedObject_t OriginalBipedAnimAttachSkinnedObject = nullptr;
    REL::Relocation<std::uintptr_t> ptr_BipedAnimAttachSkinnedObject{ REL::ID{ 1575810, 2194388 } };

    using BipedAnimAttachBipedWeapon_t = void (*)(RE::BipedAnim* biped, const RE::BGSObjectInstanceT<RE::TESObjectWEAP>& weapon, RE::BGSEquipIndex equipIndex);
    BipedAnimAttachBipedWeapon_t OriginalBipedAnimAttachBipedWeapon = nullptr;
    REL::Relocation<std::uintptr_t> ptr_BipedAnimAttachBipedWeapon{ REL::ID{ 788361, 2194353 } };

    using BipedAnimAttachToParent_t = void (*)(RE::NiAVObject* parent, RE::NiAVObject* attachedObject, RE::NiAVObject* sourceObject, RE::BSTSmartPointer<RE::BipedAnim>& biped, RE::BIPED_OBJECT bipedObject);
    BipedAnimAttachToParent_t OriginalBipedAnimAttachToParent = nullptr;
    REL::Relocation<std::uintptr_t> ptr_BipedAnimAttachToParent{ REL::ID{ 1370428, 2194378 } };

    using BipedAnimRemovePart_t = void (*)(RE::BipedAnim* biped, RE::BIPOBJECT* bipObject, bool queueDetach);
    BipedAnimRemovePart_t OriginalBipedAnimRemovePart = nullptr;
    REL::Relocation<std::uintptr_t> ptr_BipedAnimRemovePart{ REL::ID{ 575576, 2194342 } };

    using ActorLoad3D_t = RE::NiAVObject* (*)(RE::TESObjectREFR* ref, bool backgroundLoading);
    ActorLoad3D_t OriginalActorLoad3D = nullptr;
    ActorLoad3D_t OriginalPlayerCharacterLoad3D = nullptr;

    // TESObjectREFR vfuncs we hook to widen actor-tag coverage past Load3D:
    //   0x88 Set3D                — every "this ref now has a different 3D root"
    //   0x98 OnHeadInitialized    — async FaceGen completion (head-only path)
    using Set3D_t = void (*)(RE::TESObjectREFR* ref, RE::NiAVObject* a_object, bool a_queue3DTasks);
    Set3D_t OriginalActorSet3D = nullptr;
    Set3D_t OriginalPlayerCharacterSet3D = nullptr;

    using OnHeadInitialized_t = void (*)(RE::TESObjectREFR* ref);
    OnHeadInitialized_t OriginalActorOnHeadInitialized = nullptr;
    OnHeadInitialized_t OriginalPlayerCharacterOnHeadInitialized = nullptr;

    // AIProcess::Update3dModel — the engine's runtime model-rebuild entry
    // (race/sex change, NPC template re-roll, Reset3D/queue completion, PA
    // enter/exit). IDs from tools/og_to_ae_function_mapping.csv.
    using Update3DModel_t = void (*)(void* middleProcess, RE::Actor* actor, bool flag);
    Update3DModel_t OriginalUpdate3DModel = nullptr;
    REL::Relocation<std::uintptr_t> ptr_Update3DModel{ REL::ID{ 986782, 2231882 } };

    // Actor::Reset3D — script/console-driven full reload (disable;enable,
    // ResetActorReference). Eventually queues a Load3D on the loader thread.
    using Reset3D_t = void (*)(RE::Actor* actor, bool a_reloadAll, std::uint32_t a_additionalFlags, bool a_queueReset, std::uint32_t a_excludeFlags);
    Reset3D_t OriginalReset3D = nullptr;
    REL::Relocation<std::uintptr_t> ptr_Reset3D{ REL::ID{ 302888, 2229913 } };

    // Script::ModifyFaceGen::ReplaceHeadTask::Run — fires for LooksMenu apply,
    // SexChange, RegenerateHead Papyrus calls. Vtable available for OG only in
    // the commonlibf4 we ship; AE install is skipped until we have the address.
    using ReplaceHeadTaskRun_t = void (*)(void* this_);
    ReplaceHeadTaskRun_t OriginalReplaceHeadTaskRun = nullptr;

    // BSBatchRenderer::RenderCommandBufferPassesImpl - drives the command-buffer
    // replay path. We hook entry to scan the pass list and decide between two
    // strategies (see HookedRenderCommandBufferPassesImpl for the full story).
    // Mangled: ?RenderCommandBufferPassesImpl@BSBatchRenderer@@QEAAXHPEAUCommandBufferPassesData@1@I_N@Z
    using RenderBatches_t = void (*)(void* this_, unsigned int passGroupIdx, bool allowAlpha, unsigned int filter);
    RenderBatches_t OriginalRenderBatches = nullptr;
    RenderBatches_t OriginalFinishShadowRenderBatches = nullptr;
    REL::Relocation<std::uintptr_t> ptr_RenderBatches{ REL::ID{ 1083446, 0 } };

    using RenderGeometryGroup_t = void (*)(void* accumulator, unsigned int group, bool allowAlpha);
    RenderGeometryGroup_t OriginalFinishShadowRenderGeometryGroup = nullptr;
    REL::Relocation<std::uintptr_t> ptr_RenderGeometryGroup{ REL::ID{ 1379976, 2317897 } };

    REL::Relocation<std::uintptr_t> ptr_FinishAccumulatingShadowMapOrMask{ REL::ID{ 1358523, 2317874 } };
    constexpr std::uintptr_t kFinishShadowRenderBatchesCallOffsetOG = 0x38;       // 0x14282E4A8 - 0x14282E470
    constexpr std::uintptr_t kFinishShadowRenderGeometryGroupCallOffsetOG = 0x51; // 0x14282E4C1 - 0x14282E470

    using RenderCommandBufferPassesImpl_t = void (*)(void* this_, int passGroupIdx, void* cbData, unsigned int subIdx, bool allowAlpha);
    RenderCommandBufferPassesImpl_t OriginalRenderCommandBufferPassesImpl = nullptr;
    REL::Relocation<std::uintptr_t> ptr_RenderCommandBufferPassesImpl{ REL::ID{ 1184461, 2318711 } };

    using RenderPersistentPassListImpl_t = void (*)(void* persistentPassList, bool allowAlpha);
    RenderPersistentPassListImpl_t OriginalRenderPersistentPassListImpl = nullptr;
    REL::Relocation<std::uintptr_t> ptr_RenderPersistentPassListImpl{ REL::ID{ 1170655, 2318712 } };
    constexpr std::uintptr_t kGeometryGroupRenderPersistentPassListCallOffsetOG = 0x719;  // 0x142880909 - 0x1428801F0

    using ProcessCommandBuffer_t = void (*)(void* renderer, void* cbData);
    ProcessCommandBuffer_t OriginalProcessCommandBuffer = nullptr;
    REL::Relocation<std::uintptr_t> ptr_ProcessCommandBuffer{ REL::ID{ 673619, 0 } };  // OG 1.10.163: 0x141D13A10

    // BSBatchRenderer::RenderPassImpl - the immediate-path equivalent of
    // RenderCommandBufferPassesImpl. Calls RenderPassImmediately on the head
    // pass (which goes through BeginPass / state cleanup) and iterates the
    // passGroupNext chain calling RenderPassImmediatelySameTechnique for each.
    // Mangled: ?RenderPassImpl@BSBatchRenderer@@QEAAXPEAVBSRenderPass@@I_N@Z
    using RenderPassImpl_t = void (*)(void* this_, BSRenderPassLayout* head, std::uint32_t techniqueID, bool allowAlpha);
    RenderPassImpl_t OriginalRenderPassImpl = nullptr;
    REL::Relocation<std::uintptr_t> ptr_RenderPassImpl{ REL::ID{ 1543785, 2318710 } };

    using RegisterObjectShadowMapOrMask_t = bool (*)(void* accumulator, RE::BSGeometry* geometry, void* shaderProperty);
    RegisterObjectShadowMapOrMask_t OriginalRegisterObjectShadowMapOrMask = nullptr;
    REL::Relocation<std::uintptr_t> ptr_RegisterObjectShadowMapOrMask{ REL::ID{ 1071289, 2317861 } };
    constexpr std::uintptr_t kRegisterObjectShadowMapOrMaskRegisterPassCallOffsetOG = 0xCF;  // 0x14282D8CF - 0x14282D800

    using RegisterPassGeometryGroup_t = void (*)(void* batchRenderer, BSRenderPassLayout* pass, int group, bool appendOrUnk);
    RegisterPassGeometryGroup_t OriginalRegisterPassGeometryGroup = nullptr;
    REL::Relocation<std::uintptr_t> ptr_RegisterPassGeometryGroup{ REL::ID{ 197098, 0 } };

    // BSShader::BuildCommandBuffer - the chokepoint where command buffers are
    // built per pass. We hook this to inject one extra
    // CommandBufferShaderResource record (slot=DRAWTAG_SLOT, kind=0, stage=PS)
    // into the SRV table the engine memcpy's into the buffer. Because every
    // recorded entry in ProcessCommandBuffer's replay loop iterates the SRV
    // table and binds DRAWTAG_SLOT, this gives correct per-draw tag binding for
    // command-buffered draws — without needing to intercept the replay itself.
    //
    // Implementation strategy: mutate BuildCommandBufferParam in-place before
    // calling original, then restore. The engine's MemoryManager::Allocate
    // computes size from srvCount so bumping it allocates room for our extra
    // record, and the existing memcpy(SRVs, srvSrc, 16*srvCount) inside
    // BuildCommandBuffer copies our extended array. No reallocation, no
    // platform-tail shifting, no lifetime tracking.
    //
    // Mangled: ?BuildCommandBuffer@BSShader@@QEAAPEAXAEAUBuildCommandBufferParam@1@@Z
    using BuildCommandBuffer_t = char* (*)(void* this_, void* param);
    BuildCommandBuffer_t OriginalBuildCommandBuffer = nullptr;
    REL::Relocation<std::uintptr_t> ptr_BuildCommandBuffer{ REL::ID{ 833764, 2318870 } };

    // BSLight::TestFrustumCull — engine's per-light frustum cull. We wrap it
    // so LightCullPolicy can temporarily scale geometry[+0x138] (the source
    // bound radius the cull function reads) for the duration of the engine's
    // call, then restore. See src/LightCullPolicy.{h,cpp} for the why.
    // Mangled: ?TestFrustumCull@BSLight@@QEAAIAEAVNiCullingProcess@@@Z
    // AE/NG IDs not yet mapped — REL::ID's second slot is 0 and will fail to
    // resolve on AE; install is still attempted so the failure is loud rather
    // than silent. Fill in the AE ID in tools/og_ae_function_map.csv to enable.
    using BSLightTestFrustumCull_t = std::uint32_t (*)(void* light, void* cullingProcess);
    BSLightTestFrustumCull_t OriginalBSLightTestFrustumCull = nullptr;
    REL::Relocation<std::uintptr_t> ptr_BSLightTestFrustumCull{ REL::ID{ 1440624, 0 } };

    // BSDFTiledLighting::AddLight call-site redirect — MULTIPLIES the radius
    // arg by the active scale so the tiled-buffer slot (offset +0x10 of each
    // 48-byte slot) sees the scaled radius. The downstream pDFTLCS compute
    // shader uses this for tile assignment + per-tile attenuation.
    //
    // Context: LightCullPolicy applies *transient* scaling around individual
    // engine consumers (cull-time and mesh-setup-time), restoring vanilla
    // immediately after each so shadow projection / fade-distance keep their
    // original values. By the time the lambda reads geometry[+0x138] for the
    // AddLight radius arg, the cull-side scale has already been restored —
    // so the arg is vanilla. We multiply here to give the tiled-buffer slot
    // the same scaled value the per-volume mesh sees.
    //
    // The function has only ONE caller (this lambda); patch the `call rel32`
    // at lambda+0x281 directly so AddLight's RIP-relative prologue stays
    // untouched.
    using BSDFTiledLightingAddLight_t = void (*)(std::uint32_t id,
                                                 const RE::NiPoint3* pos,
                                                 float radius,
                                                 const RE::NiColor* color,
                                                 const RE::NiPoint3* dir,
                                                 bool flagA, bool flagB, bool flagC, bool flagD);
    BSDFTiledLightingAddLight_t OriginalBSDFTiledLightingAddLight = nullptr;
    REL::Relocation<std::uintptr_t> ptr_TryAddTiledLightLambda{ REL::ID{ 999390, 0 } };
    constexpr std::uintptr_t kAddLightCallSiteOffsetOG = 0x281;  // 0x142813F61 - 0x142813CE0

    // BSDFLightShader::SetupPointLightGeometry — per-volume rasterization
    // setup. Called from DrawWorld::DrawPointLight to bind the technique,
    // build the world matrix from the light's bounding sphere, and push
    // per-light constants. Internally calls SetupPointLightTransforms which
    // reads geometry[+0x138] to size the rasterized sphere mesh. Hooking the
    // outer function lets us scale the source bound radius for the duration
    // of the entire mesh-setup chain, then restore — so the volume mesh
    // expands but shadow projection (which runs outside this window) sees
    // the vanilla radius.
    // Mangled: ?SetupPointLightGeometry@BSDFLightShader@@QEAA_NPEAVBSLight@@I_N@Z
    using SetupPointLightGeometry_t = bool (*)(void* shader, void* light, std::uint32_t mask, char isUnk);
    SetupPointLightGeometry_t OriginalSetupPointLightGeometry = nullptr;
    REL::Relocation<std::uintptr_t> ptr_SetupPointLightGeometry{ REL::ID{ 212931, 0 } };

    thread_local float g_currentDrawTag = static_cast<float>(DrawMaterialTag::kUnknown);
    thread_local float g_currentDrawTagIsHead = 0.0f;
    thread_local std::vector<float> g_drawTagStack;
    thread_local std::vector<float> g_drawTagIsHeadStack;
    std::unordered_set<RE::BSGeometry*> g_actorDrawTaggedGeometry;
    // Subset of g_actorDrawTaggedGeometry consisting of geometry reachable from
    // the actor's BSFaceGenNiNode subtree (head/face/eyes/hair). Used by the
    // pixel-shader-side isHead flag so toon face shading and facegen-aware eye
    // logic can run without re-tagging materialTag.
    std::unordered_set<RE::BSGeometry*> g_actorHeadDrawTaggedGeometry;
    std::unordered_map<std::uint32_t, std::unordered_set<RE::BSGeometry*>> g_actorDrawTaggedGeometryByRef;
    // Subset of g_actorDrawTaggedGeometryByRef limited to BSGeometry reachable
    // from each ref's BSFaceGenNiNode subtree, so the OnHeadInitialized path
    // can diff the head independently of the body without re-walking the full
    // actor scenegraph. Maintained alongside the full per-ref set during full
    // refreshes; mutated alone during head-only refreshes.
    std::unordered_map<std::uint32_t, std::unordered_set<RE::BSGeometry*>> g_actorHeadDrawTaggedGeometryByRef;
    std::shared_mutex g_actorDrawTaggedGeometryLock;

    template <class T>
    T CreateBranchGateway(REL::Relocation<std::uintptr_t>& target, std::size_t prologueSize, void* hook)
    {
        const auto targetAddress = target.address();
        auto& trampoline = REL::GetTrampoline();
        auto* gateway = static_cast<std::byte*>(trampoline.allocate(prologueSize + sizeof(REL::ASM::JMP14)));
        std::memcpy(gateway, reinterpret_cast<void*>(targetAddress), prologueSize);

        const REL::ASM::JMP14 jumpBack{ targetAddress + prologueSize };
        std::memcpy(gateway + prologueSize, &jumpBack, sizeof(jumpBack));

        target.replace_func(prologueSize, hook);
        return reinterpret_cast<T>(gateway);
    }

    // Same idea as CreateBranchGateway but patches the target with a 5-byte
    // rel32 JMP (via a trampoline thunk) instead of replace_func's 14-byte
    // absolute. Use this when the target's prologue is a single 5-byte
    // instruction (e.g. `mov [rsp+0x18], rbp`) and there isn't room for a
    // 14-byte patch without clobbering an unknown follow-up instruction.
    // prologueSize must equal an exact instruction boundary >= 5 bytes.
    //
    // Handles chained hooks: if another mod (e.g. BodyShapeManager on
    // AIProcess::Update3dModel) has already 5-byte-hooked the target, the
    // bytes we capture are *their* `E9 <rel32>` JMP — RIP-relative and
    // therefore meaningless at the gateway's new address. We detect that
    // case and re-encode the rel32 so the gateway preserves the absolute
    // destination (their thunk → their hook → ultimately the real function).
    template <class T>
    T CreateBranchGateway5(REL::Relocation<std::uintptr_t>& target, std::size_t prologueSize, void* hook)
    {
        const auto targetAddress = target.address();
        auto& trampoline = REL::GetTrampoline();
        auto* gateway = static_cast<std::byte*>(trampoline.allocate(prologueSize + sizeof(REL::ASM::JMP14)));
        std::memcpy(gateway, reinterpret_cast<void*>(targetAddress), prologueSize);

        if (prologueSize >= 5 && gateway[0] == std::byte{ 0xE9 }) {
            std::int32_t oldRel32;
            std::memcpy(&oldRel32, gateway + 1, sizeof(oldRel32));
            const auto absoluteDest = static_cast<std::int64_t>(targetAddress) + 5 + oldRel32;
            const auto newRel64     = absoluteDest - (reinterpret_cast<std::int64_t>(gateway) + 5);
            if (newRel64 < INT32_MIN || newRel64 > INT32_MAX) {
                REX::WARN(
                    "CreateBranchGateway5: captured JMP destination {:#x} is unreachable from gateway {} via rel32 — chained hook likely broken",
                    static_cast<std::uintptr_t>(absoluteDest),
                    static_cast<void*>(gateway));
                return nullptr;
            }
            const auto newRel32 = static_cast<std::int32_t>(newRel64);
            std::memcpy(gateway + 1, &newRel32, sizeof(newRel32));
            REX::INFO(
                "CreateBranchGateway5: target {:#x} already hooked — re-encoded captured E9 to preserve absolute destination {:#x}",
                static_cast<std::uintptr_t>(targetAddress),
                static_cast<std::uintptr_t>(absoluteDest));
        } else if (prologueSize >= 5 && gateway[0] == std::byte{ 0xE8 }) {
            // CALL rel32 — semantics involve pushing a return address that
            // would now point inside the gateway, not target+5. Refusing
            // rather than silently corrupting control flow.
            REX::WARN(
                "CreateBranchGateway5: captured CALL rel32 at target {:#x} cannot be safely relocated; another mod hooked this function with a CALL trampoline",
                static_cast<std::uintptr_t>(targetAddress));
            return nullptr;
        }

        const REL::ASM::JMP14 jumpBack{ targetAddress + prologueSize };
        std::memcpy(gateway + prologueSize, &jumpBack, sizeof(jumpBack));

        trampoline.write_jmp5(targetAddress, reinterpret_cast<std::uintptr_t>(hook));
        return reinterpret_cast<T>(gateway);
    }

    void SetCommandBufferDrawTagWrapper(RE::BSGeometry* geometry, WrapperTag tag);
    void ResetCommandBufferDrawTagWrappers();
    FakeStructuredResource* GetCommandBufferDrawTagWrapper(RE::BSGeometry* geometry, WrapperTag tag);
    bool EnsureDrawTagWrapperResources();

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

    struct PassOcclusionState
    {
        REX::W32::ID3D11Query* query = nullptr;
        bool pending = false;
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

    constexpr std::size_t kPassOcclusionMaxEntries = 32768;
    constexpr std::uint32_t kPassOcclusionMainQueryBudget = 384;
    constexpr std::uint32_t kPassOcclusionShadowQueryBudget = 192;
    constexpr std::uint32_t kPassOcclusionMainSkipBudget = 2048;
    constexpr std::uint32_t kPassOcclusionShadowSkipBudget = 512;
    constexpr std::uint8_t kPassOcclusionMainHiddenFrames = 2;
    constexpr std::uint8_t kPassOcclusionShadowHiddenFrames = 3;
    constexpr std::uint8_t kPassOcclusionMainMaxSkips = 2;
    constexpr std::uint8_t kPassOcclusionShadowMaxSkips = 1;
    constexpr std::uint64_t kPassOcclusionStaleFrames = 600;
    constexpr std::uint32_t kPassOcclusionPrunePeriod = 120;
    constexpr std::uint64_t kPassOcclusionTelemetryPeriod = 300;

    enum class PassOcclusionGeometryReject : std::uint8_t
    {
        None,
        NullOrSkinned,
        Type,
        Radius,
        NearCamera,
        ActorTagged
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
        std::uint64_t cameraStableFrames = 0;
        std::uint64_t cameraUnstableFrames = 0;
        std::uint64_t cameraInvalidFrames = 0;
        std::uint64_t pruned = 0;
        std::array<std::uint64_t, 32> noDomainByGroup{};
        std::array<std::uint64_t, 32> mainByGroup{};
        std::array<std::uint64_t, 32> shadowByGroup{};
    };

    std::unordered_map<PassOcclusionKey, PassOcclusionState, PassOcclusionKeyHash> g_passOcclusionCache;
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

    thread_local unsigned int g_renderBatchesGroup = UINT_MAX;
    thread_local std::uint32_t g_renderBatchesDepth = 0;

    bool IsMainGBufferGroup(unsigned int group)
    {
        switch (group) {
            case 0:
            case 1:
            case 4:
            case 7:
            case 8:
            case 9:
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
        std::shared_lock lock(g_actorDrawTaggedGeometryLock);
        return g_actorDrawTaggedGeometry.contains(geometry);
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
        const UINT depthTarget = g_currentDepthTargetIndex.load(std::memory_order_relaxed);
        const bool hasRT = g_currentHasRenderTarget.load(std::memory_order_relaxed);

        if (depthTarget == static_cast<UINT>(MAIN_DEPTHSTENCIL_TARGET) &&
            hasRT &&
            IsMainGBufferGroup(group)) {
            return PassOcclusionDomain::MainGBuffer;
        }

        if (depthTarget == static_cast<UINT>(DepthStencilTarget::kShadowMap) && !hasRT) {
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
        return GetRenderBatchNodeHeadRaw(GetRenderBatchNodeAddress(batchRenderer, group, subIdx));
    }

    void ResolvePassOcclusionQuery(REX::W32::ID3D11DeviceContext* context, PassOcclusionState& state)
    {
        if (!context || !state.query || !state.pending) {
            return;
        }

        std::uint64_t samples = 0;
        const HRESULT hr = context->GetData(
            state.query,
            &samples,
            sizeof(samples),
            REX::W32::D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if (hr != S_OK) {
            ++g_passOcclusionTelemetry.queryPending;
            return;
        }

        state.pending = false;
        if (samples == 0) {
            ++g_passOcclusionTelemetry.resolvedZero;
            if (state.hiddenStreak < 255) {
                ++state.hiddenStreak;
            }
        } else {
            ++g_passOcclusionTelemetry.resolvedVisible;
            state.hiddenStreak = 0;
            state.skippedSinceRefresh = 0;
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

    struct PassOcclusionDecision
    {
        PassOcclusionState* state = nullptr;
        PassOcclusionDomain domain = PassOcclusionDomain::None;
        bool queryActive = false;
        bool skip = false;
    };

    PassOcclusionDecision BeginPassOcclusionDecision(
        REX::W32::ID3D11DeviceContext* context,
        void* batchRenderer,
        BSRenderPassLayout* head,
        unsigned int group,
        bool allowAlpha,
        bool commandBufferPath)
    {
        PassOcclusionDecision decision;
        ++g_passOcclusionTelemetry.attempts;
        if (commandBufferPath) {
            ++g_passOcclusionTelemetry.commandAttempts;
        } else {
            ++g_passOcclusionTelemetry.immediateAttempts;
        }

        if (!PASS_LEVEL_OCCLUSION_ON) {
            ++g_passOcclusionTelemetry.disabled;
            return decision;
        }
        if (!context || !batchRenderer || !head || g_renderBatchesDepth == 0) {
            ++g_passOcclusionTelemetry.badInput;
            return decision;
        }

        const UINT depthTarget = g_currentDepthTargetIndex.load(std::memory_order_relaxed);
        const bool hasRT = g_currentHasRenderTarget.load(std::memory_order_relaxed);
        const bool inDeferredPrePass = PhaseTelemetry::IsInDeferredPrePass();
        const bool mainGroup = IsMainGBufferGroup(group);
        const PassOcclusionDomain domain = GetCurrentPassOcclusionDomain(group);
        if (domain == PassOcclusionDomain::None) {
            ++g_passOcclusionTelemetry.noDomain;
            if (group < g_passOcclusionTelemetry.noDomainByGroup.size()) {
                ++g_passOcclusionTelemetry.noDomainByGroup[group];
            }
            if (depthTarget == static_cast<UINT>(MAIN_DEPTHSTENCIL_TARGET)) {
                if (!hasRT) {
                    ++g_passOcclusionTelemetry.noDomainMainNoRT;
                }
                if (!inDeferredPrePass) {
                    ++g_passOcclusionTelemetry.noDomainMainNotDeferredPrePass;
                }
                if (!mainGroup) {
                    ++g_passOcclusionTelemetry.noDomainMainGroup;
                }
            } else if (depthTarget == static_cast<UINT>(DepthStencilTarget::kShadowMap)) {
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
        ResolvePassOcclusionQuery(context, *state);

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
            decision.domain = domain;
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
        decision.domain = domain;
        return decision;
    }

    void EndPassOcclusionDecision(REX::W32::ID3D11DeviceContext* context, PassOcclusionDecision& decision)
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
    }

    void CollectActorDrawTaggedGeometry(RE::NiAVObject* root, std::unordered_set<RE::BSGeometry*>& geometry)
    {
        if (!root) {
            return;
        }

        if (netimmerse_cast<RE::NiBillboardNode*>(root)) {
            if (DEBUGGING) {
                REX::INFO("ActorDrawTagScan: skip billboard node='{}'", root->name.c_str());
            }
            return;
        }

        if (auto* geom = root->IsGeometry()) {
            const bool inserted = geometry.insert(geom).second;
            if (DEBUGGING) {
                REX::INFO(
                    "ActorDrawTagScan: {} geometry={} name='{}'",
                    inserted ? "insert" : "duplicate",
                    static_cast<void*>(geom),
                    geom->name.c_str());
            }
            return;
        }

        if (auto* node = root->IsNode()) {
            if (DEBUGGING) {
                REX::INFO("ActorDrawTagScan: descend node='{}' children={}", root->name.c_str(), node->children.size());
            }
            for (auto& child : node->children) {
                CollectActorDrawTaggedGeometry(child.get(), geometry);
            }
            return;
        }

        if (DEBUGGING) {
            REX::INFO("ActorDrawTagScan: skip unknown name='{}' (neither node nor geometry)", root->name.c_str());
        }
    }

    void RemoveActorDrawTaggedGeometry(RE::NiAVObject* root)
    {
        if (!root) {
            return;
        }

        std::unique_lock lock(g_actorDrawTaggedGeometryLock);
        RE::BSVisit::TraverseScenegraphGeometries(root, [](RE::BSGeometry* geometry) {
            if (geometry) {
                g_actorDrawTaggedGeometry.erase(geometry);
                g_actorHeadDrawTaggedGeometry.erase(geometry);
                SetCommandBufferDrawTagWrapper(geometry, WrapperTag::kUnknown);
            }
            return RE::BSVisit::BSVisitControl::kContinue;
        });
    }

    void RefreshActorDrawTaggedGeometry(RE::TESObjectREFR* ref)
    {
        if (!ref) {
            return;
        }

        const auto handle = ref->GetHandle().get_handle();
        if (!handle) {
            return;
        }

        std::unordered_set<RE::BSGeometry*> liveGeometry;
        CollectActorDrawTaggedGeometry(ref->Get3D(), liveGeometry);

        if (ref == RE::PlayerCharacter::GetSingleton()) {
            bool isFirstPerson = ref->Get3D(true) == ref->Get3D();
            CollectActorDrawTaggedGeometry(ref->Get3D(!isFirstPerson), liveGeometry);
        }

        // Snapshot the head subtree separately so a later head-only refresh
        // has a baseline to diff against.
        std::unordered_set<RE::BSGeometry*> liveHeadGeometry;
        if (auto* faceRaw = ref->GetFaceNodeSkinned()) {
            CollectActorDrawTaggedGeometry(reinterpret_cast<RE::NiAVObject*>(faceRaw), liveHeadGeometry);
        }

        std::unique_lock lock(g_actorDrawTaggedGeometryLock);
        auto& currentSet = g_actorDrawTaggedGeometryByRef[handle];

        auto& currentHeadSet = g_actorHeadDrawTaggedGeometryByRef[handle];

        // Diff: drop tags whose BSGeometry is no longer reachable in the
        // actor's scenegraph. This avoids the brief untagged window that a
        // wholesale clear-then-readd would produce on body geometry whenever
        // a non-body event (head rebuild, weapon attach, etc.) fires.
        for (auto it = currentSet.begin(); it != currentSet.end(); ) {
            if (!liveGeometry.contains(*it)) {
                g_actorDrawTaggedGeometry.erase(*it);
                g_actorHeadDrawTaggedGeometry.erase(*it);
                SetCommandBufferDrawTagWrapper(*it, WrapperTag::kUnknown);
                it = currentSet.erase(it);
            } else {
                ++it;
            }
        }

        // Diff: tag geometry the walk just discovered. Pick kActorHead for
        // anything reachable from BSFaceGenNiNode, kActor otherwise.
        for (auto* geometry : liveGeometry) {
            const bool isHead = liveHeadGeometry.contains(geometry);
            if (currentSet.insert(geometry).second) {
                g_actorDrawTaggedGeometry.insert(geometry);
                if (isHead) {
                    g_actorHeadDrawTaggedGeometry.insert(geometry);
                }
                SetCommandBufferDrawTagWrapper(geometry, isHead ? WrapperTag::kActorHead : WrapperTag::kActor);
            } else {
                // Pre-existing entry — head membership may have changed since
                // the previous full refresh (e.g. a weapon attach that runs
                // before head async completes). Resync the wrapper in case
                // the head subtree just shifted.
                const bool wasHead = currentHeadSet.contains(geometry);
                if (isHead != wasHead) {
                    if (isHead) {
                        g_actorHeadDrawTaggedGeometry.insert(geometry);
                    } else {
                        g_actorHeadDrawTaggedGeometry.erase(geometry);
                    }
                    SetCommandBufferDrawTagWrapper(geometry, isHead ? WrapperTag::kActorHead : WrapperTag::kActor);
                }
            }
        }

        currentHeadSet = std::move(liveHeadGeometry);
    }

    // Cheaper variant for FaceGen/head-only events: walks just the face node
    // and diffs against the per-ref head subset. Body/equipment tags are never
    // touched, so high-frequency head events (FaceGen async completion,
    // expression rebuilds) cannot flicker body draws.
    void RefreshActorHeadDrawTaggedGeometry(RE::TESObjectREFR* ref)
    {
        if (!ref) {
            return;
        }

        auto* faceRaw = ref->GetFaceNodeSkinned();
        if (!faceRaw) {
            return;
        }

        const auto handle = ref->GetHandle().get_handle();
        if (!handle) {
            return;
        }

        std::unordered_set<RE::BSGeometry*> liveHeadGeometry;
        CollectActorDrawTaggedGeometry(reinterpret_cast<RE::NiAVObject*>(faceRaw), liveHeadGeometry);

        std::unique_lock lock(g_actorDrawTaggedGeometryLock);
        auto& currentFullSet = g_actorDrawTaggedGeometryByRef[handle];
        auto& currentHeadSet = g_actorHeadDrawTaggedGeometryByRef[handle];

        // Diff: remove old head entries that are no longer present under the
        // face node. We trust that only head geometry was tracked here, so
        // dropping these from the full set as well is safe.
        for (auto it = currentHeadSet.begin(); it != currentHeadSet.end(); ) {
            if (!liveHeadGeometry.contains(*it)) {
                g_actorDrawTaggedGeometry.erase(*it);
                g_actorHeadDrawTaggedGeometry.erase(*it);
                SetCommandBufferDrawTagWrapper(*it, WrapperTag::kUnknown);
                currentFullSet.erase(*it);
                it = currentHeadSet.erase(it);
            } else {
                ++it;
            }
        }

        // Diff: add newly discovered head entries to both sets.
        for (auto* geometry : liveHeadGeometry) {
            if (currentHeadSet.insert(geometry).second) {
                currentFullSet.insert(geometry);
                g_actorDrawTaggedGeometry.insert(geometry);
                g_actorHeadDrawTaggedGeometry.insert(geometry);
                SetCommandBufferDrawTagWrapper(geometry, WrapperTag::kActorHead);
            }
        }
    }

    void RefreshActorDrawTaggedGeometry(RE::BipedAnim* biped)
    {
        if (!biped) {
            return;
        }

        auto ref = biped->GetRequester().get();
        RefreshActorDrawTaggedGeometry(ref.get());
    }

    struct DrawTagClassification
    {
        float materialTag;
        float isHead;
    };

    DrawTagClassification ClassifyDrawTag(BSRenderPassLayout* pass)
    {
        DrawTagClassification out{};
        out.materialTag = static_cast<float>(DrawMaterialTag::kUnknown);
        out.isHead = 0.0f;

        if (!pass || !pass->geometry) {
            return out;
        }

        std::shared_lock lock(g_actorDrawTaggedGeometryLock);
        if (g_actorDrawTaggedGeometry.contains(pass->geometry)) {
            out.materialTag = static_cast<float>(DrawMaterialTag::kActor);
            if (g_actorHeadDrawTaggedGeometry.contains(pass->geometry)) {
                out.isHead = 1.0f;
            }
        }

        return out;
    }

    REX::W32::ID3D11ShaderResourceView* GetStaticDrawTagSRV(WrapperTag tag)
    {
        switch (tag) {
            case WrapperTag::kActor:     return g_actorTagSRV;
            case WrapperTag::kActorHead: return g_actorHeadTagSRV;
            case WrapperTag::kUnknown:
            default:                     return g_unknownTagSRV;
        }
    }

    void SetCommandBufferDrawTagWrapper(RE::BSGeometry* geometry, WrapperTag tag)
    {
        if (!geometry || !g_tagWrapperResourcesReady) {
            return;
        }

        std::unique_lock lock(g_commandBufferDrawTagWrappersLock);
        auto it = g_commandBufferDrawTagWrappers.find(geometry);
        if (it == g_commandBufferDrawTagWrappers.end()) {
            if (tag == WrapperTag::kUnknown) {
                return;
            }
            it = g_commandBufferDrawTagWrappers.emplace(geometry, nullptr).first;
        }

        auto& wrapper = it->second;
        if (!wrapper) {
            wrapper = std::make_unique<FakeStructuredResource>();
            wrapper->fenceCount = 0;
        }

        wrapper->srv.store(GetStaticDrawTagSRV(tag), std::memory_order_release);
    }

    void ResetCommandBufferDrawTagWrappers()
    {
        std::unique_lock lock(g_commandBufferDrawTagWrappersLock);
        for (auto& [geometry, wrapper] : g_commandBufferDrawTagWrappers) {
            if (wrapper) {
                wrapper->srv.store(g_unknownTagSRV, std::memory_order_release);
            }
        }
    }

    FakeStructuredResource* GetCommandBufferDrawTagWrapper(RE::BSGeometry* geometry, WrapperTag tag)
    {
        if (!geometry) {
            return &g_unknownTagWrapper;
        }

        std::unique_lock lock(g_commandBufferDrawTagWrappersLock);
        auto& wrapper = g_commandBufferDrawTagWrappers[geometry];
        if (!wrapper) {
            wrapper = std::make_unique<FakeStructuredResource>();
            wrapper->fenceCount = 0;
        }

        wrapper->srv.store(GetStaticDrawTagSRV(tag), std::memory_order_release);
        return wrapper.get();
    }

    void PushCurrentDrawTag(BSRenderPassLayout* pass)
    {
        g_drawTagStack.push_back(g_currentDrawTag);
        g_drawTagIsHeadStack.push_back(g_currentDrawTagIsHead);
        const auto classification = ClassifyDrawTag(pass);
        g_currentDrawTag = classification.materialTag;
        g_currentDrawTagIsHead = classification.isHead;
    }

    void PopCurrentDrawTag()
    {
        if (!g_drawTagStack.empty()) {
            g_currentDrawTag = g_drawTagStack.back();
            g_drawTagStack.pop_back();
        } else {
            g_currentDrawTag = static_cast<float>(DrawMaterialTag::kUnknown);
        }

        if (!g_drawTagIsHeadStack.empty()) {
            g_currentDrawTagIsHead = g_drawTagIsHeadStack.back();
            g_drawTagIsHeadStack.pop_back();
        } else {
            g_currentDrawTagIsHead = 0.0f;
        }
    }

    void HookedBSLightingShaderSetupGeometry(RE::BSShader* shader, BSRenderPassLayout* pass)
    {
        PushCurrentDrawTag(pass);
        OriginalBSLightingShaderSetupGeometry(shader, pass);
    }

    void HookedBSLightingShaderRestoreGeometry(RE::BSShader* shader, BSRenderPassLayout* pass)
    {
        OriginalBSLightingShaderRestoreGeometry(shader, pass);
        PopCurrentDrawTag();
    }

    void HookedBSEffectShaderSetupGeometry(RE::BSShader* shader, BSRenderPassLayout* pass)
    {
        PushCurrentDrawTag(pass);
        OriginalBSEffectShaderSetupGeometry(shader, pass);
    }

    void HookedBSEffectShaderRestoreGeometry(RE::BSShader* shader, BSRenderPassLayout* pass)
    {
        OriginalBSEffectShaderRestoreGeometry(shader, pass);
        PopCurrentDrawTag();
    }

    void HookedBSBatchRendererDraw(BSRenderPassLayout* pass, std::uintptr_t unk2, std::uintptr_t unk3, RE::BSGraphics::DynamicTriShapeDrawData* dynamicDrawData)
    {
        EnsureD3DDrawHooksPresent();

        // Per-phase G-buffer telemetry. Cheap when INI is `off`. Attributes
        // this draw to whichever DrawWorld:: sub-phase is innermost on the TLS
        // stack (no-op if outside Render_PreUI entirely).
        PhaseTelemetry::OnDraw();
        ShadowTelemetry::OnBSDraw();

        PushCurrentDrawTag(pass);

        if (DEBUGGING) {
            const bool actorTag = g_currentDrawTag >= 0.5f;
            if (actorTag && pass && pass->geometry) {
                struct DrawKey {
                    RE::BSGeometry* geometry;
                    REX::W32::ID3D11PixelShader* shader;
                    std::uint32_t techniqueID;
                    bool operator==(const DrawKey& o) const noexcept {
                        return geometry == o.geometry && shader == o.shader && techniqueID == o.techniqueID;
                    }
                };
                struct DrawKeyHash {
                    std::size_t operator()(const DrawKey& k) const noexcept {
                        std::size_t h = std::hash<void*>{}(k.geometry);
                        h ^= std::hash<void*>{}(k.shader) + 0x9e3779b9 + (h << 6) + (h >> 2);
                        h ^= std::hash<std::uint32_t>{}(k.techniqueID) + 0x9e3779b9 + (h << 6) + (h >> 2);
                        return h;
                    }
                };

                static std::unordered_set<DrawKey, DrawKeyHash> loggedPairs;
                static std::mutex loggedPairsMutex;

                auto* originalShader = g_currentOriginalPixelShader.load(std::memory_order_acquire);
                const DrawKey key{ pass->geometry, originalShader, pass->techniqueID };

                bool shouldLog = false;
                {
                    std::lock_guard lock(loggedPairsMutex);
                    shouldLog = loggedPairs.insert(key).second;
                }

                if (shouldLog) {
                    std::string shaderUID = "<no PS>";
                    std::string defId = "<unmatched>";
                    if (originalShader) {
                        std::shared_lock entryLock(g_ShaderDB.mutex);
                        auto it = g_ShaderDB.entries.find(originalShader);
                        if (it != g_ShaderDB.entries.end()) {
                            shaderUID = it->second.shaderUID.empty() ? "<no UID>" : it->second.shaderUID;
                            if (it->second.matchedDefinition) {
                                defId = it->second.matchedDefinition->id;
                            }
                        } else {
                            shaderUID = "<no DB entry>";
                        }
                    }

                    REX::INFO(
                        "ActorDrawTag: geometry={} name='{}' techniqueID=0x{:08X} psShader={} shaderUID={} matchedDef={}",
                        static_cast<void*>(pass->geometry),
                        pass->geometry->name.c_str(),
                        pass->techniqueID,
                        static_cast<void*>(originalShader),
                        shaderUID,
                        defId);
                }
            }
        }

        // Bind+update via the immediate context here, because in some configurations
        // (ENB / ReShade-wrapped contexts, command-buffer replay paths) the D3D11
        // vtable hooks on g_rendererData->context don't fire for actor draws even
        // though BSBatchRenderer::Draw does. Doing it engine-side guarantees the
        // tag buffer is current for every pass that classified as actor.
        if (g_rendererData && g_rendererData->context) {
            BindDrawTagForCurrentDraw(g_rendererData->context, true);
            if (CustomPass::g_registry.OnBeforeDraw(g_rendererData->context, "engine-BSBatch")
                && g_activeReplacementPixelShader) {
                BindInjectedPixelShaderResources(g_rendererData->context);
            }
        }

        OriginalBSBatchRendererDraw(pass, unk2, unk3, dynamicDrawData);
        PopCurrentDrawTag();
    }

    RE::NiAVObject* HookedBipedAnimAttachSkinnedObject(RE::BipedAnim* biped, RE::NiNode* destinationRoot, RE::NiNode* sourceRoot, RE::BIPED_OBJECT bipedObject, bool firstPerson)
    {
        auto* attachedObject = OriginalBipedAnimAttachSkinnedObject(biped, destinationRoot, sourceRoot, bipedObject, firstPerson);
        RefreshActorDrawTaggedGeometry(biped);
        return attachedObject;
    }

    void HookedBipedAnimAttachBipedWeapon(RE::BipedAnim* biped, const RE::BGSObjectInstanceT<RE::TESObjectWEAP>& weapon, RE::BGSEquipIndex equipIndex)
    {
        OriginalBipedAnimAttachBipedWeapon(biped, weapon, equipIndex);
        RefreshActorDrawTaggedGeometry(biped);
    }

    void HookedBipedAnimAttachToParent(RE::NiAVObject* parent, RE::NiAVObject* attachedObject, RE::NiAVObject* sourceObject, RE::BSTSmartPointer<RE::BipedAnim>& biped, RE::BIPED_OBJECT bipedObject)
    {
        OriginalBipedAnimAttachToParent(parent, attachedObject, sourceObject, biped, bipedObject);

        if (biped && bipedObject != RE::BIPED_OBJECT::kTotal) {
            RefreshActorDrawTaggedGeometry(biped.get());
        }
    }

    void HookedBipedAnimRemovePart(RE::BipedAnim* biped, RE::BIPOBJECT* bipObject, bool queueDetach)
    {
        if (bipObject) {
            RemoveActorDrawTaggedGeometry(bipObject->partClone.get());
        }

        OriginalBipedAnimRemovePart(biped, bipObject, queueDetach);
        RefreshActorDrawTaggedGeometry(biped);
    }

    void HookedFinishShadowRenderBatches(void* this_, unsigned int passGroupIdx, bool allowAlpha, unsigned int filter)
    {
        auto renderBatches = OriginalRenderBatches ? OriginalRenderBatches : OriginalFinishShadowRenderBatches;
        if (!renderBatches) {
            return;
        }

        const unsigned int prevGroup = g_renderBatchesGroup;
        ++g_renderBatchesDepth;
        g_renderBatchesGroup = passGroupIdx;
        {
            if (!ShouldSplitRenderBatchesCall(passGroupIdx, filter)) {
                renderBatches(this_, passGroupIdx, allowAlpha, filter);
            } else {
                ScopedFinishLevelShadowSplit finishSplit;
                ScopedShadowBatchGroupExposure exposure(this_, passGroupIdx);
                if (!exposure.Empty()) {
                    renderBatches(this_, passGroupIdx, allowAlpha, filter);
                }
            }
        }
        g_renderBatchesGroup = prevGroup;
        --g_renderBatchesDepth;
    }

    void HookedFinishShadowRenderGeometryGroup(void* accumulator, unsigned int group, bool allowAlpha)
    {
        if (!OriginalFinishShadowRenderGeometryGroup) {
            return;
        }

        if (!IsShadowCacheRenderSplitActive() || group != 9 || !accumulator) {
            OriginalFinishShadowRenderGeometryGroup(accumulator, group, allowAlpha);
            return;
        }

        auto* geometryGroup = *reinterpret_cast<std::byte**>(
            static_cast<std::byte*>(accumulator) + 0x420 + static_cast<std::size_t>(group) * sizeof(void*));
        const bool persistentListMode =
            geometryGroup &&
            ((*reinterpret_cast<std::uint8_t*>(geometryGroup + kGeometryGroupFlagsOffset) & 1u) != 0);
        if (!persistentListMode) {
            OriginalFinishShadowRenderGeometryGroup(accumulator, group, allowAlpha);
            return;
        }

        {
            ScopedFinishLevelShadowSplit finishSplit;
            ScopedPersistentShadowListExposure exposure(geometryGroup + kGeometryGroupPersistentHeadOffset);
            OriginalFinishShadowRenderGeometryGroup(accumulator, group, allowAlpha);
        }
    }

    void HookedRenderBatches(void* this_, unsigned int passGroupIdx, bool allowAlpha, unsigned int filter)
    {
        const unsigned int prevGroup = g_renderBatchesGroup;
        ++g_renderBatchesDepth;
        g_renderBatchesGroup = passGroupIdx;
        {
            if (ShouldSplitRenderBatchesCall(passGroupIdx, filter)) {
                ScopedFinishLevelShadowSplit renderBatchesSplit;
                ScopedShadowBatchGroupExposure exposure(this_, passGroupIdx);
                if (!exposure.Empty()) {
                    OriginalRenderBatches(this_, passGroupIdx, allowAlpha, filter);
                }
            } else {
                ScopedStaticRenderBatchesRestore staticBuildRestore(this_, passGroupIdx);
                OriginalRenderBatches(this_, passGroupIdx, allowAlpha, filter);
            }
        }
        g_renderBatchesGroup = prevGroup;
        --g_renderBatchesDepth;
    }

    void HookedProcessCommandBuffer(void* renderer, void* cbData)
    {
        if (!tl_processCommandBufferTargetActive) {
            OriginalProcessCommandBuffer(renderer, cbData);
            return;
        }

        auto target = tl_processCommandBufferTarget;
        target.cbData = cbData;

        if (!IsShadowWorkTelemetryActive()) {
            OriginalProcessCommandBuffer(renderer, cbData);
            return;
        }

        ScopedShadowWork shadowWork(ShadowTelemetry::WorkKind::CommandBufferReplay, target);
        OriginalProcessCommandBuffer(renderer, cbData);
    }

    void HookedRenderCommandBufferPassesImpl(void* this_, int passGroupIdx, void* cbData, unsigned int subIdx, bool allowAlpha)
    {
        EnsureD3DDrawHooksPresent();

        PhaseTelemetry::OnCommandBufferDraw();
        ShadowTelemetry::OnCommandBufferDraw();

        auto* head = GetRenderBatchNodeHead(this_, passGroupIdx, subIdx);
        auto decision = BeginPassOcclusionDecision(
            g_rendererData ? g_rendererData->context : nullptr,
            this_,
            head,
            passGroupIdx >= 0 ? static_cast<unsigned int>(passGroupIdx) : g_renderBatchesGroup,
            allowAlpha,
            true);
        if (decision.skip) {
            InvalidateCommandBufferFrame(cbData);
            return;
        }

        {
            const auto node = GetRenderBatchNodeAddress(this_, passGroupIdx, subIdx);
            ActiveCommandBufferSelection fallbackSelection{};
            auto* shadowSelection =
                IsFinishLevelShadowSplitActive() ? FindActiveCommandBufferSelection(node) : nullptr;
            const bool forceSelectedReplay =
                IsFinishLevelShadowSplitActive() &&
                IsShadowCacheRenderSplitActive() &&
                (passGroupIdx == static_cast<int>(kShadowCacheBatchPassGroup) ||
                 ShadowTelemetry::IsShadowCacheActiveForCurrentShadow());
            if (forceSelectedReplay && !shadowSelection) {
                fallbackSelection.node = node;
                fallbackSelection.head = head;
                fallbackSelection.techniqueID = node ? GetRenderBatchNodeTechniqueRaw(node) : 0;
                fallbackSelection.commandBuffers = CollectCommandBufferRecords(head);
                shadowSelection = &fallbackSelection;
            }

            void* replayCbData = cbData;
            if (shadowSelection) {
                const auto phase = CurrentShadowSplitPhase();
                if (!shadowSelection->head) {
                    if (phase == ShadowSplitPhase::DynamicOverlay) {
                        InvalidateCommandBufferFrame(cbData);
                    }
                    EndPassOcclusionDecision(g_rendererData ? g_rendererData->context : nullptr, decision);
                    return;
                }

                replayCbData = PrepareShadowCommandBufferSidecar(cbData, *shadowSelection, phase);
                if (!replayCbData) {
                    OriginalRenderPassImpl(
                        this_,
                        shadowSelection->head,
                        shadowSelection->techniqueID,
                        allowAlpha);
                    if (phase == ShadowSplitPhase::DynamicOverlay) {
                        InvalidateCommandBufferFrame(cbData);
                    }
                    EndPassOcclusionDecision(g_rendererData ? g_rendererData->context : nullptr, decision);
                    return;
                }
            }

            ScopedCommandBufferPassesFilter shadowSplitFilter(this_, passGroupIdx, cbData, subIdx);
            if (shadowSplitFilter.Active()) {
                if (!shadowSplitFilter.Head()) {
                    InvalidateCommandBufferFrame(cbData);
                    EndPassOcclusionDecision(g_rendererData ? g_rendererData->context : nullptr, decision);
                    return;
                }

                if (shadowSplitFilter.NeedsImmediateFallback()) {
                    OriginalRenderPassImpl(
                        this_,
                        shadowSplitFilter.Head(),
                        shadowSplitFilter.TechniqueID(),
                        allowAlpha);
                    InvalidateCommandBufferFrame(cbData);
                    EndPassOcclusionDecision(g_rendererData ? g_rendererData->context : nullptr, decision);
                    return;
                }
            }

            std::optional<ScopedProcessCommandBufferTarget> processTarget;
            if (IsShadowWorkTelemetryActive() ||
                ShadowTelemetry::IsShadowCacheActiveForCurrentShadow()) {
                processTarget.emplace(MakeShadowPassWorkTarget(
                    this_,
                    shadowSelection ? shadowSelection->head : head,
                    replayCbData,
                    passGroupIdx,
                    subIdx,
                    allowAlpha));
            }

            // Per-draw classification for replayed command-buffer draws is handled
            // by the SRV record injected in HookedBuildCommandBuffer. Mark this
            // scope so the lower-level D3D hooks do not overwrite that recorded
            // SRV with the stale immediate-path g_currentDrawTag value.
            struct ReplayScope
            {
                ReplayScope() { ++g_commandBufferReplayDepth; }
                ~ReplayScope() { --g_commandBufferReplayDepth; }
            } replayScope;

            // If an old command buffer lacks our injected record, make it fall
            // back to unknown instead of inheriting the previous replayed draw tag.
            if (EnsureDrawTagWrapperResources() && g_rendererData && g_rendererData->context && g_unknownTagSRV) {
                g_bindingInjectedPixelResources = true;
                g_rendererData->context->PSSetShaderResources(DRAWTAG_SLOT, 1, &g_unknownTagSRV);
                g_bindingInjectedPixelResources = false;
            }

            OriginalRenderCommandBufferPassesImpl(this_, passGroupIdx, replayCbData, subIdx, allowAlpha);
            if (shadowSelection && CurrentShadowSplitPhase() == ShadowSplitPhase::DynamicOverlay) {
                InvalidateCommandBufferFrame(cbData);
            }
        }
        EndPassOcclusionDecision(g_rendererData ? g_rendererData->context : nullptr, decision);
    }

    void HookedRenderPersistentPassListImpl(void* persistentPassList, bool allowAlpha)
    {
        ScopedPersistentShadowPassListFilter shadowSplitFilter(persistentPassList);
        if (shadowSplitFilter.Empty() &&
            SHADOW_CACHE_DIRECTIONAL_MAPSLOT1_ON &&
            ShadowTelemetry::IsShadowCacheActiveForCurrentShadow()) {
            return;
        }

        OriginalRenderPersistentPassListImpl(persistentPassList, allowAlpha);
    }

    bool HookedRegisterObjectShadowMapOrMask(void* accumulator, RE::BSGeometry* geometry, void* shaderProperty)
    {
        const bool active = SHADOW_CACHE_DIRECTIONAL_MAPSLOT1_ON &&
            ShadowTelemetry::IsShadowCacheRegistrationFilterActive(accumulator, geometry);
        if (SHADOW_CACHE_DIRECTIONAL_MAPSLOT1_ON) {
            ShadowTelemetry::NoteShadowCacheShadowMapOrMaskHookDetail(active, accumulator);
        }

        return OriginalRegisterObjectShadowMapOrMask(accumulator, geometry, shaderProperty);
    }

    void HookedRegisterPassGeometryGroup(void* batchRenderer, BSRenderPassLayout* pass, int group, bool appendOrUnk)
    {
        if (SHADOW_CACHE_DIRECTIONAL_MAPSLOT1_ON && pass) {
            const bool isStaticCaster = IsPrecombineShadowGeometry(pass->geometry);
            RememberShadowPassClassification(pass, isStaticCaster);
        }

        OriginalRegisterPassGeometryGroup(batchRenderer, pass, group, appendOrUnk);
    }

    void HookedRenderPassImpl(void* this_, BSRenderPassLayout* head, std::uint32_t techniqueID, bool allowAlpha)
    {
        const unsigned int group = g_renderBatchesGroup;
        auto decision = BeginPassOcclusionDecision(
            g_rendererData ? g_rendererData->context : nullptr,
            this_,
            head,
            group,
            allowAlpha,
            false);
        if (decision.skip) {
            return;
        }

        {
            std::optional<ScopedShadowWork> shadowWork;
            if (IsShadowWorkTelemetryActive()) {
                auto target = MakeShadowPassWorkTarget(
                    this_,
                    head,
                    nullptr,
                    static_cast<std::int32_t>(group),
                    0,
                    allowAlpha);
                target.techniqueID = techniqueID;
                shadowWork.emplace(ShadowTelemetry::WorkKind::ImmediatePass, target);
            }
            ScopedShadowPassChainFilter shadowSplitFilter(head);
            if (auto* filteredHead = shadowSplitFilter.Head()) {
                OriginalRenderPassImpl(this_, filteredHead, techniqueID, allowAlpha);
            }
        }
        EndPassOcclusionDecision(g_rendererData ? g_rendererData->context : nullptr, decision);
    }

    // Lazy-create the two immutable structured buffers + SRVs and wire them
    // into the static FakeStructuredResource wrappers. Called from the
    // BuildCommandBuffer hook on first invocation, by which time the
    // device is up.
    bool EnsureDrawTagWrapperResources()
    {
        std::lock_guard initLock(g_tagWrapperInitMutex);
        if (g_tagWrapperResourcesReady) {
            return true;
        }

        if (!g_rendererData) {
            g_rendererData = RE::BSGraphics::GetRendererData();
        }
        if (!g_rendererData || !g_rendererData->device) {
            return false;
        }
        auto* device = g_rendererData->device;

        auto createTagSRV = [device](float tagValue,
                                     float isHeadValue,
                                     REX::W32::ID3D11Buffer*& outBuffer,
                                     REX::W32::ID3D11ShaderResourceView*& outSRV) -> bool
        {
            DrawTagData seed{};
            seed.materialTag = tagValue;
            seed.isHead = isHeadValue;
            seed.pad1 = 0.0f;
            seed.pad2 = 0.0f;

            REX::W32::D3D11_BUFFER_DESC desc{};
            desc.usage = REX::W32::D3D11_USAGE_IMMUTABLE;
            desc.byteWidth = sizeof(DrawTagData);
            desc.bindFlags = REX::W32::D3D11_BIND_SHADER_RESOURCE;
            desc.cpuAccessFlags = 0;
            desc.miscFlags = REX::W32::D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
            desc.structureByteStride = sizeof(DrawTagData);

            REX::W32::D3D11_SUBRESOURCE_DATA initialData{};
            initialData.sysMem = &seed;

            HRESULT hr = device->CreateBuffer(&desc, &initialData, &outBuffer);
            if (FAILED(hr)) {
                REX::WARN("EnsureDrawTagWrapperResources: CreateBuffer (tag={}) failed 0x{:08X}", tagValue, hr);
                return false;
            }

            REX::W32::D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
            srvDesc.format = REX::W32::DXGI_FORMAT_UNKNOWN;
            srvDesc.viewDimension = REX::W32::D3D11_SRV_DIMENSION_BUFFER;
            srvDesc.buffer.firstElement = 0;
            srvDesc.buffer.numElements = 1;

            hr = device->CreateShaderResourceView(outBuffer, &srvDesc, &outSRV);
            if (FAILED(hr)) {
                REX::WARN("EnsureDrawTagWrapperResources: CreateSRV (tag={}) failed 0x{:08X}", tagValue, hr);
                outBuffer->Release();
                outBuffer = nullptr;
                return false;
            }

            return true;
        };

        auto releaseTagSRV = [](REX::W32::ID3D11Buffer*& buffer,
                                REX::W32::ID3D11ShaderResourceView*& srv)
        {
            if (srv) {
                srv->Release();
                srv = nullptr;
            }
            if (buffer) {
                buffer->Release();
                buffer = nullptr;
            }
        };

        if (!createTagSRV(static_cast<float>(DrawMaterialTag::kUnknown), 0.0f, g_unknownTagBuffer, g_unknownTagSRV) ||
            !createTagSRV(static_cast<float>(DrawMaterialTag::kActor),   0.0f, g_actorTagBuffer,   g_actorTagSRV) ||
            !createTagSRV(static_cast<float>(DrawMaterialTag::kActor),   1.0f, g_actorHeadTagBuffer, g_actorHeadTagSRV)) {
            releaseTagSRV(g_unknownTagBuffer,   g_unknownTagSRV);
            releaseTagSRV(g_actorTagBuffer,     g_actorTagSRV);
            releaseTagSRV(g_actorHeadTagBuffer, g_actorHeadTagSRV);
            return false;
        }

        g_unknownTagWrapper.srv.store(g_unknownTagSRV, std::memory_order_release);
        g_unknownTagWrapper.fenceCount = 0;
        g_actorTagWrapper.srv.store(g_actorTagSRV, std::memory_order_release);
        g_actorTagWrapper.fenceCount = 0;
        g_actorHeadTagWrapper.srv.store(g_actorHeadTagSRV, std::memory_order_release);
        g_actorHeadTagWrapper.fenceCount = 0;
        g_tagWrapperResourcesReady = true;

        REX::INFO("EnsureDrawTagWrapperResources: wrappers initialized (unknownSRV={}, actorSRV={}, actorHeadSRV={})",
                  static_cast<void*>(g_unknownTagSRV),
                  static_cast<void*>(g_actorTagSRV),
                  static_cast<void*>(g_actorHeadTagSRV));
        ResetCommandBufferDrawTagWrappers();
        return true;
    }

    char* HookedBuildCommandBuffer(void* this_, void* param)
    {
        if (!param) {
            return OriginalBuildCommandBuffer(this_, param);
        }

        // Field offsets in BuildCommandBufferParam (verified in IDA):
        //   +0x00  BSGeometry*
        //   +0x14  srvCount   (DWORD)
        //   +0x38  srvSrc ptr (QWORD, points at array of CommandBufferShaderResource)
        auto* paramBytes = reinterpret_cast<char*>(param);
        auto* geom       = *reinterpret_cast<RE::BSGeometry**>(paramBytes);
        auto& srvCount   = *reinterpret_cast<std::uint32_t*>(paramBytes + 0x14);
        auto& srvSrc     = *reinterpret_cast<const CommandBufferShaderResource**>(paramBytes + 0x38);

        std::optional<ScopedShadowWork> shadowWork;
        if (IsShadowWorkTelemetryActive()) {
            ShadowTelemetry::WorkTarget shadowTarget{};
            shadowTarget.owner = this_;
            shadowTarget.geometry = static_cast<void*>(geom);
            shadowTarget.shader = this_;
            shadowTarget.srvCount = srvCount;
            shadowWork.emplace(ShadowTelemetry::WorkKind::BuildCommandBuffer, shadowTarget);
        }

        if (!EnsureDrawTagWrapperResources()) {
            return OriginalBuildCommandBuffer(this_, param);
        }

        // Classify the pass.
        WrapperTag wrapperTag = WrapperTag::kUnknown;
        if (geom) {
            std::shared_lock lock(g_actorDrawTaggedGeometryLock);
            if (g_actorDrawTaggedGeometry.contains(geom)) {
                wrapperTag = g_actorHeadDrawTaggedGeometry.contains(geom)
                                 ? WrapperTag::kActorHead
                                 : WrapperTag::kActor;
            }
        }

        const std::uint32_t origCount = srvCount;
        const auto* origSrc = srvSrc;
        if (origCount && !origSrc) {
            return OriginalBuildCommandBuffer(this_, param);
        }

        // Fall back if the pass already has more SRVs than our scratch can hold.
        // F4 shaders we've seen have 0–8 SRV records; 32 is generous headroom.
        constexpr std::size_t kMaxSRV = 32;
        if (origCount >= kMaxSRV) {
            return OriginalBuildCommandBuffer(this_, param);
        }

        std::array<CommandBufferShaderResource, kMaxSRV + 1> tempArr{};
        if (origCount && origSrc) {
            std::memcpy(tempArr.data(), origSrc, origCount * sizeof(CommandBufferShaderResource));
        }

        auto& ourRec = tempArr[origCount];
        ourRec.resourceWrapper = static_cast<void*>(GetCommandBufferDrawTagWrapper(geom, wrapperTag));
        ourRec.slot  = static_cast<std::uint8_t>(DRAWTAG_SLOT);
        ourRec.stage = 1;  // PS
        ourRec.kind  = 0;  // engine reads SRV from wrapper+0x08

        srvCount = origCount + 1;
        srvSrc   = tempArr.data();

        char* result = OriginalBuildCommandBuffer(this_, param);

        // Restore. The engine memcpy'd our extended array into the cb already,
        // so the temp stack data is no longer referenced after this point.
        srvCount = origCount;
        srvSrc   = origSrc;

        return result;
    }

    RE::NiAVObject* HookedActorLoad3D(RE::TESObjectREFR* ref, bool backgroundLoading)
    {
        auto* loaded3D = OriginalActorLoad3D(ref, backgroundLoading);
        RefreshActorDrawTaggedGeometry(ref);
        return loaded3D;
    }

    RE::NiAVObject* HookedPlayerCharacterLoad3D(RE::TESObjectREFR* ref, bool backgroundLoading)
    {
        auto* loaded3D = OriginalPlayerCharacterLoad3D(ref, backgroundLoading);
        RefreshActorDrawTaggedGeometry(ref);
        return loaded3D;
    }

    // Set3D — every "this ref now has a different NiAVObject as its 3D root".
    // Catches paths Load3D doesn't: background-load completions, Update3DModel
    // republishes, FaceGen-driven full swaps, dismemberment.
    void HookedActorSet3D(RE::TESObjectREFR* ref, RE::NiAVObject* a_object, bool a_queue3DTasks)
    {
        OriginalActorSet3D(ref, a_object, a_queue3DTasks);
        RefreshActorDrawTaggedGeometry(ref);
    }

    void HookedPlayerCharacterSet3D(RE::TESObjectREFR* ref, RE::NiAVObject* a_object, bool a_queue3DTasks)
    {
        OriginalPlayerCharacterSet3D(ref, a_object, a_queue3DTasks);
        RefreshActorDrawTaggedGeometry(ref);
    }

    // OnHeadInitialized — engine callback fired after BSFaceGenManager finishes
    // building/replacing the face node's children. Body/equipment did not
    // change, so use the head-scoped diff to avoid touching unrelated tags.
    void HookedActorOnHeadInitialized(RE::TESObjectREFR* ref)
    {
        OriginalActorOnHeadInitialized(ref);
        RefreshActorHeadDrawTaggedGeometry(ref);
    }

    void HookedPlayerCharacterOnHeadInitialized(RE::TESObjectREFR* ref)
    {
        OriginalPlayerCharacterOnHeadInitialized(ref);
        RefreshActorHeadDrawTaggedGeometry(ref);
    }

    // Actor::Update3DModel — runtime model rebuild. Most LooksMenu/SAF/
    // BodyShapeManager flows reach actor 3D through here; biped attach hooks
    // see only the parts that changed, this hook sees the whole rebuild.
    void HookedUpdate3DModel(void* middleProcess, RE::Actor* actor, bool flag)
    {
        OriginalUpdate3DModel(middleProcess, actor, flag);
        RefreshActorDrawTaggedGeometry(actor);
    }

    // Actor::Reset3D — script/console-driven reload. Often queues 3D rebuild
    // on the loader thread, so the refresh here may run before the new
    // geometry is published; Set3D / Load3D will catch the late completion.
    void HookedReset3D(RE::Actor* actor, bool a_reloadAll, std::uint32_t a_additionalFlags, bool a_queueReset, std::uint32_t a_excludeFlags)
    {
        OriginalReset3D(actor, a_reloadAll, a_additionalFlags, a_queueReset, a_excludeFlags);
        RefreshActorDrawTaggedGeometry(actor);
    }

    // Script::ModifyFaceGen::ReplaceHeadTask::Run — Papyrus-driven head swap
    // (LooksMenu apply, SexChange, etc.). The task's actor handle isn't
    // exposed via commonlibf4 yet, so this is log-only for now; once we know
    // which actor was reloaded, route it through the head refresh.
    void HookedReplaceHeadTaskRun(void* this_)
    {
        OriginalReplaceHeadTaskRun(this_);
    }
}

void ClearActorDrawTaggedGeometry_Internal()
{
    std::unique_lock lock(g_actorDrawTaggedGeometryLock);
    g_actorDrawTaggedGeometry.clear();
    g_actorHeadDrawTaggedGeometry.clear();
    g_actorDrawTaggedGeometryByRef.clear();
    g_actorHeadDrawTaggedGeometryByRef.clear();
    ResetCommandBufferDrawTagWrappers();
}

void ShutdownPassOcclusionCache_Internal()
{
    ShutdownPassOcclusionCache();
}

void ReleaseDrawTagBuffers_Internal()
{
    {
        std::unique_lock lock(g_commandBufferDrawTagWrappersLock);
        for (auto& [geometry, wrapper] : g_commandBufferDrawTagWrappers) {
            if (wrapper) {
                wrapper->srv.store(nullptr, std::memory_order_release);
            }
        }
        g_commandBufferDrawTagWrappers.clear();
    }
    g_unknownTagWrapper.srv.store(nullptr, std::memory_order_release);
    g_actorTagWrapper.srv.store(nullptr, std::memory_order_release);
    g_actorHeadTagWrapper.srv.store(nullptr, std::memory_order_release);
    g_tagWrapperResourcesReady = false;

    if (g_unknownTagSRV) {
        g_unknownTagSRV->Release();
        g_unknownTagSRV = nullptr;
    }
    if (g_unknownTagBuffer) {
        g_unknownTagBuffer->Release();
        g_unknownTagBuffer = nullptr;
    }
    if (g_actorTagSRV) {
        g_actorTagSRV->Release();
        g_actorTagSRV = nullptr;
    }
    if (g_actorTagBuffer) {
        g_actorTagBuffer->Release();
        g_actorTagBuffer = nullptr;
    }
    if (g_actorHeadTagSRV) {
        g_actorHeadTagSRV->Release();
        g_actorHeadTagSRV = nullptr;
    }
    if (g_actorHeadTagBuffer) {
        g_actorHeadTagBuffer->Release();
        g_actorHeadTagBuffer = nullptr;
    }

    for (auto& srv : g_drawTagRingSRVs) {
        if (srv) {
            srv->Release();
            srv = nullptr;
        }
    }
    for (auto& buffer : g_drawTagRingBuffers) {
        if (buffer) {
            buffer->Release();
            buffer = nullptr;
        }
    }
    g_drawTagSRV = nullptr;
    g_drawTagSRVBuffer = nullptr;
    g_drawTagRingCursor.store(0, std::memory_order_relaxed);
}

void ResetPlayerRadDamageTracking()
{
    g_lastRad = 0.0f;
    g_radDmg = 0.0f;
    g_haveRadBaseline = false;
}

static UINT PackedElementCount(std::size_t valueCount)
{
    return static_cast<UINT>(((std::max)(valueCount, std::size_t{ 1 }) + 3) / 4);
}

static void ReleaseSRVBuffer(REX::W32::ID3D11Buffer*& buffer, REX::W32::ID3D11ShaderResourceView*& srv)
{
    if (srv) {
        srv->Release();
        srv = nullptr;
    }
    if (buffer) {
        buffer->Release();
        buffer = nullptr;
    }
}

template <class T>
static bool EnsureStructuredSRV(
    REX::W32::ID3D11Device* device,
    REX::W32::ID3D11Buffer*& buffer,
    REX::W32::ID3D11ShaderResourceView*& srv,
    UINT& currentElementCount,
    UINT requiredElementCount,
    const char* label)
{
    requiredElementCount = (std::max)(requiredElementCount, 1u);
    if (buffer && srv && currentElementCount == requiredElementCount) {
        return true;
    }

    ReleaseSRVBuffer(buffer, srv);

    REX::W32::D3D11_BUFFER_DESC desc{};
    desc.usage = REX::W32::D3D11_USAGE_DYNAMIC;
    desc.byteWidth = sizeof(T) * requiredElementCount;
    desc.bindFlags = REX::W32::D3D11_BIND_SHADER_RESOURCE;
    desc.cpuAccessFlags = REX::W32::D3D11_CPU_ACCESS_WRITE;
    desc.miscFlags = REX::W32::D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    desc.structureByteStride = sizeof(T);

    HRESULT hr = device->CreateBuffer(&desc, nullptr, &buffer);
    if (FAILED(hr)) {
        REX::WARN("EnsureStructuredSRV: Failed to create {} buffer. HRESULT: 0x{:08X}", label, hr);
        currentElementCount = 0;
        return false;
    }

    REX::W32::D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.format = REX::W32::DXGI_FORMAT_UNKNOWN;
    srvDesc.viewDimension = REX::W32::D3D11_SRV_DIMENSION_BUFFER;
    srvDesc.buffer.firstElement = 0;
    srvDesc.buffer.numElements = requiredElementCount;

    hr = device->CreateShaderResourceView(buffer, &srvDesc, &srv);
    if (FAILED(hr)) {
        REX::WARN("EnsureStructuredSRV: Failed to create {} SRV. HRESULT: 0x{:08X}", label, hr);
        ReleaseSRVBuffer(buffer, srv);
        currentElementCount = 0;
        return false;
    }

    currentElementCount = requiredElementCount;
    return true;
}

template <class T>
static void UpdateStructuredSRV(REX::W32::ID3D11DeviceContext* context, REX::W32::ID3D11Buffer* buffer, const std::vector<T>& data)
{
    if (!context || !buffer || data.empty()) {
        return;
    }

    REX::W32::D3D11_MAPPED_SUBRESOURCE mapped{};
    if (SUCCEEDED(context->Map(buffer, 0, REX::W32::D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        memcpy(mapped.data, data.data(), sizeof(T) * data.size());
        context->Unmap(buffer, 0);
    }
}

static REX::W32::ID3D11ShaderResourceView* UpdateDrawTagBuffer(REX::W32::ID3D11DeviceContext* context, float materialTag, float isHead)
{
    if (!g_rendererData) {
        g_rendererData = RE::BSGraphics::GetRendererData();
        if (!g_rendererData) {
            return nullptr;
        }
    }

    auto* device = g_rendererData->device;
    if (!device) {
        return nullptr;
    }

    const UINT slot = g_drawTagRingCursor.fetch_add(1, std::memory_order_relaxed) % DRAWTAG_RING_SIZE;

    if (!g_drawTagRingBuffers[slot]) {
        REX::W32::D3D11_BUFFER_DESC desc{};
        desc.usage = REX::W32::D3D11_USAGE_DYNAMIC;
        desc.byteWidth = sizeof(DrawTagData);
        desc.bindFlags = REX::W32::D3D11_BIND_SHADER_RESOURCE;
        desc.cpuAccessFlags = REX::W32::D3D11_CPU_ACCESS_WRITE;
        desc.miscFlags = REX::W32::D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        desc.structureByteStride = sizeof(DrawTagData);

        HRESULT hr = device->CreateBuffer(&desc, nullptr, &g_drawTagRingBuffers[slot]);
        if (FAILED(hr)) {
            REX::WARN("UpdateDrawTagBuffer: Failed to create ring buffer slot {}. HRESULT: 0x{:08X}", slot, hr);
            return nullptr;
        }
    }

    if (!g_drawTagRingSRVs[slot]) {
        REX::W32::D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.format = REX::W32::DXGI_FORMAT_UNKNOWN;
        srvDesc.viewDimension = REX::W32::D3D11_SRV_DIMENSION_BUFFER;
        srvDesc.buffer.firstElement = 0;
        srvDesc.buffer.numElements = 1;

        HRESULT hr = device->CreateShaderResourceView(g_drawTagRingBuffers[slot], &srvDesc, &g_drawTagRingSRVs[slot]);
        if (FAILED(hr)) {
            REX::WARN("UpdateDrawTagBuffer: Failed to create ring SRV slot {}. HRESULT: 0x{:08X}", slot, hr);
            ReleaseSRVBuffer(g_drawTagRingBuffers[slot], g_drawTagRingSRVs[slot]);
            return nullptr;
        }
    }

    auto* ringBuffer = g_drawTagRingBuffers[slot];
    auto* ringSRV = g_drawTagRingSRVs[slot];

    // Publish the current ring entry as the "global" so paths outside
    // BindDrawTagForCurrentDraw (e.g. MyPSSetShaderResources rebind) still
    // bind a valid SRV. The value they see may be one draw stale, but our
    // per-draw bind below overrides it before the engine's draw runs.
    g_drawTagSRVBuffer = ringBuffer;
    g_drawTagSRV = ringSRV;

    if (!context) {
        return ringSRV;
    }

    // MAP_WRITE_DISCARD on this ring slot's buffer. Since each draw uses a
    // distinct buffer object, the engine's deferred command queue can hold
    // many draws in flight without their SRVs colliding.
    REX::W32::D3D11_MAPPED_SUBRESOURCE mapped{};
    if (SUCCEEDED(context->Map(ringBuffer, 0, REX::W32::D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        auto* data = static_cast<DrawTagData*>(mapped.data);
        data->materialTag = materialTag;
        data->isHead = isHead;
        data->pad1 = 0.0f;
        data->pad2 = 0.0f;
        context->Unmap(ringBuffer, 0);
    }

    g_drawTagData.materialTag = materialTag;
    g_drawTagData.isHead = isHead;
    g_drawTagData.pad1 = 0.0f;
    g_drawTagData.pad2 = 0.0f;

    return ringSRV;
}

static void BindCustomShaderResources(REX::W32::ID3D11DeviceContext* context, bool pixelShader)
{
    if (!context) {
        return;
    }

    if (g_customSRV) {
        if (pixelShader) {
            context->PSSetShaderResources(CUSTOMBUFFER_SLOT, 1, &g_customSRV);
        } else {
            context->VSSetShaderResources(CUSTOMBUFFER_SLOT, 1, &g_customSRV);
        }
    }
    if (g_drawTagSRV && g_commandBufferReplayDepth == 0) {
        if (pixelShader) {
            context->PSSetShaderResources(DRAWTAG_SLOT, 1, &g_drawTagSRV);
        } else {
            context->VSSetShaderResources(DRAWTAG_SLOT, 1, &g_drawTagSRV);
        }
    }
    if (g_modularFloatsSRV) {
        if (pixelShader) {
            context->PSSetShaderResources(MODULAR_FLOATS_SLOT, 1, &g_modularFloatsSRV);
        } else {
            context->VSSetShaderResources(MODULAR_FLOATS_SLOT, 1, &g_modularFloatsSRV);
        }
    }
    if (g_modularIntsSRV) {
        if (pixelShader) {
            context->PSSetShaderResources(MODULAR_INTS_SLOT, 1, &g_modularIntsSRV);
        } else {
            context->VSSetShaderResources(MODULAR_INTS_SLOT, 1, &g_modularIntsSRV);
        }
    }
    if (g_modularBoolsSRV) {
        if (pixelShader) {
            context->PSSetShaderResources(MODULAR_BOOLS_SLOT, 1, &g_modularBoolsSRV);
        } else {
            context->VSSetShaderResources(MODULAR_BOOLS_SLOT, 1, &g_modularBoolsSRV);
        }
    }
}

static void BindInjectedPixelShaderResources(REX::W32::ID3D11DeviceContext* context)
{
    if (!context) {
        return;
    }

    g_bindingInjectedPixelResources = true;
    BindCustomShaderResources(context, true);

    g_depthSRV = GetDepthBufferSRV_Internal();
    if (g_depthSRV) {
        context->PSSetShaderResources(DEPTHBUFFER_SLOT, 1, &g_depthSRV);
    } else if (DEBUGGING) {
        REX::WARN("BindInjectedPixelShaderResources: No depth SRV available for t{}", DEPTHBUFFER_SLOT);
    }

    // Custom pass output resources that declared srvSlot get re-bound here so
    // replacement shaders downstream (e.g. tonemap) see the latest GI texture.
    CustomPass::g_registry.BindGlobalResourceSRVs(context, /*pixelStage=*/true);

    g_bindingInjectedPixelResources = false;
}

static void BindInjectedVertexShaderResources(REX::W32::ID3D11DeviceContext* context)
{
    if (!context) {
        return;
    }

    BindCustomShaderResources(context, false);
    CustomPass::g_registry.BindGlobalResourceSRVs(context, /*pixelStage=*/false);
}

static void BindDrawTagForCurrentDraw(REX::W32::ID3D11DeviceContext* context, bool force)
{
    if (!context) {
        return;
    }

    if (!force && g_commandBufferReplayDepth != 0) {
        return;
    }

    auto* drawTagSRV = UpdateDrawTagBuffer(context, g_currentDrawTag, g_currentDrawTagIsHead);
    if (!drawTagSRV) {
        return;
    }

    g_bindingInjectedPixelResources = true;
    context->PSSetShaderResources(DRAWTAG_SLOT, 1, &drawTagSRV);
    g_bindingInjectedPixelResources = false;

    if (g_activeReplacementPixelShader) {
        BindInjectedPixelShaderResources(context);
    }

    if (DEBUGGING) {
        REX::W32::ID3D11ShaderResourceView* boundSRV = nullptr;
        context->PSGetShaderResources(DRAWTAG_SLOT, 1, &boundSRV);

        static std::uint32_t checkedDrawTagBinds = 0;
        static std::uint32_t loggedDrawTagBinds = 0;
        ++checkedDrawTagBinds;

        const bool mismatch = boundSRV != drawTagSRV;
        const bool actorTag = g_currentDrawTag >= 0.5f;
        if (loggedDrawTagBinds < 64 || mismatch || actorTag) {
            REX::INFO(
                "DrawTag bind check: draw={}, slot=t{}, tag={}, expectedSRV={}, boundSRV={}, activeReplacement={}, mismatch={}",
                checkedDrawTagBinds,
                DRAWTAG_SLOT,
                g_currentDrawTag,
                static_cast<void*>(drawTagSRV),
                static_cast<void*>(boundSRV),
                g_activeReplacementPixelShader,
                mismatch);
            ++loggedDrawTagBinds;
        }

        if (boundSRV) {
            boundSRV->Release();
        }
    }
}

static UINT FindDepthTargetIndexForDSV(REX::W32::ID3D11DepthStencilView* dsv)
{
    if (!g_rendererData || !dsv) {
        return DEPTHSTENCIL_TARGET_COUNT;
    }

    for (UINT i = 0; i < DEPTHSTENCIL_TARGET_COUNT; ++i) {
        auto& target = g_rendererData->depthStencilTargets[i];
        for (int viewIndex = 0; viewIndex < 4; ++viewIndex) {
            if (target.dsView[viewIndex] == dsv ||
                target.dsViewReadOnlyDepth[viewIndex] == dsv ||
                target.dsViewReadOnlyStencil[viewIndex] == dsv ||
                target.dsViewReadOnlyDepthStencil[viewIndex] == dsv) {
                return i;
            }
        }
    }

    return DEPTHSTENCIL_TARGET_COUNT;
}

static REX::W32::ID3D11ShaderResourceView* GetMainDepthSRV()
{
    if (!g_rendererData) {
        return nullptr;
    }

    return g_rendererData->depthStencilTargets[static_cast<UINT>(MAIN_DEPTHSTENCIL_TARGET)].srViewDepth;
}

// UI: shader-list lock snapshot (when checked we show a frozen list)
static bool g_shaderListLocked = false;
static std::vector<void*> g_lockedShaderKeys; // snapshot of map keys (original shader pointer)
// UI: show/hide replaced shaders in the list
static bool g_showReplaced = true; // default disabled
// UI: show/hide settings menu
static bool g_showSettings = false; // default disabled
static bool g_shaderSettingsSaveModalRequested = false;
static bool g_shaderSettingsSaveSucceeded = false;
static std::string g_shaderSettingsSaveMessage;

static void ApplyPassLevelOcclusionSetting(bool enabled)
{
    if (PASS_LEVEL_OCCLUSION_ON == enabled) {
        return;
    }

    PASS_LEVEL_OCCLUSION_ON = enabled;
    if (!PASS_LEVEL_OCCLUSION_ON) {
        ShutdownPassOcclusionCache_Internal();
    }
    REX::INFO("ShaderEngine Settings: PASS_LEVEL_OCCLUSION_ON set to {}", PASS_LEVEL_OCCLUSION_ON);
}

static void ApplyShadowCacheDirectionalMapSlot1Setting(bool enabled)
{
    if (SHADOW_CACHE_DIRECTIONAL_MAPSLOT1_ON == enabled) {
        return;
    }

    SHADOW_CACHE_DIRECTIONAL_MAPSLOT1_ON = enabled;
    ShadowTelemetry::ResetShadowCacheState();
    if (SHADOW_CACHE_DIRECTIONAL_MAPSLOT1_ON) {
        ShadowTelemetry::Initialize();
    }
    REX::INFO("ShaderEngine Settings: SHADOW_CACHE_DIRECTIONAL_MAPSLOT1_ON set to {}", SHADOW_CACHE_DIRECTIONAL_MAPSLOT1_ON);
}

static void SaveShaderSettingsWithFeedback()
{
    std::string shaderError;
    std::string configError;
    const bool shaderSaved = g_shaderSettings.SaveSettings(&shaderError);
    const bool configSaved = SaveShaderEngineConfig(&configError);
    g_shaderSettingsSaveSucceeded = shaderSaved && configSaved;
    if (g_shaderSettingsSaveSucceeded) {
        g_shaderSettingsSaveMessage = "Shader settings saved successfully.";
    } else {
        g_shaderSettingsSaveMessage = "Failed to save settings:";
        if (!shaderSaved) {
            g_shaderSettingsSaveMessage += "\nShader values: " + shaderError;
        }
        if (!configSaved) {
            g_shaderSettingsSaveMessage += "\nShaderEngine.ini: " + configError;
        }
    }
    g_shaderSettingsSaveModalRequested = true;
}

// UI: Compiler neon flash shader pointer
REX::W32::ID3D11PixelShader* g_flashPixelShader = nullptr;
// UI: Imgui WndProc hook variables
WNDPROC g_originalWndProc = nullptr;
extern LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
LRESULT CALLBACK ImGuiWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_KEYDOWN:
            // Detect if the key is being held down (using the repeat count bit)
            bool isPressed = (lParam & 0x40000000) == 0x0;
            if (isPressed) {
                // Compare with the stored hotkey
                if (wParam == SHADERSETTINGS_MENUHOTKEY) {
                    g_showSettings = !g_showSettings;
                    ::ShowCursor(g_showSettings);
                }
                else if (wParam == SHADERSETTINGS_SAVEHOTKEY && g_showSettings) {
                    SaveShaderSettingsWithFeedback();
                }
            }
            break;
    }

    if (g_imguiInitialized && g_showSettings){
        ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam);
        return true;
    }
    return CallWindowProc(g_originalWndProc, hwnd, msg, wParam, lParam);
}

static int32_t GetWeatherClassification(RE::TESWeather* weather)
{
    if (!weather) {
        return -1;
    }

    const auto flags = weather->weatherData[11];
    if ((flags & 1) != 0) return 0; // Pleasant
    if ((flags & 2) != 0) return 1; // Cloudy
    if ((flags & 4) != 0) return 2; // Rainy
    if ((flags & 8) != 0) return 3; // Snow

    return -1;
}

static DirectX::XMFLOAT4 TransformRotationRow(const RE::NiTransform& transform, std::size_t row)
{
    const auto& r = transform.rotate[row];
    return { r.x, r.y, r.z, r.w };
}

static DirectX::XMFLOAT4 TransformTranslationRow(const RE::NiTransform& transform)
{
    return { transform.translate.x, transform.translate.y, transform.translate.z, transform.scale };
}

static void StoreCameraTransform(
    const RE::NiTransform& transform,
    DirectX::XMFLOAT4& row0,
    DirectX::XMFLOAT4& row1,
    DirectX::XMFLOAT4& row2,
    DirectX::XMFLOAT4& row3)
{
    row0 = TransformRotationRow(transform, 0);
    row1 = TransformRotationRow(transform, 1);
    row2 = TransformRotationRow(transform, 2);
    row3 = TransformTranslationRow(transform);
}

static bool IsNodeInHierarchy(const RE::NiAVObject* object, const RE::NiAVObject* root)
{
    for (auto* current = object; current; current = current->parent) {
        if (current == root) {
            return true;
        }
    }

    return false;
}

static const RE::NiAVObject* GetPlayerCameraRoot()
{
    auto* playerCamera = RE::PlayerCamera::GetSingleton();
    if (!playerCamera) {
        return nullptr;
    }

    if (playerCamera->currentState && playerCamera->currentState->camera && playerCamera->currentState->camera->cameraRoot) {
        return playerCamera->currentState->camera->cameraRoot.get();
    }

    return playerCamera->cameraRoot.get();
}

static const RE::BSGraphics::CameraStateData* SelectGameplayCameraState(const RE::BSGraphics::State& gfxState)
{
    const auto* worldCamera = RE::Main::WorldRootCamera();
    if (worldCamera) {
        for (const auto& cachedCamera : gfxState.cameraDataCache) {
            if (cachedCamera.referenceCamera == worldCamera) {
                return std::addressof(cachedCamera);
            }
        }
    }

    const auto* playerCameraRoot = GetPlayerCameraRoot();
    if (playerCameraRoot) {
        for (const auto& cachedCamera : gfxState.cameraDataCache) {
            if (cachedCamera.referenceCamera && IsNodeInHierarchy(cachedCamera.referenceCamera, playerCameraRoot)) {
                return std::addressof(cachedCamera);
            }
        }
    }

    return std::addressof(gfxState.cameraState);
}

static bool ContainsShaderRegisterToken(const std::string& line, const std::string& token)
{
    std::size_t pos = 0;
    while ((pos = line.find(token, pos)) != std::string::npos) {
        const bool validBefore = pos == 0 || (!std::isalnum(static_cast<unsigned char>(line[pos - 1])) && line[pos - 1] != '_');
        const auto after = pos + token.size();
        const bool validAfter = after >= line.size() || !std::isdigit(static_cast<unsigned char>(line[after]));
        if (validBefore && validAfter) {
            return true;
        }
        ++pos;
    }

    return false;
}

// --- Hooks ---

// Hook IDXGISwapChain::Present (called once per frame to update the constant buffer and DEV GUI)
using Present_t = HRESULT(STDMETHODCALLTYPE*)(
    REX::W32::IDXGISwapChain* This,
    UINT SyncInterval,
    UINT Flags);
Present_t OriginalPresent = nullptr;
HRESULT STDMETHODCALLTYPE MyPresent(
    REX::W32::IDXGISwapChain* This,
    UINT SyncInterval,
    UINT Flags) {
    EnsureD3DDrawHooksPresent();

    // Light-tracker frame boundary: finalize the previous capture (if any),
    // poll Numpad * for new arms, open the next capture file. No-op when
    // DEVELOPMENT is off.
    LightTracker::Tick();
    if (CUSTOMBUFFER_ON) {
        UpdateCustomBuffer_Internal();
    }
    PassOcclusionOnFramePresent();
    // Custom-pass per-frame work: allocate resources, run any AtPresent passes,
    // ping-pong, advance frame counter. Done after the booster CB update so
    // GFXInjected[0] is fresh for any AtPresent pass.
    if (g_rendererData && g_rendererData->context) {
        CustomPass::g_registry.OnFramePresent(g_rendererData->context);
        // GPU scalar probes: dispatch each probe's 1-thread CS and read back
        // the previous frame's value. GFXInjected and modular value buffers
        // are already fresh from UpdateCustomBuffer_Internal above, so the
        // probes see the same per-frame data as regular customPass shaders.
        GpuScalar::OnFramePresent(g_rendererData->context);
    }
    // Always draw a frame if ImGui is initialized to allow hotkeys
    if (g_imguiInitialized) {
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
    }
    if (g_imguiInitialized && SHADERSETTINGS_ON && g_showSettings) {
        UIDrawShaderSettingsOverlay();
    }
    if (g_imguiInitialized && DEVGUI_ON && g_showSettings) {
        UIDrawShaderDebugOverlay();
        UIDrawCustomBufferMonitorOverlay();
        CustomPass::g_registry.DrawDebugOverlay();
    }
    if (g_imguiInitialized) {
        ImGui::Render();
        auto* context = g_rendererData ? g_rendererData->context : nullptr;
        REX::W32::ID3D11RenderTargetView* oldRTV = nullptr;
        REX::W32::ID3D11DepthStencilView* oldDSV = nullptr;
        if (context) {
            context->OMGetRenderTargets(1, &oldRTV, &oldDSV);
            auto* backBufferRTV = g_rendererData->renderWindow[0].swapChainRenderTarget.rtView;
            if (backBufferRTV) {
                context->OMSetRenderTargets(1, &backBufferRTV, nullptr);
            }
        }
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        if (context) {
            context->OMSetRenderTargets(1, &oldRTV, oldDSV);
            if (oldRTV) oldRTV->Release();
            if (oldDSV) oldDSV->Release();
        }
    }
    return OriginalPresent(This, SyncInterval, Flags);
}

// Hook for ID3D11DeviceContext::PSSetShaderResources to keep injected SRVs alive.
using PSSetShaderResources_t = void(STDMETHODCALLTYPE*)(
    REX::W32::ID3D11DeviceContext* This,
    UINT StartSlot,
    UINT NumViews,
    REX::W32::ID3D11ShaderResourceView* const* ppShaderResourceViews);
PSSetShaderResources_t OriginalPSSetShaderResources = nullptr;
void STDMETHODCALLTYPE MyPSSetShaderResources(
    REX::W32::ID3D11DeviceContext* This,
    UINT StartSlot,
    UINT NumViews,
    REX::W32::ID3D11ShaderResourceView* const* ppShaderResourceViews)
{
    OriginalPSSetShaderResources(This, StartSlot, NumViews, ppShaderResourceViews);

    if (g_activeReplacementPixelShader && !g_bindingInjectedPixelResources) {
        BindInjectedPixelShaderResources(This);
    }
}

using OMSetRenderTargets_t = void(STDMETHODCALLTYPE*)(
    REX::W32::ID3D11DeviceContext* This,
    UINT NumViews,
    REX::W32::ID3D11RenderTargetView* const* ppRenderTargetViews,
    REX::W32::ID3D11DepthStencilView* pDepthStencilView);
OMSetRenderTargets_t OriginalOMSetRenderTargets = nullptr;
void STDMETHODCALLTYPE MyOMSetRenderTargets(
    REX::W32::ID3D11DeviceContext* This,
    UINT NumViews,
    REX::W32::ID3D11RenderTargetView* const* ppRenderTargetViews,
    REX::W32::ID3D11DepthStencilView* pDepthStencilView)
{
    OriginalOMSetRenderTargets(This, NumViews, ppRenderTargetViews, pDepthStencilView);

    bool hasRT = false;
    if (ppRenderTargetViews) {
        for (UINT i = 0; i < NumViews; ++i) {
            if (ppRenderTargetViews[i]) {
                hasRT = true;
                break;
            }
        }
    }

    const UINT depthTarget = FindDepthTargetIndexForDSV(pDepthStencilView);
    g_currentDepthTargetIndex.store(depthTarget, std::memory_order_relaxed);
    g_currentRenderTargetCount.store(NumViews, std::memory_order_relaxed);
    g_currentHasRenderTarget.store(hasRT, std::memory_order_relaxed);

    if (depthTarget == static_cast<UINT>(MAIN_DEPTHSTENCIL_TARGET)) {
        g_lastSceneDepthSRV = GetMainDepthSRV();
    }
}

using ClearDepthStencilView_t = void(STDMETHODCALLTYPE*)(
    REX::W32::ID3D11DeviceContext* This,
    REX::W32::ID3D11DepthStencilView* pDepthStencilView,
    UINT ClearFlags,
    FLOAT Depth,
    UINT8 Stencil);
ClearDepthStencilView_t OriginalClearDepthStencilView = nullptr;
void STDMETHODCALLTYPE MyClearDepthStencilView(
    REX::W32::ID3D11DeviceContext* This,
    REX::W32::ID3D11DepthStencilView* pDepthStencilView,
    UINT ClearFlags,
    FLOAT Depth,
    UINT8 Stencil)
{
    if (ShadowTelemetry::HandleShadowCacheClearDepthStencilView(
            This, pDepthStencilView, ClearFlags, Depth, Stencil)) {
        return;
    }

    OriginalClearDepthStencilView(This, pDepthStencilView, ClearFlags, Depth, Stencil);
}

using DrawIndexed_t = void(STDMETHODCALLTYPE*)(
    REX::W32::ID3D11DeviceContext* This,
    UINT IndexCount,
    UINT StartIndexLocation,
    INT BaseVertexLocation);
DrawIndexed_t OriginalDrawIndexed = nullptr;
void STDMETHODCALLTYPE MyDrawIndexed(
    REX::W32::ID3D11DeviceContext* This,
    UINT IndexCount,
    UINT StartIndexLocation,
    INT BaseVertexLocation)
{
    PhaseTelemetry::OnD3DDraw();
    ShadowTelemetry::OnD3DDraw();
    BindDrawTagForCurrentDraw(This);
    // BeforeDrawForMatchedDef custom passes fire here, when the engine has
    // fully set up the pipeline for the upcoming draw. State is fresh.
    if (CustomPass::g_registry.OnBeforeDraw(This, "d3d11-DrawIndexed") && g_activeReplacementPixelShader) {
        BindInjectedPixelShaderResources(This);
    }
    OriginalDrawIndexed(This, IndexCount, StartIndexLocation, BaseVertexLocation);
}

using Draw_t = void(STDMETHODCALLTYPE*)(
    REX::W32::ID3D11DeviceContext* This,
    UINT VertexCount,
    UINT StartVertexLocation);
Draw_t OriginalDraw = nullptr;
void STDMETHODCALLTYPE MyDraw(
    REX::W32::ID3D11DeviceContext* This,
    UINT VertexCount,
    UINT StartVertexLocation)
{
    PhaseTelemetry::OnD3DDraw();
    ShadowTelemetry::OnD3DDraw();
    BindDrawTagForCurrentDraw(This);
    if (CustomPass::g_registry.OnBeforeDraw(This, "d3d11-Draw") && g_activeReplacementPixelShader) {
        BindInjectedPixelShaderResources(This);
    }
    OriginalDraw(This, VertexCount, StartVertexLocation);
}

using DrawIndexedInstanced_t = void(STDMETHODCALLTYPE*)(
    REX::W32::ID3D11DeviceContext* This,
    UINT IndexCountPerInstance,
    UINT InstanceCount,
    UINT StartIndexLocation,
    INT BaseVertexLocation,
    UINT StartInstanceLocation);
DrawIndexedInstanced_t OriginalDrawIndexedInstanced = nullptr;
void STDMETHODCALLTYPE MyDrawIndexedInstanced(
    REX::W32::ID3D11DeviceContext* This,
    UINT IndexCountPerInstance,
    UINT InstanceCount,
    UINT StartIndexLocation,
    INT BaseVertexLocation,
    UINT StartInstanceLocation)
{
    PhaseTelemetry::OnD3DDraw();
    ShadowTelemetry::OnD3DDraw();
    BindDrawTagForCurrentDraw(This);
    if (CustomPass::g_registry.OnBeforeDraw(This, "d3d11-DrawIndexedInstanced") && g_activeReplacementPixelShader) {
        BindInjectedPixelShaderResources(This);
    }
    OriginalDrawIndexedInstanced(This, IndexCountPerInstance, InstanceCount, StartIndexLocation, BaseVertexLocation, StartInstanceLocation);
}

using DrawInstanced_t = void(STDMETHODCALLTYPE*)(
    REX::W32::ID3D11DeviceContext* This,
    UINT VertexCountPerInstance,
    UINT InstanceCount,
    UINT StartVertexLocation,
    UINT StartInstanceLocation);
DrawInstanced_t OriginalDrawInstanced = nullptr;
void STDMETHODCALLTYPE MyDrawInstanced(
    REX::W32::ID3D11DeviceContext* This,
    UINT VertexCountPerInstance,
    UINT InstanceCount,
    UINT StartVertexLocation,
    UINT StartInstanceLocation)
{
    PhaseTelemetry::OnD3DDraw();
    ShadowTelemetry::OnD3DDraw();
    BindDrawTagForCurrentDraw(This);
    if (CustomPass::g_registry.OnBeforeDraw(This, "d3d11-DrawInstanced") && g_activeReplacementPixelShader) {
        BindInjectedPixelShaderResources(This);
    }
    OriginalDrawInstanced(This, VertexCountPerInstance, InstanceCount, StartVertexLocation, StartInstanceLocation);
}

namespace
{
    bool PatchVTableSlot(void** slot, void* hook)
    {
        DWORD oldProtect = 0;
        if (!VirtualProtect(slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) {
            return false;
        }

        *slot = hook;

        DWORD ignored = 0;
        VirtualProtect(slot, sizeof(void*), oldProtect, &ignored);
        return true;
    }

    template <class OriginalFn>
    void EnsureD3DDrawHookSlot(
        void** vtable,
        std::size_t index,
        void* hook,
        OriginalFn& original)
    {
        void** slot = &vtable[index];
        void* current = *slot;
        if (current == hook) {
            return;
        }

        if (current != reinterpret_cast<void*>(original)) {
            return;
        }

        PatchVTableSlot(slot, hook);
    }
}

static void EnsureD3DDrawHooksPresent()
{
    if (!g_rendererData) {
        g_rendererData = RE::BSGraphics::GetRendererData();
    }
    if (!g_rendererData || !g_rendererData->context) {
        return;
    }

    auto* vtable = *reinterpret_cast<void***>(g_rendererData->context);
    if (!vtable) {
        return;
    }

    EnsureD3DDrawHookSlot(vtable, 12, reinterpret_cast<void*>(&MyDrawIndexed), OriginalDrawIndexed);
    EnsureD3DDrawHookSlot(vtable, 13, reinterpret_cast<void*>(&MyDraw), OriginalDraw);
    EnsureD3DDrawHookSlot(vtable, 20, reinterpret_cast<void*>(&MyDrawIndexedInstanced), OriginalDrawIndexedInstanced);
    EnsureD3DDrawHookSlot(vtable, 21, reinterpret_cast<void*>(&MyDrawInstanced), OriginalDrawInstanced);
    EnsureD3DDrawHookSlot(vtable, 53, reinterpret_cast<void*>(&MyClearDepthStencilView), OriginalClearDepthStencilView);
}

// Hook for ID3D11DeviceContext::PSSetShader to replace the Pixel shader
using PSSetShader_t = void(STDMETHODCALLTYPE*)(
    REX::W32::ID3D11DeviceContext* This,
    REX::W32::ID3D11PixelShader* pPixelShader,
    REX::W32::ID3D11ClassInstance* const* ppClassInstances,
    UINT NumClassInstances);
PSSetShader_t OriginalPSSetShader = nullptr;
void STDMETHODCALLTYPE MyPSSetShader(
    REX::W32::ID3D11DeviceContext* This,
    REX::W32::ID3D11PixelShader* pPixelShader,
    REX::W32::ID3D11ClassInstance* const* ppClassInstances,
    UINT NumClassInstances) {
    bool usingReplacementPixelShader = false;
    g_currentOriginalPixelShader.store(pPixelShader, std::memory_order_release);

    // Light-tracker capture at PS-bind time. Engine sets RTVs / blend /
    // scissor / cb2 / SRVs before PSSetShader, so all the per-pass state
    // we want is live at this moment. Cheap no-op when not capturing.
    LightTracker::OnPSBind(This, pPixelShader);
    // Trigger any customPass blocks attached to this original shader. The
    // pass runs immediately *before* we forward to OriginalPSSetShader so the
    // engine state we save/restore is the state the engine just established
    // for the upcoming shader. If a pass fired we must re-bind injected
    // resources for the engine shader's draw afterwards.
    bool customPassFired = false;
    if (pPixelShader && !g_isCreatingReplacementShader && !g_bindingInjectedPixelResources) {
        customPassFired = CustomPass::g_registry.OnBeforeShaderBound(This, pPixelShader);
    }
    if (pPixelShader) {
        // Check if this shader is matched with a replacement shader in our DB
        if (g_ShaderDB.IsEntryMatched(pPixelShader)) {
            g_ShaderDB.SetEntryRecentlyUsed(pPixelShader, true); // Mark this shader as recently used for tracking
            auto* matchedDefinition = g_ShaderDB.GetMatchedDefinition(pPixelShader);
            // If the HLSL watcher flagged a disk change for this definition,
            // drop the cached compiled shader + replacement pointers BEFORE we
            // read them below. Done here on the render thread so the D3D11
            // Release happens on the immediate-context-owning thread.
            MaybeApplyHlslHotReload_Internal(matchedDefinition);
            auto* replacementPixelShader = g_ShaderDB.GetReplacementShader(pPixelShader);
            // Get the replacement shader for this original shader
            if (replacementPixelShader) {
                // Replace the shader with our replacement shader
                if (DEBUGGING) {
                    REX::INFO("MyPSSetShader: Replacing pixel shader with matched replacement for definition '{}'", matchedDefinition ? matchedDefinition->id : "Unknown");
                }
                pPixelShader = replacementPixelShader;
                usingReplacementPixelShader = true;
            } else {
                if (matchedDefinition && !matchedDefinition->buggy) {
                    if (DEBUGGING)
                        REX::INFO("MyPSSetShader: Shader is matched but no replacement shader found, trying to compile...");
                    if (CompileShader_Internal(matchedDefinition)) {
                        g_ShaderDB.SetReplacementShader(pPixelShader, matchedDefinition->loadedPixelShader);
                        if (DEBUGGING)
                            REX::INFO("MyPSSetShader: Compiled replacement shader for definition '{}'", matchedDefinition->id);
                        // Replace the shader with our custom one
                        if (DEBUGGING) {
                            REX::INFO("MyPSSetShader: Replacing pixel shader with newly compiled replacement for definition '{}'", matchedDefinition->id);
                        }
                        pPixelShader = g_ShaderDB.GetReplacementShader(pPixelShader);
                        usingReplacementPixelShader = pPixelShader != nullptr;
                    } else {
                        REX::WARN("MyPSSetShader: Failed to compile replacement shader for definition '{}'", matchedDefinition->id);
                        matchedDefinition->buggy = true; // Mark as failed to compile
                    }
                }
            }
        }
    }
    // Call original function with either the original or replacement shader
    OriginalPSSetShader(This, pPixelShader, ppClassInstances, NumClassInstances);
    g_activeReplacementPixelShader = usingReplacementPixelShader;
    // If a customPass fired, the engine's next draw still expects the injected
    // resource set we publish for replacement shaders (depth, GFXInjected, etc.)
    // to be present on its slots. The pass already re-binds them, but there is
    // no harm in publishing them again here when running a replacement.
    if (customPassFired && usingReplacementPixelShader) {
        BindInjectedPixelShaderResources(This);
    }
}

// Hook for ID3D11DeviceContext::VSSetShader to replace the Vertex shader
using VSSetShader_t = void(STDMETHODCALLTYPE*)(
    REX::W32::ID3D11DeviceContext* This,
    REX::W32::ID3D11VertexShader* pVertexShader,
    REX::W32::ID3D11ClassInstance* const* ppClassInstances,
    UINT NumClassInstances);
VSSetShader_t OriginalVSSetShader = nullptr;
void STDMETHODCALLTYPE MyVSSetShader(
    REX::W32::ID3D11DeviceContext* This,
    REX::W32::ID3D11VertexShader* pVertexShader,
    REX::W32::ID3D11ClassInstance* const* ppClassInstances,
    UINT NumClassInstances) {
        if (pVertexShader) {
        // Check if this shader is matched with a replacement shader in our DB
        if (g_ShaderDB.IsEntryMatched(pVertexShader)) {
            g_ShaderDB.SetEntryRecentlyUsed(pVertexShader, true); // Mark this shader as recently used for tracking
            auto* matchedDefinition = g_ShaderDB.GetMatchedDefinition(pVertexShader);
            // See MyPSSetShader for rationale: we evict cached compiled state
            // here, before reading the replacement, so the D3D11 Release runs
            // on the render thread.
            MaybeApplyHlslHotReload_Internal(matchedDefinition);
            auto* replacementVertexShader = g_ShaderDB.GetReplacementShader(pVertexShader);
            // Get the replacement shader for this original shader
            if (replacementVertexShader) {
                // Replace the shader with our replacement shader
                if (DEBUGGING) {
                    REX::INFO("MyVSSetShader: Replacing vertex shader with matched replacement for definition '{}'", matchedDefinition ? matchedDefinition->id : "Unknown");
                }
                pVertexShader = replacementVertexShader;
                // Set our custom SRVs for replacement shaders to use in their shader code
                BindInjectedVertexShaderResources(This);
            } else {
                if (matchedDefinition && !matchedDefinition->buggy) {
                    if (DEBUGGING)
                        REX::INFO("MyVSSetShader: Shader is matched but no replacement shader found, trying to compile...");
                    if (CompileShader_Internal(matchedDefinition)) {
                        g_ShaderDB.SetReplacementShader(pVertexShader, matchedDefinition->loadedVertexShader);
                        if (DEBUGGING)
                            REX::INFO("MyVSSetShader: Compiled replacement shader for definition '{}'", matchedDefinition->id);
                        // Replace the shader with our custom one
                        if (DEBUGGING) {
                            REX::INFO("MyVSSetShader: Replacing vertex shader with newly compiled replacement for definition '{}'", matchedDefinition->id);
                        }
                        pVertexShader = g_ShaderDB.GetReplacementShader(pVertexShader);
                        // Set our custom SRVs for replacement shaders to use in their shader code
                        BindInjectedVertexShaderResources(This);
                    } else {
                        REX::WARN("MyVSSetShader: Failed to compile replacement shader for definition '{}'", matchedDefinition->id);
                        matchedDefinition->buggy = true; // Mark as failed to compile
                    }
                }
            }
        }
    }
    // Call original function with either the original or replacement shader
    OriginalVSSetShader(This, pVertexShader, ppClassInstances, NumClassInstances);
}

// --- SHADER CREATION ---
// Hook for ID3D11Device::CreatePixelShader to analyze and store the shader in the ShaderDB
using CreatePixelShader_t = HRESULT(STDMETHODCALLTYPE*)(
    REX::W32::ID3D11Device* This,
    const void* pShaderBytecode,
    SIZE_T BytecodeLength,
    REX::W32::ID3D11ClassLinkage* pClassLinkage,
    REX::W32::ID3D11PixelShader** ppPixelShader);
CreatePixelShader_t OriginalCreatePixelShader = nullptr;
HRESULT STDMETHODCALLTYPE MyCreatePixelShader(
    REX::W32::ID3D11Device* This,
    const void* pShaderBytecode,
    SIZE_T BytecodeLength,
    REX::W32::ID3D11ClassLinkage* pClassLinkage,
    REX::W32::ID3D11PixelShader** ppPixelShader) {
    if (g_isCreatingReplacementShader) {
        // If we're in the process of creating a replacement shader, skip all processing to avoid infinite recursion and just call the original function
        return OriginalCreatePixelShader(This, pShaderBytecode, BytecodeLength, pClassLinkage, ppPixelShader);
    }
    HRESULT hr = OriginalCreatePixelShader(This, pShaderBytecode, BytecodeLength, pClassLinkage, ppPixelShader);
    if (REX::W32::SUCCESS(hr) && ppPixelShader && *ppPixelShader) {
        // Store bytecode for later dumping
        std::vector<uint8_t> bytecode(BytecodeLength);
        memcpy(bytecode.data(), pShaderBytecode, BytecodeLength);
        auto hash = static_cast<std::uint32_t>(std::hash<std::string_view>{}(std::string_view((char*)bytecode.data(), bytecode.size())));
        // Check if we've already analyzed this shader
        if (g_ShaderDB.HasEntry(*ppPixelShader)) {
            // Already in database, skip re-analysis
            return hr;
        }
        // Get the ShaderDB entry for this shader, which will analyze the shader and find a matching definition if it exists
        ShaderDBEntry entry = AnalyzeShader_Internal(*ppPixelShader, nullptr, std::move(bytecode), BytecodeLength);
        // Create a shader DB entry for this shader
        g_ShaderDB.AddShaderEntry(std::move(entry));
    }
    return hr;
}

// Hook for ID3D11Device::CreateVertexShader to analyze and store the shader in the ShaderDB
using CreateVertexShader_t = HRESULT(STDMETHODCALLTYPE*)(
    REX::W32::ID3D11Device* This,
    const void* pShaderBytecode,
    SIZE_T BytecodeLength,
    REX::W32::ID3D11ClassLinkage* pClassLinkage,
    REX::W32::ID3D11VertexShader** ppVertexShader);
CreateVertexShader_t OriginalCreateVertexShader = nullptr;
HRESULT STDMETHODCALLTYPE MyCreateVertexShader(
    REX::W32::ID3D11Device* This,
    const void* pShaderBytecode,
    SIZE_T BytecodeLength,
    REX::W32::ID3D11ClassLinkage* pClassLinkage,
    REX::W32::ID3D11VertexShader** ppVertexShader) {
    // For simplicity, we won't analyze vertex shaders for matching and replacement in this example, but we will track them for dumping if needed
    HRESULT hr = OriginalCreateVertexShader(This, pShaderBytecode, BytecodeLength, pClassLinkage, ppVertexShader);
    if (REX::W32::SUCCESS(hr) && ppVertexShader && *ppVertexShader) {
        // Store bytecode for later dumping
        std::vector<uint8_t> bytecode(BytecodeLength);
        memcpy(bytecode.data(), pShaderBytecode, BytecodeLength);
        auto hash = static_cast<std::uint32_t>(std::hash<std::string_view>{}(std::string_view((char*)bytecode.data(), bytecode.size())));
        // Check if we've already analyzed this shader
        if (g_ShaderDB.HasEntry(*ppVertexShader)) {
            // Already in database, skip re-analysis
            return hr;
        }
        // Get the ShaderDB entry for this shader, which will analyze the shader and find a matching definition if it exists
        ShaderDBEntry entry = AnalyzeShader_Internal(nullptr, *ppVertexShader, std::move(bytecode), BytecodeLength);
        // Create a shader DB entry for this shader
        g_ShaderDB.AddShaderEntry(std::move(entry));
    }
    return hr;
}

// -- Structs ---

// --- Functions ---

// Analyze the shader bytecode to extract info for matching and potential replacement.
ShaderDBEntry AnalyzeShader_Internal(REX::W32::ID3D11PixelShader* pixelShader, REX::W32::ID3D11VertexShader* vertexShader, std::vector<uint8_t> bytecode, SIZE_T BytecodeLength) {
    ShaderDBEntry entry{};
    if (!pixelShader && !vertexShader || bytecode.empty()) return entry;
    void* shader = nullptr;
    if (pixelShader) {
        shader = static_cast<void*>(pixelShader);
    } else if (vertexShader) {
        shader = static_cast<void*>(vertexShader);
    }
    entry.originalShader = shader;
    if (pixelShader) {
        entry.type = ShaderType::Pixel;
    } else if (vertexShader) {
        entry.type = ShaderType::Vertex;
    }
    entry.bytecode = std::move(bytecode);
    auto hash = static_cast<std::uint32_t>(std::hash<std::string_view>{}(std::string_view((char*)entry.bytecode.data(), entry.bytecode.size())));
    entry.hash = hash;
    entry.size = BytecodeLength;
    // Analyze the shader entry
    if (ReflectShader_Internal(entry)) {
        if (DEBUGGING) {
            REX::INFO("AnalyzeShader_Internal: Shader reflection of {} Shader successful. Hash={:08X}, Size={}", entry.type == ShaderType::Vertex ? "Vertex" : "Pixel", hash, entry.size);
        }
    } else {
        REX::WARN("AnalyzeShader_Internal: Shader reflection failed for {} Shader with hash {:08X} and size {} bytes.", entry.type == ShaderType::Vertex ? "Vertex" : "Pixel", hash, entry.size);
    }
    // Validate entry before returning
    if (!entry.hash || !entry.size) {
        entry.SetValid(false);
        return entry; // Invalid entry, do not add to ShaderDB
    } else {
        entry.SetValid(true);
    }
    // Compare to shader definitions in our INI and find a match based on filters
    // If we find a match, we can store the compiled replacement shader in the entry for quick access during rendering.
    std::shared_lock lock(g_shaderDefinitions.mutex);
    for (ShaderDefinition* def : g_shaderDefinitions.definitions) {
        if (def->active && DoesEntryMatchDefinition_Internal(entry, def)) {
            entry.SetMatched(true);
            entry.matchedDefinition = def; // Store the matched definition for later use during shader compilation
            if (DEVELOPMENT && def->log) {
                REX::INFO("AnalyzeShader_Internal: ------------------------------------------------");
                    REX::INFO("RematchAllShaders_Internal: Found matching shader definition '{}' for {} shader with ShaderUID '{}'.", def->id, entry.type == ShaderType::Vertex ? "Vertex" : "Pixel", entry.shaderUID);
                    REX::INFO("RematchAllShaders_Internal: Shader Hash: {:08X}", entry.hash);
                    REX::INFO("RematchAllShaders_Internal: Shader Size: {} bytes", entry.size);
                    REX::INFO("RematchAllShaders_Internal: Shader ASM Hash: {:08X}", entry.asmHash);
                REX::INFO(" - Shader CB Sizes: {},{},{},{},{},{},{},{},{},{},{},{},{},{}", 
                    entry.expectedCBSizes[0], entry.expectedCBSizes[1], entry.expectedCBSizes[2], entry.expectedCBSizes[3],
                    entry.expectedCBSizes[4], entry.expectedCBSizes[5], entry.expectedCBSizes[6], entry.expectedCBSizes[7],
                    entry.expectedCBSizes[8], entry.expectedCBSizes[9], entry.expectedCBSizes[10], entry.expectedCBSizes[11],
                    entry.expectedCBSizes[12], entry.expectedCBSizes[13]);
                REX::INFO(" - Shader Texture Register Slots: {}", entry.textureSlots.empty() ? "None" : "");
                for (const auto& slot : entry.textureSlots) {
                    REX::INFO("   - Slot: t{}", slot);
                }
                REX::INFO(" - Shader Texture Dimensions: {}", entry.textureDimensions.empty() ? "None" : "");
                for (const auto& [dimension, slot] : entry.textureDimensions) {
                    REX::INFO("   - Dimension: {}, Slot: t{}", dimension, slot);
                }
                REX::INFO(" - Shader Texture Usage Bitmask: 0x{:08X}", entry.textureSlotMask);
                REX::INFO(" - Shader Texture Dimension Bitmask: 0x{:08X}", entry.textureDimensionMask);
                REX::INFO(" - Shader Input Texture Count: {}", entry.inputTextureCount != -1 ? std::to_string(entry.inputTextureCount) : "X");
                REX::INFO(" - Shader Input Count: {}", entry.inputCount != -1 ? std::to_string(entry.inputCount) : "X");
                REX::INFO(" - Shader Input Mask: 0x{:08X}", entry.inputMask);
                REX::INFO(" - Shader Output Count: {}", entry.outputCount != -1 ? std::to_string(entry.outputCount) : "X");
                REX::INFO(" - Shader Output Mask: 0x{:08X}", entry.outputMask);
                REX::INFO("AnalyzeShader_Internal: ------------------------------------------------");
            }
            if (DEVELOPMENT && def->dump && !entry.IsDumped()) {
                DumpOriginalShader_Internal(entry, def);
                entry.SetDumped(true);
            }
            break; // Stop checking after the first match to keep priorities based on order in INI
        }
    }
    return entry;
}

// Compile the HLSL shaders that were defined in the INI for each shader
bool CompileShader_Internal(ShaderDefinition* def) {
    if (!def) return false;
    // Lazy-init in case a def slipped through without one (defensive).
    if (!def->compileMutex) def->compileMutex = std::make_unique<std::mutex>();
    // Per-def serialization: if the precompile worker is mid-flight on this
    // def the render thread blocks here briefly, then sees loadedPixelShader
    // already populated and short-circuits. Same in reverse. The lock spans
    // the cache lookup + D3DCompile + Create*Shader so observers always see
    // a consistent (compiledShader, loadedPixelShader/VertexShader) pair.
    std::lock_guard compileLock(*def->compileMutex);
    // Check if already compiled (re-checked under the lock so a concurrent
    // compile that finished while we were waiting is observed correctly).
    if (def->loadedPixelShader || def->loadedVertexShader) {
        if (DEBUGGING)
            REX::INFO("CompileShader_Internal: Shader '{}' is already compiled. Skipping compilation.", def->id);
        return true;
    }
    // Check the file exists
    std::ifstream shaderFile(def->shaderFile, std::ios::binary);
    if (!shaderFile.good()) {
        REX::WARN("CompileShader_Internal: Shader file not found: {}", def->shaderFile.string());
        return false;
    }
    // Build the common shader header with dynamic shader settings values injected as defines
    std::string shaderHeader = GetCommonShaderHeaderHLSLTop();
    // Struct is already closed in commonShaderHeaderHLSLTop.
    // Add the bottom (declares GFXInjected) before the #defines that reference it.
    shaderHeader += GetCommonShaderHeaderHLSLBottom();
    // Inject #define named accessors; each maps a friendly name to the correct array slot.
    // bufferIndex is packed into float4/int4/uint4 settings buffers.
    shaderHeader += "// shader settings named accessors\n";
    for (auto* sValue : g_shaderSettings.GetFloatShaderValues()) {
        shaderHeader += std::format("#define {} GFXModularFloats[{}]{}\n", sValue->id, sValue->bufferIndex / 4, std::array<std::string_view, 4>{ ".x", ".y", ".z", ".w" }[sValue->bufferIndex % 4]);
    }
    for (auto* sValue : g_shaderSettings.GetIntShaderValues()) {
        shaderHeader += std::format("#define {} GFXModularInts[{}]{}\n", sValue->id, sValue->bufferIndex / 4, std::array<std::string_view, 4>{ ".x", ".y", ".z", ".w" }[sValue->bufferIndex % 4]);
    }
    for (auto* sValue : g_shaderSettings.GetBoolShaderValues()) {
        shaderHeader += std::format("#define {} (GFXModularBools[{}]{} != 0)\n", sValue->id, sValue->bufferIndex / 4, std::array<std::string_view, 4>{ ".x", ".y", ".z", ".w" }[sValue->bufferIndex % 4]);
    }
    shaderHeader += "\n";
    // Read the shader source from file and prepend the common header
    std::string shaderBody((std::istreambuf_iterator<char>(shaderFile)), std::istreambuf_iterator<char>());
    const bool sourceMentionsDrawTag = shaderBody.find("GFXDrawTag") != std::string::npos;
    std::string shaderSource = shaderHeader;
    shaderSource += shaderBody;
    shaderFile.close();
    std::string targetProfile = "ps_5_0"; // Default to pixel shader model 5.0
    if (def->type == ShaderType::Vertex) {
        targetProfile = "vs_5_0";
    } else if (def->type == ShaderType::Pixel) {
        targetProfile = "ps_5_0";
    } else {
        REX::WARN("CompileShader_Internal: Invalid shader type for shader '{}'. Defaulting to pixel shader.", def->id);
    }
    if (!g_rendererData || !g_rendererData->device) {
        REX::WARN("CompileShader_Internal: Renderer device not available. Cannot compile shader '{}'", def->id);
        return false;
    }
    REX::W32::ID3D11Device* device = g_rendererData->device;

    // Try the on-disk compiled-shader cache first. The key is computed from
    // the fully assembled source plus profile/entry/flags plus include-dir
    // contents — any input change naturally produces a miss.
    constexpr uint32_t kCompileFlags = D3DCOMPILE_OPTIMIZATION_LEVEL3;
    constexpr std::string_view kEntry = "main";
    const std::string cacheKey = ShaderCache::ComputeKey({
        .assembledSource = shaderSource,
        .profile         = targetProfile,
        .entry           = kEntry,
        .flags           = kCompileFlags,
    });
    ID3DBlob* cachedBlob = nullptr;
    if (ShaderCache::TryLoad(cacheKey, &cachedBlob)) {
        def->compiledShader = cachedBlob;
        REX::INFO("CompileShader_Internal: cache HIT for '{}' ({} bytes)", def->id, def->compiledShader->GetBufferSize());
    } else {
        ID3DBlob* errorBlob = nullptr;
        ID3DInclude* includeHandler = new ShaderIncludeHandler(); // Custom include handler to resolve #include directives relative to the plugin directory
        HRESULT hr = D3DCompile(
            shaderSource.c_str(),
            shaderSource.size(),
            def->id.c_str(),
            nullptr,
            includeHandler,
            kEntry.data(),
            targetProfile.c_str(),
            kCompileFlags,
            0,
            &def->compiledShader,
            &errorBlob
        );
        delete includeHandler;
        if (!REX::W32::SUCCESS(hr)) {
            if (errorBlob) {
                REX::WARN("CompileShader_Internal: Shader compilation failed: {}", static_cast<const char*>(errorBlob->GetBufferPointer()));
                errorBlob->Release();
            }
            return false;
        }
        if (errorBlob) errorBlob->Release();
        // Only cache on success — failed compiles must not poison future runs.
        ShaderCache::Store(cacheKey, def->compiledShader);
        REX::INFO("CompileShader_Internal: {} compiled successfully! Bytecode size: {} bytes",
                  def->shaderFile.string(), def->compiledShader->GetBufferSize());
    }
    HRESULT hr = S_OK;
    // Set flag to prevent hook from analyzing the shader
    g_isCreatingReplacementShader = true;
    // Create the actual shader object from the compiled bytecode
    if (def->type == ShaderType::Vertex) {
        hr = device->CreateVertexShader(
            def->compiledShader->GetBufferPointer(),
            def->compiledShader->GetBufferSize(),
            nullptr,
            &def->loadedVertexShader
        );
    } else {
        hr = device->CreatePixelShader(
            def->compiledShader->GetBufferPointer(),
            def->compiledShader->GetBufferSize(),
            nullptr,
            &def->loadedPixelShader
        );
    }
    // Reset flag after creation
    g_isCreatingReplacementShader = false;
    if (!REX::W32::SUCCESS(hr)) {
        if (def->type == ShaderType::Vertex) {
            REX::WARN("CompileShader_Internal: Failed to create vertex shader for '{}'", def->id);
        } else {
            REX::WARN("CompileShader_Internal: Failed to create pixel shader for '{}'", def->id);
        }
        return false;
    }
    return true;
}

// All provided requirements must match for the function to return true.
bool DoesEntryMatchDefinition_Internal(ShaderDBEntry const& entry, ShaderDefinition* def) {
    // Basic checks
    if (!entry.valid) return false;
    if (!def) return false;
    if (!def->active) return false;
    // Check shader type
    if (def->type == ShaderType::Pixel && entry.type != ShaderType::Pixel) return false;
    if (def->type == ShaderType::Vertex && entry.type != ShaderType::Vertex) return false;
    // Check ShaderUID[s] if specified
    if (!def->shaderUID.empty()) {
        bool uidMatch = false;
        for (const auto& uid : def->shaderUID) {
            if (ToLower(entry.shaderUID) == ToLower(uid)) {
                uidMatch = true;
                break;
            }
        }
        if (!uidMatch) {
            return false;
        }
    }
    // Check hash[es] if specified
    if (def->hash.size() != 0) {
        bool hashMatch = false;
        for (const auto& hash : def->hash) {
            if (entry.hash == hash) {
                hashMatch = true;
                break;
            }
        }
        if (!hashMatch) {
            return false;
        }
    }
    // Check ASM hash if specified
    if (def->asmHash.size() != 0) {
        bool asmHashMatch = false;
        for (const auto& asmHash : def->asmHash) {
            if (entry.asmHash == asmHash) {
                asmHashMatch = true;
                break;
            }
        }
        if (!asmHashMatch) {
            return false;
        }
    }
    // Check size requirement if specified
    if (!def->sizeRequirements.empty()) {
        for (const auto& req : def->sizeRequirements) {
            if (req.op == SizeOp::Equal && entry.size != req.value) return false;
            if (req.op == SizeOp::Greater && entry.size <= req.value) return false;
            if (req.op == SizeOp::Less && entry.size >= req.value) return false;
        }
    }
    // Check constant buffer sizes
    for (const auto& [size, slot] : def->bufferSizes) {
        // Handle size@ without slot (any slot)
        if (slot < 0) {
            bool anySlotMatches = false;
            for (int i = 0; i < 14; i++) {
                if (entry.expectedCBSizes[i] == size) {
                    anySlotMatches = true;
                    break;
                }
            }
            if (!anySlotMatches) return false;
        // Handle size@slot (specific slot)
        } else if (slot >= 0 && slot < 14) {
        if (entry.expectedCBSizes[slot] != size)
            return false;
        }
    }
    // Check texture slots
    if (def->textureSlotMask != 0 && ((entry.textureSlotMask & def->textureSlotMask) != def->textureSlotMask))
        return false;
    // Check texture dimensions
    if (def->textureDimensionMask != 0 && ((entry.textureDimensionMask & def->textureDimensionMask) != def->textureDimensionMask))
        return false;
    // Check input texture count if specified
    if (!def->inputTextureCountRequirements.empty()) {
        for (const auto& req : def->inputTextureCountRequirements) {
            if (req.op == SizeOp::Equal && entry.inputTextureCount != req.value) return false;
            if (req.op == SizeOp::Greater && entry.inputTextureCount <= req.value) return false;
            if (req.op == SizeOp::Less && entry.inputTextureCount >= req.value) return false;
        }
    }
    // Check input count if specified
    if (!def->inputCountRequirements.empty()) {
        for (const auto& req : def->inputCountRequirements) {
            if (req.op == SizeOp::Equal && entry.inputCount != req.value) return false;
            if (req.op == SizeOp::Greater && entry.inputCount <= req.value) return false;
            if (req.op == SizeOp::Less && entry.inputCount >= req.value) return false;
        }
    }
    // Check input mask if specified
    if (def->inputMask != 0 && (entry.inputMask & def->inputMask) != def->inputMask)
        return false;
    // Check output count if specified
    if (!def->outputCountRequirements.empty()) {
        for (const auto& req : def->outputCountRequirements) {
            if (req.op == SizeOp::Equal && entry.outputCount != req.value) return false;
            if (req.op == SizeOp::Greater && entry.outputCount <= req.value) return false;
            if (req.op == SizeOp::Less && entry.outputCount >= req.value) return false;
        }
    }
    // Check output mask if specified
    if (def->outputMask != 0 && (entry.outputMask & def->outputMask) != def->outputMask)
        return false;
    // It matches all provided requirements
    return true;
}

// Dump the original shader bytecode to a file for analysis
void DumpOriginalShader_Internal(ShaderDBEntry const& entry, ShaderDefinition* def) {
    if (!def->dump || !entry.IsValid() || entry.IsDumped()) return;
    // Schedule the dump on the game's task queue
    if (g_taskInterface) {
        // Capture entry and def by value to ensure they remain valid in the task
        g_taskInterface->AddTask([type=entry.type,
                                  shaderUID=entry.shaderUID,
                                  hash=entry.hash,
                                  asmHash=entry.asmHash,
                                  size=entry.size,
                                  bytecode=entry.bytecode,
                                  expectedCBSizes=[&entry]() { 
                                        std::array<std::uint32_t, 14> arr; 
                                        std::memcpy(arr.data(), entry.expectedCBSizes, sizeof(entry.expectedCBSizes)); 
                                        return arr; 
                                    }(),
                                  textureSlots=entry.textureSlots,
                                  textureDimensions=entry.textureDimensions,
                                  textureSlotMask=entry.textureSlotMask,
                                  textureDimensionMask=entry.textureDimensionMask,
                                  inputTextureCount=entry.inputTextureCount,
                                  inputCount=entry.inputCount,
                                  inputMask=entry.inputMask,
                                  outputCount=entry.outputCount,
                                  outputMask=entry.outputMask,
                                  def](){
            std::filesystem::path dumpPath = g_pluginPath / "ShaderEngineDumps" / def->id;
            std::filesystem::create_directories(dumpPath);
            if (def->dump && !bytecode.empty()) {
                std::string binFilename = std::format("{}.bin", shaderUID);
                std::string asmFilename = std::format("{}.asm", shaderUID);
                std::string logFilename = std::format("{}.txt", shaderUID);
                std::filesystem::path binPath = dumpPath / binFilename;
                std::filesystem::path asmPath = dumpPath / asmFilename;
                std::filesystem::path logPath = dumpPath / logFilename;
                // Check if files already exist
                if (DEBUGGING && std::filesystem::exists(binPath)) {
                    REX::WARN("DumpOriginalShader_Internal: Binary file already exists, skipping: {}", binPath.string());
                    return;
                }
                // Dump bytecode to binary file
                std::ofstream binFile(binPath, std::ios::binary);
                binFile.write(reinterpret_cast<const char*>(bytecode.data()), bytecode.size());
                binFile.close();
                // Also disassemble to text
                ID3DBlob* disassembly = nullptr;
                HRESULT hr = D3DDisassemble(bytecode.data(), bytecode.size(), 0, nullptr, &disassembly);
                if (REX::W32::SUCCESS(hr) && disassembly) {
                    std::ofstream asmFile(asmPath);
                    asmFile.write(static_cast<const char*>(disassembly->GetBufferPointer()), disassembly->GetBufferSize());
                    asmFile.close();
                    disassembly->Release();
                }
                // Write a log file in the format of the Shader.ini
                std::ofstream logFile(logPath);
                // Write INI section header
                logFile << "[" << def->id << "]" << std::endl;
                logFile << "active=true" << std::endl;
                logFile << "priority=0" << std::endl;
                logFile << "type=" << (type == ShaderType::Pixel ? "ps" : "vs") << std::endl;
                logFile << "shaderUID=" << shaderUID << std::endl;
                logFile << "hash=0x" << std::hex << std::uppercase << hash << std::dec << std::endl;
                logFile << "asmHash=0x" << std::hex << std::uppercase << asmHash << std::dec << std::endl;
                // Size as exact match in parentheses
                logFile << "size=(" << size << ")" << std::endl;
                // Buffer sizes in format: size@slot,size@slot
                logFile << "buffersize=";
                bool firstBuffer = true;
                for (int i = 0; i < 14; ++i) {
                    if (expectedCBSizes[i] > 0) {
                        if (!firstBuffer) logFile << ",";
                        logFile << expectedCBSizes[i] << "@" << i;
                        firstBuffer = false;
                    }
                }
                logFile << std::endl;
                // Textures in format: 0,1,2,...
                logFile << "textures=";
                if (!textureSlots.empty()) {
                    bool firstSlot = true;
                    for (const auto& slot : textureSlots) {
                        if (!firstSlot) logFile << ",";
                        logFile << slot;
                        firstSlot = false;
                    }
                }
                logFile << std::endl;
                // Texture dimensions in format: dimension@slot
                logFile << "textureDimensions=";
                if (!textureDimensions.empty()) {
                    bool firstDim = true;
                    for (const auto& [dimension, slot] : textureDimensions) {
                        if (!firstDim) logFile << ",";
                        logFile << dimension << "@" << slot;
                        firstDim = false;
                    }
                }
                logFile << std::endl;
                // Bitmasks
                logFile << "textureSlotMask=0x" << std::hex << std::uppercase << textureSlotMask << std::dec << std::endl;
                logFile << "textureDimensionMask=0x" << std::hex << std::uppercase << textureDimensionMask << std::dec << std::endl;
                // Counts in parentheses
                logFile << "inputTextureCount=(" << inputTextureCount << ")" << std::endl;
                logFile << "inputcount=(" << inputCount << ")" << std::endl;
                logFile << "inputMask=0x" << std::hex << std::uppercase << inputMask << std::dec << std::endl;
                logFile << "outputcount=(" << outputCount << ")" << std::endl;
                logFile << "outputMask=0x" << std::hex << std::uppercase << outputMask << std::dec << std::endl;
                // Shader file (empty, user needs to add)
                logFile << "shader=;" << shaderUID << "_replacement.hlsl" << std::endl;
                logFile << "log=true" << std::endl;
                logFile << "dump=true" << std::endl;
                // Close section
                logFile << "[/" << def->id << "]" << std::endl;
                logFile.close();
                if (DEBUGGING)
                    REX::INFO("DumpOriginalShader_Internal: Dumped original shader for ID {} to disk for analysis. Binary: {}, Disassembly: {}, Log: {}", def->id, binFilename, asmFilename, logFilename);
            } else {
                if (DEBUGGING)
                    REX::WARN("DumpOriginalShader_Internal: Failed to dump shader for ID {} - either dumping is disabled or bytecode is not available.", def->id);
            }
        });
    } else {
        REX::WARN("DumpOriginalShader_Internal: Failed to dump shader for ID {} - task interface not available.", def->id);
    }
}

REX::W32::ID3D11ShaderResourceView* GetDepthBufferSRV_Internal() {
    if (!g_rendererData) {
        REX::WARN("GetDepthBufferSRV_Internal: RendererData not available.");
        return nullptr;
    }

    // Fallout 4's main scene depth is depthStencilTargets[2].
    // Reference: doodlum/fo4test names this DepthStencilTarget::kMain.
    if (auto* mainDepthSRV = GetMainDepthSRV()) {
        g_lastSceneDepthSRV = mainDepthSRV;
        return mainDepthSRV;
    }

    if (g_rendererData->context) {
        REX::W32::ID3D11DepthStencilView* currentDSV = nullptr;
        g_rendererData->context->OMGetRenderTargets(0, nullptr, &currentDSV);
        if (currentDSV) {
            if (FindDepthTargetIndexForDSV(currentDSV) == static_cast<UINT>(MAIN_DEPTHSTENCIL_TARGET)) {
                g_lastSceneDepthSRV = GetMainDepthSRV();
                currentDSV->Release();
                return g_lastSceneDepthSRV;
            }
            currentDSV->Release();
        }
    }

    if (g_lastSceneDepthSRV && g_lastSceneDepthSRV == GetMainDepthSRV()) {
        return g_lastSceneDepthSRV;
    }

    return nullptr;
}

// Disassemble the shader bytecode and parse it to find out details about the shader
// Normal reflection API does not provide all the info we need and it unreliable
bool ReflectShader_Internal(ShaderDBEntry& entry) {
    if (entry.bytecode.empty()) return false;
    // We disassemble the shader and parse it manually to fill in our entry data.
    ID3DBlob* disassembly = nullptr;
    HRESULT hr = D3DDisassemble(entry.bytecode.data(), entry.bytecode.size(), 0, nullptr, &disassembly);
    if (!REX::W32::SUCCESS(hr) || !disassembly) {
        REX::WARN("ReflectShader_Internal: Failed to disassemble shader bytecode for reflection.");
        return false;
    }
    std::string disasmStr(static_cast<const char*>(disassembly->GetBufferPointer()), disassembly->GetBufferSize());
    // Define regexes as static once
    static const auto regexFlags = std::regex_constants::optimize | std::regex_constants::icase;
        // Catch instructions - only real opcodes, anchored at line start
    static std::regex instrRegex(
        R"(^\s*(add|sub|mul|div|mad|max|min|dp2|dp3|dp4|rsq|sqrt|"
        "and|or|xor|not|lt|gt|le|ge|eq|ne|"
        "mov(?:c|_sat)?|sample(?:_indexable)?|"
        "loop|endloop|if|else|endif|break(?:c)?|ret)\b)",
        regexFlags);
    // Catch t# registers
    static std::regex texRegex(R"(dcl_resource_(\w+)\s*(?:\([^)]*\))?\s*(?:\([^)]+\))?\s+t(\d+))", regexFlags);
    // Catch v# registers (broad match for any dcl_input flavor)
    static std::regex inputRegex(R"(dcl_input[^\s]*\s+v(\d+))", regexFlags);
    // Catch cb# registers
    static std::regex cbRegex(R"(dcl_constantbuffer\s+cb(\d+)\[(\d+)\])", regexFlags);
    // Catch o# registers (broad match for any dcl_output flavor)
    static std::regex outputRegex(R"(dcl_output[^\s]*\s+o(\d+))", regexFlags);
    // Parse the disassembly text
    std::istringstream iss(disasmStr);
    std::string line;
    // Clear the buffers before filling them in case this is called multiple times for the same entry (e.g., if we analyze both pixel and vertex shader for the same entry)
    entry.shaderUID = "";
    entry.asmHash = 0;
    entry.textureSlots.clear();
    entry.textureDimensions.clear();
    entry.textureSlotMask = 0;
    entry.textureDimensionMask = 0;
    entry.inputTextureCount = 0;
    entry.inputCount = 0;
    entry.inputMask = 0;
    // Manual loop for safety to avoid sizeof pointer issues
    for (int i = 0; i < 14; ++i) entry.expectedCBSizes[i] = 0;
    entry.outputCount = 0;
    entry.outputMask = 0;
    std::string asmConcat;
    int inputTextureCount = 0;
    int inputCount = 0;
    int outputCount = 0;
    while (std::getline(iss, line)) {
        // Look for instructions to get a rough idea of shader complexity
        std::smatch match;
        if (std::regex_search(line, match, instrRegex)) {
            asmConcat += match[1].str() + ";"; // Add the opcode to a concatenated string for hashing
        }
        // Look for texture declarations to detect texture slots and dimensions (mainly pixel shaders)
        // Example: "dcl_resource_texture2d (float,float,float,float) t0"
        // Dimensions from d3dcommon.h
        // D3D11_SRV_DIMENSION_UNKNOWN = 0
        // D3D11_SRV_DIMENSION_BUFFER = 1
        // D3D11_SRV_DIMENSION_TEXTURE1D = 3
        // D3D11_SRV_DIMENSION_TEXTURE2D = 4
        // D3D11_SRV_DIMENSION_TEXTURE2DMS = 6
        // D3D11_SRV_DIMENSION_TEXTURE3D = 7
        // D3D11_SRV_DIMENSION_TEXTURECUBE = 8
        // D3D11_SRV_DIMENSION_TEXTURE1DARRAY = 4
        // D3D11_SRV_DIMENSION_TEXTURE2DARRAY = 5
        // D3D11_SRV_DIMENSION_TEXTURECUBEARRAY = 11
        // Texture / Resource declaration parsing (mainly pixel shaders)
        if (std::regex_search(line, match, texRegex)) {
            std::string texType = match[1];      // "texture1d"
            int slot = std::stoi(match[2]);      // 4
            int dimension = 0;
            if (texType == "texture2d") dimension = 4;
            else if (texType == "texture2dms") dimension = 6;
            else if (texType == "texture2darray") dimension = 5;
            else if (texType == "texturecube") dimension = 8;
            else if (texType == "texturecubearray") dimension = 11;
            else if (texType == "texture3d") dimension = 7;
            else if (texType == "texture1d") dimension = 3;
            else if (texType == "buffer") dimension = 1;
            else if (texType == "raw" || texType == "structured") dimension = 0; // For Vertex shaders
            else dimension = 0; // unknown or unsupported type
            entry.textureSlots.push_back(slot); // slot
            entry.textureDimensions.push_back({dimension, slot});
            entry.textureSlotMask |= (1u << slot);
            if (dimension < 32) {
                entry.textureDimensionMask |= (1u << dimension);
            }
            inputTextureCount++;
            continue;
        }
        // Input declaration parsing (mainly vertex shaders)
        // Look for input like POSITION or TEXCOORD to detect input count (mainly vertex shaders)
        if (std::regex_search(line, match, inputRegex)) {
            int regIndex = std::stoi(match[1].str());
            entry.inputMask |= (1u << regIndex);
            inputCount++;
            continue;
        }
        // Constant Buffer declaration parsing to detect expected CB sizes for matching (pixel and vertex shaders)
        // Example: "dcl_constantbuffer CB0[4], immediateIndexed"
        if (std::regex_search(line, match, cbRegex)) {
            int slot = std::stoi(match[1].str());
            int sizeInDwords = std::stoi(match[2].str());
            if (slot >= 0 && slot < 14) {
                entry.expectedCBSizes[slot] = sizeInDwords * 16;
            }
            continue;
        }
        // Look for output declarations to detect output count (pixel and vertex shaders)
        // Example: "dcl_output o0.xyzw"
        if (std::regex_search(line, match, outputRegex)) {
            int outputIndex = std::stoi(match[1].str());
            entry.outputMask |= (1u << outputIndex);
            outputCount++;
        }
    }
    // Clean up
    disassembly->Release();
    entry.asmHash = static_cast<std::uint32_t>(std::hash<std::string_view>{}(asmConcat));
    entry.inputTextureCount = inputTextureCount;
    entry.inputCount = inputCount;
    entry.outputCount = outputCount;
    entry.shaderUID = std::format("{}{:08X}I{}O{}",
        entry.type == ShaderType::Pixel ? "PS" : "VS",
        entry.asmHash,
        entry.inputCount,
        entry.outputCount);
    return true;
}

// Render-thread half of the HLSL hot-reload pipeline. The watcher thread
// just flips a flag (HlslFileWatcher::Check); we do the actual D3D11
// Release / replacement-cache eviction here, on the same thread that issues
// draws, so we never race the immediate context. Called from MyPSSetShader
// and MyVSSetShader before they reach CompileShader_Internal.
void MaybeApplyHlslHotReload_Internal(ShaderDefinition* def) {
    if (!def || !def->hlslFileWatcher) return;
    if (!def->hlslFileWatcher->ConsumeReloadRequest()) return;
    // Take the per-def compile mutex so an in-flight precompile (or a
    // concurrent on-demand render-thread compile) cannot publish bytecode
    // that we're about to drop, leaving us with a stale but newly-cached
    // replacement.
    if (!def->compileMutex) def->compileMutex = std::make_unique<std::mutex>();
    std::lock_guard compileLock(*def->compileMutex);
    if (def->loadedPixelShader)  { def->loadedPixelShader->Release();  def->loadedPixelShader  = nullptr; }
    if (def->loadedVertexShader) { def->loadedVertexShader->Release(); def->loadedVertexShader = nullptr; }
    if (def->compiledShader)     { def->compiledShader->Release();     def->compiledShader     = nullptr; }
    def->buggy = false;
    g_ShaderDB.ClearReplacementsForDefinition(def);
    REX::INFO("MaybeApplyHlslHotReload_Internal: dropped compiled state for definition '{}'", def->id);
}

// Orchestrator for hot INI reload - rematches all ShaderDB entries against current definitions
void RematchAllShaders_Internal() {
    std::unique_lock lockDB(g_ShaderDB.mutex);  // Write lock on ShaderDB
    if (DEBUGGING)
        REX::INFO("RematchAllShaders_Internal: Rematching {} pixel shaders and vertex shaders...", g_ShaderDB.entries.size());
    int matchedPS = 0;
    int matchedVS = 0;
    // Iterate definitions in priority order (already sorted)
    std::shared_lock lock(g_shaderDefinitions.mutex);
    for (ShaderDefinition* def : g_shaderDefinitions.definitions) {
        if (!def->active) continue;
        // Check all pixel shaders for this definition
        for (auto& [shader, entry] : g_ShaderDB.entries) {
            if (entry.IsMatched()) continue;
            if (DoesEntryMatchDefinition_Internal(entry, def)) {
                if (DEBUGGING)
                    REX::INFO("RematchAllShaders_Internal: Matched {} shader with hash {:08X} and size {} bytes to definition '{}'", (def->type == ShaderType::Pixel ? "pixel" : "vertex"), entry.hash, entry.size, def->id);
                entry.matchedDefinition = def;
                entry.SetMatched(true);
                if (def->type == ShaderType::Pixel) {
                    matchedPS++;
                } else if (def->type == ShaderType::Vertex) {
                    matchedVS++;
                }
                if (DEVELOPMENT && def->log) {
                    REX::INFO("RematchAllShaders_Internal: ------------------------------------------------");
                    REX::INFO("RematchAllShaders_Internal: Found matching shader definition '{}' for {} shader with ShaderUID '{}'.", def->id, entry.type == ShaderType::Vertex ? "Vertex" : "Pixel", entry.shaderUID);
                    REX::INFO("RematchAllShaders_Internal: Shader Hash: {:08X}", entry.hash);
                    REX::INFO("RematchAllShaders_Internal: Shader Size: {} bytes", entry.size);
                    REX::INFO("RematchAllShaders_Internal: Shader ASM Hash: {:08X}", entry.asmHash);
                    REX::INFO(" - Shader CB Sizes: {},{},{},{},{},{},{},{},{},{},{},{},{},{}", 
                        entry.expectedCBSizes[0], entry.expectedCBSizes[1], entry.expectedCBSizes[2], entry.expectedCBSizes[3],
                        entry.expectedCBSizes[4], entry.expectedCBSizes[5], entry.expectedCBSizes[6], entry.expectedCBSizes[7],
                        entry.expectedCBSizes[8], entry.expectedCBSizes[9], entry.expectedCBSizes[10], entry.expectedCBSizes[11],
                        entry.expectedCBSizes[12], entry.expectedCBSizes[13]);
                    REX::INFO(" - Shader Texture Register Slots: {}", entry.textureSlots.empty() ? "None" : "");
                    for (const auto& slot : entry.textureSlots) {
                        REX::INFO("   - Slot: t{}", slot);
                    }
                    REX::INFO(" - Shader Texture Dimensions: {}", entry.textureDimensions.empty() ? "None" : "");
                    for (const auto& [dimension, slot] : entry.textureDimensions) {
                        REX::INFO("   - Dimension: {}, Slot: t{}", dimension, slot);
                    }
                    REX::INFO(" - Shader Texture Usage Bitmask: 0x{:08X}", entry.textureSlotMask);
                    REX::INFO(" - Shader Texture Dimension Bitmask: 0x{:08X}", entry.textureDimensionMask);
                    REX::INFO(" - Shader Input Texture Count: {}", entry.inputTextureCount != -1 ? std::to_string(entry.inputTextureCount) : "X");
                    REX::INFO(" - Shader Input Count: {}", entry.inputCount != -1 ? std::to_string(entry.inputCount) : "X");
                    REX::INFO(" - Shader Input Mask: 0x{:08X}", entry.inputMask);
                    REX::INFO(" - Shader Output Count: {}", entry.outputCount != -1 ? std::to_string(entry.outputCount) : "X");
                    REX::INFO(" - Shader Output Mask: 0x{:08X}", entry.outputMask);
                    REX::INFO("RematchAllShaders_Internal: ------------------------------------------------");
                }
                if (DEVELOPMENT && def->dump && !entry.IsDumped()) {
                    DumpOriginalShader_Internal(entry, def);
                    entry.SetDumped(true);
                }
            }
        }
    }
    if (DEBUGGING)
        REX::INFO("RematchAllShaders_Internal: Matched {} pixel shaders and {} vertex shaders", matchedPS, matchedVS);
}

void UIDrawShaderSettingsOverlay() {
    // Position window in top-right corner
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - SHADERSETTINGS_WIDTH - 10, 10), ImGuiCond_FirstUseEver);
    // Set width/height
    ImGui::SetNextWindowSize(ImVec2(SHADERSETTINGS_WIDTH, SHADERSETTINGS_HEIGHT), ImGuiCond_FirstUseEver);
    // Make background semi-transparent
    ImGui::SetNextWindowBgAlpha(SHADERSETTINGS_OPACITY);
    // Create the Window
    ImGui::Begin("ShaderEngine Settings");
    if (ImGui::Button("Reset defaults")) {
        for (auto* sValue : g_shaderSettings.GetGlobalShaderValues()) {
            if (sValue) sValue->ResetToDefault();
        }
        for (auto* sValue : g_shaderSettings.GetLocalShaderValues()) {
            if (sValue) sValue->ResetToDefault();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Save settings")) {
        SaveShaderSettingsWithFeedback();
    }
    ImGui::Separator();

    bool passLevelOcclusionOn = PASS_LEVEL_OCCLUSION_ON;
    if (ImGui::Checkbox("Pass-level cached occlusion", &passLevelOcclusionOn)) {
        ApplyPassLevelOcclusionSetting(passLevelOcclusionOn);
    }
    bool shadowCacheDirectionalMapSlot1On = SHADOW_CACHE_DIRECTIONAL_MAPSLOT1_ON;
    if (ImGui::Checkbox("Cache directional shadow cascade", &shadowCacheDirectionalMapSlot1On)) {
        ApplyShadowCacheDirectionalMapSlot1Setting(shadowCacheDirectionalMapSlot1On);
    }
    ImGui::Separator();

    static std::string editingValueId;
    static bool focusEditInput = false;

    // Render a row for each shader value with appropriate control based on type
    auto renderRow = [&](ShaderValue &sValue) {
        ImGui::PushID(sValue.id.c_str());
        const bool canEditValue = sValue.type == ShaderValue::Type::Int || sValue.type == ShaderValue::Type::Float;
        const bool isEditingValue = canEditValue && editingValueId == sValue.id;

        if (isEditingValue && focusEditInput) {
            ImGui::SetKeyboardFocusHere();
            focusEditInput = false;
        }

        switch (sValue.type) {
            case ShaderValue::Type::Bool:
                if (ImGui::Checkbox(sValue.label.c_str(), &sValue.current.b)) {
                    /* value changed if you need to react */
                }
                break;
            case ShaderValue::Type::Int:
                if (isEditingValue) {
                    if (ImGui::InputInt(sValue.label.c_str(), &sValue.current.i, 0, 0, ImGuiInputTextFlags_EnterReturnsTrue)) {
                        sValue.current.i = std::clamp(sValue.current.i, sValue.min.i, sValue.max.i);
                        editingValueId.clear();
                    } else if (ImGui::IsItemDeactivated()) {
                        sValue.current.i = std::clamp(sValue.current.i, sValue.min.i, sValue.max.i);
                        editingValueId.clear();
                    }
                } else if (ImGui::SliderInt(sValue.label.c_str(), &sValue.current.i, sValue.min.i, sValue.max.i, "%d", ImGuiSliderFlags_AlwaysClamp)) {
                    /* value changed */
                }
                break;
            case ShaderValue::Type::Float:
                if (isEditingValue) {
                    if (ImGui::InputFloat(sValue.label.c_str(), &sValue.current.f, 0.0f, 0.0f, "%.3f", ImGuiInputTextFlags_EnterReturnsTrue)) {
                        sValue.current.f = std::clamp(sValue.current.f, sValue.min.f, sValue.max.f);
                        editingValueId.clear();
                    } else if (ImGui::IsItemDeactivated()) {
                        sValue.current.f = std::clamp(sValue.current.f, sValue.min.f, sValue.max.f);
                        editingValueId.clear();
                    }
                } else if (ImGui::SliderFloat(sValue.label.c_str(), &sValue.current.f, sValue.min.f, sValue.max.f, "%.3f", ImGuiSliderFlags_AlwaysClamp)) {
                    /* value changed */
                }
                break;
        }
        if (canEditValue) {
            ImGui::SameLine();
            if (ImGui::SmallButton("E")) {
                editingValueId = sValue.id;
                focusEditInput = true;
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Edit value");
            }
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("R")) {
            sValue.ResetToDefault();
            if (editingValueId == sValue.id) {
                editingValueId.clear();
            }
        }
        ImGui::PopID();
    };
    // Collapsing header for global shader settings
    if (ImGui::CollapsingHeader("Global Settings", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::SmallButton("Reset global")) {
            for (auto* sValue : g_shaderSettings.GetGlobalShaderValues()) {
                if (sValue) sValue->ResetToDefault();
            }
        }
        std::map<std::string, std::vector<ShaderValue*>> globalGroups;
        for (auto* sValue : g_shaderSettings.GetGlobalShaderValues()) {
            if (!sValue) continue;
            const std::string groupName = sValue->group.empty() ? "Ungrouped" : sValue->group;
            globalGroups[groupName].push_back(sValue);
        }
        for (auto& kv : globalGroups) {
            const std::string& groupName = kv.first;
            auto& vals = kv.second;
            if (ImGui::CollapsingHeader(groupName.c_str())) {
                ImGui::PushID(groupName.c_str());
                if (ImGui::SmallButton("Reset group")) {
                    for (auto* sValue : vals) {
                        if (sValue) sValue->ResetToDefault();
                    }
                }
                for (auto* sValue : vals) {
                    if (sValue) renderRow(*sValue);
                }
                ImGui::PopID();
            }
        }
    }
    // Collapsing header for active shader definitions and their settings
    if (ImGui::CollapsingHeader("Shader Settings", ImGuiTreeNodeFlags_DefaultOpen))
    {
        // Group local values by explicit Values.ini group, then folder/module.
        std::map<std::string, std::vector<ShaderValue*>> settingsGroups;
        for (auto* sValue : g_shaderSettings.GetLocalShaderValues()) {
            if (!sValue) continue;
            std::string groupName = !sValue->group.empty() ? sValue->group :
                (sValue->folderName.empty() ? sValue->shaderDefinitionId : sValue->folderName);
            settingsGroups[groupName].push_back(sValue);
        }
        // Draw one collapsing header per definition and render children only if open
        for (auto &kv : settingsGroups) {
            const std::string &groupName = kv.first;
            auto &vals = kv.second;
            if (ImGui::CollapsingHeader(groupName.c_str())) {
                ImGui::PushID(groupName.c_str());
                if (ImGui::SmallButton("Reset group")) {
                    for (auto* sValue : vals) {
                        if (sValue) sValue->ResetToDefault();
                    }
                }
                for (auto* sValue : vals) {
                    if (sValue) renderRow(*sValue);
                }
                ImGui::PopID();
            }
        }
    }

    if (g_shaderSettingsSaveModalRequested) {
        ImGui::OpenPopup("Shader settings save");
        g_shaderSettingsSaveModalRequested = false;
    }
    if (ImGui::BeginPopupModal("Shader settings save", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        constexpr float modalContentWidth = 360.0f;
        ImGui::TextColored(
            g_shaderSettingsSaveSucceeded ? ImVec4(0.3f, 1.0f, 0.3f, 1.0f) : ImVec4(1.0f, 0.3f, 0.3f, 1.0f),
            "%s",
            g_shaderSettingsSaveSucceeded ? "Success" : "Error");
        ImGui::Separator();
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + modalContentWidth);
        ImGui::TextWrapped("%s", g_shaderSettingsSaveMessage.c_str());
        ImGui::PopTextWrapPos();
        ImGui::Dummy(ImVec2(modalContentWidth, 0.0f));
        if (ImGui::Button("OK")) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    ImGui::End();
}

// DEVGUI drawing function for shader debug overlay is called by the Hook Present once per frame
// Should ONLY contain ImGui drawing code!
void UIDrawShaderDebugOverlay() {
    // Position window in top-left corner
    ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_FirstUseEver);
    // Set width/height
    ImGui::SetNextWindowSize(ImVec2(DEVGUI_WIDTH, DEVGUI_HEIGHT), ImGuiCond_FirstUseEver);
    // Make background semi-transparent
    ImGui::SetNextWindowBgAlpha(DEVGUI_OPACITY);
    // Create the Window
    ImGui::Begin("ShaderEngine Shader Monitor");
    // Tickboxes for showing replaced shaders and locking the shader list
    ImGui::SameLine(ImGui::GetWindowWidth() - 340);
    if (ImGui::SmallButton("Copy CSV")) {
        // Snapshot the same set of rows the table renders, in the same order,
        // and push them to the clipboard as CSV (definitionId,used,shaderUid).
        // Run before renderRow consumes the recentlyUsed flags so the snapshot
        // reflects what the user sees in the Used column this frame.
        std::string csv = "definition,status,used,shaderUid\n";
        auto appendRow = [&](const ShaderDBEntry& e) {
            const ShaderDefinition* def = e.GetMatchedDefinition();
            const char* id = def ? def->id.c_str() : "<removed>";
            const bool used = e.IsRecentlyUsed();
            const char* uid = e.shaderUID.empty() ? "<unknown>" : e.shaderUID.c_str();
            const bool hasReplacement = (e.type == ShaderType::Pixel)
                ? (e.GetReplacementPixelShader() != nullptr)
                : (e.GetReplacementVertexShader() != nullptr);
            const bool hasShaderFile = def && !def->shaderFile.empty();
            const bool isBuggy = def && def->buggy;
            const char* status = hasReplacement
                ? "REPLACED"
                : (isBuggy ? "BUGGY"
                           : (hasShaderFile ? "INVALID" : "MATCHED"));
            csv += id;
            csv += ',';
            csv += status;
            csv += used ? ",YES," : ",NO,";
            csv += uid;
            csv += '\n';
        };
        std::shared_lock lock(g_ShaderDB.mutex);
        if (g_shaderListLocked) {
            for (void* key : g_lockedShaderKeys) {
                auto it = g_ShaderDB.entries.find(key);
                if (it == g_ShaderDB.entries.end()) continue;
                if (!g_showReplaced) {
                    bool isReplacedNonFlash =
                        (it->second.type == ShaderType::Pixel && it->second.GetReplacementPixelShader() && it->second.GetReplacementPixelShader() != g_flashPixelShader)
                     || (it->second.type == ShaderType::Vertex && it->second.GetReplacementVertexShader());
                    if (isReplacedNonFlash) continue;
                }
                appendRow(it->second);
            }
        } else {
            for (auto& [ptr, entry] : g_ShaderDB.entries) {
                if (!entry.IsMatched() || !entry.GetMatchedDefinition()) continue;
                if (!g_showReplaced) {
                    bool isReplacedNonFlash =
                        (entry.type == ShaderType::Pixel && entry.GetReplacementPixelShader() && entry.GetReplacementPixelShader() != g_flashPixelShader)
                     || (entry.type == ShaderType::Vertex && entry.GetReplacementVertexShader());
                    if (isReplacedNonFlash) continue;
                }
                appendRow(entry);
            }
        }
        ImGui::SetClipboardText(csv.c_str());
    }
    ImGui::SameLine(ImGui::GetWindowWidth() - 240);
    ImGui::Checkbox("Replaced", &g_showReplaced);
    ImGui::SameLine(ImGui::GetWindowWidth() - 120);
    if (ImGui::Checkbox("Lock list", &g_shaderListLocked)) {
        if (g_shaderListLocked) UILockShaderList_Internal();
        else UIUnlockShaderList_Internal();
    }
    // Collapsing header for active definitions and their matched shaders
    if (ImGui::CollapsingHeader("Active Definitions", ImGuiTreeNodeFlags_DefaultOpen)) {
        constexpr ImGuiTableFlags shaderTableFlags =
            ImGuiTableFlags_Sortable
          | ImGuiTableFlags_BordersInnerV
          | ImGuiTableFlags_RowBg
          | ImGuiTableFlags_Resizable;
        if (ImGui::BeginTable("shader_columns", 5, shaderTableFlags)) {
            const float charW = ImGui::CalcTextSize("W").x;
            // Sortable columns use stable user IDs so layout/index changes don't break the comparator switch.
            // ID is the default sort, ascending — matches "sort by name, ascending" requirement.
            ImGui::TableSetupColumn("ID",        ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_PreferSortAscending | ImGuiTableColumnFlags_WidthStretch, 0.0f, 0);
            ImGui::TableSetupColumn("Status",    ImGuiTableColumnFlags_WidthFixed, charW * 9.0f, 1);
            ImGui::TableSetupColumn("Used",      ImGuiTableColumnFlags_WidthFixed, charW * 5.0f, 2);
            ImGui::TableSetupColumn("ShaderUID", ImGuiTableColumnFlags_WidthFixed, charW * 14.0f, 3);
            ImGui::TableSetupColumn("Action",    ImGuiTableColumnFlags_NoSort | ImGuiTableColumnFlags_WidthFixed, charW * 8.0f, 4);
            ImGui::TableHeadersRow();
            // Render a single row
            auto renderRow = [&](ShaderDBEntry& entry) {
                ShaderDefinition* def = entry.GetMatchedDefinition();
                ImGui::TableNextRow();
                // Column 0: ID
                ImGui::TableSetColumnIndex(0);
                const char* id = def ? def->id.c_str() : "<removed>";
                bool hasReplacement = (entry.type == ShaderType::Pixel) ? (entry.GetReplacementPixelShader() != nullptr) : (entry.GetReplacementVertexShader() != nullptr);
                bool hasShaderFile = def && !def->shaderFile.empty();
                if (hasReplacement) ImGui::TextColored(ImVec4(0,1,0,1), "%s", id);
                else if (hasShaderFile) ImGui::TextColored(ImVec4(1,0.5f,0,1), "%s", id);
                else ImGui::TextColored(ImVec4(1,1,0,1), "%s", id);
                // Column 1: Status
                ImGui::TableSetColumnIndex(1);
                if (hasReplacement) ImGui::TextColored(ImVec4(0,1,0,1), "REPLACED");
                else if (hasShaderFile) ImGui::TextColored(ImVec4(1,0.5f,0,1), "INVALID");
                else ImGui::TextColored(ImVec4(1,1,0,1), "MATCHED");
                // Column 2: Recently used
                ImGui::TableSetColumnIndex(2);
                bool usedThisFrame = entry.IsRecentlyUsed();
                if (usedThisFrame) {
                    ImGui::TextColored(ImVec4(0,1,1,1), "YES");
                    entry.SetRecentlyUsed(false); // reset for next frame
                } else {
                    ImGui::TextColored(ImVec4(0.5f,0.5f,0.5f,1), "NO");
                }
                // Column 3: ShaderUID
                ImGui::TableSetColumnIndex(3);
                const char* shaderUID = entry.shaderUID.empty() ? "<unknown>" : entry.shaderUID.c_str();
                ImGui::Text("%s", shaderUID);
                // Column 4: Actions (Flash)
                ImGui::TableSetColumnIndex(4);
                ImGui::PushID((void*)entry.originalShader); // Use original shader pointer as unique ID to avoid ID collisions in the list
                auto* currentShader = entry.GetReplacementPixelShader();
                if (entry.type == ShaderType::Pixel && (!currentShader || currentShader == g_flashPixelShader)) {
                    ImGui::BeginDisabled(!g_flashPixelShader && currentShader != g_flashPixelShader);
                    if (ImGui::SmallButton(currentShader == g_flashPixelShader ? "Unflash" : "Flash")) {
                        entry.SetReplacementPixelShader(currentShader == g_flashPixelShader ? nullptr : g_flashPixelShader);
                    }
                    ImGui::EndDisabled();
                }
                ImGui::PopID();
            };
            // Build snapshot, sort per current sort spec, then render. Lock held throughout
            // so the underlying entries (referenced by raw pointer in the snapshot) stay alive.
            {
                std::shared_lock lock(g_ShaderDB.mutex);
                std::vector<ShaderDBEntry*> rows;
                if (g_shaderListLocked) {
                    rows.reserve(g_lockedShaderKeys.size());
                    for (void* key : g_lockedShaderKeys) {
                        auto it = g_ShaderDB.entries.find(key);
                        if (it == g_ShaderDB.entries.end()) {
                            // Skip if the entry no longer exists in the ShaderDB
                            continue;
                        }
                        // Filter out replaced shaders if the option is disabled
                        if (!g_showReplaced) {
                            bool isReplacedNonFlash =
                                (it->second.type == ShaderType::Pixel && it->second.GetReplacementPixelShader() && it->second.GetReplacementPixelShader() != g_flashPixelShader)
                            || (it->second.type == ShaderType::Vertex && it->second.GetReplacementVertexShader());
                            if (isReplacedNonFlash)
                                continue;
                        }
                        rows.push_back(&it->second);
                    }
                } else {
                    rows.reserve(g_ShaderDB.entries.size());
                    for (auto& [ptr, entry] : g_ShaderDB.entries) {
                        ShaderDefinition* def = entry.GetMatchedDefinition();
                        if (entry.IsMatched() && entry.IsRecentlyUsed() && def) {
                            // Filter out replaced shaders if the option is disabled
                            if (!g_showReplaced) {
                                bool isReplacedNonFlash =
                                    (entry.type == ShaderType::Pixel && entry.GetReplacementPixelShader() && entry.GetReplacementPixelShader() != g_flashPixelShader)
                                || (entry.type == ShaderType::Vertex && entry.GetReplacementVertexShader());
                                if (isReplacedNonFlash)
                                    continue;
                            }
                            rows.push_back(&entry);
                        }
                    }
                }
                // Case-insensitive string compare for alphabetical sorts on ID and ShaderUID.
                auto ciCompare = [](const char* a, const char* b) -> int {
                    while (*a && *b) {
                        const int ca = std::tolower(static_cast<unsigned char>(*a));
                        const int cb = std::tolower(static_cast<unsigned char>(*b));
                        if (ca != cb) return ca - cb;
                        ++a; ++b;
                    }
                    return static_cast<int>(static_cast<unsigned char>(*a)) - static_cast<int>(static_cast<unsigned char>(*b));
                };
                auto statusRank = [](const ShaderDBEntry* e) -> int {
                    const ShaderDefinition* d = e->GetMatchedDefinition();
                    const bool hasReplacement = (e->type == ShaderType::Pixel)
                        ? (e->GetReplacementPixelShader() != nullptr)
                        : (e->GetReplacementVertexShader() != nullptr);
                    const bool hasShaderFile = d && !d->shaderFile.empty();
                    if (hasReplacement) return 0; // REPLACED
                    if (hasShaderFile) return 1;  // INVALID
                    return 2;                     // MATCHED
                };
                ImGuiTableSortSpecs* sortSpecs = ImGui::TableGetSortSpecs();
                if (sortSpecs && sortSpecs->SpecsCount > 0) {
                    const ImGuiTableColumnSortSpecs& spec = sortSpecs->Specs[0];
                    const bool ascending = spec.SortDirection != ImGuiSortDirection_Descending;
                    std::sort(rows.begin(), rows.end(), [&](ShaderDBEntry* a, ShaderDBEntry* b) {
                        int cmp = 0;
                        switch (spec.ColumnUserID) {
                            case 0: { // ID (Name)
                                const ShaderDefinition* da = a->GetMatchedDefinition();
                                const ShaderDefinition* db = b->GetMatchedDefinition();
                                cmp = ciCompare(da ? da->id.c_str() : "<removed>",
                                                db ? db->id.c_str() : "<removed>");
                                break;
                            }
                            case 1: // Status
                                cmp = statusRank(a) - statusRank(b);
                                break;
                            case 2: // Used: NO < YES (false < true) for ascending
                                cmp = (a->IsRecentlyUsed() ? 1 : 0) - (b->IsRecentlyUsed() ? 1 : 0);
                                break;
                            case 3: // ShaderUID
                                cmp = ciCompare(a->shaderUID.empty() ? "<unknown>" : a->shaderUID.c_str(),
                                                b->shaderUID.empty() ? "<unknown>" : b->shaderUID.c_str());
                                break;
                            default:
                                break;
                        }
                        if (cmp == 0) {
                            // Stable tie-breaker keeps row order deterministic across frames
                            return a < b;
                        }
                        return ascending ? (cmp < 0) : (cmp > 0);
                    });
                }
                for (ShaderDBEntry* entry : rows) {
                    renderRow(*entry);
                }
            }
            ImGui::EndTable();
        }
    }
    ImGui::End();
}

// DEVGUI drawing function for the custom t# buffer. This reads the CPU-side
// snapshot immediately after UpdateCustomBuffer_Internal has populated it.
void UIDrawCustomBufferMonitorOverlay() {
    static GFXBoosterAccessData previousData{};
    static DrawTagData previousDrawTag{};
    static bool hasPreviousData = false;

    const GFXBoosterAccessData data = g_customBufferData;
    const DrawTagData drawTag = g_drawTagData;

    ImGui::SetNextWindowPos(ImVec2(10, 320), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(560, 640), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(DEVGUI_OPACITY);
    ImGui::Begin("ShaderEngine Custom Buffer Monitor");

    ImGui::Text("Custom SRV slot: t%u", CUSTOMBUFFER_SLOT);
    ImGui::Text("Draw tag SRV slot: t%u", DRAWTAG_SLOT);
    ImGui::Text("Custom buffer: %p", static_cast<void*>(g_customSRVBuffer));
    ImGui::Text("Custom SRV:    %p", static_cast<void*>(g_customSRV));
    ImGui::Text("DrawTag buffer:%p", static_cast<void*>(g_drawTagSRVBuffer));
    ImGui::Text("DrawTag SRV:   %p", static_cast<void*>(g_drawTagSRV));
    ImGui::Separator();

    // ImGui Columns() doesn't persist user drag-resize across frames — the
    // SetColumnWidth calls every frame would clobber any drag anyway. Tables
    // give proper resizable / reorderable columns.
    constexpr ImGuiTableFlags kCBTableFlags =
        ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerH |
        ImGuiTableFlags_RowBg     | ImGuiTableFlags_SizingStretchProp;

    auto beginColumns = [](const char* id) -> bool {
        if (!ImGui::BeginTable(id, 3, kCBTableFlags)) return false;
        ImGui::TableSetupColumn("Name",  ImGuiTableColumnFlags_WidthStretch, 2.0f);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 1.4f);
        ImGui::TableSetupColumn("Delta", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableHeadersRow();
        return true;
    };

    auto endColumns = []() { ImGui::EndTable(); };

    auto renderFloat = [&](const char* label, float value, float previous, float warnDelta = 0.25f) {
        const float delta = hasPreviousData ? value - previous : 0.0f;
        const bool invalid = !std::isfinite(value);
        const bool jump = hasPreviousData && std::fabs(delta) >= warnDelta;
        const ImVec4 valueColor = invalid ? ImVec4(1.0f, 0.1f, 0.1f, 1.0f) : (jump ? ImVec4(1.0f, 0.8f, 0.1f, 1.0f) : ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextUnformatted(label);
        ImGui::TableNextColumn(); ImGui::TextColored(valueColor, "%.6f", value);
        ImGui::TableNextColumn();
        if (hasPreviousData) {
            ImGui::TextColored(jump ? ImVec4(1.0f, 0.8f, 0.1f, 1.0f) : ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "%+.6f", delta);
        } else {
            ImGui::TextDisabled("-");
        }
    };

    auto renderUInt = [&](const char* label, uint32_t value, uint32_t previous) {
        const bool changed = hasPreviousData && value != previous;
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextUnformatted(label);
        ImGui::TableNextColumn(); ImGui::TextColored(changed ? ImVec4(1.0f, 0.8f, 0.1f, 1.0f) : ImVec4(1.0f, 1.0f, 1.0f, 1.0f), "0x%08X", value);
        ImGui::TableNextColumn();
        if (hasPreviousData) {
            ImGui::Text("%+lld", static_cast<long long>(value) - static_cast<long long>(previous));
        } else {
            ImGui::TextDisabled("-");
        }
    };

    auto renderInt = [&](const char* label, int32_t value, int32_t previous) {
        const bool changed = hasPreviousData && value != previous;
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextUnformatted(label);
        ImGui::TableNextColumn(); ImGui::TextColored(changed ? ImVec4(1.0f, 0.8f, 0.1f, 1.0f) : ImVec4(1.0f, 1.0f, 1.0f, 1.0f), "%d", value);
        ImGui::TableNextColumn();
        if (hasPreviousData) {
            ImGui::Text("%+d", value - previous);
        } else {
            ImGui::TextDisabled("-");
        }
    };

    auto renderFloat4 = [&](const char* label, const DirectX::XMFLOAT4& value, const DirectX::XMFLOAT4& previous, float warnDelta = 0.25f) {
        renderFloat((std::string(label) + ".x").c_str(), value.x, previous.x, warnDelta);
        renderFloat((std::string(label) + ".y").c_str(), value.y, previous.y, warnDelta);
        renderFloat((std::string(label) + ".z").c_str(), value.z, previous.z, warnDelta);
        renderFloat((std::string(label) + ".w").c_str(), value.w, previous.w, warnDelta);
    };

    if (ImGui::CollapsingHeader("Frame", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (beginColumns("custom_buffer_frame_columns")) {
            renderFloat("time", data.time, previousData.time, 1.0f);
            renderFloat("delta", data.delta, previousData.delta, 0.05f);
            renderFloat("frame", data.frame, previousData.frame, 2.0f);
            renderFloat("fps", data.fps, previousData.fps, 10.0f);
            renderFloat("random", data.random, previousData.random, 0.9f);
            endColumns();
        }
    }

    if (ImGui::CollapsingHeader("Scene State", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (beginColumns("custom_buffer_scene_columns")) {
            renderFloat("dayCycle", data.dayCycle, previousData.dayCycle, 0.05f);
            renderFloat("timeOfDay", data.timeOfDay, previousData.timeOfDay, 0.25f);
            renderFloat("weatherTransition", data.weatherTransition, previousData.weatherTransition, 0.05f);
            renderUInt("currentWeatherID", data.currentWeatherID, previousData.currentWeatherID);
            renderUInt("outgoingWeatherID", data.outgoingWeatherID, previousData.outgoingWeatherID);
            renderUInt("currentLocationID", data.currentLocationID, previousData.currentLocationID);
            renderUInt("worldSpaceID", data.worldSpaceID, previousData.worldSpaceID);
            renderUInt("skyMode", data.skyMode, previousData.skyMode);
            renderInt("currentWeatherClass", data.currentWeatherClass, previousData.currentWeatherClass);
            renderInt("outgoingWeatherClass", data.outgoingWeatherClass, previousData.outgoingWeatherClass);
            renderFloat("inInterior", data.inInterior, previousData.inInterior, 0.5f);
            renderFloat("inCombat", data.inCombat, previousData.inCombat, 0.5f);
            endColumns();
        }
    }

    if (ImGui::CollapsingHeader("Camera", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (beginColumns("custom_buffer_camera_columns")) {
            renderFloat("camX", data.camX, previousData.camX, 100.0f);
            renderFloat("camY", data.camY, previousData.camY, 100.0f);
            renderFloat("camZ", data.camZ, previousData.camZ, 100.0f);
            renderFloat("viewDirX", data.viewDirX, previousData.viewDirX, 0.25f);
            renderFloat("viewDirY", data.viewDirY, previousData.viewDirY, 0.25f);
            renderFloat("viewDirZ", data.viewDirZ, previousData.viewDirZ, 0.25f);
            renderFloat("vpLeft", data.vpLeft, previousData.vpLeft, 1.0f);
            renderFloat("vpTop", data.vpTop, previousData.vpTop, 1.0f);
            renderFloat("vpWidth", data.vpWidth, previousData.vpWidth, 1.0f);
            renderFloat("vpHeight", data.vpHeight, previousData.vpHeight, 1.0f);
            endColumns();
        }
    }

    if (ImGui::CollapsingHeader("Fog", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (beginColumns("custom_buffer_fog_columns")) {
            renderFloat4("g_FogDistances0", data.g_FogDistances0, previousData.g_FogDistances0, 50.0f);
            renderFloat4("g_FogDistances1", data.g_FogDistances1, previousData.g_FogDistances1, 50.0f);
            renderFloat4("g_FogParams", data.g_FogParams, previousData.g_FogParams, 0.25f);
            renderFloat4("g_FogColor", data.g_FogColor, previousData.g_FogColor, 0.05f);
            renderFloat("g_SunR", data.g_SunR, previousData.g_SunR, 0.05f);
            renderFloat("g_SunG", data.g_SunG, previousData.g_SunG, 0.05f);
            renderFloat("g_SunB", data.g_SunB, previousData.g_SunB, 0.05f);
            renderFloat("g_SunDirX", data.g_SunDirX, previousData.g_SunDirX, 0.05f);
            renderFloat("g_SunDirY", data.g_SunDirY, previousData.g_SunDirY, 0.05f);
            renderFloat("g_SunDirZ", data.g_SunDirZ, previousData.g_SunDirZ, 0.05f);
            renderFloat("g_SunValid", data.g_SunValid, previousData.g_SunValid, 0.5f);
            endColumns();
        }
    }

    if (ImGui::CollapsingHeader("Matrices")) {
        if (beginColumns("custom_buffer_matrix_columns")) {
            renderFloat4("InvProjRow0", data.g_InvProjRow0, previousData.g_InvProjRow0, 0.05f);
            renderFloat4("InvProjRow1", data.g_InvProjRow1, previousData.g_InvProjRow1, 0.05f);
            renderFloat4("InvProjRow2", data.g_InvProjRow2, previousData.g_InvProjRow2, 0.05f);
            renderFloat4("InvProjRow3", data.g_InvProjRow3, previousData.g_InvProjRow3, 0.05f);
            renderFloat4("InvViewRow0", data.g_InvViewRow0, previousData.g_InvViewRow0, 0.05f);
            renderFloat4("InvViewRow1", data.g_InvViewRow1, previousData.g_InvViewRow1, 0.05f);
            renderFloat4("InvViewRow2", data.g_InvViewRow2, previousData.g_InvViewRow2, 0.05f);
            renderFloat4("InvViewRow3", data.g_InvViewRow3, previousData.g_InvViewRow3, 0.05f);
            renderFloat4("ViewProjRow0", data.g_ViewProjRow0, previousData.g_ViewProjRow0, 0.05f);
            renderFloat4("ViewProjRow1", data.g_ViewProjRow1, previousData.g_ViewProjRow1, 0.05f);
            renderFloat4("ViewProjRow2", data.g_ViewProjRow2, previousData.g_ViewProjRow2, 0.05f);
            renderFloat4("ViewProjRow3", data.g_ViewProjRow3, previousData.g_ViewProjRow3, 0.05f);
            endColumns();
        }
    }

    if (ImGui::CollapsingHeader("Player/Input")) {
        if (beginColumns("custom_buffer_player_columns")) {
            renderFloat("resX", data.resX, previousData.resX, 1.0f);
            renderFloat("resY", data.resY, previousData.resY, 1.0f);
            renderFloat("mouseX", data.mouseX, previousData.mouseX, 0.25f);
            renderFloat("mouseY", data.mouseY, previousData.mouseY, 0.25f);
            renderFloat("pHealthPerc", data.pHealthPerc, previousData.pHealthPerc, 0.05f);
            renderFloat("pRadDmg", data.pRadDmg, previousData.pRadDmg, 0.25f);
            renderFloat("windSpeed", data.windSpeed, previousData.windSpeed, 0.25f);
            renderFloat("windAngle", data.windAngle, previousData.windAngle, 0.25f);
            renderFloat("windTurb", data.windTurb, previousData.windTurb, 0.25f);
            endColumns();
        }
    }

    if (ImGui::CollapsingHeader("Draw Tag", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (beginColumns("custom_buffer_drawtag_columns")) {
            renderFloat("materialTag", drawTag.materialTag, previousDrawTag.materialTag, 0.5f);
            renderFloat("isHead",      drawTag.isHead,      previousDrawTag.isHead,      0.5f);
            endColumns();
        }
    }

    previousData = data;
    previousDrawTag = drawTag;
    hasPreviousData = true;

    ImGui::End();
}
// Lock the current shader list in the UI to prevent it from changing
void UILockShaderList_Internal() {
    std::shared_lock lock(g_ShaderDB.mutex);
    g_lockedShaderKeys.clear();
    g_lockedShaderKeys.reserve(g_ShaderDB.entries.size());
    for (auto& [shaderKey, entry] : g_ShaderDB.entries) {
        ShaderDefinition* def = entry.GetMatchedDefinition();
        // Snapshot **what is currently visible** in the UI
        if (entry.IsMatched() && entry.IsRecentlyUsed() && def) {
            g_lockedShaderKeys.push_back(shaderKey);
        }
    }
}
void UIUnlockShaderList_Internal() {
    g_shaderListLocked = false;
    g_lockedShaderKeys.clear();
}

struct SHRows
{
    DirectX::XMFLOAT4 r;
    DirectX::XMFLOAT4 g;
    DirectX::XMFLOAT4 b;
};

static DirectX::XMFLOAT3 Mul3x3(const float m[3][3], DirectX::XMFLOAT3 v)
{
    return {
        m[0][0] * v.x + m[0][1] * v.y + m[0][2] * v.z,
        m[1][0] * v.x + m[1][1] * v.y + m[1][2] * v.z,
        m[2][0] * v.x + m[2][1] * v.y + m[2][2] * v.z
    };
}

static DirectX::XMFLOAT4 PackChannel(
    float xp, float xn,
    float yp, float yn,
    float zp, float zn,
    const float skyToNormalBasis[3][3])
{
    // No pow here.
    DirectX::XMFLOAT3 gradientSky{
        (xp - xn) * 0.5f,
        (yp - yn) * 0.5f,
        (zp - zn) * 0.5f
    };

    DirectX::XMFLOAT3 gradientNormal =
        Mul3x3(skyToNormalBasis, gradientSky);

    float mean = (xp + xn + yp + yn + zp + zn) * (1.0f / 6.0f);

    return {
        gradientNormal.x,
        gradientNormal.y,
        gradientNormal.z,
        mean
    };
}

// Update the custom buffer for shaders
void UpdateCustomBuffer_Internal() {
    // Fill the custom buffer data structure with current frame info
    static LARGE_INTEGER frequency = {};
    static LARGE_INTEGER lastFrameTime = {};
    static LARGE_INTEGER startTime = {};
    static bool initialized = false;
    static bool firstFrame = true; // Flag to initialize smoothedFPS properly
    static float smoothedFPS = 0.0f;
    static uint32_t frameCounter = 0;
    // Initialize timing on first call
    if (!initialized) {
        QueryPerformanceFrequency(&frequency);
        QueryPerformanceCounter(&startTime);
        lastFrameTime = startTime;
        initialized = true;
    }
    // Get current time
    LARGE_INTEGER currentTime;
    QueryPerformanceCounter(&currentTime);
    // Calculate delta time
    float deltaTime = static_cast<float>(currentTime.QuadPart - lastFrameTime.QuadPart) / static_cast<float>(frequency.QuadPart);
    lastFrameTime = currentTime;
    // Calculate total elapsed time
    float totalTime = static_cast<float>(currentTime.QuadPart - startTime.QuadPart) / static_cast<float>(frequency.QuadPart);
    // Calculate Instant FPS
    // We use a small epsilon (0.0001) to prevent any potential division by zero
    float instantFPS = (deltaTime > 0.0001f) ? (1.0f / deltaTime) : 0.0f;
    // Smooth the FPS
    if (firstFrame && instantFPS > 0.0f) {
        // Use Instant FPS at start
        smoothedFPS = instantFPS;
        firstFrame = false;
    } else {
        // Standard exponential moving average for all subsequent frames
        smoothedFPS = smoothedFPS * 0.95f + instantFPS * 0.05f;
    }
    // Get screen resolution from the main render target (kMain = 3).
    float resX = 1920.0f, resY = 1080.0f;
    if (g_rendererData->renderTargets[RT::idx(RT::Color::kMain)].texture) {
        REX::W32::D3D11_TEXTURE2D_DESC desc{};
        g_rendererData->renderTargets[RT::idx(RT::Color::kMain)].texture->GetDesc(&desc);
        resX = static_cast<float>(desc.width);
        resY = static_cast<float>(desc.height);
    }
    // Get mouse position (normalized to 0.0 - 1.0)
    POINT mousePos{};
    GetCursorPos(&mousePos);
    ScreenToClient(GetActiveWindow(), &mousePos);
    float mousePosX = static_cast<float>(mousePos.x) / resX;
    float mousePosY = static_cast<float>(mousePos.y) / resY;
    // Get the viewport data to extract the camera position and forward vector
    auto gfxState = RE::BSGraphics::State::GetSingleton();
    auto& camState = *SelectGameplayCameraState(gfxState); // CameraStateData
    auto& camView  = camState.camViewData;        // viewMat, viewDir, viewUp, viewRight, viewPort
    // viewport
    auto vp = camView.viewPort;
    float vpX = vp.left, vpY = vp.top;
    float vpW = vp.right - vp.left, vpH = vp.bottom - vp.top;
    // forward vector -> yaw/pitch
    auto vd = camView.viewDir;
    float vx = vd.m128_f32[0], vy = vd.m128_f32[1], vz = vd.m128_f32[2];
    // world-space camera position from the inverse game view matrix
    auto& VM = camView.viewMat; // __m128 viewMat[4]
    DirectX::XMMATRIX view = DirectX::XMMATRIX(VM[0], VM[1], VM[2], VM[3]);
    DirectX::XMMATRIX invView = DirectX::XMMatrixInverse(nullptr, view);
    float camX = DirectX::XMVectorGetX(invView.r[3]);
    float camY = DirectX::XMVectorGetY(invView.r[3]);
    float camZ = DirectX::XMVectorGetZ(invView.r[3]);
    if (!g_player)
        g_player = RE::PlayerCharacter::GetSingleton();
    if (!g_actorValueInfo)
        g_actorValueInfo = RE::ActorValue::GetSingleton();
    if (!g_sky)
        g_sky = RE::Sky::GetSingleton();
    // Slower updates every 30 frames for expensive queries
    if (++g_frameTick >= 30) {
        g_frameTick = 0;
        // Get the player health percentage, clamped to [0,1]
        if (g_player)
            g_healthPerc = std::clamp(g_player->extraList->GetHealthPerc(), 0.0f, 1.0f);
        // Get the current incoming radiation damage. Existing accumulated rads are
        // baseline state and should not drive the shader after loading a save.
        float rawRad = 0.0f;
        if (g_player && g_actorValueInfo) {
            rawRad = g_player->GetActorValue(*g_actorValueInfo->rads);
        }
        if (!g_haveRadBaseline) {
            g_lastRad = rawRad;
            g_radDmg = 0.0f;
            g_haveRadBaseline = true;
        } else {
            const float diffRad = rawRad - g_lastRad;
            g_lastRad = rawRad;
            if (g_player && g_player->IsTakingRadDamageFromActiveEffect() && diffRad > 0.0f) {
                g_radDmg = diffRad;
            } else {
                // Decay radiation so the effect fades when the active damage stops.
                g_radDmg = (std::max)(g_radDmg - 0.1f, 0.0f);
            }
        }
        // Wind data from the sky (for foliage shaders)
        if (g_sky) {
            g_windSpeed = g_sky->windSpeed;
            g_windAngle = g_sky->windAngle;
            g_windTurbulence = g_sky->windTurbulence;
        }
        // Check if the player is in combat
        if (g_player)
            g_inCombat = g_player->IsInCombat();
        // Check if the current Cell is an interior or exterior for sky-related shader effects
        if (g_player && g_player->GetParentCell()) {
            g_inInterior = g_player->GetParentCell()->IsInterior();
        }
    }
    // Get the Projection Inverse matrix to extract the camera FOV
    auto& PM = camView.projMat; // __m128 projMat[4]
    DirectX::XMMATRIX proj = DirectX::XMMATRIX(PM[0], PM[1], PM[2], PM[3]);   // load into XMMATRIX
    DirectX::XMMATRIX invProj = DirectX::XMMatrixInverse(nullptr, proj);
    // Use the game-provided view-projection matrix directly.
    auto& VPM = camView.viewProjMat;
    DirectX::XMMATRIX viewProj = DirectX::XMMATRIX(VPM[0], VPM[1], VPM[2], VPM[3]);
    float timeOfDay = 0.0f;
    float weatherTransition = 0.0f;
    uint32_t currentWeatherID = 0;
    uint32_t outgoingWeatherID = 0;
    uint32_t currentLocationID = 0;
    uint32_t worldSpaceID = 0;
    uint32_t skyMode = 0;
    int32_t currentWeatherClass = -1;
    int32_t outgoingWeatherClass = -1;
    auto* parentCell = g_player ? g_player->GetParentCell() : nullptr;
    DirectX::XMFLOAT3 sunColor(1.0f, 1.0f, 1.0f);
    DirectX::XMFLOAT3 sunDir(0.0f, 0.0f, 0.0f);
    float sunValid = 0.0f;
    if (g_sky) {
        timeOfDay = g_sky->currentGameHour;
        skyMode = g_sky->mode.underlying();
        const bool validInterior = parentCell && parentCell->IsInterior() && parentCell->lightingTemplate;
        if (validInterior) {
            weatherTransition = g_sky->lightingTransition == 0.0f ? 1.0f : g_sky->lightingTransition;
            currentWeatherID = parentCell->lightingTemplate->GetFormID();
            outgoingWeatherID = 0;
        } else {
            weatherTransition = g_sky->currentWeatherPct;
            if (g_sky->currentWeather) {
                currentWeatherID = g_sky->currentWeather->GetFormID();
                currentWeatherClass = GetWeatherClassification(g_sky->currentWeather);
            }
            if (g_sky->lastWeather) {
                outgoingWeatherID = g_sky->lastWeather->GetFormID();
                outgoingWeatherClass = GetWeatherClassification(g_sky->lastWeather);
            }
        }
    }
    if (parentCell && parentCell->IsInterior()) {
        if (const auto* interiorData = GetInteriorDataForDominantLight(parentCell)) {
            sunColor = ColorFromPackedRGB(interiorData->directional);
            sunDir = DirectionFromInteriorAngles(interiorData->directionalXY, interiorData->directionalZ);
            sunValid = std::clamp(interiorData->directionalFade, 0.0f, 1.0f);
        }
    } else if (g_sky) {
        // Sky::GetSunLightColor is a trivial return of Sky+0x0D8, i.e.
        // skyColor[4]. Use the already-blended Sky value instead of sampling
        // TESWeather colorData manually.
        const auto& c = g_sky->skyColor[4];
        sunColor = DirectX::XMFLOAT3(c.r, c.g, c.b);
        if (g_sky->sun && g_sky->sun->light) {
            sunDir = DirectionFromNodeForward(reinterpret_cast<const RE::NiAVObject*>(g_sky->sun->light.get()));
        }
        sunValid = 1.0f;
    }
    if (g_player) {
        if (g_player->currentLocation) {
            currentLocationID = g_player->currentLocation->GetFormID();
        }
        if (g_player->cachedWorldspace) {
            worldSpaceID = g_player->cachedWorldspace->GetFormID();
        }
    }
    // Get a random number each frame
    float randomValue = static_cast<float>(rand()) / static_cast<float>(RAND_MAX);
    // Fill the custom buffer data structure
    g_customBufferData.time     = totalTime;
    g_customBufferData.delta    = deltaTime;
    g_customBufferData.dayCycle = timeOfDay / 24.0f;
    g_customBufferData.frame    = static_cast<float>(frameCounter++);
    g_customBufferData.fps      = smoothedFPS;
    g_customBufferData.resX     = resX;
    g_customBufferData.resY     = resY;
    g_customBufferData.mouseX   = static_cast<float>(mousePos.x) / resX;
    g_customBufferData.mouseY   = static_cast<float>(mousePos.y) / resY;
    g_customBufferData.windSpeed = g_windSpeed;
    g_customBufferData.windAngle = g_windAngle;
    g_customBufferData.windTurb  = g_windTurbulence;
    g_customBufferData.vpLeft   = vp.left;
    g_customBufferData.vpTop    = vp.top;
    g_customBufferData.vpWidth  = vp.right - vp.left;
    g_customBufferData.vpHeight = vp.bottom - vp.top;
    g_customBufferData.camX     = camX;
    g_customBufferData.camY     = camY;
    g_customBufferData.camZ     = camZ;
    g_customBufferData.pRadDmg  = g_radDmg;
    g_customBufferData.viewDirX = vd.m128_f32[0];
    g_customBufferData.viewDirY = vd.m128_f32[1];
    g_customBufferData.viewDirZ = vd.m128_f32[2];
    g_customBufferData.pHealthPerc = g_healthPerc;
    g_customBufferData.g_SunR = sunColor.x;
    g_customBufferData.g_SunG = sunColor.y;
    g_customBufferData.g_SunB = sunColor.z;
    g_customBufferData.g_SunDirX = sunDir.x;
    g_customBufferData.g_SunDirY = sunDir.y;
    g_customBufferData.g_SunDirZ = sunDir.z;
    g_customBufferData.g_SunValid = sunValid;
    g_customBufferData.g_SunPadding = 0.0f;

    auto Normalize3 = [](DirectX::XMFLOAT3 v) {
        const float lenSq = v.x * v.x + v.y * v.y + v.z * v.z;
        if (lenSq <= 1.0e-8f) {
            return DirectX::XMFLOAT3(0.0f, 0.0f, 0.0f);
        }

        const float invLen = 1.0f / std::sqrt(lenSq);
        return DirectX::XMFLOAT3(v.x * invLen, v.y * invLen, v.z * invLen);
    };

    auto FromXM = [](DirectX::XMVECTOR v) {
        return DirectX::XMFLOAT3(
            DirectX::XMVectorGetX(v),
            DirectX::XMVectorGetY(v),
            DirectX::XMVectorGetZ(v));
    };

    // View-space L1 SH packing — derived basis matching engine's cb2[6..8].
    //
    // Empirically reverse-engineered from CAEE89E9/B9652BF6 capture data
    // across 5+ camera orientations: cb2[6..8] = M @ (dac_diff/2) where
    //   M[0] = normalize(world_up × camera_forward)
    //   M[1] = -normalize(camera_forward × M[0])
    //   M[2] = -camera_forward
    // Plus cb2.w = mean of all 6 dac values (DC term).
    //
    // Verified bit-exact (err < 1e-6) for the engine's own cb2[6..8] given
    // current camera basis. See AmbientLightFix.md for derivation.
    //
    // KNOWN ISSUE: per-pixel output still drifts visibly with camera
    // rotation despite matching engine's cb2 packing. The dot product
    // should be rotation-invariant when both cb2 and gbuffer normal
    // rotate together — but we have evidence the gbuffer normal in
    // Fallout 4 is in WORLD space (OG's existing math requires this for
    // cascade shadows + world sun direction). Engine view-space cb2 ×
    // world-space N gives camera-dependent results. This contradicts the
    // engine's B9652BF6 producing stable output, suggesting some part of
    // our understanding is incomplete (possibly: gbuffer normal IS view-
    // space but OG transforms it implicitly through some path we haven't
    // identified, OR engine has a separate cb2[1] convention per shader).
    DirectX::XMFLOAT3 cameraForwardWorld = Normalize3(FromXM(camView.viewDir));
    DirectX::XMFLOAT3 worldUp(0.0f, 0.0f, 1.0f);
    DirectX::XMFLOAT3 row0, row1, row2;
    {
        DirectX::XMFLOAT3 cross0(
            worldUp.y * cameraForwardWorld.z - worldUp.z * cameraForwardWorld.y,
            worldUp.z * cameraForwardWorld.x - worldUp.x * cameraForwardWorld.z,
            worldUp.x * cameraForwardWorld.y - worldUp.y * cameraForwardWorld.x);
        float cross0Len = std::sqrt(cross0.x * cross0.x + cross0.y * cross0.y + cross0.z * cross0.z);
        if (cross0Len > 1e-4f) {
            row0 = DirectX::XMFLOAT3(cross0.x / cross0Len, cross0.y / cross0Len, cross0.z / cross0Len);
        } else {
            row0 = DirectX::XMFLOAT3(1.0f, 0.0f, 0.0f);
        }
        DirectX::XMFLOAT3 cross1(
            cameraForwardWorld.y * row0.z - cameraForwardWorld.z * row0.y,
            cameraForwardWorld.z * row0.x - cameraForwardWorld.x * row0.z,
            cameraForwardWorld.x * row0.y - cameraForwardWorld.y * row0.x);
        float cross1Len = std::sqrt(cross1.x * cross1.x + cross1.y * cross1.y + cross1.z * cross1.z);
        if (cross1Len > 1e-6f) {
            row1 = DirectX::XMFLOAT3(-cross1.x / cross1Len, -cross1.y / cross1Len, -cross1.z / cross1Len);
        } else {
            row1 = DirectX::XMFLOAT3(0.0f, 0.0f, -1.0f);
        }
        row2 = DirectX::XMFLOAT3(-cameraForwardWorld.x, -cameraForwardWorld.y, -cameraForwardWorld.z);
    }
    float skyToNormalBasis[3][3] = {
        { row0.x, row0.y, row0.z },
        { row1.x, row1.y, row1.z },
        { row2.x, row2.y, row2.z },
    };
    DirectX::XMFLOAT4 shR(0.0f, 0.0f, 0.0f, 0.0f);
    DirectX::XMFLOAT4 shG(0.0f, 0.0f, 0.0f, 0.0f);
    DirectX::XMFLOAT4 shB(0.0f, 0.0f, 0.0f, 0.0f);
    if (g_sky) {
        const auto& dac = g_sky->directionalAmbientColorsA;

        shR = PackChannel(
            dac[0][0].r, dac[0][1].r,
            dac[1][0].r, dac[1][1].r,
            dac[2][0].r, dac[2][1].r,
            skyToNormalBasis);

        shG = PackChannel(
            dac[0][0].g, dac[0][1].g,
            dac[1][0].g, dac[1][1].g,
            dac[2][0].g, dac[2][1].g,
            skyToNormalBasis);

        shB = PackChannel(
            dac[0][0].b, dac[0][1].b,
            dac[1][0].b, dac[1][1].b,
            dac[2][0].b, dac[2][1].b,
            skyToNormalBasis);
    }
    g_customBufferData.g_SH_R = shR;
    g_customBufferData.g_SH_G = shG;
    g_customBufferData.g_SH_B = shB;

    DirectX::XMStoreFloat4(&g_customBufferData.g_InvProjRow0, invProj.r[0]);
    DirectX::XMStoreFloat4(&g_customBufferData.g_InvProjRow1, invProj.r[1]);
    DirectX::XMStoreFloat4(&g_customBufferData.g_InvProjRow2, invProj.r[2]);
    DirectX::XMStoreFloat4(&g_customBufferData.g_InvProjRow3, invProj.r[3]);
    DirectX::XMStoreFloat4(&g_customBufferData.g_InvViewRow0, invView.r[0]);
    DirectX::XMStoreFloat4(&g_customBufferData.g_InvViewRow1, invView.r[1]);
    DirectX::XMStoreFloat4(&g_customBufferData.g_InvViewRow2, invView.r[2]);
    DirectX::XMStoreFloat4(&g_customBufferData.g_InvViewRow3, invView.r[3]);
    g_customBufferData.random  = randomValue;
    g_customBufferData.inCombat = g_inCombat ? 1.0f : 0.0f;
    g_customBufferData.inInterior = g_inInterior ? 1.0f : 0.0f;
    g_customBufferData._padding = 0.0f; // just in case, to avoid any potential uninitialized data issues in shaders
    // Snapshot the previous frame's ViewProj BEFORE writing the new one. The
    // very first frame snapshots zeros (CB is zero-initialized), which the
    // shader detects via the all-zero matrix and falls back to non-temporal.
    g_customBufferData.g_PrevViewProjRow0 = g_customBufferData.g_ViewProjRow0;
    g_customBufferData.g_PrevViewProjRow1 = g_customBufferData.g_ViewProjRow1;
    g_customBufferData.g_PrevViewProjRow2 = g_customBufferData.g_ViewProjRow2;
    g_customBufferData.g_PrevViewProjRow3 = g_customBufferData.g_ViewProjRow3;
    DirectX::XMStoreFloat4(&g_customBufferData.g_ViewProjRow0, viewProj.r[0]);
    DirectX::XMStoreFloat4(&g_customBufferData.g_ViewProjRow1, viewProj.r[1]);
    DirectX::XMStoreFloat4(&g_customBufferData.g_ViewProjRow2, viewProj.r[2]);
    DirectX::XMStoreFloat4(&g_customBufferData.g_ViewProjRow3, viewProj.r[3]);
    g_customBufferData.timeOfDay = timeOfDay;
    g_customBufferData.weatherTransition = weatherTransition;
    g_customBufferData.currentWeatherID = currentWeatherID;
    g_customBufferData.outgoingWeatherID = outgoingWeatherID;
    g_customBufferData.currentLocationID = currentLocationID;
    g_customBufferData.worldSpaceID = worldSpaceID;
    g_customBufferData.skyMode = skyMode;
    g_customBufferData.currentWeatherClass = currentWeatherClass;
    g_customBufferData.outgoingWeatherClass = outgoingWeatherClass;
    g_customBufferData.enbPadding0 = 0.0f;
    g_customBufferData.enbPadding1 = 0.0f;
    g_customBufferData.enbPadding2 = 0.0f;
    if (auto* playerCamera = RE::PlayerCamera::GetSingleton(); playerCamera && playerCamera->cameraRoot) {
        const auto* cameraNode = playerCamera->cameraRoot.get();
        StoreCameraTransform(cameraNode->local, g_customBufferData.cameraLocalRow0, g_customBufferData.cameraLocalRow1, g_customBufferData.cameraLocalRow2, g_customBufferData.cameraLocalRow3);
        StoreCameraTransform(cameraNode->world, g_customBufferData.cameraWorldRow0, g_customBufferData.cameraWorldRow1, g_customBufferData.cameraWorldRow2, g_customBufferData.cameraWorldRow3);
        StoreCameraTransform(cameraNode->previousWorld, g_customBufferData.cameraPreviousWorldRow0, g_customBufferData.cameraPreviousWorldRow1, g_customBufferData.cameraPreviousWorldRow2, g_customBufferData.cameraPreviousWorldRow3);
    } else {
        StoreCameraTransform(RE::NiTransform::IDENTITY, g_customBufferData.cameraLocalRow0, g_customBufferData.cameraLocalRow1, g_customBufferData.cameraLocalRow2, g_customBufferData.cameraLocalRow3);
        StoreCameraTransform(RE::NiTransform::IDENTITY, g_customBufferData.cameraWorldRow0, g_customBufferData.cameraWorldRow1, g_customBufferData.cameraWorldRow2, g_customBufferData.cameraWorldRow3);
        StoreCameraTransform(RE::NiTransform::IDENTITY, g_customBufferData.cameraPreviousWorldRow0, g_customBufferData.cameraPreviousWorldRow1, g_customBufferData.cameraPreviousWorldRow2, g_customBufferData.cameraPreviousWorldRow3);
    }
    if (g_sky) {
        g_customBufferData.g_FogDistances0 = DirectX::XMFLOAT4(
            g_sky->fogDistances[static_cast<size_t>(RE::Sky::FogDistance::kNear)],
            g_sky->fogDistances[static_cast<size_t>(RE::Sky::FogDistance::kFar)],
            g_sky->fogDistances[static_cast<size_t>(RE::Sky::FogDistance::kWaterNear)],
            g_sky->fogDistances[static_cast<size_t>(RE::Sky::FogDistance::kWaterFar)]);
        g_customBufferData.g_FogDistances1 = DirectX::XMFLOAT4(
            g_sky->fogDistances[static_cast<size_t>(RE::Sky::FogDistance::kHeightMid)],
            g_sky->fogDistances[static_cast<size_t>(RE::Sky::FogDistance::kHeightRange)],
            g_sky->fogDistances[static_cast<size_t>(RE::Sky::FogDistance::kFarHeightMid)],
            g_sky->fogDistances[static_cast<size_t>(RE::Sky::FogDistance::kFarHeightRange)]);
        g_customBufferData.g_FogParams = DirectX::XMFLOAT4(
            g_sky->fogHeight,
            g_sky->fogPower,
            g_sky->fogClamp,
            g_sky->fogHighDensityScale);
    } else {
        g_customBufferData.g_FogDistances0 = DirectX::XMFLOAT4(0.0f, 0.0f, 0.0f, 0.0f);
        g_customBufferData.g_FogDistances1 = DirectX::XMFLOAT4(0.0f, 0.0f, 0.0f, 0.0f);
        g_customBufferData.g_FogParams     = DirectX::XMFLOAT4(0.0f, 0.0f, 0.0f, 0.0f);
    }
    // Per-weather blended fog color requires sampling currentWeather + lastWeather
    // colorData[][] arrays at the current time-of-day and blending by
    // currentWeatherPct. Left at zero for now -- shaders should treat (0,0,0,0)
    // as "no engine-supplied color, use Values.ini knobs".
    g_customBufferData.g_FogColor = DirectX::XMFLOAT4(0.0f, 0.0f, 0.0f, 0.0f);
    // Pack Values.ini shader settings into separate 16-byte elements to avoid the structured-buffer stride limit.
    g_modularFloatData.assign(PackedElementCount(g_shaderSettings.GetFloatShaderValues().size()), {});
    g_modularIntData.assign(PackedElementCount(g_shaderSettings.GetIntShaderValues().size()), {});
    g_modularBoolData.assign(PackedElementCount(g_shaderSettings.GetBoolShaderValues().size()), {});

    auto setFloatComponent = [](ModularFloat4& item, uint32_t component, float value) {
        switch (component) {
        case 0: item.x = value; break;
        case 1: item.y = value; break;
        case 2: item.z = value; break;
        case 3: item.w = value; break;
        }
    };
    auto setIntComponent = [](ModularInt4& item, uint32_t component, int32_t value) {
        switch (component) {
        case 0: item.x = value; break;
        case 1: item.y = value; break;
        case 2: item.z = value; break;
        case 3: item.w = value; break;
        }
    };
    auto setUIntComponent = [](ModularUInt4& item, uint32_t component, uint32_t value) {
        switch (component) {
        case 0: item.x = value; break;
        case 1: item.y = value; break;
        case 2: item.z = value; break;
        case 3: item.w = value; break;
        }
    };

    for (auto* sv : g_shaderSettings.GetFloatShaderValues()) {
        if (!sv) {
            continue;
        }
        const uint32_t element = sv->bufferIndex / 4;
        if (element < g_modularFloatData.size()) {
            setFloatComponent(g_modularFloatData[element], sv->bufferIndex % 4, sv->current.f);
        }
    }
    for (auto* sv : g_shaderSettings.GetIntShaderValues()) {
        if (!sv) {
            continue;
        }
        const uint32_t element = sv->bufferIndex / 4;
        if (element < g_modularIntData.size()) {
            setIntComponent(g_modularIntData[element], sv->bufferIndex % 4, sv->current.i);
        }
    }
    for (auto* sv : g_shaderSettings.GetBoolShaderValues()) {
        if (!sv) {
            continue;
        }
        const uint32_t element = sv->bufferIndex / 4;
        if (element < g_modularBoolData.size()) {
            setUIntComponent(g_modularBoolData[element], sv->bufferIndex % 4, sv->current.b ? 1u : 0u);
        }
    }
    // Create or update our custom buffer resource view with the new data
    if (!g_rendererData) {
        g_rendererData = RE::BSGraphics::GetRendererData();
        if (!g_rendererData) {
            REX::WARN("UpdateCustomBuffer_Internal: Cannot update custom buffer: renderer data not ready");
            return;
        }
    }
    auto* device = g_rendererData->device;
    if (!g_customSRVBuffer && device) {
        REX::W32::D3D11_BUFFER_DESC desc{};
        desc.usage            = REX::W32::D3D11_USAGE_DYNAMIC;
        desc.byteWidth        = sizeof(GFXBoosterAccessData);
        desc.bindFlags        = REX::W32::D3D11_BIND_SHADER_RESOURCE;
        desc.cpuAccessFlags   = REX::W32::D3D11_CPU_ACCESS_WRITE;
        desc.miscFlags        = REX::W32::D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        desc.structureByteStride = sizeof(GFXBoosterAccessData);
        HRESULT hr = device->CreateBuffer(&desc, nullptr, &g_customSRVBuffer);
        if (FAILED(hr)) {
            REX::WARN("UpdateCustomBuffer_Internal: Failed to create custom buffer. HRESULT: 0x{:08X}", hr);
            return;
        }
    }
    if (g_customSRVBuffer && !g_customSRV && device) {
        REX::W32::D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.format                    = REX::W32::DXGI_FORMAT_UNKNOWN;
        srvDesc.viewDimension             = REX::W32::D3D11_SRV_DIMENSION_BUFFER;
        srvDesc.buffer.firstElement       = 0;
        srvDesc.buffer.numElements        = 1;
        //srvDesc.buffer.elementWidth       = sizeof(GFXBoosterAccessData); // needs to be commented or the SRV creation fails!!!111
        HRESULT hr = device->CreateShaderResourceView(g_customSRVBuffer, &srvDesc, &g_customSRV);
        if (FAILED(hr)) {
            REX::WARN("UpdateCustomBuffer_Internal: Failed to create custom SRV. HRESULT: 0x{:08X}", hr);
            return;
        }
    }
    if (device) {
        EnsureStructuredSRV<ModularFloat4>(
            device,
            g_modularFloatsSRVBuffer,
            g_modularFloatsSRV,
            g_modularFloatElementCount,
            static_cast<UINT>(g_modularFloatData.size()),
            "modular floats");
        EnsureStructuredSRV<ModularInt4>(
            device,
            g_modularIntsSRVBuffer,
            g_modularIntsSRV,
            g_modularIntElementCount,
            static_cast<UINT>(g_modularIntData.size()),
            "modular ints");
        EnsureStructuredSRV<ModularUInt4>(
            device,
            g_modularBoolsSRVBuffer,
            g_modularBoolsSRV,
            g_modularBoolElementCount,
            static_cast<UINT>(g_modularBoolData.size()),
            "modular bools");
    }
    auto* context = g_rendererData->context;
    if (g_customSRVBuffer && context) {
        REX::W32::D3D11_MAPPED_SUBRESOURCE m;
        context->Map(g_customSRVBuffer,0,REX::W32::D3D11_MAP_WRITE_DISCARD,0,&m);
        memcpy(m.data, &g_customBufferData, sizeof(g_customBufferData));
        context->Unmap(g_customSRVBuffer,0);
    }
    UpdateStructuredSRV(context, g_modularFloatsSRVBuffer, g_modularFloatData);
    UpdateStructuredSRV(context, g_modularIntsSRVBuffer, g_modularIntData);
    UpdateStructuredSRV(context, g_modularBoolsSRVBuffer, g_modularBoolData);
}

// This is called at GameData ready to set up our hooks on the graphics device and context
bool InstallGFXHooks_Internal() {
    if (!g_rendererData) {
        g_rendererData = RE::BSGraphics::GetRendererData();
    }
    if (!g_rendererData || !g_rendererData->device) {
        REX::WARN("InstallGFXHooks_Internal: Cannot install hook: device not ready");
        return false;
    }
    REX::W32::ID3D11Device* device = g_rendererData->device;
    REX::W32::ID3D11DeviceContext* context = g_rendererData->context;
    DWORD oldProtect;
    auto* contextVTable = *reinterpret_cast<void***>(g_rendererData->context);
    // Hook ID3D11DeviceContext::DrawIndexed (vtable index 12)
    OriginalDrawIndexed = reinterpret_cast<DrawIndexed_t>(contextVTable[12]);
    if (!VirtualProtect(&contextVTable[12], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) {
        REX::WARN("InstallGFXHooks_Internal: VirtualProtect failed for DrawIndexed");
        return false;
    }
    contextVTable[12] = reinterpret_cast<void*>(MyDrawIndexed);
    VirtualProtect(&contextVTable[12], sizeof(void*), oldProtect, &oldProtect);
    REX::INFO("InstallGFXHooks_Internal: DrawIndexed hook installed");
    // Hook ID3D11DeviceContext::Draw (vtable index 13)
    OriginalDraw = reinterpret_cast<Draw_t>(contextVTable[13]);
    if (!VirtualProtect(&contextVTable[13], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) {
        REX::WARN("InstallGFXHooks_Internal: VirtualProtect failed for Draw");
        return false;
    }
    contextVTable[13] = reinterpret_cast<void*>(MyDraw);
    VirtualProtect(&contextVTable[13], sizeof(void*), oldProtect, &oldProtect);
    REX::INFO("InstallGFXHooks_Internal: Draw hook installed");
    // Hook ID3D11DeviceContext::DrawIndexedInstanced (vtable index 20)
    OriginalDrawIndexedInstanced = reinterpret_cast<DrawIndexedInstanced_t>(contextVTable[20]);
    if (!VirtualProtect(&contextVTable[20], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) {
        REX::WARN("InstallGFXHooks_Internal: VirtualProtect failed for DrawIndexedInstanced");
        return false;
    }
    contextVTable[20] = reinterpret_cast<void*>(MyDrawIndexedInstanced);
    VirtualProtect(&contextVTable[20], sizeof(void*), oldProtect, &oldProtect);
    REX::INFO("InstallGFXHooks_Internal: DrawIndexedInstanced hook installed");
    // Hook ID3D11DeviceContext::DrawInstanced (vtable index 21)
    OriginalDrawInstanced = reinterpret_cast<DrawInstanced_t>(contextVTable[21]);
    if (!VirtualProtect(&contextVTable[21], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) {
        REX::WARN("InstallGFXHooks_Internal: VirtualProtect failed for DrawInstanced");
        return false;
    }
    contextVTable[21] = reinterpret_cast<void*>(MyDrawInstanced);
    VirtualProtect(&contextVTable[21], sizeof(void*), oldProtect, &oldProtect);
    REX::INFO("InstallGFXHooks_Internal: DrawInstanced hook installed");
    // Hook ID3D11DeviceContext::PSSetShaderResources (vtable index 8)
    OriginalPSSetShaderResources = reinterpret_cast<PSSetShaderResources_t>(contextVTable[8]);
    if (!VirtualProtect(&contextVTable[8], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) {
        REX::WARN("InstallGFXHooks_Internal: VirtualProtect failed for PSSetShaderResources");
        return false;
    }
    contextVTable[8] = reinterpret_cast<void*>(MyPSSetShaderResources);
    VirtualProtect(&contextVTable[8], sizeof(void*), oldProtect, &oldProtect);
    REX::INFO("InstallGFXHooks_Internal: PSSetShaderResources hook installed");
    // Hook ID3D11DeviceContext::OMSetRenderTargets (vtable index 33)
    OriginalOMSetRenderTargets = reinterpret_cast<OMSetRenderTargets_t>(contextVTable[33]);
    if (!VirtualProtect(&contextVTable[33], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) {
        REX::WARN("InstallGFXHooks_Internal: VirtualProtect failed for OMSetRenderTargets");
        return false;
    }
    contextVTable[33] = reinterpret_cast<void*>(MyOMSetRenderTargets);
    VirtualProtect(&contextVTable[33], sizeof(void*), oldProtect, &oldProtect);
    REX::INFO("InstallGFXHooks_Internal: OMSetRenderTargets hook installed");
    // Hook ID3D11DeviceContext::ClearDepthStencilView (vtable index 53)
    OriginalClearDepthStencilView = reinterpret_cast<ClearDepthStencilView_t>(contextVTable[53]);
    if (!VirtualProtect(&contextVTable[53], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) {
        REX::WARN("InstallGFXHooks_Internal: VirtualProtect failed for ClearDepthStencilView");
        return false;
    }
    contextVTable[53] = reinterpret_cast<void*>(MyClearDepthStencilView);
    VirtualProtect(&contextVTable[53], sizeof(void*), oldProtect, &oldProtect);
    REX::INFO("InstallGFXHooks_Internal: ClearDepthStencilView hook installed");
    // Hook ID3D11DeviceContext::PSSetShader (vtable index 9)
    OriginalPSSetShader = reinterpret_cast<PSSetShader_t>(contextVTable[9]);
    if (!VirtualProtect(&contextVTable[9], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) {
        REX::WARN("InstallGFXHooks_Internal: VirtualProtect failed for PSSetShader");
        return false;
    }
    contextVTable[9] = reinterpret_cast<void*>(MyPSSetShader);
    VirtualProtect(&contextVTable[9], sizeof(void*), oldProtect, &oldProtect);
    REX::INFO("InstallGFXHooks_Internal: PSSetShader hook installed");
    // Hook ID3D11DeviceContext::VSSetShader (vtable index 11)
    OriginalVSSetShader = reinterpret_cast<VSSetShader_t>(contextVTable[11]);
    if (!VirtualProtect(&contextVTable[11], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) {
        REX::WARN("InstallGFXHooks_Internal: VirtualProtect failed for VSSetShader");
        return false;
    }
    contextVTable[11] = reinterpret_cast<void*>(MyVSSetShader);
    VirtualProtect(&contextVTable[11], sizeof(void*), oldProtect, &oldProtect);
    REX::INFO("InstallGFXHooks_Internal: VSSetShader hook installed");
    // Hook IDXGISwapChain::Present (vtable index 8)
    auto* swapChain = g_rendererData->renderWindow[0].swapChain;  // First window (main)
    auto* swapChainVTable = *reinterpret_cast<void***>(swapChain);
    OriginalPresent = reinterpret_cast<Present_t>(swapChainVTable[8]);
    if (!VirtualProtect(&swapChainVTable[8], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) {
        REX::WARN("InstallGFXHooks_Internal: VirtualProtect failed for Present");
        return false;
    }
    swapChainVTable[8] = reinterpret_cast<void*>(MyPresent);
    VirtualProtect(&swapChainVTable[8], sizeof(void*), oldProtect, &oldProtect);
    REX::INFO("InstallGFXHooks_Internal: Present hook installed");
    REX::INFO("InstallGFXHooks_Internal: All Hooks installed successfully");
    // Set up ImGui if DEVGUI_ON=true
    if (!DEVGUI_ON && !SHADERSETTINGS_ON) {
        REX::INFO("InstallGFXHooks_Internal: DEVGUI_ON and SHADERSETTINGS_ON are false, skipping ImGui initialization");
        return true;
    }
    // All Hooks installed successfully
    // Compile the flash shader used for highlighting matched shaders in the dev GUI
    if (flashPixelShaderHLSL) {
        ID3DBlob* blob = nullptr;
        ID3DBlob* err  = nullptr;
        HRESULT hr = D3DCompile(
            flashPixelShaderHLSL,
            strlen(flashPixelShaderHLSL),
            "flash_ps",
            nullptr,
            nullptr,
            "main", "ps_5_0",
            D3DCOMPILE_OPTIMIZATION_LEVEL3,
            0,
            &blob,
            &err
        );
        if (!REX::W32::SUCCESS(hr)) {
            if (err)
                REX::WARN("Flash shader compile error: {}", static_cast<const char*>(err->GetBufferPointer())); err->Release();
            // return false; // Still continue, it is not essential
        }
        if (blob) {
            g_isCreatingReplacementShader = true;
            hr = g_rendererData->device->CreatePixelShader(
                blob->GetBufferPointer(),
                blob->GetBufferSize(),
                nullptr,
                &g_flashPixelShader
            );
            g_isCreatingReplacementShader = false;
            blob->Release();
            if (!REX::W32::SUCCESS(hr))
                REX::WARN("CreatePixelShader failed for flash shader with HRESULT: 0x{:08X}", hr);
            // return false; // Still continue, it is not essential
        }
    }
    REX::INFO("About to initialize ImGui...");
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    // Disable saving to imgui.ini to use the INI settings
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    HWND hwnd = g_outputWindow ? g_outputWindow : FindWindowA("Fallout4", nullptr);
    REX::INFO("HWND: {}", (void*)hwnd);
    if (!hwnd) {
        REX::WARN("Failed to get game window handle");
        return false;
    }
	::GetWindowRect(hwnd, &g_windowRect);
    g_originalWndProc = (WNDPROC)SetWindowLongPtr(hwnd, GWLP_WNDPROC, (LONG_PTR)ImGuiWndProc);
    // map the END key
    bool endDown = (GetAsyncKeyState(VK_END) & 0x8000) != 0;
    io.AddKeyEvent(ImGuiKey_End, endDown);
    // map the HOME key
    bool homeDown = (GetAsyncKeyState(VK_HOME) & 0x8000) != 0;
    io.AddKeyEvent(ImGuiKey_Home, homeDown);
    // Initialize ImGui for Win32 and DirectX 11
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(
        reinterpret_cast<ID3D11Device*>(g_rendererData->device),
        reinterpret_cast<ID3D11DeviceContext*>(g_rendererData->context)
    );
    REX::INFO("DX11 ImGui initialized");
    g_imguiInitialized = true;
    return true;
}

BOOL __stdcall HookedClipCursor(const RECT* lpRect)
{
    if (g_imguiInitialized && g_showSettings)
        lpRect = &g_windowRect;
    return ClipCursor_Orig(lpRect);
}

HRESULT __stdcall HookedD3D11CreateDeviceAndSwapChain(IDXGIAdapter* a_pAdapter,
    D3D_DRIVER_TYPE a_driverType,
    HMODULE a_software,
    UINT a_flags,
    const D3D_FEATURE_LEVEL* a_pFeatureLevels,
    UINT a_featureLevels,
    UINT a_sdkVersion,
    const DXGI_SWAP_CHAIN_DESC* a_pSwapChainDesc,
    IDXGISwapChain** a_ppSwapChain,
    ID3D11Device** a_ppDevice,
    D3D_FEATURE_LEVEL* a_pFeatureLevel,
    ID3D11DeviceContext** a_ppImmediateContext)

{
    HRESULT res = D3D11CreateDeviceAndSwapChain_Orig(a_pAdapter, a_driverType, a_software, a_flags, a_pFeatureLevels, a_featureLevels, a_sdkVersion, a_pSwapChainDesc, a_ppSwapChain, a_ppDevice, a_pFeatureLevel, a_ppImmediateContext);

    // Capture the actual render window from the swap chain desc.
    // This is more reliable than FindWindowA at ImGui init time.
    if (a_pSwapChainDesc) {
        g_outputWindow = a_pSwapChainDesc->OutputWindow;
    }

    if (!g_rendererData || !g_rendererData->device) {
        g_rendererData = RE::BSGraphics::GetRendererData();
    }
    if (!g_rendererData || !g_rendererData->device) {
        return false;
    }
    auto* deviceVTable = *reinterpret_cast<void***>(g_rendererData->device);
    DWORD oldProtect;
    // Hook ID3D11Device::CreatePixelShader (vtable index 15)
    OriginalCreatePixelShader = reinterpret_cast<CreatePixelShader_t>(deviceVTable[15]);
    VirtualProtect(&deviceVTable[15], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect);
    deviceVTable[15] = reinterpret_cast<void*>(MyCreatePixelShader);
    VirtualProtect(&deviceVTable[15], sizeof(void*), oldProtect, &oldProtect);
    REX::INFO("InstallShaderCreationHooks_Internal: CreatePixelShader hook installed");
    // Hook ID3D11Device::CreateVertexShader (vtable index 12)
    OriginalCreateVertexShader = reinterpret_cast<CreateVertexShader_t>(deviceVTable[12]);
    VirtualProtect(&deviceVTable[12], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect);
    deviceVTable[12] = reinterpret_cast<void*>(MyCreateVertexShader);
    VirtualProtect(&deviceVTable[12], sizeof(void*), oldProtect, &oldProtect);
    REX::INFO("InstallShaderCreationHooks_Internal: CreateVertexShader hook installed");

    return res;
}

// Engine entry to BSLight::TestFrustumCull. Wraps the original with an
// Enter/Leave pair from LightCullPolicy: Enter optionally scales the BSLight's
// bound radius (geometry[+0x138]) before the engine runs its frustum test,
// Leave restores the source field afterwards. The engine internally caches the
// scaled value into geometry[+0xBC] during its run; Leave deliberately does
// not undo that so downstream consumers of the cached cull sphere see the
// boosted reach. See src/LightCullPolicy.cpp for the rationale.
std::uint32_t HookedBSLightTestFrustumCull(void* light, void* cullingProcess)
{
    const float saved = LightCullPolicy::OnTestFrustumCullEnter(light);
    const auto result = OriginalBSLightTestFrustumCull(light, cullingProcess);
    LightCullPolicy::OnTestFrustumCullLeave(light, saved);
    return result;
}

// Engine entry to BSDFTiledLighting::AddLight. Multiplies the radius arg by
// the current active scale before forwarding. The lambda reads geometry[+0x138]
// for this arg AFTER our cull-side restore has run, so the value it passes us
// is vanilla. Scaling here puts the tiled slot in the same regime as the
// per-volume mesh (which gets its own transient scale via the
// SetupPointLightGeometry hook). Both paths now use the same boosted radius
// so non-shadow lights with both paths active match shadow-light coverage.
void HookedBSDFTiledLightingAddLight(std::uint32_t id,
                                     const RE::NiPoint3* pos, float radius,
                                     const RE::NiColor* color,
                                     const RE::NiPoint3* dir,
                                     bool flagA, bool flagB, bool flagC, bool flagD)
{
    float adjusted = radius;
    const float scale = LightCullPolicy::GetCurrentScale();
    if (std::isfinite(scale) && scale > 0.0f && std::abs(scale - 1.0f) >= 1e-4f) {
        adjusted = radius * scale;
    }
    OriginalBSDFTiledLightingAddLight(id, pos, adjusted, color, dir,
                                      flagA, flagB, flagC, flagD);
}

// Engine entry to BSDFLightShader::SetupPointLightGeometry. Transiently scales
// the light's geometry[+0x138] (source bound radius) for the duration of the
// engine's setup so the world-matrix construction inside
// SetupPointLightTransforms sees the boosted radius and builds an expanded
// volume mesh. Restores immediately on return so shadow projection / fade-
// distance / etc. that read +0x138 outside this window see vanilla.
//
// Helper layout: light[+0xB8] is the geometry pointer; geometry[+0x138] is
// the source bound radius. Same field LightCullPolicy::GetBoundRadiusPtr
// targets. Duplicating the offset math here to avoid widening the public
// LightCullPolicy API just for this one consumer.
bool HookedSetupPointLightGeometry(void* shader, void* light, std::uint32_t mask, char isUnk)
{
    float saved = std::numeric_limits<float>::quiet_NaN();
    float* radiusPtr = nullptr;
    if (light) {
        if (auto* geometry = *reinterpret_cast<void**>(static_cast<std::byte*>(light) + 0xB8)) {
            radiusPtr = reinterpret_cast<float*>(static_cast<std::byte*>(geometry) + 0x138);
            const float scale = LightCullPolicy::GetCurrentScale();
            if (std::isfinite(scale) && scale > 0.0f && std::abs(scale - 1.0f) >= 1e-4f) {
                saved = *radiusPtr;
                *radiusPtr = saved * scale;
            }
        }
    }
    const bool result = OriginalSetupPointLightGeometry(shader, light, mask, isUnk);
    if (!std::isnan(saved) && radiusPtr) {
        *radiusPtr = saved;
    }
    return result;
}

// This is called during Plugin load very early
bool InstallShaderCreationHooks_Internal() {
    REX::INFO("Hooking D3D11CreateDeviceAndSwapChain");
    auto* trampoline = F4SE::GetTrampolineInterface();
    std::uintptr_t d3d11Addr = ptr_D3D11CreateDeviceAndSwapChainCall.address();
    if (REX::FModule::GetRuntimeIndex() == REX::FModule::Runtime::kOG)
    {
        d3d11Addr += kCallOffsetOG;
    }
    else
    {
        d3d11Addr += kCallOffsetAE;
    }
    REL::Relocation<uintptr_t> tempAddr { d3d11Addr };
    D3D11CreateDeviceAndSwapChain_Orig = (FnD3D11CreateDeviceAndSwapChain)tempAddr.write_call<5>(&HookedD3D11CreateDeviceAndSwapChain);

    ClipCursor_Orig = *(FnClipCursor*)ptr_ClipCursor.address();
    ptr_ClipCursor.write_vfunc(0, &HookedClipCursor);
    return true;
}

bool InstallDrawTaggingHooks_Internal()
{
    if (OriginalBSLightingShaderSetupGeometry &&
        OriginalBSLightingShaderRestoreGeometry &&
        OriginalBSEffectShaderSetupGeometry &&
        OriginalBSEffectShaderRestoreGeometry &&
        OriginalBSBatchRendererDraw &&
        OriginalBipedAnimAttachSkinnedObject &&
        OriginalBipedAnimAttachBipedWeapon &&
        OriginalBipedAnimAttachToParent &&
        OriginalBipedAnimRemovePart &&
        OriginalActorLoad3D &&
        OriginalPlayerCharacterLoad3D &&
        OriginalActorSet3D &&
        OriginalPlayerCharacterSet3D &&
        OriginalActorOnHeadInitialized &&
        OriginalPlayerCharacterOnHeadInitialized &&
        OriginalUpdate3DModel &&
        OriginalReset3D &&
        OriginalRenderBatches &&
        (REX::FModule::GetRuntimeIndex() != REX::FModule::Runtime::kOG ||
            (OriginalFinishShadowRenderBatches && OriginalFinishShadowRenderGeometryGroup)) &&
        OriginalRenderCommandBufferPassesImpl &&
        OriginalRenderPersistentPassListImpl &&
        (REX::FModule::GetRuntimeIndex() != REX::FModule::Runtime::kOG || OriginalProcessCommandBuffer) &&
        OriginalRegisterObjectShadowMapOrMask &&
        (REX::FModule::GetRuntimeIndex() != REX::FModule::Runtime::kOG || OriginalRegisterPassGeometryGroup) &&
        OriginalRenderPassImpl &&
        OriginalBuildCommandBuffer &&
        OriginalReplaceHeadTaskRun &&
        OriginalBSLightTestFrustumCull &&
        OriginalBSDFTiledLightingAddLight &&
        OriginalSetupPointLightGeometry) {
        return true;
    }

    if (!OriginalBSLightingShaderSetupGeometry || !OriginalBSLightingShaderRestoreGeometry) {
        REL::Relocation<std::uintptr_t> lightingShaderVTable{ RE::VTABLE::BSLightingShader[0] };
        if (!OriginalBSLightingShaderSetupGeometry) {
            OriginalBSLightingShaderSetupGeometry = reinterpret_cast<BSShaderSetupGeometry_t>(
                lightingShaderVTable.write_vfunc(7, reinterpret_cast<void*>(&HookedBSLightingShaderSetupGeometry)));

            if (!OriginalBSLightingShaderSetupGeometry) {
                REX::WARN("InstallDrawTaggingHooks_Internal: Failed to install BSLightingShader::SetupGeometry hook");
                return false;
            }

            REX::INFO("InstallDrawTaggingHooks_Internal: BSLightingShader::SetupGeometry hook installed");
        }

        if (!OriginalBSLightingShaderRestoreGeometry) {
            OriginalBSLightingShaderRestoreGeometry = reinterpret_cast<BSShaderRestoreGeometry_t>(
                lightingShaderVTable.write_vfunc(8, reinterpret_cast<void*>(&HookedBSLightingShaderRestoreGeometry)));

            if (!OriginalBSLightingShaderRestoreGeometry) {
                REX::WARN("InstallDrawTaggingHooks_Internal: Failed to install BSLightingShader::RestoreGeometry hook");
                return false;
            }

            REX::INFO("InstallDrawTaggingHooks_Internal: BSLightingShader::RestoreGeometry hook installed");
        }
    }

    if (!OriginalBSEffectShaderSetupGeometry || !OriginalBSEffectShaderRestoreGeometry) {
        REL::Relocation<std::uintptr_t> effectShaderVTable{ RE::VTABLE::BSEffectShader[0] };
        if (!OriginalBSEffectShaderSetupGeometry) {
            OriginalBSEffectShaderSetupGeometry = reinterpret_cast<BSShaderSetupGeometry_t>(
                effectShaderVTable.write_vfunc(7, reinterpret_cast<void*>(&HookedBSEffectShaderSetupGeometry)));

            if (!OriginalBSEffectShaderSetupGeometry) {
                REX::WARN("InstallDrawTaggingHooks_Internal: Failed to install BSEffectShader::SetupGeometry hook");
                return false;
            }

            REX::INFO("InstallDrawTaggingHooks_Internal: BSEffectShader::SetupGeometry hook installed");
        }

        if (!OriginalBSEffectShaderRestoreGeometry) {
            OriginalBSEffectShaderRestoreGeometry = reinterpret_cast<BSShaderRestoreGeometry_t>(
                effectShaderVTable.write_vfunc(8, reinterpret_cast<void*>(&HookedBSEffectShaderRestoreGeometry)));

            if (!OriginalBSEffectShaderRestoreGeometry) {
                REX::WARN("InstallDrawTaggingHooks_Internal: Failed to install BSEffectShader::RestoreGeometry hook");
                return false;
            }

            REX::INFO("InstallDrawTaggingHooks_Internal: BSEffectShader::RestoreGeometry hook installed");
        }
    }

    if (!OriginalBSBatchRendererDraw) {
        constexpr std::size_t kDrawPrologueSize = 15;
        OriginalBSBatchRendererDraw = CreateBranchGateway<BSBatchRendererDraw_t>(ptr_BSBatchRendererDraw, kDrawPrologueSize, reinterpret_cast<void*>(&HookedBSBatchRendererDraw));

        if (!OriginalBSBatchRendererDraw) {
            REX::WARN("InstallDrawTaggingHooks_Internal: Failed to install BSBatchRenderer::Draw hook");
            return false;
        }

        REX::INFO("InstallDrawTaggingHooks_Internal: BSBatchRenderer::Draw hook installed");
    }

    if (!OriginalRenderBatches) {
        if (ptr_RenderBatches.address() < 0x140000000ull) {
            REX::WARN("InstallDrawTaggingHooks_Internal: BSBatchRenderer::RenderBatches REL::ID failed to resolve");
            return false;
        }
        // IDA OG 0x14287F380: 5 + 4 + 1 + 4 bytes reaches a clean boundary
        // after `sub rsp, 60h`, exactly enough for the JMP14 patch.
        constexpr std::size_t kRenderBatchesPrologueSize = 14;
        OriginalRenderBatches = CreateBranchGateway<RenderBatches_t>(
            ptr_RenderBatches,
            kRenderBatchesPrologueSize,
            reinterpret_cast<void*>(&HookedRenderBatches));

        if (!OriginalRenderBatches) {
            REX::WARN("InstallDrawTaggingHooks_Internal: Failed to install BSBatchRenderer::RenderBatches hook");
            return false;
        }

        REX::INFO("InstallDrawTaggingHooks_Internal: BSBatchRenderer::RenderBatches hook installed");
    }

    auto verifyRel32Call = [](const char* label,
                              std::uintptr_t callSite,
                              std::uintptr_t expectedTarget) -> bool {
        if (callSite < 0x140000000ull || expectedTarget < 0x140000000ull) {
            REX::WARN("InstallDrawTaggingHooks_Internal: {} call site failed to resolve ({:#x}, expected {:#x})",
                      label,
                      callSite,
                      expectedTarget);
            return false;
        }

        auto* callBytes = reinterpret_cast<const std::uint8_t*>(callSite);
        if (callBytes[0] != 0xE8) {
            REX::WARN("InstallDrawTaggingHooks_Internal: {} call site at {:#x} is not a CALL rel32",
                      label,
                      callSite);
            return false;
        }

        std::int32_t rel32 = 0;
        std::memcpy(&rel32, callBytes + 1, sizeof(rel32));
        const std::uintptr_t actualTarget = callSite + 5 + rel32;
        if (actualTarget != expectedTarget) {
            REX::WARN("InstallDrawTaggingHooks_Internal: {} call site {:#x} targets {:#x}, expected {:#x}",
                      label,
                      callSite,
                      actualTarget,
                      expectedTarget);
            return false;
        }

        return true;
    };

    auto patchVerifiedCall = [&verifyRel32Call](const char* label,
                                                std::uintptr_t callSite,
                                                std::uintptr_t expectedTarget,
                                                void* hook,
                                                std::uintptr_t* outOriginal) -> bool {
        if (!verifyRel32Call(label, callSite, expectedTarget)) {
            return false;
        }

        REL::Relocation<std::uintptr_t> callSiteRel{ callSite };
        const auto original = callSiteRel.write_call<5>(reinterpret_cast<std::uintptr_t>(hook));
        if (outOriginal) {
            *outOriginal = original;
        }
        REX::INFO("InstallDrawTaggingHooks_Internal: {} call-site redirect installed at {:#x}",
                  label,
                  callSite);
        return true;
    };

    if (REX::FModule::GetRuntimeIndex() == REX::FModule::Runtime::kOG &&
        (!OriginalFinishShadowRenderBatches || !OriginalFinishShadowRenderGeometryGroup)) {
        if (ptr_FinishAccumulatingShadowMapOrMask.address() < 0x140000000ull ||
            ptr_RenderBatches.address() < 0x140000000ull ||
            ptr_RenderGeometryGroup.address() < 0x140000000ull) {
            REX::WARN("InstallDrawTaggingHooks_Internal: FinishAccumulating_ShadowMapOrMask split call-site dependencies failed to resolve");
            return false;
        }

        const auto finishShadowRenderBatchesCallSite =
            ptr_FinishAccumulatingShadowMapOrMask.address() + kFinishShadowRenderBatchesCallOffsetOG;
        const auto finishShadowRenderGeometryGroupCallSite =
            ptr_FinishAccumulatingShadowMapOrMask.address() + kFinishShadowRenderGeometryGroupCallOffsetOG;
        if (!OriginalFinishShadowRenderBatches &&
            !verifyRel32Call(
                "FinishAccumulating_ShadowMapOrMask -> RenderBatches",
                finishShadowRenderBatchesCallSite,
                ptr_RenderBatches.address())) {
            return false;
        }
        if (!OriginalFinishShadowRenderGeometryGroup &&
            !verifyRel32Call(
                "FinishAccumulating_ShadowMapOrMask -> RenderGeometryGroup",
                finishShadowRenderGeometryGroupCallSite,
                ptr_RenderGeometryGroup.address())) {
            return false;
        }

        if (!OriginalFinishShadowRenderBatches) {
            std::uintptr_t original = 0;
            if (!patchVerifiedCall(
                    "FinishAccumulating_ShadowMapOrMask -> RenderBatches",
                    finishShadowRenderBatchesCallSite,
                    ptr_RenderBatches.address(),
                    reinterpret_cast<void*>(&HookedFinishShadowRenderBatches),
                    &original)) {
                return false;
            }
            OriginalFinishShadowRenderBatches = reinterpret_cast<RenderBatches_t>(original);
        }

        if (!OriginalFinishShadowRenderGeometryGroup) {
            std::uintptr_t original = 0;
            if (!patchVerifiedCall(
                    "FinishAccumulating_ShadowMapOrMask -> RenderGeometryGroup",
                    finishShadowRenderGeometryGroupCallSite,
                    ptr_RenderGeometryGroup.address(),
                    reinterpret_cast<void*>(&HookedFinishShadowRenderGeometryGroup),
                    &original)) {
                return false;
            }
            OriginalFinishShadowRenderGeometryGroup = reinterpret_cast<RenderGeometryGroup_t>(original);
        }
    }

    if (!OriginalRenderCommandBufferPassesImpl) {
        constexpr std::size_t kRenderCmdBufPrologueSize = 15;
        OriginalRenderCommandBufferPassesImpl = CreateBranchGateway<RenderCommandBufferPassesImpl_t>(ptr_RenderCommandBufferPassesImpl, kRenderCmdBufPrologueSize, reinterpret_cast<void*>(&HookedRenderCommandBufferPassesImpl));

        if (!OriginalRenderCommandBufferPassesImpl) {
            REX::WARN("InstallDrawTaggingHooks_Internal: Failed to install BSBatchRenderer::RenderCommandBufferPassesImpl hook");
            return false;
        }

        REX::INFO("InstallDrawTaggingHooks_Internal: BSBatchRenderer::RenderCommandBufferPassesImpl hook installed");
    }

    if (!OriginalRenderPersistentPassListImpl) {
        if (ptr_RenderPersistentPassListImpl.address() < 0x140000000ull) {
            REX::WARN("InstallDrawTaggingHooks_Internal: BSBatchRenderer::RenderPersistentPassListImpl REL::ID failed to resolve");
            return false;
        }

        // Avoid an entry detour here. Ghidra shows the shadow group-9 path as:
        //   BSBatchRenderer::GeometryGroup::Render + call 0x719
        //     -> RenderPersistentPassListImpl(PersistentPassList&, bool)
        // Patch that direct call only, and verify the rel32 target first so a
        // wrong runtime/layout fails closed instead of corrupting the prologue.
        const std::uintptr_t target = ptr_RenderPersistentPassListImpl.address();
        const std::uintptr_t callSite = target + kGeometryGroupRenderPersistentPassListCallOffsetOG;
        auto* callBytes = reinterpret_cast<const std::uint8_t*>(callSite);
        if (callBytes[0] != 0xE8) {
            REX::WARN("InstallDrawTaggingHooks_Internal: GeometryGroup::Render persistent-list call site at {:#x} is not a CALL rel32", callSite);
            return false;
        }

        std::int32_t rel32 = 0;
        std::memcpy(&rel32, callBytes + 1, sizeof(rel32));
        const std::uintptr_t actualTarget = callSite + 5 + rel32;
        if (actualTarget != target) {
            REX::WARN(
                "InstallDrawTaggingHooks_Internal: GeometryGroup::Render persistent-list call site {:#x} targets {:#x}, expected {:#x}",
                callSite,
                actualTarget,
                target);
            return false;
        }

        REL::Relocation<std::uintptr_t> callSiteRel{ callSite };
        OriginalRenderPersistentPassListImpl = reinterpret_cast<RenderPersistentPassListImpl_t>(
            callSiteRel.write_call<5>(reinterpret_cast<std::uintptr_t>(&HookedRenderPersistentPassListImpl)));

        if (!OriginalRenderPersistentPassListImpl) {
            REX::WARN("InstallDrawTaggingHooks_Internal: Failed to redirect GeometryGroup::Render persistent-list call site at {:#x}", callSite);
            return false;
        }

        REX::INFO("InstallDrawTaggingHooks_Internal: GeometryGroup::Render persistent-list call-site redirect installed at {:#x}", callSite);
    }

    if (REX::FModule::GetRuntimeIndex() == REX::FModule::Runtime::kOG && !OriginalProcessCommandBuffer) {
        // 1.10.163 version file: ID 673619 -> 0x141D13A10.
        // Prologue is three 5-byte non-RIP stack spills, so 15 bytes is a
        // clean gateway boundary for the 14-byte absolute jump patch.
        constexpr std::size_t kProcessCommandBufferPrologueSize = 15;
        OriginalProcessCommandBuffer = CreateBranchGateway<ProcessCommandBuffer_t>(
            ptr_ProcessCommandBuffer,
            kProcessCommandBufferPrologueSize,
            reinterpret_cast<void*>(&HookedProcessCommandBuffer));

        if (!OriginalProcessCommandBuffer) {
            REX::WARN("InstallDrawTaggingHooks_Internal: Failed to install BSGraphics::Renderer::ProcessCommandBuffer hook");
            return false;
        }

        REX::INFO("InstallDrawTaggingHooks_Internal: BSGraphics::Renderer::ProcessCommandBuffer hook installed");
    }

    if (!OriginalRegisterObjectShadowMapOrMask) {
        if (ptr_RegisterObjectShadowMapOrMask.address() < 0x140000000ull) {
            REX::WARN("InstallDrawTaggingHooks_Internal: RegisterObject_ShadowMapOrMask REL::ID failed to resolve");
            return false;
        }

        constexpr std::size_t kRegisterObjectShadowMapOrMaskPrologueSize = 15;
        OriginalRegisterObjectShadowMapOrMask = CreateBranchGateway5<RegisterObjectShadowMapOrMask_t>(
            ptr_RegisterObjectShadowMapOrMask,
            kRegisterObjectShadowMapOrMaskPrologueSize,
            reinterpret_cast<void*>(&HookedRegisterObjectShadowMapOrMask));

        if (!OriginalRegisterObjectShadowMapOrMask) {
            REX::WARN("InstallDrawTaggingHooks_Internal: Failed to install RegisterObject_ShadowMapOrMask hook");
            return false;
        }

        REX::INFO("InstallDrawTaggingHooks_Internal: RegisterObject_ShadowMapOrMask hook installed");
    }

    if (REX::FModule::GetRuntimeIndex() == REX::FModule::Runtime::kOG && !OriginalRegisterPassGeometryGroup) {
        if (ptr_RegisterPassGeometryGroup.address() < 0x140000000ull) {
            REX::WARN("InstallDrawTaggingHooks_Internal: BSBatchRenderer::RegisterPassGeometryGroup REL::ID failed to resolve");
            return false;
        }

        const std::uintptr_t callSite =
            ptr_RegisterObjectShadowMapOrMask.address() + kRegisterObjectShadowMapOrMaskRegisterPassCallOffsetOG;
        auto* callBytes = reinterpret_cast<const std::uint8_t*>(callSite);
        if (callBytes[0] != 0xE8) {
            REX::WARN("InstallDrawTaggingHooks_Internal: RegisterObject_ShadowMapOrMask RegisterPass call site at {:#x} is not a CALL rel32", callSite);
            return false;
        }

        std::int32_t rel32 = 0;
        std::memcpy(&rel32, callBytes + 1, sizeof(rel32));
        const std::uintptr_t actualTarget = callSite + 5 + rel32;
        const std::uintptr_t expectedTarget = ptr_RegisterPassGeometryGroup.address();
        if (actualTarget != expectedTarget) {
            REX::WARN(
                "InstallDrawTaggingHooks_Internal: RegisterObject_ShadowMapOrMask RegisterPass call site {:#x} targets {:#x}, expected {:#x}",
                callSite,
                actualTarget,
                expectedTarget);
            return false;
        }

        REL::Relocation<std::uintptr_t> callSiteRel{ callSite };
        OriginalRegisterPassGeometryGroup = reinterpret_cast<RegisterPassGeometryGroup_t>(
            callSiteRel.write_call<5>(reinterpret_cast<std::uintptr_t>(&HookedRegisterPassGeometryGroup)));

        if (!OriginalRegisterPassGeometryGroup) {
            REX::WARN("InstallDrawTaggingHooks_Internal: Failed to redirect RegisterObject_ShadowMapOrMask RegisterPass call site at {:#x}", callSite);
            return false;
        }

        REX::INFO("InstallDrawTaggingHooks_Internal: RegisterObject_ShadowMapOrMask RegisterPass call-site redirect installed at {:#x}", callSite);
    }

    if (!OriginalRenderPassImpl) {
        // IDA OG 0x142880030: clean 16-byte boundary after
        // `mov rbx, rdx`; the first 14 bytes would split that instruction.
        constexpr std::size_t kRenderPassImplPrologueSize = 16;
        OriginalRenderPassImpl = CreateBranchGateway<RenderPassImpl_t>(
            ptr_RenderPassImpl,
            kRenderPassImplPrologueSize,
            reinterpret_cast<void*>(&HookedRenderPassImpl));

        if (!OriginalRenderPassImpl) {
            REX::WARN("InstallDrawTaggingHooks_Internal: Failed to install BSBatchRenderer::RenderPassImpl hook");
            return false;
        }

        REX::INFO("InstallDrawTaggingHooks_Internal: BSBatchRenderer::RenderPassImpl hook installed");
    }

    if (!OriginalBuildCommandBuffer) {
        // Prologue: mov [rsp+arg_0], rbx; push rbp/rsi/rdi/r12/r13/r14/r15
        // 5 + 1 + 1 + 1 + 2 + 2 + 2 + 2 = 16 bytes (clean instruction boundary).
        constexpr std::size_t kBuildCommandBufferPrologueSize = 16;
        OriginalBuildCommandBuffer = CreateBranchGateway<BuildCommandBuffer_t>(ptr_BuildCommandBuffer, kBuildCommandBufferPrologueSize, reinterpret_cast<void*>(&HookedBuildCommandBuffer));

        if (!OriginalBuildCommandBuffer) {
            REX::WARN("InstallDrawTaggingHooks_Internal: Failed to install BSShader::BuildCommandBuffer hook");
            return false;
        }

        REX::INFO("InstallDrawTaggingHooks_Internal: BSShader::BuildCommandBuffer hook installed");
    }

    if (!OriginalActorLoad3D) {
        REL::Relocation<std::uintptr_t> actorVTable{ RE::VTABLE::Actor[0] };
        OriginalActorLoad3D = reinterpret_cast<ActorLoad3D_t>(
            actorVTable.write_vfunc(0x86, reinterpret_cast<void*>(&HookedActorLoad3D)));

        if (!OriginalActorLoad3D) {
            REX::WARN("InstallDrawTaggingHooks_Internal: Failed to install Actor::Load3D hook");
            return false;
        }

        REX::INFO("InstallDrawTaggingHooks_Internal: Actor::Load3D hook installed");
    }

    if (!OriginalPlayerCharacterLoad3D) {
        REL::Relocation<std::uintptr_t> playerCharacterVTable{ RE::VTABLE::PlayerCharacter[0] };
        OriginalPlayerCharacterLoad3D = reinterpret_cast<ActorLoad3D_t>(
            playerCharacterVTable.write_vfunc(0x86, reinterpret_cast<void*>(&HookedPlayerCharacterLoad3D)));

        if (!OriginalPlayerCharacterLoad3D) {
            REX::WARN("InstallDrawTaggingHooks_Internal: Failed to install PlayerCharacter::Load3D hook");
            return false;
        }

        REX::INFO("InstallDrawTaggingHooks_Internal: PlayerCharacter::Load3D hook installed");
    }

    if (!OriginalBipedAnimAttachSkinnedObject) {
        constexpr std::size_t kAttachSkinnedObjectPrologueSize = 15;
        OriginalBipedAnimAttachSkinnedObject = CreateBranchGateway<BipedAnimAttachSkinnedObject_t>(ptr_BipedAnimAttachSkinnedObject, kAttachSkinnedObjectPrologueSize, reinterpret_cast<void*>(&HookedBipedAnimAttachSkinnedObject));

        if (!OriginalBipedAnimAttachSkinnedObject) {
            REX::WARN("InstallDrawTaggingHooks_Internal: Failed to install BipedAnim::AttachSkinnedObject hook");
            return false;
        }

        REX::INFO("InstallDrawTaggingHooks_Internal: BipedAnim::AttachSkinnedObject hook installed");
    }

    if (!OriginalBipedAnimAttachBipedWeapon) {
        constexpr std::size_t kAttachBipedWeaponPrologueSize = 18;
        OriginalBipedAnimAttachBipedWeapon = CreateBranchGateway<BipedAnimAttachBipedWeapon_t>(ptr_BipedAnimAttachBipedWeapon, kAttachBipedWeaponPrologueSize, reinterpret_cast<void*>(&HookedBipedAnimAttachBipedWeapon));

        if (!OriginalBipedAnimAttachBipedWeapon) {
            REX::WARN("InstallDrawTaggingHooks_Internal: Failed to install BipedAnim::AttachBipedWeapon hook");
            return false;
        }

        REX::INFO("InstallDrawTaggingHooks_Internal: BipedAnim::AttachBipedWeapon hook installed");
    }

    if (!OriginalBipedAnimAttachToParent) {
        constexpr std::size_t kAttachToParentPrologueSize = 15;
        OriginalBipedAnimAttachToParent = CreateBranchGateway<BipedAnimAttachToParent_t>(ptr_BipedAnimAttachToParent, kAttachToParentPrologueSize, reinterpret_cast<void*>(&HookedBipedAnimAttachToParent));

        if (!OriginalBipedAnimAttachToParent) {
            REX::WARN("InstallDrawTaggingHooks_Internal: Failed to install BipedAnim::AttachToParent hook");
            return false;
        }

        REX::INFO("InstallDrawTaggingHooks_Internal: BipedAnim::AttachToParent hook installed");
    }

    if (!OriginalBipedAnimRemovePart) {
        constexpr std::size_t kRemovePartPrologueSize = 15;
        OriginalBipedAnimRemovePart = CreateBranchGateway<BipedAnimRemovePart_t>(ptr_BipedAnimRemovePart, kRemovePartPrologueSize, reinterpret_cast<void*>(&HookedBipedAnimRemovePart));

        if (!OriginalBipedAnimRemovePart) {
            REX::WARN("InstallDrawTaggingHooks_Internal: Failed to install BipedAnim::RemovePart hook");
            return false;
        }

        REX::INFO("InstallDrawTaggingHooks_Internal: BipedAnim::RemovePart hook installed");
    }

    if (!OriginalActorSet3D) {
        REL::Relocation<std::uintptr_t> actorVTable{ RE::VTABLE::Actor[0] };
        OriginalActorSet3D = reinterpret_cast<Set3D_t>(
            actorVTable.write_vfunc(0x88, reinterpret_cast<void*>(&HookedActorSet3D)));

        if (!OriginalActorSet3D) {
            REX::WARN("InstallDrawTaggingHooks_Internal: Failed to install Actor::Set3D hook");
            return false;
        }

        REX::INFO("InstallDrawTaggingHooks_Internal: Actor::Set3D hook installed");
    }

    if (!OriginalPlayerCharacterSet3D) {
        REL::Relocation<std::uintptr_t> playerCharacterVTable{ RE::VTABLE::PlayerCharacter[0] };
        OriginalPlayerCharacterSet3D = reinterpret_cast<Set3D_t>(
            playerCharacterVTable.write_vfunc(0x88, reinterpret_cast<void*>(&HookedPlayerCharacterSet3D)));

        if (!OriginalPlayerCharacterSet3D) {
            REX::WARN("InstallDrawTaggingHooks_Internal: Failed to install PlayerCharacter::Set3D hook");
            return false;
        }

        REX::INFO("InstallDrawTaggingHooks_Internal: PlayerCharacter::Set3D hook installed");
    }

    if (!OriginalActorOnHeadInitialized) {
        REL::Relocation<std::uintptr_t> actorVTable{ RE::VTABLE::Actor[0] };
        OriginalActorOnHeadInitialized = reinterpret_cast<OnHeadInitialized_t>(
            actorVTable.write_vfunc(0x98, reinterpret_cast<void*>(&HookedActorOnHeadInitialized)));

        if (!OriginalActorOnHeadInitialized) {
            REX::WARN("InstallDrawTaggingHooks_Internal: Failed to install Actor::OnHeadInitialized hook");
            return false;
        }

        REX::INFO("InstallDrawTaggingHooks_Internal: Actor::OnHeadInitialized hook installed");
    }

    if (!OriginalPlayerCharacterOnHeadInitialized) {
        REL::Relocation<std::uintptr_t> playerCharacterVTable{ RE::VTABLE::PlayerCharacter[0] };
        OriginalPlayerCharacterOnHeadInitialized = reinterpret_cast<OnHeadInitialized_t>(
            playerCharacterVTable.write_vfunc(0x98, reinterpret_cast<void*>(&HookedPlayerCharacterOnHeadInitialized)));

        if (!OriginalPlayerCharacterOnHeadInitialized) {
            REX::WARN("InstallDrawTaggingHooks_Internal: Failed to install PlayerCharacter::OnHeadInitialized hook");
            return false;
        }

        REX::INFO("InstallDrawTaggingHooks_Internal: PlayerCharacter::OnHeadInitialized hook installed");
    }

    if (!OriginalUpdate3DModel) {
        // 5-byte patch is mandatory here for BodyShapeManager coexistence.
        // BSM hooks AIProcess::Update3dModel with its own Write5Branch over
        // the 5-byte first instruction (mov [rsp+0x18], rbp). If we used a
        // 14-byte JMP14 patch, we'd overwrite bytes 0-13 — and BSM's gateway
        // hard-codes a JMP to (target+5), which would land in the middle of
        // our patch and execute garbage (observed crash: EXCEPTION_ACCESS_
        // VIOLATION jumping to a bogus RIP). With CreateBranchGateway5 we
        // only touch bytes 0-4, leaving the original `push rsi` at byte 5
        // intact for BSM's gateway to land on.
        //
        // Prologue (verified by reading Fallout4.exe at 0x140E3C9C0):
        //   48 89 6C 24 18      mov [rsp+0x18], rbp     ; 5 bytes
        constexpr std::size_t kUpdate3DModelPrologueSize = 5;
        OriginalUpdate3DModel = CreateBranchGateway5<Update3DModel_t>(
            ptr_Update3DModel, kUpdate3DModelPrologueSize, reinterpret_cast<void*>(&HookedUpdate3DModel));

        if (!OriginalUpdate3DModel) {
            REX::WARN("InstallDrawTaggingHooks_Internal: Failed to install AIProcess::Update3dModel hook");
            return false;
        }

        REX::INFO("InstallDrawTaggingHooks_Internal: AIProcess::Update3dModel hook installed");
    }

    if (!OriginalReset3D) {
        // BSM doesn't hook Reset3D, so coexistence isn't a concern here, but
        // we keep the 5-byte path for consistency and to leave room for other
        // mods that might patch the same function.
        //
        // Prologue (verified by reading Fallout4.exe at 0x140D8A1F0):
        //   48 89 5C 24 08      mov [rsp+0x08], rbx     ; 5 bytes
        constexpr std::size_t kReset3DPrologueSize = 5;
        OriginalReset3D = CreateBranchGateway5<Reset3D_t>(
            ptr_Reset3D, kReset3DPrologueSize, reinterpret_cast<void*>(&HookedReset3D));

        if (!OriginalReset3D) {
            REX::WARN("InstallDrawTaggingHooks_Internal: Failed to install Actor::Reset3D hook");
            return false;
        }

        REX::INFO("InstallDrawTaggingHooks_Internal: Actor::Reset3D hook installed");
    }

    if (!OriginalReplaceHeadTaskRun) {
        // VTABLE::Script__ModifyFaceGen__29__ReplaceHeadTask only has an OG
        // entry in the bundled commonlibf4. AE install is intentionally
        // skipped until we find the AE vtable address.
        REL::Relocation<std::uintptr_t> taskVTable{ RE::VTABLE::Script__ModifyFaceGen__29__ReplaceHeadTask[0] };
        OriginalReplaceHeadTaskRun = reinterpret_cast<ReplaceHeadTaskRun_t>(
            taskVTable.write_vfunc(1, reinterpret_cast<void*>(&HookedReplaceHeadTaskRun)));

        if (!OriginalReplaceHeadTaskRun) {
            REX::WARN("InstallDrawTaggingHooks_Internal: Failed to install Script::ModifyFaceGen::ReplaceHeadTask::Run hook");
            return false;
        }

        REX::INFO("InstallDrawTaggingHooks_Internal: Script::ModifyFaceGen::ReplaceHeadTask::Run hook installed");
    }

    if (!OriginalBSLightTestFrustumCull) {
        // Prologue at OG 0x14285ACB0 (verified by IDA disasm) — 14 bytes ends
        // on a clean instruction boundary right before `sub rsp, 70h`:
        //   mov [rsp+0x10], rbx     ; 5 bytes
        //   mov [rsp+0x18], rbp     ; 5 bytes
        //   push rsi                 ; 1 byte
        //   push rdi                 ; 1 byte
        //   push r14                 ; 2 bytes
        // Total = 14, room for the JMP14 absolute patch.
        constexpr std::size_t kBSLightTestFrustumCullPrologueSize = 14;
        OriginalBSLightTestFrustumCull = CreateBranchGateway<BSLightTestFrustumCull_t>(
            ptr_BSLightTestFrustumCull, kBSLightTestFrustumCullPrologueSize,
            reinterpret_cast<void*>(&HookedBSLightTestFrustumCull));

        if (!OriginalBSLightTestFrustumCull) {
            REX::WARN("InstallDrawTaggingHooks_Internal: Failed to install BSLight::TestFrustumCull hook");
            return false;
        }

        REX::INFO("InstallDrawTaggingHooks_Internal: BSLight::TestFrustumCull hook installed");
    }

    if (!OriginalBSDFTiledLightingAddLight) {
        // 5-byte `call rel32` patch at lambda+0x281. write_call<5> returns the
        // original target (AddLight @ 0x14286DBD0) for forwarding.
        const std::uintptr_t callSite = ptr_TryAddTiledLightLambda.address() + kAddLightCallSiteOffsetOG;
        REL::Relocation<std::uintptr_t> callSiteRel{ callSite };
        OriginalBSDFTiledLightingAddLight = reinterpret_cast<BSDFTiledLightingAddLight_t>(
            callSiteRel.write_call<5>(reinterpret_cast<std::uintptr_t>(&HookedBSDFTiledLightingAddLight)));

        if (!OriginalBSDFTiledLightingAddLight) {
            REX::WARN("InstallDrawTaggingHooks_Internal: Failed to redirect BSDFTiledLighting::AddLight call site at {:#x}", callSite);
            return false;
        }

        REX::INFO("InstallDrawTaggingHooks_Internal: BSDFTiledLighting::AddLight call-site redirect installed at {:#x} (scales radius arg)", callSite);
    }

    if (!OriginalSetupPointLightGeometry) {
        // Prologue at OG 0x1428C37A0: `mov [rsp-8+arg_18], r9b` is exactly 5
        // bytes and a clean instruction boundary. Use CreateBranchGateway5 so
        // we only patch those 5 bytes (the surrounding `push rbp/rbx/rsi/rdi`
        // etc. stay intact). The 14-byte path would mid-cut `lea rbp` at
        // offset +13.
        constexpr std::size_t kSetupPointLightGeometryPrologueSize = 5;
        OriginalSetupPointLightGeometry = CreateBranchGateway5<SetupPointLightGeometry_t>(
            ptr_SetupPointLightGeometry, kSetupPointLightGeometryPrologueSize,
            reinterpret_cast<void*>(&HookedSetupPointLightGeometry));

        if (!OriginalSetupPointLightGeometry) {
            REX::WARN("InstallDrawTaggingHooks_Internal: Failed to install BSDFLightShader::SetupPointLightGeometry hook");
            return false;
        }

        REX::INFO("InstallDrawTaggingHooks_Internal: BSDFLightShader::SetupPointLightGeometry hook installed");
    }

    return true;
}

#include <PCH.h>
#include "HiZCull.h"

#include <Global.h>
#include <Plugin.h>
#include <RenderTargets.h>

#include <d3d11.h>

#include <chrono>
#include <cstring>
#include <limits>

// =============================================================================
// HiZCull ??Hi-Z-based occlusion culling.
//
// Architecture (single-file; details in HiZCull.h).
//
//   On Initialize()
//     Resolve REL::ID-backed references for BSGeometry::OnVisible (the address
//     we expect to find pre-patch) and the world camera global. Patch slot 55
//     (NiAVObject_vtbl::OnVisible) on every BSGeometry-family vtable in
//     `kVtableTargets[]`, via REL::Relocation<>::write_vfunc, which handles
//     VirtualProtect / WriteSafeData internally. Each vtable is verified to
//     currently hold BSGeometry::OnVisible at slot 55 before patching, so
//     overriding subclasses are skipped cleanly. No D3D11 state involved.
//
//   On OnMainAccumEnter() (called by PhaseTelemetry's HookedMainAccum)
//     1. Ensure D3D11 Hi-Z resources are created (lazy, on first call with a
//        valid device + viewport).
//     2. Dispatch the Hi-Z build CS: reads g_depthSRV (the engine's main
//        depth as an SRV), linearises depth via near/far, writes max-reduced
//        view-space depth to s_hiZTex (64x36 R32_FLOAT UAV).
//     3. `CopyResource` Hi-Z ??staging[ring]. Readback is *deferred by one
//        frame* (we map staging[ring^1] which holds the previous-frame Hi-Z).
//        This avoids a CPU/GPU stall.
//     4. Capture the camera metadata that matches the depth history currently
//        in the engine depth target, then copy Hi-Z into staging[ring].
//     5. Map the previous staging buffer into a CPU-side mip pyramid. The
//        snapshot inherits that staging slot's camera metadata, not the current
//        frame camera. This is critical under TAA/DLSS jitter and camera motion.
//     6. Atomic-flip the double-buffered snapshot so OnVisible workers see
//        the new data.
//
//   On HookedOnVisible(self, cullProc)
//     Mode::Off    ??should not be reachable (vtable patch only installed
//                    when mode != Off).  Defensive: passthrough.
//     Mode::Measure ??read snapshot, compute would-cull decision, increment
//                    counters, ALWAYS call the original.
//     Mode::Cull   ??same compute, but if occluded, return without calling
//                    the original (geometry not enrolled into pass lists).
//
// Hi-Z value semantics
//   The depth buffer holds NDC z in [0, 1] (1 = far). After linearization we
//   store *view-space distance* in world units (positive, larger = farther
//   from camera) ??a single max() reduction puts the "farthest visible
//   surface in the region" in the Hi-Z. Per-object: project bound center to
//   view space, subtract radius (closest point), compare against sampled
//   max(). If sphereNearestViewDepth > sampledMaxViewDepth ??occluded.
//
// What ISN'T here in v1 (see HiZCull.h):
//   - BSCullingGroup::Process / BSCuller-arena path
//   - Shadow cascade Hi-Z
//   - Guard band / first-frame override
//   - GPU-side cull list (we readback Hi-Z and test per-object on CPU)
//
// =============================================================================

namespace HiZCull {

std::atomic<Mode> g_mode{ Mode::Off };

namespace {

// --- Engine references (resolved via CommonLibF4 REL::ID address library) ----
//
// We patch *every* vtable in the BSGeometry hierarchy whose slot 55 inherits
// BSGeometry::OnVisible ??i.e. subclasses that did not override. Patching
// only BSGeometry's own vtable would miss every BSTriShape / BSCombinedTriShape
// instance (which is the bulk of world geometry ??direct BSGeometry instances
// are rare). For each candidate vtable we first verify slot 55 currently
// resolves to BSGeometry::OnVisible; if a subclass overrides (BSSegmented,
// BSLODMultiIndex, BSMergeInstanced, BSMeshLOD, BSMultiStreamInstance) we
// skip it cleanly ??those have their own visibility logic that we'd corrupt
// by replacing it with a generic Hi-Z test.
//
// REL::IDs (OG 1.10.163, vtables from RE::VTABLE; offsets cross-checked vs
// tools/version-1-10-163-0-e.txt):
//   RE::VTABLE::BSGeometry         = 1436492  (vtable @ 0x142E161D8)
//   RE::VTABLE::BSTriShape         =  183326  (vtable @ 0x142E16AE8)
//   RE::VTABLE::BSDynamicTriShape  =  681999  (vtable @ 0x142E17BA8)
//   RE::VTABLE::BSCombinedTriShape =  358895  (vtable @ 0x142E34C98)
//   RE::VTABLE::BSSubIndexTriShape =  248868  (vtable @ 0x142E314E8)
//   BSGeometry::OnVisible REL::ID  =  844915  (fn @ 0x141BB11E0; AE 0x16D5200)
//   World camera NiCamera* global  =   81406  (OG @ 0x146723240)
//
// OnVisible vtable slot index ??CommonLibF4's NiAVObject.h annotates it as
// `// 39` (HEX). 0x39 = 57 decimal. Empirically verified by reading
// *(uintptr_t*)(0x142E161D8 + 57*8) == 0x141BB11E0 (= BSGeometry::OnVisible).
// (An earlier draft used slot 55 based on a misread of Ghidra's
// NiAVObject_vtbl struct which under-reported the slot count.)
//
// AE IDs are intentionally set to 0 (matching PhaseTelemetry's convention).
// On AE the REL resolution returns garbage; the runtime "slot N currently
// holds BSGeometry::OnVisible" check fails; module no-ops cleanly.
//
// NiAVObject::worldBound at +0xB0 (NiBound {center.xyz, radius}); CommonLibF4
// confirms (NiAVObject.h line 62). sizeof(RE::BSGeometry) == 0x160.
// NiCamera::viewFrustum at +0x160 (NiFrustum {left,right,top,bottom,near,far,ortho}).

constexpr std::size_t kOnVisibleVtableSlot    = 57;   // NiAVObject_vtbl::OnVisible (CommonLibF4 // 39 hex)
constexpr std::size_t kNiCamera_viewFrustumOff = 0x160;
constexpr std::size_t kNiAVObject_worldBoundOff = 0xB0;
constexpr std::size_t kBSShaderAccumulator_firstPersonOff = 0xB0;
constexpr std::size_t kNiAVObject_worldTranslateOff = 0xA0;
constexpr std::size_t kNiAVObject_previousWorldTranslateOff = 0xF0;
constexpr std::size_t kBSGeometry_property0Off = 0x130;
constexpr std::size_t kBSShaderProperty_flagsOff = 0x30;

struct VtableTarget {
    REL::ID     id;
    const char* name;
};
const VtableTarget kVtableTargets[] = {
    { RE::VTABLE::BSGeometry[0],         "BSGeometry"         },
    { RE::VTABLE::BSTriShape[0],         "BSTriShape"         },
    { RE::VTABLE::BSDynamicTriShape[0],  "BSDynamicTriShape"  },
    { RE::VTABLE::BSCombinedTriShape[0], "BSCombinedTriShape" },
    { RE::VTABLE::BSSubIndexTriShape[0], "BSSubIndexTriShape" },
};
constexpr std::size_t kNumVtableTargets = sizeof(kVtableTargets) / sizeof(kVtableTargets[0]);

// Resolved at Initialize() time.
std::uintptr_t s_BSGeometryOnVisibleAddr      = 0;
std::uintptr_t s_BSShaderAccumRegisterObjAddr = 0;
std::uintptr_t s_worldCameraGlobalAddr        = 0;

// BSShaderAccumulator::RegisterObject hook target.
//
// Why this hook in addition to OnVisible:
//   Empirically, Boston gameplay shows HookedOnVisible never fires (0 calls/s
//   over 60 fps). All world geometry enrolment routes through the
//   BSCullingGroup arena path ??`AccumulatePassesFromArena` dispatches
//   `accumulator->vtable[+0x168](accumulator, geometry)`, which on
//   `BSShaderAccumulator` is `RegisterObject` (REL::ID 1447420, OG 0x14282CED0).
//
//   By patching BSShaderAccumulator's vtable slot 45 (= 0x168/8) we intercept
//   the dispatch point that the arena path actually uses. The OnVisible hook
//   stays installed for the AccumulateSceneArray path (rare in MainAccum but
//   non-zero), so we can attribute work by which counter increments.
//
// Signature: `bool RegisterObject(BSShaderAccumulator* self, BSGeometry* geom)`.
//   Returns bool. The single observed call site (AccumulatePassesFromArena)
//   ignores the return value, so we can safely return `false` for the
//   "occluded ??skipped" case without engine-visible side effects.
constexpr std::size_t kRegisterObjectVtableSlot = 45;   // 0x168 / 8

using RegisterObjectFn = bool (*)(void* self, void* geom);
RegisterObjectFn s_origRegisterObject = nullptr;
bool             s_accumPatched       = false;

// Hi-Z target size: high enough to keep Boston's fragmented occluders useful,
// still small enough for cheap deferred CPU readback.
// 256x144 = 144 KB per frame for mip 0.
constexpr int kHiZWidth   = 256;
constexpr int kHiZHeight  = 144;
constexpr int kHiZMipLevels = 8;  // 256x144, 128x72, 64x36, 32x18, 16x9, 8x4, 4x2, 2x1

// Staging ring size ??readback is deferred by (kReadbackRing - 1) frames.
// 2 buffers ??1 frame delay ??Hi-Z is from 2 frames ago by the time we cull
// against it. Acceptable for gameplay; cinematic cuts may need invalidation.
constexpr std::size_t kReadbackRing = 2;

// --- Hook state -------------------------------------------------------------

using OnVisibleFn = void (*)(void* self, void* cullProc);
OnVisibleFn s_origOnVisible = nullptr;           // all targets share the same fn
bool        s_patchedAny    = false;
bool        s_patchedTarget[kNumVtableTargets] = {};

// --- D3D11 resources --------------------------------------------------------

std::mutex                   s_d3dMutex;
ID3D11ComputeShader*         s_buildCs       = nullptr;
ID3D11Buffer*                s_buildCb       = nullptr;
ID3D11Texture2D*             s_hiZTex        = nullptr;
ID3D11UnorderedAccessView*   s_hiZUav        = nullptr;
ID3D11Texture2D*             s_stagingTex[kReadbackRing] = {};
bool                         s_stagingPrimed[kReadbackRing] = {};   // has valid data
int                          s_ringWriteIndex = 0;                   // next staging to write
int                          s_renderW = 0;                          // last seen render width
int                          s_renderH = 0;                          // last seen render height
std::atomic<int>             s_depthMode{ 0 };                       // 0=forward-Z, 1=reverse-Z

// --- CPU-side snapshot (double-buffered) ------------------------------------

struct CpuMip {
    int width = 0;
    int height = 0;
    std::vector<float> pixels;  // size = width*height; max-reduced view-space distance
};

struct Snapshot {
    bool   valid = false;
    bool   temporalSafe = false;
    // (Camera-relative-world)-to-clip rows, read live at MainAccum entry from
    // BSGraphics::State.camViewData.viewProjMat. Engine builds these as
    // (rotation-only view) 횞 proj ??so input must be (world - cam_origin) and
    // cam_origin must be the engine's posAdjust (NOT cameraRoot.world.t).
    // Convention: HLSL `mul(camRelativePos, viewProj)`,
    //   clip = camRel.x*r0 + camRel.y*r1 + camRel.z*r2 + 1*r3.
    float  vp_r0[4]{};
    float  vp_r1[4]{};
    float  vp_r2[4]{};
    float  vp_r3[4]{};
    // Linearisation: D3D11 perspective, 1=far, 0=near.
    //   viewZ(linear) = (near * far) / (far - depth * (far - near))
    // Stored: precomputed (near*far) and (far-near) for the OnVisible fast path.
    float  near_plane = 0.1f;
    float  far_plane  = 1.0f;
    float  near_times_far  = 0.1f;
    float  far_minus_near  = 0.9f;
    // World camera origin (worldCenter near-plane test uses this only as a
    // sanity bound; we project via VP for the actual test).
    float  cam_origin[3]{};
    // Hi-Z mip pyramid populated from this frame's readback.
    std::array<CpuMip, kHiZMipLevels> mips{};
    // For logging.
    int    sourceW = 0;
    int    sourceH = 0;
    int    cameraSource = 0;
    float  hizMin = 0.0f;
    float  hizMax = 0.0f;
    float  hizAvg = 0.0f;
    float  hizZeroPct = 0.0f;
};

struct CameraMeta {
    bool  valid = false;
    float vp_r0[4]{};
    float vp_r1[4]{};
    float vp_r2[4]{};
    float vp_r3[4]{};
    float near_plane = 0.1f;
    float far_plane = 1.0f;
    float near_times_far = 0.1f;
    float far_minus_near = 0.9f;
    float cam_origin[3]{};
    int   source = 0;
};

Snapshot         s_snapshots[2];
std::atomic<int> s_currentSnapshot{ -1 };  // index of the snapshot OnVisible should read; -1 = none ready
std::atomic<std::uint64_t> s_frame{ 0 };
CameraMeta       s_stagingMeta[kReadbackRing];

// "Currently inside DrawWorld::MainAccum." Set true by OnMainAccumEnter,
// false by OnMainAccumExit. HookedOnVisible only consults the Hi-Z when this
// is true ??shadow path / 3D UI / refraction OnVisible calls all happen
// outside MainAccum, against a camera that isn't the snapshot's world camera,
// and would mis-cull if we applied the world Hi-Z to them.
std::atomic<bool> s_inMainAccum{ false };
std::atomic<unsigned> s_invalidateFrames{ 0 };
std::atomic<bool> s_forcePassthroughThisFrame{ false };

// --- Stats -----------------------------------------------------------------

struct Stats {
    // OnVisible hook invocations (any mode, any gate state).
    std::atomic<std::uint64_t> onVisHookCalls{ 0 };
    std::atomic<std::uint64_t> onVisInMainAccum{ 0 };  // subset where gate was open
    // RegisterObject hook invocations (any mode, any gate state).
    std::atomic<std::uint64_t> regObjHookCalls{ 0 };
    std::atomic<std::uint64_t> regObjInMainAccum{ 0 };
    // Test outcomes (combined across both hook sites).
    std::atomic<std::uint64_t> snapshotMissing{ 0 };
    std::atomic<std::uint64_t> nearPlanePassthrough{ 0 };
    std::atomic<std::uint64_t> offscreenSkipped{ 0 };
    std::atomic<std::uint64_t> firstPersonPassthrough{ 0 };
    std::atomic<std::uint64_t> temporalPassthrough{ 0 };
    std::atomic<std::uint64_t> largeFootprintPassthrough{ 0 };
    std::atomic<std::uint64_t> badDepthPassthrough{ 0 };
    std::atomic<std::uint64_t> candidatePassthrough{ 0 };
    std::atomic<std::uint64_t> wouldCullByHiZ{ 0 };
    std::atomic<std::uint64_t> notCulledByHiZ{ 0 };
    std::atomic<std::uint64_t> cullActuallySkipped{ 0 };
    // MainAccum entry counter (to verify the outer hook fires).
    std::atomic<std::uint64_t> mainAccumEnters{ 0 };
    void Reset() noexcept {
        onVisHookCalls.store(0); onVisInMainAccum.store(0);
        regObjHookCalls.store(0); regObjInMainAccum.store(0);
        snapshotMissing.store(0); nearPlanePassthrough.store(0);
        offscreenSkipped.store(0); firstPersonPassthrough.store(0);
        temporalPassthrough.store(0); largeFootprintPassthrough.store(0);
        badDepthPassthrough.store(0); candidatePassthrough.store(0);
        wouldCullByHiZ.store(0); notCulledByHiZ.store(0);
        cullActuallySkipped.store(0);
        mainAccumEnters.store(0);
    }
};
Stats s_stats;

constexpr double kLogIntervalSecs = 2.0;
std::chrono::steady_clock::time_point s_lastLogTime;

// --- HLSL: Hi-Z build compute shader ----------------------------------------
//
// One thread per Hi-Z output texel. Each thread reads a sourceW/HiZW 횞 sourceH/HiZH
// tile of the engine depth (typed SRV viewing the D24S8 depth as R24_UNORM_X8),
// linearises each sample to view-space distance, and writes the max to UAV.
//
// We use a constant buffer (b0):
//   uint  srcWidth, srcHeight, dstWidth, dstHeight;
//   float nearPlane, farPlane;
//   uint  depthMode; // 0=forward-Z, 1=reverse-Z
//   float pad1;
//
// The depth SRV format depends on what the engine bound. The engine's
// g_depthSRV is created with format DXGI_FORMAT_R24_UNORM_X8_TYPELESS, so
// .r returns the normalised depth value in [0,1] directly. If the runtime
// version uses a different SRV format the shader still compiles (Texture2D<float>
// accepts both R32_FLOAT depth and R24_UNORM_X8 typeless).

const char* kHiZBuildHlsl = R"HLSL(
cbuffer Params : register(b0) {
    uint  g_SrcWidth;
    uint  g_SrcHeight;
    uint  g_DstWidth;
    uint  g_DstHeight;
    float g_Near;
    float g_Far;
    uint  g_DepthMode;
    float g_Pad1;
};

Texture2D<float>  g_Depth : register(t0);   // engine main depth SRV
RWTexture2D<float> g_HiZ  : register(u0);   // small linear-view-depth target

float LinearizeDepth(float d)
{
    // Forward Z: 0=near, 1=far.
    // Reverse Z: 1=near, 0=far.
    float denom = (g_DepthMode == 0)
        ? (g_Far - d * (g_Far - g_Near))
        : (g_Near + d * (g_Far - g_Near));
    return (g_Near * g_Far) / max(denom, 1e-6f);
}

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= g_DstWidth || tid.y >= g_DstHeight) return;

    // Tile of source depth this thread reduces.
    uint x0 = (tid.x * g_SrcWidth ) / g_DstWidth;
    uint y0 = (tid.y * g_SrcHeight) / g_DstHeight;
    uint x1 = ((tid.x + 1) * g_SrcWidth ) / g_DstWidth;
    uint y1 = ((tid.y + 1) * g_SrcHeight) / g_DstHeight;

    float maxLinear = 0.0;
    for (uint y = y0; y < y1; ++y) {
        for (uint x = x0; x < x1; ++x) {
            float d = g_Depth.Load(int3(x, y, 0));
            float lin = LinearizeDepth(d);
            maxLinear = max(maxLinear, lin);
        }
    }
    g_HiZ[tid.xy] = maxLinear;
}
)HLSL";

// --- Helpers ---------------------------------------------------------------

inline std::uint8_t* AsBytes(void* p) noexcept {
    return reinterpret_cast<std::uint8_t*>(p);
}
inline const std::uint8_t* AsBytes(const void* p) noexcept {
    return reinterpret_cast<const std::uint8_t*>(p);
}

inline void* ReadWorldCamera() noexcept {
    if (!s_worldCameraGlobalAddr) return nullptr;
    void* cam = nullptr;
    std::memcpy(&cam, reinterpret_cast<const void*>(s_worldCameraGlobalAddr), sizeof(cam));
    return cam;
}

inline bool ReadNiFrustum(void* cam, float& near_p, float& far_p) noexcept {
    if (!cam) return false;
    // NiFrustum layout: {left, right, top, bottom, near, far, ortho}.
    const auto* base = AsBytes(cam) + kNiCamera_viewFrustumOff;
    float n = 0.0f, f = 0.0f;
    std::memcpy(&n, base + 16, sizeof(float));
    std::memcpy(&f, base + 20, sizeof(float));
    if (!(n > 0.0f) || !(f > n)) return false;
    near_p = n;
    far_p  = f;
    return true;
}

inline bool RowsLookValid(const float rows[4][4]) noexcept
{
    float sum = 0.0f;
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            const float v = rows[r][c];
            if (!std::isfinite(v)) return false;
            sum += std::abs(v);
        }
    }
    return sum > 1.0e-4f;
}

inline void StoreRows(const __m128* src, float outRows[4][4]) noexcept
{
    DirectX::XMFLOAT4X4 m;
    DirectX::XMStoreFloat4x4(&m, DirectX::XMMATRIX(src[0], src[1], src[2], src[3]));
    std::memcpy(outRows, m.m, sizeof(float) * 16);
}

inline void StoreCustomBufferRows(float outRows[4][4]) noexcept
{
    const DirectX::XMFLOAT4 rows[4] = {
        g_customBufferData.g_ViewProjRow0,
        g_customBufferData.g_ViewProjRow1,
        g_customBufferData.g_ViewProjRow2,
        g_customBufferData.g_ViewProjRow3,
    };
    std::memcpy(outRows, rows, sizeof(rows));
}

inline void CopyMetaToSnapshot(Snapshot& dst, const CameraMeta& meta) noexcept
{
    std::memcpy(dst.vp_r0, meta.vp_r0, sizeof(float) * 4);
    std::memcpy(dst.vp_r1, meta.vp_r1, sizeof(float) * 4);
    std::memcpy(dst.vp_r2, meta.vp_r2, sizeof(float) * 4);
    std::memcpy(dst.vp_r3, meta.vp_r3, sizeof(float) * 4);
    dst.near_plane = meta.near_plane;
    dst.far_plane = meta.far_plane;
    dst.near_times_far = meta.near_times_far;
    dst.far_minus_near = meta.far_minus_near;
    dst.cam_origin[0] = meta.cam_origin[0];
    dst.cam_origin[1] = meta.cam_origin[1];
    dst.cam_origin[2] = meta.cam_origin[2];
    dst.cameraSource = meta.source;
}

inline void ClearSnapshotMips(Snapshot& dst)
{
    for (auto& mip : dst.mips) {
        mip.width = 0;
        mip.height = 0;
        mip.pixels.clear();
    }
}

inline const RE::BSGraphics::CameraStateData* SelectWorldCameraState(
    RE::BSGraphics::State& state,
    int* outSource = nullptr,
    std::size_t* outCacheSize = nullptr,
    const void** outWorldCam = nullptr,
    const void** outMatchedRefCam = nullptr) noexcept
{
    if (outCacheSize) *outCacheSize = state.cameraDataCache.size();
    if (const auto* worldCam = RE::Main::WorldRootCamera()) {
        if (outWorldCam) *outWorldCam = worldCam;
        for (const auto& cached : state.cameraDataCache) {
            if (cached.referenceCamera == worldCam) {
                if (outSource) *outSource = 1;
                if (outMatchedRefCam) *outMatchedRefCam = cached.referenceCamera;
                return std::addressof(cached);
            }
        }
    }
    if (outSource) *outSource = 2;
    if (outMatchedRefCam) *outMatchedRefCam = state.cameraState.referenceCamera;
    return std::addressof(state.cameraState);
}

// Read the world camera's live ViewProj + posAdjust from BSGraphics::State.
//
// Why "live" and not g_customBufferData.g_ViewProjRow*: Plugin.cpp's
// UpdateCustomBuffer_Internal runs in MyPresent (= frame-end), so by the time
// our HookedMainAccum fires at the NEXT frame's start the cached rows are
// frame N-1's. Reading directly from BSGraphics::State at MainAccum entry
// gives us the current frame's matrix, matching the world positions the
// BSCullers are about to enrol.
//
// posAdjust is the CPU vertex pre-shift origin the engine uses; the view
// matrix has rotation-only (no translation), so the input MUST be in
// posAdjust-relative space. Using playerCamera.cameraRoot.world.translation
// here would be wrong whenever those diverge from posAdjust (first-person
// head-bob, interior cell origin, TAA settling, etc.).
//
// Mirrors Plugin.cpp::SelectGameplayCameraState: walk cameraDataCache for the
// WorldRootCamera match; fall back to the singleton's primary cameraState.
inline bool ReadLiveCameraVPAndOrigin(float outRows[4][4],
                                      float outPosAdjust[3],
                                      int* outSource = nullptr,
                                      std::size_t* outCacheSize = nullptr,
                                      const void** outWorldCam = nullptr,
                                      const void** outMatchedRefCam = nullptr) noexcept
{
    auto state = RE::BSGraphics::State::GetSingleton();
    int source = 0;
    const auto* csd = SelectWorldCameraState(state, &source, outCacheSize,
                                             outWorldCam, outMatchedRefCam);
    if (outSource) *outSource = source;

    // Use viewProjUnjittered (+0x110), NOT viewProjMat (+0xD0). The latter is
    // post-multiplied by a per-frame TAA jitter offset (sub-pixel shift); at
    // Hi-Z resolution that jitter still moves the projected screen-AABB across
    // mip texel boundaries from frame to frame, producing flickering culls on
    // a static camera. Unjittered = stable per-frame, matches the geometry
    // that's *about* to be rasterized (jitter is applied per-vertex in the
    // vertex shader; we want the underlying camera transform).
    DirectX::XMFLOAT4X4 m;
    const auto& vp = csd->camViewData.viewProjUnjittered;
    DirectX::XMStoreFloat4x4(&m, DirectX::XMMATRIX(vp[0], vp[1], vp[2], vp[3]));
    std::memcpy(outRows, m.m, sizeof(float) * 16);

    outPosAdjust[0] = csd->posAdjust.x;
    outPosAdjust[1] = csd->posAdjust.y;
    outPosAdjust[2] = csd->posAdjust.z;

    // First-frame guard: if the matrix is all-zero the engine hasn't populated
    // it yet (or we caught it mid-init); caller should bail.
    const bool allZero =
        m.m[0][0] == 0 && m.m[1][1] == 0 && m.m[2][2] == 0 && m.m[3][3] == 0;
    return !allZero;
}

// Capture the camera metadata that matches the depth buffer available at
// MainAccum entry. That depth target was produced by an earlier rendered frame,
// so current-frame live matrices are the wrong space for the Hi-Z readback.
// Prefer the plugin's frame-end jittered ViewProj rows (they match the actual
// rasterized depth, including DLSS/TAA jitter); fall back to CommonLib's
// previous unjittered matrix if the custom buffer has not been populated yet.
inline bool CaptureDepthHistoryCameraMeta(CameraMeta& out) noexcept
{
    out = {};

    auto state = RE::BSGraphics::State::GetSingleton();
    int srcPath = 0;
    std::size_t cacheSz = 0;
    const void* worldCamPtr = nullptr;
    const void* matchedRefCam = nullptr;
    const auto* csd = SelectWorldCameraState(state, &srcPath, &cacheSz,
                                             &worldCamPtr, &matchedRefCam);
    if (!csd) return false;

    float rows[4][4]{};
    StoreCustomBufferRows(rows);
    int source = 10; // plugin frame-end jittered VP, best match for depth history
    if (!RowsLookValid(rows)) {
        StoreRows(csd->camViewData.previousViewProjUnjittered, rows);
        source = 20; // engine previous unjittered VP fallback
        if (!RowsLookValid(rows)) {
            StoreRows(csd->camViewData.viewProjUnjittered, rows);
            source = 21; // last-resort current unjittered VP
            if (!RowsLookValid(rows)) {
                return false;
            }
        }
    }

    std::memcpy(out.vp_r0, rows[0], sizeof(float) * 4);
    std::memcpy(out.vp_r1, rows[1], sizeof(float) * 4);
    std::memcpy(out.vp_r2, rows[2], sizeof(float) * 4);
    std::memcpy(out.vp_r3, rows[3], sizeof(float) * 4);

    out.cam_origin[0] = csd->previousPosAdjust.x;
    out.cam_origin[1] = csd->previousPosAdjust.y;
    out.cam_origin[2] = csd->previousPosAdjust.z;

    void* cam = ReadWorldCamera();
    float nearP = 0.0f, farP = 0.0f;
    if (!ReadNiFrustum(cam, nearP, farP)) {
        return false;
    }
    out.near_plane = nearP;
    out.far_plane = farP;
    out.near_times_far = nearP * farP;
    out.far_minus_near = farP - nearP;
    out.source = source;
    out.valid = true;

    static std::atomic<bool> s_loggedFirstHistorySource{ false };
    if (!s_loggedFirstHistorySource.exchange(true, std::memory_order_relaxed)) {
        REX::INFO("HiZCull: first depth-history camera source={} (10=custom jittered, "
                  "20=prev unjittered, 21=current fallback), worldState={} "
                  "(1=cache match, 2=fallback), cacheSize={}, worldCam={:#x}, "
                  "matchedRefCam={:#x}",
                  source, srcPath, cacheSz,
                  reinterpret_cast<std::uintptr_t>(worldCamPtr),
                  reinterpret_cast<std::uintptr_t>(matchedRefCam));
    }

    return true;
}

inline bool CaptureRenderedDepthCameraMeta(CameraMeta& out, int source) noexcept
{
    out = {};

    auto state = RE::BSGraphics::State::GetSingleton();
    int srcPath = 0;
    std::size_t cacheSz = 0;
    const void* worldCamPtr = nullptr;
    const void* matchedRefCam = nullptr;
    const auto* csd = SelectWorldCameraState(state, &srcPath, &cacheSz,
                                             &worldCamPtr, &matchedRefCam);
    if (!csd) return false;

    float rows[4][4]{};
    StoreRows(csd->camViewData.viewProjMat, rows);
    if (!RowsLookValid(rows)) {
        StoreRows(csd->camViewData.viewProjUnjittered, rows);
        if (!RowsLookValid(rows)) {
            return false;
        }
    }

    std::memcpy(out.vp_r0, rows[0], sizeof(float) * 4);
    std::memcpy(out.vp_r1, rows[1], sizeof(float) * 4);
    std::memcpy(out.vp_r2, rows[2], sizeof(float) * 4);
    std::memcpy(out.vp_r3, rows[3], sizeof(float) * 4);

    out.cam_origin[0] = csd->posAdjust.x;
    out.cam_origin[1] = csd->posAdjust.y;
    out.cam_origin[2] = csd->posAdjust.z;

    float nearP = 0.0f, farP = 0.0f;
    if (!ReadNiFrustum(ReadWorldCamera(), nearP, farP)) {
        return false;
    }
    out.near_plane = nearP;
    out.far_plane = farP;
    out.near_times_far = nearP * farP;
    out.far_minus_near = farP - nearP;
    out.source = source;
    out.valid = true;

    static std::atomic<bool> s_loggedFirstRenderedSource{ false };
    if (!s_loggedFirstRenderedSource.exchange(true, std::memory_order_relaxed)) {
        REX::INFO("HiZCull: first rendered-depth camera source={} "
                  "(completed-world current jittered), worldState={} "
                  "(1=cache match, 2=fallback), cacheSize={}, worldCam={:#x}, "
                  "matchedRefCam={:#x}",
                  source, srcPath, cacheSz,
                  reinterpret_cast<std::uintptr_t>(worldCamPtr),
                  reinterpret_cast<std::uintptr_t>(matchedRefCam));
    }

    return true;
}

// Patch all candidate vtables via REL::Relocation::write_vfunc (which handles
// VirtualProtect / WriteSafeData internally). Each target is verified to
// currently hold BSGeometry::OnVisible at slot 55 before patching ??if a
// subclass shipped with an override that doesn't show up in our static map
// we skip it cleanly rather than calling the subclass override after our
// Hi-Z test under a "this is BSGeometry::OnVisible" assumption.
std::size_t PatchAllVtables(std::uintptr_t hookAddr, std::uintptr_t& outSharedOrig)
{
    std::size_t patched = 0;
    for (std::size_t i = 0; i < kNumVtableTargets; ++i) {
        REL::Relocation<std::uintptr_t> vtbl{ kVtableTargets[i].id };
        const auto vtblBase = vtbl.address();
        if (vtblBase == 0) {
            REX::WARN("HiZCull: {} vtable REL::ID failed to resolve (likely AE "
                      "without an AE mapping); skipping",
                      kVtableTargets[i].name);
            continue;
        }
        const auto slotAddr = vtblBase + kOnVisibleVtableSlot * sizeof(std::uintptr_t);
        const std::uintptr_t current = *reinterpret_cast<std::uintptr_t*>(slotAddr);
        if (current != s_BSGeometryOnVisibleAddr) {
            REX::WARN("HiZCull: skipping {} vtable @ {:#x} ??slot {} = {:#x} != "
                      "BSGeometry::OnVisible {:#x} (subclass overrides OnVisible; "
                      "instances of this type are not covered by v1)",
                      kVtableTargets[i].name, vtblBase,
                      kOnVisibleVtableSlot, current, s_BSGeometryOnVisibleAddr);
            continue;
        }
        const std::uintptr_t origRaw = vtbl.write_vfunc(kOnVisibleVtableSlot, hookAddr);
        s_patchedTarget[i] = true;
        if (outSharedOrig == 0) outSharedOrig = origRaw;
        ++patched;
        REX::INFO("HiZCull: patched {} vtable @ {:#x} slot {} (orig={:#x})",
                  kVtableTargets[i].name, vtblBase,
                  kOnVisibleVtableSlot, origRaw);
    }
    return patched;
}

void RestoreAllVtables(std::uintptr_t origAddr)
{
    if (!origAddr) return;
    for (std::size_t i = 0; i < kNumVtableTargets; ++i) {
        if (!s_patchedTarget[i]) continue;
        REL::Relocation<std::uintptr_t> vtbl{ kVtableTargets[i].id };
        if (vtbl.address() == 0) continue;
        (void)vtbl.write_vfunc(kOnVisibleVtableSlot, origAddr);
        s_patchedTarget[i] = false;
    }
}

// --- D3D11 resource lifecycle ----------------------------------------------

// Returns the engine's immediate context, or nullptr if not yet up.
REX::W32::ID3D11DeviceContext* GetContext() noexcept {
    if (!g_rendererData) return nullptr;
    return g_rendererData->context;
}
REX::W32::ID3D11Device* GetDevice() noexcept {
    if (!g_rendererData) return nullptr;
    return g_rendererData->device;
}

void ReleaseHiZResources() noexcept
{
    if (s_buildCs) { s_buildCs->Release(); s_buildCs = nullptr; }
    if (s_buildCb) { s_buildCb->Release(); s_buildCb = nullptr; }
    if (s_hiZUav)  { s_hiZUav->Release();  s_hiZUav  = nullptr; }
    if (s_hiZTex)  { s_hiZTex->Release();  s_hiZTex  = nullptr; }
    for (auto*& t : s_stagingTex) { if (t) { t->Release(); t = nullptr; } }
    for (auto& p : s_stagingPrimed) p = false;
    for (auto& meta : s_stagingMeta) meta = {};
}

bool CompileBuildShader(ID3D11Device* device)
{
    if (s_buildCs) return true;
    ID3DBlob* blob = nullptr;
    ID3DBlob* err  = nullptr;
    HRESULT hr = D3DCompile(kHiZBuildHlsl, std::strlen(kHiZBuildHlsl),
                            "HiZBuild.hlsl", nullptr, nullptr,
                            "main", "cs_5_0",
                            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &err);
    if (FAILED(hr)) {
        const char* msg = err ? static_cast<const char*>(err->GetBufferPointer()) : "(no error blob)";
        REX::WARN("HiZCull: D3DCompile(HiZBuild) failed: hr={:#x} msg={}", hr, msg);
        if (err) err->Release();
        return false;
    }
    if (err) err->Release();

    hr = device->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(),
                                     nullptr, &s_buildCs);
    blob->Release();
    if (FAILED(hr) || !s_buildCs) {
        REX::WARN("HiZCull: CreateComputeShader(HiZBuild) failed: hr={:#x}", hr);
        if (s_buildCs) { s_buildCs->Release(); s_buildCs = nullptr; }
        return false;
    }
    REX::INFO("HiZCull: HiZBuild CS compiled");
    return true;
}

bool EnsureResources(REX::W32::ID3D11Device* rexDevice, int renderW, int renderH)
{
    // Resources are sized to the Hi-Z target, not the render target. Render-size
    // change still triggers a re-init because the build CS reads from the
    // depth SRV at its current dims via Load() ??the source size is supplied
    // via the constant buffer per dispatch, so technically nothing needs to
    // resize. But re-init on dim change anyway: it clears the staging buffers
    // so we don't briefly Hi-Z against a stale (wrong-fov) frame.
    std::lock_guard guard(s_d3dMutex);

    if (s_buildCs && s_hiZTex && renderW == s_renderW && renderH == s_renderH) {
        return true;
    }

    auto* device = reinterpret_cast<ID3D11Device*>(rexDevice);
    if (!device) return false;

    ReleaseHiZResources();
    s_renderW = renderW;
    s_renderH = renderH;

    if (!CompileBuildShader(device)) return false;

    // Constant buffer.
    {
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth      = 16 * sizeof(float);   // padded to 16 floats
        bd.Usage          = D3D11_USAGE_DEFAULT;
        bd.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
        HRESULT hr = device->CreateBuffer(&bd, nullptr, &s_buildCb);
        if (FAILED(hr) || !s_buildCb) {
            REX::WARN("HiZCull: CreateBuffer(cb) failed: hr={:#x}", hr);
            ReleaseHiZResources();
            return false;
        }
    }

    // Hi-Z target ??small R32_FLOAT, UAV + (no SRV needed; we readback only).
    {
        D3D11_TEXTURE2D_DESC td{};
        td.Width            = kHiZWidth;
        td.Height           = kHiZHeight;
        td.MipLevels        = 1;
        td.ArraySize        = 1;
        td.Format           = DXGI_FORMAT_R32_FLOAT;
        td.SampleDesc.Count = 1;
        td.Usage            = D3D11_USAGE_DEFAULT;
        td.BindFlags        = D3D11_BIND_UNORDERED_ACCESS;
        HRESULT hr = device->CreateTexture2D(&td, nullptr, &s_hiZTex);
        if (FAILED(hr) || !s_hiZTex) {
            REX::WARN("HiZCull: CreateTexture2D(HiZ) failed: hr={:#x}", hr);
            ReleaseHiZResources();
            return false;
        }
        D3D11_UNORDERED_ACCESS_VIEW_DESC uavd{};
        uavd.Format             = DXGI_FORMAT_R32_FLOAT;
        uavd.ViewDimension      = D3D11_UAV_DIMENSION_TEXTURE2D;
        uavd.Texture2D.MipSlice = 0;
        hr = device->CreateUnorderedAccessView(s_hiZTex, &uavd, &s_hiZUav);
        if (FAILED(hr) || !s_hiZUav) {
            REX::WARN("HiZCull: CreateUnorderedAccessView(HiZ) failed: hr={:#x}", hr);
            ReleaseHiZResources();
            return false;
        }
    }

    // Staging ring.
    for (std::size_t i = 0; i < kReadbackRing; ++i) {
        D3D11_TEXTURE2D_DESC td{};
        td.Width            = kHiZWidth;
        td.Height           = kHiZHeight;
        td.MipLevels        = 1;
        td.ArraySize        = 1;
        td.Format           = DXGI_FORMAT_R32_FLOAT;
        td.SampleDesc.Count = 1;
        td.Usage            = D3D11_USAGE_STAGING;
        td.CPUAccessFlags   = D3D11_CPU_ACCESS_READ;
        HRESULT hr = device->CreateTexture2D(&td, nullptr, &s_stagingTex[i]);
        if (FAILED(hr) || !s_stagingTex[i]) {
            REX::WARN("HiZCull: CreateTexture2D(staging[{}]) failed: hr={:#x}", i, hr);
            ReleaseHiZResources();
            return false;
        }
        s_stagingPrimed[i] = false;
    }

    s_ringWriteIndex = 0;
    REX::INFO("HiZCull: D3D resources created at {}x{} (Hi-Z {}x{} R32_FLOAT, "
              "{} staging buffers)",
              renderW, renderH, kHiZWidth, kHiZHeight, kReadbackRing);
    return true;
}

// --- Hi-Z dispatch + readback ----------------------------------------------

void DispatchHiZBuild(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* depthSrv,
                      int srcW, int srcH, float nearP, float farP)
{
    // Update CB.
    struct CBData {
        std::uint32_t srcW, srcH, dstW, dstH;
        float nearP, farP;
        std::uint32_t depthMode;
        float pad1;
        float pad2[8];
    } cb{};
    cb.srcW = static_cast<std::uint32_t>(srcW);
    cb.srcH = static_cast<std::uint32_t>(srcH);
    cb.dstW = kHiZWidth;
    cb.dstH = kHiZHeight;
    cb.nearP = nearP;
    cb.farP  = farP;
    cb.depthMode = static_cast<std::uint32_t>(s_depthMode.load(std::memory_order_relaxed));
    ctx->UpdateSubresource(s_buildCb, 0, nullptr, &cb, 0, 0);

    // Save existing CS state so we don't disrupt the engine.
    ID3D11ComputeShader* prevCs = nullptr;
    ID3D11ClassInstance* prevInst[16] = {};
    UINT prevInstCount = 16;
    ID3D11Buffer* prevCb0 = nullptr;
    ID3D11ShaderResourceView* prevSrv0 = nullptr;
    ID3D11UnorderedAccessView* prevUav0 = nullptr;
    ctx->CSGetShader(&prevCs, prevInst, &prevInstCount);
    ctx->CSGetConstantBuffers(0, 1, &prevCb0);
    ctx->CSGetShaderResources(0, 1, &prevSrv0);
    ctx->CSGetUnorderedAccessViews(0, 1, &prevUav0);

    // D3D11 forbids reading a resource as an SRV while the same resource is
    // still bound as an output DSV. The engine can leave the scene depth DSV
    // attached after Forward; temporarily drop only the DSV so our compute
    // shader samples the real depth instead of a hazard-null SRV.
    ID3D11RenderTargetView* prevRtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    ID3D11DepthStencilView* prevDsv = nullptr;
    ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, prevRtvs, &prevDsv);
    if (prevDsv) {
        ctx->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, prevRtvs, nullptr);
    }

    // Dispatch.
    ctx->CSSetShader(s_buildCs, nullptr, 0);
    ctx->CSSetConstantBuffers(0, 1, &s_buildCb);
    ctx->CSSetShaderResources(0, 1, &depthSrv);
    UINT initCount = 0;
    ctx->CSSetUnorderedAccessViews(0, 1, &s_hiZUav, &initCount);
    const UINT gx = (kHiZWidth + 7) / 8;
    const UINT gy = (kHiZHeight + 7) / 8;
    ctx->Dispatch(gx, gy, 1);

    // Unbind UAV before the engine reuses these slots.
    ID3D11UnorderedAccessView* nullUav = nullptr;
    ID3D11ShaderResourceView*  nullSrv = nullptr;
    ctx->CSSetUnorderedAccessViews(0, 1, &nullUav, &initCount);
    ctx->CSSetShaderResources(0, 1, &nullSrv);

    // Restore.
    ctx->CSSetShader(prevCs, prevInst, prevInstCount);
    ctx->CSSetConstantBuffers(0, 1, &prevCb0);
    ctx->CSSetShaderResources(0, 1, &prevSrv0);
    ctx->CSSetUnorderedAccessViews(0, 1, &prevUav0, &initCount);
    if (prevDsv) {
        ctx->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, prevRtvs, prevDsv);
    }
    if (prevCs)   prevCs->Release();
    for (UINT i = 0; i < prevInstCount; ++i) { if (prevInst[i]) prevInst[i]->Release(); }
    if (prevCb0)  prevCb0->Release();
    if (prevSrv0) prevSrv0->Release();
    if (prevUav0) prevUav0->Release();
    for (auto*& rtv : prevRtvs) {
        if (rtv) {
            rtv->Release();
            rtv = nullptr;
        }
    }
    if (prevDsv) prevDsv->Release();
}

void CopyHiZToStaging(ID3D11DeviceContext* ctx)
{
    const int writeIdx = s_ringWriteIndex;
    ctx->CopyResource(s_stagingTex[writeIdx], s_hiZTex);
    s_stagingPrimed[writeIdx] = true;
}

bool GetDepthSourceDims(ID3D11ShaderResourceView* depthSrv, int& srcW, int& srcH)
{
    srcW = 0;
    srcH = 0;
    if (!depthSrv) return false;

    ID3D11Resource* res = nullptr;
    depthSrv->GetResource(&res);
    if (!res) return false;

    ID3D11Texture2D* tex2d = nullptr;
    if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D),
                                      reinterpret_cast<void**>(&tex2d)))) {
        D3D11_TEXTURE2D_DESC td{};
        tex2d->GetDesc(&td);
        srcW = static_cast<int>(td.Width);
        srcH = static_cast<int>(td.Height);
        tex2d->Release();
    }
    res->Release();
    return srcW > 0 && srcH > 0;
}

// Reads the *least recently written* staging buffer (which holds the
// previous frame's Hi-Z) into the inactive snapshot's mip 0. Builds the rest
// of the pyramid via CPU max() reduction. Skips if no staging buffer has
// been primed yet.
bool ReadBackInto(Snapshot& snap, ID3D11DeviceContext* ctx)
{
    // Ring: writer wrote into s_ringWriteIndex *this frame*. Reader wants
    // the buffer written previously; that's (s_ringWriteIndex ^ 1) for
    // ring size 2.
    const int readIdx = (s_ringWriteIndex + kReadbackRing - 1) % kReadbackRing;
    if (!s_stagingPrimed[readIdx]) return false;
    if (!s_stagingMeta[readIdx].valid) return false;
    CopyMetaToSnapshot(snap, s_stagingMeta[readIdx]);

    D3D11_MAPPED_SUBRESOURCE m{};
    HRESULT hr = ctx->Map(s_stagingTex[readIdx], 0, D3D11_MAP_READ, 0, &m);
    if (FAILED(hr) || !m.pData) {
        REX::WARN("HiZCull: staging Map failed: hr={:#x}", hr);
        return false;
    }

    auto& mip0 = snap.mips[0];
    mip0.width  = kHiZWidth;
    mip0.height = kHiZHeight;
    mip0.pixels.assign(static_cast<std::size_t>(kHiZWidth) * kHiZHeight, 0.0f);
    const auto* row = static_cast<const std::uint8_t*>(m.pData);
    for (int y = 0; y < kHiZHeight; ++y) {
        std::memcpy(&mip0.pixels[static_cast<std::size_t>(y) * kHiZWidth],
                    row + static_cast<std::size_t>(y) * m.RowPitch,
                    static_cast<std::size_t>(kHiZWidth) * sizeof(float));
    }
    ctx->Unmap(s_stagingTex[readIdx], 0);

    float minV = (std::numeric_limits<float>::max)();
    float maxV = 0.0f;
    double sumV = 0.0;
    std::uint32_t zeroish = 0;
    for (const float v : mip0.pixels) {
        if (!std::isfinite(v)) continue;
        minV = (std::min)(minV, v);
        maxV = (std::max)(maxV, v);
        sumV += v;
        if (v <= snap.near_plane * 1.25f) {
            ++zeroish;
        }
    }
    const auto count = static_cast<float>((std::max<std::size_t>)(1, mip0.pixels.size()));
    snap.hizMin = minV == (std::numeric_limits<float>::max)() ? 0.0f : minV;
    snap.hizMax = maxV;
    snap.hizAvg = static_cast<float>(sumV / count);
    snap.hizZeroPct = 100.0f * static_cast<float>(zeroish) / count;

    // Creation Engine's world projection is forward-Z (confirmed in
    // BSGraphics::State::CalculateCameraViewProj). A near-plane-only or all-zero
    // readback means this depth source is empty/clear at capture time. Do not
    // publish that as a usable occlusion snapshot: per-object badDepth checks are
    // just wasted CPU, and keeping a previous snapshot would be unsafe after a
    // 180-degree turn.
    if (snap.hizMax <= snap.near_plane * 8.0f &&
        snap.hizAvg <= snap.near_plane * 4.0f)
    {
        static std::atomic<std::uint64_t> s_emptyDepthWarnFrame{ 0 };
        const auto frameNow = s_frame.load(std::memory_order_relaxed);
        auto lastWarn = s_emptyDepthWarnFrame.load(std::memory_order_relaxed);
        if (frameNow > lastWarn + 120 &&
            s_emptyDepthWarnFrame.compare_exchange_strong(
                lastWarn, frameNow, std::memory_order_relaxed))
        {
            REX::WARN("HiZCull: depth readback is near-only/empty "
                      "(mode={}, min={:.2f} avg={:.2f} max={:.2f} near={} zeroish={:.1f}%). "
                      "Dropping this snapshot.",
                      s_depthMode.load(std::memory_order_relaxed) == 0 ? "forward" : "reverse",
                      snap.hizMin, snap.hizAvg, snap.hizMax, snap.near_plane, snap.hizZeroPct);
        }
        return false;
    }

    // Build pyramid: each level halves dims, max() reduces 2x2.
    for (int lvl = 1; lvl < kHiZMipLevels; ++lvl) {
        auto& src = snap.mips[lvl - 1];
        auto& dst = snap.mips[lvl];
        const int sw = src.width;
        const int sh = src.height;
        const int dw = (std::max)(1, sw / 2);
        const int dh = (std::max)(1, sh / 2);
        dst.width  = dw;
        dst.height = dh;
        dst.pixels.assign(static_cast<std::size_t>(dw) * dh, 0.0f);
        for (int y = 0; y < dh; ++y) {
            for (int x = 0; x < dw; ++x) {
                const int sx0 = x * 2;
                const int sy0 = y * 2;
                const int sx1 = (std::min)(sw - 1, sx0 + 1);
                const int sy1 = (std::min)(sh - 1, sy0 + 1);
                const float a = src.pixels[static_cast<std::size_t>(sy0) * sw + sx0];
                const float b = src.pixels[static_cast<std::size_t>(sy0) * sw + sx1];
                const float c = src.pixels[static_cast<std::size_t>(sy1) * sw + sx0];
                const float d = src.pixels[static_cast<std::size_t>(sy1) * sw + sx1];
                dst.pixels[static_cast<std::size_t>(y) * dw + x] =
                    (std::max)((std::max)(a, b), (std::max)(c, d));
            }
        }
    }
    return true;
}

// Tight 2D NDC bound for a sphere projected through a centred perspective.
//
// Reference: Mara & McGuire, "2D Polyhedral Bounds of a Clipped, Perspective-
// Projected 3D Sphere", JCGT 2013. Also Lloyd "Calculating Screen Coverage"
// and Aaltonen's Frostbite GDC 2015 talk.
//
// The standard `radius * focal / depth` AABB approximation treats the sphere
// as if its silhouette were a circle on the image plane. That is only correct
// when the sphere is on the optical axis. Off-axis, perspective skews the
// silhouette into an ellipse whose actual extents on the axis-aligned screen
// AABB are *smaller in the direction toward the optical axis* and *larger in
// the opposite direction* than the symmetric approximation predicts. For thin
// foreground geometry near the camera this asymmetry is the difference
// between sampling the Hi-Z texel that contains the object and sampling an
// adjacent texel that may hold a closer occluder ??false cull.
//
// Algorithm (per axis, in the (axis, forward) plane of camera space):
//   Camera at origin. Sphere is a 2D circle at (cAxis, cFwd) with radius r.
//   Find the two tangent lines from origin to the circle. Their slopes
//   (axis/forward) are the tangent-of-NDC-bound values; multiply by focal
//   to get NDC bounds.
//
// Returns false if the camera is inside the sphere (sphere fills view) or
// the sphere is fully behind the camera (caller should passthrough).
inline bool ProjectSphereAxisTight(float cAxis, float cFwd, float r, float focal,
                                   float& ndcMin, float& ndcMax) noexcept
{
    const float d2 = cAxis * cAxis + cFwd * cFwd;
    const float r2 = r * r;
    if (d2 <= r2) return false;             // camera inside sphere
    if (cFwd <= 0.0f && -cFwd > r) return false; // sphere fully behind camera

    // Direction from camera to sphere center (normalized in (axis, fwd) plane).
    const float invD = 1.0f / std::sqrt(d2);
    const float cosT = cFwd  * invD;        // = cos(angle to center, from forward)
    const float sinT = cAxis * invD;        // = sin(angle to center)

    // Half-angle subtended by sphere from camera.
    const float sinA = r * invD;
    const float cosA = std::sqrt((std::max)(1.0f - sinA * sinA, 0.0f));

    // Tangent directions = (cosT ??A, sinT ??A). Standard angle-addition.
    const float cosPlus  = cosT * cosA - sinT * sinA;   // toward +axis
    const float sinPlus  = sinT * cosA + cosT * sinA;
    const float cosMinus = cosT * cosA + sinT * sinA;   // toward ?뭓xis
    const float sinMinus = sinT * cosA - cosT * sinA;

    // NDC = tan(theta) * focal = sin/cos * focal. If the tangent direction's
    // forward component (cos) is non-positive, the tangent line goes parallel
    // to or behind the image plane ??bound is unbounded in that direction;
    // clamp to a far-off-screen sentinel so the AABB clip downstream handles it.
    constexpr float kSentinel = 1.0e6f;
    const float ndcPlus  = cosPlus  > 1e-6f ? (sinPlus  / cosPlus)  * focal :  kSentinel;
    const float ndcMinus = cosMinus > 1e-6f ? (sinMinus / cosMinus) * focal : -kSentinel;

    ndcMin = (std::min)(ndcMinus, ndcPlus);
    ndcMax = (std::max)(ndcMinus, ndcPlus);
    return true;
}

// Project a world-space point through the snapshot's VP matrix.  Returns
// clip-space {x, y, z, w}.
//
// IMPORTANT: The engine builds view as rotation-only (no translation row;
// confirmed via Ghidra disassembly of BSGraphics::State::CalculateCameraViewProj
// @ 0x141d216f0 ??view row 3 is hardcoded (0,0,0,1)). World vertices are
// pre-shifted by `posAdjust` on the CPU before they enter the matrix. So we
// must subtract `s.cam_origin` (= posAdjust) from the world position here.
//
// Convention: `mul(camRelPos, viewProj)` row-vector style,
//   output.c = sum_k(camRelPos[k] * row_k[c]).
inline void ProjectWorld(const Snapshot& s, float wx, float wy, float wz,
                         float& cx, float& cy, float& cz, float& cw) noexcept
{
    // Camera-relative position.
    const float rx = wx - s.cam_origin[0];
    const float ry = wy - s.cam_origin[1];
    const float rz = wz - s.cam_origin[2];
    cx = rx * s.vp_r0[0] + ry * s.vp_r1[0] + rz * s.vp_r2[0] + s.vp_r3[0];
    cy = rx * s.vp_r0[1] + ry * s.vp_r1[1] + rz * s.vp_r2[1] + s.vp_r3[1];
    cz = rx * s.vp_r0[2] + ry * s.vp_r1[2] + rz * s.vp_r2[2] + s.vp_r3[2];
    cw = rx * s.vp_r0[3] + ry * s.vp_r1[3] + rz * s.vp_r2[3] + s.vp_r3[3];
}

inline bool IsAccumulatorFirstPerson(const void* accum) noexcept
{
    if (!accum) return false;
    bool firstPerson = false;
    std::memcpy(&firstPerson, AsBytes(accum) + kBSShaderAccumulator_firstPersonOff,
                sizeof(firstPerson));
    return firstPerson;
}

inline bool IsDescendantOf(const RE::NiAVObject* object, const RE::NiAVObject* root) noexcept
{
    if (!object || !root) return false;
    const RE::NiAVObject* current = object;
    for (int depth = 0; current && depth < 96; ++depth) {
        if (current == root) return true;
        current = current->parent;
    }
    return false;
}

inline bool IsFirstPersonGeometry(const void* geom) noexcept
{
    if (!geom) return false;
    const auto* object = reinterpret_cast<const RE::NiAVObject*>(geom);
    const auto* player = RE::PlayerCharacter::GetSingleton();
    if (!player) return false;

    const RE::NiAVObject* firstPersonRoot = player->firstPerson3D.get();
    if (IsDescendantOf(object, firstPersonRoot)) return true;
    if (IsDescendantOf(object, player->firstPersonTorso)) return true;
    if (IsDescendantOf(object, player->firstPersonEye)) return true;
    return false;
}

inline bool ShouldBypassHiZ(void* accum, void* geom) noexcept
{
    if (IsAccumulatorFirstPerson(accum) || IsFirstPersonGeometry(geom)) {
        s_stats.firstPersonPassthrough.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    return false;
}

inline float Dot3(const float* a, const float* b) noexcept
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

inline void Normalize3(float* v) noexcept
{
    const float lenSq = Dot3(v, v);
    if (lenSq <= 1.0e-8f) return;
    const float invLen = 1.0f / std::sqrt(lenSq);
    v[0] *= invLen;
    v[1] *= invLen;
    v[2] *= invLen;
}

bool IsTemporalReuseSafe(const Snapshot& s) noexcept
{
    float rows[4][4];
    float pos[3];
    if (!ReadLiveCameraVPAndOrigin(rows, pos)) {
        return false;
    }

    const float dx = pos[0] - s.cam_origin[0];
    const float dy = pos[1] - s.cam_origin[1];
    const float dz = pos[2] - s.cam_origin[2];
    const float distSq = dx * dx + dy * dy + dz * dz;

    float staleForward[3] = { s.vp_r0[3], s.vp_r1[3], s.vp_r2[3] };
    float liveForward[3] = { rows[0][3], rows[1][3], rows[2][3] };
    Normalize3(staleForward);
    Normalize3(liveForward);
    const float facingDot = Dot3(staleForward, liveForward);

    // Conservative temporal Hi-Z for *active* CPU culling. This is previous
    // frame depth, so even moderate camera motion creates disocclusion holes
    // that the history buffer cannot know about. Keep this deliberately tight;
    // measure mode can still report would-cull candidates, but cull mode must
    // prefer visible over risky.
    constexpr float kMaxCameraTranslation = 12.0f;
    constexpr float kMinForwardDot = 0.9990f; // ~2.6 degrees
    return distSq <= kMaxCameraTranslation * kMaxCameraTranslation &&
           facingDot >= kMinForwardDot;
}

// Per-object occlusion test. Returns true if the sphere can be culled.
// Approximations (documented):
//   - Sphere ??screen AABB built from "radius / w" scaling per axis; this is
//     the standard tangent-plane bound, slightly conservative for far objects.
//   - Sphere near-Z computed as (center clip-w ??radius) projected into linear
//     view-space using the snapshot's near/far. Bound to >= near_plane.
//   - Hi-Z mip picked from the larger screen-space dim (in mip-0 texels).
//   - Sampled max of 4 surrounding texels at that mip ??conservative.
//
// Returns false (don't cull) on any edge case (sphere near camera, off-screen,
// no snapshot, etc.) ??these are passthrough cases.
bool TestOccluded(const Snapshot& s, const float* worldBound)
{
    const float wx = worldBound[0];
    const float wy = worldBound[1];
    const float wz = worldBound[2];
    const float wr = worldBound[3];
    if (!(wr > 0.0f)) {
        s_stats.nearPlanePassthrough.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    // Decompose viewProj into the camera basis (right, up, forward) in world
    // space + focal lengths. Valid because the engine builds the matrix as
    // (rotation-only-view 횞 proj) with proj being a centred perspective ??
    // confirmed via Ghidra BSGraphics::State::CalculateCameraViewProj.
    //
    //   forward_world = (vp_r0[3], vp_r1[3], vp_r2[3])   (col 3 of viewProj)
    //   right_world   = (vp_r0[0], vp_r1[0], vp_r2[0]) / focal_x
    //   up_world      = (vp_r0[1], vp_r1[1], vp_r2[1]) / focal_y
    //   focal_x       = |col 0 of viewProj 3횞3|
    //   focal_y       = |col 1 of viewProj 3횞3|
    auto Sqr = [](float v) { return v * v; };
    const float fx = std::sqrt(Sqr(s.vp_r0[0]) + Sqr(s.vp_r1[0]) + Sqr(s.vp_r2[0]));
    const float fy = std::sqrt(Sqr(s.vp_r0[1]) + Sqr(s.vp_r1[1]) + Sqr(s.vp_r2[1]));
    const float invFx = fx > 1e-6f ? 1.0f / fx : 1.0f;
    const float invFy = fy > 1e-6f ? 1.0f / fy : 1.0f;
    const float fwd_w[3] = { s.vp_r0[3], s.vp_r1[3], s.vp_r2[3] };
    const float rgt_w[3] = { s.vp_r0[0] * invFx, s.vp_r1[0] * invFx, s.vp_r2[0] * invFx };
    const float upx_w[3] = { s.vp_r0[1] * invFy, s.vp_r1[1] * invFy, s.vp_r2[1] * invFy };

    // Sphere center in camera space (right, up, forward).
    const float rel_x = wx - s.cam_origin[0];
    const float rel_y = wy - s.cam_origin[1];
    const float rel_z = wz - s.cam_origin[2];
    const float camX = rel_x * rgt_w[0] + rel_y * rgt_w[1] + rel_z * rgt_w[2];
    const float camY = rel_x * upx_w[0] + rel_y * upx_w[1] + rel_z * upx_w[2];
    const float camZ = rel_x * fwd_w[0] + rel_y * fwd_w[1] + rel_z * fwd_w[2];

    // Sphere extending past or near the near plane: punt. Engine should handle.
    const float centerForward  = camZ;
    const float nearestForward = centerForward - wr;
    if (nearestForward <= s.near_plane * 1.05f) {
        s_stats.nearPlanePassthrough.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    // Tight NDC bound via Mara-McGuire (per-axis tangent projection).
    float ndcMinX, ndcMaxX, ndcMinY, ndcMaxY;
    if (!ProjectSphereAxisTight(camX, camZ, wr, fx, ndcMinX, ndcMaxX) ||
        !ProjectSphereAxisTight(camY, camZ, wr, fy, ndcMinY, ndcMaxY))
    {
        // Camera inside sphere or sphere fully behind ??passthrough.
        s_stats.nearPlanePassthrough.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    // Off-screen fully? Then the engine's frustum cull should have caught it,
    // but if we slip through, treat as not-our-cull (don't claim it).
    if (ndcMaxX < -1.0f || ndcMinX > 1.0f || ndcMaxY < -1.0f || ndcMinY > 1.0f) {
        s_stats.offscreenSkipped.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    // Map NDC to Hi-Z mip-0 pixel space.
    const float u0 = std::clamp(0.5f * (ndcMinX + 1.0f), 0.0f, 1.0f);
    const float u1 = std::clamp(0.5f * (ndcMaxX + 1.0f), 0.0f, 1.0f);
    // NDC y is up; texture y is down. Flip:
    const float v0 = std::clamp(0.5f * (1.0f - ndcMaxY), 0.0f, 1.0f);
    const float v1 = std::clamp(0.5f * (1.0f - ndcMinY), 0.0f, 1.0f);
    const float px0 = u0 * kHiZWidth;
    const float px1 = u1 * kHiZWidth;
    const float py0 = v0 * kHiZHeight;
    const float py1 = v1 * kHiZHeight;
    const float dx = (std::max)(0.0f, px1 - px0);
    const float dy = (std::max)(0.0f, py1 - py0);
    const float maxDim = (std::max)(dx, dy);

    // Very large screen-space bounds are poor Hi-Z candidates in this CPU
    // integration because temporal reprojection error can move the edge by
    // several low-res texels. Passing them through is cheap insurance for
    // Boston precombines and near-camera weapon/body geometry.
    constexpr float kMaxCullWidthFrac = 0.55f;
    constexpr float kMaxCullHeightFrac = 0.55f;
    constexpr float kMaxCullAreaFrac = 0.20f;
    if (dx > kHiZWidth * kMaxCullWidthFrac ||
        dy > kHiZHeight * kMaxCullHeightFrac ||
        (dx * dy) > (kHiZWidth * kHiZHeight) * kMaxCullAreaFrac)
    {
        s_stats.largeFootprintPassthrough.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    // Mip pick: each mip halves dims, so a region of size maxDim mip-0 texels
    // is covered by 2x2 texels at mip = ceil(log2(maxDim)).
    int mip = 0;
    if (maxDim > 1.0f) {
        const float lg = std::log2(maxDim);
        mip = std::clamp(static_cast<int>(std::ceil(lg)), 0, kHiZMipLevels - 1);
    }
    const auto& cm = s.mips[mip];
    if (cm.pixels.empty()) {
        s_stats.snapshotMissing.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    // Compute the AABB in this mip's pixel coords; sample texels that cover
    // it (taking the max). A 1-texel guard absorbs the point-sampled mip's
    // texel-boundary quantization (Hi-Z mips are max-reductions at integer
    // texel positions; the rasterized object's pixels can fall on the texel
    // boundary). With Mara-McGuire-tight NDC bounds the screen footprint is
    // accurate, so a single-texel guard is sufficient. (Larger guards would
    // over-sample ??under-cull.)
    constexpr int kGuard = 2;
    const float shiftScale = 1.0f / static_cast<float>(1 << mip);
    const int   mx0 = std::clamp(static_cast<int>(std::floor(px0 * shiftScale)) - kGuard,
                                 0, cm.width  - 1);
    const int   my0 = std::clamp(static_cast<int>(std::floor(py0 * shiftScale)) - kGuard,
                                 0, cm.height - 1);
    const int   mx1 = std::clamp(static_cast<int>(std::floor(px1 * shiftScale)) + kGuard,
                                 0, cm.width  - 1);
    const int   my1 = std::clamp(static_cast<int>(std::floor(py1 * shiftScale)) + kGuard,
                                 0, cm.height - 1);
    float sampledMax = 0.0f;
    for (int y = my0; y <= my1; ++y) {
        const float* row = &cm.pixels[static_cast<std::size_t>(y) * cm.width];
        for (int x = mx0; x <= mx1; ++x) {
            sampledMax = (std::max)(sampledMax, row[x]);
        }
    }
    if (sampledMax <= s.near_plane * 1.25f) {
        s_stats.notCulledByHiZ.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    // Sphere's nearest view-space depth (= forward distance ??radius).
    // Hi-Z stores view-space distance, so direct compare works.
    const float radiusBias = wr * 0.10f;
    const float distanceBias = nearestForward * 0.01f;
    const float depthBias = (std::max)(64.0f, (std::max)(radiusBias, distanceBias));
    if (nearestForward > sampledMax + depthBias) {
        s_stats.wouldCullByHiZ.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    s_stats.notCulledByHiZ.fetch_add(1, std::memory_order_relaxed);
    return false;
}

// --- The vtable wrapper -----------------------------------------------------

std::atomic<bool> s_loggedFirstOnVisCall{ false };
std::atomic<bool> s_loggedFirstRegObjCall{ false };

std::atomic<bool> s_loggedFirstProjection{ false };

// Diagnostic ??log the snapshot's VP matrix + the first geometry's projection,
// once. Helps verify the matrix layout (Risk R1) and the projected cw value
// for a real object. Picked when the snapshot is valid and we have a sphere
// with non-zero radius.
void MaybeLogFirstProjection(const Snapshot& s, const float* bound,
                             float cx, float cy, float cz, float cw)
{
    if (s_loggedFirstProjection.exchange(true, std::memory_order_relaxed)) return;

    // Also project the camera origin ??expect (~0, ~0, ~something, ~tiny) in
    // clip space. If projection works, the camera origin projects to itself.
    float ccx, ccy, ccz, ccw;
    ProjectWorld(s, s.cam_origin[0], s.cam_origin[1], s.cam_origin[2],
                 ccx, ccy, ccz, ccw);

    // Project a point 1000 units in front of (well, offset from) the camera
    // along world X. Should produce a different cw than the origin if matrix
    // is sane.
    float dx, dy, dz, dw;
    ProjectWorld(s, s.cam_origin[0] + 1000.0f, s.cam_origin[1],
                 s.cam_origin[2], dx, dy, dz, dw);

    REX::INFO(
        "HiZCull DIAG: VP rows:\n"
        "  r0={:.4f} {:.4f} {:.4f} {:.4f}\n"
        "  r1={:.4f} {:.4f} {:.4f} {:.4f}\n"
        "  r2={:.4f} {:.4f} {:.4f} {:.4f}\n"
        "  r3={:.4f} {:.4f} {:.4f} {:.4f}\n"
        "  cam={:.1f} {:.1f} {:.1f}  near={:.2f} far={:.2f}\n"
        "  cam proj      ??({:.2f} {:.2f} {:.2f} {:.2f})\n"
        "  cam+1000x proj??({:.2f} {:.2f} {:.2f} {:.2f})\n"
        "  obj bound (w={:.1f} {:.1f} {:.1f}, r={:.1f}) proj??"
        "({:.2f} {:.2f} {:.2f} {:.2f}); ndcXY=({:.3f} {:.3f}) "
        "nearestForward={:.2f}",
        s.vp_r0[0], s.vp_r0[1], s.vp_r0[2], s.vp_r0[3],
        s.vp_r1[0], s.vp_r1[1], s.vp_r1[2], s.vp_r1[3],
        s.vp_r2[0], s.vp_r2[1], s.vp_r2[2], s.vp_r2[3],
        s.vp_r3[0], s.vp_r3[1], s.vp_r3[2], s.vp_r3[3],
        s.cam_origin[0], s.cam_origin[1], s.cam_origin[2],
        s.near_plane, s.far_plane,
        ccx, ccy, ccz, ccw,
        dx, dy, dz, dw,
        bound[0], bound[1], bound[2], bound[3],
        cx, cy, cz, cw,
        cw != 0.0f ? cx / cw : 0.0f,
        cw != 0.0f ? cy / cw : 0.0f,
        cw - bound[3]);
}

bool IsStableOpaqueCandidate(void* geomSelf) noexcept
{
    if (!geomSelf) {
        return false;
    }

    void* prop = nullptr;
    std::memcpy(&prop, AsBytes(geomSelf) + kBSGeometry_property0Off, sizeof(prop));
    if (!prop) {
        return false;
    }

    std::uint64_t flags = 0;
    std::memcpy(&flags, AsBytes(prop) + kBSShaderProperty_flagsOff, sizeof(flags));
    constexpr auto Bit = [](unsigned n) constexpr noexcept -> std::uint64_t {
        return 1ull << n;
    };
    constexpr std::uint64_t kRejectShaderFlags =
        Bit(1)  |  // skinned
        Bit(2)  |  // temp refraction
        Bit(3)  |  // vertex alpha
        Bit(10) |  // face
        Bit(15) |  // refraction
        Bit(16) |  // refraction falloff
        Bit(17) |  // eye reflect
        Bit(18) |  // hair tint
        Bit(19) |  // screendoor alpha fade
        Bit(21) |  // facegen RGB tint
        Bit(26) |  // decal
        Bit(27) |  // dynamic decal
        Bit(28) |  // character light
        Bit(30) |  // soft effect
        Bit(40) |  // dismemberment meat cuff
        Bit(47) |  // dismemberment
        Bit(49) |  // weapon blood
        Bit(55) |  // menu screen
        Bit(57) |  // alpha test
        Bit(60) |  // pipboy screen
        Bit(61) |  // tree anim
        Bit(62) |  // effect lighting
        Bit(63);   // refraction writes depth
    if ((flags & kRejectShaderFlags) != 0) {
        return false;
    }

    float worldT[3]{};
    float prevT[3]{};
    std::memcpy(worldT, AsBytes(geomSelf) + kNiAVObject_worldTranslateOff, sizeof(worldT));
    std::memcpy(prevT, AsBytes(geomSelf) + kNiAVObject_previousWorldTranslateOff, sizeof(prevT));
    if (!std::isfinite(worldT[0]) || !std::isfinite(worldT[1]) || !std::isfinite(worldT[2]) ||
        !std::isfinite(prevT[0]) || !std::isfinite(prevT[1]) || !std::isfinite(prevT[2]))
    {
        return false;
    }

    const float dx = worldT[0] - prevT[0];
    const float dy = worldT[1] - prevT[1];
    const float dz = worldT[2] - prevT[2];
    constexpr float kMaxObjectMotion = 1.0f;
    return (dx * dx + dy * dy + dz * dz) <= kMaxObjectMotion * kMaxObjectMotion;
}

// Returns true if the bound at (self + kNiAVObject_worldBoundOff) should be
// treated as occluded.  Shared between HookedOnVisible and HookedRegisterObject.
// `mode` and `s_inMainAccum` must already have been validated by the caller.
bool ShouldCullGeometry(void* geomSelf)
{
    const int snapIdx = s_currentSnapshot.load(std::memory_order_acquire);
    if (snapIdx < 0) {
        s_stats.snapshotMissing.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    const Snapshot& snap = s_snapshots[snapIdx];
    if (!snap.valid || snap.mips[0].pixels.empty()) {
        s_stats.snapshotMissing.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (s_forcePassthroughThisFrame.load(std::memory_order_relaxed) || !snap.temporalSafe) {
        s_stats.temporalPassthrough.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (snap.hizMax <= snap.near_plane * 2.0f && snap.hizAvg <= snap.near_plane * 1.5f) {
        s_stats.badDepthPassthrough.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (!IsStableOpaqueCandidate(geomSelf)) {
        s_stats.candidatePassthrough.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    float bound[4];
    std::memcpy(bound, AsBytes(geomSelf) + kNiAVObject_worldBoundOff, sizeof(bound));

    if (!s_loggedFirstProjection.load(std::memory_order_relaxed) && bound[3] > 0.0f) {
        float cx, cy, cz, cw;
        ProjectWorld(snap, bound[0], bound[1], bound[2], cx, cy, cz, cw);
        MaybeLogFirstProjection(snap, bound, cx, cy, cz, cw);
    }

    return TestOccluded(snap, bound);
}

void HookedOnVisible(void* self, void* cullProc)
{
    s_stats.onVisHookCalls.fetch_add(1, std::memory_order_relaxed);
    if (!s_loggedFirstOnVisCall.exchange(true, std::memory_order_relaxed)) {
        REX::INFO("HiZCull: first HookedOnVisible call observed "
                  "(self={:#x}, cullProc={:#x}, inMainAccum={})",
                  reinterpret_cast<std::uintptr_t>(self),
                  reinterpret_cast<std::uintptr_t>(cullProc),
                  s_inMainAccum.load(std::memory_order_relaxed));
    }
    const auto mode = g_mode.load(std::memory_order_relaxed);
    if (mode == Mode::Off || !s_origOnVisible) {
        if (s_origOnVisible) s_origOnVisible(self, cullProc);
        return;
    }
    if (!s_inMainAccum.load(std::memory_order_acquire)) {
        s_origOnVisible(self, cullProc);
        return;
    }
    s_stats.onVisInMainAccum.fetch_add(1, std::memory_order_relaxed);
    if (ShouldBypassHiZ(nullptr, self)) {
        s_origOnVisible(self, cullProc);
        return;
    }

    const bool occluded = ShouldCullGeometry(self);
    if (mode == Mode::Cull && occluded) {
        s_stats.cullActuallySkipped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    s_origOnVisible(self, cullProc);
}

// Hooks BSShaderAccumulator::RegisterObject (vtable slot 45 / +0x168).
// `geom` is a BSGeometry* ??its worldBound lives at +0xB0 the same as any
// NiAVObject. Return value is bool; v1 returns `false` on occluded-skip and
// the original's return otherwise. The single observed call site
// (AccumulatePassesFromArena) discards the return, so this is safe.
bool HookedRegisterObject(void* self, void* geom)
{
    s_stats.regObjHookCalls.fetch_add(1, std::memory_order_relaxed);
    if (!s_loggedFirstRegObjCall.exchange(true, std::memory_order_relaxed)) {
        REX::INFO("HiZCull: first HookedRegisterObject call observed "
                  "(self={:#x}, geom={:#x}, inMainAccum={})",
                  reinterpret_cast<std::uintptr_t>(self),
                  reinterpret_cast<std::uintptr_t>(geom),
                  s_inMainAccum.load(std::memory_order_relaxed));
    }
    const auto mode = g_mode.load(std::memory_order_relaxed);
    if (mode == Mode::Off || !s_origRegisterObject) {
        return s_origRegisterObject ? s_origRegisterObject(self, geom) : false;
    }
    if (!s_inMainAccum.load(std::memory_order_acquire)) {
        return s_origRegisterObject(self, geom);
    }
    if (!geom) {
        return s_origRegisterObject(self, geom);
    }
    s_stats.regObjInMainAccum.fetch_add(1, std::memory_order_relaxed);
    if (ShouldBypassHiZ(self, geom)) {
        return s_origRegisterObject(self, geom);
    }

    const bool occluded = ShouldCullGeometry(geom);
    if (mode == Mode::Cull && occluded) {
        s_stats.cullActuallySkipped.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    return s_origRegisterObject(self, geom);
}

// --- Snapshot population ----------------------------------------------------

void PopulateSnapshot(Snapshot& dst, ID3D11DeviceContext* ctx)
{
    dst.valid = false;
    dst.temporalSafe = false;

    // 1. Take VP matrix + posAdjust live from BSGraphics::State (current frame
    //    at MainAccum entry). NOT from g_customBufferData ??those are updated
    //    in MyPresent (frame-end) and thus are frame N-1 by the time we run.
    float vpRows[4][4];
    float posAdjust[3];
    int   srcPath = 0;
    std::size_t cacheSz = 0;
    const void* worldCamPtr = nullptr;
    const void* matchedRefCam = nullptr;
    if (!ReadLiveCameraVPAndOrigin(vpRows, posAdjust, &srcPath, &cacheSz,
                                   &worldCamPtr, &matchedRefCam)) {
        // First frame / engine not initialised ??bail.
        return;
    }
    static std::atomic<bool> s_loggedFirstSource{ false };
    if (!s_loggedFirstSource.exchange(true, std::memory_order_relaxed)) {
        REX::INFO("HiZCull: first snapshot source = {} (1=cache match, 2=fallback "
                  "state.cameraState), cacheSize={}, worldCam={:#x}, matchedRefCam={:#x}",
                  srcPath, cacheSz,
                  reinterpret_cast<std::uintptr_t>(worldCamPtr),
                  reinterpret_cast<std::uintptr_t>(matchedRefCam));
    }
    std::memcpy(dst.vp_r0, vpRows[0], sizeof(float) * 4);
    std::memcpy(dst.vp_r1, vpRows[1], sizeof(float) * 4);
    std::memcpy(dst.vp_r2, vpRows[2], sizeof(float) * 4);
    std::memcpy(dst.vp_r3, vpRows[3], sizeof(float) * 4);

    // 2. Read near/far from the live world camera.
    void* cam = ReadWorldCamera();
    float nearP = 0.0f, farP = 0.0f;
    if (!ReadNiFrustum(cam, nearP, farP)) {
        // No camera = treat snapshot as invalid.
        return;
    }
    dst.near_plane     = nearP;
    dst.far_plane      = farP;
    dst.near_times_far = nearP * farP;
    dst.far_minus_near = farP - nearP;

    // 3. Camera origin = posAdjust (the same value the engine subtracts from
    //    world vertex positions on the CPU before they hit the rotation-only
    //    view * proj matrix). Used by ProjectWorld for camera-relative input.
    dst.cam_origin[0] = posAdjust[0];
    dst.cam_origin[1] = posAdjust[1];
    dst.cam_origin[2] = posAdjust[2];

    // 4. Readback the *previous-frame* staging buffer into the mip pyramid.
    const bool readbackOk = ReadBackInto(dst, ctx);
    if (!readbackOk) {
        // Mark valid for camera-only path (so OnVisible has VP; it'll bail
        // out on mip emptiness inside TestOccluded). On first frame this
        // sets dst.valid=true but mips[0].pixels is empty ??TestOccluded
        // returns false ??passthrough, counted as snapshotMissing.
        ClearSnapshotMips(dst);
        dst.valid = true;
        return;
    }

    dst.temporalSafe = IsTemporalReuseSafe(dst);

    // 5. Source resolution for diagnostic.
    dst.sourceW = s_renderW;
    dst.sourceH = s_renderH;

    dst.valid = true;
}

// --- Periodic logging -------------------------------------------------------

std::atomic<bool> s_loggedFirstCall{ false };
std::atomic<bool> s_loggedFirstSnapshot{ false };

void MaybeLog()
{
    const auto now = std::chrono::steady_clock::now();
    const double secs = std::chrono::duration<double>(now - s_lastLogTime).count();
    if (secs < kLogIntervalSecs) return;
    s_lastLogTime = now;

    const auto mode = g_mode.load(std::memory_order_relaxed);
    if (mode == Mode::Off) return;  // module disabled

    const auto onVisAll  = s_stats.onVisHookCalls.load(std::memory_order_relaxed);
    const auto onVisGate = s_stats.onVisInMainAccum.load(std::memory_order_relaxed);
    const auto regObjAll = s_stats.regObjHookCalls.load(std::memory_order_relaxed);
    const auto regObjGate= s_stats.regObjInMainAccum.load(std::memory_order_relaxed);
    const auto wouldCull = s_stats.wouldCullByHiZ.load(std::memory_order_relaxed);
    const auto notCulled = s_stats.notCulledByHiZ.load(std::memory_order_relaxed);
    const auto snapMiss  = s_stats.snapshotMissing.load(std::memory_order_relaxed);
    const auto nearPlane = s_stats.nearPlanePassthrough.load(std::memory_order_relaxed);
    const auto offscreen = s_stats.offscreenSkipped.load(std::memory_order_relaxed);
    const auto firstPerson = s_stats.firstPersonPassthrough.load(std::memory_order_relaxed);
    const auto temporal = s_stats.temporalPassthrough.load(std::memory_order_relaxed);
    const auto largeFootprint = s_stats.largeFootprintPassthrough.load(std::memory_order_relaxed);
    const auto badDepth = s_stats.badDepthPassthrough.load(std::memory_order_relaxed);
    const auto candidate = s_stats.candidatePassthrough.load(std::memory_order_relaxed);
    const auto skipped   = s_stats.cullActuallySkipped.load(std::memory_order_relaxed);
    const auto entries   = s_stats.mainAccumEnters.load(std::memory_order_relaxed);

    const std::uint64_t gateOpenCalls = onVisGate + regObjGate;
    const double tested = static_cast<double>(wouldCull + notCulled);
    const double cullPct = tested > 0
        ? 100.0 * static_cast<double>(wouldCull) / tested
        : 0.0;

    REX::INFO(
        "HiZCull[{}]: MainAccum/s={:.1f}  OnVis/s={:.0f}(in={:.0f})  "
        "RegObj/s={:.0f}(in={:.0f})  testable={:.1f}%  wouldCull%={:.1f}%  "
        "skipped/s={:.0f}  passthrough(snap={} near={} off={} fp={} temporal={} large={} cand={} badDepth={})  src={}x{}",
        mode == Mode::Cull ? "cull" : (mode == Mode::Measure ? "measure" : "off"),
        entries / secs,
        onVisAll / secs, onVisGate / secs,
        regObjAll / secs, regObjGate / secs,
        gateOpenCalls > 0
            ? 100.0 * tested / static_cast<double>(gateOpenCalls)
            : 0.0,
        cullPct,
        skipped / secs,
        snapMiss, nearPlane, offscreen, firstPerson, temporal, largeFootprint, candidate, badDepth,
        s_renderW, s_renderH);

    // Periodically dump the current snapshot matrix + camera origin so we can
    // see what's actually being used during gameplay (not just first frame).
    const int snapIdx = s_currentSnapshot.load(std::memory_order_acquire);
    if (snapIdx >= 0) {
        const Snapshot& s = s_snapshots[snapIdx];
        if (s.valid) {
            REX::INFO("HiZCull VP: r0={:.4f} {:.4f} {:.4f} {:.4f}  r1={:.4f} {:.4f} "
                      "{:.4f} {:.4f}  r2={:.4f} {:.4f} {:.4f} {:.4f}  r3={:.4f} "
                      "{:.4f} {:.4f} {:.4f}  origin={:.1f} {:.1f} {:.1f}  "
                      "near={:.2f} far={:.0f} temporalSafe={} camSource={} depthMode={} "
                      "hiz(min/avg/max/zero%)={:.1f}/{:.1f}/{:.1f}/{:.1f}",
                      s.vp_r0[0], s.vp_r0[1], s.vp_r0[2], s.vp_r0[3],
                      s.vp_r1[0], s.vp_r1[1], s.vp_r1[2], s.vp_r1[3],
                      s.vp_r2[0], s.vp_r2[1], s.vp_r2[2], s.vp_r2[3],
                      s.vp_r3[0], s.vp_r3[1], s.vp_r3[2], s.vp_r3[3],
                      s.cam_origin[0], s.cam_origin[1], s.cam_origin[2],
                      s.near_plane, s.far_plane, s.temporalSafe, s.cameraSource,
                      s_depthMode.load(std::memory_order_relaxed) == 0 ? "forward" : "reverse",
                      s.hizMin, s.hizAvg, s.hizMax, s.hizZeroPct);
        }
    }

    s_stats.Reset();
}

}  // anonymous namespace

// =============================================================================
// Public API
// =============================================================================

bool Initialize()
{
    if (s_patchedAny) return true;
    const auto mode = g_mode.load(std::memory_order_relaxed);
    if (mode == Mode::Off) {
        REX::INFO("HiZCull::Initialize: mode=off ??vtables not patched (zero cost)");
        return true;
    }

    // Resolve engine addresses via REL::ID. OG IDs are from
    // tools/version-1-10-163-0-e.txt; AE IDs deliberately 0 (matches the
    // PhaseTelemetry convention ??runtime safety check will gate AE).
    {
        REL::Relocation<std::uintptr_t> onVisible{ REL::ID{ 844915, 0 } };
        s_BSGeometryOnVisibleAddr = onVisible.address();
        REL::Relocation<std::uintptr_t> regObj{ REL::ID{ 1447420, 0 } };
        s_BSShaderAccumRegisterObjAddr = regObj.address();
        REL::Relocation<std::uintptr_t> worldCam{ REL::ID{ 81406, 0 } };
        s_worldCameraGlobalAddr = worldCam.address();
    }
    if (!s_BSGeometryOnVisibleAddr || !s_worldCameraGlobalAddr ||
        !s_BSShaderAccumRegisterObjAddr) {
        REX::WARN("HiZCull::Initialize: REL::ID resolution failed "
                  "(BSGeometry::OnVisible={:#x}, RegisterObject={:#x}, "
                  "worldCamera global={:#x}) ??likely AE without mappings; "
                  "module no-ops",
                  s_BSGeometryOnVisibleAddr, s_BSShaderAccumRegisterObjAddr,
                  s_worldCameraGlobalAddr);
        return false;
    }

    // Hook 1: BSGeometry-family OnVisible. Covers AccumulateSceneArray path.
    std::uintptr_t sharedOnVis = 0;
    const std::size_t patched = PatchAllVtables(
        REX::UNRESTRICTED_CAST<std::uintptr_t>(&HookedOnVisible), sharedOnVis);
    if (patched > 0 && sharedOnVis != 0) {
        s_origOnVisible = REX::UNRESTRICTED_CAST<OnVisibleFn>(sharedOnVis);
        s_patchedAny    = true;
    }
    REX::INFO("HiZCull::Initialize: BSGeometry-family OnVisible ??{} of {} "
              "vtables patched (orig={:#x})",
              patched, kNumVtableTargets, sharedOnVis);

    // Hook 2: BSShaderAccumulator::RegisterObject (vtable slot 45). Covers
    // the BSCullingGroup arena path which is the bulk of MainAccum enrolment.
    {
        REL::Relocation<std::uintptr_t> accumVtbl{ RE::VTABLE::BSShaderAccumulator[0] };
        const auto base = accumVtbl.address();
        if (base == 0) {
            REX::WARN("HiZCull::Initialize: BSShaderAccumulator vtable "
                      "unresolved; arena-path hook skipped");
        } else {
            const auto slotPtr = base + kRegisterObjectVtableSlot * sizeof(std::uintptr_t);
            const std::uintptr_t current = *reinterpret_cast<std::uintptr_t*>(slotPtr);
            if (current != s_BSShaderAccumRegisterObjAddr) {
                REX::WARN("HiZCull::Initialize: BSShaderAccumulator vtable "
                          "slot {} = {:#x} != RegisterObject {:#x}; arena-path "
                          "hook skipped (subclass override?)",
                          kRegisterObjectVtableSlot, current,
                          s_BSShaderAccumRegisterObjAddr);
            } else {
                const std::uintptr_t origRaw = accumVtbl.write_vfunc(
                    kRegisterObjectVtableSlot,
                    REX::UNRESTRICTED_CAST<std::uintptr_t>(&HookedRegisterObject));
                s_origRegisterObject =
                    REX::UNRESTRICTED_CAST<RegisterObjectFn>(origRaw);
                s_accumPatched = true;
                REX::INFO("HiZCull::Initialize: patched BSShaderAccumulator vtable "
                          "@ {:#x} slot {} (orig={:#x})",
                          base, kRegisterObjectVtableSlot, origRaw);
            }
        }
    }

    if (!s_patchedAny && !s_accumPatched) {
        REX::WARN("HiZCull::Initialize: no hooks installed; module disabled");
        return false;
    }
    s_lastLogTime = std::chrono::steady_clock::now();
    REX::INFO("HiZCull::Initialize: complete, mode={}",
              mode == Mode::Cull ? "cull" : "measure");
    return true;
}

void Shutdown()
{
    // Restore vtables first so no in-flight render thread can land in our
    // hooks after we've torn down GPU resources.
    if (s_patchedAny && s_origOnVisible) {
        RestoreAllVtables(REX::UNRESTRICTED_CAST<std::uintptr_t>(s_origOnVisible));
        s_patchedAny = false;
        REX::INFO("HiZCull::Shutdown: BSGeometry-family OnVisible restored");
    }
    if (s_accumPatched && s_origRegisterObject) {
        REL::Relocation<std::uintptr_t> accumVtbl{ RE::VTABLE::BSShaderAccumulator[0] };
        if (accumVtbl.address() != 0) {
            (void)accumVtbl.write_vfunc(kRegisterObjectVtableSlot,
                REX::UNRESTRICTED_CAST<std::uintptr_t>(s_origRegisterObject));
        }
        s_accumPatched = false;
        REX::INFO("HiZCull::Shutdown: BSShaderAccumulator::RegisterObject restored");
    }
    std::lock_guard guard(s_d3dMutex);
    ReleaseHiZResources();
}

void OnMainAccumEnter()
{
    const auto mode = g_mode.load(std::memory_order_relaxed);
    if (mode == Mode::Off) return;

    // Open the gate FIRST ??even if the rest of this function early-returns
    // (e.g. resources not ready yet), OnVisible should still consult the
    // most-recent snapshot during MainAccum's lifetime. The Exit hook always
    // closes it.
    s_inMainAccum.store(true, std::memory_order_release);

    if (!s_loggedFirstCall.exchange(true, std::memory_order_relaxed)) {
        REX::INFO("HiZCull::OnMainAccumEnter: first invocation observed");
    }

    s_frame.fetch_add(1, std::memory_order_relaxed);
    s_stats.mainAccumEnters.fetch_add(1, std::memory_order_relaxed);
    unsigned invalidate = s_invalidateFrames.load(std::memory_order_relaxed);
    if (invalidate > 0) {
        s_forcePassthroughThisFrame.store(true, std::memory_order_release);
        while (invalidate > 0 &&
               !s_invalidateFrames.compare_exchange_weak(invalidate, invalidate - 1,
                                                          std::memory_order_relaxed)) {
        }
    } else {
        s_forcePassthroughThisFrame.store(false, std::memory_order_release);
    }
    MaybeLog();

    // --- Hi-Z build path ----------------------------------------------------
    // Both Measure and Cull modes run the full GPU pipeline so the test
    // outcomes (testable% / wouldCull%) are meaningful in measure mode.
    // The mode gate only affects whether we ACTUALLY skip the OnVisible /
    // RegisterObject call when occluded ??measure mode always passes through.
    auto* context = GetContext();

    Snapshot& dst = s_snapshots[(s_currentSnapshot.load(std::memory_order_relaxed) + 1) & 1];
    dst.valid = false;
    dst.temporalSafe = false;

    if (context) {
        PopulateSnapshot(dst, reinterpret_cast<ID3D11DeviceContext*>(context));
    }

    // Flip the snapshot atomically (publish for OnVisible workers).
    const int newIdx = (s_currentSnapshot.load(std::memory_order_relaxed) + 1) & 1;
    s_currentSnapshot.store(newIdx, std::memory_order_release);

    if (dst.valid && !s_loggedFirstSnapshot.exchange(true, std::memory_order_relaxed)) {
        REX::INFO("HiZCull: first snapshot ready (mode={}, src={}x{}, near={:.2f} far={:.2f}, "
                  "mips[0]={}x{} primed={} temporalSafe={} camSource={} depthMode={} "
                  "hiz(min/avg/max/zero%)={:.1f}/{:.1f}/{:.1f}/{:.1f})",
                  mode == Mode::Cull ? "cull" : "measure",
                  dst.sourceW, dst.sourceH, dst.near_plane, dst.far_plane,
                  dst.mips[0].width, dst.mips[0].height,
                  !dst.mips[0].pixels.empty(), dst.temporalSafe, dst.cameraSource,
                  s_depthMode.load(std::memory_order_relaxed) == 0 ? "forward" : "reverse",
                  dst.hizMin, dst.hizAvg, dst.hizMax, dst.hizZeroPct);
    }
}

void OnWorldDepthReadyExit()
{
    const auto mode = g_mode.load(std::memory_order_relaxed);
    if (mode == Mode::Off) return;

    auto* device = GetDevice();
    auto* context = GetContext();
    auto* depthSrv = g_rendererData
        ? reinterpret_cast<ID3D11ShaderResourceView*>(
              g_rendererData->depthStencilTargets[RT::idx(RT::Depth::kMain)].srViewDepth)
        : nullptr;
    auto* cachedDepthSrv = reinterpret_cast<ID3D11ShaderResourceView*>(g_depthSRV);
    if (!depthSrv) {
        depthSrv = cachedDepthSrv;
    }
    if (!device || !context || !depthSrv) return;

    int srcW = 0, srcH = 0;
    if (!GetDepthSourceDims(depthSrv, srcW, srcH)) return;
    static std::atomic<std::uintptr_t> s_loggedDepthSrv{ 0 };
    const auto depthKey = reinterpret_cast<std::uintptr_t>(depthSrv);
    if (s_loggedDepthSrv.exchange(depthKey, std::memory_order_relaxed) != depthKey) {
        REX::INFO("HiZCull: depth source SRV={} cachedGlobal={} dims={}x{} source={}",
                  static_cast<void*>(depthSrv),
                  static_cast<void*>(cachedDepthSrv),
                  srcW, srcH,
                  depthSrv == cachedDepthSrv ? "g_depthSRV/main" : "rendererData.mainDepth");
    }
    if (!EnsureResources(device, srcW, srcH)) return;

    CameraMeta writeMeta;
    if (!CaptureRenderedDepthCameraMeta(writeMeta, 60)) return;

    auto* ctx = reinterpret_cast<ID3D11DeviceContext*>(context);
    DispatchHiZBuild(ctx, depthSrv, srcW, srcH,
                      writeMeta.near_plane, writeMeta.far_plane);
    CopyHiZToStaging(ctx);
    s_stagingMeta[s_ringWriteIndex] = writeMeta;
    s_ringWriteIndex = (s_ringWriteIndex + 1) % kReadbackRing;
}

void OnMainAccumExit()
{
    // Always clear the gate; cheap relaxed write. The mode check is moot
    // here ??if the gate was never opened, this is a harmless redundant store.
    s_inMainAccum.store(false, std::memory_order_release);
    s_forcePassthroughThisFrame.store(false, std::memory_order_release);
}

void InvalidateForFrames(unsigned n)
{
    unsigned current = s_invalidateFrames.load(std::memory_order_relaxed);
    while (n > current &&
           !s_invalidateFrames.compare_exchange_weak(current, n,
                                                     std::memory_order_relaxed)) {
    }
}

}  // namespace HiZCull

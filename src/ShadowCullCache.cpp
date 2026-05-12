#include <PCH.h>
#include "ShadowCullCache.h"

#include <chrono>
#include <cmath>
#include <cstring>

namespace ShadowCullCache {

std::atomic<Mode> g_mode{ Mode::Off };

namespace {

// --- Verified addresses ----------------------------------------------------
//
// Target: BSShaderAccumulator::FinishAccumulating_ShadowMapOrMask.
//
// Iteration history:
//   1. OnVisible @ 0x14280eda0 — never called, address only appears in
//      a dead dispatch table at 0x143095310.
//   2. ProcessAllQueuedLights @ 0x1428104d0 — fired per frame but only
//      contributes ~0.1–0.26 ms/frame. Off by ~25x from the perf target.
//
// Telemetry from iteration 2 placed the real cost elsewhere. Following
// BSBatchRenderer::Draw's caller chain back up:
//
//     BSShaderAccumulator::FinishAccumulating_ShadowMapOrMask @ 0x14282e470
//        -> BSBatchRenderer::RenderBatches             @ 0x14287f380
//             -> BSBatchRenderer::Draw                 @ 0x14287ede0
//
// FinishAccumulating_ShadowMapOrMask is the shadow-pass finalizer — once
// per shadow source per frame, it walks the accumulator's accumulated
// passes and emits all caster draws into the shadow atlas slice. In our
// Boston RenderDoc capture that's the function ultimately responsible for
// the 2280 shadow draws (1905 of them for the sun cascade 0 alone).
//
// It's a static method (SAX) selected per-accumulator via a function
// pointer table set up in BSShaderManager::Initialize, so the symbol is
// only referenced once in the binary (no direct callers by name). Patching
// the function body catches all callers regardless of indirection depth.
//
// commonlibf4 Address Library IDs: 1358523 (OG) / 2317874 (AE).
//
// Prologue @ 0x14282e470 (verified via IDA disasm — register/immediate
// only, no RIP-relative ops):
//   14282e470  push rbx                       ; 2 bytes (REX-prefixed)
//   14282e472  sub  rsp, 20h                  ; 4 bytes
//   14282e476  cmp  qword ptr [rcx+10h], 0    ; 5 bytes
//   14282e47b  mov  rbx, rcx                  ; 3 bytes -> 14-byte boundary
// Lands cleanly at offset 14 just before `jz short loc_14282E4C6`.
REL::Relocation<std::uintptr_t> ptr_FinishAccumulating_ShadowMapOrMask{ REL::ID{ 1358523, 2317874 } };
constexpr std::size_t kPrologueSize = 14;

using FinishShadowAccum_t = void (*)(void* /*BSShaderAccumulator**/);
FinishShadowAccum_t s_original = nullptr;

// --- Gateway template (local copy) -----------------------------------------
//
// Mirror of Plugin.cpp's CreateBranchGateway. Kept module-local so this file
// stands alone — if/when more performance hooks share the same plumbing,
// extracting into a HookGateway.h is the obvious refactor. Behavior is
// identical: allocate trampoline space, copy `prologueSize` bytes, append a
// 14-byte absolute JMP back to `target + prologueSize`, then patch the target
// with a 14-byte absolute JMP to `hook`.
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

// --- Per-accumulator state -------------------------------------------------
//
// FinishAccumulating_ShadowMapOrMask is called once per shadow source per
// frame — RenderDoc capture shows 5 distinct shadow sub-passes (sun cascade
// 0, sun cascade 1, plus 3 spot/point lights), so 5 active accumulators.
//
// Iteration 2 used a SINGLE global s_prev which was overwritten on every
// call, producing the bogus "loosePos=100%" because consecutive calls
// within one frame compared against each other's identical cameras. Fix:
// linear-probe a small fixed map keyed by accumulator pointer so each
// shadow source compares against its own previous-frame state.
struct PerAccumState {
    void*         accum          = nullptr;  // key; nullptr = empty slot
    std::uint64_t prevHash       = 0;
    float         prevCamPos[3]  = { 0.0f, 0.0f, 0.0f };
    float         prevCamRow0[3] = { 1.0f, 0.0f, 0.0f };
    bool          valid          = false;
};
constexpr int kMaxAccums = 8;
PerAccumState s_perAccum[kMaxAccums];

PerAccumState& LookupOrInsert(void* accum)
{
    for (int i = 0; i < kMaxAccums; ++i) {
        if (s_perAccum[i].accum == accum) return s_perAccum[i];
    }
    for (int i = 0; i < kMaxAccums; ++i) {
        if (s_perAccum[i].accum == nullptr) {
            s_perAccum[i] = {};
            s_perAccum[i].accum = accum;
            return s_perAccum[i];
        }
    }
    s_perAccum[0] = {};
    s_perAccum[0].accum = accum;
    return s_perAccum[0];
}

// --- Hash inputs -----------------------------------------------------------
//
// Decompile of FinishAccumulating_ShadowMapOrMask + BSBatchRenderer::RenderBatches
// establishes the shadow accumulator's pass-list layout:
//
//   BSBatchRenderer is embedded at  accumulator + 0xC8.
//   Shadow uses group index 1 within the batch renderer:
//     firstIdx     = *(uint32_t*)(batchRenderer + 0x2E8)     (0xFFFF = empty)
//     nodeArrayPtr = *(void**)   (batchRenderer + 0x20)
//   Each linked-list node is 16 bytes:
//     +0   BSRenderPass*
//     +8   sortKey (uint32_t)
//     +12  nextIdx (uint16_t, 0xFFFF = end)
//     +14  padding
//   Per BSRenderPass fields hashed:
//     +8   BSShader*
//     +16  BSShaderProperty* (material)
//     +24  BSGeometry*
//     +72  pass flags (uint32_t)
//   Per BSGeometry we also hash the world translate (NiAVObject m_kWorld
//   m_Translate at geom+0x84) so caster movement invalidates the hash.
//
// We hash at most kMaxPassesHashed passes per call to bound observation cost
// at high cascade counts (sun cascade 0 = 1905 passes).
constexpr std::size_t   kAccumBatchRendererOffset = 0xC8;
constexpr std::size_t   kBR_Group1FirstIdxOffset  = 0x2E8;
constexpr std::size_t   kBR_Group1ArrayPtrOffset  = 0x20;
constexpr std::size_t   kPassShaderOffset         = 0x08;
constexpr std::size_t   kPassMaterialOffset       = 0x10;
constexpr std::size_t   kPassGeometryOffset       = 0x18;
constexpr std::size_t   kPassFlagsOffset          = 0x48;
constexpr std::size_t   kGeomWorldTranslateOffset = 0x84;
constexpr std::uint16_t kPassNodeListEnd          = 0xFFFFu;
constexpr std::size_t   kPassNodeSize             = 0x10;
constexpr std::size_t   kPassNodeNextIdxOffset    = 0x0C;
constexpr std::uint32_t kMaxPassesHashed          = 4096;
// FinishAccumulating_ShadowMapOrMask also calls RenderGeometryGroup(accum, 9)
// which reads accum + 8*(9+132) = accum + 0x468 — the GeometryGroup that
// holds precombines / merged geometry / instanced trees. This is where the
// bulk of Boston's 1900+ shadow draws live; without walking it our pass
// count is ~12 (just the accumulator's main group-1 list).
//
// The GeometryGroup layout per RenderGeometryGroup + GeometryGroup::Render:
//   +0x00  BSBatchRenderer*  (the GG's own sub-renderer with 13 std lists)
//   +0x08  PersistentPassList: head pointer at +0x08, tail at +0x10
//   +0x20  flag byte (bit 0 = use persistent list path)
//   +0x21  some byte read as the "+33" arg to GG sub-renderer's RenderBatches
// When bit 0 of +0x20 is set, shadow precombines render through the
// persistent path: a singly-linked BSRenderPass chain with `next` at
// pass+0x40, traversed by RenderPersistentPassListImpl until null.
constexpr std::size_t   kAccumGeomGroup9Offset    = 0x468;
constexpr std::size_t   kGG_PersistentHeadOffset  = 0x08;
constexpr std::size_t   kGG_FlagOffset            = 0x20;
constexpr std::size_t   kPersistPassNextOffset    = 0x40;

// Forward decls for the cheap mixers (defined further down — keeping them
// near the bottom because they're small inlines, and we need them here for
// the pass-list walker which is conceptually part of the "hash inputs"
// section).
inline std::uint64_t MixU64(std::uint64_t h, std::uint64_t v);
inline std::uint64_t HashBytes(std::uint64_t h, const void* data, std::size_t n);
// GeometryGroup-path diagnostic counters — defined in the Telemetry section
// below but consumed by HashGeometryGroup9. Forward-declare so order of
// definition doesn't dictate where the walker can sit.
extern std::atomic<std::uint64_t> a_ggNull;
extern std::atomic<std::uint64_t> a_ggPersistent;
extern std::atomic<std::uint64_t> a_ggSubRendererBR;

// Walks the shadow accumulator's pass list and folds per-pass content into
// a hash. Returns the running hash plus how many passes were walked (caller
// logs that to confirm we're seeing the expected ~1905 sun-cascade count).
std::uint64_t HashAccumulatorPassList(void* accum, std::uint64_t seed, std::uint32_t& outPassCount)
{
    outPassCount = 0;
    if (!accum) return seed;

    auto* accumBytes  = static_cast<std::uint8_t*>(accum);
    auto* batchRender = accumBytes + kAccumBatchRendererOffset;

    std::uint32_t idx = 0;
    std::memcpy(&idx, batchRender + kBR_Group1FirstIdxOffset, sizeof(idx));
    if ((idx & 0xFFFFu) == kPassNodeListEnd) {
        // empty list — fold the sentinel into the hash so "empty" doesn't
        // collide with "never set".
        return MixU64(seed, 0xDEADBEEFCAFEBABEull);
    }

    void* nodeArrayPtr = nullptr;
    std::memcpy(&nodeArrayPtr, batchRender + kBR_Group1ArrayPtrOffset, sizeof(nodeArrayPtr));
    if (!nodeArrayPtr) {
        return MixU64(seed, 0xBADC0FFEE0DDF00Dull);
    }

    auto* nodes = static_cast<std::uint8_t*>(nodeArrayPtr);
    std::uint64_t hash = seed;
    std::uint16_t cur = static_cast<std::uint16_t>(idx & 0xFFFFu);
    while (cur != kPassNodeListEnd && outPassCount < kMaxPassesHashed) {
        auto* node = nodes + (kPassNodeSize * cur);
        void* passPtr = nullptr;
        std::memcpy(&passPtr, node, sizeof(passPtr));

        if (passPtr) {
            auto* pass = static_cast<std::uint8_t*>(passPtr);

            void* shader = nullptr;
            void* mat    = nullptr;
            void* geom   = nullptr;
            std::uint32_t flags = 0;
            std::memcpy(&shader, pass + kPassShaderOffset,   sizeof(shader));
            std::memcpy(&mat,    pass + kPassMaterialOffset, sizeof(mat));
            std::memcpy(&geom,   pass + kPassGeometryOffset, sizeof(geom));
            std::memcpy(&flags,  pass + kPassFlagsOffset,    sizeof(flags));

            hash = MixU64(hash, reinterpret_cast<std::uintptr_t>(shader));
            hash = MixU64(hash, reinterpret_cast<std::uintptr_t>(mat));
            hash = MixU64(hash, reinterpret_cast<std::uintptr_t>(geom));
            hash = MixU64(hash, flags);

            if (geom) {
                float worldT[3] = { 0.0f, 0.0f, 0.0f };
                std::memcpy(worldT,
                            static_cast<std::uint8_t*>(geom) + kGeomWorldTranslateOffset,
                            sizeof(worldT));
                hash = HashBytes(hash, worldT, sizeof(worldT));
            }
        }

        std::uint16_t next = 0;
        std::memcpy(&next, node + kPassNodeNextIdxOffset, sizeof(next));
        cur = next;
        ++outPassCount;
    }
    return hash;
}

// Hashes the GeometryGroup-9's persistent pass list (the path FO4 uses for
// precombines and merged geometry in the world shadow pass). This is where
// the ~1900-draw sun cascade 0 traffic actually goes — group-1 only has a
// handful of dynamic shadow casters.
//
// Walks a singly-linked BSRenderPass chain starting at *(GG + 8), following
// `next` at pass+0x40 until null. For each pass, folds in the same per-pass
// fingerprint as HashAccumulatorPassList plus the geometry's world translate.
//
// We also fold in the GG pointer, the GG flag byte, and the GG's own
// BSBatchRenderer pointer regardless of which path is in use, so the hash
// catches "the engine swapped to a different group" even when the persistent
// path is empty.
// Walks one BSBatchRenderer group list (the 16-byte-node array-linked format,
// matching HashAccumulatorPassList) for a sub-BatchRenderer at given group
// index `groupIdx`. Returns updated hash; appends to outPassCount.
std::uint64_t HashBatchRendererGroupList(void* batchRenderer, std::uint32_t groupIdx,
                                         std::uint64_t seed, std::uint32_t& outPassCount)
{
    if (!batchRenderer) return seed;
    auto* br = static_cast<std::uint8_t*>(batchRenderer);

    // firstIdx for group g = *(DWORD*)(br + 736 + 8*g)  i.e. br + 0x2E0 + 8*g
    std::uint32_t firstIdx = 0;
    std::memcpy(&firstIdx, br + (736 + 8 * groupIdx), sizeof(firstIdx));
    if ((firstIdx & 0xFFFFu) == kPassNodeListEnd) return seed;

    // arrayPtr for group g = *(QWORD*)(br + 24*g + 8)
    void* nodeArrayPtr = nullptr;
    std::memcpy(&nodeArrayPtr, br + (24 * groupIdx + 8), sizeof(nodeArrayPtr));
    if (!nodeArrayPtr) return seed;

    auto* nodes = static_cast<std::uint8_t*>(nodeArrayPtr);
    std::uint64_t h = seed;
    std::uint16_t cur = static_cast<std::uint16_t>(firstIdx & 0xFFFFu);
    while (cur != kPassNodeListEnd && outPassCount < kMaxPassesHashed) {
        auto* node = nodes + (kPassNodeSize * cur);
        void* passPtr = nullptr;
        std::memcpy(&passPtr, node, sizeof(passPtr));
        if (passPtr) {
            auto* pass = static_cast<std::uint8_t*>(passPtr);
            void* shader = nullptr;
            void* mat    = nullptr;
            void* geom   = nullptr;
            std::uint32_t flags = 0;
            std::memcpy(&shader, pass + kPassShaderOffset,   sizeof(shader));
            std::memcpy(&mat,    pass + kPassMaterialOffset, sizeof(mat));
            std::memcpy(&geom,   pass + kPassGeometryOffset, sizeof(geom));
            std::memcpy(&flags,  pass + kPassFlagsOffset,    sizeof(flags));
            h = MixU64(h, reinterpret_cast<std::uintptr_t>(shader));
            h = MixU64(h, reinterpret_cast<std::uintptr_t>(mat));
            h = MixU64(h, reinterpret_cast<std::uintptr_t>(geom));
            h = MixU64(h, flags);
            if (geom) {
                float worldT[3] = { 0.0f, 0.0f, 0.0f };
                std::memcpy(worldT,
                            static_cast<std::uint8_t*>(geom) + kGeomWorldTranslateOffset,
                            sizeof(worldT));
                h = HashBytes(h, worldT, sizeof(worldT));
            }
        }
        std::uint16_t next = 0;
        std::memcpy(&next, node + kPassNodeNextIdxOffset, sizeof(next));
        cur = next;
        ++outPassCount;
    }
    return h;
}

// Hashes the shadow accumulator's GeometryGroup-9, branching based on the
// "use persistent path" flag at GG+0x20. Updates diagnostic counters so the
// periodic log shows us which path the engine is using.
std::uint64_t HashGeometryGroup9(void* accum, std::uint64_t seed, std::uint32_t& outPassCount)
{
    if (!accum) return seed;
    void* gg = nullptr;
    std::memcpy(&gg, static_cast<std::uint8_t*>(accum) + kAccumGeomGroup9Offset, sizeof(gg));
    std::uint64_t h = MixU64(seed, reinterpret_cast<std::uintptr_t>(gg));
    if (!gg) {
        a_ggNull.fetch_add(1, std::memory_order_relaxed);
        return h;
    }

    auto* ggBytes = static_cast<std::uint8_t*>(gg);

    void* subBatchRenderer = nullptr;
    std::memcpy(&subBatchRenderer, ggBytes + 0, sizeof(subBatchRenderer));
    h = MixU64(h, reinterpret_cast<std::uintptr_t>(subBatchRenderer));

    std::uint8_t flag = 0;
    std::memcpy(&flag, ggBytes + kGG_FlagOffset, sizeof(flag));
    h = MixU64(h, flag);

    if ((flag & 1) != 0) {
        // Persistent linked-list path (precombines / merged geometry).
        a_ggPersistent.fetch_add(1, std::memory_order_relaxed);
        void* head = nullptr;
        std::memcpy(&head, ggBytes + kGG_PersistentHeadOffset, sizeof(head));
        void* cur = head;
        std::uint32_t guard = 0;
        while (cur && outPassCount < kMaxPassesHashed && guard < kMaxPassesHashed) {
            auto* pass = static_cast<std::uint8_t*>(cur);
            void* shader = nullptr;
            void* mat    = nullptr;
            void* geom   = nullptr;
            std::uint32_t flags = 0;
            std::memcpy(&shader, pass + kPassShaderOffset,   sizeof(shader));
            std::memcpy(&mat,    pass + kPassMaterialOffset, sizeof(mat));
            std::memcpy(&geom,   pass + kPassGeometryOffset, sizeof(geom));
            std::memcpy(&flags,  pass + kPassFlagsOffset,    sizeof(flags));
            h = MixU64(h, reinterpret_cast<std::uintptr_t>(shader));
            h = MixU64(h, reinterpret_cast<std::uintptr_t>(mat));
            h = MixU64(h, reinterpret_cast<std::uintptr_t>(geom));
            h = MixU64(h, flags);
            if (geom) {
                float worldT[3] = { 0.0f, 0.0f, 0.0f };
                std::memcpy(worldT,
                            static_cast<std::uint8_t*>(geom) + kGeomWorldTranslateOffset,
                            sizeof(worldT));
                h = HashBytes(h, worldT, sizeof(worldT));
            }
            void* next = nullptr;
            std::memcpy(&next, pass + kPersistPassNextOffset, sizeof(next));
            cur = next;
            ++outPassCount;
            ++guard;
        }
    } else {
        // 13-list path. GG owns its own BSBatchRenderer at GG+0; iterate
        // groups 0..12 of THAT renderer using the same node format as
        // HashAccumulatorPassList.
        a_ggSubRendererBR.fetch_add(1, std::memory_order_relaxed);
        if (subBatchRenderer) {
            for (std::uint32_t g = 0; g < 13; ++g) {
                h = HashBatchRendererGroupList(subBatchRenderer, g, h, outPassCount);
                if (outPassCount >= kMaxPassesHashed) break;
            }
        }
    }
    return h;
}

// FNV-1a-ish mix; cheap and avalanche-y enough for hit-rate measurement.
inline std::uint64_t MixU64(std::uint64_t h, std::uint64_t v)
{
    h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
    return h;
}

inline std::uint64_t HashBytes(std::uint64_t h, const void* data, std::size_t n)
{
    const auto* p = static_cast<const std::uint8_t*>(data);
    // Hash 8 bytes at a time when aligned; tail handled by zero-extending.
    std::size_t i = 0;
    while (i + 8 <= n) {
        std::uint64_t chunk;
        std::memcpy(&chunk, p + i, sizeof(chunk));
        h = MixU64(h, chunk);
        i += 8;
    }
    if (i < n) {
        std::uint64_t tail = 0;
        std::memcpy(&tail, p + i, n - i);
        h = MixU64(h, tail);
    }
    return h;
}

// Defensive camera-world-transform reader. Returns false (and leaves
// out-params untouched) if the camera pointer or its expected world-transform
// slot looks unreadable. We don't dereference past the world transform's
// rotation row 0 + translate, so the cost on a hit is two cache lines max.
//
// Layout assumption (NiAVObject world NiTransform):
//   +0x60  m_kWorld.m_Rotate     (NiMatrix3 — 3x3 floats; row 0 = first 12 bytes)
//   +0x84  m_kWorld.m_Translate  (NiPoint3 — 3 floats)
// These offsets are conservative best-guesses for FO4's NiAVObject layout
// and are validated empirically by the Phase 1 telemetry — if our reads land
// in junk memory, the hash will simply never repeat and exactHashHits will
// stay at zero, which is itself a signal to revisit the offsets.
bool ReadCameraTransform(void* cameraPtr, float outRow0[3], float outTranslate[3])
{
    if (!cameraPtr) {
        return false;
    }
    auto* base = static_cast<std::uint8_t*>(cameraPtr);
    std::memcpy(outRow0, base + 0x60, sizeof(float) * 3);
    std::memcpy(outTranslate, base + 0x84, sizeof(float) * 3);
    return true;
}

// --- Telemetry -------------------------------------------------------------
//
// Counters are atomic so the hook is safe to run on worker threads. The
// culling system has multi-threaded job arenas (see AccumulatePassesFromArena
// SubGroupAlloc instantiation), so OnVisible can plausibly fire concurrently
// from CullingJob workers. Per-node state is racy if so, but the only effect
// is occasional missed hash hits on the next call — acceptable for telemetry.
std::atomic<std::uint64_t> a_calls{ 0 };
std::atomic<std::uint64_t> a_contentHits{ 0 };    // content hash matched (camera-agnostic)
std::atomic<std::uint64_t> a_loosePosHits{ 0 };   // camera position within epsilon
std::atomic<std::uint64_t> a_looseRotHits{ 0 };   // camera rotation within epsilon
std::atomic<std::uint64_t> a_combinedHits{ 0 };   // content + loosePos + looseRot all true -> safe-to-skip
std::atomic<std::uint64_t> a_skips{ 0 };          // Mode::Cache only — actual times we returned early
std::atomic<std::uint64_t> a_totalNanos{ 0 };
std::atomic<std::uint64_t> a_maxNanos{ 0 };
std::atomic<std::uint64_t> a_totalPasses{ 0 };    // sum of pass counts seen across the window
std::atomic<std::uint64_t> a_maxPasses{ 0 };      // largest single pass count seen
// Diagnostic counters for the GeometryGroup-9 path so we can see why pass
// count is low. One of these gets incremented per call depending on what
// state the GG is in.
std::atomic<std::uint64_t> a_ggNull{ 0 };          // accum+0x468 was null (no GG attached)
std::atomic<std::uint64_t> a_ggPersistent{ 0 };    // GG present, flag bit 0 set (persistent list path)
std::atomic<std::uint64_t> a_ggSubRendererBR{ 0 }; // GG present, flag bit 0 clear (13-list path)

constexpr float kLoosePosEpsilonSq    = 0.5f * 0.5f;  // <= 0.5 unit total motion
constexpr float kLooseRotDotEpsilon   = 0.999f;       // row0 . row0' > 0.999

// Periodic log throttling. We log on power-of-two call boundaries once
// `kLogSecondInterval` wall-clock seconds have elapsed since the last log.
// Tuned tight so the first measurement appears within ~2s of gameplay — at
// ~5 shadow viewports/frame × 30fps that's ~150 calls/s, so the first log
// fires at 256 calls (~1.7s).
constexpr std::uint64_t kLogCallInterval   = 256;
constexpr double        kLogSecondInterval = 2.0;
std::chrono::steady_clock::time_point s_lastLogTime;
std::atomic<bool> s_firstCallLogged{ false };

void MaybeLog()
{
    const auto calls = a_calls.load(std::memory_order_relaxed);
    if (calls == 0 || (calls & (kLogCallInterval - 1)) != 0) {
        // Only check the time budget on power-of-two boundaries to avoid
        // the steady_clock cost on every call.
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    const double secs = std::chrono::duration<double>(now - s_lastLogTime).count();
    if (secs < kLogSecondInterval) {
        return;
    }
    s_lastLogTime = now;

    const auto contentH = a_contentHits.exchange(0, std::memory_order_relaxed);
    const auto loosePos = a_loosePosHits.exchange(0, std::memory_order_relaxed);
    const auto looseRot = a_looseRotHits.exchange(0, std::memory_order_relaxed);
    const auto combined = a_combinedHits.exchange(0, std::memory_order_relaxed);
    const auto skips    = a_skips.exchange(0, std::memory_order_relaxed);
    const auto totalNs  = a_totalNanos.exchange(0, std::memory_order_relaxed);
    const auto maxNs    = a_maxNanos.exchange(0, std::memory_order_relaxed);
    const auto totalP   = a_totalPasses.exchange(0, std::memory_order_relaxed);
    const auto maxP     = a_maxPasses.exchange(0, std::memory_order_relaxed);
    const auto ggNull   = a_ggNull.exchange(0, std::memory_order_relaxed);
    const auto ggPer    = a_ggPersistent.exchange(0, std::memory_order_relaxed);
    const auto ggSub    = a_ggSubRendererBR.exchange(0, std::memory_order_relaxed);
    const auto winCalls = a_calls.exchange(0, std::memory_order_relaxed);

    if (winCalls == 0) {
        return;
    }
    const double callsD        = static_cast<double>(winCalls);
    const double avgUs         = (totalNs / 1000.0) / callsD;
    const double maxUs         = maxNs / 1000.0;
    const double totalMsPerSec = (totalNs / 1'000'000.0) / secs;
    const double avgPasses     = static_cast<double>(totalP) / callsD;
    const Mode mode = g_mode.load(std::memory_order_relaxed);
    const char* modeStr = (mode == Mode::Cache) ? "cache" : "measure";
    REX::INFO(
        "ShadowCullCache[{}]: calls={} over {:.2f}s ({:.0f}/s) "
        "avg={:.2f}us max={:.2f}us totalMs/s={:.2f} "
        "passes/call avg={:.0f} max={} "
        "GG[null={} persistent={} subBR={}] "
        "content={:.1f}% camPos={:.1f}% camRot={:.1f}% combined={:.1f}% skipped={} ({:.1f}%) "
        "(savableMs/s={:.2f})",
        modeStr,
        winCalls, secs, callsD / secs,
        avgUs, maxUs, totalMsPerSec,
        avgPasses, maxP,
        ggNull, ggPer, ggSub,
        100.0 * contentH / callsD,
        100.0 * loosePos / callsD,
        100.0 * looseRot / callsD,
        100.0 * combined / callsD,
        skips,
        100.0 * static_cast<double>(skips) / callsD,
        (totalNs / 1'000'000.0) * (static_cast<double>(combined) / callsD) / secs);
}

// BSShaderManager::spCamera global — set by the engine before each shadow
// pass so the accumulator's downstream code knows which view it's finalizing
// for. We read it (defensively) for the cam transform hash.
constexpr std::uintptr_t kAddr_BSShaderManagerSpCamera = 0x146721ae0;

// --- Hook entry ------------------------------------------------------------
void __fastcall HookedFinishShadowAccum(void* accum)
{
    const Mode mode = g_mode.load(std::memory_order_relaxed);
    if (mode == Mode::Off) {
        // Should not happen — Off means we never installed — but bail safely.
        s_original(accum);
        return;
    }

    if (!s_firstCallLogged.exchange(true, std::memory_order_relaxed)) {
        REX::INFO(
            "ShadowCullCache: first FinishAccumulating_ShadowMapOrMask call observed (accum={}, mode={})",
            accum,
            mode == Mode::Cache ? "cache" : "measure");
    }

    // --- Compute hashes ---
    //
    // We track TWO independent signals:
    //
    //   contentHash — scene-only state, NO camera:
    //     accumulator pointer + GeometryGroup pointer at accum+0x468
    //     + every group-1 pass {shader,material,geometry,flags,geom world translate}.
    //     Hits when no caster moved / was added / was removed since last frame,
    //     regardless of whether the player turned the camera. This is the
    //     "would a cache that ignores camera be safe?" upper bound.
    //
    //   Camera state — tracked separately via prevCamPos/prevCamRow0:
    //     loose-position match + loose-rotation match against per-accumulator
    //     previous state. TAA jitter is below the kLoosePosEpsilon threshold
    //     and stays a "match" — appropriate because the shadow projection
    //     resolution doesn't care about sub-pixel camera changes.
    //
    // A real cache would only skip when contentHash matches AND camera is
    // within tolerance — that's the `combinedHit` metric below.
    std::uint32_t passCount = 0;
    std::uint64_t contentHash = MixU64(0, reinterpret_cast<std::uintptr_t>(accum));

    // Walk the accumulator's group-1 standard pass list (dynamic shadow
    // casters — typically a small handful).
    contentHash = HashAccumulatorPassList(accum, contentHash, passCount);

    // Walk the GeometryGroup-9 persistent pass list — precombined / merged
    // geometry. This is where the bulk of Boston shadow draws come from
    // (~1900 for sun cascade 0). passCount accumulates across both lists,
    // so the periodic log's "passes/call max" tells us whether we're now
    // actually seeing the real draw counts vs. the prior ~12.
    contentHash = HashGeometryGroup9(accum, contentHash, passCount);

    // Read the live shadow camera pointer (set by the engine right before
    // this call) and pull its world transform for the loose-tolerance check.
    void* cameraPtr = nullptr;
    std::memcpy(&cameraPtr,
                reinterpret_cast<const void*>(kAddr_BSShaderManagerSpCamera),
                sizeof(cameraPtr));

    float camRow0[3]      = { 1.0f, 0.0f, 0.0f };
    float camTranslate[3] = { 0.0f, 0.0f, 0.0f };
    const bool readCam = ReadCameraTransform(cameraPtr, camRow0, camTranslate);

    const std::uint64_t hash = contentHash;  // alias the name kept by the diff below

    // --- Compare against per-accumulator previous state ---
    //
    // Three independent signals:
    //   contentMatch — content hash equality (no camera in hash)
    //   camStable    — camera within both position AND rotation tolerance
    //   combined     — contentMatch && camStable
    //
    // `combined` is the metric that says "safe to skip"; the other two help
    // diagnose why hit rates are low if/when they are.
    auto& state = LookupOrInsert(accum);
    bool contentMatch = false;
    bool loosePos     = false;
    bool looseRot     = false;
    if (state.valid) {
        contentMatch = (state.prevHash == hash);
        if (readCam) {
            const float dx = camTranslate[0] - state.prevCamPos[0];
            const float dy = camTranslate[1] - state.prevCamPos[1];
            const float dz = camTranslate[2] - state.prevCamPos[2];
            const float dist2 = dx * dx + dy * dy + dz * dz;
            loosePos = (dist2 <= kLoosePosEpsilonSq);

            const float dot = camRow0[0] * state.prevCamRow0[0]
                            + camRow0[1] * state.prevCamRow0[1]
                            + camRow0[2] * state.prevCamRow0[2];
            looseRot = (std::fabs(dot) >= kLooseRotDotEpsilon);
        } else {
            // No camera reading — be conservative; treat as moved.
            loosePos = false;
            looseRot = false;
        }
    }
    const bool combined = contentMatch && loosePos && looseRot;

    // --- Mode::Cache: actually skip on combined match -----------------------
    //
    // The previous frame's shadow atlas slice persists on the GPU because
    // D3D11 textures aren't auto-cleared between frames; if nothing in the
    // input set (camera + casters + materials) changed, the GPU output
    // would be byte-identical, so reading the stale slice produces a
    // correct shadow.
    //
    // RISK: if the engine clears the shadow depth target upstream of this
    // function (e.g. as part of binding it in RenderSceneDeferred), the
    // slice is empty when we skip and shadows visibly disappear that frame.
    // We mitigate by ONLY skipping when ALL three signals agree
    // (contentMatch + camStable position + camStable rotation) AND the
    // accumulator's prev state has been validated at least once. If the
    // user reports visual flicker, the next iteration is to hook upstream
    // to also suppress the clear.
    if (mode == Mode::Cache && combined) {
        a_calls.fetch_add(1, std::memory_order_relaxed);
        a_contentHits.fetch_add(1, std::memory_order_relaxed);
        a_loosePosHits.fetch_add(1, std::memory_order_relaxed);
        a_looseRotHits.fetch_add(1, std::memory_order_relaxed);
        a_combinedHits.fetch_add(1, std::memory_order_relaxed);
        a_skips.fetch_add(1, std::memory_order_relaxed);
        a_totalPasses.fetch_add(passCount, std::memory_order_relaxed);
        // No timing fetch — we didn't run the original. Don't update
        // state.prev* either: keep last-rendered state authoritative so we
        // don't accumulate hash drift across many consecutive skipped frames.
        MaybeLog();
        return;
    }

    // --- Run original, time it ---
    const auto t0 = std::chrono::steady_clock::now();
    s_original(accum);
    const auto t1 = std::chrono::steady_clock::now();
    const auto ns = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());

    // --- Update telemetry ---
    a_calls.fetch_add(1, std::memory_order_relaxed);
    if (contentMatch) a_contentHits.fetch_add(1, std::memory_order_relaxed);
    if (loosePos)     a_loosePosHits.fetch_add(1, std::memory_order_relaxed);
    if (looseRot)     a_looseRotHits.fetch_add(1, std::memory_order_relaxed);
    if (combined)     a_combinedHits.fetch_add(1, std::memory_order_relaxed);
    a_totalNanos.fetch_add(ns, std::memory_order_relaxed);
    a_totalPasses.fetch_add(passCount, std::memory_order_relaxed);
    std::uint64_t prev = a_maxNanos.load(std::memory_order_relaxed);
    while (ns > prev && !a_maxNanos.compare_exchange_weak(prev, ns, std::memory_order_relaxed)) {
    }
    std::uint64_t prevP = a_maxPasses.load(std::memory_order_relaxed);
    while (passCount > prevP && !a_maxPasses.compare_exchange_weak(prevP, passCount, std::memory_order_relaxed)) {
    }

    // --- Stash per-accumulator state ---
    state.prevHash = hash;
    if (readCam) {
        std::memcpy(state.prevCamPos,  camTranslate, sizeof(camTranslate));
        std::memcpy(state.prevCamRow0, camRow0,      sizeof(camRow0));
    }
    state.valid = true;

    MaybeLog();
}

bool s_installed = false;

}  // anonymous namespace

bool Initialize()
{
    if (s_installed) {
        REX::INFO("ShadowCullCache::Initialize: already installed; skipping");
        return true;
    }
    const auto mode = g_mode.load(std::memory_order_relaxed);
    REX::INFO("ShadowCullCache::Initialize: mode={}",
              mode == Mode::Off ? "off" : (mode == Mode::Measure ? "measure" : "unknown"));
    if (mode == Mode::Off) {
        REX::INFO("ShadowCullCache::Initialize: mode=off, hook not installed");
        return true;
    }

    // Guard against unresolved REL::ID — refuse to patch if address() doesn't
    // land in the Fallout4.exe text segment (.text starts at 0x140000000+).
    const std::uintptr_t targetAddr = ptr_FinishAccumulating_ShadowMapOrMask.address();
    if (targetAddr < 0x140000000ull) {
        REX::WARN(
            "ShadowCullCache::Initialize: FinishAccumulating_ShadowMapOrMask address resolution failed (got {:#x}); "
            "REL::ID likely missing for this runtime. Hook not installed.",
            targetAddr);
        g_mode.store(Mode::Off, std::memory_order_relaxed);
        return false;
    }

    s_original = CreateBranchGateway<FinishShadowAccum_t>(
        ptr_FinishAccumulating_ShadowMapOrMask,
        kPrologueSize,
        reinterpret_cast<void*>(&HookedFinishShadowAccum));

    if (!s_original) {
        REX::WARN("ShadowCullCache::Initialize: gateway install failed at {:#x}",
                  targetAddr);
        g_mode.store(Mode::Off, std::memory_order_relaxed);
        return false;
    }

    s_installed   = true;
    s_lastLogTime = std::chrono::steady_clock::now();
    REX::INFO(
        "ShadowCullCache::Initialize: hooked BSShaderAccumulator::FinishAccumulating_ShadowMapOrMask @ {:#x} (mode={}, log every {} calls / {:.1f}s)",
        targetAddr,
        mode == Mode::Cache ? "cache" : "measure",
        kLogCallInterval,
        kLogSecondInterval);

    // Dump the first 14 bytes at the patch site. Expected: FF 25 00 00 00 00
    // <8-byte abs addr of HookedFinishShadowAccum>.
    {
        const auto* p = reinterpret_cast<const std::uint8_t*>(targetAddr);
        REX::INFO(
            "ShadowCullCache::Initialize: patch site bytes = {:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x}, hook fn @ {:#x}",
            p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7],
            p[8], p[9], p[10], p[11], p[12], p[13],
            reinterpret_cast<std::uintptr_t>(&HookedFinishShadowAccum));
    }
    return true;
}

WindowStats GetStats()
{
    WindowStats s{};
    s.calls           = a_calls.load(std::memory_order_relaxed);
    s.exactHashHits   = a_contentHits.load(std::memory_order_relaxed);
    s.loosePosHits    = a_loosePosHits.load(std::memory_order_relaxed);
    s.looseRotHits    = a_looseRotHits.load(std::memory_order_relaxed);
    s.totalMicros     = a_totalNanos.load(std::memory_order_relaxed) / 1000.0;
    s.maxMicros       = a_maxNanos.load(std::memory_order_relaxed) / 1000.0;
    return s;
}

}  // namespace ShadowCullCache

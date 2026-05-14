#include <PCH.h>
#include "PhaseTelemetry.h"
#include "LightSorter.h"

#include <chrono>
#include <cstring>

namespace PhaseTelemetry {

std::atomic<Mode> g_mode{ Mode::Off };

namespace {

std::atomic<bool> s_forceHooks{ false };

// --- Hook targets ----------------------------------------------------------
//
// The per-frame world render entry on OG 1.10.163 is
//   Main::DrawWorld_And_UI                @ 0x140D3CBE0
//     └─ DrawWorld::Render_PreUI          @ 0x142857480
//          ├─ DrawWorld::LightUpdate
//          ├─ DrawWorld::MainAccum            (cull + AccumulateSceneArray)
//          ├─ DrawWorld::MainRenderSetup
//          ├─ DrawWorld::OpaqueWireframe
//          ├─ DrawWorld::DeferredPrePass      ← real G-buffer Pre
//          ├─ DrawWorld::UpdateMotionBlur
//          ├─ DrawWorld::DeferredDecals
//          ├─ DrawWorld::ImagespaceSAO
//          ├─ DrawWorld::NvidiaHBAO           (conditional)
//          ├─ DrawWorld::DeferredLightsImpl
//          ├─ DrawWorld::DeferredComposite
//          ├─ DrawWorld::Forward              (alpha; eventually calls Post)
//          └─ DrawWorld::Refraction
//
// We hook Render_PreUI as the outer "Frame" bucket and each *hookable*
// DrawWorld:: sub-phase as its own bucket. Five sub-phases are intentionally
// skipped because their prologues contain a RIP-relative memory access inside
// the first 14 bytes with no preceding clean boundary at byte ≥ 5
// (LightUpdate / OpaqueWireframe / UpdateMotionBlur / ImagespaceSAO /
// Refraction). Our gateway templates don't relocate rel32 displacements; their
// wall time will fall into the "unaccounted" residual = Frame total − Σ children.
//
// OG REL::IDs are from tools/version-1-10-163-0-e.txt. AE IDs are set to 0
// per user direction ("I don't care about NG/AE crashing") — the Initialize()
// path declines to install if the resolved address is outside .text. On AE
// the entire module no-ops cleanly; no telemetry, but no crash either.
//
// Per-target prologue sizes (hand-disassembled in OG):
//   Render_PreUI        984743   E9-5   8B   mov r11,rsp; push rsi; sub rsp,0x70
//                                            (RIP-rel movzx at byte 8)
//   MainAccum           718911   E9-5   9B   push rbx; push rsi; push r13; sub rsp,0x50
//                                            (RIP-rel mov at byte 9)
//   MainRenderSetup     339369   E9-5   6B   sub rsp,0x38; xor edx,edx
//                                            (RIP-rel lea at byte 6)
//   DeferredPrePass      56596   JMP14 15B   3× stack save mov [rsp+disp8],reg
//   DeferredDecals      631771   JMP14 17B   mov [rsp+0x18],rbx + 5 pushes + lea rbp
//   NvidiaHBAO          253972   JMP14 15B   mov rax,rsp + 3 saves
//   DeferredLightsImpl 1108521   JMP14 18B   mov rax,rsp; push rbp; lea rbp; sub rsp
//   DeferredComposite   728427   JMP14 15B   mov rax,rsp + 2 saves + 5 pushes
//   Forward             656535   E9-5   6B   mov [rsp+8],rbx; push rdi
//                                            (RIP-rel mov at byte 10)

REL::Relocation<std::uintptr_t> ptr_RenderPreUI        { REL::ID{  984743, 0 } };
REL::Relocation<std::uintptr_t> ptr_MainAccum          { REL::ID{  718911, 0 } };
REL::Relocation<std::uintptr_t> ptr_MainRenderSetup    { REL::ID{  339369, 0 } };
REL::Relocation<std::uintptr_t> ptr_DeferredPrePass    { REL::ID{   56596, 0 } };
REL::Relocation<std::uintptr_t> ptr_DeferredDecals     { REL::ID{  631771, 0 } };
REL::Relocation<std::uintptr_t> ptr_NvidiaHBAO         { REL::ID{  253972, 0 } };
REL::Relocation<std::uintptr_t> ptr_DeferredLightsImpl { REL::ID{ 1108521, 0 } };
REL::Relocation<std::uintptr_t> ptr_DeferredComposite  { REL::ID{  728427, 0 } };
REL::Relocation<std::uintptr_t> ptr_Forward            { REL::ID{  656535, 0 } };
// Tier 0: the five "skipped" phases, hooked via relocating gateway.
// All five share the prologue pattern `sub rsp,0x38` (4B) + one 7B RIP-rel
// instruction (mov [rip+disp], cmp [rip+disp], 0). prologueSize=11 captures
// both; the gateway relocator adjusts the single RIP-rel disp32.
REL::Relocation<std::uintptr_t> ptr_LightUpdate        { REL::ID{  102390, 0 } };
REL::Relocation<std::uintptr_t> ptr_OpaqueWireframe    { REL::ID{ 1268987, 0 } };
REL::Relocation<std::uintptr_t> ptr_UpdateMotionBlur   { REL::ID{ 1051891, 0 } };
REL::Relocation<std::uintptr_t> ptr_ImagespaceSAO      { REL::ID{   39691, 0 } };
REL::Relocation<std::uintptr_t> ptr_Refraction         { REL::ID{ 1572250, 0 } };

constexpr std::size_t kPrologueRenderPreUI         =  8;  // E9-5 patch
constexpr std::size_t kPrologueMainAccum           =  9;  // E9-5 patch
constexpr std::size_t kPrologueMainRenderSetup     =  6;  // E9-5 patch
constexpr std::size_t kPrologueDeferredPrePass     = 15;  // JMP14
constexpr std::size_t kPrologueDeferredDecals      = 17;  // JMP14
constexpr std::size_t kPrologueNvidiaHBAO          = 15;  // JMP14
constexpr std::size_t kPrologueDeferredLightsImpl  = 18;  // JMP14
constexpr std::size_t kPrologueDeferredComposite   = 15;  // JMP14
constexpr std::size_t kPrologueForward             =  6;  // E9-5 patch
constexpr std::size_t kPrologueTier0_SubAndRipRel  = 11;  // E9-5 + relocator

// All hooked functions are `void(void)` on OG.
using VoidVoid_t = void (*)();

VoidVoid_t s_origRenderPreUI        = nullptr;
VoidVoid_t s_origMainAccum          = nullptr;
VoidVoid_t s_origMainRenderSetup    = nullptr;
VoidVoid_t s_origDeferredPrePass    = nullptr;
VoidVoid_t s_origDeferredDecals     = nullptr;
VoidVoid_t s_origNvidiaHBAO         = nullptr;
VoidVoid_t s_origDeferredLightsImpl = nullptr;
VoidVoid_t s_origDeferredComposite  = nullptr;
VoidVoid_t s_origForward            = nullptr;
VoidVoid_t s_origLightUpdate        = nullptr;
VoidVoid_t s_origOpaqueWireframe    = nullptr;
VoidVoid_t s_origUpdateMotionBlur   = nullptr;
VoidVoid_t s_origImagespaceSAO      = nullptr;
VoidVoid_t s_origRefraction         = nullptr;

// --- Gateway templates -----------------------------------------------------
//
// CreateBranchGateway:  patches target with a 14-byte JMP14 (FF 25 + 8-byte
// absolute). Use when prologueSize >= 14 AND captured bytes contain no
// RIP-relative ops.
//
// CreateBranchGateway5: patches target with a 5-byte E9 rel32. Use when the
// only clean boundary >= 5 is below 14 (e.g. a RIP-relative op starts at byte
// 8 and we need to cut at 8). Captured bytes still must contain no RIP-rel.
//
// Both copy `prologueSize` bytes from the target into a freshly-allocated
// gateway region and append a JMP14 back to `target + prologueSize`.

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

template <class T>
T CreateBranchGateway5(REL::Relocation<std::uintptr_t>& target, std::size_t prologueSize, void* hook)
{
    const auto targetAddress = target.address();
    auto& trampoline = REL::GetTrampoline();
    auto* gateway = static_cast<std::byte*>(trampoline.allocate(prologueSize + sizeof(REL::ASM::JMP14)));
    std::memcpy(gateway, reinterpret_cast<void*>(targetAddress), prologueSize);
    const REL::ASM::JMP14 jumpBack{ targetAddress + prologueSize };
    std::memcpy(gateway + prologueSize, &jumpBack, sizeof(jumpBack));
    trampoline.write_jmp5(targetAddress, reinterpret_cast<std::uintptr_t>(hook));
    return reinterpret_cast<T>(gateway);
}

// --- RIP-relative prologue rewriter ----------------------------------------
//
// Emits a relocated copy of `prologueSize` original bytes into `gateway`.
// Non-RIP-rel instructions are copied verbatim. RIP-relative instructions are
// **rewritten** to use absolute 64-bit addressing instead of patched disp32,
// because in practice commonlibf4 allocates its trampoline arena ~2 GB below
// the FO4 image, while the rip-rel globals in DrawWorld:: prologues sit ~0.1 GB
// above the image — total displacement from the gateway exceeds INT32_MAX,
// so disp32-patching can never succeed regardless of arena position.
//
// Rewrites (each grows the instruction by 6 bytes):
//
//   48 8B 0D <disp32>   mov rcx, [rip+disp]   (7B)
//      → 48 B9 <abs64> ; 48 8B 09             (13B)  ; mov rcx, abs ; mov rcx, [rcx]
//   48 8B 05 <disp32>   mov rax, [rip+disp]   (7B)
//      → 48 B8 <abs64> ; 48 8B 00             (13B)
//   80 3D <disp32> <imm8>  cmp byte [rip+disp], imm8   (7B)
//      → 48 B8 <abs64> ; 80 38 <imm8>         (13B)  ; uses rax as scratch
//
// The cmp rewrite clobbers rax. Safe for our 5 targets because:
//   - rax at function entry is undefined (caller's leftover).
//   - The cmp's only useful output is RFLAGS, consumed by the jcc immediately
//     after our captured range. RFLAGS survives the JMP14-back to target+11.
//   - Verified per-target: no captured prologue does anything with rax after
//     the cmp.
//
// Worst-case gateway expansion = 13/7 ≈ 1.86×. With prologueSize=11 the
// expanded body is at most 11 + (1 rip-rel × +6) = 17 bytes, plus the 14-byte
// JMP14-back = 31 bytes per Tier 0 hook.
//
// Returns the byte count written to `gateway` (excluding the trailing JMP14),
// or 0 on failure.

constexpr std::size_t kMaxRelocatedPrologueBytes = 32;  // 11B captured × 13/7 + headroom

std::size_t EmitRelocatedPrologue(std::uint8_t* gateway,
                                  std::uintptr_t targetAddr,
                                  const std::uint8_t* original,
                                  std::size_t prologueSize)
{
    std::size_t s = 0;  // read cursor in original
    std::size_t d = 0;  // write cursor in gateway

    while (s < prologueSize) {
        const std::uint8_t b0 = original[s];

        // 48 83 /N ib = sub/add/etc r/m64, imm8 with mod=11 (reg form) — 4B verbatim.
        if (b0 == 0x48 && s + 3 < prologueSize) {
            const std::uint8_t op    = original[s + 1];
            const std::uint8_t modrm = original[s + 2];
            if (op == 0x83 && (modrm & 0xC0) == 0xC0) {
                std::memcpy(gateway + d, original + s, 4);
                s += 4;
                d += 4;
                continue;
            }
            // 48 8B <modrm with mod=00 rm=101> = mov reg, [rip+disp32]
            if (op == 0x8B && (modrm & 0xC7) == 0x05 && s + 7 <= prologueSize) {
                std::int32_t disp32;
                std::memcpy(&disp32, original + s + 3, 4);
                const std::uintptr_t absAddr =
                    targetAddr + s + 7 + static_cast<std::intptr_t>(disp32);
                const std::uint8_t reg = (modrm >> 3) & 0x07;  // dest reg, low 3 bits
                if (d + 13 > kMaxRelocatedPrologueBytes) return 0;
                // mov reg, imm64
                gateway[d + 0] = 0x48;
                gateway[d + 1] = static_cast<std::uint8_t>(0xB8 + reg);
                std::memcpy(gateway + d + 2, &absAddr, 8);
                // mov reg, [reg] — modrm = (mod=00) | (reg<<3) | rm=reg
                gateway[d + 10] = 0x48;
                gateway[d + 11] = 0x8B;
                gateway[d + 12] = static_cast<std::uint8_t>((reg << 3) | reg);
                s += 7;
                d += 13;
                continue;
            }
        }
        // 80 /N <modrm with mod=00 rm=101> imm8 = cmp/etc byte [rip+disp32], imm8
        if (b0 == 0x80 && s + 1 < prologueSize) {
            const std::uint8_t modrm = original[s + 1];
            if ((modrm & 0xC7) == 0x05 && s + 7 <= prologueSize) {
                std::int32_t disp32;
                std::memcpy(&disp32, original + s + 2, 4);
                const std::uint8_t imm8 = original[s + 6];
                const std::uintptr_t absAddr =
                    targetAddr + s + 7 + static_cast<std::intptr_t>(disp32);
                const std::uint8_t opExt = (modrm >> 3) & 0x07;  // /N field
                if (d + 13 > kMaxRelocatedPrologueBytes) return 0;
                // mov rax, imm64
                gateway[d + 0] = 0x48;
                gateway[d + 1] = 0xB8;
                std::memcpy(gateway + d + 2, &absAddr, 8);
                // <op> byte [rax], imm8 — modrm = (mod=00) | (opExt<<3) | rm=000 (rax)
                gateway[d + 10] = 0x80;
                gateway[d + 11] = static_cast<std::uint8_t>(opExt << 3);
                gateway[d + 12] = imm8;
                s += 7;
                d += 13;
                continue;
            }
        }

        REX::WARN("EmitRelocatedPrologue: reject @ off {} of {} (b0={:02x} next1={:02x} next2={:02x})",
                  s, prologueSize, b0,
                  s + 1 < prologueSize ? original[s + 1] : 0,
                  s + 2 < prologueSize ? original[s + 2] : 0);
        return 0;
    }
    return d;
}

// E9-5 gateway variant that REWRITES RIP-relative instructions in the captured
// prologue to use absolute addressing. Returns nullptr (without patching) if
// the rewriter can't decode the captured bytes.
template <class T>
T CreateBranchGateway5Relocated(REL::Relocation<std::uintptr_t>& target,
                                std::size_t prologueSize, void* hook)
{
    const auto targetAddress = target.address();
    auto& trampoline = REL::GetTrampoline();

    // Worst-case allocation: 13/7 expansion + 14-byte JMP14.
    auto* gateway = static_cast<std::byte*>(
        trampoline.allocate(kMaxRelocatedPrologueBytes + sizeof(REL::ASM::JMP14)));
    auto* gw = reinterpret_cast<std::uint8_t*>(gateway);

    const auto* original = reinterpret_cast<const std::uint8_t*>(targetAddress);
    const std::size_t written = EmitRelocatedPrologue(gw, targetAddress, original, prologueSize);
    if (written == 0) {
        return nullptr;
    }

    // Diagnostic: show original vs rewritten bytes.
    {
        char origStr[64] = {}, gwStr[96] = {};
        char* p = origStr;
        for (std::size_t k = 0; k < prologueSize && k < 16; ++k) {
            if (k) *p++ = ' ';
            std::snprintf(p, 3, "%02x", original[k]); p += 2;
        }
        p = gwStr;
        for (std::size_t k = 0; k < written && k < 24; ++k) {
            if (k) *p++ = ' ';
            std::snprintf(p, 3, "%02x", gw[k]); p += 2;
        }
        REX::INFO("EmitRelocatedPrologue: tgt={:#x} gw={:#x} orig({}B)={} | rewritten({}B)={}",
                  targetAddress, reinterpret_cast<std::uintptr_t>(gw),
                  prologueSize, origStr, written, gwStr);
    }

    // Append JMP14 back to (targetAddress + prologueSize). The original's
    // prologueSize bytes are the ones we've semantically replicated; execution
    // resumes at target + prologueSize in the original.
    const REL::ASM::JMP14 jumpBack{ targetAddress + prologueSize };
    std::memcpy(gw + written, &jumpBack, sizeof(jumpBack));

    trampoline.write_jmp5(targetAddress, reinterpret_cast<std::uintptr_t>(hook));
    return reinterpret_cast<T>(gateway);
}

// --- Per-phase buckets ----------------------------------------------------
//
// SubPhase indexes into s_subBuckets. The "Frame" bucket holds Render_PreUI's
// own wall time + total draws issued during the frame.

enum class SubPhase : std::uint8_t {
    None = 0,
    LightUpdate,
    MainAccum,
    MainRenderSetup,
    OpaqueWireframe,
    DeferredPrePass,
    UpdateMotionBlur,
    DeferredDecals,
    ImagespaceSAO,
    NvidiaHBAO,
    DeferredLightsImpl,
    DeferredComposite,
    Forward,
    Refraction,
    Count,
};

const char* SubPhaseName(SubPhase p) noexcept
{
    switch (p) {
    case SubPhase::LightUpdate:        return "LightUpdate";
    case SubPhase::MainAccum:          return "MainAccum";
    case SubPhase::MainRenderSetup:    return "MainRenderSetup";
    case SubPhase::OpaqueWireframe:    return "OpaqueWireframe";
    case SubPhase::DeferredPrePass:    return "DeferredPrePass";
    case SubPhase::UpdateMotionBlur:   return "UpdateMotionBlur";
    case SubPhase::DeferredDecals:     return "DeferredDecals";
    case SubPhase::ImagespaceSAO:      return "ImagespaceSAO";
    case SubPhase::NvidiaHBAO:         return "NvidiaHBAO";
    case SubPhase::DeferredLightsImpl: return "DeferredLightsImpl";
    case SubPhase::DeferredComposite:  return "DeferredComposite";
    case SubPhase::Forward:            return "Forward";
    case SubPhase::Refraction:         return "Refraction";
    default:                           return "?";
    }
}

struct Bucket {
    std::atomic<std::uint64_t> calls{ 0 };
    std::atomic<std::uint64_t> draws{ 0 };
    std::atomic<std::uint64_t> d3dDraws{ 0 };
    std::atomic<std::uint64_t> cmdBufDraws{ 0 };
    std::atomic<std::uint64_t> totalNs{ 0 };
    std::atomic<std::uint64_t> maxNs{ 0 };

    void Reset() noexcept
    {
        calls.store(0, std::memory_order_relaxed);
        draws.store(0, std::memory_order_relaxed);
        d3dDraws.store(0, std::memory_order_relaxed);
        cmdBufDraws.store(0, std::memory_order_relaxed);
        totalNs.store(0, std::memory_order_relaxed);
        maxNs.store(0, std::memory_order_relaxed);
    }
};

Bucket s_frame;
Bucket s_subBuckets[static_cast<std::size_t>(SubPhase::Count)];

// thread_local — render thread runs Render_PreUI and all its children
// sequentially. If a worker path ever entered, the bool defaults to false
// so child hooks fall through to passthrough.
thread_local bool     tl_inFrame   = false;
thread_local SubPhase tl_subphase  = SubPhase::None;

inline void UpdateMax(std::atomic<std::uint64_t>& slot, std::uint64_t v) noexcept
{
    std::uint64_t prev = slot.load(std::memory_order_relaxed);
    while (v > prev && !slot.compare_exchange_weak(prev, v, std::memory_order_relaxed)) {
    }
}

inline void RecordBucket(Bucket& b, std::uint64_t ns) noexcept
{
    b.calls.fetch_add(1, std::memory_order_relaxed);
    b.totalNs.fetch_add(ns, std::memory_order_relaxed);
    UpdateMax(b.maxNs, ns);
}

// --- Periodic logging ------------------------------------------------------

constexpr double kLogIntervalSecs = 2.0;
std::chrono::steady_clock::time_point s_lastLogTime;

std::atomic<bool> s_firstFrameLogged{ false };
std::atomic<bool> s_firstSubLogged[static_cast<std::size_t>(SubPhase::Count)] = {};

void MaybeLog()
{
    const auto now = std::chrono::steady_clock::now();
    const double secs = std::chrono::duration<double>(now - s_lastLogTime).count();
    if (secs < kLogIntervalSecs) return;
    s_lastLogTime = now;

    const auto frameCalls = s_frame.calls.load(std::memory_order_relaxed);
    if (frameCalls == 0) return;

    const auto frameTotalNs = s_frame.totalNs.load(std::memory_order_relaxed);
    const auto frameDraws   = s_frame.draws.load(std::memory_order_relaxed);
    const auto frameD3DDraws = s_frame.d3dDraws.load(std::memory_order_relaxed);
    const auto frameCmdBufDraws = s_frame.cmdBufDraws.load(std::memory_order_relaxed);
    const auto frameMaxNs   = s_frame.maxNs.load(std::memory_order_relaxed);

    std::uint64_t childNsSum = 0;
    for (int i = 1; i < static_cast<int>(SubPhase::Count); ++i) {
        childNsSum += s_subBuckets[i].totalNs.load(std::memory_order_relaxed);
    }

    REX::INFO(
        "PhaseTelemetry[Frame]: calls={} over {:.2f}s ({:.1f}/s) "
        "totalMs/s={:.2f} avgMs={:.2f} maxMs={:.2f} bsDraws/frame={:.0f} cmdBuf/frame={:.0f} d3dDraws/frame={:.0f} "
        "unaccountedMs={:.2f}",
        frameCalls, secs, frameCalls / secs,
        (frameTotalNs / 1'000'000.0) / secs,
        (frameTotalNs / 1'000'000.0) / static_cast<double>(frameCalls),
        frameMaxNs / 1'000'000.0,
        static_cast<double>(frameDraws) / static_cast<double>(frameCalls),
        static_cast<double>(frameCmdBufDraws) / static_cast<double>(frameCalls),
        static_cast<double>(frameD3DDraws) / static_cast<double>(frameCalls),
        (frameTotalNs > childNsSum ? (frameTotalNs - childNsSum) : 0) / 1'000'000.0);

    for (int i = 1; i < static_cast<int>(SubPhase::Count); ++i) {
        const auto& b = s_subBuckets[i];
        const auto c = b.calls.load(std::memory_order_relaxed);
        if (c == 0) continue;
        const auto d  = b.draws.load(std::memory_order_relaxed);
        const auto dd = b.d3dDraws.load(std::memory_order_relaxed);
        const auto cd = b.cmdBufDraws.load(std::memory_order_relaxed);
        const auto ns = b.totalNs.load(std::memory_order_relaxed);
        const auto mx = b.maxNs.load(std::memory_order_relaxed);
        REX::INFO(
            "  Frame[{}]: calls={} bsDraws={} cmdBuf={} d3dDraws={} totMs/s={:.2f} avgUs={:.1f} maxUs={:.1f} "
            "(%frame={:.1f})",
            SubPhaseName(static_cast<SubPhase>(i)),
            c, d, cd, dd,
            (ns / 1'000'000.0) / secs,
            (ns / 1000.0) / static_cast<double>(c),
            mx / 1000.0,
            100.0 * static_cast<double>(ns) / static_cast<double>(frameTotalNs));
    }

    s_frame.Reset();
    for (auto& b : s_subBuckets) b.Reset();
}

// --- Hook bodies ----------------------------------------------------------

void HookedRenderPreUI()
{
    if (g_mode.load(std::memory_order_relaxed) != Mode::On) {
        s_origRenderPreUI();
        return;
    }
    if (!s_firstFrameLogged.exchange(true, std::memory_order_relaxed)) {
        REX::INFO("PhaseTelemetry: first DrawWorld::Render_PreUI call observed");
    }
    const bool wasInFrame = tl_inFrame;
    tl_inFrame = true;

    const auto t0 = std::chrono::steady_clock::now();
    s_origRenderPreUI();
    const auto t1 = std::chrono::steady_clock::now();
    const auto ns = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());

    tl_inFrame = wasInFrame;
    RecordBucket(s_frame, ns);

    MaybeLog();
}

// Generic per-sub-phase body. Records into s_subBuckets[P] only if Render_PreUI
// is on the stack (tl_inFrame). Otherwise passthrough — Render_PreUI must own
// the lifetime for our model to apply.
template <SubPhase P>
void HookedSubPhase(VoidVoid_t orig)
{
    if (g_mode.load(std::memory_order_relaxed) != Mode::On) {
        orig();
        return;
    }
    if (!tl_inFrame) {
        orig();
        return;
    }
    if (!s_firstSubLogged[static_cast<std::size_t>(P)].exchange(true, std::memory_order_relaxed)) {
        REX::INFO("PhaseTelemetry: first DrawWorld::{} call observed", SubPhaseName(P));
    }
    const SubPhase prev = tl_subphase;
    tl_subphase = P;

    const auto t0 = std::chrono::steady_clock::now();
    orig();
    const auto t1 = std::chrono::steady_clock::now();
    const auto ns = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());

    tl_subphase = prev;
    RecordBucket(s_subBuckets[static_cast<std::size_t>(P)], ns);
}

void HookedMainAccumCore()
{
    s_origMainAccum();
}
void HookedMainAccum()          { HookedSubPhase<SubPhase::MainAccum         >(&HookedMainAccumCore); }
void HookedMainRenderSetup()    { HookedSubPhase<SubPhase::MainRenderSetup   >(s_origMainRenderSetup); }
void HookedDeferredPrePassCore()
{
    s_origDeferredPrePass();
}
void HookedDeferredPrePass()    { HookedSubPhase<SubPhase::DeferredPrePass   >(&HookedDeferredPrePassCore); }
void HookedDeferredDecals()     { HookedSubPhase<SubPhase::DeferredDecals    >(s_origDeferredDecals); }
void HookedNvidiaHBAO()         { HookedSubPhase<SubPhase::NvidiaHBAO        >(s_origNvidiaHBAO); }
// Special-cased: wrap the original with LightSorter::OnEnter/OnExit so the
// point-light array is stable-partitioned by stencil flag before the engine
// iterates it, then restored after. LightSorter is a no-op when its mode is
// Off, so this adds ~one atomic-load worth of overhead in that case.
void HookedDeferredLightsImplCore()
{
    LightSorter::OnEnter();
    s_origDeferredLightsImpl();
    LightSorter::OnExit();
}
void HookedDeferredLightsImpl() { HookedSubPhase<SubPhase::DeferredLightsImpl>(&HookedDeferredLightsImplCore); }
void HookedDeferredComposite()  { HookedSubPhase<SubPhase::DeferredComposite >(s_origDeferredComposite); }
void HookedForwardCore()
{
    s_origForward();
}
void HookedForward()            { HookedSubPhase<SubPhase::Forward           >(&HookedForwardCore); }
// Tier 0 — installed via E9-5 + relocator.
void HookedLightUpdate()        { HookedSubPhase<SubPhase::LightUpdate       >(s_origLightUpdate); }
void HookedOpaqueWireframe()    { HookedSubPhase<SubPhase::OpaqueWireframe   >(s_origOpaqueWireframe); }
void HookedUpdateMotionBlur()   { HookedSubPhase<SubPhase::UpdateMotionBlur  >(s_origUpdateMotionBlur); }
void HookedImagespaceSAO()      { HookedSubPhase<SubPhase::ImagespaceSAO     >(s_origImagespaceSAO); }
void HookedRefraction()         { HookedSubPhase<SubPhase::Refraction        >(s_origRefraction); }

// --- Install helpers ------------------------------------------------------

enum class HookFlavor : std::uint8_t { JMP14, E9_5, E9_5_Reloc };

bool InstallOne(const char* name, REL::Relocation<std::uintptr_t>& target,
                std::size_t prologueSize, HookFlavor flavor,
                void* hook, void** outOrig)
{
    const std::uintptr_t addr = target.address();
    if (addr < 0x140000000ull) {
        REX::WARN("PhaseTelemetry::Initialize: {} address resolution failed (got {:#x}); REL::ID likely missing for this runtime. Skipping.",
                  name, addr);
        return false;
    }
    const auto* original = reinterpret_cast<const std::uint8_t*>(addr);
    std::uint8_t captured[24] = {};
    const std::size_t capLen = std::min<std::size_t>(prologueSize, sizeof(captured));
    std::memcpy(captured, original, capLen);

    void* gateway = nullptr;
    if (flavor == HookFlavor::JMP14) {
        gateway = CreateBranchGateway<void*>(target, prologueSize, hook);
    } else if (flavor == HookFlavor::E9_5) {
        gateway = CreateBranchGateway5<void*>(target, prologueSize, hook);
    } else {
        gateway = CreateBranchGateway5Relocated<void*>(target, prologueSize, hook);
    }
    if (!gateway) {
        REX::WARN("PhaseTelemetry::Initialize: gateway install failed for {} @ {:#x} (flavor {})",
                  name, addr,
                  flavor == HookFlavor::JMP14 ? "JMP14"
                  : flavor == HookFlavor::E9_5 ? "E9-5"
                  : "E9-5+reloc (relocator rejected captured bytes)");
        return false;
    }
    *outOrig = gateway;

    std::string capStr;
    capStr.reserve(capLen * 3);
    for (std::size_t i = 0; i < capLen; ++i) {
        if (i) capStr.push_back(' ');
        char buf[3];
        std::snprintf(buf, sizeof(buf), "%02x", captured[i]);
        capStr.append(buf);
    }
    REX::INFO(
        "PhaseTelemetry::Initialize: hooked {} @ {:#x} (prologue={}B {} captured = {})",
        name, addr, prologueSize,
        flavor == HookFlavor::JMP14    ? "JMP14"
        : flavor == HookFlavor::E9_5   ? "E9-5"
                                       : "E9-5+reloc",
        capStr);
    return true;
}

bool s_installed = false;

}  // anonymous namespace

void OnDraw()
{
    if (g_mode.load(std::memory_order_relaxed) != Mode::On) return;
    if (!tl_inFrame) return;
    s_frame.draws.fetch_add(1, std::memory_order_relaxed);
    if (tl_subphase != SubPhase::None) {
        s_subBuckets[static_cast<std::size_t>(tl_subphase)]
            .draws.fetch_add(1, std::memory_order_relaxed);
    }
}

void OnD3DDraw()
{
    if (g_mode.load(std::memory_order_relaxed) != Mode::On) return;
    if (!tl_inFrame) return;
    s_frame.d3dDraws.fetch_add(1, std::memory_order_relaxed);
    if (tl_subphase != SubPhase::None) {
        s_subBuckets[static_cast<std::size_t>(tl_subphase)]
            .d3dDraws.fetch_add(1, std::memory_order_relaxed);
    }
}

void OnCommandBufferDraw()
{
    if (g_mode.load(std::memory_order_relaxed) != Mode::On) return;
    if (!tl_inFrame) return;
    s_frame.cmdBufDraws.fetch_add(1, std::memory_order_relaxed);
    if (tl_subphase != SubPhase::None) {
        s_subBuckets[static_cast<std::size_t>(tl_subphase)]
            .cmdBufDraws.fetch_add(1, std::memory_order_relaxed);
    }
}

void RequireHooks()
{
    s_forceHooks.store(true, std::memory_order_relaxed);
}

bool IsInRenderPreUI()
{
    return tl_inFrame;
}

bool IsInDeferredPrePass()
{
    return tl_inFrame && tl_subphase == SubPhase::DeferredPrePass;
}

bool IsInDeferredLightsImpl()
{
    return tl_inFrame && tl_subphase == SubPhase::DeferredLightsImpl;
}

bool Initialize()
{
    if (s_installed) {
        REX::INFO("PhaseTelemetry::Initialize: already installed; skipping");
        return true;
    }
    const auto mode = g_mode.load(std::memory_order_relaxed);

    // The DrawWorld:: hooks aren't owned by PhaseTelemetry alone — LightSorter
    // runs its OnEnter/OnExit logic from inside our HookedDeferredLightsImpl
    // wrapper, and renderer hooks can request phase context without enabling
    // telemetry logging. If either piggy-back consumer is on but
    // PhaseTelemetry itself is Off, we still install the hooks. Each per-call
    // check inside HookedSubPhase / the wrappers is a single relaxed atomic
    // load when disabled.
    const bool lightSorterOn = LightSorter::g_mode.load(std::memory_order_relaxed)
                               != LightSorter::Mode::Off;
    const bool forced = s_forceHooks.load(std::memory_order_relaxed);
    const bool needHooks = (mode == Mode::On) || lightSorterOn || forced;

    REX::INFO("PhaseTelemetry::Initialize: mode={} (hooks {} — telemetry={}, "
              "LightSorter={}, forced={})",
              mode == Mode::On ? "on" : "off",
              needHooks ? "installing" : "skipped",
              mode == Mode::On ? "on" : "off",
              lightSorterOn ? "on" : "off",
              forced ? "on" : "off");
    if (!needHooks) {
        return true;
    }

    s_lastLogTime = std::chrono::steady_clock::now();

    bool ok = true;
    ok &= InstallOne("DrawWorld::Render_PreUI",
                     ptr_RenderPreUI, kPrologueRenderPreUI, HookFlavor::E9_5,
                     reinterpret_cast<void*>(&HookedRenderPreUI),
                     reinterpret_cast<void**>(&s_origRenderPreUI));
    ok &= InstallOne("DrawWorld::MainAccum",
                     ptr_MainAccum, kPrologueMainAccum, HookFlavor::E9_5,
                     reinterpret_cast<void*>(&HookedMainAccum),
                     reinterpret_cast<void**>(&s_origMainAccum));
    ok &= InstallOne("DrawWorld::MainRenderSetup",
                     ptr_MainRenderSetup, kPrologueMainRenderSetup, HookFlavor::E9_5,
                     reinterpret_cast<void*>(&HookedMainRenderSetup),
                     reinterpret_cast<void**>(&s_origMainRenderSetup));
    ok &= InstallOne("DrawWorld::DeferredPrePass",
                     ptr_DeferredPrePass, kPrologueDeferredPrePass, HookFlavor::JMP14,
                     reinterpret_cast<void*>(&HookedDeferredPrePass),
                     reinterpret_cast<void**>(&s_origDeferredPrePass));
    ok &= InstallOne("DrawWorld::DeferredDecals",
                     ptr_DeferredDecals, kPrologueDeferredDecals, HookFlavor::JMP14,
                     reinterpret_cast<void*>(&HookedDeferredDecals),
                     reinterpret_cast<void**>(&s_origDeferredDecals));
    ok &= InstallOne("DrawWorld::NvidiaHBAO",
                     ptr_NvidiaHBAO, kPrologueNvidiaHBAO, HookFlavor::JMP14,
                     reinterpret_cast<void*>(&HookedNvidiaHBAO),
                     reinterpret_cast<void**>(&s_origNvidiaHBAO));
    ok &= InstallOne("DrawWorld::DeferredLightsImpl",
                     ptr_DeferredLightsImpl, kPrologueDeferredLightsImpl, HookFlavor::JMP14,
                     reinterpret_cast<void*>(&HookedDeferredLightsImpl),
                     reinterpret_cast<void**>(&s_origDeferredLightsImpl));
    ok &= InstallOne("DrawWorld::DeferredComposite",
                     ptr_DeferredComposite, kPrologueDeferredComposite, HookFlavor::JMP14,
                     reinterpret_cast<void*>(&HookedDeferredComposite),
                     reinterpret_cast<void**>(&s_origDeferredComposite));
    ok &= InstallOne("DrawWorld::Forward",
                     ptr_Forward, kPrologueForward, HookFlavor::E9_5,
                     reinterpret_cast<void*>(&HookedForward),
                     reinterpret_cast<void**>(&s_origForward));

    // Tier 0 — the five previously-skipped phases. All share a
    // `sub rsp,0x38; <rip-rel mov/cmp at byte 4>` prologue; we capture 11 B
    // and the relocator adjusts the disp32 of the rip-rel instruction.
    ok &= InstallOne("DrawWorld::LightUpdate",
                     ptr_LightUpdate, kPrologueTier0_SubAndRipRel, HookFlavor::E9_5_Reloc,
                     reinterpret_cast<void*>(&HookedLightUpdate),
                     reinterpret_cast<void**>(&s_origLightUpdate));
    ok &= InstallOne("DrawWorld::OpaqueWireframe",
                     ptr_OpaqueWireframe, kPrologueTier0_SubAndRipRel, HookFlavor::E9_5_Reloc,
                     reinterpret_cast<void*>(&HookedOpaqueWireframe),
                     reinterpret_cast<void**>(&s_origOpaqueWireframe));
    ok &= InstallOne("DrawWorld::UpdateMotionBlur",
                     ptr_UpdateMotionBlur, kPrologueTier0_SubAndRipRel, HookFlavor::E9_5_Reloc,
                     reinterpret_cast<void*>(&HookedUpdateMotionBlur),
                     reinterpret_cast<void**>(&s_origUpdateMotionBlur));
    ok &= InstallOne("DrawWorld::ImagespaceSAO",
                     ptr_ImagespaceSAO, kPrologueTier0_SubAndRipRel, HookFlavor::E9_5_Reloc,
                     reinterpret_cast<void*>(&HookedImagespaceSAO),
                     reinterpret_cast<void**>(&s_origImagespaceSAO));
    ok &= InstallOne("DrawWorld::Refraction",
                     ptr_Refraction, kPrologueTier0_SubAndRipRel, HookFlavor::E9_5_Reloc,
                     reinterpret_cast<void*>(&HookedRefraction),
                     reinterpret_cast<void**>(&s_origRefraction));

    if (!ok) {
        REX::WARN("PhaseTelemetry::Initialize: at least one hook failed; module remains partially installed (failed hooks: telemetry will skip those phases). All children gate on tl_inFrame, so if Render_PreUI itself failed, the module is effectively off.");
    }

    s_installed = true;
    REX::INFO("PhaseTelemetry::Initialize: installation complete; logging every {:.1f}s", kLogIntervalSecs);
    return ok;
}

}  // namespace PhaseTelemetry

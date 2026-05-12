#include <PCH.h>
#include "InstancingProbe.h"

#include <chrono>
#include <cstring>

namespace InstancingProbe {

std::atomic<Mode> g_mode{ Mode::Off };

namespace {

// Mirror of Plugin.cpp's BSRenderPassLayout offsets — validated there by
// static_assert. We don't include the full type (it lives in an anonymous
// namespace in Plugin.cpp) because the probe is meant to be self-contained;
// we just byte-grab the three pointers we need.
constexpr std::size_t kPassShaderOffset      = 0x08;
constexpr std::size_t kPassMaterialOffset    = 0x10;
constexpr std::size_t kPassGeometryOffset    = 0x18;

// Logging cadence — matches ShadowCullCache so the two read consistently.
// Aligned to power-of-two so we can check the cheap "calls & mask" gate.
constexpr std::uint64_t kLogIntervalCalls = 4096;
constexpr double        kLogIntervalSecs  = 2.0;

// "Last seen" tracking for consecutive-pair counting. BSBatchRenderer::Draw
// runs sequentially on the main render thread (chained passes are flattened
// into a single loop before any of this fires), so a single per-thread cache
// is sufficient. thread_local keeps it safe in the unlikely case a worker
// path emits draws.
thread_local void* tl_lastShader   = nullptr;
thread_local void* tl_lastMaterial = nullptr;
thread_local void* tl_lastGeometry = nullptr;
// Length of the in-progress instance-candidate run (consecutive draws with
// same shader+material+geometry).
thread_local std::uint32_t tl_curRunLen = 0;

// Aggregated counters (per logging window — exchanged to zero each log).
std::atomic<std::uint64_t> a_draws{ 0 };
std::atomic<std::uint64_t> a_sameShader{ 0 };
std::atomic<std::uint64_t> a_sameShaderMat{ 0 };
std::atomic<std::uint64_t> a_sameShaderMatGeom{ 0 };
// Sum of all completed instance-candidate run lengths in the window. Divide
// by window-count-of-runs (= a_sameShaderMatGeom transitions) for the avg
// run length. We sample a simpler "max" alongside to highlight outliers.
std::atomic<std::uint64_t> a_runLenSum{ 0 };
std::atomic<std::uint64_t> a_runLenMax{ 0 };
// Number of distinct runs (>= 2 same-mesh draws in a row) — useful for
// estimating "how many DrawInstanced calls would we emit if we merged".
std::atomic<std::uint64_t> a_runCount{ 0 };

std::atomic<bool> s_firstCallLogged{ false };
std::chrono::steady_clock::time_point s_lastLogTime;

void MaybeLog()
{
    const auto draws = a_draws.load(std::memory_order_relaxed);
    if (draws == 0 || (draws & (kLogIntervalCalls - 1)) != 0) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    const double secs = std::chrono::duration<double>(now - s_lastLogTime).count();
    if (secs < kLogIntervalSecs) {
        return;
    }
    s_lastLogTime = now;

    const auto winDraws  = a_draws.exchange(0, std::memory_order_relaxed);
    const auto sameS     = a_sameShader.exchange(0, std::memory_order_relaxed);
    const auto sameSM    = a_sameShaderMat.exchange(0, std::memory_order_relaxed);
    const auto sameSMG   = a_sameShaderMatGeom.exchange(0, std::memory_order_relaxed);
    const auto runLenSum = a_runLenSum.exchange(0, std::memory_order_relaxed);
    const auto runLenMax = a_runLenMax.exchange(0, std::memory_order_relaxed);
    const auto runCount  = a_runCount.exchange(0, std::memory_order_relaxed);

    if (winDraws == 0) {
        return;
    }
    const double drawsD = static_cast<double>(winDraws);
    const double avgRunLen = runCount > 0
                                 ? static_cast<double>(runLenSum) / static_cast<double>(runCount)
                                 : 0.0;
    REX::INFO(
        "InstancingProbe: draws={} over {:.2f}s ({:.0f}/s) "
        "sameShader={:.1f}% sameShader+Mat={:.1f}% sameShader+Mat+Geom={:.1f}% "
        "(instance-candidate) runs={} avgRunLen={:.1f} maxRunLen={}",
        winDraws, secs, drawsD / secs,
        100.0 * sameS  / drawsD,
        100.0 * sameSM / drawsD,
        100.0 * sameSMG / drawsD,
        runCount, avgRunLen, runLenMax);
}

inline void* ReadField(const void* base, std::size_t offset)
{
    if (!base) return nullptr;
    void* p = nullptr;
    std::memcpy(&p, static_cast<const std::uint8_t*>(base) + offset, sizeof(p));
    return p;
}

}  // anonymous namespace

void OnDraw(const void* pass)
{
    if (g_mode.load(std::memory_order_relaxed) != Mode::On) {
        return;
    }
    if (!s_firstCallLogged.exchange(true, std::memory_order_relaxed)) {
        REX::INFO("InstancingProbe: first BSBatchRenderer::Draw observed");
    }

    void* shader   = ReadField(pass, kPassShaderOffset);
    void* material = ReadField(pass, kPassMaterialOffset);
    void* geom     = ReadField(pass, kPassGeometryOffset);

    const bool ss  = (shader   == tl_lastShader   && shader != nullptr);
    const bool ssm = (ss && material == tl_lastMaterial);
    const bool ssg = (ssm && geom == tl_lastGeometry && geom != nullptr);

    a_draws.fetch_add(1, std::memory_order_relaxed);
    if (ss)  a_sameShader.fetch_add(1, std::memory_order_relaxed);
    if (ssm) a_sameShaderMat.fetch_add(1, std::memory_order_relaxed);
    if (ssg) a_sameShaderMatGeom.fetch_add(1, std::memory_order_relaxed);

    // Run tracking — a "run" is >= 2 consecutive draws with same shader+mat+geom.
    if (ssg) {
        tl_curRunLen++;
    } else {
        // Run broke — commit if it was a real run, then reset.
        if (tl_curRunLen >= 2) {
            a_runLenSum.fetch_add(tl_curRunLen, std::memory_order_relaxed);
            a_runCount.fetch_add(1, std::memory_order_relaxed);
            std::uint64_t prev = a_runLenMax.load(std::memory_order_relaxed);
            while (tl_curRunLen > prev &&
                   !a_runLenMax.compare_exchange_weak(prev, tl_curRunLen,
                                                     std::memory_order_relaxed)) {
            }
        }
        tl_curRunLen = 1;  // this draw starts a new potential run
    }

    tl_lastShader   = shader;
    tl_lastMaterial = material;
    tl_lastGeometry = geom;

    MaybeLog();
}

void Initialize()
{
    s_lastLogTime = std::chrono::steady_clock::now();
    const auto m = g_mode.load(std::memory_order_relaxed);
    REX::INFO("InstancingProbe::Initialize: mode={}", m == Mode::On ? "on" : "off");
}

}  // namespace InstancingProbe

#pragma once

#include <atomic>

namespace REX::W32 {
    struct ID3D11DeviceContext;
}

// Per-draw inspector for Bethesda's deferred-light family (and related 2-MRT
// passes). Off by default; armed by Numpad * when DEVELOPMENT=true in
// ShaderEngine.ini. One press captures the next rendered frame to
// `<plugin>/LightTracker/lights_<timestamp>.csv` with shader UID, blend
// state, render target, scissor, PS SRV slots, and the first 9 vec4s of
// cb2 (light pos/range/dir for local lights; SH bank for ambient).
namespace LightTracker {

// Fast-path gate consulted by every Draw* hook. Stays false unless
// DEVELOPMENT is on; in dev mode it stays true so OnDrawImpl can keep a
// running "draws observed" counter for diagnosing missing hook fires. The
// hot-path overhead is one relaxed atomic load + a predictable branch.
extern std::atomic<bool> g_isActive;

void Initialize();
void Shutdown();

// Called once per frame at the start of MyPresent. Polls the hotkey,
// finalizes the previous capture frame, opens the next capture file.
void Tick();

// Slow path; only invoked from the inline OnDraw() wrapper when capture is
// active. Snapshots pipeline state at draw-call entry.
// Capture entry. Called from MyPSSetShader after the engine has set up the
// rest of the pipeline state (RTVs, blend, scissor, cb2 contents) but
// before the actual draw fires. Use this instead of a Draw* hook because
// ENB / ReShade / other graphics wrappers intercept the D3D11 Draw vtable
// before us — same problem the draw-tag SRV solved by recording into the
// engine's own command buffer (see SRV-details.md §7).
void OnPSBindImpl(REX::W32::ID3D11DeviceContext* ctx,
                  REX::W32::ID3D11PixelShader*   originalPS);

inline void OnPSBind(REX::W32::ID3D11DeviceContext* ctx,
                     REX::W32::ID3D11PixelShader*   originalPS)
{
    if (g_isActive.load(std::memory_order_relaxed)) {
        OnPSBindImpl(ctx, originalPS);
    }
}

}  // namespace LightTracker

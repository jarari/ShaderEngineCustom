# Fallout 4 Rendering Pipeline (IDA-Derived)

This is the working model of how Fallout 4 issues draws and where this plugin can hook in to attach per-draw data such as the material tag SRV. All addresses are from the user's checked IDB (`Fallout4.exe.unpacked.exe`, OG runtime).

## High-level shape

Frames are built up as **render passes** (`BSRenderPass`) that are queued onto **batches** owned by `BSBatchRenderer`. At dispatch time, batches are emitted either through:

1. **Immediate path** — pass-by-pass, calling each pass's shader and `BSBatchRenderer::Draw`, which translates geometry into D3D11 calls. This is the path most relevant to per-pass hooking because we see one `BSRenderPass*` per draw.
2. **Command-buffer path** — `BSGraphics::Renderer::ProcessCommandBuffer` replays a prerecorded stream of D3D11 calls (texture binds, IA state, draws). Individual `BSRenderPass*` are not visible during replay; they were captured earlier.

Both paths ultimately go through `ID3D11DeviceContext::Draw*`, so vtable hooks on the context are the lowest-common-denominator point to inject per-draw GPU state.

## `BSRenderPass` layout

Confirmed from `BSRenderPass::Set`, `BSRenderPass::BSRenderPass`, and `BSBatchRenderer::Draw` reads:

```cpp
struct BSRenderPass
{
    BSRenderPass*       next;            // 0x00
    BSShader*           shader;          // 0x08
    BSShaderProperty*   shaderProperty;  // 0x10
    BSGeometry*         geometry;        // 0x18
    // 0x20 – BSInstanceGroupPass*  (read by Draw before geometry switch)
    void*               commandBuffer;   // 0x28 – NVFlex::DebrisInstanceGroupPass* in some cases
    // 0x30 padding
    BSRenderPass*       listNext;        // 0x38 (GetNext)
    BSRenderPass*       passGroupNext;   // 0x40 (GetPassGroupNext)
    uint32_t            techniqueID;     // 0x48
    // total alloc: 0x58
};
```

This is the layout used by the `BSRenderPassLayout` declaration in `Plugin.cpp`. Only the first 0x50 bytes are relevant for hooking; static asserts pin offsets 0x10, 0x18, 0x28, 0x38, 0x40, 0x48.

## Key engine functions

| Symbol                                                    | Address (OG)   |
| --------------------------------------------------------- | -------------- |
| `BSBatchRenderer::Draw(BSRenderPass*, ...)`               | `0x14287EDE0`  |
| `BSBatchRenderer::RenderBatches`                          | `0x14287F380`  |
| `BSBatchRenderer::RenderBatchesInOrder`                   | `0x14287F4E0`  |
| `BSBatchRenderer::RenderPassImmediately`                  | `0x14287F770`  |
| `BSBatchRenderer::RenderPassImmediatelySameTechnique`     | `0x14287F8D0`  |
| `BSBatchRenderer::RenderPassImmediately_Standard`         | `0x14287FC70`  |
| `BSBatchRenderer::RenderPassImmediately_Skinned`          | `0x14287FD20`  |
| `BSBatchRenderer::RenderPassImmediately_Custom`           | `0x14287FDC0`  |
| `BSBatchRenderer::RenderCommandBufferPassesImpl`          | `0x1428800D0`  |
| `BSGraphics::Renderer::ProcessCommandBuffer`              | `0x141D13A10`  |
| `BSGraphics::Renderer::DrawTriShape (no IB)`              | `0x141D0BFB0`  |
| `BSGraphics::Renderer::DrawTriShape (IB)`                 | `0x141D0C160`  |
| `BSGraphics::Renderer::DrawTriShapeLargeIndices`          | `0x141D0C2A0`  |
| `BSLightingShader::SetupGeometry`                         | `0x14289DD10`  |
| `BipedAnim::AttachToParent`                               | `0x1401C26E0`  |
| `BipedAnim::RemovePart(BIPOBJECT*, bool)`                 | `0x1401BDF50`  |

REL IDs used by the plugin live in `Plugin.cpp` next to each `Relocation<>`.

## Calling convention of `BSBatchRenderer::Draw`

IDA decompiles to:

```cpp
void __fastcall BSBatchRenderer::Draw(
    BSRenderPass* a1,
    __int64 a2,
    __int64 a3,
    BSGraphics::DynamicTriShapeDrawData* a4);
```

Despite some symbol declarations showing only one parameter, the four-arg signature is what the assembled body uses — `a4` is read inside `DrawSegmentedShape` for the dynamic tri-shape path. The plugin's hook signature must match.

## Draw dispatch — the case table inside `BSBatchRenderer::Draw`

`BSBatchRenderer::Draw` switches on `geometry + 0x158` (a single byte, `BSGeometry::Type`) to pick the GPU draw shape:

| case  | Geometry kind                      | Calls                                                 |
| ----- | ---------------------------------- | ----------------------------------------------------- |
| 0     | (default)                          | nothing — early return                                |
| 1     | NiParticles                        | `MapDynamicTriShapeDynamicData`, `DrawDynamicTriShape`|
| 3     | Standard tri-shape                 | `DrawTriShape`                                        |
| 4     | `BSDynamicTriShape`                | `DrawSegmentedShape`                                  |
| 5     | `BSMergeInstancedTriShape` / LOD   | `DrawTriShape`                                        |
| 6     | `BSLODMultiIndexTriShape`          | `DrawTriShape` (with index buffer)                    |
| 7     | `BSTriShape` w/ large indices      | `DrawTriShape`                                        |
| 8     | Segmented tri-shape                | `DrawSegmentedShape`                                  |
| 0xA   | Instanced multi-geom               | `DrawInstancedTriShape` per child                     |
| 0xB   | Particle shader tri-shape          | `DrawParticleShaderTriShape`                          |
| 0xC   | Line shape                         | `DrawLineShape`                                       |
| 0xD   | Dynamic line shape                 | `DrawDynamicLineShape`                                |
| 0xF   | Combined / multi-index             | sets VS/PS structured buffers, `DrawTriShapeLargeIndices` |

Two early-out checks happen before the switch:
- `pass + 0x28` (a `NVFlex::DebrisInstanceGroupPass*`) — if non-null, `NVFlex::DebrisInstanceGroupPass::Render` is called and `Draw` returns.
- `pass + 0x20` (a `BSInstanceGroupPass*`) — if non-null, `BSInstanceGroupPass::Render` is called and `Draw` returns.

These early returns matter for hooking: the per-pass tag is still meaningful because we hook `Draw` at entry, but no D3D11 draw call will fire on the immediate context for the NVFlex path (it's its own renderer).

## Per-pass call order

`RenderPassImmediately_Standard` and `RenderPassImmediately_Skinned` follow the same pattern:

```text
shader->SetupGeometry(pass)   // vtable[7], offset +0x38
BSBatchRenderer::Draw(pass)
shader->RestoreGeometry(pass) // vtable[8], offset +0x40
```

`Skinned` additionally calls `BSShader::InvalidateSkinInstance` and `BSShader::FlushBoneMatrices` around `Draw`. `Custom` is shorter and does not call `Draw` — it dispatches to the shader's own draw path.

This sequence is what lets the plugin push a `g_currentDrawTag` in the `SetupGeometry` hook, push it again in the `Draw` hook (idempotent because we use a stack), and pop it in `RestoreGeometry`.

## `BSLightingShader::SetupGeometry`

Behavior, from IDA:

1. Fetches the per-thread context block via the game's TLS slot:
   ```cpp
   v8 = NtCurrentTeb()->ThreadLocalStoragePointer[tls_index];
   // v8 + 2840 = ID3D11DeviceContext*
   // v8 + 2848 = BSGraphics::Context*
   ```
2. Resolves the geometry-level constant buffer for the current VS and PS via `BSGraphics::Context::GetConstantBuffer(...)`.
3. Maps each constant buffer (`vfunc + 0x70`), writes geometry-derived constants (world matrix, view-projection, directional light, point lights, projected UV, eye position, etc.), and unmaps (`vfunc + 0x78`).
4. Through `VSSetConstantBuffers` (`vtable[7]`) and `PSSetConstantBuffers` (`vtable[16]`), commits the updated constant buffers to **constant slot 2**.

Important takeaway: `SetupGeometry` does **not** touch `PSSetShaderResources`. It does not bind any SRV. So our t26 SRV is never displaced by it, and there is no contention with our injected slots.

## `BSGraphics::Renderer::DrawTriShape`

Reads its context from the same `TLS+0xB18` slot, then issues:

- `IASetIndexBuffer` (`vtable[19]`, offset 152)
- `IASetVertexBuffers` (`vtable[18]`, offset 144)
- `DrawIndexed` (`vtable[12]`, offset 96) — or `Draw` (`vtable[13]`, offset 104) on the no-index branch

This is the actual D3D11 draw call. Our `MyDrawIndexed` / `MyDraw` vtable hooks fire here, on the **same context** the engine pulled from TLS, so binding the per-draw SRV with the `This` argument we receive is guaranteed to bind on the correct context.

## Command-buffer path

`BSBatchRenderer::RenderBatches` decides per pass-group whether to take the immediate path or the recorded path:

```cpp
auto* cb = GetCommandBufferPasses(...);
if (cb && cb->frameToken == currentFrame) {
    RenderCommandBufferPassesImpl(this, group, cb, idx, allowAlpha);
} else {
    RenderPassImpl(this, head, technique, allowAlpha);  // immediate
}
```

`RenderCommandBufferPassesImpl` calls `BSGraphics::Renderer::ProcessCommandBuffer`, which iterates a packed buffer and replays it onto the immediate context. Per recorded entry it issues:

- A pair of constant-buffer maps + writes for VS / PS (`Map`, `memcpy`, `Unmap`, `VSSetConstantBuffers`).
- A loop of `PSSetShaderResources(slot, 1, srv)` (`vtable[8]`) — one call per recorded texture slot.
- A loop of `VSSetShaderResources(slot, 1, srv)` (`vtable[25]`) for the geometry/skin buffers.
- `OMSetBlendState`, `RSSetState`, `OMSetDepthStencilState` for state changes.
- `IASetIndexBuffer`, `IASetVertexBuffers`, `IASetPrimitiveTopology`.
- `DrawIndexed` (`vtable[12]`) or `Draw` (`vtable[13]`).

After the buffer is replayed it then iterates `head->passGroupNext` and calls `RenderPassImmediatelySameTechnique` for any same-technique passes that came after the buffered ones — those go through the normal immediate path.

**Implications for per-pass hooks:**
- For draws that came from the recorded buffer, neither `BSBatchRenderer::Draw` nor the shader `SetupGeometry` hook fires — only the D3D11 vtable hooks do.
- The recorded `PSSetShaderResources` calls are interleaved with the draws. They use whatever slots the shader required at record time. Slots above the shader's used range (e.g. our t26..t31 injected slots) are not touched, so our SRV stays bound across replay.
- `g_currentDrawTag` is whatever the last immediate path left in TLS, so command-buffered draws read a stale tag. In practice the command-buffer path is used for static-shader-state batches (LOD, terrain, distant grass, etc.), not actor equipment, so the actor-tag classification is unaffected. If a future tag class needs to cover those geometries, the recording path itself would need additional hooking.

## Threading and contexts

- F4 has multiple threads but D3D11 immediate-context calls are funneled to a single render thread.
- `BSGraphics::Renderer` always reads its context from `TLS+0xB18`. In every path we traced (`DrawTriShape`, `ProcessCommandBuffer`, `SetupGeometry`) this resolves to `g_rendererData->context` — the immediate context.
- Because of that, the `This` pointer that `MyDrawIndexed` and friends receive is always the same immediate context, so calling `Map(WRITE_DISCARD)` + `Unmap` + `PSSetShaderResources` on `This` directly is the right way to attach per-draw data. There is no need to fetch the context from anywhere else.

## How the plugin hooks this

Mapping from the engine pipeline above to hooks installed by `InstallDrawTaggingHooks_Internal`:

| Engine point                                              | Hook                                                  | Purpose                                          |
| --------------------------------------------------------- | ----------------------------------------------------- | ------------------------------------------------ |
| `BipedAnim::AttachToParent`                               | `HookedBipedAnimAttachToParent`                       | Refresh actor-tagged geometry set on attach      |
| `BipedAnim::AttachSkinnedObject`                          | `HookedBipedAnimAttachSkinnedObject`                  | Same as above for skinned attach                 |
| `BipedAnim::AttachBipedWeapon`                            | `HookedBipedAnimAttachBipedWeapon`                    | Same as above for weapon attach                  |
| `BipedAnim::RemovePart`                                   | `HookedBipedAnimRemovePart`                           | Remove from set before original tears it down    |
| `Actor::Load3D`, `PlayerCharacter::Load3D`                | `HookedActorLoad3D`, `HookedPlayerCharacterLoad3D`    | Initial population of the set                    |
| `BSLightingShader::SetupGeometry` / `RestoreGeometry`     | vtable hooks at indices 7 / 8                         | Push / pop the per-draw tag (stack-balanced)     |
| `BSEffectShader::SetupGeometry` / `RestoreGeometry`       | vtable hooks at indices 7 / 8                         | Same as above for effect shaders                 |
| `BSBatchRenderer::Draw`                                   | branch trampoline at function entry                   | Fallback push / pop for shader types we don't hook |
| `ID3D11DeviceContext::Draw{,Indexed,Instanced,IndexedInstanced}` | vtable hooks at indices 12, 13, 20, 21          | `Map(WRITE_DISCARD)` + write tag + bind SRV      |

The four `MyDraw*` hooks are the only place that actually writes the GPU buffer; the engine-level hooks only maintain the thread-local `g_currentDrawTag` value those hooks read.

## Why `D3D11_USAGE_DYNAMIC` + `MAP_WRITE_DISCARD`

The original implementation used two `D3D11_USAGE_IMMUTABLE` buffers (one baked with tag=0, one with tag=1) and switched the bound SRV based on the tag. That design had two issues:

1. **Race-on-creation.** Both buffers' baked contents come from the same global `g_drawTagData` at `CreateBuffer` time. A race between first-creating buffer[0] and first-creating buffer[1] could leave both buffers holding the same value, with no way to fix it after the fact (immutable).
2. **Locked to two values.** Future tag classes (clothes, armor, weapons, eyes…) cannot be expressed as a 0/1 toggle.

A single `USAGE_DYNAMIC` structured buffer mapped per draw with `D3D11_MAP_WRITE_DISCARD` solves both:

- Each `Map(DISCARD)` allocates a fresh backing region. Prior draws keep their snapshot of the buffer; the upcoming draw sees the new value. This is the documented per-draw streaming pattern.
- Any float fits — classification is now free to grow.

The plugin already uses the same pattern for `g_customSRVBuffer` in `UpdateCustomBuffer_Internal`, just at frame granularity instead of draw granularity.

## What is **not** covered

- Compute dispatches (`Dispatch`, `DispatchIndirect`) and indirect draws (`DrawIndexedInstancedIndirect`, `DrawInstancedIndirect`) are not hooked. They are uncommon for the lighting/effect shader paths the plugin targets, but a fully general implementation would hook them too.
- ENB / ReShade-style post-process injectors run their own pass after the engine and may rebind slot t26 to something else. Coexistence with such injectors has not been validated.
- The command-buffer recording path. The plugin observes recorded draws on replay but cannot influence what was recorded.

# SRV Draw-Tag Details

This note documents the Fallout 4 command-buffer findings relevant to the draw-tag SRV, why the tag can fail to stick on replayed draws, and the implementation approaches available in this repo.

## Problem Summary

The plugin exposes draw classification data to replacement shaders through `GFXDrawTag`, a structured-buffer SRV bound at `DRAWTAG_SLOT` (`t26` by default). Immediate-path draws can be tagged reliably because the plugin sees a `BSRenderPass*`/`BSRenderPassLayout*` at draw time.

Command-buffer replay is different. The engine replays a prebuilt byte stream of D3D11 work. By the time replay happens, the per-pass `BSGeometry*` and `BSRenderPass*` context is not part of the replay loop. If the draw-tag SRV is not recorded into that byte stream, slot `DRAWTAG_SLOT` remains sticky and every replayed draw sees whatever value was last bound.

## Rendering Paths

Fallout 4 uses two relevant paths in `BSBatchRenderer::RenderBatches` (`0x14287F380`):

```cpp
auto* cb = BSBatchRenderer::GetCommandBufferPasses(...);
if (cb && cb->frameToken == currentFrame) {
    BSBatchRenderer::RenderCommandBufferPassesImpl(this, passGroupIdx, cb, subIdx, allowAlpha);
} else {
    BSBatchRenderer::RenderPassImpl(this, headPass, techniqueID, allowAlpha);
}
```

Immediate path:

```text
RenderPassImpl
  -> RenderPassImmediately / RenderPassImmediatelySameTechnique
      -> shader->SetupGeometry(pass)
      -> BSBatchRenderer::Draw(pass)
      -> shader->RestoreGeometry(pass)
```

Replay path:

```text
RenderCommandBufferPassesImpl
  -> BSGraphics::Renderer::ProcessCommandBuffer
      -> Map / memcpy / Unmap constant data
      -> PSSetShaderResources / VSSetShaderResources from recorded tables
      -> IA / OM / RS state
      -> Draw / DrawIndexed
```

The immediate path has pass context. The replay path only has serialized D3D11 state.

## IDA Findings

Known relevant functions in the OG runtime:

| Function | Address | Notes |
| --- | ---: | --- |
| `BSGraphics::Renderer::ProcessCommandBuffer` | `0x141D13A10` | Replays per-pass command-buffer records. |
| `BSGraphics::Renderer::BuildTextureCommandBuffer` | `0x141D14410` | Builds 24-byte texture records for PS texture slots. |
| `BSGraphics::Renderer::BuildDrawCommandBuffer` | `0x141D146B0` | Builds platform draw-state tail. |
| `BSShader::BuildCommandBuffer` | `0x1428911F0` | Central builder for the contiguous per-pass record. |
| `BSBatchRenderer::RenderBatches` | `0x14287F380` | Chooses replay vs immediate path. |
| `BSBatchRenderer::RenderPassImpl` | `0x142880030` | Immediate-path implementation. |
| `BSBatchRenderer::RenderCommandBufferPassesImpl` | `0x1428800D0` | Null-terminates command-buffer pointer list, calls replay, then renders non-buffered passes. |
| `BSBatchRenderer::RegisterPassImpl` | `0x142881A00` | Registers normal pass and appends `pass->commandBuffer` if enabled. |
| `BSBatchRenderer::UpdateCommandBufferPasses` | `0x142881B10` | Finds/allocates `CommandBufferPassesData`, appends command-buffer pointer. |
| `BSBatchRenderer::AllocateCommandBufferPassesData` | `0x142881E70` | Allocates `0x10018` bytes for pointer-list container. |
| `BSBatchRenderer::GetCommandBufferPasses` | `0x142881D90` | Looks up existing command-buffer pointer-list container. |

## CommandBufferPassesData

`CommandBufferPassesData` is not the per-draw record itself. It is a pointer list of per-pass command-buffer records.

Observed layout:

```cpp
struct CommandBufferPassesData
{
    void* commandBuffers[8192];            // +0x00000
    void** writeCursor;                    // +0x10000
    CommandBufferPassesData* nextFree;     // +0x10008
    std::uint32_t frameToken;              // +0x10010
    std::uint32_t entryIndex;              // +0x10014
};                                         // size 0x10018
```

`RenderBatches` checks `frameToken` against the global frame counter (`dword_146541F88`). If it matches, replay is used. `RenderCommandBufferPassesImpl` writes a null pointer at `*writeCursor` before calling `ProcessCommandBuffer`.

## Per-Pass Command Buffer Layout

`BSShader::BuildCommandBuffer` creates one contiguous record. The relevant high-level layout is:

```cpp
struct CommandBuffer
{
    std::uint32_t psConstantBytes;      // +0x00
    std::uint8_t  constantSlotCount;    // +0x04
    std::uint8_t  psTextureCount;       // +0x05
    std::uint8_t  shaderResourceCount;  // +0x06
    std::uint8_t  flagsOrTopology;      // +0x07
    std::uint32_t dynamicBlockBytes;    // +0x08
    // state fields through +0x87

    // +0x88:
    // dynamic/skin data
    // PS constant bytes
    // CommandBufferTexture[psTextureCount]          // 24 bytes each
    // CommandBufferShaderResource[shaderResourceCount] // 16 bytes each
    // CommandBufferPlatform draw-state tail
};
```

Replay derives section starts from the header counts:

```cpp
base = cb + 0x88;
psConstants = base + cb->dynamicBlockBytes;
textures = psConstants + cb->psConstantBytes;
shaderResources = textures + 24 * cb->psTextureCount;
platform = shaderResources + 16 * cb->shaderResourceCount;
```

This is why blindly appending bytes to an existing command buffer is unsafe: the platform tail must move if the SRV table grows.

## Recorded SRV Entry

The 16-byte SRV table entry used by replay is:

```cpp
struct CommandBufferShaderResource
{
    void* resourceWrapper;  // +0x00
    std::uint8_t slot;      // +0x08
    std::uint8_t stage;     // +0x09, 0 = VS, 1 = PS
    std::uint8_t kind;      // +0x0A
    std::uint8_t pad0B;
    std::uint32_t pad0C;
};
```

For `stage == 1`, replay calls `PSSetShaderResources`. For `kind == 0`, replay passes an SRV pointer read from `resourceWrapper + 0x08`.

The plugin therefore uses a fake wrapper with the relevant fields:

```cpp
struct FakeStructuredResource
{
    void* pad00;                       // +0x00
    ID3D11ShaderResourceView* srv;     // +0x08
    char pad10[0x20];
    std::uint32_t fenceCount;          // +0x30, kept 0
    char pad34[12];
};
```

`fenceCount` must remain zero so replay skips resource-priority/wait branches intended for real engine resources.

## Current Implementation

The repo currently hooks `BSShader::BuildCommandBuffer` and mutates `BuildCommandBufferParam` before calling the original builder:

1. Read `BSGeometry*` from param `+0x00`.
2. Read SRV count from param `+0x14`.
3. Read SRV table source pointer from param `+0x38`.
4. Copy the original SRV table into a stack scratch array.
5. Add one extra `CommandBufferShaderResource`:
   - `resourceWrapper = per-geometry FakeStructuredResource*`
   - `slot = DRAWTAG_SLOT`
   - `stage = 1`
   - `kind = 0`
6. Increment the SRV count.
7. Call original `BSShader::BuildCommandBuffer`.
8. Restore the param fields.

This avoids manually reallocating/shifting the command buffer because the original builder computes size from the mutated SRV count and copies the extended table into the newly allocated record.

The implementation also keeps immutable static SRVs for tag values:

- unknown tag (`materialTag = 0`)
- actor tag (`materialTag = 1`)

For command-buffer records, it records a stable per-geometry wrapper. That wrapper's `srv` can be updated later when actor ownership is discovered.

## Current Issues

### 1. Classification Timing

Command buffers can be built before the actor-geometry set is populated. If the command buffer records a fixed actor/unknown wrapper at build time, actors may permanently replay as unknown.

The current mitigation is per-geometry wrappers: replay records a pointer to a wrapper keyed by `BSGeometry*`, and actor refresh/removal updates that wrapper's `srv`.

Residual risk: if a command buffer is built before the plugin can even create wrapper resources, that command buffer has no draw-tag SRV entry. Those records will not be repairable without rebuilding/invalidation.

### 2. Wrapper Lifetime

Recorded command buffers may survive across frames. Any wrapper pointer recorded into a command buffer must remain valid as long as the engine might replay that command buffer.

The current implementation stores wrappers in a map and does not delete individual wrappers on detach. It updates them to unknown. That avoids dangling recorded pointers.

Do not erase per-geometry wrappers while command buffers can reference them.

### 3. Geometry Pointer Reuse

Because wrappers are keyed by raw `BSGeometry*`, a freed geometry pointer could theoretically be reused for unrelated geometry. If the wrapper remains actor-tagged, the new object can inherit the old tag.

The current implementation attempts to set wrappers back to unknown on detach/load clear. This is probably enough for normal biped teardown, but pointer reuse remains a general raw-pointer-cache risk.

### 4. Resource Initialization Timing

If `BuildCommandBuffer` fires before `g_rendererData->device` is available, draw-tag static SRVs cannot be created yet.

The previous `std::call_once` approach made this fatal for the session: one early failure prevented future attempts. The current code uses retryable initialization.

### 5. Hook Coverage

Only OG runtime addresses are currently used for `RenderCommandBufferPassesImpl` and `BuildCommandBuffer`. AE/NG builds need separate verified addresses or REL IDs.

The command-buffer injection path depends on `BSShader::BuildCommandBuffer` being installed. The install guard must include `OriginalBuildCommandBuffer`; otherwise an earlier partial install could skip it.

### 6. Existing D3D11 Hook Conflict

Immediate draws still bind the ring-buffer SRV in `BSBatchRenderer::Draw` / D3D draw hooks. Command-buffer draws bind immutable per-tag SRVs through replay. This means `g_drawTagSRV` and the actual replay-bound SRV are not always the same object.

That is expected. Debug checks based only on `g_drawTagSRV` can be misleading for replayed draws.

### 7. ENB/ReShade Context Wrapping

D3D11 vtable hooks may miss draws when another layer wraps or swaps the device context after the plugin patches the original vtable. That is one reason command-buffer recording is preferable: it makes the engine replay its own recorded `PSSetShaderResources` call for `DRAWTAG_SLOT`.

## Approaches

### Approach A: Invalidate Command Buffers

Mutate `CommandBufferPassesData::frameToken` before `RenderBatches` compares it, forcing affected pass groups through `RenderPassImpl`.

Pros:

- Simple mental model.
- Existing immediate-path tagging works.
- No command-buffer binary-layout surgery.

Cons:

- Requires reliable hook before the token comparison or inside `RenderBatches`.
- Loses command-buffer performance for affected pass groups.
- Must decide group-level invalidation, which can be too broad.

### Approach B: Bypass Replay In `RenderCommandBufferPassesImpl`

Hook `RenderCommandBufferPassesImpl`; when the pass group contains actor geometry, call `RenderPassImpl` or equivalent immediate-path functions instead of `ProcessCommandBuffer`.

Pros:

- Keeps the decision near existing replay hook.
- Reuses engine immediate draw behavior if calling `RenderPassImpl`.

Cons:

- Re-entrancy and state cleanup must match the engine.
- If done by manually calling lower-level immediate functions, it can drift from engine behavior.
- Still group-level, not truly per-record.

### Approach C: Record Draw-Tag SRV Into Command Buffers

Hook `BSShader::BuildCommandBuffer` and add one `CommandBufferShaderResource` entry for `DRAWTAG_SLOT`.

Pros:

- Correct place architecturally: replay naturally binds the tag before each draw.
- Avoids relying on D3D draw vtable hooks.
- Preserves command-buffer replay performance.

Cons:

- Depends on private command-buffer layout.
- Must handle classification timing.
- Must keep recorded wrapper pointers valid.
- Must maintain runtime-specific addresses.

Current implementation uses this approach with per-geometry wrappers.

### Approach D: Patch Replay Directly

Patch `ProcessCommandBuffer` around the replay loop and bind `DRAWTAG_SLOT` before each draw.

Pros:

- Directly affects every replayed draw.
- Does not require changing command-buffer allocation sizes.

Cons:

- Replay loop has no `BSGeometry*`.
- Would require an external synchronized queue/list mapping replay entries back to pass context.
- Very fragile because it patches a large, dense function.

### Approach E: Source Tag From Existing Engine Data

Modify shaders to infer tag from data the engine already updates per draw, such as an unused constant-buffer field or a known material/geometry property.

Pros:

- Avoids extra SRV binding.
- Works for immediate and replay if the source data is already recorded.

Cons:

- Requires finding safe, stable unused fields.
- May be shader-family specific.
- Limited tag capacity.

## Recommended Direction

Keep Approach C as the main solution, but test it with explicit diagnostics:

1. Log when `HookedBuildCommandBuffer` injects a record:
   - geometry pointer
   - geometry name
   - original SRV count
   - actor state at build time
   - wrapper pointer
2. Log when actor refresh updates a wrapper from unknown to actor.
3. Add a temporary replay-side validation hook if possible:
   - verify `DRAWTAG_SLOT` is bound during actor command-buffer replay
   - do not rely only on immediate-path `g_drawTagSRV`
4. Confirm the replacement shader actually reads `GFXDrawTag` from the configured slot.
5. If early-built command buffers still miss the injected record, force affected actor pass groups through immediate path until command buffers are rebuilt or invalidated.

The important implementation rule is: command-buffer records must not capture a final actor/unknown decision too early. They should either record mutable per-geometry state or be rebuilt/invalidated after ownership is known.

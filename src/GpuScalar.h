#pragma once

#include <fstream>
#include <string>

namespace REX::W32 {
    struct ID3D11DeviceContext;
}

// Lets a Shader.ini block expose a single-float HLSL function's return value
// to CPU consumers (e.g. LightCullPolicy) by:
//   1. Synthesizing a 1-thread compute shader that #includes the user's file
//      and calls their function, writing the result to a 1-element UAV.
//   2. Dispatching it once per frame at MyPresent (after UpdateCustomBuffer_Internal
//      so GFXInjected is fresh).
//   3. Copying the UAV to a 2-buffer ring of staging buffers and mapping
//      "the other one" for read-back. The ring guarantees 1-frame lag without
//      stalling the CPU on GPU sync.
//   4. Storing the readback value in a plain float on the probe. CPU consumers
//      call GetValuePtr(name) once to cache a stable pointer and dereference
//      it on the cull hot path.
//
// Section schema:
//   [gpuScalar:NAME]
//   include=Include/visualSkyCommon.inc   ; path relative to common include root
//   function=VU_DirectLightMul            ; HLSL function returning a single float
//   [/gpuScalar:NAME]
//
// The synthesized HLSL receives the same #includes and modular-value defines
// (`vu_FooBar` → `GFXModularFloats[i].xyzw`) that customPass shaders get, so
// the user's existing functions compile unchanged.
//
// Limitations:
// - 1-frame readback lag is permanent. Fine for slow signals (time-of-day,
//   weather curves). Bad for high-frequency oscillations.
// - One float per probe. For vec-valued probes, declare multiple probes.
namespace GpuScalar {

struct Spec {
    std::string name;
    std::string includePath;
    std::string function;
    std::string folderName;
};

void Initialize();
void Shutdown();

// Wipe all registered probes and release their GPU resources. Called by
// hot-reload alongside CustomPass::Registry::Reset.
void Reset();

// Parse a `[gpuScalar:NAME]` block. Opening line already consumed; reads
// until `endTag` ("[/gpuScalar:NAME]"). Returns true on success.
bool ParseSection(const std::string& name,
                  std::ifstream&     file,
                  const std::string& endTag,
                  const std::string& folderName);

// True if a section name looks like ours. Mirrors CustomPass::Registry::IsCustomSection.
bool IsGpuScalarSection(const std::string& sectionName);

// Called from MyPresent after UpdateCustomBuffer_Internal: lazily compiles
// any pending probes, then dispatches each + copies to staging + maps the
// previous frame's staging for readback. No-op when the context is null.
void OnFramePresent(REX::W32::ID3D11DeviceContext* context);

// Stable pointer to the probe's last-readback float. The caller may cache
// this pointer for as long as Reset() hasn't been called. Returns nullptr
// if `name` doesn't match a registered probe. The pointed-to float is
// 1.0f until the first successful readback completes (i.e. on the third
// MyPresent after the probe is registered).
const float* GetValuePtr(const std::string& name);

}  // namespace GpuScalar

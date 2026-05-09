#pragma once

// Custom render passes & custom resources.
//
// Lifts the plugin from a pure shader-replacement harness to one that can host
// dedicated full-screen / compute passes that own their own persistent textures.
// See CustomPass.md for the INI schema; this header is the C++ surface used by
// the rest of the plugin.

#include <PCH.h>
#include <d3d11.h>

namespace CustomPass {

// --- Forward types --------------------------------------------------------

class Resource;
class Pass;
class Registry;
class FileWatcher;

// HLSL hot-reload watcher specialised for customPass. Mirrors the design of
// the existing HlslFileWatcher (in Global.h) which is hard-tied to
// ShaderDefinition, but invokes a generic lambda on change so it can target
// any pass-bound state.
class FileWatcher {
public:
    using OnChange = std::function<void()>;
    FileWatcher(std::filesystem::path path, OnChange cb);
    ~FileWatcher();
    void Start();
    void Stop();

private:
    std::filesystem::path             filePath;
    std::filesystem::file_time_type   lastWriteTime{};
    OnChange                          onChange;
    std::atomic<bool>                 running{ false };
    std::thread                       watcherThread;
    std::mutex                        stopMutex;
    std::condition_variable           stopCv;
};

// --- Resource -------------------------------------------------------------

enum class ScaleMode : uint8_t { Screen, ScreenDiv, Absolute };

struct ResourceSpec {
    std::string                             name;
    REX::W32::DXGI_FORMAT                   format = REX::W32::DXGI_FORMAT_R11G11B10_FLOAT;
    ScaleMode                               scaleMode = ScaleMode::Screen;
    uint32_t                                scaleDiv = 1;       // Screen/N
    uint32_t                                absWidth = 0;       // Absolute
    uint32_t                                absHeight = 0;      // Absolute
    uint32_t                                mipLevels = 1;
    int                                     srvSlot = -1;       // -1 disables global bind
    bool                                    needRtv = true;
    bool                                    needUav = false;
    bool                                    clearOnPresent = false;
    float                                   clearColor[4] = { 0, 0, 0, 0 };
    std::string                             copyFrom;           // empty | "currentRTV" | "renderTargets[N]"
    std::string                             copyAt;             // empty | "present"
    bool                                    persistent = true;
    std::string                             pingpongWith;       // partner resource name (symmetric)
};

class Resource {
public:
    ResourceSpec                                spec{};
    REX::W32::ID3D11Texture2D*                  texture = nullptr;
    REX::W32::ID3D11RenderTargetView*           rtv = nullptr;
    REX::W32::ID3D11ShaderResourceView*         srv = nullptr;
    REX::W32::ID3D11UnorderedAccessView*        uav = nullptr;
    uint32_t                                    width = 0;
    uint32_t                                    height = 0;
    Resource*                                   pingpongPartner = nullptr;

    // Allocate / release. Idempotent: EnsureAllocated returns true if usable.
    bool EnsureAllocated(REX::W32::ID3D11Device* device,
                         uint32_t backbufferW,
                         uint32_t backbufferH);
    void Release();

    // Swap underlying texture/view pointers with `other`. Used by ping-pong.
    void SwapContents(Resource& other);
};

// --- Pass -----------------------------------------------------------------

enum class PassType : uint8_t { Pixel, Compute };
enum class TriggerKind : uint8_t {
    None,
    BeforeShaderUID,         // raw UID match — risk of asmHash collisions
    BeforeHookId,             // resolves to BeforeMatchedDefinition after ResolveHookIdTriggers()
    BeforeMatchedDefinition, // fires inside MyPSSetShader, before the engine swaps to the
                              // new shader. State seen at this moment is STALE from the
                              // previous draw — the engine sets the new shader's inputs
                              // *after* PSSetShader returns. Use this only if you want to
                              // run "between draws", not "in the same draw context as the
                              // matched shader".
    BeforeDrawForMatchedDef, // fires inside MyDraw* hooks, after the engine has set up
                              // the full pipeline for the upcoming draw. State is fresh —
                              // OM RTV / PS SRVs reflect what the matched shader will see.
                              // PREFER THIS for tonemap-piggyback patterns like SSRTGI
                              // composite that need to read/write surfaces the matched
                              // shader is about to consume.
    AtPresent
};
enum class BlendMode : uint8_t {
    Opaque,        // dst = src                               (writeMask = 0x0F)
    Additive,      // dst = src + dst                         (HDR add: e.g. SSRTGI composite)
    PremulAlpha,   // dst = src.rgb + dst.rgb * (1 - src.a)   (src is premultiplied)
    Multiply,      // dst = src.rgb * dst.rgb                 (e.g. SSAO darkening)
};

enum class InputKind : uint8_t {
    None,
    Depth,                // g_depthSRV (main scene depth)
    CurrentRTV,           // snapshot SRV of OMGetRenderTargets()[0] at fire time
    Resource,             // a customResource (by name)
    GBufferRT,            // g_rendererData->renderTargets[N].srView (explicit index)
    GBufferNormal,        // renderTargets[NORMAL_BUFFER_INDEX].srView   (kGbufferNormal=20)
    GBufferAlbedo,        // renderTargets[kGbufferAlbedo=22].srView
    GBufferMaterial,      // renderTargets[kGbufferMaterial=24].srView
    MotionVectors,        // renderTargets[kMotionVectors=29].srView
    SceneHDR,             // renderTargets[kMain=3].srView (engine HDR scene)
};

struct InputBinding {
    int                                     slot = -1;
    InputKind                               kind = InputKind::None;
    std::string                             resourceName;        // for Resource
    int                                     gbufferIndex = -1;   // for GBufferRT
};

enum class OutputKind : uint8_t {
    Resource,                  // a customResource RTV (PS) or UAV (CS)
    GBufferRT                  // g_rendererData->renderTargets[N].rtView (PS only)
};

struct OutputBinding {
    int                                     slot = -1;
    OutputKind                              kind = OutputKind::Resource;
    std::string                             resourceName;        // for Resource
    int                                     gbufferIndex = -1;   // for GBufferRT
};

struct ThreadGroupDim {
    ScaleMode                               mode = ScaleMode::Absolute;
    uint32_t                                value = 1;           // Absolute: literal; ScreenDiv: divisor
};

struct PassSpec {
    std::string                             name;
    bool                                    active = true;
    int                                     priority = 0;
    PassType                                type = PassType::Pixel;
    std::filesystem::path                   shaderFile;
    std::string                             entry = "main";
    TriggerKind                             trigger = TriggerKind::None;
    std::string                             triggerUID;          // BeforeShaderUID
    std::string                             triggerHookId;       // BeforeHookId / BeforeDrawForHookId
    // Hint set during INI parsing: pass uses beforeDrawForHook:ID rather
    // than beforeHookId:ID. Read by ResolveHookIdTriggers to pick which
    // index the resolved pass goes into.
    bool                                    atDrawTime = false;
    bool                                    oncePerFrame = true;
    std::vector<InputBinding>               inputs;
    std::vector<OutputBinding>              outputs;             // RTV (PS) or UAV (CS)
    ScaleMode                               viewportMode = ScaleMode::Screen;
    uint32_t                                viewportDiv = 1;
    BlendMode                               blend = BlendMode::Opaque;
    bool                                    clearOnFire = false;
    bool                                    depthTest = false;
    bool                                    log = false;
    std::array<ThreadGroupDim, 3>           threadGroups{};      // CS only
};

class Pass {
public:
    PassSpec                                    spec{};
    REX::W32::ID3D11PixelShader*                psShader = nullptr;
    REX::W32::ID3D11ComputeShader*              csShader = nullptr;
    ID3DBlob*                                   compiledBlob = nullptr;
    bool                                        compileTried = false;
    bool                                        compileFailed = false;
    // Optional HLSL hot-reload watcher. Created in DEVELOPMENT mode only.
    // Touching the .hlsl file releases the compiled shader so the next
    // FirePass call re-compiles from disk.
    std::unique_ptr<FileWatcher>                hlslWatcher;
    // Sentinel value so the first frame's gating compare cannot accidentally
    // match the initial currentFrame (= 0) and skip the very first fire.
    std::atomic<uint32_t>                       lastFiredFrame{ UINT32_MAX };
    // Total fires across the lifetime of this pass — used by the debug
    // overlay and the rate-limited fire log.
    std::atomic<uint64_t>                       totalFireCount{ 0 };
    // Set by FileWatcher when the .hlsl file changes on disk. FirePass
    // observes the flag on the main render thread, drops the compiled
    // shader, and recompiles before dispatching. This avoids the watcher
    // thread mutating D3D11 objects directly (deadlock + use-after-free risk
    // if FirePass was mid-flight).
    std::atomic<bool>                           reloadRequested{ false };
    // Per-pass compile mutex. Held by EnsureCompiled so the background
    // precompile worker and the render-thread FirePass path can't both
    // compile this pass at once. unique_ptr because std::mutex is non-movable.
    std::unique_ptr<std::mutex>                 compileMutex = std::make_unique<std::mutex>();

    void Release();
};

// --- Snapshot SRV cache ---------------------------------------------------

// Maintains an SRV that views the texture currently bound as RTV0 at fire
// time. Engine HDR / scene-color textures are created with BIND_SHADER_RESOURCE
// so we can construct an SRV directly without copying. The cache is keyed by
// texture pointer so we don't leak SRVs when the bound RTV cycles between a
// few stable textures.
class SnapshotSrvCache {
public:
    REX::W32::ID3D11ShaderResourceView* Get(REX::W32::ID3D11Device* device,
                                            REX::W32::ID3D11Texture2D* texture);
    void Release();

private:
    std::unordered_map<REX::W32::ID3D11Texture2D*, REX::W32::ID3D11ShaderResourceView*> entries;
    std::mutex                                                                          mutex;
};

// --- Registry -------------------------------------------------------------

class Registry {
public:
    // Parse a `[customResource:NAME]` or `[customPass:NAME]` block. The opening
    // line has already been consumed by the host parser; `endTag` is the
    // expected `[/customX:NAME]` closer. Reads from `file` until the closer.
    bool ParseSection(const std::string&              sectionName,
                      std::ifstream&                  file,
                      const std::string&              endTag,
                      const std::filesystem::path&    folderPath,
                      const std::string&              folderName);

    static bool IsCustomSection(const std::string& sectionName);

    // Wipe everything; called on hot reload.
    void Reset();

    // Spawn HLSL hot-reload watchers for every pass currently registered.
    // Called once after Shader.ini parsing completes when DEVELOPMENT=true.
    void StartFileWatchers();

    // Submit one compile job per registered pass to the background
    // precompile worker. The worker is owned externally (g_precompileWorker)
    // — we just hand it std::function<void()>s. Each job locks the per-pass
    // compileMutex inside EnsureCompiled, so a render-thread FirePass that
    // hits the same pass while it's mid-compile blocks until the worker is
    // done, then short-circuits on the now-populated psShader/csShader.
    void EnqueuePrecompileJobs();

    // Frame hooks ------------------------------------------------------------
    // Called from MyPSSetShader before the engine swap takes effect. Returns
    // true if a pass fired (so caller knows state was disturbed and may need
    // to re-bind injected resources afterwards).
    bool OnBeforeShaderBound(REX::W32::ID3D11DeviceContext* context,
                             REX::W32::ID3D11PixelShader*    originalPS);

    // Called from MyDraw / MyDrawIndexed / MyDrawIndexedInstanced /
    // MyDrawInstanced just BEFORE forwarding to the original, and also from
    // the engine-level HookedBSBatchRendererDraw. The `source` tag is a
    // short label ("d3d11-DrawIndexed", "engine-BSBatch", etc.) recorded in
    // diagnostic logs so we can tell which hook path actually catches a
    // given draw — useful when ENB / ReShade context wrapping makes the
    // D3D11 vtable hooks unreliable. Returns true if a pass fired.
    bool OnBeforeDraw(REX::W32::ID3D11DeviceContext* context, const char* source);

    // Called from MyPresent. Performs:
    //   - resource allocation for new resources
    //   - copyAt=present operations
    //   - clearOnPresent operations
    //   - any AtPresent passes
    //   - per-frame fired flag reset
    //   - ping-pong swap
    //   - global SRV bind refresh
    void OnFramePresent(REX::W32::ID3D11DeviceContext* context);

    // Called from BindInjectedPixelShaderResources to keep customResource
    // SRVs bound on their declared slots after engine state changes.
    void BindGlobalResourceSRVs(REX::W32::ID3D11DeviceContext* context, bool pixelStage);

    // Resolve a hook id (existing [shaderId] section) to its first ShaderUID,
    // used by triggerHookId. Called once after Shader.ini load completes.
    void ResolveHookIdTriggers();

    // Diagnostic for ImGui debug overlay.
    size_t ResourceCount() const;
    size_t PassCount() const;

    // Render the ImGui debug window. Called from Plugin.cpp's Present hook.
    void DrawDebugOverlay();

private:
    bool                                ParseResourceSection(const std::string&,  std::ifstream&, const std::string&);
    bool                                ParsePassSection    (const std::string&,  std::ifstream&,  const std::string&,
                                                            const std::filesystem::path&);
    Resource*                           FindResource(const std::string& name);
    bool                                EnsureCompiled(Pass& pass);
    bool                                EnsurePassResources(Pass& pass);
    void                                FirePass(REX::W32::ID3D11DeviceContext* context, Pass& pass);
    void                                ApplyPingpong();

    // Containers store unique_ptrs so address stability survives rehash.
    std::vector<std::unique_ptr<Resource>>          resources;
    std::vector<std::unique_ptr<Pass>>              passes;
    std::unordered_multimap<std::string, Pass*>          uidIndex;     // BeforeShaderUID
    std::unordered_multimap<std::string, Pass*>          hookIdIndex;  // BeforeHookId / BeforeDrawForHookId pre-resolution
    std::unordered_multimap<ShaderDefinition*, Pass*>    defIndex;     // BeforeMatchedDefinition
    std::unordered_multimap<ShaderDefinition*, Pass*>    drawDefIndex; // BeforeDrawForMatchedDef
    std::unordered_map<std::string, Resource*>      resourceIndex;  // by name
    SnapshotSrvCache                                snapshotCache;
    mutable std::mutex                              mutex;
    uint32_t                                        currentFrame = 0;
};

// Single shared registry. Lifetime: process; reset on Shader.ini hot reload.
extern Registry g_registry;

// --- Built-in fullscreen VS used by every PS-typed pass -------------------
// Compiled once on first PS pass fire and cached.
REX::W32::ID3D11VertexShader* GetFullscreenTriangleVS(REX::W32::ID3D11Device* device);

}  // namespace CustomPass

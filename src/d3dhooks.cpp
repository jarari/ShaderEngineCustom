#include <Global.h>
#include <PCH.h>
#include <CustomPass.h>
#include <Plugin.h>
#include "d3dhooks.h"
#include <GpuScalar.h>
#include <hooks.h>
#include <ImguiMenu.h>
#include <LightTracker.h>
#include <PhaseTelemetry.h>
#include <RenderTargets.h>
#include <ShaderPipeline.h>
#include <ShaderResources.h>
#include <ShadowTelemetry.h>

extern HWND g_outputWindow;
extern std::atomic<REX::W32::ID3D11PixelShader*> g_currentOriginalPixelShader;

namespace D3D11Hooks
{
    D3D11CreateDeviceAndSwapChain_t OriginalD3D11CreateDeviceAndSwapChain = nullptr;
    ClipCursor_t OriginalClipCursor = nullptr;
    Present_t OriginalPresent = nullptr;
    PSSetShaderResources_t OriginalPSSetShaderResources = nullptr;
    OMSetRenderTargets_t OriginalOMSetRenderTargets = nullptr;
    ClearDepthStencilView_t OriginalClearDepthStencilView = nullptr;
    DrawIndexed_t OriginalDrawIndexed = nullptr;
    Draw_t OriginalDraw = nullptr;
    DrawIndexedInstanced_t OriginalDrawIndexedInstanced = nullptr;
    DrawInstanced_t OriginalDrawInstanced = nullptr;
    PSSetShader_t OriginalPSSetShader = nullptr;
    VSSetShader_t OriginalVSSetShader = nullptr;
    CreatePixelShader_t OriginalCreatePixelShader = nullptr;
    CreateVertexShader_t OriginalCreateVertexShader = nullptr;
}

RE::PlayerCharacter* g_player = nullptr;
RE::ActorValue* g_actorValueInfo = nullptr;
RE::Sky* g_sky = nullptr;

static std::atomic<std::uint64_t> g_d3dDrawCallsThisFrame{ 0 };
static std::atomic<std::uint64_t> g_d3dDrawCallsLastFrame{ 0 };

std::uint64_t GetD3DDrawCallsLastFrame_Internal()
{
    return g_d3dDrawCallsLastFrame.load(std::memory_order_relaxed);
}


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
void ResetPlayerRadDamageTracking()
{
    g_lastRad = 0.0f;
    g_radDmg = 0.0f;
    g_haveRadBaseline = false;
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

    // View-space L1 SH packing ? derived basis matching engine's cb2[6..8].
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
    // rotate together ? but we have evidence the gbuffer normal in
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
    ShaderResources::PackModularShaderValues(g_shaderSettings);

    // Create or update our custom buffer resource views with the new data.
    if (!g_rendererData) {
        g_rendererData = RE::BSGraphics::GetRendererData();
        if (!g_rendererData) {
            REX::WARN("UpdateCustomBuffer_Internal: Cannot update custom buffer: renderer data not ready");
            return;
        }
    }

    ShaderResources::EnsureInjectedShaderResourceViews(g_rendererData->device);
    ShaderResources::UpdateInjectedShaderResourceViews(g_rendererData->context);
}


// --- D3D11 hook handlers ---

void D3D11OnPresent_Internal()
{
    g_d3dDrawCallsLastFrame.store(
        g_d3dDrawCallsThisFrame.exchange(0, std::memory_order_relaxed),
        std::memory_order_relaxed);

    // Light-tracker frame boundary: finalize the previous capture (if any),
    // poll Numpad * for new arms, open the next capture file. No-op when
    // DEVELOPMENT is off.
    LightTracker::Tick();
    if (CUSTOMBUFFER_ON) {
        UpdateCustomBuffer_Internal();
    }
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
    UIRenderFrame();
}

void D3D11OnPSSetShaderResources_Internal(REX::W32::ID3D11DeviceContext* context)
{
    if (g_customPassRendering) {
        return;
    }
    if (ShaderResources::ActiveReplacementPixelShaderNeedsResourceRebind() && !g_bindingInjectedPixelResources) {
        ShaderResources::BindInjectedPixelShaderResources(context);
    }
}

void D3D11OnOMSetRenderTargets_Internal(
    REX::W32::ID3D11DeviceContext*,
    UINT numViews,
    REX::W32::ID3D11RenderTargetView* const* renderTargetViews,
    REX::W32::ID3D11DepthStencilView* depthStencilView)
{
    ShaderResources::TrackOMRenderTargets(numViews, renderTargetViews, depthStencilView);
}

bool D3D11ShouldSuppressClearDepthStencilView_Internal(
    REX::W32::ID3D11DeviceContext* context,
    REX::W32::ID3D11DepthStencilView* depthStencilView,
    UINT clearFlags,
    FLOAT depth,
    UINT8 stencil)
{
    return SHADOW_CACHE_DIRECTIONAL_MAPSLOT1_ON &&
        ShadowTelemetry::HandleShadowCacheClearDepthStencilView(
            context, depthStencilView, clearFlags, depth, stencil);
}

void D3D11OnDraw_Internal(REX::W32::ID3D11DeviceContext* context, const char* source)
{
    if (g_customPassRendering) {
        return;
    }
    g_d3dDrawCallsThisFrame.fetch_add(1, std::memory_order_relaxed);
    if (PhaseTelemetry::g_mode.load(std::memory_order_relaxed) == PhaseTelemetry::Mode::On) {
        PhaseTelemetry::OnD3DDraw();
    }
    if (ShadowTelemetry::g_mode.load(std::memory_order_relaxed) == ShadowTelemetry::Mode::On) {
        ShadowTelemetry::OnD3DDraw();
    }
    // BindDrawTagForCurrentDraw(context);
    FireArmedCustomPassDrawBatch(context, source);
    if (ShaderResources::ActiveReplacementPixelShaderNeedsResourceRebind()) {
        ShaderResources::BindInjectedPixelShaderResources(context);
    }
}

D3D11PSSetShaderResult D3D11OnPSSetShaderBefore_Internal(
    REX::W32::ID3D11DeviceContext* context,
    REX::W32::ID3D11PixelShader* pixelShader)
{
    D3D11PSSetShaderResult result{};
    result.shader = pixelShader;
    if (g_customPassRendering) {
        return result;
    }

    g_currentOriginalPixelShader.store(pixelShader, std::memory_order_release);
    ArmCustomPassDrawBatch(pixelShader);

    // Light-tracker capture at PS-bind time. Engine sets RTVs / blend /
    // scissor / cb2 / SRVs before PSSetShader, so all the per-pass state
    // we want is live at this moment. Cheap no-op when not capturing.
    LightTracker::OnPSBind(context, pixelShader);
    // Trigger any customPass blocks attached to this original shader. The
    // pass runs immediately before the hook forwards to the original
    // PSSetShader so the engine state we save/restore is the state the engine
    // just established for the upcoming shader.
    if (pixelShader && !g_isCreatingReplacementShader && !g_bindingInjectedPixelResources) {
        result.customPassFired = CustomPass::g_registry.OnBeforeShaderBound(context, pixelShader);
    }
    if (pixelShader) {
        // Check if this shader is matched with a replacement shader in our DB
        if (g_ShaderDB.IsEntryMatched(pixelShader)) {
            g_ShaderDB.SetEntryRecentlyUsed(pixelShader, true);
            auto* matchedDefinition = g_ShaderDB.GetMatchedDefinition(pixelShader);
            // If the HLSL watcher flagged a disk change for this definition,
            // drop the cached compiled shader + replacement pointers BEFORE we
            // read them below. Done here on the render thread so the D3D11
            // Release happens on the immediate-context-owning thread.
            MaybeApplyHlslHotReload_Internal(matchedDefinition);
            auto* replacementPixelShader = g_ShaderDB.GetReplacementShader(pixelShader);
            if (replacementPixelShader) {
                if (DEBUGGING) {
                    REX::INFO("MyPSSetShader: Replacing pixel shader with matched replacement for definition '{}'", matchedDefinition ? matchedDefinition->id : "Unknown");
                }
                result.shader = replacementPixelShader;
                result.usingReplacementPixelShader = true;
                result.activeReplacementDef = matchedDefinition;
            } else {
                if (matchedDefinition && !matchedDefinition->buggy) {
                    if (DEBUGGING) {
                        REX::INFO("MyPSSetShader: Shader is matched but no replacement shader found, trying to compile...");
                    }
                    if (CompileShader_Internal(matchedDefinition)) {
                        g_ShaderDB.SetReplacementShader(pixelShader, matchedDefinition->loadedPixelShader);
                        if (DEBUGGING) {
                            REX::INFO("MyPSSetShader: Compiled replacement shader for definition '{}'", matchedDefinition->id);
                            REX::INFO("MyPSSetShader: Replacing pixel shader with newly compiled replacement for definition '{}'", matchedDefinition->id);
                        }
                        result.shader = g_ShaderDB.GetReplacementShader(pixelShader);
                        result.usingReplacementPixelShader = result.shader != nullptr;
                        result.activeReplacementDef = result.usingReplacementPixelShader ? matchedDefinition : nullptr;
                    } else {
                        REX::WARN("MyPSSetShader: Failed to compile replacement shader for definition '{}'", matchedDefinition->id);
                        matchedDefinition->buggy = true;
                    }
                }
            }
        }
    }

    return result;
}

void D3D11OnPSSetShaderAfter_Internal(
    REX::W32::ID3D11DeviceContext* context,
    const D3D11PSSetShaderResult& result)
{
    if (g_customPassRendering) {
        return;
    }
    ShaderResources::SetActiveReplacementPixelShaderUsage(result.activeReplacementDef, result.usingReplacementPixelShader);
    if (ShaderResources::ActiveReplacementPixelShaderNeedsResourceRebind()) {
        ShaderResources::BindInjectedPixelShaderResources(context);
    }
    // If a customPass fired, the engine's next draw still expects the injected
    // resource set we publish for replacement shaders (depth, GFXInjected, etc.)
    // to be present on its slots. The pass already re-binds them, but there is
    // no harm in publishing them again here when running a replacement.
    if (result.customPassFired && ShaderResources::ActiveReplacementPixelShaderNeedsResourceRebind()) {
        ShaderResources::BindInjectedPixelShaderResources(context);
    }
}

REX::W32::ID3D11VertexShader* D3D11OnVSSetShader_Internal(
    REX::W32::ID3D11DeviceContext* context,
    REX::W32::ID3D11VertexShader* vertexShader)
{
    if (g_customPassRendering) {
        return vertexShader;
    }
    if (vertexShader) {
        // Check if this shader is matched with a replacement shader in our DB
        if (g_ShaderDB.IsEntryMatched(vertexShader)) {
            g_ShaderDB.SetEntryRecentlyUsed(vertexShader, true);
            auto* matchedDefinition = g_ShaderDB.GetMatchedDefinition(vertexShader);
            // See PSSetShader for rationale: we evict cached compiled state
            // here, before reading the replacement, so the D3D11 Release runs
            // on the render thread.
            MaybeApplyHlslHotReload_Internal(matchedDefinition);
            auto* replacementVertexShader = g_ShaderDB.GetReplacementShader(vertexShader);
            if (replacementVertexShader) {
                if (DEBUGGING) {
                    REX::INFO("MyVSSetShader: Replacing vertex shader with matched replacement for definition '{}'", matchedDefinition ? matchedDefinition->id : "Unknown");
                }
                vertexShader = replacementVertexShader;
                ShaderResources::BindInjectedVertexShaderResources(context);
                ShaderResources::BindReplacementSRVResources(context, matchedDefinition, /*pixelStage=*/false);
                ShaderResources::BindReplacementTextureResources(context, matchedDefinition, /*pixelStage=*/false);
            } else {
                if (matchedDefinition && !matchedDefinition->buggy) {
                    if (DEBUGGING) {
                        REX::INFO("MyVSSetShader: Shader is matched but no replacement shader found, trying to compile...");
                    }
                    if (CompileShader_Internal(matchedDefinition)) {
                        g_ShaderDB.SetReplacementShader(vertexShader, matchedDefinition->loadedVertexShader);
                        if (DEBUGGING) {
                            REX::INFO("MyVSSetShader: Compiled replacement shader for definition '{}'", matchedDefinition->id);
                            REX::INFO("MyVSSetShader: Replacing vertex shader with newly compiled replacement for definition '{}'", matchedDefinition->id);
                        }
                        vertexShader = g_ShaderDB.GetReplacementShader(vertexShader);
                        ShaderResources::BindInjectedVertexShaderResources(context);
                        ShaderResources::BindReplacementSRVResources(context, matchedDefinition, /*pixelStage=*/false);
                        ShaderResources::BindReplacementTextureResources(context, matchedDefinition, /*pixelStage=*/false);
                    } else {
                        REX::WARN("MyVSSetShader: Failed to compile replacement shader for definition '{}'", matchedDefinition->id);
                        matchedDefinition->buggy = true;
                    }
                }
            }
        }
    }

    return vertexShader;
}

void D3D11OnCreatePixelShader_Internal(
    HRESULT hr,
    const void* shaderBytecode,
    SIZE_T bytecodeLength,
    REX::W32::ID3D11PixelShader** pixelShader)
{
    if (REX::W32::SUCCESS(hr) && pixelShader && *pixelShader) {
        std::vector<uint8_t> bytecode(bytecodeLength);
        std::memcpy(bytecode.data(), shaderBytecode, bytecodeLength);
        if (g_ShaderDB.HasEntry(*pixelShader)) {
            return;
        }
        ShaderDBEntry entry = AnalyzeShader_Internal(*pixelShader, nullptr, std::move(bytecode), bytecodeLength);
        g_ShaderDB.AddShaderEntry(std::move(entry));
    }
}

void D3D11OnCreateVertexShader_Internal(
    HRESULT hr,
    const void* shaderBytecode,
    SIZE_T bytecodeLength,
    REX::W32::ID3D11VertexShader** vertexShader)
{
    if (REX::W32::SUCCESS(hr) && vertexShader && *vertexShader) {
        std::vector<uint8_t> bytecode(bytecodeLength);
        std::memcpy(bytecode.data(), shaderBytecode, bytecodeLength);
        if (g_ShaderDB.HasEntry(*vertexShader)) {
            return;
        }
        ShaderDBEntry entry = AnalyzeShader_Internal(nullptr, *vertexShader, std::move(bytecode), bytecodeLength);
        g_ShaderDB.AddShaderEntry(std::move(entry));
    }
}
namespace
{
    HRESULT STDMETHODCALLTYPE MyPresent(
        REX::W32::IDXGISwapChain* swapChain,
        UINT syncInterval,
        UINT flags)
    {
        D3D11Hooks::EnsureDrawHooksPresent();
        D3D11OnPresent_Internal();
        return D3D11Hooks::OriginalPresent(swapChain, syncInterval, flags);
    }

    void STDMETHODCALLTYPE MyPSSetShaderResources(
        REX::W32::ID3D11DeviceContext* context,
        UINT startSlot,
        UINT numViews,
        REX::W32::ID3D11ShaderResourceView* const* shaderResourceViews)
    {
        D3D11Hooks::OriginalPSSetShaderResources(context, startSlot, numViews, shaderResourceViews);
        D3D11OnPSSetShaderResources_Internal(context);
    }

    void STDMETHODCALLTYPE MyOMSetRenderTargets(
        REX::W32::ID3D11DeviceContext* context,
        UINT numViews,
        REX::W32::ID3D11RenderTargetView* const* renderTargetViews,
        REX::W32::ID3D11DepthStencilView* depthStencilView)
    {
        D3D11Hooks::OriginalOMSetRenderTargets(context, numViews, renderTargetViews, depthStencilView);
        D3D11OnOMSetRenderTargets_Internal(context, numViews, renderTargetViews, depthStencilView);
    }

    void STDMETHODCALLTYPE MyClearDepthStencilView(
        REX::W32::ID3D11DeviceContext* context,
        REX::W32::ID3D11DepthStencilView* depthStencilView,
        UINT clearFlags,
        FLOAT depth,
        UINT8 stencil)
    {
        if (D3D11ShouldSuppressClearDepthStencilView_Internal(context, depthStencilView, clearFlags, depth, stencil)) {
            return;
        }

        D3D11Hooks::OriginalClearDepthStencilView(context, depthStencilView, clearFlags, depth, stencil);
    }

    void STDMETHODCALLTYPE MyDrawIndexed(
        REX::W32::ID3D11DeviceContext* context,
        UINT indexCount,
        UINT startIndexLocation,
        INT baseVertexLocation)
    {
        D3D11OnDraw_Internal(context, "d3d11-DrawIndexed");
        D3D11Hooks::OriginalDrawIndexed(context, indexCount, startIndexLocation, baseVertexLocation);
    }

    void STDMETHODCALLTYPE MyDraw(
        REX::W32::ID3D11DeviceContext* context,
        UINT vertexCount,
        UINT startVertexLocation)
    {
        D3D11OnDraw_Internal(context, "d3d11-Draw");
        D3D11Hooks::OriginalDraw(context, vertexCount, startVertexLocation);
    }

    void STDMETHODCALLTYPE MyDrawIndexedInstanced(
        REX::W32::ID3D11DeviceContext* context,
        UINT indexCountPerInstance,
        UINT instanceCount,
        UINT startIndexLocation,
        INT baseVertexLocation,
        UINT startInstanceLocation)
    {
        D3D11OnDraw_Internal(context, "d3d11-DrawIndexedInstanced");
        D3D11Hooks::OriginalDrawIndexedInstanced(
            context,
            indexCountPerInstance,
            instanceCount,
            startIndexLocation,
            baseVertexLocation,
            startInstanceLocation);
    }

    void STDMETHODCALLTYPE MyDrawInstanced(
        REX::W32::ID3D11DeviceContext* context,
        UINT vertexCountPerInstance,
        UINT instanceCount,
        UINT startVertexLocation,
        UINT startInstanceLocation)
    {
        D3D11OnDraw_Internal(context, "d3d11-DrawInstanced");
        D3D11Hooks::OriginalDrawInstanced(
            context,
            vertexCountPerInstance,
            instanceCount,
            startVertexLocation,
            startInstanceLocation);
    }

    void STDMETHODCALLTYPE MyPSSetShader(
        REX::W32::ID3D11DeviceContext* context,
        REX::W32::ID3D11PixelShader* pixelShader,
        REX::W32::ID3D11ClassInstance* const* classInstances,
        UINT numClassInstances)
    {
        auto result = D3D11OnPSSetShaderBefore_Internal(context, pixelShader);
        D3D11Hooks::OriginalPSSetShader(context, result.shader, classInstances, numClassInstances);
        D3D11OnPSSetShaderAfter_Internal(context, result);
    }

    void STDMETHODCALLTYPE MyVSSetShader(
        REX::W32::ID3D11DeviceContext* context,
        REX::W32::ID3D11VertexShader* vertexShader,
        REX::W32::ID3D11ClassInstance* const* classInstances,
        UINT numClassInstances)
    {
        vertexShader = D3D11OnVSSetShader_Internal(context, vertexShader);
        D3D11Hooks::OriginalVSSetShader(context, vertexShader, classInstances, numClassInstances);
    }

    HRESULT STDMETHODCALLTYPE MyCreatePixelShader(
        REX::W32::ID3D11Device* device,
        const void* shaderBytecode,
        SIZE_T bytecodeLength,
        REX::W32::ID3D11ClassLinkage* classLinkage,
        REX::W32::ID3D11PixelShader** pixelShader)
    {
        if (g_isCreatingReplacementShader) {
            return D3D11Hooks::OriginalCreatePixelShader(device, shaderBytecode, bytecodeLength, classLinkage, pixelShader);
        }

        const HRESULT hr = D3D11Hooks::OriginalCreatePixelShader(device, shaderBytecode, bytecodeLength, classLinkage, pixelShader);
        D3D11OnCreatePixelShader_Internal(hr, shaderBytecode, bytecodeLength, pixelShader);
        return hr;
    }

    HRESULT STDMETHODCALLTYPE MyCreateVertexShader(
        REX::W32::ID3D11Device* device,
        const void* shaderBytecode,
        SIZE_T bytecodeLength,
        REX::W32::ID3D11ClassLinkage* classLinkage,
        REX::W32::ID3D11VertexShader** vertexShader)
    {
        if (g_isCreatingReplacementShader) {
            return D3D11Hooks::OriginalCreateVertexShader(device, shaderBytecode, bytecodeLength, classLinkage, vertexShader);
        }

        const HRESULT hr = D3D11Hooks::OriginalCreateVertexShader(device, shaderBytecode, bytecodeLength, classLinkage, vertexShader);
        D3D11OnCreateVertexShader_Internal(hr, shaderBytecode, bytecodeLength, vertexShader);
        return hr;
    }

    BOOL WINAPI HookedClipCursor(const RECT* rect)
    {
        if (g_imguiInitialized && UIIsMenuOpen()) {
            rect = UIGetWindowRect();
        }
        return D3D11Hooks::OriginalClipCursor(rect);
    }

    HRESULT WINAPI HookedD3D11CreateDeviceAndSwapChain(
        IDXGIAdapter* adapter,
        D3D_DRIVER_TYPE driverType,
        HMODULE software,
        UINT flags,
        const D3D_FEATURE_LEVEL* featureLevels,
        UINT featureLevelCount,
        UINT sdkVersion,
        const DXGI_SWAP_CHAIN_DESC* swapChainDesc,
        IDXGISwapChain** swapChain,
        ID3D11Device** device,
        D3D_FEATURE_LEVEL* featureLevel,
        ID3D11DeviceContext** immediateContext)
    {
        const HRESULT hr = D3D11Hooks::OriginalD3D11CreateDeviceAndSwapChain(
            adapter,
            driverType,
            software,
            flags,
            featureLevels,
            featureLevelCount,
            sdkVersion,
            swapChainDesc,
            swapChain,
            device,
            featureLevel,
            immediateContext);

        if (swapChainDesc) {
            g_outputWindow = swapChainDesc->OutputWindow;
        }

        if (!g_rendererData || !g_rendererData->device) {
            g_rendererData = RE::BSGraphics::GetRendererData();
        }
        if (!g_rendererData || !g_rendererData->device) {
            return hr;
        }

        auto* deviceVTable = *reinterpret_cast<void***>(g_rendererData->device);
        if (!Hooks::InstallVTableSlot(deviceVTable, 15, reinterpret_cast<void*>(MyCreatePixelShader), D3D11Hooks::OriginalCreatePixelShader)) {
            REX::WARN("InstallShaderCreationHooks_Internal: VirtualProtect failed for CreatePixelShader");
            return hr;
        }
        REX::INFO("InstallShaderCreationHooks_Internal: CreatePixelShader hook installed");

        if (!Hooks::InstallVTableSlot(deviceVTable, 12, reinterpret_cast<void*>(MyCreateVertexShader), D3D11Hooks::OriginalCreateVertexShader)) {
            REX::WARN("InstallShaderCreationHooks_Internal: VirtualProtect failed for CreateVertexShader");
            return hr;
        }
        REX::INFO("InstallShaderCreationHooks_Internal: CreateVertexShader hook installed");

        return hr;
    }
}

namespace D3D11Hooks
{
    void EnsureDrawHooksPresent()
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

        Hooks::EnsureVTableSlot(vtable, 12, reinterpret_cast<void*>(MyDrawIndexed), OriginalDrawIndexed);
        Hooks::EnsureVTableSlot(vtable, 13, reinterpret_cast<void*>(MyDraw), OriginalDraw);
        Hooks::EnsureVTableSlot(vtable, 20, reinterpret_cast<void*>(MyDrawIndexedInstanced), OriginalDrawIndexedInstanced);
        Hooks::EnsureVTableSlot(vtable, 21, reinterpret_cast<void*>(MyDrawInstanced), OriginalDrawInstanced);
        Hooks::EnsureVTableSlot(vtable, 53, reinterpret_cast<void*>(MyClearDepthStencilView), OriginalClearDepthStencilView);
    }
}

bool InstallGFXHooks_Internal()
{
    if (!g_rendererData) {
        g_rendererData = RE::BSGraphics::GetRendererData();
    }
    if (!g_rendererData || !g_rendererData->device) {
        REX::WARN("InstallGFXHooks_Internal: Cannot install hook: device not ready");
        return false;
    }

    auto installVTableHook = [](void** vtable, std::size_t index, void* hook, auto& original, const char* name) {
        if (!Hooks::InstallVTableSlot(vtable, index, hook, original)) {
            REX::WARN("InstallGFXHooks_Internal: VirtualProtect failed for {}", name);
            return false;
        }
        REX::INFO("InstallGFXHooks_Internal: {} hook installed", name);
        return true;
    };

    auto* contextVTable = *reinterpret_cast<void***>(g_rendererData->context);
    if (!installVTableHook(contextVTable, 12, reinterpret_cast<void*>(MyDrawIndexed), D3D11Hooks::OriginalDrawIndexed, "DrawIndexed") ||
        !installVTableHook(contextVTable, 13, reinterpret_cast<void*>(MyDraw), D3D11Hooks::OriginalDraw, "Draw") ||
        !installVTableHook(contextVTable, 20, reinterpret_cast<void*>(MyDrawIndexedInstanced), D3D11Hooks::OriginalDrawIndexedInstanced, "DrawIndexedInstanced") ||
        !installVTableHook(contextVTable, 21, reinterpret_cast<void*>(MyDrawInstanced), D3D11Hooks::OriginalDrawInstanced, "DrawInstanced") ||
        !installVTableHook(contextVTable, 8, reinterpret_cast<void*>(MyPSSetShaderResources), D3D11Hooks::OriginalPSSetShaderResources, "PSSetShaderResources") ||
        !installVTableHook(contextVTable, 33, reinterpret_cast<void*>(MyOMSetRenderTargets), D3D11Hooks::OriginalOMSetRenderTargets, "OMSetRenderTargets") ||
        !installVTableHook(contextVTable, 53, reinterpret_cast<void*>(MyClearDepthStencilView), D3D11Hooks::OriginalClearDepthStencilView, "ClearDepthStencilView") ||
        !installVTableHook(contextVTable, 9, reinterpret_cast<void*>(MyPSSetShader), D3D11Hooks::OriginalPSSetShader, "PSSetShader") ||
        !installVTableHook(contextVTable, 11, reinterpret_cast<void*>(MyVSSetShader), D3D11Hooks::OriginalVSSetShader, "VSSetShader")) {
        return false;
    }

    auto* swapChain = g_rendererData->renderWindow[0].swapChain;
    auto* swapChainVTable = *reinterpret_cast<void***>(swapChain);
    if (!installVTableHook(swapChainVTable, 8, reinterpret_cast<void*>(MyPresent), D3D11Hooks::OriginalPresent, "Present")) {
        return false;
    }

    REX::INFO("InstallGFXHooks_Internal: All Hooks installed successfully");
    if (!DEVGUI_ON && !SHADERSETTINGS_ON) {
        REX::INFO("InstallGFXHooks_Internal: DEVGUI_ON and SHADERSETTINGS_ON are false, skipping ImGui initialization");
        return true;
    }

    HWND hwnd = g_outputWindow ? g_outputWindow : FindWindowA("Fallout4", nullptr);
    return UIInitialize(hwnd);
}

bool InstallShaderCreationHooks_Internal()
{
    REX::INFO("Hooking D3D11CreateDeviceAndSwapChain");

    std::uintptr_t d3d11Addr = Hooks::Addresses::D3D11CreateDeviceAndSwapChainCall.address();
    if (REX::FModule::GetRuntimeIndex() == REX::FModule::Runtime::kOG) {
        d3d11Addr += Hooks::Offsets::D3D11CreateDeviceAndSwapChainCallOG;
    } else {
        d3d11Addr += Hooks::Offsets::D3D11CreateDeviceAndSwapChainCallAE;
    }

    REL::Relocation<std::uintptr_t> callSite{ d3d11Addr };
    D3D11Hooks::OriginalD3D11CreateDeviceAndSwapChain =
        reinterpret_cast<D3D11Hooks::D3D11CreateDeviceAndSwapChain_t>(
            callSite.write_call<5>(reinterpret_cast<std::uintptr_t>(&HookedD3D11CreateDeviceAndSwapChain)));

    D3D11Hooks::OriginalClipCursor =
        *reinterpret_cast<D3D11Hooks::ClipCursor_t*>(Hooks::Addresses::ClipCursor.address());
    Hooks::Addresses::ClipCursor.write_vfunc(0, &HookedClipCursor);

    return true;
}

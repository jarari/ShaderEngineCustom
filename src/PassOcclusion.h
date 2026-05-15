#pragma once

#include <PCH.h>

struct BSRenderPassLayout;

namespace PassOcclusion
{
    struct PassOcclusionState;

    struct Decision
    {
        PassOcclusionState* state = nullptr;
        bool queryActive = false;
        bool skip = false;
    };

    struct ArenaGatePatch
    {
        std::uint16_t* gate = nullptr;
        std::uint16_t original = 0;
    };

    Decision BeginDecision(
        REX::W32::ID3D11DeviceContext* context,
        void* batchRenderer,
        BSRenderPassLayout* head,
        unsigned int group,
        bool allowAlpha,
        bool commandBufferPath);
    void EndDecision(REX::W32::ID3D11DeviceContext* context, Decision& decision);

    bool ShouldEarlyCullRegisterObjectStandard(RE::BSGeometry* geometry);
    void PatchArenaGates(void* arena, std::vector<ArenaGatePatch>& patches);
    void RestoreArenaGates(std::vector<ArenaGatePatch>& patches);

    void OnFramePresent();
    void ShutdownCache();
}

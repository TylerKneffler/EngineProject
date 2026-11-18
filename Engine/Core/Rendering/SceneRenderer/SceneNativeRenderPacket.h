#pragma once

#include "Core/Rendering/SceneRenderer.h"

namespace Engine::Rendering
{
struct SceneRenderer::NativeRenderPacket
{
    std::vector<Engine::Rendering::VegetationBendCapsule> grassCapsules;
    std::vector<FrameRenderItem> items;
    std::vector<uint32_t> renderSlotItemOffsets;
    std::vector<uint32_t> renderSlotItems;
    std::vector<std::shared_ptr<IGraphicsBuffer>> meshBuffers;
};

}

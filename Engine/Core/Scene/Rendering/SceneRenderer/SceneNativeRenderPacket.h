#pragma once

#include "Core/Scene/Scene.h"

namespace Engine::Scene
{
struct Scene::NativeRenderPacket
{
    std::vector<Engine::Rendering::GrassInteractionCapsule> grassCapsules;
    std::vector<FrameRenderItem> items;
    std::vector<uint32_t> renderSlotItemOffsets;
    std::vector<uint32_t> renderSlotItems;
    std::vector<std::shared_ptr<IGraphicsBuffer>> meshBuffers;
};

}

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

// Packets released by the render thread come back here so the next frame
// reuses their storage. The item list alone is ~3 MB on the landscape; taken
// fresh every frame it cost newly committed pages on each fill. Shared with
// the packet deleters, which may outlive the renderer.
struct SceneRenderer::NativePacketRecycler
{
    std::mutex mutex;
    std::vector<std::unique_ptr<NativeRenderPacket>> free;
};

}

#pragma once
#include "Core/Scene/Rendering/SceneRenderer/ScenePortalViewScheduling.h"
#include "Core/Model/MeshData.h"

namespace Engine::Scene::Renderer
{
    struct SpatialDebugPass
    {
        uint64_t vertexOffset = 0;
        uint32_t vertexCount = 0;
        uint32_t dataSlot = std::numeric_limits<uint32_t>::max();
        uint32_t constantBufferSlot = std::numeric_limits<uint32_t>::max();
        glm::vec4 color { 1.f };
    };
class SceneSpatialDebugGeometry
{
public:
    static std::vector<SpatialDebugPass> Build(
        const std::vector<PortalStencilPass>& portalPasses,
        const std::vector<Engine::Components::SpatialManipulator*>& manipulators,
        Engine::Model::AnimationVertex* portalVertices, uint32_t& portalVertexCursor,
        uint32_t portalVertexCapacity, bool cameraUsesSourceWarpChart);
};
}

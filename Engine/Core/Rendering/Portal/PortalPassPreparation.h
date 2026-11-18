#pragma once
#include "Core/Model/MeshData.h"
#include "Core/Rendering/Portal/PortalPasses.h"

#include <vector>

#include <glm/glm.hpp>
#include <functional>

namespace Engine::Rendering
{
class PortalPassPreparation
{
public:
    struct Result
    {
        std::vector<PortalStencilPass> passes;
        uint32_t vertexCursor = 0;
    };
    using TargetResolver = std::function<Engine::Components::SpatialManipulator*(
        Engine::Components::SpatialManipulator*)>;
    static Result Build(
        const std::vector<Engine::Components::SpatialManipulator*>& manipulators,
        Engine::Model::AnimationVertex* vertices, uint32_t vertexCapacity,
        const glm::mat4& view, const glm::mat4& projection,
        const glm::vec3& cameraPosition, bool cameraUsesSourceWarpChart,
        TargetResolver resolveTarget);
};
}

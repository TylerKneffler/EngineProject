#pragma once
#include "Core/Scene/Rendering/SceneRenderer/ScenePortalViewScheduling.h"
#include <functional>

namespace Engine::Scene::Renderer
{
class ScenePortalPassPreparation
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

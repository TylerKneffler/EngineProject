#pragma once

#include <glm/glm.hpp>

namespace Engine::Graphics { class IGraphicsContext; }
namespace Engine::Core { class Object; }
namespace Engine::Scene { class Scene; }

namespace Engine::Scene::Renderer
{
class SceneRendererParticles
{
public:
    static void RecordGpuSimulation(Scene& scene, Engine::Graphics::IGraphicsContext* context);
    static void Render(Scene& scene, Engine::Graphics::IGraphicsContext* context,
        const glm::mat4& viewProjection, const glm::mat4& view,
        const Engine::Core::Object* contentScopeRoot, const glm::vec4& clipPlane, bool portalView);
};
}

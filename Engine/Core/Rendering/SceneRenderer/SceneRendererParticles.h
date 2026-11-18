#pragma once
#include "Core/Rendering/SceneRenderer.h"

#include <glm/glm.hpp>

namespace Engine::Graphics { class IGraphicsContext; }
namespace Engine::Core { class Object; }
namespace Engine::Scene { class Scene; }

namespace Engine::Rendering
{
class SceneRendererParticles
{
public:
    using Scene = Engine::Scene::Scene;
    static void RecordGpuSimulation(SceneRenderer& renderer, Engine::Graphics::IGraphicsContext* context);
    static void Render(SceneRenderer& renderer, Engine::Graphics::IGraphicsContext* context,
        const glm::mat4& viewProjection, const glm::mat4& view,
        const Engine::Core::Object* contentScopeRoot, const glm::vec4& clipPlane, bool portalView);
};
}

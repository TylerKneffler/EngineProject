#pragma once

#include "Core/Model/UIData.h"
#include <glm/glm.hpp>
#include <vector>

namespace Engine::Scene { class Scene; }
namespace Engine::UI
{
class UILayout
{
public:
    using UITextLayout = Engine::Model::UITextLayout;

    // Resolves every enabled Canvas hierarchy and returns visible text items in
    // deterministic canvas/z/hierarchy order.
    static std::vector<UITextLayout> Resolve(Engine::Scene::Scene& scene, float viewportAspect);
};
}

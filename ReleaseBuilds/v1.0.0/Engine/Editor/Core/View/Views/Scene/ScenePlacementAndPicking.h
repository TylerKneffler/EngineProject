#pragma once

#include "Engine/Editor/UI/IEditorUi.h"
#include <glm/glm.hpp>

namespace Engine::Core { class Object; }
namespace Engine::Scene { class Scene; }

namespace Engine::Editor
{
class ScenePlacementAndPicking
{
public:
    static bool FindPrefabPlacement(const Engine::Scene::Scene& scene,
        const Engine::Core::Object* prefabPreview,
        const EditorUiVec2& mousePos,
        const EditorUiVec2& viewportSize,
        glm::vec3& placement);

    static Engine::Core::Object* PickObjectInViewport(
        const Engine::Scene::Scene& scene,
        const EditorUiVec2& mousePos,
        const EditorUiVec2& viewportSize);
};
}

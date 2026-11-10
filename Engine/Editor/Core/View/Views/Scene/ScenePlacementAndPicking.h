#pragma once

#include "Engine/Editor/UI/IEditorUi.h"
#include <glm/glm.hpp>
#include <cstdint>

namespace Engine::Core { class Object; }
namespace Engine::Scene { class Scene; }

namespace Engine::Editor
{
class ScenePlacementAndPicking
{
public:
    static void PrepareSkinnedGeometry(const Engine::Scene::Scene& scene);
    static void ClearCaches();
    // Hits only terrain meshes belonging to the selected terrain or manager.
    // No editor grid fallback: object paint must land on a real surface.
    static bool FindTerrainPlacement(const Engine::Scene::Scene& scene,
        const Engine::Core::Object* terrainRoot,
        const EditorUiVec2& mousePos, const EditorUiVec2& viewportSize,
        glm::vec3& placement, glm::vec3& normal);
    // Returns the last completed hover only while its camera, pose, and scene
    // snapshot still match. A new query is queued when any input changes.
    static Engine::Core::Object* PickObjectHoverAsync(
        const Engine::Scene::Scene& scene, const EditorUiVec2& mousePos,
        const EditorUiVec2& viewportSize);
    static bool FindPrefabPlacement(const Engine::Scene::Scene& scene,
        const Engine::Core::Object* prefabPreview,
        const EditorUiVec2& mousePos,
        const EditorUiVec2& viewportSize,
        glm::vec3& placement);

    static Engine::Core::Object* PickObjectInViewport(
        const Engine::Scene::Scene& scene,
        const EditorUiVec2& mousePos,
        const EditorUiVec2& viewportSize,
        bool preserveMeshChild = false,
        uint64_t* pickedPlacementId = nullptr);
    static Engine::Core::Object* PickBoneInViewport(
        const Engine::Scene::Scene& scene,
        const EditorUiVec2& mousePos,
        const EditorUiVec2& viewportSize);
    static Engine::Core::Object* PickSkinnedBoneInViewport(
        const Engine::Scene::Scene& scene,
        const EditorUiVec2& mousePos,
        const EditorUiVec2& viewportSize);
};
}

#pragma once

#include <cstdint>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace Engine::Core { class Object; }
namespace Engine::Components { class Mesh; }

namespace Engine::Scene
{
// Editor-only view state the scene renderer and editor camera read: selection,
// previews, mesh/rig editing overlays and 2D mode. Runtime games leave it at
// defaults. Grouped here so editor concerns have one seam on Scene; the next
// step moves ownership to the editor (see .md/architecture-refactor-plan.md).
struct EditorViewState
{
    using Object = Engine::Core::Object;
    using Mesh = Engine::Components::Mesh;

    // ---- Selection ----------------------------------------------------------
    Object* selectedObject = nullptr;
    // Terrain placement instance selected through its proxy object.
    uint64_t selectedPlacementId = 0;
    Object* selectedPlacementLayer = nullptr;
    Mesh* selectedMesh = nullptr;
    Object* previewObject = nullptr;

    // ---- Mesh and rig editing overlays --------------------------------------
    bool meshEditPose = false;
    // Viewport preference only; individual rig visibility and saved assets
    // are left intact.
    bool bonesVisible = true;
    Mesh* weightPaintMesh = nullptr;
    int weightPaintBone = -1;

    // ---- Editor camera ------------------------------------------------------
    bool mode2D = false;
    bool cameraModeInitialized = false;
    Object* cameraFollowTarget = nullptr;
    glm::vec3 cameraFollowPosition { 0.f };
    glm::quat cameraFollowRotation { 1.f, 0.f, 0.f, 0.f };

    // Selects an object. A terrain placement proxy also selects (and outlines)
    // its single instance, whichever UI path made the selection.
    void SelectObject(Object* object);
    void SelectPlacement(uint64_t id, Object* layer)
    {
        selectedPlacementId = id;
        selectedPlacementLayer = id != 0 ? layer : nullptr;
    }
};
}

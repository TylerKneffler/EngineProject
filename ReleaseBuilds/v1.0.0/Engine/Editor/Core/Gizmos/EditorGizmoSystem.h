#pragma once

#include "Engine/Editor/UI/IEditorUi.h"
#include <glm/glm.hpp>

namespace Engine::Core { class Object; }
namespace Engine::Scene { class Scene; }

namespace Engine::Editor
{
enum class EditorTransformTool
{
    Translate,
    Rotate,
    Scale,
    Hand
};

struct EditorGizmoResult
{
    Engine::Core::Object* selectedObject = nullptr;
    bool selectionRequested = false;
    bool consumedClick = false;
    bool transformDragging = false;
};

// Scene-view overlays for non-mesh objects and interactive transform tools.
// Coordinates are projected into the current viewport; drawing remains behind
// IEditorUi so Editor/Core does not depend on a specific UI package.
class EditorGizmoSystem
{
public:
    EditorGizmoResult DrawAndHandle(
        Engine::Scene::Scene& scene, IEditorUi& ui,
        const EditorUiViewportInput& input, EditorTransformTool tool);

private:
    Engine::Core::Object* m_dragObject = nullptr;
    int m_dragAxis = -1;
    glm::vec3 m_dragStartLocalPosition{};
    glm::vec3 m_dragStartLocalRotation{};
    glm::vec3 m_dragStartLocalScale{1.f};
    glm::vec3 m_dragWorldAxis{};
    EditorUiVec2 m_dragStartMouse{};
    EditorUiVec2 m_dragScreenDirection{};
    float m_dragWorldUnitsPerPixel = 0.f;
    EditorTransformTool m_dragTool = EditorTransformTool::Translate;
};
}

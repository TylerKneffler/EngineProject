#pragma once

#include "Engine/Editor/Core/Gizmos/EditorGizmoSystem.h"
#include "Engine/Editor/UI/IEditorUi.h"

namespace Engine::Scene { class Scene; }
namespace Engine::Core { class Object; }

namespace Engine::Editor
{
class SceneViewportToolbar
{
public:
    bool Draw(IEditorUi& ui, const EditorUiViewportInput& input,
        Engine::Scene::Scene* scene, bool allowObjectTransform = true);

    EditorTransformTool GetTransformTool() const
    {
        return m_transformTool;
    }
    void SetTransformTool(EditorTransformTool tool) { m_transformTool = tool; }

    bool IsTransformDragging() const
    {
        return m_cubeDragObject != nullptr;
    }

private:
    bool DrawOrientationGizmo(IEditorUi& ui,
        const EditorUiViewportInput& input,
        Engine::Scene::Scene* scene, bool allowObjectTransform);

    EditorTransformTool m_transformTool = EditorTransformTool::Translate;
    Engine::Core::Object* m_cubeDragObject = nullptr;
    int m_cubeDragAxis = -1;
    EditorTransformTool m_cubeDragTool = EditorTransformTool::Translate;
    EditorUiVec2 m_cubeDragStartMouse{};
    EditorUiVec2 m_cubeDragScreenDirection{};
    glm::vec3 m_cubeDragStartPosition{};
    glm::vec3 m_cubeDragStartRotation{};
    glm::vec3 m_cubeDragStartScale{1.f};
    float m_cubeDragWorldUnitsPerPixel = 0.f;
    EditorUiVec2 m_cubeDragLastMouse{};
    glm::vec3 m_cubeDragLastWrittenPosition{};
    glm::vec3 m_cubeDragLastWrittenRotation{};
    glm::vec3 m_cubeDragLastWrittenScale{1.f};
    bool m_cubeDragHasWritten = false;
    int m_cubeDragBlockedDirection = 0;
    int m_cubeDragLastAttemptDirection = 0;
};
}

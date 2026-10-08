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
        Engine::Scene::Scene* scene, bool allowTransformTools = true,
        bool allowObjectTransform = true);

    EditorTransformTool GetTransformTool() const
    {
        return m_transformTool;
    }
    void SetTransformTool(EditorTransformTool tool) { m_transformTool = tool; }
    void SetSceneToolsVisible(bool visible) { m_sceneToolsVisible = visible; }

    bool IsTransformDragging() const
    {
        return m_cubeDragObject != nullptr;
    }

private:
    bool DrawTransformToolbar(IEditorUi& ui,
        const EditorUiViewportInput& input);
    bool DrawGridToggle(IEditorUi& ui, const EditorUiViewportInput& input,
        Engine::Scene::Scene* scene);
    bool DrawRenderModeMenu(IEditorUi& ui,
        const EditorUiViewportInput& input,
        Engine::Scene::Scene* scene);
    bool DrawSceneUiOverlayToggle(IEditorUi& ui,
        const EditorUiViewportInput& input,
        Engine::Scene::Scene* scene);
    bool DrawOrientationGizmo(IEditorUi& ui,
        const EditorUiViewportInput& input,
        Engine::Scene::Scene* scene, bool allowObjectTransform);

    EditorTransformTool m_transformTool = EditorTransformTool::Translate;
    bool m_sceneToolsVisible = true;
    bool m_renderModeExpanded = false;
    Engine::Core::Object* m_cubeDragObject = nullptr;
    int m_cubeDragAxis = -1;
    EditorTransformTool m_cubeDragTool = EditorTransformTool::Translate;
    EditorUiVec2 m_cubeDragStartMouse{};
    EditorUiVec2 m_cubeDragScreenDirection{};
    glm::vec3 m_cubeDragStartPosition{};
    glm::vec3 m_cubeDragStartRotation{};
    glm::vec3 m_cubeDragStartScale{1.f};
    float m_cubeDragWorldUnitsPerPixel = 0.f;
};
}

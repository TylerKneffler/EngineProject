#pragma once

#include "Engine/Editor/Core/Gizmos/EditorGizmoSystem.h"
#include "Engine/Editor/UI/IEditorUi.h"

namespace Engine::Scene { class Scene; }

namespace Engine::Editor
{
class SceneViewportToolbar
{
public:
    bool Draw(IEditorUi& ui, const EditorUiViewportInput& input,
        Engine::Scene::Scene* scene);

    EditorTransformTool GetTransformTool() const
    {
        return m_transformTool;
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

    EditorTransformTool m_transformTool = EditorTransformTool::Translate;
    bool m_transformToolbarExpanded = false;
    bool m_renderModeExpanded = false;
};
}

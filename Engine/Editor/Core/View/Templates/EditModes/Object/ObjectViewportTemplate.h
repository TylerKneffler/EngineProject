#pragma once

#include "Engine/Editor/Core/View/Views/Scene/SceneViewportToolbar.h"

namespace Engine::Editor
{
struct ObjectViewportTemplate
{
    static bool Draw(IEditorUi& ui, const EditorUiViewportInput& input,
        Engine::Scene::Scene* scene, SceneViewportToolbar& toolbar,
        bool allowObjectTransform)
    {
        return toolbar.Draw(ui, input, scene, true, allowObjectTransform);
    }
};
}

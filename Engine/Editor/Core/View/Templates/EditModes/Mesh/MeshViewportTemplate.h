#pragma once

#include "Engine/Editor/Core/View/Views/Scene/SceneViewportToolbar.h"

namespace Engine::Editor
{
struct MeshViewportTemplate
{
    static bool Draw(IEditorUi& ui, const EditorUiViewportInput& input,
        Engine::Scene::Scene* scene, SceneViewportToolbar& toolbar)
    {
        // Transform handles act on mesh elements; object transforms stay off.
        return toolbar.Draw(ui, input, scene, true, false);
    }
};
}

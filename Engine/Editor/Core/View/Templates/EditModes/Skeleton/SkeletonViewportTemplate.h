#pragma once

#include "Engine/Editor/Core/View/Views/Scene/SceneViewportToolbar.h"

namespace Engine::Editor
{
struct SkeletonViewportTemplate
{
    static bool Draw(IEditorUi& ui, const EditorUiViewportInput& input,
        Engine::Scene::Scene* scene, SceneViewportToolbar& toolbar)
    {
        // The viewport only draws the camera control; transform tools are in
        // the editor header.
        return toolbar.Draw(ui, input, scene, false);
    }
};
}

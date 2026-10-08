#pragma once

#include "Engine/Editor/Core/View/Views/Scene/SceneViewportToolbar.h"

namespace Engine::Editor
{
struct SkeletonViewportTemplate
{
    static bool Draw(IEditorUi& ui, const EditorUiViewportInput& input,
        Engine::Scene::Scene* scene, SceneViewportToolbar& toolbar,
        bool editingBoneTransform)
    {
        // Weight Paint uses the brush and camera controls; bone editing also
        // exposes the transform handles.
        return toolbar.Draw(ui, input, scene, editingBoneTransform, false);
    }
};
}

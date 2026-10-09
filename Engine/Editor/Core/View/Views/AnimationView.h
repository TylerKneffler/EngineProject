#pragma once
#include "SceneView.h"

namespace Engine::Editor
{
// A dedicated 3D pose viewport. It uses SceneView's camera and bone gizmos,
// but is bound to an isolated animation preview scene by EditorState.
class AnimationView final : public SceneView
{
};
}

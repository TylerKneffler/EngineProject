#pragma once

namespace Engine::Editor
{
class IEditorUi;

enum class SceneEditMode { Object, Mesh, Skeleton };

struct ViewportModeControl
{
    bool changed = false;
    bool showSave = false;
};

ViewportModeControl DrawViewportModeControl(IEditorUi& ui, int& mode);
}

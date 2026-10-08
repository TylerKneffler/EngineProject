#include "ViewportModeTemplate.h"
#include "Engine/Editor/UI/IEditorUi.h"
#include <algorithm>

namespace Engine::Editor
{
ViewportModeControl DrawViewportModeControl(IEditorUi& ui, int& mode)
{
    const float available = ui.AvailableContentWidth();
    const bool compact = available < 125.f;
    const bool showSave = available >= 175.f;
    ui.SetNextItemWidth(std::max(48.f,
        available - (compact ? 0.f : 47.f) - (showSave ? 56.f : 0.f)));
    const char* choices[]{ "Object", "Mesh", "Skeleton" };
    const bool changed = ui.Combo(compact ? "##Mode" : "Mode",
        &mode, choices, 3);
    if (compact) ui.Tooltip("Editor mode");
    return { changed, showSave };
}
}

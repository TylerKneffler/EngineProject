#pragma once
#include "Engine/Editor/Core/View/IEditorPanel.h"

namespace Engine::Editor
{
class AnimationTimelineView final : public IEditorPanel
{
public:
    std::function<void(IEditorUi&)> OnDrawTimeline;
    void DrawPanel(IEditorUi& ui) override;
};
}

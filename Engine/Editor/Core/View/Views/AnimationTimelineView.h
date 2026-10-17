#pragma once
#include "View/IEditorPanel.h"
#include "Engine/Editor/UI/IEditorUi.h"

namespace Engine::Editor
{
class AnimationTimelineView final : public IEditorPanel
{
public:
    std::function<void(IEditorUi&)> OnDrawTimeline;

    void DrawPanel(IEditorUi& ui) override
    {
        if (ui.BeginWindow(m_title.c_str(), &m_open))
        {
            if (OnDrawTimeline) OnDrawTimeline(ui);
        }
        ui.EndWindow();
    }
};
}

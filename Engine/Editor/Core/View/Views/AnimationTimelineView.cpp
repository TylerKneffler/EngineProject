#include "AnimationTimelineView.h"
#include "Engine/Editor/UI/IEditorUi.h"

namespace Engine::Editor
{
void AnimationTimelineView::DrawPanel(IEditorUi& ui)
{
    const bool visible = ui.BeginWindow(m_title.c_str(), &m_open);
    if (ui.IsWindowFocused() && OnFocused) OnFocused();
    if (visible && OnDrawTimeline) OnDrawTimeline(ui);
    ui.EndWindow();
}
}

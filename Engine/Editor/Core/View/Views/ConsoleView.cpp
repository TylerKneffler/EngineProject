#include "ConsoleView.h"
#include "Engine/Editor/UI/IEditorUi.h"

void ConsoleView::AddLog(Level level,const std::string& message){m_entries.push_back({level,message});if(m_autoScroll)m_scrollToBottom=true;}
void ConsoleView::Clear(){m_entries.clear();}
void ConsoleView::DrawPanel(IEditorUi& ui)
{
    if(!ui.BeginWindow(m_title.c_str(),&m_open)){ui.EndWindow();return;}
    if(ui.Button("Clear"))Clear();ui.SameLine();ui.Checkbox("Auto-scroll",&m_autoScroll);ui.Separator();
    if(ui.BeginChild("##log")){for(const auto&e:m_entries)ui.Selectable(e.message.c_str());if(m_scrollToBottom&&m_autoScroll){ui.ScrollToBottom();m_scrollToBottom=false;}}ui.EndChild();ui.EndWindow();
}

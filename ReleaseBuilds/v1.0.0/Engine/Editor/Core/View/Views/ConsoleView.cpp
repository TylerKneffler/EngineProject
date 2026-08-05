#include "ConsoleView.h"
#include "Engine/Editor/UI/IEditorUi.h"

namespace
{
const char* LevelName(ConsoleView::Level level)
{
    switch(level)
    {
    case ConsoleView::Level::Warning: return "Warning";
    case ConsoleView::Level::Error: return "Error";
    case ConsoleView::Level::Build: return "Build";
    default: return "Info";
    }
}

std::string DisplayText(const ConsoleView::Entry& entry)
{
    return std::string("[") + LevelName(entry.level) + "] " + entry.message;
}
}

void ConsoleView::AddLog(Level level,const std::string& message)
{
    m_entries.push_back({level,message});
    if(!m_textBlock.empty()&&m_textBlock.back()!='\n')m_textBlock+='\n';
    m_textBlock+=DisplayText(m_entries.back());
    if(m_autoScroll)m_scrollToBottom=true;
}
void ConsoleView::Clear(){m_entries.clear();m_textBlock.clear();m_scrollToBottom=false;}
void ConsoleView::DrawPanel(IEditorUi& ui)
{
    if(!ui.BeginWindow(m_title.c_str(),&m_open)){ui.EndWindow();return;}
    if(ui.Button("Clear"))Clear();
    ui.SameLine();ui.Checkbox("Auto-scroll",&m_autoScroll);
    ui.Separator();
    const bool scrollToBottom=m_scrollToBottom&&m_autoScroll;
    ui.ReadOnlyTextBlock("##consoleText",m_textBlock.c_str(),scrollToBottom);
    if(scrollToBottom)m_scrollToBottom=false;
    ui.EndWindow();
}

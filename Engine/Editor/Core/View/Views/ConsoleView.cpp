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

std::string CopyText(const ConsoleView::Entry& entry)
{
    return std::string("[") + LevelName(entry.level) + "] " + entry.message;
}
}

void ConsoleView::AddLog(Level level,const std::string& message){m_entries.push_back({level,message});if(m_autoScroll)m_scrollToBottom=true;}
void ConsoleView::Clear(){m_entries.clear();m_selectedEntry=static_cast<size_t>(-1);m_copyStatus.clear();}
void ConsoleView::DrawPanel(IEditorUi& ui)
{
    if(!ui.BeginWindow(m_title.c_str(),&m_open)){ui.EndWindow();return;}
    if(ui.Button("Clear"))Clear();
    ui.SameLine();
    ui.BeginDisabled(m_selectedEntry>=m_entries.size());
    if(ui.Button("Copy Selected"))
    {
        ui.SetClipboardText(CopyText(m_entries[m_selectedEntry]).c_str());
        m_copyStatus="Selected entry copied";
    }
    ui.EndDisabled();
    ui.SameLine();
    ui.BeginDisabled(m_entries.empty());
    if(ui.Button("Copy All"))
    {
        std::string text;
        for(size_t i=0;i<m_entries.size();++i)
        {
            if(i) text+='\n';
            text+=CopyText(m_entries[i]);
        }
        ui.SetClipboardText(text.c_str());
        m_copyStatus=std::to_string(m_entries.size())+" entries copied";
    }
    ui.EndDisabled();
    ui.SameLine();ui.Checkbox("Auto-scroll",&m_autoScroll);
    if(!m_copyStatus.empty()){ui.SameLine();ui.DisabledLabel(m_copyStatus.c_str());}
    ui.Separator();
    if(ui.BeginChild("##log"))
    {
        for(size_t i=0;i<m_entries.size();++i)
        {
            const std::string display=std::to_string(i+1)+" | "+CopyText(m_entries[i]);
            if(ui.Selectable(display.c_str(),m_selectedEntry==i))m_selectedEntry=i;
        }
        if(m_scrollToBottom&&m_autoScroll){ui.ScrollToBottom();m_scrollToBottom=false;}
    }
    ui.EndChild();ui.EndWindow();
}

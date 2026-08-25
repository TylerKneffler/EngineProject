#include "ConsoleView.h"
#include "Engine/Editor/UI/IEditorUi.h"
#include "../Focus/WindowFocusHandler.h"
#include <algorithm>
#include <cctype>

namespace Engine::Editor
{
// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------
ConsoleView::ConsoleView()
{
    // Console is an editor UI panel with normal cursor
    SetCursorBehaviorOnFocus(CursorBehaviorOnFocus::Visible);
}

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

bool ContainsDiagnostic(const std::string& message, const char* word)
{
    std::string lower = message;
    std::transform(lower.begin(), lower.end(), lower.begin(),
        [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    const std::string token(word);
    return lower.find(token + " ") != std::string::npos ||
        lower.find(token + ":") != std::string::npos ||
        lower.find("fatal " + token) != std::string::npos;
}
}

void ConsoleView::AddLog(Level level,const std::string& message)
{
    m_entries.push_back({level,message});
    if(!m_textBlock.empty()&&m_textBlock.back()!='\n')m_textBlock+='\n';
    m_textBlock+=DisplayText(m_entries.back());
    if(m_problemStore)
    {
        if(level==Level::Error || (level==Level::Build&&ContainsDiagnostic(message,"error")))
            m_problemStore->Add(EditorProblemSeverity::Error,message);
        else if(level==Level::Warning || (level==Level::Build&&ContainsDiagnostic(message,"warning")))
            m_problemStore->Add(EditorProblemSeverity::Warning,message);
    }
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
}

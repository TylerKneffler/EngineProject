#include "ConsoleView.h"

// ---------------------------------------------------------------------------
// AddLog
// ---------------------------------------------------------------------------
void ConsoleView::AddLog(Level level, const std::string& message)
{
    m_entries.push_back({ level, message });
    if (m_autoScroll)
        m_scrollToBottom = true;
}

// ---------------------------------------------------------------------------
// Clear
// ---------------------------------------------------------------------------
void ConsoleView::Clear()
{
    m_entries.clear();
}

// ---------------------------------------------------------------------------
// DrawPanel
// ---------------------------------------------------------------------------
void ConsoleView::DrawPanel()
{
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4.f, 4.f));

    if (!ImGui::Begin(m_title.c_str(), &m_open))
    {
        ImGui::End();
        ImGui::PopStyleVar();
        return;
    }

    // ---- Toolbar ----
    if (ImGui::Button("Clear"))
        Clear();
    ImGui::SameLine();
    ImGui::Checkbox("Auto-scroll", &m_autoScroll);
    ImGui::Separator();

    // ---- Log area ----
    ImGui::BeginChild("##log", ImVec2(0.f, 0.f), false,
                       ImGuiWindowFlags_HorizontalScrollbar);

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.f, 1.f));

    for (const auto& entry : m_entries)
    {
        ImVec4 color;
        switch (entry.level)
        {
            case Level::Warning: color = { 1.f,  0.85f, 0.f,  1.f }; break;
            case Level::Error:   color = { 1.f,  0.35f, 0.35f,1.f }; break;
            case Level::Build:   color = { 0.7f, 0.9f,  1.f,  1.f }; break;
            default:             color = { 1.f,  1.f,   1.f,  1.f }; break;
        }
        ImGui::PushStyleColor(ImGuiCol_Text, color);
        // Use Selectable to enable clicking and copying
        ImGui::Selectable(entry.message.c_str(), false);
        if (ImGui::IsItemHovered())
        {
            // Allow copying on right-click
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Right))
            {
                ImGui::SetClipboardText(entry.message.c_str());
            }
        }
        ImGui::PopStyleColor();
    }

    ImGui::PopStyleVar();

    if (m_scrollToBottom && m_autoScroll)
    {
        ImGui::SetScrollHereY(1.f);
        m_scrollToBottom = false;
    }

    ImGui::EndChild();
    ImGui::End();
    ImGui::PopStyleVar();
}

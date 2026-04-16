#pragma once
#include "pch.h"
#include "View/IEditorPanel.h"

// ---------------------------------------------------------------------------
// ConsoleView — editor Console panel
//
// Displays log messages from builds, the engine, and scripts.
// Messages can be added from anywhere via AddLog().
//
// Usage:
//   consoleView.AddLog(ConsoleView::Level::Build, "cmake output...");
//   consoleView.DrawPanel();
// ---------------------------------------------------------------------------
class ConsoleView : public IEditorPanel
{
public:
    enum class Level { Info, Warning, Error, Build };

    ConsoleView()  = default;
    ~ConsoleView() = default;

    // Appends a message at the given severity level.
    void AddLog(Level level, const std::string& message);
    void AddLog(const std::string& message) { AddLog(Level::Info, message); }

    // Clears all log entries.
    void Clear();

    // Draws the "Console" ImGui panel.
    void DrawPanel();

private:
    struct Entry
    {
        Level       level;
        std::string message;
    };

    std::vector<Entry> m_entries;
    bool m_autoScroll     = true;
    bool m_scrollToBottom = false;
};

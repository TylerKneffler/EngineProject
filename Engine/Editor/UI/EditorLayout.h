#pragma once

#include <chrono>
#include <string>
#include <unordered_map>

// Package-neutral editor layout persisted beside the project. Both ImGui and
// Nuklear update this model so switching UI packages preserves the workspace.
class EditorLayout
{
public:
    void Initialize(const std::string& projectFilePath);
    void Update();
    void Flush();

    float LeftWidthRatio() const { return m_leftWidthRatio; }
    float RightWidthRatio() const { return m_rightWidthRatio; }
    float ConsoleHeightRatio() const { return m_consoleHeightRatio; }
    const std::string& ActiveLeftTab() const { return m_activeLeftTab; }
    const std::string& ActiveCenterTab() const { return m_activeCenterTab; }

    void SetDockRatios(float left, float right, float consoleHeight);
    void SetActiveLeftTab(const std::string& title);
    void SetActiveCenterTab(const std::string& title);
    bool IsPanelOpen(const std::string& title, bool defaultValue = true) const;
    void SetPanelOpen(const std::string& title, bool open);

private:
    void MarkDirty();
    void Load();
    void Save();

    std::string m_filePath;
    float m_leftWidthRatio = 0.20f;
    float m_rightWidthRatio = 0.25f;
    float m_consoleHeightRatio = 0.24f;
    std::string m_activeLeftTab = "Hierarchy 1";
    std::string m_activeCenterTab = "Scene 1";
    std::unordered_map<std::string, bool> m_panelOpen;
    bool m_dirty = false;
    std::chrono::steady_clock::time_point m_lastChange{};
};

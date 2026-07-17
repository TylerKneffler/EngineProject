#include "Engine/Editor/UI/EditorLayout.h"
#include "Core/Serialization/Json.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

void EditorLayout::Initialize(const std::string& projectFilePath)
{
    const fs::path root = projectFilePath.empty()
        ? fs::current_path()
        : fs::path(projectFilePath).parent_path();
    m_filePath = (root / ".editor" / "EditorLayout.json").string();
    Load();
}

void EditorLayout::Load()
{
    if (m_filePath.empty() || !fs::is_regular_file(m_filePath)) return;
    try
    {
        const JsonValue root = JsonParseFile(m_filePath);
        if (root.Has("leftWidthRatio")) m_leftWidthRatio = root["leftWidthRatio"].AsFloat();
        if (root.Has("rightWidthRatio")) m_rightWidthRatio = root["rightWidthRatio"].AsFloat();
        if (root.Has("consoleHeightRatio")) m_consoleHeightRatio = root["consoleHeightRatio"].AsFloat();
        if (root.Has("activeLeftTab")) m_activeLeftTab = root["activeLeftTab"].AsString();
        if (root.Has("activeCenterTab")) m_activeCenterTab = root["activeCenterTab"].AsString();
        if (root.Has("panels") && root["panels"].IsObject())
        {
            const JsonValue& panels = root["panels"];
            for (std::size_t i = 0; i < panels.ObjectSize(); ++i)
                m_panelOpen[panels.ObjectKey(i)] = panels.ObjectValue(i).AsBool();
        }
        m_leftWidthRatio = std::clamp(m_leftWidthRatio, 0.10f, 0.45f);
        m_rightWidthRatio = std::clamp(m_rightWidthRatio, 0.10f, 0.45f);
        m_consoleHeightRatio = std::clamp(m_consoleHeightRatio, 0.10f, 0.50f);
    }
    catch (...) { /* Invalid user layout falls back to defaults. */ }
}

void EditorLayout::MarkDirty()
{
    m_dirty = true;
    m_lastChange = std::chrono::steady_clock::now();
}

void EditorLayout::Update()
{
    if (m_dirty && std::chrono::steady_clock::now() - m_lastChange > std::chrono::milliseconds(500))
        Save();
}

void EditorLayout::Flush()
{
    if (m_dirty) Save();
}

void EditorLayout::Save()
{
    if (m_filePath.empty()) return;
    try
    {
        JsonValue root = JsonValue::MakeObject();
        root.Set("version", JsonValue(1));
        root.Set("leftWidthRatio", JsonValue(m_leftWidthRatio));
        root.Set("rightWidthRatio", JsonValue(m_rightWidthRatio));
        root.Set("consoleHeightRatio", JsonValue(m_consoleHeightRatio));
        root.Set("activeLeftTab", JsonValue(m_activeLeftTab));
        root.Set("activeCenterTab", JsonValue(m_activeCenterTab));
        JsonValue panels = JsonValue::MakeObject();
        for (const auto& [title, open] : m_panelOpen)
            panels.Set(title, JsonValue(open));
        root.Set("panels", std::move(panels));

        fs::create_directories(fs::path(m_filePath).parent_path());
        std::ofstream output(m_filePath, std::ios::trunc);
        if (!output) return;
        output << JsonWrite(root) << '\n';
        m_dirty = false;
    }
    catch (...) { /* Layout persistence must never take down the editor. */ }
}

void EditorLayout::SetDockRatios(float left, float right, float consoleHeight)
{
    left = std::clamp(left, 0.10f, 0.45f);
    right = std::clamp(right, 0.10f, 0.45f);
    consoleHeight = std::clamp(consoleHeight, 0.10f, 0.50f);
    if (std::abs(left - m_leftWidthRatio) < 0.001f &&
        std::abs(right - m_rightWidthRatio) < 0.001f &&
        std::abs(consoleHeight - m_consoleHeightRatio) < 0.001f) return;
    m_leftWidthRatio = left;
    m_rightWidthRatio = right;
    m_consoleHeightRatio = consoleHeight;
    MarkDirty();
}

void EditorLayout::SetActiveLeftTab(const std::string& title)
{
    if (title == m_activeLeftTab) return;
    m_activeLeftTab = title;
    MarkDirty();
}

void EditorLayout::SetActiveCenterTab(const std::string& title)
{
    if (title == m_activeCenterTab) return;
    m_activeCenterTab = title;
    MarkDirty();
}

bool EditorLayout::IsPanelOpen(const std::string& title, bool defaultValue) const
{
    const auto found = m_panelOpen.find(title);
    return found == m_panelOpen.end() ? defaultValue : found->second;
}

void EditorLayout::SetPanelOpen(const std::string& title, bool open)
{
    const auto found = m_panelOpen.find(title);
    if (found != m_panelOpen.end() && found->second == open) return;
    m_panelOpen[title] = open;
    MarkDirty();
}

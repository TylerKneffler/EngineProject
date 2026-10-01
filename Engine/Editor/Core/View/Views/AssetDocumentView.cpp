#include "AssetDocumentView.h"
#include "Engine/Editor/UI/IEditorUi.h"
#include "Core/Scene/Scene.h"
#include <filesystem>
#include <fstream>
#include <iterator>
#include <algorithm>
#include <cctype>

namespace Engine::Editor
{
AssetDocumentView::AssetDocumentView(const std::string& path,
    const std::string& editorType, Engine::Scene::Scene* previewScene)
    : m_path([&path]()
        {
            std::error_code error;
            const std::filesystem::path canonical =
                std::filesystem::weakly_canonical(path, error);
            return (error ? std::filesystem::absolute(path).lexically_normal()
                : canonical).string();
        }())
    , m_identity(m_path)
    , m_editorType(editorType)
    , m_previewScene(previewScene)
{
#ifdef _WIN32
    std::transform(m_identity.begin(), m_identity.end(), m_identity.begin(),
        [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
#endif
    m_inspector.Select(m_path);
    m_knownContents = ReadContents();
    SetDefaultDockArea(EditorPanelDockArea::MainDocument);
    RefreshTitle();
    m_inspector.OnContentsChanged = [this](const std::string&)
    {
        m_assetWriteObserved = true;
    };
    m_inspector.OnRenamed = [this](const std::string& oldPath,
        const std::string& newPath)
    {
        std::error_code error;
        const std::filesystem::path canonical =
            std::filesystem::weakly_canonical(newPath, error);
        m_path = (error ? std::filesystem::absolute(newPath).lexically_normal()
            : canonical).string();
        m_identity = m_path;
#ifdef _WIN32
        std::transform(m_identity.begin(), m_identity.end(), m_identity.begin(),
            [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
#endif
        m_undo.clear();
        m_redo.clear();
        m_knownContents = ReadContents();
        RefreshTitle();
        if (OnRenamed)
            OnRenamed(oldPath, m_path);
    };
}

void AssetDocumentView::RefreshTitle()
{
    const std::string filename = std::filesystem::path(m_path).filename().string();
    // ImGui's ### keeps a path-specific ID while displaying only the filename.
    SetTitle(filename + (m_dirty ? " *" : "") + "###AssetDocument:" + m_identity);
}

std::string AssetDocumentView::ReadContents() const
{
    std::error_code error;
    const uintmax_t fileSize = std::filesystem::file_size(m_path, error);
    if (error || fileSize > SnapshotSizeLimit)
        return {};
    std::ifstream file(m_path, std::ios::binary);
    if (!file)
        return {};
    return std::string(std::istreambuf_iterator<char>(file),
        std::istreambuf_iterator<char>());
}

void AssetDocumentView::DrawPanel(IEditorUi& ui)
{
    if (!ui.BeginWindow(m_title.c_str(), &m_open))
    {
        ui.EndWindow();
        return;
    }
    ui.WindowTitleTooltip(m_path.c_str());
    if (ui.IsWindowFocused() && OnFocused)
        OnFocused();

    ui.ColoredLabel(m_editorType.c_str(), { 0.35f, 0.7f, 1.f, 1.f });
    ui.ValueLabel("File", std::filesystem::path(m_path).filename().string().c_str());
    if (ui.IsItemHovered())
        ui.Tooltip(m_path.c_str());
    m_inspector.Draw(ui, m_previewScene);
    if (m_assetWriteObserved)
    {
        m_assetWriteObserved = false;
        const std::string current = ReadContents();
        if (current != m_knownContents)
        {
            if (m_undo.size() == HistoryLimit)
                m_undo.pop_front();
            m_undo.push_back(m_knownContents);
            m_redo.clear();
            m_knownContents = current;
            m_dirty = false; // Inspector-backed asset editors save on edit.
            RefreshTitle();
            if (OnChanged)
                OnChanged();
        }
    }
    ui.EndWindow();
}

bool AssetDocumentView::RestoreSnapshot(const std::string& contents)
{
    std::ofstream file(m_path, std::ios::binary | std::ios::trunc);
    if (!file)
        return false;
    file.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    if (!file)
        return false;
    m_inspector.Select(m_path);
    m_knownContents = contents;
    m_dirty = false;
    RefreshTitle();
    if (OnChanged)
        OnChanged();
    return true;
}

bool AssetDocumentView::Undo()
{
    if (m_undo.empty())
        return false;
    const std::string current = ReadContents();
    const std::string target = std::move(m_undo.back());
    m_undo.pop_back();
    m_redo.push_back(current);
    if (!RestoreSnapshot(target))
    {
        m_undo.push_back(target);
        m_redo.pop_back();
        return false;
    }
    return true;
}

bool AssetDocumentView::Redo()
{
    if (m_redo.empty())
        return false;
    const std::string current = ReadContents();
    const std::string target = std::move(m_redo.back());
    m_redo.pop_back();
    if (m_undo.size() == HistoryLimit)
        m_undo.pop_front();
    m_undo.push_back(current);
    if (!RestoreSnapshot(target))
    {
        m_redo.push_back(target);
        m_undo.pop_back();
        return false;
    }
    return true;
}

bool AssetDocumentView::Save()
{
    const std::string current = ReadContents();
    if (current.empty() && !std::filesystem::exists(m_path))
        return false;
    m_knownContents = current;
    m_dirty = false;
    RefreshTitle();
    return true;
}
}

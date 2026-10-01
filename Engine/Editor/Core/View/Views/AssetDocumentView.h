#pragma once

#include "View/IEditorPanel.h"
#include "Engine/Editor/Core/View/Templates/Assets/AssetInspectorTemplate.h"
#include <deque>

namespace Engine::Scene { class Scene; }

namespace Engine::Editor
{
// A file-backed editor document. The canonical path is its stable identity;
// the visible title is the filename, including its extension.
class AssetDocumentView final : public IEditorPanel
{
public:
    AssetDocumentView(const std::string& path, const std::string& editorType,
        Engine::Scene::Scene* previewScene);

    void DrawPanel(IEditorUi& ui) override;
    const std::string& GetPath() const { return m_path; }
    const std::string& GetEditorType() const { return m_editorType; }
    bool CanUndo() const { return !m_undo.empty(); }
    bool CanRedo() const { return !m_redo.empty(); }
    bool IsDirty() const { return m_dirty; }
    bool Undo();
    bool Redo();
    bool Save();

    std::function<void()> OnChanged;
    std::function<void(const std::string&, const std::string&)> OnRenamed;

private:
    bool RestoreSnapshot(const std::string& contents);
    std::string ReadContents() const;
    void RefreshTitle();

    std::string m_path;
    std::string m_identity;
    std::string m_editorType;
    Engine::Scene::Scene* m_previewScene = nullptr;
    AssetInspectorTemplate m_inspector;
    std::deque<std::string> m_undo;
    std::deque<std::string> m_redo;
    std::string m_knownContents;
    bool m_dirty = false;
    bool m_assetWriteObserved = false;
    static constexpr size_t HistoryLimit = 100;
    static constexpr size_t SnapshotSizeLimit = 64ull * 1024ull * 1024ull;
};
}

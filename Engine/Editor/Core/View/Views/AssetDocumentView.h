#pragma once

#include "View/IEditorPanel.h"
#include "Engine/Editor/Core/View/Templates/Assets/AssetInspectorTemplate.h"
#include "Engine/Editor/Core/Paint/BrushPreset.h"
#include "Core/Graphics/IGraphicsTexture.h"
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
    bool HasPendingRename() const { return m_inspector.HasPendingRename(); }
    bool Undo();
    bool Redo();
    bool Save();
    void HandleExternalRename(const std::string& newPath);
    bool CanPaintImage() const;
    void DrawPaintToolbar(IEditorUi& ui, const std::string& assetsDirectory,
        bool singleRow, bool* overflow);

    std::function<void()> OnChanged;
    std::function<void(const std::string&, const std::string&)> OnRenamed;

private:
    bool RestoreSnapshot(const std::string& contents);
    std::string ReadContents() const;
    void RefreshTitle();
    void ApplyRenamedPath(const std::string& newPath);
    bool LoadImageCanvas();
    void DrawImageCanvas(IEditorUi& ui);
    bool CommitImageStroke();

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
    Paint::BrushPreset m_imageBrush;
    std::string m_imageBrushPresetPath;
    std::vector<uint8_t> m_imagePixels;
    std::shared_ptr<::Engine::Graphics::IGraphicsTexture> m_imageTexture;
    uint32_t m_imageWidth = 0;
    uint32_t m_imageHeight = 0;
    int m_imageTool = 1;
    float m_imageColor[4]{ 1.f, 1.f, 1.f, 1.f };
    float m_imageBucketTolerance = .05f;
    bool m_imageMirrorX = false;
    bool m_imageMirrorY = false;
    bool m_imageAlphaLock = false;
    bool m_imageStrokeActive = false;
    bool m_imageStrokeChanged = false;
    bool m_imageTextureDirty = false;
    float m_lastImageX = -1.f;
    float m_lastImageY = -1.f;
    uint8_t m_smearColor[4]{ 255, 255, 255, 255 };
    std::vector<uint8_t> m_tipPixels;
    std::string m_loadedTipPath;
    uint32_t m_tipWidth = 0;
    uint32_t m_tipHeight = 0;
    bool m_tipUseLuminance = false;
    std::string m_imageError;
    static constexpr size_t HistoryLimit = 100;
    static constexpr size_t SnapshotSizeLimit = 64ull * 1024ull * 1024ull;
};
}

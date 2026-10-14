#pragma once

#include <functional>
#include <memory>
#include <string>
#include "AssetPreviewCache.h"
#include "Core/Model/AnimationClipAsset.h"
#include <optional>

namespace Engine::Scene { class Scene; }
namespace Engine::Components { class Texture; }

namespace Engine::Editor
{
class IEditorUi;
// Reusable Properties-panel section for filesystem assets. It owns selection
// edit state and dispatches type-specific editors without coupling them to the
// scene-object inspector.
class AssetInspectorTemplate
{
public:
    AssetInspectorTemplate() = default;
    ~AssetInspectorTemplate();

    void Select(const std::string& path);
    void Clear();
    bool HasSelection() const { return !m_selectedPath.empty(); }
    const std::string& GetSelectedPath() const { return m_selectedPath; }
    bool HasPendingRename() const;
    bool CommitPendingRename();
    void Draw(IEditorUi& ui, Engine::Scene::Scene* scene);

    std::function<void(const std::string&, const std::string&)> OnRenamed;
    std::function<void(const std::string&)> OnContentsChanged;
    std::function<void(const std::string&)> OnOpenAnimation;

private:
    std::string m_selectedPath;
    std::string m_renameError;
    char m_nameEdit[512]{};
    std::shared_ptr<::Engine::Components::Texture> m_texturePreview;
    std::optional<Engine::Model::AnimationClipAsset> m_animationPreview;
    AssetPreviewCache m_previewCache;
};
}

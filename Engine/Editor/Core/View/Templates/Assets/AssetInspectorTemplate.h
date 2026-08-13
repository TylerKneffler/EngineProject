#pragma once

#include <functional>
#include <memory>
#include <string>

namespace Engine::Scene { class Scene; }

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
    void Draw(IEditorUi& ui, Engine::Scene::Scene* scene);

    std::function<void(const std::string&, const std::string&)> OnRenamed;
    std::function<void(const std::string&)> OnContentsChanged;

private:
    std::string m_selectedPath;
    std::string m_renameError;
    char m_nameEdit[512]{};
    std::shared_ptr<::Engine::Components::Texture> m_texturePreview;
};
}

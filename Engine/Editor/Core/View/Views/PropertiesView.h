#pragma once
#include "pch.h"
#include "Core/Object.h"
#include "View/IEditorPanel.h"
#include "Engine/Editor/Core/View/Templates/Assets/AssetInspectorTemplate.h"
#include <chrono>
#include <functional>

namespace Engine::Editor
{
struct EditorUiPrefabMenuResult;

// ---------------------------------------------------------------------------
// PropertiesView
//
// Defines the Properties panel showing the editable properties of the
// currently selected Engine::Core::Object and all its components.
//
// Usage:
//   propertiesView.SetSelectedObject(hierarchy.GetSelectedObject());
//   propertiesView.DrawPanel();
// ---------------------------------------------------------------------------
class PropertiesView : public IEditorPanel
{
public:
    PropertiesView();
    ~PropertiesView() = default;

    void Init(Engine::Scene::Scene* scene)
    { m_scene = scene; OnDrawContext = {}; }
    void SetAllowComponentStructureEdits(bool allow)
    { m_allowComponentStructureEdits = allow; }

    void SetSelectedObject(Engine::Core::Object* obj)
    {
        SetSelectedObjects(obj
            ? std::vector<Engine::Core::Object*>{ obj }
            : std::vector<Engine::Core::Object*>{});
    }
    void SetSelectedObjects(const std::vector<Engine::Core::Object*>& objects)
    {
        Engine::Core::Object* obj = objects.empty() ? nullptr : objects.back();
        if (obj != m_selectedObject)
        {
            Engine::Core::Component::ClearEditorReferenceSearches();
            if (m_deferredTransformPrefabRoot)
                m_deferredTransformPrefabRoot->PrefabOverrideCacheValid = false;
            m_deferredTransformPrefabRoot = nullptr;
            m_componentPickerOpen = false;
            m_componentSearch[0] = '\0';
            m_editingSkyboxTexture = false;
        }
        m_selectedObjects = objects;
        m_selectedObject = obj;
        m_assetInspector.Clear();
    }
    Engine::Core::Object* GetSelectedObject() const   { return m_selectedObject; }
    void SetSelectedAsset(const std::string& path);
    const std::string& GetSelectedAsset() const
    {
        return m_assetInspector.GetSelectedPath();
    }

    void DrawPanel(IEditorUi& ui) override;

    std::function<void()> OnComponentsChanged;
    std::function<void(const std::string&, bool)> OnAssetDropLog;
    std::function<void(const std::string&, const std::string&)> OnAssetRenamed;
    std::function<void(const std::string&)> OnAssetContentsChanged;
    std::function<void(const std::string&)> OnPrefabRequested;
    // Document-specific controls shown above the selected object's fields.
    // Return true to use the entire panel for the document controls.
    std::function<bool(IEditorUi&)> OnDrawContext;

private:
    void DrawTransform(IEditorUi& ui);
    void DrawMultiSelection(IEditorUi& ui);
    std::string HandleWindowAssetDrop(IEditorUi& ui);
    bool AddComponentFromAsset(const std::string& path, std::string& message);
    bool AddRegisteredComponent(const std::string& typeName, std::string& message);
    void DrawComponentPicker(IEditorUi& ui);
    bool CanEditSelectedObject() const;
    void UnpackSelectedPrefab();
    void ApplySelectedPrefabOverrides(bool includeRootTransform);
    void RevertSelectedPrefabOverrides();
    void HandlePrefabMenu(const EditorUiPrefabMenuResult& menu);
    void LogAssetDrop(const std::string& message, bool error = false) const;

    Engine::Core::Object* m_selectedObject = nullptr;
    std::vector<Engine::Core::Object*> m_selectedObjects;
    Engine::Core::Object* m_deferredTransformPrefabRoot = nullptr;
    AssetInspectorTemplate m_assetInspector;
    Engine::Scene::Scene* m_scene = nullptr;
    bool m_componentPickerOpen = false;
    bool m_positionComponentPicker = false;
    char m_componentSearch[128]{};
    bool m_editingSkyboxTexture = false;
    bool m_allowComponentStructureEdits = true;
    char m_skyboxTextureEdit[512]{};
    bool m_skyboxRevealPending = false;
    std::chrono::steady_clock::time_point m_skyboxRevealRequestedAt{};
    std::string m_skyboxRevealPath;
};
}

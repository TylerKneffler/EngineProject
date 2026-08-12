#pragma once
#include "pch.h"
#include "Core/Object.h"
#include "View/IEditorPanel.h"
#include "Engine/Editor/Core/View/Templates/Assets/AssetInspectorTemplate.h"
#include <chrono>
#include <functional>

class Scene;

// ---------------------------------------------------------------------------
// PropertiesView
//
// Defines the Properties panel showing the editable properties of the
// currently selected Object and all its components.
//
// Usage:
//   propertiesView.SetSelectedObject(hierarchy.GetSelectedObject());
//   propertiesView.DrawPanel();
// ---------------------------------------------------------------------------
class PropertiesView : public IEditorPanel
{
public:
    PropertiesView()  = default;
    ~PropertiesView() = default;

    void Init(Scene* scene) { m_scene = scene; }

    void SetSelectedObject(Object* obj)
    {
        if (obj != m_selectedObject)
        {
            m_componentPickerOpen = false;
            m_componentSearch[0] = '\0';
            m_editingSkyboxTexture = false;
        }
        m_selectedObject = obj;
        m_assetInspector.Clear();
    }
    Object* GetSelectedObject() const   { return m_selectedObject; }
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

private:
    void DrawTransform(IEditorUi& ui);
    std::string HandleWindowAssetDrop(IEditorUi& ui);
    bool AddComponentFromAsset(const std::string& path, std::string& message);
    bool AddRegisteredComponent(const std::string& typeName, std::string& message);
    void DrawComponentPicker(IEditorUi& ui);
    bool CanEditSelectedObject() const;
    void UnpackSelectedPrefab();
    void LogAssetDrop(const std::string& message, bool error = false) const;

    Object* m_selectedObject = nullptr;
    Editor::ViewTemplates::AssetInspectorTemplate m_assetInspector;
    Scene* m_scene = nullptr;
    bool m_componentPickerOpen = false;
    bool m_positionComponentPicker = false;
    char m_componentSearch[128]{};
    bool m_editingSkyboxTexture = false;
    char m_skyboxTextureEdit[512]{};
    bool m_skyboxRevealPending = false;
    std::chrono::steady_clock::time_point m_skyboxRevealRequestedAt{};
    std::string m_skyboxRevealPath;
};

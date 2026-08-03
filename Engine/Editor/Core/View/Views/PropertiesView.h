#pragma once
#include "pch.h"
#include "Core/Object.h"
#include "View/IEditorPanel.h"
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
        }
        m_selectedObject = obj;
    }
    Object* GetSelectedObject() const   { return m_selectedObject; }

    void DrawPanel(IEditorUi& ui) override;

    std::function<void()> OnComponentsChanged;
    std::function<void(const std::string&, bool)> OnAssetDropLog;

private:
    void DrawTransform(IEditorUi& ui);
    void DrawAssetDropTarget(IEditorUi& ui, const char* label);
    void AcceptAssetDrop(IEditorUi& ui);
    bool AddComponentFromAsset(const std::string& path, std::string& message);
    bool AddRegisteredComponent(const std::string& typeName, std::string& message);
    void DrawComponentPicker(IEditorUi& ui);
    void LogAssetDrop(const std::string& message, bool error = false) const;

    Object* m_selectedObject = nullptr;
    Scene* m_scene = nullptr;
    bool m_componentPickerOpen = false;
    bool m_positionComponentPicker = false;
    char m_componentSearch[128]{};
};

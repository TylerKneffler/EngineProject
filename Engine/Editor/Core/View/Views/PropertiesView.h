#pragma once
#include "pch.h"
#include "Core/Object.h"
#include "View/IEditorPanel.h"

// ---------------------------------------------------------------------------
// PropertiesView
//
// Draws the "Properties" ImGui panel showing the editable properties of the
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

    void SetSelectedObject(Object* obj) { m_selectedObject = obj; }
    Object* GetSelectedObject() const   { return m_selectedObject; }

    void DrawPanel();

private:
    void DrawTransform();
    void DrawMesh();
    void DrawMaterial();
    void DrawCamera();

    Object* m_selectedObject = nullptr;
};

#pragma once
#include "Core/Serialization/Json.h"
#include "Core/PropertyMacros.h"
#include <string>

class Object; // forward declaration — full definition in Object.h
class IEditorUi; // forward declaration — full definition in Editor/UI/IEditorUi.h

class Component
{
public:
    Component() = default;
    virtual ~Component() = default;

    Object* Owner = nullptr;     // The Object this component is attached to
    bool singlecomponent = false; // If true, only one component of this type can be added

    // ---- Serialization interface -------------------------------------------
    // Each concrete component should override all three methods so it can be
    // saved to and loaded from .scene files.
    virtual std::string GetTypeName() const { return ""; }
    virtual JsonValue   Serialize()   const { return JsonValue::MakeObject(); }
    virtual void        Deserialize(const JsonValue&) {}

    // ---- Editor UI interface -----------------------------------------------
    // Called by the Properties panel to draw editable properties in the editor.
    //
    // Default implementation automatically creates interactive UI controls for
    // all properties in Serialize(). Properties marked with PROPERTY(Inspector)
    // in headers should be included in Serialize() to appear here.
    //
    // Special handling for file paths:
    //   - Properties named "file", "path", or "texture" get asset selection UI
    //   - Shows "Browse..." button to open file picker dialog
    //   - Displays image preview for texture files (when graphics context available)
    //   - Click to edit, Apply/Cancel to confirm changes
    //
    // Override this method only when you need specialized UI controls beyond
    // the automatic float sliders, color pickers, vec3 controls, and checkboxes.
    virtual void DrawProperties(IEditorUi& ui);
};
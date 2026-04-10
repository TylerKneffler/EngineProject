#pragma once
#include "Core/Serialization/Json.h"
#include <string>

class Object; // forward declaration — full definition in Object.h

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
};
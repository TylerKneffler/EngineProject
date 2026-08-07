#pragma once
#include "Core/Serialization/Json.h"
#include "Core/PropertyMacros.h"
#include <glm/glm.hpp>
#include <functional>
#include <vector>
#include <string>

class Object; // forward declaration — full definition in Object.h
class IEditorUi; // forward declaration — full definition in Editor/UI/IEditorUi.h

#define COMPONENT_TYPE_NAME(ClassName) #ClassName

class IGraphicsProvider;

class Component
{
public:
    Component() = default;
    virtual ~Component() = default;

    Object* Owner = nullptr;     // The Object this component is attached to
    bool singlecomponent = false; // If true, only one component of this type can be added

    // ---- Serialization interface -------------------------------------------
    // The base component handles the shared JSON envelope:
    //   { "type": "ComponentType", ...fields... }
    // Derived components register fields in their constructor and usually do
    // not override Serialize()/Deserialize().
    virtual std::string GetTypeName() const { return m_typeName; }
    virtual JsonValue   Serialize() const;
    JsonValue           SerializeFields() const;
    virtual void        Deserialize(const JsonValue& v);
    // Lets components rebuild runtime resources without serializer type checks.
    virtual void        OnAfterDeserialize(IGraphicsProvider*) {}
    // Called by the Properties panel to draw editable properties in the editor.
    // Returns true when an editor control changed serialized component data.
    virtual bool DrawProperties(IEditorUi& ui);

protected:
    void SetTypeName(const char* typeName) { m_typeName = typeName ? typeName : "Component"; }

    template<typename T>
    void RegisterField(const char* name, T& member)
    {
        m_serializedFields.push_back({
            name,
            [&member]() -> JsonValue { return ToJson(member); },
            [&member](const JsonValue& value) { FromJson(value, member); }
        });
    }

private:
    struct SerializedField
    {
        std::string name;
        std::function<JsonValue()> write;
        std::function<void(const JsonValue&)> read;
    };

    static JsonValue ToJson(const std::string& value) { return JsonValue(value); }
    static JsonValue ToJson(const char* value) { return JsonValue(std::string(value ? value : "")); }
    static JsonValue ToJson(bool value) { return JsonValue(value); }
    static JsonValue ToJson(float value) { return JsonValue(value); }
    static JsonValue ToJson(double value) { return JsonValue(static_cast<float>(value)); }
    static JsonValue ToJson(int value) { return JsonValue(value); }
    static JsonValue ToJson(unsigned value) { return JsonValue(static_cast<int>(value)); }
    static JsonValue ToJson(const glm::vec3& value)
    {
        return JsonValue::MakeArray()
            .Push(JsonValue(value.x))
            .Push(JsonValue(value.y))
            .Push(JsonValue(value.z));
    }

    static void FromJson(const JsonValue& value, std::string& out) { out = value.AsString(); }
    static void FromJson(const JsonValue& value, bool& out) { out = value.AsBool(); }
    static void FromJson(const JsonValue& value, float& out) { out = value.AsFloat(); }
    static void FromJson(const JsonValue& value, double& out) { out = value.AsFloat(); }
    static void FromJson(const JsonValue& value, int& out) { out = value.AsInt(); }
    static void FromJson(const JsonValue& value, unsigned& out) { out = static_cast<unsigned>(value.AsInt()); }
    static void FromJson(const JsonValue& value, glm::vec3& out)
    {
        if (!value.IsArray() || value.ArraySize() < 3)
            return;
        out = { value.ArrayAt(0).AsFloat(), value.ArrayAt(1).AsFloat(), value.ArrayAt(2).AsFloat() };
    }

    std::vector<SerializedField> m_serializedFields;
    std::string m_typeName = "Component";
};

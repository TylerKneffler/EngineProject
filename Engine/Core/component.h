#pragma once
#include "Core/Serialization/Json.h"
#include "Core/PropertyMacros.h"
#include "Core/ComponentReference.h"
#include <glm/glm.hpp>
#include <functional>
#include <cstdint>
#include <vector>
#include <string>

#define COMPONENT_TYPE_NAME(ClassName) #ClassName

namespace Engine::Graphics { class IGraphicsProvider; }
namespace Engine::Editor { class IEditorUi; }

namespace Engine::Core
{
class Object;

class Component
{
public:
    using JsonValue = Engine::Serialization::JsonValue;
    using ComponentReference = Engine::Core::ComponentReference;
    using Object = Engine::Core::Object;
    using IGraphicsProvider = Engine::Graphics::IGraphicsProvider;

    Component() = default;
    virtual ~Component() = default;

    Object* Owner = nullptr;     // The Object this component is attached to
    bool singlecomponent = false; // If true, only one component of this type can be added
    bool editorAddable = true;   // Internal/import components stay out of Add Component.

    // ---- Serialization interface -------------------------------------------
    // The base component handles the shared JSON envelope:
    //   { "type": "ComponentType", ...fields... }
    // Derived components register fields in their constructor and usually do
    // not override Serialize()/Deserialize().
    virtual std::string GetTypeName() const { return m_typeName; }
    virtual Engine::Serialization::JsonValue Serialize() const;
    Engine::Serialization::JsonValue SerializeFields() const;
    virtual void Deserialize(const Engine::Serialization::JsonValue& v);
    // Lets components rebuild runtime resources without serializer type checks.
    virtual void        OnAfterDeserialize(Engine::Graphics::IGraphicsProvider*) {}
    // Engine components and game scripts share lifecycle dispatch. Script is
    // a semantic marker for user-authored/hot-reloadable behavior, not the
    // mechanism that makes a component update.
    virtual void Start() {}
    virtual void Update() {}
    virtual void Enabled() {}
    virtual void Disabled() {}
    virtual void OnDestroy() {}
    // Called by the Properties panel to draw editable properties in the editor.
    // Returns true when an editor control changed serialized component data.
    virtual bool DrawProperties(::Engine::Editor::IEditorUi& ui);

    // Runtime systems use this monotonically increasing value to rebuild
    // derived state only after configuration changes. Scripts that mutate
    // public component fields directly should call MarkConfigurationDirty().
    uint64_t GetConfigurationRevision() const { return m_configurationRevision; }
    void MarkConfigurationDirty()
    {
        if (++m_configurationRevision == 0)
            ++m_configurationRevision;
    }

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
    static JsonValue ToJson(const ComponentReference& value)
    {
        return JsonValue::MakeObject()
            .Set("expectedType", JsonValue(value.expectedType))
            .Set("objectPath", JsonValue(value.objectPath))
            .Set("objectName", JsonValue(value.objectName))
            .Set("componentType", JsonValue(value.componentType))
            .Set("componentIndex", JsonValue(value.componentIndex));
    }
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
    static void FromJson(const JsonValue& value, ComponentReference& out)
    {
        if (!value.IsObject()) return;
        if (value.Has("expectedType")) out.expectedType = value["expectedType"].AsString();
        out.objectPath = value["objectPath"].AsString();
        out.objectName = value["objectName"].AsString();
        out.componentType = value["componentType"].AsString();
        out.componentIndex = value["componentIndex"].AsInt();
    }
    static void FromJson(const JsonValue& value, glm::vec3& out)
    {
        if (!value.IsArray() || value.ArraySize() < 3)
            return;
        out = { value.ArrayAt(0).AsFloat(), value.ArrayAt(1).AsFloat(), value.ArrayAt(2).AsFloat() };
    }

    std::vector<SerializedField> m_serializedFields;
    std::string m_typeName = "Component";
    uint64_t m_configurationRevision = 1;
};
}

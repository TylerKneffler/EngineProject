#include "pch.h"
#include "Component.h"
#include "Core/Inspection/ComponentInspector.h"
#include "Core/Serialization/Json.h"
#include "Core/Graphics/IGraphicsProvider.h"
#include "Core/Scene/Object.h"
#include "Core/Scene/Scene.h"

bool Engine::Core::Component::DeferGraphicsPreparation(
    Engine::Graphics::IGraphicsProvider* provider)
{
    if (!provider || !Owner || !Owner->GetScene()) return false;
    Owner->GetScene()->QueueGraphicsPreparation();
    return true;
}

void Engine::Core::Component::MarkConfigurationDirty()
{
    if (Owner) Owner->AssertSceneOwner();
    if (++m_configurationRevision == 0)
        ++m_configurationRevision;
}

Engine::Core::Component::~Component()
{
    if(Owner && Owner->GetScene()) Owner->GetScene()->NotifyComponentRemoved(this);
}

namespace Engine::Core
{

bool Component::DrawProperties(Engine::Core::IInspectorUi& ui)
{
    return ComponentInspector::DrawGeneric(*this, ui);
}

bool Component::DrawPropertiesMulti(Engine::Core::IInspectorUi& ui,
    const std::vector<Component*>& targets)
{
    return ComponentInspector::DrawGeneric(*this, ui, &targets);
}

bool Component::DrawReferenceProperty(Engine::Core::IInspectorUi& ui,
    const char* fieldKey, const char* label, ComponentReference& reference,
    const Component* defaultTarget, const char* defaultDescription)
{
    return ComponentInspector::DrawReferenceProperty(*this, ui, fieldKey,
        label, reference, defaultTarget, defaultDescription);
}

bool Component::DrawAssetPathPicker(Engine::Core::IInspectorUi& ui,
    const void* context, const char* fieldKey, const char* label,
    const char* assetKind, std::string& path)
{
    return ComponentInspector::DrawAssetPathPicker(ui, context, fieldKey,
        label, assetKind, path);
}

Component::JsonValue Component::Serialize() const
{
    JsonValue data = JsonValue::MakeObject();
    data.Set("type", JsonValue(GetTypeName()));
    const JsonValue fields = SerializeFields();
    for (std::size_t i = 0; i < fields.ObjectSize(); ++i)
        data.Set(fields.ObjectKey(i), fields.ObjectValue(i));
    return data;
}

Component::JsonValue Component::SerializeFields() const
{
    JsonValue data = JsonValue::MakeObject();
    for (const auto& field : m_serializedFields)
        data.Set(field.name, field.write());
    return data;
}

void Component::Deserialize(const JsonValue& v)
{
    for (const auto& field : m_serializedFields)
        if (v.Has(field.name))
            field.read(v[field.name]);
    MarkConfigurationDirty();
}

Object* Component::FindObjectInChildrenByName(const std::string& objectName,
    bool includeSelf) const
{
    return Owner
        ? Owner->FindObjectInChildrenByName(objectName, includeSelf)
        : nullptr;
}

Object* Component::FindObjectInSceneByName(const std::string& objectName) const
{
    return Owner ? Owner->FindObjectInSceneByName(objectName) : nullptr;
}

Component* Component::GetComponentOnObjectNamedInScene(
    const std::string& objectName,
    const std::string& componentTypeName) const
{
    Object* object = FindObjectInSceneByName(objectName);
    if (!object)
        return nullptr;
    for (Component* component : object->Components)
    {
        if (component && component->GetTypeName() == componentTypeName)
            return component;
    }
    return nullptr;
}

}

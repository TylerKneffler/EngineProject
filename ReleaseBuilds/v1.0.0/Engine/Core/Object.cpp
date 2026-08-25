#include "Object.h"

#include "Core/Scene/Scene.h"

namespace
{
Engine::Core::Object* FindInChildrenByName(
    Engine::Core::Object* node,
    const std::string& objectName,
    bool includeSelf)
{
    if (!node)
        return nullptr;
    if (includeSelf && node->name == objectName)
        return node;
    for (Engine::Core::Object* child : node->Children)
    {
        if (!child)
            continue;
        if (Engine::Core::Object* found = FindInChildrenByName(child, objectName, true))
            return found;
    }
    return nullptr;
}

const Engine::Core::Object* FindInChildrenByName(
    const Engine::Core::Object* node,
    const std::string& objectName,
    bool includeSelf)
{
    if (!node)
        return nullptr;
    if (includeSelf && node->name == objectName)
        return node;
    for (const Engine::Core::Object* child : node->Children)
    {
        if (!child)
            continue;
        if (const Engine::Core::Object* found = FindInChildrenByName(child, objectName, true))
            return found;
    }
    return nullptr;
}
}

namespace Engine::Core
{

Object::Object()
{
    this->transform.Owner = this;
}

Object::Object(Transform initialTransform)
    : transform(initialTransform)
{
    this->transform.Owner = this;
}

Object::~Object()
{
    for (Component* component : Components)
        if (component) component->OnDestroy();
    for (Component* component : Components)
        delete component;
    Components.clear();
}

#pragma region Lifecycle methods
void Object::Enabled()
{
    if (enabled) return;
    enabled = true;
    for (Component* comp : Components)
        if (comp) comp->Enabled();
}

void Object::Disabled()
{
    if (!enabled) return;
    enabled = false;
    for (Component* comp : Components)
        if (comp) comp->Disabled();
}

void Object::Start()
{
    if (!IsEnabledInHierarchy()) return;
    for (Component* comp : Components)
        if (comp) comp->Start();
}

void Object::Update()
{
    if (!IsEnabledInHierarchy()) return;
    for (Component* comp : Components)
        if (comp) comp->Update();
}

void Object::Destroy()
{
    // Recursively destroy children
    for (Object* child : Children)
    {
        if (child)
            child->Destroy();
    }
    Children.clear();

    // Destroy components
    for (Component* comp : Components)
        if (comp) comp->OnDestroy();
    for (Component* comp : Components)
    {
        if (comp)
            delete comp;
    }
    Components.clear();

    // Detach from parent
    if (Parent)
    {
        Parent->Children.erase(
            std::remove(Parent->Children.begin(), Parent->Children.end(), this),
            Parent->Children.end());
        Parent = nullptr;
    }

    // Finally, delete self
    delete this;
}
#pragma endregion

Object* Object::FindObjectInChildrenByName(const std::string& objectName,
    bool includeSelf)
{
    return FindInChildrenByName(this, objectName, includeSelf);
}

const Object* Object::FindObjectInChildrenByName(const std::string& objectName,
    bool includeSelf) const
{
    return FindInChildrenByName(this, objectName, includeSelf);
}

Object* Object::FindObjectInSceneByName(const std::string& objectName)
{
    if (!OwnerScene)
        return nullptr;
    for (const auto& candidate : OwnerScene->GetObjects())
    {
        Object* object = candidate.get();
        if (object && object->name == objectName)
            return object;
    }
    return nullptr;
}

const Object* Object::FindObjectInSceneByName(const std::string& objectName) const
{
    if (!OwnerScene)
        return nullptr;
    for (const auto& candidate : OwnerScene->GetObjects())
    {
        const Object* object = candidate.get();
        if (object && object->name == objectName)
            return object;
    }
    return nullptr;
}

#pragma region Component management
// Template bodies are defined in Object.h so they are visible at every
// instantiation site — nothing to implement here.
#pragma endregion

}

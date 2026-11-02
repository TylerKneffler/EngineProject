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
    if (OwnerScene) AssertSceneOwner();
    for (Component* component : Components)
        if (component) component->FenceBackgroundWork();
    for (Component* component : Components)
        if (component) component->DispatchDestroy();
    for (Component* component : Components)
        delete component;
    Components.clear();
}

void Object::NotifyStructureChanged()
{
    if (OwnerScene)
        OwnerScene->NotifyStructureChanged(this);
}

void Object::AssertSceneOwner() const
{
    if (OwnerScene)
        OwnerScene->AssertOwnerThread();
}

#pragma region Lifecycle methods
void Object::Enabled()
{
    AssertSceneOwner();
    if (enabled) return;
    enabled = true;
    if (OwnerScene && OwnerScene->HasStarted())
    {
        // Scene storage order defines callback order for the whole subtree.
        std::vector<Object*> descendants;
        for (const auto& candidate : OwnerScene->GetObjects())
            for (Object* parent = candidate.get(); parent; parent = parent->Parent)
                if (parent == this)
                {
                    descendants.push_back(candidate.get());
                    break;
                }
        for (Object* descendant : descendants)
            if (std::any_of(OwnerScene->GetObjects().begin(),
                    OwnerScene->GetObjects().end(),
                    [descendant](const auto& current)
                    { return current.get() == descendant; }))
                descendant->Start();
    }
}

void Object::Disabled()
{
    AssertSceneOwner();
    if (!enabled) return;
    const bool wasActive = IsEnabledInHierarchy();
    enabled = false;
    if (wasActive)
    {
        for (Component* comp : Components)
            if (comp) comp->DispatchDisabled();
        for (Object* child : Children)
            if (child) child->DisableLifecycleInHierarchy();
    }
}

void Object::DisableLifecycleInHierarchy()
{
    for (Component* comp : Components)
        if (comp) comp->DispatchDisabled();
    for (Object* child : Children)
        if (child) child->DisableLifecycleInHierarchy();
}

void Object::Start()
{
    AssertSceneOwner();
    if (!IsEnabledInHierarchy()) return;
    for (Component* comp : Components)
        if (comp)
        {
            comp->DispatchStart();
            comp->DispatchEnabled();
        }
}

void Object::Update()
{
    AssertSceneOwner();
    if (!IsEnabledInHierarchy()) return;
    for (Component* comp : Components)
        if (comp && comp->RequiresUpdate())
        {
            comp->Update();
            comp->PublishUpdateOnOwner();
        }
}

void Object::Destroy()
{
    AssertSceneOwner();
    // Scene objects are owned by Scene::m_objects. Deleting one directly is
    // invalid (and especially unsafe from a component callback), because the
    // scene may still be iterating the object/component containers. Queue the
    // owned object and let Scene commit removal at a safe update boundary.
    if (OwnerScene)
    {
        OwnerScene->RequestRemoveObject(this);
        return;
    }

    // Recursively destroy children
    for (Object* child : Children)
    {
        if (child)
            child->Destroy();
    }
    Children.clear();

    // Destroy components
    for (Component* comp : Components)
        if (comp) { comp->FenceBackgroundWork(); comp->DispatchDestroy(); }
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
    return OwnerScene->FindObjectByName(objectName);
}

const Object* Object::FindObjectInSceneByName(const std::string& objectName) const
{
    if (!OwnerScene)
        return nullptr;
    return OwnerScene->FindObjectByName(objectName);
}

#pragma region Component management
// Template bodies are defined in Object.h so they are visible at every
// instantiation site — nothing to implement here.
#pragma endregion

}

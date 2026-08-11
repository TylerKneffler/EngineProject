#include "Object.h"
#include "Core/Script.h"

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
        if (Script* script = dynamic_cast<Script*>(component)) script->OnDestroy();
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
        if (Script* script = dynamic_cast<Script*>(comp)) script->Enabled();
}

void Object::Disabled()
{
    if (!enabled) return;
    enabled = false;
    for (Component* comp : Components)
        if (Script* script = dynamic_cast<Script*>(comp)) script->Disabled();
}

void Object::Start()
{
    if (!IsEnabledInHierarchy()) return;
    for (Component* comp : Components)
        if (Script* s = dynamic_cast<Script*>(comp))
            s->Start();
}

void Object::Update()
{
    if (!IsEnabledInHierarchy()) return;
    for (Component* comp : Components)
        if (Script* s = dynamic_cast<Script*>(comp))
            s->Update();
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
    {
        if (Script* script = dynamic_cast<Script*>(comp))
            script->OnDestroy();
    }
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

#pragma region Component management
// Template bodies are defined in Object.h so they are visible at every
// instantiation site — nothing to implement here.
#pragma endregion

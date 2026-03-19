#include "Object.h"

Object::Object()
{
    this->transform = Transform();
}

Object::Object(Transform transform)
{
    this->transform = transform;
}

Object::~Object()
{
}

#pragma region Lifecycle methods
void Object::Enabled()
{
}

void Object::Disabled()
{
}

void Object::Start()
{
}

void Object::Update()
{
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
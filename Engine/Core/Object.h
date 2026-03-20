#pragma once
#include "pch.h"
#include <list>
#include <algorithm>
#include "Core/component.h"         
#include "Core/Compoonents/Transform.h"

class Object
{
public:
    Object();
    Object(Transform transform);
    ~Object();

    Transform transform;

    Object*             Parent = nullptr;
    std::vector<Object*> Children;
    std::list<Component*> Components;

    // Lifecycle methods
    virtual void Enabled();
    virtual void Disabled();
    virtual void Start();
    virtual void Update();
    virtual void Destroy();

    // Component management — implementations must live in the header so the
    // compiler sees them at every instantiation site.
    template<typename T, typename... Args>
    T* AddComponent(Args&&... args)
    {
        T* component = new T(std::forward<Args>(args)...);
        component->Owner = this;
        Components.push_back(component);
        return component;
    }

    template<typename T>
    T* GetComponent()
    {
        for (Component* comp : Components)
        {
            if (T* casted = dynamic_cast<T*>(comp))
                return casted;
        }
        return nullptr;
    }
};
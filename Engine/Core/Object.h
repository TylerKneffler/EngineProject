#pragma once
#include "pch.h"
#include <list>
#include <algorithm>
#include "Core/component.h"         
#include "Core/Compoonents/Transform.h"
#include "Core/Assets/PrefabAsset.h"

class Scene; // forward declaration

class Object
{
public:
    Object();
    Object(Transform transform);
    ~Object();

    std::string name;
    bool enabled = true;
    Transform transform;

    Object*             Parent = nullptr;
    std::vector<Object*> Children;
    std::list<Component*> Components;
    Scene*              OwnerScene = nullptr;  // Set by Scene when object is added
    std::shared_ptr<const PrefabAsset> Prefab; // null for scene-only objects

    bool IsPrefabInstance() const { return static_cast<bool>(Prefab); }
    void SetPrefab(const std::string& path) { Prefab = PrefabAsset::Acquire(path); }
    Object* GetPrefabInstanceRoot()
    {
        for (Object* current = this; current; current = current->Parent)
            if (current->Prefab)
                return current;
        return nullptr;
    }
    const Object* GetPrefabInstanceRoot() const
    {
        for (const Object* current = this; current; current = current->Parent)
            if (current->Prefab)
                return current;
        return nullptr;
    }
    bool IsPartOfPrefabInstance() const
    {
        return GetPrefabInstanceRoot() != nullptr;
    }

    bool IsEnabledInHierarchy() const
    {
        for (const Object* current = this; current; current = current->Parent)
            if (!current->enabled)
                return false;
        return true;
    }

    // Lifecycle methods
    virtual void Enabled();
    virtual void Disabled();
    virtual void Start();
    virtual void Update();
    virtual void Destroy();

    // Get the scene this object belongs to
    Scene* GetScene() const { return OwnerScene; }

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

    template<typename T>
    const T* GetComponent() const
    {
        for (const Component* comp : Components)
        {
            if (const T* casted = dynamic_cast<const T*>(comp))
                return casted;
        }
        return nullptr;
    }
};

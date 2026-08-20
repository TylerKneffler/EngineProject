#pragma once
#include "pch.h"
#include <list>
#include <algorithm>
#include "Core/component.h"         
#include "Core/Compoonents/Transform.h"
#include "Core/Prefab/PrefabAsset.h"

namespace Engine::Scene { class Scene; }

namespace Engine::Core
{
class Object
{
public:
    using Transform = Engine::Components::Transform;
    using PrefabAsset = Engine::Prefab::PrefabAsset;

    Object();
    Object(Transform initialTransform);
    ~Object();

    std::string name;
    bool enabled = true;
    Transform transform;

    Object*             Parent = nullptr;
    std::vector<Object*> Children;
    std::list<Component*> Components;
    Engine::Scene::Scene* OwnerScene = nullptr;
    std::shared_ptr<const PrefabAsset> Prefab; // null for scene-only objects
    // Full prefab object state at the time this linked instance was loaded.
    // Scene serialization stores only differences from this baseline.
    std::string PrefabSourceSnapshot;
    mutable bool PrefabOverrideCacheValid = false;
    mutable bool PrefabHasOverrides = false;
    mutable bool PrefabHasNonTransformOverrides = false;
    mutable std::string PrefabOverridePatchSnapshot;
    // Kept separately so moving a prefab root can be checked without parsing
    // and diffing the complete serialized prefab hierarchy every frame.
    glm::vec3 PrefabSourcePosition { 0.f };
    glm::vec3 PrefabSourceRotation { 0.f };
    glm::vec3 PrefabSourceScale { 1.f };
    bool PrefabSourceTransformValid = false;

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
    void InvalidatePrefabOverrideCache()
    {
        if (Object* root = GetPrefabInstanceRoot())
            root->PrefabOverrideCacheValid = false;
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
    Engine::Scene::Scene* GetScene() const { return OwnerScene; }

    // Hierarchy lookup helpers
    Object* FindObjectInChildrenByName(const std::string& objectName,
        bool includeSelf = false);
    const Object* FindObjectInChildrenByName(const std::string& objectName,
        bool includeSelf = false) const;
    Object* FindObjectInSceneByName(const std::string& objectName);
    const Object* FindObjectInSceneByName(const std::string& objectName) const;

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

    template<typename T>
    T* GetComponentInParent(bool includeSelf = true)
    {
        for (Object* current = includeSelf ? this : Parent;
            current; current = current->Parent)
        {
            if (T* component = current->GetComponent<T>())
                return component;
        }
        return nullptr;
    }

    template<typename T>
    const T* GetComponentInParent(bool includeSelf = true) const
    {
        for (const Object* current = includeSelf ? this : Parent;
            current; current = current->Parent)
        {
            if (const T* component = current->GetComponent<T>())
                return component;
        }
        return nullptr;
    }

    template<typename T>
    T* GetComponentInChildren(bool includeSelf = true)
    {
        if (includeSelf)
            if (T* component = GetComponent<T>())
                return component;
        for (Object* child : Children)
        {
            if (!child)
                continue;
            if (T* component = child->GetComponentInChildren<T>(true))
                return component;
        }
        return nullptr;
    }

    template<typename T>
    const T* GetComponentInChildren(bool includeSelf = true) const
    {
        if (includeSelf)
            if (const T* component = GetComponent<T>())
                return component;
        for (const Object* child : Children)
        {
            if (!child)
                continue;
            if (const T* component = child->GetComponentInChildren<T>(true))
                return component;
        }
        return nullptr;
    }

    template<typename T>
    std::vector<T*> GetComponentsInChildren(bool includeSelf = true)
    {
        std::vector<T*> result;
        if (includeSelf)
            if (T* component = GetComponent<T>())
                result.push_back(component);
        for (Object* child : Children)
        {
            if (!child)
                continue;
            std::vector<T*> childComponents = child->GetComponentsInChildren<T>(true);
            result.insert(result.end(), childComponents.begin(), childComponents.end());
        }
        return result;
    }

    template<typename T>
    std::vector<const T*> GetComponentsInChildren(bool includeSelf = true) const
    {
        std::vector<const T*> result;
        if (includeSelf)
            if (const T* component = GetComponent<T>())
                result.push_back(component);
        for (const Object* child : Children)
        {
            if (!child)
                continue;
            std::vector<const T*> childComponents = child->GetComponentsInChildren<T>(true);
            result.insert(result.end(), childComponents.begin(), childComponents.end());
        }
        return result;
    }

};
}

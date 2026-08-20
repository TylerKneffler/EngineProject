#pragma once
#include "Core/component.h"
#include "Core/Object.h"

// ---------------------------------------------------------------------------
// Script — base class for all user scripts.
//
// Inherits from Component so it can be attached to any Object via
// AddComponent<T>(). Script identifies game-authored, hot-reloadable custom
// behavior; lifecycle dispatch itself is provided by Component.
//
// Usage:
//   class MyScript : public Script {
//   public:
//       PROPERTY(Inspector, EditAnywhere, Category = "Stats")
//       float health = 100.f;
//       
//       PROPERTY(Inspector, EditAnywhere, Category = "Movement")
//       float speed = 5.f;
//       
//       void Start()  override { /* init */ }
//       void Update() override { /* per frame */ }
//       std::string GetTypeName() const override { return "MyScript"; }
//       
//       // IMPORTANT: Include all PROPERTY(Inspector) fields in Serialize/Deserialize
//       JsonValue Serialize() const override {
//           JsonValue data = JsonValue::MakeObject();
//           data.Set("type", JsonValue(std::string("MyScript")));
//           data.Set("health", JsonValue(health));  // <- Makes it appear in inspector
//           data.Set("speed", JsonValue(speed));    // <- Makes it appear in inspector
//           return data;
//       }
//       void Deserialize(const JsonValue& v) override {
//           if (v.Has("health")) health = v["health"].AsFloat();
//           if (v.Has("speed")) speed = v["speed"].AsFloat();
//       }
//   };
//   obj->AddComponent<MyScript>();
// ---------------------------------------------------------------------------
namespace Engine::Core
{
class Script : public Component
{
public:
    Script() { SetTypeName("Script"); }
    virtual ~Script() = default;

    void Start() override {}
    void Update() override {}
    void Enabled() override {}
    void Disabled() override {}
    void OnDestroy() override {}

    // Derived scripts set their serialized type name in their constructor.
    // Returning Component's stored name preserves that identity in scene and
    // play-mode snapshot serialization.
    std::string GetTypeName() const override
    {
        return Component::GetTypeName();
    }

    // Hierarchy helpers for gameplay scripts.
    Object* FindObjectInChildrenByName(const std::string& objectName,
        bool includeSelf = false) const
    {
        return Owner
            ? Owner->FindObjectInChildrenByName(objectName, includeSelf)
            : nullptr;
    }

    Object* FindObjectInSceneByName(const std::string& objectName) const
    {
        return Owner ? Owner->FindObjectInSceneByName(objectName) : nullptr;
    }

    template<typename T>
    T* GetComponentInParent(bool includeSelf = true) const
    {
        return Owner ? Owner->GetComponentInParent<T>(includeSelf) : nullptr;
    }

    template<typename T>
    T* GetComponentInChildren(bool includeSelf = true) const
    {
        return Owner ? Owner->GetComponentInChildren<T>(includeSelf) : nullptr;
    }

    template<typename T>
    std::vector<T*> GetComponentsInChildren(bool includeSelf = true) const
    {
        return Owner ? Owner->GetComponentsInChildren<T>(includeSelf)
            : std::vector<T*>{};
    }

    template<typename T>
    T* GetComponentOnObjectNamedInScene(const std::string& objectName) const
    {
        if (!Owner)
            return nullptr;
        Object* object = Owner->FindObjectInSceneByName(objectName);
        return object ? object->GetComponent<T>() : nullptr;
    }
    
    // Override in derived classes to draw custom properties in the inspector
    // bool DrawProperties(::Engine::Editor::IEditorUi& ui) override { /* custom UI */ return false; }
};
}

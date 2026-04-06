#pragma once
#include "Core/component.h"

// ---------------------------------------------------------------------------
// Script — base class for all user scripts.
//
// Inherits from Component so it can be attached to any Object via
// AddComponent<T>(). Override the lifecycle methods to add behaviour.
// Object::Start() and Object::Update() automatically forward to every
// Script component on the object.
//
// Usage:
//   class MyScript : public Script {
//       void Start()  override { /* init */ }
//       void Update() override { /* per frame */ }
//   };
//   obj->AddComponent<MyScript>();
// ---------------------------------------------------------------------------
class Script : public Component
{
public:
    Script()          = default;
    virtual ~Script() = default;

    virtual void Start()   {}
    virtual void Update()  {}
    virtual void Enabled() {}
    virtual void Disabled(){}
    virtual void OnDestroy(){}
};

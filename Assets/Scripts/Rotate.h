#pragma once
#include "Core/Script.h"
#include <glm/glm.hpp>

// ---------------------------------------------------------------------------
// Rotate — Script component that spins the owning object each frame.
//
// Usage:
//   Rotate* r = obj->AddComponent<Rotate>();
//   r->axis  = { 0.f, 1.f, 0.f }; // spin around Y
//   r->speed = 90.f;               // degrees per second
// ---------------------------------------------------------------------------
class Rotate : public Script
{
public:
    glm::vec3 axis  { 0.f, 1.f, 0.f }; // rotation axis (world space)
    float     speed { 45.f };           // degrees per second

    void Start()  override;
    void Update() override;

    // Serialization
    std::string GetTypeName() const override { return "Rotate"; }
    JsonValue   Serialize()   const override;
    void        Deserialize(const JsonValue& v) override;
};

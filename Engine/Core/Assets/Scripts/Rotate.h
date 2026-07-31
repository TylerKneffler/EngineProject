#pragma once
#include "Core/Script.h"
#include "Core/PropertyMacros.h"
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
    Rotate();

    PROPERTY(Inspector, EditAnywhere, Category = "Rotation", Range = "-1.0, 1.0")
    glm::vec3 axis  { 0.f, 1.f, 0.f }; // rotation axis (world space)
    
    PROPERTY(Inspector, EditAnywhere, Category = "Rotation")
    float     speed { 45.f };           // degrees per second

    void Start()  override;
    void Update() override;

    // Custom property editor
    void DrawProperties(IEditorUi& ui) override;
};

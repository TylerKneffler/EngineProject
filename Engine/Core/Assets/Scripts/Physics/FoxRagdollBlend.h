#pragma once

#include "Core/Script.h"
#include "Core/PropertyMacros.h"
#include <string>

// Controller for the fox physics scene. Animation owns the pose first; after
// the hold, the per-bone physics pose gradually takes over.
class FoxRagdollBlend final : public Engine::Core::Script
{
public:
    FoxRagdollBlend();

    PROPERTY(Inspector, EditAnywhere, Category = "Fox Ragdoll")
    std::string foxObjectName = "Fox";
    PROPERTY(Inspector, EditAnywhere, Category = "Fox Ragdoll", ClampMin = "0")
    float animationSeconds = 1.25f;
    PROPERTY(Inspector, EditAnywhere, Category = "Fox Ragdoll", ClampMin = "0")
    float blendSeconds = 2.5f;

    void Start() override;
    void Update() override;
    bool DrawProperties(Engine::Editor::IEditorUi& ui) override;
    float GetInfluence() const { return m_influence; }

private:
    bool ConfigureFox();
    float m_elapsed = 0.f;
    float m_influence = 0.f;
    bool m_configured = false;
};

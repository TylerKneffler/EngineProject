#pragma once

#include "Core/Script.h"
#include "Core/PropertyMacros.h"
#include <string>

// Runs animation, then blends into gravity-driven per-bone IK. A scene can
// hand the final pose to a dynamic mesh-collider body instead of keeping IK.
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
    PROPERTY(Inspector, EditAnywhere, Category = "Fox Ragdoll")
    bool meshColliderDuringAnimation = false;
    PROPERTY(Inspector, EditAnywhere, Category = "Fox Ragdoll")
    bool meshColliderAfterBlend = false;

    void Start() override;
    void Update() override;
    bool DrawProperties(Engine::Editor::IEditorUi& ui) override;
    float GetInfluence() const { return m_influence; }

private:
    bool ConfigureFox();
    float m_elapsed = 0.f;
    float m_influence = 0.f;
    bool m_configured = false;
    bool m_blending = false;
    bool m_finished = false;
};

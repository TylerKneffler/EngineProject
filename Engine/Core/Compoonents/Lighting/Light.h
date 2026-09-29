#pragma once
#include "Core/component.h"
#include "Core/PropertyMacros.h"
#include <glm/glm.hpp>

namespace Engine::Components
{
// Realtime or baked light. Point lights attenuate from their object position;
// global lights illuminate the whole scene from their Transform rotation.
class Light : public Engine::Core::Component
{
public:
    enum class Type : int
    {
        Point = 0,
        Directional = 1,
        // Source compatibility for scripts built against the old name. The
        // serialized value is unchanged, so existing scenes load as Directional.
        Ambient = Directional
    };

    Light();
    ~Light() = default;

    PROPERTY(Inspector, EditAnywhere, Category = "Light")
    glm::vec3 color { 1.f, 0.95f, 0.85f };

    // Stored as an integer so existing generic component serialization can
    // preserve it. Scenes without this field remain point lights.
    PROPERTY(Inspector, EditAnywhere, Category = "Light")
    int lightType = static_cast<int>(Type::Point);

    PROPERTY(Inspector, EditAnywhere, Category = "Light", ClampMin = "0.0")
    float intensity = 4.f;

    PROPERTY(Inspector, EditAnywhere, Category = "Light", ClampMin = "0.01")
    float range = 8.f;

    PROPERTY(Inspector, EditAnywhere, Category = "Light", Range = "0.1, 8.0")
    float falloff = 2.f;

    // false = Realtime, true = Baked.
    PROPERTY(Inspector, EditAnywhere, Category = "Light")
    bool baked = false;

    // Realtime raster-shadow authoring state. Existing scenes deserialize to
    // false so adding the feature does not silently increase rendering cost.
    PROPERTY(Inspector, EditAnywhere, Category = "Light | Shadows")
    bool castsShadows = false;

    PROPERTY(Inspector, EditAnywhere, Category = "Light | Shadows", Range = "0.0, 1.0")
    float shadowStrength = 1.f;

    PROPERTY(Inspector, EditAnywhere, Category = "Light | Shadows", Range = "0.0, 0.05")
    float shadowDepthBias = 0.0015f;

    PROPERTY(Inspector, EditAnywhere, Category = "Light | Shadows", Range = "0.0, 0.1")
    float shadowNormalBias = 0.01f;

    PROPERTY(Inspector, EditAnywhere, Category = "Light | Shadows", Range = "0.25, 1.0")
    float shadowResolutionScale = 1.f;

    PROPERTY(Inspector, EditAnywhere, Category = "Light | Shadows", Range = "0.0, 1.0")
    float shadowFilterScale = 1.f;

    Type GetLightType() const
    {
        return lightType == static_cast<int>(Type::Directional)
            ? Type::Directional : Type::Point;
    }

    bool DrawProperties(::Engine::Editor::IEditorUi& ui) override;
};
}

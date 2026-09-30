#pragma once
#include "Core/component.h"
#include "Core/PropertyMacros.h"
#include <glm/glm.hpp>
#include <string>

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
        Spot = 2,
        // Source compatibility for scripts built against the old name. The
        // serialized value is unchanged, so existing scenes load as Directional.
        Ambient = Directional
    };

    enum class IntensityMode : int
    {
        Legacy = 0,
        Physical = 1
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

    // Compatibility preserves the historical arbitrary intensity and powered
    // linear range fade. Physical uses lumens for point/spot lights, lux for
    // directional lights, inverse-square falloff, and a smooth range cutoff.
    PROPERTY(Inspector, EditAnywhere, Category = "Light")
    int intensityMode = static_cast<int>(IntensityMode::Legacy);

    PROPERTY(Inspector, EditAnywhere, Category = "Light | Spot", Range = "0.0, 89.0")
    float innerConeAngle = 25.f;

    PROPERTY(Inspector, EditAnywhere, Category = "Light | Spot", Range = "0.1, 89.0")
    float outerConeAngle = 35.f;

    // Bitwise intersection with the receiving material's lightingChannels.
    PROPERTY(Inspector, EditAnywhere, Category = "Light")
    int lightingChannels = -1;

    // Greyscale cookie and angular photometric profile texture. Cookie UVs
    // follow the light's local X/Y axes; IES profiles use angle from its +Z.
    PROPERTY(Inspector, EditAnywhere, Category = "Light | Projection")
    std::string cookieTexture;

    PROPERTY(Inspector, EditAnywhere, Category = "Light | Projection")
    std::string iesProfileTexture;

    PROPERTY(Inspector, EditAnywhere, Category = "Light | Projection", ClampMin = "0.001")
    float cookieScale = 1.f;

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
        if (lightType == static_cast<int>(Type::Directional)) return Type::Directional;
        if (lightType == static_cast<int>(Type::Spot)) return Type::Spot;
        return Type::Point;
    }

    IntensityMode GetIntensityMode() const
    {
        return intensityMode == static_cast<int>(IntensityMode::Physical)
            ? IntensityMode::Physical : IntensityMode::Legacy;
    }

    bool DrawProperties(::Engine::Editor::IEditorUi& ui) override;
};
}

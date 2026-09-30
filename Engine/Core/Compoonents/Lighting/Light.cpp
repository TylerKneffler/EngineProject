#include "Light.h"
#include "Engine/Editor/UI/IEditorUi.h"
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <cstdlib>

namespace Engine::Components
{
Light::Light()
{
    SetTypeName(COMPONENT_TYPE_NAME(Light));
    RegisterField("lightType", lightType);
    RegisterField("color", color);
    RegisterField("intensity", intensity);
    RegisterField("range", range);
    RegisterField("falloff", falloff);
    RegisterField("intensityMode", intensityMode);
    RegisterField("innerConeAngle", innerConeAngle);
    RegisterField("outerConeAngle", outerConeAngle);
    RegisterField("lightingChannels", lightingChannels);
    RegisterField("cookieTexture", cookieTexture);
    RegisterField("iesProfileTexture", iesProfileTexture);
    RegisterField("cookieScale", cookieScale);
    RegisterField("baked", baked);
    RegisterField("castsShadows", castsShadows, "Shadows");
    RegisterField("shadowStrength", shadowStrength, "Shadows");
    RegisterField("shadowDepthBias", shadowDepthBias, "Shadows");
    RegisterField("shadowNormalBias", shadowNormalBias, "Shadows");
    RegisterField("shadowResolutionScale", shadowResolutionScale, "Shadows");
    RegisterField("shadowFilterScale", shadowFilterScale, "Shadows");
}

bool Light::DrawProperties(::Engine::Editor::IEditorUi& ui)
{
    bool changed = false;
    const char* types[] = { "Point", "Directional", "Spot" };
    if (ui.Combo("Type", &lightType, types, 3))
    {
        lightType = std::clamp(lightType, static_cast<int>(Type::Point),
            static_cast<int>(Type::Spot));
        changed = true;
    }

    changed = ui.ColorEdit3("Color", &color.x) || changed;
    char channels[16]{};
    std::snprintf(channels, sizeof(channels), "0x%08X",
        static_cast<unsigned>(lightingChannels));
    if (ui.InputText("Lighting channels", channels, sizeof(channels)))
    {
        lightingChannels = static_cast<int>(std::strtoul(channels, nullptr, 0));
        changed = true;
    }
    const char* units[] = { "Compatibility", "Physical" };
    changed = ui.Combo("Units / attenuation", &intensityMode, units, 2) || changed;
    intensityMode = std::clamp(intensityMode, 0, 1);
    const bool physical = GetIntensityMode() == IntensityMode::Physical;
    const char* intensityLabel = physical
        ? (GetLightType() == Type::Directional ? "Illuminance (lux)" : "Flux (lumens)")
        : "Intensity";
    changed = ui.DragFloat(intensityLabel, &intensity, physical ? 1.f : 0.05f,
        0.f, physical ? 100000.f : 100.f) || changed;
    if (GetLightType() != Type::Directional)
    {
        changed = ui.DragFloat("Range", &range, 0.1f, 0.01f, 1000.f) || changed;
        if (!physical)
            changed = ui.DragFloat("Falloff", &falloff, 0.05f, 0.1f, 8.f) || changed;
        else
            ui.DisabledLabel("Inverse-square attenuation with a smooth range cutoff.");
        if (GetLightType() == Type::Spot)
        {
            changed = ui.DragFloat("Inner cone (degrees)", &innerConeAngle,
                0.25f, 0.f, 89.f) || changed;
            changed = ui.DragFloat("Outer cone (degrees)", &outerConeAngle,
                0.25f, 0.1f, 89.f) || changed;
            outerConeAngle = std::clamp(outerConeAngle, 0.1f, 89.f);
            innerConeAngle = std::clamp(innerConeAngle, 0.f, outerConeAngle);
            ui.DisabledLabel("Direction follows the object's local +Z axis.");
        }
        char cookie[512]{};
        std::snprintf(cookie, sizeof(cookie), "%s", cookieTexture.c_str());
        if (ui.InputText("Cookie texture", cookie, sizeof(cookie)))
        { cookieTexture = cookie; changed = true; }
        char ies[512]{};
        std::snprintf(ies, sizeof(ies), "%s", iesProfileTexture.c_str());
        if (ui.InputText("IES profile texture", ies, sizeof(ies)))
        { iesProfileTexture = ies; changed = true; }
        changed = ui.DragFloat("Cookie scale", &cookieScale, 0.01f,
            0.001f, 1000.f) || changed;
    }
    else
    {
        ui.DisabledLabel("Direction uses the object's rotation; position is ignored.");
        char cookie[512]{};
        std::snprintf(cookie, sizeof(cookie), "%s", cookieTexture.c_str());
        if (ui.InputText("Cookie texture", cookie, sizeof(cookie)))
        { cookieTexture = cookie; changed = true; }
        changed = ui.DragFloat("Cookie scale", &cookieScale, 0.01f,
            0.001f, 1000.f) || changed;
    }

    int selectedMode = baked ? 1 : 0;
    const char* modes[] = { "Realtime", "Baked" };
    if (ui.Combo("Mode", &selectedMode, modes, 2))
    {
        baked = selectedMode == 1;
        changed = true;
    }

    if (baked)
        ui.DisabledLabel("Contributes when Scene > Bake Lighting is run.");
    else if (GetLightType() == Type::Directional)
    {
        changed = ui.Checkbox("Cast Shadows", &castsShadows) || changed;
        if (castsShadows)
        {
            changed = ui.DragFloat("Shadow Strength", &shadowStrength,
                0.01f, 0.f, 1.f) || changed;
            changed = ui.DragFloat("Shadow Depth Bias", &shadowDepthBias,
                0.00005f, 0.f, 0.05f) || changed;
            changed = ui.DragFloat("Shadow Normal Bias", &shadowNormalBias,
                0.0001f, 0.f, 0.1f) || changed;
            changed = ui.DragFloat("Shadow Resolution Scale", &shadowResolutionScale,
                0.05f, 0.25f, 1.f) || changed;
            changed = ui.DragFloat("Shadow Filter Scale", &shadowFilterScale,
                0.05f, 0.f, 1.f) || changed;
            shadowStrength = std::clamp(shadowStrength, 0.f, 1.f);
            shadowDepthBias = std::clamp(shadowDepthBias, 0.f, 0.05f);
            shadowNormalBias = std::clamp(shadowNormalBias, 0.f, 0.1f);
            shadowResolutionScale = std::clamp(shadowResolutionScale, 0.25f, 1.f);
            shadowFilterScale = std::clamp(shadowFilterScale, 0.f, 1.f);
            ui.DisabledLabel("Directional realtime shadow maps render on DirectX 11, DirectX 12, and Vulkan.");
        }
    }
    else if (GetLightType() == Type::Spot)
        ui.DisabledLabel("Spot shadow policy: unshadowed; does not consume the directional atlas budget.");
    else
        ui.DisabledLabel("Point shadow policy: unshadowed; cubemap shadows are not yet supported.");
    return changed;
}
}

#include "Light.h"
#include "Engine/Editor/UI/IEditorUi.h"
#include <algorithm>

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
    const char* types[] = { "Point", "Directional" };
    if (ui.Combo("Type", &lightType, types, 2))
    {
        lightType = lightType == static_cast<int>(Type::Directional)
            ? static_cast<int>(Type::Directional)
            : static_cast<int>(Type::Point);
        changed = true;
    }

    changed = ui.ColorEdit3("Color", &color.x) || changed;
    changed = ui.DragFloat("Intensity", &intensity, 0.05f, 0.f, 100.f) || changed;
    if (GetLightType() == Type::Point)
    {
        changed = ui.DragFloat("Range", &range, 0.1f, 0.01f, 1000.f) || changed;
        changed = ui.DragFloat("Falloff", &falloff, 0.05f, 0.1f, 8.f) || changed;
    }
    else
        ui.DisabledLabel("Direction uses the object's rotation; position is ignored.");

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
    else
        ui.DisabledLabel("Point-light cubemap shadows are not yet supported.");
    return changed;
}
}

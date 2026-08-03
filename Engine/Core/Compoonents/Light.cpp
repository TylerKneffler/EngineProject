#include "Light.h"
#include "Engine/Editor/UI/IEditorUi.h"

Light::Light()
{
    SetTypeName(COMPONENT_TYPE_NAME(Light));
    RegisterField("lightType", lightType);
    RegisterField("color", color);
    RegisterField("intensity", intensity);
    RegisterField("range", range);
    RegisterField("falloff", falloff);
    RegisterField("baked", baked);
}

void Light::DrawProperties(IEditorUi& ui)
{
    const char* types[] = { "Point", "Global / Ambient" };
    if (ui.Combo("Type", &lightType, types, 2))
        lightType = lightType == static_cast<int>(Type::Ambient)
            ? static_cast<int>(Type::Ambient)
            : static_cast<int>(Type::Point);

    ui.ColorEdit3("Color", &color.x);
    ui.DragFloat("Intensity", &intensity, 0.05f, 0.f, 100.f);
    if (GetLightType() == Type::Point)
    {
        ui.DragFloat("Range", &range, 0.1f, 0.01f, 1000.f);
        ui.DragFloat("Falloff", &falloff, 0.05f, 0.1f, 8.f);
    }
    else
        ui.DisabledLabel("Direction uses the object's rotation; position is ignored.");

    int selectedMode = baked ? 1 : 0;
    const char* modes[] = { "Realtime", "Baked" };
    if (ui.Combo("Mode", &selectedMode, modes, 2))
        baked = selectedMode == 1;

    if (baked)
        ui.DisabledLabel("Contributes when Scene > Bake Lighting is run.");
}

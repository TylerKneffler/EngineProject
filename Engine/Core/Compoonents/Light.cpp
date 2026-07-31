#include "Light.h"
#include "Engine/Editor/UI/IEditorUi.h"

Light::Light()
{
    SetTypeName(COMPONENT_TYPE_NAME(Light));
    RegisterField("color", color);
    RegisterField("intensity", intensity);
    RegisterField("range", range);
    RegisterField("falloff", falloff);
    RegisterField("baked", baked);
}

void Light::DrawProperties(IEditorUi& ui)
{
    ui.ColorEdit3("Color", &color.x);
    ui.DragFloat("Intensity", &intensity, 0.05f, 0.f, 100.f);
    ui.DragFloat("Range", &range, 0.1f, 0.01f, 1000.f);
    ui.DragFloat("Falloff", &falloff, 0.05f, 0.1f, 8.f);

    int selectedMode = baked ? 1 : 0;
    const char* modes[] = { "Realtime", "Baked" };
    if (ui.Combo("Mode", &selectedMode, modes, 2))
        baked = selectedMode == 1;

    if (baked)
        ui.DisabledLabel("Baked lighting will contribute when lightmap baking is added.");
}

#include "Light.h"
#include "Engine/Editor/UI/IEditorUi.h"

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
}

bool Light::DrawProperties(::Engine::Editor::IEditorUi& ui)
{
    bool changed = false;
    const char* types[] = { "Point", "Global / Ambient" };
    if (ui.Combo("Type", &lightType, types, 2))
    {
        lightType = lightType == static_cast<int>(Type::Ambient)
            ? static_cast<int>(Type::Ambient)
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
    return changed;
}
}

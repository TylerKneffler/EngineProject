#include "BakedLightingData.h"
#include "Engine/Editor/UI/IEditorUi.h"

BakedLightingData::BakedLightingData()
{
    SetTypeName(COMPONENT_TYPE_NAME(BakedLightingData));
    singlecomponent = true;
    RegisterField("irradiance", irradiance);
    RegisterField("directionalIrradiance", directionalIrradiance);
    RegisterField("lightDirection", lightDirection);
    RegisterField("valid", valid);
    RegisterField("version", version);
}

void BakedLightingData::DrawProperties(IEditorUi& ui)
{
    ui.DisabledLabel(valid
        ? "Generated baked irradiance (read-only)."
        : "No valid baked lighting data.");
    char value[128]{};
    snprintf(value, sizeof(value), "%.3f, %.3f, %.3f",
        irradiance.x, irradiance.y, irradiance.z);
    ui.ValueLabel("Irradiance", value);
    snprintf(value, sizeof(value), "%.3f, %.3f, %.3f",
        directionalIrradiance.x, directionalIrradiance.y,
        directionalIrradiance.z);
    ui.ValueLabel("Directional Irradiance", value);
    snprintf(value, sizeof(value), "%.3f, %.3f, %.3f",
        lightDirection.x, lightDirection.y, lightDirection.z);
    ui.ValueLabel("Light Direction", value);
    ui.ValueLabel("Bake Version", std::to_string(version).c_str());
}

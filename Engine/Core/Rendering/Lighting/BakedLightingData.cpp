#include "BakedLightingData.h"
#include "Engine/Editor/UI/IEditorUi.h"

namespace Engine::Rendering
{
BakedLightingData::BakedLightingData()
{
    SetTypeName(COMPONENT_TYPE_NAME(BakedLightingData));
    singlecomponent = true;
    RegisterField("irradiance", irradiance);
    RegisterField("directionalIrradiance", directionalIrradiance);
    RegisterField("lightDirection", lightDirection);
    RegisterField("originalMaterialAsset", originalMaterialAsset);
    RegisterField("originalMaterialSnapshot", originalMaterialSnapshot);
    RegisterField("bakedMaterialAsset", bakedMaterialAsset);
    RegisterField("bakedLightmapAsset", bakedLightmapAsset);
    RegisterField("valid", valid);
    RegisterField("version", version);
}

bool BakedLightingData::DrawProperties(::Engine::Editor::IEditorUi& ui)
{
    ui.DisabledLabel(valid
        ? "Generated baked material mapping (read-only)."
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
    ui.ValueLabel("Dominant Local Direction", value);
    ui.ValueLabel("Bake Version", std::to_string(version).c_str());
    ui.ValueLabel("Original Material", originalMaterialAsset.empty()
        ? originalMaterialSnapshot.c_str() : originalMaterialAsset.c_str());
    ui.ValueLabel("Baked Material", bakedMaterialAsset.c_str());
    ui.ValueLabel("Lightmap", bakedLightmapAsset.c_str());
    return false;
}
}
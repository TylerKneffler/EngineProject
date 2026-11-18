#include "Scripts/Grass/MeadowGrass.h"

#include "Core/Features/Grass/GrassField.h"
#include "Core/Components/Terrain/TerrainObjectLayer.h"
#include "Core/Rendering/DetailBudget.h"
#include "Core/Scene/Object.h"
#include "Core/Serialization/Json.h"

namespace MeadowGrass
{
bool IsMeadowLayer(const std::string& layerName)
{
    return layerName == "Meadow grass" || layerName == "Procedural meadow grass";
}

GrassField* Apply(Engine::Components::TerrainObjectLayer& layer,
    const std::string& viewerObjectName)
{
    if (!IsMeadowLayer(layer.layerName) || !layer.Owner) return nullptr;
    layer.snapToGround = true;
    if (viewerObjectName.empty()) return nullptr;
    auto* owner = layer.Owner;
    if (auto* existing = owner->GetComponent<GrassField>()) return existing;

    const bool procedural = layer.layerName == "Procedural meadow grass";
    if (procedural)
    {
        layer.conformToGround = true;
        layer.viewDistance = 35.f;
        layer.lod1Distance = 12.f;
        layer.lod2Distance = 25.f;
        layer.MarkConfigurationDirty();
    }
    // JsonValue(bool) is explicit: a bare bool converts to a number, which
    // bool fields do not read as true.
    using Engine::Serialization::JsonValue;
    const JsonValue proceduralValue(procedural);
    const JsonValue falseValue(false);
    // Procedural layers grow tussock patches (blades from shared crowns over
    // a low sward). Authored layers keep the scattered-blade patches their
    // placements name.
    const std::string shortPatch = procedural ? "meadow_tussock" : "meadow_short";
    const std::string tallPatch = procedural ? "field_tussock" : "field_tall";
    const auto grassMesh = [](const std::string& stem, const char* suffix) {
        return "Assets/Mesh/Grass/" + stem + suffix + ".obj";
    };
    auto settings = JsonValue::MakeObject();
    settings.Set("shortMesh", grassMesh(shortPatch, ""))
        .Set("tallMesh", grassMesh(tallPatch, ""))
        .Set("shortMeshLod1", grassMesh(shortPatch, "_lod1"))
        .Set("shortMeshLod2", grassMesh(shortPatch, "_lod2"))
        .Set("tallMeshLod1", grassMesh(tallPatch, "_lod1"))
        .Set("tallMeshLod2", grassMesh(tallPatch, "_lod2"))
        .Set("viewerObjectName", viewerObjectName)
        .Set("useEditorCamera", proceduralValue).Set("visibleRadius", 11.f)
        .Set("tileSize", 4.f).Set("shortPatchesPerSquareMeter", .5f)
        .Set("tallPatchesPerSquareMeter", .16f).Set("seed", 18731)
        .Set("maxTileBuildsPerUpdate", 8).Set("castShadows", falseValue)
        .Set("tileWorkBudgetMs", 2.f).Set("fadeDuration", .35f)
        .Set("distanceFadeWidth", 2.f).Set("retainDormantTiles", falseValue)
        .Set("allowProceduralGeneration", proceduralValue)
        // Wildflowers and weeds in drifts (procedural layers only; authored
        // layers draw accents placed with these assets).
        .Set("accentMeshes", "Assets/Mesh/Grass/meadow_daisy.obj;"
            "Assets/Mesh/Grass/meadow_buttercup.obj;Assets/Mesh/Grass/meadow_clover.obj;"
            "Assets/Mesh/Grass/meadow_plantain.obj;Assets/Mesh/Grass/meadow_knapweed.obj;"
            "Assets/Mesh/Grass/meadow_dandelion.obj")
        .Set("accentPatchesPerSquareMeter", .08f).Set("accentDriftSize", 7.f)
        // Low shrubs in thickets (procedural layers only, like accents).
        .Set("shrubMeshes", "Assets/Mesh/Grass/meadow_shrub.obj;"
            "Assets/Mesh/Grass/meadow_bramble.obj;Assets/Mesh/Grass/meadow_fern.obj")
        .Set("shrubPatchesPerSquareMeter", procedural ? .6f : 0.f)
        .Set("shrubThicketSize", 9.f).Set("shrubCoverage", .25f);
    auto* grass = new GrassField();
    grass->Deserialize(settings);
    owner->AddComponent(grass);
    // The manager picks the detail-budget layer before this hook runs.
    owner->layer = Engine::Rendering::DetailBudget::Get().FindLayerOrDefault("Grass");
    return grass;
}
}

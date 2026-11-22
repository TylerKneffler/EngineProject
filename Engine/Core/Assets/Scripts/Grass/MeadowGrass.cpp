#include "Scripts/Grass/MeadowGrass.h"

#include "Grass/GrassField.h"
#include "Core/Components/Terrain/TerrainObjectLayer.h"
#include "Core/Rendering/DetailBudget.h"
#include "Core/Scene/Object.h"
#include "Core/Serialization/Json.h"

namespace MeadowGrass
{
GrassField* ApplyFar(Engine::Components::TerrainObjectLayer& layer,
    const std::string& viewerObjectName)
{
    // The far ring: simple tufts on 64 m tiles from where the detailed
    // grass fades out to 200 m, so the meadow does not end at its range
    // (the reference meadows are unbroken for 100 m and more). Its reach is
    // not scaled by the detail budget (GrassField::OuterRadiusScale).
    layer.snapToGround = true;
    layer.conformToGround = true;
    if (viewerObjectName.empty()) return nullptr;
    auto* owner = layer.Owner;
    if (auto* existing = owner->GetComponent<GrassField>()) return existing;
    layer.viewDistance = 200.f;
    layer.lod1Distance = layer.lod2Distance = 200.f;
    layer.MarkConfigurationDirty();
    using Engine::Serialization::JsonValue;
    const std::string tuft = "Assets/Mesh/Grass/far_grass_tuft.obj";
    auto settings = JsonValue::MakeObject();
    settings.Set("shortMesh", tuft).Set("tallMesh", tuft)
        .Set("shortMeshLod1", tuft).Set("shortMeshLod2", tuft)
        .Set("tallMeshLod1", tuft).Set("tallMeshLod2", tuft)
        .Set("viewerObjectName", viewerObjectName)
        .Set("useEditorCamera", JsonValue(true)).Set("visibleRadius", 200.f)
        .Set("tileSize", 64.f).Set("shortPatchesPerSquareMeter", 1.1f)
        .Set("tallPatchesPerSquareMeter", 0.f).Set("seed", 29311)
        .Set("castShadows", JsonValue(false)).Set("tileWorkBudgetMs", 1.f)
        .Set("fadeDuration", .35f).Set("distanceFadeFraction", .25f)
        .Set("distanceThinning", 1.f).Set("innerRadius", 18.f).Set("innerFadeWidth", 7.f)
        .Set("windStrength", .6f).Set("groundTintStrength", 0.f)
        .Set("allowProceduralGeneration", JsonValue(true));
    auto* grass = new GrassField();
    grass->Deserialize(settings);
    owner->AddComponent(grass);
    owner->layer = Engine::Rendering::DetailBudget::Get().FindLayerOrDefault("Grass");
    return grass;
}

GrassField* ApplyForestFloor(Engine::Components::TerrainObjectLayer& layer,
    const std::string& viewerObjectName)
{
    // Under trees: fallen-leaf litter as the dense cover, broadleaf herbs as
    // the tall plants, seedling and dry-grass drifts, fern thickets and a few
    // bushes (tools/generate_meadow_grass.py, forest floor section).
    layer.snapToGround = true;
    layer.conformToGround = true;
    if (viewerObjectName.empty()) return nullptr;
    auto* owner = layer.Owner;
    if (auto* existing = owner->GetComponent<GrassField>()) return existing;
    layer.viewDistance = 35.f;
    layer.lod1Distance = 7.f;
    layer.lod2Distance = 13.f;
    layer.MarkConfigurationDirty();
    using Engine::Serialization::JsonValue;
    const auto mesh = [](const std::string& stem, const char* suffix) {
        return "Assets/Mesh/Grass/" + stem + suffix + ".obj";
    };
    auto settings = JsonValue::MakeObject();
    settings.Set("shortMesh", mesh("forest_litter", ""))
        .Set("shortMeshLod1", mesh("forest_litter", "_lod1"))
        .Set("shortMeshLod2", mesh("forest_litter", "_lod2"))
        .Set("tallMesh", mesh("forest_herb", ""))
        .Set("tallMeshLod1", mesh("forest_herb", "_lod1"))
        .Set("tallMeshLod2", mesh("forest_herb", "_lod2"))
        .Set("viewerObjectName", viewerObjectName)
        .Set("useEditorCamera", JsonValue(true)).Set("visibleRadius", 26.f)
        .Set("tileSize", 4.f).Set("shortPatchesPerSquareMeter", 2.2f)
        .Set("tallPatchesPerSquareMeter", 1.f).Set("seed", 40507)
        .Set("maxTileBuildsPerUpdate", 8).Set("castShadows", JsonValue(false))
        .Set("tileWorkBudgetMs", 2.f).Set("fadeDuration", .35f)
        .Set("distanceFadeFraction", .3f).Set("distanceThinning", .5f)
        .Set("retainDormantTiles", JsonValue(false))
        .Set("allowProceduralGeneration", JsonValue(true))
        // The soil keeps its own colour: no grass tint under the trees.
        .Set("groundTintStrength", 0.f)
        // Drifts of small things: seedlings, dry grass, fallen branches and
        // ground ivy.
        .Set("accentMeshes", mesh("forest_seedling", "") + ";" + mesh("dry_grass_clump", "") + ";" +
            mesh("meadow_plantain", "") + ";" + mesh("forest_branch", "") + ";" + mesh("ground_ivy", ""))
        .Set("accentPatchesPerSquareMeter", .2f).Set("accentDriftSize", 9.f)
        .Set("shrubMeshes", mesh("forest_fern", "") + ";" + mesh("forest_herb", ""))
        .Set("shrubPatchesPerSquareMeter", 1.f)
        .Set("shrubThicketSize", 10.f).Set("shrubCoverage", .45f)
        .Set("bushMeshes", mesh("meadow_bush", "") + ";" + mesh("meadow_bush_tall", ""))
        .Set("bushesPerSquareMeter", .03f)
        .Set("bushClusterSize", 20.f).Set("bushCoverage", .2f);
    auto* grass = new GrassField();
    grass->Deserialize(settings);
    owner->AddComponent(grass);
    owner->layer = Engine::Rendering::DetailBudget::Get().FindLayerOrDefault("Grass");
    return grass;
}

namespace
{
std::string ScanMesh(const char* stem) { return std::string("Assets/Mesh/Grass/") + stem + ".mesh"; }

// A field of scanned plants (card clusters baked from Poly Haven scans by
// tools/scan_bush_cards.py) with their shared alpha-masked material. The
// grass slots are required by the field but stay empty (zero density); the
// caller sets the shrubs and bushes.
GrassField* ApplyScanned(Engine::Components::TerrainObjectLayer& layer,
    const std::string& viewerObjectName, float reach, int seed,
    Engine::Serialization::JsonValue& settings)
{
    layer.snapToGround = true;
    layer.conformToGround = true;
    if (viewerObjectName.empty()) return nullptr;
    auto* owner = layer.Owner;
    if (auto* existing = owner->GetComponent<GrassField>()) return existing;
    layer.viewDistance = reach;
    layer.lod1Distance = 18.f;
    layer.lod2Distance = 40.f;
    layer.MarkConfigurationDirty();
    using Engine::Serialization::JsonValue;
    const std::string empty = "Assets/Mesh/Grass/far_grass_tuft.obj";
    settings.Set("shortMesh", empty).Set("tallMesh", empty)
        .Set("shortMeshLod1", empty).Set("shortMeshLod2", empty)
        .Set("tallMeshLod1", empty).Set("tallMeshLod2", empty)
        .Set("shortPatchesPerSquareMeter", 0.f).Set("tallPatchesPerSquareMeter", 0.f)
        .Set("materialPath", "Assets/Materials/Grass/scan_bush.material")
        .Set("viewerObjectName", viewerObjectName)
        .Set("useEditorCamera", JsonValue(true)).Set("visibleRadius", reach)
        .Set("tileSize", 16.f).Set("seed", seed)
        .Set("maxTileBuildsPerUpdate", 8).Set("castShadows", JsonValue(false))
        .Set("tileWorkBudgetMs", 1.f).Set("fadeDuration", .35f)
        .Set("distanceFadeFraction", .3f).Set("retainDormantTiles", JsonValue(false))
        .Set("windStrength", .6f).Set("groundTintStrength", 0.f)
        .Set("allowProceduralGeneration", JsonValue(true));
    auto* grass = new GrassField();
    grass->Deserialize(settings);
    owner->AddComponent(grass);
    owner->layer = Engine::Rendering::DetailBudget::Get().FindLayerOrDefault("Grass");
    return grass;
}
}

GrassField* ApplyBushes(Engine::Components::TerrainObjectLayer& layer,
    const std::string& viewerObjectName)
{
    // Bushes in loose clusters; shrub clumps, nettles, saplings and ferns in
    // thickets.
    auto settings = Engine::Serialization::JsonValue::MakeObject();
    settings.Set("shrubMeshes", ScanMesh("scan_shrub") + ";" + ScanMesh("scan_nettle") + ";" +
            ScanMesh("scan_sapling") + ";" + ScanMesh("scan_fern"))
        .Set("shrubPatchesPerSquareMeter", .5f)
        .Set("shrubThicketSize", 9.f).Set("shrubCoverage", .25f)
        .Set("bushMeshes", ScanMesh("scan_bush") + ";" + ScanMesh("scan_bush_tall"))
        .Set("bushesPerSquareMeter", .08f)
        .Set("bushClusterSize", 16.f).Set("bushCoverage", .25f);
    return ApplyScanned(layer, viewerObjectName, 80.f, 18731, settings);
}

GrassField* ApplyForestFerns(Engine::Components::TerrainObjectLayer& layer,
    const std::string& viewerObjectName)
{
    // Fern thickets under the trees (the forest floor's generated ferns had
    // 1 per m² in thickets of 10 m covering 45 %).
    auto settings = Engine::Serialization::JsonValue::MakeObject();
    settings.Set("shrubMeshes", ScanMesh("scan_fern_large") + ";" + ScanMesh("scan_fern"))
        .Set("shrubPatchesPerSquareMeter", .8f)
        .Set("shrubThicketSize", 10.f).Set("shrubCoverage", .45f);
    return ApplyScanned(layer, viewerObjectName, 50.f, 40507, settings);
}

bool IsMeadowLayer(const std::string& layerName)
{
    return layerName == "Meadow grass" || layerName == "Procedural meadow grass" ||
        layerName == FarLayerName || layerName == ForestFloorLayerName || layerName == BushLayerName ||
        layerName == FernLayerName;
}

GrassField* Apply(Engine::Components::TerrainObjectLayer& layer,
    const std::string& viewerObjectName)
{
    if (!IsMeadowLayer(layer.layerName) || !layer.Owner) return nullptr;
    if (layer.layerName == FarLayerName) return ApplyFar(layer, viewerObjectName);
    if (layer.layerName == ForestFloorLayerName) return ApplyForestFloor(layer, viewerObjectName);
    if (layer.layerName == BushLayerName) return ApplyBushes(layer, viewerObjectName);
    if (layer.layerName == FernLayerName) return ApplyForestFerns(layer, viewerObjectName);
    // Grass follows the ground through sculpting; procedural grass also
    // conforms per placement. Set on every restore (saved layers included).
    const bool procedural = layer.layerName == "Procedural meadow grass";
    layer.snapToGround = true;
    if (procedural) layer.conformToGround = true;
    if (viewerObjectName.empty()) return nullptr;
    auto* owner = layer.Owner;
    if (auto* existing = owner->GetComponent<GrassField>()) return existing;
    // Defaults for a new field only, so scene scripts can retune them.
    if (procedural)
    {
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
        // The ground under the blades reads as shaded undergrowth: a strong tint
        // toward the blade colour, darker at the roots (it had no effect while the
        // forest floor field cleared it; tuned 2026-11-20 against the reference meadow).
        .Set("groundTintStrength", .8f).Set("groundRootShade", .7f)
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
            "Assets/Mesh/Grass/meadow_bramble.obj;Assets/Mesh/Grass/meadow_fern.obj;"
            "Assets/Mesh/Grass/tall_grass_clump.obj")
        .Set("shrubPatchesPerSquareMeter", procedural ? .6f : 0.f)
        .Set("shrubThicketSize", 9.f).Set("shrubCoverage", .25f)
        // Solid bushes in loose clusters (procedural layers only).
        .Set("bushMeshes", "Assets/Mesh/Grass/meadow_bush.obj;Assets/Mesh/Grass/meadow_bush_tall.obj")
        .Set("bushesPerSquareMeter", procedural ? .06f : 0.f)
        .Set("bushClusterSize", 16.f).Set("bushCoverage", .25f);
    auto* grass = new GrassField();
    grass->Deserialize(settings);
    owner->AddComponent(grass);
    // The manager picks the detail-budget layer before this hook runs.
    owner->layer = Engine::Rendering::DetailBudget::Get().FindLayerOrDefault("Grass");
    return grass;
}
}

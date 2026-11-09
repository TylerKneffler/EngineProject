#include "TerrainManager.h"
#include "TerrainObjectLayer.h"
#include "Core/Object.h"
#include "Core/Scene/Scene.h"
#include "Core/Compoonents/Terrain/TerrainBatchAppearance.h"
#include "Core/Compoonents/Camera/Camera.h"
#include "Core/Compoonents/Mesh/Mesh.h"
#include "Core/Compoonents/Materials/Material.h"
#include "Core/Compoonents/Materials/Texture.h"
#include "Core/Compoonents/Physics/MeshObjectCollider.h"
#include "Core/Compoonents/Physics/RigidBody.h"
#include "Engine/Editor/UI/IEditorUi.h"
#include "Core/Serialization/SceneSerializer.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <type_traits>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <set>
#include <stdexcept>

namespace Engine::Components
{
TerrainManager::JsonValue TerrainManager::Serialize() const
{
    JsonValue out = Component::Serialize();
    out.Set("cellsX", JsonValue(cellsX)).Set("cellsY", JsonValue(cellsY))
        .Set("cellWidth", JsonValue(cellWidth)).Set("cellDepth", JsonValue(cellDepth))
        .Set("loadedRadius", JsonValue(loadedRadius))
        .Set("editorRadius", JsonValue(editorRadius))
        .Set("farRadius", JsonValue(farRadius))
        .Set("lod2Radius", JsonValue(lod2Radius))
        .Set("lod4Radius", JsonValue(lod4Radius))
        .Set("maxLodChangesPerUpdate", JsonValue(maxLodChangesPerUpdate))
        .Set("maxLoadsPerUpdate", JsonValue(maxLoadsPerUpdate))
        .Set("maxUnloadsPerUpdate", JsonValue(maxUnloadsPerUpdate))
        .Set("asyncStreaming", JsonValue(asyncStreaming))
        .Set("maxPreparedCells", JsonValue(maxPreparedCells))
        .Set("maxUploadsPerUpdate", JsonValue(maxUploadsPerUpdate))
        .Set("maxFarRebuildMilliseconds", JsonValue(maxFarRebuildMilliseconds))
        .Set("farTileScreenErrorPixels", JsonValue(farTileScreenErrorPixels))
        .Set("farTileErrorPixelsLod2", JsonValue(farTileErrorPixelsLod2))
        .Set("farTileErrorPixelsLod4", JsonValue(farTileErrorPixelsLod4))
        .Set("farTileErrorPixelsLod8", JsonValue(farTileErrorPixelsLod8))
        .Set("farTileErrorPixelsLod16", JsonValue(farTileErrorPixelsLod16))
        .Set("maxFarViewDistanceMeters", JsonValue(maxFarViewDistanceMeters))
        .Set("farTileLodHysteresis", JsonValue(farTileLodHysteresis))
        .Set("farVegetationImpostors", JsonValue(farVegetationImpostors))
        .Set("farVegetationClusterSpanMeters",
            JsonValue(farVegetationClusterSpanMeters))
        .Set("farVegetationImpostorBudgetPerTile",
            JsonValue(farVegetationImpostorBudgetPerTile))
        .Set("batchFarCells", JsonValue(batchFarCells))
        .Set("fastRuntimeLighting", JsonValue(fastRuntimeLighting))
        .Set("stochasticTerrainVariation", JsonValue(stochasticTerrainVariation))
        .Set("terrainMacroColorEnabled", JsonValue(terrainMacroColorEnabled))
        .Set("measurementGrid", JsonValue(measurementGrid))
        .Set("generateColliders", JsonValue(generateColliders))
        .Set("streamCollidersByDistance",
            JsonValue(streamCollidersByDistance))
        .Set("manageAuthoredCellColliders", JsonValue(manageAuthoredCellColliders))
        .Set("streamedPrefabPath", JsonValue(streamedPrefabPath))
        .Set("reuseStreamedPrefabCells", JsonValue(reuseStreamedPrefabCells))
        .Set("streamedPrefabCellName", JsonValue(streamedPrefabCellName))
        .Set("streamedPrefabHalfExtent", JsonValue(streamedPrefabHalfExtent))
        .Set("streamedPrefabRadius", JsonValue(streamedPrefabRadius))
        .Set("colliderActivationDistance", JsonValue(colliderActivationDistance))
        .Set("cpuBudgetMiB", JsonValue(cpuBudgetMiB))
        .Set("gpuBudgetMiB", JsonValue(gpuBudgetMiB))
        .Set("cacheBudgetMiB", JsonValue(cacheBudgetMiB))
        .Set("viewerObjectName", JsonValue(viewerObjectName))
        .Set("cellDataDirectory", JsonValue(cellDataDirectory))
        .Set("runtimeBakeDirectory", JsonValue(runtimeBakeDirectory));
    out.Set("requireRuntimeBake", JsonValue(requireRuntimeBake));
    JsonValue objectLayerNames = JsonValue::MakeArray();
    for (const auto& name : m_objectLayerNames)
        objectLayerNames.Push(JsonValue(name));
    out.Set("objectLayerNames", std::move(objectLayerNames));
    JsonValue objectLayerCells = JsonValue::MakeArray();
    for (const auto& [cell, stored] : m_objectLayers)
    {
        JsonValue layers = stored;
        if (const auto loaded = m_loaded.find(cell); loaded != m_loaded.end())
        {
            layers = JsonValue::MakeArray();
            if (loaded->second && loaded->second->Owner)
                for (const auto* child : loaded->second->Owner->Children)
                    if (child)
                        if (const auto* layer = child->GetComponent<TerrainObjectLayer>())
                            layers.Push(layer->Serialize());
        }
        if (layers.ArraySize())
            objectLayerCells.Push(JsonValue::MakeObject()
                .Set("x", JsonValue(cell.first))
                .Set("y", JsonValue(cell.second))
                .Set("layers", std::move(layers)));
    }
    for (const auto& [cell, loaded] : m_loaded)
        if (!m_objectLayers.count(cell) && loaded && loaded->Owner)
        {
            JsonValue layers = JsonValue::MakeArray();
            for (const auto* child : loaded->Owner->Children)
                if (child)
                    if (const auto* layer = child->GetComponent<TerrainObjectLayer>())
                        layers.Push(layer->Serialize());
            if (layers.ArraySize())
                objectLayerCells.Push(JsonValue::MakeObject()
                    .Set("x", JsonValue(cell.first))
                    .Set("y", JsonValue(cell.second))
                    .Set("layers", std::move(layers)));
        }
    out.Set("objectLayerCells", std::move(objectLayerCells));
    JsonValue cells = JsonValue::MakeArray();
    for (const auto& [cell, data] : m_cells)
    {
        const auto found = m_loaded.find(cell);
        cells.Push(JsonValue::MakeObject().Set("x", JsonValue(cell.first))
            .Set("y", JsonValue(cell.second))
            .Set("terrain", found == m_loaded.end() ? data : found->second->Serialize()));
    }
    std::set<Cell> inlineFallback;
    const auto dirtyEdits = m_dirtyProceduralEdits;
    for (const Cell cell : dirtyEdits)
    {
        const auto found = m_loaded.find(cell);
        if (found == m_loaded.end() || SaveEdit(cell, *found->second)) continue;
        inlineFallback.insert(cell);
        cells.Push(JsonValue::MakeObject().Set("x", JsonValue(cell.first))
            .Set("y", JsonValue(cell.second))
            .Set("terrain", found->second->Serialize()));
    }
    std::map<int, std::vector<int>> rows;
    for (const Cell cell : m_proceduralCells)
        if (!inlineFallback.count(cell)) rows[cell.second].push_back(cell.first);
    JsonValue runs = JsonValue::MakeArray();
    for (auto& [row, columns] : rows)
    {
        std::sort(columns.begin(), columns.end());
        for (size_t i = 0; i < columns.size(); )
        {
            const int start = columns[i];
            int end = start;
            while (++i < columns.size() && columns[i] == end + 1) end = columns[i];
            runs.Push(JsonValue::MakeObject().Set("y", JsonValue(row))
                .Set("start", JsonValue(start)).Set("end", JsonValue(end)));
        }
    }
    JsonValue edits = JsonValue::MakeArray();
    for (const Cell cell : m_editedProcedural)
        if (!inlineFallback.count(cell))
            edits.Push(JsonValue::MakeObject().Set("x", JsonValue(cell.first))
                .Set("y", JsonValue(cell.second)));
    out.Set("cells", std::move(cells))
        .Set("proceduralRuns", std::move(runs))
        .Set("editedCells", std::move(edits));
    return out;
}

void TerrainManager::Deserialize(const JsonValue& value)
{
    for (auto& [cell, object] : m_streamedPrefabCells)
        if (Owner && Owner->GetScene()) Owner->GetScene()->RemoveObject(object);
    m_streamedPrefabCells.clear();
    for (auto* object : m_streamedPrefabPool)
        if (Owner && Owner->GetScene()) Owner->GetScene()->RemoveObject(object);
    m_streamedPrefabPool.clear();
    InvalidatePrepared();
    m_runtimeBakeActive = false;
    m_bakeIndex.clear();
    m_editorStreaming = false;
    m_haveLastCenter = false;
    for (auto& [group, batch] : m_renderBatches)
        if (Owner && Owner->GetScene()) Owner->GetScene()->RemoveObject(batch.object);
    m_renderBatches.clear();
    for (auto it = m_loaded.begin(); it != m_loaded.end(); )
    { Cell cell = it->first; ++it; UnloadCell(cell); }
    Component::Deserialize(value);
    cellsX = std::clamp(value["cellsX"].AsInt(), 1, 512);
    cellsY = std::clamp(value["cellsY"].AsInt(), 1, 512);
    cellWidth = std::max(.001f, value["cellWidth"].AsFloat());
    cellDepth = std::max(.001f, value["cellDepth"].AsFloat());
    loadedRadius = std::clamp(value["loadedRadius"].AsInt(), 0, 32);
    editorRadius = value.Has("editorRadius")
        ? std::clamp(value["editorRadius"].AsInt(), 0, 32) : 6;
    farRadius = value.Has("farRadius") ? std::clamp(value["farRadius"].AsInt(), -1, 32) : 8;
    lod2Radius = value.Has("lod2Radius") ? std::clamp(value["lod2Radius"].AsInt(), 0, 32) : 8;
    lod4Radius = value.Has("lod4Radius") ? std::clamp(value["lod4Radius"].AsInt(), 0, 32) : 16;
    maxLodChangesPerUpdate = value.Has("maxLodChangesPerUpdate")
        ? std::clamp(value["maxLodChangesPerUpdate"].AsInt(), 1, 64) : 4;
    maxLoadsPerUpdate = value.Has("maxLoadsPerUpdate")
        ? std::clamp(value["maxLoadsPerUpdate"].AsInt(), 1, 64) : 2;
    maxUnloadsPerUpdate = value.Has("maxUnloadsPerUpdate")
        ? std::clamp(value["maxUnloadsPerUpdate"].AsInt(), 1, 64) : 4;
    asyncStreaming = value.Has("asyncStreaming") && value["asyncStreaming"].AsBool();
    maxPreparedCells = value.Has("maxPreparedCells")
        ? std::clamp(value["maxPreparedCells"].AsInt(), 1, 64) : 4;
    maxUploadsPerUpdate = value.Has("maxUploadsPerUpdate")
        ? std::clamp(value["maxUploadsPerUpdate"].AsInt(), 1, 64) : 2;
    maxFarRebuildMilliseconds = value.Has("maxFarRebuildMilliseconds")
        ? std::clamp(value["maxFarRebuildMilliseconds"].AsFloat(), .001f, 100.f)
        : 4.f;
    farTileScreenErrorPixels = value.Has("farTileScreenErrorPixels")
        ? std::clamp(value["farTileScreenErrorPixels"].AsFloat(), .1f, 64.f) : 3.f;
    const auto levelError = [&](const char* name) {
        return value.Has(name) ? std::clamp(value[name].AsFloat(), -1.f, 64.f)
            : -1.f;
    };
    farTileErrorPixelsLod2 = levelError("farTileErrorPixelsLod2");
    farTileErrorPixelsLod4 = levelError("farTileErrorPixelsLod4");
    farTileErrorPixelsLod8 = levelError("farTileErrorPixelsLod8");
    farTileErrorPixelsLod16 = levelError("farTileErrorPixelsLod16");
    maxFarViewDistanceMeters = value.Has("maxFarViewDistanceMeters")
        ? std::clamp(value["maxFarViewDistanceMeters"].AsFloat(), 0.f,
            1000000.f) : 0.f;
    farTileLodHysteresis = value.Has("farTileLodHysteresis")
        ? std::clamp(value["farTileLodHysteresis"].AsFloat(), 0.f, .9f) : .2f;
    farVegetationImpostors = !value.Has("farVegetationImpostors") ||
        value["farVegetationImpostors"].AsBool();
    farVegetationClusterSpanMeters = value.Has("farVegetationClusterSpanMeters")
        ? std::clamp(value["farVegetationClusterSpanMeters"].AsFloat(), 2.f, 256.f)
        : 24.f;
    farVegetationImpostorBudgetPerTile =
        value.Has("farVegetationImpostorBudgetPerTile")
            ? std::clamp(value["farVegetationImpostorBudgetPerTile"].AsInt(),
                0, 2048)
            : 96;
    batchFarCells = !value.Has("batchFarCells") || value["batchFarCells"].AsBool();
    fastRuntimeLighting = value.Has("fastRuntimeLighting") &&
        value["fastRuntimeLighting"].AsBool();
    stochasticTerrainVariation = value.Has("stochasticTerrainVariation") &&
        value["stochasticTerrainVariation"].AsBool();
    terrainMacroColorEnabled = value.Has("terrainMacroColorEnabled") &&
        value["terrainMacroColorEnabled"].AsBool();
    measurementGrid = value.Has("measurementGrid") &&
        value["measurementGrid"].AsBool();
    generateColliders = value.Has("generateColliders") &&
        value["generateColliders"].AsBool();
    streamCollidersByDistance = !value.Has("streamCollidersByDistance") ||
        value["streamCollidersByDistance"].AsBool();
    manageAuthoredCellColliders = value.Has("manageAuthoredCellColliders") &&
        value["manageAuthoredCellColliders"].AsBool();
    streamedPrefabPath = value.Has("streamedPrefabPath")
        ? value["streamedPrefabPath"].AsString() : std::string{};
    reuseStreamedPrefabCells = value.Has("reuseStreamedPrefabCells") &&
        value["reuseStreamedPrefabCells"].AsBool();
    streamedPrefabCellName = value.Has("streamedPrefabCellName")
        ? value["streamedPrefabCellName"].AsString() : "Terrain Cell";
    streamedPrefabHalfExtent = value.Has("streamedPrefabHalfExtent")
        ? std::clamp(value["streamedPrefabHalfExtent"].AsInt(), 0, 128) : 0;
    streamedPrefabRadius = value.Has("streamedPrefabRadius")
        ? std::clamp(value["streamedPrefabRadius"].AsInt(), 0, 8) : 1;
    colliderActivationDistance = value.Has("colliderActivationDistance")
        ? std::clamp(value["colliderActivationDistance"].AsFloat(), 0.f, 100.f) : 1.f;
    m_authoredCollisionRevision = 0;
    m_authoredCollisionCells.clear();
    m_collisionViewerLocal.reset();
    cpuBudgetMiB = value.Has("cpuBudgetMiB")
        ? std::clamp(value["cpuBudgetMiB"].AsInt(), 1, 16384) : 512;
    gpuBudgetMiB = value.Has("gpuBudgetMiB")
        ? std::clamp(value["gpuBudgetMiB"].AsInt(), 1, 16384) : 256;
    cacheBudgetMiB = value.Has("cacheBudgetMiB")
        ? std::clamp(value["cacheBudgetMiB"].AsInt(), 1, 16384) : 1024;
    viewerObjectName = value["viewerObjectName"].AsString();
    cellDataDirectory = value.Has("cellDataDirectory")
        ? value["cellDataDirectory"].AsString() : std::string{};
    runtimeBakeDirectory = value.Has("runtimeBakeDirectory")
        ? value["runtimeBakeDirectory"].AsString() : std::string{};
    requireRuntimeBake = value.Has("requireRuntimeBake") &&
        value["requireRuntimeBake"].AsBool();
    m_cells.clear();
    m_objectLayerNames.clear();
    const auto& objectLayerNames = value["objectLayerNames"];
    for (size_t i = 0; i < objectLayerNames.ArraySize(); ++i)
    {
        const std::string name = objectLayerNames.ArrayAt(i).AsString();
        if (!name.empty() && std::find(m_objectLayerNames.begin(),
            m_objectLayerNames.end(), name) == m_objectLayerNames.end())
            m_objectLayerNames.push_back(name);
    }
    m_objectLayers.clear();
    const auto& objectLayerCells = value["objectLayerCells"];
    for (size_t i = 0; i < objectLayerCells.ArraySize(); ++i)
    {
        const auto& entry = objectLayerCells.ArrayAt(i);
        if (entry["layers"].IsArray())
            m_objectLayers[{entry["x"].AsInt(), entry["y"].AsInt()}] =
                entry["layers"];
        const auto& layers = entry["layers"];
        for (size_t j = 0; j < layers.ArraySize(); ++j)
        {
            const std::string name = layers.ArrayAt(j)["layerName"].AsString();
            if (!name.empty() && std::find(m_objectLayerNames.begin(),
                m_objectLayerNames.end(), name) == m_objectLayerNames.end())
                m_objectLayerNames.push_back(name);
        }
    }
    m_proceduralCells.clear();
    m_editedProcedural.clear();
    m_dirtyProceduralEdits.clear();
    const auto& entries = value["cells"];
    for (size_t i = 0; i < entries.ArraySize(); ++i)
    {
        const auto& entry = entries.ArrayAt(i);
        m_cells[{entry["x"].AsInt(), entry["y"].AsInt()}] = entry["terrain"];
    }
    const auto& runs = value["proceduralRuns"];
    for (size_t i = 0; i < runs.ArraySize(); ++i)
    {
        const auto& entry = runs.ArrayAt(i);
        const int y = entry["y"].AsInt();
        const int start = entry["start"].AsInt();
        const int end = entry["end"].AsInt();
        if (end < start || static_cast<int64_t>(end) - start > 1000000) continue;
        for (int64_t x = start; x <= end; ++x)
        {
            const Cell cell{static_cast<int>(x), y};
            if (!m_cells.count(cell)) m_proceduralCells.insert(cell);
        }
    }
    const auto& legacy = value["proceduralCells"];
    for (size_t i = 0; i < legacy.ArraySize(); ++i)
    {
        const auto& entry = legacy.ArrayAt(i);
        const Cell cell{entry["x"].AsInt(), entry["y"].AsInt()};
        if (!m_cells.count(cell)) m_proceduralCells.insert(cell);
    }
    const auto& edits = value["editedCells"];
    for (size_t i = 0; i < edits.ArraySize(); ++i)
    {
        const auto& entry = edits.ArrayAt(i);
        const Cell cell{entry["x"].AsInt(), entry["y"].AsInt()};
        if (m_proceduralCells.count(cell)) m_editedProcedural.insert(cell);
    }
}

void TerrainManager::OnAfterDeserialize(IGraphicsProvider* provider)
{
    m_provider = nullptr;
    if (provider && !DeferGraphicsPreparation(provider))
        PrepareGraphicsResources(provider);
    if (manageAuthoredCellColliders) return;
    if (asyncStreaming)
    {
        // The far-height grid may not be configured until after this component
        // is deserialized. Defer the larger fallback ring until normal updates.
        StreamCells(0, 0, loadedRadius);
        return;
    }
    if (farRadius <= loadedRadius)
        SyncLoadedCells(0, 0);
    else
    {
        const int radius = std::clamp(loadedRadius, 0, 32);
        for (int y = -radius; y <= radius; ++y)
            for (int x = -radius; x <= radius; ++x)
                if (HasCell(x, y)) LoadCell({x, y});
    }
}

void TerrainManager::PrepareGraphicsResources(IGraphicsProvider* provider)
{
    m_provider = provider;
}

bool TerrainManager::DrawProperties(::Engine::Editor::IEditorUi& ui)
{
    bool changed = false;
    const int previousX = cellsX, previousY = cellsY;
    const float previousWidth = cellWidth, previousDepth = cellDepth;
    const bool priorAsync = asyncStreaming;
    if (ui.CollapsingHeader("View distance and quality"))
    {
        changed |= ui.DragFloat("View distance (m)",
            &maxFarViewDistanceMeters, 10.f, 0.f, 1000000.f);
        ui.Tooltip("Maximum distant terrain draw distance, capped by the camera far clip. Zero uses the camera far clip. Applies to distant terrain tiles; cell loading is controlled below.");
        changed |= ui.SliderFloat("Terrain quality (pixel error)",
            &farTileScreenErrorPixels, .1f, 64.f);
        ui.Tooltip("Lower values preserve more distant terrain detail and cost more to render. Higher values switch to simpler terrain sooner. Per-LOD overrides are under Advanced terrain LOD.");
        changed |= ui.SliderInt("Full detail distance (cells)", &loadedRadius, 0, 32);
        ui.Tooltip("Full-resolution terrain and interactive cells around the viewer. Distances are square grid radii: one cell spans Cells X * Cell width by Cells Y * Cell depth.");
        changed |= ui.SliderInt("Terrain loading distance (cells)", &farRadius, -1, 32);
        ui.Tooltip("Loaded terrain radius when distant terrain tiles are unavailable. -1 follows full detail distance. When distant tiles are active, only full-detail cells are loaded.");
        changed |= ui.SliderInt("Medium detail ends (cells)", &lod2Radius, 0, 32);
        ui.Tooltip("Loaded cells beyond full detail use half-resolution terrain (LOD 2) through this radius. Applies when distant terrain tiles are unavailable.");
        changed |= ui.SliderInt("Low detail ends (cells)", &lod4Radius, 0, 32);
        ui.Tooltip("Loaded cells beyond medium detail use quarter-resolution terrain (LOD 4) through this radius, then eighth-resolution terrain. Keep this at least as large as the medium detail radius.");
        changed |= ui.SliderInt("Editor detail distance (cells)", &editorRadius, 0, 32);
        ui.Tooltip("Editable full-resolution cell radius around the Scene view camera, separate from play mode.");
        ui.Label("Object view distance, LOD distances and shadow distance: Terrain Object Layer.");
        ui.Label("Shadow quality and global shadow range: Preferences > Rendering > Realtime Shadows.");
    }
    if (ui.CollapsingHeader("Advanced streaming and memory", false))
    {
        changed |= ui.SliderInt("LOD changes per update", &maxLodChangesPerUpdate, 1, 64);
        changed |= ui.SliderInt("Loads per update", &maxLoadsPerUpdate, 1, 64);
        changed |= ui.SliderInt("Unloads per update", &maxUnloadsPerUpdate, 1, 64);
        changed |= ui.Checkbox("Async streaming", &asyncStreaming);
        changed |= ui.SliderInt("Prepared cell limit", &maxPreparedCells, 1, 64);
        changed |= ui.SliderInt("Uploads per update", &maxUploadsPerUpdate, 1, 64);
        changed |= ui.SliderFloat("Far rebuild budget (ms)",
            &maxFarRebuildMilliseconds, .001f, 100.f);
        changed |= ui.SliderInt("CPU terrain budget MiB", &cpuBudgetMiB, 1, 16384);
        changed |= ui.SliderInt("GPU terrain budget MiB", &gpuBudgetMiB, 1, 16384);
        changed |= ui.SliderInt("Mesh cache budget MiB", &cacheBudgetMiB, 1, 16384);
        changed |= ui.Checkbox("Batch far terrain cells", &batchFarCells);
    }
    if (ui.CollapsingHeader("Advanced terrain LOD", false))
    {
        ui.Label("Pixel error overrides: -1 uses Terrain quality; lower values retain more detail.");
        changed |= ui.SliderFloat("LOD 2 error (pixels, -1 inherits)",
            &farTileErrorPixelsLod2, -1.f, 64.f);
        changed |= ui.SliderFloat("LOD 4 error (pixels, -1 inherits)",
            &farTileErrorPixelsLod4, -1.f, 64.f);
        changed |= ui.SliderFloat("LOD 8 error (pixels, -1 inherits)",
            &farTileErrorPixelsLod8, -1.f, 64.f);
        changed |= ui.SliderFloat("LOD 16+ error (pixels, -1 inherits)",
            &farTileErrorPixelsLod16, -1.f, 64.f);
        changed |= ui.SliderFloat("Far tile LOD hysteresis",
            &farTileLodHysteresis, 0.f, .9f);
        ui.Tooltip("Stabilizes detail changes as the camera moves. Larger values reduce switching near LOD boundaries.");
        changed |= ui.Checkbox("Far vegetation impostors",
            &farVegetationImpostors);
        changed |= ui.SliderFloat("Far vegetation cluster span (m)",
            &farVegetationClusterSpanMeters, 2.f, 256.f);
        changed |= ui.SliderInt("Far vegetation impostor budget per tile",
            &farVegetationImpostorBudgetPerTile, 0, 2048);
        ui.Tooltip("Clusters distant terrain-object vegetation into low-cost impostor billboards per far tile.");
    }
    if (ui.CollapsingHeader("Terrain appearance", false))
    {
        changed |= ui.Checkbox("Fast runtime terrain lighting", &fastRuntimeLighting);
        changed |= ui.Checkbox("Stochastic terrain tile variation",
            &stochasticTerrainVariation);
        changed |= ui.Checkbox("Terrain macro color blend",
            &terrainMacroColorEnabled);
        changed |= ui.Checkbox("Measurement grid (metres and cell borders)",
            &measurementGrid);
    }
    if (ui.CollapsingHeader("Terrain collision", false))
    {
        changed |= ui.Checkbox("Generate terrain cell colliders",
            &generateColliders);
        changed |= ui.Checkbox("Enable only near terrain colliders",
            &streamCollidersByDistance);
        changed |= ui.Checkbox("Manage authored cell colliders", &manageAuthoredCellColliders);
        if (!streamedPrefabPath.empty())
        {
            ui.ValueLabel("Streamed cell prefab", streamedPrefabPath.c_str());
            changed |= ui.Checkbox("Reuse streamed prefab cells", &reuseStreamedPrefabCells);
            changed |= ui.SliderInt("Streamed grid half extent", &streamedPrefabHalfExtent, 0, 128);
            changed |= ui.SliderInt("Streamed prefab radius", &streamedPrefabRadius, 0, 8);
            ui.ValueLabel("Loaded prefab cells", std::to_string(m_streamedPrefabCells.size()).c_str());
        }
        changed |= ui.DragFloat("Collider edge distance (m)", &colliderActivationDistance, .05f, 0.f, 100.f);
        ui.ValueLabel("Active terrain collider cells", std::to_string(m_activeColliderCells).c_str());
    }
    if (ui.CollapsingHeader("Terrain layout and baking", false))
    {
        changed |= ui.SliderInt("Cells X", &cellsX, 1, 512);
        changed |= ui.SliderInt("Cells Y", &cellsY, 1, 512);
        changed |= ui.DragFloat("Cell width (m)", &cellWidth, .1f, .001f, 1000.f);
        changed |= ui.DragFloat("Cell depth (m)", &cellDepth, .1f, .001f, 1000.f);
        char bakeDirectory[512]{};
        std::memcpy(bakeDirectory, runtimeBakeDirectory.data(),
            std::min(runtimeBakeDirectory.size(), sizeof(bakeDirectory) - 1));
        if (ui.InputText("Runtime bake directory", bakeDirectory,
            sizeof(bakeDirectory)))
        {
            runtimeBakeDirectory = bakeDirectory;
            changed = true;
        }
        changed |= ui.Checkbox("Require runtime terrain bake", &requireRuntimeBake);
        if (ui.Button("Bake all terrain cells for runtime") &&
            Owner && Owner->GetScene() && !Owner->GetScene()->HasStarted())
        {
            if (runtimeBakeDirectory.empty())
            {
                runtimeBakeDirectory = !cellDataDirectory.empty()
                    ? cellDataDirectory + "/BakedCells"
                    : "Assets/TerrainBakes";
                changed = true;
            }
            changed |= BakeRuntimeTerrain();
        }
        if (!m_bakeStatus.empty()) ui.Label(m_bakeStatus.c_str());
    }
    if (priorAsync != asyncStreaming) InvalidatePrepared();
    if (cellsX != previousX || cellsY != previousY ||
        cellWidth != previousWidth || cellDepth != previousDepth)
    {
        InvalidatePrepared();
        for (auto& [cell, data] : m_cells)
        {
            if (auto* loaded = GetLoadedCell(cell.first, cell.second))
            {
                loaded->Resize(cellsX, cellsY, cellWidth, cellDepth);
                loaded->Owner->transform.position = {
                        static_cast<double>(cell.first) * cellsX * cellWidth,
                    0.0, static_cast<double>(cell.second) * cellsY * cellDepth};
                    data = loaded->Serialize();
                }
                else
                {
                    Terrain temporary;
                    temporary.Deserialize(data);
                    temporary.Resize(cellsX, cellsY, cellWidth, cellDepth);
                    data = temporary.Serialize();
                }
            }
        }
        if (ui.CollapsingHeader("Terrain cells and object layers", false))
        {
            if (ui.Button("Add center cell")) changed |= AddCell(0, 0);
            ui.Separator();
            ui.Label("Terrain object layers (one child per loaded cell)");
            char layerName[128]{};
            std::memcpy(layerName, m_newObjectLayerName.data(),
                std::min(m_newObjectLayerName.size(), sizeof(layerName) - 1));
            if (ui.InputText("New object layer", layerName, sizeof(layerName)))
            m_newObjectLayerName = layerName;
            if (ui.Button("Add object layer") && AddObjectLayer(m_newObjectLayerName))
            {
                m_newObjectLayerName = "Objects";
                changed = true;
            }
            for (size_t i = 0; i < m_objectLayerNames.size(); ++i)
            {
                const std::string name = m_objectLayerNames[i];
                ui.Label(name.c_str());
                ui.SameLine();
                const std::string button = "Remove##ObjectLayer" +
                std::to_string(i);
                if (ui.Button(button.c_str()))
                {
                    changed |= RemoveObjectLayer(name);
                    break;
                }
            }
        }
        if (changed) MarkConfigurationDirty();
        return changed;
    }

}

#include "Core/Rendering/CellHandover.h"
#include "Core/Components/Terrain/TerrainEvents.h"
#include "Core/Components/Terrain/TerrainManager.h"
#include "Core/Components/Terrain/TerrainObjectLayer.h"
#include "Core/Components/Terrain/TerrainObjectArchive.h"
#include "Core/Components/Terrain/TerrainSurfaceSample.h"
#include "Core/Scene/Object.h"
#include "Core/Rendering/DetailBudget.h"
#include "Core/Scene/Scene.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <algorithm>
#include <climits>
#include <map>
#include <cmath>
#include <thread>

// Far objects: the trees (and other LOD-chained placements) of cells beyond
// the loaded window, drawn as their last LOD level only. Cells are computed
// once on background threads (terrain heights and paint, then the object
// layers' saved or generated placements) and cached. On the main thread they
// are grouped into blocks of farObjectBlockCells^2 cells, each one
// TerrainObjectLayer that the prefab instancer turns into a few instanced
// draws. A block leaves out cells that are loaded at full detail, so near
// and far representations never overlap.
namespace Engine::Components
{
namespace
{
using Cell = TerrainManager::Cell;
using JsonValue = Engine::Serialization::JsonValue;

struct FarObjectInputs
{
    int cellsX = 0, cellsY = 0;
    float cellWidth = 1.f, cellDepth = 1.f;
    std::function<void(Cell, Terrain&)> builder;
    TerrainManager::ObjectLayerGenerator generator;
    std::vector<std::string> layerNames;
    // Distance the loaded cells reach; far cells all lie beyond it.
    float nearReach = 0.f;
    // Move placements onto the far tiles' two-triangle cells. Off when far
    // cells draw their own heights (TerrainManager::farCellDetailPixels).
    bool snapToFarSurface = true;
    // Ground queries of cells volume edits reach (ObjectLayerContext::field).
    std::shared_ptr<const TerrainField> field;
};

// ENGINE_FAR_OBJECT_SURFACE=0 keeps far placements on full-detail heights.
bool FarObjectSurfaceSnap()
{
    static const bool enabled = [] {
        const char* value = std::getenv("ENGINE_FAR_OBJECT_SURFACE");
        return !(value && value[0] == '0');
    }();
    return enabled;
}

// Far blocks draw where far tiles do. At their finest level a far tile is two
// triangles per cell between the cell's corner heights (the far grid holds
// each cell's full-detail corners; TerrainFarLodEdges splits each quad along
// (x+1, y)-(x, y+1)). Placements are generated on the full-detail surface, so
// a tree in a valley floated over the far terrain's chord and one on a ridge
// sank into it. Moves each placement by (far surface - full surface) at its
// root. Coarser far levels further out leave a residue within their screen
// error.
void SnapToFarSurface(const Terrain& terrain, std::vector<TerrainObjectLayer::Placement>& placements)
{
    const int columns = terrain.cellsX, rows = terrain.cellsY;
    if (columns <= 0 || rows <= 0 ||
        terrain.heights.size() != static_cast<size_t>(columns + 1) * (rows + 1)) return;
    const float width = columns * terrain.cellWidth, depth = rows * terrain.cellDepth;
    const float h00 = terrain.heights.front();
    const float h10 = terrain.heights[static_cast<size_t>(columns)];
    const float h01 = terrain.heights[static_cast<size_t>(rows) * (columns + 1)];
    const float h11 = terrain.heights.back();
    for (auto& placement : placements)
    {
        const float x = std::clamp(placement.position.x, 0.f, width);
        const float z = std::clamp(placement.position.z, 0.f, depth);
        float full = 0.f;
        if (!SampleTerrainSurface(terrain.heights, columns, rows, terrain.cellWidth,
                terrain.cellDepth, x, z, full, nullptr)) continue;
        const float u = x / width, v = z / depth;
        const float farHeight = u + v <= 1.f
            ? h00 + u * (h10 - h00) + v * (h01 - h00)
            : h11 + (1.f - u) * (h01 - h11) + (1.f - v) * (h10 - h11);
        placement.position.y += farHeight - full;
    }
}

struct FarCellInput
{
    Cell cell;
    bool hasSavedTerrain = false;
    JsonValue savedTerrain;
    JsonValue savedLayers;
    // Set when the layers come only from the object archive: the job reads
    // them (file read, inflate, JSON: ~0.8 ms a cell, which on the main thread
    // made every cell crossing 20-35 ms for 3-4 frames; todo.md Step 0).
    std::shared_ptr<const TerrainObjectArchive> archive;
};

std::vector<TerrainObjectLayer::Placement> FarPlacements(
    const FarObjectInputs& inputs, const FarCellInput& input,
    std::map<std::string, float>* cullDistances = nullptr)
{
    std::vector<TerrainObjectLayer::Placement> result;
    Engine::Core::Object terrainObject;
    auto* terrain = terrainObject.AddComponent<Terrain>();
    const bool needsCell = inputs.generator || inputs.snapToFarSurface;
    if (!needsCell) {}
    else if (input.hasSavedTerrain) terrain->Deserialize(input.savedTerrain);
    else if (inputs.builder)
    {
        terrain->Resize(inputs.cellsX, inputs.cellsY, inputs.cellWidth, inputs.cellDepth);
        inputs.builder(input.cell, *terrain);
    }
    else return result;

    // Saved layers win over generated ones of the same name, as in streaming.
    std::vector<std::pair<std::string, const JsonValue*>> layers;
    for (size_t i = 0; i < input.savedLayers.ArraySize(); ++i)
    {
        const auto& saved = input.savedLayers.ArrayAt(i);
        const auto name = saved["layerName"].AsString();
        if (!name.empty()) layers.emplace_back(name, &saved);
    }
    for (const auto& name : inputs.layerNames)
        if (!name.empty() && std::none_of(layers.begin(), layers.end(),
                [&](const auto& entry) { return entry.first == name; }))
            layers.emplace_back(name, nullptr);

    for (const auto& [name, saved] : layers)
    {
        Engine::Core::Object layerObject;
        auto* layer = layerObject.AddComponent<TerrainObjectLayer>();
        layer->layerName = name;
        if (saved) layer->Deserialize(*saved);
        else if (inputs.generator)
        {
            TerrainManager::ObjectLayerContext context;
            context.cell = input.cell;
            context.cellsX = terrain->cellsX;
            context.cellsY = terrain->cellsY;
            context.cellWidth = terrain->cellWidth;
            context.cellDepth = terrain->cellDepth;
            context.heights = &terrain->heights;
            context.weights = &terrain->weights;
            context.terrainLayerCount = terrain->layers.size();
            context.farObjects = true;
            if (inputs.field && inputs.field->HasEdits(input.cell)) context.field = inputs.field;
            inputs.generator(context, name, *layer);
        }
        // Ground cover (grass) has its own reach and no far representation;
        // layers kept near (farObjects off) neither.
        if (layer->conformToGround || layer->snapToGround || !layer->farObjects) continue;
        // Layers that fade out inside the loaded cells (rocks, logs, bushes)
        // can never show in a far cell.
        if (layer->viewDistance <= inputs.nearReach) continue;
        for (const auto& placement : layer->placements)
        {
            const auto profile = layer->GetObjectLodProfile(placement.asset);
            // Assets authored to vanish after their last level keep that
            // distance in the far block. Those that vanish inside the loaded
            // cells (near-only props) have no far representation; those
            // that reach further (cliff pieces, out to their layer's view
            // distance) would otherwise end at the edge of the loaded cells:
            // with 3 loaded rings an escarpment 560 m away was bare terrain.
            const bool culled = profile.cullAfterLastLod &&
                std::isfinite(profile.finalLodCullDistance) && profile.finalLodCullDistance > 0.f;
            if (culled && profile.finalLodCullDistance <= inputs.nearReach) continue;
            auto chain = profile.meshes;
            chain.erase(std::remove(chain.begin(), chain.end(), std::string{}), chain.end());
            // Only assets with a cheaper last level (impostors) draw far away,
            // or ones with their own reach (above): those draw as they are.
            if (chain.size() < 2 && !culled) continue;
            if (chain.empty()) chain.push_back(placement.asset);
            auto distant = placement;
            distant.asset = chain.back();
            if (culled && cullDistances)
            {
                auto& distance = (*cullDistances)[distant.asset];
                distance = std::max(distance, profile.finalLodCullDistance);
            }
            result.push_back(std::move(distant));
        }
    }
    if (inputs.snapToFarSurface)
        SnapToFarSurface(*terrain, result);
    return result;
}

// Far forest clusters (TerrainManager::farClusters). Block-local far
// placements are grouped on a square grid; a square holding enough trees, most
// of which match one cluster's text, becomes a single cluster impostor at the
// trees' centroid, scaled by the square's tree count against the baked count
// (so a stand keeps roughly its density). Trees of other kinds in that square,
// and every tree of sparse or mixed squares, stay individual impostors.
std::vector<TerrainObjectLayer::Placement> ClusterFarPlacements(
    const std::vector<TerrainObjectLayer::Placement>& placements,
    const std::vector<TerrainManager::FarCluster>& clusters, float size,
    int minimum, Cell block)
{
    std::vector<std::string> matches;
    for (const auto& cluster : clusters)
        if (!cluster.match.empty() &&
            std::find(matches.begin(), matches.end(), cluster.match) == matches.end())
            matches.push_back(cluster.match);
    std::map<std::pair<int64_t, int64_t>, std::vector<size_t>> squares;
    for (size_t i = 0; i < placements.size(); ++i)
        squares[{ static_cast<int64_t>(std::floor(placements[i].position.x / size)),
            static_cast<int64_t>(std::floor(placements[i].position.z / size)) }].push_back(i);
    std::vector<TerrainObjectLayer::Placement> result;
    result.reserve(placements.size() / 4 + squares.size());
    const auto matchOf = [&](const std::string& asset) -> int {
        for (size_t m = 0; m < matches.size(); ++m)
            if (asset.find(matches[m]) != std::string::npos) return static_cast<int>(m);
        return -1;
    };
    for (const auto& [square, members] : squares)
    {
        std::vector<int> family(members.size());
        std::vector<int> counts(matches.size(), 0);
        for (size_t k = 0; k < members.size(); ++k)
            if ((family[k] = matchOf(placements[members[k]].asset)) >= 0) ++counts[family[k]];
        const auto best = std::max_element(counts.begin(), counts.end());
        const int bestFamily = best == counts.end() ? -1 : static_cast<int>(best - counts.begin());
        const int count = bestFamily < 0 ? 0 : *best;
        // Mixed squares (no family above 70%) stay as individual trees.
        if (count < minimum || count * 10 < static_cast<int>(members.size()) * 7)
        {
            for (size_t index : members) result.push_back(placements[index]);
            continue;
        }
        std::vector<const TerrainManager::FarCluster*> candidates;
        for (const auto& cluster : clusters)
            if (cluster.match == matches[bestFamily]) candidates.push_back(&cluster);
        uint64_t hash = (static_cast<uint64_t>(block.first) * 0x9e3779b97f4a7c15ull) ^
            (static_cast<uint64_t>(block.second) * 0xc2b2ae3d27d4eb4full) ^
            (static_cast<uint64_t>(square.first) * 0x165667b19e3779f9ull) ^
            (static_cast<uint64_t>(square.second) * 0x27d4eb2f165667c5ull);
        hash ^= hash >> 31; hash *= 0x94d049bb133111ebull; hash ^= hash >> 29;
        const auto& cluster = *candidates[hash % candidates.size()];
        glm::vec3 centroid(0.f);
        for (size_t k = 0; k < members.size(); ++k)
        {
            if (family[k] == bestFamily) centroid += placements[members[k]].position;
            else result.push_back(placements[members[k]]);
        }
        TerrainObjectLayer::Placement stand;
        stand.asset = cluster.asset;
        stand.position = centroid / static_cast<float>(count);
        stand.normal = glm::vec3(0.f, 1.f, 0.f);
        stand.yawDegrees = static_cast<float>((hash >> 16) % 3600u) * .1f;
        stand.scale = std::clamp(std::sqrt(static_cast<float>(count) / cluster.trees), .85f, 1.2f);
        result.push_back(std::move(stand));
    }
    return result;
}

// A far block's inputs for BuildFarBlockPlacements: each member cell's offset
// in the block and its shared placements (FarObjectCell), and the clustering.
using BlockSources = std::vector<std::pair<glm::vec3,
    std::shared_ptr<const std::vector<TerrainObjectLayer::Placement>>>>;
struct ClusterSettings
{
    bool trees = true;        // keep the single trees
    bool clustered = false;   // make the forest clusters
    std::vector<TerrainManager::FarCluster> clusters;
    float size = 0.f;
    int minimum = 1;
    Cell block{};
};

// Thread-safe: reads only its arguments.
// first: single trees, second: forest clusters (TerrainManager::FarBlockBuilt).
std::pair<std::vector<TerrainObjectLayer::Placement>, std::vector<TerrainObjectLayer::Placement>>
BuildFarBlockPlacements(const BlockSources& sources,
    const std::set<std::string>& beyondCull, const ClusterSettings& settings)
{
    size_t total = 0;
    for (const auto& [offset, placements] : sources) total += placements->size();
    std::vector<TerrainObjectLayer::Placement> result;
    result.reserve(total);
    uint64_t id = 0;
    for (const auto& [offset, placements] : sources)
        for (const auto& source : *placements)
        {
            if (!beyondCull.empty() && beyondCull.count(source.asset)) continue;
            auto& placement = result.emplace_back(source);
            placement.position += offset;
            placement.id = ++id;
        }
    std::pair<std::vector<TerrainObjectLayer::Placement>, std::vector<TerrainObjectLayer::Placement>> built;
    if (settings.clustered)
        built.second = ClusterFarPlacements(result, settings.clusters, settings.size, settings.minimum, settings.block);
    if (settings.trees) built.first = std::move(result);
    return built;
}
}

void TerrainManager::RemoveFarBlockObjects(FarObjectBlock& block)
{
    auto* scene = Owner ? Owner->GetScene() : nullptr;
    if (scene && block.object) scene->RemoveObject(block.object);
    if (scene && block.clusterObject) scene->RemoveObject(block.clusterObject);
    block.object = block.clusterObject = nullptr;
    block.layer = block.clusterLayer = nullptr;
}

void TerrainManager::InstallFarBlockPlacements(FarObjectBlock& block,
    FarBlockBuilt built, const std::map<std::string, float>& cullDistances,
    const std::set<std::string>& beyondCull, uint64_t membership)
{
    // A form the block does not carry now: its object off, placements freed.
    const auto install = [&](Engine::Core::Object* object, TerrainObjectLayer* layer,
        std::vector<TerrainObjectLayer::Placement>& placements, bool wanted)
    {
        if (!object || !layer) return;
        object->enabled = wanted;
        if (!wanted && layer->placements.empty()) return;
        layer->objectLods.clear();
        for (const auto& [asset, distance] : cullDistances)
        {
            if (beyondCull.count(asset)) continue;
            auto& lod = layer->objectLods[asset];
            lod.cullAfterLastLod = true;
            lod.finalLodCullDistance = distance;
        }
        layer->placements = wanted ? std::move(placements) : std::vector<TerrainObjectLayer::Placement>();
        layer->MarkConfigurationDirty();
    };
    install(block.object, block.layer, built.trees, (block.forms & 1) != 0);
    install(block.clusterObject, block.clusterLayer, built.clusters, (block.forms & 2) != 0);
    block.membership = membership;
    block.prepared = false;
}

void TerrainManager::FenceFarObjectJobs()
{
    for (auto& job : m_farObjectJobs)
        if (job.result.valid()) job.result.wait();
    for (auto& [key, block] : m_farObjectBlocks)
        if (block.build.valid()) block.build.wait();
    for (auto& build : m_farBlockDraining)
        if (build.valid()) build.wait();
    m_farBlockDraining.clear();
    m_farObjectJobs.clear();
    m_farObjectPending.clear();
}

namespace
{
// ENGINE_NEAR_OBJECT_BLOCKS=<n> overrides nearObjectBlockCells (1 = per cell), for A/B runs.
int ForcedNearObjectBlocks()
{
    static const int forced = []
    {
        const char* setting = std::getenv("ENGINE_NEAR_OBJECT_BLOCKS");
        return setting ? std::clamp(std::atoi(setting), 1, 8) : 0;
    }();
    return forced;
}
}

void TerrainManager::ClearNearObjects()
{
    if (Owner && Owner->GetScene())
        for (auto& [key, block] : m_nearObjectBlocks)
            if (block.object) Owner->GetScene()->RemoveObject(block.object);
    m_nearObjectBlocks.clear();
}

bool TerrainManager::NearObjectsReady(Cell cell, const Terrain& terrain) const
{
    if (!terrain.Owner) return true;
    const int blockCells = m_editorStreaming ? 1 : ForcedNearObjectBlocks()
        ? ForcedNearObjectBlocks() : std::clamp(nearObjectBlockCells, 1, 8);
    const auto floorDiv = [](int value, int divisor) {
        return value >= 0 ? value / divisor : -((-value + divisor - 1) / divisor);
    };
    const Cell blockKey{floorDiv(cell.first, blockCells), floorDiv(cell.second, blockCells)};
    for (const auto* child : terrain.Owner->Children)
    {
        const auto* layer = child ? child->GetComponent<TerrainObjectLayer>() : nullptr;
        if (!layer || !layer->suppressPrefabRendering) continue;
        const auto found = m_nearObjectBlocks.find({blockKey, layer->layerName});
        if (found == m_nearObjectBlocks.end() || !found->second.prepared ||
            std::find(found->second.members.begin(), found->second.members.end(), cell) ==
                found->second.members.end()) return false;
    }
    return true;
}

// Near object blocks (TerrainManager::nearObjectBlockCells). Each loaded
// cell's prefab layers stop making instanced batches; one runtime layer per
// block and layer name holds their placements instead, with the member
// layers' settings. A block rebuilds when a member cell loads, unloads or
// changes its placements; the prefab instancer keeps the previous batches
// on screen until the new ones are ready, as it does for far blocks.
void TerrainManager::UpdateNearObjects()
{
    auto* scene = Owner ? Owner->GetScene() : nullptr;
    if (!scene) return;
    const int blockCells = m_editorStreaming ? 1 : ForcedNearObjectBlocks()
        ? ForcedNearObjectBlocks() : std::clamp(nearObjectBlockCells, 1, 8);
    const bool active = blockCells >= 2;
    const auto floorDiv = [](int value, int divisor) {
        return value >= 0 ? value / divisor : -((-value + divisor - 1) / divisor);
    };
    struct Member { Cell cell; TerrainObjectLayer* layer; Engine::Core::Object* object; };
    std::map<std::pair<Cell, std::string>, std::vector<Member>> wanted;
    for (const auto& [cell, terrain] : m_loaded)
    {
        if (!terrain || !terrain->Owner) continue;
        for (auto* child : terrain->Owner->Children)
        {
            auto* layer = child ? child->GetComponent<TerrainObjectLayer>() : nullptr;
            if (!layer) continue;
            // Ground cover has its own renderer; only prefab placements batch.
            const bool merge = active && !layer->conformToGround && !layer->snapToGround &&
                layer->HasPrefabPlacements();
            if (layer->suppressPrefabRendering != merge)
            {
                layer->suppressPrefabRendering = merge;
                layer->MarkConfigurationDirty();
            }
            if (merge)
                wanted[{Cell{floorDiv(cell.first, blockCells), floorDiv(cell.second, blockCells)},
                    layer->layerName}].push_back({cell, layer, child});
        }
    }
    for (auto it = m_nearObjectBlocks.begin(); it != m_nearObjectBlocks.end(); )
    {
        if (wanted.count(it->first)) { ++it; continue; }
        if (it->second.object) scene->RemoveObject(it->second.object);
        it = m_nearObjectBlocks.erase(it);
    }
    if (wanted.empty()) return;
    const double spanX = static_cast<double>(cellsX) * cellWidth;
    const double spanZ = static_cast<double>(cellsY) * cellDepth;
    auto* provider = scene->GetGraphicsProvider();
    for (auto& [key, members] : wanted)
    {
        uint64_t membership = 1469598103934665603ull;
        const auto mix = [&](uint64_t value) { membership = (membership ^ value) * 1099511628211ull; };
        for (const Member& member : members)
        {
            mix(static_cast<uint64_t>(static_cast<uint32_t>(member.cell.first)));
            mix(static_cast<uint64_t>(static_cast<uint32_t>(member.cell.second)));
            mix(reinterpret_cast<uintptr_t>(member.layer));
            mix(member.layer->GetConfigurationRevision());
            mix(member.layer->placements.size());
        }
        auto& block = m_nearObjectBlocks[key];
        if (block.object && block.membership == membership)
        {
            if (!block.prepared && block.layer && block.layer->IsPrefabPreparationComplete(provider))
                block.prepared = true;
            continue;
        }
        const Cell blockCell = key.first;
        ++TerrainEvents::nearBlockRebuilds;
        TerrainEvents::Log(scene->GetFrameIndex(), "near_block", blockCell.first, blockCell.second, -1);
        if (!block.object)
        {
            block.object = scene->AddObject("Near objects " + std::to_string(blockCell.first) + "," +
                std::to_string(blockCell.second) + ": " + key.second);
            block.object->runtimeOnly = true;
            scene->MoveObject(block.object, Owner, Engine::Scene::Scene::ObjectPlacement::AsChild);
            block.object->transform.position = {
                static_cast<double>(blockCell.first) * blockCells * spanX, 0.0,
                static_cast<double>(blockCell.second) * blockCells * spanZ};
            block.layer = block.object->AddComponent<TerrainObjectLayer>();
            block.layer->layerName = key.second;
            block.layer->placementsChangeOnlyWhenMarked = true;
            block.layer->suppressPrefabColliders = true;
            block.layer->handoverRole = 1;   // a cell's own objects (CellHandover)
        }
        // Settings and project layer follow the member layers (all cells set
        // a layer name up the same way).
        const TerrainObjectLayer& source = *members.front().layer;
        block.object->layer = members.front().object->layer;
        auto& target = *block.layer;
        target.viewDistance = source.viewDistance;
        target.lod1Distance = source.lod1Distance;
        target.lod2Distance = source.lod2Distance;
        target.lodHysteresis = source.lodHysteresis;
        target.screenLodSelection = source.screenLodSelection;
        target.distanceLodOverride = source.distanceLodOverride;
        target.screenLod1Pixels = source.screenLod1Pixels;
        target.screenLod2Pixels = source.screenLod2Pixels;
        target.screenLodHysteresis = source.screenLodHysteresis;
        target.shadowDistance = source.shadowDistance;
        target.farShadowDistance = source.farShadowDistance;
        target.shadowFadeWidth = source.shadowFadeWidth;
        target.shadowLod = source.shadowLod;
        target.objectLods.clear();
        std::vector<TerrainObjectLayer::Placement> placements;
        size_t total = 0;
        for (const Member& member : members) total += member.layer->placements.size();
        placements.reserve(total);
        block.members.clear();
        uint64_t memberIndex = 0;
        for (const Member& member : members)
        {
            block.members.push_back(member.cell);
            for (const auto& [asset, lods] : member.layer->objectLods)
                target.objectLods.emplace(asset, lods);
            const glm::vec3 offset(
                static_cast<float>((member.cell.first - blockCell.first * blockCells) * spanX), 0.f,
                static_cast<float>((member.cell.second - blockCell.second * blockCells) * spanZ));
            ++memberIndex;
            for (auto placement : member.layer->placements)
            {
                placement.position += offset;
                // Unique within the block; the cell's own id stays in the low bits.
                placement.id = (memberIndex << 48u) | (placement.id & 0xffffffffffffull);
                placements.push_back(std::move(placement));
            }
        }
        target.placements = std::move(placements);
        target.MarkConfigurationDirty();
        block.membership = membership;
        block.prepared = false;
    }
}

void TerrainManager::ClearFarObjects()
{
    FenceFarObjectJobs();
    m_farObjectCells.clear();
    m_farNearCovered.clear();
    for (auto& [key, block] : m_farObjectBlocks) RemoveFarBlockObjects(block);
    m_farObjectBlocks.clear();
    m_farScanCenter = { INT_MIN, INT_MIN };
    m_farScanRadius = -1;
    m_farScanCellCount = 0;
    m_farObjectQueue.clear();
    m_farObjectQueueNext = 0;
    m_farDirtyBlocks.clear();
    m_farUrgentBlocks.clear();
    m_handoverStart.clear();
    m_handoverAwaitBlock.clear();
}

// Rendering/CellHandover.h: the cells mid-handover, nearest first: fading
// cells at their progress, covered cells whose far block still draws them at
// 1, then loaded cells whose objects are not ready at 0 (their near objects
// stay hidden even if a block shows them a frame early).
void TerrainManager::PublishCellHandover(int centerX, int centerY)
{
    auto& handover = Engine::Rendering::CellHandover::Get();
    auto* scene = Owner ? Owner->GetScene() : nullptr;
    handover.scene = scene;
    handover.Clear();
    if (!scene) return;
    const float seconds = std::clamp(scene->settings.lodTransitionSeconds, 0.f, 2.f);
    const auto now = std::chrono::steady_clock::now();
    struct Entry { Cell cell; float progress; int rank; };
    std::vector<Entry> entries;
    for (const auto& [cell, started] : m_handoverStart)
        entries.push_back({cell, seconds > 0.f
            ? std::clamp(std::chrono::duration<float>(now - started).count() / seconds, 0.f, 1.f) : 1.f, 0});
    // Covered cells whose far block still draws them: hidden far impostors
    // (p = 1). Only while the cell is still covered (it leaves the grid when
    // it is handed back, and its block rebuilds with it at once).
    for (const auto& [cell, block] : m_handoverAwaitBlock)
        if (m_farNearCovered.count(cell)) entries.push_back({cell, 1.f, 0});
    // Only cells within the loaded radius, where a handover will start once
    // they are ready: a loaded cell further out (the unload margin, or
    // uncovered while loaded) never starts one, and listing it at 0 hid its
    // near objects for good, bushes and rocks with no far impostor included
    // (objects vanished, 2026-11-27). Unlisted, they draw.
    // And only cells a near block holds: otherwise no near object draws
    // there yet and there is nothing to hide (fewer entries: the list holds
    // MaximumCells).
    std::set<Cell> blockMembers;
    for (const auto& [key, block] : m_nearObjectBlocks)
        blockMembers.insert(block.members.begin(), block.members.end());
    for (const auto& [cell, terrain] : m_loaded)
        if (!m_farNearCovered.count(cell) && !m_handoverStart.count(cell) && blockMembers.count(cell) &&
            std::max(std::abs(cell.first - centerX), std::abs(cell.second - centerY)) <= std::max(0, loadedRadius))
            entries.push_back({cell, 0.f, 1});
    const auto distance = [&](Cell cell) {
        return std::max(std::abs(cell.first - centerX), std::abs(cell.second - centerY)); };
    std::sort(entries.begin(), entries.end(), [&](const Entry& a, const Entry& b) {
        return a.rank != b.rank ? a.rank < b.rank : distance(a.cell) < distance(b.cell); });
    // Square cells (the shader takes one span); the camera cell's corner.
    const glm::dvec3 origin = Owner->transform.GetWorldPositionD();
    const double span = static_cast<double>(cellsX) * cellWidth;
    handover.grid = glm::vec3(static_cast<float>(origin.x + centerX * span),
        static_cast<float>(origin.z + centerY * span), static_cast<float>(1.0 / span));
    TerrainEvents::handoverWanted = static_cast<uint32_t>(entries.size());
    uint32_t outside = 0;
    for (const Entry& entry : entries)
        if (!handover.Set(entry.cell.first - centerX, entry.cell.second - centerY, entry.progress)) ++outside;
    TerrainEvents::handoverDropped = outside;   // beyond the grid (a larger loaded radius + margin)
}

TerrainManager::FarObjectStatus TerrainManager::GetFarObjectStatus() const
{
    FarObjectStatus status;
    status.computedCells = m_farObjectCells.size();
    status.queuedCells = m_farObjectQueue.size() - std::min(m_farObjectQueueNext, m_farObjectQueue.size());
    status.pendingCells = m_farObjectPending.size();
    status.blocks = m_farObjectBlocks.size();
    for (const auto& [key, block] : m_farObjectBlocks)
        if (!block.prepared || block.build.valid()) ++status.preparingBlocks;
    if (std::clamp(farObjectRadius, 0, 64) <= 0 || m_editorStreaming) return status;
    // Not scanned yet (no update since enabling or a reset): nothing known.
    const bool scanned = m_farScanRadius == std::clamp(farObjectRadius, 0, 64);
    status.settled = scanned && !status.queuedCells && !status.pendingCells &&
        m_farObjectJobs.empty() && m_farDirtyBlocks.empty() && m_farUrgentBlocks.empty() &&
        !status.preparingBlocks;
    return status;
}

void TerrainManager::UpdateFarObjects(int centerX, int centerY)
{
    if (!Owner || !Owner->GetScene()) return;
    const int radius = std::clamp(farObjectRadius, 0, 64);
    // Placements depend on the cell sources, builder, generator and layers.
    const uint64_t signature = m_cacheSignature ^
        (static_cast<uint64_t>(m_objectLayerNames.size()) << 56) ^
        (m_objectLayerGenerator ? 0x9e3779b97f4a7c15ull : 0ull);
    if (radius <= 0 || m_editorStreaming || signature != m_farObjectSignature)
    {
        if (!m_farObjectCells.empty() || !m_farObjectBlocks.empty() ||
            !m_farObjectJobs.empty())
            ClearFarObjects();
        m_farObjectSignature = signature;
        if (radius <= 0 || m_editorStreaming) return;
    }

    const int blockCells = std::clamp(farObjectBlockCells, 1, 32);
    const auto floorDiv = [](int value, int divisor) {
        return value >= 0 ? value / divisor : -((-value + divisor - 1) / divisor);
    };
    const auto blockOf = [&](Cell cell) {
        return Cell{ floorDiv(cell.first, blockCells), floorDiv(cell.second, blockCells) };
    };
    const auto inRadius = [&](Cell cell) {
        return std::max(std::abs(cell.first - centerX), std::abs(cell.second - centerY)) <= radius;
    };

    // Collect finished jobs; their blocks have new data.
    for (auto it = m_farObjectJobs.begin(); it != m_farObjectJobs.end(); )
    {
        if (it->result.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
        { ++it; continue; }
        try
        {
            for (auto& [cell, data] : it->result.get())
                m_farObjectCells[cell] = std::move(data);
        }
        catch (...) {}
        for (const auto& cell : it->cells)
        {
            m_farObjectPending.erase(cell);
            m_farDirtyBlocks.insert(blockOf(cell));
        }
        it = m_farObjectJobs.erase(it);
    }

    const auto scanStart = std::chrono::steady_clock::now();
    const int centerBlockX = floorDiv(centerX, blockCells);
    const int centerBlockY = floorDiv(centerY, blockCells);
    const int blockMinX = floorDiv(centerX - radius, blockCells);
    const int blockMaxX = floorDiv(centerX + radius, blockCells);
    const int blockMinY = floorDiv(centerY - radius, blockCells);
    const int blockMaxY = floorDiv(centerY + radius, blockCells);
    auto* scene = Owner->GetScene();

    // The window moved (or the cell set changed): rebuild the queue of cells
    // still to compute, re-check every block in range once, and drop blocks
    // that left it. This is the only full scan, once per cell crossed.
    const size_t cellCount = m_cells.size() + m_proceduralCells.size();
    if (m_farScanCenter != Cell{centerX, centerY} || m_farScanRadius != radius ||
        m_farScanCellCount != cellCount)
    {
        // One cell on (same radius and cells): only the edge changed. Keep the
        // queued cells still in range and add the cells that entered, instead
        // of rescanning the whole square (16.6k cells, ~2.6 ms a crossing).
        const Cell oldCenter = m_farScanCenter;
        const bool stepped = m_farScanRadius == radius && m_farScanCellCount == cellCount &&
            oldCenter.first != INT_MIN && std::abs(oldCenter.first - centerX) <= 1 &&
            std::abs(oldCenter.second - centerY) <= 1;
        const auto inOld = [&](int x, int y) {
            return std::max(std::abs(x - oldCenter.first), std::abs(y - oldCenter.second)) <= radius;
        };
        m_farScanCenter = {centerX, centerY};
        m_farScanRadius = radius;
        m_farScanCellCount = cellCount;
        const auto wanted = [&](int x, int y) {
            return HasCell(x, y) && !m_farObjectCells.count({x, y}) && !m_farObjectPending.count({x, y});
        };
        if (stepped)
        {
            std::vector<Cell> kept;
            for (size_t i = m_farObjectQueueNext; i < m_farObjectQueue.size(); ++i)
            {
                const Cell cell = m_farObjectQueue[i];
                if (inRadius(cell) && wanted(cell.first, cell.second)) kept.push_back(cell);
            }
            for (int y = centerY - radius; y <= centerY + radius; ++y)
                for (int x = centerX - radius; x <= centerX + radius; ++x)
                    if (!inOld(x, y) && wanted(x, y)) kept.emplace_back(x, y);
            m_farObjectQueue = std::move(kept);
        }
        else
        {
            m_farObjectQueue.clear();
            for (int y = centerY - radius; y <= centerY + radius; ++y)
                for (int x = centerX - radius; x <= centerX + radius; ++x)
                    if (wanted(x, y)) m_farObjectQueue.emplace_back(x, y);
        }
        m_farObjectQueueNext = 0;
        // Nearest block first, then nearest cell, so blocks complete one
        // after another and each is prepared once.
        const auto rank = [&](Cell cell) {
            const Cell block = blockOf(cell);
            return std::pair{
                std::max(std::abs(block.first - centerBlockX), std::abs(block.second - centerBlockY)),
                std::max(std::abs(cell.first - centerX), std::abs(cell.second - centerY)) };
        };
        std::sort(m_farObjectQueue.begin(), m_farObjectQueue.end(), [&](Cell a, Cell b) {
            const Cell ba = blockOf(a), bb = blockOf(b);
            const auto ra = rank(a), rb = rank(b);
            if (ra.first != rb.first) return ra.first < rb.first;
            if (ba != bb) return ba < bb;
            return ra.second != rb.second ? ra.second < rb.second : a < b;
        });
        // Blocks whose cells all stayed inside (or outside) the far square
        // keep their members: re-checked from the cache. Blocks on the band
        // between the old and new squares walk their cells again.
        for (int by = blockMinY; by <= blockMaxY; ++by)
            for (int bx = blockMinX; bx <= blockMaxX; ++bx)
            {
                bool walk = !stepped;
                if (stepped)
                {
                    const int x0 = bx * blockCells, x1 = x0 + blockCells - 1;
                    const int y0 = by * blockCells, y1 = y0 + blockCells - 1;
                    const auto inside = [&](int cx, int cy) {
                        return x0 >= cx - radius && x1 <= cx + radius && y0 >= cy - radius && y1 <= cy + radius;
                    };
                    const auto outside = [&](int cx, int cy) {
                        return x1 < cx - radius || x0 > cx + radius || y1 < cy - radius || y0 > cy + radius;
                    };
                    walk = !((inside(centerX, centerY) && inside(oldCenter.first, oldCenter.second)) ||
                             (outside(centerX, centerY) && outside(oldCenter.first, oldCenter.second)));
                }
                if (walk) m_farUrgentBlocks.insert({bx, by});
                else m_farRecheckBlocks.insert({bx, by});
            }
        for (auto it = m_farObjectBlocks.begin(); it != m_farObjectBlocks.end(); )
        {
            const Cell key = it->first;
            if (key.first >= blockMinX && key.first <= blockMaxX &&
                key.second >= blockMinY && key.second <= blockMaxY) { ++it; continue; }
            RemoveFarBlockObjects(it->second);
            DrainFarBlockBuild(it->second);
            m_farDirtyBlocks.erase(key);
            m_farUrgentBlocks.erase(key);
            m_farRecheckBlocks.erase(key);
            it = m_farObjectBlocks.erase(it);
        }
    }

    // Schedule queued cells, a few small jobs at a time. Placement generation
    // builds each cell's terrain and object layers, so spread it over more
    // than two threads on wide CPUs: a 24 km world has ~16k far cells.
    // While a large backlog remains (the first fill of the far radius, or a
    // teleport) half the cores work on it: with 4 jobs the landscape's
    // 16,800 far cells took ~30 s of the load on a 32-thread CPU. In play,
    // where a cell crossing queues a row of cells, the small count keeps
    // the frame's own workers free.
    // ENGINE_FAR_OBJECT_JOBS=<n> fixes the count (for A/B runs).
    const auto scheduleStart = std::chrono::steady_clock::now();
    size_t scheduledCells = 0;
    const size_t backlog = m_farObjectQueue.size() - std::min(m_farObjectQueueNext, m_farObjectQueue.size());
    static const size_t fixedJobs = []
    {
        const char* setting = std::getenv("ENGINE_FAR_OBJECT_JOBS");
        return setting ? size_t(std::clamp(std::atoi(setting), 0, 64)) : size_t(0);
    }();
    const size_t maximumJobs = fixedJobs ? fixedJobs : backlog > 512u
        ? std::clamp<size_t>(std::thread::hardware_concurrency() / 2u, 2u, 16u)
        : std::clamp<size_t>(std::thread::hardware_concurrency() / 8u, 2u, 6u);
    constexpr size_t cellsPerJob = 8;
    if (m_farObjectJobs.size() < maximumJobs && m_farObjectQueueNext < m_farObjectQueue.size())
    {
        FarObjectInputs inputs;
        inputs.cellsX = cellsX; inputs.cellsY = cellsY;
        inputs.cellWidth = cellWidth; inputs.cellDepth = cellDepth;
        inputs.builder = m_proceduralBuilder;
        // Far terrain is meshed from the volume (no two-triangle far cells
        // to move placements onto).
        inputs.snapToFarSurface = false;
        inputs.generator = m_objectLayerGenerator;
        inputs.field = GetTerrainField();
        inputs.layerNames = m_objectLayerNames;
        inputs.nearReach = std::max(0, loadedRadius) * std::min(cellsX * cellWidth, cellsY * cellDepth);
        while (m_farObjectJobs.size() < maximumJobs && m_farObjectQueueNext < m_farObjectQueue.size())
        {
            std::vector<FarCellInput> batch;
            FarObjectJob job;
            while (m_farObjectQueueNext < m_farObjectQueue.size() && batch.size() < cellsPerJob)
            {
                const Cell cell = m_farObjectQueue[m_farObjectQueueNext++];
                if (m_farObjectCells.count(cell) || m_farObjectPending.count(cell)) continue;
                FarCellInput input;
                input.cell = cell;
                if (const auto saved = m_cells.find(input.cell); saved != m_cells.end())
                {
                    input.hasSavedTerrain = true;
                    input.savedTerrain = saved->second;
                }
                // Archive-only cells: the job reads their layers.
                if (!m_objectLayers.count(input.cell) &&
                    !(UsesObjectFiles(input.cell) && ObjectFileCells().count(input.cell)) &&
                    m_proceduralCells.count(input.cell))
                {
                    if (auto archive = GetObjectArchive(); archive && archive->Covers(input.cell))
                        input.archive = std::move(archive);
                }
                else
                    GetSavedObjectLayers(input.cell, input.savedLayers);
                job.cells.push_back(input.cell);
                ++scheduledCells;
                m_farObjectPending.insert(input.cell);
                batch.push_back(std::move(input));
            }
            if (batch.empty()) break;
            job.result = std::async(std::launch::async,
                [inputs, batch = std::move(batch)]() mutable {
                    std::vector<std::pair<Cell, FarObjectCell>> cells;
                    for (auto& input : batch)
                    {
                        if (input.archive)
                        {
                            JsonValue layers;
                            if (input.archive->ReadCellLayers(input.cell, layers) && layers.ArraySize())
                                input.savedLayers = std::move(layers);
                            input.archive.reset();
                        }
                        FarObjectCell data;
                        try
                        {
                            data.placements = std::make_shared<const std::vector<TerrainObjectLayer::Placement>>(
                                FarPlacements(inputs, input, &data.cullDistances));
                        }
                        catch (...) {}
                        cells.emplace_back(input.cell, std::move(data));
                    }
                    return cells;
                });
            m_farObjectJobs.push_back(std::move(job));
        }
    }

    // A loaded cell takes over from the far block only once its own object
    // layers have built their instances; until then the far impostors stay,
    // so trees never blink out while a cell streams in. Only loaded and
    // latched cells can change state, so only they are checked. Latching
    // keeps periodic source re-checks from rebuilding far blocks.
    const auto coveredStart = std::chrono::steady_clock::now();
    auto* provider = scene->GetGraphicsProvider();
    for (auto it = m_farNearCovered.begin(); it != m_farNearCovered.end(); )
    {
        const auto found = m_loaded.find(it->first);
        if (found != m_loaded.end() && found->second == it->second &&
            found->second && found->second->Owner) { ++it; continue; }
        m_farUrgentBlocks.insert(blockOf(it->first));
        m_farSyncBlocks.insert(blockOf(it->first));
        ++TerrainEvents::uncovered;
        TerrainEvents::Nearest(TerrainEvents::uncoveredNearest,
            std::max(std::abs(it->first.first - centerX), std::abs(it->first.second - centerY)));
        TerrainEvents::Log(scene->GetFrameIndex(), "uncover", it->first.first, it->first.second,
            std::max(std::abs(it->first.first - centerX), std::abs(it->first.second - centerY)));
        it = m_farNearCovered.erase(it);
    }
    // ENGINE_CELL_HANDOVER=0 swaps cells in one frame as before (A/B runs).
    static const bool handoverEnabled = [] {
        const char* value = std::getenv("ENGINE_CELL_HANDOVER");
        return !(value && value[0] == '0'); }();
    const float handoverSeconds = handoverEnabled
        ? std::clamp(scene->settings.lodTransitionSeconds, 0.f, 2.f) : 0.f;
    const auto handoverNow = std::chrono::steady_clock::now();
    for (const auto& [cell, terrain] : m_loaded)
    {
        if (!terrain || !terrain->Owner || m_farNearCovered.count(cell)) continue;
        bool complete = true;
        for (const auto* child : terrain->Owner->Children)
            if (const auto* layer = child ? child->GetComponent<TerrainObjectLayer>() : nullptr)
                if (!layer->IsPrefabPreparationComplete(provider)) { complete = false; break; }
        // With near blocks the cell's trees are drawn by its blocks.
        if (complete && !NearObjectsReady(cell, *terrain)) complete = false;
        const int cellDistance = std::max(std::abs(cell.first - centerX), std::abs(cell.second - centerY));
        if (!complete)
        {
            // A fade under way holds rather than restarts: a near block
            // re-preparing because a neighbour joined it reports "not ready"
            // for a few frames while it keeps drawing its previous batches,
            // which hold this cell. Cancelling made the trees of every cell in
            // the block vanish and fade in again (16-25 restarts per run at
            // 250 m/s, 2026-11-27). It completes once ready again.
            if (m_handoverStart.count(cell))
                TerrainEvents::Log(scene->GetFrameIndex(), "handover_hold", cell.first, cell.second, cellDistance);
            continue;
        }
        // Cross-fade from the far impostors first (CellHandover); the far
        // block keeps the cell until the fade is done. Only within the loaded
        // radius: a cell kept by the unload margin is on its way out and stays
        // with the far impostors (it finished its handover and unloaded a few
        // frames later).
        if (handoverSeconds > 0.f && !m_handoverStart.count(cell) && !m_editorStreaming &&
            cellDistance > std::max(0, loadedRadius))
            continue;
        if (handoverSeconds > 0.f)
        {
            const auto [entry, started] = m_handoverStart.emplace(cell, handoverNow);
            if (started)
            {
                ++TerrainEvents::handoverStarted;
                TerrainEvents::Nearest(TerrainEvents::handoverNearest, cellDistance);
                TerrainEvents::Log(scene->GetFrameIndex(), "handover_start", cell.first, cell.second, cellDistance);
            }
            if (std::chrono::duration<float>(handoverNow - entry->second).count() < handoverSeconds) continue;
            m_handoverStart.erase(cell);
            ++TerrainEvents::handoverFaded;
            TerrainEvents::Log(scene->GetFrameIndex(), "handover_done", cell.first, cell.second, cellDistance);
        }
        else
        {
            ++TerrainEvents::handoverAbrupt;
            TerrainEvents::Nearest(TerrainEvents::handoverNearest, cellDistance);
            TerrainEvents::Log(scene->GetFrameIndex(), "handover_abrupt", cell.first, cell.second, cellDistance);
        }
        // ENGINE_FAR_OBJECT_LOG=1: how close to the camera's cell a cell's own
        // trees take over from the far impostors (in cells).
        static const bool takeoverLog = std::getenv("ENGINE_FAR_OBJECT_LOG") != nullptr;
        if (takeoverLog)
            std::printf("[FarObjects] takeover at %d cells\n",
                std::max(std::abs(cell.first - centerX), std::abs(cell.second - centerY)));
        m_farNearCovered[cell] = terrain;
        m_farUrgentBlocks.insert(blockOf(cell));
        if (handoverSeconds > 0.f) m_handoverAwaitBlock[cell] = blockOf(cell);
        else m_farSyncBlocks.insert(blockOf(cell));
    }
    for (auto it = m_handoverStart.begin(); it != m_handoverStart.end(); )
        it = m_loaded.count(it->first) ? std::next(it) : m_handoverStart.erase(it);
    TerrainEvents::handoverActive = static_cast<uint32_t>(m_handoverStart.size());

    // Re-evaluate changed blocks: a block shows its cells that are inside the
    // far radius, computed, and not covered at full detail.
    const double spanX = static_cast<double>(cellsX) * cellWidth;
    const double spanZ = static_cast<double>(cellsY) * cellDepth;
    const int projectLayer = Engine::Rendering::DetailBudget::Get().FindLayerOrDefault(
        farObjectProjectLayer);
    double gatherMs = 0.0;
    size_t clusteredRebuilds = 0, plainRebuilds = 0, rebuiltPlacements = 0, asyncRebuilds = 0, installedBuilds = 0;
    // ENGINE_FAR_BLOCK_ASYNC=0 builds every block on the main thread (A/B runs).
    static const bool asyncBlockBuilds = [] {
        const char* value = std::getenv("ENGINE_FAR_BLOCK_ASYNC");
        return !(value && value[0] == '0'); }();
    const auto evaluate = [&](Cell key, bool urgent, bool cached = false)
    {
        if (key.first < blockMinX || key.first > blockMaxX ||
            key.second < blockMinY || key.second > blockMaxY) return true;
        uint64_t membership = 1469598103934665603ull;
        std::vector<Cell> members;
        bool outstanding = false;
        const auto cachedBlock = m_farObjectBlocks.find(key);
        const bool reuse = cached && cachedBlock != m_farObjectBlocks.end() && cachedBlock->second.membersValid;
        if (reuse)
        {
            members = cachedBlock->second.members;
            for (const Cell cell : members)
                membership = (membership ^ (static_cast<uint64_t>(cell.first - key.first * blockCells) |
                    static_cast<uint64_t>(cell.second - key.second * blockCells) << 8)) * 1099511628211ull;
        }
        else
        for (int y = key.second * blockCells; y < (key.second + 1) * blockCells; ++y)
            for (int x = key.first * blockCells; x < (key.first + 1) * blockCells; ++x)
            {
                if (!inRadius({x, y}) || m_farNearCovered.count({x, y})) continue;
                const auto found = m_farObjectCells.find({x, y});
                if (found == m_farObjectCells.end())
                {
                    outstanding = outstanding || HasCell(x, y);
                    continue;
                }
                if (!found->second.placements || found->second.placements->empty()) continue;
                members.emplace_back(x, y);
                membership = (membership ^ (static_cast<uint64_t>(x - key.first * blockCells) |
                    static_cast<uint64_t>(y - key.second * blockCells) << 8)) * 1099511628211ull;
            }
        // New data waits for the rest of the block; coverage and radius
        // changes apply at once so near and far never overlap or leave holes.
        if (outstanding && !urgent) return false;
        // Blocks wholly beyond the cluster distance draw forest clusters.
        const int gapCells = std::max({0, key.first * blockCells - centerX,
            centerX - ((key.first + 1) * blockCells - 1), key.second * blockCells - centerY,
            centerY - ((key.second + 1) * blockCells - 1)});
        // Which forms the block carries (FarObjectBlock::clusterLayer): single
        // trees while part of it can be inside farClusterDistance plus the
        // dissolve band (at most 12 % of the distance, InstanceLodFadeWidth),
        // clusters while part can be beyond it less the band. The camera is
        // somewhere in its cell, so both bounds are loose by a cell.
        const bool clusters = !farClusters.empty() && farClusterDistance > 0.f;
        const double cellSpan = std::min(spanX, spanZ);
        const double nearLow = static_cast<double>(std::max(0, gapCells - 1)) * cellSpan;
        const double farHigh = static_cast<double>(gapCells + blockCells + 1) * std::max(spanX, spanZ) * 1.4143;
        const double bandMargin = farClusterDistance * .12 + 16.0;
        const int forms = !clusters ? 1 : (nearLow < farClusterDistance + bandMargin ? 1 : 0) |
            (farHigh > farClusterDistance - bandMargin ? 2 : 0);
        const bool clustered = (forms & 2) != 0;
        membership = (membership ^ (0x636c7573ull + static_cast<uint64_t>(forms))) * 1099511628211ull;
        // Assets with a cull distance (cliff pieces) leave blocks that lie
        // wholly beyond it, so those blocks carry no batches for them.
        std::map<std::string, float> cullDistances;
        if (reuse) cullDistances = cachedBlock->second.cullDistances;
        else
            for (const Cell& cell : members)
                for (const auto& [asset, distance] : m_farObjectCells[cell].cullDistances)
                    cullDistances[asset] = std::max(cullDistances[asset], distance);
        const double blockDistance = static_cast<double>(std::max(0, gapCells - 1)) * std::min(spanX, spanZ);
        std::set<std::string> beyondCull;
        for (const auto& [asset, distance] : cullDistances)
            if (blockDistance > distance) beyondCull.insert(asset);
        if (!cullDistances.empty())
            membership = (membership ^ (0x63756c6cull + beyondCull.size())) * 1099511628211ull;
        if (members.empty())
        {
            if (const auto found = m_farObjectBlocks.find(key); found != m_farObjectBlocks.end())
            {
                RemoveFarBlockObjects(found->second);
                DrainFarBlockBuild(found->second);
                m_farObjectBlocks.erase(found);
            }
            return true;
        }
        auto& block = m_farObjectBlocks[key];
        if (!reuse)
        {
            block.members = members;
            block.cullDistances = cullDistances;
            block.membersValid = !outstanding;
        }
        if (block.build.valid())
        {
            if (block.buildMembership == membership) return true;
            DrainFarBlockBuild(block);   // superseded
        }
        if (block.object && block.membership == membership) return true;
        // A form added or dropped: every placement of it is beyond its band
        // then (nothing changes on screen); counted to keep an eye on it.
        if (block.object && block.forms != forms)
        {
            ++TerrainEvents::farClusterFlips;
            TerrainEvents::Log(scene->GetFrameIndex(), (forms & ~block.forms) ? "cluster_on" : "cluster_off",
                key.first, key.second, gapCells);
        }
        block.forms = forms;
        if (!block.object)
        {
            block.object = scene->AddObject("Far objects " +
                std::to_string(key.first) + "," + std::to_string(key.second));
            block.object->runtimeOnly = true;
            block.object->layer = projectLayer;
            scene->MoveObject(block.object, Owner,
                Engine::Scene::Scene::ObjectPlacement::AsChild);
            block.object->transform.position = {
                static_cast<double>(key.first) * blockCells * spanX, 0.0,
                static_cast<double>(key.second) * blockCells * spanZ};
            block.layer = block.object->AddComponent<TerrainObjectLayer>();
            block.layer->layerName = "Far objects";
            block.layer->handoverRole = 2;   // far impostors (CellHandover)
            // Single-level chains: the far assets are already the last LOD.
            // Out to the far canopy distance when there is one: past it
            // the terrain's canopy (from these same placements) takes over.
            block.layer->viewDistance = farCanopyDistance > 0.f ? farCanopyDistance : 100000.f;
            block.layer->lod1Distance = 100000.f;
            block.layer->lod2Distance = 100000.f;
            block.layer->shadowDistance = 0.f;
            // Far trees cast only into the far shadow cascade, which
            // bounds the range; near cascades cull them by their bounds.
            block.layer->farShadowDistance = block.layer->viewDistance;
            // Placements are only ever replaced below, then marked dirty.
            block.layer->placementsChangeOnlyWhenMarked = true;
        }
        // The two forms as bands of one LOD switch at farClusterDistance.
        block.layer->forcedLodBand = clusters ? 0 : -1;
        block.layer->forcedLodDistance = farClusterDistance;
        if (clustered && !block.clusterObject)
        {
            block.clusterObject = scene->AddObject("Far clusters " +
                std::to_string(key.first) + "," + std::to_string(key.second));
            block.clusterObject->runtimeOnly = true;
            block.clusterObject->layer = projectLayer;
            scene->MoveObject(block.clusterObject, Owner, Engine::Scene::Scene::ObjectPlacement::AsChild);
            block.clusterObject->transform.position = block.object->transform.position;
            block.clusterLayer = block.clusterObject->AddComponent<TerrainObjectLayer>();
            block.clusterLayer->layerName = "Far clusters";
            block.clusterLayer->handoverRole = 2;
            block.clusterLayer->viewDistance = block.layer->viewDistance;
            block.clusterLayer->lod1Distance = 100000.f;
            block.clusterLayer->lod2Distance = 100000.f;
            block.clusterLayer->shadowDistance = 0.f;
            block.clusterLayer->placementsChangeOnlyWhenMarked = true;
            block.clusterObject->enabled = false;
        }
        if (block.clusterLayer)
        {
            block.clusterLayer->forcedLodBand = 1;
            block.clusterLayer->forcedLodDistance = farClusterDistance;
            // Clusters lie beyond farClusterDistance: when the far casters
            // stop before that (the view distance), they can never cast, so
            // they leave the shadow view instead of being prepared and
            // culled there every frame (+0.3 ms at 23 m/s).
            const float casterReach = block.clusterLayer->viewDistance;
            block.clusterLayer->farShadowDistance = casterReach <= farClusterDistance ? 0.f : casterReach;
        }
        // The block's placements: its cells' (shared, read-only) placements
        // moved into the block, then clustered past farClusterDistance. Copying
        // ~17k placements and clustering took 5-8 ms of the main thread a
        // frame flying at 250 m/s (2026-11-26), so blocks away from the loaded
        // cells build on a worker and install when ready. Blocks next to them
        // build at once: a loaded cell's takeover must hide its far trees in
        // the same frame (no doubled trees, no hole).
        BlockSources sources;
        sources.reserve(members.size());
        for (const Cell& cell : members)
            if (const auto& shared = m_farObjectCells[cell].placements)
                sources.push_back({glm::vec3(
                    static_cast<float>((cell.first - key.first * blockCells) * spanX), 0.f,
                    static_cast<float>((cell.second - key.second * blockCells) * spanZ)), shared});
        const ClusterSettings clusterSettings{(forms & 1) != 0, clustered, farClusters,
            std::clamp(farClusterSize, 4.f, 1000.f), std::max(1, farClusterMinimumTrees), key};
        // On the main thread only when a coverage change needs this frame
        // (m_farSyncBlocks); before handover fades every block next to the
        // loaded cells did (the camera's block rebuilt ~1 frame in 5 at 250 m/s).
        const bool nearLoaded = m_farSyncBlocks.count(key) != 0;
        if (asyncBlockBuilds && !nearLoaded)
        {
            block.buildMembership = membership;
            block.buildCullDistances = cullDistances;
            block.buildBeyondCull = beyondCull;
            block.build = std::async(std::launch::async,
                [sources = std::move(sources), beyondCull, clusterSettings]() {
                    auto forms = BuildFarBlockPlacements(sources, beyondCull, clusterSettings);
                    return FarBlockBuilt{std::move(forms.first), std::move(forms.second)};
                });
            ++asyncRebuilds;
            return true;
        }
        const auto gatherStart = std::chrono::steady_clock::now();
        auto bothForms = BuildFarBlockPlacements(sources, beyondCull, clusterSettings);
        FarBlockBuilt built{std::move(bothForms.first), std::move(bothForms.second)};
        gatherMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - gatherStart).count();
        rebuiltPlacements += built.trees.size() + built.clusters.size();
        if (clustered) ++clusteredRebuilds; else ++plainRebuilds;
        InstallFarBlockPlacements(block, std::move(built), cullDistances, beyondCull, membership);
        ++TerrainEvents::farBlockRebuilds;
        TerrainEvents::Log(scene->GetFrameIndex(), "far_block", key.first, key.second, gapCells);
        return true;
    };
    // ENGINE_FAR_OBJECT_LOG=1: where a slow update goes (rebuilt blocks,
    // their placements, and the preparation latch below).
    static const bool farLog = std::getenv("ENGINE_FAR_OBJECT_LOG") != nullptr;
    const auto rebuildStart = std::chrono::steady_clock::now();
    const size_t dirtyCount = m_farUrgentBlocks.size() + m_farDirtyBlocks.size();
    // A block that is still computing is not kept dirty: each finished job
    // marks its blocks again, and the last one triggers the full rebuild.
    for (const Cell key : m_farUrgentBlocks)
    {
        evaluate(key, true);
        m_farDirtyBlocks.erase(key);
        m_farRecheckBlocks.erase(key);
    }
    m_farUrgentBlocks.clear();
    for (const Cell key : m_farDirtyBlocks)
    {
        evaluate(key, false);
        m_farRecheckBlocks.erase(key);
    }
    m_farDirtyBlocks.clear();
    for (const Cell key : m_farRecheckBlocks)
        evaluate(key, true, true);
    m_farRecheckBlocks.clear();
    m_farSyncBlocks.clear();

    // Install finished worker builds; let superseded ones finish.
    for (auto& [key, block] : m_farObjectBlocks)
        if (block.build.valid() && block.build.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
        {
            FarBlockBuilt built;
            try { built = block.build.get(); }
            catch (...) { continue; }
            if (!block.layer) continue;
            InstallFarBlockPlacements(block, std::move(built), block.buildCullDistances,
                block.buildBeyondCull, block.buildMembership);
            ++installedBuilds;
            ++TerrainEvents::farBlockInstalls;
            TerrainEvents::Log(scene->GetFrameIndex(), "far_block_install", key.first, key.second, -1);
        }
    m_farBlockDraining.erase(std::remove_if(m_farBlockDraining.begin(), m_farBlockDraining.end(), [](auto& build) {
        return !build.valid() || build.wait_for(std::chrono::seconds(0)) == std::future_status::ready; }),
        m_farBlockDraining.end());

    const auto latchStart = std::chrono::steady_clock::now();
    size_t latched = 0, unprepared = 0;
    // Latch blocks whose instances finished preparing (see FarObjectBlock).
    for (auto& [key, block] : m_farObjectBlocks)
        if (!block.prepared && block.layer)
        {
            ++unprepared;
            // Both forms the block carries (a form it does not carry is off).
            const bool treesReady = !(block.forms & 1) || block.layer->IsPrefabPreparationComplete(provider);
            const bool clustersReady = !(block.forms & 2) || !block.clusterLayer ||
                block.clusterLayer->IsPrefabPreparationComplete(provider);
            if (treesReady && clustersReady) { block.prepared = true; ++latched; }
        }
    for (auto it = m_handoverAwaitBlock.begin(); it != m_handoverAwaitBlock.end(); )
    {
        const auto found = m_farObjectBlocks.find(it->second);
        const bool settled = found == m_farObjectBlocks.end() ||
            (found->second.prepared && !found->second.build.valid());
        it = settled || !m_farNearCovered.count(it->first) ? m_handoverAwaitBlock.erase(it) : std::next(it);
    }
    PublishCellHandover(centerX, centerY);
    if (farLog)
    {
        const auto now = std::chrono::steady_clock::now();
        const double rebuildMs = std::chrono::duration<double, std::milli>(latchStart - rebuildStart).count();
        const double latchMs = std::chrono::duration<double, std::milli>(now - latchStart).count();
        if (std::chrono::duration<double, std::milli>(now - scanStart).count() > 5.0)
            std::printf("[FarObjects] scan %.1f, schedule %zu cells %.1f, covered %.1f, rebuild %zu blocks %.1f "
                "(%zu plain + %zu clustered here, %zu placements %.1f ms; %zu to workers, %zu installed), latch %zu/%zu %.1f ms\n",
                std::chrono::duration<double, std::milli>(scheduleStart - scanStart).count(),
                scheduledCells, std::chrono::duration<double, std::milli>(coveredStart - scheduleStart).count(),
                std::chrono::duration<double, std::milli>(rebuildStart - coveredStart).count(),
                dirtyCount, rebuildMs, plainRebuilds, clusteredRebuilds, rebuiltPlacements, gatherMs,
                asyncRebuilds, installedBuilds, latched, unprepared, latchMs);
    }
}
}

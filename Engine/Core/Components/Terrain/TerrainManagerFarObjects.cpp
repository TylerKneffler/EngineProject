#include "Core/Components/Terrain/TerrainManager.h"
#include "Core/Components/Terrain/TerrainObjectLayer.h"
#include "Core/Components/Terrain/TerrainSurfaceSample.h"
#include "Core/Scene/Object.h"
#include "Core/Rendering/DetailBudget.h"
#include "Core/Scene/Scene.h"

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
};

std::vector<TerrainObjectLayer::Placement> FarPlacements(
    const FarObjectInputs& inputs, const FarCellInput& input,
    std::map<std::string, float>* cullDistances = nullptr)
{
    std::vector<TerrainObjectLayer::Placement> result;
    Engine::Core::Object terrainObject;
    auto* terrain = terrainObject.AddComponent<Terrain>();
    if (input.hasSavedTerrain) terrain->Deserialize(input.savedTerrain);
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
        // Ground cover (grass) has its own reach and no far representation.
        if (layer->conformToGround || layer->snapToGround) continue;
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
}

void TerrainManager::FenceFarObjectJobs()
{
    for (auto& job : m_farObjectJobs)
        if (job.result.valid()) job.result.wait();
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
    if (Owner && Owner->GetScene())
        for (auto& [key, block] : m_farObjectBlocks)
            if (block.object) Owner->GetScene()->RemoveObject(block.object);
    m_farObjectBlocks.clear();
    m_farScanCenter = { INT_MIN, INT_MIN };
    m_farScanRadius = -1;
    m_farScanCellCount = 0;
    m_farObjectQueue.clear();
    m_farObjectQueueNext = 0;
    m_farDirtyBlocks.clear();
    m_farUrgentBlocks.clear();
}

TerrainManager::FarObjectStatus TerrainManager::GetFarObjectStatus() const
{
    FarObjectStatus status;
    status.computedCells = m_farObjectCells.size();
    status.queuedCells = m_farObjectQueue.size() - std::min(m_farObjectQueueNext, m_farObjectQueue.size());
    status.pendingCells = m_farObjectPending.size();
    status.blocks = m_farObjectBlocks.size();
    for (const auto& [key, block] : m_farObjectBlocks)
        if (!block.prepared) ++status.preparingBlocks;
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
        m_farScanCenter = {centerX, centerY};
        m_farScanRadius = radius;
        m_farScanCellCount = cellCount;
        m_farObjectQueue.clear();
        m_farObjectQueueNext = 0;
        for (int y = centerY - radius; y <= centerY + radius; ++y)
            for (int x = centerX - radius; x <= centerX + radius; ++x)
                if (HasCell(x, y) && !m_farObjectCells.count({x, y}) &&
                    !m_farObjectPending.count({x, y}))
                    m_farObjectQueue.emplace_back(x, y);
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
        for (int by = blockMinY; by <= blockMaxY; ++by)
            for (int bx = blockMinX; bx <= blockMaxX; ++bx)
                m_farUrgentBlocks.insert({bx, by});
        for (auto it = m_farObjectBlocks.begin(); it != m_farObjectBlocks.end(); )
        {
            const Cell key = it->first;
            if (key.first >= blockMinX && key.first <= blockMaxX &&
                key.second >= blockMinY && key.second <= blockMaxY) { ++it; continue; }
            if (it->second.object) scene->RemoveObject(it->second.object);
            m_farDirtyBlocks.erase(key);
            m_farUrgentBlocks.erase(key);
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
        inputs.snapToFarSurface = FarObjectSurfaceSnap() &&
            (m_farHeightArchivePath.empty() || FarCellDetailPixels() <= 0.f);
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
                if (const auto layers = m_objectLayers.find(input.cell);
                    layers != m_objectLayers.end())
                    input.savedLayers = layers->second;
                job.cells.push_back(input.cell);
                m_farObjectPending.insert(input.cell);
                batch.push_back(std::move(input));
            }
            if (batch.empty()) break;
            job.result = std::async(std::launch::async,
                [inputs, batch = std::move(batch)]() {
                    std::vector<std::pair<Cell, FarObjectCell>> cells;
                    for (const auto& input : batch)
                    {
                        FarObjectCell data;
                        try { data.placements = FarPlacements(inputs, input, &data.cullDistances); }
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
    auto* provider = scene->GetGraphicsProvider();
    for (auto it = m_farNearCovered.begin(); it != m_farNearCovered.end(); )
    {
        const auto found = m_loaded.find(it->first);
        if (found != m_loaded.end() && found->second == it->second &&
            found->second && found->second->Owner) { ++it; continue; }
        m_farUrgentBlocks.insert(blockOf(it->first));
        it = m_farNearCovered.erase(it);
    }
    for (const auto& [cell, terrain] : m_loaded)
    {
        if (!terrain || !terrain->Owner || m_farNearCovered.count(cell)) continue;
        bool complete = true;
        for (const auto* child : terrain->Owner->Children)
            if (const auto* layer = child ? child->GetComponent<TerrainObjectLayer>() : nullptr)
                if (!layer->IsPrefabPreparationComplete(provider)) { complete = false; break; }
        // With near blocks the cell's trees are drawn by its blocks.
        if (complete && !NearObjectsReady(cell, *terrain)) complete = false;
        if (!complete) continue;
        m_farNearCovered[cell] = terrain;
        m_farUrgentBlocks.insert(blockOf(cell));
    }

    // Re-evaluate changed blocks: a block shows its cells that are inside the
    // far radius, computed, and not covered at full detail.
    const double spanX = static_cast<double>(cellsX) * cellWidth;
    const double spanZ = static_cast<double>(cellsY) * cellDepth;
    const int projectLayer = Engine::Rendering::DetailBudget::Get().FindLayerOrDefault(
        farObjectProjectLayer);
    const auto evaluate = [&](Cell key, bool urgent)
    {
        if (key.first < blockMinX || key.first > blockMaxX ||
            key.second < blockMinY || key.second > blockMaxY) return true;
        uint64_t membership = 1469598103934665603ull;
        std::vector<Cell> members;
        bool outstanding = false;
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
                if (found->second.placements.empty()) continue;
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
        const bool clustered = !farClusters.empty() && farClusterDistance > 0.f &&
            static_cast<double>(gapCells) * std::min(spanX, spanZ) >= farClusterDistance;
        if (clustered) membership = (membership ^ 0x636c7573ull) * 1099511628211ull;
        // Assets with a cull distance (cliff pieces) leave blocks that lie
        // wholly beyond it, so those blocks carry no batches for them.
        std::map<std::string, float> cullDistances;
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
                if (found->second.object) scene->RemoveObject(found->second.object);
                m_farObjectBlocks.erase(found);
            }
            return true;
        }
        auto& block = m_farObjectBlocks[key];
        if (block.object && block.membership == membership) return true;
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
        std::vector<TerrainObjectLayer::Placement> placements;
        uint64_t id = 0;
        for (const Cell& cell : members)
        {
            const glm::vec3 offset(
                static_cast<float>((cell.first - key.first * blockCells) * spanX), 0.f,
                static_cast<float>((cell.second - key.second * blockCells) * spanZ));
            for (auto placement : m_farObjectCells[cell].placements)
            {
                if (!beyondCull.empty() && beyondCull.count(placement.asset)) continue;
                placement.position += offset;
                placement.id = ++id;
                placements.push_back(std::move(placement));
            }
        }
        if (clustered)
            placements = ClusterFarPlacements(placements, farClusters,
                std::clamp(farClusterSize, 4.f, 1000.f), std::max(1, farClusterMinimumTrees), key);
        block.layer->objectLods.clear();
        for (const auto& [asset, distance] : cullDistances)
        {
            if (beyondCull.count(asset)) continue;
            auto& lod = block.layer->objectLods[asset];
            lod.cullAfterLastLod = true;
            lod.finalLodCullDistance = distance;
        }
        block.layer->placements = std::move(placements);
        block.layer->MarkConfigurationDirty();
        block.membership = membership;
        block.prepared = false;
        return true;
    };
    // A block that is still computing is not kept dirty: each finished job
    // marks its blocks again, and the last one triggers the full rebuild.
    for (const Cell key : m_farUrgentBlocks)
    {
        evaluate(key, true);
        m_farDirtyBlocks.erase(key);
    }
    m_farUrgentBlocks.clear();
    for (const Cell key : m_farDirtyBlocks)
        evaluate(key, false);
    m_farDirtyBlocks.clear();

    // Latch blocks whose instances finished preparing (see FarObjectBlock).
    for (auto& [key, block] : m_farObjectBlocks)
        if (!block.prepared && block.layer && block.layer->IsPrefabPreparationComplete(provider))
            block.prepared = true;
}
}

#include "Core/Components/Terrain/TerrainManager.h"
#include "Core/Components/Terrain/TerrainObjectLayer.h"
#include "Core/Scene/Object.h"
#include "Core/Rendering/DetailBudget.h"
#include "Core/Scene/Scene.h"

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
};

struct FarCellInput
{
    Cell cell;
    bool hasSavedTerrain = false;
    JsonValue savedTerrain;
    JsonValue savedLayers;
};

std::vector<TerrainObjectLayer::Placement> FarPlacements(
    const FarObjectInputs& inputs, const FarCellInput& input)
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
            // Assets authored to vanish after their last level (near-only
            // props such as cliff blocks) have no far representation.
            if (profile.cullAfterLastLod) continue;
            auto chain = profile.meshes;
            chain.erase(std::remove(chain.begin(), chain.end(), std::string{}), chain.end());
            // Only assets with a cheaper last level (impostors) draw far away.
            if (chain.size() < 2) continue;
            auto distant = placement;
            distant.asset = chain.back();
            result.push_back(std::move(distant));
        }
    }
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
    const size_t maximumJobs = std::clamp<size_t>(
        std::thread::hardware_concurrency() / 8u, 2u, 6u);
    constexpr size_t cellsPerJob = 8;
    if (m_farObjectJobs.size() < maximumJobs && m_farObjectQueueNext < m_farObjectQueue.size())
    {
        FarObjectInputs inputs;
        inputs.cellsX = cellsX; inputs.cellsY = cellsY;
        inputs.cellWidth = cellWidth; inputs.cellDepth = cellDepth;
        inputs.builder = m_proceduralBuilder;
        inputs.generator = m_objectLayerGenerator;
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
                        try { data.placements = FarPlacements(inputs, input); }
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
            block.layer->viewDistance = 100000.f;
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
                placement.position += offset;
                placement.id = ++id;
                placements.push_back(std::move(placement));
            }
        }
        if (clustered)
            placements = ClusterFarPlacements(placements, farClusters,
                std::clamp(farClusterSize, 4.f, 1000.f), std::max(1, farClusterMinimumTrees), key);
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

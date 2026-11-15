#include "Core/Compoonents/Terrain/TerrainManager.h"
#include "Core/Compoonents/Terrain/TerrainObjectLayer.h"
#include "Core/Object.h"
#include "Core/Rendering/DetailBudget.h"
#include "Core/Scene/Scene.h"

#include <algorithm>
#include <cmath>

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
            inputs.generator(context, name, *layer);
        }
        // Ground cover (grass) has its own reach and no far representation.
        if (layer->conformToGround || layer->snapToGround) continue;
        for (const auto& placement : layer->placements)
        {
            auto chain = layer->GetObjectLodProfile(placement.asset).meshes;
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
    if (Owner && Owner->GetScene())
        for (auto& [key, block] : m_farObjectBlocks)
            if (block.object) Owner->GetScene()->RemoveObject(block.object);
    m_farObjectBlocks.clear();
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

    // Collect finished jobs.
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
        for (const auto& cell : it->cells) m_farObjectPending.erase(cell);
        it = m_farObjectJobs.erase(it);
    }

    // Schedule missing cells nearest first, a few small jobs at a time.
    constexpr size_t maximumJobs = 2, cellsPerJob = 8;
    if (m_farObjectJobs.size() < maximumJobs)
    {
        std::vector<Cell> missing;
        for (int y = centerY - radius; y <= centerY + radius; ++y)
            for (int x = centerX - radius; x <= centerX + radius; ++x)
                if (HasCell(x, y) && !m_farObjectCells.count({x, y}) &&
                    !m_farObjectPending.count({x, y}))
                    missing.emplace_back(x, y);
        std::sort(missing.begin(), missing.end(), [&](Cell a, Cell b) {
            const int da = std::max(std::abs(a.first - centerX), std::abs(a.second - centerY));
            const int db = std::max(std::abs(b.first - centerX), std::abs(b.second - centerY));
            return da == db ? a < b : da < db;
        });
        FarObjectInputs inputs;
        inputs.cellsX = cellsX; inputs.cellsY = cellsY;
        inputs.cellWidth = cellWidth; inputs.cellDepth = cellDepth;
        inputs.builder = m_proceduralBuilder;
        inputs.generator = m_objectLayerGenerator;
        inputs.layerNames = m_objectLayerNames;
        size_t next = 0;
        while (m_farObjectJobs.size() < maximumJobs && next < missing.size())
        {
            std::vector<FarCellInput> batch;
            FarObjectJob job;
            for (; next < missing.size() && batch.size() < cellsPerJob; ++next)
            {
                FarCellInput input;
                input.cell = missing[next];
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

    // Group cells into blocks; a block shows its cells that are inside the
    // far radius, computed, and not loaded at full detail.
    const int blockCells = std::clamp(farObjectBlockCells, 1, 32);
    const auto floorDiv = [](int value, int divisor) {
        return value >= 0 ? value / divisor : -((-value + divisor - 1) / divisor);
    };
    const int blockMinX = floorDiv(centerX - radius, blockCells);
    const int blockMaxX = floorDiv(centerX + radius, blockCells);
    const int blockMinY = floorDiv(centerY - radius, blockCells);
    const int blockMaxY = floorDiv(centerY + radius, blockCells);
    auto* scene = Owner->GetScene();
    const double spanX = static_cast<double>(cellsX) * cellWidth;
    const double spanZ = static_cast<double>(cellsY) * cellDepth;
    const int projectLayer = Engine::Rendering::DetailBudget::Get().FindLayerOrDefault(
        farObjectProjectLayer);
    // A loaded cell takes over from the far block only once its own object
    // layers have built their instances; until then the far impostors stay,
    // so trees never blink out while a cell streams in.
    auto* provider = scene->GetGraphicsProvider();
    const auto nearCovers = [&](Cell cell) {
        const auto found = m_loaded.find(cell);
        if (found == m_loaded.end() || !found->second || !found->second->Owner) return false;
        for (const auto* child : found->second->Owner->Children)
            if (const auto* layer = child ? child->GetComponent<TerrainObjectLayer>() : nullptr)
                if (!layer->IsPrefabPreparationComplete(provider)) return false;
        return true;
    };
    std::set<Cell> activeBlocks;
    for (int by = blockMinY; by <= blockMaxY; ++by)
        for (int bx = blockMinX; bx <= blockMaxX; ++bx)
        {
            const Cell key{bx, by};
            activeBlocks.insert(key);
            uint64_t membership = 1469598103934665603ull;
            std::vector<Cell> members;
            for (int y = by * blockCells; y < (by + 1) * blockCells; ++y)
                for (int x = bx * blockCells; x < (bx + 1) * blockCells; ++x)
                {
                    if (std::max(std::abs(x - centerX), std::abs(y - centerY)) > radius ||
                        nearCovers({x, y})) continue;
                    const auto found = m_farObjectCells.find({x, y});
                    if (found == m_farObjectCells.end() || found->second.placements.empty())
                        continue;
                    members.emplace_back(x, y);
                    membership = (membership ^ (static_cast<uint64_t>(x - bx * blockCells) |
                        static_cast<uint64_t>(y - by * blockCells) << 8)) * 1099511628211ull;
                }
            auto& block = m_farObjectBlocks[key];
            if (members.empty())
            {
                if (block.object) scene->RemoveObject(block.object);
                block = {};
                continue;
            }
            if (block.object && block.membership == membership) continue;
            if (!block.object)
            {
                block.object = scene->AddObject("Far objects " +
                    std::to_string(bx) + "," + std::to_string(by));
                block.object->runtimeOnly = true;
                block.object->layer = projectLayer;
                scene->MoveObject(block.object, Owner,
                    Engine::Scene::Scene::ObjectPlacement::AsChild);
                block.object->transform.position = {
                    static_cast<double>(bx) * blockCells * spanX, 0.0,
                    static_cast<double>(by) * blockCells * spanZ};
                block.layer = block.object->AddComponent<TerrainObjectLayer>();
                block.layer->layerName = "Far objects";
                // Single-level chains: the far assets are already the last LOD.
                block.layer->viewDistance = 100000.f;
                block.layer->lod1Distance = 100000.f;
                block.layer->lod2Distance = 100000.f;
                block.layer->shadowDistance = 0.f;
            }
            std::vector<TerrainObjectLayer::Placement> placements;
            uint64_t id = 0;
            for (const Cell& cell : members)
            {
                const glm::vec3 offset(
                    static_cast<float>((cell.first - bx * blockCells) * spanX), 0.f,
                    static_cast<float>((cell.second - by * blockCells) * spanZ));
                for (auto placement : m_farObjectCells[cell].placements)
                {
                    placement.position += offset;
                    placement.id = ++id;
                    placements.push_back(std::move(placement));
                }
            }
            block.layer->placements = std::move(placements);
            block.layer->MarkConfigurationDirty();
            block.membership = membership;
        }
    // Blocks that left the far radius.
    for (auto it = m_farObjectBlocks.begin(); it != m_farObjectBlocks.end(); )
    {
        if (activeBlocks.count(it->first)) { ++it; continue; }
        if (it->second.object) scene->RemoveObject(it->second.object);
        it = m_farObjectBlocks.erase(it);
    }
}
}

#include "Core/Rendering/DetailBudget.h"
#include "TerrainManager.h"
#include "TerrainObjectLayer.h"
#include "TerrainSurfaceSample.h"
#include "Core/Scene/Object.h"
#include "Core/Scene/Scene.h"
#include "Core/Components/Terrain/TerrainBatchAppearance.h"
#include "Core/Components/Camera/Camera.h"
#include "Core/Components/Mesh/Mesh.h"
#include "Core/Components/Materials/Material.h"
#include "Core/Components/Materials/Texture.h"
#include "Core/Components/Physics/MeshObjectCollider.h"
#include "Core/Components/Physics/RigidBody.h"
#include "Core/Inspection/IInspectorUi.h"
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
void TerrainManager::CaptureObjectLayers(Cell cell,
    const Engine::Core::Object& cellObject)
{
    JsonValue layers = JsonValue::MakeArray();
    for (const auto* child : cellObject.Children)
        if (child)
            if (const auto* layer = child->GetComponent<TerrainObjectLayer>())
                layers.Push(layer->Serialize());
    if (layers.ArraySize()) m_objectLayers[cell] = std::move(layers);
    else m_objectLayers.erase(cell);
}

void TerrainManager::RestoreObjectLayers(Cell cell,
    Engine::Core::Object& cellObject)
{
    if (!cellObject.GetScene()) return;
    const auto findLayer = [&](const std::string& name) {
        for (auto* child : cellObject.Children)
            if (child)
                if (auto* layer = child->GetComponent<TerrainObjectLayer>())
                    if (layer->layerName == name) return layer;
        return static_cast<TerrainObjectLayer*>(nullptr);
    };
    const auto makeLayer = [&](const std::string& name,
        const JsonValue* saved) {
        if (name.empty()) return;
        auto* layer = findLayer(name);
        Engine::Core::Object* child = layer ? layer->Owner : nullptr;
        if (!layer)
        {
            child = cellObject.GetScene()->AddObject(name);
            child->runtimeOnly = true;
            cellObject.GetScene()->SetParentLocal(child, &cellObject);
            layer = child->AddComponent<TerrainObjectLayer>();
            if (saved) layer->Deserialize(*saved);
            layer->layerName = name;
            // Synchronous loads generate here; streamed cells arrive with
            // their generated layers already installed.
            const auto* terrain = cellObject.GetComponent<Terrain>();
            if (!saved && m_objectLayerGenerator && terrain)
            {
                ObjectLayerContext context;
                context.cell = cell;
                context.cellsX = terrain->cellsX;
                context.cellsY = terrain->cellsY;
                context.cellWidth = terrain->cellWidth;
                context.cellDepth = terrain->cellDepth;
                context.heights = &terrain->heights;
                context.weights = &terrain->weights;
                context.terrainLayerCount = terrain->layers.size();
                const size_t before = layer->placements.size();
                m_objectLayerGenerator(context, name, *layer);
                if (layer->placements.size() != before) layer->MarkConfigurationDirty();
            }
        }
        // Project layer for budgets and filtering: the first component that
        // prefers one (e.g. ground cover in "Grass"), otherwise "Props". The
        // setup hook (scene content such as grass presets, see
        // SetObjectLayerSetup) may add components or override the layer.
        const char* preferredLayer = "Props";
        for (const auto* component : child->Components)
            if (const char* preferred = component ? component->PreferredDetailLayer() : nullptr)
            {
                preferredLayer = preferred;
                break;
            }
        child->layer = Engine::Rendering::DetailBudget::Get().FindLayerOrDefault(preferredLayer);
        if (m_objectLayerSetup) m_objectLayerSetup(cell, *layer);
    };
    if (const auto found = m_objectLayers.find(cell);
        found != m_objectLayers.end())
    {
        const JsonValue& layers = found->second;
        for (size_t i = 0; i < layers.ArraySize(); ++i)
        {
            const JsonValue& saved = layers.ArrayAt(i);
            makeLayer(saved["layerName"].AsString(), &saved);
        }
    }
    for (const auto& name : m_objectLayerNames) makeLayer(name, nullptr);
}

bool TerrainManager::AddObjectLayer(const std::string& name)
{
    if (name.empty() || std::all_of(name.begin(), name.end(),
            [](unsigned char c) { return std::isspace(c) != 0; }) ||
        std::find(m_objectLayerNames.begin(), m_objectLayerNames.end(), name) !=
            m_objectLayerNames.end() ||
        (Owner && Owner->GetScene() && Owner->GetScene()->HasStarted()))
        return false;
    m_objectLayerNames.push_back(name);
    InvalidatePrepared();
    for (const auto& [cell, terrain] : m_loaded)
        if (terrain && terrain->Owner)
        {
            RestoreObjectLayers(cell, *terrain->Owner);
            CaptureObjectLayers(cell, *terrain->Owner);
        }
    MarkConfigurationDirty();
    return true;
}

bool TerrainManager::RemoveObjectLayer(const std::string& name)
{
    const auto found = std::find(m_objectLayerNames.begin(),
        m_objectLayerNames.end(), name);
    if (found == m_objectLayerNames.end() ||
        (Owner && Owner->GetScene() && Owner->GetScene()->HasStarted()))
        return false;
    m_objectLayerNames.erase(found);
    InvalidatePrepared();
    for (auto it = m_objectLayers.begin(); it != m_objectLayers.end(); )
    {
        JsonValue kept = JsonValue::MakeArray();
        for (size_t i = 0; i < it->second.ArraySize(); ++i)
        {
            const JsonValue& entry = it->second.ArrayAt(i);
            if (entry["layerName"].AsString() != name) kept.Push(entry);
        }
        if (kept.ArraySize()) { it->second = std::move(kept); ++it; }
        else it = m_objectLayers.erase(it);
    }
    for (const auto& [cell, terrain] : m_loaded)
        if (terrain && terrain->Owner && terrain->Owner->GetScene())
        {
            auto* object = terrain->Owner;
            for (auto* child : object->Children)
                if (child)
                    if (auto* layer = child->GetComponent<TerrainObjectLayer>())
                        if (layer->layerName == name)
                        {
                            object->GetScene()->RemoveObject(child);
                            break;
                        }
            CaptureObjectLayers(cell, *object);
        }
    MarkConfigurationDirty();
    return true;
}

void TerrainManager::Stitch(Cell cell)
{
    auto* terrain = GetLoadedCell(cell.first, cell.second);
    if (!terrain) return;
    bool changed = false;
    bool anyChanged = false;
    for (const Cell offset : {Cell{-1, 0}, Cell{1, 0}, Cell{0, -1}, Cell{0, 1}})
    {
        auto* neighbor = GetLoadedCell(cell.first + offset.first, cell.second + offset.second);
        if (!neighbor) continue;
        const Cell neighborCell{cell.first + offset.first,
            cell.second + offset.second};
        const bool incomingEdited = m_editedProcedural.count(cell) != 0;
        const bool neighborEdited = m_editedProcedural.count(neighborCell) != 0;
        const bool useIncoming = incomingEdited != neighborEdited
            ? incomingEdited : incomingEdited && cell < neighborCell;
        bool neighborChanged = false;
        const int count = offset.first ? cellsY : cellsX;
        for (int i = 0; i <= count; ++i)
        {
            const int ax = offset.first < 0 ? 0 : offset.first > 0 ? cellsX : i;
            const int ay = offset.second < 0 ? 0 : offset.second > 0 ? cellsY : i;
            const int bx = offset.first < 0 ? cellsX : offset.first > 0 ? 0 : i;
            const int by = offset.second < 0 ? cellsY : offset.second > 0 ? 0 : i;
            const size_t a = static_cast<size_t>(ay * (cellsX + 1) + ax);
            const size_t b = static_cast<size_t>(by * (cellsX + 1) + bx);
            Terrain* destination = useIncoming ? neighbor : terrain;
            Terrain* source = useIncoming ? terrain : neighbor;
            const size_t dst = useIncoming ? b : a;
            const size_t src = useIncoming ? a : b;
            if (dst >= destination->heights.size() || src >= source->heights.size())
                continue;
            if (destination->heights[dst] != source->heights[src])
            {
                destination->heights[dst] = source->heights[src];
                if (useIncoming) neighborChanged = true;
                else changed = true;
            }
            if (destination->layers.size() != source->layers.size()) continue;
            for (size_t layer = 0; layer < source->layers.size(); ++layer)
            {
                const size_t di = dst * source->layers.size() + layer;
                const size_t si = src * source->layers.size() + layer;
                if (destination->weights[di] == source->weights[si]) continue;
                destination->weights[di] = source->weights[si];
                if (useIncoming) neighborChanged = true;
                else changed = true;
            }
        }
        if (neighborChanged)
        {
            anyChanged = true;
            neighbor->Rebuild();
            neighbor->MarkConfigurationDirty();
            if (m_proceduralCells.count(neighborCell) && !cellDataDirectory.empty())
            {
                m_editedProcedural.insert(neighborCell);
                m_dirtyProceduralEdits.insert(neighborCell);
            }
            else
            {
                m_proceduralCells.erase(neighborCell);
                m_cells[neighborCell] = neighbor->Serialize();
            }
        }
    }
    if (changed)
    {
        anyChanged = true;
        terrain->Rebuild();
        terrain->MarkConfigurationDirty();
        if (m_proceduralCells.count(cell) && !cellDataDirectory.empty())
        {
            m_editedProcedural.insert(cell);
            m_dirtyProceduralEdits.insert(cell);
        }
        else
        {
            m_proceduralCells.erase(cell);
            m_cells[cell] = terrain->Serialize();
        }
    }
    if (anyChanged) MarkConfigurationDirty();
}

bool TerrainManager::AddCell(int x, int y)
{
    if (Owner && Owner->GetScene() && Owner->GetScene()->HasStarted()) return false;
    if (HasCell(x, y)) return false;
    Terrain initial;
    initial.Resize(cellsX, cellsY, cellWidth, cellDepth);
    m_cells[{x, y}] = initial.Serialize();
    LoadCell({x, y});
    Stitch({x, y});
    if (auto* loaded = GetLoadedCell(x, y)) m_cells[{x, y}] = loaded->Serialize();
    MarkConfigurationDirty();
    return true;
}

bool TerrainManager::AddProceduralCell(int x, int y)
{
    if (Owner && Owner->GetScene() && Owner->GetScene()->HasStarted()) return false;
    if (HasCell(x, y) || !m_proceduralBuilder) return false;
    m_proceduralCells.insert({x, y});
    MarkConfigurationDirty();
    return true;
}

bool TerrainManager::RemoveCell(int x, int y)
{
    if (Owner && Owner->GetScene() && Owner->GetScene()->HasStarted()) return false;
    if (!HasCell(x, y)) return false;
    InvalidatePrepared();
    UnloadCell({x, y});
    m_cells.erase({x, y});
    m_objectLayers.erase({x, y});
    m_proceduralCells.erase({x, y});
    m_editedProcedural.erase({x, y});
    m_dirtyProceduralEdits.erase({x, y});
    if (!cellDataDirectory.empty())
    {
        std::error_code error;
        std::filesystem::remove(std::filesystem::path(cellDataDirectory) /
            (std::to_string(x) + "_" + std::to_string(y) + ".terrainedit"), error);
    }
    MarkConfigurationDirty();
    return true;
}

bool TerrainManager::ApplyBrush(double x, double z, float radius, float strength,
    Terrain::Brush brush, int layer, bool realignObjects,
    const Terrain::BrushOptions& options)
{
    if (Owner && Owner->GetScene() && Owner->GetScene()->HasStarted()) return false;
    if (!std::isfinite(x) || !std::isfinite(z) || !std::isfinite(radius) ||
        !std::isfinite(strength) || radius <= 0.f || strength <= 0.f ||
        (brush == Terrain::Brush::Paint && layer < 0)) return false;
    // A shaped brush's turned square reaches past the radius.
    const double reach = Terrain::BrushReach(radius, options);
    const double spanX = static_cast<double>(cellsX) * cellWidth;
    const double spanZ = static_cast<double>(cellsY) * cellDepth;
    const auto cellFloor = [](double coordinate) {
        return static_cast<int64_t>(std::floor(std::clamp(coordinate,
            static_cast<double>(std::numeric_limits<int>::min()),
            static_cast<double>(std::numeric_limits<int>::max()))));
    };
    const int64_t minX = cellFloor((static_cast<double>(x) - reach) / spanX);
    const int64_t maxX = cellFloor((static_cast<double>(x) + reach) / spanX);
    const int64_t minZ = cellFloor((static_cast<double>(z) - reach) / spanZ);
    const int64_t maxZ = cellFloor((static_cast<double>(z) + reach) / spanZ);
    std::set<Cell> targets;
    const uint64_t candidateWidth = static_cast<uint64_t>(maxX - minX + 1);
    const uint64_t candidateHeight = static_cast<uint64_t>(maxZ - minZ + 1);
    if (candidateWidth > 4096 || candidateHeight > 4096 ||
        candidateWidth * candidateHeight > 4096)
    {
        for (Cell cell : GetCells())
            if (cell.first >= minX && cell.first <= maxX &&
                cell.second >= minZ && cell.second <= maxZ)
                targets.insert(cell);
    }
    else
        for (int64_t row = minZ; row <= maxZ; ++row)
            for (int64_t col = minX; col <= maxX; ++col)
                if (HasCell(static_cast<int>(col), static_cast<int>(row)))
                    targets.emplace(static_cast<int>(col), static_cast<int>(row));
    for (Cell cell : targets)
        if (!GetLoadedCell(cell.first, cell.second))
            LoadCell(cell, m_haveLastCenter
                ? DesiredLodStride(cell, m_lastCenter.first, m_lastCenter.second)
                : 1);
    // Authored cells adopt the manager's paint detail on their first stroke.
    if (brush == Terrain::Brush::Paint && paintResolution > 0)
        for (Cell cell : targets)
            if (auto* terrain = GetLoadedCell(cell.first, cell.second);
                terrain && !m_proceduralCells.count(cell) &&
                terrain->paintResolution != paintResolution)
                terrain->SetPaintResolution(paintResolution);
    // A backed stamp looks behind each point, up to the shape's far side
    // (another 2 x reach), possibly into cells the brush does not touch: it
    // reads the loaded cells' heights as they were before this dab, so cells
    // changed earlier in the loop do not move the ground the later ones see.
    std::shared_ptr<std::map<Cell, std::vector<float>>> backingCells;
    if (options.backed && options.shape && brush == Terrain::Brush::Raise && !options.backingHeight)
    {
        backingCells = std::make_shared<std::map<Cell, std::vector<float>>>();
        const double look = 3.0 * reach;
        for (int64_t row = cellFloor((z - look) / spanZ); row <= cellFloor((z + look) / spanZ); ++row)
            for (int64_t col = cellFloor((x - look) / spanX); col <= cellFloor((x + look) / spanX); ++col)
                if (const auto* terrain = GetLoadedCell(int(col), int(row));
                    terrain && terrain->heights.size() == size_t(cellsX + 1) * (cellsY + 1))
                    (*backingCells)[{int(col), int(row)}] = terrain->heights;
    }
    std::set<Cell> changedCells;
    for (Cell cell : targets)
        if (auto* terrain = GetLoadedCell(cell.first, cell.second))
        {
            Terrain::BrushOptions cellOptions = options;
            if (backingCells)
                cellOptions.backingHeight = [this, backingCells, cell, spanX, spanZ](
                    float localX, float localZ, float& height)
                {
                    const double worldX = cell.first * spanX + localX, worldZ = cell.second * spanZ + localZ;
                    const Cell at{int(std::floor(worldX / spanX)), int(std::floor(worldZ / spanZ))};
                    const auto found = backingCells->find(at);
                    return found != backingCells->end() && SampleTerrainSurface(found->second, cellsX, cellsY,
                        cellWidth, cellDepth, float(worldX - at.first * spanX), float(worldZ - at.second * spanZ),
                        height);
                };
            if (terrain->ApplyBrush(
                static_cast<float>(static_cast<double>(x) - cell.first * spanX),
                static_cast<float>(static_cast<double>(z) - cell.second * spanZ),
                radius, strength, brush, layer, realignObjects, cellOptions))
                changedCells.insert(cell);
        }
    if (changedCells.empty()) return false;

    // Paint maps: border samples are shared like vertices. Average the copies
    // held by cells of equal paint resolution so painting across a cell edge
    // leaves no seam; the vertex weights follow when each cell rebuilds.
    if (brush == Terrain::Brush::Paint)
    {
        using PaintKey = std::pair<int64_t, int64_t>;
        std::map<PaintKey, std::vector<std::pair<Terrain*, size_t>>> paintShared;
        for (Cell cell : changedCells)
        {
            auto* terrain = GetLoadedCell(cell.first, cell.second);
            if (!terrain || !terrain->HasPaintMap()) continue;
            const int resolution = terrain->paintResolution;
            const int width = terrain->PaintWidth(), height = terrain->PaintHeight();
            const double stepX = double(terrain->cellWidth) / resolution;
            const double stepZ = double(terrain->cellDepth) / resolution;
            for (int row = 0; row < height; ++row)
                for (int col = 0; col < width; ++col)
                {
                    if (col != 0 && col != width - 1 && row != 0 && row != height - 1) continue;
                    const int64_t sx = int64_t(cell.first) * (width - 1) + col;
                    const int64_t sz = int64_t(cell.second) * (height - 1) + row;
                    const double dx = sx * stepX - x, dz = sz * stepZ - z;
                    if (dx * dx + dz * dz >= reach * reach) continue;
                    // Resolution is part of the key: only equal grids share samples.
                    paintShared[{sx * 32 + resolution, sz}].emplace_back(terrain,
                        size_t(row) * width + col);
                }
        }
        std::set<Terrain*> stitched;
        for (const auto& [key, owners] : paintShared)
        {
            if (owners.size() < 2) continue;
            const size_t layerCount = owners.front().first->layers.size();
            std::vector<float> average(layerCount, 0.f);
            size_t contributors = 0;
            for (const auto& [terrain, index] : owners)
            {
                if (terrain->layers.size() != layerCount) continue;
                for (size_t l = 0; l < layerCount; ++l)
                    average[l] += terrain->paintWeights[index * layerCount + l];
                ++contributors;
            }
            if (contributors < 2) continue;
            for (float& value : average) value /= float(contributors);
            for (const auto& [terrain, index] : owners)
            {
                if (terrain->layers.size() != layerCount) continue;
                std::copy(average.begin(), average.end(),
                    terrain->paintWeights.begin() + std::ptrdiff_t(index * layerCount));
                stitched.insert(terrain);
            }
        }
        for (auto* terrain : stitched)
        {
            terrain->NotifyPaintChanged();
            terrain->Rebuild();
        }
    }

    // A border sample belongs to two cells (four at a corner). Average the
    // cells changed by this stroke once, then write the same value to every
    // owner so smoothing brushes and paint weights cannot open a seam.
    using SampleKey = std::pair<int64_t, int64_t>;
    using SampleRef = std::pair<Cell, size_t>;
    std::map<SampleKey, std::vector<SampleRef>> shared;
    for (Cell cell : targets)
    {
        auto* terrain = GetLoadedCell(cell.first, cell.second);
        if (!terrain) continue;
        for (int row = 0; row <= cellsY; ++row)
            for (int col = 0; col <= cellsX; ++col)
            {
                if (col != 0 && col != cellsX && row != 0 && row != cellsY)
                    continue;
                const int64_t sx = static_cast<int64_t>(cell.first) * cellsX + col;
                const int64_t sz = static_cast<int64_t>(cell.second) * cellsY + row;
                const double dx = sx * static_cast<double>(cellWidth) - x;
                const double dz = sz * static_cast<double>(cellDepth) - z;
                if (dx * dx + dz * dz >= reach * reach)
                    continue;
                shared[{sx, sz}].emplace_back(cell,
                    static_cast<size_t>(row * (cellsX + 1) + col));
            }
    }
    std::set<Cell> seamChanged;
    for (const auto& [sample, owners] : shared)
    {
        if (owners.size() < 2) continue;
        float height = 0.f;
        std::vector<float> weights;
        size_t contributors = 0;
        for (const auto& [cell, index] : owners)
        {
            if (!changedCells.count(cell)) continue;
            const auto* terrain = GetLoadedCell(cell.first, cell.second);
            if (weights.empty()) weights.resize(terrain->layers.size(), 0.f);
            if (weights.size() != terrain->layers.size()) continue;
            height += terrain->heights[index];
            for (size_t channel = 0; channel < weights.size(); ++channel)
                weights[channel] += terrain->weights[index * weights.size() + channel];
            ++contributors;
        }
        if (!contributors) continue;
        height /= static_cast<float>(contributors);
        for (float& weight : weights) weight /= static_cast<float>(contributors);
        for (const auto& [cell, index] : owners)
        {
            auto* terrain = GetLoadedCell(cell.first, cell.second);
            bool different = terrain->heights[index] != height;
            terrain->heights[index] = height;
            if (terrain->layers.size() == weights.size())
                for (size_t channel = 0; channel < weights.size(); ++channel)
                {
                    const size_t offset = index * weights.size() + channel;
                    different |= terrain->weights[offset] != weights[channel];
                    terrain->weights[offset] = weights[channel];
                }
            if (different) { seamChanged.insert(cell); changedCells.insert(cell); }
        }
    }
    for (Cell cell : seamChanged)
    {
        auto* terrain = GetLoadedCell(cell.first, cell.second);
        terrain->Rebuild();
        terrain->MarkConfigurationDirty();
    }
    RecordEditedCells(changedCells);
    return true;
}

void TerrainManager::RecordEditedCells(const std::set<Cell>& changedCells)
{
    if (changedCells.empty()) return;
    for (Cell cell : changedCells)
    {
        auto* terrain = GetLoadedCell(cell.first, cell.second);
        if (!terrain) continue;
        if (m_proceduralCells.count(cell) && !cellDataDirectory.empty())
        {
            m_editedProcedural.insert(cell);
            m_dirtyProceduralEdits.insert(cell);
        }
        else
        {
            m_proceduralCells.erase(cell);
            m_cells[cell] = terrain->Serialize();
        }
    }
    RefreshFarTerrainEditedCells(changedCells);
    MarkConfigurationDirty();
}

void TerrainManager::SyncLoadedCells(int centerX, int centerY)
{
    if (manageAuthoredCellColliders)
    { StreamPrefabCells(centerX, centerY); RefreshAuthoredCollisionCells(); RefreshColliderActivation(centerX, centerY); return; }
    if (proceduralOakMeadow && asyncStreaming)
    {
        StreamCells(centerX, centerY, loadedRadius);
        return;
    }
    const int radius = m_editorStreaming ? std::clamp(editorRadius, 0, 32) :
        std::clamp(m_farEnabled && !m_farHeights.empty() ? loadedRadius :
            std::max(loadedRadius, farRadius), 0, 32);
    for (int y = centerY - radius; y <= centerY + radius; ++y)
        for (int x = centerX - radius; x <= centerX + radius; ++x)
            if (HasCell(x, y))
            {
                const Cell cell{x, y};
                LoadCell(cell, DesiredLodStride(cell, centerX, centerY));
                if (auto* terrain = GetLoadedCell(x, y))
                    terrain->SetMeshLodStride(DesiredLodStride(cell, centerX, centerY));
            }
    for (auto it = m_loaded.begin(); it != m_loaded.end(); )
    {
        const Cell cell = it->first;
        ++it;
        if (std::abs(cell.first - centerX) > radius ||
            std::abs(cell.second - centerY) > radius)
            UnloadCell(cell);
    }
    RefreshColliderActivation(centerX, centerY);
    RefreshRenderBatches(64, centerX, centerY, radius);
    TrimMemoryBudget(centerX, centerY);
}

}

#include "TerrainManager.h"
#include "Core/Components/Terrain/TerrainFarCellDetail.h"
#include "Core/Components/Terrain/TerrainField.h"
#include "Core/Components/Terrain/TerrainSurfaceSample.h"
#include "Core/Scene/Object.h"

#include <cstdio>
#include <cstdlib>

#include <chrono>
#include <filesystem>
#include <mutex>
#include <set>

namespace Engine::Components
{
// What the field's seed reads (todo.md "Terrain: volume-first conversion",
// step 1): exactly the heights loaded cells are built from. The procedural
// builder when there is one (it also sinks worn trail tracks into the
// archive's heights, PaintCell), else the height archive; then the saved
// height edits of cells the manager treats as edited (LoadCell applies only
// those), and live editor edits until they are saved. Checked against every
// loaded cell with ENGINE_TERRAIN_FIELD_CHECK=1.
struct TerrainManager::FieldSource
{
    std::string archivePath;
    uint64_t signature = 0;
    int extent = 0;
    TerrainFieldLayout layout;
    std::string dataDirectory;
    std::function<void(Cell, Terrain&)> builder;
    std::once_flag archiveOnce;
    std::shared_ptr<const FarCellDetail::Archive> archive;
    std::mutex overrideMutex;
    std::map<Cell, std::shared_ptr<const std::vector<float>>> overrides;
    std::set<Cell> editedCells; // under overrideMutex

    std::string CellFile(Cell cell, const char* extension) const
    {
        return dataDirectory.empty() ? std::string{} :
            (std::filesystem::path(dataDirectory) /
                (std::to_string(cell.first) + "_" + std::to_string(cell.second) + extension)).string();
    }

    bool Heights(Cell cell, std::vector<float>& heights)
    {
        {
            std::lock_guard<std::mutex> lock(overrideMutex);
            if (const auto found = overrides.find(cell); found != overrides.end())
            {
                heights = *found->second;
                return true;
            }
        }
        const size_t expected = static_cast<size_t>(layout.cellsX + 1) * (layout.cellsY + 1);
        bool read = false;
        if (builder)
        {
            // As far objects build cells off the main thread (FarPlacements).
            Engine::Core::Object object;
            auto* terrain = object.AddComponent<Terrain>();
            terrain->Resize(layout.cellsX, layout.cellsY, layout.cellWidth, layout.cellDepth);
            builder(cell, *terrain);
            heights = terrain->heights;
            read = heights.size() == expected;
        }
        if (!read && !archivePath.empty())
        {
            std::call_once(archiveOnce, [this] {
                auto loaded = std::make_shared<FarCellDetail::Archive>();
                if (loaded->Load(archivePath, signature, extent, layout.cellsX, layout.cellsY))
                    archive = std::move(loaded);
            });
            read = archive && archive->ReadCell(cell.first, cell.second, heights);
        }
        bool edited = false;
        {
            std::lock_guard<std::mutex> lock(overrideMutex);
            edited = editedCells.count(cell) != 0;
        }
        if (read && edited)
            if (const auto path = CellFile(cell, ".terrainedit"); !path.empty())
            {
                std::error_code error;
                if (std::filesystem::exists(path, error))
                    TerrainHeightArchive::ApplyEditHeights(path, layout.cellsX, layout.cellsY, heights);
            }
        return read && heights.size() == expected;
    }
};

bool TerrainManager::ObjectLayerContext::SampleSurface(float x, float z,
    float& height, glm::vec3* normal) const
{
    if (field)
    {
        const auto& layout = field->Layout();
        const auto surface = field->SurfaceAt(cell.first * layout.SpanX() + x,
            cell.second * layout.SpanZ() + z);
        if (!surface) return false;
        height = surface->height;
        if (normal) *normal = surface->normal;
        return true;
    }
    return heights && SampleTerrainSurface(*heights, cellsX, cellsY,
        cellWidth, cellDepth, x, z, height, normal);
}

std::shared_ptr<const TerrainField> TerrainManager::GetTerrainField()
{
    uint64_t key = 1469598103934665603ull;
    const auto mix = [&key](const void* data, size_t size) {
        for (size_t i = 0; i < size; ++i)
            key = (key ^ static_cast<const uint8_t*>(data)[i]) * 1099511628211ull;
    };
    mix(&cellsX, sizeof(cellsX)); mix(&cellsY, sizeof(cellsY));
    mix(&cellWidth, sizeof(cellWidth)); mix(&cellDepth, sizeof(cellDepth));
    mix(m_farHeightArchivePath.data(), m_farHeightArchivePath.size());
    mix(&m_farSourceSignature, sizeof(m_farSourceSignature));
    mix(&m_farHalfExtentCells, sizeof(m_farHalfExtentCells));
    mix(cellDataDirectory.data(), cellDataDirectory.size());
    const bool hasBuilder = static_cast<bool>(m_proceduralBuilder);
    mix(&hasBuilder, sizeof(hasBuilder));
    if (m_terrainField && key == m_terrainFieldKey)
    {
        SeedAuthoredCells();
        return m_terrainField;
    }

    auto source = std::make_shared<FieldSource>();
    source->archivePath = m_farHeightArchivePath;
    source->signature = m_farSourceSignature;
    source->extent = m_farHalfExtentCells;
    source->layout = {cellsX, cellsY, cellWidth, cellDepth};
    source->dataDirectory = cellDataDirectory;
    source->builder = m_proceduralBuilder;
    if (m_fieldSource)
    {
        // Unsaved editor edits carry over.
        std::lock_guard<std::mutex> lock(m_fieldSource->overrideMutex);
        source->overrides = m_fieldSource->overrides;
    }
    source->editedCells = m_editedProcedural;
    m_fieldSource = source;
    ++m_terrainFieldRevision;
    m_terrainField = std::make_shared<TerrainField>(source->layout,
        [source](Cell cell, std::vector<float>& heights) { return source->Heights(cell, heights); },
        [source](Cell cell, std::vector<TerrainVolumeEdit>& edits) {
            const auto path = source->CellFile(cell, ".volumeedit");
            std::error_code error;
            if (path.empty() || !std::filesystem::exists(path, error)) return false;
            return LoadTerrainVolumeEdits(path, edits);
        });
    m_terrainFieldKey = key;
    SeedAuthoredCells();
    return m_terrainField;
}

void TerrainManager::SeedAuthoredCells()
{
    // Authored cells (saved with the scene: no builder or archive gives
    // them) seed the field from their loaded heights.
    if (!m_fieldSource || !m_terrainField) return;
    const size_t expected = static_cast<size_t>(cellsX + 1) * (cellsY + 1);
    for (const auto& [cell, terrain] : m_loaded)
    {
        if (!terrain || !m_cells.count(cell) || terrain->heights.size() != expected) continue;
        {
            std::lock_guard<std::mutex> lock(m_fieldSource->overrideMutex);
            if (m_fieldSource->overrides.count(cell)) continue;
            m_fieldSource->overrides[cell] = std::make_shared<const std::vector<float>>(terrain->heights);
        }
        m_terrainField->Invalidate(cell);
        ++m_terrainFieldRevision;
    }
}

void TerrainManager::InvalidateTerrainField(Cell cell, const std::vector<float>* heights)
{
    if (!m_fieldSource || !m_terrainField) return;
    if (heights && heights->size() == static_cast<size_t>(cellsX + 1) * (cellsY + 1))
    {
        std::lock_guard<std::mutex> lock(m_fieldSource->overrideMutex);
        m_fieldSource->overrides[cell] = std::make_shared<const std::vector<float>>(*heights);
        m_fieldSource->editedCells.insert(cell);
    }
    m_terrainField->Invalidate(cell);
    ++m_terrainFieldRevision;
}

std::shared_ptr<const TerrainField> TerrainManager::EditedFieldFor(const Terrain& terrain,
    float& originX, float& originZ)
{
    if (!Owner || !terrain.Owner || cellsX <= 0 || cellsY <= 0) return nullptr;
    const glm::dvec3 local = glm::dvec3(glm::inverse(Owner->transform.GetWorldMatrixD()) *
        terrain.Owner->transform.GetWorldMatrixD()[3]);
    const double spanX = static_cast<double>(cellsX) * cellWidth;
    const double spanZ = static_cast<double>(cellsY) * cellDepth;
    const Cell cell{static_cast<int>(std::lround(local.x / spanX)),
        static_cast<int>(std::lround(local.z / spanZ))};
    auto field = GetTerrainField();
    if (!field || !field->HasEdits(cell)) return nullptr;
    originX = static_cast<float>(cell.first * spanX);
    originZ = static_cast<float>(cell.second * spanZ);
    return field;
}
}

namespace Engine::Components
{
void TerrainManager::CheckTerrainField()
{
    static const bool enabled = [] {
        const char* value = std::getenv("ENGINE_TERRAIN_FIELD_CHECK");
        return value && value[0] == '1';
    }();
    const int side = 2 * std::max(0, loadedRadius) + 1;
    if (!enabled || m_terrainFieldChecked || static_cast<int>(m_loaded.size()) < side * side) return;
    m_terrainFieldChecked = true;
    // Timing: a fresh field, so the first pass seeds every cell.
    m_terrainField.reset();
    const auto field = GetTerrainField();
    const auto started = std::chrono::steady_clock::now();
    const size_t expected = static_cast<size_t>(cellsX + 1) * (cellsY + 1);
    const float spanX = cellsX * cellWidth, spanZ = cellsY * cellDepth;
    size_t samples = 0, missing = 0, cells = 0;
    float worst = 0.f;
    Cell worstCell{};
    std::map<Cell, float> cellErrors;
    for (const auto& [cell, terrain] : m_loaded)
    {
        if (!terrain || terrain->heights.size() != expected) continue;
        ++cells;
        // Every grid point and the centre of every grid square.
        for (int row = 0; row < cellsY; ++row)
            for (int col = 0; col < cellsX; ++col)
                for (const float offset : {0.f, .5f})
                {
                    const float localX = (col + offset) * cellWidth, localZ = (row + offset) * cellDepth;
                    float height = 0.f;
                    SampleTerrainSurface(terrain->heights, cellsX, cellsY, cellWidth, cellDepth,
                        localX, localZ, height);
                    const auto surface = field->SurfaceAt(cell.first * spanX + localX,
                        cell.second * spanZ + localZ);
                    ++samples;
                    if (!surface) { ++missing; continue; }
                    const float error = std::abs(surface->height - height);
                    if (error > worst) { worst = error; worstCell = cell; }
                    if (error > .01f) cellErrors[cell] = std::max(cellErrors[cell], error);
                }
    }
    const double coldMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    // Warm: the same queries again from the cache.
    const auto warmStart = std::chrono::steady_clock::now();
    size_t warmQueries = 0;
    for (const auto& [cell, terrain] : m_loaded)
        for (int i = 0; i < 2048; ++i, ++warmQueries)
            field->SurfaceAt(cell.first * spanX + (i % 64) * spanX / 64.f, cell.second * spanZ + (i / 64) * spanZ / 32.f);
    const double warmMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - warmStart).count();
    std::printf("[TerrainField] cold pass %.1f ms (%.2f ms a cell, seeding included); warm %.3f us a query\n",
        coldMs, coldMs / std::max<size_t>(1, cells), warmMs * 1000.0 / std::max<size_t>(1, warmQueries));
    std::printf("[TerrainField] check: %zu loaded cells, %zu samples, %zu without a surface, "
        "worst height difference %.4f m (cell %d,%d)\n",
        cells, samples, missing, worst, worstCell.first, worstCell.second);
    for (const auto& [cell, error] : cellErrors)
        std::printf("[TerrainField]   cell %d,%d differs by up to %.3f m%s\n", cell.first, cell.second, error,
            m_editedProcedural.count(cell) ? " (edited)" : "");
    std::fflush(stdout);
}
}

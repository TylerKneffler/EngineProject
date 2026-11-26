#include "Core/Components/Terrain/TerrainManager.h"
#include "Core/Components/Terrain/TerrainField.h"
#include "Core/Components/Terrain/TerrainObjectLayer.h"
#include "Core/Scene/Object.h"
#include "Core/Scene/Scene.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <mutex>

// Foliage density (TerrainFoliage.h): painted chunks per cell, the brush, and
// the files with their undo history.
namespace Engine::Components
{
namespace
{
// Every foliage state of a file seen this session, by content signature, so
// a restored snapshot (undo) can bring it back.
struct FoliageHistory
{
    std::mutex mutex;
    std::map<std::string, std::map<uint64_t, std::shared_ptr<const TerrainFoliageCell>>> files;
};
FoliageHistory& History()
{
    static FoliageHistory history;
    return history;
}

uint64_t SignatureOf(const TerrainFoliageCell& cell) { return cell.Empty() ? 0ull : cell.Signature(); }

void Remember(const std::string& path, const std::shared_ptr<const TerrainFoliageCell>& cell)
{
    if (path.empty() || !cell) return;
    auto& history = History();
    std::lock_guard lock(history.mutex);
    history.files[path].emplace(SignatureOf(*cell), cell);
}

const std::shared_ptr<const TerrainFoliageCell>& EmptyCell()
{
    static const auto empty = std::make_shared<const TerrainFoliageCell>();
    return empty;
}

float Falloff(float distance, float radius, float falloff)
{
    if (distance >= radius) return 0.f;
    const float edge = radius * std::clamp(falloff, 0.f, 1.f);
    if (edge <= 0.f || distance <= radius - edge) return 1.f;
    const float t = (radius - distance) / edge;
    return t * t * (3.f - 2.f * t);
}
}

std::string TerrainManager::GetFoliageFilePath(Cell cell) const
{
    return cellDataDirectory.empty() ? std::string{} :
        (std::filesystem::path(cellDataDirectory) /
            (std::to_string(cell.first) + "_" + std::to_string(cell.second) + ".foliage")).string();
}

std::shared_ptr<const TerrainFoliageCell> TerrainManager::GetFoliage(Cell cell) const
{
    if (const auto found = m_foliage.find(cell); found != m_foliage.end()) return found->second;
    auto loaded = std::make_shared<TerrainFoliageCell>();
    const std::string path = GetFoliageFilePath(cell);
    if (!TerrainFoliageCell::Load(path, *loaded))
    {
        std::fprintf(stderr, "[TerrainManager] unreadable foliage file %s\n", path.c_str());
        loaded->layers.clear();
    }
    std::shared_ptr<const TerrainFoliageCell> result = std::move(loaded);
    if (result->Empty()) result = EmptyCell();
    Remember(path, result);
    m_foliage[cell] = result;
    return result;
}

uint64_t TerrainManager::GetFoliageRevision(Cell cell) const
{
    const auto found = m_foliageRevisions.find(cell);
    return found == m_foliageRevisions.end() ? 0 : found->second;
}

std::optional<TerrainManager::Cell> TerrainManager::CellOf(const Terrain& terrain) const
{
    for (const auto& [cell, loaded] : m_loaded)
        if (loaded == &terrain) return cell;
    return std::nullopt;
}

void TerrainManager::SetFoliage(Cell cell, std::shared_ptr<const TerrainFoliageCell> foliage)
{
    if (!foliage || foliage->Empty()) foliage = EmptyCell();
    m_foliage[cell] = foliage;
    m_foliageDirty.insert(cell);
    m_foliageRevisions[cell] = ++m_foliageRevisionCounter;
}

bool TerrainManager::PaintFoliage(const std::string& layerName, const TerrainFoliageStroke& stroke)
{
    if (layerName.empty() || stroke.radius <= 0.f || stroke.strength <= 0.f) return false;
    const auto field = GetTerrainField();
    const float spanX = cellsX * cellWidth, spanZ = cellsY * cellDepth;
    const int x0 = static_cast<int>(std::floor((stroke.centre.x - stroke.radius) / spanX));
    const int x1 = static_cast<int>(std::floor((stroke.centre.x + stroke.radius) / spanX));
    const int z0 = static_cast<int>(std::floor((stroke.centre.z - stroke.radius) / spanZ));
    const int z1 = static_cast<int>(std::floor((stroke.centre.z + stroke.radius) / spanZ));
    bool changed = false;
    for (int cz = z0; cz <= z1; ++cz)
        for (int cx = x0; cx <= x1; ++cx)
        {
            const Cell cell{cx, cz};
            const auto loaded = m_loaded.find(cell);
            if (loaded == m_loaded.end() || !loaded->second || !loaded->second->Owner) continue;
            const Terrain& terrain = *loaded->second;
            // The component growing this layer gives the baseline.
            const TerrainFoliageSource* source = nullptr;
            for (const auto* child : terrain.Owner->Children)
                if (child && !source)
                    for (const auto* component : child->Components)
                        if (const auto* candidate = dynamic_cast<const TerrainFoliageSource*>(component);
                            candidate && candidate->FoliageLayerName() == layerName)
                        { source = candidate; break; }
            const glm::vec3 origin(cx * spanX, 0.f, cz * spanZ);
            const glm::vec3 centre = stroke.centre - origin;
            const auto normalAt = [&](const glm::vec3& local) {
                return field ? field->Normal(local + origin) : glm::vec3(0.f, 1.f, 0.f);
            };
            const auto baseline = [&](const glm::vec3& local) {
                return source ? std::clamp(source->FoliageBaseline(terrain, local, normalAt(local)), 0.f, 1.f) : 0.f;
            };
            auto edited = std::make_shared<TerrainFoliageCell>(*GetFoliage(cell));
            auto& layer = edited->layers[layerName];
            bool cellChanged = false;
            const glm::ivec3 low = TerrainFoliageCell::VoxelOf(centre - stroke.radius);
            const glm::ivec3 high = TerrainFoliageCell::VoxelOf(centre + stroke.radius);
            for (int vz = low.z; vz <= high.z; ++vz)
                for (int vy = low.y; vy <= high.y; ++vy)
                    for (int vx = low.x; vx <= high.x; ++vx)
                    {
                        const glm::ivec3 voxel(vx, vy, vz);
                        const glm::vec3 at = TerrainFoliageCell::VoxelCentre(voxel);
                        // Only this cell's own voxels (neighbours paint theirs).
                        if (at.x < 0.f || at.z < 0.f || at.x >= spanX || at.z >= spanZ) continue;
                        const float weight = Falloff(glm::length(at - centre), stroke.radius, stroke.falloff) *
                            std::clamp(stroke.strength, 0.f, 1.f);
                        if (weight <= 0.f) continue;
                        // Near the surface only (density is about metres to it).
                        if (field && std::abs(field->Density(at + origin)) > stroke.shell) continue;
                        const float ruled = stroke.weight ? weight * std::clamp(stroke.weight(terrain, at, normalAt(at)), 0.f, 1.f)
                            : weight;
                        if (ruled <= 0.f) continue;
                        const auto key = TerrainFoliageCell::ChunkOf(voxel);
                        auto chunk = layer.chunks.find(key);
                        if (chunk == layer.chunks.end())
                        {
                            // A new chunk: every voxel at the baseline (offset 0).
                            TerrainFoliageCell::Chunk values;
                            values.fill(128);
                            chunk = layer.chunks.emplace(key, values).first;
                        }
                        uint8_t& stored = chunk->second[static_cast<size_t>(TerrainFoliageCell::IndexInChunk(voxel))];
                        const uint8_t before = stored;
                        const float offset = TerrainFoliageCell::DecodeOffset(stored);
                        float next = offset;
                        switch (stroke.mode)
                        {
                        case TerrainFoliageStroke::Mode::Add: next = offset + ruled * (1.f - offset); break;
                        case TerrainFoliageStroke::Mode::Erase: next = offset + ruled * (-1.f - offset); break;
                        case TerrainFoliageStroke::Mode::Set:
                        {
                            // Toward the offset that makes the density the
                            // target here (the baseline at the voxel).
                            const float wanted = std::clamp(stroke.target, 0.f, 1.f) - baseline(at);
                            next = offset + (wanted - offset) * ruled;
                            break;
                        }
                        }
                        stored = TerrainFoliageCell::EncodeOffset(next);
                        // A dab always moves at least one step toward its goal.
                        if (stored == before && std::abs(next - offset) > 1e-6f)
                            stored = static_cast<uint8_t>(std::clamp(int(before) + (next > offset ? 1 : -1), 1, 255));
                        cellChanged |= stored != before;
                    }
            if (!cellChanged) continue;
            SetFoliage(cell, std::move(edited));
            changed = true;
        }
    return changed;
}

size_t TerrainManager::SaveFoliageFiles() const
{
    // Play (a game, a capture) never writes the editor's cell data.
    if (Owner && Owner->GetScene() && Owner->GetScene()->HasStarted()) return 0;
    size_t written = 0;
    for (auto it = m_foliageDirty.begin(); it != m_foliageDirty.end(); it = m_foliageDirty.erase(it))
    {
        const std::string path = GetFoliageFilePath(*it);
        const auto found = m_foliage.find(*it);
        if (path.empty() || found == m_foliage.end()) continue;
        if (TerrainFoliageCell::Save(path, *found->second))
        {
            Remember(path, found->second);
            ++written;
        }
        else
            std::fprintf(stderr, "[TerrainManager] could not write %s\n", path.c_str());
    }
    return written;
}

void TerrainManager::RestoreFoliageFiles(const JsonValue& listed)
{
    std::map<Cell, uint64_t> wanted;
    for (size_t i = 0; i < listed.ArraySize(); ++i)
    {
        const auto& entry = listed.ArrayAt(i);
        wanted[{entry["x"].AsInt(), entry["y"].AsInt()}] = std::stoull(entry["signature"].AsString());
    }
    // Cells painted now but not in the snapshot go back to no paint.
    for (const auto& [cell, foliage] : m_foliage)
        if (foliage && !foliage->Empty() && !wanted.count(cell)) wanted[cell] = 0;
    for (const auto& [cell, signature] : wanted)
    {
        if (SignatureOf(*GetFoliage(cell)) == signature) continue;
        std::shared_ptr<const TerrainFoliageCell> state;
        if (!signature) state = EmptyCell();
        else
        {
            auto& history = History();
            std::lock_guard lock(history.mutex);
            const auto file = history.files.find(GetFoliageFilePath(cell));
            if (file != history.files.end())
                if (const auto found = file->second.find(signature); found != file->second.end())
                    state = found->second;
        }
        if (!state) continue;   // not seen this session: keep what is there
        SetFoliage(cell, std::move(state));
    }
}
}

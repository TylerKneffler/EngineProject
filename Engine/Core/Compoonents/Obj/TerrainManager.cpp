#include "TerrainManager.h"
#include "Core/Object.h"
#include "Core/Scene/Scene.h"
#include "Core/Compoonents/Obj/TerrainBatchAppearance.h"
#include "Core/Compoonents/Camera/Camera.h"
#include "Core/Compoonents/Obj/Mesh.h"
#include "Core/Compoonents/Materials/Material.h"
#include "Core/Compoonents/Materials/Texture.h"
#include "Core/Compoonents/Physics/MeshObjectCollider.h"
#include "Core/Compoonents/Physics/RigidBody.h"
#include "Engine/Editor/UI/IEditorUi.h"
#include <algorithm>
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

namespace
{
template<class T> bool WriteBinary(std::ofstream& file, const T& value)
{
    static_assert(std::is_trivially_copyable_v<T>);
    file.write(reinterpret_cast<const char*>(&value), sizeof(value));
    return static_cast<bool>(file);
}
template<class T> bool ReadBinary(std::ifstream& file, T& value)
{
    static_assert(std::is_trivially_copyable_v<T>);
    file.read(reinterpret_cast<char*>(&value), sizeof(value));
    return static_cast<bool>(file);
}

}

namespace Engine::Components
{
static bool LoadEditFile(const std::string& path, Terrain& terrain);
TerrainManager::TerrainManager()
{
    SetTypeName(COMPONENT_TYPE_NAME(TerrainManager));
    singlecomponent = true;
}

TerrainManager::~TerrainManager()
{
    {
        std::lock_guard<std::mutex> lock(m_farPrepareMutex);
        m_stopFarWorker = true;
        m_farPrepareQueue.clear();
    }
    m_farPrepareWake.notify_one();
    if (m_farWorker.joinable()) m_farWorker.join();
    {
        std::lock_guard<std::mutex> lock(m_prepareMutex);
        m_stopPrepareWorker = true;
        m_prepareQueue.clear();
    }
    m_prepareWake.notify_one();
    if (m_prepareWorker.joinable()) m_prepareWorker.join();
}

bool TerrainManager::HasCell(int x, int y) const
{
    return m_cells.count({x, y}) != 0 || m_proceduralCells.count({x, y}) != 0;
}

void TerrainManager::SetProceduralBuilder(std::function<void(Cell, Terrain&)> builder)
{
    m_proceduralBuilder = std::move(builder);
}

void TerrainManager::SetStaticCache(std::string directory, uint64_t signature)
{
    if (m_cacheSignature != signature || m_cacheDirectory != directory)
    {
        InvalidatePrepared();
        for (auto it = m_loaded.begin(); it != m_loaded.end(); )
        {
            const Cell cell = it->first;
            ++it;
            if (m_proceduralCells.count(cell)) UnloadCell(cell);
        }
    }
    m_cacheDirectory = std::move(directory);
    m_cacheSignature = signature;
    uint64_t bytes = 0;
    std::error_code error;
    const std::filesystem::path activeCache =
        std::filesystem::path(m_cacheDirectory) / std::to_string(m_cacheSignature);
    if (!m_cacheDirectory.empty() && std::filesystem::exists(activeCache, error))
        for (std::filesystem::recursive_directory_iterator it(activeCache,
                 std::filesystem::directory_options::skip_permission_denied, error), end;
             it != end && !error; it.increment(error))
            if (it->is_regular_file(error) && it->path().extension() == ".terrainmesh")
                bytes += it->file_size(error);
    m_cacheReservedBytes.store(bytes);
}

std::string TerrainManager::GetRuntimeBakeFilePath(Cell cell) const
{
    if (runtimeBakeDirectory.empty()) return {};
    return (std::filesystem::path(Mesh::ResolveFilePath(runtimeBakeDirectory)) /
        std::to_string(m_cacheSignature) /
        (std::to_string(cell.first) + "_" + std::to_string(cell.second) +
            ".terrainmesh")).string();
}

std::string TerrainManager::GetRuntimeBakeArchivePath() const
{
    if (runtimeBakeDirectory.empty()) return {};
    return (std::filesystem::path(Mesh::ResolveFilePath(runtimeBakeDirectory)) /
        (std::to_string(m_cacheSignature) + ".terrainpack")).string();
}

bool TerrainManager::ReadRuntimeBakeIndex(const std::string& path,
    std::map<Cell, BakeEntry>& entries) const
{
    entries.clear();
    std::ifstream file(path, std::ios::binary);
    uint64_t magic = 0, signature = 0;
    uint32_t version = 0, count = 0;
    if (!file || !ReadBinary(file, magic) ||
        magic != 0x31304b41504e5254ull || // TRNPAK01
        !ReadBinary(file, version) || version != 1 ||
        !ReadBinary(file, signature) || signature != m_cacheSignature ||
        !ReadBinary(file, count) || count != GetCellCount()) return false;
    std::error_code error;
    const uint64_t fileSize = std::filesystem::file_size(path, error);
    const uint64_t dataStart = 24ull + static_cast<uint64_t>(count) * 24;
    if (error || fileSize < dataStart) return false;
    std::vector<std::pair<uint64_t, uint64_t>> ranges;
    ranges.reserve(count);
    for (uint32_t i = 0; i < count; ++i)
    {
        int32_t x = 0, y = 0;
        BakeEntry entry;
        if (!ReadBinary(file, x) || !ReadBinary(file, y) ||
            !ReadBinary(file, entry.offset) || !ReadBinary(file, entry.size) ||
            !HasCell(x, y) || entry.offset < dataStart ||
            entry.size == 0 || entry.size > (256ull << 20) ||
            entry.offset > fileSize || entry.size > fileSize - entry.offset ||
            !entries.emplace(Cell{x, y}, entry).second) return false;
        ranges.emplace_back(entry.offset, entry.offset + entry.size);
    }
    std::sort(ranges.begin(), ranges.end());
    uint64_t next = dataStart;
    for (const auto& range : ranges)
    {
        if (range.first != next) return false;
        next = range.second;
    }
    if (entries.size() != GetCellCount() || next != fileSize) return false;
    std::map<std::string, int64_t> materialStamps;
    for (const auto& [cell, entry] : entries)
        if (!Terrain::CacheMaterialsMatchRange(file, m_cacheSignature,
            cellsX, cellsY, entry.offset, entry.size, materialStamps))
            return false;
    return true;
}

bool TerrainManager::ReadBakedCellHeights(Cell cell,
    std::vector<float>& heights, std::vector<float>* weights,
    uint32_t* weightLayerCount) const
{
    if (!m_runtimeBakeActive || !HasCell(cell.first, cell.second))
        return false;
    const auto found = m_bakeIndex.find(cell);
    return found != m_bakeIndex.end() &&
        Terrain::ReadCacheHeightsRange(GetRuntimeBakeArchivePath(),
            m_cacheSignature, cellsX, cellsY, heights,
            found->second.offset, found->second.size,
            weights, weightLayerCount);
}

bool TerrainManager::BakeRuntimeTerrain(bool reuseExistingCells)
{
    if (!Owner || !Owner->GetScene() || Owner->GetScene()->HasStarted() ||
        runtimeBakeDirectory.empty() ||
        (m_proceduralCells.empty() && m_cells.empty())) return false;
    // One indexed file preserves random cell reads without thousands of
    // filesystem entries. The temporary file is committed only when complete.
    const std::filesystem::path target(GetRuntimeBakeArchivePath());
    const std::filesystem::path temporary = target.string() + ".tmp";
    const std::filesystem::path cellTemporary = target.string() + ".cell";
    std::error_code error;
    std::filesystem::create_directories(target.parent_path(), error);
    if (error) return false;
    std::ofstream packed(temporary, std::ios::binary | std::ios::trunc);
    const uint64_t magic = 0x31304b41504e5254ull;
    const uint32_t version = 1;
    const auto cells = GetCells();
    const uint32_t count = static_cast<uint32_t>(cells.size());
    if (!packed || !WriteBinary(packed, magic) ||
        !WriteBinary(packed, version) ||
        !WriteBinary(packed, m_cacheSignature) || !WriteBinary(packed, count))
        return false;
    const std::vector<char> emptyIndex(static_cast<size_t>(count) * 24);
    packed.write(emptyIndex.data(),
        static_cast<std::streamsize>(emptyIndex.size()));
    if (!packed) return false;
    std::map<Cell, BakeEntry> index;
    for (const Cell cell : cells)
    {
        std::string sourcePath;
        std::vector<float> legacyHeights;
        if (reuseExistingCells && Terrain::ReadCacheHeights(
            GetRuntimeBakeFilePath(cell), m_cacheSignature, cellsX,
            cellsY, legacyHeights))
            sourcePath = GetRuntimeBakeFilePath(cell);
        else
        {
            Engine::Core::Object object;
            auto* terrain = object.AddComponent<Terrain>();
            const auto saved = m_cells.find(cell);
            if (saved != m_cells.end()) terrain->Deserialize(saved->second);
            else
            {
                if (!m_proceduralBuilder) return false;
                terrain->Resize(cellsX, cellsY, cellWidth, cellDepth);
                m_proceduralBuilder(cell, *terrain);
            }
            if (m_editedProcedural.count(cell))
            {
                if (const auto* loaded = GetLoadedCell(cell.first, cell.second))
                {
                    terrain->heights = loaded->heights;
                    terrain->weights = loaded->weights;
                    terrain->Rebuild();
                }
                else if (!LoadEdit(cell, *terrain)) return false;
            }
            if (!terrain->SaveCache(cellTemporary.string(),
                m_cacheSignature, true))
            {
                m_bakeStatus = "Bake failed at cell " +
                    std::to_string(cell.first) + "," + std::to_string(cell.second);
                return false;
            }
            sourcePath = cellTemporary.string();
        }
        const uint64_t size = std::filesystem::file_size(sourcePath, error);
        if (error || !size || size > (256ull << 20)) return false;
        const uint64_t offset = static_cast<uint64_t>(packed.tellp());
        std::ifstream source(sourcePath, std::ios::binary);
        packed << source.rdbuf();
        if (!source || !packed ||
            static_cast<uint64_t>(packed.tellp()) != offset + size) return false;
        source.close();
        index[cell] = {offset, size};
        if (sourcePath == cellTemporary.string())
            std::filesystem::remove(cellTemporary, error);
    }
    packed.seekp(24);
    for (const auto& [cell, entry] : index)
    {
        const int32_t x = cell.first, y = cell.second;
        if (!WriteBinary(packed, x) || !WriteBinary(packed, y) ||
            !WriteBinary(packed, entry.offset) ||
            !WriteBinary(packed, entry.size)) return false;
    }
    packed.close();
    std::map<Cell, BakeEntry> verified;
    if (!packed || !ReadRuntimeBakeIndex(temporary.string(), verified))
        return false;
    std::filesystem::rename(temporary, target, error);
    if (error)
    {
        error.clear();
        std::filesystem::remove(target, error);
        error.clear();
        std::filesystem::rename(temporary, target, error);
    }
    if (error) return false;
    m_bakeStatus = "Baked " + std::to_string(count) +
        " terrain cells into one file";
    return true;
}

bool TerrainManager::HasCompleteRuntimeBake() const
{
    if (runtimeBakeDirectory.empty() || !GetCellCount()) return false;
    std::map<Cell, BakeEntry> entries;
    return ReadRuntimeBakeIndex(GetRuntimeBakeArchivePath(), entries);
}

bool TerrainManager::ReserveCacheWrite(const std::string& path,
    const Terrain& terrain)
{
    if (path.empty()) return false;
    std::error_code error;
    if (std::filesystem::exists(path, error)) return true;
    // Match Terrain::SaveCache's header, layer records, and three vectors.
    // Reserve the exact encoded size before the worker or baker writes.
    uint64_t reservation = 45 + 3 * sizeof(uint64_t) +
        terrain.heights.size() * (sizeof(float) + 24ull) +
        terrain.weights.size() * sizeof(float);
    for (const auto& layer : terrain.layers)
        reservation += sizeof(uint32_t) + layer.material.size() +
            5 * sizeof(float) + sizeof(int64_t);
    const uint64_t limit = static_cast<uint64_t>(std::max(0, cacheBudgetMiB)) << 20;
    uint64_t used = m_cacheReservedBytes.load();
    while (used <= limit && reservation <= limit - used)
        if (m_cacheReservedBytes.compare_exchange_weak(used, used + reservation))
            return true;
    return false;
}

TerrainManager::MemoryUsage TerrainManager::GetMemoryUsage() const
{
    MemoryUsage usage;
    usage.cacheBytes = m_cacheReservedBytes.load();
    for (const auto& [cell, terrain] : m_loaded)
    {
        if (!terrain || !terrain->Owner) continue;
        usage.cpuBytes += terrain->heights.capacity() * sizeof(float) +
            terrain->weights.capacity() * sizeof(float) +
            terrain->layers.capacity() * sizeof(Terrain::Layer) +
            terrain->GetBakedCpuMemoryBytes();
        for (const auto& layer : terrain->layers)
            usage.cpuBytes += layer.material.capacity();
        if (const auto* mesh = terrain->Owner->GetComponent<Mesh>())
        {
            usage.cpuBytes += mesh->GetCpuMeshMemoryBytes() +
                mesh->GetUploadShadowMemoryBytes();
            usage.gpuBytes += mesh->GetGpuBufferMemoryBytes();
        }
    }
    for (const auto& [group, batch] : m_renderBatches)
        if (batch.object)
            if (const auto* mesh = batch.object->GetComponent<Mesh>())
            {
                usage.cpuBytes += mesh->GetCpuMeshMemoryBytes() +
                    mesh->GetUploadShadowMemoryBytes();
                usage.gpuBytes += mesh->GetGpuBufferMemoryBytes();
            }
    usage.cpuBytes += m_farHeights.capacity() * sizeof(float);
    if (m_farHeightsSnapshot)
        usage.cpuBytes += m_farHeightsSnapshot->capacity() * sizeof(float);
    for (const auto& tile : m_farTiles)
        if (tile.mesh)
        {
            usage.cpuBytes += tile.mesh->GetCpuMeshMemoryBytes() +
                tile.mesh->GetUploadShadowMemoryBytes();
            usage.gpuBytes += tile.mesh->GetGpuBufferMemoryBytes();
        }
    {
        std::lock_guard<std::mutex> lock(m_farPrepareMutex);
        for (const auto& prepared : m_preparedFarTiles)
            usage.preparedBytes +=
                prepared.vertices.capacity() * sizeof(Mesh::Vertex) +
                prepared.indices.capacity() * sizeof(uint32_t);
        for (const auto& task : m_farPrepareQueue)
        {
            for (const auto& [cell, surface] : task.seams)
                usage.preparedBytes +=
                    (surface.heights.capacity() +
                        surface.weights.capacity()) * sizeof(float);
            for (const auto& [index, patch] : task.patches)
                usage.preparedBytes +=
                    (patch.heights.capacity() + patch.colors.capacity()) *
                        sizeof(float);
        }
    }
    std::lock_guard<std::mutex> lock(m_prepareMutex);
    for (const auto& prepared : m_preparedCells)
    {
        const auto& data = prepared.terrain;
        usage.preparedBytes += data.heights.capacity() * sizeof(float) +
            data.weights.capacity() * sizeof(float) +
            data.vertices.capacity() * sizeof(Mesh::Vertex) +
            data.indices.capacity() * sizeof(uint32_t) +
            data.layers.capacity() * sizeof(Terrain::Layer);
        usage.preparedBytes +=
            data.bakedAppearance.capacity() * sizeof(Terrain::BakedAppearance);
        for (const auto& lod : data.bakedLods)
            usage.preparedBytes +=
                (lod.sourceVertices.capacity() + lod.indices.capacity()) *
                sizeof(uint32_t);
    }
    usage.cpuBytes += usage.preparedBytes;
    return usage;
}

uint64_t TerrainManager::GetTerrainTextureMemoryBytes() const
{
    std::set<const Texture*> unique;
    uint64_t bytes = 0;
    const auto countTexture = [&](const std::shared_ptr<Texture>& texture) {
        if (!texture || !texture->GetGraphicsTexture() ||
            !unique.insert(texture.get()).second) return;
        uint32_t width = texture->GetWidth();
        uint32_t height = texture->GetHeight();
        const uint32_t pixelBytes =
            Engine::Graphics::GraphicsTextureBytesPerPixel(texture->GetFormat());
        for (uint32_t level = 0; level < texture->GetMipLevels(); ++level)
        {
            bytes += static_cast<uint64_t>(width) * height * pixelBytes;
            width = std::max(1u, width / 2);
            height = std::max(1u, height / 2);
        }
    };
    for (const auto& [cell, terrain] : m_loaded)
        if (terrain && terrain->Owner)
            for (const auto& material : terrain->GetLayerMaterials())
                if (material)
                {
                    countTexture(material->baseColorTexture);
                    countTexture(material->metallicRoughnessTexture);
                    countTexture(material->normalTexture);
                    countTexture(material->heightTexture);
                    countTexture(material->occlusionTexture);
                    countTexture(material->emissiveTexture);
                }
    return bytes;
}

void TerrainManager::TrimMemoryBudget(int centerX, int centerY)
{
    const uint64_t cpuLimit = static_cast<uint64_t>(std::max(1, cpuBudgetMiB)) << 20;
    const uint64_t gpuLimit = static_cast<uint64_t>(std::max(1, gpuBudgetMiB)) << 20;
    for (;;)
    {
        const auto usage = GetMemoryUsage();
        if ((usage.cpuBytes <= cpuLimit && usage.gpuBytes <= gpuLimit) ||
            m_loaded.empty()) break;
        auto farthest = m_loaded.end();
        int distance = -1;
        for (auto it = m_loaded.begin(); it != m_loaded.end(); ++it)
        {
            const int d = std::max(std::abs(it->first.first - centerX),
                std::abs(it->first.second - centerY));
            if (d > distance) { distance = d; farthest = it; }
        }
        if (farthest == m_loaded.end()) break;
        UnloadCell(farthest->first);
        RefreshRenderBatches(0, centerX, centerY, 0);
    }
}

void TerrainManager::StartPrepareWorker()
{
    if (m_prepareWorker.joinable()) return;
    m_prepareWorker = std::thread([this] { PrepareLoop(); });
}

void TerrainManager::InvalidatePrepared()
{
    std::lock_guard<std::mutex> lock(m_prepareMutex);
    ++m_prepareRevision;
    m_prepareQueue.clear();
    m_preparedCells.clear();
    m_preparingCells.clear();
}

void TerrainManager::PrepareLoop()
{
    for (;;)
    {
        PrepareTask task;
        {
            std::unique_lock<std::mutex> lock(m_prepareMutex);
            m_prepareWake.wait(lock, [this] {
                return m_stopPrepareWorker || !m_prepareQueue.empty();
            });
            if (m_stopPrepareWorker) return;
            task = std::move(m_prepareQueue.front());
            m_prepareQueue.pop_front();
        }
        PreparedCell result;
        result.cell = task.cell;
        result.revision = task.revision;
        try
        {
            Engine::Core::Object object;
            auto* terrain = object.AddComponent<Terrain>();
            const bool baked = !task.bakePath.empty() && task.bakeSize &&
                terrain->LoadCacheRange(task.bakePath,
                    task.cacheSignature, nullptr, task.bakeOffset,
                    task.bakeSize);
            if (task.requireBake && !baked)
                throw std::runtime_error("Missing or stale runtime terrain bake");
            result.cacheHit = baked;
            if (!baked && task.hasSaved) terrain->Deserialize(task.saved);
            else if (!baked)
            {
                result.cacheHit = !task.cachePath.empty() &&
                    terrain->LoadCache(task.cachePath,
                        task.cacheSignature, nullptr);
                if (!result.cacheHit && task.builder)
                {
                    terrain->Resize(task.cellsX, task.cellsY,
                        task.cellWidth, task.cellDepth);
                    task.builder(task.cell, *terrain);
                    terrain->Rebuild();
                    if (ReserveCacheWrite(task.cachePath, *terrain))
                        terrain->SaveCache(task.cachePath, task.cacheSignature);
                }
            }
            if (!baked && task.edited && !task.editPath.empty())
                LoadEditFile(task.editPath, *terrain);
            terrain->SetMeshLodStride(task.lodStride);
            result.terrain = terrain->TakePrepared();
        }
        catch (...) { result.terrain.vertices.clear(); }
        {
            std::lock_guard<std::mutex> lock(m_prepareMutex);
            if (task.revision == m_prepareRevision)
                m_preparedCells.push_back(std::move(result));
        }
    }
}

int TerrainManager::CommitPrepared(int centerX, int centerY, int radius,
    int uploadBudget)
{
    const int limit = std::clamp(uploadBudget, 0, 64);
    int uploaded = 0;
    for (; uploaded < limit; )
    {
        PreparedCell prepared;
        {
            std::lock_guard<std::mutex> lock(m_prepareMutex);
            if (m_preparedCells.empty()) break;
            const auto inWindow = [&](Cell cell, Cell center) {
                return std::abs(cell.first - center.first) <= radius &&
                    std::abs(cell.second - center.second) <= radius;
            };
            auto ready = std::find_if(m_preparedCells.begin(),
                m_preparedCells.end(), [&](const PreparedCell& candidate) {
                    const bool current = inWindow(candidate.cell, {centerX, centerY});
                    const bool prefetched = m_prefetchCenter &&
                        inWindow(candidate.cell, *m_prefetchCenter);
                    return candidate.revision != m_prepareRevision ||
                        current || !prefetched;
                });
            if (ready == m_preparedCells.end()) break;
            prepared = std::move(*ready);
            m_preparedCells.erase(ready);
            if (prepared.revision != m_prepareRevision) continue;
            const auto found = m_preparingCells.find(prepared.cell);
            if (found != m_preparingCells.end() &&
                found->second == prepared.revision) m_preparingCells.erase(found);
        }
        const Cell cell = prepared.cell;
        if (!Owner || !Owner->GetScene() || !HasCell(cell.first, cell.second) ||
            m_loaded.count(cell) ||
            std::abs(cell.first - centerX) > radius ||
            std::abs(cell.second - centerY) > radius ||
            prepared.terrain.vertices.empty()) continue;
        auto* object = Owner->GetScene()->AddObject("Terrain cell " +
            std::to_string(cell.first) + "," + std::to_string(cell.second));
        object->runtimeOnly = true;
        Owner->GetScene()->MoveObject(object, Owner,
            Engine::Scene::Scene::ObjectPlacement::AsChild);
        object->transform.position = {
            static_cast<double>(cell.first) * cellsX * cellWidth, 0.0,
            static_cast<double>(cell.second) * cellsY * cellDepth};
        auto* terrain = object->AddComponent<Terrain>();
        if (!terrain->InstallPrepared(std::move(prepared.terrain), nullptr))
        {
            Owner->GetScene()->RemoveObject(object);
            continue;
        }
        if (prepared.cacheHit) ++m_cacheHits;
        m_loaded[cell] = terrain;
        Stitch(cell);
        terrain->SetMeshLodStride(DesiredLodStride(cell, centerX, centerY));
        terrain->FinalizePreparedGraphics(m_provider);
        ConfigureCellPhysics(*object);
        ++uploaded;
    }
    return uploaded;
}

std::string TerrainManager::GetCacheFilePath(int x, int y) const
{
    return m_cacheDirectory.empty() ? std::string{} :
        (std::filesystem::path(m_cacheDirectory) /
            std::to_string(m_cacheSignature) /
            (std::to_string(x) + "_" + std::to_string(y) +
                ".terrainmesh")).string();
}

bool TerrainManager::BakeProceduralCell(int x, int y, bool force)
{
    if (!Owner || !Owner->GetScene() || !m_proceduralBuilder ||
        m_cacheDirectory.empty()) return false;
    const auto path = GetCacheFilePath(x, y);
    auto* object = Owner->GetScene()->AddObject("Terrain bake cell");
    object->runtimeOnly = true;
    auto* terrain = object->AddComponent<Terrain>();
    bool result = !force && terrain->LoadCache(path, m_cacheSignature, nullptr);
    if (!result)
    {
        terrain->Resize(cellsX, cellsY, cellWidth, cellDepth);
        m_proceduralBuilder({x, y}, *terrain);
        terrain->OnAfterDeserialize(nullptr);
        result = ReserveCacheWrite(path, *terrain) &&
            terrain->SaveCache(path, m_cacheSignature);
    }
    Owner->GetScene()->RemoveObject(object);
    return result;
}

std::vector<TerrainManager::Cell> TerrainManager::GetCells() const
{
    std::vector<Cell> result;
    for (const auto& [cell, data] : m_cells) result.push_back(cell);
    result.insert(result.end(), m_proceduralCells.begin(), m_proceduralCells.end());
    return result;
}

Terrain* TerrainManager::GetLoadedCell(int x, int y) const
{
    const auto found = m_loaded.find({x, y});
    return found == m_loaded.end() ? nullptr : found->second;
}

bool TerrainManager::SaveEdit(Cell cell, const Terrain& terrain) const
{
    if (cellDataDirectory.empty() || !m_proceduralBuilder) return false;
    Terrain baseline;
    baseline.Resize(cellsX, cellsY, cellWidth, cellDepth);
    m_proceduralBuilder(cell, baseline);
    if (baseline.heights.size() != terrain.heights.size() ||
        baseline.weights.size() != terrain.weights.size() ||
        baseline.layers.size() != terrain.layers.size() ||
        baseline.layers.size() > 6) return false;
    std::error_code error;
    std::filesystem::create_directories(cellDataDirectory, error);
    if (error) return false;
    const auto target = std::filesystem::path(cellDataDirectory) /
        (std::to_string(cell.first) + "_" + std::to_string(cell.second) + ".terrainedit");
    const auto temporary = target.string() + ".tmp";
    std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
    if (!file) return false;
    const uint64_t magic = 0x31544944454e5254ull; // TRNEDIT1
    const uint32_t version = 1;
    const uint32_t x = static_cast<uint32_t>(terrain.cellsX);
    const uint32_t y = static_cast<uint32_t>(terrain.cellsY);
    const uint32_t layerCount = static_cast<uint32_t>(terrain.layers.size());
    uint32_t heightCount = 0, weightCount = 0;
    for (size_t i = 0; i < terrain.heights.size(); ++i)
    {
        heightCount += terrain.heights[i] != baseline.heights[i];
        bool different = false;
        for (size_t layer = 0; layer < layerCount; ++layer)
            different |= terrain.weights[i * layerCount + layer] !=
                baseline.weights[i * layerCount + layer];
        weightCount += different;
    }
    bool valid = WriteBinary(file, magic) && WriteBinary(file, version) &&
        WriteBinary(file, x) && WriteBinary(file, y) &&
        WriteBinary(file, layerCount) && WriteBinary(file, heightCount) &&
        WriteBinary(file, weightCount);
    for (uint32_t i = 0; valid && i < terrain.heights.size(); ++i)
        if (terrain.heights[i] != baseline.heights[i])
            valid = WriteBinary(file, i) && WriteBinary(file, terrain.heights[i]);
    for (uint32_t i = 0; valid && i < terrain.heights.size(); ++i)
    {
        bool different = false;
        for (size_t layer = 0; layer < layerCount; ++layer)
            different |= terrain.weights[i * layerCount + layer] !=
                baseline.weights[i * layerCount + layer];
        if (!different) continue;
        valid = WriteBinary(file, i);
        for (size_t layer = 0; valid && layer < layerCount; ++layer)
            valid = WriteBinary(file, terrain.weights[i * layerCount + layer]);
    }
    file.close();
    valid &= static_cast<bool>(file);
    bool committed = false;
    if (valid)
    {
        std::filesystem::rename(temporary, target, error);
        if (error)
        {
            error.clear();
            std::filesystem::remove(target, error);
            error.clear();
            std::filesystem::rename(temporary, target, error);
        }
        committed = !error;
    }
    if (!committed) std::filesystem::remove(temporary, error);
    if (committed) m_dirtyProceduralEdits.erase(cell);
    return committed;
}

static bool LoadEditFile(const std::string& path, Terrain& terrain)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    uint64_t magic = 0;
    uint32_t version = 0, x = 0, y = 0, layerCount = 0;
    uint32_t heightCount = 0, weightCount = 0;
    if (!ReadBinary(file, magic) || magic != 0x31544944454e5254ull ||
        !ReadBinary(file, version) || version != 1 ||
        !ReadBinary(file, x) || x != static_cast<uint32_t>(terrain.cellsX) ||
        !ReadBinary(file, y) || y != static_cast<uint32_t>(terrain.cellsY) ||
        !ReadBinary(file, layerCount) || layerCount == 0 ||
        layerCount > terrain.layers.size() ||
        !ReadBinary(file, heightCount) || heightCount > terrain.heights.size() ||
        !ReadBinary(file, weightCount) || weightCount > terrain.heights.size())
        return false;
    auto heights = terrain.heights;
    auto weights = terrain.weights;
    for (uint32_t n = 0; n < heightCount; ++n)
    {
        uint32_t index = 0;
        float value = 0.f;
        if (!ReadBinary(file, index) || index >= heights.size() ||
            !ReadBinary(file, value)) return false;
        heights[index] = value;
    }
    for (uint32_t n = 0; n < weightCount; ++n)
    {
        uint32_t index = 0;
        if (!ReadBinary(file, index) || index >= heights.size()) return false;
        const size_t destination = static_cast<size_t>(index) *
            terrain.layers.size();
        std::fill_n(weights.begin() + destination,
            terrain.layers.size(), 0.f);
        for (uint32_t layer = 0; layer < layerCount; ++layer)
            if (!ReadBinary(file, weights[destination + layer])) return false;
    }
    char trailing = 0;
    if (file.read(&trailing, 1) || !file.eof()) return false;
    terrain.heights = std::move(heights);
    terrain.weights = std::move(weights);
    terrain.Rebuild();
    return true;
}

std::string TerrainManager::GetEditFilePath(Cell cell) const
{
    return cellDataDirectory.empty() ? std::string{} :
        (std::filesystem::path(cellDataDirectory) /
            (std::to_string(cell.first) + "_" + std::to_string(cell.second) +
                ".terrainedit")).string();
}

bool TerrainManager::LoadEdit(Cell cell, Terrain& terrain) const
{
    const auto path = GetEditFilePath(cell);
    return !path.empty() && LoadEditFile(path, terrain);
}

void TerrainManager::ConfigureCellPhysics(Engine::Core::Object& object) const
{
    if (!generateColliders) return;
    auto* mesh = object.GetComponent<Mesh>();
    if (!mesh || mesh->GetVertexCount() == 0u) return;
    auto* collider = object.GetComponent<MeshObjectCollider>();
    if (!collider) collider = object.AddComponent<MeshObjectCollider>();
    collider->convex = false;
    auto* body = object.GetComponent<RigidBody>();
    if (!body) body = object.AddComponent<RigidBody>();
    body->bodyType = "Static";
    body->useGravity = false;
    body->friction = 0.9f;
    body->restitution = 0.f;
}

void TerrainManager::RefreshColliderActivation(int centerX, int centerY)
{
    if (!generateColliders || !streamCollidersByDistance) return;
    const int colliderRadius = std::clamp(loadedRadius + 1, 0, 64);
    for (const auto& [cell, terrain] : m_loaded)
    {
        if (!terrain || !terrain->Owner) continue;
        auto* collider = terrain->Owner->GetComponent<MeshObjectCollider>();
        if (!collider) continue;
        const bool active = std::abs(cell.first - centerX) <= colliderRadius &&
            std::abs(cell.second - centerY) <= colliderRadius;
        if (collider->collisionEnabled == active) continue;
        collider->collisionEnabled = active;
        collider->MarkConfigurationDirty();
    }
}

void TerrainManager::LoadCell(Cell cell, int lodStride)
{
    if (!Owner || !Owner->GetScene() || !HasCell(cell.first, cell.second) || m_loaded.count(cell)) return;
    const auto saved = m_cells.find(cell);
    if (saved == m_cells.end() && !m_proceduralBuilder &&
        !(Owner->GetScene()->HasStarted() && requireRuntimeBake)) return;
    auto* object = Owner->GetScene()->AddObject("Terrain cell " +
        std::to_string(cell.first) + "," + std::to_string(cell.second));
    object->runtimeOnly = true;
    Owner->GetScene()->MoveObject(object, Owner,
        Engine::Scene::Scene::ObjectPlacement::AsChild);
    object->transform.position = {
        static_cast<double>(cell.first) * cellsX * cellWidth, 0.0,
        static_cast<double>(cell.second) * cellsY * cellDepth};
    auto* terrain = object->AddComponent<Terrain>();
    const auto cachePath = GetCacheFilePath(cell.first, cell.second);
    const std::string bakePath = (m_runtimeBakeActive || requireRuntimeBake) &&
        Owner->GetScene()->HasStarted()
        ? GetRuntimeBakeArchivePath() : std::string{};
    const auto bakeEntry = m_bakeIndex.find(cell);
    bool ready = !bakePath.empty() && bakeEntry != m_bakeIndex.end() &&
        terrain->LoadCacheRange(bakePath, m_cacheSignature, m_provider,
            bakeEntry->second.offset, bakeEntry->second.size);
    if (Owner->GetScene()->HasStarted() &&
        (m_runtimeBakeActive || requireRuntimeBake) && !ready)
    {
        Owner->GetScene()->RemoveObject(object);
        return;
    }
    const bool baked = ready;
    if (ready) ++m_cacheHits;
    if (!ready && saved != m_cells.end()) terrain->Deserialize(saved->second);
    else if (!ready)
    {
        ready = !cachePath.empty() &&
            terrain->LoadCache(cachePath, m_cacheSignature, m_provider);
        if (ready) ++m_cacheHits;
        if (!ready)
        {
            terrain->Resize(cellsX, cellsY, cellWidth, cellDepth);
            m_proceduralBuilder(cell, *terrain);
        }
    }
    if (!ready)
    {
        terrain->OnAfterDeserialize(m_provider);
        if (saved == m_cells.end() && ReserveCacheWrite(cachePath, *terrain))
            terrain->SaveCache(cachePath, m_cacheSignature);
    }
    if (!baked && m_editedProcedural.count(cell)) LoadEdit(cell, *terrain);
    m_loaded[cell] = terrain;
    Stitch(cell);
    terrain->SetMeshLodStride(lodStride);
    ConfigureCellPhysics(*object);
}

int TerrainManager::DesiredLodStride(Cell cell, int centerX, int centerY) const
{
    if (m_editorStreaming) return 1;
    const int distance = std::max(std::abs(cell.first - centerX),
        std::abs(cell.second - centerY));
    if (distance <= loadedRadius) return 1;
    if (distance <= lod2Radius) return 2;
    if (distance <= lod4Radius) return 4;
    return 8;
}

void TerrainManager::UnloadCell(Cell cell)
{
    const auto found = m_loaded.find(cell);
    if (found == m_loaded.end()) return;
    Terrain* terrain = found->second;
    if (m_editedProcedural.count(cell) && m_dirtyProceduralEdits.count(cell) &&
        !SaveEdit(cell, *terrain))
    {
        m_cells[cell] = terrain->Serialize();
        m_proceduralCells.erase(cell);
        m_editedProcedural.erase(cell);
        m_dirtyProceduralEdits.erase(cell);
    }
    if (!m_proceduralCells.count(cell)) m_cells[cell] = terrain->Serialize();
    auto* object = terrain->Owner;
    m_loaded.erase(found);
    if (object && object->GetScene()) object->GetScene()->RemoveObject(object);
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
    Terrain::Brush brush, int layer)
{
    if (Owner && Owner->GetScene() && Owner->GetScene()->HasStarted()) return false;
    if (!std::isfinite(x) || !std::isfinite(z) || !std::isfinite(radius) ||
        !std::isfinite(strength) || radius <= 0.f || strength <= 0.f ||
        (brush == Terrain::Brush::Paint && layer < 0)) return false;
    const double spanX = static_cast<double>(cellsX) * cellWidth;
    const double spanZ = static_cast<double>(cellsY) * cellDepth;
    const auto cellFloor = [](double coordinate) {
        return static_cast<int64_t>(std::floor(std::clamp(coordinate,
            static_cast<double>(std::numeric_limits<int>::min()),
            static_cast<double>(std::numeric_limits<int>::max()))));
    };
    const int64_t minX = cellFloor((static_cast<double>(x) - radius) / spanX);
    const int64_t maxX = cellFloor((static_cast<double>(x) + radius) / spanX);
    const int64_t minZ = cellFloor((static_cast<double>(z) - radius) / spanZ);
    const int64_t maxZ = cellFloor((static_cast<double>(z) + radius) / spanZ);
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
    std::set<Cell> changedCells;
    for (Cell cell : targets)
        if (auto* terrain = GetLoadedCell(cell.first, cell.second))
            if (terrain->ApplyBrush(
                static_cast<float>(static_cast<double>(x) - cell.first * spanX),
                static_cast<float>(static_cast<double>(z) - cell.second * spanZ),
                radius, strength, brush, layer))
                changedCells.insert(cell);
    if (changedCells.empty()) return false;

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
                if (dx * dx + dz * dz >= static_cast<double>(radius) * radius)
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
        GetLoadedCell(cell.first, cell.second)->Rebuild();
    for (Cell cell : changedCells)
    {
        auto* terrain = GetLoadedCell(cell.first, cell.second);
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
    return true;
}

void TerrainManager::SyncLoadedCells(int centerX, int centerY)
{
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

void TerrainManager::RefreshRenderBatches(int uploadBudget, int centerX,
    int centerY, int radius)
{
    if (!Owner || !Owner->GetScene()) return;
    const auto refreshStart = std::chrono::steady_clock::now();
    m_renderBatchRefreshPending = false;
    constexpr int side = 4;
    const auto groupOf = [side](Cell cell) -> Cell {
        const auto floorDiv = [side](int value) {
            return value >= 0 ? value / side : (value - side + 1) / side;
        };
        return {floorDiv(cell.first), floorDiv(cell.second)};
    };
    using Members = std::vector<std::pair<Cell, Terrain*>>;
    std::map<Cell, Members> groups;
    if (batchFarCells)
    for (const auto& [cell, terrain] : m_loaded)
    {
        if (!terrain || !terrain->Owner || terrain->GetMeshLodStride() < 2)
            continue;
        auto* mesh = terrain->Owner->GetComponent<Mesh>();
        if (mesh && mesh->IsReady() && !mesh->GetVertices().empty() &&
            !mesh->GetIndices().empty())
            groups[groupOf(cell)].emplace_back(cell, terrain);
    }
    const auto revisionOf = [](const Members& members) {
        uint64_t revision = 1469598103934665603ull;
        for (const auto& [cell, terrain] : members)
        {
            if (!terrain || !terrain->Owner) continue;
            const auto* mesh = terrain->Owner->GetComponent<Mesh>();
            if (!mesh) continue;
            for (uint64_t value : {static_cast<uint64_t>(static_cast<uint32_t>(cell.first)),
                    static_cast<uint64_t>(static_cast<uint32_t>(cell.second)),
                    mesh->GetAuthoredGeometryRevision(),
                    terrain->Owner->transform.GetWorldRevision()})
                revision = (revision ^ value) * 1099511628211ull;
        }
        return revision;
    };
    for (auto it = m_renderBatches.begin(); it != m_renderBatches.end(); )
    {
        const auto found = groups.find(it->first);
        if (found != groups.end() && found->second.size() >= 2 &&
            it->second.revision == revisionOf(found->second))
        { ++it; continue; }
        // A far-cell LOD change uploads its cell mesh first. With no upload
        // slot left, keep drawing the previous batch for the same members;
        // replace it on a later frame instead of exposing every cell draw.
        if (!m_editorStreaming && uploadBudget <= 0 &&
            found != groups.end() &&
            found->second.size() == it->second.members.size())
        {
            bool sameMembers = true;
            bool lodChanged = false;
            for (const auto& [cell, terrain] : found->second)
            {
                if (!terrain || !terrain->Owner)
                { sameMembers = false; break; }
                const auto oldLod = it->second.lodStrides.find(cell);
                const auto oldGeometry =
                    it->second.geometryRevisions.find(cell);
                const auto oldTransform =
                    it->second.transformRevisions.find(cell);
                const auto* mesh = terrain->Owner->GetComponent<Mesh>();
                if (!it->second.members.count(cell) ||
                    oldLod == it->second.lodStrides.end() ||
                    oldGeometry == it->second.geometryRevisions.end() ||
                    oldTransform == it->second.transformRevisions.end() ||
                    oldTransform->second !=
                        terrain->Owner->transform.GetWorldRevision() ||
                    (!mesh || (oldLod->second == terrain->GetMeshLodStride() &&
                        oldGeometry->second != mesh->GetAuthoredGeometryRevision())))
                { sameMembers = false; break; }
                lodChanged |= oldLod->second != terrain->GetMeshLodStride();
            }
            if (sameMembers && lodChanged)
            {
                m_renderBatchRefreshPending = true;
                ++it;
                continue;
            }
        }
        // A moving streaming window fills a 4x4 group one cell at a time.
        // Keep its old batch while the missing cells render individually;
        // replace it once every cell required by this window has arrived.
        bool waitingForCells = false;
        for (int y = it->first.second * side;
            y < (it->first.second + 1) * side && !waitingForCells; ++y)
            for (int x = it->first.first * side;
                x < (it->first.first + 1) * side; ++x)
                if (std::abs(x - centerX) <= radius &&
                    std::abs(y - centerY) <= radius && HasCell(x, y) &&
                    !m_loaded.count({x, y}))
                { waitingForCells = true; break; }
        if (found != groups.end() && waitingForCells &&
            found->second.size() > it->second.members.size())
        {
            Members previousMembers;
            for (const auto& member : found->second)
                if (member.second && member.second->Owner &&
                    it->second.members.count(member.first))
                    previousMembers.push_back(member);
            if (previousMembers.size() == it->second.members.size() &&
                it->second.revision == revisionOf(previousMembers))
            { ++it; continue; }
        }
        for (Cell cell : it->second.members)
            if (auto* terrain = GetLoadedCell(cell.first, cell.second))
                if (auto* mesh = terrain->Owner->GetComponent<Mesh>())
                    mesh->SetEditorVisible(true);
        Owner->GetScene()->RemoveObject(it->second.object);
        it = m_renderBatches.erase(it);
    }
    for (const auto& [group, members] : groups)
    {
        if (members.size() < 2 || m_renderBatches.count(group)) continue;
        const Terrain* first = members.front().second;
        if (!first || !first->Owner) continue;
        const auto sameAppearance = [&](const Terrain* terrain) {
            if (!terrain || !terrain->Owner) return false;
            if (terrain->layers.size() != first->layers.size() ||
                terrain->textureTiling != first->textureTiling) return false;
            for (size_t layer = 0; layer < first->layers.size(); ++layer)
                if (terrain->layers[layer].material != first->layers[layer].material)
                    return false;
            return true;
        };
        if (!std::all_of(members.begin(), members.end(),
            [&](const auto& member) { return sameAppearance(member.second); }))
            continue;
        if (uploadBudget <= 0)
        {
            m_renderBatchRefreshPending = true;
            continue;
        }
        const auto buildStart = std::chrono::steady_clock::now();
        std::vector<Mesh::Vertex> vertices;
        std::vector<uint32_t> indices;
        size_t vertexCount = 0, indexCount = 0;
        for (const auto& [cell, terrain] : members)
        {
            if (!terrain || !terrain->Owner) continue;
            const auto* mesh = terrain->Owner->GetComponent<Mesh>();
            if (!mesh || mesh->GetVertices().empty() || mesh->GetIndices().empty())
                continue;
            vertexCount += mesh->GetVertices().size();
            indexCount += mesh->GetIndices().size();
        }
        if (vertexCount == 0 || indexCount == 0) continue;
        vertices.reserve(vertexCount);
        indices.reserve(indexCount);
        for (const auto& [cell, terrain] : members)
        {
            if (!terrain || !terrain->Owner) continue;
            const auto* mesh = terrain->Owner->GetComponent<Mesh>();
            if (!mesh || mesh->GetVertices().empty() || mesh->GetIndices().empty())
                continue;
            const glm::mat4 world = terrain->Owner->transform.GetWorldMatrix();
            const bool translationOnly =
                glm::length(glm::vec3(world[0]) - glm::vec3(1.f, 0.f, 0.f)) < 1e-6f &&
                glm::length(glm::vec3(world[1]) - glm::vec3(0.f, 1.f, 0.f)) < 1e-6f &&
                glm::length(glm::vec3(world[2]) - glm::vec3(0.f, 0.f, 1.f)) < 1e-6f;
            const glm::mat3 normalMatrix = translationOnly
                ? glm::mat3(1.f) :
                glm::transpose(glm::inverse(glm::mat3(world)));
            const glm::vec3 translation(world[3]);
            const uint32_t base = static_cast<uint32_t>(vertices.size());
            for (auto vertex : mesh->GetVertices())
            {
                const glm::vec3 position = translationOnly
                    ? glm::vec3(vertex.pos[0], vertex.pos[1], vertex.pos[2]) + translation
                    : glm::vec3(world * glm::vec4(
                        vertex.pos[0], vertex.pos[1], vertex.pos[2], 1.f));
                vertex.pos[0] = position.x; vertex.pos[1] = position.y;
                vertex.pos[2] = position.z;
                if (!translationOnly)
                {
                    const glm::vec3 normal = glm::normalize(normalMatrix *
                        glm::vec3(vertex.normal[0], vertex.normal[1],
                            vertex.normal[2]));
                    vertex.normal[0] = normal.x; vertex.normal[1] = normal.y;
                    vertex.normal[2] = normal.z;
                }
                vertices.push_back(vertex);
            }
            const size_t vertexSize = mesh->GetVertices().size();
            for (uint32_t index : mesh->GetIndices())
            {
                if (index >= vertexSize) continue;
                indices.push_back(base + index);
            }
        }
        if (vertices.empty() || indices.empty()) continue;
        auto* object = Owner->GetScene()->AddObject("Terrain batch " +
            std::to_string(group.first) + "," + std::to_string(group.second));
        object->runtimeOnly = true;
        Owner->GetScene()->MoveObject(object, Owner,
            Engine::Scene::Scene::ObjectPlacement::AsChild);
        auto* batchMesh = object->AddComponent<Mesh>();
        if (!batchMesh->SetIndexedGeometry(std::move(vertices), std::move(indices)))
        {
            Owner->GetScene()->RemoveObject(object);
            continue;
        }
        batchMesh->OnAfterDeserialize(m_provider);
        auto* appearance = object->AddComponent<TerrainBatchAppearance>();
        appearance->materials = first->GetLayerMaterials();
        appearance->textureTiling = first->textureTiling;
        appearance->completeFootprint = members.size() == side * side;
        RenderBatch batch;
        batch.object = object;
        batch.revision = revisionOf(members);
        for (const auto& [cell, terrain] : members)
        {
            batch.members.insert(cell);
            batch.lodStrides[cell] = terrain->GetMeshLodStride();
            batch.geometryRevisions[cell] = terrain->Owner->
                GetComponent<Mesh>()->GetAuthoredGeometryRevision();
            batch.transformRevisions[cell] =
                terrain->Owner->transform.GetWorldRevision();
            terrain->Owner->GetComponent<Mesh>()->SetEditorVisible(false);
        }
        m_renderBatches[group] = std::move(batch);
        ++m_renderBatchUploadCount;
        m_renderBatchBuildMilliseconds += std::chrono::duration<double,
            std::milli>(std::chrono::steady_clock::now() - buildStart).count();
        --uploadBudget;
    }
    m_renderBatchRefreshMilliseconds += std::chrono::duration<double,
        std::milli>(std::chrono::steady_clock::now() - refreshStart).count();
}

void TerrainManager::UpdateStreaming(int centerX, int centerY)
{
    if (m_editorStreaming) InvalidatePrepared();
    m_editorStreaming = false;
    m_prefetchCenter.reset();
    StreamCells(centerX, centerY);
}

void TerrainManager::UpdateStreamingAtPosition(
    const glm::dvec3& worldPosition)
{
    if (!Owner) return;
    const glm::dvec3 local = glm::dvec3(
        glm::inverse(Owner->transform.GetWorldMatrixD()) *
        glm::dvec4(worldPosition, 1.0));
    const double spanX = static_cast<double>(cellsX) * cellWidth;
    const double spanY = static_cast<double>(cellsY) * cellDepth;
    if (spanX <= 0.0 || spanY <= 0.0) return;
    const double cellX = local.x / spanX;
    const double cellY = local.z / spanY;
    const int centerX = static_cast<int>(std::floor(cellX));
    const int centerY = static_cast<int>(std::floor(cellY));
    const double fractionX = cellX - centerX;
    const double fractionY = cellY - centerY;
    const int aheadX = fractionX >= .875 ? 1 : fractionX < .125 ? -1 : 0;
    const int aheadY = fractionY >= .875 ? 1 : fractionY < .125 ? -1 : 0;
    m_prefetchCenter = asyncStreaming && !m_editorStreaming &&
        (aheadX || aheadY)
        ? std::optional<Cell>{{centerX + aheadX, centerY + aheadY}}
        : std::nullopt;
    if (m_editorStreaming) InvalidatePrepared();
    m_editorStreaming = false;
    StreamCells(centerX, centerY);
}

void TerrainManager::UpdateEditorStreaming(const glm::dvec3& worldPosition,
    const Camera* camera, int viewportHeight, float aspect)
{
    if (!Owner || !Owner->GetScene() || Owner->GetScene()->HasStarted()) return;
    const glm::dvec3 local = glm::dvec3(
        glm::inverse(Owner->transform.GetWorldMatrixD()) *
        glm::dvec4(worldPosition, 1.0));
    const double spanX = static_cast<double>(cellsX) * cellWidth;
    const double spanZ = static_cast<double>(cellsY) * cellDepth;
    if (spanX <= 0.0 || spanZ <= 0.0) return;
    if (!m_editorStreaming) InvalidatePrepared();
    m_editorStreaming = true;
    m_prefetchCenter.reset();
    const int centerX = static_cast<int>(std::floor(local.x / spanX));
    const int centerY = static_cast<int>(std::floor(local.z / spanZ));
    StreamCells(centerX, centerY);
    if (m_farEnabled) UpdateFarTerrain(centerX, centerY, camera,
        viewportHeight, aspect);
}

bool TerrainManager::TryGetHeightAtWorldPosition(
    const glm::dvec3& worldPosition, double& height) const
{
    if (!Owner || cellsX <= 0 || cellsY <= 0 ||
        cellWidth <= 0.f || cellDepth <= 0.f)
        return false;
    const glm::dvec3 local = glm::dvec3(
        glm::inverse(Owner->transform.GetWorldMatrixD()) *
        glm::dvec4(worldPosition, 1.0));
    const double spanX = static_cast<double>(cellsX) * cellWidth;
    const double spanZ = static_cast<double>(cellsY) * cellDepth;
    if (spanX <= 0.0 || spanZ <= 0.0)
        return false;
    const int cellX = static_cast<int>(std::floor(local.x / spanX));
    const int cellY = static_cast<int>(std::floor(local.z / spanZ));
    const Terrain* terrain = GetLoadedCell(cellX, cellY);
    if (!terrain || terrain->heights.size() !=
        static_cast<size_t>(cellsX + 1) * (cellsY + 1))
        return false;
    const double cellLocalX = local.x - static_cast<double>(cellX) * spanX;
    const double cellLocalZ = local.z - static_cast<double>(cellY) * spanZ;
    const double sampleX = std::clamp(cellLocalX /
        static_cast<double>(cellWidth), 0.0, static_cast<double>(cellsX));
    const double sampleZ = std::clamp(cellLocalZ /
        static_cast<double>(cellDepth), 0.0, static_cast<double>(cellsY));
    const int x0 = std::clamp(static_cast<int>(std::floor(sampleX)), 0, cellsX);
    const int z0 = std::clamp(static_cast<int>(std::floor(sampleZ)), 0, cellsY);
    const int x1 = std::min(cellsX, x0 + 1);
    const int z1 = std::min(cellsY, z0 + 1);
    const double tx = sampleX - static_cast<double>(x0);
    const double tz = sampleZ - static_cast<double>(z0);
    const auto sample = [&](int x, int z) {
        const size_t index = static_cast<size_t>(z) *
            static_cast<size_t>(cellsX + 1) + static_cast<size_t>(x);
        return static_cast<double>(terrain->heights[index]);
    };
    const double h00 = sample(x0, z0);
    const double h10 = sample(x1, z0);
    const double h01 = sample(x0, z1);
    const double h11 = sample(x1, z1);
    const double hx0 = h00 + (h10 - h00) * tx;
    const double hx1 = h01 + (h11 - h01) * tx;
    const double localHeight = hx0 + (hx1 - hx0) * tz;
    const glm::dvec4 world = Owner->transform.GetWorldMatrixD() *
        glm::dvec4(local.x, localHeight, local.z, 1.0);
    height = world.y;
    return std::isfinite(height);
}

void TerrainManager::StreamCells(int centerX, int centerY)
{
    const auto* scene = Owner ? Owner->GetScene() : nullptr;
    const bool runtimeBudget = scene && scene->HasStarted();
    if (runtimeBudget && m_uploadBudgetFrame != scene->GetFrameIndex())
    {
        m_uploadBudgetFrame = scene->GetFrameIndex();
        m_nearFrameUploads = 0;
        m_farFrameUploads = 0;
    }
    const int frameCapacity = std::clamp(maxUploadsPerUpdate, 1, 64);
    const size_t alreadyUploaded = m_nearFrameUploads + m_farFrameUploads;
    const int frameRemaining = runtimeBudget ? static_cast<int>(
        alreadyUploaded < static_cast<size_t>(frameCapacity)
            ? frameCapacity - alreadyUploaded : 0) : frameCapacity;
    const uint64_t batchesBefore = m_renderBatchUploadCount;
    for (auto it = m_loaded.begin(); it != m_loaded.end(); )
    {
        Terrain* terrain = it->second;
        if (!terrain || !terrain->Owner || !terrain->Owner->GetScene())
        {
            it = m_loaded.erase(it);
            continue;
        }
        ++it;
    }
    const int radius = m_editorStreaming ? std::clamp(editorRadius, 0, 32) :
        std::clamp(m_farEnabled && !m_farHeights.empty() ? loadedRadius :
            std::max(loadedRadius, farRadius), 0, 32);
    const auto farCovers = [&](Cell cell) {
        for (const auto& tile : m_farTiles)
        {
            if (!tile.object || !tile.object->enabled || !tile.hasGeometry ||
                cell.first < tile.startX || cell.first >= tile.endX ||
                cell.second < tile.startY || cell.second >= tile.endY)
                continue;
            if (tile.lodStride > 1) return true;
            const size_t index = static_cast<size_t>(cell.second - tile.startY) *
                (tile.endX - tile.startX) + cell.first - tile.startX;
            if (index < tile.builtVisibility.size() &&
                tile.builtVisibility[index]) return true;
        }
        return false;
    };
    const auto wanted = [&](Cell cell) {
        const auto inside = [&](Cell center) {
            return std::abs(cell.first - center.first) <= radius &&
                std::abs(cell.second - center.second) <= radius;
        };
        return inside({centerX, centerY}) ||
            (m_prefetchCenter && inside(*m_prefetchCenter));
    };
    const Cell travel{m_haveLastCenter ? centerX - m_lastCenter.first : 0,
        m_haveLastCenter ? centerY - m_lastCenter.second : 0};
    m_lastCenter = {centerX, centerY};
    m_haveLastCenter = true;
    if (asyncStreaming)
    {
        StartPrepareWorker();
        std::lock_guard<std::mutex> lock(m_prepareMutex);
        for (auto it = m_prepareQueue.begin(); it != m_prepareQueue.end(); )
        {
            const Cell cell = it->cell;
            if (!wanted(cell) ||
                !HasCell(cell.first, cell.second))
            {
                m_preparingCells.erase(cell);
                it = m_prepareQueue.erase(it);
            }
            else ++it;
        }
        const auto score = [&](Cell cell) {
            const int dx = cell.first - centerX, dy = cell.second - centerY;
            return 4 * (dx * dx + dy * dy) -
                2 * (dx * std::clamp(travel.first, -1, 1) +
                    dy * std::clamp(travel.second, -1, 1));
        };
        std::sort(m_prepareQueue.begin(), m_prepareQueue.end(),
            [&](const PrepareTask& a, const PrepareTask& b) {
                const int da = score(a.cell), db = score(b.cell);
                return da == db ? a.cell < b.cell : da < db;
            });
    }
    int unloaded = 0;
    for (auto it = m_loaded.begin(); it != m_loaded.end() &&
        unloaded < std::clamp(maxUnloadsPerUpdate, 1, 64); )
    {
        const Cell cell = it->first;
        ++it;
        if ((std::abs(cell.first - centerX) > radius ||
            std::abs(cell.second - centerY) > radius) &&
            (!m_farEnabled || farCovers(cell)))
        {
            UnloadCell(cell);
            ++unloaded;
        }
    }
    std::vector<Cell> pending;
    const int minX = std::min(centerX,
        m_prefetchCenter ? m_prefetchCenter->first : centerX) - radius;
    const int maxX = std::max(centerX,
        m_prefetchCenter ? m_prefetchCenter->first : centerX) + radius;
    const int minY = std::min(centerY,
        m_prefetchCenter ? m_prefetchCenter->second : centerY) - radius;
    const int maxY = std::max(centerY,
        m_prefetchCenter ? m_prefetchCenter->second : centerY) + radius;
    for (int y = minY; y <= maxY; ++y)
        for (int x = minX; x <= maxX; ++x)
            if (wanted({x, y}) && HasCell(x, y) && !GetLoadedCell(x, y) &&
                (!asyncStreaming || !m_preparingCells.count({x, y})))
                pending.emplace_back(x, y);
    const auto priority = [&](Cell cell) {
        const int dx = cell.first - centerX, dy = cell.second - centerY;
        return 4 * (dx * dx + dy * dy) -
            2 * (dx * std::clamp(travel.first, -1, 1) +
                dy * std::clamp(travel.second, -1, 1));
    };
    std::sort(pending.begin(), pending.end(), [&](Cell a, Cell b) {
        const int da = priority(a), db = priority(b);
        return da == db ? a < b : da < db;
    });
    int loaded = 0;
    const auto beforeLoads = GetMemoryUsage();
    const bool memoryFull =
        beforeLoads.cpuBytes >= (static_cast<uint64_t>(std::max(1, cpuBudgetMiB)) << 20) ||
        beforeLoads.gpuBytes >= (static_cast<uint64_t>(std::max(1, gpuBudgetMiB)) << 20);
    for (Cell cell : pending)
    {
        if (loaded >= std::clamp(maxLoadsPerUpdate, 1, 64)) break;
        if (runtimeBudget && !asyncStreaming && loaded >= frameRemaining) break;
        if (memoryFull) break;
        if (asyncStreaming)
        {
            if (m_preparingCells.size() >=
                static_cast<size_t>(std::clamp(maxPreparedCells, 1, 64))) break;
            PrepareTask task;
            task.cell = cell;
            task.revision = m_prepareRevision;
            task.cacheSignature = m_cacheSignature;
            task.cellsX = cellsX; task.cellsY = cellsY;
            task.cellWidth = cellWidth; task.cellDepth = cellDepth;
            task.lodStride = DesiredLodStride(cell, centerX, centerY);
            const auto saved = m_cells.find(cell);
            task.hasSaved = saved != m_cells.end();
            if (task.hasSaved) task.saved = saved->second;
            task.builder = m_proceduralBuilder;
            if (!task.hasSaved && !task.builder &&
                !(Owner->GetScene()->HasStarted() && requireRuntimeBake)) continue;
            task.cachePath = GetCacheFilePath(cell.first, cell.second);
            if ((m_runtimeBakeActive || requireRuntimeBake) &&
                Owner->GetScene()->HasStarted())
            {
                task.bakePath = GetRuntimeBakeArchivePath();
                if (const auto found = m_bakeIndex.find(cell);
                    found != m_bakeIndex.end())
                {
                    task.bakeOffset = found->second.offset;
                    task.bakeSize = found->second.size;
                }
            }
            task.requireBake = Owner->GetScene()->HasStarted() &&
                (m_runtimeBakeActive || requireRuntimeBake);
            task.edited = m_editedProcedural.count(cell) != 0;
            task.editPath = GetEditFilePath(cell);
            {
                std::lock_guard<std::mutex> lock(m_prepareMutex);
                m_prepareQueue.push_back(std::move(task));
            }
            m_preparingCells[cell] = m_prepareRevision;
            m_prepareWake.notify_one();
            ++loaded;
        }
        else
        {
            LoadCell(cell, DesiredLodStride(cell, centerX, centerY));
            if (GetLoadedCell(cell.first, cell.second)) ++loaded;
        }
    }
    std::vector<Cell> lodPending;
    for (const auto& [cell, terrain] : m_loaded)
        if (terrain && terrain->Owner &&
            terrain->GetMeshLodStride() != DesiredLodStride(cell, centerX, centerY))
            lodPending.push_back(cell);
    std::sort(lodPending.begin(), lodPending.end(), [&](Cell a, Cell b) {
        const auto foundA = m_loaded.find(a);
        const auto foundB = m_loaded.find(b);
        if (foundA == m_loaded.end() || !foundA->second ||
            !foundA->second->Owner) return false;
        if (foundB == m_loaded.end() || !foundB->second ||
            !foundB->second->Owner) return true;
        const int strideA = DesiredLodStride(a, centerX, centerY);
        const int strideB = DesiredLodStride(b, centerX, centerY);
        const bool upgradeA = strideA < foundA->second->GetMeshLodStride();
        const bool upgradeB = strideB < foundB->second->GetMeshLodStride();
        if (upgradeA != upgradeB) return upgradeA;
        const int distanceA = std::max(std::abs(a.first - centerX), std::abs(a.second - centerY));
        const int distanceB = std::max(std::abs(b.first - centerX), std::abs(b.second - centerY));
        return distanceA == distanceB ? a < b : distanceA < distanceB;
    });
    int changed = 0;
    const int uploadCapacity = runtimeBudget
        ? std::max(0, frameRemaining - (asyncStreaming ? 0 : loaded))
        : std::clamp(maxUploadsPerUpdate, 1, 64);
    const int reservedBatchUploads =
        asyncStreaming && batchFarCells && m_renderBatchRefreshPending ? 1 : 0;
    for (Cell cell : lodPending)
    {
        const int lodLimit = runtimeBudget || asyncStreaming
            ? std::min(std::clamp(maxLodChangesPerUpdate, 1, 64),
                std::max(0, uploadCapacity - reservedBatchUploads))
            : std::clamp(maxLodChangesPerUpdate, 1, 64);
        if (changed >= lodLimit) break;
        if (auto found = m_loaded.find(cell);
            found != m_loaded.end() && found->second && found->second->Owner)
        {
            found->second->SetMeshLodStride(
                DesiredLodStride(cell, centerX, centerY));
            ++changed;
        }
    }
    int batchUploads = runtimeBudget
        ? std::max(0, uploadCapacity - changed) : 8;
    int preparedUploads = 0;
    if (asyncStreaming)
    {
        if (!runtimeBudget)
            batchUploads = std::max(0, uploadCapacity - changed);
        const int preparedBudget = batchUploads > 0
            ? std::min(batchUploads, std::max(1,
                batchUploads - reservedBatchUploads))
            : 0;
        preparedUploads = CommitPrepared(centerX, centerY, radius,
            preparedBudget);
        batchUploads -= preparedUploads;
    }
    RefreshColliderActivation(centerX, centerY);
    RefreshRenderBatches(batchUploads, centerX, centerY, radius);
    if (runtimeBudget)
        m_nearFrameUploads += changed + preparedUploads +
            (asyncStreaming ? 0 : loaded) +
            static_cast<size_t>(m_renderBatchUploadCount - batchesBefore);
    TrimMemoryBudget(centerX, centerY);
}

void TerrainManager::Update()
{
    if (!Owner || viewerObjectName.empty()) return;
    auto* viewer = Owner->FindObjectInSceneByName(viewerObjectName);
    if (!viewer) return;
    UpdateStreamingAtPosition(viewer->transform.GetWorldPositionD());
}

void TerrainManager::Start()
{
    m_runtimeBakeActive = false;
    m_bakeIndex.clear();
    if (runtimeBakeDirectory.empty())
    {
        m_bakeStatus = requireRuntimeBake ?
            "Runtime bake required; set a bake directory and bake terrain" :
            "No runtime bake directory; using source terrain";
    }
    else
    {
        m_runtimeBakeActive = ReadRuntimeBakeIndex(
            GetRuntimeBakeArchivePath(), m_bakeIndex);
        if (!m_runtimeBakeActive) m_bakeIndex.clear();
        m_bakeStatus = m_runtimeBakeActive ? "Runtime terrain bake active" :
            requireRuntimeBake ? "Runtime bake required; use Bake all terrain cells" :
            "No complete runtime bake; using source terrain";
    }
    InvalidatePrepared();
    for (auto it = m_loaded.begin(); it != m_loaded.end(); )
    {
        const Cell cell = it->first;
        ++it;
        UnloadCell(cell);
    }
    for (auto& [group, batch] : m_renderBatches)
        if (Owner && Owner->GetScene()) Owner->GetScene()->RemoveObject(batch.object);
    m_renderBatches.clear();
    m_haveLastCenter = false;
}

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
        .Set("farTileLodHysteresis", JsonValue(farTileLodHysteresis))
        .Set("batchFarCells", JsonValue(batchFarCells))
        .Set("fastRuntimeLighting", JsonValue(fastRuntimeLighting))
        .Set("stochasticTerrainVariation", JsonValue(stochasticTerrainVariation))
        .Set("terrainMacroColorEnabled", JsonValue(terrainMacroColorEnabled))
        .Set("measurementGrid", JsonValue(measurementGrid))
        .Set("generateColliders", JsonValue(generateColliders))
        .Set("streamCollidersByDistance",
            JsonValue(streamCollidersByDistance))
        .Set("cpuBudgetMiB", JsonValue(cpuBudgetMiB))
        .Set("gpuBudgetMiB", JsonValue(gpuBudgetMiB))
        .Set("cacheBudgetMiB", JsonValue(cacheBudgetMiB))
        .Set("viewerObjectName", JsonValue(viewerObjectName))
        .Set("cellDataDirectory", JsonValue(cellDataDirectory))
        .Set("runtimeBakeDirectory", JsonValue(runtimeBakeDirectory));
    out.Set("requireRuntimeBake", JsonValue(requireRuntimeBake));
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
    return out.Set("cells", std::move(cells))
        .Set("proceduralRuns", std::move(runs))
        .Set("editedCells", std::move(edits));
}

void TerrainManager::Deserialize(const JsonValue& value)
{
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
    farTileLodHysteresis = value.Has("farTileLodHysteresis")
        ? std::clamp(value["farTileLodHysteresis"].AsFloat(), 0.f, .9f) : .2f;
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
    m_provider = provider;
    if (asyncStreaming)
    {
        UpdateStreaming(0, 0);
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

bool TerrainManager::DrawProperties(::Engine::Editor::IEditorUi& ui)
{
    bool changed = false;
    const int previousX = cellsX, previousY = cellsY;
    const float previousWidth = cellWidth, previousDepth = cellDepth;
    changed |= ui.SliderInt("Cells X", &cellsX, 1, 512);
    changed |= ui.SliderInt("Cells Y", &cellsY, 1, 512);
    changed |= ui.DragFloat("Cell width", &cellWidth, .1f, .001f, 1000.f);
    changed |= ui.DragFloat("Cell depth", &cellDepth, .1f, .001f, 1000.f);
    changed |= ui.SliderInt("Interactive radius", &loadedRadius, 0, 32);
    changed |= ui.SliderInt("Editor radius", &editorRadius, 0, 32);
    changed |= ui.SliderInt("LOD changes per update", &maxLodChangesPerUpdate, 1, 64);
    changed |= ui.SliderInt("Loads per update", &maxLoadsPerUpdate, 1, 64);
    changed |= ui.SliderInt("Unloads per update", &maxUnloadsPerUpdate, 1, 64);
    const bool priorAsync = asyncStreaming;
    changed |= ui.Checkbox("Async streaming", &asyncStreaming);
    changed |= ui.SliderInt("Prepared cell limit", &maxPreparedCells, 1, 64);
    changed |= ui.SliderInt("Uploads per update", &maxUploadsPerUpdate, 1, 64);
    changed |= ui.SliderFloat("Far rebuild budget (ms)",
        &maxFarRebuildMilliseconds, .001f, 100.f);
    changed |= ui.SliderFloat("Far tile error (pixels)",
        &farTileScreenErrorPixels, .1f, 64.f);
    changed |= ui.SliderFloat("Far tile LOD hysteresis",
        &farTileLodHysteresis, 0.f, .9f);
    changed |= ui.Checkbox("Batch far terrain cells", &batchFarCells);
    changed |= ui.Checkbox("Fast runtime terrain lighting", &fastRuntimeLighting);
    changed |= ui.Checkbox("Stochastic terrain tile variation",
        &stochasticTerrainVariation);
    changed |= ui.Checkbox("Terrain macro color blend",
        &terrainMacroColorEnabled);
    changed |= ui.Checkbox("Measurement grid (metres and cell borders)",
        &measurementGrid);
    changed |= ui.Checkbox("Generate terrain cell colliders",
        &generateColliders);
    changed |= ui.Checkbox("Enable only near terrain colliders",
        &streamCollidersByDistance);
    changed |= ui.SliderInt("CPU terrain budget MiB", &cpuBudgetMiB, 1, 16384);
    changed |= ui.SliderInt("GPU terrain budget MiB", &gpuBudgetMiB, 1, 16384);
    changed |= ui.SliderInt("Mesh cache budget MiB", &cacheBudgetMiB, 1, 16384);
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
    if (ui.Button("Add center cell")) changed |= AddCell(0, 0);
    if (changed) MarkConfigurationDirty();
    return changed;
}
namespace
{
constexpr uint64_t FarTileCacheMagic = 0x31454c4954524146ull;
constexpr uint32_t FarTileCacheVersion = 2;
void HashBytes(uint64_t& hash, const void* data, size_t bytes)
{
    const auto* begin = static_cast<const unsigned char*>(data);
    for (size_t i = 0; i < bytes; ++i)
        hash = (hash ^ begin[i]) * 1099511628211ull;
}
template<class T> void HashValue(uint64_t& hash, const T& value)
{
    HashBytes(hash, &value, sizeof(value));
}
struct CachedFarVertex
{
    float pos[3], normal[3], uv[2], color[4];
};
static_assert(sizeof(CachedFarVertex) == sizeof(float) * 12);
struct FarTileCacheHeader
{
    uint64_t magic = FarTileCacheMagic;
    uint32_t version = FarTileCacheVersion;
    uint32_t vertexCount = 0, indexCount = 0;
    uint64_t fingerprint = 0;
    uint64_t payloadHash = 0;
};
bool ReadFarTileCache(const std::filesystem::path& path,
    uint64_t fingerprint, std::vector<Mesh::Vertex>& vertices,
    std::vector<uint32_t>& indices)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) return false;
    const auto bytes = file.tellg();
    file.seekg(0);
    FarTileCacheHeader header;
    file.read(reinterpret_cast<char*>(&header), sizeof(header));
    if (!file || header.magic != FarTileCacheMagic ||
        header.version != FarTileCacheVersion ||
        header.fingerprint != fingerprint ||
        header.vertexCount == 0 || header.vertexCount > 200000 ||
        header.indexCount == 0 || header.indexCount > 1200000 ||
        bytes != static_cast<std::streamoff>(sizeof(header) +
            static_cast<size_t>(header.vertexCount) * sizeof(CachedFarVertex) +
            static_cast<size_t>(header.indexCount) * sizeof(uint32_t)))
        return false;
    std::vector<CachedFarVertex> compact(header.vertexCount);
    indices.resize(header.indexCount);
    file.read(reinterpret_cast<char*>(compact.data()),
        compact.size() * sizeof(CachedFarVertex));
    file.read(reinterpret_cast<char*>(indices.data()),
        indices.size() * sizeof(uint32_t));
    if (!file) return false;
    uint64_t payloadHash = 14695981039346656037ull;
    HashBytes(payloadHash, compact.data(),
        compact.size() * sizeof(CachedFarVertex));
    HashBytes(payloadHash, indices.data(), indices.size() * sizeof(uint32_t));
    if (payloadHash != header.payloadHash) return false;
    for (uint32_t index : indices)
        if (index >= compact.size()) return false;
    vertices.resize(compact.size());
    for (size_t i = 0; i < compact.size(); ++i)
    {
        std::memcpy(vertices[i].pos, compact[i].pos, sizeof(compact[i].pos));
        std::memcpy(vertices[i].normal, compact[i].normal,
            sizeof(compact[i].normal));
        std::memcpy(vertices[i].uv, compact[i].uv, sizeof(compact[i].uv));
        std::memcpy(vertices[i].color, compact[i].color,
            sizeof(compact[i].color));
    }
    return true;
}
void WriteFarTileCache(const std::filesystem::path& path,
    uint64_t fingerprint, const std::vector<Mesh::Vertex>& vertices,
    const std::vector<uint32_t>& indices)
{
    if (vertices.empty() || indices.empty()) return;
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) return;
    const auto temporary = path.string() + ".tmp";
    std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
    if (!file) return;
    std::vector<CachedFarVertex> compact;
    compact.reserve(vertices.size());
    for (const auto& vertex : vertices)
    {
        compact.push_back({{vertex.pos[0], vertex.pos[1], vertex.pos[2]},
            {vertex.normal[0], vertex.normal[1], vertex.normal[2]},
            {vertex.uv[0], vertex.uv[1]},
            {vertex.color[0], vertex.color[1], vertex.color[2], vertex.color[3]}});
    }
    uint64_t payloadHash = 14695981039346656037ull;
    HashBytes(payloadHash, compact.data(),
        compact.size() * sizeof(CachedFarVertex));
    HashBytes(payloadHash, indices.data(), indices.size() * sizeof(uint32_t));
    const FarTileCacheHeader header{FarTileCacheMagic, FarTileCacheVersion,
        static_cast<uint32_t>(vertices.size()),
        static_cast<uint32_t>(indices.size()), fingerprint, payloadHash};
    file.write(reinterpret_cast<const char*>(&header), sizeof(header));
    file.write(reinterpret_cast<const char*>(compact.data()),
        compact.size() * sizeof(CachedFarVertex));
    file.write(reinterpret_cast<const char*>(indices.data()),
        indices.size() * sizeof(uint32_t));
    file.close();
    if (!file) return;
    std::filesystem::rename(temporary, path, error);
    if (error)
    {
        error.clear();
        std::filesystem::remove(path, error);
        error.clear();
        std::filesystem::rename(temporary, path, error);
    }
}
}

void TerrainManager::StartFarPrepareWorker()
{
    if (m_farWorker.joinable()) return;
    m_farWorker = std::thread([this] { FarPrepareLoop(); });
}

void TerrainManager::FarPrepareLoop()
{
    for (;;)
    {
        FarPrepareTask task;
        {
            std::unique_lock<std::mutex> lock(m_farPrepareMutex);
            m_farPrepareWake.wait(lock, [this] {
                return m_stopFarWorker || !m_farPrepareQueue.empty();
            });
            if (m_stopFarWorker) return;
            task = std::move(m_farPrepareQueue.front());
            m_farPrepareQueue.pop_front();
        }
        PreparedFarTile result;
        result.tileIndex = task.tileIndex;
        result.revision = task.revision;
        const auto taskStart = std::chrono::steady_clock::now();
        try
        {
            result.cacheHit = !task.cachePath.empty() &&
                ReadFarTileCache(task.cachePath, task.fingerprint,
                    result.vertices, result.indices);
            if (result.cacheHit && task.lodStride == 1)
            {
                const size_t width = static_cast<size_t>(
                    task.endX - task.startX + 1);
                const size_t depth = static_cast<size_t>(
                    task.endY - task.startY + 1);
                size_t offset = width * depth +
                    4 * (width + depth - 2);
                const size_t patchCount = static_cast<size_t>(task.cellsX + 1) *
                    (task.cellsY + 1);
                for (const auto& [patchIndex, patch] : task.patches)
                    if (!task.hiddenCells.count({patch.x, patch.y}) &&
                        patch.heights.size() == patchCount)
                    {
                        result.patchVertexOffsets[patchIndex] = offset;
                        offset += patchCount;
                    }
                if (offset > result.vertices.size())
                    result.patchVertexOffsets.clear();
            }
            if (!result.cacheHit)
            {
                const int width = (task.endX - task.startX +
                    task.lodStride - 1) / task.lodStride + 1;
                const int depth = (task.endY - task.startY +
                    task.lodStride - 1) / task.lodStride + 1;
                const auto sample = [&](int x, int y) {
                    return (*task.heights)[static_cast<size_t>(
                        std::clamp(y + task.halfExtent, 0, task.side - 1)) *
                        task.side + std::clamp(x + task.halfExtent,
                            0, task.side - 1)];
                };
                const auto visibleCell = [&](int x, int y) {
                    return !task.hiddenCells.count({x, y});
                };
                const auto seamSide = [&](int x, int y) {
                    if (!visibleCell(x - 1, y)) return 0;
                    if (!visibleCell(x, y + 1)) return 1;
                    if (!visibleCell(x + 1, y)) return 2;
                    if (!visibleCell(x, y - 1)) return 3;
                    return -1;
                };
                const auto seamSurface = [&](int x, int y, int seam)
                    -> const FarPrepareTask::SeamSurface* {
                    const Cell neighbor = seam == 0 ? Cell{x - 1, y} :
                        seam == 1 ? Cell{x, y + 1} :
                        seam == 2 ? Cell{x + 1, y} : Cell{x, y - 1};
                    const auto found = task.seams.find(neighbor);
                    return found == task.seams.end() ? nullptr : &found->second;
                };
                const auto detailedNormal = [&](const std::vector<float>& heights,
                    int col, int row) {
                    const auto heightAt = [&](int x, int y) {
                        return heights[static_cast<size_t>(
                            std::clamp(y, 0, task.cellsY) *
                                (task.cellsX + 1) +
                            std::clamp(x, 0, task.cellsX))];
                    };
                    return glm::normalize(glm::vec3(
                        (heightAt(col - 1, row) - heightAt(col + 1, row)) /
                            (2.f * task.cellWidth), 1.f,
                        (heightAt(col, row - 1) - heightAt(col, row + 1)) /
                            (2.f * task.cellDepth)));
                };
                const auto seamNormal = [&](const FarPrepareTask::SeamSurface* surface,
                    int seam, int col, int row) -> std::optional<glm::vec3> {
                    if (!surface) return std::nullopt;
                    if (seam == 0 && col == 0)
                        return detailedNormal(surface->heights, task.cellsX, row);
                    if (seam == 1 && row == task.cellsY)
                        return detailedNormal(surface->heights, col, 0);
                    if (seam == 2 && col == task.cellsX)
                        return detailedNormal(surface->heights, 0, row);
                    if (seam == 3 && row == 0)
                        return detailedNormal(surface->heights, col, task.cellsY);
                    return std::nullopt;
                };
                const auto setColor = [&](Mesh::Vertex& vertex, float height) {
                    float weights[TerrainHeightArchive::LayerCount];
                    TerrainHeightArchive::BlendBaseLayers(height,
                        TerrainHeightArchive::LayerForHeight(height), weights);
                    glm::vec3 color(0.f);
                    for (int layer = 0;
                        layer < TerrainHeightArchive::LayerCount; ++layer)
                        color += task.palette.farColors[layer] * weights[layer];
                    vertex.color[0] = color.r;
                    vertex.color[1] = color.g;
                    vertex.color[2] = color.b;
                    vertex.color[3] = 1.f;
                };
                const auto blendSeamColor = [&](Mesh::Vertex& vertex,
                    const FarPrepareTask::SeamSurface* surface,
                    int seam, int col, int row, float u, float v) {
                    if (!surface || (surface->layerCount != 4 &&
                        surface->layerCount != 6) ||
                        surface->weights.size() != surface->heights.size() *
                            surface->layerCount) return;
                    const float blend = seam == 0 ? 1.f - u :
                        seam == 1 ? v : seam == 2 ? u :
                        seam == 3 ? 1.f - v : 0.f;
                    if (blend <= 0.f) return;
                    const int nearCol = seam == 0 ? task.cellsX :
                        seam == 2 ? 0 : col;
                    const int nearRow = seam == 1 ? 0 :
                        seam == 3 ? task.cellsY : row;
                    const size_t sampleIndex = static_cast<size_t>(nearRow) *
                        (task.cellsX + 1) + nearCol;
                    glm::vec3 color(0.f);
                    for (uint32_t layer = 0; layer < surface->layerCount; ++layer)
                    {
                        const int nearLayer = surface->layerCount == 4
                            ? task.palette.archiveToNear[layer] :
                            static_cast<int>(layer);
                        color += task.palette.nearColors[nearLayer] *
                            surface->weights[sampleIndex *
                                surface->layerCount + layer];
                    }
                    const glm::vec3 farColor(vertex.color[0], vertex.color[1],
                        vertex.color[2]);
                    color = glm::mix(farColor, color, blend);
                    vertex.color[0] = color.r;
                    vertex.color[1] = color.g;
                    vertex.color[2] = color.b;
                };
                result.vertices.resize(static_cast<size_t>(width) * depth);
                float minimumHeight = std::numeric_limits<float>::max();
                float maximumHeight = std::numeric_limits<float>::lowest();
                for (int row = 0; row < depth; ++row)
                    for (int col = 0; col < width; ++col)
                    {
                        const int x = std::min(task.startX + col *
                            task.lodStride, task.endX);
                        const int y = std::min(task.startY + row *
                            task.lodStride, task.endY);
                        auto& vertex = result.vertices[
                            static_cast<size_t>(row) * width + col];
                        const float height = sample(x, y);
                        minimumHeight = std::min(minimumHeight, height);
                        maximumHeight = std::max(maximumHeight, height);
                        vertex.pos[0] = (x - task.startX) * task.spanX;
                        vertex.pos[1] = height;
                        vertex.pos[2] = (y - task.startY) * task.spanZ;
                        const glm::vec3 normal = glm::normalize(glm::vec3(
                            (sample(x - 1, y) - sample(x + 1, y)) /
                                (2.f * task.spanX), 1.f,
                            (sample(x, y - 1) - sample(x, y + 1)) /
                                (2.f * task.spanZ)));
                        vertex.normal[0] = normal.x;
                        vertex.normal[1] = normal.y;
                        vertex.normal[2] = normal.z;
                        vertex.uv[0] = static_cast<float>(x + task.halfExtent) /
                            (task.side - 1);
                        vertex.uv[1] = static_cast<float>(y + task.halfExtent) /
                            (task.side - 1);
                        setColor(vertex, height);
                    }
                if (task.lodStride > 1)
                {
                    for (int row = 0; row < depth - 1; ++row)
                        for (int col = 0; col < width - 1; ++col)
                        {
                            const uint32_t a = row * width + col;
                            const uint32_t b = a + width;
                            result.indices.insert(result.indices.end(),
                                {a, b, a + 1, a + 1, b, b + 1});
                        }
                }
                else for (int y = task.startY; y < task.endY; ++y)
                    for (int x = task.startX; x < task.endX; ++x)
                    {
                        const bool patched = std::any_of(task.patches.begin(),
                            task.patches.end(), [&](const auto& item) {
                                return item.second.x == x && item.second.y == y;
                            });
                        if (!visibleCell(x, y) || patched) continue;
                        const int seam = seamSide(x, y);
                        const auto* surface = seam >= 0
                            ? seamSurface(x, y, seam) : nullptr;
                        if (surface && surface->heights.size() ==
                            static_cast<size_t>(task.cellsX + 1) *
                                (task.cellsY + 1))
                        {
                            const float topLeft = sample(x, y);
                            const float topRight = sample(x + 1, y);
                            const float bottomLeft = sample(x, y + 1);
                            const float bottomRight = sample(x + 1, y + 1);
                            const auto addVertex = [&](float u, float v) {
                                Mesh::Vertex vertex{};
                                float height = (1.f - u) * (1.f - v) * topLeft +
                                    u * (1.f - v) * topRight +
                                    (1.f - u) * v * bottomLeft +
                                    u * v * bottomRight;
                                const int col = static_cast<int>(std::round(
                                    u * task.cellsX));
                                const int row = static_cast<int>(std::round(
                                    v * task.cellsY));
                                const size_t stride = task.cellsX + 1;
                                if (seam == 0 && u == 0.f)
                                    height = surface->heights[row * stride +
                                        task.cellsX];
                                else if (seam == 1 && v == 1.f)
                                    height = surface->heights[col];
                                else if (seam == 2 && u == 1.f)
                                    height = surface->heights[row * stride];
                                else if (seam == 3 && v == 0.f)
                                    height = surface->heights[
                                        task.cellsY * stride + col];
                                vertex.pos[0] = (x - task.startX + u) * task.spanX;
                                vertex.pos[1] = height;
                                vertex.pos[2] = (y - task.startY + v) * task.spanZ;
                                const float dx = ((1.f - v) *
                                    (topRight - topLeft) + v *
                                    (bottomRight - bottomLeft)) / task.spanX;
                                const float dz = ((1.f - u) *
                                    (bottomLeft - topLeft) + u *
                                    (bottomRight - topRight)) / task.spanZ;
                                const glm::vec3 normal = seamNormal(surface,
                                    seam, col, row).value_or(glm::normalize(
                                        glm::vec3(-dx, 1.f, -dz)));
                                vertex.normal[0] = normal.x;
                                vertex.normal[1] = normal.y;
                                vertex.normal[2] = normal.z;
                                vertex.uv[0] = static_cast<float>(x + u +
                                    task.halfExtent) / (task.side - 1);
                                vertex.uv[1] = static_cast<float>(y + v +
                                    task.halfExtent) / (task.side - 1);
                                setColor(vertex, height);
                                blendSeamColor(vertex, surface, seam, col,
                                    row, u, v);
                                const uint32_t index = static_cast<uint32_t>(
                                    result.vertices.size());
                                result.vertices.push_back(vertex);
                                return index;
                            };
                            const uint32_t center = addVertex(.5f, .5f);
                            std::vector<uint32_t> boundary;
                            const int leftSteps = seam == 0 ? task.cellsY : 1;
                            const int bottomSteps = seam == 1 ? task.cellsX : 1;
                            const int rightSteps = seam == 2 ? task.cellsY : 1;
                            const int topSteps = seam == 3 ? task.cellsX : 1;
                            for (int i = 0; i < leftSteps; ++i)
                                boundary.push_back(addVertex(0.f,
                                    static_cast<float>(i) / leftSteps));
                            for (int i = 0; i < bottomSteps; ++i)
                                boundary.push_back(addVertex(
                                    static_cast<float>(i) / bottomSteps, 1.f));
                            for (int i = 0; i < rightSteps; ++i)
                                boundary.push_back(addVertex(1.f,
                                    1.f - static_cast<float>(i) / rightSteps));
                            for (int i = 0; i < topSteps; ++i)
                                boundary.push_back(addVertex(
                                    1.f - static_cast<float>(i) / topSteps, 0.f));
                            for (size_t i = 0; i < boundary.size(); ++i)
                                result.indices.insert(result.indices.end(),
                                    {center, boundary[i],
                                        boundary[(i + 1) % boundary.size()]});
                            continue;
                        }
                        const uint32_t a = static_cast<uint32_t>(
                            (y - task.startY) * width + x - task.startX);
                        const uint32_t b = a + width;
                        result.indices.insert(result.indices.end(),
                            {a, b, a + 1, a + 1, b, b + 1});
                    }
                if (!result.indices.empty())
                {
                const float skirtBottom = minimumHeight -
                    std::max(2.f, maximumHeight - minimumHeight);
                const auto addSkirtEdge = [&](uint32_t first, uint32_t second) {
                    Mesh::Vertex lowFirst = result.vertices[first];
                    Mesh::Vertex lowSecond = result.vertices[second];
                    lowFirst.pos[1] = lowSecond.pos[1] = skirtBottom;
                    const uint32_t lowIndex = static_cast<uint32_t>(
                        result.vertices.size());
                    result.vertices.push_back(lowFirst);
                    result.vertices.push_back(lowSecond);
                    result.indices.insert(result.indices.end(),
                        {first, lowIndex, second, second, lowIndex,
                            lowIndex + 1, second, lowIndex, first,
                            lowIndex + 1, lowIndex, second});
                };
                for (int col = 0; col < width - 1; ++col)
                {
                    addSkirtEdge(col, col + 1);
                    addSkirtEdge((depth - 1) * width + col,
                        (depth - 1) * width + col + 1);
                }
                for (int row = 0; row < depth - 1; ++row)
                {
                    addSkirtEdge(row * width, (row + 1) * width);
                    addSkirtEdge(row * width + width - 1,
                        (row + 1) * width + width - 1);
                }
                }
                for (const auto& [patchIndex, patch] : task.patches)
                {
                    if (!visibleCell(patch.x, patch.y)) continue;
                    const int patchWidth = task.cellsX + 1;
                    const int patchDepth = task.cellsY + 1;
                    if (patch.heights.size() != static_cast<size_t>(
                        patchWidth) * patchDepth) continue;
                    const int seam = seamSide(patch.x, patch.y);
                    const auto* surface = seam >= 0
                        ? seamSurface(patch.x, patch.y, seam) : nullptr;
                    const bool hasNear = surface && surface->heights.size() ==
                        patch.heights.size();
                    const float topLeft = sample(patch.x, patch.y);
                    const float topRight = sample(patch.x + 1, patch.y);
                    const float bottomLeft = sample(patch.x, patch.y + 1);
                    const float bottomRight = sample(patch.x + 1, patch.y + 1);
                    const size_t base = result.vertices.size();
                    result.patchVertexOffsets[patchIndex] = base;
                    const auto patchSample = [&](int x, int y) {
                        return patch.heights[static_cast<size_t>(
                            std::clamp(y, 0, task.cellsY)) * patchWidth +
                            std::clamp(x, 0, task.cellsX)];
                    };
                    for (int row = 0; row < patchDepth; ++row)
                        for (int col = 0; col < patchWidth; ++col)
                        {
                            const float u = static_cast<float>(col) / task.cellsX;
                            const float v = static_cast<float>(row) / task.cellsY;
                            const float edge = (1.f - u) * (1.f - v) * topLeft +
                                u * (1.f - v) * topRight +
                                (1.f - u) * v * bottomLeft +
                                u * v * bottomRight;
                            const float fade = std::min({1.f,
                                static_cast<float>(col), static_cast<float>(row),
                                static_cast<float>(task.cellsX - col),
                                static_cast<float>(task.cellsY - row)});
                            float height = edge +
                                (patchSample(col, row) - edge) * fade;
                            if (hasNear)
                            {
                                if (seam == 0 && col == 0)
                                    height = surface->heights[static_cast<size_t>(
                                        row * patchWidth + task.cellsX)];
                                else if (seam == 1 && row == task.cellsY)
                                    height = surface->heights[col];
                                else if (seam == 2 && col == task.cellsX)
                                    height = surface->heights[static_cast<size_t>(
                                        row * patchWidth)];
                                else if (seam == 3 && row == 0)
                                    height = surface->heights[static_cast<size_t>(
                                        task.cellsY * patchWidth + col)];
                            }
                            Mesh::Vertex vertex{};
                            vertex.pos[0] = (patch.x - task.startX) * task.spanX +
                                col * task.cellWidth;
                            vertex.pos[1] = height;
                            vertex.pos[2] = (patch.y - task.startY) * task.spanZ +
                                row * task.cellDepth;
                            const glm::vec3 coarseNormal = glm::normalize(glm::vec3(
                                (patchSample(col - 1, row) -
                                    patchSample(col + 1, row)) /
                                    (2.f * task.cellWidth), 1.f,
                                (patchSample(col, row - 1) -
                                    patchSample(col, row + 1)) /
                                    (2.f * task.cellDepth)));
                            const glm::vec3 normal = seamNormal(hasNear ?
                                surface : nullptr, seam, col, row).value_or(
                                    coarseNormal);
                            vertex.normal[0] = normal.x;
                            vertex.normal[1] = normal.y;
                            vertex.normal[2] = normal.z;
                            vertex.uv[0] = u;
                            vertex.uv[1] = v;
                            setColor(vertex, height);
                            if (patch.colors.size() == patch.heights.size() * 3)
                                for (int channel = 0; channel < 3; ++channel)
                                    vertex.color[channel] = patch.colors[
                                        (static_cast<size_t>(row) * patchWidth +
                                            col) * 3 + channel];
                            blendSeamColor(vertex, hasNear ? surface : nullptr,
                                seam, col, row, u, v);
                            result.vertices.push_back(vertex);
                        }
                    for (int row = 0; row < task.cellsY; ++row)
                        for (int col = 0; col < task.cellsX; ++col)
                        {
                            const uint32_t a = static_cast<uint32_t>(base +
                                row * patchWidth + col);
                            const uint32_t b = a + patchWidth;
                            result.indices.insert(result.indices.end(),
                                {a, b, a + 1, a + 1, b, b + 1});
                        }
                }
                if (!task.cachePath.empty())
                    WriteFarTileCache(task.cachePath, task.fingerprint,
                        result.vertices, result.indices);
            }
            result.success = true;
        }
        catch (...)
        {
            result.vertices.clear();
            result.indices.clear();
        }
        if (task.lodStride == 1)
            for (int y = task.startY; y < task.endY; ++y)
                for (int x = task.startX; x < task.endX; ++x)
                    result.builtVisibility.push_back(static_cast<uint8_t>(
                        !task.hiddenCells.count({x, y})));
        result.buildMilliseconds = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - taskStart).count();
        {
            std::lock_guard<std::mutex> lock(m_farPrepareMutex);
            if (task.revision == m_farPrepareRevision)
                m_preparedFarTiles.push_back(std::move(result));
        }
    }
}

void TerrainManager::ConfigureFarTerrain(int halfExtentCells,
    std::vector<float> heights, std::vector<TerrainHeightArchive::FarPatch> patches,
    std::string heightArchivePath, uint64_t sourceSignature,
    FarTerrainPalette palette)
{
    ClearFarTerrain();
    m_farHalfExtentCells = halfExtentCells;
    m_farHeights = std::move(heights);
    m_farHeightsSnapshot = std::make_shared<const std::vector<float>>(m_farHeights);
    m_farPatches = std::move(patches);
    for (const auto& patch : m_farPatches)
        m_farPatchCells.emplace(patch.x, patch.y);
    m_farHeightArchivePath = std::move(heightArchivePath);
    m_farSourceSignature = sourceSignature;
    m_farPalette = std::move(palette);
    m_farEnabled = true;
}

void TerrainManager::RefreshFarTerrainEditedCells(
    const std::set<Cell>& changedCells)
{
    if (m_farHeights.empty()) return;
    if (!changedCells.empty())
    {
        std::lock_guard<std::mutex> lock(m_farPrepareMutex);
        ++m_farPrepareRevision;
        m_farPrepareQueue.clear();
        m_preparedFarTiles.clear();
        for (auto& tile : m_farTiles) tile.preparing = false;
    }
    const size_t expected = static_cast<size_t>(cellsX + 1) * (cellsY + 1);
    for (Cell cell : changedCells)
    {
        if (std::abs(cell.first) > m_farHalfExtentCells ||
            std::abs(cell.second) > m_farHalfExtentCells) continue;
        const Terrain* terrain = GetLoadedCell(cell.first, cell.second);
        if (!terrain || terrain->heights.size() != expected) continue;
        std::vector<float> colors;
        if (terrain->weights.size() == expected * 6)
        {
            colors.assign(expected * 3, 0.f);
            for (size_t i = 0; i < expected; ++i)
                for (int layer = 0; layer < 6; ++layer)
                    for (int channel = 0; channel < 3; ++channel)
                        colors[i * 3 + channel] +=
                            terrain->weights[i * 6 + layer] *
                            m_farPalette.nearColors[layer][channel];
        }
        auto patch = std::find_if(m_farPatches.begin(), m_farPatches.end(),
            [&](const auto& item) {
                return item.x == cell.first && item.y == cell.second;
            });
        const bool newPatch = patch == m_farPatches.end();
        size_t patchIndex = newPatch ? m_farPatches.size() :
            static_cast<size_t>(patch - m_farPatches.begin());
        if (patch == m_farPatches.end())
        {
            m_farPatches.push_back({cell.first, cell.second,
                terrain->heights, std::move(colors)});
            m_farPatchCells.insert(cell);
            for (auto& tile : m_farTiles)
                if (tile.lodStride == 1 && cell.first >= tile.startX &&
                    cell.first < tile.endX && cell.second >= tile.startY &&
                    cell.second < tile.endY)
                {
                    tile.patchIndices.push_back(patchIndex);
                    break;
                }
        }
        else
        {
            patch->heights = terrain->heights;
            patch->colors = std::move(colors);
        }
        for (auto& tile : m_farTiles)
            if (cell.first >= tile.startX && cell.first < tile.endX &&
                cell.second >= tile.startY && cell.second < tile.endY)
            {
                if (!newPatch && tile.lodStride == 1 && tile.built &&
                    tile.patchVertexOffsets.count(patchIndex))
                    tile.dirtyPatchIndices.insert(patchIndex);
                else tile.built = false;
            }
    }
}

void TerrainManager::ClearFarTerrain()
{
    {
        std::lock_guard<std::mutex> lock(m_farPrepareMutex);
        ++m_farPrepareRevision;
        m_farPrepareQueue.clear();
        m_preparedFarTiles.clear();
    }
    if (Owner && Owner->GetScene())
        for (const auto& tile : m_farTiles)
            if (tile.object) Owner->GetScene()->RemoveObject(tile.object);
    m_farTiles.clear();
    m_farHeights.clear();
    m_farHeightsSnapshot.reset();
    m_farPatches.clear();
    m_farPatchCells.clear();
    m_farHeightArchivePath.clear();
    m_farHalfExtentCells = 0;
    m_farSourceSignature = 0;
    m_farTileUploadCount = 0;
    m_farTileCacheHits = 0;
    m_farTileUploadedBytes = 0;
    m_farTileRebuildMilliseconds = 0.0;
    m_farEnabled = false;
}

void TerrainManager::SetFarTerrainEnabled(bool enabled)
{
    if (m_farEnabled == enabled) return;
    m_farEnabled = enabled;
    if (!enabled)
    {
        {
            std::lock_guard<std::mutex> lock(m_farPrepareMutex);
            ++m_farPrepareRevision;
            m_farPrepareQueue.clear();
            m_preparedFarTiles.clear();
        }
        for (auto& tile : m_farTiles) tile.preparing = false;
        for (const auto& tile : m_farTiles)
            if (tile.object) tile.object->enabled = false;
    }
}

std::vector<Mesh*>
TerrainManager::GetFarTileMeshes() const
{
    std::vector<Mesh*> meshes;
    for (const auto& tile : m_farTiles)
        if (tile.object && tile.object->enabled && tile.mesh &&
            tile.mesh->IsReady())
            meshes.push_back(tile.mesh);
    return meshes;
}

size_t TerrainManager::GetVisibleFarTileCount() const
{
    size_t count = 0;
    for (const auto& tile : m_farTiles)
        if (tile.object && tile.object->enabled && tile.hasGeometry) ++count;
    return count;
}

size_t TerrainManager::GetVisibleFarTileCountForStride(int stride) const
{
    return std::count_if(m_farTiles.begin(), m_farTiles.end(),
        [stride](const FarTile& tile) {
            return tile.lodStride == stride && tile.object &&
                tile.object->enabled && tile.hasGeometry;
        });
}

size_t TerrainManager::GetDesiredFarTileCount() const
{
    return std::count_if(m_farTiles.begin(), m_farTiles.end(),
        [](const FarTile& tile) { return tile.desired; });
}

size_t TerrainManager::GetBuiltFarTileCount() const
{
    return std::count_if(m_farTiles.begin(), m_farTiles.end(),
        [](const FarTile& tile) { return tile.built; });
}

size_t TerrainManager::GetPreparingFarTileCount() const
{
    return std::count_if(m_farTiles.begin(), m_farTiles.end(),
        [](const FarTile& tile) { return tile.preparing; });
}

size_t TerrainManager::GetQueuedFarTileCount() const
{
    std::lock_guard<std::mutex> lock(m_farPrepareMutex);
    return m_farPrepareQueue.size();
}

size_t TerrainManager::GetPreparedFarTileCount() const
{
    std::lock_guard<std::mutex> lock(m_farPrepareMutex);
    return m_preparedFarTiles.size();
}

size_t TerrainManager::GetVisibleFarTileVertexCount() const
{
    size_t count = 0;
    for (const auto& tile : m_farTiles)
        if (tile.object && tile.object->enabled && tile.hasGeometry &&
            tile.mesh) count += tile.mesh->GetVertexCount();
    return count;
}

void TerrainManager::UpdateFarTerrain(int centerX, int centerY,
    const Camera* camera, int viewportHeight, float aspect)
{
    if (!Owner || !m_farEnabled || m_farHeights.empty()) return;
    auto* scene = Owner->GetScene();
    if (!scene) return;
    const int side = m_farHalfExtentCells * 2 + 2;
    if (m_farHeights.size() != static_cast<size_t>(side) * side) return;
    const float spanX = cellsX * cellWidth;
    const float spanZ = cellsY * cellDepth;
    constexpr int tileCells = 32;
    const int nearRadius = m_editorStreaming
        ? std::clamp(editorRadius, 0, 32)
        : std::clamp(loadedRadius, 0, 32);
    const auto visible = [this, nearRadius](int x, int y, int cx, int cy) {
        if (std::max(std::abs(x - cx), std::abs(y - cy)) > nearRadius)
            return true;
        const Terrain* nearTerrain = GetLoadedCell(x, y);
        if (!nearTerrain || !nearTerrain->Owner ||
            !nearTerrain->Owner->enabled) return true;
        const Mesh* nearMesh = nearTerrain->Owner->GetComponent<Mesh>();
        return !nearMesh || !nearMesh->IsReady();
    };
    const auto gridHeight = [&](int x, int y) {
        return m_farHeights[static_cast<size_t>(
            std::clamp(y + m_farHalfExtentCells, 0, side - 1)) * side +
            std::clamp(x + m_farHalfExtentCells, 0, side - 1)];
    };
    // The detailed LOD keeps all outer samples. Read those same heights for
    // the first far cell so its shared edge has identical vertices.
    const std::string& archivePath = m_farHeightArchivePath;
    struct SeamSurface
    {
        const std::vector<float>* heights = nullptr;
        const std::vector<float>* weights = nullptr;
        uint32_t layerCount = 0;
        std::vector<float> ownedHeights;
        std::vector<float> ownedWeights;
    };
    std::map<std::pair<int, int>, SeamSurface> seamCache;
    const auto seamSurface = [&](int x, int y) -> const SeamSurface* {
        const size_t expected = static_cast<size_t>(cellsX + 1) *
            (cellsY + 1);
        auto [it, inserted] = seamCache.try_emplace({x, y});
        SeamSurface& surface = it->second;
        if (inserted)
        {
            if (const auto* loaded = GetLoadedCell(x, y);
                loaded && loaded->heights.size() == expected)
            {
                surface.heights = &loaded->heights;
                surface.weights = &loaded->weights;
                surface.layerCount = static_cast<uint32_t>(loaded->layers.size());
            }
            else
            {
                const bool bakedRuntime = scene->HasStarted() &&
                    requireRuntimeBake;
                bool read = false;
                if (bakedRuntime)
                    read = ReadBakedCellHeights({x, y},
                        surface.ownedHeights, &surface.ownedWeights,
                        &surface.layerCount);
                else
                {
                    read = TerrainHeightArchive::ReadCell(archivePath,
                        m_farSourceSignature, m_farHalfExtentCells,
                        cellsX, cellsY, x, y,
                        surface.ownedHeights, &surface.ownedWeights);
                    surface.layerCount = TerrainHeightArchive::LayerCount;
                }
                if (read)
                {
                    surface.heights = &surface.ownedHeights;
                    surface.weights = &surface.ownedWeights;
                }
            }
        }
        return surface.heights && surface.heights->size() == expected
            ? &surface : nullptr;
    };
    // 0/1/2/3 identify the far cell's left/bottom/right/top edge.
    const auto seamSide = [&](int x, int y, int cx, int cy) {
        if (!visible(x - 1, y, cx, cy)) return 0;
        if (!visible(x, y + 1, cx, cy)) return 1;
        if (!visible(x + 1, y, cx, cy)) return 2;
        if (!visible(x, y - 1, cx, cy)) return 3;
        return -1;
    };
    const auto detailedNormal = [&](const std::vector<float>& heights,
        int col, int row) {
        const auto sample = [&](int x, int y) {
            return heights[static_cast<size_t>(
                std::clamp(y, 0, cellsY) * (cellsX + 1) +
                std::clamp(x, 0, cellsX))];
        };
        return glm::normalize(glm::vec3(
            (sample(col - 1, row) - sample(col + 1, row)) /
                (2.f * cellWidth), 1.f,
            (sample(col, row - 1) - sample(col, row + 1)) /
                (2.f * cellDepth)));
    };
    const auto seamNormal = [&](const std::vector<float>* nearHeights,
        int seam, int col, int row) -> std::optional<glm::vec3> {
        if (!nearHeights) return std::nullopt;
        if (seam == 0 && col == 0)
            return detailedNormal(*nearHeights, cellsX, row);
        if (seam == 1 && row == cellsY)
            return detailedNormal(*nearHeights, col, 0);
        if (seam == 2 && col == cellsX)
            return detailedNormal(*nearHeights, 0, row);
        if (seam == 3 && row == 0)
            return detailedNormal(*nearHeights, col, cellsY);
        return std::nullopt;
    };
    const auto setColor = [&](Mesh::Vertex& vertex,
        float height) {
        float weights[TerrainHeightArchive::LayerCount];
        TerrainHeightArchive::BlendBaseLayers(height,
            TerrainHeightArchive::LayerForHeight(height), weights);
        glm::vec3 color(0.f);
        for (int layer = 0; layer < TerrainHeightArchive::LayerCount; ++layer)
            color += m_farPalette.farColors[layer] * weights[layer];
        vertex.color[0] = color.r;
        vertex.color[1] = color.g;
        vertex.color[2] = color.b;
        vertex.color[3] = 1.f;
    };
    const auto blendSeamColor = [&](Mesh::Vertex& vertex,
        const SeamSurface* surface, int seam, int col, int row,
        float u, float v) {
        if (!surface || !surface->weights ||
            (surface->layerCount != 4 && surface->layerCount != 6))
            return;
        const float edgeBlend = seam == 0 ? 1.f - u :
            seam == 1 ? v : seam == 2 ? u :
            seam == 3 ? 1.f - v : 0.f;
        if (edgeBlend <= 0.f) return;
        const int nearCol = seam == 0 ? cellsX :
            seam == 2 ? 0 : col;
        const int nearRow = seam == 1 ? 0 :
            seam == 3 ? cellsY : row;
        const size_t sample = static_cast<size_t>(nearRow) *
            (cellsX + 1) + nearCol;
        if (surface->weights->size() != surface->heights->size() *
            surface->layerCount) return;
        glm::vec3 color(0.f);
        for (uint32_t layer = 0; layer < surface->layerCount; ++layer)
        {
            const int nearLayer = surface->layerCount == 4
                ? m_farPalette.archiveToNear[layer] : static_cast<int>(layer);
            color += m_farPalette.nearColors[nearLayer] *
                (*surface->weights)[sample * surface->layerCount + layer];
        }
        const glm::vec3 farColor(vertex.color[0], vertex.color[1],
            vertex.color[2]);
        color = glm::mix(farColor, color, edgeBlend);
        vertex.color[0] = color.r;
        vertex.color[1] = color.g;
        vertex.color[2] = color.b;
    };
    if (m_farTiles.empty())
    {
        // Align one parent around the origin so the outer regions can use
        // coarse tiles even on a 129-cell-wide authored map.
        constexpr int parentCells = tileCells * 2;
        const int minimum = -m_farHalfExtentCells;
        const int maximum = m_farHalfExtentCells + 1;
        const int firstParent = -tileCells + static_cast<int>(std::floor(
            static_cast<double>(minimum + tileCells) / parentCells)) *
            parentCells;
        for (int y = firstParent; y < maximum; y += parentCells)
            for (int x = firstParent; x < maximum; x += parentCells)
            {
                FarTile parent;
                parent.startX = std::max(x, minimum);
                parent.startY = std::max(y, minimum);
                parent.endX = std::min(x + parentCells, maximum);
                parent.endY = std::min(y + parentCells, maximum);
                if (parent.startX >= parent.endX ||
                    parent.startY >= parent.endY) continue;
                parent.lodStride = 2;
                parent.minHeight = std::numeric_limits<float>::max();
                parent.maxHeight = std::numeric_limits<float>::lowest();
                for (int gy = parent.startY; gy <= parent.endY; ++gy)
                    for (int gx = parent.startX; gx <= parent.endX; ++gx)
                    {
                        const float height = gridHeight(gx, gy);
                        parent.minHeight = std::min(parent.minHeight, height);
                        parent.maxHeight = std::max(parent.maxHeight, height);
                        const int x0 = parent.startX +
                            ((gx - parent.startX) / 2) * 2;
                        const int y0 = parent.startY +
                            ((gy - parent.startY) / 2) * 2;
                        const int x1 = std::min(x0 + 2, parent.endX);
                        const int y1 = std::min(y0 + 2, parent.endY);
                        const float tx = x1 == x0 ? 0.f :
                            static_cast<float>(gx - x0) / (x1 - x0);
                        const float ty = y1 == y0 ? 0.f :
                            static_cast<float>(gy - y0) / (y1 - y0);
                        const float bottom = glm::mix(gridHeight(x0, y0),
                            gridHeight(x1, y0), tx);
                        const float top = glm::mix(gridHeight(x0, y1),
                            gridHeight(x1, y1), tx);
                        parent.maxHeightError = std::max(parent.maxHeightError,
                            std::abs(height - glm::mix(bottom, top, ty)));
                    }
                for (int childY = y; childY < y + parentCells;
                    childY += tileCells)
                    for (int childX = x; childX < x + parentCells;
                        childX += tileCells)
                    {
                        FarTile child;
                        child.startX = std::max(childX, minimum);
                        child.startY = std::max(childY, minimum);
                        child.endX = std::min(childX + tileCells, maximum);
                        child.endY = std::min(childY + tileCells, maximum);
                        if (child.startX >= child.endX ||
                            child.startY >= child.endY) continue;
                        parent.children.push_back(m_farTiles.size());
                        m_farTiles.push_back(std::move(child));
                    }
                for (size_t childIndex : parent.children)
                    m_farTiles[childIndex].parentIndex = m_farTiles.size();
                m_farTiles.push_back(std::move(parent));
            }
        // Group four 64-cell tiles into a 128-cell, quarter-resolution tile.
        // The root can replace a whole region with one draw at long range.
        const size_t parentCount = m_farTiles.size();
        std::map<Cell, std::vector<size_t>> rootGroups;
        const auto floorDiv = [](int value, int divisor) {
            return value >= 0 ? value / divisor :
                (value - divisor + 1) / divisor;
        };
        for (size_t index = 0; index < parentCount; ++index)
            if (!m_farTiles[index].children.empty())
            {
                const auto& tile = m_farTiles[index];
                rootGroups[{floorDiv(tile.startX + 96, 128),
                    floorDiv(tile.startY + 96, 128)}].push_back(index);
            }
        for (auto& [group, children] : rootGroups)
        {
            FarTile root;
            root.lodStride = 4;
            root.children = std::move(children);
            root.startX = root.startY = std::numeric_limits<int>::max();
            root.endX = root.endY = std::numeric_limits<int>::lowest();
            for (size_t childIndex : root.children)
            {
                const auto& child = m_farTiles[childIndex];
                root.startX = std::min(root.startX, child.startX);
                root.startY = std::min(root.startY, child.startY);
                root.endX = std::max(root.endX, child.endX);
                root.endY = std::max(root.endY, child.endY);
            }
            root.minHeight = std::numeric_limits<float>::max();
            root.maxHeight = std::numeric_limits<float>::lowest();
            for (int gy = root.startY; gy <= root.endY; ++gy)
                for (int gx = root.startX; gx <= root.endX; ++gx)
                {
                    const float height = gridHeight(gx, gy);
                    root.minHeight = std::min(root.minHeight, height);
                    root.maxHeight = std::max(root.maxHeight, height);
                    const int x0 = root.startX + ((gx - root.startX) / 4) * 4;
                    const int y0 = root.startY + ((gy - root.startY) / 4) * 4;
                    const int x1 = std::min(x0 + 4, root.endX);
                    const int y1 = std::min(y0 + 4, root.endY);
                    const float tx = x1 == x0 ? 0.f :
                        static_cast<float>(gx - x0) / (x1 - x0);
                    const float ty = y1 == y0 ? 0.f :
                        static_cast<float>(gy - y0) / (y1 - y0);
                    const float bottom = glm::mix(gridHeight(x0, y0),
                        gridHeight(x1, y0), tx);
                    const float top = glm::mix(gridHeight(x0, y1),
                        gridHeight(x1, y1), tx);
                    root.maxHeightError = std::max(root.maxHeightError,
                        std::abs(height - glm::mix(bottom, top, ty)));
                }
            for (size_t childIndex : root.children)
                m_farTiles[childIndex].parentIndex = m_farTiles.size();
            m_farTiles.push_back(std::move(root));
        }
        for (size_t patchIndex = 0; patchIndex < m_farPatches.size();
            ++patchIndex)
        {
            const auto& patch = m_farPatches[patchIndex];
            for (auto& tile : m_farTiles)
                if (tile.lodStride == 1 && patch.x >= tile.startX &&
                    patch.x < tile.endX && patch.y >= tile.startY &&
                    patch.y < tile.endY)
                {
                    tile.patchIndices.push_back(patchIndex);
                    break;
                }
        }
    }
    for (auto& tile : m_farTiles) tile.desired = false;
    const glm::dmat4 terrainWorld = Owner->transform.GetWorldMatrixD();
    const glm::dmat4 cameraView = camera ? camera->GetViewMatrixD() :
        glm::dmat4(1.0);
    const double terrainScale = std::max({
        glm::length(glm::dvec3(terrainWorld[0])),
        glm::length(glm::dvec3(terrainWorld[1])),
        glm::length(glm::dvec3(terrainWorld[2]))});
    const double halfFovY = camera ? glm::radians(static_cast<double>(
        std::clamp(camera->fov, 1.f, 179.f))) * .5 : 0.0;
    const double tanY = camera && !camera->orthographic ?
        std::tan(halfFovY) : 0.0;
    const double viewAspect = std::max(.01, static_cast<double>(aspect));
    const double viewHeight = std::max(1, viewportHeight);
    const glm::dvec3 cameraWorld = camera && camera->Owner
        ? camera->Owner->transform.GetWorldPositionD() : glm::dvec3(0.0);
    for (auto& tile : m_farTiles)
    {
        tile.outOfRange = false;
        if (!camera) continue;
        const glm::dvec3 localCenter(
            (tile.startX + tile.endX) * .5 * spanX,
            (tile.minHeight + tile.maxHeight) * .5,
            (tile.startY + tile.endY) * .5 * spanZ);
        const glm::dvec3 worldCenter = glm::dvec3(terrainWorld *
            glm::dvec4(localCenter, 1.0));
        const double radius = terrainScale * glm::length(glm::dvec3(
            (tile.endX - tile.startX) * .5 * spanX,
            (tile.maxHeight - tile.minHeight) * .5,
            (tile.endY - tile.startY) * .5 * spanZ));
        tile.outOfRange = glm::distance(worldCenter, cameraWorld) - radius >
            camera->farPlane;
    }
    std::function<void(size_t)> selectTile = [&](size_t tileIndex)
    {
        auto& parent = m_farTiles[tileIndex];
        if (parent.outOfRange) return;
        if (parent.children.empty())
        {
            parent.desired = true;
            return;
        }
        const int dx = std::max({parent.startX - centerX, 0,
            centerX - (parent.endX - 1)});
        const int dy = std::max({parent.startY - centerY, 0,
            centerY - (parent.endY - 1)});
        const bool hasPatch = std::any_of(m_farPatchCells.begin(),
            m_farPatchCells.end(), [&](Cell cell) {
                return cell.first >= parent.startX && cell.first < parent.endX &&
                    cell.second >= parent.startY && cell.second < parent.endY;
            });
        const bool outsideNear = std::max(dx, dy) > nearRadius + 1;
        if (!outsideNear || hasPatch)
            parent.coarseSelected = false;
        else if (camera)
        {
            const glm::dvec3 localCenter(
                (parent.startX + parent.endX) * .5 * spanX,
                (parent.minHeight + parent.maxHeight) * .5,
                (parent.startY + parent.endY) * .5 * spanZ);
            const glm::dvec3 viewCenter = glm::dvec3(cameraView *
                terrainWorld * glm::dvec4(localCenter, 1.0));
            const double radius = terrainScale * glm::length(glm::dvec3(
                (parent.endX - parent.startX) * .5 * spanX,
                (parent.maxHeight - parent.minHeight) * .5,
                (parent.endY - parent.startY) * .5 * spanZ));
            const double nearDistance = std::max(.001,
                static_cast<double>(camera->nearPlane));
            const double farDistance = std::max(nearDistance,
                static_cast<double>(camera->farPlane));
            const double halfY = camera->orthographic ?
                std::max(.01, static_cast<double>(camera->orthographicSize)) :
                std::max(0.0, viewCenter.z) * tanY;
            const bool inView = viewCenter.z + radius >= nearDistance &&
                viewCenter.z - radius <= farDistance &&
                std::abs(viewCenter.x) <= halfY * viewAspect + radius &&
                std::abs(viewCenter.y) <= halfY + radius;
            const double errorPixels = camera->orthographic ?
                parent.maxHeightError * terrainScale * viewHeight /
                    (2.0 * std::max(.01, static_cast<double>(camera->orthographicSize))) :
                parent.maxHeightError * terrainScale * viewHeight /
                    (2.0 * tanY * std::max(nearDistance, viewCenter.z - radius));
            const double threshold = std::clamp(farTileScreenErrorPixels, .1f, 64.f);
            const double hysteresis = std::clamp(farTileLodHysteresis, 0.f, .9f);
            if (!inView) parent.coarseSelected = true;
            else if (parent.coarseSelected)
                parent.coarseSelected = errorPixels <= threshold * (1.0 + hysteresis);
            else
                parent.coarseSelected = errorPixels < threshold * (1.0 - hysteresis);
        }
        else parent.coarseSelected = true;
        parent.desired = outsideNear && !hasPatch && parent.coarseSelected;
        if (!parent.desired)
            for (size_t childIndex : parent.children)
                selectTile(childIndex);
    };
    for (size_t index = 0; index < m_farTiles.size(); ++index)
        if (m_farTiles[index].parentIndex == static_cast<size_t>(-1))
            selectTile(index);
    const auto needsRebuild = [&](const FarTile& tile) {
        if (!tile.built) return true;
        if (tile.lodStride > 1) return false;
        if (tile.builtNearRadius != nearRadius) return true;
        if (tile.builtVisibility.size() != static_cast<size_t>(
            (tile.endX - tile.startX) * (tile.endY - tile.startY))) return true;
        for (int y = tile.startY; y < tile.endY; ++y)
            for (int x = tile.startX; x < tile.endX; ++x)
                if (tile.builtVisibility[static_cast<size_t>(
                    y - tile.startY) * (tile.endX - tile.startX) +
                    x - tile.startX] !=
                    static_cast<uint8_t>(visible(x, y, centerX, centerY)))
                    return true;
        return false;
    };
    std::vector<size_t> pending;
    pending.reserve(m_farTiles.size());
    for (size_t index = 0; index < m_farTiles.size(); ++index)
    {
        auto& tile = m_farTiles[index];
        if (tile.desired && (needsRebuild(tile) ||
            !tile.dirtyPatchIndices.empty())) pending.push_back(index);
    }
    const auto tileDistance = [&](size_t index) {
        const FarTile& tile = m_farTiles[index];
        const int dx = std::max({tile.startX - centerX, 0,
            centerX - (tile.endX - 1)});
        const int dy = std::max({tile.startY - centerY, 0,
            centerY - (tile.endY - 1)});
        return std::max(dx, dy);
    };
    const auto exposesGap = [&](size_t index) {
        const FarTile& tile = m_farTiles[index];
        if (tile.lodStride != 1 || !tile.built ||
            tile.builtVisibility.size() != static_cast<size_t>(
                (tile.endX - tile.startX) * (tile.endY - tile.startY)))
            return false;
        for (int y = tile.startY; y < tile.endY; ++y)
            for (int x = tile.startX; x < tile.endX; ++x)
                if (visible(x, y, centerX, centerY) &&
                    !tile.builtVisibility[static_cast<size_t>(
                        y - tile.startY) * (tile.endX - tile.startX) +
                        x - tile.startX])
                    return true;
        return false;
    };
    std::set<size_t> readyIndices;
    {
        std::lock_guard<std::mutex> lock(m_farPrepareMutex);
        for (const auto& prepared : m_preparedFarTiles)
            if (prepared.revision == m_farPrepareRevision)
                readyIndices.insert(prepared.tileIndex);
    }
    const auto cameraVisible = [&](size_t index) {
        if (!camera) return true;
        const FarTile& tile = m_farTiles[index];
        const glm::dvec3 localCenter(
            (tile.startX + tile.endX) * .5 * spanX, 0.0,
            (tile.startY + tile.endY) * .5 * spanZ);
        const glm::dvec3 viewCenter = glm::dvec3(cameraView *
            terrainWorld * glm::dvec4(localCenter, 1.0));
        const double radius = terrainScale * glm::length(glm::dvec3(
            (tile.endX - tile.startX) * .5 * spanX,
            std::max(1.f, tile.maxHeight - tile.minHeight),
            (tile.endY - tile.startY) * .5 * spanZ));
        const double halfY = camera->orthographic ?
            std::max(.01, static_cast<double>(camera->orthographicSize)) :
            std::max(0.0, viewCenter.z) * tanY;
        return viewCenter.z + radius >= camera->nearPlane &&
            viewCenter.z - radius <= camera->farPlane &&
            std::abs(viewCenter.x) <= halfY * viewAspect + radius &&
            std::abs(viewCenter.y) <= halfY + radius;
    };
    std::sort(pending.begin(), pending.end(), [&](size_t a, size_t b) {
        if (exposesGap(a) != exposesGap(b)) return exposesGap(a);
        if (readyIndices.count(a) != readyIndices.count(b))
            return readyIndices.count(a) != 0;
        if (cameraVisible(a) != cameraVisible(b)) return cameraVisible(a);
        const int distanceA = tileDistance(a), distanceB = tileDistance(b);
        return distanceA == distanceB ? a < b : distanceA < distanceB;
    });
    {
        std::map<size_t, size_t> priority;
        for (size_t order = 0; order < pending.size(); ++order)
            priority[pending[order]] = order;
        std::lock_guard<std::mutex> lock(m_farPrepareMutex);
        for (auto it = m_farPrepareQueue.begin();
            it != m_farPrepareQueue.end(); )
        {
            if (it->revision != m_farPrepareRevision ||
                !priority.count(it->tileIndex))
            {
                if (it->tileIndex < m_farTiles.size())
                    m_farTiles[it->tileIndex].preparing = false;
                it = m_farPrepareQueue.erase(it);
            }
            else ++it;
        }
        for (auto it = m_preparedFarTiles.begin();
            it != m_preparedFarTiles.end(); )
        {
            if (it->revision != m_farPrepareRevision ||
                !priority.count(it->tileIndex))
            {
                if (it->tileIndex < m_farTiles.size())
                    m_farTiles[it->tileIndex].preparing = false;
                it = m_preparedFarTiles.erase(it);
            }
            else ++it;
        }
        std::sort(m_farPrepareQueue.begin(), m_farPrepareQueue.end(),
            [&](const FarPrepareTask& a, const FarPrepareTask& b) {
                return priority.at(a.tileIndex) < priority.at(b.tileIndex);
            });
    }
    const size_t urgentCount = static_cast<size_t>(std::count_if(
        pending.begin(), pending.end(), exposesGap));
    // Urgent coverage repairs can exceed the ordinary upload limit so an
    // unloaded near cell never leaves a hole for a full frame.
    const size_t configuredUploads = static_cast<size_t>(
        std::clamp(maxUploadsPerUpdate, 1, 64));
    size_t ordinaryUploads = configuredUploads;
    if (scene->HasStarted())
    {
        const uint64_t frame = scene->GetFrameIndex();
        if (m_uploadBudgetFrame != frame)
        {
            m_uploadBudgetFrame = frame;
            m_nearFrameUploads = 0;
            m_farFrameUploads = 0;
        }
        const size_t used = m_nearFrameUploads + m_farFrameUploads;
        ordinaryUploads = configuredUploads > used
            ? configuredUploads - used : 0;
    }
    const size_t uploadLimit = std::max(urgentCount, ordinaryUploads);
    size_t uploadsThisUpdate = 0;
    size_t workedTiles = 0;
    const auto updateStart = std::chrono::steady_clock::now();
    for (size_t order = 0; order < pending.size(); ++order)
    {
        const size_t tileIndex = pending[order];
        // A task already running on the worker must not consume this frame's
        // scheduling budget again and starve every tile behind it.
        if (m_farTiles[tileIndex].preparing &&
            !readyIndices.count(tileIndex)) continue;
        const bool canUpload = uploadsThisUpdate < uploadLimit;
        // The time limit is soft: complete at least one tile so a large tile
        // cannot permanently block the queue.
        if (order >= urgentCount && workedTiles > 0 &&
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - updateStart).count() >=
                std::clamp(maxFarRebuildMilliseconds, .001f, 100.f)) break;
        ++workedTiles;
        auto& tile = m_farTiles[tileIndex];
        const auto rebuildStart = std::chrono::steady_clock::now();
        if (!canUpload && !tile.dirtyPatchIndices.empty()) continue;
        if (tile.built && tile.mesh && !needsRebuild(tile) &&
            !tile.dirtyPatchIndices.empty())
        {
            bool canUpdate = true;
            const size_t patchWidth = static_cast<size_t>(cellsX + 1);
            const size_t patchCount = patchWidth * (cellsY + 1);
            for (size_t patchIndex : tile.dirtyPatchIndices)
            {
                const auto& patch = m_farPatches[patchIndex];
                const auto offset = tile.patchVertexOffsets.find(patchIndex);
                if (offset == tile.patchVertexOffsets.end() ||
                    patch.heights.size() != patchCount ||
                    offset->second + patchCount > tile.mesh->GetVertices().size() ||
                    seamSide(patch.x, patch.y, centerX, centerY) != -1 ||
                    !visible(patch.x, patch.y, centerX, centerY))
                { canUpdate = false; break; }
            }
            if (canUpdate)
            {
                for (size_t patchIndex : tile.dirtyPatchIndices)
                {
                    const auto& patch = m_farPatches[patchIndex];
                    const size_t offset = tile.patchVertexOffsets[patchIndex];
                    const auto& stored = tile.mesh->GetVertices();
                    std::vector<Mesh::Vertex> changed(stored.begin() + offset,
                        stored.begin() + offset + patchCount);
                    const float topLeft = gridHeight(patch.x, patch.y);
                    const float topRight = gridHeight(patch.x + 1, patch.y);
                    const float bottomLeft = gridHeight(patch.x, patch.y + 1);
                    const float bottomRight = gridHeight(patch.x + 1, patch.y + 1);
                    const auto sample = [&](int sx, int sy) {
                        return patch.heights[static_cast<size_t>(
                            std::clamp(sy, 0, cellsY)) * patchWidth +
                            std::clamp(sx, 0, cellsX)];
                    };
                    for (int row = 0; row <= cellsY; ++row)
                        for (int col = 0; col <= cellsX; ++col)
                        {
                            const float u = static_cast<float>(col) / cellsX;
                            const float v = static_cast<float>(row) / cellsY;
                            const float edge = (1.f - u) * (1.f - v) * topLeft +
                                u * (1.f - v) * topRight +
                                (1.f - u) * v * bottomLeft +
                                u * v * bottomRight;
                            const float fade = std::min({1.f,
                                static_cast<float>(col), static_cast<float>(row),
                                static_cast<float>(cellsX - col),
                                static_cast<float>(cellsY - row)});
                            const float height = edge +
                                (sample(col, row) - edge) * fade;
                            auto& vertex = changed[static_cast<size_t>(row) *
                                patchWidth + col];
                            vertex.pos[1] = height;
                            const glm::vec3 normal = glm::normalize(glm::vec3(
                                (sample(col - 1, row) - sample(col + 1, row)) /
                                    (2.f * cellWidth), 1.f,
                                (sample(col, row - 1) - sample(col, row + 1)) /
                                    (2.f * cellDepth)));
                            vertex.normal[0] = normal.x;
                            vertex.normal[1] = normal.y;
                            vertex.normal[2] = normal.z;
                            setColor(vertex, height);
                            if (patch.colors.size() == patchCount * 3)
                                for (int channel = 0; channel < 3; ++channel)
                                    vertex.color[channel] = patch.colors[
                                        (static_cast<size_t>(row) * patchWidth +
                                            col) * 3 + channel];
                        }
                    if (!tile.mesh->UpdateIndexedVertexRange(offset, changed))
                    { canUpdate = false; break; }
                    ++m_farTileUploadCount;
                    m_farTileUploadedBytes += changed.size() * sizeof(Mesh::Vertex);
                }
                if (canUpdate)
                {
                    tile.dirtyPatchIndices.clear();
                    ++uploadsThisUpdate;
                    m_farTileRebuildMilliseconds += std::chrono::duration<double,
                        std::milli>(std::chrono::steady_clock::now() -
                            rebuildStart).count();
                    continue;
                }
            }
            tile.built = false;
        }
        const int width = (tile.endX - tile.startX + tile.lodStride - 1) /
            tile.lodStride + 1;
        const int depth = (tile.endY - tile.startY + tile.lodStride - 1) /
            tile.lodStride + 1;
        const int nearestX = std::max({tile.startX - centerX, 0,
            centerX - (tile.endX - 1)});
        const int nearestY = std::max({tile.startY - centerY, 0,
            centerY - (tile.endY - 1)});
        const bool fullyDistant = std::max(nearestX, nearestY) > nearRadius + 1;
        const bool cacheable = fullyDistant && !m_cacheDirectory.empty();
        std::filesystem::path cachePath;
        uint64_t fingerprint = 14695981039346656037ull;
        if (cacheable)
        {
            HashValue(fingerprint, m_farSourceSignature);
            HashValue(fingerprint, m_cacheSignature);
            HashValue(fingerprint, m_farHalfExtentCells);
            HashValue(fingerprint, cellsX); HashValue(fingerprint, cellsY);
            HashValue(fingerprint, cellWidth); HashValue(fingerprint, cellDepth);
            HashValue(fingerprint, tile.startX); HashValue(fingerprint, tile.startY);
            HashValue(fingerprint, tile.endX); HashValue(fingerprint, tile.endY);
            HashValue(fingerprint, tile.lodStride);
            for (const auto& color : m_farPalette.farColors)
                for (int channel = 0; channel < 3; ++channel)
                    HashValue(fingerprint, color[channel]);
            for (const auto& color : m_farPalette.nearColors)
                for (int channel = 0; channel < 3; ++channel)
                    HashValue(fingerprint, color[channel]);
            for (int layer : m_farPalette.archiveToNear)
                HashValue(fingerprint, layer);
            for (int y = tile.startY - 1; y <= tile.endY + 1; ++y)
                for (int x = tile.startX - 1; x <= tile.endX + 1; ++x)
                {
                    const float height = gridHeight(x, y);
                    HashValue(fingerprint, height);
                }
            for (size_t patchIndex : tile.patchIndices)
            {
                const auto& patch = m_farPatches[patchIndex];
                HashValue(fingerprint, patch.x); HashValue(fingerprint, patch.y);
                for (float height : patch.heights)
                    HashValue(fingerprint, height);
                for (float color : patch.colors)
                    HashValue(fingerprint, color);
            }
            cachePath = std::filesystem::path(m_cacheDirectory) / "far_tiles" /
                "v4" / std::to_string(m_farSourceSignature) /
                (std::to_string(tile.startX) + "_" +
                    std::to_string(tile.startY) + "_" +
                    std::to_string(tile.lodStride) + ".fartile");
        }
        std::vector<Mesh::Vertex> vertices;
        std::vector<uint32_t> indices;
        std::map<size_t, size_t> patchOffsets;
        std::vector<uint8_t> preparedVisibility;
        const bool asyncEligible = m_farHeightsSnapshot &&
            !exposesGap(pending[order]);
        bool asyncPrepared = false;
        bool cacheHit = false;
        if (asyncEligible)
        {
            {
                std::lock_guard<std::mutex> lock(m_farPrepareMutex);
                const auto ready = std::find_if(m_preparedFarTiles.begin(),
                    m_preparedFarTiles.end(), [&](const PreparedFarTile& item) {
                        return item.tileIndex == pending[order] &&
                            item.revision == m_farPrepareRevision;
                    });
                if (ready != m_preparedFarTiles.end() && canUpload)
                {
                    vertices = std::move(ready->vertices);
                    indices = std::move(ready->indices);
                    patchOffsets = std::move(ready->patchVertexOffsets);
                    preparedVisibility = std::move(ready->builtVisibility);
                    cacheHit = ready->cacheHit;
                    m_farTileRebuildMilliseconds += ready->buildMilliseconds;
                    asyncPrepared = ready->success;
                    m_preparedFarTiles.erase(ready);
                    tile.preparing = false;
                }
            }
            if (asyncPrepared && tile.lodStride == 1)
            {
                size_t cellIndex = 0;
                for (int y = tile.startY; y < tile.endY && asyncPrepared; ++y)
                    for (int x = tile.startX; x < tile.endX; ++x)
                        if (cellIndex >= preparedVisibility.size() ||
                            preparedVisibility[cellIndex++] !=
                                static_cast<uint8_t>(visible(x, y,
                                    centerX, centerY)))
                        { asyncPrepared = false; break; }
            }
            if (!asyncPrepared)
            {
                if (!tile.preparing)
                {
                    const size_t preparingCount = static_cast<size_t>(
                        std::count_if(m_farTiles.begin(), m_farTiles.end(),
                            [](const FarTile& candidate) {
                                return candidate.preparing;
                            }));
                    if (preparingCount >= static_cast<size_t>(
                            std::clamp(maxPreparedCells, 1, 64))) continue;
                    const auto usage = GetMemoryUsage();
                    if (usage.cpuBytes >= (static_cast<uint64_t>(
                            std::max(1, cpuBudgetMiB)) << 20) ||
                        usage.gpuBytes >= (static_cast<uint64_t>(
                            std::max(1, gpuBudgetMiB)) << 20)) continue;
                        FarPrepareTask task;
                        task.tileIndex = pending[order];
                        task.revision = m_farPrepareRevision;
                        task.fingerprint = fingerprint;
                        task.startX = tile.startX;
                        task.startY = tile.startY;
                        task.endX = tile.endX;
                        task.endY = tile.endY;
                        task.lodStride = tile.lodStride;
                        task.side = side;
                        task.halfExtent = m_farHalfExtentCells;
                        task.spanX = spanX;
                        task.spanZ = spanZ;
                        task.cellsX = cellsX;
                        task.cellsY = cellsY;
                        task.cellWidth = cellWidth;
                        task.cellDepth = cellDepth;
                        task.palette = m_farPalette;
                        task.heights = m_farHeightsSnapshot;
                        task.cachePath = cachePath;
                        if (tile.lodStride == 1)
                        {
                            for (int y = tile.startY - 1; y <= tile.endY; ++y)
                                for (int x = tile.startX - 1;
                                    x <= tile.endX; ++x)
                                    if (!visible(x, y, centerX, centerY))
                                        task.hiddenCells.insert({x, y});
                            for (int y = tile.startY; y < tile.endY; ++y)
                                for (int x = tile.startX; x < tile.endX; ++x)
                                {
                                    if (task.hiddenCells.count({x, y})) continue;
                                    for (Cell neighbor : {Cell{x - 1, y},
                                        Cell{x, y + 1}, Cell{x + 1, y},
                                        Cell{x, y - 1}})
                                    {
                                        if (!task.hiddenCells.count(neighbor) ||
                                            task.seams.count(neighbor)) continue;
                                        const Terrain* nearTerrain = GetLoadedCell(
                                            neighbor.first, neighbor.second);
                                        if (nearTerrain && nearTerrain->heights.size() ==
                                            static_cast<size_t>(cellsX + 1) *
                                                (cellsY + 1))
                                            task.seams[neighbor] = {nearTerrain->heights,
                                                nearTerrain->weights,
                                                static_cast<uint32_t>(
                                                    nearTerrain->layers.size())};
                                    }
                                }
                            for (size_t patchIndex : tile.patchIndices)
                                task.patches.emplace_back(patchIndex,
                                    m_farPatches[patchIndex]);
                        }
                        uint64_t taskBytes = 0;
                        for (const auto& [cell, surface] : task.seams)
                            taskBytes += (surface.heights.size() +
                                surface.weights.size()) * sizeof(float);
                        for (const auto& [index, patch] : task.patches)
                            taskBytes += (patch.heights.size() +
                                patch.colors.size()) * sizeof(float);
                        const uint64_t cpuLimit = static_cast<uint64_t>(
                            std::max(1, cpuBudgetMiB)) << 20;
                        if (taskBytes > cpuLimit - usage.cpuBytes) continue;
                        StartFarPrepareWorker();
                        {
                            std::lock_guard<std::mutex> lock(m_farPrepareMutex);
                            m_farPrepareQueue.push_back(std::move(task));
                        }
                        tile.preparing = true;
                        m_farPrepareWake.notify_one();
                }
                continue;
            }
        }
        else
        {
            if (!canUpload) continue;
            if (tile.preparing) tile.preparing = false;
            cacheHit = cacheable && ReadFarTileCache(cachePath,
                fingerprint, vertices, indices);
        }
        if (!cacheHit && !asyncPrepared)
        {
        vertices.resize(static_cast<size_t>(width) * depth);
        for (int row = 0; row < depth; ++row)
            for (int col = 0; col < width; ++col)
            {
                const int x = std::min(tile.startX + col * tile.lodStride,
                    tile.endX);
                const int y = std::min(tile.startY + row * tile.lodStride,
                    tile.endY);
                auto& vertex = vertices[static_cast<size_t>(row) * width + col];
                const float height = gridHeight(x, y);
                vertex.pos[0] = (x - tile.startX) * spanX;
                vertex.pos[1] = height;
                vertex.pos[2] = (y - tile.startY) * spanZ;
                const glm::vec3 normal = glm::normalize(glm::vec3(
                    (gridHeight(x - 1, y) - gridHeight(x + 1, y)) /
                        (2.f * spanX), 1.f,
                    (gridHeight(x, y - 1) - gridHeight(x, y + 1)) /
                        (2.f * spanZ)));
                vertex.normal[0] = normal.x;
                vertex.normal[1] = normal.y;
                vertex.normal[2] = normal.z;
                vertex.uv[0] = static_cast<float>(x + m_farHalfExtentCells) /
                    (side - 1);
                vertex.uv[1] = static_cast<float>(y + m_farHalfExtentCells) /
                    (side - 1);
                setColor(vertex, height);
            }
        if (tile.lodStride > 1)
        {
            for (int row = 0; row < depth - 1; ++row)
                for (int col = 0; col < width - 1; ++col)
                {
                    const uint32_t a = row * width + col;
                    const uint32_t b = a + width;
                    indices.insert(indices.end(),
                        {a, b, a + 1, a + 1, b, b + 1});
                }
        }
        else for (int y = tile.startY; y < tile.endY; ++y)
            for (int x = tile.startX; x < tile.endX; ++x)
            {
                if (!visible(x, y, centerX, centerY) ||
                    m_farPatchCells.count({x, y})) continue;
                const int seam = seamSide(x, y, centerX, centerY);
                const SeamSurface* nearSurface = seam == 0
                    ? seamSurface(x - 1, y) : seam == 1
                    ? seamSurface(x, y + 1) : seam == 2
                    ? seamSurface(x + 1, y) : seam == 3
                    ? seamSurface(x, y - 1) : nullptr;
                const std::vector<float>* nearHeights = nearSurface
                    ? nearSurface->heights : nullptr;
                if (nearHeights)
                {
                    const float topLeft = gridHeight(x, y);
                    const float topRight = gridHeight(x + 1, y);
                    const float bottomLeft = gridHeight(x, y + 1);
                    const float bottomRight = gridHeight(x + 1, y + 1);
                    const auto addVertex = [&](float u, float v) {
                        Mesh::Vertex vertex{};
                        float height = (1.f - u) * (1.f - v) * topLeft +
                            u * (1.f - v) * topRight +
                            (1.f - u) * v * bottomLeft +
                            u * v * bottomRight;
                        const int col = static_cast<int>(std::round(u * cellsX));
                        const int row = static_cast<int>(std::round(v * cellsY));
                        if (seam == 0 && u == 0.f)
                            height = (*nearHeights)[static_cast<size_t>(
                                row * (cellsX + 1) + cellsX)];
                        else if (seam == 1 && v == 1.f)
                            height = (*nearHeights)[static_cast<size_t>(col)];
                        else if (seam == 2 && u == 1.f)
                            height = (*nearHeights)[static_cast<size_t>(
                                row * (cellsX + 1))];
                        else if (seam == 3 && v == 0.f)
                            height = (*nearHeights)[static_cast<size_t>(
                                cellsY * (cellsX + 1) + col)];
                        vertex.pos[0] = (x - tile.startX + u) * spanX;
                        vertex.pos[1] = height;
                        vertex.pos[2] = (y - tile.startY + v) * spanZ;
                        const float dx = ((1.f - v) * (topRight - topLeft) +
                            v * (bottomRight - bottomLeft)) / spanX;
                        const float dz = ((1.f - u) * (bottomLeft - topLeft) +
                            u * (bottomRight - topRight)) / spanZ;
                        const glm::vec3 normal = seamNormal(nearHeights,
                            seam, col, row).value_or(glm::normalize(
                                glm::vec3(-dx, 1.f, -dz)));
                        vertex.normal[0] = normal.x;
                        vertex.normal[1] = normal.y;
                        vertex.normal[2] = normal.z;
                        vertex.uv[0] = static_cast<float>(x + u + m_farHalfExtentCells) /
                            (side - 1);
                        vertex.uv[1] = static_cast<float>(y + v + m_farHalfExtentCells) /
                            (side - 1);
                        setColor(vertex, height);
                        blendSeamColor(vertex, nearSurface, seam, col, row,
                            u, v);
                        const uint32_t index = static_cast<uint32_t>(vertices.size());
                        vertices.push_back(vertex);
                        return index;
                    };
                    const uint32_t center = addVertex(.5f, .5f);
                    std::vector<uint32_t> boundary;
                    const int leftSteps = seam == 0 ? cellsY : 1;
                    const int bottomSteps = seam == 1 ? cellsX : 1;
                    const int rightSteps = seam == 2 ? cellsY : 1;
                    const int topSteps = seam == 3 ? cellsX : 1;
                    for (int i = 0; i < leftSteps; ++i)
                        boundary.push_back(addVertex(0.f,
                            static_cast<float>(i) / leftSteps));
                    for (int i = 0; i < bottomSteps; ++i)
                        boundary.push_back(addVertex(
                            static_cast<float>(i) / bottomSteps, 1.f));
                    for (int i = 0; i < rightSteps; ++i)
                        boundary.push_back(addVertex(1.f,
                            1.f - static_cast<float>(i) / rightSteps));
                    for (int i = 0; i < topSteps; ++i)
                        boundary.push_back(addVertex(
                            1.f - static_cast<float>(i) / topSteps, 0.f));
                    for (size_t i = 0; i < boundary.size(); ++i)
                        indices.insert(indices.end(), {center, boundary[i],
                            boundary[(i + 1) % boundary.size()]});
                    continue;
                }
                const uint32_t a = static_cast<uint32_t>(
                    (y - tile.startY) * width + x - tile.startX);
                const uint32_t b = a + width;
                indices.insert(indices.end(), {a, b, a + 1, a + 1, b, b + 1});
            }
        // A vertical skirt closes the height difference where a stride-two
        // parent borders a stride-one child. It uses the same X/Z edge as the
        // tile, so either side can change LOD without rebuilding its neighbor.
        if (!indices.empty())
        {
            float minimumHeight = std::numeric_limits<float>::max();
            float maximumHeight = std::numeric_limits<float>::lowest();
            const size_t baseVertexCount = static_cast<size_t>(width) * depth;
            for (size_t index = 0; index < baseVertexCount; ++index)
            {
                minimumHeight = std::min(minimumHeight, vertices[index].pos[1]);
                maximumHeight = std::max(maximumHeight, vertices[index].pos[1]);
            }
            const float skirtBottom = minimumHeight -
                std::max(2.f, maximumHeight - minimumHeight);
            const auto addSkirtEdge = [&](uint32_t first, uint32_t second) {
                Mesh::Vertex lowFirst = vertices[first];
                Mesh::Vertex lowSecond = vertices[second];
                lowFirst.pos[1] = skirtBottom;
                lowSecond.pos[1] = skirtBottom;
                const uint32_t lowIndex = static_cast<uint32_t>(vertices.size());
                vertices.push_back(lowFirst);
                vertices.push_back(lowSecond);
                indices.insert(indices.end(), {first, lowIndex, second,
                    second, lowIndex, lowIndex + 1,
                    second, lowIndex, first,
                    lowIndex + 1, lowIndex, second});
            };
            for (int col = 0; col < width - 1; ++col)
            {
                addSkirtEdge(col, col + 1);
                addSkirtEdge((depth - 1) * width + col,
                    (depth - 1) * width + col + 1);
            }
            for (int row = 0; row < depth - 1; ++row)
            {
                addSkirtEdge(row * width, (row + 1) * width);
                addSkirtEdge(row * width + width - 1,
                    (row + 1) * width + width - 1);
            }
        }
        for (size_t patchIndex : tile.patchIndices)
        {
            const auto& patch = m_farPatches[patchIndex];
            if (patch.x < tile.startX || patch.x >= tile.endX ||
                patch.y < tile.startY || patch.y >= tile.endY ||
                !visible(patch.x, patch.y, centerX, centerY)) continue;
            const int patchWidth = cellsX + 1;
            const int patchDepth = cellsY + 1;
            if (patch.heights.size() !=
                static_cast<size_t>(patchWidth) * patchDepth) continue;
            const int seam = seamSide(patch.x, patch.y, centerX, centerY);
            const SeamSurface* nearSurface = seam == 0
                ? seamSurface(patch.x - 1, patch.y) : seam == 1
                ? seamSurface(patch.x, patch.y + 1) : seam == 2
                ? seamSurface(patch.x + 1, patch.y) : seam == 3
                ? seamSurface(patch.x, patch.y - 1) : nullptr;
            const std::vector<float>* nearHeights = nearSurface
                ? nearSurface->heights : nullptr;
            const float topLeft = gridHeight(patch.x, patch.y);
            const float topRight = gridHeight(patch.x + 1, patch.y);
            const float bottomLeft = gridHeight(patch.x, patch.y + 1);
            const float bottomRight = gridHeight(patch.x + 1, patch.y + 1);
            const uint32_t base = static_cast<uint32_t>(vertices.size());
            patchOffsets[patchIndex] = base;
            for (int row = 0; row < patchDepth; ++row)
                for (int col = 0; col < patchWidth; ++col)
                {
                    const float u = static_cast<float>(col) / cellsX;
                    const float v = static_cast<float>(row) / cellsY;
                    const float edge = (1.f - u) * (1.f - v) * topLeft +
                        u * (1.f - v) * topRight +
                        (1.f - u) * v * bottomLeft + u * v * bottomRight;
                    const auto sample = [&](int sx, int sy) {
                        return patch.heights[static_cast<size_t>(
                            std::clamp(sy, 0, patchDepth - 1)) * patchWidth +
                            std::clamp(sx, 0, patchWidth - 1)];
                    };
                    const float borderFade = std::min({1.f,
                        static_cast<float>(col), static_cast<float>(row),
                        static_cast<float>(patchWidth - 1 - col),
                        static_cast<float>(patchDepth - 1 - row)});
                    float height = edge +
                        (sample(col, row) - edge) * borderFade;
                    if (nearHeights)
                    {
                        if (seam == 0 && col == 0)
                            height = (*nearHeights)[static_cast<size_t>(
                                row * patchWidth + cellsX)];
                        else if (seam == 1 && row == cellsY)
                            height = (*nearHeights)[static_cast<size_t>(col)];
                        else if (seam == 2 && col == cellsX)
                            height = (*nearHeights)[static_cast<size_t>(
                                row * patchWidth)];
                        else if (seam == 3 && row == 0)
                            height = (*nearHeights)[static_cast<size_t>(
                                cellsY * patchWidth + col)];
                    }
                    Mesh::Vertex vertex{};
                    vertex.pos[0] = (patch.x - tile.startX) * spanX +
                        col * cellWidth;
                    vertex.pos[1] = height;
                    vertex.pos[2] = (patch.y - tile.startY) * spanZ +
                        row * cellDepth;
                    const glm::vec3 coarseNormal = glm::normalize(glm::vec3(
                        (sample(col - 1, row) - sample(col + 1, row)) /
                            (2.f * cellWidth), 1.f,
                        (sample(col, row - 1) - sample(col, row + 1)) /
                            (2.f * cellDepth)));
                    const glm::vec3 normal = seamNormal(nearHeights,
                        seam, col, row).value_or(coarseNormal);
                    vertex.normal[0] = normal.x;
                    vertex.normal[1] = normal.y;
                    vertex.normal[2] = normal.z;
                    vertex.uv[0] = u; vertex.uv[1] = v;
                    setColor(vertex, height);
                    if (patch.colors.size() == patch.heights.size() * 3)
                        for (int channel = 0; channel < 3; ++channel)
                            vertex.color[channel] = patch.colors[
                                (static_cast<size_t>(row) * patchWidth + col) *
                                    3 + channel];
                    blendSeamColor(vertex, nearSurface, seam, col, row, u, v);
                    vertices.push_back(vertex);
                }
            for (int row = 0; row < cellsY; ++row)
                for (int col = 0; col < cellsX; ++col)
                {
                    const uint32_t a = base + row * patchWidth + col;
                    const uint32_t b = a + patchWidth;
                    indices.insert(indices.end(),
                        {a, b, a + 1, a + 1, b, b + 1});
                }
        }
        if (cacheable && !asyncPrepared && !indices.empty())
            WriteFarTileCache(cachePath, fingerprint, vertices, indices);
        }
        const auto recordVisibility = [&] {
            if (tile.lodStride > 1) return;
            tile.builtVisibility.clear();
            tile.builtVisibility.reserve(static_cast<size_t>(
                (tile.endX - tile.startX) * (tile.endY - tile.startY)));
            for (int y = tile.startY; y < tile.endY; ++y)
                for (int x = tile.startX; x < tile.endX; ++x)
                    tile.builtVisibility.push_back(static_cast<uint8_t>(
                        visible(x, y, centerX, centerY)));
        };
        if (indices.empty())
        {
            if (tile.object) tile.object->enabled = false;
            tile.hasGeometry = false;
            tile.built = true;
            recordVisibility();
            tile.patchVertexOffsets.clear();
            tile.dirtyPatchIndices.clear();
            tile.builtCenterX = centerX;
            tile.builtCenterY = centerY;
            tile.builtNearRadius = nearRadius;
            m_farTileRebuildMilliseconds += std::chrono::duration<double,
                std::milli>(std::chrono::steady_clock::now() - rebuildStart).count();
            ++uploadsThisUpdate;
            continue;
        }
        const uint64_t uploadBytes =
            vertices.size() * sizeof(Mesh::Vertex) +
            indices.size() * sizeof(uint32_t);
        const auto memory = GetMemoryUsage();
        const uint64_t previousGpuBytes = tile.mesh
            ? tile.mesh->GetGpuBufferMemoryBytes() : 0;
        const uint64_t gpuLimit = static_cast<uint64_t>(
            std::max(1, gpuBudgetMiB)) << 20;
        if (!exposesGap(pending[order]) &&
            memory.gpuBytes - std::min(memory.gpuBytes, previousGpuBytes) +
                uploadBytes > gpuLimit)
        {
            if (asyncPrepared)
            {
                PreparedFarTile deferred;
                deferred.tileIndex = pending[order];
                deferred.revision = m_farPrepareRevision;
                deferred.success = true;
                deferred.cacheHit = cacheHit;
                deferred.vertices = std::move(vertices);
                deferred.indices = std::move(indices);
                deferred.patchVertexOffsets = std::move(patchOffsets);
                deferred.builtVisibility = std::move(preparedVisibility);
                std::lock_guard<std::mutex> lock(m_farPrepareMutex);
                m_preparedFarTiles.push_back(std::move(deferred));
                tile.preparing = true;
            }
            continue;
        }
        if (cacheHit) ++m_farTileCacheHits;
        if (!tile.object)
        {
            tile.object = scene->AddObject("Distant terrain tile " +
                std::to_string(tile.startX) + "," +
                std::to_string(tile.startY));
            tile.object->runtimeOnly = true;
            scene->MoveObject(tile.object, Owner,
                Engine::Scene::Scene::ObjectPlacement::AsChild);
            tile.object->transform.position = {
                static_cast<double>(tile.startX) * spanX, 0.0,
                static_cast<double>(tile.startY) * spanZ};
            tile.mesh = tile.object->AddComponent<Mesh>();
            auto* material = tile.object->AddComponent<
                Material>();
            material->diffuseColor = {1.f, 1.f, 1.f};
            material->unlit = true;
            material->castsShadows = false;
            tile.object->AddComponent<
                TerrainBatchAppearance>()->distantVisual = true;
        }
        if (!tile.mesh->SetIndexedGeometry(
            std::move(vertices), std::move(indices)))
            throw std::runtime_error("Could not build distant terrain tile");
        if (auto* provider = scene->GetGraphicsProvider())
            if (!tile.mesh->IsReady()) tile.mesh->OnAfterDeserialize(provider);
        tile.object->enabled = true;
        tile.hasGeometry = true;
        tile.built = true;
        recordVisibility();
        tile.patchVertexOffsets = std::move(patchOffsets);
        tile.dirtyPatchIndices.clear();
        tile.builtCenterX = centerX;
        tile.builtCenterY = centerY;
        tile.builtNearRadius = nearRadius;
        ++m_farTileUploadCount;
        ++uploadsThisUpdate;
        m_farTileUploadedBytes += uploadBytes;
        m_farTileRebuildMilliseconds += std::chrono::duration<double,
            std::milli>(std::chrono::steady_clock::now() - rebuildStart).count();
    }
    // Switch every level atomically. A coarse tile stays visible until all
    // descendants covering its area have their replacement geometry ready.
    std::function<void(size_t)> hideBranch = [&](size_t index) {
        auto& tile = m_farTiles[index];
        if (tile.object) tile.object->enabled = false;
        for (size_t child : tile.children) hideBranch(child);
    };
    std::function<bool(size_t)> showBranch = [&](size_t index) {
        auto& tile = m_farTiles[index];
        if (tile.outOfRange)
        {
            hideBranch(index);
            return true;
        }
        const bool ready = tile.built && tile.hasGeometry &&
            tile.dirtyPatchIndices.empty() && !needsRebuild(tile);
        if (tile.children.empty())
        {
            if (tile.object) tile.object->enabled = ready;
            return ready;
        }
        if (tile.desired && ready)
        {
            if (tile.object) tile.object->enabled = true;
            for (size_t child : tile.children) hideBranch(child);
            return true;
        }
        bool childrenReady = true;
        for (size_t child : tile.children)
            childrenReady = showBranch(child) && childrenReady;
        if (childrenReady)
        {
            if (tile.object) tile.object->enabled = false;
            return true;
        }
        if (tile.object) tile.object->enabled = tile.hasGeometry;
        if (tile.hasGeometry)
        {
            for (size_t child : tile.children) hideBranch(child);
            return true;
        }
        return false;
    };
    for (size_t index = 0; index < m_farTiles.size(); ++index)
        if (m_farTiles[index].parentIndex == static_cast<size_t>(-1))
            showBranch(index);
    // Keep only the active representation resident. The other level can be
    // reloaded from its versioned cache when the camera requests it again.
    for (auto& tile : m_farTiles)
        if (tile.object && !tile.object->enabled && !tile.desired)
        {
            scene->RemoveObject(tile.object);
            tile.object = nullptr;
            tile.mesh = nullptr;
            tile.built = false;
            tile.hasGeometry = false;
            tile.patchVertexOffsets.clear();
            tile.builtVisibility.clear();
        }
    if (scene->HasStarted()) m_farFrameUploads += uploadsThisUpdate;
}

}

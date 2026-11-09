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
    FenceBackgroundWork();
}

void TerrainManager::FenceBackgroundWork()
{
    {
        std::lock_guard<std::mutex> lock(m_farPrepareMutex);
        m_stopFarWorker = true;
        m_farPrepareQueue.clear();
    }
    m_farPrepareWake.notify_one();
    if (m_farWorker.joinable()) m_farWorker.join();
    {
        std::lock_guard<std::mutex> lock(m_farPrepareMutex);
        m_preparedFarTiles.clear();
        m_stopFarWorker = false;
        ++m_farPrepareRevision;
    }
    {
        std::lock_guard<std::mutex> lock(m_prepareMutex);
        m_stopPrepareWorker = true;
        ++m_prepareRevision;
        m_prepareQueue.clear();
    }
    m_prepareWake.notify_one();
    if (m_prepareWorker.joinable()) m_prepareWorker.join();
    {
        std::lock_guard<std::mutex> lock(m_prepareMutex);
        m_preparedCells.clear();
        m_preparingCells.clear();
        m_stopPrepareWorker = false;
        ++m_prepareRevision;
    }
}

bool TerrainManager::HasCell(int x, int y) const
{
    return m_cells.count({x, y}) != 0 || m_proceduralCells.count({x, y}) != 0;
}

void TerrainManager::SetProceduralBuilder(std::function<void(Cell, Terrain&)> builder)
{
    InvalidatePrepared();
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
        result.sceneGeneration = task.sceneGeneration;
        const auto cancelled = [&] {
            return m_stopPrepareWorker.load(std::memory_order_acquire) ||
                task.revision != m_prepareRevision.load(std::memory_order_acquire) ||
                (task.scene && task.scene->CaptureSceneGeneration() !=
                    task.sceneGeneration);
        };
        if (cancelled()) continue;
        try
        {
            Engine::Core::Object object;
            auto* terrain = object.AddComponent<Terrain>();
            if (task.prepared)
            {
                result.cacheHit = task.preparedCacheHit;
                if (!terrain->InstallPrepared(std::move(*task.prepared), nullptr))
                    throw std::runtime_error("Invalid prepared terrain cell");
            }
            else
            {
                const bool baked = !task.bakePath.empty() && task.bakeSize &&
                    terrain->LoadCacheRange(task.bakePath,
                        task.cacheSignature, nullptr, task.bakeOffset,
                        task.bakeSize);
                if (task.requireBake && !baked)
                    throw std::runtime_error("Missing or stale runtime terrain bake");
                if (cancelled()) continue;
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
            }
            if (cancelled()) continue;
            terrain->SetMeshLodStride(task.lodStride);
            result.terrain = terrain->TakePrepared();
            result.terrain.bounds = Mesh::PrepareIndexedGeometry(
                result.terrain.vertices, result.terrain.indices);
            if (!result.terrain.bounds)
                throw std::runtime_error("Invalid prepared terrain bounds");
            result.objectLayers = std::move(task.preparedLayers);
            for (auto& [name, saved] : task.objectLayers)
            {
                if (cancelled()) break;
                auto layer = std::make_unique<TerrainObjectLayer>();
                if (!saved.IsNull()) layer->Deserialize(saved);
                layer->layerName = name;
                if (!layer->HasLoadedPreparedData()) layer->GetPreparedTiles(4.f);
                result.objectLayers.push_back(std::move(layer));
            }
        }
        catch (...) { result.terrain.vertices.clear(); }
        {
            std::lock_guard<std::mutex> lock(m_prepareMutex);
            if (!cancelled() && task.revision == m_prepareRevision)
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
        if (!Owner || !Owner->GetScene() ||
            Owner->GetScene()->CaptureSceneGeneration() !=
                prepared.sceneGeneration ||
            !HasCell(cell.first, cell.second) ||
            m_loaded.count(cell) ||
            std::abs(cell.first - centerX) > radius ||
            std::abs(cell.second - centerY) > radius ||
            prepared.terrain.vertices.empty()) continue;
        int desiredStride = DesiredLodStride(cell, centerX, centerY);
        while (desiredStride > 1 &&
            (prepared.terrain.cellsX < desiredStride ||
                prepared.terrain.cellsY < desiredStride ||
                prepared.terrain.cellsX % desiredStride ||
                prepared.terrain.cellsY % desiredStride))
            desiredStride /= 2;
        if (prepared.terrain.lodStride != desiredStride)
        {
            PrepareTask task;
            task.cell = cell;
            task.revision = m_prepareRevision;
            task.scene = Owner->GetScene();
            task.sceneGeneration = prepared.sceneGeneration;
            task.lodStride = desiredStride;
            task.prepared = std::move(prepared.terrain);
            task.preparedCacheHit = prepared.cacheHit;
            task.preparedLayers = std::move(prepared.objectLayers);
            {
                std::lock_guard<std::mutex> lock(m_prepareMutex);
                m_prepareQueue.push_front(std::move(task));
            }
            m_preparingCells[cell] = m_prepareRevision;
            m_prepareWake.notify_one();
            continue;
        }
        const auto commitStart = std::chrono::steady_clock::now();
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
        for (auto& preparedLayer : prepared.objectLayers)
        {
            auto* child = Owner->GetScene()->AddObject(preparedLayer->layerName);
            child->runtimeOnly = true;
            Owner->GetScene()->MoveObject(child, object,
                Engine::Scene::Scene::ObjectPlacement::AsChild);
            child->AddComponent<TerrainObjectLayer>()->InstallPrepared(
                std::move(*preparedLayer));
        }
        const auto sceneInstalled = std::chrono::steady_clock::now();
        Stitch(cell);
        const auto stitched = std::chrono::steady_clock::now();
        const auto lodInstalled = std::chrono::steady_clock::now();
        terrain->FinalizePreparedGraphics(m_provider);
        const auto graphicsReady = std::chrono::steady_clock::now();
        ConfigureCellPhysics(*object);
        const auto physicsReady = std::chrono::steady_clock::now();
        const auto elapsed = [](auto begin, auto end) {
            return std::chrono::duration<double, std::milli>(end - begin).count();
        };
        m_lastStreamingTiming.commitSceneMs +=
            elapsed(commitStart, sceneInstalled);
        m_lastStreamingTiming.commitStitchMs +=
            elapsed(sceneInstalled, stitched);
        m_lastStreamingTiming.commitLodMs +=
            elapsed(stitched, lodInstalled);
        m_lastStreamingTiming.commitGraphicsMs +=
            elapsed(lodInstalled, graphicsReady);
        m_lastStreamingTiming.commitPhysicsMs +=
            elapsed(graphicsReady, physicsReady);
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
    RestoreObjectLayers(cell, *object);
    Stitch(cell);
    terrain->SetMeshLodStride(lodStride);
    ConfigureCellPhysics(*object);
}

int TerrainManager::DesiredLodStride(Cell cell, int centerX, int centerY) const
{
    if (m_editorStreaming) return 1;
    const int distance = std::max(std::abs(cell.first - centerX),
        std::abs(cell.second - centerY));
    int stride = distance <= loadedRadius ? 1 :
        distance <= lod2Radius ? 2 : distance <= lod4Radius ? 4 : 8;
    // Match Terrain::SetMeshLodStride: unsupported strides otherwise consume
    // the LOD/upload budget every frame without ever changing the mesh.
    while (stride > 1 && (cellsX < stride || cellsY < stride ||
        cellsX % stride || cellsY % stride)) stride /= 2;
    return stride;
}

int TerrainManager::DesiredPrepareLodStride(Cell cell, int centerX,
    int centerY) const
{
    if (m_prefetchCenter &&
        (std::abs(cell.first - centerX) > loadedRadius ||
            std::abs(cell.second - centerY) > loadedRadius))
        return DesiredLodStride(cell, m_prefetchCenter->first,
            m_prefetchCenter->second);
    return DesiredLodStride(cell, centerX, centerY);
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
    if (object) CaptureObjectLayers(cell, *object);
    m_loaded.erase(found);
    if (object && object->GetScene()) object->GetScene()->RemoveObject(object);
}


}


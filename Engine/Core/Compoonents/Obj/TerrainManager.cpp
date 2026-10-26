#include "TerrainManager.h"
#include "Core/Object.h"
#include "Core/Scene/Scene.h"
#include "Core/Compoonents/Obj/TerrainBatchAppearance.h"
#include "Engine/Editor/UI/IEditorUi.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <type_traits>
#include <chrono>
#include <cstdint>
#include <limits>

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
        if (!terrain) continue;
        usage.cpuBytes += terrain->heights.capacity() * sizeof(float) +
            terrain->weights.capacity() * sizeof(float) +
            terrain->layers.capacity() * sizeof(Terrain::Layer);
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
    std::lock_guard<std::mutex> lock(m_prepareMutex);
    for (const auto& prepared : m_preparedCells)
    {
        const auto& data = prepared.terrain;
        usage.preparedBytes += data.heights.capacity() * sizeof(float) +
            data.weights.capacity() * sizeof(float) +
            data.vertices.capacity() * sizeof(Mesh::Vertex) +
            data.indices.capacity() * sizeof(uint32_t) +
            data.layers.capacity() * sizeof(Terrain::Layer);
    }
    usage.cpuBytes += usage.preparedBytes;
    return usage;
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
            if (task.hasSaved) terrain->Deserialize(task.saved);
            else
            {
                result.cacheHit = !task.cachePath.empty() &&
                    terrain->LoadCache(task.cachePath, task.cacheSignature, nullptr);
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
            if (task.edited && !task.editPath.empty())
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
            prepared = std::move(m_preparedCells.front());
            m_preparedCells.pop_front();
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
    if (saved == m_cells.end() && !m_proceduralBuilder) return;
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
    bool ready = false;
    if (saved != m_cells.end()) terrain->Deserialize(saved->second);
    else
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
    if (m_editedProcedural.count(cell)) LoadEdit(cell, *terrain);
    m_loaded[cell] = terrain;
    Stitch(cell);
    terrain->SetMeshLodStride(lodStride);
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
    MarkConfigurationDirty();
    return true;
}

void TerrainManager::SyncLoadedCells(int centerX, int centerY)
{
    const int radius = m_editorStreaming ? std::clamp(editorRadius, 0, 32) :
        std::clamp(std::max(loadedRadius, farRadius), 0, 32);
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
    RefreshRenderBatches(64, centerX, centerY, radius);
    TrimMemoryBudget(centerX, centerY);
}

void TerrainManager::RefreshRenderBatches(int uploadBudget, int centerX,
    int centerY, int radius)
{
    if (!Owner || !Owner->GetScene()) return;
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
        if (!terrain || terrain->GetMeshLodStride() < 2) continue;
        auto* mesh = terrain->Owner->GetComponent<Mesh>();
        if (mesh && mesh->IsReady() && !mesh->GetVertices().empty())
            groups[groupOf(cell)].emplace_back(cell, terrain);
    }
    const auto revisionOf = [](const Members& members) {
        uint64_t revision = 1469598103934665603ull;
        for (const auto& [cell, terrain] : members)
        {
            const auto* mesh = terrain->Owner->GetComponent<Mesh>();
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
                if (it->second.members.count(member.first))
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
        if (uploadBudget <= 0) break;
        if (members.size() < 2 || m_renderBatches.count(group)) continue;
        const Terrain* first = members.front().second;
        const auto sameAppearance = [&](const Terrain* terrain) {
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
        std::vector<Mesh::Vertex> vertices;
        std::vector<uint32_t> indices;
        for (const auto& [cell, terrain] : members)
        {
            const auto* mesh = terrain->Owner->GetComponent<Mesh>();
            const glm::mat4 world = terrain->Owner->transform.GetWorldMatrix();
            const glm::mat3 normalMatrix = glm::transpose(glm::inverse(glm::mat3(world)));
            const uint32_t base = static_cast<uint32_t>(vertices.size());
            for (auto vertex : mesh->GetVertices())
            {
                const glm::vec3 position = glm::vec3(world * glm::vec4(
                    vertex.pos[0], vertex.pos[1], vertex.pos[2], 1.f));
                const glm::vec3 normal = glm::normalize(normalMatrix * glm::vec3(
                    vertex.normal[0], vertex.normal[1], vertex.normal[2]));
                vertex.pos[0] = position.x; vertex.pos[1] = position.y;
                vertex.pos[2] = position.z;
                vertex.normal[0] = normal.x; vertex.normal[1] = normal.y;
                vertex.normal[2] = normal.z;
                vertices.push_back(vertex);
            }
            for (uint32_t index : mesh->GetIndices()) indices.push_back(base + index);
        }
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
        RenderBatch batch;
        batch.object = object;
        batch.revision = revisionOf(members);
        for (const auto& [cell, terrain] : members)
        {
            batch.members.insert(cell);
            terrain->Owner->GetComponent<Mesh>()->SetEditorVisible(false);
        }
        m_renderBatches[group] = std::move(batch);
        ++m_renderBatchUploadCount;
        --uploadBudget;
    }
}

void TerrainManager::UpdateStreaming(int centerX, int centerY)
{
    if (m_editorStreaming) InvalidatePrepared();
    m_editorStreaming = false;
    StreamCells(centerX, centerY);
}

void TerrainManager::UpdateEditorStreaming(const glm::dvec3& worldPosition)
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
    StreamCells(static_cast<int>(std::floor(local.x / spanX)),
        static_cast<int>(std::floor(local.z / spanZ)));
}

void TerrainManager::StreamCells(int centerX, int centerY)
{
    const int radius = m_editorStreaming ? std::clamp(editorRadius, 0, 32) :
        std::clamp(std::max(loadedRadius, farRadius), 0, 32);
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
            if (std::abs(cell.first - centerX) > radius ||
                std::abs(cell.second - centerY) > radius ||
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
        if (std::abs(cell.first - centerX) > radius ||
            std::abs(cell.second - centerY) > radius)
        {
            UnloadCell(cell);
            ++unloaded;
        }
    }
    std::vector<Cell> pending;
    for (int y = centerY - radius; y <= centerY + radius; ++y)
        for (int x = centerX - radius; x <= centerX + radius; ++x)
            if (HasCell(x, y) && !GetLoadedCell(x, y) &&
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
            if (!task.hasSaved && !task.builder) continue;
            task.cachePath = GetCacheFilePath(cell.first, cell.second);
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
        if (terrain->GetMeshLodStride() != DesiredLodStride(cell, centerX, centerY))
            lodPending.push_back(cell);
    std::sort(lodPending.begin(), lodPending.end(), [&](Cell a, Cell b) {
        const int strideA = DesiredLodStride(a, centerX, centerY);
        const int strideB = DesiredLodStride(b, centerX, centerY);
        const bool upgradeA = strideA < m_loaded.at(a)->GetMeshLodStride();
        const bool upgradeB = strideB < m_loaded.at(b)->GetMeshLodStride();
        if (upgradeA != upgradeB) return upgradeA;
        const int distanceA = std::max(std::abs(a.first - centerX), std::abs(a.second - centerY));
        const int distanceB = std::max(std::abs(b.first - centerX), std::abs(b.second - centerY));
        return distanceA == distanceB ? a < b : distanceA < distanceB;
    });
    int changed = 0;
    for (Cell cell : lodPending)
    {
        const int lodLimit = asyncStreaming
            ? std::min(std::clamp(maxLodChangesPerUpdate, 1, 64),
                std::clamp(maxUploadsPerUpdate, 1, 64))
            : std::clamp(maxLodChangesPerUpdate, 1, 64);
        if (changed >= lodLimit) break;
        m_loaded.at(cell)->SetMeshLodStride(DesiredLodStride(cell, centerX, centerY));
        ++changed;
    }
    int batchUploads = 8;
    if (asyncStreaming)
    {
        batchUploads = std::max(0, std::clamp(maxUploadsPerUpdate, 1, 64) - changed);
        batchUploads -= CommitPrepared(centerX, centerY, radius, batchUploads);
    }
    RefreshRenderBatches(batchUploads, centerX, centerY, radius);
    TrimMemoryBudget(centerX, centerY);
}

void TerrainManager::Update()
{
    if (!Owner || viewerObjectName.empty()) return;
    auto* viewer = Owner->FindObjectInSceneByName(viewerObjectName);
    if (!viewer) return;
    const glm::dvec3 local = glm::dvec3(
        glm::inverse(Owner->transform.GetWorldMatrixD()) *
        glm::dvec4(viewer->transform.GetWorldPositionD(), 1.0));
    UpdateStreaming(static_cast<int>(std::floor(local.x / (cellsX * cellWidth))),
        static_cast<int>(std::floor(local.z / (cellsY * cellDepth))));
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
        .Set("batchFarCells", JsonValue(batchFarCells))
        .Set("fastRuntimeLighting", JsonValue(fastRuntimeLighting))
        .Set("measurementGrid", JsonValue(measurementGrid))
        .Set("cpuBudgetMiB", JsonValue(cpuBudgetMiB))
        .Set("gpuBudgetMiB", JsonValue(gpuBudgetMiB))
        .Set("cacheBudgetMiB", JsonValue(cacheBudgetMiB))
        .Set("viewerObjectName", JsonValue(viewerObjectName))
        .Set("cellDataDirectory", JsonValue(cellDataDirectory));
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
    farRadius = value.Has("farRadius") ? std::clamp(value["farRadius"].AsInt(), -1, 32) : -1;
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
    batchFarCells = !value.Has("batchFarCells") || value["batchFarCells"].AsBool();
    fastRuntimeLighting = value.Has("fastRuntimeLighting") &&
        value["fastRuntimeLighting"].AsBool();
    measurementGrid = value.Has("measurementGrid") &&
        value["measurementGrid"].AsBool();
    cpuBudgetMiB = value.Has("cpuBudgetMiB")
        ? std::clamp(value["cpuBudgetMiB"].AsInt(), 1, 16384) : 512;
    gpuBudgetMiB = value.Has("gpuBudgetMiB")
        ? std::clamp(value["gpuBudgetMiB"].AsInt(), 1, 16384) : 256;
    cacheBudgetMiB = value.Has("cacheBudgetMiB")
        ? std::clamp(value["cacheBudgetMiB"].AsInt(), 1, 16384) : 1024;
    viewerObjectName = value["viewerObjectName"].AsString();
    cellDataDirectory = value.Has("cellDataDirectory")
        ? value["cellDataDirectory"].AsString() : std::string{};
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
    changed |= ui.SliderInt("Loaded radius", &loadedRadius, 0, 32);
    changed |= ui.SliderInt("Editor radius", &editorRadius, 0, 32);
    changed |= ui.SliderInt("Far radius", &farRadius, -1, 32);
    changed |= ui.SliderInt("LOD 2 radius", &lod2Radius, 0, 32);
    changed |= ui.SliderInt("LOD 4 radius", &lod4Radius, 0, 32);
    changed |= ui.SliderInt("LOD changes per update", &maxLodChangesPerUpdate, 1, 64);
    changed |= ui.SliderInt("Loads per update", &maxLoadsPerUpdate, 1, 64);
    changed |= ui.SliderInt("Unloads per update", &maxUnloadsPerUpdate, 1, 64);
    const bool priorAsync = asyncStreaming;
    changed |= ui.Checkbox("Async streaming", &asyncStreaming);
    changed |= ui.SliderInt("Prepared cell limit", &maxPreparedCells, 1, 64);
    changed |= ui.SliderInt("Uploads per update", &maxUploadsPerUpdate, 1, 64);
    changed |= ui.Checkbox("Batch far terrain cells", &batchFarCells);
    changed |= ui.Checkbox("Fast runtime terrain lighting", &fastRuntimeLighting);
    changed |= ui.Checkbox("Measurement grid (metres and cell borders)",
        &measurementGrid);
    changed |= ui.SliderInt("CPU terrain budget MiB", &cpuBudgetMiB, 1, 16384);
    changed |= ui.SliderInt("GPU terrain budget MiB", &gpuBudgetMiB, 1, 16384);
    changed |= ui.SliderInt("Mesh cache budget MiB", &cacheBudgetMiB, 1, 16384);
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
}

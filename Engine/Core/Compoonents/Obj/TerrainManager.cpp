#include "TerrainManager.h"
#include "Core/Object.h"
#include "Core/Scene/Scene.h"
#include "Engine/Editor/UI/IEditorUi.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <type_traits>
#include <chrono>

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
                    if (!task.cachePath.empty())
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

void TerrainManager::CommitPrepared(int centerX, int centerY, int radius,
    int uploadBudget)
{
    const int limit = std::clamp(uploadBudget, 0, 64);
    for (int uploaded = 0; uploaded < limit; )
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
        object->transform.position = glm::vec3(Owner->transform.GetWorldMatrix() *
            glm::vec4(cell.first * cellsX * cellWidth, 0.f,
                cell.second * cellsY * cellDepth, 1.f));
        Owner->GetScene()->MoveObject(object, Owner,
            Engine::Scene::Scene::ObjectPlacement::AsChild);
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
        result = terrain->SaveCache(path, m_cacheSignature);
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
        !ReadBinary(file, layerCount) || layerCount != terrain.layers.size() ||
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
        for (uint32_t layer = 0; layer < layerCount; ++layer)
            if (!ReadBinary(file, weights[index * layerCount + layer])) return false;
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
    object->transform.position = glm::vec3(Owner->transform.GetWorldMatrix() *
        glm::vec4(cell.first * cellsX * cellWidth, 0.f,
            cell.second * cellsY * cellDepth, 1.f));
    Owner->GetScene()->MoveObject(object, Owner,
        Engine::Scene::Scene::ObjectPlacement::AsChild);
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
        if (saved == m_cells.end() && !cachePath.empty())
            terrain->SaveCache(cachePath, m_cacheSignature);
    }
    if (m_editedProcedural.count(cell)) LoadEdit(cell, *terrain);
    m_loaded[cell] = terrain;
    Stitch(cell);
    terrain->SetMeshLodStride(lodStride);
}

int TerrainManager::DesiredLodStride(Cell cell, int centerX, int centerY) const
{
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
    for (const Cell offset : {Cell{-1, 0}, Cell{1, 0}, Cell{0, -1}, Cell{0, 1}})
    {
        auto* neighbor = GetLoadedCell(cell.first + offset.first, cell.second + offset.second);
        if (!neighbor) continue;
        const int count = offset.first ? cellsY : cellsX;
        for (int i = 0; i <= count; ++i)
        {
            const int ax = offset.first < 0 ? 0 : offset.first > 0 ? cellsX : i;
            const int ay = offset.second < 0 ? 0 : offset.second > 0 ? cellsY : i;
            const int bx = offset.first < 0 ? cellsX : offset.first > 0 ? 0 : i;
            const int by = offset.second < 0 ? cellsY : offset.second > 0 ? 0 : i;
            const size_t a = static_cast<size_t>(ay * (cellsX + 1) + ax);
            const size_t b = static_cast<size_t>(by * (cellsX + 1) + bx);
            if (terrain->heights[a] != neighbor->heights[b])
            {
                terrain->heights[a] = neighbor->heights[b];
                changed = true;
            }
        }
    }
    if (changed) terrain->Rebuild();
}

bool TerrainManager::AddCell(int x, int y)
{
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
    if (HasCell(x, y) || !m_proceduralBuilder) return false;
    m_proceduralCells.insert({x, y});
    MarkConfigurationDirty();
    return true;
}

bool TerrainManager::RemoveCell(int x, int y)
{
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

bool TerrainManager::ApplyBrush(float x, float z, float radius, float strength,
    Terrain::Brush brush, int layer)
{
    bool changed = false;
    for (auto& [cell, terrain] : m_loaded)
        if (terrain->ApplyBrush(x - cell.first * cellsX * cellWidth,
            z - cell.second * cellsY * cellDepth, radius, strength, brush, layer))
        {
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
            changed = true;
        }
    if (changed)
    {
        MarkConfigurationDirty();
    }
    return changed;
}

void TerrainManager::SyncLoadedCells(int centerX, int centerY)
{
    const int radius = std::clamp(std::max(loadedRadius, farRadius), 0, 32);
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
}

void TerrainManager::UpdateStreaming(int centerX, int centerY)
{
    const int radius = std::clamp(std::max(loadedRadius, farRadius), 0, 32);
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
    for (Cell cell : pending)
    {
        if (loaded >= std::clamp(maxLoadsPerUpdate, 1, 64)) break;
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
    if (asyncStreaming)
        CommitPrepared(centerX, centerY, radius,
            std::clamp(maxUploadsPerUpdate, 1, 64) - changed);
}

void TerrainManager::Update()
{
    if (!Owner || viewerObjectName.empty()) return;
    auto* viewer = Owner->FindObjectInSceneByName(viewerObjectName);
    if (!viewer) return;
    const glm::vec3 local = glm::vec3(glm::inverse(Owner->transform.GetWorldMatrix()) *
        glm::vec4(viewer->transform.GetWorldMatrix()[3]));
    UpdateStreaming(static_cast<int>(std::floor(local.x / (cellsX * cellWidth))),
        static_cast<int>(std::floor(local.z / (cellsY * cellDepth))));
}

TerrainManager::JsonValue TerrainManager::Serialize() const
{
    JsonValue out = Component::Serialize();
    out.Set("cellsX", JsonValue(cellsX)).Set("cellsY", JsonValue(cellsY))
        .Set("cellWidth", JsonValue(cellWidth)).Set("cellDepth", JsonValue(cellDepth))
        .Set("loadedRadius", JsonValue(loadedRadius))
        .Set("farRadius", JsonValue(farRadius))
        .Set("lod2Radius", JsonValue(lod2Radius))
        .Set("lod4Radius", JsonValue(lod4Radius))
        .Set("maxLodChangesPerUpdate", JsonValue(maxLodChangesPerUpdate))
        .Set("maxLoadsPerUpdate", JsonValue(maxLoadsPerUpdate))
        .Set("maxUnloadsPerUpdate", JsonValue(maxUnloadsPerUpdate))
        .Set("asyncStreaming", JsonValue(asyncStreaming))
        .Set("maxPreparedCells", JsonValue(maxPreparedCells))
        .Set("maxUploadsPerUpdate", JsonValue(maxUploadsPerUpdate))
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
    for (auto it = m_loaded.begin(); it != m_loaded.end(); )
    { Cell cell = it->first; ++it; UnloadCell(cell); }
    Component::Deserialize(value);
    cellsX = std::clamp(value["cellsX"].AsInt(), 1, 512);
    cellsY = std::clamp(value["cellsY"].AsInt(), 1, 512);
    cellWidth = std::max(.001f, value["cellWidth"].AsFloat());
    cellDepth = std::max(.001f, value["cellDepth"].AsFloat());
    loadedRadius = std::clamp(value["loadedRadius"].AsInt(), 0, 32);
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
                loaded->Owner->transform.position = {cell.first * cellsX * cellWidth,
                    0.f, cell.second * cellsY * cellDepth};
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

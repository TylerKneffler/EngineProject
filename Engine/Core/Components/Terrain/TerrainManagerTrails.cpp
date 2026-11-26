#include "Core/Components/Terrain/TerrainManager.h"
#include "Core/Graphics/IGraphicsProvider.h"
#include "Core/Graphics/IGraphicsTexture.h"
#include "Core/Scene/Object.h"
#include "Core/Scene/Scene.h"

#include <algorithm>
#include <vector>

// Trails as scene data (TerrainTrails.h): publishers, the shaded and carved
// networks, and the renderer's texture.
namespace Engine::Components
{
namespace
{
// Pause after the last trail edit before carved cells reload: dragging a
// control point publishes every frame.
constexpr auto CarveSettle = std::chrono::milliseconds(400);

uint64_t CarveKey(const std::shared_ptr<const TerrainTrailNetwork>& network)
{
    return network && !network->Empty() ? network->Signature() : 0ull;
}
}

std::shared_ptr<const TerrainTrailNetwork> TerrainManager::GetTrails() const
{
    std::lock_guard lock(m_trailState->mutex);
    if (m_trailState->live) return m_trailState->live;
    static const auto empty = TerrainTrailNetwork::Build({});
    return empty;
}

void TerrainManager::SetTrail(uint64_t key, TerrainTrail trail)
{
    m_trailSources[key] = std::move(trail);
    RebuildTrailNetwork();
}

void TerrainManager::SetTrailWatcher(uint64_t key, std::function<void()> watcher)
{
    if (watcher) m_trailWatchers[key] = std::move(watcher);
    else m_trailWatchers.erase(key);
}

void TerrainManager::RemoveTrail(uint64_t key)
{
    if (m_trailSources.erase(key)) RebuildTrailNetwork();
}

void TerrainManager::RebuildTrailNetwork()
{
    std::vector<TerrainTrail> trails;
    trails.reserve(m_trailSources.size());
    for (const auto& [key, source] : m_trailSources) trails.push_back(source);
    auto network = TerrainTrailNetwork::Build(std::move(trails));
    {
        std::lock_guard lock(m_trailState->mutex);
        m_trailState->live = std::move(network);
    }
    // Nothing built yet: carve from the start, no reload needed.
    if (m_loaded.empty()) ApplyTrailCarving();
    else
    {
        m_trailCarvePending = true;
        m_trailChangedAt = std::chrono::steady_clock::now();
    }
}

void TerrainManager::ApplyTrailCarving()
{
    m_trailCarvePending = false;
    std::shared_ptr<const TerrainTrailNetwork> previous, next;
    {
        std::lock_guard lock(m_trailState->mutex);
        previous = m_trailState->carved;
        next = m_trailState->live;
    }
    if (CarveKey(previous) == CarveKey(next)) return;
    // Cells the old or new carve reaches reload. Unloaded before the switch,
    // so a cell's sculpt edit is saved against the carve it was made on.
    InvalidatePrepared();
    const double spanX = double(cellsX) * cellWidth, spanZ = double(cellsY) * cellDepth;
    std::vector<Cell> reload;
    for (const auto& [cell, terrain] : m_loaded)
    {
        if (!m_proceduralCells.count(cell)) continue;
        const glm::vec2 low(float(cell.first * spanX), float(cell.second * spanZ));
        const glm::vec2 high(float((cell.first + 1) * spanX), float((cell.second + 1) * spanZ));
        if ((previous && previous->CarvesArea(low, high)) || (next && next->CarvesArea(low, high)))
            reload.push_back(cell);
    }
    for (const Cell cell : reload) UnloadCell(cell);
    {
        std::lock_guard lock(m_trailState->mutex);
        m_trailState->carved = next;
    }
    // Cached cells were built with the old carve.
    m_cacheSignature = m_baseCacheSignature ^ CarveKey(next);
}

void TerrainManager::UpdateTrailCarving()
{
    if (m_trailCarvePending && std::chrono::steady_clock::now() - m_trailChangedAt >= CarveSettle)
        ApplyTrailCarving();
}

TerrainManager::TrailGpuData TerrainManager::GetTrailGpuData(IGraphicsProvider* provider)
{
    const auto network = GetTrails();
    if (network == m_trailTextureNetwork && provider == m_trailTextureProvider &&
        m_farPalette.farColors == m_trailTexturePalette)
        return m_trailGpu;
    m_trailTextureNetwork = network;
    m_trailTexturePalette = m_farPalette.farColors;
    m_trailTextureProvider = provider;
    m_trailTexture.reset();
    m_trailGpu = {};
    if (network->Empty() || !provider) return m_trailGpu;
    auto* factory = provider->GetTextureFactory();
    if (!factory) return m_trailGpu;
    const auto layout = network->PackGpu(m_farPalette.farColors.data(), m_farPalette.farColors.size());
    m_trailTexture = factory->CreateTexture2D(layout.width, layout.height,
        reinterpret_cast<const uint8_t*>(layout.texels.data()), 1,
        Engine::Graphics::GraphicsTextureFormat::Rgba32Float, false);
    if (!m_trailTexture) return m_trailGpu;
    m_trailGpu.texture = m_trailTexture.get();
    m_trailGpu.grid = layout.grid;
    m_trailGpu.keys = layout.keys;
    return m_trailGpu;
}
}

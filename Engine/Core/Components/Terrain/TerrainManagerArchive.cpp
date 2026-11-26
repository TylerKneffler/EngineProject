#include "Core/Components/Terrain/TerrainManager.h"
#include "Core/Components/Terrain/TerrainHeightArchive.h"
#include "Core/Components/Terrain/TerrainPaintArchive.h"
#include "Core/Components/Mesh/Mesh.h"

#include <cstdio>
#include <stdexcept>

// A map stored as archives: heights, paint and layers as scene data, read by
// the manager itself (TerrainManager.h, ConfigureArchiveTerrain).
namespace Engine::Components
{
int TerrainManager::ArchiveHalfExtent() const
{
    int extent = 0;
    uint64_t signature = 0;
    return !heightArchive.empty() && TerrainHeightArchive::ReadHeader(
        Engine::Components::Mesh::ResolveFilePath(heightArchive), cellsX, cellsY, extent, signature) ? extent : 0;
}

uint64_t TerrainManager::ArchiveSignature() const
{
    int extent = 0;
    uint64_t signature = 0;
    return !heightArchive.empty() && TerrainHeightArchive::ReadHeader(
        Engine::Components::Mesh::ResolveFilePath(heightArchive), cellsX, cellsY, extent, signature) ? signature : 0;
}

std::shared_ptr<const TerrainPaintArchive> TerrainManager::GetPaintArchive() const
{
    if (paintArchive.empty()) return nullptr;
    if (m_paintArchiveOpened != paintArchive)
    {
        m_paintArchiveOpened = paintArchive;
        m_paintArchive = TerrainPaintArchive::Open(Engine::Components::Mesh::ResolveFilePath(paintArchive));
        if (!m_paintArchive)
            std::fprintf(stderr, "[TerrainManager] no paint archive at %s\n", paintArchive.c_str());
    }
    return m_paintArchive;
}

bool TerrainManager::ConfigureArchiveTerrain()
{
    if (heightArchive.empty()) return false;
    // A script's own builder (SetProceduralBuilder) wins.
    if (m_proceduralBuilder && !m_archiveBuilder) return false;
    const std::string path = Engine::Components::Mesh::ResolveFilePath(heightArchive);
    int extent = 0;
    uint64_t signature = 0;
    if (!TerrainHeightArchive::ReadHeader(path, cellsX, cellsY, extent, signature))
    {
        std::fprintf(stderr, "[TerrainManager] no height archive at %s for %dx%d cells\n",
            heightArchive.c_str(), cellsX, cellsY);
        return false;
    }
    const auto paint = GetPaintArchive();
    // The key of everything a built cell holds: heights, paint, layers.
    uint64_t key = signature ^ 0x4152434849564531ull;
    const auto mix = [&](const void* data, size_t size) {
        for (size_t i = 0; i < size; ++i) key = (key ^ static_cast<const uint8_t*>(data)[i]) * 1099511628211ull;
    };
    const uint64_t paintKey = paint ? paint->Signature() : 0ull;
    mix(&paintKey, sizeof(paintKey));
    for (const auto& layer : terrainLayers) { mix(layer.material.data(), layer.material.size()); mix(&layer.preview, sizeof(layer.preview)); }
    mix(&defaultPaintLayer, sizeof(defaultPaintLayer));
    mix(&layerRepeatMetres, sizeof(layerRepeatMetres));
    if (m_archiveBuilder && key == m_archiveKey) return true;
    SetStaticCache(cacheDirectory, key);
    std::vector<Terrain::Layer> layers;
    for (const auto& layer : terrainLayers) layers.push_back({layer.material, -1000.f, 1000.f, layer.preview});
    const size_t layerCount = layers.size();
    const int defaultLayer = std::clamp(defaultPaintLayer, 0, std::max(0, int(layerCount) - 1));
    const float tiling = cellsX * cellWidth / std::max(layerRepeatMetres, .01f);
    const int countX = cellsX, countY = cellsY;
    // Everything by value: it runs on streaming workers.
    InstallBuilder([=](Cell cell, Terrain& terrain) {
        terrain.textureTiling = tiling;
        terrain.layers = layers;
        std::vector<uint8_t> codes;
        if (!TerrainHeightArchive::ReadCellCodes(path, signature, extent, countX, countY,
                cell.first, cell.second, terrain.heights, codes))
            throw std::runtime_error("Could not read height archive cell");
        const size_t vertices = terrain.heights.size();
        if (!(paint && layerCount && paint->GetHeader().layers == int(layerCount) &&
                paint->ReadCell(cell, terrain.weights, terrain.lushness) &&
                terrain.weights.size() == vertices * layerCount && terrain.lushness.size() == vertices))
        {
            terrain.weights.assign(vertices * layerCount, 0.f);
            if (layerCount)
                for (size_t v = 0; v < vertices; ++v) terrain.weights[v * layerCount + size_t(defaultLayer)] = 1.f;
            terrain.lushness.assign(vertices, 0.f);
        }
        terrain.Rebuild();
    });
    m_archiveBuilder = true;
    m_archiveKey = key;
    // The macro colour map from the same archives (TerrainManagerArchiveMap.cpp).
    InstallArchiveMacroColors();
    return true;
}
}

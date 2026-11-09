#include "TerrainManager.h"
#include "TerrainManagerFarTerrainCache.h"
#include "TerrainObjectLayer.h"
#include "Core/Compoonents/Terrain/TerrainBatchAppearance.h"
#include "Core/Compoonents/Camera/Camera.h"
#include "Core/Compoonents/Mesh/Mesh.h"
#include "Core/Compoonents/Materials/Material.h"
#include "Core/Compoonents/Materials/Texture.h"
#include "Core/Object.h"
#include "Core/Scene/Scene.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <map>

namespace Engine::Components
{
using FarTerrainInternal::HashValue;
using FarTerrainInternal::ReadFarTileCache;
using FarTerrainInternal::WriteFarTileCache;
namespace
{
int StreamingRequestScore(int dx, int dy, int travelX, int travelY)
{
    return 4 * (dx * dx + dy * dy) -
        2 * (dx * std::clamp(travelX, -1, 1) +
            dy * std::clamp(travelY, -1, 1));
}
}
void TerrainManager::UpdateFarTerrain(int centerX, int centerY,
    const Camera* camera, int viewportHeight, float aspect)
{
    using FarClock = std::chrono::steady_clock;
    const auto profileStart = FarClock::now();
    m_lastFarUpdateTiming = {};
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
        TerrainHeightArchive::BlendHeightBands(height,
            m_farPalette.heightBoundaries,
            m_farPalette.heightBlendHalfWidth, weights);
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
    struct VegetationCluster
    {
        glm::vec3 localPosition{0.f};
        float scale = 1.f;
        uint32_t count = 0;
    };
    std::map<Cell, std::vector<VegetationCluster>>
        farVegetationClustersByCell;
    if (farVegetationImpostors && farVegetationImpostorBudgetPerTile > 0)
    {
        struct ClusterAccumulator
        {
            glm::vec3 weightedPosition{0.f};
            float weight = 0.f;
            float maxScale = 0.f;
            uint32_t count = 0;
        };
        const float clusterSpan = std::max(2.f,
            farVegetationClusterSpanMeters);
        const auto isVegetationAsset = [](std::string asset)
        {
            std::transform(asset.begin(), asset.end(), asset.begin(),
                [](unsigned char ch) {
                    return static_cast<char>(std::tolower(ch));
                });
            return asset.find("tree") != std::string::npos ||
                asset.find("oak") != std::string::npos ||
                asset.find("pine") != std::string::npos ||
                asset.find("bush") != std::string::npos ||
                asset.find("shrub") != std::string::npos ||
                asset.find("foliage") != std::string::npos ||
                asset.find("grass") != std::string::npos ||
                asset.find("plant") != std::string::npos;
        };
        const auto collectLayerClusters = [&](Cell cell,
            const JsonValue& layers)
        {
            std::map<Cell, ClusterAccumulator> accumulators;
            for (size_t layerIndex = 0; layerIndex < layers.ArraySize();
                ++layerIndex)
            {
                const auto& layer = layers.ArrayAt(layerIndex);
                const auto& placements = layer["placements"];
                const std::string layerName = layer["layerName"].AsString();
                std::string loweredLayerName = layerName;
                std::transform(loweredLayerName.begin(),
                    loweredLayerName.end(), loweredLayerName.begin(),
                    [](unsigned char ch) {
                        return static_cast<char>(std::tolower(ch));
                    });
                const bool vegetationLayer =
                    loweredLayerName.find("tree") != std::string::npos ||
                    loweredLayerName.find("foliage") != std::string::npos ||
                    loweredLayerName.find("grass") != std::string::npos ||
                    loweredLayerName.find("plant") != std::string::npos;
                for (size_t placementIndex = 0;
                    placementIndex < placements.ArraySize(); ++placementIndex)
                {
                    const auto& placement = placements.ArrayAt(
                        placementIndex);
                    const auto& position = placement["position"];
                    if (position.ArraySize() != 3)
                        continue;
                    const std::string asset = placement["asset"].AsString();
                    if (!vegetationLayer && !isVegetationAsset(asset))
                        continue;
                    const float scale = placement.Has("scale")
                        ? std::max(0.f, placement["scale"].AsFloat()) : 1.f;
                    if (!std::isfinite(scale) || scale <= 0.f)
                        continue;
                    const glm::vec3 localPosition(
                        position.ArrayAt(0).AsFloat(),
                        position.ArrayAt(1).AsFloat(),
                        position.ArrayAt(2).AsFloat());
                    if (!std::isfinite(localPosition.x) ||
                        !std::isfinite(localPosition.y) ||
                        !std::isfinite(localPosition.z))
                        continue;
                    const Cell clusterCell{
                        static_cast<int>(std::floor(localPosition.x /
                            clusterSpan)),
                        static_cast<int>(std::floor(localPosition.z /
                            clusterSpan))
                    };
                    auto& cluster = accumulators[clusterCell];
                    cluster.weightedPosition += localPosition * scale;
                    cluster.weight += scale;
                    cluster.maxScale = std::max(cluster.maxScale, scale);
                    ++cluster.count;
                }
            }
            if (accumulators.empty())
                return;
            auto& output = farVegetationClustersByCell[cell];
            output.reserve(output.size() + accumulators.size());
            for (const auto& [clusterCell, accumulator] : accumulators)
            {
                if (accumulator.count == 0 || accumulator.weight <= 0.f)
                    continue;
                VegetationCluster cluster;
                cluster.localPosition = accumulator.weightedPosition /
                    accumulator.weight;
                cluster.scale = std::max(0.5f, accumulator.maxScale);
                cluster.count = accumulator.count;
                output.push_back(cluster);
            }
        };
        for (const auto& [cell, layers] : m_objectLayers)
            if (std::max(std::abs(cell.first - centerX),
                std::abs(cell.second - centerY)) > nearRadius + 1)
                collectLayerClusters(cell, layers);
        for (const auto& [cell, terrain] : m_loaded)
        {
            if (!terrain || !terrain->Owner)
                continue;
            if (std::max(std::abs(cell.first - centerX),
                std::abs(cell.second - centerY)) <= nearRadius + 1)
                continue;
            JsonValue layers = JsonValue::MakeArray();
            for (const auto* child : terrain->Owner->Children)
                if (child)
                    if (const auto* layer = child->GetComponent<
                        TerrainObjectLayer>())
                        layers.Push(layer->Serialize());
            if (layers.ArraySize())
                collectLayerClusters(cell, layers);
        }
    }
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
        // Keep adding coarser parents for maps wider than the original
        // 129-cell test scene. Only the selected nodes create scene objects.
        std::vector<size_t> layer;
        for (size_t index = 0; index < m_farTiles.size(); ++index)
            if (m_farTiles[index].lodStride == 4) layer.push_back(index);
        for (int stride = 8; layer.size() > 4 && stride <= 1024;
            stride *= 2)
        {
            int firstX = std::numeric_limits<int>::max();
            int firstY = std::numeric_limits<int>::max();
            for (size_t index : layer)
            {
                firstX = std::min(firstX, m_farTiles[index].startX);
                firstY = std::min(firstY, m_farTiles[index].startY);
            }
            const int groupCells = tileCells * stride;
            std::map<Cell, std::vector<size_t>> groups;
            for (size_t index : layer)
            {
                const auto& tile = m_farTiles[index];
                groups[{(tile.startX - firstX) / groupCells,
                    (tile.startY - firstY) / groupCells}].push_back(index);
            }
            std::vector<size_t> next;
            for (auto& [coordinate, children] : groups)
            {
                FarTile parent;
                parent.lodStride = stride;
                parent.children = std::move(children);
                parent.startX = parent.startY = std::numeric_limits<int>::max();
                parent.endX = parent.endY = std::numeric_limits<int>::lowest();
                parent.minHeight = std::numeric_limits<float>::max();
                parent.maxHeight = std::numeric_limits<float>::lowest();
                for (size_t childIndex : parent.children)
                {
                    const auto& child = m_farTiles[childIndex];
                    parent.startX = std::min(parent.startX, child.startX);
                    parent.startY = std::min(parent.startY, child.startY);
                    parent.endX = std::max(parent.endX, child.endX);
                    parent.endY = std::max(parent.endY, child.endY);
                    parent.minHeight = std::min(parent.minHeight, child.minHeight);
                    parent.maxHeight = std::max(parent.maxHeight, child.maxHeight);
                }
                for (int gy = parent.startY; gy <= parent.endY; ++gy)
                    for (int gx = parent.startX; gx <= parent.endX; ++gx)
                    {
                        const float height = gridHeight(gx, gy);
                        const int x0 = parent.startX +
                            ((gx - parent.startX) / stride) * stride;
                        const int y0 = parent.startY +
                            ((gy - parent.startY) / stride) * stride;
                        const int x1 = std::min(x0 + stride, parent.endX);
                        const int y1 = std::min(y0 + stride, parent.endY);
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
                for (size_t childIndex : parent.children)
                    m_farTiles[childIndex].parentIndex = m_farTiles.size();
                next.push_back(m_farTiles.size());
                m_farTiles.push_back(std::move(parent));
            }
            if (next.size() >= layer.size()) break;
            layer = std::move(next);
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
        // Parent indices are stable now. Mark the ancestor chain once instead
        // of searching every edited cell for every parent on every frame.
        for (size_t index = 0; index < m_farTiles.size(); ++index)
        {
            if (m_farTiles[index].patchIndices.empty()) continue;
            for (size_t ancestor = index; ancestor != static_cast<size_t>(-1);
                ancestor = m_farTiles[ancestor].parentIndex)
                m_farTiles[ancestor].containsEditedPatch = true;
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
    const double viewDistance = camera ?
        (maxFarViewDistanceMeters > 0.f
            ? std::min(static_cast<double>(camera->farPlane),
                static_cast<double>(maxFarViewDistanceMeters))
            : static_cast<double>(camera->farPlane)) :
        static_cast<double>(maxFarViewDistanceMeters);
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
            viewDistance;
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
        const bool hasPatch = parent.containsEditedPatch;
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
            const double farDistance = std::max(nearDistance, viewDistance);
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
            const float levelError = parent.lodStride == 2
                ? farTileErrorPixelsLod2 : parent.lodStride == 4
                ? farTileErrorPixelsLod4 : parent.lodStride == 8
                ? farTileErrorPixelsLod8 : farTileErrorPixelsLod16;
            const double threshold = std::clamp(levelError >= 0.f
                ? levelError : farTileScreenErrorPixels, .1f, 64.f);
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
        if (!tile.desired) tile.requestedAt.reset();
        if (tile.desired && (needsRebuild(tile) ||
            !tile.dirtyPatchIndices.empty()))
        {
            if (!tile.requestedAt) tile.requestedAt = FarClock::now();
            pending.push_back(index);
        }
    }
    const auto tileDistance = [&](size_t index) {
        const FarTile& tile = m_farTiles[index];
        const int dx = tile.startX > centerX ? tile.startX - centerX :
            tile.endX <= centerX ? tile.endX - 1 - centerX : 0;
        const int dy = tile.startY > centerY ? tile.startY - centerY :
            tile.endY <= centerY ? tile.endY - 1 - centerY : 0;
        const int travelX = m_prefetchCenter
            ? m_prefetchCenter->first - centerX : 0;
        const int travelY = m_prefetchCenter
            ? m_prefetchCenter->second - centerY : 0;
        return StreamingRequestScore(dx, dy, travelX, travelY);
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
            viewCenter.z - radius <= viewDistance &&
            std::abs(viewCenter.x) <= halfY * viewAspect + radius &&
            std::abs(viewCenter.y) <= halfY + radius;
    };
    struct TilePriority
    {
        size_t index;
        bool gap, ready, inView;
        int distance;
    };
    std::vector<TilePriority> priorities;
    priorities.reserve(pending.size());
    for (size_t index : pending)
        priorities.push_back({index, exposesGap(index),
            readyIndices.count(index) != 0, cameraVisible(index),
            tileDistance(index)});
    std::sort(priorities.begin(), priorities.end(),
        [](const TilePriority& a, const TilePriority& b) {
            if (a.gap != b.gap) return a.gap;
            if (a.ready != b.ready) return a.ready;
            if (a.inView != b.inView) return a.inView;
            return a.distance == b.distance ? a.index < b.index :
                a.distance < b.distance;
        });
    for (size_t order = 0; order < priorities.size(); ++order)
        pending[order] = priorities[order].index;
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
    const auto selectedAt = FarClock::now();
    m_lastFarUpdateTiming.selectionMs =
        std::chrono::duration<double, std::milli>(
            selectedAt - profileStart).count();
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
                    const auto patchUploadStart = FarClock::now();
                    const bool patchUploaded =
                        tile.mesh->UpdateIndexedVertexRange(offset, changed);
                    m_lastFarUpdateTiming.uploadMs +=
                        std::chrono::duration<double, std::milli>(
                            FarClock::now() - patchUploadStart).count();
                    if (!patchUploaded)
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
        std::optional<Mesh::IndexedGeometryBounds> preparedBounds;
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
                    preparedBounds = ready->bounds;
                    cacheHit = ready->cacheHit;
                    m_farTileRebuildMilliseconds += ready->buildMilliseconds;
                    m_lastFarUpdateTiming.workerBuildMs += ready->buildMilliseconds;
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
                        ++m_lastFarUpdateTiming.queuedTiles;
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
        if (!farVegetationClustersByCell.empty() &&
            farVegetationImpostorBudgetPerTile > 0 && tile.lodStride > 1)
        {
            struct TileCluster
            {
                glm::vec3 position{0.f};
                float scale = 1.f;
                uint32_t count = 0;
                float priority = 0.f;
            };
            std::vector<TileCluster> tileClusters;
            for (int cellY = tile.startY; cellY < tile.endY; ++cellY)
                for (int cellX = tile.startX; cellX < tile.endX; ++cellX)
                {
                    if (!visible(cellX, cellY, centerX, centerY))
                        continue;
                    const auto found = farVegetationClustersByCell.find(
                        {cellX, cellY});
                    if (found == farVegetationClustersByCell.end())
                        continue;
                    for (const auto& cluster : found->second)
                    {
                        TileCluster candidate;
                        candidate.position = glm::vec3(
                            (cellX - tile.startX) * spanX +
                                cluster.localPosition.x,
                            cluster.localPosition.y,
                            (cellY - tile.startY) * spanZ +
                                cluster.localPosition.z);
                        candidate.scale = cluster.scale;
                        candidate.count = cluster.count;
                        candidate.priority = cluster.scale *
                            std::sqrt(static_cast<float>(
                                std::max(1u, cluster.count)));
                        tileClusters.push_back(candidate);
                    }
                }
            if (!tileClusters.empty())
            {
                std::sort(tileClusters.begin(), tileClusters.end(),
                    [](const TileCluster& first, const TileCluster& second) {
                        return first.priority == second.priority
                            ? first.position.x < second.position.x
                            : first.priority > second.priority;
                    });
                if (tileClusters.size() > static_cast<size_t>(
                    farVegetationImpostorBudgetPerTile))
                    tileClusters.resize(static_cast<size_t>(
                        farVegetationImpostorBudgetPerTile));
                const auto addImpostorCross = [&](const TileCluster& cluster) {
                    const float width = std::clamp(cluster.scale * 1.6f,
                        1.f, 24.f);
                    const float height = std::clamp(cluster.scale * 4.f +
                        std::sqrt(static_cast<float>(cluster.count)),
                        2.f, 48.f);
                    const float x = cluster.position.x;
                    const float y = cluster.position.y;
                    const float z = cluster.position.z;
                    const glm::vec3 tint = cluster.count >= 4
                        ? glm::vec3(0.11f, 0.30f, 0.12f)
                        : glm::vec3(0.16f, 0.36f, 0.14f);
                    const auto appendVertex = [&](float vx, float vy,
                        float vz) -> uint32_t {
                        Mesh::Vertex vertex{};
                        vertex.pos[0] = vx;
                        vertex.pos[1] = vy;
                        vertex.pos[2] = vz;
                        vertex.normal[0] = 0.f;
                        vertex.normal[1] = 1.f;
                        vertex.normal[2] = 0.f;
                        vertex.color[0] = tint.r;
                        vertex.color[1] = tint.g;
                        vertex.color[2] = tint.b;
                        vertex.color[3] = 1.f;
                        vertices.push_back(vertex);
                        return static_cast<uint32_t>(vertices.size() - 1);
                    };
                    const float half = width * .5f;
                    const uint32_t a0 = appendVertex(x - half, y, z);
                    const uint32_t a1 = appendVertex(x + half, y, z);
                    const uint32_t a2 = appendVertex(x - half, y + height, z);
                    const uint32_t a3 = appendVertex(x + half, y + height, z);
                    const uint32_t b0 = appendVertex(x, y, z - half);
                    const uint32_t b1 = appendVertex(x, y, z + half);
                    const uint32_t b2 = appendVertex(x, y + height, z - half);
                    const uint32_t b3 = appendVertex(x, y + height, z + half);
                    indices.insert(indices.end(), {
                        a0, a2, a1, a1, a2, a3,
                        a1, a2, a0, a3, a2, a1,
                        b0, b2, b1, b1, b2, b3,
                        b1, b2, b0, b3, b2, b1});
                };
                for (const auto& cluster : tileClusters)
                    addImpostorCross(cluster);
            }
        }
        const uint64_t uploadBytes=
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
                deferred.bounds = preparedBounds;
                std::lock_guard<std::mutex> lock(m_farPrepareMutex);
                m_preparedFarTiles.push_back(std::move(deferred));
                tile.preparing = true;
            }
            continue;
        }
        if (cacheHit) ++m_farTileCacheHits;
        const auto uploadStart = FarClock::now();
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
        const auto objectReady = FarClock::now();
        const bool installed = asyncPrepared && preparedBounds
            ? tile.mesh->SetPreparedIndexedGeometry(
                std::move(vertices), std::move(indices), *preparedBounds)
            : tile.mesh->SetIndexedGeometry(
                std::move(vertices), std::move(indices));
        if (!installed)
            throw std::runtime_error("Could not build distant terrain tile");
        const auto& meshTiming = tile.mesh->GetLastIndexedGeometryTiming();
        m_lastFarUpdateTiming.meshValidationMs += meshTiming.validationMs;
        m_lastFarUpdateTiming.meshBoundsMs += meshTiming.boundsMs;
        m_lastFarUpdateTiming.meshBufferMs += meshTiming.bufferMs;
        const auto meshReady = FarClock::now();
        if (auto* provider = scene->GetGraphicsProvider())
            if (!tile.mesh->IsReady()) tile.mesh->OnAfterDeserialize(provider);
        const auto graphicsReady = FarClock::now();
        m_lastFarUpdateTiming.objectSetupMs +=
            std::chrono::duration<double, std::milli>(
                objectReady - uploadStart).count();
        m_lastFarUpdateTiming.meshInstallMs +=
            std::chrono::duration<double, std::milli>(
                meshReady - objectReady).count();
        m_lastFarUpdateTiming.graphicsInitMs +=
            std::chrono::duration<double, std::milli>(
                graphicsReady - meshReady).count();
        m_lastFarUpdateTiming.uploadMs +=
            std::chrono::duration<double, std::milli>(
                graphicsReady - uploadStart).count();
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
    const auto preparedAt = FarClock::now();
    const double loopMs = std::chrono::duration<double, std::milli>(
        preparedAt - selectedAt).count();
    m_lastFarUpdateTiming.preparationMs = std::max(0.0,
        loopMs - m_lastFarUpdateTiming.uploadMs);
    m_lastFarUpdateTiming.uploadedTiles = uploadsThisUpdate;
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
            if (tile.object) tile.object->enabled =
                ready || tile.hasGeometry;
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
    const auto visibleAt = FarClock::now();
    for (auto& tile : m_farTiles)
        if (tile.requestedAt && tile.desired && tile.object &&
            tile.object->enabled && tile.mesh && tile.mesh->IsReady() &&
            tile.hasGeometry && !needsRebuild(tile))
        {
            m_lastFarUpdateTiming.maxAppearanceLatencyMs = std::max(
                m_lastFarUpdateTiming.maxAppearanceLatencyMs,
                std::chrono::duration<double, std::milli>(
                    visibleAt - *tile.requestedAt).count());
            ++m_lastFarUpdateTiming.appearedTiles;
            tile.requestedAt.reset();
        }
    // Keep coarse parents in range as fallback coverage while detailed tiles
    // rebuild when the near-cell window crosses their children.
    for (auto& tile : m_farTiles)
        if (tile.object && !tile.object->enabled && !tile.desired &&
            (tile.lodStride == 1 || tile.outOfRange))
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
    const auto finishedAt = FarClock::now();
    m_lastFarUpdateTiming.visibilityMs =
        std::chrono::duration<double, std::milli>(
            finishedAt - preparedAt).count();
    m_lastFarUpdateTiming.totalMs =
        std::chrono::duration<double, std::milli>(
            finishedAt - profileStart).count();
}

}

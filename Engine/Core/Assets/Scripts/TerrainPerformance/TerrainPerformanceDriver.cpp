#include "Scripts/TerrainPerformance/TerrainPerformanceDriver.h"
#include "Scripts/TerrainPerformance/TerrainHeightArchive.h"
#include "Core/Compoonents/Obj/TerrainManager.h"
#include "Core/Compoonents/Obj/Mesh.h"
#include "Core/Compoonents/Materials/Material.h"
#include "Core/Compoonents/Obj/TerrainBatchAppearance.h"
#include "Core/Compoonents/Camera/Camera.h"
#include "Core/Object.h"
#include "Core/Scene/Scene.h"
#include "Core/Serialization/SceneSerializer.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <filesystem>
#include <map>
#include <set>
#include <stdexcept>

TerrainPerformanceDriver::TerrainPerformanceDriver()
{
    SetTypeName(COMPONENT_TYPE_NAME(TerrainPerformanceDriver));
    RegisterField("viewerObjectName", viewerObjectName);
    RegisterField("mapHalfExtentCells", mapHalfExtentCells);
    RegisterField("cacheDirectory", cacheDirectory);
    RegisterField("heightArchive", heightArchive);
    RegisterField("archiveSignature", archiveSignature);
    RegisterField("farGrid", farGrid);
    RegisterField("reportIntervalSeconds", reportIntervalSeconds);
}

namespace
{
struct TerrainPerformanceDriverRegistration
{
    TerrainPerformanceDriverRegistration()
    {
        Engine::Serialization::RegisterComponentType<TerrainPerformanceDriver>(
            "TerrainPerformanceDriver");
    }
} registration;
}

uint64_t TerrainPerformanceDriver::SourceSignature() const
{
    if (!Owner) return 0;
    auto* manager = Owner->GetComponent<Engine::Components::TerrainManager>();
    if (!manager) return 0;
    const std::string path =
        Engine::Components::Mesh::ResolveFilePath(heightArchive);
    uint64_t saved = 0;
    if (!archiveSignature.empty())
    {
        try { saved = std::stoull(archiveSignature); }
        catch (...) { return 0; }
    }
    uint64_t archive = 0;
    if (TerrainHeightArchive::ReadSignature(path, mapHalfExtentCells,
        manager->cellsX, manager->cellsY, archive))
        return saved && saved != archive ? 0 : archive;
    return saved;
}

bool TerrainPerformanceDriver::ConfigureTerrain()
{
    if (!Owner) return false;
    auto* manager = Owner->GetComponent<Engine::Components::TerrainManager>();
    if (!manager) return false;
    const uint64_t signature = SourceSignature();
    const std::string archivePath = Engine::Components::Mesh::ResolveFilePath(heightArchive);
    if (!signature) return false;
    const int countX = manager->cellsX, countY = manager->cellsY;
    const int extent = mapHalfExtentCells;
    std::error_code archiveError;
    const bool sourceAvailable = std::filesystem::exists(archivePath) &&
        std::filesystem::file_size(archivePath, archiveError) ==
            TerrainHeightArchive::ExpectedBytes(extent, countX, countY) &&
        !archiveError;
    std::vector<float> probeHeights, probeWeights;
    if (sourceAvailable && !TerrainHeightArchive::ReadCell(archivePath,
        signature, extent, countX, countY, 0, 0, probeHeights,
        &probeWeights)) return false;
    if (!sourceAvailable && !manager->requireRuntimeBake) return false;
    // Mesh caches include reconstructed paint weights and texture tiling.
    // Refresh them when either appearance setting changes.
    manager->SetStaticCache(cacheDirectory, signature ^ 0x4c41524745555633ull);
    manager->SetProceduralBuilder(sourceAvailable ?
        std::function<void(Engine::Components::TerrainManager::Cell,
            Engine::Components::Terrain&)>{[=](Engine::Components::TerrainManager::Cell cell,
        Engine::Components::Terrain& terrain)
    {
        // One 512 px color map now covers 4 m, so its texels are about 8 mm
        // across instead of 6 cm in a close, human-scale view.
        terrain.textureTiling = 32.f;
        terrain.layers = {
            {"Assets/Materials/Terrain/dark_soil.material", -1000.f, -12.f,
                {.26f, .22f, .17f}},
            {"Assets/Materials/Terrain/warm_dirt.material", -12.f, 0.f,
                {.47f, .36f, .24f}},
            {"Assets/Materials/Terrain/meadow_grass.material", 0.f, 16.f,
                {.27f, .44f, .18f}},
            {"Assets/Materials/Terrain/highland_grass.material", 16.f, 1000.f,
                {.49f, .53f, .29f}},
            {"Assets/Materials/Terrain/valley_grass.material", -1000.f, -12.f,
                {.23f, .41f, .17f}},
            {"Assets/Materials/Terrain/field_grass.material", -12.f, 0.f,
                {.39f, .50f, .22f}}
        };
        std::vector<float> archivedWeights;
        if (!TerrainHeightArchive::ReadCell(archivePath, signature, extent,
            countX, countY, cell.first, cell.second, terrain.heights,
            &archivedWeights))
            throw std::runtime_error("Could not read saved terrain height cell");
        // The four archived bands retain their original indices for existing
        // dirt paint edits. New base terrain routes the two lowest bands to
        // grass while leaving dirt available in paint channels zero and one.
        constexpr size_t archivedLayers = TerrainHeightArchive::LayerCount;
        const size_t layerCount = terrain.layers.size();
        terrain.weights.assign(terrain.heights.size() * layerCount, 0.f);
        for (size_t vertex = 0; vertex < terrain.heights.size(); ++vertex)
        {
            terrain.weights[vertex * layerCount + 4] =
                archivedWeights[vertex * archivedLayers];
            terrain.weights[vertex * layerCount + 5] =
                archivedWeights[vertex * archivedLayers + 1];
            terrain.weights[vertex * layerCount + 2] =
                archivedWeights[vertex * archivedLayers + 2];
            terrain.weights[vertex * layerCount + 3] =
                archivedWeights[vertex * archivedLayers + 3];
        }
        terrain.Rebuild();
    }} : std::function<void(Engine::Components::TerrainManager::Cell,
        Engine::Components::Terrain&)>{});
    m_configuredSignature = signature;
    m_farHeights.clear();
    m_farPatches.clear();
    if (auto* scene = Owner->GetScene())
        for (const auto& tile : m_farTiles)
            if (tile.object) scene->RemoveObject(tile.object);
    m_farTiles.clear();
    m_farTileUploadCount = 0;
    m_farTileUploadedBytes = 0;
    m_farTileRebuildMilliseconds = 0.0;
    m_farCenterValid = false;
    m_farAttempted = false;
    return true;
}

std::vector<Engine::Components::Mesh*>
TerrainPerformanceDriver::GetFarTileMeshes() const
{
    std::vector<Engine::Components::Mesh*> meshes;
    for (const auto& tile : m_farTiles)
        if (tile.object && tile.object->enabled && tile.mesh &&
            tile.mesh->IsReady())
            meshes.push_back(tile.mesh);
    return meshes;
}

void TerrainPerformanceDriver::RefreshFarTerrain(int centerX, int centerY)
{
    if (!Owner || m_farHeights.empty()) return;
    auto* manager = Owner->GetComponent<Engine::Components::TerrainManager>();
    auto* scene = Owner->GetScene();
    if (!manager || !scene) return;
    const int side = mapHalfExtentCells * 2 + 2;
    if (m_farHeights.size() != static_cast<size_t>(side) * side) return;
    const float spanX = manager->cellsX * manager->cellWidth;
    const float spanZ = manager->cellsY * manager->cellDepth;
    constexpr int tileCells = 32;
    // Average linear RGB of the four sRGB terrain textures. The near path
    // decodes texture samples to linear before lighting, so the far vertex
    // palette must use the same space or it appears much brighter.
    constexpr glm::vec3 palette[] = {
        {.04926f, .15254f, .03097f}, {.13833f, .22573f, .04635f},
        {.07026f, .17219f, .03528f}, {.21771f, .25187f, .07453f}};
    const int nearRadius = std::max(manager->loadedRadius, manager->farRadius);
    const auto visible = [nearRadius](int x, int y, int cx, int cy) {
        return std::abs(x - cx) > nearRadius ||
            std::abs(y - cy) > nearRadius;
    };
    const auto gridHeight = [&](int x, int y) {
        return m_farHeights[static_cast<size_t>(
            std::clamp(y + mapHalfExtentCells, 0, side - 1)) * side +
            std::clamp(x + mapHalfExtentCells, 0, side - 1)];
    };
    // The detailed LOD keeps all outer samples. Read those same heights for
    // the first far cell so its shared edge has identical vertices.
    const std::string archivePath = Engine::Components::Mesh::ResolveFilePath(
        heightArchive);
    std::map<std::pair<int, int>, std::vector<float>> seamCache;
    const auto seamHeights = [&](int x, int y) -> const std::vector<float>* {
        const size_t expected = static_cast<size_t>(manager->cellsX + 1) *
            (manager->cellsY + 1);
        if (const auto* loaded = manager->GetLoadedCell(x, y))
            if (loaded->heights.size() == expected) return &loaded->heights;
        auto [it, inserted] = seamCache.try_emplace({x, y});
        const bool bakedRuntime = scene->HasStarted() &&
            manager->requireRuntimeBake;
        if (inserted && !(bakedRuntime
            ? manager->ReadBakedCellHeights({x, y}, it->second)
            : TerrainHeightArchive::ReadCell(archivePath,
                m_configuredSignature, mapHalfExtentCells, manager->cellsX,
                manager->cellsY, x, y, it->second)))
            it->second.clear();
        return it->second.size() == expected ? &it->second : nullptr;
    };
    // 0/1/2/3 identify the far cell's left/bottom/right/top edge.
    const auto seamSide = [&](int x, int y, int cx, int cy) {
        if (!visible(x - 1, y, cx, cy)) return 0;
        if (!visible(x, y + 1, cx, cy)) return 1;
        if (!visible(x + 1, y, cx, cy)) return 2;
        if (!visible(x, y - 1, cx, cy)) return 3;
        return -1;
    };
    const auto setColor = [&](Engine::Components::Mesh::Vertex& vertex,
        float height) {
        float weights[TerrainHeightArchive::LayerCount];
        TerrainHeightArchive::BlendBaseLayers(height,
            TerrainHeightArchive::LayerForHeight(height), weights);
        glm::vec3 color(0.f);
        for (int layer = 0; layer < TerrainHeightArchive::LayerCount; ++layer)
            color += palette[layer] * weights[layer];
        vertex.color[0] = color.r;
        vertex.color[1] = color.g;
        vertex.color[2] = color.b;
        vertex.color[3] = 1.f;
    };
    if (m_farTiles.empty())
        for (int y = -mapHalfExtentCells; y <= mapHalfExtentCells;
            y += tileCells)
            for (int x = -mapHalfExtentCells; x <= mapHalfExtentCells;
                x += tileCells)
                m_farTiles.push_back({x, y,
                    std::min(x + tileCells, mapHalfExtentCells + 1),
                    std::min(y + tileCells, mapHalfExtentCells + 1)});
    std::set<std::pair<int, int>> patchCells;
    for (const auto& patch : m_farPatches)
        patchCells.emplace(patch.x, patch.y);
    for (auto& tile : m_farTiles)
    {
        bool changed = !m_farCenterValid;
        if (!changed)
            for (int y = tile.startY; y < tile.endY && !changed; ++y)
                for (int x = tile.startX; x < tile.endX; ++x)
                    if (visible(x, y, m_farCenterX, m_farCenterY) !=
                        visible(x, y, centerX, centerY))
                    { changed = true; break; }
        if (!changed) continue;
        const auto rebuildStart = std::chrono::steady_clock::now();
        const int width = tile.endX - tile.startX + 1;
        const int depth = tile.endY - tile.startY + 1;
        std::vector<Engine::Components::Mesh::Vertex> vertices(
            static_cast<size_t>(width) * depth);
        for (int row = 0; row < depth; ++row)
            for (int col = 0; col < width; ++col)
            {
                const int x = tile.startX + col, y = tile.startY + row;
                auto& vertex = vertices[static_cast<size_t>(row) * width + col];
                const float height = gridHeight(x, y);
                vertex.pos[0] = col * spanX;
                vertex.pos[1] = height;
                vertex.pos[2] = row * spanZ;
                const glm::vec3 normal = glm::normalize(glm::vec3(
                    (gridHeight(x - 1, y) - gridHeight(x + 1, y)) /
                        (2.f * spanX), 1.f,
                    (gridHeight(x, y - 1) - gridHeight(x, y + 1)) /
                        (2.f * spanZ)));
                vertex.normal[0] = normal.x;
                vertex.normal[1] = normal.y;
                vertex.normal[2] = normal.z;
                vertex.uv[0] = static_cast<float>(x + mapHalfExtentCells) /
                    (side - 1);
                vertex.uv[1] = static_cast<float>(y + mapHalfExtentCells) /
                    (side - 1);
                setColor(vertex, height);
            }
        std::vector<uint32_t> indices;
        for (int y = tile.startY; y < tile.endY; ++y)
            for (int x = tile.startX; x < tile.endX; ++x)
            {
                if (!visible(x, y, centerX, centerY) ||
                    patchCells.count({x, y})) continue;
                const int seam = seamSide(x, y, centerX, centerY);
                const std::vector<float>* nearHeights = seam == 0
                    ? seamHeights(x - 1, y) : seam == 1
                    ? seamHeights(x, y + 1) : seam == 2
                    ? seamHeights(x + 1, y) : seam == 3
                    ? seamHeights(x, y - 1) : nullptr;
                if (nearHeights)
                {
                    const float topLeft = gridHeight(x, y);
                    const float topRight = gridHeight(x + 1, y);
                    const float bottomLeft = gridHeight(x, y + 1);
                    const float bottomRight = gridHeight(x + 1, y + 1);
                    const auto addVertex = [&](float u, float v) {
                        Engine::Components::Mesh::Vertex vertex{};
                        float height = (1.f - u) * (1.f - v) * topLeft +
                            u * (1.f - v) * topRight +
                            (1.f - u) * v * bottomLeft +
                            u * v * bottomRight;
                        const int col = static_cast<int>(std::round(u * manager->cellsX));
                        const int row = static_cast<int>(std::round(v * manager->cellsY));
                        if (seam == 0 && u == 0.f)
                            height = (*nearHeights)[static_cast<size_t>(
                                row * (manager->cellsX + 1) + manager->cellsX)];
                        else if (seam == 1 && v == 1.f)
                            height = (*nearHeights)[static_cast<size_t>(col)];
                        else if (seam == 2 && u == 1.f)
                            height = (*nearHeights)[static_cast<size_t>(
                                row * (manager->cellsX + 1))];
                        else if (seam == 3 && v == 0.f)
                            height = (*nearHeights)[static_cast<size_t>(
                                manager->cellsY * (manager->cellsX + 1) + col)];
                        vertex.pos[0] = (x - tile.startX + u) * spanX;
                        vertex.pos[1] = height;
                        vertex.pos[2] = (y - tile.startY + v) * spanZ;
                        const float dx = ((1.f - v) * (topRight - topLeft) +
                            v * (bottomRight - bottomLeft)) / spanX;
                        const float dz = ((1.f - u) * (bottomLeft - topLeft) +
                            u * (bottomRight - topRight)) / spanZ;
                        const glm::vec3 normal = glm::normalize(
                            glm::vec3(-dx, 1.f, -dz));
                        vertex.normal[0] = normal.x;
                        vertex.normal[1] = normal.y;
                        vertex.normal[2] = normal.z;
                        vertex.uv[0] = static_cast<float>(x + u + mapHalfExtentCells) /
                            (side - 1);
                        vertex.uv[1] = static_cast<float>(y + v + mapHalfExtentCells) /
                            (side - 1);
                        setColor(vertex, height);
                        const uint32_t index = static_cast<uint32_t>(vertices.size());
                        vertices.push_back(vertex);
                        return index;
                    };
                    const uint32_t center = addVertex(.5f, .5f);
                    std::vector<uint32_t> boundary;
                    const int leftSteps = seam == 0 ? manager->cellsY : 1;
                    const int bottomSteps = seam == 1 ? manager->cellsX : 1;
                    const int rightSteps = seam == 2 ? manager->cellsY : 1;
                    const int topSteps = seam == 3 ? manager->cellsX : 1;
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
        for (const auto& patch : m_farPatches)
        {
            if (patch.x < tile.startX || patch.x >= tile.endX ||
                patch.y < tile.startY || patch.y >= tile.endY ||
                !visible(patch.x, patch.y, centerX, centerY)) continue;
            const int patchWidth = manager->cellsX + 1;
            const int patchDepth = manager->cellsY + 1;
            if (patch.heights.size() !=
                static_cast<size_t>(patchWidth) * patchDepth) continue;
            const int seam = seamSide(patch.x, patch.y, centerX, centerY);
            const std::vector<float>* nearHeights = seam == 0
                ? seamHeights(patch.x - 1, patch.y) : seam == 1
                ? seamHeights(patch.x, patch.y + 1) : seam == 2
                ? seamHeights(patch.x + 1, patch.y) : seam == 3
                ? seamHeights(patch.x, patch.y - 1) : nullptr;
            const float topLeft = gridHeight(patch.x, patch.y);
            const float topRight = gridHeight(patch.x + 1, patch.y);
            const float bottomLeft = gridHeight(patch.x, patch.y + 1);
            const float bottomRight = gridHeight(patch.x + 1, patch.y + 1);
            const uint32_t base = static_cast<uint32_t>(vertices.size());
            for (int row = 0; row < patchDepth; ++row)
                for (int col = 0; col < patchWidth; ++col)
                {
                    const float u = static_cast<float>(col) / manager->cellsX;
                    const float v = static_cast<float>(row) / manager->cellsY;
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
                                row * patchWidth + manager->cellsX)];
                        else if (seam == 1 && row == manager->cellsY)
                            height = (*nearHeights)[static_cast<size_t>(col)];
                        else if (seam == 2 && col == manager->cellsX)
                            height = (*nearHeights)[static_cast<size_t>(
                                row * patchWidth)];
                        else if (seam == 3 && row == 0)
                            height = (*nearHeights)[static_cast<size_t>(
                                manager->cellsY * patchWidth + col)];
                    }
                    Engine::Components::Mesh::Vertex vertex{};
                    vertex.pos[0] = (patch.x - tile.startX) * spanX +
                        col * manager->cellWidth;
                    vertex.pos[1] = height;
                    vertex.pos[2] = (patch.y - tile.startY) * spanZ +
                        row * manager->cellDepth;
                    const glm::vec3 normal = glm::normalize(glm::vec3(
                        (sample(col - 1, row) - sample(col + 1, row)) /
                            (2.f * manager->cellWidth), 1.f,
                        (sample(col, row - 1) - sample(col, row + 1)) /
                            (2.f * manager->cellDepth)));
                    vertex.normal[0] = normal.x;
                    vertex.normal[1] = normal.y;
                    vertex.normal[2] = normal.z;
                    vertex.uv[0] = u; vertex.uv[1] = v;
                    setColor(vertex, height);
                    vertices.push_back(vertex);
                }
            for (int row = 0; row < manager->cellsY; ++row)
                for (int col = 0; col < manager->cellsX; ++col)
                {
                    const uint32_t a = base + row * patchWidth + col;
                    const uint32_t b = a + patchWidth;
                    indices.insert(indices.end(),
                        {a, b, a + 1, a + 1, b, b + 1});
                }
        }
        if (indices.empty())
        {
            if (tile.object) tile.object->enabled = false;
            m_farTileRebuildMilliseconds += std::chrono::duration<double,
                std::milli>(std::chrono::steady_clock::now() - rebuildStart).count();
            continue;
        }
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
            tile.mesh = tile.object->AddComponent<Engine::Components::Mesh>();
            auto* material = tile.object->AddComponent<
                Engine::Components::Material>();
            material->diffuseColor = {1.f, 1.f, 1.f};
            material->unlit = true;
            material->castsShadows = false;
            tile.object->AddComponent<
                Engine::Components::TerrainBatchAppearance>()->distantVisual = true;
        }
        const uint64_t uploadBytes =
            vertices.size() * sizeof(Engine::Components::Mesh::Vertex) +
            indices.size() * sizeof(uint32_t);
        if (!tile.mesh->SetIndexedGeometry(
            std::move(vertices), std::move(indices)))
            throw std::runtime_error("Could not build distant terrain tile");
        if (auto* provider = scene->GetGraphicsProvider())
            if (!tile.mesh->IsReady()) tile.mesh->OnAfterDeserialize(provider);
        tile.object->enabled = true;
        ++m_farTileUploadCount;
        m_farTileUploadedBytes += uploadBytes;
        m_farTileRebuildMilliseconds += std::chrono::duration<double,
            std::milli>(std::chrono::steady_clock::now() - rebuildStart).count();
    }
    m_farCenterX = centerX;
    m_farCenterY = centerY;
    m_farCenterValid = true;
}

void TerrainPerformanceDriver::OnAfterDeserialize(IGraphicsProvider*)
{
    if (!ConfigureTerrain() || !Owner) return;
    auto* manager = Owner->GetComponent<Engine::Components::TerrainManager>();
    if (!manager || m_previewLoaded) return;
    // Scene editing does not run the play-mode streaming loop. Load only the
    // nearby saved cells so the map is visible as soon as the scene opens.
    const int previousFarRadius = manager->farRadius;
    manager->farRadius = manager->loadedRadius;
    manager->SyncLoadedCells(0, -2);
    manager->farRadius = previousFarRadius;
    if (auto* scene = Owner->GetScene())
    {
        scene->editorCamera.transform.position = {0.f, 180.f, -300.f};
        if (auto* camera = scene->editorCamera.GetComponent<Engine::Components::Camera>())
        {
            camera->target = {0.f, 0.f, 0.f};
            camera->farPlane = 1000.f;
        }
    }
    m_previewLoaded = true;
}

void TerrainPerformanceDriver::Update()
{
    if (!Owner) return;
    auto* manager = Owner->GetComponent<Engine::Components::TerrainManager>();
    if (!manager) return;
    // A running scene uses its startup snapshot. Authored changes take effect
    // when the scene is restarted and the saved archive/cache is reopened.
    if (Owner->GetScene() && !Owner->GetScene()->HasStarted() &&
        SourceSignature() != m_configuredSignature && !ConfigureTerrain()) return;
    const float spanX = manager->cellsX * manager->cellWidth;
    const float spanZ = manager->cellsY * manager->cellDepth;
    if (Owner->GetScene() && Owner->GetScene()->HasStarted())
    {
        if (!m_farAttempted)
        {
            m_farAttempted = true;
            TerrainHeightArchive::ReadFarGrid(
                Engine::Components::Mesh::ResolveFilePath(farGrid),
                m_configuredSignature, mapHalfExtentCells, m_farHeights,
                &m_farPatches);
        }
        if (!m_farHeights.empty())
            if (auto* viewer = Owner->FindObjectInSceneByName(viewerObjectName))
            {
                const glm::dvec3 local = glm::dvec3(
                    glm::inverse(Owner->transform.GetWorldMatrixD()) *
                    glm::dvec4(viewer->transform.GetWorldPositionD(), 1.0));
                const int centerX = static_cast<int>(std::floor(local.x / spanX));
                const int centerY = static_cast<int>(std::floor(local.z / spanZ));
                if (!m_farCenterValid || centerX != m_farCenterX ||
                    centerY != m_farCenterY)
                    RefreshFarTerrain(centerX, centerY);
            }
    }
    m_peakLoaded = std::max(m_peakLoaded, manager->GetLoadedCellCount());
    m_reportElapsed += Owner->GetScene()
        ? std::max(0.f, Owner->GetScene()->GetDeltaTime()) : 0.f;
    if (m_reportElapsed >= std::max(.1f, reportIntervalSeconds))
    {
        std::cout << "[Terrain performance] saved=" << manager->GetCellCount()
            << " loaded=" << manager->GetLoadedCellCount()
            << " peakLoaded=" << m_peakLoaded
            << " nearRadiusCells=" << manager->loadedRadius
            << " farRadiusCells=" << std::max(manager->loadedRadius, manager->farRadius)
            << " viewDistanceMeters=" << std::max(manager->loadedRadius,
                manager->farRadius) * std::max(spanX, spanZ)
            << '\n';
        m_reportElapsed = 0.f;
    }
}

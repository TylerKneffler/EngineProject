#include "Scripts/TerrainPerformance/TerrainPerformanceDriver.h"
#include "Core/Compoonents/Obj/TerrainHeightArchive.h"
#include "Core/Compoonents/Obj/TerrainManager.h"
#include "Core/Compoonents/Obj/Mesh.h"
#include "Core/Compoonents/Camera/Camera.h"
#include "Core/Object.h"
#include "Core/Scene/Scene.h"
#include "Core/Serialization/SceneSerializer.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <filesystem>
#include <chrono>
#include <future>
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
    RegisterField("enableFarTerrain", enableFarTerrain);
    RegisterField("reportIntervalSeconds", reportIntervalSeconds);
}

namespace
{
constexpr std::array<float, TerrainHeightArchive::LayerCount - 1>
    TerrainMaterialBoundaries{-36.f, -24.f, -12.f};
constexpr float TerrainMaterialBlendHalfWidth = 4.f;

std::vector<float> MakeNearWeights(const std::vector<float>& heights,
    const std::vector<float>& archivedWeights)
{
    std::vector<float> nearWeights(heights.size() * 6, 0.f);
    if (archivedWeights.size() !=
        heights.size() * TerrainHeightArchive::LayerCount)
        return nearWeights;
    for (size_t i = 0; i < heights.size(); ++i)
    {
        float expected[TerrainHeightArchive::LayerCount];
        float bands[TerrainHeightArchive::LayerCount];
        TerrainHeightArchive::BlendBaseLayers(heights[i],
            TerrainHeightArchive::LayerForHeight(heights[i]), expected);
        TerrainHeightArchive::BlendHeightBands(heights[i],
            TerrainMaterialBoundaries, TerrainMaterialBlendHalfWidth, bands);
        // Keep archive paint that differs from its original height bands.
        bool painted = false;
        for (int layer = 0; layer < TerrainHeightArchive::LayerCount; ++layer)
            painted |= std::abs(archivedWeights[i * 4 + layer] -
                expected[layer]) > .01f;
        const float* source = painted ? archivedWeights.data() + i * 4 : bands;
        nearWeights[i * 6 + 4] = source[0];
        nearWeights[i * 6 + 5] = source[1];
        nearWeights[i * 6 + 2] = source[2];
        nearWeights[i * 6 + 3] = source[3];
    }
    return nearWeights;
}

struct TerrainPerformanceDriverRegistration
{
    TerrainPerformanceDriverRegistration()
    {
        Engine::Serialization::RegisterComponentType<TerrainPerformanceDriver>(
            "TerrainPerformanceDriver");
    }
} registration;
Engine::Components::TerrainManager::FarTerrainPalette MakeFarPalette()
{
    Engine::Components::TerrainManager::FarTerrainPalette palette;
    palette.farColors = {{
        {.04926f, .15254f, .03097f},
        {.04270f, .09440f, .02921f},
        {.04561f, .07992f, .03262f},
        {.07032f, .07998f, .06848f}}};
    palette.nearColors = {{
        {.05856f, .04472f, .02751f},
        {.19217f, .11348f, .05328f},
        {.04561f, .07992f, .03262f},
        {.07032f, .07998f, .06848f},
        {.04927f, .15253f, .03096f},
        {.04270f, .09440f, .02921f}}};
    palette.archiveToNear = {4, 5, 2, 3};
    palette.heightBoundaries = TerrainMaterialBoundaries;
    palette.heightBlendHalfWidth = TerrainMaterialBlendHalfWidth;
    return palette;
}
}

TerrainPerformanceDriver::FarGridLoad
TerrainPerformanceDriver::LoadFarGridData(const std::string& path,
    const std::string& archivePath, const std::string& editDirectory,
    const std::vector<std::pair<int, int>>& editedCells,
    uint64_t signature, int extent, int cellsX, int cellsY)
{
    FarGridLoad result;
    result.loaded = TerrainHeightArchive::ReadFarGrid(path, signature,
        extent, result.heights, &result.patches);
    if (!result.loaded) return result;
    for (const auto& cell : editedCells)
    {
        std::vector<float> heights, archivedWeights;
        if (!TerrainHeightArchive::ReadCell(archivePath, signature,
            extent, cellsX, cellsY, cell.first, cell.second, heights,
            &archivedWeights)) continue;
        std::vector<float> nearWeights = MakeNearWeights(heights,
            archivedWeights);
        const auto editPath = std::filesystem::path(editDirectory) /
            (std::to_string(cell.first) + "_" +
                std::to_string(cell.second) + ".terrainedit");
        if (!TerrainHeightArchive::ApplyEditSurface(editPath.string(),
            cellsX, cellsY, heights, &nearWeights, 6)) continue;
        const auto palette = MakeFarPalette();
        std::vector<float> colors(heights.size() * 3, 0.f);
        for (size_t i = 0; i < heights.size(); ++i)
            for (int layer = 0; layer < 6; ++layer)
                for (int channel = 0; channel < 3; ++channel)
                    colors[i * 3 + channel] +=
                        nearWeights[i * 6 + layer] *
                        palette.nearColors[layer][channel];
        auto found = std::find_if(result.patches.begin(), result.patches.end(),
            [&](const auto& patch) {
                return patch.x == cell.first && patch.y == cell.second;
            });
        if (found == result.patches.end())
            result.patches.push_back({cell.first, cell.second,
                std::move(heights), std::move(colors)});
        else
        {
            found->heights = std::move(heights);
            found->colors = std::move(colors);
        }
    }
    return result;
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
    manager->SetStaticCache(cacheDirectory, signature ^ 0x4c41524745555636ull);
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
            {"Assets/Materials/Terrain/shaded_moss.material", -24.f, -12.f,
                {.23f, .31f, .19f}},
            {"Assets/Materials/Terrain/weathered_slate.material", -12.f, 1000.f,
                {.29f, .31f, .28f}},
            {"Assets/Materials/Terrain/valley_grass.material", -1000.f, -36.f,
                {.23f, .41f, .17f}},
            {"Assets/Materials/Terrain/woodland_grass.material", -36.f, -24.f,
                {.21f, .33f, .17f}}
        };
        std::vector<float> archivedWeights;
        if (!TerrainHeightArchive::ReadCell(archivePath, signature, extent,
            countX, countY, cell.first, cell.second, terrain.heights,
            &archivedWeights))
            throw std::runtime_error("Could not read saved terrain height cell");
        terrain.weights = MakeNearWeights(terrain.heights, archivedWeights);
        terrain.Rebuild();
    }} : std::function<void(Engine::Components::TerrainManager::Cell,
        Engine::Components::Terrain&)>{});
    m_configuredSignature = signature;
    manager->ClearFarTerrain();
    m_farLoad = {};
    m_farAttempted = false;
    m_farDataReady = false;
    return true;
}

void TerrainPerformanceDriver::Start()
{
    // Play mode refreshes the full-map far grid asynchronously. The camera
    // script waits for this snapshot before leaving its start position.
    m_farLoad = {};
    m_farAttempted = false;
    m_farDataReady = !enableFarTerrain || farGrid.empty();
}

bool TerrainPerformanceDriver::IsInitialMapDataReady() const
{
    if (!m_farDataReady || !Owner) return false;
    const auto* manager = Owner->GetComponent<Engine::Components::TerrainManager>();
    return manager && (!enableFarTerrain || farGrid.empty() ||
        (manager->AreDesiredFarTilesBuilt() &&
            manager->GetPreparingFarTileCount() == 0u &&
            manager->GetQueuedFarTileCount() == 0u &&
            manager->GetPreparedFarTileCount() == 0u));
}

std::vector<Engine::Components::Mesh*>
TerrainPerformanceDriver::GetFarTileMeshes() const
{
    const auto* manager = Owner
        ? Owner->GetComponent<Engine::Components::TerrainManager>() : nullptr;
    return manager ? manager->GetFarTileMeshes()
        : std::vector<Engine::Components::Mesh*>{};
}

uint64_t TerrainPerformanceDriver::GetFarTileUploadCount() const
{
    const auto* manager = Owner
        ? Owner->GetComponent<Engine::Components::TerrainManager>() : nullptr;
    return manager ? manager->GetFarTileUploadCount() : 0;
}

uint64_t TerrainPerformanceDriver::GetFarTileUploadedBytes() const
{
    const auto* manager = Owner
        ? Owner->GetComponent<Engine::Components::TerrainManager>() : nullptr;
    return manager ? manager->GetFarTileUploadedBytes() : 0;
}

double TerrainPerformanceDriver::GetFarTileRebuildMilliseconds() const
{
    const auto* manager = Owner
        ? Owner->GetComponent<Engine::Components::TerrainManager>() : nullptr;
    return manager ? manager->GetFarTileRebuildMilliseconds() : 0.0;
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
    if (enableFarTerrain && !farGrid.empty())
    {
        FarGridLoad result = LoadFarGridData(
            Engine::Components::Mesh::ResolveFilePath(farGrid),
            Engine::Components::Mesh::ResolveFilePath(heightArchive),
            Engine::Components::Mesh::ResolveFilePath(manager->cellDataDirectory),
            manager->GetEditedProceduralCells(), m_configuredSignature,
            mapHalfExtentCells, manager->cellsX, manager->cellsY);
        if (result.loaded)
        {
            manager->ConfigureFarTerrain(mapHalfExtentCells,
                std::move(result.heights), std::move(result.patches),
                Engine::Components::Mesh::ResolveFilePath(heightArchive),
                m_configuredSignature, MakeFarPalette());
            m_farDataReady = true;
        }
    }
    if (auto* scene = Owner->GetScene())
    {
        scene->editorCamera.transform.position = {0.f, 180.f, -300.f};
        if (auto* camera = scene->editorCamera.GetComponent<Engine::Components::Camera>())
        {
            camera->target = {0.f, 0.f, 0.f};
            camera->farPlane = enableFarTerrain ? 3218.f : 1000.f;
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
    Engine::Core::Object* viewer =
        Owner->FindObjectInSceneByName(viewerObjectName);
    glm::dvec3 viewerWorld(0.0);
    glm::dvec3 viewerLocal(0.0);
    int viewerCellX = 0, viewerCellY = 0;
    bool viewerCellValid = false;
    if (viewer)
    {
        viewerWorld = viewer->transform.GetWorldPositionD();
        viewerLocal = glm::dvec3(glm::inverse(Owner->transform.GetWorldMatrixD()) *
            glm::dvec4(viewerWorld, 1.0));
        viewerCellX = static_cast<int>(std::floor(viewerLocal.x / spanX));
        viewerCellY = static_cast<int>(std::floor(viewerLocal.z / spanZ));
        viewerCellValid = std::isfinite(viewerLocal.x) &&
            std::isfinite(viewerLocal.z);
    }
    if (Owner->GetScene() && Owner->GetScene()->HasStarted())
    {
        if (!enableFarTerrain)
        {
            manager->SetFarTerrainEnabled(false);
        }
        else
        {
            if (!m_farAttempted)
            {
                m_farAttempted = true;
                const std::string path =
                    Engine::Components::Mesh::ResolveFilePath(farGrid);
                const uint64_t signature = m_configuredSignature;
                const int extent = mapHalfExtentCells;
                const std::string archivePath =
                    Engine::Components::Mesh::ResolveFilePath(heightArchive);
                const std::string editDirectory =
                    Engine::Components::Mesh::ResolveFilePath(
                        manager->cellDataDirectory);
                const auto editedCells = manager->GetEditedProceduralCells();
                const int cellsX = manager->cellsX, cellsY = manager->cellsY;
                m_farLoad = std::async(std::launch::async,
                    [path, archivePath, editDirectory, editedCells,
                        signature, extent, cellsX, cellsY] {
                        return LoadFarGridData(path, archivePath,
                            editDirectory, editedCells, signature, extent,
                            cellsX, cellsY);
                    });
            }
            if (m_farLoad.valid() &&
                m_farLoad.wait_for(std::chrono::seconds(0)) ==
                    std::future_status::ready)
            {
                FarGridLoad result = m_farLoad.get();
                if (result.loaded)
                {
                    manager->ConfigureFarTerrain(mapHalfExtentCells,
                        std::move(result.heights), std::move(result.patches),
                        Engine::Components::Mesh::ResolveFilePath(heightArchive),
                        m_configuredSignature, MakeFarPalette());
                    m_farDataReady = true;
                }
            }
            manager->SetFarTerrainEnabled(true);
            if (viewerCellValid)
                manager->UpdateFarTerrain(viewerCellX, viewerCellY,
                    viewer ? viewer->GetComponent<Engine::Components::Camera>() : nullptr);
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
                manager->farRadius) * std::max(spanX, spanZ);
        if (viewer)
        {
            const bool hasCell = viewerCellValid &&
                manager->HasCell(viewerCellX, viewerCellY);
            const bool loadedCell = viewerCellValid &&
                manager->GetLoadedCell(viewerCellX, viewerCellY) != nullptr;
            double groundY = 0.0;
            const bool sampledHeight =
                manager->TryGetHeightAtWorldPosition(viewerWorld, groundY);
            std::cout << " viewerWorld=(" << viewerWorld.x << ','
                << viewerWorld.y << ',' << viewerWorld.z << ')'
                << " viewerLocal=(" << viewerLocal.x << ','
                << viewerLocal.y << ',' << viewerLocal.z << ')'
                << " viewerCell=(" << viewerCellX << ',' << viewerCellY << ')'
                << " cellInBounds=" << (hasCell ? "true" : "false")
                << " cellLoaded=" << (loadedCell ? "true" : "false")
                << " sampledHeight=" << (sampledHeight ? "true" : "false");
            if (sampledHeight)
                std::cout << " groundY=" << groundY
                    << " clearanceY=" << (viewerWorld.y - groundY);
        }
        else
            std::cout << " viewerMissing=true";
        std::cout
            << '\n';
        m_reportElapsed = 0.f;
    }
}

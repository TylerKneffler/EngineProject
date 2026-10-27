// Run with TERRAIN_TEST_VIEWPOINT=low|ridge|edge. For a disk-cache pair,
// set TERRAIN_TEST_CACHE_DIRECTORY to an empty benchmark-owned directory,
// run once with TERRAIN_TEST_CACHE_MODE=cold, then again with mode=warm.
// Optional view-distance controls:
// TERRAIN_TEST_VIEW_DISTANCE_METERS, TERRAIN_TEST_VIEW_DISTANCE_CELLS,
// TERRAIN_TEST_LOADED_RADIUS_CELLS, TERRAIN_TEST_LOD2_RADIUS_CELLS,
// TERRAIN_TEST_LOD4_RADIUS_CELLS.
// Reports are written under .local/performance-logs.
#include "Core/Scene/Scene.h"
#include "Core/Compoonents/Obj/TerrainManager.h"
#include "Core/Compoonents/Camera/Camera.h"
#include "Core/Compoonents/Lighting/Light.h"
#include "Core/Renderers/DX11/DX11GameRenderer.h"
#include "Core/Window.h"
#include "Scripts/TerrainPerformance/TerrainPerformanceDriver.h"
#include <Windows.h>
#include <Psapi.h>
#include <wincodec.h>
#include <wrl/client.h>
#pragma comment(lib, "Psapi.lib")
#pragma comment(lib, "windowscodecs.lib")
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <numeric>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>

namespace
{
using Clock = std::chrono::steady_clock;
constexpr unsigned Width = 1920, Height = 1080;
constexpr double MiB = 1024.0 * 1024.0;
void Require(bool condition, const char* message);

bool SavePng(const std::filesystem::path& path,
    const std::vector<uint8_t>& rgba)
{
    if (rgba.size() != static_cast<size_t>(Width) * Height * 4) return false;
    const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(init) && init != RPC_E_CHANGED_MODE) return false;
    Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
    Microsoft::WRL::ComPtr<IWICStream> stream;
    Microsoft::WRL::ComPtr<IWICBitmapEncoder> encoder;
    Microsoft::WRL::ComPtr<IWICBitmapFrameEncode> frame;
    Microsoft::WRL::ComPtr<IPropertyBag2> properties;
    bool saved = false;
    do
    {
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
            CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) ||
            FAILED(factory->CreateStream(&stream)) ||
            FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) ||
            FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr,
                &encoder)) ||
            FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) ||
            FAILED(encoder->CreateNewFrame(&frame, &properties)) ||
            FAILED(frame->Initialize(properties.Get())) ||
            FAILED(frame->SetSize(Width, Height))) break;
        WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
        std::vector<uint8_t> bgra = rgba;
        for (size_t pixel = 0; pixel < bgra.size(); pixel += 4)
            std::swap(bgra[pixel], bgra[pixel + 2]);
        if (FAILED(frame->SetPixelFormat(&format)) ||
            !IsEqualGUID(format, GUID_WICPixelFormat32bppBGRA) ||
            FAILED(frame->WritePixels(Height, Width * 4u,
                static_cast<UINT>(bgra.size()), bgra.data())) ||
            FAILED(frame->Commit()) || FAILED(encoder->Commit())) break;
        saved = true;
    } while (false);
    properties.Reset(); frame.Reset(); encoder.Reset(); stream.Reset();
    factory.Reset();
    if (SUCCEEDED(init)) CoUninitialize();
    return saved;
}

int ReadIntSetting(const char* name, int fallback, int minimum, int maximum)
{
    char buffer[32]{};
    const DWORD length = GetEnvironmentVariableA(name, buffer, sizeof(buffer));
    if (length == 0) return fallback;
    Require(length < sizeof(buffer), "terrain benchmark setting is too long");
    char* end = nullptr;
    const long value = std::strtol(buffer, &end, 10);
    Require(end != buffer && *end == '\0' && value >= minimum &&
        value <= maximum, "invalid terrain benchmark setting");
    return static_cast<int>(value);
}

std::optional<int> ReadOptionalIntSetting(const char* name,
    int minimum, int maximum)
{
    char buffer[32]{};
    const DWORD length = GetEnvironmentVariableA(name, buffer, sizeof(buffer));
    if (length == 0) return std::nullopt;
    Require(length < sizeof(buffer), "terrain benchmark setting is too long");
    char* end = nullptr;
    const long value = std::strtol(buffer, &end, 10);
    Require(end != buffer && *end == '\0' && value >= minimum &&
        value <= maximum, "invalid terrain benchmark setting");
    return static_cast<int>(value);
}

void Require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

double Milliseconds(Clock::time_point start, Clock::time_point end)
{
    return std::chrono::duration<double, std::milli>(end - start).count();
}

double Percentile(std::vector<double> values, double fraction)
{
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    const size_t index = static_cast<size_t>(std::ceil(fraction * values.size())) - 1;
    return values[std::min(index, values.size() - 1)];
}

double PhasePercentile(const std::vector<double>& values,
    size_t begin, size_t end, double fraction)
{
    return Percentile(std::vector<double>(values.begin() + begin,
        values.begin() + end), fraction);
}

struct Samples
{
    std::vector<double> frames, streaming, managerCpu, farTileCpu,
        renderCpu, prepareCpu, sceneRenderCpu, frameEndCpu,
        gpuTotal, gpuTerrain, batchRefreshCpu, batchBuildCpu;
    std::vector<double> visibleDistances;
    double worstStreaming = 0.0, visibleDistance = 0.0;
    uint64_t peakTerrainCpu = 0, peakTerrainGpu = 0, peakCache = 0;
    uint64_t peakTerrainTextureBytes = 0;
    uint64_t peakWorkingSet = 0, peakPrivate = 0;
    size_t peakResident = 0;
    uint32_t peakDraws = 0;
    std::vector<double> draws, terrainOcclusionCulls,
        submittedTriangles, objectUploadKiB,
        residentCells;
    std::array<std::vector<double>,
        static_cast<size_t>(Engine::Graphics::GpuTimingStage::Count)> gpuStages;
    uint64_t lastGpuSampleId = 0;
};

struct TemporaryCache
{
    std::filesystem::path path;
    bool removeOnExit = true;
    ~TemporaryCache()
    {
        if (!removeOnExit) return;
        std::error_code error;
        const auto parent = std::filesystem::weakly_canonical(".local/cache", error);
        if (error) return;
        const auto target = std::filesystem::weakly_canonical(path, error);
        if (!error && target.parent_path() == parent)
            std::filesystem::remove_all(target, error);
    }
};

std::string ReadStringSetting(const char* name, const char* fallback)
{
    const DWORD length = GetEnvironmentVariableA(name, nullptr, 0);
    if (!length) return fallback;
    std::string value(length, '\0');
    const DWORD copied = GetEnvironmentVariableA(name, value.data(), length);
    Require(copied > 0 && copied < length, "invalid terrain benchmark setting");
    value.resize(copied);
    return value;
}

void SampleMemory(Samples& samples,
    Engine::Components::TerrainManager& manager)
{
    const auto memory = manager.GetMemoryUsage();
    samples.peakTerrainCpu = std::max(samples.peakTerrainCpu, memory.cpuBytes);
    samples.peakTerrainGpu = std::max(samples.peakTerrainGpu, memory.gpuBytes);
    samples.peakCache = std::max(samples.peakCache, memory.cacheBytes);
    samples.peakTerrainTextureBytes = std::max(
        samples.peakTerrainTextureBytes,
        manager.GetTerrainTextureMemoryBytes());
    samples.peakResident = std::max(samples.peakResident,
        manager.GetLoadedCellCount());
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (GetProcessMemoryInfo(GetCurrentProcess(),
        reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters)))
    {
        samples.peakWorkingSet = std::max(samples.peakWorkingSet,
            static_cast<uint64_t>(counters.WorkingSetSize));
        samples.peakPrivate = std::max(samples.peakPrivate,
            static_cast<uint64_t>(counters.PrivateUsage));
    }
}

double VisibleCellCenterDistance(
    const Engine::Components::TerrainManager& manager,
    const Engine::Components::Camera& camera,
    const std::vector<Engine::Components::TerrainManager::Cell>& cells)
{
    const glm::dmat4 viewProjection =
        camera.GetProjectionMatrixD(static_cast<double>(Width) / Height) *
        camera.GetViewMatrixD();
    const glm::dvec3 eye = camera.Owner->transform.GetWorldPositionD();
    const double halfX = .5 * manager.cellsX * manager.cellWidth;
    const double halfZ = .5 * manager.cellsY * manager.cellDepth;
    double farthest = 0.0;
    for (const auto cell : cells)
    {
        const auto* terrain = manager.GetLoadedCell(cell.first, cell.second);
        if (!terrain) continue;
        const glm::dvec3 center = terrain->Owner->transform.GetWorldPositionD() +
            glm::dvec3(halfX, 0.0, halfZ);
        if (glm::distance(center, eye) > camera.farPlane) continue;
        const glm::dvec4 clip = viewProjection * glm::dvec4(center, 1.0);
        if (clip.w <= 0.0 || std::abs(clip.x) > clip.w ||
            std::abs(clip.y) > clip.w || clip.z < 0.0 || clip.z > clip.w)
            continue;
        farthest = std::max(farthest, glm::length(glm::dvec2(
            center.x - eye.x, center.z - eye.z)));
    }
    return farthest;
}
}

int Run()
{
    SetErrorMode(SEM_NOGPFAULTERRORBOX);
    int farPlaneMeters = ReadIntSetting(
        "TERRAIN_TEST_FAR_PLANE_METERS", 3218, 1000, 50000);
    int farRadiusCells = ReadIntSetting(
        "TERRAIN_TEST_FAR_RADIUS_CELLS", 8, 1, 32);
    if (const auto viewDistanceMeters = ReadOptionalIntSetting(
        "TERRAIN_TEST_VIEW_DISTANCE_METERS", 1000, 50000))
        farPlaneMeters = *viewDistanceMeters;
    if (const auto viewDistanceCells = ReadOptionalIntSetting(
        "TERRAIN_TEST_VIEW_DISTANCE_CELLS", 1, 32))
        farRadiusCells = *viewDistanceCells;
    const int loadedRadiusCells = ReadIntSetting(
        "TERRAIN_TEST_LOADED_RADIUS_CELLS", 2, 0, 16);
    int lod2RadiusCells = ReadIntSetting(
        "TERRAIN_TEST_LOD2_RADIUS_CELLS", 8, 0, 32);
    int lod4RadiusCells = ReadIntSetting(
        "TERRAIN_TEST_LOD4_RADIUS_CELLS", 12, 0, 32);
    lod2RadiusCells = std::max(lod2RadiusCells, loadedRadiusCells);
    lod4RadiusCells = std::max(lod4RadiusCells, lod2RadiusCells);
    farRadiusCells = std::max(farRadiusCells, loadedRadiusCells);
    const std::string viewpoint = ReadStringSetting(
        "TERRAIN_TEST_VIEWPOINT", "ridge");
    Require(viewpoint == "low" || viewpoint == "ridge" ||
        viewpoint == "edge", "unknown TERRAIN_TEST_VIEWPOINT");
    const std::string cacheMode = ReadStringSetting(
        "TERRAIN_TEST_CACHE_MODE", "cold");
    const std::string runtimeMode = ReadStringSetting(
        "TERRAIN_TEST_RUNTIME_MODE", "baked");
    Require(runtimeMode == "baked" || runtimeMode == "legacy",
        "TERRAIN_TEST_RUNTIME_MODE must be baked or legacy");
    const std::string headingMode = ReadStringSetting(
        "TERRAIN_TEST_HEADING_MODE", "oscillate");
    Require(headingMode == "oscillate" || headingMode == "sweep",
        "TERRAIN_TEST_HEADING_MODE must be oscillate or sweep");
    Require(cacheMode == "cold" || cacheMode == "warm",
        "TERRAIN_TEST_CACHE_MODE must be cold or warm");
    const std::string persistentCache = ReadStringSetting(
        "TERRAIN_TEST_CACHE_DIRECTORY", "");
    Require(runtimeMode != "legacy" || cacheMode != "warm" ||
        !persistentCache.empty(),
        "warm cache mode requires TERRAIN_TEST_CACHE_DIRECTORY");
    if (runtimeMode == "legacy" && cacheMode == "warm")
        Require(std::filesystem::is_directory(persistentCache) &&
            !std::filesystem::is_empty(persistentCache),
            "warm terrain cache directory is missing or empty");
    Engine::Core::Window window(GetModuleHandleW(nullptr),
        L"Terrain 1080p performance test", Width, Height);
    Engine::Renderers::DX11GameRenderer renderer;
    if (!renderer.Init(window.GetHWND(), Width, Height))
    {
        std::cout << "SKIP: DX11 unavailable\n";
        return 0;
    }
    const bool captureImages =
        GetEnvironmentVariableA("TERRAIN_CAPTURE_IMAGES", nullptr, 0) > 0;
    const bool captureWithoutBatches = captureImages &&
        GetEnvironmentVariableA("TERRAIN_CAPTURE_DISABLE_BATCHING", nullptr, 0) > 0;
    if (captureImages)
        Require(renderer.EnableOffscreenExport(Width, Height, 1, 2),
            "could not enable terrain image capture");
    // Destroy the cache after the scene has stopped its streaming workers.
    const TemporaryCache cache{
        persistentCache.empty()
            ? std::filesystem::path(".local/cache") /
                ("terrain_streaming_test_" + std::to_string(GetCurrentProcessId()) +
                    "_" + std::to_string(Clock::now().time_since_epoch().count()))
            : std::filesystem::path(persistentCache),
        persistentCache.empty()};
    if (runtimeMode == "legacy" && cacheMode == "cold" &&
        !persistentCache.empty())
    {
        // Only remove a cache owned by this benchmark, never an arbitrary path.
        Require(!std::filesystem::exists(cache.path) ||
            std::filesystem::is_empty(cache.path),
            "cold terrain cache directory must be empty");
    }
    Engine::Scene::Scene scene;
    scene.Init(renderer.GetGraphicsProvider());
    Require(scene.Load("Engine/Core/Assets/Scenes/terrain/terrain_performance.scene"),
        "could not load saved terrain performance scene");
    auto* map = scene.FindObjectByName("Terrain Performance Map");
    auto* cameraObject = scene.FindObjectByName("Terrain Performance Camera");
    Require(map && cameraObject, "terrain benchmark scene is incomplete");
    auto* manager = map->GetComponent<Engine::Components::TerrainManager>();
    auto* driver = map->GetComponent<TerrainPerformanceDriver>();
    auto* camera = cameraObject->GetComponent<Engine::Components::Camera>();
    Require(manager && driver && camera &&
        manager->requireRuntimeBake, "terrain benchmark components missing");
    Require(manager->GetCellCount() == 16641,
        "terrain benchmark must use the saved full map");
    const bool farFieldEnabled = GetEnvironmentVariableA(
        "TERRAIN_DISABLE_FAR_FIELD", nullptr, 0) == 0;
    const bool shadowsEnabled = GetEnvironmentVariableA(
        "TERRAIN_TEST_DISABLE_SHADOWS", nullptr, 0) == 0;
    const bool sunEnabled = GetEnvironmentVariableA(
        "TERRAIN_TEST_DISABLE_SUN", nullptr, 0) == 0;
    const bool unlit = GetEnvironmentVariableA(
        "TERRAIN_TEST_UNLIT", nullptr, 0) > 0;
    const bool stochasticVariation = GetEnvironmentVariableA(
        "TERRAIN_TEST_STOCHASTIC", nullptr, 0) > 0;
    manager->stochasticTerrainVariation = stochasticVariation;
    const bool macroColorEnabled = GetEnvironmentVariableA(
        "TERRAIN_TEST_MACRO_COLOR", nullptr, 0) > 0;
    manager->terrainMacroColorEnabled = macroColorEnabled;
    if (unlit) scene.settings.renderMode = Engine::Model::SceneRenderMode::Unlit;
    if (!shadowsEnabled)
        if (auto* sun = scene.FindObjectByName("Terrain Performance Sun"))
            if (auto* light = sun->GetComponent<Engine::Components::Light>())
                light->castsShadows = false;
    if (!sunEnabled)
        if (auto* sun = scene.FindObjectByName("Terrain Performance Sun"))
            sun->enabled = false;
    if (!farFieldEnabled) driver->farGrid.clear();
    camera->farPlane = static_cast<float>(farPlaneMeters);
    // Without a caller-owned cache path, each run gets a fresh cache. Pass
    // the same directory to separate cold and warm process runs to compare
    // disk-cache behavior without deleting another run's data.
    driver->cacheDirectory = cache.path.string();
    Require(driver->ConfigureTerrain(), "could not configure test-owned mesh cache");
    if (runtimeMode == "baked")
    {
        if (!manager->HasCompleteRuntimeBake())
        {
            std::cerr << "Runtime terrain bake missing or incomplete; baking "
                << manager->GetCellCount() << " cells before benchmark...\n";
            Require(manager->BakeRuntimeTerrain(true),
                "could not bake the full runtime terrain map");
        }
        Require(manager->HasCompleteRuntimeBake(),
            "runtime terrain bake is incomplete");
    }
    else
    {
        manager->requireRuntimeBake = false;
        manager->runtimeBakeDirectory.clear();
    }

    // Preload the first viewpoint outside measured frames. The camera clip
    // and streamed radius vary independently, so a long clip plane cannot
    // imply coverage.
    manager->loadedRadius = loadedRadiusCells;
    if (captureWithoutBatches) manager->batchFarCells = false;
    manager->farRadius = farRadiusCells;
    manager->lod2Radius = lod2RadiusCells;
    manager->lod4Radius = lod4RadiusCells;
    manager->maxLoadsPerUpdate = 8;
    manager->maxUnloadsPerUpdate = 32;
    manager->maxPreparedCells = 16;
    manager->maxUploadsPerUpdate = 4;
    const int initialCell = viewpoint == "edge" ? 61 : -60;
    if (runtimeMode == "baked") scene.Start();
    manager->SyncLoadedCells(initialCell, initialCell);
    // SyncLoadedCells preloads the viewpoint; measured frames use the scene's
    // actual streaming limits rather than the temporary preload limits.
    manager->maxLoadsPerUpdate = 2;
    manager->maxUnloadsPerUpdate = 4;
    manager->maxPreparedCells = 4;
    manager->maxUploadsPerUpdate = 1;
    if (runtimeMode != "baked") scene.Start();
    if (captureImages)
    {
        char requestedView[32]{};
        const DWORD viewLength = GetEnvironmentVariableA(
            "TERRAIN_CAPTURE_VIEW", requestedView, sizeof(requestedView));
        Require(viewLength > 0 && viewLength < sizeof(requestedView),
            "TERRAIN_CAPTURE_VIEW must name human, ground, high, or crossing");
        const std::string viewName(requestedView, viewLength);
        const auto imageDirectory =
            std::filesystem::path(".local/performance-logs/terrain-captures");
        std::filesystem::create_directories(imageDirectory);
        const auto capture = [&](const char* name, double x, double y,
            double z, float pitch, float yaw) {
            std::cerr << "Capturing terrain view: " << name << std::endl;
            cameraObject->transform.position = {x, y, z};
            cameraObject->transform.rotation = {pitch, yaw, 0.f};
            driver->Update();
            std::cerr << "Updated terrain view: " << name << std::endl;
            renderer.BeginExportFrame(1);
            renderer.BeginFrame();
            renderer.Clear(.2f, .25f, .3f);
            auto context = renderer.CreateFrameGraphicsContext();
            scene.PrepareRenderFrame();
            scene.Render(context.get(), 1.f, camera, false,
                Width, Height, true);
            scene.Render(context.get(), 1.f, camera, false, Width, Height);
            renderer.EndFrame();
            std::cerr << "Rendered terrain view: " << name << std::endl;
            renderer.WaitIdle();
            std::cerr << "Waited for terrain view: " << name << std::endl;
            std::vector<uint8_t> pixels;
            Require(renderer.ReadExportFrameRGBA(pixels, true),
                "could not read terrain image capture");
            std::cerr << "Read terrain view: " << name << std::endl;
            if (std::string_view(name) == "range_high" && !unlit && sunEnabled)
            {
                const size_t sample =
                    (static_cast<size_t>(Height) * 2 / 3 * Width + Width / 2) * 4;
                Require(pixels[sample] + pixels[sample + 1] +
                    pixels[sample + 2] > 180,
                    "detailed terrain became dark beyond lighting distance");
                const size_t farSample =
                    (static_cast<size_t>(Height) * 5 / 12 * Width + Width / 2) * 4;
                Require(pixels[farSample] + pixels[farSample + 1] +
                    pixels[farSample + 2] > 220,
                    "distant terrain lost global directional sunlight");
                const size_t farJoin =
                    (static_cast<size_t>(Height) * 13 / 27 * Width + Width / 2) * 4;
                const size_t nearJoin =
                    (static_cast<size_t>(Height) * 19 / 36 * Width + Width / 2) * 4;
                const int farBrightness = pixels[farJoin] + pixels[farJoin + 1] +
                    pixels[farJoin + 2];
                const int nearBrightness = pixels[nearJoin] + pixels[nearJoin + 1] +
                    pixels[nearJoin + 2];
                Require(farBrightness <= nearBrightness * 1.75f + 25.f,
                    "distant terrain is brighter than nearby painted terrain");
            }
            Require(SavePng(imageDirectory / (std::string(name) +
                (captureWithoutBatches ? "_no_batches" : "") +
                (!shadowsEnabled ? "_no_shadows" : "") +
                (!sunEnabled ? "_no_sun" : "") +
                (unlit ? "_unlit" : "") +
                (stochasticVariation ? "_stochastic" : "") +
                (macroColorEnabled ? "_macro" : "") + ".png"),
                pixels), "could not save terrain PNG");
            std::cerr << "Saved terrain view: " << name << std::endl;
        };
        constexpr double viewX = -60.0 * 128.0 + 64.0;
        constexpr float viewYaw = .7853982f;
        if (viewName == "human")
            capture("range_human", viewX, -11.77, viewX,
                .04f, 2.3561945f);
        else if (viewName == "ground")
            capture("range_ground", viewX, 110.0, viewX, .06f, viewYaw);
        else if (viewName == "high")
            capture("range_high", viewX, 600.0, viewX, .18f, viewYaw);
        else if (viewName == "crossing")
        {
            manager->SyncLoadedCells(-59, -60);
            capture("cell_crossing", -60.0 * 128.0 + 128.5,
                110.0, viewX, .18f, viewYaw);
        }
        else Require(false, "unknown TERRAIN_CAPTURE_VIEW");
        return 0;
    }
    const auto allCells = manager->GetCells();
    const double viewHeight = viewpoint == "low" ? -11.77 :
        viewpoint == "ridge" ? 600.0 : 110.0;
    const float viewPitch = viewpoint == "ridge" ? .18f : .06f;
    const auto runFrame = [&](double x, double z, double yaw, Samples* samples) {
        const auto start = Clock::now();
        const double refreshBefore =
            manager->GetRenderBatchRefreshMilliseconds();
        const double buildBefore =
            manager->GetRenderBatchBuildMilliseconds();
        cameraObject->transform.position = {
            x, viewHeight, z};
        cameraObject->transform.rotation = {viewPitch, static_cast<float>(yaw), 0.f};
        manager->UpdateStreamingAtPosition(
            cameraObject->transform.GetWorldPositionD());
        const auto managed = Clock::now();
        driver->Update();
        const auto streamed = Clock::now();
        renderer.BeginFrame();
        renderer.Clear(.2f, .25f, .3f);
        auto context = renderer.CreateFrameGraphicsContext();
        scene.PrepareRenderFrame();
        const auto prepared = Clock::now();
        scene.Render(context.get(), 1.f, camera, false,
            Width, Height, true);
        scene.Render(context.get(), 1.f, camera, false, Width, Height);
        const auto rendered = Clock::now();
        renderer.EndFrame();
        // GPU timing queries resolve asynchronously. Waiting for the whole
        // GPU here would turn a fixed-view frame sample into a flush test.
        const auto finished = Clock::now();
        if (!samples) return;
        samples->frames.push_back(Milliseconds(start, finished));
        const double streamingMs = Milliseconds(start, streamed);
        samples->streaming.push_back(streamingMs);
        samples->managerCpu.push_back(Milliseconds(start, managed));
        samples->batchRefreshCpu.push_back(
            manager->GetRenderBatchRefreshMilliseconds() - refreshBefore);
        samples->batchBuildCpu.push_back(
            manager->GetRenderBatchBuildMilliseconds() - buildBefore);
        samples->farTileCpu.push_back(Milliseconds(managed, streamed));
        samples->worstStreaming = std::max(samples->worstStreaming, streamingMs);
        samples->renderCpu.push_back(Milliseconds(streamed, finished));
        samples->prepareCpu.push_back(Milliseconds(streamed, prepared));
        samples->sceneRenderCpu.push_back(Milliseconds(prepared, rendered));
        samples->frameEndCpu.push_back(Milliseconds(rendered, finished));
        samples->peakDraws = std::max(samples->peakDraws,
            scene.GetLastOrdinaryDrawCount());
        samples->draws.push_back(scene.GetLastOrdinaryDrawCount());
        samples->terrainOcclusionCulls.push_back(
            scene.GetLastTerrainOcclusionCulledCount());
        samples->submittedTriangles.push_back(
            static_cast<double>(scene.GetLastOrdinaryTriangleCount()));
        samples->residentCells.push_back(
            static_cast<double>(manager->GetLoadedCellCount()));
        samples->objectUploadKiB.push_back(
            scene.GetLastObjectDataUploadBytes() / 1024.0);
        const auto telemetry = renderer.GetFrameTimingTelemetry();
        if (telemetry.gpuTimingsValid && telemetry.gpuRegionCount > 0 &&
            telemetry.gpuSampleId != samples->lastGpuSampleId)
        {
            samples->lastGpuSampleId = telemetry.gpuSampleId;
            double total = 0.0;
            for (double stage : telemetry.gpuMilliseconds) total += stage;
            samples->gpuTotal.push_back(total);
            samples->gpuTerrain.push_back(telemetry.gpuMilliseconds[
                static_cast<size_t>(Engine::Graphics::GpuTimingStage::Terrain)]);
            for (size_t stage = 0; stage < samples->gpuStages.size(); ++stage)
                samples->gpuStages[stage].push_back(telemetry.gpuMilliseconds[stage]);
        }
        SampleMemory(*samples, *manager);
        if (samples->frames.size() % 8 == 0)
        {
            const double visible = VisibleCellCenterDistance(
                *manager, *camera, allCells);
            samples->visibleDistances.push_back(visible);
            samples->visibleDistance = std::max(samples->visibleDistance, visible);
        }
    };

    constexpr int FramesPerPhase = 90;
    constexpr double FrameSeconds = 1.0 / 60.0;
    const double heading = viewpoint == "edge"
        ? -2.356194490192345 : .7853981633974483;
    const double startX = initialCell * 128.0 + 64.0;
    const double startZ = startX;
    for (int i = 0; i < 30; ++i)
        runFrame(startX, startZ, heading, nullptr);
    Samples samples;
    // Look across the map from one high viewpoint, then move at 1.4 m/s
    // walking and 5 m/s running speeds using a fixed 60 Hz simulation step.
    for (int frame = 0; frame < FramesPerPhase; ++frame)
        runFrame(startX, startZ,
            heading + (headingMode == "sweep"
                ? 6.283185307179586 * frame / (FramesPerPhase - 1)
                : .35 * std::sin(frame * .07)), &samples);
    double travel = 0.0;
    for (int frame = 0; frame < FramesPerPhase; ++frame)
    {
        travel += 1.4 * FrameSeconds;
        runFrame(startX + travel / std::sqrt(2.0),
            startZ + travel / std::sqrt(2.0), heading, &samples);
    }
    for (int frame = 0; frame < FramesPerPhase; ++frame)
    {
        travel += 5.0 * FrameSeconds;
        runFrame(startX + travel / std::sqrt(2.0),
            startZ + travel / std::sqrt(2.0), heading, &samples);
    }
    renderer.WaitIdle();

    const auto farMeshes = driver->GetFarTileMeshes();
    Require(!farFieldEnabled || !farMeshes.empty(),
        "baked distant terrain did not render");
    uint64_t farFieldTriangles = 0;
    for (const auto* mesh : farMeshes)
        farFieldTriangles += mesh->GetIndexCount() / 3;
    double farFieldVisibleMeters = 0.0;
    if (farFieldEnabled)
    {
        const glm::dmat4 viewProjection =
            camera->GetProjectionMatrixD(static_cast<double>(Width) / Height) *
            camera->GetViewMatrixD();
        const glm::dvec3 eye = cameraObject->transform.GetWorldPositionD();
        for (const auto* mesh : farMeshes)
        for (const auto& vertex : mesh->GetVertices())
        {
            const glm::dvec3 point =
                mesh->Owner->transform.GetWorldPositionD() +
                glm::dvec3(vertex.pos[0], vertex.pos[1], vertex.pos[2]);
            if (glm::distance(point, eye) > camera->farPlane) continue;
            const glm::dvec4 clip = viewProjection * glm::dvec4(point, 1.0);
            if (clip.w <= 0.0 || std::abs(clip.x) > clip.w ||
                std::abs(clip.y) > clip.w || clip.z < 0.0 || clip.z > clip.w)
                continue;
            farFieldVisibleMeters = std::max(farFieldVisibleMeters,
                glm::length(glm::dvec2(point.x - eye.x, point.z - eye.z)));
        }
        if (farPlaneMeters >= 10000)
            Require(farFieldVisibleMeters > 10000.0,
                "distant terrain did not reach across the map");
    }
    Require(samples.frames.size() == FramesPerPhase * 3,
        "look, walk, and run test was incomplete");
    if (samples.peakResident == 0 || samples.peakDraws == 0)
        std::cerr << "Terrain samples: peakResident=" << samples.peakResident
            << " peakDraws=" << samples.peakDraws << '\n';
    Require(samples.peakResident > 0 && samples.peakDraws > 0,
        "terrain did not render during the performance test");
    Require(!samples.gpuTotal.empty(), "GPU timing queries did not resolve");
    Require(samples.visibleDistance >= 1000.0,
        "terrain did not reach kilometer-scale visibility");
    Require(samples.peakTerrainCpu <=
        static_cast<uint64_t>(manager->cpuBudgetMiB) << 20,
        "terrain CPU budget exceeded");
    Require(samples.peakTerrainGpu <=
        static_cast<uint64_t>(manager->gpuBudgetMiB) << 20,
        "terrain GPU buffer budget exceeded");
    Require(samples.peakCache <=
        static_cast<uint64_t>(manager->cacheBudgetMiB) << 20,
        "terrain mesh cache budget exceeded");
    const double boundaryX = initialCell * 128.0 + 127.5;
    runFrame(boundaryX, startZ, heading, nullptr);
    const uint64_t uploadsBeforeCrossing = driver->GetFarTileUploadCount();
    const uint64_t bytesBeforeCrossing = driver->GetFarTileUploadedBytes();
    const double rebuildBeforeCrossing =
        driver->GetFarTileRebuildMilliseconds();
    const uint64_t batchUploadsBeforeCrossing =
        manager->GetRenderBatchUploadCount();
    const double batchBuildBeforeCrossing =
        manager->GetRenderBatchBuildMilliseconds();
    const size_t loadedBeforeCrossing = manager->GetLoadedCellCount();
    std::map<Engine::Components::TerrainManager::Cell, int> lodBeforeCrossing;
    for (const auto cell : allCells)
        if (const auto* terrain = manager->GetLoadedCell(cell.first, cell.second))
            lodBeforeCrossing[cell] = terrain->GetMeshLodStride();
    Samples crossingSamples;
    const auto crossingStart = Clock::now();
    for (int frame = 1; frame <= 60; ++frame)
    {
        runFrame(boundaryX + 5.0 * frame / 60.0, startZ,
            heading, &crossingSamples);
        std::this_thread::sleep_until(crossingStart +
            std::chrono::duration_cast<Clock::duration>(
                std::chrono::duration<double>(frame / 60.0)));
    }
    renderer.WaitIdle();
    const uint64_t crossingTileUploads =
        driver->GetFarTileUploadCount() - uploadsBeforeCrossing;
    const uint64_t crossingTileUploadBytes =
        driver->GetFarTileUploadedBytes() - bytesBeforeCrossing;
    const double crossingTileRebuildMs =
        driver->GetFarTileRebuildMilliseconds() - rebuildBeforeCrossing;
    const uint64_t crossingBatchUploads =
        manager->GetRenderBatchUploadCount() - batchUploadsBeforeCrossing;
    uint32_t crossingLodChanges = 0;
    for (const auto& [cell, oldStride] : lodBeforeCrossing)
        if (const auto* terrain = manager->GetLoadedCell(cell.first, cell.second))
            crossingLodChanges += terrain->GetMeshLodStride() != oldStride;
    Require(crossingLodChanges > 0,
        "cell crossing did not exercise a terrain LOD transition");
    if (farFieldEnabled)
        Require(crossingTileUploads > 0 && crossingTileUploads < 16,
            "distant terrain crossing rebuilt too many tiles");
    const double median = Percentile(samples.frames, .5);
    const double p95 = Percentile(samples.frames, .95);
    const double average = std::accumulate(samples.frames.begin(),
        samples.frames.end(), 0.0) / samples.frames.size();
    const double averageFps = average > 0.0 ? 1000.0 / average : 0.0;
    const bool meetsAverage100Fps = average <= 10.0;
    const bool meets100Fps = p95 <= 10.0;
    const auto report = [&](std::ostream& out) {
        const auto metric = [&](const char* name, const std::vector<double>& values) {
            out << "  \"" << name << "P50Ms\": " << Percentile(values, .5)
                << ",\n  \"" << name << "P95Ms\": " << Percentile(values, .95)
                << ",\n  \"" << name << "P99Ms\": " << Percentile(values, .99)
                << ",\n";
        };
        out << std::fixed << std::setprecision(3)
            << "{\n  \"resolution\": \"1920x1080\",\n"
            << "  \"cacheMode\": \""
                << (runtimeMode == "baked" ? "not-applicable" : cacheMode)
                << "\",\n"
            << "  \"runtimeMode\": \"" << runtimeMode << "\",\n"
            << "  \"headingMode\": \"" << headingMode << "\",\n"
            << "  \"viewpoint\": \"" << viewpoint << "\",\n"
            << "  \"cameraFarPlaneMeters\": " << farPlaneMeters << ",\n"
            << "  \"terrainFarRadiusCells\": " << farRadiusCells << ",\n"
            << "  \"terrainLoadedRadiusCells\": " << loadedRadiusCells << ",\n"
            << "  \"terrainLod2RadiusCells\": " << lod2RadiusCells << ",\n"
            << "  \"terrainLod4RadiusCells\": " << lod4RadiusCells << ",\n"
            << "  \"farFieldEnabled\": " << (farFieldEnabled ? "true" : "false")
                << ",\n"
            << "  \"shadowsEnabled\": " << (shadowsEnabled ? "true" : "false")
                << ",\n"
            << "  \"sunEnabled\": " << (sunEnabled ? "true" : "false")
                << ",\n"
            << "  \"unlit\": " << (unlit ? "true" : "false") << ",\n"
            << "  \"stochasticTerrainVariation\": "
                << (stochasticVariation ? "true" : "false") << ",\n"
            << "  \"terrainMacroColorEnabled\": "
                << (macroColorEnabled ? "true" : "false") << ",\n"
            << "  \"farFieldTriangles\": " << farFieldTriangles << ",\n"
            << "  \"submittedTrianglesP50\": "
                << Percentile(samples.submittedTriangles, .5) << ",\n"
            << "  \"submittedTrianglesP95\": "
                << Percentile(samples.submittedTriangles, .95) << ",\n"
            << "  \"submittedTrianglesP99\": "
                << Percentile(samples.submittedTriangles, .99) << ",\n"
            << "  \"peakTerrainTextureMiBEstimate\": "
                << samples.peakTerrainTextureBytes / 1048576.0 << ",\n"
            << "  \"farFieldTiles\": " << farMeshes.size() << ",\n"
            << "  \"crossingFarTileUploads\": " << crossingTileUploads << ",\n"
            << "  \"crossingFarTileUploadKiB\": "
                << crossingTileUploadBytes / 1024.0 << ",\n"
            << "  \"crossingFarTileRebuildMs\": "
                << crossingTileRebuildMs << ",\n"
            << "  \"crossingBatchUploads\": "
                << crossingBatchUploads << ",\n"
            << "  \"crossingLodChanges\": "
                << crossingLodChanges << ",\n"
            << "  \"crossingLoadedCellsBefore\": "
                << loadedBeforeCrossing << ",\n"
            << "  \"crossingLoadedCellsAfter\": "
                << manager->GetLoadedCellCount() << ",\n"
            << "  \"crossingDrawsP95\": "
                << Percentile(crossingSamples.draws, .95) << ",\n"
            << "  \"crossingObjectUploadKiBP95\": "
                << Percentile(crossingSamples.objectUploadKiB, .95) << ",\n"
            << "  \"crossingFrameP95Ms\": "
                << Percentile(crossingSamples.frames, .95) << ",\n"
            << "  \"crossingManagerP95Ms\": "
                << Percentile(crossingSamples.managerCpu, .95) << ",\n"
            << "  \"crossingBatchRefreshP95Ms\": "
                << Percentile(crossingSamples.batchRefreshCpu, .95) << ",\n"
            << "  \"crossingBatchBuildP95Ms\": "
                << Percentile(crossingSamples.batchBuildCpu, .95) << ",\n"
            << "  \"crossingBatchBuildTotalMs\": "
                << manager->GetRenderBatchBuildMilliseconds() -
                    batchBuildBeforeCrossing << ",\n"
            << "  \"crossingFarTileP95Ms\": "
                << Percentile(crossingSamples.farTileCpu, .95) << ",\n"
            << "  \"crossingRenderCpuP95Ms\": "
                << Percentile(crossingSamples.renderCpu, .95) << ",\n"
            << "  \"crossingPrepareP95Ms\": "
                << Percentile(crossingSamples.prepareCpu, .95) << ",\n"
            << "  \"crossingSceneRenderP95Ms\": "
                << Percentile(crossingSamples.sceneRenderCpu, .95) << ",\n"
            << "  \"crossingFrameEndP95Ms\": "
                << Percentile(crossingSamples.frameEndCpu, .95) << ",\n"
            << "  \"crossingGpuP95Ms\": "
                << Percentile(crossingSamples.gpuTotal, .95) << ",\n"
            << "  \"crossingWorstStreamingStallMs\": "
                << crossingSamples.worstStreaming << ",\n"
            << "  \"farFieldVisibleMeters\": " << farFieldVisibleMeters << ",\n"
            << "  \"cameraMotion\": \"look, walk, run\",\n"
            << "  \"walkMetersPerSecond\": 1.4,\n"
            << "  \"runMetersPerSecond\": 5.0,\n"
            << "  \"travelMeters\": " << travel << ",\n"
            << "  \"frameCount\": " << samples.frames.size() << ",\n"
            << "  \"frameAverageMs\": " << average << ",\n"
            << "  \"frameAverageFps\": " << averageFps << ",\n"
            << "  \"frameMedianMs\": " << median << ",\n"
            << "  \"frameP95Ms\": " << p95 << ",\n"
            << "  \"lookMedianMs\": " << PhasePercentile(samples.frames,
                0, FramesPerPhase, .5) << ",\n"
            << "  \"walkMedianMs\": " << PhasePercentile(samples.frames,
                FramesPerPhase, FramesPerPhase * 2, .5) << ",\n"
            << "  \"runMedianMs\": " << PhasePercentile(samples.frames,
                FramesPerPhase * 2, FramesPerPhase * 3, .5) << ",\n"
            << "  \"frameWorstMs\": " << *std::max_element(
                samples.frames.begin(), samples.frames.end()) << ",\n"
            << "  \"worstStreamingStallMs\": " << samples.worstStreaming << ",\n"
            << "  \"renderCpuMedianMs\": " << Percentile(samples.renderCpu, .5) << ",\n"
            << "  \"renderCpuP95Ms\": " << Percentile(samples.renderCpu, .95) << ",\n"
            << "  \"gpuTotalMedianMs\": " << Percentile(samples.gpuTotal, .5) << ",\n"
            << "  \"gpuTotalP95Ms\": " << Percentile(samples.gpuTotal, .95) << ",\n"
            << "  \"gpuTerrainMedianMs\": " << Percentile(samples.gpuTerrain, .5) << ",\n"
            << "  \"gpuTimingSamples\": " << samples.gpuTotal.size() << ",\n"
            << "  \"gpuQuerySyncIntervalFrames\": 0,\n"
            << "  \"visibleCellCenterMeters\": " << samples.visibleDistance << ",\n"
            << "  \"visibleCellCenterMedianMeters\": "
            << Percentile(samples.visibleDistances, .5) << ",\n"
            << "  \"visibleCellCenterP05Meters\": "
            << Percentile(samples.visibleDistances, .05) << ",\n"
            << "  \"peakResidentCells\": " << samples.peakResident << ",\n"
            << "  \"peakDraws\": " << samples.peakDraws << ",\n"
            << "  \"peakTerrainCpuMiB\": " << samples.peakTerrainCpu / MiB << ",\n"
            << "  \"peakTerrainGpuMiB\": " << samples.peakTerrainGpu / MiB << ",\n"
            << "  \"peakCacheMiB\": " << samples.peakCache / MiB << ",\n"
            << "  \"peakWorkingSetMiB\": " << samples.peakWorkingSet / MiB << ",\n"
            << "  \"peakPrivateMiB\": " << samples.peakPrivate / MiB << ",\n"
            << "  \"targetAverageUnder10Ms\": " <<
                (meetsAverage100Fps ? "true" : "false") << ",\n"
            << "  \"targetP95Under10Ms\": " << (meets100Fps ? "true" : "false")
            << ",\n";
        metric("frame", samples.frames);
        metric("managerCpu", samples.managerCpu);
        metric("farTileCpu", samples.farTileCpu);
        metric("renderCpu", samples.renderCpu);
        metric("gpuTotal", samples.gpuTotal);
        metric("gpuShadow", samples.gpuStages[static_cast<size_t>(
            Engine::Graphics::GpuTimingStage::Shadow)]);
        metric("gpuTerrain", samples.gpuStages[static_cast<size_t>(
            Engine::Graphics::GpuTimingStage::Terrain)]);
        metric("gpuOpaque", samples.gpuStages[static_cast<size_t>(
            Engine::Graphics::GpuTimingStage::Opaque)]);
        metric("gpuTransparent", samples.gpuStages[static_cast<size_t>(
            Engine::Graphics::GpuTimingStage::Transparent)]);
        metric("crossingFrame", crossingSamples.frames);
        metric("crossingGpuTotal", crossingSamples.gpuTotal);
        out << "  \"drawsP50\": " << Percentile(samples.draws, .5)
            << ",\n  \"drawsP95\": " << Percentile(samples.draws, .95)
            << ",\n  \"drawsP99\": " << Percentile(samples.draws, .99)
            << ",\n  \"terrainOcclusionCullsP95\": " <<
                Percentile(samples.terrainOcclusionCulls, .95)
            << ",\n  \"residentCellsP50\": "
            << Percentile(samples.residentCells, .5)
            << ",\n  \"residentCellsP95\": "
            << Percentile(samples.residentCells, .95)
            << ",\n  \"residentCellsP99\": "
            << Percentile(samples.residentCells, .99)
            << "\n}\n";
    };
    const auto output = std::filesystem::path(".local/performance-logs") /
        ("terrain-1080p-far-" + std::to_string(farPlaneMeters) +
            "-radius-" + std::to_string(farRadiusCells) +
            (farFieldEnabled ? "-distant-" : "-cells-only-") + viewpoint +
            "-" + runtimeMode + "-" + headingMode + "-" + cacheMode +
            (shadowsEnabled ? "" : "-no-shadows") +
            (sunEnabled ? "" : "-no-sun") +
            (unlit ? "-unlit" : "") +
            (stochasticVariation ? "-stochastic" : "") +
            (macroColorEnabled ? "-macro" : "") +
            ".json");
    std::filesystem::create_directories(output.parent_path());
    std::ofstream file(output, std::ios::trunc);
    Require(static_cast<bool>(file), "could not write terrain performance report");
    report(file);
    file.flush();
    report(std::cout);
    if (GetEnvironmentVariableA("TERRAIN_REQUIRE_100FPS_AVERAGE", nullptr, 0) > 0)
        Require(meetsAverage100Fps,
            "terrain 1080p average frame time did not meet 100 FPS target");
    if (GetEnvironmentVariableA("TERRAIN_REQUIRE_100FPS", nullptr, 0) > 0)
        Require(meets100Fps, "terrain 1080p p95 did not meet 100 FPS target");
    return 0;
}

int main()
{
    try { return Run(); }
    catch (const std::exception& error)
    {
        std::cerr << "Terrain performance test failed: " << error.what() << '\n';
        return 1;
    }
}

#include "Core/Assets/Scripts/TerrainGen/TerrainChunk.h"
#include "Core/Assets/Scripts/TerrainGen/TerrainGen.h"
#include "Core/Assets/Scripts/TerrainGen/TerrainStreamingCameraDriver.h"
#include "Core/Compoonents/Camera/Camera.h"
#include "Core/Compoonents/Obj/Mesh.h"
#include "Core/Graphics/IGraphicsContext.h"
#include "Core/Model/ProjectSettings.h"
#include "Core/Object.h"
#include "Core/Renderers/IGameRenderer.h"
#include "Core/Renderers/RendererFactory.h"
#include "Core/Renderers/DX11/DX11GraphicsBuffer.h"
#include "Core/Scene/Scene.h"
#include "Core/Window.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
using Clock = std::chrono::steady_clock;

void PumpWindowMessages()
{
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
    {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

double Percentile(std::vector<double> values, double percentile)
{
    if (values.empty())
        return 0.0;
    std::sort(values.begin(), values.end());
    const size_t index = static_cast<size_t>(std::round(
        percentile * static_cast<double>(values.size() - 1u)));
    return values[std::min(index, values.size() - 1u)];
}

struct RenderedBorderVertex
{
    glm::vec3 world{};
    glm::vec4 clip{};
    glm::vec3 ndc{};
    glm::vec2 pixel{};
    bool visible = false;
};

struct RenderedBorderAudit
{
    bool valid = true;
    size_t neighborPairs = 0;
    size_t comparedVertices = 0;
    size_t visibleVertices = 0;
    size_t uploadedVertices = 0;
    size_t missingBuffers = 0;
    size_t emptyBorderDirections = 0;
    size_t unmatchedVertices = 0;
    float maximumUploadError = 0.f;
    float maximumWorldError = 0.f;
    float maximumClipError = 0.f;
    float maximumNdcError = 0.f;
    float maximumPixelError = 0.f;
};

RenderedBorderAudit AuditRenderedChunkBorders(
    Engine::Core::Object& terrainObject,
    Engine::Components::Camera& camera,
    float chunkSize, uint32_t width, uint32_t height)
{
    const float aspect = static_cast<float>(width) /
        static_cast<float>(height);
    const glm::mat4 viewProjection =
        camera.GetProjectionMatrix(aspect, false) * camera.GetViewMatrix();
    std::map<std::pair<int, int>, Engine::Core::Object*> chunks;
    for (Engine::Core::Object* object : terrainObject.Children)
    {
        const TerrainChunk* chunk = object
            ? object->GetComponent<TerrainChunk>() : nullptr;
        if (chunk)
            chunks[{ chunk->chunkX, chunk->chunkZ }] = object;
    }

    RenderedBorderAudit audit;
    const auto collectBorder = [&](Engine::Core::Object* chunk,
        bool xAxis, float border)
    {
        std::vector<RenderedBorderVertex> result;
        for (Engine::Core::Object* patchObject : chunk->Children)
        {
            const auto* mesh = patchObject
                ? patchObject->GetComponent<Engine::Components::Mesh>() : nullptr;
            if (!mesh || !mesh->GetGraphicsBuffer())
                continue;
            const auto* dxBuffer = dynamic_cast<const Engine::Renderers::
                D3D11GraphicsBuffer*>(mesh->GetGraphicsBuffer());
            const uint8_t* uploadedBytes = dxBuffer
                ? dxBuffer->GetShadowData() : nullptr;
            const uint64_t requiredBytes = static_cast<uint64_t>(
                mesh->GetVertexCount()) * mesh->GetVertexStride();
            if (!uploadedBytes || mesh->GetGraphicsBuffer()->GetSize() <
                requiredBytes)
            {
                audit.valid = false;
                ++audit.missingBuffers;
                continue;
            }
            const glm::mat4 authoredWorldMatrix =
                patchObject->transform.GetWorldMatrix();
            const glm::mat4 renderWorldMatrix =
                patchObject->transform.GetWorldMatrixWithLayer();
            for (size_t index = 0; index < mesh->GetVertexCount(); ++index)
            {
                const float* uploadedValues = reinterpret_cast<const float*>(
                    uploadedBytes + index * mesh->GetVertexStride());
                const float* cpuValues = mesh->UsesTerrainVertexFormat()
                    ? mesh->GetTerrainVertices()[index].pos
                    : mesh->GetVertices()[index].pos;
                const glm::vec3 uploadedPosition(uploadedValues[0],
                    uploadedValues[1], uploadedValues[2]);
                const glm::vec3 cpuPosition(cpuValues[0], cpuValues[1],
                    cpuValues[2]);
                audit.maximumUploadError = std::max(audit.maximumUploadError,
                    glm::length(uploadedPosition - cpuPosition));
                ++audit.uploadedVertices;

                // This is the Object.hlsl vertex path: uploaded local vertex,
                // per-draw world matrix, then the shared view-projection.
                const glm::vec4 authoredWorldPosition = authoredWorldMatrix *
                    glm::vec4(uploadedPosition, 1.f);
                const float coordinate = xAxis
                    ? authoredWorldPosition.x : authoredWorldPosition.z;
                if (std::abs(coordinate - border) > 0.000001f)
                    continue;
                const glm::vec4 worldPosition = renderWorldMatrix *
                    glm::vec4(uploadedPosition, 1.f);
                RenderedBorderVertex sample{};
                sample.world = glm::vec3(worldPosition);
                sample.clip = viewProjection * worldPosition;
                if (std::abs(sample.clip.w) > 0.000001f)
                    sample.ndc = glm::vec3(sample.clip) / sample.clip.w;
                sample.pixel = {
                    (sample.ndc.x * 0.5f + 0.5f) * static_cast<float>(width),
                    (1.f - (sample.ndc.y * 0.5f + 0.5f)) *
                        static_cast<float>(height) };
                sample.visible = sample.clip.w > 0.f &&
                    sample.ndc.x >= -1.f && sample.ndc.x <= 1.f &&
                    sample.ndc.y >= -1.f && sample.ndc.y <= 1.f &&
                    sample.ndc.z >= 0.f && sample.ndc.z <= 1.f;
                result.push_back(sample);
            }
        }
        return result;
    };
    const auto compareDirections = [&](const std::vector<RenderedBorderVertex>& source,
        const std::vector<RenderedBorderVertex>& target)
    {
        if (source.empty() || target.empty())
        {
            audit.valid = false;
            ++audit.emptyBorderDirections;
            return;
        }
        for (const RenderedBorderVertex& point : source)
        {
            const RenderedBorderVertex* closest = nullptr;
            float nearest = std::numeric_limits<float>::max();
            for (const RenderedBorderVertex& candidate : target)
            {
                const float distance = glm::length(point.world - candidate.world);
                if (distance < nearest)
                {
                    nearest = distance;
                    closest = &candidate;
                }
            }
            if (!closest)
            {
                audit.valid = false;
                ++audit.unmatchedVertices;
                continue;
            }
            audit.maximumWorldError = std::max(audit.maximumWorldError, nearest);
            audit.maximumClipError = std::max(audit.maximumClipError,
                glm::length(point.clip - closest->clip));
            audit.maximumNdcError = std::max(audit.maximumNdcError,
                glm::length(point.ndc - closest->ndc));
            audit.maximumPixelError = std::max(audit.maximumPixelError,
                glm::length(point.pixel - closest->pixel));
            ++audit.comparedVertices;
            if (point.visible && closest->visible)
                ++audit.visibleVertices;
            if (nearest > 0.000001f)
                audit.valid = false;
        }
    };

    for (const auto& [coordinate, chunk] : chunks)
    {
        for (const auto& direction : {
            std::pair<int, int>{ 1, 0 }, std::pair<int, int>{ 0, 1 } })
        {
            const std::pair<int, int> neighborCoordinate {
                coordinate.first + direction.first,
                coordinate.second + direction.second };
            const auto neighbor = chunks.find(neighborCoordinate);
            if (neighbor == chunks.end())
                continue;
            const bool xAxis = direction.first != 0;
            const float border = static_cast<float>(xAxis
                ? neighborCoordinate.first : neighborCoordinate.second) *
                chunkSize;
            const auto first = collectBorder(chunk, xAxis, border);
            const auto second = collectBorder(
                neighbor->second, xAxis, border);
            compareDirections(first, second);
            compareDirections(second, first);
            ++audit.neighborPairs;
        }
    }
    audit.valid = audit.valid && audit.maximumUploadError == 0.f &&
        audit.maximumWorldError <= 0.000001f &&
        audit.maximumClipError <= 0.000001f &&
        audit.maximumNdcError <= 0.000001f &&
        audit.maximumPixelError <= 0.0001f;
    return audit;
}
}

int main(int argumentCount, char** arguments)
{
    try
    {
        const int radius = argumentCount > 1
            ? std::clamp(std::atoi(arguments[1]), 0, 8) : 8;
        constexpr uint32_t width = 1280;
        constexpr uint32_t height = 720;
        const int measuredFrames = argumentCount > 2
            ? (std::strcmp(arguments[2], "--audit-only") == 0
                ? 1 : std::clamp(std::atoi(arguments[2]), 1, 3600))
            : 600;

        const char* renderingApi = argumentCount > 5
            ? arguments[5] : "DirectX11";
        Engine::Model::ProjectSettings settings{};
        settings.gameRenderingAPI = renderingApi;
        Engine::Core::Window window(GetModuleHandleW(nullptr),
            L"TerrainGen Rendered Benchmark", width, height);
        auto renderer = Engine::Renderers::RendererFactory::CreateGameRenderer(settings);
        if (!renderer || !renderer->Init(window.GetHWND(), width, height))
            throw std::runtime_error(std::string(renderingApi) +
                " renderer initialization failed");

        Engine::Scene::Scene scene;
        scene.Init(renderer->GetGraphicsProvider());
        if (!scene.Load(
            "Engine/Core/Assets/Scenes/Procedural/terrain_gen_streaming.scene"))
            throw std::runtime_error("Terrain streaming scene failed to load");

        auto* terrainObject = scene.FindObjectByName("Long Range Terrain");
        auto* cameraObject = scene.FindObjectByName("Long Range Terrain Camera");
        auto* terrain = terrainObject
            ? terrainObject->GetComponent<TerrainGen>() : nullptr;
        auto* driver = cameraObject
            ? cameraObject->GetComponent<TerrainStreamingCameraDriver>() : nullptr;
        if (!terrain || !driver)
            throw std::runtime_error("Terrain benchmark components are missing");
        terrain->viewRadiusInChunks = radius;
        if (argumentCount > 3)
            terrain->parallelChunkBuilds = std::clamp(
                std::atoi(arguments[3]), 1, 16);
        const bool stationaryCamera = argumentCount > 4 &&
            std::strcmp(arguments[4], "--stationary") == 0;
        scene.Start();
        const float traversalSpeed = driver->movementSpeed;
        driver->movementSpeed = 0.01f;

        ShowWindow(window.GetHWND(), SW_SHOWNOACTIVATE);
        const float aspect = static_cast<float>(width) / static_cast<float>(height);
        bool collectStageTimings = false;
        double pumpTotalMs = 0.0, updateTotalMs = 0.0, prepareTotalMs = 0.0;
        double renderTotalMs = 0.0, presentTotalMs = 0.0;
        double pumpWorstMs = 0.0, updateWorstMs = 0.0, prepareWorstMs = 0.0;
        double renderWorstMs = 0.0, presentWorstMs = 0.0;
        double measuredStreamingWorstMs = 0.0;
        uint64_t objectUploadTotalBytes = 0;
        uint64_t objectUploadMaximumBytes = 0;
        uint64_t occlusionCulledTotal = 0;
        uint32_t occlusionCulledMaximum = 0;
        uint64_t constantArenaWritesTotal = 0;
        uint64_t constantDiscardMapsTotal = 0;
        std::array<double, static_cast<size_t>(
            Engine::Graphics::GpuTimingStage::Count)> gpuStageTotalMs{};
        uint32_t gpuTimingSamples = 0;
        uint64_t lastGpuSampleId = 0;
        double backendCpuSubmissionTotalMs = 0.0;
        double backendCpuPresentationTotalMs = 0.0;
        bool flipModelSwapChain = false;
        uint32_t lastGpuRegionCount = 0;
        const auto renderFrame = [&]()
        {
            const auto start = Clock::now();
            PumpWindowMessages();
            const auto afterPump = Clock::now();
            scene.Update(1.f / 60.f);
            const auto afterUpdate = Clock::now();
            scene.PrepareRenderFrame();
            const auto afterPrepare = Clock::now();
            renderer->BeginFrame();
            renderer->Clear(0.1f, 0.1f, 0.1f);
            auto context = renderer->CreateFrameGraphicsContext();
            if (context)
            {
                if (auto* camera = scene.FindGameCamera())
                    scene.Render(context.get(), aspect, camera, false);
            }
            const auto afterRender = Clock::now();
            renderer->EndFrame();
            const auto end = Clock::now();
            if (collectStageTimings)
            {
                const double pumpMs = std::chrono::duration<double, std::milli>(
                    afterPump - start).count();
                const double updateMs = std::chrono::duration<double, std::milli>(
                    afterUpdate - afterPump).count();
                const double prepareMs = std::chrono::duration<double, std::milli>(
                    afterPrepare - afterUpdate).count();
                const double renderMs = std::chrono::duration<double, std::milli>(
                    afterRender - afterPrepare).count();
                const double presentMs = std::chrono::duration<double, std::milli>(
                    end - afterRender).count();
                pumpTotalMs += pumpMs;
                updateTotalMs += updateMs;
                prepareTotalMs += prepareMs;
                renderTotalMs += renderMs;
                presentTotalMs += presentMs;
                pumpWorstMs = std::max(pumpWorstMs, pumpMs);
                updateWorstMs = std::max(updateWorstMs, updateMs);
                prepareWorstMs = std::max(prepareWorstMs, prepareMs);
                renderWorstMs = std::max(renderWorstMs, renderMs);
                presentWorstMs = std::max(presentWorstMs, presentMs);
                const auto telemetry = renderer->GetFrameTimingTelemetry();
                backendCpuSubmissionTotalMs +=
                    telemetry.cpuSubmissionMilliseconds;
                backendCpuPresentationTotalMs +=
                    telemetry.cpuPresentationMilliseconds;
                flipModelSwapChain = telemetry.flipModelSwapChain;
                lastGpuRegionCount = telemetry.gpuRegionCount;
                if (telemetry.gpuTimingsValid &&
                    telemetry.gpuSampleId != lastGpuSampleId)
                {
                    for (size_t stage = 0; stage < gpuStageTotalMs.size(); ++stage)
                        gpuStageTotalMs[stage] += telemetry.gpuMilliseconds[stage];
                    ++gpuTimingSamples;
                    lastGpuSampleId = telemetry.gpuSampleId;
                }
                measuredStreamingWorstMs = std::max(measuredStreamingWorstMs,
                    terrain->GetLastStreamingMilliseconds());
                const uint64_t objectUploadBytes =
                    scene.GetLastObjectDataUploadBytes();
                objectUploadTotalBytes += objectUploadBytes;
                objectUploadMaximumBytes = std::max(
                    objectUploadMaximumBytes, objectUploadBytes);
                const uint32_t occlusionCulled =
                    scene.GetLastOcclusionCulledCount();
                occlusionCulledTotal += occlusionCulled;
                occlusionCulledMaximum = std::max(
                    occlusionCulledMaximum, occlusionCulled);
                if (auto* factory = renderer->GetGraphicsProvider()
                    ->GetContextFactory())
                {
                    constantArenaWritesTotal +=
                        factory->GetConstantBufferArenaWrites();
                    constantDiscardMapsTotal +=
                        factory->GetConstantBufferDiscardMaps();
                }
            }
            return std::chrono::duration<double, std::milli>(end - start).count();
        };

        const size_t diameter = static_cast<size_t>(radius * 2 + 1);
        const size_t desiredChunks = diameter * diameter;
        const auto fillStart = Clock::now();
        double fillWorstMs = 0.0;
        while (terrain->GetLoadedChunkCount() != desiredChunks ||
            terrain->GetQueuedChunkCount() != 0u ||
            terrain->GetInFlightChunkCount() != 0u)
        {
            fillWorstMs = std::max(fillWorstMs, renderFrame());
            if (Clock::now() - fillStart > std::chrono::seconds(90))
                throw std::runtime_error("Initial terrain fill timed out");
        }
        const double fillSeconds = std::chrono::duration<double>(
            Clock::now() - fillStart).count();

        // The stationary profile includes a deterministic visibility case:
        // one opaque box completely covers a smaller box behind it. This
        // verifies query behavior independently of terrain shape or camera
        // placement while the normal moving profile remains unchanged.
        if (stationaryCamera)
        {
            const auto addCameraRelativeCube = [&](const char* name,
                const glm::vec3& position, const glm::vec3& scale)
            {
                Engine::Core::Object* object = scene.AddObject(name);
                object->Parent = cameraObject;
                cameraObject->Children.push_back(object);
                object->transform.position = position;
                object->transform.scale = scale;
                auto* mesh = object->AddComponent<Engine::Components::Mesh>();
                mesh->LoadFromFile("Engine/Core/Assets/Mesh/cube.obj");
                mesh->OnAfterDeserialize(renderer->GetGraphicsProvider());
            };
            addCameraRelativeCube("Occlusion Test Wall",
                { 0.f, 0.f, 10.f }, { 12.f, 12.f, 0.5f });
            addCameraRelativeCube("Occlusion Test Hidden",
                { 0.f, 0.f, 20.f }, { 1.f, 1.f, 1.f });
        }

        driver->movementSpeed = stationaryCamera ? 0.f : traversalSpeed;
        if (stationaryCamera)
            driver->moveOnStart = false;
        std::vector<double> frameTimes;
        frameTimes.reserve(measuredFrames);
        collectStageTimings = true;
        size_t maximumMissingChunks = 0u;
        int viewerChunkMissingFrames = 0;
        int forwardChunkMissingFrames = 0;
        glm::vec3 travelDirection = driver->endPosition - driver->startPosition;
        travelDirection.y = 0.f;
        travelDirection = glm::normalize(travelDirection);
        for (int frame = 0; frame < measuredFrames; ++frame)
        {
            frameTimes.push_back(renderFrame());
            const size_t loaded = terrain->GetLoadedChunkCount();
            maximumMissingChunks = std::max(maximumMissingChunks,
                desiredChunks > loaded ? desiredChunks - loaded : 0u);
            const glm::vec3 viewerPosition = cameraObject->transform.GetWorldPosition();
            const int viewerX = static_cast<int>(std::floor(
                viewerPosition.x / terrain->chunkSize));
            const int viewerZ = static_cast<int>(std::floor(
                viewerPosition.z / terrain->chunkSize));
            if (!terrain->IsChunkLoaded(viewerX, viewerZ))
                ++viewerChunkMissingFrames;
            const glm::vec3 forwardPosition = viewerPosition +
                travelDirection * terrain->chunkSize;
            const int forwardX = static_cast<int>(std::floor(
                forwardPosition.x / terrain->chunkSize));
            const int forwardZ = static_cast<int>(std::floor(
                forwardPosition.z / terrain->chunkSize));
            if (!terrain->IsChunkLoaded(forwardX, forwardZ))
                ++forwardChunkMissingFrames;
        }

        size_t residentVertices = 0u;
        size_t residentIndices = 0u;
        uint64_t cpuMeshBytes = 0u;
        uint64_t uploadShadowBytes = 0u;
        uint64_t gpuBufferBytes = 0u;
        uint64_t legacyExpandedBytes = 0u;
        for (Engine::Core::Object* chunkObject : terrainObject->Children)
        {
            if (!chunkObject || !chunkObject->GetComponent<TerrainChunk>())
                continue;
            for (Engine::Core::Object* patchObject : chunkObject->Children)
            {
                const auto* mesh = patchObject
                    ? patchObject->GetComponent<Engine::Components::Mesh>() : nullptr;
                if (mesh)
                {
                    residentVertices += mesh->GetVertexCount();
                    residentIndices += mesh->GetIndexCount();
                    cpuMeshBytes += mesh->GetCpuMeshMemoryBytes();
                    uploadShadowBytes += mesh->GetUploadShadowMemoryBytes();
                    gpuBufferBytes += mesh->GetGpuBufferMemoryBytes();
                    legacyExpandedBytes += static_cast<uint64_t>(
                        mesh->GetIndexCount() > 0u
                            ? mesh->GetIndexCount() : mesh->GetVertexCount()) *
                        sizeof(Engine::Model::AnimationVertex);
                }
            }
        }
        Engine::Components::Camera* renderedCamera = scene.FindGameCamera();
        if (!renderedCamera)
            throw std::runtime_error("Rendered border audit camera is missing");
        const RenderedBorderAudit renderedBorderAudit =
            std::strcmp(renderingApi, "DirectX11") == 0
            ? AuditRenderedChunkBorders(*terrainObject, *renderedCamera,
                terrain->chunkSize, width, height)
            : RenderedBorderAudit{};
        if (!renderedBorderAudit.valid)
        {
            std::fprintf(stderr,
                "render border mismatch pairs=%zu vertices=%zu visible=%zu "
                "uploaded=%zu missing_buffers=%zu empty_directions=%zu "
                "unmatched=%zu upload=%.9f world=%.9f clip=%.9f ndc=%.9f "
                "pixel=%.9f\n",
                renderedBorderAudit.neighborPairs,
                renderedBorderAudit.comparedVertices,
                renderedBorderAudit.visibleVertices,
                renderedBorderAudit.uploadedVertices,
                renderedBorderAudit.missingBuffers,
                renderedBorderAudit.emptyBorderDirections,
                renderedBorderAudit.unmatchedVertices,
                renderedBorderAudit.maximumUploadError,
                renderedBorderAudit.maximumWorldError,
                renderedBorderAudit.maximumClipError,
                renderedBorderAudit.maximumNdcError,
                renderedBorderAudit.maximumPixelError);
            throw std::runtime_error("Uploaded terrain borders diverge in render space");
        }
        if (legacyExpandedBytes > 0u &&
            (cpuMeshBytes * 2u >= legacyExpandedBytes ||
                gpuBufferBytes * 2u >= legacyExpandedBytes))
            throw std::runtime_error(
                "Packed terrain did not reduce resident mesh memory by at least 50%");

        const double averageMs = std::accumulate(frameTimes.begin(),
            frameTimes.end(), 0.0) / static_cast<double>(frameTimes.size());
        const int overBudget = static_cast<int>(std::count_if(frameTimes.begin(),
            frameTimes.end(), [](double milliseconds)
            {
                return milliseconds > 16.667;
            }));
        std::printf("rendered api=%s resolution=%ux%u radius=%d chunks=%zu "
            "stationary=%s "
            "view_distance=%.0f fill_s=%.3f fill_worst_ms=%.3f avg_ms=%.3f "
            "p95_ms=%.3f p99_ms=%.3f worst_ms=%.3f over_budget=%d/%d "
            "max_missing=%zu viewer_missing=%d forward_missing=%d "
            "vertices=%zu indices=%zu cpu_mesh_bytes=%llu "
            "upload_shadow_bytes=%llu gpu_buffer_bytes=%llu "
            "legacy_expanded_bytes=%llu cpu_mesh_mib=%.2f "
            "upload_shadow_mib=%.2f gpu_buffer_mib=%.2f "
            "render_border_pairs=%zu render_border_vertices=%zu "
            "visible_border_vertices=%zu upload_error=%.9f world_error=%.9f "
            "clip_error=%.9f ndc_error=%.9f pixel_error=%.9f "
            "pump_avg=%.3f pump_worst=%.3f update_avg=%.3f "
            "update_worst=%.3f prepare_avg=%.3f "
            "prepare_worst=%.3f render_avg=%.3f render_worst=%.3f "
            "present_avg=%.3f present_worst=%.3f "
            "terrain_stream_worst=%.3f "
            "object_upload_avg_bytes=%.0f object_upload_max_bytes=%llu "
            "occlusion_culled_avg=%.2f occlusion_culled_max=%u "
            "constant_arena=%s constant_writes_avg=%.1f "
            "constant_discards_avg=%.1f "
            "cpu_submit_avg=%.3f backend_present_avg=%.3f "
            "gpu_terrain_avg=%.3f gpu_portal_avg=%.3f "
            "gpu_opaque_avg=%.3f gpu_transparent_avg=%.3f "
            "gpu_samples=%u gpu_regions=%u flip_model=%s "
            "built=%llu unloaded=%llu\n",
            renderingApi, width, height, radius, desiredChunks,
            stationaryCamera ? "true" : "false",
            terrain->chunkSize * static_cast<float>(radius), fillSeconds,
            fillWorstMs, averageMs, Percentile(frameTimes, 0.95),
            Percentile(frameTimes, 0.99),
            *std::max_element(frameTimes.begin(), frameTimes.end()), overBudget,
            measuredFrames, maximumMissingChunks, viewerChunkMissingFrames,
            forwardChunkMissingFrames, residentVertices, residentIndices,
            static_cast<unsigned long long>(cpuMeshBytes),
            static_cast<unsigned long long>(uploadShadowBytes),
            static_cast<unsigned long long>(gpuBufferBytes),
            static_cast<unsigned long long>(legacyExpandedBytes),
            static_cast<double>(cpuMeshBytes) / (1024.0 * 1024.0),
            static_cast<double>(uploadShadowBytes) / (1024.0 * 1024.0),
            static_cast<double>(gpuBufferBytes) / (1024.0 * 1024.0),
            renderedBorderAudit.neighborPairs,
            renderedBorderAudit.comparedVertices,
            renderedBorderAudit.visibleVertices,
            renderedBorderAudit.maximumUploadError,
            renderedBorderAudit.maximumWorldError,
            renderedBorderAudit.maximumClipError,
            renderedBorderAudit.maximumNdcError,
            renderedBorderAudit.maximumPixelError,
            pumpTotalMs / measuredFrames, pumpWorstMs,
            updateTotalMs / measuredFrames, updateWorstMs,
            prepareTotalMs / measuredFrames, prepareWorstMs,
            renderTotalMs / measuredFrames, renderWorstMs,
            presentTotalMs / measuredFrames, presentWorstMs,
            measuredStreamingWorstMs,
            static_cast<double>(objectUploadTotalBytes) / measuredFrames,
            static_cast<unsigned long long>(objectUploadMaximumBytes),
            static_cast<double>(occlusionCulledTotal) / measuredFrames,
            occlusionCulledMaximum,
            renderer->GetGraphicsProvider()->GetContextFactory()
                ->UsesPersistentConstantBufferArena() ? "true" : "false",
            static_cast<double>(constantArenaWritesTotal) / measuredFrames,
            static_cast<double>(constantDiscardMapsTotal) / measuredFrames,
            backendCpuSubmissionTotalMs / measuredFrames,
            backendCpuPresentationTotalMs / measuredFrames,
            gpuStageTotalMs[static_cast<size_t>(
                Engine::Graphics::GpuTimingStage::Terrain)] /
                std::max(1u, gpuTimingSamples),
            gpuStageTotalMs[static_cast<size_t>(
                Engine::Graphics::GpuTimingStage::Portal)] /
                std::max(1u, gpuTimingSamples),
            gpuStageTotalMs[static_cast<size_t>(
                Engine::Graphics::GpuTimingStage::Opaque)] /
                std::max(1u, gpuTimingSamples),
            gpuStageTotalMs[static_cast<size_t>(
                Engine::Graphics::GpuTimingStage::Transparent)] /
                std::max(1u, gpuTimingSamples),
            gpuTimingSamples, lastGpuRegionCount,
            flipModelSwapChain ? "true" : "false",
            static_cast<unsigned long long>(terrain->GetTotalChunksBuilt()),
            static_cast<unsigned long long>(terrain->GetTotalChunksUnloaded()));
        renderer->WaitIdle();
        return 0;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "rendered terrain benchmark failed: %s\n", error.what());
        return 1;
    }
}

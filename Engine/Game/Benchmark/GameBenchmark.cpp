#include "Game/Benchmark/GameBenchmark.h"
#include "Core/Scene/Scene.h"
#include "Core/Object.h"
#include "Core/Compoonents/Camera/Camera.h"
#include "Core/Compoonents/Mesh/Mesh.h"
#include "Core/Compoonents/Terrain/Terrain.h"
#include "Core/Compoonents/Terrain/TerrainManager.h"
#include "Core/Compoonents/Terrain/TerrainObjectLayer.h"
#include "Core/Renderers/IGameRenderer.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace Engine::Game
{
namespace
{
std::string Json(const std::string& text)
{
    std::ostringstream out;
    out << '"';
    for (unsigned char c : text)
    {
        if (c == '"' || c == '\\') out << '\\' << char(c);
        else if (c < 32) out << "\\u00" << std::hex << std::setw(2)
            << std::setfill('0') << unsigned(c) << std::dec;
        else out << char(c);
    }
    return out.str() + '"';
}

std::string Timestamp()
{
    const auto now = std::chrono::system_clock::now();
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count() % 1000;
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm local{};
    localtime_s(&local, &time);
    std::ostringstream out;
    out << std::put_time(&local, "%Y%m%d-%H%M%S") << '-'
        << std::setw(3) << std::setfill('0') << milliseconds;
    return out.str();
}
}

GameBenchmark::GameBenchmark(const GameBenchmarkOptions& options,
    Engine::Renderers::IGameRenderer& renderer, const std::string& scene,
    uint32_t maxFps, bool clusteredLighting, double startupMs) : m_options(options)
{
    m_directory = std::filesystem::absolute(options.output) /
        (options.scene.stem().string() + "-game-" + options.backend + "-" + Timestamp());
    if (!std::filesystem::create_directories(m_directory))
        throw std::runtime_error("could not create unique capture directory: " + m_directory.string());
    m_log.open(m_directory / "frames.jsonl");
    if (!m_log) throw std::runtime_error("could not create benchmark log");
    m_log.imbue(std::locale::classic());
    m_log << std::setprecision(10)
        << "{\"type\":\"session\",\"scope\":\"standalone_game\",\"scene\":" << Json(scene)
        << ",\"project\":" << Json(options.project.string())
        << ",\"backend\":" << Json(renderer.GetBackendName())
        << ",\"device\":" << Json(renderer.GetGraphicsDeviceName())
        << ",\"requested_device\":" << Json(options.device.empty() ? "project_default" : options.device)
        << ",\"width\":" << renderer.GetWidth() << ",\"height\":" << renderer.GetHeight()
        << ",\"warmup_frames\":" << options.warmup << ",\"sample_frames\":" << options.frames
        << ",\"residency_timeout_seconds\":" << options.residencyTimeoutSeconds
        << ",\"synthetic_forward_frames\":" << options.driveForwardFrames
        << ",\"game_max_fps\":" << maxFps << ",\"startup_to_scene_start_ms\":" << startupMs
        << ",\"clustered_lighting_setting\":" << (clusteredLighting ? "true" : "false")
        << ",\"render_scale\":" << renderer.GetRenderScale()
        << ",\"dynamic_render_scale\":" << (renderer.IsDynamicRenderScaleEnabled() ? "true" : "false")
        << ",\"temporal_upscaling\":" << (renderer.IsTemporalUpscalingEnabled() ? "true" : "false")
#ifdef ENGINE_BUILD_CONFIGURATION
        << ",\"build_configuration\":" << Json(ENGINE_BUILD_CONFIGURATION)
#endif
#ifdef NDEBUG
        << ",\"assertions_enabled\":false"
#else
        << ",\"assertions_enabled\":true"
#endif
#ifdef ENGINE_SHADER_UNOPTIMIZED_DEBUG
        << ",\"d3d_shader_optimization\":\"disabled\""
#else
        << ",\"d3d_shader_optimization\":\"level3\""
#endif
        << ",\"d3d_shader_debug_info\":true"
        << ",\"triangle_count_scope\":\"cpu_submitted_including_indirect_candidates\"}\n";
    m_log.flush();
    if (!m_log) throw std::runtime_error("failed to write benchmark session");
}

GameBenchmarkSceneSample GameBenchmark::CollectScene(Engine::Scene::Scene& scene)
{
    GameBenchmarkSceneSample result;
    result.render = scene.GetLastRenderCpuTimings();
    result.sceneGeneration = scene.CaptureSceneGeneration();
    const auto* camera = scene.FindGameCamera();
    result.camera = camera && camera->Owner;
    if (result.camera)
    {
        const auto position = camera->Owner->transform.GetWorldPositionD();
        result.cameraPosition[0] = position.x;
        result.cameraPosition[1] = position.y;
        result.cameraPosition[2] = position.z;
    }
    if (!result.camera) result.render = {};
    result.objects = scene.GetObjects().size();
    result.meshes = scene.GetComponents<Engine::Components::Mesh>().size();
    result.terrains = scene.GetComponents<Engine::Components::Terrain>().size();
    for (auto* layer : scene.GetComponents<Engine::Components::TerrainObjectLayer>())
        result.placements += layer->placements.size();
    for (auto* manager : scene.GetComponents<Engine::Components::TerrainManager>())
    {
        result.storedCells += manager->GetCellCount();
        result.loadedCells += manager->GetLoadedCellCount();
        result.preparingCells += manager->GetPreparingCellCount();
    }
    return result;
}

void GameBenchmark::Record(uint64_t frame, const GameBenchmarkFrameSample& s)
{
    const auto& r = s.scene.render;
    m_intervals.push_back(s.intervalMs);
    m_cameraSeen |= s.scene.camera;
    if (s.scene.camera)
    {
        if (!m_cameraOriginSet)
        {
            std::copy_n(s.scene.cameraPosition, 3, m_cameraOrigin);
            m_cameraOriginSet = true;
        }
        double squared = 0.0;
        for (size_t axis = 0; axis < 3; ++axis)
        {
            const double delta = s.scene.cameraPosition[axis] - m_cameraOrigin[axis];
            squared += delta * delta;
        }
        m_maxCameraDisplacement = std::max(m_maxCameraDisplacement, std::sqrt(squared));
    }
    m_peakDraws = std::max(m_peakDraws, r.physicalDraws);
    m_peakTriangles = std::max(m_peakTriangles, r.submittedTriangles);
    m_peakTerrains = std::max(m_peakTerrains, s.scene.terrains);
    m_peakLoadedCells = std::max(m_peakLoadedCells, s.scene.loadedCells);
    const bool validFrame = s.scene.camera && r.physicalDraws && r.submittedTriangles &&
        s.width == m_options.width && s.height == m_options.height &&
        (!m_options.requireTerrain || s.scene.terrains) &&
        (!m_options.requireStreamedCells || s.scene.loadedCells);
    m_validFrameSeen |= validFrame;
    m_validFrameCount += validFrame ? 1 : 0;
    m_log << "{\"type\":\"frame\",\"frame\":" << frame
        << ",\"valid_frame\":" << (validFrame ? "true" : "false")
        << ",\"width\":" << s.width << ",\"height\":" << s.height
        << ",\"render_scale\":" << s.renderScale
        << ",\"frame_interval_ms\":" << s.intervalMs
        << ",\"simulation_update_ms\":" << s.updateMs
        << ",\"scene_prepare_ms\":" << s.prepareMs
        << ",\"command_record_ms\":" << s.recordMs
        << ",\"update_handoff_wait_ms\":" << s.updateWaitMs
        << ",\"prepare_handoff_wait_ms\":" << s.prepareWaitMs
        << ",\"scene_replay_ms\":" << s.replayMs
        << ",\"end_frame_ms\":" << s.endFrameMs
        << ",\"render_thread_ms\":" << s.renderThreadMs
        << ",\"frame_latency_ms\":" << s.latencyMs
        << ",\"metrics_collection_ms\":" << s.metricsCollectionMs
        << ",\"residency_wait_ms\":" << s.residencyWaitMs
        << ",\"gpu_timing_valid\":" << (s.gpu.gpuTimingsValid ? "true" : "false")
        << ",\"gpu_sample_id\":" << s.gpu.gpuSampleId
        << ",\"gpu_opaque_ms\":";
    if (s.gpu.gpuTimingsValid) m_log << s.gpu.OpaqueMilliseconds(); else m_log << "null";
    const auto gpuStage = [&](const char* name, Engine::Graphics::GpuTimingStage stage)
    {
        m_log << ",\"gpu_" << name << "_ms\":";
        if (s.gpu.gpuTimingsValid) m_log << s.gpu.gpuMilliseconds[static_cast<size_t>(stage)];
        else m_log << "null";
    };
    gpuStage("shadow", Engine::Graphics::GpuTimingStage::Shadow);
    gpuStage("terrain", Engine::Graphics::GpuTimingStage::Terrain);
    gpuStage("grass", Engine::Graphics::GpuTimingStage::Grass);
    gpuStage("transparent", Engine::Graphics::GpuTimingStage::Transparent);
    m_log << ",\"scene_objects_loaded\":" << s.scene.objects
        << ",\"scene_generation\":" << s.scene.sceneGeneration
        << ",\"mesh_components\":" << s.scene.meshes
        << ",\"terrain_components\":" << s.scene.terrains
        << ",\"authored_foliage_placements\":" << s.scene.placements
        << ",\"terrain_cells_stored\":" << s.scene.storedCells
        << ",\"terrain_cells_loaded\":" << s.scene.loadedCells
        << ",\"terrain_cells_preparing\":" << s.scene.preparingCells
        << ",\"game_camera_present\":" << (s.scene.camera ? "true" : "false")
        << ",\"game_camera_position\":[" << s.scene.cameraPosition[0] << ','
        << s.scene.cameraPosition[1] << ',' << s.scene.cameraPosition[2] << ']'
        << ",\"draws_candidate\":" << r.candidateDraws
        << ",\"draws_submitted\":" << r.physicalDraws
        << ",\"triangles_candidate\":" << r.candidateTriangles
        << ",\"triangles_submitted\":" << r.submittedTriangles
        << ",\"instances_submitted\":" << r.submittedInstances
        << ",\"camera_visible_items\":" << r.cameraVisibility.acceptedItems
        << ",\"foliage_placement_candidates\":" << r.cameraVisibility.placementCandidates
        << ",\"foliage_screen_selected_lod0\":" << r.cameraVisibility.screenSelectedInstancesByLod[0]
        << ",\"foliage_screen_selected_lod1\":" << r.cameraVisibility.screenSelectedInstancesByLod[1]
        << ",\"foliage_screen_selected_lod2\":" << r.cameraVisibility.screenSelectedInstancesByLod[2]
        << ",\"render_cpu_ms\":" << r.totalMs
        << ",\"render_cluster_prepare_ms\":" << r.clusterPreparationMs
        << ",\"render_cluster_cache_hits\":" << r.clusterCacheHits
        << ",\"render_cluster_upload_bytes\":" << r.clusterUploadBytes
        << ",\"render_material_ms\":" << r.materialMs
        << ",\"render_material_pipeline_ms\":" << r.materialPipelineMs
        << ",\"render_material_geometry_binding_ms\":" << r.materialGeometryBindingMs
        << ",\"render_material_other_ms\":" << r.materialOtherMs
        << ",\"render_material_shader_pipeline_cache_hits\":" << r.materialShaderPipelineCacheHits
        << ",\"render_material_shader_pipeline_cache_misses\":" << r.materialShaderPipelineCacheMisses
        << ",\"render_batching_ms\":" << r.batchingMs
        << ",\"render_batch_snapshot_ms\":" << r.batchSnapshotMs
        << ",\"render_batch_build_ms\":" << r.batchBuildMs
        << ",\"render_batch_wait_ms\":" << r.batchWaitMs
        << ",\"render_batch_cache_hits\":" << r.batchCacheHits
        << ",\"render_batch_reused_keys\":" << r.batchReusedKeys << "}\n";
    if (!m_log) throw std::runtime_error("benchmark log write failed");
    // Preserve all samples before the separate image frame starts, without
    // adding a flush to the measured frames between samples.
    if (m_intervals.size() == m_options.frames)
    {
        m_log.flush();
        if (!m_log) throw std::runtime_error("benchmark log flush failed");
    }
}

void GameBenchmark::Capture(Engine::Renderers::IGameRenderer& renderer)
{
    renderer.WaitIdle();
    std::vector<uint8_t> pixels;
    const uint32_t width = renderer.GetWidth(), height = renderer.GetHeight();
    if (!renderer.ReadSceneCaptureRGBA(pixels) || pixels.size() != size_t(width) * height * 4)
        throw std::runtime_error("renderer did not return the requested scene capture");
    std::ofstream image(m_directory / "capture.ppm", std::ios::binary);
    image << "P6\n" << width << ' ' << height << "\n255\n";
    for (size_t i = 0; i < pixels.size(); i += 4)
        image.write(reinterpret_cast<const char*>(pixels.data() + i), 3);
    if (!image) throw std::runtime_error("failed to write capture image");
    m_captureWritten = true;
}

bool GameBenchmark::Finish()
{
    std::sort(m_intervals.begin(), m_intervals.end());
    const double median = m_intervals.empty() ? 0.0 :
        (m_intervals[(m_intervals.size() - 1) / 2] + m_intervals[m_intervals.size() / 2]) / 2.0;
    const double p95 = m_intervals.empty() ? 0.0 :
        m_intervals[static_cast<size_t>(std::ceil(.95 * m_intervals.size())) - 1];
    const bool valid = m_intervals.size() == m_options.frames && m_captureWritten &&
        m_validFrameCount == m_options.frames &&
        (!m_options.requireMovement || m_maxCameraDisplacement >= 0.05);
    m_log << "{\"type\":\"summary\",\"valid\":" << (valid ? "true" : "false")
        << ",\"measured_frames\":" << m_intervals.size()
        << ",\"frame_interval_median_ms\":" << median
        << ",\"frame_interval_p95_ms\":" << p95
        << ",\"peak_draws\":" << m_peakDraws << ",\"peak_submitted_triangles\":" << m_peakTriangles
        << ",\"peak_terrain_components\":" << m_peakTerrains
        << ",\"peak_loaded_cells\":" << m_peakLoadedCells
        << ",\"camera_seen\":" << (m_cameraSeen ? "true" : "false")
        << ",\"valid_frame_seen\":" << (m_validFrameSeen ? "true" : "false")
        << ",\"valid_frame_count\":" << m_validFrameCount
        << ",\"camera_max_displacement\":" << m_maxCameraDisplacement
        << ",\"require_movement\":" << (m_options.requireMovement ? "true" : "false")
        << ",\"capture_written\":" << (m_captureWritten ? "true" : "false")
        << ",\"require_terrain\":" << (m_options.requireTerrain ? "true" : "false")
        << ",\"require_streamed_cells\":" << (m_options.requireStreamedCells ? "true" : "false") << "}\n";
    m_log.close();
    if (!m_log) throw std::runtime_error("failed to finish benchmark log");
    std::cout << "Game benchmark " << (valid ? "valid" : "invalid") << ": "
        << m_intervals.size() << " frames, median " << median
        << " ms, p95 " << p95 << " ms\nOutput: " << m_directory.string() << '\n';
    return valid;
}
}

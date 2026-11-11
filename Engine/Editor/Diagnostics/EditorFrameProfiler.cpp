#include "Engine/Editor/Diagnostics/EditorFrameProfiler.h"
#include "Engine/Editor/Diagnostics/FrameActivity.h"
#include "Engine/Editor/Diagnostics/PerformanceProvenance.h"

#include "Core/Physics/Adapters/BulletPhysicsAdapter.h"
#include "Core/Renderers/DX11/DX11EditorRenderer.h"
#include "Core/Renderers/IEditorRenderer.h"
#include "Core/Scene/Scene.h"
#include "Core/Compoonents/Camera/Camera.h"

#include <cstdlib>
#if defined(_WIN32)
#include <windows.h>
#include <psapi.h>
#endif
#include <filesystem>
#include <string>
#include <sstream>
#include <locale>
#include <algorithm>

namespace Engine::Editor
{
    EditorFrameProfiler::EditorFrameProfiler(Scene::Scene* scene,
        Renderers::IEditorRenderer* renderer, bool autoProfilePlay)
    {
        // Automated editor playback also enables this trace by default.
        const char* requestedProfile = std::getenv("ENGINE_EDITOR_FRAME_PROFILE");
        if (!requestedProfile && autoProfilePlay)
            requestedProfile = "1";
        if (requestedProfile && std::string(requestedProfile) != "0")
        {
            const std::filesystem::path path =
                std::string(requestedProfile) == "1"
                ? std::filesystem::path(".local/performance-logs/editor-play-phases.jsonl")
                : std::filesystem::path(requestedProfile);
            if (!path.parent_path().empty())
                std::filesystem::create_directories(path.parent_path());
            output_.Start(path);
        }
        scene->SetUpdateProfilingEnabled(output_.IsOpen());
        if (auto* bullet = dynamic_cast<Engine::Physics::BulletPhysicsAdapter*>(
            &scene->GetPhysics().GetAdapter()))
            bullet->SetProfilingEnabled(output_.IsOpen());
        if (auto* dx11 = dynamic_cast<Engine::Renderers::DX11EditorRenderer*>(renderer))
            dx11->SetGpuTimingEnabled(output_.IsOpen());
    }

    std::string EditorFrameProfiler::SerializeFields(const EditorFrameProfileSample& sample,
        Scene::Scene* scene, Renderers::IEditorRenderer* renderer)
    {
        return CaptureFields(sample,scene,renderer).Format();
    }
    Engine::Threading::CapturedText EditorFrameProfiler::CaptureFields(const EditorFrameProfileSample& sample,
        Scene::Scene* scene, Renderers::IEditorRenderer* renderer)
    {
        Engine::Threading::CapturedText output;
        output << PerformanceProvenanceFields() << ",\"graphics_backend\":"
            << PerformanceJsonString(renderer->GetBackendName())
            << ",\"graphics_device\":"
            << PerformanceJsonString(renderer->GetGraphicsDeviceName())
            << ",\"scene_path\":"
            << PerformanceJsonString(sample.scenePath)
            << ",\"benchmark_scope\":"
            << PerformanceJsonString(std::getenv("ENGINE_EDITOR_BENCHMARK_SCOPE")
                ? std::getenv("ENGINE_EDITOR_BENCHMARK_SCOPE") : "editor")
            << ",\"render_width\":" << renderer->GetWidth()
            << ",\"render_height\":" << renderer->GetHeight()
            << ",\"render_scale\":" << renderer->GetRenderScale()
            << ",\"dynamic_render_scale\":" << (renderer->IsDynamicRenderScaleEnabled() ? "true" : "false")
            << ",\"temporal_upscaling\":" << (renderer->IsTemporalUpscalingEnabled() ? "true" : "false") << ',';
        auto effectiveQuality = scene->settings.graphicsQuality.Resolve();
        auto qualityChannels = Engine::Model::GraphicsQualitySettings::Channels(effectiveQuality);
        output << "\"quality_preset\":" << scene->settings.graphicsQuality.preset << ',';
        for (size_t i=0;i<qualityChannels.size();++i)
            output << "\"quality_" << Engine::Model::GraphicsQualitySettings::Keys[i] << "\":" << *qualityChannels[i] << ',';
        output << "\"simulation_time_seconds\":" << scene->GetElapsedTime() << ',';
        if (auto* camera=scene->FindGameCamera())
        {
            const auto view=camera->GetViewMatrix();
            const auto projection=camera->GetProjectionMatrix(sample.gameViewHeight ?
                float(sample.gameViewWidth)/sample.gameViewHeight : 1.f);
            const auto matrix=[&](const char* name,const glm::mat4& value) {
                output << '\"' << name << "\":[";
                for(int i=0;i<16;++i) { if(i) output << ','; output << value[i/4][i%4]; }
                output << "],";
            };
            matrix("game_camera_view",view); matrix("game_camera_projection",projection);
        }
        const auto ms = [](auto first, auto last)
        {
            return std::chrono::duration<double, std::milli>(
                last - first).count();
        };
        uint32_t physicsSubsteps = 0;
        double physicsDroppedMilliseconds = 0.0;
        double physicsPendingMilliseconds = 0.0;
        Engine::Physics::BulletPhysicsAdapter::StepTimings bulletTimings{};
        const auto updateTimings = sample.playing
            ? scene->GetLastUpdateTimings() : Scene::Scene::UpdateTimings{};
        const auto& prepareTimings = scene->GetLastPrepareTimings();
        double uiRenderMilliseconds = 0.0;
        const double presentMilliseconds = renderer->GetLastPresentationMilliseconds();
        double prepareFrameMilliseconds = 0.0;
        double beginUiMilliseconds = 0.0;
        double renderSetupMilliseconds = 0.0;
        const auto gpuTiming = renderer->GetGraphicsProvider()
            ->GetContextFactory()->GetFrameTimingTelemetry();
        const double queueSubmissionMilliseconds =
            renderer->GetLastQueueSubmissionMilliseconds();
        if (auto* dx11 = dynamic_cast<Engine::Renderers::DX11EditorRenderer*>(renderer))
        {
            uiRenderMilliseconds = dx11->GetLastUiRenderMilliseconds();
            prepareFrameMilliseconds = dx11->GetLastPrepareFrameMilliseconds();
            beginUiMilliseconds = dx11->GetLastBeginUiMilliseconds();
            renderSetupMilliseconds = dx11->GetLastRenderSetupMilliseconds();
        }
        if (sample.playing)
            if (auto* bullet = dynamic_cast<Engine::Physics::BulletPhysicsAdapter*>(
                &scene->GetPhysics().GetAdapter()))
            {
                physicsSubsteps = bullet->GetLastSubstepCount();
                physicsDroppedMilliseconds = bullet->GetLastDroppedSeconds() * 1000.0;
                physicsPendingMilliseconds = bullet->GetPendingSeconds() * 1000.0;
                bulletTimings = bullet->GetLastStepTimings();
            }
        double gpuTotal=0;
        for(double value:gpuTiming.gpuMilliseconds) gpuTotal+=value;
        double mainCpuMs=0, workerCpuMs=0;
        for(const auto& stage:sample.stages)
        { mainCpuMs+=stage.mainBusyMs; workerCpuMs+=stage.workerMs; }
#if defined(_WIN32)
        PROCESS_MEMORY_COUNTERS_EX memory{};
        memory.cb=sizeof(memory);
        if (GetProcessMemoryInfo(GetCurrentProcess(),
            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),sizeof(memory)))
            output << "\"editor_working_set_bytes\":" << memory.WorkingSetSize
                << ",\"editor_peak_working_set_bytes\":" << memory.PeakWorkingSetSize
                << ",\"editor_private_bytes\":" << memory.PrivateUsage << ',';
#endif
        output << "\"play_snapshot_capacity_bytes\":" << sample.playSnapshotBytes
            << ",\"scene_history_capacity_bytes\":" << sample.sceneHistoryBytes << ',';
        output << "\"schema_version\":3,\"playing\":"
            << (sample.playing ? "true" : "false")
            << ",\"dt_ms\":" << sample.updateIntervalMs
            << ",\"simulation_delta_ms\":" << sample.deltaSeconds * 1000.f
            << ",\"fps\":" << (sample.updateIntervalMs>0 ? 1000./sample.updateIntervalMs:0.)
            << ",\"fps_scope\":\"editor_update_interval\""
            << ",\"editor_update_interval_ms\":" << sample.updateIntervalMs
            << ",\"editor_updates_per_second\":" << (sample.updateIntervalMs>0 ? 1000./sample.updateIntervalMs:0.)
            << ",\"submission_count_valid\":" << (sample.submissionCountValid ? "true":"false")
            << ",\"submitted_frames\":" << sample.submittedFrames
            << ",\"submission_interval_ms\":" << sample.submissionIntervalMs
            << ",\"main_cpu_work_ms\":" << mainCpuMs
            << ",\"worker_execution_sum_ms\":" << workerCpuMs
            << ",\"main_nonrunning_ms\":" << std::max(0.,ms(sample.frameBegin,sample.afterPendingLoad)-mainCpuMs)
            << ",\"previous_idle_wait_ms\":" << sample.idleWaitMs
            << ",\"frame_latency_ms\":" << sample.updateIntervalMs
            << ",\"frame_ms\":" << ms(sample.frameBegin, sample.afterPendingLoad)
            << ",\"pacing_ms\":" << ms(sample.frameBegin, sample.afterPacing)
            << ",\"background_ms\":" << ms(sample.afterPacing, sample.afterBackground)
            << ",\"update_ms\":" << ms(sample.afterBackground, sample.afterSceneUpdate)
            << ",\"objects_ms\":" << updateTimings.objectsMs
            << ",\"pose_ms\":" << updateTimings.poseMs
            << ",\"constraints_ms\":" << updateTimings.constraintsMs
            << ",\"audio_ms\":" << updateTimings.audioMs
            << ",\"physics_ms\":" << updateTimings.physicsMs
            << ",\"physics_setup_ms\":" << bulletTimings.setupMs
            << ",\"physics_simulation_ms\":" << bulletTimings.simulationMs
            << ",\"physics_post_ms\":" << bulletTimings.postMs
            << ",\"post_physics_ms\":" << updateTimings.postPhysicsMs
            << ",\"render_ms\":" << ms(sample.afterSceneUpdate, sample.afterRender)
            << ",\"prepare_ms\":" << sample.prepareMilliseconds
            << ",\"render_entry_ms\":" << sample.renderEntryMilliseconds
            << ",\"gpu_query_poll_ms\":" << prepareFrameMilliseconds
            << ",\"render_setup_ms\":" << renderSetupMilliseconds
            << ",\"begin_ui_ms\":" << beginUiMilliseconds
            << ",\"clear_ms\":" << sample.clearMilliseconds
            << ",\"scene_prepare_ms\":" << sample.scenePrepareMilliseconds
            << ",\"prepare_asset_publication_ms\":" << prepareTimings.assetPublicationMs
            << ",\"prepare_texture_publication_ms\":" << prepareTimings.texturePublicationMs
            << ",\"prepare_shader_source_poll_ms\":" << prepareTimings.shaderSourcePollMs
            << ",\"prepare_shader_pipeline_publication_ms\":" << prepareTimings.shaderPipelinePublicationMs
            << ",\"prepare_object_commit_ms\":" << prepareTimings.objectCommitMs
            << ",\"prepare_prefab_instances_ms\":" << prepareTimings.prefabInstancesMs
            << ",\"prepare_prefab_signature_ms\":" << prepareTimings.prefabSignatureMs
            << ",\"prepare_prefab_source_probe_ms\":" << prepareTimings.prefabSourceProbeMs
            << ",\"prepare_graphics_preparation_ms\":" << prepareTimings.graphicsPreparationMs
            << ",\"prepare_native_buffers_ms\":" << prepareTimings.nativeBuffersMs
            << ",\"prepare_transforms_ms\":" << prepareTimings.transformsMs
            << ",\"prepare_transform_snapshot_ms\":" << prepareTimings.transformSnapshotMs
            << ",\"prepare_transform_compute_ms\":" << prepareTimings.transformComputeMs
            << ",\"prepare_transform_commit_ms\":" << prepareTimings.transformCommitMs
            << ",\"prepare_transform_local_updated\":" << prepareTimings.transformLocalUpdated
            << ",\"prepare_transform_world_updated\":" << prepareTimings.transformWorldUpdated
            << ",\"prepare_transform_world_reused\":" << prepareTimings.transformWorldReused
            << ",\"prepare_transform_levels\":" << prepareTimings.transformLevels
            << ",\"prepare_transform_structure_fallback\":" << prepareTimings.transformStructureFallback
            << ",\"prepare_lookup_ms\":" << prepareTimings.lookupMs
            << ",\"prepare_items_ms\":" << prepareTimings.itemsMs
            << ",\"prepare_instanced_items_reused\":" << prepareTimings.instancedRenderItemsReused
            << ",\"prepare_instanced_items_updated\":" << prepareTimings.instancedRenderItemsUpdated
            << ",\"prepare_item_validation_ms\":" << prepareTimings.itemValidationMs
            << ",\"prepare_item_textures_ms\":" << prepareTimings.itemTexturesMs
            << ",\"prepare_item_shaders_ms\":" << prepareTimings.itemShadersMs
            << ",\"prepare_item_mesh_state_ms\":" << prepareTimings.itemMeshStateMs
            << ",\"prepare_item_bounds_ms\":" << prepareTimings.itemBoundsMs
            << ",\"prepare_item_lod_ms\":" << prepareTimings.itemLodMs
            << ",\"prepare_item_build_ms\":" << prepareTimings.itemBuildMs
            << ",\"prepare_item_shader_requests\":" << prepareTimings.itemShaderRequests
            << ",\"prepare_item_shader_resolutions\":" << prepareTimings.itemShaderResolutions
            << ",\"prepare_bone_upload_ms\":" << prepareTimings.boneUploadMs
            << ",\"prepare_capacity_ms\":" << prepareTimings.capacityMs
            << ",\"prepare_spatial_ms\":" << prepareTimings.spatialMs
            << ",\"prepare_spatial_index_ms\":" << prepareTimings.spatialIndexMs
            << ",\"prepare_spatial_compare_ms\":" << prepareTimings.spatialCompareMs
            << ",\"prepare_spatial_refit_ms\":" << prepareTimings.spatialRefitMs
            << ",\"prepare_spatial_build_ms\":" << prepareTimings.spatialBuildMs
            << ",\"prepare_spatial_changed_bounds\":" << prepareTimings.spatialChangedBounds
            << ",\"prepare_spatial_active_slots\":" << prepareTimings.spatialActiveSlots
            << ",\"prepare_spatial_retained_slots\":" << prepareTimings.spatialRetainedSlots
            << ",\"prepare_spatial_reused_bounds\":" << prepareTimings.spatialReusedBounds
            << ",\"prepare_spatial_dirty_leaves\":" << prepareTimings.spatialDirtyLeaves
            << ",\"prepare_spatial_refit_nodes\":" << prepareTimings.spatialRefitNodes
            << ",\"prepare_spatial_rebuilds\":" << prepareTimings.spatialRebuilds
            << ",\"prepare_spatial_quality_rebuilds\":" << prepareTimings.spatialQualityRebuilds
            << ",\"prepare_bounds_reused\":" << prepareTimings.boundsReused
            << ",\"prepare_bounds_updated\":" << prepareTimings.boundsUpdated
            << ",\"prepare_worker_active_ms\":" << prepareTimings.workerMs
            << ",\"prepare_main_job_ms\":" << prepareTimings.mainJobMs
            << ",\"prepare_worker_wait_ms\":" << prepareTimings.workerWaitMs
            << ",\"prepare_worker_queue_ms\":" << prepareTimings.workerQueueMs
            << ",\"view_ms\":" << sample.viewMilliseconds
            << ",\"shadow_submit_ms\":" << sample.shadowSubmitMilliseconds
            << ",\"main_submit_ms\":" << sample.mainSubmitMilliseconds
            << ",\"editor_ui_ms\":" << sample.editorUiMilliseconds
            << ",\"editor_play_pipeline\":" << (sample.playPipeline ? "true" : "false")
            << ",\"simulation_ms\":" << sample.simulationMilliseconds
            << ",\"simulation_cpu_ms\":" << sample.simulationCpuMilliseconds
            << ",\"simulation_wait_ms\":" << sample.simulationWaitMilliseconds
            << ",\"simulation_overlap_ms\":" << sample.simulationOverlapMilliseconds
            << ",\"simulation_gpu_wait_ms\":" << sample.simulationGpuWaitMilliseconds
            << ",\"pipeline_record_ms\":" << sample.pipelineRecordMilliseconds
            << ",\"pipeline_compute_record_ms\":" << sample.pipelineComputeRecordMilliseconds
            << ",\"pipeline_snapshot_ms\":" << sample.pipelineSnapshotMilliseconds
            << ",\"pipeline_view_record_ms\":" << sample.pipelineViewRecordMilliseconds
            << ",\"pipeline_record_other_ms\":" << std::max(0.0,sample.pipelineRecordMilliseconds-
                sample.pipelineComputeRecordMilliseconds-sample.pipelineSnapshotMilliseconds-sample.pipelineViewRecordMilliseconds)
            << ",\"pipeline_queue_wait_ms\":" << sample.pipelineQueueWaitMilliseconds
            << ",\"pipeline_queue_waits\":" << sample.pipelineQueueWaits
            << ",\"pipeline_retained_frames\":" << sample.pipelineRetainedFrames
            << ",\"ui_render_ms\":" << uiRenderMilliseconds
            << ",\"present_ms\":" << presentMilliseconds
            << ",\"queue_submission_ms\":" << queueSubmissionMilliseconds
            << ",\"gpu_valid\":"
            << (gpuTiming.gpuTimingsValid ? "true" : "false")
            << ",\"gpu_sample_id\":" << gpuTiming.gpuSampleId
            << ",\"gpu_region_count\":" << gpuTiming.gpuRegionCount
            << ",\"gpu_total_ms\":" << gpuTotal
            << ",\"gpu_query_resolve_ms\":"
            << gpuTiming.cpuGpuQueryResolveMilliseconds
            << ",\"gpu_query_begin_ms\":"
            << gpuTiming.cpuGpuQueryBeginMilliseconds
            << ",\"gpu_shadow_ms\":" << gpuTiming.gpuMilliseconds[
                static_cast<size_t>(Engine::Graphics::GpuTimingStage::Shadow)]
            << ",\"gpu_terrain_ms\":" << gpuTiming.gpuMilliseconds[
                static_cast<size_t>(Engine::Graphics::GpuTimingStage::Terrain)]
            << ",\"gpu_grass_ms\":" << gpuTiming.gpuMilliseconds[
                static_cast<size_t>(Engine::Graphics::GpuTimingStage::Grass)]
            << ",\"gpu_portal_ms\":" << gpuTiming.gpuMilliseconds[
                static_cast<size_t>(Engine::Graphics::GpuTimingStage::Portal)]
            << ",\"gpu_opaque_ms\":" << gpuTiming.OpaqueMilliseconds()
            << ",\"gpu_transparent_ms\":" << gpuTiming.gpuMilliseconds[
                static_cast<size_t>(Engine::Graphics::GpuTimingStage::Transparent)]
            << ",\"pending_load_ms\":" << ms(sample.afterRender, sample.afterPendingLoad)
            << ",\"views\":" << sample.renderedViews
            << ",\"scene_views\":" << sample.renderedSceneViews
            << ",\"game_views\":" << sample.renderedGameViews
            << ",\"view_pixels\":" << sample.renderedViewPixels
            << ",\"game_view_width\":" << sample.gameViewWidth
            << ",\"game_view_height\":" << sample.gameViewHeight
            << ",\"shadow_draws\":" << sample.submittedShadowDraws
            << ",\"physics_substeps\":" << physicsSubsteps;
        gpuTiming.WriteOpaqueJson(output);
        static constexpr const char* stageNames[]={"pacing","background","update","render","pending_load"};
        output << ",\"stage_work\":[";
        for(size_t i=0;i<sample.stages.size();++i)
        {
            const auto& stage=sample.stages[i];
            if(i) output << ',';
            output << "{\"stage\":\"" << stageNames[i] << "\",\"wall_ms\":" << stage.wallMs
                << ",\"main_busy_ms\":" << stage.mainBusyMs
                << ",\"worker_execution_ms\":" << stage.workerMs
                << ",\"queue_delay_ms\":" << stage.queueMs
                << ",\"dependency_wait_ms\":" << stage.dependencyMs
                << ",\"scheduler_wait_ms\":" << stage.waitMs
                << ",\"render_thread_stall_ms\":" << stage.renderStallMs << '}';
        }
        output << ']';
        output << ",\"physics_dropped_ms\":" << physicsDroppedMilliseconds
            << ",\"physics_pending_ms\":" << physicsPendingMilliseconds;
        const auto writeStepArray = [&](const char* key, const auto& values)
        {
            output << ",\"" << key << "\":[";
            for (size_t index = 0; index < values.size(); ++index)
            {
                if (index) output << ',';
                output << values[index];
            }
            output << ']';
        };
        writeStepArray("physics_step_ms", bulletTimings.substepMs);
        writeStepArray("physics_bullet_world_ms", bulletTimings.bulletWorldMs);
        writeStepArray("physics_contacts_ms", bulletTimings.contactsMs);
        const auto& shadow = scene->GetLastShadowCpuTiming();
        const auto& shadowState = scene->GetShadowDebugSnapshot();
        output << ",\"shadow_state_enabled\":" << (shadowState.enabled ? 1:0)
            << ",\"shadow_state_selected_light\":" << (shadowState.hasSelectedLight ? 1:0)
            << ",\"shadow_state_cascade_count\":" << shadowState.cascadeCount
            << ",\"shadow_state_refreshed_cascade_mask\":" << shadowState.refreshedCascadeMask
            << ",\"shadow_state_atlas_resolution\":" << shadowState.atlasResolution
            << ",\"shadow_state_atlas_generation\":" << shadowState.atlasAllocationGeneration
            << ",\"shadow_state_caster_count\":" << shadowState.casterCount;
        output << ",\"shadow_state_cascade_splits\":[";
        for(size_t i=0;i<4;++i) { if(i) output << ','; output << shadowState.cascadeSplits[i]; }
        output << "]";
        output << ",\"shadow_state_cascade_radii\":[";
        for(size_t i=0;i<4;++i) { if(i) output << ','; output << shadowState.cascadeRadii[i]; }
        output << "]";
        shadow.instances.WriteJson(output,"shadow_instance_");
        output << ",\"shadow_total_ms\":" << shadow.totalMs
            << ",\"shadow_explicit_instance_draws\":" << shadow.explicitInstanceDraws
            << ",\"shadow_automatic_draws\":" << shadow.automaticDraws
            << ",\"shadow_gpu_grass_batches\":" << shadow.gpuGrassBatches
            << ",\"shadow_ordinary_draws\":" << shadow.ordinaryDraws
            << ",\"shadow_prepare_ms\":" << shadow.scenePreparationMs
            << ",\"shadow_culling_ms\":" << shadow.cascadeCullingMs
            << ",\"shadow_upload_ms\":" << shadow.recordUploadMs
            << ",\"shadow_draw_ms\":" << shadow.drawSubmissionMs
            << ",\"shadow_portal_ms\":" << shadow.portalMs;
        output << ",\"shadow_constant_bind_ms\":" << shadow.constantBindingMs
            << ",\"shadow_texture_bind_ms\":" << shadow.textureBindingMs
            << ",\"shadow_geometry_draw_ms\":" << shadow.geometryDrawMs
            << ",\"shadow_submission_wall_ms\":" << shadow.submissionWallMs
            << ",\"shadow_submission_thread_cycles\":" << shadow.submissionThreadCycles
            << ",\"shadow_max_batch_ms\":" << shadow.maxBatchMs
            << ",\"shadow_restore_ms\":" << shadow.passRestoreMs;
        const auto* factory = renderer->GetGraphicsProvider()->GetContextFactory();
        factory->GetNativeSubmissionTimings().WriteJson(output);
        output << ",\"constant_arena_writes\":" << factory->GetConstantBufferArenaWrites()
            << ",\"constant_discard_maps\":" << factory->GetConstantBufferDiscardMaps();
        output << ",\"spatial_active_fields\":" << scene->GetSpatialUpdateScheduler().activeCount
            << ",\"spatial_dormant_fields\":" << scene->GetSpatialUpdateScheduler().dormantCount;
        const auto& grass = scene->GetGrassPerformance();
        const auto& renderCpu=sample.renderCpu;
        const auto frameUploadStats = scene->GetCurrentFrameUploadStats();
        renderCpu.instances.WriteJson(output);
        output << ",\"render_metrics_scope\":\"all_main_views\""
            << ",\"frame_upload_bytes\":" << frameUploadStats.bytes
            << ",\"frame_upload_buffer_allocations\":" << frameUploadStats.bufferAllocations
            << ",\"frame_upload_allocator_growths\":" << frameUploadStats.allocatorGrowths
            << ",\"render_cpu_total_ms\":" << renderCpu.totalMs
            << ",\"render_startup_ms\":" << renderCpu.startupMs
            << ",\"render_cluster_prepare_ms\":" << renderCpu.clusterPreparationMs
            << ",\"render_cluster_cache_hits\":" << renderCpu.clusterCacheHits
            << ",\"render_cluster_upload_bytes\":" << renderCpu.clusterUploadBytes
            << ",\"render_terrain_prepare_ms\":" << renderCpu.terrainPreparationMs
            << ",\"render_occlusion_ms\":" << renderCpu.occlusionMs
            << ",\"render_other_ms\":" << renderCpu.otherMs
            << ",\"render_retained_ui_ms\":" << renderCpu.retainedUiMs
            << ",\"render_portal_ms\":" << renderCpu.portalMs
            << ",\"render_worker_active_ms\":" << renderCpu.workerActiveMs
            << ",\"render_main_job_ms\":" << renderCpu.mainJobMs
            << ",\"render_worker_wait_ms\":" << renderCpu.workerWaitMs
            << ",\"worker_metrics_scope\":\"engine_jobs_during_render\""
            << ",\"render_worker_queue_ms\":" << renderCpu.workerQueueMs
            << ",\"render_worker_dependency_ms\":" << renderCpu.workerDependencyMs
            << ",\"configured_workers\":" << renderCpu.configuredWorkers
            << ",\"render_record_pack_ms\":" << renderCpu.recordPackingMs
            << ",\"render_record_wait_ms\":" << renderCpu.recordWaitMs
            << ",\"render_worker_jobs\":" << renderCpu.workerJobs
            << ",\"render_worker_tasks\":" << renderCpu.workerTasks
            << ",\"gpu_grass_batches\":" << renderCpu.gpuGrassBatches
            << ",\"grass_batch_cache_hits\":" << renderCpu.grassBatchCacheHits
            << ",\"grass_batch_placement_upload_bytes\":" << renderCpu.grassPlacementUploadBytes
            << ",\"render_candidate_draws\":" << renderCpu.candidateDraws
            << ",\"render_physical_draws\":" << renderCpu.physicalDraws
            << ",\"render_candidate_triangles\":" << renderCpu.candidateTriangles
            << ",\"render_visibility_frame_items\":" << renderCpu.visibilityFrameItems
            << ",\"render_visibility_candidates\":" << renderCpu.visibilityCandidates
            << ",\"render_visibility_accepted\":" << renderCpu.visibilityAccepted
            << ",\"render_camera_visibility_views\":" << renderCpu.cameraVisibility.views
            << ",\"render_camera_visibility_frame_items\":" << renderCpu.cameraVisibility.frameItems
            << ",\"render_camera_visibility_candidates\":" << renderCpu.cameraVisibility.candidateItems
            << ",\"render_camera_visibility_accepted\":" << renderCpu.cameraVisibility.acceptedItems
            << ",\"render_camera_bvh_nodes_visited\":" << renderCpu.cameraVisibility.bvhNodesVisited
            << ",\"render_camera_bvh_bounds_tested\":" << renderCpu.cameraVisibility.bvhBoundsTested
            << ",\"render_camera_visible_slots\":" << renderCpu.cameraVisibility.visibleSlots
            << ",\"render_camera_instance_tiles_submitted\":" << renderCpu.cameraVisibility.instanceTilesSubmitted
            << ",\"render_camera_non_terrain_instance_tiles_submitted\":" << renderCpu.cameraVisibility.nonTerrainInstanceTilesSubmitted
            << ",\"render_camera_placement_candidates\":" << renderCpu.cameraVisibility.placementCandidates
            << ",\"render_camera_draw_groups\":" << renderCpu.cameraVisibility.drawGroups
            << ",\"render_camera_material_groups\":" << renderCpu.cameraVisibility.materialGroups
            << ",\"render_camera_screen_selected_instances_lod0\":" << renderCpu.cameraVisibility.screenSelectedInstancesByLod[0]
            << ",\"render_camera_screen_selected_instances_lod1\":" << renderCpu.cameraVisibility.screenSelectedInstancesByLod[1]
            << ",\"render_camera_screen_selected_instances_lod2\":" << renderCpu.cameraVisibility.screenSelectedInstancesByLod[2]
            << ",\"render_camera_screen_selected_triangles_lod0\":" << renderCpu.cameraVisibility.screenSelectedTrianglesByLod[0]
            << ",\"render_camera_screen_selected_triangles_lod1\":" << renderCpu.cameraVisibility.screenSelectedTrianglesByLod[1]
            << ",\"render_camera_screen_selected_triangles_lod2\":" << renderCpu.cameraVisibility.screenSelectedTrianglesByLod[2]
            << ",\"render_camera_placement_candidates_lod0\":" << renderCpu.cameraVisibility.placementCandidatesByLod[0]
            << ",\"render_camera_placement_candidates_lod1\":" << renderCpu.cameraVisibility.placementCandidatesByLod[1]
            << ",\"render_camera_placement_candidates_lod2\":" << renderCpu.cameraVisibility.placementCandidatesByLod[2]
            << ",\"render_camera_placement_triangles_lod0\":" << renderCpu.cameraVisibility.placementTrianglesByLod[0]
            << ",\"render_camera_placement_triangles_lod1\":" << renderCpu.cameraVisibility.placementTrianglesByLod[1]
            << ",\"render_camera_placement_triangles_lod2\":" << renderCpu.cameraVisibility.placementTrianglesByLod[2]
            << ",\"render_shadow_visibility_views\":" << renderCpu.shadowVisibility.views
            << ",\"render_shadow_visibility_frame_items\":" << renderCpu.shadowVisibility.frameItems
            << ",\"render_shadow_visibility_candidates\":" << renderCpu.shadowVisibility.candidateItems
            << ",\"render_shadow_visibility_accepted\":" << renderCpu.shadowVisibility.acceptedItems
            << ",\"render_shadow_bvh_nodes_visited\":" << renderCpu.shadowVisibility.bvhNodesVisited
            << ",\"render_shadow_bvh_bounds_tested\":" << renderCpu.shadowVisibility.bvhBoundsTested
            << ",\"render_shadow_visible_slots\":" << renderCpu.shadowVisibility.visibleSlots
            << ",\"render_shadow_instance_tiles_submitted\":" << renderCpu.shadowVisibility.instanceTilesSubmitted
            << ",\"render_shadow_non_terrain_instance_tiles_submitted\":" << renderCpu.shadowVisibility.nonTerrainInstanceTilesSubmitted
            << ",\"render_shadow_placement_candidates\":" << renderCpu.shadowVisibility.placementCandidates
            << ",\"render_shadow_draw_groups\":" << renderCpu.shadowVisibility.drawGroups
            << ",\"render_shadow_material_groups\":" << renderCpu.shadowVisibility.materialGroups
            << ",\"render_shadow_placement_candidates_lod0\":" << renderCpu.shadowVisibility.placementCandidatesByLod[0]
            << ",\"render_shadow_placement_candidates_lod1\":" << renderCpu.shadowVisibility.placementCandidatesByLod[1]
            << ",\"render_shadow_placement_candidates_lod2\":" << renderCpu.shadowVisibility.placementCandidatesByLod[2]
            << ",\"render_shadow_placement_triangles_lod0\":" << renderCpu.shadowVisibility.placementTrianglesByLod[0]
            << ",\"render_shadow_placement_triangles_lod1\":" << renderCpu.shadowVisibility.placementTrianglesByLod[1]
            << ",\"render_shadow_placement_triangles_lod2\":" << renderCpu.shadowVisibility.placementTrianglesByLod[2]
            << ",\"render_camera_gpu_selected_instances_lod0\":" << renderCpu.cameraVisibility.gpuSelectedInstancesByLod[0]
            << ",\"render_camera_gpu_selected_instances_lod1\":" << renderCpu.cameraVisibility.gpuSelectedInstancesByLod[1]
            << ",\"render_camera_gpu_selected_instances_lod2\":" << renderCpu.cameraVisibility.gpuSelectedInstancesByLod[2]
            << ",\"render_camera_gpu_transition_instances_lod0\":" << renderCpu.cameraVisibility.gpuTransitionBandInstancesByLod[0]
            << ",\"render_camera_gpu_transition_instances_lod1\":" << renderCpu.cameraVisibility.gpuTransitionBandInstancesByLod[1]
            << ",\"render_camera_gpu_transition_instances_lod2\":" << renderCpu.cameraVisibility.gpuTransitionBandInstancesByLod[2]
            << ",\"render_camera_gpu_readback_samples\":" << renderCpu.cameraVisibility.gpuReadbackSamples
            << ",\"render_shadow_gpu_selected_instances_lod0\":" << renderCpu.shadowVisibility.gpuSelectedInstancesByLod[0]
            << ",\"render_shadow_gpu_selected_instances_lod1\":" << renderCpu.shadowVisibility.gpuSelectedInstancesByLod[1]
            << ",\"render_shadow_gpu_selected_instances_lod2\":" << renderCpu.shadowVisibility.gpuSelectedInstancesByLod[2]
            << ",\"render_shadow_gpu_transition_instances_lod0\":" << renderCpu.shadowVisibility.gpuTransitionBandInstancesByLod[0]
            << ",\"render_shadow_gpu_transition_instances_lod1\":" << renderCpu.shadowVisibility.gpuTransitionBandInstancesByLod[1]
            << ",\"render_shadow_gpu_transition_instances_lod2\":" << renderCpu.shadowVisibility.gpuTransitionBandInstancesByLod[2]
            << ",\"render_shadow_gpu_readback_samples\":" << renderCpu.shadowVisibility.gpuReadbackSamples
            << ",\"render_camera_view_identity\":" << renderCpu.cameraVisibility.viewIdentity
            << ",\"render_shadow_view_identity\":" << renderCpu.shadowVisibility.viewIdentity
            << ",\"render_color_selected_lod0_draws\":" << renderCpu.colorSelectedLodDraws[0]
            << ",\"render_color_selected_lod1_draws\":" << renderCpu.colorSelectedLodDraws[1]
            << ",\"render_color_selected_lod2_draws\":" << renderCpu.colorSelectedLodDraws[2]
            << ",\"render_shadow_selected_lod0_draws\":" << renderCpu.shadowSelectedLodDraws[0]
            << ",\"render_shadow_selected_lod1_draws\":" << renderCpu.shadowSelectedLodDraws[1]
            << ",\"render_shadow_selected_lod2_draws\":" << renderCpu.shadowSelectedLodDraws[2]
            << ",\"render_submitted_instances\":" << renderCpu.submittedInstances
            << ",\"render_submitted_triangles\":" << renderCpu.submittedTriangles
            << ",\"render_terrain_occlusion_rejected\":" << renderCpu.terrainOcclusionRejected
            << ",\"render_solid_occlusion_rejected\":" << renderCpu.solidOcclusionRejected
            << ",\"render_culling_ms\":" << renderCpu.cullingMs
            << ",\"render_sorting_ms\":" << renderCpu.sortingMs
            << ",\"render_material_ms\":" << renderCpu.materialMs
            << ",\"render_upload_ms\":" << renderCpu.uploadMs
            << ",\"render_submission_ms\":" << renderCpu.submissionMs
            << ",\"render_batching_ms\":" << renderCpu.batchingMs
            << ",\"render_batch_snapshot_ms\":" << renderCpu.batchSnapshotMs
            << ",\"render_instance_tile_prepare_ms\":" << renderCpu.instanceTilePrepareMs
            << ",\"render_instance_tile_reused\":" << renderCpu.instanceTileReused
            << ",\"render_instance_tile_built\":" << renderCpu.instanceTileBuilt
            << ",\"render_instance_tile_worker_passes\":" << renderCpu.instanceTileWorkerPasses
            << ",\"render_batch_build_ms\":" << renderCpu.batchBuildMs
            << ",\"render_batch_wait_ms\":" << renderCpu.batchWaitMs
            << ",\"render_batch_eligible_draws\":" << renderCpu.batchEligibleDraws
            << ",\"render_batch_cache_hits\":" << renderCpu.batchCacheHits
            << ",\"render_batch_worker_jobs\":" << renderCpu.batchWorkerJobs
            << ",\"render_batch_reused_keys\":" << renderCpu.batchReusedKeys
            << ",\"automatic_batches\":" << renderCpu.automaticBatches
            << ",\"automatic_instances\":" << renderCpu.automaticInstances
            << ",\"gpu_instance_batches\":" << renderCpu.gpuBatches
            << ",\"render_changed_upload_bytes\":" << renderCpu.changedUploadBytes;
        output << ",\"grass_total_ms\":" << grass.totalMs
            << ",\"grass_references_ms\":" << grass.referencesMs
            << ",\"grass_sources_ms\":" << grass.sourcesMs
            << ",\"grass_placement_ms\":" << grass.placementMs
            << ",\"grass_visibility_ms\":" << grass.visibilityMs
            << ",\"grass_retirement_ms\":" << grass.retirementMs
            << ",\"grass_scheduling_ms\":" << grass.schedulingMs
            << ",\"grass_build_ms\":" << grass.buildMs
            << ",\"grass_accounting_ms\":" << grass.accountingMs
            << ",\"grass_other_ms\":" << grass.otherMs
            << ",\"grass_scan_ms\":" << grass.scanMs
            << ",\"grass_generation_ms\":" << grass.generationMs
            << ",\"grass_upload_ms\":" << grass.uploadMs
            << ",\"grass_placements_scanned\":" << grass.placementsScanned
            << ",\"grass_tiles_built\":" << grass.tilesBuilt
            << ",\"grass_lod_switches\":" << grass.lodSwitches
            << ",\"grass_pending\":" << grass.pending
            << ",\"grass_resident_lods\":" << grass.residentLods
            << ",\"grass_gpu_bytes\":" << grass.gpuBytes
            << ",\"grass_instances\":" << grass.instances
            << ",\"grass_instance_upload_bytes\":" << grass.instanceUploadBytes;
        const auto writeViewIdentities = [&output](const char* field,
            const Engine::Rendering::RenderVisibilityMetrics& visibility)
        {
            output << ",\"" << field << "\":[";
            for (size_t i=0;i<visibility.viewIdentities.size();++i)
            {
                if (i) output << ',';
                const auto& id=visibility.viewIdentities[i];
                output << "{\"id\":" << id.id << ",\"portal\":"
                    << (id.portal ? "true":"false") << ",\"shadow\":"
                    << (id.shadow ? "true":"false") << '}';
            }
            output << ']';
        };
        writeViewIdentities("render_camera_view_identities",renderCpu.cameraVisibility);
        writeViewIdentities("render_shadow_view_identities",renderCpu.shadowVisibility);
        output << ",\"grass_active_tiles\":" << grass.activeTiles
            << ",\"grass_lod_transition_tiles\":" << grass.transitionTiles
            << ",\"grass_fading_tiles\":" << grass.fadingTiles;
        output << ",\"measured\":" << (sample.measured ? "true":"false");
        output << ",\"recurring_frame\":" << ((!sample.startup && !sample.activity) ? "true":"false")
            << ",\"frame_activity\":[";
        bool activityWritten = false;
        const auto activity = [&](uint32_t bit, const char* name)
        {
            if (sample.activity & bit)
            {
                if (activityWritten) output << ',';
                output << '"' << name << '"';
                activityWritten = true;
            }
        };
        if (sample.startup) { output << "\"startup\""; activityWritten = true; }
        activity(FrameActivity::Import, "import");
        activity(FrameActivity::Save, "save");
        activity(FrameActivity::Bake, "bake");
        activity(FrameActivity::Load, "load");
        activity(FrameActivity::PlayStart, "play_start");
        activity(FrameActivity::PlayStop, "play_stop");
        output << "]"
            << ",\"ui_thread_cpu_ms\":" << sample.uiCpuMilliseconds
            << ",\"render_thread_cpu_ms\":" << sample.stages[3].mainBusyMs
            << ",\"worker_queue_pending\":" << sample.workerPending
            << ",\"worker_count\":" << sample.workerCount
            << ",\"writer_queue_records\":" << sample.writer.queuedRecords
            << ",\"writer_queue_bytes\":" << sample.writer.queuedBytes
            << ",\"writer_written_records\":" << sample.writer.written
            << ",\"writer_dropped_records\":" << sample.writer.dropped
            << ",\"writer_failed_records\":" << sample.writer.failed;
        output << ",\"ui_panels\":[";
        double panelTotal = 0;
        bool firstPanel = true;
        for (const auto& panel : sample.uiPanels)
        {
            if (!firstPanel) output << ',';
            firstPanel = false;
            output << "{\"title\":\"";
            // Titles can contain asset paths or user-authored names. Own and
            // escape them before the asynchronous writer sees the sample.
            constexpr char hex[] = "0123456789abcdef";
            for (unsigned char c : panel.title)
            {
                if (c == '"' || c == '\\') output << '\\' << char(c);
                else if (c < 0x20) output << "\\u00" << hex[c >> 4] << hex[c & 15];
                else output << char(c);
            }
            output << "\",\"draw_ms\":" << panel.milliseconds << '}';
            panelTotal += panel.milliseconds;
        }
        output << "],\"ui_panels_ms\":" << panelTotal
            << ",\"ui_other_ms\":" << std::max(0.0, sample.editorUiMilliseconds - panelTotal);
        return output;
    }

    void EditorFrameProfiler::Stop()
    {
        if(output_.IsOpen()) output_.Enqueue([] { return "{\"type\":\"capture_complete\"}"; },64);
        output_.Stop();
    }

    void EditorFrameProfiler::Record(const EditorFrameProfileSample& sample,
        Scene::Scene* scene, Renderers::IEditorRenderer* renderer)
    {
#if defined(_WIN32)
        const auto now=std::chrono::steady_clock::now();
        const double seconds=std::chrono::duration<double>(now-cpuSampleStart_).count();
        if(cpuSampleStart_.time_since_epoch().count()==0 || seconds>=.5)
        {
            FILETIME created{},exited{},kernel{},user{};
            const auto ticks=[](FILETIME value) { return (uint64_t(value.dwHighDateTime)<<32)|value.dwLowDateTime; };
            if(GetProcessTimes(GetCurrentProcess(),&created,&exited,&kernel,&user))
            {
                const auto process=ticks(kernel)+ticks(user);
                if(GetThreadTimes(GetCurrentThread(),&created,&exited,&kernel,&user))
                {
                    const auto thread=ticks(kernel)+ticks(user);
                    if(cpuSampleStart_.time_since_epoch().count()!=0)
                    {
                        const double cores=double(process-processTicks_)/(seconds*1.e7);
                        const double mainPercent=100.*double(thread-threadTicks_)/(seconds*1.e7);
                        const auto processors=std::max<DWORD>(1,GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
                        Engine::Threading::CapturedText fields;
                        fields << "{\"type\":\"sample\",\"playing\":" << (sample.playing ? "true":"false")
                            << ",\"measured\":" << (sample.measured ? "true":"false")
                            << ",\"process_cpu_core_equivalents\":" << cores
                            << ",\"process_cpu_pct\":" << cores*100./processors
                            << ",\"main_thread_cpu_pct\":" << mainPercent << "}";
                        const auto bytes=fields.OwnedBytes();
                        output_.Enqueue([fields=std::move(fields)] { return fields.Format(); },bytes);
                    }
                    processTicks_=process; threadTicks_=thread; cpuSampleStart_=now;
                }
            }
        }
#endif
        auto fields=CaptureFields(sample,scene,renderer);
        const auto bytes=fields.OwnedBytes(),index=frameIndex_++;
        output_.Enqueue([fields=std::move(fields),index] {
            return "{\"frame\":"+std::to_string(index)+","+fields.Format()+"}";
        },bytes);
    }
}

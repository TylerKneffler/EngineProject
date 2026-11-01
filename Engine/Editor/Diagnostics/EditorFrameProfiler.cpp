#include "Engine/Editor/Diagnostics/EditorFrameProfiler.h"

#include "Core/Physics/Adapters/BulletPhysicsAdapter.h"
#include "Core/Renderers/DX11/DX11EditorRenderer.h"
#include "Core/Renderers/IEditorRenderer.h"
#include "Core/Scene/Scene.h"

#include <cstdlib>
#include <filesystem>
#include <string>
#include <sstream>
#include <locale>

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
            output_.open(path, std::ios::trunc);
        }
        scene->SetUpdateProfilingEnabled(output_.is_open());
        if (auto* bullet = dynamic_cast<Engine::Physics::BulletPhysicsAdapter*>(
            &scene->GetPhysics().GetAdapter()))
            bullet->SetProfilingEnabled(output_.is_open());
        if (auto* dx11 = dynamic_cast<Engine::Renderers::DX11EditorRenderer*>(renderer))
            dx11->SetGpuTimingEnabled(output_.is_open());
    }

    std::string EditorFrameProfiler::SerializeFields(const EditorFrameProfileSample& sample,
        Scene::Scene* scene, Renderers::IEditorRenderer* renderer)
    {
        std::ostringstream output;
        output.imbue(std::locale::classic());
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
        double presentMilliseconds = 0.0;
        double prepareFrameMilliseconds = 0.0;
        double beginUiMilliseconds = 0.0;
        double renderSetupMilliseconds = 0.0;
        const auto gpuTiming = renderer->GetGraphicsProvider()
            ->GetContextFactory()->GetFrameTimingTelemetry();
        if (auto* dx11 = dynamic_cast<Engine::Renderers::DX11EditorRenderer*>(
            renderer))
        {
            uiRenderMilliseconds = dx11->GetLastUiRenderMilliseconds();
            presentMilliseconds = dx11->GetLastPresentMilliseconds();
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
        output << "\"schema_version\":2,\"playing\":"
            << (sample.playing ? "true" : "false")
            << ",\"dt_ms\":" << sample.deltaSeconds * 1000.f
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
            << ",\"prepare_lookup_ms\":" << prepareTimings.lookupMs
            << ",\"prepare_items_ms\":" << prepareTimings.itemsMs
            << ",\"prepare_bone_upload_ms\":" << prepareTimings.boneUploadMs
            << ",\"prepare_capacity_ms\":" << prepareTimings.capacityMs
            << ",\"prepare_spatial_ms\":" << prepareTimings.spatialMs
            << ",\"view_ms\":" << sample.viewMilliseconds
            << ",\"shadow_submit_ms\":" << sample.shadowSubmitMilliseconds
            << ",\"main_submit_ms\":" << sample.mainSubmitMilliseconds
            << ",\"editor_ui_ms\":" << sample.editorUiMilliseconds
            << ",\"ui_render_ms\":" << uiRenderMilliseconds
            << ",\"present_ms\":" << presentMilliseconds
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
            << ",\"gpu_portal_ms\":" << gpuTiming.gpuMilliseconds[
                static_cast<size_t>(Engine::Graphics::GpuTimingStage::Portal)]
            << ",\"gpu_opaque_ms\":" << gpuTiming.gpuMilliseconds[
                static_cast<size_t>(Engine::Graphics::GpuTimingStage::Opaque)]
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
        output << ",\"shadow_total_ms\":" << shadow.totalMs
            << ",\"shadow_explicit_instance_draws\":" << shadow.explicitInstanceDraws
            << ",\"shadow_automatic_draws\":" << shadow.automaticDraws
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
        output << ",\"constant_arena_writes\":" << factory->GetConstantBufferArenaWrites()
            << ",\"constant_discard_maps\":" << factory->GetConstantBufferDiscardMaps();
        output << ",\"spatial_active_fields\":" << scene->GetSpatialUpdateScheduler().activeCount
            << ",\"spatial_dormant_fields\":" << scene->GetSpatialUpdateScheduler().dormantCount;
        const auto& grass = scene->GetGrassPerformance();
        const auto& renderCpu=sample.renderCpu;
        output << ",\"render_metrics_scope\":\"all_main_views\""
            << ",\"render_cpu_total_ms\":" << renderCpu.totalMs
            << ",\"render_startup_ms\":" << renderCpu.startupMs
            << ",\"render_terrain_prepare_ms\":" << renderCpu.terrainPreparationMs
            << ",\"render_occlusion_ms\":" << renderCpu.occlusionMs
            << ",\"render_other_ms\":" << renderCpu.otherMs
            << ",\"render_retained_ui_ms\":" << renderCpu.retainedUiMs
            << ",\"render_portal_ms\":" << renderCpu.portalMs
            << ",\"render_worker_active_ms\":" << renderCpu.workerActiveMs
            << ",\"render_main_job_ms\":" << renderCpu.mainJobMs
            << ",\"render_worker_wait_ms\":" << renderCpu.workerWaitMs
            << ",\"render_record_pack_ms\":" << renderCpu.recordPackingMs
            << ",\"render_record_wait_ms\":" << renderCpu.recordWaitMs
            << ",\"render_worker_jobs\":" << renderCpu.workerJobs
            << ",\"render_worker_tasks\":" << renderCpu.workerTasks
            << ",\"render_candidate_draws\":" << renderCpu.candidateDraws
            << ",\"render_physical_draws\":" << renderCpu.physicalDraws
            << ",\"render_candidate_triangles\":" << renderCpu.candidateTriangles
            << ",\"render_culling_ms\":" << renderCpu.cullingMs
            << ",\"render_sorting_ms\":" << renderCpu.sortingMs
            << ",\"render_material_ms\":" << renderCpu.materialMs
            << ",\"render_upload_ms\":" << renderCpu.uploadMs
            << ",\"render_submission_ms\":" << renderCpu.submissionMs
            << ",\"render_batching_ms\":" << renderCpu.batchingMs
            << ",\"automatic_batches\":" << renderCpu.automaticBatches
            << ",\"automatic_instances\":" << renderCpu.automaticInstances
            << ",\"gpu_instance_batches\":" << renderCpu.gpuBatches
            << ",\"render_changed_upload_bytes\":" << renderCpu.changedUploadBytes;
        output << ",\"grass_total_ms\":" << grass.totalMs
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
        output << ",\"grass_active_tiles\":" << grass.activeTiles
            << ",\"grass_lod_transition_tiles\":" << grass.transitionTiles
            << ",\"grass_fading_tiles\":" << grass.fadingTiles;
        return output.str();
    }

    void EditorFrameProfiler::Record(const EditorFrameProfileSample& sample,
        Scene::Scene* scene, Renderers::IEditorRenderer* renderer)
    {
        output_ << "{\"frame\":" << frameIndex_ << ","
            << SerializeFields(sample, scene, renderer) << "}\n";
        if (++frameIndex_ % 30u == 0u)
            output_.flush();
    }
}

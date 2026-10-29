#include "Engine/Editor/Diagnostics/EditorFrameProfiler.h"

#include "Core/Physics/Adapters/BulletPhysicsAdapter.h"
#include "Core/Renderers/DX11/DX11EditorRenderer.h"
#include "Core/Renderers/IEditorRenderer.h"
#include "Core/Scene/Scene.h"

#include <cstdlib>
#include <filesystem>
#include <string>

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

    void EditorFrameProfiler::Record(const EditorFrameProfileSample& sample,
        Scene::Scene* scene, Renderers::IEditorRenderer* renderer)
    {
        const auto ms = [](auto first, auto last)
        {
            return std::chrono::duration<double, std::milli>(
                last - first).count();
        };
        uint32_t physicsSubsteps = 0;
        double physicsDroppedMilliseconds = 0.0;
        double physicsPendingMilliseconds = 0.0;
        Engine::Physics::BulletPhysicsAdapter::StepTimings bulletTimings{};
        const auto& updateTimings = scene->GetLastUpdateTimings();
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
        output_ << "{\"frame\":" << frameIndex_
            << ",\"playing\":"
            << (sample.playing ? "true" : "false")
            << ",\"dt_ms\":" << sample.deltaSeconds * 1000.f
            << ",\"frame_ms\":" << ms(sample.frameBegin, sample.afterPendingLoad)
            << ",\"pacing_ms\":" << ms(sample.frameBegin, sample.afterPacing)
            << ",\"background_ms\":" << ms(sample.afterPacing, sample.afterBackground)
            << ",\"update_ms\":" << ms(sample.afterBackground, sample.afterSceneUpdate)
            << ",\"objects_ms\":" << updateTimings.objectsMs
            << ",\"pose_ms\":" << updateTimings.poseMs
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
            << ",\"view_ms\":" << sample.viewMilliseconds
            << ",\"shadow_submit_ms\":" << sample.shadowSubmitMilliseconds
            << ",\"main_submit_ms\":" << sample.mainSubmitMilliseconds
            << ",\"editor_ui_ms\":" << sample.editorUiMilliseconds
            << ",\"ui_render_ms\":" << uiRenderMilliseconds
            << ",\"present_ms\":" << presentMilliseconds
            << ",\"gpu_valid\":"
            << (gpuTiming.gpuTimingsValid ? "true" : "false")
            << ",\"gpu_sample_id\":" << gpuTiming.gpuSampleId
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
        output_ << ",\"physics_dropped_ms\":" << physicsDroppedMilliseconds
            << ",\"physics_pending_ms\":" << physicsPendingMilliseconds;
        const auto writeStepArray = [&](const char* key, const auto& values)
        {
            output_ << ",\"" << key << "\":[";
            for (size_t index = 0; index < values.size(); ++index)
            {
                if (index) output_ << ',';
                output_ << values[index];
            }
            output_ << ']';
        };
        writeStepArray("physics_step_ms", bulletTimings.substepMs);
        writeStepArray("physics_bullet_world_ms", bulletTimings.bulletWorldMs);
        writeStepArray("physics_contacts_ms", bulletTimings.contactsMs);
        output_ << "}\n";
        if (++frameIndex_ % 30u == 0u)
            output_.flush();
    }
}

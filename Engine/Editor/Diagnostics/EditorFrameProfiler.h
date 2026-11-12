#pragma once

#include <chrono>
#include <cstdint>
#include <array>
#include <fstream>
#include <string>
#include <vector>
#include "Core/Rendering/RenderPerformance.h"
#include "Core/Threading/CapturedText.h"
#include "Core/Threading/AsyncTextWriter.h"

namespace Engine::Renderers { class IEditorRenderer; }
namespace Engine::Scene { class Scene; }

namespace Engine::Editor
{
    struct EditorFrameProfileSample
    {
        struct StageWork
        {
            double wallMs=0, mainBusyMs=0, workerMs=0, queueMs=0;
            double dependencyMs=0, waitMs=0, renderStallMs=0;
        };
        // Pacing, background, update, render, pending load, in that order.
        std::array<StageWork,5> stages{};
        using TimePoint = std::chrono::steady_clock::time_point;
        TimePoint frameBegin, afterPacing, afterBackground, afterSceneUpdate;
        TimePoint afterRender, afterPendingLoad;
        float deltaSeconds = 0.f; // Simulation delta; may reset on play transitions.
        double updateIntervalMs = 0, submissionIntervalMs = 0, idleWaitMs = 0;
        std::string scenePath;
        uint64_t submittedFrames = 0;
        bool submissionCountValid = false;
        uint64_t playSnapshotBytes=0, sceneHistoryBytes=0;
        bool playing = false;
        bool measured = true;
        bool playPipeline = false;
        bool startup = false;
        uint32_t activity = 0;
        double uiCpuMilliseconds = 0;
        bool uiToolbarTimingValid = false;
        double uiToolbarMilliseconds = 0, uiToolbarLodSummaryMilliseconds = 0;
        std::array<double, 9> uiToolbarSectionMilliseconds{};
        std::array<uint32_t, 9> uiToolbarSectionDraws{};
        uint32_t uiToolbarLodSummaryCalls = 0;
        struct UiPanel { std::string title; double milliseconds = 0; };
        std::vector<UiPanel> uiPanels;
        struct ViewRender
        {
            std::string kind;
            uint32_t width=0, height=0;
            Engine::Rendering::RenderCpuTimings renderCpu;
        };
        std::vector<ViewRender> viewRenders;
        uint64_t workerPending = 0;
        unsigned workerCount = 0;
        Engine::Threading::AsyncTextWriter::Statistics writer;
        double simulationMilliseconds = 0, simulationCpuMilliseconds = 0;
        double simulationWaitMilliseconds = 0, simulationOverlapMilliseconds = 0;
        double simulationGpuWaitMilliseconds = 0, pipelineRecordMilliseconds = 0;
        double pipelineComputeRecordMilliseconds=0,pipelineSnapshotMilliseconds=0,pipelineViewRecordMilliseconds=0;
        double pipelineQueueWaitMilliseconds = 0;
        uint32_t pipelineQueueWaits = 0, pipelineRetainedFrames = 0;
        double prepareMilliseconds = 0.0;
        double renderEntryMilliseconds = 0.0;
        double clearMilliseconds = 0.0;
        double scenePrepareMilliseconds = 0.0;
        double viewMilliseconds = 0.0;
        double shadowSubmitMilliseconds = 0.0;
        double mainSubmitMilliseconds = 0.0;
        double editorUiMilliseconds = 0.0;
        uint32_t renderedViews = 0;
        uint32_t renderedSceneViews = 0;
        uint32_t renderedGameViews = 0;
        uint64_t renderedViewPixels = 0;
        uint32_t gameViewWidth = 0;
        uint32_t gameViewHeight = 0;
        uint32_t submittedShadowDraws = 0;
        Engine::Rendering::RenderCpuTimings renderCpu;
    };

    class EditorFrameProfiler
    {
    public:
        EditorFrameProfiler(Scene::Scene* scene, Renderers::IEditorRenderer* renderer,
            bool autoProfilePlay);
        explicit operator bool() const { return output_.IsOpen(); }
        // Shared by the opt-in trace and normal Performance recordings.
        static std::string SerializeFields(const EditorFrameProfileSample& sample,
            Scene::Scene* scene, Renderers::IEditorRenderer* renderer);
        static Engine::Threading::CapturedText CaptureFields(const EditorFrameProfileSample& sample,
            Scene::Scene* scene, Renderers::IEditorRenderer* renderer);
        void Stop();
        void Record(const EditorFrameProfileSample& sample, Scene::Scene* scene,
            Renderers::IEditorRenderer* renderer);
        Engine::Threading::AsyncTextWriter::Statistics GetWriterStatistics() const
        { return output_.ReadStatistics(); }

    private:
        Engine::Threading::AsyncTextWriter output_;
        uint64_t frameIndex_ = 0;
        std::chrono::steady_clock::time_point cpuSampleStart_{};
        uint64_t processTicks_=0,threadTicks_=0;
    };
}

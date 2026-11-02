#pragma once

#include <chrono>
#include <cstdint>
#include <fstream>
#include <string>
#include "Core/Rendering/RenderPerformance.h"
#include "Core/Threading/CapturedText.h"
#include "Core/Threading/AsyncTextWriter.h"

namespace Engine::Renderers { class IEditorRenderer; }
namespace Engine::Scene { class Scene; }

namespace Engine::Editor
{
    struct EditorFrameProfileSample
    {
        using TimePoint = std::chrono::steady_clock::time_point;
        TimePoint frameBegin, afterPacing, afterBackground, afterSceneUpdate;
        TimePoint afterRender, afterPendingLoad;
        float deltaSeconds = 0.f;
        bool playing = false;
        bool measured = true;
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

    private:
        Engine::Threading::AsyncTextWriter output_;
        uint64_t frameIndex_ = 0;
        std::chrono::steady_clock::time_point cpuSampleStart_{};
        uint64_t processTicks_=0,threadTicks_=0;
    };
}

#pragma once

#include <chrono>
#include <cstdint>
#include <fstream>

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
        uint32_t submittedShadowDraws = 0;
    };

    class EditorFrameProfiler
    {
    public:
        EditorFrameProfiler(Scene::Scene* scene, Renderers::IEditorRenderer* renderer,
            bool autoProfileMmdPlay);
        explicit operator bool() const { return output_.is_open(); }
        void Record(const EditorFrameProfileSample& sample, Scene::Scene* scene,
            Renderers::IEditorRenderer* renderer);

    private:
        std::ofstream output_;
        uint64_t frameIndex_ = 0;
    };
}

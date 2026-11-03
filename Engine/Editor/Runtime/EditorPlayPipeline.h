#pragma once
#include "Core/Scene/Scene.h"
#include "Core/Input/NativeInputSnapshot.h"
#include "Core/Graphics/GraphicsCommandPacket.h"
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace Engine::Renderers { class IEditorRenderer; }

namespace Engine::Editor
{
// One bounded simulation tick overlaps replay of an already recorded frame.
// Live editor access resumes only after Finish, outside RenderIfNeeded.
class EditorPlayPipeline
{
public:
    using Commands = std::shared_ptr<const Graphics::GraphicsCommandPacket>;
    struct Timings
    {
        double updateMs = 0, updateCpuMs = 0, waitMs = 0, overlapMs = 0, gpuWaitMs = 0;
        double queueWaitMs = 0;
        uint32_t queueWaits = 0, retainedFrames = 0;
    };
    explicit EditorPlayPipeline(Renderers::IEditorRenderer& renderer);
    ~EditorPlayPipeline();
    EditorPlayPipeline(const EditorPlayPipeline&) = delete;
    EditorPlayPipeline& operator=(const EditorPlayPipeline&) = delete;
    void Begin(Scene::Scene& scene, float delta, Core::NativeInputSnapshot input);
    // Call after frame submission. Services destructive-lifecycle GPU waits on
    // the graphics owner, then restores ownership even if Update threw.
    Timings Finish(std::vector<Commands> commands = {});
    void Drain();
    bool Busy() const;
    std::shared_ptr<Graphics::GraphicsCommandPacket> AcquireCommands();

private:
    using Clock = std::chrono::steady_clock;
    void Run();
    void WaitForGraphics();
    struct QueueCollection
    {
        double waitMs = 0;
        uint32_t waits = 0, retainedFrames = 0;
    };
    QueueCollection CollectCompletedFrames(bool boundQueue = true);
    Renderers::IEditorRenderer& m_renderer;
    const std::thread::id m_editorOwner;
    std::thread::id m_simulationOwner;
    mutable std::mutex m_mutex;
    std::condition_variable m_wake;
    std::thread m_worker;
    Scene::Scene* m_scene = nullptr;
    Core::NativeInputSnapshot m_input;
    float m_delta = 0;
    bool m_ready = false, m_pending = false, m_done = false, m_stop = false;
    bool m_gpuRequested = false, m_gpuCompleted = false;
    std::exception_ptr m_failure, m_gpuFailure;
    Clock::time_point m_started, m_finished;
    double m_updateCpuMs = 0;
    struct RetiredFrame
    {
        uint64_t token;
        std::vector<Commands> commands;
        Scene::Scene::RetiredGraphicsResources resources;
    };
    std::deque<RetiredFrame> m_frames;
    std::vector<std::shared_ptr<Graphics::GraphicsCommandPacket>> m_recycledCommands;
};
}

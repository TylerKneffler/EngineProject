#include "Engine/Editor/Runtime/EditorPlayPipeline.h"
#include "Core/Renderers/IEditorRenderer.h"
#include <algorithm>
#include <stdexcept>
#include <utility>

namespace Engine::Editor
{
namespace
{
double ThreadCpuMs()
{
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user)) return 0;
    const auto ticks = [](FILETIME time) {
        return (uint64_t(time.dwHighDateTime) << 32) | time.dwLowDateTime;
    };
    return double(ticks(kernel) + ticks(user)) / 10000.;
}
}

EditorPlayPipeline::EditorPlayPipeline(Renderers::IEditorRenderer& renderer)
    : m_renderer(renderer), m_editorOwner(std::this_thread::get_id())
{
    m_worker = std::thread([this] { Run(); });
    std::unique_lock lock(m_mutex);
    m_wake.wait(lock, [this] { return m_ready; });
}

EditorPlayPipeline::~EditorPlayPipeline()
{
    try { Drain(); } catch (...) {}
    { std::lock_guard lock(m_mutex); m_stop = true; }
    m_wake.notify_all();
    m_worker.join();
}

bool EditorPlayPipeline::Busy() const
{
    std::lock_guard lock(m_mutex);
    return m_pending;
}

void EditorPlayPipeline::Begin(Scene::Scene& scene, float delta, Core::NativeInputSnapshot input)
{
    if (std::this_thread::get_id() != m_editorOwner)
        throw std::logic_error("Editor pipeline dispatch requires editor ownership");
    std::lock_guard lock(m_mutex);
    if (m_pending) throw std::logic_error("Editor simulation already has an outstanding tick");
    scene.SetDeferredGraphicsRetirement(true);
    scene.SetBeforeGpuResourceDestruction([this] { WaitForGraphics(); });
    scene.TransferOwnerTo(m_simulationOwner);
    m_scene = &scene;
    m_input = std::move(input);
    m_delta = delta;
    m_done = false;
    m_failure = nullptr;
    m_gpuFailure = nullptr;
    m_gpuRequested = m_gpuCompleted = false;
    m_pending = true;
    m_wake.notify_all();
}

void EditorPlayPipeline::WaitForGraphics()
{
    if (std::this_thread::get_id() == m_editorOwner)
    {
        m_renderer.WaitIdle();
        return;
    }
    std::unique_lock lock(m_mutex);
    m_gpuRequested = true;
    m_gpuCompleted = false;
    m_wake.notify_all();
    m_wake.wait(lock, [this] { return m_gpuCompleted; });
    if (m_gpuFailure) std::rethrow_exception(m_gpuFailure);
}

EditorPlayPipeline::Timings EditorPlayPipeline::Finish(std::vector<Commands> commands)
{
    if (std::this_thread::get_id() != m_editorOwner)
        throw std::logic_error("Editor pipeline completion requires editor ownership");
    const auto submissionFinished = Clock::now();
    Timings result;
    std::unique_lock lock(m_mutex);
    if (!m_pending) return result;
    while (!m_done)
    {
        m_wake.wait(lock, [this] { return m_done || m_gpuRequested; });
        if (m_gpuRequested)
        {
            m_gpuRequested = false;
            lock.unlock();
            const auto waitStart = Clock::now();
            std::exception_ptr failure;
            try { m_renderer.WaitIdle(); } catch (...) { failure = std::current_exception(); }
            result.gpuWaitMs += std::chrono::duration<double, std::milli>(Clock::now() - waitStart).count();
            lock.lock();
            m_gpuFailure = failure;
            m_gpuCompleted = true;
            m_wake.notify_all();
        }
    }
    result.updateMs = std::chrono::duration<double, std::milli>(m_finished - m_started).count();
    result.updateCpuMs = m_updateCpuMs;
    result.waitMs = std::chrono::duration<double, std::milli>(Clock::now() - submissionFinished).count();
    result.overlapMs = std::max(0., std::chrono::duration<double, std::milli>(
        std::min(m_finished, submissionFinished) - m_started).count());
    auto* scene = m_scene;
    auto failure = std::exchange(m_failure, nullptr);
    m_pending = false;
    lock.unlock();
    scene->AssertOwnerThread();
    scene->SetBeforeGpuResourceDestruction([renderer = &m_renderer] { renderer->WaitIdle(); });
    Core::ApplyNativeCursorRequests(m_input);
    m_frames.push_back({m_renderer.GetSubmittedFrameToken(), std::move(commands),
        scene->TakeRetiredGraphicsResources()});
    const auto collection = CollectCompletedFrames();
    result.queueWaitMs = collection.waitMs;
    result.queueWaits = collection.waits;
    result.retainedFrames = collection.retainedFrames;
    if (failure) std::rethrow_exception(failure);
    return result;
}

std::shared_ptr<Graphics::GraphicsCommandPacket> EditorPlayPipeline::AcquireCommands()
{
    if (std::this_thread::get_id() != m_editorOwner || Busy())
        throw std::logic_error("Render packet acquisition requires idle editor ownership");
    CollectCompletedFrames(false);
    if (m_recycledCommands.empty()) return std::make_shared<Graphics::GraphicsCommandPacket>();
    auto result = std::move(m_recycledCommands.back());
    m_recycledCommands.pop_back();
    return result;
}

EditorPlayPipeline::QueueCollection EditorPlayPipeline::CollectCompletedFrames(bool boundQueue)
{
    const auto retireCompleted = [this]
    {
        while (!m_frames.empty() && m_renderer.IsFrameComplete(m_frames.front().token))
        {
            // A current-frame reader may still hold a packet. It keeps that
            // packet alive; the completed frame need not block retirement.
            for (auto& packet : m_frames.front().commands)
                if (packet.use_count() == 1 && m_recycledCommands.size() < 64)
                {
                    auto reusable = std::const_pointer_cast<Graphics::GraphicsCommandPacket>(std::move(packet));
                    reusable->Reset();
                    m_recycledCommands.push_back(std::move(reusable));
                }
            m_frames.pop_front();
        }
    };
    retireCompleted();
    QueueCollection result;
    // Bound retained resources while waiting for only the oldest frame. A
    // full device drain would also wait for newer frames still in flight.
    if (boundQueue && m_frames.size() >= 3)
    {
        const auto start = Clock::now();
        m_renderer.WaitForFrame(m_frames.front().token);
        result.waitMs = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
        result.waits = 1;
        retireCompleted();
    }
    result.retainedFrames = static_cast<uint32_t>(m_frames.size());
    return result;
}

void EditorPlayPipeline::Drain()
{
    Finish();
    if (!m_frames.empty())
    {
        m_renderer.WaitIdle();
        m_frames.clear();
    }
}

void EditorPlayPipeline::Run()
{
    SetThreadDescription(GetCurrentThread(), L"Editor Play simulation");
    std::unique_lock lock(m_mutex);
    m_simulationOwner = std::this_thread::get_id();
    m_ready = true;
    m_wake.notify_all();
    while (true)
    {
        m_wake.wait(lock, [this] { return m_stop || (m_pending && !m_done); });
        if (m_stop) return;
        lock.unlock();
        m_started = Clock::now();
        const double cpuStart = ThreadCpuMs();
        std::exception_ptr failure;
        try
        {
            Core::SetCurrentNativeInputSnapshot(&m_input);
            m_scene->Update(m_delta);
        }
        catch (...) { failure = std::current_exception(); }
        Core::SetCurrentNativeInputSnapshot(nullptr);
        // Transfer even on failure; editor Stop/hot reload can then fence and
        // dispose the partially updated runtime on its normal owner.
        m_scene->TransferOwnerTo(m_editorOwner);
        m_finished = Clock::now();
        m_updateCpuMs = ThreadCpuMs() - cpuStart;
        lock.lock();
        m_failure = failure;
        m_done = true;
        m_wake.notify_all();
    }
}
}

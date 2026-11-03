#include "Engine/Editor/Runtime/EditorPlayPipeline.h"
#include "Core/Renderers/IEditorRenderer.h"
#include "Core/Renderers/IView.h"
#include "Core/Renderers/UIRenderer.h"
#include <future>
#include <iostream>

void Require(bool value, const char* message)
{ if (!value) throw std::runtime_error(message); }

struct Renderer final : Engine::Renderers::IEditorRenderer
{
    const std::thread::id owner = std::this_thread::get_id();
    uint64_t submitted = 1, completed = 0;
    unsigned idleWaits = 0;
    unsigned frameWaits = 0;
    uint64_t lastWaitedToken = 0;
    bool submittedFrame = true;
    bool Init(void*, uint32_t, uint32_t) override { return true; }
    void Resize(uint32_t, uint32_t) override {}
    uint32_t GetWidth() const override { return 64; }
    uint32_t GetHeight() const override { return 64; }
    void Clear(float, float, float, float) override {}
    Engine::Graphics::IGraphicsProvider* GetGraphicsProvider() override { return nullptr; }
    void WaitIdle() override
    {
        Require(std::this_thread::get_id() == owner, "GPU wait escaped graphics owner");
        Require(submittedFrame, "GPU wait ran before frame submission completed");
        ++idleWaits; completed = submitted;
    }
    bool SupportsFrameCompletion() const override { return true; }
    uint64_t GetSubmittedFrameToken() const override { return submitted; }
    bool IsFrameComplete(uint64_t token) override { return token <= completed; }
    void WaitForFrame(uint64_t token) override
    {
        Require(std::this_thread::get_id() == owner, "frame wait escaped graphics owner");
        Require(token <= submitted, "frame wait requested an unsubmitted token");
        ++frameWaits; lastWaitedToken = token; completed = token;
    }
    void MarkDirty() override {}
    bool IsDirty() const override { return true; }
    void SetUiRenderHooks(Engine::Renderers::EditorUiRenderHooks) override {}
    void RenderIfNeeded(std::function<void()> draw) override { if (draw) draw(); }
    std::pair<std::pair<void*, void*>, uint32_t> AllocateSrvSlot() override { return {}; }
    void FreeSrvSlot(uint32_t) override {}
    bool CanAllocateSrvSlot() const override { return false; }
    uint32_t GetAvailableSrvSlots() const override { return 0; }
    void* GetNativeDeviceHandle() const override { return nullptr; }
    std::unique_ptr<Engine::Renderers::IView> CreateViewBackend() override { return {}; }
    void* GetCurrentCommandBuffer() const override { return nullptr; }
    void* GetCurrentRenderTargetHandle() const override { return nullptr; }
};

struct Probe final : Engine::Core::Component
{
    std::function<void()> callback;
    void Update() override { if (callback) callback(); }
};

int main()
{
    try
    {
        Renderer renderer;
        Engine::Scene::Scene scene;
        auto* probe = scene.AddObject("Update probe")->AddComponent<Probe>();
        scene.CommitPendingObjectChanges();
        Engine::Editor::EditorPlayPipeline pipeline(renderer);
        std::promise<void> entered, release;
        auto arrived = entered.get_future();
        auto resume = release.get_future();
        probe->callback = [&] {
            Require(std::this_thread::get_id() != renderer.owner, "Update ran on editor thread");
            scene.AssertOwnerThread();
            entered.set_value();
            resume.wait();
        };
        pipeline.Begin(scene, .016f, {});
        Require(arrived.wait_for(std::chrono::seconds(5)) == std::future_status::ready,
            "simulation did not start while submission owner was free");
        bool ownerRejected = false, extraTickRejected = false;
        try { scene.AssertOwnerThread(); } catch (const std::logic_error&) { ownerRejected = true; }
        try { pipeline.Begin(scene, .016f, {}); } catch (const std::logic_error&) { extraTickRejected = true; }
        release.set_value();
        auto timings = pipeline.Finish();
        Require(ownerRejected && extraTickRejected, "live scene or queue boundary was not enforced");
        Require(scene.IsOwnerThread() && timings.overlapMs > 0, "ownership or overlap was lost");

        // Packet references outlive CPU replay and retire only after GPU completion.
        auto commands = std::make_shared<Engine::Graphics::GraphicsCommandPacket>();
        std::weak_ptr<const Engine::Graphics::GraphicsCommandPacket> weak = commands;
        probe->callback = {};
        pipeline.Begin(scene, .016f, {});
        pipeline.Finish({commands});
        commands.reset();
        Require(!weak.expired(), "packet was released before GPU completion");
        renderer.completed = renderer.submitted;
        auto reused = pipeline.AcquireCommands();
        Require(weak.lock() == reused, "completed packet storage was not reused");
        reused.reset();
        pipeline.Begin(scene, .016f, {});
        pipeline.Finish();

        // A destructive lifecycle callback waits until the graphics owner has
        // finished submission. The owner pumps the request while joining.
        renderer.submittedFrame = false;
        probe->callback = [&] { scene.WaitForGpuResourceDestruction(); };
        pipeline.Begin(scene, .016f, {});
        renderer.submittedFrame = true;
        const unsigned waits = renderer.idleWaits;
        pipeline.Finish();
        Require(renderer.idleWaits > waits && scene.IsOwnerThread(), "lifecycle wait did not complete");

        probe->callback = [] { throw std::runtime_error("expected update failure"); };
        pipeline.Begin(scene, .016f, {});
        bool failed = false;
        try { pipeline.Finish(); } catch (const std::runtime_error&) { failed = true; }
        Require(failed && scene.IsOwnerThread() && !pipeline.Busy(), "failed update stranded ownership");
        probe->callback = {};
        pipeline.Begin(scene, .016f, {});
        pipeline.Finish();
        pipeline.Drain();

        const unsigned beforeBoundIdle = renderer.idleWaits;
        const unsigned beforeBoundFrame = renderer.frameWaits;
        uint64_t firstToken = renderer.submitted + 1;
        Engine::Editor::EditorPlayPipeline::Timings bounded;
        for (int frame = 0; frame < 3; ++frame)
        {
            ++renderer.submitted;
            pipeline.Begin(scene, .016f, {});
            bounded = pipeline.Finish();
        }
        Require(renderer.idleWaits == beforeBoundIdle && renderer.frameWaits == beforeBoundFrame + 1,
            "queue bound drained the device instead of waiting for one frame");
        Require(renderer.lastWaitedToken == firstToken && renderer.completed < renderer.submitted,
            "queue bound waited for the newest frame");
        Require(bounded.queueWaits == 1 && bounded.retainedFrames == 2,
            "queue wait was not measured or older resources were not retired");

        const auto published = scene.GetPublishedSceneSnapshot();
        probe->Owner->name = "Owner edit before replay";
        scene.PrepareRenderFrame(false);
        Require(scene.GetPublishedSceneSnapshot() == published,
            "replay preparation redundantly published a simulation snapshot");
        pipeline.Begin(scene, .016f, {});
        pipeline.Finish();
        Require(scene.GetPublishedSceneSnapshot() != published,
            "simulation commit did not publish the preceding owner edit");

        Engine::Renderers::UIRenderer ui;
        ui.SetPointerInput(37, 19, 640, 360, true, true);
        Engine::Core::NativeInputSnapshot input;
        input.focused = true;
        Require(ui.CaptureEmbeddedPointer(input) && input.clientWidth == 640 &&
            input.clientHeight == 360 && input.clientCursor.x == 37 &&
            input.cursorInWindow && (input.keys[VK_LBUTTON] & 0x8000),
            "embedded game input was replaced by window coordinates");
        input.focused = false;
        ui.CaptureEmbeddedPointer(input);
        Require(!input.cursorInWindow && !input.keys[VK_LBUTTON],
            "unfocused editor retained an embedded click");
        std::cout << "Editor pipeline ownership, overlap, retirement, lifecycle, failure and input checks passed.\n";
    }
    catch (const std::exception& error)
    { std::cerr << error.what() << '\n'; return 1; }
}

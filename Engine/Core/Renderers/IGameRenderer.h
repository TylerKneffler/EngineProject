#pragma once
#include "IRenderer.h"
#include "Core/Graphics/IGraphicsContext.h"
#include <memory>


namespace Engine::Graphics { class GraphicsCommandPacket; }
namespace Engine::Renderers
{
// ---------------------------------------------------------------------------
// IGameRenderer — Abstract interface for game renderers.
//
// A game renderer typically renders every frame unconditionally (v-synced or
// uncapped depending on settings). It provides per-frame setup/teardown methods
// that the game loop calls in sequence:
//   - BeginFrame()  prepare GPU work (reset allocators, transition targets, etc.)
//   - [draw calls]  caller issues draw commands
//   - EndFrame()    submit work, present, synchronize
// ---------------------------------------------------------------------------
class IGameRenderer : public IRenderer
{
public:
    virtual ~IGameRenderer() = default;

    // Prepare the renderer for a new frame. Called once per game tick before
    // any draw commands. Must be paired with a corresponding EndFrame().
    virtual void BeginFrame() = 0;

    // Finish recording the current frame: execute command lists, present
    // buffers to screen, and synchronize GPU/CPU state. Must follow a
    // corresponding BeginFrame().
    virtual void EndFrame() = 0;

    // Create a graphics context bound to the renderer's current frame command
    // stream. Call after BeginFrame() and before EndFrame().
    virtual std::unique_ptr<Engine::Graphics::IGraphicsContext> CreateFrameGraphicsContext() = 0;
    // Called on a recording worker after BeginFrame. The render owner joins
    // that worker and calls SubmitWorkerCommands before Clear/scene replay.
    virtual bool RecordWorkerCommands(const Engine::Graphics::GraphicsCommandPacket&) { return false; }
    virtual bool SupportsWorkerCommands() const { return false; }
    virtual void SetWorkerRecordingEnabled(bool) {}
    virtual void SubmitWorkerCommands() {}

    // Monotonic GPU completion watermark for resources retired after EndFrame.
    // Poll only on the renderer's owning thread. Token zero means no submission.
    virtual uint64_t GetSubmittedFrameToken() const = 0;
    virtual bool IsFrameComplete(uint64_t token) = 0;

    // Optional frame-accurate readback for standalone diagnostics.
    virtual void RequestSceneCapture() {}
    virtual bool ReadSceneCaptureRGBA(std::vector<uint8_t>&) { return false; }
};
}

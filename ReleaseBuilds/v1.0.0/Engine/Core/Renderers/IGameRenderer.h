#pragma once
#include "IRenderer.h"
#include <memory>

class IGraphicsContext;

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
    virtual std::unique_ptr<IGraphicsContext> CreateFrameGraphicsContext() = 0;
};

#pragma once
#include "Core/Graphics/IGraphicsProvider.h"
#include "Core/Model/RendererData.h"
#include <algorithm>

namespace Engine::Renderers
{
// ---------------------------------------------------------------------------
// IRenderer — Abstract base for all renderer implementations.
//
// This interface decouples the application from specific graphics APIs
// (DirectX 12, Vulkan, etc.). All renderers must implement these core methods.
//
// A concrete renderer (e.g., DX12EditorRenderer) inherits from this and
// implements graphics-API-specific initialization and rendering logic.
// ---------------------------------------------------------------------------
class IRenderer
{
public:
    virtual ~IRenderer() = default;

    // Initialize the renderer with the given window handle and dimensions.
    virtual bool Init(void* hwnd, uint32_t width, uint32_t height) = 0;

    // Resize the render targets to the given dimensions.
    virtual void Resize(uint32_t width, uint32_t height) = 0;

    // Get the current render target width and height.
    virtual uint32_t GetWidth() const = 0;
    virtual uint32_t GetHeight() const = 0;

    // Clear the frame. The exact semantics (immediate vs deferred) depend on
    // the renderer implementation (see DX12EditorRenderer vs DX12GameRenderer).
    virtual void Clear(float r, float g, float b, float a = 1.0f) = 0;

    // Dynamic internal render scale lets heavy scenes trade a little image
    // sharpness for lower GPU cost while maintaining the same logical viewport.
    virtual void SetRenderScale(float scale)
    {
        m_renderScale = std::clamp(scale, 0.5f, 1.0f);
        m_renderScaleMode = m_renderScale > 0.90f ? Engine::Model::RenderScaleMode::Native :
            m_renderScale >= 0.70f ? Engine::Model::RenderScaleMode::Quality75 :
            m_renderScale >= 0.58f ? Engine::Model::RenderScaleMode::Quality67 :
            Engine::Model::RenderScaleMode::Quality50;
    }
    virtual float GetRenderScale() const { return m_renderScale; }
    virtual void SetRenderScaleMode(Engine::Model::RenderScaleMode mode)
    {
        m_renderScaleMode = mode;
        m_renderScale = Engine::Model::RenderQualitySettings::ScaleForMode(mode);
    }
    virtual Engine::Model::RenderScaleMode GetRenderScaleMode() const
    { return m_renderScaleMode; }
    virtual void SetTemporalUpscaling(bool enabled) { m_temporalUpscaling = enabled; }
    virtual bool IsTemporalUpscalingEnabled() const { return m_temporalUpscaling; }
    virtual void SetDynamicRenderScale(bool enabled) { m_dynamicRenderScale = enabled; }
    virtual bool IsDynamicRenderScaleEnabled() const { return m_dynamicRenderScale; }

    // Get access to graphics services (shader compilation, buffer creation, pipeline building)
    virtual Engine::Graphics::IGraphicsProvider* GetGraphicsProvider() = 0;

    // Drain submitted GPU work before caller-owned scene resources are
    // destroyed. Normal frame rendering must not call this blocking method.
    virtual void WaitIdle() {}
    virtual Engine::Graphics::FrameTimingTelemetry GetFrameTimingTelemetry() const
    { return {}; }

protected:
    float m_renderScale = 1.0f;
    bool m_temporalUpscaling = false;
    bool m_dynamicRenderScale = false;
    Engine::Model::RenderScaleMode m_renderScaleMode = Engine::Model::RenderScaleMode::Native;
};
}

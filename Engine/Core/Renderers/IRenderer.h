#pragma once

// Forward declaration
class IGraphicsProvider;

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

    // Get access to graphics services (shader compilation, buffer creation, pipeline building)
    virtual IGraphicsProvider* GetGraphicsProvider() = 0;
};

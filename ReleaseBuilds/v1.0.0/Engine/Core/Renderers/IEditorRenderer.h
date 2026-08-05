#pragma once
#include "IRenderer.h"
#include <functional>
#include <memory>
#include <utility>
#include <cstdint>

// Forward declare the view interface so renderers can work with it.
class IView;

// Package-neutral callbacks installed by the editor UI layer. Keeping this
// value type in Core prevents Engine/Core from depending on Engine/Editor/UI.
struct EditorUiRenderHooks
{
    std::function<void()> beginFrame;
    std::function<void(void* commandBuffer)> render;
    std::function<void()> endFrame;
};

struct EditorUiTextureHooks
{
    std::function<void*(void* sampler, void* imageView, uint32_t imageLayout)> registerTexture;
    std::function<void(void* texture)> unregisterTexture;
};

// ---------------------------------------------------------------------------
// IEditorRenderer — Abstract interface for editor renderers.
//
// An editor renderer typically implements on-demand / dirty-driven rendering:
// - MarkDirty() flags that a new frame is needed
// - RenderIfNeeded() only produces a frame if the dirty flag is set
// - This minimizes GPU/CPU usage when the editor is idle
//
// Editor renderers also manage SRV slots for 3-D views (SceneView, GameView).
// ---------------------------------------------------------------------------
class IEditorRenderer : public IRenderer
{
public:
    virtual ~IEditorRenderer() = default;

    // Mark the back-buffer as needing a redraw. Call after any change
    // (resize, input, property edits, scene changes, etc.).
    virtual void MarkDirty() = 0;

    virtual void SetUiRenderHooks(EditorUiRenderHooks hooks) = 0;
    virtual void SetUiTextureHooks(EditorUiTextureHooks) {}

    // If the dirty flag is set, record and execute one frame, then clear the
    // flag. drawFn is called between BeginFrame and EndFrame so the caller can
    // issue draw commands. If drawFn is nullptr, just clear to background colour.
    virtual void RenderIfNeeded(std::function<void()> drawFn = nullptr) = 0;

    // Allocate an SRV slot for a view. Returns a handle pair and slot index.
    // Slot 0 is reserved for the selected UI backend; slots 1+ are views.
    virtual std::pair<std::pair<void*, void*>, uint32_t> AllocateSrvSlot() = 0;

    // Free a previously-allocated SRV slot (identified by its index).
    // The slot can then be reused for another view.
    virtual void FreeSrvSlot(uint32_t slotIndex) = 0;

    // Check if an SRV slot is available (i.e., AllocateSrvSlot won't fail).
    virtual bool CanAllocateSrvSlot() const = 0;

    // Get the number of available SRV slots.
    virtual uint32_t GetAvailableSrvSlots() const = 0;

    // Opaque native device handle used by view initialization.
    // D3D12: ID3D12Device*; D3D11: ID3D11Device*; Vulkan: backend device context.
    virtual void* GetNativeDeviceHandle() const = 0;

    // Create a view backend for offscreen editor rendering.
    virtual std::unique_ptr<IView> CreateViewBackend() = 0;

    // Get the current active command buffer / command list as a void*.
    // D3D12: returns ID3D12GraphicsCommandList*
    // D3D11: returns ID3D11DeviceContext*; Vulkan: returns VkCommandBuffer
    // Only valid while inside a RenderIfNeeded drawFn callback.
    virtual void* GetCurrentCommandBuffer() const = 0;

    // Get a pointer to the current render target handle (API-specific).
    // D3D12: returns D3D12_CPU_DESCRIPTOR_HANDLE*
    // D3D11: returns ID3D11RenderTargetView*; Vulkan: returns nullptr
    // Only valid while inside a RenderIfNeeded drawFn callback.
    virtual void* GetCurrentRenderTargetHandle() const = 0;

    // Get the maximum number of SRV slots.
    static constexpr uint32_t MAX_SRV_SLOTS = 32;
};

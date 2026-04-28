#pragma once
#include "IRenderer.h"
#include <functional>
#include <memory>
#include <utility>

// Forward declare the view interface so renderers can work with it.
class IView;

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

    // If the dirty flag is set, record and execute one frame, then clear the
    // flag. drawFn is called between BeginFrame and EndFrame so the caller can
    // issue draw commands. If drawFn is nullptr, just clear to background colour.
    virtual void RenderIfNeeded(std::function<void()> drawFn = nullptr) = 0;

    // Allocate an SRV slot for a view. Returns a handle pair and slot index.
    // Slot 0 is reserved for ImGui; slots 1+ are for scene/game views.
    virtual std::pair<std::pair<void*, void*>, uint32_t> AllocateSrvSlot() = 0;

    // Free a previously-allocated SRV slot (identified by its index).
    // The slot can then be reused for another view.
    virtual void FreeSrvSlot(uint32_t slotIndex) = 0;

    // Check if an SRV slot is available (i.e., AllocateSrvSlot won't fail).
    virtual bool CanAllocateSrvSlot() const = 0;

    // Get the number of available SRV slots.
    virtual uint32_t GetAvailableSrvSlots() const = 0;

    // Opaque native device handle used by view initialization.
    // D3D12: ID3D12Device*; Vulkan: VkDevice.
    virtual void* GetNativeDeviceHandle() const = 0;

    // Create a view backend for offscreen editor rendering.
    virtual std::unique_ptr<IView> CreateViewBackend() = 0;

    // Get the maximum number of SRV slots.
    static constexpr uint32_t MAX_SRV_SLOTS = 32;
};

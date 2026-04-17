#pragma once
#include "IEditorPanel.h"

// ---------------------------------------------------------------------------
// IView — Abstract interface for renderer-agnostic views.
//
// Views are 2-D panels that can contain offscreen-rendered 3-D content
// (e.g., SceneView, GameView). This interface decouples view logic from
// the underlying renderer API (DirectX 12, Vulkan, etc.).
//
// Each view owns:
// - An offscreen texture for 3-D rendering
// - Descriptor handles (what these are depends on the renderer)
// - A slot index in the renderer's SRV heap (for reuse tracking)
//
// Concrete implementations (e.g., DX12View) will inherit from this and
// IEditorPanel, providing ImGui rendering (DrawPanel) and 3-D content
// (via OnRender callbacks wired by subclasses).
// ---------------------------------------------------------------------------
class IView : public virtual IEditorPanel
{
public:
    virtual ~IView() = default;

    // Initialize the view with offscreen texture resources.
    // Concrete implementations will interpret descriptorHandles according to
    // their renderer backend (D3D12 GPU/CPU handles, Vulkan image handles, etc.).
    //
    // descriptorHandles: A pair of opaque handles returned by the renderer's
    //                    AllocateSrvSlot(). For D3D12, these are CPU/GPU
    //                    descriptor handles cast to void*.
    // srvSlotIndex:      The slot index in the renderer's SRV heap, used for
    //                    cleanup when the view is destroyed.
    virtual bool Init(uint32_t width, uint32_t height,
                      std::pair<void*, void*> descriptorHandles,
                      uint32_t srvSlotIndex) = 0;

    // Resize the view's offscreen texture to the given dimensions.
    // Safe to call only when the GPU is idle.
    virtual void Resize(uint32_t width, uint32_t height) = 0;

    // Render 3-D content to the view's offscreen texture.
    // Concrete implementations will use renderer-specific command lists,
    // transitions, etc. to set up the rendering environment, then invoke
    // drawFn to let the caller issue draw commands.
    virtual void Render(std::function<void()> drawFn = nullptr) = 0;

    // Access view metadata.
    virtual float    GetAspect() const = 0;
    virtual uint32_t GetWidth() const = 0;
    virtual uint32_t GetHeight() const = 0;
    virtual uint32_t GetSrvSlotIndex() const = 0;

    // ImGui panel rendering (implemented by subclasses).
    virtual void DrawPanel() = 0;
};

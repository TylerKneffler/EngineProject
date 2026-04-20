#pragma once
#include <functional>
#include <cstdint>

// Forward declaration - editor UI interface (no dependency from Core layer)
class IEditorPanel;

// ---------------------------------------------------------------------------
// IView — Graphics-agnostic interface for offscreen rendered editor panels
//
// Abstracts offscreen rendering for editor views (SceneView, GameView, etc).
// The view owns:
//   - An offscreen render target texture
//   - A depth buffer
//   - SRV registration for ImGui display
//
// Graphics API specifics (D3D12, Vulkan, Metal) are hidden from public API.
// All handles are opaque void* pointers.
//
// Note: View class will implement both IView (graphics API) and IEditorPanel
// (editor UI) to provide complete panel functionality.
// ---------------------------------------------------------------------------
class IView
{
public:
    virtual ~IView() = default;

    // Initialize offscreen rendering resources
    // device: opaque graphics device handle
    // srvCpu/srvGpu: opaque descriptor handles for shader resource view
    // srvSlotIndex: identifies the heap slot for cleanup
    virtual void Init(void* device,
                      uint32_t width, uint32_t height,
                      void* srvCpu, void* srvGpu,
                      uint32_t srvSlotIndex = 0) = 0;

    // Resize rendering targets
    // device: opaque graphics device handle
    virtual void Resize(void* device, uint32_t width, uint32_t height) = 0;

    // Render to offscreen target, then transition back to SRV
    // cmdList: opaque graphics command list handle
    // mainRtv: opaque main render target handle (restored after rendering)
    // drawFn: callback function invoked between clear and transition operations
    virtual void Render(void* cmdList, void* mainRtv,
                        std::function<void(void*)> drawFn = nullptr) = 0;

    // Query dimensions and aspect ratio
    virtual float    GetAspect() const = 0;
    virtual uint32_t GetWidth()  const = 0;
    virtual uint32_t GetHeight() const = 0;

    // SRV slot index for resource cleanup
    virtual uint32_t GetSrvSlotIndex() const = 0;
};

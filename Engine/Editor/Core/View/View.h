#pragma once
#include "../../../Core/Renderers/IView.h"
#include "IEditorPanel.h"
#include <memory>
#include <functional>

// Forward declaration — graphics API implementation (D3D12, Vulkan, etc.)
class D3D12View;

// ---------------------------------------------------------------------------
// View — Graphics-agnostic base for DX12-backed editor panels
//
// Derives from IView for API-neutral interface and IEditorPanel for
// panel management. Composes a D3D12View instance for offscreen rendering.
// Subclasses (SceneView, GameView) override DrawPanel() and Render3D().
//
// The graphics API implementation is completely hidden; D3D12 is encapsulated
// in the composed D3D12View member.
// ---------------------------------------------------------------------------
class View : public IEditorPanel
{
public:
    View();
    virtual ~View() = default;

    // NeedsRender always returns true for graphics-backed views.
    bool NeedsRender() const override { return true; }

    // Initialize offscreen rendering resources
    // device: opaque graphics device handle
    // srvCpu/srvGpu: opaque descriptor handles
    // srvSlotIndex: heap slot identifier
    void Init(void* device,
              uint32_t width, uint32_t height,
              void* srvCpu, void* srvGpu,
              uint32_t srvSlotIndex = 0);

    // Resize rendering targets
    // device: opaque graphics device handle
    void Resize(void* device, uint32_t width, uint32_t height);

    // Render to offscreen target with custom drawing function
    // cmdList: opaque graphics command list handle
    // mainRtv: opaque main render target handle
    // drawFn: callback for scene rendering
    void Render(void* cmdList, void* mainRtv,
                std::function<void(void*)> drawFn = nullptr);

    // Query rendering target properties
    float    GetAspect() const;
    uint32_t GetWidth()  const;
    uint32_t GetHeight() const;

    // SRV slot index for resource cleanup
    uint32_t GetSrvSlotIndex() const;

    // DrawPanel — implemented by subclasses for ImGui content
    virtual void DrawPanel() = 0;

    // Render3D — implemented by subclasses to record scene rendering commands
    // cmd: opaque graphics command list handle
    virtual void Render3D(void* cmd) = 0;

protected:
    // Access to underlying D3D12View for derived classes
    D3D12View* GetD3D12View();
    const D3D12View* GetD3D12View() const;

private:
    std::unique_ptr<D3D12View> m_d3d12View;  // Graphics API implementation
};

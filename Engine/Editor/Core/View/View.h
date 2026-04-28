#pragma once
#include "../../../Core/Renderers/IView.h"
#include "IEditorPanel.h"
#include <memory>
#include <functional>

// ---------------------------------------------------------------------------
// View — Graphics-agnostic base for editor panels
//
// Derives from IView for API-neutral interface and IEditorPanel for
// panel management. Composes an IView backend for offscreen rendering.
// Subclasses (SceneView, GameView) override DrawPanel() and Render3D().
// The backend implementation is provided by the active renderer.
// ---------------------------------------------------------------------------
class View : public IEditorPanel
{
public:
    View();
    virtual ~View();

    // Inject graphics backend implementation created by the renderer.
    void SetViewBackend(std::unique_ptr<IView> viewBackend);

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
    void*    GetImGuiTextureHandle() const;

    // SRV slot index for resource cleanup
    uint32_t GetSrvSlotIndex() const;

    // DrawPanel — implemented by subclasses for ImGui content
    virtual void DrawPanel() = 0;

    // Render3D — implemented by subclasses to record scene rendering commands
    // cmd: opaque graphics command list handle
    virtual void Render3D(void* cmd) = 0;

protected:
private:
    std::unique_ptr<IView> m_viewBackend;  // Graphics API implementation
};

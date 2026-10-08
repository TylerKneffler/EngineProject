#pragma once
#include "../../../Core/Renderers/IView.h"
#include "IEditorPanel.h"
#include <memory>
#include <functional>

namespace Engine::Editor
{
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
    void SetViewBackend(std::unique_ptr<::Engine::Renderers::IView> viewBackend);

    // A graphics-backed panel owns an offscreen target even while its docked
    // tab is hidden. Rendering can skip the hidden target separately.
    bool NeedsRender() const override { return true; }
    bool IsRenderVisible() const { return m_panelVisible; }
    bool ConsumeVisibilityChanged()
    {
        const bool changed = m_visibilityChanged;
        m_visibilityChanged = false;
        return changed;
    }

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
                std::function<void(void*)> drawFn = nullptr,
                std::function<void(void*)> preDrawFn = nullptr);

    void SetClearColor(float r, float g, float b, float a = 1.0f);

    // Query rendering target properties
    float    GetAspect() const;
    uint32_t GetWidth()  const;
    uint32_t GetHeight() const;
    void*    GetUiTextureHandle() const;

    // SRV slot index for resource cleanup
    uint32_t GetSrvSlotIndex() const;

    // DrawPanel is implemented against the package-neutral UI facade.
    virtual void DrawPanel(IEditorUi& ui) = 0;

    // Render3D — implemented by subclasses to record scene rendering commands
    // cmd: opaque graphics command list handle
    virtual void Render3D(void* cmd) = 0;
    virtual void RenderShadow3D(void* cmd) = 0;

protected:
    void SetPanelVisible(bool visible)
    {
        if (m_panelVisible != visible) m_visibilityChanged = true;
        m_panelVisible = visible;
    }
private:
    bool m_panelVisible = true;
    bool m_visibilityChanged = false;
    std::unique_ptr<::Engine::Renderers::IView> m_viewBackend;  // Graphics API implementation
};
}

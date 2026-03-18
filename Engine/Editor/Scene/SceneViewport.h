#pragma once
#include "pch.h"

// ---------------------------------------------------------------------------
// SceneViewport
//
// Renders the 3-D scene into an off-screen texture and presents it inside an
// ImGui panel labelled "Scene".
//
// ---- Per-frame call order (inside drawFn) ----
//
//   // 1. Record GPU work that writes 3-D content into the offscreen texture.
//   sceneViewport.Render(renderer.GetCommandList(), renderer.GetCurrentRTV());
//
//   // 2. Draw the ImGui panel — the texture is now in SRV state for ImGui.
//   sceneViewport.DrawPanel();
//
// ---- Resize ----
//
//   Call Resize() from window->OnResize *after* renderer->Resize() so the GPU
//   is already idle (renderer->Resize() calls FlushGPU internally).
//
// ---- SRV slot ----
//
//   The scene texture SRV must live in ImGui's shader-visible descriptor heap.
//   Use DX12EditorRenderer::GetSrvSlot(1) to obtain a free slot — slot 0 is
//   reserved for the ImGui font atlas.
// ---------------------------------------------------------------------------

class SceneViewport
{
public:
    SceneViewport()  = default;
    ~SceneViewport() = default;

    // Creates the offscreen texture and registers it in ImGui's SRV heap.
    // srvCpu/srvGpu must be a free slot in ImGui's shader-visible SRV heap.
    void Init(ID3D12Device* device,
              uint32_t width, uint32_t height,
              D3D12_CPU_DESCRIPTOR_HANDLE srvCpu,
              D3D12_GPU_DESCRIPTOR_HANDLE srvGpu);

    // Recreates the offscreen texture at the new dimensions.
    // Safe to call only when the GPU is idle (i.e. after renderer->Resize()).
    void Resize(ID3D12Device* device, uint32_t width, uint32_t height);

    // Records GPU commands to render 3-D content into the offscreen texture.
    // Transitions the texture SRV→RTV, clears it, calls drawFn (if provided) so
    // callers can issue draw calls, then transitions RTV→SRV and restores mainRtv.
    // drawFn receives the open command list; it must not close or execute it.
    void Render(ID3D12GraphicsCommandList* cmdList,
                D3D12_CPU_DESCRIPTOR_HANDLE mainRtv,
                std::function<void(ID3D12GraphicsCommandList*)> drawFn = nullptr);

    // Draws the "Scene" ImGui panel displaying the offscreen texture.
    void DrawPanel();

    // Returns the aspect ratio (width / height) of the scene panel as measured
    // in the most recent DrawPanel() call. Defaults to 1.0 before the first call.
    // Use this for the projection matrix to avoid squishing the rendered geometry.
    float GetAspect() const { return m_aspect; }

    // Per-frame mouse drag delta over the scene image (pixels), captured during
    // the most recent DrawPanel() call. Both values are 0 when not dragging.
    float GetDragDeltaX() const { return m_dragDeltaX; }
    float GetDragDeltaY() const { return m_dragDeltaY; }

private:
    void CreateResources(ID3D12Device* device, uint32_t width, uint32_t height);

    ComPtr<ID3D12Resource>       m_texture;     // offscreen colour render target / SRV
    ComPtr<ID3D12DescriptorHeap> m_rtvHeap;     // 1-slot non-shader-visible RTV heap
    ComPtr<ID3D12Resource>       m_depthBuffer; // depth buffer for 3-D rendering
    ComPtr<ID3D12DescriptorHeap> m_dsvHeap;     // 1-slot non-shader-visible DSV heap

    D3D12_CPU_DESCRIPTOR_HANDLE  m_srvCpu{};  // slot in ImGui's SRV heap (CPU side)
    D3D12_GPU_DESCRIPTOR_HANDLE  m_srvGpu{};  // slot in ImGui's SRV heap (GPU side)

    uint32_t m_width   = 0;
    uint32_t m_height  = 0;
    float    m_aspect  = 1.0f;  // scene panel aspect ratio, updated each DrawPanel()
    float    m_dragDeltaX = 0.0f; // per-frame mouse drag over the image (pixels)
    float    m_dragDeltaY = 0.0f;
};

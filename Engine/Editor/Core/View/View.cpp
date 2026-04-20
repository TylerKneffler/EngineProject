#include "View.h"
#include "../../../Core/Renderers/DX12/D3D12View.h"

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------
View::View()
    : m_d3d12View(std::make_unique<D3D12View>())
{
}

// ---------------------------------------------------------------------------
// Init — delegate to D3D12View
// ---------------------------------------------------------------------------
void View::Init(void* device,
                uint32_t width, uint32_t height,
                void* srvCpu, void* srvGpu,
                uint32_t srvSlotIndex)
{
    if (m_d3d12View)
        m_d3d12View->Init(device, width, height, srvCpu, srvGpu, srvSlotIndex);
}

// ---------------------------------------------------------------------------
// Resize — delegate to D3D12View
// ---------------------------------------------------------------------------
void View::Resize(void* device, uint32_t width, uint32_t height)
{
    if (m_d3d12View)
        m_d3d12View->Resize(device, width, height);
}

// ---------------------------------------------------------------------------
// Render — delegate to D3D12View
// ---------------------------------------------------------------------------
void View::Render(void* cmdList, void* mainRtv,
                  std::function<void(void*)> drawFn)
{
    if (m_d3d12View)
        m_d3d12View->Render(cmdList, mainRtv, drawFn);
}

// ---------------------------------------------------------------------------
// Property accessors — delegate to D3D12View
// ---------------------------------------------------------------------------
float View::GetAspect() const
{
    return m_d3d12View ? m_d3d12View->GetAspect() : 1.0f;
}

uint32_t View::GetWidth() const
{
    return m_d3d12View ? m_d3d12View->GetWidth() : 0;
}

uint32_t View::GetHeight() const
{
    return m_d3d12View ? m_d3d12View->GetHeight() : 0;
}

uint32_t View::GetSrvSlotIndex() const
{
    return m_d3d12View ? m_d3d12View->GetSrvSlotIndex() : 0;
}

// ---------------------------------------------------------------------------
// GetD3D12View — derived classes access the D3D12 implementation
// ---------------------------------------------------------------------------
D3D12View* View::GetD3D12View()
{
    return m_d3d12View.get();
}

const D3D12View* View::GetD3D12View() const
{
    return m_d3d12View.get();
}


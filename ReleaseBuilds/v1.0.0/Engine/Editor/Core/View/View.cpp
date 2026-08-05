#include "View.h"

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------
View::View() = default;

View::~View() = default;

void View::SetViewBackend(std::unique_ptr<IView> viewBackend)
{
    m_viewBackend = std::move(viewBackend);
}

// ---------------------------------------------------------------------------
// Init — delegate to D3D12View
// ---------------------------------------------------------------------------
void View::Init(void* device,
                uint32_t width, uint32_t height,
                void* srvCpu, void* srvGpu,
                uint32_t srvSlotIndex)
{
    if (m_viewBackend)
        m_viewBackend->Init(device, width, height, srvCpu, srvGpu, srvSlotIndex);
}

// ---------------------------------------------------------------------------
// Resize — delegate to D3D12View
// ---------------------------------------------------------------------------
void View::Resize(void* device, uint32_t width, uint32_t height)
{
    if (m_viewBackend)
        m_viewBackend->Resize(device, width, height);
}

// ---------------------------------------------------------------------------
// Render — delegate to D3D12View
// ---------------------------------------------------------------------------
void View::Render(void* cmdList, void* mainRtv,
                  std::function<void(void*)> drawFn)
{
    if (m_viewBackend)
        m_viewBackend->Render(cmdList, mainRtv, drawFn);
}

// ---------------------------------------------------------------------------
// Property accessors — delegate to D3D12View
// ---------------------------------------------------------------------------
float View::GetAspect() const
{
    return m_viewBackend ? m_viewBackend->GetAspect() : 1.0f;
}

uint32_t View::GetWidth() const
{
    return m_viewBackend ? m_viewBackend->GetWidth() : 0;
}

uint32_t View::GetHeight() const
{
    return m_viewBackend ? m_viewBackend->GetHeight() : 0;
}

void* View::GetUiTextureHandle() const
{
    return m_viewBackend ? m_viewBackend->GetUiTextureHandle() : nullptr;
}

uint32_t View::GetSrvSlotIndex() const
{
    return m_viewBackend ? m_viewBackend->GetSrvSlotIndex() : 0;
}


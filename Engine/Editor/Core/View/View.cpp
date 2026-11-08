#include "View.h"
#include <algorithm>
#include <cmath>

namespace Engine::Editor
{
// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------
View::View() = default;

View::~View() = default;

void View::SetViewBackend(std::unique_ptr<::Engine::Renderers::IView> viewBackend)
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
    if (m_fixedWidth && m_fixedHeight)
    {
        width = m_fixedWidth;
        height = m_fixedHeight;
    }
    const float scale = EffectiveRenderScale();
    width = std::max(1u, static_cast<uint32_t>(std::lround(width * scale)));
    height = std::max(1u, static_cast<uint32_t>(std::lround(height * scale)));
    if (m_viewBackend && (width != GetWidth() || height != GetHeight()))
        m_viewBackend->Resize(device, width, height);
}

void View::SetFixedRenderSize(void* device, uint32_t width, uint32_t height)
{
    m_fixedWidth = width && height ? width : 0;
    m_fixedHeight = width && height ? height : 0;
    m_requestedWidth = m_requestedHeight = 0;
    m_renderSizeChanged = true;
    if (m_fixedWidth) Resize(device, m_fixedWidth, m_fixedHeight);
}

void View::RequestRenderSize(float width, float height)
{
    if (m_fixedWidth && m_fixedHeight) return;
    if (width <= 1.f || height <= 1.f) return;
    const uint32_t requestedWidth = static_cast<uint32_t>(
        std::max(1l, std::lround(width)));
    const uint32_t requestedHeight = static_cast<uint32_t>(
        std::max(1l, std::lround(height)));
    if (requestedWidth != m_requestedWidth ||
        requestedHeight != m_requestedHeight)
        m_renderSizeChanged = true;
    m_requestedWidth = requestedWidth;
    m_requestedHeight = requestedHeight;
}

void View::ApplyRequestedRenderSize(void* device)
{
    if (m_fixedWidth && m_fixedHeight) Resize(device, m_fixedWidth, m_fixedHeight);
    else if (m_requestedWidth && m_requestedHeight)
        Resize(device, m_requestedWidth, m_requestedHeight);
}

// ---------------------------------------------------------------------------
// Render — delegate to D3D12View
// ---------------------------------------------------------------------------
void View::Render(void* cmdList, void* mainRtv,
                  std::function<void(void*)> drawFn,
                  std::function<void(void*)> preDrawFn)
{
    if (m_viewBackend)
        m_viewBackend->Render(cmdList, mainRtv, drawFn, preDrawFn);
}

void View::SetClearColor(float r, float g, float b, float a)
{
    if (m_viewBackend)
        m_viewBackend->SetClearColor(r, g, b, a);
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
}

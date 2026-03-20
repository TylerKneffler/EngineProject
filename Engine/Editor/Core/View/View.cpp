#include "View.h"

static constexpr DXGI_FORMAT VIEW_FORMAT = DXGI_FORMAT_R8G8B8A8_UNORM;

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------
void View::Init(ID3D12Device* device,
                uint32_t width, uint32_t height,
                D3D12_CPU_DESCRIPTOR_HANDLE srvCpu,
                D3D12_GPU_DESCRIPTOR_HANDLE srvGpu)
{
    m_srvCpu = srvCpu;
    m_srvGpu = srvGpu;
    CreateResources(device, width, height);
}

// ---------------------------------------------------------------------------
// Resize
// ---------------------------------------------------------------------------
void View::Resize(ID3D12Device* device, uint32_t width, uint32_t height)
{
    if (width == 0 || height == 0)              return;
    if (width == m_width && height == m_height) return;

    m_texture.Reset();
    m_depthBuffer.Reset();
    CreateResources(device, width, height);
}

// ---------------------------------------------------------------------------
// CreateResources
// ---------------------------------------------------------------------------
void View::CreateResources(ID3D12Device* device, uint32_t width, uint32_t height)
{
    m_width  = width;
    m_height = height;

    D3D12_RESOURCE_DESC texDesc{};
    texDesc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width            = width;
    texDesc.Height           = height;
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels        = 1;
    texDesc.Format           = VIEW_FORMAT;
    texDesc.SampleDesc       = { 1, 0 };
    texDesc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    texDesc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    const float clearColor[4] = { 0.1f, 0.1f, 0.1f, 1.0f };
    D3D12_CLEAR_VALUE clearVal{};
    clearVal.Format = VIEW_FORMAT;
    memcpy(clearVal.Color, clearColor, sizeof(clearColor));

    D3D12_HEAP_PROPERTIES heapProps{};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    ThrowIfFailed(device->CreateCommittedResource(
        &heapProps, D3D12_HEAP_FLAG_NONE, &texDesc,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
        &clearVal, IID_PPV_ARGS(&m_texture)));

    if (!m_rtvHeap)
    {
        D3D12_DESCRIPTOR_HEAP_DESC rtvDesc{};
        rtvDesc.NumDescriptors = 1;
        rtvDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rtvDesc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        ThrowIfFailed(device->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&m_rtvHeap)));
    }
    device->CreateRenderTargetView(
        m_texture.Get(), nullptr,
        m_rtvHeap->GetCPUDescriptorHandleForHeapStart());

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format                  = VIEW_FORMAT;
    srvDesc.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels     = 1;
    device->CreateShaderResourceView(m_texture.Get(), &srvDesc, m_srvCpu);

    D3D12_RESOURCE_DESC depthDesc = texDesc;
    depthDesc.Format = DXGI_FORMAT_D32_FLOAT;
    depthDesc.Flags  = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_CLEAR_VALUE depthClear{};
    depthClear.Format               = DXGI_FORMAT_D32_FLOAT;
    depthClear.DepthStencil.Depth   = 1.0f;
    depthClear.DepthStencil.Stencil = 0;

    ThrowIfFailed(device->CreateCommittedResource(
        &heapProps, D3D12_HEAP_FLAG_NONE, &depthDesc,
        D3D12_RESOURCE_STATE_DEPTH_WRITE, &depthClear,
        IID_PPV_ARGS(&m_depthBuffer)));

    if (!m_dsvHeap)
    {
        D3D12_DESCRIPTOR_HEAP_DESC dsvDesc{};
        dsvDesc.NumDescriptors = 1;
        dsvDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        dsvDesc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        ThrowIfFailed(device->CreateDescriptorHeap(&dsvDesc, IID_PPV_ARGS(&m_dsvHeap)));
    }
    D3D12_DEPTH_STENCIL_VIEW_DESC dsvViewDesc{};
    dsvViewDesc.Format        = DXGI_FORMAT_D32_FLOAT;
    dsvViewDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    device->CreateDepthStencilView(
        m_depthBuffer.Get(), &dsvViewDesc,
        m_dsvHeap->GetCPUDescriptorHandleForHeapStart());
}

// ---------------------------------------------------------------------------
// Render
// ---------------------------------------------------------------------------
void View::Render(ID3D12GraphicsCommandList* cmdList,
                  D3D12_CPU_DESCRIPTOR_HANDLE mainRtv,
                  std::function<void(ID3D12GraphicsCommandList*)> drawFn)
{
    D3D12_RESOURCE_BARRIER toRtv{};
    toRtv.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toRtv.Transition.pResource   = m_texture.Get();
    toRtv.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    toRtv.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
    toRtv.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cmdList->ResourceBarrier(1, &toRtv);

    D3D12_CPU_DESCRIPTOR_HANDLE sceneRtv = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    D3D12_CPU_DESCRIPTOR_HANDLE dsv      = m_dsvHeap->GetCPUDescriptorHandleForHeapStart();

    D3D12_VIEWPORT vp{ 0.f, 0.f,
        static_cast<float>(m_width), static_cast<float>(m_height), 0.f, 1.f };
    D3D12_RECT sr{ 0, 0,
        static_cast<LONG>(m_width), static_cast<LONG>(m_height) };

    cmdList->RSSetViewports(1, &vp);
    cmdList->RSSetScissorRects(1, &sr);
    cmdList->OMSetRenderTargets(1, &sceneRtv, FALSE, &dsv);

    const float clearColor[4] = { 0.0f, 0.0f, 0.502f, 1.0f }; // navy blue
    cmdList->ClearRenderTargetView(sceneRtv, clearColor, 0, nullptr);
    cmdList->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

    if (drawFn)
        drawFn(cmdList);

    D3D12_RESOURCE_BARRIER toSrv{};
    toSrv.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toSrv.Transition.pResource   = m_texture.Get();
    toSrv.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    toSrv.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    toSrv.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cmdList->ResourceBarrier(1, &toSrv);

    cmdList->OMSetRenderTargets(1, &mainRtv, FALSE, nullptr);
}
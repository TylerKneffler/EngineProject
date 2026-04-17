#include "DX12GraphicsContext.h"
#include "Core/Graphics/IGraphicsBuffer.h"
#include "Core/Graphics/IPipelineState.h"
#include "DX12GraphicsBuffer.h"
#include <stdexcept>

// ---------------------------------------------------------------------------
// D3D12GraphicsContext
// ---------------------------------------------------------------------------

D3D12GraphicsContext::D3D12GraphicsContext(ID3D12GraphicsCommandList* cmdList)
    : m_cmdList(cmdList)
{
}

void D3D12GraphicsContext::SetPipeline(const IPipelineState* pipeline)
{
    if (!pipeline || !m_cmdList) return;

    auto* nativeHandle = static_cast<ID3D12PipelineState*>(const_cast<void*>(pipeline->GetNativeHandle()));
    if (nativeHandle)
    {
        m_cmdList->SetPipelineState(nativeHandle);
    }
}

void D3D12GraphicsContext::SetConstantBuffer(uint32_t slot, const IGraphicsBuffer* buffer, uint64_t offset)
{
    if (!buffer || !m_cmdList) return;

    auto* d3dBuffer = static_cast<D3D12GraphicsBuffer*>(const_cast<IGraphicsBuffer*>(buffer));
    D3D12_GPU_VIRTUAL_ADDRESS gpuAddr = d3dBuffer->GetGPUVirtualAddress() + offset;
    m_cmdList->SetGraphicsRootConstantBufferView(slot, gpuAddr);
}

void D3D12GraphicsContext::SetVertexBuffer(uint32_t slot, const IGraphicsBuffer* buffer, uint32_t stride, uint64_t offset)
{
    if (!buffer || !m_cmdList) return;

    auto* d3dBuffer = static_cast<D3D12GraphicsBuffer*>(const_cast<IGraphicsBuffer*>(buffer));
    
    D3D12_VERTEX_BUFFER_VIEW vbv{};
    vbv.BufferLocation = d3dBuffer->GetGPUVirtualAddress() + offset;
    vbv.SizeInBytes = static_cast<UINT>(d3dBuffer->GetSize() - offset);
    vbv.StrideInBytes = stride;

    m_cmdList->IASetVertexBuffers(slot, 1, &vbv);
}

void D3D12GraphicsContext::SetIndexBuffer(const IGraphicsBuffer* buffer, uint32_t indexCount, uint64_t offset)
{
    if (!buffer || !m_cmdList) return;

    auto* d3dBuffer = static_cast<D3D12GraphicsBuffer*>(const_cast<IGraphicsBuffer*>(buffer));
    
    D3D12_INDEX_BUFFER_VIEW ibv{};
    ibv.BufferLocation = d3dBuffer->GetGPUVirtualAddress() + offset;
    ibv.SizeInBytes = static_cast<UINT>(d3dBuffer->GetSize() - offset);
    ibv.Format = DXGI_FORMAT_R32_UINT;

    m_cmdList->IASetIndexBuffer(&ibv);
    m_indexCount = indexCount;
}

void D3D12GraphicsContext::SetViewport(const Viewport& vp)
{
    if (!m_cmdList) return;

    D3D12_VIEWPORT viewport{};
    viewport.TopLeftX = vp.x;
    viewport.TopLeftY = vp.y;
    viewport.Width = vp.width;
    viewport.Height = vp.height;
    viewport.MinDepth = vp.minDepth;
    viewport.MaxDepth = vp.maxDepth;

    m_cmdList->RSSetViewports(1, &viewport);
}

void D3D12GraphicsContext::SetScissorRect(const ScissorRect& rect)
{
    if (!m_cmdList) return;

    D3D12_RECT scissorRect{};
    scissorRect.left = rect.left;
    scissorRect.top = rect.top;
    scissorRect.right = rect.right;
    scissorRect.bottom = rect.bottom;

    m_cmdList->RSSetScissorRects(1, &scissorRect);
}

void D3D12GraphicsContext::Clear(float r, float g, float b, float a, float depth)
{
    // This would typically clear the current render target
    // Implementation depends on having RTV/DSV descriptors
    // For now, this is a placeholder
}

void D3D12GraphicsContext::DrawInstanced(
    uint32_t vertexCountPerInstance,
    uint32_t instanceCount,
    uint32_t startVertexLocation,
    uint32_t startInstanceLocation)
{
    if (!m_cmdList) return;
    m_cmdList->DrawInstanced(vertexCountPerInstance, instanceCount, startVertexLocation, startInstanceLocation);
}

void D3D12GraphicsContext::DrawIndexedInstanced(
    uint32_t indexCountPerInstance,
    uint32_t instanceCount,
    uint32_t startIndexLocation,
    int32_t baseVertexLocation,
    uint32_t startInstanceLocation)
{
    if (!m_cmdList) return;
    m_cmdList->DrawIndexedInstanced(indexCountPerInstance, instanceCount, startIndexLocation, baseVertexLocation, startInstanceLocation);
}

void D3D12GraphicsContext::TransitionResource(void* resource, ResourceState stateBefore, ResourceState stateAfter)
{
    if (!resource || !m_cmdList) return;

    auto* d3dResource = static_cast<ID3D12Resource*>(resource);
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = d3dResource;
    barrier.Transition.StateBefore = ConvertResourceState(stateBefore);
    barrier.Transition.StateAfter = ConvertResourceState(stateAfter);
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

    m_cmdList->ResourceBarrier(1, &barrier);
}

D3D12_RESOURCE_STATES D3D12GraphicsContext::ConvertResourceState(ResourceState state) const
{
    switch (state)
    {
        case ResourceState::Common: return D3D12_RESOURCE_STATE_COMMON;
        case ResourceState::VertexAndConstantBuffer: return D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
        case ResourceState::IndexBuffer: return D3D12_RESOURCE_STATE_INDEX_BUFFER;
        case ResourceState::RenderTarget: return D3D12_RESOURCE_STATE_RENDER_TARGET;
        case ResourceState::UnorderedAccess: return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        case ResourceState::DepthWrite: return D3D12_RESOURCE_STATE_DEPTH_WRITE;
        case ResourceState::DepthRead: return D3D12_RESOURCE_STATE_DEPTH_READ;
        case ResourceState::ShaderResource: return D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        case ResourceState::StreamOut: return D3D12_RESOURCE_STATE_STREAM_OUT;
        case ResourceState::IndirectArgument: return D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
        case ResourceState::CopyDest: return D3D12_RESOURCE_STATE_COPY_DEST;
        case ResourceState::CopySrc: return D3D12_RESOURCE_STATE_COPY_SOURCE;
        case ResourceState::Present: return D3D12_RESOURCE_STATE_PRESENT;
        default: return D3D12_RESOURCE_STATE_COMMON;
    }
}

// ---------------------------------------------------------------------------
// D3D12GraphicsContextFactory
// ---------------------------------------------------------------------------

D3D12GraphicsContextFactory::D3D12GraphicsContextFactory(
    ID3D12Device* device,
    ID3D12CommandQueue* commandQueue)
    : m_device(device), m_commandQueue(commandQueue)
{
    // Create command allocator
    device->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        IID_PPV_ARGS(&m_cmdAllocator));

    // Create command list
    device->CreateCommandList(
        0,
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        m_cmdAllocator.Get(),
        nullptr,
        IID_PPV_ARGS(&m_cmdList));
}

std::unique_ptr<IGraphicsContext> D3D12GraphicsContextFactory::CreateContext()
{
    // In a full implementation, this would reset the allocator and command list
    // and return a new wrapper. For now, we return a wrapper around the existing list.
    return std::make_unique<D3D12GraphicsContext>(m_cmdList.Get());
}

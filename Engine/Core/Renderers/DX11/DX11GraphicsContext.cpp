#include "pch.h"
#include "DX11GraphicsContext.h"
#include "DX11GraphicsBuffer.h"
#include "DX11PipelineStateBuilder.h"
#include <algorithm>
#include <cstring>

D3D11GraphicsContext::D3D11GraphicsContext(ID3D11Device* device, ID3D11DeviceContext* context)
    : m_device(device), m_context(context)
{
}

void D3D11GraphicsContext::SetPipeline(const IPipelineState* state)
{
    if (!m_context || !state) return;
    const auto* pipeline = dynamic_cast<const D3D11PipelineState*>(state);
    if (!pipeline) return;

    m_context->VSSetShader(pipeline->vertexShader.Get(), nullptr, 0);
    m_context->PSSetShader(pipeline->pixelShader.Get(), nullptr, 0);
    m_context->IASetInputLayout(pipeline->inputLayout.Get());
    m_context->IASetPrimitiveTopology(pipeline->topology);
    m_context->RSSetState(pipeline->rasterizerState.Get());
    const float blendFactor[4]{};
    m_context->OMSetBlendState(pipeline->blendState.Get(), blendFactor, UINT_MAX);
    m_context->OMSetDepthStencilState(pipeline->depthStencilState.Get(), 0);
}

void D3D11GraphicsContext::SetConstantBuffer(uint32_t slot, const IGraphicsBuffer* buffer, uint64_t offset)
{
    if (!m_device || !m_context || !buffer || slot >= CONSTANT_BUFFER_SLOTS) return;
    const auto* source = dynamic_cast<const D3D11GraphicsBuffer*>(buffer);
    if (!source || !source->GetShadowData() || offset >= source->GetSize()) return;

    if (!m_constantBuffers[slot])
    {
        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth = CONSTANT_BUFFER_SIZE;
        desc.Usage = D3D11_USAGE_DYNAMIC;
        desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        ThrowIfFailed(m_device->CreateBuffer(&desc, nullptr, &m_constantBuffers[slot]));
    }

    D3D11_MAPPED_SUBRESOURCE mapped{};
    ThrowIfFailed(m_context->Map(m_constantBuffers[slot].Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped));
    std::memset(mapped.pData, 0, CONSTANT_BUFFER_SIZE);
    const size_t available = static_cast<size_t>(source->GetSize() - offset);
    std::memcpy(mapped.pData, source->GetShadowData() + offset,
                std::min<size_t>(CONSTANT_BUFFER_SIZE, available));
    m_context->Unmap(m_constantBuffers[slot].Get(), 0);

    ID3D11Buffer* native = m_constantBuffers[slot].Get();
    m_context->VSSetConstantBuffers(slot, 1, &native);
    m_context->PSSetConstantBuffers(slot, 1, &native);
}

void D3D11GraphicsContext::SetVertexBuffer(uint32_t slot, const IGraphicsBuffer* buffer, uint32_t stride, uint64_t offset)
{
    if (!m_context || !buffer || offset > UINT_MAX) return;
    const auto* nativeBuffer = dynamic_cast<const D3D11GraphicsBuffer*>(buffer);
    if (!nativeBuffer) return;
    ID3D11Buffer* native = nativeBuffer->GetBuffer();
    UINT nativeOffset = static_cast<UINT>(offset);
    m_context->IASetVertexBuffers(slot, 1, &native, &stride, &nativeOffset);
}

void D3D11GraphicsContext::SetIndexBuffer(const IGraphicsBuffer* buffer, uint32_t, uint64_t offset)
{
    if (!m_context || !buffer || offset > UINT_MAX) return;
    const auto* nativeBuffer = dynamic_cast<const D3D11GraphicsBuffer*>(buffer);
    if (nativeBuffer)
        m_context->IASetIndexBuffer(nativeBuffer->GetBuffer(), DXGI_FORMAT_R32_UINT, static_cast<UINT>(offset));
}

void D3D11GraphicsContext::SetViewport(const Viewport& value)
{
    if (!m_context) return;
    D3D11_VIEWPORT viewport{ value.x, value.y, value.width, value.height, value.minDepth, value.maxDepth };
    m_context->RSSetViewports(1, &viewport);
}

void D3D11GraphicsContext::SetScissorRect(const ScissorRect& value)
{
    if (!m_context) return;
    D3D11_RECT rect{ value.left, value.top, value.right, value.bottom };
    m_context->RSSetScissorRects(1, &rect);
}

void D3D11GraphicsContext::Clear(float, float, float, float, float) {}

void D3D11GraphicsContext::DrawInstanced(uint32_t vertices, uint32_t instances,
                                         uint32_t startVertex, uint32_t startInstance)
{
    if (m_context) m_context->DrawInstanced(vertices, instances, startVertex, startInstance);
}

void D3D11GraphicsContext::DrawIndexedInstanced(uint32_t indices, uint32_t instances,
                                                uint32_t startIndex, int32_t baseVertex,
                                                uint32_t startInstance)
{
    if (m_context) m_context->DrawIndexedInstanced(indices, instances, startIndex, baseVertex, startInstance);
}

void D3D11GraphicsContext::TransitionResource(void*, ResourceState, ResourceState) {}

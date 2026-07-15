#include "pch.h"
#include "DX11PipelineStateBuilder.h"
#include <cstring>
#include <sstream>

IPipelineStateBuilder& D3D11PipelineStateBuilder::SetVertexShader(const IShader* shader)
{
    m_vsBytecode.clear();
    if (shader && shader->GetBytecode())
    {
        const auto* begin = static_cast<const uint8_t*>(shader->GetBytecode());
        m_vsBytecode.assign(begin, begin + shader->GetBytecodeSize());
    }
    return *this;
}

IPipelineStateBuilder& D3D11PipelineStateBuilder::SetPixelShader(const IShader* shader)
{
    m_psBytecode.clear();
    if (shader && shader->GetBytecode())
    {
        const auto* begin = static_cast<const uint8_t*>(shader->GetBytecode());
        m_psBytecode.assign(begin, begin + shader->GetBytecodeSize());
    }
    return *this;
}

IPipelineStateBuilder& D3D11PipelineStateBuilder::SetFillMode(bool value) { m_fillMode = value ? D3D11_FILL_WIREFRAME : D3D11_FILL_SOLID; return *this; }
IPipelineStateBuilder& D3D11PipelineStateBuilder::SetCullMode(bool value) { m_cullMode = value ? D3D11_CULL_BACK : D3D11_CULL_NONE; return *this; }
IPipelineStateBuilder& D3D11PipelineStateBuilder::SetFrontCounterClockwise(bool value) { m_frontCCW = value; return *this; }
IPipelineStateBuilder& D3D11PipelineStateBuilder::SetDepthClipEnable(bool value) { m_depthClip = value; return *this; }
IPipelineStateBuilder& D3D11PipelineStateBuilder::SetBlendEnable(bool value) { m_blendEnable = value; return *this; }
IPipelineStateBuilder& D3D11PipelineStateBuilder::SetSrcBlend(int value) { m_srcBlend = ConvertBlend(value); return *this; }
IPipelineStateBuilder& D3D11PipelineStateBuilder::SetDestBlend(int value) { m_destBlend = ConvertBlend(value); return *this; }
IPipelineStateBuilder& D3D11PipelineStateBuilder::SetBlendOp(int value) { m_blendOp = ConvertBlendOp(value); return *this; }
IPipelineStateBuilder& D3D11PipelineStateBuilder::SetSrcBlendAlpha(int value) { m_srcBlendAlpha = ConvertBlend(value); return *this; }
IPipelineStateBuilder& D3D11PipelineStateBuilder::SetDestBlendAlpha(int value) { m_destBlendAlpha = ConvertBlend(value); return *this; }
IPipelineStateBuilder& D3D11PipelineStateBuilder::SetBlendOpAlpha(int value) { m_blendOpAlpha = ConvertBlendOp(value); return *this; }
IPipelineStateBuilder& D3D11PipelineStateBuilder::SetDepthEnable(bool value) { m_depthEnable = value; return *this; }
IPipelineStateBuilder& D3D11PipelineStateBuilder::SetDepthWriteEnable(bool value) { m_depthWrite = value; return *this; }
IPipelineStateBuilder& D3D11PipelineStateBuilder::SetDepthFunc(int value) { m_depthFunc = ConvertComparison(value); return *this; }
IPipelineStateBuilder& D3D11PipelineStateBuilder::SetRenderTargetFormat(int, int) { return *this; }

IPipelineStateBuilder& D3D11PipelineStateBuilder::SetInputLayout(const VertexElement* elements, uint32_t count)
{
    m_inputLayout.clear();
    for (uint32_t i = 0; elements && i < count; ++i)
    {
        D3D11_INPUT_ELEMENT_DESC desc{};
        desc.SemanticName = elements[i].semanticName;
        desc.SemanticIndex = elements[i].semanticIndex;
        desc.Format = static_cast<DXGI_FORMAT>(elements[i].format);
        desc.InputSlot = elements[i].inputSlot;
        desc.AlignedByteOffset = elements[i].alignedByteOffset;
        desc.InputSlotClass = elements[i].perInstance ? D3D11_INPUT_PER_INSTANCE_DATA : D3D11_INPUT_PER_VERTEX_DATA;
        desc.InstanceDataStepRate = elements[i].perInstance ? 1u : 0u;
        m_inputLayout.push_back(desc);
    }
    return *this;
}

IPipelineStateBuilder& D3D11PipelineStateBuilder::SetPrimitiveTopology(PrimitiveTopology value)
{
    switch (value)
    {
        case PrimitiveTopology::PointList: m_topology = D3D11_PRIMITIVE_TOPOLOGY_POINTLIST; break;
        case PrimitiveTopology::LineList: m_topology = D3D11_PRIMITIVE_TOPOLOGY_LINELIST; break;
        case PrimitiveTopology::LineStrip: m_topology = D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP; break;
        case PrimitiveTopology::TriangleList: m_topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST; break;
        case PrimitiveTopology::TriangleStrip: m_topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP; break;
    }
    return *this;
}

std::unique_ptr<IPipelineState> D3D11PipelineStateBuilder::Build()
{
    m_lastError.clear();
    if (!m_device || m_vsBytecode.empty() || m_psBytecode.empty())
    {
        m_lastError = "DX11 pipeline requires a device, vertex shader, and pixel shader";
        return nullptr;
    }

    auto pipeline = std::make_unique<D3D11PipelineState>();
    HRESULT hr = m_device->CreateVertexShader(m_vsBytecode.data(), m_vsBytecode.size(), nullptr, &pipeline->vertexShader);
    if (SUCCEEDED(hr))
        hr = m_device->CreatePixelShader(m_psBytecode.data(), m_psBytecode.size(), nullptr, &pipeline->pixelShader);
    if (SUCCEEDED(hr) && !m_inputLayout.empty())
        hr = m_device->CreateInputLayout(m_inputLayout.data(), static_cast<UINT>(m_inputLayout.size()),
            m_vsBytecode.data(), m_vsBytecode.size(), &pipeline->inputLayout);

    D3D11_RASTERIZER_DESC raster{};
    raster.FillMode = m_fillMode;
    raster.CullMode = m_cullMode;
    raster.FrontCounterClockwise = m_frontCCW;
    raster.DepthClipEnable = m_depthClip;
    if (SUCCEEDED(hr)) hr = m_device->CreateRasterizerState(&raster, &pipeline->rasterizerState);

    D3D11_BLEND_DESC blend{};
    blend.RenderTarget[0].BlendEnable = m_blendEnable;
    blend.RenderTarget[0].SrcBlend = m_srcBlend;
    blend.RenderTarget[0].DestBlend = m_destBlend;
    blend.RenderTarget[0].BlendOp = m_blendOp;
    blend.RenderTarget[0].SrcBlendAlpha = m_srcBlendAlpha;
    blend.RenderTarget[0].DestBlendAlpha = m_destBlendAlpha;
    blend.RenderTarget[0].BlendOpAlpha = m_blendOpAlpha;
    blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (SUCCEEDED(hr)) hr = m_device->CreateBlendState(&blend, &pipeline->blendState);

    D3D11_DEPTH_STENCIL_DESC depth{};
    depth.DepthEnable = m_depthEnable;
    depth.DepthWriteMask = m_depthWrite ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
    depth.DepthFunc = m_depthFunc;
    if (SUCCEEDED(hr)) hr = m_device->CreateDepthStencilState(&depth, &pipeline->depthStencilState);

    if (FAILED(hr))
    {
        std::ostringstream out;
        out << "Failed to create DX11 pipeline state (HRESULT 0x" << std::hex << hr << ')';
        m_lastError = out.str();
        return nullptr;
    }
    pipeline->topology = m_topology;
    return pipeline;
}

D3D11_BLEND D3D11PipelineStateBuilder::ConvertBlend(int value)
{
    switch (value)
    {
        case 0: return D3D11_BLEND_ZERO; case 1: return D3D11_BLEND_ONE;
        case 2: return D3D11_BLEND_SRC_COLOR; case 3: return D3D11_BLEND_INV_SRC_COLOR;
        case 4: return D3D11_BLEND_SRC_ALPHA; case 5: return D3D11_BLEND_INV_SRC_ALPHA;
        case 6: return D3D11_BLEND_DEST_ALPHA; case 7: return D3D11_BLEND_INV_DEST_ALPHA;
        case 8: return D3D11_BLEND_DEST_COLOR; case 9: return D3D11_BLEND_INV_DEST_COLOR;
        default: return D3D11_BLEND_ONE;
    }
}

D3D11_BLEND_OP D3D11PipelineStateBuilder::ConvertBlendOp(int value)
{
    switch (value)
    {
        case 1: return D3D11_BLEND_OP_SUBTRACT; case 2: return D3D11_BLEND_OP_REV_SUBTRACT;
        case 3: return D3D11_BLEND_OP_MIN; case 4: return D3D11_BLEND_OP_MAX;
        default: return D3D11_BLEND_OP_ADD;
    }
}

D3D11_COMPARISON_FUNC D3D11PipelineStateBuilder::ConvertComparison(int value)
{
    switch (value)
    {
        case 0: return D3D11_COMPARISON_NEVER; case 1: return D3D11_COMPARISON_LESS;
        case 2: return D3D11_COMPARISON_EQUAL; case 3: return D3D11_COMPARISON_LESS_EQUAL;
        case 4: return D3D11_COMPARISON_GREATER; case 5: return D3D11_COMPARISON_NOT_EQUAL;
        case 6: return D3D11_COMPARISON_GREATER_EQUAL; case 7: return D3D11_COMPARISON_ALWAYS;
        default: return D3D11_COMPARISON_LESS;
    }
}

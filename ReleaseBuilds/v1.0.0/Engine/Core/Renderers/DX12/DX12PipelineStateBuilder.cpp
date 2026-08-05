#include "DX12PipelineStateBuilder.h"
#include "Core/Graphics/IShader.h"
#include "DX12ShaderCompiler.h"
#include <stdexcept>
#include <sstream>

// ---------------------------------------------------------------------------
// D3D12PipelineStateBuilder Implementation
// ---------------------------------------------------------------------------

D3D12PipelineStateBuilder::D3D12PipelineStateBuilder(ID3D12Device* device, ID3D12RootSignature* rootSig)
    : m_device(device), m_rootSig(rootSig)
{
}

IPipelineStateBuilder& D3D12PipelineStateBuilder::SetVertexShader(const IShader* shader)
{
    if (shader)
    {
        auto vsbytes = shader->GetBytecode();
        auto vssize = shader->GetBytecodeSize();
        D3DCreateBlob(vssize, &m_vsBytecode);
        memcpy(m_vsBytecode->GetBufferPointer(), vsbytes, vssize);
    }
    return *this;
}

IPipelineStateBuilder& D3D12PipelineStateBuilder::SetPixelShader(const IShader* shader)
{
    if (shader)
    {
        auto psbytes = shader->GetBytecode();
        auto pssize = shader->GetBytecodeSize();
        D3DCreateBlob(pssize, &m_psBytecode);
        memcpy(m_psBytecode->GetBufferPointer(), psbytes, pssize);
    }
    return *this;
}



IPipelineStateBuilder& D3D12PipelineStateBuilder::SetFillMode(bool wireframe)
{
    m_fillMode = wireframe ? D3D12_FILL_MODE_WIREFRAME : D3D12_FILL_MODE_SOLID;
    return *this;
}

IPipelineStateBuilder& D3D12PipelineStateBuilder::SetCullMode(bool cullBackFaces)
{
    m_cullMode = cullBackFaces ? D3D12_CULL_MODE_BACK : D3D12_CULL_MODE_NONE;
    return *this;
}

IPipelineStateBuilder& D3D12PipelineStateBuilder::SetFrontCounterClockwise(bool ccw)
{
    m_frontCCW = ccw ? TRUE : FALSE;
    return *this;
}

IPipelineStateBuilder& D3D12PipelineStateBuilder::SetDepthClipEnable(bool enable)
{
    m_depthClip = enable ? TRUE : FALSE;
    return *this;
}

IPipelineStateBuilder& D3D12PipelineStateBuilder::SetBlendEnable(bool enable)
{
    m_blendEnable = enable ? TRUE : FALSE;
    return *this;
}

IPipelineStateBuilder& D3D12PipelineStateBuilder::SetSrcBlend(int mode)
{
    m_srcBlend = ConvertBlendMode(mode);
    return *this;
}

IPipelineStateBuilder& D3D12PipelineStateBuilder::SetDestBlend(int mode)
{
    m_destBlend = ConvertBlendMode(mode);
    return *this;
}

IPipelineStateBuilder& D3D12PipelineStateBuilder::SetBlendOp(int op)
{
    m_blendOp = ConvertBlendOp(op);
    return *this;
}

IPipelineStateBuilder& D3D12PipelineStateBuilder::SetSrcBlendAlpha(int mode)
{
    m_srcBlendAlpha = ConvertBlendMode(mode);
    return *this;
}

IPipelineStateBuilder& D3D12PipelineStateBuilder::SetDestBlendAlpha(int mode)
{
    m_destBlendAlpha = ConvertBlendMode(mode);
    return *this;
}

IPipelineStateBuilder& D3D12PipelineStateBuilder::SetBlendOpAlpha(int op)
{
    m_blendOpAlpha = ConvertBlendOp(op);
    return *this;
}

IPipelineStateBuilder& D3D12PipelineStateBuilder::SetDepthEnable(bool enable)
{
    m_depthEnable = enable ? TRUE : FALSE;
    return *this;
}

IPipelineStateBuilder& D3D12PipelineStateBuilder::SetDepthWriteEnable(bool enable)
{
    m_depthWriteEnable = enable ? TRUE : FALSE;
    return *this;
}

IPipelineStateBuilder& D3D12PipelineStateBuilder::SetDepthFunc(int func)
{
    m_depthFunc = ConvertComparisonFunc(func);
    return *this;
}

IPipelineStateBuilder& D3D12PipelineStateBuilder::SetInputLayout(const VertexElement* elements, uint32_t elementCount)
{
    m_inputLayout.clear();
    for (uint32_t i = 0; i < elementCount; ++i)
    {
        const auto& elem = elements[i];
        D3D12_INPUT_ELEMENT_DESC desc{};
        desc.SemanticName = elem.semanticName;
        desc.SemanticIndex = elem.semanticIndex;
        desc.Format = static_cast<DXGI_FORMAT>(elem.format);
        desc.InputSlot = elem.inputSlot;
        desc.AlignedByteOffset = elem.alignedByteOffset;
        desc.InputSlotClass = elem.perInstance ? D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA : D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
        desc.InstanceDataStepRate = elem.perInstance ? 1 : 0;
        m_inputLayout.push_back(desc);
    }
    return *this;
}

IPipelineStateBuilder& D3D12PipelineStateBuilder::SetPrimitiveTopology(PrimitiveTopology topology)
{
    switch (topology)
    {
        case PrimitiveTopology::PointList:
            m_primitiveTopology = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
            break;
        case PrimitiveTopology::LineList:
            m_primitiveTopology = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
            break;
        case PrimitiveTopology::LineStrip:
            m_primitiveTopology = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
            break;
        case PrimitiveTopology::TriangleList:
            m_primitiveTopology = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            break;
        case PrimitiveTopology::TriangleStrip:
            m_primitiveTopology = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            break;
    }
    return *this;
}

IPipelineStateBuilder& D3D12PipelineStateBuilder::SetRenderTargetFormat(int format, int depthFormat)
{
    m_rtFormat = static_cast<DXGI_FORMAT>(format);
    m_dsFormat = depthFormat >= 0 ? static_cast<DXGI_FORMAT>(depthFormat) : DXGI_FORMAT_D32_FLOAT;
    return *this;
}

std::unique_ptr<IPipelineState> D3D12PipelineStateBuilder::Build()
{
    if (!m_device || !m_rootSig)
    {
        m_lastError = "Device or root signature is null";
        return nullptr;
    }

    if (!m_vsBytecode || !m_psBytecode)
    {
        m_lastError = "Vertex and pixel shaders must be set";
        return nullptr;
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature = m_rootSig;

    // Shaders
    psoDesc.VS = {
        m_vsBytecode->GetBufferPointer(),
        m_vsBytecode->GetBufferSize()
    };
    psoDesc.PS = {
        m_psBytecode->GetBufferPointer(),
        m_psBytecode->GetBufferSize()
    };

    // Rasterizer
    psoDesc.RasterizerState.FillMode = m_fillMode;
    psoDesc.RasterizerState.CullMode = m_cullMode;
    psoDesc.RasterizerState.FrontCounterClockwise = m_frontCCW;
    psoDesc.RasterizerState.DepthClipEnable = m_depthClip;
    psoDesc.RasterizerState.AntialiasedLineEnable = FALSE;
    psoDesc.RasterizerState.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;

    // Blend
    psoDesc.BlendState.AlphaToCoverageEnable = FALSE;
    psoDesc.BlendState.IndependentBlendEnable = FALSE;
    psoDesc.BlendState.RenderTarget[0].BlendEnable = m_blendEnable;
    psoDesc.BlendState.RenderTarget[0].LogicOpEnable = FALSE;
    psoDesc.BlendState.RenderTarget[0].SrcBlend = m_srcBlend;
    psoDesc.BlendState.RenderTarget[0].DestBlend = m_destBlend;
    psoDesc.BlendState.RenderTarget[0].BlendOp = m_blendOp;
    psoDesc.BlendState.RenderTarget[0].SrcBlendAlpha = m_srcBlendAlpha;
    psoDesc.BlendState.RenderTarget[0].DestBlendAlpha = m_destBlendAlpha;
    psoDesc.BlendState.RenderTarget[0].BlendOpAlpha = m_blendOpAlpha;
    psoDesc.BlendState.RenderTarget[0].LogicOp = D3D12_LOGIC_OP_NOOP;
    psoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    // Depth
    psoDesc.DepthStencilState.DepthEnable = m_depthEnable;
    psoDesc.DepthStencilState.DepthWriteMask = m_depthWriteEnable ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
    psoDesc.DepthStencilState.DepthFunc = m_depthFunc;
    psoDesc.DepthStencilState.StencilEnable = FALSE;

    // Input layout
    if (!m_inputLayout.empty())
    {
        psoDesc.InputLayout.pInputElementDescs = m_inputLayout.data();
        psoDesc.InputLayout.NumElements = static_cast<UINT>(m_inputLayout.size());
    }

    // Topology
    psoDesc.PrimitiveTopologyType = m_primitiveTopology;

    // Render targets
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = m_rtFormat;
    psoDesc.DSVFormat = m_dsFormat;

    psoDesc.SampleDesc.Count = 1;
    psoDesc.SampleMask = UINT_MAX;

    Microsoft::WRL::ComPtr<ID3D12PipelineState> pso;
    HRESULT hr = m_device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&pso));

    if (FAILED(hr))
    {
        std::ostringstream oss;
        oss << "CreateGraphicsPipelineState failed with HRESULT 0x" << std::hex << hr;
        m_lastError = oss.str();
        return nullptr;
    }

    return std::unique_ptr<IPipelineState>(std::make_unique<D3D12PipelineState>(pso));
}

D3D12_BLEND D3D12PipelineStateBuilder::ConvertBlendMode(int mode) const
{
    switch (mode)
    {
        case 0: return D3D12_BLEND_ZERO;
        case 1: return D3D12_BLEND_ONE;
        case 2: return D3D12_BLEND_SRC_COLOR;
        case 3: return D3D12_BLEND_INV_SRC_COLOR;
        case 4: return D3D12_BLEND_SRC_ALPHA;
        case 5: return D3D12_BLEND_INV_SRC_ALPHA;
        case 6: return D3D12_BLEND_DEST_ALPHA;
        case 7: return D3D12_BLEND_INV_DEST_ALPHA;
        case 8: return D3D12_BLEND_DEST_COLOR;
        case 9: return D3D12_BLEND_INV_DEST_COLOR;
        default: return D3D12_BLEND_ONE;
    }
}

D3D12_BLEND_OP D3D12PipelineStateBuilder::ConvertBlendOp(int op) const
{
    switch (op)
    {
        case 0: return D3D12_BLEND_OP_ADD;
        case 1: return D3D12_BLEND_OP_SUBTRACT;
        case 2: return D3D12_BLEND_OP_REV_SUBTRACT;
        case 3: return D3D12_BLEND_OP_MIN;
        case 4: return D3D12_BLEND_OP_MAX;
        default: return D3D12_BLEND_OP_ADD;
    }
}

D3D12_COMPARISON_FUNC D3D12PipelineStateBuilder::ConvertComparisonFunc(int func) const
{
    switch (func)
    {
        case 0: return D3D12_COMPARISON_FUNC_NEVER;
        case 1: return D3D12_COMPARISON_FUNC_LESS;
        case 2: return D3D12_COMPARISON_FUNC_EQUAL;
        case 3: return D3D12_COMPARISON_FUNC_LESS_EQUAL;
        case 4: return D3D12_COMPARISON_FUNC_GREATER;
        case 5: return D3D12_COMPARISON_FUNC_NOT_EQUAL;
        case 6: return D3D12_COMPARISON_FUNC_GREATER_EQUAL;
        case 7: return D3D12_COMPARISON_FUNC_ALWAYS;
        default: return D3D12_COMPARISON_FUNC_LESS;
    }
}

// ---------------------------------------------------------------------------
// D3D12PipelineStateFactory Implementation
// ---------------------------------------------------------------------------

std::unique_ptr<IPipelineStateBuilder> D3D12PipelineStateFactory::CreateBuilder()
{
    return std::make_unique<D3D12PipelineStateBuilder>(m_device, m_rootSig);
}

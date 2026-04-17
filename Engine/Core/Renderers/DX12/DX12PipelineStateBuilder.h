#pragma once
#include "Core/Graphics/IPipelineState.h"
#include <wrl/client.h>
#include <d3d12.h>
#include <vector>
#include <memory>
#include <string>

// Forward declarations
class D3D12Shader;

// ---------------------------------------------------------------------------
// D3D12PipelineState — DirectX 12 pipeline state wrapper
// ---------------------------------------------------------------------------
class D3D12PipelineState : public IPipelineState
{
public:
    explicit D3D12PipelineState(Microsoft::WRL::ComPtr<ID3D12PipelineState> pso)
        : m_pso(pso) {}

    void* GetNativeHandle() const override
    {
        return m_pso.Get();
    }

    ID3D12PipelineState* GetD3D12PipelineState() const { return m_pso.Get(); }

private:
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_pso;
};

// ---------------------------------------------------------------------------
// D3D12PipelineStateBuilder — Fluent pipeline state construction
// ---------------------------------------------------------------------------
class D3D12PipelineStateBuilder : public IPipelineStateBuilder
{
public:
    explicit D3D12PipelineStateBuilder(ID3D12Device* device, ID3D12RootSignature* rootSig);

    IPipelineStateBuilder& SetVertexShader(const IShader* shader) override;
    IPipelineStateBuilder& SetPixelShader(const IShader* shader) override;

    IPipelineStateBuilder& SetFillMode(bool wireframe) override;
    IPipelineStateBuilder& SetCullMode(bool cullBackFaces) override;
    IPipelineStateBuilder& SetFrontCounterClockwise(bool ccw) override;
    IPipelineStateBuilder& SetDepthClipEnable(bool enable) override;

    IPipelineStateBuilder& SetBlendEnable(bool enable) override;
    IPipelineStateBuilder& SetSrcBlend(int mode) override;
    IPipelineStateBuilder& SetDestBlend(int mode) override;
    IPipelineStateBuilder& SetBlendOp(int op) override;
    IPipelineStateBuilder& SetSrcBlendAlpha(int mode) override;
    IPipelineStateBuilder& SetDestBlendAlpha(int mode) override;
    IPipelineStateBuilder& SetBlendOpAlpha(int op) override;

    IPipelineStateBuilder& SetDepthEnable(bool enable) override;
    IPipelineStateBuilder& SetDepthWriteEnable(bool enable) override;
    IPipelineStateBuilder& SetDepthFunc(int func) override;

    IPipelineStateBuilder& SetInputLayout(const VertexElement* elements, uint32_t elementCount) override;
    IPipelineStateBuilder& SetPrimitiveTopology(PrimitiveTopology topology) override;
    IPipelineStateBuilder& SetRenderTargetFormat(int format, int depthFormat = -1) override;

    std::unique_ptr<IPipelineState> Build() override;
    std::string GetLastError() const override { return m_lastError; }

private:
    ID3D12Device* m_device;
    ID3D12RootSignature* m_rootSig;
    mutable std::string m_lastError;

    // Pipeline state components
    Microsoft::WRL::ComPtr<ID3DBlob> m_vsBytecode;
    Microsoft::WRL::ComPtr<ID3DBlob> m_psBytecode;

    std::vector<D3D12_INPUT_ELEMENT_DESC> m_inputLayout;

    D3D12_FILL_MODE m_fillMode = D3D12_FILL_MODE_SOLID;
    D3D12_CULL_MODE m_cullMode = D3D12_CULL_MODE_BACK;
    BOOL m_frontCCW = TRUE;
    BOOL m_depthClip = TRUE;

    BOOL m_blendEnable = FALSE;
    D3D12_BLEND m_srcBlend = D3D12_BLEND_ONE;
    D3D12_BLEND m_destBlend = D3D12_BLEND_ZERO;
    D3D12_BLEND_OP m_blendOp = D3D12_BLEND_OP_ADD;
    D3D12_BLEND m_srcBlendAlpha = D3D12_BLEND_ONE;
    D3D12_BLEND m_destBlendAlpha = D3D12_BLEND_ZERO;
    D3D12_BLEND_OP m_blendOpAlpha = D3D12_BLEND_OP_ADD;

    BOOL m_depthEnable = TRUE;
    BOOL m_depthWriteEnable = TRUE;
    D3D12_COMPARISON_FUNC m_depthFunc = D3D12_COMPARISON_FUNC_LESS;

    D3D12_PRIMITIVE_TOPOLOGY_TYPE m_primitiveTopology = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    DXGI_FORMAT m_rtFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    DXGI_FORMAT m_dsFormat = DXGI_FORMAT_D32_FLOAT;

    D3D12_BLEND ConvertBlendMode(int mode) const;
    D3D12_BLEND_OP ConvertBlendOp(int op) const;
    D3D12_COMPARISON_FUNC ConvertComparisonFunc(int func) const;
};

// ---------------------------------------------------------------------------
// D3D12PipelineStateFactory — Create pipeline state builders
// ---------------------------------------------------------------------------
class D3D12PipelineStateFactory : public IPipelineStateFactory
{
public:
    D3D12PipelineStateFactory(ID3D12Device* device, ID3D12RootSignature* rootSig)
        : m_device(device), m_rootSig(rootSig) {}

    std::unique_ptr<IPipelineStateBuilder> CreateBuilder() override;

private:
    ID3D12Device* m_device;
    ID3D12RootSignature* m_rootSig;
};

#pragma once
#include "Core/Graphics/IPipelineState.h"
#include <wrl/client.h>
#include <d3d11.h>
#include <vector>

namespace Engine::Renderers
{
class D3D11PipelineState : public Engine::Graphics::IPipelineState
{
public:
    void* GetNativeHandle() const override { return const_cast<D3D11PipelineState*>(this); }

    Microsoft::WRL::ComPtr<ID3D11VertexShader> vertexShader;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> pixelShader;
    Microsoft::WRL::ComPtr<ID3D11InputLayout> inputLayout;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> rasterizerState;
    Microsoft::WRL::ComPtr<ID3D11BlendState> blendState;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depthStencilState;
    D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
};

class D3D11PipelineStateBuilder : public Engine::Graphics::IPipelineStateBuilder
{
public:
    explicit D3D11PipelineStateBuilder(ID3D11Device* device) : m_device(device) {}

    Engine::Graphics::IPipelineStateBuilder& SetVertexShader(const Engine::Graphics::IShader* shader) override;
    Engine::Graphics::IPipelineStateBuilder& SetPixelShader(const Engine::Graphics::IShader* shader) override;
    Engine::Graphics::IPipelineStateBuilder& SetFillMode(bool wireframe) override;
    Engine::Graphics::IPipelineStateBuilder& SetCullMode(bool cullBackFaces) override;
    Engine::Graphics::IPipelineStateBuilder& SetFrontCounterClockwise(bool ccw) override;
    Engine::Graphics::IPipelineStateBuilder& SetDepthClipEnable(bool enable) override;
    Engine::Graphics::IPipelineStateBuilder& SetBlendEnable(bool enable) override;
    Engine::Graphics::IPipelineStateBuilder& SetSrcBlend(int mode) override;
    Engine::Graphics::IPipelineStateBuilder& SetDestBlend(int mode) override;
    Engine::Graphics::IPipelineStateBuilder& SetBlendOp(int op) override;
    Engine::Graphics::IPipelineStateBuilder& SetSrcBlendAlpha(int mode) override;
    Engine::Graphics::IPipelineStateBuilder& SetDestBlendAlpha(int mode) override;
    Engine::Graphics::IPipelineStateBuilder& SetBlendOpAlpha(int op) override;
    Engine::Graphics::IPipelineStateBuilder& SetDepthEnable(bool enable) override;
    Engine::Graphics::IPipelineStateBuilder& SetDepthWriteEnable(bool enable) override;
    Engine::Graphics::IPipelineStateBuilder& SetDepthFunc(int func) override;
    Engine::Graphics::IPipelineStateBuilder& SetInputLayout(const VertexElement* elements, uint32_t elementCount) override;
    Engine::Graphics::IPipelineStateBuilder& SetPrimitiveTopology(PrimitiveTopology topology) override;
    Engine::Graphics::IPipelineStateBuilder& SetRenderTargetFormat(int format, int depthFormat = -1) override;
    std::unique_ptr<Engine::Graphics::IPipelineState> Build() override;
    std::string GetLastError() const override { return m_lastError; }

private:
    static D3D11_BLEND ConvertBlend(int value);
    static D3D11_BLEND_OP ConvertBlendOp(int value);
    static D3D11_COMPARISON_FUNC ConvertComparison(int value);

    ID3D11Device* m_device = nullptr;
    std::vector<uint8_t> m_vsBytecode;
    std::vector<uint8_t> m_psBytecode;
    std::vector<D3D11_INPUT_ELEMENT_DESC> m_inputLayout;
    D3D11_FILL_MODE m_fillMode = D3D11_FILL_SOLID;
    D3D11_CULL_MODE m_cullMode = D3D11_CULL_BACK;
    BOOL m_frontCCW = TRUE;
    BOOL m_depthClip = TRUE;
    BOOL m_blendEnable = FALSE;
    D3D11_BLEND m_srcBlend = D3D11_BLEND_ONE;
    D3D11_BLEND m_destBlend = D3D11_BLEND_ZERO;
    D3D11_BLEND_OP m_blendOp = D3D11_BLEND_OP_ADD;
    D3D11_BLEND m_srcBlendAlpha = D3D11_BLEND_ONE;
    D3D11_BLEND m_destBlendAlpha = D3D11_BLEND_ZERO;
    D3D11_BLEND_OP m_blendOpAlpha = D3D11_BLEND_OP_ADD;
    BOOL m_depthEnable = TRUE;
    BOOL m_depthWrite = TRUE;
    D3D11_COMPARISON_FUNC m_depthFunc = D3D11_COMPARISON_LESS;
    D3D11_PRIMITIVE_TOPOLOGY m_topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    std::string m_lastError;
};

class D3D11PipelineStateFactory : public Engine::Graphics::IPipelineStateFactory
{
public:
    explicit D3D11PipelineStateFactory(ID3D11Device* device) : m_device(device) {}
    std::unique_ptr<Engine::Graphics::IPipelineStateBuilder> CreateBuilder() override
    {
        return std::make_unique<D3D11PipelineStateBuilder>(m_device);
    }
private:
    ID3D11Device* m_device = nullptr;
};
}

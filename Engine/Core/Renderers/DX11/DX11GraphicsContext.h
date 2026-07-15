#pragma once
#include "Core/Graphics/IGraphicsContext.h"
#include <wrl/client.h>
#include <d3d11.h>
#include <array>

class D3D11GraphicsContext : public IGraphicsContext
{
public:
    D3D11GraphicsContext(ID3D11Device* device, ID3D11DeviceContext* context);

    void SetPipeline(const IPipelineState* pipeline) override;
    void SetConstantBuffer(uint32_t slot, const IGraphicsBuffer* buffer, uint64_t offset = 0) override;
    void SetVertexBuffer(uint32_t slot, const IGraphicsBuffer* buffer, uint32_t stride, uint64_t offset = 0) override;
    void SetIndexBuffer(const IGraphicsBuffer* buffer, uint32_t indexCount, uint64_t offset = 0) override;
    void SetViewport(const Viewport& vp) override;
    void SetScissorRect(const ScissorRect& rect) override;
    void Clear(float r, float g, float b, float a, float depth = 1.0f) override;
    void DrawInstanced(uint32_t vertexCountPerInstance, uint32_t instanceCount,
                       uint32_t startVertexLocation = 0, uint32_t startInstanceLocation = 0) override;
    void DrawIndexedInstanced(uint32_t indexCountPerInstance, uint32_t instanceCount,
                              uint32_t startIndexLocation = 0, int32_t baseVertexLocation = 0,
                              uint32_t startInstanceLocation = 0) override;
    void TransitionResource(void* resource, ResourceState stateBefore, ResourceState stateAfter) override;
    void* GetNativeHandle() const override { return m_context; }

private:
    static constexpr uint32_t CONSTANT_BUFFER_SIZE = 256;
    static constexpr uint32_t CONSTANT_BUFFER_SLOTS = 16;
    ID3D11Device* m_device = nullptr;
    ID3D11DeviceContext* m_context = nullptr;
    std::array<Microsoft::WRL::ComPtr<ID3D11Buffer>, CONSTANT_BUFFER_SLOTS> m_constantBuffers;
};

class D3D11GraphicsContextFactory : public IGraphicsContextFactory
{
public:
    D3D11GraphicsContextFactory(ID3D11Device* device, ID3D11DeviceContext* context)
        : m_device(device), m_context(context) {}

    void SetCommandBuffer(void* context) override
    {
        m_externalContext = static_cast<ID3D11DeviceContext*>(context);
    }
    std::unique_ptr<IGraphicsContext> CreateContext() override
    {
        return std::make_unique<D3D11GraphicsContext>(
            m_device, m_externalContext ? m_externalContext : m_context);
    }

private:
    ID3D11Device* m_device = nullptr;
    ID3D11DeviceContext* m_context = nullptr;
    ID3D11DeviceContext* m_externalContext = nullptr;
};

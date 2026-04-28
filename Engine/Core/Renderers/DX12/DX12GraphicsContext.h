#pragma once
#include "Core/Graphics/IGraphicsContext.h"
#include <wrl/client.h>
#include <d3d12.h>
#include <vector>

// ---------------------------------------------------------------------------
// D3D12GraphicsContext — DirectX 12 command recording wrapper
// ---------------------------------------------------------------------------
class D3D12GraphicsContext : public IGraphicsContext
{
public:
    explicit D3D12GraphicsContext(ID3D12GraphicsCommandList* cmdList);    D3D12GraphicsContext(ID3D12GraphicsCommandList* cmdList, ID3D12RootSignature* rootSig);
    void SetPipeline(const IPipelineState* pipeline) override;
    void SetConstantBuffer(uint32_t slot, const IGraphicsBuffer* buffer, uint64_t offset = 0) override;
    void SetVertexBuffer(uint32_t slot, const IGraphicsBuffer* buffer, uint32_t stride, uint64_t offset = 0) override;
    void SetIndexBuffer(const IGraphicsBuffer* buffer, uint32_t indexCount, uint64_t offset = 0) override;

    void SetViewport(const Viewport& vp) override;
    void SetScissorRect(const ScissorRect& rect) override;

    void Clear(float r, float g, float b, float a, float depth = 1.0f) override;
    void DrawInstanced(
        uint32_t vertexCountPerInstance,
        uint32_t instanceCount,
        uint32_t startVertexLocation = 0,
        uint32_t startInstanceLocation = 0) override;

    void DrawIndexedInstanced(
        uint32_t indexCountPerInstance,
        uint32_t instanceCount,
        uint32_t startIndexLocation = 0,
        int32_t baseVertexLocation = 0,
        uint32_t startInstanceLocation = 0) override;

    void TransitionResource(void* resource, ResourceState stateBefore, ResourceState stateAfter) override;

    void* GetNativeHandle() const override { return m_cmdList; }

private:
    ID3D12GraphicsCommandList* m_cmdList;
    ID3D12RootSignature* m_rootSignature = nullptr;
    uint32_t m_indexCount = 0;

    D3D12_RESOURCE_STATES ConvertResourceState(ResourceState state) const;
};

// ---------------------------------------------------------------------------
// D3D12GraphicsContextFactory — Create command lists
// ---------------------------------------------------------------------------
class D3D12GraphicsContextFactory : public IGraphicsContextFactory
{
public:
    D3D12GraphicsContextFactory(
        ID3D12Device* device,
        ID3D12CommandQueue* commandQueue,
        ID3D12RootSignature* rootSignature);

    std::unique_ptr<IGraphicsContext> CreateContext() override;

private:
    ID3D12Device* m_device;
    ID3D12CommandQueue* m_commandQueue;
    ID3D12RootSignature* m_rootSignature;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> m_cmdAllocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> m_cmdList;
};

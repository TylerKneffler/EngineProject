#pragma once
#include "Core/Graphics/IGraphicsContext.h"
#include <wrl/client.h>
#include <d3d12.h>
#include <vector>
#include <memory>

namespace Engine::Renderers
{
struct D3D12OcclusionQueryState;
struct D3D12GpuTimingState;

// ---------------------------------------------------------------------------
// D3D12GraphicsContext — DirectX 12 command recording wrapper
// ---------------------------------------------------------------------------
class D3D12GraphicsContext : public Engine::Graphics::IGraphicsContext
{
public:
    explicit D3D12GraphicsContext(ID3D12GraphicsCommandList* cmdList);
    D3D12GraphicsContext(ID3D12GraphicsCommandList* cmdList,
        ID3D12RootSignature* rootSig,
        std::shared_ptr<D3D12OcclusionQueryState> occlusionState = {},
        std::shared_ptr<D3D12GpuTimingState> gpuTimings = {});
    void SetPipeline(const Engine::Graphics::IPipelineState* pipeline) override;
    void SetStencilReference(uint32_t reference) override;
    void SetConstantBuffer(uint32_t slot, const Engine::Graphics::IGraphicsBuffer* buffer, uint64_t offset = 0) override;
    void SetStructuredBuffer(uint32_t slot, const Engine::Graphics::IGraphicsBuffer* buffer) override;
    void SetVertexBuffer(uint32_t slot, const Engine::Graphics::IGraphicsBuffer* buffer, uint32_t stride, uint64_t offset = 0) override;
    void SetIndexBuffer(const Engine::Graphics::IGraphicsBuffer* buffer, uint32_t indexCount, uint64_t offset = 0) override;
    void SetTexture(uint32_t slot, const Engine::Graphics::IGraphicsTexture* texture) override;

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

    bool BeginOcclusionFrame(uint64_t viewId, uint64_t sceneSignature) override;
    bool IsOccluded(uint64_t objectId) override;
    void BeginOcclusionQuery(uint64_t objectId) override;
    void EndOcclusionQuery() override;
    void BeginGpuTiming(Engine::Graphics::GpuTimingStage stage) override;
    void EndGpuTiming(Engine::Graphics::GpuTimingStage stage) override;

    void TransitionResource(void* resource, ResourceState stateBefore, ResourceState stateAfter) override;

    void* GetNativeHandle() const override { return m_cmdList; }

private:
    ID3D12GraphicsCommandList* m_cmdList;
    ID3D12RootSignature* m_rootSignature = nullptr;
    uint32_t m_indexCount = 0;
    std::shared_ptr<D3D12OcclusionQueryState> m_occlusionState;
    std::shared_ptr<D3D12GpuTimingState> m_gpuTimings;
    uint64_t m_occlusionViewId = 0;
    uint64_t m_occlusionSceneSignature = 0;
    uint64_t m_activeOcclusionKey = 0;
    uint32_t m_activeOcclusionIndex = UINT32_MAX;

    D3D12_RESOURCE_STATES ConvertResourceState(ResourceState state) const;
};

// ---------------------------------------------------------------------------
// D3D12GraphicsContextFactory — Create command lists
// ---------------------------------------------------------------------------
class D3D12GraphicsContextFactory : public Engine::Graphics::IGraphicsContextFactory
{
public:
    D3D12GraphicsContextFactory(
        ID3D12Device* device,
        ID3D12CommandQueue* commandQueue,
        ID3D12RootSignature* rootSignature);

    void SetCommandBuffer(void* cmd) override;
    void PrepareFrame(uint32_t frameSlot) override;
    void FinalizeFrame() override;
    Engine::Graphics::FrameTimingTelemetry GetFrameTimingTelemetry() const override;
    std::unique_ptr<Engine::Graphics::IGraphicsContext> CreateContext() override;

private:
    ID3D12Device* m_device;
    ID3D12CommandQueue* m_commandQueue;
    ID3D12RootSignature* m_rootSignature;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> m_cmdAllocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> m_cmdList;
    ID3D12GraphicsCommandList* m_externalCmdList = nullptr; // set via SetCommandBuffer
    std::shared_ptr<D3D12OcclusionQueryState> m_occlusionState;
    std::shared_ptr<D3D12GpuTimingState> m_gpuTimings;
};
}

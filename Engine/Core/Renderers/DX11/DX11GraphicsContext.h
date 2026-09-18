#pragma once
#include "Core/Graphics/IGraphicsContext.h"
#include <wrl/client.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <array>
#include <memory>
#include <unordered_map>

namespace Engine::Renderers
{
struct D3D11OcclusionQueryState
{
    struct Entry
    {
        Microsoft::WRL::ComPtr<ID3D11Query> query;
        uint64_t viewId = 0;
        uint64_t issueSignature = 0;
        uint64_t lastProbeFrame = 0;
        uint64_t lastTouchedFrame = 0;
        bool pending = false;
        bool occluded = false;
    };
    std::unordered_map<uint64_t, Entry> entries;
    std::unordered_map<uint64_t, uint64_t> viewSignatures;
    uint64_t frameIndex = 0;
};

struct D3D11FrameResourceState
{
    static constexpr uint32_t ConstantSize = 256;
    static constexpr uint32_t ConstantSlots = 16;
    static constexpr uint32_t ArenaSize = 4u * 1024u * 1024u;

    D3D11FrameResourceState(ID3D11Device* device, ID3D11DeviceContext* context);
    void PrepareFrame();

    Microsoft::WRL::ComPtr<ID3D11Buffer> constantArena;
    std::array<Microsoft::WRL::ComPtr<ID3D11Buffer>, ConstantSlots>
        fallbackConstantBuffers;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> materialSampler;
    uint32_t arenaCursor = 0;
    bool firstArenaWrite = true;
    bool arenaSupported = false;
    uint32_t arenaWrites = 0;
    uint32_t discardMaps = 0;
};

class D3D11GraphicsContext : public Engine::Graphics::IGraphicsContext
{
public:
    D3D11GraphicsContext(ID3D11Device* device, ID3D11DeviceContext* context,
        std::shared_ptr<D3D11OcclusionQueryState> occlusionState = {},
        std::shared_ptr<D3D11FrameResourceState> frameResources = {});

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
    void DrawInstanced(uint32_t vertexCountPerInstance, uint32_t instanceCount,
                       uint32_t startVertexLocation = 0, uint32_t startInstanceLocation = 0) override;
    void DrawIndexedInstanced(uint32_t indexCountPerInstance, uint32_t instanceCount,
                              uint32_t startIndexLocation = 0, int32_t baseVertexLocation = 0,
                              uint32_t startInstanceLocation = 0) override;
    bool BeginOcclusionFrame(uint64_t viewId, uint64_t sceneSignature) override;
    bool IsOccluded(uint64_t objectId) override;
    void BeginOcclusionQuery(uint64_t objectId) override;
    void EndOcclusionQuery() override;
    void TransitionResource(void* resource, ResourceState stateBefore, ResourceState stateAfter) override;
    void* GetNativeHandle() const override { return m_context; }

private:
    ID3D11Device* m_device = nullptr;
    ID3D11DeviceContext* m_context = nullptr;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext1> m_context1;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> m_depthStencilState;
    const Engine::Graphics::IPipelineState* m_boundPipeline = nullptr;
    std::array<ID3D11ShaderResourceView*,
        D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT> m_boundTextureViews{};
    std::array<bool, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT>
        m_textureSlotInitialized{};
    bool m_materialSamplerBound = false;
    uint32_t m_stencilReference = 0;
    std::shared_ptr<D3D11OcclusionQueryState> m_occlusionState;
    std::shared_ptr<D3D11FrameResourceState> m_frameResources;
    D3D11OcclusionQueryState::Entry* m_activeOcclusionQuery = nullptr;
    uint64_t m_occlusionViewId = 0;
    uint64_t m_occlusionSceneSignature = 0;
};

class D3D11GraphicsContextFactory : public Engine::Graphics::IGraphicsContextFactory
{
public:
    D3D11GraphicsContextFactory(ID3D11Device* device, ID3D11DeviceContext* context);

    void SetCommandBuffer(void* context) override
    {
        m_externalContext = static_cast<ID3D11DeviceContext*>(context);
    }
    void PrepareFrame(uint32_t) override
    {
        if (m_frameResources) m_frameResources->PrepareFrame();
    }
    bool UsesPersistentConstantBufferArena() const override
    { return m_frameResources && m_frameResources->arenaSupported; }
    uint32_t GetConstantBufferArenaWrites() const override
    { return m_frameResources ? m_frameResources->arenaWrites : 0; }
    uint32_t GetConstantBufferDiscardMaps() const override
    { return m_frameResources ? m_frameResources->discardMaps : 0; }
    std::unique_ptr<Engine::Graphics::IGraphicsContext> CreateContext() override
    {
        return std::make_unique<D3D11GraphicsContext>(
            m_device, m_externalContext ? m_externalContext : m_context,
            m_occlusionState, m_frameResources);
    }

private:
    ID3D11Device* m_device = nullptr;
    ID3D11DeviceContext* m_context = nullptr;
    ID3D11DeviceContext* m_externalContext = nullptr;
    std::shared_ptr<D3D11OcclusionQueryState> m_occlusionState;
    std::shared_ptr<D3D11FrameResourceState> m_frameResources;
};
}

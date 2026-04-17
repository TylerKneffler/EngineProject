#pragma once
#include <memory>
#include <cstdint>
#include "IPipelineState.h"
#include "IGraphicsBuffer.h"

// ---------------------------------------------------------------------------
// IGraphicsContext — Rendering command recorder
// 
// Abstracts ID3D12GraphicsCommandList and VkCommandBuffer
// This is the primary interface Scene uses to record rendering commands.
// 
// Instead of:
//   cmd->SetGraphicsRootSignature(...)
//   cmd->SetPipelineState(...)
//   cmd->SetGraphicsRootConstantBufferView(...)
//   cmd->DrawInstanced(...)
//
// Scene uses:
//   context->SetPipeline(pipeline)
//   context->SetConstantBuffer(slot, buffer)
//   context->DrawInstanced(vertexCount, instanceCount, ...)
// ---------------------------------------------------------------------------
class IGraphicsContext
{
public:
    virtual ~IGraphicsContext() = default;

    // Pipeline state
    virtual void SetPipeline(const IPipelineState* pipeline) = 0;

    // Constant buffer binding
    virtual void SetConstantBuffer(uint32_t slot, const IGraphicsBuffer* buffer, uint64_t offset = 0) = 0;

    // Vertex buffer binding
    virtual void SetVertexBuffer(uint32_t slot, const IGraphicsBuffer* buffer, uint32_t stride, uint64_t offset = 0) = 0;

    // Index buffer binding
    virtual void SetIndexBuffer(const IGraphicsBuffer* buffer, uint32_t indexCount, uint64_t offset = 0) = 0;

    // Viewport and scissor
    struct Viewport
    {
        float x, y, width, height, minDepth, maxDepth;
    };
    struct ScissorRect
    {
        int32_t left, top, right, bottom;
    };
    virtual void SetViewport(const Viewport& vp) = 0;
    virtual void SetScissorRect(const ScissorRect& rect) = 0;

    // Rendering commands
    virtual void Clear(float r, float g, float b, float a, float depth = 1.0f) = 0;
    virtual void DrawInstanced(
        uint32_t vertexCountPerInstance,
        uint32_t instanceCount,
        uint32_t startVertexLocation = 0,
        uint32_t startInstanceLocation = 0) = 0;

    virtual void DrawIndexedInstanced(
        uint32_t indexCountPerInstance,
        uint32_t instanceCount,
        uint32_t startIndexLocation = 0,
        int32_t baseVertexLocation = 0,
        uint32_t startInstanceLocation = 0) = 0;

    // Resource barriers / state transitions
    // These are needed for render-to-texture workflows
    enum class ResourceState
    {
        Common,
        VertexAndConstantBuffer,
        IndexBuffer,
        RenderTarget,
        UnorderedAccess,
        DepthWrite,
        DepthRead,
        ShaderResource,
        StreamOut,
        IndirectArgument,
        CopyDest,
        CopySrc,
        Present,
    };
    virtual void TransitionResource(void* resource, ResourceState stateBefore, ResourceState stateAfter) = 0;

    // Get the underlying native handle for API-specific operations
    // E.g., ID3D12GraphicsCommandList* for D3D12, VkCommandBuffer for Vulkan
    virtual void* GetNativeHandle() const = 0;
};

// ---------------------------------------------------------------------------
// IGraphicsContextFactory — Creates graphics contexts
// 
// Obtained from the renderer to record commands for a frame
// ---------------------------------------------------------------------------
class IGraphicsContextFactory
{
public:
    virtual ~IGraphicsContextFactory() = default;
    virtual std::unique_ptr<IGraphicsContext> CreateContext() = 0;
};

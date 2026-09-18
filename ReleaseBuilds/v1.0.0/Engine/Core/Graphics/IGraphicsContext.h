#pragma once
#include <memory>
#include <cstdint>
#include <array>
#include <chrono>
#include "IPipelineState.h"
#include "IGraphicsBuffer.h"
#include "IGraphicsTexture.h"

namespace Engine::Graphics
{
enum class GpuTimingStage : uint32_t
{
    Terrain,
    Portal,
    Opaque,
    Transparent,
    Count
};

struct FrameTimingTelemetry
{
    std::array<double, static_cast<size_t>(GpuTimingStage::Count)>
        gpuMilliseconds{};
    double cpuSubmissionMilliseconds = 0.0;
    double cpuPresentationMilliseconds = 0.0;
    uint64_t gpuSampleId = 0;
    uint32_t gpuRegionCount = 0;
    bool gpuTimingsValid = false;
    bool flipModelSwapChain = false;
};

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
    // Dynamic stencil reference used to isolate independent portal apertures.
    // Pipelines still define compare/write operations and masks.
    virtual void SetStencilReference(uint32_t) {}

    // Constant buffer binding
    virtual void SetConstantBuffer(uint32_t slot, const IGraphicsBuffer* buffer, uint64_t offset = 0) = 0;
    virtual void SetStructuredBuffer(uint32_t slot, const IGraphicsBuffer* buffer) = 0;

    // Vertex buffer binding
    virtual void SetVertexBuffer(uint32_t slot, const IGraphicsBuffer* buffer, uint32_t stride, uint64_t offset = 0) = 0;

    // Index buffer binding
    virtual void SetIndexBuffer(const IGraphicsBuffer* buffer, uint32_t indexCount, uint64_t offset = 0) = 0;

    // Pixel-shader texture binding (t0, t1, ...). Backends without sampled
    // texture support may retain the default no-op.
    virtual void SetTexture(uint32_t, const IGraphicsTexture*) {}

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

    // Optional asynchronous visibility feedback. Unsupported backends retain
    // the safe default of drawing everything. Implementations must never wait
    // for the GPU when answering IsOccluded.
    // Returns true when the current view is stable enough to issue queries.
    virtual bool BeginOcclusionFrame(uint64_t, uint64_t) { return false; }
    virtual bool IsOccluded(uint64_t) { return false; }
    virtual void BeginOcclusionQuery(uint64_t) {}
    virtual void EndOcclusionQuery() {}

    // Asynchronous GPU timestamp regions. A stage may contain multiple
    // regions; backends aggregate completed regions without stalling.
    virtual void BeginGpuTiming(GpuTimingStage) {}
    virtual void EndGpuTiming(GpuTimingStage) {}

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
    // Set the command buffer/list to wrap for the upcoming CreateContext() call.
    // For DX12: ID3D12GraphicsCommandList*. For Vulkan: VkCommandBuffer.
    // The default is a no-op; override in each backend.
    virtual void SetCommandBuffer(void* /*cmd*/) {}
    // Called after the renderer's existing per-slot fence wait and before any
    // commands are recorded. Explicit APIs use this to consume completed
    // asynchronous query results and recycle that slot's query storage.
    virtual void PrepareFrame(uint32_t /*frameSlot*/) {}
    virtual void FinalizeFrame() {}
    virtual FrameTimingTelemetry GetFrameTimingTelemetry() const { return {}; }
    virtual bool UsesPersistentConstantBufferArena() const { return false; }
    virtual uint32_t GetConstantBufferArenaWrites() const { return 0; }
    virtual uint32_t GetConstantBufferDiscardMaps() const { return 0; }
    virtual std::unique_ptr<IGraphicsContext> CreateContext() = 0;
};
}

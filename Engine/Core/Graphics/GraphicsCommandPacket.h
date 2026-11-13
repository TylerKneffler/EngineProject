#pragma once

#include "Core/Graphics/IGraphicsContext.h"
#include <atomic>
#include <cstring>
#include <unordered_set>
#include <vector>
#include <stdexcept>

namespace Engine::Graphics
{
// Records value arguments and resource handles on the scene owner. Replay is
// the only operation that calls the backend graphics context.
class GraphicsCommandPacket final : public IGraphicsContext
{
public:
    // Reuse only after GPU completion and after the last external reader has
    // released the packet. Preserve command capacity, release resource owners.
    void Reset()
    {
        m_commandCount = 0;
        m_ownedBuffers.clear();
        m_sharedBuffers.clear();
        m_seenBuffers.clear();
        m_ownedTextures.clear();
        m_seenTextures.clear();
        m_phase = NativePhase::Other;
        m_depthRectangleClearSupported=false;
        m_replayOcclusionCulled.store(0, std::memory_order_relaxed);
    }
    void RetainBuffer(std::unique_ptr<IGraphicsBuffer> buffer)
    { if (buffer) m_ownedBuffers.push_back(std::move(buffer)); }
    void RetainBuffer(std::shared_ptr<IGraphicsBuffer> buffer)
    { if (buffer) m_sharedBuffers.push_back(std::move(buffer)); }
    uint32_t GetReplayOcclusionCulledCount() const
    { return m_replayOcclusionCulled.load(std::memory_order_acquire); }
    void Replay(IGraphicsContext& target) const
    {
        unsigned skippedDepthPasses = 0;
        bool skipOccludedDraw = false;
        uint32_t occlusionCulled = 0;
        for (size_t index = 0; index < m_commandCount; ++index)
        {
            const Command& command = m_commands[index];
            if (skippedDepthPasses)
            {
                if (command.op == Op::BeginDepthOnlyPass) ++skippedDepthPasses;
                else if (command.op == Op::EndDepthOnlyPass) --skippedDepthPasses;
                continue;
            }
            if (command.op == Op::OcclusionDecision)
            {
                skipOccludedDraw = target.IsOccluded(command.n[0]);
                if (skipOccludedDraw) ++occlusionCulled;
                continue;
            }
            if (skipOccludedDraw)
            {
                if (command.op == Op::EndOcclusionQuery)
                {
                    skipOccludedDraw = false;
                    continue;
                }
                if (command.op == Op::Draw ||
                    command.op == Op::DrawIndexed ||
                    command.op == Op::DrawIndirect ||
                    command.op == Op::BeginOcclusionQuery)
                    continue;
            }
            switch (command.op)
            {
            case Op::NativePhase: target.SetNativeTimingPhase(static_cast<NativePhase>(command.n[0])); break;
            case Op::Pipeline: target.SetPipeline(static_cast<const IPipelineState*>(command.pointer)); break;
            case Op::Stencil: target.SetStencilReference(static_cast<uint32_t>(command.n[0])); break;
            case Op::PrepareConstants: target.PrepareConstantBuffer(static_cast<const IGraphicsBuffer*>(command.pointer), command.n[0]); break;
            case Op::ConstantBuffer: target.SetConstantBuffer(static_cast<uint32_t>(command.n[0]), static_cast<const IGraphicsBuffer*>(command.pointer), command.n[1]); break;
            case Op::StructuredBuffer: target.SetStructuredBuffer(static_cast<uint32_t>(command.n[0]), static_cast<const IGraphicsBuffer*>(command.pointer)); break;
            case Op::ComputePipeline: target.SetComputePipeline(static_cast<const IPipelineState*>(command.pointer)); break;
            case Op::ComputeConstants: target.SetComputeConstants(command.bytes.data(), static_cast<uint32_t>(command.bytes.size())); break;
            case Op::ComputeRead: target.SetComputeReadBuffer(static_cast<const IGraphicsBuffer*>(command.pointer), static_cast<uint32_t>(command.n[0])); break;
            case Op::ComputeWrite: target.SetComputeWriteBuffer(static_cast<const IGraphicsBuffer*>(command.pointer)); break;
            case Op::Dispatch: target.DispatchCompute(static_cast<uint32_t>(command.n[0]), static_cast<uint32_t>(command.n[1]), static_cast<uint32_t>(command.n[2])); break;
            case Op::StorageBarrier: target.StorageBarrier(static_cast<const IGraphicsBuffer*>(command.pointer)); break;
            case Op::BeginCompute: target.BeginComputePass(); break;
            case Op::EndCompute: target.EndComputePass(); break;
            case Op::DrawIndirect: target.DrawInstancesIndirect(static_cast<const IGraphicsBuffer*>(command.pointer), command.n[0], command.n[1] != 0); break;
            case Op::CopyBuffer: target.CopyBuffer(static_cast<const IGraphicsBuffer*>(command.pointer), static_cast<IGraphicsBuffer*>(command.mutablePointer), command.n[0]); break;
            case Op::VertexBuffer: target.SetVertexBuffer(static_cast<uint32_t>(command.n[0]), static_cast<const IGraphicsBuffer*>(command.pointer), static_cast<uint32_t>(command.n[1]), command.n[2]); break;
            case Op::IndexBuffer: target.SetIndexBuffer(static_cast<const IGraphicsBuffer*>(command.pointer), static_cast<uint32_t>(command.n[0]), command.n[1]); break;
            case Op::Texture: target.SetTexture(static_cast<uint32_t>(command.n[0]), static_cast<const IGraphicsTexture*>(command.pointer)); break;
            case Op::BeginDepthOnlyPass:
                if (!target.BeginDepthOnlyPass(static_cast<const IGraphicsTexture*>(command.pointer), command.f[0]))
                    skippedDepthPasses = 1;
                break;
            case Op::EndDepthOnlyPass: target.EndDepthOnlyPass(); break;
            case Op::ClearDepthRectangle:
                if(!target.SupportsDepthRectangleClear())
                    throw std::logic_error("Recorded depth rectangle clear unsupported by replay target");
                target.ClearDepthRectangle(command.scissor,command.f[0]); break;
            case Op::Viewport: target.SetViewport(command.viewport); break;
            case Op::Scissor: target.SetScissorRect(command.scissor); break;
            case Op::Clear: target.Clear(command.f[0], command.f[1], command.f[2], command.f[3], command.f[4]); break;
            case Op::Draw: target.DrawInstanced(static_cast<uint32_t>(command.n[0]), static_cast<uint32_t>(command.n[1]), static_cast<uint32_t>(command.n[2]), static_cast<uint32_t>(command.n[3])); break;
            case Op::DrawIndexed: target.DrawIndexedInstanced(static_cast<uint32_t>(command.n[0]), static_cast<uint32_t>(command.n[1]), static_cast<uint32_t>(command.n[2]), command.signedValue, static_cast<uint32_t>(command.n[3])); break;
            case Op::BeginOcclusionQuery: target.BeginOcclusionQuery(command.n[0]); break;
            case Op::EndOcclusionQuery: target.EndOcclusionQuery(); break;
            case Op::BeginOcclusionFrame: target.BeginOcclusionFrame(command.n[0], command.n[1]); break;
            case Op::OcclusionDecision: break;
            case Op::BeginGpuTiming: target.BeginGpuTiming(static_cast<GpuTimingStage>(command.n[0])); break;
            case Op::EndGpuTiming: target.EndGpuTiming(static_cast<GpuTimingStage>(command.n[0])); break;
            case Op::Transition: target.TransitionResource(command.mutablePointer, static_cast<ResourceState>(command.n[0]), static_cast<ResourceState>(command.n[1])); break;
            }
        }
        m_replayOcclusionCulled.store(occlusionCulled,
            std::memory_order_release);
    }

    NativePhase GetNativeTimingPhase() const override { return m_phase; }
    void SetNativeTimingPhase(NativePhase phase) override
    { m_phase = phase; Add(Op::NativePhase).n[0] = static_cast<uint64_t>(phase); }
    void SetPipeline(const IPipelineState* pipeline) override { Add(Op::Pipeline).pointer = pipeline; }
    void SetStencilReference(uint32_t reference) override { Add(Op::Stencil).n[0] = reference; }
    void PrepareConstantBuffer(const IGraphicsBuffer* buffer, uint64_t count) override
    { TrackBuffer(buffer); auto& c = Add(Op::PrepareConstants); c.pointer = buffer; c.n[0] = count; }
    void SetConstantBuffer(uint32_t slot, const IGraphicsBuffer* buffer, uint64_t offset = 0) override
    { TrackBuffer(buffer); auto& c = Add(Op::ConstantBuffer); c.pointer = buffer; c.n[0] = slot; c.n[1] = offset; }
    void SetStructuredBuffer(uint32_t slot, const IGraphicsBuffer* buffer) override
    { TrackBuffer(buffer); auto& c = Add(Op::StructuredBuffer); c.pointer = buffer; c.n[0] = slot; }
    bool SetComputePipeline(const IPipelineState* pipeline) override
    { if (!pipeline) return false; Add(Op::ComputePipeline).pointer = pipeline; return true; }
    void SetComputeConstants(const void* data, uint32_t size) override
    {
        auto& c = Add(Op::ComputeConstants);
        if (data && size)
        {
            c.bytes.resize(size);
            std::memcpy(c.bytes.data(), data, size);
        }
        else c.bytes.clear();
    }
    void SetComputeReadBuffer(const IGraphicsBuffer* buffer, uint32_t slot = 0) override
    { TrackBuffer(buffer); auto& c = Add(Op::ComputeRead); c.pointer = buffer; c.n[0] = slot; }
    void SetComputeWriteBuffer(const IGraphicsBuffer* buffer) override
    { TrackBuffer(buffer); Add(Op::ComputeWrite).pointer = buffer; }
    void DispatchCompute(uint32_t x, uint32_t y = 1, uint32_t z = 1) override
    { auto& c = Add(Op::Dispatch); c.n[0] = x; c.n[1] = y; c.n[2] = z; }
    void StorageBarrier(const IGraphicsBuffer* buffer) override
    { TrackBuffer(buffer); Add(Op::StorageBarrier).pointer = buffer; }
    bool BeginComputePass() override { Add(Op::BeginCompute); return true; }
    void EndComputePass() override { Add(Op::EndCompute); }
    bool SupportsIndirectDraws() const override { return true; }
    bool DrawInstancesIndirect(const IGraphicsBuffer* buffer, uint64_t offset, bool indexed) override
    { if (!buffer) return false; TrackBuffer(buffer); auto& c = Add(Op::DrawIndirect); c.pointer = buffer; c.n[0] = offset; c.n[1] = indexed; return true; }
    bool CopyBuffer(const IGraphicsBuffer* source, IGraphicsBuffer* destination, uint64_t size = 0) override
    { if (!source || !destination) return false; TrackBuffer(source); TrackBuffer(destination); auto& c = Add(Op::CopyBuffer); c.pointer = source; c.mutablePointer = destination; c.n[0] = size; return true; }
    void SetVertexBuffer(uint32_t slot, const IGraphicsBuffer* buffer, uint32_t stride, uint64_t offset = 0) override
    { TrackBuffer(buffer); auto& c = Add(Op::VertexBuffer); c.pointer = buffer; c.n[0] = slot; c.n[1] = stride; c.n[2] = offset; }
    void SetIndexBuffer(const IGraphicsBuffer* buffer, uint32_t count, uint64_t offset = 0) override
    { TrackBuffer(buffer); auto& c = Add(Op::IndexBuffer); c.pointer = buffer; c.n[0] = count; c.n[1] = offset; }
    void SetTexture(uint32_t slot, const IGraphicsTexture* texture) override
    {
        if (texture && m_seenTextures.insert(texture).second)
            if (auto owner = texture->weak_from_this().lock())
                m_ownedTextures.push_back(std::move(owner));
        auto& c = Add(Op::Texture); c.pointer = texture; c.n[0] = slot;
    }
    bool BeginDepthOnlyPass(const IGraphicsTexture* texture, float depth = 1.f) override
    {
        if (!texture) return false;
        if (m_seenTextures.insert(texture).second)
            if (auto owner = texture->weak_from_this().lock())
                m_ownedTextures.push_back(std::move(owner));
        auto& c = Add(Op::BeginDepthOnlyPass); c.pointer = texture;
        c.f[0] = depth; return true;
    }
    void EndDepthOnlyPass() override { Add(Op::EndDepthOnlyPass); }
    void SetDepthRectangleClearSupported(bool supported) { m_depthRectangleClearSupported=supported; }
    bool SupportsDepthRectangleClear() const override { return m_depthRectangleClearSupported; }
    void ClearDepthRectangle(const ScissorRect& rect,float depth=1.f) override
    { auto& c=Add(Op::ClearDepthRectangle); c.scissor=rect; c.f[0]=depth; }
    void SetViewport(const Viewport& viewport) override { Add(Op::Viewport).viewport = viewport; }
    void SetScissorRect(const ScissorRect& scissor) override { Add(Op::Scissor).scissor = scissor; }
    void Clear(float r, float g, float b, float a, float depth = 1.f) override
    { auto& c = Add(Op::Clear); c.f[0] = r; c.f[1] = g; c.f[2] = b; c.f[3] = a; c.f[4] = depth; }
    void DrawInstanced(uint32_t vertices, uint32_t instances, uint32_t firstVertex = 0, uint32_t firstInstance = 0) override
    { auto& c = Add(Op::Draw); c.n[0] = vertices; c.n[1] = instances; c.n[2] = firstVertex; c.n[3] = firstInstance; }
    void DrawIndexedInstanced(uint32_t indices, uint32_t instances, uint32_t firstIndex = 0, int32_t baseVertex = 0, uint32_t firstInstance = 0) override
    { auto& c = Add(Op::DrawIndexed); c.n[0] = indices; c.n[1] = instances; c.n[2] = firstIndex; c.signedValue = baseVertex; c.n[3] = firstInstance; }
    bool BeginOcclusionFrame(uint64_t viewId, uint64_t signature) override
    { auto& c = Add(Op::BeginOcclusionFrame); c.n[0] = viewId; c.n[1] = signature; return true; }
    bool IsOccluded(uint64_t objectId) override
    { Add(Op::OcclusionDecision).n[0] = objectId; return false; }
    void BeginOcclusionQuery(uint64_t id) override { Add(Op::BeginOcclusionQuery).n[0] = id; }
    void EndOcclusionQuery() override { Add(Op::EndOcclusionQuery); }
    void BeginGpuTiming(GpuTimingStage stage) override { Add(Op::BeginGpuTiming).n[0] = static_cast<uint64_t>(stage); }
    void EndGpuTiming(GpuTimingStage stage) override { Add(Op::EndGpuTiming).n[0] = static_cast<uint64_t>(stage); }
    void TransitionResource(void* resource, ResourceState before, ResourceState after) override
    { auto& c = Add(Op::Transition); c.mutablePointer = resource; c.n[0] = static_cast<uint64_t>(before); c.n[1] = static_cast<uint64_t>(after); }
    void* GetNativeHandle() const override { return nullptr; }

private:
    enum class Op : uint8_t
    {
        NativePhase, Pipeline, Stencil, PrepareConstants, ConstantBuffer,
        StructuredBuffer, ComputePipeline, ComputeConstants, ComputeRead,
        ComputeWrite, Dispatch, StorageBarrier, BeginCompute, EndCompute,
        DrawIndirect, CopyBuffer, VertexBuffer, IndexBuffer, Texture,
        BeginDepthOnlyPass, EndDepthOnlyPass, ClearDepthRectangle, Viewport, Scissor, Clear,
        Draw, DrawIndexed, BeginOcclusionFrame, OcclusionDecision,
        BeginOcclusionQuery, EndOcclusionQuery,
        BeginGpuTiming, EndGpuTiming, Transition
    };
    struct Command
    {
        Op op{};
        const void* pointer = nullptr;
        void* mutablePointer = nullptr;
        uint64_t n[4]{};
        int32_t signedValue = 0;
        float f[5]{};
        Viewport viewport{};
        ScissorRect scissor{};
        std::vector<uint8_t> bytes;
    };
    Command& Add(Op op)
    {
        if (m_commandCount == m_commands.size()) m_commands.emplace_back();
        auto& command = m_commands[m_commandCount++];
        // Every operation setter supplies all fields consumed by its replay
        // case. Reuse the record and its byte-vector allocation after Reset.
        command.op = op;
        return command;
    }
    void TrackBuffer(const IGraphicsBuffer* buffer)
    {
        if (buffer && m_seenBuffers.insert(buffer).second)
            if (auto owner = buffer->weak_from_this().lock())
                m_sharedBuffers.push_back(std::move(owner));
    }
    std::vector<Command> m_commands;
    size_t m_commandCount = 0;
    std::vector<std::unique_ptr<IGraphicsBuffer>> m_ownedBuffers;
    std::vector<std::shared_ptr<const IGraphicsBuffer>> m_sharedBuffers;
    std::unordered_set<const IGraphicsBuffer*> m_seenBuffers;
    std::vector<std::shared_ptr<const IGraphicsTexture>> m_ownedTextures;
    std::unordered_set<const IGraphicsTexture*> m_seenTextures;
    NativePhase m_phase = NativePhase::Other;
    bool m_depthRectangleClearSupported=false;
    mutable std::atomic<uint32_t> m_replayOcclusionCulled{0};
};

// Single producer. Submitted frames must retain packets until GPU completion;
// reader ownership then prevents recycling while replay or the GPU still uses
// their resources. Each pass can keep a pool to preserve its command layout.
class GraphicsCommandPacketPool
{
public:
    std::shared_ptr<GraphicsCommandPacket> Acquire()
    {
        for (auto& packet : m_packets)
            if (packet.use_count() == 1)
            {
                packet->Reset();
                return packet;
            }
        auto packet = std::make_shared<GraphicsCommandPacket>();
        if (m_packets.size() < 8) m_packets.push_back(packet);
        return packet;
    }
private:
    std::vector<std::shared_ptr<GraphicsCommandPacket>> m_packets;
};
}

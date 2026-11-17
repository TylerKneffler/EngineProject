#pragma once

#include "Core/Graphics/IGraphicsBuffer.h"
#include "Core/Graphics/IPipelineState.h"
#include <glm/glm.hpp>
#include <cstdint>
#include <limits>
#include <memory>
#include <vector>

namespace Engine::Graphics
{
class IGraphicsContext;
class IGraphicsProvider;
}

namespace Engine::Components
{
// GPU neighbor search over the dense ParticleGpu buffer. Results are a bitset
// per query; source indices are recovered in ascending order from set bits.
// This correctness-first path records a grid build and query batch together.
class GpuParticleNeighborGrid
{
public:
    struct Query
    {
        glm::vec3 position{0.f};
        float radius = 0.f;
        uint32_t excludeIndex = std::numeric_limits<uint32_t>::max();
    };

    explicit GpuParticleNeighborGrid(Engine::Graphics::IGraphicsProvider* provider)
        : m_provider(provider) {}

    // particleBuffer uses the ParticleGpu layout: count header at slot zero,
    // followed by up to capacity dense 64-byte particle records.
    bool Record(Engine::Graphics::IGraphicsContext* context,
        const Engine::Graphics::IGraphicsBuffer* particleBuffer,
        uint32_t capacity, float smoothingRadius,
        const std::vector<Query>& queries);
    const Engine::Graphics::IGraphicsBuffer* GetResultsBuffer() const
    { return m_results.get(); }
    uint32_t GetWordsPerQuery() const { return m_wordsPerQuery; }
    uint32_t GetQueryCount() const { return m_queryCount; }

    // Call after recording, then submit and wait for the frame fence before
    // ResolveReadback on DX12/Vulkan.
    bool RequestReadback(Engine::Graphics::IGraphicsContext* context);
    bool ResolveReadback(std::vector<std::vector<uint32_t>>& neighbors);
    // Reclaims buffers from earlier Record calls after the submitting frame
    // fence has completed. ResolveReadback also performs this reclamation.
    void ReleaseRetiredAfterFence() { m_retired.clear(); }

private:
    bool EnsurePipelines();
    Engine::Graphics::IGraphicsProvider* m_provider = nullptr;
    std::unique_ptr<Engine::Graphics::IPipelineState> m_clearPipeline;
    std::unique_ptr<Engine::Graphics::IPipelineState> m_buildPipeline;
    std::unique_ptr<Engine::Graphics::IPipelineState> m_queryPipeline;
    std::unique_ptr<Engine::Graphics::IGraphicsBuffer> m_grid;
    std::unique_ptr<Engine::Graphics::IGraphicsBuffer> m_results;
    std::unique_ptr<Engine::Graphics::IGraphicsBuffer> m_readback;
    std::vector<std::unique_ptr<Engine::Graphics::IGraphicsBuffer>> m_retired;
    uint32_t m_queryCount = 0;
    uint32_t m_wordsPerQuery = 0;
    uint32_t m_capacity = 0;
    bool m_readbackPending = false;
};
}

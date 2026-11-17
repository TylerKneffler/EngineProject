#include "GpuParticleNeighborGrid.h"
#include "Core/Graphics/IGraphicsContext.h"
#include "Core/Graphics/IGraphicsProvider.h"
#include <algorithm>
#include <cmath>
#include <filesystem>

#ifndef ENGINE_SHADERS_PATH
#define ENGINE_SHADERS_PATH "Engine/Core/Shaders/"
#endif

namespace Engine::Components
{
namespace
{
struct alignas(16) GridRecord
{
    int32_t cellNext[4]{};
    float positionRadius[4]{};
    uint32_t metadata[4]{};
};
static_assert(sizeof(GridRecord) == 48);

struct alignas(16) Params
{
    uint32_t u0[4]{};
    float c1[4]{};
};
static_assert(sizeof(Params) == 32);

std::string ShaderPath(const char* name)
{
    const auto bundled = std::filesystem::path("Engine/Shaders/ParticleGpu") / name;
    if (std::filesystem::is_regular_file(bundled)) return bundled.string();
    return (std::filesystem::path(ENGINE_SHADERS_PATH) /
        "ParticleGpu" / name).string();
}

uint32_t BucketCount(uint32_t capacity)
{
    uint32_t buckets = 64;
    while (buckets < capacity * 2u) buckets <<= 1u;
    return buckets;
}
}

bool GpuParticleNeighborGrid::EnsurePipelines()
{
    if (m_clearPipeline && m_buildPipeline && m_queryPipeline) return true;
    if (!m_provider || !m_provider->GetShaderCompiler() ||
        !m_provider->GetPipelineStateFactory()) return false;
    auto* compiler = m_provider->GetShaderCompiler();
    auto* factory = m_provider->GetPipelineStateFactory();
    const auto compile = [&](const std::string& path, const char* entry)
    {
        auto shader = compiler->CompileFromFile(path, entry,
            Engine::Graphics::IShaderCompiler::CompileProfile::CS_5_0);
        return shader ? factory->CreateComputePipeline(shader.get()) : nullptr;
    };
    const auto buildPath = ShaderPath("ParticleNeighborBuild.hlsl");
    m_clearPipeline = compile(buildPath, "CSClear");
    m_buildPipeline = compile(buildPath, "CSBuild");
    m_queryPipeline = compile(ShaderPath("ParticleNeighborQuery.hlsl"),
        "CSQuery");
    return m_clearPipeline && m_buildPipeline && m_queryPipeline;
}

bool GpuParticleNeighborGrid::Record(
    Engine::Graphics::IGraphicsContext* context,
    const Engine::Graphics::IGraphicsBuffer* particleBuffer,
    uint32_t capacity, float smoothingRadius,
    const std::vector<Query>& queries)
{
    if (!context || !particleBuffer || !m_provider ||
        !m_provider->GetBufferFactory() || !EnsurePipelines() ||
        capacity > 100000u || queries.empty() ||
        particleBuffer->GetSize() < (static_cast<size_t>(capacity) + 1u) * 64u ||
        !std::isfinite(smoothingRadius) || smoothingRadius <= 0.f ||
        queries.size() > 65535u || m_readbackPending)
        return false;
    const uint32_t bucketCount = BucketCount(capacity);
    const uint32_t words = std::max(1u, (capacity + 31u) / 32u);
    const uint64_t resultBytes = static_cast<uint64_t>(queries.size()) *
        words * sizeof(uint32_t);
    if (resultBytes > 64u * 1024u * 1024u) return false;

    std::vector<GridRecord> initial(static_cast<size_t>(bucketCount) +
        capacity + queries.size());
    for (size_t i = 0; i < queries.size(); ++i)
    {
        auto& record = initial[static_cast<size_t>(bucketCount) + capacity + i];
        record.positionRadius[0] = queries[i].position.x;
        record.positionRadius[1] = queries[i].position.y;
        record.positionRadius[2] = queries[i].position.z;
        record.positionRadius[3] = queries[i].radius;
        record.metadata[1] = queries[i].excludeIndex;
    }
    auto* buffers = m_provider->GetBufferFactory();
    std::unique_ptr<Engine::Graphics::IGraphicsBuffer> grid;
    std::unique_ptr<Engine::Graphics::IGraphicsBuffer> results;
    try
    {
        grid = buffers->CreateBuffer(
            Engine::Graphics::IGraphicsBuffer::Usage::StorageBuffer,
            Engine::Graphics::IGraphicsBuffer::AccessMode::Default,
            initial.size() * sizeof(GridRecord), initial.data(),
            sizeof(GridRecord));
        results = buffers->CreateBuffer(
            Engine::Graphics::IGraphicsBuffer::Usage::StorageBuffer,
            Engine::Graphics::IGraphicsBuffer::AccessMode::Default,
            resultBytes, nullptr, sizeof(uint32_t));
    }
    catch (...) { return false; }
    if (!grid || !results) return false;

    Params params{};
    params.u0[0] = capacity;
    params.u0[1] = bucketCount;
    params.u0[2] = static_cast<uint32_t>(queries.size());
    params.u0[3] = words;
    params.c1[0] = smoothingRadius;
    if (!context->SetComputePipeline(m_clearPipeline.get())) return false;
    context->SetComputeConstants(&params, sizeof(params));
    context->SetComputeWriteBuffer(grid.get());
    context->DispatchCompute((std::max(bucketCount, capacity) + 63u) / 64u);
    context->StorageBarrier(grid.get());
    if (capacity)
    {
        if (!context->SetComputePipeline(m_buildPipeline.get())) return false;
        context->SetComputeConstants(&params, sizeof(params));
        context->SetComputeReadBuffer(particleBuffer);
        context->SetComputeWriteBuffer(grid.get());
        context->DispatchCompute((capacity + 63u) / 64u);
        context->StorageBarrier(grid.get());
    }
    if (!context->SetComputePipeline(m_queryPipeline.get())) return false;
    context->SetComputeConstants(&params, sizeof(params));
    context->SetComputeReadBuffer(grid.get());
    context->SetComputeWriteBuffer(results.get());
    context->DispatchCompute((params.u0[2] + 63u) / 64u);
    context->StorageBarrier(results.get());

    if (m_grid) m_retired.push_back(std::move(m_grid));
    if (m_results) m_retired.push_back(std::move(m_results));
    if (m_readback) m_retired.push_back(std::move(m_readback));
    m_grid = std::move(grid);
    m_results = std::move(results);
    m_queryCount = params.u0[2];
    m_wordsPerQuery = words;
    m_capacity = capacity;
    return true;
}

bool GpuParticleNeighborGrid::RequestReadback(
    Engine::Graphics::IGraphicsContext* context)
{
    if (!context || !m_results || m_readbackPending || !m_provider ||
        !m_provider->GetBufferFactory()) return false;
    try
    {
        m_readback = m_provider->GetBufferFactory()->CreateBuffer(
            Engine::Graphics::IGraphicsBuffer::Usage::StorageBuffer,
            Engine::Graphics::IGraphicsBuffer::AccessMode::Readback,
            m_results->GetSize(), nullptr, sizeof(uint32_t));
    }
    catch (...) { return false; }
    if (!m_readback || !context->CopyBuffer(m_results.get(), m_readback.get()))
        return false;
    m_readbackPending = true;
    return true;
}

bool GpuParticleNeighborGrid::ResolveReadback(
    std::vector<std::vector<uint32_t>>& neighbors)
{
    if (!m_readbackPending || !m_readback) return false;
    const auto* bits = static_cast<const uint32_t*>(m_readback->Map());
    if (!bits) return false;
    std::vector<std::vector<uint32_t>> resolved(m_queryCount);
    for (uint32_t q = 0; q < m_queryCount; ++q)
        for (uint32_t source = 0; source < m_capacity; ++source)
            if (bits[static_cast<size_t>(q) * m_wordsPerQuery + source / 32u] &
                (1u << (source & 31u)))
                resolved[q].push_back(source);
    m_readback->Unmap();
    m_readbackPending = false;
    neighbors = std::move(resolved);
    m_readback.reset();
    ReleaseRetiredAfterFence();
    return true;
}
}

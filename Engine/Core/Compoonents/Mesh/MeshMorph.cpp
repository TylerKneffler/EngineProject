#include "Mesh.h"
#include "Core/Rendering/SharedMeshBuffers.h"
#include "Core/Threading/JobScheduler.h"
#include "Core/Object.h"
#include "Core/Scene/Scene.h"
#include "Core/Model/MeshPackAsset.h"
#include "Core/Graphics/IGraphicsProvider.h"
#include "Engine/Editor/UI/IEditorUi.h"
#include <fstream>
#include <algorithm>
#include <sstream>
#include <array>
#include <filesystem>
#include <stdexcept>
#include <cstring>
#include <cstdio>
#include <cmath>
#include <chrono>
#include <limits>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <glm/gtc/quaternion.hpp>
#ifdef _WIN32
#define NOMINMAX
#include <Windows.h>
#endif

namespace Engine::Components
{
void Mesh::SetMorphData(unsigned nodeIndex, std::vector<MorphTarget> targets,
    std::vector<float> weights)
{
    ClearRuntimeLods();
    m_morphFilePath.clear();
    m_morphNodeIndex = nodeIndex;
    m_preparedMorphDeltas.clear();
    m_morphTargets = std::move(targets);
    m_morphWeights = std::move(weights);
    m_morphWeights.resize(m_morphTargets.size(), 0.f);
    m_observedMorphWeights = m_morphWeights;
    m_morphWeightsObserved = true;
    AdvanceMorphWeightsRevision();
    UpdateBounds();
    CreateMorphBuffers();
    MarkConfigurationDirty();
}

bool Mesh::SaveMorphFile(const std::string& path,
    const std::vector<MorphTarget>& targets)
{
    static_assert(sizeof(glm::vec3) == sizeof(float) * 3);
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) return false;
    const uint32_t magic = 0x46524f4du; // MORF
    const uint32_t version = 3;
    const uint32_t count = static_cast<uint32_t>(targets.size());
    file.write(reinterpret_cast<const char*>(&magic), sizeof(magic));
    file.write(reinterpret_cast<const char*>(&version), sizeof(version));
    file.write(reinterpret_cast<const char*>(&count), sizeof(count));
    for (const auto& target : targets)
    {
        const uint32_t nameSize = static_cast<uint32_t>(target.name.size());
        file.write(reinterpret_cast<const char*>(&nameSize), sizeof(nameSize));
        file.write(target.name.data(), nameSize);
        for (const auto* values : { &target.positions, &target.normals,
                &target.tangents })
        {
            const uint32_t size = static_cast<uint32_t>(values->size());
            file.write(reinterpret_cast<const char*>(&size), sizeof(size));
            file.write(reinterpret_cast<const char*>(values->data()),
                static_cast<std::streamsize>(size * sizeof(glm::vec3)));
        }
        const uint32_t uvSize = static_cast<uint32_t>(target.uv.size());
        file.write(reinterpret_cast<const char*>(&uvSize), sizeof(uvSize));
        file.write(reinterpret_cast<const char*>(target.uv.data()),
            static_cast<std::streamsize>(uvSize * sizeof(glm::vec4)));
        for (const auto& channel : target.uvChannels)
        {
            const uint32_t size = static_cast<uint32_t>(channel.size());
            file.write(reinterpret_cast<const char*>(&size), sizeof(size));
            file.write(reinterpret_cast<const char*>(channel.data()),
                static_cast<std::streamsize>(size * sizeof(glm::vec4)));
        }
    }
    return file.good();
}

bool Mesh::LoadMorphFile(const std::string& path, unsigned nodeIndex,
    std::vector<float> weights)
{
    static_assert(sizeof(glm::vec3) == sizeof(float) * 3);
    auto morphFile = Engine::Model::MeshPackAsset::Open(ResolveFilePath(path));
    if (!morphFile) return false;
    std::istream& file = *morphFile;
    uint32_t magic = 0, version = 0, count = 0;
    file.read(reinterpret_cast<char*>(&magic), sizeof(magic));
    file.read(reinterpret_cast<char*>(&version), sizeof(version));
    file.read(reinterpret_cast<char*>(&count), sizeof(count));
    if (!file || magic != 0x46524f4du || (version != 1 && version != 2 && version != 3) ||
        count > 10000u)
        return false;
    std::vector<MorphTarget> targets;
    targets.reserve(count);
    for (uint32_t index = 0; index < count; ++index)
    {
        MorphTarget target;
        uint32_t nameSize = 0;
        file.read(reinterpret_cast<char*>(&nameSize), sizeof(nameSize));
        if (!file || nameSize > 4096u) return false;
        target.name.resize(nameSize);
        file.read(target.name.data(), nameSize);
        for (auto* values : { &target.positions, &target.normals,
                &target.tangents })
        {
            uint32_t size = 0;
            file.read(reinterpret_cast<char*>(&size), sizeof(size));
            if (!file || size > 100000000u ||
                (!m_vertices.empty() && size != 0 && size != m_vertices.size()))
                return false;
            values->resize(size);
            file.read(reinterpret_cast<char*>(values->data()),
                static_cast<std::streamsize>(size * sizeof(glm::vec3)));
            if (!file) return false;
        }
        if (version >= 2)
        {
            uint32_t size = 0;
            file.read(reinterpret_cast<char*>(&size), sizeof(size));
            if (!file || size > 100000000u ||
                (!m_vertices.empty() && size != 0 && size != m_vertices.size()))
                return false;
            target.uv.resize(size);
            file.read(reinterpret_cast<char*>(target.uv.data()),
                static_cast<std::streamsize>(size * sizeof(glm::vec4)));
            if (!file) return false;
        }
        if (version >= 3)
            for (auto& channel : target.uvChannels)
            {
                uint32_t size = 0;
                file.read(reinterpret_cast<char*>(&size), sizeof(size));
                if (!file || size > 100000000u ||
                    (!m_vertices.empty() && size != 0 && size != m_vertices.size()))
                    return false;
                channel.resize(size);
                file.read(reinterpret_cast<char*>(channel.data()),
                    static_cast<std::streamsize>(size * sizeof(glm::vec4)));
                if (!file) return false;
            }
        targets.push_back(std::move(target));
    }
    SetMorphData(nodeIndex, std::move(targets), std::move(weights));
    m_morphFilePath = path;
    return true;
}

namespace
{
bool SameMorphWeights(const std::vector<float>& first,
    const std::vector<float>& second)
{
    return first.size() == second.size() &&
        (first.empty() || std::memcmp(first.data(), second.data(),
            first.size() * sizeof(float)) == 0);
}
}

void Mesh::AdvanceMorphWeightsRevision() const
{
    if (++m_morphWeightsRevision == 0)
        ++m_morphWeightsRevision;
}

void Mesh::ObserveMorphWeights() const
{
    if (m_morphWeightsObserved &&
        SameMorphWeights(m_morphWeights, m_observedMorphWeights))
        return;
    if (m_morphWeightsObserved)
        AdvanceMorphWeightsRevision();
    m_observedMorphWeights = m_morphWeights;
    m_morphWeightsObserved = true;
}

bool Mesh::SetMorphWeights(const std::vector<float>& weights)
{
    ObserveMorphWeights();
    if (SameMorphWeights(m_morphWeights, weights))
        return false;
    m_morphWeights = weights;
    m_observedMorphWeights = m_morphWeights;
    m_morphWeightsObserved = true;
    AdvanceMorphWeightsRevision();
    UploadMorphWeights();
    MarkConfigurationDirty();
    return true;
}

void Mesh::SyncMorphWeights()
{
    const uint64_t revision = m_morphWeightsRevision;
    ObserveMorphWeights();
    if (revision != m_morphWeightsRevision)
    {
        UploadMorphWeights();
        MarkConfigurationDirty();
    }
}

uint64_t Mesh::GetMorphWeightsRevision() const
{
    ObserveMorphWeights();
    return m_morphWeightsRevision;
}

void Mesh::PrepareDeferredGraphicsData()
{
    if (Owner || m_bufferFactory)
        throw std::logic_error("Deferred mesh preparation requires a detached CPU mesh");
    if (!m_vertices.empty() && !m_morphTargets.empty())
        m_preparedMorphDeltas = BuildMorphDeltas();
}

std::vector<glm::vec4> Mesh::BuildMorphDeltas()
{
    const size_t vertexCount = m_vertices.size();
    const bool hasFullUv = std::any_of(m_morphTargets.begin(),
        m_morphTargets.end(), [](const MorphTarget& target)
        {
            for (size_t channel = 0; channel < target.uvChannels.size(); ++channel)
            {
                const auto& values = target.uvChannels[channel];
                if (channel >= 2 && !values.empty()) return true;
                if (std::any_of(values.begin(), values.end(), [](const glm::vec4& value)
                    { return value.z != 0.f || value.w != 0.f; })) return true;
            }
            return false;
        });
    const bool hasUv = std::any_of(m_morphTargets.begin(),
        m_morphTargets.end(), [](const MorphTarget& target)
        { return !target.uv.empty(); });
    const bool hasNormalOrTangent = std::any_of(m_morphTargets.begin(),
        m_morphTargets.end(), [](const MorphTarget& target)
        { return !target.normals.empty() || !target.tangents.empty(); });
    m_morphDeltaStrideFloat4 = hasFullUv ? 9u : hasUv ? 4u :
        hasNormalOrTangent ? 3u : 1u;
    std::vector<glm::vec4> deltas(vertexCount * m_morphTargets.size() *
        m_morphDeltaStrideFloat4, glm::vec4(0.f));
    for (size_t targetIndex = 0; targetIndex < m_morphTargets.size(); ++targetIndex)
    {
        const MorphTarget& target = m_morphTargets[targetIndex];
        for (size_t vertexIndex = 0; vertexIndex < vertexCount; ++vertexIndex)
        {
            const size_t base = (targetIndex * vertexCount + vertexIndex) *
                m_morphDeltaStrideFloat4;
            if (vertexIndex < target.positions.size())
                deltas[base] = glm::vec4(
                    target.positions[vertexIndex], 0.f);
            if (m_morphDeltaStrideFloat4 >= 3 &&
                vertexIndex < target.normals.size())
                deltas[base + 1] = glm::vec4(
                    target.normals[vertexIndex], 0.f);
            if (m_morphDeltaStrideFloat4 >= 3 &&
                vertexIndex < target.tangents.size())
                deltas[base + 2] = glm::vec4(
                    target.tangents[vertexIndex], 0.f);
            if (m_morphDeltaStrideFloat4 >= 4 && vertexIndex < target.uv.size())
                deltas[base + 3] = target.uv[vertexIndex];
            if (hasFullUv)
                for (size_t channel = 0; channel < target.uvChannels.size(); ++channel)
                    if (vertexIndex < target.uvChannels[channel].size())
                        deltas[base + 4 + channel] = target.uvChannels[channel][vertexIndex];
        }
    }
    return deltas;
}

void Mesh::CreateMorphBuffers()
{
    ReplaceMorphDeltaBuffer({});
    ReplaceMorphWeightBuffer({});
    m_packedMorphWeights.clear();
    m_activeMorphTargetCount = 0;
    if (!m_bufferFactory || m_vertices.empty() || m_morphTargets.empty())
        return;

    auto deltas = m_preparedMorphDeltas.empty()
        ? BuildMorphDeltas() : std::move(m_preparedMorphDeltas);
    m_preparedMorphDeltas.clear();
    ReplaceMorphDeltaBuffer(m_bufferFactory->CreateStaticShaderResource(
        static_cast<uint64_t>(deltas.size()) * sizeof(glm::vec4),
        deltas.data(), sizeof(glm::vec4)));

    // Each float4 stores the authored target index and its current weight.
    // The vertex shaders can then skip inactive targets entirely.
    m_packedMorphWeights.assign(m_morphTargets.size(), glm::vec4(0.f));
    ReplaceMorphWeightBuffer(m_bufferFactory->CreateBuffer(
        IGraphicsBuffer::Usage::ShaderResource,
        IGraphicsBuffer::AccessMode::Upload,
        static_cast<uint64_t>(m_packedMorphWeights.size()) * sizeof(glm::vec4),
        m_packedMorphWeights.data(), sizeof(glm::vec4)));
    UploadMorphWeights();
}

void Mesh::UploadMorphWeights()
{
    if (!m_morphWeightBuffer)
        return;
    m_activeMorphTargetCount = 0;
    for (size_t index = 0; index < m_morphWeights.size() &&
        index < m_morphTargets.size(); ++index)
        if (m_morphWeights[index] != 0.f)
            m_packedMorphWeights[m_activeMorphTargetCount++] = glm::vec4(
                static_cast<float>(index), m_morphWeights[index], 0.f, 0.f);
    if (UsesDeferredGraphicsRetirement() && m_bufferFactory)
    {
        ReplaceMorphWeightBuffer(m_bufferFactory->CreateBuffer(
            IGraphicsBuffer::Usage::ShaderResource,
            IGraphicsBuffer::AccessMode::Upload,
            static_cast<uint64_t>(m_packedMorphWeights.size()) * sizeof(glm::vec4),
            m_packedMorphWeights.data(), sizeof(glm::vec4)));
        return;
    }
    if (void* mapped = m_morphWeightBuffer->Map())
    {
        const uint64_t byteCount = static_cast<uint64_t>(m_activeMorphTargetCount) *
            sizeof(glm::vec4);
        std::memcpy(mapped, m_packedMorphWeights.data(), static_cast<size_t>(byteCount));
        m_morphWeightBuffer->Unmap();
    }
}

} // namespace Engine::Components


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
std::vector<Mesh::Vertex> Mesh::BuildPosedVertices(
    const std::vector<glm::mat4>* skinPalette,
    bool includeSurfaceOffsets) const
{
    return BuildPosedVerticesFromData(m_vertices, m_indices, m_morphTargets,
        m_morphWeights, m_surfaceOffsets, skinPalette, includeSurfaceOffsets);
}

Mesh::PoseSnapshot Mesh::CapturePoseSnapshot() const
{
    return { m_vertices, m_indices, m_morphTargets, m_morphWeights,
        m_surfaceOffsets };
}

std::vector<Mesh::Vertex> Mesh::BuildPosedVertices(
    const PoseSnapshot& snapshot, const std::vector<glm::mat4>* skinPalette,
    bool includeSurfaceOffsets)
{
    return BuildPosedVerticesFromData(snapshot.vertices, snapshot.indices,
        snapshot.morphTargets, snapshot.morphWeights, snapshot.surfaceOffsets,
        skinPalette, includeSurfaceOffsets);
}

std::vector<Mesh::Vertex> Mesh::BuildPosedVerticesFromData(
    const std::vector<Vertex>& vertices, const std::vector<uint32_t>& indices,
    const std::vector<MorphTarget>& morphTargets,
    const std::vector<float>& morphWeights,
    const std::vector<glm::vec3>& surfaceOffsets,
    const std::vector<glm::mat4>* skinPalette, bool includeSurfaceOffsets)
{
    std::vector<Vertex> deformed = vertices;
    for (size_t targetIndex = 0; targetIndex < morphTargets.size() &&
        targetIndex < morphWeights.size(); ++targetIndex)
    {
        const float weight = morphWeights[targetIndex];
        const MorphTarget& target = morphTargets[targetIndex];
        if (std::abs(weight) <= 1e-7f) continue;
        Engine::Threading::JobScheduler::Get().For(deformed.size(), 1024, [&](size_t index)
        {
            Vertex& vertex = deformed[index];
            if (index < target.positions.size())
            {
                vertex.pos[0] += target.positions[index].x * weight;
                vertex.pos[1] += target.positions[index].y * weight;
                vertex.pos[2] += target.positions[index].z * weight;
            }
            if (index < target.normals.size())
            {
                vertex.normal[0] += target.normals[index].x * weight;
                vertex.normal[1] += target.normals[index].y * weight;
                vertex.normal[2] += target.normals[index].z * weight;
            }
            if (index < target.tangents.size())
            {
                vertex.tangent[0] += target.tangents[index].x * weight;
                vertex.tangent[1] += target.tangents[index].y * weight;
                vertex.tangent[2] += target.tangents[index].z * weight;
            }
            if (index < target.uv.size())
            {
                vertex.uv[0] += target.uv[index].x * weight;
                vertex.uv[1] += target.uv[index].y * weight;
                vertex.uv1[0] += target.uv[index].z * weight;
                vertex.uv1[1] += target.uv[index].w * weight;
            }
            for (size_t channel = 0; channel < target.uvChannels.size(); ++channel)
                if (index < target.uvChannels[channel].size())
                {
                    const glm::vec4 delta = target.uvChannels[channel][index] * weight;
                    for (size_t component = channel < 2 ? 2 : 0; component < 4; ++component)
                        vertex.uvChannels[channel][component] += delta[static_cast<glm::length_t>(component)];
                }
            vertex.uvChannels[0][0] = vertex.uv[0];
            vertex.uvChannels[0][1] = vertex.uv[1];
            vertex.uvChannels[1][0] = vertex.uv1[0];
            vertex.uvChannels[1][1] = vertex.uv1[1];
        });
    }
    if (skinPalette && !skinPalette->empty())
    {
        Engine::Threading::JobScheduler::Get().For(deformed.size(), 1024, [&](size_t index)
        {
            Vertex& vertex = deformed[index];
            const glm::vec3 sourcePosition(vertex.pos[0], vertex.pos[1], vertex.pos[2]);
            const glm::vec3 sourceNormal(vertex.normal[0], vertex.normal[1], vertex.normal[2]);
            const glm::vec3 sourceTangent(vertex.tangent[0], vertex.tangent[1], vertex.tangent[2]);
            const int deformMode = static_cast<int>(vertex.sdefCenterAndMode[3]);
            glm::vec3 specialPosition(0.f), specialNormal(0.f), specialTangent(0.f);
            bool special = false;
            if (deformMode == 3)
            {
                const auto joint0 = static_cast<uint32_t>(vertex.joints0[0]);
                const auto joint1 = static_cast<uint32_t>(vertex.joints0[1]);
                if (joint0 < skinPalette->size() && joint1 < skinPalette->size())
                {
                    const glm::mat4& bone0 = (*skinPalette)[joint0];
                    const glm::mat4& bone1 = (*skinPalette)[joint1];
                    const float weight = vertex.weights0[0];
                    const glm::vec3 center(vertex.sdefCenterAndMode[0],
                        vertex.sdefCenterAndMode[1], vertex.sdefCenterAndMode[2]);
                    const glm::vec3 radius0(vertex.sdefRadius0[0],
                        vertex.sdefRadius0[1], vertex.sdefRadius0[2]);
                    const glm::vec3 radius1(vertex.sdefRadius1[0],
                        vertex.sdefRadius1[1], vertex.sdefRadius1[2]);
                    glm::quat q0 = glm::normalize(glm::quat_cast(bone0));
                    glm::quat q1 = glm::normalize(glm::quat_cast(bone1));
                    if (glm::dot(q0, q1) < 0.f) q1 = -q1;
                    const glm::quat rotation = glm::normalize(glm::slerp(q1, q0, weight));
                    specialPosition = weight * glm::vec3(bone0 * glm::vec4(
                        sourcePosition - center + radius0, 1.f)) +
                        (1.f - weight) * glm::vec3(bone1 * glm::vec4(
                            sourcePosition - center + radius1, 1.f)) +
                        rotation * (center - weight * radius0 -
                            (1.f - weight) * radius1);
                    specialNormal = rotation * sourceNormal;
                    specialTangent = rotation * sourceTangent;
                    special = true;
                }
            }
            else if (deformMode == 4)
            {
                glm::quat real(0.f, 0.f, 0.f, 0.f);
                glm::quat dual(0.f, 0.f, 0.f, 0.f);
                glm::quat reference(1.f, 0.f, 0.f, 0.f);
                bool hasReference = false;
                for (int influence = 0; influence < 4; ++influence)
                {
                    const auto joint = static_cast<uint32_t>(vertex.joints0[influence]);
                    const float weight = vertex.weights0[influence];
                    if (joint >= skinPalette->size() || weight == 0.f) continue;
                    const glm::mat4& bone = (*skinPalette)[joint];
                    glm::quat rotation = glm::normalize(glm::quat_cast(bone));
                    if (!hasReference) { reference = rotation; hasReference = true; }
                    if (glm::dot(reference, rotation) < 0.f) rotation = -rotation;
                    const glm::vec3 translation(bone[3]);
                    const glm::quat displacement = .5f *
                        glm::quat(0.f, translation.x, translation.y, translation.z) * rotation;
                    real += weight * rotation;
                    dual += weight * displacement;
                }
                const float magnitude = glm::length(real);
                if (magnitude > 1e-7f)
                {
                    real /= magnitude;
                    dual /= magnitude;
                    const glm::quat displacement = dual * glm::conjugate(real);
                    specialPosition = real * sourcePosition + 2.f *
                        glm::vec3(displacement.x, displacement.y, displacement.z);
                    specialNormal = real * sourceNormal;
                    specialTangent = real * sourceTangent;
                    special = true;
                }
            }
            if (special)
            {
                const glm::vec3 normal = glm::length(specialNormal) > 1e-7f
                    ? glm::normalize(specialNormal) : specialNormal;
                const glm::vec3 tangent = glm::length(specialTangent) > 1e-7f
                    ? glm::normalize(specialTangent) : specialTangent;
                vertex.pos[0] = specialPosition.x; vertex.pos[1] = specialPosition.y;
                vertex.pos[2] = specialPosition.z;
                vertex.normal[0] = normal.x; vertex.normal[1] = normal.y;
                vertex.normal[2] = normal.z;
                vertex.tangent[0] = tangent.x; vertex.tangent[1] = tangent.y;
                vertex.tangent[2] = tangent.z;
                return;
            }
            glm::mat4 skin(0.f);
            float totalWeight = 0.f;
            for (int influence = 0; influence < 4; ++influence)
            {
                const uint32_t joint0 = static_cast<uint32_t>(vertex.joints0[influence]);
                const uint32_t joint1 = static_cast<uint32_t>(vertex.joints1[influence]);
                if (joint0 < skinPalette->size())
                {
                    skin += (*skinPalette)[joint0] * vertex.weights0[influence];
                    totalWeight += vertex.weights0[influence];
                }
                if (joint1 < skinPalette->size())
                {
                    skin += (*skinPalette)[joint1] * vertex.weights1[influence];
                    totalWeight += vertex.weights1[influence];
                }
            }
            if (totalWeight <= 1e-7f) return;
            const glm::vec4 position = skin * glm::vec4(
                vertex.pos[0], vertex.pos[1], vertex.pos[2], 1.f);
            glm::vec3 normal = glm::mat3(skin) * glm::vec3(
                vertex.normal[0], vertex.normal[1], vertex.normal[2]);
            glm::vec3 tangent = glm::mat3(skin) * glm::vec3(
                vertex.tangent[0], vertex.tangent[1], vertex.tangent[2]);
            if (glm::length(normal) > 1e-7f) normal = glm::normalize(normal);
            if (glm::length(tangent) > 1e-7f) tangent = glm::normalize(tangent);
            vertex.pos[0] = position.x; vertex.pos[1] = position.y;
            vertex.pos[2] = position.z;
            vertex.normal[0] = normal.x; vertex.normal[1] = normal.y;
            vertex.normal[2] = normal.z;
            vertex.tangent[0] = tangent.x; vertex.tangent[1] = tangent.y;
            vertex.tangent[2] = tangent.z;
        });
    }
    if (includeSurfaceOffsets && surfaceOffsets.size() == deformed.size())
    {
        for (size_t index = 0; index < deformed.size(); ++index)
        {
            const glm::vec3 position(deformed[index].pos[0],
                deformed[index].pos[1], deformed[index].pos[2]);
            const glm::vec3 moved = position + surfaceOffsets[index];
            deformed[index].pos[0] = moved.x;
            deformed[index].pos[1] = moved.y;
            deformed[index].pos[2] = moved.z;
        }
        std::vector<glm::vec3> accumulated(deformed.size(), glm::vec3(0.f));
        const size_t triangleCount = indices.empty() ? deformed.size() / 3
            : indices.size() / 3;
        for (size_t triangle = 0; triangle < triangleCount; ++triangle)
        {
            const size_t a = indices.empty() ? triangle * 3 : indices[triangle * 3];
            const size_t b = indices.empty() ? triangle * 3 + 1 : indices[triangle * 3 + 1];
            const size_t c = indices.empty() ? triangle * 3 + 2 : indices[triangle * 3 + 2];
            if (a >= deformed.size() || b >= deformed.size() || c >= deformed.size()) continue;
            const auto point = [&](size_t i) { return glm::vec3(
                deformed[i].pos[0], deformed[i].pos[1], deformed[i].pos[2]); };
            const glm::vec3 normal = glm::cross(point(b) - point(a), point(c) - point(a));
            accumulated[a] += normal;
            accumulated[b] += normal;
            accumulated[c] += normal;
        }
        for (size_t i = 0; i < deformed.size(); ++i)
            if (glm::dot(accumulated[i], accumulated[i]) > 1e-12f)
            {
                const glm::vec3 normal = glm::normalize(accumulated[i]);
                deformed[i].normal[0] = normal.x;
                deformed[i].normal[1] = normal.y;
                deformed[i].normal[2] = normal.z;
                glm::vec3 tangent(deformed[i].tangent[0], deformed[i].tangent[1],
                    deformed[i].tangent[2]);
                tangent -= normal * glm::dot(normal, tangent);
                if (glm::dot(tangent, tangent) > 1e-12f)
                {
                    tangent = glm::normalize(tangent);
                    deformed[i].tangent[0] = tangent.x;
                    deformed[i].tangent[1] = tangent.y;
                    deformed[i].tangent[2] = tangent.z;
                }
            }
    }
    return deformed;
}

std::vector<Mesh::Vertex> Mesh::BuildPortalCutTriangleStream(
    const std::vector<glm::mat4>* skinPalette) const
{
    std::vector<Vertex> deformed = BuildPosedVertices(skinPalette);
    if (m_indices.empty()) return deformed;
    std::vector<Vertex> triangles;
    triangles.reserve(m_indices.size());
    for (uint32_t index : m_indices)
        if (index < deformed.size()) triangles.push_back(deformed[index]);
    return triangles;
}

void Mesh::SetSurfaceOffsets(std::vector<glm::vec3> offsets,
    const std::vector<glm::mat4>* skinPalette)
{
    if (offsets.size() != m_vertices.size() || offsets.empty())
    {
        ClearSurfaceOffsets();
        return;
    }
    ClearRuntimeLods();
    m_surfaceOffsets = std::move(offsets);
    ++m_surfaceOffsetsRevision;
    if (!m_bufferFactory) return;
    std::vector<Vertex> posed = BuildPosedVertices(skinPalette);
    const uint64_t bytes = posed.size() * sizeof(Vertex);
    if (!m_posedVertexBuffer || m_posedVertexBuffer->GetSize() != bytes ||
        UsesDeferredGraphicsRetirement())
        ReplacePosedVertexBuffer(m_bufferFactory->CreateBuffer(
            IGraphicsBuffer::Usage::VertexBuffer,
            IGraphicsBuffer::AccessMode::Upload, bytes, posed.data()));
    else if (void* mapped = m_posedVertexBuffer->Map())
    {
        std::memcpy(mapped, posed.data(), static_cast<size_t>(bytes));
        m_posedVertexBuffer->Unmap();
        m_posedVertexBuffer->FlushMappedWrites();
    }
    else
        ReplacePosedVertexBuffer(m_bufferFactory->CreateBuffer(
            IGraphicsBuffer::Usage::VertexBuffer,
            IGraphicsBuffer::AccessMode::Upload, bytes, posed.data()));
    if (!m_posedVertexBuffer) m_surfaceOffsets.clear();
}

void Mesh::ClearSurfaceOffsets()
{
    if (!m_surfaceOffsets.empty()) ++m_surfaceOffsetsRevision;
    m_surfaceOffsets.clear();
    ReplacePosedVertexBuffer({});
}
} // namespace Engine::Components


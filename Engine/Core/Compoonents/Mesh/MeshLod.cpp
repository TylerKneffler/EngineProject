#include "Mesh.h"
#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <vector>

namespace Engine::Components
{
bool Mesh::StoreRuntimeLod(int lod, std::unique_ptr<Mesh> geometry)
{
    if (lod < 0 || lod >= MaximumRuntimeLods || !geometry || geometry.get() == this ||
        HasMorphTargets() || HasSurfaceOffsets() || geometry->HasMorphTargets() ||
        geometry->HasSurfaceOffsets() || !geometry->m_runtimeLods.empty()) return false;
    if (m_runtimeLods.empty() && GetVertexCount() > 0)
    {
        // Preserve an ordinary object's existing geometry as its base LOD.
        m_runtimeLods.push_back(std::make_unique<Mesh>());
        m_activeRuntimeLod = 0;
    }
    if (m_runtimeLods.size() <= static_cast<size_t>(lod)) m_runtimeLods.resize(lod + 1);
    if (lod == m_previousRuntimeLod || lod == m_activeRuntimeLod)
    { m_previousRuntimeLod = -1; m_runtimeLodProgress = 1.f; }
    if (m_activeRuntimeLod == lod)
    {
        if (!ExchangeStaticGeometry(*geometry)) return false;
        // geometry now owns the replaced buffers; release them, retaining a holder.
        m_runtimeLods[lod] = std::make_unique<Mesh>();
    }
    else m_runtimeLods[lod] = std::move(geometry);
    return true;
}

bool Mesh::IsRuntimeLodResident(int lod) const
{
    return lod >= 0 && static_cast<size_t>(lod) < m_runtimeLods.size() &&
        m_runtimeLods[lod] != nullptr;
}

bool Mesh::SelectRuntimeLod(int lod)
{
    if (lod == m_activeRuntimeLod || !IsRuntimeLodResident(lod) ||
        HasMorphTargets() || HasSurfaceOffsets()) return false;
    if (m_activeRuntimeLod >= 0 &&
        !ExchangeStaticGeometry(*m_runtimeLods[m_activeRuntimeLod])) return false;
    ExchangeStaticGeometry(*m_runtimeLods[lod]);
    m_activeRuntimeLod = lod;
    m_previousRuntimeLod = -1;
    m_runtimeLodProgress = 1.f;
    return true;
}

bool Mesh::BeginRuntimeLodTransition(int lod, float durationSeconds)
{
    // Complete the current pair before accepting another target. The caller
    // can retry its desired level next frame without allocations or thrashing.
    if (m_previousRuntimeLod >= 0) return false;
    const int previous = m_activeRuntimeLod;
    if (!SelectRuntimeLod(lod)) return false;
    if (previous >= 0 && durationSeconds > 0.f)
    {
        m_previousRuntimeLod = previous;
        m_runtimeLodProgress = 0.f;
        m_runtimeLodDuration = durationSeconds;
    }
    return true;
}

bool Mesh::AdvanceRuntimeLodTransition(float deltaSeconds)
{
    if (m_previousRuntimeLod < 0 || deltaSeconds <= 0.f) return false;
    m_runtimeLodProgress = std::min(1.f, m_runtimeLodProgress +
        deltaSeconds / std::max(.001f, m_runtimeLodDuration));
    if (m_runtimeLodProgress >= 1.f)
    { m_previousRuntimeLod = -1; ++m_runtimeFadeRevision; }
    return true;
}

Mesh* Mesh::GetPreviousRuntimeLodMesh() const
{
    return IsRuntimeLodResident(m_previousRuntimeLod)
        ? m_runtimeLods[m_previousRuntimeLod].get() : nullptr;
}

void Mesh::SetRuntimeVisibility(float visibility)
{
    const float next = std::clamp(visibility, 0.f, 1.f);
    if ((next <= 0.f) != (m_runtimeVisibility <= 0.f) ||
        (next >= 1.f) != (m_runtimeVisibility >= 1.f)) ++m_runtimeFadeRevision;
    m_runtimeVisibility = next;
}

glm::vec4 Mesh::GetRuntimeGeometryFade(bool previous) const
{
    const float split = m_previousRuntimeLod >= 0 ? m_runtimeLodProgress : 1.f;
    const float boundary = split * m_runtimeVisibility;
    // Complementary coverage intervals preserve opaque depth writes and avoid
    // transparent sorting. Ordinary, fully visible meshes use the fast path.
    if (previous) return {boundary, m_runtimeVisibility, 0.f, 1.f};
    return {0.f, boundary, 0.f, boundary < 1.f ? 1.f : 0.f};
}

int Mesh::ChooseRuntimeLod(float distance, std::initializer_list<float> thresholds,
    float hysteresis) const
{
    return ChooseRuntimeLod(distance, std::vector<float>(thresholds), hysteresis);
}

int Mesh::ChooseRuntimeLod(float distance, const std::vector<float>& thresholds,
    float hysteresis) const
{
    if (std::isnan(distance)) return std::max(0, m_activeRuntimeLod);
    hysteresis = std::max(0.f, hysteresis);
    int target = 0, level = 0;
    float lower = 0.f;
    float activeLower = 0.f, activeUpper = std::numeric_limits<float>::infinity();
    for (float threshold : thresholds)
    {
        if (level == MaximumRuntimeLods - 1) break;
        const float upper = std::max(lower, threshold);
        if (distance >= upper) target = level + 1;
        if (m_activeRuntimeLod == level) { activeLower = lower; activeUpper = upper; }
        lower = upper;
        ++level;
    }
    if (m_activeRuntimeLod == level) activeLower = lower;
    if (m_activeRuntimeLod >= 0 && m_activeRuntimeLod <= level &&
        (m_activeRuntimeLod == 0 || distance >= activeLower - hysteresis) &&
        distance < activeUpper + hysteresis) return m_activeRuntimeLod;
    return target;
}

int Mesh::ChooseScreenRuntimeLod(const glm::mat4& world,
    const Engine::Rendering::MeshLodView& view,
    const std::vector<float>& pixelThresholds, float hysteresis,
    std::optional<int> distanceOverride) const
{
    if (distanceOverride) return std::clamp(*distanceOverride, 0, MaximumRuntimeLods - 1);
    if (!HasBounds()) return 0;
    glm::vec3 minimum = m_boundsMin, maximum = m_boundsMax;
    float windStrength = foliageWindStrength;
    // Use the same wind-expanded union regardless of the selected geometry.
    // Streaming callers must retain an authored envelope for nonresident LODs.
    for (const auto& lod : m_runtimeLods)
        if (lod && lod->HasBounds())
        {
            minimum = glm::min(minimum, lod->m_boundsMin);
            maximum = glm::max(maximum, lod->m_boundsMax);
            windStrength = std::max(windStrength, lod->foliageWindStrength);
        }
    const glm::vec3 windPadding(1.5f * std::max(0.f, maximum.y) * std::clamp(windStrength, 0.f, 1.f));
    minimum -= windPadding; maximum += windPadding;
    return std::min(MaximumRuntimeLods - 1, Engine::Rendering::SelectScreenMeshLod(
        Engine::Rendering::ProjectedLodPixels(minimum, maximum, world, view),
        pixelThresholds, m_activeRuntimeLod, hysteresis));
}

size_t Mesh::GetResidentRuntimeLodCount() const
{
    return std::count_if(m_runtimeLods.begin(), m_runtimeLods.end(),
        [](const auto& geometry) { return geometry != nullptr; });
}

uint64_t Mesh::GetRuntimeLodGpuMemoryBytes() const
{
    if (IsInstanced())
    {
        std::unordered_set<const void*> resources;
        uint64_t bytes = 0;
        const auto count = [&](const Mesh& mesh) {
            if (auto* buffer = mesh.GetGraphicsBuffer(); buffer && resources.insert(buffer).second)
                bytes += buffer->GetSize();
            if (mesh.m_instances) for (const auto& slice : mesh.m_instances->slices)
                if (auto* buffer = slice.buffer.get(); buffer && resources.insert(buffer).second)
                    bytes += buffer->GetSize();
        };
        count(*this);
        for (const auto& geometry : m_runtimeLods) if (geometry) count(*geometry);
        return bytes;
    }
    uint64_t bytes = m_activeRuntimeLod >= 0 ? GetGpuBufferMemoryBytes() : 0;
    for (const auto& geometry : m_runtimeLods)
        if (geometry) bytes += geometry->GetGpuBufferMemoryBytes();
    return bytes;
}

void Mesh::ClearRuntimeLods()
{
    m_runtimeLods.clear();
    m_activeRuntimeLod = -1;
    m_previousRuntimeLod = -1;
    m_runtimeLodProgress = 1.f;
}


} // namespace Engine::Components

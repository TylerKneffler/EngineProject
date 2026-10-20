#include "ParticleNeighborGrid.h"
#include <algorithm>
#include <cmath>
#include <functional>

namespace Engine::Components
{
namespace
{
bool Finite(const glm::vec3& value)
{
    return std::isfinite(value.x) && std::isfinite(value.y) &&
        std::isfinite(value.z);
}

bool WithinRadius(const glm::vec3& a, const glm::vec3& b, double radiusSquared)
{
    const double dx = static_cast<double>(a.x) - b.x;
    const double dy = static_cast<double>(a.y) - b.y;
    const double dz = static_cast<double>(a.z) - b.z;
    return dx * dx + dy * dy + dz * dz <= radiusSquared;
}
}

size_t ParticleNeighborGrid::CellHash::operator()(const Cell& cell) const
{
    size_t hash = std::hash<int64_t>{}(cell.x);
    const auto combine = [&hash](int64_t value)
    {
        hash ^= std::hash<int64_t>{}(value) +
            static_cast<size_t>(0x9e3779b97f4a7c15ull) +
            (hash << 6) + (hash >> 2);
    };
    combine(cell.y);
    combine(cell.z);
    return hash;
}

ParticleNeighborGrid::Cell ParticleNeighborGrid::CellFor(
    double x, double y, double z) const
{
    const auto axis = [this](double value)
    {
        // Leave headroom for the bounded neighbor-cell loops at extreme input.
        constexpr double Limit = 1e15;
        return static_cast<int64_t>(std::clamp(
            std::floor(value / static_cast<double>(m_cellSize)), -Limit, Limit));
    };
    return { axis(x), axis(y), axis(z) };
}

void ParticleNeighborGrid::Clear()
{
    m_entries.clear();
    m_cells.clear();
}

bool ParticleNeighborGrid::Build(const ParticleState* particles,
    size_t count, float smoothingRadius)
{
    Clear();
    if (!std::isfinite(smoothingRadius) || smoothingRadius <= 0.f ||
        (count && !particles) || count > std::numeric_limits<uint32_t>::max())
        return false;
    m_cellSize = smoothingRadius;
    m_entries.reserve(count);
    for (size_t index = 0; index < count; ++index)
    {
        const ParticleState& particle = particles[index];
        if (!particle.active || !Finite(particle.position)) continue;
        const uint32_t entryIndex = static_cast<uint32_t>(m_entries.size());
        m_entries.push_back({ particle.position, static_cast<uint32_t>(index) });
        const Cell cell = CellFor(particle.position.x,
            particle.position.y, particle.position.z);
        m_cells[cell].push_back(entryIndex);
    }
    return true;
}

void ParticleNeighborGrid::QueryRadius(const glm::vec3& position,
    float radius, std::vector<uint32_t>& indices, uint32_t excludeIndex) const
{
    indices.clear();
    if (!Finite(position) || !std::isfinite(radius) || radius < 0.f)
        return;
    const double radiusSquared = static_cast<double>(radius) * radius;
    const auto include = [&](const Entry& entry)
    {
        if (entry.sourceIndex != excludeIndex &&
            WithinRadius(position, entry.position, radiusSquared))
            indices.push_back(entry.sourceIndex);
    };

    // Very large radii are cheaper to scan directly than as many empty cells.
    if (static_cast<double>(radius) > 2.0 * m_cellSize)
    {
        for (const Entry& entry : m_entries) include(entry);
    }
    else
    {
        const Cell low = CellFor(static_cast<double>(position.x) - radius,
            static_cast<double>(position.y) - radius,
            static_cast<double>(position.z) - radius);
        const Cell high = CellFor(static_cast<double>(position.x) + radius,
            static_cast<double>(position.y) + radius,
            static_cast<double>(position.z) + radius);
        for (int64_t z = low.z; z <= high.z; ++z)
            for (int64_t y = low.y; y <= high.y; ++y)
                for (int64_t x = low.x; x <= high.x; ++x)
            if (const auto found = m_cells.find({ x, y, z }); found != m_cells.end())
                for (const uint32_t entryIndex : found->second)
                    include(m_entries[entryIndex]);
    }
    std::sort(indices.begin(), indices.end());
}
}

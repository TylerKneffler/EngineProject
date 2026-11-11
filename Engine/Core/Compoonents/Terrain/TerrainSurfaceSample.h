#pragma once
#include <algorithm>
#include <cmath>
#include <vector>
#include <glm/glm.hpp>

namespace Engine::Components
{
// Match the two triangles used by Terrain::Rebuild, including their diagonal.
inline bool SampleTerrainSurface(const std::vector<float>& heights,
    int cellsX, int cellsY, float width, float depth, float x, float z,
    float& height, glm::vec3* normal = nullptr)
{
    if (cellsX <= 0 || cellsY <= 0 || width <= 0.f || depth <= 0.f ||
        heights.size() != static_cast<size_t>(cellsX + 1) * (cellsY + 1)) return false;
    const float sx = std::clamp(x / width, 0.f, static_cast<float>(cellsX));
    const float sz = std::clamp(z / depth, 0.f, static_cast<float>(cellsY));
    const int ix = std::min(static_cast<int>(sx), cellsX - 1);
    const int iz = std::min(static_cast<int>(sz), cellsY - 1);
    const float tx = sx - ix, tz = sz - iz;
    const auto at = [&](int col, int row) { return heights[static_cast<size_t>(row) * (cellsX + 1) + col]; };
    const float a = at(ix, iz), b = at(ix + 1, iz);
    const float c = at(ix, iz + 1), d = at(ix + 1, iz + 1);
    const float dx = tx + tz <= 1.f ? b - a : d - c;
    const float dz = tx + tz <= 1.f ? c - a : d - b;
    height = tx + tz <= 1.f ? a + dx * tx + dz * tz :
        d - dx * (1.f - tx) - dz * (1.f - tz);
    if (normal) *normal = glm::normalize(glm::vec3(-dx / width, 1.f, -dz / depth));
    return true;
}
}

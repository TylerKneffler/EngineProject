#include "TerrainManager.h"
#include "TerrainManagerFarTerrainCache.h"
#include "Core/Compoonents/Mesh/Mesh.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <optional>
#include <stdexcept>

namespace Engine::Components
{
using FarTerrainInternal::ReadFarTileCache;
using FarTerrainInternal::WriteFarTileCache;
void TerrainManager::StartFarPrepareWorker()
{
    if (m_farWorker.joinable()) return;
    m_farWorker = std::thread([this] { FarPrepareLoop(); });
}

void TerrainManager::FarPrepareLoop()
{
    for (;;)
    {
        FarPrepareTask task;
        {
            std::unique_lock<std::mutex> lock(m_farPrepareMutex);
            m_farPrepareWake.wait(lock, [this] {
                return m_stopFarWorker || !m_farPrepareQueue.empty();
            });
            if (m_stopFarWorker) return;
            task = std::move(m_farPrepareQueue.front());
            m_farPrepareQueue.pop_front();
        }
        PreparedFarTile result;
        result.tileIndex = task.tileIndex;
        result.revision = task.revision;
        const auto taskStart = std::chrono::steady_clock::now();
        try
        {
            result.cacheHit = !task.cachePath.empty() &&
                ReadFarTileCache(task.cachePath, task.fingerprint,
                    result.vertices, result.indices);
            if (result.cacheHit && task.lodStride == 1)
            {
                const size_t width = static_cast<size_t>(
                    task.endX - task.startX + 1);
                const size_t depth = static_cast<size_t>(
                    task.endY - task.startY + 1);
                size_t offset = width * depth +
                    4 * (width + depth - 2);
                const size_t patchCount = static_cast<size_t>(task.cellsX + 1) *
                    (task.cellsY + 1);
                for (const auto& [patchIndex, patch] : task.patches)
                    if (!task.hiddenCells.count({patch.x, patch.y}) &&
                        patch.heights.size() == patchCount)
                    {
                        result.patchVertexOffsets[patchIndex] = offset;
                        offset += patchCount;
                    }
                if (offset > result.vertices.size())
                    result.patchVertexOffsets.clear();
            }
            if (!result.cacheHit)
            {
                const int width = (task.endX - task.startX +
                    task.lodStride - 1) / task.lodStride + 1;
                const int depth = (task.endY - task.startY +
                    task.lodStride - 1) / task.lodStride + 1;
                const auto sample = [&](int x, int y) {
                    return (*task.heights)[static_cast<size_t>(
                        std::clamp(y + task.halfExtent, 0, task.side - 1)) *
                        task.side + std::clamp(x + task.halfExtent,
                            0, task.side - 1)];
                };
                const auto visibleCell = [&](int x, int y) {
                    return !task.hiddenCells.count({x, y});
                };
                const auto seamSide = [&](int x, int y) {
                    if (!visibleCell(x - 1, y)) return 0;
                    if (!visibleCell(x, y + 1)) return 1;
                    if (!visibleCell(x + 1, y)) return 2;
                    if (!visibleCell(x, y - 1)) return 3;
                    return -1;
                };
                const auto seamSurface = [&](int x, int y, int seam)
                    -> const FarPrepareTask::SeamSurface* {
                    const Cell neighbor = seam == 0 ? Cell{x - 1, y} :
                        seam == 1 ? Cell{x, y + 1} :
                        seam == 2 ? Cell{x + 1, y} : Cell{x, y - 1};
                    const auto found = task.seams.find(neighbor);
                    return found == task.seams.end() ? nullptr : &found->second;
                };
                const auto detailedNormal = [&](const std::vector<float>& heights,
                    int col, int row) {
                    const auto heightAt = [&](int x, int y) {
                        return heights[static_cast<size_t>(
                            std::clamp(y, 0, task.cellsY) *
                                (task.cellsX + 1) +
                            std::clamp(x, 0, task.cellsX))];
                    };
                    return glm::normalize(glm::vec3(
                        (heightAt(col - 1, row) - heightAt(col + 1, row)) /
                            (2.f * task.cellWidth), 1.f,
                        (heightAt(col, row - 1) - heightAt(col, row + 1)) /
                            (2.f * task.cellDepth)));
                };
                const auto seamNormal = [&](const FarPrepareTask::SeamSurface* surface,
                    int seam, int col, int row) -> std::optional<glm::vec3> {
                    if (!surface) return std::nullopt;
                    if (seam == 0 && col == 0)
                        return detailedNormal(surface->heights, task.cellsX, row);
                    if (seam == 1 && row == task.cellsY)
                        return detailedNormal(surface->heights, col, 0);
                    if (seam == 2 && col == task.cellsX)
                        return detailedNormal(surface->heights, 0, row);
                    if (seam == 3 && row == 0)
                        return detailedNormal(surface->heights, col, task.cellsY);
                    return std::nullopt;
                };
                const auto setColor = [&](Mesh::Vertex& vertex, float height) {
                    float weights[6];
                    TerrainHeightArchive::BlendHeightBands(height,
                        task.palette.heightBoundaries,
                        task.palette.heightBlendHalfWidth, weights);
                    glm::vec3 color(0.f);
                    for (int layer = 0; layer < 6; ++layer)
                        color += task.palette.farColors[layer] * weights[layer];
                    vertex.color[0] = color.r;
                    vertex.color[1] = color.g;
                    vertex.color[2] = color.b;
                    vertex.color[3] = 1.f;
                };
                const auto blendSeamColor = [&](Mesh::Vertex& vertex,
                    const FarPrepareTask::SeamSurface* surface,
                    int seam, int col, int row, float u, float v) {
                    if (!surface || (surface->layerCount != 4 &&
                        surface->layerCount != 6) ||
                        surface->weights.size() != surface->heights.size() *
                            surface->layerCount) return;
                    const float blend = seam == 0 ? 1.f - u :
                        seam == 1 ? v : seam == 2 ? u :
                        seam == 3 ? 1.f - v : 0.f;
                    if (blend <= 0.f) return;
                    const int nearCol = seam == 0 ? task.cellsX :
                        seam == 2 ? 0 : col;
                    const int nearRow = seam == 1 ? 0 :
                        seam == 3 ? task.cellsY : row;
                    const size_t sampleIndex = static_cast<size_t>(nearRow) *
                        (task.cellsX + 1) + nearCol;
                    glm::vec3 color(0.f);
                    for (uint32_t layer = 0; layer < surface->layerCount; ++layer)
                    {
                        const int nearLayer = surface->layerCount == 4
                            ? task.palette.archiveToNear[layer] :
                            static_cast<int>(layer);
                        color += task.palette.nearColors[nearLayer] *
                            surface->weights[sampleIndex *
                                surface->layerCount + layer];
                    }
                    const glm::vec3 farColor(vertex.color[0], vertex.color[1],
                        vertex.color[2]);
                    color = glm::mix(farColor, color, blend);
                    vertex.color[0] = color.r;
                    vertex.color[1] = color.g;
                    vertex.color[2] = color.b;
                };
                result.vertices.resize(static_cast<size_t>(width) * depth);
                float minimumHeight = std::numeric_limits<float>::max();
                float maximumHeight = std::numeric_limits<float>::lowest();
                for (int row = 0; row < depth; ++row)
                    for (int col = 0; col < width; ++col)
                    {
                        const int x = std::min(task.startX + col *
                            task.lodStride, task.endX);
                        const int y = std::min(task.startY + row *
                            task.lodStride, task.endY);
                        auto& vertex = result.vertices[
                            static_cast<size_t>(row) * width + col];
                        const float height = sample(x, y);
                        minimumHeight = std::min(minimumHeight, height);
                        maximumHeight = std::max(maximumHeight, height);
                        vertex.pos[0] = (x - task.startX) * task.spanX;
                        vertex.pos[1] = height;
                        vertex.pos[2] = (y - task.startY) * task.spanZ;
                        const glm::vec3 normal = glm::normalize(glm::vec3(
                            (sample(x - 1, y) - sample(x + 1, y)) /
                                (2.f * task.spanX), 1.f,
                            (sample(x, y - 1) - sample(x, y + 1)) /
                                (2.f * task.spanZ)));
                        vertex.normal[0] = normal.x;
                        vertex.normal[1] = normal.y;
                        vertex.normal[2] = normal.z;
                        vertex.uv[0] = static_cast<float>(x + task.halfExtent) /
                            (task.side - 1);
                        vertex.uv[1] = static_cast<float>(y + task.halfExtent) /
                            (task.side - 1);
                        setColor(vertex, height);
                    }
                if (task.lodStride > 1)
                {
                    for (int row = 0; row < depth - 1; ++row)
                        for (int col = 0; col < width - 1; ++col)
                        {
                            const uint32_t a = row * width + col;
                            const uint32_t b = a + width;
                            result.indices.insert(result.indices.end(),
                                {a, b, a + 1, a + 1, b, b + 1});
                        }
                }
                else for (int y = task.startY; y < task.endY; ++y)
                    for (int x = task.startX; x < task.endX; ++x)
                    {
                        const bool patched = std::any_of(task.patches.begin(),
                            task.patches.end(), [&](const auto& item) {
                                return item.second.x == x && item.second.y == y;
                            });
                        if (!visibleCell(x, y) || patched) continue;
                        const int seam = seamSide(x, y);
                        const auto* surface = seam >= 0
                            ? seamSurface(x, y, seam) : nullptr;
                        if (surface && surface->heights.size() ==
                            static_cast<size_t>(task.cellsX + 1) *
                                (task.cellsY + 1))
                        {
                            const float topLeft = sample(x, y);
                            const float topRight = sample(x + 1, y);
                            const float bottomLeft = sample(x, y + 1);
                            const float bottomRight = sample(x + 1, y + 1);
                            const auto addVertex = [&](float u, float v) {
                                Mesh::Vertex vertex{};
                                float height = (1.f - u) * (1.f - v) * topLeft +
                                    u * (1.f - v) * topRight +
                                    (1.f - u) * v * bottomLeft +
                                    u * v * bottomRight;
                                const int col = static_cast<int>(std::round(
                                    u * task.cellsX));
                                const int row = static_cast<int>(std::round(
                                    v * task.cellsY));
                                const size_t stride = task.cellsX + 1;
                                if (seam == 0 && u == 0.f)
                                    height = surface->heights[row * stride +
                                        task.cellsX];
                                else if (seam == 1 && v == 1.f)
                                    height = surface->heights[col];
                                else if (seam == 2 && u == 1.f)
                                    height = surface->heights[row * stride];
                                else if (seam == 3 && v == 0.f)
                                    height = surface->heights[
                                        task.cellsY * stride + col];
                                vertex.pos[0] = (x - task.startX + u) * task.spanX;
                                vertex.pos[1] = height;
                                vertex.pos[2] = (y - task.startY + v) * task.spanZ;
                                const float dx = ((1.f - v) *
                                    (topRight - topLeft) + v *
                                    (bottomRight - bottomLeft)) / task.spanX;
                                const float dz = ((1.f - u) *
                                    (bottomLeft - topLeft) + u *
                                    (bottomRight - topRight)) / task.spanZ;
                                const glm::vec3 normal = seamNormal(surface,
                                    seam, col, row).value_or(glm::normalize(
                                        glm::vec3(-dx, 1.f, -dz)));
                                vertex.normal[0] = normal.x;
                                vertex.normal[1] = normal.y;
                                vertex.normal[2] = normal.z;
                                vertex.uv[0] = static_cast<float>(x + u +
                                    task.halfExtent) / (task.side - 1);
                                vertex.uv[1] = static_cast<float>(y + v +
                                    task.halfExtent) / (task.side - 1);
                                setColor(vertex, height);
                                blendSeamColor(vertex, surface, seam, col,
                                    row, u, v);
                                const uint32_t index = static_cast<uint32_t>(
                                    result.vertices.size());
                                result.vertices.push_back(vertex);
                                return index;
                            };
                            const uint32_t center = addVertex(.5f, .5f);
                            std::vector<uint32_t> boundary;
                            const int leftSteps = seam == 0 ? task.cellsY : 1;
                            const int bottomSteps = seam == 1 ? task.cellsX : 1;
                            const int rightSteps = seam == 2 ? task.cellsY : 1;
                            const int topSteps = seam == 3 ? task.cellsX : 1;
                            for (int i = 0; i < leftSteps; ++i)
                                boundary.push_back(addVertex(0.f,
                                    static_cast<float>(i) / leftSteps));
                            for (int i = 0; i < bottomSteps; ++i)
                                boundary.push_back(addVertex(
                                    static_cast<float>(i) / bottomSteps, 1.f));
                            for (int i = 0; i < rightSteps; ++i)
                                boundary.push_back(addVertex(1.f,
                                    1.f - static_cast<float>(i) / rightSteps));
                            for (int i = 0; i < topSteps; ++i)
                                boundary.push_back(addVertex(
                                    1.f - static_cast<float>(i) / topSteps, 0.f));
                            for (size_t i = 0; i < boundary.size(); ++i)
                                result.indices.insert(result.indices.end(),
                                    {center, boundary[i],
                                        boundary[(i + 1) % boundary.size()]});
                            continue;
                        }
                        const uint32_t a = static_cast<uint32_t>(
                            (y - task.startY) * width + x - task.startX);
                        const uint32_t b = a + width;
                        result.indices.insert(result.indices.end(),
                            {a, b, a + 1, a + 1, b, b + 1});
                    }
                if (!result.indices.empty())
                {
                const float skirtBottom = minimumHeight -
                    std::max(2.f, maximumHeight - minimumHeight);
                const auto addSkirtEdge = [&](uint32_t first, uint32_t second) {
                    Mesh::Vertex lowFirst = result.vertices[first];
                    Mesh::Vertex lowSecond = result.vertices[second];
                    lowFirst.pos[1] = lowSecond.pos[1] = skirtBottom;
                    const uint32_t lowIndex = static_cast<uint32_t>(
                        result.vertices.size());
                    result.vertices.push_back(lowFirst);
                    result.vertices.push_back(lowSecond);
                    result.indices.insert(result.indices.end(),
                        {first, lowIndex, second, second, lowIndex,
                            lowIndex + 1, second, lowIndex, first,
                            lowIndex + 1, lowIndex, second});
                };
                for (int col = 0; col < width - 1; ++col)
                {
                    addSkirtEdge(col, col + 1);
                    addSkirtEdge((depth - 1) * width + col,
                        (depth - 1) * width + col + 1);
                }
                for (int row = 0; row < depth - 1; ++row)
                {
                    addSkirtEdge(row * width, (row + 1) * width);
                    addSkirtEdge(row * width + width - 1,
                        (row + 1) * width + width - 1);
                }
                }
                for (const auto& [patchIndex, patch] : task.patches)
                {
                    if (!visibleCell(patch.x, patch.y)) continue;
                    const int patchWidth = task.cellsX + 1;
                    const int patchDepth = task.cellsY + 1;
                    if (patch.heights.size() != static_cast<size_t>(
                        patchWidth) * patchDepth) continue;
                    const int seam = seamSide(patch.x, patch.y);
                    const auto* surface = seam >= 0
                        ? seamSurface(patch.x, patch.y, seam) : nullptr;
                    const bool hasNear = surface && surface->heights.size() ==
                        patch.heights.size();
                    const float topLeft = sample(patch.x, patch.y);
                    const float topRight = sample(patch.x + 1, patch.y);
                    const float bottomLeft = sample(patch.x, patch.y + 1);
                    const float bottomRight = sample(patch.x + 1, patch.y + 1);
                    const size_t base = result.vertices.size();
                    result.patchVertexOffsets[patchIndex] = base;
                    const auto patchSample = [&](int x, int y) {
                        return patch.heights[static_cast<size_t>(
                            std::clamp(y, 0, task.cellsY)) * patchWidth +
                            std::clamp(x, 0, task.cellsX)];
                    };
                    for (int row = 0; row < patchDepth; ++row)
                        for (int col = 0; col < patchWidth; ++col)
                        {
                            const float u = static_cast<float>(col) / task.cellsX;
                            const float v = static_cast<float>(row) / task.cellsY;
                            const float edge = (1.f - u) * (1.f - v) * topLeft +
                                u * (1.f - v) * topRight +
                                (1.f - u) * v * bottomLeft +
                                u * v * bottomRight;
                            const float fade = std::min({1.f,
                                static_cast<float>(col), static_cast<float>(row),
                                static_cast<float>(task.cellsX - col),
                                static_cast<float>(task.cellsY - row)});
                            float height = edge +
                                (patchSample(col, row) - edge) * fade;
                            if (hasNear)
                            {
                                if (seam == 0 && col == 0)
                                    height = surface->heights[static_cast<size_t>(
                                        row * patchWidth + task.cellsX)];
                                else if (seam == 1 && row == task.cellsY)
                                    height = surface->heights[col];
                                else if (seam == 2 && col == task.cellsX)
                                    height = surface->heights[static_cast<size_t>(
                                        row * patchWidth)];
                                else if (seam == 3 && row == 0)
                                    height = surface->heights[static_cast<size_t>(
                                        task.cellsY * patchWidth + col)];
                            }
                            Mesh::Vertex vertex{};
                            vertex.pos[0] = (patch.x - task.startX) * task.spanX +
                                col * task.cellWidth;
                            vertex.pos[1] = height;
                            vertex.pos[2] = (patch.y - task.startY) * task.spanZ +
                                row * task.cellDepth;
                            const glm::vec3 coarseNormal = glm::normalize(glm::vec3(
                                (patchSample(col - 1, row) -
                                    patchSample(col + 1, row)) /
                                    (2.f * task.cellWidth), 1.f,
                                (patchSample(col, row - 1) -
                                    patchSample(col, row + 1)) /
                                    (2.f * task.cellDepth)));
                            const glm::vec3 normal = seamNormal(hasNear ?
                                surface : nullptr, seam, col, row).value_or(
                                    coarseNormal);
                            vertex.normal[0] = normal.x;
                            vertex.normal[1] = normal.y;
                            vertex.normal[2] = normal.z;
                            vertex.uv[0] = u;
                            vertex.uv[1] = v;
                            setColor(vertex, height);
                            if (patch.colors.size() == patch.heights.size() * 3)
                                for (int channel = 0; channel < 3; ++channel)
                                    vertex.color[channel] = patch.colors[
                                        (static_cast<size_t>(row) * patchWidth +
                                            col) * 3 + channel];
                            blendSeamColor(vertex, hasNear ? surface : nullptr,
                                seam, col, row, u, v);
                            result.vertices.push_back(vertex);
                        }
                    for (int row = 0; row < task.cellsY; ++row)
                        for (int col = 0; col < task.cellsX; ++col)
                        {
                            const uint32_t a = static_cast<uint32_t>(base +
                                row * patchWidth + col);
                            const uint32_t b = a + patchWidth;
                            result.indices.insert(result.indices.end(),
                                {a, b, a + 1, a + 1, b, b + 1});
                        }
                }
                if (!task.cachePath.empty())
                    WriteFarTileCache(task.cachePath, task.fingerprint,
                        result.vertices, result.indices);
            }
            if (!result.indices.empty())
            {
                result.bounds = Mesh::PrepareIndexedGeometry(
                    result.vertices, result.indices);
                if (!result.bounds)
                    throw std::runtime_error("Invalid prepared far tile mesh");
            }
            result.success = true;
        }
        catch (...)
        {
            result.vertices.clear();
            result.indices.clear();
        }
        if (task.lodStride == 1)
            for (int y = task.startY; y < task.endY; ++y)
                for (int x = task.startX; x < task.endX; ++x)
                    result.builtVisibility.push_back(static_cast<uint8_t>(
                        !task.hiddenCells.count({x, y})));
        result.buildMilliseconds = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - taskStart).count();
        {
            std::lock_guard<std::mutex> lock(m_farPrepareMutex);
            if (task.revision == m_farPrepareRevision)
                m_preparedFarTiles.push_back(std::move(result));
        }
    }
}

}

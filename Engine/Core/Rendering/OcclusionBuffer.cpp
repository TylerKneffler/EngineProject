#include "Core/Rendering/OcclusionBuffer.h"

#include "Core/Rendering/RenderJobs.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>

namespace Engine::Rendering
{
namespace
{
std::mutex s_frameStatisticsMutex;
OcclusionBuffer::FrameStatistics s_frameStatistics;
constexpr float NearW = .05f;
constexpr float Far = std::numeric_limits<float>::max();
constexpr int BandRows = 8;

struct ScreenTriangle
{
    float x[3], y[3], inverseW[3];
    float minimumX, maximumX, minimumY, maximumY;
};

// Clips a clip-space polygon against w >= NearW (Sutherland-Hodgman).
int ClipNear(const glm::vec4* input, int count, glm::vec4* output)
{
    int written = 0;
    for (int index = 0; index < count; ++index)
    {
        const glm::vec4& a = input[index];
        const glm::vec4& b = input[(index + 1) % count];
        const bool insideA = a.w >= NearW, insideB = b.w >= NearW;
        if (insideA) output[written++] = a;
        if (insideA != insideB)
        {
            const float t = (NearW - a.w) / (b.w - a.w);
            output[written++] = a + (b - a) * t;
        }
    }
    return written;
}
}

void OcclusionBuffer::Build(const glm::mat4& viewProjection, float aspect,
    const std::vector<Occluder>& occluders)
{
    const auto start = std::chrono::steady_clock::now();
    m_viewProjection = viewProjection;
    m_height = std::clamp(static_cast<int>(std::lround(Width / std::max(aspect, .25f))), 16, 512);
    m_statistics = {};
    m_statistics.occluders = occluders.size();
    const float width = static_cast<float>(Width), height = static_cast<float>(m_height);

    // Transform and clip every occluder's triangles to screen space.
    std::vector<std::vector<ScreenTriangle>> triangles(occluders.size());
    std::vector<uint64_t> counted(occluders.size(), 0);
    RenderJobs::Get().For(occluders.size(), 1, [&](size_t index)
    {
        const Occluder& occluder = occluders[index];
        if (!occluder.vertices || !occluder.indices || occluder.indexCount < 3) return;
        const glm::mat4 transform = viewProjection * occluder.world;
        std::vector<glm::vec4> clip(occluder.vertexCount);
        const auto* bytes = static_cast<const uint8_t*>(occluder.vertices);
        for (size_t vertex = 0; vertex < occluder.vertexCount; ++vertex)
        {
            float position[3];
            std::memcpy(position, bytes + vertex * occluder.vertexStride, sizeof(position));
            clip[vertex] = transform * glm::vec4(position[0], position[1], position[2], 1.f);
        }
        auto& output = triangles[index];
        counted[index] = occluder.indexCount / 3;
        for (size_t base = 0; base + 2 < occluder.indexCount; base += 3)
        {
            const uint32_t i0 = occluder.indices[base], i1 = occluder.indices[base + 1],
                i2 = occluder.indices[base + 2];
            if (i0 >= clip.size() || i1 >= clip.size() || i2 >= clip.size()) continue;
            glm::vec4 polygon[3] = { clip[i0], clip[i1], clip[i2] };
            // Trivial rejects against the side planes (all three outside one).
            bool outside = false;
            for (int axis = 0; axis < 2 && !outside; ++axis)
                outside = (polygon[0][axis] > polygon[0].w && polygon[1][axis] > polygon[1].w &&
                    polygon[2][axis] > polygon[2].w) ||
                    (polygon[0][axis] < -polygon[0].w && polygon[1][axis] < -polygon[1].w &&
                    polygon[2][axis] < -polygon[2].w);
            if (outside) continue;
            glm::vec4 clipped[4];
            int count = 3;
            const glm::vec4* vertices = polygon;
            if (polygon[0].w < NearW || polygon[1].w < NearW || polygon[2].w < NearW)
            {
                count = ClipNear(polygon, 3, clipped);
                vertices = clipped;
                if (count < 3) continue;
            }
            float sx[4], sy[4], iw[4];
            for (int vertex = 0; vertex < count; ++vertex)
            {
                iw[vertex] = 1.f / vertices[vertex].w;
                sx[vertex] = (vertices[vertex].x * iw[vertex] * .5f + .5f) * width;
                sy[vertex] = (.5f - vertices[vertex].y * iw[vertex] * .5f) * height;
            }
            for (int fan = 1; fan + 1 < count; ++fan)
            {
                ScreenTriangle triangle;
                const int order[3] = { 0, fan, fan + 1 };
                for (int corner = 0; corner < 3; ++corner)
                {
                    triangle.x[corner] = sx[order[corner]];
                    triangle.y[corner] = sy[order[corner]];
                    triangle.inverseW[corner] = iw[order[corner]];
                }
                triangle.minimumX = std::min({ triangle.x[0], triangle.x[1], triangle.x[2] });
                triangle.maximumX = std::max({ triangle.x[0], triangle.x[1], triangle.x[2] });
                triangle.minimumY = std::min({ triangle.y[0], triangle.y[1], triangle.y[2] });
                triangle.maximumY = std::max({ triangle.y[0], triangle.y[1], triangle.y[2] });
                if (triangle.maximumX < 0.f || triangle.minimumX > width ||
                    triangle.maximumY < 0.f || triangle.minimumY > height) continue;
                // Centre sampling: a triangle containing no pixel centre
                // (most distant terrain at this resolution) writes nothing.
                if (std::ceil(triangle.minimumX - .5f) > std::floor(triangle.maximumX - .5f) ||
                    std::ceil(triangle.minimumY - .5f) > std::floor(triangle.maximumY - .5f)) continue;
                output.push_back(triangle);
            }
        }
    });

    // Bin triangles by the bands of rows they touch. Triangles that contain
    // no pixel centre (most distant terrain at this resolution) write
    // nothing with centre sampling and are dropped here.
    const int bands = (m_height + BandRows - 1) / BandRows;
    std::vector<std::vector<const ScreenTriangle*>> binned(bands);
    for (const auto& list : triangles)
        for (const auto& triangle : list)
        {
            const int x0 = static_cast<int>(std::ceil(triangle.minimumX - .5f));
            const int x1 = static_cast<int>(std::floor(triangle.maximumX - .5f));
            const int y0 = std::max(0, static_cast<int>(std::ceil(triangle.minimumY - .5f)));
            const int y1 = std::min(m_height - 1, static_cast<int>(std::floor(triangle.maximumY - .5f)));
            if (x0 > x1 || y0 > y1 || x1 < 0 || x0 >= Width) continue;
            for (int band = y0 / BandRows; band <= y1 / BandRows; ++band)
                binned[band].push_back(&triangle);
        }

    // Rasterize nearest depth in horizontal bands; each band owns its rows.
    std::vector<float> depth(static_cast<size_t>(Width) * m_height, Far);
    std::vector<uint64_t> rasterized(bands, 0);
    RenderJobs::Get().For(static_cast<size_t>(bands), 1, [&](size_t band)
    {
        const int rowStart = static_cast<int>(band) * BandRows;
        const int rowEnd = std::min(m_height, rowStart + BandRows);
        for (const ScreenTriangle* pointer : binned[band])
            {
                const ScreenTriangle& triangle = *pointer;
                const float area = (triangle.x[1] - triangle.x[0]) * (triangle.y[2] - triangle.y[0]) -
                    (triangle.x[2] - triangle.x[0]) * (triangle.y[1] - triangle.y[0]);
                if (std::abs(area) < 1.e-8f) continue;
                const float sign = area > 0.f ? 1.f : -1.f;
                const float inverseArea = 1.f / std::abs(area);
                const int x0 = std::max(0, static_cast<int>(std::floor(triangle.minimumX)));
                const int x1 = std::min(Width - 1, static_cast<int>(std::ceil(triangle.maximumX)));
                const int y0 = std::max(rowStart, static_cast<int>(std::floor(triangle.minimumY)));
                const int y1 = std::min(rowEnd - 1, static_cast<int>(std::ceil(triangle.maximumY)));
                if (x0 > x1 || y0 > y1) continue;
                ++rasterized[band];
                const auto edge = [&](int a, int b, float px, float py)
                {
                    return ((triangle.x[b] - triangle.x[a]) * (py - triangle.y[a]) -
                        (triangle.y[b] - triangle.y[a]) * (px - triangle.x[a])) * sign;
                };
                for (int y = y0; y <= y1; ++y)
                {
                    const float py = y + .5f;
                    float* row = depth.data() + static_cast<size_t>(y) * Width;
                    for (int x = x0; x <= x1; ++x)
                    {
                        const float px = x + .5f;
                        const float w0 = edge(1, 2, px, py), w1 = edge(2, 0, px, py),
                            w2 = edge(0, 1, px, py);
                        if (w0 < 0.f || w1 < 0.f || w2 < 0.f) continue;
                        const float inverseW = (w0 * triangle.inverseW[0] + w1 * triangle.inverseW[1] +
                            w2 * triangle.inverseW[2]) * inverseArea;
                        if (inverseW <= 0.f) continue;
                        row[x] = std::min(row[x], 1.f / inverseW);
                    }
                }
            }
    });

    // Erode: each pixel takes the farthest depth of its 3x3 neighbourhood,
    // so partly covered silhouette pixels never hide anything.
    std::vector<float> horizontal(depth.size());
    for (int y = 0; y < m_height; ++y)
        for (int x = 0; x < Width; ++x)
        {
            const size_t index = static_cast<size_t>(y) * Width + x;
            float value = depth[index];
            if (x > 0) value = std::max(value, depth[index - 1]);
            if (x + 1 < Width) value = std::max(value, depth[index + 1]);
            horizontal[index] = value;
        }
    m_levels.clear();
    m_levelSizes.clear();
    m_levels.emplace_back(depth.size());
    auto& level0 = m_levels.front();
    for (int y = 0; y < m_height; ++y)
        for (int x = 0; x < Width; ++x)
        {
            const size_t index = static_cast<size_t>(y) * Width + x;
            float value = horizontal[index];
            if (y > 0) value = std::max(value, horizontal[index - Width]);
            if (y + 1 < m_height) value = std::max(value, horizontal[index + Width]);
            level0[index] = value;
        }
    m_levelSizes.push_back({ Width, m_height });
    // Max-depth pyramid.
    while (m_levelSizes.back().x > 1 || m_levelSizes.back().y > 1)
    {
        const glm::ivec2 source = m_levelSizes.back();
        const glm::ivec2 size((source.x + 1) / 2, (source.y + 1) / 2);
        std::vector<float> next(static_cast<size_t>(size.x) * size.y);
        const auto& previous = m_levels.back();
        for (int y = 0; y < size.y; ++y)
            for (int x = 0; x < size.x; ++x)
            {
                float value = 0.f;
                for (int dy = 0; dy < 2; ++dy)
                    for (int dx = 0; dx < 2; ++dx)
                    {
                        const int sx = std::min(source.x - 1, x * 2 + dx);
                        const int sy = std::min(source.y - 1, y * 2 + dy);
                        value = std::max(value, previous[static_cast<size_t>(sy) * source.x + sx]);
                    }
                next[static_cast<size_t>(y) * size.x + x] = value;
            }
        m_levels.push_back(std::move(next));
        m_levelSizes.push_back(size);
    }
    for (const auto value : counted) m_statistics.triangles += value;
    for (const auto value : rasterized) m_statistics.rasterizedTriangles += value;
    m_valid = true;
    m_statistics.buildMilliseconds = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();
}

bool OcclusionBuffer::IsBoxHidden(const glm::vec3& minimum, const glm::vec3& maximum) const
{
    if (!m_valid) return false;
    float x0 = Far, y0 = Far, x1 = -Far, y1 = -Far, nearest = Far;
    for (int corner = 0; corner < 8; ++corner)
    {
        const glm::vec3 point((corner & 1) ? maximum.x : minimum.x,
            (corner & 2) ? maximum.y : minimum.y, (corner & 4) ? maximum.z : minimum.z);
        const glm::vec4 clip = m_viewProjection * glm::vec4(point, 1.f);
        if (clip.w <= NearW) return false;
        const float x = (clip.x / clip.w * .5f + .5f) * Width;
        const float y = (.5f - clip.y / clip.w * .5f) * m_height;
        x0 = std::min(x0, x); x1 = std::max(x1, x);
        y0 = std::min(y0, y); y1 = std::max(y1, y);
        nearest = std::min(nearest, clip.w);
    }
    return IsRectangleHidden(x0, y0, x1, y1, nearest);
}

bool OcclusionBuffer::IsSphereHidden(const glm::vec3& center, float radius) const
{
    return IsBoxHidden(center - glm::vec3(radius), center + glm::vec3(radius));
}

bool OcclusionBuffer::IsRectangleHidden(float x0, float y0, float x1, float y1, float nearest) const
{
    // Bounds entirely off screen stay with the frustum test. The off-screen
    // part of partly visible bounds cannot be seen, so the rectangle clamps.
    if (x1 < 0.f || y1 < 0.f || x0 >= Width || y0 >= m_height) return false;
    const int ix0 = std::clamp(static_cast<int>(std::floor(x0)), 0, Width - 1);
    const int iy0 = std::clamp(static_cast<int>(std::floor(y0)), 0, m_height - 1);
    const int ix1 = std::clamp(static_cast<int>(std::floor(x1)), 0, Width - 1);
    const int iy1 = std::clamp(static_cast<int>(std::floor(y1)), 0, m_height - 1);
    size_t level = 0;
    while (level + 1 < m_levels.size() && ((ix1 >> level) - (ix0 >> level) > 1 ||
        (iy1 >> level) - (iy0 >> level) > 1)) ++level;
    const glm::ivec2 size = m_levelSizes[level];
    const auto& depth = m_levels[level];
    float farthest = 0.f;
    for (int y = iy0 >> level; y <= std::min(size.y - 1, iy1 >> static_cast<int>(level)); ++y)
        for (int x = ix0 >> level; x <= std::min(size.x - 1, ix1 >> static_cast<int>(level)); ++x)
            farthest = std::max(farthest, depth[static_cast<size_t>(y) * size.x + x]);
    if (farthest >= Far) return false;
    // A small margin keeps surfaces lying on an occluder (or sharing its
    // depth at a seam) visible.
    return nearest > farthest * 1.002f + .05f;
}

size_t OcclusionBuffer::GpuFloatCount(int height)
{
    size_t count = GpuHeaderFloats;
    int width = Width;
    height = std::max(1, height);
    for (size_t level = 0; level < GpuMaximumLevels; ++level)
    {
        count += static_cast<size_t>(width) * height;
        if (width == 1 && height == 1) break;
        width = (width + 1) / 2; height = (height + 1) / 2;
    }
    return count;
}

void OcclusionBuffer::SerializeForGpu(std::vector<float>& output) const
{
    output.assign(GpuHeaderFloats, 0.f);
    if (!m_valid) return;
    const size_t levels = std::min(m_levels.size(), GpuMaximumLevels);
    output[0] = static_cast<float>(levels);
    output[1] = static_cast<float>(Width);
    output[2] = static_cast<float>(m_height);
    for (int row = 0; row < 4; ++row)
        for (int column = 0; column < 4; ++column)
            output[64 + row * 4 + column] = m_viewProjection[column][row];
    for (size_t level = 0; level < levels; ++level)
    {
        output[4 + level * 3] = static_cast<float>(output.size());
        output[5 + level * 3] = static_cast<float>(m_levelSizes[level].x);
        output[6 + level * 3] = static_cast<float>(m_levelSizes[level].y);
        output.insert(output.end(), m_levels[level].begin(), m_levels[level].end());
    }
}

void OcclusionBuffer::PublishFrameStatistics(const FrameStatistics& statistics)
{
    std::lock_guard lock(s_frameStatisticsMutex);
    s_frameStatistics = statistics;
}

OcclusionBuffer::FrameStatistics OcclusionBuffer::LastFrameStatistics()
{
    std::lock_guard lock(s_frameStatisticsMutex);
    return s_frameStatistics;
}
}

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
// Build work is split into chunks of this many vertices or triangles.
constexpr size_t VertexChunk = 4096, TriangleChunk = 2048;

// A screen triangle ready for edge stepping. Edge i is A*x + B*y + C,
// non-negative inside; inverse w is the plane P*x + Q*y + R. The pixel
// range holds every pixel centre inside the triangle's bounding box.
struct ScreenTriangle
{
    float edgeA[3], edgeB[3], edgeC[3];
    float planeP, planeQ, planeR;
    int16_t x0, x1, y0, y1;
};

// One chunk's triangles, listed per band of rows they touch.
struct OccluderTriangles
{
    std::vector<ScreenTriangle> triangles;
    std::vector<uint32_t> bandStart; // bands + 1 offsets into bandTriangles
    std::vector<uint32_t> bandTriangles;
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

// Screen rectangle of local bounds.
enum class BoundsProjection { OnScreenPlane, CrossesNear, Behind };
BoundsProjection ProjectBounds(const glm::mat4& transform, const glm::vec3& minimum,
    const glm::vec3& maximum, float width, float height, float& x0, float& y0, float& x1, float& y1)
{
    x0 = y0 = Far; x1 = y1 = -Far;
    int behind = 0;
    for (int corner = 0; corner < 8; ++corner)
    {
        const glm::vec4 clip = transform * glm::vec4((corner & 1) ? maximum.x : minimum.x,
            (corner & 2) ? maximum.y : minimum.y, (corner & 4) ? maximum.z : minimum.z, 1.f);
        if (clip.w <= NearW) { ++behind; continue; }
        const float x = (clip.x / clip.w * .5f + .5f) * width;
        const float y = (.5f - clip.y / clip.w * .5f) * height;
        x0 = std::min(x0, x); x1 = std::max(x1, x);
        y0 = std::min(y0, y); y1 = std::max(y1, y);
    }
    // Every corner behind the near plane: so is everything inside. Some
    // behind: the size on screen is unknown.
    return behind == 8 ? BoundsProjection::Behind : behind ? BoundsProjection::CrossesNear
        : BoundsProjection::OnScreenPlane;
}

// Sets up a screen triangle. False when its bounding box holds no pixel
// centre on screen (with centre sampling it would write nothing) or it is
// degenerate.
bool SetupTriangle(const float* sx, const float* sy, const float* iw, int width, int height,
    ScreenTriangle& triangle)
{
    const float minimumX = std::min({ sx[0], sx[1], sx[2] }), maximumX = std::max({ sx[0], sx[1], sx[2] });
    const float minimumY = std::min({ sy[0], sy[1], sy[2] }), maximumY = std::max({ sy[0], sy[1], sy[2] });
    if (maximumX < 0.f || minimumX > width || maximumY < 0.f || minimumY > height) return false;
    const int x0 = std::max(0, static_cast<int>(std::ceil(minimumX - .5f)));
    const int x1 = std::min(width - 1, static_cast<int>(std::floor(maximumX - .5f)));
    const int y0 = std::max(0, static_cast<int>(std::ceil(minimumY - .5f)));
    const int y1 = std::min(height - 1, static_cast<int>(std::floor(maximumY - .5f)));
    if (x0 > x1 || y0 > y1) return false;
    const float area = (sx[1] - sx[0]) * (sy[2] - sy[0]) - (sx[2] - sx[0]) * (sy[1] - sy[0]);
    if (std::abs(area) < 1.e-8f) return false;
    const float sign = area > 0.f ? 1.f : -1.f;
    const float inverseArea = 1.f / std::abs(area);
    // Edge i runs between the other two corners; its value is the
    // barycentric weight of corner i times the area.
    static constexpr int From[3] = { 1, 2, 0 }, To[3] = { 2, 0, 1 };
    triangle.planeP = triangle.planeQ = triangle.planeR = 0.f;
    for (int edge = 0; edge < 3; ++edge)
    {
        const int a = From[edge], b = To[edge];
        triangle.edgeA[edge] = -(sy[b] - sy[a]) * sign;
        triangle.edgeB[edge] = (sx[b] - sx[a]) * sign;
        triangle.edgeC[edge] = ((sy[b] - sy[a]) * sx[a] - (sx[b] - sx[a]) * sy[a]) * sign;
        triangle.planeP += triangle.edgeA[edge] * iw[edge];
        triangle.planeQ += triangle.edgeB[edge] * iw[edge];
        triangle.planeR += triangle.edgeC[edge] * iw[edge];
    }
    triangle.planeP *= inverseArea; triangle.planeQ *= inverseArea; triangle.planeR *= inverseArea;
    triangle.x0 = static_cast<int16_t>(x0); triangle.x1 = static_cast<int16_t>(x1);
    triangle.y0 = static_cast<int16_t>(y0); triangle.y1 = static_cast<int16_t>(y1);
    return true;
}

// Rasterizes rows [rowStart, rowEnd) of a triangle into a buffer of inverse
// w, where the largest value is the nearest.
void RasterizeRows(const ScreenTriangle& triangle, int rowStart, int rowEnd, float* inverseDepth)
{
    const int y0 = std::max(rowStart, static_cast<int>(triangle.y0));
    const int y1 = std::min(rowEnd - 1, static_cast<int>(triangle.y1));
    const int x0 = triangle.x0, x1 = triangle.x1;
    const float px0 = x0 + .5f;
    for (int y = y0; y <= y1; ++y)
    {
        // Each row starts from a direct evaluation, so stepping error never
        // builds up across rows.
        const float py = y + .5f;
        float e0 = triangle.edgeA[0] * px0 + triangle.edgeB[0] * py + triangle.edgeC[0];
        float e1 = triangle.edgeA[1] * px0 + triangle.edgeB[1] * py + triangle.edgeC[1];
        float e2 = triangle.edgeA[2] * px0 + triangle.edgeB[2] * py + triangle.edgeC[2];
        float inverseW = triangle.planeP * px0 + triangle.planeQ * py + triangle.planeR;
        float* row = inverseDepth + static_cast<size_t>(y) * OcclusionBuffer::Width;
        for (int x = x0; x <= x1; ++x)
        {
            if (e0 >= 0.f && e1 >= 0.f && e2 >= 0.f && inverseW > row[x]) row[x] = inverseW;
            e0 += triangle.edgeA[0]; e1 += triangle.edgeA[1]; e2 += triangle.edgeA[2];
            inverseW += triangle.planeP;
        }
    }
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
    const int bands = (m_height + BandRows - 1) / BandRows;

    // Occluders off screen, or too small to keep a pixel through the
    // erosion, are skipped before any vertex work.
    std::vector<glm::mat4> transforms(occluders.size());
    std::vector<uint8_t> active(occluders.size(), 0);
    for (size_t index = 0; index < occluders.size(); ++index)
    {
        const Occluder& occluder = occluders[index];
        if (!occluder.vertices || !occluder.indices || occluder.indexCount < 3) continue;
        m_statistics.triangles += occluder.indexCount / 3;
        transforms[index] = viewProjection * occluder.world;
        float bx0, by0, bx1, by1;
        const BoundsProjection projection = occluder.hasBounds ? ProjectBounds(transforms[index],
            occluder.boundsMinimum, occluder.boundsMaximum, width, height, bx0, by0, bx1, by1)
            : BoundsProjection::CrossesNear;
        if (projection == BoundsProjection::Behind || (projection == BoundsProjection::OnScreenPlane &&
            (bx1 < 0.f || by1 < 0.f || bx0 > width || by0 > height ||
            bx1 - bx0 < occluder.minimumPixels || by1 - by0 < occluder.minimumPixels)))
        {
            ++m_statistics.skippedOccluders;
            continue;
        }
        active[index] = 1;
    }

    // Work is split into fixed-size chunks rather than one job per occluder,
    // so one large near tile does not hold up the build.
    struct Chunk { size_t occluder, begin, end; };
    std::vector<Chunk> vertexChunks, triangleChunks;
    std::vector<std::vector<glm::vec4>> clips(occluders.size());
    // Screen x, y and inverse w of each vertex in front of the near plane,
    // projected once instead of once per triangle that uses it.
    std::vector<std::vector<glm::vec3>> screens(occluders.size());
    for (size_t index = 0; index < occluders.size(); ++index)
    {
        if (!active[index]) continue;
        const Occluder& occluder = occluders[index];
        clips[index].resize(occluder.vertexCount);
        screens[index].resize(occluder.vertexCount);
        for (size_t begin = 0; begin < occluder.vertexCount; begin += VertexChunk)
            vertexChunks.push_back({ index, begin, std::min(occluder.vertexCount, begin + VertexChunk) });
        const size_t triangleCount = occluder.indexCount / 3;
        for (size_t begin = 0; begin < triangleCount; begin += TriangleChunk)
            triangleChunks.push_back({ index, begin, std::min(triangleCount, begin + TriangleChunk) });
    }
    RenderJobs::Get().For(vertexChunks.size(), 1, [&](size_t chunkIndex)
    {
        const Chunk& chunk = vertexChunks[chunkIndex];
        const Occluder& occluder = occluders[chunk.occluder];
        const glm::mat4& transform = transforms[chunk.occluder];
        glm::vec4* clip = clips[chunk.occluder].data();
        glm::vec3* screen = screens[chunk.occluder].data();
        const auto* bytes = static_cast<const uint8_t*>(occluder.vertices);
        for (size_t vertex = chunk.begin; vertex < chunk.end; ++vertex)
        {
            float position[3];
            std::memcpy(position, bytes + vertex * occluder.vertexStride, sizeof(position));
            const glm::vec4 projected = transform * glm::vec4(position[0], position[1], position[2], 1.f);
            clip[vertex] = projected;
            if (projected.w >= NearW)
            {
                const float inverseW = 1.f / projected.w;
                screen[vertex] = { (projected.x * inverseW * .5f + .5f) * width,
                    (.5f - projected.y * inverseW * .5f) * height, inverseW };
            }
        }
    });

    // Clip and set up each chunk's triangles and list them by band.
    std::vector<OccluderTriangles> outputs(triangleChunks.size());
    RenderJobs::Get().For(triangleChunks.size(), 1, [&](size_t chunkIndex)
    {
        const Chunk& chunk = triangleChunks[chunkIndex];
        const Occluder& occluder = occluders[chunk.occluder];
        const std::vector<glm::vec4>& clip = clips[chunk.occluder];
        const std::vector<glm::vec3>& screen = screens[chunk.occluder];
        thread_local std::vector<uint32_t> nextInBand;
        auto& output = outputs[chunkIndex];
        output.triangles.reserve(chunk.end - chunk.begin);
        for (size_t base = chunk.begin * 3; base < chunk.end * 3; base += 3)
        {
            const uint32_t i0 = occluder.indices[base], i1 = occluder.indices[base + 1],
                i2 = occluder.indices[base + 2];
            if (i0 >= clip.size() || i1 >= clip.size() || i2 >= clip.size()) continue;
            if (clip[i0].w >= NearW && clip[i1].w >= NearW && clip[i2].w >= NearW)
            {
                // Wholly in front of the near plane: the projected corners
                // are ready, and SetupTriangle rejects anything off screen.
                const float fx[3] = { screen[i0].x, screen[i1].x, screen[i2].x };
                const float fy[3] = { screen[i0].y, screen[i1].y, screen[i2].y };
                const float fw[3] = { screen[i0].z, screen[i1].z, screen[i2].z };
                ScreenTriangle triangle;
                if (SetupTriangle(fx, fy, fw, Width, m_height, triangle))
                    output.triangles.push_back(triangle);
                continue;
            }
            const glm::vec4 polygon[3] = { clip[i0], clip[i1], clip[i2] };
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
                const float fx[3] = { sx[0], sx[fan], sx[fan + 1] };
                const float fy[3] = { sy[0], sy[fan], sy[fan + 1] };
                const float fw[3] = { iw[0], iw[fan], iw[fan + 1] };
                ScreenTriangle triangle;
                if (SetupTriangle(fx, fy, fw, Width, m_height, triangle))
                    output.triangles.push_back(triangle);
            }
        }
        // Counting sort: each triangle is listed in every band it touches.
        output.bandStart.assign(static_cast<size_t>(bands) + 1, 0);
        for (const auto& triangle : output.triangles)
            for (int band = triangle.y0 / BandRows; band <= triangle.y1 / BandRows; ++band)
                ++output.bandStart[static_cast<size_t>(band) + 1];
        for (int band = 0; band < bands; ++band)
            output.bandStart[static_cast<size_t>(band) + 1] += output.bandStart[band];
        output.bandTriangles.resize(output.bandStart.back());
        nextInBand.assign(output.bandStart.begin(), output.bandStart.end() - 1);
        for (uint32_t triangle = 0; triangle < output.triangles.size(); ++triangle)
        {
            const auto& screen = output.triangles[triangle];
            for (int band = screen.y0 / BandRows; band <= screen.y1 / BandRows; ++band)
                output.bandTriangles[nextInBand[band]++] = triangle;
        }
    });

    // Rasterize the nearest depth, as the largest inverse w, in horizontal
    // bands; each band owns its rows.
    std::vector<float> inverseDepth(static_cast<size_t>(Width) * m_height, 0.f);
    RenderJobs::Get().For(static_cast<size_t>(bands), 1, [&](size_t band)
    {
        const int rowStart = static_cast<int>(band) * BandRows;
        const int rowEnd = std::min(m_height, rowStart + BandRows);
        for (const auto& output : outputs)
        {
            if (output.bandStart.empty()) continue;
            for (uint32_t entry = output.bandStart[band]; entry < output.bandStart[band + 1]; ++entry)
                RasterizeRows(output.triangles[output.bandTriangles[entry]], rowStart, rowEnd,
                    inverseDepth.data());
        }
    });

    // Erode: each pixel takes the farthest depth of its 3x3 neighbourhood,
    // so partly covered silhouette pixels never hide anything. In inverse w
    // the farthest is the smallest; level 0 stores depth again.
    std::vector<float> horizontal(inverseDepth.size());
    for (int y = 0; y < m_height; ++y)
        for (int x = 0; x < Width; ++x)
        {
            const size_t index = static_cast<size_t>(y) * Width + x;
            float value = inverseDepth[index];
            if (x > 0) value = std::min(value, inverseDepth[index - 1]);
            if (x + 1 < Width) value = std::min(value, inverseDepth[index + 1]);
            horizontal[index] = value;
        }
    m_levels.clear();
    m_levelSizes.clear();
    m_levels.emplace_back(inverseDepth.size());
    auto& level0 = m_levels.front();
    for (int y = 0; y < m_height; ++y)
        for (int x = 0; x < Width; ++x)
        {
            const size_t index = static_cast<size_t>(y) * Width + x;
            float value = horizontal[index];
            if (y > 0) value = std::min(value, horizontal[index - Width]);
            if (y + 1 < m_height) value = std::min(value, horizontal[index + Width]);
            level0[index] = value > 0.f ? 1.f / value : Far;
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
    for (const auto& output : outputs) m_statistics.rasterizedTriangles += output.triangles.size();
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

std::shared_ptr<const OcclusionBuffer> OcclusionBufferCache::Acquire(const glm::mat4& viewProjection,
    float aspect, const std::vector<OcclusionBuffer::Occluder>& occluders, bool& reused)
{
    std::lock_guard lock(m_mutex);
    const auto same = [&]
    {
        if (!m_buffer || aspect != m_aspect || occluders.size() != m_keys.size() ||
            std::memcmp(&viewProjection, &m_viewProjection, sizeof(glm::mat4)) != 0) return false;
        for (size_t index = 0; index < occluders.size(); ++index)
        {
            const auto& occluder = occluders[index];
            const Key& key = m_keys[index];
            if (occluder.vertices != key.vertices || occluder.vertexStride != key.vertexStride ||
                occluder.vertexCount != key.vertexCount || occluder.indices != key.indices ||
                occluder.indexCount != key.indexCount || occluder.revision != key.revision ||
                std::memcmp(&occluder.world, &key.world, sizeof(glm::mat4)) != 0) return false;
        }
        return true;
    };
    reused = same();
    if (reused) return m_buffer;
    auto buffer = std::make_shared<OcclusionBuffer>();
    buffer->Build(viewProjection, aspect, occluders);
    m_buffer = buffer;
    m_viewProjection = viewProjection;
    m_aspect = aspect;
    m_keys.clear();
    for (const auto& occluder : occluders)
        m_keys.push_back({ occluder.vertices, occluder.vertexStride, occluder.vertexCount,
            occluder.indices, occluder.indexCount, occluder.world, occluder.revision });
    return buffer;
}

void OcclusionBufferCache::Clear()
{
    std::lock_guard lock(m_mutex);
    m_buffer.reset();
    m_keys.clear();
}
}

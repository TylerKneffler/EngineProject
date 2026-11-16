#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>
#include <glm/glm.hpp>

namespace Engine::Rendering
{
// Software occlusion buffer for one camera view, built on the CPU for the
// current frame (no one-frame lag).
//
// Occluder triangles are rasterized at low resolution, storing the nearest
// view depth (clip w) per pixel. The result is made conservative by
// eroding it: every pixel takes the farthest depth of its 3x3
// neighbourhood. Silhouette pixels the occluders only partly cover then
// never hide anything behind them. A max-depth mip pyramid answers bounds
// queries with at most a few reads.
//
// A bounds query is hidden only when its nearest point lies behind the
// farthest occluder depth over its whole screen rectangle. Bounds that
// cross the near plane or leave the screen are reported visible; the
// frustum test decides those.
class OcclusionBuffer
{
public:
    static constexpr int Width = 256;

    struct Occluder
    {
        // Positions are read as three floats at the start of each vertex.
        const void* vertices = nullptr;
        size_t vertexStride = 0;
        size_t vertexCount = 0;
        const uint32_t* indices = nullptr;
        size_t indexCount = 0;
        glm::mat4 world{ 1.f };
    };

    struct Statistics
    {
        uint64_t occluders = 0, triangles = 0, rasterizedTriangles = 0;
        double buildMilliseconds = 0.0;
    };

    // aspect = viewport width / height; the buffer keeps it.
    void Build(const glm::mat4& viewProjection, float aspect,
        const std::vector<Occluder>& occluders);

    bool Valid() const { return m_valid; }
    int Height() const { return m_height; }
    const Statistics& GetStatistics() const { return m_statistics; }

    // World-space axis-aligned box.
    bool IsBoxHidden(const glm::vec3& minimum, const glm::vec3& maximum) const;
    bool IsSphereHidden(const glm::vec3& center, float radius) const;

    // GPU copy for the instance cull (Shaders/InstanceCull/InstanceCull.hlsl,
    // IsPlacementOccluded): one float array.
    //   [0] level count (0 = no occlusion), [1] width, [2] height
    //   [4 + 3 * level] data offset, width, height (up to GpuMaximumLevels)
    //   [64..79] view-projection matrix, row-major
    //   [80..] level data
    static constexpr size_t GpuHeaderFloats = 80;
    static constexpr size_t GpuMaximumLevels = 16;
    void SerializeForGpu(std::vector<float>& output) const;
    static size_t GpuFloatCount(int height);

    // The game view publishes what its buffer did each frame (benchmark log).
    struct FrameStatistics
    {
        uint64_t occluders = 0, triangles = 0, rasterizedTriangles = 0;
        uint32_t tested = 0, hidden = 0;
        double buildMilliseconds = 0.0;
    };
    static void PublishFrameStatistics(const FrameStatistics& statistics);
    static FrameStatistics LastFrameStatistics();

    // Level 0 after erosion, row-major Width x Height; for tests and debug views.
    const std::vector<float>& Depth() const { return m_levels.empty() ? m_empty : m_levels.front(); }

private:
    bool IsRectangleHidden(float x0, float y0, float x1, float y1, float nearest) const;

    glm::mat4 m_viewProjection{ 1.f };
    int m_height = 0;
    bool m_valid = false;
    std::vector<std::vector<float>> m_levels;
    std::vector<glm::ivec2> m_levelSizes;
    std::vector<float> m_empty;
    Statistics m_statistics;
};
}

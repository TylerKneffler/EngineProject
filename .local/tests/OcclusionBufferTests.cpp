#include "Core/Rendering/OcclusionBuffer.h"

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

using Engine::Rendering::OcclusionBuffer;
using Engine::Rendering::OcclusionBufferCache;

namespace
{
void Require(bool value, const char* what)
{
    if (value) return;
    std::fprintf(stderr, "OcclusionBufferTests failed: %s\n", what);
    std::abort();
}

constexpr float Aspect = 16.f / 9.f;

// Camera at the origin looking down +z (left-handed, reverse-Z depth with
// near and far swapped, as Camera::GetRenderProjectionMatrix).
glm::mat4 ViewProjection(const glm::vec3& eye = glm::vec3(0.f), const glm::vec3& forward = { 0.f, 0.f, 1.f })
{
    return glm::perspectiveLH_ZO(glm::radians(60.f), Aspect, 1000.f, .1f) *
        glm::lookAtLH(eye, eye + forward, glm::vec3(0.f, 1.f, 0.f));
}

// A quad in the plane z = depth covering [x0, x1] x [y0, y1], as two
// triangles in both windings.
struct Quad
{
    std::vector<glm::vec3> vertices;
    std::vector<uint32_t> indices{ 0, 1, 2, 0, 2, 3 };
    Quad(float x0, float y0, float x1, float y1, float depth)
        : vertices{ { x0, y0, depth }, { x1, y0, depth }, { x1, y1, depth }, { x0, y1, depth } } {}
    OcclusionBuffer::Occluder Occluder(bool bounds = false) const
    {
        OcclusionBuffer::Occluder occluder;
        occluder.vertices = vertices.data();
        occluder.vertexStride = sizeof(glm::vec3);
        occluder.vertexCount = vertices.size();
        occluder.indices = indices.data();
        occluder.indexCount = indices.size();
        if (bounds)
        {
            occluder.hasBounds = true;
            occluder.boundsMinimum = vertices[0];
            occluder.boundsMaximum = vertices[2];
        }
        return occluder;
    }
};

bool BoxHidden(const OcclusionBuffer& buffer, const glm::vec3& center, float half)
{
    return buffer.IsBoxHidden(center - glm::vec3(half), center + glm::vec3(half));
}
}

int main()
{
    const glm::mat4 viewProjection = ViewProjection();

    // A wall filling the view at z = 10.
    {
        const Quad wall(-100.f, -100.f, 100.f, 100.f, 10.f);
        OcclusionBuffer buffer;
        buffer.Build(viewProjection, Aspect, { wall.Occluder() });
        Require(buffer.Valid() && buffer.Height() == 144, "wall buffer valid");
        Require(buffer.GetStatistics().triangles == 2 && buffer.GetStatistics().rasterizedTriangles == 2,
            "wall statistics");
        // Depth is clip w: every pixel of the plane z = 10 stores 10.
        for (const float depth : buffer.Depth())
            Require(std::abs(depth - 10.f) < 1.e-3f, "wall depth is its view distance");
        Require(BoxHidden(buffer, { 0.f, 0.f, 20.f }, 1.f), "box behind the wall is hidden");
        Require(BoxHidden(buffer, { 3.f, -2.f, 50.f }, 4.f), "large box far behind the wall is hidden");
        Require(!BoxHidden(buffer, { 0.f, 0.f, 5.f }, 1.f), "box in front of the wall is visible");
        Require(!BoxHidden(buffer, { 0.f, 0.f, 10.f }, 1.f), "box crossing the wall is visible");
        Require(!BoxHidden(buffer, { 0.f, 0.f, 0.f }, 1.f), "box crossing the near plane is visible");
        Require(!BoxHidden(buffer, { 0.f, 0.f, -20.f }, 1.f), "box behind the camera is visible");
        Require(!BoxHidden(buffer, { 500.f, 0.f, 20.f }, 1.f), "box off screen is left to the frustum test");
        Require(buffer.IsSphereHidden({ 0.f, 0.f, 30.f }, 2.f), "sphere behind the wall is hidden");
    }

    // Silhouettes: a wall covering only the left half of the view. Erosion
    // keeps anything near its edge visible.
    {
        const Quad half(-100.f, -100.f, 0.f, 100.f, 10.f);
        OcclusionBuffer buffer;
        buffer.Build(viewProjection, Aspect, { half.Occluder() });
        Require(BoxHidden(buffer, { -10.f, 0.f, 20.f }, 1.f), "box well inside the silhouette is hidden");
        Require(!BoxHidden(buffer, { 10.f, 0.f, 20.f }, 1.f), "box beside the wall is visible");
        Require(!BoxHidden(buffer, { -.5f, 0.f, 20.f }, 1.f), "box straddling the silhouette is visible");
        // The edge pixel column lies at x = 128; erosion clears the column
        // the wall only half covers.
        const auto& depth = buffer.Depth();
        const int row = buffer.Height() / 2;
        Require(depth[row * OcclusionBuffer::Width + 120] < 11.f, "covered pixel keeps the wall depth");
        Require(depth[row * OcclusionBuffer::Width + 128] > 1.e30f, "pixel past the edge stays empty");
        Require(depth[row * OcclusionBuffer::Width + 127] > 1.e30f, "last covered pixel is eroded");
    }

    // Near plane: a floor reaching from behind the camera into the distance
    // is clipped, not dropped, and still hides what lies below it.
    {
        Quad floor(-100.f, 0.f, 100.f, 0.f, 0.f);
        floor.vertices = { { -100.f, -2.f, -50.f }, { 100.f, -2.f, -50.f }, { 100.f, -2.f, 200.f },
            { -100.f, -2.f, 200.f } };
        OcclusionBuffer buffer;
        buffer.Build(viewProjection, Aspect, { floor.Occluder() });
        Require(buffer.GetStatistics().rasterizedTriangles >= 2, "clipped floor is rasterized");
        Require(BoxHidden(buffer, { 0.f, -8.f, 30.f }, 1.f), "box under the floor is hidden");
        Require(!BoxHidden(buffer, { 0.f, 0.f, 30.f }, 1.f), "box above the floor is visible");
    }

    // An empty buffer (no occluders) hides nothing.
    {
        OcclusionBuffer buffer;
        Require(!buffer.Valid() && !BoxHidden(buffer, { 0.f, 0.f, 20.f }, 1.f), "unbuilt buffer hides nothing");
        buffer.Build(viewProjection, Aspect, {});
        Require(buffer.Valid() && !BoxHidden(buffer, { 0.f, 0.f, 20.f }, 1.f), "empty buffer hides nothing");
        std::vector<float> gpu;
        OcclusionBuffer().SerializeForGpu(gpu);
        Require(gpu.size() == OcclusionBuffer::GpuHeaderFloats && gpu[0] == 0.f, "empty GPU copy has no levels");
        buffer.SerializeForGpu(gpu);
        Require(gpu.size() == OcclusionBuffer::GpuFloatCount(buffer.Height()), "GPU copy size");
    }

    // Occluders with tiny or off-screen projected bounds are skipped; large
    // ones with bounds are drawn.
    {
        const Quad tiny(-.01f, -.01f, .01f, .01f, 10.f);
        const Quad offScreen(400.f, -1.f, 410.f, 1.f, 10.f);
        const Quad wall(-100.f, -100.f, 100.f, 100.f, 10.f);
        OcclusionBuffer buffer;
        buffer.Build(viewProjection, Aspect, { tiny.Occluder(true), offScreen.Occluder(true), wall.Occluder(true) });
        Require(buffer.GetStatistics().skippedOccluders == 2, "tiny and off-screen occluders skipped");
        Require(buffer.GetStatistics().rasterizedTriangles == 2, "wall with bounds still drawn");
        Require(BoxHidden(buffer, { 0.f, 0.f, 20.f }, 1.f), "wall with bounds hides");
    }

    // A per-occluder minimum: a 2 x 2 m block at 10 m spans about 25 buffer
    // pixels, kept at the default and skipped when 40 pixels are required.
    {
        const Quad block(-1.f, -1.f, 1.f, 1.f, 10.f);
        auto occluder = block.Occluder(true);
        OcclusionBuffer kept;
        kept.Build(viewProjection, Aspect, { occluder });
        Require(kept.GetStatistics().skippedOccluders == 0, "block above the default minimum is drawn");
        Require(BoxHidden(kept, { 0.f, 0.f, 20.f }, .2f), "block hides a small box behind it");
        occluder.minimumPixels = 40.f;
        OcclusionBuffer strict;
        strict.Build(viewProjection, Aspect, { occluder });
        Require(strict.GetStatistics().skippedOccluders == 1, "block below a raised minimum is skipped");
        Require(!BoxHidden(strict, { 0.f, 0.f, 20.f }, .2f), "skipped block hides nothing");
        // Wholly behind the camera: skipped before any vertex work, while a
        // wall crossing the near plane is kept and clipped.
        const Quad behind(-1.f, -1.f, 1.f, 1.f, -10.f);
        OcclusionBuffer back;
        back.Build(viewProjection, Aspect, { behind.Occluder(true) });
        Require(back.GetStatistics().skippedOccluders == 1, "occluder behind the camera is skipped");
        Quad crossing(0.f, 0.f, 0.f, 0.f, 0.f);
        crossing.vertices = { { -5.f, -5.f, -5.f }, { 5.f, -5.f, -5.f }, { 5.f, -5.f, 50.f }, { -5.f, -5.f, 50.f } };
        auto crossingOccluder = crossing.Occluder();
        crossingOccluder.hasBounds = true;
        crossingOccluder.boundsMinimum = { -5.f, -5.f, -5.f };
        crossingOccluder.boundsMaximum = { 5.f, -5.f, 50.f };
        OcclusionBuffer cross;
        cross.Build(viewProjection, Aspect, { crossingOccluder });
        Require(cross.GetStatistics().skippedOccluders == 0 && cross.GetStatistics().rasterizedTriangles > 0,
            "occluder crossing the near plane is kept");
    }

    // Many occluders across chunks and bands give the same result as one.
    {
        std::vector<Quad> strips;
        for (int strip = 0; strip < 40; ++strip)
            strips.emplace_back(-100.f, -100.f + strip * 5.f, 100.f, -95.f + strip * 5.f, 10.f);
        std::vector<OcclusionBuffer::Occluder> occluders;
        for (const auto& strip : strips) occluders.push_back(strip.Occluder());
        OcclusionBuffer buffer;
        buffer.Build(viewProjection, Aspect, occluders);
        Require(BoxHidden(buffer, { 0.f, 0.f, 20.f }, 1.f), "seamed strips hide like one wall");
        Require(!BoxHidden(buffer, { 0.f, 0.f, 5.f }, 1.f), "box before the strips is visible");
    }

    // Reuse: identical camera and occluders return the same buffer; a moved
    // camera, a moved occluder or a new revision rebuilds.
    {
        const Quad wall(-100.f, -100.f, 100.f, 100.f, 10.f);
        std::vector<OcclusionBuffer::Occluder> occluders{ wall.Occluder() };
        OcclusionBufferCache cache;
        bool reused = true;
        const auto first = cache.Acquire(viewProjection, Aspect, occluders, reused);
        Require(first && !reused, "first acquire builds");
        const auto same = cache.Acquire(viewProjection, Aspect, occluders, reused);
        Require(same == first && reused, "unchanged inputs reuse");
        const auto moved = cache.Acquire(ViewProjection({ 0.f, 0.f, .001f }), Aspect, occluders, reused);
        Require(moved != first && !reused, "moved camera rebuilds");
        occluders[0].revision = 7;
        cache.Acquire(ViewProjection({ 0.f, 0.f, .001f }), Aspect, occluders, reused);
        Require(!reused, "new revision rebuilds");
        occluders[0].world[3].x = 1.f;
        cache.Acquire(ViewProjection({ 0.f, 0.f, .001f }), Aspect, occluders, reused);
        Require(!reused, "moved occluder rebuilds");
        cache.Acquire(ViewProjection({ 0.f, 0.f, .001f }), Aspect, occluders, reused);
        Require(reused, "settled inputs reuse again");
        cache.Clear();
        cache.Acquire(ViewProjection({ 0.f, 0.f, .001f }), Aspect, occluders, reused);
        Require(!reused, "cleared cache rebuilds");
    }

    std::printf("OcclusionBufferTests passed\n");
    return 0;
}

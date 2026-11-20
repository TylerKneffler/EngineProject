// GPU occlusion pyramid from the scene depth (.md/todo.md Foliage Phase 2).
//
// The CPU occlusion buffer (Core/Rendering/OcclusionBuffer.h) rasterizes only
// terrain and a few mesh occluders, so at a forest edge the thousands of
// trees hidden behind the first rows still go through culling, the foliage
// depth pre-pass and shading. This pass reduces the camera's real depth (copied
// into a buffer for ambient occlusion, IGraphicsContext::CopySceneDepth) to the
// same pyramid layout (OcclusionBuffer::SerializeForGpu) that InstanceCull.hlsl
// IsBoxOccluded reads: per cell the farthest view depth (clip.w) of the pixels
// it covers, 3e38 where any pixel is sky. The next frame's camera culling
// tests against it (SceneRenderer binds it in place of the CPU pyramid when
// the camera has barely moved), so trees hidden by nearer trees are dropped.
//
// Layout (floats): [0] levels, [1] width, [2] height, [4 + 3 l] offset,
// [5 + 3 l] width, [6 + 3 l] height of level l, [64..79] view-projection
// (row-major), level data from 80. Every cell reads its whole pixel footprint
// directly from depth (no level-to-level dependency, so one dispatch writes all
// levels): each level reads the depth once, Levels levels in all.
//
// Buffers: t0 scene depth (float per pixel, row pitch extent.z), u0 pyramid.
// Vulkan SPIR-V is prebuilt by CMakeLists.txt (DepthPyramid.CS.spv).
// Vulkan: binding 0 = t0, 1 = u0.

struct PyramidConstants
{
    float4x4 viewProjection; // row-major
    float4 depth;            // A, B (view depth w = B / (depth - A)), unused, unused
    uint4 extent;            // full width, full height, depth row pitch, levels
    uint4 pyramid;           // level-0 width, height, total cells, unused
};

#ifdef VULKAN
[[vk::push_constant]] PyramidConstants cb;
[[vk::binding(0, 0)]] StructuredBuffer<float> sceneDepth;
[[vk::binding(1, 0)]] RWStructuredBuffer<float> pyramidOutput;
#else
cbuffer PyramidCB : register(b0) { PyramidConstants cb; };
StructuredBuffer<float> sceneDepth : register(t0);
RWStructuredBuffer<float> pyramidOutput : register(u0);
#endif

static const uint HeaderFloats = 80u;
static const float Sky = 3.0e38;

uint2 LevelSize(uint level)
{
    return uint2(max(1u, (cb.pyramid.x + (1u << level) - 1u) >> level),
        max(1u, (cb.pyramid.y + (1u << level) - 1u) >> level));
}

[numthreads(64, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    const uint levels = cb.extent.w;
    if (id.x == 0u)
    {
        pyramidOutput[0] = (float)levels;
        pyramidOutput[1] = (float)cb.pyramid.x;
        pyramidOutput[2] = (float)cb.pyramid.y;
        uint offset = HeaderFloats;
        for (uint l = 0u; l < levels; ++l)
        {
            const uint2 size = LevelSize(l);
            pyramidOutput[4u + l * 3u] = (float)offset;
            pyramidOutput[5u + l * 3u] = (float)size.x;
            pyramidOutput[6u + l * 3u] = (float)size.y;
            offset += size.x * size.y;
        }
        [unroll] for (uint row = 0u; row < 4u; ++row)
            [unroll] for (uint column = 0u; column < 4u; ++column)
                pyramidOutput[64u + row * 4u + column] = cb.viewProjection[row][column];
    }
    if (id.x >= cb.pyramid.z)
        return;
    // Which level and cell this thread owns.
    uint cell = id.x, level = 0u, offset = HeaderFloats;
    uint2 size = LevelSize(0u);
    [loop] while (level + 1u < levels && cell >= size.x * size.y)
    {
        cell -= size.x * size.y;
        offset += size.x * size.y;
        ++level;
        size = LevelSize(level);
    }
    const uint2 coordinate = uint2(cell % size.x, cell / size.x);
    // The cell's footprint in full-resolution pixels (level-0 cells tile the
    // screen; a level-l cell covers 2^l of them each way), rounded outward so
    // every pixel is counted.
    const float2 level0 = float2(cb.pyramid.xy);
    const float2 scale = float2(cb.extent.xy) / level0 * (float)(1u << level);
    const uint2 first = min((uint2)floor((float2)coordinate * scale), cb.extent.xy - 1u);
    const uint2 last = min((uint2)ceil((float2)(coordinate + 1u) * scale), cb.extent.xy);
    float farthest = 0.0;
    bool sky = false;
    [loop] for (uint y = first.y; y < last.y && !sky; ++y)
        [loop] for (uint x = first.x; x < last.x; ++x)
        {
            const float device = sceneDepth[y * cb.extent.z + x];
            // Reverse-Z: the clear (far plane) is 0; anything at or beyond
            // it is sky and leaves the cell unoccluding.
            if (device <= max(cb.depth.x, 0.0) + 1.0e-7) { sky = true; break; }
            farthest = max(farthest, cb.depth.y / (device - cb.depth.x));
        }
    pyramidOutput[offset + cell] = sky ? Sky : farthest;
}

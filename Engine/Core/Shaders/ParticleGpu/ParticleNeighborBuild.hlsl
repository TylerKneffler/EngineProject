// GPU counterpart of ParticleNeighborGrid. Buckets hold linked-list heads;
// nodes retain the dense source index so insertion order cannot affect results.
struct GpuParticle
{
    float4 positionAge;
    float4 velocityLife;
    float4 colorSize;
    uint4 meta;
};

struct GridRecord
{
    int4 cellNext;       // xyz cell, w next node or bucket head (-1 = empty)
    float4 positionRadius;
    uint4 metadata;      // x = source index (0xffffffff = inactive)
};

#ifdef VULKAN
struct Params { uint4 u0; float4 c1; };
[[vk::push_constant]] Params cb;
#define u0 cb.u0
#define c1 cb.c1
[[vk::binding(0, 0)]] StructuredBuffer<GpuParticle> source;
[[vk::binding(1, 0)]] RWStructuredBuffer<GridRecord> grid;
#else
cbuffer Params : register(b0) { uint4 u0; float4 c1; };
StructuredBuffer<GpuParticle> source : register(t0);
RWStructuredBuffer<GridRecord> grid : register(u0);
#endif

uint HashCell(int3 cell)
{
    uint hash = asuint(cell.x) * 73856093u ^
        asuint(cell.y) * 19349663u ^ asuint(cell.z) * 83492791u;
    hash ^= hash >> 16;
    return hash & (u0.y - 1u); // bucket count is a power of two
}

[numthreads(64, 1, 1)]
void CSClear(uint3 dispatchId : SV_DispatchThreadID)
{
    const uint index = dispatchId.x;
    if (index < u0.y) grid[index].cellNext.w = -1;
    if (index < u0.x) grid[u0.y + index].metadata.x = 0xffffffffu;
}

[numthreads(64, 1, 1)]
void CSBuild(uint3 dispatchId : SV_DispatchThreadID)
{
    const uint index = dispatchId.x;
    if (index >= min(source[0].meta.x, u0.x)) return;
    const GpuParticle particle = source[index + 1u];
    const float3 position = particle.positionAge.xyz;
    if (particle.meta.y == 0u || !all(isfinite(position))) return;
    const int3 cell = int3(clamp(floor(position / c1.x),
        -1000000000.0, 1000000000.0));
    const uint node = u0.y + index;
    grid[node].cellNext.xyz = cell;
    grid[node].positionRadius = float4(position, 0.0);
    grid[node].metadata.x = index;
    int previous;
    InterlockedExchange(grid[HashCell(cell)].cellNext.w, int(node), previous);
    grid[node].cellNext.w = previous;
}

// One thread per query. Bitsets encode source indices in ascending order
// when read back, regardless of the order of atomic bucket insertion.
struct GridRecord
{
    int4 cellNext;
    float4 positionRadius; // query xyz/radius at queryOffset + queryIndex
    uint4 metadata;        // query metadata.y = excluded source index
};

#ifdef VULKAN
struct Params { uint4 u0; float4 c1; };
[[vk::push_constant]] Params cb;
#define u0 cb.u0
#define c1 cb.c1
[[vk::binding(0, 0)]] StructuredBuffer<GridRecord> grid;
[[vk::binding(1, 0)]] RWStructuredBuffer<uint> results;
#else
cbuffer Params : register(b0) { uint4 u0; float4 c1; };
StructuredBuffer<GridRecord> grid : register(t0);
RWStructuredBuffer<uint> results : register(u0);
#endif

uint HashCell(int3 cell)
{
    uint hash = asuint(cell.x) * 73856093u ^
        asuint(cell.y) * 19349663u ^ asuint(cell.z) * 83492791u;
    hash ^= hash >> 16;
    return hash & (u0.y - 1u);
}

void Include(uint queryIndex, uint sourceIndex, float3 sourcePosition,
    float3 center, float radiusSquared, uint excludeIndex)
{
    if (sourceIndex == 0xffffffffu || sourceIndex == excludeIndex) return;
    const float3 delta = sourcePosition - center;
    if (dot(delta, delta) > radiusSquared) return;
    const uint base = queryIndex * u0.w + sourceIndex / 32u;
    results[base] |= 1u << (sourceIndex & 31u);
}

[numthreads(64, 1, 1)]
void CSQuery(uint3 dispatchId : SV_DispatchThreadID)
{
    const uint queryIndex = dispatchId.x;
    if (queryIndex >= u0.z) return;
    const uint outputBase = queryIndex * u0.w;
    for (uint word = 0; word < u0.w; ++word)
        results[outputBase + word] = 0u;

    const GridRecord query = grid[u0.y + u0.x + queryIndex];
    const float3 center = query.positionRadius.xyz;
    const float radius = query.positionRadius.w;
    if (!all(isfinite(center)) || !isfinite(radius) || radius < 0.0) return;
    const float radiusSquared = radius * radius;
    const uint excludeIndex = query.metadata.y;

    // Mirrors the CPU reference's direct scan for a radius above two cells.
    if (radius > 2.0 * c1.x)
    {
        for (uint index = 0; index < u0.x; ++index)
        {
            const GridRecord node = grid[u0.y + index];
            Include(queryIndex, node.metadata.x, node.positionRadius.xyz,
                center, radiusSquared, excludeIndex);
        }
        return;
    }

    const int3 low = int3(clamp(floor((center - radius) / c1.x),
        -1000000000.0, 1000000000.0));
    const int3 high = int3(clamp(floor((center + radius) / c1.x),
        -1000000000.0, 1000000000.0));
    for (int z = low.z; z <= high.z; ++z)
        for (int y = low.y; y <= high.y; ++y)
            for (int x = low.x; x <= high.x; ++x)
            {
                const int3 cell = int3(x, y, z);
                int nodeIndex = grid[HashCell(cell)].cellNext.w;
                for (uint steps = 0; nodeIndex >= 0 && steps < u0.x; ++steps)
                {
                    const GridRecord node = grid[nodeIndex];
                    if (all(node.cellNext.xyz == cell))
                        Include(queryIndex, node.metadata.x,
                            node.positionRadius.xyz, center, radiusSquared,
                            excludeIndex);
                    nodeIndex = node.cellNext.w;
                }
            }
}

// Dense GPU particle pool. Slot zero stores the live count in meta.x; live
// particles occupy slots 1..count. One update pass compacts survivors and a
// spawn pass appends new particles. The tail is the reusable free space.
struct GpuParticle
{
    float4 positionAge;
    float4 velocityLife;
    float4 colorSize;
    uint4 meta;
};

#ifdef VULKAN
struct ComputeParams
{
    uint4 u0;
    float4 c1, c2, c3, c4, c5, c6, c7;
};
[[vk::push_constant]] ComputeParams cb;
#define u0 cb.u0
#define c1 cb.c1
#define c2 cb.c2
#define c3 cb.c3
#define c4 cb.c4
#define c5 cb.c5
#define c6 cb.c6
#define c7 cb.c7
[[vk::binding(0, 0)]] StructuredBuffer<GpuParticle> source;
[[vk::binding(1, 0)]] RWStructuredBuffer<GpuParticle> target;
#else
cbuffer ComputeParams : register(b0)
{
    uint4 u0;
    float4 c1, c2, c3, c4, c5, c6, c7;
};
StructuredBuffer<GpuParticle> source : register(t0);
RWStructuredBuffer<GpuParticle> target : register(u0);
#endif

uint AllocateSlot(uint capacity)
{
    uint observed = target[0].meta.x;
    while (observed < capacity)
    {
        uint previous;
        InterlockedCompareExchange(target[0].meta.x, observed,
            observed + 1, previous);
        if (previous == observed) return observed + 1;
        observed = previous;
    }
    return 0;
}

[numthreads(1, 1, 1)]
void CSClear(uint3 dispatchId : SV_DispatchThreadID)
{
    target[0].meta.x = 0;
}

[numthreads(64, 1, 1)]
void CSUpdate(uint3 dispatchId : SV_DispatchThreadID)
{
    const uint index = dispatchId.x;
    const uint capacity = u0.x;
    if (index >= min(source[0].meta.x, capacity)) return;
    GpuParticle particle = source[index + 1];
    if (particle.meta.y == 0) return;
    const float dt = c1.x;
    particle.velocityLife.xyz =
        (particle.velocityLife.xyz + c2.xyz * dt) *
        exp(-max(0.0, c1.y) * dt);
    particle.positionAge.xyz += particle.velocityLife.xyz * dt;
    particle.positionAge.w += dt;
    if (particle.positionAge.w >= particle.velocityLife.w) return;
    const uint destination = AllocateSlot(capacity);
    if (destination != 0) target[destination] = particle;
}

uint Hash(uint value)
{
    value ^= value >> 16;
    value *= 0x7feb352du;
    value ^= value >> 15;
    value *= 0x846ca68bu;
    return value ^ (value >> 16);
}

float Random01(uint value)
{
    return (Hash(value) >> 8) * (1.0 / 16777216.0);
}

[numthreads(64, 1, 1)]
void CSSpawn(uint3 dispatchId : SV_DispatchThreadID)
{
    const uint index = dispatchId.x;
    if (index >= u0.x) return;
    const uint destination = AllocateSlot(u0.y);
    if (destination == 0) return;
    const uint id = u0.z + index;
    const uint randomBase = id * 1664525u + u0.w * 1013904223u;
    float3 localPosition = 0.0;
    const uint shape = (uint)c4.w;
    if (shape == 1)
    {
        localPosition = (float3(Random01(randomBase),
            Random01(randomBase + 1u), Random01(randomBase + 2u)) *
            2.0 - 1.0) * abs(c6.xyz);
    }
    else if (shape == 2)
    {
        const float z = 2.0 * Random01(randomBase) - 1.0;
        const float angle = 6.28318530718 * Random01(randomBase + 1u);
        const float radius = pow(Random01(randomBase + 2u), 1.0 / 3.0);
        const float xy = sqrt(max(0.0, 1.0 - z * z));
        localPosition = radius * max(0.0, c5.w) *
            float3(xy * cos(angle), z, xy * sin(angle));
    }
    const float3 axis = normalize(dot(c5.xyz, c5.xyz) > 1e-8
        ? c5.xyz : float3(0.0, 1.0, 0.0));
    const float cone = clamp(c3.w, 0.0, 180.0) * 0.01745329252;
    const float cosine = 1.0 - Random01(randomBase + 3u) *
        (1.0 - cos(cone));
    const float sine = sqrt(max(0.0, 1.0 - cosine * cosine));
    const float phi = 6.28318530718 * Random01(randomBase + 4u);
    const float3 tangent = normalize(cross(axis,
        abs(axis.y) < 0.9 ? float3(0.0, 1.0, 0.0) : float3(1.0, 0.0, 0.0)));
    const float3 bitangent = cross(axis, tangent);
    const float3 localVelocity = max(0.0, c1.w) *
        (axis * cosine + sine * (tangent * cos(phi) +
            bitangent * sin(phi)));
    GpuParticle particle;
    particle.positionAge = float4(c4.xyz + c1.xyz * localPosition.x +
        c2.xyz * localPosition.y + c3.xyz * localPosition.z, 0.0);
    particle.velocityLife = float4(c1.xyz * localVelocity.x +
        c2.xyz * localVelocity.y + c3.xyz * localVelocity.z,
        max(0.001, c2.w));
    particle.colorSize = float4(c7.xyz, max(0.0, c6.w));
    particle.meta = uint4(id, 1u, 0u, 0u);
    target[destination] = particle;
}

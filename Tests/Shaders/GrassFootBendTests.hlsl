#include "../../Engine/Core/Shaders/GrassFootBend.hlsli"
RWStructuredBuffer<float4> results : register(u0);
[numthreads(8, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    float4 capsules[8];
    [unroll] for (uint i = 0; i < 8; ++i) capsules[i] = 0;
    capsules[0] = float4(0, .5, 0, .5);
    capsules[1] = float4(0, 1.5, 0, .65);
    uint count = 1;
    float3 root = float3(.2, 0, 0);
    float height = id.x == 0 ? 0 : 1;
    if (id.x == 2) root.x = 4;
    if (id.x == 3) { capsules[0].y += 5; capsules[1].y += 5; }
    if (id.x == 4) count = 0;
    if (id.x == 5) root.x = 0;
    if (id.x == 6) root.x = .8;
    if (id.x == 7)
    {
        root.x = 0;
        count = 2;
        capsules[0].x = -.2; capsules[1].x = -.2;
        capsules[2] = float4(.2, .5, 0, .5);
        capsules[3] = float4(.2, 1.5, 0, .65);
    }
    float3 rest = root + float3(0, height, 0);
    float3 normal = normalize(float3(1, .2, 0));
    float3 tangent = float3(0, 0, 1);
    float3 position = rest;
    ApplyGrassFootBend(capsules, count, root, float3(0, 1, 0), height,
        rest, normal, tangent, position, normal, tangent);
    results[id.x * 2] = float4(position, 1);
    results[id.x * 2 + 1] = float4(normal, 1);
}

// Water material shader: a flat surface whose normal is animated with
// procedural waves (no textures). The engine surface path supplies lighting
// (sun, shadows, sun glint from the low roughness); this shader perturbs the
// normal, adds a Fresnel sky reflection and makes grazing views opaque.
// Needs "shaderUsesTime": true in the material so ObjectData carries scene
// time in grassAnimation.x.
#define PSMain EngineSurfacePS
#include "../../Shaders/Object/Object.hlsl"
#undef PSMain

float2 WaterHash2(float2 p)
{
    p = float2(dot(p, float2(127.1, 311.7)), dot(p, float2(269.5, 183.3)));
    return frac(sin(p) * 43758.5453) * 2.0 - 1.0;
}

// Gradient noise with analytic derivatives: x = value, yz = d/dx, d/dy.
float3 WaterNoise(float2 p)
{
    const float2 i = floor(p);
    const float2 f = frac(p);
    const float2 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
    const float2 du = 30.0 * f * f * (f * (f - 2.0) + 1.0);
    const float2 ga = WaterHash2(i);
    const float2 gb = WaterHash2(i + float2(1, 0));
    const float2 gc = WaterHash2(i + float2(0, 1));
    const float2 gd = WaterHash2(i + float2(1, 1));
    const float va = dot(ga, f);
    const float vb = dot(gb, f - float2(1, 0));
    const float vc = dot(gc, f - float2(0, 1));
    const float vd = dot(gd, f - float2(1, 1));
    const float value = va + u.x * (vb - va) + u.y * (vc - va) +
        u.x * u.y * (va - vb - vc + vd);
    const float2 derivative = ga + u.x * (gb - ga) + u.y * (gc - ga) +
        u.x * u.y * (ga - gb - gc + gd) +
        du * (u.yx * (va - vb - vc + vd) + float2(vb, vc) - va);
    return float3(value, derivative);
}

// Surface slope (dh/dx, dh/dz) of the animated water height at a point.
// Each wave fades out once it is smaller than a few pixels, so distant water
// settles to a calm, unaliased surface instead of shimmering.
float2 WaterSlope(float2 xz, float time, float footprint)
{
    float2 slope = 0.0;
    // Directional swells (deep-water dispersion: omega = sqrt(g k)).
    const float2 directions[4] = {
        float2(0.958, 0.287), float2(-0.371, 0.928),
        float2(0.753, -0.658), float2(-0.981, -0.196) };
    const float lengths[4] = { 13.0, 7.1, 4.3, 2.6 };
    // Faint: regular sines are what the eye picks out as a repeating pattern.
    const float heights[4] = { 0.012, 0.007, 0.0, 0.0 };
    [unroll] for (uint i = 0u; i < 4u; ++i)
    {
        const float k = 6.2831853 / lengths[i];
        const float omega = sqrt(9.81 * k);
        const float fade = saturate(lengths[i] / (6.0 * footprint) - 0.5);
        const float phase = dot(directions[i], xz) * k - omega * time;
        slope += directions[i] * (heights[i] * k * cos(phase) * fade);
    }
    // Gusts: large patches (tens of metres) of stronger and calmer wind that
    // drift slowly, so neighbouring stretches of water never look alike.
    const float gustA = WaterNoise(xz * 0.011 + float2(0.013, 0.007) * time).x;
    const float gustB = WaterNoise(xz * 0.037 + float2(-0.02, 0.031) * time + 17.0).x;
    const float gust = saturate(0.55 + 0.9 * gustA + 0.45 * gustB);
    // Wind ripples: three drifting octaves of gradient noise, each rotated
    // against the last, scaled by the gusts.
    const float rippleScales[3] = { 0.35, 0.9, 2.3 };
    const float rippleHeights[3] = { 0.09, 0.04, 0.016 };
    const float2 rippleDrift[3] = { float2(0.12, 0.07), float2(0.31, 0.17), float2(-0.22, 0.41) };
    [unroll] for (uint j = 0u; j < 3u; ++j)
    {
        const float scale = rippleScales[j];
        const float fade = saturate(1.0 / (scale * 6.0 * footprint) - 0.5);
        const float angle = 0.9 * j;
        const float2x2 rotation = float2x2(cos(angle), -sin(angle), sin(angle), cos(angle));
        float3 noise = WaterNoise(mul(rotation, xz) * scale + rippleDrift[j] * time);
        noise.yz = mul(noise.yz, rotation);
        slope += noise.yz * (rippleHeights[j] * scale * fade * (0.25 + 1.1 * gust));
    }
    return slope;
}

// Simple sky for reflections: brighter haze at the horizon, deeper blue up.
float3 WaterSkyColor(float3 direction)
{
    const float up = saturate(direction.y);
    const float3 horizon = float3(0.50, 0.64, 0.80);
    const float3 zenith = float3(0.18, 0.34, 0.66);
    return lerp(horizon, zenith, sqrt(up));
}

float4 PSMain(
    float4 pos : SV_POSITION, float3 worldPos : TEXCOORD2,
    float3 normal : NORMAL, float2 uv : TEXCOORD, float2 uv1 : TEXCOORD1,
    float4 vertexColor : COLOR, float4 tangent : TANGENT,
    float4 uvChannels0 : TEXCOORD3, float4 uvChannels1 : TEXCOORD4,
    float4 uvChannels2 : TEXCOORD5, float4 uvChannels3 : TEXCOORD6,
    float4 uvChannels4 : TEXCOORD7, nointerpolation uint objectIndex : TEXCOORD11,
    bool frontFace : SV_IsFrontFace,
    uint primitiveId : SV_PrimitiveID) : SV_TARGET
{
    g_primitiveId = primitiveId; // editor Triangles view
    ObjectData objectData = objects[(draw.drawFlags & 0x13080000u) != 0u
        ? objectIndex : draw.objectIndex];
    const float time = objectData.grassAnimation.x;
    const float3 toEye = objectData.viewPositionAlphaCutoff.xyz - worldPos;
    const float3 view = normalize(toEye);
    // Metres covered by one pixel on the water.
    const float footprint = max(length(fwidth(worldPos.xz)), 0.001);

    // Grazing distant water needs steeper slopes to vary the reflected sky
    // at all; up close the true slopes read best.
    const float2 slope = WaterSlope(worldPos.xz, time, footprint) *
        (1.0 + 1.6 * smoothstep(25.0, 300.0, length(toEye)));
    const float3 waveNormal = normalize(float3(-slope.x, 1.0, -slope.y));

    // Engine lighting of the water body (dark base color, sun glint).
    const float4 surface = EngineSurfacePS(pos, worldPos, waveNormal, uv, uv1,
        vertexColor, tangent, uvChannels0, uvChannels1, uvChannels2,
        uvChannels3, uvChannels4, objectIndex, frontFace);
    if (IsTriangleDraw(objects[g_triangleObject]))
        return surface;

    // Schlick Fresnel for water (F0 = 0.02) and the reflected sky.
    const float facing = saturate(dot(waveNormal, view));
    const float fresnel = 0.02 + 0.98 * pow(1.0 - facing, 5.0);
    float3 reflected = reflect(-view, waveNormal);
    reflected.y = abs(reflected.y);
    const float3 sky = WaterSkyColor(reflected);

    const float3 color = surface.rgb + sky * (fresnel * 1.6);
    // Steep views show the lake bed through the material's alpha; grazing
    // views are mostly reflection, and distant water reads as deep.
    const float distanceOpacity = smoothstep(20.0, 160.0, length(toEye));
    const float alpha = lerp(surface.a, 1.0,
        saturate(max(fresnel * 3.0, distanceOpacity * 0.85)));
    return ApplyObjectAtmosphereFog(float4(color, alpha), worldPos,
        objectIndex);
}

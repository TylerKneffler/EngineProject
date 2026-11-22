// Material shader for GPU grass blades (Rendering/GpuGrassBlades.h). The
// carrier mesh is one blade template (uv.x across 0..1, uv.y root to tip); each
// instance is a blade record written by Shaders/GrassBlades/GrassBlades.hlsl.
// The vertex shader shapes the blade (a bent quadratic curve with the meadow
// generator's leaf outline), widens it when seen edge-on (Ghost of Tsushima's
// view-space widening) and to the grass minimum width in pixels, colours it
// with the generator's palettes and bends it in the shared grass wind. The
// pixel shader is the meadow's (PSGrassMain), so shading, depth in the mass
// and colour variation stay identical to the mesh grass.
#define GRASS_SURFACE_SHADING 1
#define VSMain VSObjectMain
#include "Foliage.hlsl"
#undef VSMain

// tools/generate_meadow_grass.py palettes (BLADE_*, TALL_*, DRY_TIP).
static const float3 BladeRoot = float3(0.03, 0.065, 0.018);
static const float3 BladeMid = float3(0.065, 0.125, 0.032);
static const float3 BladeTip = float3(0.115, 0.175, 0.055);
static const float3 TallRoot = float3(0.016, 0.036, 0.011);
static const float3 TallMid = float3(0.05, 0.10, 0.026);
static const float3 TallTip = float3(0.16, 0.22, 0.07);
static const float3 DryTip = float3(0.30, 0.26, 0.13);

// generate_meadow_grass.py blade_width: a narrow sheath, widest a fifth of
// the way up, parallel margins, then a long tapering tip.
float BladeWidthProfile(float t)
{
    const float sheath = 0.55 + 0.45 * smoothstep(0.0, 0.2, t);
    const float taper = t < 0.55 ? 1.0 : pow(max(0.0, (1.0 - t) / 0.45), 0.75);
    return sheath * taper;
}

void VSMain(
    float3 pos : POSITION,
    float3 normal : NORMAL,
    float2 uv : TEXCOORD,
    float4 tangent : TANGENT,
    float2 uv1 : TEXCOORD1,
    float4 color : COLOR,
    float4 joints0 : JOINTS0,
    float4 weights0 : WEIGHTS0,
    float4 joints1 : JOINTS1,
    float4 weights1 : WEIGHTS1,
    float4 uvChannels0 : TEXCOORD3,
    float4 uvChannels1 : TEXCOORD4,
    float4 uvChannels2 : TEXCOORD5,
    float4 uvChannels3 : TEXCOORD6,
    float4 uvChannels4 : TEXCOORD7,
    float4 sdefCenterAndMode : TEXCOORD8,
    float4 sdefRadius0 : TEXCOORD9,
    float4 sdefRadius1 : TEXCOORD10,
    uint vertexId : SV_VertexID,
    uint instanceId : SV_InstanceID,
    out float4 oPos : SV_POSITION,
    out float3 oWorldPos : TEXCOORD2,
    out float3 oNormal : NORMAL,
    out float2 oUv : TEXCOORD,
    out float2 oUv1 : TEXCOORD1,
    out float4 oColor : COLOR,
    out float4 oTangent : TANGENT,
    out float4 oUvChannels0 : TEXCOORD3,
    out float4 oUvChannels1 : TEXCOORD4,
    out float4 oUvChannels2 : TEXCOORD5,
    out float4 oUvChannels3 : TEXCOORD6,
    out float4 oUvChannels4 : TEXCOORD7,
    out nointerpolation uint oObjectIndex : TEXCOORD11,
    out float oInstanceLodDistance : TEXCOORD12)
{
    const InstanceTransform record = meshInstances[instanceId + 1];
    const uint4 r0 = asuint(record.row0), r1 = asuint(record.row1), r2 = asuint(record.row2);
    oObjectIndex = draw.objectIndex + r0.x;
    const ObjectData objectData = objects[oObjectIndex];
    const float3 root = asfloat(r0.yzw);
    const float height = f16tof32(r1.x & 0xffffu), width = f16tof32(r1.x >> 16);
    const float yaw = f16tof32(r1.y & 0xffffu), bend = f16tof32(r1.y >> 16);
    const bool tall = (r1.z & 0xffu) > 127u;
    const float shade = (float)((r1.z >> 8) & 0xffu) / 255.0;
    const bool dry = ((r1.z >> 16) & 0xffu) > 127u;
    const float hue = (float)(r1.z >> 24) / 255.0;
    const float2 lean = float2(f16tof32(r1.w & 0xffffu), f16tof32(r1.w >> 16));
    const float phase = asfloat(r2.x);
    const float3 terrainNormal = float3(f16tof32(r2.y & 0xffffu), 0.0, f16tof32(r2.y >> 16));

    const float t = saturate(uv.y);
    const float side = uv.x - 0.5;
    // Blades stand mostly upright, a little with the ground's slope.
    const float3 up = normalize(float3(0.0, 1.0, 0.0) + terrainNormal * 0.3);
    float3 facing = float3(sin(yaw), 0.0, cos(yaw));
    facing = normalize(facing - up * dot(facing, up));
    // A quadratic curve: upright core, tip bent out by `bend` and the clump's lean.
    const float3 tipDirection = normalize(up * cos(bend) + facing * sin(bend) +
        float3(lean.x, 0.0, lean.y));
    const float3 control = root + up * (height * 0.55);
    const float3 tip = root + tipDirection * height;
    const float3 a = lerp(root, control, t), b = lerp(control, tip, t);
    const float3 centre = lerp(a, b, t);
    float3 along = b - a;
    along = dot(along, along) > 1.0e-10 ? normalize(along) : up;
    float3 across = normalize(cross(up, facing));
    // Edge-on blades turn their width toward the screen and widen a little,
    // so a field seen side-on does not thin to lines.
    float3 toCamera = objectData.viewPositionAlphaCutoff.xyz - centre;
    const float cameraDistance = max(length(toCamera), 1.0e-3);
    toCamera /= cameraDistance;
    const float3 bladeNormal = normalize(cross(across, along));
    const float edgeOn = 1.0 - abs(dot(bladeNormal, toCamera));
    float3 screenAcross = cross(toCamera, along);
    if (dot(screenAcross, screenAcross) > 1.0e-8)
    {
        screenAcross = normalize(screenAcross);
        if (dot(screenAcross, across) < 0.0) screenAcross = -screenAcross;
        across = normalize(lerp(across, screenAcross, saturate(edgeOn * 0.7)));
    }
    float bladeWidth = width * (1.0 + 0.25 * edgeOn);
    // Grass minimum width in pixels (as the mesh grass, Object.hlsl VSMain).
    if (objectData.placementFade.z > 0.0)
        bladeWidth = max(bladeWidth, objectData.placementFade.z * objectData.placementFade.w * cameraDistance);
    float3 worldPosition = centre + across * (side * bladeWidth * BladeWidthProfile(t));
    float3 worldNormal = normalize(cross(across, along));
    float3 worldTangent = across;
    // The shared grass wind, root-pinned, with a phase per blade.
    const bool useGrassWind = objectData.grassAnimation.y > 0.5 && objectData.shadowCasterFade.z <= 0.5;
    if (useGrassWind)
        ApplyGrassWind(objectData.grassAnimation.x, objectData.grassAnimation.z, root, up,
            height * t, phase, worldPosition, worldNormal, worldTangent);

    oPos = mul(objectData.viewProjection, float4(worldPosition, 1.0));
    oWorldPos = worldPosition;
    oNormal = worldNormal;
    oUv = float2(uv.x, t);
    oUv1 = 0.0;
    float3 colour = tall
        ? (t < 0.5 ? lerp(TallRoot, TallMid, t * 2.0) : lerp(TallMid, dry ? DryTip : TallTip, t * 2.0 - 1.0))
        : (t < 0.5 ? lerp(BladeRoot, BladeMid, t * 2.0) : lerp(BladeMid, dry ? DryTip : BladeTip, t * 2.0 - 1.0));
    colour *= 0.88 + 0.24 * shade;
    colour *= float3(1.0 + (hue - 0.5) * 0.16, 1.0, 1.0 - (hue - 0.5) * 0.2);
    oColor = float4(GrassColorVariation(colour, root), 1.0);
    oTangent = float4(worldTangent, 1.0);
    oUvChannels0 = float4(oUv, 0.0, 0.0);
    oUvChannels1 = 0.0;
    oUvChannels2 = 0.0;
    oUvChannels3 = 0.0;
    oUvChannels4 = 0.0;
    oInstanceLodDistance = -1.0;
}

// Indexed object/material data and a scene-wide point-light list. Only the
// draw indices/count are push constants on Vulkan.

struct DrawConstants
{
    uint objectIndex;
    uint lightCount;
    uint drawFlags;
    // Instanced ground cover: this range's share of the placement fade and
    // thinning distances in 1/1024ths (0 = all of it).
    uint placementReach;
};

struct ObjectData
{
    float4x4 viewProjection;
    float4x4 sphereView;
    float4x4 world;
    float4 baseColor;
    float4 ambientUnlit;
    float4 emissiveOcclusion;
    float4 materialParams;
    float4 viewPositionAlphaCutoff;
    float4 bakedDirectional;
    float4 bakedLightDirection;
    float4 parallaxParams;
    float4 spriteUvRect;
    float4 textureUvSets0;
    float4 textureUvSets1;
    float4 skinParams;
    float4 morphParams; // target count, vertex count, editor bone, float4 stride
    float4 environmentParams;
    float4 environmentSH[9];
    float4 reflectionEnvironmentParams;
    float4 reflectionEnvironmentSH[9];
    float4x4 shadowViewProjections[4];
    float4 shadowParams;
    float4 shadowCascadeSplits;
    float4 shadowCascadeData;
    float4 shadowCameraData;
    float4 lightingClusterParams;
    float4 lightingCameraData;
    float4 portalClipPlane;
    float4 traversalClipPlane;
    uint4 lightingChannels;
    float4 subsurfaceColorWeight;
    float4 subsurfaceRadiusThickness;
    float4 subsurfaceTransmission;
    float4 blinnPhongSpecularShininess;
    float4 pmxTextureTint;
    float4 pmxSphereTint;
    float4 pmxToonTint;
    float4 pmxEdgeColor;
    float4 pmxParams;
    uint4 terrainMapMasks;
    float4 terrainLayerParams[6];
    float4 terrainMacroColors[6];
    float4 geometryFade; // dither interval, reserved, enabled
    float4 shadowCasterFade; // range, fade width, shadow pass enabled, reserved
    float4 grassAnimation; // scene time, grass enabled, reserved
    float4 textureLodParams; // near, far, maximum mip bias, enabled/shading tier
    float4 alphaLodParams; // max alpha mip bias, disable mask at far, masked coverage transmission, card edge fade
    float4 grassCapsules[8];
    // Height fog (see AtmosphereFog.hlsli); fog0.w = 0 disables it.
    float4 atmosphereFog0;
    float4 atmosphereFog1;
    float4 atmosphereFog2;
    float4 atmosphereFog3;
    float4 terrainSunShadow; // map corner x/z, 1 / extent x/z (texture slot 4)
    float4 terrainMacroMap; // map corner x/z, 1 / extent x/z (texture slot 5)
    float4 terrainMacroParams; // blend start, full distance, bound, reserved
    float4 placementFade; // start, end (instanced ground cover, see VSMain)
    float4 groundCoverColor; // terrain: cover colour, tint strength
    float4 groundCoverRange; // terrain: fade start, fade end, layer mask, root shade
    float4 placementThin; // start, end, minimum share kept (instanced ground cover)
    // Terrain contour paths (TerrainManager::ContourPaths; TerrainPathWeight).
    float4 terrainPathNoise; // period A, weight A, period B, weight B
    uint4 terrainPathKeys;   // seed A, seed B, path layer + 1 (0 = off), crossable layer mask
    float4 terrainPathShape; // core and edge half-width (m), contour value, gradient floor
    float4 terrainPathBand;  // height band: rise start, rise end, fall start, fall end
    float4 terrainPathFar;   // far tile colour, strength
    // x = the view's mip bias (<= 0) while TAA upscales it from a lower
    // render size, so textures keep the output's sharpness; yzw reserved.
    float4 viewTextureParams;
    // Shadow atlas tiles: x = each cascade's tile (3 bits per cascade); y, z
    // = refresh blend slots (cascade + 8 * previous tile, -1 idle); w = rows.
    float4 shadowAtlasTiles;
    // Per blend slot: clip offset to the previous image (xyz), new weight (w).
    float4 shadowRefreshBlend[2];
};

struct SceneLightData
{
    float4 positionRange;
    float4 colorIntensity;
    float4 params;
    float4 directionCone;
    float4 attenuation;
    float4 cookieRight;
    float4 cookieUp;
    float4 photometry;
};

struct InstanceTransform { float4 row0; float4 row1; float4 row2; };
struct PersistentInstance { float4 row0; float4 row1; float4 row2; uint4 identity; };

#ifdef VULKAN
[[vk::push_constant]] DrawConstants draw;
[[vk::binding(7, 0)]] StructuredBuffer<SceneLightData> sceneLights;
[[vk::binding(8, 0)]] StructuredBuffer<ObjectData> objects;
[[vk::binding(9, 0)]] StructuredBuffer<float4x4> boneMatrices;
[[vk::binding(11, 0)]] StructuredBuffer<float4> morphDeltas;
[[vk::binding(12, 0)]] StructuredBuffer<float4> morphWeights;
[[vk::binding(46, 0)]] StructuredBuffer<InstanceTransform> meshInstances;
[[vk::binding(47, 0)]] StructuredBuffer<PersistentInstance> persistentInstances;
[[vk::binding(48, 0)]] StructuredBuffer<uint> screenOcclusion;
[[vk::binding(0, 0)]] Texture2D baseColorMap;
[[vk::binding(1, 0)]] Texture2D metallicRoughnessMap;
[[vk::binding(2, 0)]] Texture2D normalMap;
[[vk::binding(3, 0)]] Texture2D occlusionMap;
[[vk::binding(4, 0)]] Texture2D emissiveMap;
[[vk::binding(5, 0)]] Texture2D heightMap;
[[vk::binding(10, 0)]] Texture2D environmentMap;
[[vk::binding(13, 0)]] Texture2D<float> shadowMap;
[[vk::binding(14, 0)]] Texture2D brdfIntegrationLut;
[[vk::binding(15, 0)]] StructuredBuffer<uint2> clusteredLightGrid;
[[vk::binding(16, 0)]] StructuredBuffer<uint> clusteredLightIndices;
[[vk::binding(17, 0)]] Texture2D cookie0;
[[vk::binding(18, 0)]] Texture2D cookie1;
[[vk::binding(19, 0)]] Texture2D cookie2;
[[vk::binding(20, 0)]] Texture2D cookie3;
[[vk::binding(21, 0)]] Texture2D ies0;
[[vk::binding(22, 0)]] Texture2D ies1;
[[vk::binding(23, 0)]] Texture2D ies2;
[[vk::binding(24, 0)]] Texture2D ies3;
[[vk::binding(25, 0)]] Texture2D subsurfaceThicknessMap;
[[vk::binding(26, 0)]] Texture2D sphereMap;
[[vk::binding(27, 0)]] Texture2D toonMap;
// Terrain layer arrays (logical texture slots 12-14); slice = painted layer.
[[vk::binding(28, 0)]] Texture2DArray terrainAlbedoHeight;
[[vk::binding(29, 0)]] Texture2DArray terrainNormal;
[[vk::binding(30, 0)]] Texture2DArray terrainRoughAo;
// Paint map (logical slot 15): slice 0 = layer weights 0-3, slice 1 = 4-5.
[[vk::binding(31, 0)]] Texture2DArray terrainPaint;
[[vk::binding(6, 0)]] SamplerState materialSampler;
#else
cbuffer DrawBuffer : register(b0)
{
    DrawConstants draw;
};
Texture2D baseColorMap         : register(t0);
Texture2D metallicRoughnessMap : register(t1);
Texture2D normalMap            : register(t2);
Texture2D occlusionMap         : register(t3);
Texture2D emissiveMap          : register(t4);
Texture2D heightMap            : register(t5);
Texture2D environmentMap       : register(t9);
Texture2D<float> shadowMap      : register(t12);
Texture2D brdfIntegrationLut    : register(t13);
StructuredBuffer<uint2> clusteredLightGrid : register(t14);
StructuredBuffer<uint> clusteredLightIndices : register(t15);
Texture2D cookie0 : register(t16); Texture2D cookie1 : register(t17);
Texture2D cookie2 : register(t18); Texture2D cookie3 : register(t19);
Texture2D ies0 : register(t20); Texture2D ies1 : register(t21);
Texture2D ies2 : register(t22); Texture2D ies3 : register(t23);
Texture2D subsurfaceThicknessMap : register(t24);
Texture2D sphereMap : register(t25);
Texture2D toonMap : register(t26);
// Terrain layer arrays (logical texture slots 12-14); slice = painted layer.
Texture2DArray terrainAlbedoHeight : register(t27);
Texture2DArray terrainNormal : register(t28);
Texture2DArray terrainRoughAo : register(t29);
Texture2DArray terrainPaint : register(t30);
StructuredBuffer<SceneLightData> sceneLights : register(t6);
StructuredBuffer<ObjectData> objects : register(t7);
StructuredBuffer<float4x4> boneMatrices : register(t8);
StructuredBuffer<float4> morphDeltas : register(t10);
StructuredBuffer<float4> morphWeights : register(t11);
StructuredBuffer<InstanceTransform> meshInstances : register(t45);
StructuredBuffer<PersistentInstance> persistentInstances : register(t46);
StructuredBuffer<uint> screenOcclusion : register(t47);
SamplerState materialSampler : register(s0);
#endif

#include "../SkinDeformation.hlsli"

// Screen-space ambient occlusion (Shaders/AmbientOcclusion/AmbientOcclusion.hlsl,
// structured slot 47). The buffer holds the main view's previous-frame AO
// at half resolution behind a header with the camera it was computed from:
// project the surface through that camera, filter the four nearest texels
// whose stored view depth matches, and fall back to 1 (unoccluded) off
// screen, on disocclusion, or when the header is not valid (AO off or a
// one-element placeholder bound).
float ScreenAmbientOcclusion(float3 worldPos)
{
    [branch] if (asfloat(screenOcclusion[11]) < 0.5)
        return 1.0;
    const float3 offset = worldPos - asfloat(uint3(screenOcclusion[0], screenOcclusion[1],
        screenOcclusion[2]));
    const float w = dot(offset, asfloat(uint3(screenOcclusion[12], screenOcclusion[13],
        screenOcclusion[14])));
    if (w <= 0.05)
        return 1.0;
    const float2 ndc = float2(
        asfloat(screenOcclusion[3]) * dot(offset, asfloat(uint3(screenOcclusion[4],
            screenOcclusion[5], screenOcclusion[6]))),
        asfloat(screenOcclusion[7]) * dot(offset, asfloat(uint3(screenOcclusion[8],
            screenOcclusion[9], screenOcclusion[10])))) / w;
    const float2 uv = ndc * float2(0.5, -0.5) + 0.5;
    if (any(uv < 0.0) || any(uv > 1.0))
        return 1.0;
    const uint packedExtent = screenOcclusion[15];
    const int2 extent = int2(packedExtent & 0xFFFFu, packedExtent >> 16);
    // A depth-matched 3x3 tent around the nearest half-resolution texel. The
    // AO pass rotates its two slices per pixel with interleaved gradient
    // noise, which is only uniform over 3x3 neighbourhoods; a bilinear 2x2
    // lookup let its diagonal structure through as dark stripes wherever the
    // temporal history is rejected (wind-moved leaves and fronds).
    const float2 texel = uv * float2(extent) - 0.5;
    const int2 base = int2(floor(texel + 0.5));
    float occlusion = 0.0, weight = 0.0;
    [unroll] for (int dy = -1; dy <= 1; ++dy)
    [unroll] for (int dx = -1; dx <= 1; ++dx)
    {
        const int2 coordinate = clamp(base + int2(dx, dy), int2(0, 0), extent - 1);
        const uint packed = screenOcclusion[16u + uint(coordinate.y * extent.x + coordinate.x)];
        const float storedDepth = f16tof32(packed >> 16);
        const float tent = (dx == 0 ? 2.0 : 1.0) * (dy == 0 ? 2.0 : 1.0);
        const float match = abs(storedDepth - w) <= 0.04 * w + 0.1 ? tent : 0.0;
        occlusion += match * f16tof32(packed & 0xFFFFu);
        weight += match;
    }
    return weight > 0.05 ? saturate(occlusion / weight) : 1.0;
}

float SampleCookie(uint index, float2 uv)
{
    if (index == 1u) return cookie0.Sample(materialSampler, uv).r;
    if (index == 2u) return cookie1.Sample(materialSampler, uv).r;
    if (index == 3u) return cookie2.Sample(materialSampler, uv).r;
    if (index == 4u) return cookie3.Sample(materialSampler, uv).r;
    return 1.0;
}

// Directional light cookie (Light::cookieTexture): projected along the light
// over world space, scrolled by cookieRight.w / cookieUp.w (Light::
// cookieScrollSpeed). Cloud shadows (.md/todo.md Foliage Phase 3) are a tiling
// cookie on the sun. Every lighting path that takes the sun calls this, so
// terrain, grass, foliage and objects darken together.
float DirectionalCookie(SceneLightData light, float3 worldPos)
{
    const uint index = (uint)light.photometry.x;
    if (index == 0u) return 1.0;
    const float2 uv = float2(dot(worldPos, light.cookieRight.xyz),
        dot(worldPos, light.cookieUp.xyz)) / max(light.photometry.z, 0.001) + 0.5 +
        float2(light.cookieRight.w, light.cookieUp.w);
    return SampleCookie(index, uv);
}

// The sun's cookie for draws that light by objectData.bakedDirectional (the
// terrain paths): w is the sun's light index + 1.
float SunCookie(ObjectData objectData, float3 worldPos)
{
    return objectData.bakedDirectional.w > 0.5
        ? DirectionalCookie(sceneLights[(uint)objectData.bakedDirectional.w - 1u], worldPos) : 1.0;
}

float SampleIes(uint index, float cosine)
{
    float2 uv = float2(acos(clamp(cosine, -1.0, 1.0)) / 3.14159265, 0.5);
    if (index == 1u) return ies0.Sample(materialSampler, uv).r;
    if (index == 2u) return ies1.Sample(materialSampler, uv).r;
    if (index == 3u) return ies2.Sample(materialSampler, uv).r;
    if (index == 4u) return ies3.Sample(materialSampler, uv).r;
    return 1.0;
}

uint ResolveObjectIndex(uint instanceId)
{
    // Root SRVs for inactive paths can be a one-record placeholder. Do not
    // flatten these branches into speculative reads of instanceId+1/+2.
    [branch] if ((draw.drawFlags & 0x13080000u) == 0u)
        return draw.objectIndex;
    [branch] if ((draw.drawFlags & 0x00080000u) != 0u)
        return draw.objectIndex+asuint(meshInstances[instanceId+1].row0.x);
    [branch] if ((draw.drawFlags & 0x01000000u) != 0u)
        return draw.objectIndex + asuint(meshInstances[instanceId * 2 + 2].row0.x);
    [branch] if ((draw.drawFlags & 0x02000000u) != 0u)
        return draw.objectIndex + asuint(meshInstances[instanceId * 2 + 1].row0.x);
    return draw.objectIndex + asuint(meshInstances[instanceId + 1].row0.x);
}

// Occlusion debug mark written by InstanceCull.hlsl (ENGINE_OCCLUSION_DEBUG):
// row0.z of the same record ResolveObjectIndex reads. Explicit-mesh records
// carry other data there and are never marked.
bool ResolveOcclusionMark(uint instanceId)
{
    [branch] if ((draw.drawFlags & 0x00080000u) != 0u)
        return asuint(meshInstances[instanceId+1].row0.z) != 0u;
    [branch] if ((draw.drawFlags & 0x13080000u) == 0u ||
        (draw.drawFlags & 0x03000000u) != 0u)
        return false;
    return asuint(meshInstances[instanceId + 1].row0.z) != 0u;
}

#include "../GrassWind.hlsli"
#include "../AtmosphereFog.hlsli"
// ApplyObjectAtmosphereFog follows the directional shadow functions below.
#ifdef GRASS_CAPSULE_INTERACTION
#include "../GrassFootBend.hlsli"
#endif

// Shared by the grass surface shader and the terrain under ground cover
// (Terrain::GroundCover), so both carry the same dry/lush meadow pattern.
// How green a colour is: 1 for blades and leaves, 0 for petals, seed heads
// and straw tips, which keep their authored colour and flat shading.
float GrassLeafiness(float3 color)
{
    // Relative to the colour's brightness, so dark, realistic greens count as
    // leafy as much as bright ones (an absolute difference halved it when the
    // plant palette was darkened).
    return saturate((color.g - max(color.r, color.b)) / max(color.g, 1.0e-4) * 3.0);
}

// Broad dry/lush meadow patches (11 m period), without per-clump jitter.
float3 GrassPatchVariation(float3 color, float3 position)
{
    float leafy = GrassLeafiness(color);
    if (leafy <= 0.0) return color;
    float dry = saturate(GrassWindPerlin(position.xz / 11.0, 0x51ed270bu) * .9 + .05);
    float luminance = dot(color, float3(.2126, .7152, .0722));
    float3 straw = luminance * float3(1.3, 1.05, .4);
    return lerp(color, lerp(color, straw, dry * .35), leafy);
}

// Grass drawn into the terrain itself, for where the blades thin out and
// beyond them (.md/todo.md, Landscape demo expectations: grass cover). The
// ground is shaded as a layer of tufts ~0.3 m deep instead of a flat tint:
// four parallax shells through a procedural tuft height field. A view ray
// that reaches a shell inside a tuft stops there, so tufts hide the ground
// behind them at grazing angles and show dark gaps seen from above, the way
// the blade field does. Returns a multiplier on the cover colour: dark at
// the roots, lighter and a little yellower at the tips.
float GrassTuftHeight(float2 position, float bladeDetail)
{
    const float clump = GrassWindPerlin(position * 1.9, 0x2c9277b5u);       // ~0.5 m tufts
    const float blades = GrassWindPerlin(position * 9.0, 0x6a09e667u);      // blade-scale breakup
    return saturate(0.7 + 0.9 * clump + 0.3 * blades * bladeDetail);
}

float3 GrassGroundShells(float3 worldPos, float3 toCamera, float3 normal, float footprint)
{
    const float depth = 0.3;
    const float cosine = saturate(dot(toCamera, normal));
    // How far the view ray travels over the ground per metre it climbs; capped
    // so grazing pixels do not sample far outside their own patch.
    const float2 slide = toCamera.xz / max(cosine, 0.12) * depth;
    // Each octave fades out before it gets smaller than a pixel (no shimmer).
    const float bladeDetail = saturate(1.0 - footprint * 6.0);
    const float resolved = saturate(1.0 - footprint * 1.4);
    float level = 0.0;
    [unroll] for (int shell = 1; shell <= 4; ++shell)
    {
        const float height = shell * 0.25;
        // Higher shells are nearer the camera along the ray, so the last hit wins.
        if (GrassTuftHeight(worldPos.xz + slide * height, bladeDetail) >= height)
            level = height;
    }
    const float3 traced = lerp(float3(0.42, 0.45, 0.42), float3(1.22, 1.16, 0.92), level);
    // Unresolved, the shells average out: mostly tips at grazing angles,
    // roots and gaps from above.
    const float grazing = 1.0 - cosine;
    const float3 mean = lerp(float3(0.84, 0.85, 0.8), float3(1.14, 1.1, 0.9), grazing * sqrt(grazing));
    // A ray skimming the field always ends in the top of some tuft; four
    // samples miss most of them, so toward the horizon the result goes to
    // the tips (a meadow seen at eye level shows no ground at all).
    const float skim = smoothstep(0.8, 0.98, grazing);
    return lerp(lerp(mean, traced, resolved), float3(1.16, 1.12, 0.9) * (0.93 + 0.14 * level), skim);
}

// The wind over grass too far off for its blades to be seen: the broad gust
// fronts of ApplyGrassWind (same field, direction and speed) shaded as the
// light changing on a leaning field. Blades leaning toward the camera show
// their lit sides and tips (lighter, yellower), leaning away their roots
// (darker); any strong lean adds a little sheen. The fronts are 18 m along
// the wind and GrassWindFrontStretch times that across (~144 m), so they stay
// several pixels wide hundreds of metres away. Returns a multiplier on the cover colour.
float3 GrassGroundWind(float3 worldPos, float sceneTime, float3 toCamera)
{
    const float direction = radians(GrassWindDirectionDegrees);
    const float2 wind = float2(cos(direction), sin(direction));
    const float2 traveling = worldPos.xz - wind * (sceneTime * max(0.0, GrassWindSpeed) * 2.0);
    const float2 coordinate = GrassWindFrontCoordinate(traveling, wind, GrassWindWaveSize);
    // ApplyGrassWind's gust without per-blade detail (bladeDetail 0).
    const float gust = clamp(2.1 * .85 * GrassWindPerlin(coordinate, GrassWindSeed), -1.0, 1.0) * .7;
    const float2 view = toCamera.xz;
    const float viewLength = length(view);
    const float facing = viewLength > 1e-4 ? dot(wind, view / viewLength) : 0.0;
    const float lean = gust * facing * GrassWindStrength;
    const float light = .34 * lean + .12 * abs(gust) - .06;
    return float3(1.0 + light, 1.0 + light, 1.0 + light - .5 * max(light, 0.0));
}

#ifdef GRASS_SURFACE_SHADING
// Meadow colour variation keyed to the placement root (stable under wind):
// broad dry/lush patches plus a per-clump brightness and hue jitter.
float3 GrassColorVariation(float3 color, float3 root)
{
    float leafy = GrassLeafiness(color);
    if (leafy <= 0.0) return color;
    float dry = saturate(GrassWindPerlin(root.xz / 11.0, 0x51ed270bu) * .9 + .05);
    uint clumpHash = GrassWindHash(int2(floor(root.xz * 4.0)), 0x2545f491u);
    float brightness = lerp(.86, 1.14, (float)(clumpHash & 255u) / 255.0);
    float hue = (float)((clumpHash >> 8) & 255u) / 255.0 - .5;
    float luminance = dot(color, float3(.2126, .7152, .0722));
    float3 straw = luminance * float3(1.3, 1.05, .4);
    float3 varied = lerp(color, straw, dry * .35);
    varied *= brightness * float3(1.0 + hue * .16, 1.0, 1.0 - hue * .2);
    return lerp(color, varied, leafy);
}
#endif

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
    oObjectIndex = ResolveObjectIndex(instanceId);
    ObjectData objectData = objects[oObjectIndex];
    oInstanceLodDistance = -1.0;
    float4 localPosition = float4(pos, 1.0);
    float3 localNormal = normal;
    float3 localTangent = tangent.xyz;
    float3 grassRoot = float3(0, 0, 0);
    float3 grassUp = float3(0, 1, 0);
    float3 grassScaleRow = float3(1, 0, 0);
    // Octahedral impostors (below): the crown centre (uvChannels2.xyz) and the
    // placement's own X and Z axes, through the instance transform.
    float3 octahedralCenter = uvChannels2.xyz;
    float3 instanceAxisX = float3(1, 0, 0);
    float3 instanceAxisZ = float3(0, 0, 1);
    [branch] if ((draw.drawFlags & 0x20000000u) != 0u)
    {
        InstanceTransform instance = meshInstances[(draw.drawFlags & 0x01000000u) != 0u ? instanceId * 2 + 1 :
            ((draw.drawFlags & 0x02000000u) != 0u ? instanceId * 2 : instanceId)];
        [branch] if((draw.drawFlags & 0x00080000u) != 0u)
        {
            PersistentInstance placement=persistentInstances[asuint(meshInstances[instanceId+1].row0.y)];
            instance.row0=placement.row0; instance.row1=placement.row1; instance.row2=placement.row2;
        }
        grassRoot = float3(instance.row0.w, instance.row1.w, instance.row2.w);
        grassUp = float3(instance.row0.y, instance.row1.y, instance.row2.y);
        grassScaleRow = instance.row0.xyz;
        instanceAxisX = float3(instance.row0.x, instance.row1.x, instance.row2.x);
        instanceAxisZ = float3(instance.row0.z, instance.row1.z, instance.row2.z);
        octahedralCenter = float3(dot(instance.row0, float4(octahedralCenter, 1.0)),
            dot(instance.row1, float4(octahedralCenter, 1.0)),
            dot(instance.row2, float4(octahedralCenter, 1.0)));
        localPosition = float4(dot(instance.row0, localPosition),
            dot(instance.row1, localPosition), dot(instance.row2, localPosition), 1.0);
        localNormal = float3(dot(instance.row0.xyz, localNormal),
            dot(instance.row1.xyz, localNormal), dot(instance.row2.xyz, localNormal));
        localTangent = float3(dot(instance.row0.xyz, localTangent),
            dot(instance.row1.xyz, localTangent), dot(instance.row2.xyz, localTangent));
    }
    float4 localUv = float4(uv, uv1);
    float4 fullUv0 = float4(uv, uvChannels0.zw);
    float4 fullUv1 = float4(uv1, uvChannels1.zw);
    float4 fullUv2 = uvChannels2;
    float4 fullUv3 = uvChannels3;
    float4 fullUv4 = uvChannels4;
    uint activeMorphCount = (uint)objectData.morphParams.x;
    uint morphVertexCount = (uint)objectData.morphParams.y;
    uint morphStride = (uint)objectData.morphParams.w;
    [loop] for (uint active = 0; active < activeMorphCount; ++active)
    {
        float2 morph = morphWeights[active].xy;
        uint target = (uint)morph.x;
        float weight = morph.y;
        uint base = (target * morphVertexCount + vertexId) * morphStride;
        localPosition.xyz += morphDeltas[base].xyz * weight;
        if (morphStride >= 3)
        {
            localNormal += morphDeltas[base + 1].xyz * weight;
            localTangent += morphDeltas[base + 2].xyz * weight;
        }
        if (morphStride >= 4)
            localUv += morphDeltas[base + 3] * weight;
        if (morphStride >= 9)
        {
            fullUv0.zw += morphDeltas[base + 4].zw * weight;
            fullUv1.zw += morphDeltas[base + 5].zw * weight;
            fullUv2 += morphDeltas[base + 6] * weight;
            fullUv3 += morphDeltas[base + 7] * weight;
            fullUv4 += morphDeltas[base + 8] * weight;
        }
    }
    if (dot(localNormal, localNormal) > 0.000001)
        localNormal = normalize(localNormal);
    if (dot(localTangent, localTangent) > 0.000001)
        localTangent = normalize(localTangent);
    uint jointCount = (uint)objectData.skinParams.y;
    if (jointCount > 0)
    {
        uint paletteOffset = (uint)objectData.skinParams.x;
        SkinDeform(localPosition, localNormal, localTangent,
            joints0, weights0, joints1, weights1, sdefCenterAndMode,
            sdefRadius0, sdefRadius1, paletteOffset, jointCount);
        localNormal = normalize(localNormal);
        if (dot(localTangent, localTangent) > 0.0001)
            localTangent = normalize(localTangent);
    }
    // Resolve world space first, then use the shared view-projection matrix.
    // Adjacent terrain patches can own different transforms while still
    // producing bit-identical clip positions along their common border.
    float4 worldPosition = mul(objectData.world, localPosition);
    // Instanced LOD bands choose a band per placement (see ApplyGeometryFade):
    // every vertex of a placement carries the distance to its origin, so the
    // interpolated value is constant across it. With screen coverage
    // (geometryFade.w 5) a placement scaled by s covers s times the pixels,
    // so its distance counts as distance / s; InstanceCull.hlsl (bit 32)
    // divides the same way.
    if ((draw.drawFlags & 0x20000000u) != 0u && objectData.geometryFade.w >= 4.0)
    {
        oInstanceLodDistance = length(mul(objectData.world, float4(grassRoot, 1.0)).xyz -
            objectData.viewPositionAlphaCutoff.xyz);
        // 5 and 7 are the screen-coverage variants of 4 (instanced bands)
        // and 6 (ground cover with a detail level per placement).
        if ((objectData.geometryFade.w >= 4.5 && objectData.geometryFade.w < 5.5) ||
            objectData.geometryFade.w >= 6.5)
            oInstanceLodDistance /= clamp(length(grassScaleRow), 0.25, 4.0);
    }
    // Detail view occlusion debug: -2 marks a placement the cull would hide.
    [branch] if (objectData.morphParams.z < -50.0 && ResolveOcclusionMark(instanceId))
        oInstanceLodDistance = -2.0;
    // Instanced ground cover fades out per placement (Mesh::placementFadeStart
    // /End): each plant shrinks toward its root along its own up axis (right
    // on slopes) and narrows a little, so the range edge has no tile squares
    // and no dither. A per-placement offset spreads the edge so it never
    // forms a ring; it only brings the fade forward, so every placement is
    // gone by the end distance.
    //
    // Distance thinning (Mesh::placementThin*): past placementThin.x the
    // share of plants kept falls toward placementThin.z by placementThin.y.
    // Each plant has a fixed random rank and shrinks away as the share drops
    // past it (no popping); survivors widen by 1 / sqrt(share) so the cover
    // stays as dense on screen while far fewer plants are drawn.
    float placementScale = 1.0;
    float placementWiden = 1.0;
    // Shrubs and bushes (placementReach bit 10) dissolve in at the range edge
    // at their full size instead of rising out of the ground: their range fade
    // goes to the pixel shader (uvChannels4.w, g_placementOpacity), which
    // clips it with the screen dither. Grass keeps the shrink.
    float placementOpacity = 1.0;
    const bool placementDissolve = (draw.drawFlags & 0x20000000u) != 0u && (draw.placementReach & 1024u) != 0u;
    const bool placementFadeOn = objectData.placementFade.y > objectData.placementFade.x;
    // start > end runs the thinning backwards: a fade-in (far grass ring).
    const bool placementThinOn = objectData.placementThin.z < 1.0 &&
        objectData.placementThin.y != objectData.placementThin.x;
    [branch] if ((draw.drawFlags & 0x20000000u) != 0u && (placementFadeOn || placementThinOn))
    {
        const float3 root = mul(objectData.world, float4(grassRoot, 1.0)).xyz;
        // Fine grass ends sooner than the shrubs of the same tile: measuring
        // its distance longer by 1 / reach is the same as scaling every fade
        // and thinning distance by reach. InstanceCull.hlsl scales to match.
        const float cameraDistance = length(root - objectData.viewPositionAlphaCutoff.xyz) *
            ((draw.placementReach & 1023u) != 0u ? 1024.0 / (float)(draw.placementReach & 1023u) : 1.0);
        if (placementFadeOn)
        {
            const float band = objectData.placementFade.y - objectData.placementFade.x;
            const uint hash = GrassWindHash(int2(floor(root.xz * 16.0)), 0x68e31da4u);
            const float advance = (float)(hash & 1023u) / 1023.0 * 0.35 * band;
            placementScale = 1.0 - smoothstep(objectData.placementFade.x, objectData.placementFade.y,
                cameraDistance + advance);
            if (placementDissolve) { placementOpacity = placementScale; placementScale = 1.0; }
        }
        // Bit 10: the range is not thinned (shrubs, bushes).
        if (placementThinOn && (draw.placementReach & 1024u) == 0u)
        {
            const float share = lerp(1.0, saturate(objectData.placementThin.z), smoothstep(
                objectData.placementThin.x, objectData.placementThin.y, cameraDistance));
            const uint rankHash = GrassWindHash(int2(floor(root.xz * 16.0)), 0x1b873593u);
            const float rank = (float)(rankHash & 4095u) / 4095.0;
            placementScale *= saturate((share - rank) / 0.06);
            placementWiden = objectData.placementThin.w > 0.5 ? 1.0 : rsqrt(max(share, 0.05));
        }
        const float3 placementUp = normalize(mul((float3x3)objectData.world, grassUp));
        const float3 offset = worldPosition.xyz - root;
        const float rise = dot(offset, placementUp);
        const float lateral = placementScale > 0.0 ? lerp(0.5, 1.0, placementScale) * placementWiden : 0.0;
        worldPosition.xyz = root + placementUp * (rise * placementScale) +
            (offset - placementUp * rise) * lateral;
    }
    float3 worldNormal = normalize(mul((float3x3)objectData.world, localNormal));
    float3 worldTangent = mul((float3x3)objectData.world, localTangent);
    float3 worldUp = mul((float3x3)objectData.world, grassUp);
    float upLength = max(length(worldUp), 0.0001);
    // View-baked impostor cards (Material::impostorBillboard). Each vertical
    // card is a plane through the placement's up axis, and its normal is the
    // direction its view was baked from. Turn every card about that axis to
    // face the camera, and keep only the views nearest the camera direction:
    // a view owns +-20 degrees and hands over to its neighbour (60 degrees
    // apart for the 6-view oak atlases) across the next 20. The two weights
    // are w and 1 - w, so the pixel shader splits the screen between them
    // with one dither (ApplyGeometryFade, billboardFade): the card turned
    // one way keeps hash < w, the other hash >= 1 - w. No pixel is drawn by
    // both coplanar cards, so they cannot depth-fight at any distance.
    // Cards with no weight collapse to a point and rasterize nothing; the
    // horizontal top-down card only shows when looking steeply down.
    float billboardAlpha = 1.0;
    float billboardFade = 1.0;
    float billboardStart = 0.0;
    [branch] if (((uint)objectData.materialParams.w & 1024u) != 0u)
    {
        const float3 axis = worldUp / upLength;
        const float3 root = mul(objectData.world, float4(grassRoot, 1.0)).xyz;
        // Shadow views turn the cards toward the light so the caster has the
        // tree's silhouette as the sun sees it: light-space depth is
        // dot(viewProjection[2], p), so that row is the light axis (either
        // sign gives a valid, mirrored silhouette).
        const float3 toCamera = objectData.shadowCasterFade.z > 0.5
            ? -objectData.viewProjection[2].xyz * (1000.0 /
                max(length(objectData.viewProjection[2].xyz), 1.0e-8))
            : objectData.viewPositionAlphaCutoff.xyz - root;
        const float cameraDistance = max(length(toCamera), 1.0e-4);
        float3 cameraFlat = toCamera - axis * dot(toCamera, axis);
        const float flatLength = length(cameraFlat);
        const float axisFacing = dot(worldNormal, axis);
        [branch] if (fullUv3.x > 0.5)
        {
            // Hemi-octahedral impostor (tools/bake_octahedral_impostors.py):
            // N x N views of the tree over the upper hemisphere, view (i, j)
            // from the direction that decodes from (i, j) / (N - 1). The
            // mesh is three quads; each takes one of the three views around
            // the direction to the camera (or the light, for shadows), lies
            // in that view's image plane through the crown centre, and draws
            // the screen pixels of its barycentric weight's dither interval,
            // so the three views blend without overlapping. The view's
            // camera basis becomes the quad's tangent frame, so the baked
            // normals light as the tree's own.
            const float gridSize = fullUv3.x;
            const uint quad = (uint)(fullUv3.y + 0.5);
            const float3 center = mul(objectData.world, float4(octahedralCenter, 1.0)).xyz;
            const float3 worldAxisX = mul((float3x3)objectData.world, instanceAxisX);
            const float radius = uvChannels2.w * length(worldAxisX);
            const float3 objectX = normalize(worldAxisX);
            const float3 objectZ = normalize(cross(objectX, axis));
            float3 toView = objectData.shadowCasterFade.z > 0.5 ? toCamera
                : objectData.viewPositionAlphaCutoff.xyz - center;
            // The light axis: either sign is the same line; take the upper one.
            if (objectData.shadowCasterFade.z > 0.5 && dot(toView, axis) < 0.0)
                toView = -toView;
            float3 local = float3(dot(toView, objectX), max(dot(toView, axis), 0.0),
                dot(toView, objectZ));
            local /= max(abs(local.x) + abs(local.y) + abs(local.z), 1.0e-6);
            const float2 encoded = float2(local.x + local.z, local.x - local.z) * 0.5 + 0.5;
            const float2 grid = saturate(encoded) * (gridSize - 1.0);
            const float2 base = clamp(floor(grid), 0.0, gridSize - 2.0);
            const float2 f = saturate(grid - base);
            float2 cells[3];
            float weights[3];
            if (f.x + f.y < 1.0)
            {
                cells[0] = base; cells[1] = base + float2(1, 0); cells[2] = base + float2(0, 1);
                weights[0] = 1.0 - f.x - f.y; weights[1] = f.x; weights[2] = f.y;
            }
            else
            {
                cells[0] = base + float2(1, 1); cells[1] = base + float2(1, 0); cells[2] = base + float2(0, 1);
                weights[0] = f.x + f.y - 1.0; weights[1] = 1.0 - f.y; weights[2] = 1.0 - f.x;
            }
            // Sharpen the blend (weights^4, renormalized): the nearest view
            // draws the whole tree most of the time and the dither only runs
            // near the boundaries between views. Plain barycentric weights
            // kept nearly every far tree split three ways, a constant
            // dither that read as grain wherever TAA history was rejected.
            [unroll] for (uint k = 0u; k < 3u; ++k)
                weights[k] = pow(saturate(weights[k]), 4.0);
            const float weightSum = max(weights[0] + weights[1] + weights[2], 1.0e-6);
            [unroll] for (uint m = 0u; m < 3u; ++m)
                weights[m] /= weightSum;
            const float2 cell = quad == 0u ? cells[0] : (quad == 1u ? cells[1] : cells[2]);
            const float weight = quad == 0u ? weights[0] : (quad == 1u ? weights[1] : weights[2]);
            const float start = quad == 0u ? 0.0 : (quad == 1u ? weights[0] : weights[0] + weights[1]);
            // The view's direction and camera basis (as the baker's).
            const float2 centered = cell / (gridSize - 1.0) - 0.5;
            const float px = centered.x + centered.y, pz = centered.x - centered.y;
            const float3 viewLocal = normalize(float3(px, max(1.0 - abs(px) - abs(pz), 0.0), pz));
            const float3 forward = normalize(objectX * viewLocal.x + axis * viewLocal.y +
                objectZ * viewLocal.z);
            const float3 referenceUp = viewLocal.y > 0.999 ? -objectZ : axis;
            const float3 right = normalize(cross(referenceUp, forward));
            const float3 up = cross(forward, right);
            const float2 corner = localUv.xy; // 0..1, v down
            worldPosition.xyz = weight > 0.0 ? center + (right * (corner.x * 2.0 - 1.0) +
                up * (1.0 - corner.y * 2.0)) * radius : center;
            localUv.xy = (cell + corner) / gridSize;
            worldNormal = forward;
            worldTangent = right;
            billboardAlpha = weight;
            // Interval mode for ApplyGeometryFade: w = 3 + end, z = start.
            billboardFade = 3.0 + start + weight;
            billboardStart = start;
        }
        else if (abs(axisFacing) > 0.7)
        {
            // sin(50) .. sin(70): top card fades in as the view steepens.
            billboardAlpha = smoothstep(0.77, 0.94, dot(toCamera, axis) / cameraDistance);
            billboardFade = billboardAlpha;
        }
        else if (flatLength > 1.0e-4)
        {
            cameraFlat /= flatLength;
            const float3 viewFlat = normalize(worldNormal - axis * axisFacing);
            const float cosine = clamp(dot(viewFlat, cameraFlat), -1.0, 1.0);
            const float sine = dot(cross(viewFlat, cameraFlat), axis);
            billboardAlpha = saturate((40.0 - degrees(acos(cosine))) / 20.0);
            // Neighbouring views turn opposite ways toward the camera.
            billboardFade = sine >= 0.0 ? billboardAlpha : -billboardAlpha;
            // Rodrigues rotation about the placement axis: viewFlat -> cameraFlat.
            const float3 offset = worldPosition.xyz - root;
            worldPosition.xyz = root + offset * cosine + cross(axis, offset) * sine +
                axis * (dot(axis, offset) * (1.0 - cosine));
            worldTangent = worldTangent * cosine + cross(axis, worldTangent) * sine +
                axis * (dot(axis, worldTangent) * (1.0 - cosine));
            // A canopy seen from afar is lit like a dome, not a wall: bend the
            // card normal up so a back-lit card does not turn black.
            worldNormal = normalize(cameraFlat + axis * 0.5);
        }
        if (billboardAlpha <= 0.0)
            worldPosition.xyz = root;
    }
#ifdef GRASS_CAPSULE_INTERACTION
    float3 restWorldPosition = worldPosition.xyz;
    float3 restWorldNormal = worldNormal;
    float3 restWorldTangent = worldTangent;
#endif
    // Wind must not deform shadow casters; otherwise shadow maps redraw for
    // every shader-driven vertex change while the mesh is still static in the
    // light pass.
    // grassAnimation.y: 1 grass bend, 2 tree wind (Mesh::foliageWindTree).
    const bool useTreeWind = objectData.grassAnimation.y > 1.5 &&
        objectData.shadowCasterFade.z <= 0.5;
    const bool useGrassWind = objectData.grassAnimation.y > 0.5 && !useTreeWind &&
        objectData.shadowCasterFade.z <= 0.5;
    float windHeight = max(0.0, pos.y) * upLength * placementScale;
#ifdef GRASS_SURFACE_SHADING
    // Instanced meadow meshes carry a wind response in vertex alpha: 1 for
    // grass blades (the default), lower for woody shrubs, which then sway
    // and give way underfoot as if they were shorter.
    if ((draw.drawFlags & 0x20000000u) != 0u)
        windHeight *= saturate(color.a);
#endif
    // Tree wind moves what is rasterized, but shading keeps the rest position:
    // shadow casters stay static, and a swaying leaf or trunk compared
    // against its own unmoved caster shadows itself in diagonal acne bands.
    float3 shadingPosition = 0.0;
    bool shadeAtRest = false;
    [branch] if (useTreeWind)
    {
        shadingPosition = worldPosition.xyz;
        shadeAtRest = true;
        // Fades out from 60 to 100 m: far crowns are too small to show it and
        // skip the work. Cooked leaf meshes carry their canopy shell in uv1.
        const float3 treeRoot = mul(objectData.world, float4(grassRoot, 1)).xyz;
        const float fade = 1.0 - smoothstep(60.0, 100.0,
            distance(treeRoot, objectData.viewPositionAlphaCutoff.xyz));
        const float leafShell = uv1.y > 3.5 ? saturate(uv1.x) : 0.0;
        if (fade > 0.0)
            ApplyTreeWind(objectData.grassAnimation.x, objectData.grassAnimation.z * fade,
                treeRoot, worldUp / upLength, leafShell, worldPosition.xyz, worldNormal);
    }
    if (useGrassWind)
    {
        const float3 windRoot = mul(objectData.world, float4(grassRoot, 1)).xyz;
        ApplyGrassWind(objectData.grassAnimation.x, objectData.grassAnimation.z,
            windRoot, worldUp / upLength,
            windHeight, GrassBladePhase(pos),
            worldPosition.xyz, worldNormal, worldTangent,
            GrassWindBladeDetail(distance(windRoot, objectData.viewPositionAlphaCutoff.xyz)));
    }
#ifdef GRASS_CAPSULE_INTERACTION
    if (objectData.shadowCasterFade.z <= 0.5)
        ApplyGrassFootBend(objectData.grassCapsules, (uint)objectData.grassAnimation.w,
            mul(objectData.world, float4(grassRoot, 1)).xyz, worldUp / upLength,
            windHeight, restWorldPosition, restWorldNormal, restWorldTangent,
            worldPosition.xyz, worldNormal, worldTangent);
#endif
#ifdef GRASS_SURFACE_SHADING
    // Grass minimum width (RenderingEffects): a blade thinner than
    // placementFade.z pixels widens across itself (its UV u axis, the vertex
    // tangent) until it is that wide, so distant blades stay solid strokes
    // instead of breaking into sparkling dots. Centre-line vertices (u = 0.5:
    // midribs, tips) stay put. Blade widths are not stored per vertex; 1.2 cm
    // (scaled with the placement) is the generators' typical blade width.
    [branch] if ((draw.drawFlags & 0x20000000u) != 0u && objectData.placementFade.z > 0.0 &&
        objectData.shadowCasterFade.z <= 0.5)
    {
        const float tangentLength = length(worldTangent);
        if (tangentLength > 1e-5)
        {
            const float depth = distance(worldPosition.xyz, objectData.viewPositionAlphaCutoff.xyz);
            const float minimumWidth = objectData.placementFade.z * objectData.placementFade.w * depth;
            const float bladeWidth = 0.012 * clamp(length(grassScaleRow), 0.25, 4.0);
            // Plants shrinking at the range edge (placementScale) thin out too.
            const float extra = max(0.0, minimumWidth - bladeWidth) * placementScale;
            worldPosition.xyz += worldTangent / tangentLength * ((saturate(uv.x) - 0.5) * extra);
        }
    }
#endif
    oPos = mul(objectData.viewProjection, worldPosition);
    // Portal apertures are stencil masks rather than visible surfaces. Keep a
    // mask in front of the near plane rasterizable while the camera approaches
    // it, otherwise the connected view abruptly disappears before crossing.
    if ((draw.drawFlags & 0x40000000u) != 0u && oPos.w > 0.0)
        oPos.z = min(oPos.z, oPos.w); // reverse-Z: the near plane is z = w
    // Portal depth reset draws reuse the procedural aperture geometry but
    // place it at the far plane, clearing local-scene depth only inside the
    // aperture's stencil mask before the connected view is rendered.
    if ((draw.drawFlags & 0x80000000u) != 0u)
        oPos.z = 0.0; // reverse-Z far plane
    oWorldPos = shadeAtRest ? shadingPosition : worldPosition.xyz;
    oNormal = worldNormal;
    oUv = objectData.spriteUvRect.xy + localUv.xy * objectData.spriteUvRect.zw;
    oUv1 = localUv.zw;
    oUvChannels0 = float4(localUv.xy, fullUv0.zw);
    oUvChannels1 = float4(localUv.zw, fullUv1.zw);
    oUvChannels2 = fullUv2;
    oUvChannels3 = fullUv3;
    oUvChannels4 = fullUv4;
    oColor = color;
    // Billboard impostor cards carry their signed dither weight in the spare
    // w of UV set 4 (their materials never sample that set).
    if (((uint)objectData.materialParams.w & 1024u) != 0u)
    {
        oUvChannels4.z = billboardStart;
        oUvChannels4.w = billboardFade;
    }
    // Shrubs and bushes: their range-edge opacity (see placementDissolve).
    // Ground-cover materials never sample UV set 4.
    else if (placementDissolve)
        oUvChannels4.w = placementOpacity;
#ifdef GRASS_SURFACE_SHADING
    if ((draw.drawFlags & 0x20000000u) != 0u)
    {
        oColor.rgb = GrassColorVariation(oColor.rgb,
            mul(objectData.world, float4(grassRoot, 1)).xyz);
        oColor.a = 1.0; // the wind response above, not coverage
    }
#endif
    // Distant tiles are lit here only when the pixel shader has neither the
    // long-range shadow map nor the macro color map to light them with.
    if (((uint)objectData.materialParams.w & 16384u) != 0u &&
        objectData.terrainSunShadow.z <= 0.0 && objectData.terrainMacroParams.z <= 0.5)
    {
        float sunFacing = saturate(dot(oNormal,
            objectData.bakedLightDirection.xyz));
        const float3 sunLight = objectData.bakedDirectional.rgb *
            (sunFacing * 0.31830988618);
        const float3 light = objectData.ambientUnlit.rgb + sunLight;
        oColor.rgb *= light;
        // Alpha carries the sun's share of the light (distant tiles are
        // opaque), so the pixel shader can remove it where shadowed.
        const float3 luma = float3(0.2126, 0.7152, 0.0722);
        oColor.a = saturate(dot(sunLight, luma) / max(dot(light, luma), 1.0e-4));
    }
    if (objectData.morphParams.z > 0.0)
    {
        uint selectedJoint = (uint)objectData.morphParams.z - 1u;
        float weight = 0.0;
        [unroll] for (uint influence = 0; influence < 4; ++influence)
        {
            if ((uint)joints0[influence] == selectedJoint)
                weight += weights0[influence];
            if ((uint)joints1[influence] == selectedJoint)
                weight += weights1[influence];
        }
        weight = saturate(weight);
        float3 heat = weight < 0.3333
            ? lerp(float3(0.70, 0.70, 0.70), float3(0.13, 0.45, 1.0),
                weight * 3.0)
            : (weight < 0.6667
                ? lerp(float3(0.13, 0.45, 1.0), float3(1.0, 0.92, 0.12),
                    (weight - 0.3333) * 3.0)
                : lerp(float3(1.0, 0.92, 0.12), float3(1.0, 0.12, 0.08),
                    (weight - 0.6667) * 3.0));
        oColor = float4(heat, 1.0);
    }
    oTangent = float4(normalize(worldTangent), tangent.w);
}

// Compact indexed terrain input. Terrain has no skin, morph target, secondary
// UV, or authored tangent stream. Its UVs are planar X/Z coordinates, so
// reconstruct a tangent aligned with +X and project it onto the surface.
void VSTerrainMain(
    uint instanceId : SV_InstanceID,
    float3 pos : POSITION,
    float3 normal : NORMAL,
    float2 uv : TEXCOORD,
    float4 color : COLOR,
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
    oObjectIndex = ResolveObjectIndex(instanceId);
    ObjectData objectData = objects[oObjectIndex];
    oInstanceLodDistance = -1.0;
    const float3 localNormal = normalize(normal);
    float3 localTangent = float3(1.0, 0.0, 0.0) -
        localNormal * localNormal.x;
    if (dot(localTangent, localTangent) < 0.000001)
    {
        localTangent = float3(0.0, 0.0, 1.0) -
            localNormal * localNormal.z;
    }
    localTangent = normalize(localTangent);
    const float4 worldPosition = mul(objectData.world, float4(pos, 1.0));
    oPos = mul(objectData.viewProjection, worldPosition);
    if ((draw.drawFlags & 0x40000000u) != 0u && oPos.w > 0.0)
        oPos.z = min(oPos.z, oPos.w); // reverse-Z: the near plane is z = w
    if ((draw.drawFlags & 0x80000000u) != 0u)
        oPos.z = 0.0; // reverse-Z far plane
    oWorldPos = worldPosition.xyz;
    oNormal = normalize(mul((float3x3)objectData.world, localNormal));
    oUv = objectData.spriteUvRect.xy + uv * objectData.spriteUvRect.zw;
    oUv1 = uv;
    oUvChannels0 = float4(uv, 0.0, 0.0);
    oUvChannels1 = float4(uv, 0.0, 0.0);
    oUvChannels2 = 0.0;
    oUvChannels3 = 0.0;
    oUvChannels4 = 0.0;
    oColor = color;
    // cross(+Y, +X) points toward -Z, while terrain V increases toward +Z.
    oTangent = float4(normalize(mul((float3x3)objectData.world,
        localTangent)), -1.0);
}

static const float PI = 3.14159265359;

float DistributionGGX(float3 n, float3 h, float roughness)
{
    float alpha = roughness * roughness;
    float alpha2 = alpha * alpha;
    float nDotH = saturate(dot(n, h));
    float denominator = nDotH * nDotH * (alpha2 - 1.0) + 1.0;
    return alpha2 / max(PI * denominator * denominator, 0.000001);
}

float GeometrySchlickGGX(float nDotDirection, float roughness)
{
    float r = roughness + 1.0;
    float k = r * r / 8.0;
    return nDotDirection /
        max(nDotDirection * (1.0 - k) + k, 0.000001);
}

float GeometrySmith(float3 n, float3 v, float3 l, float roughness)
{
    return GeometrySchlickGGX(saturate(dot(n, v)), roughness) *
        GeometrySchlickGGX(saturate(dot(n, l)), roughness);
}

float3 FresnelSchlick(float cosine, float3 f0)
{
    return f0 + (1.0 - f0) * pow(saturate(1.0 - cosine), 5.0);
}

float3 RotateEnvironmentDirection(float3 direction, float radians)
{
    float sine = sin(-radians);
    float cosine = cos(-radians);
    direction.xz = float2(
        cosine * direction.x - sine * direction.z,
        sine * direction.x + cosine * direction.z);
    return direction;
}

void EnvironmentBasis(float3 direction, out float basis[9])
{
    basis[0] = 0.282095;
    basis[1] = 0.488603 * direction.y;
    basis[2] = 0.488603 * direction.z;
    basis[3] = 0.488603 * direction.x;
    basis[4] = 1.092548 * direction.x * direction.y;
    basis[5] = 1.092548 * direction.y * direction.z;
    basis[6] = 0.315392 * (3.0 * direction.z * direction.z - 1.0);
    basis[7] = 1.092548 * direction.x * direction.z;
    basis[8] = 0.546274 * (direction.x * direction.x - direction.y * direction.y);
}

float3 EvaluateEnvironmentRadiance(ObjectData objectData, float3 direction)
{
    float basis[9];
    EnvironmentBasis(RotateEnvironmentDirection(direction,
        objectData.environmentParams.y), basis);
    float3 radiance = 0.0;
    [unroll]
    for (uint coefficient = 0; coefficient < 9; ++coefficient)
        radiance += objectData.environmentSH[coefficient].rgb * basis[coefficient];
    return max(radiance, 0.0);
}

float3 EvaluateCustomReflectionRadiance(ObjectData objectData, float3 direction)
{
    float basis[9];
    EnvironmentBasis(RotateEnvironmentDirection(direction,
        objectData.reflectionEnvironmentParams.y), basis);
    float3 radiance = 0.0;
    [unroll]
    for (uint coefficient = 0; coefficient < 9; ++coefficient)
        radiance += objectData.reflectionEnvironmentSH[coefficient].rgb *
            basis[coefficient];
    return max(radiance, 0.0) * objectData.reflectionEnvironmentParams.x;
}

float2 CubeAtlasUv(float3 direction, float lod, uint atlasWidth)
{
    float3 absoluteDirection = abs(direction);
    float2 local;
    uint face;
    if (absoluteDirection.x >= absoluteDirection.y &&
        absoluteDirection.x >= absoluteDirection.z)
    {
        if (direction.x >= 0.0)
        {
            face = 0;
            local = float2(-direction.z, direction.y) / absoluteDirection.x;
        }
        else
        {
            face = 1;
            local = float2(direction.z, direction.y) / absoluteDirection.x;
        }
    }
    else if (absoluteDirection.y >= absoluteDirection.z)
    {
        if (direction.y >= 0.0)
        {
            face = 2;
            local = float2(direction.x, -direction.z) / absoluteDirection.y;
        }
        else
        {
            face = 3;
            local = float2(direction.x, direction.z) / absoluteDirection.y;
        }
    }
    else if (direction.z >= 0.0)
    {
        face = 4;
        local = float2(direction.x, direction.y) / absoluteDirection.z;
    }
    else
    {
        face = 5;
        local = float2(-direction.x, direction.y) / absoluteDirection.z;
    }
    local = float2(local.x * 0.5 + 0.5, 0.5 - local.y * 0.5);
    const float baseFaceSize = max((float)atlasWidth / 3.0, 1.0);
    const float faceSize = max(baseFaceSize / exp2(floor(lod)), 1.0);
    const float inset = 0.5 / faceSize;
    local = clamp(local, inset, 1.0 - inset);
    return (float2((float)(face % 3), (float)(face / 3)) + local) /
        float2(3.0, 2.0);
}

float3 EvaluateDirectReflectionRadiance(
    ObjectData objectData, float3 direction, float roughness)
{
    const bool customEnvironment =
        objectData.reflectionEnvironmentParams.z > 0.5;
    const float rotation = customEnvironment
        ? objectData.reflectionEnvironmentParams.y
        : objectData.environmentParams.y;
    direction = RotateEnvironmentDirection(direction, rotation);
    uint width = 0, height = 0, mipLevels = 1;
    environmentMap.GetDimensions(0, width, height, mipLevels);
    // The cached 3x2 cubemap atlas stores one GGX convolution per mip.
    const float lod = roughness * max((float)mipLevels - 1.0, 0.0);
    const float2 uv = CubeAtlasUv(direction, lod, width);
    const float exposure = customEnvironment
        ? objectData.reflectionEnvironmentParams.x
        : objectData.environmentParams.x;
    return max(environmentMap.SampleLevel(materialSampler, uv, lod).rgb, 0.0) *
        exposure;
}

float3 EvaluateEnvironmentIrradiance(ObjectData objectData, float3 normal)
{
    float basis[9];
    EnvironmentBasis(RotateEnvironmentDirection(normal,
        objectData.environmentParams.y), basis);
    const float bandScale[9] = {
        PI, 2.0943951, 2.0943951, 2.0943951,
        0.7853982, 0.7853982, 0.7853982, 0.7853982, 0.7853982 };
    float3 irradiance = 0.0;
    [unroll]
    for (uint coefficient = 0; coefficient < 9; ++coefficient)
        irradiance += objectData.environmentSH[coefficient].rgb *
            basis[coefficient] * bandScale[coefficient];
    return max(irradiance, 0.0);
}

float3 EvaluatePbrBrdf(
    float3 baseColor, float metallic, float roughness, float3 f0,
    float3 n, float3 v, float3 l, float materialDetail)
{
    // Use the base tint for rough metals too when distant reflections are omitted.
    const float3 distantDiffuse = baseColor / PI;
    [branch] if (materialDetail <= 0.0) return distantDiffuse;
    float3 h = normalize(v + l);
    float3 fresnel = FresnelSchlick(saturate(dot(h, v)), f0);
    float distribution = DistributionGGX(n, h, roughness);
    float geometry = GeometrySmith(n, v, l, roughness);
    float denominator = max(
        4.0 * saturate(dot(n, v)) * saturate(dot(n, l)), 0.0001);
    float3 specular = distribution * geometry * fresnel / denominator;
    float3 diffuseWeight = (1.0 - fresnel) * (1.0 - metallic);
    return lerp(distantDiffuse, diffuseWeight * baseColor / PI + specular, materialDetail);
}

// Explicit quality policy adds bias to the hardware's derivative-based mip selection.
float TextureLodBlend(ObjectData data, float3 worldPosition)
{
    if (data.textureLodParams.w < 0.5) return 0.0;
    return smoothstep(data.textureLodParams.x, data.textureLodParams.y,
        distance(worldPosition, data.viewPositionAlphaCutoff.xyz));
}
float TextureLodBias(ObjectData data, float3 worldPosition)
{
    return TextureLodBlend(data, worldPosition) * data.textureLodParams.z +
        data.viewTextureParams.x;
}

// Mip bias of alpha-tested coverage: the texture LOD's alpha bias plus the
// view's upscaling bias.
float AlphaLodBias(ObjectData data, float3 worldPosition)
{
    return TextureLodBlend(data, worldPosition) * data.alphaLodParams.x +
        data.viewTextureParams.x;
}

float AlphaCoverageHash(int2 cell, int level)
{
    uint hash = (uint)cell.x * 1664525u + (uint)cell.y * 1013904223u +
        (uint)level * 747796405u;
    hash ^= hash >> 16u;
    hash *= 2246822519u;
    hash ^= hash >> 13u;
    hash *= 3266489917u;
    hash ^= hash >> 16u;
    return (hash & 0x00ffffffu) * (1.0 / 16777216.0);
}

// Fractional leaf coverage without the sorted transparent pass, as hashed
// alpha testing (Wyman and McGuire 2017). The pattern is anchored to the
// surface's texture coordinates, so it rides along with wind-swayed cards and
// does not crawl as the camera moves, and its cells are sized to about one
// pixel at every distance. (A fixed 2 mm world-space pattern was subpixel on
// distant trees: any sway or camera motion resampled it, so low-LOD leaves
// sparkled every frame.) Two neighbouring power-of-two cell sizes blend as
// the footprint changes, and the blend is remapped through its CDF so the
// discarded fraction stays exact.
// uvDx/uvDy must be taken before any clip(): a discard may end quad lanes.
float AlphaCoverageThreshold(float2 uv, float2 uvDx, float2 uvDy)
{
    const float footprint = max(max(length(uvDx), length(uvDy)), 1.0e-7);
    const float level = -log2(footprint);
    const float lower = floor(level);
    const float t = level - lower;
    const float a = AlphaCoverageHash(int2(floor(uv * exp2(lower))), (int)lower);
    const float b = AlphaCoverageHash(int2(floor(uv * exp2(lower + 1.0))), (int)lower + 1);
    const float x = lerp(a, b, t);
    const float m = min(t, 1.0 - t);
    if (m < 1.0e-4) return x;
    const float spread = 2.0 * m * (1.0 - m);
    if (x < m) return x * x / spread;
    if (x < 1.0 - m) return (x - 0.5 * m) / (1.0 - m);
    return 1.0 - (1.0 - x) * (1.0 - x) / spread;
}

// edgeFade is CardEdgeFade(): besides eroding alpha it dissolves coverage, so
// cards whose texels are fully opaque still fade smoothly as they turn
// edge-on instead of streaking until a hard cut. One hash serves both: a
// pixel survives when transmission <= h < transmission + edgeFade *
// (1 - transmission), so the two coverages multiply exactly.
void ApplyMaskedAlphaTransmission(ObjectData data, float2 uv, float2 uvDx, float2 uvDy,
    float edgeFade)
{
    const float transmission = saturate(data.alphaLodParams.z);
    [branch] if (transmission > 0.0 || edgeFade < 1.0)
    {
        const float h = AlphaCoverageThreshold(uv, uvDx, uvDy);
        clip(h - transmission);
        clip(transmission + saturate(edgeFade) * (1.0 - transmission) - h);
    }
}

// Flat triangle normal (unnormalized) from position derivatives. Take it
// before any clip(): a discard may end quad lanes and break derivatives.
float3 SurfaceFacet(float3 worldPosition)
{
    return cross(ddx(worldPosition), ddy(worldPosition));
}

// Masked cards seen edge-on smear their texels into streaks; low-LOD tree
// crosses and horizontal canopy planes are the worst case. Scaling alpha by
// how squarely the card faces the camera erodes its least opaque texels
// first, so a turning card thins out while the overlapping cards that face
// the viewer carry the silhouette. The flat triangle normal comes from
// position derivatives: authored card normals are often bent toward the
// canopy for lighting and would not reveal the card's real orientation.
// facet is SurfaceFacet(worldPosition), taken before any clip(). Shadow views
// leave the strength zero.
float CardEdgeFade(ObjectData data, float3 worldPosition, float3 facet)
{
    const float strength = data.alphaLodParams.w;
    [branch] if (strength <= 0.0 || ((uint)data.materialParams.w & 32u) == 0u)
        return 1.0;
    const float facetLength = length(facet);
    if (facetLength <= 1.0e-12) return 1.0;
    const float3 toCamera = normalize(data.viewPositionAlphaCutoff.xyz - worldPosition);
    const float facing = abs(dot(facet / facetLength, toCamera));
    return saturate((facing - 0.1 * strength) / (0.35 * strength));
}

void ApplyAlphaLod(inout ObjectData data, float3 worldPosition)
{
    if (data.alphaLodParams.y > 0.5 && TextureLodBlend(data, worldPosition) >= 1.0)
        data.materialParams.w = (float)((uint)data.materialParams.w & ~32u);
}

// Alpha quality is independent of color quality; zero bias preserves coverage.
// Color and shadow masks use the same distance-based alpha bias.
// Editor/capture Detail render mode (SceneRenderMode::Detail): the CPU writes
// morphParams.z = -100, or -1000 - (mean triangle edge, m) for meshes without
// instanced LOD bands. g_detailTexture is how well the resident base color
// texture covers this pixel: 1 at one texel per pixel or more, falling to 0
// at one texel per 8 pixels (blurred by streaming, distance mips or a
// texture too small for the view).
static float g_detailTexture = 1.0;
// Editor Triangles view (morphParams.z = -20000): every triangle gets its
// own colour, so triangle size and count read directly as geometric detail.
// The entry points record SV_PrimitiveID and the resolved object record.
static uint g_primitiveId = 0u;
static uint g_triangleObject = 0u;
bool IsTriangleDraw(ObjectData data) { return data.morphParams.z < -15000.0; }
bool IsDetailHeatDraw(ObjectData data)
{
    return data.morphParams.z < -50.0 && !IsTriangleDraw(data);
}

float4 SampleObjectBaseColor(ObjectData data, float2 sourceUv, float textureBias,
    float alphaMultiplier, float alphaBias)
{
    [branch] if (IsDetailHeatDraw(data))
    {
        // Streamed textures are created from their resident mips, so level
        // 0 of the bound view is the finest resident level.
        uint width, height, levels;
        baseColorMap.GetDimensions(0, width, height, levels);
        const float2 size = float2(width, height);
        const float2 dx = ddx(sourceUv) * size, dy = ddy(sourceUv) * size;
        // Texels the finest resident level spans per screen pixel.
        const float texelsPerPixel = sqrt(max(max(dot(dx, dx), dot(dy, dy)), 1.0e-12));
        g_detailTexture = saturate(1.0 + log2(texelsPerPixel) / 3.0);
    }
    [branch] if ((uint)data.materialParams.w & 32u)
    {
        float4 color = baseColorMap.SampleBias(materialSampler, sourceUv, alphaBias);
        clip(color.a * alphaMultiplier - data.viewPositionAlphaCutoff.w);
        // Only surviving pixels fetch a separate color mip when needed.
        [branch] if (textureBias != alphaBias)
            color.rgb = baseColorMap.SampleBias(materialSampler, sourceUv, textureBias).rgb;
        return color;
    }
    return baseColorMap.SampleBias(materialSampler, sourceUv, textureBias);
}

// Height textures are white = raised unless the material marks them as depth
// maps (parallaxParams.w bit 0). The march below works in depth below the
// surface, so a raised texel has depth 0.
// Tangent-space normal from either a 3-channel map or a BC5 (two-channel)
// map. BC5 samples return blue = 0, which no valid tangent-space normal
// encodes (z >= 0), so Z is rebuilt from XY in that case.
float3 UnpackNormal(float3 encoded)
{
    float3 n = encoded * 2.0 - 1.0;
    if (encoded.z <= 0.0)
        n.z = sqrt(saturate(1.0 - dot(n.xy, n.xy)));
    return n;
}

// World-space triplanar sampling (Material::triplanarScale). p is the world
// position in texture repeats; dx/dy its screen derivatives, taken in uniform
// control flow by the caller so each projection can be fetched inside a
// branch. Projections: X faces map (z, -y), Y faces (x, z), Z faces (x, -y),
// so +v runs down the world on the side faces like the mesh importers' UVs.
float4 SampleTriplanar(Texture2D map, float3 p, float3 dx, float3 dy, float3 w)
{
    float4 result = 0.0;
    [branch] if (w.x > 0.0)
        result += map.SampleGrad(materialSampler, float2(p.z, -p.y),
            float2(dx.z, -dx.y), float2(dy.z, -dy.y)) * w.x;
    [branch] if (w.y > 0.0)
        result += map.SampleGrad(materialSampler, p.xz, dx.xz, dy.xz) * w.y;
    [branch] if (w.z > 0.0)
        result += map.SampleGrad(materialSampler, float2(p.x, -p.y),
            float2(dx.x, -dx.y), float2(dy.x, -dy.y)) * w.z;
    return result;
}

// Triplanar normal mapping: each projection's tangent-space normal is turned
// into a world-space offset along that projection's u and v axes (UDN blend)
// and added to the geometry normal.
float3 SampleTriplanarNormal(float3 p, float3 dx, float3 dy, float3 w,
    float3 geometryNormal, float strength)
{
    float3 offset = 0.0;
    [branch] if (w.x > 0.0)
    {
        const float3 t = UnpackNormal(normalMap.SampleGrad(materialSampler,
            float2(p.z, -p.y), float2(dx.z, -dx.y), float2(dy.z, -dy.y)).xyz);
        offset += w.x * float3(0.0, -t.y, t.x);
    }
    [branch] if (w.y > 0.0)
    {
        const float3 t = UnpackNormal(normalMap.SampleGrad(materialSampler,
            p.xz, dx.xz, dy.xz).xyz);
        offset += w.y * float3(t.x, 0.0, t.y);
    }
    [branch] if (w.z > 0.0)
    {
        const float3 t = UnpackNormal(normalMap.SampleGrad(materialSampler,
            float2(p.x, -p.y), float2(dx.x, -dx.y), float2(dy.x, -dy.y)).xyz);
        offset += w.z * float3(t.x, -t.y, 0.0);
    }
    // Keep only the part across the surface so the offset cannot flip it.
    offset -= geometryNormal * dot(offset, geometryNormal);
    return normalize(geometryNormal + offset * strength);
}

float SampleParallaxDepth(float2 uv, float2 uvDx, float2 uvDy, bool heightIsDepth)
{
    const float value = heightMap.SampleGrad(materialSampler, uv, uvDx, uvDy).r;
    return heightIsDepth ? value : 1.0 - value;
}

// parallaxParams.w packs the parallax flags in its integer part and the
// self-shadow strength (0..1, stored as 0..0.5) in its fraction.
static const uint ParallaxFlagHeightIsDepth = 1u;
static const uint ParallaxFlagSelfShadow = 2u;
static const uint ParallaxFlagClampUv = 4u;
static const uint ParallaxFlagDepthOffset = 8u;
static const uint ParallaxFlagCostView = 16u; // editor: fetch-count heat map

// Clip-space depth of the parallax hit, written by PSMain for
// PSParallaxDepthMain. Negative = no relief offset for this pixel.
static float g_parallaxClipDepth = -1.0;

struct ParallaxHit
{
    float2 uv;
    float depth;       // 0 = surface, 1 = heightScale below it
    float2 sampleDx;   // gradients every relief fetch uses
    float2 sampleDy;
    bool marched;      // false when the relief is below half a pixel
    float fetches;     // height-map samples taken (cost view)
};

// uvDx/uvDy are the screen derivatives of sourceUv, taken by the caller in
// uniform control flow. Every step samples with those gradients so the height
// fetches use the mip the surface footprint needs (plus the texture LOD bias)
// instead of the full-resolution level.
//
// A coarse linear march (half the authored step count) brackets the first
// intersection, then binary search plus a final secant step refines it.
// Refinement converges far faster than more linear steps, so edges stay
// smooth with fewer fetches (contact refinement).
ParallaxHit ParallaxOcclusion(
    float2 sourceUv, float2 uvDx, float2 uvDy, float3 viewDirectionTangent,
    float heightScale, float minimumSteps, float maximumSteps,
    float textureBias, bool heightIsDepth)
{
    ParallaxHit hit;
    hit.uv = sourceUv;
    hit.depth = 0.0;
    const float gradientScale = exp2(textureBias);
    hit.sampleDx = uvDx * gradientScale;
    hit.sampleDy = uvDy * gradientScale;
    hit.marched = false;
    hit.fetches = 0.0;

    float viewAngle = saturate(abs(viewDirectionTangent.z));
    float2 ray = viewDirectionTangent.xy /
        max(abs(viewDirectionTangent.z), 0.08) * heightScale;

    // Largest on-screen shift, in pixels, that the relief can produce. Below
    // about half a pixel parallax is invisible, so distant and grazing-far
    // surfaces skip the march entirely. With refinement, one linear step per
    // two traversed pixels is enough to find the right intersection.
    const float uvPerPixel = sqrt(max(abs(uvDx.x * uvDy.y - uvDx.y * uvDy.x),
        1e-14));
    const float parallaxPixels = length(ray) / uvPerPixel;
    [branch] if (parallaxPixels < 0.5)
        return hit;
    hit.marched = true;
    float layerCount = round(lerp(maximumSteps, minimumSteps, viewAngle) * 0.5);
    layerCount = clamp(min(layerCount, ceil(parallaxPixels * 0.5)), 4.0, 32.0);
    float layerDepth = 1.0 / layerCount;

    float currentLayerDepth = 0.0;
    float currentDepth = SampleParallaxDepth(sourceUv, hit.sampleDx,
        hit.sampleDy, heightIsDepth);
    float previousDepth = currentDepth;
    [loop]
    for (int step = 0; step < 32; ++step)
    {
        if (currentLayerDepth >= currentDepth || step >= (int)layerCount)
            break;
        previousDepth = currentDepth;
        currentLayerDepth += layerDepth;
        currentDepth = SampleParallaxDepth(sourceUv - ray * currentLayerDepth,
            hit.sampleDx, hit.sampleDy, heightIsDepth);
    }
    hit.fetches = 1.0 + round(currentLayerDepth * layerCount);
    [branch] if (currentLayerDepth <= 0.0)
        return hit; // raised texel right at the surface
    hit.fetches += 4.0;

    // f(t) = t - depth(t): negative above the relief, >= 0 inside it.
    float above = currentLayerDepth - layerDepth;
    float below = currentLayerDepth;
    float aboveError = above - previousDepth;
    float belowError = below - currentDepth;
    [unroll]
    for (int refine = 0; refine < 4; ++refine)
    {
        float middle = 0.5 * (above + below);
        float middleDepth = SampleParallaxDepth(sourceUv - ray * middle,
            hit.sampleDx, hit.sampleDy, heightIsDepth);
        float middleError = middle - middleDepth;
        if (middleError >= 0.0) { below = middle; belowError = middleError; }
        else { above = middle; aboveError = middleError; }
    }
    float errorRange = belowError - aboveError;
    float t = errorRange > 0.00001
        ? lerp(above, below, saturate(-aboveError / errorRange)) : below;
    hit.uv = sourceUv - ray * t;
    hit.depth = t;
    return hit;
}

// Soft self-shadow from the relief toward one light (Tatarchuk 2006): march
// from the hit point up to the surface and keep the largest blocker height,
// weighted so near blockers cast harder shadows than distant ones.
// Returns 1 = lit, 0 = fully occluded.
float ParallaxSelfShadow(ParallaxHit hit, float3 lightDirectionTangent,
    float heightScale, bool heightIsDepth, float steps)
{
    [branch] if (hit.depth <= 0.001 || lightDirectionTangent.z <= 0.0)
        return 1.0;
    // UV shift per unit of depth climbed toward the light.
    const float2 ray = lightDirectionTangent.xy /
        max(lightDirectionTangent.z, 0.08) * heightScale;
    const float stepDepth = hit.depth / steps;
    float occlusion = 0.0;
    [loop]
    for (float i = 1.0; i <= steps; i += 1.0)
    {
        const float rayDepth = hit.depth - stepDepth * i;
        const float surfaceDepth = SampleParallaxDepth(
            hit.uv + ray * (stepDepth * i), hit.sampleDx, hit.sampleDy,
            heightIsDepth);
        // Positive when the relief rises above the ray toward the light.
        occlusion = max(occlusion,
            (rayDepth - surfaceDepth) * (1.0 - (i - 1.0) / steps));
    }
    return 1.0 - saturate(occlusion * 8.0);
}

// Gather returns the same four depths as a bilinear comparison footprint.
// Clamp its origin inside the cascade tile; swizzles replicate edge texels
// without allowing adjacent atlas tiles (or a wrapping material sampler) in.
float4 GatherShadowDepths(int2 origin, int2 tileMinimum, int2 tileMaximum,
    float2 atlasSize)
{
    int2 safeOrigin = clamp(origin, tileMinimum, tileMaximum - 1);
    float4 depths = shadowMap.GatherRed(materialSampler,
        (float2(safeOrigin) + 1.0) / atlasSize).wzxy;
    if (origin.x < tileMinimum.x) depths = depths.xxzz;
    else if (origin.x >= tileMaximum.x) depths = depths.yyww;
    if (origin.y < tileMinimum.y) depths = depths.xyxy;
    else if (origin.y >= tileMaximum.y) depths = depths.zwzw;
    return depths;
}

// One atlas tile's filtered shadow at a light clip-space position. Tiles are
// square: 2 x 2 of them, or 2 x 3 with the refresh blend's spares
// (ObjectData::shadowAtlasTiles); a single cascade uses the whole map.
float SampleDirectionalShadowTile(ObjectData objectData,
    uint tile, float3 projected, int pcfRadius)
{
    float2 shadowUv = float2(projected.x * 0.5 + 0.5,
        0.5 - projected.y * 0.5);
    if (projected.z <= 0.0 || projected.z >= 1.0 ||
        any(shadowUv <= 0.0) || any(shadowUv >= 1.0))
        return 1.0;

    uint width = 0, height = 0;
    shadowMap.GetDimensions(width, height);
    uint cascadeCount = clamp((uint)objectData.shadowCascadeData.x, 1u, 4u);
    uint tileWidth = cascadeCount > 1u ? width / 2u : width;
    uint tileHeight = cascadeCount > 1u ? tileWidth : height;
    int2 tileOrigin = cascadeCount > 1u
        ? int2((tile & 1u) * tileWidth, (tile >> 1u) * tileHeight)
        : int2(0, 0);
    int radius = clamp(pcfRadius, 0, 4);
    // Interpolate the comparison result between neighboring texels. Snapping
    // the PCF kernel to an integer center makes shadow edges visibly blocky.
    float2 texelPosition = shadowUv * float2(tileWidth, tileHeight) - 0.5;
    int2 center = tileOrigin + int2(floor(texelPosition));
    float2 texelFraction = frac(texelPosition);
    int2 tileMaximum = tileOrigin + int2(
        (int)tileWidth - 1, (int)tileHeight - 1);
    // Shadow maps are reverse-Z (near the light = 1): a receiver is lit when
    // its biased depth is at least the stored caster depth.
    // Terrain asks for a single bilinear shadow tap. Keep that path to four
    // depth reads instead of entering the general PCF loop for every pixel.
    [branch] if (radius == 0)
    {
        float receiverDepth = projected.z + objectData.shadowParams.x;
        float4 comparisons = float4(receiverDepth.xxxx >= GatherShadowDepths(
            center, tileOrigin, tileMaximum, float2(width, height)));
        float s00 = comparisons.x, s10 = comparisons.y;
        float s01 = comparisons.z, s11 = comparisons.w;
        return lerp(lerp(s00, s10, texelFraction.x),
            lerp(s01, s11, texelFraction.x), texelFraction.y);
    }
    float receiverDepth = projected.z + objectData.shadowParams.x;
    float litSamples = 0.0;
    [branch] if (radius == 1)
    {
        // Four gathers cover the existing weighted 4x4 footprint exactly.
        [unroll] for (int y = -1; y <= 1; y += 2)
        [unroll] for (int x = -1; x <= 1; x += 2)
        {
            float2 weightsX = x == -1 ? float2(1.0 - texelFraction.x, 1.0)
                : float2(1.0, texelFraction.x);
            float2 weightsY = y == -1 ? float2(1.0 - texelFraction.y, 1.0)
                : float2(1.0, texelFraction.y);
            float4 comparisons = float4(receiverDepth.xxxx >= GatherShadowDepths(
                center + int2(x, y), tileOrigin, tileMaximum, float2(width, height)));
            litSamples += dot(comparisons, weightsX.xyxy * weightsY.xxyy);
        }
        return litSamples / 9.0;
    }
    [branch] if (radius == 2)
    {
        [unroll] for (int y = -2; y <= 3; ++y)
        [unroll] for (int x = -2; x <= 3; ++x)
        {
            float weightX = x == -2 ? 1.0 - texelFraction.x :
                (x == 3 ? texelFraction.x : 1.0);
            float weightY = y == -2 ? 1.0 - texelFraction.y :
                (y == 3 ? texelFraction.y : 1.0);
            int2 coordinate = clamp(center + int2(x, y),
                tileOrigin, tileMaximum);
            float storedDepth = shadowMap.Load(int3(coordinate, 0));
            litSamples += (receiverDepth >= storedDepth ? 1.0 : 0.0) *
                weightX * weightY;
        }
        return litSamples / 25.0;
    }
    const int minimumOffset = -radius;
    const int maximumOffset = radius + 1;
    [loop] for (int y = minimumOffset; y <= maximumOffset; ++y)
    [loop] for (int x = minimumOffset; x <= maximumOffset; ++x)
    {
        float weightX = x == minimumOffset ? 1.0 - texelFraction.x :
            (x == maximumOffset ? texelFraction.x : 1.0);
        float weightY = y == minimumOffset ? 1.0 - texelFraction.y :
            (y == maximumOffset ? texelFraction.y : 1.0);
        int2 coordinate = clamp(center + int2(x, y),
            tileOrigin, tileMaximum);
        float storedDepth = shadowMap.Load(int3(coordinate, 0));
        litSamples += (receiverDepth >= storedDepth ? 1.0 : 0.0) *
            weightX * weightY;
    }
    float diameter = 2.0 * radius + 1.0;
    return litSamples / (diameter * diameter);
}

// The previous image during a refresh blend: one bilinear comparison (four
// depths), not the full PCF kernel. Every shadow call site is inlined, so a
// second full kernel per site made Object.hlsl take many minutes to compile;
// the image being faded out needs no more than this for ~20 frames.
float SampleDirectionalShadowTileBilinear(ObjectData objectData,
    uint tile, float3 projected)
{
    float2 shadowUv = float2(projected.x * 0.5 + 0.5,
        0.5 - projected.y * 0.5);
    if (projected.z <= 0.0 || projected.z >= 1.0 ||
        any(shadowUv <= 0.0) || any(shadowUv >= 1.0))
        return 1.0;
    uint width = 0, height = 0;
    shadowMap.GetDimensions(width, height);
    uint tileSize = width / 2u;
    int2 tileOrigin = int2((tile & 1u) * tileSize, (tile >> 1u) * tileSize);
    int2 tileMaximum = tileOrigin + int2((int)tileSize - 1, (int)tileSize - 1);
    float2 texelPosition = shadowUv * float(tileSize) - 0.5;
    float2 texelFraction = frac(texelPosition);
    float receiverDepth = projected.z + objectData.shadowParams.x;
    float4 comparisons = float4(receiverDepth.xxxx >= GatherShadowDepths(
        tileOrigin + int2(floor(texelPosition)), tileOrigin, tileMaximum,
        float2(width, height)));
    return lerp(lerp(comparisons.x, comparisons.y, texelFraction.x),
        lerp(comparisons.z, comparisons.w, texelFraction.x), texelFraction.y);
}

// A cascade's filtered shadow, from the tile that holds its image
// (ObjectData::shadowAtlasTiles).
float SampleDirectionalShadowCascade(ObjectData objectData,
    uint cascade, float3 biasedPosition, int pcfRadius)
{
    float4 lightClip = mul(objectData.shadowViewProjections[cascade],
        float4(biasedPosition, 1.0));
    if (lightClip.w <= 0.0)
        return 1.0;
    const uint tiles = (uint)objectData.shadowAtlasTiles.x;
    return SampleDirectionalShadowTile(objectData,
        (tiles >> (3u * cascade)) & 7u, lightClip.xyz / lightClip.w, pcfRadius);
}

// The same, for the sun's lighting: while the cascade fades in after a
// refresh (SceneRendererShadows refresh blend), its previous image, kept in
// another tile and reached through the new projection plus a clip offset,
// is blended out by the new image's weight. Only the lighting lookup blends
// (fog shafts and canopy taps do not): every call site is inlined, and
// blending them all more than doubled Object.hlsl's compile time.
float SampleDirectionalShadowCascadeBlended(ObjectData objectData,
    uint cascade, float3 biasedPosition, int pcfRadius)
{
    float4 lightClip = mul(objectData.shadowViewProjections[cascade],
        float4(biasedPosition, 1.0));
    if (lightClip.w <= 0.0)
        return 1.0;
    const float3 projected = lightClip.xyz / lightClip.w;
    const uint tiles = (uint)objectData.shadowAtlasTiles.x;
    float shadow = SampleDirectionalShadowTile(objectData,
        (tiles >> (3u * cascade)) & 7u, projected, pcfRadius);
    // The blend slot of this cascade, if one is active.
    const float2 info = objectData.shadowAtlasTiles.yz;
    const bool first = info.x >= 0.0 && ((uint)info.x & 7u) == cascade;
    const bool second = info.y >= 0.0 && ((uint)info.y & 7u) == cascade;
    [branch] if (first || second)
    {
        const float4 blend = first ? objectData.shadowRefreshBlend[0]
            : objectData.shadowRefreshBlend[1];
        const float previous = SampleDirectionalShadowTileBilinear(objectData,
            (uint)(first ? info.x : info.y) >> 3u, projected + blend.xyz);
        shadow = lerp(previous, shadow, saturate(blend.w));
    }
    return shadow;
}

// Terrain macro color map (TerrainManager::GetMacroColorMap), bound in the
// height slot (logical 5), which terrain does not use: linear albedo of the
// map sample under worldPosition, alpha = forest cover. Gradients come from
// the caller so it may be sampled inside divergent branches.
float4 SampleTerrainMacroColor(ObjectData objectData, float3 worldPosition,
    float3 worldDx, float3 worldDy)
{
    const float2 scale = objectData.terrainMacroMap.zw;
    return heightMap.SampleGrad(materialSampler,
        (worldPosition.xz - objectData.terrainMacroMap.xy) * scale,
        worldDx.xz * scale, worldDy.xz * scale);
}

// World-space terrain normal from the macro map's companion texture (normal
// slot, logical 2; terrainMacroParams.w = 1 when bound), 16 m samples.
float3 SampleTerrainMacroNormal(ObjectData objectData, float3 worldPosition,
    float3 worldDx, float3 worldDy)
{
    const float2 scale = objectData.terrainMacroMap.zw;
    const float3 encoded = normalMap.SampleGrad(materialSampler,
        (worldPosition.xz - objectData.terrainMacroMap.xy) * scale,
        worldDx.xz * scale, worldDy.xz * scale).xyz;
    return normalize(encoded * 2.0 - 1.0);
}

// Terrain contour paths (TerrainManager::ContourPaths). A path runs along a
// contour of smooth value noise (two octaves); its half-width is the distance
// to the contour, |value - contour| / |gradient|. Evaluated per pixel with an
// analytic gradient, so it is the same sharp track on every terrain level,
// near cell or far tile, with no data to stream. The hash and noise match the
// CPU's (LandscapeDriver ValueNoise) so vegetation keeps clear of the same
// track. Past the pixel footprint the soft edge widens and the strength falls
// by the same share, so a track narrower than a pixel stays a faint
// continuous line instead of breaking into spots.
uint TerrainPathHash(int x, int z, uint seed)
{
    uint h = asuint(x) * 0x9e3779b1u ^ asuint(z) * 0x85ebca77u ^ seed * 0xc2b2ae3du;
    h ^= h >> 15; h *= 0x2c1b3c6du; h ^= h >> 12; h *= 0x297a2d39u; h ^= h >> 15;
    return h;
}

// Value noise in 0..1 and its gradient (per metre).
float TerrainPathNoise(float2 position, float period, uint seed, out float2 gradient)
{
    const float2 p = position / period;
    const float2 cellOrigin = floor(p);
    const int2 cell = int2(cellOrigin);
    const float2 t = p - cellOrigin;
    const float2 s = t * t * (3.0 - 2.0 * t);
    const float2 ds = 6.0 * t * (1.0 - t);
    const float scale = 1.0 / 16777215.0;
    const float c00 = (TerrainPathHash(cell.x, cell.y, seed) & 0xffffffu) * scale;
    const float c10 = (TerrainPathHash(cell.x + 1, cell.y, seed) & 0xffffffu) * scale;
    const float c01 = (TerrainPathHash(cell.x, cell.y + 1, seed) & 0xffffffu) * scale;
    const float c11 = (TerrainPathHash(cell.x + 1, cell.y + 1, seed) & 0xffffffu) * scale;
    const float bottom = lerp(c00, c10, s.x), top = lerp(c01, c11, s.x);
    gradient = float2(lerp(c10 - c00, c11 - c01, s.y) * ds.x, (top - bottom) * ds.y) / period;
    return lerp(bottom, top, s.y);
}

// Path share (0..1) at worldPos, before the caller's surface rules.
// footprint: ground metres per pixel.
float TerrainPathWeight(ObjectData objectData, float3 worldPos, float footprint)
{
    if (objectData.terrainPathKeys.z == 0u) return 0.0;
    const float4 band = objectData.terrainPathBand;
    const float bandShare = smoothstep(band.x, band.y, worldPos.y) *
        (1.0 - smoothstep(band.z, band.w, worldPos.y));
    [branch] if (bandShare <= 0.0) return 0.0;
    const float4 noise = objectData.terrainPathNoise;
    float2 gradientA, gradientB;
    const float value = noise.y * TerrainPathNoise(worldPos.xz, noise.x,
        objectData.terrainPathKeys.x, gradientA) +
        noise.w * TerrainPathNoise(worldPos.xz, noise.z,
        objectData.terrainPathKeys.y, gradientB);
    const float2 gradient = noise.y * gradientA + noise.w * gradientB;
    const float4 shape = objectData.terrainPathShape;
    const float metres = abs(value - shape.z) / max(length(gradient), shape.w);
    const float spread = max(footprint, 0.0);
    const float core = shape.x, edge = shape.y;
    const float track = 1.0 - smoothstep(core - 0.5 * spread, edge + 0.5 * spread, metres);
    return track * (core + edge) / (core + edge + spread) * bandShare;
}

// Smooth value noise in 0..1 on a unit lattice (world-space, so distant
// canopy relief stays put as tiles change LOD).
float CanopyValueNoise(float2 p)
{
    const float2 cell = floor(p);
    float2 t = p - cell;
    t = t * t * (3.0 - 2.0 * t);
    const float4 corners = frac(sin(float4(
        dot(cell, float2(127.1, 311.7)), dot(cell + float2(1, 0), float2(127.1, 311.7)),
        dot(cell + float2(0, 1), float2(127.1, 311.7)), dot(cell + float2(1, 1), float2(127.1, 311.7))))
        * 43758.5453);
    return lerp(lerp(corners.x, corners.y, t.x), lerp(corners.z, corners.w, t.x), t.y);
}

// Foliage colour variation (Material::foliageColorVariation, carried in
// textureUvSets1.w): world-space noise at crown (~9 m) and stand (~70 m)
// scales trades red against blue around the green (yellow-green to
// blue-green) and scales the value, so a forest is not one flat green.
// World-anchored, so near leaf cards, impostors and forest clusters that
// share the amount agree as LODs change.
float3 ApplyFoliageVariation(float3 color, float3 worldPosition, float amount)
{
    const float crown = CanopyValueNoise(worldPosition.xz * (1.0 / 9.0)) - 0.5;
    const float stand = CanopyValueNoise(worldPosition.xz * (1.0 / 70.0) + 31.7) - 0.5;
    const float hue = clamp(stand * 1.3 + crown * 0.7, -0.75, 0.75);
    const float value = 1.0 + amount * (crown * 0.32 + stand * 0.24);
    const float3 tint = float3(1.0 + 0.28 * hue, 1.0 - 0.04 * abs(hue), 1.0 - 0.28 * hue);
    return color * lerp(1.0, tint, amount) * value;
}

// Leaf lighting for masked foliage (Material::foliageTranslucency, carried in
// subsurfaceTransmission.y; .md/todo.md Foliage Phase 1). Meshes cooked by
// tools/cook_foliage_lighting.py carry crown data in uv1: x = canopy
// occlusion (1 on the crown's outer shell, ~0.2 deep inside), y = 4 + depth
// into the crown. Real UVs never reach 3.5, so that marks a cooked mesh.
// Cooked vertex normals are bent toward the crown surface and point outward,
// so they are kept on both card faces instead of flipping with the face.
struct FoliageLeaf
{
    bool on;
    bool cooked;
    float translucency;
    float occlusion;
    float crownScatter; // Material::foliageCrownScatter
};

FoliageLeaf GetFoliageLeaf(ObjectData data, uint flags, float2 uv1)
{
    FoliageLeaf leaf;
    leaf.translucency = data.subsurfaceTransmission.y;
    leaf.crownScatter = saturate(data.subsurfaceTransmission.z);
    leaf.on = leaf.translucency > 0.0 && (flags & 32u) != 0u;
    leaf.cooked = leaf.on && uv1.y > 3.5;
    leaf.occlusion = leaf.cooked ? saturate(uv1.x) : 1.0;
    return leaf;
}

// The normal a cooked leaf offsets its shadow lookup along: the real card
// (triangle) normal turned toward the light. Its shading normal is bent toward
// the crown, which no longer says how edge-on the card is to the sun, so the
// normal-offset bias built from it left grazing cards self-shadowing in
// diagonal bands.
float3 FoliageShadowNormal(FoliageLeaf leaf, float3 facet, float3 shadingNormal,
    float3 lightDirection)
{
    const float facetLength = length(facet);
    if (!leaf.cooked || facetLength <= 1.0e-12)
        return shadingNormal;
    const float3 card = facet / facetLength;
    return dot(card, lightDirection) < 0.0 ? -card : card;
}

// Extra diffuse that wraps a leaf's lighting past the terminator: light
// scatters inside a crown, so its shadow side is not a hard Lambert edge.
// Added on top of the regular n.l term, so it is zero where n.l = 1.
float FoliageWrapExtra(float nDotLRaw)
{
    return saturate((nDotLRaw + 0.5) / 1.5) - saturate(nDotLRaw);
}

// Sunlight through a thin leaf: a diffuse back term on the side away from
// the light plus forward scattering when looking toward it. The light is
// filtered by the leaf (brighter, yellower green). A leaf inside the crown's
// shadow transmits little, and the canopy shell scales it so outer leaves
// glow while the crown interior stays dark. radiance excludes 1 / PI.
float3 FoliageTransmission(float3 albedo, float3 radiance, float3 n, float3 v,
    float3 l, float shadow, FoliageLeaf leaf)
{
    const float back = saturate(-dot(n, l));
    const float forward = pow(saturate(dot(-v, l)), 4.0);
    const float shell = leaf.occlusion * leaf.occlusion;
    const float3 filtered = albedo * float3(1.05, 1.2, 0.5);
    // Light that already passed through the leaves above: a shaded leaf under
    // a sunlit canopy is lit from every side by green, scattered light, so
    // crown undersides are dark but not black (they were ~5x darker than the
    // reference frames' crowns once the sky fill was no longer over-bright).
    // It follows the leaf's shadow, so a whole tree in terrain shade keeps a
    // little of it too; small enough not to read as sunlight.
    // Scaled by the material's crown scatter, as is the 8 % of forward light
    // kept in full shadow: plants standing in another tree's shade (the
    // woodland pack's bushes and tall grass cards) were lit with 30 % of the
    // sun through their own leaf colour under the canopy, while the rocks,
    // ferns and ground beside them had sky light only (yellow-orange blades
    // glowing at ForestFloor, 2026-11-22). Those materials set it to 0.
    const float crown = leaf.crownScatter;
    const float scattered = 0.3 * (1.0 - shadow) * lerp(0.5, 1.0, leaf.occlusion) * crown;
    return filtered * radiance * (leaf.translucency / PI) *
        ((0.35 * back + 0.9 * forward) * lerp(0.08 * crown, 1.0, shadow) * shell + scattered);
}

// Forest canopy painted on distant terrain past the real trees' draw
// distance (TerrainManager::farCanopyDistance, in terrainMacroParams.z): the
// canopy colour (.030, .052, .020 at the macro map's .85 brightness).
static const float3 TerrainCanopyColor = float3(0.0255, 0.0442, 0.017);

// Bumpy canopy over forested distant terrain (macro map alpha = cover):
// tree-crown, clump and stand-scale height noise tilts the normal, and its
// hollows darken (crown occlusion). Each octave fades out once a pixel spans
// half its period, so far hills do not shimmer. Returns the occlusion; the
// normal is updated in place.
float ApplyDistantCanopy(inout float3 surfaceNormal, float3 worldPosition,
    float footprint, float cover, float strength)
{
    if (cover <= 0.0 || strength <= 0.0) return 1.0;
    const float periods[3] = { 14.0, 42.0, 130.0 };
    const float amplitudes[3] = { 0.30, 0.22, 0.14 };
    float2 slope = 0.0;
    float occlusion = 0.0;
    [unroll] for (uint octave = 0u; octave < 3u; ++octave)
    {
        const float period = periods[octave];
        const float fade = 1.0 - smoothstep(0.25 * period, 0.5 * period, footprint);
        if (fade <= 0.0) continue;
        const float2 p = worldPosition.xz / period + float(octave) * 17.3;
        const float step = 0.2;
        const float h = CanopyValueNoise(p);
        const float2 gradient = float2(CanopyValueNoise(p + float2(step, 0.0)),
            CanopyValueNoise(p + float2(0.0, step))) - h;
        // Relief height = amplitude * period, so its slope is amplitude-scaled.
        slope += gradient / step * amplitudes[octave] * fade;
        occlusion += (1.0 - h) * amplitudes[octave] * fade;
    }
    const float amount = cover * strength;
    surfaceNormal = normalize(surfaceNormal - float3(slope.x, 0.0, slope.y) * amount);
    return 1.0 - saturate(occlusion * 0.9 * amount);
}

// Terrain-scale sun visibility from the far height grid (see
// TerrainManager::GetSunShadowMap), bound in the emissive slot, which terrain
// does not use. Outside the map, and when disabled, the sun is unoccluded.
float TerrainLongRangeShadow(ObjectData objectData, float3 worldPosition)
{
    [branch] if (objectData.terrainSunShadow.z <= 0.0)
        return 1.0;
    const float2 uv = (worldPosition.xz - objectData.terrainSunShadow.xy) *
        objectData.terrainSunShadow.zw;
    if (any(uv < 0.0) || any(uv > 1.0))
        return 1.0;
    return emissiveMap.SampleLevel(materialSampler, uv, 0.0).r;
}

float EvaluateDirectionalShadow(ObjectData objectData, SceneLightData light,
    float3 worldPosition, float3 surfaceNormal, float3 lightDirection,
    int pcfRadius)
{
    // Skip filtering for disabled shadows. Backends still bind a valid null
    // descriptor for empty texture slots, including this shader's atlas.
    [branch] if (light.params.z < 0.0 || light.params.w <= 0.0)
        return 1.0;

    // A billboard impostor's camera-facing card lies inside its own caster
    // (that card turned toward the light). Test from a point beyond the crown
    // toward the light, so hills and neighbouring trees still shade it but
    // its own caster cannot.
    if (((uint)objectData.materialParams.w & 1024u) != 0u)
        worldPosition += lightDirection * 14.0;
    float normalFacing = saturate(dot(surfaceNormal, lightDirection));
    // Normal offset in world units, grown with each cascade's extent: far
    // cascades have proportionally larger texels, and a fixed offset that
    // suits the first one leaves grazing-lit ground (terrain at a low sun)
    // banded with self-shadowing acne farther out.
    const float normalOffset = objectData.shadowParams.y * (1.0 - normalFacing);
    const float firstExtent = max(objectData.shadowCascadeSplits.x, 0.001);
    uint cascadeCount = clamp((uint)objectData.shadowCascadeData.x, 1u, 4u);
    // Radial bands keep a fixed receiver in the same cascade when the player
    // turns in place. Camera-forward depth makes shadows change with look.
    float viewDistance = distance(worldPosition,
        objectData.viewPositionAlphaCutoff.xyz);
    if (viewDistance >= objectData.shadowCascadeData.w)
        return 1.0;
    uint cascade = 0u;
    if (cascadeCount > 1u && viewDistance > objectData.shadowCascadeSplits.x)
        cascade = 1u;
    if (cascadeCount > 2u && viewDistance > objectData.shadowCascadeSplits.y)
        cascade = 2u;
    if (cascadeCount > 3u && viewDistance > objectData.shadowCascadeSplits.z)
        cascade = 3u;

    const float cascadeExtent = cascade == 0u ? firstExtent :
        max(objectData.shadowCascadeSplits[cascade], firstExtent);
    float3 biasedPosition = worldPosition + surfaceNormal *
        normalOffset * (cascadeExtent / firstExtent);
    float filtered = SampleDirectionalShadowCascadeBlended(
        objectData, cascade, biasedPosition, pcfRadius);
    if (cascade + 1u < cascadeCount)
    {
        float previousSplit = cascade == 0u ? objectData.shadowCameraData.w :
            objectData.shadowCascadeSplits[cascade - 1u];
        float boundary = objectData.shadowCascadeSplits[cascade];
        float blendWidth = max((boundary - previousSplit) *
            objectData.shadowCascadeData.z, 0.0001);
        float cascadeBlend = saturate(
            (viewDistance - (boundary - blendWidth)) / blendWidth);
        if (cascadeBlend > 0.0)
        {
            const float nextExtent = max(cascade + 1u < 4u ?
                objectData.shadowCascadeSplits[cascade + 1u] : cascadeExtent, cascadeExtent);
            float nextFiltered = SampleDirectionalShadowCascadeBlended(
                objectData, cascade + 1u, worldPosition + surfaceNormal *
                    normalOffset * (nextExtent / firstExtent), pcfRadius);
            filtered = lerp(filtered, nextFiltered, cascadeBlend);
        }
    }
    float farFadeWidth = max(objectData.shadowCascadeData.w *
        objectData.shadowCascadeData.z, 0.0001);
    float farFade = saturate((viewDistance -
        (objectData.shadowCascadeData.w - farFadeWidth)) / farFadeWidth);
    filtered = lerp(filtered, 1.0, farFade);
    return lerp(1.0, filtered,
        saturate(light.params.w * objectData.shadowParams.z));
}

// Sun visibility of a point in the air for light shafts: one bilinear tap
// of the cascade covering `viewDistance`. Lit beyond the shadow range.
float FogShaftSunVisibility(ObjectData objectData, float3 position,
    float viewDistance, uint cascadeCount)
{
    if (viewDistance >= objectData.shadowCascadeSplits[cascadeCount - 1u])
        return 1.0;
    uint cascade = 0u;
    if (cascadeCount > 1u && viewDistance > objectData.shadowCascadeSplits.x)
        cascade = 1u;
    if (cascadeCount > 2u && viewDistance > objectData.shadowCascadeSplits.y)
        cascade = 2u;
    if (cascadeCount > 3u && viewDistance > objectData.shadowCascadeSplits.z)
        cascade = 3u;
    return SampleDirectionalShadowCascade(objectData, cascade, position, 0);
}

// Sky visibility under a canopy (.md/todo.md Foliage Phase 3). The sun's shadow
// map already records where tree crowns are: five taps on a 3 m disk, 2.5 m
// above the surface (so the surface itself and low plants do not count),
// average to how open the canopy is, and ambient (sky) light scales with it.
// Under a dense forest that is ~35 % of the open-sky ambient, which is most
// of why forest interiors read darker than meadows. No new textures: it
// reuses the cascades. The estimate sits a little downwind of the crowns
// (shadows fall away from the sun) and fades out toward the shadow range's
// end, where the cascades stop.
float CanopySkyVisibility(ObjectData objectData, float3 worldPos)
{
    [branch] if (objectData.shadowCascadeData.x < 0.5 || objectData.shadowParams.z <= 0.0)
        return 1.0;
    const uint cascadeCount = clamp((uint)objectData.shadowCascadeData.x, 1u, 4u);
    const float range = objectData.shadowCascadeSplits[cascadeCount - 1u];
    const float viewDistance = distance(worldPos, objectData.viewPositionAlphaCutoff.xyz);
    [branch] if (viewDistance >= range)
        return 1.0;
    const float3 center = worldPos + float3(0.0, 2.5, 0.0);
    float lit = FogShaftSunVisibility(objectData, center, viewDistance, cascadeCount);
    lit += FogShaftSunVisibility(objectData, center + float3(3.0, 0.0, 0.0), viewDistance, cascadeCount);
    lit += FogShaftSunVisibility(objectData, center - float3(3.0, 0.0, 0.0), viewDistance, cascadeCount);
    lit += FogShaftSunVisibility(objectData, center + float3(0.0, 0.0, 3.0), viewDistance, cascadeCount);
    lit += FogShaftSunVisibility(objectData, center - float3(0.0, 0.0, 3.0), viewDistance, cascadeCount);
    const float visibility = lerp(0.35, 1.0, smoothstep(0.0, 0.65, lit * 0.2));
    return lerp(visibility, 1.0, saturate((viewDistance - 0.7 * range) / (0.3 * range)));
}

// Opacity-weighted sun visibility along the fogged view ray, for
// ApplyAtmosphereFogShadowed. The ray is cut into segments spaced
// quadratically (finer near the camera, where shafts are resolved); each
// segment's analytic fog opacity is weighted by the shadow at one point
// inside it, offset by a per-pixel dither. The range uses the view-wide
// cascade splits, not the per-object shadowCascadeData.w, so shafts do not
// break at object edges. pixel.x < 0 samples segment centers.
float ObjectFogSunVisibility(ObjectData objectData, float3 worldPos,
    float2 pixel)
{
    const float4 fog0 = objectData.atmosphereFog0;
    const float4 fog1 = objectData.atmosphereFog1;
    const uint sampleCount = min(AtmosphereShaftSampleCount(fog0), 32u);
    [branch] if (sampleCount == 0u || objectData.shadowCascadeData.x < 0.5 ||
        all(objectData.atmosphereFog3.rgb <= 0.0))
        return 1.0;
    const float4 fog3 = objectData.atmosphereFog3;
    const float3 cameraPos = objectData.viewPositionAlphaCutoff.xyz;
    const float3 toSurface = worldPos - cameraPos;
    const float distanceToSurface = length(toSurface);
    const float startDistance = AtmosphereStartDistance(fog0, fog1);
    const float rayLength = max(distanceToSurface - startDistance, 0.0);
    const uint cascadeCount =
        clamp((uint)objectData.shadowCascadeData.x, 1u, 4u);
    const float marchLength = min(rayLength, objectData.shadowCascadeSplits[
        cascadeCount - 1u] - startDistance);
    [branch] if (marchLength <= 0.0)
        return 1.0;
    const float3 viewDirection = toSurface / max(distanceToSurface, 1e-4);
    const float totalOpacity = 1.0 - exp(-AtmosphereScalarOpticalDepth(cameraPos,
        viewDirection, rayLength, fog0, fog1, fog3));
    [branch] if (totalOpacity < 1e-5)
        return 1.0;
    // Interleaved gradient noise: a stable screen-space dither. There is no
    // temporal resolve, so it is not animated.
    const float jitter = pixel.x < 0.0 ? 0.5 : frac(52.9829189 *
        frac(dot(pixel, float2(0.06711056, 0.00583715))));
    const float3 rayStart = cameraPos + viewDirection * startDistance;
    const float inverseCount = 1.0 / (float)sampleCount;
    float previousTransmittance = 1.0;
    float litOpacity = 0.0;
    [loop] for (uint i = 0u; i < sampleCount; ++i)
    {
        const float segmentEnd = (float)(i + 1u) * inverseCount;
        const float transmittance = exp(-AtmosphereScalarOpticalDepth(cameraPos,
            viewDirection, marchLength * segmentEnd * segmentEnd, fog0, fog1, fog3));
        const float sampleFraction = ((float)i + jitter) * inverseCount;
        const float sampleDistance = marchLength * sampleFraction *
            sampleFraction;
        const float visibility = FogShaftSunVisibility(objectData,
            rayStart + viewDirection * sampleDistance,
            startDistance + sampleDistance, cascadeCount);
        litOpacity += (previousTransmittance - transmittance) * visibility;
        previousTransmittance = transmittance;
    }
    // Fog past the shadow range is sunlit.
    litOpacity += previousTransmittance - (1.0 - totalOpacity);
    return saturate(litOpacity / totalOpacity);
}

// Applies the scene height fog to a final surface color. Entry points call
// this once on their output; material shaders that wrap the surface function
// (Water, Foliage) call it after their own shading. `pixel` (SV_POSITION.xy)
// dithers the light-shaft samples; the overload without it samples segment
// centers, which bands slightly but stays correct.
float4 ApplyObjectAtmosphereFog(float4 color, float3 worldPos,
    uint objectIndex, float2 pixel)
{
    const ObjectData fogObject = objects[(draw.drawFlags & 0x13080000u) != 0u
        ? objectIndex : draw.objectIndex];
    [branch] if (fogObject.atmosphereFog0.w < 0.5)
        return color;
    return float4(ApplyAtmosphereFogShadowed(color.rgb, worldPos,
        fogObject.viewPositionAlphaCutoff.xyz, fogObject.atmosphereFog0,
        fogObject.atmosphereFog1, fogObject.atmosphereFog2,
        fogObject.atmosphereFog3,
        ObjectFogSunVisibility(fogObject, worldPos, pixel)), color.a);
}

float4 ApplyObjectAtmosphereFog(float4 color, float3 worldPos, uint objectIndex)
{
    return ApplyObjectAtmosphereFog(color, worldPos, objectIndex,
        float2(-1.0, -1.0));
}

float EvaluateSubsurfaceShadowChannel(ObjectData objectData,
    SceneLightData light, float3 position, float3 geometryNormal,
    float3 lightDirection, float3 tangent, float radius,
    float centerShadow)
{
    if (radius <= 0.00001)
        return centerShadow;
    float3 offset = tangent * radius;
    return (centerShadow * 2.0 +
        EvaluateDirectionalShadow(objectData, light, position + offset,
            geometryNormal, lightDirection, 0) +
        EvaluateDirectionalShadow(objectData, light, position - offset,
            geometryNormal, lightDirection, 0)) * 0.25;
}

// Integrate incident directional light at neighboring points on a local
// tangent plane. Each RGB channel samples its authored scene-unit radius.
float3 EvaluateSubsurfaceShadow(ObjectData objectData,
    SceneLightData light, float3 position, float3 geometryNormal,
    float3 lightDirection, float3 scatteringRadius, float centerShadow)
{
    if (abs(light.params.y - 1.0) >= 0.25 ||
        light.params.z < 0.0 || light.params.w <= 0.0)
        return centerShadow.xxx;
    float3 axis = abs(geometryNormal.z) < 0.9
        ? float3(0.0, 0.0, 1.0) : float3(0.0, 1.0, 0.0);
    float3 projectedLight = lightDirection - geometryNormal *
        dot(geometryNormal, lightDirection);
    float3 tangent = dot(projectedLight, projectedLight) > 0.0001
        ? normalize(projectedLight) : normalize(cross(geometryNormal, axis));
    return float3(
        EvaluateSubsurfaceShadowChannel(objectData, light, position,
            geometryNormal, lightDirection, tangent,
            scatteringRadius.r, centerShadow),
        EvaluateSubsurfaceShadowChannel(objectData, light, position,
            geometryNormal, lightDirection, tangent,
            scatteringRadius.g, centerShadow),
        EvaluateSubsurfaceShadowChannel(objectData, light, position,
            geometryNormal, lightDirection, tangent,
            scatteringRadius.b, centerShadow));
}

// UV selector: 0-4 choose XY from primary/additional UV1-4;
// 5-9 choose ZW from the same sets.
float2 SelectMaterialUv(float set, float2 uv, float2 uv1,
    float4 uvChannels0, float4 uvChannels1, float4 uvChannels2,
    float4 uvChannels3, float4 uvChannels4)
{
    uint index = min((uint)max(set, 0.0), 9u);
    if (index == 0u) return uv;
    if (index == 1u) return uv1;
    if (index == 2u) return uvChannels2.xy;
    if (index == 3u) return uvChannels3.xy;
    if (index == 4u) return uvChannels4.xy;
    if (index == 5u) return uvChannels0.zw;
    if (index == 6u) return uvChannels1.zw;
    if (index == 7u) return uvChannels2.zw;
    if (index == 8u) return uvChannels3.zw;
    return uvChannels4.zw;
}

// Smooth, world-anchored offset breaks up the short repeating period of the
// terrain color maps when hex-tiling is off.
float2 TerrainTextureHash(float2 gridPoint)
{
    float3 value = frac(float3(gridPoint.x, gridPoint.y, gridPoint.x) * 0.1031);
    value += dot(value, value.yzx + 33.33);
    return frac((value.xx + value.yz) * value.zy);
}

float2 TerrainTextureOffset(float2 worldXZ)
{
    float2 cell = worldXZ / 48.0;
    float2 corner = floor(cell);
    float2 blend = frac(cell);
    blend = blend * blend * (3.0 - 2.0 * blend);
    float2 a = TerrainTextureHash(corner);
    float2 b = TerrainTextureHash(corner + float2(1.0, 0.0));
    float2 c = TerrainTextureHash(corner + float2(0.0, 1.0));
    float2 d = TerrainTextureHash(corner + 1.0);
    return lerp(lerp(a, b, blend.x), lerp(c, d, blend.x), blend.y) - 0.5;
}

// ---------------------------------------------------------------------------
// Terrain layer arrays. Slice i of each array is painted layer i.
//   terrainAlbedoHeight: rgb albedo (sRGB view), a height (white = raised)
//   terrainNormal:       rg tangent normal XY (BC5)
//   terrainRoughAo:      r roughness, g occlusion (BC5)
// The shader works on a packed float4 (normal XY, roughness, occlusion).
// Layers are sampled with explicit gradients so hex-tiling and the
// biplanar cliff projection keep correct mip selection.
// ---------------------------------------------------------------------------
static const float TerrainHexTileRate = 0.6;  // hex cells per texture repeat
static const float TerrainHexRotation = 1.0;  // fraction of a full random turn
static const float TerrainHexContrast = 0.6;  // height-weighted seam contrast

struct TerrainLayerSample
{
    float3 albedo;
    float height;
    float3 normal; // world space, unnormalised
    float roughness;
    float occlusion;
};

uint2 TerrainPcg2d(uint2 v)
{
    v = v * 1664525u + 1013904223u;
    v.x += v.y * 1664525u;
    v.y += v.x * 1664525u;
    v ^= v >> 16u;
    v.x += v.y * 1664525u;
    v.y += v.x * 1664525u;
    v ^= v >> 16u;
    return v;
}

float3 UnpackTerrainNormal(float2 encoded)
{
    float3 n;
    n.xy = encoded * 2.0 - 1.0;
    n.z = sqrt(saturate(1.0 - dot(n.xy, n.xy)));
    return n;
}

// Mikkelsen, "Practical Real-Time Hex-Tiling" (JCGT 2022): three randomly
// offset and rotated copies on a triangle grid, blended with a contrast ramp.
// Returns the blend of the copies; tangent normals are rotated back into the
// unrotated UV frame.
// Triangle grid cell of a UV: barycentric weights and the three vertices.
void TerrainHexGrid(float2 uv, out float3 barycentric, out float2 vertices[3])
{
    const float2 st = uv * (TerrainHexTileRate * 3.46410162);
    const float2 skewed = float2(st.x - 0.57735027 * st.y, 1.15470054 * st.y);
    const float2 baseCell = floor(skewed);
    float3 temp = float3(frac(skewed), 0.0);
    temp.z = 1.0 - temp.x - temp.y;
    const float s = step(0.0, -temp.z);
    const float s2 = 2.0 * s - 1.0;
    barycentric = float3(-temp.z * s2, s - temp.y * s2, s - temp.x * s2);
    vertices[0] = baseCell + float2(s, s);
    vertices[1] = baseCell + float2(s, 1.0 - s);
    vertices[2] = baseCell + float2(1.0 - s, s);
}

// UV of the copy belonging to one grid vertex: random offset and rotation
// about the hex centre.
float2 TerrainHexCopyUv(float2 uv, float2 vertex, out float2x2 rotation)
{
    const uint2 hash = TerrainPcg2d(asuint(int2(vertex)));
    const float2 offset = float2(hash.x & 0xffffu, hash.x >> 16u) / 65535.0;
    const float angle = ((hash.y & 0xffffu) / 65535.0 * 2.0 - 1.0) *
        3.14159265 * TerrainHexRotation;
    float sine, cosine;
    sincos(angle, sine, cosine);
    rotation = float2x2(cosine, -sine, sine, cosine);
    const float2 centre = float2(vertex.x + 0.5 * vertex.y, 0.8660254 * vertex.y) /
        (TerrainHexTileRate * 3.46410162);
    return mul(uv - centre, rotation) + centre + offset;
}

// Blend weights of the three copies: sharp toward the hex edges, taller
// texels winning the overlap.
float3 TerrainHexWeights(float3 barycentric, float3 heights)
{
    float3 weights = pow(barycentric, 7.0) * lerp(1.0, heights, TerrainHexContrast);
    return weights / max(dot(weights, 1.0), 1e-6);
}

// Height (white = raised) exactly as the hex-tiled or plain albedo sample
// shows it, for the relief march.
float SampleTerrainHeight(float2 uv, float2 uvDx, float2 uvDy, float slice, bool hex)
{
    if (!hex)
        return terrainAlbedoHeight.SampleGrad(materialSampler, float3(uv, slice), uvDx, uvDy).a;
    float3 barycentric;
    float2 vertices[3];
    TerrainHexGrid(uv, barycentric, vertices);
    float3 heights;
    [unroll] for (uint i = 0u; i < 3u; ++i)
    {
        float2x2 rotation;
        const float2 copyUv = TerrainHexCopyUv(uv, vertices[i], rotation);
        heights[i] = terrainAlbedoHeight.SampleGrad(materialSampler, float3(copyUv, slice),
            mul(uvDx, rotation), mul(uvDy, rotation)).a;
    }
    return dot(TerrainHexWeights(barycentric, heights), heights);
}

// Relief march on one layer's height. shift is the UV offset, in the
// caller's (unscaled) UV units, at full relief depth along the view ray.
// Returns the UV where the ray meets the surface.
float2 TerrainParallax(float2 uv, float2 uvDx, float2 uvDy, float uvScale, float slice,
    bool hex, float2 shift, out float hitDepth)
{
    hitDepth = 0.0;
    const float2 layerDx = uvDx * uvScale, layerDy = uvDy * uvScale;
    const float uvPerPixel = sqrt(max(abs(layerDx.x * layerDy.y - layerDx.y * layerDy.x), 1e-14));
    const float pixels = length(shift * uvScale) / uvPerPixel;
    [branch] if (pixels < 0.5) return uv;
    // One step per two pixels of travel; hex heights cost three fetches.
    const float steps = clamp(ceil(pixels * 0.5), 4.0, hex ? 8.0 : 16.0);
    const float stepDepth = 1.0 / steps;
    float rayDepth = 0.0;
    float surfaceDepth = 1.0 - SampleTerrainHeight(uv * uvScale, layerDx, layerDy, slice, hex);
    float previousRay = 0.0, previousSurface = surfaceDepth;
    [loop] for (int i = 0; i < 16; ++i)
    {
        if (rayDepth >= surfaceDepth || i >= (int)steps) break;
        previousRay = rayDepth;
        previousSurface = surfaceDepth;
        rayDepth += stepDepth;
        surfaceDepth = 1.0 - SampleTerrainHeight((uv - shift * rayDepth) * uvScale,
            layerDx, layerDy, slice, hex);
    }
    // Secant step between the last point above and the first below.
    const float above = previousSurface - previousRay;
    const float below = rayDepth - surfaceDepth;
    const float t = saturate(above / max(above + below, 1e-5));
    hitDepth = lerp(previousRay, rayDepth, t);
    return uv - shift * hitDepth;
}

// Relief state the lighting needs after the march (top projection).
struct TerrainRelief
{
    float weight;        // 0 = no relief shadow
    float2 uv;           // hit, in terrain UV units
    float depth;         // hit depth, 0..1 of the relief
    float depthToUv;     // terrain UV travelled per unit of depth at 45 degrees
    float layerScale;
    float slice;
    float2 uvDx, uvDy;
    bool hex;
    float3 tangent, bitangent, normal;
};

// Soft self-shadow of the relief toward a light (Tatarchuk): march from the
// hit toward the light and keep the strongest occluder, weighted down with
// distance along the ray. Same falloff and gain as the mesh version.
float TerrainReliefShadow(TerrainRelief relief, float3 lightDirection)
{
    const float3 lightTangent = float3(dot(lightDirection, relief.tangent),
        dot(lightDirection, relief.bitangent), dot(lightDirection, relief.normal));
    [branch] if (relief.weight <= 0.0 || relief.depth <= 0.001 || lightTangent.z <= 0.0)
        return 1.0;
    const float2 ray = lightTangent.xy / max(lightTangent.z, 0.08) * relief.depthToUv;
    const float steps = round(lerp(3.0, relief.hex ? 6.0 : 10.0, relief.depth));
    const float stepDepth = relief.depth / steps;
    const float2 layerDx = relief.uvDx * relief.layerScale;
    const float2 layerDy = relief.uvDy * relief.layerScale;
    float occlusion = 0.0;
    [loop] for (float i = 1.0; i <= steps; i += 1.0)
    {
        const float rayDepth = relief.depth - stepDepth * i;
        const float surfaceDepth = 1.0 - SampleTerrainHeight(
            (relief.uv + ray * (stepDepth * i)) * relief.layerScale,
            layerDx, layerDy, relief.slice, relief.hex);
        occlusion = max(occlusion, (rayDepth - surfaceDepth) * (1.0 - (i - 1.0) / steps));
    }
    return lerp(1.0, 1.0 - saturate(occlusion * 8.0), relief.weight);
}

void SampleTerrainHex(float2 uv, float2 uvDx, float2 uvDy, float slice,
    bool sampleNormal, out float4 albedoHeight, out float4 normalRough)
{
    float3 barycentric;
    float2 vertices[3];
    TerrainHexGrid(uv, barycentric, vertices);

    float4 a[3];
    float4 b[3];
    float2x2 rotation[3];
    [unroll] for (uint i = 0u; i < 3u; ++i)
    {
        const float2 copyUv = TerrainHexCopyUv(uv, vertices[i], rotation[i]);
        const float2 copyDx = mul(uvDx, rotation[i]);
        const float2 copyDy = mul(uvDy, rotation[i]);
        a[i] = terrainAlbedoHeight.SampleGrad(materialSampler,
            float3(copyUv, slice), copyDx, copyDy);
        b[i] = sampleNormal ? float4(
            terrainNormal.SampleGrad(materialSampler, float3(copyUv, slice), copyDx, copyDy).rg,
            terrainRoughAo.SampleGrad(materialSampler, float3(copyUv, slice), copyDx, copyDy).rg)
            : float4(0.5, 0.5, 1.0, 1.0);
    }
    // Sharp falloff toward each hex edge; taller texels win the overlap.
    const float3 weights = TerrainHexWeights(barycentric, float3(a[0].a, a[1].a, a[2].a));
    albedoHeight = a[0] * weights.x + a[1] * weights.y + a[2] * weights.z;
    float2 normalXy = 0.0;
    float2 rest = 0.0;
    [unroll] for (uint j = 0u; j < 3u; ++j)
    {
        // A copy sampled at uv' = R^T (uv - c) has gradients R * g in uv.
        normalXy += mul(rotation[j], b[j].rg * 2.0 - 1.0) * weights[j];
        rest += b[j].ba * weights[j];
    }
    normalRough = float4(normalXy * 0.5 + 0.5, rest);
}

void SampleTerrainPlain(float2 uv, float2 uvDx, float2 uvDy, float slice,
    bool sampleNormal, out float4 albedoHeight, out float4 normalRough)
{
    albedoHeight = terrainAlbedoHeight.SampleGrad(materialSampler,
        float3(uv, slice), uvDx, uvDy);
    normalRough = sampleNormal ? float4(
        terrainNormal.SampleGrad(materialSampler, float3(uv, slice), uvDx, uvDy).rg,
        terrainRoughAo.SampleGrad(materialSampler, float3(uv, slice), uvDx, uvDy).rg)
        : float4(0.5, 0.5, 1.0, 1.0);
}

// One planar projection of one layer: hex-tiled near (hexAmount 1), plain
// far (0), both in the narrow band between.
void SampleTerrainProjection(float2 uv, float2 uvDx, float2 uvDy, float slice,
    bool sampleNormal, float hexAmount, out float4 albedoHeight, out float4 normalRough)
{
    albedoHeight = 0.0;
    normalRough = 0.0;
    [branch] if (hexAmount > 0.0)
        SampleTerrainHex(uv, uvDx, uvDy, slice, sampleNormal, albedoHeight, normalRough);
    [branch] if (hexAmount < 1.0)
    {
        float4 plainAlbedo, plainNormal;
        SampleTerrainPlain(uv, uvDx, uvDy, slice, sampleNormal, plainAlbedo, plainNormal);
        albedoHeight = lerp(plainAlbedo, albedoHeight, hexAmount);
        normalRough = lerp(plainNormal, normalRough, hexAmount);
    }
}

struct TerrainProjection
{
    // Top projection (terrain UV) and its tangent frame.
    float2 topUv, topDx, topDy;
    float3 tangent, bitangent, normal;
    // Side projections for cliffs, in world metres scaled to texture repeats.
    float2 sideXUv, sideXDx, sideXDy;
    float2 sideZUv, sideZDx, sideZDy;
    float3 weights; // top, x-facing, z-facing
    float hexAmount;
    // Past the hex-tiled range: share of a second, larger and rotated sample
    // of the top projection's albedo (SampleTerrainLayer).
    float dualScale;
};

TerrainLayerSample SampleTerrainLayer(TerrainProjection projection, uint layer,
    float uvScale, bool sampleNormal, float normalStrength)
{
    TerrainLayerSample result = (TerrainLayerSample)0;
    const float slice = (float)layer;
    const float3 n = projection.normal;
    float4 albedoHeight, normalRough;
    [branch] if (projection.weights.x > 0.0)
    {
        SampleTerrainProjection(projection.topUv * uvScale, projection.topDx * uvScale,
            projection.topDy * uvScale, slice, sampleNormal, projection.hexAmount,
            albedoHeight, normalRough);
        // Beyond the hex-tiled range one plain sample repeats every few
        // metres, and from 40-500 m away the repeats read as a grid. Blend
        // in the same texture at 0.37 of the scale, turned 37 degrees: two
        // tilings that never line up hide each other's period (albedo only;
        // the normal and roughness keep the first sample).
        [branch] if (projection.dualScale > 0.0)
        {
            const float2x2 turn = float2x2(0.7986, -0.6018, 0.6018, 0.7986);
            const float scale = 0.37 * uvScale;
            const float3 second = terrainAlbedoHeight.SampleGrad(materialSampler,
                float3(mul(turn, projection.topUv) * scale + float2(0.31, 0.67), slice),
                mul(turn, projection.topDx) * scale, mul(turn, projection.topDy) * scale).rgb;
            albedoHeight.rgb = lerp(albedoHeight.rgb, second, 0.5 * projection.dualScale);
        }
        float3 tn = UnpackTerrainNormal(normalRough.rg);
        tn.xy *= normalStrength;
        const float w = projection.weights.x;
        result.albedo += albedoHeight.rgb * w;
        result.height += albedoHeight.a * w;
        result.roughness += normalRough.b * w;
        result.occlusion += normalRough.a * w;
        result.normal += (projection.tangent * tn.x + projection.bitangent * tn.y +
            n * tn.z) * w;
    }
    // Cliffs: world-aligned projections, normals reoriented with a UDN blend
    // (Golus, "Normal Mapping for a Triplanar Shader"). No hex here; rock
    // faces vary enough and the side bands are narrow.
    [branch] if (projection.weights.y > 0.0)
    {
        SampleTerrainPlain(projection.sideXUv * uvScale, projection.sideXDx * uvScale,
            projection.sideXDy * uvScale, slice, sampleNormal, albedoHeight, normalRough);
        float3 tn = UnpackTerrainNormal(normalRough.rg);
        tn.xy *= normalStrength;
        const float side = n.x >= 0.0 ? 1.0 : -1.0;
        tn.x *= side;
        const float w = projection.weights.y;
        result.albedo += albedoHeight.rgb * w;
        result.height += albedoHeight.a * w;
        result.roughness += normalRough.b * w;
        result.occlusion += normalRough.a * w;
        result.normal += float3(n.x, tn.y + n.y, tn.x + n.z) * w;
    }
    [branch] if (projection.weights.z > 0.0)
    {
        SampleTerrainPlain(projection.sideZUv * uvScale, projection.sideZDx * uvScale,
            projection.sideZDy * uvScale, slice, sampleNormal, albedoHeight, normalRough);
        float3 tn = UnpackTerrainNormal(normalRough.rg);
        tn.xy *= normalStrength;
        const float side = n.z >= 0.0 ? 1.0 : -1.0;
        tn.x *= -side;
        const float w = projection.weights.z;
        result.albedo += albedoHeight.rgb * w;
        result.height += albedoHeight.a * w;
        result.roughness += normalRough.b * w;
        result.occlusion += normalRough.a * w;
        result.normal += float3(tn.x + n.x, tn.y + n.y, n.z) * w;
    }
    return result;
}

// World-anchored coverage is stable as the camera moves. Both LODs and their
// shadows use the same intervals, so transitioning surfaces do not alpha-blend
// or require transparent sorting.
// Distance from the camera to the origin of the instanced placement being
// shaded, or negative when unknown. Pixel-shader entry points set it from the
// vertex shader's TEXCOORD12 before any shading function runs.
static float g_instanceLodDistance = -1.0;
// Billboard impostor card dither weight (VSMain, billboardFade): w > 0
// keeps screen hash < w, w < 0 keeps hash >= 1 - |w|. Set by the same entry
// points from TEXCOORD7.w; only read for impostorBillboard materials.
static float g_billboardFade = 1.0;
// Octahedral impostor views (g_billboardFade >= 2): the start of this view's
// dither interval, from TEXCOORD7.z; its end is g_billboardFade - 3.
static float g_billboardStart = 0.0;
// Shrub and bush range-edge opacity (VSMain placementDissolve), read from
// TEXCOORD7.w by the pixel entry points; 1 for everything else.
static float g_placementOpacity = 1.0;
void ReadPlacementOpacity(float4 uvChannels4)
{
    g_placementOpacity = (draw.drawFlags & 0x20000000u) != 0u && (draw.placementReach & 1024u) != 0u
        ? saturate(uvChannels4.w) : 1.0;
}

// Visibility (tile fade-in) carried in geometryFade.w: 2 + visibility for tile
// levels, 6 + 0.4 * (1 - visibility) for per-placement ground cover
// (SceneRendering.cpp perPlacementFade), 1 for the other modes (bands 4-5,
// screen coverage 7).
float GroundCoverVisibility(float w)
{
    if (w >= 5.5 && w < 6.45) return saturate(1.0 - (w - 6.0) / 0.4);
    return w >= 4.0 ? 1.0 : saturate(w - 2.0);
}

// Half-width of the dissolve at an instanced LOD switch distance. Must match
// the per-placement band test in InstanceCull.hlsl.
float InstanceLodFadeWidth(float switchDistance)
{
    return clamp(switchDistance * 0.04, 0.5, 6.0);
}

// A fixed threshold per screen pixel for complementary dissolves (LOD bands,
// impostor view hand-over): an 8x8 Bayer matrix. Interleaved gradient noise
// was used before; without TAA its diagonal line structure showed through as
// dark bottom-left-to-top-right stripes across dissolving crowns (the forest
// behind showing between them). The ordered matrix has no line structure: at
// 50 % it is a fine checkerboard that reads as an even tone.
// frame (ObjectData skinParams.z): 0 without TAA (a fixed pattern), 1..8 while
// TAA jitters the view; every pixel's threshold then rotates by the golden
// ratio each frame (the pattern itself stays put: shifting it moved it along
// a diagonal, which survived in the history as faint hatching), so the
// history resolves the dissolve to a smooth blend.
float ScreenDitherThreshold(float2 screenPosition, float frame)
{
    const uint2 p = uint2(floor(screenPosition)) & 7u;
    const uint x = p.x ^ p.y;
    const uint bayer = ((x & 1u) << 5) | ((p.y & 1u) << 4) | ((x & 2u) << 2) |
        ((p.y & 2u) << 1) | ((x & 4u) >> 1) | ((p.y & 4u) >> 2);
    return frac((float(bayer) + 0.5) / 64.0 + frame * 0.61803399);
}

// Cutout foliage (leaves, ferns, bushes, grass: material flag 32) dissolves
// as it nears the camera, so walking through or past a plant never fills the
// view with a leaf card that pops between frames. Per pixel, from
// NearFoliageFadeEnd (fully drawn) to NearFoliageFadeStart (gone), with the
// rotating screen dither the LOD dissolves use (TAA resolves it to a blend).
// Camera records only: shadow records (shadowCasterFade.z) keep the plant.
// The foliage depth pre-pass runs PSShadowMain on camera records, so it clips
// the same pixels as the lighting pass.
static const float NearFoliageFadeStart = 0.35;
static const float NearFoliageFadeEnd = 1.25;
void ApplyNearFoliageFade(ObjectData data, float3 worldPosition, float2 screenPosition)
{
    [branch] if ((((uint)data.materialParams.w) & 32u) != 0u && data.shadowCasterFade.z < 0.5)
    {
        const float distance = length(worldPosition - data.viewPositionAlphaCutoff.xyz);
        [branch] if (distance < NearFoliageFadeEnd)
            clip(smoothstep(NearFoliageFadeStart, NearFoliageFadeEnd, distance) -
                ScreenDitherThreshold(screenPosition, data.skinParams.z) - 1.0e-4);
    }
}

void ApplyGeometryFade(ObjectData data, float3 worldPosition, float2 screenPosition)
{
    ApplyNearFoliageFade(data, worldPosition, screenPosition);
    // Shrubs and bushes dissolving in at their range edge (whole plant, one
    // opacity, rotating screen dither: TAA resolves it to a blend).
    [branch] if (g_placementOpacity < 1.0)
        clip(g_placementOpacity - ScreenDitherThreshold(screenPosition, data.skinParams.z) - 1.0e-4);
    // Billboard impostors hand one view to the next with complementary
    // screen dither (weights w and 1 - w), so coplanar cards never overlap.
    [branch] if (((uint)data.materialParams.w & 1024u) != 0u &&
        abs(g_billboardFade) < 1.0)
    {
        const float threshold = ScreenDitherThreshold(screenPosition, data.skinParams.z);
        clip(g_billboardFade >= 0.0
            ? g_billboardFade - threshold
            : threshold - (1.0 + g_billboardFade));
    }
    // Octahedral impostor views keep the pixels whose threshold falls in
    // their interval [start, end) of the three views' weights.
    [branch] if (((uint)data.materialParams.w & 1024u) != 0u && g_billboardFade >= 2.0)
    {
        const float threshold = ScreenDitherThreshold(screenPosition, data.skinParams.z);
        clip(min(threshold - g_billboardStart + 1.0e-4,
            (g_billboardFade - 3.0) - threshold - 1.0e-4));
    }
    // Screen LOD candidates were selected with per-view hysteresis. Preserve
    // appearance/range fade, but do not apply the authored distance LOD again.
    // Shadow records retain their independent distance/fixed-proxy policy.
    if ((draw.drawFlags & 0x00800000u) != 0u && data.shadowCasterFade.z < 0.5)
        data.geometryFade = float4(0, data.geometryFade.w >= 2.0 ? GroundCoverVisibility(data.geometryFade.w) :
            data.geometryFade.y, 0, 1);
    // Instanced LOD bands with a known placement distance: the whole
    // placement (a tree) takes one band and dissolves as a unit, with
    // screen-space thresholds so the outgoing and incoming LODs cover exactly
    // complementary pixels. Per-pixel distances would slice large objects
    // with a sphere around the camera that sweeps through them as it moves.
    const bool perPlacement = data.geometryFade.w >= 4.0 && g_instanceLodDistance >= 0.0;
    if (data.geometryFade.w >= 2.0)
    {
        const float distance=perPlacement ? g_instanceLodDistance :
            length((data.geometryFade.w>=4.0 ? worldPosition:mul(data.world,float4(0,0,0,1)).xyz)-data.viewPositionAlphaCutoff.xyz);
        const float widthFraction=data.geometryFade.w>=4.0 ? .25 : data.geometryFade.x;
        const float width=max(0.0,min(.5,(data.geometryFade.z-data.geometryFade.y)*widthFraction));
        const float widthFirst=perPlacement ? InstanceLodFadeWidth(data.geometryFade.y) : width;
        const float widthSecond=perPlacement ? InstanceLodFadeWidth(data.geometryFade.z) : width;
        const float first=widthFirst>0.0 ? smoothstep(data.geometryFade.y-widthFirst,data.geometryFade.y+widthFirst,distance) : step(data.geometryFade.y,distance);
        const float second=widthSecond>0.0 ? smoothstep(data.geometryFade.z-widthSecond,data.geometryFade.z+widthSecond,distance) : step(data.geometryFade.z,distance);
        // Bands (4, 5) carry their level in x; ground cover tiles (2-3 per
        // tile, 6-7 per placement) draw each level with the level in the flags.
        const uint lod=(data.geometryFade.w>=4.0 && data.geometryFade.w<5.5) ? (uint)data.geometryFade.x:(draw.drawFlags >> 21u) & 3u;
        const float visibility=GroundCoverVisibility(data.geometryFade.w);
        if(lod==0) data.geometryFade=float4(0,(1-first)*visibility,0,1);
        else if(lod==1) data.geometryFade=distance<data.geometryFade.z-widthSecond ? float4((1-first)*visibility,visibility,0,1):float4(0,(1-second)*visibility,0,1);
        else data.geometryFade=float4((1-second)*visibility,visibility,0,1);
    }
    if (data.shadowCasterFade.z > 0.5 && data.shadowCasterFade.x > 0.0)
    {
        const float range=data.shadowCasterFade.x;
        const float width=max(0.001,data.shadowCasterFade.y);
        const float distance=length(worldPosition-data.viewPositionAlphaCutoff.xyz);
        float opacity=1.0-smoothstep(max(0.0,range-width),range,distance);
        // A far caster (w: minimum distance) fades in over the window where
        // the near fixed-band caster fades out, so the two hand over.
        if(data.shadowCasterFade.w>0.0)
            opacity*=smoothstep(max(0.0,data.shadowCasterFade.w-width),
                data.shadowCasterFade.w,distance);
        // Scale complementary LOD intervals together, preserving their union.
        if(data.geometryFade.w>0.5) data.geometryFade.xy*=opacity;
        // A fully covered fixed proxy needs neither a dither hash nor clips.
        else if(opacity<1.0) data.geometryFade=float4(0,opacity,0,1);
    }
    if (data.geometryFade.w > 0.5)
    {
        if (data.geometryFade.y <= data.geometryFade.x) clip(-1.0);
        // The hash is in [0,1), so a fully covered interval cannot reject a
        // pixel. Most forest fragments are outside the narrow LOD crossfade.
        [branch] if (data.geometryFade.x <= 0.0 && data.geometryFade.y >= 1.0)
            return;
        float3 cell = floor(worldPosition * 96.0);
        float threshold = perPlacement ? ScreenDitherThreshold(screenPosition, data.skinParams.z) :
            frac(sin(dot(cell, float3(12.9898, 78.233, 37.719))) * 43758.5453);
        clip(threshold - data.geometryFade.x);
        clip(data.geometryFade.y - threshold);
    }
}

float4 PSShadowMain(
    float4 pos : SV_POSITION,
    float3 worldPos : TEXCOORD2,
    float3 normal : NORMAL,
    float2 uv : TEXCOORD,
    float2 uv1 : TEXCOORD1,
    float4 vertexColor : COLOR,
    float4 tangent : TANGENT,
    float4 uvChannels0 : TEXCOORD3,
    float4 uvChannels1 : TEXCOORD4,
    float4 uvChannels2 : TEXCOORD5,
    float4 uvChannels3 : TEXCOORD6,
    float4 uvChannels4 : TEXCOORD7,
    nointerpolation uint objectIndex : TEXCOORD11,
    float instanceLodDistance : TEXCOORD12) : SV_TARGET
{
    g_instanceLodDistance = instanceLodDistance;
    g_billboardFade = uvChannels4.w;
    g_billboardStart = uvChannels4.z;
    ReadPlacementOpacity(uvChannels4);
    g_triangleObject = (draw.drawFlags & 0x13080000u) != 0u ? objectIndex : draw.objectIndex;
    ObjectData objectData = objects[g_triangleObject];
    // Derivatives before any clip() below.
    const float3 facet = SurfaceFacet(worldPos);
    const float2 baseUv = SelectMaterialUv(objectData.textureUvSets0.x,
        uv, uv1, uvChannels0, uvChannels1, uvChannels2,
        uvChannels3, uvChannels4);
    const float2 baseUvDx = ddx(baseUv), baseUvDy = ddy(baseUv);
    ApplyGeometryFade(objectData, worldPos, pos.xy);
    ApplyAlphaLod(objectData, worldPos);
    if (dot(objectData.traversalClipPlane.xyz,
        objectData.traversalClipPlane.xyz) > 0.0 &&
        dot(float4(worldPos, 1.0), objectData.traversalClipPlane) < 0.0)
        clip(-1.0);
    if (dot(objectData.portalClipPlane.xyz,
        objectData.portalClipPlane.xyz) > 0.0 &&
        dot(float4(worldPos, 1.0), objectData.portalClipPlane) < 0.0)
        clip(-1.0);
    float textureBias = TextureLodBias(objectData, worldPos);
    uint flags = (uint)objectData.materialParams.w;
    if (flags & 32u)
    {
        const float edgeFade = CardEdgeFade(objectData, worldPos, facet);
        float alpha = objectData.baseColor.a *
            ((flags & 4096u) ? 1.0 : vertexColor.a) * edgeFade;
        if (flags & 1u)
            alpha *= baseColorMap.SampleBias(materialSampler, baseUv, AlphaLodBias(objectData, worldPos)).a *
                objectData.pmxTextureTint.a;
        clip(alpha - objectData.viewPositionAlphaCutoff.w);
        ApplyMaskedAlphaTransmission(objectData, baseUv, baseUvDx, baseUvDy, edgeFade);
    }
    return 0.0;
}

// Opaque foliage keeps the shared instancing and LOD vertex path, but skips
// clustered lighting, specular BRDFs, reflections and the wide shadow kernel.
// Editor Performance render mode. The CPU writes morphParams.z = -(1 + heat)
// (positive values are the weight-paint bone), heat in [0, 1] ranking the
// draw's CPU + GPU cost. Shading by the view angle keeps shapes readable.
bool IsPerformanceHeatDraw(ObjectData data)
{
    return data.morphParams.z < -0.5;
}

uint TriangleHash(uint value)
{
    value ^= value >> 16; value *= 0x7feb352du;
    value ^= value >> 15; value *= 0x846ca68bu;
    return value ^ (value >> 16);
}

float3 HueToRgb(float hue)
{
    return saturate(abs(frac(hue + float3(0.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0) - 1.0);
}

// Neighbouring triangles have nearby primitive IDs (strips, grid rows). The
// R2 low-discrepancy sequence (plastic-number steps) spreads consecutive IDs
// as far apart as possible in hue x lightness for every offset, unlike a
// random hash, which repeats close colours by chance. Each object record
// starts the sequence at its own hashed offset.
float3 TriangleColor(ObjectData data, float3 worldPos, float3 normal, bool frontFace)
{
    const float sequence = float(g_primitiveId % 1048576u) +
        float(TriangleHash(g_triangleObject) % 65536u);
    const float2 r2 = frac(sequence * float2(0.7548776662, 0.5698402910) + 0.5);
    const float3 hue = HueToRgb(r2.x);
    const float level = 0.45 + 0.55 * r2.y;
    float3 color = lerp(level.xxx, hue * level, 0.85);
    // Light view-angle shading keeps silhouettes readable without blending
    // neighbouring colours together.
    float3 n = normalize(frontFace ? normal : -normal);
    float3 v = normalize(data.viewPositionAlphaCutoff.xyz - worldPos);
    return color * (0.7 + 0.3 * saturate(abs(dot(n, v))));
}

float4 PerformanceHeatColor(ObjectData data, float3 worldPos, float3 normal,
    bool frontFace, float alpha)
{
    if (IsTriangleDraw(data))
        return float4(TriangleColor(data, worldPos, normal, frontFace), alpha);
    // Cold-to-hot ramp: blue, cyan, green, yellow, red at quarter steps.
    // Keep in sync with the legend swatch in EditToolsViewTemplate.cpp.
    float heat = saturate(-data.morphParams.z - 1.0);
    if (IsDetailHeatDraw(data))
    {
        // Detail view: red = full geometry and full texture detail, blue =
        // least; the lower of the two decides.
        // Geometry: instanced LOD bands rate by band (LOD0 1, LOD1 .55,
        // impostor .2), which is what the detail budget moves. Other meshes
        // (terrain cells and far tiles, props) carry their mean triangle
        // edge in metres (morphParams.z = -1000 - edge) and rate by its size
        // on screen here: up to 3 pixels is full detail, 96 pixels none.
        float geometry = 1.0;
        if (data.geometryFade.w >= 4.0)
        {
            const float band = data.geometryFade.x;
            geometry = band < .5 ? 1.0 : band < 1.5 ? .55 : .2;
        }
        else if (-data.morphParams.z >= 1000.0 && -data.morphParams.z < 8000.0)
        {
            const float edge = -data.morphParams.z - 1000.0;
            const float pixelWorld = sqrt(length(ddx(worldPos)) * length(ddy(worldPos)));
            const float edgePixels = edge / max(pixelWorld, 1.0e-5);
            geometry = saturate(1.0 - log2(max(edgePixels, 1.0) / 3.0) / 5.0);
        }
        heat = min(geometry, g_detailTexture);
        // Occlusion debug (ENGINE_OCCLUSION_DEBUG): anything the occlusion
        // buffer would hide is kept and painted magenta. Magenta on screen
        // means something visible would have been culled.
        if (-data.morphParams.z >= 8000.0 || g_instanceLodDistance < -1.5)
            return float4(1.0, 0.0, 1.0, alpha);
    }
    float3 ramp[5] = {
        float3(0.10, 0.20, 0.95),
        float3(0.05, 0.80, 0.95),
        float3(0.10, 0.85, 0.20),
        float3(1.00, 0.85, 0.05),
        float3(1.00, 0.08, 0.03) };
    float position = heat * 4.0;
    uint segment = min((uint)position, 3u);
    float3 color = lerp(ramp[segment], ramp[segment + 1u], position - segment);
    float3 n = normalize(frontFace ? normal : -normal);
    float3 v = normalize(data.viewPositionAlphaCutoff.xyz - worldPos);
    float facing = saturate(abs(dot(n, v)));
    return float4(color * (0.35 + 0.65 * facing), alpha);
}

float4 PSFoliageMain(
    float4 pos : SV_POSITION,
    float3 worldPos : TEXCOORD2,
    float3 normal : NORMAL,
    float2 uv : TEXCOORD,
    float2 uv1 : TEXCOORD1,
    float4 vertexColor : COLOR,
    nointerpolation uint objectIndex : TEXCOORD11,
    bool frontFace : SV_IsFrontFace) : SV_TARGET
{
    // Called as a function by Foliage.hlsl, which records g_primitiveId.
    g_triangleObject = (draw.drawFlags & 0x13080000u) != 0u
        ? objectIndex : draw.objectIndex;
    ObjectData objectData = objects[g_triangleObject];
    // Derivatives before any clip() below.
    const float3 facet = SurfaceFacet(worldPos);
    const float2 uvDx = ddx(uv), uvDy = ddy(uv);
    ApplyGeometryFade(objectData, worldPos, pos.xy);
    ApplyAlphaLod(objectData, worldPos);
    if (dot(objectData.traversalClipPlane.xyz,
        objectData.traversalClipPlane.xyz) > 0.0 &&
        dot(float4(worldPos, 1.0), objectData.traversalClipPlane) < 0.0)
        clip(-1.0);
    if (dot(objectData.portalClipPlane.xyz,
        objectData.portalClipPlane.xyz) > 0.0 &&
        dot(float4(worldPos, 1.0), objectData.portalClipPlane) < 0.0)
        clip(-1.0);

    float textureBias = TextureLodBias(objectData, worldPos);
    uint flags = (uint)objectData.materialParams.w;
    float4 base = objectData.baseColor *
        ((flags & 4096u) ? float4(1.0, 1.0, 1.0, 1.0) : vertexColor);
    const float edgeFade = CardEdgeFade(objectData, worldPos, facet);
    base.a *= edgeFade;
    if (flags & 1u)
        base *= SampleObjectBaseColor(objectData, uv, textureBias,
            base.a * objectData.pmxTextureTint.a,
            AlphaLodBias(objectData, worldPos)) *
            objectData.pmxTextureTint;
    if (flags & 32u)
    {
        clip(base.a - objectData.viewPositionAlphaCutoff.w);
        ApplyMaskedAlphaTransmission(objectData, uv, uvDx, uvDy, edgeFade);
    }
    if (IsPerformanceHeatDraw(objectData))
        return PerformanceHeatColor(objectData, worldPos, normal, frontFace, base.a);
    if (objectData.ambientUnlit.w > 0.5)
        return float4(base.rgb + objectData.emissiveOcclusion.rgb, base.a);

    // Texture LOD's optional shading tier is also used by foliage as an
    // opt-in far-distance lighting simplification. Keep alpha sampling and
    // coverage above independent from this color-only transition. At the far
    // end, approximate average canopy shadowing without per-pixel shadow taps.
    const float shadingBlend = objectData.textureLodParams.w > 1.5
        ? TextureLodBlend(objectData, worldPos) : 0.0;
    const FoliageLeaf leaf = GetFoliageLeaf(objectData, flags, uv1);
    float3 n = normalize(frontFace || leaf.cooked ? normal : -normal);
    const float3 v = normalize(objectData.viewPositionAlphaCutoff.xyz - worldPos);
    const float leafDirect = lerp(0.45, 1.0, leaf.occlusion);
    float3 lit = objectData.ambientUnlit.rgb * ScreenAmbientOcclusion(worldPos) *
        leaf.occlusion * CanopySkyVisibility(objectData, worldPos);
    float3 transmitted = 0.0;
    [loop] for (uint i = 0u; i < draw.lightCount; ++i)
    {
        SceneLightData light = sceneLights[i];
        if (abs(light.params.y - 1.0) >= 0.25 ||
            (objectData.lightingChannels.x & asuint(light.attenuation.z)) == 0u)
            continue;
        float3 direction = normalize(light.positionRange.xyz);
        const float nDotLRaw = dot(n, direction);
        float diffuse = saturate(nDotLRaw);
        if (leaf.on)
            diffuse = (diffuse + FoliageWrapExtra(nDotLRaw)) * leafDirect;
        if (diffuse <= 0.0 && !leaf.on) continue;
        float shadow = 1.0;
        [branch] if (objectData.shadowParams.z > 0.0)
        {
            // Retain representative shadowing (0.75) in the simplified tier
            // to avoid a large brightness jump as detailed shadowing fades.
            // Branching lets fully distant foliage skip the shadow lookup.
            shadow = shadingBlend >= 1.0 ? 0.75 :
                EvaluateDirectionalShadow(objectData, light,
                    worldPos, FoliageShadowNormal(leaf, facet, n, direction), direction, 0);
            shadow = lerp(shadow, 0.75, shadingBlend);
        }
        shadow *= DirectionalCookie(light, worldPos);
        lit += light.colorIntensity.rgb *
            (light.colorIntensity.w * diffuse * shadow * 0.31830988618);
        [branch] if (leaf.on)
            transmitted += FoliageTransmission(base.rgb,
                light.colorIntensity.rgb * light.colorIntensity.w, n, v, direction,
                shadow, leaf);
    }
    return float4(base.rgb * lit + transmitted + objectData.emissiveOcclusion.rgb,
        base.a);
}

#ifdef GRASS_SURFACE_SHADING
// Grass and meadow plants shade as a pelt rather than as flat cards:
//  * normals lean toward the sky, so a field lights as one soft surface, and
//    green ribbons are rounded across u like a real blade;
//  * wrapped diffuse and root occlusion (uv.y runs root to tip);
//  * transmission when the sun is behind the blades;
//  * a Kajiya-Kay highlight along each blade, the anisotropic sheen of fur.
//    The blade direction comes from the wind-bent tangent frame, so passing
//    gusts sweep bright waves across the field.
// Petals and other non-green colours keep their own normals and skip the
// sheen, but still transmit light.
float4 PSGrassMain(
    float4 pos, float3 worldPos, float3 normal, float2 uv, float4 vertexColor,
    float4 tangent, uint objectIndex, bool frontFace)
{
    g_triangleObject = (draw.drawFlags & 0x13080000u) != 0u
        ? objectIndex : draw.objectIndex;
    ObjectData objectData = objects[g_triangleObject];
    // Derivatives before any clip() below.
    const float3 facet = SurfaceFacet(worldPos);
    const float2 uvDx = ddx(uv), uvDy = ddy(uv);
    ApplyGeometryFade(objectData, worldPos, pos.xy);
    ApplyAlphaLod(objectData, worldPos);
    if (dot(objectData.traversalClipPlane.xyz,
        objectData.traversalClipPlane.xyz) > 0.0 &&
        dot(float4(worldPos, 1.0), objectData.traversalClipPlane) < 0.0)
        clip(-1.0);
    if (dot(objectData.portalClipPlane.xyz,
        objectData.portalClipPlane.xyz) > 0.0 &&
        dot(float4(worldPos, 1.0), objectData.portalClipPlane) < 0.0)
        clip(-1.0);

    float textureBias = TextureLodBias(objectData, worldPos);
    uint flags = (uint)objectData.materialParams.w;
    float4 base = objectData.baseColor *
        ((flags & 4096u) ? float4(1.0, 1.0, 1.0, 1.0) : vertexColor);
    const float edgeFade = CardEdgeFade(objectData, worldPos, facet);
    base.a *= edgeFade;
    const bool textured = (flags & 1u) != 0u;
    if (textured)
        base *= SampleObjectBaseColor(objectData, uv, textureBias,
            base.a * objectData.pmxTextureTint.a,
            AlphaLodBias(objectData, worldPos)) *
            objectData.pmxTextureTint;
    if (flags & 32u)
    {
        clip(base.a - objectData.viewPositionAlphaCutoff.w);
        ApplyMaskedAlphaTransmission(objectData, uv, uvDx, uvDy, edgeFade);
    }
    if (IsPerformanceHeatDraw(objectData))
        return PerformanceHeatColor(objectData, worldPos, normal, frontFace, base.a);
    if (objectData.ambientUnlit.w > 0.5)
        return float4(base.rgb + objectData.emissiveOcclusion.rgb, base.a);

    const float shadingBlend = objectData.textureLodParams.w > 1.5
        ? TextureLodBlend(objectData, worldPos) : 0.0;
    // Textured grass cards carry atlas UVs, not a root-to-tip blade layout.
    const float height = textured ? 1.0 : saturate(uv.y);
    const float leafy = GrassLeafiness(base.rgb);
    // Fine mottling so blades and leaves are not flat vertex colour: two
    // octaves of world-space value noise (7 cm and 1.8 cm), +-14 % value with
    // a slight yellow/blue shift, fading out by 25 m where it is sub-pixel.
    {
        const float mottleFade = 1.0 - smoothstep(12.0, 25.0,
            distance(worldPos, objectData.viewPositionAlphaCutoff.xyz));
        [branch] if (mottleFade > 0.0 && !textured)
        {
            const float2 q = worldPos.xz + worldPos.y * float2(0.61, 0.37);
            const float mottle = (CanopyValueNoise(q * 14.0) - 0.5) * 0.6 +
                (CanopyValueNoise(q * 55.0 + 9.3) - 0.5) * 0.4;
            base.rgb *= 1.0 + mottle * 0.28 * mottleFade *
                float3(1.0 + 0.5 * mottle, 1.0, 1.0 - 0.5 * mottle);
        }
    }
    const float3 up = float3(0.0, 1.0, 0.0);
    float3 geometric = normalize(frontFace ? normal : -normal);
    float3 across = tangent.xyz - geometric * dot(tangent.xyz, geometric);
    const bool hasTangent = dot(across, across) > 1.0e-6;
    across = hasTangent ? normalize(across) : float3(0.0, 0.0, 0.0);
    // Sign does not matter: Kajiya-Kay uses the fibre axis only.
    float3 blade = hasTangent ? normalize(cross(geometric, across)) : up;
    float3 n = geometric;
    if (!textured)
        n = normalize(n + across * ((uv.x - 0.5) * 1.3 * leafy));
    // Textured plants (scanned bush cards, tools/scan_bush_cards.py) carry
    // volume normals of their own, so they keep most of their shaded side.
    n = normalize(lerp(n, up, (textured ? 0.15 : 0.55) * leafy));
    float3 v = normalize(objectData.viewPositionAlphaCutoff.xyz - worldPos);

    // Depth in the mass (uv.y runs root to tip): low on a blade it is shaded
    // by the blades around it, from the sky and the sun alike. The reference
    // meadows go from near-black between the stems to bright lit tips
    // (luminance 10th-90th percentile 0.005-0.15 against our 0.05-0.14 with
    // the earlier 0.6 / 0.82 floors). The upper quarter of a blade is in the
    // open: floors of 0.3 / 0.35 over the whole length darkened the tips too
    // (90th percentile 0.14 -> 0.10), and the meadow as a whole fell to a
    // quarter of the sky's brightness against about a half in the references.
    const float depthLight = smoothstep(0.0, 0.75, height);
    float ambientOcclusion = lerp(1.0, lerp(0.4, 1.0, depthLight), leafy);
    float directOcclusion = lerp(1.0, lerp(0.45, 1.0, depthLight), leafy);
    float3 lit = objectData.ambientUnlit.rgb * ambientOcclusion *
        ScreenAmbientOcclusion(worldPos) * CanopySkyVisibility(objectData, worldPos) *
        (0.9 + 0.1 * saturate(n.y));
    float3 transmitted = 0.0;
    float3 sheen = 0.0;
    [loop] for (uint i = 0u; i < draw.lightCount; ++i)
    {
        SceneLightData light = sceneLights[i];
        if (abs(light.params.y - 1.0) >= 0.25 ||
            (objectData.lightingChannels.x & asuint(light.attenuation.z)) == 0u)
            continue;
        float3 direction = normalize(light.positionRange.xyz);
        float wrapped = saturate((dot(n, direction) + 0.35) / 1.35);
        // Thin tissue lights the side away from the sun, most strongly when
        // looking toward it; tips are thinner than roots.
        float backlight = (saturate(-dot(geometric, direction)) * 0.35 +
            pow(saturate(dot(-v, direction)), 4.0) * 0.65) *
            lerp(0.35, 1.0, height);
        if (wrapped <= 0.0 && backlight <= 0.0) continue;
        float shadow = 1.0;
        [branch] if (objectData.shadowParams.z > 0.0)
        {
            // Same representative far-tier shadowing as PSFoliageMain.
            shadow = shadingBlend >= 1.0 ? 0.75 :
                EvaluateDirectionalShadow(objectData, light,
                    worldPos, geometric, direction, 0);
            shadow = lerp(shadow, 0.75, shadingBlend);
        }
        shadow *= DirectionalCookie(light, worldPos);
        float3 radiance = light.colorIntensity.rgb *
            (light.colorIntensity.w * shadow * 0.31830988618);
        lit += radiance * (wrapped * directOcclusion);
        // Light through the blades is shaded by the mass in front of it too.
        transmitted += radiance * (backlight * directOcclusion);
        float3 halfVector = normalize(direction + v);
        float alongBlade = dot(blade, halfVector);
        float sinAngle = sqrt(saturate(1.0 - alongBlade * alongBlade));
        sheen += radiance * (pow(sinAngle, 24.0) * wrapped);
    }
    float3 color = base.rgb * lit +
        base.rgb * transmitted * float3(0.55, 0.7, 0.33) +
        sheen * (0.07 * leafy * height * (1.0 - 0.6 * shadingBlend)) *
            float3(0.85, 1.0, 0.7);
    return float4(color + objectData.emissiveOcclusion.rgb, base.a);
}
#endif

// The object surface shader. PSMain below is its entry point; material
// shaders that wrap this file call it through their renamed PSMain.
float4 ObjectSurfacePS(
    float4 pos : SV_POSITION,
    float3 worldPos : TEXCOORD2,
    float3 normal : NORMAL,
    float2 uv : TEXCOORD,
    float2 uv1 : TEXCOORD1,
    float4 vertexColor : COLOR,
    float4 tangent : TANGENT,
    float4 uvChannels0 : TEXCOORD3,
    float4 uvChannels1 : TEXCOORD4,
    float4 uvChannels2 : TEXCOORD5,
    float4 uvChannels3 : TEXCOORD6,
    float4 uvChannels4 : TEXCOORD7,
    nointerpolation uint objectIndex : TEXCOORD11,
    bool frontFace : SV_IsFrontFace) : SV_TARGET
{
    g_triangleObject = (draw.drawFlags & 0x13080000u) != 0u ? objectIndex : draw.objectIndex;
    ObjectData objectData = objects[g_triangleObject];
    // Derivatives before any clip() below (the coverage hash ignores parallax
    // offsets: it needs a stable surface anchor, not the exact texel).
    const float3 facet = SurfaceFacet(worldPos);
    const float2 maskUv = SelectMaterialUv(objectData.textureUvSets0.x,
        uv, uv1, uvChannels0, uvChannels1, uvChannels2,
        uvChannels3, uvChannels4);
    const float2 maskUvDx = ddx(maskUv), maskUvDy = ddy(maskUv);
    ApplyGeometryFade(objectData, worldPos, pos.xy);
    ApplyAlphaLod(objectData, worldPos);
    float textureBias = TextureLodBias(objectData, worldPos);
    uint flags = (uint)objectData.materialParams.w;
    float materialDetail = objectData.textureLodParams.w > 1.5
        ? 1.0 - TextureLodBlend(objectData, worldPos) : 1.0;
    objectData.environmentParams.w *= materialDetail;

    // Split traversal instances retain their own chart clipping even while
    // they are drawn through another portal view. This must be separate from
    // the destination aperture plane below; overwriting either reveals both
    // halves of a crossing object or hides the connected scene.
    if (dot(objectData.traversalClipPlane.xyz, objectData.traversalClipPlane.xyz) > 0.0 &&
        dot(float4(worldPos, 1.0), objectData.traversalClipPlane) < 0.0)
        clip(-1.0);
    // Portal views keep only the connected side of the target aperture. This
    // avoids geometry from behind the destination plane appearing through a
    // stencil-correct but spatially invalid portal view.
    if (dot(objectData.portalClipPlane.xyz, objectData.portalClipPlane.xyz) > 0.0 &&
        dot(float4(worldPos, 1.0), objectData.portalClipPlane) < 0.0)
        clip(-1.0);
    float4 base = objectData.baseColor * ((flags & 4096u) ? float4(1, 1, 1, 1) : vertexColor);
    const float edgeFade = CardEdgeFade(objectData, worldPos, facet);
    base.a *= edgeFade;
    // Alpha-tested surfaces fall through to keep their cutout silhouettes.
    if (IsPerformanceHeatDraw(objectData) && !(flags & 32u))
    {
        // The Detail view also rates the base color texture (the mask UV set
        // is the base color set unless a material splits them).
        [branch] if (IsDetailHeatDraw(objectData) && (flags & 1u))
            SampleObjectBaseColor(objectData, maskUv, 0.0, 1.0, 0.0);
        return PerformanceHeatColor(objectData, worldPos, normal, frontFace, base.a);
    }
    if (flags & 32768u)
    {
        // The ruler repeats every 100 metres in world space. Cell borders use
        // the manager's real dimensions, so LODs and distant tiles line up.
        float3 gridColor = baseColorMap.Sample(materialSampler,
            worldPos.xz / 100.0).rgb;
        float2 span = max(objectData.textureUvSets1.xy, 0.001);
        float2 cellMetres = abs(frac(worldPos.xz / span + 0.5) - 0.5) * span;
        float2 borderWidth = max(fwidth(worldPos.xz), 0.2);
        float border = 1.0 - saturate(min(cellMetres.x / borderWidth.x,
            cellMetres.y / borderWidth.y));
        gridColor = lerp(gridColor, float3(1.0, 0.66, 0.15), border);
        return float4(gridColor, 1.0);
    }
    // Distant tiles are lit in the vertex shader unless a long-range shadow
    // map is bound: then the sun term is applied here, shadowed per pixel.
    // Without the map, one far-cascade shadow tap removes the sun's share
    // (vertex alpha) where hills and forests shade them. Their unlit material
    // has no other texture or per-pixel lighting work.
    if (flags & 16384u)
    {
        const bool macroMap = objectData.terrainMacroParams.z > 0.5;
        const bool sunMap = objectData.terrainSunShadow.z > 0.0;
        float3 surfaceNormal = normalize(normal);
        // Macro color map: its albedo replaces the far palette's vertex
        // color, and its 16 m normals the tiles' 128 m triangle normals, so
        // distant ranges shade with ridges and gullies instead of facets.
        [branch] if (macroMap)
        {
            const float3 positionDx = ddx(worldPos), positionDy = ddy(worldPos);
            const float4 macro = SampleTerrainMacroColor(objectData, worldPos,
                positionDx, positionDy);
            // The macro color is the ground; its alpha the forest cover,
            // splatted from the real tree placements (LandscapeDriver). The
            // painted canopy (colour and relief) shows only where the real
            // trees stop drawing (TerrainManager::farCanopyDistance, z),
            // fading in over its last 10 %. Nearer, the real trees are the
            // only cover (fair visibility, 2026-11-24).
            const float canopyEnd = objectData.terrainMacroParams.z;
            const float canopyCover = macro.a * smoothstep(canopyEnd * 0.9, canopyEnd,
                distance(worldPos, objectData.viewPositionAlphaCutoff.xyz));
            base.rgb = objectData.baseColor.rgb * lerp(macro.rgb, TerrainCanopyColor, canopyCover);
            // w: 0 = no normal map, else 1 + canopy relief strength.
            [branch] if (objectData.terrainMacroParams.w > 0.5)
            {
                surfaceNormal = SampleTerrainMacroNormal(objectData, worldPos,
                    positionDx, positionDy);
                base.rgb *= ApplyDistantCanopy(surfaceNormal, worldPos,
                    max(length(positionDx), length(positionDy)), canopyCover,
                    objectData.terrainMacroParams.w - 1.0);
            }
        }
        // Contour paths (TerrainPathWeight) at the far tiles' own resolution.
        // Far tiles carry no paint: steep ground (where the near paint turns
        // to rock) keeps the track off. Only where the tile is lit per pixel
        // (base is albedo there; otherwise VSMain baked the light in).
        [branch] if (objectData.terrainPathKeys.z != 0u && (sunMap || macroMap))
        {
            const float3 pathDx = ddx(worldPos), pathDy = ddy(worldPos);
            const float footprint = max(length(pathDx.xz), length(pathDy.xz));
            const float share = TerrainPathWeight(objectData, worldPos, footprint) *
                smoothstep(0.62, 0.78, surfaceNormal.y) * objectData.terrainPathFar.w;
            base.rgb = lerp(base.rgb, objectData.terrainPathFar.rgb * objectData.baseColor.rgb, share);
        }
        // Lit per pixel whenever a map is bound (VSMain leaves the light out).
        [branch] if (sunMap || macroMap)
        {
            const float sunFacing = saturate(dot(surfaceNormal,
                objectData.bakedLightDirection.xyz));
            float shadow = sunMap ? TerrainLongRangeShadow(objectData, worldPos) : 1.0;
            [branch] if (!sunMap && sunFacing > 0.0 && objectData.bakedDirectional.w > 0.5 &&
                objectData.shadowParams.z > 0.5)
            {
                SceneLightData sun = sceneLights[
                    (uint)objectData.bakedDirectional.w - 1u];
                shadow = EvaluateDirectionalShadow(objectData, sun, worldPos,
                    surfaceNormal, objectData.bakedLightDirection.xyz, 0);
            }
            shadow *= SunCookie(objectData, worldPos);
            base.rgb *= objectData.ambientUnlit.rgb * ScreenAmbientOcclusion(worldPos) +
                objectData.bakedDirectional.rgb * (sunFacing * shadow * 0.31830988618);
            return float4(base.rgb + objectData.emissiveOcclusion.rgb, base.a);
        }
        float shadow = 1.0;
        [branch] if (objectData.bakedDirectional.w > 0.5 &&
            objectData.shadowParams.z > 0.5 && vertexColor.a > 0.0)
        {
            SceneLightData sun = sceneLights[
                (uint)objectData.bakedDirectional.w - 1u];
            shadow = EvaluateDirectionalShadow(objectData, sun, worldPos,
                normalize(normal), objectData.bakedLightDirection.xyz, 0);
        }
        shadow *= SunCookie(objectData, worldPos);
        const float sunShare = saturate(vertexColor.a);
        return float4(base.rgb * (1.0 - sunShare * (1.0 - shadow)) +
            objectData.emissiveOcclusion.rgb, 1.0);
    }
    // Height-field triangles and normals consistently face the top surface.
    float3 terrainNormalForLighting = normalize(normal);
    float terrainRoughness = 1.0;
    float terrainOcclusion = 1.0;
    // Relief self-shadow toward the first directional light (normally the
    // sun), resolved here so the lighting loops carry one float.
    float terrainReliefShadow = 1.0;
    uint terrainReliefLight = 0xffffffffu;
    // How much of the pixel is triplanar rock (below).
    float terrainRockShare = 0.0;
    // Authored terrain stores six painted weights in the spare UV channels.
    // Vertex color carries the contribution of layers without a texture.
    if (flags & 2048u)
    {
        // Texture repeats per metre. Runtime cells map world X/Z; without a
        // known span the terrain's own 0..1 UV is tiled.
        const bool worldMapped = objectData.textureUvSets1.x > 0.0;
        const float repeatsPerMetre = worldMapped
            ? objectData.textureUvSets1.z / objectData.textureUvSets1.x : 0.0;
        const float uvTiling = max(1.0, objectData.textureUvSets1.z);
        // Gradients for every projection, taken before any per-pixel branch.
        // The texture-LOD bias becomes a gradient scale for SampleGrad.
        const float gradientScale = exp2(textureBias);
        const float3 worldDx = ddx(worldPos) * gradientScale;
        const float3 worldDy = ddy(worldPos) * gradientScale;
        float2 terrainUv = worldMapped ? worldPos.xz * repeatsPerMetre : uv * uvTiling;
        const float2 terrainDx = worldMapped ? worldDx.xz * repeatsPerMetre
            : ddx(uv) * uvTiling * gradientScale;
        const float2 terrainDy = worldMapped ? worldDy.xz * repeatsPerMetre
            : ddy(uv) * uvTiling * gradientScale;
        const float2 terrainVariation = TerrainTextureOffset(worldPos.xz);
        const float nearDistance = distance(worldPos,
            objectData.viewPositionAlphaCutoff.xyz);
        const bool hexTiling = (flags & 65536u) != 0u;
        if (!hexTiling) terrainUv += terrainVariation * 0.9;
        // Measured lowest-mip colors supply inexpensive macro appearance.
        // From 300 m (512 before 2026-11-24: 40-500 m of plain tiling
        // showed its repeats), fully at 1.5 km.
        const float macroBlend = (flags & 131072u)
            ? smoothstep(300.0, 1500.0, nearDistance) : 0.0;
        float weights[6] = {uvChannels2.x, uvChannels2.y,
            uvChannels2.z, uvChannels2.w, uvChannels3.x, uvChannels3.y};
        // Paint map: weights at finer than vertex spacing. Its samples sit on
        // the vertex grid, so texel centres map to uv 0 and 1 exactly.
        [branch] if (flags & 524288u)
        {
            uint paintWidth, paintHeight, paintSlices;
            terrainPaint.GetDimensions(paintWidth, paintHeight, paintSlices);
            const float2 paintSize = float2(paintWidth, paintHeight);
            const float2 paintUv = (saturate(uv) * (paintSize - 1.0) + 0.5) / paintSize;
            const float4 paintA = terrainPaint.Sample(materialSampler, float3(paintUv, 0.0));
            const float2 paintB = terrainPaint.Sample(materialSampler, float3(paintUv, 1.0)).rg;
            weights[0] = paintA.x; weights[1] = paintA.y;
            weights[2] = paintA.z; weights[3] = paintA.w;
            weights[4] = paintB.x; weights[5] = paintB.y;
        }
        // Contour paths (TerrainPathWeight): the path layer takes over where
        // the track runs, on the layers it may cross (not rock, snow or
        // shore), as the CPU paint did at the vertices.
        float pathShare = 0.0;
        [branch] if (objectData.terrainPathKeys.z != 0u)
        {
            float crossable = 0.0, total = 0.0;
            [unroll] for (uint layer = 0u; layer < 6u; ++layer)
            {
                total += weights[layer];
                if (objectData.terrainPathKeys.w & (1u << layer)) crossable += weights[layer];
            }
            const float footprint = max(length(worldDx.xz), length(worldDy.xz)) / gradientScale;
            pathShare = total > 0.00001
                ? TerrainPathWeight(objectData, worldPos, footprint) * (crossable / total) *
                    objectData.terrainPathFar.w
                : 0.0;
            [branch] if (pathShare > 0.0)
            {
                const uint pathLayer = objectData.terrainPathKeys.z - 1u;
                [unroll] for (uint layer = 0u; layer < 6u; ++layer)
                    weights[layer] = weights[layer] * (1.0 - pathShare) +
                        (layer == pathLayer ? pathShare * total : 0.0);
            }
        }
        float originalWeightSum = 0.0;
        [unroll] for (uint layer = 0u; layer < 6u; ++layer)
            originalWeightSum += weights[layer];
        const uint4 masks = objectData.terrainMapMasks;

        [branch] if ((flags & 262144u) == 0u || macroBlend >= 0.999)
        {
            // Layer arrays not resident yet, or beyond detail range: the
            // measured mean color of each layer.
            float roughnessSum = 0.0;
            [unroll] for (uint layer = 0u; layer < 6u; ++layer)
            {
                if (masks.w & (1u << layer))
                    base.rgb += objectData.terrainMacroColors[layer].rgb * weights[layer];
                roughnessSum += weights[layer] * objectData.terrainLayerParams[layer].x;
            }
            if (originalWeightSum > 0.00001)
                terrainRoughness = clamp(roughnessSum / originalWeightSum, 0.045, 1.0);
        }
        else
        {
            // Shade the three strongest layers; weaker ones are dropped.
            uint selected[3] = {0u, 0u, 0u};
            float selectedWeight[3] = {0.0, 0.0, 0.0};
            [unroll] for (uint layer = 0u; layer < 6u; ++layer)
            {
                const float w = weights[layer];
                if (w > selectedWeight[0])
                {
                    selected[2] = selected[1]; selectedWeight[2] = selectedWeight[1];
                    selected[1] = selected[0]; selectedWeight[1] = selectedWeight[0];
                    selected[0] = layer; selectedWeight[0] = w;
                }
                else if (w > selectedWeight[1])
                {
                    selected[2] = selected[1]; selectedWeight[2] = selectedWeight[1];
                    selected[1] = layer; selectedWeight[1] = w;
                }
                else if (w > selectedWeight[2])
                {
                    selected[2] = layer; selectedWeight[2] = w;
                }
            }
            float selectedSum = 0.0;
            [unroll] for (uint k = 0u; k < 3u; ++k)
            {
                if (selectedWeight[k] < 0.01) selectedWeight[k] = 0.0;
                selectedSum += selectedWeight[k];
            }

            TerrainProjection projection;
            projection.normal = terrainNormalForLighting;
            const float3 tangentAxis = tangent.xyz - projection.normal *
                dot(projection.normal, tangent.xyz);
            projection.tangent = dot(tangentAxis, tangentAxis) > 1e-8
                ? normalize(tangentAxis) : float3(1.0, 0.0, 0.0);
            projection.bitangent = cross(projection.normal, projection.tangent) * tangent.w;
            projection.topUv = terrainUv;
            projection.topDx = terrainDx;
            projection.topDy = terrainDy;
            // Slope-gated biplanar mapping: faces steeper than ~37 degrees
            // fade to world-aligned side projections, which do not stretch.
            // The projection follows the triangle where it leans far from
            // its smoothed normal (sharp facets, overhangs)
            // and faces pointing down take the top projection; a side
            // projection chosen by the smoothed normal smeared their texture
            // into vertical streaks. Height-field triangles stay close to
            // their normals, so they keep the smooth selection.
            float3 n = projection.normal;
            [flatten] if (dot(facet, facet) > 1e-12)
            {
                float3 faceNormal = normalize(facet);
                if (dot(faceNormal, n) < 0.0) faceNormal = -faceNormal;
                const float facetShare = saturate((0.94 - dot(faceNormal, n)) * 5.0);
                n = normalize(lerp(n, faceNormal, facetShare));
            }
            const float sideAmount = worldMapped
                ? saturate((0.80 - abs(n.y)) / 0.15) : 0.0;
            const float xShare = saturate((abs(n.x) - abs(n.z)) * 4.0 + 0.5);
            projection.weights = float3(1.0 - sideAmount,
                sideAmount * xShare, sideAmount * (1.0 - xShare));
            const float signX = n.x >= 0.0 ? 1.0 : -1.0;
            const float signZ = n.z >= 0.0 ? 1.0 : -1.0;
            projection.sideXUv = float2(worldPos.z * signX, worldPos.y) * repeatsPerMetre;
            projection.sideXDx = float2(worldDx.z * signX, worldDx.y) * repeatsPerMetre;
            projection.sideXDy = float2(worldDy.z * signX, worldDy.y) * repeatsPerMetre;
            projection.sideZUv = float2(-worldPos.x * signZ, worldPos.y) * repeatsPerMetre;
            projection.sideZDx = float2(-worldDx.x * signZ, worldDx.y) * repeatsPerMetre;
            projection.sideZDy = float2(-worldDy.x * signZ, worldDy.y) * repeatsPerMetre;
            // Hex-tiling near; one sample per map beyond 40 m.
            projection.hexAmount = hexTiling
                ? 1.0 - smoothstep(24.0, 40.0, nearDistance) : 0.0;
            // Dual-scale albedo where the hex tiling ends, until the macro
            // color takes over.
            projection.dualScale = smoothstep(28.0, 60.0, nearDistance) * (1.0 - macroBlend);

            // Relief near the camera: march the dominant layer's height and
            // shift the top projection of every layer to the hit. Height
            // scale is in the layer's texture repeats (terrainMacroColors.w).
            [branch] if ((flags & 1048576u) && materialDetail > 0.0 &&
                projection.weights.x > 0.5 && selectedWeight[0] > 0.0 &&
                nearDistance < 15.0)
            {
                const uint layer = selected[0];
                const float layerScale = objectData.terrainLayerParams[layer].w > 0.0
                    ? objectData.terrainLayerParams[layer].w : 1.0;
                const float relief = objectData.terrainMacroColors[layer].w *
                    (1.0 - smoothstep(10.0, 15.0, nearDistance)) * materialDetail;
                [branch] if (relief > 0.0)
                {
                    const float3 viewDir = normalize(
                        objectData.viewPositionAlphaCutoff.xyz - worldPos);
                    const float3 viewTangent = float3(dot(viewDir, projection.tangent),
                        dot(viewDir, projection.bitangent), dot(viewDir, projection.normal));
                    const float2 shift = viewTangent.xy / max(viewTangent.z, 0.15) *
                        (relief / layerScale);
                    float hitDepth;
                    TerrainRelief terrainRelief = (TerrainRelief)0;
                    projection.topUv = TerrainParallax(projection.topUv, projection.topDx,
                        projection.topDy, layerScale, (float)layer,
                        projection.hexAmount >= 0.5, shift, hitDepth);
                    // Relief shadows are their own, costlier quality tier.
                    terrainRelief.weight = (flags & 2097152u)
                        ? (1.0 - smoothstep(8.0, 12.0, nearDistance)) *
                            materialDetail * projection.weights.x : 0.0;
                    terrainRelief.uv = projection.topUv;
                    terrainRelief.depth = hitDepth;
                    terrainRelief.depthToUv = relief / layerScale;
                    terrainRelief.layerScale = layerScale;
                    terrainRelief.slice = (float)layer;
                    terrainRelief.uvDx = projection.topDx;
                    terrainRelief.uvDy = projection.topDy;
                    terrainRelief.hex = projection.hexAmount >= 0.5;
                    terrainRelief.tangent = projection.tangent;
                    terrainRelief.bitangent = projection.bitangent;
                    terrainRelief.normal = projection.normal;
                    [branch] if (terrainRelief.weight > 0.0)
                    {
                        [loop] for (uint lightIndex = 0u; lightIndex < draw.lightCount; ++lightIndex)
                        {
                            const SceneLightData light = sceneLights[lightIndex];
                            if (abs(light.params.y - 1.0) >= 0.25) continue;
                            terrainReliefLight = lightIndex;
                            terrainReliefShadow = TerrainReliefShadow(terrainRelief,
                                normalize(light.positionRange.xyz));
                            break;
                        }
                    }
                }
            }

            TerrainLayerSample samples[3];
            [unroll] for (uint k = 0u; k < 3u; ++k)
            {
                samples[k] = (TerrainLayerSample)0;
                samples[k].normal = projection.normal;
                samples[k].roughness = 1.0;
                samples[k].occlusion = 1.0;
                [branch] if (selectedWeight[k] > 0.0)
                {
                    const uint layer = selected[k];
                    const float4 params = objectData.terrainLayerParams[layer];
                    const bool sampleNormal = materialDetail > 0.0 &&
                        ((masks.x | masks.y) & (1u << layer)) != 0u;
                    samples[k] = SampleTerrainLayer(projection, layer,
                        params.w > 0.0 ? params.w : 1.0, sampleNormal,
                        params.y * materialDetail);
                }
            }

            // Height-lerp: w' = max(w + s*h - (max(w + s*h) - depth), 0), so
            // the taller layer shows through in the transition (stones over
            // sand) instead of a linear cross-fade.
            float finalWeight[3];
            float heightTop = -1e9;
            float heightValue[3];
            [unroll] for (uint k = 0u; k < 3u; ++k)
            {
                const uint layer = selected[k];
                const float strength = (masks.z & (1u << layer))
                    ? objectData.terrainLayerParams[layer].z : 0.0;
                heightValue[k] = selectedWeight[k] / max(selectedSum, 1e-6) +
                    strength * samples[k].height;
                if (selectedWeight[k] > 0.0) heightTop = max(heightTop, heightValue[k]);
            }
            float heightSum = 0.0;
            [unroll] for (uint k = 0u; k < 3u; ++k)
            {
                finalWeight[k] = selectedWeight[k] > 0.0
                    ? max(heightValue[k] - (heightTop - 0.2), 0.0) : 0.0;
                heightSum += finalWeight[k];
            }
            const float heightBlend = materialDetail *
                (((masks.z & ((1u << selected[0]) | (1u << selected[1]) |
                    (1u << selected[2]))) != 0u) ? 1.0 : 0.0);
            [unroll] for (uint k = 0u; k < 3u; ++k)
                finalWeight[k] = lerp(selectedWeight[k] / max(selectedSum, 1e-6),
                    finalWeight[k] / max(heightSum, 1e-6), heightBlend);

            // Large-scale variation beyond 32 m: a 1/8-scale, rotated sample
            // modulates brightness against the layer's mean color.
            const float detailAmount = smoothstep(32.0, 96.0, nearDistance) *
                (1.0 - macroBlend);
            float3 blendedNormal = 0.0;
            float roughnessSum = 0.0;
            float occlusionSum = 0.0;
            [unroll] for (uint k = 0u; k < 3u; ++k)
            {
                const float w = finalWeight[k];
                if (w <= 0.0) continue;
                const uint layer = selected[k];
                const float4 params = objectData.terrainLayerParams[layer];
                const float3 macroColor = objectData.terrainMacroColors[layer].rgb;
                if (masks.w & (1u << layer))
                {
                    float3 albedo = samples[k].albedo;
                    [branch] if (detailAmount > 0.0)
                    {
                        const float scale = (params.w > 0.0 ? params.w : 1.0) * 0.125;
                        const float3 far = terrainAlbedoHeight.SampleGrad(materialSampler,
                            float3(float2(-terrainUv.y, terrainUv.x) * scale, (float)layer),
                            float2(-terrainDx.y, terrainDx.x) * scale,
                            float2(-terrainDy.y, terrainDy.x) * scale).rgb;
                        const float3 luminance = float3(0.2126, 0.7152, 0.0722);
                        const float variation = dot(far, luminance) /
                            max(dot(macroColor, luminance), 0.02);
                        albedo *= lerp(1.0, clamp(variation, 0.75, 1.25), detailAmount * 0.75);
                    }
                    base.rgb += lerp(albedo, macroColor, macroBlend) *
                        (w * originalWeightSum);
                }
                roughnessSum += params.x * lerp(1.0, samples[k].roughness, materialDetail) * w;
                occlusionSum += lerp(1.0, samples[k].occlusion, materialDetail) * w;
                blendedNormal += samples[k].normal * w;
            }
            if (selectedSum > 0.00001)
            {
                terrainRoughness = clamp(roughnessSum, 0.045, 1.0);
                terrainOcclusion = saturate(occlusionSum);
                if (dot(blendedNormal, blendedNormal) > 1e-8)
                    terrainNormalForLighting = normalize(blendedNormal);
            }
        }
        if (!hexTiling)
            base.rgb *= 1.0 + (terrainVariation.x + terrainVariation.y) * 0.10;
        // Lush valley floors (Terrain::lushness, uvChannels1.w): the darker,
        // greener tint the macro color map gives the same ground far away
        // (LandscapeDriver ApplyErosionPaint, LushTint).
        base.rgb *= lerp(float3(1.0, 1.0, 1.0), float3(0.72, 0.92, 0.55),
            saturate(uvChannels1.w));
        // Ground under ground cover (Terrain::GroundCover). Inside the cover's
        // range the gaps between plants show darker root-coloured
        // undergrowth instead of the bare layer texture; across and past its
        // fade the cover's own colour, with the same dry/lush pattern as the
        // blades (GrassColorVariation), carries on so its edge does not show.
        // The layer texture survives as brightness detail.
        [branch] if (objectData.groundCoverColor.w > 0.0 && originalWeightSum > 0.00001)
        {
            const uint coverMask = (uint)objectData.groundCoverRange.z;
            float cover = 0.0;
            float3 layerMean = 0.0;
            [unroll] for (uint layer = 0u; layer < 6u; ++layer)
            {
                if (coverMask & (1u << layer)) cover += weights[layer];
                layerMean += objectData.terrainMacroColors[layer].rgb * weights[layer];
            }
            cover = saturate(cover / originalWeightSum);
            [branch] if (cover > 0.0)
            {
                layerMean /= originalWeightSum;
                const float3 luma = float3(0.2126, 0.7152, 0.0722);
                const float detail = clamp(dot(base.rgb, luma) /
                    max(dot(layerMean, luma) * originalWeightSum, 0.002), 0.6, 1.4);
                const float fadeStart = objectData.groundCoverRange.x;
                const float fadeEnd = objectData.groundCoverRange.y;
                const float beyond = fadeEnd > fadeStart
                    ? smoothstep(fadeStart, fadeEnd, nearDistance) : 1.0;
                const float3 coverColor = objectData.groundCoverColor.rgb;
                const float3 target = GrassPatchVariation(lerp(
                    coverColor * objectData.groundCoverRange.w, coverColor, beyond), worldPos);
                // Keep more texture detail up close, where it reads as soil.
                float3 tinted = target * lerp(1.0, detail, lerp(0.8, 0.5, beyond));
                // Where the blades thin out and beyond them the ground is
                // shaded as grass tufts with depth (GrassGroundShells) rather
                // than flat paint, so it carries on from the blade field.
                [branch] if (beyond > 0.0)
                {
                    // The pixel's narrow axis: at a grazing angle a pixel covers a
                    // long strip of ground along the view, but the tufts slide along
                    // that same direction, so they stay readable across it.
                    const float footprint = min(length(ddx(worldPos.xz)), length(ddy(worldPos.xz)));
                    const float3 toCamera = normalize(objectData.viewPositionAlphaCutoff.xyz - worldPos);
                    tinted *= lerp(1.0, GrassGroundShells(worldPos, toCamera,
                        terrainNormalForLighting, footprint), beyond);
                    // Scene time is 0 for draws without wind (SceneRendering.cpp).
                    [branch] if (objectData.grassAnimation.x > 0.0)
                        tinted *= lerp(1.0, GrassGroundWind(worldPos, objectData.grassAnimation.x,
                            toCamera), beyond);
                }
                base.rgb = lerp(base.rgb, tinted * originalWeightSum,
                    cover * objectData.groundCoverColor.w);
            }
        }
        // Triplanar rock (VolumeTerrain's steep faces) marks its vertices
        // with a negative slice (uvChannels4: repeats per metre, -,
        // -(slice + 1), blend): a seamless rock tile (tools/cliff_rock_texture.py),
        // a layer-array slice after the painted layers, projected from three
        // sides, its normals combined with the surface's on each side, tinted
        // at the vertex (uvChannels0.zw, uvChannels1.z).
        const float rockBlend = (flags & 262144u) && macroBlend < 0.999 && uvChannels4.z < -0.5
            ? saturate(uvChannels4.w) : 0.0;
        terrainRockShare = rockBlend;
        [branch] if (rockBlend > 0.001)
        {
            const float slice = -uvChannels4.z - 1.0;
            const float scale = uvChannels4.x;
            const float3 surface = normalize(terrainNormalForLighting);
            float3 sideWeights = pow(abs(surface), 4.0);
            sideWeights /= max(sideWeights.x + sideWeights.y + sideWeights.z, 1.0e-4);
            const float2 uvX = worldPos.zy * scale, uvY = worldPos.xz * scale, uvZ = worldPos.xy * scale;
            const float2 dX0 = worldDx.zy * scale, dX1 = worldDy.zy * scale;
            const float2 dY0 = worldDx.xz * scale, dY1 = worldDy.xz * scale;
            const float2 dZ0 = worldDx.xy * scale, dZ1 = worldDy.xy * scale;
            float4 albedoSum = 0.0;
            float2 roughAoSum = 0.0;
            float3 normalSum = 0.0;
            [branch] if (sideWeights.x > 0.01)
            {
                const float3 c = float3(uvX, slice);
                albedoSum += terrainAlbedoHeight.SampleGrad(materialSampler, c, dX0, dX1) * sideWeights.x;
                roughAoSum += terrainRoughAo.SampleGrad(materialSampler, c, dX0, dX1).rg * sideWeights.x;
                float3 t = UnpackTerrainNormal(terrainNormal.SampleGrad(materialSampler, c, dX0, dX1).rg);
                t = float3(t.xy + surface.zy, abs(t.z) * surface.x);
                normalSum += t.zyx * sideWeights.x;
            }
            [branch] if (sideWeights.y > 0.01)
            {
                const float3 c = float3(uvY, slice);
                albedoSum += terrainAlbedoHeight.SampleGrad(materialSampler, c, dY0, dY1) * sideWeights.y;
                roughAoSum += terrainRoughAo.SampleGrad(materialSampler, c, dY0, dY1).rg * sideWeights.y;
                float3 t = UnpackTerrainNormal(terrainNormal.SampleGrad(materialSampler, c, dY0, dY1).rg);
                t = float3(t.xy + surface.xz, abs(t.z) * surface.y);
                normalSum += t.xzy * sideWeights.y;
            }
            [branch] if (sideWeights.z > 0.01)
            {
                const float3 c = float3(uvZ, slice);
                albedoSum += terrainAlbedoHeight.SampleGrad(materialSampler, c, dZ0, dZ1) * sideWeights.z;
                roughAoSum += terrainRoughAo.SampleGrad(materialSampler, c, dZ0, dZ1).rg * sideWeights.z;
                float3 t = UnpackTerrainNormal(terrainNormal.SampleGrad(materialSampler, c, dZ0, dZ1).rg);
                t = float3(t.xy + surface.xy, abs(t.z) * surface.z);
                normalSum += t.xyz * sideWeights.z;
            }
            const float3 tint = float3(uvChannels0.zw, uvChannels1.z);
            base.rgb = lerp(base.rgb, albedoSum.rgb * tint, rockBlend);
            terrainNormalForLighting = normalize(lerp(terrainNormalForLighting,
                normalize(normalSum), rockBlend));
            terrainRoughness = lerp(terrainRoughness, clamp(roughAoSum.x, 0.045, 1.0), rockBlend);
            terrainOcclusion = lerp(terrainOcclusion, roughAoSum.y, rockBlend);
        }
        // Toward the edge of the loaded cells, fade to the macro color map
        // the distant tiles beyond use, so both sides meet with one color.
        // Rock keeps its own surface: a flat macro texel over a rock face
        // changed it as its cell came within the stride-1 rings.
        [branch] if (objectData.terrainMacroParams.z > 0.5)
        {
            const float macroAmount = smoothstep(objectData.terrainMacroParams.x,
                objectData.terrainMacroParams.y, nearDistance) * (1.0 - terrainRockShare);
            // The macro map carries no paths (they are shaded per pixel).
            [branch] if (macroAmount > 0.0)
                base.rgb = lerp(base.rgb, lerp(SampleTerrainMacroColor(objectData,
                    worldPos, worldDx, worldDy).rgb, objectData.terrainPathFar.rgb, pathShare),
                    macroAmount);
        }
    }
    // Static runtime terrain shares one global sun at every view distance.
    // It takes a single-tap realtime shadow across the whole shadow range,
    // including the far cascade: hills and forests shade the ground at any
    // distance, and the cascades fade out on their own at the range's end.
    // Triplanar rock faces take the full lighting below; this pass's flat
    // ambient and Lambert sun left them far darker than scanned rock in the
    // same light.
    if ((flags & 8192u) && objectData.ambientUnlit.w < 0.5 && terrainRockShare < 0.5)
    {
        float3 terrainNormal = terrainNormalForLighting;
        // Canopy shade only on ground trees can stand on: on steep slopes the
        // estimate's taps land inside the hillside (and a mountain's own
        // shadow is not a canopy), which turned rock faces nearly black.
        float3 diffuseLight = objectData.ambientUnlit.rgb * terrainOcclusion *
            ScreenAmbientOcclusion(worldPos) * lerp(1.0, CanopySkyVisibility(objectData, worldPos),
                smoothstep(0.72, 0.9, terrainNormal.y));
        float3 specularLight = 0.0;
        float3 sunDirection = objectData.bakedLightDirection.xyz;
        float sunFacing = saturate(dot(terrainNormal, sunDirection));
        if (sunFacing > 0.0)
        {
            float shadow = 1.0;
            [branch] if (objectData.bakedDirectional.w > 0.5 &&
                objectData.shadowParams.z > 0.5)
            {
                SceneLightData sun = sceneLights[
                    (uint)objectData.bakedDirectional.w - 1u];
                shadow = EvaluateDirectionalShadow(objectData, sun,
                    worldPos, terrainNormal, sunDirection, 0);
            }
            // The long-range shadow is traced against the height grid; rock
            // faces (volume overhangs) stand off it, so the grid shadowed
            // them wrongly. They are real geometry in the cascades above.
            shadow *= terrainReliefShadow *
                lerp(TerrainLongRangeShadow(objectData, worldPos), 1.0, terrainRockShare) *
                SunCookie(objectData, worldPos);
            diffuseLight += objectData.bakedDirectional.rgb *
                (sunFacing * shadow / PI);
            // Near ring: one GGX sun lobe on the blended normal and roughness
            // so wet rock and packed soil keep a highlight. Dielectric F0.
            const float specularFade = 1.0 - smoothstep(64.0, 96.0,
                distance(worldPos, objectData.viewPositionAlphaCutoff.xyz));
            [branch] if (specularFade > 0.0)
            {
                const float3 viewDir = normalize(
                    objectData.viewPositionAlphaCutoff.xyz - worldPos);
                const float3 halfDir = normalize(viewDir + sunDirection);
                const float nDotV = max(dot(terrainNormal, viewDir), 0.001);
                const float specular = DistributionGGX(terrainNormal, halfDir, terrainRoughness) *
                    GeometrySmith(terrainNormal, viewDir, sunDirection, terrainRoughness) /
                    (4.0 * nDotV * sunFacing + 0.0001) *
                    FresnelSchlick(saturate(dot(halfDir, viewDir)), 0.04).x;
                specularLight = objectData.bakedDirectional.rgb *
                    (specular * sunFacing * shadow * specularFade);
            }
        }
        return float4(base.rgb * diffuseLight + specularLight +
            objectData.emissiveOcclusion.rgb, base.a);
    }
    float metallic = saturate(objectData.materialParams.x);
    float roughness = (flags & 2048u) ? terrainRoughness :
        clamp(objectData.materialParams.y, 0.045, 1.0);
    float occlusion = (flags & 2048u) ? terrainOcclusion : 1.0;
    float3 emissive = objectData.emissiveOcclusion.rgb;
    float2 baseUv = SelectMaterialUv(objectData.textureUvSets0.x, uv, uv1,
        uvChannels0, uvChannels1, uvChannels2, uvChannels3, uvChannels4);
    float2 metallicRoughnessUv = SelectMaterialUv(objectData.textureUvSets0.y, uv, uv1,
        uvChannels0, uvChannels1, uvChannels2, uvChannels3, uvChannels4);
    float2 normalUv = SelectMaterialUv(objectData.textureUvSets0.z, uv, uv1,
        uvChannels0, uvChannels1, uvChannels2, uvChannels3, uvChannels4);
    float2 occlusionUv = SelectMaterialUv(objectData.textureUvSets0.w, uv, uv1,
        uvChannels0, uvChannels1, uvChannels2, uvChannels3, uvChannels4);
    float2 emissiveUv = SelectMaterialUv(objectData.textureUvSets1.x, uv, uv1,
        uvChannels0, uvChannels1, uvChannels2, uvChannels3, uvChannels4);
    float2 heightUv = SelectMaterialUv(objectData.textureUvSets1.y, uv, uv1,
        uvChannels0, uvChannels1, uvChannels2, uvChannels3, uvChannels4);

    const FoliageLeaf leaf = GetFoliageLeaf(objectData, flags, uv1);
    float3 geometryNormal = normalize(normal) * (frontFace || leaf.cooked ? 1.0 : -1.0);
    bool hasTangents = dot(tangent.xyz, tangent.xyz) > 0.0001;
    float3 tangentDirection = hasTangents
        ? normalize(tangent.xyz - geometryNormal *
            dot(geometryNormal, tangent.xyz))
        : float3(1.0, 0.0, 0.0);
    float3 bitangentDirection =
        cross(geometryNormal, tangentDirection) * tangent.w;
    float3 viewDirection = normalize(
        objectData.viewPositionAlphaCutoff.xyz - worldPos);

    // Triplanar props (Material::triplanarScale, metres per repeat). Terrain
    // and the measurement grid use this lane for their own tiling.
    const float triplanarScale = (flags & (2048u | 32768u)) == 0u
        ? objectData.textureUvSets1.z : 0.0;
    const bool triplanar = triplanarScale > 0.0;
    const float3 worldPosDx = ddx(worldPos);
    const float3 worldPosDy = ddy(worldPos);
    float3 triplanarPos = 0.0, triplanarDx = 0.0, triplanarDy = 0.0, triplanarWeights = 0.0;
    if (triplanar)
    {
        const float repeatsPerMetre = 1.0 / triplanarScale;
        const float gradientScale = exp2(textureBias) * repeatsPerMetre;
        triplanarPos = worldPos * repeatsPerMetre;
        triplanarDx = worldPosDx * gradientScale;
        triplanarDy = worldPosDy * gradientScale;
        // Sharp blend: mostly one projection, short transitions, and
        // projections under 4% skipped entirely.
        float3 w = pow(abs(geometryNormal), 4.0);
        w /= max(w.x + w.y + w.z, 1e-5);
        w = saturate((w - 0.04) / 0.96);
        triplanarWeights = w / max(w.x + w.y + w.z, 1e-5);
    }

    // Derivatives must be taken in uniform control flow, before the branch.
    const float2 heightUvDx = ddx(heightUv);
    const float2 heightUvDy = ddy(heightUv);
    // World metres per UV unit, for converting relief depth to world depth.
    const float worldPerUv = sqrt(length(cross(ddx(worldPos), ddy(worldPos))) /
        max(abs(heightUvDx.x * heightUvDy.y - heightUvDx.y * heightUvDy.x), 1e-14));
    const uint parallaxFlags = (uint)objectData.parallaxParams.w;
    const bool heightIsDepth = (parallaxFlags & ParallaxFlagHeightIsDepth) != 0u;
    const float parallaxHeightScale = objectData.parallaxParams.x * materialDetail;
    ParallaxHit parallaxHit = (ParallaxHit)0;
    // Relief self-shadow weight: material strength, faded out by 15 m.
    float parallaxShadowWeight = 0.0;
    if (materialDetail > 0.0 && (flags & 64u) && hasTangents && !triplanar &&
        objectData.parallaxParams.x > 0.0)
    {
        float3 viewDirectionTangent = float3(
            dot(viewDirection, tangentDirection),
            dot(viewDirection, bitangentDirection),
            dot(viewDirection, geometryNormal));
        parallaxHit = ParallaxOcclusion(
            heightUv, heightUvDx, heightUvDy, viewDirectionTangent,
            parallaxHeightScale,
            objectData.parallaxParams.y, objectData.parallaxParams.z, textureBias,
            heightIsDepth);
        // Decals and trim sheets: relief that leaves the 0..1 tile is cut.
        if (parallaxFlags & ParallaxFlagClampUv)
            clip(min(min(parallaxHit.uv.x, parallaxHit.uv.y),
                1.0 - max(parallaxHit.uv.x, parallaxHit.uv.y)));
        if ((parallaxFlags & ParallaxFlagSelfShadow) && parallaxHit.marched)
            parallaxShadowWeight = saturate(frac(objectData.parallaxParams.w) * 2.0) *
                (1.0 - smoothstep(10.0, 15.0, distance(worldPos,
                    objectData.viewPositionAlphaCutoff.xyz)));
        // Push the pixel along the view ray to the relief hit so props and
        // contact with other geometry intersect the visible surface.
        [branch] if ((parallaxFlags & ParallaxFlagDepthOffset) && parallaxHit.depth > 0.0)
        {
            const float rayLength = parallaxHit.depth * parallaxHeightScale *
                worldPerUv / max(abs(viewDirectionTangent.z), 0.08);
            const float4 hitClip = mul(objectData.viewProjection,
                float4(worldPos - viewDirection * rayLength, 1.0));
            g_parallaxClipDepth = hitClip.z / hitClip.w;
        }
        float2 parallaxOffset = parallaxHit.uv - heightUv;
        baseUv += parallaxOffset; metallicRoughnessUv += parallaxOffset;
        normalUv += parallaxOffset; occlusionUv += parallaxOffset;
        emissiveUv += parallaxOffset;
    }

    [branch] if ((flags & 1u) && triplanar)
        base *= SampleTriplanar(baseColorMap, triplanarPos, triplanarDx, triplanarDy,
            triplanarWeights) * objectData.pmxTextureTint;
    else if (flags & 1u)
        base *= SampleObjectBaseColor(objectData, baseUv, textureBias,
            base.a * objectData.pmxTextureTint.a,
            AlphaLodBias(objectData, worldPos)) *
            objectData.pmxTextureTint;
    // Reject empty leaf-card pixels before fetching normal, roughness or occlusion.
    if (flags & 32u)
    {
        clip(base.a - objectData.viewPositionAlphaCutoff.w);
        ApplyMaskedAlphaTransmission(objectData, maskUv, maskUvDx, maskUvDy, edgeFade);
        if (IsPerformanceHeatDraw(objectData))
            return PerformanceHeatColor(objectData, worldPos, normal, frontFace, base.a);
    }
    [branch] if ((flags & 32u) && (flags & 2048u) == 0u && objectData.textureUvSets1.w > 0.0)
        base.rgb = ApplyFoliageVariation(base.rgb, worldPos, objectData.textureUvSets1.w);
    float subsurfaceThickness = objectData.subsurfaceRadiusThickness.w;
    if ((flags & 128u) && objectData.subsurfaceColorWeight.w > 0.0)
        subsurfaceThickness *= subsurfaceThicknessMap.SampleBias(
            materialSampler, baseUv, textureBias).r;
    [branch] if ((flags & 2u) && materialDetail > 0.0)
    {
        float4 mr;
        [branch] if (triplanar)
            mr = SampleTriplanar(metallicRoughnessMap, triplanarPos, triplanarDx, triplanarDy,
                triplanarWeights);
        else
            mr = metallicRoughnessMap.SampleBias(materialSampler, metallicRoughnessUv, textureBias);
        roughness *= lerp(1.0, mr.g, materialDetail);
        metallic *= lerp(1.0, mr.b, materialDetail);
    }
    roughness = clamp(roughness, 0.045, 1.0);
    metallic = saturate(metallic);
    const bool blinnPhongSpecular = objectData.blinnPhongSpecularShininess.w > 0.0;
    if (blinnPhongSpecular)
        roughness = clamp(sqrt(2.0 /
            (objectData.blinnPhongSpecularShininess.w + 1.0)), 0.045, 1.0);
    [branch] if ((flags & 8u) && materialDetail > 0.0)
    {
        float sampledOcclusion;
        [branch] if (triplanar)
            sampledOcclusion = SampleTriplanar(occlusionMap, triplanarPos, triplanarDx,
                triplanarDy, triplanarWeights).r;
        else
            sampledOcclusion = occlusionMap.SampleBias(materialSampler, occlusionUv, textureBias).r;
        occlusion = lerp(1.0, sampledOcclusion, objectData.emissiveOcclusion.w * materialDetail);
    }
    if (flags & 16u)
        emissive *= emissiveMap.SampleBias(materialSampler, emissiveUv, textureBias).rgb;
    // Cooked leaves: sky and ambient light reach the crown interior far less
    // than its shell. Direct light keeps more of it (the shadow map already
    // darkens the interior, but too coarsely for single leaves).
    occlusion *= leaf.occlusion;
    // Sky light under a tree canopy (cooked leaves carry their own crown
    // occlusion instead).
    // Not on steep terrain (taps would land inside the hillside).
    if (!leaf.cooked)
        occlusion *= lerp(1.0, CanopySkyVisibility(objectData, worldPos),
            (flags & 2048u) ? smoothstep(0.72, 0.9, terrainNormalForLighting.y) : 1.0);
    const float leafDirect = lerp(0.45, 1.0, leaf.occlusion);
    normal = (flags & 2048u) ? terrainNormalForLighting : geometryNormal;
    [branch] if ((flags & 4u) && triplanar && materialDetail > 0.0)
        normal = SampleTriplanarNormal(triplanarPos, triplanarDx, triplanarDy, triplanarWeights,
            geometryNormal, objectData.materialParams.z * materialDetail);
    else if ((flags & 4u) && hasTangents && materialDetail > 0.0)
    {
        float3 sampledNormal = UnpackNormal(normalMap.SampleBias(materialSampler, normalUv, textureBias).xyz);
        sampledNormal.xy *= objectData.materialParams.z * materialDetail;
        normal = normalize(
            sampledNormal.x * tangentDirection +
            sampledNormal.y * bitangentDirection +
            sampledNormal.z * geometryNormal);
    }
    if ((flags & 256u) && objectData.pmxParams.y > 0.5)
    {
        // Modes 1 and 2 project the posed world normal in camera space.
        // Mode 3 uses the first PMX additional UV channel directly.
        float2 sphereUv = objectData.pmxParams.y > 2.5 ? uv1 :
            normalize(mul((float3x3)objectData.sphereView,
                geometryNormal)).xy * float2(0.5, -0.5) + 0.5;
        float4 sphere = sphereMap.SampleBias(materialSampler, sphereUv, textureBias) *
            objectData.pmxSphereTint;
        if (objectData.pmxParams.y > 1.5 && objectData.pmxParams.y < 2.5)
            base.rgb += sphere.rgb * sphere.a;
        else
            base.rgb *= lerp(float3(1.0, 1.0, 1.0), sphere.rgb,
                saturate(sphere.a));
    }


    if (objectData.ambientUnlit.w > 0.5)
        return float4(base.rgb + emissive, base.a);

    float3 n = normalize(normal);
    float3 v = viewDirection;
    // Cooked leaf normals point out of the crown on both card faces, so on
    // the crown's far side they can face away from the viewer; the GGX term
    // then divides by n.v ~ 0 and sparkles white. Bend them just into the
    // view hemisphere.
    [branch] if (leaf.cooked && dot(n, v) < 0.1)
        n = normalize(n + v * (0.1 - dot(n, v)));
    float3 f0 = blinnPhongSpecular
        ? saturate(objectData.blinnPhongSpecularShininess.rgb)
        : lerp(float3(0.04, 0.04, 0.04), base.rgb, metallic);
    float3 directLight = 0.0;
    float toonLight = 0.0;
    uint lightListOffset = 0u;
    uint lightIterationCount = draw.lightCount;
    const bool clusteredLighting = (draw.drawFlags & 1u) != 0u;
    if (clusteredLighting)
    {
        uint tileSize = max((uint)objectData.lightingClusterParams.x, 1u);
        uint tilesX = max((uint)objectData.lightingClusterParams.y, 1u);
        uint tilesY = max((uint)objectData.lightingClusterParams.z, 1u);
        uint depthSlices = max((uint)objectData.lightingClusterParams.w, 1u);
        uint tileX = min((uint)pos.x / tileSize, tilesX - 1u);
        uint tileY = min((uint)pos.y / tileSize, tilesY - 1u);
        float viewDepth = max(0.001,
            dot(worldPos - objectData.viewPositionAlphaCutoff.xyz,
                objectData.lightingCameraData.xyz));
        float nearPlane = max(objectData.shadowCameraData.w, 0.001);
        uint depthSlice = min((uint)max(0.0,
            log2(viewDepth / nearPlane) * objectData.lightingCameraData.w),
            depthSlices - 1u);
        uint cluster = depthSlice * tilesX * tilesY + tileY * tilesX + tileX;
        uint2 header = clusteredLightGrid[cluster];
        lightListOffset = header.x;
        lightIterationCount = min(header.y, draw.lightCount);
    }
    [loop] for (uint iteration = 0; iteration < lightIterationCount; ++iteration)
    {
        uint index = clusteredLighting
            ? clusteredLightIndices[lightListOffset + iteration]
            : iteration;
        SceneLightData light = sceneLights[index];
        if ((objectData.lightingChannels.x & asuint(light.attenuation.z)) == 0u)
            continue;
        float3 lightDirection;
        float attenuation = 1.0;
        bool directional = abs(light.params.y - 1.0) < 0.25;
        if (directional)
        {
            lightDirection = normalize(light.positionRange.xyz);
        }
        else
        {
            float3 toLight = light.positionRange.xyz - worldPos;
            float distanceToLight = length(toLight);
            lightDirection = distanceToLight > 0.0001
                ? toLight / distanceToLight : float3(0.0, 1.0, 0.0);
            float normalizedDistance = distanceToLight /
                max(light.positionRange.w, 0.0001);
            if (light.attenuation.y > 0.5)
            {
                float smoothCutoff = saturate(1.0 -
                    normalizedDistance * normalizedDistance *
                    normalizedDistance * normalizedDistance);
                attenuation = smoothCutoff * smoothCutoff /
                    max(distanceToLight * distanceToLight, 0.01);
            }
            else
                attenuation = pow(saturate(1.0 - normalizedDistance),
                    max(light.params.x, 0.1));

            if (light.params.y > 1.5)
            {
                float cosine = dot(-lightDirection,
                    normalize(light.directionCone.xyz));
                attenuation *= smoothstep(light.directionCone.w,
                    max(light.attenuation.x, light.directionCone.w + 0.0001),
                    cosine);
            }
        }
        uint cookieIndex = (uint)light.photometry.x;
        uint iesIndex = (uint)light.photometry.y;
        if (cookieIndex != 0u && light.params.y > 0.5)
        {
            float2 cookieUv;
            if (directional)
                attenuation *= DirectionalCookie(light, worldPos);
            else
            {
                float3 fromLight = worldPos - light.positionRange.xyz;
                float forward = max(dot(fromLight,
                    normalize(light.directionCone.xyz)), 0.0001);
                float coneRadius = forward * sqrt(max(1.0 /
                    max(light.directionCone.w * light.directionCone.w, 0.0001) - 1.0, 0.0));
                cookieUv = float2(dot(fromLight, light.cookieRight.xyz),
                    dot(fromLight, light.cookieUp.xyz)) /
                    max(2.0 * coneRadius * light.photometry.z, 0.0001) + 0.5;
                if (any(cookieUv < 0.0) || any(cookieUv > 1.0)) attenuation = 0.0;
                attenuation *= SampleCookie(cookieIndex, cookieUv);
            }
        }
        if (iesIndex != 0u && light.params.y > 1.5)
            attenuation *= SampleIes(iesIndex, dot(-lightDirection,
                normalize(light.directionCone.xyz)));
        const float nDotLRaw = dot(n, lightDirection);
        float nDotL = saturate(nDotLRaw);
        toonLight = max(toonLight, nDotL);
        float3 radiance = light.colorIntensity.rgb *
            light.colorIntensity.w * attenuation;
        float scatteringWeight = objectData.subsurfaceColorWeight.w * materialDetail;
        float centerShadow = 1.0;
        // Unlit-facing surfaces have no direct contribution to shadow unless
        // subsurface scattering or leaf transmission is active. Avoid
        // filtering an unused shadow.
        [branch] if (nDotL > 0.0 || scatteringWeight > 0.0 || leaf.on)
            centerShadow = EvaluateDirectionalShadow(
                objectData, light, worldPos,
                FoliageShadowNormal(leaf, facet, n, lightDirection), lightDirection,
                (int)objectData.shadowParams.w);
        // Relief self-shadow for directional lights, near the camera only.
        [branch] if (directional && nDotL > 0.0 && parallaxShadowWeight > 0.0)
        {
            const float3 lightDirectionTangent = float3(
                dot(lightDirection, tangentDirection),
                dot(lightDirection, bitangentDirection),
                dot(lightDirection, geometryNormal));
            const float shadowSteps = round(lerp(6.0, 16.0, parallaxHit.depth));
            centerShadow *= lerp(1.0, ParallaxSelfShadow(parallaxHit,
                lightDirectionTangent, parallaxHeightScale, heightIsDepth,
                shadowSteps), parallaxShadowWeight);
            if (parallaxHit.depth > 0.001 && lightDirectionTangent.z > 0.0)
                parallaxHit.fetches += shadowSteps;
        }
        if (index == terrainReliefLight) centerShadow *= terrainReliefShadow;
        [branch] if (nDotL > 0.0)
        directLight += EvaluatePbrBrdf(
            base.rgb, metallic, roughness, f0, n, v, lightDirection, materialDetail) *
            radiance * centerShadow * nDotL * leafDirect;
        [branch] if (leaf.on)
            directLight += radiance * centerShadow * base.rgb *
                ((1.0 - metallic) / PI * FoliageWrapExtra(nDotLRaw) * leafDirect) +
                FoliageTransmission(base.rgb, radiance, n, v, lightDirection,
                    centerShadow, leaf);
        if (scatteringWeight > 0.0)
        {
            float3 radius = max(objectData.subsurfaceRadiusThickness.rgb, 0.0);
            float3 wrap = radius / (radius + 0.01);
            float3 wrappedNdotL = saturate((dot(geometryNormal,
                lightDirection) + wrap) / (1.0 + wrap));
            float3 halfDirection = normalize(v + lightDirection);
            float3 diffuse = (1.0 - FresnelSchlick(
                saturate(dot(halfDirection, v)), f0)) *
                (1.0 - metallic) * base.rgb / PI;
            float3 tint = saturate(objectData.subsurfaceColorWeight.rgb);
            float3 diffusedShadow = EvaluateSubsurfaceShadow(objectData,
                light, worldPos, geometryNormal, lightDirection,
                radius, centerShadow);
            directLight += diffuse * radiance * scatteringWeight *
                (tint * wrappedNdotL * diffusedShadow -
                    nDotL * centerShadow);
            float3 transmission = exp(-max(subsurfaceThickness, 0.0) /
                max(radius, 0.000001));
            directLight += diffuse * radiance * centerShadow * tint * transmission *
                scatteringWeight * saturate(objectData.subsurfaceTransmission.x) *
                saturate(-dot(geometryNormal, lightDirection));
        }
    }

    float3 bakedDirect = 0.0;
    if (dot(objectData.bakedDirectional.rgb,
        objectData.bakedDirectional.rgb) > 0.000001)
    {
        float3 bakedDirection = normalize(objectData.bakedLightDirection.xyz);
        bakedDirect = EvaluatePbrBrdf(
            base.rgb, metallic, roughness, f0, n, v, bakedDirection, materialDetail) *
            objectData.bakedDirectional.rgb * saturate(dot(n, bakedDirection));
    }
    const float screenOcclusionFactor = ScreenAmbientOcclusion(worldPos);
    float3 ambient = objectData.ambientUnlit.rgb *
        (base.rgb * (1.0 - metallic) + f0 * 0.25) * occlusion * screenOcclusionFactor;
    float3 environment = 0.0;
    bool useEnvironmentDiffuse = objectData.environmentParams.x > 0.0 &&
        objectData.environmentParams.z > 0.0;
    bool useEnvironmentReflection = objectData.environmentParams.w > 0.0 &&
        (objectData.environmentParams.x > 0.0 ||
            objectData.reflectionEnvironmentParams.z > 0.5);
    if (useEnvironmentDiffuse || useEnvironmentReflection)
    {
        float3 diffuseEnvironment = 0.0;
        if (useEnvironmentDiffuse)
        {
            float3 diffuseIrradiance = EvaluateEnvironmentIrradiance(objectData, n);
            diffuseEnvironment = diffuseIrradiance * base.rgb *
                (1.0 - metallic) / PI * objectData.environmentParams.z *
                objectData.environmentParams.x;
        }

        float3 specularEnvironment = 0.0;
        if (useEnvironmentReflection)
        {
            float3 reflectionDirection = reflect(-v, n);
            float3 reflectedRadiance;
            if (objectData.reflectionEnvironmentParams.w > 0.5)
                reflectedRadiance = EvaluateDirectReflectionRadiance(
                    objectData, reflectionDirection, roughness);
            else
            {
                float3 roughReflectionDirection = normalize(lerp(
                    reflectionDirection, n, roughness * roughness));
                reflectedRadiance =
                    objectData.reflectionEnvironmentParams.z > 0.5
                        ? EvaluateCustomReflectionRadiance(
                            objectData, roughReflectionDirection)
                        : EvaluateEnvironmentRadiance(
                            objectData, roughReflectionDirection) *
                            objectData.environmentParams.x;
            }
            float2 integratedBrdf = brdfIntegrationLut.SampleLevel(
                materialSampler,
                float2(saturate(dot(n, v)), roughness), 0.0).rg;
            specularEnvironment = reflectedRadiance *
                (f0 * integratedBrdf.x + integratedBrdf.y) *
                objectData.environmentParams.w;
        }
        environment = (diffuseEnvironment + specularEnvironment) * occlusion *
            screenOcclusionFactor;
    }
    float3 litColor = ambient + environment + directLight + bakedDirect;
    if ((flags & 512u) || objectData.pmxParams.z >= 0.0)
    {
        float builtinToon = lerp(0.65, 1.0,
            smoothstep(0.0, 1.0, toonLight));
        float3 toon = (flags & 512u)
            ? toonMap.Sample(materialSampler,
                float2(0.5, clamp(1.0 - toonLight, 0.001, 0.999))).rgb
            : float3(builtinToon, builtinToon, builtinToon);
        litColor *= lerp(float3(1.0, 1.0, 1.0),
            toon * objectData.pmxToonTint.rgb,
            saturate(objectData.pmxToonTint.a));
    }
    // Parallax cost view: height fetches per pixel on the performance ramp,
    // 0 = blue, 32 (the worst-case ground budget) = red; grey = no march.
    [branch] if (parallaxFlags & ParallaxFlagCostView)
    {
        if (parallaxHit.fetches <= 0.0)
            return float4(0.25, 0.25, 0.25, base.a);
        ObjectData heat = objectData;
        heat.morphParams.z = -1.0 - saturate(parallaxHit.fetches / 32.0);
        return PerformanceHeatColor(heat, worldPos, geometryNormal, true, base.a);
    }
    return float4(litColor + emissive, base.a);
}

#ifdef PSMain
// A material shader renamed PSMain before including this file to define its
// own entry point: give it the surface shader under that name. It sets
// g_instanceLodDistance itself if it reads TEXCOORD12.
float4 PSMain(
    float4 pos : SV_POSITION, float3 worldPos : TEXCOORD2,
    float3 normal : NORMAL, float2 uv : TEXCOORD, float2 uv1 : TEXCOORD1,
    float4 vertexColor : COLOR, float4 tangent : TANGENT,
    float4 uvChannels0 : TEXCOORD3, float4 uvChannels1 : TEXCOORD4,
    float4 uvChannels2 : TEXCOORD5, float4 uvChannels3 : TEXCOORD6,
    float4 uvChannels4 : TEXCOORD7,
    nointerpolation uint objectIndex : TEXCOORD11,
    bool frontFace : SV_IsFrontFace) : SV_TARGET
{
    return ObjectSurfacePS(pos, worldPos, normal, uv, uv1, vertexColor, tangent,
        uvChannels0, uvChannels1, uvChannels2, uvChannels3, uvChannels4,
        objectIndex, frontFace);
}
#else
float4 PSMain(
    float4 pos : SV_POSITION, float3 worldPos : TEXCOORD2,
    float3 normal : NORMAL, float2 uv : TEXCOORD, float2 uv1 : TEXCOORD1,
    float4 vertexColor : COLOR, float4 tangent : TANGENT,
    float4 uvChannels0 : TEXCOORD3, float4 uvChannels1 : TEXCOORD4,
    float4 uvChannels2 : TEXCOORD5, float4 uvChannels3 : TEXCOORD6,
    float4 uvChannels4 : TEXCOORD7,
    nointerpolation uint objectIndex : TEXCOORD11,
    float instanceLodDistance : TEXCOORD12,
    bool frontFace : SV_IsFrontFace,
    uint primitiveId : SV_PrimitiveID) : SV_TARGET
{
    g_primitiveId = primitiveId;
    g_instanceLodDistance = instanceLodDistance;
    g_billboardFade = uvChannels4.w;
    g_billboardStart = uvChannels4.z;
    ReadPlacementOpacity(uvChannels4);
    return ApplyObjectAtmosphereFog(ObjectSurfacePS(pos, worldPos, normal, uv,
        uv1, vertexColor, tangent, uvChannels0, uvChannels1, uvChannels2,
        uvChannels3, uvChannels4, objectIndex, frontFace), worldPos,
        objectIndex, pos.xy);
}
#endif

// Only used with depth-equal testing and depth writes disabled, after the
// matching alpha/LOD coverage pass. Early rejection avoids lighting hidden
// leaf layers; discards cannot write depth through transparent leaf holes.
[earlydepthstencil]
float4 PSDepthEqualMain(
    float4 pos : SV_POSITION, float3 worldPos : TEXCOORD2,
    float3 normal : NORMAL, float2 uv : TEXCOORD, float2 uv1 : TEXCOORD1,
    float4 vertexColor : COLOR, float4 tangent : TANGENT,
    float4 uvChannels0 : TEXCOORD3, float4 uvChannels1 : TEXCOORD4,
    float4 uvChannels2 : TEXCOORD5, float4 uvChannels3 : TEXCOORD6,
    float4 uvChannels4 : TEXCOORD7,
    nointerpolation uint objectIndex : TEXCOORD11,
    float instanceLodDistance : TEXCOORD12,
    bool frontFace : SV_IsFrontFace,
    uint primitiveId : SV_PrimitiveID) : SV_TARGET
{
    g_primitiveId = primitiveId;
    g_instanceLodDistance = instanceLodDistance;
    g_billboardFade = uvChannels4.w;
    g_billboardStart = uvChannels4.z;
    ReadPlacementOpacity(uvChannels4);
    return ApplyObjectAtmosphereFog(ObjectSurfacePS(pos, worldPos, normal, uv,
        uv1, vertexColor, tangent, uvChannels0, uvChannels1, uvChannels2,
        uvChannels3, uvChannels4, objectIndex, frontFace), worldPos,
        objectIndex, pos.xy);
}

// Opaque parallax materials with parallaxDepthOffset. Depth only moves away
// from the camera, which under reverse-Z is toward 0, so SV_DepthLessEqual
// keeps early-Z rejection against geometry already in front of the flat
// surface.
float4 PSParallaxDepthMain(
    noperspective centroid float4 pos : SV_POSITION, float3 worldPos : TEXCOORD2,
    float3 normal : NORMAL, float2 uv : TEXCOORD, float2 uv1 : TEXCOORD1,
    float4 vertexColor : COLOR, float4 tangent : TANGENT,
    float4 uvChannels0 : TEXCOORD3, float4 uvChannels1 : TEXCOORD4,
    float4 uvChannels2 : TEXCOORD5, float4 uvChannels3 : TEXCOORD6,
    float4 uvChannels4 : TEXCOORD7,
    nointerpolation uint objectIndex : TEXCOORD11,
    float instanceLodDistance : TEXCOORD12,
    bool frontFace : SV_IsFrontFace,
    uint primitiveId : SV_PrimitiveID,
    out float depth : SV_DepthLessEqual) : SV_TARGET
{
    g_primitiveId = primitiveId;
    g_instanceLodDistance = instanceLodDistance;
    g_billboardFade = uvChannels4.w;
    g_billboardStart = uvChannels4.z;
    ReadPlacementOpacity(uvChannels4);
    float4 color = ObjectSurfacePS(pos, worldPos, normal, uv, uv1, vertexColor,
        tangent, uvChannels0, uvChannels1, uvChannels2, uvChannels3,
        uvChannels4, objectIndex, frontFace);
    color = ApplyObjectAtmosphereFog(color, worldPos, objectIndex, pos.xy);
    depth = g_parallaxClipDepth >= 0.0
        ? clamp(g_parallaxClipDepth, 0.0, pos.z) : pos.z;
    return color;
}

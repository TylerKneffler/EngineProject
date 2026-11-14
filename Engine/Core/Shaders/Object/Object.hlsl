// Indexed object/material data and a scene-wide point-light list. Only the
// draw indices/count are push constants on Vulkan.

struct DrawConstants
{
    uint objectIndex;
    uint lightCount;
    uint drawFlags;
    uint padding;
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
    float4 alphaLodParams; // max alpha mip bias, disable mask at far, masked coverage transmission
    float4 grassCapsules[8];
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
StructuredBuffer<SceneLightData> sceneLights : register(t6);
StructuredBuffer<ObjectData> objects : register(t7);
StructuredBuffer<float4x4> boneMatrices : register(t8);
StructuredBuffer<float4> morphDeltas : register(t10);
StructuredBuffer<float4> morphWeights : register(t11);
StructuredBuffer<InstanceTransform> meshInstances : register(t45);
StructuredBuffer<PersistentInstance> persistentInstances : register(t46);
SamplerState materialSampler : register(s0);
#endif

#include "../SkinDeformation.hlsli"

float SampleCookie(uint index, float2 uv)
{
    if (index == 1u) return cookie0.Sample(materialSampler, uv).r;
    if (index == 2u) return cookie1.Sample(materialSampler, uv).r;
    if (index == 3u) return cookie2.Sample(materialSampler, uv).r;
    if (index == 4u) return cookie3.Sample(materialSampler, uv).r;
    return 1.0;
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

#include "../GrassWind.hlsli"
#ifdef GRASS_CAPSULE_INTERACTION
#include "../GrassFootBend.hlsli"
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
    out nointerpolation uint oObjectIndex : TEXCOORD11)
{
    oObjectIndex = ResolveObjectIndex(instanceId);
    ObjectData objectData = objects[oObjectIndex];
    float4 localPosition = float4(pos, 1.0);
    float3 localNormal = normal;
    float3 localTangent = tangent.xyz;
    float3 grassRoot = float3(0, 0, 0);
    float3 grassUp = float3(0, 1, 0);
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
    float3 worldNormal = normalize(mul((float3x3)objectData.world, localNormal));
    float3 worldTangent = mul((float3x3)objectData.world, localTangent);
    float3 worldUp = mul((float3x3)objectData.world, grassUp);
    float upLength = max(length(worldUp), 0.0001);
#ifdef GRASS_CAPSULE_INTERACTION
    float3 restWorldPosition = worldPosition.xyz;
    float3 restWorldNormal = worldNormal;
    float3 restWorldTangent = worldTangent;
#endif
    // Wind must not deform shadow casters; otherwise shadow maps redraw for
    // every shader-driven vertex change while the mesh is still static in the
    // light pass.
    const bool useGrassWind = objectData.grassAnimation.y > 0.5 &&
        objectData.shadowCasterFade.z <= 0.5;
    if (useGrassWind)
        ApplyGrassWind(objectData.grassAnimation.x, objectData.grassAnimation.z,
            mul(objectData.world, float4(grassRoot, 1)).xyz, worldUp / upLength,
            max(0.0, pos.y) * upLength, worldPosition.xyz, worldNormal, worldTangent);
#ifdef GRASS_CAPSULE_INTERACTION
    if (objectData.shadowCasterFade.z <= 0.5)
        ApplyGrassFootBend(objectData.grassCapsules, (uint)objectData.grassAnimation.w,
            mul(objectData.world, float4(grassRoot, 1)).xyz, worldUp / upLength,
            max(0.0, pos.y) * upLength, restWorldPosition, restWorldNormal, restWorldTangent,
            worldPosition.xyz, worldNormal, worldTangent);
#endif
    oPos = mul(objectData.viewProjection, worldPosition);
    // Portal apertures are stencil masks rather than visible surfaces. Keep a
    // mask in front of the near plane rasterizable while the camera approaches
    // it, otherwise the connected view abruptly disappears before crossing.
    if ((draw.drawFlags & 0x40000000u) != 0u && oPos.w > 0.0)
        oPos.z = max(oPos.z, 0.0);
    // Portal depth reset draws reuse the procedural aperture geometry but
    // place it at the far plane, clearing local-scene depth only inside the
    // aperture's stencil mask before the connected view is rendered.
    if ((draw.drawFlags & 0x80000000u) != 0u)
        oPos.z = oPos.w;
    oWorldPos = worldPosition.xyz;
    oNormal = worldNormal;
    oUv = objectData.spriteUvRect.xy + localUv.xy * objectData.spriteUvRect.zw;
    oUv1 = localUv.zw;
    oUvChannels0 = float4(localUv.xy, fullUv0.zw);
    oUvChannels1 = float4(localUv.zw, fullUv1.zw);
    oUvChannels2 = fullUv2;
    oUvChannels3 = fullUv3;
    oUvChannels4 = fullUv4;
    oColor = color;
    if (((uint)objectData.materialParams.w & 16384u) != 0u)
    {
        float sunFacing = saturate(dot(oNormal,
            objectData.bakedLightDirection.xyz));
        oColor.rgb *= objectData.ambientUnlit.rgb +
            objectData.bakedDirectional.rgb * (sunFacing * 0.31830988618);
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
    out nointerpolation uint oObjectIndex : TEXCOORD11)
{
    oObjectIndex = ResolveObjectIndex(instanceId);
    ObjectData objectData = objects[oObjectIndex];
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
        oPos.z = max(oPos.z, 0.0);
    if ((draw.drawFlags & 0x80000000u) != 0u)
        oPos.z = oPos.w;
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
    return TextureLodBlend(data, worldPosition) * data.textureLodParams.z;
}

float AlphaCoverageNoise(float3 worldPosition)
{
    // A world-space subpixel pattern stays stable while the camera moves and
    // needs only integer ALU. It approximates fractional leaf coverage without
    // sending dense foliage through the sorted transparent pass.
    const uint3 cell = (uint3)floor(worldPosition * 512.0);
    uint hash = cell.x * 1664525u + cell.y * 1013904223u + cell.z * 747796405u;
    hash ^= hash >> 16u;
    hash *= 2246822519u;
    hash ^= hash >> 13u;
    return (hash & 0x00ffffffu) * (1.0 / 16777216.0);
}

void ApplyMaskedAlphaTransmission(ObjectData data, float3 worldPosition)
{
    const float transmission = saturate(data.alphaLodParams.z);
    [branch] if (transmission > 0.0)
        clip(AlphaCoverageNoise(worldPosition) - transmission);
}

void ApplyAlphaLod(inout ObjectData data, float3 worldPosition)
{
    if (data.alphaLodParams.y > 0.5 && TextureLodBlend(data, worldPosition) >= 1.0)
        data.materialParams.w = (float)((uint)data.materialParams.w & ~32u);
}

// Alpha quality is independent of color quality; zero bias preserves coverage.
// Color and shadow masks use the same distance-based alpha bias.
float4 SampleObjectBaseColor(ObjectData data, float2 sourceUv, float textureBias,
    float alphaMultiplier, float alphaBias)
{
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

float SampleDirectionalShadowCascade(ObjectData objectData,
    uint cascade, float3 biasedPosition, int pcfRadius)
{
    float4 lightClip = mul(objectData.shadowViewProjections[cascade],
        float4(biasedPosition, 1.0));
    if (lightClip.w <= 0.0)
        return 1.0;
    float3 projected = lightClip.xyz / lightClip.w;
    float2 shadowUv = float2(projected.x * 0.5 + 0.5,
        0.5 - projected.y * 0.5);
    if (projected.z <= 0.0 || projected.z >= 1.0 ||
        any(shadowUv <= 0.0) || any(shadowUv >= 1.0))
        return 1.0;

    uint width = 0, height = 0;
    shadowMap.GetDimensions(width, height);
    uint cascadeCount = clamp((uint)objectData.shadowCascadeData.x, 1u, 4u);
    uint tileWidth = cascadeCount > 1u ? width / 2u : width;
    uint tileHeight = cascadeCount > 1u ? height / 2u : height;
    int2 tileOrigin = cascadeCount > 1u
        ? int2((cascade & 1u) * tileWidth, (cascade >> 1u) * tileHeight)
        : int2(0, 0);
    int radius = clamp(pcfRadius, 0, 4);
    // Interpolate the comparison result between neighboring texels. Snapping
    // the PCF kernel to an integer center makes shadow edges visibly blocky.
    float2 texelPosition = shadowUv * float2(tileWidth, tileHeight) - 0.5;
    int2 center = tileOrigin + int2(floor(texelPosition));
    float2 texelFraction = frac(texelPosition);
    int2 tileMaximum = tileOrigin + int2(
        (int)tileWidth - 1, (int)tileHeight - 1);
    // Terrain asks for a single bilinear shadow tap. Keep that path to four
    // depth reads instead of entering the general PCF loop for every pixel.
    [branch] if (radius == 0)
    {
        float receiverDepth = projected.z - objectData.shadowParams.x;
        float4 comparisons = float4(receiverDepth.xxxx <= GatherShadowDepths(
            center, tileOrigin, tileMaximum, float2(width, height)));
        float s00 = comparisons.x, s10 = comparisons.y;
        float s01 = comparisons.z, s11 = comparisons.w;
        return lerp(lerp(s00, s10, texelFraction.x),
            lerp(s01, s11, texelFraction.x), texelFraction.y);
    }
    float receiverDepth = projected.z - objectData.shadowParams.x;
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
            float4 comparisons = float4(receiverDepth.xxxx <= GatherShadowDepths(
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
            litSamples += (receiverDepth <= storedDepth ? 1.0 : 0.0) *
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
        litSamples += (receiverDepth <= storedDepth ? 1.0 : 0.0) *
            weightX * weightY;
    }
    float diameter = 2.0 * radius + 1.0;
    return litSamples / (diameter * diameter);
}

float EvaluateDirectionalShadow(ObjectData objectData, SceneLightData light,
    float3 worldPosition, float3 surfaceNormal, float3 lightDirection,
    int pcfRadius)
{
    // Skip filtering for disabled shadows. Backends still bind a valid null
    // descriptor for empty texture slots, including this shader's atlas.
    [branch] if (light.params.z < 0.0 || light.params.w <= 0.0)
        return 1.0;

    float normalFacing = saturate(dot(surfaceNormal, lightDirection));
    float3 biasedPosition = worldPosition + surfaceNormal *
        objectData.shadowParams.y * (1.0 - normalFacing);
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

    float filtered = SampleDirectionalShadowCascade(
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
            float nextFiltered = SampleDirectionalShadowCascade(
                objectData, cascade + 1u, biasedPosition, pcfRadius);
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
void SampleTerrainHex(float2 uv, float2 uvDx, float2 uvDy, float slice,
    bool sampleNormal, out float4 albedoHeight, out float4 normalRough)
{
    const float2 st = uv * (TerrainHexTileRate * 3.46410162);
    const float2 skewed = float2(st.x - 0.57735027 * st.y, 1.15470054 * st.y);
    const float2 baseCell = floor(skewed);
    float3 temp = float3(frac(skewed), 0.0);
    temp.z = 1.0 - temp.x - temp.y;
    const float s = step(0.0, -temp.z);
    const float s2 = 2.0 * s - 1.0;
    const float3 barycentric = float3(-temp.z * s2, s - temp.y * s2, s - temp.x * s2);
    const float2 vertices[3] = {
        baseCell + float2(s, s),
        baseCell + float2(s, 1.0 - s),
        baseCell + float2(1.0 - s, s) };

    float4 a[3];
    float4 b[3];
    float2x2 rotation[3];
    [unroll] for (uint i = 0u; i < 3u; ++i)
    {
        const uint2 hash = TerrainPcg2d(asuint(int2(vertices[i])));
        const float2 offset = float2(hash.x & 0xffffu, hash.x >> 16u) / 65535.0;
        const float angle = ((hash.y & 0xffffu) / 65535.0 * 2.0 - 1.0) *
            3.14159265 * TerrainHexRotation;
        float sine, cosine;
        sincos(angle, sine, cosine);
        rotation[i] = float2x2(cosine, -sine, sine, cosine);
        // Hex centre back in UV space.
        const float2 centre = float2(vertices[i].x + 0.5 * vertices[i].y,
            0.8660254 * vertices[i].y) / (TerrainHexTileRate * 3.46410162);
        const float2 copyUv = mul(uv - centre, rotation[i]) + centre + offset;
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
    float3 weights = pow(barycentric, 7.0) *
        lerp(1.0, float3(a[0].a, a[1].a, a[2].a), TerrainHexContrast);
    weights /= max(dot(weights, 1.0), 1e-6);
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
void ApplyGeometryFade(ObjectData data, float3 worldPosition)
{
    // Screen LOD candidates were selected with per-view hysteresis. Preserve
    // appearance/range fade, but do not apply the authored distance LOD again.
    // Shadow records retain their independent distance/fixed-proxy policy.
    if ((draw.drawFlags & 0x00800000u) != 0u && data.shadowCasterFade.z < 0.5)
        data.geometryFade = float4(0, data.geometryFade.w >= 4.0 ? 1.0 :
            data.geometryFade.w >= 2.0 ? saturate(data.geometryFade.w-2.0) : data.geometryFade.y, 0, 1);
    if (data.geometryFade.w >= 2.0)
    {
        const float distance=length((data.geometryFade.w>=4.0 ? worldPosition:mul(data.world,float4(0,0,0,1)).xyz)-data.viewPositionAlphaCutoff.xyz);
        const float widthFraction=data.geometryFade.w>=4.0 ? .25 : data.geometryFade.x;
        const float width=max(0.0,min(.5,(data.geometryFade.z-data.geometryFade.y)*widthFraction));
        const float first=width>0.0 ? smoothstep(data.geometryFade.y-width,data.geometryFade.y+width,distance) : step(data.geometryFade.y,distance);
        const float second=width>0.0 ? smoothstep(data.geometryFade.z-width,data.geometryFade.z+width,distance) : step(data.geometryFade.z,distance);
        const uint lod=data.geometryFade.w>=4.0 ? (uint)data.geometryFade.x:(draw.drawFlags >> 21u) & 3u;
        const float visibility=saturate(data.geometryFade.w-2.0);
        if(lod==0) data.geometryFade=float4(0,(1-first)*visibility,0,1);
        else if(lod==1) data.geometryFade=distance<data.geometryFade.z-width ? float4((1-first)*visibility,visibility,0,1):float4(0,(1-second)*visibility,0,1);
        else data.geometryFade=float4((1-second)*visibility,visibility,0,1);
    }
    if (data.shadowCasterFade.z > 0.5 && data.shadowCasterFade.x > 0.0)
    {
        const float range=data.shadowCasterFade.x;
        const float width=max(0.001,data.shadowCasterFade.y);
        const float distance=length(worldPosition-data.viewPositionAlphaCutoff.xyz);
        const float opacity=1.0-smoothstep(max(0.0,range-width),range,distance);
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
        float threshold = frac(sin(dot(cell, float3(12.9898, 78.233, 37.719))) * 43758.5453);
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
    nointerpolation uint objectIndex : TEXCOORD11) : SV_TARGET
{
    ObjectData objectData = objects[(draw.drawFlags & 0x13080000u) != 0u ? objectIndex : draw.objectIndex];
    ApplyGeometryFade(objectData, worldPos);
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
        float alpha = objectData.baseColor.a *
            ((flags & 4096u) ? 1.0 : vertexColor.a);
        if (flags & 1u)
        {
            float2 baseUv = SelectMaterialUv(objectData.textureUvSets0.x,
                uv, uv1, uvChannels0, uvChannels1, uvChannels2,
                uvChannels3, uvChannels4);
            alpha *= baseColorMap.SampleBias(materialSampler, baseUv, TextureLodBlend(objectData, worldPos) * objectData.alphaLodParams.x).a *
                objectData.pmxTextureTint.a;
        }
        clip(alpha - objectData.viewPositionAlphaCutoff.w);
        ApplyMaskedAlphaTransmission(objectData, worldPos);
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

float4 PerformanceHeatColor(ObjectData data, float3 worldPos, float3 normal,
    bool frontFace, float alpha)
{
    // Cold-to-hot ramp: blue, cyan, green, yellow, red at quarter steps.
    // Keep in sync with the legend swatch in EditToolsViewTemplate.cpp.
    float heat = saturate(-data.morphParams.z - 1.0);
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
    float4 vertexColor : COLOR,
    nointerpolation uint objectIndex : TEXCOORD11,
    bool frontFace : SV_IsFrontFace) : SV_TARGET
{
    ObjectData objectData = objects[(draw.drawFlags & 0x13080000u) != 0u
        ? objectIndex : draw.objectIndex];
    ApplyGeometryFade(objectData, worldPos);
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
    if (flags & 1u)
        base *= SampleObjectBaseColor(objectData, uv, textureBias,
            base.a * objectData.pmxTextureTint.a,
            TextureLodBlend(objectData, worldPos) * objectData.alphaLodParams.x) *
            objectData.pmxTextureTint;
    if (flags & 32u)
    {
        clip(base.a - objectData.viewPositionAlphaCutoff.w);
        ApplyMaskedAlphaTransmission(objectData, worldPos);
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
    float3 n = normalize(frontFace ? normal : -normal);
    float3 lit = objectData.ambientUnlit.rgb;
    [loop] for (uint i = 0u; i < draw.lightCount; ++i)
    {
        SceneLightData light = sceneLights[i];
        if (abs(light.params.y - 1.0) >= 0.25 ||
            (objectData.lightingChannels.x & asuint(light.attenuation.z)) == 0u)
            continue;
        float3 direction = normalize(light.positionRange.xyz);
        float diffuse = saturate(dot(n, direction));
        if (diffuse <= 0.0) continue;
        float shadow = 1.0;
        [branch] if (objectData.shadowParams.z > 0.0)
        {
            // Retain representative shadowing (0.75) in the simplified tier
            // to avoid a large brightness jump as detailed shadowing fades.
            // Branching lets fully distant foliage skip the shadow lookup.
            shadow = shadingBlend >= 1.0 ? 0.75 :
                EvaluateDirectionalShadow(objectData, light,
                    worldPos, n, direction, 0);
            shadow = lerp(shadow, 0.75, shadingBlend);
        }
        lit += light.colorIntensity.rgb *
            (light.colorIntensity.w * diffuse * shadow * 0.31830988618);
    }
    return float4(base.rgb * lit + objectData.emissiveOcclusion.rgb,
        base.a);
}

float4 PSMain(
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
    ObjectData objectData = objects[(draw.drawFlags & 0x13080000u) != 0u ? objectIndex : draw.objectIndex];
    ApplyGeometryFade(objectData, worldPos);
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
    // Alpha-tested surfaces fall through to keep their cutout silhouettes.
    if (IsPerformanceHeatDraw(objectData) && !(flags & 32u))
        return PerformanceHeatColor(objectData, worldPos, normal, frontFace, base.a);
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
    // Distant tiles already received directional light in the vertex shader.
    // Their unlit material has no texture or per-pixel lighting work.
    if (flags & 16384u)
        return float4(base.rgb + objectData.emissiveOcclusion.rgb, base.a);
    // Height-field triangles and normals consistently face the top surface.
    float3 terrainNormalForLighting = normalize(normal);
    float terrainRoughness = 1.0;
    float terrainOcclusion = 1.0;
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
        const float macroBlend = (flags & 131072u)
            ? smoothstep(512.0, 2048.0, nearDistance) : 0.0;
        float weights[6] = {uvChannels2.x, uvChannels2.y,
            uvChannels2.z, uvChannels2.w, uvChannels3.x, uvChannels3.y};
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
            const float3 n = projection.normal;
            const float sideAmount = worldMapped
                ? saturate((0.80 - n.y) / 0.15) : 0.0;
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
    }
    // Static runtime terrain shares one global sun at every view distance.
    // Only the closest terrain receives a single-tap realtime shadow.
    if ((flags & 8192u) && objectData.ambientUnlit.w < 0.5)
    {
        float3 terrainNormal = terrainNormalForLighting;
        float3 diffuseLight = objectData.ambientUnlit.rgb * terrainOcclusion;
        float3 specularLight = 0.0;
        float3 sunDirection = objectData.bakedLightDirection.xyz;
        float sunFacing = saturate(dot(terrainNormal, sunDirection));
        if (sunFacing > 0.0)
        {
            float shadow = 1.0;
            [branch] if (objectData.bakedDirectional.w > 0.5 &&
                objectData.shadowParams.z > 0.5)
            {
                float cameraDistance = distance(worldPos,
                    objectData.viewPositionAlphaCutoff.xyz);
                [branch] if (cameraDistance < 96.0)
                {
                    SceneLightData sun = sceneLights[
                        (uint)objectData.bakedDirectional.w - 1u];
                    shadow = lerp(EvaluateDirectionalShadow(objectData, sun,
                        worldPos, terrainNormal, sunDirection, 0), 1.0,
                        smoothstep(48.0, 96.0, cameraDistance));
                }
            }
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

    float3 geometryNormal = normalize(normal) * (frontFace ? 1.0 : -1.0);
    bool hasTangents = dot(tangent.xyz, tangent.xyz) > 0.0001;
    float3 tangentDirection = hasTangents
        ? normalize(tangent.xyz - geometryNormal *
            dot(geometryNormal, tangent.xyz))
        : float3(1.0, 0.0, 0.0);
    float3 bitangentDirection =
        cross(geometryNormal, tangentDirection) * tangent.w;
    float3 viewDirection = normalize(
        objectData.viewPositionAlphaCutoff.xyz - worldPos);

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
    if (materialDetail > 0.0 && (flags & 64u) && hasTangents && objectData.parallaxParams.x > 0.0)
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

    if (flags & 1u)
        base *= SampleObjectBaseColor(objectData, baseUv, textureBias,
            base.a * objectData.pmxTextureTint.a,
            TextureLodBlend(objectData, worldPos) * objectData.alphaLodParams.x) *
            objectData.pmxTextureTint;
    // Reject empty leaf-card pixels before fetching normal, roughness or occlusion.
    if (flags & 32u)
    {
        clip(base.a - objectData.viewPositionAlphaCutoff.w);
        ApplyMaskedAlphaTransmission(objectData, worldPos);
        if (IsPerformanceHeatDraw(objectData))
            return PerformanceHeatColor(objectData, worldPos, normal, frontFace, base.a);
    }
    float subsurfaceThickness = objectData.subsurfaceRadiusThickness.w;
    if ((flags & 128u) && objectData.subsurfaceColorWeight.w > 0.0)
        subsurfaceThickness *= subsurfaceThicknessMap.SampleBias(
            materialSampler, baseUv, textureBias).r;
    [branch] if ((flags & 2u) && materialDetail > 0.0)
    {
        float4 mr = metallicRoughnessMap.SampleBias(materialSampler, metallicRoughnessUv, textureBias);
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
        occlusion = lerp(1.0, occlusionMap.SampleBias(materialSampler, occlusionUv, textureBias).r,
            objectData.emissiveOcclusion.w * materialDetail);
    if (flags & 16u)
        emissive *= emissiveMap.SampleBias(materialSampler, emissiveUv, textureBias).rgb;
    normal = (flags & 2048u) ? terrainNormalForLighting : geometryNormal;
    [branch] if ((flags & 4u) && hasTangents && materialDetail > 0.0)
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
                cookieUv = float2(dot(worldPos, light.cookieRight.xyz),
                    dot(worldPos, light.cookieUp.xyz)) /
                    max(light.photometry.z, 0.001) + 0.5;
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
            }
            attenuation *= SampleCookie(cookieIndex, cookieUv);
        }
        if (iesIndex != 0u && light.params.y > 1.5)
            attenuation *= SampleIes(iesIndex, dot(-lightDirection,
                normalize(light.directionCone.xyz)));
        float nDotL = saturate(dot(n, lightDirection));
        toonLight = max(toonLight, nDotL);
        float3 radiance = light.colorIntensity.rgb *
            light.colorIntensity.w * attenuation;
        float scatteringWeight = objectData.subsurfaceColorWeight.w * materialDetail;
        float centerShadow = 1.0;
        // Unlit-facing surfaces have no direct contribution to shadow unless
        // subsurface scattering is active. Avoid filtering an unused shadow.
        [branch] if (nDotL > 0.0 || scatteringWeight > 0.0)
            centerShadow = EvaluateDirectionalShadow(
                objectData, light, worldPos, n, lightDirection,
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
        [branch] if (nDotL > 0.0)
        directLight += EvaluatePbrBrdf(
            base.rgb, metallic, roughness, f0, n, v, lightDirection, materialDetail) *
            radiance * centerShadow * nDotL;
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
    float3 ambient = objectData.ambientUnlit.rgb *
        (base.rgb * (1.0 - metallic) + f0 * 0.25) * occlusion;
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
        environment = (diffuseEnvironment + specularEnvironment) * occlusion;
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
    bool frontFace : SV_IsFrontFace) : SV_TARGET
{
    return PSMain(pos, worldPos, normal, uv, uv1, vertexColor, tangent,
        uvChannels0, uvChannels1, uvChannels2, uvChannels3, uvChannels4,
        objectIndex, frontFace);
}

// Opaque parallax materials with parallaxDepthOffset. Depth only moves away
// from the camera, so SV_DepthGreaterEqual keeps early-Z rejection against
// geometry already in front of the flat surface.
float4 PSParallaxDepthMain(
    noperspective centroid float4 pos : SV_POSITION, float3 worldPos : TEXCOORD2,
    float3 normal : NORMAL, float2 uv : TEXCOORD, float2 uv1 : TEXCOORD1,
    float4 vertexColor : COLOR, float4 tangent : TANGENT,
    float4 uvChannels0 : TEXCOORD3, float4 uvChannels1 : TEXCOORD4,
    float4 uvChannels2 : TEXCOORD5, float4 uvChannels3 : TEXCOORD6,
    float4 uvChannels4 : TEXCOORD7,
    nointerpolation uint objectIndex : TEXCOORD11,
    bool frontFace : SV_IsFrontFace,
    out float depth : SV_DepthGreaterEqual) : SV_TARGET
{
    const float4 color = PSMain(pos, worldPos, normal, uv, uv1, vertexColor,
        tangent, uvChannels0, uvChannels1, uvChannels2, uvChannels3,
        uvChannels4, objectIndex, frontFace);
    depth = g_parallaxClipDepth >= 0.0
        ? clamp(g_parallaxClipDepth, pos.z, 1.0) : pos.z;
    return color;
}

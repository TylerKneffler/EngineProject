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

#ifdef VULKAN
[[vk::push_constant]] DrawConstants draw;
[[vk::binding(7, 0)]] StructuredBuffer<SceneLightData> sceneLights;
[[vk::binding(8, 0)]] StructuredBuffer<ObjectData> objects;
[[vk::binding(9, 0)]] StructuredBuffer<float4x4> boneMatrices;
[[vk::binding(11, 0)]] StructuredBuffer<float4> morphDeltas;
[[vk::binding(12, 0)]] StructuredBuffer<float4> morphWeights;
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
[[vk::binding(28, 0)]] Texture2D terrainNormal0;
[[vk::binding(29, 0)]] Texture2D terrainNormal1;
[[vk::binding(30, 0)]] Texture2D terrainNormal2;
[[vk::binding(31, 0)]] Texture2D terrainNormal3;
[[vk::binding(32, 0)]] Texture2D terrainNormal4;
[[vk::binding(33, 0)]] Texture2D terrainNormal5;
[[vk::binding(34, 0)]] Texture2D terrainRoughness0;
[[vk::binding(35, 0)]] Texture2D terrainRoughness1;
[[vk::binding(36, 0)]] Texture2D terrainRoughness2;
[[vk::binding(37, 0)]] Texture2D terrainRoughness3;
[[vk::binding(38, 0)]] Texture2D terrainRoughness4;
[[vk::binding(39, 0)]] Texture2D terrainRoughness5;
[[vk::binding(40, 0)]] Texture2D terrainHeight0;
[[vk::binding(41, 0)]] Texture2D terrainHeight1;
[[vk::binding(42, 0)]] Texture2D terrainHeight2;
[[vk::binding(43, 0)]] Texture2D terrainHeight3;
[[vk::binding(44, 0)]] Texture2D terrainHeight4;
[[vk::binding(45, 0)]] Texture2D terrainHeight5;
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
Texture2D terrainNormal0 : register(t27);
Texture2D terrainNormal1 : register(t28);
Texture2D terrainNormal2 : register(t29);
Texture2D terrainNormal3 : register(t30);
Texture2D terrainNormal4 : register(t31);
Texture2D terrainNormal5 : register(t32);
Texture2D terrainRoughness0 : register(t33);
Texture2D terrainRoughness1 : register(t34);
Texture2D terrainRoughness2 : register(t35);
Texture2D terrainRoughness3 : register(t36);
Texture2D terrainRoughness4 : register(t37);
Texture2D terrainRoughness5 : register(t38);
Texture2D terrainHeight0 : register(t39);
Texture2D terrainHeight1 : register(t40);
Texture2D terrainHeight2 : register(t41);
Texture2D terrainHeight3 : register(t42);
Texture2D terrainHeight4 : register(t43);
Texture2D terrainHeight5 : register(t44);
StructuredBuffer<SceneLightData> sceneLights : register(t6);
StructuredBuffer<ObjectData> objects : register(t7);
StructuredBuffer<float4x4> boneMatrices : register(t8);
StructuredBuffer<float4> morphDeltas : register(t10);
StructuredBuffer<float4> morphWeights : register(t11);
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
    out float4 oUvChannels4 : TEXCOORD7)
{
    ObjectData objectData = objects[draw.objectIndex];
    float4 localPosition = float4(pos, 1.0);
    float3 localNormal = normal;
    float3 localTangent = tangent.xyz;
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
    oNormal = normalize(mul((float3x3)objectData.world, localNormal));
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
    oTangent = float4(
        normalize(mul((float3x3)objectData.world, localTangent)), tangent.w);
}

// Compact indexed terrain input. Terrain has no skin, morph target, secondary
// UV, or authored tangent stream. Its UVs are planar X/Z coordinates, so
// reconstruct a tangent aligned with +X and project it onto the surface.
void VSTerrainMain(
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
    out float4 oUvChannels4 : TEXCOORD7)
{
    ObjectData objectData = objects[draw.objectIndex];
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
    float3 n, float3 v, float3 l)
{
    float3 h = normalize(v + l);
    float3 fresnel = FresnelSchlick(saturate(dot(h, v)), f0);
    float distribution = DistributionGGX(n, h, roughness);
    float geometry = GeometrySmith(n, v, l, roughness);
    float denominator = max(
        4.0 * saturate(dot(n, v)) * saturate(dot(n, l)), 0.0001);
    float3 specular = distribution * geometry * fresnel / denominator;
    float3 diffuseWeight = (1.0 - fresnel) * (1.0 - metallic);
    return diffuseWeight * baseColor / PI + specular;
}

float2 ParallaxOcclusionUv(
    float2 sourceUv, float3 viewDirectionTangent,
    float heightScale, float minimumSteps, float maximumSteps)
{
    float viewAngle = saturate(abs(viewDirectionTangent.z));
    float layerCount = round(lerp(maximumSteps, minimumSteps, viewAngle));
    layerCount = clamp(layerCount, 4.0, 64.0);
    float layerDepth = 1.0 / layerCount;
    float2 ray = viewDirectionTangent.xy /
        max(abs(viewDirectionTangent.z), 0.08) * heightScale;
    float2 uvStep = ray / layerCount;

    float2 currentUv = sourceUv;
    float currentLayerDepth = 0.0;
    float currentHeight = heightMap.SampleLevel(
        materialSampler, currentUv, 0.0).r;
    [loop]
    for (int step = 0; step < 64; ++step)
    {
        if (currentLayerDepth >= currentHeight || step >= (int)layerCount)
            break;
        currentUv -= uvStep;
        currentLayerDepth += layerDepth;
        currentHeight = heightMap.SampleLevel(
            materialSampler, currentUv, 0.0).r;
    }

    float2 previousUv = currentUv + uvStep;
    float afterDepth = currentHeight - currentLayerDepth;
    float previousHeight = heightMap.SampleLevel(
        materialSampler, previousUv, 0.0).r;
    float beforeDepth = previousHeight -
        (currentLayerDepth - layerDepth);
    float depthDifference = afterDepth - beforeDepth;
    float interpolation = abs(depthDifference) > 0.00001
        ? saturate(afterDepth / depthDifference) : 0.0;
    return lerp(currentUv, previousUv, interpolation);
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
        int2 p00 = clamp(center, tileOrigin, tileMaximum);
        int2 p10 = clamp(center + int2(1, 0), tileOrigin, tileMaximum);
        int2 p01 = clamp(center + int2(0, 1), tileOrigin, tileMaximum);
        int2 p11 = clamp(center + int2(1, 1), tileOrigin, tileMaximum);
        float receiverDepth = projected.z - objectData.shadowParams.x;
        float s00 = receiverDepth <= shadowMap.Load(int3(p00, 0));
        float s10 = receiverDepth <= shadowMap.Load(int3(p10, 0));
        float s01 = receiverDepth <= shadowMap.Load(int3(p01, 0));
        float s11 = receiverDepth <= shadowMap.Load(int3(p11, 0));
        return lerp(lerp(s00, s10, texelFraction.x),
            lerp(s01, s11, texelFraction.x), texelFraction.y);
    }
    float litSamples = 0.0;
    [loop] for (int y = -4; y <= 5; ++y)
    [loop] for (int x = -4; x <= 5; ++x)
    {
        if (x < -radius || x > radius + 1 ||
            y < -radius || y > radius + 1)
            continue;
        float weightX = x == -radius ? 1.0 - texelFraction.x :
            (x == radius + 1 ? texelFraction.x : 1.0);
        float weightY = y == -radius ? 1.0 - texelFraction.y :
            (y == radius + 1 ? texelFraction.y : 1.0);
        int2 coordinate = clamp(center + int2(x, y),
            tileOrigin, tileMaximum);
        float storedDepth = shadowMap.Load(int3(coordinate, 0));
        litSamples += (projected.z - objectData.shadowParams.x <= storedDepth
            ? 1.0 : 0.0) * weightX * weightY;
    }
    float diameter = 2.0 * radius + 1.0;
    return litSamples / (diameter * diameter);
}

float EvaluateDirectionalShadow(ObjectData objectData, SceneLightData light,
    float3 worldPosition, float3 surfaceNormal, float3 lightDirection,
    int pcfRadius)
{
    if (light.params.z < 0.0 || light.params.w <= 0.0)
        return 1.0;

    float normalFacing = saturate(dot(surfaceNormal, lightDirection));
    float3 biasedPosition = worldPosition + surfaceNormal *
        objectData.shadowParams.y * (1.0 - normalFacing);
    uint cascadeCount = clamp((uint)objectData.shadowCascadeData.x, 1u, 4u);
    float viewDistance = max(0.0, dot(
        worldPosition - objectData.viewPositionAlphaCutoff.xyz,
        objectData.shadowCameraData.xyz));
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
// terrain color maps without adding another texture fetch per painted layer.
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

float3 SampleTerrainColor(Texture2D colorMap, float2 uv,
    float2 alternateUv, float blendAmount)
{
    float3 color = colorMap.Sample(materialSampler, uv).rgb;
    if (blendAmount > 0.0)
        color = lerp(color,
            colorMap.Sample(materialSampler, alternateUv).rgb,
            blendAmount);
    return color;
}

float3 SampleTerrainLayerColor(uint layer, float2 uv, float2 alternateUv,
    float blendAmount, float macroBlend, float3 macroColor)
{
    float3 color;
    if (layer == 0u) color = SampleTerrainColor(baseColorMap, uv, alternateUv, blendAmount);
    else if (layer == 1u) color = SampleTerrainColor(metallicRoughnessMap, uv, alternateUv, blendAmount);
    else if (layer == 2u) color = SampleTerrainColor(normalMap, uv, alternateUv, blendAmount);
    else if (layer == 3u) color = SampleTerrainColor(occlusionMap, uv, alternateUv, blendAmount);
    else if (layer == 4u) color = SampleTerrainColor(emissiveMap, uv, alternateUv, blendAmount);
    else color = SampleTerrainColor(heightMap, uv, alternateUv, blendAmount);
    return lerp(color, macroColor, macroBlend);
}

float3 SampleTerrainLayerNormal(uint layer, float2 uv)
{
    if (layer == 0u) return terrainNormal0.Sample(materialSampler, uv).xyz;
    if (layer == 1u) return terrainNormal1.Sample(materialSampler, uv).xyz;
    if (layer == 2u) return terrainNormal2.Sample(materialSampler, uv).xyz;
    if (layer == 3u) return terrainNormal3.Sample(materialSampler, uv).xyz;
    if (layer == 4u) return terrainNormal4.Sample(materialSampler, uv).xyz;
    return terrainNormal5.Sample(materialSampler, uv).xyz;
}

float SampleTerrainLayerRoughness(uint layer, float2 uv)
{
    if (layer == 0u) return terrainRoughness0.Sample(materialSampler, uv).g;
    if (layer == 1u) return terrainRoughness1.Sample(materialSampler, uv).g;
    if (layer == 2u) return terrainRoughness2.Sample(materialSampler, uv).g;
    if (layer == 3u) return terrainRoughness3.Sample(materialSampler, uv).g;
    if (layer == 4u) return terrainRoughness4.Sample(materialSampler, uv).g;
    return terrainRoughness5.Sample(materialSampler, uv).g;
}

float SampleTerrainLayerHeight(uint layer, float2 uv)
{
    if (layer == 0u) return terrainHeight0.Sample(materialSampler, uv).r;
    if (layer == 1u) return terrainHeight1.Sample(materialSampler, uv).r;
    if (layer == 2u) return terrainHeight2.Sample(materialSampler, uv).r;
    if (layer == 3u) return terrainHeight3.Sample(materialSampler, uv).r;
    if (layer == 4u) return terrainHeight4.Sample(materialSampler, uv).r;
    return terrainHeight5.Sample(materialSampler, uv).r;
}

float4 PSShadowMain(
    float3 worldPos : TEXCOORD2,
    float2 uv : TEXCOORD,
    float2 uv1 : TEXCOORD1,
    float4 vertexColor : COLOR,
    float4 uvChannels0 : TEXCOORD3,
    float4 uvChannels1 : TEXCOORD4,
    float4 uvChannels2 : TEXCOORD5,
    float4 uvChannels3 : TEXCOORD6,
    float4 uvChannels4 : TEXCOORD7) : SV_TARGET
{
    ObjectData objectData = objects[draw.objectIndex];
    if (dot(objectData.traversalClipPlane.xyz,
        objectData.traversalClipPlane.xyz) > 0.0 &&
        dot(float4(worldPos, 1.0), objectData.traversalClipPlane) < 0.0)
        clip(-1.0);
    if (dot(objectData.portalClipPlane.xyz,
        objectData.portalClipPlane.xyz) > 0.0 &&
        dot(float4(worldPos, 1.0), objectData.portalClipPlane) < 0.0)
        clip(-1.0);
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
            alpha *= baseColorMap.Sample(materialSampler, baseUv).a *
                objectData.pmxTextureTint.a;
        }
        clip(alpha - objectData.viewPositionAlphaCutoff.w);
    }
    return 0.0;
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
    bool frontFace : SV_IsFrontFace) : SV_TARGET
{
    ObjectData objectData = objects[draw.objectIndex];
    uint flags = (uint)objectData.materialParams.w;
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
    float3 terrainNormalForLighting = normalize(normal) *
        (frontFace ? 1.0 : -1.0);
    float terrainRoughness = 1.0;
    // Authored terrain stores six painted weights in the spare UV channels.
    // Vertex color carries the contribution of layers without a texture.
    if (flags & 2048u)
    {
        float2 terrainUv = uv * max(1.0, objectData.textureUvSets1.z);
        if (objectData.textureUvSets1.x > 0.0)
            terrainUv = worldPos.xz *
                (objectData.textureUvSets1.z / objectData.textureUvSets1.x);
        const float2 terrainVariation = TerrainTextureOffset(worldPos.xz);
        const float nearDistance = distance(worldPos,
            objectData.viewPositionAlphaCutoff.xyz);
        const bool stochastic = (flags & 65536u) != 0u;
        if (!stochastic) terrainUv += terrainVariation * 0.9;
        float2 alternateUv = float2(-terrainUv.y, terrainUv.x) * 0.73 +
            float2(0.173, 0.617);
        float detailBlend = 0.5 * (1.0 - smoothstep(16.0, 48.0,
            nearDistance));
        if (stochastic)
        {
            const float2 tile = worldPos.xz / 24.0;
            const float2 local = frac(tile);
            const float edge = min(min(local.x, 1.0 - local.x),
                min(local.y, 1.0 - local.y));
            const float2 randomOffset = TerrainTextureHash(floor(tile));
            alternateUv = float2(-terrainUv.y, terrainUv.x) * 0.73 +
                randomOffset;
            detailBlend = 0.5 * smoothstep(0.0, 0.18, edge) *
                (1.0 - smoothstep(16.0, 48.0, nearDistance));
        }
        // Measured lowest-mip colors supply inexpensive macro appearance.
        const float macroBlend = (flags & 131072u)
            ? smoothstep(512.0, 2048.0, nearDistance) : 0.0;
        float weights[6] = {uvChannels2.x, uvChannels2.y,
            uvChannels2.z, uvChannels2.w, uvChannels3.x, uvChannels3.y};
        float originalWeightSum = 0.0;
        [unroll] for (uint layer = 0u; layer < 6u; ++layer)
            originalWeightSum += weights[layer];
        [branch] if (objectData.terrainMapMasks.x == 0u &&
            objectData.terrainMapMasks.y == 0u &&
            objectData.terrainMapMasks.z == 0u)
        {
            [branch] if ((flags & 131072u) == 0u)
            {
                if (weights[0] > 0.0 && (objectData.terrainMapMasks.w & 1u))
                    base.rgb += SampleTerrainColor(baseColorMap, terrainUv,
                        alternateUv, detailBlend) * weights[0];
                if (weights[1] > 0.0 && (objectData.terrainMapMasks.w & 2u))
                    base.rgb += SampleTerrainColor(metallicRoughnessMap,
                        terrainUv, alternateUv, detailBlend) * weights[1];
                if (weights[2] > 0.0 && (objectData.terrainMapMasks.w & 4u))
                    base.rgb += SampleTerrainColor(normalMap, terrainUv,
                        alternateUv, detailBlend) * weights[2];
                if (weights[3] > 0.0 && (objectData.terrainMapMasks.w & 8u))
                    base.rgb += SampleTerrainColor(occlusionMap, terrainUv,
                        alternateUv, detailBlend) * weights[3];
                if (weights[4] > 0.0 && (objectData.terrainMapMasks.w & 16u))
                    base.rgb += SampleTerrainColor(emissiveMap, terrainUv,
                        alternateUv, detailBlend) * weights[4];
                if (weights[5] > 0.0 && (objectData.terrainMapMasks.w & 32u))
                    base.rgb += SampleTerrainColor(heightMap, terrainUv,
                        alternateUv, detailBlend) * weights[5];
            }
            else
            {
                [branch] if (macroBlend >= 0.999)
                {
                    [unroll] for (uint layer = 0u; layer < 6u; ++layer)
                        if (objectData.terrainMapMasks.w & (1u << layer))
                            base.rgb += objectData.terrainMacroColors[layer].rgb *
                                weights[layer];
                }
                else
                {
                    if (weights[0] > 0.0 && (objectData.terrainMapMasks.w & 1u))
                        base.rgb += lerp(SampleTerrainColor(baseColorMap,
                            terrainUv, alternateUv, detailBlend),
                            objectData.terrainMacroColors[0].rgb, macroBlend) * weights[0];
                    if (weights[1] > 0.0 && (objectData.terrainMapMasks.w & 2u))
                        base.rgb += lerp(SampleTerrainColor(metallicRoughnessMap,
                            terrainUv, alternateUv, detailBlend),
                            objectData.terrainMacroColors[1].rgb, macroBlend) * weights[1];
                    if (weights[2] > 0.0 && (objectData.terrainMapMasks.w & 4u))
                        base.rgb += lerp(SampleTerrainColor(normalMap,
                            terrainUv, alternateUv, detailBlend),
                            objectData.terrainMacroColors[2].rgb, macroBlend) * weights[2];
                    if (weights[3] > 0.0 && (objectData.terrainMapMasks.w & 8u))
                        base.rgb += lerp(SampleTerrainColor(occlusionMap,
                            terrainUv, alternateUv, detailBlend),
                            objectData.terrainMacroColors[3].rgb, macroBlend) * weights[3];
                    if (weights[4] > 0.0 && (objectData.terrainMapMasks.w & 16u))
                        base.rgb += lerp(SampleTerrainColor(emissiveMap,
                            terrainUv, alternateUv, detailBlend),
                            objectData.terrainMacroColors[4].rgb, macroBlend) * weights[4];
                    if (weights[5] > 0.0 && (objectData.terrainMapMasks.w & 32u))
                        base.rgb += lerp(SampleTerrainColor(heightMap,
                            terrainUv, alternateUv, detailBlend),
                            objectData.terrainMacroColors[5].rgb, macroBlend) * weights[5];
                }
            }
            if (originalWeightSum > 0.00001)
            {
                float roughnessSum = 0.0;
                [unroll] for (uint layer = 0u; layer < 6u; ++layer)
                    roughnessSum += weights[layer] *
                        objectData.terrainLayerParams[layer].x;
                terrainRoughness = clamp(roughnessSum / originalWeightSum,
                    0.045, 1.0);
            }
        }
        else if (originalWeightSum > 0.00001)
        {
            [unroll] for (uint layer = 0u; layer < 6u; ++layer)
                weights[layer] /= originalWeightSum;
            float heightWeightSum = 0.0;
            [unroll] for (uint layer = 0u; layer < 6u; ++layer)
            {
                if (weights[layer] > 0.0 &&
                    (objectData.terrainMapMasks.z & (1u << layer)))
                {
                    const float height = SampleTerrainLayerHeight(layer,
                        terrainUv);
                    weights[layer] = max(0.0, weights[layer] +
                        objectData.terrainLayerParams[layer].z *
                        (height - 0.5));
                }
                heightWeightSum += weights[layer];
            }
            if (heightWeightSum <= 0.00001)
            {
                weights[0] = uvChannels2.x / originalWeightSum;
                weights[1] = uvChannels2.y / originalWeightSum;
                weights[2] = uvChannels2.z / originalWeightSum;
                weights[3] = uvChannels2.w / originalWeightSum;
                weights[4] = uvChannels3.x / originalWeightSum;
                weights[5] = uvChannels3.y / originalWeightSum;
                heightWeightSum = 1.0;
            }
            if (heightWeightSum > 0.00001)
            {
                float3 tangentNormal = float3(0.0, 0.0, 0.0);
                float roughnessSum = 0.0;
                [unroll] for (uint layer = 0u; layer < 6u; ++layer)
                {
                    const float weight = weights[layer] / heightWeightSum;
                    if (weight <= 0.0) continue;
                    if (objectData.terrainMapMasks.w & (1u << layer))
                        base.rgb += SampleTerrainLayerColor(layer, terrainUv,
                            alternateUv, detailBlend, macroBlend,
                            objectData.terrainMacroColors[layer].rgb) *
                            (weight * originalWeightSum);
                    float layerRoughness =
                        objectData.terrainLayerParams[layer].x;
                    if (objectData.terrainMapMasks.y & (1u << layer))
                        layerRoughness *= SampleTerrainLayerRoughness(layer,
                            terrainUv);
                    roughnessSum += layerRoughness * weight;
                    float3 layerNormal = float3(0.0, 0.0, 1.0);
                    if (objectData.terrainMapMasks.x & (1u << layer))
                    {
                        layerNormal = SampleTerrainLayerNormal(layer,
                            terrainUv) * 2.0 - 1.0;
                        layerNormal.xy *=
                            objectData.terrainLayerParams[layer].y;
                    }
                    tangentNormal += normalize(layerNormal) * weight;
                }
                terrainRoughness = clamp(roughnessSum, 0.045, 1.0);
                if (objectData.terrainMapMasks.x != 0u)
                {
                    const float3 terrainTangent = normalize(tangent.xyz);
                    const float3 terrainBitangent = normalize(cross(
                        terrainNormalForLighting, terrainTangent)) * tangent.w;
                    terrainNormalForLighting = normalize(
                        terrainTangent * tangentNormal.x +
                        terrainBitangent * tangentNormal.y +
                        terrainNormalForLighting * tangentNormal.z);
                }
            }
        }
        if (!stochastic)
            base.rgb *= 1.0 + (terrainVariation.x + terrainVariation.y) * 0.10;
    }
    // Static runtime terrain shares one global sun at every view distance.
    // Only the closest terrain receives a single-tap realtime shadow.
    if ((flags & 8192u) && objectData.ambientUnlit.w < 0.5)
    {
        float3 terrainNormal = terrainNormalForLighting;
        float3 diffuseLight = objectData.ambientUnlit.rgb;
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
        }
        return float4(base.rgb * diffuseLight + objectData.emissiveOcclusion.rgb,
            base.a);
    }
    float metallic = saturate(objectData.materialParams.x);
    float roughness = (flags & 2048u) ? terrainRoughness :
        clamp(objectData.materialParams.y, 0.045, 1.0);
    float occlusion = 1.0;
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

    if ((flags & 64u) && hasTangents && objectData.parallaxParams.x > 0.0)
    {
        float3 viewDirectionTangent = float3(
            dot(viewDirection, tangentDirection),
            dot(viewDirection, bitangentDirection),
            dot(viewDirection, geometryNormal));
        float2 parallaxUv = ParallaxOcclusionUv(
            heightUv, viewDirectionTangent, objectData.parallaxParams.x,
            objectData.parallaxParams.y, objectData.parallaxParams.z);
        float2 parallaxOffset = parallaxUv - heightUv;
        baseUv += parallaxOffset; metallicRoughnessUv += parallaxOffset;
        normalUv += parallaxOffset; occlusionUv += parallaxOffset;
        emissiveUv += parallaxOffset;
    }

    if (flags & 1u)
        base *= baseColorMap.Sample(materialSampler, baseUv) *
            objectData.pmxTextureTint;
    float subsurfaceThickness = objectData.subsurfaceRadiusThickness.w;
    if ((flags & 128u) && objectData.subsurfaceColorWeight.w > 0.0)
        subsurfaceThickness *= subsurfaceThicknessMap.Sample(
            materialSampler, baseUv).r;
    if (flags & 2u)
    {
        float4 mr = metallicRoughnessMap.Sample(materialSampler, metallicRoughnessUv);
        roughness *= mr.g;
        metallic *= mr.b;
    }
    roughness = clamp(roughness, 0.045, 1.0);
    metallic = saturate(metallic);
    const bool blinnPhongSpecular = objectData.blinnPhongSpecularShininess.w > 0.0;
    if (blinnPhongSpecular)
        roughness = clamp(sqrt(2.0 /
            (objectData.blinnPhongSpecularShininess.w + 1.0)), 0.045, 1.0);
    if (flags & 8u)
        occlusion = lerp(1.0, occlusionMap.Sample(materialSampler, occlusionUv).r,
            objectData.emissiveOcclusion.w);
    if (flags & 16u)
        emissive *= emissiveMap.Sample(materialSampler, emissiveUv).rgb;
    normal = (flags & 2048u) ? terrainNormalForLighting : geometryNormal;
    if ((flags & 4u) && hasTangents)
    {
        float3 sampledNormal = normalMap.Sample(materialSampler, normalUv).xyz * 2.0 - 1.0;
        sampledNormal.xy *= objectData.materialParams.z;
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
        float4 sphere = sphereMap.Sample(materialSampler, sphereUv) *
            objectData.pmxSphereTint;
        if (objectData.pmxParams.y > 1.5 && objectData.pmxParams.y < 2.5)
            base.rgb += sphere.rgb * sphere.a;
        else
            base.rgb *= lerp(float3(1.0, 1.0, 1.0), sphere.rgb,
                saturate(sphere.a));
    }

    if (flags & 32u)
        clip(base.a - objectData.viewPositionAlphaCutoff.w);

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
        float centerShadow = EvaluateDirectionalShadow(
            objectData, light, worldPos, n, lightDirection,
            (int)objectData.shadowParams.w);
        directLight += EvaluatePbrBrdf(
            base.rgb, metallic, roughness, f0, n, v, lightDirection) *
            radiance * centerShadow * nDotL;
        float scatteringWeight = objectData.subsurfaceColorWeight.w;
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
            base.rgb, metallic, roughness, f0, n, v, bakedDirection) *
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
    return float4(litColor + emissive, base.a);
}

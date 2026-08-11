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
    float4x4 mvp;
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
};

struct SceneLightData
{
    float4 positionRange;
    float4 colorIntensity;
    float4 params;
};

#ifdef VULKAN
[[vk::push_constant]] DrawConstants draw;
[[vk::binding(7, 0)]] StructuredBuffer<SceneLightData> sceneLights;
[[vk::binding(8, 0)]] StructuredBuffer<ObjectData> objects;
[[vk::binding(9, 0)]] StructuredBuffer<float4x4> boneMatrices;
[[vk::binding(0, 0)]] Texture2D baseColorMap;
[[vk::binding(1, 0)]] Texture2D metallicRoughnessMap;
[[vk::binding(2, 0)]] Texture2D normalMap;
[[vk::binding(3, 0)]] Texture2D occlusionMap;
[[vk::binding(4, 0)]] Texture2D emissiveMap;
[[vk::binding(5, 0)]] Texture2D heightMap;
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
StructuredBuffer<SceneLightData> sceneLights : register(t6);
StructuredBuffer<ObjectData> objects : register(t7);
StructuredBuffer<float4x4> boneMatrices : register(t8);
SamplerState materialSampler : register(s0);
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
    out float4 oPos : SV_POSITION,
    out float3 oWorldPos : TEXCOORD2,
    out float3 oNormal : NORMAL,
    out float2 oUv : TEXCOORD,
    out float2 oUv1 : TEXCOORD1,
    out float4 oColor : COLOR,
    out float4 oTangent : TANGENT)
{
    ObjectData objectData = objects[draw.objectIndex];
    float4 localPosition = float4(pos, 1.0);
    float3 localNormal = normal;
    float3 localTangent = tangent.xyz;
    uint jointCount = (uint)objectData.skinParams.y;
    if (jointCount > 0)
    {
        uint paletteOffset = (uint)objectData.skinParams.x;
        float4x4 skin = (float4x4)0;
        [unroll] for (uint influence = 0; influence < 4; ++influence)
        {
            uint joint0 = (uint)joints0[influence];
            uint joint1 = (uint)joints1[influence];
            if (joint0 < jointCount) skin += boneMatrices[paletteOffset + joint0] * weights0[influence];
            if (joint1 < jointCount) skin += boneMatrices[paletteOffset + joint1] * weights1[influence];
        }
        localPosition = mul(skin, localPosition);
        localNormal = normalize(mul((float3x3)skin, localNormal));
        if (dot(localTangent, localTangent) > 0.0001)
            localTangent = normalize(mul((float3x3)skin, localTangent));
    }
    oPos = mul(objectData.mvp, localPosition);
    oWorldPos = mul(objectData.world, localPosition).xyz;
    oNormal = normalize(mul((float3x3)objectData.world, localNormal));
    oUv = objectData.spriteUvRect.xy + uv * objectData.spriteUvRect.zw;
    oUv1 = uv1;
    oColor = color;
    oTangent = float4(
        normalize(mul((float3x3)objectData.world, localTangent)), tangent.w);
}

static const float PI = 3.14159265359;

float3 LinearToSrgb(float3 linearColor)
{
    linearColor = max(linearColor, 0.0);
    float3 lower = linearColor * 12.92;
    float3 upper = 1.055 * pow(linearColor, 1.0 / 2.4) - 0.055;
    return lerp(lower, upper, step(0.0031308, linearColor));
}

float4 EncodeOutput(float3 linearColor, float alpha)
{
    // Scene and game color targets are UNORM on every backend. Material
    // textures are sampled as sRGB and lighting is evaluated in linear space,
    // so encode once here before the target is displayed or presented.
    return float4(LinearToSrgb(linearColor), alpha);
}

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

float3 EvaluatePbrBrdf(
    float3 baseColor, float metallic, float roughness,
    float3 n, float3 v, float3 l)
{
    float3 h = normalize(v + l);
    float3 f0 = lerp(float3(0.04, 0.04, 0.04), baseColor, metallic);
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

float4 PSMain(
    float4 pos : SV_POSITION,
    float3 worldPos : TEXCOORD2,
    float3 normal : NORMAL,
    float2 uv : TEXCOORD,
    float2 uv1 : TEXCOORD1,
    float4 vertexColor : COLOR,
    float4 tangent : TANGENT,
    bool frontFace : SV_IsFrontFace) : SV_TARGET
{
    ObjectData objectData = objects[draw.objectIndex];
    uint flags = (uint)objectData.materialParams.w;
    float4 base = objectData.baseColor * vertexColor;
    float metallic = saturate(objectData.materialParams.x);
    float roughness = clamp(objectData.materialParams.y, 0.045, 1.0);
    float occlusion = 1.0;
    float3 emissive = objectData.emissiveOcclusion.rgb;
    float2 baseUv = objectData.textureUvSets0.x > 0.5 ? uv1 : uv;
    float2 metallicRoughnessUv = objectData.textureUvSets0.y > 0.5 ? uv1 : uv;
    float2 normalUv = objectData.textureUvSets0.z > 0.5 ? uv1 : uv;
    float2 occlusionUv = objectData.textureUvSets0.w > 0.5 ? uv1 : uv;
    float2 emissiveUv = objectData.textureUvSets1.x > 0.5 ? uv1 : uv;
    float2 heightUv = objectData.textureUvSets1.y > 0.5 ? uv1 : uv;

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
        base *= baseColorMap.Sample(materialSampler, baseUv);
    if (flags & 2u)
    {
        float4 mr = metallicRoughnessMap.Sample(materialSampler, metallicRoughnessUv);
        roughness *= mr.g;
        metallic *= mr.b;
    }
    roughness = clamp(roughness, 0.045, 1.0);
    metallic = saturate(metallic);
    if (flags & 8u)
        occlusion = lerp(1.0, occlusionMap.Sample(materialSampler, occlusionUv).r,
            objectData.emissiveOcclusion.w);
    if (flags & 16u)
        emissive *= emissiveMap.Sample(materialSampler, emissiveUv).rgb;
    normal = geometryNormal;
    if ((flags & 4u) && hasTangents)
    {
        float3 sampledNormal = normalMap.Sample(materialSampler, normalUv).xyz * 2.0 - 1.0;
        sampledNormal.xy *= objectData.materialParams.z;
        normal = normalize(
            sampledNormal.x * tangentDirection +
            sampledNormal.y * bitangentDirection +
            sampledNormal.z * geometryNormal);
    }

    if (flags & 32u)
        clip(base.a - objectData.viewPositionAlphaCutoff.w);

    if (objectData.ambientUnlit.w > 0.5)
        return EncodeOutput(base.rgb + emissive, base.a);

    float3 n = normalize(normal);
    float3 v = viewDirection;
    float3 directLight = 0.0;
    [loop]
    for (uint index = 0; index < draw.lightCount; ++index)
    {
        SceneLightData light = sceneLights[index];
        float3 lightDirection;
        float attenuation = 1.0;
        if (light.params.y > 0.5)
        {
            lightDirection = normalize(light.positionRange.xyz);
        }
        else
        {
            float3 toLight = light.positionRange.xyz - worldPos;
            float distanceToLight = length(toLight);
            lightDirection = distanceToLight > 0.0001
                ? toLight / distanceToLight : float3(0.0, 1.0, 0.0);
            float rangeAttenuation = saturate(
                1.0 - distanceToLight / max(light.positionRange.w, 0.0001));
            attenuation = pow(
                rangeAttenuation, max(light.params.x, 0.1));
        }
        float nDotL = saturate(dot(n, lightDirection));
        float3 radiance = light.colorIntensity.rgb *
            light.colorIntensity.w * attenuation;
        directLight += EvaluatePbrBrdf(
            base.rgb, metallic, roughness, n, v, lightDirection) *
            radiance * nDotL;
    }

    float3 bakedDirect = 0.0;
    if (dot(objectData.bakedDirectional.rgb,
        objectData.bakedDirectional.rgb) > 0.000001)
    {
        float3 bakedDirection = normalize(objectData.bakedLightDirection.xyz);
        bakedDirect = EvaluatePbrBrdf(
            base.rgb, metallic, roughness, n, v, bakedDirection) *
            objectData.bakedDirectional.rgb * saturate(dot(n, bakedDirection));
    }
    float3 f0 = lerp(float3(0.04, 0.04, 0.04), base.rgb, metallic);
    float3 ambient = objectData.ambientUnlit.rgb *
        (base.rgb * (1.0 - metallic) + f0 * 0.25) * occlusion;
    return EncodeOutput(
        ambient + directLight + bakedDirect + emissive, base.a);
}

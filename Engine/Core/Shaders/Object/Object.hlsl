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
    float4 specularShininess;
};

struct PointLightData
{
    float4 positionRange;
    float4 colorIntensity;
    float4 params;
};

#ifdef VULKAN
[[vk::push_constant]] DrawConstants draw;
[[vk::binding(6, 0)]] StructuredBuffer<PointLightData> sceneLights;
[[vk::binding(7, 0)]] StructuredBuffer<ObjectData> objects;
[[vk::binding(0, 0)]] Texture2D baseColorMap;
[[vk::binding(1, 0)]] Texture2D metallicRoughnessMap;
[[vk::binding(2, 0)]] Texture2D normalMap;
[[vk::binding(3, 0)]] Texture2D occlusionMap;
[[vk::binding(4, 0)]] Texture2D emissiveMap;
[[vk::binding(5, 0)]] SamplerState materialSampler;
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
StructuredBuffer<PointLightData> sceneLights : register(t5);
StructuredBuffer<ObjectData> objects : register(t6);
SamplerState materialSampler : register(s0);
#endif

void VSMain(
    float3 pos : POSITION,
    float3 normal : NORMAL,
    float2 uv : TEXCOORD,
    float4 tangent : TANGENT,
    out float4 oPos : SV_POSITION,
    out float3 oWorldPos : TEXCOORD1,
    out float3 oNormal : NORMAL,
    out float2 oUv : TEXCOORD,
    out float4 oTangent : TANGENT)
{
    ObjectData objectData = objects[draw.objectIndex];
    oPos = mul(objectData.mvp, float4(pos, 1.0));
    oWorldPos = mul(objectData.world, float4(pos, 1.0)).xyz;
    oNormal = normalize(mul((float3x3)objectData.world, normal));
    oUv = uv;
    oTangent = float4(
        normalize(mul((float3x3)objectData.world, tangent.xyz)), tangent.w);
}

float4 PSMain(
    float4 pos : SV_POSITION,
    float3 worldPos : TEXCOORD1,
    float3 normal : NORMAL,
    float2 uv : TEXCOORD,
    float4 tangent : TANGENT) : SV_TARGET
{
    ObjectData objectData = objects[draw.objectIndex];
    uint flags = (uint)objectData.materialParams.w;
    float4 base = objectData.baseColor;
    float metallic = objectData.materialParams.x;
    float roughness = objectData.materialParams.y;
    float occlusion = 1.0;
    float3 emissive = objectData.emissiveOcclusion.rgb;

    if (flags & 1u)
        base *= baseColorMap.Sample(materialSampler, uv);
    if (flags & 2u)
    {
        float4 mr = metallicRoughnessMap.Sample(materialSampler, uv);
        roughness *= mr.g;
        metallic *= mr.b;
    }
    if (flags & 8u)
        occlusion = lerp(1.0, occlusionMap.Sample(materialSampler, uv).r,
            objectData.emissiveOcclusion.w);
    if (flags & 16u)
        emissive *= emissiveMap.Sample(materialSampler, uv).rgb;
    if ((flags & 4u) && dot(tangent.xyz, tangent.xyz) > 0.0001)
    {
        float3 n = normalize(normal);
        float3 t = normalize(tangent.xyz - n * dot(n, tangent.xyz));
        float3 b = cross(n, t) * tangent.w;
        float3 sampledNormal = normalMap.Sample(materialSampler, uv).xyz * 2.0 - 1.0;
        sampledNormal.xy *= objectData.materialParams.z;
        normal = normalize(
            sampledNormal.x * t + sampledNormal.y * b + sampledNormal.z * n);
    }

    if (objectData.ambientUnlit.w > 0.5)
        return float4(base.rgb + emissive, base.a);

    float3 directLight = 0.0;
    [loop]
    for (uint index = 0; index < draw.lightCount; ++index)
    {
        PointLightData light = sceneLights[index];
        float3 toLight = light.positionRange.xyz - worldPos;
        float distanceToLight = length(toLight);
        float3 lightDir = distanceToLight > 0.0001
            ? toLight / distanceToLight : float3(0.0, 1.0, 0.0);
        float rangeAttenuation = saturate(
            1.0 - distanceToLight / max(light.positionRange.w, 0.0001));
        float attenuation = pow(rangeAttenuation, max(light.params.x, 0.1));
        float diffuse = saturate(dot(normalize(normal), lightDir));
        directLight += light.colorIntensity.rgb *
            light.colorIntensity.w * attenuation * diffuse;
    }

    float3 litBase = base.rgb *
        (objectData.ambientUnlit.rgb + directLight * (1.0 - metallic));
    return float4(litBase * occlusion + emissive, base.a);
}

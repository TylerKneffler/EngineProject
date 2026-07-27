// Textured glTF-compatible material shader.
// The 128-byte constant layout also fits Vulkan's guaranteed push-constant size.

#ifdef VULKAN
struct ObjectConst
{
    float4x4 mvp;
    float4 diffuseColor;
    float4 ambientColor;
    float4 specularColor;  // xyz emissive factor, w occlusion strength
    float4 materialParams; // metallic, roughness, normal scale, texture bit mask
};
[[vk::push_constant]] ObjectConst cb;
#define mvp            cb.mvp
#define diffuseColor   cb.diffuseColor
#define ambientColor   cb.ambientColor
#define specularColor  cb.specularColor
#define materialParams cb.materialParams

[[vk::binding(0, 0)]] Texture2D baseColorMap;
[[vk::binding(1, 0)]] Texture2D metallicRoughnessMap;
[[vk::binding(2, 0)]] Texture2D normalMap;
[[vk::binding(3, 0)]] Texture2D occlusionMap;
[[vk::binding(4, 0)]] Texture2D emissiveMap;
[[vk::binding(5, 0)]] SamplerState materialSampler;
#else
cbuffer Constants : register(b0)
{
    float4x4 mvp;
    float4 diffuseColor;
    float4 ambientColor;
    float4 specularColor;
    float4 materialParams;
};

Texture2D baseColorMap         : register(t0);
Texture2D metallicRoughnessMap : register(t1);
Texture2D normalMap            : register(t2);
Texture2D occlusionMap         : register(t3);
Texture2D emissiveMap          : register(t4);
SamplerState materialSampler   : register(s0);
#endif

void VSMain(
    float3 pos : POSITION,
    float3 normal : NORMAL,
    float2 uv : TEXCOORD,
    float4 tangent : TANGENT,
    out float4 oPos : SV_POSITION,
    out float3 oNormal : NORMAL,
    out float2 oUv : TEXCOORD,
    out float4 oTangent : TANGENT)
{
    oPos = mul(mvp, float4(pos, 1.0));
    oNormal = normal;
    oUv = uv;
    oTangent = tangent;
}

float4 PSMain(
    float4 pos : SV_POSITION,
    float3 normal : NORMAL,
    float2 uv : TEXCOORD,
    float4 tangent : TANGENT) : SV_TARGET
{
    uint flags = (uint)materialParams.w;
    float4 base = diffuseColor;
    float metallic = materialParams.x;
    float roughness = materialParams.y;
    float occlusion = 1.0;
    float3 emissive = specularColor.rgb;

    if (flags & 1u)
        base *= baseColorMap.Sample(materialSampler, uv);
    if (flags & 2u)
    {
        float4 mr = metallicRoughnessMap.Sample(materialSampler, uv);
        roughness *= mr.g;
        metallic *= mr.b;
    }
    if (flags & 8u)
        occlusion = lerp(
            1.0, occlusionMap.Sample(materialSampler, uv).r, specularColor.w);
    if (flags & 16u)
        emissive *= emissiveMap.Sample(materialSampler, uv).rgb;
    if ((flags & 4u) && dot(tangent.xyz, tangent.xyz) > 0.0001)
    {
        float3 n = normalize(normal);
        float3 t = normalize(tangent.xyz - n * dot(n, tangent.xyz));
        float3 b = cross(n, t) * tangent.w;
        float3 sampledNormal =
            normalMap.Sample(materialSampler, uv).xyz * 2.0 - 1.0;
        sampledNormal.xy *= materialParams.z;
        normal = normalize(
            sampledNormal.x * t + sampledNormal.y * b + sampledNormal.z * n);
    }

    float3 lightDir = normalize(float3(1.0, 2.0, -1.0));
    float diffuse = saturate(dot(normalize(normal), lightDir));
    float3 litBase =
        base.rgb * (ambientColor.rgb + diffuse * (1.0 - metallic));
    float3 color = litBase * occlusion + emissive;
    return float4(color, base.a);
}

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

#ifdef VULKAN
[[vk::push_constant]] DrawConstants draw;
[[vk::binding(7, 0)]] StructuredBuffer<ObjectData> objects;
#else
cbuffer DrawBuffer : register(b0) { DrawConstants draw; };
StructuredBuffer<ObjectData> objects : register(t6);
#endif

void VSMain(
    float3 pos : POSITION,
    float3 normal : NORMAL,
    float2 uv : TEXCOORD,
    float4 tangent : TANGENT,
    out float4 oPos : SV_POSITION)
{
    oPos = mul(objects[draw.objectIndex].mvp, float4(pos * 1.03, 1.0));
}

float4 PSMain(float4 pos : SV_POSITION) : SV_TARGET
{
    return float4(1.0, 0.85, 0.1, 1.0);
}

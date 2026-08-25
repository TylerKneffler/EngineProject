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

#ifdef VULKAN
[[vk::push_constant]] DrawConstants draw;
[[vk::binding(8, 0)]] StructuredBuffer<ObjectData> objects;
[[vk::binding(9, 0)]] StructuredBuffer<float4x4> boneMatrices;
#else
cbuffer DrawBuffer : register(b0) { DrawConstants draw; };
StructuredBuffer<ObjectData> objects : register(t7);
StructuredBuffer<float4x4> boneMatrices : register(t8);
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
    out float4 oPos : SV_POSITION)
{
    ObjectData objectData = objects[draw.objectIndex];
    float4 localPosition = float4(pos, 1.0);
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
    }
    localPosition.xyz *= 1.03;
    oPos = mul(objectData.mvp, localPosition);
}

float4 PSMain(float4 pos : SV_POSITION) : SV_TARGET
{
    return float4(1.0, 0.85, 0.1, 1.0);
}

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
    float4 morphParams;
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
};

#ifdef VULKAN
[[vk::push_constant]] DrawConstants draw;
[[vk::binding(8, 0)]] StructuredBuffer<ObjectData> objects;
[[vk::binding(9, 0)]] StructuredBuffer<float4x4> boneMatrices;
[[vk::binding(11, 0)]] StructuredBuffer<float4> morphDeltas;
[[vk::binding(12, 0)]] StructuredBuffer<float4> morphWeights;
#else
cbuffer DrawBuffer : register(b0) { DrawConstants draw; };
StructuredBuffer<ObjectData> objects : register(t7);
StructuredBuffer<float4x4> boneMatrices : register(t8);
StructuredBuffer<float4> morphDeltas : register(t10);
StructuredBuffer<float4> morphWeights : register(t11);
#endif

#include "../SkinDeformation.hlsli"

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
    out float4 oPos : SV_POSITION)
{
    ObjectData objectData = objects[draw.objectIndex];
    float4 localPosition = float4(pos, 1.0);
    uint activeMorphCount = (uint)objectData.morphParams.x;
    uint morphVertexCount = (uint)objectData.morphParams.y;
    uint morphStride = (uint)objectData.morphParams.w;
    [loop] for (uint active = 0; active < activeMorphCount; ++active)
    {
        float2 morph = morphWeights[active].xy;
        uint target = (uint)morph.x;
        localPosition.xyz += morphDeltas[
            (target * morphVertexCount + vertexId) * morphStride].xyz * morph.y;
    }
    uint jointCount = (uint)objectData.skinParams.y;
    if (jointCount > 0)
    {
        uint paletteOffset = (uint)objectData.skinParams.x;
        float3 skinNormal = normal, skinTangent = tangent.xyz;
        SkinDeform(localPosition, skinNormal, skinTangent,
            joints0, weights0, joints1, weights1, sdefCenterAndMode,
            sdefRadius0, sdefRadius1, paletteOffset, jointCount);
    }
    localPosition.xyz *= 1.03;
    const float4 worldPosition = mul(objectData.world, localPosition);
    oPos = mul(objectData.viewProjection, worldPosition);
}

void VSTerrainMain(
    float3 pos : POSITION,
    float3 normal : NORMAL,
    float2 uv : TEXCOORD,
    float4 color : COLOR,
    out float4 oPos : SV_POSITION)
{
    ObjectData objectData = objects[draw.objectIndex];
    float4 localPosition = float4(pos * 1.03, 1.0);
    const float4 worldPosition = mul(objectData.world, localPosition);
    oPos = mul(objectData.viewProjection, worldPosition);
}

float4 PSMain(float4 pos : SV_POSITION) : SV_TARGET
{
    return float4(1.0, 0.85, 0.1, 1.0);
}

void VSMmdMain(
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
    out float4 oPos : SV_POSITION)
{
    ObjectData objectData = objects[draw.objectIndex];
    float4 localPosition = float4(pos, 1.0);
    float3 outlineNormal = normal;
    uint activeMorphCount = (uint)objectData.morphParams.x;
    uint morphVertexCount = (uint)objectData.morphParams.y;
    uint morphStride = (uint)objectData.morphParams.w;
    [loop] for (uint active = 0; active < activeMorphCount; ++active)
    {
        float2 morph = morphWeights[active].xy;
        uint target = (uint)morph.x;
        uint base = (target * morphVertexCount + vertexId) * morphStride;
        localPosition.xyz += morphDeltas[base].xyz * morph.y;
        outlineNormal += morphDeltas[base + 1].xyz * morph.y;
    }
    uint jointCount = (uint)objectData.skinParams.y;
    if (jointCount > 0)
    {
        uint paletteOffset = (uint)objectData.skinParams.x;
        float3 skinTangent = tangent.xyz;
        SkinDeform(localPosition, outlineNormal, skinTangent,
            joints0, weights0, joints1, weights1, sdefCenterAndMode,
            sdefRadius0, sdefRadius1, paletteOffset, jointCount);
    }
    localPosition.xyz += normalize(outlineNormal) *
        max(objectData.pmxParams.x, 0.0) * 0.01;
    oPos = mul(objectData.viewProjection,
        mul(objectData.world, localPosition));
}

float4 PSMmdMain(float4 pos : SV_POSITION) : SV_TARGET
{
    return objects[draw.objectIndex].pmxEdgeColor;
}

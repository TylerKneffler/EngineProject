// Opt-in forest diagnostic only. Same color vertex path and alpha/depth coverage,
// without material lighting. White makes activation and cutouts verifiable.
#define PSMain PSObjectMain
#include "../../../Shaders/Object/Object.hlsl"
#undef PSMain

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
    PSShadowMain(pos, worldPos, normal, uv, uv1, vertexColor, tangent,
        uvChannels0, uvChannels1, uvChannels2, uvChannels3, uvChannels4,
        objectIndex);
    // Saturate the diagnostic mask through DX11's display tone mapping too;
    // explicit-backend scene readbacks clamp HDR directly to display white.
    return float4(65504, 65504, 65504, 1);
}

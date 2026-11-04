// Material shader asset: the normal object vertex path preserves instancing,
// GPU LOD, root-pinned grass wind and dithered fades. The foliage color pass is simplified.
#define PSMain PSObjectMain
#include "../../Shaders/Object/Object.hlsl"
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
    return PSFoliageMain(pos, worldPos, normal, uv, vertexColor,
        objectIndex, frontFace);
}

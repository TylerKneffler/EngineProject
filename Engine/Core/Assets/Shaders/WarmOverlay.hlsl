// Retain the engine mesh inputs, skinning, terrain, texture bindings and clipping.
#define PSMain EngineSurfacePS
#include "../../Shaders/Object/Object.hlsl"
#undef PSMain

float4 PSMain(
    float4 pos : SV_POSITION, float3 worldPos : TEXCOORD2,
    float3 normal : NORMAL, float2 uv : TEXCOORD, float2 uv1 : TEXCOORD1,
    float4 vertexColor : COLOR, float4 tangent : TANGENT,
    float4 uvChannels0 : TEXCOORD3, float4 uvChannels1 : TEXCOORD4,
    float4 uvChannels2 : TEXCOORD5, float4 uvChannels3 : TEXCOORD6,
    float4 uvChannels4 : TEXCOORD7, nointerpolation uint objectIndex : TEXCOORD11,
    bool frontFace : SV_IsFrontFace) : SV_TARGET
{
    // Calling the surface function preserves alpha masking and portal clips.
    float4 surface = EngineSurfacePS(pos, worldPos, normal, uv, uv1,
        vertexColor, tangent, uvChannels0, uvChannels1, uvChannels2,
        uvChannels3, uvChannels4, objectIndex, frontFace);
    return float4(1.0, 0.15, 0.05, surface.a * 0.3);
}

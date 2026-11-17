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
    float instanceLodDistance : TEXCOORD12,
    bool frontFace : SV_IsFrontFace,
    uint primitiveId : SV_PrimitiveID) : SV_TARGET
{
    g_primitiveId = primitiveId; // editor Triangles view
    // Instanced LOD bands dissolve per tree (see ApplyGeometryFade).
    g_instanceLodDistance = instanceLodDistance;
#ifdef GRASS_SURFACE_SHADING
    return PSGrassMain(pos, worldPos, normal, uv, vertexColor, tangent,
        objectIndex, frontFace);
#else
    return PSFoliageMain(pos, worldPos, normal, uv, vertexColor,
        objectIndex, frontFace);
#endif
}

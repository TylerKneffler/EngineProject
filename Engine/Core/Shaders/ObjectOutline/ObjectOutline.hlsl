// ObjectOutline.hlsl
// Dedicated selected-object outline shader.
// Uses the same constant-buffer layout as Object.hlsl to keep CPU bindings unchanged.

#ifdef VULKAN
struct ObjectConst
{
    float4x4 mvp;
    float4 diffuseColor;
    float4 ambientColor;
    float4 specularColor;
    float4 materialParams;
};
[[vk::push_constant]] ObjectConst cb;
#define mvp          cb.mvp
#define diffuseColor cb.diffuseColor
#else
cbuffer Constants : register(b0)
{
    float4x4 mvp;
    float4 diffuseColor;
    float4 ambientColor;
    float4 specularColor;
    float4 materialParams;
};
#endif

void VSMain(
    float3 pos : POSITION,
    float3 normal : NORMAL,
    float2 uv : TEXCOORD,
    float4 tangent : TANGENT,
    out float4 oPos : SV_POSITION)
{
    oPos = mul(mvp, float4(pos, 1.0));
}

float4 PSMain(float4 pos : SV_POSITION) : SV_TARGET
{
    return diffuseColor;
}

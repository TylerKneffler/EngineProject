// Object.hlsl — Phong-shaded mesh shader.

// Layout (112 bytes, fits Vulkan's 128-byte push-constant minimum):
//   float4x4 mvp          (64 b)
//   float4   diffuseColor (16 b)
//   float4   ambientColor (16 b)
//   float4   specularColor (16 b)
//
// When compiled with -DVULKAN=1 (dxc -spirv), the cbuffer becomes a
// push-constant block so no descriptor set is needed.

#ifdef VULKAN
struct ObjectConst
{
    float4x4 mvp;
    float4   diffuseColor;
    float4   ambientColor;
    float4   specularColor; // xyz = color, w = shininess
};
[[vk::push_constant]] ObjectConst cb;
#define mvp           cb.mvp
#define diffuseColor  cb.diffuseColor
#define ambientColor  cb.ambientColor
#define specularColor cb.specularColor
#else
cbuffer Constants : register(b0)
{
    float4x4 mvp;
    float4   diffuseColor;
    float4   ambientColor;
    float4   specularColor; // xyz = color, w = shininess
};
#endif

// ---------------------------------------------------------------------------
// Vertex shader
// ---------------------------------------------------------------------------
void VSMain(float3 pos    : POSITION,
            float3 normal : NORMAL,
            out float4 oPos    : SV_POSITION,
            out float3 oNormal : NORMAL)
{
    oPos    = mul(mvp, float4(pos, 1.0));
    oNormal = normal; // object-space normal (approximation without world matrix)
}

// ---------------------------------------------------------------------------
// Pixel shader
// ---------------------------------------------------------------------------
float4 PSMain(float4 pos    : SV_POSITION,
              float3 normal : NORMAL) : SV_TARGET
{
    float3 lightDir = normalize(float3(1.0, 2.0, -1.0));
    float  diffuse  = saturate(dot(normalize(normal), lightDir));
    float3 color    = ambientColor.rgb + diffuseColor.rgb * diffuse;
    return float4(color, 1.0);
}

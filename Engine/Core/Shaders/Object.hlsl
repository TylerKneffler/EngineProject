// Object.hlsl — Phong-shaded mesh shader.

cbuffer Constants : register(b0)
{
    float4x4 mvp;
    float4x4 world;
    float4   diffuseColor;
    float4   ambientColor;
    float4   specularColor; // xyz = color, w = shininess
};

// ---------------------------------------------------------------------------
// Vertex shader
// ---------------------------------------------------------------------------
void VSMain(float3 pos    : POSITION,
            float3 normal : NORMAL,
            out float4 oPos    : SV_POSITION,
            out float3 oNormal : NORMAL)
{
    oPos    = mul(mvp, float4(pos, 1.0));
    oNormal = mul((float3x3)world, normal);
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

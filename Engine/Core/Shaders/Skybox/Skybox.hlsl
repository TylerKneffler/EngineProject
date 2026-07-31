// Equirectangular panorama skybox rendered as a fullscreen triangle.

#ifdef VULKAN
struct SkyboxConst { float4x4 invVP; };
[[vk::push_constant]] SkyboxConst cb;
#define invVP cb.invVP
[[vk::binding(0, 0)]] Texture2D skyTexture;
[[vk::binding(5, 0)]] SamplerState skySampler;
#else
cbuffer SkyboxCB : register(b0) { float4x4 invVP; };
Texture2D skyTexture : register(t0);
SamplerState skySampler : register(s0);
#endif

void VSMain(uint id : SV_VertexID,
            out float4 oPos : SV_POSITION,
            out float2 oNDC : TEXCOORD0)
{
    oNDC.x = (id == 1) ? 3.0 : -1.0;
    oNDC.y = (id == 2) ? 3.0 : -1.0;
    oPos = float4(oNDC, 1.0, 1.0);
}

float4 PSMain(float4 position : SV_POSITION,
              float2 ndc : TEXCOORD0) : SV_TARGET
{
    float4 nearPoint = mul(invVP, float4(ndc, 0.0, 1.0));
    float4 farPoint = mul(invVP, float4(ndc, 1.0, 1.0));
    nearPoint /= nearPoint.w;
    farPoint /= farPoint.w;
    float3 direction = normalize(farPoint.xyz - nearPoint.xyz);

    const float inverseTwoPi = 0.1591549431;
    const float inversePi = 0.3183098862;
    float2 uv;
    uv.x = 0.5 + atan2(direction.z, direction.x) * inverseTwoPi;
    uv.y = acos(clamp(direction.y, -1.0, 1.0)) * inversePi;
    return float4(skyTexture.Sample(skySampler, uv).rgb, 1.0);
}

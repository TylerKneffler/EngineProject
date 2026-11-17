// Equirectangular panorama skybox rendered as a fullscreen triangle.
// The scene's height fog (AtmosphereFog.hlsli) hazes it toward the horizon.

#include "../AtmosphereFog.hlsli"

#ifdef VULKAN
struct SkyboxConst { float4x4 invVP; float4 displayParams; float4 skyFog[3]; };
[[vk::push_constant]] SkyboxConst cb;
#define invVP cb.invVP
#define displayParams cb.displayParams
#define skyFog cb.skyFog
[[vk::binding(0, 0)]] Texture2D skyTexture;
[[vk::binding(6, 0)]] SamplerState skySampler;
#else
cbuffer SkyboxCB : register(b0)
{
    float4x4 invVP;
    float4 displayParams; // 2D mode, exposure scale, rotation, sun dir z
    // [0] density at camera, height falloff, sky ray length, anisotropy
    // [1] ambient inscatter rgb, sun dir x
    // [2] sun glow rgb, sun dir y
    float4 skyFog[3];
};
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

float3 ApplySkyFog(float3 color, float3 direction)
{
    [branch] if (skyFog[0].x <= 0.0 || skyFog[0].z <= 0.0)
        return color;
    const float opticalDepth = AtmosphereOpticalDepth(skyFog[0].x,
        skyFog[0].y, direction.y, skyFog[0].z);
    const float opacity = 1.0 - exp(-opticalDepth);
    const float3 sunDirection = float3(skyFog[1].w, skyFog[2].w,
        displayParams.w);
    const float3 inscatter = AtmosphereInscatterColor(direction, sunDirection,
        skyFog[1].rgb, skyFog[2].rgb, skyFog[0].w);
    return lerp(color, inscatter, opacity);
}

float4 PSMain(float4 position : SV_POSITION,
              float2 ndc : TEXCOORD0) : SV_TARGET
{
    float3 sampledColor;
    if (displayParams.x > 0.5)
    {
        float2 backgroundUv = float2(
            ndc.x * 0.5 + 0.5,
            0.5 - ndc.y * 0.5);
        sampledColor = skyTexture.Sample(skySampler, backgroundUv).rgb *
            displayParams.y;
    }
    else
    {
        float4 nearPoint = mul(invVP, float4(ndc, 0.0, 1.0));
        float4 farPoint = mul(invVP, float4(ndc, 1.0, 1.0));
        nearPoint /= nearPoint.w;
        farPoint /= farPoint.w;
        const float3 worldDirection = normalize(farPoint.xyz - nearPoint.xyz);
        float3 direction = worldDirection;
        float sine = sin(-displayParams.z);
        float cosine = cos(-displayParams.z);
        direction.xz = float2(
            cosine * direction.x - sine * direction.z,
            sine * direction.x + cosine * direction.z);

        const float inverseTwoPi = 0.1591549431;
        const float inversePi = 0.3183098862;
        float2 uv;
        uv.x = 0.5 + atan2(direction.z, direction.x) * inverseTwoPi;
        uv.y = acos(clamp(direction.y, -1.0, 1.0)) * inversePi;
        sampledColor = skyTexture.Sample(skySampler, uv).rgb * displayParams.y;
        sampledColor = ApplySkyFog(max(sampledColor, 0.0), worldDirection);
    }
    return float4(max(sampledColor, 0.0), 1.0);
}

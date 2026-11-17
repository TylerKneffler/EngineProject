// Equirectangular panorama skybox rendered as a fullscreen triangle, plus an
// optional sun disc and forward-scattering glow toward the brightest
// directional light. The scene's height fog (AtmosphereFog.hlsli) hazes it
// toward the horizon.
//
// 128 bytes, the Vulkan push-constant limit (SceneRendererLighting::
// SkyboxConstants). The view ray is affine in NDC, so three vectors replace
// the inverse view-projection and their w components carry fog terms:
// rayX/rayY/rayOrigin: world ray = rayX * ndc.x + rayY * ndc.y + rayOrigin;
//                      w = fog density at the camera, height falloff, ray length
// displayParams: 2D background, exposure scale, rotation (radians), sun glow
// sunDirection:  world direction toward the sun, w = cos(disc radius)
// sunRadiance:   light radiance rgb, w = disc intensity; zero disables
// fogInscatter:  ambient inscatter rgb, w = Henyey-Greenstein anisotropy
// fogSunGlow:    fog sun glow rgb

#include "../AtmosphereFog.hlsli"

#ifdef VULKAN
struct SkyboxConst
{
    float4 rayX; float4 rayY; float4 rayOrigin; float4 displayParams;
    float4 sunDirection; float4 sunRadiance; float4 fogInscatter; float4 fogSunGlow;
};
[[vk::push_constant]] SkyboxConst cb;
#define rayX cb.rayX
#define rayY cb.rayY
#define rayOrigin cb.rayOrigin
#define displayParams cb.displayParams
#define sunDirection cb.sunDirection
#define sunRadiance cb.sunRadiance
#define fogInscatter cb.fogInscatter
#define fogSunGlow cb.fogSunGlow
[[vk::binding(0, 0)]] Texture2D skyTexture;
[[vk::binding(6, 0)]] SamplerState skySampler;
#else
cbuffer SkyboxCB : register(b0)
{
    float4x4 invVP;
    float4 rayX;
    float4 rayY;
    float4 rayOrigin;
    float4 displayParams;
    float4 sunDirection;
    float4 sunRadiance;
    float4 fogInscatter;
    float4 fogSunGlow;
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
    const float density = rayX.w;
    const float rayLength = rayOrigin.w;
    [branch] if (density <= 0.0 || rayLength <= 0.0)
        return color;
    const float opticalDepth = AtmosphereOpticalDepth(density, rayY.w,
        direction.y, rayLength);
    const float opacity = 1.0 - exp(-opticalDepth);
    const float3 inscatter = AtmosphereInscatterColor(direction,
        sunDirection.xyz, fogInscatter.rgb, fogSunGlow.rgb, fogInscatter.w);
    return lerp(color, inscatter, opacity);
}

// Disc with limb darkening, antialiased over one pixel, and a
// Henyey-Greenstein lobe (g = 0.76) for the bright haze around it.
float3 SunContribution(float3 direction)
{
    const float3 sunColor = sunRadiance.rgb;
    if (dot(sunColor, sunColor) <= 0.0)
        return 0.0;
    const float cosTheta = dot(direction, sunDirection.xyz);
    const float cosRadius = sunDirection.w;
    const float rim = max(fwidth(cosTheta), 1e-6);
    const float disc = saturate((cosTheta - cosRadius) / rim + 0.5);
    const float radial = saturate((1.0 - cosTheta) / max(1.0 - cosRadius, 1e-6));
    const float limb = 1.0 - 0.6 * (1.0 - sqrt(saturate(1.0 - radial)));
    const float g = 0.76;
    const float phase = (1.0 - g * g) /
        (12.5663706 * pow(max(1.0 + g * g - 2.0 * g * cosTheta, 1e-4), 1.5));
    return sunColor * (sunRadiance.w * disc * limb + displayParams.w * phase);
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
        const float3 worldDirection = normalize(rayX.xyz * ndc.x +
            rayY.xyz * ndc.y + rayOrigin.xyz);
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
        // The sun belongs to the world, not the rotated panorama; the fog
        // then hazes sky and sun together.
        sampledColor += SunContribution(worldDirection);
        sampledColor = ApplySkyFog(max(sampledColor, 0.0), worldDirection);
    }
    return float4(max(sampledColor, 0.0), 1.0);
}

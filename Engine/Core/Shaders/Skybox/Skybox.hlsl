// Equirectangular panorama skybox rendered as a fullscreen triangle, plus an
// optional sun disc and forward-scattering glow toward the brightest
// directional light.
//
// displayParams: 2D background, exposure scale, rotation (radians), glow
// sunDirection:  world direction toward the sun, w = cos(disc radius)
// sunRadiance:   light radiance rgb, w = disc intensity; zero disables

#ifdef VULKAN
struct SkyboxConst { float4x4 invVP; float4 displayParams; float4 sunDirection; float4 sunRadiance; };
[[vk::push_constant]] SkyboxConst cb;
#define invVP cb.invVP
#define displayParams cb.displayParams
#define sunDirection cb.sunDirection
#define sunRadiance cb.sunRadiance
[[vk::binding(0, 0)]] Texture2D skyTexture;
[[vk::binding(6, 0)]] SamplerState skySampler;
#else
cbuffer SkyboxCB : register(b0)
{
    float4x4 invVP;
    float4 displayParams;
    float4 sunDirection;
    float4 sunRadiance;
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
        // The sun belongs to the world, not the rotated panorama.
        sampledColor += SunContribution(worldDirection);
    }
    return float4(max(sampledColor, 0.0), 1.0);
}

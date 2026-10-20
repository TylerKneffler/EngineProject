// CPU simulated particle records, drawn as camera-facing quads.
#ifdef VULKAN
struct ParticleConstants
{
    float4x4 viewProjection;
    float4 cameraRight;
    float4 cameraUp;
    float4 particleParams;
};
[[vk::push_constant]] ParticleConstants cb;
#define viewProjection cb.viewProjection
#define cameraRight cb.cameraRight
#define cameraUp cb.cameraUp
#define particleParams cb.particleParams
[[vk::binding(0, 0)]] Texture2D particleTexture;
[[vk::binding(6, 0)]] SamplerState particleSampler;
#else
cbuffer ParticleConstants : register(b0)
{
    float4x4 viewProjection;
    float4 cameraRight;
    float4 cameraUp;
    float4 particleParams;
};
Texture2D particleTexture : register(t0);
SamplerState particleSampler : register(s0);
#endif

struct ParticleInput
{
    float4 positionSize : POSITION;
    float4 colorAlpha : COLOR0;
    float4 rotation : TEXCOORD0;
};

struct ParticleVertex
{
    float4 position : SV_POSITION;
    float4 color : COLOR0;
    float2 uv : TEXCOORD0;
};

ParticleVertex VSMain(ParticleInput input, uint vertexId : SV_VertexID)
{
    float2 corner = float2(
        (vertexId == 1 || vertexId == 4 || vertexId == 5) ? 1.0 : -1.0,
        (vertexId == 2 || vertexId == 3 || vertexId == 5) ? 1.0 : -1.0);
    float sine = sin(input.rotation.x);
    float cosine = cos(input.rotation.x);
    float2 rotated = float2(cosine * corner.x - sine * corner.y,
        sine * corner.x + cosine * corner.y);
    float3 world = input.positionSize.xyz + input.positionSize.w *
        (rotated.x * cameraRight.xyz + rotated.y * cameraUp.xyz);
    ParticleVertex result;
    result.position = mul(viewProjection, float4(world, 1.0));
    result.color = input.colorAlpha;
    result.uv = float2(corner.x * 0.5 + 0.5, 0.5 - corner.y * 0.5);
    return result;
}

float4 PSMain(ParticleVertex input) : SV_TARGET
{
    if (particleParams.x > 0.5)
    {
        float4 sampled = particleTexture.Sample(particleSampler, input.uv);
        return float4(input.color.rgb * sampled.rgb, input.color.a * sampled.a);
    }
    float2 centered = input.uv * 2.0 - 1.0;
    float radial = saturate(1.0 - dot(centered, centered));
    return float4(input.color.rgb, input.color.a * radial * radial);
}

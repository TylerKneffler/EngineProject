// CPU simulated particle records, drawn as camera-facing quads.
#ifdef VULKAN
struct ParticleConstants
{
    float4x4 viewProjection;
    float4 cameraRight;
    float4 cameraUp;
    float4 particleParams;
    float4 portalClipPlane;
};
[[vk::push_constant]] ParticleConstants cb;
#define viewProjection cb.viewProjection
#define cameraRight cb.cameraRight
#define cameraUp cb.cameraUp
#define particleParams cb.particleParams
#define portalClipPlane cb.portalClipPlane
[[vk::binding(0, 0)]] Texture2D particleTexture;
[[vk::binding(6, 0)]] SamplerState particleSampler;
struct GpuParticle
{
    float4 positionAge;
    float4 velocityLife;
    float4 colorSize;
    uint4 meta;
};
[[vk::binding(7, 0)]] StructuredBuffer<GpuParticle> gpuParticles;
#else
cbuffer ParticleConstants : register(b0)
{
    float4x4 viewProjection;
    float4 cameraRight;
    float4 cameraUp;
    float4 particleParams;
    float4 portalClipPlane;
};
Texture2D particleTexture : register(t0);
SamplerState particleSampler : register(s0);
struct GpuParticle
{
    float4 positionAge;
    float4 velocityLife;
    float4 colorSize;
    uint4 meta;
};
StructuredBuffer<GpuParticle> gpuParticles : register(t6);
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
    float3 worldPosition : TEXCOORD1;
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
    result.worldPosition = world;
    return result;
}

ParticleVertex VSMainGpu(uint vertexId : SV_VertexID,
    uint instanceId : SV_InstanceID)
{
    ParticleVertex result;
    if (instanceId >= gpuParticles[0].meta.x)
    {
        result.position = float4(0.0, 0.0, 2.0, 1.0);
        result.color = 0.0;
        result.uv = 0.0;
        result.worldPosition = 0.0;
        return result;
    }
    GpuParticle particle = gpuParticles[instanceId + 1];
    float2 corner = float2(
        (vertexId == 1 || vertexId == 4 || vertexId == 5) ? 1.0 : -1.0,
        (vertexId == 2 || vertexId == 3 || vertexId == 5) ? 1.0 : -1.0);
    float3 world = particle.positionAge.xyz + particle.colorSize.w *
        (corner.x * cameraRight.xyz + corner.y * cameraUp.xyz);
    result.position = mul(viewProjection, float4(world, 1.0));
    result.color = float4(particle.colorSize.xyz,
        saturate(1.0 - particle.positionAge.w /
            max(0.001, particle.velocityLife.w)) * particleParams.y);
    result.uv = float2(corner.x * 0.5 + 0.5, 0.5 - corner.y * 0.5);
    result.worldPosition = world;
    return result;
}

float4 PSMain(ParticleVertex input) : SV_TARGET
{
    if (dot(portalClipPlane.xyz, portalClipPlane.xyz) > 0.0 &&
        dot(float4(input.worldPosition, 1.0), portalClipPlane) < 0.0)
        clip(-1.0);
    if (particleParams.x > 0.5)
    {
        float4 sampled = particleTexture.Sample(particleSampler, input.uv);
        return float4(input.color.rgb * sampled.rgb, input.color.a * sampled.a);
    }
    float2 centered = input.uv * 2.0 - 1.0;
    float radial = saturate(1.0 - dot(centered, centered));
    return float4(input.color.rgb, input.color.a * radial * radial);
}

// One bounded screen-space density field is drawn for the entire fluid emitter.
// Contributions are accumulated before choosing an isosurface, so the final
// shape has no independent particle-disc edges.
struct FluidVertex
{
    float4 position : SV_POSITION;
    float2 ndc : TEXCOORD0;
};

bool ProjectFluidParticle(GpuParticle particle, out float2 center,
    out float2 radii, out float2 flowAxis, out float stretch,
    out float depth)
{
    center = 0.0;
    radii = 0.0;
    flowAxis = float2(1.0, 0.0);
    stretch = 1.0;
    depth = 1.0;
    if (dot(portalClipPlane.xyz, portalClipPlane.xyz) > 0.0 &&
        dot(float4(particle.positionAge.xyz, 1.0), portalClipPlane) < 0.0)
        return false;
    float4 projected = mul(viewProjection,
        float4(particle.positionAge.xyz, 1.0));
    if (projected.w <= 0.001) return false;
    float radius = max(particle.colorSize.w, 0.001);
    float4 right = mul(viewProjection, float4(
        particle.positionAge.xyz + cameraRight.xyz * radius, 1.0));
    float4 up = mul(viewProjection, float4(
        particle.positionAge.xyz + cameraUp.xyz * radius, 1.0));
    if (right.w <= 0.001 || up.w <= 0.001) return false;
    center = projected.xy / projected.w;
    radii = max(abs(float2(right.x / right.w - center.x,
        up.y / up.w - center.y)), float2(0.0001, 0.0001));
    float speed = length(particle.velocityLife.xyz);
    if (speed > 0.05)
    {
        float3 flow = particle.velocityLife.xyz / speed;
        float4 flowClip = mul(viewProjection,
            float4(particle.positionAge.xyz + flow * radius, 1.0));
        if (flowClip.w > 0.001)
        {
            float2 screenFlow = (flowClip.xy / flowClip.w - center) / radii;
            if (dot(screenFlow, screenFlow) > 0.0001)
            {
                flowAxis = normalize(screenFlow);
                // Keep projected area roughly constant as a fast stream
                // lengthens along its flow and narrows across it.
                stretch = 1.0 + min(speed * 0.22, 1.2);
            }
        }
    }
    depth = saturate(projected.z / projected.w);
    return true;
}

FluidVertex VSMainFluid(uint vertexId : SV_VertexID)
{
    float2 minimum = float2(2.0, 2.0);
    float2 maximum = float2(-2.0, -2.0);
    [loop] for (uint i = 0; i < min(gpuParticles[0].meta.x, 512u); ++i)
    {
        float2 center, radii, flowAxis;
        float stretch;
        float depth;
        if (!ProjectFluidParticle(gpuParticles[i + 1u], center, radii,
            flowAxis, stretch, depth))
            continue;
        minimum = min(minimum, center - radii * stretch * 2.1);
        maximum = max(maximum, center + radii * stretch * 2.1);
    }
    minimum = clamp(minimum, -1.0, 1.0);
    maximum = clamp(maximum, -1.0, 1.0);
    float2 corner = float2(
        (vertexId == 1 || vertexId == 4 || vertexId == 5) ? 1.0 : 0.0,
        (vertexId == 2 || vertexId == 3 || vertexId == 5) ? 1.0 : 0.0);
    FluidVertex result;
    result.ndc = lerp(minimum, maximum, corner);
    result.position = float4(result.ndc, 0.0, 1.0);
    return result;
}

struct FluidPixel
{
    float4 color : SV_TARGET0;
    float depth : SV_Depth;
};

FluidPixel PSMainFluid(FluidVertex input)
{
    float density = 0.0;
    float frontDepth = 1.0;
    float3 colorSum = 0.0;
    float speedSum = 0.0;
    [loop] for (uint i = 0; i < min(gpuParticles[0].meta.x, 512u); ++i)
    {
        GpuParticle particle = gpuParticles[i + 1u];
        float2 center, radii, flowAxis;
        float stretch;
        float depth;
        if (!ProjectFluidParticle(particle, center, radii,
            flowAxis, stretch, depth)) continue;
        float2 relative = (input.ndc - center) / radii;
        float2 crossAxis = float2(-flowAxis.y, flowAxis.x);
        float2 kernelPosition = float2(dot(relative, flowAxis) / stretch,
            dot(relative, crossAxis) * stretch);
        float radiusSquared = dot(kernelPosition, kernelPosition);
        float kernel = saturate(1.0 - radiusSquared * 0.25);
        float weight = kernel * kernel * kernel;
        density += weight;
        colorSum += particle.colorSize.xyz * weight;
        speedSum += length(particle.velocityLife.xyz) * weight;
        if (weight > 0.015) frontDepth = min(frontDepth, depth);
    }
    clip(density - 0.12);
    float3 tint = colorSum / max(density, 0.001);
    float nearestSpeed = speedSum / max(density, 0.001);
    float thickness = saturate(density * 0.27);
    float coverage = smoothstep(0.12, 0.22, density);
    float rim = 1.0 - smoothstep(0.16, 0.72, density);
    float time = particleParams.z;
    float phaseA = dot(input.ndc, float2(84.0, 37.0)) - time * 2.1;
    float phaseB = dot(input.ndc, float2(-51.0, 73.0)) + time * 1.6;
    float2 rippleSlope = float2(cos(phaseA), cos(phaseB));
    float2 silhouetteSlope = float2(ddx(density), -ddy(density));
    float3 normal = normalize(float3(-silhouetteSlope * 1.8 +
        rippleSlope * 0.04, 1.0));
    float fresnel = pow(1.0 - saturate(normal.z), 2.5);
    float specular = pow(saturate(dot(normal,
        normalize(float3(-0.4, 0.55, 1.0)))), 16.0);
    float flowHighlight = pow(saturate(0.5 + 0.5 *
        sin(phaseA + 0.3 * sin(phaseB))), 10.0) *
        smoothstep(0.3, 2.5, nearestSpeed);
    float3 interior = lerp(float3(0.42, 0.68, 0.78), tint, 0.22);
    float3 water = lerp(interior, float3(0.38, 0.78, 1.0), rim);
    water += (0.10 * specular + 0.035 * flowHighlight) *
        float3(0.85, 0.95, 1.0);
    float opacity = particleParams.y * coverage *
        (0.12 + 0.60 * rim + 0.07 * fresnel + 0.04 * thickness);
    FluidPixel result;
    result.color = float4(saturate(water), opacity);
    result.depth = frontDepth;
    return result;
}

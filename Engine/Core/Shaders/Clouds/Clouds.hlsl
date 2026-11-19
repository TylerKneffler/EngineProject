// Volumetric cloud layer. CSMain ray-marches it at half resolution into a
// buffer (one float4 of premultiplied radiance + alpha per half-res pixel);
// PSMain then upsamples that bilinearly and is blended (premultiplied alpha)
// over the sky right after Skybox.hlsl, before the scene's surfaces, so
// terrain and objects in front of it hide it. A flat layer between
// baseHeight and topHeight (world metres):
//   density   = coverage-remapped 3D value-noise FBM x cumulus height profile,
//               spatially varied by a large 2D weather noise and eroded at
//               the edges by a finer noise (detail)
//   lighting  = short march toward the sun (Beer-Lambert with a multiple-
//               scattering floor) x two-lobe Henyey-Greenstein phase, plus
//               sky light that brightens toward the cloud tops
//   integral  = energy-conserving per step (scattering * (1 - T_step))
// Clouds fade out toward maxDistance so the horizon blends into the sky.
//
// Constants (128 bytes, the Vulkan push-constant limit; see
// Rendering/Lighting/ViewLighting.h CloudConstants):
//   rayX/rayY/rayOrigin.xyz  view ray basis (as Skybox.hlsl): ray = rayX * ndc.x
//                            + rayY * ndc.y + rayOrigin, direction only
//   rayX.w   time (s)        rayY.w base height      rayOrigin.w top height
//   camera   world position, w = coverage
//   sunDirection  toward the sun, w = extinction per metre
//   sunRadiance   rgb (sun intensity applied), w = max distance
//   ambient       rgb sky light, w = shape scale (m)
//   shape         x/z wind offset (m), y = detail, w = packed uint (asuint):
//                 half-res width (bits 0-11), height (12-23), steps (24-31)

#ifdef VULKAN
struct CloudConst
{
    float4 rayX; float4 rayY; float4 rayOrigin; float4 camera;
    float4 sunDirection; float4 sunRadiance; float4 ambient; float4 shape;
};
[[vk::push_constant]] CloudConst cb;
#define rayX cb.rayX
#define rayY cb.rayY
#define rayOrigin cb.rayOrigin
#define camera cb.camera
#define sunDirection cb.sunDirection
#define sunRadiance cb.sunRadiance
#define ambient cb.ambient
#define shape cb.shape
#ifdef CLOUD_MARCH
[[vk::binding(1, 0)]] RWStructuredBuffer<float4> cloudOutput; // compute write slot
#else
[[vk::binding(7, 0)]] StructuredBuffer<float4> cloudSamples;  // graphics slot 6
#endif
#else
cbuffer CloudCB : register(b0)
{
    float4 rayX;
    float4 rayY;
    float4 rayOrigin;
    float4 camera;
    float4 sunDirection;
    float4 sunRadiance;
    float4 ambient;
    float4 shape;
};
RWStructuredBuffer<float4> cloudOutput : register(u0); // compute write slot
StructuredBuffer<float4> cloudSamples : register(t6);  // graphics slot 6
#endif

// Half-res grid width, height and march steps (shape.w bit-packed).
uint3 CloudGrid()
{
    const uint packed = asuint(shape.w);
    return uint3(packed & 0xFFFu, (packed >> 12) & 0xFFFu, packed >> 24);
}

void VSMain(uint id : SV_VertexID,
            out float4 oPos : SV_POSITION,
            out float2 oNDC : TEXCOORD0)
{
    oNDC.x = (id == 1) ? 3.0 : -1.0;
    oNDC.y = (id == 2) ? 3.0 : -1.0;
    oPos = float4(oNDC, 0.0, 1.0); // far plane (reverse-Z); depth is not tested
}

float Hash3(float3 p)
{
    p = frac(p * 0.3183099 + 0.1);
    p *= 17.0;
    return frac(p.x * p.y * p.z * (p.x + p.y + p.z));
}

// Smooth 3D value noise in 0..1.
float ValueNoise3(float3 x)
{
    const float3 i = floor(x);
    float3 f = frac(x);
    f = f * f * (3.0 - 2.0 * f);
    return lerp(
        lerp(lerp(Hash3(i), Hash3(i + float3(1, 0, 0)), f.x),
             lerp(Hash3(i + float3(0, 1, 0)), Hash3(i + float3(1, 1, 0)), f.x), f.y),
        lerp(lerp(Hash3(i + float3(0, 0, 1)), Hash3(i + float3(1, 0, 1)), f.x),
             lerp(Hash3(i + float3(0, 1, 1)), Hash3(i + float3(1, 1, 1)), f.x), f.y), f.z);
}

float Fbm3(float3 p, uint octaves)
{
    float sum = 0.0, amplitude = 0.5, norm = 0.0;
    [loop] for (uint i = 0u; i < octaves; ++i)
    {
        sum += ValueNoise3(p) * amplitude;
        norm += amplitude;
        p = p * 2.03 + float3(17.1, 3.7, 9.3);
        amplitude *= 0.5;
    }
    return sum / norm;
}

float3 Hash33(float3 p)
{
    p = frac(p * float3(0.1031, 0.1030, 0.0973));
    p += dot(p, p.yxz + 33.33);
    return frac((p.xxy + p.yxx) * p.zyx);
}

// Distance to the nearest of one random point per unit cell (Worley F1),
// about 0 at a point and up to ~1 between them: inverted, round "bubbles".
float Worley3(float3 x)
{
    const float3 cell = floor(x);
    const float3 local = x - cell;
    float nearest = 1.0;
    [unroll] for (int k = -1; k <= 1; ++k)
    [unroll] for (int j = -1; j <= 1; ++j)
    [unroll] for (int i = -1; i <= 1; ++i)
    {
        const float3 offset = float3(i, j, k);
        const float3 toPoint = offset + Hash33(cell + offset) - local;
        nearest = min(nearest, dot(toPoint, toPoint));
    }
    return sqrt(nearest);
}

// Cloud density (extinction per metre) at a world position. cheap (the
// light march) skips the billows, the edge erosion and two shape octaves.
float CloudDensity(float3 position, bool cheap)
{
    const float base = rayY.w, top = rayOrigin.w;
    const float h = saturate((position.y - base) / max(top - base, 1.0));
    if (h <= 0.0 || h >= 1.0) return 0.0;
    const float scale = ambient.w;
    const float3 p = float3(position.x + shape.x, position.y, position.z + shape.z) / scale;
    // Weather: which regions of the sky hold clouds at all.
    const float weather = ValueNoise3(float3(p.x * 0.18, 0.0, p.z * 0.18)) * 0.65 +
        ValueNoise3(float3(p.x * 0.45, 3.1, p.z * 0.45)) * 0.35;
    const float coverage = saturate(camera.w * (0.7 + 0.9 * weather));
    if (coverage <= 0.001) return 0.0;
    const float cloudShape = Fbm3(float3(p.x, p.y * 1.6, p.z), cheap ? 2u : 4u);
    // Cumulus: flat bases at the condensation level, domed tops (the
    // threshold rises with height, so only the strongest cells reach up).
    const float threshold = (1.0 - coverage) + h * h * 0.25;
    float density = saturate((cloudShape - threshold) / max(1.0 - threshold, 0.05)) *
        saturate(h * 14.0);
    [branch] if (!cheap && density > 0.0)
    {
        // Billows: inverted cellular noise rounds the edges into puffs.
        const float billow = Worley3(p * 3.1 + float3(0.0, rayX.w * 0.0008, 0.0));
        density = saturate(density - billow * 0.55 * (1.0 - density));
        [branch] if (density > 0.0 && shape.y > 0.0)
        {
            const float erosion = ValueNoise3(p * 11.0 + rayX.w * 0.004);
            density = saturate(density - erosion * shape.y * (1.0 - density) * 0.8);
        }
        // Drop the thinnest remnants: they scatter as isolated specks.
        density = saturate((density - 0.06) / 0.94);
    }
    return density * sunDirection.w;
}

float HenyeyGreenstein(float cosTheta, float g)
{
    const float g2 = g * g;
    return (1.0 - g2) / (12.5663706 * pow(max(1.0 + g2 - 2.0 * g * cosTheta, 1e-4), 1.5));
}

// Premultiplied cloud radiance and alpha along the view ray through ndc.
// pixel only seeds the step jitter.
float4 MarchClouds(float2 ndc, float2 pixel)
{
    const float3 direction = normalize(rayX.xyz * ndc.x + rayY.xyz * ndc.y + rayOrigin.xyz);
    const float3 eye = camera.xyz;
    const float base = rayY.w, top = rayOrigin.w, maxDistance = sunRadiance.w;
    // The segment of the view ray inside the layer.
    float t0 = 0.0, t1 = maxDistance;
    if (eye.y < base)
    {
        if (direction.y <= 1e-4) return 0.0;
        t0 = (base - eye.y) / direction.y;
        t1 = (top - eye.y) / direction.y;
    }
    else if (eye.y > top)
    {
        if (direction.y >= -1e-4) return 0.0;
        t0 = (top - eye.y) / direction.y;
        t1 = (base - eye.y) / direction.y;
    }
    else if (abs(direction.y) > 1e-4)
        t1 = ((direction.y > 0.0 ? top : base) - eye.y) / direction.y;
    t1 = min(t1, maxDistance);
    if (t0 >= t1) return 0.0;

    const uint steps = max(CloudGrid().z, 1u);
    const float stepLength = (t1 - t0) / steps;
    // Interleaved gradient noise hides the step pattern.
    const float jitter = frac(52.9829189 * frac(dot(pixel, float2(0.06711056, 0.00583715))));
    const float3 toSun = sunDirection.xyz;
    const float cosTheta = dot(direction, toSun);
    const float phase = lerp(HenyeyGreenstein(cosTheta, -0.25),
        HenyeyGreenstein(cosTheta, 0.7), 0.65) * 12.5663706;
    const float lightStep = (top - base) / 12.0;

    float transmittance = 1.0;
    float3 scattered = 0.0;
    float firstHit = -1.0;
    [loop] for (uint i = 0u; i < steps; ++i)
    {
        const float t = t0 + (i + jitter) * stepLength;
        const float3 samplePosition = eye + direction * t;
        const float density = CloudDensity(samplePosition, false);
        if (density <= 0.0) continue;
        if (firstHit < 0.0) firstHit = t;
        // Optical depth toward the sun over a few growing steps.
        float lightDepth = 0.0;
        [unroll] for (uint j = 0u; j < 5u; ++j)
        {
            const float reach = lightStep * (j * j * 0.5 + j + 0.5);
            lightDepth += CloudDensity(samplePosition + toSun * reach, true) *
                lightStep * (j + 1.0);
        }
        // Multiple scattering keeps thick clouds from going black inside.
        const float sunVisibility = max(exp(-lightDepth), 0.35 * exp(-0.25 * lightDepth));
        const float height = saturate((samplePosition.y - base) / max(top - base, 1.0));
        const float3 luminance = sunRadiance.rgb * sunVisibility * phase +
            ambient.rgb * lerp(0.6, 1.0, height);
        const float stepTransmittance = exp(-density * stepLength);
        scattered += transmittance * luminance * (1.0 - stepTransmittance);
        transmittance *= stepTransmittance;
        if (transmittance < 0.01) break;
    }
    if (firstHit < 0.0) return 0.0;
    // Distant clouds dissolve into the (already hazed) sky behind them.
    const float fade = 1.0 - smoothstep(0.45 * maxDistance, maxDistance, firstHit);
    return float4(scattered * fade, (1.0 - transmittance) * fade);
}

#if !defined(VULKAN) || defined(CLOUD_MARCH)
// One thread per half-res pixel, rows along x.
[numthreads(64, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    const uint3 grid = CloudGrid();
    if (id.x >= grid.x * grid.y) return;
    const uint2 pixel = uint2(id.x % grid.x, id.x / grid.x);
    const float2 ndc = float2((pixel.x + 0.5) / grid.x * 2.0 - 1.0,
        1.0 - (pixel.y + 0.5) / grid.y * 2.0);
    cloudOutput[id.x] = MarchClouds(ndc, float2(pixel));
}
#endif

#if !defined(VULKAN) || !defined(CLOUD_MARCH)
float4 CloudSample(int2 pixel, uint3 grid)
{
    pixel = clamp(pixel, int2(0, 0), int2(grid.xy) - 1);
    return cloudSamples[pixel.y * grid.x + pixel.x];
}

// Bilinear upsample of the half-res march (clouds are soft; their edges
// against terrain are resolved at full resolution by the terrain drawn later).
float4 PSMain(float4 position : SV_POSITION, float2 ndc : TEXCOORD0) : SV_TARGET
{
    const uint3 grid = CloudGrid();
    if (grid.x == 0u || grid.y == 0u) return 0.0;
    const float2 coordinate = float2((ndc.x * 0.5 + 0.5) * grid.x,
        (0.5 - ndc.y * 0.5) * grid.y) - 0.5;
    const int2 corner = int2(floor(coordinate));
    const float2 t = coordinate - corner;
    return lerp(lerp(CloudSample(corner, grid), CloudSample(corner + int2(1, 0), grid), t.x),
        lerp(CloudSample(corner + int2(0, 1), grid), CloudSample(corner + int2(1, 1), grid), t.x),
        t.y);
}
#endif

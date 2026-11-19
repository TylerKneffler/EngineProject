// Post-processing for every backend: the bloom mip chain and the final
// compose (bloom mix, exposure, tone mapping, color grading, output encoding).
// DX11 and DX12 compile this file at runtime; Vulkan compiles VSMain, PSMain,
// BloomDownsamplePS and BloomUpsamplePS to SPIR-V at build time.
//
// Constants (64 bytes, see Core/Graphics/PostProcess.h):
//   params0       compose: exposure, operator (uint bits), bloom intensity,
//                 accumulation sample weight; bloom passes: source texel size
//   shadowTint    rgb, w = grading enabled
//   highlightTint rgb, w = shadow/highlight balance (-1..1)
//   grade         contrast, saturation, 1 / bloom levels, auto exposure
//   (auto exposure passes: see MakeExposureConstants)
//
// Operators: 0 none, 1 ACES, 2 Reinhard, 3 HDR10 PQ export, 4 accumulate a
// weighted sample (no tone mapping), 5 linear HDR export.
//
// Auto exposure (ExposureLuminancePS, ExposureHistogramPS, ExposureAdaptPS)
// meters the scene into a log-luminance histogram and eases an adapted EV
// toward its trimmed mean; compose maps that EV to mid grey (0.18).

#ifdef VULKAN
[[vk::binding(0, 0)]] Texture2D<float4> sceneColor;
[[vk::binding(1, 0)]] SamplerState sceneSampler;
[[vk::binding(2, 0)]] Texture2D<float4> bloomColor;
[[vk::binding(3, 0)]] Texture2D<float4> exposureColor;
struct PostProcessParameters
{
    float4 params0;
    float4 shadowTint;
    float4 highlightTint;
    float4 grade;
};
[[vk::push_constant]] PostProcessParameters parameters;
#define params0 parameters.params0
#define shadowTint parameters.shadowTint
#define highlightTint parameters.highlightTint
#define grade parameters.grade
#else
Texture2D<float4> sceneColor : register(t0);
Texture2D<float4> bloomColor : register(t1);
Texture2D<float4> exposureColor : register(t2);
SamplerState sceneSampler : register(s0);
cbuffer PostProcessCB : register(b0)
{
    float4 params0;
    float4 shadowTint;
    float4 highlightTint;
    float4 grade;
};
#endif

struct VertexOutput { float4 position : SV_Position; float2 uv : TEXCOORD0; };

VertexOutput VSMain(uint vertexId : SV_VertexID)
{
    VertexOutput output;
    output.uv = float2((vertexId << 1) & 2, vertexId & 2);
    output.position = float4(output.uv * float2(2.0, -2.0) +
        float2(-1.0, 1.0), 0.0, 1.0);
    return output;
}

// Clamp keeps a stray NaN/Inf or extreme firefly from flooding the chain.
float3 SampleSource(float2 uv)
{
    return min(max(sceneColor.SampleLevel(sceneSampler, uv, 0.0).rgb, 0.0), 32768.0);
}

// 13-tap downsample (Jimenez, "Next Generation Post Processing in Call of
// Duty: Advanced Warfare"): a centre box weighted 0.5 plus four overlapping
// corner boxes at 0.125 each. Avoids the pulsing of a plain 2x2 box.
float4 BloomDownsamplePS(VertexOutput input) : SV_Target
{
    const float2 texel = params0.xy;
    const float2 uv = input.uv;
    float3 a = SampleSource(uv + texel * float2(-2.0, -2.0));
    float3 b = SampleSource(uv + texel * float2( 0.0, -2.0));
    float3 c = SampleSource(uv + texel * float2( 2.0, -2.0));
    float3 d = SampleSource(uv + texel * float2(-1.0, -1.0));
    float3 e = SampleSource(uv + texel * float2( 1.0, -1.0));
    float3 f = SampleSource(uv + texel * float2(-2.0,  0.0));
    float3 g = SampleSource(uv);
    float3 h = SampleSource(uv + texel * float2( 2.0,  0.0));
    float3 i = SampleSource(uv + texel * float2(-1.0,  1.0));
    float3 j = SampleSource(uv + texel * float2( 1.0,  1.0));
    float3 k = SampleSource(uv + texel * float2(-2.0,  2.0));
    float3 l = SampleSource(uv + texel * float2( 0.0,  2.0));
    float3 m = SampleSource(uv + texel * float2( 2.0,  2.0));
    float3 result = (d + e + i + j) * 0.125;
    result += (a + b + f + g) * 0.03125;
    result += (b + c + g + h) * 0.03125;
    result += (f + g + k + l) * 0.03125;
    result += (g + h + l + m) * 0.03125;
    return float4(result, 1.0);
}

// 3x3 tent upsample of the next smaller level, added (blend ONE, ONE) onto
// the current level, so level 0 ends up holding the sum of every level.
float4 BloomUpsamplePS(VertexOutput input) : SV_Target
{
    const float2 texel = params0.xy;
    const float2 uv = input.uv;
    float3 result = SampleSource(uv) * 4.0;
    result += (SampleSource(uv + texel * float2( 0.0, -1.0)) +
               SampleSource(uv + texel * float2(-1.0,  0.0)) +
               SampleSource(uv + texel * float2( 1.0,  0.0)) +
               SampleSource(uv + texel * float2( 0.0,  1.0))) * 2.0;
    result += SampleSource(uv + texel * float2(-1.0, -1.0)) +
              SampleSource(uv + texel * float2( 1.0, -1.0)) +
              SampleSource(uv + texel * float2(-1.0,  1.0)) +
              SampleSource(uv + texel * float2( 1.0,  1.0));
    return float4(result * (1.0 / 16.0), 1.0);
}

static const float3 LuminanceWeights = float3(0.2126, 0.7152, 0.0722);

// Exposure map texel: geometric mean luminance of 4x4 bilinear taps over its
// footprint (log2 in r) and a centre-weighted metering weight (g).
// highlightTint.zw = 1 / map size.
float4 ExposureLuminancePS(VertexOutput input) : SV_Target
{
    float logSum = 0.0;
    [unroll] for (int y = 0; y < 4; ++y)
    [unroll] for (int x = 0; x < 4; ++x)
    {
        const float2 offset = (float2(x, y) - 1.5) * 0.25 * highlightTint.zw;
        const float luminance = dot(SampleSource(input.uv + offset), LuminanceWeights);
        logSum += log2(max(luminance, 1e-6));
    }
    const float2 centre = (input.uv - 0.5) * 2.0;
    const float weight = lerp(1.0, 0.25, saturate(dot(centre, centre) * 0.5));
    return float4(logSum / 16.0, weight, 0.0, 1.0);
}

uint ExposureBin(float logLuminance)
{
    const float bins = grade.z;
    const float t = saturate((logLuminance - params0.x) / params0.y);
    return (uint)min(t * bins, bins - 1.0);
}

// One histogram bin per pixel of a bins x 1 target: the metering weight of
// every exposure-map texel (t0) that falls in it.
float4 ExposureHistogramPS(VertexOutput input) : SV_Target
{
    const uint bin = (uint)input.position.x;
    const uint width = (uint)grade.x, texels = width * (uint)grade.y;
    float count = 0.0;
    // One flat loop: fxc mis-compiled a nested, partially unrolled version
    // (DX counted about a tenth of the map).
    [loop] for (uint i = 0; i < texels; ++i)
    {
        const float2 texel = sceneColor.Load(int3(i % width, i / width, 0)).rg;
        count += ExposureBin(texel.r) == bin ? texel.g : 0.0;
    }
    return float4(count, 0.0, 0.0, 1.0);
}

// 1x1: the histogram (t0) trimmed to [shadowTint.x, shadowTint.y] of its
// weight gives the target EV, clamped to [highlightTint.x, highlightTint.y];
// the previous result (t1) eases toward it at shadowTint.z (scene got
// brighter) or shadowTint.w (darker) per second, over params0.z seconds.
// Without history (params0.w = 0) it snaps. Output: adapted EV, 1, target.
float4 ExposureAdaptPS(VertexOutput input) : SV_Target
{
    const uint bins = (uint)grade.z;
    float total = 0.0;
    for (uint i = 0; i < bins; ++i)
        total += sceneColor.Load(int3(i, 0, 0)).r;
    const float low = total * shadowTint.x, high = total * shadowTint.y;
    float cumulative = 0.0, kept = 0.0, logSum = 0.0;
    for (uint b = 0; b < bins; ++b)
    {
        const float count = sceneColor.Load(int3(b, 0, 0)).r;
        const float share = max(min(cumulative + count, high) - max(cumulative, low), 0.0);
        logSum += share * (params0.x + (b + 0.5) / grade.z * params0.y);
        kept += share;
        cumulative += count;
    }
    const bool history = params0.w > 0.5;
    const float previous = history ? bloomColor.Load(int3(0, 0, 0)).r : 0.0;
    float target = kept > 0.0 ? logSum / kept : (history ? previous : 0.0);
    target = clamp(target, highlightTint.x, highlightTint.y);
    float adapted = target;
    if (history)
    {
        const float speed = target > previous ? shadowTint.z : shadowTint.w;
        adapted = previous + (target - previous) * (1.0 - exp(-params0.z * speed));
    }
    return float4(adapted, 1.0, target, 1.0);
}

float3 ToneMap(uint op, float3 color)
{
    if (op == 1u)
        return saturate((color * (2.51 * color + 0.03)) /
            (color * (2.43 * color + 0.59) + 0.14));
    if (op == 2u)
        return color / (1.0 + color);
    return color;
}

// Display-referred split-tone grade: contrast around mid grey, saturation,
// then shadow and highlight tints blended by perceived lightness.
float3 ApplyColorGrade(float3 color)
{
    if (shadowTint.w < 0.5)
        return color;
    const float3 lumaWeights = float3(0.2126, 0.7152, 0.0722);
    color = 0.18 * pow(max(color, 1e-5) / 0.18, grade.x);
    const float luma = dot(color, lumaWeights);
    color = max(lerp(luma.xxx, color, grade.y), 0.0);
    const float lightness = sqrt(saturate(luma));
    const float highlight = smoothstep(0.1, 0.9,
        lightness + highlightTint.w * 0.4);
    return color * lerp(shadowTint.rgb, highlightTint.rgb, highlight);
}

float3 LinearToSrgb(float3 color)
{
    color = max(color, 0.0);
    return lerp(color * 12.92,
        1.055 * pow(color, 1.0 / 2.4) - 0.055,
        step(0.0031308, color));
}

float3 LinearToPq(float3 color)
{
    const float m1 = 2610.0 / 16384.0, m2 = 2523.0 / 32.0;
    const float c1 = 3424.0 / 4096.0, c2 = 2413.0 / 128.0, c3 = 2392.0 / 128.0;
    const float3 l = pow(saturate(color * 0.1), m1);
    return pow((c1 + c2 * l) / (1.0 + c3 * l), m2);
}

float4 PSMain(VertexOutput input) : SV_Target
{
    const float4 source = sceneColor.Sample(sceneSampler, input.uv);
    const uint op = asuint(params0.y);
    if (op == 4u)
        return source * params0.w;
    float3 color = max(source.rgb, 0.0);
    [branch] if (params0.z > 0.0)
    {
        const float3 bloom = bloomColor.SampleLevel(sceneSampler, input.uv, 0.0).rgb *
            grade.z;
        color = lerp(color, bloom, params0.z);
    }
    color *= max(params0.x, 0.0);
    // Auto exposure: the adapted average luminance maps to mid grey.
    [branch] if (grade.w > 0.5)
        color *= 0.18 * exp2(-exposureColor.Load(int3(0, 0, 0)).r);
    if (op == 5u)
        return float4(color, source.a);
    if (op == 3u)
    {
        color = mul(float3x3(0.6274, 0.3293, 0.0433,
                             0.0691, 0.9195, 0.0114,
                             0.0164, 0.0880, 0.8956), color);
        return float4(LinearToPq(color), 1.0);
    }
    color = ApplyColorGrade(ToneMap(op, color));
    return float4(LinearToSrgb(color), source.a);
}

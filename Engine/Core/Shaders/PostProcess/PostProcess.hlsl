// Post-processing for every backend: the bloom mip chain and the final
// compose (bloom mix, exposure, tone mapping, color grading, output encoding).
// DX11 and DX12 compile this file at runtime; Vulkan compiles VSMain, PSMain,
// BloomDownsamplePS and BloomUpsamplePS to SPIR-V at build time.
//
// Constants (64 bytes, see Core/Graphics/PostProcess.h):
//   params0       compose: exposure, operator (uint bits), bloom intensity,
//                 accumulation sample weight (operator 4) or FXAA (w < 0,
//                 see ApplyFxaa); bloom passes: source texel size
//   shadowTint    rgb, w = grading enabled
//   highlightTint rgb, w = shadow/highlight balance (-1..1)
//   grade         contrast, saturation, 1 / bloom levels, auto exposure
//   (auto exposure passes: see MakeExposureConstants)
//
// Operators: 0 none, 1 ACES, 2 Reinhard, 3 HDR10 PQ export, 4 accumulate a
// weighted sample (no tone mapping), 5 linear HDR export, 6 AgX.
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
    float4 temporal;
};
[[vk::push_constant]] PostProcessParameters parameters;
#define params0 parameters.params0
#define shadowTint parameters.shadowTint
#define highlightTint parameters.highlightTint
#define grade parameters.grade
#define temporal parameters.temporal
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
    float4 temporal;
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
    float weight = lerp(1.0, 0.25, saturate(dot(centre, centre) * 0.5));
    // Ground weighting (temporal.x, 0-1; AutoExposureSettings::groundWeight):
    // the top of the frame (sky, a dark forest wall) counts down to a fifth as
    // much as the lower half, where the ground is (uv.y = 0 at the top).
    weight *= lerp(1.0, lerp(0.2, 1.0, smoothstep(0.3, 0.6, input.uv.y)), temporal.x);
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

// Temporal anti-aliasing resolve (before bloom, exposure and compose): this
// frame's jittered scene (t0) blends with the previous result (t1) fetched
// at the pixel's reprojected position, using scene depth (t2, device depth
// in .r). Rows of the reprojection matrix (this clip -> previous clip) are
// params0, shadowTint, highlightTint, grade; temporal = (history weight,
// history valid). History is clipped to the 3x3 neighbourhood's colour
// spread (YCoCg), which keeps moving and animated surfaces from ghosting
// without per-object motion vectors. Blending happens on luminance-weighted
// colour so bright specks do not dominate.
float3 TemporalToYCoCg(float3 c)
{
    return float3(dot(c, float3(0.25, 0.5, 0.25)), dot(c, float3(0.5, 0.0, -0.5)),
        dot(c, float3(-0.25, 0.5, -0.25)));
}

float3 TemporalFromYCoCg(float3 c)
{
    return float3(c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z);
}

float3 TemporalCompress(float3 c) { return c / (1.0 + dot(c, LuminanceWeights)); }
float3 TemporalExpand(float3 c) { return c / max(1.0 - dot(c, LuminanceWeights), 1e-4); }

// Temporal upscaling: the scene (t0, depth t2) is smaller than the history
// and output (t1). Each output pixel rebuilds this frame's colour from the
// 3x3 scene samples around it, placed where the jitter (temporal.zw, render
// pixels) put them, with a Gaussian of about half a render pixel (an
// approximation of the Blackman-Harris filter). A sample landing close to
// the output pixel centre gets more weight against the history, so the
// jitter sequence fills in detail finer than the render grid over frames.
// History clipping and reprojection work as in TemporalResolvePS.
float4 TemporalUpscale(VertexOutput input, int2 renderSize, int2 outputSize)
{
    const float2 outputPosition = input.position.xy; // pixel centre, output pixels
    const float2 toRender = float2(renderSize) / float2(outputSize);
    const float2 renderPosition = outputPosition * toRender;
    const float2 jitter = temporal.zw;
    // The sample at render pixel i shows the image at i + 0.5 - jitter.
    const int2 nearest = int2(floor(renderPosition + jitter));
    float3 weighted = 0.0, mean = 0.0, meanSquared = 0.0;
    float weightSum = 0.0, nearestDistance = 1e9, alpha = 1.0;
    float closestDepth = 0.0; // reverse-Z: larger is nearer
    int2 closestPixel = clamp(nearest, int2(0, 0), renderSize - 1);
    [unroll] for (int y = -1; y <= 1; ++y)
    [unroll] for (int x = -1; x <= 1; ++x)
    {
        const int2 at = clamp(nearest + int2(x, y), int2(0, 0), renderSize - 1);
        const float4 raw = sceneColor.Load(int3(at, 0));
        const float3 c = TemporalToYCoCg(TemporalCompress(min(max(raw.rgb, 0.0), 32768.0)));
        const float2 offset = renderPosition - (float2(at) + 0.5 - jitter);
        const float distanceSquared = dot(offset, offset);
        const float weight = exp(-2.29 * distanceSquared);
        weighted += c * weight;
        weightSum += weight;
        mean += c;
        meanSquared += c * c;
        if (distanceSquared < nearestDistance) { nearestDistance = distanceSquared; alpha = raw.a; }
        const float d = exposureColor.Load(int3(at, 0)).r;
        if (d > closestDepth) { closestDepth = d; closestPixel = at; }
    }
    const float3 current = weighted / max(weightSum, 1e-4);
    mean /= 9.0;
    const float3 deviation = sqrt(max(meanSquared / 9.0 - mean * mean, 0.0));
    const float3 currentRgb = TemporalExpand(max(TemporalFromYCoCg(current), 0.0));
    if (temporal.y < 0.5)
        return float4(currentRgb, alpha);
    const float2 uv = (float2(closestPixel) + 0.5 - jitter) / float2(renderSize);
    const float4 clip = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, closestDepth, 1.0);
    const float4 previous = float4(dot(params0, clip), dot(shadowTint, clip),
        dot(highlightTint, clip), dot(grade, clip));
    if (abs(previous.w) < 1e-6)
        return float4(currentRgb, alpha);
    const float2 previousNdc = previous.xy / previous.w;
    const float2 outputUv = outputPosition / float2(outputSize);
    const float2 historyUv = outputUv +
        (float2(previousNdc.x * 0.5 + 0.5, 0.5 - previousNdc.y * 0.5) - uv);
    if (any(historyUv < 0.0) || any(historyUv > 1.0))
        return float4(currentRgb, alpha);
    float3 history = TemporalToYCoCg(TemporalCompress(
        max(bloomColor.SampleLevel(sceneSampler, historyUv, 0.0).rgb, 0.0)));
    const float3 extent = deviation * 1.25 + 1e-4;
    const float3 offset = history - mean;
    const float3 units = abs(offset / extent);
    const float largest = max(units.x, max(units.y, units.z));
    if (largest > 1.0) history = mean + offset / largest;
    const float2 motionPixels = (historyUv - outputUv) * float2(outputSize);
    const float feedback = temporal.x * lerp(1.0, 0.8, saturate(length(motionPixels) / 16.0));
    // Confidence: how close the nearest sample fell to this output pixel's
    // centre, in output pixels (1 on it, ~0.6 half a pixel away).
    const float scale = 0.5 * (toRender.x + toRender.y);
    const float confidence = exp(-2.0 * nearestDistance / (scale * scale));
    const float currentWeight = saturate((1.0 - feedback) * (0.3 + 1.7 * confidence));
    const float3 resolved = lerp(history, current, currentWeight);
    return float4(TemporalExpand(max(TemporalFromYCoCg(resolved), 0.0)), alpha);
}

// Render size below the output without TAA: a bilinear stretch of the scene.
float4 UpscalePS(VertexOutput input) : SV_Target
{
    return sceneColor.SampleLevel(sceneSampler, input.uv, 0.0);
}

float4 TemporalResolvePS(VertexOutput input) : SV_Target
{
    const int2 pixel = int2(input.position.xy);
    int width, height;
    sceneColor.GetDimensions(width, height);
    {
        int outputWidth, outputHeight;
        bloomColor.GetDimensions(outputWidth, outputHeight);
        if (outputWidth != width || outputHeight != height)
            return TemporalUpscale(input, int2(width, height), int2(outputWidth, outputHeight));
    }
    const float4 centre = sceneColor.Load(int3(pixel, 0));
    const float3 current = TemporalCompress(min(max(centre.rgb, 0.0), 32768.0));
    float3 mean = 0.0, meanSquared = 0.0;
    float closestDepth = 0.0; // reverse-Z: larger is nearer
    int2 closestPixel = pixel;
    [unroll] for (int y = -1; y <= 1; ++y)
    [unroll] for (int x = -1; x <= 1; ++x)
    {
        const int2 at = clamp(pixel + int2(x, y), int2(0, 0), int2(width - 1, height - 1));
        const float3 c = TemporalToYCoCg(TemporalCompress(
            min(max(sceneColor.Load(int3(at, 0)).rgb, 0.0), 32768.0)));
        mean += c;
        meanSquared += c * c;
        const float d = exposureColor.Load(int3(at, 0)).r;
        if (d > closestDepth) { closestDepth = d; closestPixel = at; }
    }
    mean /= 9.0;
    const float3 deviation = sqrt(max(meanSquared / 9.0 - mean * mean, 0.0));
    if (temporal.y < 0.5)
        return float4(TemporalExpand(current), centre.a);
    // Reproject the nearest surface in the neighbourhood, so edges take the
    // foreground's motion (the background's would smear them).
    const float2 uv = (float2(closestPixel) + 0.5) / float2(width, height);
    const float4 clip = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, closestDepth, 1.0);
    const float4 previous = float4(dot(params0, clip), dot(shadowTint, clip),
        dot(highlightTint, clip), dot(grade, clip));
    if (abs(previous.w) < 1e-6)
        return float4(TemporalExpand(current), centre.a);
    const float2 previousNdc = previous.xy / previous.w;
    const float2 historyUv = (input.position.xy / float2(width, height)) +
        (float2(previousNdc.x * 0.5 + 0.5, 0.5 - previousNdc.y * 0.5) - uv);
    if (any(historyUv < 0.0) || any(historyUv > 1.0))
        return float4(TemporalExpand(current), centre.a);
    float3 history = TemporalToYCoCg(TemporalCompress(
        max(bloomColor.SampleLevel(sceneSampler, historyUv, 0.0).rgb, 0.0)));
    // Clip the history toward the neighbourhood mean into its colour box.
    const float3 extent = deviation * 1.25 + 1e-4;
    const float3 offset = history - mean;
    const float3 units = abs(offset / extent);
    const float largest = max(units.x, max(units.y, units.z));
    if (largest > 1.0) history = mean + offset / largest;
    // Less history when the pixel moved a lot: resampling blurs it.
    const float2 motionPixels = (historyUv - input.position.xy / float2(width, height)) *
        float2(width, height);
    const float feedback = temporal.x * lerp(1.0, 0.8, saturate(length(motionPixels) / 16.0));
    const float3 resolved = lerp(TemporalToYCoCg(current), history, feedback);
    return float4(TemporalExpand(max(TemporalFromYCoCg(resolved), 0.0)), centre.a);
}

// AgX (Troy Sobotka), in the common polynomial fit: a log2 encoding of an
// inset (slightly desaturating) basis, a sigmoid contrast curve, then back to
// linear display values. Bright foliage and sky roll off softly instead of
// clipping to saturated primaries.
float3 AgxContrast(float3 x)
{
    const float3 x2 = x * x;
    const float3 x4 = x2 * x2;
    return 15.5 * x4 * x2 - 40.14 * x4 * x + 31.96 * x4 - 6.868 * x2 * x +
        0.4298 * x2 + 0.1191 * x - 0.00232;
}

float3 AgxToneMap(float3 color)
{
    // Rows here are the reference GLSL matrix's columns (mul(v, M) == M * v there).
    const float3x3 inset = float3x3(
        0.842479062253094, 0.0423282422610123, 0.0423756549057051,
        0.0784335999999992, 0.878468636469772, 0.0784336,
        0.0792237451477643, 0.0791661274605434, 0.879142973793104);
    const float3x3 outset = float3x3(
        1.19687900512017, -0.0528968517574562, -0.0529716355144438,
        -0.0980208811401368, 1.15190312990417, -0.0980434501171241,
        -0.0990297440797205, -0.0989611768448433, 1.15107367264116);
    const float minEv = -12.47393, maxEv = 4.026069;
    float3 encoded = mul(max(color, 1e-10), inset);
    encoded = saturate((clamp(log2(encoded), minEv, maxEv) - minEv) / (maxEv - minEv));
    encoded = AgxContrast(encoded);
    return pow(saturate(mul(encoded, outset)), 2.2);
}

float3 ToneMap(uint op, float3 color)
{
    if (op == 6u)
        return AgxToneMap(color);
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

// FXAA (after Lottes, FXAA 3.11 "console" edge blend plus a sub-pixel pass),
// run on the HDR scene before bloom, exposure and tone mapping so compose
// still happens once. Edges are found on exposed, compressed luma so the
// threshold matches what reaches the screen; params0.w < 0 enables it and
// carries -(edge threshold in thousandths + subpixel share * 0.999).
float FxaaLuma(float3 hdr, float exposureScale)
{
    const float luma = dot(max(hdr, 0.0), LuminanceWeights) * exposureScale;
    return luma / (1.0 + luma);
}

float3 FxaaSample(float2 uv)
{
    return SampleSource(uv);
}

float3 ApplyFxaa(float2 uv, float3 centre, float exposureScale, float encoded)
{
    const float threshold = floor(encoded) * 0.001;
    const float subpixel = frac(encoded) / 0.999;
    float width, height;
    sceneColor.GetDimensions(width, height);
    const float2 texel = 1.0 / float2(width, height);
    const float3 n = FxaaSample(uv + float2(0.0, -texel.y));
    const float3 s = FxaaSample(uv + float2(0.0, texel.y));
    const float3 e = FxaaSample(uv + float2(texel.x, 0.0));
    const float3 w = FxaaSample(uv + float2(-texel.x, 0.0));
    const float lumaM = FxaaLuma(centre, exposureScale);
    const float lumaN = FxaaLuma(n, exposureScale), lumaS = FxaaLuma(s, exposureScale);
    const float lumaE = FxaaLuma(e, exposureScale), lumaW = FxaaLuma(w, exposureScale);
    const float lumaMax = max(lumaM, max(max(lumaN, lumaS), max(lumaE, lumaW)));
    const float lumaMin = min(lumaM, min(min(lumaN, lumaS), min(lumaE, lumaW)));
    const float range = lumaMax - lumaMin;
    // Flat areas (most of the image) leave after five taps.
    [branch] if (range < max(threshold * lumaMax, 0.0312))
        return centre;
    const float3 nw = FxaaSample(uv + float2(-texel.x, -texel.y));
    const float3 ne = FxaaSample(uv + float2(texel.x, -texel.y));
    const float3 sw = FxaaSample(uv + float2(-texel.x, texel.y));
    const float3 se = FxaaSample(uv + float2(texel.x, texel.y));
    const float lumaNW = FxaaLuma(nw, exposureScale), lumaNE = FxaaLuma(ne, exposureScale);
    const float lumaSW = FxaaLuma(sw, exposureScale), lumaSE = FxaaLuma(se, exposureScale);

    // Edge direction from the diagonals; blend two and four taps along it
    // and keep the wider blend unless it overshoots the local range.
    float2 direction = float2(-((lumaNW + lumaNE) - (lumaSW + lumaSE)),
        (lumaNW + lumaSW) - (lumaNE + lumaSE));
    const float reduce = max((lumaNW + lumaNE + lumaSW + lumaSE) * 0.03125, 1.0 / 128.0);
    const float scale = 1.0 / (min(abs(direction.x), abs(direction.y)) + reduce);
    direction = clamp(direction * scale, -8.0, 8.0) * texel;
    const float3 blendA = 0.5 * (FxaaSample(uv + direction * (1.0 / 3.0 - 0.5)) +
        FxaaSample(uv + direction * (2.0 / 3.0 - 0.5)));
    const float3 blendB = blendA * 0.5 + 0.25 * (FxaaSample(uv - direction * 0.5) +
        FxaaSample(uv + direction * 0.5));
    const float lumaB = FxaaLuma(blendB, exposureScale);
    float3 result = (lumaB < lumaMin || lumaB > lumaMax) ? blendA : blendB;

    // Sub-pixel: a lone pixel unlike its neighbourhood (a blade thinner than
    // a pixel) moves toward the neighbourhood average.
    const float lumaAverage = ((lumaN + lumaS + lumaE + lumaW) * 2.0 +
        (lumaNW + lumaNE + lumaSW + lumaSE)) / 12.0;
    float amount = saturate(abs(lumaAverage - lumaM) / range);
    amount = amount * amount * (3.0 - 2.0 * amount);
    amount = amount * amount * subpixel * 0.75;
    const float3 average = ((n + s + e + w) * 2.0 + (nw + ne + sw + se)) / 12.0;
    return lerp(result, average, amount);
}

float4 PSMain(VertexOutput input) : SV_Target
{
    const float4 source = sceneColor.Sample(sceneSampler, input.uv);
    const uint op = asuint(params0.y);
    if (op == 4u)
        return source * params0.w;
    float3 color = max(source.rgb, 0.0);
    // Sharpening after TAA (temporal.z): unsharp mask against the four
    // neighbours, on compressed colour so bright edges do not ring.
    [branch] if (temporal.z > 0.0)
    {
        float width, height;
        sceneColor.GetDimensions(width, height);
        const float2 texel = 1.0 / float2(width, height);
        const float3 neighbours = (TemporalCompress(SampleSource(input.uv + float2(texel.x, 0.0))) +
            TemporalCompress(SampleSource(input.uv - float2(texel.x, 0.0))) +
            TemporalCompress(SampleSource(input.uv + float2(0.0, texel.y))) +
            TemporalCompress(SampleSource(input.uv - float2(0.0, texel.y)))) * 0.25;
        const float3 centre = TemporalCompress(min(color, 32768.0));
        color = TemporalExpand(max(centre + (centre - neighbours) * temporal.z, 0.0));
    }
    [branch] if (params0.w < 0.0)
    {
        const float autoScale = grade.w > 0.5
            ? 0.18 * exp2(-exposureColor.Load(int3(0, 0, 0)).r) : 1.0;
        color = ApplyFxaa(input.uv, min(color, 32768.0),
            max(params0.x, 0.0) * autoScale, -params0.w);
    }
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

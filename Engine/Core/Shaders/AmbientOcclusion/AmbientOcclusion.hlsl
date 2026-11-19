// Screen-space ambient occlusion: ground-truth AO (GTAO, Jimenez et al.
// 2016) at half resolution, accumulated over frames.
//
// CSMain runs after the main view's opaque draws, from a copy of the scene
// depth (IGraphicsContext::CopySceneDepth). Per half-resolution pixel it
// rebuilds the view-space position and normal from depth, finds the horizon
// on both sides of two screen-space slices (rotated per pixel and per frame)
// within `radius` metres, and integrates the cosine-weighted visible arc
// against the normal projected into each slice. The result is blended with
// the previous frame's AO, reprojected through the camera stored in the
// history buffer's header and rejected where the stored depth disagrees.
//
// Output buffer (uint per element, also read by Object.hlsl next frame):
//   [0..15]  header: camera position, x scale (proj[0][0]), right axis,
//            y scale (proj[1][1]), up axis, valid (1/0), forward axis,
//            half-resolution width | height << 16 (floats as asuint)
//   [16 + y * width + x]  f16 AO | f16 view depth (0 = sky) << 16
//
// Buffers: t0 depth (float per full-resolution pixel, row pitch
// extent.w), t1 previous output (history), u0 output. Vulkan: binding 0 =
// t0, 1 = u0, 2 = t1.

#ifdef VULKAN
struct AmbientOcclusionConstants
{
    float4 cameraPositionScaleX;
    float4 cameraRightScaleY;
    float4 cameraUpDepthA;
    float4 cameraForwardDepthB;
    uint4 extent;
    float4 params;
};
[[vk::push_constant]] AmbientOcclusionConstants cb;
#define cameraPositionScaleX cb.cameraPositionScaleX
#define cameraRightScaleY cb.cameraRightScaleY
#define cameraUpDepthA cb.cameraUpDepthA
#define cameraForwardDepthB cb.cameraForwardDepthB
#define extent cb.extent
#define params cb.params
[[vk::binding(0, 0)]] StructuredBuffer<float> sceneDepth;
[[vk::binding(1, 0)]] RWStructuredBuffer<uint> occlusionOutput;
[[vk::binding(2, 0)]] StructuredBuffer<uint> occlusionHistory;
#else
// cameraPositionScaleX: camera position, proj[0][0]
// cameraRightScaleY:    right axis, proj[1][1]
// cameraUpDepthA:       up axis, A; cameraForwardDepthB: forward axis, B;
//                       view depth w = B / (depth - A)
// extent: full width, full height, half width | half height << 16, depth
//         row pitch
// params: radius (m), intensity (AO exponent), frame index, weight of this
//         frame against history
cbuffer AmbientOcclusionCB : register(b0)
{
    float4 cameraPositionScaleX;
    float4 cameraRightScaleY;
    float4 cameraUpDepthA;
    float4 cameraForwardDepthB;
    uint4 extent;
    float4 params;
};
StructuredBuffer<float> sceneDepth : register(t0);
RWStructuredBuffer<uint> occlusionOutput : register(u0);
StructuredBuffer<uint> occlusionHistory : register(t1);
#endif

static const uint HeaderSize = 16u;
static const float Pi = 3.14159265;
static const uint SliceCount = 2u;
static const uint StepCount = 6u;

uint2 FullExtent() { return extent.xy; }
uint2 HalfExtent() { return uint2(extent.z & 0xFFFFu, extent.z >> 16); }

float DepthAt(int2 pixel)
{
    pixel = clamp(pixel, int2(0, 0), int2(FullExtent()) - 1);
    return sceneDepth[pixel.y * extent.w + pixel.x];
}

// View space: x right, y up, z forward (= view depth w).
float3 ViewPosition(float2 pixel, float depth)
{
    const float2 size = float2(FullExtent());
    const float2 ndc = float2(pixel.x / size.x * 2.0 - 1.0, 1.0 - pixel.y / size.y * 2.0);
    const float w = cameraForwardDepthB.w / (depth - cameraUpDepthA.w);
    return float3(ndc.x * w / cameraPositionScaleX.w, ndc.y * w / cameraRightScaleY.w, w);
}

float InterleavedGradientNoise(float2 pixel)
{
    return frac(52.9829189 * frac(dot(pixel, float2(0.06711056, 0.00583715))));
}

uint PackOcclusion(float ao, float viewDepth)
{
    return f32tof16(saturate(ao)) | (f32tof16(max(viewDepth, 0.0)) << 16);
}

float2 UnpackOcclusion(uint packed)
{
    return float2(f16tof32(packed & 0xFFFFu), f16tof32(packed >> 16));
}

// Cosine-weighted visible arc from the slice normal angle n to horizon h.
float IntegrateArc(float h, float n)
{
    return 0.25 * (-cos(2.0 * h - n) + cos(n) + 2.0 * h * sin(n));
}

float ComputeOcclusion(float2 pixel, float depth)
{
    const float3 position = ViewPosition(pixel, depth);
    const float3 view = normalize(-position);

    // Normal from the flatter neighbour on each axis (two full-resolution
    // pixels away, one half-resolution pixel).
    const float3 left = ViewPosition(pixel - float2(2.0, 0.0), DepthAt(int2(pixel) - int2(2, 0)));
    const float3 right = ViewPosition(pixel + float2(2.0, 0.0), DepthAt(int2(pixel) + int2(2, 0)));
    const float3 up = ViewPosition(pixel - float2(0.0, 2.0), DepthAt(int2(pixel) - int2(0, 2)));
    const float3 down = ViewPosition(pixel + float2(0.0, 2.0), DepthAt(int2(pixel) + int2(0, 2)));
    const float3 dx = abs(right.z - position.z) < abs(position.z - left.z)
        ? right - position : position - left;
    const float3 dy = abs(up.z - position.z) < abs(position.z - down.z)
        ? up - position : position - down;
    float3 normal = normalize(cross(dy, dx));
    if (dot(normal, view) < 0.0) normal = -normal;

    const float radius = max(params.x, 0.01);
    const float radiusSquared = radius * radius;
    // Radius in full-resolution pixels at this depth.
    const float pixelsPerMetre = cameraPositionScaleX.w * 0.5 * float(FullExtent().x) / position.z;
    const float screenRadius = min(radius * pixelsPerMetre, 256.0);
    if (screenRadius < 2.0)
        return 1.0;

    const float noise = InterleavedGradientNoise(pixel + params.z * 5.588238);
    const float jitter = InterleavedGradientNoise(pixel.yx * 1.37 + params.z * 3.1);
    // No break/continue in these loops: fxc has compiled such early exits
    // inside unrolled loops into a GPU hang.
    float visibility = 0.0;
    [unroll] for (uint slice = 0u; slice < SliceCount; ++slice)
    {
        const float phi = (float(slice) + noise) * (Pi / float(SliceCount));
        const float2 direction = float2(cos(phi), sin(phi)); // screen, y down
        const float3 sliceDirection = float3(direction.x, -direction.y, 0.0);
        const float3 axis = normalize(cross(sliceDirection, view));
        const float3 ortho = sliceDirection - dot(sliceDirection, view) * view;
        const float3 projected = normal - axis * dot(normal, axis);
        const float projectedLength = max(length(projected), 1e-4);
        const float cosN = clamp(dot(projected, view) / projectedLength, -1.0, 1.0);
        const float n = (dot(ortho, projected) < 0.0 ? -1.0 : 1.0) * acos(cosN);
        float arcs = 0.0;
        [unroll] for (uint sideIndex = 0u; sideIndex < 2u; ++sideIndex)
        {
            const float side = sideIndex == 0u ? -1.0 : 1.0;
            const float lowest = cos(n + side * 0.5 * Pi);
            float horizon = lowest;
            [unroll] for (uint stepIndex = 0u; stepIndex < StepCount; ++stepIndex)
            {
                float t = (float(stepIndex) + jitter) / float(StepCount);
                t *= t;
                const float offsetPixels = max(t * screenRadius, float(stepIndex) + 1.0);
                const float2 samplePixel = pixel + direction * (side * offsetPixels);
                const bool inside = all(samplePixel >= 0.0) &&
                    all(samplePixel < float2(FullExtent()));
                const float sampleDepth = inside ? DepthAt(int2(samplePixel)) : 0.0;
                // Off screen and sky samples leave the horizon open.
                const float3 delta = ViewPosition(floor(samplePixel) + 0.5,
                    max(sampleDepth, 1e-6)) - position;
                const float lengthSquared = dot(delta, delta);
                const float cosine = dot(delta, view) * rsqrt(max(lengthSquared, 1e-8));
                const float falloff = sampleDepth > 0.0
                    ? saturate(1.0 - lengthSquared / radiusSquared) : 0.0;
                horizon = max(horizon, lerp(lowest, cosine, falloff));
            }
            float h = side * acos(clamp(horizon, -1.0, 1.0));
            h = n + clamp(h - n, -0.5 * Pi, 0.5 * Pi);
            arcs += IntegrateArc(h, n);
        }
        visibility += projectedLength * arcs;
    }
    visibility /= float(SliceCount);
    return pow(saturate(visibility), max(params.y, 0.0));
}

[numthreads(64, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    const uint2 halfExtent = HalfExtent();
    if (id.x == 0u)
    {
        occlusionOutput[0] = asuint(cameraPositionScaleX.x);
        occlusionOutput[1] = asuint(cameraPositionScaleX.y);
        occlusionOutput[2] = asuint(cameraPositionScaleX.z);
        occlusionOutput[3] = asuint(cameraPositionScaleX.w);
        occlusionOutput[4] = asuint(cameraRightScaleY.x);
        occlusionOutput[5] = asuint(cameraRightScaleY.y);
        occlusionOutput[6] = asuint(cameraRightScaleY.z);
        occlusionOutput[7] = asuint(cameraRightScaleY.w);
        occlusionOutput[8] = asuint(cameraUpDepthA.x);
        occlusionOutput[9] = asuint(cameraUpDepthA.y);
        occlusionOutput[10] = asuint(cameraUpDepthA.z);
        occlusionOutput[11] = asuint(1.0);
        occlusionOutput[12] = asuint(cameraForwardDepthB.x);
        occlusionOutput[13] = asuint(cameraForwardDepthB.y);
        occlusionOutput[14] = asuint(cameraForwardDepthB.z);
        occlusionOutput[15] = extent.z;
    }
    if (id.x >= halfExtent.x * halfExtent.y)
        return;
    const uint2 halfPixel = uint2(id.x % halfExtent.x, id.x / halfExtent.x);
    const float2 pixel = float2(halfPixel * 2u) + 1.0;
    const float depth = DepthAt(int2(pixel));
    if (depth <= 0.0)
    {
        occlusionOutput[HeaderSize + id.x] = PackOcclusion(1.0, 0.0);
        return;
    }
    const float3 position = ViewPosition(pixel, depth);
    float ao = ComputeOcclusion(pixel, depth);

    // Temporal accumulation through the history's camera.
    [branch] if (params.w < 1.0 && asfloat(occlusionHistory[11]) > 0.5)
    {
        const float3 world = cameraPositionScaleX.xyz + cameraRightScaleY.xyz * position.x +
            cameraUpDepthA.xyz * position.y + cameraForwardDepthB.xyz * position.z;
        const float3 historyCamera = asfloat(uint3(occlusionHistory[0], occlusionHistory[1],
            occlusionHistory[2]));
        const float3 historyRight = asfloat(uint3(occlusionHistory[4], occlusionHistory[5],
            occlusionHistory[6]));
        const float3 historyUp = asfloat(uint3(occlusionHistory[8], occlusionHistory[9],
            occlusionHistory[10]));
        const float3 historyForward = asfloat(uint3(occlusionHistory[12], occlusionHistory[13],
            occlusionHistory[14]));
        const uint historyPacked = occlusionHistory[15];
        const uint2 historyExtent = uint2(historyPacked & 0xFFFFu, historyPacked >> 16);
        const float3 offset = world - historyCamera;
        const float w = dot(offset, historyForward);
        if (w > 0.01 && all(historyExtent > 0u))
        {
            const float2 ndc = float2(asfloat(occlusionHistory[3]) * dot(offset, historyRight),
                asfloat(occlusionHistory[7]) * dot(offset, historyUp)) / w;
            const float2 uv = ndc * float2(0.5, -0.5) + 0.5;
            const float2 texel = uv * float2(historyExtent) - 0.5;
            const int2 base = int2(floor(texel));
            const float2 f = texel - float2(base);
            float history = 0.0, weight = 0.0;
            [unroll] for (uint tap = 0u; tap < 4u; ++tap)
            {
                const int2 offsetTexel = int2(tap & 1u, tap >> 1);
                const int2 unclamped = base + offsetTexel;
                const bool inside = all(unclamped >= 0) && all(unclamped < int2(historyExtent));
                const int2 coordinate = clamp(unclamped, int2(0, 0), int2(historyExtent) - 1);
                const float2 stored = UnpackOcclusion(occlusionHistory[HeaderSize +
                    coordinate.y * historyExtent.x + coordinate.x]);
                const float bilinear = (offsetTexel.x ? f.x : 1.0 - f.x) *
                    (offsetTexel.y ? f.y : 1.0 - f.y);
                // Same surface: stored depth within a few percent.
                const bool match = inside && abs(stored.y - w) <= 0.03 * w + 0.05;
                weight += match ? bilinear : 0.0;
                history += match ? bilinear * stored.x : 0.0;
            }
            if (weight > 0.25)
                ao = lerp(history / weight, ao, params.w);
        }
    }
    occlusionOutput[HeaderSize + id.x] = PackOcclusion(ao, position.z);
}

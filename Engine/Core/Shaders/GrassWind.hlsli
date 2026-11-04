// Root-pinned bending shared by the color, depth and shadow vertex paths.
// Shared settings for every grass layer. Bounds reserve 1.5 times blade height
// for the lateral bend and lowering of tips at the 85-degree limit.
static const float GrassWindStrength = 1.0;
static const float GrassWindMaximumBendDegrees = 85.0;
static const float GrassWindRootStiffnessHeight = 0.18; // metres
static const float GrassWindSpeed = 2;
static const float GrassWindDirectionDegrees = 35.0;
static const float GrassWindWaveSize = 32.0; // metres; broad, slowly passing fronts
static const uint GrassWindSeed = 18731u;
uint GrassWindHash(int2 cell, uint seed)
{
    uint hash = asuint(cell.x) * 0x8da6b343u ^ asuint(cell.y) * 0xd8163841u ^ seed;
    hash ^= hash >> 16;
    hash *= 0x7feb352du;
    hash ^= hash >> 15;
    hash *= 0x846ca68bu;
    return hash ^ (hash >> 16);
}

float GrassWindGradient(int2 cell, float2 offset, uint seed)
{
    const float2 gradients[8] = {
        float2(1, 0), float2(-1, 0), float2(0, 1), float2(0, -1),
        float2(.70710678, .70710678), float2(-.70710678, .70710678),
        float2(.70710678, -.70710678), float2(-.70710678, -.70710678)};
    return dot(gradients[GrassWindHash(cell, seed) & 7u], offset);
}

// Seeded gradient Perlin noise: integer lattice gradients and quintic fade
// keep both value and slope continuous across lattice/cell boundaries.
float GrassWindPerlin(float2 position, uint seed)
{
    int2 cell = int2(floor(position));
    float2 offset = frac(position);
    float2 fade = offset * offset * offset * (offset * (offset * 6.0 - 15.0) + 10.0);
    float bottom = lerp(GrassWindGradient(cell, offset, seed),
        GrassWindGradient(cell + int2(1, 0), offset - float2(1, 0), seed), fade.x);
    float top = lerp(GrassWindGradient(cell + int2(0, 1), offset - float2(0, 1), seed),
        GrassWindGradient(cell + int2(1, 1), offset - float2(1, 1), seed), fade.x);
    return clamp(1.41421356 * lerp(bottom, top, fade.y), -1.0, 1.0);
}

void ApplyGrassWind(float sceneTime,
    float3 root, float3 up, float height,
    inout float3 position, inout float3 normal, inout float3 tangent)
{
    float direction = radians(GrassWindDirectionDegrees);
    float4 wind = float4(cos(direction), sin(direction),
        saturate(GrassWindStrength), sceneTime * max(0.0, GrassWindSpeed));
    float noiseScale = 1.0 / max(1.0, GrassWindWaveSize);
    uint seed = GrassWindSeed;
    if (wind.z <= 0.0 || height <= 0.0) return;
    float2 side = float2(-wind.y, wind.x);
    // Advect a shared world-space field downwind. Stretch across the wind
    // to form broad fronts rather than independently shaking each patch.
    float2 traveling = root.xz - wind.xy * wind.w * 2.0;
    float2 coordinate = float2(dot(traveling, wind.xy), dot(traveling, side) * .25)
        * max(noiseScale, .0001);
    float broad = GrassWindPerlin(coordinate, seed);
    float detail = GrassWindPerlin(coordinate * 1.35 + float2(17.2, 3.7 + wind.w * .025),
        seed ^ 0x9e3779b9u);
    // Signed, zero-centered gusts let each passing wave lean the grass both
    // ways and return through upright, instead of imposing a permanent lean.
    float gust = clamp(2.4 * (broad * .90 + detail * .10), -1.0, 1.0);
    float crossSway = .35 * detail;
    float2 heading = wind.xy + side * crossSway;
    float3 bendDirection = float3(heading.x, 0.0, heading.y);
    bendDirection -= up * dot(bendDirection, up);
    float lateralLength = length(bendDirection);
    if (lateralLength < .0001) return;
    bendDirection /= lateralLength;
    // Rotate each height segment toward the wind instead of stretching it
    // sideways. Tall tips lower toward the ground; roots stay fixed and stiff.
    float stiffness = max(.01, GrassWindRootStiffnessHeight);
    float maximumAngle = radians(clamp(GrassWindMaximumBendDegrees, 0.0, 85.0)) * wind.z * gust;
    float angle = maximumAngle * height / (height + stiffness);
    float angleDerivative = maximumAngle * stiffness / ((height + stiffness) * (height + stiffness));
    float sine, cosine;
    sincos(angle, sine, cosine);
    position += height * (bendDirection * sine + up * (cosine - 1.0));
    float3 slope = bendDirection * (sine + height * cosine * angleDerivative) +
        up * (cosine - 1.0 - height * sine * angleDerivative);
    normal = normalize(normal - up * (dot(slope, normal) / (1.0 + dot(slope, up))));
    tangent = normalize(tangent + slope * dot(up, tangent));
}

// GPU grass blade generation (.md/todo.md "Next" 2; Rendering/GpuGrassBlades.h).
//
// Every frame, one group per patch of a camera-centred grid writes the visible
// blades of that patch into a GPU grass carrier's visible list (record 0 holds
// the indexed draw arguments, as InstanceCull.hlsl writes them). Blades come
// from a deterministic hash of the patch and blade index, so a blade is the
// same blade every frame and nothing is built on the CPU:
//  * position: uniform in the patch; height and grass paint sampled from the
//    cell's grids (bilinear, as the CPU grass samples them);
//  * which blades: a patch holds up to maxPerPatch blades at full density;
//    blade i is kept while its rank (i + 0.5) / maxPerPatch is under the
//    share at its distance, (keepDistance / d)^2 past keepDistance, and the
//    survivors widen by 1 / sqrt(share) (Ghost of Tsushima's fewer, wider
//    blades with distance; the last kept blade grows in, so nothing pops);
//  * density: short blades thin under the tall grass fields, tall blades grow
//    in them (the same field noise as GrassField::TallField, in cell-local
//    coordinates, so the fields line up with the far ring's tall tufts);
//  * clumps: a Voronoi cell per blade (nearest jittered point of a clumpSize
//    grid) gives height, lean and hue shared by its blades;
//  * culling: patches outside the view frustum or behind the camera's
//    occlusion buffer (as InstanceCull) are skipped whole; blades fade out by
//    shrinking toward the root between fadeStart and the reach.
//
// fieldData (floats): header [0..63], cell table [64..319] (64 cells x
// origin x, origin z, base y, data offset), then per cell a samples x samples
// height grid (relative to base y) followed by a grid of grass paint weight.
// Header: 0 cell count, 1 samples per side, 2 sample spacing (m), 3 patch size
// (m), 4 short blades per m2, 5 tall blades per m2 (in a field), 6 field size,
// 7 field coverage, 8 short share under tall, 9 seed, 10 keep distance, 11 fade
// start, 12 clump size, 13 max blades per patch, 14-15 short height range,
// 16-17 tall height range, 18 short width, 19 tall width, 20 dry share,
// 21-22 paint weight range (none .. all), 23 tussock size (m), 24 pocket width
// (tussock sizes from the border of two tussocks), 25 short blades thinned at
// a border, 26 darkening there, 27 short height lost there.
//
// Record (48 bytes, read by Assets/Shaders/GrassBlades.hlsl):
//  row0: object slot, root x, y, z (float bits)
//  row1: height | width (half), yaw | bend (half), bytes tall, shade, dry, hue,
//        lean x | z (half)
//  row2: phase (float bits), terrain normal x | z (half), pocket shade (float
//        bits, 1 = none), 0
struct OutputRecord { uint4 row0; uint4 row1; uint4 row2; };
struct BladeConstants
{
    float4x4 viewProjection;
    float4 camera;  // xyz position, w reach
    int4 grid;      // patch origin x, z (in patches), patches per side, mode (1 reset)
    uint4 draw;     // object slot, index count, capacity, flags (1 occlusion)
};
#ifdef VULKAN
[[vk::push_constant]] BladeConstants constants;
[[vk::binding(0, 0)]] StructuredBuffer<float> fieldData;
[[vk::binding(1, 0)]] RWStructuredBuffer<OutputRecord> outputInstances;
[[vk::binding(2, 0)]] StructuredBuffer<float> fieldDataSecond;
[[vk::binding(3, 0)]] StructuredBuffer<float> occlusionPyramid;
#else
cbuffer Constants : register(b0) { BladeConstants constants; };
StructuredBuffer<float> fieldData : register(t0);
RWStructuredBuffer<OutputRecord> outputInstances : register(u0);
StructuredBuffer<float> fieldDataSecond : register(t1);
StructuredBuffer<float> occlusionPyramid : register(t2);
#endif

static const uint CellTable = 64u;

uint Avalanche(uint hash)
{
    hash ^= hash >> 16; hash *= 0x7feb352du;
    hash ^= hash >> 15; hash *= 0x846ca68bu;
    return hash ^ (hash >> 16);
}
uint Hash3(int a, int b, uint c)
{
    return Avalanche(asuint(a) * 0x8da6b343u ^ asuint(b) * 0xd8163841u ^ c);
}
float Unit(uint hash) { return (float)(hash & 0x00ffffffu) / 16777216.0; }

// GrassField::ThicketNoise, value for value.
float ThicketValue(float x, float z, float period, uint salt, uint seed)
{
    const float u = x / period, v = z / period;
    const float fx = floor(u), fz = floor(v);
    const int ix = (int)fx, iz = (int)fz;
    float tx = u - fx, tz = v - fz;
    tx = tx * tx * (3.0 - 2.0 * tx); tz = tz * tz * (3.0 - 2.0 * tz);
    float corners[4];
    [unroll] for (uint corner = 0u; corner < 4u; ++corner)
    {
        uint h = (uint)(ix + (int)(corner & 1u)) * 0x8da6b343u ^
            (uint)(iz + (int)(corner >> 1u)) * 0xd8163841u ^ seed ^ salt;
        corners[corner] = (float)(Avalanche(h) & 0xffffu) / 65535.0;
    }
    return lerp(lerp(corners[0], corners[1], tx), lerp(corners[2], corners[3], tx), tz);
}
float ThicketNoise(float x, float z, float size, float coverage, uint variant, uint seed)
{
    const float thicket = max(1.0, size);
    const float n = .68 * ThicketValue(x, z, thicket, 0x5b1d7a3u ^ variant, seed) +
        .32 * ThicketValue(x, z, thicket * .37, 0x2f6e2b1u ^ variant, seed);
    const float threshold = .5 + (.5 - saturate(coverage)) * .5;
    const float t = saturate((n - (threshold - .04)) / .14);
    return t * t * (3.0 - 2.0 * t);
}

struct Cell { uint offset; float2 origin; float baseY; float extent; uint samples; float spacing; };
bool FindCell(float2 position, out Cell cell)
{
    cell.samples = (uint)fieldData[1];
    cell.spacing = fieldData[2];
    cell.extent = (float)(cell.samples - 1u) * cell.spacing;
    cell.offset = 0u; cell.origin = 0.0; cell.baseY = 0.0;
    const uint count = (uint)fieldData[0];
    [loop] for (uint index = 0u; index < count; ++index)
    {
        const uint entry = CellTable + index * 4u;
        const float2 origin = float2(fieldData[entry], fieldData[entry + 1u]);
        const float2 local = position - origin;
        if (all(local >= 0.0) && all(local < cell.extent))
        {
            cell.origin = origin;
            cell.baseY = fieldData[entry + 2u];
            cell.offset = (uint)fieldData[entry + 3u];
            return true;
        }
    }
    return false;
}
float SampleGrid(Cell cell, uint grid, float2 local)
{
    const float2 g = clamp(local / cell.spacing, 0.0, (float)(cell.samples - 1u) - 0.001);
    const uint2 i = (uint2)floor(g);
    const float2 f = g - (float2)i;
    const uint base = cell.offset + grid * cell.samples * cell.samples;
    const uint row0 = base + i.y * cell.samples + i.x, row1 = row0 + cell.samples;
    return lerp(lerp(fieldData[row0], fieldData[row0 + 1u], f.x),
        lerp(fieldData[row1], fieldData[row1 + 1u], f.x), f.y);
}

// InstanceCull.hlsl IsBoxOccluded, unchanged.
bool IsBoxOccluded(float3 center, float3 extent)
{
    const uint levels = (uint)occlusionPyramid[0];
    if (levels == 0u) return false;
    const float width = occlusionPyramid[1], height = occlusionPyramid[2];
    float4x4 viewProjection;
    [unroll] for (uint row = 0u; row < 4u; ++row)
        viewProjection[row] = float4(occlusionPyramid[64u + row * 4u], occlusionPyramid[65u + row * 4u],
            occlusionPyramid[66u + row * 4u], occlusionPyramid[67u + row * 4u]);
    float2 low = float2(3.0e38, 3.0e38), high = float2(-3.0e38, -3.0e38);
    float nearest = 3.0e38;
    [unroll] for (uint corner = 0u; corner < 8u; ++corner)
    {
        const float3 offset = float3((corner & 1u) ? 1.0 : -1.0, (corner & 2u) ? 1.0 : -1.0,
            (corner & 4u) ? 1.0 : -1.0);
        const float4 clip = mul(viewProjection, float4(center + extent * offset, 1.0));
        if (clip.w <= 0.05) return false;
        const float2 screen = float2((clip.x / clip.w * 0.5 + 0.5) * width,
            (0.5 - clip.y / clip.w * 0.5) * height);
        low = min(low, screen); high = max(high, screen);
        nearest = min(nearest, clip.w);
    }
    if (high.x < 0.0 || high.y < 0.0 || low.x >= width || low.y >= height) return false;
    const int2 first = clamp(int2(floor(low)), int2(0, 0), int2(width - 1.0, height - 1.0));
    const int2 last = clamp(int2(floor(high)), int2(0, 0), int2(width - 1.0, height - 1.0));
    uint level = 0u;
    [loop] while (level + 1u < levels &&
        ((last.x >> level) - (first.x >> level) > 1 || (last.y >> level) - (first.y >> level) > 1))
        ++level;
    const uint offset = (uint)occlusionPyramid[4u + level * 3u];
    const int levelWidth = (int)occlusionPyramid[5u + level * 3u];
    const int levelHeight = (int)occlusionPyramid[6u + level * 3u];
    float farthest = 0.0;
    [loop] for (int y = first.y >> level; y <= min(levelHeight - 1, last.y >> level); ++y)
        [loop] for (int x = first.x >> level; x <= min(levelWidth - 1, last.x >> level); ++x)
            farthest = max(farthest, occlusionPyramid[offset + (uint)(y * levelWidth + x)]);
    if (farthest >= 3.0e38) return false;
    return nearest > farthest * 1.002 + 0.05;
}

float KeepShare(float distance, float keepDistance)
{
    return distance <= keepDistance ? 1.0 : (keepDistance * keepDistance) / (distance * distance);
}

uint PackHalf2(float a, float b) { return f32tof16(a) | (f32tof16(b) << 16); }

[numthreads(64, 1, 1)]
void CSMain(uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID)
{
    if (constants.grid.w == 1)
    {
        if (group.x == 0u && thread.x == 0u)
        {
            OutputRecord args;
            args.row0 = uint4(constants.draw.y, 0u, 0u, 0u);
            args.row1 = 0u; args.row2 = 0u;
            outputInstances[0] = args;
        }
        return;
    }
    const int side = constants.grid.z;
    const int px = constants.grid.x + (int)(group.x % (uint)side);
    const int pz = constants.grid.y + (int)(group.x / (uint)side);
    const float patch = fieldData[3];
    const float2 low = float2(px, pz) * patch;
    const float2 centre = low + patch * 0.5;
    const float3 camera = constants.camera.xyz;
    const float reach = constants.camera.w;
    if (length(centre - camera.xz) - patch * 0.7072 > reach) return;
    Cell cell;
    if (!FindCell(centre, cell)) return;
    // Patch box from its corner heights and the tallest blade.
    float lowY = 3.0e38, highY = -3.0e38;
    [unroll] for (uint corner = 0u; corner < 4u; ++corner)
    {
        const float2 at = low + float2((corner & 1u) ? patch : 0.0, (corner & 2u) ? patch : 0.0);
        const float y = cell.baseY + SampleGrid(cell, 0u, at - cell.origin);
        lowY = min(lowY, y); highY = max(highY, y);
    }
    const float tallest = max(fieldData[15], fieldData[17]) * 1.3;
    const float3 boxCentre = float3(centre.x, (lowY + highY + tallest) * 0.5, centre.y);
    const float3 boxExtent = float3(patch * 0.5 + 0.3, (highY + tallest - lowY) * 0.5 + 0.1, patch * 0.5 + 0.3);
    float4 rows[4];
    [unroll] for (uint row = 0u; row < 4u; ++row) rows[row] = constants.viewProjection[row];
    const float4 planes[6] = { rows[3] + rows[0], rows[3] - rows[0], rows[3] + rows[1],
        rows[3] - rows[1], rows[2], rows[3] - rows[2] };
    [unroll] for (uint plane = 0u; plane < 6u; ++plane)
        if (dot(planes[plane].xyz, boxCentre) + planes[plane].w < -dot(abs(planes[plane].xyz), boxExtent)) return;
    if ((constants.draw.w & 1u) != 0u && IsBoxOccluded(boxCentre, boxExtent)) return;

    const uint seed = (uint)fieldData[9];
    const float keepDistance = max(1.0, fieldData[10]);
    const float fadeStart = fieldData[11];
    const uint maxPerPatch = (uint)fieldData[13];
    const float nearest = max(0.0, length(boxCentre - camera) - length(boxExtent));
    const uint count = min(maxPerPatch, (uint)ceil((float)maxPerPatch * KeepShare(nearest, keepDistance)));
    const float shortDensity = fieldData[4], tallDensity = fieldData[5];
    const float maximumDensity = max(1.0e-3, shortDensity + tallDensity);
    const float clumpSize = max(0.1, fieldData[12]);
    const uint patchSeed = Hash3(px, pz, seed ^ 0x6a09e667u);
    [loop] for (uint index = thread.x; index < count; index += 64u)
    {
        uint h = Avalanche(patchSeed ^ (index * 0x9e3779b9u));
        const float2 position = low + float2(Unit(h), Unit(Avalanche(h ^ 0x1u))) * patch;
        const float2 local = position - cell.origin;
        if (any(local < 0.0) || any(local >= cell.extent)) continue;
        const float y = cell.baseY + SampleGrid(cell, 0u, local);
        const float3 root = float3(position.x, y, position.y);
        const float distance = length(root - camera);
        if (distance > reach) continue;
        // Rank thinning with a grow-in for the last kept blade.
        const float share = KeepShare(distance, keepDistance);
        const float rank = ((float)index + 0.5) / (float)maxPerPatch;
        if (rank > share) continue;
        const float growIn = saturate((share - rank) * (float)maxPerPatch);
        // Grass paint.
        const float paint = smoothstep(fieldData[21], fieldData[22], SampleGrid(cell, 1u, local));
        if (Unit(Avalanche(h ^ 0x2u)) >= paint) continue;
        // Short and tall blades by the tall-field noise.
        const float field = ThicketNoise(local.x, local.y, fieldData[6], fieldData[7], 0x51e7a3c9u, seed);
        const float shortHere = shortDensity * (1.0 - (1.0 - fieldData[8]) * field);
        const float tallHere = tallDensity * field;
        const float total = shortHere + tallHere;
        if (Unit(Avalanche(h ^ 0x3u)) * maximumDensity >= total) continue;
        const bool tall = Unit(Avalanche(h ^ 0x4u)) * total < tallHere;
        // Fade out toward the reach by shrinking to the root.
        const float shrink = (1.0 - smoothstep(fadeStart, reach, distance)) * growIn;
        if (shrink <= 0.02) continue;
        // Voronoi clump: the nearest jittered point of the clump grid.
        const int2 clumpCell = int2(floor(position / clumpSize));
        float nearestClump = 3.0e38; uint clumpHash = 0u; float2 clumpPoint = position;
        [unroll] for (int dz = -1; dz <= 1; ++dz)
            [unroll] for (int dx = -1; dx <= 1; ++dx)
            {
                const int2 c = clumpCell + int2(dx, dz);
                const uint ch = Hash3(c.x, c.y, seed ^ 0x3c6ef372u);
                const float2 site = ((float2)c + float2(Unit(ch), Unit(Avalanche(ch)))) * clumpSize;
                const float d2 = dot(site - position, site - position);
                if (d2 < nearestClump) { nearestClump = d2; clumpHash = ch; clumpPoint = site; }
            }
        // Pockets between tussocks (a coarser Voronoi than the clumps): toward
        // the border of two tussocks short blades thin out, stay low and
        // darken, so the ground between them reads as shadowed hollows, as
        // between the mesh grass's tussocks.
        const float tussockSize = max(0.1, fieldData[23]);
        const int2 tussockCell = int2(floor(position / tussockSize));
        float firstTussock = 3.0e38, secondTussock = 3.0e38;
        [unroll] for (int tz = -1; tz <= 1; ++tz)
            [unroll] for (int tx = -1; tx <= 1; ++tx)
            {
                const int2 c = tussockCell + int2(tx, tz);
                const uint th = Hash3(c.x, c.y, seed ^ 0x7f4a7c15u);
                const float2 site = ((float2)c + 0.15 + 0.7 * float2(Unit(th), Unit(Avalanche(th)))) * tussockSize;
                const float d2 = dot(site - position, site - position);
                secondTussock = min(secondTussock, max(firstTussock, d2));
                firstTussock = min(firstTussock, d2);
            }
        const float border = sqrt(secondTussock) - sqrt(firstTussock);
        const float pocket = 1.0 - smoothstep(0.0, tussockSize * fieldData[24], border);
        if (!tall && Unit(Avalanche(h ^ 0xeu)) < pocket * fieldData[25]) continue;
        const float pocketShade = 1.0 - pocket * fieldData[26];
        const uint c1 = Avalanche(clumpHash ^ 0x5u), c2 = Avalanche(clumpHash ^ 0x6u), c3 = Avalanche(clumpHash ^ 0x7u);
        const float clumpScale = 0.78 + 0.44 * Unit(c1);
        const float leanAngle = Unit(c2) * 6.2831853;
        const float leanAmount = 0.05 + 0.12 * Unit(c3);
        const float hue = Unit(Avalanche(clumpHash ^ 0x8u));
        // Per blade.
        // Short blades skew low: many make a sward that closes over the soil.
        const float heightRandom = Unit(Avalanche(h ^ 0x5u));
        const float heightRange = tall ? heightRandom : pow(heightRandom, 1.6);
        float height = (tall ? lerp(fieldData[16], fieldData[17], heightRange)
                             : lerp(fieldData[14], fieldData[15], heightRange)) * clumpScale * shrink *
            (tall ? 1.0 : 1.0 - fieldData[27] * pocket);
        // Measurement (draw flag 2): zero-height blades cost vertices but no pixels.
        if ((constants.draw.w & 2u) != 0u) height = 0.0;
        float width = (tall ? fieldData[19] : fieldData[18]) * (0.8 + 0.4 * Unit(Avalanche(h ^ 0x6u))) *
            rsqrt(max(share, 0.01)) * lerp(0.6, 1.0, shrink);
        // Blades fan out from their clump's centre.
        const float2 outward = position - clumpPoint;
        const float randomYaw = Unit(Avalanche(h ^ 0x7u)) * 6.2831853;
        const float outwardYaw = atan2(outward.x, outward.y);
        const float yaw = dot(outward, outward) > 1.0e-6 && Unit(Avalanche(h ^ 0x8u)) < 0.6
            ? outwardYaw + (Unit(Avalanche(h ^ 0x9u)) - 0.5) * 1.2 : randomYaw;
        // Clumps are fountains (Ghost of Tsushima): upright at the heart,
        // arching out toward the rim, so the outer blades close over the
        // ground between clumps. Tall blades stay more upright.
        const float bendRandom = Unit(Avalanche(h ^ 0xau));
        const float rim = saturate(sqrt(nearestClump) / (clumpSize * 0.7));
        const float bend = radians((tall ? 4.0 + 26.0 * rim * rim : 5.0 + 38.0 * rim * rim) +
            (bendRandom - 0.5) * 14.0);
        const bool dry = Unit(Avalanche(h ^ 0xbu)) < fieldData[20];
        const float shade = Unit(Avalanche(h ^ 0xcu));
        // Terrain normal from the height grid.
        const float e = cell.spacing;
        const float slopeX = (SampleGrid(cell, 0u, local + float2(e, 0)) - SampleGrid(cell, 0u, local - float2(e, 0))) / (2.0 * e);
        const float slopeZ = (SampleGrid(cell, 0u, local + float2(0, e)) - SampleGrid(cell, 0u, local - float2(0, e))) / (2.0 * e);
        const float3 normal = normalize(float3(-slopeX, 1.0, -slopeZ));
        uint destination;
        InterlockedAdd(outputInstances[0].row0.y, 1u, destination);
        if (destination >= constants.draw.z)
        {
            InterlockedAdd(outputInstances[0].row0.y, 0xffffffffu);
            continue;
        }
        OutputRecord record;
        record.row0 = uint4(constants.draw.x, asuint(root.x), asuint(root.y), asuint(root.z));
        record.row1 = uint4(PackHalf2(height, width), PackHalf2(yaw, bend),
            (tall ? 255u : 0u) | ((uint)(shade * 255.0) << 8) | ((dry ? 255u : 0u) << 16) | ((uint)(hue * 255.0) << 24),
            PackHalf2(cos(leanAngle) * leanAmount, sin(leanAngle) * leanAmount));
        record.row2 = uint4(asuint(dot(position, float2(8.3, 6.1)) + Unit(Avalanche(h ^ 0xdu)) * 6.2831853),
            PackHalf2(normal.x, normal.z), asuint(pocketShade), 0u);
        outputInstances[destination + 1u] = record;
    }
}

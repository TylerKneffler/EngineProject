#include "Core/Rendering/DetailBudget.h"
#include "Scripts/Landscape/LandscapeDriver.h"
#include "Grass/GrassField.h"
#include "Grass/GrassBlades.h"
#include "Scripts/Grass/MeadowGrass.h"
#include "Core/Components/Terrain/TerrainHeightArchive.h"
#include "Core/Components/Terrain/TerrainHydrologyMap.h"
#include "Core/Components/Terrain/TerrainManager.h"
#include "Core/Components/Terrain/TerrainObjectLayer.h"
#include "Core/Components/Terrain/TerrainSurfaceSample.h"
#include "Core/Components/Mesh/Mesh.h"
#include "Core/Components/Materials/Material.h"
#include "Core/Components/Camera/Camera.h"
#include "Core/Scene/Object.h"
#include "Core/Scene/Scene.h"
#include "Core/Serialization/SceneSerializer.h"
#include <cstdlib>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <functional>
#include <memory>
#include <stdexcept>

namespace
{
struct LandscapeDriverRegistration
{
    LandscapeDriverRegistration()
    {
        Engine::Serialization::RegisterComponentType<LandscapeDriver>("LandscapeDriver");
    }
} registration;

using Engine::Components::Terrain;
using Engine::Components::TerrainManager;
using Engine::Components::TerrainObjectLayer;

// Paint layers, in terrain layer order.
enum Layer { Sand, Gravel, Grass, ForestSoil, Rock, Snow, LayerCount };
const std::array<const char*, LayerCount> LayerMaterials{
    "Assets/Materials/Terrain/PBR/sand.material",
    "Assets/Materials/Terrain/PBR/gravel.material",
    "Assets/Materials/Terrain/PBR/grass.material",
    "Assets/Materials/Terrain/PBR/forest_soil.material",
    "Assets/Materials/Terrain/PBR/rock.material",
    "Assets/Materials/Terrain/PBR/snow.material"};
const std::array<glm::vec3, LayerCount> LayerPreview{{
    {.62f, .55f, .42f}, {.45f, .43f, .40f}, {.13f, .18f, .07f},
    {.22f, .15f, .10f}, {.40f, .38f, .34f}, {.85f, .88f, .92f}}};

// Mesh caches store the painted weights: change this with the paint rules.
// (Also bumped when tree placement changes: generated layers share the key.)
constexpr uint64_t PaintRevision = 0x4c414e445343503cull; // ...3b: before trees on slopes to 42 degrees (2026-11-24)

// Heights the paint and forests follow (see LandscapeDriver's properties).
// blend scales the height transitions above the shore, so taller maps get
// proportionally wider rock and snow fringes.
struct Altitudes
{
    float water = -40.f, rock = 58.f, snow = 102.f, tree = 46.f, blend = 1.f;
};

float SmoothStep(float a, float b, float x)
{
    const float t = std::clamp((x - a) / (b - a), 0.f, 1.f);
    return t * t * (3.f - 2.f * t);
}

// Steepest ground trees are placed on (normal.y). Full density up to ~42
// degrees (0.74, where the paint turns to rock, rock class 3), thinning to a
// quarter at ~58 degrees (0.53), none beyond: forested hills with rock
// showing through on their steepest faces, bare cliffs past that. 42 degrees
// was the hard limit until 2026-11-24, which left the steep sides of wooded
// hills bare (seen from the meadow trail once far cells drew their real
// heights). A placement rule only: what draws far away comes from these
// placements (the far blocks and the macro map's canopy), never from the slope.
constexpr float TreeMinNormalY = .53f;
constexpr float TreeFullNormalY = .74f;
float SlopeTreeShare(float normalY)
{
    return .25f + .75f * SmoothStep(TreeMinNormalY, TreeFullNormalY, normalY);
}
// Extra depth (m) a trunk is sunk on a slope so its downhill side does not
// stand on air: about a trunk radius times the slope's tangent.
float SlopeTrunkSink(float normalY)
{
    const float y = std::clamp(normalY, .2f, 1.f);
    return std::min(1.2f, .35f * std::sqrt(1.f - y * y) / y);
}

uint32_t Hash(int32_t x, int32_t z, uint32_t seed)
{
    uint32_t h = static_cast<uint32_t>(x) * 0x9e3779b1u ^
        static_cast<uint32_t>(z) * 0x85ebca77u ^ seed * 0xc2b2ae3du;
    h ^= h >> 15; h *= 0x2c1b3c6du; h ^= h >> 12; h *= 0x297a2d39u; h ^= h >> 15;
    return h;
}

// Smooth value noise in 0..1 over world metres. Position-only, so paint and
// trees agree across cell borders and between worker threads.
float ValueNoise(float x, float z, float period, uint32_t seed)
{
    x /= period; z /= period;
    const float fx = std::floor(x), fz = std::floor(z);
    const int32_t ix = static_cast<int32_t>(fx), iz = static_cast<int32_t>(fz);
    float tx = x - fx, tz = z - fz;
    tx = tx * tx * (3.f - 2.f * tx); tz = tz * tz * (3.f - 2.f * tz);
    const auto corner = [&](int dx, int dz)
    { return (Hash(ix + dx, iz + dz, seed) & 0xffffffu) / float(0xffffff); };
    return (corner(0, 0) * (1 - tx) + corner(1, 0) * tx) * (1 - tz) +
        (corner(0, 1) * (1 - tx) + corner(1, 1) * tx) * tz;
}

// 0 in open meadow, 1 inside forest. Ground well above the water is more
// wooded (the noise is raised by .06 from 30 to 80 m above the water level,
// about 44% forest on the valley floors and 60% above), so hills carry
// forest with meadow clearings while the valley floors stay half open.
// tools/add_landscape_viewpoints.py replicates this.
float ForestMask(float x, float z, float height, const Altitudes& a)
{
    const float n = .6f * ValueNoise(x, z, 260.f, 11u) +
        .28f * ValueNoise(x, z, 95.f, 23u) + .12f * ValueNoise(x, z, 37.f, 37u) +
        .06f * SmoothStep(a.water + 30.f, a.water + 80.f, height);
    return SmoothStep(.46f, .58f, n);
}

// Where the forest edge is planted up: 0 in open meadow and in closed
// forest, 1 across the outer part of the mask's transition (about 15 m wide
// on the ground). `inner` is the mask value where the belt has faded out.
float ForestEdgeBelt(float forest, float inner)
{
    return SmoothStep(.02f, .15f, forest) * (1.f - SmoothStep(inner * .6f, inner, forest));
}

// Share of forest allowed at a height: none on shores or above the tree line.
float ForestBand(float height, const Altitudes& a)
{
    return SmoothStep(a.water + 4.f, a.water + 9.f, height) *
        (1.f - SmoothStep(a.tree - 8.f * a.blend, a.tree, height));
}

// Trails: 1 on a track, 0 away from it. Tracks follow the 0.5 contour of a
// broad noise, so they wind across the whole map, never end, and join in
// loops; the distance to the contour (value / gradient) gives an even ~6 m
// wide dirt track (terrain vertices are 4 m apart, so narrower paths cannot
// be painted). Paint, trees, undergrowth and props all read this one mask.
// Metres from a trail's centre line (the 0.5 contour), approximately.
float TrailDistance(float x, float z)
{
    const auto field = [](float px, float pz)
    {
        return .7f * ValueNoise(px, pz, 520.f, 71u) + .3f * ValueNoise(px, pz, 170.f, 73u);
    };
    const float e = 4.f;
    const float centre = field(x, z);
    const float gx = (field(x + e, z) - field(x - e, z)) / (2.f * e);
    const float gz = (field(x, z + e) - field(x, z - e)) / (2.f * e);
    // A floor on the gradient keeps flat spots of the noise from spreading
    // the track into a clearing.
    return std::abs(centre - .5f) / std::max(std::sqrt(gx * gx + gz * gz), .0006f);
}

float TrailMask(float x, float z)
{
    return 1.f - SmoothStep(1.6f, 4.2f, TrailDistance(x, z));
}

// How far each species' wood and leaves reach out from the trunk, per 0.25 m
// of height from the ground (model metres, scale 1; tools/measure_tree_reach.py,
// re-run and paste when the tree models change).
struct TreeReachEntry { const char* asset; std::array<float, 16> reach; };
const std::array<TreeReachEntry, 14> TreeReach{{
    {"Oaks/Big_oak_tree_001", {0.93f, 0.00f, 0.73f, 0.00f, 0.64f, 0.00f, 0.60f, 0.00f, 5.80f, 4.74f, 6.32f, 5.49f, 7.23f, 7.61f, 7.92f, 7.84f}},
    {"Oaks/Big_oak_tree_002", {0.85f, 0.00f, 0.70f, 0.00f, 0.58f, 0.00f, 0.67f, 0.00f, 0.67f, 0.32f, 0.73f, 2.82f, 5.87f, 6.15f, 6.36f, 5.62f}},
    {"Oaks/Large_oak_tree_001", {0.88f, 0.70f, 0.65f, 0.00f, 0.67f, 0.00f, 0.69f, 0.00f, 5.57f, 5.72f, 6.02f, 8.35f, 8.53f, 7.83f, 8.84f, 9.79f}},
    {"Oaks/Large_oak_tree_002", {0.90f, 0.00f, 0.73f, 0.00f, 0.66f, 0.75f, 0.72f, 0.58f, 5.09f, 7.13f, 8.53f, 8.54f, 9.48f, 9.77f, 10.29f, 10.84f}},
    {"Oaks/Medium_oak_tree_001", {0.27f, 0.22f, 0.21f, 0.24f, 0.27f, 0.30f, 0.36f, 0.39f, 0.44f, 0.45f, 0.44f, 2.55f, 2.71f, 3.78f, 4.55f, 4.76f}},
    {"Oaks/Medium_oak_tree_002", {0.27f, 0.23f, 0.23f, 0.25f, 0.25f, 0.22f, 2.15f, 2.98f, 3.18f, 3.43f, 3.35f, 3.70f, 3.96f, 4.16f, 4.11f, 4.28f}},
    {"Oaks/Small_oak_tree_001", {0.10f, 0.08f, 0.08f, 0.08f, 0.08f, 0.99f, 1.72f, 1.82f, 1.90f, 1.75f, 2.08f, 2.12f, 2.04f, 1.89f, 2.06f, 2.30f}},
    {"Oaks/Sapling_oak_tree_001", {0.02f, 0.39f, 0.37f, 0.37f, 0.20f, 0.00f, 0.00f, 0.00f, 0.00f, 0.00f, 0.00f, 0.00f, 0.00f, 0.00f, 0.00f, 0.00f}},
    {"Conifers/Spruce_tall_01", {0.00f, 0.00f, 1.55f, 1.73f, 1.08f, 0.74f, 3.06f, 3.59f, 3.62f, 3.36f, 3.74f, 3.91f, 3.63f, 3.85f, 3.84f, 4.03f}},
    {"Conifers/Pine_scots_01", {0.00f, 0.00f, 0.00f, 0.00f, 0.38f, 0.39f, 0.00f, 0.00f, 0.00f, 0.32f, 0.32f, 0.00f, 0.00f, 0.00f, 0.29f, 0.28f}},
    {"Conifers/Fir_subalpine_01", {2.15f, 2.36f, 2.39f, 2.39f, 2.42f, 2.31f, 2.28f, 2.17f, 1.99f, 2.10f, 2.17f, 2.00f, 2.09f, 1.91f, 1.94f, 1.73f}},
    {"Conifers/Spruce_young_01", {2.61f, 2.50f, 2.59f, 2.50f, 2.63f, 2.45f, 2.31f, 2.43f, 2.17f, 2.26f, 2.26f, 2.21f, 2.16f, 2.03f, 1.96f, 1.89f}},
    {"Broadleaf/Broadleaf_slender_01", {0.11f, 0.13f, 0.18f, 0.18f, 0.25f, 1.18f, 1.00f, 1.32f, 1.85f, 2.04f, 2.65f, 2.75f, 2.93f, 2.69f, 2.93f, 2.79f}},
    {"Broadleaf/Broadleaf_upright_01", {0.07f, 0.08f, 0.10f, 0.13f, 0.13f, 0.14f, 0.20f, 0.25f, 0.91f, 0.91f, 0.99f, 1.39f, 1.33f, 1.94f, 1.82f, 2.01f}},
}};

// Whether a tree at (worldX, worldZ) keeps its limbs out of a walker's way on
// the nearest trail. Trunks already keep ~4 m off the track (TrailMask), but
// the big oaks throw limbs 5-8 m out from 2 m up, at eye height when scaled
// down, and a walk along the trail went through their crowns and one limb
// (2026-11-23). In the band a walker's eye sweeps (0.8-2.6 m above the
// ground), the tree's reach at its scale plus 1.5 m of headroom must stay
// short of the track's centre line, where the walk runs. `sink`: metres the
// tree stands below the ground (edge shrubs), lowering its limbs with it.
// Unknown assets pass.
bool LimbsClearOfTrail(const std::string& asset, float scale, float worldX, float worldZ, float sink = 0.f)
{
    constexpr float Headroom = 1.5f, Slice = .25f;
    const float BandLow = .8f + sink, BandHigh = 2.6f + sink;
    for (const auto& entry : TreeReach)
    {
        const std::string suffix = std::string(entry.asset) + ".prefab";
        if (asset.size() < suffix.size() || asset.compare(asset.size() - suffix.size(), suffix.size(), suffix))
            continue;
        const int first = std::max(0, int(std::floor(BandLow / scale / Slice)));
        const int last = std::min(int(entry.reach.size()) - 1, int(std::floor(BandHigh / scale / Slice)));
        float reach = 0.f;
        for (int slice = first; slice <= last; ++slice) reach = std::max(reach, entry.reach[size_t(slice)]);
        return TrailDistance(worldX, worldZ) > reach * scale + Headroom;
    }
    return true;
}

// Trails are shaded per pixel on every terrain level from the same noise
// (TerrainManager::ContourPaths, Object.hlsl TerrainPathWeight); the paint
// then leaves them out. ENGINE_TERRAIN_PATHS=0 paints them at the vertices
// again (4 m in loaded cells, 16 m in the far macro map: the old spots).
bool ShaderTrails()
{
    static const bool enabled = [] {
        const char* value = std::getenv("ENGINE_TERRAIN_PATHS");
        return !(value && value[0] == '0');
    }();
    return enabled;
}

// Six layer weights for one sample. rockClass is the archive's 0..3 code:
// slope and ridge rockiness computed over the whole map by the generator.
void PaintSample(float height, uint8_t rockClass, float worldX, float worldZ,
    const Altitudes& a, float* weights)
{
    const float waterLevel = a.water;
    const float detail = ValueNoise(worldX, worldZ, 22.f, 51u) - .5f;
    const float land = SmoothStep(waterLevel + 1.5f, waterLevel + 4.f, height);
    const float shore = SmoothStep(waterLevel - 3.5f, waterLevel - 1.f, height);
    const float rockSlope = rockClass / 3.f;
    const float rockHigh = SmoothStep(a.rock - 8.f * a.blend, a.rock + 8.f * a.blend,
        height + detail * 14.f * a.blend);
    const float rock = land * std::max(rockSlope, rockHigh);
    const float snow = SmoothStep(a.snow - 6.f * a.blend, a.snow + 6.f * a.blend,
        height + detail * 10.f * a.blend) *
        (1.f - .55f * rockSlope);
    // Forest soil only well inside the forest: the outer part of the edge
    // keeps meadow grass under its trees and shrub belt, so no line of bare
    // soil shows under the edge from the meadow.
    const float soil = land * (1.f - rock) *
        SmoothStep(.3f, .85f, ForestMask(worldX, worldZ, height, a)) *
        ForestBand(height, a) * .85f;
    weights[Sand] = 1.f - shore;
    weights[Gravel] = shore * (1.f - land);
    weights[Snow] = land * snow;
    weights[Rock] = rock * (1.f - snow);
    weights[ForestSoil] = soil * (1.f - snow);
    weights[Grass] = std::max(0.f, land * (1.f - rock) * (1.f - snow) - weights[ForestSoil]);
    // Trails: packed gravel through meadow and forest, not over rock, snow,
    // shores or steep ground (rockSlope). Shaded per pixel instead unless
    // ENGINE_TERRAIN_PATHS=0 (ShaderTrails).
    const float trail = ShaderTrails() ? 0.f : TrailMask(worldX, worldZ) * land *
        (1.f - rock) * (1.f - snow) * ForestBand(height, a);
    if (trail > 0.f)
    {
        for (int layer = 0; layer < LayerCount; ++layer) weights[layer] *= 1.f - .9f * trail;
        weights[Gravel] += .9f * trail;
    }
    float sum = 0.f;
    for (int layer = 0; layer < LayerCount; ++layer) sum += weights[layer];
    if (sum < 1e-5f) { weights[Grass] = 1.f; return; }
    for (int layer = 0; layer < LayerCount; ++layer) weights[layer] /= sum;
}

glm::vec3 LayerMacroColor(const char* path)
{
    Engine::Components::Material material;
    if (!material.LoadFromFile(Engine::Components::Mesh::ResolveFilePath(path)))
        return glm::vec3(.2f);
    return glm::length(material.terrainMacroColor) > 0.f
        ? material.terrainMacroColor : material.diffuseColor;
}

// Far tiles are colored by height band only (no slope or forest mask):
// sand, gravel, meadow, a darker band where forests grow, rock, and summits
// where snow lies between rock faces. They are 15% darker than the layer
// means because near shading adds occlusion and texture contrast.
TerrainManager::FarTerrainPalette MakePalette(const Altitudes& a)
{
    const float waterLevel = a.water;
    TerrainManager::FarTerrainPalette palette;
    for (int layer = 0; layer < LayerCount; ++layer)
        palette.nearColors[layer] = LayerMacroColor(LayerMaterials[layer]);
    // Steep far faces: the scanned cliff rock volume cells show on steep
    // faces (VolumeTerrain::rockMaterial), as dark as the far bands.
    palette.steepColor = LayerMacroColor("Assets/TerrainData/Sculpts/cliff_rock.material") * .85f;
    const auto& mean = palette.nearColors;
    const glm::vec3 wooded = glm::mix(mean[Grass], mean[ForestSoil] * .6f, .35f);
    const glm::vec3 summit = glm::mix(mean[Rock], mean[Snow], .55f);
    palette.farColors = {{mean[Sand] * .85f, mean[Gravel] * .85f, mean[Grass] * .85f,
        wooded * .85f, mean[Rock] * .85f, summit * .85f}};
    palette.heightBoundaries = {{waterLevel - 2.f, waterLevel + 3.f,
        waterLevel + 30.f, a.rock, a.snow}};
    palette.heightBlendHalfWidth = 5.f * a.blend;
    palette.archiveToNear = {Sand, Gravel, Grass, Rock};
    return palette;
}

// Far-tile colors per cell corner (used when no macro color map is bound):
// the near paint averaged over the corner's cell-sized neighborhood, so
// distant terrain shows the rock, snow and lake beds the near cells show.
// Streams the archive a cell at a time (large maps do not fit in memory at
// full resolution) and paints every fourth sample, 16 m on the demo map.
std::vector<glm::vec3> ComputeFarColors(const std::string& archivePath, uint64_t signature,
    int extent, int cellsX, int cellsY, float cellWidth, float cellDepth, const Altitudes& a,
    const std::array<glm::vec3, LayerCount>& means)
{
    if (cellsX != cellsY) return {};
    const int cellSide = extent * 2 + 1;
    const int cornerSide = cellSide + 1;
    std::vector<glm::vec4> sums(static_cast<size_t>(cornerSide) * cornerSide, glm::vec4(0.f));
    const int step = std::max(1, cellsX / 8);
    const int half = cellsX / 2;
    const bool read = TerrainHeightArchive::ForEachCell(archivePath, signature, extent,
        cellsX, cellsY, [&](int cx, int cy, const std::vector<float>& heights,
            const std::vector<uint8_t>& codes)
    {
        // Rows and columns shared with the next cell are visited there,
        // except along the map's far edges.
        const int lastRow = cy == extent ? cellsY : cellsY - 1;
        const int lastCol = cx == extent ? cellsX : cellsX - 1;
        for (int row = 0; row <= lastRow; row += step)
            for (int col = 0; col <= lastCol; col += step)
            {
                const size_t index = static_cast<size_t>(row) * (cellsX + 1) + col;
                const float worldX = (cx * cellsX + col) * cellWidth;
                const float worldZ = (cy * cellsY + row) * cellDepth;
                float weights[LayerCount];
                PaintSample(heights[index], codes[index], worldX, worldZ, a, weights);
                glm::vec3 color(0.f);
                for (int layer = 0; layer < LayerCount; ++layer)
                    color += means[layer] * weights[layer];
                // The ground only: forest canopy comes from the real trees
                // (the far blocks, and the macro map's canopy beyond them),
                // never from the forest paint (2026-11-24).
                // Every corner within half a cell on both axes.
                for (int dy = 0; dy <= 1; ++dy)
                    for (int dx = 0; dx <= 1; ++dx)
                    {
                        if (std::abs(row - dy * cellsY) > half ||
                            std::abs(col - dx * cellsX) > half) continue;
                        const size_t corner = static_cast<size_t>(cy + extent + dy) *
                            cornerSide + cx + extent + dx;
                        sums[corner] += glm::vec4(color, 1.f);
                    }
            }
        return true;
    });
    if (!read) return {};
    std::vector<glm::vec3> colors(sums.size());
    for (size_t i = 0; i < sums.size(); ++i)
        colors[i] = sums[i].w > 0.f ? glm::vec3(sums[i]) / sums[i].w * .85f : glm::vec3(.1f);
    return colors;
}

// Macro color rules (LandscapeDriver "Macro color" properties).
struct MacroColorRules
{
    float lushness = .6f, flowThreshold = .55f, scree = .6f, screeDepth = 6.f;
    float gullyRock = .5f, brightness = .85f;
};

// Darker, greener ground on lush valley floors (Object.hlsl applies the same
// tint to near cells from Terrain::lushness).
const glm::vec3 LushTint(.72f, .92f, .55f);

// Erosion on the paint, one rule for the near cells and the macro map (so
// ground keeps its look as it goes from near shading to the macro colour;
// before 2026-11-24 only the macro map had these): wet valley floors under
// grass and forest grow lush (returned: the share of LushTint), sediment
// settled below rock and on alpine ground becomes scree (gravel and rock),
// and steep, heavily drained slopes scour to rock. slope: rise over run at
// the 16 m erosion grid; wet: drained flow 0..1 past the threshold;
// sediment: deposited metres.
float ApplyErosionPaint(float* weights, float height, float slope, float wet,
    float sediment, const MacroColorRules& rules, const Altitudes& a)
{
    const auto shift = [&](float amount, float rock, float gravel)
    {
        amount = std::clamp(amount, 0.f, 1.f);
        if (amount <= 0.f) return;
        for (int layer = 0; layer < LayerCount; ++layer) weights[layer] *= 1.f - amount;
        weights[Rock] += amount * rock;
        weights[Gravel] += amount * gravel;
    };
    const float vegetated = weights[Grass] + weights[ForestSoil];
    const float lush = std::clamp(rules.lushness * wet * vegetated *
        (1.f - SmoothStep(.35f, .7f, slope)), 0.f, 1.f);
    const float snow = weights[Snow];
    const float alpine = std::max(weights[Rock] + weights[Gravel],
        SmoothStep(a.tree, a.tree + 12.f * a.blend, height));
    shift(rules.scree * alpine * std::clamp(sediment / std::max(rules.screeDepth, .1f), 0.f, 1.f) *
        (1.f - snow), .4f, .6f);
    shift(rules.gullyRock * wet * SmoothStep(.45f, .9f, slope) * (1.f - snow), 1.f, 0.f);
    return lush;
}

// The macro color map: the near paint every fourth sample (16 m on the demo
// map, the generator's erosion grid), shaped by erosion. Valley floors and
// streams that carried a lot of water turn lush, steep gullies scour to bare
// rock, and sediment fans show as scree; forest canopy covers forest soil
// (alpha = canopy cover). Rows along +Z from the map's -X/-Z corner.
std::vector<glm::vec4> ComputeMacroColors(const std::string& archivePath, uint64_t signature,
    int extent, int cellsX, int cellsY, float cellWidth, float cellDepth, const Altitudes& a,
    const std::array<glm::vec3, LayerCount>& means, const TerrainHydrologyMap& flow,
    const TerrainHydrologyMap& deposition, const MacroColorRules& rules,
    int& side, std::vector<glm::vec3>& normals,
    const std::function<void(int, int, const std::vector<float>&, const std::vector<uint8_t>&)>& onCell = {})
{
    side = 0;
    normals.clear();
    if (cellsX != cellsY || cellsX % 4 != 0) return {};
    const int step = 4;
    const int perCell = cellsX / step;
    side = (extent * 2 + 1) * perCell + 1;
    std::vector<glm::vec4> colors(static_cast<size_t>(side) * side, glm::vec4(.1f, .1f, .1f, 0.f));
    normals.assign(colors.size(), glm::vec3(0.f, 1.f, 0.f));
    const bool read = TerrainHeightArchive::ForEachCell(archivePath, signature, extent,
        cellsX, cellsY, [&](int cx, int cy, const std::vector<float>& heights,
            const std::vector<uint8_t>& codes)
    {
        if (onCell) onCell(cx, cy, heights, codes);
        const auto height = [&](int row, int col) {
            row = std::clamp(row, 0, cellsY); col = std::clamp(col, 0, cellsX);
            return heights[static_cast<size_t>(row) * (cellsX + 1) + col];
        };
        for (int row = 0; row <= cellsY; row += step)
            for (int col = 0; col <= cellsX; col += step)
            {
                const size_t index = static_cast<size_t>(row) * (cellsX + 1) + col;
                const float worldX = (cx * cellsX + col) * cellWidth;
                const float worldZ = (cy * cellsY + row) * cellDepth;
                float weights[LayerCount];
                PaintSample(heights[index], codes[index], worldX, worldZ, a, weights);
                glm::vec3 color(0.f);
                for (int layer = 0; layer < LayerCount; ++layer)
                    color += means[layer] * weights[layer];
                // Rise over run across one sample spacing each way.
                const float dx = (height(row, col + step) - height(row, col - step)) /
                    (2.f * step * cellWidth);
                const float dz = (height(row + step, col) - height(row - step, col)) /
                    (2.f * step * cellDepth);
                const float slope = std::sqrt(dx * dx + dz * dz);
                const float wet = SmoothStep(rules.flowThreshold, 1.f,
                    flow.Normalized(worldX, worldZ));
                const float sediment = deposition.Normalized(worldX, worldZ) * deposition.scale;
                // The near cells' erosion paint (ApplyErosionPaint), averaged.
                const float lushShare = ApplyErosionPaint(weights, heights[index], slope, wet,
                    sediment, rules, a);
                color = glm::vec3(0.f);
                for (int layer = 0; layer < LayerCount; ++layer)
                    color += means[layer] * weights[layer];
                color *= glm::mix(glm::vec3(1.f), LushTint, lushShare);
                // Steep faces: the scanned cliff rock near volume cells show
                // there (VolumeTerrain's steep band, 1 - normal.y .25 .. .45),
                // not the paint's pale rock layer: distant cliffs and the
                // escarpment read as bright tan faces (2026-11-24).
                {
                    static const glm::vec3 cliff = LayerMacroColor("Assets/TerrainData/Sculpts/cliff_rock.material");
                    const float normalY = 1.f / std::sqrt(1.f + slope * slope);
                    float t = std::clamp((1.f - normalY - .25f) / .2f, 0.f, 1.f);
                    t = t * t * (3.f - 2.f * t);
                    color = glm::mix(color, cliff, t);
                }
                // The color is the ground only; alpha (canopy cover) is
                // filled by the caller from the real tree placements, and
                // Object.hlsl paints that canopy only past the real trees'
                // draw distance (TerrainManager::farCanopyDistance). Canopy
                // from the forest paint showed forest where the real trees
                // were sparser and turned to bare ground as cells loaded
                // (2026-11-24).
                const size_t sample = static_cast<size_t>((cy + extent) * perCell + row / step) *
                    side + static_cast<size_t>((cx + extent) * perCell + col / step);
                colors[sample] = glm::vec4(color * rules.brightness, 0.f);
                // Same slopes: the surface normal at this sample's 16 m scale.
                normals[sample] = glm::normalize(glm::vec3(-dx, 1.f, -dz));
            }
        return true;
    });
    if (!read) { side = 0; normals.clear(); return {}; }
    return colors;
}

// Oak species with authored LOD chains, smallest last. Instanced batches
// merge across cells only when they share an asset, so the map-wide species
// count costs CPU time too: all seventeen species measured about 8 ms more
// per frame than these eight.
const std::array<const char*, 8> Species{
    "Big_oak_tree_001", "Big_oak_tree_002", "Large_oak_tree_001",
    "Large_oak_tree_002", "Medium_oak_tree_001", "Medium_oak_tree_002",
    "Small_oak_tree_001", "Sapling_oak_tree_001"};
// Generated conifers (Tools/generate_conifers.py): large (spruce or pine),
// medium (fir) and small (young spruce). They take over from the oaks over
// the upper forest band below the tree line.
const std::array<const char*, 4> ConiferSpecies{
    "Spruce_tall_01", "Pine_scots_01", "Fir_subalpine_01", "Spruce_young_01"};
// Card-crown broadleaf trees baked from Poly Haven's tree_small_02 scan
// (Tools/scan_tree_cards.py): an airy, slender tree with a curved trunk and an
// upright, narrow-crowned variant. They stand alone in the meadows, in front
// of the oak edges. ENGINE_BROADLEAF=0 plants oaks in their place (A/B).
// The edge-belt placement is off by default (ENGINE_BROADLEAF=all or =edge
// turns it on): the belt is dense, and with any share of broadleaves in it
// one stood over the ForestEdge camera, whose view looked into its crown
// (captures bl2-bla, 2026-11-22; .md/todo.md "Forest edge variety").
const std::array<const char*, 2> BroadleafSpecies{"Broadleaf_slender_01", "Broadleaf_upright_01"};
const char* const BroadleafPrefix = "Assets/Prefabs/Woodland/Broadleaf/";
bool BroadleafOn(bool edge)
{
    static const std::string mode = [] { const char* v = std::getenv("ENGINE_BROADLEAF"); return std::string(v ? v : "lone"); }();
    if (mode == "0") return false;
    if (mode == "edge") return edge;
    if (mode == "lone") return !edge;
    return true;
}

// Share of conifers among the trees at a height: oak lowlands, mixed
// foothills, conifers over the upper slopes (350-710 m on the demo map,
// whose tree line is 950 m with an altitude blend of 6).
float ConiferShare(float height, const Altitudes& a)
{
    return SmoothStep(a.tree - 100.f * a.blend, a.tree - 40.f * a.blend, height);
}

// A/B switch: ENGINE_FAR_PATCHES=0 drops the far grid's full-resolution
// cell patches.
bool EnvironmentSwitch(const char* name)
{
    const char* value = std::getenv(name);
    return !(value && value[0] == '0');
}

// ENGINE_LANDSCAPE_SHADOW_SCALE=<factor> scales the shadow ranges of the
// forest, prop and undergrowth layers (for A/B runs; 1 = as authored).
float LandscapeShadowScale()
{
    static const float scale = []
    { const char* v = std::getenv("ENGINE_LANDSCAPE_SHADOW_SCALE"); return v ? std::clamp(float(std::atof(v)), .1f, 2.f) : 1.f; }();
    return scale;
}

// A layer's shadow range: <name> from the environment when set (A/B runs of
// longer ranges), else the authored one, times LandscapeShadowScale.
float LandscapeShadowRange(const char* name, float authored)
{
    const char* v = std::getenv(name);
    const float range = v ? std::clamp(float(std::atof(v)), 0.f, 900.f) : authored;
    return range * LandscapeShadowScale();
}

// Forest for one cell: random candidates kept where the forest mask, the
// height band and the slope allow, plus a few lone trees in the meadows.
void GenerateForest(const TerrainManager::ObjectLayerContext& context,
    TerrainObjectLayer& layer, const Altitudes& a, int treesPerCell)
{
    if (!context.heights || context.cellsX <= 0 || context.cellsY <= 0 ||
        context.heights->size() != static_cast<size_t>(context.cellsX + 1) *
            (context.cellsY + 1)) return;
    // Forests read to the horizon of the loaded area: full trees near, then
    // the authored impostors (a few textured quads each) from 220 m out to
    // 900 m. Each oak prefab carries LOD0, LOD1, LOD2 (and for large oaks a
    // canopy level) plus the impostor; instanced layers keep three resident
    // bands, so the prefab's own chain resolves to LOD0, LOD1, impostor.
    layer.viewDistance = 900.f;
    layer.lod1Distance = 60.f;
    layer.lod2Distance = 220.f;
    // Trees cast with their LOD1 mesh to 220 m, inside the 250 m near
    // cascades and the foliage caster budget (RealtimeShadowSettings);
    // ENGINE_TREE_SHADOW_DISTANCE for A/B runs (90 m before 2026-11-22).
    layer.shadowDistance = LandscapeShadowRange("ENGINE_TREE_SHADOW_DISTANCE", 220.f);
    layer.shadowLod = 1;
    // Beyond that the impostors cast into the far shadow cascade, so forests
    // keep their shadows on the ground and on each other at any distance.
    layer.farShadowDistance = layer.viewDistance;
    // The two casters cross-fade (dithered) over the last metres before
    // shadowDistance. 1 m (the layer default) swapped the crown shadow's
    // shape and density at once at 220 m; 30 m spreads the change over a
    // walk of ~20 s. ENGINE_TREE_SHADOW_FADE=<m> for A/B runs.
    layer.shadowFadeWidth = std::min(layer.shadowDistance,
        LandscapeShadowRange("ENGINE_TREE_SHADOW_FADE", 30.f));
    const std::string prefix = "Assets/Prefabs/Woodland/Oaks/";
    const std::string coniferPrefix = "Assets/Prefabs/Woodland/Conifers/";
    for (const char* species : Species)
    {
        auto& lod = layer.objectLods[prefix + species + ".prefab"];
        lod.distances = {60.f, 220.f};
    }
    for (const char* species : ConiferSpecies)
        layer.objectLods[coniferPrefix + species + ".prefab"].distances = {60.f, 220.f};
    for (const char* species : BroadleafSpecies)
        layer.objectLods[std::string(BroadleafPrefix) + species + ".prefab"].distances = {60.f, 220.f};
    const float width = context.cellsX * context.cellWidth;
    const float depth = context.cellsY * context.cellDepth;
    uint32_t state = Hash(context.cell.first, context.cell.second, 0x74726565u) | 1u;
    const auto random = [&state]
    {
        state ^= state << 13; state ^= state >> 17; state ^= state << 5;
        return (state & 0xffffffu) / float(0x1000000);
    };
    // Each cell grows a stand of two species per size band (large, medium,
    // small), mixed tree by tree, from the map's eight oak species. One
    // species per band made every large tree in a 128 m cell the same model.
    // Batches merge across cells per asset, so the map-wide species count is
    // what costs CPU (see Species); mixing within a cell adds no assets.
    const uint32_t stand = Hash(context.cell.first, context.cell.second, 0x7374616eu);
    const size_t largeA = stand % 4u;
    const size_t largeB = (largeA + 1u + (stand >> 4) % 3u) % 4u;
    const std::array<std::array<size_t, 2>, 3> standSpecies{{
        {largeA, largeB}, {4u, 5u}, {6u, 7u}}};
    const std::array<std::array<size_t, 2>, 3> standConifers{{{0u, 1u}, {2u, 2u}, {3u, 3u}}};
    uint64_t id = 0;
    for (int attempt = 0; attempt < treesPerCell; ++attempt)
    {
        const float x = (.01f + .98f * random()) * width;
        const float z = (.01f + .98f * random()) * depth;
        const float chance = random();
        const float pick = random(), yaw = random(), size = random();
        float height = 0.f;
        glm::vec3 normal(0.f, 1.f, 0.f);
        if (!Engine::Components::SampleTerrainSurface(*context.heights, context.cellsX,
                context.cellsY, context.cellWidth, context.cellDepth, x, z, height, &normal))
            continue;
        if (normal.y < TreeMinNormalY) continue; // steeper than about 58 degrees
        const float worldX = context.cell.first * width + x;
        const float worldZ = context.cell.second * depth + z;
        // Lone meadow trees keep a 3% chance outside the forest mask.
        const float interior = ForestMask(worldX, worldZ, height, a);
        const float density = std::max(interior, .03f) * ForestBand(height, a) *
            SlopeTreeShare(normal.y);
        if (chance >= density) continue;
        // Trails stay clear (the mask reaches ~4 m either side of the track).
        if (TrailMask(worldX, worldZ) > .02f) continue;
        TerrainObjectLayer::Placement placement;
        placement.id = ++id;
        // Forest interiors favour large trees, edges and meadows small ones.
        const float sizeClass = pick * .6f + (1.f - interior) * .4f;
        const int sizeBand = sizeClass < .5f ? 0 : sizeClass < .75f ? 1 : 2;
        // Conifer or oak, the stand's species, and the tree's lean come from
        // the draws above (no extra random, so tree positions are unchanged).
        const float familyDraw = std::fmod(pick * 7.31f + yaw * 3.17f, 1.f);
        const size_t variant = std::fmod(size * 13.71f + yaw * 5.37f + pick * 2.11f, 1.f) < .5f ? 0u : 1u;
        const bool conifer = familyDraw < ConiferShare(height, a);
        if (conifer)
            placement.asset = coniferPrefix + ConiferSpecies[standConifers[sizeBand][variant]] + ".prefab";
        else
            placement.asset = prefix + Species[standSpecies[sizeBand][variant]] + ".prefab";
        placement.position = {x, height - .08f - SlopeTrunkSink(normal.y), z};
        placement.yawDegrees = yaw * 360.f;
        placement.scale = .75f + size * .55f;
        // Lone meadow trees out in the open: half are the card-crown
        // broadleaves (4.5 m as baked, so scaled to 7-11 m). Their crowns
        // start low and spread to one side, so trees within ~15 m of the
        // forest (where paths and viewpoints run along the edge) stay oaks:
        // one there roofed over the ForestEdge view.
        const float broadleafDraw = std::fmod(yaw * 17.3f + size * 5.9f, 1.f);
        const auto openMeadow = [&]
        {
            if (interior >= .02f) return false;
            for (int k = 0; k < 6; ++k)
            {
                const float angle = k * glm::two_pi<float>() / 6.f;
                if (ForestMask(worldX + 15.f * std::cos(angle), worldZ + 15.f * std::sin(angle), height, a) >= .02f)
                    return false;
            }
            return true;
        };
        if (BroadleafOn(false) && !conifer && broadleafDraw < .5f && openMeadow())
        {
            placement.asset = std::string(BroadleafPrefix) + BroadleafSpecies[broadleafDraw < .25f ? 0u : 1u] +
                ".prefab";
            placement.scale = 1.6f + size * .8f;
        }
        // Limbs out of a walker's way on a nearby trail: a tree whose crown
        // would reach the walking line gives way to its stand's smaller
        // species (the canopy along the trail stays as dense), else is left out.
        if (!LimbsClearOfTrail(placement.asset, placement.scale, worldX, worldZ))
        {
            bool cleared = false;
            for (int smaller = sizeBand - 1; smaller >= 0 && !cleared; --smaller)
            {
                placement.asset = conifer
                    ? coniferPrefix + ConiferSpecies[standConifers[size_t(smaller)][variant]] + ".prefab"
                    : prefix + Species[standSpecies[size_t(smaller)][variant]] + ".prefab";
                placement.scale = .75f + size * .55f;
                cleared = LimbsClearOfTrail(placement.asset, placement.scale, worldX, worldZ);
            }
            if (!cleared) continue;
        }
        // A slight lean (the layer does not conform to the ground, so the
        // normal tilts the tree; far blocks copy it): most trees stand nearly
        // straight, a few lean up to ~7 degrees; conifers grow straighter.
        const float leanDraw = std::fmod(size * 7.13f + pick * 11.3f, 1.f);
        const float lean = glm::radians((conifer ? 2.5f : 7.f) * leanDraw * leanDraw);
        const float leanDirection = std::fmod(yaw * 9.7f + size * 3.1f, 1.f) * glm::two_pi<float>();
        placement.normal = {std::sin(lean) * std::cos(leanDirection), std::cos(lean),
            std::sin(lean) * std::sin(leanDirection)};
        layer.placements.push_back(std::move(placement));
    }
    // Forest edge. The pass above thins out with the mask, which left the
    // edge as scattered trunks over open ground with sky between the crowns.
    // A second pass closes it from the ground up, in three steps across the
    // mask's transition (about 20 m on the ground), meadow side first:
    //  * shrubs: small oaks sunk to the base of their crowns (the leaves
    //    start 1.7 m up a bare stem), so each is a 2-4.5 m leafy mound
    //    standing on the grass, up to one per ~8 m2;
    //  * a skirt: medium oaks sunk the same way (crowns start at 3.6 m),
    //    5-8 m mounds that carry the wall up to the crowns behind;
    //  * standing medium and small trees through the rest of the transition.
    // In the conifer band young spruces do all three; their branches reach
    // the ground as they are. The pack bushes cannot do this: they are under
    // 2 m at any sensible scale and single-LOD props end ~25 m from the
    // camera (captured 2026-11-20), so the belt of them was removed.
    // Its own random stream leaves the trees above where they were. Loaded
    // cells only: these are too small to matter in the far blocks, and every
    // far tree costs an impostor.
    if (!context.farObjects)
    {
        uint32_t edgeState = Hash(context.cell.first, context.cell.second, 0x65646765u) | 1u;
        const auto edgeRandom = [&edgeState]
        {
            edgeState ^= edgeState << 13; edgeState ^= edgeState >> 17; edgeState ^= edgeState << 5;
            return (edgeState & 0xffffffu) / float(0x1000000);
        };
        for (int attempt = 0; attempt < treesPerCell * 10; ++attempt)
        {
            const float x = (.01f + .98f * edgeRandom()) * width;
            const float z = (.01f + .98f * edgeRandom()) * depth;
            const float chance = edgeRandom(), pick = edgeRandom(), yaw = edgeRandom(),
                size = edgeRandom();
            float height = 0.f;
            glm::vec3 normal(0.f, 1.f, 0.f);
            if (!Engine::Components::SampleTerrainSurface(*context.heights, context.cellsX,
                    context.cellsY, context.cellWidth, context.cellDepth, x, z, height, &normal))
                continue;
            // The edge belt (shrubs and skirts sunk to their crowns) stays off steep ground.
            if (normal.y < TreeFullNormalY) continue;
            const float worldX = context.cell.first * width + x;
            const float worldZ = context.cell.second * depth + z;
            const float forest = ForestMask(worldX, worldZ, height, a) * ForestBand(height, a);
            if (forest <= .02f || forest >= .9f) continue;
            // Chances per attempt (one attempt per ~7.4 m2). The shrub line
            // is ragged by a 14 m noise but never opens up.
            const float ragged = .65f + .35f * ValueNoise(worldX, worldZ, 14.f, 227u);
            const float shrubChance = .9f * ragged * SmoothStep(.02f, .1f, forest) *
                (1.f - SmoothStep(.35f, .6f, forest));
            const float skirtChance = .22f * SmoothStep(.08f, .25f, forest) *
                (1.f - SmoothStep(.6f, .9f, forest));
            const float standingChance = .24f * ForestEdgeBelt(forest, .9f);
            const int role = chance < shrubChance ? 0 :
                chance < shrubChance + skirtChance ? 1 :
                chance < shrubChance + skirtChance + standingChance ? 2 : -1;
            if (role < 0) continue;
            if (TrailMask(worldX, worldZ) > .02f) continue;
            const bool conifer = std::fmod(pick * 7.31f + yaw * 3.17f, 1.f) < ConiferShare(height, a);
            TerrainObjectLayer::Placement placement;
            placement.id = ++id;
            float sink = .08f, lean = 0.f;
            if (conifer)
            {
                placement.asset = coniferPrefix + ConiferSpecies[role == 2 && pick > .5f ? 2u : 3u] + ".prefab";
                placement.scale = role == 0 ? .3f + .3f * size : role == 1 ? .55f + .35f * size :
                    .8f + .6f * size;
                lean = glm::radians(2.f);
            }
            else if (role == 0)
            {
                placement.asset = prefix + Species[6] + ".prefab";
                placement.scale = .5f + .6f * size;
                sink = 1.6f * placement.scale;
                lean = glm::radians(4.f * size);
            }
            else if (role == 1)
            {
                placement.asset = prefix + Species[pick < .5f ? 4u : 5u] + ".prefab";
                placement.scale = .7f + .4f * size;
                sink = 3.f * placement.scale;
                lean = glm::radians(3.f * size);
            }
            else
            {
                // 0 at the meadow side of the transition, 1 at the forest side.
                const float inward = SmoothStep(.05f, .7f, forest);
                const bool medium = pick * .65f + inward * .35f > .62f;
                placement.asset = prefix + Species[medium ? (size < .5f ? 4u : 5u) : 6u] + ".prefab";
                placement.scale = .8f + size * .6f;
                // Off by default (BroadleafOn): deep in the belt, three in ten
                // upright broadleaves, 12-15 m, rising over the oaks in front.
                const float broadleafDraw = std::fmod(pick * 3.1f + size * 11.9f, 1.f);
                if (BroadleafOn(true) && forest > .65f && broadleafDraw < .3f)
                {
                    placement.asset = std::string(BroadleafPrefix) + BroadleafSpecies[1] + ".prefab";
                    placement.scale = 2.f + size * .5f;
                }
                // Edge trees lean out toward the light, up to ~9 degrees.
                lean = glm::radians(9.f * size * size);
            }
            // Limbs out of a walker's way on a nearby trail (sunk shrubs and
            // skirts carry their limbs lower).
            if (!LimbsClearOfTrail(placement.asset, placement.scale, worldX, worldZ, sink)) continue;
            placement.position = {x, height - sink, z};
            placement.yawDegrees = yaw * 360.f;
            const float leanDirection = pick * glm::two_pi<float>();
            placement.normal = {std::sin(lean) * std::cos(leanDirection), std::cos(lean),
                std::sin(lean) * std::sin(leanDirection)};
            layer.placements.push_back(std::move(placement));
        }
    }
}

// Ground detail cooked by tools/PropCook: rocks from one scanned pack and a
// fallen log, each one prefab carrying three levels, and the woodland bush pack
// (single LOD). As with trees, the map-wide asset count costs CPU time, so
// the landscape uses six of the twelve cooked rocks and six bushes.
constexpr int RockCount = 12;
const std::array<int, 6> LandscapeRocks{0, 2, 3, 7, 9, 11};
const std::array<const char*, 6> Bushes{
    "Small_bush_1", "Small_bush_2", "Medium_bush_1", "Medium_bush_2",
    "tall_bush_2", "tall_bush_3"};

std::string RockPrefab(int index)
{
    char name[96];
    std::snprintf(name, sizeof(name), "Assets/Prefabs/Woodland/Rocks/Rock_%02d.prefab",
        index + 1);
    return name;
}

// Photo-scanned forest props (Poly Haven, CC0; tools/fetch_polyhaven_props.py),
// each a prefab with three levels: stumps, mossy rocks, dry branches, ferns.
std::string ScanPrefab(const char* stem, size_t index)
{
    char name[96];
    std::snprintf(name, sizeof(name), "Assets/Prefabs/Woodland/Scans/%s_%02d.prefab", stem,
        static_cast<int>(index) + 1);
    return name;
}
constexpr size_t StumpCount = 2, MossRockCount = 6, DryBranchCount = 3, FernCount = 4;

std::string LogPrefab()
{
    return "Assets/Prefabs/Woodland/Logs/FallenLog.prefab";
}

// Generated faceted boulders (Tools/generate_boulders.py, triplanar rock),
// already at their real size; one per cell's stand, in rock fields.
const std::array<const char*, 4> Boulders{
    "Boulder_01", "Boulder_02", "Boulder_03", "Boulder_flat_01"};

std::string BoulderPrefab(size_t index)
{
    return std::string("Assets/Prefabs/Woodland/Boulders/") + Boulders[index % Boulders.size()] + ".prefab";
}

// Rocks and fallen logs, or bushes (undergrowth), for one cell, from the same
// paint and forest mask as the terrain. Both normally share one layer: every
// terrain object layer costs fixed per-cell bookkeeping (prefab profile and
// source checks), which measured more than the instances themselves.
//  * rocks gather on rock paint (slopes, ridges, above the tree line), in
//    scattered boulder fields, and as small stones along gravel shores;
//  * logs lie on gentle forest floor;
//  * bushes ring the forest edges, fill some of the forest floor and dot the
//    meadows in clumps.
// Both layers end inside the loaded cells, so they have no far representation.
void GenerateProps(const TerrainManager::ObjectLayerContext& context,
    TerrainObjectLayer& layer, const Altitudes& a, bool undergrowth, float density)
{
    const float waterLevel = a.water;
    if (!context.heights || context.cellsX <= 0 || context.cellsY <= 0 ||
        context.heights->size() != static_cast<size_t>(context.cellsX + 1) *
            (context.cellsY + 1)) return;
    if (undergrowth && layer.objectLods.empty())
    {
        // A dedicated undergrowth layer (bushes have a single LOD).
        layer.viewDistance = 150.f;
        layer.lod1Distance = layer.lod2Distance = 150.f;
        layer.shadowDistance = 45.f * LandscapeShadowScale();
    }
    else if (!undergrowth)
    {
        layer.viewDistance = 220.f;
        layer.lod1Distance = 35.f;
        layer.lod2Distance = 100.f;
        // Rocks, boulders, logs and stumps cast to 150 m (80 m before
        // 2026-11-22; no measurable cost); ENGINE_PROP_SHADOW_DISTANCE for A/B.
        layer.shadowDistance = LandscapeShadowRange("ENGINE_PROP_SHADOW_DISTANCE", 150.f);
        layer.shadowLod = 1;
        // The prefabs carry their own LOD0-2 chains; only distances are set.
        for (int rock : LandscapeRocks)
            layer.objectLods[RockPrefab(rock)].distances = {35.f, 100.f};
        layer.objectLods[LogPrefab()].distances = {35.f, 100.f};
        for (size_t boulder = 0; boulder < Boulders.size(); ++boulder)
            layer.objectLods[BoulderPrefab(boulder)].distances = {35.f, 100.f};
        for (size_t i = 0; i < StumpCount; ++i)
            layer.objectLods[ScanPrefab("Stump", i)].distances = {35.f, 100.f};
        for (size_t i = 0; i < MossRockCount; ++i)
            layer.objectLods[ScanPrefab("MossRock", i)].distances = {35.f, 100.f};
        for (size_t i = 0; i < DryBranchCount; ++i)
            layer.objectLods[ScanPrefab("DryBranch", i)].distances = {25.f, 70.f};
    }
    // Bushes have a single LOD; an explicit one-entry chain gives them a stable
    // profile like every other asset in the layer.
    if (undergrowth)
        for (const char* bush : Bushes)
        {
            const std::string path = std::string("Assets/Prefabs/Woodland/Bushes/") + bush + ".prefab";
            auto& lod = layer.objectLods[path];
            lod.meshes = {path};
            lod.distances.clear();
        }
    // Scanned ferns keep their own three levels.
    if (undergrowth)
        for (size_t i = 0; i < FernCount; ++i)
            layer.objectLods[ScanPrefab("FernScan", i)].distances = {20.f, 50.f};
    const float width = context.cellsX * context.cellWidth;
    const float depth = context.cellsY * context.cellDepth;
    uint32_t state = Hash(context.cell.first, context.cell.second,
        undergrowth ? 0x62757368u : 0x726f636bu) | 1u;
    const auto random = [&state]
    {
        state ^= state << 13; state ^= state >> 17; state ^= state << 5;
        return (state & 0xffffffu) / float(0x1000000);
    };
    const size_t layerCount = context.terrainLayerCount;
    const bool painted = context.weights && layerCount >= LayerCount &&
        context.weights->size() == context.heights->size() * layerCount;
    // Paint weights at the nearest terrain vertex.
    const auto paint = [&](float x, float z, int layerIndex)
    {
        if (!painted) return layerIndex == Grass ? 1.f : 0.f;
        const int col = std::clamp(static_cast<int>(std::lround(x / context.cellWidth)), 0, context.cellsX);
        const int row = std::clamp(static_cast<int>(std::lround(z / context.cellDepth)), 0, context.cellsY);
        return (*context.weights)[(static_cast<size_t>(row) * (context.cellsX + 1) + col) *
            layerCount + layerIndex];
    };
    uint64_t id = layer.placements.size();
    const auto place = [&](std::string asset, float x, float y, float z,
        const glm::vec3& normal, float yaw, float scale)
    {
        TerrainObjectLayer::Placement placement;
        placement.id = ++id;
        placement.asset = std::move(asset);
        placement.position = {x, y, z};
        placement.normal = normal;
        placement.yawDegrees = yaw * 360.f;
        placement.scale = scale;
        layer.placements.push_back(std::move(placement));
    };
    // Each cell uses a stand of two rock shapes (plus the log) or two bushes
    // (one low, one tall): every distinct asset in a cell is a set of
    // instanced draws, so stands keep the per-cell cost flat while
    // neighbouring cells still mix the whole set.
    const uint32_t stand = Hash(context.cell.first, context.cell.second, 0x70726f70u);
    const std::array<int, 2> standRocks{LandscapeRocks[stand % 3u],
        LandscapeRocks[3u + (stand >> 8) % 3u]};
    const size_t standBoulder = (stand >> 16) % Boulders.size();
    const std::array<size_t, 2> standBushes{(stand >> 4) % 4u, 4u + (stand >> 12) % 2u};
    // One stump, one dry branch, two mossy rocks and two ferns per cell.
    const size_t standStump = (stand >> 20) % StumpCount;
    const size_t standBranch = (stand >> 21) % DryBranchCount;
    const std::array<size_t, 2> standMossRocks{(stand >> 23) % 3u, 3u + (stand >> 25) % 3u};
    const std::array<size_t, 2> standFerns{(stand >> 27) % 2u, 2u + (stand >> 28) % 2u};
    const int attempts = static_cast<int>(std::lround((undergrowth ? 420.f : 300.f) *
        std::clamp(density, 0.f, 4.f)));
    for (int attempt = 0; attempt < attempts; ++attempt)
    {
        // Every attempt draws the same randoms so layouts stay stable when
        // one rule changes.
        const float x = (.01f + .98f * random()) * width;
        const float z = (.01f + .98f * random()) * depth;
        const float chance = random(), pick = random(), yaw = random(), size = random(),
            kind = random();
        float height = 0.f;
        glm::vec3 normal(0.f, 1.f, 0.f);
        if (!Engine::Components::SampleTerrainSurface(*context.heights, context.cellsX,
                context.cellsY, context.cellWidth, context.cellDepth, x, z, height, &normal))
            continue;
        const float worldX = context.cell.first * width + x;
        const float worldZ = context.cell.second * depth + z;
        const float forest = ForestMask(worldX, worldZ, height, a) * ForestBand(height, a);
        // Nothing grows or lies on a trail.
        if (TrailMask(worldX, worldZ) > .1f) continue;
        if (undergrowth)
        {
            if (height < waterLevel + 1.5f || normal.y < .82f) continue;
            const float edge = 4.f * forest * (1.f - forest);
            const float clump = SmoothStep(.45f, .75f, ValueNoise(worldX, worldZ, 16.f, 211u));
            const float meadow = paint(x, z, Grass);
            const float odds = (.5f * edge + .08f * forest + .1f * meadow) * (.35f + 1.3f * clump) *
                (1.f - paint(x, z, Snow)) * (1.f - paint(x, z, Rock));
            // Under the trees, scanned ferns in drifts (their own noise, so
            // they mass in some glades and leave others open).
            if (forest > .45f && kind < .55f)
            {
                const float drift = SmoothStep(.4f, .7f, ValueNoise(worldX, worldZ, 21.f, 223u));
                if (chance >= .85f * forest * (.15f + drift)) continue;
                place(ScanPrefab("FernScan", standFerns[pick < .5f ? 0 : 1]), x, height - .03f, z,
                    glm::vec3(0.f, 1.f, 0.f), yaw, 1.1f + .9f * size);
                continue;
            }
            if (chance >= odds) continue;
            // Small bushes in the open, tall ones under and beside trees.
            const float shade = std::clamp(forest + edge * .5f, 0.f, 1.f);
            const float tallness = pick * .55f + shade * .45f;
            const size_t bush = standBushes[tallness < .5f ? 0 : 1];
            place(std::string("Assets/Prefabs/Woodland/Bushes/") + Bushes[bush] + ".prefab",
                x, height - .05f, z, glm::vec3(0.f, 1.f, 0.f), yaw, .7f + .6f * size);
            continue;
        }
        if (height < waterLevel - .8f) continue;
        // Logs and stumps: gentle forest floor only.
        if (kind < .16f)
        {
            if (normal.y < .93f || height < waterLevel + 2.f || chance >= .9f * forest) continue;
            if (kind < .09f)
                place(LogPrefab(), x, height - .04f, z, normal, yaw, .8f + .6f * size);
            else
                place(ScanPrefab("Stump", standStump), x, height - .03f, z,
                    glm::vec3(0.f, 1.f, 0.f), yaw, .9f + .6f * size);
            continue;
        }
        // Dry branches lying on the forest floor.
        if (kind < .34f && forest > .3f)
        {
            if (normal.y < .9f || chance >= .7f * forest) continue;
            place(ScanPrefab("DryBranch", standBranch), x, height + .01f, z, normal, yaw,
                .9f + .8f * size);
            continue;
        }
        const float rocky = std::min(1.f, paint(x, z, Rock) + .5f * paint(x, z, Snow));
        const float shore = paint(x, z, Gravel);
        const float field = SmoothStep(.66f, .82f, ValueNoise(worldX, worldZ, 70.f, 101u));
        // Scree: ground with a steep slope a little way uphill collects what
        // falls off it, so cliff feet get boulder aprons instead of rocks
        // spread evenly over the hills.
        float scree = 0.f;
        {
            const glm::vec2 slope(normal.x, normal.z);
            const float steepness = glm::length(slope);
            if (steepness > .05f)
            {
                const glm::vec2 uphill = -slope / steepness;
                float upHeight = 0.f;
                glm::vec3 upNormal(0.f, 1.f, 0.f);
                for (float reach : {9.f, 18.f})
                    if (Engine::Components::SampleTerrainSurface(*context.heights, context.cellsX,
                            context.cellsY, context.cellWidth, context.cellDepth,
                            x + uphill.x * reach, z + uphill.y * reach, upHeight, &upNormal))
                        scree = std::max(scree, SmoothStep(.8f, .62f, upNormal.y));
                // On the gentler ground below, not on the face itself.
                scree *= SmoothStep(.6f, .8f, normal.y);
            }
        }
        const float odds = .025f + .45f * rocky + .3f * field + .25f * shore + .4f * scree;
        if (chance >= odds) continue;
        // Mostly small stones; boulders in rock fields, on rock paint and in scree.
        float scale = .35f + 2.4f * std::pow(size, 2.4f) *
            (.45f + .55f * std::max({rocky, field, scree}));
        if (shore > .5f) scale = .25f + .45f * size;
        // Rocks lean partway with the slope, so they read as resting on it.
        const glm::vec3 lean = glm::normalize(glm::mix(normal, glm::vec3(0.f, 1.f, 0.f), .45f));
        // Large rocks in fields and on rock paint: a generated boulder.
        // Not in scree: a cell has one boulder shape, and an apron of forty
        // copies of it under a cliff read as a field of columns.
        if (kind > .72f && scale > 1.2f && shore <= .5f && scree < .3f)
        {
            place(BoulderPrefab(standBoulder), x, height - .05f, z, lean, yaw,
                .55f + .5f * size);
            continue;
        }
        // Inside forest the rocks are mossy.
        if (forest > .5f && shore <= .5f)
        {
            const float mossScale = .18f + .4f * size * size;
            place(ScanPrefab("MossRock", standMossRocks[pick < .5f ? 0 : 1]), x,
                height - .12f * mossScale, z, lean, yaw, mossScale);
            continue;
        }
        const int rock = standRocks[pick < .5f ? 0 : 1];
        // Scree is fallen rock: it lies on its side at any angle (tipped 45
        // to 85 degrees), half buried, and stays under ~2 m. Standing
        // upright, the pack's taller rocks made an apron of forty columns
        // under the escarpment (captured 2026-11-20).
        if (scree > .3f && shore <= .5f)
        {
            scale = std::min(scale, .5f + 1.4f * size);
            const float tip = glm::radians(45.f + 40.f * kind);
            const float tipDirection = pick * 2.f * glm::two_pi<float>();
            const glm::vec3 tipped(std::sin(tip) * std::cos(tipDirection), std::cos(tip),
                std::sin(tip) * std::sin(tipDirection));
            place(RockPrefab(rock), x, height - .05f * scale, z, tipped, yaw, scale);
            continue;
        }
        place(RockPrefab(rock), x, height - .08f * scale, z, lean, yaw, scale);
    }
}
}

Altitudes AltitudesOf(const LandscapeDriver& driver)
{
    return {driver.waterLevel, driver.rockLine, driver.snowLine, driver.treeLine,
        std::clamp(driver.altitudeBlend, .1f, 50.f)};
}

// The erosion paint rules ("Macro color" properties), for the near cells'
// paint and the macro map alike (ApplyErosionPaint).
MacroColorRules MacroRulesOf(const LandscapeDriver& driver)
{
    MacroColorRules rules;
    rules.lushness = std::clamp(driver.macroLushness, 0.f, 1.f);
    rules.flowThreshold = std::clamp(driver.macroFlowThreshold, 0.f, .99f);
    rules.scree = std::clamp(driver.macroScree, 0.f, 1.f);
    rules.screeDepth = std::clamp(driver.macroScreeDepth, .1f, 100.f);
    rules.gullyRock = std::clamp(driver.macroGullyRock, 0.f, 1.f);
    rules.brightness = std::clamp(driver.macroBrightness, .1f, 2.f);
    return rules;
}

// Crown footprint of each tree at scale 1 (tools/measure_tree_crowns.py, from
// LOD0; re-run and paste when the models change): crown radius (m) and the
// area its leaves cover seen from above (m^2). The far canopy splats these
// from the real placements.
struct TreeCrownEntry { const char* asset; float radius, area; };
const std::array<TreeCrownEntry, 23> TreeCrown{{
    {"Oaks/Big_oak_tree_001", 8.68f, 140.7f}, {"Oaks/Big_oak_tree_002", 7.59f, 114.3f},
    {"Oaks/Big_oak_tree_003", 8.28f, 147.2f}, {"Oaks/Big_oak_tree_004", 9.93f, 205.2f},
    {"Oaks/Large_oak_tree_001", 11.35f, 260.2f}, {"Oaks/Large_oak_tree_002", 10.29f, 240.0f},
    {"Oaks/Large_oak_tree_003", 11.94f, 374.3f}, {"Oaks/Large_oak_tree_004", 9.38f, 182.4f},
    {"Oaks/Medium_oak_tree_001", 3.78f, 26.9f}, {"Oaks/Medium_oak_tree_002", 3.66f, 35.7f},
    {"Oaks/Medium_oak_tree_003", 3.80f, 39.2f}, {"Oaks/Sapling_oak_tree_001", 0.34f, 0.1f},
    {"Oaks/Sapling_oak_tree_002", 0.32f, 0.1f}, {"Oaks/Sapling_oak_tree_003", 0.36f, 0.1f},
    {"Oaks/Small_oak_tree_001", 1.72f, 5.7f}, {"Oaks/Small_oak_tree_002", 1.64f, 5.7f},
    {"Oaks/Small_oak_tree_003", 1.77f, 5.4f}, {"Conifers/Fir_subalpine_01", 1.70f, 12.0f},
    {"Conifers/Pine_scots_01", 3.26f, 25.2f}, {"Conifers/Spruce_tall_01", 2.87f, 34.4f},
    {"Conifers/Spruce_young_01", 1.96f, 14.2f}, {"Broadleaf/Broadleaf_slender_01", 2.28f, 4.5f},
    {"Broadleaf/Broadleaf_upright_01", 1.65f, 2.9f},
}};

const TreeCrownEntry* FindTreeCrown(const std::string& asset)
{
    for (const auto& entry : TreeCrown)
    {
        const std::string suffix = std::string(entry.asset) + ".prefab";
        if (asset.size() >= suffix.size() &&
            !asset.compare(asset.size() - suffix.size(), suffix.size(), suffix))
            return &entry;
    }
    return nullptr;
}

// One cell's paint as the near cells have it: the paint, the erosion paint
// (lushness) and the worn trail heights. Shared by the cell builder and the
// far canopy (so its forest generator sees the heights the cells' does).
void PaintCell(TerrainManager::Cell cell, int countX, int countY, float cellWidth, float cellDepth,
    const std::vector<uint8_t>& codes, const Altitudes& level, const MacroColorRules& rules,
    const TerrainHydrologyMap& flow, const TerrainHydrologyMap& deposition,
    std::vector<float>& heights, std::vector<float>& weights, std::vector<float>& lushness)
{
    const float originX = cell.first * countX * cellWidth;
    const float originZ = cell.second * countY * cellDepth;
    weights.assign(heights.size() * LayerCount, 0.f);
    for (int row = 0; row <= countY; ++row)
        for (int col = 0; col <= countX; ++col)
        {
            const size_t i = static_cast<size_t>(row) * (countX + 1) + col;
            PaintSample(heights[i], codes[i], originX + col * cellWidth,
                originZ + row * cellDepth, level, &weights[i * LayerCount]);
        }
    // Erosion paint, as the macro map has it: slope at the 16 m erosion grid
    // (four samples each way, one-sided at the cell edge).
    lushness.clear();
    if (!flow.Empty())
    {
        lushness.assign(heights.size(), 0.f);
        const auto heightAt = [&](int row, int col) {
            row = std::clamp(row, 0, countY); col = std::clamp(col, 0, countX);
            return heights[static_cast<size_t>(row) * (countX + 1) + col];
        };
        for (int row = 0; row <= countY; ++row)
            for (int col = 0; col <= countX; ++col)
            {
                const size_t i = static_cast<size_t>(row) * (countX + 1) + col;
                const int c0 = std::max(col - 4, 0), c1 = std::min(col + 4, countX);
                const int r0 = std::max(row - 4, 0), r1 = std::min(row + 4, countY);
                const float dx = (heightAt(row, c1) - heightAt(row, c0)) /
                    (std::max(c1 - c0, 1) * cellWidth);
                const float dz = (heightAt(r1, col) - heightAt(r0, col)) /
                    (std::max(r1 - r0, 1) * cellDepth);
                const float worldX = originX + col * cellWidth;
                const float worldZ = originZ + row * cellDepth;
                const float wet = SmoothStep(rules.flowThreshold, 1.f, flow.Normalized(worldX, worldZ));
                const float sediment = deposition.Empty() ? 0.f :
                    deposition.Normalized(worldX, worldZ) * deposition.scale;
                lushness[i] = ApplyErosionPaint(&weights[i * LayerCount], heights[i],
                    std::sqrt(dx * dx + dz * dz), wet, sediment, rules, level);
            }
    }
    // A worn track: trails sink a little into the ground (collision and
    // grass follow the cell heights). Only where the shaded path runs: off
    // rock, snow and shores and inside the forest band.
    if (ShaderTrails())
        for (int row = 0; row <= countY; ++row)
            for (int col = 0; col <= countX; ++col)
            {
                const size_t i = static_cast<size_t>(row) * (countX + 1) + col;
                const float* w = &weights[i * LayerCount];
                const float crossable = w[Gravel] + w[Grass] + w[ForestSoil];
                const float track = TrailMask(originX + col * cellWidth,
                    originZ + row * cellDepth) * crossable * ForestBand(heights[i], level);
                heights[i] -= .12f * track;
            }
}

LandscapeDriver::LandscapeDriver()
{
    SetTypeName(COMPONENT_TYPE_NAME(LandscapeDriver));
    RegisterField("viewerObjectName", viewerObjectName);
    RegisterField("mapHalfExtentCells", mapHalfExtentCells);
    RegisterField("heightArchive", heightArchive);
    RegisterField("farGrid", farGrid);
    RegisterField("cacheDirectory", cacheDirectory);
    RegisterField("enableFarTerrain", enableFarTerrain);
    RegisterField("waterLevel", waterLevel);
    RegisterField("rockLine", rockLine);
    RegisterField("snowLine", snowLine);
    RegisterField("treeLine", treeLine);
    RegisterField("altitudeBlend", altitudeBlend);
    RegisterField("treesPerCell", treesPerCell);
    RegisterField("forestLayerName", forestLayerName);
    RegisterField("grassRadius", grassRadius);
    RegisterField("propsLayerName", propsLayerName);
    RegisterField("undergrowthLayerName", undergrowthLayerName);
    RegisterField("propsDensity", propsDensity);
    RegisterField("enableWater", enableWater);
    RegisterField("waterMaterial", waterMaterial);
    RegisterField("enableMacroColors", enableMacroColors, "Macro color");
    RegisterField("flowMap", flowMap, "Macro color");
    RegisterField("depositionMap", depositionMap, "Macro color");
    RegisterField("macroLushness", macroLushness, "Macro color");
    RegisterField("macroFlowThreshold", macroFlowThreshold, "Macro color");
    RegisterField("macroScree", macroScree, "Macro color");
    RegisterField("macroScreeDepth", macroScreeDepth, "Macro color");
    RegisterField("macroGullyRock", macroGullyRock, "Macro color");
    RegisterField("macroBrightness", macroBrightness, "Macro color");
    RegisterFieldTooltip("enableMacroColors", "Compose the 16 m macro color map that distant terrain shows (needs TerrainManager > Macro color map). Rebuilds on a worker when these settings change.");
    RegisterFieldTooltip("flowMap", "Erosion flow map written by tools/build_landscape_demo.py (world.flow). Missing or from another map: no lush valleys or gullies.");
    RegisterFieldTooltip("depositionMap", "Erosion deposition map written by tools/build_landscape_demo.py (world.deposition). Missing or from another map: no scree.");
    RegisterFieldTooltip("macroLushness", "How strongly valley floors and streams (high erosion flow) turn a deeper green on meadow and forest floor.");
    RegisterFieldTooltip("macroFlowThreshold", "Normalized (logarithmic) flow where the lush tint and gully rock begin. 0 = everywhere water ran, 1 = only the largest streams.");
    RegisterFieldTooltip("macroScree", "How strongly sediment fans below rock and on alpine ground show as pale scree.");
    RegisterFieldTooltip("macroScreeDepth", "Metres of deposited sediment that give full scree.");
    RegisterFieldTooltip("macroGullyRock", "Steep slopes that carry a lot of water are scoured to bare rock; this sets how much.");
    RegisterFieldTooltip("macroBrightness", "Overall multiplier on the macro colors. Below 1 compensates for the occlusion and texture contrast the near shading adds.");
}

void LandscapeDriver::FenceBackgroundWork()
{
    if (m_farLoad.valid()) m_farLoad.wait();
}

uint64_t LandscapeDriver::SourceSignature() const
{
    const auto* manager = Owner ? Owner->GetComponent<TerrainManager>() : nullptr;
    if (!manager) return 0;
    uint64_t signature = 0;
    return TerrainHeightArchive::ReadSignature(
        Engine::Components::Mesh::ResolveFilePath(heightArchive), mapHalfExtentCells,
        manager->cellsX, manager->cellsY, signature) ? signature : 0;
}

bool LandscapeDriver::ConfigureTerrain()
{
    auto* manager = Owner ? Owner->GetComponent<TerrainManager>() : nullptr;
    if (!manager) return false;
    const uint64_t signature = SourceSignature();
    if (!signature) return false;
    const std::string archivePath = Engine::Components::Mesh::ResolveFilePath(heightArchive);
    const int countX = manager->cellsX, countY = manager->cellsY;
    const int extent = mapHalfExtentCells;
    const Altitudes level = AltitudesOf(*this);
    const int trees = std::clamp(treesPerCell, 0, 600);
    const std::string forestName = forestLayerName;
    const std::string propsName = propsLayerName, undergrowthName = undergrowthLayerName;
    const float propDensity = std::clamp(propsDensity, 0.f, 4.f);
    const float tiling = manager->cellsX * manager->cellWidth / 2.f; // 2 m repeats
    // Erosion paint for the near cells (ApplyErosionPaint): the same maps and
    // rules as the macro map. Missing or mismatched maps leave just the paint.
    const MacroColorRules erosionRules = MacroRulesOf(*this);
    auto hydrology = std::make_shared<std::pair<TerrainHydrologyMap, TerrainHydrologyMap>>();
    hydrology->first.Load(Engine::Components::Mesh::ResolveFilePath(flowMap),
        TerrainHydrologyMap::FlowMagic, signature);
    hydrology->second.Load(Engine::Components::Mesh::ResolveFilePath(depositionMap),
        TerrainHydrologyMap::DepositionMagic, signature);

    uint64_t paintKey = 0;
    for (float value : {level.water, level.rock, level.snow, level.tree, level.blend,
            erosionRules.lushness, erosionRules.flowThreshold, erosionRules.scree,
            erosionRules.screeDepth, erosionRules.gullyRock})
        paintKey = paintKey * 0x100000001b3ull ^
            static_cast<uint64_t>(std::llround(value * 16.f));
    if (!hydrology->first.Empty()) paintKey ^= 0x46574f4cull;
    // Trails leave the paint and carve the heights when shaded per pixel.
    // Rock shapes are part of the heights themselves (stamp brushes edit the
    // height data), so nothing else goes into the key.
    manager->SetStaticCache(cacheDirectory,
        signature ^ PaintRevision ^ paintKey ^
        (ShaderTrails() ? 0x5452414953484144ull : 0ull));
    // Trails as contour paths (ShaderTrails): the TrailMask noise, its track
    // widths, the gravel layer, over meadow, forest soil and gravel only, in
    // the forest's height band (ForestBand: off shores and above the tree line).
    {
        auto& paths = manager->contourPaths;
        paths.enabled = ShaderTrails();
        paths.periodA = 520.f; paths.weightA = .7f; paths.seedA = 71u;
        paths.periodB = 170.f; paths.weightB = .3f; paths.seedB = 73u;
        paths.contour = .5f; paths.core = 1.6f; paths.edge = 4.2f; paths.gradientFloor = .0006f;
        paths.layer = Gravel;
        paths.crossableLayers = (1u << Gravel) | (1u << Grass) | (1u << ForestSoil);
        paths.strength = .9f;
        paths.riseStart = level.water + 4.f; paths.riseEnd = level.water + 9.f;
        paths.fallStart = level.tree - 8.f * level.blend; paths.fallEnd = level.tree;
        paths.farColor = MakePalette(level).farColors[Gravel];
    }
    // Everything captured by value: these run on streaming workers.
    manager->SetProceduralBuilder([=](TerrainManager::Cell cell, Terrain& terrain)
    {
        terrain.textureTiling = tiling;
        terrain.layers.clear();
        for (int layer = 0; layer < LayerCount; ++layer)
            terrain.layers.push_back({LayerMaterials[layer], -1000.f, 1000.f,
                LayerPreview[layer]});
        std::vector<uint8_t> codes;
        if (!TerrainHeightArchive::ReadCellCodes(archivePath, signature, extent,
                countX, countY, cell.first, cell.second, terrain.heights, codes))
            throw std::runtime_error("Could not read landscape height cell");
        PaintCell(cell, countX, countY, terrain.cellWidth, terrain.cellDepth, codes, level,
            erosionRules, hydrology->first, hydrology->second,
            terrain.heights, terrain.weights, terrain.lushness);
        terrain.Rebuild();
    });
    manager->SetObjectLayerGenerator([=](const TerrainManager::ObjectLayerContext& context,
        const std::string& layerName, TerrainObjectLayer& layer)
    {
        if (layerName == forestName) GenerateForest(context, layer, level, trees);
        // Props end inside the loaded cells; far cells never need them.
        else if (context.farObjects) return;
        else if (!propsName.empty() && layerName == propsName)
        {
            GenerateProps(context, layer, level, false, propDensity);
            // Bushes join the props layer unless they have their own.
            if (undergrowthName.empty() || undergrowthName == propsName)
                GenerateProps(context, layer, level, true, propDensity);
        }
        else if (!undergrowthName.empty() && layerName == undergrowthName)
            GenerateProps(context, layer, level, true, propDensity);
    });
    // The meadow preset attaches a GrassField to the procedural grass layer;
    // keep the grass on the meadow paint and give it this map's density.
    const float radius = grassRadius;
    const std::string viewer = manager->viewerObjectName;
    manager->SetObjectLayerSetup([radius, forestName, viewer](TerrainManager::Cell, TerrainObjectLayer& layer)
    {
        MeadowGrass::Apply(layer, viewer);
        // Forests budget their detail in the project's "Trees" layer.
        if (layer.Owner && layer.layerName == forestName)
            layer.Owner->layer = Engine::Rendering::DetailBudget::Get().FindLayerOrDefault("Trees");
        auto* grass = layer.Owner ? layer.Owner->GetComponent<GrassField>() : nullptr;
        if (!grass) return;
        // Scanned bushes, shrubs and ferns (textured fields of their own) grow
        // on the meadow and the forest soil, fern thickets on the forest soil;
        // the grass fields below drop their generated bushes, shrubs and
        // ferns (ENGINE_SCAN_BUSHES=0 keeps the generated ones for A/B runs).
        static const bool scanBushes = []
        { const char* v = std::getenv("ENGINE_SCAN_BUSHES"); return !(v && v[0] == '0'); }();
        const bool ferns = layer.layerName == MeadowGrass::FernLayerName;
        if (ferns || layer.layerName == MeadowGrass::BushLayerName)
        {
            const int mask = !scanBushes ? 0 : ferns ? 1 << ForestSoil : (1 << Grass) | (1 << ForestSoil);
            // Full reach: the detail budget sits at its floor here, which
            // would halve it (see the ground cover below).
            // Each plant picks its own detail level (as the ground cover
            // does). 32 m fern tiles (fewer draws) were not all built 60
            // frames in, and would arrive a whole tile at a time.
            const float tile = 16.f;
            // Flowers and herbs (the fields' accents, a "fine" class) end
            // sooner than the plants: ~40 m on the meadow, 30 m in the forest.
            const float flowerReach = ferns ? .6f : .5f;
            if (grass->proceduralLayerMask != mask || grass->minimumRadiusScale != 1.f ||
                !grass->perPlantDetail || grass->tileSize != tile ||
                grass->fineReachFraction != flowerReach)
            {
                grass->minimumRadiusScale = 1.f;
                grass->perPlantDetail = true;
                grass->tileSize = tile;
                grass->fineReachFraction = flowerReach;
                grass->proceduralLayerMask = mask;
                if (!scanBushes)
                {
                    grass->bushesPerSquareMeter = 0.f;
                    grass->shrubPatchesPerSquareMeter = 0.f;
                    grass->accentPatchesPerSquareMeter = 0.f;
                }
                grass->MarkConfigurationDirty();
            }
            // Shadows of the scanned plants (alpha-tested cards) out to their
            // reach (ENGINE_SCAN_BUSH_SHADOWS=1).
            static const bool scanShadows = []
            { const char* v = std::getenv("ENGINE_SCAN_BUSH_SHADOWS"); return v && v[0] == '1'; }();
            if (scanShadows && !grass->castShadows)
            {
                grass->castShadows = true;
                layer.shadowDistance = LandscapeShadowRange("ENGINE_SCAN_BUSH_SHADOW_DISTANCE", 150.f);
            }
            return;
        }
        if (scanBushes && !grass->bushMeshes.empty())
        {
            grass->bushMeshes.clear();
            grass->bushesPerSquareMeter = 0.f;
            // The meadow keeps its tall grass clumps (one of its four shrub
            // species, at a quarter of the density). The forest floor's only
            // shrub was its fern (its herb, also listed, is its tall plant and
            // so skipped as a shrub); the fern layer replaces it.
            if (layer.layerName == "Procedural meadow grass")
            {
                grass->shrubMeshes = "Assets/Mesh/Grass/tall_grass_clump.obj";
                grass->shrubPatchesPerSquareMeter *= .25f;
            }
            else if (layer.layerName == MeadowGrass::ForestFloorLayerName)
            {
                grass->shrubMeshes.clear();
                grass->shrubPatchesPerSquareMeter = 0.f;
                // Half the generated herbs: scanned periwinkle and weeds grow
                // between them (the fern layer's accents).
                grass->tallPatchesPerSquareMeter *= .5f;
            }
            grass->MarkConfigurationDirty();
        }
        if (layer.layerName == MeadowGrass::FarLayerName)
        {
            // The far ring is fully in before the detailed grass (radius +
            // 10, fading over its last 30%) starts to fade: their fades must
            // overlap or a band of bare ground shows between them.
            const float detailed = radius + 10.f;
            // GPU blades reach the full detailed radius and shrink away over
            // its last 10 m (GrassBlades::Settings::fadeBand): the ring fades
            // in over the same band instead of overlapping 30-60 m of blades.
            const bool blades = GrassBlades::Enabled();
            const float inner = blades ? detailed - 10.f : detailed * .5f;
            const float innerFade = blades ? 10.f : detailed * .2f;
            if (grass->innerRadius != inner || grass->innerFadeWidth != innerFade ||
                grass->proceduralLayerMask != (1 << Grass))
            {
                grass->innerRadius = inner;
                grass->innerFadeWidth = innerFade;
                grass->proceduralLayerMask = 1 << Grass;
                grass->MarkConfigurationDirty();
            }
            // The tall fields carry on into the far ring: taller, denser tufts
            // (far_tall_tuft, 0.75-1.05 m) where the detailed grass grows its
            // tall fields, the short tufts thinning under them. The same seed
            // and field noise as the meadow field (MeadowGrass, tall fields
            // below), so the fields line up across the hand-off.
            // ENGINE_LANDSCAPE_FAR_TALL=0 keeps the uniform ring (A/B).
            static const bool farTall = []
            { const char* v = std::getenv("ENGINE_LANDSCAPE_FAR_TALL"); return !(v && v[0] == '0'); }();
            const bool tallFieldsOn = []
            { const char* v = std::getenv("ENGINE_LANDSCAPE_TALL_FIELDS"); return !(v && v[0] == '0'); }();
            if (farTall && tallFieldsOn && grass->tallFieldDensity == 0.f)
            {
                // Its levels thin like the far tufts' (45 / 90 m, MeadowGrass::ApplyFar).
                grass->tallMesh = "Assets/Mesh/Grass/far_tall_tuft.obj";
                grass->tallMeshLod1 = "Assets/Mesh/Grass/far_tall_tuft_lod1.obj";
                grass->tallMeshLod2 = "Assets/Mesh/Grass/far_tall_tuft_lod2.obj";
                grass->seed = 18731;
                grass->tallFieldDensity = .9f;
                grass->tallFieldSize = 28.f;
                grass->tallFieldCoverage = .85f;
                grass->shortUnderTallShare = .3f;
                grass->MarkConfigurationDirty();
            }
            return;
        }
        // Ground cover keeps its authored reach: with the detail budget at its
        // floor (it always is here) the radius shrank to ~16 m and ferns,
        // bushes and grass came into being a few metres ahead of the camera.
        // Larger tiles than the preset's 4 m pay for it (fewer tile objects)
        // (ENGINE_LANDSCAPE_GRASS_TILE=<m> and ENGINE_LANDSCAPE_GRASS_FLOOR=<scale> for A/B runs).
        // Tiles grow with the reach so their count stays near 170-210.
        static const float forcedTile = []
        { const char* v = std::getenv("ENGINE_LANDSCAPE_GRASS_TILE"); return v ? std::clamp(float(std::atof(v)), 2.f, 32.f) : 0.f; }();
        const float grassTile = forcedTile > 0.f ? forcedTile : radius >= 40.f ? 16.f : 8.f;
        static const float grassFloor = []
        { const char* v = std::getenv("ENGINE_LANDSCAPE_GRASS_FLOOR"); return v ? std::clamp(float(std::atof(v)), .25f, 2.f) : 1.f; }();
        if (grass->tileSize != grassTile || grass->minimumRadiusScale != grassFloor)
        {
            grass->tileSize = grassTile;
            grass->minimumRadiusScale = grassFloor;
            grass->MarkConfigurationDirty();
        }
        // A tile takes its detail level from the distance to its own origin,
        // so a 16 m tile underfoot drew the thinned level and the meadow
        // showed soil (6.9 % of the lower frame against 4.5 % with 4 m tiles).
        // The level distances move out by part of the extra tile size: 0.35
        // of it brings the soil back to 4.9 % for 5.5 ms of grass GPU time at
        // the forest edge (3.0 with 4 m tiles); 0.7 gives 4.7 % for 7.9 ms.
        // The lasting fix is a detail level per placement, as trees have
        // (ENGINE_LANDSCAPE_GRASS_LODBOOST=<factor> for A/B runs).
        // With a detail level per plant (the default) no push is needed.
        // ENGINE_LANDSCAPE_GRASS_PER_PLANT=0 goes back to a level per tile.
        static const bool perPlant = []
        { const char* v = std::getenv("ENGINE_LANDSCAPE_GRASS_PER_PLANT"); return !(v && v[0] == '0'); }();
        if (grass->perPlantDetail != perPlant) { grass->perPlantDetail = perPlant; grass->MarkConfigurationDirty(); }
        // GPU grass blades (default; ENGINE_GPU_GRASS=0 off, GrassBlades.h): the meadow's
        // fine grass and tall fields are generated on the GPU; its tiles keep
        // the accents and shrubs.
        if (layer.layerName == "Procedural meadow grass" && grass->gpuBlades != GrassBlades::Enabled())
        {
            grass->gpuBlades = GrassBlades::Enabled();
            // Its remaining shrub, tall_grass_clump (2,448 / 1,015 / 241
            // triangles), is grass the blades already draw.
            if (grass->gpuBlades && grass->shrubMeshes.find("tall_grass_clump") != std::string::npos)
            {
                grass->shrubMeshes.clear();
                grass->shrubPatchesPerSquareMeter = 0.f;
            }
            grass->MarkConfigurationDirty();
        }
        static const float lodBoost = []
        { const char* v = std::getenv("ENGINE_LANDSCAPE_GRASS_LODBOOST"); return v ? std::clamp(float(std::atof(v)), 0.f, 2.f) : perPlant ? 0.f : .35f; }();
        const float grassLod1 = 7.f + lodBoost * (grassTile - 4.f), grassLod2 = 13.f + lodBoost * (grassTile - 4.f);
        if (layer.layerName != MeadowGrass::FarLayerName &&
            (layer.lod1Distance != grassLod1 || layer.lod2Distance != grassLod2))
        {
            layer.lod1Distance = grassLod1;
            layer.lod2Distance = grassLod2;
            layer.MarkConfigurationDirty();
        }
        // Reach by plant size: ferns, shrubs and bushes are large enough to be
        // seen arriving, so they reach further than the fine grass, which
        // still ends at radius + 10 (ENGINE_LANDSCAPE_SHRUB_REACH=<m>, 0 = same
        // reach as the grass). Walking, 80 m costs ~0.3 ms in the forest and
        // ~1 ms on the meadow; 100 m costs ~0.7 ms in the forest.
        static const float shrubReach = []
        { const char* v = std::getenv("ENGINE_LANDSCAPE_SHRUB_REACH"); return v ? std::clamp(float(std::atof(v)), 0.f, 300.f) : 80.f; }();
        const float coverReach = std::max(radius + 10.f, shrubReach);
        const float fineReach = (radius + 10.f) / coverReach;
        if (grass->fineReachFraction != fineReach) { grass->fineReachFraction = fineReach; grass->MarkConfigurationDirty(); }
        if (layer.layerName == MeadowGrass::ForestFloorLayerName)
        {
            // Litter, herbs and ferns grow on the forest soil (the paint the
            // trees stand on).
            if (layer.viewDistance != coverReach)
            {
                layer.viewDistance = coverReach;
                layer.MarkConfigurationDirty();
            }
            if (grass->proceduralLayerMask != (1 << ForestSoil) || grass->visibleRadius != radius)
            {
                grass->proceduralLayerMask = 1 << ForestSoil;
                grass->visibleRadius = radius;
                grass->MarkConfigurationDirty();
            }
            return;
        }
        // Layer distances every restore (cheap; dirty only on change).
        if (layer.viewDistance != coverReach)
        {
            layer.viewDistance = coverReach;
            layer.MarkConfigurationDirty();
        }
        if (grass->proceduralLayerMask == (1 << Grass)) return;
        grass->proceduralLayerMask = 1 << Grass;
        grass->visibleRadius = radius;
        // Tussock patches (the manager's procedural default) close over the
        // ground like a pelt: crowns of blades over a low sward.
        grass->shortPatchesPerSquareMeter = 3.5f;
        grass->tallPatchesPerSquareMeter = .4f;
        // Tall grass fields (the reference meadows: knee- to waist-high fine
        // grass as one mass, the ground hidden except from steeply above).
        // Overlapping metre-wide patches (tools/generate_meadow_grass.py
        // meadow_tall_grass) over most of the meadow (reference frame 03 shows
        // no ground at all), densest in each field's heart; the short sward
        // thins under them and stays in the openings between.
        // ENGINE_LANDSCAPE_TALL_FIELDS=0 keeps the old meadow (A/B).
        static const bool tallFields = []
        { const char* v = std::getenv("ENGINE_LANDSCAPE_TALL_FIELDS"); return !(v && v[0] == '0'); }();
        if (tallFields)
        {
            grass->tallMesh = "Assets/Mesh/Grass/meadow_tall_grass.obj";
            grass->tallMeshLod1 = "Assets/Mesh/Grass/meadow_tall_grass_lod1.obj";
            grass->tallMeshLod2 = "Assets/Mesh/Grass/meadow_tall_grass_lod2.obj";
            grass->tallPatchesPerSquareMeter = .25f;
            grass->tallFieldDensity = 2.2f;
            grass->tallFieldSize = 28.f;
            grass->tallFieldCoverage = .85f;
            grass->shortUnderTallShare = .25f;
            // What shows between blades reads as the mass's shaded depth.
            grass->groundRootShade = .85f;
        }
        // Wildflower and weed drifts (meshes come from the meadow preset).
        grass->accentPatchesPerSquareMeter = .18f;
        // Low shrub, bramble and fern thickets over about a fifth of the meadow.
        grass->shrubPatchesPerSquareMeter = .7f;
        grass->shrubThicketSize = 11.f;
        grass->shrubCoverage = .22f;
        // Bush clusters over about a quarter of the meadow.
        grass->bushesPerSquareMeter = .12f;
        grass->bushClusterSize = 16.f;
        grass->bushCoverage = .35f;
        grass->MarkConfigurationDirty();
    });
    m_configuredSignature = signature;
    RegisterMacroColors(*manager);
    manager->ClearFarTerrain();
    m_farLoad = {};
    m_farAttempted = false;
    m_farDataReady = false;
    return true;
}

uint64_t LandscapeDriver::MacroColorRevision() const
{
    if (!enableMacroColors || !m_configuredSignature) return 0;
    uint64_t revision = m_configuredSignature ^ 0x4d4143524f434c52ull;
    const auto mix = [&](uint64_t value) { revision = (revision ^ value) * 0x100000001b3ull; };
    for (float value : {macroLushness, macroFlowThreshold, macroScree, macroScreeDepth,
            macroGullyRock, macroBrightness, waterLevel, rockLine, snowLine, treeLine,
            altitudeBlend})
        mix(static_cast<uint64_t>(std::llround(double(value) * 4096.0)));
    mix(static_cast<uint64_t>(treesPerCell)); // the canopy is splatted from the trees
    mix(std::hash<std::string>{}(flowMap));
    mix(std::hash<std::string>{}(depositionMap));
    mix(static_cast<uint64_t>(mapHalfExtentCells));
    return revision ? revision : 1;
}

void LandscapeDriver::RegisterMacroColors(TerrainManager& manager)
{
    TerrainManager::MacroColorSource source;
    source.revision = [this] { return MacroColorRevision(); };
    source.makeBuild = [this]() -> std::function<TerrainManager::MacroColorImage()>
    {
        const auto* manager = Owner ? Owner->GetComponent<TerrainManager>() : nullptr;
        if (!manager || !m_configuredSignature) return {};
        // Snapshot everything: the build runs on a worker.
        const std::string archive = Engine::Components::Mesh::ResolveFilePath(heightArchive);
        const std::string flowPath = Engine::Components::Mesh::ResolveFilePath(flowMap);
        const std::string depositionPath = Engine::Components::Mesh::ResolveFilePath(depositionMap);
        const uint64_t signature = m_configuredSignature;
        const int extent = mapHalfExtentCells;
        const int cellsX = manager->cellsX, cellsY = manager->cellsY;
        const float cellWidth = manager->cellWidth, cellDepth = manager->cellDepth;
        const Altitudes level = AltitudesOf(*this);
        // Layer means come from material files: read them here, not on the job.
        const auto means = MakePalette(level).nearColors;
        const MacroColorRules rules = MacroRulesOf(*this);
        const int trees = std::clamp(treesPerCell, 0, 600);
        return [=]
        {
            // Missing or mismatched erosion maps leave just the paint.
            TerrainHydrologyMap flowSamples, depositionSamples;
            flowSamples.Load(flowPath, TerrainHydrologyMap::FlowMagic, signature);
            depositionSamples.Load(depositionPath, TerrainHydrologyMap::DepositionMagic, signature);
            int side = 0;
            std::vector<glm::vec3> normals;
            // Canopy cover from the real trees: each cell painted as the
            // cells are (PaintCell), its forest generated as the far blocks
            // generate it, every tree's crown (TreeCrown, at its scale)
            // splatted into the map's 16 m grid as covered area.
            const float spacing = cellWidth * 4.f;
            const float origin = -float(extent) * cellsX * cellWidth;
            const int gridSide = (extent * 2 + 1) * (cellsX / 4) + 1;
            std::vector<float> covered(static_cast<size_t>(gridSide) * gridSide, 0.f);
            const auto splatTrees = [&](int cx, int cy, const std::vector<float>& archiveHeights,
                const std::vector<uint8_t>& codes)
            {
                std::vector<float> heights = archiveHeights, weights, lushness;
                PaintCell({cx, cy}, cellsX, cellsY, cellWidth, cellDepth, codes, level, rules,
                    flowSamples, depositionSamples, heights, weights, lushness);
                TerrainManager::ObjectLayerContext context;
                context.cell = {cx, cy};
                context.cellsX = cellsX; context.cellsY = cellsY;
                context.cellWidth = cellWidth; context.cellDepth = cellDepth;
                context.heights = &heights;
                context.weights = &weights;
                context.terrainLayerCount = LayerCount;
                context.farObjects = true;
                Engine::Core::Object layerObject;
                auto* layer = layerObject.AddComponent<TerrainObjectLayer>();
                GenerateForest(context, *layer, level, trees);
                for (const auto& placement : layer->placements)
                {
                    const auto* crown = FindTreeCrown(placement.asset);
                    const float scale = placement.scale;
                    const float radius = (crown ? crown->radius : 3.f) * scale + .5f * spacing;
                    const float area = (crown ? crown->area : 20.f) * scale * scale;
                    const float wx = cx * cellsX * cellWidth + placement.position.x;
                    const float wz = cy * cellsY * cellDepth + placement.position.z;
                    const int i0 = std::max(0, int(std::floor((wx - radius - origin) / spacing)));
                    const int i1 = std::min(gridSide - 1, int(std::ceil((wx + radius - origin) / spacing)));
                    const int j0 = std::max(0, int(std::floor((wz - radius - origin) / spacing)));
                    const int j1 = std::min(gridSide - 1, int(std::ceil((wz + radius - origin) / spacing)));
                    float total = 0.f;
                    for (int j = j0; j <= j1; ++j)
                        for (int i = i0; i <= i1; ++i)
                            total += std::max(0.f, 1.f - std::hypot(origin + i * spacing - wx,
                                origin + j * spacing - wz) / radius);
                    if (total <= 0.f) continue;
                    for (int j = j0; j <= j1; ++j)
                        for (int i = i0; i <= i1; ++i)
                        {
                            const float w = std::max(0.f, 1.f - std::hypot(origin + i * spacing - wx,
                                origin + j * spacing - wz) / radius);
                            covered[static_cast<size_t>(j) * gridSide + i] += area * w / total;
                        }
                }
            };
            auto colors = ComputeMacroColors(archive, signature, extent, cellsX, cellsY,
                cellWidth, cellDepth, level, means, flowSamples, depositionSamples, rules, side, normals,
                splatTrees);
            if (side == gridSide)
                for (size_t i = 0; i < colors.size(); ++i)
                    colors[i].a = 1.f - std::exp(-covered[i] / (spacing * spacing));
            return TerrainManager::EncodeMacroColorImage(side, side, origin, origin,
                spacing, colors, &normals);
        };
    };
    manager.SetMacroColorSource(std::move(source));
}

void LandscapeDriver::OnDestroy()
{
    if (auto* manager = Owner ? Owner->GetComponent<TerrainManager>() : nullptr)
        manager->SetMacroColorSource({});
}

void LandscapeDriver::Start()
{
    m_farLoad = {};
    m_farAttempted = false;
    m_farDataReady = !enableFarTerrain || farGrid.empty();
}

void LandscapeDriver::OnAfterDeserialize(IGraphicsProvider*)
{
    auto* manager = Owner ? Owner->GetComponent<TerrainManager>() : nullptr;
    if (!manager || m_previewLoaded || !ConfigureTerrain()) return;
    const float spanX = manager->cellsX * manager->cellWidth;
    const float spanZ = manager->cellsY * manager->cellDepth;
    // The editor has no streaming loop: load the cells around the explorer
    // and the far silhouette so the map is visible as soon as it opens.
    glm::dvec3 start(0.0);
    if (const auto* viewer = Owner->FindObjectInSceneByName(viewerObjectName))
        start = glm::dvec3(glm::inverse(Owner->transform.GetWorldMatrixD()) *
            glm::dvec4(viewer->transform.GetWorldPositionD(), 1.0));
    const int previousFarRadius = manager->farRadius;
    manager->farRadius = manager->loadedRadius;
    manager->SyncLoadedCells(static_cast<int>(std::floor(start.x / spanX)),
        static_cast<int>(std::floor(start.z / spanZ)));
    manager->farRadius = previousFarRadius;
    if (enableFarTerrain && !farGrid.empty())
    {
        FarGridLoad result;
        result.loaded = TerrainHeightArchive::ReadFarGrid(
            Engine::Components::Mesh::ResolveFilePath(farGrid), m_configuredSignature,
            mapHalfExtentCells, result.heights, &result.patches);
        if (!EnvironmentSwitch("ENGINE_FAR_PATCHES")) result.patches.clear();
        if (result.loaded)
        {
            auto palette = MakePalette(AltitudesOf(*this));
            palette.gridColors = ComputeFarColors(
                Engine::Components::Mesh::ResolveFilePath(heightArchive), m_configuredSignature,
                mapHalfExtentCells, manager->cellsX, manager->cellsY, manager->cellWidth,
                manager->cellDepth, AltitudesOf(*this), palette.nearColors);
            palette.gridHalfExtent = mapHalfExtentCells;
            manager->ConfigureFarTerrain(mapHalfExtentCells, std::move(result.heights),
                std::move(result.patches),
                Engine::Components::Mesh::ResolveFilePath(heightArchive),
                m_configuredSignature, std::move(palette));
            m_farDataReady = true;
        }
    }
    if (auto* scene = Owner->GetScene())
    {
        scene->editorCamera.transform.position = start + glm::dvec3{0.0, 60.0, -90.0};
        if (auto* camera = scene->editorCamera.GetComponent<Engine::Components::Camera>())
        {
            camera->target = glm::vec3(start);
            // See the whole map from its centre.
            camera->farPlane = std::max(8000.f, 1.5f * (mapHalfExtentCells + 1) * std::max(spanX, spanZ));
        }
    }
    m_previewLoaded = true;
}

void LandscapeDriver::UpdateWater()
{
    auto* manager = Owner ? Owner->GetComponent<TerrainManager>() : nullptr;
    auto* scene = Owner ? Owner->GetScene() : nullptr;
    if (!manager || !scene) return;
    auto* existing = m_waterObjectId ? scene->FindObjectById(m_waterObjectId) : nullptr;
    const float spanX = manager->cellsX * manager->cellWidth;
    const float spanZ = manager->cellsY * manager->cellDepth;
    uint64_t signature = 1469598103934665603ull;
    const auto mix = [&](const void* data, size_t size) {
        for (size_t i = 0; i < size; ++i)
            signature = (signature ^ static_cast<const uint8_t*>(data)[i]) * 1099511628211ull;
    };
    mix(&waterLevel, sizeof(waterLevel)); mix(&mapHalfExtentCells, sizeof(mapHalfExtentCells));
    mix(&spanX, sizeof(spanX)); mix(&spanZ, sizeof(spanZ));
    mix(waterMaterial.data(), waterMaterial.size());
    if (!enableWater || waterMaterial.empty())
    {
        if (existing) scene->RemoveObject(existing);
        m_waterObjectId = 0;
        return;
    }
    if (existing && signature == m_waterSignature) return;
    if (existing) scene->RemoveObject(existing);

    // One flat grid over the map at the water level. Terrain above the level
    // hides it; the carved lake basins show it.
    auto* object = scene->AddObject("Lake water");
    object->runtimeOnly = true;
    object->layer = Engine::Rendering::DetailBudget::Get().FindLayerOrDefault("Water");
    scene->MoveObject(object, Owner, Engine::Scene::Scene::ObjectPlacement::AsChild);
    object->transform.position = { 0.0, static_cast<double>(waterLevel), 0.0 };
    const float minX = -mapHalfExtentCells * spanX, maxX = (mapHalfExtentCells + 1) * spanX;
    const float minZ = -mapHalfExtentCells * spanZ, maxZ = (mapHalfExtentCells + 1) * spanZ;
    constexpr int quads = 16;
    std::vector<Engine::Components::Mesh::Vertex> vertices;
    std::vector<uint32_t> indices;
    for (int z = 0; z <= quads; ++z)
        for (int x = 0; x <= quads; ++x)
        {
            Engine::Components::Mesh::Vertex vertex{};
            vertex.pos[0] = minX + (maxX - minX) * x / quads;
            vertex.pos[2] = minZ + (maxZ - minZ) * z / quads;
            vertex.normal[1] = 1.f;
            vertex.uv[0] = vertex.pos[0] / 16.f;
            vertex.uv[1] = vertex.pos[2] / 16.f;
            vertex.tangent[0] = 1.f; vertex.tangent[3] = 1.f;
            vertices.push_back(vertex);
        }
    for (int z = 0; z < quads; ++z)
        for (int x = 0; x < quads; ++x)
        {
            const uint32_t a = z * (quads + 1) + x, b = a + 1;
            const uint32_t c = a + quads + 1, d = c + 1;
            indices.insert(indices.end(), { a, c, b, b, c, d });
        }
    auto* mesh = object->AddComponent<Engine::Components::Mesh>();
    mesh->SetIndexedGeometry(std::move(vertices), std::move(indices));
    auto* material = object->AddComponent<Engine::Components::Material>();
    auto settings = Engine::Serialization::JsonValue::MakeObject();
    settings.Set("materialAsset", Engine::Serialization::JsonValue(waterMaterial));
    material->Deserialize(settings);
    if (auto* provider = scene->GetGraphicsProvider())
    {
        mesh->OnAfterDeserialize(provider);
        material->OnAfterDeserialize(provider);
    }
    m_waterObjectId = scene->GetObjectId(object);
    m_waterSignature = signature;
}

void LandscapeDriver::Update()
{
    auto* manager = Owner ? Owner->GetComponent<TerrainManager>() : nullptr;
    if (!manager) return;
    UpdateWater();
    auto* scene = Owner->GetScene();
    if (scene && !scene->HasStarted() && SourceSignature() != m_configuredSignature &&
        !ConfigureTerrain()) return;
    if (!scene || !scene->HasStarted()) return;
    if (!enableFarTerrain)
    {
        manager->SetFarTerrainEnabled(false);
        return;
    }
    if (!m_farAttempted)
    {
        m_farAttempted = true;
        const std::string path = Engine::Components::Mesh::ResolveFilePath(farGrid);
        const std::string archive = Engine::Components::Mesh::ResolveFilePath(heightArchive);
        const uint64_t signature = m_configuredSignature;
        const int extent = mapHalfExtentCells;
        const int cellsX = manager->cellsX, cellsY = manager->cellsY;
        const float cellWidth = manager->cellWidth, cellDepth = manager->cellDepth;
        const Altitudes level = AltitudesOf(*this);
        // Layer means come from material files: read them here, not on the job.
        const auto means = MakePalette(level).nearColors;
        m_farLoad = SubmitJob([=]
        {
            FarGridLoad result;
            result.loaded = TerrainHeightArchive::ReadFarGrid(path, signature, extent,
                result.heights, &result.patches);
            if (!EnvironmentSwitch("ENGINE_FAR_PATCHES")) result.patches.clear();
            if (result.loaded)
                result.gridColors = ComputeFarColors(archive, signature, extent, cellsX,
                    cellsY, cellWidth, cellDepth, level, means);
            return result;
        });
    }
    if (m_farLoad.valid() &&
        m_farLoad.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
    {
        FarGridLoad result = m_farLoad.get();
        if (result.loaded)
        {
            auto palette = MakePalette(AltitudesOf(*this));
            palette.gridColors = std::move(result.gridColors);
            palette.gridHalfExtent = mapHalfExtentCells;
            manager->ConfigureFarTerrain(mapHalfExtentCells, std::move(result.heights),
                std::move(result.patches),
                Engine::Components::Mesh::ResolveFilePath(heightArchive),
                m_configuredSignature, std::move(palette));
            m_farDataReady = true;
        }
    }
    manager->SetFarTerrainEnabled(true);
    if (auto* viewer = Owner->FindObjectInSceneByName(viewerObjectName))
    {
        const glm::dvec3 local = glm::dvec3(glm::inverse(Owner->transform.GetWorldMatrixD()) *
            glm::dvec4(viewer->transform.GetWorldPositionD(), 1.0));
        const double spanX = double(manager->cellsX) * manager->cellWidth;
        const double spanZ = double(manager->cellsY) * manager->cellDepth;
        if (std::isfinite(local.x) && std::isfinite(local.z))
            manager->UpdateFarTerrain(static_cast<int>(std::floor(local.x / spanX)),
                static_cast<int>(std::floor(local.z / spanZ)),
                viewer->GetComponent<Engine::Components::Camera>());
    }
}

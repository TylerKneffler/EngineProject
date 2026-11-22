#include "Core/Rendering/DetailBudget.h"
#include "Scripts/Landscape/LandscapeDriver.h"
#include "Grass/GrassField.h"
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
constexpr uint64_t PaintRevision = 0x4c414e445343503aull; // ...39: before stamp proxies (2026-11-21)

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
float TrailMask(float x, float z)
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
    const float distance = std::abs(centre - .5f) / std::max(std::sqrt(gx * gx + gz * gz), .0006f);
    return 1.f - SmoothStep(1.6f, 4.2f, distance);
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
    // shores or steep ground (rockSlope).
    const float trail = TrailMask(worldX, worldZ) * land * (1.f - rock) * (1.f - snow) *
        ForestBand(height, a);
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

// Far-tile colors per cell corner: the near paint averaged over the corner's
// cell-sized neighborhood, with forest canopy over forest soil, so distant
// terrain shows the forests, rock, snow and lake beds the near cells show.
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
    // Canopy seen from afar: dark leaf green.
    const glm::vec3 canopy(.030f, .052f, .020f);
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
                // Forest soil is .85 of the forest share; trees cover it.
                color = glm::mix(color, canopy, std::min(1.f, weights[ForestSoil] / .85f));
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

// A cliff piece as the macro maps see it (see ComputeMacroColors): where it
// stands, how far its rock reaches across the ground, its stamp's mean colour
// (terrainMacroColor) and the way its wall faces.
struct CliffMark
{
    glm::vec2 position{0.f};
    float radius = 0.f;
    glm::vec3 color{0.f}, facing{0.f, 1.f, 0.f};
};

// The macro color map: the near paint every fourth sample (16 m on the demo
// map, the generator's erosion grid), shaped by erosion. Valley floors and
// streams that carried a lot of water turn lush, steep gullies scour to bare
// rock, and sediment fans show as scree; forest canopy covers forest soil
// (alpha = canopy cover). Rows along +Z from the map's -X/-Z corner.
std::vector<glm::vec4> ComputeMacroColors(const std::string& archivePath, uint64_t signature,
    int extent, int cellsX, int cellsY, float cellWidth, float cellDepth, const Altitudes& a,
    const std::array<glm::vec3, LayerCount>& means, const TerrainHydrologyMap& flow,
    const TerrainHydrologyMap& deposition, const MacroColorRules& rules,
    const std::vector<CliffMark>& cliffs, int& side, std::vector<glm::vec3>& normals)
{
    side = 0;
    normals.clear();
    if (cellsX != cellsY || cellsX % 4 != 0) return {};
    const int step = 4;
    const int perCell = cellsX / step;
    side = (extent * 2 + 1) * perCell + 1;
    std::vector<glm::vec4> colors(static_cast<size_t>(side) * side, glm::vec4(.1f, .1f, .1f, 0.f));
    normals.assign(colors.size(), glm::vec3(0.f, 1.f, 0.f));
    const glm::vec3 canopy(.030f, .052f, .020f);
    const glm::vec3 lush = means[Grass] * glm::vec3(.72f, .92f, .55f);
    const glm::vec3 screeColor = glm::mix(means[Gravel], means[Rock], .4f) * 1.15f;
    const bool read = TerrainHeightArchive::ForEachCell(archivePath, signature, extent,
        cellsX, cellsY, [&](int cx, int cy, const std::vector<float>& heights,
            const std::vector<uint8_t>& codes)
    {
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
                // Lush valley floors on meadow and under forest.
                const float vegetated = weights[Grass] + weights[ForestSoil];
                color = glm::mix(color, lush, rules.lushness * wet * vegetated *
                    (1.f - SmoothStep(.35f, .7f, slope)));
                // Scree where sediment settled below rock and on alpine ground.
                const float alpine = std::max(weights[Rock] + weights[Gravel],
                    SmoothStep(a.tree, a.tree + 12.f * a.blend, heights[index]));
                const float sediment = deposition.Normalized(worldX, worldZ) * deposition.scale;
                color = glm::mix(color, screeColor, rules.scree * alpine *
                    std::clamp(sediment / std::max(rules.screeDepth, .1f), 0.f, 1.f) *
                    (1.f - weights[Snow]));
                // Steep, heavily drained slopes: bare rock gullies.
                color = glm::mix(color, means[Rock], rules.gullyRock * wet *
                    SmoothStep(.45f, .9f, slope) * (1.f - weights[Snow]));
                // Forest soil is .85 of the forest share; trees cover it.
                const float cover = std::min(1.f, weights[ForestSoil] / .85f);
                color = glm::mix(color, canopy, cover);
                const size_t sample = static_cast<size_t>((cy + extent) * perCell + row / step) *
                    side + static_cast<size_t>((cx + extent) * perCell + col / step);
                colors[sample] = glm::vec4(color * rules.brightness, cover);
                // Same slopes: the surface normal at this sample's 16 m scale.
                normals[sample] = glm::normalize(glm::vec3(-dx, 1.f, -dz));
            }
        return true;
    });
    if (!read) { side = 0; normals.clear(); return {}; }
    // Cliff pieces: loaded cells draw them as terrain stamps, far tiles only
    // through these maps. Far tiles light by the 16 m normals, which smooth a
    // wall into an upward slope facing the sun, so a dark rock face showed as
    // a pale slope until its cells loaded. Where a piece stands the maps take
    // its scan's colour and its wall's facing instead (no canopy there).
    const float spacing = step * cellWidth;
    const float origin = -float(extent) * cellsX * cellWidth;
    for (const auto& cliff : cliffs)
    {
        const float reach = cliff.radius + .5f * spacing;
        const int c0 = std::max(0, static_cast<int>(std::floor((cliff.position.x - reach - origin) / spacing)));
        const int c1 = std::min(side - 1, static_cast<int>(std::ceil((cliff.position.x + reach - origin) / spacing)));
        const int r0 = std::max(0, static_cast<int>(std::floor((cliff.position.y - reach - origin) / spacing)));
        const int r1 = std::min(side - 1, static_cast<int>(std::ceil((cliff.position.y + reach - origin) / spacing)));
        for (int row = r0; row <= r1; ++row)
            for (int col = c0; col <= c1; ++col)
            {
                const float distance = glm::length(glm::vec2(origin + col * spacing,
                    origin + row * spacing) - cliff.position);
                const float cover = 1.f - SmoothStep(.5f * cliff.radius, reach, distance);
                if (cover <= 0.f) continue;
                const size_t sample = static_cast<size_t>(row) * side + col;
                auto& color = colors[sample];
                color = glm::vec4(glm::mix(glm::vec3(color), cliff.color * rules.brightness, cover),
                    color.a * (1.f - cover));
                normals[sample] = glm::normalize(glm::mix(normals[sample], cliff.facing, cover));
            }
    }
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

// Share of conifers among the trees at a height: oak lowlands, mixed
// foothills, conifers over the upper slopes (350-710 m on the demo map,
// whose tree line is 950 m with an altitude blend of 6).
float ConiferShare(float height, const Altitudes& a)
{
    return SmoothStep(a.tree - 100.f * a.blend, a.tree - 40.f * a.blend, height);
}

// Generated cliff blocks (Tools/generate_boulders.py): up to two per cell
// on steep rock below the snow, upright and sunk into the slope. They live in
// the forest layer for its 900 m reach, and are authored to cull after their
// last level, so far blocks leave them out (thousands of steep cells would
// otherwise add millions of far triangles). Their own random stream keeps
// the tree layout unchanged.
// Prefab (under Assets/Prefabs/Woodland/), half width along the contour and
// half depth into the slope at scale 1 (m), and the scale that brings the
// piece to crag size. The first five are photo-scanned rock faces (Poly
// Haven, CC0; tools/fetch_polyhaven_props.py) a few metres across, scaled up;
// the rest are the generated shapes, kept as filler.
struct CliffShape { const char* name; float halfWidth, halfDepth, baseScale; };
const std::array<CliffShape, 8> Cliffs{{
    {"Scans/Cliff_Scan_02", 10.f, 3.3f, 2.2f}, {"Scans/Mountainside_01", 5.1f, 2.6f, 3.6f},
    {"Scans/Cliff_Scan_01", 4.1f, 2.2f, 4.f}, {"Scans/RockFace_01", 3.6f, 2.8f, 4.f},
    {"Scans/RockFace_02", 2.4f, 1.7f, 4.5f},
    {"Boulders/Crag_spire_01", 6.5f, 5.5f, 1.f}, {"Boulders/Cliff_overhang_01", 4.5f, 3.f, 1.3f},
    {"Boulders/Outcrop_ledge_01", 5.5f, 3.4f, 1.3f}}};

// Cliffs placed when the map is built (tools/landscape_cliffs.py, world.cliffs
// next to the height archive): the builder stamps each piece into the heights
// (the ground meets the scan instead of falling away under it), so for the
// cells it covers the pieces must come from its list, not from the rules
// below. Cells outside its extent keep runtime placement.
struct CliffList
{
    struct Piece
    {
        uint8_t shape = 0;
        glm::vec3 position{0.f}, normal{0.f, 1.f, 0.f};
        float yawDegrees = 0.f, scale = 1.f;
    };
    int extent = -1;   // cells with |x| and |z| <= extent are covered
    uint64_t contentHash = 0;
    std::map<TerrainManager::Cell, std::vector<Piece>> cells;
};

// Null when the file is missing, malformed or from another archive.
std::shared_ptr<const CliffList> LoadCliffList(const std::string& path, uint64_t archiveSignature)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) return nullptr;
    const std::vector<char> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    constexpr size_t HeaderSize = 8 + 4 + 8 + 4 + 4, RecordSize = 40;
    if (bytes.size() < HeaderSize) return nullptr;
    const auto read = [&](size_t offset, auto& value) { std::memcpy(&value, bytes.data() + offset, sizeof(value)); };
    uint64_t magic = 0, signature = 0;
    uint32_t version = 0, count = 0;
    int32_t extent = -1;
    read(0, magic); read(8, version); read(12, signature); read(20, extent); read(24, count);
    constexpr uint64_t Magic = 0x31464c43524e5254ull; // "TRNRCLF1"
    if (magic != Magic || version != 1 || signature != archiveSignature || extent < 0 ||
        bytes.size() != HeaderSize + static_cast<size_t>(count) * RecordSize) return nullptr;
    auto list = std::make_shared<CliffList>();
    list->extent = extent;
    uint64_t hash = 1469598103934665603ull;
    for (char byte : bytes) hash = (hash ^ static_cast<uint8_t>(byte)) * 1099511628211ull;
    list->contentHash = hash;
    for (uint32_t i = 0; i < count; ++i)
    {
        const size_t at = HeaderSize + static_cast<size_t>(i) * RecordSize;
        int16_t cellX = 0, cellZ = 0;
        float values[8] = {};
        CliffList::Piece piece;
        read(at, cellX); read(at + 2, cellZ); read(at + 4, piece.shape);
        std::memcpy(values, bytes.data() + at + 8, sizeof(values));
        piece.position = {values[0], values[1], values[2]};
        piece.normal = {values[3], values[4], values[5]};
        piece.yawDegrees = values[6];
        piece.scale = values[7];
        list->cells[{cellX, cellZ}].push_back(piece);
    }
    return list;
}

// What draws a cliff piece as an object (only cells outside the stamp list,
// whose pieces are placed at runtime): the stamp's proxy
// (tools/bake_terrain_stamp.py write_proxy), the same shape and surface a
// stamp has. The photo-scanned meshes are only the stamps' bake sources.
// Shapes past the five stamps keep their own prefab.
// A/B switches for the cliff work (2026-11-22 bisect of a walk slowdown):
// ENGINE_TERRAIN_STAMPS=0 leaves cells without stamps or sculpts (and so
// without cliffs), ENGINE_FAR_PATCHES=0 drops the far grid's full-resolution
// cliff-cell patches.
bool EnvironmentSwitch(const char* name)
{
    const char* value = std::getenv(name);
    return !(value && value[0] == '0');
}

// The stamp a scanned shape (the first five) was baked into: its file name
// under Assets/TerrainData/Stamps (and Prefabs/Woodland/Stamps for its proxy).
std::string StampName(size_t shape)
{
    const std::string name = Cliffs[shape].name;
    const auto slash = name.find('/');
    return name.substr(slash == std::string::npos ? 0 : slash + 1);
}

std::string CliffPrefab(size_t shape)
{
    if (shape >= 5) return std::string("Assets/Prefabs/Woodland/") + Cliffs[shape].name + ".prefab";
    return "Assets/Prefabs/Woodland/Stamps/" + StampName(shape) + ".prefab";
}

// The terrain stamps of a cell's cliff pieces (tools/bake_terrain_stamp.py
// bakes one per scan into Assets/TerrainData/Stamps), in the cell's space.
// Sculpted cells (tools/cliff_sculpt.py, Assets/TerrainData/Sculpts): the
// sculpt and its material's index in the terrain's stamp materials.
using SculptedCells = std::map<TerrainManager::Cell,
    std::pair<std::shared_ptr<const Engine::Components::TerrainSculpt>, int>>;

SculptedCells LoadSculptedCells(std::vector<std::string>& stampMaterials)
{
    SculptedCells cells;
    // ENGINE_SCULPTS=0 keeps the stamps in sculpted cells (A/B runs).
    if (const char* setting = std::getenv("ENGINE_SCULPTS"); setting && setting[0] == '0')
        return cells;
    std::error_code error;
    const std::filesystem::path directory =
        Engine::Components::Mesh::ResolveFilePath("Assets/TerrainData/Sculpts");
    for (const auto& entry : std::filesystem::directory_iterator(directory, error))
    {
        if (entry.path().extension() != ".sculpt") continue;
        auto sculpt = Engine::Components::TerrainSculpt::Load(entry.path().string());
        if (!sculpt || sculpt->material.empty()) continue;
        // Cells share their rock material (one layer-array slice).
        auto found = std::find(stampMaterials.begin(), stampMaterials.end(), sculpt->material);
        if (found == stampMaterials.end())
            found = stampMaterials.insert(stampMaterials.end(), sculpt->material);
        cells[{sculpt->cellX, sculpt->cellZ}] =
            {sculpt, static_cast<int>(found - stampMaterials.begin())};
    }
    return cells;
}

std::vector<Engine::Components::TerrainStampInstance> StampsForCell(const CliffList& cliffs,
    const SculptedCells& sculpted, TerrainManager::Cell cell)
{
    std::vector<Engine::Components::TerrainStampInstance> stamps;
    // A sculpted cell's surface already has its pieces' shape.
    if (const auto found = sculpted.find(cell); found != sculpted.end())
    {
        Engine::Components::TerrainStampInstance instance;
        instance.sculpt = found->second.first;
        instance.materialIndex = found->second.second;
        stamps.push_back(std::move(instance));
        return stamps;
    }
    if (std::abs(cell.first) > cliffs.extent || std::abs(cell.second) > cliffs.extent) return stamps;
    const auto found = cliffs.cells.find(cell);
    if (found == cliffs.cells.end()) return stamps;
    for (const auto& piece : found->second)
    {
        if (piece.shape >= 5) continue;
        auto stamp = Engine::Components::TerrainStamp::Load(Engine::Components::Mesh::ResolveFilePath(
            "Assets/TerrainData/Stamps/" + StampName(piece.shape) + ".stamp"));
        if (!stamp) continue;
        Engine::Components::TerrainStampInstance instance;
        instance.stamp = std::move(stamp);
        instance.materialIndex = piece.shape;
        instance.transform = glm::translate(glm::mat4(1.f), piece.position) *
            glm::mat4_cast(TerrainObjectLayer::SurfaceRotation(piece.normal, piece.yawDegrees)) *
            glm::scale(glm::mat4(1.f), glm::vec3(piece.scale));
        stamps.push_back(std::move(instance));
    }
    return stamps;
}

void GenerateCliffs(const TerrainManager::ObjectLayerContext& context,
    TerrainObjectLayer& layer, const Altitudes& a, uint64_t& id, const CliffList* cliffs)
{
    // Far cells get cliffs too: the far blocks draw their last level out to
    // the cull distance below (TerrainManager far objects).
    for (size_t shape = 0; shape < Cliffs.size(); ++shape)
    {
        auto& lod = layer.objectLods[CliffPrefab(shape)];
        // Crags hold their detail further out than the small blocks.
        lod.distances = {140.f, 420.f};
        lod.cullAfterLastLod = true;
        lod.finalLodCullDistance = layer.viewDistance;
    }
    // Cells the build placed cliffs in draw them as terrain stamps
    // (StampsForCell) at every terrain detail level, the far cells' stride-8
    // meshes included, so no object stands in for them: far-block proxies
    // there only fought the stamps' own surface for depth. Past the far cells
    // (farRadius, 4 km) a rock face is a few pixels, carried by the macro
    // colour and normal maps (ComputeMacroColors).
    if (cliffs && std::abs(context.cell.first) <= cliffs->extent &&
        std::abs(context.cell.second) <= cliffs->extent)
        return;
    const size_t layerCount = context.terrainLayerCount;
    const bool painted = context.weights && layerCount >= LayerCount &&
        context.weights->size() == context.heights->size() * layerCount;
    const float width = context.cellsX * context.cellWidth;
    const float depth = context.cellsY * context.cellDepth;
    uint32_t state = Hash(context.cell.first, context.cell.second, 0x636c6966u) | 1u;
    const auto random = [&state]
    {
        state ^= state << 13; state ^= state >> 17; state ^= state << 5;
        return (state & 0xffffffu) / float(0x1000000);
    };
    // A cliff is a band, not a lone block: each seed found on steep rock
    // grows along the contour (across the slope) while the ground stays
    // steep, so blocks line up into faces and ledges. Every block faces
    // downhill, which is what lets the overhang shape lean out over the drop.
    const auto steepRock = [&](float x, float z, float& height, glm::vec3& normal)
    {
        if (x < 2.f || z < 2.f || x > width - 2.f || z > depth - 2.f) return false;
        if (!Engine::Components::SampleTerrainSurface(*context.heights, context.cellsX,
                context.cellsY, context.cellWidth, context.cellDepth, x, z, height, &normal))
            return false;
        // Steep (from about 38 degrees), above the shores, below the snow.
        // Sheer faces count too: they take little room on the map but are
        // most of what is seen of a cliff, and were left as bare painted
        // terrain when this stopped at 72 degrees.
        if (normal.y > .79f || normal.y < .08f) return false;
        if (height < a.water + 25.f || height > a.snow) return false;
        // The slope must keep rising behind the piece: scanned rock faces are
        // open shells, and one standing at a crest shows its hollow back
        // against the sky as a thin sheet.
        {
            const glm::vec2 slope(normal.x, normal.z);
            const glm::vec2 uphill = -slope / std::max(glm::length(slope), 1.e-4f);
            for (float reach : {14.f, 28.f})
            {
                float behind = 0.f;
                if (!Engine::Components::SampleTerrainSurface(*context.heights, context.cellsX,
                        context.cellsY, context.cellWidth, context.cellDepth,
                        x + uphill.x * reach, z + uphill.y * reach, behind, nullptr) ||
                    behind < height + reach * .55f)
                    return false;
            }
        }
        if (painted)
        {
            const int col = std::clamp(static_cast<int>(std::lround(x / context.cellWidth)), 0, context.cellsX);
            const int row = std::clamp(static_cast<int>(std::lround(z / context.cellDepth)), 0, context.cellsY);
            if ((*context.weights)[(static_cast<size_t>(row) * (context.cellsX + 1) + col) *
                    layerCount + Rock] < .35f) return false;
        }
        return true;
    };
    // Returns the placed shape's half width (for the stride to the next one).
    const auto placeBlock = [&](float x, float z, float height, const glm::vec3& normal,
        float size, float pick, float turn)
    {
        // Mostly crags (rock-face pieces 25-40 m across) so neighbours overlap
        // into a broken wall; spires, blocks, overhangs and ledges break it up.
        // Scanned faces only. The generated spire, overhang and ledge were
        // one piece in eight as filler, but the spire is 25 m tall and stood
        // through the canopy as rows of dark columns along the escarpment
        // (captured 2026-11-20); they stay in the table, unused.
        const size_t shape = pick < .3f ? 0u : pick < .55f ? 1u : pick < .72f ? 2u :
            pick < .88f ? 3u : 4u;
        const float scale = Cliffs[shape].baseScale * (.75f + .6f * size);
        // Sink the piece so its back is buried and its downhill edge meets
        // the slope: its half depth times the slope's gradient.
        const float gradient = std::sqrt(std::max(0.f, 1.f - normal.y * normal.y)) / normal.y;
        TerrainObjectLayer::Placement placement;
        placement.id = ++id;
        placement.asset = CliffPrefab(shape);
        placement.position = {x, height - std::min(16.f,
            Cliffs[shape].halfDepth * scale * std::min(gradient, 1.6f) * .8f), z};
        // Lean a third of the way with the slope, so faces stand proud of it
        // rather than upright like walls.
        placement.normal = glm::normalize(glm::mix(glm::vec3(0.f, 1.f, 0.f), normal, .35f));
        // The meshes face +z; turn that downhill, with some variation.
        placement.yawDegrees = glm::degrees(std::atan2(normal.x, normal.z)) + (turn - .5f) * 50.f;
        placement.scale = scale;
        layer.placements.push_back(std::move(placement));
        return Cliffs[shape].halfWidth * scale;
    };
    int placed = 0;
    constexpr int MaximumBlocks = 64;
    for (int attempt = 0; attempt < 220 && placed < MaximumBlocks; ++attempt)
    {
        const float x = (.04f + .92f * random()) * width;
        const float z = (.04f + .92f * random()) * depth;
        const float yaw = random(), size = random(), pick = random();
        float height = 0.f;
        glm::vec3 normal(0.f, 1.f, 0.f);
        if (!steepRock(x, z, height, normal)) continue;
        const float seedHalfWidth = placeBlock(x, z, height, normal, size, pick, yaw);
        ++placed;
        // Along the contour, both ways, up to three more pieces each,
        // stepping by the pieces' widths so they overlap by about a third.
        const glm::vec2 slope(normal.x, normal.z);
        const glm::vec2 contour = glm::normalize(glm::vec2(-slope.y, slope.x));
        for (float direction : {1.f, -1.f})
        {
            float bx = x, bz = z, previousHalfWidth = seedHalfWidth;
            for (int step = 0; step < 3 && placed < MaximumBlocks; ++step)
            {
                const float nextSize = random(), nextPick = random(), nextTurn = random(),
                    drift = random();
                const float stride = previousHalfWidth * 1.25f + 3.f;
                bx += contour.x * stride * direction + slope.x * (drift - .5f) * 8.f;
                bz += contour.y * stride * direction + slope.y * (drift - .5f) * 8.f;
                float blockHeight = 0.f;
                glm::vec3 blockNormal(0.f, 1.f, 0.f);
                if (!steepRock(bx, bz, blockHeight, blockNormal)) break;
                previousHalfWidth = placeBlock(bx, bz, blockHeight, blockNormal, nextSize,
                    nextPick, nextTurn);
                ++placed;
            }
        }
    }
}

// ENGINE_LANDSCAPE_SHADOW_SCALE=<factor> scales the shadow ranges of the
// forest, prop and undergrowth layers (for A/B runs; 1 = as authored).
float LandscapeShadowScale()
{
    static const float scale = []
    { const char* v = std::getenv("ENGINE_LANDSCAPE_SHADOW_SCALE"); return v ? std::clamp(float(std::atof(v)), .1f, 2.f) : 1.f; }();
    return scale;
}

// Forest for one cell: random candidates kept where the forest mask, the
// height band and the slope allow, plus a few lone trees in the meadows.
void GenerateForest(const TerrainManager::ObjectLayerContext& context,
    TerrainObjectLayer& layer, const Altitudes& a, int treesPerCell, const CliffList* cliffs)
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
    layer.shadowDistance = 90.f * LandscapeShadowScale();
    layer.shadowLod = 1;
    // Beyond 90 m the impostors cast into the far shadow cascade, so forests
    // keep their shadows on the ground and on each other at any distance.
    layer.farShadowDistance = layer.viewDistance;
    const std::string prefix = "Assets/Prefabs/Woodland/Oaks/";
    const std::string coniferPrefix = "Assets/Prefabs/Woodland/Conifers/";
    for (const char* species : Species)
    {
        auto& lod = layer.objectLods[prefix + species + ".prefab"];
        lod.distances = {60.f, 220.f};
    }
    for (const char* species : ConiferSpecies)
        layer.objectLods[coniferPrefix + species + ".prefab"].distances = {60.f, 220.f};
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
        if (normal.y < .86f) continue; // steeper than about 30 degrees
        const float worldX = context.cell.first * width + x;
        const float worldZ = context.cell.second * depth + z;
        // Lone meadow trees keep a 3% chance outside the forest mask.
        const float interior = ForestMask(worldX, worldZ, height, a);
        const float density = std::max(interior, .03f) * ForestBand(height, a);
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
        placement.position = {x, height - .08f, z};
        placement.yawDegrees = yaw * 360.f;
        placement.scale = .75f + size * .55f;
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
            if (normal.y < .86f) continue;
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
                // Edge trees lean out toward the light, up to ~9 degrees.
                lean = glm::radians(9.f * size * size);
            }
            placement.position = {x, height - sink, z};
            placement.yawDegrees = yaw * 360.f;
            const float leanDirection = pick * glm::two_pi<float>();
            placement.normal = {std::sin(lean) * std::cos(leanDirection), std::cos(lean),
                std::sin(lean) * std::sin(leanDirection)};
            layer.placements.push_back(std::move(placement));
        }
    }
    GenerateCliffs(context, layer, a, id, cliffs);
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
        layer.shadowDistance = 80.f * LandscapeShadowScale();
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

    uint64_t paintKey = 0;
    for (float value : {level.water, level.rock, level.snow, level.tree, level.blend})
        paintKey = paintKey * 0x100000001b3ull ^
            static_cast<uint64_t>(std::llround(value * 16.f));
    // Built-time cliffs (world.cliffs beside the archive); generated layers
    // are cached, so the list is part of the cache key.
    const std::shared_ptr<const CliffList> cliffs = LoadCliffList(
        std::filesystem::path(archivePath).replace_extension(".cliffs").string(), signature);
    manager->SetStaticCache(cacheDirectory,
        signature ^ PaintRevision ^ paintKey ^ (cliffs ? cliffs->contentHash : 0ull));
    // Cliff pieces in loaded cells are part of the terrain (stamps); the
    // stamp files are read once and shared.
    static const bool stampsEnabled = EnvironmentSwitch("ENGINE_TERRAIN_STAMPS");
    if (cliffs && stampsEnabled)
    {
        // Their surface images (tools/stamp_textures.py), in Cliffs order.
        std::vector<std::string> stampMaterials;
        for (size_t shape = 0; shape < 5; ++shape)
            stampMaterials.push_back("Assets/TerrainData/Stamps/" + StampName(shape) + ".material");
        // Then each sculpted cell's own rock images.
        auto sculpted = std::make_shared<const SculptedCells>(LoadSculptedCells(stampMaterials));
        manager->SetTerrainStampMaterials(std::move(stampMaterials));
        manager->SetTerrainStamps([cliffs, sculpted](TerrainManager::Cell cell) {
            return StampsForCell(*cliffs, *sculpted, cell);
        });
    }
    else
    {
        manager->SetTerrainStampMaterials({});
        manager->SetTerrainStamps({});
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
        const float originX = cell.first * countX * terrain.cellWidth;
        const float originZ = cell.second * countY * terrain.cellDepth;
        terrain.weights.assign(terrain.heights.size() * LayerCount, 0.f);
        for (int row = 0; row <= countY; ++row)
            for (int col = 0; col <= countX; ++col)
            {
                const size_t i = static_cast<size_t>(row) * (countX + 1) + col;
                PaintSample(terrain.heights[i], codes[i], originX + col * terrain.cellWidth,
                    originZ + row * terrain.cellDepth, level, &terrain.weights[i * LayerCount]);
            }
        terrain.Rebuild();
    });
    manager->SetObjectLayerGenerator([=](const TerrainManager::ObjectLayerContext& context,
        const std::string& layerName, TerrainObjectLayer& layer)
    {
        if (layerName == forestName) GenerateForest(context, layer, level, trees, cliffs.get());
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
            if (grass->innerRadius != detailed * .5f ||
                grass->proceduralLayerMask != (1 << Grass))
            {
                grass->innerRadius = detailed * .5f;
                grass->innerFadeWidth = detailed * .2f;
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
    mix(std::hash<std::string>{}(flowMap));
    mix(std::hash<std::string>{}(depositionMap));
    mix(static_cast<uint64_t>(mapHalfExtentCells));
    // Cliff pieces paint their stamps' colour and facing (ComputeMacroColors).
    mix(0x434c4946464d4b31ull);
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
        MacroColorRules rules;
        rules.lushness = std::clamp(macroLushness, 0.f, 1.f);
        rules.flowThreshold = std::clamp(macroFlowThreshold, 0.f, .99f);
        rules.scree = std::clamp(macroScree, 0.f, 1.f);
        rules.screeDepth = std::clamp(macroScreeDepth, .1f, 100.f);
        rules.gullyRock = std::clamp(macroGullyRock, 0.f, 1.f);
        rules.brightness = std::clamp(macroBrightness, .1f, 2.f);
        // The stamps' colours, likewise from their material files.
        std::array<glm::vec3, 5> stampColors;
        for (size_t shape = 0; shape < stampColors.size(); ++shape)
            stampColors[shape] = LayerMacroColor(
                ("Assets/TerrainData/Stamps/" + StampName(shape) + ".material").c_str());
        return [=]
        {
            // Missing or mismatched erosion maps leave just the paint.
            TerrainHydrologyMap flowSamples, depositionSamples;
            flowSamples.Load(flowPath, TerrainHydrologyMap::FlowMagic, signature);
            depositionSamples.Load(depositionPath, TerrainHydrologyMap::DepositionMagic, signature);
            // Built-time cliff pieces (world.cliffs), in world space.
            std::vector<CliffMark> marks;
            if (const auto cliffs = LoadCliffList(
                    std::filesystem::path(archive).replace_extension(".cliffs").string(), signature))
                for (const auto& [cell, pieces] : cliffs->cells)
                    for (const auto& piece : pieces)
                    {
                        if (piece.shape >= stampColors.size()) continue;
                        const auto stamp = Engine::Components::TerrainStamp::Load(
                            Engine::Components::Mesh::ResolveFilePath(
                                "Assets/TerrainData/Stamps/" + StampName(piece.shape) + ".stamp"));
                        if (!stamp) continue;
                        CliffMark mark;
                        mark.position = glm::vec2(cell.first * cellsX * cellWidth + piece.position.x,
                            cell.second * cellsY * cellDepth + piece.position.z);
                        mark.radius = Cliffs[piece.shape].halfWidth * piece.scale;
                        mark.color = stampColors[piece.shape];
                        mark.facing = glm::normalize(TerrainObjectLayer::SurfaceRotation(
                            piece.normal, piece.yawDegrees) * stamp->direction);
                        marks.push_back(mark);
                    }
            int side = 0;
            std::vector<glm::vec3> normals;
            const auto colors = ComputeMacroColors(archive, signature, extent, cellsX, cellsY,
                cellWidth, cellDepth, level, means, flowSamples, depositionSamples, rules, marks,
                side, normals);
            const float spacing = cellWidth * 4.f;
            const float origin = -float(extent) * cellsX * cellWidth;
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

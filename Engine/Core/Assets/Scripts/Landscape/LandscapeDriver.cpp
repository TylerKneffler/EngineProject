#include "Core/Rendering/DetailBudget.h"
#include "Scripts/Landscape/LandscapeDriver.h"
#include "Grass/GrassField.h"
#include "Grass/GrassBlades.h"
#include "Scripts/Grass/MeadowGrass.h"
#include "Core/Components/Terrain/TerrainHeightArchive.h"
#include "Core/Components/Terrain/TerrainManager.h"
#include "Core/Components/Terrain/TerrainObjectLayer.h"
#include "Core/Components/Terrain/TerrainObjectArchive.h"
#include "Core/Components/Terrain/TerrainPaintArchive.h"
#include "Core/Components/Terrain/TerrainSurfaceSample.h"
#include "Core/Components/Mesh/Mesh.h"
#include "Core/Components/Materials/Material.h"
#include "Core/Components/Camera/Camera.h"
#include "Core/Scene/Object.h"
#include "Core/Scene/Scene.h"
#include "Core/Serialization/Json.h"
#include "Core/Serialization/SceneSerializer.h"
#include <cstdlib>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <functional>
#include <mutex>
#include <set>
#include <tuple>
#include <future>
#include <thread>
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
using Engine::Components::TerrainPaintArchive;

// A cell's baked paint (TerrainPaintArchive): its layer weights and lushness
// per grid vertex. A cell the archive lacks (or no archive) is plain grass.
void ReadBakedPaint(const TerrainPaintArchive* paint, std::pair<int, int> cell, size_t vertices,
    std::vector<float>& weights, std::vector<float>& lushness)
{
    if (paint && paint->GetHeader().layers == LayerCount && paint->ReadCell(cell, weights, lushness) &&
        weights.size() == vertices * LayerCount && lushness.size() == vertices)
        return;
    weights.assign(vertices * LayerCount, 0.f);
    for (size_t v = 0; v < vertices; ++v) weights[v * LayerCount + Grass] = 1.f;
    lushness.assign(vertices, 0.f);
}
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
constexpr uint64_t PaintRevision = 0x4c414e445343503eull; // ...3d: before paint baked (2026-11-25)

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
    int extent, int cellsX, int cellsY, const std::array<glm::vec3, LayerCount>& means,
    const TerrainPaintArchive* paint)
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
        (void)codes;
        std::vector<float> painted, lushness;
        ReadBakedPaint(paint, {cx, cy}, heights.size(), painted, lushness);
        // Rows and columns shared with the next cell are visited there,
        // except along the map's far edges.
        const int lastRow = cy == extent ? cellsY : cellsY - 1;
        const int lastCol = cx == extent ? cellsX : cellsX - 1;
        for (int row = 0; row <= lastRow; row += step)
            for (int col = 0; col <= lastCol; col += step)
            {
                const size_t index = static_cast<size_t>(row) * (cellsX + 1) + col;
                const float* weights = &painted[index * LayerCount];
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

// Macro color settings (LandscapeDriver "Macro color" properties).
struct MacroColorRules
{
    float brightness = .85f;
};

// Darker, greener ground on lush valley floors (Object.hlsl applies the same
// tint to near cells from Terrain::lushness).
const glm::vec3 LushTint(.72f, .92f, .55f);

// One macro sample's ground color (linear, no canopy): the paint there
// (layer weights, and lushness: the share of LushTint), dx and dz the rise
// over run across the 16 m samples.
glm::vec3 MacroGroundColor(const float* weights, float lushness, float dx, float dz,
    const std::array<glm::vec3, LayerCount>& means)
{
    const float slope = std::sqrt(dx * dx + dz * dz);
    glm::vec3 color(0.f);
    for (int layer = 0; layer < LayerCount; ++layer)
        color += means[layer] * weights[layer];
    color *= glm::mix(glm::vec3(1.f), LushTint, std::clamp(lushness, 0.f, 1.f));
    // Steep faces: the scanned cliff rock near volume cells show there
    // (VolumeTerrain's steep band, 1 - normal.y .25 .. .45), not the paint's
    // pale rock layer: distant cliffs and the escarpment read as bright tan
    // faces (2026-11-24).
    static const glm::vec3 cliff = LayerMacroColor("Assets/TerrainData/Sculpts/cliff_rock.material");
    const float normalY = 1.f / std::sqrt(1.f + slope * slope);
    float t = std::clamp((1.f - normalY - .25f) / .2f, 0.f, 1.f);
    t = t * t * (3.f - 2.f * t);
    return glm::mix(color, cliff, t);
}

// The macro color map: the baked paint every fourth sample (16 m on the demo
// map) with its lushness; forest canopy covers it (alpha = canopy cover).
// Rows along +Z from the map's -X/-Z corner.
std::vector<glm::vec4> ComputeMacroColors(const std::string& archivePath, uint64_t signature,
    int extent, int cellsX, int cellsY, float cellWidth, float cellDepth,
    const std::array<glm::vec3, LayerCount>& means, const TerrainPaintArchive* paint,
    const MacroColorRules& rules, int& side, std::vector<glm::vec3>& normals,
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
        std::vector<float> painted, lushness;
        ReadBakedPaint(paint, {cx, cy}, heights.size(), painted, lushness);
        const auto height = [&](int row, int col) {
            row = std::clamp(row, 0, cellsY); col = std::clamp(col, 0, cellsX);
            return heights[static_cast<size_t>(row) * (cellsX + 1) + col];
        };
        for (int row = 0; row <= cellsY; row += step)
            for (int col = 0; col <= cellsX; col += step)
            {
                const size_t index = static_cast<size_t>(row) * (cellsX + 1) + col;
                // Rise over run across one sample spacing each way.
                const float dx = (height(row, col + step) - height(row, col - step)) /
                    (2.f * step * cellWidth);
                const float dz = (height(row + step, col) - height(row - step, col)) /
                    (2.f * step * cellDepth);
                const glm::vec3 color = MacroGroundColor(&painted[index * LayerCount], lushness[index],
                    dx, dz, means);
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

// A/B switch: ENGINE_FAR_PATCHES=0 drops the far grid's full-resolution
// cell patches.
bool EnvironmentSwitch(const char* name)
{
    const char* value = std::getenv(name);
    return !(value && value[0] == '0');
}

// A/B override: the variable's number, else the fallback.
float EnvironmentNumber(const char* name, float fallback)
{
    const char* value = std::getenv(name);
    return value && value[0] ? static_cast<float>(std::atof(value)) : fallback;
}

// A layer's shadow range: <name> from the environment when set (A/B runs of
// longer ranges), else the authored one, times ENGINE_LANDSCAPE_SHADOW_SCALE
// (1 = as authored).
float LandscapeShadowRange(const char* name, float authored)
{
    static const float scale = []
    { const char* v = std::getenv("ENGINE_LANDSCAPE_SHADOW_SCALE"); return v ? std::clamp(float(std::atof(v)), .1f, 2.f) : 1.f; }();
    const char* v = std::getenv(name);
    const float range = v ? std::clamp(float(std::atof(v)), 0.f, 900.f) : authored;
    return range * scale;
}


}

Altitudes AltitudesOf(const LandscapeDriver& driver)
{
    return {driver.waterLevel, driver.rockLine, driver.snowLine, driver.treeLine,
        std::clamp(driver.altitudeBlend, .1f, 50.f)};
}

// The macro map's settings ("Macro color" properties).
MacroColorRules MacroRulesOf(const LandscapeDriver& driver)
{
    MacroColorRules rules;
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

// Covered crown area (m^2) a tree adds to the samples of a grid (spacing
// apart from origin, side x side) around it: its crown's area spread over
// the samples within its reach, weighted by closeness. visit(i, j, area) for
// each sample it covers; the whole map's canopy and cell patches share it.
template<class Visit>
void SplatCrown(const std::string& asset, float scale, float wx, float wz, float origin,
    float spacing, int side, Visit&& visit)
{
    const auto* crown = FindTreeCrown(asset);
    const float radius = (crown ? crown->radius : 3.f) * scale + .5f * spacing;
    const float area = (crown ? crown->area : 20.f) * scale * scale;
    const int i0 = std::max(0, int(std::floor((wx - radius - origin) / spacing)));
    const int i1 = std::min(side - 1, int(std::ceil((wx + radius - origin) / spacing)));
    const int j0 = std::max(0, int(std::floor((wz - radius - origin) / spacing)));
    const int j1 = std::min(side - 1, int(std::ceil((wz + radius - origin) / spacing)));
    float total = 0.f;
    for (int j = j0; j <= j1; ++j)
        for (int i = i0; i <= i1; ++i)
            total += std::max(0.f, 1.f - std::hypot(origin + i * spacing - wx,
                origin + j * spacing - wz) / radius);
    if (total <= 0.f) return;
    for (int j = j0; j <= j1; ++j)
        for (int i = i0; i <= i1; ++i)
        {
            const float w = std::max(0.f, 1.f - std::hypot(origin + i * spacing - wx,
                origin + j * spacing - wz) / radius);
            if (w > 0.f) visit(i, j, area * w / total);
        }
}

// A worn track: trails sink a little into the ground (collision and grass
// follow the cell heights). Only where the shaded path runs: off rock, snow
// and shores and inside the forest band. (Runs at load until trails are
// scene splines: .md/todo.md, "bake everything", stage 3.)
void CarveTrails(TerrainManager::Cell cell, int countX, int countY, float cellWidth, float cellDepth,
    const std::vector<float>& weights, const Altitudes& level, std::vector<float>& heights)
{
    const float originX = cell.first * countX * cellWidth;
    const float originZ = cell.second * countY * cellDepth;
    if (ShaderTrails() && weights.size() == heights.size() * LayerCount)
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
    RegisterField("paintArchive", paintArchive);
    RegisterField("farGrid", farGrid);
    RegisterField("cacheDirectory", cacheDirectory);
    RegisterField("enableFarTerrain", enableFarTerrain);
    RegisterField("waterLevel", waterLevel);
    RegisterField("rockLine", rockLine);
    RegisterField("snowLine", snowLine);
    RegisterField("treeLine", treeLine);
    RegisterField("altitudeBlend", altitudeBlend);
    RegisterField("forestLayerName", forestLayerName);
    RegisterField("grassRadius", grassRadius);
    RegisterField("enableWater", enableWater);
    RegisterField("waterMaterial", waterMaterial);
    RegisterField("enableMacroColors", enableMacroColors, "Macro color");
    RegisterField("macroBrightness", macroBrightness, "Macro color");
    RegisterFieldTooltip("enableMacroColors", "Compose the 16 m macro color map that distant terrain shows (needs TerrainManager > Macro color map). Rebuilds on a worker when these settings change.");
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
    // ENGINE_TERRAIN_LOADED_RADIUS=<cells> overrides the full-detail radius (A/B runs).
    static const int forcedLoaded = static_cast<int>(EnvironmentNumber("ENGINE_TERRAIN_LOADED_RADIUS", -1.f));
    if (forcedLoaded >= 0) manager->loadedRadius = std::clamp(forcedLoaded, 0, 32);
    const uint64_t signature = SourceSignature();
    if (!signature) return false;
    const std::string archivePath = Engine::Components::Mesh::ResolveFilePath(heightArchive);
    const int countX = manager->cellsX, countY = manager->cellsY;
    const int extent = mapHalfExtentCells;
    const Altitudes level = AltitudesOf(*this);
    const std::string forestName = forestLayerName;
    const float tiling = manager->cellsX * manager->cellWidth / 2.f; // 2 m repeats
    // The map's paint, baked: loaded as it is, never repainted by rules.
    // Without it the ground is plain grass.
    m_paint = paintArchive.empty() ? nullptr
        : TerrainPaintArchive::Open(Engine::Components::Mesh::ResolveFilePath(paintArchive));
    if (!m_paint && !paintArchive.empty())
        std::fprintf(stderr, "[LandscapeDriver] no baked paint at %s\n",
            paintArchive.c_str());
    const auto paint = m_paint;
    const uint64_t paintKey = paint ? paint->Signature() : 0ull;
    // Trails carve the heights when shaded per pixel. Rock shapes are part
    // of the heights themselves, so nothing else goes into the key.
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
        ReadBakedPaint(paint.get(), cell, terrain.heights.size(), terrain.weights, terrain.lushness);
        CarveTrails(cell, countX, countY, terrain.cellWidth, terrain.cellDepth, terrain.weights, level,
            terrain.heights);
        terrain.Rebuild();
    });
    // Trees, rocks and bushes are not generated here: they load from the
    // object archive (TerrainManager::objectArchive)
    // and the cells' own .objects files.
    manager->SetObjectLayerGenerator({});
    // The meadow preset attaches a GrassField to the procedural grass layer;
    // keep the grass on the meadow paint and give it this map's density.
    // ENGINE_LANDSCAPE_GRASS_RADIUS=<m> overrides grassRadius (A/B runs).
    const float radius = EnvironmentNumber("ENGINE_LANDSCAPE_GRASS_RADIUS", grassRadius);
    const std::string viewer = manager->viewerObjectName;
    manager->SetObjectLayerSetup([radius, forestName, viewer](TerrainManager::Cell, TerrainObjectLayer& layer)
    {
        MeadowGrass::Apply(layer, viewer);
        // Forests budget their detail in the project's "Trees" layer.
        if (layer.Owner && (layer.layerName == forestName || layer.layerName == forestName + " edge"))
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
    for (float value : {macroBrightness, waterLevel, rockLine, snowLine, treeLine, altitudeBlend})
        mix(static_cast<uint64_t>(std::llround(double(value) * 4096.0)));
    mix(m_paint ? m_paint->Signature() : 0);
    if (const auto* manager = Owner ? Owner->GetComponent<TerrainManager>() : nullptr)
        if (const auto objects = manager->GetObjectArchive()) mix(objects->Signature());
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
        const uint64_t signature = m_configuredSignature;
        const int extent = mapHalfExtentCells;
        const int cellsX = manager->cellsX, cellsY = manager->cellsY;
        const float cellWidth = manager->cellWidth, cellDepth = manager->cellDepth;
        const Altitudes level = AltitudesOf(*this);
        // Layer means come from material files: read them here, not on the job.
        const auto means = MakePalette(level).nearColors;
        const MacroColorRules rules = MacroRulesOf(*this);
        // The baked placements: every far-drawn tree's crown.
        const auto objects = manager->GetObjectArchive();
        const auto paint = m_paint;
        return [=]
        {
            int side = 0;
            std::vector<glm::vec3> normals;
            // Canopy cover from the baked trees: every far-drawn tree's crown
            // (TreeCrown, at its scale) splatted into the map's 16 m grid as
            // covered area.
            const float spacing = cellWidth * 4.f;
            const float origin = -float(extent) * cellsX * cellWidth;
            const int gridSide = (extent * 2 + 1) * (cellsX / 4) + 1;
            std::vector<float> covered(static_cast<size_t>(gridSide) * gridSide, 0.f);
            const auto splatTrees = [&](int cx, int cy, const std::vector<float>&, const std::vector<uint8_t>&)
            {
                std::vector<Engine::Components::TerrainObjectArchive::Layer> layers;
                if (!objects || !objects->ReadCell({cx, cy}, layers)) return;
                for (const auto& layer : layers)
                {
                    // Layers drawn far (not a forest edge's shrubs).
                    const auto* layerTemplate = objects->Template(layer.name);
                    if (layerTemplate && layerTemplate->Has("farObjects") && !(*layerTemplate)["farObjects"].AsBool())
                        continue;
                    for (const auto& placement : layer.placements)
                    {
                        if (!FindTreeCrown(placement.asset)) continue;
                        SplatCrown(placement.asset, placement.scale,
                            cx * cellsX * cellWidth + placement.position.x,
                            cy * cellsY * cellDepth + placement.position.z, origin, spacing, gridSide,
                            [&](int i, int j, float area)
                            { covered[static_cast<size_t>(j) * gridSide + i] += area; });
                    }
                }
            };
            auto colors = ComputeMacroColors(archive, signature, extent, cellsX, cellsY,
                cellWidth, cellDepth, means, paint.get(), rules, side, normals, splatTrees);
            if (side == gridSide)
                for (size_t i = 0; i < colors.size(); ++i)
                    colors[i].a = 1.f - std::exp(-covered[i] / (spacing * spacing));
            return TerrainManager::EncodeMacroColorImage(side, side, origin, origin,
                spacing, colors, &normals);
        };
    };
    source.makeCellUpdate = [this](const std::vector<TerrainManager::Cell>& cells,
        const TerrainManager::MacroColorImage& map)
        -> std::function<std::vector<TerrainManager::MacroColorPatch>()>
    {
        auto* manager = Owner ? Owner->GetComponent<TerrainManager>() : nullptr;
        if (!manager || !m_configuredSignature || cells.empty() || map.width <= 0) return {};
        return MakeMacroCellUpdate(*manager, cells, map);
    };
    manager.SetMacroColorSource(std::move(source));
}

std::function<std::vector<TerrainManager::MacroColorPatch>()> LandscapeDriver::MakeMacroCellUpdate(
    TerrainManager& manager, const std::vector<TerrainManager::Cell>& cells,
    const TerrainManager::MacroColorImage& map)
{
    using Cell = TerrainManager::Cell;
    const int cellsX = manager.cellsX, cellsY = manager.cellsY;
    const float cellWidth = manager.cellWidth, cellDepth = manager.cellDepth;
    const int extent = mapHalfExtentCells;
    if (cellsX != cellsY || cellWidth != cellDepth || cellsX % 4 != 0) return {};
    // A loaded cell as it is now, copied here (scene thread): its paint
    // (baked, then hand paint) and its trees.
    struct Tree { std::string asset; float scale; float x, z; };
    struct LoadedCell
    {
        std::vector<float> weights, lushness;
        std::vector<Tree> trees;
    };
    auto loaded = std::make_shared<std::map<Cell, LoadedCell>>();
    std::set<Cell> wanted;
    for (const Cell cell : cells)
        for (int dz = -1; dz <= 1; ++dz)
            for (int dx = -1; dx <= 1; ++dx) wanted.insert({cell.first + dx, cell.second + dz});
    for (const Cell cell : wanted)
    {
        const Terrain* terrain = manager.GetLoadedCell(cell.first, cell.second);
        if (!terrain || terrain->heights.size() != static_cast<size_t>(cellsX + 1) * (cellsY + 1) ||
            terrain->weights.size() != terrain->heights.size() * LayerCount) continue;
        LoadedCell copy;
        copy.weights = terrain->weights;
        if (terrain->lushness.size() == terrain->heights.size()) copy.lushness = terrain->lushness;
        if (terrain->Owner)
            for (auto* child : terrain->Owner->Children)
            {
                // Every tree the cell has, painted ones included: any
                // placement whose asset has a known crown.
                auto* layer = child ? child->GetComponent<TerrainObjectLayer>() : nullptr;
                if (!layer || !layer->farObjects) continue;
                const glm::mat4 toCell = child->transform.GetLocalMatrix();
                for (const auto& placement : layer->placements)
                {
                    if (!FindTreeCrown(placement.asset)) continue;
                    const glm::vec3 local(toCell * glm::vec4(placement.position, 1.f));
                    copy.trees.push_back({placement.asset, placement.scale,
                        cell.first * cellsX * cellWidth + local.x, cell.second * cellsY * cellDepth + local.z});
                }
            }
        (*loaded)[cell] = std::move(copy);
    }
    // Cells not loaded: their saved objects (an object file, else the
    // archive block), copied here.
    auto saved = std::make_shared<std::map<Cell, Engine::Serialization::JsonValue>>();
    for (const Cell cell : wanted)
        if (!loaded->count(cell))
            if (Engine::Serialization::JsonValue layers; manager.GetSavedObjectLayers(cell, layers))
                (*saved)[cell] = std::move(layers);
    const auto field = manager.GetTerrainField();
    if (!field) return {};
    const std::string archive = Engine::Components::Mesh::ResolveFilePath(heightArchive);
    const std::string dataDirectory = manager.cellDataDirectory;
    const uint64_t signature = m_configuredSignature;
    const MacroColorRules rules = MacroRulesOf(*this);
    const auto means = MakePalette(AltitudesOf(*this)).nearColors;
    const auto paint = m_paint;
    const float origin = map.originX, originZ = map.originZ, spacing = map.spacing;
    const int side = map.width, sideZ = map.height;
    return [=]() -> std::vector<TerrainManager::MacroColorPatch>
    {
        const int step = int(std::lround(spacing / cellWidth));
        if (step <= 0 || cellsX % step || originZ != origin || sideZ != side) return {};
        const int perCell = cellsX / step;
        const size_t vertices = static_cast<size_t>(cellsX + 1) * (cellsY + 1);
        const auto editPath = [&](Cell cell) {
            return dataDirectory.empty() ? std::string{} : (std::filesystem::path(dataDirectory) /
                (std::to_string(cell.first) + "_" + std::to_string(cell.second) + ".terrainedit")).string();
        };
        // A cell's archive heights (where the field has no surface), its
        // paint as the near cells have it, and its trees.
        struct CellData
        {
            bool valid = false;
            std::vector<float> seedHeights, weights, lushness;
            std::vector<Tree> trees;
        };
        std::map<Cell, CellData> data;
        const auto get = [&](Cell cell) -> const CellData&
        {
            auto [found, inserted] = data.try_emplace(cell);
            CellData& d = found->second;
            if (!inserted) return d;
            std::vector<uint8_t> codes;
            if (!TerrainHeightArchive::ReadCellCodes(archive, signature, extent, cellsX, cellsY,
                    cell.first, cell.second, d.seedHeights, codes)) return d;
            d.valid = true;
            if (const auto live = loaded->find(cell); live != loaded->end())
            {
                d.weights = live->second.weights;
                d.lushness = live->second.lushness;
                d.trees = live->second.trees;
            }
            else
            {
                // Not loaded: the baked paint, then the cell's saved hand paint.
                ReadBakedPaint(paint.get(), cell, vertices, d.weights, d.lushness);
                TerrainHeightArchive::EditEntries edits;
                if (TerrainHeightArchive::ReadEditEntries(editPath(cell), cellsX, cellsY, edits) &&
                    edits.layers == LayerCount)
                    for (const auto& [index, weights] : edits.weights)
                        if (index < vertices)
                            std::copy(weights.begin(), weights.end(), d.weights.begin() + index * LayerCount);
                // Its saved trees (object files, else its archive block):
                // cells without any have none.
                if (const auto found = saved->find(cell); found != saved->end())
                    for (size_t i = 0; i < found->second.ArraySize(); ++i)
                    {
                        Engine::Core::Object savedObject;
                        auto* layer = savedObject.AddComponent<TerrainObjectLayer>();
                        layer->Deserialize(found->second.ArrayAt(i));
                        if (!layer->farObjects) continue;
                        for (const auto& placement : layer->placements)
                            if (FindTreeCrown(placement.asset))
                                d.trees.push_back({placement.asset, placement.scale,
                                    cell.first * cellsX * cellWidth + placement.position.x,
                                    cell.second * cellsY * cellDepth + placement.position.z});
                    }
            }
            if (d.weights.size() != vertices * LayerCount) d.valid = false;
            return d;
        };
        std::vector<TerrainManager::MacroColorPatch> patches;
        for (const Cell cell : cells)
        {
            if (std::abs(cell.first) > extent || std::abs(cell.second) > extent) continue;
            const CellData& own = get(cell);
            if (!own.valid) continue;
            TerrainManager::MacroColorPatch patch;
            patch.width = patch.height = perCell + 1;
            patch.x0 = (cell.first + extent) * perCell;
            patch.y0 = (cell.second + extent) * perCell;
            patch.colors.assign(size_t(patch.width) * patch.height, glm::vec4(0.f));
            patch.normals.assign(patch.colors.size(), glm::vec3(0.f, 1.f, 0.f));
            const float originX = cell.first * cellsX * cellWidth;
            const float originZ = cell.second * cellsY * cellDepth;
            // The surface (top, under open sky) at each sample and its
            // neighbours a sample away, across cells: sculpts and volume
            // edits shape the slope (and so the cliff tint).
            const auto surface = [&](float x, float z, const float fallback) {
                const auto top = field->SurfaceAt(x, z);
                return top ? top->height : fallback;
            };
            for (int row = 0; row <= perCell; ++row)
                for (int col = 0; col <= perCell; ++col)
                {
                    const size_t vertex = static_cast<size_t>(row * step) * (cellsX + 1) + col * step;
                    const float x = originX + col * spacing, z = originZ + row * spacing;
                    const float height = surface(x, z, own.seedHeights[vertex]);
                    const float dx = (surface(x + spacing, z, height) - surface(x - spacing, z, height)) /
                        (2.f * spacing);
                    const float dz = (surface(x, z + spacing, height) - surface(x, z - spacing, height)) /
                        (2.f * spacing);
                    const float lush = own.lushness.size() == vertices ? own.lushness[vertex] : 0.f;
                    const glm::vec3 color = MacroGroundColor(&own.weights[vertex * LayerCount], lush, dx, dz, means);
                    const size_t sample = size_t(row) * patch.width + col;
                    patch.colors[sample] = glm::vec4(color * rules.brightness, 0.f);
                    patch.normals[sample] = glm::normalize(glm::vec3(-dx, 1.f, -dz));
                }
            // Canopy: the crowns of the trees in this cell and around it,
            // as the whole build splats them.
            std::vector<float> covered(patch.colors.size(), 0.f);
            for (int dz = -1; dz <= 1; ++dz)
                for (int dx = -1; dx <= 1; ++dx)
                {
                    const CellData& around = get({cell.first + dx, cell.second + dz});
                    for (const Tree& tree : around.trees)
                        SplatCrown(tree.asset, tree.scale, tree.x, tree.z, origin, spacing, side,
                            [&](int i, int j, float area) {
                                const int col = i - patch.x0, row = j - patch.y0;
                                if (col >= 0 && row >= 0 && col < patch.width && row < patch.height)
                                    covered[size_t(row) * patch.width + col] += area;
                            });
                }
            for (size_t i = 0; i < covered.size(); ++i)
                patch.colors[i].a = 1.f - std::exp(-covered[i] / (spacing * spacing));
            patches.push_back(std::move(patch));
        }
        return patches;
    };
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
                mapHalfExtentCells, manager->cellsX, manager->cellsY, palette.nearColors, m_paint.get());
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
        // Layer means come from material files: read them here, not on the job.
        const auto means = MakePalette(AltitudesOf(*this)).nearColors;
        const auto paint = m_paint;
        m_farLoad = SubmitJob([=]
        {
            FarGridLoad result;
            result.loaded = TerrainHeightArchive::ReadFarGrid(path, signature, extent,
                result.heights, &result.patches);
            if (!EnvironmentSwitch("ENGINE_FAR_PATCHES")) result.patches.clear();
            if (result.loaded)
                result.gridColors = ComputeFarColors(archive, signature, extent, cellsX,
                    cellsY, means, paint.get());
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

#include "Core/Rendering/DetailBudget.h"
#include "Scripts/Landscape/LandscapeDriver.h"
#include "Scripts/Grass/GrassField.h"
#include "Core/Compoonents/Terrain/TerrainHeightArchive.h"
#include "Core/Compoonents/Terrain/TerrainManager.h"
#include "Core/Compoonents/Terrain/TerrainObjectLayer.h"
#include "Core/Compoonents/Terrain/TerrainSurfaceSample.h"
#include "Core/Compoonents/Mesh/Mesh.h"
#include "Core/Compoonents/Materials/Material.h"
#include "Core/Compoonents/Camera/Camera.h"
#include "Core/Object.h"
#include "Core/Scene/Scene.h"
#include "Core/Serialization/SceneSerializer.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
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
    {.62f, .55f, .42f}, {.45f, .43f, .40f}, {.24f, .32f, .12f},
    {.22f, .15f, .10f}, {.40f, .38f, .34f}, {.85f, .88f, .92f}}};

// Mesh caches store the painted weights: change this with the paint rules.
constexpr uint64_t PaintRevision = 0x4c414e4453435035ull;
// Heights where rock and snow take over regardless of slope. Snow stays on
// the summits (the ranges reach about 120 m).
constexpr float RockLine = 58.f, SnowLine = 102.f;
// Forests grow between the shore and this height.
constexpr float TreeLine = 46.f;

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

// 0 in open meadow, 1 inside forest.
float ForestMask(float x, float z)
{
    const float n = .6f * ValueNoise(x, z, 260.f, 11u) +
        .28f * ValueNoise(x, z, 95.f, 23u) + .12f * ValueNoise(x, z, 37.f, 37u);
    return SmoothStep(.46f, .58f, n);
}

// Share of forest allowed at a height: none on shores or above the tree line.
float ForestBand(float height, float waterLevel)
{
    return SmoothStep(waterLevel + 4.f, waterLevel + 9.f, height) *
        (1.f - SmoothStep(TreeLine - 8.f, TreeLine, height));
}

// Six layer weights for one sample. rockClass is the archive's 0..3 code:
// slope and ridge rockiness computed over the whole map by the generator.
void PaintSample(float height, uint8_t rockClass, float worldX, float worldZ,
    float waterLevel, float* weights)
{
    const float detail = ValueNoise(worldX, worldZ, 22.f, 51u) - .5f;
    const float land = SmoothStep(waterLevel + 1.5f, waterLevel + 4.f, height);
    const float shore = SmoothStep(waterLevel - 3.5f, waterLevel - 1.f, height);
    const float rockSlope = rockClass / 3.f;
    const float rockHigh = SmoothStep(RockLine - 8.f, RockLine + 8.f, height + detail * 14.f);
    const float rock = land * std::max(rockSlope, rockHigh);
    const float snow = SmoothStep(SnowLine - 6.f, SnowLine + 6.f, height + detail * 10.f) *
        (1.f - .55f * rockSlope);
    const float soil = land * (1.f - rock) * ForestMask(worldX, worldZ) *
        ForestBand(height, waterLevel) * .85f;
    weights[Sand] = 1.f - shore;
    weights[Gravel] = shore * (1.f - land);
    weights[Snow] = land * snow;
    weights[Rock] = rock * (1.f - snow);
    weights[ForestSoil] = soil * (1.f - snow);
    weights[Grass] = std::max(0.f, land * (1.f - rock) * (1.f - snow) - weights[ForestSoil]);
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
TerrainManager::FarTerrainPalette MakePalette(float waterLevel)
{
    TerrainManager::FarTerrainPalette palette;
    for (int layer = 0; layer < LayerCount; ++layer)
        palette.nearColors[layer] = LayerMacroColor(LayerMaterials[layer]);
    const auto& mean = palette.nearColors;
    const glm::vec3 wooded = glm::mix(mean[Grass], mean[ForestSoil] * .6f, .35f);
    const glm::vec3 summit = glm::mix(mean[Rock], mean[Snow], .55f);
    palette.farColors = {{mean[Sand] * .85f, mean[Gravel] * .85f, mean[Grass] * .85f,
        wooded * .85f, mean[Rock] * .85f, summit * .85f}};
    palette.heightBoundaries = {{waterLevel - 2.f, waterLevel + 3.f,
        waterLevel + 30.f, RockLine, SnowLine}};
    palette.heightBlendHalfWidth = 5.f;
    palette.archiveToNear = {Sand, Gravel, Grass, Rock};
    return palette;
}

// Far-tile colors per cell corner: the near paint averaged over the corner's
// cell-sized neighborhood, with forest canopy over forest soil, so distant
// terrain shows the forests, rock, snow and lake beds the near cells show.
std::vector<glm::vec3> ComputeFarColors(const std::string& archivePath, uint64_t signature,
    int extent, int cellsX, int cellsY, float cellWidth, float cellDepth, float waterLevel,
    const std::array<glm::vec3, LayerCount>& means)
{
    const int cellSide = extent * 2 + 1;
    const int sampleSide = cellSide * cellsX + 1;
    if (cellsX != cellsY) return {};
    std::vector<float> heights(static_cast<size_t>(sampleSide) * sampleSide, 0.f);
    std::vector<uint8_t> codes(heights.size(), 0);
    std::vector<float> cellHeights;
    std::vector<uint8_t> cellCodes;
    for (int cy = -extent; cy <= extent; ++cy)
        for (int cx = -extent; cx <= extent; ++cx)
        {
            if (!TerrainHeightArchive::ReadCellCodes(archivePath, signature, extent,
                    cellsX, cellsY, cx, cy, cellHeights, cellCodes)) return {};
            const size_t originRow = static_cast<size_t>(cy + extent) * cellsY;
            const size_t originCol = static_cast<size_t>(cx + extent) * cellsX;
            for (int row = 0; row <= cellsY; ++row)
                for (int col = 0; col <= cellsX; ++col)
                {
                    const size_t source = static_cast<size_t>(row) * (cellsX + 1) + col;
                    const size_t target = (originRow + row) * sampleSide + originCol + col;
                    heights[target] = cellHeights[source];
                    codes[target] = cellCodes[source];
                }
        }
    // Canopy seen from afar: dark leaf green.
    const glm::vec3 canopy(.030f, .052f, .020f);
    const int cornerSide = cellSide + 1;
    std::vector<glm::vec3> colors(static_cast<size_t>(cornerSide) * cornerSide);
    const int step = 2; // every second sample: 8 m
    for (int gy = 0; gy < cornerSide; ++gy)
        for (int gx = 0; gx < cornerSide; ++gx)
        {
            glm::vec3 sum(0.f);
            float count = 0.f;
            const int centerRow = gy * cellsY, centerCol = gx * cellsX;
            for (int dr = -cellsY / 2; dr <= cellsY / 2; dr += step)
                for (int dc = -cellsX / 2; dc <= cellsX / 2; dc += step)
                {
                    const int row = centerRow + dr, col = centerCol + dc;
                    if (row < 0 || col < 0 || row >= sampleSide || col >= sampleSide) continue;
                    const size_t index = static_cast<size_t>(row) * sampleSide + col;
                    const float worldX = (col - extent * cellsX) * cellWidth;
                    const float worldZ = (row - extent * cellsY) * cellDepth;
                    float weights[LayerCount];
                    PaintSample(heights[index], codes[index], worldX, worldZ, waterLevel, weights);
                    glm::vec3 color(0.f);
                    for (int layer = 0; layer < LayerCount; ++layer)
                        color += means[layer] * weights[layer];
                    // Forest soil is .85 of the forest share; trees cover it.
                    color = glm::mix(color, canopy, std::min(1.f, weights[ForestSoil] / .85f));
                    sum += color;
                    count += 1.f;
                }
            colors[static_cast<size_t>(gy) * cornerSide + gx] = count > 0.f ? sum / count * .85f
                : glm::vec3(.1f);
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

// Forest for one cell: random candidates kept where the forest mask, the
// height band and the slope allow, plus a few lone trees in the meadows.
void GenerateForest(const TerrainManager::ObjectLayerContext& context,
    TerrainObjectLayer& layer, float waterLevel, int treesPerCell)
{
    if (!context.heights || context.cellsX <= 0 || context.cellsY <= 0 ||
        context.heights->size() != static_cast<size_t>(context.cellsX + 1) *
            (context.cellsY + 1)) return;
    // Forests read to the horizon of the loaded area: full trees near, then
    // the authored impostors (a few textured quads each) from 220 m out to
    // 900 m. Instanced layers keep three bands, so the chain names them
    // directly: LOD0, LOD1, impostor.
    layer.viewDistance = 900.f;
    layer.lod1Distance = 60.f;
    layer.lod2Distance = 220.f;
    layer.shadowDistance = 90.f;
    layer.shadowLod = 1;
    const std::string prefix = "Assets/Prefabs/Woodland/Oaks/";
    for (const char* species : Species)
    {
        auto& lod = layer.objectLods[prefix + species + "_LOD0.prefab"];
        lod.meshes = {prefix + species + "_LOD0.prefab", prefix + species + "_LOD1.prefab",
            prefix + species + "_Impostor.prefab"};
        lod.distances = {60.f, 220.f};
    }
    const float width = context.cellsX * context.cellWidth;
    const float depth = context.cellsY * context.cellDepth;
    uint32_t state = Hash(context.cell.first, context.cell.second, 0x74726565u) | 1u;
    const auto random = [&state]
    {
        state ^= state << 13; state ^= state >> 17; state ^= state << 5;
        return (state & 0xffffffu) / float(0x1000000);
    };
    // Each cell grows one stand: a large, a medium and a small species chosen
    // per cell. Every species in a cell costs a set of instanced draws per
    // LOD band, so stands keep distant forests cheap to submit while
    // neighbouring cells still mix all eight species.
    const uint32_t stand = Hash(context.cell.first, context.cell.second, 0x7374616eu);
    const std::array<size_t, 3> standSpecies{stand % 4u, 4u + (stand >> 8) % 2u,
        6u + (stand >> 16) % 2u};
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
        const float density = std::max(ForestMask(worldX, worldZ), .03f) *
            ForestBand(height, waterLevel);
        if (chance >= density) continue;
        TerrainObjectLayer::Placement placement;
        placement.id = ++id;
        // Forest interiors favour large trees, edges and meadows small ones.
        const float interior = ForestMask(worldX, worldZ);
        const float sizeClass = pick * .6f + (1.f - interior) * .4f;
        const size_t index = standSpecies[sizeClass < .5f ? 0 : sizeClass < .75f ? 1 : 2];
        placement.asset = prefix + Species[index] + "_LOD0.prefab";
        placement.position = {x, height - .08f, z};
        placement.yawDegrees = yaw * 360.f;
        placement.scale = .8f + size * .45f;
        layer.placements.push_back(std::move(placement));
    }
}

// Ground detail cooked by tools/PropCook: rocks from one scanned pack and a
// fallen log, each a LOD0/LOD1/LOD2 prefab chain, and the woodland bush pack
// (single LOD). As with trees, the map-wide asset count costs CPU time, so
// the landscape uses six of the twelve cooked rocks and six bushes.
constexpr int RockCount = 12;
const std::array<int, 6> LandscapeRocks{0, 2, 3, 7, 9, 11};
const std::array<const char*, 6> Bushes{
    "Small_bush_1", "Small_bush_2", "Medium_bush_1", "Medium_bush_2",
    "tall_bush_2", "tall_bush_3"};

std::string RockPrefab(int index, int lod)
{
    char name[96];
    std::snprintf(name, sizeof(name), "Assets/Prefabs/Woodland/Rocks/Rock_%02d_LOD%d.prefab",
        index + 1, lod);
    return name;
}

std::string LogPrefab(int lod)
{
    return "Assets/Prefabs/Woodland/Logs/FallenLog_LOD" + std::to_string(lod) + ".prefab";
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
    TerrainObjectLayer& layer, float waterLevel, bool undergrowth, float density)
{
    if (!context.heights || context.cellsX <= 0 || context.cellsY <= 0 ||
        context.heights->size() != static_cast<size_t>(context.cellsX + 1) *
            (context.cellsY + 1)) return;
    if (undergrowth && layer.objectLods.empty())
    {
        // A dedicated undergrowth layer (bushes have a single LOD).
        layer.viewDistance = 150.f;
        layer.lod1Distance = layer.lod2Distance = 150.f;
        layer.shadowDistance = 45.f;
    }
    else if (!undergrowth)
    {
        layer.viewDistance = 220.f;
        layer.lod1Distance = 35.f;
        layer.lod2Distance = 100.f;
        layer.shadowDistance = 80.f;
        layer.shadowLod = 1;
        for (int rock : LandscapeRocks)
        {
            auto& lod = layer.objectLods[RockPrefab(rock, 0)];
            lod.meshes = {RockPrefab(rock, 0), RockPrefab(rock, 1), RockPrefab(rock, 2)};
            lod.distances = {35.f, 100.f};
        }
        auto& log = layer.objectLods[LogPrefab(0)];
        log.meshes = {LogPrefab(0), LogPrefab(1), LogPrefab(2)};
        log.distances = {35.f, 100.f};
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
    const std::array<size_t, 2> standBushes{(stand >> 4) % 4u, 4u + (stand >> 12) % 2u};
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
        const float forest = ForestMask(worldX, worldZ) * ForestBand(height, waterLevel);
        if (undergrowth)
        {
            if (height < waterLevel + 1.5f || normal.y < .82f) continue;
            const float edge = 4.f * forest * (1.f - forest);
            const float clump = SmoothStep(.45f, .75f, ValueNoise(worldX, worldZ, 16.f, 211u));
            const float meadow = paint(x, z, Grass);
            const float odds = (.5f * edge + .08f * forest + .1f * meadow) * (.35f + 1.3f * clump) *
                (1.f - paint(x, z, Snow)) * (1.f - paint(x, z, Rock));
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
        // Logs: gentle forest floor only.
        if (kind < .16f)
        {
            if (normal.y < .93f || height < waterLevel + 2.f || chance >= .9f * forest) continue;
            place(LogPrefab(0), x, height - .04f, z, normal, yaw, .8f + .6f * size);
            continue;
        }
        const float rocky = std::min(1.f, paint(x, z, Rock) + .5f * paint(x, z, Snow));
        const float shore = paint(x, z, Gravel);
        const float field = SmoothStep(.66f, .82f, ValueNoise(worldX, worldZ, 70.f, 101u));
        const float odds = .025f + .45f * rocky + .3f * field + .25f * shore;
        if (chance >= odds) continue;
        // Mostly small stones; boulders in rock fields and on rock paint.
        float scale = .35f + 2.4f * std::pow(size, 2.4f) * (.45f + .55f * std::max(rocky, field));
        if (shore > .5f) scale = .25f + .45f * size;
        // Rocks lean partway with the slope, so they read as resting on it.
        const glm::vec3 lean = glm::normalize(glm::mix(normal, glm::vec3(0.f, 1.f, 0.f), .45f));
        const int rock = standRocks[pick < .5f ? 0 : 1];
        place(RockPrefab(rock, 0), x, height - .08f * scale, z, lean, yaw, scale);
    }
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
    RegisterField("treesPerCell", treesPerCell);
    RegisterField("forestLayerName", forestLayerName);
    RegisterField("grassRadius", grassRadius);
    RegisterField("propsLayerName", propsLayerName);
    RegisterField("undergrowthLayerName", undergrowthLayerName);
    RegisterField("propsDensity", propsDensity);
    RegisterField("enableWater", enableWater);
    RegisterField("waterMaterial", waterMaterial);
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
    const float level = waterLevel;
    const int trees = std::clamp(treesPerCell, 0, 600);
    const std::string forestName = forestLayerName;
    const std::string propsName = propsLayerName, undergrowthName = undergrowthLayerName;
    const float propDensity = std::clamp(propsDensity, 0.f, 4.f);
    const float tiling = manager->cellsX * manager->cellWidth / 2.f; // 2 m repeats

    manager->SetStaticCache(cacheDirectory, signature ^ PaintRevision ^
        static_cast<uint64_t>(std::lround(level * 16.f)));
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
    // The manager attaches a GrassField to its procedural grass layer; keep
    // the grass on the meadow layer and give it this map's density.
    const float radius = grassRadius;
    manager->SetObjectLayerSetup([radius, forestName](TerrainManager::Cell, TerrainObjectLayer& layer)
    {
        // Forests budget their detail in the project's "Trees" layer.
        if (layer.Owner && layer.layerName == forestName)
            layer.Owner->layer = Engine::Rendering::DetailBudget::Get().FindLayerOrDefault("Trees");
        auto* grass = layer.Owner ? layer.Owner->GetComponent<GrassField>() : nullptr;
        if (!grass || grass->proceduralLayerMask == (1 << Grass)) return;
        grass->proceduralLayerMask = 1 << Grass;
        grass->visibleRadius = radius;
        // Dense enough that blades close over the ground like a pelt.
        grass->shortPatchesPerSquareMeter = 2.6f;
        grass->tallPatchesPerSquareMeter = .45f;
        // Wildflower and weed drifts (meshes come from the manager's defaults).
        grass->accentPatchesPerSquareMeter = .18f;
        grass->MarkConfigurationDirty();
        layer.viewDistance = radius + 10.f;
        layer.lod1Distance = 7.f;
        layer.lod2Distance = 13.f;
        layer.MarkConfigurationDirty();
    });
    m_configuredSignature = signature;
    manager->ClearFarTerrain();
    m_farLoad = {};
    m_farAttempted = false;
    m_farDataReady = false;
    return true;
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
        if (result.loaded)
        {
            auto palette = MakePalette(waterLevel);
            palette.gridColors = ComputeFarColors(
                Engine::Components::Mesh::ResolveFilePath(heightArchive), m_configuredSignature,
                mapHalfExtentCells, manager->cellsX, manager->cellsY, manager->cellWidth,
                manager->cellDepth, waterLevel, palette.nearColors);
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
            camera->farPlane = 8000.f;
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
        const float level = waterLevel;
        // Layer means come from material files: read them here, not on the job.
        const auto means = MakePalette(level).nearColors;
        m_farLoad = SubmitJob([=]
        {
            FarGridLoad result;
            result.loaded = TerrainHeightArchive::ReadFarGrid(path, signature, extent,
                result.heights, &result.patches);
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
            auto palette = MakePalette(waterLevel);
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

#include "Core/Rendering/DetailBudget.h"
#include "Scripts/Landscape/LandscapeDriver.h"
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
    // Rock shapes are part of the heights themselves, so nothing else goes
    // into the key. Trails are scene objects (TerrainTrail scripts): the
    // manager carves them and adds them to the key itself.
    manager->SetStaticCache(cacheDirectory, signature ^ PaintRevision ^ paintKey);
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
        terrain.Rebuild();
    });
    // Trees, rocks and bushes are not generated here: they load from the
    // object archive (TerrainManager::objectArchive)
    // and the cells' own .objects files.
    manager->SetObjectLayerGenerator({});
    // Ground cover (grass, forest floor, shrubs, ferns, the far ring) and
    // the layers' draw settings are scene data: the TerrainManager's layer
    // settings carry each layer's GrassField, and where it grows is the
    // foliage density (TerrainFoliage.h: the paint, then painted edits).
    manager->SetObjectLayerSetup({});
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

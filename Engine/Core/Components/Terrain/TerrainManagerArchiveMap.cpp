#include "Core/Components/Terrain/TerrainManager.h"
#include "Core/Components/Terrain/TerrainHeightArchive.h"
#include "Core/Components/Terrain/TerrainObjectArchive.h"
#include "Core/Components/Terrain/TerrainPaintArchive.h"
#include "Core/Components/Camera/Camera.h"
#include "Core/Components/Materials/Material.h"
#include "Core/Components/Mesh/Mesh.h"
#include "Core/Scene/Object.h"
#include "Core/Scene/Scene.h"
#include "Core/Serialization/Json.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <set>

// What a map stored as archives shows beyond its loaded cells, read from the
// same archives and scene data (TerrainManager.h, "archive map"): the far
// terrain (corner heights and colours from the paint), the macro colour map
// (paint, lushness, steep faces, tree canopy) and the preview on load.
namespace Engine::Components
{
namespace
{
using Cell = TerrainManager::Cell;

glm::vec3 MaterialMacroColor(const std::string& path)
{
    Material material;
    if (path.empty() || !material.LoadFromFile(Mesh::ResolveFilePath(path))) return glm::vec3(.2f);
    return glm::length(material.terrainMacroColor) > 0.f ? material.terrainMacroColor : material.diffuseColor;
}

// What the far terrain and the macro map are composed from (scene thread
// snapshot; the jobs run on workers).
struct ArchiveMapInputs
{
    std::string archive;
    uint64_t signature = 0;
    int extent = 0, cellsX = 0, cellsY = 0;
    float cellWidth = 0.f, cellDepth = 0.f;
    std::vector<glm::vec3> means;          // per terrain layer
    size_t defaultLayer = 0;
    std::shared_ptr<const TerrainPaintArchive> paint;
    float brightness = .85f;
    glm::vec3 steep{-1.f};                 // negative: no steep colour
};

// A cell's baked paint: its layer weights and lushness per grid vertex. A
// cell the archive lacks (or no archive) is all the default layer.
void ReadBakedPaint(const ArchiveMapInputs& in, Cell cell, size_t vertices,
    std::vector<float>& weights, std::vector<float>& lushness)
{
    const size_t layers = in.means.size();
    if (in.paint && in.paint->GetHeader().layers == int(layers) && in.paint->ReadCell(cell, weights, lushness) &&
        weights.size() == vertices * layers && lushness.size() == vertices)
        return;
    weights.assign(vertices * layers, 0.f);
    if (layers)
        for (size_t v = 0; v < vertices; ++v) weights[v * layers + in.defaultLayer] = 1.f;
    lushness.assign(vertices, 0.f);
}

glm::vec3 PaintColor(const ArchiveMapInputs& in, const float* weights)
{
    glm::vec3 color(0.f);
    for (size_t layer = 0; layer < in.means.size(); ++layer) color += in.means[layer] * weights[layer];
    return color;
}

// Far colours per cell corner: the paint averaged over the corner's
// cell-sized neighbourhood (every fourth sample of each cell, streamed a cell
// at a time), so distant terrain shows the rock, snow and lake beds the near
// cells show. The ground only: canopy comes from the real trees.
std::vector<glm::vec3> ComputeFarColors(const ArchiveMapInputs& in)
{
    const int extent = in.extent, cellsX = in.cellsX, cellsY = in.cellsY;
    if (cellsX != cellsY || in.means.empty()) return {};
    const int cornerSide = extent * 2 + 2;
    std::vector<glm::vec4> sums(size_t(cornerSide) * cornerSide, glm::vec4(0.f));
    const int step = std::max(1, cellsX / 8);
    const int half = cellsX / 2;
    const size_t layers = in.means.size();
    const bool read = TerrainHeightArchive::ForEachCell(in.archive, in.signature, extent, cellsX, cellsY,
        [&](int cx, int cy, const std::vector<float>& heights, const std::vector<uint8_t>&)
    {
        std::vector<float> painted, lushness;
        ReadBakedPaint(in, {cx, cy}, heights.size(), painted, lushness);
        // Rows and columns shared with the next cell are visited there,
        // except along the map's far edges.
        const int lastRow = cy == extent ? cellsY : cellsY - 1;
        const int lastCol = cx == extent ? cellsX : cellsX - 1;
        for (int row = 0; row <= lastRow; row += step)
            for (int col = 0; col <= lastCol; col += step)
            {
                const size_t index = size_t(row) * (cellsX + 1) + col;
                const glm::vec3 color = PaintColor(in, &painted[index * layers]);
                for (int dy = 0; dy <= 1; ++dy)
                    for (int dx = 0; dx <= 1; ++dx)
                    {
                        if (std::abs(row - dy * cellsY) > half || std::abs(col - dx * cellsX) > half) continue;
                        sums[size_t(cy + extent + dy) * cornerSide + cx + extent + dx] += glm::vec4(color, 1.f);
                    }
            }
        return true;
    });
    if (!read) return {};
    std::vector<glm::vec3> colors(sums.size());
    for (size_t i = 0; i < sums.size(); ++i)
        colors[i] = sums[i].w > 0.f ? glm::vec3(sums[i]) / sums[i].w * in.brightness : glm::vec3(.1f);
    return colors;
}

// Darker, greener lush ground: the tint Object.hlsl gives near cells by
// Terrain::lushness (keep the two the same).
const glm::vec3 LushTint(.72f, .92f, .55f);

// One macro sample's ground colour (linear, no canopy): the paint, darker
// and greener by lushness (Object.hlsl tints near cells the same way), and
// the steep colour on steep faces (VolumeTerrain's band, 1 - normal.y
// .25 .. .45). dx, dz: rise over run across the samples.
glm::vec3 MacroGroundColor(const ArchiveMapInputs& in, const float* weights, float lushness, float dx, float dz)
{
    glm::vec3 color = PaintColor(in, weights) *
        glm::mix(glm::vec3(1.f), LushTint, std::clamp(lushness, 0.f, 1.f));
    if (in.steep.x < 0.f) return color;
    const float normalY = 1.f / std::sqrt(1.f + dx * dx + dz * dz);
    float t = std::clamp((1.f - normalY - .25f) / .2f, 0.f, 1.f);
    t = t * t * (3.f - 2.f * t);
    return glm::mix(color, in.steep, t);
}

// Crown footprint of a tree at scale 1 (TerrainManager::canopyCrownTable):
// radius (m) and covered area seen from above (m^2), keyed by an asset path
// suffix (family/name, without .prefab).
struct CanopyCrown { std::string asset; float radius, area; };
using CanopyCrowns = std::vector<CanopyCrown>;

const CanopyCrown* FindCrown(const CanopyCrowns& crowns, const std::string& asset)
{
    for (const auto& entry : crowns)
    {
        const std::string suffix = entry.asset + ".prefab";
        if (asset.size() >= suffix.size() && !asset.compare(asset.size() - suffix.size(), suffix.size(), suffix))
            return &entry;
    }
    return nullptr;
}

// Covered crown area a tree adds to the samples of a grid around it (side x
// side, spacing apart from origin): its crown's area spread over the samples
// within its reach, weighted by closeness.
template<class Visit>
void SplatCrown(const CanopyCrown& crown, float scale, float wx, float wz, float origin,
    float spacing, int side, Visit&& visit)
{
    const float radius = crown.radius * scale + .5f * spacing;
    const float area = crown.area * scale * scale;
    const int i0 = std::max(0, int(std::floor((wx - radius - origin) / spacing)));
    const int i1 = std::min(side - 1, int(std::ceil((wx + radius - origin) / spacing)));
    const int j0 = std::max(0, int(std::floor((wz - radius - origin) / spacing)));
    const int j1 = std::min(side - 1, int(std::ceil((wz + radius - origin) / spacing)));
    const auto weight = [&](int i, int j) {
        return std::max(0.f, 1.f - std::hypot(origin + i * spacing - wx, origin + j * spacing - wz) / radius);
    };
    float total = 0.f;
    for (int j = j0; j <= j1; ++j)
        for (int i = i0; i <= i1; ++i) total += weight(i, j);
    if (total <= 0.f) return;
    for (int j = j0; j <= j1; ++j)
        for (int i = i0; i <= i1; ++i)
            if (const float w = weight(i, j); w > 0.f) visit(i, j, area * w / total);
}

std::shared_ptr<const CanopyCrowns> LoadCrowns(const std::string& table)
{
    auto crowns = std::make_shared<CanopyCrowns>();
    if (table.empty()) return crowns;
    const std::string path = Mesh::ResolveFilePath(table);
    try
    {
        const auto document = Engine::Serialization::JsonParseFile(path);
        const auto& entries = document["crowns"];
        for (size_t i = 0; i < entries.ObjectSize(); ++i)
            crowns->push_back({entries.ObjectKey(i), entries.ObjectValue(i)["radius"].AsFloat(),
                entries.ObjectValue(i)["area"].AsFloat()});
    }
    catch (...) {}
    if (crowns->empty())
        std::fprintf(stderr, "[TerrainManager] no tree crowns in %s: the macro map has no canopy\n", path.c_str());
    return crowns;
}

// The whole macro map: every fourth sample of each cell (16 m on 4 m quads)
// with its slope normal; alpha is the canopy cover of the baked trees.
// Rows along +Z from the map's -X/-Z corner.
TerrainManager::MacroColorImage BuildMacroColors(const ArchiveMapInputs& in,
    std::shared_ptr<const TerrainObjectArchive> objects, std::shared_ptr<const CanopyCrowns> crowns)
{
    const int extent = in.extent, cellsX = in.cellsX, cellsY = in.cellsY;
    if (cellsX != cellsY || cellsX % 4 != 0 || in.means.empty()) return {};
    const int step = 4, perCell = cellsX / step;
    const int side = (extent * 2 + 1) * perCell + 1;
    const float spacing = in.cellWidth * step;
    const float origin = -float(extent) * cellsX * in.cellWidth;
    const size_t layers = in.means.size();
    std::vector<glm::vec4> colors(size_t(side) * side, glm::vec4(.1f, .1f, .1f, 0.f));
    std::vector<glm::vec3> normals(colors.size(), glm::vec3(0.f, 1.f, 0.f));
    std::vector<float> covered(colors.size(), 0.f);
    const bool read = TerrainHeightArchive::ForEachCell(in.archive, in.signature, extent, cellsX, cellsY,
        [&](int cx, int cy, const std::vector<float>& heights, const std::vector<uint8_t>&)
    {
        // Canopy: the crowns of the cell's baked trees (layers drawn far).
        std::vector<TerrainObjectArchive::Layer> objectLayers;
        if (objects && crowns && objects->ReadCell({cx, cy}, objectLayers))
            for (const auto& layer : objectLayers)
            {
                const auto* layerTemplate = objects->Template(layer.name);
                if (layerTemplate && layerTemplate->Has("farObjects") && !(*layerTemplate)["farObjects"].AsBool())
                    continue;
                for (const auto& placement : layer.placements)
                    if (const auto* crown = FindCrown(*crowns, placement.asset))
                        SplatCrown(*crown, placement.scale, cx * cellsX * in.cellWidth + placement.position.x,
                            cy * cellsY * in.cellDepth + placement.position.z, origin, spacing, side,
                            [&](int i, int j, float area) { covered[size_t(j) * side + i] += area; });
            }
        std::vector<float> painted, lushness;
        ReadBakedPaint(in, {cx, cy}, heights.size(), painted, lushness);
        const auto height = [&](int row, int col) {
            row = std::clamp(row, 0, cellsY); col = std::clamp(col, 0, cellsX);
            return heights[size_t(row) * (cellsX + 1) + col];
        };
        for (int row = 0; row <= cellsY; row += step)
            for (int col = 0; col <= cellsX; col += step)
            {
                const size_t index = size_t(row) * (cellsX + 1) + col;
                const float dx = (height(row, col + step) - height(row, col - step)) / (2.f * spacing);
                const float dz = (height(row + step, col) - height(row - step, col)) /
                    (2.f * step * in.cellDepth);
                const size_t sample = size_t((cy + extent) * perCell + row / step) * side +
                    size_t((cx + extent) * perCell + col / step);
                colors[sample] = glm::vec4(MacroGroundColor(in, &painted[index * layers], lushness[index], dx, dz) *
                    in.brightness, 0.f);
                normals[sample] = glm::normalize(glm::vec3(-dx, 1.f, -dz));
            }
        return true;
    });
    if (!read) return {};
    // Object.hlsl paints this canopy only past the real trees' draw distance
    // (farCanopyDistance).
    for (size_t i = 0; i < colors.size(); ++i) colors[i].a = 1.f - std::exp(-covered[i] / (spacing * spacing));
    return TerrainManager::EncodeMacroColorImage(side, side, origin, origin, spacing, colors, &normals);
}
}

std::shared_ptr<const void> TerrainManager::ArchiveCanopyCrowns() const
{
    if (!m_canopyCrowns || m_canopyCrownsLoaded != canopyCrownTable)
    {
        m_canopyCrownsLoaded = canopyCrownTable;
        m_canopyCrowns = LoadCrowns(canopyCrownTable);
    }
    return m_canopyCrowns;
}

namespace
{
ArchiveMapInputs ArchiveMapSnapshot(const TerrainManager& manager)
{
    ArchiveMapInputs in;
    in.archive = manager.heightArchive.empty() ? std::string{} : Mesh::ResolveFilePath(manager.heightArchive);
    in.signature = manager.ArchiveSignature();
    in.extent = manager.ArchiveHalfExtent();
    in.cellsX = manager.cellsX; in.cellsY = manager.cellsY;
    in.cellWidth = manager.cellWidth; in.cellDepth = manager.cellDepth;
    // Material files are read here, on the scene thread.
    for (const auto& layer : manager.terrainLayers) in.means.push_back(MaterialMacroColor(layer.material));
    in.defaultLayer = in.means.empty() ? 0 : size_t(std::clamp(manager.defaultPaintLayer, 0, int(in.means.size()) - 1));
    in.paint = manager.GetPaintArchive();
    in.brightness = std::clamp(manager.archiveColorBrightness, .1f, 2.f);
    if (!manager.steepMaterial.empty()) in.steep = MaterialMacroColor(manager.steepMaterial);
    return in;
}
}

uint64_t TerrainManager::ArchiveMapKey() const
{
    if (heightArchive.empty()) return 0;
    const uint64_t signature = ArchiveSignature();
    if (!signature) return 0;
    uint64_t key = signature ^ 0x4d4143524f434c52ull;
    const auto mix = [&](const void* data, size_t size) {
        for (size_t i = 0; i < size; ++i) key = (key ^ static_cast<const uint8_t*>(data)[i]) * 1099511628211ull;
    };
    const auto paint = GetPaintArchive();
    const uint64_t paintKey = paint ? paint->Signature() : 0;
    mix(&paintKey, sizeof(paintKey));
    const auto objects = GetObjectArchive();
    const uint64_t objectKey = objects ? objects->Signature() : 0;
    mix(&objectKey, sizeof(objectKey));
    for (const auto& layer : terrainLayers) mix(layer.material.data(), layer.material.size() + 1);
    mix(&defaultPaintLayer, sizeof(defaultPaintLayer));
    mix(&archiveColorBrightness, sizeof(archiveColorBrightness));
    mix(steepMaterial.data(), steepMaterial.size() + 1);
    mix(canopyCrownTable.data(), canopyCrownTable.size() + 1);
    mix(&cellsX, sizeof(cellsX)); mix(&cellsY, sizeof(cellsY));
    mix(&cellWidth, sizeof(cellWidth)); mix(&cellDepth, sizeof(cellDepth));
    return key ? key : 1;
}

TerrainManager::FarTerrainPalette TerrainManager::ArchiveFarPalette() const
{
    const ArchiveMapInputs in = ArchiveMapSnapshot(*this);
    FarTerrainPalette palette;
    // The corner grid colours cover the map; the height bands are only the
    // fallback outside it: the layers in order.
    for (size_t layer = 0; layer < palette.nearColors.size(); ++layer)
    {
        const glm::vec3 mean = in.means.empty() ? glm::vec3(.2f) : in.means[std::min(layer, in.means.size() - 1)];
        palette.nearColors[layer] = mean;
        palette.farColors[layer] = mean * in.brightness;
    }
    if (in.steep.x >= 0.f) palette.steepColor = in.steep * in.brightness;
    palette.gridHalfExtent = in.extent;
    return palette;
}

void TerrainManager::InstallArchiveMacroColors()
{
    if (!macroColorMap)
    {
        if (m_archiveMacroInstalled) SetMacroColorSource({});
        m_archiveMacroInstalled = false;
        return;
    }
    MacroColorSource source;
    source.revision = [this] { return macroColorMap && m_archiveBuilder ? ArchiveMapKey() : 0; };
    source.makeBuild = [this]() -> std::function<MacroColorImage()>
    {
        if (!m_archiveBuilder) return {};
        ArchiveMapInputs in = ArchiveMapSnapshot(*this);
        if (!in.signature) return {};
        auto objects = GetObjectArchive();
        auto crowns = std::static_pointer_cast<const CanopyCrowns>(ArchiveCanopyCrowns());
        return [in = std::move(in), objects, crowns] { return BuildMacroColors(in, objects, crowns); };
    };
    source.makeCellUpdate = [this](const std::vector<Cell>& cells, const MacroColorImage& map)
        -> std::function<std::vector<MacroColorPatch>()>
    {
        if (!m_archiveBuilder || cells.empty() || map.width <= 0) return {};
        return MakeArchiveMacroCellUpdate(cells, map);
    };
    SetMacroColorSource(std::move(source));
    m_archiveMacroInstalled = true;
}

// Recomposes edited cells from the terrain as it is: heights from the field
// (sculpts and volume edits), the cells' paint (loaded, else baked plus
// saved hand paint) and the trees in and around them for the canopy.
std::function<std::vector<TerrainManager::MacroColorPatch>()> TerrainManager::MakeArchiveMacroCellUpdate(
    const std::vector<Cell>& cells, const MacroColorImage& map)
{
    if (cellsX != cellsY || cellWidth != cellDepth || cellsX % 4 != 0) return {};
    const ArchiveMapInputs in = ArchiveMapSnapshot(*this);
    if (!in.signature || in.means.empty()) return {};
    const size_t layers = in.means.size();
    const auto crowns = std::static_pointer_cast<const CanopyCrowns>(ArchiveCanopyCrowns());
    struct Tree { const CanopyCrown* crown; float scale, x, z; };
    struct LoadedCell { std::vector<float> weights, lushness; std::vector<Tree> trees; };
    auto loaded = std::make_shared<std::map<Cell, LoadedCell>>();
    std::set<Cell> wanted;
    for (const Cell cell : cells)
        for (int dz = -1; dz <= 1; ++dz)
            for (int dx = -1; dx <= 1; ++dx) wanted.insert({cell.first + dx, cell.second + dz});
    // Loaded cells as they are now, copied here (scene thread).
    for (const Cell cell : wanted)
    {
        const Terrain* terrain = GetLoadedCell(cell.first, cell.second);
        if (!terrain || terrain->heights.size() != size_t(cellsX + 1) * (cellsY + 1) ||
            terrain->weights.size() != terrain->heights.size() * layers) continue;
        LoadedCell copy;
        copy.weights = terrain->weights;
        if (terrain->lushness.size() == terrain->heights.size()) copy.lushness = terrain->lushness;
        if (terrain->Owner)
            for (auto* child : terrain->Owner->Children)
            {
                auto* layer = child ? child->GetComponent<TerrainObjectLayer>() : nullptr;
                if (!layer || !layer->farObjects) continue;
                const glm::mat4 toCell = child->transform.GetLocalMatrix();
                for (const auto& placement : layer->placements)
                    if (const auto* crown = FindCrown(*crowns, placement.asset))
                    {
                        const glm::vec3 local(toCell * glm::vec4(placement.position, 1.f));
                        copy.trees.push_back({crown, placement.scale,
                            cell.first * cellsX * cellWidth + local.x, cell.second * cellsY * cellDepth + local.z});
                    }
            }
        (*loaded)[cell] = std::move(copy);
    }
    // Cells not loaded: their saved objects (an object file, else the
    // archive block).
    auto saved = std::make_shared<std::map<Cell, JsonValue>>();
    for (const Cell cell : wanted)
        if (!loaded->count(cell))
            if (JsonValue savedLayers; GetSavedObjectLayers(cell, savedLayers)) (*saved)[cell] = std::move(savedLayers);
    const auto field = GetTerrainField();
    if (!field) return {};
    const std::string dataDirectory = cellDataDirectory;
    const float origin = map.originX, originZ = map.originZ, spacing = map.spacing;
    const int side = map.width, sideZ = map.height;
    const int countX = cellsX, countY = cellsY;
    const float width = cellWidth, depth = cellDepth;
    return [=]() -> std::vector<MacroColorPatch>
    {
        const int step = int(std::lround(spacing / width));
        if (step <= 0 || countX % step || originZ != origin || sideZ != side) return {};
        const int perCell = countX / step;
        const size_t vertices = size_t(countX + 1) * (countY + 1);
        const auto editPath = [&](Cell cell) {
            return dataDirectory.empty() ? std::string{} : (std::filesystem::path(dataDirectory) /
                (std::to_string(cell.first) + "_" + std::to_string(cell.second) + ".terrainedit")).string();
        };
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
            if (!TerrainHeightArchive::ReadCellCodes(in.archive, in.signature, in.extent, countX, countY,
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
                ReadBakedPaint(in, cell, vertices, d.weights, d.lushness);
                TerrainHeightArchive::EditEntries edits;
                if (TerrainHeightArchive::ReadEditEntries(editPath(cell), countX, countY, edits) &&
                    edits.layers == int(layers))
                    for (const auto& [index, weights] : edits.weights)
                        if (index < vertices)
                            std::copy(weights.begin(), weights.end(), d.weights.begin() + index * layers);
                if (const auto found = saved->find(cell); found != saved->end())
                    for (size_t i = 0; i < found->second.ArraySize(); ++i)
                    {
                        Engine::Core::Object savedObject;
                        auto* layer = savedObject.AddComponent<TerrainObjectLayer>();
                        layer->Deserialize(found->second.ArrayAt(i));
                        if (!layer->farObjects) continue;
                        for (const auto& placement : layer->placements)
                            if (const auto* crown = FindCrown(*crowns, placement.asset))
                                d.trees.push_back({crown, placement.scale,
                                    cell.first * countX * width + placement.position.x,
                                    cell.second * countY * depth + placement.position.z});
                    }
            }
            if (d.weights.size() != vertices * layers) d.valid = false;
            return d;
        };
        std::vector<MacroColorPatch> patches;
        for (const Cell cell : cells)
        {
            if (std::abs(cell.first) > in.extent || std::abs(cell.second) > in.extent) continue;
            const CellData& own = get(cell);
            if (!own.valid) continue;
            MacroColorPatch patch;
            patch.width = patch.height = perCell + 1;
            patch.x0 = (cell.first + in.extent) * perCell;
            patch.y0 = (cell.second + in.extent) * perCell;
            patch.colors.assign(size_t(patch.width) * patch.height, glm::vec4(0.f));
            patch.normals.assign(patch.colors.size(), glm::vec3(0.f, 1.f, 0.f));
            const float cellOriginX = cell.first * countX * width;
            const float cellOriginZ = cell.second * countY * depth;
            // The top surface at each sample and a sample away, across cells.
            const auto surface = [&](float x, float z, float fallback) {
                const auto top = field->SurfaceAt(x, z);
                return top ? top->height : fallback;
            };
            for (int row = 0; row <= perCell; ++row)
                for (int col = 0; col <= perCell; ++col)
                {
                    const size_t vertex = size_t(row * step) * (countX + 1) + col * step;
                    const float x = cellOriginX + col * spacing, z = cellOriginZ + row * spacing;
                    const float height = surface(x, z, own.seedHeights[vertex]);
                    const float dx = (surface(x + spacing, z, height) - surface(x - spacing, z, height)) / (2.f * spacing);
                    const float dz = (surface(x, z + spacing, height) - surface(x, z - spacing, height)) / (2.f * spacing);
                    const float lush = own.lushness.size() == vertices ? own.lushness[vertex] : 0.f;
                    const size_t sample = size_t(row) * patch.width + col;
                    patch.colors[sample] = glm::vec4(
                        MacroGroundColor(in, &own.weights[vertex * layers], lush, dx, dz) * in.brightness, 0.f);
                    patch.normals[sample] = glm::normalize(glm::vec3(-dx, 1.f, -dz));
                }
            std::vector<float> covered(patch.colors.size(), 0.f);
            for (int dz = -1; dz <= 1; ++dz)
                for (int dx = -1; dx <= 1; ++dx)
                    for (const Tree& tree : get({cell.first + dx, cell.second + dz}).trees)
                        SplatCrown(*tree.crown, tree.scale, tree.x, tree.z, origin, spacing, side,
                            [&](int i, int j, float area) {
                                const int col = i - patch.x0, row = j - patch.y0;
                                if (col >= 0 && row >= 0 && col < patch.width && row < patch.height)
                                    covered[size_t(row) * patch.width + col] += area;
                            });
            for (size_t i = 0; i < covered.size(); ++i)
                patch.colors[i].a = 1.f - std::exp(-covered[i] / (spacing * spacing));
            patches.push_back(std::move(patch));
        }
        return patches;
    };
}

bool TerrainManager::LoadArchiveFarTerrain(bool wait)
{
    const uint64_t key = ArchiveMapKey();
    if (!archiveFarTerrain || !m_archiveBuilder || !key) return false;
    if (key == m_archiveFarKey) return true;
    if (!m_archiveFarJob.valid() || m_archiveFarJobKey != key)
    {
        if (m_archiveFarJob.valid()) m_archiveFarJob.wait();
        // The corner grid straight from the height archive; sculpts are
        // applied by ConfigureFarTerrain.
        const ArchiveMapInputs in = ArchiveMapSnapshot(*this);
        m_archiveFarJobKey = key;
        m_archiveFarJob = std::async(wait ? std::launch::deferred : std::launch::async, [in]
        {
            ArchiveFarGrid grid;
            grid.loaded = TerrainHeightArchive::ReadCornerGrid(in.archive, in.signature, in.extent,
                in.cellsX, in.cellsY, grid.heights);
            if (grid.loaded) grid.colors = ComputeFarColors(in);
            return grid;
        });
    }
    if (!wait && m_archiveFarJob.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return false;
    ArchiveFarGrid grid = m_archiveFarJob.get();
    m_archiveFarJob = {};
    // Mark it done either way: an unreadable archive is not retried every frame.
    m_archiveFarKey = key;
    if (!grid.loaded) return false;
    FarTerrainPalette palette = ArchiveFarPalette();
    palette.gridColors = std::move(grid.colors);
    ConfigureFarTerrain(ArchiveHalfExtent(), std::move(grid.heights), {},
        Mesh::ResolveFilePath(heightArchive), ArchiveSignature(), std::move(palette));
    m_archiveFarActive = true;
    return true;
}

void TerrainManager::UpdateArchiveMap(const Object* viewer)
{
    if (!m_archiveBuilder) return;
    if (!archiveFarTerrain)
    {
        if (m_archiveFarActive) SetFarTerrainEnabled(false);
        return;
    }
    LoadArchiveFarTerrain(false);
    if (!m_archiveFarActive) return;
    SetFarTerrainEnabled(true);
    // The editor's streaming (UpdateEditorStreaming) moves the far terrain
    // with its own camera.
    if (!viewer || !Owner) return;
    const glm::dvec3 local = glm::dvec3(glm::inverse(Owner->transform.GetWorldMatrixD()) *
        glm::dvec4(viewer->transform.GetWorldPositionD(), 1.0));
    const double spanX = double(cellsX) * cellWidth, spanZ = double(cellsY) * cellDepth;
    if (std::isfinite(local.x) && std::isfinite(local.z) && spanX > 0.0 && spanZ > 0.0)
        UpdateFarTerrain(int(std::floor(local.x / spanX)), int(std::floor(local.z / spanZ)),
            viewer->GetComponent<Camera>());
}

void TerrainManager::PreviewArchiveMap()
{
    if (!m_archiveBuilder || !Owner) return;
    // ENGINE_TERRAIN_LOADED_RADIUS=<cells> overrides the full-detail radius (A/B runs).
    if (const char* forced = std::getenv("ENGINE_TERRAIN_LOADED_RADIUS"); forced && forced[0])
        loadedRadius = std::clamp(std::atoi(forced), 0, 32);
    const float spanX = cellsX * cellWidth, spanZ = cellsY * cellDepth;
    if (spanX <= 0.f || spanZ <= 0.f) return;
    // Load the cells around the viewer and the far terrain at once, so the
    // map shows as soon as it opens (the editor has no streaming loop yet).
    glm::dvec3 start(0.0);
    if (const auto* viewer = viewerObjectName.empty() ? nullptr : Owner->FindObjectInSceneByName(viewerObjectName))
        start = glm::dvec3(glm::inverse(Owner->transform.GetWorldMatrixD()) *
            glm::dvec4(viewer->transform.GetWorldPositionD(), 1.0));
    const int previousFarRadius = farRadius;
    farRadius = loadedRadius;
    SyncLoadedCells(int(std::floor(start.x / spanX)), int(std::floor(start.z / spanZ)));
    farRadius = previousFarRadius;
    if (archiveFarTerrain) LoadArchiveFarTerrain(true);
    if (auto* scene = Owner->GetScene())
    {
        scene->editorCamera.transform.position = start + glm::dvec3{0.0, 60.0, -90.0};
        if (auto* camera = scene->editorCamera.GetComponent<Camera>())
        {
            camera->target = glm::vec3(start);
            // See the whole map from its centre.
            camera->farPlane = std::max(8000.f, 1.5f * (ArchiveHalfExtent() + 1) * std::max(spanX, spanZ));
        }
    }
}
}

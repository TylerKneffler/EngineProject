#include "Core/Components/Terrain/TerrainPlacementBrush.h"
#include "Core/Brushes/BrushRules.h"
#include "Core/Components/Terrain/Terrain.h"
#include "Core/Components/Terrain/TerrainBrushShape.h"
#include "Core/Components/Terrain/TerrainField.h"
#include "Core/Components/Terrain/TerrainManager.h"
#include "Core/Components/Terrain/TerrainObjectLayer.h"
#include "Core/Components/Terrain/TerrainRegionEdit.h"
#include "Core/Components/Terrain/TerrainSurfaceSample.h"
#include "Core/Scene/Object.h"
#include "Core/Scene/Scene.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <set>

namespace Engine::Components
{
namespace
{
float NextRandom(uint32_t& state)
{
    state ^= state << 13; state ^= state >> 17; state ^= state << 5;
    return (state & 0xffffffu) / float(0x1000000);
}

// The cell under a point of the target's local space and the point in the
// cell's own space.
struct Spot
{
    Terrain* cell = nullptr;
    glm::dvec2 cellOrigin{0.0};
};

Spot CellAt(TerrainManager* manager, Terrain* terrain, double x, double z)
{
    if (!manager) return {terrain, {0.0, 0.0}};
    const double spanX = double(manager->cellsX) * manager->cellWidth;
    const double spanZ = double(manager->cellsY) * manager->cellDepth;
    if (!(spanX > 0.0) || !(spanZ > 0.0)) return {};
    const int cx = int(std::floor(x / spanX)), cz = int(std::floor(z / spanZ));
    return {manager->GetLoadedCell(cx, cz), {cx * spanX, cz * spanZ}};
}

TerrainObjectLayer* FindLayer(const Terrain& cell, const std::string& name)
{
    if (!cell.Owner) return nullptr;
    for (auto* child : cell.Owner->Children)
        if (auto* layer = child ? child->GetComponent<TerrainObjectLayer>() : nullptr)
            if (layer->layerName == name) return layer;
    return nullptr;
}

TerrainObjectLayer* TargetLayer(Engine::Scene::Scene& scene, Terrain& cell, const std::string& name,
    const TerrainManager* manager)
{
    if (auto* layer = FindLayer(cell, name)) return layer;
    if (!cell.Owner) return nullptr;
    auto* child = scene.AddObject(name);
    scene.MoveObject(child, cell.Owner, Engine::Scene::Scene::ObjectPlacement::AsChild);
    child->transform.position = {0, 0, 0};
    child->transform.rotation = {0, 0, 0};
    child->transform.scale = {1, 1, 1};
    auto* layer = child->AddComponent<TerrainObjectLayer>();
    layer->layerName = name;
    // The layer name's settings (view, LOD and shadow distances).
    if (manager) manager->ApplyObjectLayerSettings(*layer);
    return layer;
}

// The cell's paint at a point of its own space (bilinear over vertices).
std::vector<float> PaintAt(const Terrain& cell, double x, double z)
{
    const size_t layers = cell.layers.size();
    std::vector<float> paint(layers, 0.f);
    if (!layers || cell.weights.size() != cell.heights.size() * layers) return paint;
    const double fx = std::clamp(x / cell.cellWidth, 0.0, double(cell.cellsX));
    const double fz = std::clamp(z / cell.cellDepth, 0.0, double(cell.cellsY));
    const int col = std::min(int(fx), cell.cellsX - 1), row = std::min(int(fz), cell.cellsY - 1);
    const float tx = float(fx - col), tz = float(fz - row);
    for (size_t layer = 0; layer < layers; ++layer)
    {
        const auto at = [&](int c, int r) { return cell.weights[(size_t(r) * (cell.cellsX + 1) + c) * layers + layer]; };
        paint[layer] = (at(col, row) * (1.f - tx) + at(col + 1, row) * tx) * (1.f - tz) +
            (at(col, row + 1) * (1.f - tx) + at(col + 1, row + 1) * tx) * tz;
    }
    return paint;
}
}

namespace
{
// Places or thins over an area of the target's space: [low, high] bounds
// it, outlineAt(x, z) is how much the stroke covers each point (0..1).
int PlaceOverArea(Engine::Scene::Scene& scene, TerrainManager* manager, Terrain* terrain,
    const Engine::Brushes::BrushPreset& brush, const std::string& layerName, float strength, bool erase,
    glm::dvec2 low, glm::dvec2 high, const std::function<float(double, double)>& outlineAt, uint32_t& random)
{
    const auto& place = brush.terrain.placement;
    auto* root = manager ? manager->Owner : terrain ? terrain->Owner : nullptr;
    if (!root || !(strength > 0.f) || scene.HasStarted() || !(high.x > low.x) || !(high.y > low.y)) return 0;
    if (!erase && (place.assets.empty() || !(place.density > 0.f))) return 0;
    const auto field = manager ? manager->GetTerrainField() : nullptr;
    const glm::dmat4 rootWorld = root->transform.GetWorldMatrixD();
    // The cells the dab (and the spacing around it) can touch.
    std::vector<Terrain*> cells;
    if (manager)
    {
        const double spanX = double(manager->cellsX) * manager->cellWidth;
        const double spanZ = double(manager->cellsY) * manager->cellDepth;
        const double margin = place.minSpacing;
        for (int cz = int(std::floor((low.y - margin) / spanZ)); cz <= int(std::floor((high.y + margin) / spanZ)); ++cz)
            for (int cx = int(std::floor((low.x - margin) / spanX)); cx <= int(std::floor((high.x + margin) / spanX)); ++cx)
                if (auto* cell = manager->GetLoadedCell(cx, cz)) cells.push_back(cell);
    }
    else if (terrain) cells.push_back(terrain);
    // The rules' layer materials as each cell's layer indices.
    std::map<const Terrain*, Engine::Brushes::BrushLayerSet> layerSets;
    // How much the brush acts at a point of the target's space: its outline
    // there, times its rules on the ground there. Fills height and normal.
    const auto actsAt = [&](double x, double z, const Spot& spot, float& height, glm::vec3& normal) -> float
    {
        const float shaped = outlineAt(x, z);
        if (shaped <= 0.f || !spot.cell) return 0.f;
        const double lx = x - spot.cellOrigin.x, lz = z - spot.cellOrigin.y;
        bool found = false;
        if (field && field->HasEdits(field->CellAt(float(x), float(z))))
            if (const auto top = field->SurfaceAt(float(x), float(z)))
            { height = top->height; normal = top->normal; found = true; }
        // Cells sit unrotated at their origin under the manager, so their
        // heights and normals are the target's.
        if (!found && !SampleTerrainSurface(spot.cell->heights, spot.cell->cellsX, spot.cell->cellsY,
                spot.cell->cellWidth, spot.cell->cellDepth, float(lx), float(lz), height, &normal))
            return 0.f;
        const float slope = std::acos(std::clamp(normal.y, -1.f, 1.f)) * 57.2957795f;
        const auto paint = PaintAt(*spot.cell, lx, lz);
        auto resolved = layerSets.find(spot.cell);
        if (resolved == layerSets.end())
            resolved = layerSets.emplace(spot.cell, spot.cell->ResolveRuleLayers(brush.terrain.rules)).first;
        const float trail = manager && brush.terrain.rules.trailClearance > 0.f
            ? manager->ContourPathDistance(float(x), float(z), height)
            : std::numeric_limits<float>::infinity();
        return shaped * Engine::Brushes::BrushRulesWeight(brush.terrain.rules, height, slope,
            &resolved->second, paint.empty() ? nullptr : paint.data(), paint.size(), trail);
    };
    const auto rootPosition = [&](const Terrain& cell, const TerrainObjectLayer& layer,
        const TerrainObjectLayer::Placement& placement) {
        const glm::dmat4 toRoot = glm::inverse(rootWorld) * layer.Owner->transform.GetWorldMatrixD();
        return glm::dvec3(toRoot * glm::dvec4(glm::dvec3(placement.position), 1.0));
    };
    int changed = 0;
    // The macro colour map recomposes the cells that changed (canopy).
    std::set<std::pair<int, int>> touched;
    const auto touch = [&](double x, double z) {
        if (!manager) return;
        const double spanX = double(manager->cellsX) * manager->cellWidth;
        const double spanZ = double(manager->cellsY) * manager->cellDepth;
        touched.insert({int(std::floor(x / spanX)), int(std::floor(z / spanZ))});
    };
    const auto finish = [&](int count) {
        if (manager) for (const auto& cell : touched) manager->MarkMacroCellsDirty(cell);
        return count;
    };
    if (erase)
    {
        for (auto* cell : cells)
        {
            auto* layer = FindLayer(*cell, layerName);
            if (!layer || !layer->Owner) continue;
            const auto before = layer->placements.size();
            layer->placements.erase(std::remove_if(layer->placements.begin(), layer->placements.end(),
                [&](const TerrainObjectLayer::Placement& placement)
                {
                    if (!place.assets.empty() && std::none_of(place.assets.begin(), place.assets.end(),
                            [&](const auto& asset) { return asset.asset == placement.asset; })) return false;
                    const glm::dvec3 p = rootPosition(*cell, *layer, placement);
                    float height = 0.f;
                    glm::vec3 normal(0.f, 1.f, 0.f);
                    const float acts = actsAt(p.x, p.z, CellAt(manager, terrain, p.x, p.z), height, normal);
                    const bool removed = acts > 0.f && NextRandom(random) < acts * std::min(strength, 1.f);
                    if (removed) touch(p.x, p.z);
                    return removed;
                }), layer->placements.end());
            if (layer->placements.size() != before)
            {
                changed += int(before - layer->placements.size());
                layer->MarkConfigurationDirty();
            }
        }
        return finish(changed);
    }
    // Spacing: every placement already in the target layer near the dab.
    std::vector<glm::dvec2> taken;
    const double spacing2 = double(place.minSpacing) * place.minSpacing;
    for (auto* cell : cells)
        if (auto* layer = FindLayer(*cell, layerName); layer && layer->Owner)
            for (const auto& placement : layer->placements)
            {
                const glm::dvec3 p = rootPosition(*cell, *layer, placement);
                if (p.x >= low.x - place.minSpacing && p.x <= high.x + place.minSpacing &&
                    p.z >= low.y - place.minSpacing && p.z <= high.y + place.minSpacing)
                    taken.emplace_back(p.x, p.z);
            }
    float anyWeight = 0.f;
    for (const auto& asset : place.assets) anyWeight += std::max(asset.weight, 0.f);
    if (!(anyWeight > 0.f)) return 0;
    // Candidates over the bounds, kept by the outline and rules, so the
    // density holds over the stroke's own shape.
    const double expected = double(place.density) * (high.x - low.x) * (high.y - low.y) *
        std::clamp(strength, 0.f, 1.f);
    int candidates = int(std::floor(expected));
    if (NextRandom(random) < expected - candidates) ++candidates;
    candidates = std::min(candidates, 2000000);
    for (int n = 0; n < candidates; ++n)
    {
        const double x = low.x + NextRandom(random) * (high.x - low.x);
        const double z = low.y + NextRandom(random) * (high.y - low.y);
        const Spot spot = CellAt(manager, terrain, x, z);
        float height = 0.f;
        glm::vec3 normal(0.f, 1.f, 0.f);
        const float acts = actsAt(x, z, spot, height, normal);
        if (acts <= 0.f || NextRandom(random) >= acts) continue;
        if (std::any_of(taken.begin(), taken.end(), [&](const glm::dvec2& other) {
                const glm::dvec2 d = other - glm::dvec2(x, z); return glm::dot(d, d) < spacing2; }))
            continue;
        auto* layer = TargetLayer(scene, *spot.cell, layerName, manager);
        if (!layer || !layer->Owner) continue;
        // The asset, by its weight at this height (species by altitude);
        // none suits the height: nothing here.
        float totalWeight = 0.f;
        for (const auto& candidate : place.assets) totalWeight += std::max(candidate.WeightAt(height), 0.f);
        if (!(totalWeight > 0.f)) continue;
        float pick = NextRandom(random) * totalWeight;
        const Engine::Brushes::BrushPlacementAsset* asset = nullptr;
        for (const auto& candidate : place.assets)
            if (const float w = std::max(candidate.WeightAt(height), 0.f); w > 0.f)
            {
                asset = &candidate;
                if ((pick -= w) < 0.f) break;
            }
        if (!asset) continue;
        const glm::dmat4 toLayer = glm::inverse(layer->Owner->transform.GetWorldMatrixD()) * rootWorld;
        TerrainObjectLayer::Placement placement;
        placement.asset = asset->asset;
        placement.position = glm::vec3(toLayer * glm::dvec4(x, double(height) - place.sink, z, 1.0));
        placement.normal = place.alignToSurface
            ? glm::normalize(glm::vec3(glm::dmat3(toLayer) * glm::dvec3(normal))) : glm::vec3(0.f, 1.f, 0.f);
        placement.yawDegrees = place.randomYaw ? NextRandom(random) * 360.f - 180.f : 0.f;
        placement.scale = asset->minScale + (asset->maxScale - asset->minScale) * NextRandom(random);
        layer->placements.push_back(placement);
        layer->MarkConfigurationDirty();
        taken.emplace_back(x, z);
        touch(x, z);
        ++changed;
    }
    return finish(changed);
}
}

int ApplyPlacementBrush(Engine::Scene::Scene& scene, TerrainManager* manager, Terrain* terrain,
    const Engine::Brushes::BrushPreset& brush, const TerrainPlacementDab& dab, uint32_t& random)
{
    if (!(dab.radius > 0.f)) return 0;
    Terrain::BrushOptions outline;
    outline.shape = dab.shape;
    outline.shapeYaw = dab.yaw;
    outline.hardness = dab.hardness;
    outline.falloff = dab.falloff;
    const double reach = Terrain::BrushReach(dab.radius, outline);
    return PlaceOverArea(scene, manager, terrain, brush, dab.layer.empty() ? std::string("Objects") : dab.layer,
        dab.strength, dab.erase, {dab.center.x - reach, dab.center.z - reach},
        {dab.center.x + reach, dab.center.z + reach},
        [&](double x, double z) {
            return Terrain::BrushWeight(float(x - dab.center.x), float(z - dab.center.z), dab.radius, outline);
        }, random);
}

int ApplyPlacementRegion(Engine::Scene::Scene& scene, TerrainManager* manager, Terrain* terrain,
    const Engine::Brushes::BrushPreset& brush, const TerrainRegionMask& mask, float strength,
    const std::string& layer, bool erase, uint32_t& random)
{
    glm::dvec2 low, high;
    if (!mask.Bounds(low, high)) return 0;
    return PlaceOverArea(scene, manager, terrain, brush, layer.empty() ? std::string("Objects") : layer,
        strength, erase, low, high, [&](double x, double z) { return mask.Weight(x, z); }, random);
}
}

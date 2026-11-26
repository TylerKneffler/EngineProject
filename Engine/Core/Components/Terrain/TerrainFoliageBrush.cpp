#include "Core/Components/Terrain/TerrainFoliageBrush.h"
#include "Core/Brushes/BrushRules.h"
#include "Core/Components/Terrain/Terrain.h"
#include "Core/Components/Terrain/TerrainManager.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <vector>

namespace Engine::Components
{
namespace
{
// A cell's paint at a point of its own space (bilinear over vertices).
std::vector<float> PaintAt(const Terrain& cell, float x, float z)
{
    const size_t layers = cell.layers.size();
    std::vector<float> paint(layers, 0.f);
    if (!layers || cell.weights.size() != cell.heights.size() * layers) return paint;
    const float fx = std::clamp(x / cell.cellWidth, 0.f, float(cell.cellsX));
    const float fz = std::clamp(z / cell.cellDepth, 0.f, float(cell.cellsY));
    const int col = std::min(int(fx), cell.cellsX - 1), row = std::min(int(fz), cell.cellsY - 1);
    const float tx = fx - col, tz = fz - row;
    for (size_t layer = 0; layer < layers; ++layer)
    {
        const auto at = [&](int c, int r) { return cell.weights[(size_t(r) * (cell.cellsX + 1) + c) * layers + layer]; };
        paint[layer] = (at(col, row) * (1.f - tx) + at(col + 1, row) * tx) * (1.f - tz) +
            (at(col, row + 1) * (1.f - tx) + at(col + 1, row + 1) * tx) * tz;
    }
    return paint;
}
}

bool ApplyFoliageBrush(TerrainManager& manager, const Engine::Brushes::BrushPreset& brush,
    const TerrainFoliageDab& dab)
{
    const auto& foliage = brush.terrain.foliage;
    if (!foliage.Any() || !(dab.radius > 0.f) || !(dab.strength > 0.f)) return false;
    const auto& rules = brush.terrain.rules;
    const float spanX = manager.cellsX * manager.cellWidth, spanZ = manager.cellsY * manager.cellDepth;
    std::map<const Terrain*, Engine::Brushes::BrushLayerSet> layerSets;
    TerrainFoliageStroke stroke;
    stroke.centre = dab.centre;
    stroke.radius = dab.radius;
    stroke.falloff = 1.f - std::clamp(dab.hardness, 0.f, 1.f);
    stroke.shell = foliage.shell;
    if (rules.Any())
        stroke.weight = [&](const Terrain& cell, const glm::vec3& local, const glm::vec3& normal) {
            // The cell's corner in the manager's space, for trails.
            glm::vec2 corner(0.f);
            if (const auto key = manager.CellOf(cell)) corner = {key->first * spanX, key->second * spanZ};
            const float slope = std::acos(std::clamp(normal.y, -1.f, 1.f)) * 57.2957795f;
            const auto paint = PaintAt(cell, local.x, local.z);
            auto resolved = layerSets.find(&cell);
            if (resolved == layerSets.end()) resolved = layerSets.emplace(&cell, cell.ResolveRuleLayers(rules)).first;
            const float trail = rules.trailClearance > 0.f
                ? manager.TrailDistance(corner.x + local.x, corner.y + local.z)
                : std::numeric_limits<float>::infinity();
            return Engine::Brushes::BrushRulesWeight(rules, local.y, slope, &resolved->second,
                paint.empty() ? nullptr : paint.data(), paint.size(), trail);
        };
    using Mode = Engine::Brushes::BrushFoliage::Mode;
    bool changed = false;
    for (const auto& layer : foliage.layers)
    {
        if (layer.layer.empty()) continue;
        const Mode mode = dab.erase ? Mode::Erase : foliage.mode;
        stroke.mode = mode == Mode::Erase ? TerrainFoliageStroke::Mode::Erase
            : mode == Mode::Set ? TerrainFoliageStroke::Mode::Set : TerrainFoliageStroke::Mode::Add;
        // Set paints toward the amount at the dab's strength; Add and Erase
        // take the amount as their share of it.
        stroke.target = layer.amount;
        stroke.strength = std::clamp(dab.strength * (mode == Mode::Set && !dab.erase ? 1.f : layer.amount), 0.f, 1.f);
        changed = manager.PaintFoliage(layer.layer, stroke) || changed;
    }
    return changed;
}
}

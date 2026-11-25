#pragma once
#include "Core/Brushes/BrushPreset.h"
#include <glm/glm.hpp>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace Engine::Scene { class Scene; }
namespace Engine::Components
{
class Terrain;
class TerrainManager;
struct TerrainBrushShape;
struct TerrainRegionMask;

// One dab of a Place brush (a .brush with a placement section): where, how
// big and how hard. Positions are in the target's local space (the
// manager's object, or the terrain's), as Terrain::ApplyBrush has them.
struct TerrainPlacementDab
{
    glm::dvec3 center{0.0};
    float radius = 4.f;
    // 0..1: share of the brush's density placed (or of the placements
    // thinned when erasing) by this dab.
    float strength = 1.f;
    bool erase = false;
    // The object layer placed into (or thinned): the tool's choice. Empty:
    // "Objects".
    std::string layer;
    // The brush's outline (TerrainBrushShape::Load of the brush; null: round
    // with hardness and falloff), turned by yaw (radians about +Y).
    std::shared_ptr<const TerrainBrushShape> shape;
    float yaw = 0.f;
    float hardness = .5f;
    int falloff = 2;
};

// Places the brush's assets under a dab, or thins them (erase): picks by
// weight, at `density` per square metre over the brush's outline times
// strength, kept `minSpacing` from every placement already in the target
// layer (and from each other), only where the brush's rules allow (height
// and slope bands, paint layers) and on loaded ground. Each lands in the
// dab's object layer of the cell under it (created when missing), standing
// up or aligned to the surface, turned at random, sunk by `sink`. Erasing
// removes placements of the brush's assets (any asset, for a brush with
// none) under the outline with probability strength x outline x rules.
// Returns how many were placed or removed; `random` advances.
//
// Edits are the editor's (the scene must not be playing); a script that
// paints terrain the way a user would calls this the same way.
int ApplyPlacementBrush(Engine::Scene::Scene& scene, TerrainManager* manager, Terrain* terrain,
    const Engine::Brushes::BrushPreset& brush, const TerrainPlacementDab& dab, uint32_t& random);

// The same over a region (the Select tool's selection, a feathered shape in
// the target's local X/Z): the brush's density, spacing and rules over the
// whole mask, faded by its feather; erase thins by strength. Placements go
// into `layer` (empty: "Objects").
int ApplyPlacementRegion(Engine::Scene::Scene& scene, TerrainManager* manager, Terrain* terrain,
    const Engine::Brushes::BrushPreset& brush, const TerrainRegionMask& mask, float strength,
    const std::string& layer, bool erase, uint32_t& random);
}

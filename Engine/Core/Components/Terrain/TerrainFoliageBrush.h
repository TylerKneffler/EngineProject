#pragma once
#include "Core/Brushes/BrushPreset.h"
#include <glm/glm.hpp>

namespace Engine::Components
{
class TerrainManager;

// One dab of a Foliage brush (a .brush with a foliage section): where, how
// big and how hard, in the manager's space. The centre is a point on any
// surface (open ground, a cliff face, a cave ceiling): the dab reaches the
// voxels within the radius and the brush's shell of the surface.
struct TerrainFoliageDab
{
    glm::vec3 centre{0.f};
    float radius = 4.f;
    float strength = .5f;   // 0..1, times each layer's amount
    bool erase = false;     // Shift: erase whatever the brush's mode
    float hardness = .5f;   // share of the radius at full strength
};

// Paints the brush's foliage layers under a dab (TerrainManager::
// PaintFoliage), each by its amount, where the brush's rules allow (height
// and slope bands, paint layers, trail clearance). True when a density
// changed. Edits are the editor's; a script painting as a user would calls
// this the same way.
bool ApplyFoliageBrush(TerrainManager& manager, const Engine::Brushes::BrushPreset& brush,
    const TerrainFoliageDab& dab);
}

#pragma once

#include <string>

class GrassField;
namespace Engine::Components { class TerrainObjectLayer; }

// Meadow grass for terrain object layers: the bundled meadow species
// (tools/generate_meadow_grass.py, tools/generate_meadow_accents.py) on a
// GrassField. Scene content, not engine behaviour: scene scripts apply it from
// their TerrainManager::SetObjectLayerSetup hook.
namespace MeadowGrass
{
// Layer name of the far grass ring (simple tufts beyond the detailed grass).
inline constexpr const char* FarLayerName = "Procedural far grass";
// Layer name of the forest floor: litter, broadleaf herbs, seedlings and ferns
// under trees (.md/todo.md Foliage Phase 4). The scene script points it at its
// forest paint layer (GrassField::proceduralLayerMask).
inline constexpr const char* ForestFloorLayerName = "Procedural forest floor";
// "Meadow grass" (authored placements), "Procedural meadow grass", the far
// ring or the forest floor.
bool IsMeadowLayer(const std::string& layerName);

// For a meadow layer: marks it as ground cover and, when a viewer is named,
// attaches the preset GrassField (once) and moves the layer into the
// project's "Grass" detail-budget layer. Returns the layer's GrassField, or
// null for other layers or without a viewer.
GrassField* Apply(Engine::Components::TerrainObjectLayer& layer,
    const std::string& viewerObjectName);
}

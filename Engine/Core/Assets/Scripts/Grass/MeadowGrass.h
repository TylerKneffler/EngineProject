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
// "Meadow grass" (authored placements) or "Procedural meadow grass".
bool IsMeadowLayer(const std::string& layerName);

// For a meadow layer: marks it as ground cover and, when a viewer is named,
// attaches the preset GrassField (once) and moves the layer into the
// project's "Grass" detail-budget layer. Returns the layer's GrassField, or
// null for other layers or without a viewer.
GrassField* Apply(Engine::Components::TerrainObjectLayer& layer,
    const std::string& viewerObjectName);
}

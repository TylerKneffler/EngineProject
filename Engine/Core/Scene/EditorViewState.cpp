#include "pch.h"
#include "Core/Scene/EditorViewState.h"
#include "Core/Scene/Object.h"
#include "Core/Components/Terrain/TerrainPlacementHandle.h"

namespace Engine::Scene
{
void EditorViewState::SelectObject(Object* object)
{
    selectedObject = object;
    selectedPlacementId = 0;
    selectedPlacementLayer = nullptr;
    if (const auto* handle = object
            ? object->GetComponent<Engine::Components::TerrainPlacementHandle>() : nullptr;
        handle && handle->placementId != 0 && object->Parent)
    {
        selectedPlacementId = handle->placementId;
        selectedPlacementLayer = object->Parent;
    }
}
}

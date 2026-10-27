#pragma once
#include "Core/Component.h"
#include "Core/Compoonents/Materials/Material.h"
#include <memory>
#include <vector>

namespace Engine::Components
{
// Runtime-only appearance shared by merged terrain cells. Keeping the same
// material instances avoids reloading and rebinding textures for each cell.
class TerrainBatchAppearance final : public Engine::Core::Component
{
public:
    TerrainBatchAppearance()
    {
        SetTypeName(COMPONENT_TYPE_NAME(TerrainBatchAppearance));
        singlecomponent = true;
    }
    std::vector<std::shared_ptr<Material>> materials;
    float textureTiling = 16.f;
    bool distantVisual = false;
    // True only when every cell of a resident 4x4 batch is present.
    bool completeFootprint = false;
};
}

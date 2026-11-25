#include "Core/Rendering/DetailBudget.h"
#include "TerrainManager.h"
#include "TerrainObjectLayer.h"
#include "Core/Scene/Object.h"
#include "Core/Scene/Scene.h"
#include "Core/Components/Terrain/TerrainBatchAppearance.h"
#include "Core/Components/Camera/Camera.h"
#include "Core/Components/Mesh/Mesh.h"
#include "Core/Components/Materials/Material.h"
#include "Core/Components/Materials/Texture.h"
#include "Core/Components/Physics/MeshObjectCollider.h"
#include "Core/Components/Physics/RigidBody.h"
#include "Core/Inspection/IInspectorUi.h"
#include "Core/Serialization/SceneSerializer.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <type_traits>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <set>
#include <stdexcept>

namespace Engine::Components
{
void TerrainManager::RefreshRenderBatches(int, int, int, int)
{
    // Batches joined distant loaded cells' height-grid meshes into one draw.
    // Cells are volumes now (VolumeTerrain), which have no height mesh to
    // join: any batch left from before goes.
    m_renderBatchRefreshPending = false;
    if (m_renderBatches.empty() || !Owner || !Owner->GetScene()) return;
    for (auto& [group, batch] : m_renderBatches)
        Owner->GetScene()->RemoveObject(batch.object);
    m_renderBatches.clear();
}

}

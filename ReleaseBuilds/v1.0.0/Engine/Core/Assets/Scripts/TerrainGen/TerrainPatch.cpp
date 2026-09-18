#include "Scripts/TerrainGen/TerrainPatch.h"

#include "Core/Serialization/SceneSerializer.h"

TerrainPatch::TerrainPatch()
{
    SetTypeName(COMPONENT_TYPE_NAME(TerrainPatch));
    RegisterField("patchX", patchX);
    RegisterField("patchZ", patchZ);
    RegisterField("triangleCount", triangleCount);
}

namespace
{
struct TerrainPatchRegistration
{
    TerrainPatchRegistration()
    {
        Engine::Serialization::RegisterComponentType<TerrainPatch>(
            "TerrainPatch");
    }
};
TerrainPatchRegistration g_registration;
}

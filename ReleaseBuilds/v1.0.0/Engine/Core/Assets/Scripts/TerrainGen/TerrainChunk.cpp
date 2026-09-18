#include "Scripts/TerrainGen/TerrainChunk.h"

#include "Core/Serialization/SceneSerializer.h"

TerrainChunk::TerrainChunk()
{
    SetTypeName(COMPONENT_TYPE_NAME(TerrainChunk));
    RegisterField("chunkX", chunkX);
    RegisterField("chunkZ", chunkZ);
    RegisterField("patchCount", patchCount);
}

namespace
{
struct TerrainChunkRegistration
{
    TerrainChunkRegistration()
    {
        Engine::Serialization::RegisterComponentType<TerrainChunk>(
            "TerrainChunk");
    }
};
TerrainChunkRegistration g_registration;
}

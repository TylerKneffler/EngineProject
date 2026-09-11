#include "Scripts/GeneralizedBehaviors/MarchingCubesChunk.h"

#include "Core/Serialization/SceneSerializer.h"

MarchingCubesChunk::MarchingCubesChunk()
{
    SetTypeName(COMPONENT_TYPE_NAME(MarchingCubesChunk));
    RegisterField("chunkX", chunkX);
    RegisterField("chunkZ", chunkZ);
    RegisterField("quadCount", quadCount);
}

namespace
{
struct MarchingCubesChunkRegistration
{
    MarchingCubesChunkRegistration()
    {
        Engine::Serialization::RegisterComponentType<MarchingCubesChunk>(
            "MarchingCubesChunk");
    }
};
MarchingCubesChunkRegistration g_registration;
}

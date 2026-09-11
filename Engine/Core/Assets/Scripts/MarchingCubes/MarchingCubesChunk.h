#pragma once

#include "Core/PropertyMacros.h"
#include "Core/Script.h"

// Runtime metadata attached to a streamed terrain chunk. Keeping this as a
// component makes generated hierarchy nodes inspectable and reusable by other
// terrain systems without coupling them to the streaming controller.
class MarchingCubesChunk final : public Engine::Core::Script
{
public:
    MarchingCubesChunk();

    PROPERTY(Inspector, EditAnywhere, Category = "Marching Cubes Chunk")
    int chunkX = 0;

    PROPERTY(Inspector, EditAnywhere, Category = "Marching Cubes Chunk")
    int chunkZ = 0;

    PROPERTY(Inspector, EditAnywhere, Category = "Marching Cubes Chunk", ClampMin = "1")
    int quadCount = 0;
};

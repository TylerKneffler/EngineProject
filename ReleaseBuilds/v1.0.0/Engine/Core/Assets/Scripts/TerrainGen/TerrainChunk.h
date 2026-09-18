#pragma once

#include "Core/PropertyMacros.h"
#include "Core/Script.h"

// Runtime metadata attached to a streamed terrain chunk. Keeping this as a
// component makes generated hierarchy nodes inspectable and reusable by other
// terrain systems without coupling them to the streaming controller.
class TerrainChunk final : public Engine::Core::Script
{
public:
    TerrainChunk();

    PROPERTY(Inspector, EditAnywhere, Category = "Terrain Chunk")
    int chunkX = 0;

    PROPERTY(Inspector, EditAnywhere, Category = "Terrain Chunk")
    int chunkZ = 0;

    PROPERTY(Inspector, EditAnywhere, Category = "Terrain Chunk", ClampMin = "1")
    int patchCount = 0;
};

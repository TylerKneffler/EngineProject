#pragma once

#include "Core/PropertyMacros.h"
#include "Core/Script.h"

// Identifies one independently renderable mesh batch inside a terrain chunk.
class TerrainPatch final : public Engine::Core::Script
{
public:
    TerrainPatch();

    PROPERTY(Inspector, EditAnywhere, Category = "Terrain Patch")
    int patchX = 0;

    PROPERTY(Inspector, EditAnywhere, Category = "Terrain Patch")
    int patchZ = 0;

    PROPERTY(Inspector, EditAnywhere, Category = "Terrain Patch", ClampMin = "0")
    int triangleCount = 0;
};

#pragma once

#include "Core/PropertyMacros.h"
#include "Core/Script.h"

// Identifies one independently renderable horizontal patch of a terrain
// chunk. Quads are mesh batches (not single geometric quadrilaterals).
class MarchingCubesQuad final : public Engine::Core::Script
{
public:
    MarchingCubesQuad();

    PROPERTY(Inspector, EditAnywhere, Category = "Marching Cubes Quad")
    int quadX = 0;

    PROPERTY(Inspector, EditAnywhere, Category = "Marching Cubes Quad")
    int quadZ = 0;

    PROPERTY(Inspector, EditAnywhere, Category = "Marching Cubes Quad", ClampMin = "0")
    int triangleCount = 0;
};

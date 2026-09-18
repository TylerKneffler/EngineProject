#pragma once

#include "Core/PropertyMacros.h"
#include "Core/Script.h"
#include <glm/glm.hpp>

// Deterministic, seedable coherent-noise source shared by procedural scene
// behaviors. Sampling is stateless, so independently loaded chunks agree at
// their borders regardless of creation order.
class PerlinNoiseField final : public Engine::Core::Script
{
public:
    PerlinNoiseField();

    PROPERTY(Inspector, EditAnywhere, Category = "Noise")
    int seed = 1337;

    PROPERTY(Inspector, EditAnywhere, Category = "Noise", ClampMin = "0.000001")
    float frequency = 0.045f;

    PROPERTY(Inspector, EditAnywhere, Category = "Noise", ClampMin = "1", ClampMax = "12")
    int octaves = 4;

    PROPERTY(Inspector, EditAnywhere, Category = "Noise", ClampMin = "1")
    float lacunarity = 2.f;

    PROPERTY(Inspector, EditAnywhere, Category = "Noise", Range = "0, 1")
    float persistence = 0.5f;

    PROPERTY(Inspector, EditAnywhere, Category = "Noise")
    glm::vec3 coordinateOffset { 0.f };

    float Sample(const glm::vec3& worldPosition) const;
    float Sample2D(float worldX, float worldZ) const;
    float SampleFractal(const glm::vec3& worldPosition) const;
    float SampleFractal2D(float worldX, float worldZ) const;
    float SampleFractal(const glm::dvec3& worldPosition) const;
    float SampleFractal2D(double worldX, double worldZ) const;

private:
    float SampleUnit(const glm::vec3& position) const;
    float SampleUnit(const glm::dvec3& position) const;
};

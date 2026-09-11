#include "Scripts/GeneralizedBehaviors/PerlinNoiseField.h"

#include "Core/Serialization/SceneSerializer.h"
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace
{
float Fade(float value)
{
    return value * value * value *
        (value * (value * 6.f - 15.f) + 10.f);
}

float Lerp(float first, float second, float amount)
{
    return first + (second - first) * amount;
}

uint32_t HashCoordinate(int x, int y, int z, int seed)
{
    uint32_t value = static_cast<uint32_t>(x) * 0x8da6b343u;
    value ^= static_cast<uint32_t>(y) * 0xd8163841u;
    value ^= static_cast<uint32_t>(z) * 0xcb1ab31fu;
    value ^= static_cast<uint32_t>(seed) * 0x165667b1u;
    value ^= value >> 16u;
    value *= 0x7feb352du;
    value ^= value >> 15u;
    value *= 0x846ca68bu;
    return value ^ (value >> 16u);
}

float Gradient(uint32_t hash, float x, float y, float z)
{
    static constexpr glm::vec3 directions[] = {
        { 1, 1, 0 }, { -1, 1, 0 }, { 1, -1, 0 }, { -1, -1, 0 },
        { 1, 0, 1 }, { -1, 0, 1 }, { 1, 0, -1 }, { -1, 0, -1 },
        { 0, 1, 1 }, { 0, -1, 1 }, { 0, 1, -1 }, { 0, -1, -1 }
    };
    const glm::vec3& direction = directions[hash % 12u];
    return (direction.x * x + direction.y * y + direction.z * z) * 0.70710678f;
}
}

PerlinNoiseField::PerlinNoiseField()
{
    SetTypeName(COMPONENT_TYPE_NAME(PerlinNoiseField));
    RegisterField("seed", seed);
    RegisterField("frequency", frequency);
    RegisterField("octaves", octaves);
    RegisterField("lacunarity", lacunarity);
    RegisterField("persistence", persistence);
    RegisterField("coordinateOffset", coordinateOffset);
}

namespace
{
struct PerlinNoiseFieldRegistration
{
    PerlinNoiseFieldRegistration()
    {
        Engine::Serialization::RegisterComponentType<PerlinNoiseField>(
            "PerlinNoiseField");
    }
};
PerlinNoiseFieldRegistration g_registration;
}

float PerlinNoiseField::SampleUnit(const glm::vec3& position) const
{
    const int x0 = static_cast<int>(std::floor(position.x));
    const int y0 = static_cast<int>(std::floor(position.y));
    const int z0 = static_cast<int>(std::floor(position.z));
    const float x = position.x - static_cast<float>(x0);
    const float y = position.y - static_cast<float>(y0);
    const float z = position.z - static_cast<float>(z0);
    const float u = Fade(x), v = Fade(y), w = Fade(z);

    const auto corner = [&](int dx, int dy, int dz)
    {
        return Gradient(HashCoordinate(x0 + dx, y0 + dy, z0 + dz, seed),
            x - static_cast<float>(dx), y - static_cast<float>(dy),
            z - static_cast<float>(dz));
    };
    const float lowerFront = Lerp(corner(0, 0, 0), corner(1, 0, 0), u);
    const float upperFront = Lerp(corner(0, 1, 0), corner(1, 1, 0), u);
    const float lowerBack = Lerp(corner(0, 0, 1), corner(1, 0, 1), u);
    const float upperBack = Lerp(corner(0, 1, 1), corner(1, 1, 1), u);
    return Lerp(Lerp(lowerFront, upperFront, v),
        Lerp(lowerBack, upperBack, v), w);
}

float PerlinNoiseField::Sample(const glm::vec3& worldPosition) const
{
    return SampleUnit((worldPosition + coordinateOffset) *
        std::max(0.000001f, frequency));
}

float PerlinNoiseField::Sample2D(float worldX, float worldZ) const
{
    return Sample(glm::vec3(worldX, 0.f, worldZ));
}

float PerlinNoiseField::SampleFractal(const glm::vec3& worldPosition) const
{
    float value = 0.f;
    float weight = 1.f;
    float normalization = 0.f;
    glm::vec3 samplePosition = (worldPosition + coordinateOffset) *
        std::max(0.000001f, frequency);
    const int octaveCount = std::clamp(octaves, 1, 12);
    for (int octave = 0; octave < octaveCount; ++octave)
    {
        value += SampleUnit(samplePosition) * weight;
        normalization += weight;
        samplePosition *= std::max(1.f, lacunarity);
        weight *= std::clamp(persistence, 0.f, 1.f);
    }
    return normalization > 0.f ? value / normalization : 0.f;
}

float PerlinNoiseField::SampleFractal2D(float worldX, float worldZ) const
{
    return SampleFractal(glm::vec3(worldX, 0.f, worldZ));
}

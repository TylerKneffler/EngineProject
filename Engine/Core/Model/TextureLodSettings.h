#pragma once
#include <algorithm>
#include <cmath>
#include <glm/glm.hpp>

namespace Engine::Model
{
struct TextureLodSettings
{
    bool enabled = false;
    float nearDistance = 20.f;
    float farDistance = 80.f;
    float mipBias = 4.f;
    bool simplifyShading = true;

    float alphaMipBias = 0.f;
    bool disableAlphaAtFar = false;

    glm::vec4 AlphaGpuParameters() const
    {
        auto normalized = *this;
        normalized.Normalize();
        return {normalized.alphaMipBias, normalized.disableAlphaAtFar ? 1.f : 0.f, 0.f, 0.f};
    }

    void Normalize()
    {
        nearDistance = std::isfinite(nearDistance) ? std::clamp(nearDistance, 0.f, 999999.f) : 20.f;
        farDistance = std::isfinite(farDistance) ? std::clamp(farDistance, nearDistance + 1.f, 1000000.f) : nearDistance + 60.f;
        alphaMipBias = std::isfinite(alphaMipBias) ? std::clamp(alphaMipBias, 0.f, 8.f) : 0.f;
        mipBias = std::isfinite(mipBias) ? std::clamp(mipBias, 0.f, 8.f) : 4.f;
    }
    glm::vec4 GpuParameters() const
    {
        auto normalized = *this;
        normalized.Normalize();
        return {normalized.nearDistance, normalized.farDistance, normalized.mipBias,
            normalized.enabled ? (normalized.simplifyShading ? 2.f : 1.f) : 0.f};
    }
};
}

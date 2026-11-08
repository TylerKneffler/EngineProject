#pragma once
#include "Core/Model/TextureLodSettings.h"
#include <array>

namespace Engine::Model
{
// Multipliers only reduce authored distance/LOD budgets. Assets remain immutable.
struct GraphicsQualityValues
{
    float geometry = 1.f, vegetation = 1.f, viewDistance = 1.f;
    float texture = 1.f, material = 1.f, shadows = 1.f, renderScale = 1.f;
};
struct GraphicsQualitySettings
{
    // Reference, High, Medium, Low. Reference retains all legacy authored settings.
    int preset = 0;
    // Negative values inherit renderer policy; each channel is independent.
    GraphicsQualityValues overrides{-1.f,-1.f,-1.f,-1.f,-1.f,-1.f,-1.f};
    static constexpr std::array<const char*, 7> Keys{
        "geometry", "vegetation", "viewDistance", "texture", "material", "shadows", "renderScale"};
    static std::array<float*, 7> Channels(GraphicsQualityValues& v)
    { return {&v.geometry,&v.vegetation,&v.viewDistance,&v.texture,&v.material,&v.shadows,&v.renderScale}; }
    GraphicsQualityValues Resolve() const
    {
        GraphicsQualityValues result;
        switch (preset) {
        case 1: result={.85f,.85f,.9f,.85f,1.f,.85f,1.f}; break;
        case 2: result={.65f,.65f,.75f,.65f,.65f,.65f,.75f}; break;
        case 3: result={.45f,.4f,.5f,.4f,.35f,.4f,.5f}; break;
        default: break;
        }
        auto copy=overrides;
        auto input=Channels(copy), output=Channels(result);
        for (size_t i=0;i<input.size();++i)
            if (std::isfinite(*input[i]) && *input[i]>=0.f)
                *output[i]=std::clamp(*input[i], i==6 ? .5f : .05f, 1.f);
        return result;
    }
    TextureLodSettings ResolveTexture(TextureLodSettings authored) const
    {
        const auto effective=Resolve();
        if (effective.texture<1.f || effective.material<1.f) {
            const bool wasEnabled = authored.enabled;
            authored.enabled=true;
            authored.mipBias=std::max(wasEnabled ? authored.mipBias : 0.f, (1.f-effective.texture)*8.f);
            // Texture and material policy must be independently selectable.
            authored.simplifyShading=(wasEnabled && authored.simplifyShading) || effective.material<1.f;
        }
        authored.Normalize();
        return authored;
    }
};
}

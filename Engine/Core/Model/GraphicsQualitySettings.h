#pragma once
#include "Core/Model/TextureLodSettings.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>

namespace Engine::Model
{
// Multipliers only reduce authored distance/LOD budgets. Assets remain immutable.
struct GraphicsQualityValues
{
    float geometry = 1.f, vegetation = 1.f, viewDistance = 1.f;
    float texture = 1.f, material = 1.f, shadows = 1.f, renderScale = 1.f;
};
// Channel values the Auto preset resolves to. The runtime controller
// (Core/Rendering/AutoQuality.h) writes them; render threads read them.
struct AutoQualityChannels
{
    std::array<std::atomic<float>, 7> values;
    AutoQualityChannels() { for (auto& value : values) value.store(1.f, std::memory_order_relaxed); }
    GraphicsQualityValues Load() const
    {
        GraphicsQualityValues result;
        float* channels[] = {&result.geometry,&result.vegetation,&result.viewDistance,
            &result.texture,&result.material,&result.shadows,&result.renderScale};
        for (size_t i = 0; i < values.size(); ++i)
            *channels[i] = values[i].load(std::memory_order_relaxed);
        return result;
    }
};
inline AutoQualityChannels& SharedAutoQualityChannels()
{
    static AutoQualityChannels channels;
    return channels;
}

struct GraphicsQualitySettings
{
    // Reference, High, Medium, Low, Auto. Reference retains all legacy
    // authored settings. Auto adjusts the channels at runtime for the best
    // quality that holds the frame-time target (Core/Rendering/AutoQuality.h);
    // overrides still cap it.
    static constexpr int AutoPreset = 4;
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
        case AutoPreset: result=SharedAutoQualityChannels().Load(); break;
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

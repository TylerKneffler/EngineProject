#pragma once
#include "Core/Brushes/BrushPreset.h"
#include <algorithm>
#include <cctype>
#include <cstddef>
#include <limits>
#include <string>
#include <vector>

namespace Engine::Brushes
{
// A brush's paint-layer rules resolved against one terrain's layers: the
// indices of the layers whose material the rules name.
struct BrushLayerSet
{
    std::vector<int> on, avoid;
    // The rules name layers the ground must have: with none of them on this
    // terrain, the brush acts nowhere on it.
    bool needsOn = false;
};

// Whether a rule entry names a layer material: the same asset path (case and
// slashes aside), or the material's file name with or without extension
// ("grass" and "grass.material" match ".../PBR/grass.material").
inline bool RuleNamesMaterial(const std::string& entry, const std::string& material)
{
    const auto normal = [](std::string text) {
        for (char& c : text) c = c == '\\' ? '/' : char(std::tolower(static_cast<unsigned char>(c)));
        return text;
    };
    const std::string a = normal(entry), b = normal(material);
    if (a.empty() || b.empty()) return false;
    if (a == b) return true;
    if (a.find('/') != std::string::npos) return false;
    const std::string file = b.substr(b.find_last_of('/') + 1);
    return a == file || a == file.substr(0, file.find_last_of('.'));
}

inline BrushLayerSet ResolveBrushLayers(const BrushRules& rules,
    const std::vector<std::string>& layerMaterials)
{
    BrushLayerSet set;
    set.needsOn = !rules.onLayers.empty();
    for (size_t layer = 0; layer < layerMaterials.size(); ++layer)
    {
        const auto named = [&](const std::vector<std::string>& entries) {
            return std::any_of(entries.begin(), entries.end(),
                [&](const std::string& entry) { return RuleNamesMaterial(entry, layerMaterials[layer]); });
        };
        if (named(rules.onLayers)) set.on.push_back(int(layer));
        if (named(rules.avoidLayers)) set.avoid.push_back(int(layer));
    }
    return set;
}

// How much a brush with these rules acts at a point (0..1): inside its height
// and slope bands (fading over heightFade metres and slopeFade degrees past
// their edges), on one of its paint layers, off its avoided ones. Sculpt,
// paint and place strokes all scale by it. layers: the rules resolved for
// this terrain (null: layer rules pass); layerWeights: the point's paint,
// layerCount weights; trailDistance: metres to the nearest trail's centre
// line (infinite where there is none).
inline float BrushRulesWeight(const BrushRules& rules, float height, float slopeDegrees,
    const BrushLayerSet* layers = nullptr, const float* layerWeights = nullptr, size_t layerCount = 0,
    float trailDistance = std::numeric_limits<float>::infinity())
{
    const auto band = [](float value, float low, float high, float fade) {
        if (value < low) return fade > 0.f ? std::clamp(1.f - (low - value) / fade, 0.f, 1.f) : 0.f;
        if (value > high) return fade > 0.f ? std::clamp(1.f - (value - high) / fade, 0.f, 1.f) : 0.f;
        return 1.f;
    };
    float weight = band(height, rules.minHeight, rules.maxHeight, rules.heightFade) *
        band(slopeDegrees, rules.minSlope, rules.maxSlope, rules.slopeFade);
    // Trails: none within the clearance, rising to full over the fade past it.
    if (weight > 0.f && rules.trailClearance > 0.f)
        weight *= rules.trailFade > 0.f
            ? std::clamp((trailDistance - rules.trailClearance) / rules.trailFade, 0.f, 1.f)
            : (trailDistance >= rules.trailClearance ? 1.f : 0.f);
    if (weight <= 0.f || !layers) return weight;
    const auto at = [&](int layer) {
        return layerWeights && layer >= 0 && size_t(layer) < layerCount ? layerWeights[layer] : 0.f;
    };
    if (layers->needsOn)
    {
        float on = 0.f;
        for (int layer : layers->on) on += at(layer);
        const float threshold = std::max(rules.layerThreshold, 1.e-3f);
        weight *= std::clamp(on / threshold, 0.f, 1.f);
        if (rules.maxLayerShare < 1.f)
            weight *= std::clamp((rules.maxLayerShare + .1f - on) / .1f, 0.f, 1.f);
    }
    for (int layer : layers->avoid) weight *= 1.f - std::clamp(at(layer), 0.f, 1.f);
    return weight;
}
}

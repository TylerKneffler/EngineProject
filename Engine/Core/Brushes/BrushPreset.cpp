#include "Core/Brushes/BrushPreset.h"
#include <pugixml.hpp>
#include <algorithm>
#include <cmath>
#include <sstream>

namespace Engine::Brushes
{
namespace
{
bool Valid(const BrushPreset& p)
{
    const auto unit = [](float value)
    { return std::isfinite(value) && value >= 0.f && value <= 1.f; };
    return !p.name.empty() && p.shape >= 0 && p.shape <= 1 &&
        p.falloff >= 0 && p.falloff <= 2 &&
        std::isfinite(p.size) && p.size >= 2.f && p.size <= 250.f &&
        unit(p.hardness) && unit(p.strength) && unit(p.opacity) &&
        unit(p.flow) && unit(p.spacing) && unit(p.scatter) &&
        unit(p.pressureSize) && unit(p.pressureStrength) &&
        std::isfinite(p.rotation) && p.rotation >= -180.f &&
        p.rotation <= 180.f;
}

bool ValidTerrain(const BrushPreset& p)
{
    const auto finite = [](float value) { return std::isfinite(value); };
    const auto& r = p.relief;
    const auto& rules = p.terrain.rules;
    const auto& place = p.terrain.placement;
    if (!finite(r.heightRatio) || r.heightRatio < 0.f || r.heightRatio > 100.f ||
        !finite(r.facingX) || !finite(r.facingY)) return false;
    if (!(rules.minHeight <= rules.maxHeight) || !finite(rules.heightFade) || rules.heightFade < 0.f ||
        !finite(rules.minSlope) || !finite(rules.maxSlope) || rules.minSlope < 0.f ||
        rules.maxSlope > 90.f || rules.minSlope > rules.maxSlope || !finite(rules.slopeFade) ||
        rules.slopeFade < 0.f || !finite(rules.layerThreshold) || rules.layerThreshold < 0.f ||
        rules.layerThreshold > 1.f || !finite(rules.maxLayerShare) || rules.maxLayerShare < 0.f ||
        rules.maxLayerShare > 1.f || !finite(rules.trailClearance) || rules.trailClearance < 0.f ||
        !finite(rules.trailFade) || rules.trailFade < 0.f) return false;
    for (const auto& layer : rules.onLayers) if (layer.empty() || layer.find(';') != std::string::npos) return false;
    for (const auto& layer : rules.avoidLayers) if (layer.empty() || layer.find(';') != std::string::npos) return false;
    if (!finite(place.density) || place.density < 0.f || place.density > 100.f ||
        !finite(place.minSpacing) || place.minSpacing < 0.f || place.minSpacing > 1000.f ||
        !finite(place.sink) || std::abs(place.sink) > 100.f) return false;
    for (const auto& asset : place.assets)
        if (asset.asset.empty() || !finite(asset.weight) || asset.weight < 0.f ||
            !finite(asset.minScale) || !finite(asset.maxScale) || asset.minScale <= 0.f ||
            asset.maxScale < asset.minScale || !(asset.minHeight <= asset.maxHeight) ||
            !finite(asset.heightFade) || asset.heightFade < 0.f) return false;
    return int(p.terrain.tool) >= 0 && int(p.terrain.tool) <= 3;
}

// Layer materials, ';' separated (paths may hold spaces).
std::string JoinLayers(const std::vector<std::string>& layers)
{
    std::string text;
    for (const auto& layer : layers) text += (text.empty() ? "" : ";") + layer;
    return text;
}

std::vector<std::string> SplitLayers(const char* text)
{
    std::vector<std::string> layers;
    std::istringstream stream(text ? text : "");
    for (std::string item; std::getline(stream, item, ';');)
    {
        const auto first = item.find_first_not_of(" \t"), last = item.find_last_not_of(" \t");
        if (first != std::string::npos) layers.push_back(item.substr(first, last - first + 1));
    }
    return layers;
}

const char* ToolName(BrushTool tool)
{
    switch (tool)
    {
    case BrushTool::Sculpt: return "sculpt";
    case BrushTool::Paint: return "paint";
    case BrushTool::Place: return "place";
    default: return "any";
    }
}

BrushTool ToolFromName(const std::string& name, bool& known)
{
    known = true;
    if (name == "sculpt") return BrushTool::Sculpt;
    if (name == "paint") return BrushTool::Paint;
    if (name == "place") return BrushTool::Place;
    known = name == "any" || name.empty();
    return BrushTool::Any;
}
}

bool SaveBrushPreset(const std::filesystem::path& path,
    const BrushPreset& preset, std::string& error)
{
    if (!Valid(preset) || !ValidTerrain(preset))
    { error = "Brush settings are outside their supported ranges."; return false; }
    pugi::xml_document document;
    auto root = document.append_child("BrushPreset");
    const bool extended = preset.relief.Present() || !preset.relief.mapPath.empty() ||
        preset.terrain.Any();
    root.append_attribute("version") = extended ? 2 : 1;
    root.append_attribute("name") = preset.name.c_str();
    root.append_attribute("tip") = preset.tipPath.c_str();
    root.append_attribute("shape") = preset.shape;
    root.append_attribute("falloff") = preset.falloff;
    root.append_attribute("size") = preset.size;
    root.append_attribute("hardness") = preset.hardness;
    root.append_attribute("strength") = preset.strength;
    root.append_attribute("opacity") = preset.opacity;
    root.append_attribute("flow") = preset.flow;
    root.append_attribute("spacing") = preset.spacing;
    root.append_attribute("rotation") = preset.rotation;
    root.append_attribute("scatter") = preset.scatter;
    root.append_attribute("pressureSize") = preset.pressureSize;
    root.append_attribute("pressureStrength") = preset.pressureStrength;
    if (!preset.relief.mapPath.empty())
    {
        auto relief = root.append_child("Relief");
        relief.append_attribute("map") = preset.relief.mapPath.c_str();
        relief.append_attribute("heightRatio") = preset.relief.heightRatio;
        relief.append_attribute("facingX") = preset.relief.facingX;
        relief.append_attribute("facingY") = preset.relief.facingY;
        relief.append_attribute("backed") = preset.relief.backed;
    }
    if (preset.terrain.Any())
    {
        const auto& t = preset.terrain;
        auto terrain = root.append_child("Terrain");
        terrain.append_attribute("tool") = ToolName(t.tool);
        if (t.rules.Any())
        {
            auto rules = terrain.append_child("Rules");
            if (t.rules.minHeight > -std::numeric_limits<float>::max())
                rules.append_attribute("minHeight") = t.rules.minHeight;
            if (t.rules.maxHeight < std::numeric_limits<float>::max())
                rules.append_attribute("maxHeight") = t.rules.maxHeight;
            rules.append_attribute("heightFade") = t.rules.heightFade;
            rules.append_attribute("minSlope") = t.rules.minSlope;
            rules.append_attribute("maxSlope") = t.rules.maxSlope;
            rules.append_attribute("slopeFade") = t.rules.slopeFade;
            rules.append_attribute("onLayers") = JoinLayers(t.rules.onLayers).c_str();
            rules.append_attribute("avoidLayers") = JoinLayers(t.rules.avoidLayers).c_str();
            rules.append_attribute("layerThreshold") = t.rules.layerThreshold;
            rules.append_attribute("maxLayerShare") = t.rules.maxLayerShare;
            rules.append_attribute("trailClearance") = t.rules.trailClearance;
            rules.append_attribute("trailFade") = t.rules.trailFade;
        }
        if (t.placement.Any())
        {
            auto place = terrain.append_child("Placement");
            place.append_attribute("density") = t.placement.density;
            place.append_attribute("minSpacing") = t.placement.minSpacing;
            place.append_attribute("alignToSurface") = t.placement.alignToSurface;
            place.append_attribute("randomYaw") = t.placement.randomYaw;
            place.append_attribute("sink") = t.placement.sink;
            for (const auto& asset : t.placement.assets)
            {
                auto item = place.append_child("Asset");
                item.append_attribute("path") = asset.asset.c_str();
                item.append_attribute("weight") = asset.weight;
                item.append_attribute("minScale") = asset.minScale;
                item.append_attribute("maxScale") = asset.maxScale;
                if (asset.minHeight > -std::numeric_limits<float>::max())
                    item.append_attribute("minHeight") = asset.minHeight;
                if (asset.maxHeight < std::numeric_limits<float>::max())
                    item.append_attribute("maxHeight") = asset.maxHeight;
                item.append_attribute("heightFade") = asset.heightFade;
            }
        }
    }
    if (!document.save_file(path.c_str()))
    { error = "Could not write brush preset: " + path.string(); return false; }
    error.clear();
    return true;
}

bool LoadBrushPreset(const std::filesystem::path& path,
    BrushPreset& preset, std::string& error)
{
    pugi::xml_document document;
    const auto result = document.load_file(path.c_str());
    if (!result)
    { error = "Could not read brush preset: " + path.string(); return false; }
    const auto root = document.child("BrushPreset");
    const int version = root ? root.attribute("version").as_int() : 0;
    if (!root || (version != 1 && version != 2))
    { error = "Unsupported brush preset version."; return false; }
    // Reject missing values rather than silently replacing them with defaults.
    constexpr const char* required[] = { "name", "tip", "shape", "falloff",
        "size", "hardness", "strength", "opacity", "flow", "spacing",
        "rotation", "scatter", "pressureSize", "pressureStrength" };
    for (const char* key : required)
        if (!root.attribute(key))
        { error = std::string("Brush preset is missing ") + key + "."; return false; }
    for (const auto attribute : root.attributes())
    {
        const std::string key = attribute.name();
        if (key == "version") continue;
        if (std::find_if(std::begin(required), std::end(required),
            [&](const char* known) { return key == known; }) ==
            std::end(required))
        {
            error = "Unsupported brush setting: " + key;
            return false;
        }
    }
    BrushPreset loaded;
    loaded.name = root.attribute("name").as_string();
    loaded.tipPath = root.attribute("tip").as_string();
    loaded.shape = root.attribute("shape").as_int(-1);
    loaded.falloff = root.attribute("falloff").as_int(-1);
    loaded.size = root.attribute("size").as_float(-1.f);
    loaded.hardness = root.attribute("hardness").as_float(-1.f);
    loaded.strength = root.attribute("strength").as_float(-1.f);
    loaded.opacity = root.attribute("opacity").as_float(-1.f);
    loaded.flow = root.attribute("flow").as_float(-1.f);
    loaded.spacing = root.attribute("spacing").as_float(-1.f);
    loaded.rotation = root.attribute("rotation").as_float(-1000.f);
    loaded.scatter = root.attribute("scatter").as_float(-1.f);
    loaded.pressureSize = root.attribute("pressureSize").as_float(-1.f);
    loaded.pressureStrength = root.attribute("pressureStrength").as_float(-1.f);
    if (!Valid(loaded))
    { error = "Brush preset contains invalid settings."; return false; }
    for (const auto child : root.children())
    {
        const std::string tag = child.name();
        if (version < 2 && child.type() == pugi::node_element)
        { error = "Brush preset version 1 has no " + tag + " section."; return false; }
        if (tag == "Relief")
        {
            auto& r = loaded.relief;
            r.mapPath = child.attribute("map").as_string();
            r.heightRatio = child.attribute("heightRatio").as_float(0.f);
            r.facingX = child.attribute("facingX").as_float(0.f);
            r.facingY = child.attribute("facingY").as_float(1.f);
            r.backed = child.attribute("backed").as_bool(false);
        }
        else if (tag == "Terrain")
        {
            auto& t = loaded.terrain;
            bool known = true;
            t.tool = ToolFromName(child.attribute("tool").as_string("any"), known);
            if (!known) { error = "Unknown terrain brush tool."; return false; }
            if (const auto rules = child.child("Rules"))
            {
                auto& r = t.rules;
                if (rules.attribute("minHeight")) r.minHeight = rules.attribute("minHeight").as_float();
                if (rules.attribute("maxHeight")) r.maxHeight = rules.attribute("maxHeight").as_float();
                r.heightFade = rules.attribute("heightFade").as_float(r.heightFade);
                r.minSlope = rules.attribute("minSlope").as_float(r.minSlope);
                r.maxSlope = rules.attribute("maxSlope").as_float(r.maxSlope);
                r.slopeFade = rules.attribute("slopeFade").as_float(r.slopeFade);
                r.onLayers = SplitLayers(rules.attribute("onLayers").as_string());
                r.avoidLayers = SplitLayers(rules.attribute("avoidLayers").as_string());
                r.layerThreshold = rules.attribute("layerThreshold").as_float(r.layerThreshold);
                r.maxLayerShare = rules.attribute("maxLayerShare").as_float(r.maxLayerShare);
                r.trailClearance = rules.attribute("trailClearance").as_float(r.trailClearance);
                r.trailFade = rules.attribute("trailFade").as_float(r.trailFade);
            }
            if (const auto place = child.child("Placement"))
            {
                auto& p = t.placement;
                p.density = place.attribute("density").as_float(p.density);
                p.minSpacing = place.attribute("minSpacing").as_float(p.minSpacing);
                p.alignToSurface = place.attribute("alignToSurface").as_bool(false);
                p.randomYaw = place.attribute("randomYaw").as_bool(true);
                p.sink = place.attribute("sink").as_float(0.f);
                for (const auto item : place.children("Asset"))
                {
                    BrushPlacementAsset asset;
                    asset.asset = item.attribute("path").as_string();
                    asset.weight = item.attribute("weight").as_float(1.f);
                    asset.minScale = item.attribute("minScale").as_float(1.f);
                    asset.maxScale = item.attribute("maxScale").as_float(asset.minScale);
                    if (item.attribute("minHeight")) asset.minHeight = item.attribute("minHeight").as_float();
                    if (item.attribute("maxHeight")) asset.maxHeight = item.attribute("maxHeight").as_float();
                    asset.heightFade = item.attribute("heightFade").as_float(asset.heightFade);
                    p.assets.push_back(std::move(asset));
                }
            }
        }
        else if (child.type() == pugi::node_element)
        { error = "Unsupported brush section: " + tag; return false; }
    }
    if (!ValidTerrain(loaded))
    { error = "Brush preset contains invalid terrain settings."; return false; }
    preset = std::move(loaded);
    error.clear();
    return true;
}
}

#include "BrushPreset.h"
#include <pugixml.hpp>
#include <cmath>
#include <algorithm>

namespace Engine::Editor::Paint
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
}

bool SaveBrushPreset(const std::filesystem::path& path,
    const BrushPreset& preset, std::string& error)
{
    if (!Valid(preset))
    { error = "Brush settings are outside their supported ranges."; return false; }
    pugi::xml_document document;
    auto root = document.append_child("BrushPreset");
    root.append_attribute("version") = 1;
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
    if (!root || root.attribute("version").as_int() != 1)
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
    preset = std::move(loaded);
    error.clear();
    return true;
}
}

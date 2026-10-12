#include "Editor/Core/Paint/BrushPreset.h"
#include <pugixml.hpp>
#include <cassert>
#include <filesystem>
#include <fstream>

int main()
{
    namespace fs = std::filesystem;
    using namespace Engine::Editor::Paint;
    const fs::path path = fs::temp_directory_path() / "engine-brush-preset-test.brush";
    BrushPreset original;
    original.name = "Soft square";
    original.tipPath = "Assets/Brushes/Tips/sample.png";
    original.shape = 1;
    original.size = 73.f;
    original.opacity = .7f;
    original.flow = .8f;
    original.spacing = .21f;
    original.rotation = 35.f;
    original.scatter = .15f;
    original.pressureStrength = .25f;
    std::string error;
    assert(SaveBrushPreset(path, original, error));
    BrushPreset loaded;
    assert(LoadBrushPreset(path, loaded, error));
    assert(loaded.name == original.name);
    assert(loaded.tipPath == original.tipPath);
    assert(loaded.shape == original.shape);
    assert(loaded.size == original.size);
    assert(loaded.opacity == original.opacity);
    assert(loaded.flow == original.flow);
    assert(loaded.spacing == original.spacing);
    assert(loaded.rotation == original.rotation);
    assert(loaded.scatter == original.scatter);
    assert(loaded.pressureStrength == original.pressureStrength);
    pugi::xml_document edited;
    assert(edited.load_file(path.c_str()));
    edited.child("BrushPreset").append_attribute("unsupportedDynamics") = 1;
    assert(edited.save_file(path.c_str()));
    assert(!LoadBrushPreset(path, loaded, error));
    assert(error.find("unsupportedDynamics") != std::string::npos);
    assert(loaded.name == original.name);
    // A malformed import must leave the current brush intact.
    std::ofstream(path, std::ios::trunc) <<
        "<BrushPreset version=\"2\" name=\"Unknown\"/>";
    assert(!LoadBrushPreset(path, loaded, error));
    assert(loaded.name == original.name);
    fs::remove(path);
}

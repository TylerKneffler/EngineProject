#include "Core/Compoonents/Materials/Material.h"
#include "Core/Compoonents/Obj/TerrainSurfaceWinding.h"
#include "Core/Scene/Settings/SceneSettingsSerialization.h"
#include <filesystem>
#include <iostream>
#include <fstream>
#include <limits>
#include <stdexcept>

static void Require(bool value, const char* message)
{
    if (!value) throw std::runtime_error(message);
}
int main()
{
    try
    {
        Engine::Model::SceneSettings settings, loaded;
        Require(settings.textureLod.GpuParameters().w == 0.f, "legacy scene changed quality");
        settings.textureLod = {true, 12.f, 48.f, 5.f, true};
        Engine::Scene::DeserializeSceneSettings(loaded, Engine::Scene::SerializeSceneSettings(settings));
        Require(loaded.textureLod.GpuParameters() == glm::vec4(12, 48, 5, 2), "scene settings lost on save");
        Engine::Scene::DeserializeSceneSettings(loaded, Engine::Serialization::JsonValue::MakeObject());
        Require(!loaded.textureLod.enabled, "legacy scene inherited previous quality settings");
        Engine::Model::TextureLodSettings invalid{true, -2.f, -10.f,
            std::numeric_limits<float>::quiet_NaN(), true};
        const auto gpu = invalid.GpuParameters();
        Require(gpu.x == 0.f && gpu.y > gpu.x && std::isfinite(gpu.z), "invalid shader interval");
        Engine::Components::Material source, destination;
        source.overrideTextureLod = true;
        source.textureLod = {true, 4.f, 24.f, 6.f, false};
        source.textureLod.alphaMipBias = 3.f;
        source.textureLod.disableAlphaAtFar = true;
        destination.Deserialize(source.Serialize());
        Require(destination.overrideTextureLod && destination.textureLod.GpuParameters() == glm::vec4(4, 24, 6, 1),
            "inline material override lost on save");
        Require(destination.textureLod.AlphaGpuParameters() == glm::vec4(3, 1, 0, 0), "inline alpha LOD lost");
        const std::filesystem::path path = ".local/tests-generated/texture-lod.material";
        std::filesystem::create_directories(path.parent_path());
        Require(source.SaveToFile(path.string()), "material asset save failed");
        Engine::Components::Material asset;
        Require(asset.LoadFromFile(path.string()), "material asset load failed");
        Require(asset.overrideTextureLod && asset.textureLod.GpuParameters() == glm::vec4(4, 24, 6, 1),
            "material asset override lost");
        Require(asset.textureLod.AlphaGpuParameters() == glm::vec4(3, 1, 0, 0), "asset alpha LOD lost");
        settings.textureLod.alphaMipBias = 2.f;
        settings.textureLod.disableAlphaAtFar = true;
        Engine::Scene::DeserializeSceneSettings(loaded, Engine::Scene::SerializeSceneSettings(settings));
        Require(loaded.textureLod.AlphaGpuParameters() == glm::vec4(2, 1, 0, 0), "scene alpha LOD lost");
        invalid.alphaMipBias = std::numeric_limits<float>::quiet_NaN();
        Require(invalid.AlphaGpuParameters().x == 0.f, "invalid alpha bias");
        { std::ofstream legacy(path); legacy << "{\"version\":1}"; }
        Require(asset.LoadFromFile(path.string()) && !asset.overrideTextureLod && !asset.textureLod.enabled,
            "legacy material inherited previous quality override");
        Require(asset.textureLod.AlphaGpuParameters() == glm::vec4(0), "legacy alpha defaults changed");
        std::filesystem::remove(path);
        struct Vertex { float pos[3]; };
        std::vector<Vertex> vertices{{{0, 0, 0}}, {{1, .3f, 0}}, {{0, .5f, 1}}};
        std::vector<uint32_t> indices{0, 1, 2};
        Engine::Components::OrientTerrainSurfaceUp(vertices, indices);
        Require(indices == std::vector<uint32_t>({0, 2, 1}), "legacy downward triangle not corrected");
        Engine::Components::OrientTerrainSurfaceUp(vertices, indices);
        Require(indices == std::vector<uint32_t>({0, 2, 1}), "winding normalization not stable");
        std::cout << "Texture LOD serialization, validation, and terrain winding tests passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

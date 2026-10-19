#include "Editor/Core/Importers/FbxModelDecoder.h"
#include "Editor/Core/Importers/ModelAssetWriter.h"
#include "Core/Scene/Scene.h"
#include "Core/Serialization/SceneSerializer.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

int main()
{
    namespace fs = std::filesystem;
    const fs::path source = fs::path("Engine/Core/Assets/Models/crash-dummy/source") /
        "crush_dummy_UE4_skinned.fbx";
    if (!fs::is_regular_file(source))
    {
        std::cout << "Legacy FBX fixture is not present; skipping\n";
        return 0;
    }
    Engine::Model::ImportedModel model;
    std::string error;
    if (!Engine::Editor::FbxModelDecoder::Decode(source.string(), model, error))
    {
        std::cerr << error << '\n';
        return 1;
    }
    if (model.nodes.size() < 70 || model.primitives.empty() ||
        model.skins.empty() || model.primitives.front().vertices.empty())
    {
        std::cerr << "Legacy FBX lost its mesh or skeleton\n";
        return 1;
    }
    bool hasWeights = false;
    for (const auto& primitive : model.primitives)
        for (const auto& vertex : primitive.vertices)
            hasWeights |= vertex.weights0[0] > 0.f;
    if (!hasWeights || model.materials.empty() ||
        model.materials.front().baseColorTexture.empty())
    {
        std::cerr << "Legacy FBX lost weights or its nearby texture\n";
        return 1;
    }
    const fs::path output = fs::temp_directory_path() / "engine-legacy-fbx-test";
    fs::create_directories(output);
    const auto imported = Engine::Editor::ModelAssetWriter::Write(
        model, source.string(), output.string());
    if (!imported.success || !fs::is_regular_file(imported.prefabPath))
    {
        std::cerr << "Legacy FBX asset writing failed: " << imported.message << '\n';
        return 1;
    }
    std::ifstream material(fs::path(imported.outputDirectory) /
        "Materials/default 1.material");
    const std::string content(std::istreambuf_iterator<char>{material}, {});
    if (content.find("crush_dummy_default_BaseColor.tga.png") == std::string::npos)
    {
        std::cerr << "Generated material lost the resolved texture\n";
        return 1;
    }
    Engine::Scene::Scene scene;
    if (!Engine::Serialization::SceneSerializer::InstantiatePrefab(
            scene, imported.prefabPath, nullptr))
    {
        std::cerr << "Generated legacy prefab could not be instantiated\n";
        return 1;
    }
    std::cout << "Imported " << model.primitives.size() << " meshes and "
        << model.skins.size() << " skins\n";
    return 0;
}

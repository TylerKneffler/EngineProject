#include "ModelAssetWriter.h"

#include "Core/AssetRecord.h"
#include "Core/Compoonents/Material.h"
#include "Core/Compoonents/Animation/ModelAnimation.h"
#include "Core/Object.h"
#include "Core/Scene/Scene.h"
#include "Core/Serialization/SceneSerializer.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <unordered_map>

namespace Engine::Editor
{
namespace fs = std::filesystem;

namespace
{
std::string SafeName(std::string name, const std::string& fallback)
{
    if (name.empty()) name = fallback;
    for (char& c : name)
    {
        const unsigned char value = static_cast<unsigned char>(c);
        if (value < 32 || c == '<' || c == '>' || c == ':' || c == '"' ||
            c == '/' || c == '\\' || c == '|' || c == '?' || c == '*') c = '_';
    }
    while (!name.empty() && (name.back() == ' ' || name.back() == '.')) name.pop_back();
    return name.empty() ? fallback : name;
}

fs::path UniqueDirectory(const fs::path& assets, const std::string& base)
{
    fs::path result = assets / base;
    for (unsigned suffix = 2; fs::exists(result); ++suffix)
        result = assets / (base + " " + std::to_string(suffix));
    return result;
}

glm::vec3 QuaternionEuler(const glm::quat& q)
{
    const float sinX = 2.f * (q.w * q.x + q.y * q.z);
    const float cosX = 1.f - 2.f * (q.x * q.x + q.y * q.y);
    const float sinY = std::clamp(2.f * (q.w * q.y - q.z * q.x), -1.f, 1.f);
    const float sinZ = 2.f * (q.w * q.z + q.x * q.y);
    const float cosZ = 1.f - 2.f * (q.y * q.y + q.z * q.z);
    return { std::atan2(sinX, cosX), std::asin(sinY), std::atan2(sinZ, cosZ) };
}

Engine::Core::Object* AddChild(Engine::Scene::Scene& scene, Engine::Core::Object* parent, const std::string& name)
{
    Engine::Core::Object* child = scene.AddObject(name);
    child->Parent = parent;
    parent->Children.push_back(child);
    return child;
}
}

Engine::Model::ModelImportResult ModelAssetWriter::Write(const Engine::Model::ImportedModel& model,
    const std::string& sourcePath, const std::string& assetsDirectory)
{
    Engine::Model::ModelImportResult result;
    const fs::path source = fs::path(sourcePath).lexically_normal();
    const fs::path assets = fs::path(assetsDirectory).lexically_normal();
    fs::path output;
    try
    {
        const std::string sourceName = SafeName(model.name, SafeName(source.stem().string(), "Model"));
        output = UniqueDirectory(assets, sourceName);
        const fs::path meshesDirectory = output / "Meshes";
        const fs::path materialsDirectory = output / "Materials";
        const fs::path texturesDirectory = output / "Textures";
        fs::create_directories(meshesDirectory);
        fs::create_directories(materialsDirectory);
        fs::create_directories(texturesDirectory);

        std::unordered_map<std::string, std::string> importedTextures;
        auto importTexture = [&](const std::string& value) -> std::string
        {
            if (value.empty()) return {};
            if (const auto found = importedTextures.find(value); found != importedTextures.end())
                return found->second;
            if (value.front() == '*')
            {
                const auto embedded = std::find_if(model.textures.begin(), model.textures.end(),
                    [&](const Engine::Model::ImportedTexture& texture) { return texture.source == value; });
                if (embedded == model.textures.end() || embedded->bytes.empty()) return {};
                const fs::path destination = texturesDirectory /
                    SafeName(embedded->name, "Embedded Texture.bin");
                std::ofstream file(destination, std::ios::binary);
                file.write(reinterpret_cast<const char*>(embedded->bytes.data()),
                    static_cast<std::streamsize>(embedded->bytes.size()));
                if (!file.good()) throw std::runtime_error("Could not write embedded texture");
                Engine::Core::AssetRecord::Ensure(destination, source,
                    { { "importer", std::string("model-texture") },
                      { "sourceFormat", model.sourceFormat } });
                return importedTextures.emplace(value, destination.generic_string()).first->second;
            }
            fs::path original(value);
            if (original.is_relative()) original = source.parent_path() / original;
            if (!fs::exists(original)) return {};
            fs::path destination = texturesDirectory / original.filename();
            for (unsigned suffix = 2; fs::exists(destination); ++suffix)
                destination = texturesDirectory /
                    (original.stem().string() + " " + std::to_string(suffix) + original.extension().string());
            fs::copy_file(original, destination);
            Engine::Core::AssetRecord::Ensure(destination, source,
                { { "importer", std::string("model-texture") },
                  { "sourceFormat", model.sourceFormat } });
            return importedTextures.emplace(value, destination.generic_string()).first->second;
        };

        std::vector<std::string> materialPaths;
        materialPaths.reserve(model.materials.size() + 1);
        for (size_t index = 0; index < model.materials.size(); ++index)
        {
            const Engine::Model::ImportedMaterial& imported = model.materials[index];
            Engine::Components::Material material;
            material.diffuseColor = glm::vec3(imported.baseColor);
            material.baseColorAlpha = imported.baseColor.a;
            material.emissiveColor = imported.emissiveColor;
            material.metallicFactor = imported.metallic;
            material.roughnessFactor = imported.roughness;
            material.alphaCutoff = imported.alphaCutoff;
            material.normalScale = imported.normalScale;
            material.doubleSided = imported.doubleSided;
            material.unlit = imported.unlit;
            material.alphaMode = imported.alphaMode;
            material.baseColorUvSet = imported.baseColorUvSet;
            material.metallicRoughnessUvSet = imported.metallicRoughnessUvSet;
            material.normalUvSet = imported.normalUvSet;
            material.occlusionUvSet = imported.occlusionUvSet;
            material.emissiveUvSet = imported.emissiveUvSet;
            material.SetBaseColorTexture(importTexture(imported.baseColorTexture));
            material.SetMetallicRoughnessTexture(importTexture(imported.metallicRoughnessTexture));
            material.SetNormalTexture(importTexture(imported.normalTexture));
            material.SetOcclusionTexture(importTexture(imported.occlusionTexture));
            material.SetEmissiveTexture(importTexture(imported.emissiveTexture));
            const fs::path path = materialsDirectory /
                (SafeName(imported.name, "Material") + " " + std::to_string(index + 1) + ".material");
            if (!material.SaveToFile(path.string())) throw std::runtime_error("Could not save material");
            Engine::Core::AssetRecord::Ensure(path, source,
                { { "importer", std::string("model-material") },
                  { "sourceFormat", model.sourceFormat },
                  { "materialIndex", static_cast<double>(index) } });
            materialPaths.push_back(path.generic_string());
        }
        Engine::Components::Material defaultMaterial;
        const fs::path defaultMaterialPath = materialsDirectory / "Default.material";
        if (!defaultMaterial.SaveToFile(defaultMaterialPath.string()))
            throw std::runtime_error("Could not save default material");

        std::vector<std::string> meshPaths(model.primitives.size());
        for (size_t index = 0; index < model.primitives.size(); ++index)
        {
            const Engine::Model::ImportedPrimitive& primitive = model.primitives[index];
            if (primitive.vertices.empty()) continue;
            const fs::path path = meshesDirectory /
                (SafeName(primitive.name, "Mesh") + " " + std::to_string(index + 1) + ".mesh");
            if (!Engine::Components::Mesh::SaveNativeFile(path.string(), primitive.vertices))
                throw std::runtime_error("Could not save mesh: " + path.string());
            Engine::Core::AssetRecord::Ensure(path, source,
                { { "importer", std::string("model-mesh") },
                  { "sourceFormat", model.sourceFormat },
                  { "primitiveIndex", static_cast<double>(index) } });
            meshPaths[index] = path.generic_string();
        }

        ::Engine::Scene::Scene scene;
        Engine::Core::Object* root = scene.AddObject(sourceName);
        for (size_t index = 0; index < model.skins.size(); ++index)
        {
            Engine::Components::Skeleton* skeleton =
                root->AddComponent<Engine::Components::Skeleton>();
            skeleton->skinIndex = static_cast<unsigned>(index);
            skeleton->jointNodes = model.skins[index].jointNodes;
            skeleton->inverseBindMatrices = model.skins[index].inverseBindMatrices;
        }
        for (const Engine::Model::ImportedAnimation& imported : model.animations)
        {
            Engine::Components::Animation* animation = root->AddComponent<Engine::Components::Animation>();
            animation->clipName = imported.name;
            animation->duration = imported.duration;
            animation->channels = imported.channels;
        }
        if (!model.animations.empty())
        {
            Engine::Components::AnimationManager* manager = root->AddComponent<Engine::Components::AnimationManager>();
            manager->clip = model.animations.front().name;
        }

        ::Engine::Components::Model* modelComponent =
            root->AddComponent<::Engine::Components::Model>();
        std::vector<Engine::Core::Object*> objects(model.nodes.size(), nullptr);
        for (size_t index = 0; index < model.nodes.size(); ++index)
        {
            const Engine::Model::ImportedNode& imported = model.nodes[index];
            Engine::Core::Object* parent = imported.parent >= 0 && static_cast<size_t>(imported.parent) < objects.size()
                ? objects[static_cast<size_t>(imported.parent)] : root;
            Engine::Core::Object* object = AddChild(scene, parent,
                SafeName(imported.name, "Node " + std::to_string(index + 1)));
            objects[index] = object;
            modelComponent->BindNode(static_cast<unsigned>(index), object);
            object->transform.position = imported.translation;
            object->transform.rotation = QuaternionEuler(imported.rotation);
            object->transform.scale = imported.scale;

            for (size_t slot = 0; slot < imported.primitives.size(); ++slot)
            {
                const unsigned primitiveIndex = imported.primitives[slot];
                if (primitiveIndex >= model.primitives.size() || meshPaths[primitiveIndex].empty()) continue;
                const Engine::Model::ImportedPrimitive& primitive = model.primitives[primitiveIndex];
                Engine::Core::Object* target = imported.primitives.size() == 1 ? object :
                    AddChild(scene, object, SafeName(primitive.name, "Primitive " + std::to_string(slot + 1)));
                Engine::Components::Mesh* mesh = target->AddComponent<Engine::Components::Mesh>();
                mesh->LoadFromFile(meshPaths[primitiveIndex]);
                Engine::Components::Material* material = target->AddComponent<Engine::Components::Material>();
                const std::string materialPath = primitive.materialIndex >= 0 &&
                    static_cast<size_t>(primitive.materialIndex) < materialPaths.size()
                    ? materialPaths[static_cast<size_t>(primitive.materialIndex)]
                    : defaultMaterialPath.generic_string();
                if (!material->LoadFromFile(materialPath))
                    throw std::runtime_error("Could not load generated material");
                if (!primitive.morphTargets.empty())
                    mesh->SetMorphData(static_cast<unsigned>(index),
                        primitive.morphTargets, primitive.morphWeights);
                if (primitive.skinIndex >= 0 || !primitive.morphTargets.empty())
                {
                    Engine::Components::SkinnedMesh* deformer = target->AddComponent<Engine::Components::SkinnedMesh>();
                    deformer->skinIndex = primitive.skinIndex;
                }
            }
        }

        const fs::path prefabPath = output / (output.filename().string() + ".prefab");
        if (!Engine::Serialization::SceneSerializer::SavePrefab(*root, prefabPath.string()))
            throw std::runtime_error("Could not save prefab");
        Engine::Core::AssetRecord::Ensure(prefabPath, source,
            { { "importer", std::string("model") }, { "sourceFormat", model.sourceFormat } });
        result.success = true;
        result.prefabPath = prefabPath.generic_string();
        result.outputDirectory = output.generic_string();
        result.message = "Imported " + source.filename().string();
    }
    catch (const std::exception& error)
    {
        std::error_code cleanupError;
        if (!output.empty() && output.parent_path().lexically_normal() == assets)
            fs::remove_all(output, cleanupError);
        result.message = error.what();
    }
    return result;
}
}

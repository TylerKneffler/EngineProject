#include "Editor/Core/Importers/ModelAssetWriter.h"
#include "Core/Model/ImportedModel.h"
#include "Core/Scene/Scene.h"
#include "Core/Serialization/SceneSerializer.h"
#include "Core/Compoonents/Camera/Camera.h"
#include "Core/Compoonents/Lighting/Light.h"
#include "Core/Compoonents/Animation/AnimationManager.h"
#include "Core/Compoonents/Animation/BoneGroup.h"
#include "Core/Compoonents/Animation/Skeleton.h"
#include "Core/Model/RigAsset.h"
#include "Core/Compoonents/Obj/Mesh.h"
#include "Core/Object.h"
#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace fs = std::filesystem;

int main()
{
    const fs::path assets = fs::current_path() / ".local" / "test-output" / "fbx-scene";
    try
    {
        fs::create_directories(assets);
        Engine::Model::ImportedModel model;
        model.name = "FBX scene regression";
        model.sourceFormat = "fbx";
        model.nodes.push_back({ "SceneRoot" });
        Engine::Model::ImportedNode meshA; meshA.name = "Mesh A"; meshA.parent = 0;
        Engine::Model::ImportedNode meshB; meshB.name = "Mesh B"; meshB.parent = 0;
        model.nodes.push_back(meshA);
        model.nodes.push_back(meshB);
        Engine::Model::ImportedNode faceNode;
        faceNode.name = "FaceNode";
        faceNode.parent = 0;
        faceNode.primitives = { 0, 1 };
        model.nodes.push_back(faceNode);
        for (const std::string& expression : { "Blink", "Smile" })
        {
            Engine::Model::ImportedPrimitive primitive;
            primitive.name = "Face";
            primitive.vertices.resize(3);
            primitive.vertices[0].pos[0] = 0.f;
            primitive.vertices[1].pos[0] = 1.f;
            primitive.vertices[2].pos[1] = 1.f;
            Engine::Model::MorphTarget target;
            target.name = expression;
            target.positions.resize(3, glm::vec3(.1f, 0.f, 0.f));
            primitive.morphTargets.push_back(std::move(target));
            primitive.morphWeights.push_back(0.f);
            model.primitives.push_back(std::move(primitive));
        }
        Engine::Model::ImportedCamera camera;
        camera.nodeIndex = 1;
        camera.position = { 1.f, 2.f, 3.f };
        camera.verticalFov = 42.f;
        model.cameras.push_back(camera);
        Engine::Model::ImportedLight light;
        light.nodeIndex = 2;
        light.type = Engine::Model::ImportedLight::Type::Spot;
        light.position = { 4.f, 5.f, 6.f };
        light.outerConeAngle = 39.f;
        model.lights.push_back(light);
        Engine::Model::ImportedSkin skinA; skinA.jointNodes = { 1 }; skinA.inverseBindMatrices = { glm::mat4(1.f) };
        Engine::Model::ImportedSkin skinB; skinB.jointNodes = { 2 }; skinB.inverseBindMatrices = { glm::mat4(1.f) };
        model.skins = { skinA, skinB };
        Engine::Model::ImportedAnimation clip;
        clip.name = "Move"; clip.duration = 1.f;
        Engine::Model::AnimationChannel channel;
        channel.nodeIndex = 1;
        channel.path = Engine::Model::AnimationChannel::Path::Translation;
        channel.valueWidth = 3;
        channel.times = { 0.f, 1.f };
        channel.values = { 0.f, 0.f, 0.f, 1.f, 0.f, 0.f };
        clip.channels.push_back(channel);
        model.animations.push_back(clip);
        clip.name = "Move Again";
        model.animations.push_back(clip);
        auto result = Engine::Editor::ModelAssetWriter::Write(model,
            (assets / "source.fbx").string(), assets.string());
        if (!result.success) throw std::runtime_error(result.message);
        unsigned animationFiles = 0;
        for (const auto& file : fs::directory_iterator(result.outputDirectory))
            if (file.path().extension() == ".anim") ++animationFiles;
        if (animationFiles != 2) throw std::runtime_error("Expected one asset per clip across both skins");
        Engine::Scene::Scene scene;
        auto* root = Engine::Serialization::SceneSerializer::InstantiatePrefab(scene, result.prefabPath);
        if (!root || root->Children.size() != 1 || root->Children[0]->Children.size() != 3)
            throw std::runtime_error("FBX hierarchy did not survive prefab reload");
        auto* a = root->Children[0]->Children[0];
        auto* b = root->Children[0]->Children[1];
        auto* face = root->Children[0]->Children[2];
        auto* rootSkeleton = root->GetComponent<Engine::Components::Skeleton>();
        auto* groupA = a->GetComponent<Engine::Components::BoneGroup>();
        auto* groupB = b->GetComponent<Engine::Components::BoneGroup>();
        if (!rootSkeleton || rootSkeleton->jointNodes != std::vector<unsigned>{ 1 } ||
            rootSkeleton->rootBoneNodes != std::vector<unsigned>({ 1, 2 }) ||
            !groupA || !groupB || groupA->skinIndex != 0 ||
            groupB->skinIndex != 1 ||
            Engine::Components::BoneGroup::ForSelection(a->Children[0]) != groupA ||
            !groupA->Contains(a->Children[0]) || groupA->Contains(b))
            throw std::runtime_error("Bone groups or root skeleton lost on prefab reload");
        auto* nestedGroup = a->Children[0]
            ->AddComponent<Engine::Components::BoneGroup>();
        if (groupA->Contains(a->Children[0]) ||
            Engine::Components::BoneGroup::ForSelection(a->Children[0]) != nestedGroup)
            throw std::runtime_error("Nested bone groups must stay separate");
        if (a->name != "Mesh A" || b->name != "Mesh B" ||
            a->Children.empty() || b->Children.empty())
            throw std::runtime_error("Separate FBX objects did not survive prefab reload");
        if (face->name != "FaceNode" || face->Children.size() != 2 ||
            face->Children[0]->name != "Face" ||
            face->Children[1]->name != "Face 2")
            throw std::runtime_error("Duplicate facial mesh names lost distinct prefab paths");
        const auto* firstFace = face->Children[0]->GetComponent<Engine::Components::Mesh>();
        const auto* secondFace = face->Children[1]->GetComponent<Engine::Components::Mesh>();
        if (!firstFace || !secondFace ||
            firstFace->GetMorphTargets().size() != 1 ||
            secondFace->GetMorphTargets().size() != 1 ||
            firstFace->GetMorphTargets()[0].name != "Blink" ||
            secondFace->GetMorphTargets()[0].name != "Smile")
            throw std::runtime_error("Facial morph names or mesh owners lost on prefab reload");
        auto* importedCamera = a->Children[0]->GetComponent<Engine::Components::Camera>();
        auto* importedLight = b->Children[0]->GetComponent<Engine::Components::Light>();
        if (!importedCamera || importedCamera->fov != 42.f ||
            !importedLight || importedLight->GetLightType() != Engine::Components::Light::Type::Spot ||
            importedLight->outerConeAngle != 39.f)
            throw std::runtime_error("Camera or light data did not survive prefab reload");
        auto* manager = root->GetComponent<Engine::Components::AnimationManager>();
        if (!manager || manager->rigPaths.size() != 1 ||
            Engine::Model::RigAsset::LoadAll(manager->rigPaths.front()).size() != 2 ||
            manager->clips.size() != 2)
            throw std::runtime_error("Two skins and both animation clips did not survive prefab reload");
        fs::remove_all(result.outputDirectory);
        std::cout << "FBX scene prefab preserves objects, camera, light, two skins and two shared clips\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

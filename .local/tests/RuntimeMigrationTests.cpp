#include "Core/Compoonents/Animation/RigController.h"
#include "Core/Compoonents/Animation/RigConstraints.h"
#include "Core/Compoonents/Animation/RigMorphControls.h"
#include "Core/Compoonents/Physics/RigPhysicsBinding.h"
#include "Core/Compoonents/Animation/AnimationManager.h"
#include "Core/Compoonents/Animation/Model.h"
#include "Core/Compoonents/Materials/Material.h"
#include "Core/Compoonents/Materials/MaterialPropertyMorph.h"
#include "Core/Object.h"
#include "Core/Scene/Scene.h"
#include "Core/Serialization/SceneSerializer.h"
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace
{
void Check(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

void ReplaceAll(std::string& text, const std::string& oldText,
    const std::string& newText)
{
    size_t at = 0;
    while ((at = text.find(oldText, at)) != std::string::npos)
    {
        text.replace(at, oldText.size(), newText);
        at += newText.size();
    }
}
}

int main()
{
    try
    {
        Engine::Scene::Scene scene;
        auto* root = Engine::Serialization::SceneSerializer::InstantiatePrefab(
            scene, PMX_MIGRATION_PREFAB);
        Check(root != nullptr, "Mococo prefab did not load");
        auto* rig = root->GetComponent<Engine::Components::RigConstraints>();
        auto* physics = root->GetComponent<Engine::Components::RigPhysicsBinding>();
        Check(rig && !rig->ikChains.empty() && physics && !physics->physicsJoints.empty(),
            "Mococo prefab lost its rig constraints or joints");
        Check(root->GetComponent<Engine::Components::RigController>() == nullptr,
            "Bundled prefab still stores the monolithic rig controller");
        bool foundMorph = false;
        bool foundSpecular = false;
        for (const auto& object : scene.GetObjects())
        {
            if (auto* morph = object->GetComponent<Engine::Components::MaterialPropertyMorph>())
                foundMorph |= !morph->morphs.empty();
            if (auto* material = object->GetComponent<Engine::Components::Material>())
                foundSpecular |= material->specularModel == "BlinnPhong";
        }
        Check(foundMorph && foundSpecular,
            "Mococo prefab lost material morphs or its specular model");
        auto* animation = root->GetComponent<Engine::Components::AnimationManager>();
        Check(animation != nullptr,
            "Mococo prefab lost its animation manager");
        scene.Start();
        Check(animation->FindClip(animation->clip) != nullptr,
            "Mococo body animation did not bind after loading the prefab");
        Check(animation->FindClip("dance1 facial") != nullptr &&
            !animation->layers.empty(),
            "Mococo facial animation did not bind after loading the prefab");
        scene.Update(1.f / 60.f);
        Check(animation->time > 0.f && animation->layers[0].time > 0.f,
            "Mococo body or facial animation did not advance during scene playback");
        const std::string saved = Engine::Serialization::SceneSerializer::SavePrefabToString(*root);
        Check(saved.find("components:RigConstraints") != std::string::npos &&
            saved.find("components:RigPhysicsBinding") != std::string::npos &&
            saved.find("components:RigController") == std::string::npos,
            "Reusable rig components did not serialize independently");
        Check(saved.find("PmxRigController") == std::string::npos &&
            saved.find("PmxMaterialMorph") == std::string::npos &&
            saved.find("useMmdSpecular") == std::string::npos,
            "Mococo prefab still serializes legacy component or material fields");

        auto* authored = scene.AddObject("Authored material");
        auto* material = authored->AddComponent<Engine::Components::Material>();
        auto* morph = authored->AddComponent<Engine::Components::MaterialPropertyMorph>();
        Check(morph->editorAddable && rig->editorAddable,
            "Reusable components are hidden from authoring");
        Engine::Model::ImportedMaterialMorph control;
        control.name = "fade";
        Engine::Model::ImportedMaterialMorphOffset offset;
        offset.additive = true;
        offset.diffuse.a = -.25f;
        control.offsets.push_back(offset);
        morph->morphs.push_back(control);
        morph->SetAnimatedMorphWeights({ { "fade", 1.f } });
        morph->Apply();
        Check(std::abs(material->baseColorAlpha - .75f) < .0001f,
            "Authored material morph did not apply");
        auto* authoredRigRoot = scene.AddObject("Authored rig");
        auto* authoredModel = authoredRigRoot->AddComponent<Engine::Components::Model>();
        auto* authoredRig = authoredRigRoot->AddComponent<Engine::Components::RigController>();
        auto* authoredBone = scene.AddObject("Authored bone");
        authoredBone->Parent = authoredRigRoot;
        authoredRigRoot->Children.push_back(authoredBone);
        authoredModel->BindNode(0, authoredBone);
        authoredBone->transform.rotation = { .3f, .4f, .2f };
        authoredRig->fixedAxes.push_back({ 0u, { 0.f, 1.f, 0.f } });
        authoredRig->Apply();
        Check(std::abs(authoredBone->transform.rotation.x) < .001f &&
            std::abs(authoredBone->transform.rotation.z) < .001f,
            "Authored rig axis constraint did not apply");
        auto* oldObject = scene.AddObject("Old component names");
        oldObject->AddComponent<Engine::Components::RigController>()
            ->fixedAxes.push_back({ 0u, { 0.f, 1.f, 0.f } });
        oldObject->AddComponent<Engine::Components::Material>();
        oldObject->AddComponent<Engine::Components::MaterialPropertyMorph>();
        std::string oldXml = Engine::Serialization::SceneSerializer::SaveObjectToString(*oldObject);
        ReplaceAll(oldXml, "RigController", "PmxRigController");
        ReplaceAll(oldXml, "MaterialPropertyMorph", "PmxMaterialMorph");
        ReplaceAll(oldXml, "specularModel", "useMmdSpecular");
        ReplaceAll(oldXml, "MetallicRoughness", "true");
        Check(oldXml.find("PmxRigController") != std::string::npos,
            "Legacy rig fixture was not constructed");
        Check(oldXml.find("PmxMaterialMorph") != std::string::npos,
            "Legacy material morph fixture was not constructed");
        Check(oldXml.find("useMmdSpecular") != std::string::npos,
            "Legacy shading fixture was not constructed");
        auto* converted = Engine::Serialization::SceneSerializer::InstantiateObjectFromString(
            scene, oldXml);
        Check(converted != nullptr, "Old object did not load");
        Check(converted->GetComponent<Engine::Components::RigController>() != nullptr,
            "Old rig type did not convert on load");
        Check(converted->GetComponent<Engine::Components::RigConstraints>() != nullptr,
            "Old rig constraints did not migrate on load");
        Check(converted->GetComponent<Engine::Components::MaterialPropertyMorph>() != nullptr,
            "Old material morph type did not convert on load");
        auto* convertedMaterial = converted->GetComponent<Engine::Components::Material>();
        Check(convertedMaterial && convertedMaterial->specularModel == "BlinnPhong",
            "Old material specular setting did not convert on load");
        const std::string convertedXml =
            Engine::Serialization::SceneSerializer::SaveObjectToString(*converted);
        Check(convertedXml.find("PmxRigController") == std::string::npos &&
            convertedXml.find("PmxMaterialMorph") == std::string::npos &&
            convertedXml.find("useMmdSpecular") == std::string::npos,
            "Converted object still saves legacy component or material fields");
        Engine::Components::RigPhysicsBinding physicsCopy;
        physicsCopy.Deserialize(physics->Serialize());
        Check(physicsCopy.physicsJoints.size() == physics->physicsJoints.size(),
            "Physics joint descriptors did not round trip independently");
        auto* morphRoot = scene.AddObject("Named bone morph");
        auto* morphModel = morphRoot->AddComponent<Engine::Components::Model>();
        auto* morphBone = scene.AddObject("Morph bone");
        morphBone->Parent = morphRoot;
        morphRoot->Children.push_back(morphBone);
        morphModel->BindNode(0, morphBone);
        auto* boneControls = morphRoot->AddComponent<Engine::Components::RigMorphControls>();
        Engine::Model::ImportedBoneMorph boneMorph;
        boneMorph.name = "lift";
        boneMorph.offsets.push_back({ 0u, { 0.f, 2.f, 0.f },
            { 1.f, 0.f, 0.f, 0.f } });
        boneControls->boneMorphs.push_back(boneMorph);
        boneControls->SetAnimatedMorphWeights({ { "lift", 1.f } });
        boneControls->ApplyBoneMorphs();
        Check(std::abs(morphBone->transform.position.y - 2.f) < .001f,
            "Named bone morph animation input did not apply");
        boneControls->SetAnimatedMorphWeights({});
        boneControls->ApplyBoneMorphs();
        Check(std::abs(morphBone->transform.position.y) < .001f,
            "Bone morph did not restore its prior pose");
        Check(!Engine::Serialization::SceneSerializer::GetRegisteredFactory(
                "PmxRigController") &&
            !Engine::Serialization::SceneSerializer::GetRegisteredFactory(
                "PmxMaterialMorph"),
            "Legacy PMX component classes remain registered");
        std::cout << "Reusable rig and material components reload from Mococo prefab\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

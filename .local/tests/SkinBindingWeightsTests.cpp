#include "Editor/Core/View/Templates/EditModes/Skeleton/SkinBindingWeights.h"
#include "Core/Compoonents/Animation/SkinnedMesh.h"
#include "Core/Compoonents/Animation/Skeleton.h"
#include "Core/Compoonents/Animation/Model.h"
#include "Core/Compoonents/Animation/AnimationBone.h"
#include "Core/Compoonents/Obj/Mesh.h"
#include "Core/ComponentReference.h"
#include "Core/Object.h"
#include "Core/Scene/Scene.h"
#include "Core/Serialization/SceneSerializer.h"
#include <cassert>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <limits>

using namespace Engine::Editor::SkinBindingWeights;

static bool Near(float a, float b)
{ return std::abs(a - b) < 1e-5f; }

int main()
{
    std::vector<Vertex> vertices(3);
    vertices[0].pos[0] = 0.f;
    vertices[1].pos[0] = 1.f;
    vertices[2].pos[0] = 2.f;
    const std::vector<glm::vec3> joints{ { 0.f, 0.f, 0.f },
        { 2.f, 0.f, 0.f } };
    assert(Generate(vertices, joints));
    assert(Near(Weight(vertices[0], 0), 1.f));
    assert(Near(Joint(vertices[0], 0), 0.f));
    assert(Near(Weight(vertices[2], 0), 1.f));
    assert(Near(Joint(vertices[2], 0), 1.f));
    assert(Near(Weight(vertices[1], 0) + Weight(vertices[1], 1), 1.f));
    assert(Inspect(vertices, { true, true }).missingJoints == 0);

    auto original = vertices;
    assert(!Remap(vertices, { 0, -1 }));
    assert(vertices[2].joints0[0] == original[2].joints0[0]);
    assert(Remap(vertices, { 1, 0 }));
    assert(Near(Joint(vertices[0], 0), 1.f));
    assert(Near(Joint(vertices[2], 0), 0.f));

    std::vector<Vertex> removal(2);
    Joint(removal[0], 0) = 2.f;
    Weight(removal[0], 0) = 1.f;
    Joint(removal[1], 0) = 1.f;
    Weight(removal[1], 0) = 1.f;
    assert(!RemoveUnweightedJoint(removal, 1));
    assert(Near(Joint(removal[0], 0), 2.f));
    assert(Near(Joint(removal[1], 0), 1.f));
    Weight(removal[1], 0) = 0.f;
    assert(RemoveUnweightedJoint(removal, 1));
    assert(Near(Joint(removal[0], 0), 1.f));
    assert(Near(Joint(removal[1], 0), 0.f));

    Joint(vertices[0], 0) = 99.f;
    Joint(vertices[0], 1) = std::numeric_limits<float>::quiet_NaN();
    Weight(vertices[0], 1) = .25f;
    Weight(vertices[1], 0) = 0.f;
    Weight(vertices[1], 1) = 0.f;
    const auto diagnosis = Inspect(vertices, { true, true });
    assert(diagnosis.missingJoints == 1);
    assert(diagnosis.unweighted == 1);
    assert(Repair(vertices, { true, true }, joints));
    const auto repaired = Inspect(vertices, { true, true });
    assert(repaired.missingJoints == 0 && repaired.unweighted == 0);
    assert(std::isfinite(Joint(vertices[0], 1)));
    assert(Assign(vertices, { 0, 2 }, 1));
    assert(Near(Joint(vertices[0], 0), 1.f));
    assert(Near(Weight(vertices[0], 0), 1.f));
    assert(!Assign(vertices, { 3 }, 1));

    Engine::Components::SkinnedMesh skin;
    skin.bindMeshToModel[3][0] = 2.5f;
    skin.skeletonReference.componentType = "Skeleton";
    skin.skeletonReference.objectName = "Rig";
    const auto saved = skin.Serialize();
    Engine::Components::SkinnedMesh loaded;
    loaded.Deserialize(saved);
    assert(Near(loaded.bindMeshToModel[3][0], 2.5f));
    assert(loaded.skeletonReference.objectName == "Rig");

    Engine::Components::Skeleton skeleton;
    skeleton.skinIndex = 7;
    skeleton.jointNodes = { 2, 4 };
    skeleton.inverseBindMatrices.assign(2, glm::mat4(1.f));
    skeleton.inverseBindMatrices[1][3][1] = -3.f;
    Engine::Components::Skeleton restored;
    restored.Deserialize(skeleton.Serialize());
    assert(restored.skinIndex == 7 && restored.jointNodes[1] == 4);
    assert(Near(restored.inverseBindMatrices[1][3][1], -3.f));

    const auto path = std::filesystem::temp_directory_path() /
        ("skin-binding-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()) +
            ".mesh");
    assert(Engine::Components::Mesh::SaveNativeFile(path.string(), vertices));
    Engine::Components::Mesh reloadedMesh;
    reloadedMesh.LoadFromFile(path.string());
    assert(reloadedMesh.GetVertices().size() == vertices.size());
    assert(Near(reloadedMesh.GetVertices()[0].joints0[0], 1.f));

    Engine::Scene::Scene scene;
    auto* rigObject = scene.AddObject("Rig");
    auto* sceneModel = rigObject->AddComponent<Engine::Components::Model>();
    auto* sceneSkeleton = rigObject->AddComponent<Engine::Components::Skeleton>();
    sceneSkeleton->skinIndex = 7;
    sceneSkeleton->modelReference =
        Engine::Core::CaptureComponentReference(sceneModel, "Model");
    auto* jointObject = scene.AddObject("Joint");
    assert(scene.MoveObject(jointObject, rigObject,
        Engine::Scene::Scene::ObjectPlacement::AsChild));
    auto* bone = jointObject->AddComponent<Engine::Components::AnimationBone>();
    bone->skinIndex = 7;
    bone->nodeIndex = 0;
    bone->paletteIndex = 0;
    sceneModel->BindNode(0, jointObject);
    auto* secondJoint = scene.AddObject("Joint 2");
    assert(scene.MoveObject(secondJoint, rigObject,
        Engine::Scene::Scene::ObjectPlacement::AsChild));
    auto* secondBone =
        secondJoint->AddComponent<Engine::Components::AnimationBone>();
    secondBone->skinIndex = 7;
    secondBone->nodeIndex = 1;
    secondBone->paletteIndex = 1;
    sceneModel->BindNode(1, secondJoint);
    sceneSkeleton->jointNodes = { 0, 1 };
    sceneSkeleton->inverseBindMatrices.assign(2, glm::mat4(1.f));
    auto* skinnedObject = scene.AddObject("Skinned Surface");
    assert(scene.MoveObject(skinnedObject, rigObject,
        Engine::Scene::Scene::ObjectPlacement::AsChild));
    auto* sceneMesh = skinnedObject->AddComponent<Engine::Components::Mesh>();
    sceneMesh->LoadFromFile(path.string());
    auto* sceneSkin = skinnedObject->AddComponent<Engine::Components::SkinnedMesh>();
    sceneSkin->skinIndex = 7;
    sceneSkin->meshReference =
        Engine::Core::CaptureComponentReference(sceneMesh, "Mesh");
    sceneSkin->skeletonReference =
        Engine::Core::CaptureComponentReference(sceneSkeleton, "Skeleton");
    const auto sceneText = scene.SaveToString();
    Engine::Scene::Scene reloadedScene;
    assert(Engine::Serialization::SceneSerializer::LoadFromString(
        reloadedScene, sceneText, nullptr));
    auto* reloadedObject = reloadedScene.FindObjectByName("Skinned Surface");
    assert(reloadedObject);
    auto* reloadedSkin =
        reloadedObject->GetComponent<Engine::Components::SkinnedMesh>();
    assert(reloadedSkin && reloadedSkin->ResolveSkeleton());
    assert(reloadedSkin->ResolveSkeleton()->jointNodes.size() == 2);
    assert(Near(reloadedObject->GetComponent<Engine::Components::Mesh>()
        ->GetVertices()[0].joints0[0], 1.f));
    assert(reloadedScene.FindObjectByName("Joint")
        ->GetComponent<Engine::Components::AnimationBone>()->nodeIndex == 0);
    std::filesystem::remove(path);
}

#include "Editor/Core/SkeletonBindPose.h"
#include "Editor/Core/MeshEditSnapshot.h"
#include "Editor/Core/SnapshotHistory.h"
#include "Editor/Core/View/Templates/EditModes/Skeleton/SkinBindingWeights.h"
#include "Editor/Core/View/Templates/EditModes/Skeleton/WeightInfluence.h"
#include "Editor/Core/View/Templates/EditModes/Skeleton/MirrorWeightPaint.h"
#include "Editor/Core/View/Templates/EditModes/Skeleton/WeightPaintSurface.h"
#include "Core/Compoonents/Animation/AnimationBone.h"
#include "Core/Compoonents/Animation/AnimationManager.h"
#include "Core/Compoonents/Animation/Model.h"
#include "Core/Compoonents/Animation/Skeleton.h"
#include "Core/Compoonents/Animation/SkinnedMesh.h"
#include "Core/Compoonents/Obj/Mesh.h"
#include "Core/ComponentReference.h"
#include "Core/Object.h"
#include "Core/Scene/Scene.h"
#include "Core/Serialization/SceneSerializer.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <glm/gtc/matrix_inverse.hpp>

#define CHECK(condition) do { if (!(condition)) { \
    std::fprintf(stderr, "%s:%d: %s failed\n", __FILE__, __LINE__, #condition); \
    return 1; } } while (false)

static bool Near(float a, float b, float tolerance = 1e-4f)
{ return std::abs(a - b) < tolerance; }

static bool Identity(const glm::mat4& matrix)
{
    for (int column = 0; column < 4; ++column)
        for (int row = 0; row < 4; ++row)
            if (!Near(matrix[column][row], column == row ? 1.f : 0.f))
                return false;
    return true;
}

int main()
{
    using Engine::Components::AnimationBone;
    using Engine::Components::AnimationManager;
    using Engine::Components::Mesh;
    using Engine::Components::Model;
    using Engine::Components::Skeleton;
    using Engine::Components::SkinnedMesh;
    using Engine::Scene::Scene;
    namespace Serializer = Engine::Serialization;

    {
        namespace Mirror = Engine::Editor::MirrorWeightPaint;
        glm::mat4 sourceRoot(1.f), sourceTip(1.f);
        sourceTip[3] = { 0.f, 2.f, 0.f, 1.f };
        glm::mat4 targetRoot(1.f), targetTip(1.f);
        targetRoot[3] = { 4.f, 0.f, 0.f, 1.f };
        targetTip[0] = { -1.f, 0.f, 0.f, 0.f };
        targetTip[2] = { 0.f, 0.f, -1.f, 0.f };
        targetTip[3] = { 4.f, 4.f, 0.f, 1.f };
        Mirror::BoneFrame sourceFrame, targetFrame;
        CHECK(Mirror::BuildFrame(sourceRoot, sourceTip, sourceFrame));
        CHECK(Mirror::BuildFrame(targetRoot, targetTip, targetFrame));
        const glm::vec3 center = Mirror::MapPoint(sourceFrame, targetFrame,
            { .5f, 1.f, .25f });
        CHECK(Near(center.x, 4.5f) && Near(center.y, 2.f) &&
            Near(center.z, -.25f));
        // Bone length is normalized; lateral brush dimensions do not scale.
        const glm::vec3 lateral = Mirror::MapPoint(sourceFrame, targetFrame,
            { .75f, 1.f, .25f });
        CHECK(Near(glm::length(lateral - center), .25f));
        CHECK(Near(Mirror::MapPoint(sourceFrame, targetFrame,
            { 0.f, 2.f, 0.f }).y, 4.f));
        const auto triangle = Mirror::ClosestPoint({ 4.5f, 2.f, .1f },
            { 4.f, 1.f, 0.f }, { 5.f, 1.f, 0.f }, { 4.5f, 3.f, 0.f });
        CHECK(Near(triangle.point.x, 4.5f) &&
            Near(triangle.point.y, 2.f) && Near(triangle.point.z, 0.f));
        CHECK(Near(triangle.barycentric.x + triangle.barycentric.y +
            triangle.barycentric.z, 1.f));
    }
    {
        namespace Surface = Engine::Editor::WeightPaintSurface;
        // Two connected faces share an edge; the third face occupies the
        // same screen area but has separate vertices (an overlapping layer).
        const std::vector<glm::vec3> positions{
            { 0.f, 0.f, 0.f }, { 10.f, 0.f, 0.f },
            { 0.f, 10.f, 0.f }, { 10.f, 10.f, 0.f },
            { 0.f, 0.f, 0.f }, { 10.f, 0.f, 0.f },
            { 0.f, 10.f, 0.f } };
        const auto topology = Surface::BuildTopology(
            { 0, 1, 2, 1, 3, 2, 4, 5, 6 }, positions.size());
        const auto metric = [](const glm::vec3& offset)
        { return glm::length(glm::vec2(offset)); };
        const auto hard = [](float distance, float radius)
        { return distance <= radius ? 1.f : 0.f; };
        const auto visible = [](size_t,
            const Engine::Editor::MirrorWeightPaint::TrianglePoint&)
        { return true; };
        const auto sparse = Surface::Coverage(topology, positions, 0,
            { 3.f, 3.f, 0.f }, .5f, metric, hard, visible);
        CHECK(sparse[0] > 0.f && sparse[1] > 0.f && sparse[2] > 0.f);
        CHECK(sparse[4] == 0.f && sparse[5] == 0.f && sparse[6] == 0.f);
        const auto acrossEdge = Surface::Coverage(topology, positions, 0,
            { 7.f, 5.f, 0.f }, 6.f, metric, hard, visible);
        CHECK(acrossEdge[3] > 0.f);
        CHECK(acrossEdge[4] == 0.f && acrossEdge[5] == 0.f &&
            acrossEdge[6] == 0.f);
        const auto occluded = Surface::Coverage(topology, positions, 0,
            { 7.f, 5.f, 0.f }, 6.f, metric, hard,
            [](size_t face,
                const Engine::Editor::MirrorWeightPaint::TrianglePoint&)
            { return face != 1; });
        CHECK(occluded[3] == 0.f);
    }

    const auto path = std::filesystem::temp_directory_path() /
        ("skeleton-authoring-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()) +
            ".mesh");
    struct RemoveTemp
    {
        std::filesystem::path path;
        ~RemoveTemp() { std::error_code ignored; std::filesystem::remove(path, ignored); }
    } cleanup{ path };
    std::vector<Mesh::Vertex> vertices(3);
    vertices[0].pos[0] = -1.f;
    vertices[1].pos[0] = 1.f;
    vertices[2].pos[1] = 1.f;
    for (auto& vertex : vertices)
    {
        vertex.joints0[0] = 0.f;
        vertex.joints0[1] = 1.f;
        vertex.weights0[0] = .5f;
        vertex.weights0[1] = .5f;
    }
    CHECK(Mesh::SaveNativeFile(path.string(), vertices));

    Scene scene;
    auto* rig = scene.AddObject("Rig");
    rig->transform.position = { 5.f, 0.f, 0.f };
    auto* model = rig->AddComponent<Model>();
    auto* skeleton = rig->AddComponent<Skeleton>();
    skeleton->modelReference =
        Engine::Core::CaptureComponentReference(model, "Model");
    skeleton->skinIndex = 3;
    auto* shoulder = scene.AddObject("Shoulder");
    CHECK(scene.MoveObject(shoulder, rig, Scene::ObjectPlacement::AsChild));
    shoulder->transform.position = { 0.f, 1.f, 0.f };
    auto* shoulderBone = shoulder->AddComponent<AnimationBone>();
    shoulderBone->nodeIndex = 0;
    shoulderBone->paletteIndex = 0;
    shoulderBone->skinIndex = 3;
    model->BindNode(0, shoulder);
    auto* elbow = scene.AddObject("Elbow");
    CHECK(scene.MoveObject(elbow, shoulder, Scene::ObjectPlacement::AsChild));
    elbow->transform.position = { 0.f, 2.f, 0.f };
    auto* elbowBone = elbow->AddComponent<AnimationBone>();
    elbowBone->nodeIndex = 1;
    elbowBone->paletteIndex = 1;
    elbowBone->parentPaletteIndex = 0;
    elbowBone->skinIndex = 3;
    model->BindNode(1, elbow);
    skeleton->jointNodes = { 0, 1 };
    const glm::mat4 inverseRoot = glm::inverse(rig->transform.GetWorldMatrix());
    for (auto* joint : { shoulder, elbow })
        skeleton->inverseBindMatrices.push_back(glm::inverse(
            inverseRoot * joint->transform.GetWorldMatrix()));

    auto* surface = scene.AddObject("Skinned Surface");
    CHECK(scene.MoveObject(surface, rig, Scene::ObjectPlacement::AsChild));
    surface->transform.position = { 0.f, 0.f, 0.f };
    auto* mesh = surface->AddComponent<Mesh>();
    mesh->LoadFromFile(path.string());
    auto* skin = surface->AddComponent<SkinnedMesh>();
    skin->skinIndex = 3;
    skin->meshReference = Engine::Core::CaptureComponentReference(mesh, "Mesh");
    skin->skeletonReference =
        Engine::Core::CaptureComponentReference(skeleton, "Skeleton");
    skin->MarkConfigurationDirty();
    CHECK(skin->ResolveSkeleton() == skeleton);
    CHECK(skeleton->ResolveJoints().size() == 2);
    for (const auto& matrix : skin->BuildPalette()) CHECK(Identity(matrix));

    // Editing the rest transform must update inverse binds and every skin's
    // palette without moving the rendered mesh in its new rest pose.
    const glm::mat4 previousBind = skeleton->inverseBindMatrices[1];
    elbow->transform.position.y = 2.5f;
    std::string bindError;
    CHECK(Engine::Editor::SkeletonBindPose::Commit(
        scene, *skeleton, bindError));
    CHECK(skeleton->inverseBindMatrices[1] != previousBind);
    for (const auto& matrix : skin->BuildPalette()) CHECK(Identity(matrix));
    elbow->transform.position.y = 5.f;
    CHECK(Engine::Editor::SkeletonBindPose::Restore(*skeleton));
    CHECK(Near(elbow->transform.position.y, 2.5f));
    elbow->transform.position.y = 2.f;
    CHECK(Engine::Editor::SkeletonBindPose::Commit(
        scene, *skeleton, bindError));

    auto* animation = rig->AddComponent<AnimationManager>();
    Engine::Model::AnimationClip clip;
    clip.clipName = "Bend";
    clip.duration = 1.f;
    Engine::Model::AnimationChannel channel;
    channel.nodeIndex = 1;
    channel.path = Engine::Model::AnimationChannel::Path::Translation;
    channel.times = { 0.f, 1.f };
    channel.values = { 0.f, 2.f, 0.f, 0.f, 4.f, 0.f };
    clip.channels.push_back(channel);
    animation->clips.push_back(clip);
    animation->clip = "Bend";
    animation->looping = false;
    animation->Start();
    animation->Tick(.5f);
    CHECK(Near(elbow->transform.position.y, 3.f));

    // Skeleton Edit restores the rest pose and freezes animation sampling.
    scene.SetEditorMeshEditPose(true);
    CHECK(Engine::Editor::SkeletonBindPose::Restore(*skeleton));
    CHECK(Near(elbow->transform.position.y, 2.f));
    animation->Tick(.25f);
    CHECK(Near(elbow->transform.position.y, 2.f));
    for (const auto& matrix : skin->BuildPalette()) CHECK(Identity(matrix));

    // Weight Paint uses the same static pose, even while weights change.
    scene.SetEditorWeightPaint(mesh, 1);
    auto painted = mesh->GetVertices();
    CHECK(Engine::Editor::WeightInfluence::Set(
        painted[0], 1, .8f, std::vector<bool>(2, false)));
    CHECK(mesh->UpdateAuthoredVertices(std::move(painted)));
    animation->Tick(.25f);
    CHECK(Near(elbow->transform.position.y, 2.f));
    for (const auto& matrix : skin->BuildPalette()) CHECK(Identity(matrix));
    scene.SetEditorWeightPaint(nullptr, -1);
    animation->Tick(.25f);
    CHECK(Near(elbow->transform.position.y, 2.f));
    scene.SetEditorMeshEditPose(false);
    animation->Tick(.25f);
    CHECK(elbow->transform.position.y > 2.f);
    CHECK(Engine::Editor::SkeletonBindPose::Restore(*skeleton));

    // The scene serializer persists the rig and skin reference; weights live
    // in the shared mesh asset, so save them before reloading the scene.
    CHECK(Mesh::SaveNativeFile(path.string(), mesh->GetVertices()));
    const std::string beforeHierarchy = scene.SaveToString();
    Scene loaded;
    CHECK(Serializer::SceneSerializer::LoadFromString(
        loaded, beforeHierarchy, nullptr));
    auto* loadedRig = loaded.FindObjectByName("Rig");
    auto* loadedSurface = loaded.FindObjectByName("Skinned Surface");
    CHECK(loadedRig && loadedSurface);
    auto* loadedSkeleton = loadedRig->GetComponent<Skeleton>();
    auto* loadedSkin = loadedSurface->GetComponent<SkinnedMesh>();
    auto* loadedMesh = loadedSurface->GetComponent<Mesh>();
    CHECK(loadedSkeleton && loadedSkin && loadedMesh);
    CHECK(loadedSkin->ResolveSkeleton() == loadedSkeleton);
    CHECK(loadedSkeleton->ResolveJoints().size() == 2);
    CHECK(!loaded.IsEditorMeshEditPose());
    CHECK(Near(Engine::Editor::WeightInfluence::Get(
        loadedMesh->GetVertices()[0], 1), .8f));
    CHECK(loaded.FindObjectByName("Elbow")->GetComponent<AnimationBone>()
        ->nodeIndex == 1);

    // Hierarchy changes preserve joint identity after rebinding model paths.
    auto* loadedElbow = loaded.FindObjectByName("Elbow");
    CHECK(!loaded.MoveObject(loadedRig, loadedElbow,
        Scene::ObjectPlacement::AsChild));
    CHECK(loaded.MoveObject(loadedElbow, loadedRig,
        Scene::ObjectPlacement::AsChild));
    auto* loadedModel = loadedRig->GetComponent<Model>();
    loadedModel->BindNode(1, loadedElbow);
    loadedElbow->GetComponent<AnimationBone>()->parentPaletteIndex = -1;
    CHECK(loadedSkeleton->ResolveJoints()[1] == loadedElbow);
    auto* wrist = loaded.AddObject("Wrist");
    CHECK(loaded.MoveObject(wrist, loadedElbow,
        Scene::ObjectPlacement::AsChild));
    wrist->transform.position = { 0.f, 1.f, 0.f };
    auto* wristBone = wrist->AddComponent<AnimationBone>();
    wristBone->nodeIndex = 2;
    wristBone->paletteIndex = 2;
    wristBone->parentPaletteIndex = 1;
    wristBone->skinIndex = 3;
    loadedModel->BindNode(2, wrist);
    loadedSkeleton->jointNodes.push_back(2);
    loadedSkeleton->inverseBindMatrices.push_back(glm::inverse(
        glm::inverse(loadedRig->transform.GetWorldMatrix()) *
        wrist->transform.GetWorldMatrix()));
    CHECK(loadedSkeleton->ResolveJoints()[2] == wrist);
    const std::string afterHierarchy = loaded.SaveToString();
    std::string legacyScene = afterHierarchy;
    const std::string marker = " _valueType=\"string\"";
    for (size_t at = 0; (at = legacyScene.find(marker, at)) !=
        std::string::npos;)
        legacyScene.erase(at, marker.size());
    Scene legacyLoaded;
    CHECK(Serializer::SceneSerializer::LoadFromString(
        legacyLoaded, legacyScene, nullptr));
    CHECK(legacyLoaded.FindObjectByName("Rig")->GetComponent<Skeleton>()
        ->ResolveJoints()[2] == legacyLoaded.FindObjectByName("Wrist"));

    // These are the same transactional snapshot steps used by document Undo
    // and Redo. A failed restore must leave history and scene untouched.
    std::deque<std::string> undo{ beforeHierarchy }, redo;
    std::string baseline = afterHierarchy;
    const auto restoreScene = [&](const std::string& snapshot)
    { return Serializer::SceneSerializer::LoadFromString(
        loaded, snapshot, nullptr); };
    undo.push_back("invalid scene snapshot");
    CHECK(!Engine::Editor::StepSnapshotHistory(false,
        undo, redo, baseline, restoreScene));
    CHECK(loaded.FindObjectByName("Wrist") != nullptr);
    CHECK(undo.size() == 2 && redo.empty());
    undo.pop_back();
    CHECK(Engine::Editor::StepSnapshotHistory(false,
        undo, redo, baseline, restoreScene));
    CHECK(loaded.FindObjectByName("Wrist") == nullptr);
    CHECK(Engine::Editor::StepSnapshotHistory(true,
        undo, redo, baseline, restoreScene));
    CHECK(loaded.FindObjectByName("Wrist") != nullptr);
    loadedRig = loaded.FindObjectByName("Rig");
    loadedSkeleton = loadedRig->GetComponent<Skeleton>();
    CHECK(loadedSkeleton->ResolveJoints().size() == 3);
    CHECK(loadedSkeleton->ResolveJoints()[2] == loaded.FindObjectByName("Wrist"));
    CHECK(loaded.FindObjectByName("Elbow")->Parent == loadedRig);
    loadedSkeleton->jointNodes.pop_back();
    loadedSkeleton->inverseBindMatrices.pop_back();
    loaded.RemoveObject(loaded.FindObjectByName("Wrist"));
    loadedRig->GetComponent<Model>()->UnbindNode(2);
    CHECK(loadedRig->GetComponent<Model>()->ResolveNode(2) == nullptr);
    CHECK(loadedSkeleton->ResolveJoints().size() == 2);
    Scene afterRemoval;
    CHECK(Serializer::SceneSerializer::LoadFromString(
        afterRemoval, loaded.SaveToString(), nullptr));
    CHECK(afterRemoval.FindObjectByName("Wrist") == nullptr);
    CHECK(afterRemoval.FindObjectByName("Rig")->GetComponent<Model>()
        ->ResolveNode(2) == nullptr);
    CHECK(afterRemoval.FindObjectByName("Rig")->GetComponent<Skeleton>()
        ->ResolveJoints().size() == 2);

    // Mesh and Weight Paint use the same authored snapshot history.
    loadedMesh = loaded.FindObjectByName("Skinned Surface")->GetComponent<Mesh>();
    const std::string originalWeights =
        Engine::Editor::CaptureMeshSnapshot(*loadedMesh);
    auto edited = loadedMesh->GetVertices();
    CHECK(Engine::Editor::SkinBindingWeights::Assign(edited, { 0, 1 }, 0));
    CHECK(loadedMesh->UpdateAuthoredVertices(std::move(edited)));
    const std::string assignedWeights =
        Engine::Editor::CaptureMeshSnapshot(*loadedMesh);
    std::deque<std::string> weightUndo{ originalWeights }, weightRedo;
    std::string weightBaseline = assignedWeights;
    CHECK(Engine::Editor::StepMeshSnapshotHistory(*loadedMesh, false,
        weightUndo, weightRedo, weightBaseline));
    CHECK(Engine::Editor::CaptureMeshSnapshot(*loadedMesh) == originalWeights);
    CHECK(Engine::Editor::StepMeshSnapshotHistory(*loadedMesh, true,
        weightUndo, weightRedo, weightBaseline));
    CHECK(Engine::Editor::CaptureMeshSnapshot(*loadedMesh) == assignedWeights);
    weightUndo.push_back("invalid mesh snapshot");
    CHECK(!Engine::Editor::StepMeshSnapshotHistory(*loadedMesh, false,
        weightUndo, weightRedo, weightBaseline));
    CHECK(Engine::Editor::CaptureMeshSnapshot(*loadedMesh) == assignedWeights);
    CHECK(weightUndo.back() == "invalid mesh snapshot");
    return 0;
}

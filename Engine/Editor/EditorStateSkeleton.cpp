#include "pch.h"
#include "EditorState.h"
#include "Core/Scene/Scene.h"
#include "Core/Compoonents/Animation/AnimationBone.h"
#include "Core/Compoonents/Animation/Model.h"
#include "Core/Compoonents/Animation/Skeleton.h"
#include "Core/Compoonents/Animation/SkinnedMesh.h"
#include "Core/Compoonents/Camera/Camera.h"
#include "Core/Compoonents/Obj/Mesh.h"
#include "Core/Compoonents/Transform.h"
#include "Core/View/Views/SceneView.h"
#include "Core/View/Views/HierarchyView.h"
#include "Core/View/Views/PropertiesView.h"
#include "Core/View/Views/ConsoleView.h"
#include "Core/View/Templates/EditModes/Skeleton/WeightInfluence.h"
#include "Core/View/Templates/EditModes/Skeleton/SkinBindingWeights.h"
#include "Core/View/Templates/EditModes/Skeleton/MirrorWeightPaint.h"
#include "Core/SkeletonBindPose.h"
#include "Core/MeshEditSnapshot.h"
#include "Core/SnapshotHistory.h"
#include <algorithm>
#include <charconv>
#include <cctype>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <numeric>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <glm/gtc/matrix_inverse.hpp>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/matrix_decompose.hpp>

namespace Engine::Editor
{
namespace
{
float BoneWeight(const Engine::Components::Mesh::Vertex& vertex, int bone)
{
    return WeightInfluence::Get(vertex, bone);
}


std::string MeshSavePath(const Engine::Components::Mesh& mesh,
    const std::string& documentPath)
{
    std::filesystem::path path(mesh.GetFilePath());
    if (path.empty() && !documentPath.empty())
        path = std::filesystem::path(documentPath).parent_path() /
            ((mesh.Owner ? mesh.Owner->name : std::string("Mesh")) + ".mesh");
    if (!path.empty()) path.replace_extension(".mesh");
    return path.string();
}

std::string MeshCacheKey(const std::string& path)
{
    std::error_code error;
    std::string key = std::filesystem::weakly_canonical(path, error)
        .generic_string();
    if (error) key = std::filesystem::path(path).lexically_normal()
        .generic_string();
#ifdef _WIN32
    std::transform(key.begin(), key.end(), key.begin(),
        [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
#endif
    return key;
}

bool ValidateBoneReferences(Engine::Components::Skeleton& skeleton,
    std::string& error)
{
    auto* model = skeleton.ResolveModel();
    const auto& joints = skeleton.ResolveJoints();
    if (!model || !model->Owner || joints.empty() ||
        joints.size() != skeleton.jointNodes.size() ||
        joints.size() != skeleton.inverseBindMatrices.size())
    { error = "The model, joint references, or inverse bind matrices are incomplete.";
        return false; }
    if (std::abs(glm::determinant(
            model->Owner->transform.GetWorldMatrix())) < 1e-8f)
    { error = "The model root has zero scale."; return false; }
    std::unordered_set<Engine::Core::Object*> unique;
    for (size_t i = 0; i < joints.size(); ++i)
    {
        auto* joint = joints[i];
        if (!joint || !unique.insert(joint).second ||
            model->ResolveNode(skeleton.jointNodes[i]) != joint)
        { error = "The joint palette has missing or duplicate node references.";
            return false; }
        if (std::abs(glm::determinant(
                joint->transform.GetWorldMatrix())) < 1e-8f)
        { error = "A joint transform has zero scale."; return false; }
        bool insideModel = false;
        for (auto* parent = joint; parent; parent = parent->Parent)
            if (parent == model->Owner) { insideModel = true; break; }
        auto* bone = joint->GetComponent<Engine::Components::AnimationBone>();
        if (!insideModel || !bone || bone->skinIndex != skeleton.skinIndex ||
            bone->nodeIndex != skeleton.jointNodes[i] ||
            (bone->paletteIndex >= 0 &&
                bone->paletteIndex != static_cast<int>(i)))
        { error = "A joint is outside the model or has inconsistent bone metadata.";
            return false; }
        std::unordered_set<Engine::Core::Object*> ancestors;
        for (auto* parent = joint; parent; parent = parent->Parent)
            if (!ancestors.insert(parent).second)
            { error = "A bone parent cycle was found."; return false; }
    }
    for (size_t i = 0; i < joints.size(); ++i)
    {
        auto* bone = joints[i]->GetComponent<Engine::Components::AnimationBone>();
        int parentPalette = -1;
        for (auto* parent = joints[i]->Parent; parent; parent = parent->Parent)
        {
            auto found = std::find(joints.begin(), joints.end(), parent);
            if (found != joints.end())
            { parentPalette = static_cast<int>(found - joints.begin()); break; }
        }
        if (bone->parentPaletteIndex >= 0 &&
            bone->parentPaletteIndex != parentPalette)
        { error = "A bone parent reference does not match the hierarchy.";
            return false; }
    }
    return true;
}

}

EditorState::SkeletonEditSession* EditorState::ActiveSkeletonEditSession()
{
    if (m_activeSceneAssetDocument)
        return &m_activeSceneAssetDocument->skeletonEdit;
    if (m_prefabDocumentFocused && m_prefabScene)
        return &m_prefabSkeletonEdit;
    return &m_mainSkeletonEdit;
}

void EditorState::SetSkeletonEditMode(Engine::Scene::Scene* scene,
    SkeletonEditSession& session, MeshEditSession& meshSession, bool enabled)
{
    if (!scene) return;
    FinishSkeletonPaintStroke(session);
    session.enabled = enabled;
    session.observedSubmode = -1;
    scene->SetEditorWeightPaint(nullptr, -1);
    scene->SetEditorMeshEditPose(enabled);
    scene->SetEditorSelectedMesh(nullptr);
    if (enabled)
    {
        meshSession.enabled = false;
        SyncSkeletonEditSelection(scene, session);
    }
    else
    {
        session.skeleton = nullptr;
        session.mesh = nullptr;
        session.bindingMesh = nullptr;
        session.boneIndex = -1;
        session.mirrorBoneIndex = -1;
    }
    for (const auto& panel : m_panels)
        if (auto* view = dynamic_cast<SceneView*>(panel.get());
            view && view->GetScene() == scene)
        {
            view->AllowObjectTransform = !enabled ||
                (session.submode == 0 && session.boneTool == 0);
            view->SetEditMode(enabled ? SceneEditMode::Skeleton :
                SceneEditMode::Object);
        }
    if (m_renderer) m_renderer->MarkDirty();
}

void EditorState::FinishSkeletonPaintStroke(SkeletonEditSession& session)
{
    if (!session.painting) return;
    session.painting = false;
    if (session.strokeChanged && session.mesh)
    {
        MeshEditSession* edit = ActiveMeshEditSession();
        if (edit)
        {
            if (m_historyLimit > 0 && edit->undo.size() >= m_historyLimit)
                edit->undo.pop_front();
            if (m_historyLimit > 0) edit->undo.push_back(session.strokeBefore);
            edit->redo.clear();
            if (auto* scene = GetActiveDocumentScene(); scene && session.mesh->Owner)
                scene->TryGetObjectPath(session.mesh->Owner,
                    session.historyMeshPath);
            RecordSkeletonHistory(session,
                SkeletonEditSession::HistoryKind::Mesh);
            edit->baseline = CaptureMeshSnapshot(*session.mesh);
            edit->dirty = edit->baseline != edit->savedSnapshot;
            if (m_activeSceneAssetDocument)
                RefreshSceneAssetDocumentTitle(*m_activeSceneAssetDocument);
            if (!edit->savePath.empty())
            {
                MeshEditSession cached = *edit;
                cached.activeMesh = nullptr;
                m_meshEditCache[MeshCacheKey(edit->savePath)] = std::move(cached);
            }
        }
    }
    session.strokeChanged = false;
    session.strokeBefore.clear();
}

void EditorState::RecordSkeletonHistory(SkeletonEditSession& session,
    SkeletonEditSession::HistoryKind kind)
{
    if (!session.enabled || m_historyLimit == 0) return;
    if (kind == SkeletonEditSession::HistoryKind::Mesh)
    {
        if (m_activeSceneAssetDocument)
            m_activeSceneAssetDocument->redo.clear();
        else if (GetActiveDocumentScene() == m_scene.get())
            m_redoHistory.clear();
    }
    else if (auto* meshEdit = ActiveMeshEditSession())
        meshEdit->redo.clear();
    if (session.undoOrder.size() >= m_historyLimit)
        session.undoOrder.pop_front();
    SkeletonEditSession::HistoryAction action;
    action.kind = kind;
    action.meshPath = session.historyMeshPath;
    if (auto* meshEdit = ActiveMeshEditSession())
        action.meshSavePath = meshEdit->savePath;
    if (kind != SkeletonEditSession::HistoryKind::Mesh)
        action.assetEdits.swap(session.pendingAssetEdits);
    session.undoOrder.push_back(std::move(action));
    session.redoOrder.clear();
}

bool EditorState::ApplySkeletonHistory(bool redo)
{
    SkeletonEditSession* session = ActiveSkeletonEditSession();
    Engine::Scene::Scene* scene = GetActiveDocumentScene();
    if (!session || !session->enabled || !scene) return false;
    FinishSkeletonPaintStroke(*session);
    if (scene == m_scene.get())
    {
        TrackSceneChanges(true, false);
        CommitPendingHistoryEdit();
    }
    auto& source = redo ? session->redoOrder : session->undoOrder;
    auto& destination = redo ? session->undoOrder : session->redoOrder;
    if (source.empty()) return false;
    const auto action = source.back();
    const auto kind = action.kind;
    auto restoreMeshPointer = [&]()
    {
        MeshEditSession* edit = ActiveMeshEditSession();
        if (!edit) return;
        if (kind == SkeletonEditSession::HistoryKind::Scene &&
            !edit->savePath.empty())
        {
            for (const auto& object : scene->GetObjects())
                if (object)
                    if (auto* mesh = object->GetComponent<
                            Engine::Components::Mesh>(); mesh &&
                        MeshCacheKey(MeshSavePath(*mesh,
                            m_activeSceneAssetDocument
                                ? m_activeSceneAssetDocument->path
                                : scene == m_prefabScene.get()
                                    ? m_activePrefabPath : m_currentScenePath)) ==
                            MeshCacheKey(edit->savePath))
                    {
                        edit->activeMesh = mesh;
                        if (!edit->baseline.empty())
                            RestoreMeshSnapshot(*mesh, edit->baseline);
                        return;
                    }
            edit->activeMesh = nullptr;
            return;
        }
        if (action.meshPath.empty())
        {
            edit->activeMesh = nullptr;
            return;
        }
        auto* owner = scene->FindObjectByPath(action.meshPath);
        if (kind != SkeletonEditSession::HistoryKind::Scene &&
            edit->savePath != action.meshSavePath &&
            !action.meshSavePath.empty())
        {
            if (!edit->savePath.empty())
            {
                MeshEditSession cached = *edit;
                cached.activeMesh = nullptr;
                m_meshEditCache[MeshCacheKey(edit->savePath)] =
                    std::move(cached);
            }
            if (auto found = m_meshEditCache.find(
                    MeshCacheKey(action.meshSavePath));
                found != m_meshEditCache.end())
            {
                const bool enabled = edit->enabled;
                *edit = found->second;
                edit->enabled = enabled;
            }
        }
        edit->activeMesh = owner
            ? owner->GetComponent<Engine::Components::Mesh>() : nullptr;
        if (edit->activeMesh && !edit->baseline.empty())
            RestoreMeshSnapshot(*edit->activeMesh, edit->baseline);
    };
    auto stepScene = [&]() -> bool
    {
        if (m_activeSceneAssetDocument)
        {
            auto& document = *m_activeSceneAssetDocument;
            if (!StepSnapshotHistory(redo, document.undo, document.redo,
                document.baseline, [&](const std::string& snapshot)
                { return RestoreSceneAssetDocumentSnapshot(document, snapshot); }))
                return false;
            document.dirty = document.baseline != document.savedSnapshot;
            RefreshSceneAssetDocumentTitle(document);
            if (m_primaryHierarchy) m_primaryHierarchy->SetSelectedObject(nullptr);
            if (m_primaryProperties) m_primaryProperties->SetSelectedObject(nullptr);
        }
        else
        {
            CommitPendingHistoryEdit();
            auto& previous = redo ? m_redoHistory : m_undoHistory;
            auto& next = redo ? m_undoHistory : m_redoHistory;
            if (previous.empty()) return false;
            HistoryEntry target = std::move(previous.back());
            previous.pop_back();
            next.push_back(std::move(m_historyBaseline));
            TrimHistory();
            ApplyHistoryEntry(std::move(target), redo ? "Redo" : "Undo");
        }
        // Reloading a scene recreates Mesh components from their assets.
        // Reapply every in-memory edited mesh before stepping this action.
        for (const auto& object : scene->GetObjects())
            if (object)
                if (auto* mesh = object->GetComponent<Engine::Components::Mesh>())
                {
                    const std::string path = MeshSavePath(*mesh,
                        m_activeSceneAssetDocument
                            ? m_activeSceneAssetDocument->path
                            : scene == m_prefabScene.get()
                                ? m_activePrefabPath : m_currentScenePath);
                    if (path.empty()) continue;
                    if (auto cached = m_meshEditCache.find(MeshCacheKey(path));
                        cached != m_meshEditCache.end() &&
                        !cached->second.baseline.empty())
                        RestoreMeshSnapshot(*mesh, cached->second.baseline);
                }
        restoreMeshPointer();
        for (const auto& asset : action.assetEdits)
        {
            auto* owner = scene->FindObjectByPath(
                redo ? asset.afterPath : asset.beforePath);
            auto* mesh = owner
                ? owner->GetComponent<Engine::Components::Mesh>() : nullptr;
            if (!mesh) continue;
            const std::string& snapshot = redo ? asset.after : asset.before;
            RestoreMeshSnapshot(*mesh, snapshot);
            auto& cached = m_meshEditCache[MeshCacheKey(asset.savePath)];
            cached.activeMesh = nullptr;
            cached.savePath = asset.savePath;
            cached.baseline = snapshot;
            cached.dirty = cached.baseline != cached.savedSnapshot;
            if (auto* active = ActiveMeshEditSession(); active &&
                MeshCacheKey(active->savePath) == MeshCacheKey(asset.savePath))
            {
                active->baseline = snapshot;
                active->dirty = active->baseline != active->savedSnapshot;
            }
        }
        SyncSkeletonEditSelection(scene, *session);
        return true;
    };
    if (kind == SkeletonEditSession::HistoryKind::Scene ||
        kind == SkeletonEditSession::HistoryKind::Binding)
        if (!stepScene()) return false;
    if (kind == SkeletonEditSession::HistoryKind::Mesh ||
        kind == SkeletonEditSession::HistoryKind::Binding)
    {
        restoreMeshPointer();
        if (!ApplyMeshHistory(redo)) return false;
    }
    source.pop_back();
    destination.push_back(action);
    if (m_renderer) m_renderer->MarkDirty();
    return true;
}

void EditorState::SyncSkeletonEditSelection(Engine::Scene::Scene* scene,
    SkeletonEditSession& session)
{
    if (!scene || !session.enabled) return;
    Engine::Core::Object* selected = scene->GetSelectedObject();
    const uint64_t structureRevision = scene->GetStructureRevision();
    if (session.skeleton && selected == session.observedSelection &&
        structureRevision == session.observedStructureRevision &&
        session.submode == session.observedSubmode)
    {
        scene->SetEditorWeightPaint(session.submode == 1 ? session.mesh : nullptr,
            session.submode == 1 ? session.boneIndex : -1);
        scene->SetEditorSelectedMesh(nullptr);
        return;
    }
    Engine::Core::Object* previousSelection = session.observedSelection;
    session.observedSelection = selected;
    session.observedStructureRevision = structureRevision;
    session.observedSubmode = session.submode;
    const int previousBoneIndex = session.boneIndex;
    Engine::Components::Skeleton* chosen = nullptr;
    if (selected)
        for (auto* component : selected->Components)
            if (auto* skin = dynamic_cast<Engine::Components::SkinnedMesh*>(component))
            { chosen = skin->ResolveSkeleton(); break; }
    if (!chosen)
        for (Engine::Core::Object* parent = selected; parent && !chosen;
            parent = parent->Parent)
            for (auto* component : parent->Components)
                if (auto* skeleton = dynamic_cast<Engine::Components::Skeleton*>(component))
                { chosen = skeleton; break; }
    // A rig's model root and its joint hierarchy need not contain the
    // Skeleton component. Resolve a selected joint from the palette itself.
    if (selected && (!chosen || chosen->Owner != selected))
    {
        Engine::Components::Skeleton* jointSkeleton = nullptr;
        for (const auto& object : scene->GetObjects())
        {
            if (!object || jointSkeleton) continue;
            for (auto* component : object->Components)
                if (auto* skeleton = dynamic_cast<Engine::Components::Skeleton*>(component))
                {
                    const auto& joints = skeleton->ResolveJoints();
                    if (std::find(joints.begin(), joints.end(), selected) != joints.end())
                    { jointSkeleton = skeleton; break; }
                }
        }
        if (jointSkeleton) chosen = jointSkeleton;
    }
    if (!chosen && selected)
        for (const auto& object : scene->GetObjects())
            if (object && object.get() == selected)
                for (auto* component : object->Components)
                    if (auto* skin = dynamic_cast<Engine::Components::SkinnedMesh*>(component))
                    { chosen = skin->ResolveSkeleton(); break; }
    if (!chosen && session.skeleton)
        for (const auto& object : scene->GetObjects())
            if (object)
                for (auto* component : object->Components)
                    if (component == session.skeleton)
                    { chosen = session.skeleton; break; }
    if (!chosen)
        for (const auto& object : scene->GetObjects())
            if (object)
                for (auto* component : object->Components)
                    if (auto* skeleton = dynamic_cast<Engine::Components::Skeleton*>(component))
                    { chosen = skeleton; break; }
    if (chosen != session.skeleton)
    {
        FinishSkeletonPaintStroke(session);
        session.skeleton = chosen;
        session.lockedBones.clear();
        session.boneIndex = -1;
        session.mirrorBoneIndex = -1;
        session.mesh = nullptr;
        session.adjacency.reset();
        session.surfaceTopology.reset();
        session.observedBindTransforms.clear();
        if (chosen)
        {
            SkeletonBindPose::Restore(*chosen);
            for (auto* joint : chosen->ResolveJoints())
                if (joint)
                    session.observedBindTransforms.push_back(
                        joint->transform.GetWorldMatrix());
        }
    }
    if (chosen)
    {
        const auto& joints = chosen->ResolveJoints();
        session.lockedBones.resize(joints.size(), false);
        if (selected)
            for (size_t i = 0; i < joints.size(); ++i)
                if (joints[i] == selected)
                { session.boneIndex = static_cast<int>(i); break; }
        if (session.boneIndex < 0 ||
            session.boneIndex >= static_cast<int>(joints.size()) ||
            !joints[session.boneIndex])
        {
            session.boneIndex = -1;
            for (size_t i = 0; i < joints.size(); ++i)
                if (joints[i])
                { session.boneIndex = static_cast<int>(i); break; }
        }
        if (selected != previousSelection ||
            session.boneIndex != previousBoneIndex ||
            session.boneName[0] == '\0')
        {
            const std::string name = session.boneIndex >= 0 &&
                joints[session.boneIndex] ? joints[session.boneIndex]->name
                : std::string{};
            const size_t count = std::min(name.size(),
                sizeof(session.boneName) - 1);
            std::memcpy(session.boneName, name.data(), count);
            session.boneName[count] = '\0';
        }
        Engine::Components::Mesh* mesh = nullptr;
        Engine::Components::Mesh* firstMesh = nullptr;
        for (const auto& object : scene->GetObjects())
            if (object)
                if (auto* skin = object->GetComponent<Engine::Components::SkinnedMesh>();
                    skin && skin->ResolveSkeleton() == chosen)
                {
                    auto* candidate = object->GetComponent<Engine::Components::Mesh>();
                    if (!candidate || candidate->GetVertices().empty()) continue;
                    if (!firstMesh) firstMesh = candidate;
                    if (candidate == session.mesh || object.get() == selected)
                    { mesh = candidate; break; }
                }
        session.mesh = mesh ? mesh : firstMesh;
    }
    scene->SetEditorMeshEditPose(true);
    scene->SetEditorWeightPaint(session.submode == 1 ? session.mesh : nullptr,
        session.submode == 1 ? session.boneIndex : -1);
    scene->SetEditorSelectedMesh(nullptr);
}

void EditorState::CommitSkeletonEdit(Engine::Scene::Scene* scene)
{
    if (m_activeSceneAssetDocument && m_activeSceneAssetDocument->scene.get() == scene)
        TrackSceneChanges(true, false);
    else if (scene == m_prefabScene.get())
        SetPrefabDirty(true);
    else
    {
        MarkSceneEdited();
        TrackSceneChanges(true, false);
    }
    if (m_renderer) m_renderer->MarkDirty();
}

bool EditorState::ApplySkinBindingAction(Engine::Scene::Scene* scene,
    SkeletonEditSession& session, int action)
{
    using namespace SkinBindingWeights;
    auto fail = [&](std::string message)
    { session.bindingStatus = std::move(message); return false; };
    auto* mesh = session.bindingMesh;
    auto* skeleton = session.skeleton;
    if (!scene || !mesh || !mesh->Owner || !skeleton)
        return fail("Select a mesh and skeleton first.");
    const auto& targetJoints = skeleton->ResolveJoints();
    if (targetJoints.empty()) return fail("The target skeleton has no joints.");
    if (skeleton->inverseBindMatrices.size() != targetJoints.size())
        return fail("The skeleton has missing inverse bind matrices; repair its bind pose first.");
    auto* model = skeleton->ResolveModel();
    auto* root = model && model->Owner ? model->Owner : skeleton->Owner;
    if (!root || std::abs(glm::determinant(
        root->transform.GetWorldMatrix())) < 1e-8f)
        return fail("Skeleton root transform has zero scale.");
    std::vector<bool> resolved(targetJoints.size());
    std::vector<glm::vec3> positions(targetJoints.size());
    const glm::mat4 meshWorld = mesh->Owner->transform.GetWorldMatrix();
    if (std::abs(glm::determinant(meshWorld)) < 1e-8f)
        return fail("Mesh transform has zero scale.");
    const glm::mat4 inverseMesh = glm::inverse(meshWorld);
    for (size_t i = 0; i < targetJoints.size(); ++i)
    {
        resolved[i] = targetJoints[i] != nullptr;
        if (resolved[i])
            positions[i] = glm::vec3(inverseMesh * glm::vec4(
                targetJoints[i]->transform.GetWorldMatrix()[3]));
    }
    auto* skin = mesh->Owner->GetComponent<Engine::Components::SkinnedMesh>();
    if (skin && skin->meshReference.IsAssigned() &&
        Engine::Core::ResolveComponentReference<Engine::Components::Mesh>(
            skin->Owner, skin->meshReference) != mesh)
        return fail("This object's Skin component references another mesh.");
    auto* sourceSkeleton = skin ? skin->ResolveSkeleton() : nullptr;
    const bool animationRetargetNeeded = sourceSkeleton &&
        sourceSkeleton != skeleton;
    const std::string path = mesh->GetFilePath().empty() ? std::string{}
        : MeshCacheKey(mesh->GetFilePath());
    bool sharedMeshAsset = false;
    for (const auto& object : scene->GetObjects())
        if (object)
            for (auto* component : object->Components)
                if (auto* otherMesh = dynamic_cast<Engine::Components::Mesh*>(component);
                    otherMesh && otherMesh != mesh && !path.empty() &&
                    !otherMesh->GetFilePath().empty() &&
                    MeshCacheKey(otherMesh->GetFilePath()) == path)
                    sharedMeshAsset = true;
    for (const auto& object : scene->GetObjects())
        if (object)
            for (auto* component : object->Components)
                if (auto* other = dynamic_cast<Engine::Components::SkinnedMesh*>(component);
                    other && other != skin)
                {
                    auto* otherMesh = other->meshReference.IsAssigned()
                        ? Engine::Core::ResolveComponentReference<Engine::Components::Mesh>(
                            other->Owner, other->meshReference)
                        : other->Owner->GetComponent<Engine::Components::Mesh>();
                    if (otherMesh && (otherMesh == mesh ||
                        (!path.empty() && !otherMesh->GetFilePath().empty() &&
                            MeshCacheKey(otherMesh->GetFilePath()) == path)) &&
                        other->ResolveSkeleton() != skeleton)
                        return fail("Shared mesh is bound to another skeleton; copy its mesh asset before rebinding.");
                }

    std::vector<Vertex> vertices(mesh->GetVertices());
    bool editWeights = false;
    if (action == 0) // Preserve weights and remap the palette if necessary.
    {
        if (sourceSkeleton && sourceSkeleton != skeleton)
        {
            std::vector<int> mapping(sourceSkeleton->jointNodes.size(), -1);
            const auto& sourceJoints = sourceSkeleton->ResolveJoints();
            std::unordered_map<std::string, int> names;
            for (size_t i = 0; i < targetJoints.size(); ++i)
                if (targetJoints[i])
                {
                    auto [it, inserted] = names.emplace(targetJoints[i]->name,
                        static_cast<int>(i));
                    if (!inserted) it->second = -1;
                }
            for (size_t i = 0; i < sourceJoints.size(); ++i)
                if (sourceJoints[i])
                {
                    for (size_t target = 0; target < targetJoints.size(); ++target)
                        if (sourceJoints[i] == targetJoints[target])
                        { mapping[i] = static_cast<int>(target); break; }
                    if (mapping[i] < 0)
                    {
                        const auto found = names.find(sourceJoints[i]->name);
                        if (found != names.end()) mapping[i] = found->second;
                    }
                }
            if (!Remap(vertices, mapping))
                return fail("A weighted source joint has no unique target match. Repair the rig mapping or regenerate weights.");
            editWeights = true;
        }
        else if (!sourceSkeleton && skin && skin->skinIndex >= 0)
            return fail("The current skeleton reference is missing; restore it or regenerate weights.");
        const Diagnostics diagnosis = Inspect(vertices, resolved);
        if (diagnosis.missingJoints)
            return fail("Weights reference missing target joints; repair or regenerate before binding.");
    }
    else if (action == 1)
    {
        if (std::find(resolved.begin(), resolved.end(), false) != resolved.end())
            return fail("Resolve every target joint before generating weights.");
        if (!Generate(vertices, positions))
            return fail("Could not generate initial weights.");
        editWeights = true;
    }
    else if (action == 2)
    {
        if (sourceSkeleton != skeleton)
            return fail("Bind this mesh to the selected skeleton before repairing weights.");
        if (!Repair(vertices, resolved, positions))
            return fail("No resolved target joints are available for repair.");
        editWeights = true;
    }
    else if (action == 3 || action == 4)
    {
        if (sourceSkeleton != skeleton)
            return fail("Bind this mesh to the selected skeleton before assigning vertices.");
        if (session.boneIndex < 0 ||
            session.boneIndex >= static_cast<int>(resolved.size()) ||
            !resolved[session.boneIndex])
            return fail("Select a resolved bone first.");
        std::vector<uint32_t> selection;
        if (action == 4)
        {
            auto* edit = ActiveMeshEditSession();
            if (edit && edit->activeMesh == mesh && edit->selectionMode == 0)
                selection = edit->selectedElements;
        }
        else
        {
            std::stringstream stream(session.bindingVertexIds);
            std::string token;
            while (std::getline(stream, token, ','))
            {
                const auto dash = token.find('-');
                auto parse = [](std::string_view value, uint32_t& result)
                {
                    while (!value.empty() && std::isspace(
                        static_cast<unsigned char>(value.front()))) value.remove_prefix(1);
                    while (!value.empty() && std::isspace(
                        static_cast<unsigned char>(value.back()))) value.remove_suffix(1);
                    const auto parsed = std::from_chars(value.data(),
                        value.data() + value.size(), result);
                    return !value.empty() && parsed.ec == std::errc{} &&
                        parsed.ptr == value.data() + value.size();
                };
                uint32_t first = 0, last = 0;
                if (!parse(std::string_view(token).substr(0, dash), first) ||
                    (dash != std::string::npos &&
                        !parse(std::string_view(token).substr(dash + 1), last)))
                    return fail("Enter vertex IDs as comma-separated numbers or ranges.");
                if (dash == std::string::npos) last = first;
                if (first > last || last >= vertices.size())
                    return fail("Vertex selection is outside this mesh.");
                for (uint32_t id = first; id <= last; ++id)
                    selection.push_back(id);
            }
        }
        if (!Assign(vertices, selection,
            static_cast<size_t>(session.boneIndex)))
            return fail("Select at least one valid vertex.");
        editWeights = true;
    }
    else return fail("Unknown skin binding action.");

    if (editWeights && vertices.size() == mesh->GetVertices().size() &&
        std::memcmp(vertices.data(), mesh->GetVertices().data(),
            vertices.size() * sizeof(Vertex)) == 0)
        editWeights = false;
    const bool forkMeshAsset = editWeights && !mesh->GetFilePath().empty() &&
        (action == 1 || animationRetargetNeeded);
    if (editWeights && sharedMeshAsset && !forkMeshAsset)
        return fail("This mesh asset is shared by another object; make a separate mesh asset before changing its weights.");

    std::string forkSavePath;
    if (forkMeshAsset)
    {
        const std::filesystem::path original(MeshSavePath(*mesh,
            m_activeSceneAssetDocument ? m_activeSceneAssetDocument->path
                : scene == m_prefabScene.get() ? m_activePrefabPath
                : m_currentScenePath));
        if (original.empty())
            return fail("Save the scene before creating a separate skin mesh.");
        std::error_code pathError;
        for (unsigned suffix = 1; suffix < 10000; ++suffix)
        {
            const auto candidate = original.parent_path() /
                (original.stem().string() + "_skin_" +
                    std::to_string(skeleton->skinIndex) + "_" +
                    std::to_string(suffix) + ".mesh");
            if (!std::filesystem::exists(candidate, pathError) && !pathError)
            { forkSavePath = candidate.string(); break; }
            if (pathError) break;
        }
        if (forkSavePath.empty())
            return fail("Could not choose a new path for the rebound mesh asset.");
    }

    if (editWeights)
    {
        auto* edit = ActiveMeshEditSession();
        if (!edit) return fail("Mesh edit history is unavailable.");
        if (edit->activeMesh != mesh && edit->dirty)
            return fail("Save the other edited mesh before changing this one.");
        const std::string before = CaptureMeshSnapshot(*mesh);
        if (!mesh->UpdateAuthoredVertices(std::move(vertices)))
            return fail("Mesh rejected the authored weight change.");
        if (edit->activeMesh != mesh)
        {
            if (edit->activeMesh && !edit->savePath.empty())
            {
                MeshEditSession cached = *edit;
                cached.activeMesh = nullptr;
                m_meshEditCache[MeshCacheKey(edit->savePath)] =
                    std::move(cached);
            }
            edit->activeMesh = mesh;
            edit->savePath = MeshSavePath(*mesh,
                m_activeSceneAssetDocument ? m_activeSceneAssetDocument->path
                    : scene == m_prefabScene.get() ? m_activePrefabPath
                    : m_currentScenePath);
            edit->baseline = before;
            edit->savedSnapshot = before;
            edit->undo.clear(); edit->redo.clear(); edit->dirty = false;
        }
        if (forkMeshAsset)
        {
            if (!edit->savePath.empty())
                m_meshEditCache.erase(MeshCacheKey(edit->savePath));
            edit->savePath = forkSavePath;
        }
        if (m_historyLimit > 0 && edit->undo.size() >= m_historyLimit)
            edit->undo.pop_front();
        if (m_historyLimit > 0) edit->undo.push_back(before);
        edit->redo.clear();
        if (mesh->Owner)
            scene->TryGetObjectPath(mesh->Owner, session.historyMeshPath);
        session.pendingBindingMeshHistory = true;
        edit->baseline = CaptureMeshSnapshot(*mesh);
        edit->dirty = edit->baseline != edit->savedSnapshot;
        if (!edit->savePath.empty())
        {
            MeshEditSession cached = *edit;
            cached.activeMesh = nullptr;
            m_meshEditCache[MeshCacheKey(edit->savePath)] = std::move(cached);
        }
        if (m_activeSceneAssetDocument)
            RefreshSceneAssetDocumentTitle(*m_activeSceneAssetDocument);
    }
    if (action <= 1)
    {
        const bool newBinding = !skin || sourceSkeleton != skeleton;
        if (!skin) skin = mesh->Owner->AddComponent<Engine::Components::SkinnedMesh>();
        skin->meshReference = Engine::Core::CaptureComponentReference(mesh, "Mesh");
        skin->skeletonReference = Engine::Core::CaptureComponentReference(
            skeleton, "Skeleton");
        skin->skinIndex = static_cast<int>(skeleton->skinIndex);
        if (newBinding)
            skin->bindMeshToModel = glm::inverse(
                root->transform.GetWorldMatrix()) * meshWorld;
        skin->MarkConfigurationDirty();
        skin->Start();
    }
    CommitSkeletonEdit(scene);
    if (session.pendingBindingMeshHistory)
    {
        RecordSkeletonHistory(session, SkeletonEditSession::HistoryKind::Mesh);
        session.pendingBindingMeshHistory = false;
    }
    session.bindingStatus = animationRetargetNeeded
        ? "Weights remapped; animation clips still target the original rig and may need retargeting."
        : action == 0
        ? "Bound with existing weights. Animation node references were preserved."
        : forkMeshAsset
        ? "Weights updated; save to create a separate mesh asset for this binding."
        : "Weights updated. Save binding and weights to persist both assets.";
    return true;
}

bool EditorState::ApplySkeletonBoneAction(Engine::Scene::Scene* scene,
    SkeletonEditSession& session, int action)
{
    auto* skeleton = session.skeleton;
    if (!scene || !skeleton) return false;
    auto* model = skeleton->ResolveModel();
    if (!model && skeleton->Owner)
        model = skeleton->Owner->AddComponent<Engine::Components::Model>();
    if (!model || !model->Owner)
    { session.error = "A model root is required to edit bones."; return false; }
    if ((!skeleton->jointNodes.empty() || action != 0) &&
        !ValidateBoneReferences(*skeleton, session.error))
        return false;
    session.pendingAssetEdits.clear();
    const auto& bones = skeleton->ResolveJoints();
    Engine::Core::Object* selected = session.boneIndex >= 0 &&
        session.boneIndex < static_cast<int>(bones.size()) &&
        bones[session.boneIndex] ? bones[session.boneIndex] : nullptr;
    const auto captureNodes = [&]()
    {
        const auto& nodes = model->ResolveNodes();
        return std::vector<Engine::Core::Object*>(nodes.begin(), nodes.end());
    };
    const auto rebindNodes = [&](const std::vector<Engine::Core::Object*>& nodes,
        Engine::Core::Object* removed = nullptr)
    {
        for (size_t node = 0; node < nodes.size(); ++node)
            if (nodes[node] && nodes[node] != removed)
                model->BindNode(static_cast<unsigned>(node), nodes[node]);
    };
    if (action == 0) // Add a child bone.
    {
        if (std::abs(glm::determinant(
                model->Owner->transform.GetWorldMatrix())) < 1e-8f)
        { session.error = "The model root has zero scale."; return false; }
        auto* parent = selected ? selected : model->Owner;
        bool insideModel = false;
        for (auto* ancestor = parent; ancestor; ancestor = ancestor->Parent)
            if (ancestor == model->Owner) { insideModel = true; break; }
        if (!insideModel)
        { session.error = "Selected bone is outside the model root."; return false; }
        auto* object = scene->AddObject("Bone " +
            std::to_string(skeleton->jointNodes.size() + 1));
        scene->MoveObject(object, parent, Engine::Scene::Scene::ObjectPlacement::AsChild);
        object->transform.position = { 0.f, .2f, 0.f };
        object->transform.NotifyEditorTransformChanged(
            Engine::Components::Transform::EditorAll);
        const unsigned node = static_cast<unsigned>(model->GetNodeCount());
        model->BindNode(node, object);
        auto* bone = object->AddComponent<Engine::Components::AnimationBone>();
        bone->skinIndex = skeleton->skinIndex;
        bone->nodeIndex = node;
        bone->paletteIndex = static_cast<int>(skeleton->jointNodes.size());
        bone->parentPaletteIndex = selected ? session.boneIndex : -1;
        bone->hierarchyRoot = !selected;
        skeleton->jointNodes.push_back(node);
        session.lockedBones.push_back(false);
        const glm::mat4 modelWorld = model->Owner->transform.GetWorldMatrix();
        skeleton->inverseBindMatrices.push_back(glm::inverse(
            glm::inverse(modelWorld) * object->transform.GetWorldMatrix()));
        session.boneIndex = bone->paletteIndex;
        scene->SetSelectedObject(object);
        if (m_primaryHierarchy) m_primaryHierarchy->SetSelectedObject(object);
        if (m_primaryProperties) m_primaryProperties->SetSelectedObject(object);
    }
    else if (action == 1) // Remove an unweighted leaf bone.
    {
        if (!selected || selected == skeleton->Owner ||
            selected == model->Owner || !selected->Children.empty())
        { session.error = "Remove child objects first; the rig root cannot be removed.";
            return false; }
        for (const auto& object : scene->GetObjects())
            if (object)
                if (auto* skin = object->GetComponent<Engine::Components::SkinnedMesh>();
                    skin && skin->ResolveSkeleton() == skeleton)
                    if (auto* mesh = object->GetComponent<Engine::Components::Mesh>())
                        for (const auto& vertex : mesh->GetVertices())
                            if (BoneWeight(vertex, session.boneIndex) > 1e-6f)
                            {
                                session.error = "Clear this bone's weights before removing it.";
                                return false;
                            }
        struct MeshRemap
        {
            Engine::Components::Mesh* mesh = nullptr;
            Engine::Components::SkinnedMesh* skin = nullptr;
            std::vector<Engine::Components::Mesh::Vertex> vertices;
            std::string before;
            Engine::Scene::Scene::ObjectPath beforePath;
        };
        std::vector<MeshRemap> remaps;
        for (const auto& object : scene->GetObjects())
            if (object)
                if (auto* skin = object->GetComponent<Engine::Components::SkinnedMesh>();
                    skin && skin->ResolveSkeleton() == skeleton)
                    if (auto* mesh = object->GetComponent<Engine::Components::Mesh>())
                    {
                        if (MeshSavePath(*mesh,
                                m_activeSceneAssetDocument
                                    ? m_activeSceneAssetDocument->path
                                    : scene == m_prefabScene.get()
                                        ? m_activePrefabPath : m_currentScenePath)
                            .empty())
                        { session.error = "Save the scene before changing a bound mesh palette.";
                            return false; }
                        if (!mesh->GetFilePath().empty())
                            for (const auto& otherObject : scene->GetObjects())
                                if (otherObject && otherObject.get() != object.get())
                                    if (auto* otherMesh = otherObject->GetComponent<
                                            Engine::Components::Mesh>();
                                        otherMesh && !otherMesh->GetFilePath().empty() &&
                                        MeshCacheKey(otherMesh->GetFilePath()) ==
                                            MeshCacheKey(mesh->GetFilePath()))
                                    {
                                        auto* otherSkin = otherObject->GetComponent<
                                            Engine::Components::SkinnedMesh>();
                                        if (!otherSkin ||
                                            otherSkin->ResolveSkeleton() != skeleton)
                                        { session.error = "This mesh asset is also used outside the rig; copy it before deleting a bone.";
                                            return false; }
                                    }
                        MeshRemap remap{ mesh, skin,
                            std::vector<Engine::Components::Mesh::Vertex>(
                                mesh->GetVertices()), CaptureMeshSnapshot(*mesh) };
                        scene->TryGetObjectPath(mesh->Owner, remap.beforePath);
                        if (SkinBindingWeights::Inspect(remap.vertices,
                                std::vector<bool>(bones.size(), true)).missingJoints)
                        { session.error = "Repair invalid joint references in the mesh before deleting a bone.";
                            return false; }
                        if (!SkinBindingWeights::RemoveUnweightedJoint(
                                remap.vertices, session.boneIndex))
                        { session.error = "Clear this bone's weights before removing it.";
                            return false; }
                        if (skin->joints.size() != skin->weights.size())
                        { session.error = "Legacy skin joint and weight counts differ.";
                            return false; }
                        for (size_t vertex = 0; vertex < skin->joints.size() &&
                            vertex < skin->weights.size(); ++vertex)
                            for (int slot = 0; slot < 4; ++slot)
                            {
                                if (skin->weights[vertex][slot] > 1e-6f &&
                                    skin->joints[vertex][slot] >= bones.size())
                                { session.error = "Repair invalid legacy joint references before deleting a bone.";
                                    return false; }
                                if (skin->joints[vertex][slot] ==
                                        static_cast<unsigned>(session.boneIndex) &&
                                    skin->weights[vertex][slot] > 1e-6f)
                                { session.error = "Clear this bone's legacy weights before removing it.";
                                    return false; }
                            }
                        remaps.push_back(std::move(remap));
                    }
        for (size_t i = 0; i < remaps.size(); ++i)
            if (!remaps[i].mesh->UpdateAuthoredVertices(
                    std::move(remaps[i].vertices)))
            {
                for (size_t restored = 0; restored < i; ++restored)
                    RestoreMeshSnapshot(*remaps[restored].mesh,
                        remaps[restored].before);
                session.error = "A dependent mesh rejected the palette remap.";
                return false;
            }
        const auto retained = captureNodes();
        const std::vector<Engine::Core::Object*> oldJoints(bones.begin(), bones.end());
        const unsigned removedNode = skeleton->jointNodes[session.boneIndex];
        scene->SetSelectedObject(nullptr);
        if (m_primaryHierarchy) m_primaryHierarchy->SetSelectedObject(nullptr);
        if (m_primaryProperties) m_primaryProperties->SetSelectedObject(nullptr);
        scene->RemoveObject(selected);
        skeleton->jointNodes.erase(skeleton->jointNodes.begin() + session.boneIndex);
        if (session.boneIndex < static_cast<int>(session.lockedBones.size()))
            session.lockedBones.erase(session.lockedBones.begin() + session.boneIndex);
        skeleton->inverseBindMatrices.erase(
            skeleton->inverseBindMatrices.begin() + session.boneIndex);
        rebindNodes(retained, selected);
        model->UnbindNode(removedNode);
        for (size_t palette = 0; palette < oldJoints.size(); ++palette)
            if (palette != static_cast<size_t>(session.boneIndex))
                if (auto* bone = oldJoints[palette]->GetComponent<
                    Engine::Components::AnimationBone>())
                {
                    if (bone->paletteIndex > session.boneIndex)
                        --bone->paletteIndex;
                    if (bone->parentPaletteIndex > session.boneIndex)
                        --bone->parentPaletteIndex;
                }
        for (auto& remap : remaps)
        {
            for (auto& joint : remap.skin->joints)
                for (int slot = 0; slot < 4; ++slot)
                    if (joint[slot] > static_cast<unsigned>(session.boneIndex))
                        --joint[slot];
            SkeletonEditSession::HistoryAction::AssetEdit asset;
            asset.beforePath = remap.beforePath;
            scene->TryGetObjectPath(remap.mesh->Owner, asset.afterPath);
            asset.savePath = MeshSavePath(*remap.mesh,
                m_activeSceneAssetDocument ? m_activeSceneAssetDocument->path
                    : scene == m_prefabScene.get() ? m_activePrefabPath
                    : m_currentScenePath);
            asset.before = std::move(remap.before);
            asset.after = CaptureMeshSnapshot(*remap.mesh);
            session.pendingAssetEdits.push_back(asset);
            if (!asset.savePath.empty())
            {
                auto& cached = m_meshEditCache[MeshCacheKey(asset.savePath)];
                cached.activeMesh = nullptr;
                cached.savePath = asset.savePath;
                cached.baseline = asset.after;
                if (cached.savedSnapshot.empty())
                    cached.savedSnapshot = asset.before;
                cached.dirty = cached.baseline != cached.savedSnapshot;
            }
        }
        session.boneIndex = skeleton->jointNodes.empty() ? -1 : 0;
    }
    else if (action == 2) // Commit current default pose as the bind pose.
    {
        // The common post-action update below handles this operation.
    }
    else if (action == 3) // Duplicate one joint with a new palette entry.
    {
        if (!selected)
        { session.error = "Select a bone to duplicate."; return false; }
        auto* parent = selected->Parent ? selected->Parent : model->Owner;
        auto* copy = scene->AddObject(selected->name + " Copy");
        if (!scene->MoveObject(copy, parent,
                Engine::Scene::Scene::ObjectPlacement::AsChild))
        { scene->RemoveObject(copy); session.error = "Could not duplicate this bone.";
            return false; }
        copy->transform.position = selected->transform.position;
        copy->transform.rotation = selected->transform.rotation;
        copy->transform.scale = selected->transform.scale;
        copy->transform.NotifyEditorTransformChanged(
            Engine::Components::Transform::EditorAll);
        const unsigned node = static_cast<unsigned>(model->GetNodeCount());
        model->BindNode(node, copy);
        auto* bone = copy->AddComponent<Engine::Components::AnimationBone>();
        bone->skinIndex = skeleton->skinIndex;
        bone->nodeIndex = node;
        bone->paletteIndex = static_cast<int>(skeleton->jointNodes.size());
        bone->parentPaletteIndex = selected->GetComponent<
            Engine::Components::AnimationBone>()->parentPaletteIndex;
        bone->hierarchyRoot = bone->parentPaletteIndex < 0;
        skeleton->jointNodes.push_back(node);
        session.lockedBones.push_back(false);
        skeleton->inverseBindMatrices.push_back(glm::mat4(1.f));
        session.boneIndex = bone->paletteIndex;
        scene->SetSelectedObject(copy);
        if (m_primaryHierarchy) m_primaryHierarchy->SetSelectedObject(copy);
        if (m_primaryProperties) m_primaryProperties->SetSelectedObject(copy);
    }
    else if (action == 4) // Rename.
    {
        if (!selected || session.boneName[0] == '\0')
        { session.error = "Enter a bone name."; return false; }
        selected->name = session.boneName;
    }
    else if (action == 5) // Reparent, preserving the world bind pose.
    {
        if (!selected || selected == model->Owner || selected == skeleton->Owner)
        { session.error = "Select a movable bone."; return false; }
        const int targetIndex = session.reparentBoneIndex;
        if (targetIndex < -1 || targetIndex >= static_cast<int>(bones.size()))
        { session.error = "Choose a valid parent bone."; return false; }
        auto* parent = targetIndex < 0 ? model->Owner : bones[targetIndex];
        if (!parent || parent == selected)
        { session.error = "A bone cannot parent itself."; return false; }
        for (auto* ancestor = parent; ancestor; ancestor = ancestor->Parent)
            if (ancestor == selected)
            { session.error = "Reparenting would create a bone cycle.";
                return false; }
        const auto retained = captureNodes();
        if (!scene->MoveObject(selected, parent,
                Engine::Scene::Scene::ObjectPlacement::AsChild))
        { session.error = "Could not reparent this bone."; return false; }
        rebindNodes(retained);
        auto* bone = selected->GetComponent<Engine::Components::AnimationBone>();
        bone->parentPaletteIndex = targetIndex;
        bone->hierarchyRoot = targetIndex < 0;
        selected->transform.NotifyEditorTransformChanged(
            Engine::Components::Transform::EditorAll);
    }
    else if (action == 6) // Mirror across the model's X plane.
    {
        if (!selected)
        { session.error = "Select a bone to mirror."; return false; }
        const glm::mat4 modelWorld = model->Owner->transform.GetWorldMatrix();
        if (std::abs(glm::determinant(modelWorld)) < 1e-8f)
        { session.error = "The model root has zero scale."; return false; }
        glm::mat4 reflection(1.f);
        reflection[0][0] = -1.f;
        const glm::mat4 mirror = modelWorld * reflection *
            glm::inverse(modelWorld);
        std::vector<std::pair<Engine::Core::Object*, glm::mat4>> branch;
        for (auto* joint : bones)
            if (joint)
                for (auto* ancestor = joint; ancestor; ancestor = ancestor->Parent)
                    if (ancestor == selected)
                    { branch.push_back({ joint,
                        mirror * joint->transform.GetWorldMatrix() * reflection });
                        break; }
        const auto depth = [](Engine::Core::Object* object)
        {
            int count = 0;
            for (; object; object = object->Parent) ++count;
            return count;
        };
        std::sort(branch.begin(), branch.end(), [&](const auto& a, const auto& b)
            { return depth(a.first) < depth(b.first); });
        for (const auto& [joint, world] : branch)
        {
            const glm::mat4 parentWorld = joint->Parent
                ? joint->Parent->transform.GetWorldMatrix() : glm::mat4(1.f);
            if (std::abs(glm::determinant(parentWorld)) < 1e-8f)
            { session.error = "A parent transform has zero scale."; return false; }
            const glm::mat4 local = glm::inverse(parentWorld) * world;
            glm::vec3 scale, position, skew;
            glm::vec4 perspective;
            glm::quat rotation;
            if (!glm::decompose(local, scale, rotation, position, skew,
                    perspective))
            { session.error = "Could not mirror this bone transform.";
                return false; }
            joint->transform.position = position;
            joint->transform.rotation = glm::eulerAngles(glm::normalize(rotation));
            joint->transform.scale = scale;
            joint->transform.NotifyEditorTransformChanged(
                Engine::Components::Transform::EditorAll);
        }
    }
    else
    { session.error = "Unknown bone action."; return false; }
    if (!SkeletonBindPose::Commit(*scene, *skeleton, session.error))
        return false;
    session.error.clear();
    CommitSkeletonEdit(scene);
    return true;
}


void EditorState::RefreshSkeletonBindPose(SkeletonEditSession& session)
{
    auto* skeleton = session.skeleton;
    if (!skeleton || session.submode != 0) return;
    const auto& joints = skeleton->ResolveJoints();
    if (joints.empty() || joints.size() != skeleton->jointNodes.size()) return;
    std::vector<glm::mat4> current;
    current.reserve(joints.size());
    for (auto* joint : joints)
    {
        if (!joint) return;
        current.push_back(joint->transform.GetWorldMatrix());
    }
    bool moved = current.size() != session.observedBindTransforms.size();
    if (!moved)
        moved = std::memcmp(current.data(),
            session.observedBindTransforms.data(),
            current.size() * sizeof(glm::mat4)) != 0;
    if (moved)
    {
        auto* scene = skeleton->Owner ? skeleton->Owner->GetScene() : nullptr;
        if (!scene || !SkeletonBindPose::Commit(*scene, *skeleton,
                session.error))
            return;
        if (m_renderer) m_renderer->MarkDirty();
    }
    session.observedBindTransforms = std::move(current);
}

bool EditorState::HandleSkeletonViewport(IEditorUi& ui,
    const EditorUiViewportInput& input, Engine::Scene::Scene* scene,
    SkeletonEditSession& session)
{
    if (!session.enabled || !scene) return false;
    SyncSkeletonEditSelection(scene, session);
    if (session.submode == 0)
    {
        if (session.skeleton)
            EditorGizmoSystem::DrawSkeletonOverlay(*scene, *session.skeleton,
                ui, input, session.boneIndex);
        RefreshSkeletonBindPose(session);
        return false;
    }
    if (session.submode == 1 && session.skeleton)
        EditorGizmoSystem::DrawSkeletonOverlay(*scene, *session.skeleton,
            ui, input, session.boneIndex);
    if (session.submode != 1 || !session.mesh || session.boneIndex < 0 ||
        input.available.x < 1.f || input.available.y < 1.f)
        return false;
    auto* camera = scene->editorCamera.GetComponent<Engine::Components::Camera>();
    if (session.boneIndex < static_cast<int>(session.lockedBones.size()) &&
        session.lockedBones[session.boneIndex]) return false;
    if (session.mirrorBoneIndex >= 0)
    {
        const auto& joints = session.skeleton->ResolveJoints();
        if (session.mirrorBoneIndex == session.boneIndex ||
            session.mirrorBoneIndex >= static_cast<int>(joints.size()) ||
            !joints[session.mirrorBoneIndex] ||
            (session.mirrorBoneIndex < static_cast<int>(session.lockedBones.size()) &&
                session.lockedBones[session.mirrorBoneIndex]))
        {
            session.error = "Choose a different, unlocked mirror target bone.";
            return false;
        }
    }
    if (!camera || !session.mesh->Owner) return false;
    const auto& source = session.mesh->GetVertices();
    if (source.empty()) return false;
    const float radius = std::max(session.brushRadius, 2.f);
    const auto mouse = input.mousePosInViewport;
    if (session.brushShape == 0)
    {
        ui.DrawViewportCircle(mouse, radius, { 0.f, 0.f, 0.f, .8f }, false);
        ui.DrawViewportCircle(mouse, radius - 1.f,
            { 1.f, .9f, .3f, .95f }, false);
    }
    else
    {
        const EditorUiVec2 a{ mouse.x - radius, mouse.y - radius };
        const EditorUiVec2 b{ mouse.x + radius, mouse.y - radius };
        const EditorUiVec2 c{ mouse.x + radius, mouse.y + radius };
        const EditorUiVec2 d{ mouse.x - radius, mouse.y + radius };
        const EditorUiColor line{ 1.f, .9f, .3f, .95f };
        ui.DrawViewportLine(a, b, line, 1.f);
        ui.DrawViewportLine(b, c, line, 1.f);
        ui.DrawViewportLine(c, d, line, 1.f);
        ui.DrawViewportLine(d, a, line, 1.f);
    }
    if ((input.leftReleased || !input.leftDown) && session.painting)
    {
        FinishSkeletonPaintStroke(session);
        return true;
    }
    if (!input.hovered || input.rightDown || input.middleDown ||
        !input.leftDown) return session.painting;
    if (!session.painting)
    {
        if (!input.leftClicked) return false;
        session.painting = true;
        session.strokeChanged = false;
        session.strokeBefore = CaptureMeshSnapshot(*session.mesh);
        session.lastPaintPosition = { -10000.f, -10000.f };
        MeshEditSession* edit = ActiveMeshEditSession();
        if (edit && edit->activeMesh != session.mesh)
        {
            if (edit->activeMesh && !edit->savePath.empty())
            {
                MeshEditSession cached = *edit;
                cached.activeMesh = nullptr;
                m_meshEditCache[MeshCacheKey(edit->savePath)] =
                    std::move(cached);
            }
            edit->activeMesh = session.mesh;
            edit->savePath = MeshSavePath(*session.mesh,
                m_activeSceneAssetDocument ? m_activeSceneAssetDocument->path
                : scene == m_prefabScene.get() ? m_activePrefabPath
                : m_currentScenePath);
            if (auto cached = m_meshEditCache.find(
                    MeshCacheKey(edit->savePath));
                !edit->savePath.empty() && cached != m_meshEditCache.end())
            {
                const bool enabled = edit->enabled;
                *edit = cached->second;
                edit->enabled = enabled;
                edit->activeMesh = session.mesh;
                RestoreMeshSnapshot(*session.mesh, edit->baseline);
                session.strokeBefore = edit->baseline;
            }
            else
            {
                edit->baseline = session.strokeBefore;
                edit->savedSnapshot = edit->baseline;
                edit->undo.clear(); edit->redo.clear(); edit->dirty = false;
            }
        }
    }
    if (std::hypot(mouse.x - session.lastPaintPosition.x,
        mouse.y - session.lastPaintPosition.y) < std::max(1.f, radius * .08f))
        return true;
    session.lastPaintPosition = mouse;
    const glm::mat4 projection = camera->GetProjectionMatrix(
        input.available.x / input.available.y) * camera->GetViewMatrix() *
        session.mesh->Owner->transform.GetWorldMatrix();
    struct ProjectedVertex
    {
        float x = 0.f, y = 0.f, depth = 0.f;
        bool visible = false;
    };
    std::vector<ProjectedVertex> projected(source.size());
    for (size_t i = 0; i < source.size(); ++i)
    {
        const auto& p = source[i].pos;
        const glm::vec4 clip = projection * glm::vec4(p[0], p[1], p[2], 1.f);
        if (clip.w <= 1e-5f) continue;
        const glm::vec3 ndc = glm::vec3(clip) / clip.w;
        if (ndc.z < 0.f || ndc.z > 1.f) continue;
        projected[i] = { (ndc.x * .5f + .5f) * input.available.x,
            (.5f - ndc.y * .5f) * input.available.y, ndc.z, true };
    }
    // Require a hit on the visible triangle surface before a stroke can
    // affect vertices. The nearest projected face wins when meshes overlap.
    const auto& indices = session.mesh->GetIndices();
    const size_t indexCount = indices.empty() ? source.size() : indices.size();
    float hitDepth = INFINITY;
    size_t hitFaceIndex = SIZE_MAX;
    for (size_t i = 0; i + 2 < indexCount; i += 3)
    {
        const uint32_t a = indices.empty() ? static_cast<uint32_t>(i) : indices[i];
        const uint32_t b = indices.empty() ? static_cast<uint32_t>(i + 1) : indices[i + 1];
        const uint32_t c = indices.empty() ? static_cast<uint32_t>(i + 2) : indices[i + 2];
        if (a >= projected.size() || b >= projected.size() ||
            c >= projected.size()) continue;
        const auto& pa = projected[a], &pb = projected[b], &pc = projected[c];
        if (!pa.visible || !pb.visible || !pc.visible) continue;
        const float denominator = (pb.y - pc.y) * (pa.x - pc.x) +
            (pc.x - pb.x) * (pa.y - pc.y);
        if (std::abs(denominator) < 1e-5f) continue;
        const float u = ((pb.y - pc.y) * (mouse.x - pc.x) +
            (pc.x - pb.x) * (mouse.y - pc.y)) / denominator;
        const float v = ((pc.y - pa.y) * (mouse.x - pc.x) +
            (pa.x - pc.x) * (mouse.y - pc.y)) / denominator;
        if (u < 0.f || v < 0.f || u + v > 1.f) continue;
        const float depth = pa.depth * u + pb.depth * v +
            pc.depth * (1.f - u - v);
        if (depth < hitDepth)
        {
            hitDepth = depth;
            hitFaceIndex = i / 3;
        }
    }
    if (!std::isfinite(hitDepth)) return true;
    std::vector<float> original(source.size());
    for (size_t i = 0; i < source.size(); ++i)
        original[i] = BoneWeight(source[i], session.boneIndex);
    const std::vector<std::vector<uint32_t>>* neighbors = nullptr;
    if (session.brushOperation == 3)
    {
        auto& cache = session.adjacency;
        if (!cache || cache->mesh != session.mesh ||
            cache->vertexCount != source.size() || cache->indices != indices)
        {
            cache = std::make_shared<SkeletonEditSession::WeightAdjacencyCache>();
            cache->mesh = session.mesh;
            cache->vertexCount = source.size();
            cache->indices = indices;
            cache->neighbors.resize(source.size());
            const size_t count = indices.empty() ? source.size() : indices.size();
            for (size_t i = 0; i + 2 < count; i += 3)
            {
                const uint32_t a = indices.empty() ? static_cast<uint32_t>(i) : indices[i];
                const uint32_t b = indices.empty() ? static_cast<uint32_t>(i + 1) : indices[i + 1];
                const uint32_t c = indices.empty() ? static_cast<uint32_t>(i + 2) : indices[i + 2];
                if (a >= source.size() || b >= source.size() || c >= source.size())
                    continue;
                cache->neighbors[a].push_back(b);
                cache->neighbors[a].push_back(c);
                cache->neighbors[b].push_back(a);
                cache->neighbors[b].push_back(c);
                cache->neighbors[c].push_back(a);
                cache->neighbors[c].push_back(b);
            }
        }
        neighbors = &cache->neighbors;
    }
    std::vector<Engine::Components::Mesh::Vertex> vertices(source);
    bool changed = false;
    bool mirrorReady = session.mirrorBoneIndex < 0;
    const auto falloffAt = [&](float distance, float brushRadius)
    {
        if (distance > brushRadius) return 0.f;
        if (session.brushFalloff == 0) return 1.f;
        const float hardness = std::clamp(session.brushHardness, 0.f, .999f);
        const float edge = std::clamp((distance / brushRadius - hardness) /
            (1.f - hardness), 0.f, 1.f);
        const float linear = 1.f - edge;
        return session.brushFalloff == 2
            ? linear * linear * (3.f - 2.f * linear) : linear;
    };
    const auto paintVertex = [&](size_t index, int bone, float falloff,
        const std::vector<float>& initial)
    {
        if (falloff <= 0.f) return;
        const float opacity = session.brushStrength * falloff;
        float target = initial[index];
        switch (session.brushOperation)
        {
        case 0: target += (std::max(target, session.brushPaintWeight) -
            target) * opacity; break;
        case 1: target *= 1.f - session.brushPaintWeight * opacity; break;
        case 2: target += (session.brushPaintWeight - target) * opacity; break;
        case 3:
            if (neighbors && !(*neighbors)[index].empty())
            {
                float average = 0.f;
                for (uint32_t neighbor : (*neighbors)[index])
                    average += initial[neighbor];
                average /= static_cast<float>((*neighbors)[index].size());
                target += (average - target) * opacity;
            }
            break;
        case 4:
        {
            float total = 0.f;
            for (float value : source[index].weights0) total += value;
            for (float value : source[index].weights1) total += value;
            if (std::abs(total - 1.f) > 1e-4f)
                changed = WeightInfluence::Set(vertices[index], bone,
                    initial[index], session.lockedBones) || changed;
            return;
        }
        }
        if (std::abs(target - initial[index]) < 1e-5f) return;
        changed = WeightInfluence::Set(vertices[index], bone,
            target, session.lockedBones) || changed;
    };
    auto& topologyCache = session.surfaceTopology;
    if (!topologyCache || topologyCache->vertexCount != source.size() ||
        topologyCache->indices != indices)
        topologyCache = std::make_shared<WeightPaintSurface::Topology>(
            WeightPaintSurface::BuildTopology(indices, source.size()));
    const auto& topology = *topologyCache;
    std::vector<glm::vec3> screenPositions(source.size());
    for (size_t i = 0; i < source.size(); ++i)
        screenPositions[i] = { projected[i].x, projected[i].y, 0.f };
    std::vector<uint32_t> screenFaces;
    for (size_t face = 0; face < topology.faces.size(); ++face)
    {
        const auto& tri = topology.faces[face];
        if (tri[0] >= source.size() || tri[1] >= source.size() ||
            tri[2] >= source.size() || !projected[tri[0]].visible ||
            !projected[tri[1]].visible || !projected[tri[2]].visible)
            continue;
        const float left = std::min({ projected[tri[0]].x,
            projected[tri[1]].x, projected[tri[2]].x });
        const float right = std::max({ projected[tri[0]].x,
            projected[tri[1]].x, projected[tri[2]].x });
        const float top = std::min({ projected[tri[0]].y,
            projected[tri[1]].y, projected[tri[2]].y });
        const float bottom = std::max({ projected[tri[0]].y,
            projected[tri[1]].y, projected[tri[2]].y });
        if (left <= mouse.x + radius && right >= mouse.x - radius &&
            top <= mouse.y + radius && bottom >= mouse.y - radius)
            screenFaces.push_back(static_cast<uint32_t>(face));
    }
    const auto sourceVisible = [&](size_t face,
        const MirrorWeightPaint::TrianglePoint& nearest)
    {
        const auto& tri = topology.faces[face];
        if (!projected[tri[0]].visible || !projected[tri[1]].visible ||
            !projected[tri[2]].visible) return false;
        const float faceDepth = nearest.barycentric.x * projected[tri[0]].depth +
            nearest.barycentric.y * projected[tri[1]].depth +
            nearest.barycentric.z * projected[tri[2]].depth;
        for (uint32_t otherFace : screenFaces)
        {
            if (otherFace == face) continue;
            const auto& other = topology.faces[otherFace];
            const glm::vec3& a = screenPositions[other[0]];
            const glm::vec3& b = screenPositions[other[1]];
            const glm::vec3& c = screenPositions[other[2]];
            const auto front = MirrorWeightPaint::ClosestPoint(
                nearest.point, a, b, c);
            if (glm::length(front.point - nearest.point) > .5f) continue;
            const float frontDepth = front.barycentric.x *
                    projected[other[0]].depth +
                front.barycentric.y * projected[other[1]].depth +
                front.barycentric.z * projected[other[2]].depth;
            if (frontDepth < faceDepth - 1e-4f) return false;
        }
        return true;
    };
    const auto screenDistance = [&](const glm::vec3& offset)
    {
        return session.brushShape == 0
            ? std::hypot(offset.x, offset.y)
            : std::max(std::abs(offset.x), std::abs(offset.y));
    };
    const auto sourceCoverage = WeightPaintSurface::Coverage(topology,
        screenPositions, hitFaceIndex, { mouse.x, mouse.y, 0.f }, radius,
        screenDistance, falloffAt, sourceVisible);
    for (size_t i = 0; i < vertices.size(); ++i)
        paintVertex(i, session.boneIndex, sourceCoverage[i], original);
    if (session.mirrorBoneIndex >= 0)
    {
        const auto& joints = session.skeleton->ResolveJoints();
        auto* sourceJoint = joints[session.boneIndex];
        auto* targetJoint = joints[session.mirrorBoneIndex];
        MirrorWeightPaint::BoneFrame sourceFrame, targetFrame;
        const bool framed = sourceJoint && sourceJoint->Parent &&
            targetJoint && targetJoint->Parent &&
            MirrorWeightPaint::BuildFrame(
                sourceJoint->Parent->transform.GetWorldMatrix(),
                sourceJoint->transform.GetWorldMatrix(), sourceFrame) &&
            MirrorWeightPaint::BuildFrame(
                targetJoint->Parent->transform.GetWorldMatrix(),
                targetJoint->transform.GetWorldMatrix(), targetFrame);
        if (!framed)
            session.error = "Both mirror bones need a nonzero parent-to-joint length.";
        else
        {
            const glm::mat4 viewProjection = camera->GetProjectionMatrix(
                input.available.x / input.available.y) * camera->GetViewMatrix();
            const glm::mat4 inverseViewProjection = glm::inverse(viewProjection);
            const auto unproject = [&](float x)
            {
                glm::vec4 world = inverseViewProjection * glm::vec4(
                    2.f * x / input.available.x - 1.f,
                    1.f - 2.f * mouse.y / input.available.y,
                    hitDepth, 1.f);
                return glm::vec3(world) / world.w;
            };
            const glm::vec3 sourceCenter = unproject(mouse.x);
            const float worldRadius = glm::length(
                unproject(mouse.x + radius) - sourceCenter);
            if (std::isfinite(worldRadius) && worldRadius > 1e-6f)
            {
                const glm::vec3 mirrorCenter = MirrorWeightPaint::MapPoint(
                    sourceFrame, targetFrame, sourceCenter);
                const glm::mat4 meshWorld =
                    session.mesh->Owner->transform.GetWorldMatrix();
                std::vector<glm::vec3> worldVertices(source.size());
                for (size_t i = 0; i < source.size(); ++i)
                    worldVertices[i] = glm::vec3(meshWorld * glm::vec4(
                        source[i].pos[0], source[i].pos[1],
                        source[i].pos[2], 1.f));
                float nearestDistance = INFINITY;
                float bestInfluence = -1.f;
                size_t targetFaceIndex = SIZE_MAX;
                for (size_t face = 0; face < topology.faces.size(); ++face)
                {
                    const auto& tri = topology.faces[face];
                    const uint32_t a = tri[0], b = tri[1], c = tri[2];
                    if (a >= source.size() || b >= source.size() ||
                        c >= source.size()) continue;
                    const glm::vec3 centroid = (worldVertices[a] +
                        worldVertices[b] + worldVertices[c]) / 3.f;
                    if (MirrorWeightPaint::DistanceToBone(targetFrame,
                            centroid) >= MirrorWeightPaint::DistanceToBone(
                                sourceFrame, centroid)) continue;
                    const auto closest = MirrorWeightPaint::ClosestPoint(
                        mirrorCenter, worldVertices[a], worldVertices[b],
                        worldVertices[c]);
                    const float distance = glm::length(
                        closest.point - mirrorCenter);
                    const float influence = (BoneWeight(source[a],
                        session.mirrorBoneIndex) + BoneWeight(source[b],
                            session.mirrorBoneIndex) + BoneWeight(source[c],
                                session.mirrorBoneIndex)) / 3.f;
                    const float tieTolerance = std::max(1e-4f,
                        worldRadius * .02f);
                    if (distance < nearestDistance - tieTolerance ||
                        (std::abs(distance - nearestDistance) <= tieTolerance &&
                            influence > bestInfluence))
                    {
                        nearestDistance = distance;
                        bestInfluence = influence;
                        targetFaceIndex = face;
                    }
                }
                if (targetFaceIndex != SIZE_MAX &&
                    nearestDistance <= std::max(worldRadius * 2.f,
                        targetFrame.length * .25f))
                {
                    const auto worldDistance = [&](const glm::vec3& offset)
                    {
                        return session.brushShape == 0 ? glm::length(offset)
                            : std::max({ std::abs(glm::dot(offset,
                                    targetFrame.x)),
                                std::abs(glm::dot(offset, targetFrame.y)),
                                std::abs(glm::dot(offset, targetFrame.z)) });
                    };
                    const auto targetVisible = [&](size_t face,
                        const MirrorWeightPaint::TrianglePoint&)
                    {
                        const auto& tri = topology.faces[face];
                        const glm::vec3 centroid = (worldVertices[tri[0]] +
                            worldVertices[tri[1]] + worldVertices[tri[2]]) / 3.f;
                        return MirrorWeightPaint::DistanceToBone(targetFrame,
                            centroid) < MirrorWeightPaint::DistanceToBone(
                                sourceFrame, centroid);
                    };
                    const auto targetCoverage = WeightPaintSurface::Coverage(
                        topology, worldVertices, targetFaceIndex, mirrorCenter,
                        worldRadius, worldDistance, falloffAt, targetVisible);
                    bool reachesTarget = false;
                    std::vector<float> targetOriginal(source.size());
                    for (size_t i = 0; i < source.size(); ++i)
                        targetOriginal[i] = BoneWeight(source[i],
                            session.mirrorBoneIndex);
                    for (size_t i = 0; i < source.size(); ++i)
                    {
                        if (MirrorWeightPaint::DistanceToBone(targetFrame,
                                worldVertices[i]) >
                            MirrorWeightPaint::DistanceToBone(sourceFrame,
                                worldVertices[i])) continue;
                        reachesTarget = reachesTarget || targetCoverage[i] > 0.f;
                        paintVertex(i, session.mirrorBoneIndex,
                            targetCoverage[i],
                            targetOriginal);
                    }
                    mirrorReady = reachesTarget;
                    session.error = mirrorReady ? std::string{} :
                        "The mirrored brush does not reach the target surface.";
                }
                else session.error = "No mesh surface was found at the mirrored brush position.";
            }
        }
    }
    if (changed && mirrorReady &&
        session.mesh->UpdateAuthoredVertices(std::move(vertices)))
    {
        session.strokeChanged = true;
        if (MeshEditSession* edit = ActiveMeshEditSession())
        {
            edit->dirty = true;
            if (m_activeSceneAssetDocument)
                RefreshSceneAssetDocumentTitle(*m_activeSceneAssetDocument);
        }
        if (m_renderer) m_renderer->MarkDirty();
    }
    return true;
}
}

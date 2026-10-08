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
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <numeric>
#include <unordered_set>
#include <glm/gtc/matrix_inverse.hpp>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/matrix_decompose.hpp>

namespace Engine::Editor
{
namespace
{
std::string SnapshotWeights(const Engine::Components::Mesh& mesh)
{
    const auto& vertices = mesh.GetVertices();
    const auto& indices = mesh.GetIndices();
    const uint32_t counts[]{ static_cast<uint32_t>(vertices.size()),
        static_cast<uint32_t>(indices.size()) };
    std::string result(reinterpret_cast<const char*>(counts), sizeof(counts));
    result.append(reinterpret_cast<const char*>(vertices.data()),
        vertices.size() * sizeof(Engine::Components::Mesh::Vertex));
    result.append(reinterpret_cast<const char*>(indices.data()),
        indices.size() * sizeof(uint32_t));
    return result;
}

float BoneWeight(const Engine::Components::Mesh::Vertex& vertex, int bone)
{
    float weight = 0.f;
    for (int slot = 0; slot < 8; ++slot)
    {
        const int offset = slot % 4;
        const float joint = slot < 4 ? vertex.joints0[offset] : vertex.joints1[offset];
        const float value = slot < 4 ? vertex.weights0[offset] : vertex.weights1[offset];
        if (value > 0.f && static_cast<int>(joint) == bone) weight += value;
    }
    return weight;
}

void SetBoneWeight(Engine::Components::Mesh::Vertex& vertex, int bone,
    float target)
{
    float* weights[]{ &vertex.weights0[0], &vertex.weights0[1],
        &vertex.weights0[2], &vertex.weights0[3], &vertex.weights1[0],
        &vertex.weights1[1], &vertex.weights1[2], &vertex.weights1[3] };
    float* joints[]{ &vertex.joints0[0], &vertex.joints0[1],
        &vertex.joints0[2], &vertex.joints0[3], &vertex.joints1[0],
        &vertex.joints1[1], &vertex.joints1[2], &vertex.joints1[3] };
    int selected = -1, freeSlot = -1, weakest = 0;
    float otherTotal = 0.f;
    for (int slot = 0; slot < 8; ++slot)
    {
        if (*weights[slot] > 0.f && static_cast<int>(*joints[slot]) == bone)
        {
            if (selected < 0) selected = slot;
            else { weights[selected][0] += *weights[slot]; *weights[slot] = 0.f; }
        }
        else
        {
            otherTotal += *weights[slot];
            if (*weights[slot] <= 0.f && freeSlot < 0) freeSlot = slot;
            if (*weights[slot] < *weights[weakest]) weakest = slot;
        }
    }
    if (selected < 0)
    {
        selected = freeSlot >= 0 ? freeSlot : weakest;
        otherTotal -= *weights[selected];
        *joints[selected] = static_cast<float>(bone);
    }
    target = std::clamp(target, 0.f, 1.f);
    if (otherTotal <= 1e-6f) target = 1.f;
    *weights[selected] = target;
    const float factor = otherTotal > 1e-6f ? (1.f - target) / otherTotal : 0.f;
    for (int slot = 0; slot < 8; ++slot)
        if (slot != selected) *weights[slot] *= factor;
}

bool ValidBone(const Engine::Components::Skeleton* skeleton,
    const Engine::Core::Object* object)
{
    if (!skeleton || !object) return false;
    for (auto* joint : skeleton->ResolveJoints())
        if (joint == object) return true;
    return false;
}

// The inverse bind matrices are the authored rest pose. Animation playback
// changes the joint transforms, but disabling skinning alone leaves those
// transforms at the last sampled frame.
void RestoreSkeletonBindPose(Engine::Components::Skeleton& skeleton)
{
    auto* model = skeleton.ResolveModel();
    const auto& joints = skeleton.ResolveJoints();
    if (!model || !model->Owner ||
        joints.size() != skeleton.inverseBindMatrices.size()) return;

    std::vector<size_t> order(joints.size());
    std::iota(order.begin(), order.end(), 0);
    const auto depth = [](const Engine::Core::Object* object)
    {
        size_t result = 0;
        for (; object; object = object->Parent) ++result;
        return result;
    };
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b)
        { return depth(joints[a]) < depth(joints[b]); });

    const glm::mat4 modelWorld = model->Owner->transform.GetWorldMatrix();
    for (size_t index : order)
    {
        auto* joint = joints[index];
        const glm::mat4& inverseBind = skeleton.inverseBindMatrices[index];
        if (!joint || std::abs(glm::determinant(inverseBind)) < 1e-8f)
            continue;
        const glm::mat4 world = modelWorld * glm::inverse(inverseBind);
        const glm::mat4 parentWorld = joint->Parent
            ? joint->Parent->transform.GetWorldMatrix() : glm::mat4(1.f);
        if (std::abs(glm::determinant(parentWorld)) < 1e-8f) continue;
        const glm::mat4 local = glm::inverse(parentWorld) * world;
        glm::vec3 scale, translation, skew;
        glm::vec4 perspective;
        glm::quat rotation;
        if (!glm::decompose(local, scale, rotation, translation, skew,
                perspective)) continue;
        joint->transform.position = translation;
        joint->transform.rotation = glm::eulerAngles(glm::normalize(rotation));
        joint->transform.scale = scale;
    }
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
        session.boneIndex = -1;
    }
    for (const auto& panel : m_panels)
        if (auto* view = dynamic_cast<SceneView*>(panel.get());
            view && view->GetScene() == scene)
            view->AllowObjectTransform = !enabled ||
                (session.submode == 0 && session.boneTool == 0);
    m_pendingEditToolsOpen = true;
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
            edit->baseline = SnapshotWeights(*session.mesh);
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
    session.observedSelection = selected;
    session.observedStructureRevision = structureRevision;
    session.observedSubmode = session.submode;
    Engine::Components::Skeleton* chosen = nullptr;
    for (Engine::Core::Object* parent = selected; parent && !chosen;
        parent = parent->Parent)
        for (auto* component : parent->Components)
            if (auto* skeleton = dynamic_cast<Engine::Components::Skeleton*>(component))
            { chosen = skeleton; break; }
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
        session.skeleton = chosen;
        session.boneIndex = -1;
        session.mesh = nullptr;
        session.adjacency.reset();
        session.observedBindTransforms.clear();
        if (chosen) RestoreSkeletonBindPose(*chosen);
    }
    if (chosen)
    {
        const auto& joints = chosen->ResolveJoints();
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
    const auto& bones = skeleton->ResolveJoints();
    Engine::Core::Object* selected = session.boneIndex >= 0 &&
        session.boneIndex < static_cast<int>(bones.size()) &&
        bones[session.boneIndex] ? bones[session.boneIndex] : nullptr;
    if (action == 0) // Add a child bone.
    {
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
        // Keep the palette stable for every other bone and mesh. Removing a
        // non-last palette entry would require a multi-asset remap transaction.
        if (session.boneIndex != static_cast<int>(bones.size()) - 1)
        { session.error = "Remove bones from the end of the joint palette."; return false; }
        const auto boundNodes = model->ResolveNodes();
        std::vector<Engine::Core::Object*> retained(boundNodes.begin(),
            boundNodes.end());
        scene->SetSelectedObject(nullptr);
        if (m_primaryHierarchy) m_primaryHierarchy->SetSelectedObject(nullptr);
        if (m_primaryProperties) m_primaryProperties->SetSelectedObject(nullptr);
        scene->RemoveObject(selected);
        skeleton->jointNodes.pop_back();
        if (!skeleton->inverseBindMatrices.empty())
            skeleton->inverseBindMatrices.pop_back();
        for (size_t node = 0; node < retained.size(); ++node)
            if (retained[node] && retained[node] != selected)
                model->BindNode(static_cast<unsigned>(node), retained[node]);
        session.boneIndex = skeleton->jointNodes.empty() ? -1 : 0;
    }
    else if (action == 2) // Commit current default pose as the bind pose.
    {
        const auto& joints = skeleton->ResolveJoints();
        if (joints.empty() || joints.size() != skeleton->jointNodes.size())
        { session.error = "The skeleton has unresolved joints."; return false; }
        const glm::mat4 inverseModel = glm::inverse(
            model->Owner->transform.GetWorldMatrix());
        std::vector<glm::mat4> inverseBind;
        inverseBind.reserve(joints.size());
        for (auto* joint : joints)
        {
            if (!joint) { session.error = "The skeleton has unresolved joints."; return false; }
            const glm::mat4 inModel = inverseModel * joint->transform.GetWorldMatrix();
            if (std::abs(glm::determinant(inModel)) < 1e-8f)
            { session.error = "A joint transform has zero scale."; return false; }
            inverseBind.push_back(glm::inverse(inModel));
        }
        skeleton->inverseBindMatrices = std::move(inverseBind);
    }
    session.error.clear();
    CommitSkeletonEdit(scene);
    return true;
}

void EditorState::DrawSkeletonEditTools(IEditorUi& ui)
{
    auto* session = ActiveSkeletonEditSession();
    auto* scene = GetActiveDocumentScene();
    if (!session || !session->enabled || !scene) return;
    SyncSkeletonEditSelection(scene, *session);
    const char* modes[]{ "Bones", "Weight Paint" };
    if (ui.Combo("Skeleton tool", &session->submode, modes, 2))
    {
        FinishSkeletonPaintStroke(*session);
        scene->SetEditorWeightPaint(nullptr, -1);
        for (const auto& panel : m_panels)
            if (auto* view = dynamic_cast<SceneView*>(panel.get());
                view && view->GetScene() == scene)
                view->AllowObjectTransform = session->submode == 0 &&
                    session->boneTool == 0;
        SyncSkeletonEditSelection(scene, *session);
    }
    auto* skeleton = session->skeleton;
    if (!skeleton)
    { ui.DisabledLabel("Select an object containing a skeleton."); return; }
    const auto& bones = skeleton->ResolveJoints();
    std::vector<std::string> labels;
    std::vector<const char*> names;
    labels.reserve(bones.size()); names.reserve(bones.size());
    for (size_t i = 0; i < bones.size(); ++i)
        labels.push_back(bones[i] ? bones[i]->name : "Missing joint");
    for (const auto& label : labels) names.push_back(label.c_str());
    int selected = std::max(0, session->boneIndex);
    if (!names.empty() && ui.Combo("Bone", &selected, names.data(),
        static_cast<int>(names.size())))
    {
        session->boneIndex = selected;
        auto* object = bones[selected];
        if (object)
        {
            scene->SetSelectedObject(object);
            if (m_primaryHierarchy) m_primaryHierarchy->SetSelectedObject(object);
            if (m_primaryProperties) m_primaryProperties->SetSelectedObject(object);
        }
        SyncSkeletonEditSelection(scene, *session);
    }
    if (!session->error.empty()) ui.ColoredLabel(session->error.c_str(),
        { 1.f, .55f, .3f, 1.f });
    if (session->submode == 0)
    {
        ui.DisabledLabel("Edit the default bind pose. Animation is inactive.");
        if (ui.Checkbox("Show bone overlays", &skeleton->showBones))
        {
            CommitSkeletonEdit(scene);
            if (m_renderer) m_renderer->MarkDirty();
        }
        const char* boneTools[]{ "Edit bone", "Add child", "Remove bone" };
        if (ui.Combo("Bone tool", &session->boneTool, boneTools, 3))
            for (const auto& panel : m_panels)
                if (auto* view = dynamic_cast<SceneView*>(panel.get());
                    view && view->GetScene() == scene)
                    view->AllowObjectTransform = session->boneTool == 0;
        if (session->boneTool == 1 && ui.Button("Add child bone"))
            ApplySkeletonBoneAction(scene, *session, 0);
        if (session->boneTool == 2 && ui.Button("Remove selected bone"))
            ApplySkeletonBoneAction(scene, *session, 1);
        if (ui.Button("Apply bind pose")) ApplySkeletonBoneAction(scene, *session, 2);
        if (session->boneTool == 0 &&
            ValidBone(skeleton, scene->GetSelectedObject()))
        {
            ui.Separator();
            DrawObjectEditTools(ui);
        }
        RefreshSkeletonBindPose(*session);
    }
    else
    {
        auto* mesh = session->mesh;
        if (!mesh || session->boneIndex < 0)
        { ui.DisabledLabel("A skinned mesh and bone are required."); return; }
        std::vector<Engine::Components::Mesh*> meshes;
        std::vector<const char*> meshNames;
        for (const auto& object : scene->GetObjects())
            if (object)
                if (auto* skin = object->GetComponent<Engine::Components::SkinnedMesh>();
                    skin && skin->ResolveSkeleton() == skeleton)
                    if (auto* candidate = object->GetComponent<Engine::Components::Mesh>();
                        candidate && !candidate->GetVertices().empty())
                        meshes.push_back(candidate);
        if (meshes.size() > 1)
        {
            int selectedMesh = 0;
            for (size_t i = 0; i < meshes.size(); ++i)
            {
                meshNames.push_back(meshes[i]->Owner
                    ? meshes[i]->Owner->name.c_str() : "Mesh");
                if (meshes[i] == mesh) selectedMesh = static_cast<int>(i);
            }
            if (ui.Combo("Skinned mesh", &selectedMesh, meshNames.data(),
                static_cast<int>(meshNames.size())))
            {
                session->mesh = meshes[selectedMesh];
                mesh = session->mesh;
                scene->SetEditorWeightPaint(mesh, session->boneIndex);
                scene->SetEditorSelectedMesh(nullptr);
            }
        }
        ui.ValueLabel("Mesh", mesh->Owner ? mesh->Owner->name.c_str() : "Mesh");
        const char* operations[]{ "Add", "Subtract", "Replace", "Smooth",
            "Normalize" };
        const char* shapes[]{ "Circle", "Square" };
        const char* falloffs[]{ "Hard", "Linear", "Smooth" };
        ui.Combo("Brush", &session->brushOperation, operations, 5);
        ui.Combo("Shape", &session->brushShape, shapes, 2);
        ui.Combo("Feather", &session->brushFalloff, falloffs, 3);
        ui.SliderFloat("Size (pixels)", &session->brushRadius, 2.f, 250.f);
        if (session->brushFalloff != 0)
            ui.SliderFloat("Hardness", &session->brushHardness, 0.f, 1.f);
        ui.SliderFloat("Strength", &session->brushStrength, .01f, 1.f);
        if (session->brushOperation == 2)
            ui.SliderFloat("Target weight", &session->brushTarget, 0.f, 1.f);
        ui.DisabledLabel("Drag on the gray mesh to paint this bone's heat map.");
        MeshEditSession* edit = ActiveMeshEditSession();
        if (edit && ui.Button(edit->dirty ? "Save weights *" : "Save weights"))
        {
            FinishSkeletonPaintStroke(*session);
            SaveMeshEditSession(*edit);
        }
    }
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
    bool moved = current.size() == session.observedBindTransforms.size();
    if (moved)
        moved = std::memcmp(current.data(),
            session.observedBindTransforms.data(),
            current.size() * sizeof(glm::mat4)) != 0;
    if (moved)
    {
        auto* model = skeleton->ResolveModel();
        if (model && model->Owner)
        {
            const glm::mat4 inverseModel = glm::inverse(
                model->Owner->transform.GetWorldMatrix());
            std::vector<glm::mat4> inverseBind;
            inverseBind.reserve(current.size());
            for (const glm::mat4& world : current)
            {
                const glm::mat4 inModel = inverseModel * world;
                if (std::abs(glm::determinant(inModel)) < 1e-8f)
                    return;
                inverseBind.push_back(glm::inverse(inModel));
            }
            skeleton->inverseBindMatrices = std::move(inverseBind);
            if (m_renderer) m_renderer->MarkDirty();
        }
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
        session.strokeBefore = SnapshotWeights(*session.mesh);
        session.lastPaintPosition = { -10000.f, -10000.f };
        MeshEditSession* edit = ActiveMeshEditSession();
        if (edit && edit->activeMesh != session.mesh)
        {
            edit->activeMesh = session.mesh;
            edit->savePath = MeshSavePath(*session.mesh,
                m_activeSceneAssetDocument ? m_activeSceneAssetDocument->path
                : scene == m_prefabScene.get() ? m_activePrefabPath
                : m_currentScenePath);
            edit->baseline = session.strokeBefore;
            edit->savedSnapshot = edit->baseline;
            edit->undo.clear(); edit->redo.clear(); edit->dirty = false;
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
    uint32_t hitFace[3]{ UINT32_MAX, UINT32_MAX, UINT32_MAX };
    float hitBarycentric[3]{};
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
            hitFace[0] = a; hitFace[1] = b; hitFace[2] = c;
            hitBarycentric[0] = u;
            hitBarycentric[1] = v;
            hitBarycentric[2] = 1.f - u - v;
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
    for (size_t i = 0; i < vertices.size(); ++i)
    {
        const auto& p = projected[i];
        int hitCorner = -1;
        for (int corner = 0; corner < 3; ++corner)
            if (hitFace[corner] == i) { hitCorner = corner; break; }
        if (!p.visible || (hitCorner < 0 && p.depth > hitDepth + .015f))
            continue;
        const float dx = p.x - mouse.x;
        const float dy = p.y - mouse.y;
        const float distance = session.brushShape == 0
            ? std::hypot(dx, dy) : std::max(std::abs(dx), std::abs(dy));
        const float t = std::clamp(distance / radius, 0.f, 1.f);
        float falloff = 0.f;
        if (distance <= radius)
        {
            if (session.brushFalloff == 0) falloff = 1.f;
            else
            {
                const float hardness = std::clamp(session.brushHardness,
                    0.f, .999f);
                const float edge = std::clamp((t - hardness) /
                    (1.f - hardness), 0.f, 1.f);
                falloff = 1.f - edge;
                if (session.brushFalloff == 2)
                    falloff = falloff * falloff * (3.f - 2.f * falloff);
            }
        }
        if (hitCorner >= 0)
            falloff = std::max(falloff,
                .5f * hitBarycentric[hitCorner]);
        if (falloff <= 0.f) continue;
        const float opacity = session.brushStrength * falloff;
        float target = original[i];
        switch (session.brushOperation)
        {
        case 0: target += (1.f - target) * opacity; break;
        case 1: target *= 1.f - opacity; break;
        case 2: target += (session.brushTarget - target) * opacity; break;
        case 3:
            if (neighbors && !(*neighbors)[i].empty())
            {
                float average = 0.f;
                for (uint32_t neighbor : (*neighbors)[i])
                    average += original[neighbor];
                average /= static_cast<float>((*neighbors)[i].size());
                target += (average - target) * opacity;
            }
            break;
        case 4:
        {
            float total = 0.f;
            for (float value : source[i].weights0) total += value;
            for (float value : source[i].weights1) total += value;
            if (std::abs(total - 1.f) > 1e-4f)
            {
                SetBoneWeight(vertices[i], session.boneIndex, original[i]);
                changed = true;
            }
            continue;
        }
        }
        if (std::abs(target - original[i]) < 1e-5f) continue;
        SetBoneWeight(vertices[i], session.boneIndex, target);
        changed = true;
    }
    if (changed && session.mesh->UpdateAuthoredVertices(std::move(vertices)))
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

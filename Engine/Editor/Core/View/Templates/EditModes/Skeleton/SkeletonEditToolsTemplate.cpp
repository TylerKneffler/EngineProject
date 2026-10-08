#include "pch.h"
#include "Engine/Editor/EditorState.h"
#include "Engine/Editor/UI/IEditorUi.h"
#include "Engine/Editor/Core/View/Views/SceneView.h"
#include "Engine/Editor/Core/View/Views/HierarchyView.h"
#include "Engine/Editor/Core/View/Views/PropertiesView.h"
#include "Core/Scene/Scene.h"
#include "Core/Object.h"
#include "Core/Compoonents/Animation/Skeleton.h"
#include "Core/Compoonents/Animation/SkinnedMesh.h"
#include "Core/Compoonents/Obj/Mesh.h"
#include "Engine/Editor/Core/View/Templates/EditModes/Skeleton/SkinBindingWeights.h"
#include <algorithm>

namespace Engine::Editor
{
namespace
{
bool ValidBone(const Engine::Components::Skeleton* skeleton,
    const Engine::Core::Object* object)
{
    if (!skeleton || !object) return false;
    for (auto* joint : skeleton->ResolveJoints())
        if (joint == object) return true;
    return false;
}


}
void EditorState::DrawSkeletonEditTools(IEditorUi& ui)
{
    auto* session = ActiveSkeletonEditSession();
    auto* scene = GetActiveDocumentScene();
    if (!session || !session->enabled || !scene) return;
    const bool toolbarPopup = m_toolbarPopupTool == "Bone Settings" ||
        m_toolbarPopupTool == "Brush Settings";
    SyncSkeletonEditSelection(scene, *session);
    const char* modes[]{ "Bones", "Weight Paint", "Skin Binding" };
    if (!toolbarPopup && ui.Combo("Skeleton tool", &session->submode, modes, 3))
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
        FinishSkeletonPaintStroke(*session);
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
    if (session->boneIndex >= 0 &&
        session->boneIndex < static_cast<int>(session->lockedBones.size()))
    {
        bool locked = session->lockedBones[session->boneIndex];
        if (ui.Checkbox("Lock selected bone weights", &locked))
        {
            FinishSkeletonPaintStroke(*session);
            session->lockedBones[session->boneIndex] = locked;
        }
    }
    if (session->submode == 0)
    {
        ui.DisabledLabel("Edit the default bind pose. Animation is inactive.");
        const char* pivots[]{ "Joint", "Parent", "Skeleton root", "Custom world" };
        ui.Combo("Bone pivot", &session->pivotMode, pivots, 4);
        if (session->pivotMode == 3)
            ui.DragFloat3("Custom pivot", session->customPivot, .01f);
        ui.DragFloat("Translation snap (0 off)", &session->translationSnap, .01f);
        ui.DragFloat("Rotation snap (0 off)", &session->rotationSnap, 1.f);
        ui.DragFloat("Scale snap (0 off)", &session->scaleSnap, .01f);
        for (const auto& panel : m_panels)
            if (auto* view = dynamic_cast<SceneView*>(panel.get());
                view && view->GetScene() == scene)
                view->SetBoneTransformSettings(session->pivotMode,
                    { session->customPivot[0], session->customPivot[1],
                        session->customPivot[2] },
                    std::max(0.f, session->translationSnap),
                    std::max(0.f, session->rotationSnap),
                    std::max(0.f, session->scaleSnap));
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
        if (!toolbarPopup && session->boneTool == 1 && ui.Button("Add child bone"))
            ApplySkeletonBoneAction(scene, *session, 0);
        if (!toolbarPopup && session->boneTool == 2 &&
            ui.Button("Remove selected bone"))
            ApplySkeletonBoneAction(scene, *session, 1);
        if (!toolbarPopup && ui.Button("Duplicate selected bone"))
            ApplySkeletonBoneAction(scene, *session, 3);
        ui.InputText("Bone name", session->boneName,
            sizeof(session->boneName));
        if (!toolbarPopup && ui.Button("Rename selected bone"))
            ApplySkeletonBoneAction(scene, *session, 4);
        std::vector<const char*> parents;
        parents.push_back("Model root");
        for (const auto& label : labels) parents.push_back(label.c_str());
        int parentChoice = session->reparentBoneIndex + 1;
        if (ui.Combo("New bone parent", &parentChoice, parents.data(),
            static_cast<int>(parents.size())))
            session->reparentBoneIndex = parentChoice - 1;
        if (!toolbarPopup && ui.Button("Reparent selected bone"))
            ApplySkeletonBoneAction(scene, *session, 5);
        if (!toolbarPopup && ui.Button("Mirror selected bone (X)"))
            ApplySkeletonBoneAction(scene, *session, 6);
        if (!toolbarPopup && ui.Button("Apply bind pose"))
            ApplySkeletonBoneAction(scene, *session, 2);
        if (!toolbarPopup && session->boneTool == 0 &&
            ValidBone(skeleton, scene->GetSelectedObject()))
        {
            ui.Separator();
            auto* joint = scene->GetSelectedObject();
            auto& transform = joint->transform;
            const glm::vec3 oldPosition = transform.position;
            const glm::vec3 oldRotation = transform.rotation;
            float orientation[]{ glm::degrees(oldRotation.x),
                glm::degrees(oldRotation.y), glm::degrees(oldRotation.z) };
            bool changed = ui.DragFloat3("Joint position (local)", &transform.position.x,
                .01f);
            if (ui.DragFloat3("Joint orientation (local degrees)",
                    orientation, .5f))
            {
                transform.rotation = { glm::radians(orientation[0]),
                    glm::radians(orientation[1]), glm::radians(orientation[2]) };
                changed = true;
            }
            float roll = glm::degrees(transform.rotation.y);
            if (ui.DragFloat("Bone roll (local Y degrees)", &roll, .5f))
            { transform.rotation.y = glm::radians(roll); changed = true; }
            if (changed)
            {
                uint8_t channels = 0;
                if (transform.position != oldPosition)
                    channels |= Engine::Components::Transform::EditorPosition;
                if (transform.rotation != oldRotation)
                    channels |= Engine::Components::Transform::EditorRotation;
                transform.NotifyEditorTransformChanged(channels);
                RefreshSkeletonBindPose(*session);
                CommitSkeletonEdit(scene);
            }
        }
        RefreshSkeletonBindPose(*session);
    }
    else if (session->submode == 1)
    {
        auto* mesh = session->mesh;
        if (!mesh || session->boneIndex < 0)
        { ui.DisabledLabel("A skinned mesh and bone are required."); return; }
        if (session->lockedBones[session->boneIndex])
            ui.DisabledLabel("Selected bone is locked; choose another bone to paint.");
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
                FinishSkeletonPaintStroke(*session);
                session->mesh = meshes[selectedMesh];
                mesh = session->mesh;
                scene->SetEditorWeightPaint(mesh, session->boneIndex);
                scene->SetEditorSelectedMesh(nullptr);
            }
        }
        ui.ValueLabel("Mesh", mesh->Owner ? mesh->Owner->name.c_str() : "Mesh");
        std::vector<const char*> mirrorNames;
        mirrorNames.reserve(names.size() + 1);
        mirrorNames.push_back("Off");
        mirrorNames.insert(mirrorNames.end(), names.begin(), names.end());
        int mirrorChoice = session->mirrorBoneIndex + 1;
        if (ui.Combo("Mirror paint onto bone", &mirrorChoice,
                mirrorNames.data(), static_cast<int>(mirrorNames.size())))
        {
            FinishSkeletonPaintStroke(*session);
            session->mirrorBoneIndex = mirrorChoice - 1;
        }
        if (session->mirrorBoneIndex == session->boneIndex)
            ui.DisabledLabel("Choose a different target bone for mirroring.");
        else if (session->mirrorBoneIndex >= 0 &&
            session->mirrorBoneIndex < static_cast<int>(session->lockedBones.size()) &&
            session->lockedBones[session->mirrorBoneIndex])
            ui.DisabledLabel("The mirror target is locked; unlock it to paint.");
        else if (session->mirrorBoneIndex >= 0)
            ui.DisabledLabel("Bone length maps along the pair; brush size stays the same.");
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
        if (session->brushOperation <= 2)
            ui.SliderFloat("Paint weight", &session->brushPaintWeight, 0.f, 1.f);
        ui.SliderFloat("Brush strength", &session->brushStrength, .01f, 1.f);
        ui.DisabledLabel("Drag on the gray mesh to paint this bone's heat map.");
        MeshEditSession* edit = ActiveMeshEditSession();
        if (!toolbarPopup && edit &&
            ui.Button(edit->dirty ? "Save weights *" : "Save weights"))
        {
            FinishSkeletonPaintStroke(*session);
            SaveMeshEditSession(*edit);
        }
    }
    else
    {
        std::vector<Engine::Components::Mesh*> meshes;
        std::vector<std::string> meshLabels;
        for (const auto& object : scene->GetObjects())
            if (object)
                if (auto* candidate = object->GetComponent<Engine::Components::Mesh>();
                    candidate && !candidate->GetVertices().empty())
                {
                    meshes.push_back(candidate);
                    meshLabels.push_back(object->name);
                }
        if (meshes.empty())
        { ui.DisabledLabel("No editable meshes in this scene."); return; }
        if (std::find(meshes.begin(), meshes.end(), session->bindingMesh) == meshes.end())
            session->bindingMesh = std::find(meshes.begin(), meshes.end(),
                session->mesh) != meshes.end() ? session->mesh : meshes.front();
        int selectedMesh = static_cast<int>(std::find(meshes.begin(),
            meshes.end(), session->bindingMesh) - meshes.begin());
        if (selectedMesh >= static_cast<int>(meshes.size())) selectedMesh = 0;
        std::vector<const char*> meshNames;
        for (const auto& label : meshLabels) meshNames.push_back(label.c_str());
        if (ui.Combo("Mesh to bind", &selectedMesh, meshNames.data(),
            static_cast<int>(meshNames.size())))
            session->bindingMesh = meshes[selectedMesh];
        auto* mesh = session->bindingMesh;
        auto* skin = mesh->Owner->GetComponent<Engine::Components::SkinnedMesh>();
        const auto* current = skin ? skin->ResolveSkeleton() : nullptr;
        ui.ValueLabel("Current skeleton", current && current->Owner
            ? current->Owner->name.c_str() : "Unbound");
        std::vector<bool> resolved(bones.size());
        for (size_t i = 0; i < bones.size(); ++i) resolved[i] = bones[i] != nullptr;
        const auto diagnostics = SkinBindingWeights::Inspect(
            mesh->GetVertices(), resolved);
        const std::string summary = std::to_string(diagnostics.unweighted) +
            " unweighted, " + std::to_string(diagnostics.missingJoints) +
            " with missing joints, " +
            std::to_string(diagnostics.unnormalized) + " unnormalized";
        ui.ValueLabel("Weight diagnostics", summary.c_str());
        if (!session->bindingStatus.empty())
            ui.ColoredLabel(session->bindingStatus.c_str(),
                { 1.f, .7f, .3f, 1.f });
        if (!toolbarPopup && ui.Button("Bind / rebind preserving weights"))
            ApplySkinBindingAction(scene, *session, 0);
        if (!toolbarPopup && ui.Button("Generate initial weights and bind"))
            ApplySkinBindingAction(scene, *session, 1);
        if (!toolbarPopup && ui.Button("Repair missing / unweighted vertices"))
            ApplySkinBindingAction(scene, *session, 2);
        ui.InputText("Vertex IDs (e.g. 0,2-5)", session->bindingVertexIds,
            sizeof(session->bindingVertexIds));
        if (!toolbarPopup && ui.Button("Assign vertices to selected bone"))
            ApplySkinBindingAction(scene, *session, 3);
        if (auto* edit = ActiveMeshEditSession(); edit &&
            edit->activeMesh == mesh && edit->selectionMode == 0 &&
            !edit->selectedElements.empty() && !toolbarPopup &&
            ui.Button("Assign Mesh Edit selection"))
            ApplySkinBindingAction(scene, *session, 4);
        if (!toolbarPopup && ui.Button("Save binding and weights"))
            SaveScene();
    }
}

}

#include "pch.h"
#include "Engine/Editor/EditorState.h"
#include "Engine/Editor/UI/IEditorUi.h"
#include "Engine/Editor/Core/View/Views/SceneView.h"
#include "Engine/Editor/Core/View/Views/HierarchyView.h"
#include "Engine/Editor/Core/View/Views/PropertiesView.h"
#include "Core/Scene/Scene.h"
#include "Core/Object.h"
#include "Core/Compoonents/Animation/Skeleton.h"
#include "Core/Compoonents/Animation/BoneGroup.h"
#include "Core/Compoonents/Animation/Model.h"
#include "Core/Compoonents/Animation/SkinnedMesh.h"
#include "Core/Compoonents/Obj/Mesh.h"
#include "Engine/Editor/Core/View/Templates/EditModes/Skeleton/SkinBindingWeights.h"
#include "Engine/Editor/Core/Paint/BrushPreset.h"
#include "Engine/Editor/Core/Paint/BrushPreview.h"
#include <algorithm>
#include <filesystem>

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

bool ExplicitRigSelection(Engine::Components::Skeleton* skeleton,
    Engine::Core::Object* selected)
{
    if (!skeleton || !selected) return false;
    if (selected == skeleton->Owner || ValidBone(skeleton, selected))
        return true;
    if (auto* skin = selected->GetComponent<Engine::Components::SkinnedMesh>())
        return skin->ResolveSkeleton() == skeleton;
    return false;
}

size_t RigOrdinal(Engine::Scene::Scene& scene,
    Engine::Components::Skeleton* target)
{
    auto* prefabRoot = target->Owner->GetPrefabInstanceRoot();
    size_t ordinal = 0;
    for (const auto& object : scene.GetObjects())
    {
        if (!object || (prefabRoot &&
            object->GetPrefabInstanceRoot() != prefabRoot)) continue;
        for (auto* component : object->Components)
            if (auto* candidate = dynamic_cast<
                    Engine::Components::Skeleton*>(component))
            {
                if (candidate == target) return ordinal;
                ++ordinal;
            }
    }
    return ordinal;
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
                view->AllowObjectTransform =
                    (session->submode == 0 && session->boneTool == 0) ||
                    (session->submode == 1 && session->paintTool == 0);
        SyncSkeletonEditSelection(scene, *session);
    }
    auto* skeleton = session->skeleton;
    if (!skeleton)
    { ui.DisabledLabel("Select an object containing a skeleton."); return; }
    if (!toolbarPopup && ui.Button("Edit Animation"))
        QueueAnimationObjectOpen(skeleton->Owner);
    const auto& bones = skeleton->ResolveJoints();
    const auto* selectedGroup = Engine::Components::BoneGroup::ForSelection(
        scene->GetSelectedObject());
    std::vector<std::string> labels;
    std::vector<const char*> names;
    std::vector<int> boneChoices;
    labels.reserve(bones.size()); names.reserve(bones.size());
    for (size_t i = 0; i < bones.size(); ++i)
        if (!selectedGroup ||
            (selectedGroup->skinIndex == skeleton->skinIndex &&
                selectedGroup->Contains(bones[i])))
        {
            boneChoices.push_back(static_cast<int>(i));
            labels.push_back(bones[i] ? bones[i]->name : "Missing joint");
        }
    for (const auto& label : labels) names.push_back(label.c_str());
    auto foundChoice = std::find(boneChoices.begin(), boneChoices.end(),
        session->boneIndex);
    int selected = foundChoice == boneChoices.end() ? 0
        : static_cast<int>(foundChoice - boneChoices.begin());
    if (!names.empty() && ui.Combo("Bone", &selected, names.data(),
        static_cast<int>(names.size())))
    {
        FinishSkeletonPaintStroke(*session);
        session->boneIndex = boneChoices[selected];
        auto* object = bones[session->boneIndex];
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
        auto* displaySkeleton = skeleton;
        if (auto* model = skeleton->ResolveModel(); model && model->Owner)
            if (auto* root = model->Owner->GetComponent<
                    Engine::Components::Skeleton>();
                root && !root->rootBoneNodes.empty())
                displaySkeleton = root;
        if (ui.Checkbox("Show bone overlays", &displaySkeleton->showBones))
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
        std::vector<int> parentChoices;
        if (!selectedGroup)
        { parents.push_back("Model root"); parentChoices.push_back(-1); }
        for (size_t choice = 0; choice < labels.size(); ++choice)
        {
            parents.push_back(labels[choice].c_str());
            parentChoices.push_back(boneChoices[choice]);
        }
        auto foundParent = std::find(parentChoices.begin(), parentChoices.end(),
            session->reparentBoneIndex);
        int parentChoice = foundParent == parentChoices.end() ? 0
            : static_cast<int>(foundParent - parentChoices.begin());
        if (!parents.empty() && ui.Combo("New bone parent", &parentChoice,
                parents.data(), static_cast<int>(parents.size())))
            session->reparentBoneIndex = parentChoices[parentChoice];
        if (foundParent == parentChoices.end() && !parentChoices.empty())
            session->reparentBoneIndex = parentChoices.front();
        if (!toolbarPopup && ui.Button("Reparent selected bone"))
            ApplySkeletonBoneAction(scene, *session, 5);
        if (!toolbarPopup && ui.Button("Mirror selected bone (X)"))
            ApplySkeletonBoneAction(scene, *session, 6);
        if (!toolbarPopup && ui.Button("Apply bind pose"))
            ApplySkeletonBoneAction(scene, *session, 2);
        if (!toolbarPopup && session->boneTool == 0 &&
            ValidBone(skeleton, scene->GetSelectedObject()) &&
            (!selectedGroup || selectedGroup->Contains(scene->GetSelectedObject())))
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
        auto foundMirror = std::find(boneChoices.begin(), boneChoices.end(),
            session->mirrorBoneIndex);
        if (session->mirrorBoneIndex >= 0 &&
            foundMirror == boneChoices.end())
        {
            FinishSkeletonPaintStroke(*session);
            session->mirrorBoneIndex = -1;
        }
        int mirrorChoice = session->mirrorBoneIndex < 0 ? 0
            : static_cast<int>(foundMirror - boneChoices.begin()) + 1;
        if (ui.Combo("Mirror paint onto bone", &mirrorChoice,
                mirrorNames.data(), static_cast<int>(mirrorNames.size())))
        {
            FinishSkeletonPaintStroke(*session);
            session->mirrorBoneIndex = mirrorChoice == 0 ? -1
                : boneChoices[mirrorChoice - 1];
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
        Paint::BrushPreset preview;
        preview.shape = session->brushShape;
        preview.falloff = session->brushFalloff;
        preview.hardness = session->brushHardness;
        Paint::DrawBrushPreview(preview);
        namespace fs = std::filesystem;
        const fs::path assetsDirectory = fs::path(
            m_projectSettings.assetsDirectory.empty() ? "Assets" :
                m_projectSettings.assetsDirectory);
        const fs::path brushDirectory = assetsDirectory / "Brushes";
        std::vector<fs::path> presetPaths;
        std::error_code brushError;
        if (fs::is_directory(assetsDirectory, brushError))
            for (fs::recursive_directory_iterator it(assetsDirectory,
                    fs::directory_options::skip_permission_denied, brushError), end;
                !brushError && it != end; it.increment(brushError))
                if (it->is_regular_file() && it->path().extension() == ".brush")
                    presetPaths.push_back(it->path());
        std::sort(presetPaths.begin(), presetPaths.end());
        std::vector<std::string> presetLabels{ "Custom" };
        int presetChoice = 0;
        for (const auto& path : presetPaths)
        {
            presetLabels.push_back(path.lexically_relative(assetsDirectory).string());
            if (path.string() == session->brushPresetPath)
                presetChoice = static_cast<int>(presetLabels.size()) - 1;
        }
        std::vector<const char*> presetNames;
        for (const auto& label : presetLabels)
            presetNames.push_back(label.c_str());
        if (ui.Combo("Brush preset", &presetChoice, presetNames.data(),
            static_cast<int>(presetNames.size())))
        {
            FinishSkeletonPaintStroke(*session);
            if (presetChoice == 0) session->brushPresetPath.clear();
            else
            {
                Paint::BrushPreset preset;
                if (Paint::LoadBrushPreset(presetPaths[presetChoice - 1],
                    preset, session->error))
                {
                    session->brushPresetPath = presetPaths[presetChoice - 1].string();
                    session->brushShape = preset.shape;
                    session->brushFalloff = preset.falloff;
                    session->brushRadius = preset.size;
                    session->brushHardness = preset.hardness;
                    session->brushStrength = preset.strength;
                    session->brushOpacity = preset.opacity;
                    session->brushFlow = preset.flow;
                    session->brushSpacing = preset.spacing;
                    session->brushRotation = preset.rotation;
                    session->brushScatter = preset.scatter;
                    session->brushPressureSize = preset.pressureSize;
                    session->brushPressureStrength = preset.pressureStrength;
                    session->brushTipPath = preset.tipPath;
                    const size_t nameLength = std::min(preset.name.size(),
                        sizeof(session->brushPresetName) - 1);
                    std::copy_n(preset.name.data(), nameLength,
                        session->brushPresetName);
                    session->brushPresetName[nameLength] = 0;
                }
            }
        }
        ui.InputText("Preset name", session->brushPresetName,
            sizeof(session->brushPresetName));
        const auto savePreset = [&](bool duplicate)
        {
            Paint::BrushPreset preset;
            preset.name = session->brushPresetName;
            preset.tipPath = session->brushTipPath;
            preset.shape = session->brushShape;
            preset.falloff = session->brushFalloff;
            preset.size = session->brushRadius;
            preset.hardness = session->brushHardness;
            preset.strength = session->brushStrength;
            preset.opacity = session->brushOpacity;
            preset.flow = session->brushFlow;
            preset.spacing = session->brushSpacing;
            preset.rotation = session->brushRotation;
            preset.scatter = session->brushScatter;
            preset.pressureSize = session->brushPressureSize;
            preset.pressureStrength = session->brushPressureStrength;
            fs::create_directories(brushDirectory, brushError);
            if (brushError)
            { session->error = brushError.message(); return; }
            fs::path path = session->brushPresetPath;
            if (duplicate || path.empty())
            {
                std::string base = preset.name;
                for (char& ch : base)
                    if (ch == '/' || ch == '\\' || ch == ':' || ch == '*' ||
                        ch == '?' || ch == '"' || ch == '<' || ch == '>' || ch == '|')
                        ch = '_';
                path = brushDirectory / (base + ".brush");
                for (int suffix = 2; fs::exists(path); ++suffix)
                    path = brushDirectory /
                        (base + " " + std::to_string(suffix) + ".brush");
            }
            if (Paint::SaveBrushPreset(path, preset, session->error))
                session->brushPresetPath = path.string();
        };
        if (ui.Button("Create brush asset")) savePreset(true);
        if (!session->brushPresetPath.empty() &&
            ui.Button("Save brush asset")) savePreset(false);
        if (!session->brushPresetPath.empty() &&
            ui.Button("Duplicate brush asset")) savePreset(true);
        const char* paintTools[]{ "Cursor", "Brush", "Eraser", "Bucket" };
        int activePaintTool = session->paintTool;
        if (ui.Combo("Paint tool", &activePaintTool, paintTools, 4))
        {
            FinishSkeletonPaintStroke(*session);
            session->paintTool = activePaintTool;
            for (const auto& panel : m_panels)
                if (auto* view = dynamic_cast<SceneView*>(panel.get());
                    view && view->GetScene() == scene)
                    view->AllowObjectTransform = activePaintTool == 0;
        }
        if (session->paintTool == 1)
            ui.Combo("Brush", &session->brushOperation, operations, 5);
        if (session->paintTool == 1 || session->paintTool == 2)
        {
            ui.Combo("Shape", &session->brushShape, shapes, 2);
            ui.Combo("Feather", &session->brushFalloff, falloffs, 3);
            ui.SliderFloat("Size (pixels)", &session->brushRadius, 2.f, 250.f);
            if (session->brushFalloff != 0)
                ui.SliderFloat("Hardness", &session->brushHardness, 0.f, 1.f);
        }
        if (session->paintTool == 3)
        {
            ui.SliderFloat("Bucket tolerance", &session->bucketTolerance,
                0.f, 1.f);
            ui.SliderFloat("Bucket opacity", &session->bucketOpacity,
                0.f, 1.f);
            ui.SliderFloat("Fill weight", &session->brushPaintWeight,
                0.f, 1.f);
            ui.DisabledLabel("Bucket fills connected faces with similar starting weights.");
            if (session->mirrorBoneIndex >= 0)
                ui.DisabledLabel("Turn off mirror paint before using Bucket.");
        }
        else if (session->paintTool == 1 && session->brushOperation <= 2)
            ui.SliderFloat("Paint weight", &session->brushPaintWeight, 0.f, 1.f);
        if (session->paintTool == 1 || session->paintTool == 2)
        {
            ui.SliderFloat("Brush strength", &session->brushStrength, .01f, 1.f);
            ui.SliderFloat("Opacity", &session->brushOpacity, 0.f, 1.f);
            ui.SliderFloat("Flow", &session->brushFlow, 0.f, 1.f);
            ui.SliderFloat("Spacing (diameter)", &session->brushSpacing,
                .01f, 1.f);
            if (session->brushShape == 1)
                ui.SliderFloat("Rotation (degrees)", &session->brushRotation,
                    -180.f, 180.f);
            ui.SliderFloat("Scatter", &session->brushScatter, 0.f, 1.f);
        }
        if (session->paintTool == 2)
            ui.DisabledLabel("Erase redistributes weight to other unlocked bones.");
        else if (session->paintTool == 3)
            ui.DisabledLabel("Click a mesh surface to fill its connected region.");
        else if (session->paintTool == 1)
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

#include "pch.h"
#include "Engine/Editor/EditorState.h"
#include "Engine/Editor/UI/IEditorUi.h"
#include "Engine/Editor/Core/View/Views/SceneView.h"
#include "Core/Scene/Scene.h"
#include "Core/Object.h"
#include "Core/Compoonents/Obj/Mesh.h"
#include "Core/Prefab/PrefabAsset.h"
#include "Core/Serialization/SceneSerializer.h"
#include "imgui.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <string_view>

namespace Engine::Editor
{
namespace
{
std::string ToolbarIconLabel(ImWchar codepoint, const char* glyph,
    const char* suffix, const char* id, const char* fallback)
{
    std::string label = ImGui::GetFont()->IsGlyphInFont(codepoint)
        ? std::string(glyph) + suffix : fallback;
    label += "##";
    label += id;
    return label;
}

bool ToolbarIconButton(ImWchar codepoint, const char* glyph,
    const char* suffix, const char* id, const char* fallback,
    const char* tooltip)
{
    const std::string label = ToolbarIconLabel(codepoint, glyph,
        suffix, id, fallback);
    const bool clicked = ImGui::Button(label.c_str());
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", tooltip);
    return clicked;
}
}

float EditorState::GetGlobalToolbarHeight() const
{
    const float playWidth = m_toolbarVisibility.playControls ? 150.f : 0.f;
    if (m_gameViewFocused)
    {
        const float width = std::max(180.f,
            ImGui::GetMainViewport()->WorkSize.x - 24.f);
        return 10.f + 28.f * std::max(1,
            static_cast<int>(std::ceil((90.f + playWidth) / width)));
    }
    if (m_activeAssetDocument)
    {
        const float width = std::max(180.f,
            ImGui::GetMainViewport()->WorkSize.x - 24.f);
        const float contentWidth = 120.f + playWidth +
            (m_toolbarVisibility.save ? 110.f : 0.f) +
            (m_toolbarVisibility.undoRedo ? 80.f : 0.f);
        return 10.f + 28.f * std::max(1,
            static_cast<int>(std::ceil(contentWidth / width)));
    }
    const MeshEditSession& mesh = m_activeSceneAssetDocument
        ? m_activeSceneAssetDocument->meshEdit
        : m_prefabDocumentFocused ? m_prefabMeshEdit : m_mainMeshEdit;
    const SkeletonEditSession& skeleton = m_activeSceneAssetDocument
        ? m_activeSceneAssetDocument->skeletonEdit
        : m_prefabDocumentFocused ? m_prefabSkeletonEdit : m_mainSkeletonEdit;
    const float width = std::max(220.f,
        ImGui::GetMainViewport()->WorkSize.x - 24.f);
    const float topWidth = 120.f + playWidth +
        (m_toolbarVisibility.editMode ? 115.f : 0.f) +
        (m_toolbarVisibility.save ? 115.f : 0.f) +
        (m_toolbarVisibility.undoRedo ? 80.f : 0.f) +
        (m_toolbarVisibility.modeTools
            ? (skeleton.enabled ? 300.f : mesh.enabled ? 170.f : 0.f)
            : 0.f) +
        (m_toolbarVisibility.transformTools ? 300.f : 0.f) +
        (m_toolbarVisibility.sceneDisplay ? 265.f : 0.f);
    Engine::Scene::Scene* scene = GetActiveDocumentScene();
    Engine::Core::Object* selected = scene ? scene->GetSelectedObject() : nullptr;
    Engine::Core::Object* prefabRoot = selected
        ? selected->GetPrefabInstanceRoot() : nullptr;
    const float prefabWidth = m_toolbarVisibility.prefabActions &&
        prefabRoot && prefabRoot->Prefab ? 420.f : 0.f;
    const int topRows = std::max(1, static_cast<int>(std::ceil(
        (topWidth + prefabWidth) / width)));
    const float topHeight = 10.f + topRows * 28.f;
    if (!m_toolbarVisibility.toolDetails) return topHeight;
    const float toolWidth = mesh.enabled
        ? (m_activeSceneAssetDocument && m_activeSceneAssetDocument->meshStage
            ? 410.f : 210.f) : skeleton.enabled ? 550.f :
        m_activeSceneAssetDocument && m_activeSceneAssetDocument->meshStage
            ? 300.f : 250.f;
    const int rows = std::max(1, static_cast<int>(std::ceil(
        toolWidth / width)));
    return topHeight + rows * 27.f + 8.f;
}

bool EditorState::HasToolbarToolRow() const
{
    return m_toolbarVisibility.toolDetails && !m_gameViewFocused &&
        !m_activeAssetDocument && GetActiveDocumentScene();
}

void EditorState::DrawToolbarTools(IEditorUi& ui)
{
    if (!m_toolbarVisibility.toolDetails || m_gameViewFocused ||
        m_activeAssetDocument) return;
    Engine::Scene::Scene* scene = GetActiveDocumentScene();
    if (!scene) return;

    bool previousItem = false;
    const auto button = [&](const char* label)
    {
        const ImGuiStyle& style = ImGui::GetStyle();
        const float width = ImGui::CalcTextSize(label).x +
            style.FramePadding.x * 2.f;
        const float right = ImGui::GetWindowPos().x +
            ImGui::GetWindowContentRegionMax().x;
        if (previousItem && ImGui::GetItemRectMax().x +
            style.ItemSpacing.x + width <= right)
            ImGui::SameLine();
        previousItem = true;
        return ImGui::SmallButton(label);
    };
    const auto iconButton = [&](ImWchar codepoint, const char* glyph,
        const char* suffix, const char* id, const char* fallback,
        const char* tooltip)
    {
        const std::string label = ToolbarIconLabel(codepoint, glyph,
            suffix, id, fallback);
        const bool clicked = button(label.c_str());
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("%s", tooltip);
        return clicked;
    };
    MeshEditSession* mesh = ActiveMeshEditSession();
    SkeletonEditSession* skeleton = ActiveSkeletonEditSession();
    const int activeMode = skeleton && skeleton->enabled ? 2 :
        mesh && mesh->enabled ? 1 : 0;
    const auto open = [&](const char* label, const char* tool)
    {
        if (button(label))
        {
            m_toolbarPopupTool = tool;
            m_toolbarPopupScene = scene;
            m_toolbarPopupMode = activeMode;
            ImGui::OpenPopup("##ToolbarToolPopup");
        }
    };
    const auto showError = [&](const char* message)
    {
        if (!message || !*message) return;
        const ImGuiStyle& style = ImGui::GetStyle();
        const float right = ImGui::GetWindowPos().x +
            ImGui::GetWindowContentRegionMax().x;
        if (previousItem && ImGui::GetItemRectMax().x +
            style.ItemSpacing.x + 12.f <= right)
            ImGui::SameLine();
        ImGui::TextColored({1.f, .55f, .3f, 1.f}, "!");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", message);
        previousItem = true;
    };
    if (skeleton && skeleton->enabled)
    {
        SyncSkeletonEditSelection(scene, *skeleton);
        if (skeleton->submode == 0)
        {
            open("Bone...", "Bone Settings");
            if (iconButton(0xE710, u8"\uE710", " Bone", "AddBone",
                "Add Bone", "Add child bone"))
                ApplySkeletonBoneAction(scene, *skeleton, 0);
            if (iconButton(0xE738, u8"\uE738", " Bone", "RemoveBone",
                "Remove Bone", "Remove selected bone"))
                ApplySkeletonBoneAction(scene, *skeleton, 1);
            if (button("Bind Pose")) ApplySkeletonBoneAction(scene, *skeleton, 2);
            open("Bone Transform...", "Bone Transform");
        }
        else
        {
            open("Brush...", "Brush Settings");
            if (m_toolbarVisibility.save && mesh &&
                iconButton(0xE74E, u8"\uE74E", mesh->dirty ? " *" : "",
                    "SaveWeights", mesh->dirty ? "Save Weights *" : "Save Weights",
                    "Save Weights"))
            {
                FinishSkeletonPaintStroke(*skeleton);
                SaveMeshEditSession(*mesh);
            }
        }
        showError(skeleton->error.c_str());
    }
    else if (mesh && mesh->enabled)
    {
        SyncMeshEditSelection(scene, *mesh);
        const bool meshReady = mesh->activeMesh != nullptr;
        const bool topologyReady = meshReady &&
            !mesh->activeMesh->HasMorphTargets();
        if (button("Mesh Tools..."))
        {
            m_meshToolbarSearch[0] = '\0';
            m_meshToolbarScene = scene;
            m_meshToolbarSelectionMode = mesh->selectionMode;
            ImGui::OpenPopup("##MeshToolChooser");
        }
        const char* directTool = nullptr;
        const char* settingsTool = nullptr;
        if (ImGui::IsPopupOpen("##MeshToolChooser"))
        {
            ImGui::SetNextWindowSizeConstraints({240.f, 0.f}, {360.f, 500.f});
            constexpr ImGuiWindowFlags chooserFlags = ImGuiWindowFlags_NoDocking |
                ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                ImGuiWindowFlags_AlwaysAutoResize;
            if (ImGui::BeginPopup("##MeshToolChooser", chooserFlags))
            {
                if (m_meshToolbarScene != scene ||
                    m_meshToolbarSelectionMode != mesh->selectionMode)
                    ImGui::CloseCurrentPopup();
                else
                {
                    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
                    ImGui::InputTextWithHint("##MeshToolSearch", "Search mesh tools",
                        m_meshToolbarSearch, sizeof(m_meshToolbarSearch));
                    ImGui::Separator();
                    struct MeshToolChoice
                    {
                        const char* label;
                        const char* tool;
                        int selectionMode;
                        bool settings;
                    };
                    constexpr MeshToolChoice choices[] = {
                        {"Transform", "Transform", -1, true},
                        {"Weld vertices", "Weld vertices", 0, false},
                        {"Split edge", "Split edge", 1, false},
                        {"Bevel edge", "Bevel edge", 1, true},
                        {"Loop cut", "Loop cut", 1, false},
                        {"Fill boundary", "Fill boundary", 1, false},
                        {"Bridge boundaries", "Bridge boundaries", 1, false},
                        {"Extrude face", "Extrude face", 2, true},
                        {"Inset face", "Inset face", 2, true},
                        {"Duplicate faces", "Duplicate faces", 2, true},
                        {"Delete selection", "Delete selection", -1, false},
                    };
                    std::string search = m_meshToolbarSearch;
                    std::transform(search.begin(), search.end(), search.begin(),
                        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                    bool hasMatch = false;
                    for (const MeshToolChoice& choice : choices)
                    {
                        std::string label = choice.label;
                        std::transform(label.begin(), label.end(), label.begin(),
                            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                        if (label.find(search) == std::string::npos) continue;
                        hasMatch = true;
                        const bool available = (choice.selectionMode < 0 ||
                            choice.selectionMode == mesh->selectionMode) &&
                            (std::string_view(choice.tool) == "Transform"
                                ? meshReady : topologyReady);
                        ImGui::BeginDisabled(!available);
                        if (ImGui::Selectable(choice.label))
                        {
                            if (choice.settings) settingsTool = choice.tool;
                            else directTool = choice.tool;
                            ImGui::CloseCurrentPopup();
                        }
                        ImGui::EndDisabled();
                    }
                    if (!hasMatch) ImGui::TextDisabled("No matching tools");
                }
                ImGui::EndPopup();
            }
        }
        if (directTool)
        {
            m_toolbarDirectTool = directTool;
            DrawMeshEditTools(ui);
            m_toolbarDirectTool.clear();
        }
        if (settingsTool)
        {
            m_toolbarPopupTool = settingsTool;
            m_toolbarPopupScene = scene;
            m_toolbarPopupMode = activeMode;
            ImGui::OpenPopup("##ToolbarToolPopup");
        }
        ImGui::BeginDisabled(!meshReady);
        if (m_toolbarVisibility.save &&
            iconButton(0xE74E, u8"\uE74E", mesh->dirty ? " *" : "",
                "SaveMesh", mesh->dirty ? "Save Mesh *" : "Save Mesh",
                "Save Mesh"))
            SaveMeshEditSession(*mesh);
        ImGui::EndDisabled();
        if (m_activeSceneAssetDocument &&
            m_activeSceneAssetDocument->meshStage)
            open("Vertex Attributes...", "Vertex Attributes");
        if (m_activeSceneAssetDocument &&
            m_activeSceneAssetDocument->meshStage)
            open("UV Map...", "UV Map");
        showError(mesh->toolError.c_str());
    }
    else if (m_activeSceneAssetDocument &&
        m_activeSceneAssetDocument->meshStage)
    {
        open("Vertex Attributes...", "Vertex Attributes");
        if (m_toolbarVisibility.save &&
            iconButton(0xE74E, u8"\uE74E", "", "SaveStageMesh",
                "Save Mesh", "Save Mesh"))
            SaveSceneAssetDocument(*m_activeSceneAssetDocument);
    }
    else
    {
        open("Transform...", "Object Transform");
        if (m_activeSceneAssetDocument &&
            m_activeSceneAssetDocument->objectStage)
            open("Hierarchy...", "Hierarchy");
    }

    if (ImGui::IsPopupOpen("##ToolbarToolPopup"))
    {
        ImGui::SetNextWindowSizeConstraints({290.f, 0.f}, {460.f, 650.f});
        constexpr ImGuiWindowFlags popupFlags = ImGuiWindowFlags_NoDocking |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_AlwaysAutoResize;
        if (ImGui::BeginPopup("##ToolbarToolPopup", popupFlags))
        {
            if (m_toolbarPopupScene != scene ||
                m_toolbarPopupMode != activeMode)
            {
                ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
                m_toolbarPopupTool.clear();
                return;
            }
            ImGui::TextUnformatted(m_toolbarPopupTool.c_str());
            ImGui::Separator();
            if (m_toolbarPopupTool == "Object Transform" ||
                m_toolbarPopupTool == "Bone Transform")
                DrawObjectEditTools(ui);
            else if (m_toolbarPopupTool == "Hierarchy" &&
                m_activeSceneAssetDocument)
                DrawObjectStageTools(ui, *m_activeSceneAssetDocument);
            else if ((m_toolbarPopupTool == "Vertex Attributes" ||
                m_toolbarPopupTool == "UV Map") &&
                m_activeSceneAssetDocument)
                DrawMeshStageTools(ui, *m_activeSceneAssetDocument);
            else if (m_toolbarPopupTool == "Bone Settings" ||
                m_toolbarPopupTool == "Brush Settings")
                DrawSkeletonEditTools(ui);
            else
                DrawMeshEditTools(ui);
            ImGui::EndPopup();
        }
    }
    else m_toolbarPopupTool.clear();
}

void EditorState::DrawGlobalToolbar(IEditorUi& ui)
{
    if (m_gameViewFocused)
    {
        m_toolbarPopupTool.clear();
        m_toolbarPopupScene = nullptr;
        ui.Label("Game view");
        return;
    }
    if (m_activeAssetDocument)
    {
        m_toolbarPopupTool.clear();
        m_toolbarPopupScene = nullptr;
        ui.Label("Asset document");
        if (m_toolbarVisibility.save)
        {
            ImGui::BeginDisabled(!m_playModeSceneSnapshot.empty());
            const float right = ImGui::GetWindowPos().x +
                ImGui::GetWindowContentRegionMax().x;
            if (ImGui::GetItemRectMax().x + 36.f <= right)
                ui.SameLine();
            if (ToolbarIconButton(0xE74E, u8"\uE74E", "",
                "SaveAsset", "Save Asset", "Save Asset")) SaveScene();
            if (ImGui::GetItemRectMax().x + 70.f <= right)
                ui.SameLine();
            if (ToolbarIconButton(0xE74E, u8"\uE74E", " All",
                "SaveAllAssets", "Save All", "Save All")) SaveAll();
            ImGui::EndDisabled();
        }
        if (m_toolbarVisibility.undoRedo)
        {
            const float right = ImGui::GetWindowPos().x +
                ImGui::GetWindowContentRegionMax().x;
            if (ImGui::GetItemRectMax().x + 36.f <= right)
                ui.SameLine();
            ImGui::BeginDisabled(!CanUndo() ||
                !m_playModeSceneSnapshot.empty());
            if (ToolbarIconButton(0xE7A7, u8"\uE7A7", "",
                "UndoAsset", "Undo", "Undo")) Undo();
            ImGui::EndDisabled();
            if (ImGui::GetItemRectMax().x + 36.f <= right)
                ui.SameLine();
            ImGui::BeginDisabled(!CanRedo() ||
                !m_playModeSceneSnapshot.empty());
            if (ToolbarIconButton(0xE7A6, u8"\uE7A6", "",
                "RedoAsset", "Redo", "Redo")) Redo();
            ImGui::EndDisabled();
        }
        return;
    }
    Engine::Scene::Scene* scene = GetActiveDocumentScene();
    if (!scene) return;
    SceneView* view = nullptr;
    if (m_activeSceneAssetDocument)
        view = m_activeSceneAssetDocument->view;
    else if (m_prefabDocumentFocused)
        view = m_prefabSceneView;
    else
        for (const auto& panel : m_panels)
            if (auto* candidate = dynamic_cast<SceneView*>(panel.get());
                candidate && candidate->GetScene() == scene)
            { view = candidate; break; }
    if (!view || !view->UseGlobalToolbar)
    {
        ui.Label("Document tools are in the active viewport");
        return;
    }

    MeshEditSession& mesh = m_activeSceneAssetDocument
        ? m_activeSceneAssetDocument->meshEdit
        : m_prefabDocumentFocused ? m_prefabMeshEdit : m_mainMeshEdit;
    SkeletonEditSession& skeleton = m_activeSceneAssetDocument
        ? m_activeSceneAssetDocument->skeletonEdit
        : m_prefabDocumentFocused ? m_prefabSkeletonEdit : m_mainSkeletonEdit;
    ui.Label(m_activeSceneAssetDocument ? "Document" :
        m_prefabDocumentFocused ? "Prefab" : "Scene");
    const auto nextItem = [&](float width)
    {
        const ImGuiStyle& style = ImGui::GetStyle();
        const float right = ImGui::GetWindowPos().x +
            ImGui::GetWindowContentRegionMax().x;
        if (ImGui::GetItemRectMax().x + style.ItemSpacing.x + width <= right)
            ui.SameLine();
    };
    nextItem(105.f);
    if (m_toolbarVisibility.editMode && m_activeSceneAssetDocument &&
        m_activeSceneAssetDocument->modelStage &&
        m_activeSceneAssetDocument->focusPicker)
    {
        ui.SetNextItemWidth(105.f);
        m_activeSceneAssetDocument->focusPicker(ui);
    }
    else if (m_toolbarVisibility.editMode && m_activeSceneAssetDocument &&
        m_activeSceneAssetDocument->meshStage)
        ui.Label("Mesh");
    else if (m_toolbarVisibility.editMode)
    {
        int mode = skeleton.enabled ? 2 : mesh.enabled ? 1 : 0;
        const char* modes[]{ "Object", "Mesh", "Skeleton" };
        ui.SetNextItemWidth(105.f);
        if (ui.Combo("##GlobalEditMode", &mode, modes, 3))
        {
            SetSkeletonEditMode(scene, skeleton, mesh, false);
            SetMeshEditMode(scene, mesh, mode == 1);
            if (mode == 2) SetSkeletonEditMode(scene, skeleton, mesh, true);
        }
        ui.Tooltip("Edit mode");
    }
    if (m_toolbarVisibility.save)
    {
        ImGui::BeginDisabled(!m_playModeSceneSnapshot.empty());
        const char* saveLabel = m_activeSceneAssetDocument &&
            m_activeSceneAssetDocument->meshStage ? "Save Mesh" :
            (m_prefabDocumentFocused ||
                (m_activeSceneAssetDocument && m_activeSceneAssetDocument->prefab))
                ? "Save Prefab" : "Save Scene";
        nextItem(36.f);
        if (ToolbarIconButton(0xE74E, u8"\uE74E", "",
            "SaveCurrent", saveLabel, saveLabel))
        {
            if (m_activeSceneAssetDocument)
                SaveSceneAssetDocument(*m_activeSceneAssetDocument);
            else SaveScene();
        }
        nextItem(70.f);
        if (ToolbarIconButton(0xE74E, u8"\uE74E", " All",
            "SaveAll", "Save All", "Save All")) SaveAll();
        ImGui::EndDisabled();
    }
    if (m_toolbarVisibility.undoRedo)
    {
        nextItem(36.f);
        ImGui::BeginDisabled(!CanUndo() ||
            !m_playModeSceneSnapshot.empty());
        if (ToolbarIconButton(0xE7A7, u8"\uE7A7", "",
            "UndoDocument", "Undo", "Undo")) Undo();
        ImGui::EndDisabled();
        nextItem(36.f);
        ImGui::BeginDisabled(!CanRedo() ||
            !m_playModeSceneSnapshot.empty());
        if (ToolbarIconButton(0xE7A6, u8"\uE7A6", "",
            "RedoDocument", "Redo", "Redo")) Redo();
        ImGui::EndDisabled();
    }

    Engine::Core::Object* selectedObject = scene->GetSelectedObject();
    Engine::Core::Object* prefabRoot = selectedObject
        ? selectedObject->GetPrefabInstanceRoot() : nullptr;
    if (m_toolbarVisibility.prefabActions && prefabRoot && prefabRoot->Prefab)
    {
        ImGui::BeginDisabled(!m_playModeSceneSnapshot.empty());
        const bool hasOverrides =
            Engine::Serialization::SceneSerializer::HasPrefabOverrides(
                *prefabRoot, true);
        nextItem(36.f);
        if (ToolbarIconButton(0xE70F, u8"\uE70F", "",
            "EditPrefab", "Edit Prefab", "Edit Prefab"))
            QueueSceneAssetDocumentOpen(prefabRoot->Prefab->GetPath());
        ui.BeginDisabled(!hasOverrides);
        nextItem(125.f);
        if (ui.Button("Apply Overrides") &&
            Engine::Serialization::SceneSerializer::ApplyPrefabOverridesToAsset(
                *prefabRoot, false, scene->GetGraphicsProvider()))
            MarkSceneEdited();
        nextItem(80.f);
        if (ui.Button("Apply All") &&
            Engine::Serialization::SceneSerializer::ApplyPrefabOverridesToAsset(
                *prefabRoot, true, scene->GetGraphicsProvider()))
            MarkSceneEdited();
        nextItem(36.f);
        if (ToolbarIconButton(0xE72C, u8"\uE72C", "",
            "RevertPrefab", "Revert", "Revert prefab overrides"))
        {
            SelectObject(prefabRoot);
            if (Engine::Serialization::SceneSerializer::RevertPrefabOverrides(
                *prefabRoot, scene->GetGraphicsProvider()))
                MarkSceneEdited();
        }
        ui.EndDisabled();
        nextItem(115.f);
        if (ui.Button("Unpack Prefab"))
        {
            prefabRoot->Prefab.reset();
            prefabRoot->PrefabSourceSnapshot.clear();
            MarkSceneEdited();
        }
        ImGui::EndDisabled();
    }

    if (m_toolbarVisibility.modeTools && skeleton.enabled)
    {
        nextItem(110.f);
        const char* submodes[]{ "Bones", "Weight Paint" };
        int submode = skeleton.submode;
        ui.SetNextItemWidth(110.f);
        if (ui.Combo("##GlobalSkeletonTool", &submode, submodes, 2))
        {
            FinishSkeletonPaintStroke(skeleton);
            scene->SetEditorWeightPaint(nullptr, -1);
            skeleton.submode = submode;
            view->AllowObjectTransform = submode == 0 &&
                skeleton.boneTool == 0;
            SyncSkeletonEditSelection(scene, skeleton);
        }
        ui.Tooltip("Skeleton tool");
        if (skeleton.submode == 1)
        {
            nextItem(100.f);
            const char* brushes[]{ "Add", "Subtract", "Replace", "Smooth",
                "Normalize" };
            ui.SetNextItemWidth(100.f);
            ui.Combo("##GlobalWeightBrush", &skeleton.brushOperation,
                brushes, 5);
            ui.Tooltip("Weight paint operation");
            if (ui.AvailableContentWidth() > 190.f)
            {
                nextItem(80.f);
                ui.SetNextItemWidth(80.f);
                ui.SliderFloat("##GlobalBrushSize", &skeleton.brushRadius,
                    2.f, 250.f);
                ui.Tooltip("Brush size in pixels");
            }
        }
    }
    else if (m_toolbarVisibility.modeTools && mesh.enabled)
    {
        nextItem(80.f);
        const char* elements[]{ "Vertex", "Edge", "Face" };
        ui.SetNextItemWidth(80.f);
        if (ui.Combo("##GlobalMeshElement", &mesh.selectionMode,
            elements, 3))
        {
            mesh.selectedElement = 0;
            mesh.selectedElements = { 0 };
            mesh.gizmoDragging = false;
            mesh.gizmoStartPositions.clear();
        }
        ui.Tooltip("Mesh element");
        nextItem(70.f);
        const char* selectionTools[]{ "Click", "Box", "Lasso" };
        ui.SetNextItemWidth(70.f);
        ui.Combo("##GlobalSelectionTool", &mesh.selectionTool,
            selectionTools, 3);
        ui.Tooltip("Selection tool");
    }
    if (m_toolbarVisibility.transformTools &&
        ui.AvailableContentWidth() > 340.f)
    {
        const EditorTransformTool tools[]{ EditorTransformTool::Hand,
            EditorTransformTool::Translate, EditorTransformTool::Rotate,
            EditorTransformTool::Scale };
        const char* toolNames[]{ "Hand", "Move", "Rotate", "Scale" };
        for (int index = 0; index < 4; ++index)
        {
            nextItem(75.f);
            const bool selected = view->GetTransformTool() == tools[index];
            const std::string label = std::string(toolNames[index]) +
                (selected ? " *" : "");
            if (ui.Button(label.c_str())) view->SetTransformTool(tools[index]);
        }
    }
    else if (m_toolbarVisibility.transformTools)
    {
        nextItem(100.f);
        const char* tools[]{ "Hand", "Move", "Rotate", "Scale" };
        int tool = view->GetTransformTool() == EditorTransformTool::Hand ? 0
            : view->GetTransformTool() == EditorTransformTool::Translate ? 1
            : view->GetTransformTool() == EditorTransformTool::Rotate ? 2 : 3;
        ui.SetNextItemWidth(100.f);
        if (ui.Combo("##GlobalTransformTool", &tool, tools, 4))
        {
            const EditorTransformTool values[]{ EditorTransformTool::Hand,
                EditorTransformTool::Translate, EditorTransformTool::Rotate,
                EditorTransformTool::Scale };
            view->SetTransformTool(values[tool]);
        }
        ui.Tooltip("Transform tool");
    }
    if (m_toolbarVisibility.sceneDisplay)
    {
        nextItem(55.f);
        if (ui.Button(scene->settings.showGrid ? "Grid *" : "Grid"))
            scene->settings.showGrid = !scene->settings.showGrid;
        nextItem(95.f);
        int renderMode = static_cast<int>(scene->settings.renderMode);
        const char* renderNames[]{ "Lit", "Unlit", "Wireframe" };
        ui.SetNextItemWidth(95.f);
        if (ui.Combo("##GlobalRenderMode", &renderMode, renderNames, 3))
            scene->settings.renderMode =
                static_cast<Engine::Model::SceneRenderMode>(renderMode);
        ui.Tooltip("Scene render mode");
        nextItem(100.f);
        if (ui.Button(scene->settings.sceneViewUiOverlay
            ? "UI Overlay *" : "UI Overlay"))
            scene->settings.sceneViewUiOverlay =
                !scene->settings.sceneViewUiOverlay;
        ui.Tooltip("Show scene UI in the editor viewport");
    }
}

}

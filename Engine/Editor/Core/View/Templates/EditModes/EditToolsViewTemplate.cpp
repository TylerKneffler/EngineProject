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
    const std::string display = label.substr(0, label.find("##"));
    const ImVec2 textSize = ImGui::CalcTextSize(display.c_str());
    const float width = std::max(24.f, textSize.x +
        ImGui::GetStyle().FramePadding.x * 2.f);
    const std::string buttonId = std::string("##") + id;
    const bool clicked = ImGui::Button(buttonId.c_str(), {width, 20.f});
    const ImVec2 minimum = ImGui::GetItemRectMin();
    const ImVec2 maximum = ImGui::GetItemRectMax();
    ImGui::GetWindowDrawList()->AddText(
        {minimum.x + (maximum.x - minimum.x - textSize.x) * .5f,
         minimum.y + (maximum.y - minimum.y - textSize.y) * .5f - 1.f},
        ImGui::GetColorU32(ImGuiCol_Text), display.c_str());
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", tooltip);
    return clicked;
}

float ToolbarButtonWidth(const char* label)
{
    return std::max(24.f, ImGui::CalcTextSize(label).x +
        ImGui::GetStyle().FramePadding.x * 2.f);
}

struct ToolbarRow
{
    bool singleRow;
    bool* overflow;
    bool previous = false;
    bool hidden = false;

    bool Next(float width)
    {
        const ImGuiStyle& style = ImGui::GetStyle();
        const float right = ImGui::GetWindowPos().x +
            ImGui::GetWindowContentRegionMax().x;
        const float x = previous ? ImGui::GetItemRectMax().x +
            style.ItemSpacing.x : ImGui::GetCursorScreenPos().x;
        if (singleRow && (hidden || x + width > right))
        {
            hidden = true;
            if (overflow) *overflow = true;
            return false;
        }
        if (previous && (singleRow || x + width <= right))
            ImGui::SameLine();
        previous = true;
        return true;
    }
};
}

float EditorState::GetGlobalToolbarHeight() const
{
    return 34.f;
}

bool EditorState::HasToolbarSection(ToolbarSection section) const
{
    if (m_gameViewFocused) return false;
    if (m_activeAssetDocument)
        return section == ToolbarSection::File ? m_toolbarVisibility.save :
            section == ToolbarSection::Edit && m_toolbarVisibility.undoRedo;
    Engine::Scene::Scene* scene = GetActiveDocumentScene();
    if (!scene) return false;
    switch (section)
    {
    case ToolbarSection::File: return m_toolbarVisibility.save;
    case ToolbarSection::Edit: return m_toolbarVisibility.undoRedo;
    case ToolbarSection::Mode:
        return m_toolbarVisibility.editMode || m_toolbarVisibility.modeTools;
    case ToolbarSection::Tools: return m_toolbarVisibility.toolDetails;
    case ToolbarSection::Transform: return m_toolbarVisibility.transformTools;
    case ToolbarSection::View: return m_toolbarVisibility.sceneDisplay;
    case ToolbarSection::Prefab:
    {
        Engine::Core::Object* selected = scene->GetSelectedObject();
        Engine::Core::Object* root = selected
            ? selected->GetPrefabInstanceRoot() : nullptr;
        return m_toolbarVisibility.prefabActions && root && root->Prefab;
    }
    }
    return false;
}

void EditorState::DrawToolbarTools(IEditorUi& ui, bool singleRow,
    bool* overflow)
{
    if (!m_toolbarVisibility.toolDetails || m_gameViewFocused ||
        m_activeAssetDocument) return;
    Engine::Scene::Scene* scene = GetActiveDocumentScene();
    if (!scene) return;

    ToolbarRow row{singleRow, overflow};
    const auto button = [&](const char* label)
    {
        return row.Next(ToolbarButtonWidth(label)) && ImGui::SmallButton(label);
    };
    const auto iconButton = [&](ImWchar codepoint, const char* glyph,
        const char* suffix, const char* id, const char* fallback,
        const char* tooltip)
    {
        const std::string label = ToolbarIconLabel(codepoint, glyph,
            suffix, id, fallback);
        return row.Next(ToolbarButtonWidth(label.c_str())) &&
            ToolbarIconButton(codepoint, glyph, suffix, id, fallback, tooltip);
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
        if (!row.Next(12.f)) return;
        ImGui::TextColored({1.f, .55f, .3f, 1.f}, "!");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", message);
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

    if (CanOpenAnimationForSelection() && button("Animation Editor"))
        OpenAnimationForSelection();

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
}

void EditorState::DrawGlobalToolbar(IEditorUi& ui, ToolbarSection section,
    bool singleRow, bool* overflow)
{
    if (overflow) *overflow = false;
    if (m_gameViewFocused)
    {
        m_toolbarPopupTool.clear();
        m_toolbarPopupScene = nullptr;
        return;
    }
    if (m_activeAssetDocument)
    {
        m_toolbarPopupTool.clear();
        m_toolbarPopupScene = nullptr;
        ToolbarRow row{singleRow, overflow};
        if (section == ToolbarSection::File && m_toolbarVisibility.save)
        {
            ImGui::BeginDisabled(!m_playModeSceneSnapshot.empty());
            const std::string save = ToolbarIconLabel(0xE74E, u8"\uE74E",
                "", "SaveAsset", "Save Asset");
            if (row.Next(ToolbarButtonWidth(save.c_str())) &&
                ToolbarIconButton(0xE74E, u8"\uE74E", "",
                "SaveAsset", "Save Asset", "Save Asset")) SaveScene();
            const std::string saveAll = ToolbarIconLabel(0xE74E, u8"\uE74E",
                " All", "SaveAllAssets", "Save All");
            if (row.Next(ToolbarButtonWidth(saveAll.c_str())) &&
                ToolbarIconButton(0xE74E, u8"\uE74E", " All",
                "SaveAllAssets", "Save All", "Save All")) SaveAll();
            ImGui::EndDisabled();
        }
        if (section == ToolbarSection::Edit && m_toolbarVisibility.undoRedo)
        {
            ImGui::BeginDisabled(!CanUndo() ||
                !m_playModeSceneSnapshot.empty());
            const std::string undo = ToolbarIconLabel(0xE7A7, u8"\uE7A7",
                "", "UndoAsset", "Undo");
            if (row.Next(ToolbarButtonWidth(undo.c_str())) &&
                ToolbarIconButton(0xE7A7, u8"\uE7A7", "",
                "UndoAsset", "Undo", "Undo")) Undo();
            ImGui::EndDisabled();
            ImGui::BeginDisabled(!CanRedo() ||
                !m_playModeSceneSnapshot.empty());
            const std::string redo = ToolbarIconLabel(0xE7A6, u8"\uE7A6",
                "", "RedoAsset", "Redo");
            if (row.Next(ToolbarButtonWidth(redo.c_str())) &&
                ToolbarIconButton(0xE7A6, u8"\uE7A6", "",
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
    if (!view || !view->UseGlobalToolbar) return;

    if (section == ToolbarSection::Tools)
    {
        DrawToolbarTools(ui, singleRow, overflow);
        return;
    }

    MeshEditSession& mesh = m_activeSceneAssetDocument
        ? m_activeSceneAssetDocument->meshEdit
        : m_prefabDocumentFocused ? m_prefabMeshEdit : m_mainMeshEdit;
    SkeletonEditSession& skeleton = m_activeSceneAssetDocument
        ? m_activeSceneAssetDocument->skeletonEdit
        : m_prefabDocumentFocused ? m_prefabSkeletonEdit : m_mainSkeletonEdit;
    ToolbarRow row{singleRow, overflow};
    const bool showEditMode = section == ToolbarSection::Mode &&
        m_toolbarVisibility.editMode && row.Next(105.f);
    if (showEditMode && m_activeSceneAssetDocument &&
        m_activeSceneAssetDocument->modelStage &&
        m_activeSceneAssetDocument->focusPicker)
    {
        ui.SetNextItemWidth(105.f);
        m_activeSceneAssetDocument->focusPicker(ui);
    }
    else if (showEditMode && m_activeSceneAssetDocument &&
        m_activeSceneAssetDocument->meshStage)
        ui.Label("Mesh");
    else if (showEditMode)
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
    if (section == ToolbarSection::File && m_toolbarVisibility.save)
    {
        ImGui::BeginDisabled(!m_playModeSceneSnapshot.empty());
        const char* saveLabel = m_activeSceneAssetDocument &&
            m_activeSceneAssetDocument->meshStage ? "Save Mesh" :
            (m_prefabDocumentFocused ||
                (m_activeSceneAssetDocument && m_activeSceneAssetDocument->prefab))
                ? "Save Prefab" : "Save Scene";
        const std::string save = ToolbarIconLabel(0xE74E, u8"\uE74E",
            "", "SaveCurrent", saveLabel);
        if (row.Next(ToolbarButtonWidth(save.c_str())) &&
            ToolbarIconButton(0xE74E, u8"\uE74E", "",
            "SaveCurrent", saveLabel, saveLabel))
        {
            if (m_activeSceneAssetDocument)
                SaveSceneAssetDocument(*m_activeSceneAssetDocument);
            else SaveScene();
        }
        const std::string saveAll = ToolbarIconLabel(0xE74E, u8"\uE74E",
            " All", "SaveAll", "Save All");
        if (row.Next(ToolbarButtonWidth(saveAll.c_str())) &&
            ToolbarIconButton(0xE74E, u8"\uE74E", " All",
            "SaveAll", "Save All", "Save All")) SaveAll();
        ImGui::EndDisabled();
    }
    if (section == ToolbarSection::Edit && m_toolbarVisibility.undoRedo)
    {
        ImGui::BeginDisabled(!CanUndo() ||
            !m_playModeSceneSnapshot.empty());
        const std::string undo = ToolbarIconLabel(0xE7A7, u8"\uE7A7",
            "", "UndoDocument", "Undo");
        if (row.Next(ToolbarButtonWidth(undo.c_str())) &&
            ToolbarIconButton(0xE7A7, u8"\uE7A7", "",
            "UndoDocument", "Undo", "Undo")) Undo();
        ImGui::EndDisabled();
        ImGui::BeginDisabled(!CanRedo() ||
            !m_playModeSceneSnapshot.empty());
        const std::string redo = ToolbarIconLabel(0xE7A6, u8"\uE7A6",
            "", "RedoDocument", "Redo");
        if (row.Next(ToolbarButtonWidth(redo.c_str())) &&
            ToolbarIconButton(0xE7A6, u8"\uE7A6", "",
            "RedoDocument", "Redo", "Redo")) Redo();
        ImGui::EndDisabled();
    }

    Engine::Core::Object* selectedObject = scene->GetSelectedObject();
    Engine::Core::Object* prefabRoot = selectedObject
        ? selectedObject->GetPrefabInstanceRoot() : nullptr;
    if (section == ToolbarSection::Prefab && m_toolbarVisibility.prefabActions && prefabRoot && prefabRoot->Prefab)
    {
        ImGui::BeginDisabled(!m_playModeSceneSnapshot.empty());
        const bool hasOverrides =
            Engine::Serialization::SceneSerializer::HasPrefabOverrides(
                *prefabRoot, true);
        const std::string edit = ToolbarIconLabel(0xE70F, u8"\uE70F",
            "", "EditPrefab", "Edit Prefab");
        if (row.Next(ToolbarButtonWidth(edit.c_str())) &&
            ToolbarIconButton(0xE70F, u8"\uE70F", "",
            "EditPrefab", "Edit Prefab", "Edit Prefab"))
            QueueSceneAssetDocumentOpen(prefabRoot->Prefab->GetPath());
        ui.BeginDisabled(!hasOverrides);
        if (row.Next(ToolbarButtonWidth("Apply Overrides")) &&
            ui.Button("Apply Overrides") &&
            Engine::Serialization::SceneSerializer::ApplyPrefabOverridesToAsset(
                *prefabRoot, false, scene->GetGraphicsProvider()))
            MarkSceneEdited();
        if (row.Next(ToolbarButtonWidth("Apply All")) &&
            ui.Button("Apply All") &&
            Engine::Serialization::SceneSerializer::ApplyPrefabOverridesToAsset(
                *prefabRoot, true, scene->GetGraphicsProvider()))
            MarkSceneEdited();
        const std::string revert = ToolbarIconLabel(0xE72C, u8"\uE72C",
            "", "RevertPrefab", "Revert");
        if (row.Next(ToolbarButtonWidth(revert.c_str())) &&
            ToolbarIconButton(0xE72C, u8"\uE72C", "",
            "RevertPrefab", "Revert", "Revert prefab overrides"))
        {
            SelectObject(prefabRoot);
            if (Engine::Serialization::SceneSerializer::RevertPrefabOverrides(
                *prefabRoot, scene->GetGraphicsProvider()))
                MarkSceneEdited();
        }
        ui.EndDisabled();
        if (row.Next(ToolbarButtonWidth("Unpack Prefab")) &&
            ui.Button("Unpack Prefab"))
        {
            prefabRoot->Prefab.reset();
            prefabRoot->PrefabSourceSnapshot.clear();
            MarkSceneEdited();
        }
        ImGui::EndDisabled();
    }

    if (section == ToolbarSection::Mode && m_toolbarVisibility.modeTools && skeleton.enabled)
    {
        const char* submodes[]{ "Bones", "Weight Paint" };
        int submode = skeleton.submode;
        if (row.Next(110.f))
        {
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
        }
        if (skeleton.submode == 1)
        {
            const char* brushes[]{ "Add", "Subtract", "Replace", "Smooth",
                "Normalize" };
            if (row.Next(100.f))
            {
                ui.SetNextItemWidth(100.f);
                ui.Combo("##GlobalWeightBrush", &skeleton.brushOperation,
                    brushes, 5);
                ui.Tooltip("Weight paint operation");
            }
            if (singleRow || ui.AvailableContentWidth() > 190.f)
            {
                if (row.Next(80.f))
                {
                    ui.SetNextItemWidth(80.f);
                    ui.SliderFloat("##GlobalBrushSize", &skeleton.brushRadius,
                        2.f, 250.f);
                    ui.Tooltip("Brush size in pixels");
                }
            }
        }
    }
    else if (section == ToolbarSection::Mode && m_toolbarVisibility.modeTools && mesh.enabled)
    {
        const char* elements[]{ "Vertex", "Edge", "Face" };
        if (row.Next(80.f))
        {
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
        }
        const char* selectionTools[]{ "Click", "Box", "Lasso" };
        if (row.Next(70.f))
        {
            ui.SetNextItemWidth(70.f);
            ui.Combo("##GlobalSelectionTool", &mesh.selectionTool,
                selectionTools, 3);
            ui.Tooltip("Selection tool");
        }
    }
    if (section == ToolbarSection::Transform && m_toolbarVisibility.transformTools &&
        (singleRow || ui.AvailableContentWidth() > 340.f))
    {
        const EditorTransformTool tools[]{ EditorTransformTool::Hand,
            EditorTransformTool::Translate, EditorTransformTool::Rotate,
            EditorTransformTool::Scale };
        const char* toolNames[]{ "Hand", "Move", "Rotate", "Scale" };
        for (int index = 0; index < 4; ++index)
        {
            const bool selected = view->GetTransformTool() == tools[index];
            const std::string label = std::string(toolNames[index]) +
                (selected ? " *" : "");
            if (row.Next(ToolbarButtonWidth(label.c_str())) &&
                ui.Button(label.c_str()))
                view->SetTransformTool(tools[index]);
        }
    }
    else if (section == ToolbarSection::Transform && m_toolbarVisibility.transformTools)
    {
        const char* tools[]{ "Hand", "Move", "Rotate", "Scale" };
        int tool = view->GetTransformTool() == EditorTransformTool::Hand ? 0
            : view->GetTransformTool() == EditorTransformTool::Translate ? 1
            : view->GetTransformTool() == EditorTransformTool::Rotate ? 2 : 3;
        if (row.Next(100.f))
        {
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
    }
    if (section == ToolbarSection::View && m_toolbarVisibility.sceneDisplay)
    {
        const char* gridLabel = scene->settings.showGrid ? "Grid *" : "Grid";
        if (row.Next(ToolbarButtonWidth(gridLabel)) && ui.Button(gridLabel))
            scene->settings.showGrid = !scene->settings.showGrid;
        int renderMode = static_cast<int>(scene->settings.renderMode);
        const char* renderNames[]{ "Lit", "Unlit", "Wireframe" };
        if (row.Next(95.f))
        {
            ui.SetNextItemWidth(95.f);
            if (ui.Combo("##GlobalRenderMode", &renderMode, renderNames, 3))
                scene->settings.renderMode =
                    static_cast<Engine::Model::SceneRenderMode>(renderMode);
            ui.Tooltip("Scene render mode");
        }
        const char* overlayLabel = scene->settings.sceneViewUiOverlay
            ? "UI Overlay *" : "UI Overlay";
        if (row.Next(ToolbarButtonWidth(overlayLabel)))
        {
            if (ui.Button(overlayLabel))
                scene->settings.sceneViewUiOverlay =
                    !scene->settings.sceneViewUiOverlay;
            ui.Tooltip("Show scene UI in the editor viewport");
        }
    }
}

}

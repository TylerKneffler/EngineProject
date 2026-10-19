#include "pch.h"
#include "Engine/Editor/Core/View/Views/AssetDocumentView.h"
#include "Engine/Editor/Core/Paint/BrushPreview.h"
#include "Engine/Editor/EditorState.h"
#include "Engine/Editor/UI/IEditorUi.h"
#include "Engine/Editor/Core/View/Views/SceneView.h"
#include "Editor/Core/View/EditorViewportSettings.h"
#include "Core/Scene/Scene.h"
#include "Core/Object.h"
#include "Core/Compoonents/Obj/Mesh.h"
#include "Core/Compoonents/Obj/Sprite.h"
#include "Core/Compoonents/Animation/Skeleton.h"
#include "Core/Compoonents/Camera/Camera.h"
#include "Core/Compoonents/Lighting/Light.h"
#include "Core/Compoonents/Lighting/LightProbe.h"
#include "Core/Compoonents/Materials/Material.h"
#include "Core/Compoonents/Materials/Texture.h"
#include "Core/Prefab/PrefabAsset.h"
#include "Core/Serialization/SceneSerializer.h"
#include "imgui.h"
#include <algorithm>
#include <cctype>
#include <cfloat>
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
    constexpr float iconSize = 13.f;
    ImFont* font = ImGui::GetFont();
    const bool hasIcon = font->IsGlyphInFont(codepoint);
    const ImVec2 iconExtent = hasIcon
        ? font->CalcTextSizeA(iconSize, FLT_MAX, 0.f, glyph)
        : ImGui::CalcTextSize(fallback);
    const ImVec2 suffixExtent = hasIcon && suffix[0]
        ? ImGui::CalcTextSize(suffix) : ImVec2{};
    const float contentWidth = iconExtent.x + suffixExtent.x;
    const float width = std::max(28.f, contentWidth +
        ImGui::GetStyle().FramePadding.x * 2.f);
    const std::string buttonId = std::string("##") + id;
    const bool clicked = ImGui::Button(buttonId.c_str(), {width, 20.f});
    const ImVec2 minimum = ImGui::GetItemRectMin();
    const ImVec2 maximum = ImGui::GetItemRectMax();
    const float x = minimum.x + (maximum.x - minimum.x - contentWidth) * .5f;
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const ImU32 color = ImGui::GetColorU32(ImGuiCol_Text);
    if (hasIcon)
    {
        drawList->AddText(font, iconSize,
            {x, minimum.y + (maximum.y - minimum.y - iconExtent.y) * .5f},
            color, glyph);
        if (suffix[0])
            drawList->AddText(
                {x + iconExtent.x,
                 minimum.y + (maximum.y - minimum.y - suffixExtent.y) * .5f},
                color, suffix);
    }
    else
        drawList->AddText(
            {x, minimum.y + (maximum.y - minimum.y - iconExtent.y) * .5f},
            color, fallback);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", tooltip);
    return clicked;
}

float ToolbarButtonWidth(const char* label)
{
    return std::max(28.f, ImGui::CalcTextSize(label).x +
        ImGui::GetStyle().FramePadding.x * 2.f);
}

// Draw these directly so transform tools have recognizable icons even when
// the optional system icon font is unavailable.
bool TransformIconButton(const char* id, EditorTransformTool tool,
    bool selected, const char* tooltip)
{
    if (selected)
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{.18f, .38f, .68f, 1.f});
    const bool clicked = ImGui::Button(id, {28.f, 20.f});
    if (selected) ImGui::PopStyleColor();
    const ImVec2 minimum = ImGui::GetItemRectMin();
    const ImVec2 maximum = ImGui::GetItemRectMax();
    const ImVec2 center{(minimum.x + maximum.x) * .5f,
        (minimum.y + maximum.y) * .5f};
    auto* draw = ImGui::GetWindowDrawList();
    const ImU32 color = ImGui::GetColorU32(ImGuiCol_Text);
    const auto point = [&](float x, float y)
    { return ImVec2{center.x + x, center.y + y}; };
    if (tool == EditorTransformTool::Translate)
    {
        draw->AddLine(point(-6.f, 0.f), point(6.f, 0.f), color, 1.5f);
        draw->AddLine(point(0.f, -6.f), point(0.f, 6.f), color, 1.5f);
        draw->AddTriangleFilled(point(8.f, 0.f), point(4.f, -3.f), point(4.f, 3.f), color);
        draw->AddTriangleFilled(point(-8.f, 0.f), point(-4.f, -3.f), point(-4.f, 3.f), color);
        draw->AddTriangleFilled(point(0.f, -8.f), point(-3.f, -4.f), point(3.f, -4.f), color);
        draw->AddTriangleFilled(point(0.f, 8.f), point(-3.f, 4.f), point(3.f, 4.f), color);
    }
    else if (tool == EditorTransformTool::Rotate)
    {
        draw->AddCircle(center, 6.f, color, 24, 1.5f);
        draw->AddTriangleFilled(point(7.f, -1.f), point(2.f, -5.f), point(8.f, -6.f), color);
    }
    else if (tool == EditorTransformTool::Scale)
    {
        draw->AddLine(point(-5.f, 5.f), point(5.f, -5.f), color, 1.5f);
        draw->AddRectFilled(point(-7.f, 3.f), point(-3.f, 7.f), color);
        draw->AddRectFilled(point(3.f, -7.f), point(7.f, -3.f), color);
    }
    else
    {
        draw->AddRect(point(-4.f, -1.f), point(4.f, 7.f), color, 3.f, 0, 1.5f);
        for (int finger = 0; finger < 4; ++finger)
        {
            const float x = -4.f + finger * 2.5f;
            draw->AddLine(point(x, 1.f), point(x, finger == 0 || finger == 3 ? -5.f : -7.f), color, 1.5f);
        }
        draw->AddLine(point(-4.f, 4.f), point(-7.f, 0.f), color, 1.5f);
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", tooltip);
    return clicked;
}

enum class ViewIcon { Bones, Grid, Overlays, Ui };

bool ViewIconButton(const char* id, ViewIcon icon, bool active,
    const char* tooltip)
{
    if (active)
    {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{.18f, .38f, .68f, 1.f});
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4{.24f, .46f, .75f, 1.f});
    }
    const bool clicked = ImGui::Button(id, {28.f, 20.f});
    if (active) ImGui::PopStyleColor(2);
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 center{min.x + 14.f, min.y + 10.f};
    const auto point = [&](float x, float y)
    { return ImVec2{center.x + x, center.y + y}; };
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImU32 color = ImGui::GetColorU32(ImGuiCol_Text);
    switch (icon)
    {
    case ViewIcon::Bones:
        draw->AddLine(point(-5.f, 4.f), point(5.f, -4.f), color, 2.f);
        draw->AddCircleFilled(point(-6.f, 5.f), 2.f, color);
        draw->AddCircleFilled(point(6.f, -5.f), 2.f, color);
        break;
    case ViewIcon::Grid:
        for (int offset = -1; offset <= 1; ++offset)
        {
            draw->AddLine(point(-7.f, offset * 5.f),
                point(7.f, offset * 5.f), color, 1.f);
            draw->AddLine(point(offset * 7.f, -5.f),
                point(offset * 7.f, 5.f), color, 1.f);
        }
        break;
    case ViewIcon::Overlays:
        draw->AddCircle(center, 5.f, color, 16, 1.5f);
        draw->AddLine(point(-8.f, 0.f), point(-5.f, 0.f), color, 1.5f);
        draw->AddLine(point(5.f, 0.f), point(8.f, 0.f), color, 1.5f);
        draw->AddLine(point(0.f, -8.f), point(0.f, -5.f), color, 1.5f);
        draw->AddLine(point(0.f, 5.f), point(0.f, 8.f), color, 1.5f);
        break;
    case ViewIcon::Ui:
        draw->AddRect(point(-7.f, -5.f), point(7.f, 5.f), color, 1.f, 0, 1.3f);
        draw->AddLine(point(-7.f, -2.f), point(7.f, -2.f), color);
        draw->AddRectFilled(point(-4.f, 0.f), point(-1.f, 3.f), color);
        break;
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", tooltip);
    return clicked;
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
    return 36.f;
}

SceneView* EditorState::GetGlobalToolbarSceneView() const
{
    Engine::Scene::Scene* scene = GetActiveDocumentScene();
    if (!scene) return nullptr;
    EditorViewportSettings::Apply(*scene);
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
    return view && view->IsOpen() && view->UseGlobalToolbar ? view : nullptr;
}

bool EditorState::HasToolbarSection(ToolbarSection section) const
{
    if (m_gameViewFocused) return false;
    if (m_activeAnimationDocument)
        return section == ToolbarSection::Mode;
    if (m_activeAssetDocument)
        return section == ToolbarSection::File ? m_toolbarVisibility.save :
            section == ToolbarSection::Edit ? m_toolbarVisibility.undoRedo :
            section == ToolbarSection::Paint && m_toolbarVisibility.paintTools &&
                m_activeAssetDocument->CanPaintImage();
    Engine::Scene::Scene* scene = GetActiveDocumentScene();
    if (!scene || !GetGlobalToolbarSceneView()) return false;
    const MeshEditSession& mesh = m_activeSceneAssetDocument
        ? m_activeSceneAssetDocument->meshEdit
        : m_prefabDocumentFocused ? m_prefabMeshEdit : m_mainMeshEdit;
    const SkeletonEditSession& skeleton = m_activeSceneAssetDocument
        ? m_activeSceneAssetDocument->skeletonEdit
        : m_prefabDocumentFocused ? m_prefabSkeletonEdit : m_mainSkeletonEdit;
    switch (section)
    {
    case ToolbarSection::File: return m_toolbarVisibility.save;
    case ToolbarSection::Edit: return m_toolbarVisibility.undoRedo;
    case ToolbarSection::Mode:
        return m_toolbarVisibility.editMode ||
            (m_toolbarVisibility.modeTools && (mesh.enabled || skeleton.enabled));
    case ToolbarSection::Tools:
        return m_toolbarVisibility.toolDetails &&
            !(skeleton.enabled && skeleton.submode == 1);
    case ToolbarSection::Paint:
        return m_toolbarVisibility.paintTools &&
            ((skeleton.enabled && skeleton.submode == 1) || mesh.enabled);
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

float EditorState::GetToolbarSectionMinimumWidth(ToolbarSection section) const
{
    if (m_activeAnimationDocument)
        return section == ToolbarSection::Mode ? 280.f : 0.f;
    if (m_activeAssetDocument && section == ToolbarSection::Paint)
        return 190.f;
    float controlWidth = 0.f;
    switch (section)
    {
    case ToolbarSection::File:
    {
        const char* fallback = m_activeAssetDocument ? "Save Asset" :
            m_activeSceneAssetDocument && m_activeSceneAssetDocument->meshStage
                ? "Save Mesh" : m_prefabDocumentFocused ||
                (m_activeSceneAssetDocument && m_activeSceneAssetDocument->prefab)
                ? "Save Prefab" : "Save Scene";
        const std::string label = ToolbarIconLabel(0xE74E, u8"\uE74E", "",
            "MinimumSave", fallback);
        controlWidth = ToolbarButtonWidth(label.c_str());
        break;
    }
    case ToolbarSection::Edit:
    {
        const std::string label = ToolbarIconLabel(0xE7A7, u8"\uE7A7", "",
            "MinimumUndo", "Undo");
        controlWidth = ToolbarButtonWidth(label.c_str());
        break;
    }
    case ToolbarSection::Mode: controlWidth = 105.f; break;
    case ToolbarSection::Paint:
    {
        const MeshEditSession& mesh = m_activeSceneAssetDocument
            ? m_activeSceneAssetDocument->meshEdit
            : m_prefabDocumentFocused ? m_prefabMeshEdit : m_mainMeshEdit;
        controlWidth = mesh.enabled ?
            ToolbarButtonWidth(mesh.vertexPaintMode ? "Leave Paint" :
                "Paint Mesh") + 85.f + ToolbarButtonWidth("Paint...") +
                2.f * ImGui::GetStyle().ItemSpacing.x :
            82.f + ImGui::GetStyle().ItemSpacing.x +
                ToolbarButtonWidth("Paint...");
        break;
    }
    case ToolbarSection::Tools:
    {
        const MeshEditSession& mesh = m_activeSceneAssetDocument
            ? m_activeSceneAssetDocument->meshEdit
            : m_prefabDocumentFocused ? m_prefabMeshEdit : m_mainMeshEdit;
        const SkeletonEditSession& skeleton = m_activeSceneAssetDocument
            ? m_activeSceneAssetDocument->skeletonEdit
            : m_prefabDocumentFocused ? m_prefabSkeletonEdit : m_mainSkeletonEdit;
        const char* first = skeleton.enabled
            ? (skeleton.submode == 0 ? "Bone..." : "Brush...")
            : mesh.enabled ? "Mesh Tools..."
            : m_activeSceneAssetDocument && m_activeSceneAssetDocument->meshStage
                ? "Vertex Attributes..." : "Transform...";
        const std::string suffix = std::string(" ") + first;
        const std::string label = ToolbarIconLabel(0xE713, u8"\uE713",
            suffix.c_str(), "MinimumTools", first);
        controlWidth = ToolbarButtonWidth(label.c_str());
        break;
    }
    case ToolbarSection::Transform:
        controlWidth = 4.f * 28.f + 3.f * ImGui::GetStyle().ItemSpacing.x;
        break;
    case ToolbarSection::View:
        controlWidth = 28.f;
        break;
    case ToolbarSection::Prefab:
    {
        const std::string label = ToolbarIconLabel(0xE70F, u8"\uE70F", "",
            "MinimumPrefab", "Edit Prefab");
        controlWidth = ToolbarButtonWidth(label.c_str());
        break;
    }
    }
    // Seven pixels precede the child, and its handle and padding use 25 more.
    return controlWidth + 34.f;
}

float EditorState::GetToolbarSectionPreferredWidth(ToolbarSection section) const
{
    float width = GetToolbarSectionMinimumWidth(section);
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    if (section == ToolbarSection::File)
    {
        const std::string saveAll = ToolbarIconLabel(0xE74E, u8"\uE74E",
            " All", "PreferredSaveAll", "Save All");
        width += spacing + ToolbarButtonWidth(saveAll.c_str());
    }
    else if (section == ToolbarSection::Edit)
    {
        const std::string redo = ToolbarIconLabel(0xE7A6, u8"\uE7A6",
            "", "PreferredRedo", "Redo");
        width += spacing + ToolbarButtonWidth(redo.c_str());
    }
    else if (section == ToolbarSection::View)
        width += 3.f * (28.f + spacing);
    else if (section == ToolbarSection::Mode && !m_activeAnimationDocument &&
        !m_activeAssetDocument && m_toolbarVisibility.modeTools)
    {
        const MeshEditSession& mesh = m_activeSceneAssetDocument
            ? m_activeSceneAssetDocument->meshEdit
            : m_prefabDocumentFocused ? m_prefabMeshEdit : m_mainMeshEdit;
        const SkeletonEditSession& skeleton = m_activeSceneAssetDocument
            ? m_activeSceneAssetDocument->skeletonEdit
            : m_prefabDocumentFocused ? m_prefabSkeletonEdit : m_mainSkeletonEdit;
        if (skeleton.enabled) width += 110.f + spacing;
        else if (mesh.enabled) width += 150.f + 2.f * spacing;
    }
    return width;
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
        const bool animation = std::string_view(label) == "Animation Editor";
        const bool bindPose = std::string_view(label) == "Bind Pose";
        const ImWchar codepoint = animation ? 0xE768 : bindPose ? 0xE74E : 0xE713;
        const char* glyph = animation ? u8"\uE768" : bindPose ? u8"\uE74E" : u8"\uE713";
        const std::string suffix = std::string(" ") + label;
        const std::string id = std::string("Tool-") + label;
        const std::string iconLabel = ToolbarIconLabel(codepoint, glyph,
            suffix.c_str(), id.c_str(), label);
        return row.Next(ToolbarButtonWidth(iconLabel.c_str())) &&
            ToolbarIconButton(codepoint, glyph, suffix.c_str(), id.c_str(), label, label);
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
        ImGui::AlignTextToFramePadding();
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
        open("UV unwrap...", "UV unwrap");
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
    if (m_activeAnimationDocument)
    {
        if (section == ToolbarSection::Mode)
            DrawAnimationRigToolbar(ui, *m_activeAnimationDocument);
        return;
    }
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
        if (section == ToolbarSection::Paint && m_toolbarVisibility.paintTools)
        {
            m_activeAssetDocument->DrawPaintToolbar(ui,
                m_projectSettings.assetsDirectory, singleRow, overflow);
            return;
        }
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
    SceneView* view = GetGlobalToolbarSceneView();
    if (!view) return;

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
    if (section == ToolbarSection::Paint && mesh.enabled &&
        m_toolbarVisibility.paintTools &&
        !(skeleton.enabled && skeleton.submode == 1))
    {
        if (row.Next(ToolbarButtonWidth(mesh.vertexPaintMode ?
                "Leave Paint" : "Paint Mesh")) &&
            ImGui::Button(mesh.vertexPaintMode ? "Leave Paint" : "Paint Mesh",
                {0.f, 20.f}))
        {
            FinishVertexPaintStroke(mesh);
            FinishTexturePaintStroke(mesh);
            mesh.vertexPaintMode = !mesh.vertexPaintMode;
            mesh.vertexPaintTool = mesh.vertexPaintMode ? 1 : 0;
            view->AllowObjectTransform = !mesh.vertexPaintMode;
        }
        if (!mesh.vertexPaintMode) return;
        const char* tools[]{ "Cursor", "Brush", "Eraser", "Bucket" };
        if (row.Next(85.f))
        {
            ui.SetNextItemWidth(85.f);
            int selectedTool = mesh.vertexPaintTool;
            if (ui.Combo("##VertexPaintTool", &selectedTool, tools, 4))
            {
                FinishVertexPaintStroke(mesh);
                FinishTexturePaintStroke(mesh);
                mesh.vertexPaintTool = selectedTool;
                view->AllowObjectTransform = selectedTool == 0;
            }
        }
        if (row.Next(ToolbarButtonWidth("Paint...")) &&
            ImGui::Button("Paint...", {0.f, 20.f}))
            ImGui::OpenPopup("##VertexPaintOptions");
        if (mesh.vertexPaintTool == 1 || mesh.vertexPaintTool == 2)
            if (row.Next(70.f))
            {
                ui.SetNextItemWidth(70.f);
                ui.SliderFloat("##VertexPaintSize", &mesh.vertexBrush.size,
                    2.f, 250.f);
            }
        if (ImGui::BeginPopup("##VertexPaintOptions"))
        {
            ImGui::TextUnformatted("Mesh Paint Options");
            ImGui::Separator();
            const char* targets[]{ "Vertex colors", "Material texture" };
            if (ui.Combo("Paint target", &mesh.paintTarget, targets, 2))
            {
                FinishVertexPaintStroke(mesh);
                FinishTexturePaintStroke(mesh);
            }
            if (mesh.paintTarget == 1)
            {
                ImGui::Checkbox("Mirror local X", &mesh.paintMirrorX);
                ImGui::Checkbox("Alpha Lock", &mesh.textureAlphaLock);
                const char* slots[]{ "Base color", "Metallic / roughness",
                    "Normal", "Occlusion", "Emissive", "Height" };
                if (ui.Combo("Material slot", &mesh.textureSlot, slots, 6))
                    FinishTexturePaintStroke(mesh);
                auto* material = mesh.activeMesh && mesh.activeMesh->Owner
                    ? mesh.activeMesh->Owner->GetComponent<Engine::Components::Material>()
                    : nullptr;
                int uvSet = 0;
                std::shared_ptr<Engine::Components::Texture> texture;
                if (material)
                    switch (mesh.textureSlot)
                    {
                    case 0: texture = material->baseColorTexture;
                        uvSet = material->baseColorUvSet; break;
                    case 1: texture = material->metallicRoughnessTexture;
                        uvSet = material->metallicRoughnessUvSet; break;
                    case 2: texture = material->normalTexture;
                        uvSet = material->normalUvSet; break;
                    case 3: texture = material->occlusionTexture;
                        uvSet = material->occlusionUvSet; break;
                    case 4: texture = material->emissiveTexture;
                        uvSet = material->emissiveUvSet; break;
                    case 5: texture = material->heightTexture;
                        uvSet = material->heightUvSet; break;
                    }
                if (texture)
                {
                    ui.ValueLabel("Texture", texture->GetFilePath().c_str());
                    const std::string set = std::to_string(uvSet);
                    ui.ValueLabel("UV set", set.c_str());
                }
                else ui.ColoredLabel("Assign a PNG to this material slot first.",
                    {1.f, .6f, .3f, 1.f});
            }
            else
            {
                ImGui::Checkbox("Mirror local X", &mesh.paintMirrorX);
                if (mesh.selectionMode == 0)
                    ImGui::Checkbox("Selected vertices only",
                        &mesh.vertexSelectionMask);
                else if (mesh.vertexSelectionMask)
                    ui.ColoredLabel("Select vertices to use the selection mask.",
                        {1.f, .6f, .3f, 1.f});
            }
            Paint::DrawBrushPreview(mesh.vertexBrush);
            namespace fs = std::filesystem;
            const fs::path root = m_projectSettings.assetsDirectory.empty() ?
                fs::path("Assets") : fs::path(m_projectSettings.assetsDirectory);
            std::vector<fs::path> paths;
            std::error_code error;
            if (fs::is_directory(root, error))
                for (fs::recursive_directory_iterator it(root,
                        fs::directory_options::skip_permission_denied,
                        error), end; !error && it != end; it.increment(error))
                    if (it->is_regular_file() &&
                        it->path().extension() == ".brush")
                        paths.push_back(it->path());
            std::sort(paths.begin(), paths.end());
            std::vector<std::string> labels{ "Custom" };
            int choice = 0;
            for (const auto& path : paths)
            {
                labels.push_back(path.lexically_relative(root).string());
                if (path.string() == mesh.vertexBrushPresetPath)
                    choice = static_cast<int>(labels.size()) - 1;
            }
            std::vector<const char*> names;
            for (const auto& label : labels) names.push_back(label.c_str());
            if (ui.Combo("Brush preset", &choice, names.data(),
                static_cast<int>(names.size())))
            {
                if (choice == 0) mesh.vertexBrushPresetPath.clear();
                else
                {
                    Paint::BrushPreset loaded;
                    if (Paint::LoadBrushPreset(paths[choice - 1], loaded,
                        mesh.toolError))
                    {
                        mesh.vertexBrush = std::move(loaded);
                        mesh.vertexBrushPresetPath = paths[choice - 1].string();
                    }
                }
            }
            char presetName[128]{};
            const size_t nameLength = std::min(mesh.vertexBrush.name.size(),
                sizeof(presetName) - 1);
            std::copy_n(mesh.vertexBrush.name.data(), nameLength, presetName);
            if (ui.InputText("Preset name", presetName, sizeof(presetName)))
                mesh.vertexBrush.name = presetName;
            const auto savePreset = [&](bool duplicate)
            {
                const fs::path directory = root / "Brushes";
                std::error_code createError;
                fs::create_directories(directory, createError);
                if (createError)
                { mesh.toolError = createError.message(); return; }
                fs::path path = mesh.vertexBrushPresetPath;
                if (duplicate || path.empty())
                {
                    std::string base = mesh.vertexBrush.name.empty() ?
                        "Brush" : mesh.vertexBrush.name;
                    for (char& ch : base)
                        if (ch == '/' || ch == '\\' || ch == ':' || ch == '*' ||
                            ch == '?' || ch == '"' || ch == '<' || ch == '>' ||
                            ch == '|') ch = '_';
                    path = directory / (base + ".brush");
                    for (int suffix = 2; fs::exists(path); ++suffix)
                        path = directory /
                            (base + " " + std::to_string(suffix) + ".brush");
                }
                if (Paint::SaveBrushPreset(path, mesh.vertexBrush,
                    mesh.toolError)) mesh.vertexBrushPresetPath = path.string();
            };
            if (ui.Button("Create brush asset")) savePreset(true);
            if (!mesh.vertexBrushPresetPath.empty() &&
                ui.Button("Save brush asset")) savePreset(false);
            if (!mesh.vertexBrushPresetPath.empty() &&
                ui.Button("Duplicate brush asset")) savePreset(true);
            ui.ColorEdit4("Paint color", mesh.vertexPaintColor);
            if (mesh.vertexPaintTool == 3)
                ui.SliderFloat("Color tolerance",
                    &mesh.vertexBucketTolerance, 0.f, 1.f);
            ui.SliderFloat("Size", &mesh.vertexBrush.size, 2.f, 250.f);
            ui.SliderFloat("Hardness", &mesh.vertexBrush.hardness, 0.f, 1.f);
            ui.SliderFloat("Strength", &mesh.vertexBrush.strength, 0.f, 1.f);
            ui.SliderFloat("Opacity", &mesh.vertexBrush.opacity, 0.f, 1.f);
            ui.SliderFloat("Flow", &mesh.vertexBrush.flow, 0.f, 1.f);
            ui.SliderFloat("Spacing", &mesh.vertexBrush.spacing, 0.f, 1.f);
            ui.SliderFloat("Pressure size", &mesh.vertexBrush.pressureSize,
                0.f, 1.f);
            ui.SliderFloat("Pressure strength",
                &mesh.vertexBrush.pressureStrength, 0.f, 1.f);
            if (mesh.vertexBrush.shape != 0 ||
                mesh.vertexBrush.rotation != 0.f ||
                mesh.vertexBrush.scatter != 0.f ||
                mesh.vertexBrush.falloff < 0 ||
                mesh.vertexBrush.falloff > 2)
                ui.ColoredLabel("This preset has unsupported mesh paint settings; brush strokes are blocked.",
                    {1.f, .6f, .3f, 1.f});
            if (!mesh.toolError.empty())
                ui.ColoredLabel(mesh.toolError.c_str(), {1.f, .4f, .3f, 1.f});
            ImGui::EndPopup();
        }
        return;
    }
    if (section == ToolbarSection::Paint && skeleton.enabled &&
        skeleton.submode == 1 && m_toolbarVisibility.paintTools)
    {
        const char* tools[]{ "Cursor", "Brush", "Eraser", "Bucket" };
        if (row.Next(82.f))
        {
            ui.SetNextItemWidth(82.f);
            int activeTool = skeleton.paintTool;
            if (ui.Combo("##GlobalPaintTool", &activeTool, tools, 4))
            {
                FinishSkeletonPaintStroke(skeleton);
                skeleton.paintTool = activeTool;
                view->AllowObjectTransform = activeTool == 0;
            }
            ui.Tooltip("Switch between object selection and weight painting");
        }
        if (row.Next(ToolbarButtonWidth("Paint...")) &&
            ImGui::Button("Paint...", {0.f, 20.f}))
        {
            m_toolbarPopupTool = "Brush Settings";
            m_toolbarPopupScene = scene;
            m_toolbarPopupMode = 2;
            ImGui::OpenPopup("##ToolbarToolPopup");
        }
        if (skeleton.paintTool == 1)
        {
            const char* brushes[]{ "Add", "Subtract", "Replace", "Smooth",
                "Normalize" };
            if (row.Next(94.f))
            {
                ui.SetNextItemWidth(94.f);
                ui.Combo("##GlobalWeightBrush", &skeleton.brushOperation,
                    brushes, 5);
                ui.Tooltip("Weight paint operation");
            }
            if (row.Next(65.f))
            {
                ui.SetNextItemWidth(65.f);
                ui.SliderFloat("##GlobalBrushSize", &skeleton.brushRadius,
                    2.f, 250.f);
                ui.Tooltip("Brush size in pixels");
            }
        }
        else if (skeleton.paintTool == 2 && row.Next(65.f))
        {
            ui.SetNextItemWidth(65.f);
            ui.SliderFloat("##GlobalEraserSize", &skeleton.brushRadius,
                2.f, 250.f);
            ui.Tooltip("Eraser size in pixels");
        }
        else if (skeleton.paintTool == 3 && row.Next(74.f))
        {
            ui.SetNextItemWidth(74.f);
            ui.SliderFloat("##GlobalBucketTolerance",
                &skeleton.bucketTolerance, 0.f, 1.f);
            ui.Tooltip("Bucket tolerance for connected surface weights");
        }
        if (m_toolbarVisibility.save && mesh.dirty &&
            row.Next(ToolbarButtonWidth("Save *")) &&
            ImGui::Button("Save *", {0.f, 20.f}))
        {
            FinishSkeletonPaintStroke(skeleton);
            SaveMeshEditSession(mesh);
        }
        if (ImGui::IsPopupOpen("##ToolbarToolPopup"))
        {
            ImGui::SetNextWindowSizeConstraints({290.f, 0.f},
                {460.f, 650.f});
            constexpr ImGuiWindowFlags popupFlags = ImGuiWindowFlags_NoDocking |
                ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                ImGuiWindowFlags_AlwaysAutoResize;
            if (ImGui::BeginPopup("##ToolbarToolPopup", popupFlags))
            {
                ImGui::TextUnformatted("Paint Options");
                ImGui::Separator();
                DrawSkeletonEditTools(ui);
                ImGui::EndPopup();
            }
        }
        return;
    }
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
    {
        ImGui::AlignTextToFramePadding();
        ui.Label("Mesh");
    }
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
                view->AllowObjectTransform = (submode == 0 &&
                    skeleton.boneTool == 0) ||
                    (submode == 1 && skeleton.paintTool == 0);
                SyncSkeletonEditSelection(scene, skeleton);
            }
            ui.Tooltip("Skeleton tool");
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
    if (section == ToolbarSection::Transform && m_toolbarVisibility.transformTools)
    {
        const EditorTransformTool tools[]{ EditorTransformTool::Hand,
            EditorTransformTool::Translate, EditorTransformTool::Rotate,
            EditorTransformTool::Scale };
        const char* toolNames[]{ "Hand", "Move", "Rotate", "Scale" };
        const char* toolIds[]{ "##HeaderHand", "##HeaderMove", "##HeaderRotate", "##HeaderScale" };
        for (int index = 0; index < 4; ++index)
        {
            const bool selected = view->GetTransformTool() == tools[index];
            if (row.Next(28.f) &&
                TransformIconButton(toolIds[index], tools[index], selected, toolNames[index]))
                view->SetTransformTool(tools[index]);
        }
    }
    if (section == ToolbarSection::View && m_toolbarVisibility.sceneDisplay)
    {
        if (row.Next(28.f) && ViewIconButton("##ViewBones", ViewIcon::Bones,
            scene->AreEditorBonesVisible(),
            "Show or hide all bone overlays in the Scene View"))
        {
            EditorViewportSettings::SetBonesVisible(!scene->AreEditorBonesVisible());
            EditorViewportSettings::Apply(*scene);
        }
        if (row.Next(28.f) && ViewIconButton("##ViewGrid", ViewIcon::Grid,
            scene->settings.showGrid, "Show or hide the scene grid"))
            scene->settings.showGrid = !scene->settings.showGrid;
        Engine::Core::Object* selected = scene->GetSelectedObject();
        const bool hasOverlays = (selected && selected->editorOverlays != 0) ||
            EditorViewportSettings::AreCollidersVisible() ||
            EditorViewportSettings::AreMeshNormalsVisible() ||
            EditorViewportSettings::AreBoundsVisible() ||
            EditorViewportSettings::AreLightVolumesVisible() ||
            EditorViewportSettings::AreCameraFrustumsVisible();
        if (row.Next(28.f) && ViewIconButton("##ViewOverlays",
            ViewIcon::Overlays, hasOverlays,
            "Scene View visuals and selected object controls"))
            ImGui::OpenPopup("##ViewOverlayOptions");
        if (ImGui::BeginPopup("##ViewOverlayOptions"))
        {
            ImGui::TextDisabled("Scene View visuals");
            const auto visualOption = [](const char* label, bool visible,
                void (*setVisible)(bool))
            {
                if (ImGui::MenuItem(label, nullptr, visible)) setVisible(!visible);
            };
            visualOption("Colliders", EditorViewportSettings::AreCollidersVisible(),
                EditorViewportSettings::SetCollidersVisible);
            visualOption("Mesh normals", EditorViewportSettings::AreMeshNormalsVisible(),
                EditorViewportSettings::SetMeshNormalsVisible);
            visualOption("All object bounds", EditorViewportSettings::AreBoundsVisible(),
                EditorViewportSettings::SetBoundsVisible);
            visualOption("All light volumes", EditorViewportSettings::AreLightVolumesVisible(),
                EditorViewportSettings::SetLightVolumesVisible);
            visualOption("All camera frustums", EditorViewportSettings::AreCameraFrustumsVisible(),
                EditorViewportSettings::SetCameraFrustumsVisible);
            ImGui::Separator();
            if (!selected)
                ImGui::TextDisabled("Select an object to edit its visual controls.");
            else
            {
                ImGui::TextDisabled("Visual controls: %s", selected->name.c_str());
                ImGui::Separator();
                const auto overlayItem = [&](const char* label, uint8_t flag)
                {
                    if (ImGui::MenuItem(label, nullptr,
                        (selected->editorOverlays & flag) != 0))
                        selected->editorOverlays ^= flag;
                };
                overlayItem("Position arrows", Engine::Core::Object::PositionOverlay);
                overlayItem("Rotation ellipses", Engine::Core::Object::RotationOverlay);
                overlayItem("Scale handles", Engine::Core::Object::ScaleOverlay);
                if (selected->GetComponent<Engine::Components::Skeleton>())
                {
                    ImGui::Separator();
                    overlayItem("Bones", Engine::Core::Object::BoneOverlay);
                    overlayItem("Skeleton joints", Engine::Core::Object::SkeletonOverlay);
                    if (!scene->AreEditorBonesVisible())
                        ImGui::TextDisabled("Enable the toolbar bone icon to see these.");
                }
                if (selected->GetComponent<Engine::Components::Mesh>() ||
                    selected->GetComponent<Engine::Components::Sprite>() ||
                    selected->GetComponent<Engine::Components::LightProbeGroup>())
                    overlayItem("Object bounds", Engine::Core::Object::BoundsOverlay);
                if (selected->GetComponent<Engine::Components::Light>())
                    overlayItem("Light range or cone", Engine::Core::Object::LightVolumeOverlay);
                if (selected->GetComponent<Engine::Components::Camera>())
                    overlayItem("Camera frustum", Engine::Core::Object::CameraFrustumOverlay);
            }
            ImGui::EndPopup();
        }
        int renderMode = static_cast<int>(scene->settings.renderMode);
        const char* renderNames[]{ "Lit", "Unlit", "Wireframe" };
        if (row.Next(92.f))
        {
            ui.SetNextItemWidth(92.f);
            if (ui.Combo("##GlobalRenderMode", &renderMode, renderNames, 3))
                scene->settings.renderMode =
                    static_cast<Engine::Model::SceneRenderMode>(renderMode);
            ui.Tooltip("Scene render mode");
        }
        if (row.Next(28.f) && ViewIconButton("##ViewUi", ViewIcon::Ui,
            scene->settings.sceneViewUiOverlay,
            "Show or hide scene UI in the editor viewport"))
            scene->settings.sceneViewUiOverlay =
                !scene->settings.sceneViewUiOverlay;
    }
}

}

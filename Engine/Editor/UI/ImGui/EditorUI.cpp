#include "pch.h"
#include "Engine/Editor/UI/ImGui/EditorUI.h"
#include "Engine/Editor/EditorState.h"
#include "imgui.h"
#include <cmath>

namespace Engine::Editor
{
EditorUI::EditorUI(EditorState* state)
    : m_state(state)
{
    OutputDebugStringA("[EditorUI] Constructor called\n");
}

void EditorUI::Render(PlayState playState)
{
    if (!m_state) return;
    m_mainMenu.Draw(*m_state, playState, m_gameBuildManager);
    m_panelHost.ReserveToolbar(*m_state);
    m_dockspace.Draw();
    m_panelHost.Draw(*m_state);
    m_panelHost.DrawToolbar(*m_state, playState, m_gameBuildManager);
    m_sceneLoadWarningPopup.Draw(*m_state);
    if (const std::string hierarchy = m_state->ConsumeImportHierarchyFocusRequest();
        !hierarchy.empty())
        ImGui::SetWindowFocus(hierarchy.c_str());
    if (m_state->HasPendingImportAsset())
    {
        ImGui::SetNextWindowSize(ImVec2(500.f, 0.f), ImGuiCond_FirstUseEver);
        bool open = true;
        if (ImGui::Begin("Import Asset", &open, ImGuiWindowFlags_NoCollapse))
        {
            ImGui::TextWrapped("Source: %s", m_state->GetPendingImportAssetPath().c_str());
            ImGui::Separator();
            bool intoScene = m_state->IsPendingImportIntoScene();
            bool asPrefab = m_state->IsPendingImportAsPrefab();
            if (ImGui::RadioButton("Import to Assets directory", !intoScene && !asPrefab))
            {
                m_state->SetPendingImportIntoScene(false);
                m_state->SetPendingImportAsPrefab(false);
            }
            ImGui::BeginDisabled(!m_state->CanImportIntoScene());
            if (ImGui::RadioButton("Import into active scene", intoScene))
                m_state->SetPendingImportIntoScene(true);
            ImGui::EndDisabled();
            if (ImGui::RadioButton("Import as prefab", asPrefab))
                m_state->SetPendingImportAsPrefab(true);
            if (m_state->PendingImportSupportsReference() && !intoScene && !asPrefab)
                ImGui::TextWrapped("Saves the VMD source tracks as an animation pack. Import into the active scene to bind body and facial motion to a character.");
            if (m_state->PendingImportSupportsReference() && intoScene)
                ImGui::TextWrapped("Creates camera and light objects when those tracks exist. Choose an imported FBX or PMX character below to create playable body and facial clips.");
            if (m_state->IsPendingImportAsPrefab())
                ImGui::TextWrapped(m_state->PendingImportSupportsReference()
                    ? "Requires camera or light keys. Saves a reusable scene track prefab; character tracks stay in the animation pack."
                    : "Saves a reusable scene object prefab in the chosen Assets directory.");
            char directory[1024]{};
            strncpy_s(directory, sizeof(directory),
                m_state->GetPendingImportDirectory().c_str(), _TRUNCATE);
            if (ImGui::InputText("Assets directory", directory, sizeof(directory)))
                m_state->SetPendingImportDirectory(directory);
            if (m_state->IsPendingPmxImport())
            {
                ImGui::Separator();
                ImGui::TextUnformatted("PMX parameters");
                float scale = m_state->GetPendingPmxUnitScaleToMeters();
                if (ImGui::InputFloat("Meters per PMX unit", &scale,
                        0.01f, 0.1f, "%.6g"))
                    m_state->SetPendingPmxUnitScaleToMeters(scale);
                ImGui::TextWrapped("PMX does not specify a physical unit. This scales the generated prefab root; the source geometry and skeleton retain their original coordinates.");
                bool morphs = m_state->GetPendingPmxImportMorphs();
                if (ImGui::Checkbox("Morphs", &morphs))
                    m_state->SetPendingPmxImportMorphs(morphs);
                ImGui::TextDisabled("Vertex/UV targets and bone/material controls.");
                bool constraints = m_state->GetPendingPmxImportRigConstraints();
                if (ImGui::Checkbox("Append and IK constraints", &constraints))
                    m_state->SetPendingPmxImportRigConstraints(constraints);
                bool colliders = m_state->GetPendingPmxImportFollowBoneColliders();
                if (ImGui::Checkbox("Follow-bone colliders", &colliders))
                    m_state->SetPendingPmxImportFollowBoneColliders(colliders);
                bool dynamicPhysics = m_state->GetPendingPmxImportDynamicPhysics();
                if (ImGui::Checkbox("Dynamic rigid bodies (experimental)",
                        &dynamicPhysics))
                    m_state->SetPendingPmxImportDynamicPhysics(dynamicPhysics);
                ImGui::TextDisabled("Dynamic bodies and supported PMX joints drive bones.");
                bool fitted = m_state->GetPendingPmxWeightFittedColliders();
                if (ImGui::Checkbox("Fit rigid-body colliders to skin weights",
                        &fitted))
                    m_state->SetPendingPmxWeightFittedColliders(fitted);
                ImGui::TextWrapped("Builds a convex hull for each imported PMX body from the mesh vertices weighted to its bone. Bodies without enough vertices keep their PMX collider shape.");
            }
            if (m_state->IsPendingImportIntoScene())
            {
                ImGui::Separator();
                const bool vmd = m_state->PendingImportSupportsReference();
                ImGui::TextUnformatted(vmd
                    ? "Character reference (optional)" : "Parent object (optional)");
                const std::string& reference = m_state->GetPendingImportReferenceLabel();
                ImGui::TextWrapped("%s", reference.empty()
                    ? (vmd ? "None - import scene camera and light only"
                        : "None - add at scene root") : reference.c_str());
                if (ImGui::Button(m_state->IsPickingImportReference()
                        ? "Select an object in Hierarchy..." : "Pick in Hierarchy"))
                    m_state->BeginImportReferencePick();
                if (!reference.empty())
                {
                    ImGui::SameLine();
                    if (ImGui::Button("Clear reference"))
                        m_state->ClearImportReference();
                }
                if (m_state->IsPickingImportReference())
                    ImGui::TextUnformatted("Waiting for a Hierarchy selection.");
            }
            if (!m_state->GetPendingImportError().empty())
                ImGui::TextWrapped("%s", m_state->GetPendingImportError().c_str());
            ImGui::Separator();
            ImGui::BeginDisabled(m_state->IsPickingImportReference());
            if (ImGui::Button("Confirm Import"))
                m_state->ConfirmPendingImportAsset();
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Cancel"))
                m_state->CancelPendingImportAsset();
        }
        ImGui::End();
        if (!open) m_state->CancelPendingImportAsset();
    }
    if (m_state->IsLoadingOverlayVisible())
    {
        ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);
        ImGui::SetNextWindowBgAlpha(0.86f);
        constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoNav;
        ImGui::Begin("##editorLoading", nullptr, flags);
        const char* message = m_state->GetLoadingOverlayMessage().empty()
            ? "Loading..." : m_state->GetLoadingOverlayMessage().c_str();
        const ImVec2 textSize = ImGui::CalcTextSize(message);
        const ImVec2 windowSize = ImGui::GetWindowSize();
        const ImVec2 windowPos = ImGui::GetWindowPos();
        const ImVec2 center(windowPos.x + windowSize.x * 0.5f,
            windowPos.y + windowSize.y * 0.5f - 18.0f);
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        drawList->AddCircle(center, 14.0f, IM_COL32(95, 95, 95, 255), 32, 3.0f);
        constexpr float fullTurn = 6.28318530718f;
        const float start = static_cast<float>(ImGui::GetTime()) * 4.0f;
        ImVec2 arc[17];
        for (int i = 0; i < 17; ++i)
        {
            const float angle = start + fullTurn * static_cast<float>(i) / 24.0f;
            arc[i] = ImVec2(center.x + std::cos(angle) * 14.0f,
                center.y + std::sin(angle) * 14.0f);
        }
        drawList->AddPolyline(arc, 17, IM_COL32(235, 235, 235, 255), 0, 3.0f);
        ImGui::SetCursorPos({
            (windowSize.x - textSize.x) * 0.5f,
            windowSize.y * 0.5f + 10.0f });
        ImGui::TextUnformatted(message);
        ImGui::End();
    }
}
}

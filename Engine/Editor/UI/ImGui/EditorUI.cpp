#include "pch.h"
#include "Engine/Editor/UI/ImGui/EditorUI.h"
#include "Engine/Editor/EditorState.h"
#include "imgui.h"

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
                ImGui::TextWrapped("Creates camera and light objects when those tracks exist. Choose an imported FBX character below to create playable body and facial clips.");
            if (m_state->IsPendingImportAsPrefab())
                ImGui::TextWrapped(m_state->PendingImportSupportsReference()
                    ? "Requires camera or light keys. Saves a reusable scene track prefab; character tracks stay in the animation pack."
                    : "Saves a reusable scene object prefab in the chosen Assets directory.");
            char directory[1024]{};
            strncpy_s(directory, sizeof(directory),
                m_state->GetPendingImportDirectory().c_str(), _TRUNCATE);
            if (ImGui::InputText("Assets directory", directory, sizeof(directory)))
                m_state->SetPendingImportDirectory(directory);
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
        ImGui::Begin("##scriptReloadLoading", nullptr, flags);
        const char* message = m_state->GetLoadingOverlayMessage().empty()
            ? "Compiling scripts..." : m_state->GetLoadingOverlayMessage().c_str();
        const ImVec2 textSize = ImGui::CalcTextSize(message);
        ImGui::SetCursorPos({
            (ImGui::GetWindowSize().x - textSize.x) * 0.5f,
            (ImGui::GetWindowSize().y - textSize.y) * 0.5f });
        ImGui::TextUnformatted(message);
        ImGui::End();
    }
}
}

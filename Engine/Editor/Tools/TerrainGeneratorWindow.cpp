#include "pch.h"
#include "Engine/Editor/Tools/TerrainGeneratorWindow.h"
#include "Engine/Editor/State/EditorState.h"
#include "Engine/Editor/Core/View/Views/ConsoleView.h"
#include "Core/Components/Mesh/Mesh.h"
#include "Core/Components/Terrain/TerrainManager.h"
#include "Core/Scene/Object.h"
#include "Core/Scene/Scene.h"
#include "imgui.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace Engine::Editor
{
namespace
{
using Engine::Components::TerrainManager;
using Engine::Serialization::JsonValue;

// The scene object whose plane fills the lakes (build_landscape_demo.py):
// moved to the new water level and resized with the map.
const char* const LakeWaterName = "Lake water";

TerrainManager* FindManager(Engine::Scene::Scene* scene)
{
    if (!scene) return nullptr;
    for (const auto& object : scene->GetObjects())
        if (auto* manager = object ? object->GetComponent<TerrainManager>() : nullptr) return manager;
    return nullptr;
}

std::string Resolve(const std::string& path)
{
    return path.empty() ? path : Engine::Components::Mesh::ResolveFilePath(path);
}
}

void TerrainGeneratorWindow::Open(EditorState& state)
{
    m_open = true;
    if (m_phase == Phase::Idle) Prefill(state);
}

void TerrainGeneratorWindow::Prefill(EditorState& state)
{
    m_paths = {};
    m_status.clear();
    auto* manager = FindManager(state.GetScene());
    if (!manager || manager->heightArchive.empty())
    {
        m_status = "The open scene has no terrain manager with a height archive (e.g. the landscape demo).";
        return;
    }
    m_paths.heights = Resolve(manager->heightArchive);
    m_paths.objectArchive = Resolve(manager->objectArchive);
    m_paths.cellData = manager->cellDataDirectory;
    m_sceneExtent = manager->ArchiveHalfExtent();
    if (m_sceneExtent > 0) m_settings.halfExtentCells = m_sceneExtent;
    // The water level (the scene's lake plane, where it has one) and play
    // area of the current map.
    if (const auto* water = state.GetScene()->FindObjectByName(LakeWaterName))
        m_settings.waterLevel = static_cast<float>(water->transform.GetWorldPositionD().y);
    std::ifstream file(m_paths.heights, std::ios::binary);
    char header[44]{};
    if (file.read(header, sizeof(header)))
    {
        int32_t fine = 0;
        std::memcpy(&fine, header + 36, 4);
        if (fine >= 0 && fine <= m_settings.halfExtentCells) m_settings.fineHalfExtentCells = fine;
    }
}

void TerrainGeneratorWindow::Start(EditorState& state)
{
    if (!state.SaveMainSceneNow())
    {
        m_status = "Could not save the scene: generation not started.";
        return;
    }
    m_scenePath = state.GetCurrentScenePath();
    m_cancel = false;
    m_fraction = 0.f;
    m_stage = "Starting";
    m_jobError.clear();
    m_report = {};
    m_phase = Phase::Generating;
    const auto settings = m_settings;
    const auto paths = m_paths;
    m_job = std::async(std::launch::async, [this, settings, paths] {
        return Engine::Components::GenerateLandscape(settings, paths, m_jobError,
            [this](float fraction, const char* stage) {
                std::lock_guard lock(m_progressMutex);
                m_fraction = fraction;
                m_stage = stage;
            }, &m_cancel, &m_report);
    });
}

void TerrainGeneratorWindow::Draw(EditorState& state)
{
    // The steps after generating run whether or not the window is open.
    auto* console = state.GetConsole();
    const auto log = [&](ConsoleView::Level level, const std::string& text) {
        if (console) console->AddLog(level, "[Terrain generator] " + text);
    };
    if (m_phase == Phase::Generating && m_job.valid() &&
        m_job.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
    {
        if (m_job.get())
        {
            char summary[256];
            std::snprintf(summary, sizeof(summary),
                "heights %.0f..%.0f m in %.1f s: %zu cells changed; re-seated %zu sculpt edits, %zu placements, "
                "%zu object files, %zu volume edit files, %zu foliage files",
                m_report.lowest, m_report.highest, m_report.seconds, m_report.changedCells,
                m_report.sculptsMoved, m_report.placementsMoved, m_report.objectFilesMoved,
                m_report.volumeEditsMoved, m_report.foliageCellsMoved);
            m_status = summary;
            log(ConsoleView::Level::Build, summary);
            // Reload from disk without saving (see the header).
            state.SetHasUnsavedChanges(false);
            state.LoadScene(m_scenePath);
            m_phase = Phase::Reloading;
            m_settleFrames = 2;
        }
        else
        {
            m_status = "Generation failed: " + m_jobError;
            log(ConsoleView::Level::Error, m_status);
            m_phase = Phase::Idle;
        }
    }
    else if ((m_phase == Phase::Reloading || m_phase == Phase::ReloadingResized) && !state.IsSceneLoadPending() &&
        --m_settleFrames <= 0)
    {
        // The lake plane: at the new water level, over the whole map.
        auto* manager = FindManager(state.GetScene());
        if (auto* water = state.GetScene()->FindObjectByName(LakeWaterName); water && manager &&
            m_phase == Phase::Reloading && !water->Parent)
        {
            const double spanX = double(manager->cellsX) * manager->cellWidth;
            const double spanZ = double(manager->cellsY) * manager->cellDepth;
            const int cells = 2 * m_settings.halfExtentCells + 1;
            const glm::dvec3 position(spanX / 2.0, m_settings.waterLevel, spanZ / 2.0);
            const glm::vec3 scale(float(cells * spanX), 1.f, float(cells * spanZ));
            if (water->transform.position != position || water->transform.scale != scale)
            {
                water->transform.position = position;
                water->transform.scale = scale;
                state.SetHasUnsavedChanges(true);
            }
        }
        if (m_phase == Phase::Reloading && m_settings.halfExtentCells != m_sceneExtent)
        {
            // The new size: the manager's procedural cells (the archive
            // already has it), saved, then loaded again.
            if (auto* manager = FindManager(state.GetScene()))
            {
                manager->SetProceduralSquare(m_settings.halfExtentCells);
                state.SetHasUnsavedChanges(true);
                if (state.SaveMainSceneNow())
                {
                    state.LoadScene(m_scenePath);
                    m_sceneExtent = m_settings.halfExtentCells;
                    m_phase = Phase::ReloadingResized;
                    m_settleFrames = 2;
                    log(ConsoleView::Level::Build, "map resized to " +
                        std::to_string(2 * m_settings.halfExtentCells + 1) + " cells a side");
                    return;
                }
            }
            log(ConsoleView::Level::Error, "could not apply the new map size to the scene");
        }
        m_phase = Phase::Idle;
        Prefill(state);
    }

    if (!m_open) return;
    ImGui::SetNextWindowSize(ImVec2(460, 0), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Terrain generator", &m_open))
    {
        ImGui::End();
        return;
    }
    const bool idle = m_phase == Phase::Idle;
    if (!idle) ImGui::BeginDisabled();
    auto& s = m_settings;
    ImGui::TextWrapped("Regenerates the map's base heights for the open scene's terrain. Paint stays; placed "
        "objects, sculpts, volume edits and painted foliage move with the ground. The scene is saved first "
        "and reloaded after.");
    ImGui::Separator();
    ImGui::DragInt("Map half size (cells)", &s.halfExtentCells, .2f, 4, 400);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The map is 2n + 1 cells of 128 m a side (94: 24.2 km).");
    ImGui::DragInt("Play area half size (cells)", &s.fineHalfExtentCells, .2f, 0, s.halfExtentCells);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Stored at 4 m with fine detail; the rest at 16 m.");
    s.fineHalfExtentCells = std::clamp(s.fineHalfExtentCells, 0, s.halfExtentCells);
    ImGui::InputInt("Seed", &s.seed);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("0 is the demo map; any other varies every noise.");
    ImGui::SliderFloat("Relief", &s.relief, .1f, 2.f, "%.2f");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Height of the ranges (1: peaks up to about 1.9 km).");
    ImGui::SliderFloat("Erosion", &s.erosion, 0.f, 1.f, "%.2f droplets / texel");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Droplet hydraulic erosion over the 16 m field (0.2 default; 0 none).");
    ImGui::DragFloat("Water level (m)", &s.waterLevel, .5f, -500.f, 500.f, "%.1f");
    ImGui::Checkbox("Lakes", &s.lakes);
    ImGui::SameLine();
    ImGui::Checkbox("Lowland hills", &s.hills);
    ImGui::SameLine();
    ImGui::Checkbox("Escarpment", &s.escarpment);
    ImGui::Separator();
    ImGui::TextDisabled("Heights: %s", m_paths.heights.empty() ? "(none)" : m_paths.heights.c_str());
    ImGui::TextDisabled("Cell data: %s", m_paths.cellData.empty() ? "(none)" : m_paths.cellData.c_str());
    if (ImGui::Button("Defaults"))
    {
        const auto extent = s.halfExtentCells;
        s = {};
        s.halfExtentCells = extent;
    }
    ImGui::SameLine();
    if (ImGui::Button("Refresh from scene")) Prefill(state);
    ImGui::SameLine();
    const bool ready = !m_paths.heights.empty();
    if (!ready) ImGui::BeginDisabled();
    if (ImGui::Button("Generate")) Start(state);
    if (!ready) ImGui::EndDisabled();
    if (!idle) ImGui::EndDisabled();
    if (m_phase == Phase::Generating)
    {
        float fraction;
        std::string stage;
        {
            std::lock_guard lock(m_progressMutex);
            fraction = m_fraction;
            stage = m_stage;
        }
        ImGui::ProgressBar(fraction, ImVec2(-1, 0), stage.c_str());
        if (ImGui::Button("Cancel")) m_cancel = true;
    }
    else if (m_phase != Phase::Idle)
        ImGui::TextUnformatted("Reloading the scene...");
    if (!m_status.empty()) ImGui::TextWrapped("%s", m_status.c_str());
    ImGui::End();
}
}

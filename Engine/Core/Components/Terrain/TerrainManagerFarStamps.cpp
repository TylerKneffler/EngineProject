#include "TerrainManager.h"
#include "Core/Components/Mesh/Mesh.h"
#include "Core/Components/Terrain/TerrainBatchAppearance.h"
#include "Core/Rendering/DetailBudget.h"
#include "Core/Scene/Object.h"
#include "Core/Scene/Scene.h"
#include <algorithm>
#include <chrono>
#include <cstdlib>

// Far stamps (TerrainManager::farStampRadius). With far terrain on, only the
// cells within loadedRadius are terrain cells; past them the far tiles carry
// the heights (cliff cells have full-resolution far patches) but not the
// stamps, so an escarpment was a smooth wall in its mean colour until it came
// within loadedRadius, then turned into rock. Each stamped cell in range gets
// its stamps as one runtime mesh, built the way its own mesh builds them (the
// coarsest stamp level), shaded as terrain with the loaded cells' layer
// arrays, so the rock reads the same on either side of the hand-off. It is
// hidden while the cell itself is loaded and draws its own stamps.
namespace Engine::Components
{
namespace
{
// Mesh stride the far stamps are built at: the coarsest stamp level.
constexpr int FarStampStride = 8;
// New builds started per update and running at once.
constexpr size_t FarStampStartsPerUpdate = 2, FarStampJobsRunning = 4;
}

void TerrainManager::ClearFarStamps()
{
    for (auto& [cell, entry] : m_farStamps)
    {
        if (entry.job.valid()) entry.job.wait();
        if (entry.object && Owner && Owner->GetScene())
            Owner->GetScene()->RemoveObject(entry.object);
    }
    m_farStamps.clear();
    m_stampCellKnown.clear();
}

void TerrainManager::UpdateFarStamps(int centerX, int centerY)
{
    auto* scene = Owner ? Owner->GetScene() : nullptr;
    // ENGINE_FAR_STAMPS=<cells> overrides farStampRadius (0 disables), for A/B runs.
    static const int forced = []
    {
        const char* setting = std::getenv("ENGINE_FAR_STAMPS");
        return setting ? std::clamp(std::atoi(setting), 0, 64) : -1;
    }();
    const int radius = forced >= 0 ? forced : std::clamp(farStampRadius, 0, 64);
    if (!scene || m_editorStreaming || !m_farEnabled || !m_stampProvider ||
        !m_proceduralBuilder || radius <= loadedRadius)
    {
        if (!m_farStamps.empty()) ClearFarStamps();
        return;
    }
    const auto distanceOf = [&](Cell cell) {
        return std::max(std::abs(cell.first - centerX), std::abs(cell.second - centerY));
    };
    // The loaded cells' layer-array materials (paint layers, then the stamp
    // materials): far stamps share their arrays and slice numbering.
    const std::vector<std::shared_ptr<Material>>* arrayMaterials = nullptr;
    float textureTiling = 16.f;
    for (const auto& [cell, terrain] : m_loaded)
        if (terrain && !terrain->stamps.empty() &&
            terrain->GetArrayMaterials().size() > terrain->layers.size())
        {
            arrayMaterials = &terrain->GetArrayMaterials();
            textureTiling = terrain->textureTiling;
            break;
        }

    // Leave cells well outside the range (one ring of slack, so a cell on
    // the edge does not rebuild as the camera wobbles across a border).
    for (auto it = m_farStamps.begin(); it != m_farStamps.end(); )
    {
        if (distanceOf(it->first) <= radius + 1) { ++it; continue; }
        if (it->second.job.valid()) { ++it; continue; } // finish first
        if (it->second.object) scene->RemoveObject(it->second.object);
        it = m_farStamps.erase(it);
    }

    // Finished builds become scene objects.
    size_t running = 0;
    for (auto& [cell, entry] : m_farStamps)
    {
        if (!entry.job.valid()) continue;
        if (entry.job.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
        { ++running; continue; }
        if (!arrayMaterials) { ++running; continue; } // no materials to shade with yet
        FarStampGeometry geometry;
        try { geometry = entry.job.get(); }
        catch (...) { entry.empty = true; continue; }
        if (geometry.vertices.empty() || geometry.indices.empty()) { entry.empty = true; continue; }
        auto* object = scene->AddObject("Far stamps " + std::to_string(cell.first) + "," +
            std::to_string(cell.second));
        object->runtimeOnly = true;
        object->layer = Engine::Rendering::DetailBudget::Get().FindLayerOrDefault("Terrain");
        scene->MoveObject(object, Owner, Engine::Scene::Scene::ObjectPlacement::AsChild);
        object->transform.position = {
            static_cast<double>(cell.first) * cellsX * cellWidth, 0.0,
            static_cast<double>(cell.second) * cellsY * cellDepth};
        auto* mesh = object->AddComponent<Mesh>();
        if (!mesh->SetIndexedGeometry(std::move(geometry.vertices), std::move(geometry.indices)))
        {
            scene->RemoveObject(object);
            entry.empty = true;
            continue;
        }
        mesh->OnAfterDeserialize(m_provider);
        auto* appearance = object->AddComponent<TerrainBatchAppearance>();
        appearance->materials = *arrayMaterials;
        appearance->textureTiling = geometry.textureTiling;
        entry.object = object;
    }

    // Start builds for stamped cells in range, nearest first.
    std::vector<std::pair<int, Cell>> wanted;
    for (int y = centerY - radius; y <= centerY + radius; ++y)
        for (int x = centerX - radius; x <= centerX + radius; ++x)
        {
            const Cell cell{x, y};
            if (m_farStamps.count(cell) || !HasCell(x, y)) continue;
            auto known = m_stampCellKnown.find(cell);
            if (known == m_stampCellKnown.end())
                known = m_stampCellKnown.emplace(cell, !m_stampProvider(cell).empty()).first;
            if (known->second) wanted.emplace_back(distanceOf(cell), cell);
        }
    std::sort(wanted.begin(), wanted.end());
    size_t started = 0;
    for (const auto& [distance, cell] : wanted)
    {
        if (started >= FarStampStartsPerUpdate || running >= FarStampJobsRunning) break;
        auto& entry = m_farStamps[cell];
        entry.job = std::async(std::launch::async,
            [cell, builder = m_proceduralBuilder, stamps = m_stampProvider,
                materials = m_stampMaterials, x = cellsX, y = cellsY,
                width = cellWidth, depth = cellDepth]()
            {
                FarStampGeometry geometry;
                Engine::Core::Object object;
                auto* terrain = object.AddComponent<Terrain>();
                terrain->Resize(x, y, width, depth);
                builder(cell, *terrain);
                terrain->stamps = stamps(cell);
                terrain->stampMaterials = materials;
                terrain->BuildStampSurface(FarStampStride, geometry.vertices, geometry.indices);
                geometry.textureTiling = terrain->textureTiling;
                return geometry;
            });
        ++started;
        ++running;
    }

    // Shown only while the cell itself is not drawing (its stamps are part
    // of its own mesh once it is loaded and ready).
    for (auto& [cell, entry] : m_farStamps)
    {
        if (!entry.object) continue;
        const auto loaded = m_loaded.find(cell);
        const Mesh* cellMesh = loaded != m_loaded.end() && loaded->second && loaded->second->Owner
            ? loaded->second->Owner->GetComponent<Mesh>() : nullptr;
        const bool covered = cellMesh && cellMesh->IsReady();
        if (auto* mesh = entry.object->GetComponent<Mesh>())
            mesh->SetEditorVisible(!covered);
    }
}
}

#include "Scripts/TerrainGen/TerrainGen.h"
#include "Scripts/TerrainGen/TerrainChunk.h"
#include "Scripts/TerrainGen/TerrainPatch.h"
#include "Scripts/TerrainGen/PerlinNoiseField.h"
#include "Core/Compoonents/Materials/Material.h"
#include "Core/Compoonents/Mesh/Mesh.h"
#include "Core/Compoonents/Physics/PrimitiveObjectCollider.h"
#include "Core/Compoonents/Physics/MeshObjectCollider.h"
#include "Core/Compoonents/Physics/RigidBody.h"
#include "Core/Object.h"
#include "Core/Scene/Scene.h"
#include "Core/Serialization/SceneSerializer.h"
#include "Engine/Editor/UI/IEditorUi.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <thread>
#include <utility>
#if defined(_WIN32)
#include <Windows.h>
#endif

bool TerrainGen::DrawProperties(::Engine::Editor::IEditorUi& ui)
{
    bool changed = false;
    char viewerName[256]{};
    char noiseName[256]{};
    std::snprintf(viewerName, sizeof(viewerName), "%s", viewerObjectName.c_str());
    std::snprintf(noiseName, sizeof(noiseName), "%s", noiseObjectName.c_str());
    if (ui.InputText("Viewer Object", viewerName, sizeof(viewerName)))
    {
        viewerObjectName = viewerName;
        changed = true;
    }
    if (ui.InputText("Noise Object", noiseName, sizeof(noiseName)))
    {
        noiseObjectName = noiseName;
        changed = true;
    }

    changed = ui.DragFloat("Chunk Size", &chunkSize, 0.5f, 1.f, 1000.f) || changed;
    const auto editChunkOrigin = [&](const char* label, int& coordinate)
    {
        char text[32]{};
        std::snprintf(text, sizeof(text), "%d", coordinate);
        if (!ui.InputText(label, text, sizeof(text)))
            return false;
        char* end = nullptr;
        const long long parsed = std::strtoll(text, &end, 10);
        if (end == text || *end != '\0')
            return false;
        coordinate = static_cast<int>(std::clamp(parsed,
            static_cast<long long>(std::numeric_limits<int>::lowest()),
            static_cast<long long>(std::numeric_limits<int>::max())));
        return true;
    };
    changed = editChunkOrigin("World Origin Chunk X", worldOriginChunkX) || changed;
    changed = editChunkOrigin("World Origin Chunk Z", worldOriginChunkZ) || changed;
    ui.DisabledLabel(
        "Large logical coordinates are rebased near zero for stable rendering and physics.");
    changed = ui.SliderInt("View Radius (Chunks)", &viewRadiusInChunks, 0, 8) || changed;
    changed = ui.SliderInt("Launches Per Update", &maxChunkBuildsPerUpdate, 1, 16) || changed;
    changed = ui.SliderInt("Parallel Chunk Builds", &parallelChunkBuilds, 1, 16) || changed;
    changed = ui.SliderInt("Chunk Commits Per Update",
        &maxChunkCommitsPerUpdate, 1, 16) || changed;
    changed = ui.SliderInt("Chunk Unloads Per Update",
        &maxChunkUnloadsPerUpdate, 1, 32) || changed;
    changed = ui.Checkbox("Cache Unloaded Chunk Meshes",
        &cacheUnloadedChunkMeshes) || changed;
    if (cacheUnloadedChunkMeshes)
    {
        changed = ui.SliderInt("Unloaded Mesh Cache Capacity",
            &unloadedMeshCacheCapacity, 0, 1024) || changed;
    }
    changed = ui.SliderInt("Horizontal Cells", &horizontalCellsPerChunk, 2, 48) || changed;
    changed = ui.SliderInt("Vertical Cells", &verticalCells, 2, 48) || changed;
    changed = ui.SliderInt("Mesh Patches Per Axis", &patchesPerAxis, 1, 8) || changed;
    static const char* terrainShapes[] = {
        "Smooth Surface", "Cubes", "Triangles", "Hexagons"
    };
    changed = ui.Combo("Terrain Shape", &terrainShape, terrainShapes, 4) || changed;
    const TerrainShape shape = static_cast<TerrainShape>(terrainShape);
    if (shape == TerrainShape::Cubes || shape == TerrainShape::Triangles ||
        shape == TerrainShape::Hexagons)
    {
        changed = ui.DragFloat("Height Step", &heightStep,
            0.05f, 0.05f, 100.f) || changed;
        ui.DisabledLabel(shape == TerrainShape::Cubes
            ? "Square columns use horizontal tops and vertical walls."
            : shape == TerrainShape::Triangles
                ? "Triangular columns use horizontal tops and vertical walls."
                : "Hexagonal columns use six-sided tops and exposed walls.");
    }
    changed = ui.DragFloat("Vertical Size", &verticalSize, 0.5f, 1.f, 1000.f) || changed;
    changed = ui.DragFloat("Base Height", &baseHeight, 0.1f) || changed;
    changed = ui.DragFloat("Height Amplitude", &heightAmplitude, 0.1f, 0.f, 1000.f) || changed;
    if (shape == TerrainShape::SmoothSurface)
    {
        changed = ui.DragFloat("Cave Strength", &caveStrength, 0.05f, 0.f, 100.f) || changed;
        changed = ui.DragFloat("Cave Frequency Multiplier", &caveFrequencyMultiplier,
            0.05f, 0.01f, 100.f) || changed;
        changed = ui.DragFloat("Iso Level", &isoLevel, 0.05f) || changed;
    }
    changed = ui.DragFloat("Texture Scale", &textureScale, 0.1f, 0.01f, 1000.f) || changed;
    changed = ui.DragFloat("Low Height Maximum", &lowHeightMaximum, 0.1f) || changed;
    changed = ui.ColorEdit3("Low Height Color", &lowHeightColor.x) || changed;
    changed = ui.DragFloat("Middle Height Maximum", &middleHeightMaximum, 0.1f) || changed;
    changed = ui.ColorEdit3("Middle Height Color", &middleHeightColor.x) || changed;
    changed = ui.ColorEdit3("High Height Color", &highHeightColor.x) || changed;
    if (shape == TerrainShape::SmoothSurface)
    {
        changed = ui.DragFloat("Height Color Blend", &heightColorBlend,
            0.005f, 0.f, 0.5f) || changed;
    }
    changed = ui.Checkbox("Generate Colliders", &generateColliders) || changed;
    if (generateColliders)
    {
        changed = ui.SliderInt("Collision Radius (Chunks)",
            &collisionRadiusInChunks, 0, 8) || changed;
        changed = ui.SliderInt("Collider Activations Per Update",
            &maxColliderActivationsPerUpdate, 1, 16) || changed;
        ui.DisabledLabel(
            "Each mesh patch has a collider; nearby patches activate gradually.");
    }

    char value[96]{};
    std::snprintf(value, sizeof(value), "%zu", m_chunks.size());
    ui.ValueLabel("Loaded Chunks", value);
    std::snprintf(value, sizeof(value), "%llu built / %llu unloaded",
        static_cast<unsigned long long>(m_totalChunksBuilt),
        static_cast<unsigned long long>(m_totalChunksUnloaded));
    ui.ValueLabel("Streaming Activity", value);
    std::snprintf(value, sizeof(value), "%.3f ms last / %.3f ms maximum",
        m_lastStreamingMilliseconds, m_maximumStreamingMilliseconds);
    ui.ValueLabel("Streaming CPU Time", value);
    std::snprintf(value, sizeof(value), "%zu cached / %llu hits / %llu misses",
        m_meshCache.size(), static_cast<unsigned long long>(m_meshCacheHits),
        static_cast<unsigned long long>(m_meshCacheMisses));
    ui.ValueLabel("Mesh Cache", value);
    std::snprintf(value, sizeof(value), "%zu queued / %zu generating",
        m_chunkQueue.size(), m_inFlightChunks.size());
    ui.ValueLabel("Parallel Build Queue", value);
    std::snprintf(value, sizeof(value), "%zu", m_pendingChunkUnloadCount);
    ui.ValueLabel("Pending Chunk Unloads", value);
    ui.Separator();
    if (ui.Button("Generate Terrain Now"))
    {
        GenerateTerrain();
        changed = true;
    }
    if (!m_editorGenerationStatus.empty())
        ui.DisabledLabel(m_editorGenerationStatus.c_str());
    return changed;
}

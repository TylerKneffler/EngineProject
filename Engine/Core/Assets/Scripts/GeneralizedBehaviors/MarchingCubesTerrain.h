#pragma once

#include "Core/PropertyMacros.h"
#include "Core/Script.h"
#include "Core/Model/MeshData.h"
#include <glm/glm.hpp>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

class PerlinNoiseField;
class MarchingCubesChunk;

// Streams a square set of volumetric terrain chunks around a named viewer.
// Every cube cell is polygonized from its twelve edge intersections. An
// asymptotic face decider keeps ambiguous saddle cases deterministic.
class MarchingCubesTerrain final : public Engine::Core::Script
{
public:
    MarchingCubesTerrain();

    PROPERTY(Inspector, EditAnywhere, Category = "Terrain | References")
    std::string viewerObjectName = "Marching Cubes Camera";

    PROPERTY(Inspector, EditAnywhere, Category = "Terrain | References")
    std::string noiseObjectName;

    PROPERTY(Inspector, EditAnywhere, Category = "Terrain | Streaming", ClampMin = "1")
    float chunkSize = 16.f;

    PROPERTY(Inspector, EditAnywhere, Category = "Terrain | Streaming", ClampMin = "0", ClampMax = "8")
    int viewRadiusInChunks = 1;

    PROPERTY(Inspector, EditAnywhere, Category = "Terrain | Streaming", ClampMin = "1", ClampMax = "16")
    int maxChunkBuildsPerUpdate = 2;

    PROPERTY(Inspector, EditAnywhere, Category = "Terrain | Resolution", ClampMin = "2", ClampMax = "48")
    int horizontalCellsPerChunk = 12;

    PROPERTY(Inspector, EditAnywhere, Category = "Terrain | Resolution", ClampMin = "2", ClampMax = "48")
    int verticalCells = 12;

    PROPERTY(Inspector, EditAnywhere, Category = "Terrain | Resolution", ClampMin = "1", ClampMax = "8")
    int quadsPerAxis = 2;

    PROPERTY(Inspector, EditAnywhere, Category = "Terrain | Shape", ClampMin = "1")
    float verticalSize = 18.f;

    PROPERTY(Inspector, EditAnywhere, Category = "Terrain | Shape")
    float baseHeight = 0.f;

    PROPERTY(Inspector, EditAnywhere, Category = "Terrain | Shape", ClampMin = "0")
    float heightAmplitude = 6.f;

    PROPERTY(Inspector, EditAnywhere, Category = "Terrain | Shape", ClampMin = "0")
    float caveStrength = 1.4f;

    PROPERTY(Inspector, EditAnywhere, Category = "Terrain | Shape", ClampMin = "0.01")
    float caveFrequencyMultiplier = 2.35f;

    PROPERTY(Inspector, EditAnywhere, Category = "Terrain | Shape")
    float isoLevel = 0.f;

    PROPERTY(Inspector, EditAnywhere, Category = "Terrain | Rendering", ClampMin = "0.01")
    float textureScale = 12.f;

    PROPERTY(Inspector, EditAnywhere, Category = "Terrain | Physics")
    bool generateColliders = false;

    void Start() override;
    void Update() override;
    void OnDestroy() override;

    std::size_t GetLoadedChunkCount() const { return m_chunks.size(); }

private:
    using Vertex = Engine::Model::Vertex;

    static int64_t ChunkKey(int x, int z);
    glm::ivec2 ViewerChunk() const;
    PerlinNoiseField* ResolveNoise() const;
    void RefreshChunks();
    void BuildChunk(int chunkX, int chunkZ);
    std::vector<Vertex> BuildQuadVertices(int chunkX, int chunkZ,
        int quadX, int quadZ, const PerlinNoiseField& noise) const;
    float Density(const glm::vec3& terrainPosition,
        const PerlinNoiseField& noise) const;

    std::unordered_map<int64_t, Engine::Core::Object*> m_chunks;
    glm::ivec2 m_viewerChunk { 0 };
    bool m_hasViewerChunk = false;
};

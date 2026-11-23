#pragma once

#include "Core/Components/Component.h"
#include "Core/Components/Terrain/TerrainVolume.h"
#include <future>
#include <memory>
#include <string>
#include <vector>

namespace Engine::Core { class Object; }

namespace Engine::Components
{
class Material;

// Terrain drawn from a density volume (TerrainVolume), in chunks of
// `chunkCells`^3 cells, each a runtime child object:
//   * Level of detail: a chunk shows level 0 (every sample) inside
//     `lod1Distance` metres of the camera, then 1, 2 and 3 (every 2nd, 4th,
//     8th sample) out to `lod2Distance`, `lod3Distance` and `drawDistance`,
//     switching with the Mesh runtime-LOD cross-fade (`lodFadeSeconds`).
//     Skirts along each chunk's edges hide the gaps between chunks at
//     different levels.
//   * Streaming: chunks are meshed on worker threads (nearest first, at most
//     a few at once) when they come within reach, and dropped past
//     `drawDistance`.
//   * Collision: chunks within `collisionDistance` get a hidden level-0
//     mesh with a MeshObjectCollider on a static RigidBody.
//   * Shading: the terrain's layered path (TerrainBatchAppearance):
//     `flatMaterial` on surfaces facing up, `steepMaterial` on steep faces,
//     overhangs and ceilings, blended by slope (`steepStart` .. `steepEnd`
//     in 1 - normal.y), `textureTiling` repeats over `worldExtent` metres.
// Volume terrain stages 1-2 (.md/todo.md); the data comes whole from
// `volumeFile` (tools/terrain_volume.py), not streamed from disk.
class VolumeTerrain : public Engine::Core::Component
{
public:
    VolumeTerrain();
    ~VolumeTerrain() override;
    void Deserialize(const Engine::Serialization::JsonValue& value) override;
    void OnAfterDeserialize(Engine::Graphics::IGraphicsProvider* provider) override;
    void Update() override;
    void OnDestroy() override;
    // Reloads the volume and drops every chunk (they are remeshed as needed).
    void Rebuild();
    const TerrainVolume* Volume() const { return m_volume.get(); }
    struct Stats
    {
        size_t chunksShown = 0, chunksMeshing = 0, colliders = 0;
        size_t triangles = 0;
        size_t chunksAtLevel[4] = {};
    };
    const Stats& GetStats() const { return m_stats; }

    std::string volumeFile;
    std::string flatMaterial;
    std::string steepMaterial;
    float steepStart = 0.25f;
    float steepEnd = 0.45f;
    float textureTiling = 16.f;
    float worldExtent = 128.f;
    int chunkCells = 32;
    float lod1Distance = 64.f;
    float lod2Distance = 128.f;
    float lod3Distance = 256.f;
    float drawDistance = 600.f;
    float collisionDistance = 96.f;
    float lodFadeSeconds = .35f;

private:
    struct Built
    {
        int level = 0;
        std::vector<Mesh::Vertex> vertices;
        std::vector<uint32_t> indices;
    };
    struct Chunk
    {
        glm::ivec3 index{0};
        glm::vec3 minimum{0.f}, maximum{0.f};   // object space
        Engine::Core::Object* object = nullptr;
        Mesh* mesh = nullptr;
        Engine::Core::Object* collision = nullptr;
        bool resident[4] = {};
        bool empty[4] = {};
        int shown = -1;
        std::future<Built> job;
        bool meshing = false;
        size_t triangles[4] = {};
    };
    void ReleaseChunks();
    void Install(Chunk& chunk, Built built);
    void DropChunk(Chunk& chunk);

    std::shared_ptr<const TerrainVolume> m_volume;
    std::vector<std::shared_ptr<Material>> m_layerMaterials;
    std::vector<Chunk> m_chunks;
    Stats m_stats;
};
}

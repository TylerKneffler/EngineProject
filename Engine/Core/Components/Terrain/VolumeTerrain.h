#pragma once

#include "Core/Components/Component.h"
#include "Core/Components/Terrain/TerrainVolume.h"
#include "Core/Components/Terrain/TerrainVolumeBuild.h"
#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Engine::Core { class Object; }

namespace Engine::Components
{
class Material;

// Terrain drawn from a density volume (TerrainVolume), in chunks of
// `chunkCells`^3 cells, each a runtime child object, all one sheet:
//   * Level of detail: a chunk shows level 0 (every sample) inside
//     `lod1Distance` metres of the camera, then 1, 2 and 3 (every 2nd, 4th,
//     8th sample) out to `lod2Distance`, `lod3Distance` and `drawDistance`.
//   * One layer: chunks tile exactly and are joined across levels
//     (MeshSurfaceNets with SurfaceNetsSeams: the finer chunk joins its edge
//     to the coarser chunk's vertices), so the surface never doubles, also
//     between cells. A chunk's mesh then depends on its neighbours' levels,
//     so levels are chosen for every volume in the scene together, from one
//     camera position that moves on in steps of `lodStepMetres` once the last
//     step is shown; the chunks that change are meshed on worker threads (at
//     most a few at once, nearest first) and all swap in on one frame,
//     cross-fading over `lodFadeSeconds`. Cells join the next step once their
//     volume is built.
//   * Collision: level-0 chunks within `collisionDistance` get a hidden copy
//     with a MeshObjectCollider on a static RigidBody.
//   * Shading: the terrain's layered path (TerrainBatchAppearance):
//     `flatMaterial` on surfaces facing up, `steepMaterial` on steep faces,
//     overhangs and ceilings, blended by slope (`steepStart` .. `steepEnd`
//     in 1 - normal.y), `textureTiling` repeats over `worldExtent` metres.
// Volume terrain stages 1-3 (.md/todo.md); `volumeFile` (tools/terrain_volume.py)
// or the object's Terrain (fromTerrain).
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
    // fromTerrain: the input changed (edits, neighbours): builds again and
    // swaps the new chunks in once the view's are ready; the current ones
    // draw until then. A build under way is followed by another.
    void Refresh();
    const TerrainVolume* Volume() const { return m_volume.get(); }
    struct Stats
    {
        size_t chunksShown = 0, chunksMeshing = 0, colliders = 0;
        size_t triangles = 0;
        size_t chunksAtLevel[4] = {};
    };
    const Stats& GetStats() const { return m_stats; }

    std::string volumeFile;
    // Stage 3: build the volume from this object's Terrain (its heights,
    // BuildTerrainVolume) instead of `volumeFile`, shade it with the
    // terrain's layers (painted weights on gentle ground, `steepLayer` on
    // steep faces) and hide the terrain's own mesh. The Terrain keeps serving
    // heights and paint to everything else.
    bool fromTerrain = false;
    int steepLayer = 4;
    float sampleSpacing = 1.f;
    // fromTerrain: the neighbours' heights (TerrainVolumeInput); the volume
    // reads past the cell's edge. Not serialized.
    std::array<std::vector<float>, 9> neighbourHeights;
    // fromTerrain: shapes added and carved (caves, arches), in this cell's
    // space: `editFile`'s (a .volumeedit), then `edits` (TerrainManager's,
    // the neighbours' included; not serialized).
    std::string editFile;
    std::vector<TerrainVolumeEdit> edits;
    // fromTerrain: paints points of the cell's surface (cell space) with
    // `layers` weights each and a lushness (Terrain::lushness), from the
    // paint rules at the point itself (todo.md "Terrain: volume-first
    // conversion", step 4). Runs on meshing workers, so it must be
    // thread-safe. Null: the terrain's painted grid, interpolated.
    using SurfacePainter = std::function<void(size_t count, const glm::vec3* positions,
        const glm::vec3* normals, size_t layers, float* weights, float* lushness)>;
    std::shared_ptr<const SurfacePainter> surfacePainter;
    // Cells of one grid (TerrainManager sets them; not serialized): volumes
    // of one group at neighbouring cells join into one sheet. A volume of no
    // group stands alone.
    const void* group = nullptr;
    int cellX = 0, cellZ = 0;
    // fromTerrain: scanned rock over steep faces (a seamless rock tile,
    // projected from three sides at its terrainUvScale), tinted.
    std::string rockMaterial = "Assets/TerrainData/Sculpts/cliff_rock.material";
    glm::vec3 rockTint{1.f};
    std::string flatMaterial;
    std::string steepMaterial;
    float steepStart = 0.25f;
    float steepEnd = 0.45f;
    float textureTiling = 16.f;
    float worldExtent = 128.f;
    // Must divide 32 (cells share chunk boundaries) and the volume's width.
    int chunkCells = 32;
    float lod1Distance = 64.f;
    float lod2Distance = 128.f;
    float lod3Distance = 256.f;
    float drawDistance = 600.f;
    float collisionDistance = 96.f;
    float lodFadeSeconds = .35f;
    float lodStepMetres = 16.f;

    // A chunk's mesh: its level and its 26 neighbours' strides (0: none).
    struct Key
    {
        int level = -1;   // -1: not drawn
        std::array<int, 27> strides{};
        // The volumes of this cell and the cells around (seams read them).
        std::array<const void*, 9> cells{};
        bool operator==(const Key& other) const
        { return level == other.level && strides == other.strides && cells == other.cells; }
        bool operator!=(const Key& other) const { return !(*this == other); }
    };

private:
    // A meshed chunk. The meshing job also builds the CPU mesh (bounds
    // included) and its buffers' bytes, and the GPU vertex buffer where the
    // backend can create one off the main thread; Update installs the rest
    // as each chunk arrives. The swap, which every cell of a group takes on
    // the same frame, then only switches meshes: installing ~100 chunks at
    // the swap held the main thread up to 96 ms (meadow walk, 2026-11-24).
    struct Built
    {
        Key key;
        size_t triangles = 0;
        std::unique_ptr<Mesh> mesh;
        std::shared_ptr<Engine::Graphics::IGraphicsBuffer> vertexBuffer;
        bool installed = false;
    };
    struct Chunk
    {
        glm::ivec3 index{0};                    // in this volume
        glm::ivec3 world{0};                    // in the group
        glm::vec3 minimum{0.f}, maximum{0.f};   // object space
        Engine::Core::Object* object = nullptr;
        Mesh* mesh = nullptr;
        Engine::Core::Object* collision = nullptr;
        Key shown;                  // drawn now
        int slot = 0;               // the drawn mesh's runtime-LOD slot
        size_t triangles = 0;
        Key wanted;                 // for the step being prepared
        std::future<Built> job;
        Key jobKey;
        bool meshing = false;
        std::optional<Built> ready; // meshed for `wanted`, waiting for the swap
    };
    void ReleaseChunks();
    void InstallBuilt(Built& built, Engine::Graphics::IGraphicsProvider* provider);
    // Sets up the chunk table for m_volume.
    void InstallVolume();
    // fromTerrain: the density being built on a worker thread.
    std::future<std::shared_ptr<TerrainVolume>> m_volumeJob;
    // fromTerrain: starts a build of the current input (false: no build slot
    // free, or no heights yet).
    bool StartBuild();
    // Another build is wanted once the one under way ends (Refresh).
    bool m_rebuildPending = false;
    // Chunks of the volume being replaced, drawn until the new ones show.
    std::vector<Chunk> m_retiring;
    void DropChunk(Chunk& chunk);
    // Level choice shared by the scene's volumes (VolumeTerrain.cpp).
    void Register();
    void Unregister();
    int LevelAt(const glm::ivec3& world, const glm::dvec3& camera) const;
    int StrideAt(const glm::ivec3& world, const glm::dvec3& camera) const;
    bool HasChunk(const glm::ivec3& world) const;
    void ChooseKeys(const glm::dvec3& camera);
    // The volumes of the cells around (index (dz + 1) * 3 + dx + 1) in the
    // step; null where there is none.
    std::array<std::shared_ptr<const TerrainVolume>, 9> m_cellVolumes;
    void SwapIn(const glm::vec3& eye);
    glm::ivec3 ChunkGrid() const;
    int ChunkYBase() const;
    glm::dvec3 Anchor() const;   // world position of the group's chunk (0, 0, 0)
    uint64_t m_keysStep = 0;     // the step m_chunks' wanted keys are for
    bool m_registered = false;

public:
    // A terrain's painted layer weights (fromTerrain), read by meshing jobs.
    struct Paint
    {
        int cellsX = 0, cellsY = 0;
        float cellWidth = 1.f, cellDepth = 1.f;
        size_t layers = 0;
        std::vector<float> weights;   // per grid vertex and layer
        std::vector<float> lushness;  // per grid vertex (may be empty)
        std::shared_ptr<const SurfacePainter> painter;
    };
    // Scanned rock over steep faces (the triplanar rock path, Object.hlsl):
    // a layer-array slice after the painted layers, projected from three sides.
    struct RockSlice
    {
        int slice = -1;            // -1: none
        float repeatsPerMetre = .1f;
        glm::vec3 tint{1.f};
    };
    // Read by the shared level choice.
    bool PreparedFor(uint64_t step) const { return m_preparedStep == step; }
    // fromTerrain: its chunks draw the cell (the first ones have swapped in).
    bool IsShowing() const { return m_terrainHidden; }
    // ENGINE_VOLUME_TERRAIN_CHECK=1, after each swap: the shown chunks of
    // these volumes must be one closed sheet. Vertical lines every metre must
    // cross it an odd number of times (an even count is a hole or a doubled
    // layer) and every edge must be shared by two triangles; prints counts and
    // the first places that fail.
    static void CheckLayer(const std::vector<VolumeTerrain*>& volumes);
private:
    uint64_t m_preparedStep = 0;
    std::shared_ptr<const TerrainVolume> m_volume;
    std::shared_ptr<const Paint> m_paint;
    std::vector<std::shared_ptr<Material>> m_layerMaterials;
    RockSlice m_rock;
    float m_tiling = 16.f, m_extent = 128.f;
    // fromTerrain: the terrain's own mesh has been handed over (hidden).
    bool m_terrainHidden = false;
    // fromTerrain: the Terrain surface revision the volume was built from;
    // a newer one (height or paint edits) builds again.
    uint64_t m_terrainSurfaceRevision = 0;
    std::chrono::steady_clock::time_point m_buildStarted{};
    std::vector<Chunk> m_chunks;
    Stats m_stats;
};
}

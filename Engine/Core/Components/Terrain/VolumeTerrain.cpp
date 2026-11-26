#include "Core/Components/Terrain/VolumeTerrain.h"

#include "Core/Components/Camera/Camera.h"
#include "Core/Components/Materials/Material.h"
#include "Core/Components/Mesh/Mesh.h"
#include "Core/Components/Physics/MeshObjectCollider.h"
#include "Core/Components/Physics/RigidBody.h"
#include "Core/Components/Terrain/Terrain.h"
#include "Core/Components/Terrain/TerrainBatchAppearance.h"
#include "Core/Components/Terrain/TerrainVolumeBuild.h"
#include "Core/Scene/Object.h"
#include "Core/Rendering/SharedMeshBuffers.h"
#include "Core/Graphics/IGraphicsProvider.h"
#include "Core/Scene/Scene.h"
#include <algorithm>
#include <chrono>
#include <atomic>
#include <map>
#include <deque>
#include <fstream>
#include <future>
#include <filesystem>
#include <unordered_map>
#include <tuple>
#include <mutex>
#include <cstdio>
#include <cstdlib>
#include <thread>

namespace Engine::Components
{
namespace
{
constexpr int LevelCount = 4;

// Shared by every volume (a landscape loads dozens of cells at once): density
// builds and chunk meshing jobs in flight, each capped.
std::atomic<int> g_builds{0};
std::atomic<int> g_meshJobs{0};
// GPU buffers of finished chunks are installed under one budget per frame
// for all volumes of a scene (main thread only): jobs finish in bursts.
// ENGINE_VOLUME_INSTALL_MS overrides the 2 ms.
struct InstallBudget
{
    uint64_t frame = UINT64_MAX;
    double usedMs = 0.0;
    int installs = 0;
};
std::map<const void*, InstallBudget> g_installBudgets;
// Replaced chunk meshes, released a few a frame (main thread only): a
// group's swap replaced ~100 at once and released their GPU buffers on the
// spot, up to 7 ms a cell (meadow walk, 2026-11-24).
struct RetiredMeshes
{
    uint64_t frame = UINT64_MAX;
    std::deque<std::unique_ptr<Engine::Components::Mesh>> meshes;
};
std::map<const void*, RetiredMeshes> g_retiredMeshes;
// The frame volumes step by: the scene clock while playing; in edit mode
// that clock stands still (every update read the same frame, so no level
// step was ever shown and the upload budget never renewed), so the editor
// advances a count of its own (VolumeTerrain::AdvanceEditorFrame).
std::atomic<uint64_t> g_editorFrame{0};
uint64_t FrameOf(const Engine::Scene::Scene& scene)
{
    return scene.HasStarted() ? scene.GetFrameIndex() : (uint64_t(1) << 63) | g_editorFrame.load();
}

void ReleaseRetiredMeshes(const void* scene, uint64_t frame)
{
    auto& retired = g_retiredMeshes[scene];
    if (retired.frame == frame) return;
    retired.frame = frame;
    const auto start = std::chrono::steady_clock::now();
    const size_t queued = retired.meshes.size();
    // Destroyed on a worker: each chunk mesh's committed D3D12 buffers took
    // ~1 ms to release on the main thread (23 m/s walk: 2058 meshes, a 1 ms
    // budget spent every frame, a backlog of up to 360). The meshes have no
    // owner object (no scene hooks), are no longer drawn, and command packets
    // that still bind their buffers hold references of their own.
    // ENGINE_VOLUME_ASYNC_RELEASE=0 releases on the main thread (A/B).
    static const bool async = [] {
        const char* value = std::getenv("ENGINE_VOLUME_ASYNC_RELEASE");
        return !(value && value[0] == '0');
    }();
    static std::future<void> releasing;
    if (async && !retired.meshes.empty() &&
        (!releasing.valid() || releasing.wait_for(std::chrono::seconds(0)) == std::future_status::ready))
    {
        if (releasing.valid()) releasing.get();
        releasing = std::async(std::launch::async,
            [meshes = std::move(retired.meshes)]() mutable { meshes.clear(); });
        retired.meshes.clear();
    }
    while (!retired.meshes.empty() && !async &&
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() < 1.0)
        retired.meshes.pop_front();
    static const bool log = std::getenv("ENGINE_VOLUME_TERRAIN_LOG") != nullptr;
    if (log && queued)
        std::printf("[VolumeRetire] frame %llu released %zu of %zu in %.2f ms\n",
            static_cast<unsigned long long>(frame), queued - retired.meshes.size(), queued,
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
}
double InstallBudgetMs()
{
    static const double budget = [] {
        const char* value = std::getenv("ENGINE_VOLUME_INSTALL_MS");
        return value && *value ? std::max(.1, std::atof(value)) : 2.0;
    }();
    return budget;
}
constexpr int MaxBuilds = 2;
int MaxMeshJobs()
{
    static const int jobs = std::clamp(static_cast<int>(std::thread::hardware_concurrency()) / 2, 1, 6);
    return jobs;
}
// Decrements a counter when a job ends, however it ends.
struct JobSlot
{
    std::atomic<int>* counter;
    ~JobSlot() { counter->fetch_sub(1); }
};

// Layer 0 (flat) and 1 (steep) weights in the terrain shader's vertex
// channels, by slope; uv for the paint map's place (unused without one).
void ApplySlopeWeights(std::vector<Mesh::Vertex>& vertices, float start, float end, float extent)
{
    start = std::min(start, end - 1.e-3f);
    for (auto& vertex : vertices)
    {
        const float t = std::clamp((1.f - vertex.normal[1] - start) / (end - start), 0.f, 1.f);
        const float steep = t * t * (3.f - 2.f * t);
        vertex.uvChannels[2][0] = 1.f - steep;
        vertex.uvChannels[2][1] = steep;
        vertex.uvChannels[2][2] = vertex.uvChannels[2][3] = 0.f;
        vertex.uvChannels[3][0] = vertex.uvChannels[3][1] = 0.f;
        vertex.uv[0] = std::clamp(vertex.pos[0] / std::max(extent, 1.f), 0.f, 1.f);
        vertex.uv[1] = std::clamp(vertex.pos[2] / std::max(extent, 1.f), 0.f, 1.f);
        // The terrain shader adds textured layers onto the vertex colour,
        // which carries untextured layers' colours (Terrain::BuildVertex):
        // none here.
        vertex.color[0] = vertex.color[1] = vertex.color[2] = 0.f;
        vertex.color[3] = 1.f;
    }
}

using RockSurface = VolumeTerrain::RockSlice;
// ENGINE_VOLUME_TERRAIN_LOG: paint time on meshing workers (microseconds)
// and vertices painted, over the run.
std::atomic<uint64_t> g_paintMicroseconds{0}, g_paintVertices{0};

// The paint at each vertex: the painter's (the paint rules at the vertex's
// own position, height and normal) or else the terrain's painted grid at its
// ground position; then the steep layer on steep faces, overhangs and
// ceilings, and the scanned rock blended over those (when there is one) by
// the same slope. Lushness (uvChannels1.w) as Terrain::BuildVertex has it.
void ApplyPaintWeights(std::vector<Mesh::Vertex>& vertices, const VolumeTerrain::Paint& paint,
    int steepLayer, float start, float end, const RockSurface& rock)
{
    start = std::min(start, end - 1.e-3f);
    const size_t layers = std::min<size_t>(paint.layers, 6);
    const auto grid = [&](const std::vector<float>& values, size_t stride, size_t layer,
        int col, int row, float tx, float tz)
    {
        const auto at = [&](int c, int r) {
            c = std::clamp(c, 0, paint.cellsX);
            r = std::clamp(r, 0, paint.cellsY);
            const size_t i = (size_t(r) * (paint.cellsX + 1) + c) * stride + layer;
            return i < values.size() ? values[i] : 0.f;
        };
        return (at(col, row) * (1 - tx) + at(col + 1, row) * tx) * (1 - tz) +
            (at(col, row + 1) * (1 - tx) + at(col + 1, row + 1) * tx) * tz;
    };
    for (size_t index = 0; index < vertices.size(); ++index)
    {
        auto& vertex = vertices[index];
        const float fx = std::clamp(vertex.pos[0] / paint.cellWidth, 0.f, float(paint.cellsX));
        const float fz = std::clamp(vertex.pos[2] / paint.cellDepth, 0.f, float(paint.cellsY));
        const int col = std::min(int(fx), paint.cellsX - 1), row = std::min(int(fz), paint.cellsY - 1);
        const float tx = fx - col, tz = fz - row;
        const float t = std::clamp((1.f - vertex.normal[1] - start) / (end - start), 0.f, 1.f);
        const float steep = t * t * (3.f - 2.f * t);
        for (size_t layer = 0; layer < 6; ++layer)
        {
            float w = 0.f;
            if (layer < layers) w = grid(paint.weights, paint.layers, layer, col, row, tx, tz);
            w = w * (1.f - steep) + (int(layer) == steepLayer ? steep : 0.f);
            vertex.uvChannels[2 + layer / 4][layer % 4] = w;
        }
        vertex.uvChannels[1][3] = paint.lushness.empty() ? 0.f : grid(paint.lushness, 1, 0, col, row, tx, tz);
        vertex.uv[0] = std::clamp(vertex.pos[0] / (paint.cellsX * paint.cellWidth), 0.f, 1.f);
        vertex.uv[1] = std::clamp(vertex.pos[2] / (paint.cellsY * paint.cellDepth), 0.f, 1.f);
        vertex.color[0] = vertex.color[1] = vertex.color[2] = 0.f;
        vertex.color[3] = 1.f;
        if (rock.slice >= 0)
        {
            // uvChannels4: repeats per metre, -, -(slice + 1), blend; tint in
            // uvChannels0.zw and uvChannels1.z.
            vertex.uvChannels[4][0] = rock.repeatsPerMetre;
            vertex.uvChannels[4][1] = 0.f;
            vertex.uvChannels[4][2] = -(static_cast<float>(rock.slice) + 1.f);
            vertex.uvChannels[4][3] = steep;
            vertex.uvChannels[0][2] = rock.tint.x;
            vertex.uvChannels[0][3] = rock.tint.y;
            vertex.uvChannels[1][2] = rock.tint.z;
        }
    }
}

TerrainVolumeInput WithNeighbours(TerrainVolumeInput input,
    const std::array<std::vector<float>, 9>& heights, std::vector<TerrainVolumeEdit> edits)
{
    input.neighbourHeights = heights;
    input.edits = std::move(edits);
    return input;
}

float DistanceToBox(const glm::vec3& point, const glm::vec3& minimum, const glm::vec3& maximum)
{
    const glm::vec3 outside = glm::max(glm::max(minimum - point, point - maximum), glm::vec3(0.f));
    return glm::length(outside);
}

int FloorDiv(int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

// Level choice shared by a scene's volumes. A chunk's mesh depends on its
// neighbours' levels (the seams), so every volume chooses from one camera
// position and the same set of volumes (a step), meshes what changes, and
// all show the step on one frame once every volume has it ready. The next
// step starts once that one is shown: when the camera has moved
// lodStepMetres, or volumes came or went.
using CellKey = std::tuple<const void*, int, int>;
struct LevelSteps
{
    std::map<CellKey, VolumeTerrain*> cells;     // volumes built
    std::map<CellKey, VolumeTerrain*> members;   // the step's volumes
    glm::dvec3 camera{0.0};
    uint64_t step = 0, shown = 0;
    bool dirty = false;
    bool swap = false;      // this frame shows `step`
    bool check = false;     // ENGINE_VOLUME_TERRAIN_CHECK: check the shown step next frame
    uint64_t frame = UINT64_MAX;
};
std::map<const void*, LevelSteps> g_steps;   // by scene; main thread only

void Advance(LevelSteps& steps, uint64_t frame, const glm::dvec3& eye, float stepMetres)
{
    if (steps.frame == frame) return;
    steps.frame = frame;
    steps.swap = false;
    if (steps.check)
    {
        steps.check = false;
        std::vector<VolumeTerrain*> volumes;
        for (const auto& [key, volume] : steps.members) volumes.push_back(volume);
        VolumeTerrain::CheckLayer(volumes);
    }
    if (steps.shown == steps.step)
    {
        if (steps.cells.empty()) return;
        if (!steps.dirty && glm::length(eye - steps.camera) <= std::max(stepMetres, 0.f)) return;
        steps.camera = eye;
        steps.members = steps.cells;
        ++steps.step;
        steps.dirty = false;
        return;
    }
    for (const auto& [key, volume] : steps.members)
        if (!volume->PreparedFor(steps.step)) return;
    steps.swap = true;
    steps.shown = steps.step;
    static const bool check = std::getenv("ENGINE_VOLUME_TERRAIN_CHECK") != nullptr;
    steps.check = check;
}
}

VolumeTerrain::VolumeTerrain()
{
    SetTypeName(COMPONENT_TYPE_NAME(VolumeTerrain));
    singlecomponent = true;
    RegisterField("volumeFile", volumeFile, "Volume");
    RegisterField("fromTerrain", fromTerrain, "Volume");
    RegisterField("steepLayer", steepLayer, "Volume");
    RegisterField("sampleSpacing", sampleSpacing, "Volume");
    RegisterField("rockMaterial", rockMaterial, "Volume");
    RegisterField("editFile", editFile, "Volume");
    RegisterField("flatMaterial", flatMaterial, "Shading");
    RegisterField("steepMaterial", steepMaterial, "Shading");
    RegisterField("steepStart", steepStart, "Shading");
    RegisterField("steepEnd", steepEnd, "Shading");
    RegisterField("textureTiling", textureTiling, "Shading");
    RegisterField("worldExtent", worldExtent, "Shading");
    RegisterField("chunkCells", chunkCells, "Detail");
    RegisterField("lod1Distance", lod1Distance, "Detail");
    RegisterField("lod2Distance", lod2Distance, "Detail");
    RegisterField("lod3Distance", lod3Distance, "Detail");
    RegisterField("drawDistance", drawDistance, "Detail");
    RegisterField("collisionDistance", collisionDistance, "Detail");
    RegisterField("lodFadeSeconds", lodFadeSeconds, "Detail");
    RegisterField("lodStepMetres", lodStepMetres, "Detail");
}

VolumeTerrain::~VolumeTerrain()
{
    Unregister();
    // Jobs hold the volume, not this component; just let them finish.
    for (auto* list : {&m_chunks, &m_retiring})
        for (auto& chunk : *list)
            if (chunk.job.valid()) chunk.job.wait();
}

void VolumeTerrain::Deserialize(const Engine::Serialization::JsonValue& value)
{
    Component::Deserialize(value);
    Rebuild();
}

void VolumeTerrain::OnAfterDeserialize(Engine::Graphics::IGraphicsProvider*)
{
    Rebuild();
}

void VolumeTerrain::OnDestroy()
{
    ReleaseChunks();
}

void VolumeTerrain::ReleaseChunks()
{
    Unregister();
    for (auto* list : {&m_chunks, &m_retiring})
    {
        for (auto& chunk : *list)
        {
            if (chunk.job.valid()) chunk.job.wait();
            DropChunk(chunk);
        }
        list->clear();
    }
}

void VolumeTerrain::DropChunk(Chunk& chunk)
{
    auto* scene = Owner ? Owner->GetScene() : nullptr;
    if (scene)
    {
        if (chunk.object) scene->RequestRemoveObject(chunk.object);
        if (chunk.collision) scene->RequestRemoveObject(chunk.collision);
    }
    chunk.object = nullptr;
    chunk.collision = nullptr;
    chunk.mesh = nullptr;
    chunk.shown = Key{};
    chunk.slot = 0;
    chunk.triangles = 0;
}

void VolumeTerrain::Rebuild()
{
    if (!Owner) return;
    // A build already under way (load calls Rebuild from Deserialize and
    // again from OnAfterDeserialize) is the same build: keep it and its
    // paint and materials.
    if (fromTerrain && m_volumeJob.valid()) return;
    ReleaseChunks();
    m_paint.reset();
    m_layerMaterials.clear();
    m_tiling = textureTiling;
    m_extent = worldExtent;
    if (fromTerrain)
    {
        // Built from the Terrain beside it; until that terrain has heights
        // (component load order) or a build slot is free, Update retries.
        m_volume.reset();
        m_rebuildPending = !StartBuild();
        // The volume draws the cell; the terrain's own mesh
        // keeps drawing until the volume covers the view (Update), then stays
        // for its data and queries only.
        m_terrainHidden = false;
        m_buildStarted = std::chrono::steady_clock::now();
        if (auto* mesh = Owner->GetComponent<Mesh>()) mesh->SetAnimationVisible(true);
        return;
    }
    m_volume = volumeFile.empty() ? nullptr : TerrainVolume::Load(Mesh::ResolveFilePath(volumeFile));
    if (!m_volume) return;
    // Layer materials, shared by every chunk (one set of layer arrays).
    if (!flatMaterial.empty() || !steepMaterial.empty())
        for (const std::string* path : {&flatMaterial, &steepMaterial})
        {
            auto material = std::make_shared<Material>();
            m_layerMaterials.push_back(
                !path->empty() && material->LoadFromFile(Mesh::ResolveFilePath(*path))
                    ? std::move(material) : nullptr);
        }
    InstallVolume();
}

void VolumeTerrain::Refresh()
{
    if (!fromTerrain) { Rebuild(); return; }
    if (!Owner) return;
    // A build under way read the old input: another follows it.
    if (m_volumeJob.valid() || !m_volume) { m_rebuildPending = true; return; }
    m_rebuildPending = !StartBuild();
}

bool VolumeTerrain::StartBuild()
{
    auto* terrain = Owner ? Owner->GetComponent<Terrain>() : nullptr;
    if (!terrain || terrain->heights.empty() || m_volumeJob.valid()) return false;
    if (g_builds.fetch_add(1) >= MaxBuilds) { g_builds.fetch_sub(1); return false; }
    m_terrainSurfaceRevision = terrain->GetSurfaceRevision();
    std::vector<TerrainVolumeEdit> allEdits;
    if (!editFile.empty() && !LoadTerrainVolumeEdits(Mesh::ResolveFilePath(editFile), allEdits))
        std::fprintf(stderr, "[VolumeTerrain] %s: malformed edit file %s\n", Owner->name.c_str(), editFile.c_str());
    allEdits.insert(allEdits.end(), edits.begin(), edits.end());
    static const bool log = std::getenv("ENGINE_VOLUME_TERRAIN_LOG") != nullptr;
    if (log)
    {
        int neighbours = 0;
        for (const auto& heights : neighbourHeights) neighbours += !heights.empty();
        std::printf("[VolumeTerrain] %s build: %zu edits, %d neighbour grids\n",
            Owner->name.c_str(), allEdits.size(), neighbours);
    }
    // The density is built on a worker from a copy of the terrain's data;
    // Update installs it (InstallVolume) when it is ready.
    m_volumeJob = std::async(std::launch::async,
        [input = WithNeighbours(TerrainVolumeInput::From(*terrain), neighbourHeights,
             std::move(allEdits)),
         spacing = std::max(sampleSpacing, .25f)]
        { JobSlot slot{&g_builds}; return BuildTerrainVolume(input, spacing); });
    auto paint = std::make_shared<Paint>();
    paint->cellsX = terrain->cellsX; paint->cellsY = terrain->cellsY;
    paint->cellWidth = terrain->cellWidth; paint->cellDepth = terrain->cellDepth;
    paint->layers = terrain->layers.size();
    paint->weights = terrain->weights;
    paint->lushness = terrain->lushness;
    m_paint = std::move(paint);
    m_layerMaterials = terrain->GetLayerMaterials();
    m_layerMaterials.resize(std::min<size_t>(m_layerMaterials.size(), 6));
    // Scanned rock: the slice after the painted layers. One material object
    // for every cell, so they share one set of layer arrays.
    m_rock = {};
    // ENGINE_VOLUME_ROCK=0: no scanned rock slice (A/B its shading cost).
    static const bool rockOff = [] {
        const char* value = std::getenv("ENGINE_VOLUME_ROCK");
        return value && value[0] == '0';
    }();
    if (!rockMaterial.empty() && !rockOff)
    {
        static std::mutex mutex;
        static std::map<std::string, std::shared_ptr<Material>> shared;
        std::lock_guard lock(mutex);
        auto& material = shared[rockMaterial];
        if (!material)
        {
            auto loaded = std::make_shared<Material>();
            if (loaded->LoadFromFile(Mesh::ResolveFilePath(rockMaterial))) material = std::move(loaded);
        }
        if (material)
        {
            m_rock.slice = static_cast<int>(m_layerMaterials.size());
            m_rock.repeatsPerMetre = material->terrainUvScale > 0.f ? material->terrainUvScale : .1f;
            m_rock.tint = rockTint;
            m_layerMaterials.push_back(material);
        }
    }
    m_tiling = terrain->textureTiling;
    m_extent = std::max(terrain->cellsX * terrain->cellWidth, terrain->cellsY * terrain->cellDepth);
    return true;
}

void VolumeTerrain::InstallVolume()
{
    if (!m_volume) return;
    const int cells = std::clamp(chunkCells, 4, 256);
    // Chunks tile the meshed interior; a volume's margin is only read.
    const glm::ivec3 grid = ChunkGrid();
    const int yBase = ChunkYBase();
    m_chunks.resize(static_cast<size_t>(grid.x) * grid.y * grid.z);
    size_t next = 0;
    for (int y = 0; y < grid.y; ++y)
        for (int z = 0; z < grid.z; ++z)
            for (int x = 0; x < grid.x; ++x)
            {
                auto& chunk = m_chunks[next++];
                chunk.index = {x, y, z};
                chunk.world = {cellX * grid.x + x, yBase + y, cellZ * grid.z + z};
                chunk.minimum = m_volume->origin +
                    glm::vec3(m_volume->interiorFirst + chunk.index * cells) * m_volume->spacing;
                chunk.maximum = chunk.minimum + glm::vec3(static_cast<float>(cells) * m_volume->spacing);
            }
    m_keysStep = 0;
    m_preparedStep = 0;
    Register();
}

glm::ivec3 VolumeTerrain::ChunkGrid() const
{
    if (!m_volume) return glm::ivec3(0);
    const int cells = std::clamp(chunkCells, 4, 256);
    return (m_volume->InteriorCells() + cells - 1) / cells;
}

int VolumeTerrain::ChunkYBase() const
{
    if (!m_volume) return 0;
    const float size = std::clamp(chunkCells, 4, 256) * m_volume->spacing;
    return static_cast<int>(std::floor(m_volume->origin.y / size + .5f));
}

glm::dvec3 VolumeTerrain::Anchor() const
{
    // World position of the group's chunk (0, 0, 0): this cell's first chunk,
    // less the cells before it.
    const glm::dvec3 world = Owner->transform.GetWorldPositionD();
    const glm::ivec3 grid = ChunkGrid();
    const double size = std::clamp(chunkCells, 4, 256) * static_cast<double>(m_volume->spacing);
    const glm::dvec3 first = glm::dvec3(m_volume->origin) +
        glm::dvec3(m_volume->interiorFirst) * static_cast<double>(m_volume->spacing);
    return world + glm::dvec3(first.x - cellX * grid.x * size, 0.0, first.z - cellZ * grid.z * size);
}

int VolumeTerrain::LevelAt(const glm::ivec3& world, const glm::dvec3& camera) const
{
    const double size = std::clamp(chunkCells, 4, 256) * static_cast<double>(m_volume->spacing);
    const glm::dvec3 low = Anchor() + glm::dvec3(world) * size;
    const glm::dvec3 outside = glm::max(glm::max(low - camera, camera - (low + size)), glm::dvec3(0.0));
    const double distance = glm::length(outside);
    const float bounds[LevelCount] = {lod1Distance, lod2Distance, lod3Distance, drawDistance};
    for (int level = 0; level < LevelCount; ++level)
        if (distance <= bounds[level])
        {
            // A chunk on the border with distant terrain (a neighbouring
            // cell that is not loaded) meshes at the distant nodes' spacing
            // (a one-cell node: NodeGrid = 32 cells across the cell), so its
            // border samples are the node's and both put their border
            // vertices at the same points (MeshSurfaceNets snaps them onto
            // the face from the face's own crossings).
            const int border = BorderLevel();
            return border >= 0 && BordersDistant(world) ? border : level;
        }
    return -1;
}

int VolumeTerrain::BorderLevel() const
{
    const int across = m_volume ? m_volume->InteriorCells().x : 0;
    for (int level = 0; level < LevelCount; ++level)
        if ((32 << level) == across) return level;
    return -1;
}

bool VolumeTerrain::BordersDistant(const glm::ivec3& world) const
{
    const glm::ivec3 grid = ChunkGrid();
    if (grid.x <= 0 || grid.z <= 0) return false;
    const int lx = world.x - cellX * grid.x, lz = world.z - cellZ * grid.z;
    const int sx = lx == 0 ? -1 : lx == grid.x - 1 ? 1 : 0;
    const int sz = lz == 0 ? -1 : lz == grid.z - 1 ? 1 : 0;
    if (!sx && !sz) return false;
    auto* scene = Owner ? Owner->GetScene() : nullptr;
    const auto steps = g_steps.find(scene);
    if (steps == g_steps.end()) return false;
    const auto missing = [&](int dx, int dz) {
        return !steps->second.members.count({group ? group : Owner, cellX + dx, cellZ + dz});
    };
    return (sx && missing(sx, 0)) || (sz && missing(0, sz)) || (sx && sz && missing(sx, sz));
}

bool VolumeTerrain::HasChunk(const glm::ivec3& world) const
{
    const glm::ivec3 grid = ChunkGrid();
    const glm::ivec3 local = world - glm::ivec3(cellX * grid.x, ChunkYBase(), cellZ * grid.z);
    return glm::all(glm::greaterThanEqual(local, glm::ivec3(0))) && glm::all(glm::lessThan(local, grid));
}

int VolumeTerrain::StrideAt(const glm::ivec3& world, const glm::dvec3& camera) const
{
    const glm::ivec3 grid = ChunkGrid();
    if (grid.x <= 0 || grid.z <= 0) return 0;
    const int x = FloorDiv(world.x, grid.x), z = FloorDiv(world.z, grid.z);
    const VolumeTerrain* volume = this;
    if (x != cellX || z != cellZ)
    {
        auto* scene = Owner ? Owner->GetScene() : nullptr;
        const auto steps = g_steps.find(scene);
        if (steps == g_steps.end()) return 0;
        const auto found = steps->second.members.find({group ? group : Owner, x, z});
        if (found == steps->second.members.end()) return 0;
        volume = found->second;
    }
    if (!volume->m_volume || !volume->HasChunk(world)) return 0;
    const int level = LevelAt(world, camera);
    return level < 0 ? 0 : 1 << level;
}

void VolumeTerrain::Register()
{
    auto* scene = Owner ? Owner->GetScene() : nullptr;
    if (!scene) return;
    auto& steps = g_steps[scene];
    steps.cells[{group ? group : Owner, cellX, cellZ}] = this;
    steps.dirty = true;
    m_registered = true;
}

void VolumeTerrain::Unregister()
{
    if (!m_registered) return;
    m_registered = false;
    for (auto& [scene, steps] : g_steps)
    {
        for (auto* map : {&steps.cells, &steps.members})
            for (auto it = map->begin(); it != map->end(); )
                it = it->second == this ? map->erase(it) : std::next(it);
        steps.dirty = true;
    }
}

void VolumeTerrain::ChooseKeys(const glm::dvec3& camera)
{
    // The step's cells around this one.
    m_cellVolumes = {};
    auto* scene = Owner ? Owner->GetScene() : nullptr;
    const auto steps = g_steps.find(scene);
    for (int dz = -1; dz <= 1; ++dz)
        for (int dx = -1; dx <= 1; ++dx)
        {
            const VolumeTerrain* other = this;
            if (dx || dz)
            {
                other = nullptr;
                if (steps != g_steps.end())
                    if (const auto found = steps->second.members.find({group ? group : Owner, cellX + dx, cellZ + dz});
                        found != steps->second.members.end())
                        other = found->second;
            }
            if (other && other->m_volume && other->m_volume->spacing == m_volume->spacing &&
                other->m_volume->InteriorCells().x == m_volume->InteriorCells().x &&
                other->m_volume->InteriorCells().z == m_volume->InteriorCells().z)
                m_cellVolumes[size_t((dz + 1) * 3 + dx + 1)] = other->m_volume;
        }
    for (auto& chunk : m_chunks)
    {
        Key key;
        for (size_t i = 0; i < key.cells.size(); ++i) key.cells[i] = m_cellVolumes[i].get();
        key.level = LevelAt(chunk.world, camera);
        if (key.level >= 0)
            for (int dy = -1; dy <= 1; ++dy)
                for (int dz = -1; dz <= 1; ++dz)
                    for (int dx = -1; dx <= 1; ++dx)
                        key.strides[size_t((dy + 1) * 9 + (dz + 1) * 3 + dx + 1)] =
                            (dx || dy || dz) ? StrideAt(chunk.world + glm::ivec3(dx, dy, dz), camera) : 1 << key.level;
        chunk.wanted = key;
        if (chunk.ready && chunk.ready->key != key) chunk.ready.reset();
    }
}

void VolumeTerrain::SwapIn(const glm::vec3& eye)
{
    using SwapClock = std::chrono::steady_clock;
    double dropMs = 0.0, createMs = 0.0, storeMs = 0.0, retireMs = 0.0;
    auto* scene = Owner->GetScene();
    auto* provider = scene ? scene->GetGraphicsProvider() : nullptr;
    for (auto& chunk : m_chunks)
    {
        if (chunk.wanted == chunk.shown) continue;
        if (chunk.wanted.level < 0 || !chunk.ready || !chunk.ready->mesh)
        {
            const auto dropStart = SwapClock::now();
            DropChunk(chunk);   // streamed out, or nothing to draw
            dropMs += std::chrono::duration<double, std::milli>(SwapClock::now() - dropStart).count();
            chunk.shown = chunk.wanted;
            chunk.ready.reset();
            continue;
        }
        Built built = std::move(*chunk.ready);
        chunk.ready.reset();
        chunk.triangles = built.triangles;
        const bool fade = chunk.object && chunk.mesh;
        const auto createStart = SwapClock::now();
        if (!chunk.object)
        {
            const auto& i = chunk.index;
            chunk.object = scene->AddObject("Volume chunk " + std::to_string(i.x) + "," +
                std::to_string(i.y) + "," + std::to_string(i.z));
            chunk.object->runtimeOnly = true;
            scene->SetParentLocal(chunk.object, Owner);   // keep the chunk at the cell's origin
            if (!m_layerMaterials.empty())
            {
                auto* appearance = chunk.object->AddComponent<TerrainBatchAppearance>();
                appearance->materials = m_layerMaterials;
                appearance->textureTiling = m_tiling;
                appearance->worldExtent = m_extent;
            }
            chunk.mesh = chunk.object->AddComponent<Mesh>();
            if (provider) chunk.mesh->PrepareGraphicsResources(provider);
        }
        createMs += std::chrono::duration<double, std::milli>(SwapClock::now() - createStart).count();
        const auto storeStart = SwapClock::now();
        // Normally installed when it arrived (Update); a late one here.
        if (!built.installed && provider) InstallBuilt(built, provider);
        auto geometry = std::move(built.mesh);
        // Two slots in turn: the new mesh fades in over the old one.
        if (fade) chunk.mesh->AdvanceRuntimeLodTransition(1.e3f);   // finish the last fade first
        const int slot = fade ? 1 - chunk.slot : 0;
        // The level this replaces goes to the retired queue, released later.
        if (auto replaced = chunk.mesh->TakeRuntimeLod(slot))
            g_retiredMeshes[scene].meshes.push_back(std::move(replaced));
        chunk.mesh->StoreRuntimeLod(slot, std::move(geometry));
        if (!fade || !chunk.mesh->BeginRuntimeLodTransition(slot, lodFadeSeconds))
            chunk.mesh->SelectRuntimeLod(slot);
        chunk.slot = slot;
        storeMs += std::chrono::duration<double, std::milli>(SwapClock::now() - storeStart).count();
        chunk.object->enabled = true;
        chunk.shown = chunk.wanted;
        // The collision copy follows the new mesh (Update rebuilds it) when
        // the chunk leaves level 0. At level 0 a new mesh differs only along
        // its seams (a neighbour's level changed): keep the cooked collider,
        // which was re-cooked on every such swap (post-physics p99 0.3 -> 7 ms).
        if (chunk.collision && chunk.shown.level != 0)
        {
            scene->RequestRemoveObject(chunk.collision);
            chunk.collision = nullptr;
        }
    }
    (void)eye;
    // A replaced volume's chunks go; (fromTerrain) the terrain's mesh hands over.
    const auto retireStart = SwapClock::now();
    for (auto& chunk : m_retiring) DropChunk(chunk);
    m_retiring.clear();
    retireMs = std::chrono::duration<double, std::milli>(SwapClock::now() - retireStart).count();
    static const bool logSwap = std::getenv("ENGINE_VOLUME_TERRAIN_LOG") != nullptr;
    if (logSwap && dropMs + createMs + storeMs + retireMs > 1.0)
        std::fprintf(stderr, "[VolumeSwap] drop %.1f create %.1f store %.1f retire %.1f ms\n", dropMs, createMs, storeMs, retireMs);
    if (fromTerrain && !m_terrainHidden)
        {
            // The cell is drawn by its chunks from now on (a Terrain has no
            // height mesh; hide any other mesh on the object).
            if (auto* mesh = Owner->GetComponent<Mesh>()) mesh->SetAnimationVisible(false);
            m_terrainHidden = true;
            // ENGINE_VOLUME_TERRAIN_LOG=1: when each cell handed over.
            static const bool log = std::getenv("ENGINE_VOLUME_TERRAIN_LOG") != nullptr;
            if (log)
            {
                size_t triangles = 0;
                for (const auto& chunk : m_chunks)
                    if (chunk.shown.level >= 0) triangles += chunk.triangles;
                std::printf("[VolumeTerrain] %s ready after %.1f s: %zu chunks, %zu triangles, %.1f MB density; "
                    "paint so far %.1f ms over %llu vertices\n",
                    Owner->name.c_str(), std::chrono::duration<double>(
                        std::chrono::steady_clock::now() - m_buildStarted).count(),
                    m_chunks.size(), triangles, m_volume->MemoryBytes() / 1.e6,
                    g_paintMicroseconds.load() / 1000.0, static_cast<unsigned long long>(g_paintVertices.load()));
            }
        }
}

void VolumeTerrain::InstallBuilt(Built& built, Engine::Graphics::IGraphicsProvider* provider)
{
    if (built.installed || !built.mesh || !provider) return;
    static const bool logInstalls = std::getenv("ENGINE_VOLUME_TERRAIN_LOG") != nullptr;
    const auto installStart = std::chrono::steady_clock::now();
    const size_t vertexCount = built.mesh->GetVertices().size(), indexCount = built.mesh->GetIndices().size();
    const bool onWorker = static_cast<bool>(built.vertexBuffer);
    auto* factory = provider->GetBufferFactory();
    const auto& vertices = built.mesh->GetVertices();
    const auto& indices = built.mesh->GetIndices();
    std::shared_ptr<Engine::Graphics::IGraphicsBuffer> vertexBuffer = std::move(built.vertexBuffer);
    if (!vertexBuffer)
        vertexBuffer = factory->CreateStaticVertexBufferDeferred(vertices.size() * sizeof(Mesh::Vertex), vertices.data());
    std::shared_ptr<Engine::Graphics::IGraphicsBuffer> indexBuffer = factory->CreateBuffer(
        Engine::Graphics::IGraphicsBuffer::Usage::IndexBuffer, Engine::Graphics::IGraphicsBuffer::AccessMode::Upload,
        indices.size() * sizeof(uint32_t), indices.data(), sizeof(uint32_t));
    built.mesh->InstallOwnBuffers(std::move(vertexBuffer), std::move(indexBuffer), factory);
    built.installed = true;
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - installStart).count();
    if (logInstalls && ms > .5)
        std::fprintf(stderr, "[VolumeInstall] %.2f ms, %zu vertices, %zu indices, vertex buffer from worker %d\n",
            ms, vertexCount, indexCount, onWorker ? 1 : 0);
}

void VolumeTerrain::AdvanceEditorFrame() { ++g_editorFrame; }

void VolumeTerrain::Update()
{
    using UpdateClock = std::chrono::steady_clock;
    const auto updateStart = UpdateClock::now();
    double installMs = 0.0, swapMs = 0.0, collisionMs = 0.0;
    double arrivalMs = 0.0, uploadMs = 0.0, keysMs = 0.0, launchMs = 0.0;
    int launched = 0;
    int collisionsBuilt = 0;
    auto* scene = Owner ? Owner->GetScene() : nullptr;
    if (scene && fromTerrain && m_terrainSurfaceRevision)
        if (const auto* terrain = Owner->GetComponent<Terrain>();
            terrain && terrain->GetSurfaceRevision() != m_terrainSurfaceRevision)
        {
            // The heights or paint changed (brushes, seam stitching).
            m_terrainSurfaceRevision = terrain->GetSurfaceRevision();
            Refresh();
        }
    if (scene && fromTerrain)
    {
        if (m_volumeJob.valid() && m_volumeJob.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
        {
            auto built = m_volumeJob.get();
            // Stale (the input changed while it was built): build again.
            if (!m_rebuildPending && built)
            {
                // A replacement: the current chunks draw until the new ones
                // swap in.
                for (auto& chunk : m_chunks)
                {
                    if (chunk.job.valid()) chunk.job.wait();
                    m_retiring.push_back(std::move(chunk));
                }
                m_chunks.clear();
                m_volume = std::move(built);
                const auto installStart = UpdateClock::now();
                InstallVolume();
                installMs = std::chrono::duration<double, std::milli>(UpdateClock::now() - installStart).count();
            }
        }
        if (!m_volumeJob.valid() && m_rebuildPending) m_rebuildPending = !StartBuild();
    }
    if (!scene || !m_volume) return;
    // The camera, in the world and in the volume's space.
    glm::dvec3 eyeWorld = glm::dvec3(scene->editorCamera.transform.GetWorldPosition());
    if (scene->HasStarted())
        if (auto* camera = scene->FindGameCamera(); camera && camera->Owner)
            eyeWorld = camera->Owner->transform.GetWorldPositionD();
    const glm::vec3 eye = glm::vec3(glm::inverse(Owner->transform.GetWorldMatrixD()) * glm::dvec4(eyeWorld, 1.0));
    // Edit mode has no game clock (its delta is 0, so fades never ended):
    // the time since this volume's last update instead.
    float delta = std::clamp(scene->GetDeltaTime(), 0.f, .1f);
    if (!scene->HasStarted())
    {
        const auto now = UpdateClock::now();
        delta = m_editorTick.time_since_epoch().count() == 0 ? 0.f
            : std::clamp(std::chrono::duration<float>(now - m_editorTick).count(), 0.f, .1f);
        m_editorTick = now;
    }

    auto& steps = g_steps[scene];
    Advance(steps, FrameOf(*scene), eyeWorld, lodStepMetres);
    ReleaseRetiredMeshes(scene, FrameOf(*scene));
    const auto self = steps.members.find({group ? group : Owner, cellX, cellZ});
    const bool member = self != steps.members.end() && self->second == this;

    // Finished meshing jobs: kept if still wanted.
    const auto arrivalStart = UpdateClock::now();
    for (auto& chunk : m_chunks)
        if (chunk.meshing && chunk.job.valid() &&
            chunk.job.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
        {
            chunk.meshing = false;
            Built built = chunk.job.get();
            if (built.key == chunk.wanted && chunk.wanted != chunk.shown) chunk.ready = std::move(built);
        }
    arrivalMs = std::chrono::duration<double, std::milli>(UpdateClock::now() - arrivalStart).count();
    if (member && m_keysStep != steps.step)
    {
        const auto keysStart = UpdateClock::now();
        ChooseKeys(steps.camera);
        m_keysStep = steps.step;
        keysMs = std::chrono::duration<double, std::milli>(UpdateClock::now() - keysStart).count();
    }
    if (member && steps.swap && m_keysStep == steps.shown)
    {
        const auto swapStart = UpdateClock::now();
        SwapIn(eye);
        swapMs = std::chrono::duration<double, std::milli>(UpdateClock::now() - swapStart).count();
    }

    // Finished chunks' GPU buffers, nearest first, under the shared budget,
    // so the swap never installs them.
    if (auto* provider = scene->GetGraphicsProvider())
    {
        auto& budget = g_installBudgets[scene];
        if (budget.frame != FrameOf(*scene))
            budget = {FrameOf(*scene), 0.0, 0};
        std::vector<std::pair<float, size_t>> arrived;
        for (size_t i = 0; i < m_chunks.size(); ++i)
            if (m_chunks[i].ready && !m_chunks[i].ready->installed && m_chunks[i].ready->mesh)
                arrived.emplace_back(DistanceToBox(eye, m_chunks[i].minimum, m_chunks[i].maximum), i);
        std::sort(arrived.begin(), arrived.end());
        for (const auto& [distance, i] : arrived)
        {
            if (budget.installs > 0 && budget.usedMs >= InstallBudgetMs()) break;
            const auto installStart = UpdateClock::now();
            InstallBuilt(*m_chunks[i].ready, provider);
            const double spent = std::chrono::duration<double, std::milli>(UpdateClock::now() - installStart).count();
            budget.usedMs += spent;
            uploadMs += spent;
            ++budget.installs;
        }
    }

    // Mesh what the step being prepared needs, nearest first.
    bool prepared = member && m_keysStep == steps.step;
    std::vector<std::pair<float, size_t>> wanting;
    for (size_t i = 0; i < m_chunks.size(); ++i)
    {
        const auto& chunk = m_chunks[i];
        if (chunk.wanted == chunk.shown || chunk.wanted.level < 0) continue;
        if (chunk.ready && chunk.ready->key == chunk.wanted)
        {
            // Meshed; prepared once its buffers are in.
            if (!chunk.ready->installed && chunk.ready->mesh) prepared = false;
            continue;
        }
        prepared = false;
        if (!chunk.meshing) wanting.emplace_back(DistanceToBox(eye, chunk.minimum, chunk.maximum), i);
    }
    m_preparedStep = prepared ? steps.step : 0;
    std::sort(wanting.begin(), wanting.end());
    const auto launchStart = UpdateClock::now();
    const int cells = std::clamp(chunkCells, 4, 256);
    for (const auto& [distance, i] : wanting)
    {
        if (g_meshJobs.fetch_add(1) >= MaxMeshJobs()) { g_meshJobs.fetch_sub(1); break; }
        auto& chunk = m_chunks[i];
        const Key key = chunk.wanted;
        const int stride = 1 << key.level;
        // Chunks tile exactly; the seams join levels (SurfaceNetsSeams).
        const glm::ivec3 first = m_volume->interiorFirst + chunk.index * cells;
        const glm::ivec3 span(std::max(cells / stride, 1));
        SurfaceNetsSeams seams;
        seams.strides = key.strides;
        seams.alignment = m_volume->interiorFirst;
        seams.cellSamples = glm::ivec2(m_volume->InteriorCells().x, m_volume->InteriorCells().z);
        // Edges toward cells that are not loaded end on distant terrain:
        // skirted, as the distant nodes' own edges are (TerrainManagerFarVolume).
        seams.skirt = 2.f * m_volume->spacing * float(stride) + 2.f;
        auto cellVolumes = m_cellVolumes;   // held by the job
        const float start = steepStart, end = steepEnd, extent = m_extent;
        const bool weights = !m_layerMaterials.empty();
        const int steep = steepLayer;
        const RockSurface rock = m_rock;
        auto* provider = scene->GetGraphicsProvider();
        auto* bufferFactory = provider ? provider->GetBufferFactory() : nullptr;
        const bool workerBuffer = bufferFactory &&
            bufferFactory->SupportsWorkerThreadStaticVertexBufferCreation();
        chunk.job = std::async(std::launch::async,
            [volume = m_volume, paint = m_paint, first, span, stride, seams, key, start, end, extent,
             weights, steep, rock, cellVolumes, bufferFactory, workerBuffer]() mutable
            {
                JobSlot slot{&g_meshJobs};
                for (size_t i = 0; i < cellVolumes.size(); ++i)
                    seams.cellVolumes[i] = i == 4 ? nullptr : cellVolumes[i].get();
                Built built;
                built.key = key;
                std::vector<Mesh::Vertex> vertices;
                std::vector<uint32_t> indices;
                MeshSurfaceNets(*volume, first, span, stride, seams, vertices, indices);
                if (paint)
                {
                    const auto paintStart = std::chrono::steady_clock::now();
                    ApplyPaintWeights(vertices, *paint, steep, start, end, rock);
                    g_paintMicroseconds += uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - paintStart).count());
                    g_paintVertices += vertices.size();
                }
                else if (weights) ApplySlopeWeights(vertices, start, end, extent);
                if (indices.empty()) return built;
                const auto bounds = Mesh::PrepareIndexedGeometry(vertices, indices);
                if (!bounds) return built;
                built.triangles = indices.size() / 3;
                auto mesh = std::make_unique<Mesh>();
                if (!mesh->SetPreparedIndexedGeometry(std::move(vertices), std::move(indices), *bounds))
                    return built;
                // Each chunk is unique: its own buffers, not the shared
                // content cache (see Mesh::InstallOwnBuffers).
                const auto& meshVertices = mesh->GetVertices();
                if (workerBuffer)
                    built.vertexBuffer = bufferFactory->CreateStaticVertexBuffer(
                        meshVertices.size() * sizeof(Mesh::Vertex), meshVertices.data());
                built.mesh = std::move(mesh);
                return built;
            });
        chunk.jobKey = key;
        chunk.meshing = true;
        ++launched;
    }
    launchMs = std::chrono::duration<double, std::milli>(UpdateClock::now() - launchStart).count();

    // Fades, collision near the camera, stats.
    m_stats = {};
    for (auto& chunk : m_chunks)
    {
        if (chunk.mesh) chunk.mesh->AdvanceRuntimeLodTransition(delta);
        const float distance = DistanceToBox(eye, chunk.minimum, chunk.maximum);
        if (chunk.collision && (distance > collisionDistance * 1.5f || chunk.shown.level != 0))
        {
            scene->RequestRemoveObject(chunk.collision);
            chunk.collision = nullptr;
        }
        else if (!chunk.collision && chunk.shown.level == 0 && chunk.mesh && chunk.triangles &&
            distance <= collisionDistance)
            if (const Mesh* shown = chunk.mesh->GetRuntimeLodMesh(chunk.slot))
            {
                const auto collisionStart = UpdateClock::now();
                ++collisionsBuilt;
                const auto& i = chunk.index;
                chunk.collision = scene->AddObject("Volume collision " + std::to_string(i.x) + "," +
                    std::to_string(i.y) + "," + std::to_string(i.z));
                chunk.collision->runtimeOnly = true;
                scene->SetParentLocal(chunk.collision, Owner);
                auto* mesh = chunk.collision->AddComponent<Mesh>();
                mesh->SetIndexedGeometry(shown->GetVertices(), shown->GetIndices());
                mesh->SetAnimationVisible(false);   // collision only, never drawn
                auto* collider = chunk.collision->AddComponent<MeshObjectCollider>();
                collider->convex = false;
                auto* body = chunk.collision->AddComponent<RigidBody>();
                body->bodyType = "Static";
                body->useGravity = false;
                body->friction = .9f;
                body->restitution = 0.f;
                collisionMs += std::chrono::duration<double, std::milli>(UpdateClock::now() - collisionStart).count();
            }
        if (chunk.object && chunk.shown.level >= 0)
        {
            ++m_stats.chunksShown;
            m_stats.triangles += chunk.triangles;
            ++m_stats.chunksAtLevel[chunk.shown.level];
        }
        m_stats.colliders += chunk.collision != nullptr;
        m_stats.chunksMeshing += chunk.meshing;
    }
    // ENGINE_VOLUME_TERRAIN_LOG=1: updates that took a while, and on what.
    static const bool logSlow = std::getenv("ENGINE_VOLUME_TERRAIN_LOG") != nullptr;
    const double totalMs = std::chrono::duration<double, std::milli>(UpdateClock::now() - updateStart).count();
    if (logSlow && scene)
    {
        // All volumes of a frame together.
        static std::map<const void*, std::tuple<uint64_t, double, int>> frames;
        auto& [frame, sum, count] = frames[scene];
        if (frame != scene->GetFrameIndex())
        {
            if (sum > 3.0)
                std::printf("[VolumeTerrain] frame %llu all volumes %.1f ms over %d updates\n",
                    static_cast<unsigned long long>(frame), sum, count);
            frame = scene->GetFrameIndex(); sum = 0.0; count = 0;
        }
        sum += totalMs; ++count;
    }
    if (logSlow && totalMs > 1.0 && Owner)
        std::printf("[VolumeTerrain] %s frame %llu update %.1f ms: install %.1f, swap %.1f, collision %.1f (%d built), arrivals %.1f, uploads %.1f, keys %.1f, launch %.1f (%d jobs)\n",
            Owner->name.c_str(), static_cast<unsigned long long>(scene ? scene->GetFrameIndex() : 0),
            totalMs, installMs, swapMs, collisionMs, collisionsBuilt, arrivalMs, uploadMs, keysMs, launchMs, launched);
}


namespace
{
// ENGINE_VOLUME_TERRAIN_DUMP=<folder>: the first check that finds a hole
// writes the cells around it there (their volumes as v2 .volume files and
// each chunk's shown level and seams), so it can be meshed again offline.
void DumpVolume(const std::string& path, const TerrainVolume& v)
{
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    const auto write = [&](const auto& value) { file.write(reinterpret_cast<const char*>(&value), sizeof(value)); };
    write(uint64_t(0x314c4f56524e5254ull));
    write(uint32_t(2));
    write(int32_t(v.sizeX)); write(int32_t(v.sizeY)); write(int32_t(v.sizeZ));
    write(v.spacing); write(v.origin.x); write(v.origin.y); write(v.origin.z); write(v.unitsPerMetre);
    write(uint32_t(v.chunkSamples));
    for (const auto& chunk : v.chunks)
    {
        if (chunk.samples.empty()) { write(uint8_t(0)); write(chunk.uniform); continue; }
        write(uint8_t(1));
        file.write(reinterpret_cast<const char*>(chunk.samples.data()), chunk.samples.size() * sizeof(int16_t));
    }
}
}

void VolumeTerrain::CheckLayer(const std::vector<VolumeTerrain*>& volumes)
{
    // Every shown triangle of every volume, in world space relative to the
    // first volume (doubles, then floats near the origin).
    if (volumes.empty() || !volumes.front()->Owner) return;
    const auto started = std::chrono::steady_clock::now();
    const glm::dvec3 base = volumes.front()->Owner->transform.GetWorldPositionD();
    std::vector<glm::vec3> corners;
    struct Area { glm::vec2 low, high; const VolumeTerrain* volume; };
    std::vector<Area> areas;
    for (const auto* volume : volumes)
    {
        if (!volume->m_volume || !volume->Owner) continue;
        const glm::dmat4 world = volume->Owner->transform.GetWorldMatrixD();
        const auto& v = *volume->m_volume;
        const glm::vec3 low = v.origin + glm::vec3(v.interiorFirst) * v.spacing;
        const glm::vec3 high = low + glm::vec3(v.InteriorCells()) * v.spacing;
        const glm::dvec3 a = glm::dvec3(world * glm::dvec4(glm::dvec3(low), 1.0)) - base;
        const glm::dvec3 b = glm::dvec3(world * glm::dvec4(glm::dvec3(high), 1.0)) - base;
        areas.push_back({glm::vec2(std::min(a.x, b.x), std::min(a.z, b.z)),
            glm::vec2(std::max(a.x, b.x), std::max(a.z, b.z)), volume});
        for (const auto& chunk : volume->m_chunks)
        {
            if (!chunk.mesh || chunk.shown.level < 0 || !chunk.object || !chunk.object->enabled) continue;
            const Mesh* mesh = chunk.mesh->GetRuntimeLodMesh(chunk.slot);
            if (!mesh) continue;
            const auto& vertices = mesh->GetVertices();
            for (uint32_t index : mesh->GetIndices())
            {
                const auto& p = vertices[index].pos;
                corners.push_back(glm::vec3(glm::dvec3(world * glm::dvec4(p[0], p[1], p[2], 1.0)) - base));
            }
        }
    }
    if (corners.empty() || areas.empty()) return;
    glm::vec2 low(1e30f), high(-1e30f);
    for (const auto& area : areas) { low = glm::min(low, area.low); high = glm::max(high, area.high); }
    // Triangles bucketed by x, z bounds (2 m buckets).
    constexpr float Bucket = 2.f;
    const int across = int(std::ceil((high.x - low.x) / Bucket)) + 1, deep = int(std::ceil((high.y - low.y) / Bucket)) + 1;
    std::vector<std::vector<uint32_t>> buckets(size_t(across) * deep);
    const auto bucketOf = [&](float x, float z) {
        return size_t(std::clamp(int((z - low.y) / Bucket), 0, deep - 1)) * across +
            size_t(std::clamp(int((x - low.x) / Bucket), 0, across - 1));
    };
    for (uint32_t t = 0; t + 2 < corners.size(); t += 3)
    {
        const glm::vec3 a = glm::min(corners[t], glm::min(corners[t + 1], corners[t + 2]));
        const glm::vec3 b = glm::max(corners[t], glm::max(corners[t + 1], corners[t + 2]));
        for (float z = std::floor((a.z - low.y) / Bucket) * Bucket + low.y; z <= b.z; z += Bucket)
            for (float x = std::floor((a.x - low.x) / Bucket) * Bucket + low.x; x <= b.x; x += Bucket)
                buckets[bucketOf(x + .5f * Bucket, z + .5f * Bucket)].push_back(t);
    }
    const auto areaName = [&](float x, float z) {
        for (const auto& area : areas)
            if (x >= area.low.x && x <= area.high.x && z >= area.low.y && z <= area.high.y)
                return area.volume->Owner->name;
        return std::string("?");
    };
    // Columns every metre, off the grid's lines, inside the cells (not
    // within a metre of the outer edge of them all).
    size_t columns = 0, holes = 0, doubled = 0, otherEven = 0, printed = 0;
    glm::vec2 firstHole(1e30f);
    for (const auto& area : areas)
        for (float z = area.low.y + .5137f; z < area.high.y; z += 1.f)
            for (float x = area.low.x + .5211f; x < area.high.x; x += 1.f)
            {
                if (x < low.x + 1.f || x > high.x - 1.f || z < low.y + 1.f || z > high.y - 1.f) continue;
                std::vector<float> hits;
                for (uint32_t t : buckets[bucketOf(x, z)])
                {
                    const glm::vec3 &a = corners[t], &b = corners[t + 1], &c = corners[t + 2];
                    const float area2 = (b.x - a.x) * (c.z - a.z) - (c.x - a.x) * (b.z - a.z);
                    if (std::abs(area2) < 1e-9f) continue;
                    const float u = ((b.x - x) * (c.z - z) - (c.x - x) * (b.z - z)) / area2;
                    const float v = ((c.x - x) * (a.z - z) - (a.x - x) * (c.z - z)) / area2;
                    const float w = 1.f - u - v;
                    if (u < 0.f || v < 0.f || w < 0.f) continue;
                    hits.push_back(a.y * u + b.y * v + c.y * w);
                }
                ++columns;
                if (hits.size() % 2 == 1) continue;
                std::sort(hits.begin(), hits.end());
                bool close = false;
                for (size_t i = 1; i < hits.size(); ++i) close = close || hits[i] - hits[i - 1] < 1.f;
                const char* kind = hits.empty() ? "hole" : close ? "doubled" : "even";
                if (hits.empty() && firstHole.x > 1e29f) firstHole = glm::vec2(x, z);
                ++(hits.empty() ? holes : close ? doubled : otherEven);
                if (printed++ < 30)
                {
                    std::printf("[VolumeTerrain check]   %s at %.1f, %.1f (world %.1f, %.1f) in %s, %zu crossings\n",
                        kind, x, z, x + base.x, z + base.z, areaName(x, z).c_str(), hits.size());
                    // The first few: does the density have a surface there,
                    // and what do the column's chunks show?
                    if (printed <= 4)
                        for (const auto& owner : areas)
                        {
                            if (x < owner.low.x || x > owner.high.x || z < owner.low.y || z > owner.high.y) continue;
                            const auto* volume = owner.volume;
                            const glm::dmat4 inverse = glm::inverse(volume->Owner->transform.GetWorldMatrixD());
                            const glm::vec3 local(inverse * glm::dvec4(base + glm::dvec3(x, 0.0, z), 1.0));
                            const auto& v = *volume->m_volume;
                            int changes = 0;
                            float last = v.Sample(glm::vec3(local.x, v.origin.y, local.z));
                            for (int j = 1; j < v.sizeY; ++j)
                            {
                                const float d = v.Sample(glm::vec3(local.x, v.origin.y + j * v.spacing, local.z));
                                changes += (d > 0.f) != (last > 0.f);
                                last = d;
                            }
                            std::printf("[VolumeTerrain check]     density: %d sign changes over y %.0f..%.0f\n",
                                changes, v.origin.y, v.origin.y + (v.sizeY - 1) * v.spacing);
                            for (const auto& chunk : volume->m_chunks)
                                if (local.x >= chunk.minimum.x && local.x <= chunk.maximum.x &&
                                    local.z >= chunk.minimum.z && local.z <= chunk.maximum.z)
                                    std::printf("[VolumeTerrain check]     chunk %d,%d,%d (world %d,%d,%d): shown %d, "
                                        "wanted %d, %zu triangles, object %s, meshing %d, ready %d\n",
                                        chunk.index.x, chunk.index.y, chunk.index.z, chunk.world.x, chunk.world.y,
                                        chunk.world.z, chunk.shown.level, chunk.wanted.level, chunk.triangles,
                                        chunk.object ? (chunk.object->enabled ? "on" : "off") : "none",
                                        int(chunk.meshing), int(chunk.ready.has_value()));
                        }
                }
            }
    static bool dumped = false;
    const char* dumpFolder = std::getenv("ENGINE_VOLUME_TERRAIN_DUMP");
    // ENGINE_VOLUME_TERRAIN_DUMP_HOLES: only a check with at least this many.
    const char* dumpHoles = std::getenv("ENGINE_VOLUME_TERRAIN_DUMP_HOLES");
    if (dumpFolder && !dumped && firstHole.x < 1e29f && holes >= size_t(dumpHoles ? std::atoi(dumpHoles) : 1))
    {
        dumped = true;
        std::error_code error;
        std::filesystem::create_directories(dumpFolder, error);
        const glm::dvec3 hole = base + glm::dvec3(firstHole.x, 0.0, firstHole.y);
        std::ofstream holes(std::string(dumpFolder) + "/hole.txt");
        holes << hole.x << ' ' << hole.z << '\n';
        for (const auto* volume : volumes)
        {
            const glm::dvec3 at = volume->Owner->transform.GetWorldPositionD();
            const auto& v = *volume->m_volume;
            const double span = v.InteriorCells().x * double(v.spacing);
            if (hole.x < at.x - span || hole.x > at.x + 2 * span || hole.z < at.z - span || hole.z > at.z + 2 * span)
                continue;
            const std::string name = std::string(dumpFolder) + "/cell_" + std::to_string(volume->cellX) + "_" +
                std::to_string(volume->cellZ);
            DumpVolume(name + ".volume", v);
            std::ofstream chunks(name + ".chunks");
            const glm::ivec3 cells = v.InteriorCells();
            chunks << v.interiorFirst.x << ' ' << v.interiorFirst.y << ' ' << v.interiorFirst.z << ' '
                   << cells.x << ' ' << cells.y << ' ' << cells.z << ' ' << volume->cellX << ' ' << volume->cellZ << ' '
                   << at.x << ' ' << at.y << ' ' << at.z << ' ' << volume->chunkCells << '\n';
            for (const auto& chunk : volume->m_chunks)
            {
                chunks << chunk.index.x << ' ' << chunk.index.y << ' ' << chunk.index.z << ' ' << chunk.shown.level;
                for (int stride : chunk.shown.strides) chunks << ' ' << stride;
                chunks << '\n';
            }
        }
        std::printf("[VolumeTerrain check] dumped the cells around %.1f, %.1f to %s\n", hole.x, hole.z, dumpFolder);
    }
    // Edges: shared by exactly two triangles; open ones are sorted by where
    // they lie (cell borders between volumes, the outer edge, inside a cell).
    std::unordered_map<uint64_t, int> uses;
    const auto key = [&](const glm::vec3& p) {
        const auto q = glm::ivec3(glm::round((p - glm::vec3(low.x, -4096.f, low.y)) * 64.f));
        return (uint64_t(uint32_t(q.x) & 0x3fffff) << 42) | (uint64_t(uint32_t(q.y) & 0x1fffff) << 21) |
            uint64_t(uint32_t(q.z) & 0x1fffff);
    };
    std::unordered_map<uint64_t, glm::vec3> where;
    for (size_t t = 0; t + 2 < corners.size(); t += 3)
        for (int e = 0; e < 3; ++e)
        {
            uint64_t a = key(corners[t + e]), b = key(corners[t + (e + 1) % 3]);
            if (a == b) continue;
            if (b < a) std::swap(a, b);
            const uint64_t edge = a * 0x9E3779B97F4A7C15ull ^ b;
            if (++uses[edge] == 1) where[edge] = (corners[t + e] + corners[t + (e + 1) % 3]) * .5f;
        }
    size_t openInside = 0, openBorder = 0, openOuter = 0, overused = 0, edgePrinted = 0;
    for (const auto& [edge, count] : uses)
    {
        if (count > 2) { ++overused; continue; }
        if (count != 1) continue;
        const glm::vec3 m = where[edge];
        if (m.x < low.x + .05f || m.x > high.x - .05f || m.z < low.y + .05f || m.z > high.y - .05f) { ++openOuter; continue; }
        bool border = false;
        for (const auto& area : areas)
            border = border || std::abs(m.x - area.low.x) < .05f || std::abs(m.x - area.high.x) < .05f ||
                std::abs(m.z - area.low.y) < .05f || std::abs(m.z - area.high.y) < .05f;
        ++(border ? openBorder : openInside);
        if (edgePrinted++ < 20)
            std::printf("[VolumeTerrain check]   open edge (%s) at %.1f, %.1f, %.1f (world %.1f, %.1f) in %s\n",
                border ? "cell border" : "inside", m.x, m.y, m.z, m.x + base.x, m.z + base.z, areaName(m.x, m.z).c_str());
    }
    std::printf("[VolumeTerrain check] %zu volumes, %zu triangles: columns %zu, holes %zu, doubled %zu, other even %zu; "
        "open edges inside %zu, on cell borders %zu, outer %zu; edges in 3+ triangles %zu (%.0f ms)\n",
        volumes.size(), corners.size() / 3, columns, holes, doubled, otherEven, openInside, openBorder, openOuter,
        overused, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count());
    std::fflush(stdout);
}
}

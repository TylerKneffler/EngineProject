#include "TerrainManager.h"
#include "Core/Components/Camera/Camera.h"
#include "Core/Graphics/IGraphicsProvider.h"
#include "Core/Components/Materials/Material.h"
#include "Core/Components/Mesh/Mesh.h"
#include "Core/Components/Terrain/TerrainBatchAppearance.h"
#include "Core/Components/Terrain/TerrainVolume.h"
#include "Core/Components/Terrain/TerrainVolumeBuild.h"
#include "Core/Components/Terrain/VolumeTerrain.h"
#include "Core/Rendering/DetailBudget.h"
#include "Core/Scene/Object.h"
#include "Core/Scene/Scene.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>

// Distant terrain as volume (todo.md "Terrain: volume-first conversion",
// step 7): a quadtree of square nodes, 1 to 64 cells across, each meshed by
// surface nets from the terrain's density on a 32 x 32 grid (4 m apart for a
// one-cell node, 256 m for the largest). A node splits while the viewer is
// within `farVolumeDetail` of its own size, and nodes over loaded cells split
// down to single cells; a single loaded cell whose volume shows is left out,
// so the far nodes and the loaded volume cells tile the ground exactly.
//
// The density is the field's: seed heights (edited cells' heights, else the
// height archive's full detail, else the 128 m corner grid for nodes that
// coarse) and the volume edits of the cells a node reaches. Nodes join
// their neighbours with a skirt under each edge. They are coloured with the
// far palette (macro colour grid or height bands, rock on steep faces) and
// carry the 128 m corner grid as their occluder (the CPU occlusion buffer).
//
// The wanted set is chosen from the viewer's position in steps of
// `farVolumeStepMetres` (or when loaded cells or edits change); its nodes are
// built on worker threads, and the whole set swaps in once every node of it
// is ready, so the ground never opens while nodes are built.
namespace Engine::Components
{
namespace
{
constexpr int NodeGrid = 32;      // surface-nets cells across a node
constexpr int TopNodeCells = 64;  // largest node, in terrain cells
constexpr int MaxFarJobs = 4;
std::atomic<int> g_farJobs{0};

struct FarJobSlot
{
    ~FarJobSlot() { g_farJobs.fetch_sub(1); }
};

bool FarVolumeLog()
{
    static const bool log = std::getenv("ENGINE_FAR_VOLUME_LOG") != nullptr;
    return log;
}
}

// Worker input for one node (copies and shared snapshots only).
struct TerrainManager::FarVolumeJobInput
{
    FarVolumeKey key;
    int cellsX = 32, cellsY = 32;
    float spanX = 128.f, spanZ = 128.f;
    int halfExtent = 0;
    std::shared_ptr<const std::vector<float>> grid;   // corner heights, (2h+2)^2
    std::shared_ptr<const FarCellDetail::Archive> archive;
    std::shared_ptr<const std::map<Cell, std::vector<float>>> patches;
    std::shared_ptr<const FarTerrainPalette> palette;
    std::vector<TerrainVolumeEdit> edits;             // node space
};

std::shared_ptr<TerrainManager::FarVolumeMesh> TerrainManager::BuildFarVolumeNode(
    const FarVolumeJobInput& in)
{
    auto result = std::make_shared<FarVolumeMesh>();
    const auto start = std::chrono::steady_clock::now();
    const int side = in.halfExtent * 2 + 2;
    if (!in.grid || in.grid->size() != static_cast<size_t>(side) * side) return result;
    const float nodeMetres = in.key.size * in.spanX;
    const float spacing = nodeMetres / NodeGrid;
    const double originX = static_cast<double>(in.key.x) * in.spanX;
    const double originZ = static_cast<double>(in.key.z) * in.spanZ;

    // Corner grid (cells' corners, 128 m), bilinear between them.
    const auto corner = [&](int x, int y) {
        x = std::clamp(x + in.halfExtent, 0, side - 1);
        y = std::clamp(y + in.halfExtent, 0, side - 1);
        return (*in.grid)[static_cast<size_t>(y) * side + x];
    };
    const auto gridHeight = [&](double x, double z) {
        const double gx = x / in.spanX, gz = z / in.spanZ;
        const int x0 = static_cast<int>(std::floor(gx)), z0 = static_cast<int>(std::floor(gz));
        const float tx = static_cast<float>(gx - x0), tz = static_cast<float>(gz - z0);
        const float a = corner(x0, z0), b = corner(x0 + 1, z0);
        const float c = corner(x0, z0 + 1), d = corner(x0 + 1, z0 + 1);
        return (a + (b - a) * tx) * (1.f - tz) + (c + (d - c) * tx) * tz;
    };
    // Full-detail cells, decoded once per node.
    std::map<Cell, std::vector<float>> decoded;
    const bool fine = spacing < in.spanX;
    const auto cellHeights = [&](Cell cell) -> const std::vector<float>* {
        if (in.patches)
            if (const auto found = in.patches->find(cell); found != in.patches->end())
                return &found->second;
        if (!fine || !in.archive) return nullptr;
        auto [it, inserted] = decoded.try_emplace(cell);
        if (inserted && !in.archive->ReadCell(cell.first, cell.second, it->second)) it->second.clear();
        return it->second.empty() ? nullptr : &it->second;
    };
    const float sampleX = in.spanX / in.cellsX, sampleZ = in.spanZ / in.cellsY;
    const auto height = [&](double x, double z) {
        const Cell cell{static_cast<int>(std::floor(x / in.spanX)), static_cast<int>(std::floor(z / in.spanZ))};
        if (const auto* heights = cellHeights(cell))
        {
            const double lx = x - cell.first * static_cast<double>(in.spanX);
            const double lz = z - cell.second * static_cast<double>(in.spanZ);
            const float fx = std::clamp(static_cast<float>(lx / sampleX), 0.f, float(in.cellsX));
            const float fz = std::clamp(static_cast<float>(lz / sampleZ), 0.f, float(in.cellsY));
            const int c0 = std::min(static_cast<int>(fx), in.cellsX - 1);
            const int r0 = std::min(static_cast<int>(fz), in.cellsY - 1);
            const float tx = fx - c0, tz = fz - r0;
            const auto at = [&](int c, int r) { return (*heights)[static_cast<size_t>(r) * (in.cellsX + 1) + c]; };
            return (at(c0, r0) + (at(c0 + 1, r0) - at(c0, r0)) * tx) * (1.f - tz) +
                (at(c0, r0 + 1) + (at(c0 + 1, r0 + 1) - at(c0, r0 + 1)) * tx) * tz;
        }
        return gridHeight(x, z);
    };

    // The node and its eight neighbours' footprints as one input (the
    // volume reads past the node's edges, its margin).
    TerrainVolumeInput input;
    input.cellsX = input.cellsY = NodeGrid;
    input.cellWidth = input.cellDepth = spacing;
    const auto fill = [&](int dx, int dz, std::vector<float>& heights) {
        heights.resize(static_cast<size_t>(NodeGrid + 1) * (NodeGrid + 1));
        for (int row = 0; row <= NodeGrid; ++row)
            for (int col = 0; col <= NodeGrid; ++col)
                heights[static_cast<size_t>(row) * (NodeGrid + 1) + col] = height(
                    originX + (dx * NodeGrid + col) * static_cast<double>(spacing),
                    originZ + (dz * NodeGrid + row) * static_cast<double>(spacing));
    };
    fill(0, 0, input.heights);
    for (int dz = -1; dz <= 1; ++dz)
        for (int dx = -1; dx <= 1; ++dx)
            if (dx || dz) fill(dx, dz, input.neighbourHeights[static_cast<size_t>((dz + 1) * 3 + dx + 1)]);
    input.edits = in.edits;
    const float band = std::max(8.f, 2.f * spacing);
    const auto volume = BuildTerrainVolume(input, spacing, band);
    if (!volume) return result;
    std::vector<Mesh::Vertex> surface;
    std::vector<uint32_t> indices;
    // Every side ends on the node's face (no neighbouring block), its border
    // vertices placed from the face's own crossings: a loaded cell beside it
    // meshes its border at this spacing and puts its vertices at the same
    // points. Skirts under the edges cover joins to nodes of other sizes.
    SurfaceNetsSeams seams;
    seams.strides.fill(0);
    seams.strides[13] = 1;
    seams.alignment = volume->interiorFirst;
    seams.skirt = 2.f * spacing;
    MeshSurfaceNets(*volume, volume->interiorFirst, volume->InteriorCells(), 1, seams, surface, indices);

    // Occluder first: the node's 128 m corner grid (two triangles a cell).
    auto& mesh = result->geometry;
    const int cells = in.key.size;
    mesh.vertices.reserve(static_cast<size_t>(cells + 1) * (cells + 1) + surface.size());
    const auto macroUv = [&](double cellX, double cellZ, float* uv) {
        uv[0] = static_cast<float>((cellX + in.halfExtent) / (side - 1));
        uv[1] = static_cast<float>((cellZ + in.halfExtent) / (side - 1));
    };
    for (int row = 0; row <= cells; ++row)
        for (int col = 0; col <= cells; ++col)
        {
            Mesh::TerrainVertex vertex{};
            vertex.pos[0] = col * in.spanX;
            vertex.pos[1] = corner(in.key.x + col, in.key.z + row);
            vertex.pos[2] = row * in.spanZ;
            vertex.normal[1] = 1.f;
            macroUv(in.key.x + col, in.key.z + row, vertex.uv);
            mesh.vertices.push_back(vertex);
        }
    for (int row = 0; row < cells; ++row)
        for (int col = 0; col < cells; ++col)
        {
            const uint32_t a = static_cast<uint32_t>(row * (cells + 1) + col);
            const uint32_t b = a + static_cast<uint32_t>(cells + 1);
            result->occluderIndices.insert(result->occluderIndices.end(), {a, b, a + 1, a + 1, b, b + 1});
        }
    result->occluderVertexCount = mesh.vertices.size();
    const uint32_t base = static_cast<uint32_t>(mesh.vertices.size());
    const auto& palette = *in.palette;
    for (const auto& source : surface)
    {
        Mesh::TerrainVertex vertex{};
        std::copy_n(source.pos, 3, vertex.pos);
        std::copy_n(source.normal, 3, vertex.normal);
        const double cellX = in.key.x + source.pos[0] / static_cast<double>(in.spanX);
        const double cellZ = in.key.z + source.pos[2] / static_cast<double>(in.spanZ);
        macroUv(cellX, cellZ, vertex.uv);
        glm::vec3 color;
        if (!palette.SampleGridColor(static_cast<float>(cellX), static_cast<float>(cellZ), color))
        {
            float weights[6];
            TerrainHeightArchive::BlendHeightBands(source.pos[1], palette.heightBoundaries,
                palette.heightBlendHalfWidth, weights);
            color = glm::vec3(0.f);
            for (int layer = 0; layer < 6; ++layer) color += palette.farColors[layer] * weights[layer];
        }
        vertex.color[0] = color.r; vertex.color[1] = color.g; vertex.color[2] = color.b;
        vertex.color[3] = 1.f;
        palette.ApplySteep(vertex.color, vertex.normal[1]);
        mesh.vertices.push_back(vertex);
    }
    mesh.indices.reserve(indices.size());
    for (const uint32_t index : indices) mesh.indices.push_back(base + index);
    result->buildMilliseconds = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();
    return result;
}

void TerrainManager::UpdateFarTerrain(int centerX, int centerY,
    const Camera* camera, int, float)
{
    using FarClock = std::chrono::steady_clock;
    const auto updateStart = FarClock::now();
    m_lastFarUpdateTiming = {};
    if (!Owner || !m_farEnabled || m_farHeights.empty()) return;
    auto* scene = Owner->GetScene();
    if (!scene) return;
    const int side = m_farHalfExtentCells * 2 + 2;
    if (m_farHeights.size() != static_cast<size_t>(side) * side) return;
    const float spanX = cellsX * cellWidth, spanZ = cellsY * cellDepth;
    if (m_farArchiveJob.valid() &&
        m_farArchiveJob.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
    {
        m_farArchive = m_farArchiveJob.get();
        m_farStepValid = false;   // finer nodes now read the archive
        for (auto& [key, node] : m_farNodes) node.stale = true;
    }

    // The viewer, in the manager's space.
    glm::dvec3 eye(centerX * static_cast<double>(spanX) + spanX * .5, 0.0,
        centerY * static_cast<double>(spanZ) + spanZ * .5);
    if (camera && camera->Owner)
        eye = glm::dvec3(glm::inverse(Owner->transform.GetWorldMatrixD()) *
            glm::dvec4(camera->Owner->transform.GetWorldPositionD(), 1.0));

    // Loaded cells drawn by their volumes, within the loaded radius: a cell
    // past it is about to unload once a far node covers it (StreamCells).
    std::set<Cell> shownCells;
    const int nearRadius = std::clamp(m_editorStreaming ? editorRadius : loadedRadius, 0, 32);
    for (const auto& [cell, terrain] : m_loaded)
        if (std::max(std::abs(cell.first - centerX), std::abs(cell.second - centerY)) <= nearRadius &&
            terrain && terrain->Owner && terrain->Owner->enabled)
            if (const auto* volume = terrain->Owner->GetComponent<VolumeTerrain>();
                volume && volume->IsShowing())
                shownCells.insert(cell);
    const bool loadedChanged = shownCells != m_farShownCells;

    // The loaded cells' layer materials and tiling: the distant path samples
    // them as detail on the macro colour (Object.hlsl, flag 4194304), so
    // distant ground is textured like the near cells. Nodes installed before
    // any cell loaded get them once a cell has.
    if (m_farLayerMaterials.empty())
        for (const auto& [cell, terrain] : m_loaded)
            if (terrain && !terrain->GetLayerMaterials().empty())
            {
                m_farLayerMaterials = terrain->GetLayerMaterials();
                m_farLayerTiling = terrain->textureTiling;
                for (auto& [nodeKey, existing] : m_farNodes)
                    if (existing.object)
                        if (auto* appearance = existing.object->GetComponent<TerrainBatchAppearance>();
                            appearance && appearance->materials.empty())
                        {
                            appearance->materials = m_farLayerMaterials;
                            appearance->textureTiling = m_farLayerTiling;
                        }
                break;
            }
    // Install finished builds (a few a frame).
    const auto installStart = FarClock::now();
    int installs = 0;
    for (auto& [key, node] : m_farNodes)
    {
        if (!node.job.valid() || node.job.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
            continue;
        // A time budget, not a count: 1-4 installs took 3-14 ms on one frame
        // at cell crossings (todo.md Step 0).
        if (installs >= 1 && std::chrono::duration<double, std::milli>(FarClock::now() - installStart).count() >=
            std::max(farVolumeInstallMilliseconds, .1f)) break;
        auto built = node.job.get();
        ++installs;
        if (!built) continue;
        m_lastFarUpdateTiming.workerBuildMs += built->buildMilliseconds;
        if (!node.object)
        {
            node.object = scene->AddObject("Distant terrain " + std::to_string(key.x) + "," +
                std::to_string(key.z) + " x" + std::to_string(key.size));
            node.object->runtimeOnly = true;
            node.object->layer = Engine::Rendering::DetailBudget::Get().FindLayerOrDefault("Terrain");
            scene->MoveObject(node.object, Owner, Engine::Scene::Scene::ObjectPlacement::AsChild);
            node.object->transform.position = {key.x * static_cast<double>(spanX), 0.0,
                key.z * static_cast<double>(spanZ)};
            node.mesh = node.object->AddComponent<Mesh>();
            auto* material = node.object->AddComponent<Material>();
            material->diffuseColor = {1.f, 1.f, 1.f};
            material->unlit = true;
            // Distant hills shade valleys through the far shadow cascade.
            material->castsShadows = true;
            node.object->AddComponent<TerrainBatchAppearance>()->distantVisual = true;
            node.object->enabled = false;
        }
        if (auto* appearance = node.object->GetComponent<TerrainBatchAppearance>();
            appearance && appearance->materials.empty() && !m_farLayerMaterials.empty())
        {
            appearance->materials = m_farLayerMaterials;
            appearance->textureTiling = m_farLayerTiling;
        }
        node.vertexCount = built->geometry.vertices.size();
        const bool hasGeometry = !built->geometry.indices.empty();
        auto* provider = scene->GetGraphicsProvider();
        auto* factory = provider ? provider->GetBufferFactory() : nullptr;
        // Own buffers, created without a wait: the vertex upload is recorded
        // with the next frame (CreateStaticVertexBufferDeferred) and indices
        // go in an upload buffer. The content-cached path (AcquireMeshBuffer)
        // hashed every vertex and waited for the copy: 6-14 ms of the main
        // thread for 1-4 nodes on each cell crossing (todo.md Step 0).
        const bool ownBuffers = factory && factory->SupportsWorkerThreadStaticVertexBufferCreation();
        if (hasGeometry && node.mesh->SetTerrainGeometry(std::move(built->geometry), false, true, ownBuffers))
        {
            node.mesh->occluderIndices = std::move(built->occluderIndices);
            node.mesh->occluderVertexCount = built->occluderVertexCount;
            if (ownBuffers)
            {
                const auto& vertices = node.mesh->GetTerrainVertices();
                const auto& indices = node.mesh->GetIndices();
                std::shared_ptr<Engine::Graphics::IGraphicsBuffer> vertexBuffer = factory->CreateStaticVertexBufferDeferred(
                    vertices.size() * sizeof(Mesh::TerrainVertex), vertices.data());
                std::shared_ptr<Engine::Graphics::IGraphicsBuffer> indexBuffer = factory->CreateBuffer(
                    Engine::Graphics::IGraphicsBuffer::Usage::IndexBuffer, Engine::Graphics::IGraphicsBuffer::AccessMode::Upload,
                    indices.size() * sizeof(uint32_t), indices.data(), sizeof(uint32_t));
                node.mesh->InstallOwnTerrainBuffers(std::move(vertexBuffer), std::move(indexBuffer), factory);
            }
            if (provider)
            {
                if (node.mesh->GetGpuBufferMemoryBytes() == 0)
                    node.mesh->CreateBuffer(factory);
                if (!node.mesh->IsReady()) node.mesh->OnAfterDeserialize(provider);
            }
            node.hasGeometry = true;
        }
        else node.hasGeometry = false;
        node.built = true;
        node.stale = false;
        ++m_farTileUploadCount;
        // A node shown already (a rebuild after an edit) keeps drawing.
        if (std::find(m_farShown.begin(), m_farShown.end(), key) != m_farShown.end())
            node.object->enabled = node.hasGeometry;
    }
    m_lastFarUpdateTiming.uploadedTiles = static_cast<size_t>(installs);
    m_lastFarUpdateTiming.uploadMs = std::chrono::duration<double, std::milli>(
        FarClock::now() - installStart).count();

    // A new wanted set once the viewer has moved a step (or the loaded cells
    // or edits changed) and the last set is shown.
    const auto selectionStart = FarClock::now();
    const bool swapPending = m_farWanted != m_farShown;
    const double stepMetres = std::max(farVolumeStepMetres, 1.f);
    const bool moved = !m_farStepValid ||
        glm::length(glm::dvec2(eye.x - m_farStep.x, eye.z - m_farStep.z)) >= stepMetres ||
        std::abs(eye.y - m_farStep.y) >= stepMetres;
    if (!swapPending && (moved || loadedChanged || m_farDirty))
    {
        m_farStep = eye;
        m_farStepValid = true;
        m_farShownCells = shownCells;
        m_farDirty = false;
        std::vector<FarVolumeKey> wanted;
        // ENGINE_FAR_VOLUME_DETAIL overrides farVolumeDetail (A/B runs).
        static const float forcedDetail = [] {
            const char* value = std::getenv("ENGINE_FAR_VOLUME_DETAIL");
            return value ? static_cast<float>(std::atof(value)) : 0.f;
        }();
        const float detail = std::max(forcedDetail > 0.f ? forcedDetail : farVolumeDetail, 1.f);
        const int first = -m_farHalfExtentCells - 1, last = m_farHalfExtentCells + 1;
        const auto floorTo = [](int value, int step) {
            return value >= 0 ? value / step * step : -((-value + step - 1) / step) * step;
        };
        // Visits a node: splits it near the viewer or over loaded cells.
        const std::function<void(FarVolumeKey)> visit = [&](FarVolumeKey key) {
            if (key.x + key.size <= first || key.x >= last || key.z + key.size <= first || key.z >= last)
                return;
            const double x0 = key.x * static_cast<double>(spanX), x1 = (key.x + key.size) * static_cast<double>(spanX);
            const double z0 = key.z * static_cast<double>(spanZ), z1 = (key.z + key.size) * static_cast<double>(spanZ);
            const double dx = std::max({x0 - eye.x, 0.0, eye.x - x1});
            const double dz = std::max({z0 - eye.z, 0.0, eye.z - z1});
            const double distance = std::sqrt(dx * dx + dz * dz);
            bool overLoaded = false;
            for (const Cell cell : shownCells)
                if (cell.first >= key.x && cell.first < key.x + key.size &&
                    cell.second >= key.z && cell.second < key.z + key.size) { overLoaded = true; break; }
            if (key.size > 1 && (overLoaded || distance < detail * key.size * spanX))
            {
                const int half = key.size / 2;
                for (int cz = 0; cz < 2; ++cz)
                    for (int cx = 0; cx < 2; ++cx)
                        visit({key.x + cx * half, key.z + cz * half, half});
                return;
            }
            if (key.size == 1 && overLoaded) return;   // the cell's volume draws it
            wanted.push_back(key);
        };
        for (int z = floorTo(first, TopNodeCells); z < last; z += TopNodeCells)
            for (int x = floorTo(first, TopNodeCells); x < last; x += TopNodeCells)
                visit({x, z, TopNodeCells});
        std::sort(wanted.begin(), wanted.end());
        m_farWanted = std::move(wanted);
        // Nearest first.
        m_farBuildOrder = m_farWanted;
        std::sort(m_farBuildOrder.begin(), m_farBuildOrder.end(), [&](const FarVolumeKey& a, const FarVolumeKey& b) {
            const auto centre = [&](const FarVolumeKey& k) {
                return glm::dvec2((k.x + k.size * .5) * spanX - eye.x, (k.z + k.size * .5) * spanZ - eye.z);
            };
            return glm::length(centre(a)) < glm::length(centre(b));
        });
    }
    m_lastFarUpdateTiming.selectionMs = std::chrono::duration<double, std::milli>(
        FarClock::now() - selectionStart).count();

    // Start builds for wanted nodes that have none (or are stale).
    const auto preparationStart = FarClock::now();
    for (const FarVolumeKey& key : m_farBuildOrder)
    {
        if (g_farJobs.load() >= MaxFarJobs) break;
        auto& node = m_farNodes[key];
        if (node.job.valid() || (node.built && !node.stale)) continue;
        auto input = std::make_shared<FarVolumeJobInput>();
        input->key = key;
        input->cellsX = cellsX; input->cellsY = cellsY;
        input->spanX = spanX; input->spanZ = spanZ;
        input->halfExtent = m_farHalfExtentCells;
        input->grid = m_farHeightsSnapshot;
        input->archive = m_farArchive;
        input->patches = m_farPatchHeights;
        input->palette = m_farPaletteShared;
        // Edits of the cells within a cell of the node, in the node's space.
        for (const Cell owner : VolumeEditOwners())
        {
            if (owner.first < key.x - 1 || owner.first > key.x + key.size ||
                owner.second < key.z - 1 || owner.second > key.z + key.size) continue;
            const glm::vec3 offset((owner.first - key.x) * spanX, 0.f, (owner.second - key.z) * spanZ);
            for (const auto& edit : GetVolumeEdits(owner)) input->edits.push_back(edit.Shifted(offset));
        }
        g_farJobs.fetch_add(1);
        node.job = std::async(std::launch::async, [input] {
            FarJobSlot slot;
            return BuildFarVolumeNode(*input);
        });
        ++m_lastFarUpdateTiming.queuedTiles;
    }
    m_lastFarUpdateTiming.preparationMs = std::chrono::duration<double, std::milli>(
        FarClock::now() - preparationStart).count();

    // Swap the wanted set in once every node of it is built.
    if (m_farWanted != m_farShown)
    {
        bool ready = true;
        for (const FarVolumeKey& key : m_farWanted)
        {
            const auto found = m_farNodes.find(key);
            if (found == m_farNodes.end() || !found->second.built) { ready = false; break; }
        }
        if (ready)
        {
            for (const FarVolumeKey& key : m_farShown)
                if (const auto found = m_farNodes.find(key); found != m_farNodes.end() && found->second.object)
                    found->second.object->enabled = false;
            for (const FarVolumeKey& key : m_farWanted)
            {
                auto& node = m_farNodes[key];
                if (node.object) node.object->enabled = node.hasGeometry;
            }
            m_farShown = m_farWanted;
            m_lastFarUpdateTiming.appearedTiles = m_farShown.size();
            // Drop nodes no longer wanted (their builds finish first).
            for (auto it = m_farNodes.begin(); it != m_farNodes.end(); )
            {
                if (std::binary_search(m_farWanted.begin(), m_farWanted.end(), it->first) ||
                    it->second.job.valid()) { ++it; continue; }
                if (it->second.object) scene->RemoveObject(it->second.object);
                it = m_farNodes.erase(it);
            }
            if (FarVolumeLog())
            {
                size_t vertices = 0;
                for (const auto& key : m_farShown) vertices += m_farNodes[key].vertexCount;
                std::printf("[FarVolume] swapped in %zu nodes, %zu vertices\n", m_farShown.size(), vertices);
            }
        }
    }
    m_lastFarUpdateTiming.totalMs = std::chrono::duration<double, std::milli>(
        FarClock::now() - updateStart).count();
    static const bool partLog = std::getenv("ENGINE_UPDATE_TYPE_LOG") != nullptr;
    if (partLog && m_lastFarUpdateTiming.totalMs > 2.0)
        std::printf("[FarVolume] total %.1f: upload %zu nodes %.1f, selection %.1f, preparation %zu queued %.1f, shown %zu nodes\n",
            m_lastFarUpdateTiming.totalMs, m_lastFarUpdateTiming.uploadedTiles, m_lastFarUpdateTiming.uploadMs,
            m_lastFarUpdateTiming.selectionMs, m_lastFarUpdateTiming.queuedTiles, m_lastFarUpdateTiming.preparationMs,
            m_farShown.size());
}

void TerrainManager::FenceFarVolumeJobs()
{
    for (auto& [key, node] : m_farNodes)
        if (node.job.valid()) node.job.wait();
}

void TerrainManager::MarkFarVolumeDirty(Cell cell)
{
    for (auto& [key, node] : m_farNodes)
        if (cell.first >= key.x - 1 && cell.first <= key.x + key.size &&
            cell.second >= key.z - 1 && cell.second <= key.z + key.size)
            node.stale = true;
    m_farDirty = true;
}

std::vector<Mesh*> TerrainManager::GetFarTileMeshes() const
{
    std::vector<Mesh*> meshes;
    for (const auto& key : m_farShown)
        if (const auto found = m_farNodes.find(key); found != m_farNodes.end() &&
            found->second.object && found->second.object->enabled && found->second.mesh)
            meshes.push_back(found->second.mesh);
    return meshes;
}

size_t TerrainManager::GetVisibleFarTileCount() const { return GetFarTileMeshes().size(); }
size_t TerrainManager::GetDesiredFarTileCount() const { return m_farWanted.size(); }
size_t TerrainManager::GetBuiltFarTileCount() const
{
    size_t built = 0;
    for (const auto& [key, node] : m_farNodes) built += node.built;
    return built;
}
bool TerrainManager::AreDesiredFarTilesBuilt() const
{
    return !m_farEnabled || m_farHeights.empty() || (m_farStepValid && m_farWanted == m_farShown);
}
size_t TerrainManager::GetPreparingFarTileCount() const
{
    size_t preparing = 0;
    for (const auto& [key, node] : m_farNodes) preparing += node.job.valid();
    return preparing;
}
size_t TerrainManager::GetVisibleFarTileVertexCount() const
{
    size_t vertices = 0;
    for (const auto& key : m_farShown)
        if (const auto found = m_farNodes.find(key); found != m_farNodes.end() && found->second.hasGeometry)
            vertices += found->second.vertexCount;
    return vertices;
}
std::vector<TerrainManager::FarLevelUsage> TerrainManager::GetFarLevelUsage() const
{
    std::map<int, FarLevelUsage> levels;
    for (const auto& [key, node] : m_farNodes)
    {
        auto& usage = levels[key.size];
        usage.stride = key.size;
        ++usage.residentTiles;
        if (node.object && node.object->enabled) ++usage.visibleTiles;
        if (node.mesh) usage.gpuBytes += node.mesh->GetGpuBufferMemoryBytes();
    }
    std::vector<FarLevelUsage> result;
    for (const auto& [size, usage] : levels) result.push_back(usage);
    return result;
}

size_t TerrainManager::CountVisibleCoverageGaps(int centerX, int centerY, int radius) const
{
    // Cells within the radius that neither a showing volume nor a shown far
    // node covers.
    if (!m_farEnabled) return 0;
    size_t gaps = 0;
    for (int y = centerY - radius; y <= centerY + radius; ++y)
        for (int x = centerX - radius; x <= centerX + radius; ++x)
        {
            if (std::abs(x) > m_farHalfExtentCells || std::abs(y) > m_farHalfExtentCells) continue;
            if (m_farShownCells.count({x, y})) continue;
            bool covered = false;
            for (const auto& key : m_farShown)
                if (x >= key.x && x < key.x + key.size && y >= key.z && y < key.z + key.size)
                { covered = true; break; }
            gaps += !covered;
        }
    return gaps;
}
}

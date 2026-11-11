#include "Scripts/Grass/GrassField.h"

#include "Core/Object.h"
#include "Core/Scene/Scene.h"
#include "Core/Compoonents/Terrain/Terrain.h"
#include "Core/Compoonents/Terrain/TerrainObjectLayer.h"
#include "Core/Compoonents/Terrain/TerrainSurfaceSample.h"
#include <glm/gtc/matrix_transform.hpp>
#include "Core/Compoonents/Materials/Material.h"
#include "Core/Serialization/SceneSerializer.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <random>
#include <set>
#include <limits>
#include <filesystem>
#include <tuple>
#include "Core/Memory/CacheStore.h"
#include "Core/Graphics/IGraphicsProvider.h"

namespace
{
struct GrassFieldRegistration
{
    GrassFieldRegistration()
    {
        Engine::Serialization::RegisterComponentType<GrassField>("GrassField");
    }
};
GrassFieldRegistration g_registration;

uint32_t TileSeed(int seed, int x, int z)
{
    uint32_t value = static_cast<uint32_t>(seed) ^ 0x9e3779b9u;
    value ^= static_cast<uint32_t>(x) * 0x85ebca6bu;
    value = (value ^ (value >> 16u)) * 0xc2b2ae35u;
    value ^= static_cast<uint32_t>(z) * 0x27d4eb2fu;
    return value ^ (value >> 16u);
}

constexpr uint64_t HashOffset = 1469598103934665603ull;
constexpr uint64_t HashPrime = 1099511628211ull;

void HashBytes(uint64_t& hash, const void* bytes, size_t byteCount)
{
    const auto* source = static_cast<const unsigned char*>(bytes);
    for (size_t index = 0; index < byteCount; ++index)
    {
        hash ^= static_cast<uint64_t>(source[index]);
        hash *= HashPrime;
    }
}

void HashFloat(uint64_t& hash, float value)
{
    uint32_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value), "float size mismatch");
    std::memcpy(&bits, &value, sizeof(bits));
    HashBytes(hash, &bits, sizeof(bits));
}
}

GrassField::GrassField()
{
    SetTypeName(COMPONENT_TYPE_NAME(GrassField));
    RegisterField("shortMesh", shortMesh, "Assets");
    RegisterField("tallMesh", tallMesh, "Assets");
    RegisterField("shortMeshLod1", shortMeshLod1, "Assets");
    RegisterField("shortMeshLod2", shortMeshLod2, "Assets");
    RegisterField("tallMeshLod1", tallMeshLod1, "Assets");
    RegisterField("tallMeshLod2", tallMeshLod2, "Assets");
    RegisterField("viewerObjectName", viewerObjectName, "Viewer");
    RegisterField("useEditorCamera", useEditorCamera, "Viewer");
    RegisterField("visibleRadius", visibleRadius, "Viewer");
    RegisterField("tileSize", tileSize, "Tiles");
    RegisterField("shortPatchesPerSquareMeter", shortPatchesPerSquareMeter, "Tiles");
    RegisterField("tallPatchesPerSquareMeter", tallPatchesPerSquareMeter, "Tiles");
    RegisterField("seed", seed, "Tiles");
    RegisterField("maxTileBuildsPerUpdate", maxTileBuildsPerUpdate, "Tiles");
    RegisterField("castShadows", castShadows, "Lighting");
    RegisterField("shaderPath", shaderPath, "Lighting");
    RegisterField("materialPath", materialPath, "Assets");
    RegisterField("windStrength", windStrength, "Lighting");
    RegisterField("tileWorkBudgetMs", tileWorkBudgetMs, "Tiles");
    RegisterField("fadeDuration", fadeDuration, "LOD");
    RegisterField("distanceFadeWidth", distanceFadeWidth, "LOD");
    RegisterField("gpuLodSelection", gpuLodSelection, "LOD");
    RegisterField("allowProceduralGeneration", allowProceduralGeneration, "Tiles");
    RegisterField("retainDormantTiles", retainDormantTiles, "Tiles");
}

std::map<std::string, Engine::Components::ObjectLodProfileData>
GrassField::GetObjectLodProfiles() const
{
    std::map<std::string, Engine::Components::ObjectLodProfileData> definitions;
    for (const auto& paths : {std::vector<std::string>{shortMesh, shortMeshLod1, shortMeshLod2},
            std::vector<std::string>{tallMesh, tallMeshLod1, tallMeshLod2}})
    {
        if (paths.front().empty()) continue;
        auto& profile = definitions[paths.front()];
        if (!profile.meshes.empty()) continue;
        for (const auto& path : paths)
            if (!path.empty()) profile.meshes.push_back(path);
    }
    return definitions;
}

bool GrassField::LoadSources()
{
    auto* provider = Owner->GetScene()->GetGraphicsProvider();
    auto* factory = provider ? provider->GetBufferFactory() : nullptr;
    auto definitions = ObjectLodProvider::GetObjectLodMeshes();
    // Resolve only the rendered species here; enumerating every placement belongs
    // in the inspector, not in the per-frame streaming path.
    if (m_cachedLayer)
        for (const auto& asset : {shortMesh, tallMesh})
            if (const auto found = m_cachedLayer->objectLods.find(asset);
                found != m_cachedLayer->objectLods.end() && !found->second.meshes.empty())
            {
                auto meshes = found->second.meshes;
                meshes.erase(std::remove(meshes.begin(), meshes.end(), std::string{}), meshes.end());
                if (meshes.size() > Engine::Components::TerrainObjectLayer::MaximumObjectLods)
                    meshes.resize(Engine::Components::TerrainObjectLayer::MaximumObjectLods);
                if (!meshes.empty()) definitions[asset] = std::move(meshes);
            }
    auto shortLevels = definitions[shortMesh], tallLevels = definitions[tallMesh];
    if (shortLevels.empty() || tallLevels.empty()) return false;
    const auto distances = [&](const std::string& asset, size_t count) {
        if (m_cachedLayer) return m_cachedLayer->GetObjectLodDistances(asset, count);
        std::vector<float> result;
        for (size_t i = 1; i < count; ++i) result.push_back(std::max(2.f, visibleRadius));
        return result;
    };
    const auto shortDistances = distances(shortMesh, shortLevels.size());
    const auto tallDistances = distances(tallMesh, tallLevels.size());
    // A shared tile changes only the species whose boundary was crossed.
    // The sorted union supports different LOD counts and distances per asset.
    m_lodDistances = shortDistances;
    m_lodDistances.insert(m_lodDistances.end(), tallDistances.begin(), tallDistances.end());
    std::sort(m_lodDistances.begin(), m_lodDistances.end());
    m_lodDistances.erase(std::unique(m_lodDistances.begin(), m_lodDistances.end()), m_lodDistances.end());
    m_useGpuLodSelection = (gpuLodSelection || (m_cachedLayer && m_cachedLayer->screenLodSelection)) && shortLevels.size() == 3 &&
        tallLevels.size() == 3 && shortDistances == tallDistances && m_lodDistances.size() == 2;
    std::vector<std::string> shortPaths, tallPaths;
    for (size_t i = 0; i <= m_lodDistances.size(); ++i)
    {
        const auto level = [&](const std::vector<float>& thresholds) {
            return i == 0 ? size_t(0) : size_t(std::upper_bound(thresholds.begin(), thresholds.end(), m_lodDistances[i - 1]) - thresholds.begin());
        };
        shortPaths.push_back(shortLevels[level(shortDistances)]);
        tallPaths.push_back(tallLevels[level(tallDistances)]);
    }
    if (m_loaded && m_loadedFactory == factory && m_loadedShortPaths == shortPaths &&
        m_loadedTallPaths == tallPaths)
        return m_shortVertices[0] && m_tallVertices[0] && !m_shortVertices[0]->empty() && !m_tallVertices[0]->empty();
    m_loaded = true;
    m_loadedFactory = factory;
    m_hasGenerationSignature = false;
    m_shortVertices.resize(shortPaths.size());
    m_tallVertices.resize(tallPaths.size());
    m_sourceMeshes.resize(shortPaths.size());
    m_loadedShortPaths = shortPaths;
    m_loadedTallPaths = tallPaths;
    try
    {
        const auto load = [](const std::string& path) -> std::shared_ptr<const Source> {
            const auto resolved = Engine::Components::Mesh::ResolveFilePath(path);
            std::error_code error;
            const auto modified = std::filesystem::last_write_time(resolved, error);
            const std::string key = Engine::Memory::CacheStore::PathKey(resolved,
                error ? "missing" : std::to_string(modified.time_since_epoch().count()));
            return Engine::Memory::CacheStore::Get().GetOrCreate<Source>(
                Engine::Memory::CacheLifetime::LongTerm, "GrassSpecies", key, [&]() {
                    Engine::Components::Mesh mesh;
                    mesh.LoadFromFile(path);
                    if (!mesh.GetIndices().empty())
                    {
                        auto triangles=std::make_shared<Source>();
                        triangles->reserve(mesh.GetIndices().size());
                        for (const auto index:mesh.GetIndices()) triangles->push_back(mesh.GetVertices().at(index));
                        return triangles;
                    }
                    return std::make_shared<Source>(mesh.TakeVertices());
                });
        };
        for (int lod = 0; lod < static_cast<int>(m_sourceMeshes.size()); ++lod)
        {
            m_shortVertices[lod] = load(shortPaths[lod]);
            m_tallVertices[lod] = load(tallPaths[lod]);
        }
        // Weak ownership keeps GPU resources tied to their users/device instead
        // of retaining them forever in the process-wide CPU asset cache.
        using Mesh = Engine::Components::Mesh;
        static std::map<std::tuple<const void*, const void*, const void*>, std::weak_ptr<Mesh>> sharedMeshes;
        for (auto it = sharedMeshes.begin(); it != sharedMeshes.end();)
            if (it->second.expired()) it = sharedMeshes.erase(it); else ++it;
        bool first = true;
        for (int lod = 0; lod < static_cast<int>(m_sourceMeshes.size()); ++lod)
        {
            const auto key = std::make_tuple(factory, m_shortVertices[lod].get(), m_tallVertices[lod].get());
            auto mesh = sharedMeshes[key].lock();
            if (!mesh)
            {
                Source vertices = *m_shortVertices[lod];
                vertices.insert(vertices.end(), m_tallVertices[lod]->begin(), m_tallVertices[lod]->end());
                mesh = std::make_shared<Mesh>();
                mesh->SetAuthoredVertices(std::move(vertices));
                if (factory) mesh->CreateBuffer(factory);
                sharedMeshes[key] = mesh;
            }
            m_sourceMeshes[lod] = mesh;
            if (first) { m_sourceMinimum = mesh->GetBoundsMin(); m_sourceMaximum = mesh->GetBoundsMax(); first = false; }
            else { m_sourceMinimum = glm::min(m_sourceMinimum, mesh->GetBoundsMin());
                m_sourceMaximum = glm::max(m_sourceMaximum, mesh->GetBoundsMax()); }
        }
        // Reserve the largest permitted bend once, for every LOD. Wind edits
        // never regenerate placements or change their GPU buffers.
        const float windPadding = 1.5f * std::max(0.f, m_sourceMaximum.y);
        m_sourceMinimum -= glm::vec3(windPadding);
        m_sourceMaximum += glm::vec3(windPadding);
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "GrassField: unable to load source mesh: %s\n",
            error.what());
        for (auto& source : m_shortVertices) source.reset();
        for (auto& source : m_tallVertices) source.reset();
        for (auto& source : m_sourceMeshes) source.reset();
    }
    return m_shortVertices[0] && m_tallVertices[0] && !m_shortVertices[0]->empty() && !m_tallVertices[0]->empty();
}

void GrassField::Start()
{
    // Keep editor-warmed GPU geometry when entering Play.
    m_cachedViewer = nullptr;
    m_cachedStructureRevision = 0;
    EnsureAuthoredPreviewHidden();
}

void GrassField::ClearTiles()
{
    if (Owner && Owner->GetScene())
        for (auto& entry : m_tiles)
            if (entry.second.object)
            {
                entry.second.object->enabled = false;
                Owner->GetScene()->RequestRemoveObject(entry.second.object);
            }
    m_tiles.clear();
    m_build.reset();
    m_hasGenerationSignature = false;
}

void GrassField::SuspendTiles()
{
    m_build.reset();
    for (auto& [key, tile] : m_tiles)
    {
        tile.object->enabled = false;
        tile.visibility = tile.appearance = 0.f;
        tile.mesh->SetRuntimeVisibility(0.f);
    }
}

void GrassField::LeaveSpatialRegion()
{
    if (retainDormantTiles) SuspendTiles();
    else ClearTiles();
    m_performance = {};
}

bool GrassField::EnsureAuthoredPreviewHidden()
{
    if (m_authoredPreviewHidden || !Owner) return false;
    bool changed = false;
    // Retire legacy preview objects and serialized generated tiles once.
    // Placements remain authored on the layer; tiles are regenerated as needed.
    for (auto* child : Owner->Children)
    {
        if (!child || (child->name != "Grass Preview" &&
            child->name.rfind("Grass Tile (", 0) != 0)) continue;
        if (child->enabled) { child->enabled = false; changed = true; }
        child->runtimeOnly = true;
        if (Owner->GetScene()) Owner->GetScene()->RequestRemoveObject(child);
    }
    m_authoredPreviewHidden = true;
    return changed;
}

bool GrassField::ResolveCachedReferences()
{
    if (!Owner)
    {
        m_cachedScene = nullptr;
        m_cachedViewer = nullptr;
        m_cachedTerrain = nullptr;
        m_cachedLayer = nullptr;
        m_cachedStructureRevision = 0;
        return false;
    }
    auto* scene = Owner->GetScene();
    if (!scene)
    {
        m_cachedScene = nullptr;
        m_cachedViewer = nullptr;
        m_cachedTerrain = nullptr;
        m_cachedLayer = nullptr;
        m_cachedStructureRevision = 0;
        return false;
    }
    const uint64_t structureRevision = scene->GetStructureRevision();
    const bool refreshPointers = m_cachedScene != scene ||
        m_cachedStructureRevision != structureRevision ||
        m_cachedViewerName != viewerObjectName ||
        ((m_cachedViewer == &scene->editorCamera) !=
            (useEditorCamera && !scene->HasStarted())) ||
        !m_cachedViewer || !m_cachedTerrain;
    m_cachedScene = scene;
    m_cachedLayer = Owner->GetComponent<Engine::Components::TerrainObjectLayer>();
    if (refreshPointers)
    {
        m_cachedViewer = useEditorCamera && !scene->HasStarted()
            ? &scene->editorCamera : Owner->FindObjectInSceneByName(viewerObjectName);
        m_cachedTerrain = Owner->GetComponentInParent<Engine::Components::Terrain>();
        m_cachedViewerName = viewerObjectName;
        m_cachedStructureRevision = structureRevision;
    }
    return m_cachedViewer && m_cachedTerrain;
}

float GrassField::SampleHeight(float x, float z, glm::vec3* normal) const
{
    const auto* terrain = Owner
        ? Owner->GetComponentInParent<Engine::Components::Terrain>() : nullptr;
    if (!terrain || terrain->cellsX <= 0 || terrain->cellsY <= 0 ||
        terrain->cellWidth <= 0.f || terrain->cellDepth <= 0.f ||
        terrain->heights.size() != static_cast<size_t>(terrain->cellsX + 1) *
            static_cast<size_t>(terrain->cellsY + 1)) return 0.f;
    float height = 0.f;
    Engine::Components::SampleTerrainSurface(terrain->heights, terrain->cellsX,
        terrain->cellsY, terrain->cellWidth, terrain->cellDepth, x, z, height, normal);
    return height;
}

void GrassField::RefreshPlacementIndex()
{
    const auto* layer = Owner->GetComponent<Engine::Components::TerrainObjectLayer>();
    const auto revision = layer ? layer->GetConfigurationRevision() : 0;
    const auto* data = layer ? layer->placements.data() : nullptr;
    const auto count = layer ? layer->placements.size() : 0;
    if (layer == m_indexedLayer && revision == m_placementRevision &&
        data == m_placementData && count == m_placementCount &&
        tileSize == m_indexedTileSize) return;
    const auto started = Clock::now();
    m_placementTiles=layer ? &layer->GetPreparedTiles(std::max(1.f,tileSize)):nullptr;
    m_placementSignature=layer ? layer->GetPreparedSignature(std::max(1.f,tileSize)):HashOffset;
    if(layer && !layer->HasLoadedPreparedData()) m_performance.placementsScanned+=layer->placements.size();
    m_indexedLayer = layer;
    m_placementRevision = revision;
    m_placementData = data;
    m_placementCount = count;
    m_indexedTileSize = tileSize;
    m_performance.scanMs += std::chrono::duration<double, std::milli>(
        Clock::now() - started).count();
}

uint64_t GrassField::ComputeGenerationSignature() const
{
    uint64_t hash = m_placementSignature;
    for (const auto* path : {&shortMesh, &tallMesh, &shortMeshLod1,
            &shortMeshLod2, &tallMeshLod1, &tallMeshLod2})
        HashBytes(hash, path->data(), path->size());
    HashFloat(hash, tileSize);
    HashFloat(hash, shortPatchesPerSquareMeter);
    HashFloat(hash, tallPatchesPerSquareMeter);
    HashBytes(hash, &seed, sizeof(seed));
    HashBytes(hash,&allowProceduralGeneration,sizeof(allowProceduralGeneration));
    if (m_cachedTerrain)
    {
        const auto revision = m_cachedTerrain->GetConfigurationRevision();
        HashBytes(hash, &revision, sizeof(revision));
        HashBytes(hash, &m_cachedTerrain->cellsX, sizeof(m_cachedTerrain->cellsX));
        HashBytes(hash, &m_cachedTerrain->cellsY, sizeof(m_cachedTerrain->cellsY));
        HashFloat(hash, m_cachedTerrain->cellWidth);
        HashFloat(hash, m_cachedTerrain->cellDepth);
    }
    return hash;
}

void GrassField::BeginBuild(Tile tile, int lod, uint64_t signature)
{
    m_build.emplace();
    auto& job = *m_build;
    job.tile = tile;
    job.lod = lod;
    job.signature = signature;
    if (m_placementCount && m_placementTiles)
    {
        const auto found=m_placementTiles->find(tile);
        if(found!=m_placementTiles->end())
        {
            const auto& prepared=found->second;
            std::vector<glm::mat4> transforms;
            std::vector<uint64_t> placementIds;
            size_t placementCount=0;
            for (const auto& [asset, records] : prepared.groups)
                placementCount += records.size();
            transforms.reserve(placementCount);
            placementIds.reserve(placementCount);
            for(bool tall:{false,true}) for(const auto& [asset,records]:prepared.groups) if((asset==tallMesh)==tall)
                for(const auto& record:records)
                {
                    glm::mat4 transform(1.f);
                    for (int row = 0; row < 3; ++row)
                    {
                        transform[0][row] = record.transform.rows[row].x;
                        transform[1][row] = record.transform.rows[row].y;
                        transform[2][row] = record.transform.rows[row].z;
                        transform[3][row] = record.transform.rows[row].w;
                    }
                    transforms.push_back(transform);
                    placementIds.push_back(m_cachedLayer->placements[record.index].id);
                    if(!tall) ++job.shortCount;
                }
            job.instanceLayer.SetPlacements(std::move(transforms),
                std::move(placementIds));
            const float radius=glm::length(glm::max(glm::abs(m_sourceMinimum),glm::abs(m_sourceMaximum)))*prepared.maximumScale;
            job.minimum=prepared.minimum-glm::vec3(radius); job.maximum=prepared.maximum+glm::vec3(radius);
        }
        job.totalCount=uint32_t(job.instanceLayer.Size()); job.placement=job.totalCount; job.prepared=true;
        uint64_t identity=HashOffset;
        for(auto* owner=Owner;owner;owner=owner->Parent) HashBytes(identity,owner->name.data(),owner->name.size());
        HashBytes(identity,&tile.first,sizeof(tile.first)); HashBytes(identity,&tile.second,sizeof(tile.second));
        job.instanceLayer.GetInstanceData()->stableId=identity;
        return;
    }
    else
    {
        const float span = std::max(1.f, tileSize);
        const float width = m_cachedTerrain->cellsX * m_cachedTerrain->cellWidth;
        const float depth = m_cachedTerrain->cellsY * m_cachedTerrain->cellDepth;
        std::mt19937 rng(TileSeed(seed, tile.first, tile.second));
        std::uniform_real_distribution<float> unit(0.f, 1.f);
        const auto append = [&](const std::string& asset, float density, int cap)
        {
            const int count = std::clamp(static_cast<int>(std::round(
                density * span * span)), 0, cap);
            for (int i = 0; i < count; ++i)
            {
                Placement p;
                p.position.x = tile.first * span + unit(rng) * span;
                p.position.z = tile.second * span + unit(rng) * span;
                if (p.position.x < .3f || p.position.z < .3f ||
                    p.position.x > width - .3f || p.position.z > depth - .3f) continue;
                p.asset = asset;
                p.position.y = SampleHeight(p.position.x, p.position.z, &p.normal);
                p.yawDegrees = unit(rng) * 360.f;
                p.scale = .8f + .4f * unit(rng);
                job.placements.push_back(std::move(p));
            }
        };
        append(shortMesh, shortPatchesPerSquareMeter, 64);
        append(tallMesh, tallPatchesPerSquareMeter, 32);
    }
    job.totalCount=uint32_t(job.placements.size());
    // Procedural opt-in data still needs preparation; saved layers use baked records.
    // Group instances by species so each source vertex range is drawn once.
    std::stable_partition(job.placements.begin(), job.placements.end(),
        [&](const Placement& p) { return p.asset != tallMesh; });
    job.shortCount = static_cast<uint32_t>(std::count_if(job.placements.begin(), job.placements.end(),
        [&](const Placement& p) { return p.asset != tallMesh; }));
    job.instanceLayer.Reserve(job.placements.size());
}

bool GrassField::AdvanceBuild(Clock::time_point deadline)
{
    auto& job = *m_build;
    const auto generated = Clock::now();
    const float span = std::max(1.f, tileSize);
    while (!job.prepared && job.placement < job.placements.size())
    {
        const size_t placementIndex = job.placement++;
        const auto& p = job.placements[placementIndex];
        glm::mat4 transform = m_cachedLayer
            ? m_cachedLayer->PlacementTransform(p)
            : glm::translate(glm::mat4(1.f), p.position) *
                glm::mat4_cast(Engine::Components::TerrainObjectLayer::SurfaceRotation(p.normal, p.yawDegrees)) *
                glm::scale(glm::mat4(1.f), glm::vec3(p.scale));
        transform[3] -= glm::vec4(job.tile.first * span, 0.f, job.tile.second * span, 0.f);
        uint64_t placementId = HashOffset;
        HashBytes(placementId, &seed, sizeof(seed));
        HashBytes(placementId, &job.tile.first, sizeof(job.tile.first));
        HashBytes(placementId, &job.tile.second, sizeof(job.tile.second));
        HashBytes(placementId, &placementIndex, sizeof(placementIndex));
        if (!placementId) placementId = 1;
        p.id = placementId;
        job.instanceLayer.AddPlacement(transform, placementId);
        // Eight bounding corners per placement, independent of blade complexity.
        for (int corner = 0; corner < 8; ++corner)
        {
            const glm::vec3 point{corner & 1 ? m_sourceMaximum.x : m_sourceMinimum.x,
                corner & 2 ? m_sourceMaximum.y : m_sourceMinimum.y,
                corner & 4 ? m_sourceMaximum.z : m_sourceMinimum.z};
            const glm::vec3 position = glm::vec3(transform * glm::vec4(point, 1.f));
            if (job.placement == 1 && corner == 0) job.minimum = job.maximum = position;
            else { job.minimum = glm::min(job.minimum, position);
                job.maximum = glm::max(job.maximum, position); }
        }
        if (job.placement % 16 == 0 && Clock::now() >= deadline)
        {
            m_performance.generationMs += std::chrono::duration<double, std::milli>(
                Clock::now() - generated).count();
            return false;
        }
    }
    m_performance.generationMs += std::chrono::duration<double, std::milli>(
        Clock::now() - generated).count();
    // Defer publication if preparation consumed the frame's budget.
    if (Clock::now() >= deadline) return false;
    auto& tile = m_tiles.at(job.tile);
    using Mesh = Engine::Components::Mesh;
    // Register in the persistent GPU pool on first render. Custom shader
    // fallbacks create native slices lazily; normal draws have no duplicate upload.
    // All LODs reuse these placement buffers. A distance change only selects
    // another shared source range; it never schedules generation or an upload.
    for (int lod = 0; lod < static_cast<int>(m_sourceMeshes.size()); ++lod)
    {
        auto geometry = std::make_unique<Mesh>();
        const auto shortVertices = static_cast<uint32_t>(m_shortVertices[lod]->size());
        std::vector<Mesh::InstanceRange> ranges{
            {0, shortVertices, 0, job.shortCount},
            {shortVertices, static_cast<uint32_t>(m_tallVertices[lod]->size()), job.shortCount,
                job.totalCount - job.shortCount}};
        geometry->SetInstancedGeometry(m_sourceMeshes[lod],
            job.instanceLayer.GetInstanceData(), std::move(ranges),
            job.minimum, job.maximum);
        tile.mesh->StoreRuntimeLod(lod, std::move(geometry));
    }
    ++m_performance.tilesBuilt;
    m_build.reset();
    return true;
}

bool GrassField::ActivateTileLod(TileState& tile, int lod)
{
    if (!tile.mesh->BeginRuntimeLodTransition(lod, std::max(0.f, fadeDuration))) return false;
    tile.object->enabled = tile.mesh->GetVertexCount() > 0;
    ++m_performance.lodSwitches;
    return true;
}

bool GrassField::Refresh()
{
    const auto started = Clock::now();
    const float elapsed = m_lastRefresh.time_since_epoch().count()
        ? std::chrono::duration<float>(started - m_lastRefresh).count() : 0.f;
    m_lastRefresh = started;
    m_performance = {};
    if (!Owner || !Owner->GetScene()) return false;
    auto& scene = *Owner->GetScene();
    auto phaseStarted = started;
    const auto checkpoint = [&](double& milliseconds) {
        const auto now = Clock::now();
        milliseconds += std::chrono::duration<double, std::milli>(now-phaseStarted).count();
        phaseStarted = now;
    };
    const auto publish = [&] {
        checkpoint(m_performance.otherMs);
        m_performance.totalMs = std::chrono::duration<double, std::milli>(phaseStarted-started).count();
        scene.AddGrassPerformance(m_performance);
    };
    const float delta = std::clamp(scene.HasStarted() ? scene.GetDeltaTime() : elapsed, 0.f, .1f);
    bool changed = EnsureAuthoredPreviewHidden();
    const bool referencesReady = ResolveCachedReferences();
    checkpoint(m_performance.referencesMs);
    if (!referencesReady) { publish(); return changed; }
    const glm::vec3 local = glm::vec3(glm::inverse(
        m_cachedTerrain->Owner->transform.GetWorldMatrix()) *
        glm::vec4(m_cachedViewer->transform.GetWorldPosition(), 1.f));
    const auto* layer = m_cachedLayer;
    const float span = std::max(1.f, tileSize);
    const float radius = layer ? std::max(1.f, layer->viewDistance) : std::max(2.f, visibleRadius);
    const float terrainWidth = m_cachedTerrain->cellsX * m_cachedTerrain->cellWidth;
    const float terrainDepth = m_cachedTerrain->cellsY * m_cachedTerrain->cellDepth;
    const float cellEdgeX = local.x - std::clamp(local.x, 0.f, terrainWidth);
    const float cellEdgeZ = local.z - std::clamp(local.z, 0.f, terrainDepth);
    const float retentionRadius = radius + 2.f * span;
    if (cellEdgeX * cellEdgeX + cellEdgeZ * cellEdgeZ > retentionRadius * retentionRadius)
    {
        // Distant cells must not load species meshes, index placements, or
        // generate tile geometry just because they exist in the stress scene.
        if (!m_tiles.empty() || m_build)
        { if (retainDormantTiles) SuspendTiles(); else ClearTiles(); changed = true; }
        checkpoint(m_performance.retirementMs);
        publish();
        return changed;
    }
    if((!m_cachedLayer || m_cachedLayer->placements.empty()) && !allowProceduralGeneration)
    {
        if(!m_tiles.empty() || m_build) { ClearTiles(); changed=true; }
        checkpoint(m_performance.retirementMs);
        publish(); return changed;
    }
    checkpoint(m_performance.otherMs);
    const bool sourcesReady = LoadSources();
    checkpoint(m_performance.sourcesMs);
    if (!sourcesReady) { publish(); return changed; }
    RefreshPlacementIndex();
    const uint64_t signature = ComputeGenerationSignature();
    if (!m_hasGenerationSignature || signature != m_lastGenerationSignature)
    {
        ClearTiles();
        m_lastGenerationSignature = signature;
        m_hasGenerationSignature = true;
    }
    checkpoint(m_performance.placementMs);
    const float lod1 = m_lodDistances.empty() ? radius : m_lodDistances.front();
    const float lod2 = m_lodDistances.size() < 2 ? radius : m_lodDistances[1];
    const float hysteresis = layer ? std::max(0.f, layer->lodHysteresis) : 0;
    const float shadowRadius = layer ? std::clamp(layer->shadowDistance, 0.f, radius) : radius;
    const int reach = static_cast<int>(std::ceil(radius / span)) + 1;
    const int centerX = static_cast<int>(std::floor(local.x / span));
    const int centerZ = static_cast<int>(std::floor(local.z / span));
    const int maxX = static_cast<int>(std::ceil(m_cachedTerrain->cellsX * m_cachedTerrain->cellWidth / span));
    const int maxZ = static_cast<int>(std::ceil(m_cachedTerrain->cellsY * m_cachedTerrain->cellDepth / span));
    std::set<Tile> desired;
    for (int z = std::max(0, centerZ - reach); z < std::min(maxZ, centerZ + reach + 1); ++z)
        for (int x = std::max(0, centerX - reach); x < std::min(maxX, centerX + reach + 1); ++x)
        {
            const float dx = (x + .5f) * span - local.x;
            const float dz = (z + .5f) * span - local.z;
            const float edgeX = std::max(0.f, std::abs(dx) - span * .5f);
            const float edgeZ = std::max(0.f, std::abs(dz) - span * .5f);
            if (edgeX * edgeX + edgeZ * edgeZ > radius * radius) continue;
            if (m_placementCount && !m_placementTiles->count({x,z})) continue;
            const Tile key{x,z};
            desired.insert(key);
            auto& tile = m_tiles[key];
            if (!tile.object)
            {
                char name[64];
                std::snprintf(name, sizeof(name), "Grass Tile (%d, %d)", x, z);
                tile.object = scene.AddObject(name);
                tile.object->runtimeOnly = true;
                tile.object->enabled = false;
                scene.SetParentLocal(tile.object, Owner);
                tile.object->transform.position = {x * span, 0, z * span};
                tile.mesh = tile.object->AddComponent<Engine::Components::Mesh>();
                tile.mesh->useBoundsForShadowCulling = true;
                tile.mesh->usesGrassShadowBudget = true;
                tile.mesh->SetRuntimeVisibility(0.f);
                auto* material = tile.object->AddComponent<Engine::Components::Material>();
                material->diffuseColor = {1,1,1};
                material->useVertexColors = true;
                if (!shaderPath.empty())
                    material->shader = shaderPath;
                material->roughnessFactor = 1;
                material->metallicFactor = 0;
                material->doubleSided = true;
                changed = true;
            }
            changed = tile.mesh->AdvanceRuntimeLodTransition(delta) || changed;
            const float edgeDistance = std::sqrt(edgeX * edgeX + edgeZ * edgeZ);
            const float targetVisibility = distanceFadeWidth > 0.f
                ? std::clamp((radius - edgeDistance) / distanceFadeWidth, 0.f, 1.f) : 1.f;
            const float previousVisibility = tile.visibility;
            if (tile.mesh->GetActiveRuntimeLod() >= 0)
            {
                const float step = fadeDuration > 0.f ? delta / fadeDuration : 1.f;
                tile.appearance = std::min(1.f, tile.appearance + step);
                tile.visibility = tile.appearance * targetVisibility;
                tile.mesh->SetRuntimeVisibility(tile.visibility);
            }
            changed = previousVisibility != tile.visibility || changed;
            tile.distanceSquared = dx * dx + dz * dz;
            const float distance = std::sqrt(tile.distanceSquared);
            const int lod = m_useGpuLodSelection ? 0 : tile.mesh->ChooseRuntimeLod(distance, m_lodDistances, hysteresis);
            tile.desiredLod = m_useGpuLodSelection ? 0 : lod;
            tile.mesh->gpuLodSelection=m_useGpuLodSelection;
            tile.mesh->screenLodSelection=m_useGpuLodSelection && layer && layer->screenLodSelection;
            tile.mesh->distanceLodOverride=layer && layer->distanceLodOverride;
            if(layer)
            {
                tile.mesh->screenLod1Pixels=layer->screenLod1Pixels;
                tile.mesh->screenLod2Pixels=layer->screenLod2Pixels;
                tile.mesh->screenLodHysteresis=layer->screenLodHysteresis;
            }
            tile.mesh->gpuLod1Distance=lod1; tile.mesh->gpuLod2Distance=lod2;
            // Keep caster eligibility stable. The shadow shader fades each
            // surface at the range edge; compute culls distant placements.
            const bool shadows = castShadows && shadowRadius > 0;
            tile.mesh->runtimeShadowDistance=shadowRadius;
            tile.mesh->runtimeShadowFadeWidth=std::min(shadowRadius,std::max(.25f,distanceFadeWidth));
            tile.castsShadows = shadows;
            auto* tileMaterial = tile.object->GetComponent<Engine::Components::Material>();
            if (!materialPath.empty())
            {
                if (m_loadedMaterialPath!=materialPath)
                {
                    m_loadedMaterialPath=materialPath;
                    m_resolvedMaterialPath=Engine::Components::Mesh::ResolveFilePath(materialPath);
                }
                if (tileMaterial->GetFilePath()!=m_resolvedMaterialPath && tileMaterial->LoadFromFile(m_resolvedMaterialPath))
                    tileMaterial->PrepareTextures(scene.GetGraphicsProvider());
            }
            tile.mesh->foliageWindStrength=std::clamp(windStrength,0.f,1.f);
            tile.mesh->usesGrassShadowBudget=windStrength>0.f;
            tileMaterial->castsShadows = shadows;
            tileMaterial->shader = shaderPath;
            changed = ActivateTileLod(tile, tile.desiredLod) || changed;
            tile.object->enabled = tile.mesh->GetActiveRuntimeLod() >= 0 && tile.visibility > 0.f;
        }
    checkpoint(m_performance.visibilityMs);
    for (auto it = m_tiles.begin(); it != m_tiles.end();)
    {
        if (desired.count(it->first)) { ++it; continue; }
        const float edgeX = std::max(0.f, std::abs((it->first.first + .5f) * span - local.x) - span * .5f);
        const float edgeZ = std::max(0.f, std::abs((it->first.second + .5f) * span - local.z) - span * .5f);
        if (edgeX * edgeX + edgeZ * edgeZ <= retentionRadius * retentionRadius)
        {
            it->second.object->enabled = false;
            it->second.visibility = it->second.appearance = 0.f;
            it->second.mesh->SetRuntimeVisibility(0.f);
            ++it;
            continue;
        }
        if (m_build && m_build->tile == it->first) m_build.reset();
        it->second.object->enabled = false;
        if (retainDormantTiles)
        {
            it->second.visibility = it->second.appearance = 0.f;
            it->second.mesh->SetRuntimeVisibility(0.f);
            ++it;
        }
        else
        {
            scene.RequestRemoveObject(it->second.object);
            it = m_tiles.erase(it);
        }
        changed = true;
    }
    checkpoint(m_performance.retirementMs);
    // Prioritize nearby missing tiles. Preparing placements installs all three
    // shared-source LODs together, so resident tiles require no further builds.
    TileState* nearest = nullptr;
    Tile selected{};
    int selectedLod = -1;
    bool urgent = false;
    for (auto& [key, tile] : m_tiles)
    {
        if (!desired.count(key)) continue;
        int missing = !tile.mesh->IsRuntimeLodResident(tile.desiredLod) ? tile.desiredLod : -1;
        const bool required = missing >= 0;
        if (missing < 0)
            for (int lod = 0; lod < static_cast<int>(m_sourceMeshes.size()); ++lod)
                if (!tile.mesh->IsRuntimeLodResident(lod)) { missing = lod; break; }
        if (missing < 0) continue;
        ++m_performance.pending;
        if (!nearest || (required && !urgent) || (required == urgent &&
            tile.distanceSquared < nearest->distanceSquared))
        { nearest = &tile; selected = key; selectedLod = missing; urgent = required; }
    }
    if (m_build)
    {
        const auto& tile = m_tiles.at(m_build->tile);
        if (m_build->signature != signature || (urgent &&
            m_build->lod != tile.desiredLod && tile.mesh->IsRuntimeLodResident(tile.desiredLod)))
            m_build.reset();
    }
    const double spent = scene.GetGrassPerformance().generationMs + scene.GetGrassPerformance().uploadMs;
    const double remaining = std::max(0., std::clamp(double(tileWorkBudgetMs), .1, 16.) - spent);
    if (!m_build && nearest && remaining > 0.) BeginBuild(selected, selectedLod, signature);
    checkpoint(m_performance.schedulingMs);
    if (m_build && remaining > 0.)
    {
        const auto deadline = started + std::chrono::duration_cast<Clock::duration>(
            std::chrono::duration<double, std::milli>(remaining));
        const Tile key = m_build->tile;
        if (AdvanceBuild(deadline))
        {
            auto& tile = m_tiles.at(key);
            // Keep current geometry until the desired replacement is resident.
            changed = ActivateTileLod(tile, tile.desiredLod) || changed;
            if (tile.mesh->GetActiveRuntimeLod() < 0)
                for (int lod = 0; lod < static_cast<int>(m_sourceMeshes.size()); ++lod)
                    if (tile.mesh->IsRuntimeLodResident(lod)) { changed = ActivateTileLod(tile, lod) || changed; break; }
        }
    }
    checkpoint(m_performance.buildMs);
    for (const auto& [key, tile] : m_tiles)
    {
        m_performance.residentLods += tile.mesh->GetResidentRuntimeLodCount();
        if (const auto& instances = tile.mesh->GetInstanceData())
        {
            if (tile.object->enabled)
            {
                m_performance.instances += instances->transforms.size();
                ++m_performance.activeTiles;
                m_performance.transitionTiles+=tile.mesh->GetPreviousRuntimeLodMesh()!=nullptr;
                m_performance.fadingTiles+=tile.mesh->GetRuntimeVisibility()<1.f;
            }
            for (const auto& slice : instances->slices)
                if (scene.AccountGrassGpuResource(slice.buffer.get()))
                    m_performance.gpuBytes += slice.buffer->GetSize();
        }
    }
    for (const auto& source : m_sourceMeshes)
        if (source && scene.AccountGrassGpuResource(source->GetGraphicsBuffer()))
            m_performance.gpuBytes += source->GetGraphicsBuffer()->GetSize();
    checkpoint(m_performance.accountingMs);
    publish();
    return changed;
}

uint64_t GrassField::SpatialRegionRevision() const
{
    uint64_t revision=HashOffset;
    if(Owner) { const auto world=Owner->transform.GetWorldRevision(); HashBytes(revision,&world,sizeof(world)); }
    HashBytes(revision,viewerObjectName.data(),viewerObjectName.size());
    HashBytes(revision,shaderPath.data(),shaderPath.size());
    HashBytes(revision,materialPath.data(),materialPath.size()); HashFloat(revision,windStrength);
    HashBytes(revision,&useEditorCamera,sizeof(useEditorCamera)); HashFloat(revision,tileSize); HashFloat(revision,visibleRadius);
    if(Owner && Owner->GetScene()) { const bool started=Owner->GetScene()->HasStarted(); HashBytes(revision,&started,sizeof(started)); }
    if(m_cachedLayer)
    {
        HashFloat(revision,m_cachedLayer->viewDistance);
        HashBytes(revision,&m_cachedLayer->screenLodSelection,sizeof(bool));
        HashBytes(revision,&m_cachedLayer->distanceLodOverride,sizeof(bool));
        HashFloat(revision,m_cachedLayer->screenLod1Pixels);
        HashFloat(revision,m_cachedLayer->screenLod2Pixels);
        HashFloat(revision,m_cachedLayer->screenLodHysteresis);
    }
    if(m_cachedTerrain)
    {
        HashBytes(revision,&m_cachedTerrain->cellsX,sizeof(m_cachedTerrain->cellsX));
        HashBytes(revision,&m_cachedTerrain->cellsY,sizeof(m_cachedTerrain->cellsY));
        HashFloat(revision,m_cachedTerrain->cellWidth); HashFloat(revision,m_cachedTerrain->cellDepth);
    }
    return revision;
}
Engine::Components::SpatialUpdateRegion GrassField::DescribeSpatialRegion()
{
    if(!ResolveCachedReferences()) return {};
    const auto& terrain=*m_cachedTerrain;
    const auto world=terrain.Owner->transform.GetWorldMatrix();
    float low=0,high=0;
    if(!terrain.heights.empty()) { const auto range=std::minmax_element(terrain.heights.begin(),terrain.heights.end()); low=*range.first; high=*range.second; }
    auto bounds=Engine::Rendering::TransformBounds({0,low,0},
        {terrain.cellsX*terrain.cellWidth,high,terrain.cellsY*terrain.cellDepth},world);
    const float radius=(m_cachedLayer ? std::max(1.f,m_cachedLayer->viewDistance):std::max(2.f,visibleRadius))+2.f*std::max(1.f,tileSize);
    const float scale=glm::length(glm::vec3(glm::length(glm::vec3(world[0])),glm::length(glm::vec3(world[1])),glm::length(glm::vec3(world[2]))));
    bounds.minimum-=glm::vec3(radius*scale); bounds.maximum+=glm::vec3(radius*scale);
    return {Owner,m_cachedViewer,bounds};
}

void GrassField::Update() { Refresh(); }
void GrassField::OnDestroy() { ClearTiles(); }
bool GrassField::RefreshEditorPreview()
{
    if (!Owner || !Owner->GetScene() || Owner->GetScene()->HasStarted()) return false;
    return Refresh();
}

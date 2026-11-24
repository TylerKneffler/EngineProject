#include "Core/Rendering/DetailBudget.h"
#include "Grass/GrassField.h"
#include "Grass/GrassBlades.h"
#include "Grass/GrassStats.h"

#include "Core/Scene/Object.h"
#include "Core/Scene/Scene.h"
#include "Core/Rendering/RenderSystem.h"
#include "Core/Rendering/SceneRenderer.h"
#include "Core/Components/Terrain/Terrain.h"
#include "Core/Components/Terrain/TerrainManager.h"
#include "Core/Components/Terrain/TerrainObjectLayer.h"
#include "Core/Components/Terrain/TerrainSurfaceSample.h"
#include <cstdlib>
#include <glm/gtc/matrix_transform.hpp>
#include "Core/Components/Materials/Material.h"
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

// Triangle-list vertices of one species mesh, shared process-wide.
std::shared_ptr<const std::vector<Engine::Components::Mesh::Vertex>> LoadSpeciesSource(const std::string& path)
{
    using Source = std::vector<Engine::Components::Mesh::Vertex>;
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
                auto triangles = std::make_shared<Source>();
                triangles->reserve(mesh.GetIndices().size());
                for (const auto index : mesh.GetIndices()) triangles->push_back(mesh.GetVertices().at(index));
                return triangles;
            }
            return std::make_shared<Source>(mesh.TakeVertices());
        });
}

// Grass is a script asset, not an engine built-in: it registers itself when
// its project (or the engine sandbox) compiles Assets/Grass.
struct GrassFieldRegistration
{
    GrassFieldRegistration()
    {
        Engine::Serialization::RegisterComponentType<GrassField>("GrassField");
    }
};

GrassFieldRegistration g_registration;
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
    // Set by scene scripts (LandscapeDriver); serialized so test scenes can
    // match the landscape's fields.
    RegisterField("minimumRadiusScale", minimumRadiusScale, "Viewer");
    RegisterField("perPlantDetail", perPlantDetail, "LOD");
    RegisterField("fineReachFraction", fineReachFraction, "LOD");
    RegisterField("tileSize", tileSize, "Tiles");
    RegisterField("shortPatchesPerSquareMeter", shortPatchesPerSquareMeter, "Tiles");
    RegisterField("tallPatchesPerSquareMeter", tallPatchesPerSquareMeter, "Tiles");
    RegisterField("tallFieldDensity", tallFieldDensity, "Tall fields");
    RegisterField("tallFieldSize", tallFieldSize, "Tall fields");
    RegisterField("tallFieldCoverage", tallFieldCoverage, "Tall fields");
    RegisterField("shortUnderTallShare", shortUnderTallShare, "Tall fields");
    RegisterField("seed", seed, "Tiles");
    RegisterField("maxTileBuildsPerUpdate", maxTileBuildsPerUpdate, "Tiles");
    RegisterField("castShadows", castShadows, "Lighting");
    RegisterField("shaderPath", shaderPath, "Lighting");
    RegisterField("materialPath", materialPath, "Assets");
    RegisterField("windStrength", windStrength, "Lighting");
    RegisterField("tileWorkBudgetMs", tileWorkBudgetMs, "Tiles");
    RegisterField("fadeDuration", fadeDuration, "LOD");
    RegisterField("distanceFadeWidth", distanceFadeWidth, "LOD");
    RegisterField("distanceFadeFraction", distanceFadeFraction, "LOD");
    RegisterField("distanceThinning", distanceThinning, "LOD");
    RegisterField("innerRadius", innerRadius, "LOD");
    RegisterField("innerFadeWidth", innerFadeWidth, "LOD");
    RegisterField("groundTintStrength", groundTintStrength, "Ground");
    RegisterField("groundRootShade", groundRootShade, "Ground");
    RegisterField("gpuLodSelection", gpuLodSelection, "LOD");
    RegisterField("allowProceduralGeneration", allowProceduralGeneration, "Tiles");
    RegisterField("proceduralLayerMask", proceduralLayerMask, "Tiles");
    RegisterField("retainDormantTiles", retainDormantTiles, "Tiles");
    RegisterField("accentMeshes", accentMeshes, "Accents");
    RegisterField("accentPatchesPerSquareMeter", accentPatchesPerSquareMeter, "Accents");
    RegisterField("accentDriftSize", accentDriftSize, "Accents");
    RegisterField("shrubMeshes", shrubMeshes, "Shrubs");
    RegisterField("shrubPatchesPerSquareMeter", shrubPatchesPerSquareMeter, "Shrubs");
    RegisterField("shrubThicketSize", shrubThicketSize, "Shrubs");
    RegisterField("shrubCoverage", shrubCoverage, "Shrubs");
    RegisterField("bushMeshes", bushMeshes, "Bushes");
    RegisterField("bushesPerSquareMeter", bushesPerSquareMeter, "Bushes");
    RegisterField("bushClusterSize", bushClusterSize, "Bushes");
    RegisterField("bushCoverage", bushCoverage, "Bushes");
}

const std::vector<std::vector<std::string>>& GrassField::ResolveLodChains(
    const std::string& list, LodChains& cache) const
{
    if (cache.resolved && cache.source == list) return cache.chains;
    cache.resolved = true;
    cache.source = list;
    cache.chains.clear();
    const auto exists = [](const std::string& path) {
        std::error_code error;
        return std::filesystem::exists(Engine::Components::Mesh::ResolveFilePath(path), error);
    };
    size_t start = 0;
    while (start <= list.size())
    {
        size_t end = list.find_first_of(";,", start);
        if (end == std::string::npos) end = list.size();
        std::string path = list.substr(start, end - start);
        start = end + 1;
        const auto first = path.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) continue;
        path = path.substr(first, path.find_last_not_of(" \t\r\n") - first + 1);
        if (path == shortMesh || path == tallMesh) continue;
        if (!exists(path))
        {
            std::fprintf(stderr, "GrassField: mesh not found: %s\n", path.c_str());
            continue;
        }
        std::vector<std::string> chain{path};
        const auto dot = path.find_last_of('.');
        const std::string stem = dot == std::string::npos ? path : path.substr(0, dot);
        const std::string extension = dot == std::string::npos ? std::string{} : path.substr(dot);
        for (const char* suffix : {"_lod1", "_lod2"})
        {
            const std::string lod = stem + suffix + extension;
            if (!exists(lod)) break;
            chain.push_back(lod);
        }
        cache.chains.push_back(std::move(chain));
    }
    return cache.chains;
}

float GrassField::ShrubThicket(float x, float z) const
{
    return ThicketNoise(x, z, shrubThicketSize, shrubCoverage, 0u);
}

float GrassField::BushCluster(float x, float z) const
{
    return ThicketNoise(x, z, bushClusterSize, bushCoverage, 0x7c3e9a15u);
}

float GrassField::TallField(float x, float z) const
{
    return ThicketNoise(x, z, tallFieldSize, tallFieldCoverage, 0x51e7a3c9u);
}

float GrassField::ThicketNoise(float x, float z, float size, float coverage, uint32_t variant) const
{
    // Two octaves of value noise; thickets are where it rises over a
    // threshold chosen from shrubCoverage (value noise clusters around .5).
    const auto noise = [&](float period, uint32_t salt) {
        const float u = x / period, v = z / period;
        const float fx = std::floor(u), fz = std::floor(v);
        const int ix = static_cast<int>(fx), iz = static_cast<int>(fz);
        float tx = u - fx, tz = v - fz;
        tx = tx * tx * (3.f - 2.f * tx); tz = tz * tz * (3.f - 2.f * tz);
        // A full avalanche hash: TileSeed leaves axis-aligned stripes in
        // smooth noise over consecutive lattice points.
        const auto corner = [&](int dx, int dz) {
            uint32_t h = static_cast<uint32_t>(ix + dx) * 0x8da6b343u ^
                static_cast<uint32_t>(iz + dz) * 0xd8163841u ^ static_cast<uint32_t>(seed) ^ salt;
            h ^= h >> 16; h *= 0x7feb352du; h ^= h >> 15; h *= 0x846ca68bu; h ^= h >> 16;
            return static_cast<float>(h & 0xffffu) / 65535.f;
        };
        return (corner(0, 0) * (1.f - tx) + corner(1, 0) * tx) * (1.f - tz) +
            (corner(0, 1) * (1.f - tx) + corner(1, 1) * tx) * tz;
    };
    const float thicket = std::max(1.f, size);
    const float n = .68f * noise(thicket, 0x5b1d7a3u ^ variant) + .32f * noise(thicket * .37f, 0x2f6e2b1u ^ variant);
    const float threshold = .5f + (.5f - std::clamp(coverage, 0.f, 1.f)) * .5f;
    const float t = std::clamp((n - (threshold - .04f)) / .14f, 0.f, 1.f);
    return t * t * (3.f - 2.f * t);
}

size_t GrassField::SpeciesOf(const std::string& asset) const
{
    const auto found = m_speciesIndex.find(asset);
    return found == m_speciesIndex.end() ? 0 : found->second;
}

std::map<std::string, Engine::Components::ObjectLodProfileData>
GrassField::GetObjectLodProfiles() const
{
    std::map<std::string, Engine::Components::ObjectLodProfileData> definitions;
    std::vector<std::vector<std::string>> chains{
        {shortMesh, shortMeshLod1, shortMeshLod2}, {tallMesh, tallMeshLod1, tallMeshLod2}};
    const auto& accents = AccentLods();
    chains.insert(chains.end(), accents.begin(), accents.end());
    const auto& shrubs = ShrubLods();
    chains.insert(chains.end(), shrubs.begin(), shrubs.end());
    const auto& bushes = BushLods();
    chains.insert(chains.end(), bushes.begin(), bushes.end());
    for (const auto& paths : chains)
    {
        if (paths.front().empty()) continue;
        auto& profile = definitions[paths.front()];
        if (!profile.meshes.empty()) continue;
        for (const auto& path : paths)
            if (!path.empty()) profile.meshes.push_back(path);
    }
    return definitions;
}

// Runs every refresh of every field in range. Resolving the species lists
// builds maps and strings (1.2-1.6 ms a frame over the landscape's fields),
// so it is redone only when this field, its layer or the device changed.
bool GrassField::LoadSources()
{
    auto* provider = Owner->GetScene()->GetGraphicsProvider();
    const void* factory = provider ? provider->GetBufferFactory() : nullptr;
    const uint64_t layerRevision = m_cachedLayer ? m_cachedLayer->GetConfigurationRevision() : 0;
    // (Not reset when the scene's structure changes: every new grass tile
    // changes it, and a layer's species only change with its revision.)
    if (m_sourcesReady && m_sourcesFactory == factory && m_sourcesLayer == m_cachedLayer &&
        m_sourcesRevision == GetConfigurationRevision() && m_sourcesLayerRevision == layerRevision)
        return true;
    m_sourcesReady = ResolveSources();
    m_sourcesFactory = factory;
    m_sourcesLayer = m_cachedLayer;
    m_sourcesRevision = GetConfigurationRevision();
    m_sourcesLayerRevision = layerRevision;
    return m_sourcesReady;
}

bool GrassField::ResolveSources()
{
    auto* provider = Owner->GetScene()->GetGraphicsProvider();
    auto* factory = provider ? provider->GetBufferFactory() : nullptr;
    auto definitions = ObjectLodProvider::GetObjectLodMeshes();
    std::vector<std::string> assets{shortMesh, tallMesh};
    // Kind per species: 0 grass, 1 accent, 2 shrub, 3 bush. A path listed
    // twice keeps its first role.
    std::vector<int> kinds{0, 0};
    for (int kind = 1; kind <= 3; ++kind)
        for (const auto& chain : kind == 1 ? AccentLods() : kind == 2 ? ShrubLods() : BushLods())
            if (std::find(assets.begin(), assets.end(), chain.front()) == assets.end())
            {
                assets.push_back(chain.front());
                kinds.push_back(kind);
            }
    // Resolve only the rendered species here; enumerating every placement belongs
    // in the inspector, not in the per-frame streaming path.
    if (m_cachedLayer)
        for (const auto& asset : assets)
            if (const auto found = m_cachedLayer->objectLods.find(asset);
                found != m_cachedLayer->objectLods.end() && !found->second.meshes.empty())
            {
                auto meshes = found->second.meshes;
                meshes.erase(std::remove(meshes.begin(), meshes.end(), std::string{}), meshes.end());
                if (meshes.size() > Engine::Components::TerrainObjectLayer::MaximumObjectLods)
                    meshes.resize(Engine::Components::TerrainObjectLayer::MaximumObjectLods);
                if (!meshes.empty()) definitions[asset] = std::move(meshes);
            }
    std::vector<std::vector<std::string>> levels;
    for (const auto& asset : assets) levels.push_back(definitions[asset]);
    if (levels[0].empty() || levels[1].empty()) return false;
    for (size_t species = levels.size(); species-- > 2;)
        if (levels[species].empty())
        {
            levels.erase(levels.begin() + species);
            assets.erase(assets.begin() + species);
            kinds.erase(kinds.begin() + species);
        }
    m_accentSpecies = static_cast<size_t>(std::count(kinds.begin(), kinds.end(), 1));
    m_shrubSpecies = static_cast<size_t>(std::count(kinds.begin(), kinds.end(), 2));
    m_bushSpecies = static_cast<size_t>(std::count(kinds.begin(), kinds.end(), 3));
    const auto distances = [&](const std::string& asset, size_t count) {
        if (m_cachedLayer) return m_cachedLayer->GetObjectLodDistances(asset, count);
        std::vector<float> result;
        for (size_t i = 1; i < count; ++i) result.push_back(std::max(2.f, visibleRadius));
        return result;
    };
    std::vector<std::vector<float>> speciesDistances;
    for (size_t species = 0; species < assets.size(); ++species)
        speciesDistances.push_back(distances(assets[species], levels[species].size()));
    // A shared tile changes only the species whose boundary was crossed.
    // The sorted union supports different LOD counts and distances per asset.
    m_lodDistances.clear();
    for (const auto& thresholds : speciesDistances)
        m_lodDistances.insert(m_lodDistances.end(), thresholds.begin(), thresholds.end());
    std::sort(m_lodDistances.begin(), m_lodDistances.end());
    m_lodDistances.erase(std::unique(m_lodDistances.begin(), m_lodDistances.end()), m_lodDistances.end());
    m_useGpuLodSelection = (gpuLodSelection || (m_cachedLayer && m_cachedLayer->screenLodSelection)) &&
        m_lodDistances.size() == 2;
    for (size_t species = 0; species < assets.size(); ++species)
        m_useGpuLodSelection = m_useGpuLodSelection && levels[species].size() == 3 &&
            speciesDistances[species] == speciesDistances[0];
    std::vector<std::vector<std::string>> paths(assets.size());
    for (size_t i = 0; i <= m_lodDistances.size(); ++i)
        for (size_t species = 0; species < assets.size(); ++species)
        {
            const auto& thresholds = speciesDistances[species];
            const size_t level = i == 0 ? size_t(0) : size_t(std::upper_bound(thresholds.begin(),
                thresholds.end(), m_lodDistances[i - 1]) - thresholds.begin());
            paths[species].push_back(levels[species][level]);
        }
    const auto ready = [&] {
        return m_speciesVertices.size() >= 2 && m_speciesVertices[0][0] && m_speciesVertices[1][0] &&
            !m_speciesVertices[0][0]->empty() && !m_speciesVertices[1][0]->empty();
    };
    if (m_loaded && m_loadedFactory == factory && m_loadedSpeciesPaths == paths)
        return ready();
    m_loaded = true;
    m_loadedFactory = factory;
    m_hasGenerationSignature = false;
    const size_t lodCount = paths[0].size();
    m_speciesAssets = assets;
    m_speciesIndex.clear();
    for (size_t species = 0; species < assets.size(); ++species)
        m_speciesIndex.emplace(assets[species], species);
    m_speciesVertices.assign(assets.size(), std::vector<std::shared_ptr<const Source>>(lodCount));
    m_sourceMeshes.assign(lodCount, nullptr);
    m_loadedSpeciesPaths = paths;
    try
    {
        for (size_t species = 0; species < paths.size(); ++species)
            for (size_t lod = 0; lod < lodCount; ++lod)
                m_speciesVertices[species][lod] = LoadSpeciesSource(paths[species][lod]);
        // Weak ownership keeps GPU resources tied to their users/device instead
        // of retaining them forever in the process-wide CPU asset cache.
        using Mesh = Engine::Components::Mesh;
        static std::map<std::pair<const void*, std::vector<const void*>>, std::weak_ptr<Mesh>> sharedMeshes;
        for (auto it = sharedMeshes.begin(); it != sharedMeshes.end();)
            if (it->second.expired()) it = sharedMeshes.erase(it); else ++it;
        bool first = true;
        for (int lod = 0; lod < static_cast<int>(m_sourceMeshes.size()); ++lod)
        {
            std::pair<const void*, std::vector<const void*>> key{factory, {}};
            for (const auto& species : m_speciesVertices) key.second.push_back(species[lod].get());
            auto mesh = sharedMeshes[key].lock();
            if (!mesh)
            {
                Source vertices;
                for (const auto& species : m_speciesVertices)
                    vertices.insert(vertices.end(), species[lod]->begin(), species[lod]->end());
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
        for (auto& species : m_speciesVertices)
            for (auto& source : species) source.reset();
        for (auto& source : m_sourceMeshes) source.reset();
    }
    return ready();
}

void GrassField::Start()
{
    // Keep editor-warmed GPU geometry when entering Play.
    m_cachedViewer = nullptr;
    m_cachedStructureRevision = 0;
    EnsureAuthoredPreviewHidden();
    PublishGroundCover();
}

float GrassField::GrassRadius() const
{
    const auto* layer = Owner ? Owner->GetComponent<Engine::Components::TerrainObjectLayer>() : nullptr;
    return (layer ? std::max(1.f, layer->viewDistance) : std::max(2.f, visibleRadius)) *
        OuterRadiusScale();
}

void GrassField::PublishGroundCover()
{
    auto* terrain = Owner ? Owner->GetComponentInParent<Engine::Components::Terrain>() : nullptr;
    if (!terrain) return;
    auto& cover = terrain->groundCover;
    // A far ring (innerRadius) leaves the tint to the detailed field.
    if (innerRadius > 0.f) return;
    // Only procedural grass knows which paint layers it grows on.
    // A field without a tint leaves another field's cover alone: the forest
    // floor field (no tint) used to clear the meadow's cover on every refresh,
    // which left the bare terrain texture showing pale between the blades.
    if (groundTintStrength <= 0.f || proceduralLayerMask == 0 || !allowProceduralGeneration)
    {
        if (cover.owner == this) cover = {};
        return;
    }
    // Another field's live cover stays; a claim not renewed for two seconds
    // (its field was removed or disabled) can be taken over.
    const double now = std::chrono::duration<double>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    if (cover.owner && cover.owner != this && cover.strength > 0.f && now - cover.publishedAt < 2.0) return;
    // Mean colour of the grass species (short and tall at full detail),
    // measured once per mesh pair from the shared CPU sources.
    const std::string source = shortMesh + ";" + tallMesh;
    if (m_coverColorSource != source)
    {
        m_coverColorSource = source;
        glm::vec3 sum{0.f};
        size_t count = 0;
        try
        {
            for (const auto* path : {&shortMesh, &tallMesh})
                if (!path->empty())
                    if (const auto vertices = LoadSpeciesSource(*path))
                        for (const auto& vertex : *vertices)
                        {
                            sum += glm::vec3(vertex.color[0], vertex.color[1], vertex.color[2]);
                            ++count;
                        }
        }
        catch (const std::exception& error)
        {
            std::fprintf(stderr, "GrassField: unable to measure grass colour: %s\n", error.what());
        }
        m_coverColor = count ? sum / static_cast<float>(count) : glm::vec3(0.f);
    }
    if (glm::length(m_coverColor) <= 0.f) { if (cover.owner == this) cover = {}; return; }
    const float radius = GrassRadius();
    const float band = std::clamp(distanceFadeFraction, 0.f, .9f) * radius;
    const float scale = glm::length(glm::vec3(terrain->Owner->transform.GetWorldMatrix()[0]));
    // Upright blades shade each other and are seen edge-on, so a field looks
    // darker and duller than its blades' albedo on flat, sunlit ground. At
    // .8 the open ground past the blades measured 1.6x the meadow's
    // luminance in full sun (a bright band where the grass ends); .45 is level.
    const float luminance = glm::dot(m_coverColor, glm::vec3(.2126f, .7152f, .0722f));
    cover.color = glm::mix(m_coverColor, glm::vec3(luminance), .25f) * .45f;
    cover.strength = std::clamp(groundTintStrength, 0.f, 1.f);
    cover.rootShade = std::clamp(groundRootShade, 0.f, 1.f);
    // The ground takes the cover's colour where the plants fade out.
    cover.fadeStart = (radius - std::max(band, std::max(.5f, distanceFadeWidth))) * scale;
    cover.fadeEnd = radius * scale;
    cover.layerMask = static_cast<uint32_t>(proceduralLayerMask);
    cover.owner = this;
    cover.publishedAt = now;
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
    if (Owner && Owner->GetScene())
        for (auto& tile : m_freeTiles)
            if (tile.object) Owner->GetScene()->RequestRemoveObject(tile.object);
    m_freeTiles.clear();
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
    HashBytes(hash, accentMeshes.data(), accentMeshes.size());
    HashFloat(hash, accentPatchesPerSquareMeter);
    HashFloat(hash, accentDriftSize);
    HashBytes(hash, shrubMeshes.data(), shrubMeshes.size());
    HashFloat(hash, shrubPatchesPerSquareMeter);
    HashFloat(hash, shrubThicketSize);
    HashFloat(hash, shrubCoverage);
    HashBytes(hash, bushMeshes.data(), bushMeshes.size());
    HashFloat(hash, bushesPerSquareMeter);
    HashFloat(hash, bushClusterSize);
    HashFloat(hash, bushCoverage);
    HashFloat(hash, tileSize);
    HashFloat(hash, fineReachFraction);
    HashFloat(hash, gpuBlades ? 1.f : 0.f);
    HashFloat(hash, shortPatchesPerSquareMeter);
    HashFloat(hash, tallPatchesPerSquareMeter);
    HashFloat(hash, tallFieldDensity);
    HashFloat(hash, tallFieldSize);
    HashFloat(hash, tallFieldCoverage);
    HashFloat(hash, shortUnderTallShare);
    HashBytes(hash, &seed, sizeof(seed));
    HashBytes(hash,&allowProceduralGeneration,sizeof(allowProceduralGeneration));
    HashBytes(hash,&proceduralLayerMask,sizeof(proceduralLayerMask));
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
    const size_t speciesCount = std::max<size_t>(2, m_speciesAssets.size());
    job.speciesCounts.assign(speciesCount, 0);
    if (m_placementCount && m_placementTiles)
    {
        const auto found=m_placementTiles->find(tile);
        if(found!=m_placementTiles->end())
        {
            const auto& prepared=found->second;
            // Prepared transforms are tile-relative in x/z only; the tile
            // object also sits at its ground height (TileState::originHeight).
            const float span=std::max(1.f,tileSize);
            const auto tileState=m_tiles.find(tile);
            const float originHeight=tileState!=m_tiles.end() ? tileState->second.originHeight
                : SampleHeight((tile.first+.5f)*span,(tile.second+.5f)*span);
            std::vector<glm::mat4> transforms;
            std::vector<uint64_t> placementIds;
            size_t placementCount=0;
            for (const auto& [asset, records] : prepared.groups)
                placementCount += records.size();
            transforms.reserve(placementCount);
            placementIds.reserve(placementCount);
            // Group instances by species so each source vertex range is drawn once.
            for(size_t species=0;species<speciesCount;++species)
                for(const auto& [asset,records]:prepared.groups) if(SpeciesOf(asset)==species)
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
                    transform[3][1] -= originHeight;
                    transforms.push_back(transform);
                    placementIds.push_back(m_cachedLayer->placements[record.index].id);
                    ++job.speciesCounts[species];
                }
            job.instanceLayer.SetPlacements(std::move(transforms),
                std::move(placementIds));
            const float radius=glm::length(glm::max(glm::abs(m_sourceMinimum),glm::abs(m_sourceMaximum)))*prepared.maximumScale;
            const glm::vec3 origin(0.f,originHeight,0.f);
            job.minimum=prepared.minimum-origin-glm::vec3(radius); job.maximum=prepared.maximum-origin+glm::vec3(radius);
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
        // Paint weight of the masked layers at the nearest terrain vertex.
        const auto& terrain = *m_cachedTerrain;
        const size_t layerCount = terrain.layers.size();
        const bool masked = proceduralLayerMask != 0 && layerCount > 0 &&
            terrain.weights.size() == terrain.heights.size() * layerCount;
        // Paths shaded per pixel (TerrainManager::ContourPaths) are not in the
        // paint: keep off their track as the painted path did (it took the
        // grass weight under .5 from a path share of about .45).
        const auto* paths = terrain.Owner
            ? terrain.Owner->GetComponentInParent<Engine::Components::TerrainManager>() : nullptr;
        if (paths && !paths->contourPaths.enabled) paths = nullptr;
        const glm::vec3 pathOrigin = paths
            ? glm::vec3(terrain.Owner->transform.GetWorldMatrix()[3]) : glm::vec3(0.f);
        const auto grows = [&](float x, float z)
        {
            if (paths)
            {
                float height = 0.f;
                if (Engine::Components::SampleTerrainSurface(terrain.heights, terrain.cellsX,
                        terrain.cellsY, terrain.cellWidth, terrain.cellDepth, x, z, height, nullptr) &&
                    paths->ContourPathShare(pathOrigin.x + x, pathOrigin.z + z,
                        pathOrigin.y + height) > .45f) return false;
            }
            if (!masked) return true;
            const int col = std::clamp(static_cast<int>(std::lround(x / terrain.cellWidth)), 0, terrain.cellsX);
            const int row = std::clamp(static_cast<int>(std::lround(z / terrain.cellDepth)), 0, terrain.cellsY);
            const float* sample = &terrain.weights[(static_cast<size_t>(row) *
                (terrain.cellsX + 1) + col) * layerCount];
            float weight = 0.f;
            for (size_t layer = 0; layer < layerCount && layer < 31; ++layer)
                if (proceduralLayerMask & (1 << layer)) weight += sample[layer];
            return weight >= .5f;
        };
        const size_t accentCount = m_accentSpecies;
        const size_t shrubCount = m_shrubSpecies;
        const bool thickets = shrubCount && shrubPatchesPerSquareMeter > 0.f;
        const size_t bushCount = m_bushSpecies;
        const bool bushClusters = bushCount && bushesPerSquareMeter > 0.f;
        const bool tallFields = tallFieldDensity > 0.f;
        // thinInFields: the short patches give way to tall fields; inField:
        // the tall fields' own placements, kept where the field is in.
        const auto append = [&](const std::string& asset, float density, int cap,
            bool thinInFields = false, bool inField = false)
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
                // Grass thins under shrub thickets (the extra random is drawn
                // only with thickets on, so other layouts stay unchanged).
                if (thickets && unit(rng) < .4f * ShrubThicket(p.position.x, p.position.z)) continue;
                // Bushes shade out more of the grass beneath them.
                if (bushClusters && unit(rng) < .5f * BushCluster(p.position.x, p.position.z)) continue;
                // Extra randoms only with fields on, so other layouts stay unchanged.
                if (thinInFields && tallFields &&
                    unit(rng) < (1.f - shortUnderTallShare) * TallField(p.position.x, p.position.z)) continue;
                if (inField && unit(rng) >= TallField(p.position.x, p.position.z)) continue;
                if (!grows(p.position.x, p.position.z)) continue;
                p.asset = asset;
                p.position.y = SampleHeight(p.position.x, p.position.z, &p.normal);
                p.yawDegrees = unit(rng) * 360.f;
                p.scale = .8f + .4f * unit(rng);
                job.placements.push_back(std::move(p));
            }
        };
        // Caps are per 4 m tile; larger tiles (far rings) scale them.
        const float areaScale = std::max(1.f, span * span / 16.f);
        // Blade mode: the fine grass is generated on the GPU (GrassBlades).
        if (!gpuBlades)
        {
            append(shortMesh, shortPatchesPerSquareMeter, static_cast<int>(64 * areaScale), true);
            append(tallMesh, tallPatchesPerSquareMeter, static_cast<int>(32 * areaScale));
            if (tallFields)
                append(tallMesh, tallFieldDensity, static_cast<int>(48 * areaScale), false, true);
        }
        // Accents grow in drifts. Each world cell of accentDriftSize favours
        // one species and has its own richness (some cells stay bare), so
        // flowers gather in patches instead of an even sprinkle. Cells are
        // keyed in terrain space, so drifts continue across tile borders.
        if (accentCount && accentPatchesPerSquareMeter > 0.f)
        {
            // Richness is uniform in [-.3, 1.5]: mean acceptance 1/1.8.
            const int count = std::clamp(static_cast<int>(std::round(
                accentPatchesPerSquareMeter * span * span * 1.8f)), 0, 96);
            const float drift = std::max(1.f, accentDriftSize);
            for (int i = 0; i < count; ++i)
            {
                Placement p;
                p.position.x = tile.first * span + unit(rng) * span;
                p.position.z = tile.second * span + unit(rng) * span;
                const float accept = unit(rng), pick = unit(rng);
                p.yawDegrees = unit(rng) * 360.f;
                p.scale = .85f + .3f * unit(rng);
                if (p.position.x < .3f || p.position.z < .3f ||
                    p.position.x > width - .3f || p.position.z > depth - .3f) continue;
                const uint32_t cell = TileSeed(seed ^ 0x2c1b3c6d,
                    static_cast<int>(std::floor(p.position.x / drift)),
                    static_cast<int>(std::floor(p.position.z / drift)));
                const float richness = static_cast<float>(cell & 1023u) / 1023.f * 1.8f - .3f;
                if (accept >= richness) continue;
                if (!grows(p.position.x, p.position.z)) continue;
                const size_t favoured = (cell >> 10u) % accentCount;
                const size_t species = pick < .7f ? favoured :
                    std::min(accentCount - 1, static_cast<size_t>((pick - .7f) / .3f * accentCount));
                p.asset = m_speciesAssets[2 + species];
                p.position.y = SampleHeight(p.position.x, p.position.z, &p.normal);
                job.placements.push_back(std::move(p));
            }
        }
        // Shrubs gather in thickets keyed in terrain space (so they continue
        // across tiles). Each thicket cell favours one species; density
        // rises from the thicket rim to its heart, so they close over the
        // ground there and thin out into the meadow at the edges.
        if (thickets)
        {
            // The cap is per 8 m square of tile: a fixed 128 halved the
            // forest floor's ferns once tiles grew to 16 m.
            const int cap = static_cast<int>(128.f * std::max(1.f, span * span / 64.f));
            const int count = std::clamp(static_cast<int>(std::round(
                shrubPatchesPerSquareMeter * span * span)), 0, cap);
            const float thicket = std::max(1.f, shrubThicketSize);
            for (int i = 0; i < count; ++i)
            {
                Placement p;
                p.position.x = tile.first * span + unit(rng) * span;
                p.position.z = tile.second * span + unit(rng) * span;
                const float accept = unit(rng), pick = unit(rng);
                p.yawDegrees = unit(rng) * 360.f;
                p.scale = .7f + .6f * unit(rng);
                if (p.position.x < .5f || p.position.z < .5f ||
                    p.position.x > width - .5f || p.position.z > depth - .5f) continue;
                if (accept >= ShrubThicket(p.position.x, p.position.z)) continue;
                if (!grows(p.position.x, p.position.z)) continue;
                const uint32_t cell = TileSeed(seed ^ 0x6b43a9b5,
                    static_cast<int>(std::floor(p.position.x / thicket)),
                    static_cast<int>(std::floor(p.position.z / thicket)));
                const size_t favoured = cell % shrubCount;
                const size_t species = pick < .75f ? favoured :
                    std::min(shrubCount - 1, static_cast<size_t>((pick - .75f) / .25f * shrubCount));
                p.asset = m_speciesAssets[2 + accentCount + species];
                p.position.y = SampleHeight(p.position.x, p.position.z, &p.normal);
                // Woody plants grow mostly upright on slopes (a conforming
                // layer already keeps them upright and shears only the base)
                // and sit a little into the ground so no gap shows under
                // their lowest leaves.
                if (!m_cachedLayer || !m_cachedLayer->conformToGround)
                    p.normal = glm::normalize(glm::mix(p.normal, glm::vec3(0.f, 1.f, 0.f), .6f));
                p.position.y -= .03f * p.scale;
                job.placements.push_back(std::move(p));
            }
        }
        // Bushes: loose clusters (cluster noise keyed in terrain space, so
        // they continue across tiles) plus a few scattered singles. Each
        // cluster cell favours one species.
        if (bushClusters)
        {
            const int cap = static_cast<int>(64.f * std::max(1.f, span * span / 64.f));
            const int count = std::clamp(static_cast<int>(std::round(
                bushesPerSquareMeter * span * span)), 0, cap);
            const float cluster = std::max(1.f, bushClusterSize);
            for (int i = 0; i < count; ++i)
            {
                Placement p;
                p.position.x = tile.first * span + unit(rng) * span;
                p.position.z = tile.second * span + unit(rng) * span;
                const float accept = unit(rng), pick = unit(rng);
                p.yawDegrees = unit(rng) * 360.f;
                p.scale = .75f + .5f * unit(rng);
                if (p.position.x < .8f || p.position.z < .8f ||
                    p.position.x > width - .8f || p.position.z > depth - .8f) continue;
                if (accept >= .08f + .92f * BushCluster(p.position.x, p.position.z)) continue;
                if (!grows(p.position.x, p.position.z)) continue;
                const uint32_t cell = TileSeed(seed ^ 0x3a8f05c5,
                    static_cast<int>(std::floor(p.position.x / cluster)),
                    static_cast<int>(std::floor(p.position.z / cluster)));
                const size_t favoured = cell % bushCount;
                const size_t species = pick < .7f ? favoured :
                    std::min(bushCount - 1, static_cast<size_t>((pick - .7f) / .3f * bushCount));
                p.asset = m_speciesAssets[2 + accentCount + shrubCount + species];
                p.position.y = SampleHeight(p.position.x, p.position.z, &p.normal);
                // Bushes stand upright and sit a little into the ground.
                if (!m_cachedLayer || !m_cachedLayer->conformToGround)
                    p.normal = glm::normalize(glm::mix(p.normal, glm::vec3(0.f, 1.f, 0.f), .8f));
                p.position.y -= .05f * p.scale;
                job.placements.push_back(std::move(p));
            }
        }
    }
    job.totalCount=uint32_t(job.placements.size());
    // Procedural opt-in data still needs preparation; saved layers use baked records.
    // Group instances by species so each source vertex range is drawn once.
    std::stable_sort(job.placements.begin(), job.placements.end(),
        [&](const Placement& a, const Placement& b) { return SpeciesOf(a.asset) < SpeciesOf(b.asset); });
    for (const auto& p : job.placements) ++job.speciesCounts[SpeciesOf(p.asset)];
    job.instanceLayer.Reserve(job.placements.size());
}

bool GrassField::AdvanceBuild(Clock::time_point deadline)
{
    auto& job = *m_build;
    const auto generated = Clock::now();
    const float span = std::max(1.f, tileSize);
    // Placements are relative to the tile object, which sits at the tile's
    // ground height (TileState::originHeight).
    const auto tileState = m_tiles.find(job.tile);
    const float originHeight = tileState != m_tiles.end() ? tileState->second.originHeight
        : SampleHeight((job.tile.first + .5f) * span, (job.tile.second + .5f) * span);
    while (!job.prepared && job.placement < job.placements.size())
    {
        const size_t placementIndex = job.placement++;
        const auto& p = job.placements[placementIndex];
        glm::mat4 transform = m_cachedLayer
            ? m_cachedLayer->PlacementTransform(p)
            : glm::translate(glm::mat4(1.f), p.position) *
                glm::mat4_cast(Engine::Components::TerrainObjectLayer::SurfaceRotation(p.normal, p.yawDegrees)) *
                glm::scale(glm::mat4(1.f), glm::vec3(p.scale));
        transform[3] -= glm::vec4(job.tile.first * span, originHeight, job.tile.second * span, 0.f);
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
        std::vector<Mesh::InstanceRange> ranges;
        uint32_t firstVertex = 0, firstInstance = 0;
        for (size_t species = 0; species < m_speciesVertices.size(); ++species)
        {
            const auto vertexCount = static_cast<uint32_t>(m_speciesVertices[species][lod]->size());
            const uint32_t instanceCount = species < job.speciesCounts.size() ? job.speciesCounts[species] : 0;
            // Species order: short, tall, accents, then shrubs and bushes.
            // Shrubs and bushes keep their full reach and are not thinned:
            // they are large enough that a plant growing back in at mid
            // range reads as it rising out of the ground.
            const bool fine = species < 2 + m_accentSpecies;
            ranges.push_back({firstVertex, vertexCount, firstInstance, instanceCount,
                fine ? std::clamp(fineReachFraction, .05f, 1.f) : 1.f, fine});
            firstVertex += vertexCount;
            firstInstance += instanceCount;
        }
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
    // ENGINE_GRASS_LOD_FADE_SECONDS overrides the tile LOD cross-fade (A/B runs).
    static const float fadeOverride = [] {
        const char* value = std::getenv("ENGINE_GRASS_LOD_FADE_SECONDS");
        return value ? static_cast<float>(std::atof(value)) : -1.f; }();
    if (!tile.mesh->BeginRuntimeLodTransition(lod, std::max(0.f, fadeOverride >= 0.f ? fadeOverride : fadeDuration)))
        return false;
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
    // Grass statistics and the shared tile-work budget (GrassStats) only
    // apply to drawn scenes; headless scenes never reset them.
    auto* stats = Engine::Rendering::RenderSystem::Find(scene)
        ? &Engine::Grass::GrassStats::For(scene) : nullptr;
    auto phaseStarted = started;
    const auto checkpoint = [&](double& milliseconds) {
        const auto now = Clock::now();
        milliseconds += std::chrono::duration<double, std::milli>(now-phaseStarted).count();
        phaseStarted = now;
    };
    const auto publish = [&] {
        checkpoint(m_performance.otherMs);
        m_performance.totalMs = std::chrono::duration<double, std::milli>(phaseStarted-started).count();
        if (stats) stats->Add(m_performance);
    };
    const float delta = std::clamp(scene.HasStarted() ? scene.GetDeltaTime() : elapsed, 0.f, .1f);
    bool changed = EnsureAuthoredPreviewHidden();
    PublishGroundCover();
    const bool referencesReady = ResolveCachedReferences();
    checkpoint(m_performance.referencesMs);
    if (!referencesReady) { publish(); return changed; }
    if (gpuBlades) SyncGpuBlades();
    // The renderer cannot draw blades (GrassBlades::Update): mesh grass again.
    if (gpuBlades && !GrassBlades::Enabled())
    {
        gpuBlades = false;
        ReleaseGpuBlades();
        MarkConfigurationDirty();
    }
    const glm::vec3 local = glm::vec3(glm::inverse(
        m_cachedTerrain->Owner->transform.GetWorldMatrix()) *
        glm::vec4(m_cachedViewer->transform.GetWorldPosition(), 1.f));
    const auto* layer = m_cachedLayer;
    const float span = std::max(1.f, tileSize);
    const float radius = (layer ? std::max(1.f, layer->viewDistance) : std::max(2.f, visibleRadius)) *
        OuterRadiusScale();
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
        // ENGINE_GRASS_LOG=1: every regeneration (all tiles cleared and faded in again).
        static const bool log = std::getenv("ENGINE_GRASS_LOG") != nullptr;
        if (log && m_hasGenerationSignature)
            std::printf("[GrassField] %s regenerates: %zu tiles, terrain revision %llu\n",
                Owner ? Owner->name.c_str() : "?", m_tiles.size(),
                m_cachedTerrain ? static_cast<unsigned long long>(m_cachedTerrain->GetConfigurationRevision()) : 0ull);
        ClearTiles();
        m_lastGenerationSignature = signature;
        m_hasGenerationSignature = true;
    }
    checkpoint(m_performance.placementMs);
    const float lod1 = m_lodDistances.empty() ? radius : m_lodDistances.front();
    const float lod2 = m_lodDistances.size() < 2 ? radius : m_lodDistances[1];
    const float hysteresis = layer ? std::max(0.f, layer->lodHysteresis) : 0;
    const float shadowRadius = layer ? std::clamp(layer->shadowDistance, 0.f, radius) : radius;
    // Per-placement shrink fade (Object.hlsl VSMain) in world distances; the
    // terrain may be scaled.
    const float fadeBand = std::clamp(distanceFadeFraction, 0.f, .9f) * radius;
    const bool placementFade = fadeBand > 0.f;
    const float worldScale = glm::length(glm::vec3(m_cachedTerrain->Owner->transform.GetWorldMatrix()[0]));
    const int reach = static_cast<int>(std::ceil(radius / span)) + 1;
    const int centerX = static_cast<int>(std::floor(local.x / span));
    const int centerZ = static_cast<int>(std::floor(local.z / span));
    const int maxX = static_cast<int>(std::ceil(m_cachedTerrain->cellsX * m_cachedTerrain->cellWidth / span));
    const int maxZ = static_cast<int>(std::ceil(m_cachedTerrain->cellsY * m_cachedTerrain->cellDepth / span));
    // Tiles wanted this refresh carry the serial; no per-frame set of keys.
    const uint64_t refresh = ++m_refreshSerial;
    for (int z = std::max(0, centerZ - reach); z < std::min(maxZ, centerZ + reach + 1); ++z)
        for (int x = std::max(0, centerX - reach); x < std::min(maxX, centerX + reach + 1); ++x)
        {
            const float dx = (x + .5f) * span - local.x;
            const float dz = (z + .5f) * span - local.z;
            const float edgeX = std::max(0.f, std::abs(dx) - span * .5f);
            const float edgeZ = std::max(0.f, std::abs(dz) - span * .5f);
            if (edgeX * edgeX + edgeZ * edgeZ > radius * radius) continue;
            // Far ring: skip tiles wholly inside the inner radius.
            if (innerRadius > 0.f)
            {
                // The inner edge follows the detailed field, whose radius the
                // detail budget scales.
                const float inner = innerRadius * DetailRadiusScale();
                const float farX = std::abs(dx) + span * .5f, farZ = std::abs(dz) + span * .5f;
                if (farX * farX + farZ * farZ < inner * inner) continue;
            }
            if (m_placementCount && !m_placementTiles->count({x,z})) continue;
            const Tile key{x,z};
            auto& tile = m_tiles[key];
            tile.desiredRefresh = refresh;
            if (!tile.object && !m_freeTiles.empty())
            {
                // Reuse a retired tile object: no scene structure change.
                tile = m_freeTiles.back();
                m_freeTiles.pop_back();
                tile.desiredRefresh = refresh;
                tile.desiredLod = 0;
                tile.castsShadows = false;
                tile.distanceSquared = tile.visibility = tile.appearance = 0.f;
                tile.originHeight = SampleHeight((x + .5f) * span, (z + .5f) * span);
                tile.object->transform.position = {x * span, tile.originHeight, z * span};
                changed = true;
            }
            if (!tile.object)
            {
                char name[64];
                std::snprintf(name, sizeof(name), "Grass Tile (%d, %d)", x, z);
                tile.object = scene.AddObject(name);
                if (Owner) tile.object->layer = Owner->layer;
                tile.object->runtimeOnly = true;
                tile.object->enabled = false;
                scene.SetParentLocal(tile.object, Owner);
                tile.originHeight = SampleHeight((x + .5f) * span, (z + .5f) * span);
                tile.object->transform.position = {x * span, tile.originHeight, z * span};
                tile.mesh = tile.object->AddComponent<Engine::Components::Mesh>();
                tile.mesh->useBoundsForShadowCulling = true;
                tile.mesh->usesGrassShadowBudget = true;
                tile.mesh->SetRuntimeVisibility(0.f);
                auto* material = tile.object->AddComponent<Engine::Components::Material>();
                tile.material = material;
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
            // With the placement fade each plant fades itself; a tile-wide
            // dither would show the 4 m tile squares at the edge.
            const float targetVisibility = placementFade ? 1.f : distanceFadeWidth > 0.f
                ? std::clamp((radius - edgeDistance) / distanceFadeWidth, 0.f, 1.f) : 1.f;
            tile.mesh->placementFadeStart = placementFade ? (radius - fadeBand) * worldScale : 0.f;
            tile.mesh->placementFadeEnd = placementFade ? radius * worldScale : 0.f;
            if (innerRadius > 0.f)
            {
                // Fade in: none kept at the inner radius, all by
                // innerFadeWidth beyond it, without widening.
                const float innerScale = DetailRadiusScale();
                tile.mesh->placementThinStart = (innerRadius + innerFadeWidth) * innerScale * worldScale;
                tile.mesh->placementThinEnd = innerRadius * innerScale * worldScale;
                tile.mesh->placementThinMinimum = 0.f;
                tile.mesh->placementThinKeepWidth = true;
            }
            else
            {
                tile.mesh->placementThinStart = radius * .2f * worldScale;
                tile.mesh->placementThinEnd = std::max(radius * .25f, radius - fadeBand) * worldScale;
                tile.mesh->placementThinMinimum = std::clamp(distanceThinning, .1f, 1.f);
                tile.mesh->placementThinKeepWidth = false;
            }
            const float previousVisibility = tile.visibility;
            if (tile.mesh->GetActiveRuntimeLod() >= 0)
            {
                // ENGINE_GRASS_LOG=1: where each tile starts to fade in, against the range.
                static const bool log = std::getenv("ENGINE_GRASS_LOG") != nullptr;
                if (log && tile.appearance <= 0.f)
                    std::printf("[GrassField] f%llu %s tile (%d,%d) appears: nearest edge %.1f m, radius %.1f m, "
                        "centre %.1f m\n", static_cast<unsigned long long>(scene.GetFrameIndex()),
                        Owner ? Owner->name.c_str() : "?", x, z, edgeDistance, radius,
                        std::sqrt(dx * dx + dz * dz));
                // ENGINE_GRASS_APPEAR_SECONDS overrides the tile fade-in (A/B runs).
                static const float appearOverride = [] {
                    const char* value = std::getenv("ENGINE_GRASS_APPEAR_SECONDS");
                    return value ? static_cast<float>(std::atof(value)) : -1.f; }();
                const float appear = appearOverride >= 0.f ? appearOverride : fadeDuration;
                const float step = appear > 0.f ? delta / appear : 1.f;
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
            tile.mesh->perPlacementLod=perPlantDetail && m_useGpuLodSelection;
            // Keep caster eligibility stable. The shadow shader fades each
            // surface at the range edge; compute culls distant placements.
            const bool shadows = castShadows && shadowRadius > 0;
            tile.mesh->runtimeShadowDistance=shadowRadius;
            tile.mesh->runtimeShadowFadeWidth=std::min(shadowRadius,std::max(.25f,distanceFadeWidth));
            tile.castsShadows = shadows;
            auto* tileMaterial = tile.material;
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
            if (tileMaterial->shader != shaderPath) tileMaterial->shader = shaderPath;
            changed = ActivateTileLod(tile, tile.desiredLod) || changed;
            tile.object->enabled = tile.mesh->GetActiveRuntimeLod() >= 0 && tile.visibility > 0.f;
        }
    checkpoint(m_performance.visibilityMs);
    for (auto it = m_tiles.begin(); it != m_tiles.end();)
    {
        if (it->second.desiredRefresh == refresh) { ++it; continue; }
        const float edgeX = std::max(0.f, std::abs((it->first.first + .5f) * span - local.x) - span * .5f);
        const float edgeZ = std::max(0.f, std::abs((it->first.second + .5f) * span - local.z) - span * .5f);
        if (edgeX * edgeX + edgeZ * edgeZ <= retentionRadius * retentionRadius)
        {
            // ENGINE_GRASS_LOG=1: a tile in reach goes dormant (its fade restarts if wanted again).
            static const bool logDormant = std::getenv("ENGINE_GRASS_LOG") != nullptr;
            if (logDormant && it->second.appearance > 0.f)
                std::printf("[GrassField] f%llu %s tile (%d,%d) dormant: nearest edge %.1f m, radius %.1f m\n",
                    static_cast<unsigned long long>(scene.GetFrameIndex()), Owner ? Owner->name.c_str() : "?",
                    it->first.first, it->first.second, std::sqrt(edgeX * edgeX + edgeZ * edgeZ), radius);
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
            // Emptied and kept for reuse; removed only if it cannot be emptied.
            it->second.visibility = it->second.appearance = 0.f;
            it->second.mesh->SetRuntimeVisibility(0.f);
            // ENGINE_GRASS_TILE_POOL=0 removes the object instead (for A/B runs).
            static const bool pooled = []
            {
                const char* setting = std::getenv("ENGINE_GRASS_TILE_POOL");
                return !(setting && setting[0] == '0');
            }();
            // The tile's meshes and placement buffers may still be drawn by
            // the frame in flight: they leave on a carrier object that the
            // scene releases behind the frame fence, as it would the tile
            // object itself had it been removed.
            auto carrier = pooled ? std::make_unique<Engine::Core::Object>() : nullptr;
            if (carrier && it->second.mesh->ResetRuntimeLods(
                    carrier->AddComponent<Engine::Components::Mesh>()))
            {
                scene.RetireRenderObject(std::move(carrier));
                m_freeTiles.push_back(it->second);
            }
            else scene.RequestRemoveObject(it->second.object);
            it = m_tiles.erase(it);
        }
        changed = true;
    }
    checkpoint(m_performance.retirementMs);
    // Prioritize nearby missing tiles. Preparing placements installs all three
    // shared-source LODs together, so resident tiles require no further builds.
    // Several tiles per refresh, nearest first, within maxTileBuildsPerUpdate
    // and the shared work budget. One per refresh left ~50 tiles waiting
    // while walking the landscape (128 before the far ring was trimmed):
    // grass and ferns appeared late, inside the radius, after their fade
    // distance had passed. A tile costs ~0.01-0.15 ms to build.
    const auto selectNearest = [&](Tile& selected, int& selectedLod, bool& urgent, bool count)
    {
        TileState* nearest = nullptr;
        selectedLod = -1; urgent = false;
        for (auto& [key, tile] : m_tiles)
        {
            if (tile.desiredRefresh != refresh) continue;
            int missing = !tile.mesh->IsRuntimeLodResident(tile.desiredLod) ? tile.desiredLod : -1;
            const bool required = missing >= 0;
            if (missing < 0)
                for (int lod = 0; lod < static_cast<int>(m_sourceMeshes.size()); ++lod)
                    if (!tile.mesh->IsRuntimeLodResident(lod)) { missing = lod; break; }
            if (missing < 0) continue;
            if (count) ++m_performance.pending;
            if (!nearest || (required && !urgent) || (required == urgent &&
                tile.distanceSquared < nearest->distanceSquared))
            { nearest = &tile; selected = key; selectedLod = missing; urgent = required; }
        }
        return nearest;
    };
    const int maximumBuilds = std::clamp(maxTileBuildsPerUpdate, 1, 64);
    for (int builds = 0; builds < maximumBuilds; ++builds)
    {
        Tile selected{};
        int selectedLod = -1;
        bool urgent = false;
        TileState* nearest = selectNearest(selected, selectedLod, urgent, builds == 0);
        if (m_build)
        {
            const auto& tile = m_tiles.at(m_build->tile);
            if (m_build->signature != signature || (urgent &&
                m_build->lod != tile.desiredLod && tile.mesh->IsRuntimeLodResident(tile.desiredLod)))
                m_build.reset();
        }
        const double spent = (stats ? stats->Get().generationMs + stats->Get().uploadMs : 0.) +
            m_performance.generationMs + m_performance.uploadMs;
        const double remaining = std::max(0., std::clamp(double(tileWorkBudgetMs), .1, 16.) - spent);
        if (!m_build && nearest && remaining > 0.) BeginBuild(selected, selectedLod, signature);
        if (builds == 0) checkpoint(m_performance.schedulingMs);
        if (!m_build || remaining <= 0.) break;
        const auto deadline = Clock::now() + std::chrono::duration_cast<Clock::duration>(
            std::chrono::duration<double, std::milli>(remaining));
        const Tile key = m_build->tile;
        if (!AdvanceBuild(deadline)) break;      // out of budget: continues next refresh
        auto& tile = m_tiles.at(key);
        // Keep current geometry until the desired replacement is resident.
        changed = ActivateTileLod(tile, tile.desiredLod) || changed;
        if (tile.mesh->GetActiveRuntimeLod() < 0)
            for (int lod = 0; lod < static_cast<int>(m_sourceMeshes.size()); ++lod)
                if (tile.mesh->IsRuntimeLodResident(lod)) { changed = ActivateTileLod(tile, lod) || changed; break; }
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
                if (stats && stats->AccountGpuResource(slice.buffer.get()))
                    m_performance.gpuBytes += slice.buffer->GetSize();
        }
    }
    for (const auto& source : m_sourceMeshes)
        if (source && stats && stats->AccountGpuResource(source->GetGraphicsBuffer()))
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
    // Cells outside the spatial region never refresh; their ground still
    // takes the cover colour.
    PublishGroundCover();
    if(!ResolveCachedReferences()) return {};
    const auto& terrain=*m_cachedTerrain;
    const auto world=terrain.Owner->transform.GetWorldMatrix();
    float low=0,high=0;
    if(!terrain.heights.empty()) { const auto range=std::minmax_element(terrain.heights.begin(),terrain.heights.end()); low=*range.first; high=*range.second; }
    auto bounds=Engine::Rendering::TransformBounds({0,low,0},
        {terrain.cellsX*terrain.cellWidth,high,terrain.cellsY*terrain.cellDepth},world);
    const float radius=(m_cachedLayer ? std::max(1.f,m_cachedLayer->viewDistance):std::max(2.f,visibleRadius))*OuterRadiusScale()+2.f*std::max(1.f,tileSize);
    const float scale=glm::length(glm::vec3(glm::length(glm::vec3(world[0])),glm::length(glm::vec3(world[1])),glm::length(glm::vec3(world[2]))));
    bounds.minimum-=glm::vec3(radius*scale); bounds.maximum+=glm::vec3(radius*scale);
    return {Owner,m_cachedViewer,bounds};
}

void GrassField::Update() { Refresh(); }
void GrassField::OnDestroy() { ClearTiles(); ReleaseGpuBlades(); }

void GrassField::SyncGpuBlades()
{
    auto* scene = Owner ? Owner->GetScene() : nullptr;
    if (!scene || !m_cachedTerrain || !GrassBlades::Enabled()) return;
    auto blades = GrassBlades::For(*scene);
    const auto& terrain = *m_cachedTerrain;
    const uint64_t revision = terrain.GetConfigurationRevision();
    if (m_bladeTerrain != &terrain || m_bladeTerrainRevision != revision)
    {
        const int samplesX = terrain.cellsX + 1, samplesZ = terrain.cellsY + 1;
        const size_t layerCount = terrain.layers.size();
        if (samplesX == samplesZ && terrain.heights.size() == size_t(samplesX * samplesZ) &&
            terrain.weights.size() == terrain.heights.size() * layerCount)
        {
            std::vector<float> paint(terrain.heights.size(), 1.f);
            if (proceduralLayerMask != 0 && layerCount)
                for (size_t sample = 0; sample < paint.size(); ++sample)
                {
                    float weight = 0.f;
                    for (size_t layer = 0; layer < layerCount && layer < 31; ++layer)
                        if (proceduralLayerMask & (1 << layer)) weight += terrain.weights[sample * layerCount + layer];
                    paint[sample] = weight;
                }
            const glm::vec3 origin(terrain.Owner->transform.GetWorldMatrix()[3]);
            // Paths shaded per pixel are not in the paint (see grows above):
            // the blades thin out along them as they did on the painted path.
            if (const auto* paths = terrain.Owner->GetComponentInParent<Engine::Components::TerrainManager>();
                paths && paths->contourPaths.enabled)
                for (size_t sample = 0; sample < paint.size(); ++sample)
                {
                    const float x = origin.x + float(sample % size_t(samplesX)) * terrain.cellWidth;
                    const float z = origin.z + float(sample / size_t(samplesX)) * terrain.cellDepth;
                    paint[sample] *= 1.f - std::min(1.f, paths->ContourPathShare(x, z,
                        origin.y + terrain.heights[sample]) / .9f);
                }
            blades->SetCell(this, origin, samplesX, terrain.cellWidth, terrain.heights, std::move(paint));
            m_bladeTerrain = &terrain;
            m_bladeTerrainRevision = revision;
        }
    }
    GrassBlades::Settings settings;
    settings.seed = seed;
    settings.fieldSize = tallFieldSize;
    settings.fieldCoverage = tallFieldCoverage;
    settings.shortUnderTall = shortUnderTallShare;
    settings.reach = GrassRadius() * std::clamp(fineReachFraction, .05f, 1.f);
    settings.windStrength = windStrength;
    settings.projectLayer = Owner->layer;
    blades->SetSettings(settings);
    blades->Update(*scene, scene->GetGraphicsProvider());
}

void GrassField::ReleaseGpuBlades()
{
    if (!m_bladeTerrain) return;
    if (auto* scene = Owner ? Owner->GetScene() : nullptr)
        if (scene->grassBlades) GrassBlades::For(*scene)->RemoveCell(this);
    m_bladeTerrain = nullptr;
}
bool GrassField::RefreshEditorPreview()
{
    if (!Owner || !Owner->GetScene() || Owner->GetScene()->HasStarted()) return false;
    return Refresh();
}

float GrassField::DetailRadiusScale() const
{
    // The project layer's detail scale (see DetailBudget). Generation is CPU
    // work the GPU budget cannot see, so grass reach stays within half to
    // twice its authored radius.
    const int layer = Owner ? Owner->layer : 0;
    return std::clamp(Engine::Rendering::DetailBudget::Get().LayerLodScale(layer),
        std::clamp(minimumRadiusScale, .25f, 2.f), 2.f);
}

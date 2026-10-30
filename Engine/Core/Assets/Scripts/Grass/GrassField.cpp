#include "Scripts/Grass/GrassField.h"

#include "Core/Object.h"
#include "Core/Scene/Scene.h"
#include "Core/Compoonents/Obj/Terrain.h"
#include "Core/Compoonents/Obj/TerrainObjectLayer.h"
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
    RegisterField("visibleRadius", visibleRadius, "Viewer");
    RegisterField("tileSize", tileSize, "Tiles");
    RegisterField("shortPatchesPerSquareMeter", shortPatchesPerSquareMeter, "Tiles");
    RegisterField("tallPatchesPerSquareMeter", tallPatchesPerSquareMeter, "Tiles");
    RegisterField("seed", seed, "Tiles");
    RegisterField("maxTileBuildsPerUpdate", maxTileBuildsPerUpdate, "Tiles");
    RegisterField("castShadows", castShadows, "Lighting");
}

bool GrassField::LoadSources()
{
    const std::array<std::string, 3> shortPaths{
        shortMesh, shortMeshLod1, shortMeshLod2};
    const std::array<std::string, 3> tallPaths{
        tallMesh, tallMeshLod1, tallMeshLod2};
    if (m_loaded && m_loadedShortPaths == shortPaths &&
        m_loadedTallPaths == tallPaths)
        return !m_shortVertices[0].empty() && !m_tallVertices[0].empty();
    m_loaded = true;
    m_loadedShortPaths = shortPaths;
    m_loadedTallPaths = tallPaths;
    try
    {
        for (int lod = 0; lod < 3; ++lod)
        {
            Engine::Components::Mesh shortSource, tallSource;
            shortSource.LoadFromFile(shortPaths[lod]);
            tallSource.LoadFromFile(tallPaths[lod]);
            m_shortVertices[lod] = shortSource.GetVertices();
            m_tallVertices[lod] = tallSource.GetVertices();
        }
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "GrassField: unable to load source mesh: %s\n",
            error.what());
        for (auto& source : m_shortVertices) source.clear();
        for (auto& source : m_tallVertices) source.clear();
    }
    return !m_shortVertices[0].empty() && !m_tallVertices[0].empty();
}

void GrassField::Start()
{
    ClearTiles();
    m_loaded = false;
    m_authoredPreviewHidden = false;
    m_hasGenerationSignature = false;
    m_cachedScene = nullptr;
    m_cachedStructureRevision = 0;
    m_cachedViewerName.clear();
    m_cachedViewer = nullptr;
    m_cachedTerrain = nullptr;
    m_cachedLayer = nullptr;
    EnsureAuthoredPreviewHidden();
}

void GrassField::ClearTiles()
{
    if (Owner && Owner->GetScene())
        for (auto& entry : m_tiles)
            if (entry.second.object)
                Owner->GetScene()->RequestRemoveObject(entry.second.object);
    m_tiles.clear();
    m_pendingTileUpdates.clear();
    m_cachedViewer = nullptr;
    m_cachedTerrain = nullptr;
    m_cachedLayer = nullptr;
}

bool GrassField::EnsureAuthoredPreviewHidden()
{
    if (m_authoredPreviewHidden || !Owner) return false;
    bool changed = false;
    for (auto* child : Owner->Children)
    {
        if (!child || child->name != "Grass Preview")
            continue;
        if (child->enabled)
        {
            child->enabled = false;
            changed = true;
        }
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
        !m_cachedViewer || !m_cachedTerrain;
    m_cachedScene = scene;
    m_cachedLayer = Owner->GetComponent<Engine::Components::TerrainObjectLayer>();
    if (refreshPointers)
    {
        m_cachedViewer = Owner->FindObjectInSceneByName(viewerObjectName);
        m_cachedTerrain = Owner->GetComponentInParent<Engine::Components::Terrain>();
        m_cachedViewerName = viewerObjectName;
        m_cachedStructureRevision = structureRevision;
    }
    return m_cachedViewer && m_cachedTerrain;
}

float GrassField::SampleHeight(float x, float z) const
{
    const auto* terrain = Owner
        ? Owner->GetComponentInParent<Engine::Components::Terrain>() : nullptr;
    if (!terrain || terrain->cellsX <= 0 || terrain->cellsY <= 0 ||
        terrain->cellWidth <= 0.f || terrain->cellDepth <= 0.f ||
        terrain->heights.size() != static_cast<size_t>(terrain->cellsX + 1) *
            static_cast<size_t>(terrain->cellsY + 1)) return 0.f;
    const float sx = std::clamp(x / terrain->cellWidth, 0.f,
        static_cast<float>(terrain->cellsX));
    const float sz = std::clamp(z / terrain->cellDepth, 0.f,
        static_cast<float>(terrain->cellsY));
    const int x0 = static_cast<int>(sx), z0 = static_cast<int>(sz);
    const int x1 = std::min(x0 + 1, terrain->cellsX);
    const int z1 = std::min(z0 + 1, terrain->cellsY);
    const auto at = [&](int col, int row) {
        return terrain->heights[static_cast<size_t>(row) *
            static_cast<size_t>(terrain->cellsX + 1) + static_cast<size_t>(col)];
    };
    const float a = at(x0, z0) + (at(x1, z0) - at(x0, z0)) * (sx - x0);
    const float b = at(x0, z1) + (at(x1, z1) - at(x0, z1)) * (sx - x0);
    return a + (b - a) * (sz - z0);
}

uint64_t GrassField::ComputeGenerationSignature() const
{
    uint64_t hash = HashOffset;
    HashBytes(hash, shortMesh.data(), shortMesh.size());
    HashBytes(hash, tallMesh.data(), tallMesh.size());
    HashBytes(hash, shortMeshLod1.data(), shortMeshLod1.size());
    HashBytes(hash, shortMeshLod2.data(), shortMeshLod2.size());
    HashBytes(hash, tallMeshLod1.data(), tallMeshLod1.size());
    HashBytes(hash, tallMeshLod2.data(), tallMeshLod2.size());
    HashFloat(hash, visibleRadius);
    HashFloat(hash, tileSize);
    HashFloat(hash, shortPatchesPerSquareMeter);
    HashFloat(hash, tallPatchesPerSquareMeter);
    HashBytes(hash, &seed, sizeof(seed));
    HashBytes(hash, &castShadows, sizeof(castShadows));

    const auto* layer = Owner
        ? Owner->GetComponent<Engine::Components::TerrainObjectLayer>() : nullptr;
    if (!layer) return hash;

    HashFloat(hash, layer->viewDistance);
    HashFloat(hash, layer->lod1Distance);
    HashFloat(hash, layer->lod2Distance);
    HashFloat(hash, layer->lodHysteresis);
    HashFloat(hash, layer->shadowDistance);
    HashBytes(hash, layer->layerName.data(), layer->layerName.size());
    for (const auto& placement : layer->placements)
    {
        HashBytes(hash, placement.asset.data(), placement.asset.size());
        HashFloat(hash, placement.position.x);
        HashFloat(hash, placement.position.y);
        HashFloat(hash, placement.position.z);
        HashFloat(hash, placement.normal.x);
        HashFloat(hash, placement.normal.y);
        HashFloat(hash, placement.normal.z);
        HashFloat(hash, placement.yawDegrees);
        HashFloat(hash, placement.scale);
    }
    return hash;
}

uint64_t GrassField::ComputeTileGeometrySignature(Tile tile) const
{
    uint64_t hash = HashOffset;
    HashBytes(hash, &tile.first, sizeof(tile.first));
    HashBytes(hash, &tile.second, sizeof(tile.second));
    const auto* layer = Owner
        ? Owner->GetComponent<Engine::Components::TerrainObjectLayer>() : nullptr;
    if (!layer || layer->placements.empty())
        return hash;
    for (const auto& placement : layer->placements)
    {
        if (placement.position.x < tile.first * std::max(1.f, tileSize) ||
            placement.position.x >= (tile.first + 1) * std::max(1.f, tileSize) ||
            placement.position.z < tile.second * std::max(1.f, tileSize) ||
            placement.position.z >= (tile.second + 1) * std::max(1.f, tileSize))
            continue;
        HashFloat(hash, placement.position.x);
        HashFloat(hash, placement.position.z);
        HashBytes(hash, placement.asset.data(), placement.asset.size());
        HashFloat(hash, placement.position.y);
        HashFloat(hash, placement.normal.x);
        HashFloat(hash, placement.normal.y);
        HashFloat(hash, placement.normal.z);
        HashFloat(hash, placement.yawDegrees);
        HashFloat(hash, placement.scale);
    }
    return hash;
}

bool GrassField::BuildTile(Tile tile, int lod, bool castsTileShadows,
    uint64_t geometrySignature)
{
    if (!Owner || !Owner->GetScene()) return false;
    const auto* terrain = Owner->GetComponentInParent<Engine::Components::Terrain>();
    if (!terrain) return false;
    const float span = std::max(1.f, tileSize);
    const float terrainX = terrain->cellsX * terrain->cellWidth;
    const float terrainZ = terrain->cellsY * terrain->cellDepth;
    const float tileX = tile.first * span, tileZ = tile.second * span;
    if (tileX >= terrainX || tileZ >= terrainZ ||
        tileX + span <= 0.f || tileZ + span <= 0.f) return false;

    const auto* layer = Owner->GetComponent<Engine::Components::TerrainObjectLayer>();
    lod = std::clamp(lod, 0, 2);
    const auto& shortVertices = m_shortVertices[lod];
    const auto& tallVertices = m_tallVertices[lod];
    std::mt19937 rng(TileSeed(seed, tile.first, tile.second));
    std::uniform_real_distribution<float> unit(0.f, 1.f);
    std::vector<Engine::Components::Mesh::Vertex> vertices;
    const int shortCount = std::clamp(static_cast<int>(std::round(
        shortPatchesPerSquareMeter * span * span)), 0, 64);
    const int tallCount = std::clamp(static_cast<int>(std::round(
        tallPatchesPerSquareMeter * span * span)), 0, 32);
    vertices.reserve(static_cast<size_t>(shortCount) * shortVertices.size() +
        static_cast<size_t>(tallCount) * tallVertices.size());
    const auto appendPlacement = [&](
        const std::vector<Engine::Components::Mesh::Vertex>& source,
        float x, float y, float z, float angleDegrees, float scale,
        const glm::vec3& normal)
    {
        const glm::mat3 rotation = glm::mat3_cast(
            Engine::Components::TerrainObjectLayer::SurfaceRotation(
                normal, angleDegrees));
        for (auto vertex : source)
        {
            const glm::vec3 p = rotation * (scale * glm::vec3(
                vertex.pos[0], vertex.pos[1], vertex.pos[2]));
            const glm::vec3 n = rotation * glm::vec3(
                vertex.normal[0], vertex.normal[1], vertex.normal[2]);
            vertex.pos[0] = x - tileX + p.x;
            vertex.pos[1] = y + p.y;
            vertex.pos[2] = z - tileZ + p.z;
            vertex.normal[0] = n.x;
            vertex.normal[1] = n.y;
            vertex.normal[2] = n.z;
            vertices.push_back(vertex);
        }
    };
    if (layer && !layer->placements.empty())
    {
        for (const auto& placement : layer->placements)
        {
            const float x = placement.position.x, z = placement.position.z;
            if (x < tileX || x >= tileX + span ||
                z < tileZ || z >= tileZ + span) continue;
            const auto& source = placement.asset == tallMesh
                ? tallVertices : shortVertices;
            appendPlacement(source, x, placement.position.y, z,
                placement.yawDegrees, placement.scale, placement.normal);
        }
    }
    else
    {
        const auto appendRandom = [&](const auto& source, int count)
        {
            for (int instance = 0; instance < count; ++instance)
            {
                const float x = tileX + unit(rng) * span;
                const float z = tileZ + unit(rng) * span;
                if (x < .3f || z < .3f || x > terrainX - .3f ||
                    z > terrainZ - .3f) continue;
                appendPlacement(source, x, SampleHeight(x, z), z,
                    unit(rng) * 360.f, .8f + .4f * unit(rng),
                    {0.f, 1.f, 0.f});
            }
        };
        appendRandom(shortVertices, shortCount);
        appendRandom(tallVertices, tallCount);
    }
    if (vertices.empty()) return false;

    auto& scene = *Owner->GetScene();
    auto existing = m_tiles.find(tile);
    Engine::Core::Object* object = existing != m_tiles.end()
        ? existing->second.object : nullptr;
    const bool created = object == nullptr;
    if (!object)
    {
        char name[64]{};
        std::snprintf(name, sizeof(name), "Grass Tile (%d, %d)",
            tile.first, tile.second);
        object = scene.AddObject(name);
        object->Parent = Owner;
        Owner->Children.push_back(object);
    }
    object->transform.position = { tileX, 0.f, tileZ };
    auto* mesh = object->GetComponent<Engine::Components::Mesh>();
    if (!mesh)
        mesh = object->AddComponent<Engine::Components::Mesh>();
    if (!mesh->SetAuthoredVertices(std::move(vertices)))
    {
        if (created)
            scene.RequestRemoveObject(object);
        return false;
    }
    if (scene.GetGraphicsProvider())
        mesh->OnAfterDeserialize(scene.GetGraphicsProvider());
    auto* material = object->GetComponent<Engine::Components::Material>();
    if (!material)
        material = object->AddComponent<Engine::Components::Material>();
    material->diffuseColor = {1.f, 1.f, 1.f};
    material->useVertexColors = true;
    material->roughnessFactor = 1.f;
    material->metallicFactor = 0.f;
    material->doubleSided = true;
    material->castsShadows = castsTileShadows;
    TileState& state = m_tiles[tile];
    state.object = object;
    state.lod = lod;
    state.castsShadows = castsTileShadows;
    state.geometrySignature = geometrySignature;
    return true;
}

bool GrassField::Refresh()
{
    if (!Owner || !Owner->GetScene() || !LoadSources()) return false;
    bool changed = EnsureAuthoredPreviewHidden();
    ++m_refreshEpoch;
    const uint64_t generationSignature = ComputeGenerationSignature();
    const bool rebuildVisibleTiles = !m_hasGenerationSignature ||
        generationSignature != m_lastGenerationSignature;
    m_lastGenerationSignature = generationSignature;
    m_hasGenerationSignature = true;
    if (!ResolveCachedReferences()) return changed;
    auto* viewer = m_cachedViewer;
    auto* terrain = m_cachedTerrain;
    const glm::vec3 local = glm::vec3(glm::inverse(
        terrain->Owner->transform.GetWorldMatrix()) * glm::vec4(
            viewer->transform.GetWorldPosition(), 1.f));
    const auto* layer = m_cachedLayer;
    const float span = std::max(1.f, tileSize);
    const float radius = layer ? std::max(1.f, layer->viewDistance)
        : std::max(2.f, visibleRadius);
    const float lod1 = layer ? std::clamp(layer->lod1Distance, 0.f, radius)
        : radius;
    const float lod2 = layer ? std::clamp(layer->lod2Distance, lod1, radius)
        : radius;
    const float hysteresis = layer ? std::max(0.f, layer->lodHysteresis) : 0.f;
    const float shadowRadius = layer ?
        std::clamp(layer->shadowDistance, 0.f, radius) : radius;
    const int maxX = static_cast<int>(std::ceil(
        terrain->cellsX * terrain->cellWidth / span));
    const int maxZ = static_cast<int>(std::ceil(
        terrain->cellsY * terrain->cellDepth / span));
    // Search only the viewer's tile neighborhood. A large terrain must not
    // scan its entire map just to maintain a small grass ring.
    const int reach = static_cast<int>(std::ceil(radius / span)) + 1;
    const int centerX = static_cast<int>(std::floor(local.x / span));
    const int centerZ = static_cast<int>(std::floor(local.z / span));
    const int firstX = std::max(0, centerX - reach);
    const int firstZ = std::max(0, centerZ - reach);
    const int endX = std::min(maxX, centerX + reach + 1);
    const int endZ = std::min(maxZ, centerZ + reach + 1);
    struct DesiredTile
    {
        int lod = 0;
        bool castsShadows = false;
        uint64_t geometrySignature = 0;
        float distanceSquared = 0.f;
    };
    std::map<Tile, DesiredTile> desired;
    for (int z = firstZ; z < endZ; ++z)
        for (int x = firstX; x < endX; ++x)
        {
            const float tileCenterX = (x + .5f) * span;
            const float tileCenterZ = (z + .5f) * span;
            const float dx = tileCenterX - local.x;
            const float dz = tileCenterZ - local.z;
            const float distance = std::sqrt(dx * dx + dz * dz);
            const float edgeX = std::max(0.f, std::abs(dx) - span * .5f);
            const float edgeZ = std::max(0.f, std::abs(dz) - span * .5f);
            if (edgeX * edgeX + edgeZ * edgeZ > radius * radius) continue;
            const Tile tile{x, z};
            int lod = distance < lod1 ? 0 : distance < lod2 ? 1 : 2;
            if (const auto old = m_tiles.find(tile); old != m_tiles.end())
            {
                if (old->second.lod == 0 && distance < lod1 + hysteresis)
                    lod = 0;
                else if (old->second.lod == 1 &&
                    distance >= lod1 - hysteresis &&
                    distance < lod2 + hysteresis) lod = 1;
                else if (old->second.lod == 2 &&
                    distance >= lod2 - hysteresis) lod = 2;
            }
            bool tileShadows = castShadows && distance < shadowRadius;
            if (castShadows && shadowRadius > 0.f)
                if (const auto old = m_tiles.find(tile); old != m_tiles.end())
                {
                    if (old->second.castsShadows &&
                        distance < shadowRadius + hysteresis)
                        tileShadows = true;
                    else if (!old->second.castsShadows &&
                        distance >= shadowRadius - hysteresis)
                        tileShadows = false;
                }
            desired.emplace(tile, DesiredTile{
                lod, tileShadows, ComputeTileGeometrySignature(tile),
                dx * dx + dz * dz});
        }
    for (auto it = m_tiles.begin(); it != m_tiles.end(); )
    {
        if (desired.count(it->first)) { ++it; continue; }
        Owner->GetScene()->RequestRemoveObject(it->second.object);
        m_pendingTileUpdates.erase(it->first);
        it = m_tiles.erase(it);
        changed = true;
    }
    for (const auto& [tile, target] : desired)
    {
        const auto existing = m_tiles.find(tile);
        const bool needsGeometryUpdate = existing == m_tiles.end() ||
            existing->second.lod != target.lod ||
            existing->second.geometrySignature != target.geometrySignature ||
            rebuildVisibleTiles;
        if (!needsGeometryUpdate)
        {
            m_pendingTileUpdates.erase(tile);
            if (existing->second.castsShadows != target.castsShadows)
            {
                if (auto* material = existing->second.object->
                    GetComponent<Engine::Components::Material>())
                    material->castsShadows = target.castsShadows;
                existing->second.castsShadows = target.castsShadows;
                changed = true;
            }
            continue;
        }
        auto& pending = m_pendingTileUpdates[tile];
        pending.lod = target.lod;
        pending.castsShadows = target.castsShadows;
        pending.geometrySignature = target.geometrySignature;
        pending.distanceSquared = target.distanceSquared;
        pending.refreshEpoch = m_refreshEpoch;
    }
    for (auto it = m_pendingTileUpdates.begin();
         it != m_pendingTileUpdates.end();)
    {
        if (it->second.refreshEpoch != m_refreshEpoch ||
            !desired.count(it->first))
            it = m_pendingTileUpdates.erase(it);
        else
            ++it;
    }

    int remaining = std::clamp(maxTileBuildsPerUpdate, 1, 16);
    std::vector<Tile> orderedPending;
    orderedPending.reserve(m_pendingTileUpdates.size());
    for (const auto& entry : m_pendingTileUpdates)
        orderedPending.push_back(entry.first);
    std::sort(orderedPending.begin(), orderedPending.end(),
        [&](const Tile& first, const Tile& second)
        {
            const float firstDistance =
                m_pendingTileUpdates[first].distanceSquared;
            const float secondDistance =
                m_pendingTileUpdates[second].distanceSquared;
            if (firstDistance != secondDistance)
                return firstDistance < secondDistance;
            if (first.first != second.first)
                return first.first < second.first;
            return first.second < second.second;
        });
    for (const Tile& tile : orderedPending)
    {
        if (remaining <= 0) break;
        const auto pending = m_pendingTileUpdates.find(tile);
        if (pending == m_pendingTileUpdates.end()) continue;
        if (BuildTile(tile, pending->second.lod, pending->second.castsShadows,
                pending->second.geometrySignature))
        {
            m_pendingTileUpdates.erase(pending);
            changed = true;
        }
        --remaining;
    }
    return changed;
}

void GrassField::Update()
{
    Refresh();
}

void GrassField::OnDestroy()
{
    ClearTiles();
}

bool GrassField::RefreshEditorPreview()
{
    if (!Owner || !Owner->GetScene() || Owner->GetScene()->HasStarted())
        return false;
    return Refresh();
}

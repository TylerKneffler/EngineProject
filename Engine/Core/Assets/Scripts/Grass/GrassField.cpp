#include "Scripts/Grass/GrassField.h"

#include "Core/Object.h"
#include "Core/Scene/Scene.h"
#include "Core/Compoonents/Obj/Terrain.h"
#include "Core/Compoonents/Obj/TerrainObjectLayer.h"
#include "Core/Compoonents/Materials/Material.h"
#include "Core/Serialization/SceneSerializer.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
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
    if (m_loaded) return !m_shortVertices[0].empty() &&
        !m_tallVertices[0].empty();
    m_loaded = true;
    try
    {
        const std::array<std::string, 3> shortPaths{
            shortMesh, shortMeshLod1, shortMeshLod2};
        const std::array<std::string, 3> tallPaths{
            tallMesh, tallMeshLod1, tallMeshLod2};
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
    m_tiles.clear();
    m_loaded = false;
    // The small authored preview stays visible in the editor, while runtime
    // tiles replace it in play mode.
    if (Owner)
        for (auto* child : Owner->Children)
            if (child && child->name == "Grass Preview") child->enabled = false;
    LoadSources();
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

void GrassField::BuildTile(Tile tile, int lod, bool castsTileShadows)
{
    if (!Owner || !Owner->GetScene()) return;
    const auto* terrain = Owner->GetComponentInParent<Engine::Components::Terrain>();
    if (!terrain) return;
    const float span = std::max(1.f, tileSize);
    const float terrainX = terrain->cellsX * terrain->cellWidth;
    const float terrainZ = terrain->cellsY * terrain->cellDepth;
    const float tileX = tile.first * span, tileZ = tile.second * span;
    if (tileX >= terrainX || tileZ >= terrainZ ||
        tileX + span <= 0.f || tileZ + span <= 0.f) return;

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
    if (vertices.empty()) return;

    auto& scene = *Owner->GetScene();
    char name[64]{};
    std::snprintf(name, sizeof(name), "Grass Tile (%d, %d)",
        tile.first, tile.second);
    auto* object = scene.AddObject(name);
    object->Parent = Owner;
    Owner->Children.push_back(object);
    object->transform.position = { tileX, 0.f, tileZ };
    auto* mesh = object->AddComponent<Engine::Components::Mesh>();
    if (!mesh->SetAuthoredVertices(std::move(vertices)))
    {
        scene.RequestRemoveObject(object);
        return;
    }
    if (scene.GetGraphicsProvider())
        mesh->OnAfterDeserialize(scene.GetGraphicsProvider());
    auto* material = object->AddComponent<Engine::Components::Material>();
    material->diffuseColor = {1.f, 1.f, 1.f};
    material->useVertexColors = true;
    material->roughnessFactor = 1.f;
    material->metallicFactor = 0.f;
    material->doubleSided = true;
    material->castsShadows = castsTileShadows;
    m_tiles[tile] = {object, lod, castsTileShadows};
}

void GrassField::Refresh()
{
    if (!Owner || !Owner->GetScene() || !LoadSources()) return;
    auto* viewer = Owner->FindObjectInSceneByName(viewerObjectName);
    auto* terrain = Owner->GetComponentInParent<Engine::Components::Terrain>();
    if (!viewer || !terrain) return;
    const glm::vec3 local = glm::vec3(glm::inverse(
        terrain->Owner->transform.GetWorldMatrix()) * glm::vec4(
            viewer->transform.GetWorldPosition(), 1.f));
    const auto* layer = Owner->GetComponent<Engine::Components::TerrainObjectLayer>();
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
    std::map<Tile, std::pair<int, bool>> desired;
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
            desired.emplace(tile, std::make_pair(lod, tileShadows));
        }
    for (auto it = m_tiles.begin(); it != m_tiles.end(); )
    {
        if (desired.count(it->first)) { ++it; continue; }
        Owner->GetScene()->RequestRemoveObject(it->second.object);
        it = m_tiles.erase(it);
    }
    int remaining = std::clamp(maxTileBuildsPerUpdate, 1, 16);
    for (const auto& [tile, target] : desired)
    {
        const auto existing = m_tiles.find(tile);
        if (existing != m_tiles.end() && existing->second.lod == target.first)
        {
            if (existing->second.castsShadows != target.second)
            {
                if (auto* material = existing->second.object->
                    GetComponent<Engine::Components::Material>())
                    material->castsShadows = target.second;
                existing->second.castsShadows = target.second;
            }
            continue;
        }
        if (remaining <= 0) break;
        auto* oldObject = existing != m_tiles.end()
            ? existing->second.object : nullptr;
        BuildTile(tile, target.first, target.second);
        if (oldObject && m_tiles.find(tile) != m_tiles.end() &&
            m_tiles[tile].object != oldObject)
            Owner->GetScene()->RequestRemoveObject(oldObject);
        --remaining;
    }
}

void GrassField::Update()
{
    Refresh();
}

void GrassField::OnDestroy()
{
    m_tiles.clear();
}

#include "SceneTerrainTools.h"

#include "Core/Object.h"
#include "Core/Scene/Scene.h"
#include "Core/Compoonents/Terrain/Terrain.h"
#include "Core/Compoonents/Terrain/TerrainManager.h"
#include "Core/Compoonents/Terrain/TerrainObjectLayer.h"
#include "Core/Compoonents/Terrain/TerrainSurfaceSample.h"
#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace Engine::Editor
{
namespace
{
using Engine::Components::Terrain;
using Engine::Components::TerrainManager;
using Engine::Components::TerrainObjectLayer;

float NextRandom(uint32_t& state)
{
    // xorshift32: deterministic per stroke, independent of std::rand.
    state ^= state << 13; state ^= state >> 17; state ^= state << 5;
    return (state & 0xffffffu) / float(0x1000000);
}

// Cells under a root-local disk.
std::vector<Terrain*> CellsUnder(const SceneTerrainTools::Target& target,
    const glm::dvec3& center, float radius)
{
    std::vector<Terrain*> cells;
    if (!target.manager)
    {
        if (target.terrain) cells.push_back(target.terrain);
        return cells;
    }
    const double spanX = double(target.manager->cellsX) * target.manager->cellWidth;
    const double spanZ = double(target.manager->cellsY) * target.manager->cellDepth;
    if (spanX <= 0.0 || spanZ <= 0.0) return cells;
    const int minX = int(std::floor((center.x - radius) / spanX));
    const int maxX = int(std::floor((center.x + radius) / spanX));
    const int minZ = int(std::floor((center.z - radius) / spanZ));
    const int maxZ = int(std::floor((center.z + radius) / spanZ));
    for (int z = minZ; z <= maxZ; ++z)
        for (int x = minX; x <= maxX; ++x)
            if (auto* cell = target.manager->GetLoadedCell(x, z)) cells.push_back(cell);
    return cells;
}

// The layer a painted prefab goes to on this cell: the one already holding
// it, else the brush's own "Objects" layer (created on first use). Never a
// ground-conforming layer such as grass, whose LOD and conforming settings
// would apply to the painted objects.
TerrainObjectLayer* LayerFor(Engine::Scene::Scene& scene, Terrain& cell,
    const std::string& prefab)
{
    TerrainObjectLayer* objectsLayer = nullptr;
    for (auto* child : cell.Owner->Children)
        if (auto* layer = child ? child->GetComponent<TerrainObjectLayer>() : nullptr)
        {
            if (std::any_of(layer->placements.begin(), layer->placements.end(),
                    [&](const auto& existing) { return existing.asset == prefab; }))
                return layer;
            if (!objectsLayer && layer->layerName == "Objects" &&
                !layer->conformToGround && !layer->snapToGround)
                objectsLayer = layer;
        }
    if (objectsLayer) return objectsLayer;
    auto* child = scene.AddObject("Objects");
    scene.MoveObject(child, cell.Owner, Engine::Scene::Scene::ObjectPlacement::AsChild);
    child->transform.position = {0, 0, 0};
    child->transform.rotation = {0, 0, 0};
    child->transform.scale = {1, 1, 1};
    return child->AddComponent<TerrainObjectLayer>();
}
}

glm::dvec3 SceneTerrainTools::ToLocal(const Target& target, const glm::dvec3& world)
{
    if (!target.root) return world;
    return glm::dvec3(glm::inverse(target.root->transform.GetWorldMatrixD()) *
        glm::dvec4(world, 1.0));
}

bool SceneTerrainTools::SampleSurface(const Target& target, double localX, double localZ,
    glm::dvec3& world, glm::vec3& worldNormal, Terrain** cellOut)
{
    Terrain* cell = target.manager ? nullptr : target.terrain;
    double x = localX, z = localZ;
    if (target.manager)
    {
        const double spanX = double(target.manager->cellsX) * target.manager->cellWidth;
        const double spanZ = double(target.manager->cellsY) * target.manager->cellDepth;
        if (spanX <= 0.0 || spanZ <= 0.0) return false;
        const int cx = int(std::floor(localX / spanX));
        const int cz = int(std::floor(localZ / spanZ));
        cell = target.manager->GetLoadedCell(cx, cz);
        x = localX - cx * spanX;
        z = localZ - cz * spanZ;
    }
    if (!cell || !cell->Owner) return false;
    if (x < 0.0 || z < 0.0 || x > cell->cellsX * cell->cellWidth ||
        z > cell->cellsY * cell->cellDepth) return false;
    float height = 0.f;
    glm::vec3 normal(0.f, 1.f, 0.f);
    if (!Engine::Components::SampleTerrainSurface(cell->heights, cell->cellsX,
            cell->cellsY, cell->cellWidth, cell->cellDepth, float(x), float(z),
            height, &normal)) return false;
    const glm::dmat4 cellWorld = cell->Owner->transform.GetWorldMatrixD();
    world = glm::dvec3(cellWorld * glm::dvec4(x, height, z, 1.0));
    worldNormal = glm::normalize(glm::vec3(glm::dmat3(cellWorld) * glm::dvec3(normal)));
    if (cellOut) *cellOut = cell;
    return true;
}

void SceneTerrainTools::DrawBrushRing(const Target& target, const glm::dvec3& localCenter,
    float radius, float hardness, const glm::dmat4& viewProjection,
    const glm::vec2& screenMin, const glm::vec2& screenSize, uint32_t color)
{
    auto* draw = ImGui::GetForegroundDrawList();
    const auto ring = [&](float ringRadius, float thickness, uint32_t ringColor)
    {
        constexpr int segments = 64;
        std::vector<ImVec2> points;
        points.reserve(segments + 1);
        const auto flush = [&]
        {
            if (points.size() > 1)
                draw->AddPolyline(points.data(), int(points.size()), ringColor, 0, thickness);
            points.clear();
        };
        for (int i = 0; i <= segments; ++i)
        {
            const double angle = 6.283185307179586 * i / segments;
            glm::dvec3 world;
            glm::vec3 normal;
            if (!SampleSurface(target, localCenter.x + std::cos(angle) * ringRadius,
                    localCenter.z + std::sin(angle) * ringRadius, world, normal))
            { flush(); continue; }
            // Lift slightly along the normal so the ring is not z-fighting
            // visually with steep faces (it is drawn on top regardless).
            const glm::dvec4 clip = viewProjection * glm::dvec4(world + glm::dvec3(normal) * 0.02, 1.0);
            if (clip.w <= 1e-6) { flush(); continue; }
            points.push_back({float(screenMin.x + (clip.x / clip.w + 1.0) * screenSize.x * 0.5),
                float(screenMin.y + (1.0 - clip.y / clip.w) * screenSize.y * 0.5)});
        }
        flush();
    };
    ring(radius, 2.f, color);
    if (hardness > 0.02f && hardness < 0.98f)
        ring(radius * hardness, 1.f, (color & 0x00ffffffu) | 0x80000000u);
}

int SceneTerrainTools::Scatter(Engine::Scene::Scene& scene, const Target& target,
    const glm::dvec3& localCenter, float radius, const ScatterSettings& settings,
    uint32_t& random)
{
    if (settings.prefab.empty() || settings.count <= 0) return 0;
    int placed = 0;
    for (int index = 0; index < settings.count; ++index)
    {
        // A single object per dab goes exactly under the cursor.
        double x = localCenter.x, z = localCenter.z;
        if (settings.count > 1)
        {
            const double distance = radius * std::sqrt(NextRandom(random));
            const double angle = 6.283185307179586 * NextRandom(random);
            x += std::cos(angle) * distance;
            z += std::sin(angle) * distance;
        }
        glm::dvec3 world;
        glm::vec3 normal;
        Terrain* cell = nullptr;
        if (!SampleSurface(target, x, z, world, normal, &cell) || !cell) continue;
        auto* layer = LayerFor(scene, *cell, settings.prefab);
        if (!layer || !layer->Owner) continue;
        const glm::dmat4 inverse = glm::inverse(layer->Owner->transform.GetWorldMatrixD());
        TerrainObjectLayer::Placement placement;
        placement.asset = settings.prefab;
        placement.position = glm::vec3(inverse * glm::dvec4(world, 1.0));
        placement.normal = settings.alignToSurface
            ? glm::normalize(glm::vec3(glm::dmat3(inverse) * glm::dvec3(normal)))
            : glm::vec3(0.f, 1.f, 0.f);
        placement.yawDegrees = settings.randomYaw ? NextRandom(random) * 360.f - 180.f : 0.f;
        const float low = std::max(0.01f, std::min(settings.minScale, settings.maxScale));
        const float high = std::max(low, std::max(settings.minScale, settings.maxScale));
        placement.scale = low + (high - low) * NextRandom(random);
        layer->placements.push_back(placement);
        layer->MarkConfigurationDirty();
        ++placed;
    }
    return placed;
}

int SceneTerrainTools::Erase(const Target& target, const glm::dvec3& localCenter,
    float radius, const std::string& prefab)
{
    if (!target.root) return 0;
    const glm::dvec3 worldCenter = glm::dvec3(
        target.root->transform.GetWorldMatrixD() * glm::dvec4(localCenter, 1.0));
    int removed = 0;
    for (auto* cell : CellsUnder(target, localCenter, radius))
    {
        if (!cell->Owner) continue;
        for (auto* child : cell->Owner->Children)
        {
            auto* layer = child ? child->GetComponent<TerrainObjectLayer>() : nullptr;
            if (!layer) continue;
            if (prefab.empty() && (layer->conformToGround || layer->snapToGround)) continue;
            const glm::dmat4 layerWorld = child->transform.GetWorldMatrixD();
            const auto before = layer->placements.size();
            layer->placements.erase(std::remove_if(layer->placements.begin(),
                layer->placements.end(), [&](const TerrainObjectLayer::Placement& placement)
                {
                    if (!prefab.empty() && placement.asset != prefab) return false;
                    const glm::dvec3 world = glm::dvec3(layerWorld *
                        glm::dvec4(glm::dvec3(placement.position), 1.0));
                    const double dx = world.x - worldCenter.x, dz = world.z - worldCenter.z;
                    return dx * dx + dz * dz <= double(radius) * radius;
                }), layer->placements.end());
            if (layer->placements.size() != before)
            {
                removed += int(before - layer->placements.size());
                layer->MarkConfigurationDirty();
            }
        }
    }
    return removed;
}
}

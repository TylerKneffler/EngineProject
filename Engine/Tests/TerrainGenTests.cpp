#include "Core/Assets/Scripts/TerrainGen/TerrainChunk.h"
#include "Core/Assets/Scripts/TerrainGen/TerrainPatch.h"
#include "Core/Assets/Scripts/TerrainGen/TerrainGen.h"
#include "Core/Assets/Scripts/TerrainGen/PerlinNoiseField.h"
#include "Core/Assets/Scripts/TerrainGen/TerrainStreamingCameraDriver.h"
#include "Core/Compoonents/Materials/Material.h"
#include "Core/Compoonents/Obj/Mesh.h"
#include "Core/Object.h"
#include "Core/Scene/Scene.h"
#include "Core/Serialization/SceneSerializer.h"
#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <limits>
#include <thread>
#include <map>

namespace
{
struct QuantizedPoint
{
    long long x = 0;
    long long y = 0;
    long long z = 0;

    bool operator<(const QuantizedPoint& other) const
    {
        if (x != other.x) return x < other.x;
        if (y != other.y) return y < other.y;
        return z < other.z;
    }
};

struct QuantizedEdge
{
    QuantizedPoint first{};
    QuantizedPoint second{};

    bool operator<(const QuantizedEdge& other) const
    {
        if (first < other.first) return true;
        if (other.first < first) return false;
        return second < other.second;
    }
};

QuantizedPoint Quantize(const glm::vec3& point)
{
    constexpr double scale = 100000.0;
    return { std::llround(static_cast<double>(point.x) * scale),
        std::llround(static_cast<double>(point.y) * scale),
        std::llround(static_cast<double>(point.z) * scale) };
}

QuantizedEdge MakeEdge(const glm::vec3& first, const glm::vec3& second)
{
    QuantizedEdge result { Quantize(first), Quantize(second) };
    if (result.second < result.first)
        std::swap(result.first, result.second);
    return result;
}

bool AuditSmoothGridTopology(Engine::Core::Object& terrainObject,
    float chunkSize)
{
    struct EdgeUse
    {
        int count = 0;
        int orientation = 0;
        int diagnosticCount = 0;
        std::array<glm::vec3, 2> faceNormals{};
        std::array<glm::ivec2, 2> chunkCoordinates{};
        std::array<std::array<glm::vec3, 3>, 2> triangles{};
    };
    std::map<QuantizedEdge, EdgeUse> edgeUses;
    int minimumChunkX = std::numeric_limits<int>::max();
    int maximumChunkX = std::numeric_limits<int>::lowest();
    int minimumChunkZ = std::numeric_limits<int>::max();
    int maximumChunkZ = std::numeric_limits<int>::lowest();
    for (Engine::Core::Object* chunkObject : terrainObject.Children)
    {
        const TerrainChunk* chunk = chunkObject
            ? chunkObject->GetComponent<TerrainChunk>() : nullptr;
        if (!chunk)
            continue;
        minimumChunkX = std::min(minimumChunkX, chunk->chunkX);
        maximumChunkX = std::max(maximumChunkX, chunk->chunkX);
        minimumChunkZ = std::min(minimumChunkZ, chunk->chunkZ);
        maximumChunkZ = std::max(maximumChunkZ, chunk->chunkZ);
        for (Engine::Core::Object* patchObject : chunkObject->Children)
        {
            const auto* mesh = patchObject
                ? patchObject->GetComponent<Engine::Components::Mesh>() : nullptr;
            if (!mesh)
                continue;
            const glm::mat4 world = patchObject->transform.GetWorldMatrix();
            const auto& vertices = mesh->GetVertices();
            for (size_t triangle = 0; triangle + 2 < vertices.size(); triangle += 3)
            {
                glm::vec3 points[3]{};
                for (int corner = 0; corner < 3; ++corner)
                {
                    const auto& vertex = vertices[triangle + corner];
                    points[corner] = glm::vec3(world * glm::vec4(
                        vertex.pos[0], vertex.pos[1], vertex.pos[2], 1.f));
                }
                for (int edge = 0; edge < 3; ++edge)
                {
                    const QuantizedPoint from = Quantize(points[edge]);
                    const QuantizedPoint to = Quantize(points[(edge + 1) % 3]);
                    if (!(from < to) && !(to < from))
                        continue;
                    EdgeUse& use = edgeUses[MakeEdge(
                        points[edge], points[(edge + 1) % 3])];
                    ++use.count;
                    use.orientation += to < from ? -1 : 1;
                    const glm::vec3 faceCross = glm::cross(
                        points[1] - points[0], points[2] - points[0]);
                    if (use.diagnosticCount < 2)
                    {
                        const int diagnostic = use.diagnosticCount++;
                        use.faceNormals[diagnostic] = glm::length(faceCross) > 0.f
                            ? glm::normalize(faceCross) : glm::vec3(0.f);
                        use.chunkCoordinates[diagnostic] =
                            { chunk->chunkX, chunk->chunkZ };
                        use.triangles[diagnostic] =
                            { points[0], points[1], points[2] };
                    }
                }
            }
        }
    }

    // A single-use edge is valid only on the outside of the generated ring.
    // Any such edge at an internal chunk border is a real terrain crack.
    if (minimumChunkX > maximumChunkX || minimumChunkZ > maximumChunkZ)
        return false;
    const float outerMinimumX = static_cast<float>(minimumChunkX) * chunkSize;
    const float outerMaximumX = static_cast<float>(maximumChunkX + 1) * chunkSize;
    const float outerMinimumZ = static_cast<float>(minimumChunkZ) * chunkSize;
    const float outerMaximumZ = static_cast<float>(maximumChunkZ + 1) * chunkSize;
    for (const auto& [edge, use] : edgeUses)
    {
        if (use.count != 1 && use.orientation == 0)
            continue;
        const double inverseScale = 1.0 / 100000.0;
        const double x0 = edge.first.x * inverseScale;
        const double x1 = edge.second.x * inverseScale;
        const double z0 = edge.first.z * inverseScale;
        const double z1 = edge.second.z * inverseScale;
        const bool outside =
            (std::abs(x0 - outerMinimumX) < 0.00002 &&
                std::abs(x1 - outerMinimumX) < 0.00002) ||
            (std::abs(x0 - outerMaximumX) < 0.00002 &&
                std::abs(x1 - outerMaximumX) < 0.00002) ||
            (std::abs(z0 - outerMinimumZ) < 0.00002 &&
                std::abs(z1 - outerMinimumZ) < 0.00002) ||
            (std::abs(z0 - outerMaximumZ) < 0.00002 &&
                std::abs(z1 - outerMaximumZ) < 0.00002);
        if (!outside)
        {
            std::fprintf(stderr,
                "Internal terrain edge is %s: (%.5f, %.5f, %.5f) to "
                "(%.5f, %.5f, %.5f), uses=%d orientation=%d\n",
                use.count == 1 ? "open" : "wound inconsistently",
                x0, edge.first.y * inverseScale, z0,
                x1, edge.second.y * inverseScale, z1,
                use.count, use.orientation);
            for (int source = 0; source < use.diagnosticCount; ++source)
            {
                const glm::vec3 normal = use.faceNormals[source];
                const glm::ivec2 chunk = use.chunkCoordinates[source];
                std::fprintf(stderr,
                    "  source chunk=(%d,%d) face_normal=(%.5f,%.5f,%.5f)\n",
                    chunk.x, chunk.y, normal.x, normal.y, normal.z);
                const auto& triangle = use.triangles[source];
                std::fprintf(stderr,
                    "    triangle=(%.5f,%.5f,%.5f) (%.5f,%.5f,%.5f) "
                    "(%.5f,%.5f,%.5f)\n",
                    triangle[0].x, triangle[0].y, triangle[0].z,
                    triangle[1].x, triangle[1].y, triangle[1].z,
                    triangle[2].x, triangle[2].y, triangle[2].z);
            }
            return false;
        }
    }
    return true;
}

struct SharedBorderAudit
{
    bool valid = true;
    size_t neighborPairs = 0;
    size_t comparedVertices = 0;
    float maximumPositionError = 0.f;
};

SharedBorderAudit AuditSharedChunkWorldVertices(
    Engine::Core::Object& terrainObject, float chunkSize)
{
    std::map<std::pair<int, int>, Engine::Core::Object*> chunks;
    for (Engine::Core::Object* object : terrainObject.Children)
    {
        const TerrainChunk* chunk = object
            ? object->GetComponent<TerrainChunk>() : nullptr;
        if (chunk)
            chunks[{ chunk->chunkX, chunk->chunkZ }] = object;
    }

    const auto borderVertices = [](Engine::Core::Object* chunk,
        bool xAxis, float border)
    {
        std::vector<glm::vec3> result;
        for (Engine::Core::Object* patchObject : chunk->Children)
        {
            const auto* mesh = patchObject
                ? patchObject->GetComponent<Engine::Components::Mesh>() : nullptr;
            if (!mesh)
                continue;
            const glm::mat4 world = patchObject->transform.GetWorldMatrix();
            for (const auto& vertex : mesh->GetVertices())
            {
                const glm::vec3 position = glm::vec3(world * glm::vec4(
                    vertex.pos[0], vertex.pos[1], vertex.pos[2], 1.f));
                const float coordinate = xAxis ? position.x : position.z;
                if (std::abs(coordinate - border) <= 0.000001f)
                    result.push_back(position);
            }
        }
        return result;
    };
    const auto compareDirections = [](const std::vector<glm::vec3>& source,
        const std::vector<glm::vec3>& target, SharedBorderAudit& audit)
    {
        if (source.empty() || target.empty())
        {
            audit.valid = false;
            return;
        }
        for (const glm::vec3& point : source)
        {
            float nearest = std::numeric_limits<float>::max();
            for (const glm::vec3& candidate : target)
                nearest = std::min(nearest, glm::length(point - candidate));
            audit.maximumPositionError = std::max(
                audit.maximumPositionError, nearest);
            ++audit.comparedVertices;
            if (nearest > 0.000001f)
                audit.valid = false;
        }
    };

    SharedBorderAudit audit;
    for (const auto& [coordinate, chunk] : chunks)
    {
        for (const auto& direction : {
            std::pair<int, int>{ 1, 0 }, std::pair<int, int>{ 0, 1 } })
        {
            const std::pair<int, int> neighborCoordinate {
                coordinate.first + direction.first,
                coordinate.second + direction.second };
            const auto neighbor = chunks.find(neighborCoordinate);
            if (neighbor == chunks.end())
                continue;
            const bool xAxis = direction.first != 0;
            const float border = static_cast<float>(xAxis
                ? neighborCoordinate.first : neighborCoordinate.second) *
                chunkSize;
            const std::vector<glm::vec3> first = borderVertices(
                chunk, xAxis, border);
            const std::vector<glm::vec3> second = borderVertices(
                neighbor->second, xAxis, border);
            compareDirections(first, second, audit);
            compareDirections(second, first, audit);
            ++audit.neighborPairs;
            if (!audit.valid)
            {
                std::fprintf(stderr,
                    "World-space terrain border mismatch: chunks=(%d,%d) and "
                    "(%d,%d), axis=%c border=%.5f max_error=%.9f\n",
                    coordinate.first, coordinate.second,
                    neighborCoordinate.first, neighborCoordinate.second,
                    xAxis ? 'x' : 'z', border,
                    audit.maximumPositionError);
                return audit;
            }
        }
    }
    return audit;
}
}

int main(int argumentCount, char** arguments)
{
    Engine::Scene::Scene scene;
    // Rendering calls MapSpatialMatrix even in ordinary scenes. With no warp
    // volumes, large coordinates must retain an exact identity Jacobian;
    // finite-differencing identity used to shrink 128-unit chunks by 3 units.
    const glm::vec3 distantPoint(1850.666625f, -6.f, 1125.333375f);
    const auto identitySample = scene.SampleSpatialPoint(distantPoint,
        { Engine::Scene::Scene::SpatialQueryDomain::Rendering, nullptr });
    const glm::mat4 distantMatrix = glm::translate(
        glm::mat4(1.f), distantPoint);
    const glm::mat4 mappedDistantMatrix = scene.MapSpatialMatrix(distantMatrix,
        { Engine::Scene::Scene::SpatialQueryDomain::Rendering, nullptr });
    if (identitySample.point != distantPoint ||
        identitySample.jacobian != glm::mat3(1.f) ||
        mappedDistantMatrix != distantMatrix)
        return 43;
    Engine::Core::Object* terrainObject = scene.AddObject("Editor Terrain");
    terrainObject->AddComponent<PerlinNoiseField>();
    TerrainGen* terrain =
        terrainObject->AddComponent<TerrainGen>();
    terrain->viewerObjectName.clear();
    terrain->viewRadiusInChunks = 1;
    terrain->horizontalCellsPerChunk = 4;
    terrain->verticalCells = 4;
    terrain->patchesPerAxis = 2;
    terrain->generateColliders = false;

    // This deliberately does not call Scene::Start(). It exercises the same
    // immediate path used by the component's editor button.
    if (!terrain->GenerateTerrain() || terrain->GetLoadedChunkCount() != 9u ||
        terrainObject->Children.size() != 9u)
    {
        std::fprintf(stderr, "Editor generation did not create the 3x3 chunk ring\n");
        return 1;
    }

    std::size_t patchCount = 0;
    for (Engine::Core::Object* chunkObject : terrainObject->Children)
    {
        TerrainChunk* chunk = chunkObject
            ? chunkObject->GetComponent<TerrainChunk>() : nullptr;
        if (!chunk || chunk->patchCount != 4 || chunkObject->Children.size() != 4u)
            return 2;
        for (Engine::Core::Object* patchObject : chunkObject->Children)
        {
            TerrainPatch* patch = patchObject
                ? patchObject->GetComponent<TerrainPatch>() : nullptr;
            Engine::Components::Mesh* mesh = patchObject
                ? patchObject->GetComponent<Engine::Components::Mesh>() : nullptr;
            if (!patch || !mesh ||
                !patchObject->GetComponent<Engine::Components::Material>() ||
                static_cast<std::size_t>(patch->triangleCount) * 3u !=
                    mesh->GetVertexCount())
            {
                return 3;
            }
            ++patchCount;
        }
    }
    if (patchCount != 36u)
        return 4;
    if (!AuditSmoothGridTopology(*terrainObject, terrain->chunkSize))
        return 36;

    // Procedural vertices are not serialized. Standalone loading must reject
    // editor-saved chunk shells and regenerate their meshes and transforms.
    Engine::Scene::Scene standaloneReloadScene;
    if (!Engine::Serialization::SceneSerializer::Load(standaloneReloadScene,
            "Engine/Core/Assets/Scenes/Procedural/terrain_gen.scene", nullptr))
        return 37;
    auto* reloadedTerrainObject = standaloneReloadScene.FindObjectByName(
        "Base TerrainGen");
    auto* reloadedTerrain = reloadedTerrainObject
        ? reloadedTerrainObject->GetComponent<TerrainGen>() : nullptr;
    if (!reloadedTerrain)
        return 38;
    standaloneReloadScene.Start();
    if (reloadedTerrain->GetLoadedChunkCount() != 0u)
        return 39;
    bool standaloneTerrainRebuilt = false;
    for (int update = 0; update < 5000; ++update)
    {
        standaloneReloadScene.Update(1.f / 60.f);
        if (reloadedTerrain->GetLoadedChunkCount() == 9u &&
            reloadedTerrain->GetQueuedChunkCount() == 0u &&
            reloadedTerrain->GetInFlightChunkCount() == 0u)
        {
            standaloneTerrainRebuilt = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const bool standaloneTopologyValid = standaloneTerrainRebuilt &&
        AuditSmoothGridTopology(*reloadedTerrainObject,
            reloadedTerrain->chunkSize);
    if (!standaloneTerrainRebuilt ||
        reloadedTerrainObject->Children.size() != 9u ||
        !standaloneTopologyValid)
    {
        std::fprintf(stderr,
            "Standalone shell rebuild failed: rebuilt=%d loaded=%zu children=%zu "
            "queued=%zu inFlight=%zu topology=%d\n",
            standaloneTerrainRebuilt ? 1 : 0,
            reloadedTerrain->GetLoadedChunkCount(),
            reloadedTerrainObject->Children.size(),
            reloadedTerrain->GetQueuedChunkCount(),
            reloadedTerrain->GetInFlightChunkCount(),
            standaloneTopologyValid ? 1 : 0);
        return 40;
    }

    // Smooth chunks must produce the same contour positions and normals on a
    // shared border. Otherwise tiny cracks or lighting seams appear as the
    // camera moves away from the origin.
    struct BorderVertex
    {
        glm::vec3 position{};
        glm::vec3 normal{};
    };
    const auto findChunk = [&](int x, int z)
    {
        for (Engine::Core::Object* chunkObject : terrainObject->Children)
        {
            TerrainChunk* chunk = chunkObject
                ? chunkObject->GetComponent<TerrainChunk>() : nullptr;
            if (chunk && chunk->chunkX == x && chunk->chunkZ == z)
                return chunkObject;
        }
        return static_cast<Engine::Core::Object*>(nullptr);
    };
    const auto borderVertices = [](Engine::Core::Object* chunkObject,
        float borderX)
    {
        std::vector<BorderVertex> result;
        if (!chunkObject)
            return result;
        for (Engine::Core::Object* patchObject : chunkObject->Children)
        {
            const auto* mesh = patchObject
                ? patchObject->GetComponent<Engine::Components::Mesh>() : nullptr;
            if (!mesh)
                continue;
            const glm::mat4 world = patchObject->transform.GetWorldMatrix();
            for (const auto& vertex : mesh->GetVertices())
            {
                const glm::vec3 position = glm::vec3(world * glm::vec4(
                    vertex.pos[0], vertex.pos[1], vertex.pos[2], 1.f));
                if (std::abs(position.x - borderX) > 0.0001f)
                    continue;
                result.push_back({ position, glm::vec3(vertex.normal[0],
                    vertex.normal[1], vertex.normal[2]) });
            }
        }
        return result;
    };
    const float sharedBorderX = terrain->chunkSize;
    const auto leftBorder = borderVertices(findChunk(0, 0), sharedBorderX);
    const auto rightBorder = borderVertices(findChunk(1, 0), sharedBorderX);
    if (leftBorder.empty() || rightBorder.empty())
        return 25;
    for (const BorderVertex& left : leftBorder)
    {
        const bool matches = std::any_of(rightBorder.begin(), rightBorder.end(),
            [&](const BorderVertex& right)
            {
                return glm::length(left.position - right.position) < 0.000001f &&
                    glm::length(left.normal - right.normal) < 0.002f;
            });
        if (!matches)
            return 26;
    }

    // Cube mode may triangulate square faces for the GPU, but every triangle
    // must lie on an axis-aligned plane and use one of the selected height
    // range colors. No diagonal terrain connection is permitted.
    terrain->viewRadiusInChunks = 0;
    terrain->horizontalCellsPerChunk = 6;
    terrain->patchesPerAxis = 2;
    terrain->terrainShape = static_cast<int>(
        TerrainGen::TerrainShape::Cubes);
    terrain->heightStep = 1.f;
    terrain->lowHeightColor = { 1.f, 0.f, 0.f };
    terrain->middleHeightColor = { 0.f, 1.f, 0.f };
    terrain->highHeightColor = { 0.f, 0.f, 1.f };
    if (!terrain->GenerateTerrain())
        return 5;
    std::size_t cubeVertexCount = 0;
    for (Engine::Core::Object* chunkObject : terrainObject->Children)
    {
        for (Engine::Core::Object* patchObject : chunkObject->Children)
        {
            Engine::Components::Mesh* mesh = patchObject
                ? patchObject->GetComponent<Engine::Components::Mesh>() : nullptr;
            if (!mesh)
                return 6;
            const auto& vertices = mesh->GetVertices();
            cubeVertexCount += vertices.size();
            for (std::size_t index = 0; index + 2 < vertices.size(); index += 3)
            {
                const auto& vertex = vertices[index];
                const glm::vec3 normal(vertex.normal[0], vertex.normal[1],
                    vertex.normal[2]);
                const glm::vec3 absoluteNormal = glm::abs(normal);
                const int axisCount = static_cast<int>(absoluteNormal.x > 0.999f) +
                    static_cast<int>(absoluteNormal.y > 0.999f) +
                    static_cast<int>(absoluteNormal.z > 0.999f);
                if (axisCount != 1 || std::abs(glm::length(normal) - 1.f) > 0.001f)
                    return 7;
                const glm::vec3 first(vertex.pos[0], vertex.pos[1], vertex.pos[2]);
                const auto& secondVertex = vertices[index + 1];
                const auto& thirdVertex = vertices[index + 2];
                const glm::vec3 second(secondVertex.pos[0], secondVertex.pos[1],
                    secondVertex.pos[2]);
                const glm::vec3 third(thirdVertex.pos[0], thirdVertex.pos[1],
                    thirdVertex.pos[2]);
                if (std::abs(glm::dot(second - first, normal)) > 0.001f ||
                    std::abs(glm::dot(third - first, normal)) > 0.001f)
                    return 8;
                const glm::vec3 color(vertex.color[0], vertex.color[1],
                    vertex.color[2]);
                const bool selectedBand = glm::length(color - terrain->lowHeightColor) < 0.001f ||
                    glm::length(color - terrain->middleHeightColor) < 0.001f ||
                    glm::length(color - terrain->highHeightColor) < 0.001f;
                if (!selectedBand)
                    return 9;
            }
        }
    }
    if (cubeVertexCount == 0u)
        return 10;

    // The generalized generator must produce distinct, valid mesh topology
    // for both a triangulated height field and hexagonal columns.
    terrain->terrainShape = static_cast<int>(TerrainGen::TerrainShape::Triangles);
    terrain->patchesPerAxis = 1;
    if (!terrain->GenerateTerrain())
        return 18;
    const auto* triangleMesh = terrainObject->Children.front()->Children.front()
        ->GetComponent<Engine::Components::Mesh>();
    if (!triangleMesh || triangleMesh->GetVertexCount() !=
        static_cast<std::size_t>(terrain->horizontalCellsPerChunk *
            terrain->horizontalCellsPerChunk * 6))
        return 19;

    terrain->terrainShape = static_cast<int>(TerrainGen::TerrainShape::Hexagons);
    terrain->viewRadiusInChunks = 1;
    if (!terrain->GenerateTerrain())
        return 20;
    const auto* hexMesh = terrainObject->Children.front()->Children.front()
        ->GetComponent<Engine::Components::Mesh>();
    bool foundHexSide = false;
    if (!hexMesh || hexMesh->GetVertexCount() == 0u ||
        hexMesh->GetVertexCount() % 3u != 0u)
        return 21;
    for (const auto& vertex : hexMesh->GetVertices())
    {
        const glm::vec3 normal(vertex.normal[0], vertex.normal[1], vertex.normal[2]);
        if (std::abs(normal.y) < 0.001f && std::abs(normal.x) > 0.1f &&
            std::abs(normal.z) > 0.1f)
            foundHexSide = true;
    }
    if (!foundHexSide)
        return 22;
    struct OwnedHexCenter
    {
        glm::vec3 position{};
        const Engine::Core::Object* chunk = nullptr;
        int triangleCount = 0;
    };
    std::vector<OwnedHexCenter> ownedHexCenters;
    for (Engine::Core::Object* chunkObject : terrainObject->Children)
    {
        for (Engine::Core::Object* patchObject : chunkObject->Children)
        {
            const auto* mesh = patchObject
                ? patchObject->GetComponent<Engine::Components::Mesh>() : nullptr;
            if (!mesh)
                continue;
            const glm::mat4 world = patchObject->transform.GetWorldMatrix();
            const auto& vertices = mesh->GetVertices();
            for (size_t triangle = 0; triangle + 2 < vertices.size(); triangle += 3)
            {
                if (vertices[triangle].normal[1] < 0.999f)
                    continue;
                const glm::vec3 center = glm::vec3(world * glm::vec4(
                    vertices[triangle].pos[0], vertices[triangle].pos[1],
                    vertices[triangle].pos[2], 1.f));
                auto existing = std::find_if(ownedHexCenters.begin(),
                    ownedHexCenters.end(), [&](const OwnedHexCenter& value)
                    {
                        return glm::length(value.position - center) < 0.000001f;
                    });
                if (existing == ownedHexCenters.end())
                    ownedHexCenters.push_back({ center, chunkObject, 1 });
                else
                {
                    if (existing->chunk != chunkObject)
                        return 32;
                    ++existing->triangleCount;
                }
            }
        }
    }
    if (ownedHexCenters.empty() || std::any_of(ownedHexCenters.begin(),
        ownedHexCenters.end(), [](const OwnedHexCenter& value)
        {
            return value.triangleCount != 6;
        }))
        return 33;

    terrain->terrainShape = static_cast<int>(TerrainGen::TerrainShape::Cubes);
    terrain->patchesPerAxis = 2;

    Engine::Core::Object* viewer = scene.AddObject("Streaming Test Viewer");
    terrain->viewerObjectName = viewer->name;
    terrain->viewRadiusInChunks = 1;
    terrain->maxChunkBuildsPerUpdate = 16;
    viewer->transform.position = { 0.f, 10.f, 0.f };
    if (!terrain->GenerateTerrain() || terrain->GetLoadedChunkCount() != 9u)
        return 11;
    scene.Start();
    const auto waitForStreaming = [&scene, terrain](std::size_t expectedChunks)
    {
        for (int update = 0; update < 500; ++update)
        {
            scene.Update(1.f / 60.f);
            if (terrain->GetLoadedChunkCount() == expectedChunks &&
                terrain->GetQueuedChunkCount() == 0u &&
                terrain->GetInFlightChunkCount() == 0u)
                return true;
            std::this_thread::yield();
        }
        return false;
    };
    viewer->transform.position.x = terrain->chunkSize * 3.f;
    if (!waitForStreaming(9u) ||
        terrain->GetTotalChunksBuilt() < 18u ||
        terrain->GetTotalChunksUnloaded() < 9u ||
        !std::isfinite(terrain->GetLastStreamingMilliseconds()) ||
        terrain->GetMaximumStreamingMilliseconds() <
            terrain->GetLastStreamingMilliseconds())
        return 12;
    viewer->transform.position.x = 0.f;
    if (!waitForStreaming(9u) ||
        terrain->GetMeshCacheHits() < 9u ||
        terrain->GetCachedChunkCount() == 0u)
    {
        std::fprintf(stderr, "Cache return failed: loaded=%zu queued=%zu in_flight=%zu hits=%llu cached=%zu\n",
            terrain->GetLoadedChunkCount(), terrain->GetQueuedChunkCount(),
            terrain->GetInFlightChunkCount(),
            static_cast<unsigned long long>(terrain->GetMeshCacheHits()),
            terrain->GetCachedChunkCount());
        return 13;
    }

    terrain->ClearTerrain();
    if (terrain->GetLoadedChunkCount() != 0u ||
        terrain->GetCachedChunkCount() != 0u ||
        terrain->GetQueuedChunkCount() != 0u ||
        terrain->GetInFlightChunkCount() != 0u ||
        !terrainObject->Children.empty())
        return 23;
    if (!terrain->GenerateTerrain() || terrain->GetLoadedChunkCount() != 9u)
        return 24;

    Engine::Scene::Scene performanceScene;
    if (!Engine::Serialization::SceneSerializer::Load(performanceScene,
        "Engine/Core/Assets/Scenes/Procedural/terrain_gen_streaming.scene",
        nullptr))
        return 14;
    auto* loadedTerrainObject = performanceScene.FindObjectByName(
        "Long Range Terrain");
    auto* loadedCameraObject = performanceScene.FindObjectByName(
        "Long Range Terrain Camera");
    auto* loadedTerrain = loadedTerrainObject
        ? loadedTerrainObject->GetComponent<TerrainGen>() : nullptr;
    auto* loadedDriver = loadedCameraObject
        ? loadedCameraObject->GetComponent<TerrainStreamingCameraDriver>() : nullptr;
    if (!loadedTerrain || !loadedCameraObject || !loadedDriver ||
        static_cast<TerrainGen::TerrainShape>(loadedTerrain->terrainShape) !=
            TerrainGen::TerrainShape::SmoothSurface ||
        loadedTerrain->chunkSize * loadedTerrain->viewRadiusInChunks < 1000.f)
        return 15;
    loadedDriver->waitForInitialTerrain = false;
    loadedDriver->Start();
    const glm::vec3 cameraStart = loadedCameraObject->transform.position;
    loadedDriver->Update();
    const glm::vec3 cameraDelta = loadedCameraObject->transform.position - cameraStart;
    glm::vec3 expectedDirection = loadedDriver->endPosition -
        loadedDriver->startPosition;
    expectedDirection.y = 0.f;
    expectedDirection = glm::normalize(expectedDirection);
    const glm::vec3 cameraForward = glm::normalize(glm::vec3(
        loadedCameraObject->transform.GetWorldMatrix()[2]));
    if (!loadedDriver->moveInfinitely ||
        glm::dot(cameraDelta, expectedDirection) <= 0.f ||
        glm::dot(glm::normalize(glm::vec3(expectedDirection.x,
            -loadedDriver->downwardLook, expectedDirection.z)),
            cameraForward) < 0.999f)
        return 16;

    // Verify the production long-range resolution at its negative-coordinate
    // starting area, where floating-point border errors are easiest to expose.
    loadedDriver->waitForInitialTerrain = true;
    loadedDriver->terrainObjectName = loadedTerrainObject->name;
    loadedTerrain->ClearTerrain();
    loadedDriver->Start();
    const glm::vec3 waitingPosition = loadedCameraObject->transform.position;
    loadedDriver->Update();
    if (glm::length(loadedCameraObject->transform.position - waitingPosition) >
        0.000001f)
        return 34;
    loadedTerrain->viewRadiusInChunks = 1;
    if (!loadedTerrain->GenerateTerrain())
        return 27;
    loadedDriver->Update();
    if (glm::length(loadedCameraObject->transform.position - waitingPosition) <=
        0.000001f)
        return 35;
    const auto findLoadedChunk = [&](int x, int z)
    {
        for (Engine::Core::Object* chunkObject : loadedTerrainObject->Children)
        {
            TerrainChunk* chunk = chunkObject
                ? chunkObject->GetComponent<TerrainChunk>() : nullptr;
            if (chunk && chunk->chunkX == x && chunk->chunkZ == z)
                return chunkObject;
        }
        return static_cast<Engine::Core::Object*>(nullptr);
    };
    const auto bordersMatch = [&](Engine::Core::Object* first,
        Engine::Core::Object* second, float borderX)
    {
        const auto firstBorder = borderVertices(first, borderX);
        const auto secondBorder = borderVertices(second, borderX);
        if (firstBorder.empty() || secondBorder.empty())
            return false;
        const auto allMatch = [](const std::vector<BorderVertex>& source,
            const std::vector<BorderVertex>& target)
        {
            return std::all_of(source.begin(), source.end(),
                [&](const BorderVertex& sourceVertex)
                {
                    return std::any_of(target.begin(), target.end(),
                        [&](const BorderVertex& targetVertex)
                        {
                            return glm::length(sourceVertex.position -
                                targetVertex.position) < 0.000001f &&
                                glm::length(sourceVertex.normal -
                                    targetVertex.normal) < 0.002f;
                        });
                });
        };
        return allMatch(firstBorder, secondBorder) &&
            allMatch(secondBorder, firstBorder);
    };
    if (!bordersMatch(findLoadedChunk(-6, -2), findLoadedChunk(-5, -2),
        -640.f))
        return 28;
    struct BorderSegment { glm::vec3 first{}, second{}; };
    const auto xBorderSegments = [](Engine::Core::Object* chunkObject,
        float borderX)
    {
        std::vector<BorderSegment> result;
        if (!chunkObject)
            return result;
        for (Engine::Core::Object* patchObject : chunkObject->Children)
        {
            const auto* mesh = patchObject
                ? patchObject->GetComponent<Engine::Components::Mesh>() : nullptr;
            if (!mesh)
                continue;
            const glm::mat4 world = patchObject->transform.GetWorldMatrix();
            const auto& vertices = mesh->GetVertices();
            for (size_t triangle = 0; triangle + 2 < vertices.size(); triangle += 3)
            {
                glm::vec3 points[3]{};
                for (int corner = 0; corner < 3; ++corner)
                    points[corner] = glm::vec3(world * glm::vec4(
                        vertices[triangle + corner].pos[0],
                        vertices[triangle + corner].pos[1],
                        vertices[triangle + corner].pos[2], 1.f));
                for (int edge = 0; edge < 3; ++edge)
                {
                    const glm::vec3& first = points[edge];
                    const glm::vec3& second = points[(edge + 1) % 3];
                    if (std::abs(first.x - borderX) <= 0.000001f &&
                        std::abs(second.x - borderX) <= 0.000001f &&
                        glm::length(first - second) > 0.000001f)
                        result.push_back({ first, second });
                }
            }
        }
        return result;
    };
    const auto leftSegments = xBorderSegments(findLoadedChunk(-6, -2), -640.f);
    const auto rightSegments = xBorderSegments(findLoadedChunk(-5, -2), -640.f);
    const auto segmentMatches = [](const BorderSegment& segment,
        const std::vector<BorderSegment>& candidates)
    {
        return std::any_of(candidates.begin(), candidates.end(),
            [&](const BorderSegment& candidate)
            {
                const bool forward = glm::length(segment.first - candidate.first) <
                    0.000001f && glm::length(segment.second - candidate.second) < 0.000001f;
                const bool reverse = glm::length(segment.first - candidate.second) <
                    0.000001f && glm::length(segment.second - candidate.first) < 0.000001f;
                return forward || reverse;
            });
    };
    if (leftSegments.empty() || rightSegments.empty() ||
        !std::all_of(leftSegments.begin(), leftSegments.end(),
            [&](const BorderSegment& segment)
            {
                return segmentMatches(segment, rightSegments);
            }) ||
        !std::all_of(rightSegments.begin(), rightSegments.end(),
            [&](const BorderSegment& segment)
            {
                return segmentMatches(segment, leftSegments);
            }))
        return 31;
    const auto zBorderVertices = [](Engine::Core::Object* chunkObject,
        float borderZ)
    {
        std::vector<BorderVertex> result;
        if (!chunkObject)
            return result;
        for (Engine::Core::Object* patchObject : chunkObject->Children)
        {
            const auto* mesh = patchObject
                ? patchObject->GetComponent<Engine::Components::Mesh>() : nullptr;
            if (!mesh)
                continue;
            const glm::mat4 world = patchObject->transform.GetWorldMatrix();
            for (const auto& vertex : mesh->GetVertices())
            {
                const glm::vec3 position = glm::vec3(world * glm::vec4(
                    vertex.pos[0], vertex.pos[1], vertex.pos[2], 1.f));
                if (std::abs(position.z - borderZ) <= 0.0001f)
                    result.push_back({ position, glm::vec3(vertex.normal[0],
                        vertex.normal[1], vertex.normal[2]) });
            }
        }
        return result;
    };
    const auto lowerZBorder = zBorderVertices(findLoadedChunk(-6, -2), -128.f);
    const auto upperZBorder = zBorderVertices(findLoadedChunk(-6, -1), -128.f);
    if (lowerZBorder.empty() || upperZBorder.empty())
        return 29;
    for (const BorderVertex& vertex : lowerZBorder)
    {
        if (!std::any_of(upperZBorder.begin(), upperZBorder.end(),
            [&](const BorderVertex& other)
            {
                return glm::length(vertex.position - other.position) < 0.000001f &&
                    glm::length(vertex.normal - other.normal) < 0.002f;
            }))
            return 30;
    }

    const uint64_t streamedBuilds = terrain->GetTotalChunksBuilt();
    const uint64_t streamedUnloads = terrain->GetTotalChunksUnloaded();
    const double maximumStreamingMilliseconds =
        terrain->GetMaximumStreamingMilliseconds();

    terrain->viewRadiusInChunks = 0;
    terrain->patchesPerAxis = 1;
    if (!terrain->GenerateTerrain() || terrain->GetLoadedChunkCount() != 1u ||
        terrainObject->Children.size() != 1u ||
        terrainObject->Children.front()->Children.size() != 1u)
    {
        std::fprintf(stderr, "Editor regeneration left stale generated objects\n");
        return 17;
    }

    if (argumentCount > 1 && std::strcmp(arguments[1], "--stress") == 0)
    {
        Engine::Scene::Scene stressScene;
        auto* stressViewer = stressScene.AddObject("Stress Viewer");
        auto* stressOwner = stressScene.AddObject("Stress Terrain");
        auto* stressNoise = stressOwner->AddComponent<PerlinNoiseField>();
        auto* stressTerrain = stressOwner->AddComponent<TerrainGen>();
        const bool longRange = argumentCount > 3 &&
            std::strcmp(arguments[3], "long-range") == 0;
        const int requestedRadius = argumentCount > 4
            ? std::clamp(std::atoi(arguments[4]), 0, 8)
            : (longRange ? 8 : 4);
        stressTerrain->viewerObjectName = stressViewer->name;
        stressTerrain->chunkSize = longRange ? 128.f : 32.f;
        stressTerrain->viewRadiusInChunks = requestedRadius;
        stressTerrain->maxChunkBuildsPerUpdate = 16;
        stressTerrain->parallelChunkBuilds = longRange ? 4 : 8;
        stressTerrain->maxChunkCommitsPerUpdate = longRange ? 1 : 4;
        stressTerrain->horizontalCellsPerChunk = longRange ? 24 : 32;
        stressTerrain->verticalCells = longRange ? 16 : 32;
        stressTerrain->patchesPerAxis = longRange ? 1 : 2;
        // This profile measures first-time generation, not revisiting old
        // chunks. A large vertex cache would mix memory pressure into it.
        stressTerrain->cacheUnloadedChunkMeshes = false;
        TerrainGen::TerrainShape stressShape = TerrainGen::TerrainShape::SmoothSurface;
        const char* stressShapeName = "smooth";
        if (argumentCount > 2 && std::strcmp(arguments[2], "cubes") == 0)
        {
            stressShape = TerrainGen::TerrainShape::Cubes;
            stressShapeName = "cubes";
        }
        else if (argumentCount > 2 && std::strcmp(arguments[2], "triangles") == 0)
        {
            stressShape = TerrainGen::TerrainShape::Triangles;
            stressShapeName = "triangles";
        }
        else if (argumentCount > 2 && std::strcmp(arguments[2], "hexagons") == 0)
        {
            stressShape = TerrainGen::TerrainShape::Hexagons;
            stressShapeName = "hexagons";
        }
        stressTerrain->terrainShape = static_cast<int>(stressShape);
        stressTerrain->caveStrength = 2.1f;
        stressTerrain->unloadedMeshCacheCapacity = 256;
        if (longRange)
        {
            stressNoise->seed = 9187;
            stressNoise->frequency = 0.035f;
            stressNoise->octaves = 4;
            stressNoise->lacunarity = 2.f;
            stressNoise->persistence = 0.5f;
            stressNoise->coordinateOffset = { 41.f, 0.f, -27.f };
            stressTerrain->verticalSize = 96.f;
            stressTerrain->heightAmplitude = 36.f;
            stressTerrain->caveFrequencyMultiplier = 2.35f;
        }
        const int startChunkX = argumentCount > 5
            ? std::atoi(arguments[5]) : 0;
        const int startChunkZ = argumentCount > 6
            ? std::atoi(arguments[6]) : 0;
        stressViewer->transform.position = {
            static_cast<float>(startChunkX) * stressTerrain->chunkSize,
            12.f,
            static_cast<float>(startChunkZ) * stressTerrain->chunkSize };
        stressScene.Start();

        const std::size_t diameter = static_cast<std::size_t>(
            stressTerrain->viewRadiusInChunks * 2 + 1);
        const std::size_t desiredChunks = diameter * diameter;
        const auto fillStart = std::chrono::steady_clock::now();
        while ((stressTerrain->GetLoadedChunkCount() != desiredChunks ||
            stressTerrain->GetQueuedChunkCount() != 0u ||
            stressTerrain->GetInFlightChunkCount() != 0u) &&
            std::chrono::steady_clock::now() - fillStart < std::chrono::seconds(60))
        {
            stressScene.Update(1.f / 60.f);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        const double initialFillSeconds = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - fillStart).count();
        const bool initialTopologyValid =
            stressShape != TerrainGen::TerrainShape::SmoothSurface ||
            (stressTerrain->GetLoadedChunkCount() == desiredChunks &&
                AuditSmoothGridTopology(*stressOwner, stressTerrain->chunkSize));
        const SharedBorderAudit initialBorderAudit =
            stressShape == TerrainGen::TerrainShape::SmoothSurface
                ? AuditSharedChunkWorldVertices(
                    *stressOwner, stressTerrain->chunkSize)
                : SharedBorderAudit{};
        if (!initialTopologyValid || !initialBorderAudit.valid)
            return 41;

        double worstUpdateMs = 0.0;
        int updatesOverBudget = 0;
        std::size_t maximumMissingChunks = 0u;
        constexpr int movementFrames = 600;
        const float cameraSpeed = longRange ? 160.f : 96.f;
        auto nextFrame = std::chrono::steady_clock::now();
        for (int frame = 0; frame < movementFrames; ++frame)
        {
            nextFrame += std::chrono::milliseconds(16);
            stressViewer->transform.position.x += cameraSpeed / 60.f;
            const auto updateStart = std::chrono::steady_clock::now();
            stressScene.Update(1.f / 60.f);
            const double updateMs = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - updateStart).count();
            worstUpdateMs = std::max(worstUpdateMs, updateMs);
            if (updateMs > 16.667)
                ++updatesOverBudget;
            maximumMissingChunks = std::max(maximumMissingChunks,
                desiredChunks > stressTerrain->GetLoadedChunkCount()
                    ? desiredChunks - stressTerrain->GetLoadedChunkCount() : 0u);
            std::this_thread::sleep_until(nextFrame);
        }
        const auto drainStart = std::chrono::steady_clock::now();
        while ((stressTerrain->GetLoadedChunkCount() != desiredChunks ||
            stressTerrain->GetQueuedChunkCount() != 0u ||
            stressTerrain->GetInFlightChunkCount() != 0u) &&
            std::chrono::steady_clock::now() - drainStart < std::chrono::seconds(60))
        {
            stressScene.Update(1.f / 60.f);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        const double drainSeconds = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - drainStart).count();
        const bool finalTopologyValid =
            stressShape != TerrainGen::TerrainShape::SmoothSurface ||
            (stressTerrain->GetLoadedChunkCount() == desiredChunks &&
                AuditSmoothGridTopology(*stressOwner, stressTerrain->chunkSize));
        const SharedBorderAudit finalBorderAudit =
            stressShape == TerrainGen::TerrainShape::SmoothSurface
                ? AuditSharedChunkWorldVertices(
                    *stressOwner, stressTerrain->chunkSize)
                : SharedBorderAudit{};
        if (!finalTopologyValid || !finalBorderAudit.valid)
            return 42;
        std::size_t residentVertices = 0u;
        for (Engine::Core::Object* chunkObject : stressOwner->Children)
        {
            if (!chunkObject || !chunkObject->GetComponent<TerrainChunk>())
                continue;
            for (Engine::Core::Object* patchObject : chunkObject->Children)
            {
                const auto* mesh = patchObject
                    ? patchObject->GetComponent<Engine::Components::Mesh>() : nullptr;
                if (mesh)
                    residentVertices += mesh->GetVertexCount();
            }
        }
        const double residentMeshMiB = static_cast<double>(residentVertices) *
            sizeof(Engine::Model::Vertex) / (1024.0 * 1024.0);
        std::printf("stress shape=%s initial_fill_s=%.3f camera_speed=%.1f chunk_size=%.1f "
            "radius=%d cells=%dx%d worst_update_ms=%.3f over_budget=%d/%d "
            "max_missing=%zu drain_s=%.3f resident_chunks=%zu resident_vertices=%zu "
            "mesh_mib=%.2f topology=valid border_pairs=%zu "
            "border_vertices=%zu max_border_error=%.9f built=%llu unloaded=%llu\n",
            stressShapeName, initialFillSeconds, cameraSpeed, stressTerrain->chunkSize,
            stressTerrain->viewRadiusInChunks,
            stressTerrain->horizontalCellsPerChunk, stressTerrain->verticalCells,
            worstUpdateMs, updatesOverBudget,
            movementFrames, maximumMissingChunks, drainSeconds,
            stressTerrain->GetLoadedChunkCount(), residentVertices, residentMeshMiB,
            finalBorderAudit.neighborPairs,
            finalBorderAudit.comparedVertices,
            finalBorderAudit.maximumPositionError,
            static_cast<unsigned long long>(stressTerrain->GetTotalChunksBuilt()),
            static_cast<unsigned long long>(stressTerrain->GetTotalChunksUnloaded()));
    }

    std::printf("terrain_gen_generation chunks=1 patches=1 "
        "initial_chunks=9 initial_patches=%zu cube_vertices=%zu "
        "streamed_builds=%llu streamed_unloads=%llu max_stream_ms=%.3f "
        "runtime_streaming_tested=true\n",
        patchCount, cubeVertexCount,
        static_cast<unsigned long long>(streamedBuilds),
        static_cast<unsigned long long>(streamedUnloads),
        maximumStreamingMilliseconds);
    return 0;
}

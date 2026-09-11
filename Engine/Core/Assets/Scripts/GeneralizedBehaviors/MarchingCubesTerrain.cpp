#include "Scripts/GeneralizedBehaviors/MarchingCubesTerrain.h"

#include "Scripts/GeneralizedBehaviors/MarchingCubesChunk.h"
#include "Scripts/GeneralizedBehaviors/MarchingCubesQuad.h"
#include "Scripts/GeneralizedBehaviors/PerlinNoiseField.h"
#include "Core/Compoonents/Materials/Material.h"
#include "Core/Compoonents/Obj/Mesh.h"
#include "Core/Compoonents/Physics/Collider.h"
#include "Core/Compoonents/Physics/RigidBody.h"
#include "Core/Object.h"
#include "Core/Scene/Scene.h"
#include "Core/Serialization/SceneSerializer.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

namespace
{
using Object = Engine::Core::Object;
using Scene = Engine::Scene::Scene;

Object* AddChild(Scene& scene, Object& parent, const std::string& name)
{
    Object* child = scene.AddObject(name);
    child->Parent = &parent;
    parent.Children.push_back(child);
    return child;
}

template<typename T>
T* EnsureComponent(Object& object)
{
    if (T* existing = object.GetComponent<T>())
        return existing;
    return object.AddComponent<T>();
}
}

MarchingCubesTerrain::MarchingCubesTerrain()
{
    SetTypeName(COMPONENT_TYPE_NAME(MarchingCubesTerrain));
    RegisterField("viewerObjectName", viewerObjectName);
    RegisterField("noiseObjectName", noiseObjectName);
    RegisterField("chunkSize", chunkSize);
    RegisterField("viewRadiusInChunks", viewRadiusInChunks);
    RegisterField("maxChunkBuildsPerUpdate", maxChunkBuildsPerUpdate);
    RegisterField("horizontalCellsPerChunk", horizontalCellsPerChunk);
    RegisterField("verticalCells", verticalCells);
    RegisterField("quadsPerAxis", quadsPerAxis);
    RegisterField("verticalSize", verticalSize);
    RegisterField("baseHeight", baseHeight);
    RegisterField("heightAmplitude", heightAmplitude);
    RegisterField("caveStrength", caveStrength);
    RegisterField("caveFrequencyMultiplier", caveFrequencyMultiplier);
    RegisterField("isoLevel", isoLevel);
    RegisterField("textureScale", textureScale);
    RegisterField("generateColliders", generateColliders);
}

namespace
{
struct MarchingCubesTerrainRegistration
{
    MarchingCubesTerrainRegistration()
    {
        Engine::Serialization::RegisterComponentType<MarchingCubesTerrain>(
            "MarchingCubesTerrain");
    }
};
MarchingCubesTerrainRegistration g_registration;
}

int64_t MarchingCubesTerrain::ChunkKey(int x, int z)
{
    const uint64_t key = (static_cast<uint64_t>(static_cast<uint32_t>(x)) << 32u) |
        static_cast<uint32_t>(z);
    return static_cast<int64_t>(key);
}

PerlinNoiseField* MarchingCubesTerrain::ResolveNoise() const
{
    if (!Owner)
        return nullptr;
    if (!noiseObjectName.empty())
    {
        Object* noiseObject = Owner->FindObjectInSceneByName(noiseObjectName);
        if (noiseObject)
            return noiseObject->GetComponent<PerlinNoiseField>();
    }
    return Owner->GetComponent<PerlinNoiseField>();
}

glm::ivec2 MarchingCubesTerrain::ViewerChunk() const
{
    if (!Owner)
        return {};
    Object* viewer = viewerObjectName.empty()
        ? nullptr : Owner->FindObjectInSceneByName(viewerObjectName);
    if (!viewer)
        viewer = Owner;
    const glm::vec3 localPosition(glm::inverse(
        Owner->transform.GetWorldMatrix()) * glm::vec4(
            viewer->transform.GetWorldPosition(), 1.f));
    const float size = std::max(1.f, chunkSize);
    return {
        static_cast<int>(std::floor(localPosition.x / size)),
        static_cast<int>(std::floor(localPosition.z / size))
    };
}

void MarchingCubesTerrain::Start()
{
    if (!Owner || !Owner->GetScene() || !ResolveNoise())
        return;

    m_chunks.clear();
    for (Object* child : Owner->Children)
    {
        if (auto* chunk = child ? child->GetComponent<MarchingCubesChunk>() : nullptr)
            m_chunks[ChunkKey(chunk->chunkX, chunk->chunkZ)] = child;
    }

    m_viewerChunk = ViewerChunk();
    m_hasViewerChunk = true;
    // Make the viewer's own chunk available immediately. Neighboring chunks
    // honor the per-update budget to avoid a large streaming hitch. Runtime
    // object insertion is deferred to Update because Scene::Start is walking
    // its stable object array.
    if (m_chunks.find(ChunkKey(m_viewerChunk.x, m_viewerChunk.y)) !=
        m_chunks.end())
    {
        BuildChunk(m_viewerChunk.x, m_viewerChunk.y);
    }
}

void MarchingCubesTerrain::Update()
{
    if (!Owner || !Owner->GetScene() || !ResolveNoise())
        return;
    const glm::ivec2 current = ViewerChunk();
    if (!m_hasViewerChunk || current != m_viewerChunk)
    {
        m_viewerChunk = current;
        m_hasViewerChunk = true;
    }
    // Refresh every frame until the desired ring has filled. Once filled this
    // is a small map lookup and performs no allocations or mesh work.
    RefreshChunks();
}

void MarchingCubesTerrain::OnDestroy()
{
    m_chunks.clear();
}

void MarchingCubesTerrain::RefreshChunks()
{
    if (!Owner || !Owner->GetScene() || !m_hasViewerChunk)
        return;
    const int radius = std::clamp(viewRadiusInChunks, 0, 8);

    for (auto iterator = m_chunks.begin(); iterator != m_chunks.end();)
    {
        MarchingCubesChunk* chunk = iterator->second
            ? iterator->second->GetComponent<MarchingCubesChunk>() : nullptr;
        if (!chunk || std::abs(chunk->chunkX - m_viewerChunk.x) > radius ||
            std::abs(chunk->chunkZ - m_viewerChunk.y) > radius)
        {
            if (iterator->second)
                Owner->GetScene()->RequestRemoveObject(iterator->second);
            iterator = m_chunks.erase(iterator);
        }
        else
            ++iterator;
    }

    struct Candidate { int x = 0; int z = 0; int distance = 0; };
    std::vector<Candidate> missing;
    for (int z = m_viewerChunk.y - radius; z <= m_viewerChunk.y + radius; ++z)
    {
        for (int x = m_viewerChunk.x - radius; x <= m_viewerChunk.x + radius; ++x)
        {
            if (m_chunks.find(ChunkKey(x, z)) == m_chunks.end())
                missing.push_back({ x, z,
                    std::abs(x - m_viewerChunk.x) +
                    std::abs(z - m_viewerChunk.y) });
        }
    }
    std::sort(missing.begin(), missing.end(),
        [](const Candidate& first, const Candidate& second)
        {
            if (first.distance != second.distance)
                return first.distance < second.distance;
            if (first.z != second.z)
                return first.z < second.z;
            return first.x < second.x;
        });

    int budget = std::clamp(maxChunkBuildsPerUpdate, 1, 16);
    for (const Candidate& candidate : missing)
    {
        if (budget-- <= 0)
            break;
        BuildChunk(candidate.x, candidate.z);
    }
}

float MarchingCubesTerrain::Density(const glm::vec3& position,
    const PerlinNoiseField& noise) const
{
    const float surface = baseHeight + heightAmplitude *
        noise.SampleFractal2D(position.x, position.z);
    const float caves = caveStrength > 0.f
        ? caveStrength * noise.SampleFractal(
            position * std::max(0.01f, caveFrequencyMultiplier))
        : 0.f;
    // Negative is solid and positive is air.
    return position.y - surface + caves;
}

std::vector<MarchingCubesTerrain::Vertex>
MarchingCubesTerrain::BuildQuadVertices(int chunkX, int chunkZ,
    int quadX, int quadZ, const PerlinNoiseField& noise) const
{
    const int horizontalCells = std::clamp(horizontalCellsPerChunk, 2, 48);
    const int yCells = std::clamp(verticalCells, 2, 48);
    const int quadAxisCount = std::clamp(quadsPerAxis, 1,
        std::min(8, horizontalCells));
    const int startX = quadX * horizontalCells / quadAxisCount;
    const int endX = (quadX + 1) * horizontalCells / quadAxisCount;
    const int startZ = quadZ * horizontalCells / quadAxisCount;
    const int endZ = (quadZ + 1) * horizontalCells / quadAxisCount;
    const float horizontalStep = std::max(1.f, chunkSize) /
        static_cast<float>(horizontalCells);
    const float ySize = std::max(1.f, verticalSize);
    const float verticalStep = ySize / static_cast<float>(yCells);
    const float minimumY = -ySize * 0.5f;
    const glm::vec3 quadOrigin(
        static_cast<float>(startX) * horizontalStep, 0.f,
        static_cast<float>(startZ) * horizontalStep);
    const glm::vec3 chunkOrigin(
        static_cast<float>(chunkX) * std::max(1.f, chunkSize), 0.f,
        static_cast<float>(chunkZ) * std::max(1.f, chunkSize));

    static constexpr int cubeCorners[8][3] = {
        { 0, 0, 0 }, { 1, 0, 0 }, { 1, 0, 1 }, { 0, 0, 1 },
        { 0, 1, 0 }, { 1, 1, 0 }, { 1, 1, 1 }, { 0, 1, 1 }
    };
    static constexpr int cubeEdges[12][2] = {
        { 0, 1 }, { 1, 2 }, { 2, 3 }, { 3, 0 },
        { 4, 5 }, { 5, 6 }, { 6, 7 }, { 7, 4 },
        { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 }
    };
    // Corners and perimeter edges use matching cyclic order on every face.
    static constexpr int cubeFaces[6][4] = {
        { 0, 1, 2, 3 }, { 4, 5, 6, 7 }, { 0, 1, 5, 4 },
        { 1, 2, 6, 5 }, { 2, 3, 7, 6 }, { 3, 0, 4, 7 }
    };
    static constexpr int faceEdges[6][4] = {
        { 0, 1, 2, 3 }, { 4, 5, 6, 7 }, { 0, 9, 4, 8 },
        { 1, 10, 5, 9 }, { 2, 11, 6, 10 }, { 3, 8, 7, 11 }
    };

    std::vector<Vertex> vertices;
    vertices.reserve(static_cast<size_t>(endX - startX) *
        static_cast<size_t>(endZ - startZ) * yCells * 18u);

    const float gradientStep = std::min(horizontalStep, verticalStep) * 0.2f;
    const auto gradient = [&](const glm::vec3& point)
    {
        const glm::vec3 x(gradientStep, 0.f, 0.f);
        const glm::vec3 y(0.f, gradientStep, 0.f);
        const glm::vec3 z(0.f, 0.f, gradientStep);
        glm::vec3 result(
            Density(point + x, noise) - Density(point - x, noise),
            Density(point + y, noise) - Density(point - y, noise),
            Density(point + z, noise) - Density(point - z, noise));
        const float length = glm::length(result);
        return length > 0.000001f ? result / length : glm::vec3(0.f, 1.f, 0.f);
    };

    const auto makeVertex = [&](const glm::vec3& terrainPosition)
    {
        Vertex vertex{};
        const glm::vec3 local = terrainPosition - chunkOrigin - quadOrigin;
        const glm::vec3 normal = gradient(terrainPosition);
        glm::vec3 tangent = glm::vec3(1.f, 0.f, 0.f) - normal * normal.x;
        if (glm::length(tangent) < 0.0001f)
            tangent = glm::vec3(0.f, 0.f, 1.f);
        tangent = glm::normalize(tangent);
        vertex.pos[0] = local.x; vertex.pos[1] = local.y; vertex.pos[2] = local.z;
        vertex.normal[0] = normal.x; vertex.normal[1] = normal.y; vertex.normal[2] = normal.z;
        vertex.uv[0] = terrainPosition.x / std::max(0.01f, textureScale);
        vertex.uv[1] = terrainPosition.z / std::max(0.01f, textureScale);
        vertex.tangent[0] = tangent.x; vertex.tangent[1] = tangent.y;
        vertex.tangent[2] = tangent.z; vertex.tangent[3] = 1.f;
        const float grass = std::clamp((normal.y - 0.35f) / 0.55f, 0.f, 1.f);
        const glm::vec3 rock(0.32f, 0.29f, 0.25f);
        const glm::vec3 green(0.19f, 0.48f, 0.16f);
        const glm::vec3 color = rock * (1.f - grass) + green * grass;
        vertex.color[0] = color.r; vertex.color[1] = color.g;
        vertex.color[2] = color.b; vertex.color[3] = 1.f;
        return vertex;
    };

    const auto emitPolygon = [&](std::vector<glm::vec3> polygon)
    {
        if (polygon.size() < 3)
            return;
        glm::vec3 normal(0.f);
        for (const glm::vec3& point : polygon)
        {
            normal += gradient(point);
        }
        normal = glm::length(normal) > 0.0001f
            ? glm::normalize(normal) : glm::vec3(0.f, 1.f, 0.f);
        for (size_t index = 1; index + 1 < polygon.size(); ++index)
        {
            glm::vec3 first = polygon[0];
            glm::vec3 second = polygon[index];
            glm::vec3 third = polygon[index + 1];
            if (glm::dot(glm::cross(second - first, third - first), normal) < 0.f)
                std::swap(second, third);
            vertices.push_back(makeVertex(first));
            vertices.push_back(makeVertex(second));
            vertices.push_back(makeVertex(third));
        }
    };

    for (int z = startZ; z < endZ; ++z)
    {
        for (int x = startX; x < endX; ++x)
        {
            for (int y = 0; y < yCells; ++y)
            {
                std::array<glm::vec3, 8> positions{};
                std::array<float, 8> densities{};
                for (int corner = 0; corner < 8; ++corner)
                {
                    positions[corner] = chunkOrigin + glm::vec3(
                        static_cast<float>(x + cubeCorners[corner][0]) * horizontalStep,
                        minimumY + static_cast<float>(y + cubeCorners[corner][1]) * verticalStep,
                        static_cast<float>(z + cubeCorners[corner][2]) * horizontalStep);
                    densities[corner] = Density(positions[corner], noise);
                }

                std::array<glm::vec3, 12> intersections{};
                std::array<bool, 12> activeEdges{};
                for (int edge = 0; edge < 12; ++edge)
                {
                    const int first = cubeEdges[edge][0];
                    const int second = cubeEdges[edge][1];
                    const float firstDensity = densities[first];
                    const float secondDensity = densities[second];
                    if ((firstDensity < isoLevel) == (secondDensity < isoLevel))
                        continue;
                    const float denominator = secondDensity - firstDensity;
                    const float amount = std::abs(denominator) > 0.000001f
                        ? std::clamp((isoLevel - firstDensity) / denominator, 0.f, 1.f)
                        : 0.5f;
                    activeEdges[edge] = true;
                    intersections[edge] = positions[first] +
                        (positions[second] - positions[first]) * amount;
                }

                std::array<std::array<int, 2>, 12> neighbors{};
                std::array<int, 12> neighborCounts{};
                for (auto& edgeNeighbors : neighbors)
                    edgeNeighbors = { -1, -1 };
                const auto connect = [&](int first, int second)
                {
                    if (first == second)
                        return;
                    if (neighborCounts[first] < 2)
                        neighbors[first][neighborCounts[first]++] = second;
                    if (neighborCounts[second] < 2)
                        neighbors[second][neighborCounts[second]++] = first;
                };

                for (int face = 0; face < 6; ++face)
                {
                    std::array<int, 4> crossed{};
                    int crossingCount = 0;
                    for (int index = 0; index < 4; ++index)
                    {
                        const int edge = faceEdges[face][index];
                        if (activeEdges[edge])
                            crossed[crossingCount++] = edge;
                    }
                    if (crossingCount == 2)
                    {
                        connect(crossed[0], crossed[1]);
                    }
                    else if (crossingCount == 4)
                    {
                        // Resolve a checkerboard face by sampling its bilinear
                        // center. This prevents neighboring chunks from making
                        // different choices on the shared face.
                        float centerDensity = 0.f;
                        for (int index = 0; index < 4; ++index)
                            centerDensity += densities[cubeFaces[face][index]];
                        centerDensity *= 0.25f;
                        const bool centerMatchesFirst =
                            (centerDensity < isoLevel) ==
                            (densities[cubeFaces[face][0]] < isoLevel);
                        if (centerMatchesFirst)
                        {
                            connect(faceEdges[face][0], faceEdges[face][1]);
                            connect(faceEdges[face][2], faceEdges[face][3]);
                        }
                        else
                        {
                            connect(faceEdges[face][3], faceEdges[face][0]);
                            connect(faceEdges[face][1], faceEdges[face][2]);
                        }
                    }
                }

                std::array<bool, 12> visited{};
                for (int start = 0; start < 12; ++start)
                {
                    if (!activeEdges[start] || visited[start] ||
                        neighborCounts[start] == 0)
                        continue;
                    std::vector<glm::vec3> polygon;
                    polygon.reserve(12);
                    int previous = -1;
                    int current = start;
                    for (int step = 0; step < 12; ++step)
                    {
                        polygon.push_back(intersections[current]);
                        visited[current] = true;
                        const int next = neighbors[current][0] != previous
                            ? neighbors[current][0] : neighbors[current][1];
                        if (next < 0 || next == start)
                            break;
                        if (visited[next])
                            break;
                        previous = current;
                        current = next;
                    }
                    emitPolygon(std::move(polygon));
                }
            }
        }
    }
    return vertices;
}

void MarchingCubesTerrain::BuildChunk(int chunkX, int chunkZ)
{
    if (!Owner || !Owner->GetScene())
        return;
    PerlinNoiseField* noise = ResolveNoise();
    if (!noise)
        return;

    Scene& scene = *Owner->GetScene();
    const int64_t key = ChunkKey(chunkX, chunkZ);
    Object* chunkObject = nullptr;
    const auto existingChunk = m_chunks.find(key);
    if (existingChunk != m_chunks.end())
        chunkObject = existingChunk->second;
    if (!chunkObject)
    {
        char name[64]{};
        std::snprintf(name, sizeof(name), "Chunk (%d, %d)", chunkX, chunkZ);
        chunkObject = AddChild(scene, *Owner, name);
        chunkObject->transform.position = {
            static_cast<float>(chunkX) * std::max(1.f, chunkSize), 0.f,
            static_cast<float>(chunkZ) * std::max(1.f, chunkSize) };
        m_chunks[key] = chunkObject;
    }

    MarchingCubesChunk* chunk = EnsureComponent<MarchingCubesChunk>(*chunkObject);
    chunk->chunkX = chunkX;
    chunk->chunkZ = chunkZ;
    const int quadAxisCount = std::clamp(quadsPerAxis, 1,
        std::min(8, std::clamp(horizontalCellsPerChunk, 2, 48)));
    chunk->quadCount = quadAxisCount * quadAxisCount;

    for (int quadZ = 0; quadZ < quadAxisCount; ++quadZ)
    {
        for (int quadX = 0; quadX < quadAxisCount; ++quadX)
        {
            Object* quadObject = nullptr;
            for (Object* child : chunkObject->Children)
            {
                MarchingCubesQuad* quad = child
                    ? child->GetComponent<MarchingCubesQuad>() : nullptr;
                if (quad && quad->quadX == quadX && quad->quadZ == quadZ)
                {
                    quadObject = child;
                    break;
                }
            }
            if (!quadObject)
            {
                char name[64]{};
                std::snprintf(name, sizeof(name), "Quad (%d, %d)", quadX, quadZ);
                quadObject = AddChild(scene, *chunkObject, name);
            }

            const int horizontalCells = std::clamp(horizontalCellsPerChunk, 2, 48);
            const int startX = quadX * horizontalCells / quadAxisCount;
            const int startZ = quadZ * horizontalCells / quadAxisCount;
            const float cellSize = std::max(1.f, chunkSize) /
                static_cast<float>(horizontalCells);
            quadObject->transform.position = {
                static_cast<float>(startX) * cellSize, 0.f,
                static_cast<float>(startZ) * cellSize };

            MarchingCubesQuad* quad = EnsureComponent<MarchingCubesQuad>(*quadObject);
            quad->quadX = quadX;
            quad->quadZ = quadZ;
            std::vector<Vertex> vertices = BuildQuadVertices(
                chunkX, chunkZ, quadX, quadZ, *noise);
            quad->triangleCount = static_cast<int>(vertices.size() / 3u);

            auto* mesh = EnsureComponent<Engine::Components::Mesh>(*quadObject);
            mesh->SetDeformedVertices(vertices);
            if (scene.GetGraphicsProvider())
                mesh->OnAfterDeserialize(scene.GetGraphicsProvider());

            auto* material = EnsureComponent<Engine::Components::Material>(*quadObject);
            material->diffuseColor = { 0.82f, 0.9f, 0.78f };
            material->ambientColor = { 0.08f, 0.11f, 0.07f };
            material->specularColor = { 0.18f, 0.2f, 0.16f };
            material->roughnessFactor = 0.88f;
            material->metallicFactor = 0.f;

            if (generateColliders && !vertices.empty())
            {
                auto* collider = EnsureComponent<Engine::Components::MeshObjectCollider>(
                    *quadObject);
                collider->convex = false;
                auto* body = EnsureComponent<Engine::Components::RigidBody>(*quadObject);
                body->bodyType = "Static";
                body->useGravity = false;
            }
        }
    }
}

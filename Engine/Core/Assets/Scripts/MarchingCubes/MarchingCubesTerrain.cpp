#include "Scripts/MarchingCubes/MarchingCubesTerrain.h"

#include "Scripts/MarchingCubes/MarchingCubesChunk.h"
#include "Scripts/MarchingCubes/MarchingCubesQuad.h"
#include "Scripts/MarchingCubes/PerlinNoiseField.h"
#include "Core/Compoonents/Materials/Material.h"
#include "Core/Compoonents/Obj/Mesh.h"
#include "Core/Compoonents/Physics/Collider.h"
#include "Core/Compoonents/Physics/RigidBody.h"
#include "Core/Object.h"
#include "Core/Scene/Scene.h"
#include "Core/Serialization/SceneSerializer.h"
#include "Engine/Editor/UI/IEditorUi.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>

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
    RegisterField("parallelChunkBuilds", parallelChunkBuilds);
    RegisterField("maxChunkCommitsPerUpdate", maxChunkCommitsPerUpdate);
    RegisterField("cacheUnloadedChunkMeshes", cacheUnloadedChunkMeshes);
    RegisterField("unloadedMeshCacheCapacity", unloadedMeshCacheCapacity);
    RegisterField("horizontalCellsPerChunk", horizontalCellsPerChunk);
    RegisterField("verticalCells", verticalCells);
    RegisterField("quadsPerAxis", quadsPerAxis);
    RegisterField("geometryMode", geometryMode);
    RegisterField("orthogonalHeightStep", orthogonalHeightStep);
    RegisterField("verticalSize", verticalSize);
    RegisterField("baseHeight", baseHeight);
    RegisterField("heightAmplitude", heightAmplitude);
    RegisterField("caveStrength", caveStrength);
    RegisterField("caveFrequencyMultiplier", caveFrequencyMultiplier);
    RegisterField("isoLevel", isoLevel);
    RegisterField("textureScale", textureScale);
    RegisterField("lowHeightMaximum", lowHeightMaximum);
    RegisterField("middleHeightMaximum", middleHeightMaximum);
    RegisterField("lowHeightColor", lowHeightColor);
    RegisterField("middleHeightColor", middleHeightColor);
    RegisterField("highHeightColor", highHeightColor);
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
    m_meshCache.clear();
    m_chunkQueue.clear();
    m_inFlightChunks.clear();
}

void MarchingCubesTerrain::RefreshChunks()
{
    if (!Owner || !Owner->GetScene() || !m_hasViewerChunk)
        return;
    const auto refreshStart = std::chrono::steady_clock::now();
    const int radius = std::clamp(viewRadiusInChunks, 0, 8);
    if (const PerlinNoiseField* noise = ResolveNoise())
    {
        const uint64_t configuration = MeshConfigurationHash(*noise);
        if (m_meshConfigurationHash != 0u &&
            m_meshConfigurationHash != configuration)
            m_meshCache.clear();
        m_meshConfigurationHash = configuration;
    }

    for (auto iterator = m_chunks.begin(); iterator != m_chunks.end();)
    {
        MarchingCubesChunk* chunk = iterator->second
            ? iterator->second->GetComponent<MarchingCubesChunk>() : nullptr;
        if (!chunk || std::abs(chunk->chunkX - m_viewerChunk.x) > radius ||
            std::abs(chunk->chunkZ - m_viewerChunk.y) > radius)
        {
            if (iterator->second)
            {
                CacheChunkMesh(iterator->first, *iterator->second);
                Owner->GetScene()->RequestRemoveObject(iterator->second);
            }
            iterator = m_chunks.erase(iterator);
            ++m_totalChunksUnloaded;
        }
        else
            ++iterator;
    }

    int commitBudget = std::clamp(maxChunkCommitsPerUpdate, 1, 16);
    commitBudget -= CommitCompletedChunks(commitBudget);

    m_chunkQueue.clear();
    for (int z = m_viewerChunk.y - radius; z <= m_viewerChunk.y + radius; ++z)
    {
        for (int x = m_viewerChunk.x - radius; x <= m_viewerChunk.x + radius; ++x)
        {
            const int64_t key = ChunkKey(x, z);
            if (m_chunks.find(key) == m_chunks.end() &&
                m_inFlightChunks.find(key) == m_inFlightChunks.end())
            {
                const int dx = x - m_viewerChunk.x;
                const int dz = z - m_viewerChunk.y;
                m_chunkQueue.push_back({ x, z, dx * dx + dz * dz });
            }
        }
    }
    std::sort(m_chunkQueue.begin(), m_chunkQueue.end(),
        [](const QueuedChunk& first, const QueuedChunk& second)
        {
            if (first.distanceSquared != second.distanceSquared)
                return first.distanceSquared < second.distanceSquared;
            if (first.z != second.z)
                return first.z < second.z;
            return first.x < second.x;
        });

    // Cached meshes need no worker. Restore the closest cached chunks first,
    // but retain a commit budget because scene objects and GPU buffers are
    // deliberately created only on the game thread.
    for (auto iterator = m_chunkQueue.begin();
        iterator != m_chunkQueue.end() && commitBudget > 0;)
    {
        if (m_meshCache.find(ChunkKey(iterator->x, iterator->z)) ==
            m_meshCache.end())
        {
            ++iterator;
            continue;
        }
        BuildChunk(iterator->x, iterator->z);
        iterator = m_chunkQueue.erase(iterator);
        --commitBudget;
    }

    const PerlinNoiseField* noise = ResolveNoise();
    if (noise)
    {
        const GenerationSnapshot snapshot = CaptureGenerationSnapshot(*noise);
        int launchBudget = std::clamp(maxChunkBuildsPerUpdate, 1, 16);
        const unsigned hardwareThreads = std::thread::hardware_concurrency();
        const size_t workerLimit = hardwareThreads > 1u
            ? static_cast<size_t>(hardwareThreads - 1u) : 1u;
        const size_t concurrency = std::min(workerLimit, static_cast<size_t>(
            std::clamp(parallelChunkBuilds, 1, 16)));
        while (!m_chunkQueue.empty() && launchBudget > 0 &&
            m_inFlightChunks.size() < concurrency)
        {
            const auto candidateIterator = std::find_if(m_chunkQueue.begin(),
                m_chunkQueue.end(), [this](const QueuedChunk& candidate)
                {
                    return m_meshCache.find(ChunkKey(candidate.x, candidate.z)) ==
                        m_meshCache.end();
                });
            if (candidateIterator == m_chunkQueue.end())
                break;
            const QueuedChunk candidate = *candidateIterator;
            m_chunkQueue.erase(candidateIterator);
            --launchBudget;
            const int64_t key = ChunkKey(candidate.x, candidate.z);
            InFlightChunk task;
            task.x = candidate.x;
            task.z = candidate.z;
            task.future = std::async(std::launch::async,
                [snapshot, x = candidate.x, z = candidate.z]()
                {
                    return GenerateChunkMesh(snapshot, x, z);
                });
            m_inFlightChunks.emplace(key, std::move(task));
            ++m_meshCacheMisses;
        }
    }
    m_lastStreamingMilliseconds = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - refreshStart).count();
    m_maximumStreamingMilliseconds = std::max(m_maximumStreamingMilliseconds,
        m_lastStreamingMilliseconds);
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
    return static_cast<GeometryMode>(geometryMode) == GeometryMode::OrthogonalQuads
        ? BuildOrthogonalQuadVertices(chunkX, chunkZ, quadX, quadZ, noise)
        : BuildSmoothQuadVertices(chunkX, chunkZ, quadX, quadZ, noise);
}

std::vector<MarchingCubesTerrain::Vertex>
MarchingCubesTerrain::BuildSmoothQuadVertices(int chunkX, int chunkZ,
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
        const glm::vec3 color = ColorForHeight(terrainPosition.y);
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

MarchingCubesTerrain::GenerationSnapshot
MarchingCubesTerrain::CaptureGenerationSnapshot(
    const PerlinNoiseField& noise) const
{
    GenerationSnapshot snapshot;
    snapshot.chunkSize = chunkSize;
    snapshot.horizontalCells = horizontalCellsPerChunk;
    snapshot.verticalCells = verticalCells;
    snapshot.quadsPerAxis = quadsPerAxis;
    snapshot.geometryMode = geometryMode;
    snapshot.orthogonalHeightStep = orthogonalHeightStep;
    snapshot.verticalSize = verticalSize;
    snapshot.baseHeight = baseHeight;
    snapshot.heightAmplitude = heightAmplitude;
    snapshot.caveStrength = caveStrength;
    snapshot.caveFrequencyMultiplier = caveFrequencyMultiplier;
    snapshot.isoLevel = isoLevel;
    snapshot.textureScale = textureScale;
    snapshot.lowHeightMaximum = lowHeightMaximum;
    snapshot.middleHeightMaximum = middleHeightMaximum;
    snapshot.lowHeightColor = lowHeightColor;
    snapshot.middleHeightColor = middleHeightColor;
    snapshot.highHeightColor = highHeightColor;
    snapshot.noiseSeed = noise.seed;
    snapshot.noiseFrequency = noise.frequency;
    snapshot.noiseOctaves = noise.octaves;
    snapshot.noiseLacunarity = noise.lacunarity;
    snapshot.noisePersistence = noise.persistence;
    snapshot.noiseOffset = noise.coordinateOffset;
    snapshot.configurationHash = MeshConfigurationHash(noise);
    return snapshot;
}

MarchingCubesTerrain::GeneratedChunkMesh
MarchingCubesTerrain::GenerateChunkMesh(const GenerationSnapshot& snapshot,
    int chunkX, int chunkZ)
{
    MarchingCubesTerrain generator;
    generator.chunkSize = snapshot.chunkSize;
    generator.horizontalCellsPerChunk = snapshot.horizontalCells;
    generator.verticalCells = snapshot.verticalCells;
    generator.quadsPerAxis = snapshot.quadsPerAxis;
    generator.geometryMode = snapshot.geometryMode;
    generator.orthogonalHeightStep = snapshot.orthogonalHeightStep;
    generator.verticalSize = snapshot.verticalSize;
    generator.baseHeight = snapshot.baseHeight;
    generator.heightAmplitude = snapshot.heightAmplitude;
    generator.caveStrength = snapshot.caveStrength;
    generator.caveFrequencyMultiplier = snapshot.caveFrequencyMultiplier;
    generator.isoLevel = snapshot.isoLevel;
    generator.textureScale = snapshot.textureScale;
    generator.lowHeightMaximum = snapshot.lowHeightMaximum;
    generator.middleHeightMaximum = snapshot.middleHeightMaximum;
    generator.lowHeightColor = snapshot.lowHeightColor;
    generator.middleHeightColor = snapshot.middleHeightColor;
    generator.highHeightColor = snapshot.highHeightColor;

    PerlinNoiseField noise;
    noise.seed = snapshot.noiseSeed;
    noise.frequency = snapshot.noiseFrequency;
    noise.octaves = snapshot.noiseOctaves;
    noise.lacunarity = snapshot.noiseLacunarity;
    noise.persistence = snapshot.noisePersistence;
    noise.coordinateOffset = snapshot.noiseOffset;

    GeneratedChunkMesh result;
    result.x = chunkX;
    result.z = chunkZ;
    result.configurationHash = snapshot.configurationHash;
    const int quadCount = std::clamp(snapshot.quadsPerAxis, 1,
        std::min(8, std::clamp(snapshot.horizontalCells, 2, 48)));
    result.quads.reserve(static_cast<size_t>(quadCount * quadCount));
    for (int quadZ = 0; quadZ < quadCount; ++quadZ)
    {
        for (int quadX = 0; quadX < quadCount; ++quadX)
        {
            result.quads.push_back({ quadX, quadZ,
                generator.BuildQuadVertices(chunkX, chunkZ, quadX, quadZ,
                    noise) });
        }
    }
    return result;
}

bool MarchingCubesTerrain::IsChunkDesired(int x, int z) const
{
    const int radius = std::clamp(viewRadiusInChunks, 0, 8);
    return std::abs(x - m_viewerChunk.x) <= radius &&
        std::abs(z - m_viewerChunk.y) <= radius;
}

int MarchingCubesTerrain::CommitCompletedChunks(int budget)
{
    struct ReadyTask { int64_t key = 0; int distanceSquared = 0; };
    std::vector<ReadyTask> ready;
    for (auto& [key, task] : m_inFlightChunks)
    {
        if (task.future.valid() && task.future.wait_for(
            std::chrono::milliseconds(0)) == std::future_status::ready)
        {
            const int dx = task.x - m_viewerChunk.x;
            const int dz = task.z - m_viewerChunk.y;
            ready.push_back({ key, dx * dx + dz * dz });
        }
    }
    std::sort(ready.begin(), ready.end(),
        [](const ReadyTask& first, const ReadyTask& second)
        {
            return first.distanceSquared < second.distanceSquared;
        });

    int committed = 0;
    for (const ReadyTask& readyTask : ready)
    {
        auto iterator = m_inFlightChunks.find(readyTask.key);
        if (iterator == m_inFlightChunks.end())
            continue;
        const bool desired = IsChunkDesired(iterator->second.x,
            iterator->second.z);
        if (desired && committed >= budget)
            continue;
        GeneratedChunkMesh result = iterator->second.future.get();
        m_inFlightChunks.erase(iterator);
        if (!desired || result.configurationHash != m_meshConfigurationHash ||
            m_chunks.find(readyTask.key) != m_chunks.end())
            continue;
        BuildChunk(result.x, result.z, &result.quads);
        ++committed;
    }
    return committed;
}

uint64_t MarchingCubesTerrain::MeshConfigurationHash(
    const PerlinNoiseField& noise) const
{
    uint64_t hash = 1469598103934665603ull;
    const auto addBytes = [&](const void* data, size_t byteCount)
    {
        const auto* bytes = static_cast<const unsigned char*>(data);
        for (size_t index = 0; index < byteCount; ++index)
        {
            hash ^= bytes[index];
            hash *= 1099511628211ull;
        }
    };
    const auto add = [&](const auto& value) { addBytes(&value, sizeof(value)); };
    add(chunkSize); add(horizontalCellsPerChunk); add(verticalCells);
    add(quadsPerAxis); add(geometryMode); add(orthogonalHeightStep);
    add(verticalSize); add(baseHeight); add(heightAmplitude); add(caveStrength);
    add(caveFrequencyMultiplier); add(isoLevel); add(textureScale);
    add(lowHeightMaximum); add(middleHeightMaximum);
    add(lowHeightColor); add(middleHeightColor); add(highHeightColor);
    add(noise.seed); add(noise.frequency); add(noise.octaves);
    add(noise.lacunarity); add(noise.persistence); add(noise.coordinateOffset);
    return hash;
}

void MarchingCubesTerrain::CacheChunkMesh(int64_t key,
    const Engine::Core::Object& chunkObject)
{
    if (!cacheUnloadedChunkMeshes || unloadedMeshCacheCapacity <= 0)
        return;
    CachedChunkMesh cached;
    cached.lastUse = ++m_meshCacheClock;
    cached.quads.reserve(chunkObject.Children.size());
    for (const Object* child : chunkObject.Children)
    {
        const MarchingCubesQuad* quad = child
            ? child->GetComponent<MarchingCubesQuad>() : nullptr;
        const auto* mesh = child
            ? child->GetComponent<Engine::Components::Mesh>() : nullptr;
        if (!quad || !mesh || mesh->GetVertices().empty())
            continue;
        cached.quads.push_back({ quad->quadX, quad->quadZ,
            mesh->GetVertices() });
    }
    if (!cached.quads.empty())
    {
        m_meshCache[key] = std::move(cached);
        TrimMeshCache();
    }
}

void MarchingCubesTerrain::TrimMeshCache()
{
    const size_t capacity = static_cast<size_t>(
        std::clamp(unloadedMeshCacheCapacity, 0, 1024));
    while (m_meshCache.size() > capacity)
    {
        auto oldest = m_meshCache.end();
        for (auto iterator = m_meshCache.begin(); iterator != m_meshCache.end();
            ++iterator)
        {
            if (oldest == m_meshCache.end() ||
                iterator->second.lastUse < oldest->second.lastUse)
                oldest = iterator;
        }
        if (oldest == m_meshCache.end())
            break;
        m_meshCache.erase(oldest);
    }
}

float MarchingCubesTerrain::OrthogonalHeight(float worldX, float worldZ,
    const PerlinNoiseField& noise) const
{
    const float step = std::max(0.05f, orthogonalHeightStep);
    const float rawHeight = baseHeight + heightAmplitude *
        noise.SampleFractal2D(worldX, worldZ);
    const float halfHeight = std::max(1.f, verticalSize) * 0.5f;
    return std::clamp(std::round(rawHeight / step) * step,
        -halfHeight, halfHeight);
}

glm::vec3 MarchingCubesTerrain::ColorForHeight(float height) const
{
    const float lower = std::min(lowHeightMaximum, middleHeightMaximum);
    const float upper = std::max(lowHeightMaximum, middleHeightMaximum);
    if (height <= lower)
        return glm::clamp(lowHeightColor, glm::vec3(0.f), glm::vec3(1.f));
    if (height <= upper)
        return glm::clamp(middleHeightColor, glm::vec3(0.f), glm::vec3(1.f));
    return glm::clamp(highHeightColor, glm::vec3(0.f), glm::vec3(1.f));
}

std::vector<MarchingCubesTerrain::Vertex>
MarchingCubesTerrain::BuildOrthogonalQuadVertices(int chunkX, int chunkZ,
    int quadX, int quadZ, const PerlinNoiseField& noise) const
{
    const int horizontalCells = std::clamp(horizontalCellsPerChunk, 2, 48);
    const int quadAxisCount = std::clamp(quadsPerAxis, 1,
        std::min(8, horizontalCells));
    const int startX = quadX * horizontalCells / quadAxisCount;
    const int endX = (quadX + 1) * horizontalCells / quadAxisCount;
    const int startZ = quadZ * horizontalCells / quadAxisCount;
    const int endZ = (quadZ + 1) * horizontalCells / quadAxisCount;
    const float cellSize = std::max(1.f, chunkSize) /
        static_cast<float>(horizontalCells);
    const glm::vec3 chunkOrigin(
        static_cast<float>(chunkX) * std::max(1.f, chunkSize), 0.f,
        static_cast<float>(chunkZ) * std::max(1.f, chunkSize));
    const glm::vec3 quadOrigin(
        static_cast<float>(startX) * cellSize, 0.f,
        static_cast<float>(startZ) * cellSize);

    std::vector<Vertex> vertices;
    vertices.reserve(static_cast<size_t>(endX - startX) *
        static_cast<size_t>(endZ - startZ) * 30u);

    const auto makeVertex = [&](const glm::vec3& terrainPosition,
        const glm::vec3& normal)
    {
        Vertex vertex{};
        const glm::vec3 local = terrainPosition - chunkOrigin - quadOrigin;
        glm::vec3 tangent = std::abs(normal.x) < 0.9f
            ? glm::vec3(1.f, 0.f, 0.f) : glm::vec3(0.f, 0.f, 1.f);
        tangent -= normal * glm::dot(tangent, normal);
        tangent = glm::normalize(tangent);
        vertex.pos[0] = local.x; vertex.pos[1] = local.y; vertex.pos[2] = local.z;
        vertex.normal[0] = normal.x; vertex.normal[1] = normal.y;
        vertex.normal[2] = normal.z;
        vertex.uv[0] = terrainPosition.x / std::max(0.01f, textureScale);
        vertex.uv[1] = terrainPosition.z / std::max(0.01f, textureScale);
        vertex.tangent[0] = tangent.x; vertex.tangent[1] = tangent.y;
        vertex.tangent[2] = tangent.z; vertex.tangent[3] = 1.f;
        const glm::vec3 color = ColorForHeight(terrainPosition.y);
        vertex.color[0] = color.r; vertex.color[1] = color.g;
        vertex.color[2] = color.b; vertex.color[3] = 1.f;
        return vertex;
    };
    const auto emitQuad = [&](glm::vec3 first, glm::vec3 second,
        glm::vec3 third, glm::vec3 fourth, const glm::vec3& normal)
    {
        if (glm::dot(glm::cross(second - first, third - first), normal) < 0.f)
            std::swap(second, fourth);
        vertices.push_back(makeVertex(first, normal));
        vertices.push_back(makeVertex(second, normal));
        vertices.push_back(makeVertex(third, normal));
        vertices.push_back(makeVertex(first, normal));
        vertices.push_back(makeVertex(third, normal));
        vertices.push_back(makeVertex(fourth, normal));
    };
    // Include a one-cell halo so every shared top/wall decision is sampled
    // exactly once for this mesh batch. The previous per-face calls evaluated
    // the same four-octave noise value as many as five times per terrain cell.
    const int sampledWidth = endX - startX + 2;
    const int sampledDepth = endZ - startZ + 2;
    std::vector<float> sampledHeights(static_cast<size_t>(sampledWidth) *
        static_cast<size_t>(sampledDepth));
    for (int sampleZ = 0; sampleZ < sampledDepth; ++sampleZ)
    {
        for (int sampleX = 0; sampleX < sampledWidth; ++sampleX)
        {
            const int cellX = startX + sampleX - 1;
            const int cellZ = startZ + sampleZ - 1;
            sampledHeights[static_cast<size_t>(sampleZ) * sampledWidth +
                sampleX] = OrthogonalHeight(
                    chunkOrigin.x + (static_cast<float>(cellX) + 0.5f) * cellSize,
                    chunkOrigin.z + (static_cast<float>(cellZ) + 0.5f) * cellSize,
                    noise);
        }
    }
    const auto heightAtCell = [&](int cellX, int cellZ)
    {
        const int sampleX = cellX - startX + 1;
        const int sampleZ = cellZ - startZ + 1;
        return sampledHeights[static_cast<size_t>(sampleZ) * sampledWidth +
            sampleX];
    };

    static constexpr int directions[4][2] = {
        { -1, 0 }, { 1, 0 }, { 0, -1 }, { 0, 1 }
    };
    static constexpr glm::vec3 normals[4] = {
        { -1.f, 0.f, 0.f }, { 1.f, 0.f, 0.f },
        { 0.f, 0.f, -1.f }, { 0.f, 0.f, 1.f }
    };

    for (int z = startZ; z < endZ; ++z)
    {
        for (int x = startX; x < endX; ++x)
        {
            const float x0 = chunkOrigin.x + static_cast<float>(x) * cellSize;
            const float x1 = x0 + cellSize;
            const float z0 = chunkOrigin.z + static_cast<float>(z) * cellSize;
            const float z1 = z0 + cellSize;
            const float height = heightAtCell(x, z);
            emitQuad({ x0, height, z0 }, { x0, height, z1 },
                { x1, height, z1 }, { x1, height, z0 }, { 0.f, 1.f, 0.f });

            for (int direction = 0; direction < 4; ++direction)
            {
                const float neighborHeight = heightAtCell(
                    x + directions[direction][0],
                    z + directions[direction][1]);
                if (height <= neighborHeight + 0.0001f)
                    continue;
                if (direction == 0)
                    emitQuad({ x0, neighborHeight, z0 }, { x0, neighborHeight, z1 },
                        { x0, height, z1 }, { x0, height, z0 }, normals[direction]);
                else if (direction == 1)
                    emitQuad({ x1, neighborHeight, z1 }, { x1, neighborHeight, z0 },
                        { x1, height, z0 }, { x1, height, z1 }, normals[direction]);
                else if (direction == 2)
                    emitQuad({ x1, neighborHeight, z0 }, { x0, neighborHeight, z0 },
                        { x0, height, z0 }, { x1, height, z0 }, normals[direction]);
                else
                    emitQuad({ x0, neighborHeight, z1 }, { x1, neighborHeight, z1 },
                        { x1, height, z1 }, { x0, height, z1 }, normals[direction]);
            }
        }
    }
    return vertices;
}

void MarchingCubesTerrain::BuildChunk(int chunkX, int chunkZ,
    const std::vector<CachedQuadMesh>* preparedQuads)
{
    if (!Owner || !Owner->GetScene())
        return;
    PerlinNoiseField* noise = ResolveNoise();
    if (!noise)
        return;

    const uint64_t configuration = MeshConfigurationHash(*noise);
    if (m_meshConfigurationHash != 0u &&
        m_meshConfigurationHash != configuration)
        m_meshCache.clear();
    m_meshConfigurationHash = configuration;

    Scene& scene = *Owner->GetScene();
    const int64_t key = ChunkKey(chunkX, chunkZ);
    auto cachedChunk = m_meshCache.find(key);
    const bool usesCachedMesh = !preparedQuads && cacheUnloadedChunkMeshes &&
        cachedChunk != m_meshCache.end();
    if (usesCachedMesh)
        ++m_meshCacheHits;
    else if (!preparedQuads)
        ++m_meshCacheMisses;
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
            std::vector<Vertex> vertices;
            const std::vector<CachedQuadMesh>* sourceQuads = preparedQuads;
            if (!sourceQuads && usesCachedMesh)
                sourceQuads = &cachedChunk->second.quads;
            if (sourceQuads)
            {
                const auto cachedQuad = std::find_if(
                    sourceQuads->begin(), sourceQuads->end(),
                    [&](const CachedQuadMesh& value)
                    {
                        return value.quadX == quadX && value.quadZ == quadZ;
                    });
                if (cachedQuad != sourceQuads->end())
                    vertices = cachedQuad->vertices;
            }
            if (vertices.empty())
                vertices = BuildQuadVertices(chunkX, chunkZ, quadX, quadZ, *noise);
            quad->triangleCount = static_cast<int>(vertices.size() / 3u);

            auto* mesh = EnsureComponent<Engine::Components::Mesh>(*quadObject);
            mesh->SetDeformedVertices(vertices);
            if (scene.GetGraphicsProvider())
                mesh->OnAfterDeserialize(scene.GetGraphicsProvider());

            auto* material = EnsureComponent<Engine::Components::Material>(*quadObject);
            // Vertex colors carry the selected height bands. Keep the material
            // neutral so the picker values reach the shader unchanged.
            material->diffuseColor = { 1.f, 1.f, 1.f };
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
    if (usesCachedMesh)
        m_meshCache.erase(cachedChunk);
    ++m_totalChunksBuilt;
}

bool MarchingCubesTerrain::GenerateTerrain()
{
    if (!Owner || !Owner->GetScene() || !ResolveNoise())
    {
        m_editorGenerationStatus = "Generation failed: terrain requires a scene and noise field.";
        return false;
    }

    Scene& scene = *Owner->GetScene();
    m_chunkQueue.clear();
    m_inFlightChunks.clear();
    const std::vector<Object*> existingChildren = Owner->Children;
    for (Object* child : existingChildren)
    {
        if (child && child->GetComponent<MarchingCubesChunk>())
            scene.RemoveObject(child);
    }
    m_chunks.clear();
    m_meshCache.clear();
    m_meshConfigurationHash = 0u;
    m_meshCacheClock = 0u;
    m_viewerChunk = ViewerChunk();
    m_hasViewerChunk = true;
    m_totalChunksBuilt = 0;
    m_totalChunksUnloaded = 0;
    m_meshCacheHits = 0;
    m_meshCacheMisses = 0;
    m_lastStreamingMilliseconds = 0.0;
    m_maximumStreamingMilliseconds = 0.0;

    const auto generationStart = std::chrono::steady_clock::now();
    const int radius = std::clamp(viewRadiusInChunks, 0, 8);
    for (int z = m_viewerChunk.y - radius; z <= m_viewerChunk.y + radius; ++z)
    {
        for (int x = m_viewerChunk.x - radius; x <= m_viewerChunk.x + radius; ++x)
            BuildChunk(x, z);
    }
    const double milliseconds = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - generationStart).count();
    char status[160]{};
    std::snprintf(status, sizeof(status),
        "Generated %zu chunks in %.2f ms without starting the game.",
        m_chunks.size(), milliseconds);
    m_editorGenerationStatus = status;
    return true;
}

bool MarchingCubesTerrain::DrawProperties(::Engine::Editor::IEditorUi& ui)
{
    bool changed = false;
    char viewerName[256]{};
    char noiseName[256]{};
    std::snprintf(viewerName, sizeof(viewerName), "%s", viewerObjectName.c_str());
    std::snprintf(noiseName, sizeof(noiseName), "%s", noiseObjectName.c_str());
    if (ui.InputText("Viewer Object", viewerName, sizeof(viewerName)))
    {
        viewerObjectName = viewerName;
        changed = true;
    }
    if (ui.InputText("Noise Object", noiseName, sizeof(noiseName)))
    {
        noiseObjectName = noiseName;
        changed = true;
    }

    changed = ui.DragFloat("Chunk Size", &chunkSize, 0.5f, 1.f, 1000.f) || changed;
    changed = ui.SliderInt("View Radius (Chunks)", &viewRadiusInChunks, 0, 8) || changed;
    changed = ui.SliderInt("Launches Per Update", &maxChunkBuildsPerUpdate, 1, 16) || changed;
    changed = ui.SliderInt("Parallel Chunk Builds", &parallelChunkBuilds, 1, 16) || changed;
    changed = ui.SliderInt("Chunk Commits Per Update",
        &maxChunkCommitsPerUpdate, 1, 16) || changed;
    changed = ui.Checkbox("Cache Unloaded Chunk Meshes",
        &cacheUnloadedChunkMeshes) || changed;
    if (cacheUnloadedChunkMeshes)
    {
        changed = ui.SliderInt("Unloaded Mesh Cache Capacity",
            &unloadedMeshCacheCapacity, 0, 1024) || changed;
    }
    changed = ui.SliderInt("Horizontal Cells", &horizontalCellsPerChunk, 2, 48) || changed;
    changed = ui.SliderInt("Vertical Cells", &verticalCells, 2, 48) || changed;
    changed = ui.SliderInt("Quad Batches Per Axis", &quadsPerAxis, 1, 8) || changed;
    static const char* geometryModes[] = { "Smooth Marching Cubes", "Orthogonal Quads" };
    changed = ui.Combo("Terrain Geometry", &geometryMode, geometryModes, 2) || changed;
    if (static_cast<GeometryMode>(geometryMode) == GeometryMode::OrthogonalQuads)
    {
        changed = ui.DragFloat("Orthogonal Height Step", &orthogonalHeightStep,
            0.05f, 0.05f, 100.f) || changed;
        ui.DisabledLabel("Surfaces connect with horizontal and vertical quads only.");
    }
    changed = ui.DragFloat("Vertical Size", &verticalSize, 0.5f, 1.f, 1000.f) || changed;
    changed = ui.DragFloat("Base Height", &baseHeight, 0.1f) || changed;
    changed = ui.DragFloat("Height Amplitude", &heightAmplitude, 0.1f, 0.f, 1000.f) || changed;
    if (static_cast<GeometryMode>(geometryMode) == GeometryMode::SmoothMarchingCubes)
    {
        changed = ui.DragFloat("Cave Strength", &caveStrength, 0.05f, 0.f, 100.f) || changed;
        changed = ui.DragFloat("Cave Frequency Multiplier", &caveFrequencyMultiplier,
            0.05f, 0.01f, 100.f) || changed;
        changed = ui.DragFloat("Iso Level", &isoLevel, 0.05f) || changed;
    }
    changed = ui.DragFloat("Texture Scale", &textureScale, 0.1f, 0.01f, 1000.f) || changed;
    changed = ui.DragFloat("Low Height Maximum", &lowHeightMaximum, 0.1f) || changed;
    changed = ui.ColorEdit3("Low Height Color", &lowHeightColor.x) || changed;
    changed = ui.DragFloat("Middle Height Maximum", &middleHeightMaximum, 0.1f) || changed;
    changed = ui.ColorEdit3("Middle Height Color", &middleHeightColor.x) || changed;
    changed = ui.ColorEdit3("High Height Color", &highHeightColor.x) || changed;
    changed = ui.Checkbox("Generate Colliders", &generateColliders) || changed;

    char value[96]{};
    std::snprintf(value, sizeof(value), "%zu", m_chunks.size());
    ui.ValueLabel("Loaded Chunks", value);
    std::snprintf(value, sizeof(value), "%llu built / %llu unloaded",
        static_cast<unsigned long long>(m_totalChunksBuilt),
        static_cast<unsigned long long>(m_totalChunksUnloaded));
    ui.ValueLabel("Streaming Activity", value);
    std::snprintf(value, sizeof(value), "%.3f ms last / %.3f ms maximum",
        m_lastStreamingMilliseconds, m_maximumStreamingMilliseconds);
    ui.ValueLabel("Streaming CPU Time", value);
    std::snprintf(value, sizeof(value), "%zu cached / %llu hits / %llu misses",
        m_meshCache.size(), static_cast<unsigned long long>(m_meshCacheHits),
        static_cast<unsigned long long>(m_meshCacheMisses));
    ui.ValueLabel("Mesh Cache", value);
    std::snprintf(value, sizeof(value), "%zu queued / %zu generating",
        m_chunkQueue.size(), m_inFlightChunks.size());
    ui.ValueLabel("Parallel Build Queue", value);
    ui.Separator();
    if (ui.Button("Generate Terrain Now"))
    {
        GenerateTerrain();
        changed = true;
    }
    if (!m_editorGenerationStatus.empty())
        ui.DisabledLabel(m_editorGenerationStatus.c_str());
    return changed;
}

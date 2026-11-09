#include "Scripts/TerrainGen/TerrainGen.h"
#include "Scripts/TerrainGen/TerrainChunk.h"
#include "Scripts/TerrainGen/TerrainPatch.h"
#include "Scripts/TerrainGen/PerlinNoiseField.h"
#include "Core/Compoonents/Materials/Material.h"
#include "Core/Compoonents/Mesh/Mesh.h"
#include "Core/Compoonents/Physics/PrimitiveObjectCollider.h"
#include "Core/Compoonents/Physics/MeshObjectCollider.h"
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
#include <cstdlib>
#include <cstring>
#include <limits>
#include <thread>
#include <utility>
#if defined(_WIN32)
#include <Windows.h>
#endif

float TerrainGen::Density(const glm::vec3& position,
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

std::vector<TerrainGen::Vertex>
TerrainGen::BuildPatchVertices(int chunkX, int chunkZ,
    int patchX, int patchZ, const PerlinNoiseField& noise) const
{
    switch (static_cast<TerrainShape>(terrainShape))
    {
    case TerrainShape::Cubes:
        return BuildCubeVertices(chunkX, chunkZ, patchX, patchZ, noise);
    case TerrainShape::Triangles:
        return BuildTriangleVertices(chunkX, chunkZ, patchX, patchZ, noise);
    case TerrainShape::Hexagons:
        return BuildHexagonVertices(chunkX, chunkZ, patchX, patchZ, noise);
    case TerrainShape::SmoothSurface:
    default:
        return BuildSmoothSurfaceVertices(chunkX, chunkZ, patchX, patchZ, noise);
    }
}

std::vector<TerrainGen::Vertex>
TerrainGen::BuildSmoothSurfaceVertices(int chunkX, int chunkZ,
    int patchX, int patchZ, const PerlinNoiseField& noise) const
{
    const int horizontalCells = std::clamp(horizontalCellsPerChunk, 2, 48);
    const int yCells = std::clamp(verticalCells, 2, 48);
    const int patchAxisCount = std::clamp(patchesPerAxis, 1,
        std::min(8, horizontalCells));
    const int startX = patchX * horizontalCells / patchAxisCount;
    const int endX = (patchX + 1) * horizontalCells / patchAxisCount;
    const int startZ = patchZ * horizontalCells / patchAxisCount;
    const int endZ = (patchZ + 1) * horizontalCells / patchAxisCount;
    const float terrainChunkSize = std::max(1.f, chunkSize);
    const float horizontalStep = terrainChunkSize /
        static_cast<float>(horizontalCells);
    const float ySize = std::max(1.f, verticalSize);
    const float verticalStep = ySize / static_cast<float>(yCells);
    const float minimumY = -ySize * 0.5f;
    const double logicalPatchOriginX = static_cast<double>(chunkX) *
        terrainChunkSize + static_cast<double>(startX) * terrainChunkSize /
        horizontalCells;
    const double logicalPatchOriginZ = static_cast<double>(chunkZ) *
        terrainChunkSize + static_cast<double>(startZ) * terrainChunkSize /
        horizontalCells;
    const double uvScale = static_cast<double>(std::max(0.01f, textureScale));
    const double uvBaseX = std::floor(logicalPatchOriginX / uvScale);
    const double uvBaseZ = std::floor(logicalPatchOriginZ / uvScale);
    // Use one global integer lattice for samples shared by neighboring chunks.
    // This avoids small density/position disagreements caused by reaching the
    // same border through different floating-point addition paths.
    const auto worldGridCoordinate = [horizontalCells, terrainChunkSize](
        int chunk, int cell)
    {
        const int64_t globalCell = static_cast<int64_t>(chunk) *
            horizontalCells + cell;
        return static_cast<double>(globalCell) *
            static_cast<double>(terrainChunkSize) /
            static_cast<double>(horizontalCells);
    };
    const auto localGridCoordinate = [horizontalCells, terrainChunkSize](
        int cell, int patchStart)
    {
        return static_cast<float>(static_cast<double>(cell - patchStart) *
            static_cast<double>(terrainChunkSize) /
            static_cast<double>(horizontalCells));
    };

    static constexpr int cubeCorners[8][3] = {
        { 0, 0, 0 }, { 1, 0, 0 }, { 1, 0, 1 }, { 0, 0, 1 },
        { 0, 1, 0 }, { 1, 1, 0 }, { 1, 1, 1 }, { 0, 1, 1 }
    };
    static constexpr int cubeEdges[12][2] = {
        { 0, 1 }, { 1, 2 }, { 2, 3 }, { 3, 0 },
        { 4, 5 }, { 5, 6 }, { 6, 7 }, { 7, 4 },
        { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 }
    };
    static constexpr int faceEdges[6][4] = {
        { 0, 1, 2, 3 }, { 4, 5, 6, 7 }, { 0, 9, 4, 8 },
        { 1, 10, 5, 9 }, { 2, 11, 6, 10 }, { 3, 8, 7, 11 }
    };

    std::vector<Vertex> vertices;
    vertices.reserve(static_cast<size_t>(endX - startX) *
        static_cast<size_t>(endZ - startZ) * yCells * 18u);

    // This cache belongs only to this new patch build. It avoids recomputing
    // shared cell corners and derives normals from the same density field; it
    // is unrelated to the persistent cache used when revisiting old chunks.
    const int gridWidth = endX - startX + 3;
    const int gridDepth = endZ - startZ + 3;
    const int gridHeight = yCells + 3;
    const auto gridIndex = [=](int x, int y, int z)
    {
        return (static_cast<size_t>(y) * gridDepth + z) * gridWidth + x;
    };
    std::vector<float> surfaceHeights(static_cast<size_t>(gridWidth) * gridDepth);
    for (int z = 0; z < gridDepth; ++z)
    {
        for (int x = 0; x < gridWidth; ++x)
        {
            const double worldX = worldGridCoordinate(chunkX, startX + x - 1);
            const double worldZ = worldGridCoordinate(chunkZ, startZ + z - 1);
            surfaceHeights[static_cast<size_t>(z) * gridWidth + x] =
                baseHeight + heightAmplitude * noise.SampleFractal2D(worldX, worldZ);
        }
    }
    std::vector<float> densityGrid(static_cast<size_t>(gridWidth) *
        gridDepth * gridHeight);
    for (int y = 0; y < gridHeight; ++y)
    {
        const float worldY = minimumY + static_cast<float>(y - 1) * verticalStep;
        for (int z = 0; z < gridDepth; ++z)
        {
            const double worldZ = worldGridCoordinate(chunkZ, startZ + z - 1);
            for (int x = 0; x < gridWidth; ++x)
            {
                const double worldX = worldGridCoordinate(chunkX, startX + x - 1);
                const float caves = caveStrength > 0.f
                    ? caveStrength * noise.SampleFractal(glm::dvec3(
                        worldX, static_cast<double>(worldY), worldZ) *
                        static_cast<double>(std::max(0.01f,
                            caveFrequencyMultiplier)))
                    : 0.f;
                densityGrid[gridIndex(x, y, z)] = worldY -
                    surfaceHeights[static_cast<size_t>(z) * gridWidth + x] + caves;
            }
        }
    }
    const auto gradientAt = [&](int x, int y, int z)
    {
        glm::vec3 result(
            (densityGrid[gridIndex(x + 1, y, z)] -
                densityGrid[gridIndex(x - 1, y, z)]) / (2.f * horizontalStep),
            (densityGrid[gridIndex(x, y + 1, z)] -
                densityGrid[gridIndex(x, y - 1, z)]) / (2.f * verticalStep),
            (densityGrid[gridIndex(x, y, z + 1)] -
                densityGrid[gridIndex(x, y, z - 1)]) / (2.f * horizontalStep));
        const float length = glm::length(result);
        return length > 0.000001f ? result / length : glm::vec3(0.f, 1.f, 0.f);
    };

    const auto makeVertex = [&](const glm::vec3& localPosition,
        const glm::vec3& normal)
    {
        Vertex vertex{};
        glm::vec3 tangent = glm::vec3(1.f, 0.f, 0.f) - normal * normal.x;
        if (glm::length(tangent) < 0.0001f)
            tangent = glm::vec3(0.f, 0.f, 1.f);
        tangent = glm::normalize(tangent);
        vertex.pos[0] = localPosition.x; vertex.pos[1] = localPosition.y;
        vertex.pos[2] = localPosition.z;
        vertex.normal[0] = normal.x; vertex.normal[1] = normal.y; vertex.normal[2] = normal.z;
        vertex.uv[0] = static_cast<float>((logicalPatchOriginX +
            static_cast<double>(localPosition.x)) / uvScale - uvBaseX);
        vertex.uv[1] = static_cast<float>((logicalPatchOriginZ +
            static_cast<double>(localPosition.z)) / uvScale - uvBaseZ);
        vertex.tangent[0] = tangent.x; vertex.tangent[1] = tangent.y;
        vertex.tangent[2] = tangent.z; vertex.tangent[3] = 1.f;
        // A hard band selection at marching-surface vertices exposes the
        // triangulation as long colored wedges when the terrain recedes from
        // the camera. Smooth surfaces blend around each height boundary;
        // block/column modes retain their deliberately discrete bands.
        const glm::vec3 color = SmoothColorForHeight(localPosition.y);
        vertex.color[0] = color.r; vertex.color[1] = color.g;
        vertex.color[2] = color.b; vertex.color[3] = 1.f;
        return vertex;
    };

    const auto emitPolygon = [&](const std::array<glm::vec3, 12>& polygon,
        const std::array<glm::vec3, 12>& polygonNormals, int polygonSize)
    {
        if (polygonSize < 3)
            return;
        std::array<glm::vec3, 12> compactPolygon{};
        std::array<glm::vec3, 12> compactNormals{};
        int compactSize = 0;
        const float mergeDistance =
            std::max(horizontalStep, verticalStep) * 0.00001f;
        const float mergeDistanceSquared = mergeDistance * mergeDistance;
        for (int index = 0; index < polygonSize; ++index)
        {
            bool duplicate = false;
            for (int prior = 0; prior < compactSize; ++prior)
            {
                const glm::vec3 delta =
                    polygon[index] - compactPolygon[prior];
                if (glm::dot(delta, delta) <= mergeDistanceSquared)
                {
                    duplicate = true;
                    break;
                }
            }
            if (!duplicate)
            {
                compactPolygon[compactSize] = polygon[index];
                compactNormals[compactSize] = polygonNormals[index];
                ++compactSize;
            }
        }
        if (compactSize < 3)
            return;
        glm::vec3 faceNormal(0.f);
        for (int index = 0; index < compactSize; ++index)
            faceNormal += compactNormals[index];
        faceNormal = glm::length(faceNormal) > 0.0001f
            ? glm::normalize(faceNormal) : glm::vec3(0.f, 1.f, 0.f);
        for (int index = 1; index + 1 < compactSize; ++index)
        {
            glm::vec3 first = compactPolygon[0];
            glm::vec3 second = compactPolygon[index];
            glm::vec3 third = compactPolygon[index + 1];
            glm::vec3 firstNormal = compactNormals[0];
            glm::vec3 secondNormal = compactNormals[index];
            glm::vec3 thirdNormal = compactNormals[index + 1];
            const glm::vec3 faceCross =
                glm::cross(second - first, third - first);
            if (glm::dot(faceCross, faceCross) <=
                mergeDistanceSquared * mergeDistanceSquared)
                continue;
            if (glm::dot(faceCross, faceNormal) < 0.f)
            {
                std::swap(second, third);
                std::swap(secondNormal, thirdNormal);
            }
            vertices.push_back(makeVertex(first, firstNormal));
            vertices.push_back(makeVertex(second, secondNormal));
            vertices.push_back(makeVertex(third, thirdNormal));
        }
    };

    for (int z = startZ; z < endZ; ++z)
    {
        for (int x = startX; x < endX; ++x)
        {
            for (int y = 0; y < yCells; ++y)
            {
                std::array<glm::vec3, 8> positions{};
                std::array<glm::vec3, 8> cornerGradients{};
                std::array<float, 8> densities{};
                for (int corner = 0; corner < 8; ++corner)
                {
                    positions[corner] = glm::vec3(
                        localGridCoordinate(x + cubeCorners[corner][0], startX),
                        minimumY + static_cast<float>(y + cubeCorners[corner][1]) * verticalStep,
                        localGridCoordinate(z + cubeCorners[corner][2], startZ));
                    const int gridX = x - startX + cubeCorners[corner][0] + 1;
                    const int gridY = y + cubeCorners[corner][1] + 1;
                    const int gridZ = z - startZ + cubeCorners[corner][2] + 1;
                    densities[corner] = densityGrid[gridIndex(gridX, gridY, gridZ)];
                    cornerGradients[corner] = gradientAt(gridX, gridY, gridZ);
                }

                std::array<glm::vec3, 12> intersections{};
                std::array<glm::vec3, 12> intersectionNormals{};
                std::array<bool, 12> activeEdges{};
                for (int edge = 0; edge < 12; ++edge)
                {
                    int first = cubeEdges[edge][0];
                    int second = cubeEdges[edge][1];
                    if ((densities[first] < isoLevel) ==
                        (densities[second] < isoLevel))
                        continue;
                    // A shared lattice edge must take the same arithmetic path
                    // from either adjacent cell/chunk. Canonically ordering its
                    // world-space endpoints prevents sub-pixel raster cracks.
                    const auto pointLess = [&](int left, int right)
                    {
                        if (positions[left].x != positions[right].x)
                            return positions[left].x < positions[right].x;
                        if (positions[left].y != positions[right].y)
                            return positions[left].y < positions[right].y;
                        return positions[left].z < positions[right].z;
                    };
                    if (pointLess(second, first))
                        std::swap(first, second);
                    const float denominator = densities[second] - densities[first];
                    float amount = std::abs(denominator) > 0.000001f
                        ? std::clamp((isoLevel - densities[first]) / denominator,
                            0.f, 1.f)
                        : 0.5f;
                    // Resolve near-corner crossings to the exact shared
                    // lattice point. Otherwise incident edges can create
                    // duplicate sliver triangles with slightly different
                    // endpoint positions.
                    constexpr float endpointSnap = 0.00001f;
                    if (amount <= endpointSnap)
                        amount = 0.f;
                    else if (amount >= 1.f - endpointSnap)
                        amount = 1.f;
                    activeEdges[edge] = true;
                    intersections[edge] = positions[first] +
                        (positions[second] - positions[first]) * amount;
                    const glm::vec3 interpolatedGradient = cornerGradients[first] +
                        (cornerGradients[second] - cornerGradients[first]) * amount;
                    intersectionNormals[edge] = glm::length(interpolatedGradient) >
                        0.000001f ? glm::normalize(interpolatedGradient) :
                        glm::vec3(0.f, 1.f, 0.f);
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
                        // A saddle face has two valid contour pairings. Do
                        // not choose one from cubeFaces[face][0]: the cell on
                        // the other side sees the same face with a different
                        // first corner and can choose the opposite diagonal.
                        // Select the shorter world-equivalent pairing, with a
                        // coordinate-canonical tie break, so adjacent cells,
                        // patches and chunks make the identical decision.
                        const auto distanceSquared = [&](int first, int second)
                        {
                            const glm::vec3 delta = intersections[first] -
                                intersections[second];
                            return static_cast<double>(glm::dot(delta, delta));
                        };
                        const double firstPairingLength =
                            distanceSquared(crossed[0], crossed[1]) +
                            distanceSquared(crossed[2], crossed[3]);
                        const double secondPairingLength =
                            distanceSquared(crossed[3], crossed[0]) +
                            distanceSquared(crossed[1], crossed[2]);
                        const auto positionLess = [&](int first, int second)
                        {
                            const glm::vec3& a = intersections[first];
                            const glm::vec3& b = intersections[second];
                            if (a.x != b.x) return a.x < b.x;
                            if (a.y != b.y) return a.y < b.y;
                            return a.z < b.z;
                        };
                        const auto canonicalPartnerOfMinimum =
                            [&](int firstA, int firstB, int secondA, int secondB)
                        {
                            std::array<std::array<int, 2>, 2> pairs {{
                                {{ firstA, firstB }}, {{ secondA, secondB }} }};
                            for (auto& pair : pairs)
                                if (positionLess(pair[1], pair[0]))
                                    std::swap(pair[0], pair[1]);
                            if (positionLess(pairs[1][0], pairs[0][0]))
                                std::swap(pairs[0], pairs[1]);
                            return pairs[0][1];
                        };
                        constexpr double pairingEpsilon = 1e-12;
                        bool useFirstPairing = firstPairingLength <
                            secondPairingLength - pairingEpsilon;
                        if (std::abs(firstPairingLength -
                                secondPairingLength) <= pairingEpsilon)
                        {
                            const int firstPartner = canonicalPartnerOfMinimum(
                                crossed[0], crossed[1], crossed[2], crossed[3]);
                            const int secondPartner = canonicalPartnerOfMinimum(
                                crossed[3], crossed[0], crossed[1], crossed[2]);
                            useFirstPairing = positionLess(
                                firstPartner, secondPartner);
                        }
                        if (useFirstPairing)
                        {
                            connect(crossed[0], crossed[1]);
                            connect(crossed[2], crossed[3]);
                        }
                        else
                        {
                            connect(crossed[3], crossed[0]);
                            connect(crossed[1], crossed[2]);
                        }
                    }
                }

                std::array<bool, 12> visited{};
                for (int start = 0; start < 12; ++start)
                {
                    if (!activeEdges[start] || visited[start] ||
                        neighborCounts[start] == 0)
                        continue;
                    std::array<glm::vec3, 12> polygon{};
                    std::array<glm::vec3, 12> polygonNormals{};
                    int polygonSize = 0;
                    int previous = -1;
                    int current = start;
                    for (int step = 0; step < 12; ++step)
                    {
                        polygon[polygonSize] = intersections[current];
                        polygonNormals[polygonSize] = intersectionNormals[current];
                        ++polygonSize;
                        visited[current] = true;
                        const int next = neighbors[current][0] != previous
                            ? neighbors[current][0] : neighbors[current][1];
                        if (next < 0 || next == start || visited[next])
                            break;
                        previous = current;
                        current = next;
                    }
                    emitPolygon(polygon, polygonNormals, polygonSize);
                }
            }
        }
    }
    return vertices;
}

TerrainGen::GenerationSnapshot
TerrainGen::CaptureGenerationSnapshot(
    const PerlinNoiseField& noise) const
{
    GenerationSnapshot snapshot;
    snapshot.chunkSize = chunkSize;
    snapshot.horizontalCells = horizontalCellsPerChunk;
    snapshot.verticalCells = verticalCells;
    snapshot.patchesPerAxis = patchesPerAxis;
    snapshot.terrainShape = terrainShape;
    snapshot.heightStep = heightStep;
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
    snapshot.heightColorBlend = heightColorBlend;
    snapshot.noiseSeed = noise.seed;
    snapshot.noiseFrequency = noise.frequency;
    snapshot.noiseOctaves = noise.octaves;
    snapshot.noiseLacunarity = noise.lacunarity;
    snapshot.noisePersistence = noise.persistence;
    snapshot.noiseOffset = noise.coordinateOffset;
    snapshot.configurationHash = MeshConfigurationHash(noise);
    return snapshot;
}

TerrainGen::GeneratedChunkMesh
TerrainGen::GenerateChunkMesh(const GenerationSnapshot& snapshot,
    int chunkX, int chunkZ)
{
    TerrainGen generator;
    generator.chunkSize = snapshot.chunkSize;
    generator.horizontalCellsPerChunk = snapshot.horizontalCells;
    generator.verticalCells = snapshot.verticalCells;
    generator.patchesPerAxis = snapshot.patchesPerAxis;
    generator.terrainShape = snapshot.terrainShape;
    generator.heightStep = snapshot.heightStep;
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
    generator.heightColorBlend = snapshot.heightColorBlend;

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
    const int patchCount = std::clamp(snapshot.patchesPerAxis, 1,
        std::min(8, std::clamp(snapshot.horizontalCells, 2, 48)));
    result.patches.reserve(static_cast<size_t>(patchCount * patchCount));
    for (int patchZ = 0; patchZ < patchCount; ++patchZ)
    {
        for (int patchX = 0; patchX < patchCount; ++patchX)
        {
            result.patches.push_back({ patchX, patchZ,
                Engine::Components::Mesh::BuildIndexedTerrain(
                    generator.BuildPatchVertices(chunkX, chunkZ, patchX,
                        patchZ, noise)) });
        }
    }
    return result;
}


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

float TerrainGen::SteppedHeight(double worldX, double worldZ,
    const PerlinNoiseField& noise) const
{
    const float step = std::max(0.05f, heightStep);
    const float rawHeight = baseHeight + heightAmplitude *
        noise.SampleFractal2D(worldX, worldZ);
    const float halfHeight = std::max(1.f, verticalSize) * 0.5f;
    return std::clamp(std::round(rawHeight / step) * step,
        -halfHeight, halfHeight);
}

glm::vec3 TerrainGen::ColorForHeight(float height) const
{
    const float lower = std::min(lowHeightMaximum, middleHeightMaximum);
    const float upper = std::max(lowHeightMaximum, middleHeightMaximum);
    if (height <= lower)
        return glm::clamp(lowHeightColor, glm::vec3(0.f), glm::vec3(1.f));
    if (height <= upper)
        return glm::clamp(middleHeightColor, glm::vec3(0.f), glm::vec3(1.f));
    return glm::clamp(highHeightColor, glm::vec3(0.f), glm::vec3(1.f));
}

glm::vec3 TerrainGen::SmoothColorForHeight(float height) const
{
    const float lower = std::min(lowHeightMaximum, middleHeightMaximum);
    const float upper = std::max(lowHeightMaximum, middleHeightMaximum);
    const glm::vec3 low = glm::clamp(
        lowHeightColor, glm::vec3(0.f), glm::vec3(1.f));
    const glm::vec3 middle = glm::clamp(
        middleHeightColor, glm::vec3(0.f), glm::vec3(1.f));
    const glm::vec3 high = glm::clamp(
        highHeightColor, glm::vec3(0.f), glm::vec3(1.f));
    const float range = upper - lower;
    if (range <= 0.0001f)
        return height <= lower ? low : high;

    // Keep the transition configurable and narrow by default. The former 20%
    // half-width made both transitions consume most of the middle band and
    // flattened the visual elevation cues on rounded terrain.
    const float halfWidth = std::max(0.001f, range *
        std::clamp(heightColorBlend, 0.f, 0.5f));
    const float lowToMiddle = glm::smoothstep(
        lower - halfWidth, lower + halfWidth, height);
    const float middleToHigh = glm::smoothstep(
        upper - halfWidth, upper + halfWidth, height);
    return glm::mix(glm::mix(low, middle, lowToMiddle),
        high, middleToHigh);
}

std::vector<TerrainGen::Vertex>
TerrainGen::BuildCubeVertices(int chunkX, int chunkZ,
    int patchX, int patchZ, const PerlinNoiseField& noise) const
{
    const int horizontalCells = std::clamp(horizontalCellsPerChunk, 2, 48);
    const int patchAxisCount = std::clamp(patchesPerAxis, 1,
        std::min(8, horizontalCells));
    const int startX = patchX * horizontalCells / patchAxisCount;
    const int endX = (patchX + 1) * horizontalCells / patchAxisCount;
    const int startZ = patchZ * horizontalCells / patchAxisCount;
    const int endZ = (patchZ + 1) * horizontalCells / patchAxisCount;
    const float cellSize = std::max(1.f, chunkSize) /
        static_cast<float>(horizontalCells);
    const double size = static_cast<double>(std::max(1.f, chunkSize));
    const double logicalPatchOriginX = static_cast<double>(chunkX) * size +
        static_cast<double>(startX) * size / horizontalCells;
    const double logicalPatchOriginZ = static_cast<double>(chunkZ) * size +
        static_cast<double>(startZ) * size / horizontalCells;
    const double uvScale = static_cast<double>(std::max(0.01f, textureScale));
    const double uvBaseX = std::floor(logicalPatchOriginX / uvScale);
    const double uvBaseZ = std::floor(logicalPatchOriginZ / uvScale);
    const auto logicalCellCoordinate = [horizontalCells, size](int chunk,
        int cell)
    {
        return (static_cast<int64_t>(chunk) * horizontalCells + cell) *
            size / horizontalCells;
    };

    std::vector<Vertex> vertices;
    vertices.reserve(static_cast<size_t>(endX - startX) *
        static_cast<size_t>(endZ - startZ) * 30u);

    const auto makeVertex = [&](const glm::vec3& localPosition,
        const glm::vec3& normal)
    {
        Vertex vertex{};
        glm::vec3 tangent = std::abs(normal.x) < 0.9f
            ? glm::vec3(1.f, 0.f, 0.f) : glm::vec3(0.f, 0.f, 1.f);
        tangent -= normal * glm::dot(tangent, normal);
        tangent = glm::normalize(tangent);
        vertex.pos[0] = localPosition.x; vertex.pos[1] = localPosition.y;
        vertex.pos[2] = localPosition.z;
        vertex.normal[0] = normal.x; vertex.normal[1] = normal.y;
        vertex.normal[2] = normal.z;
        vertex.uv[0] = static_cast<float>((logicalPatchOriginX +
            localPosition.x) / uvScale - uvBaseX);
        vertex.uv[1] = static_cast<float>((logicalPatchOriginZ +
            localPosition.z) / uvScale - uvBaseZ);
        vertex.tangent[0] = tangent.x; vertex.tangent[1] = tangent.y;
        vertex.tangent[2] = tangent.z; vertex.tangent[3] = 1.f;
        const glm::vec3 color = ColorForHeight(localPosition.y);
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
                sampleX] = SteppedHeight(
                    logicalCellCoordinate(chunkX, cellX) + size /
                        (2.0 * horizontalCells),
                    logicalCellCoordinate(chunkZ, cellZ) + size /
                        (2.0 * horizontalCells),
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
            const float x0 = static_cast<float>(x - startX) * cellSize;
            const float x1 = x0 + cellSize;
            const float z0 = static_cast<float>(z - startZ) * cellSize;
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

std::vector<TerrainGen::Vertex>
TerrainGen::BuildTriangleVertices(int chunkX, int chunkZ,
    int patchX, int patchZ, const PerlinNoiseField& noise) const
{
    const int cells = std::clamp(horizontalCellsPerChunk, 2, 48);
    const int patchCount = std::clamp(patchesPerAxis, 1, std::min(8, cells));
    const int startX = patchX * cells / patchCount;
    const int endX = (patchX + 1) * cells / patchCount;
    const int startZ = patchZ * cells / patchCount;
    const int endZ = (patchZ + 1) * cells / patchCount;
    const double size = static_cast<double>(std::max(1.f, chunkSize));
    const float cellSize = static_cast<float>(size / cells);
    const double logicalPatchOriginX = static_cast<double>(chunkX) * size +
        static_cast<double>(startX) * size / cells;
    const double logicalPatchOriginZ = static_cast<double>(chunkZ) * size +
        static_cast<double>(startZ) * size / cells;
    const double uvScale = static_cast<double>(std::max(0.01f, textureScale));
    const double uvBaseX = std::floor(logicalPatchOriginX / uvScale);
    const double uvBaseZ = std::floor(logicalPatchOriginZ / uvScale);

    const auto makeVertex = [&](const glm::vec3& point, const glm::vec3& normal)
    {
        Vertex vertex{};
        glm::vec3 tangent = glm::vec3(1.f, 0.f, 0.f) - normal * normal.x;
        if (glm::length(tangent) < 0.0001f)
            tangent = glm::vec3(0.f, 0.f, 1.f);
        tangent = glm::normalize(tangent);
        vertex.pos[0] = point.x; vertex.pos[1] = point.y;
        vertex.pos[2] = point.z;
        vertex.normal[0] = normal.x; vertex.normal[1] = normal.y;
        vertex.normal[2] = normal.z;
        vertex.uv[0] = static_cast<float>((logicalPatchOriginX + point.x) /
            uvScale - uvBaseX);
        vertex.uv[1] = static_cast<float>((logicalPatchOriginZ + point.z) /
            uvScale - uvBaseZ);
        vertex.tangent[0] = tangent.x; vertex.tangent[1] = tangent.y;
        vertex.tangent[2] = tangent.z; vertex.tangent[3] = 1.f;
        const glm::vec3 color = ColorForHeight(point.y);
        vertex.color[0] = color.r; vertex.color[1] = color.g;
        vertex.color[2] = color.b; vertex.color[3] = 1.f;
        return vertex;
    };

    std::vector<Vertex> vertices;
    vertices.reserve(static_cast<size_t>(endX - startX) *
        static_cast<size_t>(endZ - startZ) * 42u);
    const auto emitTriangle = [&](glm::vec3 a, glm::vec3 b, glm::vec3 c,
        const glm::vec3& desiredNormal)
    {
        if (glm::dot(glm::cross(b - a, c - a), desiredNormal) < 0.f)
            std::swap(b, c);
        vertices.push_back(makeVertex(a, desiredNormal));
        vertices.push_back(makeVertex(b, desiredNormal));
        vertices.push_back(makeVertex(c, desiredNormal));
    };
    const auto emitWall = [&](const glm::vec3& first, const glm::vec3& second,
        float bottom, const glm::vec3& normal)
    {
        const glm::vec3 lowFirst(first.x, bottom, first.z);
        const glm::vec3 lowSecond(second.x, bottom, second.z);
        emitTriangle(lowFirst, lowSecond, second, normal);
        emitTriangle(lowFirst, second, first, normal);
    };
    const auto worldCoordinate = [cells, size](int chunk, int cell)
    {
        const int64_t globalCell = static_cast<int64_t>(chunk) * cells + cell;
        return static_cast<double>(globalCell) * size /
            static_cast<double>(cells);
    };
    const auto sampleTriangleHeight = [&](int cellX, int cellZ, int triangle)
    {
        const double x0 = worldCoordinate(chunkX, cellX);
        const double x1 = worldCoordinate(chunkX, cellX + 1);
        const double z0 = worldCoordinate(chunkZ, cellZ);
        const double z1 = worldCoordinate(chunkZ, cellZ + 1);
        // The two right triangles use the p00-p11 diagonal. Sampling their
        // centroids gives each tile one deterministic height across patch and
        // chunk boundaries instead of bending its corners independently.
        const double sampleX = triangle == 0
            ? (x0 + x0 + x1) / 3.0
            : (x0 + x1 + x1) / 3.0;
        const double sampleZ = triangle == 0
            ? (z0 + z1 + z1) / 3.0
            : (z0 + z1 + z0) / 3.0;
        return SteppedHeight(sampleX, sampleZ, noise);
    };
    const int sampledWidth = endX - startX + 2;
    const int sampledDepth = endZ - startZ + 2;
    std::vector<std::array<float, 2>> sampledHeights(
        static_cast<size_t>(sampledWidth) * sampledDepth);
    for (int sampleZ = 0; sampleZ < sampledDepth; ++sampleZ)
    {
        for (int sampleX = 0; sampleX < sampledWidth; ++sampleX)
        {
            const int cellX = startX + sampleX - 1;
            const int cellZ = startZ + sampleZ - 1;
            auto& heights = sampledHeights[
                static_cast<size_t>(sampleZ) * sampledWidth + sampleX];
            heights[0] = sampleTriangleHeight(cellX, cellZ, 0);
            heights[1] = sampleTriangleHeight(cellX, cellZ, 1);
        }
    }
    const auto triangleHeight = [&](int cellX, int cellZ, int triangle)
    {
        const int sampleX = cellX - startX + 1;
        const int sampleZ = cellZ - startZ + 1;
        return sampledHeights[static_cast<size_t>(sampleZ) * sampledWidth +
            sampleX][triangle];
    };
    const auto emitColumn = [&](const std::array<glm::vec3, 3>& footprint,
        float height, const std::array<float, 3>& neighborHeights)
    {
        std::array<glm::vec3, 3> top = footprint;
        for (glm::vec3& point : top)
            point.y = height;
        emitTriangle(top[0], top[1], top[2], { 0.f, 1.f, 0.f });
        for (int edge = 0; edge < 3; ++edge)
        {
            if (height <= neighborHeights[edge] + 0.0001f)
                continue;
            const int next = (edge + 1) % 3;
            glm::vec3 normal = glm::cross(top[next] - top[edge],
                glm::vec3(0.f, 1.f, 0.f));
            normal = glm::normalize(normal);
            emitWall(top[edge], top[next], neighborHeights[edge], normal);
        }
    };

    for (int z = startZ; z < endZ; ++z)
    {
        for (int x = startX; x < endX; ++x)
        {
            const float x0 = static_cast<float>(x - startX) * cellSize;
            const float x1 = x0 + cellSize;
            const float z0 = static_cast<float>(z - startZ) * cellSize;
            const float z1 = z0 + cellSize;
            const glm::vec3 p00(x0, 0.f, z0);
            const glm::vec3 p10(x1, 0.f, z0);
            const glm::vec3 p01(x0, 0.f, z1);
            const glm::vec3 p11(x1, 0.f, z1);
            const float firstHeight = triangleHeight(x, z, 0);
            const float secondHeight = triangleHeight(x, z, 1);
            emitColumn({ p00, p01, p11 }, firstHeight, {
                triangleHeight(x - 1, z, 1),
                triangleHeight(x, z + 1, 1),
                secondHeight });
            emitColumn({ p00, p11, p10 }, secondHeight, {
                firstHeight,
                triangleHeight(x + 1, z, 0),
                triangleHeight(x, z - 1, 0) });
        }
    }
    return vertices;
}

std::vector<TerrainGen::Vertex>
TerrainGen::BuildHexagonVertices(int chunkX, int chunkZ,
    int patchX, int patchZ, const PerlinNoiseField& noise) const
{
    const int cells = std::clamp(horizontalCellsPerChunk, 2, 48);
    const int patchCount = std::clamp(patchesPerAxis, 1, std::min(8, cells));
    const int startX = patchX * cells / patchCount;
    const int endX = (patchX + 1) * cells / patchCount;
    const int startZ = patchZ * cells / patchCount;
    const int endZ = (patchZ + 1) * cells / patchCount;
    const double size = static_cast<double>(std::max(1.f, chunkSize));
    const double cellSize = size / static_cast<double>(cells);
    const double rootThree = std::sqrt(3.0);
    const double radius = cellSize / rootThree;
    const double columnSpacing = radius * 1.5;
    const double rowSpacing = cellSize;
    const double logicalPatchOriginX = static_cast<double>(chunkX) * size +
        static_cast<double>(startX) * cellSize;
    const double logicalPatchOriginZ = static_cast<double>(chunkZ) * size +
        static_cast<double>(startZ) * cellSize;
    const double uvScale = static_cast<double>(std::max(0.01f, textureScale));
    const double uvBaseX = std::floor(logicalPatchOriginX / uvScale);
    const double uvBaseZ = std::floor(logicalPatchOriginZ / uvScale);
    const double minimumX = logicalPatchOriginX;
    const double maximumX = static_cast<double>(chunkX) * size +
        static_cast<double>(endX) * cellSize;
    const double minimumZ = logicalPatchOriginZ;
    const double maximumZ = static_cast<double>(chunkZ) * size +
        static_cast<double>(endZ) * cellSize;

    const auto makeVertex = [&](const glm::dvec3& point, const glm::vec3& normal)
    {
        Vertex vertex{};
        const glm::dvec3 local(point.x - logicalPatchOriginX, point.y,
            point.z - logicalPatchOriginZ);
        glm::vec3 tangent = std::abs(normal.x) < 0.9f
            ? glm::vec3(1.f, 0.f, 0.f) : glm::vec3(0.f, 0.f, 1.f);
        tangent = glm::normalize(tangent - normal * glm::dot(tangent, normal));
        vertex.pos[0] = static_cast<float>(local.x);
        vertex.pos[1] = static_cast<float>(local.y);
        vertex.pos[2] = static_cast<float>(local.z);
        vertex.normal[0] = normal.x; vertex.normal[1] = normal.y;
        vertex.normal[2] = normal.z;
        vertex.uv[0] = static_cast<float>(point.x / uvScale - uvBaseX);
        vertex.uv[1] = static_cast<float>(point.z / uvScale - uvBaseZ);
        vertex.tangent[0] = tangent.x; vertex.tangent[1] = tangent.y;
        vertex.tangent[2] = tangent.z; vertex.tangent[3] = 1.f;
        const glm::vec3 color = ColorForHeight(static_cast<float>(point.y));
        vertex.color[0] = color.r; vertex.color[1] = color.g;
        vertex.color[2] = color.b; vertex.color[3] = 1.f;
        return vertex;
    };
    std::vector<Vertex> vertices;
    const auto emitTriangle = [&](glm::dvec3 a, glm::dvec3 b, glm::dvec3 c,
        const glm::vec3& desiredNormal)
    {
        if (glm::dot(glm::cross(b - a, c - a),
                glm::dvec3(desiredNormal)) < 0.0)
            std::swap(b, c);
        vertices.push_back(makeVertex(a, desiredNormal));
        vertices.push_back(makeVertex(b, desiredNormal));
        vertices.push_back(makeVertex(c, desiredNormal));
    };
    const auto emitWall = [&](glm::dvec3 a, glm::dvec3 b, float bottom,
        const glm::vec3& normal)
    {
        const glm::dvec3 lowA(a.x, bottom, a.z);
        const glm::dvec3 lowB(b.x, bottom, b.z);
        emitTriangle(lowA, lowB, b, normal);
        emitTriangle(lowA, b, a, normal);
    };

    const int64_t firstColumn = static_cast<int64_t>(
        std::ceil(minimumX / columnSpacing));
    const int64_t lastColumn = static_cast<int64_t>(
        std::ceil(maximumX / columnSpacing));
    for (int64_t column = firstColumn; column < lastColumn; ++column)
    {
        // Hex vertices live on an integer half-radius/half-row lattice. This
        // makes a corner shared across a jagged chunk boundary bit-identical,
        // regardless of which hex computes it.
        const auto latticePoint = [&](int64_t xUnits, int64_t zUnits)
        {
            return glm::dvec2(static_cast<double>(xUnits) * radius * 0.5,
                static_cast<double>(zUnits) * cellSize * 0.5);
        };
        const double centerX = latticePoint(column * 3, 0).x;
        if (centerX < minimumX || centerX >= maximumX)
            continue;
        const int columnParity = static_cast<int>(column & 1);
        const double rowOffset = columnParity != 0 ? rowSpacing * 0.5 : 0.0;
        const int64_t firstRow = static_cast<int64_t>(std::ceil(
            (minimumZ - rowOffset) / rowSpacing));
        const int64_t lastRow = static_cast<int64_t>(std::ceil(
            (maximumZ - rowOffset) / rowSpacing));
        for (int64_t row = firstRow; row < lastRow; ++row)
        {
            const glm::dvec2 centerPoint = latticePoint(column * 3,
                row * 2 + columnParity);
            const double centerZ = centerPoint.y;
            if (centerZ < minimumZ || centerZ >= maximumZ)
                continue;
            const float height = SteppedHeight(centerX, centerZ, noise);
            const glm::dvec3 center(centerX, height, centerZ);
            std::array<glm::dvec3, 6> corners{};
            static constexpr int cornerOffsets[6][2] = {
                { 2, 0 }, { 1, 1 }, { -1, 1 },
                { -2, 0 }, { -1, -1 }, { 1, -1 }
            };
            for (int side = 0; side < 6; ++side)
            {
                const glm::dvec2 point = latticePoint(
                    column * 3 + cornerOffsets[side][0],
                    row * 2 + columnParity + cornerOffsets[side][1]);
                corners[side] = { point.x, height, point.y };
            }
            static constexpr int evenNeighborOffsets[6][2] = {
                { 1, 0 }, { 0, 1 }, { -1, 0 },
                { -1, -1 }, { 0, -1 }, { 1, -1 }
            };
            static constexpr int oddNeighborOffsets[6][2] = {
                { 1, 1 }, { 0, 1 }, { -1, 1 },
                { -1, 0 }, { 0, -1 }, { 1, 0 }
            };
            static constexpr glm::vec3 sideNormals[6] = {
                { 0.866025404f, 0.f, 0.5f }, { 0.f, 0.f, 1.f },
                { -0.866025404f, 0.f, 0.5f },
                { -0.866025404f, 0.f, -0.5f }, { 0.f, 0.f, -1.f },
                { 0.866025404f, 0.f, -0.5f }
            };
            for (int side = 0; side < 6; ++side)
            {
                const int next = (side + 1) % 6;
                emitTriangle(center, corners[side], corners[next],
                    { 0.f, 1.f, 0.f });
                const glm::vec3 normal = sideNormals[side];
                const int (*neighborOffsets)[2] = columnParity != 0
                    ? oddNeighborOffsets : evenNeighborOffsets;
                const int64_t neighborColumn = column + neighborOffsets[side][0];
                const int64_t neighborRow = row + neighborOffsets[side][1];
                const glm::dvec2 neighborCenter = latticePoint(neighborColumn * 3,
                    neighborRow * 2 + (neighborColumn & 1));
                const float neighborHeight = SteppedHeight(
                    neighborCenter.x, neighborCenter.y, noise);
                if (height > neighborHeight + 0.0001f)
                    emitWall(corners[side], corners[next], neighborHeight, normal);
            }
        }
    }
    return vertices;
}


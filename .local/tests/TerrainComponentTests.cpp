#include "Core/Object.h"
#include "Core/Compoonents/Obj/Terrain.h"
#include "Core/Compoonents/Obj/Mesh.h"
#include <cassert>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <stdexcept>
#include <vector>

namespace
{
void Require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}
}

int main()
{
    Engine::Core::Object object;
    auto* terrain = object.AddComponent<Engine::Components::Terrain>();
    terrain->Resize(2, 2, 2.f, 3.f);
    auto* mesh = object.GetComponent<Engine::Components::Mesh>();
    assert(mesh && mesh->GetVertexCount() == 9 && mesh->GetIndexCount() == 24);
    assert(mesh->GetVertices()[8].pos[0] == 4.f);
    assert(mesh->GetVertices()[8].pos[2] == 6.f);

    assert(terrain->ApplyBrush(2.f, 3.f, 1.f, 1.f,
        Engine::Components::Terrain::Brush::Raise));
    assert(terrain->heights[4] == 1.f);
    assert(mesh->GetVertices()[4].pos[1] == 1.f);
    assert(!terrain->ApplyBrushFromLocalRay({2.f, 1.f, 3.f},
        {2.f, 2.f, 3.f}, 1.f, 1.f,
        Engine::Components::Terrain::Brush::Raise));
    assert(!terrain->ApplyBrushFromLocalRay({20.f, 1.f, 3.f},
        {20.f, -1.f, 3.f}, 1.f, 1.f,
        Engine::Components::Terrain::Brush::Raise));
    assert(terrain->ApplyBrushFromLocalRay({2.f, 1.f, 3.f},
        {2.f, -1.f, 3.f}, 1.f, 1.f,
        Engine::Components::Terrain::Brush::Lower));

    terrain->layers.push_back({"Assets/test.material", 0.f, 2.f, {1.f, 0.f, 0.f}});
    terrain->Resize(2, 2, 2.f, 3.f);
    assert(terrain->ApplyBrush(2.f, 3.f, 1.f, 1.f,
        Engine::Components::Terrain::Brush::Paint, 1));
    assert(terrain->weights[4 * 2 + 1] == 1.f);
    assert(mesh->GetVertices()[4].color[0] == 1.f);

    const auto saved = terrain->Serialize();
    Engine::Core::Object restoredObject;
    auto* restored = restoredObject.AddComponent<Engine::Components::Terrain>();
    restored->Deserialize(saved);
    assert(restored->layers.size() == 2);
    assert(restored->layers[1].material == "Assets/test.material");
    assert(restored->weights[4 * 2 + 1] == 1.f);
    assert(restoredObject.GetComponent<Engine::Components::Mesh>()->GetIndexCount() == 24);

    Engine::Core::Object lodObject;
    auto* lod = lodObject.AddComponent<Engine::Components::Terrain>();
    lod->Resize(8, 8, 1.f, 1.f);
    lod->heights[2 * 9 + 3] = 4.f;
    lod->Rebuild();
    auto* lodMesh = lodObject.GetComponent<Engine::Components::Mesh>();
    const auto detailed = lodMesh->GetVertices();
    const auto normalAt = [&](int x, int y) {
        const auto& vertex = detailed[static_cast<size_t>(y) * 9 + x];
        return glm::vec3(vertex.normal[0], vertex.normal[1],
            vertex.normal[2]);
    };
    glm::vec3 averaged(0.f);
    for (int y = 0; y <= 4; ++y)
        for (int x = 0; x <= 4; ++x)
            averaged += normalAt(x, y);
    averaged = glm::normalize(averaged);
    lod->SetMeshLodStride(4);
    const auto checkCoarseNormals = [&](const auto& vertices) {
        bool foundCenter = false, foundBorder = false;
        for (const auto& vertex : vertices)
        {
            const int x = static_cast<int>(vertex.pos[0]);
            const int y = static_cast<int>(vertex.pos[2]);
            const glm::vec3 actual(vertex.normal[0], vertex.normal[1],
                vertex.normal[2]);
            if (x == 2 && y == 2)
            {
                Require(glm::dot(actual, averaged) > .9999f,
                    "coarse center normal was not filtered");
                Require(glm::dot(actual, normalAt(x, y)) < .99f,
                    "coarse center retained its detailed normal");
                foundCenter = true;
            }
            if (x == 0 || x == 8 || y == 0 || y == 8)
            {
                Require(glm::dot(actual, normalAt(x, y)) > .99999f,
                    "coarse border normal changed");
                foundBorder = true;
            }
        }
        Require(foundCenter && foundBorder, "coarse LOD fixture incomplete");
    };
    checkCoarseNormals(lodMesh->GetVertices());
    lod->SetMeshLodStride(1);
    const auto cachePath = std::filesystem::temp_directory_path() /
        ("terrain_filtered_lod_" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()) +
            ".terrainmesh");
    Require(lod->SaveCache(cachePath.string(), 47, true),
        "could not write filtered LOD fixture");
    Engine::Core::Object bakedObject;
    auto* baked = bakedObject.AddComponent<Engine::Components::Terrain>();
    Require(baked->LoadCache(cachePath.string(), 47, nullptr),
        "could not reload filtered LOD fixture");
    baked->SetMeshLodStride(4);
    checkCoarseNormals(bakedObject.GetComponent<Engine::Components::Mesh>()->
        GetVertices());
    std::filesystem::remove(cachePath);

    // A tiled far mesh retains grid vertices under the detailed near window.
    // Those unreferenced vertices must not keep an offscreen tile visible.
    Engine::Components::Mesh boundsMesh;
    std::vector<Engine::Components::Mesh::Vertex> vertices(4);
    vertices[0].pos[0] = 0.f; vertices[0].pos[2] = 0.f;
    vertices[1].pos[0] = 1.f; vertices[1].pos[2] = 0.f;
    vertices[2].pos[0] = 0.f; vertices[2].pos[2] = 1.f;
    vertices[3].pos[0] = 10000.f; vertices[3].pos[2] = 10000.f;
    assert(boundsMesh.SetIndexedGeometry(std::move(vertices), {0, 1, 2}));
    assert(boundsMesh.HasBounds());
    assert(boundsMesh.GetBoundsMax().x == 1.f);
    assert(boundsMesh.GetBoundsMax().z == 1.f);
}

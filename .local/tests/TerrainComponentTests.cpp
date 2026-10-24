#include "Core/Object.h"
#include "Core/Compoonents/Obj/Terrain.h"
#include "Core/Compoonents/Obj/Mesh.h"
#include <cassert>
#include <cmath>

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
}

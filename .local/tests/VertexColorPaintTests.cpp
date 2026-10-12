#include "Core/Compoonents/Obj/Mesh.h"
#include "Editor/Core/MeshEditSnapshot.h"
#include <cassert>
#include <cmath>
#include <filesystem>

int main()
{
    using Engine::Components::Mesh;
    using namespace Engine::Editor;
    std::vector<Mesh::Vertex> vertices(3);
    vertices[0].pos[0] = 0.f;
    vertices[1].pos[0] = 1.f;
    vertices[2].pos[1] = 1.f;
    for (auto& vertex : vertices) vertex.color[0] = 1.f;
    Mesh mesh;
    assert(mesh.SetIndexedGeometry(vertices, {0, 1, 2}));
    const std::string before = CaptureMeshSnapshot(mesh);
    vertices[0].color[0] = .25f;
    vertices[0].color[1] = .75f;
    assert(mesh.SetIndexedGeometry(vertices, {0, 1, 2}));
    const std::string after = CaptureMeshSnapshot(mesh);
    std::deque<std::string> undo{before}, redo;
    std::string baseline = after;
    assert(StepMeshSnapshotHistory(mesh, false, undo, redo, baseline));
    assert(std::abs(mesh.GetVertices()[0].color[0] - 1.f) < 1e-6f);
    assert(StepMeshSnapshotHistory(mesh, true, undo, redo, baseline));
    assert(std::abs(mesh.GetVertices()[0].color[1] - .75f) < 1e-6f);
    const auto path = std::filesystem::temp_directory_path() /
        "engine-vertex-paint-regression.mesh";
    assert(Mesh::SaveNativeFile(path.string(), mesh.GetVertices(),
        mesh.GetIndices()));
    Mesh reloaded;
    reloaded.LoadFromFile(path.string());
    assert(std::abs(reloaded.GetVertices()[0].color[0] - .25f) < 1e-6f);
    assert(std::abs(reloaded.GetVertices()[0].color[1] - .75f) < 1e-6f);
    std::filesystem::remove(path);
}

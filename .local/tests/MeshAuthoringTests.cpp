#include "Core/Compoonents/Obj/Mesh.h"
#include "Editor/Core/MeshEditGeometry.h"
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <vector>

int main()
{
    using Engine::Components::Mesh;
    namespace fs = std::filesystem;
    const fs::path path = fs::temp_directory_path() / "engine_mesh_authoring_test.mesh";
    fs::remove(path);
    std::vector<Mesh::Vertex> vertices(4);
    vertices[0].pos[0] = -2.f;
    vertices[1].pos[0] = 2.f;
    vertices[2].pos[1] = 3.f;
    vertices[3].pos[2] = 4.f;
    vertices[2].color[0] = 0.25f;
    vertices[3].weights0[0] = 1.f;
    const std::vector<uint32_t> indices{0, 1, 2, 0, 2, 3};
    if (!Mesh::SaveNativeFile(path.string(), vertices, indices)) return 1;
    Mesh loaded;
    loaded.LoadFromFile(path.string());
    if (loaded.GetVertices().size() != 4 || loaded.GetIndices() != indices ||
        loaded.GetVertices()[2].color[0] != 0.25f ||
        loaded.GetVertices()[3].weights0[0] != 1.f ||
        !loaded.HasBounds() || loaded.GetBoundsMin().x != -2.f ||
        loaded.GetBoundsMax().z != 4.f) return 2;
    const uint64_t revision = loaded.GetConfigurationRevision();
    const uint64_t authoredRevision = loaded.GetAuthoredGeometryRevision();
    auto invalid = vertices;
    invalid[0].pos[0] = std::numeric_limits<float>::quiet_NaN();
    if (loaded.SetIndexedGeometry(invalid, indices) ||
        loaded.GetConfigurationRevision() != revision ||
        loaded.GetAuthoredGeometryRevision() != authoredRevision ||
        loaded.GetIndices() != indices) return 3;
    if (Mesh::SaveNativeFile(path.string(), invalid, indices)) return 4;
    Mesh unchanged;
    unchanged.LoadFromFile(path.string());
    if (unchanged.GetIndices() != indices ||
        unchanged.GetVertices()[0].pos[0] != -2.f) return 5;
    if (loaded.SetIndexedGeometry(vertices, {0, 1, 4})) return 6;
    if (!loaded.SetIndexedGeometry(vertices, indices) ||
        loaded.GetConfigurationRevision() == revision ||
        loaded.GetAuthoredGeometryRevision() == authoredRevision) return 7;
    auto extruded = Engine::Editor::MeshEditGeometry::FromMesh(loaded);
    if (!extruded.ExtrudeFace(0, .5f) || extruded.FaceCount() != 8 ||
        !Mesh::ValidateAuthoredGeometry(extruded.vertices, extruded.indices))
        return 8;
    auto inset = Engine::Editor::MeshEditGeometry::FromMesh(loaded);
    if (!inset.InsetFace(0, .25f) || inset.FaceCount() != 8 ||
        !Mesh::ValidateAuthoredGeometry(inset.vertices, inset.indices))
        return 9;
    auto split = Engine::Editor::MeshEditGeometry::FromMesh(loaded);
    const uint32_t initialEdges = split.EdgeCount();
    if (!split.SplitEdge(0) || split.EdgeCount() <= initialEdges ||
        !Mesh::ValidateAuthoredGeometry(split.vertices, split.indices))
        return 10;
    auto deleted = Engine::Editor::MeshEditGeometry::FromMesh(loaded);
    if (!deleted.DeleteFace(0) || deleted.FaceCount() != 1 ||
        !Mesh::ValidateAuthoredGeometry(deleted.vertices, deleted.indices))
        return 11;
    Mesh legacy;
    legacy.LoadFromFile("Engine/Core/Assets/Prefabs/Fox/Meshes/fox1 1_1.mesh");
    if (legacy.GetVertices().empty() || !legacy.GetIndices().empty()) return 12;
    fs::remove(path);
    return 0;
}

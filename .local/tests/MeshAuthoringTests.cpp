#include "Core/Compoonents/Obj/Mesh.h"
#include "Editor/Core/MeshEditGeometry.h"
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <vector>
#include <glm/glm.hpp>

static float SurfaceArea(const Engine::Editor::MeshEditGeometry& geometry)
{
    float area = 0.f;
    for (size_t i = 0; i + 2 < geometry.indices.size(); i += 3)
    {
        const auto position = [&](uint32_t index)
        {
            const auto& v = geometry.vertices[geometry.indices[i + index]];
            return glm::vec3(v.pos[0], v.pos[1], v.pos[2]);
        };
        area += .5f * glm::length(glm::cross(position(1) - position(0),
            position(2) - position(0)));
    }
    return area;
}

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
    const uint64_t interactiveRevision = loaded.GetAuthoredGeometryRevision();
    auto movedVertices = loaded.GetVertices();
    movedVertices[0].pos[0] = -1.5f;
    if (!loaded.UpdateAuthoredVertices(std::move(movedVertices)) ||
        loaded.GetIndices() != indices ||
        loaded.GetAuthoredGeometryRevision() == interactiveRevision ||
        loaded.GetBoundsMin().x != -1.5f) return 31;
    if (!loaded.SetIndexedGeometry(vertices, indices)) return 32;
    auto extruded = Engine::Editor::MeshEditGeometry::FromMesh(loaded);
    if (!extruded.ExtrudeFace(0, .5f) || extruded.FaceCount() != 8 ||
        !Mesh::ValidateAuthoredGeometry(extruded.vertices, extruded.indices))
        return 8;
    auto inset = Engine::Editor::MeshEditGeometry::FromMesh(loaded);
    const float originalArea = SurfaceArea(inset);
    if (!inset.InsetFace(0, .25f) || inset.FaceCount() != 8 ||
        !Mesh::ValidateAuthoredGeometry(inset.vertices, inset.indices) ||
        std::abs(SurfaceArea(inset) - originalArea) > 1e-4f)
        return 9;
    auto split = Engine::Editor::MeshEditGeometry::FromMesh(loaded);
    const uint32_t initialEdges = split.EdgeCount();
    if (!split.SplitEdge(0) || split.EdgeCount() <= initialEdges ||
        !Mesh::ValidateAuthoredGeometry(split.vertices, split.indices) ||
        std::abs(SurfaceArea(split) - originalArea) > 1e-4f)
        return 10;
    auto deleted = Engine::Editor::MeshEditGeometry::FromMesh(loaded);
    if (!deleted.DeleteFace(0) || deleted.FaceCount() != 1 ||
        !Mesh::ValidateAuthoredGeometry(deleted.vertices, deleted.indices))
        return 11;
    auto transformed = Engine::Editor::MeshEditGeometry::FromMesh(loaded);
    if (!transformed.TransformSelection(0, {0, 1}, { .26f, 0.f, 0.f },
            { 0.f, 0.f, 0.f }, { 1.f, 1.f, 1.f }, 2,
            { 0.f, 0.f, 0.f }, .1f) ||
        std::abs(transformed.vertices[0].pos[0] - (-1.7f)) > 1e-4f ||
        transformed.vertices[2].pos[1] != 3.f) return 13;
    const auto edgeList = transformed.EdgeList();
    if (edgeList.size() != transformed.EdgeCount() ||
        edgeList != Engine::Editor::MeshEditGeometry::EdgeListFromIndices(
            transformed.indices)) return 28;
    for (uint32_t edge = 0; edge < edgeList.size(); ++edge)
        if (edgeList[edge] != transformed.EdgeAt(edge)) return 29;
    auto rotated = Engine::Editor::MeshEditGeometry::FromMesh(loaded);
    if (!rotated.TransformSelection(0, {1}, {0.f, 0.f, 0.f},
            {0.f, 0.f, 44.f}, {1.f, 1.f, 1.f}, 2,
            {0.f, 0.f, 0.f}, 0.f, 45.f, 0.f) ||
        std::abs(rotated.vertices[1].pos[0] - 1.4142135f) > 1e-3f ||
        std::abs(rotated.vertices[1].pos[1] - 1.4142135f) > 1e-3f)
        return 30;
    auto duplicated = Engine::Editor::MeshEditGeometry::FromMesh(loaded);
    if (!duplicated.DuplicateFaces({0}, {0.f, 0.f, 1.f}) ||
        duplicated.FaceCount() != 3 ||
        !Mesh::ValidateAuthoredGeometry(duplicated.vertices, duplicated.indices))
        return 14;
    auto welded = Engine::Editor::MeshEditGeometry::FromMesh(loaded);
    if (!welded.WeldVertices({1, 3}) ||
        !Mesh::ValidateAuthoredGeometry(welded.vertices, welded.indices))
        return 15;
    auto removed = Engine::Editor::MeshEditGeometry::FromMesh(loaded);
    if (!removed.DeleteSelection(2, {1}) || removed.FaceCount() != 1)
        return 16;
    std::vector<Mesh::Vertex> quadVertices(4);
    quadVertices[0].pos[0] = 0.f; quadVertices[0].pos[1] = 0.f;
    quadVertices[1].pos[0] = 1.f; quadVertices[1].pos[1] = 0.f;
    quadVertices[2].pos[0] = 1.f; quadVertices[2].pos[1] = 1.f;
    quadVertices[3].pos[0] = 0.f; quadVertices[3].pos[1] = 1.f;
    Engine::Editor::MeshEditGeometry quad{ quadVertices, {0, 1, 2, 0, 2, 3} };
    auto cut = quad;
    uint32_t bottom = UINT32_MAX, diagonal = UINT32_MAX;
    for (uint32_t edge = 0; edge < quad.EdgeCount(); ++edge)
    {
        if (quad.EdgeAt(edge) == std::pair<uint32_t,uint32_t>{0,1}) bottom = edge;
        if (quad.EdgeAt(edge) == std::pair<uint32_t,uint32_t>{0,2}) diagonal = edge;
    }
    if (bottom == UINT32_MAX || diagonal == UINT32_MAX ||
        cut.LoopCut(diagonal) || !cut.LoopCut(bottom) ||
        cut.FaceCount() != 4 ||
        std::abs(SurfaceArea(cut) - SurfaceArea(quad)) > 1e-4f)
        return 17;
    auto edgeDeleted = quad;
    if (!edgeDeleted.DeleteSelection(1, { bottom }) ||
        edgeDeleted.FaceCount() != 1) return 34;
    auto beveled = quad;
    if (!beveled.BevelEdge(diagonal, .2f) || beveled.FaceCount() != 4 ||
        !Mesh::ValidateAuthoredGeometry(beveled.vertices, beveled.indices))
        return 18;
    std::vector<Mesh::Vertex> rings(6);
    for (int i = 0; i < 6; ++i)
    {
        rings[i].pos[0] = static_cast<float>(i % 3 == 1);
        rings[i].pos[1] = static_cast<float>(i % 3 == 2);
        rings[i].pos[2] = i >= 3 ? 1.f : 0.f;
    }
    Engine::Editor::MeshEditGeometry bridge{ rings, {0, 1, 2, 3, 4, 5} };
    std::vector<uint32_t> boundary;
    for (uint32_t edge = 0; edge < bridge.EdgeCount(); ++edge)
        boundary.push_back(edge);
    if (!bridge.BridgeBoundaries(boundary) || bridge.FaceCount() != 8 ||
        !Mesh::ValidateAuthoredGeometry(bridge.vertices, bridge.indices))
        return 19;
    auto fill = quad;
    if (fill.FillBoundary({diagonal}) ||
        fill.FaceCount() != quad.FaceCount()) return 20;
    Engine::Editor::MeshEditGeometry openTetra{ vertices,
        {0, 2, 1, 0, 1, 3, 1, 2, 3} };
    std::vector<uint32_t> rim;
    for (uint32_t edge = 0; edge < openTetra.EdgeCount(); ++edge)
    {
        auto pair = openTetra.EdgeAt(edge);
        if (pair == std::pair<uint32_t,uint32_t>{0,2} ||
            pair == std::pair<uint32_t,uint32_t>{0,3} ||
            pair == std::pair<uint32_t,uint32_t>{2,3}) rim.push_back(edge);
    }
    if (rim.size() != 3 || !openTetra.FillBoundary(rim) ||
        openTetra.FaceCount() != 4 ||
        !Mesh::ValidateAuthoredGeometry(openTetra.vertices, openTetra.indices))
        return 21;
    auto invalidBridge = quad;
    if (invalidBridge.BridgeBoundaries({bottom}) ||
        invalidBridge.FaceCount() != quad.FaceCount()) return 22;
    std::vector<Mesh::Vertex> stripVertices(6);
    for (int i = 0; i < 6; ++i)
    {
        stripVertices[i].pos[0] = static_cast<float>(i % 3);
        stripVertices[i].pos[1] = i >= 3 ? 1.f : 0.f;
    }
    Engine::Editor::MeshEditGeometry strip{ stripVertices,
        {0, 1, 4, 0, 4, 3, 1, 2, 5, 1, 5, 4} };
    uint32_t stripStart = UINT32_MAX;
    for (uint32_t edge = 0; edge < strip.EdgeCount(); ++edge)
        if (strip.EdgeAt(edge) == std::pair<uint32_t,uint32_t>{0,3})
            stripStart = edge;
    if (stripStart == UINT32_MAX) return 23;
    auto interiorCut = strip;
    uint32_t interiorEdge = UINT32_MAX;
    for (uint32_t edge = 0; edge < interiorCut.EdgeCount(); ++edge)
        if (interiorCut.EdgeAt(edge) == std::pair<uint32_t,uint32_t>{1,4})
            interiorEdge = edge;
    if (interiorEdge == UINT32_MAX || !interiorCut.LoopCut(interiorEdge) ||
        interiorCut.FaceCount() != 8) return 27;
    if (!strip.LoopCut(stripStart)) return 24;
    if (strip.FaceCount() != 8) return 25;
    if (std::abs(SurfaceArea(strip) - 2.f) > 1e-4f) return 26;
    constexpr uint32_t stripLength = 64;
    std::vector<Mesh::Vertex> longVertices((stripLength + 1) * 2);
    for (uint32_t x = 0; x <= stripLength; ++x)
    {
        longVertices[x].pos[0] = static_cast<float>(x);
        longVertices[stripLength + 1 + x].pos[0] = static_cast<float>(x);
        longVertices[stripLength + 1 + x].pos[1] = 1.f;
    }
    std::vector<uint32_t> longIndices;
    for (uint32_t x = 0; x < stripLength; ++x)
    {
        const uint32_t top = stripLength + 1 + x;
        longIndices.insert(longIndices.end(), {x, x + 1, top + 1,
            x, top + 1, top});
    }
    Engine::Editor::MeshEditGeometry longStrip{ longVertices, longIndices };
    uint32_t longInterior = UINT32_MAX;
    const auto longEdges = longStrip.EdgeList();
    for (uint32_t edge = 0; edge < longEdges.size(); ++edge)
        if (longEdges[edge] ==
            std::pair<uint32_t,uint32_t>{32, stripLength + 1 + 32})
            longInterior = edge;
    if (longInterior == UINT32_MAX || !longStrip.LoopCut(longInterior) ||
        longStrip.FaceCount() != stripLength * 4 ||
        std::abs(SurfaceArea(longStrip) - stripLength) > 1e-3f)
        return 35;
    Mesh legacy;
    legacy.LoadFromFile("Engine/Core/Assets/Prefabs/Fox/Meshes/fox1 1_1.mesh");
    if (legacy.GetVertices().empty() || !legacy.GetIndices().empty()) return 12;
    auto legacyVertices = legacy.GetVertices();
    legacyVertices[0].pos[0] += .01f;
    if (!legacy.UpdateAuthoredVertices(std::move(legacyVertices)) ||
        !legacy.GetIndices().empty()) return 33;
    fs::remove(path);
    return 0;
}

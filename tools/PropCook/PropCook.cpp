// PropCook: turns a scanned or high-poly source model (FBX or OBJ, via ufbx)
// into game-ready terrain props: native .mesh LOD chains with a ground pivot,
// plus one prefab carrying every level (Mesh.lodMeshes) for TerrainObjectLayer.
//
//   PropCook <source.fbx|obj> <outMeshDir> [options]
//
//   --list                 print every mesh node (index, name, triangles, size) and exit
//   --split                one prop per mesh node (rock packs); default merges all nodes
//   --only 3,7,12          with --split: keep these node indices (from --list)
//   --name <prefix>        output prefix (default: source stem); --split appends _NN
//   --lods 3000,900,250    triangle targets for LOD0..n
//   --scale <s>            extra uniform scale after conversion to metres
//   --sink <fraction>      lower the pivot by this share of the height (default 0.04),
//                          so uneven bases sit in the ground instead of floating
//   --max-error <fraction> LOD0 error budget as a share of the bounding diagonal
//                          (default 0.01); it doubles per LOD. A LOD stops above
//                          its triangle target rather than exceed the budget.
//   --mesh-asset <path>    asset path of outMeshDir as prefabs reference it
//                          (e.g. Assets/Models/Woodland/Rocks/Meshes)
//   --prefab-dir <dir>     write <name>.prefab here (LOD1..n in Mesh.lodMeshes)
//   --material <asset>     Material.materialAsset written into the prefabs
//
// Geometry is converted to metres, Y up, with V flipped like the engine's FBX
// importer. Simplification is quadric edge collapse (Garland-Heckbert) that
// keeps UV seams intact, unlike the engine's vertex clustering: a vertex split
// across a seam only collapses when each of its copies has a partner in the
// target that shares a triangle with it, so islands never smear. Open borders
// stay locked. Every LOD is simplified from the full source; tangents are
// rebuilt per LOD.

#include "Core/Compoonents/Mesh/Mesh.h"

#include <ufbx.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <queue>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;
using Engine::Components::Mesh;

namespace
{
struct SourceVertex
{
    float position[3];
    float normal[3];
    float uv[2];
};

struct Geometry
{
    std::string name;
    std::vector<SourceVertex> corners; // unindexed triangle list
};

int Fail(const std::string& message)
{
    std::fprintf(stderr, "PropCook: %s\n", message.c_str());
    return 1;
}

std::vector<int> ParseList(const std::string& text)
{
    std::vector<int> values;
    std::stringstream stream(text);
    std::string item;
    while (std::getline(stream, item, ','))
        if (!item.empty()) values.push_back(std::stoi(item));
    return values;
}

void AppendNode(const ufbx_node& node, Geometry& geometry)
{
    const ufbx_mesh& mesh = *node.mesh;
    const ufbx_matrix normalMatrix = ufbx_matrix_for_normals(&node.geometry_to_world);
    // Mirrored transforms flip the winding.
    const ufbx_matrix& m = node.geometry_to_world;
    const double determinant =
        m.m00 * (m.m11 * m.m22 - m.m12 * m.m21) -
        m.m01 * (m.m10 * m.m22 - m.m12 * m.m20) +
        m.m02 * (m.m10 * m.m21 - m.m11 * m.m20);
    std::vector<uint32_t> triangles(mesh.max_face_triangles * 3);
    for (size_t face = 0; face < mesh.faces.count; ++face)
    {
        const uint32_t count = ufbx_triangulate_face(triangles.data(), triangles.size(),
            &mesh, mesh.faces.data[face]);
        for (uint32_t triangle = 0; triangle < count; ++triangle)
            for (int corner = 0; corner < 3; ++corner)
            {
                const int source = determinant < 0 ? 2 - corner : corner;
                const uint32_t index = triangles[triangle * 3 + source];
                SourceVertex vertex{};
                const ufbx_vec3 p = ufbx_transform_position(&node.geometry_to_world,
                    ufbx_get_vertex_vec3(&mesh.vertex_position, index));
                vertex.position[0] = float(p.x);
                vertex.position[1] = float(p.y);
                vertex.position[2] = float(p.z);
                if (mesh.vertex_normal.exists)
                {
                    ufbx_vec3 n = ufbx_transform_direction(&normalMatrix,
                        ufbx_get_vertex_vec3(&mesh.vertex_normal, index));
                    const double length = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
                    if (length > 1e-12) { n.x /= length; n.y /= length; n.z /= length; }
                    vertex.normal[0] = float(n.x);
                    vertex.normal[1] = float(n.y);
                    vertex.normal[2] = float(n.z);
                }
                if (mesh.vertex_uv.exists)
                {
                    const ufbx_vec2 uv = ufbx_get_vertex_vec2(&mesh.vertex_uv, index);
                    vertex.uv[0] = float(uv.x);
                    vertex.uv[1] = 1.f - float(uv.y);
                }
                geometry.corners.push_back(vertex);
            }
    }
}

// Face normals for sources without authored normals.
void FillMissingNormals(std::vector<SourceVertex>& corners)
{
    for (size_t i = 0; i + 2 < corners.size(); i += 3)
    {
        auto& a = corners[i]; auto& b = corners[i + 1]; auto& c = corners[i + 2];
        if (a.normal[0] || a.normal[1] || a.normal[2]) continue;
        const float e1[3]{b.position[0] - a.position[0], b.position[1] - a.position[1], b.position[2] - a.position[2]};
        const float e2[3]{c.position[0] - a.position[0], c.position[1] - a.position[1], c.position[2] - a.position[2]};
        float n[3]{e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
        const float length = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (length > 1e-20f) for (float& value : n) value /= length;
        for (auto* v : {&a, &b, &c}) std::copy(n, n + 3, v->normal);
    }
}

void Bounds(const std::vector<SourceVertex>& corners, float minimum[3], float maximum[3])
{
    for (int axis = 0; axis < 3; ++axis) { minimum[axis] = 1e30f; maximum[axis] = -1e30f; }
    for (const auto& v : corners)
        for (int axis = 0; axis < 3; ++axis)
        {
            minimum[axis] = std::min(minimum[axis], v.position[axis]);
            maximum[axis] = std::max(maximum[axis], v.position[axis]);
        }
}

struct BytesHash
{
    size_t operator()(const std::string& key) const noexcept { return std::hash<std::string>{}(key); }
};

// Welds identical corners (bit-exact) into an indexed mesh.
void Weld(const std::vector<SourceVertex>& corners, std::vector<SourceVertex>& vertices,
    std::vector<uint32_t>& indices)
{
    std::unordered_map<std::string, uint32_t, BytesHash> unique;
    unique.reserve(corners.size());
    indices.resize(corners.size());
    for (size_t i = 0; i < corners.size(); ++i)
    {
        const std::string key(reinterpret_cast<const char*>(&corners[i]), sizeof(SourceVertex));
        auto [it, inserted] = unique.emplace(key, uint32_t(vertices.size()));
        if (inserted) vertices.push_back(corners[i]);
        indices[i] = it->second;
    }
}

// Quadric edge collapse (half-edge: a vertex moves onto a neighbour, so no new
// positions or attributes are invented) with seam-preserving remapping.
class Simplifier
{
public:
    Simplifier(const std::vector<SourceVertex>& vertices, const std::vector<uint32_t>& indices)
        : m_vertices(vertices), m_indices(indices) {}

    // Stops at targetTriangles or when the next collapse would move the
    // surface by more than maxError (RMS distance, metres), whichever is first.
    std::vector<uint32_t> Run(size_t targetTriangles, double maxError)
    {
        Setup();
        const double limit = maxError * maxError;
        size_t alive = m_triangles.size();
        while (alive > targetTriangles && !m_heap.empty())
        {
            const Candidate candidate = m_heap.top();
            if (candidate.cost > limit) break;
            m_heap.pop();
            if (!m_groupAlive[candidate.from] || !m_groupAlive[candidate.to] ||
                m_version[candidate.from] != candidate.fromVersion ||
                m_version[candidate.to] != candidate.toVersion) continue;
            alive -= Collapse(candidate.from, candidate.to);
        }
        std::vector<uint32_t> result;
        for (size_t t = 0; t < m_triangles.size(); ++t)
            if (m_triangleAlive[t])
                result.insert(result.end(), m_triangles[t].begin(), m_triangles[t].end());
        return result;
    }

private:
    using Quadric = std::array<double, 10>;
    using Point = std::array<double, 3>;
    struct Candidate
    {
        double cost;
        uint32_t from, to, fromVersion, toVersion;
        bool operator<(const Candidate& other) const { return cost > other.cost; }
    };

    const std::vector<SourceVertex>& m_vertices;
    const std::vector<uint32_t>& m_indices;
    std::vector<uint32_t> m_group;                 // attribute vertex -> position group
    std::vector<std::vector<uint32_t>> m_groupTriangles;
    std::vector<Point> m_position;
    std::vector<Quadric> m_quadric;
    std::vector<double> m_weight; // summed plane areas behind each quadric
    std::vector<uint8_t> m_groupAlive, m_border;
    std::vector<uint32_t> m_version;
    std::vector<std::array<uint32_t, 3>> m_triangles;
    std::vector<uint8_t> m_triangleAlive;
    std::priority_queue<Candidate> m_heap;

    static void AddPlane(Quadric& q, double a, double b, double c, double d, double w)
    {
        const double v[10]{a * a, a * b, a * c, a * d, b * b, b * c, b * d, c * c, c * d, d * d};
        for (int i = 0; i < 10; ++i) q[i] += v[i] * w;
    }
    static double Evaluate(const Quadric& q, const Point& p)
    {
        const double x = p[0], y = p[1], z = p[2];
        return q[0] * x * x + 2 * q[1] * x * y + 2 * q[2] * x * z + 2 * q[3] * x +
            q[4] * y * y + 2 * q[5] * y * z + 2 * q[6] * y + q[7] * z * z + 2 * q[8] * z + q[9];
    }
    // Unnormalised face normal; the moved group may be placed at `replacement`.
    Point Normal(const std::array<uint32_t, 3>& tri, uint32_t moved, const Point* replacement) const
    {
        Point p[3];
        for (int k = 0; k < 3; ++k)
            p[k] = replacement && m_group[tri[k]] == moved ? *replacement : m_position[m_group[tri[k]]];
        const double e1[3]{p[1][0] - p[0][0], p[1][1] - p[0][1], p[1][2] - p[0][2]};
        const double e2[3]{p[2][0] - p[0][0], p[2][1] - p[0][1], p[2][2] - p[0][2]};
        return {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
    }

    void Setup()
    {
        // Position groups: attribute vertices that share a position.
        std::unordered_map<std::string, uint32_t, BytesHash> groups;
        m_group.resize(m_vertices.size());
        m_position.clear();
        for (uint32_t v = 0; v < m_vertices.size(); ++v)
        {
            const std::string key(reinterpret_cast<const char*>(m_vertices[v].position), sizeof(float) * 3);
            auto [it, inserted] = groups.emplace(key, uint32_t(m_position.size()));
            if (inserted)
                m_position.push_back({m_vertices[v].position[0], m_vertices[v].position[1], m_vertices[v].position[2]});
            m_group[v] = it->second;
        }
        const size_t groupCount = m_position.size();
        m_groupTriangles.assign(groupCount, {});
        m_quadric.assign(groupCount, Quadric{});
        m_weight.assign(groupCount, 0.0);
        m_groupAlive.assign(groupCount, 1);
        m_border.assign(groupCount, 0);
        m_version.assign(groupCount, 0);
        m_triangles.clear();
        m_triangleAlive.clear();
        std::unordered_map<uint64_t, int> edgeUse;
        for (size_t i = 0; i + 2 < m_indices.size(); i += 3)
        {
            const std::array<uint32_t, 3> tri{m_indices[i], m_indices[i + 1], m_indices[i + 2]};
            const uint32_t a = m_group[tri[0]], b = m_group[tri[1]], c = m_group[tri[2]];
            if (a == b || b == c || a == c) continue;
            const uint32_t index = uint32_t(m_triangles.size());
            m_triangles.push_back(tri);
            m_triangleAlive.push_back(1);
            Point n = Normal(tri, ~0u, nullptr);
            const double area = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
            if (area > 1e-30) for (double& value : n) value /= area;
            const double d = -(n[0] * m_position[a][0] + n[1] * m_position[a][1] + n[2] * m_position[a][2]);
            for (uint32_t g : {a, b, c})
            {
                m_groupTriangles[g].push_back(index);
                AddPlane(m_quadric[g], n[0], n[1], n[2], d, area);
                m_weight[g] += area;
            }
            for (int k = 0; k < 3; ++k)
            {
                const uint32_t x = m_group[tri[k]], y = m_group[tri[(k + 1) % 3]];
                ++edgeUse[(uint64_t(std::min(x, y)) << 32) | std::max(x, y)];
            }
        }
        for (const auto& [edge, uses] : edgeUse)
            if (uses == 1) { m_border[uint32_t(edge >> 32)] = 1; m_border[uint32_t(edge)] = 1; }
        for (uint32_t g = 0; g < groupCount; ++g)
        {
            std::vector<uint32_t> neighbors;
            Neighbors(g, neighbors);
            for (uint32_t n : neighbors) Push(g, n);
        }
    }

    void Neighbors(uint32_t g, std::vector<uint32_t>& out) const
    {
        out.clear();
        for (uint32_t t : m_groupTriangles[g])
            if (m_triangleAlive[t])
                for (uint32_t v : m_triangles[t])
                    if (m_group[v] != g) out.push_back(m_group[v]);
        std::sort(out.begin(), out.end());
        out.erase(std::unique(out.begin(), out.end()), out.end());
    }

    void Push(uint32_t from, uint32_t to)
    {
        if (m_border[from]) return; // open borders stay put
        Quadric q = m_quadric[from];
        for (int i = 0; i < 10; ++i) q[i] += m_quadric[to][i];
        // Area-normalised: the mean squared distance from the merged planes.
        const double weight = std::max(1e-30, m_weight[from] + m_weight[to]);
        m_heap.push({std::max(0.0, Evaluate(q, m_position[to])) / weight, from, to,
            m_version[from], m_version[to]});
    }

    // Moves group `from` onto `to`; returns the triangles removed (0 if refused).
    size_t Collapse(uint32_t from, uint32_t to)
    {
        // Every attribute copy of `from` needs one partner in `to` sharing a
        // triangle with it; that keeps seams and UV islands intact.
        std::unordered_map<uint32_t, uint32_t> remap;
        for (uint32_t t : m_groupTriangles[from])
        {
            if (!m_triangleAlive[t]) continue;
            uint32_t mine = ~0u, partner = ~0u;
            for (uint32_t v : m_triangles[t])
            {
                if (m_group[v] == from) mine = v;
                if (m_group[v] == to) partner = v;
            }
            if (partner == ~0u) continue;
            auto [it, inserted] = remap.emplace(mine, partner);
            if (!inserted && it->second != partner) return 0; // ambiguous across a seam
        }
        for (uint32_t t : m_groupTriangles[from])
            if (m_triangleAlive[t])
                for (uint32_t v : m_triangles[t])
                    if (m_group[v] == from && !remap.count(v)) return 0;
        // Link condition: an interior edge shares exactly two neighbours.
        std::vector<uint32_t> a, b, common;
        Neighbors(from, a);
        Neighbors(to, b);
        std::set_intersection(a.begin(), a.end(), b.begin(), b.end(), std::back_inserter(common));
        if (common.size() > 2) return 0;
        // Reject flips and slivers among the triangles that survive.
        for (uint32_t t : m_groupTriangles[from])
        {
            if (!m_triangleAlive[t]) continue;
            const auto& tri = m_triangles[t];
            bool hasTo = false;
            for (uint32_t v : tri) hasTo = hasTo || m_group[v] == to;
            if (hasTo) continue;
            const Point before = Normal(tri, ~0u, nullptr);
            const Point after = Normal(tri, from, &m_position[to]);
            const double lb = std::sqrt(before[0] * before[0] + before[1] * before[1] + before[2] * before[2]);
            const double la = std::sqrt(after[0] * after[0] + after[1] * after[1] + after[2] * after[2]);
            if (la < 1e-14 || lb < 1e-14) return 0;
            if ((before[0] * after[0] + before[1] * after[1] + before[2] * after[2]) / (la * lb) < .3) return 0;
        }
        size_t removed = 0;
        for (uint32_t t : m_groupTriangles[from])
        {
            if (!m_triangleAlive[t]) continue;
            auto& tri = m_triangles[t];
            bool hasTo = false;
            for (uint32_t v : tri) hasTo = hasTo || m_group[v] == to;
            if (hasTo) { m_triangleAlive[t] = 0; ++removed; continue; }
            for (uint32_t& v : tri)
                if (m_group[v] == from) v = remap.at(v);
            m_groupTriangles[to].push_back(t);
        }
        for (int i = 0; i < 10; ++i) m_quadric[to][i] += m_quadric[from][i];
        m_weight[to] += m_weight[from];
        m_groupAlive[from] = 0;
        m_groupTriangles[from].clear();
        auto& list = m_groupTriangles[to];
        list.erase(std::remove_if(list.begin(), list.end(),
            [&](uint32_t t) { return !m_triangleAlive[t]; }), list.end());
        ++m_version[to];
        std::vector<uint32_t> neighbors;
        Neighbors(to, neighbors);
        for (uint32_t n : neighbors) { Push(to, n); Push(n, to); }
        return removed;
    }
};

// Drops unreferenced vertices and renumbers the index buffer.
void Compact(const std::vector<SourceVertex>& vertices, std::vector<uint32_t>& indices,
    std::vector<SourceVertex>& output)
{
    std::vector<uint32_t> remap(vertices.size(), ~0u);
    output.clear();
    for (uint32_t& index : indices)
    {
        if (remap[index] == ~0u)
        {
            remap[index] = uint32_t(output.size());
            output.push_back(vertices[index]);
        }
        index = remap[index];
    }
}

// Per-vertex tangents from UV derivatives, orthonormalised against the normal;
// w orients cross(N, T) along +V as the engine's OBJ path does.
std::vector<Mesh::Vertex> BuildEngineVertices(const std::vector<SourceVertex>& vertices,
    const std::vector<uint32_t>& indices)
{
    std::vector<float> tangent(vertices.size() * 3, 0.f), bitangent(vertices.size() * 3, 0.f);
    for (size_t i = 0; i + 2 < indices.size(); i += 3)
    {
        const auto& a = vertices[indices[i]];
        const auto& b = vertices[indices[i + 1]];
        const auto& c = vertices[indices[i + 2]];
        const float e1[3]{b.position[0] - a.position[0], b.position[1] - a.position[1], b.position[2] - a.position[2]};
        const float e2[3]{c.position[0] - a.position[0], c.position[1] - a.position[1], c.position[2] - a.position[2]};
        const float du1 = b.uv[0] - a.uv[0], dv1 = b.uv[1] - a.uv[1];
        const float du2 = c.uv[0] - a.uv[0], dv2 = c.uv[1] - a.uv[1];
        const float det = du1 * dv2 - du2 * dv1;
        if (std::abs(det) < 1e-20f) continue;
        const float r = 1.f / det;
        for (int axis = 0; axis < 3; ++axis)
        {
            const float t = (e1[axis] * dv2 - e2[axis] * dv1) * r;
            const float s = (e2[axis] * du1 - e1[axis] * du2) * r;
            for (int k = 0; k < 3; ++k)
            {
                tangent[indices[i + k] * 3 + axis] += t;
                bitangent[indices[i + k] * 3 + axis] += s;
            }
        }
    }
    std::vector<Mesh::Vertex> result(vertices.size());
    for (size_t i = 0; i < vertices.size(); ++i)
    {
        Mesh::Vertex vertex{};
        const auto& source = vertices[i];
        std::copy(source.position, source.position + 3, vertex.pos);
        std::copy(source.normal, source.normal + 3, vertex.normal);
        std::copy(source.uv, source.uv + 2, vertex.uv);
        std::copy(source.uv, source.uv + 2, vertex.uv1);
        const float* n = source.normal;
        float* t = &tangent[i * 3];
        const float d = t[0] * n[0] + t[1] * n[1] + t[2] * n[2];
        float o[3]{t[0] - n[0] * d, t[1] - n[1] * d, t[2] - n[2] * d};
        float length = std::sqrt(o[0] * o[0] + o[1] * o[1] + o[2] * o[2]);
        if (length < 1e-12f)
        {
            // Any unit vector perpendicular to the normal.
            const float reference[3]{std::abs(n[0]) < .9f ? 1.f : 0.f, std::abs(n[0]) < .9f ? 0.f : 1.f, 0.f};
            const float rd = reference[0] * n[0] + reference[1] * n[1];
            o[0] = reference[0] - n[0] * rd; o[1] = reference[1] - n[1] * rd; o[2] = -n[2] * rd;
            length = std::sqrt(o[0] * o[0] + o[1] * o[1] + o[2] * o[2]);
        }
        for (int axis = 0; axis < 3; ++axis) vertex.tangent[axis] = o[axis] / length;
        const float* b = &bitangent[i * 3];
        const float cross[3]{n[1] * vertex.tangent[2] - n[2] * vertex.tangent[1],
            n[2] * vertex.tangent[0] - n[0] * vertex.tangent[2],
            n[0] * vertex.tangent[1] - n[1] * vertex.tangent[0]};
        vertex.tangent[3] = cross[0] * b[0] + cross[1] * b[1] + cross[2] * b[2] < 0.f ? -1.f : 1.f;
        result[i] = vertex;
    }
    return result;
}

// meshAssets[0] is LOD0; the rest become Mesh.lodMeshes (LOD1..n).
std::string PrefabText(const std::string& name, const std::vector<std::string>& meshAssets,
    const std::string& material)
{
    std::ostringstream xml;
    xml << "<?xml version='1.0' encoding='utf-8'?>\n"
        << "<Prefab xmlns:components=\"urn:engine-components\" version=\"1\" type=\"prefab\">\n"
        << "  <object name=\"" << name << "\" enabled=\"true\">\n"
        << "    <components>\n"
        << "      <components:Mesh Mesh.file=\"" << meshAssets.front()
        << "\" Mesh.foliageWindStrength=\"0\" Mesh.useLightProbes=\"false\" Mesh.gpuLodSelection=\"false\"";
    if (meshAssets.size() < 2) xml << " />\n";
    else
    {
        xml << ">\n        <Mesh.lodMeshes>\n";
        for (size_t lod = 1; lod < meshAssets.size(); ++lod)
            xml << "          <item>" << meshAssets[lod] << "</item>\n";
        xml << "        </Mesh.lodMeshes>\n      </components:Mesh>\n";
    }
    xml << "      <components:Material Material.materialAsset=\"" << material << "\" />\n"
        << "    </components>\n"
        << "    <children />\n"
        << "    <transform>\n"
        << "      <Transform.position _arrayEncoding=\"spaceSeparatedNumbers\">0 0 0</Transform.position>\n"
        << "      <Transform.rotation _arrayEncoding=\"spaceSeparatedNumbers\">0 0 0</Transform.rotation>\n"
        << "      <Transform.scale _arrayEncoding=\"spaceSeparatedNumbers\">1 1 1</Transform.scale>\n"
        << "    </transform>\n"
        << "  </object>\n"
        << "</Prefab>\n";
    return xml.str();
}

bool Cook(Geometry geometry, const fs::path& meshDir, const std::string& name,
    const std::vector<int>& lods, float scale, float sink, float maxError,
    const std::string& meshAsset,
    const fs::path& prefabDir, const std::string& material)
{
    FillMissingNormals(geometry.corners);
    float minimum[3], maximum[3];
    Bounds(geometry.corners, minimum, maximum);
    // Ground pivot: centred in X/Z, base lowered by `sink` of the height.
    const float height = maximum[1] - minimum[1];
    const double diagonal = scale * std::sqrt(double(maximum[0] - minimum[0]) * (maximum[0] - minimum[0]) +
        double(height) * height + double(maximum[2] - minimum[2]) * (maximum[2] - minimum[2]));
    const float pivot[3]{(minimum[0] + maximum[0]) * .5f, minimum[1] + sink * height,
        (minimum[2] + maximum[2]) * .5f};
    for (auto& v : geometry.corners)
        for (int axis = 0; axis < 3; ++axis)
            v.position[axis] = (v.position[axis] - pivot[axis]) * scale;

    std::vector<SourceVertex> vertices;
    std::vector<uint32_t> indices;
    Weld(geometry.corners, vertices, indices);
    const size_t vertexCount = vertices.size();

    std::printf("%s: source %zu triangles, %zu vertices, %.2f x %.2f x %.2f m\n", name.c_str(),
        indices.size() / 3, vertexCount, (maximum[0] - minimum[0]) * scale, height * scale,
        (maximum[2] - minimum[2]) * scale);
    std::vector<std::string> meshAssets;
    for (size_t lod = 0; lod < lods.size(); ++lod)
    {
        const size_t target = size_t(std::max(lods[lod], 4));
        std::vector<uint32_t> simplified = target * 3 < indices.size()
            ? Simplifier(vertices, indices).Run(target, maxError * diagonal * double(1u << lod))
            : indices;
        const size_t count = simplified.size();
        std::vector<SourceVertex> compact;
        Compact(vertices, simplified, compact);
        const size_t used = compact.size();
        const auto engineVertices = BuildEngineVertices(compact, simplified);
        const std::string lodName = name + "_LOD" + std::to_string(lod);
        const fs::path meshPath = meshDir / (lodName + ".mesh");
        if (!Mesh::SaveNativeFile(meshPath.string(), engineVertices, simplified))
        {
            std::fprintf(stderr, "PropCook: cannot write %s\n", meshPath.string().c_str());
            return false;
        }
        std::printf("  LOD%zu: %zu triangles, %zu vertices -> %s\n", lod, count / 3,
            used, meshPath.string().c_str());
        meshAssets.push_back(meshAsset + "/" + lodName + ".mesh");
    }
    if (!prefabDir.empty() && !meshAssets.empty())
    {
        std::ofstream prefab(prefabDir / (name + ".prefab"), std::ios::binary | std::ios::trunc);
        prefab << PrefabText(name, meshAssets, material);
        if (!prefab) return false;
    }
    return true;
}
}

int main(int argc, char** argv)
{
    fs::path source, meshDir, prefabDir;
    std::string name, meshAsset, material;
    std::vector<int> lods{3000, 900, 250}, only;
    bool list = false, split = false;
    float scale = 1.f, sink = .04f, maxError = .01f;
    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        const auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : std::string(); };
        if (arg == "--list") list = true;
        else if (arg == "--split") split = true;
        else if (arg == "--only") only = ParseList(next());
        else if (arg == "--name") name = next();
        else if (arg == "--lods") lods = ParseList(next());
        else if (arg == "--scale") scale = std::stof(next());
        else if (arg == "--sink") sink = std::stof(next());
        else if (arg == "--max-error") maxError = std::stof(next());
        else if (arg == "--mesh-asset") meshAsset = next();
        else if (arg == "--prefab-dir") prefabDir = next();
        else if (arg == "--material") material = next();
        else if (!arg.empty() && arg[0] == '-') return Fail("unknown option " + arg);
        else if (source.empty()) source = arg;
        else if (meshDir.empty()) meshDir = arg;
        else return Fail("too many arguments");
    }
    if (source.empty() || (!list && meshDir.empty()))
        return Fail("usage: PropCook <source.fbx|obj> <outMeshDir> [--list] [--split] [--only i,j] "
            "[--name prefix] [--lods 3000,900,250] [--scale s] [--sink f] [--max-error f] "
            "[--mesh-asset Assets/...] [--prefab-dir dir] [--material asset]");
    if (lods.empty() || scale <= 0.f) return Fail("--lods needs at least one target and --scale must be positive");
    if (!prefabDir.empty() && (meshAsset.empty() || material.empty()))
        return Fail("--prefab-dir needs --mesh-asset and --material");
    if (name.empty()) name = source.stem().string();

    ufbx_load_opts options{};
    options.target_axes = ufbx_axes_right_handed_y_up;
    options.target_unit_meters = 1.f;
    options.space_conversion = UFBX_SPACE_CONVERSION_MODIFY_GEOMETRY;
    ufbx_error error{};
    ufbx_scene* scene = ufbx_load_file(source.string().c_str(), &options, &error);
    if (!scene)
        return Fail("cannot load " + source.string() + ": " +
            std::string(error.description.data, error.description.length));
    struct Free { ufbx_scene* s; ~Free() { ufbx_free_scene(s); } } release{scene};

    std::vector<const ufbx_node*> nodes;
    for (size_t i = 0; i < scene->nodes.count; ++i)
        if (scene->nodes.data[i]->mesh && scene->nodes.data[i]->mesh->num_triangles)
            nodes.push_back(scene->nodes.data[i]);
    if (nodes.empty()) return Fail("no meshes in " + source.string());

    if (list)
    {
        for (size_t i = 0; i < nodes.size(); ++i)
        {
            Geometry geometry;
            AppendNode(*nodes[i], geometry);
            float minimum[3], maximum[3];
            Bounds(geometry.corners, minimum, maximum);
            std::printf("%3zu %-28s %8zu tris  %.2f x %.2f x %.2f m  at (%.1f, %.1f, %.1f)\n", i,
                nodes[i]->name.data, geometry.corners.size() / 3,
                maximum[0] - minimum[0], maximum[1] - minimum[1], maximum[2] - minimum[2],
                (minimum[0] + maximum[0]) * .5f, minimum[1], (minimum[2] + maximum[2]) * .5f);
        }
        return 0;
    }

    fs::create_directories(meshDir);
    if (!prefabDir.empty()) fs::create_directories(prefabDir);
    if (!split)
    {
        Geometry geometry{name, {}};
        for (const auto* node : nodes) AppendNode(*node, geometry);
        return Cook(std::move(geometry), meshDir, name, lods, scale, sink, maxError, meshAsset,
            prefabDir, material) ? 0 : 1;
    }
    int written = 0;
    for (size_t i = 0; i < nodes.size(); ++i)
    {
        if (!only.empty() && std::find(only.begin(), only.end(), int(i)) == only.end()) continue;
        Geometry geometry{nodes[i]->name.data, {}};
        AppendNode(*nodes[i], geometry);
        char suffix[16];
        std::snprintf(suffix, sizeof(suffix), "_%02d", written + 1);
        if (!Cook(std::move(geometry), meshDir, name + suffix, lods, scale, sink, maxError, meshAsset,
                prefabDir, material)) return 1;
        ++written;
    }
    return written ? 0 : Fail("--only matched no nodes");
}

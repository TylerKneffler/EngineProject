#include "MeshEditGeometry.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <set>
#include <map>
#include <numeric>
#include <unordered_set>
#include <glm/gtc/matrix_transform.hpp>
#include <utility>
#include <glm/glm.hpp>

namespace Engine::Editor
{
namespace
{
using Edge = std::pair<uint32_t, uint32_t>;
Edge SortedEdge(uint32_t a, uint32_t b)
{ return std::minmax(a, b); }

std::vector<Edge> Edges(const std::vector<uint32_t>& indices)
{
    std::set<Edge> unique;
    for (size_t i = 0; i + 2 < indices.size(); i += 3)
    {
        unique.insert(SortedEdge(indices[i], indices[i + 1]));
        unique.insert(SortedEdge(indices[i + 1], indices[i + 2]));
        unique.insert(SortedEdge(indices[i + 2], indices[i]));
    }
    return { unique.begin(), unique.end() };
}

glm::vec3 Position(const MeshEditGeometry::Vertex& v)
{ return { v.pos[0], v.pos[1], v.pos[2] }; }

void SetPosition(MeshEditGeometry::Vertex& v, glm::vec3 p)
{ v.pos[0] = p.x; v.pos[1] = p.y; v.pos[2] = p.z; }

MeshEditGeometry::Vertex Blend(const MeshEditGeometry::Vertex& a,
    const MeshEditGeometry::Vertex& b, float fraction)
{
    MeshEditGeometry::Vertex result = a;
    const auto blend = [fraction](float* destination, const float* other,
        size_t count)
    {
        for (size_t i = 0; i < count; ++i)
            destination[i] += (other[i] - destination[i]) * fraction;
    };
    blend(result.pos, b.pos, 3);
    blend(result.normal, b.normal, 3);
    blend(result.uv, b.uv, 2);
    blend(result.tangent, b.tangent, 4);
    blend(result.uv1, b.uv1, 2);
    blend(result.color, b.color, 4);
    const glm::vec3 normal(result.normal[0], result.normal[1], result.normal[2]);
    if (glm::dot(normal, normal) > 1e-12f)
    {
        const glm::vec3 unit = glm::normalize(normal);
        result.normal[0] = unit.x;
        result.normal[1] = unit.y;
        result.normal[2] = unit.z;
    }
    // A midpoint can have a different joint palette on either endpoint.
    // Preserve a valid influence set until weight painting handles remapping.
    return result;
}
}

MeshEditGeometry MeshEditGeometry::FromMesh(const Engine::Components::Mesh& mesh)
{
    MeshEditGeometry result{ mesh.GetVertices(), mesh.GetIndices() };
    if (result.indices.empty())
    {
        result.indices.reserve(result.vertices.size());
        for (uint32_t i = 0; i < result.vertices.size(); ++i)
            result.indices.push_back(i);
    }
    return result;
}

uint32_t MeshEditGeometry::EdgeCount() const
{ return static_cast<uint32_t>(Edges(indices).size()); }

std::vector<std::pair<uint32_t, uint32_t>> MeshEditGeometry::EdgeList() const
{ return Edges(indices); }

std::vector<std::pair<uint32_t, uint32_t>>
MeshEditGeometry::EdgeListFromIndices(const std::vector<uint32_t>& indices)
{ return Edges(indices); }

std::pair<uint32_t, uint32_t> MeshEditGeometry::EdgeAt(uint32_t edge) const
{
    const auto edges = Edges(indices);
    return edge < edges.size() ? edges[edge] : Edge{};
}

std::vector<uint32_t> MeshEditGeometry::SelectionVertices(int mode,
    const std::vector<uint32_t>& selected) const
{
    std::vector<uint32_t> ids;
    ids.reserve(selected.size() * (mode == 2 ? 3u : 2u));
    std::unordered_set<uint32_t> seen;
    const auto add = [&](uint32_t id)
    { if (seen.insert(id).second) ids.push_back(id); };
    const auto edges = mode == 1 ? Edges(indices) : std::vector<Edge>{};
    for (uint32_t id : selected)
    {
        if (mode == 0 && id < vertices.size()) add(id);
        else if (mode == 1 && id < edges.size())
        { add(edges[id].first); add(edges[id].second); }
        else if (mode == 2 && id < FaceCount())
            for (int corner = 0; corner < 3; ++corner)
                add(indices[id * 3 + corner]);
    }
    return ids;
}

bool MeshEditGeometry::TransformSelection(int mode,
    const std::vector<uint32_t>& selected,
    const std::array<float, 3>& translation,
    const std::array<float, 3>& rotation,
    const std::array<float, 3>& scale, int pivotMode,
    const std::array<float, 3>& customPivot, float snapStep,
    float rotationSnap, float scaleSnap)
{
    const auto ids = SelectionVertices(mode, selected);
    if (ids.empty()) return false;
    for (float value : translation) if (!std::isfinite(value)) return false;
    for (float value : rotation) if (!std::isfinite(value)) return false;
    for (float value : scale) if (!std::isfinite(value) || value <= 0.f) return false;
    glm::vec3 pivot{};
    if (pivotMode == 3) pivot = { customPivot[0], customPivot[1], customPivot[2] };
    else if (pivotMode == 0)
    {
        for (uint32_t id : ids) pivot += Position(vertices[id]);
        pivot /= static_cast<float>(ids.size());
    }
    else if (pivotMode == 1)
    {
        glm::vec3 low(INFINITY), high(-INFINITY);
        for (uint32_t id : ids)
        { low = glm::min(low, Position(vertices[id])); high = glm::max(high, Position(vertices[id])); }
        pivot = (low + high) * .5f;
    }
    glm::vec3 move(translation[0], translation[1], translation[2]);
    if (std::isfinite(snapStep) && snapStep > 0.f)
        for (int axis = 0; axis < 3; ++axis)
            move[axis] = std::round(move[axis] / snapStep) * snapStep;
    glm::mat4 turn(1.f);
    for (int axis = 0; axis < 3; ++axis)
    {
        glm::vec3 basis{}; basis[axis] = 1.f;
        float angle = rotation[axis];
        if (std::isfinite(rotationSnap) && rotationSnap > 0.f)
            angle = std::round(angle / rotationSnap) * rotationSnap;
        turn = glm::rotate(turn, glm::radians(angle), basis);
    }
    glm::vec3 sizing(scale[0], scale[1], scale[2]);
    if (std::isfinite(scaleSnap) && scaleSnap > 0.f)
        for (int axis = 0; axis < 3; ++axis)
            sizing[axis] = 1.f + std::round((sizing[axis] - 1.f) /
                scaleSnap) * scaleSnap;
    if (sizing.x <= 0.f || sizing.y <= 0.f || sizing.z <= 0.f) return false;
    for (uint32_t id : ids)
    {
        glm::vec3 p = Position(vertices[id]) - pivot;
        p *= sizing;
        SetPosition(vertices[id], pivot + glm::vec3(turn * glm::vec4(p, 1.f)) + move);
    }
    return true;
}

bool MeshEditGeometry::DeleteSelection(int mode, const std::vector<uint32_t>& selected)
{
    if (selected.empty()) return false;
    const auto edges = mode == 1 ? Edges(indices) : std::vector<Edge>{};
    std::set<uint32_t> ids(selected.begin(), selected.end());
    std::set<Edge> selectedEdgeKeys;
    if (mode == 1)
        for (uint32_t id : ids)
            if (id < edges.size()) selectedEdgeKeys.insert(edges[id]);
    std::vector<uint32_t> output;
    for (uint32_t face = 0; face < FaceCount(); ++face)
    {
        const uint32_t* t = indices.data() + face * 3;
        bool erase = mode == 2 && ids.count(face);
        if (mode == 0)
            erase = ids.count(t[0]) || ids.count(t[1]) || ids.count(t[2]);
        if (mode == 1)
            erase = selectedEdgeKeys.count(SortedEdge(t[0], t[1])) ||
                selectedEdgeKeys.count(SortedEdge(t[1], t[2])) ||
                selectedEdgeKeys.count(SortedEdge(t[2], t[0]));
        if (!erase) output.insert(output.end(), t, t + 3);
    }
    if (output.empty() || output.size() == indices.size()) return false;
    indices = std::move(output);
    return true;
}

bool MeshEditGeometry::DuplicateFaces(const std::vector<uint32_t>& faces,
    const std::array<float, 3>& offset)
{
    if (faces.empty()) return false;
    for (float value : offset) if (!std::isfinite(value)) return false;
    const uint32_t count = FaceCount();
    for (uint32_t face : faces) if (face >= count) return false;
    std::map<uint32_t, uint32_t> copies;
    for (uint32_t face : std::set<uint32_t>(faces.begin(), faces.end()))
        for (int corner = 0; corner < 3; ++corner)
        {
            uint32_t source = indices[face * 3 + corner];
            if (!copies.count(source))
            {
                copies[source] = static_cast<uint32_t>(vertices.size());
                vertices.push_back(vertices[source]);
                SetPosition(vertices.back(), Position(vertices.back()) +
                    glm::vec3(offset[0], offset[1], offset[2]));
            }
            indices.push_back(copies[source]);
        }
    return true;
}

bool MeshEditGeometry::WeldVertices(const std::vector<uint32_t>& selected)
{
    std::set<uint32_t> ids(selected.begin(), selected.end());
    if (ids.size() < 2 || *ids.rbegin() >= vertices.size()) return false;
    glm::vec3 center{};
    for (uint32_t id : ids) center += Position(vertices[id]);
    center /= static_cast<float>(ids.size());
    const uint32_t target = *ids.begin();
    std::vector<uint32_t> output;
    for (size_t i = 0; i < indices.size(); i += 3)
    {
        std::array<uint32_t, 3> t{ indices[i], indices[i + 1], indices[i + 2] };
        for (uint32_t& id : t) if (ids.count(id)) id = target;
        if (t[0] != t[1] && t[1] != t[2] && t[2] != t[0])
            output.insert(output.end(), t.begin(), t.end());
    }
    if (output.empty()) return false;
    SetPosition(vertices[target], center);
    indices = std::move(output);
    return true;
}

bool MeshEditGeometry::FillBoundary(const std::vector<uint32_t>& selectedEdges)
{
    const auto edges = Edges(indices);
    std::map<Edge, int> incidence;
    for (size_t i = 0; i < indices.size(); i += 3)
        for (int j = 0; j < 3; ++j)
            ++incidence[SortedEdge(indices[i + j], indices[i + (j + 1) % 3])];
    std::map<uint32_t, std::vector<uint32_t>> adjacency;
    for (uint32_t id : std::set<uint32_t>(selectedEdges.begin(), selectedEdges.end()))
    {
        if (id >= edges.size() || incidence[edges[id]] != 1) return false;
        const auto [a, b] = edges[id];
        adjacency[a].push_back(b); adjacency[b].push_back(a);
    }
    if (adjacency.size() < 3) return false;
    for (const auto& [id, neighbors] : adjacency)
        if (neighbors.size() != 2) return false;
    std::vector<uint32_t> loop{ adjacency.begin()->first };
    uint32_t previous = UINT32_MAX, current = loop.front();
    do
    {
        const auto& neighbors = adjacency[current];
        uint32_t next = neighbors[0] == previous ? neighbors[1] : neighbors[0];
        previous = current; current = next;
        if (current != loop.front()) loop.push_back(current);
        if (loop.size() > adjacency.size()) return false;
    } while (current != loop.front());
    if (loop.size() != adjacency.size()) return false;
    // Follow the winding opposite to the existing face at the first edge.
    for (size_t i = 0; i < indices.size(); i += 3)
        for (int j = 0; j < 3; ++j)
            if (indices[i + j] == loop[0] && indices[i + (j + 1) % 3] == loop[1])
            { std::reverse(loop.begin(), loop.end()); i = indices.size(); break; }
    const glm::vec3 origin = Position(vertices[loop[0]]);
    glm::vec3 normal{};
    for (size_t i = 1; i + 1 < loop.size(); ++i)
        normal += glm::cross(Position(vertices[loop[i]]) - origin,
            Position(vertices[loop[i + 1]]) - origin);
    if (glm::length(normal) < 1e-6f) return false;
    normal = glm::normalize(normal);
    for (uint32_t id : loop)
        if (std::abs(glm::dot(Position(vertices[id]) - origin, normal)) > 1e-3f)
            return false;
    for (size_t i = 0; i < loop.size(); ++i)
    {
        const glm::vec3 a = Position(vertices[loop[i]]);
        const glm::vec3 b = Position(vertices[loop[(i + 1) % loop.size()]]);
        const glm::vec3 c = Position(vertices[loop[(i + 2) % loop.size()]]);
        if (glm::dot(glm::cross(b - a, c - b), normal) <= 1e-6f) return false;
    }
    for (size_t i = 1; i + 1 < loop.size(); ++i)
        indices.insert(indices.end(), { loop[0], loop[i], loop[i + 1] });
    return true;
}

bool MeshEditGeometry::BridgeBoundaries(const std::vector<uint32_t>& selectedEdges)
{
    const auto edges = Edges(indices);
    std::map<Edge, int> incidence;
    for (size_t i = 0; i < indices.size(); i += 3)
        for (int j = 0; j < 3; ++j)
            ++incidence[SortedEdge(indices[i + j], indices[i + (j + 1) % 3])];
    std::map<uint32_t, std::vector<uint32_t>> adjacency;
    for (uint32_t id : std::set<uint32_t>(selectedEdges.begin(), selectedEdges.end()))
    {
        if (id >= edges.size() || incidence[edges[id]] != 1) return false;
        auto [a, b] = edges[id];
        adjacency[a].push_back(b); adjacency[b].push_back(a);
    }
    for (const auto& [id, neighbors] : adjacency)
        if (neighbors.size() != 2) return false;
    std::vector<std::vector<uint32_t>> loops;
    std::set<uint32_t> visited;
    for (const auto& [start, neighbors] : adjacency)
    {
        if (visited.count(start)) continue;
        std::vector<uint32_t> loop{ start };
        uint32_t previous = UINT32_MAX, current = start;
        do
        {
            visited.insert(current);
            const auto& adjacent = adjacency[current];
            uint32_t next = adjacent[0] == previous ? adjacent[1] : adjacent[0];
            previous = current; current = next;
            if (current != start) loop.push_back(current);
            if (loop.size() > adjacency.size()) return false;
        } while (current != start);
        loops.push_back(std::move(loop));
    }
    if (loops.size() != 2 || loops[0].size() < 3 ||
        loops[0].size() != loops[1].size()) return false;
    auto& a = loops[0]; auto& b = loops[1];
    const auto followsFace = [&](const std::vector<uint32_t>& loop)
    {
        for (size_t i = 0; i < indices.size(); i += 3)
            for (int j = 0; j < 3; ++j)
                if (indices[i + j] == loop[0] &&
                    indices[i + (j + 1) % 3] == loop[1]) return true;
        return false;
    };
    if (followsFace(a)) std::reverse(a.begin(), a.end());
    if (!followsFace(b)) std::reverse(b.begin(), b.end());
    float best = INFINITY; size_t offset = 0;
    for (size_t shift = 0; shift < b.size(); ++shift)
        {
            float score = 0.f;
            for (size_t i = 0; i < a.size(); ++i)
            {
                size_t j = (shift + i) % b.size();
                glm::vec3 delta = Position(vertices[a[i]]) - Position(vertices[b[j]]);
                score += glm::dot(delta, delta);
            }
            if (score < best) { best = score; offset = shift; }
        }
    std::vector<uint32_t> aligned;
    for (size_t i = 0; i < b.size(); ++i)
        aligned.push_back(b[(offset + i) % b.size()]);
    for (size_t i = 0; i < a.size(); ++i)
    {
        size_t next = (i + 1) % a.size();
        indices.insert(indices.end(), { a[i], a[next], aligned[next],
            a[i], aligned[next], aligned[i] });
    }
    return true;
}

bool MeshEditGeometry::BevelEdge(uint32_t edge, float width)
{
    const auto edges = Edges(indices);
    if (edge >= edges.size() || !std::isfinite(width) || width <= 0.f || width >= .5f)
        return false;
    const auto [a, b] = edges[edge];
    std::vector<uint32_t> incident;
    for (uint32_t face = 0; face < FaceCount(); ++face)
    {
        const uint32_t* t = indices.data() + face * 3;
        if (std::find(t, t + 3, a) != t + 3 &&
            std::find(t, t + 3, b) != t + 3) incident.push_back(face);
    }
    if (incident.size() != 2) return false;
    std::array<uint32_t, 4> inset{};
    for (size_t side = 0; side < 2; ++side)
    {
        const uint32_t* t = indices.data() + incident[side] * 3;
        uint32_t c = *std::find_if(t, t + 3,
            [a, b](uint32_t id) { return id != a && id != b; });
        if (glm::length(glm::cross(Position(vertices[b]) - Position(vertices[a]),
            Position(vertices[c]) - Position(vertices[a]))) < 1e-6f) return false;
        inset[side * 2] = static_cast<uint32_t>(vertices.size());
        vertices.push_back(Blend(vertices[a], vertices[c], width));
        inset[side * 2 + 1] = static_cast<uint32_t>(vertices.size());
        vertices.push_back(Blend(vertices[b], vertices[c], width));
        for (int j = 0; j < 3; ++j)
            if (indices[incident[side] * 3 + j] == a)
                indices[incident[side] * 3 + j] = inset[side * 2];
            else if (indices[incident[side] * 3 + j] == b)
                indices[incident[side] * 3 + j] = inset[side * 2 + 1];
    }
    indices.insert(indices.end(), { inset[0], inset[1], inset[3],
        inset[0], inset[3], inset[2] });
    return true;
}

bool MeshEditGeometry::LoopCut(uint32_t edge)
{
    const auto edges = Edges(indices);
    if (edge >= edges.size()) return false;
    const auto target = edges[edge];
    std::map<Edge, std::vector<uint32_t>> edgeFaces;
    for (uint32_t face = 0; face < FaceCount(); ++face)
        for (int j = 0; j < 3; ++j)
            edgeFaces[SortedEdge(indices[face * 3 + j],
                indices[face * 3 + (j + 1) % 3])].push_back(face);
    const uint32_t incidentFaces = static_cast<uint32_t>(
        edgeFaces[target].size());
    if (incidentFaces == 2)
    {
        // Interior strip edges are resolved from a boundary end. Accept only
        // when the resulting cut actually inserts a midpoint on this edge.
        const glm::vec3 center = (Position(vertices[target.first]) +
            Position(vertices[target.second])) * .5f;
        const uint32_t* first = indices.data() + edgeFaces[target][0] * 3;
        const uint32_t other = *std::find_if(first, first + 3,
            [&](uint32_t id) { return id != target.first &&
                id != target.second; });
        const glm::vec3 edgeDirection = glm::normalize(
            Position(vertices[target.second]) - Position(vertices[target.first]));
        const glm::vec3 planeNormal = glm::normalize(glm::cross(
            Position(vertices[target.second]) - Position(vertices[target.first]),
            Position(vertices[other]) - Position(vertices[target.first])));
        const float targetLength = glm::length(
            Position(vertices[target.second]) - Position(vertices[target.first]));
        if (!std::isfinite(targetLength) || targetLength < 1e-6f ||
            !std::isfinite(planeNormal.x)) return false;
        for (uint32_t candidate = 0; candidate < edges.size(); ++candidate)
        {
            if (edgeFaces[edges[candidate]].size() != 1) continue;
            const auto [a, b] = edges[candidate];
            const glm::vec3 direction = Position(vertices[b]) -
                Position(vertices[a]);
            const float length = glm::length(direction);
            if (length < 1e-6f ||
                std::abs(glm::dot(direction / length, edgeDirection)) < .95f)
                continue;
            const glm::vec3 offset =
                (Position(vertices[a]) + Position(vertices[b])) * .5f - center;
            if (std::abs(glm::dot(offset, planeNormal)) > 1e-3f ||
                std::abs(glm::dot(offset, edgeDirection)) >
                    1.5f * std::max(targetLength, length)) continue;
            MeshEditGeometry attempt = *this;
            if (!attempt.LoopCut(candidate)) continue;
            for (size_t id = vertices.size(); id < attempt.vertices.size(); ++id)
                if (glm::length(Position(attempt.vertices[id]) - center) < 1e-5f)
                { *this = std::move(attempt); return true; }
        }
        return false;
    }
    if (incidentFaces != 1) return false;
    struct Quad { uint32_t u, v, right, left, first, second; };
    std::vector<Quad> strip;
    std::set<uint32_t> used;
    Edge front = edges[edge];
    for (;;)
    {
        const auto [a, b] = front;
        uint32_t face = UINT32_MAX, opposite = UINT32_MAX;
        for (uint32_t i : edgeFaces[front])
        {
            if (used.count(i)) continue;
            const uint32_t* t = indices.data() + i * 3;
            if (std::find(t, t + 3, a) == t + 3 ||
                std::find(t, t + 3, b) == t + 3) continue;
            if (face != UINT32_MAX) return false;
            face = i;
            opposite = *std::find_if(t, t + 3,
                [a, b](uint32_t id) { return id != a && id != b; });
        }
        if (face == UINT32_MAX) break;
        uint32_t secondFace = UINT32_MAX, fourth = UINT32_MAX,
            sharedEnd = UINT32_MAX;
        float bestParallel = -1.f;
        std::set<uint32_t> adjacent;
        for (uint32_t id : edgeFaces[SortedEdge(a, opposite)])
            adjacent.insert(id);
        for (uint32_t id : edgeFaces[SortedEdge(b, opposite)])
            adjacent.insert(id);
        for (uint32_t i : adjacent)
        {
            if (i == face || used.count(i)) continue;
            const uint32_t* t = indices.data() + i * 3;
            if (std::find(t, t + 3, opposite) == t + 3) continue;
            const bool hasA = std::find(t, t + 3, a) != t + 3;
            const bool hasB = std::find(t, t + 3, b) != t + 3;
            if (hasA == hasB) continue;
            uint32_t candidateFourth = *std::find_if(t, t + 3,
                [a, b, opposite](uint32_t id)
                { return id != a && id != b && id != opposite; });
            const glm::vec3 incoming = Position(vertices[b]) - Position(vertices[a]);
            const glm::vec3 outgoing = Position(vertices[opposite]) -
                Position(vertices[candidateFourth]);
            if (glm::length(incoming) < 1e-6f ||
                glm::length(outgoing) < 1e-6f) continue;
            const float parallel = std::abs(glm::dot(glm::normalize(incoming),
                glm::normalize(outgoing)));
            const glm::vec3 centerOffset =
                (Position(vertices[opposite]) +
                    Position(vertices[candidateFourth]) -
                    Position(vertices[a]) - Position(vertices[b])) * .5f;
            const float drift = std::abs(glm::dot(centerOffset,
                glm::normalize(incoming))) / glm::length(incoming);
            const float score = parallel - .25f * drift;
            if (score > bestParallel + 1e-4f)
            {
                bestParallel = score;
                secondFace = i;
                sharedEnd = hasA ? a : b;
                fourth = candidateFourth;
            }
            else if (std::abs(score - bestParallel) <= 1e-4f)
                return false;
        }
        if (secondFace == UINT32_MAX || fourth == UINT32_MAX) return false;
        const uint32_t* t = indices.data() + face * 3;
        uint32_t u = a, v = b;
        for (int j = 0; j < 3; ++j)
            if (t[j] == b && t[(j + 1) % 3] == a)
            { u = b; v = a; break; }
        const uint32_t* other = indices.data() + secondFace * 3;
        bool windingValid = false;
        for (int j = 0; j < 3; ++j)
            if (sharedEnd == u && other[j] == u &&
                other[(j + 1) % 3] == opposite) windingValid = true;
            else if (sharedEnd == v && other[j] == opposite &&
                other[(j + 1) % 3] == v) windingValid = true;
        if (!windingValid) return false;
        uint32_t left = sharedEnd == u ? fourth : opposite;
        uint32_t right = sharedEnd == u ? opposite : fourth;
        glm::vec3 normal = glm::cross(Position(vertices[v]) - Position(vertices[u]),
            Position(vertices[opposite]) - Position(vertices[u]));
        if (glm::length(normal) < 1e-6f ||
            std::abs(glm::dot(glm::normalize(normal), Position(vertices[fourth]) -
                Position(vertices[u]))) > 1e-3f) return false;
        const std::array<uint32_t, 4> boundary{ u, v, right, left };
        for (int j = 0; j < 4; ++j)
        {
            glm::vec3 p = Position(vertices[boundary[j]]);
            glm::vec3 q = Position(vertices[boundary[(j + 1) % 4]]);
            glm::vec3 r = Position(vertices[boundary[(j + 2) % 4]]);
            if (glm::dot(glm::cross(q - p, r - q), normal) <= 1e-6f)
                return false;
        }
        strip.push_back({ u, v, right, left, face, secondFace });
        used.insert(face); used.insert(secondFace);
        front = SortedEdge(left, right);
        if (strip.size() > FaceCount() / 2) return false;
    }
    if (strip.empty()) return false;
    std::map<Edge, uint32_t> midpoints;
    const auto midpoint = [&](uint32_t a, uint32_t b)
    {
        Edge key = SortedEdge(a, b);
        if (auto found = midpoints.find(key); found != midpoints.end())
            return found->second;
        uint32_t id = static_cast<uint32_t>(vertices.size());
        vertices.push_back(Blend(vertices[a], vertices[b], .5f));
        midpoints[key] = id;
        return id;
    };
    for (const Quad& quad : strip)
    {
        uint32_t m = midpoint(quad.u, quad.v);
        uint32_t q = midpoint(quad.left, quad.right);
        indices[quad.first * 3] = quad.u;
        indices[quad.first * 3 + 1] = m;
        indices[quad.first * 3 + 2] = q;
        indices[quad.second * 3] = quad.u;
        indices[quad.second * 3 + 1] = q;
        indices[quad.second * 3 + 2] = quad.left;
        indices.insert(indices.end(), { m, quad.v, quad.right,
            m, quad.right, q });
    }
    return true;
}

bool MeshEditGeometry::ExtrudeFace(uint32_t face, float distance)
{
    if (face >= FaceCount() || !std::isfinite(distance) || distance == 0.f)
        return false;
    const size_t offset = static_cast<size_t>(face) * 3;
    const std::array<uint32_t, 3> base{
        indices[offset], indices[offset + 1], indices[offset + 2] };
    const glm::vec3 a = Position(vertices[base[0]]);
    const glm::vec3 b = Position(vertices[base[1]]);
    const glm::vec3 c = Position(vertices[base[2]]);
    const glm::vec3 cross = glm::cross(b - a, c - a);
    if (glm::dot(cross, cross) < 1e-12f) return false;
    const glm::vec3 translation = glm::normalize(cross) * distance;
    std::array<uint32_t, 3> top{};
    for (size_t i = 0; i < 3; ++i)
    {
        top[i] = static_cast<uint32_t>(vertices.size());
        vertices.push_back(vertices[base[i]]);
        SetPosition(vertices.back(), Position(vertices.back()) + translation);
    }
    indices[offset] = top[0];
    indices[offset + 1] = top[1];
    indices[offset + 2] = top[2];
    for (size_t i = 0; i < 3; ++i)
    {
        const size_t next = (i + 1) % 3;
        const glm::vec3 edge = Position(vertices[base[next]]) -
            Position(vertices[base[i]]);
        const glm::vec3 wallNormal = glm::normalize(
            glm::cross(edge, translation));
        std::array<uint32_t, 4> wall{};
        const std::array<uint32_t, 4> source{
            base[i], base[next], top[next], top[i] };
        for (size_t corner = 0; corner < 4; ++corner)
        {
            wall[corner] = static_cast<uint32_t>(vertices.size());
            vertices.push_back(vertices[source[corner]]);
            vertices.back().normal[0] = wallNormal.x;
            vertices.back().normal[1] = wallNormal.y;
            vertices.back().normal[2] = wallNormal.z;
        }
        indices.insert(indices.end(), { wall[0], wall[1], wall[2],
            wall[0], wall[2], wall[3] });
    }
    return true;
}

bool MeshEditGeometry::InsetFace(uint32_t face, float amount)
{
    if (face >= FaceCount() || !std::isfinite(amount) ||
        amount <= 0.f || amount >= 1.f) return false;
    const size_t offset = static_cast<size_t>(face) * 3;
    const std::array<uint32_t, 3> base{
        indices[offset], indices[offset + 1], indices[offset + 2] };
    std::array<uint32_t, 3> inner{};
    for (size_t i = 0; i < 3; ++i)
    {
        const auto& a = vertices[base[i]];
        const auto& b = vertices[base[(i + 1) % 3]];
        const auto& c = vertices[base[(i + 2) % 3]];
        Vertex mid = Blend(Blend(a, b, .5f), c, 1.f / 3.f);
        inner[i] = static_cast<uint32_t>(vertices.size());
        vertices.push_back(Blend(a, mid, amount));
    }
    indices[offset] = inner[0];
    indices[offset + 1] = inner[1];
    indices[offset + 2] = inner[2];
    for (size_t i = 0; i < 3; ++i)
    {
        const size_t next = (i + 1) % 3;
        indices.insert(indices.end(), { base[i], base[next], inner[next],
            base[i], inner[next], inner[i] });
    }
    return true;
}

bool MeshEditGeometry::SplitEdge(uint32_t edge)
{
    const auto edges = Edges(indices);
    if (edge >= edges.size()) return false;
    const auto [a, b] = edges[edge];
    const uint32_t midpoint = static_cast<uint32_t>(vertices.size());
    vertices.push_back(Blend(vertices[a], vertices[b], .5f));
    std::vector<uint32_t> output;
    output.reserve(indices.size() + 6);
    for (size_t i = 0; i + 2 < indices.size(); i += 3)
    {
        const std::array<uint32_t, 3> triangle{
            indices[i], indices[i + 1], indices[i + 2] };
        bool split = false;
        for (size_t j = 0; j < 3; ++j)
        {
            const uint32_t first = triangle[j];
            const uint32_t second = triangle[(j + 1) % 3];
            if (SortedEdge(first, second) != edges[edge]) continue;
            const uint32_t third = triangle[(j + 2) % 3];
            output.insert(output.end(), { first, midpoint, third,
                midpoint, second, third });
            split = true;
            break;
        }
        if (!split) output.insert(output.end(), triangle.begin(), triangle.end());
    }
    indices = std::move(output);
    return true;
}

bool MeshEditGeometry::DeleteFace(uint32_t face)
{
    if (face >= FaceCount() || FaceCount() <= 1) return false;
    indices.erase(indices.begin() + static_cast<size_t>(face) * 3,
        indices.begin() + static_cast<size_t>(face) * 3 + 3);
    return true;
}
}

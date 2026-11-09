#include "Mesh.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace Engine::Components
{
namespace
{
float SignedDistanceToPlane(const glm::vec3& point,
    const glm::vec3& planePoint, const glm::vec3& planeNormal)
{
    return glm::dot(point - planePoint, glm::normalize(planeNormal));
}

Mesh::Vertex InterpolateVertex(const Mesh::Vertex& a, const Mesh::Vertex& b, float t)
{
    Mesh::Vertex mixed{};
    for (int i = 0; i < 3; ++i)
    {
        mixed.pos[i] = a.pos[i] + (b.pos[i] - a.pos[i]) * t;
        mixed.normal[i] = a.normal[i] + (b.normal[i] - a.normal[i]) * t;
        if (i < 2)
        {
            mixed.uv[i] = a.uv[i] + (b.uv[i] - a.uv[i]) * t;
        }
    }
    for (int i = 0; i < 4; ++i)
    {
        mixed.tangent[i] = a.tangent[i] + (b.tangent[i] - a.tangent[i]) * t;
    }
    for (int i = 0; i < 2; ++i)
    {
        mixed.uv1[i] = a.uv1[i] + (b.uv1[i] - a.uv1[i]) * t;
    }
    for (int i = 0; i < 4; ++i)
    {
        mixed.color[i] = a.color[i] + (b.color[i] - a.color[i]) * t;
    }
    for (int channel = 0; channel < 5; ++channel)
        for (int component = 0; component < 4; ++component)
            mixed.uvChannels[channel][component] = a.uvChannels[channel][component] +
                (b.uvChannels[channel][component] - a.uvChannels[channel][component]) * t;
    for (int i = 0; i < 4; ++i)
    {
        mixed.joints0[i] = a.joints0[i] + (b.joints0[i] - a.joints0[i]) * t;
        mixed.weights0[i] = a.weights0[i] + (b.weights0[i] - a.weights0[i]) * t;
        mixed.joints1[i] = a.joints1[i] + (b.joints1[i] - a.joints1[i]) * t;
        mixed.weights1[i] = a.weights1[i] + (b.weights1[i] - a.weights1[i]) * t;
    }
    return mixed;
}

void AppendTriangulatedPolygon(std::vector<Mesh::Vertex>& output,
    const std::vector<Mesh::Vertex>& polygon)
{
    if (polygon.size() < 3)
        return;
    for (size_t index = 1; index + 1 < polygon.size(); ++index)
    {
        output.push_back(polygon.front());
        output.push_back(polygon[index]);
        output.push_back(polygon[index + 1]);
    }
}

void ClipPolygonToHalfSpace(const std::vector<Mesh::Vertex>& polygon,
    const glm::vec3& planePoint, const glm::vec3& planeNormal,
    bool keepPositiveSide, std::vector<Mesh::Vertex>& output)
{
    output.clear();
    if (polygon.empty())
        return;

    const float epsilon = 1e-5f;
    const glm::vec3 normal = glm::normalize(planeNormal);
    std::vector<Mesh::Vertex> current = polygon;

    for (size_t i = 0; i < current.size(); ++i)
    {
        const Mesh::Vertex& a = current[i];
        const Mesh::Vertex& b = current[(i + 1) % current.size()];
        const glm::vec3 va(a.pos[0], a.pos[1], a.pos[2]);
        const glm::vec3 vb(b.pos[0], b.pos[1], b.pos[2]);
        const float da = SignedDistanceToPlane(va, planePoint, normal);
        const float db = SignedDistanceToPlane(vb, planePoint, normal);
        const bool insideA = keepPositiveSide ? da >= -epsilon : da <= epsilon;
        const bool insideB = keepPositiveSide ? db >= -epsilon : db <= epsilon;

        if (insideA && insideB)
        {
            output.push_back(b);
        }
        else if (insideA && !insideB)
        {
            const float t = std::max(0.f, std::min(1.f, da / (da - db)));
            output.push_back(InterpolateVertex(a, b, t));
        }
        else if (!insideA && insideB)
        {
            const float t = std::max(0.f, std::min(1.f, da / (da - db)));
            output.push_back(InterpolateVertex(a, b, t));
            output.push_back(b);
        }
    }
}

glm::vec3 VertexPosition(const Mesh::Vertex& vertex)
{
    return glm::vec3(vertex.pos[0], vertex.pos[1], vertex.pos[2]);
}

bool SamePosition(const glm::vec3& first, const glm::vec3& second,
    float epsilon = 1e-4f)
{
    const glm::vec3 delta = first - second;
    return glm::dot(delta, delta) <= epsilon * epsilon;
}

struct CutSegment
{
    size_t first = 0;
    size_t second = 0;
};

size_t FindOrAddWeldedCutVertex(std::vector<Mesh::Vertex>& cutVertices,
    const Mesh::Vertex& vertex)
{
    const glm::vec3 position = VertexPosition(vertex);
    for (size_t index = 0; index < cutVertices.size(); ++index)
    {
        if (SamePosition(VertexPosition(cutVertices[index]), position))
            return index;
    }

    cutVertices.push_back(vertex);
    return cutVertices.size() - 1u;
}

void AppendUniqueCutVertex(std::vector<Mesh::Vertex>& vertices,
    const Mesh::Vertex& candidate)
{
    for (const Mesh::Vertex& vertex : vertices)
    {
        if (SamePosition(VertexPosition(vertex), VertexPosition(candidate)))
            return;
    }
    vertices.push_back(candidate);
}

void CollectCutSegment(const std::vector<Mesh::Vertex>& triangle,
    const glm::vec3& planePoint, const glm::vec3& planeNormal,
    std::vector<Mesh::Vertex>& cutVertices, std::vector<CutSegment>& segments)
{
    constexpr float epsilon = 1e-5f;
    std::array<float, 3> distances{};
    bool hasPositive = false;
    bool hasNegative = false;
    for (size_t index = 0; index < triangle.size(); ++index)
    {
        distances[index] = SignedDistanceToPlane(VertexPosition(triangle[index]),
            planePoint, planeNormal);
        hasPositive = hasPositive || distances[index] > epsilon;
        hasNegative = hasNegative || distances[index] < -epsilon;
    }

    // A coplanar edge is already enclosed by its adjacent surface. Only a
    // triangle that crosses the plane contributes a boundary segment.
    if (!hasPositive || !hasNegative)
        return;

    std::vector<Mesh::Vertex> intersections;
    intersections.reserve(2);
    for (size_t index = 0; index < triangle.size(); ++index)
    {
        const size_t next = (index + 1u) % triangle.size();
        const float firstDistance = distances[index];
        const float secondDistance = distances[next];
        if (std::abs(firstDistance) <= epsilon)
            AppendUniqueCutVertex(intersections, triangle[index]);
        if ((firstDistance > epsilon && secondDistance < -epsilon) ||
            (firstDistance < -epsilon && secondDistance > epsilon))
        {
            const float t = firstDistance / (firstDistance - secondDistance);
            AppendUniqueCutVertex(intersections,
                InterpolateVertex(triangle[index], triangle[next], t));
        }
    }

    if (intersections.size() != 2u ||
        SamePosition(VertexPosition(intersections[0]), VertexPosition(intersections[1])))
        return;

    const size_t first = FindOrAddWeldedCutVertex(cutVertices, intersections[0]);
    const size_t second = FindOrAddWeldedCutVertex(cutVertices, intersections[1]);
    if (first != second)
        segments.push_back({ first, second });
}

float Cross2D(const glm::vec2& first, const glm::vec2& second,
    const glm::vec2& third)
{
    const glm::vec2 a = second - first;
    const glm::vec2 b = third - first;
    return a.x * b.y - a.y * b.x;
}

bool PointInTriangle2D(const glm::vec2& point, const glm::vec2& first,
    const glm::vec2& second, const glm::vec2& third)
{
    constexpr float epsilon = 1e-6f;
    const float a = Cross2D(first, second, point);
    const float b = Cross2D(second, third, point);
    const float c = Cross2D(third, first, point);
    return a >= -epsilon && b >= -epsilon && c >= -epsilon;
}

Mesh::Vertex BuildCapVertex(const Mesh::Vertex& boundary,
    const glm::vec3& planePoint, const glm::vec3& tangent,
    const glm::vec3& bitangent, const glm::vec3& capNormal,
    bool normalPointsPositive)
{
    Mesh::Vertex cap = boundary;
    const glm::vec3 relative = VertexPosition(boundary) - planePoint;
    const float u = glm::dot(relative, tangent);
    const float v = glm::dot(relative, bitangent);
    cap.normal[0] = capNormal.x;
    cap.normal[1] = capNormal.y;
    cap.normal[2] = capNormal.z;
    cap.tangent[0] = tangent.x;
    cap.tangent[1] = tangent.y;
    cap.tangent[2] = tangent.z;
    // Tangent-space bitangent must retain the same planar UV orientation on
    // both caps even though their surface normals face opposite directions.
    cap.tangent[3] = normalPointsPositive ? 1.f : -1.f;
    cap.uv[0] = u;
    cap.uv[1] = v;
    cap.uv1[0] = u;
    cap.uv1[1] = v;
    return cap;
}

void AppendCapForLoop(std::vector<Mesh::Vertex>& output,
    const std::vector<Mesh::Vertex>& cutVertices, std::vector<size_t> loop,
    const glm::vec3& planePoint, const glm::vec3& planeNormal,
    bool normalPointsPositive)
{
    if (loop.size() < 3u)
        return;

    const glm::vec3 normal = glm::normalize(planeNormal);
    const glm::vec3 reference = std::abs(normal.z) < 0.999f
        ? glm::vec3(0.f, 0.f, 1.f) : glm::vec3(0.f, 1.f, 0.f);
    const glm::vec3 tangent = glm::normalize(glm::cross(reference, normal));
    const glm::vec3 bitangent = glm::cross(normal, tangent);
    std::vector<glm::vec2> projected(cutVertices.size());
    for (size_t index = 0; index < cutVertices.size(); ++index)
    {
        const glm::vec3 relative = VertexPosition(cutVertices[index]) - planePoint;
        projected[index] = glm::vec2(glm::dot(relative, tangent),
            glm::dot(relative, bitangent));
    }

    // Adjacent source triangles often meet the plane on their shared face
    // diagonal. Remove that collinear seam before triangulating so a planar
    // quad produces two cap triangles instead of a fan of redundant slivers.
    bool removedCollinearPoint = true;
    while (removedCollinearPoint && loop.size() > 3u)
    {
        removedCollinearPoint = false;
        for (size_t index = 0; index < loop.size(); ++index)
        {
            const size_t previous = loop[(index + loop.size() - 1u) % loop.size()];
            const size_t current = loop[index];
            const size_t next = loop[(index + 1u) % loop.size()];
            if (std::abs(Cross2D(projected[previous], projected[current],
                projected[next])) <= 1e-6f)
            {
                loop.erase(loop.begin() + static_cast<std::ptrdiff_t>(index));
                removedCollinearPoint = true;
                break;
            }
        }
    }

    float signedArea = 0.f;
    for (size_t index = 0; index < loop.size(); ++index)
    {
        const glm::vec2& first = projected[loop[index]];
        const glm::vec2& second = projected[loop[(index + 1u) % loop.size()]];
        signedArea += first.x * second.y - first.y * second.x;
    }
    if (std::abs(signedArea) <= 1e-6f)
        return;
    if (signedArea < 0.f)
        std::reverse(loop.begin(), loop.end());

    const glm::vec3 capNormal = normalPointsPositive ? normal : -normal;
    std::vector<size_t> remaining = loop;
    // Ear clipping handles concave cross-sections, unlike a fan from one cut
    // point. The working winding is counter-clockwise in the plane basis.
    while (remaining.size() > 2u)
    {
        bool foundEar = false;
        for (size_t index = 0; index < remaining.size(); ++index)
        {
            const size_t previous = remaining[(index + remaining.size() - 1u) % remaining.size()];
            const size_t current = remaining[index];
            const size_t next = remaining[(index + 1u) % remaining.size()];
            if (Cross2D(projected[previous], projected[current], projected[next]) <= 1e-6f)
                continue;

            bool containsVertex = false;
            for (const size_t candidate : remaining)
            {
                if (candidate == previous || candidate == current || candidate == next)
                    continue;
                if (PointInTriangle2D(projected[candidate], projected[previous],
                    projected[current], projected[next]))
                {
                    containsVertex = true;
                    break;
                }
            }
            if (containsVertex)
                continue;

            const Mesh::Vertex a = BuildCapVertex(cutVertices[previous], planePoint,
                tangent, bitangent, capNormal, normalPointsPositive);
            const Mesh::Vertex b = BuildCapVertex(cutVertices[current], planePoint,
                tangent, bitangent, capNormal, normalPointsPositive);
            const Mesh::Vertex c = BuildCapVertex(cutVertices[next], planePoint,
                tangent, bitangent, capNormal, normalPointsPositive);
            if (normalPointsPositive)
            {
                output.push_back(a);
                output.push_back(b);
                output.push_back(c);
            }
            else
            {
                output.push_back(a);
                output.push_back(c);
                output.push_back(b);
            }
            remaining.erase(remaining.begin() + static_cast<std::ptrdiff_t>(index));
            foundEar = true;
            break;
        }

        // An open/self-intersecting cut must not receive a malformed cap.
        if (!foundEar)
            return;
    }
}

void AppendCutCaps(std::vector<Mesh::Vertex>& front,
    std::vector<Mesh::Vertex>& back, const std::vector<Mesh::Vertex>& cutVertices,
    const std::vector<CutSegment>& segments, const glm::vec3& planePoint,
    const glm::vec3& planeNormal)
{
    if (cutVertices.empty() || segments.empty())
        return;

    std::vector<bool> consumed(segments.size(), false);
    for (size_t segmentIndex = 0; segmentIndex < segments.size(); ++segmentIndex)
    {
        if (consumed[segmentIndex])
            continue;

        const CutSegment& firstSegment = segments[segmentIndex];
        std::vector<size_t> loop{ firstSegment.first, firstSegment.second };
        consumed[segmentIndex] = true;
        size_t current = firstSegment.second;
        bool closed = false;
        while (true)
        {
            if (current == loop.front())
            {
                closed = true;
                break;
            }

            size_t nextSegment = segments.size();
            for (size_t candidate = 0; candidate < segments.size(); ++candidate)
            {
                if (consumed[candidate])
                    continue;
                if (segments[candidate].first == current || segments[candidate].second == current)
                {
                    nextSegment = candidate;
                    break;
                }
            }
            if (nextSegment == segments.size())
                break;

            consumed[nextSegment] = true;
            const CutSegment& segment = segments[nextSegment];
            current = segment.first == current ? segment.second : segment.first;
            loop.push_back(current);
            if (loop.size() > segments.size() + 1u)
                break;
        }

        if (!closed || loop.size() < 4u)
            continue;
        loop.pop_back();
        // The positive half is closed toward negative plane normal; the
        // negative half is closed toward positive plane normal.
        AppendCapForLoop(front, cutVertices, loop, planePoint, planeNormal, false);
        AppendCapForLoop(back, cutVertices, loop, planePoint, planeNormal, true);
    }
}
}

Mesh::SliceResult Mesh::SliceByPlane(const std::vector<Vertex>& vertices,
    const glm::vec3& planePoint, const glm::vec3& planeNormal)
{
    if (vertices.size() < 3)
        return {{}, {}};

    std::vector<Vertex> front;
    std::vector<Vertex> back;
    std::vector<Vertex> cutVertices;
    std::vector<CutSegment> cutSegments;
    const glm::vec3 normal = glm::normalize(planeNormal);

    for (size_t index = 0; index < vertices.size(); index += 3)
    {
        std::vector<Vertex> triangle;
        triangle.reserve(3);
        for (size_t offset = 0; offset < 3; ++offset)
        {
            const size_t vertexIndex = index + offset;
            if (vertexIndex >= vertices.size())
                break;
            triangle.push_back(vertices[vertexIndex]);
        }
        if (triangle.size() != 3)
            continue;

        std::vector<Vertex> positiveSide;
        std::vector<Vertex> negativeSide;
        ClipPolygonToHalfSpace(triangle, planePoint, normal, true, positiveSide);
        ClipPolygonToHalfSpace(triangle, planePoint, normal, false, negativeSide);

        AppendTriangulatedPolygon(front, positiveSide);
        AppendTriangulatedPolygon(back, negativeSide);
        CollectCutSegment(triangle, planePoint, normal, cutVertices, cutSegments);
    }

    AppendCutCaps(front, back, cutVertices, cutSegments, planePoint, normal);

    return { front, back };
}
} // namespace Engine::Components


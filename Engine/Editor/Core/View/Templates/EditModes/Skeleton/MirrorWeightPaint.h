#pragma once

#include <glm/glm.hpp>
#include <algorithm>
#include <cmath>

namespace Engine::Editor::MirrorWeightPaint
{
// The segment gives the bone its length and Y axis. The joint orientation
// supplies the lateral X axis, so rotated pairs need no world-axis symmetry.
struct BoneFrame
{
    glm::vec3 root{};
    glm::vec3 x{ 1.f, 0.f, 0.f };
    glm::vec3 y{ 0.f, 1.f, 0.f };
    glm::vec3 z{ 0.f, 0.f, 1.f };
    float length = 0.f;
};

inline bool BuildFrame(const glm::mat4& rootWorld,
    const glm::mat4& tipWorld, BoneFrame& frame)
{
    const glm::vec3 root = glm::vec3(rootWorld[3]);
    const glm::vec3 tip = glm::vec3(tipWorld[3]);
    const glm::vec3 segment = tip - root;
    const float length = glm::length(segment);
    if (!std::isfinite(length) || length <= 1e-5f) return false;
    const glm::vec3 y = segment / length;
    glm::vec3 x = glm::vec3(tipWorld[0]);
    x -= y * glm::dot(x, y);
    if (glm::dot(x, x) <= 1e-8f)
    {
        x = glm::vec3(tipWorld[2]);
        x -= y * glm::dot(x, y);
    }
    if (glm::dot(x, x) <= 1e-8f) return false;
    x = glm::normalize(x);
    const glm::vec3 z = glm::normalize(glm::cross(x, y));
    frame = { root, glm::normalize(glm::cross(y, z)), y, z, length };
    return true;
}

// Normalize only the distance along the bone. Lateral distances and the
// paint radius stay in world units even when the two bones differ in length.
inline glm::vec3 MapPoint(const BoneFrame& source,
    const BoneFrame& target, const glm::vec3& worldPoint)
{
    const glm::vec3 offset = worldPoint - source.root;
    const float along = glm::dot(offset, source.y) / source.length;
    const float across = glm::dot(offset, source.x);
    const float depth = glm::dot(offset, source.z);
    return target.root + target.y * (along * target.length) -
        target.x * across + target.z * depth;
}

inline float DistanceToBone(const BoneFrame& frame, const glm::vec3& point)
{
    const float along = std::clamp(glm::dot(point - frame.root, frame.y),
        0.f, frame.length);
    return glm::length(point - (frame.root + frame.y * along));
}

struct TrianglePoint
{
    glm::vec3 point{};
    glm::vec3 barycentric{};
};

// Closest point on a triangle, including edge and vertex regions.
inline TrianglePoint ClosestPoint(const glm::vec3& p,
    const glm::vec3& a, const glm::vec3& b, const glm::vec3& c)
{
    const glm::vec3 ab = b - a, ac = c - a, ap = p - a;
    const float d1 = glm::dot(ab, ap), d2 = glm::dot(ac, ap);
    if (d1 <= 0.f && d2 <= 0.f) return { a, { 1.f, 0.f, 0.f } };
    const glm::vec3 bp = p - b;
    const float d3 = glm::dot(ab, bp), d4 = glm::dot(ac, bp);
    if (d3 >= 0.f && d4 <= d3) return { b, { 0.f, 1.f, 0.f } };
    const float vc = d1 * d4 - d3 * d2;
    if (vc <= 0.f && d1 >= 0.f && d3 <= 0.f)
    {
        const float v = d1 / (d1 - d3);
        return { a + v * ab, { 1.f - v, v, 0.f } };
    }
    const glm::vec3 cp = p - c;
    const float d5 = glm::dot(ab, cp), d6 = glm::dot(ac, cp);
    if (d6 >= 0.f && d5 <= d6) return { c, { 0.f, 0.f, 1.f } };
    const float vb = d5 * d2 - d1 * d6;
    if (vb <= 0.f && d2 >= 0.f && d6 <= 0.f)
    {
        const float w = d2 / (d2 - d6);
        return { a + w * ac, { 1.f - w, 0.f, w } };
    }
    const float va = d3 * d6 - d5 * d4;
    if (va <= 0.f && d4 - d3 >= 0.f && d5 - d6 >= 0.f)
    {
        const float w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        return { b + w * (c - b), { 0.f, 1.f - w, w } };
    }
    const float sum = va + vb + vc;
    if (std::abs(sum) <= 1e-12f)
        return { a, { 1.f, 0.f, 0.f } };
    const float inverse = 1.f / sum;
    const float v = vb * inverse, w = vc * inverse;
    return { a + v * ab + w * ac, { 1.f - v - w, v, w } };
}
}

#pragma once

#include "Core/Model/MeshData.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

namespace Engine::Editor::SkinBindingWeights
{
using Vertex = Engine::Model::AnimationVertex;

struct Diagnostics
{
    size_t unweighted = 0;
    size_t missingJoints = 0;
    size_t unnormalized = 0;
};

inline float& Weight(Vertex& vertex, int slot)
{ return slot < 4 ? vertex.weights0[slot] : vertex.weights1[slot - 4]; }
inline float& Joint(Vertex& vertex, int slot)
{ return slot < 4 ? vertex.joints0[slot] : vertex.joints1[slot - 4]; }
inline float Weight(const Vertex& vertex, int slot)
{ return slot < 4 ? vertex.weights0[slot] : vertex.weights1[slot - 4]; }
inline float Joint(const Vertex& vertex, int slot)
{ return slot < 4 ? vertex.joints0[slot] : vertex.joints1[slot - 4]; }

inline Diagnostics Inspect(const std::vector<Vertex>& vertices,
    const std::vector<bool>& resolved)
{
    Diagnostics result;
    for (const Vertex& vertex : vertices)
    {
        float sum = 0.f;
        bool missing = false;
        for (int slot = 0; slot < 8; ++slot)
        {
            const float weight = Weight(vertex, slot);
            if (weight <= 0.f) continue;
            sum += weight;
            const float joint = Joint(vertex, slot);
            if (!std::isfinite(joint) || joint < 0.f ||
                std::floor(joint) != joint || joint >= resolved.size() ||
                !resolved[static_cast<size_t>(joint)]) missing = true;
        }
        result.unweighted += sum <= 1e-6f;
        result.missingJoints += missing;
        result.unnormalized += sum > 1e-6f && std::abs(sum - 1.f) > 1e-4f;
    }
    return result;
}

inline bool Generate(std::vector<Vertex>& vertices,
    const std::vector<glm::vec3>& jointPositions)
{
    if (jointPositions.empty()) return false;
    for (const glm::vec3& position : jointPositions)
        if (!std::isfinite(position.x) || !std::isfinite(position.y) ||
            !std::isfinite(position.z)) return false;
    for (Vertex& vertex : vertices)
    {
        const glm::vec3 point(vertex.pos[0], vertex.pos[1], vertex.pos[2]);
        std::array<std::pair<float, size_t>, 4> nearest;
        nearest.fill({ std::numeric_limits<float>::infinity(), 0 });
        for (size_t joint = 0; joint < jointPositions.size(); ++joint)
        {
            const glm::vec3 offset = point - jointPositions[joint];
            const float distance2 = glm::dot(offset, offset);
            if (!std::isfinite(distance2)) return false;
            for (size_t slot = 0; slot < nearest.size(); ++slot)
                if (distance2 < nearest[slot].first)
                {
                    for (size_t shift = nearest.size() - 1; shift > slot; --shift)
                        nearest[shift] = nearest[shift - 1];
                    nearest[slot] = { distance2, joint };
                    break;
                }
        }
        for (int slot = 0; slot < 8; ++slot)
        { Weight(vertex, slot) = 0.f; Joint(vertex, slot) = 0.f; }
        if (nearest[0].first < 1e-10f)
        {
            Joint(vertex, 0) = static_cast<float>(nearest[0].second);
            Weight(vertex, 0) = 1.f;
            continue;
        }
        float total = 0.f;
        for (size_t slot = 0; slot < nearest.size(); ++slot)
        {
            if (!std::isfinite(nearest[slot].first)) break;
            Joint(vertex, static_cast<int>(slot)) =
                static_cast<float>(nearest[slot].second);
            Weight(vertex, static_cast<int>(slot)) =
                1.f / std::sqrt(std::max(nearest[slot].first, 1e-10f));
            total += Weight(vertex, static_cast<int>(slot));
        }
        for (int slot = 0; slot < 4; ++slot)
            Weight(vertex, slot) /= total;
    }
    return true;
}

inline bool Repair(std::vector<Vertex>& vertices,
    const std::vector<bool>& resolved,
    const std::vector<glm::vec3>& jointPositions)
{
    if (resolved.size() != jointPositions.size()) return false;
    std::vector<size_t> valid;
    for (size_t i = 0; i < resolved.size(); ++i)
        if (resolved[i]) valid.push_back(i);
    if (valid.empty()) return false;
    for (Vertex& vertex : vertices)
    {
        float total = 0.f;
        for (int slot = 0; slot < 8; ++slot)
        {
            const float joint = Joint(vertex, slot);
            if (!std::isfinite(joint) || joint < 0.f ||
                std::floor(joint) != joint || joint >= resolved.size() ||
                !resolved[static_cast<size_t>(joint)])
            {
                Weight(vertex, slot) = 0.f;
                Joint(vertex, slot) = 0.f;
            }
            total += Weight(vertex, slot);
        }
        if (total > 1e-6f)
        {
            for (int slot = 0; slot < 8; ++slot)
                Weight(vertex, slot) /= total;
            continue;
        }
        const glm::vec3 point(vertex.pos[0], vertex.pos[1], vertex.pos[2]);
        size_t nearest = valid.front();
        float best = std::numeric_limits<float>::infinity();
        for (size_t index : valid)
        {
            const glm::vec3 offset = point - jointPositions[index];
            const float distance = glm::dot(offset, offset);
            if (distance < best) { best = distance; nearest = index; }
        }
        for (int slot = 0; slot < 8; ++slot)
        { Weight(vertex, slot) = 0.f; Joint(vertex, slot) = 0.f; }
        Joint(vertex, 0) = static_cast<float>(nearest);
        Weight(vertex, 0) = 1.f;
    }
    return true;
}

// A failed remap leaves the source untouched so the editor can report the
// unmapped palette entry before offering explicit weight regeneration.
inline bool Remap(std::vector<Vertex>& vertices, const std::vector<int>& mapping)
{
    for (const Vertex& vertex : vertices)
        for (int slot = 0; slot < 8; ++slot)
            if (Weight(vertex, slot) > 0.f)
            {
                const float joint = Joint(vertex, slot);
                if (!std::isfinite(joint) || joint < 0.f ||
                    std::floor(joint) != joint ||
                    joint >= mapping.size() ||
                    mapping[static_cast<size_t>(joint)] < 0) return false;
            }
    for (Vertex& vertex : vertices)
        for (int slot = 0; slot < 8; ++slot)
            if (Weight(vertex, slot) > 0.f)
                Joint(vertex, slot) = static_cast<float>(
                    mapping[static_cast<size_t>(Joint(vertex, slot))]);
    return true;
}

// Removing a leaf joint changes every later palette index. Validate the
// complete mesh first so a weighted joint is never partially removed.
inline bool RemoveUnweightedJoint(std::vector<Vertex>& vertices, size_t removed)
{
    for (const Vertex& vertex : vertices)
        for (int slot = 0; slot < 8; ++slot)
        {
            const float joint = Joint(vertex, slot);
            if (Weight(vertex, slot) > 1e-6f &&
                (!std::isfinite(joint) || joint < 0.f ||
                    std::floor(joint) != joint ||
                    joint == static_cast<float>(removed)))
                return false;
        }
    for (Vertex& vertex : vertices)
        for (int slot = 0; slot < 8; ++slot)
        {
            float& joint = Joint(vertex, slot);
            if (std::isfinite(joint) && joint > static_cast<float>(removed))
                --joint;
            else if (joint == static_cast<float>(removed) &&
                Weight(vertex, slot) <= 1e-6f)
                joint = 0.f;
        }
    return true;
}

inline bool Assign(std::vector<Vertex>& vertices,
    const std::vector<uint32_t>& selection, size_t joint)
{
    if (selection.empty()) return false;
    for (uint32_t index : selection)
        if (index >= vertices.size()) return false;
    for (uint32_t index : selection)
    {
        Vertex& vertex = vertices[index];
        for (int slot = 0; slot < 8; ++slot)
        { Joint(vertex, slot) = 0.f; Weight(vertex, slot) = 0.f; }
        Joint(vertex, 0) = static_cast<float>(joint);
        Weight(vertex, 0) = 1.f;
    }
    return true;
}
}

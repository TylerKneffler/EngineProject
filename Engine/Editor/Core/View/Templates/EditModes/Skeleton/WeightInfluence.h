#pragma once

#include "Core/Model/MeshData.h"
#include <algorithm>
#include <cmath>
#include <vector>

namespace Engine::Editor::WeightInfluence
{
inline float Get(const Engine::Model::AnimationVertex& vertex, int bone)
{
    float result = 0.f;
    for (int slot = 0; slot < 8; ++slot)
    {
        const int offset = slot % 4;
        const float joint = slot < 4 ? vertex.joints0[offset] : vertex.joints1[offset];
        const float weight = slot < 4 ? vertex.weights0[offset] : vertex.weights1[offset];
        if (weight > 0.f && static_cast<int>(joint) == bone) result += weight;
    }
    return result;
}

// Keeps locked influences byte-for-byte intact. The remaining budget is
// distributed between the painted bone and the other unlocked influences.
inline bool Set(Engine::Model::AnimationVertex& vertex, int bone, float target,
    const std::vector<bool>& locked)
{
    if (bone < 0 || (bone < static_cast<int>(locked.size()) && locked[bone]) ||
        !std::isfinite(target)) return false;
    float* weights[]{ &vertex.weights0[0], &vertex.weights0[1],
        &vertex.weights0[2], &vertex.weights0[3], &vertex.weights1[0],
        &vertex.weights1[1], &vertex.weights1[2], &vertex.weights1[3] };
    float* joints[]{ &vertex.joints0[0], &vertex.joints0[1],
        &vertex.joints0[2], &vertex.joints0[3], &vertex.joints1[0],
        &vertex.joints1[1], &vertex.joints1[2], &vertex.joints1[3] };
    float before[8];
    for (int slot = 0; slot < 8; ++slot) before[slot] = *weights[slot];

    int selected = -1, freeSlot = -1, weakest = -1;
    float lockedTotal = 0.f, otherTotal = 0.f;
    for (int slot = 0; slot < 8; ++slot)
    {
        const float weight = *weights[slot];
        const int joint = static_cast<int>(*joints[slot]);
        if (weight > 0.f && joint >= 0 &&
            joint < static_cast<int>(locked.size()) && locked[joint])
        {
            lockedTotal += weight;
            continue;
        }
        if (weight > 0.f && joint == bone)
        {
            if (selected < 0) selected = slot;
            else { *weights[selected] += weight; *weights[slot] = 0.f; }
        }
        else
        {
            otherTotal += weight;
            if (weight <= 0.f && freeSlot < 0) freeSlot = slot;
            if (weakest < 0 || weight < *weights[weakest]) weakest = slot;
        }
    }
    const float budget = std::max(0.f, 1.f - lockedTotal);
    target = std::clamp(target, 0.f, budget);
    if (selected < 0 && (target > 0.f ||
        (otherTotal <= 1e-6f && budget > 0.f)))
    {
        selected = freeSlot >= 0 ? freeSlot : weakest;
        if (selected < 0) return false;
        otherTotal -= *weights[selected];
        *joints[selected] = static_cast<float>(bone);
    }
    if (otherTotal <= 1e-6f && selected >= 0) target = budget;
    if (selected >= 0) *weights[selected] = target;
    const float factor = otherTotal > 1e-6f
        ? (budget - target) / otherTotal : 0.f;
    for (int slot = 0; slot < 8; ++slot)
    {
        if (slot == selected) continue;
        const int joint = static_cast<int>(*joints[slot]);
        if (*weights[slot] > 0.f && joint >= 0 &&
            joint < static_cast<int>(locked.size()) && locked[joint]) continue;
        *weights[slot] *= factor;
    }
    for (int slot = 0; slot < 8; ++slot)
        if (std::abs(*weights[slot] - before[slot]) > 1e-6f) return true;
    return false;
}
}

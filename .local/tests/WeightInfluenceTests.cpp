#include "Editor/Core/View/Templates/EditModes/Skeleton/WeightInfluence.h"
#include <cassert>
#include <cmath>

using Engine::Model::AnimationVertex;
using Engine::Editor::WeightInfluence::Get;
using Engine::Editor::WeightInfluence::Set;

static bool Near(float a, float b)
{
    return std::abs(a - b) < 1e-5f;
}

int main()
{
    AnimationVertex vertex{};
    vertex.joints0[0] = 0.f; vertex.weights0[0] = .25f;
    vertex.joints0[1] = 1.f; vertex.weights0[1] = .5f;
    vertex.joints0[2] = 2.f; vertex.weights0[2] = .25f;
    const std::vector<bool> locked{ false, true, false, false };
    assert(Set(vertex, 0, .4f, locked));
    assert(Near(Get(vertex, 0), .4f));
    assert(Near(Get(vertex, 1), .5f));
    assert(Near(Get(vertex, 2), .1f));
    assert(!Set(vertex, 1, .2f, locked));
    assert(Near(Get(vertex, 1), .5f));
    assert(Set(vertex, 3, .3f, locked));
    assert(Near(Get(vertex, 3), .3f));
    assert(Near(Get(vertex, 1), .5f));
    assert(Near(Get(vertex, 0) + Get(vertex, 1) +
        Get(vertex, 2) + Get(vertex, 3), 1.f));

    AnimationVertex full{};
    for (int slot = 0; slot < 4; ++slot)
    {
        full.joints0[slot] = static_cast<float>(slot);
        full.joints1[slot] = static_cast<float>(slot + 4);
        full.weights0[slot] = .125f;
        full.weights1[slot] = .125f;
    }
    const std::vector<bool> oneLocked{ true };
    assert(Set(full, 8, .25f, oneLocked));
    assert(Near(Get(full, 0), .125f));
    assert(Near(Get(full, 8), .25f));
    float sum = 0.f;
    for (float weight : full.weights0) sum += weight;
    for (float weight : full.weights1) sum += weight;
    assert(Near(sum, 1.f));

    AnimationVertex fixed{};
    fixed.joints0[0] = 0.f; fixed.weights0[0] = 1.f;
    assert(!Set(fixed, 1, .5f, oneLocked));
    assert(Near(Get(fixed, 0), 1.f));
}

#include "Editor/Core/View/Templates/EditModes/Skeleton/WeightPaintBucket.h"
#include "Editor/Core/View/Templates/EditModes/Skeleton/WeightInfluence.h"
#include <cassert>
#include <cmath>

int main()
{
    using namespace Engine::Editor;
    const std::vector<uint32_t> indices{
        0, 1, 2, 1, 2, 3, 2, 3, 4, 5, 6, 7 };
    const auto topology = WeightPaintSurface::BuildTopology(indices, 8);
    const std::vector<float> weights{ .2f, .2f, .2f, .2f,
        1.f, .2f, .2f, .2f };
    const auto filled = WeightPaintBucket::ConnectedVertices(topology,
        weights, 0, .05f);
    assert((filled == std::vector<uint32_t>{ 0, 1, 2, 3 }));
    assert(filled == WeightPaintBucket::ConnectedVertices(topology,
        weights, 0, .05f));
    assert((WeightPaintBucket::ConnectedVertices(topology, weights, 0,
        1.f) == std::vector<uint32_t>{ 0, 1, 2, 3, 4 }));
    assert(WeightPaintBucket::ConnectedVertices(topology, weights, 99,
        .05f).empty());

    Engine::Model::AnimationVertex vertex{};
    vertex.joints0[0] = 0.f; vertex.weights0[0] = .6f;
    vertex.joints0[1] = 1.f; vertex.weights0[1] = .4f;
    assert(WeightInfluence::Set(vertex, 0, 0.f, { false, false }));
    assert(std::abs(WeightInfluence::Get(vertex, 0)) < 1e-6f);
    assert(std::abs(WeightInfluence::Get(vertex, 1) - 1.f) < 1e-6f);

    Engine::Model::AnimationVertex locked{};
    locked.joints0[0] = 0.f; locked.weights0[0] = .6f;
    locked.joints0[1] = 1.f; locked.weights0[1] = .4f;
    assert(!WeightInfluence::Set(locked, 0, 0.f, { false, true }));
    assert(std::abs(WeightInfluence::Get(locked, 0) - .6f) < 1e-6f);
    assert(std::abs(WeightInfluence::Get(locked, 1) - .4f) < 1e-6f);
}

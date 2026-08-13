#pragma once

#include <vector>
#include <string>

// Serializable animation channel data. Sampling and playback live in the
// animation components, not in this model type.
namespace Engine::Model
{
struct AnimationChannel
{
    enum class Path { Translation, Rotation, Scale, Weights };
    enum class Interpolation { Linear, Step, CubicSpline };
    unsigned nodeIndex = 0;
    Path path = Path::Translation;
    Interpolation interpolation = Interpolation::Linear;
    unsigned valueWidth = 3;
    std::vector<float> times;
    std::vector<float> values;
};

struct AnimationLayer
{
    std::string clip;
    float weight = 1.f;
    float speed = 1.f;
    float time = 0.f;
    bool enabled = true;
    bool looping = true;
    bool additive = false;
    std::vector<unsigned> nodeMask;
};
}


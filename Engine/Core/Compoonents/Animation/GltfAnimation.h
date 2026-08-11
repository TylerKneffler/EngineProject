#pragma once

#include "Core/Script.h"
#include "Core/Compoonents/Mesh.h"
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <chrono>
#include <vector>
#include <string>
#include <unordered_map>

class Object;

class ModelNode : public Component
{
public:
    ModelNode();
    unsigned nodeIndex = 0;
};

struct AnimationChannel
{
    enum class Path { Translation, Rotation, Scale, Weights };
    enum class Interpolation { Linear, Step, CubicSpline };
    unsigned nodeIndex = 0;
    Path path = Path::Translation;
    Interpolation interpolation = Interpolation::Linear;
    unsigned valueWidth = 3;
    std::vector<float> times;
    // Flat key data. Cubic splines store in-tangent/value/out-tangent per key.
    std::vector<float> values;
};

class Animation : public Component
{
public:
    Animation();
    std::string clipName;
    float duration = 0.f;
    std::vector<AnimationChannel> channels;
    JsonValue Serialize() const override;
    void Deserialize(const JsonValue& value) override;
};

class MorphTargets : public Component
{
public:
    struct Target
    {
        std::vector<glm::vec3> positions;
        std::vector<glm::vec3> normals;
        std::vector<glm::vec3> tangents;
    };

    MorphTargets();
    unsigned nodeIndex = 0;
    std::vector<float> weights;
    std::vector<Target> targets;
    JsonValue Serialize() const override;
    void Deserialize(const JsonValue& value) override;
};

class Skeleton : public Component
{
public:
    Skeleton();
    ComponentReference hierarchyRootReference { "ModelNode" };
    unsigned skinIndex = 0;
    std::vector<unsigned> jointNodes;
    std::vector<glm::mat4> inverseBindMatrices;
    JsonValue Serialize() const override;
    void Deserialize(const JsonValue& value) override;
    Object* FindNode(unsigned index) const;
};

class SkinnedMesh : public Script
{
public:
    SkinnedMesh();
    ComponentReference meshReference { "Mesh" };
    ComponentReference morphTargetsReference { "MorphTargets" };
    ComponentReference skeletonReference { "Skeleton" };
    int skinIndex = -1;
    std::vector<glm::uvec4> joints;
    std::vector<glm::vec4> weights;
    void Start() override;
    void Update() override;
    void OnAfterDeserialize(IGraphicsProvider*) override { Start(); }
    bool BuildPalette(std::vector<glm::mat4>& palette) const;
    JsonValue Serialize() const override;
    void Deserialize(const JsonValue& value) override;

private:
    std::vector<Vertex> m_baseVertices;
};

class AnimationManager : public Script
{
public:
    struct Layer
    {
        std::string clip;
        float weight = 1.f;
        float speed = 1.f;
        float time = 0.f;
        bool enabled = true;
        bool looping = true;
        bool additive = false;
        // Empty affects the entire hierarchy; otherwise only these model nodes.
        std::vector<unsigned> nodeMask;
    };

    AnimationManager();
    ComponentReference hierarchyRootReference { "ModelNode" };
    ComponentReference animationSourceReference { "Animation" };
    std::string clip;
    bool playing = true;
    bool looping = true;
    float speed = 1.f;
    float time = 0.f;
    std::vector<Layer> layers;
    void Start() override;
    void Update() override;
    void Tick(float deltaSeconds);
    void Play(const std::string& clipName, float fadeSeconds = 0.2f);
    JsonValue Serialize() const override;
    void Deserialize(const JsonValue& value) override;
    bool DrawProperties(IEditorUi& ui) override;

private:
    struct RestTransform
    {
        glm::vec3 translation{};
        glm::quat rotation { 1.f, 0.f, 0.f, 0.f };
        glm::vec3 scale { 1.f };
    };
    std::chrono::steady_clock::time_point m_lastTick{};
    std::unordered_map<unsigned, RestTransform> m_restPose;
    std::unordered_map<unsigned, std::vector<float>> m_restMorphs;
    std::string m_previousClip;
    float m_previousTime = 0.f;
    float m_fadeDuration = 0.f;
    float m_fadeElapsed = 0.f;
};

// Source compatibility for projects using the original glTF-specific names.
using GltfNode = ModelNode;
using GltfAnimationChannel = AnimationChannel;

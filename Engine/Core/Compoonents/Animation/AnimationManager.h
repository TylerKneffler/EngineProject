#pragma once

#include "Core/Component.h"
#include "Core/Model/AnimationData.h"
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <chrono>
#include <string>
#include <unordered_map>
#include <vector>

class AnimationManager : public Component
{
public:
    using Layer = AnimationLayer;

    AnimationManager();
    ComponentReference modelReference { "Model" };
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

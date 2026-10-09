#pragma once

#include "Core/Component.h"
#include "Core/Model/AnimationData.h"
#include "Core/Model/AnimationClip.h"
#include "Core/Model/RigAsset.h"
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace Engine::Components
{
class Model;
struct AnimationManagerScratch;
class AnimationManager : public Engine::Core::Component
{
public:
    using Layer = Engine::Model::AnimationLayer;

    AnimationManager();
    ~AnimationManager() override;
    ComponentReference modelReference { "Model" };
    std::string clip;
    bool playing = true;
    // Keep the current skeletal transforms when playback stops, including
    // an IK-blended pose authored by another component.
    bool holdCurrentPoseWhenStopped = false;
    bool looping = true;
    float speed = 1.f;
    float time = 0.f;
    std::vector<Engine::Model::AnimationClip> clips;
    std::vector<std::string> rigPaths;
    std::vector<Engine::Model::RigAsset> LoadRigAssets() const;
    std::vector<Layer> layers;
    const Engine::Model::AnimationClip* FindClip(const std::string& name = {}) const;
    std::vector<const Engine::Model::AnimationClip*> GetAvailableClips() const;
    void Start() override;
    void Update() override;
    void Tick(float deltaSeconds);
    void Play(const std::string& clipName, float fadeSeconds = 0.2f);
    JsonValue Serialize() const override;
    void Deserialize(const JsonValue& value) override;
    bool DrawProperties(::Engine::Editor::IEditorUi& ui) override;

private:
    Model* ResolveModel() const;
    struct RestTransform
    {
        glm::vec3 translation{};
        glm::quat rotation { 1.f, 0.f, 0.f, 0.f };
        glm::vec3 scale { 1.f };
    };
    std::unordered_map<unsigned, RestTransform> m_restPose;
    std::unordered_map<unsigned, std::vector<float>> m_restMorphs;
    std::string m_previousClip;
    float m_previousTime = 0.f;
    float m_fadeDuration = 0.f;
    float m_fadeElapsed = 0.f;
    mutable Model* m_cachedModel = nullptr;
    mutable uint64_t m_cachedStructureRevision = 0;
    mutable uint64_t m_cachedConfigurationRevision = 0;
    mutable bool m_modelCacheValid = false;
    std::unique_ptr<AnimationManagerScratch> m_scratch;
};
}

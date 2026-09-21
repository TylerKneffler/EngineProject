#include "AnimationManager.h"
#include "Animation.h"
#include "Model.h"
#include "Core/Compoonents/Obj/Mesh.h"
#include "Core/Object.h"
#include "Core/Scene/Scene.h"
#include "Engine/Editor/UI/IEditorUi.h"
#include <algorithm>
#include <cmath>
#include <sstream>

namespace Engine::Components
{
namespace
{
glm::vec3 QuaternionEuler(const glm::quat& q)
{
    const float sinX = 2.f * (q.w * q.x + q.y * q.z);
    const float cosX = 1.f - 2.f * (q.x * q.x + q.y * q.y);
    const float sinY = std::clamp(2.f * (q.w * q.y - q.z * q.x), -1.f, 1.f);
    const float sinZ = 2.f * (q.w * q.z + q.x * q.y);
    const float cosZ = 1.f - 2.f * (q.y * q.y + q.z * q.z);
    return { std::atan2(sinX, cosX), std::asin(sinY), std::atan2(sinZ, cosZ) };
}

glm::quat EulerQuaternion(const glm::vec3& euler)
{
    return glm::normalize(glm::angleAxis(euler.z, glm::vec3(0.f, 0.f, 1.f)) *
        glm::angleAxis(euler.y, glm::vec3(0.f, 1.f, 0.f)) *
        glm::angleAxis(euler.x, glm::vec3(1.f, 0.f, 0.f)));
}

void Sample(const Engine::Model::AnimationChannel& channel, float time,
    std::vector<float>& result)
{
    result.assign(channel.valueWidth, 0.f);
    if (channel.times.empty() || channel.valueWidth == 0) return;
    const auto upper = std::upper_bound(channel.times.begin(), channel.times.end(), time);
    const size_t next = std::min<size_t>(upper - channel.times.begin(), channel.times.size() - 1);
    const size_t previous = next == 0 ? 0 : next - 1;
    const float span = channel.times[next] - channel.times[previous];
    float amount = span > 0.f ? (time - channel.times[previous]) / span : 0.f;
    amount = std::clamp(amount, 0.f, 1.f);
    if (channel.interpolation == Engine::Model::AnimationChannel::Interpolation::Step) amount = 0.f;
    const size_t stride = channel.valueWidth *
        (channel.interpolation == Engine::Model::AnimationChannel::Interpolation::CubicSpline ? 3u : 1u);
    const size_t valueOffset = channel.interpolation == Engine::Model::AnimationChannel::Interpolation::CubicSpline
        ? channel.valueWidth : 0u;
    if ((next + 1) * stride > channel.values.size()) return;
    if (channel.path == Engine::Model::AnimationChannel::Path::Rotation && channel.valueWidth == 4 &&
        channel.interpolation == Engine::Model::AnimationChannel::Interpolation::Linear)
    {
        const glm::quat first(channel.values[previous * stride + 3],
            channel.values[previous * stride], channel.values[previous * stride + 1],
            channel.values[previous * stride + 2]);
        const glm::quat second(channel.values[next * stride + 3],
            channel.values[next * stride], channel.values[next * stride + 1],
            channel.values[next * stride + 2]);
        const glm::quat resultRotation = glm::normalize(glm::slerp(first, second, amount));
        result[0] = resultRotation.x;
        result[1] = resultRotation.y;
        result[2] = resultRotation.z;
        result[3] = resultRotation.w;
        return;
    }
    for (size_t component = 0; component < channel.valueWidth; ++component)
    {
        const float first = channel.values[previous * stride + valueOffset + component];
        const float second = channel.values[next * stride + valueOffset + component];
        if (channel.interpolation != Engine::Model::AnimationChannel::Interpolation::CubicSpline || previous == next)
            result[component] = first + (second - first) * amount;
        else
        {
            const float squared = amount * amount;
            const float cubed = squared * amount;
            const float outTangent = channel.values[
                previous * stride + valueOffset * 2 + component] * span;
            const float inTangent = channel.values[next * stride + component] * span;
            result[component] = (2 * cubed - 3 * squared + 1) * first +
                (cubed - 2 * squared + amount) * outTangent +
                (-2 * cubed + 3 * squared) * second +
                (cubed - squared) * inTangent;
        }
    }
}

struct NodePose
{
    bool active = false;
    bool hasTranslation = false;
    bool hasRotation = false;
    bool hasScale = false;
    bool hasWeights = false;
    glm::vec3 translation{};
    glm::quat rotation { 1.f, 0.f, 0.f, 0.f };
    glm::vec3 scale { 1.f };
    std::vector<float> weights;
};
struct ReusablePose
{
    std::vector<NodePose> nodes;
    std::vector<unsigned> activeNodes;

    void Reset()
    {
        for (const unsigned index : activeNodes)
        {
            NodePose& node = nodes[index];
            node.active = false;
            node.hasTranslation = false;
            node.hasRotation = false;
            node.hasScale = false;
            node.hasWeights = false;
        }
        activeNodes.clear();
    }

    NodePose& Get(unsigned index)
    {
        if (nodes.size() <= index)
            nodes.resize(static_cast<size_t>(index) + 1u);
        NodePose& node = nodes[index];
        if (!node.active)
        {
            node.active = true;
            activeNodes.push_back(index);
        }
        return node;
    }

    const NodePose* Find(unsigned index) const
    {
        if (index >= nodes.size()) return nullptr;
        const NodePose& node = nodes[index];
        return node.active ? &node : nullptr;
    }
};

Animation* FindAnimation(Engine::Core::Object* owner, const std::string& name)
{
    if (!owner) return nullptr;
    Animation* first = nullptr;
    for (Engine::Core::Component* component : owner->Components)
        if (auto* animation = dynamic_cast<Animation*>(component))
        {
            if (!first) first = animation;
            if (!name.empty() && animation->clipName == name) return animation;
        }
    return name.empty() ? first : nullptr;
}

void SampleAnimation(const Animation* animation, float time,
    size_t nodeCount, ReusablePose& pose, std::vector<float>& channelValues)
{
    pose.Reset();
    if (!animation) return;
    for (const Engine::Model::AnimationChannel& channel : animation->channels)
    {
        if (channel.nodeIndex >= nodeCount)
            continue;
        NodePose& node = pose.Get(channel.nodeIndex);
        std::vector<float>& sampled =
            channel.path == Engine::Model::AnimationChannel::Path::Weights
                ? node.weights : channelValues;
        Sample(channel, time, sampled);
        switch (channel.path)
        {
        case Engine::Model::AnimationChannel::Path::Translation:
            if (sampled.size() >= 3) { node.hasTranslation = true; node.translation = { sampled[0], sampled[1], sampled[2] }; }
            break;
        case Engine::Model::AnimationChannel::Path::Rotation:
            if (sampled.size() >= 4) { node.hasRotation = true; node.rotation = glm::normalize(glm::quat(sampled[3], sampled[0], sampled[1], sampled[2])); }
            break;
        case Engine::Model::AnimationChannel::Path::Scale:
            if (sampled.size() >= 3) { node.hasScale = true; node.scale = { sampled[0], sampled[1], sampled[2] }; }
            break;
        case Engine::Model::AnimationChannel::Path::Weights:
            node.hasWeights = true; break;
        }
    }
}

bool IncludesNode(const AnimationManager::Layer& layer, unsigned node)
{
    return layer.nodeMask.empty() ||
        std::find(layer.nodeMask.begin(), layer.nodeMask.end(), node) != layer.nodeMask.end();
}

float AdvanceTime(float value, float delta, float speed, bool looping,
    const Animation* animation)
{
    value += delta * speed;
    if (!animation || animation->duration <= 0.f) return value;
    return looping ? std::fmod(std::max(value, 0.f), animation->duration)
        : std::clamp(value, 0.f, animation->duration);
}
}

struct AnimationManagerScratch
{
    ReusablePose basePose;
    ReusablePose sampledPose;
    ReusablePose finalPose;
    std::vector<float> channelValues;
};

AnimationManager::AnimationManager()
    : m_scratch(std::make_unique<AnimationManagerScratch>())
{
    SetTypeName(COMPONENT_TYPE_NAME(AnimationManager));
    RegisterField("modelReference", modelReference);
    RegisterField("animationSourceReference", animationSourceReference);
    RegisterField("clip", clip);
    RegisterField("playing", playing);
    RegisterField("looping", looping);
    RegisterField("speed", speed);
    RegisterField("time", time);
}

AnimationManager::~AnimationManager() = default;

Model* AnimationManager::ResolveModel() const
{
    const uint64_t structureRevision = Owner && Owner->GetScene()
        ? Owner->GetScene()->GetStructureRevision() : 0;
    const uint64_t configurationRevision = GetConfigurationRevision();
    if (m_modelCacheValid &&
        m_cachedStructureRevision == structureRevision &&
        m_cachedConfigurationRevision == configurationRevision)
        return m_cachedModel;

    Model* model = modelReference.IsAssigned()
        ? Engine::Core::ResolveComponentReference<Model>(Owner, modelReference) : nullptr;
    if (!modelReference.IsAssigned())
        for (Engine::Core::Object* current = Owner; current && !model;
            current = current->Parent)
            model = current->GetComponent<Model>();
    m_cachedModel = model;
    m_cachedStructureRevision = structureRevision;
    m_cachedConfigurationRevision = configurationRevision;
    m_modelCacheValid = true;
    return m_cachedModel;
}

void AnimationManager::Start()
{
    m_lastTick = std::chrono::steady_clock::now();
    m_restPose.clear();
    m_restMorphs.clear();
    Model* model = ResolveModel();
    if (!model) return;
    const std::vector<Engine::Core::Object*>& nodes = model->ResolveNodes();
    for (ReusablePose* pose : { &m_scratch->basePose,
        &m_scratch->sampledPose, &m_scratch->finalPose })
    {
        pose->nodes.resize(nodes.size());
        pose->activeNodes.reserve(nodes.size());
    }
    for (unsigned index = 0; index < nodes.size(); ++index)
        if (Engine::Core::Object* object = nodes[index])
            m_restPose[index] = { object->transform.position,
                EulerQuaternion(object->transform.rotation), object->transform.scale };
    const auto captureMorphs = [&](auto&& self, Engine::Core::Object* object) -> void
    {
        if (!object) return;
        if (const auto* mesh = object->GetComponent<Mesh>();
            mesh && mesh->HasMorphTargets())
            m_restMorphs.emplace(mesh->GetMorphNodeIndex(), mesh->GetMorphWeights());
        for (Engine::Core::Object* child : object->Children) self(self, child);
    };
    captureMorphs(captureMorphs, model->Owner);
    for (const auto& [nodeIndex, weights] : m_restMorphs)
        if (nodeIndex < nodes.size())
            for (ReusablePose* pose : { &m_scratch->basePose,
                &m_scratch->sampledPose, &m_scratch->finalPose })
                pose->nodes[nodeIndex].weights.reserve(weights.size());
    if (clip.empty())
    {
        Animation* first = animationSourceReference.IsAssigned()
            ? Engine::Core::ResolveComponentReference<Animation>(Owner, animationSourceReference)
            : FindAnimation(Owner, {});
        if (first) clip = first->clipName;
    }
}

void AnimationManager::Play(const std::string& clipName, float fadeSeconds)
{
    if (clipName.empty() || clipName == clip) return;
    if (!clip.empty() && fadeSeconds > 0.f)
    {
        m_previousClip = clip;
        m_previousTime = time;
        m_fadeDuration = fadeSeconds;
        m_fadeElapsed = 0.f;
    }
    else
    {
        m_previousClip.clear();
        m_fadeDuration = m_fadeElapsed = 0.f;
    }
    clip = clipName;
    time = 0.f;
    playing = true;
}

void AnimationManager::Update()
{
    const auto now = std::chrono::steady_clock::now();
    if (m_lastTick.time_since_epoch().count() == 0) m_lastTick = now;
    const float frameDelta = std::min(
        std::chrono::duration<float>(now - m_lastTick).count(), 0.1f);
    m_lastTick = now;
    Tick(frameDelta);
}

void AnimationManager::Tick(float frameDelta)
{
    if (m_restPose.empty()) Start();
    frameDelta = std::max(frameDelta, 0.f);
    const float delta = playing ? frameDelta : 0.f;
    const auto resolveAnimation = [this](const std::string& clipName)
    {
        Animation* assigned = animationSourceReference.IsAssigned()
            ? Engine::Core::ResolveComponentReference<Animation>(Owner, animationSourceReference) : nullptr;
        if (animationSourceReference.IsAssigned())
            return assigned && (clipName.empty() || assigned->clipName == clipName)
                ? assigned : nullptr;
        return FindAnimation(Owner, clipName);
    };
    Animation* animation = resolveAnimation(clip);
    if (!animation) return;
    Model* model = ResolveModel();
    if (!model) return;
    const size_t nodeCount = model->GetNodeCount();
    time = AdvanceTime(time, delta, speed, looping, animation);
    ReusablePose& basePose = m_scratch->basePose;
    ReusablePose& sampledPose = m_scratch->sampledPose;
    ReusablePose& finalPose = m_scratch->finalPose;
    SampleAnimation(animation, time, nodeCount, basePose,
        m_scratch->channelValues);

    if (!m_previousClip.empty() && m_fadeDuration > 0.f)
    {
        Animation* previousAnimation = resolveAnimation(m_previousClip);
        m_previousTime = AdvanceTime(m_previousTime, delta, speed, looping, previousAnimation);
        m_fadeElapsed += delta;
        const float blend = std::clamp(m_fadeElapsed / m_fadeDuration, 0.f, 1.f);
        SampleAnimation(previousAnimation, m_previousTime, nodeCount,
            sampledPose, m_scratch->channelValues);
        for (const auto& [nodeIndex, rest] : m_restPose)
        {
            NodePose& output = basePose.Get(nodeIndex);
            const NodePose* old = sampledPose.Find(nodeIndex);
            output.translation = glm::mix(old && old->hasTranslation ? old->translation : rest.translation,
                output.hasTranslation ? output.translation : rest.translation, blend);
            output.rotation = glm::normalize(glm::slerp(
                old && old->hasRotation ? old->rotation : rest.rotation,
                output.hasRotation ? output.rotation : rest.rotation, blend));
            output.scale = glm::mix(old && old->hasScale ? old->scale : rest.scale,
                output.hasScale ? output.scale : rest.scale, blend);
            output.hasTranslation = output.hasRotation = output.hasScale = true;
        }
        for (const auto& [nodeIndex, restWeights] : m_restMorphs)
        {
            NodePose& output = basePose.Get(nodeIndex);
            const NodePose* old = sampledPose.Find(nodeIndex);
            const std::vector<float>& oldWeights = old && old->hasWeights
                ? old->weights : restWeights;
            const bool useOutputWeights = output.hasWeights;
            const size_t newWeightCount = useOutputWeights
                ? output.weights.size() : restWeights.size();
            output.weights.resize(std::max(oldWeights.size(), newWeightCount), 0.f);
            for (size_t i = 0; i < output.weights.size(); ++i)
            {
                const float oldValue = i < oldWeights.size() ? oldWeights[i] : 0.f;
                const float newValue = useOutputWeights && i < newWeightCount
                    ? output.weights[i]
                    : (i < restWeights.size() ? restWeights[i] : 0.f);
                output.weights[i] = oldValue + (newValue - oldValue) * blend;
            }
            output.hasWeights = true;
        }
        if (blend >= 1.f)
        {
            m_previousClip.clear();
            m_fadeDuration = m_fadeElapsed = 0.f;
        }
    }

    finalPose.Reset();
    for (const auto& [nodeIndex, rest] : m_restPose)
    {
        NodePose& result = finalPose.Get(nodeIndex);
        result.hasTranslation = result.hasRotation = result.hasScale = true;
        result.translation = rest.translation;
        result.rotation = rest.rotation;
        result.scale = rest.scale;
        if (const NodePose* found = basePose.Find(nodeIndex))
        {
            if (found->hasTranslation) result.translation = found->translation;
            if (found->hasRotation) result.rotation = found->rotation;
            if (found->hasScale) result.scale = found->scale;
        }
    }
    for (const auto& [nodeIndex, restWeights] : m_restMorphs)
    {
        NodePose& result = finalPose.Get(nodeIndex);
        result.hasWeights = true;
        result.weights = restWeights;
        if (const NodePose* found = basePose.Find(nodeIndex);
            found && found->hasWeights)
            result.weights = found->weights;
    }

    for (Layer& layer : layers)
    {
        if (!layer.enabled || layer.weight <= 0.f) continue;
        Animation* layerAnimation = resolveAnimation(layer.clip);
        if (!layerAnimation) continue;
        layer.time = AdvanceTime(layer.time, delta, layer.speed, layer.looping, layerAnimation);
        SampleAnimation(layerAnimation, layer.time, nodeCount, sampledPose,
            m_scratch->channelValues);
        const float layerWeight = std::clamp(layer.weight, 0.f, 1.f);
        for (const unsigned nodeIndex : sampledPose.activeNodes)
        {
            const NodePose& sampled = sampledPose.nodes[nodeIndex];
            if (!IncludesNode(layer, nodeIndex)) continue;
            NodePose& result = finalPose.Get(nodeIndex);
            if (const auto restFound = m_restPose.find(nodeIndex); restFound != m_restPose.end())
            {
                const RestTransform& rest = restFound->second;
                if (sampled.hasTranslation)
                    result.translation = layer.additive
                        ? result.translation + (sampled.translation - rest.translation) * layerWeight
                        : glm::mix(result.translation, sampled.translation, layerWeight);
                if (sampled.hasRotation)
                    result.rotation = layer.additive
                        ? glm::normalize(result.rotation * glm::slerp(
                            glm::quat(1.f, 0.f, 0.f, 0.f),
                            sampled.rotation * glm::inverse(rest.rotation), layerWeight))
                        : glm::normalize(glm::slerp(result.rotation, sampled.rotation, layerWeight));
                if (sampled.hasScale)
                    result.scale = layer.additive
                        ? result.scale * glm::mix(glm::vec3(1.f), sampled.scale /
                            glm::max(glm::abs(rest.scale), glm::vec3(0.00001f)), layerWeight)
                        : glm::mix(result.scale, sampled.scale, layerWeight);
            }
            if (sampled.hasWeights)
            {
                const auto restWeights = m_restMorphs.find(nodeIndex);
                result.weights.resize(std::max(result.weights.size(), sampled.weights.size()), 0.f);
                for (size_t i = 0; i < result.weights.size(); ++i)
                {
                    const float sampledValue = i < sampled.weights.size() ? sampled.weights[i] : 0.f;
                    const float restValue = restWeights != m_restMorphs.end() &&
                        i < restWeights->second.size() ? restWeights->second[i] : 0.f;
                    result.weights[i] = layer.additive
                        ? result.weights[i] + (sampledValue - restValue) * layerWeight
                        : result.weights[i] + (sampledValue - result.weights[i]) * layerWeight;
                }
                result.hasWeights = true;
            }
        }
    }

    const std::vector<Engine::Core::Object*>& nodes = model->ResolveNodes();
    for (const unsigned nodeIndex : finalPose.activeNodes)
    {
        const NodePose& pose = finalPose.nodes[nodeIndex];
        if (Engine::Core::Object* target = nodeIndex < nodes.size()
            ? nodes[nodeIndex] : nullptr)
        {
            if (pose.hasTranslation) target->transform.position = pose.translation;
            if (pose.hasRotation) target->transform.rotation = QuaternionEuler(pose.rotation);
            if (pose.hasScale) target->transform.scale = pose.scale;
        }
    }
    const auto applyMorphs = [&](auto&& self, Engine::Core::Object* object) -> void
    {
        if (!object) return;
        if (auto* mesh = object->GetComponent<Mesh>();
            mesh && mesh->HasMorphTargets())
            if (const NodePose* found = finalPose.Find(mesh->GetMorphNodeIndex());
                found && found->hasWeights)
                mesh->SetMorphWeights(found->weights);
        for (Engine::Core::Object* child : object->Children) self(self, child);
    };
    applyMorphs(applyMorphs, model->Owner);
}

AnimationManager::JsonValue AnimationManager::Serialize() const
{
    JsonValue result = Component::Serialize();
    JsonValue serializedLayers = JsonValue::MakeArray();
    for (const Layer& layer : layers)
    {
        JsonValue mask = JsonValue::MakeArray();
        for (unsigned node : layer.nodeMask)
            mask.Push(JsonValue(static_cast<int>(node)));
        serializedLayers.Push(JsonValue::MakeObject().Set("clip", JsonValue(layer.clip))
            .Set("weight", JsonValue(layer.weight)).Set("speed", JsonValue(layer.speed))
            .Set("time", JsonValue(layer.time)).Set("enabled", JsonValue(layer.enabled))
            .Set("looping", JsonValue(layer.looping)).Set("additive", JsonValue(layer.additive))
            .Set("nodeMask", std::move(mask)));
    }
    return result.Set("layers", std::move(serializedLayers));
}

void AnimationManager::Deserialize(const JsonValue& value)
{
    Component::Deserialize(value);
    layers.clear();
    const JsonValue& serializedLayers = value["layers"];
    for (size_t i = 0; i < serializedLayers.ArraySize(); ++i)
    {
        const JsonValue& item = serializedLayers.ArrayAt(i);
        Layer layer;
        layer.clip = item["clip"].AsString();
        layer.weight = item["weight"].AsFloat();
        layer.speed = item["speed"].AsFloat();
        layer.time = item["time"].AsFloat();
        layer.enabled = !item.Has("enabled") || item["enabled"].AsBool();
        layer.looping = !item.Has("looping") || item["looping"].AsBool();
        layer.additive = item.Has("additive") && item["additive"].AsBool();
        for (size_t node = 0; node < item["nodeMask"].ArraySize(); ++node)
            layer.nodeMask.push_back(static_cast<unsigned>(item["nodeMask"].ArrayAt(node).AsInt()));
        layers.push_back(std::move(layer));
    }
}

bool AnimationManager::DrawProperties(::Engine::Editor::IEditorUi& ui)
{
    bool changed = Component::DrawProperties(ui);
    ui.Separator();
    ui.Label("Animation Layers");
    size_t removeIndex = layers.size();
    for (size_t index = 0; index < layers.size(); ++index)
    {
        Layer& layer = layers[index];
        ui.PushId(&layer);
        const std::string title = layer.clip.empty()
            ? "Layer " + std::to_string(index + 1) : layer.clip;
        if (ui.CollapsingHeader(title.c_str(), false))
        {
            char clipName[256]{};
            strncpy_s(clipName, sizeof(clipName), layer.clip.c_str(), _TRUNCATE);
            if (ui.InputText("Clip", clipName, sizeof(clipName)))
            {
                layer.clip = clipName;
                changed = true;
            }
            changed |= ui.Checkbox("Enabled", &layer.enabled);
            changed |= ui.Checkbox("Looping", &layer.looping);
            changed |= ui.Checkbox("Additive", &layer.additive);
            changed |= ui.DragFloat("Weight", &layer.weight, 0.01f, 0.f, 1.f);
            changed |= ui.DragFloat("Speed", &layer.speed, 0.05f, -10.f, 10.f);
            changed |= ui.DragFloat("Time", &layer.time, 0.01f, 0.f, 0.f);
            std::ostringstream maskText;
            for (size_t maskIndex = 0; maskIndex < layer.nodeMask.size(); ++maskIndex)
            {
                if (maskIndex) maskText << ',';
                maskText << layer.nodeMask[maskIndex];
            }
            char mask[512]{};
            strncpy_s(mask, sizeof(mask), maskText.str().c_str(), _TRUNCATE);
            if (ui.InputText("Node Mask", mask, sizeof(mask)))
            {
                layer.nodeMask.clear();
                std::istringstream input(mask);
                std::string token;
                while (std::getline(input, token, ','))
                    try { layer.nodeMask.push_back(static_cast<unsigned>(std::stoul(token))); }
                    catch (...) {}
                changed = true;
            }
            if (ui.Button("Remove Layer")) removeIndex = index;
        }
        ui.PopId();
    }
    if (removeIndex < layers.size())
    {
        layers.erase(layers.begin() + removeIndex);
        changed = true;
    }
    if (ui.Button("Add Layer"))
    {
        Layer layer;
        if (Animation* animation = FindAnimation(Owner, {}))
            layer.clip = animation->clipName;
        layers.push_back(std::move(layer));
        changed = true;
    }
    return changed;
}
}

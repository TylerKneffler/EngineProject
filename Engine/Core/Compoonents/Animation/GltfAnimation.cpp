#include "ModelAnimation.h"
#include "Core/Compoonents/Mesh.h"
#include "Core/Object.h"
#include "Engine/Editor/UI/IEditorUi.h"
#include <glm/gtc/quaternion.hpp>
#include <algorithm>
#include <cmath>
#include <functional>
#include <sstream>

namespace
{
JsonValue FloatArray(const std::vector<float>& values)
{
    JsonValue result = JsonValue::MakeArray();
    for (float value : values) result.Push(JsonValue(value));
    return result;
}

std::vector<float> ReadFloats(const JsonValue& value)
{
    std::vector<float> result;
    result.reserve(value.ArraySize());
    for (size_t i = 0; i < value.ArraySize(); ++i) result.push_back(value.ArrayAt(i).AsFloat());
    return result;
}

JsonValue Vec3Array(const std::vector<glm::vec3>& values)
{
    JsonValue result = JsonValue::MakeArray();
    for (const glm::vec3& value : values)
        result.Push(JsonValue::MakeArray().Push(JsonValue(value.x)).Push(JsonValue(value.y)).Push(JsonValue(value.z)));
    return result;
}

std::vector<glm::vec3> ReadVec3Array(const JsonValue& value)
{
    std::vector<glm::vec3> result;
    result.reserve(value.ArraySize());
    for (size_t i = 0; i < value.ArraySize(); ++i)
    {
        const JsonValue& item = value.ArrayAt(i);
        result.emplace_back(item.ArrayAt(0).AsFloat(), item.ArrayAt(1).AsFloat(), item.ArrayAt(2).AsFloat());
    }
    return result;
}

Object* Root(Object* object)
{
    while (object && object->Parent) object = object->Parent;
    return object;
}

Object* FindNodeRecursive(Object* object, unsigned index)
{
    if (!object) return nullptr;
    if (const auto* marker = object->GetComponent<ModelNode>(); marker && marker->nodeIndex == index)
        return object;
    for (Object* child : object->Children)
        if (Object* found = FindNodeRecursive(child, index)) return found;
    return nullptr;
}

glm::vec3 QuaternionEuler(const glm::quat& q)
{
    const float sinX = 2.f * (q.w * q.x + q.y * q.z);
    const float cosX = 1.f - 2.f * (q.x * q.x + q.y * q.y);
    const float sinY = std::clamp(2.f * (q.w * q.y - q.z * q.x), -1.f, 1.f);
    const float sinZ = 2.f * (q.w * q.z + q.x * q.y);
    const float cosZ = 1.f - 2.f * (q.y * q.y + q.z * q.z);
    return { std::atan2(sinX, cosX), std::asin(sinY), std::atan2(sinZ, cosZ) };
}

std::vector<float> Sample(const AnimationChannel& channel, float time)
{
    std::vector<float> result(channel.valueWidth, 0.f);
    if (channel.times.empty() || channel.valueWidth == 0) return result;
    const auto upper = std::upper_bound(channel.times.begin(), channel.times.end(), time);
    const size_t next = std::min<size_t>(upper - channel.times.begin(), channel.times.size() - 1);
    const size_t previous = next == 0 ? 0 : next - 1;
    const float span = channel.times[next] - channel.times[previous];
    float amount = span > 0.f ? (time - channel.times[previous]) / span : 0.f;
    amount = std::clamp(amount, 0.f, 1.f);
    if (channel.interpolation == AnimationChannel::Interpolation::Step) amount = 0.f;
    const size_t stride = channel.valueWidth *
        (channel.interpolation == AnimationChannel::Interpolation::CubicSpline ? 3u : 1u);
    const size_t valueOffset = channel.interpolation == AnimationChannel::Interpolation::CubicSpline
        ? channel.valueWidth : 0u;
    if ((next + 1) * stride > channel.values.size()) return result;
    if (channel.path == AnimationChannel::Path::Rotation && channel.valueWidth == 4 &&
        channel.interpolation == AnimationChannel::Interpolation::Linear)
    {
        const glm::quat a(channel.values[previous * stride + 3], channel.values[previous * stride],
            channel.values[previous * stride + 1], channel.values[previous * stride + 2]);
        const glm::quat b(channel.values[next * stride + 3], channel.values[next * stride],
            channel.values[next * stride + 1], channel.values[next * stride + 2]);
        const glm::quat q = glm::normalize(glm::slerp(a, b, amount));
        return { q.x, q.y, q.z, q.w };
    }
    for (size_t component = 0; component < channel.valueWidth; ++component)
    {
        const float a = channel.values[previous * stride + valueOffset + component];
        const float b = channel.values[next * stride + valueOffset + component];
        if (channel.interpolation != AnimationChannel::Interpolation::CubicSpline || previous == next)
            result[component] = a + (b - a) * amount;
        else
        {
            const float t2 = amount * amount, t3 = t2 * amount;
            const float outTangent = channel.values[previous * stride + valueOffset * 2 + component] * span;
            const float inTangent = channel.values[next * stride + component] * span;
            result[component] = (2*t3 - 3*t2 + 1)*a + (t3 - 2*t2 + amount)*outTangent +
                (-2*t3 + 3*t2)*b + (t3 - t2)*inTangent;
        }
    }
    return result;
}

struct NodePose
{
    bool hasTranslation = false, hasRotation = false, hasScale = false, hasWeights = false;
    glm::vec3 translation{};
    glm::quat rotation { 1.f, 0.f, 0.f, 0.f };
    glm::vec3 scale { 1.f };
    std::vector<float> weights;
};

using Pose = std::unordered_map<unsigned, NodePose>;

Animation* FindAnimation(Object* owner, const std::string& name)
{
    if (!owner) return nullptr;
    Animation* first = nullptr;
    for (Component* component : owner->Components)
        if (auto* animation = dynamic_cast<Animation*>(component))
        {
            if (!first) first = animation;
            if (!name.empty() && animation->clipName == name) return animation;
        }
    return name.empty() ? first : nullptr;
}

Pose SampleAnimation(const Animation* animation, float time)
{
    Pose pose;
    if (!animation) return pose;
    for (const AnimationChannel& channel : animation->channels)
    {
        const std::vector<float> sampled = Sample(channel, time);
        NodePose& node = pose[channel.nodeIndex];
        switch (channel.path)
        {
        case AnimationChannel::Path::Translation:
            if (sampled.size() >= 3) { node.hasTranslation = true; node.translation = { sampled[0], sampled[1], sampled[2] }; }
            break;
        case AnimationChannel::Path::Rotation:
            if (sampled.size() >= 4) { node.hasRotation = true; node.rotation = glm::normalize(glm::quat(sampled[3], sampled[0], sampled[1], sampled[2])); }
            break;
        case AnimationChannel::Path::Scale:
            if (sampled.size() >= 3) { node.hasScale = true; node.scale = { sampled[0], sampled[1], sampled[2] }; }
            break;
        case AnimationChannel::Path::Weights:
            node.hasWeights = true; node.weights = sampled; break;
        }
    }
    return pose;
}

glm::quat EulerQuaternion(const glm::vec3& euler)
{
    return glm::normalize(glm::angleAxis(euler.z, glm::vec3(0.f, 0.f, 1.f)) *
        glm::angleAxis(euler.y, glm::vec3(0.f, 1.f, 0.f)) *
        glm::angleAxis(euler.x, glm::vec3(1.f, 0.f, 0.f)));
}

bool IncludesNode(const AnimationManager::Layer& layer, unsigned node)
{
    return layer.nodeMask.empty() ||
        std::find(layer.nodeMask.begin(), layer.nodeMask.end(), node) != layer.nodeMask.end();
}

float AdvanceTime(float value, float delta, float speed, bool looping, const Animation* animation)
{
    value += delta * speed;
    if (!animation || animation->duration <= 0.f) return value;
    return looping ? std::fmod(std::max(value, 0.f), animation->duration)
                   : std::clamp(value, 0.f, animation->duration);
}
}

ModelNode::ModelNode()
{
    SetTypeName(COMPONENT_TYPE_NAME(ModelNode));
    RegisterField("nodeIndex", nodeIndex);
}

Animation::Animation() { SetTypeName(COMPONENT_TYPE_NAME(Animation)); }

JsonValue Animation::Serialize() const
{
    JsonValue value = JsonValue::MakeObject().Set("type", JsonValue(GetTypeName()))
        .Set("name", JsonValue(clipName)).Set("duration", JsonValue(duration));
    JsonValue serializedChannels = JsonValue::MakeArray();
    for (const auto& channel : channels)
    {
        serializedChannels.Push(JsonValue::MakeObject()
            .Set("node", JsonValue(static_cast<int>(channel.nodeIndex))).Set("path", JsonValue(static_cast<int>(channel.path)))
            .Set("interpolation", JsonValue(static_cast<int>(channel.interpolation)))
            .Set("width", JsonValue(static_cast<int>(channel.valueWidth))).Set("times", FloatArray(channel.times))
            .Set("values", FloatArray(channel.values)));
    }
    return value.Set("channels", std::move(serializedChannels));
}

void Animation::Deserialize(const JsonValue& value)
{
    clipName = value["name"].AsString();
    duration = value["duration"].AsFloat();
    channels.clear();
    const JsonValue& list = value["channels"];
    for (size_t i = 0; i < list.ArraySize(); ++i)
    {
        const JsonValue& item = list.ArrayAt(i);
        AnimationChannel channel;
        channel.nodeIndex = static_cast<unsigned>(item["node"].AsInt());
        channel.path = static_cast<AnimationChannel::Path>(item["path"].AsInt());
        channel.interpolation = static_cast<AnimationChannel::Interpolation>(item["interpolation"].AsInt());
        channel.valueWidth = static_cast<unsigned>(item["width"].AsInt());
        channel.times = ReadFloats(item["times"]); channel.values = ReadFloats(item["values"]);
        channels.push_back(std::move(channel));
    }
}

MorphTargets::MorphTargets() { SetTypeName(COMPONENT_TYPE_NAME(MorphTargets)); }

JsonValue MorphTargets::Serialize() const
{
    JsonValue result = JsonValue::MakeObject().Set("type", JsonValue(GetTypeName()))
        .Set("nodeIndex", JsonValue(static_cast<int>(nodeIndex))).Set("weights", FloatArray(weights));
    JsonValue list = JsonValue::MakeArray();
    for (const Target& target : targets)
        list.Push(JsonValue::MakeObject().Set("positions", Vec3Array(target.positions))
            .Set("normals", Vec3Array(target.normals)).Set("tangents", Vec3Array(target.tangents)));
    return result.Set("targets", std::move(list));
}

void MorphTargets::Deserialize(const JsonValue& value)
{
    nodeIndex = static_cast<unsigned>(value["nodeIndex"].AsInt());
    weights = ReadFloats(value["weights"]); targets.clear();
    const JsonValue& list = value["targets"];
    for (size_t i = 0; i < list.ArraySize(); ++i)
    {
        Target target;
        target.positions = ReadVec3Array(list.ArrayAt(i)["positions"]);
        target.normals = ReadVec3Array(list.ArrayAt(i)["normals"]);
        target.tangents = ReadVec3Array(list.ArrayAt(i)["tangents"]);
        targets.push_back(std::move(target));
    }
}

Skeleton::Skeleton()
{
    SetTypeName(COMPONENT_TYPE_NAME(Skeleton));
    RegisterField("hierarchyRootReference", hierarchyRootReference);
}

JsonValue Skeleton::Serialize() const
{
    JsonValue result = Component::Serialize().Set("skinIndex", JsonValue(static_cast<int>(skinIndex)));
    JsonValue nodes = JsonValue::MakeArray(), matrices = JsonValue::MakeArray();
    for (unsigned node : jointNodes) nodes.Push(JsonValue(static_cast<int>(node)));
    for (const glm::mat4& matrix : inverseBindMatrices)
    {
        JsonValue serialized = JsonValue::MakeArray();
        const float* data = &matrix[0][0];
        for (size_t i = 0; i < 16; ++i) serialized.Push(JsonValue(data[i]));
        matrices.Push(std::move(serialized));
    }
    return result.Set("joints", std::move(nodes)).Set("inverseBindMatrices", std::move(matrices));
}

void Skeleton::Deserialize(const JsonValue& value)
{
    Component::Deserialize(value);
    skinIndex = static_cast<unsigned>(value["skinIndex"].AsInt()); jointNodes.clear(); inverseBindMatrices.clear();
    for (size_t i = 0; i < value["joints"].ArraySize(); ++i)
        jointNodes.push_back(static_cast<unsigned>(value["joints"].ArrayAt(i).AsInt()));
    for (size_t i = 0; i < value["inverseBindMatrices"].ArraySize(); ++i)
    {
        glm::mat4 matrix(1.f); float* data = &matrix[0][0];
        for (size_t j = 0; j < 16; ++j) data[j] = value["inverseBindMatrices"].ArrayAt(i).ArrayAt(j).AsFloat();
        inverseBindMatrices.push_back(matrix);
    }
}

Object* Skeleton::FindNode(unsigned index) const
{
    ModelNode* rootMarker = hierarchyRootReference.IsAssigned()
        ? ResolveComponentReference<ModelNode>(Owner, hierarchyRootReference) : nullptr;
    return FindNodeRecursive(rootMarker ? rootMarker->Owner : Root(Owner), index);
}

SkinnedMesh::SkinnedMesh()
{
    SetTypeName(COMPONENT_TYPE_NAME(SkinnedMesh));
    RegisterField("meshReference", meshReference);
    RegisterField("morphTargetsReference", morphTargetsReference);
    RegisterField("skeletonReference", skeletonReference);
}
void SkinnedMesh::Start()
{
    Mesh* mesh = Owner ? (meshReference.IsAssigned()
        ? ResolveComponentReference<Mesh>(Owner, meshReference)
        : Owner->GetComponent<Mesh>()) : nullptr;
    if (!mesh) return;
    m_baseVertices = mesh->GetVertices();
    // Migrate prefabs imported by the CPU-skinning implementation: their
    // influences lived on this component rather than in the native mesh.
    if (joints.size() == m_baseVertices.size() && weights.size() == m_baseVertices.size())
    {
        bool changed = false;
        for (size_t vertexIndex = 0; vertexIndex < m_baseVertices.size(); ++vertexIndex)
        {
            float existingWeight = 0.f;
            for (size_t influence = 0; influence < 4; ++influence)
                existingWeight += m_baseVertices[vertexIndex].weights0[influence];
            if (existingWeight > 0.f) continue;
            for (size_t influence = 0; influence < 4; ++influence)
            {
                m_baseVertices[vertexIndex].joints0[influence] =
                    static_cast<float>(joints[vertexIndex][static_cast<glm::length_t>(influence)]);
                m_baseVertices[vertexIndex].weights0[influence] =
                    weights[vertexIndex][static_cast<glm::length_t>(influence)];
            }
            changed = true;
        }
        if (changed) mesh->SetDeformedVertices(m_baseVertices);
    }
}

void SkinnedMesh::Update()
{
    Mesh* mesh = Owner ? (meshReference.IsAssigned()
        ? ResolveComponentReference<Mesh>(Owner, meshReference)
        : Owner->GetComponent<Mesh>()) : nullptr;
    if (!mesh) return;
    MorphTargets* morphs = morphTargetsReference.IsAssigned()
        ? ResolveComponentReference<MorphTargets>(Owner, morphTargetsReference)
        : Owner->GetComponent<MorphTargets>();
    if (!morphs) return;
    if (m_baseVertices.size() != mesh->GetVertices().size()) m_baseVertices = mesh->GetVertices();
    std::vector<Vertex> deformed = m_baseVertices;
    if (morphs)
    {
        for (size_t targetIndex = 0; targetIndex < morphs->targets.size(); ++targetIndex)
        {
            const float weight = targetIndex < morphs->weights.size() ? morphs->weights[targetIndex] : 0.f;
            if (weight == 0.f) continue;
            const auto& target = morphs->targets[targetIndex];
            for (size_t i = 0; i < deformed.size(); ++i)
            {
                if (i < target.positions.size()) { deformed[i].pos[0] += target.positions[i].x * weight; deformed[i].pos[1] += target.positions[i].y * weight; deformed[i].pos[2] += target.positions[i].z * weight; }
                if (i < target.normals.size()) { deformed[i].normal[0] += target.normals[i].x * weight; deformed[i].normal[1] += target.normals[i].y * weight; deformed[i].normal[2] += target.normals[i].z * weight; }
                if (i < target.tangents.size()) { deformed[i].tangent[0] += target.tangents[i].x * weight; deformed[i].tangent[1] += target.tangents[i].y * weight; deformed[i].tangent[2] += target.tangents[i].z * weight; }
            }
        }
    }
    for (Vertex& vertex : deformed)
    {
        glm::vec3 normal(vertex.normal[0], vertex.normal[1], vertex.normal[2]);
        if (glm::dot(normal, normal) > 0.000001f)
        {
            normal = glm::normalize(normal);
            vertex.normal[0] = normal.x; vertex.normal[1] = normal.y; vertex.normal[2] = normal.z;
        }
        glm::vec3 tangent(vertex.tangent[0], vertex.tangent[1], vertex.tangent[2]);
        if (glm::dot(tangent, tangent) > 0.000001f)
        {
            tangent = glm::normalize(tangent);
            vertex.tangent[0] = tangent.x; vertex.tangent[1] = tangent.y; vertex.tangent[2] = tangent.z;
        }
    }
    mesh->SetDeformedVertices(deformed);
}

bool SkinnedMesh::BuildPalette(std::vector<glm::mat4>& palette) const
{
    palette.clear();
    if (!Owner || skinIndex < 0) return false;
    Skeleton* skeleton = skeletonReference.IsAssigned()
        ? ResolveComponentReference<Skeleton>(Owner, skeletonReference) : nullptr;
    if (!skeletonReference.IsAssigned())
        for (Object* ancestor = Owner; ancestor && !skeleton; ancestor = ancestor->Parent)
            for (Component* component : ancestor->Components)
                if (auto* candidate = dynamic_cast<Skeleton*>(component);
                    candidate && candidate->skinIndex == static_cast<unsigned>(skinIndex))
                { skeleton = candidate; break; }
    if (!skeleton) return false;
    palette.assign(skeleton->jointNodes.size(), glm::mat4(1.f));
    Mesh* mesh = meshReference.IsAssigned()
        ? ResolveComponentReference<Mesh>(Owner, meshReference)
        : Owner->GetComponent<Mesh>();
    Object* meshObject = mesh && mesh->Owner ? mesh->Owner : Owner;
    const glm::mat4 inverseMesh = glm::inverse(meshObject->transform.GetWorldMatrix());
    for (size_t i = 0; i < palette.size(); ++i)
        if (Object* joint = skeleton->FindNode(skeleton->jointNodes[i]))
            palette[i] = inverseMesh * joint->transform.GetWorldMatrix() *
                (i < skeleton->inverseBindMatrices.size()
                    ? skeleton->inverseBindMatrices[i] : glm::mat4(1.f));
    return !palette.empty();
}

JsonValue SkinnedMesh::Serialize() const
{
    JsonValue result = Component::Serialize().Set("skinIndex", JsonValue(skinIndex));
    JsonValue serializedJoints = JsonValue::MakeArray(), serializedWeights = JsonValue::MakeArray();
    for (size_t i = 0; i < joints.size(); ++i)
    {
        serializedJoints.Push(JsonValue::MakeArray().Push(JsonValue(static_cast<int>(joints[i].x))).Push(JsonValue(static_cast<int>(joints[i].y))).Push(JsonValue(static_cast<int>(joints[i].z))).Push(JsonValue(static_cast<int>(joints[i].w))));
        serializedWeights.Push(JsonValue::MakeArray().Push(JsonValue(weights[i].x)).Push(JsonValue(weights[i].y)).Push(JsonValue(weights[i].z)).Push(JsonValue(weights[i].w)));
    }
    return result.Set("joints", std::move(serializedJoints)).Set("weights", std::move(serializedWeights));
}

void SkinnedMesh::Deserialize(const JsonValue& value)
{
    Component::Deserialize(value);
    skinIndex = value["skinIndex"].AsInt(); joints.clear(); weights.clear();
    for (size_t i = 0; i < value["joints"].ArraySize(); ++i)
    {
        const JsonValue& j = value["joints"].ArrayAt(i); const JsonValue& w = value["weights"].ArrayAt(i);
        joints.emplace_back(j.ArrayAt(0).AsInt(), j.ArrayAt(1).AsInt(), j.ArrayAt(2).AsInt(), j.ArrayAt(3).AsInt());
        weights.emplace_back(w.ArrayAt(0).AsFloat(), w.ArrayAt(1).AsFloat(), w.ArrayAt(2).AsFloat(), w.ArrayAt(3).AsFloat());
    }
}

AnimationManager::AnimationManager()
{
    SetTypeName(COMPONENT_TYPE_NAME(AnimationManager));
    RegisterField("hierarchyRootReference", hierarchyRootReference);
    RegisterField("animationSourceReference", animationSourceReference);
    RegisterField("clip", clip); RegisterField("playing", playing); RegisterField("looping", looping);
    RegisterField("speed", speed); RegisterField("time", time);
}
void AnimationManager::Start()
{
    m_lastTick = std::chrono::steady_clock::now();
    m_restPose.clear(); m_restMorphs.clear();
    std::function<void(Object*)> capture = [&](Object* object)
    {
        if (const auto* marker = object->GetComponent<ModelNode>())
            m_restPose[marker->nodeIndex] = {
                object->transform.position, EulerQuaternion(object->transform.rotation),
                object->transform.scale };
        if (const auto* morphs = object->GetComponent<MorphTargets>())
            m_restMorphs.emplace(morphs->nodeIndex, morphs->weights);
        for (Object* child : object->Children) capture(child);
    };
    ModelNode* rootMarker = hierarchyRootReference.IsAssigned()
        ? ResolveComponentReference<ModelNode>(Owner, hierarchyRootReference) : nullptr;
    capture(rootMarker ? rootMarker->Owner : Root(Owner));
    if (clip.empty())
    {
        Animation* first = animationSourceReference.IsAssigned()
            ? ResolveComponentReference<Animation>(Owner, animationSourceReference)
            : FindAnimation(Owner, {});
        if (first) clip = first->clipName;
    }
}

void AnimationManager::Play(const std::string& clipName, float fadeSeconds)
{
    if (clipName.empty() || clipName == clip) return;
    if (!clip.empty() && fadeSeconds > 0.f)
    {
        m_previousClip = clip; m_previousTime = time;
        m_fadeDuration = fadeSeconds; m_fadeElapsed = 0.f;
    }
    else
    {
        m_previousClip.clear(); m_fadeDuration = m_fadeElapsed = 0.f;
    }
    clip = clipName; time = 0.f; playing = true;
}

void AnimationManager::Update()
{
    const auto now = std::chrono::steady_clock::now();
    if (m_lastTick.time_since_epoch().count() == 0) m_lastTick = now;
    const float frameDelta = std::min(std::chrono::duration<float>(now - m_lastTick).count(), 0.1f); m_lastTick = now;
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
            ? ResolveComponentReference<Animation>(Owner, animationSourceReference) : nullptr;
        if (animationSourceReference.IsAssigned())
            return assigned && (clipName.empty() || assigned->clipName == clipName)
                ? assigned : nullptr;
        return FindAnimation(Owner, clipName);
    };
    Animation* animation = resolveAnimation(clip);
    if (!animation) return;
    time = AdvanceTime(time, delta, speed, looping, animation);

    Pose basePose = SampleAnimation(animation, time);
    if (!m_previousClip.empty() && m_fadeDuration > 0.f)
    {
        Animation* previousAnimation = resolveAnimation(m_previousClip);
        m_previousTime = AdvanceTime(m_previousTime, delta, speed, looping, previousAnimation);
        m_fadeElapsed += delta;
        const float blend = std::clamp(m_fadeElapsed / m_fadeDuration, 0.f, 1.f);
        Pose previousPose = SampleAnimation(previousAnimation, m_previousTime);
        for (const auto& [nodeIndex, rest] : m_restPose)
        {
            NodePose& output = basePose[nodeIndex];
            const auto oldFound = previousPose.find(nodeIndex);
            const NodePose* old = oldFound == previousPose.end() ? nullptr : &oldFound->second;
            const glm::vec3 oldTranslation = old && old->hasTranslation ? old->translation : rest.translation;
            const glm::quat oldRotation = old && old->hasRotation ? old->rotation : rest.rotation;
            const glm::vec3 oldScale = old && old->hasScale ? old->scale : rest.scale;
            output.translation = glm::mix(oldTranslation, output.hasTranslation ? output.translation : rest.translation, blend);
            output.rotation = glm::normalize(glm::slerp(oldRotation, output.hasRotation ? output.rotation : rest.rotation, blend));
            output.scale = glm::mix(oldScale, output.hasScale ? output.scale : rest.scale, blend);
            output.hasTranslation = output.hasRotation = output.hasScale = true;
        }
        for (const auto& [nodeIndex, restWeights] : m_restMorphs)
        {
            NodePose& output = basePose[nodeIndex];
            const auto oldFound = previousPose.find(nodeIndex);
            const std::vector<float>& oldWeights = oldFound != previousPose.end() && oldFound->second.hasWeights
                ? oldFound->second.weights : restWeights;
            const std::vector<float> newWeights = output.hasWeights ? output.weights : restWeights;
            output.weights.resize(std::max(oldWeights.size(), newWeights.size()), 0.f);
            for (size_t i = 0; i < output.weights.size(); ++i)
            {
                const float oldValue = i < oldWeights.size() ? oldWeights[i] : 0.f;
                const float newValue = i < newWeights.size() ? newWeights[i] : 0.f;
                output.weights[i] = oldValue + (newValue - oldValue) * blend;
            }
            output.hasWeights = true;
        }
        if (blend >= 1.f) { m_previousClip.clear(); m_fadeDuration = m_fadeElapsed = 0.f; }
    }

    // Start at the rest pose so switching to a clip that omits a channel does
    // not leave stale values from the previous clip.
    Pose finalPose;
    for (const auto& [nodeIndex, rest] : m_restPose)
    {
        NodePose& result = finalPose[nodeIndex];
        result.hasTranslation = result.hasRotation = result.hasScale = true;
        result.translation = rest.translation; result.rotation = rest.rotation; result.scale = rest.scale;
        if (auto found = basePose.find(nodeIndex); found != basePose.end())
        {
            if (found->second.hasTranslation) result.translation = found->second.translation;
            if (found->second.hasRotation) result.rotation = found->second.rotation;
            if (found->second.hasScale) result.scale = found->second.scale;
        }
    }
    for (const auto& [nodeIndex, restWeights] : m_restMorphs)
    {
        NodePose& result = finalPose[nodeIndex]; result.hasWeights = true; result.weights = restWeights;
        if (auto found = basePose.find(nodeIndex); found != basePose.end() && found->second.hasWeights)
            result.weights = found->second.weights;
    }

    for (Layer& layer : layers)
    {
        if (!layer.enabled || layer.weight <= 0.f) continue;
        Animation* layerAnimation = resolveAnimation(layer.clip);
        if (!layerAnimation) continue;
        layer.time = AdvanceTime(layer.time, delta, layer.speed, layer.looping, layerAnimation);
        const Pose layerPose = SampleAnimation(layerAnimation, layer.time);
        const float layerWeight = std::clamp(layer.weight, 0.f, 1.f);
        for (const auto& [nodeIndex, sampled] : layerPose)
        {
            if (!IncludesNode(layer, nodeIndex)) continue;
            NodePose& result = finalPose[nodeIndex];
            const auto restFound = m_restPose.find(nodeIndex);
            if (restFound != m_restPose.end())
            {
                const RestTransform& rest = restFound->second;
                if (sampled.hasTranslation) result.translation = layer.additive
                    ? result.translation + (sampled.translation - rest.translation) * layerWeight
                    : glm::mix(result.translation, sampled.translation, layerWeight);
                if (sampled.hasRotation)
                {
                    if (layer.additive)
                    {
                        const glm::quat deltaRotation = sampled.rotation * glm::inverse(rest.rotation);
                        result.rotation = glm::normalize(result.rotation *
                            glm::slerp(glm::quat(1.f, 0.f, 0.f, 0.f), deltaRotation, layerWeight));
                    }
                    else result.rotation = glm::normalize(glm::slerp(result.rotation, sampled.rotation, layerWeight));
                }
                if (sampled.hasScale) result.scale = layer.additive
                    ? result.scale * glm::mix(glm::vec3(1.f), sampled.scale / glm::max(glm::abs(rest.scale), glm::vec3(0.00001f)), layerWeight)
                    : glm::mix(result.scale, sampled.scale, layerWeight);
            }
            if (sampled.hasWeights)
            {
                const auto restWeights = m_restMorphs.find(nodeIndex);
                result.weights.resize(std::max(result.weights.size(), sampled.weights.size()), 0.f);
                for (size_t i = 0; i < result.weights.size(); ++i)
                {
                    const float sampledValue = i < sampled.weights.size() ? sampled.weights[i] : 0.f;
                    const float restValue = restWeights != m_restMorphs.end() && i < restWeights->second.size() ? restWeights->second[i] : 0.f;
                    result.weights[i] = layer.additive
                        ? result.weights[i] + (sampledValue - restValue) * layerWeight
                        : result.weights[i] + (sampledValue - result.weights[i]) * layerWeight;
                }
                result.hasWeights = true;
            }
        }
    }

    ModelNode* rootMarker = hierarchyRootReference.IsAssigned()
        ? ResolveComponentReference<ModelNode>(Owner, hierarchyRootReference) : nullptr;
    Object* hierarchyRoot = rootMarker ? rootMarker->Owner : Root(Owner);
    for (const auto& [nodeIndex, pose] : finalPose)
        if (Object* target = FindNodeRecursive(hierarchyRoot, nodeIndex))
        {
            if (pose.hasTranslation) target->transform.position = pose.translation;
            if (pose.hasRotation) target->transform.rotation = QuaternionEuler(pose.rotation);
            if (pose.hasScale) target->transform.scale = pose.scale;
        }
    std::function<void(Object*)> applyMorphs = [&](Object* object)
    {
        if (auto* morphs = object->GetComponent<MorphTargets>())
            if (auto found = finalPose.find(morphs->nodeIndex); found != finalPose.end() && found->second.hasWeights)
                morphs->weights = found->second.weights;
        for (Object* child : object->Children) applyMorphs(child);
    };
    applyMorphs(hierarchyRoot);
}

JsonValue AnimationManager::Serialize() const
{
    JsonValue result = Component::Serialize();
    JsonValue serializedLayers = JsonValue::MakeArray();
    for (const Layer& layer : layers)
    {
        JsonValue mask = JsonValue::MakeArray();
        for (unsigned node : layer.nodeMask) mask.Push(JsonValue(static_cast<int>(node)));
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
    Component::Deserialize(value); layers.clear();
    const JsonValue& serializedLayers = value["layers"];
    for (size_t i = 0; i < serializedLayers.ArraySize(); ++i)
    {
        const JsonValue& item = serializedLayers.ArrayAt(i); Layer layer;
        layer.clip = item["clip"].AsString(); layer.weight = item["weight"].AsFloat();
        layer.speed = item["speed"].AsFloat(); layer.time = item["time"].AsFloat();
        layer.enabled = !item.Has("enabled") || item["enabled"].AsBool();
        layer.looping = !item.Has("looping") || item["looping"].AsBool();
        layer.additive = item.Has("additive") && item["additive"].AsBool();
        for (size_t node = 0; node < item["nodeMask"].ArraySize(); ++node)
            layer.nodeMask.push_back(static_cast<unsigned>(item["nodeMask"].ArrayAt(node).AsInt()));
        layers.push_back(std::move(layer));
    }
}

bool AnimationManager::DrawProperties(IEditorUi& ui)
{
    bool changed = Component::DrawProperties(ui);
    ui.Separator(); ui.Label("Animation Layers");
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
            if (ui.InputText("Clip", clipName, sizeof(clipName))) { layer.clip = clipName; changed = true; }
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
                layer.nodeMask.clear(); std::istringstream input(mask); std::string token;
                while (std::getline(input, token, ','))
                    try { layer.nodeMask.push_back(static_cast<unsigned>(std::stoul(token))); }
                    catch (...) {}
                changed = true;
            }
            if (ui.Button("Remove Layer")) removeIndex = index;
        }
        ui.PopId();
    }
    if (removeIndex < layers.size()) { layers.erase(layers.begin() + removeIndex); changed = true; }
    if (ui.Button("Add Layer"))
    {
        Layer layer;
        if (Animation* animation = FindAnimation(Owner, {})) layer.clip = animation->clipName;
        layers.push_back(std::move(layer)); changed = true;
    }
    return changed;
}

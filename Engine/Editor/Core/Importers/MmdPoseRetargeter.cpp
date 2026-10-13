#include "Editor/Core/Importers/MmdPoseRetargeter.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/matrix_decompose.hpp>
#include <unordered_map>

namespace Engine::Editor
{
namespace
{
glm::vec3 Position(const Serialization::JsonValue& array)
{
    return { array.ArrayAt(0).AsFloat(), array.ArrayAt(1).AsFloat(),
        array.ArrayAt(2).AsFloat() };
}

glm::mat4 Local(const Model::ImportedNode& node)
{
    return glm::translate(glm::mat4(1.f), node.translation) *
        glm::mat4_cast(node.rotation) * glm::scale(glm::mat4(1.f), node.scale);
}

bool DirectJoint(const std::string& role)
{
    return role == "joint" || role == "shoulder-parent";
}

bool UnrepresentedHelper(const std::string& role)
{
    return role == "twist" || role == "distributed-twist" ||
        role == "waist-cancel" || role == "deform-helper" ||
        role == "toe-helper" || role == "head-helper";
}

bool NonNeutral(const VmdBoneFrame& key)
{
    const glm::vec3 translation(key.translation[0], key.translation[1], key.translation[2]);
    const glm::vec3 imaginary(key.rotation[0], key.rotation[1], key.rotation[2]);
    return glm::length(translation) > 1e-4f ||
        glm::length(imaginary) > 1e-3f ||
        std::abs(std::abs(key.rotation[3]) - 1.f) > 1e-4f;
}

std::string SafeObjectName(std::string name, const std::string& fallback)
{
    if (name.empty()) name = fallback;
    for (char& c : name)
    {
        const unsigned char value = static_cast<unsigned char>(c);
        if (value < 32 || c == '<' || c == '>' || c == ':' || c == '"' ||
            c == '/' || c == '\\' || c == '|' || c == '?' || c == '*') c = '_';
    }
    while (!name.empty() && (name.back() == ' ' || name.back() == '.'))
        name.pop_back();
    return name.empty() ? fallback : name;
}

std::string ExpressionName(const std::string& name)
{
    const size_t separator = name.rfind('.');
    if (separator != std::string::npos && separator * 2 + 1 == name.size() &&
        name.compare(0, separator, name, separator + 1,
            separator) == 0)
        return name.substr(0, separator);
    return name;
}

struct KeyPose
{
    glm::vec3 translation { 0.f };
    glm::quat rotation { 1.f, 0.f, 0.f, 0.f };
};

float Bezier(float frameFraction, const std::array<uint8_t, 64>& bytes,
    size_t channel)
{
    const auto byte = [&](size_t row) -> float
    {
        size_t index = row * 4 + channel;
        // Some VMD writers put per-key physics flags over Z/R control bytes.
        if (row == 0 && (channel == 2 || channel == 3) &&
            bytes[2] == 99 && bytes[3] == 15)
            index = channel == 2 ? 17 : 18;
        return std::clamp(static_cast<float>(bytes[index]) / 127.f, 0.f, 1.f);
    };
    const float x1 = byte(0), y1 = byte(1), x2 = byte(2), y2 = byte(3);
    if (x1 == 0.f && y1 == 0.f && x2 == 0.f && y2 == 0.f)
        return frameFraction; // synthetic or absent interpolation data
    const auto cubic = [](float t, float a, float b)
    {
        const float u = 1.f - t;
        return 3.f * u * u * t * a + 3.f * u * t * t * b + t * t * t;
    };
    float low = 0.f, high = 1.f;
    for (int i = 0; i < 18; ++i)
    {
        const float mid = (low + high) * 0.5f;
        if (cubic(mid, x1, x2) < frameFraction) low = mid;
        else high = mid;
    }
    return cubic((low + high) * 0.5f, y1, y2);
}

KeyPose SampleKey(const std::vector<VmdBoneFrame>& keys, float frame)
{
    if (keys.empty()) return {};
    const auto upper = std::lower_bound(keys.begin(), keys.end(), frame,
        [](const VmdBoneFrame& key, float value) { return key.frame < value; });
    const VmdBoneFrame& before = upper == keys.begin() ? keys.front() : *(upper - 1);
    const VmdBoneFrame& after = upper == keys.end() ? keys.back() : *upper;
    const float span = static_cast<float>(after.frame) - before.frame;
    const float t = span <= 0.f ? 0.f :
        std::clamp((frame - before.frame) / span, 0.f, 1.f);
    const glm::vec3 a(before.translation[0], before.translation[1], before.translation[2]);
    const glm::vec3 b(after.translation[0], after.translation[1], after.translation[2]);
    const glm::quat qa(before.rotation[3], before.rotation[0], before.rotation[1], before.rotation[2]);
    const glm::quat qb(after.rotation[3], after.rotation[0], after.rotation[1], after.rotation[2]);
    const auto& curve = after.interpolation;
    const glm::vec3 position(
        glm::mix(a.x, b.x, Bezier(t, curve, 0)),
        glm::mix(a.y, b.y, Bezier(t, curve, 1)),
        glm::mix(a.z, b.z, Bezier(t, curve, 2)));
    return { position, glm::normalize(glm::slerp(qa, qb,
        Bezier(t, curve, 3))) };
}

glm::quat WeightedRotation(glm::quat source, float weight)
{
    if (weight < 0.f) source = glm::inverse(source);
    return glm::normalize(glm::slerp(glm::quat(1.f, 0.f, 0.f, 0.f),
        source, std::abs(weight)));
}

glm::quat FromTo(glm::vec3 from, glm::vec3 to)
{
    from = glm::normalize(from);
    to = glm::normalize(to);
    const float dot = std::clamp(glm::dot(from, to), -1.f, 1.f);
    if (dot > 1.f - 1e-6f) return glm::quat(1.f, 0.f, 0.f, 0.f);
    glm::vec3 axis = glm::cross(from, to);
    if (glm::dot(axis, axis) < 1e-10f)
        axis = glm::cross(from, std::abs(from.y) < 0.9f
            ? glm::vec3(0.f, 1.f, 0.f) : glm::vec3(1.f, 0.f, 0.f));
    return glm::angleAxis(std::acos(dot), glm::normalize(axis));
}
}

bool MmdPoseRetargeter::Initialize(const Model::ImportedModel& target,
    const Serialization::JsonValue& profile,
    const Serialization::JsonValue& mapping,
    const VmdMotion& motion, std::string& error)
{
    error.clear();
    const auto& bones = profile["bones"];
    const auto& entries = mapping["bones"];
    const auto& root = mapping["rootMotion"];
    const auto& coordinates = mapping["coordinates"];
    if (bones.ArraySize() == 0 || bones.ArraySize() != entries.ArraySize() ||
        coordinates["axisSigns"].ArraySize() != 3 ||
        root["mode"].AsString() != "scene-travel" ||
        root["composePmxIndicesInOrder"].ArraySize() != 5)
    { error = "Unsupported or incomplete MMD retarget map"; return false; }
    m_target = target.nodes;
    m_facialMeshes.clear();
    for (size_t node = 0; node < target.nodes.size(); ++node)
    {
        const auto& owner = target.nodes[node];
        std::unordered_map<std::string, unsigned> primitiveNameCounts;
        for (size_t slot = 0; slot < owner.primitives.size(); ++slot)
        {
            const unsigned primitiveIndex = owner.primitives[slot];
            if (primitiveIndex >= target.primitives.size()) continue;
            const auto& primitive = target.primitives[primitiveIndex];
            std::string objectName = SafeObjectName(primitive.name,
                "Primitive " + std::to_string(slot + 1));
            const unsigned sameName = ++primitiveNameCounts[objectName];
            if (sameName > 1) objectName += " " + std::to_string(sameName);
            if (primitive.morphTargets.empty()) continue;
            FacialMesh mesh;
            mesh.ownerNode = static_cast<unsigned>(node);
            mesh.objectName = std::move(objectName);
            mesh.childObject = owner.primitives.size() > 1;
            mesh.defaults = primitive.morphWeights;
            for (const auto& morph : primitive.morphTargets)
                mesh.targetNames.push_back(morph.name);
            m_facialMeshes.push_back(std::move(mesh));
        }
    }
    m_rootSource = root["composePmxIndicesInOrder"].ArrayAt(4).AsInt();
    m_rootTarget = root["targetNode"].AsInt();
    if (m_rootSource < 0 || m_rootSource >= static_cast<int>(bones.ArraySize()) ||
        m_rootTarget < 0 || m_rootTarget >= static_cast<int>(m_target.size()))
    { error = "Invalid MMD root mapping"; return false; }
    const float scale = coordinates["pmxToFbxWorldScale"].AsFloat();
    if (!(scale > 0.f)) { error = "Invalid MMD coordinate scale"; return false; }
    m_axis = glm::mat4(1.f);
    for (int i = 0; i < 3; ++i)
        m_axis[i][i] = scale * coordinates["axisSigns"].ArrayAt(i).AsFloat();
    m_inverseAxis = glm::inverse(m_axis);

    std::unordered_map<std::string, int> names;
    m_source.assign(bones.ArraySize(), SourceBone{});
    for (size_t i = 0; i < m_source.size(); ++i)
    {
        const auto& bone = bones.ArrayAt(i);
        const auto& entry = entries.ArrayAt(i);
        auto& out = m_source[i];
        out.parent = bone["parent"].AsInt();
        out.target = entry["fbxNode"].AsInt();
        out.name = bone["name"].AsString();
        out.role = entry["role"].AsString();
        out.rest = Position(bone["position"]);
        if (bone.Has("inherit"))
        {
            const auto& inherited = bone["inherit"];
            out.inheritSource = inherited["bone"].AsInt();
            out.inheritWeight = inherited["weight"].AsFloat();
            out.inheritRotation = inherited["rotation"].AsBool();
            out.inheritTranslation = inherited["translation"].AsBool();
            if (out.inheritSource < 0 || out.inheritSource >= static_cast<int>(i) ||
                !std::isfinite(out.inheritWeight))
            { error = "Invalid PMX inherited transform"; return false; }
        }
        if (out.parent >= static_cast<int>(i) || out.parent < -1 ||
            out.target < 0 || out.target >= static_cast<int>(m_target.size()) ||
            entry["pmxIndex"].AsInt() != static_cast<int>(i) ||
            entry["fbxName"].AsString() != m_target[out.target].name)
        { error = "Stale or non-topological MMD bone map"; return false; }
        names.emplace(bone["name"].AsString(), static_cast<int>(i));
    }
    m_ik.clear();
    for (size_t i = 0; i < bones.ArraySize(); ++i)
    {
        const auto& ik = bones.ArrayAt(i)["ik"];
        if (ik.IsNull()) continue;
        IkChain chain;
        chain.controller = static_cast<int>(i);
        chain.tip = ik["target"].AsInt();
        chain.iterations = ik["iterations"].AsInt();
        chain.stepRadians = ik["angleLimitRadians"].AsFloat();
        if (chain.tip < 0 || chain.tip >= static_cast<int>(m_source.size()) ||
            chain.iterations < 1 || chain.iterations > 1000 ||
            !std::isfinite(chain.stepRadians) || chain.stepRadians <= 0.f)
        { error = "Invalid PMX IK chain"; return false; }
        const auto& links = ik["links"];
        for (size_t linkIndex = 0; linkIndex < links.ArraySize(); ++linkIndex)
        {
            const auto& item = links.ArrayAt(linkIndex);
            IkLink link;
            link.bone = item["bone"].AsInt();
            link.limited = item.Has("minimum") && item.Has("maximum");
            if (link.limited)
            {
                link.minimum = Position(item["minimum"]);
                link.maximum = Position(item["maximum"]);
            }
            if (link.bone < 0 || link.bone >= static_cast<int>(m_source.size()) ||
                (link.limited && (glm::any(glm::greaterThan(link.minimum, link.maximum)))))
            { error = "Invalid PMX IK link"; return false; }
            chain.links.push_back(link);
        }
        if (chain.links.empty()) { error = "Empty PMX IK chain"; return false; }
        m_ik.push_back(std::move(chain));
    }
    m_properties = motion.properties;
    m_morphs = motion.morphs;
    std::sort(m_properties.begin(), m_properties.end(),
        [](const VmdPropertyFrame& a, const VmdPropertyFrame& b)
        { return a.frame < b.frame; });
    const auto& aliases = mapping["motionAliases"];
    for (size_t i = 0; i < aliases.ArraySize(); ++i)
        names[aliases.ArrayAt(i)["vmdName"].AsString()] = aliases.ArrayAt(i)["pmxIndex"].AsInt();
    for (const auto& key : motion.bones)
    {
        const auto found = names.find(key.name);
        if (found != names.end() && found->second >= 0 &&
            found->second < static_cast<int>(m_source.size()))
            m_source[found->second].keys.push_back(key);
    }
    for (auto& bone : m_source)
    {
        std::sort(bone.keys.begin(), bone.keys.end(),
            [](const VmdBoneFrame& a, const VmdBoneFrame& b) { return a.frame < b.frame; });
        if (UnrepresentedHelper(bone.role) &&
            std::any_of(bone.keys.begin(), bone.keys.end(), NonNeutral))
        { error = "Animated PMX helper has no distinct FBX target: " + bone.role; return false; }
    }

    m_restLocal.resize(m_target.size());
    m_restWorld.resize(m_target.size());
    for (size_t i = 0; i < m_target.size(); ++i)
    {
        const int parent = m_target[i].parent;
        if (parent >= static_cast<int>(i) || parent < -1)
        { error = "Non-topological FBX hierarchy"; return false; }
        m_restLocal[i] = Local(m_target[i]);
        m_restWorld[i] = parent < 0 ? m_restLocal[i] :
            m_restWorld[parent] * m_restLocal[i];
    }
    return true;
}

MmdPoseRetargeter::Pose MmdPoseRetargeter::Sample(float frame) const
{
    Pose result;
    if (m_source.empty()) return result;
    result.sourceWorld.resize(m_source.size(), glm::mat4(1.f));
    std::vector<KeyPose> raw(m_source.size()), applied(m_source.size());
    std::vector<glm::quat> ikOffset(m_source.size(), glm::quat(1.f, 0.f, 0.f, 0.f));
    for (size_t i = 0; i < m_source.size(); ++i)
        raw[i] = SampleKey(m_source[i].keys, frame);
    const auto rebuildSource = [&]()
    {
        for (size_t i = 0; i < m_source.size(); ++i)
        {
            const auto& bone = m_source[i];
            KeyPose pose = raw[i];
            if (bone.inheritSource >= 0)
            {
                const auto& inherited = applied[bone.inheritSource];
                if (bone.inheritTranslation)
                    pose.translation += inherited.translation * bone.inheritWeight;
                if (bone.inheritRotation)
                    pose.rotation = glm::normalize(pose.rotation *
                        WeightedRotation(inherited.rotation, bone.inheritWeight));
            }
            pose.rotation = glm::normalize(pose.rotation * ikOffset[i]);
            applied[i] = pose;
            const glm::vec3 parentRest = bone.parent < 0 ? glm::vec3(0.f) :
                m_source[bone.parent].rest;
            const glm::mat4 local = glm::translate(glm::mat4(1.f),
                bone.rest - parentRest + pose.translation) * glm::mat4_cast(pose.rotation);
            result.sourceWorld[i] = bone.parent < 0 ? local :
                result.sourceWorld[bone.parent] * local;
        }
    };
    rebuildSource();
    const VmdPropertyFrame* switches = nullptr;
    for (const auto& property : m_properties)
        if (static_cast<float>(property.frame) <= frame) switches = &property;
        else break;
    for (const auto& chain : m_ik)
    {
        bool enabled = true;
        if (switches)
            for (const auto& setting : switches->ik)
                if (setting.name == m_source[chain.controller].name)
                    enabled = setting.enabled;
        if (!enabled)
        {
            result.ikError.push_back(0.f);
            result.ikTargetDrift.push_back(0.f);
            result.ikOriginalError.push_back(0.f);
            continue;
        }
        const glm::vec3 originalGoal(result.sourceWorld[chain.controller][3]);
        glm::vec3 goal = originalGoal;
        // Retain model travel while fitting each control to the reach of its
        // actual PMX chain. The discarded distance is reported to the caller.
        const int rootLink = chain.links.back().bone;
        const glm::vec3 rootPosition(result.sourceWorld[rootLink][3]);
        float maximumReach = 0.f;
        int previous = rootLink;
        for (auto it = chain.links.rbegin() + 1; it != chain.links.rend(); ++it)
        {
            maximumReach += glm::distance(
                glm::vec3(result.sourceWorld[previous][3]),
                glm::vec3(result.sourceWorld[it->bone][3]));
            previous = it->bone;
        }
        maximumReach += glm::distance(
            glm::vec3(result.sourceWorld[previous][3]),
            glm::vec3(result.sourceWorld[chain.tip][3]));
        float minimumReach = 0.f;
        if (chain.links.size() == 1) minimumReach = maximumReach;
        else if (chain.links.size() == 2)
        {
            const float first = glm::distance(rootPosition,
                glm::vec3(result.sourceWorld[chain.links.front().bone][3]));
            const float second = maximumReach - first;
            minimumReach = std::abs(first - second);
        }
        const glm::vec3 toGoal = originalGoal - rootPosition;
        const float distance = glm::length(toGoal);
        glm::vec3 direction = distance > 1e-6f ? toGoal / distance :
            glm::normalize(glm::vec3(result.sourceWorld[chain.tip][3]) - rootPosition);
        const float reachableDistance = chain.links.size() == 1
            ? maximumReach : std::clamp(distance,
                minimumReach + 0.001f, maximumReach - 0.001f);
        goal = rootPosition + direction * reachableDistance;
        result.ikTargetDrift.push_back(glm::distance(goal, originalGoal));
        // A one-link toe control can need several bounded corrections after
        // its target is projected onto a new reach sphere.
        const int solveIterations = chain.links.size() == 1
            ? std::max(chain.iterations, 16) : chain.iterations;
        for (int iteration = 0; iteration < solveIterations; ++iteration)
        {
            const float currentError = glm::distance(
                glm::vec3(result.sourceWorld[chain.tip][3]), goal);
            if (currentError < 1e-3f) break;
            for (const auto& link : chain.links)
            {
                const glm::vec3 pivot(result.sourceWorld[link.bone][3]);
                const glm::vec3 toTip = glm::vec3(result.sourceWorld[chain.tip][3]) - pivot;
                const glm::vec3 toLinkGoal = goal - pivot;
                if (glm::length(toTip) < 1e-6f || glm::length(toLinkGoal) < 1e-6f)
                    continue;
                glm::quat delta = FromTo(toTip, toLinkGoal);
                const float angle = glm::angle(delta);
                if (angle > chain.stepRadians)
                    delta = glm::slerp(glm::quat(1.f, 0.f, 0.f, 0.f), delta,
                        chain.stepRadians / angle);
                const int parent = m_source[link.bone].parent;
                const glm::quat parentWorld = parent < 0
                    ? glm::quat(1.f, 0.f, 0.f, 0.f)
                    : glm::normalize(glm::quat_cast(glm::mat3(result.sourceWorld[parent])));
                glm::quat local = glm::normalize(glm::inverse(parentWorld) *
                    delta * parentWorld * applied[link.bone].rotation);
                if (link.limited)
                {
                    const glm::vec3 euler = glm::eulerAngles(local);
                    local = glm::quat(glm::clamp(euler, link.minimum, link.maximum));
                }
                const glm::quat base = applied[link.bone].rotation *
                    glm::inverse(ikOffset[link.bone]);
                ikOffset[link.bone] = glm::normalize(glm::inverse(base) * local);
                rebuildSource();
            }
        }
        const glm::vec3 solved(result.sourceWorld[chain.tip][3]);
        result.ikError.push_back(glm::distance(solved, goal));
        result.ikOriginalError.push_back(glm::distance(solved, originalGoal));
    }
    std::vector<int> sourceForNode(m_target.size(), -1);
    sourceForNode[m_rootTarget] = m_rootSource;
    for (size_t i = 0; i < m_source.size(); ++i)
    {
        const auto& bone = m_source[i];
        if (bone.target != m_rootTarget && DirectJoint(bone.role))
            sourceForNode[bone.target] = static_cast<int>(i);
    }
    result.local.resize(m_target.size());
    result.world.resize(m_target.size());
    for (size_t i = 0; i < m_target.size(); ++i)
    {
        const int parent = m_target[i].parent;
        const int source = sourceForNode[i];
        if (source < 0)
            result.local[i] = m_restLocal[i];
        else
        {
            const glm::mat4 sourceRest = glm::translate(glm::mat4(1.f),
                m_source[source].rest);
            const glm::mat4 sourceDelta = result.sourceWorld[source] * glm::inverse(sourceRest);
            const glm::mat4 desiredWorld = m_axis * sourceDelta * m_inverseAxis *
                m_restWorld[i];
            result.local[i] = parent < 0 ? desiredWorld :
                glm::inverse(result.world[parent]) * desiredWorld;
        }
        result.world[i] = parent < 0 ? result.local[i] :
            result.world[parent] * result.local[i];
    }
    return result;
}

Model::AnimationClip MmdPoseRetargeter::BakeBody(unsigned lastFrame,
    const std::string& clipName) const
{
    Model::AnimationClip clip;
    clip.clipName = clipName;
    clip.duration = static_cast<float>(lastFrame) / 30.f;
    clip.looping = false;
    for (const auto& property : m_properties)
        if (property.frame <= lastFrame)
            clip.visibility.push_back({
                static_cast<float>(property.frame) / 30.f,
                property.visible });
    if (m_target.empty()) return clip;

    std::vector<std::string> paths(m_target.size());
    for (size_t node = 0; node < m_target.size(); ++node)
    {
        const int parent = m_target[node].parent;
        const std::string name = SafeObjectName(m_target[node].name,
            "Node " + std::to_string(node + 1));
        paths[node] = parent < 0 ? name : paths[parent] + "/" + name;
    }
    // Four samples per VMD frame preserve curved keys and solved IK while
    // remaining playable by the engine's ordinary linear channel sampler.
    constexpr unsigned samplesPerFrame = 4;
    const size_t sampleCount = static_cast<size_t>(lastFrame) *
        samplesPerFrame + 1;
    clip.channels.reserve(m_target.size() * 3);
    for (size_t node = 0; node < m_target.size(); ++node)
        for (const auto path : { Model::AnimationChannel::Path::Translation,
            Model::AnimationChannel::Path::Rotation,
            Model::AnimationChannel::Path::Scale })
        {
            Model::AnimationChannel channel;
            channel.nodeIndex = static_cast<unsigned>(node);
            channel.targetPath = paths[node];
            channel.path = path;
            channel.valueWidth = path == Model::AnimationChannel::Path::Rotation
                ? 4u : 3u;
            channel.times.reserve(sampleCount);
            channel.values.reserve(sampleCount * channel.valueWidth);
            clip.channels.push_back(std::move(channel));
        }
    for (size_t sample = 0; sample < sampleCount; ++sample)
    {
        const float frame = std::min(static_cast<float>(lastFrame),
            static_cast<float>(sample) / samplesPerFrame);
        const float time = frame / 30.f;
        const Pose pose = Sample(frame);
        for (size_t node = 0; node < m_target.size(); ++node)
        {
            glm::vec3 scale, position, skew;
            glm::vec4 perspective;
            glm::quat rotation;
            if (!glm::decompose(pose.local[node], scale, rotation,
                    position, skew, perspective))
                continue;
            rotation = glm::normalize(rotation);
            auto& translation = clip.channels[node * 3];
            auto& orientation = clip.channels[node * 3 + 1];
            auto& scaling = clip.channels[node * 3 + 2];
            translation.times.push_back(time);
            translation.values.insert(translation.values.end(),
                { position.x, position.y, position.z });
            orientation.times.push_back(time);
            // Keep quaternion signs continuous for interpolation and export.
            if (!orientation.values.empty())
            {
                const size_t offset = orientation.values.size() - 4;
                const glm::quat previous(orientation.values[offset + 3],
                    orientation.values[offset], orientation.values[offset + 1],
                    orientation.values[offset + 2]);
                if (glm::dot(previous, rotation) < 0.f) rotation = -rotation;
            }
            orientation.values.insert(orientation.values.end(),
                { rotation.x, rotation.y, rotation.z, rotation.w });
            scaling.times.push_back(time);
            scaling.values.insert(scaling.values.end(),
                { scale.x, scale.y, scale.z });
        }
    }
    clip.channels.erase(std::remove_if(clip.channels.begin(),
        clip.channels.end(), [this](const Model::AnimationChannel& channel)
        {
            if (channel.times.empty()) return true;
            const size_t width = channel.valueWidth;
            for (size_t offset = width; offset < channel.values.size();
                offset += width)
                for (size_t i = 0; i < width; ++i)
                    if (std::abs(channel.values[offset + i] -
                        channel.values[i]) > 1e-5f)
                        return false;
            const auto& rest = m_target[channel.nodeIndex];
            if (channel.path == Model::AnimationChannel::Path::Rotation)
            {
                const glm::quat first(channel.values[3], channel.values[0],
                    channel.values[1], channel.values[2]);
                return std::abs(glm::dot(first, rest.rotation)) > 1.f - 1e-5f;
            }
            const glm::vec3 first(channel.values[0], channel.values[1],
                channel.values[2]);
            return glm::distance(first,
                channel.path == Model::AnimationChannel::Path::Translation
                    ? rest.translation : rest.scale) < 1e-5f;
        }), clip.channels.end());
    return clip;
}

MmdPoseRetargeter::FacialBake MmdPoseRetargeter::BakeFacial(
    unsigned lastFrame, const std::string& clipName) const
{
    FacialBake result;
    result.clip.clipName = clipName;
    result.clip.duration = static_cast<float>(lastFrame) / 30.f;
    result.clip.looping = false;
    std::map<std::string, std::map<unsigned, float>> sourceKeys;
    std::set<unsigned> frames { 0u, lastFrame };
    for (const auto& key : m_morphs)
    {
        if (key.frame > lastFrame) continue;
        sourceKeys[key.name][key.frame] = key.weight;
        frames.insert(key.frame);
    }
    std::vector<std::string> nodePaths(m_target.size());
    for (size_t node = 0; node < m_target.size(); ++node)
    {
        const int parent = m_target[node].parent;
        const std::string name = SafeObjectName(m_target[node].name,
            "Node " + std::to_string(node + 1));
        nodePaths[node] = parent < 0 ? name : nodePaths[parent] + "/" + name;
    }
    std::set<std::string> matched;
    for (const FacialMesh& mesh : m_facialMeshes)
    {
        Model::AnimationChannel channel;
        channel.nodeIndex = mesh.ownerNode;
        channel.targetPath = nodePaths[mesh.ownerNode];
        channel.targetMesh = channel.targetPath;
        if (mesh.childObject)
            channel.targetMesh += "/" + mesh.objectName;
        channel.path = Model::AnimationChannel::Path::Weights;
        channel.valueWidth = static_cast<unsigned>(mesh.targetNames.size());
        channel.times.reserve(frames.size());
        channel.values.reserve(frames.size() * mesh.targetNames.size());
        for (unsigned frame : frames)
        {
            channel.times.push_back(static_cast<float>(frame) / 30.f);
            for (size_t index = 0; index < mesh.targetNames.size(); ++index)
            {
                const std::string name = ExpressionName(mesh.targetNames[index]);
                const auto source = sourceKeys.find(name);
                float weight = index < mesh.defaults.size() ? mesh.defaults[index] : 0.f;
                if (source != sourceKeys.end() && !source->second.empty())
                {
                    matched.insert(name);
                    ++result.matchedTargets;
                    const auto next = source->second.upper_bound(frame);
                    if (next == source->second.begin())
                        weight = next->second;
                    else if (next == source->second.end())
                        weight = std::prev(next)->second;
                    else
                    {
                        const auto before = std::prev(next);
                        const float amount = static_cast<float>(frame -
                            before->first) / static_cast<float>(next->first -
                            before->first);
                        weight = glm::mix(before->second, next->second, amount);
                    }
                }
                channel.values.push_back(weight);
            }
        }
        result.clip.channels.push_back(std::move(channel));
    }
    // Count targets once, not once per sampled frame.
    result.matchedTargets /= frames.size();
    result.matchedExpressions = matched.size();
    for (const auto& [name, keys] : sourceKeys)
        if (!matched.count(name)) result.unmatchedExpressions.push_back(name);
    return result;
}
}

#include "SceneCameraLightTrack.h"
#include "Core/Compoonents/Camera/Camera.h"
#include "Core/Compoonents/Lighting/Light.h"
#include "Core/Object.h"
#include "Core/Scene/Scene.h"
#include <algorithm>
#include <cmath>
#include <glm/gtc/quaternion.hpp>

namespace Engine::Components
{
namespace
{
using JsonValue = Engine::Serialization::JsonValue;

template<size_t N>
JsonValue FloatArray(const std::array<float, N>& source)
{
    JsonValue result = JsonValue::MakeArray();
    for (float value : source) result.Push(JsonValue(value));
    return result;
}

template<size_t N>
void ReadFloats(const JsonValue& value, std::array<float, N>& target)
{
    for (size_t i = 0; i < N && i < value.ArraySize(); ++i)
        target[i] = value.ArrayAt(i).AsFloat();
}

float Bezier(float t, const std::array<float, 24>& bytes, size_t channel)
{
    const size_t offset = channel * 4;
    const float x1 = std::clamp(bytes[offset], 0.f, 1.f);
    const float y1 = std::clamp(bytes[offset + 1], 0.f, 1.f);
    const float x2 = std::clamp(bytes[offset + 2], 0.f, 1.f);
    const float y2 = std::clamp(bytes[offset + 3], 0.f, 1.f);
    if (bytes[offset] == 0 && bytes[offset + 1] == 0 &&
        bytes[offset + 2] == 0 && bytes[offset + 3] == 0) return t;
    const auto curve = [](float u, float a, float b)
    {
        const float v = 1.f - u;
        return 3.f * v * v * u * a + 3.f * v * u * u * b + u * u * u;
    };
    float low = 0.f, high = 1.f;
    for (int i = 0; i < 18; ++i)
    {
        const float mid = (low + high) * 0.5f;
        if (curve(mid, x1, x2) < t) low = mid;
        else high = mid;
    }
    return curve((low + high) * 0.5f, y1, y2);
}

float Mix(float a, float b, float t) { return a + (b - a) * t; }

glm::vec3 Euler(const std::array<float, 3>& rotation)
{
    return { rotation[0], rotation[1], rotation[2] };
}

glm::quat Rotation(const std::array<float, 3>& euler)
{
    return glm::normalize(glm::angleAxis(euler[2], glm::vec3(0.f, 0.f, 1.f)) *
        glm::angleAxis(euler[1], glm::vec3(0.f, 1.f, 0.f)) *
        glm::angleAxis(euler[0], glm::vec3(1.f, 0.f, 0.f)));
}
}

SceneCameraLightTrack::SceneCameraLightTrack()
{
    SetTypeName(COMPONENT_TYPE_NAME(SceneCameraLightTrack));
    RegisterField("kind", kind);
    RegisterField("playOnStart", playOnStart);
    RegisterField("looping", looping);
    RegisterField("speed", speed);
    RegisterField("time", time);
    RegisterField("framesPerSecond", framesPerSecond);
}

SceneCameraLightTrack::JsonValue SceneCameraLightTrack::Serialize() const
{
    JsonValue result = Component::Serialize();
    JsonValue cameraKeys = JsonValue::MakeArray();
    for (const auto& key : cameras)
        cameraKeys.Push(JsonValue::MakeObject()
            .Set("frame", JsonValue(static_cast<double>(key.frame)))
            .Set("distance", JsonValue(key.distance))
            .Set("target", FloatArray(key.target))
            .Set("rotation", FloatArray(key.rotation))
            .Set("interpolation", FloatArray(key.interpolation))
            .Set("fov", JsonValue(key.fov))
            .Set("perspective", JsonValue(key.perspective)));
    JsonValue lightKeys = JsonValue::MakeArray();
    for (const auto& key : lights)
        lightKeys.Push(JsonValue::MakeObject()
            .Set("frame", JsonValue(static_cast<double>(key.frame)))
            .Set("color", FloatArray(key.color))
            .Set("position", FloatArray(key.position)));
    result.Set("cameras", std::move(cameraKeys));
    return result.Set("lights", std::move(lightKeys));
}

void SceneCameraLightTrack::Deserialize(const JsonValue& value)
{
    Component::Deserialize(value);
    cameras.clear(); lights.clear();
    for (size_t i = 0; i < value["cameras"].ArraySize(); ++i)
    {
        const auto& item = value["cameras"].ArrayAt(i);
        CameraKey key;
        key.frame = static_cast<uint32_t>(item["frame"].AsDouble());
        key.distance = item["distance"].AsFloat();
        ReadFloats(item["target"], key.target);
        ReadFloats(item["rotation"], key.rotation);
        for (size_t j = 0; j < key.interpolation.size() &&
            j < item["interpolation"].ArraySize(); ++j)
            key.interpolation[j] = item["interpolation"].ArrayAt(j).AsFloat();
        key.fov = item["fov"].AsFloat();
        key.perspective = item["perspective"].AsBool();
        cameras.push_back(key);
    }
    for (size_t i = 0; i < value["lights"].ArraySize(); ++i)
    {
        const auto& item = value["lights"].ArrayAt(i);
        LightKey key;
        key.frame = static_cast<uint32_t>(item["frame"].AsDouble());
        ReadFloats(item["color"], key.color);
        ReadFloats(item["position"], key.position);
        lights.push_back(key);
    }
    std::stable_sort(cameras.begin(), cameras.end(),
        [](const auto& a, const auto& b) { return a.frame < b.frame; });
    std::stable_sort(lights.begin(), lights.end(),
        [](const auto& a, const auto& b) { return a.frame < b.frame; });
    m_playing = false;
}

float SceneCameraLightTrack::Duration() const
{
    const auto last = kind == static_cast<int>(Kind::Camera)
        ? (cameras.empty() ? 0u : cameras.back().frame)
        : (lights.empty() ? 0u : lights.back().frame);
    return framesPerSecond > 0.f ? last / framesPerSecond : 0.f;
}

void SceneCameraLightTrack::Start()
{
    time = 0.f;
    m_playing = playOnStart;
    Apply();
}

void SceneCameraLightTrack::Restart()
{
    time = 0.f;
    m_playing = true;
    Apply();
}

void SceneCameraLightTrack::Seek(float seconds)
{
    time = std::clamp(seconds, 0.f, Duration());
    Apply();
}

void SceneCameraLightTrack::Update()
{
    if (!m_playing || !Owner || !Owner->GetScene()) return;
    const float duration = Duration();
    if (duration <= 0.f) { Apply(); m_playing = false; return; }
    time += std::max(0.f, Owner->GetScene()->GetDeltaTime()) * speed;
    if (looping)
    {
        time = std::fmod(time, duration);
        if (time < 0.f) time += duration;
    }
    else
    {
        time = std::clamp(time, 0.f, duration);
        if (time == duration || time == 0.f && speed < 0.f) m_playing = false;
    }
    Apply();
}

void SceneCameraLightTrack::Apply()
{
    if (!Owner) return;
    const float frame = time * framesPerSecond;
    if (kind == static_cast<int>(Kind::Camera))
    {
        auto* camera = Owner->GetComponent<Camera>();
        if (!camera || cameras.empty()) return;
        const auto next = std::lower_bound(cameras.begin(), cameras.end(), frame,
            [](const CameraKey& key, float value) { return key.frame < value; });
        const auto& b = next == cameras.end() ? cameras.back() : *next;
        const auto& a = next == cameras.begin() ? b : *(next - 1);
        const float t = a.frame == b.frame ? 0.f :
            std::clamp((frame - a.frame) / (b.frame - a.frame), 0.f, 1.f);
        std::array<float, 3> target{}, rotation{};
        for (size_t i = 0; i < 3; ++i)
            target[i] = Mix(a.target[i], b.target[i], Bezier(t, b.interpolation, i));
        const float rotationT = Bezier(t, b.interpolation, 3);
        for (size_t i = 0; i < 3; ++i)
            rotation[i] = Mix(a.rotation[i], b.rotation[i], rotationT);
        const float distance = Mix(a.distance, b.distance,
            Bezier(t, b.interpolation, 4));
        const glm::quat orientation = Rotation(rotation);
        Owner->transform.position = glm::vec3(target[0], target[1], target[2]) +
            orientation * glm::vec3(0.f, 0.f, distance);
        const glm::quat view = distance > 0.f
            ? orientation * glm::angleAxis(3.14159265f, glm::vec3(0.f, 1.f, 0.f))
            : orientation;
        const auto euler = glm::eulerAngles(view);
        Owner->transform.rotation = Euler({ euler.x, euler.y, euler.z });
        camera->fov = std::clamp(Mix(a.fov, b.fov,
            Bezier(t, b.interpolation, 5)), 1.f, 179.f);
        camera->orthographic = !(frame < b.frame ? a.perspective : b.perspective);
        camera->orthographicSize = std::max(0.01f,
            std::abs(distance) * std::tan(glm::radians(camera->fov) * 0.5f));
    }
    else
    {
        auto* light = Owner->GetComponent<Light>();
        if (!light || lights.empty()) return;
        const auto next = std::lower_bound(lights.begin(), lights.end(), frame,
            [](const LightKey& key, float value) { return key.frame < value; });
        const auto& b = next == lights.end() ? lights.back() : *next;
        const auto& a = next == lights.begin() ? b : *(next - 1);
        const float t = a.frame == b.frame ? 0.f :
            std::clamp((frame - a.frame) / (b.frame - a.frame), 0.f, 1.f);
        for (size_t i = 0; i < 3; ++i)
        {
            Owner->transform.position[static_cast<int>(i)] = Mix(a.position[i], b.position[i], t);
            light->color[static_cast<int>(i)] = Mix(a.color[i], b.color[i], t);
        }
    }
}
}

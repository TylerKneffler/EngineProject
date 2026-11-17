#pragma once

#include "Core/Component.h"
#include <array>
#include <cstdint>
#include <vector>

namespace Engine::Components
{
// Camera and light keyframes in engine coordinates. Interpolation stores six
// cubic Bezier curves as normalized (x1, y1, x2, y2) control points.
class SceneCameraLightTrack : public Engine::Core::Component
{
public:
    enum class Kind : int { Camera = 0, Light = 1 };
    struct CameraKey
    {
        uint32_t frame = 0;
        float distance = 0.f;
        std::array<float, 3> target{};
        std::array<float, 3> rotation{};
        std::array<float, 24> interpolation{};
        float fov = 45.f;
        bool perspective = true;
    };
    struct LightKey
    {
        uint32_t frame = 0;
        std::array<float, 3> color{};
        std::array<float, 3> position{};
    };

    SceneCameraLightTrack();
    int kind = static_cast<int>(Kind::Camera);
    bool playOnStart = true;
    bool looping = false;
    float speed = 1.f;
    float time = 0.f;
    float framesPerSecond = 30.f;
    std::vector<CameraKey> cameras;
    std::vector<LightKey> lights;

    JsonValue Serialize() const override;
    void Deserialize(const JsonValue& value) override;
    void Start() override;
    void Update() override;
    void Play() { m_playing = true; }
    void Pause() { m_playing = false; }
    void Restart();
    void Seek(float seconds);
    bool IsPlaying() const { return m_playing; }
    float Duration() const;

private:
    void Apply();
    bool m_playing = false;
};
}

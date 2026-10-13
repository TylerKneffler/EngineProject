#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace Engine::Editor
{
struct VmdBoneFrame
{
    std::string name;
    uint32_t frame = 0;
    float timeSeconds = 0.f;
    std::array<float, 3> translation{};
    std::array<float, 4> rotation{}; // XYZW, in source MMD coordinates.
    std::array<uint8_t, 64> interpolation{};
};

struct VmdMorphFrame
{
    std::string name;
    uint32_t frame = 0;
    float timeSeconds = 0.f;
    float weight = 0.f;
};

struct VmdIkSwitch
{
    std::string name;
    bool enabled = false;
};

struct VmdPropertyFrame
{
    uint32_t frame = 0;
    float timeSeconds = 0.f;
    bool visible = true;
    std::vector<VmdIkSwitch> ik;
};

struct VmdCameraFrame
{
    uint32_t frame = 0;
    float timeSeconds = 0.f;
    float distance = 0.f;
    std::array<float, 3> position{};
    std::array<float, 3> rotation{};
    std::array<uint8_t, 24> interpolation{};
    uint32_t fieldOfViewDegrees = 0;
    bool perspective = true;
};

struct VmdLightFrame
{
    uint32_t frame = 0;
    float timeSeconds = 0.f;
    std::array<float, 3> color{};
    std::array<float, 3> position{};
};

struct VmdShadowFrame
{
    uint32_t frame = 0;
    float timeSeconds = 0.f;
    uint8_t mode = 0;
    float distance = 0.f;
};

struct VmdMotion
{
    enum class Track { Bones, Morphs, Camera, Light, Shadow, Properties };
    std::string modelName;
    std::vector<VmdBoneFrame> bones;
    std::vector<VmdMorphFrame> morphs;
    std::vector<VmdPropertyFrame> properties;
    std::vector<VmdCameraFrame> cameras;
    std::vector<VmdLightFrame> lights;
    std::vector<VmdShadowFrame> shadows;
    uint32_t cameraFrames = 0;
    uint32_t lightFrames = 0;
    uint32_t shadowFrames = 0;
    uint32_t lastFrame = 0;
    float durationSeconds = 0.f;

    // A selected track retains the source time span so independently applied
    // tracks can be synchronized to the original VMD timeline.
    VmdMotion SelectTrack(Track track) const;
};

class VmdMotionDecoder
{
public:
    static bool DecodeFile(const std::string& path, VmdMotion& motion,
        std::string& error);
    static bool DecodeBytes(const std::vector<uint8_t>& bytes, VmdMotion& motion,
        std::string& error);
};
}

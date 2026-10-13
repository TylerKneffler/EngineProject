#include "Editor/Core/Importers/VmdMotionDecoder.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

using Engine::Editor::VmdMotion;
using Engine::Editor::VmdMotionDecoder;

namespace
{
void U32(std::vector<uint8_t>& bytes, uint32_t value)
{
    for (int offset = 0; offset < 32; offset += 8)
        bytes.push_back(static_cast<uint8_t>(value >> offset));
}

void F32(std::vector<uint8_t>& bytes, float value)
{
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    U32(bytes, bits);
}

void Fixed(std::vector<uint8_t>& bytes, const std::string& value, size_t width)
{
    assert(value.size() <= width);
    bytes.insert(bytes.end(), value.begin(), value.end());
    bytes.insert(bytes.end(), width - value.size(), 0);
}

void Bone(std::vector<uint8_t>& bytes, const std::string& name,
    uint32_t frame, float x)
{
    Fixed(bytes, name, 15);
    U32(bytes, frame);
    F32(bytes, x); F32(bytes, 0.f); F32(bytes, 0.f);
    F32(bytes, 0.f); F32(bytes, 0.f); F32(bytes, 0.f); F32(bytes, 1.f);
    bytes.insert(bytes.end(), 64, 0);
}

void Camera(std::vector<uint8_t>& bytes, uint32_t frame, float distance)
{
    U32(bytes, frame); F32(bytes, distance);
    for (int i = 0; i < 3; ++i) F32(bytes, static_cast<float>(i + 1));
    for (int i = 0; i < 3; ++i) F32(bytes, 0.f);
    bytes.insert(bytes.end(), 24, 42);
    U32(bytes, 45); bytes.push_back(1);
}

void Light(std::vector<uint8_t>& bytes, uint32_t frame, float color)
{
    U32(bytes, frame);
    for (int i = 0; i < 3; ++i) F32(bytes, color);
    for (int i = 0; i < 3; ++i) F32(bytes, 2.f);
}

void Shadow(std::vector<uint8_t>& bytes, uint32_t frame, uint8_t mode)
{
    U32(bytes, frame); bytes.push_back(mode); F32(bytes, 0.25f);
}

std::vector<uint8_t> Fixture()
{
    std::vector<uint8_t> bytes;
    Fixed(bytes, "Vocaloid Motion Data 0002", 30);
    Fixed(bytes, "test", 20);
    const std::string center("\x83\x5a\x83\x93\x83\x5e\x81\x5b", 8);
    U32(bytes, 3);
    Bone(bytes, center, 30, 1.f);
    Bone(bytes, center, 0, 0.f); // sparse and out of order
    Bone(bytes, center, 30, 2.f); // duplicate: last key wins
    U32(bytes, 1);
    Fixed(bytes, std::string("\x8f\xce\x82\xa2", 4), 15); // 笑い
    U32(bytes, 15);
    F32(bytes, 0.75f);
    U32(bytes, 3); // camera: out of order with a repeated frame
    Camera(bytes, 20, 4.f); Camera(bytes, 0, 1.f); Camera(bytes, 20, 5.f);
    U32(bytes, 3); // light
    Light(bytes, 10, 0.25f); Light(bytes, 0, 0.1f); Light(bytes, 10, 0.5f);
    U32(bytes, 3); // shadow
    Shadow(bytes, 25, 1); Shadow(bytes, 0, 0); Shadow(bytes, 25, 2);
    U32(bytes, 1); // property frames
    U32(bytes, 30);
    bytes.push_back(0); // hidden
    U32(bytes, 1);
    Fixed(bytes, std::string("\x8d\xb6\x91\xab\x82\x68\x82\x6a", 8), 20);
    bytes.push_back(1); // IK enabled
    return bytes;
}
}

int main()
{
    const auto bytes = Fixture();
    VmdMotion motion;
    std::string error;
    assert(VmdMotionDecoder::DecodeBytes(bytes, motion, error));
    assert(error.empty());
    assert(motion.bones.size() == 2);
    assert(motion.bones[0].name == "センター");
    assert(motion.bones[0].frame == 0);
    assert(motion.bones[1].frame == 30);
    assert(motion.bones[1].translation[0] == 2.f);
    assert(std::abs(motion.bones[1].timeSeconds - 1.f) < 1e-6f);
    assert(motion.morphs.size() == 1 && motion.morphs[0].name == "笑い");
    assert(motion.cameraFrames == 3 && motion.lightFrames == 3 &&
        motion.shadowFrames == 3);
    assert(motion.cameras.size() == 2 && motion.cameras[0].frame == 0 &&
        motion.cameras[1].frame == 20 && motion.cameras[1].distance == 5.f &&
        motion.cameras[1].fieldOfViewDegrees == 45 &&
        !motion.cameras[1].perspective &&
        motion.cameras[1].interpolation[0] == 42);
    assert(motion.lights.size() == 2 && motion.lights[0].frame == 0 &&
        motion.lights[1].frame == 10 && motion.lights[1].color[0] == 0.5f);
    assert(motion.shadows.size() == 2 && motion.shadows[0].frame == 0 &&
        motion.shadows[1].frame == 25 && motion.shadows[1].mode == 2 &&
        motion.shadows[1].distance == 0.25f);
    assert(motion.properties.size() == 1 && !motion.properties[0].visible);
    assert(motion.properties[0].ik.size() == 1 &&
        motion.properties[0].ik[0].name == "左足ＩＫ" &&
        motion.properties[0].ik[0].enabled);
    assert(motion.lastFrame == 30 && std::abs(motion.durationSeconds - 1.f) < 1e-6f);
    const auto cameraOnly = motion.SelectTrack(VmdMotion::Track::Camera);
    assert(cameraOnly.cameras.size() == 2 && cameraOnly.bones.empty() &&
        cameraOnly.morphs.empty() && cameraOnly.lights.empty() &&
        cameraOnly.properties.empty() && cameraOnly.durationSeconds == 1.f);
    const auto lightOnly = motion.SelectTrack(VmdMotion::Track::Light);
    assert(lightOnly.lights.size() == 2 && lightOnly.cameras.empty() &&
        lightOnly.bones.empty() && lightOnly.lastFrame == 30);
    const auto bodyOnly = motion.SelectTrack(VmdMotion::Track::Bones);
    assert(bodyOnly.bones.size() == 2 && bodyOnly.cameras.empty() &&
        bodyOnly.lights.empty() && bodyOnly.morphs.empty());

    auto truncated = bytes;
    truncated.pop_back();
    assert(!VmdMotionDecoder::DecodeBytes(truncated, motion, error));
    assert(!error.empty() && motion.bones.empty());
    auto badSignature = bytes;
    badSignature[0] = 'X';
    assert(!VmdMotionDecoder::DecodeBytes(badSignature, motion, error));
    auto hugeCount = bytes;
    hugeCount[50] = hugeCount[51] = hugeCount[52] = hugeCount[53] = 0xff;
    assert(!VmdMotionDecoder::DecodeBytes(hugeCount, motion, error));
    auto badFlag = bytes;
    badFlag[badFlag.size() - 26] = 2; // property visibility
    assert(!VmdMotionDecoder::DecodeBytes(badFlag, motion, error));
    auto badProjection = bytes;
    // The first camera follows the three bones and one morph.
    constexpr size_t firstCamera = 30 + 20 + 4 + 3 * 111 + 4 + 23 + 4;
    badProjection[firstCamera + 60] = 2;
    assert(!VmdMotionDecoder::DecodeBytes(badProjection, motion, error));
    assert(error == "Invalid VMD camera projection flag");
    assert(VmdMotionDecoder::DecodeBytes(bytes, motion, error));
    assert(!VmdMotionDecoder::DecodeFile("missing-vmd-file.vmd", motion, error));
    assert(motion.bones.empty() && error == "Could not open VMD file");
}

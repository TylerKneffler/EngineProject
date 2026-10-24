#include "VmdMotionDecoder.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <map>
#include <stdexcept>
#include <string_view>

#ifdef _WIN32
#define NOMINMAX
#include <Windows.h>
#endif

namespace Engine::Editor
{
VmdMotion VmdMotion::SelectTrack(Track track) const
{
    VmdMotion selected;
    selected.modelName = modelName;
    selected.lastFrame = lastFrame;
    selected.durationSeconds = durationSeconds;
    switch (track)
    {
    case Track::Bones: selected.bones = bones; break;
    case Track::Morphs: selected.morphs = morphs; break;
    case Track::Camera:
        selected.cameras = cameras;
        selected.cameraFrames = cameraFrames;
        break;
    case Track::Light:
        selected.lights = lights;
        selected.lightFrames = lightFrames;
        break;
    case Track::Shadow:
        selected.shadows = shadows;
        selected.shadowFrames = shadowFrames;
        break;
    case Track::Properties: selected.properties = properties; break;
    }
    return selected;
}

namespace
{
class Reader
{
public:
    explicit Reader(const std::vector<uint8_t>& bytes) : m_bytes(bytes) {}
    size_t Remaining() const { return m_bytes.size() - m_offset; }
    bool Empty() const { return Remaining() == 0; }
    size_t Offset() const { return m_offset; }
    const uint8_t* Read(size_t count)
    {
        if (count > Remaining()) throw std::runtime_error("Truncated VMD section");
        const uint8_t* result = m_bytes.data() + m_offset;
        m_offset += count;
        return result;
    }
    uint8_t U8() { return *Read(1); }
    uint32_t U32()
    {
        const uint8_t* value = Read(4);
        return uint32_t(value[0]) | (uint32_t(value[1]) << 8) |
            (uint32_t(value[2]) << 16) | (uint32_t(value[3]) << 24);
    }
    float F32()
    {
        const uint32_t bits = U32();
        float result;
        std::memcpy(&result, &bits, sizeof(result));
        if (!std::isfinite(result)) throw std::runtime_error("Non-finite VMD value");
        return result;
    }
    uint32_t Count(const char* section, size_t minimumBytes)
    {
        const uint32_t count = U32();
        if (count > 1'000'000 || count > Remaining() / minimumBytes)
            throw std::runtime_error(std::string("Invalid VMD ") + section + " count");
        return count;
    }

private:
    const std::vector<uint8_t>& m_bytes;
    size_t m_offset = 0;
};

std::string Cp932Name(const uint8_t* bytes, size_t capacity)
{
    size_t size = 0;
    while (size < capacity && bytes[size] != 0) ++size;
    if (size == 0) return {};
#ifdef _WIN32
    // VMD exporters can byte-truncate names at the fixed field boundary,
    // leaving the lead byte of a CP932 character without its trailing byte.
    // Discard only that incomplete final character; malformed sequences
    // elsewhere (or before a terminator) must still fail strict conversion.
    if (size == capacity)
        for (size_t index = 0; index < size; ++index)
            if (IsDBCSLeadByteEx(932, bytes[index]))
            {
                if (index + 1 == size) { --size; break; }
                ++index;
            }
    if (size == 0) return {};
    const int wideCount = MultiByteToWideChar(932, MB_ERR_INVALID_CHARS,
        reinterpret_cast<const char*>(bytes), static_cast<int>(size), nullptr, 0);
    if (wideCount <= 0) throw std::runtime_error("Invalid CP932 VMD name");
    std::wstring wide(static_cast<size_t>(wideCount), L'\0');
    if (!MultiByteToWideChar(932, MB_ERR_INVALID_CHARS,
        reinterpret_cast<const char*>(bytes), static_cast<int>(size),
        wide.data(), wideCount))
        throw std::runtime_error("Invalid CP932 VMD name");
    const int utf8Count = WideCharToMultiByte(CP_UTF8, 0, wide.data(),
        wideCount, nullptr, 0, nullptr, nullptr);
    if (utf8Count <= 0) throw std::runtime_error("Could not convert VMD name to UTF-8");
    std::string utf8(static_cast<size_t>(utf8Count), '\0');
    if (!WideCharToMultiByte(CP_UTF8, 0, wide.data(), wideCount,
        utf8.data(), utf8Count, nullptr, nullptr))
        throw std::runtime_error("Could not convert VMD name to UTF-8");
    return utf8;
#else
    throw std::runtime_error("CP932 VMD names require a platform converter");
#endif
}

std::string Name(Reader& reader, size_t capacity)
{
    return Cp932Name(reader.Read(capacity), capacity);
}

void ObserveFrame(VmdMotion& motion, uint32_t frame)
{
    motion.lastFrame = std::max(motion.lastFrame, frame);
}
}

bool VmdMotionDecoder::DecodeFile(const std::string& path, VmdMotion& motion,
    std::string& error)
{
    motion = {};
    error.clear();
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) { error = "Could not open VMD file"; return false; }
    const std::streamoff size = file.tellg();
    if (size < 0 || size > 64 * 1024 * 1024)
    { error = "VMD file exceeds 64 MiB limit"; return false; }
    file.seekg(0);
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    if (!file.read(reinterpret_cast<char*>(bytes.data()), size))
    { error = "Could not read VMD file"; return false; }
    return DecodeBytes(bytes, motion, error);
}

bool VmdMotionDecoder::DecodeBytes(const std::vector<uint8_t>& bytes,
    VmdMotion& motion, std::string& error)
{
    motion = {};
    error.clear();
    try
    {
        if (bytes.size() > 64 * 1024 * 1024)
            throw std::runtime_error("VMD file exceeds 64 MiB limit");
        Reader reader(bytes);
        const uint8_t* signature = reader.Read(30);
        const std::string_view header(reinterpret_cast<const char*>(signature), 30);
        const bool modern = header.substr(0, 25) == "Vocaloid Motion Data 0002";
        const bool legacy = header.substr(0, 25) == "Vocaloid Motion Data file";
        if (!modern && !legacy) throw std::runtime_error("Invalid VMD signature");
        motion.modelName = Name(reader, modern ? 20 : 10);

        std::map<std::pair<std::string, uint32_t>, VmdBoneFrame> bones;
        for (uint32_t index = 0, count = reader.Count("bone", 111); index < count; ++index)
        {
            VmdBoneFrame value;
            value.name = Name(reader, 15);
            value.frame = reader.U32();
            value.timeSeconds = static_cast<float>(value.frame / 30.0);
            for (float& component : value.translation) component = reader.F32();
            for (float& component : value.rotation) component = reader.F32();
            std::copy_n(reader.Read(64), 64, value.interpolation.begin());
            ObserveFrame(motion, value.frame);
            bones[{ value.name, value.frame }] = std::move(value); // last key wins
        }
        for (auto& entry : bones) motion.bones.push_back(std::move(entry.second));

        std::map<std::pair<std::string, uint32_t>, VmdMorphFrame> morphs;
        for (uint32_t index = 0, count = reader.Count("morph", 23); index < count; ++index)
        {
            VmdMorphFrame value;
            value.name = Name(reader, 15);
            value.frame = reader.U32();
            value.timeSeconds = static_cast<float>(value.frame / 30.0);
            value.weight = reader.F32();
            ObserveFrame(motion, value.frame);
            morphs[{ value.name, value.frame }] = std::move(value);
        }
        for (auto& entry : morphs) motion.morphs.push_back(std::move(entry.second));

        if (!reader.Empty())
        {
            motion.cameraFrames = reader.Count("camera", 61);
            std::map<uint32_t, VmdCameraFrame> cameras;
            for (uint32_t index = 0; index < motion.cameraFrames; ++index)
            {
                VmdCameraFrame value;
                value.frame = reader.U32();
                value.timeSeconds = static_cast<float>(value.frame / 30.0);
                value.distance = reader.F32();
                for (float& component : value.position) component = reader.F32();
                for (float& component : value.rotation) component = reader.F32();
                std::copy_n(reader.Read(24), 24, value.interpolation.begin());
                value.fieldOfViewDegrees = reader.U32();
                const uint8_t projection = reader.U8();
                if (projection > 1)
                    throw std::runtime_error("Invalid VMD camera projection flag");
                value.perspective = projection == 0;
                ObserveFrame(motion, value.frame);
                cameras[value.frame] = std::move(value); // last key wins
            }
            for (auto& entry : cameras) motion.cameras.push_back(std::move(entry.second));
        }
        if (!reader.Empty())
        {
            motion.lightFrames = reader.Count("light", 28);
            std::map<uint32_t, VmdLightFrame> lights;
            for (uint32_t index = 0; index < motion.lightFrames; ++index)
            {
                VmdLightFrame value;
                value.frame = reader.U32();
                value.timeSeconds = static_cast<float>(value.frame / 30.0);
                for (float& component : value.color) component = reader.F32();
                for (float& component : value.position) component = reader.F32();
                ObserveFrame(motion, value.frame);
                lights[value.frame] = std::move(value);
            }
            for (auto& entry : lights) motion.lights.push_back(std::move(entry.second));
        }
        if (!reader.Empty())
        {
            motion.shadowFrames = reader.Count("shadow", 9);
            std::map<uint32_t, VmdShadowFrame> shadows;
            for (uint32_t index = 0; index < motion.shadowFrames; ++index)
            {
                VmdShadowFrame value;
                value.frame = reader.U32();
                value.timeSeconds = static_cast<float>(value.frame / 30.0);
                value.mode = reader.U8();
                value.distance = reader.F32();
                ObserveFrame(motion, value.frame);
                shadows[value.frame] = std::move(value);
            }
            for (auto& entry : shadows) motion.shadows.push_back(std::move(entry.second));
        }
        if (!reader.Empty())
        {
            std::map<uint32_t, VmdPropertyFrame> properties;
            for (uint32_t index = 0, count = reader.Count("property", 9); index < count; ++index)
            {
                VmdPropertyFrame value;
                value.frame = reader.U32();
                value.timeSeconds = static_cast<float>(value.frame / 30.0);
                const uint8_t visible = reader.U8();
                if (visible > 1) throw std::runtime_error("Invalid VMD visibility flag");
                value.visible = visible != 0;
                const uint32_t ikCount = reader.Count("IK switch", 21);
                std::map<std::string, bool> switches;
                for (uint32_t ik = 0; ik < ikCount; ++ik)
                {
                    const std::string name = Name(reader, 20);
                    const uint8_t enabled = reader.U8();
                    if (enabled > 1) throw std::runtime_error("Invalid VMD IK flag");
                    switches[name] = enabled != 0;
                }
                for (const auto& [name, enabled] : switches)
                    value.ik.push_back({ name, enabled });
                ObserveFrame(motion, value.frame);
                properties[value.frame] = std::move(value);
            }
            for (auto& entry : properties)
                motion.properties.push_back(std::move(entry.second));
        }
        if (!reader.Empty())
            throw std::runtime_error("Unexpected trailing VMD bytes at " +
                std::to_string(reader.Offset()));
        motion.durationSeconds = static_cast<float>(motion.lastFrame / 30.0);
        return true;
    }
    catch (const std::exception& exception)
    {
        motion = {};
        error = exception.what();
        return false;
    }
}
}

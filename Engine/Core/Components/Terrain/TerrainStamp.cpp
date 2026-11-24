#include "Core/Components/Terrain/TerrainStamp.h"

#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>

namespace Engine::Components
{
namespace
{
std::shared_ptr<const TerrainStamp> ReadStamp(const std::string& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) return nullptr;
    const std::vector<char> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    size_t at = 0;
    const auto read = [&](auto& value)
    {
        if (at + sizeof(value) > bytes.size()) return false;
        std::memcpy(&value, bytes.data() + at, sizeof(value));
        at += sizeof(value);
        return true;
    };
    constexpr uint64_t Magic = 0x31505453524e5254ull; // "TRNRSTP1"
    uint64_t magic = 0;
    uint32_t version = 0, width = 0, height = 0, materialLength = 0;
    float spacing = 0.f, origin[2] = {}, direction[3] = {}, axisU[3] = {}, axisV[3] = {};
    if (!read(magic) || !read(version) || !read(width) || !read(height) || !read(spacing) ||
        !read(origin) || !read(direction) || !read(axisU) || !read(axisV) || !read(materialLength))
        return nullptr;
    if (magic != Magic || version != 2 || width < 2 || height < 2 || width > 4096 ||
        height > 4096 || spacing <= 0.f || materialLength > 4096 ||
        at + materialLength + static_cast<size_t>(width) * height * 9 * sizeof(float) > bytes.size())
        return nullptr;
    auto stamp = std::make_shared<TerrainStamp>();
    stamp->width = static_cast<int>(width);
    stamp->height = static_cast<int>(height);
    stamp->spacing = spacing;
    stamp->direction = {direction[0], direction[1], direction[2]};
    stamp->axisU = {axisU[0], axisU[1], axisU[2]};
    stamp->axisV = {axisV[0], axisV[1], axisV[2]};
    stamp->material.assign(bytes.data() + at, materialLength);
    at += materialLength;
    stamp->points.resize(static_cast<size_t>(width) * height);
    for (auto& point : stamp->points)
    {
        float values[9];
        std::memcpy(values, bytes.data() + at, sizeof(values));
        at += sizeof(values);
        point.position = {values[0], values[1], values[2]};
        point.normal = {values[3], values[4], values[5]};
        point.texcoord = {values[6], values[7]};
        point.weight = values[8];
    }
    uint32_t levelCount = 0;
    if (!read(levelCount) || levelCount == 0 || levelCount > 8) return nullptr;
    const size_t pointCount = stamp->points.size();
    for (uint32_t level = 0; level < levelCount; ++level)
    {
        uint32_t triangles = 0;
        if (!read(triangles) || at + static_cast<size_t>(triangles) * 3 * sizeof(uint32_t) > bytes.size())
            return nullptr;
        std::vector<uint32_t> indices(static_cast<size_t>(triangles) * 3);
        std::memcpy(indices.data(), bytes.data() + at, indices.size() * sizeof(uint32_t));
        at += indices.size() * sizeof(uint32_t);
        for (uint32_t index : indices)
            if (index >= pointCount) return nullptr;
        stamp->levels.push_back(std::move(indices));
    }
    if (at != bytes.size()) return nullptr;
    return stamp;
}
}

std::shared_ptr<const TerrainStamp> TerrainStamp::Load(const std::string& path)
{
    static std::mutex mutex;
    static std::map<std::string, std::shared_ptr<const TerrainStamp>> loaded;
    std::lock_guard lock(mutex);
    const auto found = loaded.find(path);
    if (found != loaded.end()) return found->second;
    auto stamp = ReadStamp(path);
    loaded.emplace(path, stamp);
    return stamp;
}
}

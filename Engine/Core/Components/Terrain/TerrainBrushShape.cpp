#include "Core/Components/Terrain/TerrainBrushShape.h"
#include "Core/Brushes/BrushPreset.h"
#include "Core/Components/Mesh/Mesh.h"

#include <stb_image.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <mutex>
#include <filesystem>

namespace Engine::Components
{
namespace
{
float Bilinear(const std::vector<float>& grid, int resolution, const glm::vec2& uv)
{
    if (resolution < 2 || grid.size() != size_t(resolution) * resolution) return 0.f;
    if (!(std::abs(uv.x) <= 1.f && std::abs(uv.y) <= 1.f)) return 0.f;
    const float fx = (uv.x * .5f + .5f) * (resolution - 1);
    const float fz = (uv.y * .5f + .5f) * (resolution - 1);
    const int x0 = std::clamp(int(std::floor(fx)), 0, resolution - 2);
    const int z0 = std::clamp(int(std::floor(fz)), 0, resolution - 2);
    const float tx = std::clamp(fx - x0, 0.f, 1.f), tz = std::clamp(fz - z0, 0.f, 1.f);
    const auto at = [&](int x, int z) { return grid[size_t(z) * resolution + x]; };
    return (at(x0, z0) * (1.f - tx) + at(x0 + 1, z0) * tx) * (1.f - tz) +
        (at(x0, z0 + 1) * (1.f - tx) + at(x0 + 1, z0 + 1) * tx) * tz;
}
}

float TerrainBrushShape::SampleHeight(const glm::vec2& uv) const
{
    return Bilinear(heights, resolution, uv);
}

float TerrainBrushShape::SampleMask(const glm::vec2& uv) const
{
    return Bilinear(mask, resolution, uv);
}

glm::vec2 TerrainBrushShape::ToShape(float dx, float dz, float radius, float yawRadians)
{
    if (!(radius > 0.f)) return glm::vec2(2.f);
    const float c = std::cos(yawRadians), s = std::sin(yawRadians);
    // Undo a rotation by yaw about +Y (x' = c x + s z, z' = -s x + c z).
    return glm::vec2(c * dx - s * dz, s * dx + c * dz) / radius;
}

glm::vec2 TerrainBrushShape::FromShape(const glm::vec2& uv, float radius, float yawRadians)
{
    const float c = std::cos(yawRadians), s = std::sin(yawRadians);
    const glm::vec2 p = uv * radius;
    return glm::vec2(c * p.x + s * p.y, -s * p.x + c * p.y);
}

float TerrainBrushShape::Falloff(float distance, float hardness, int falloff)
{
    if (!(distance <= 1.f)) return 0.f;
    hardness = std::clamp(hardness, 0.f, 1.f);
    if (distance <= hardness) return 1.f;
    if (falloff == 0 || hardness >= 1.f) return 0.f;
    const float t = (1.f - distance) / (1.f - hardness);
    return falloff == 1 ? t : t * t * (3.f - 2.f * t);
}

void TerrainBrushShape::BuildOutline()
{
    outline.clear();
    for (int i = 0; i < 64; ++i)
    {
        const float angle = 6.2831853f * i / 64.f;
        const glm::vec2 direction(std::cos(angle), std::sin(angle));
        float reach = 0.f;
        for (float r = 0.f; r <= 1.415f; r += 1.f / std::max(resolution, 8))
            if (SampleMask(direction * r) > .5f) reach = r;
        outline.push_back(direction * reach);
    }
}

namespace
{
// An image's samples (0..1, rows top first) resampled bilinearly onto an
// n x n grid. channels: 1 (gray) or 4; for RGBA, alpha unless the image is
// opaque, then inverted luminance (as the painting tools read tips).
struct Image
{
    int width = 0, height = 0;
    std::vector<float> values;
    float At(float u, float v) const   // u, v in [0, 1]
    {
        const float fx = std::clamp(u, 0.f, 1.f) * (width - 1), fy = std::clamp(v, 0.f, 1.f) * (height - 1);
        const int x0 = std::min(int(fx), width - 1), y0 = std::min(int(fy), height - 1);
        const int x1 = std::min(x0 + 1, width - 1), y1 = std::min(y0 + 1, height - 1);
        const float tx = fx - x0, ty = fy - y0;
        const auto at = [&](int x, int y) { return values[size_t(y) * width + x]; };
        return (at(x0, y0) * (1.f - tx) + at(x1, y0) * tx) * (1.f - ty) +
            (at(x0, y1) * (1.f - tx) + at(x1, y1) * tx) * ty;
    }
};

bool LoadTip(const std::string& path, Image& image)
{
    int width = 0, height = 0, channels = 0;
    stbi_uc* pixels = stbi_load(path.c_str(), &width, &height, &channels, 4);
    if (!pixels) return false;
    bool opaque = true;
    for (size_t i = 3; i < size_t(width) * height * 4 && opaque; i += 4) opaque = pixels[i] == 255;
    image.width = width; image.height = height;
    image.values.resize(size_t(width) * height);
    for (size_t i = 0; i < image.values.size(); ++i)
        image.values[i] = opaque
            ? 1.f - (pixels[i * 4] + pixels[i * 4 + 1] + pixels[i * 4 + 2]) / 765.f
            : pixels[i * 4 + 3] / 255.f;
    stbi_image_free(pixels);
    return width > 0 && height > 0;
}

bool LoadRelief(const std::string& path, Image& image)
{
    int width = 0, height = 0, channels = 0;
    stbi_us* pixels = stbi_load_16(path.c_str(), &width, &height, &channels, 1);
    if (!pixels) return false;
    image.width = width; image.height = height;
    image.values.resize(size_t(width) * height);
    for (size_t i = 0; i < image.values.size(); ++i) image.values[i] = pixels[i] / 65535.f;
    stbi_image_free(pixels);
    return width > 0 && height > 0;
}
}

std::shared_ptr<const TerrainBrushShape> TerrainBrushShape::FromPreset(
    const Engine::Brushes::BrushPreset& preset, int resolution)
{
    const bool tip = !preset.tipPath.empty();
    const bool relief = preset.relief.Present();
    if (!tip && preset.shape == 0 && !relief) return nullptr;   // the round brush
    const int n = std::clamp(resolution, 8, 512);
    Image tipImage, reliefImage;
    if (tip && !LoadTip(Engine::Components::Mesh::ResolveFilePath(preset.tipPath), tipImage)) return nullptr;
    if (relief && !LoadRelief(Engine::Components::Mesh::ResolveFilePath(preset.relief.mapPath), reliefImage))
        return nullptr;
    auto shape = std::make_shared<TerrainBrushShape>();
    shape->resolution = n;
    shape->halfExtent = 1.f;
    shape->mask.assign(size_t(n) * n, 0.f);
    shape->heights.assign(size_t(n) * n, 0.f);
    for (int row = 0; row < n; ++row)
        for (int col = 0; col < n; ++col)
        {
            const float u = float(col) / (n - 1), v = float(row) / (n - 1);
            const float x = u * 2.f - 1.f, z = v * 2.f - 1.f;
            const size_t i = size_t(row) * n + col;
            if (tip) shape->mask[i] = std::clamp(tipImage.At(u, v), 0.f, 1.f);
            else
            {
                const float distance = preset.shape == 1 ? std::max(std::abs(x), std::abs(z))
                    : std::sqrt(x * x + z * z);
                shape->mask[i] = Falloff(distance, preset.hardness, preset.falloff);
            }
            if (relief)
            {
                shape->heights[i] = reliefImage.At(u, v) * preset.relief.heightRatio;
                shape->maxHeight = std::max(shape->maxHeight, shape->heights[i]);
            }
        }
    const glm::vec2 facing(preset.relief.facingX, preset.relief.facingY);
    if (glm::length(facing) > 1.e-3f) shape->facing = glm::normalize(facing);
    shape->BuildOutline();
    return shape;
}

std::shared_ptr<const TerrainBrushShape> TerrainBrushShape::Load(const std::string& brushPath)
{
    // Keyed by the files' write times too, so an edited brush reloads.
    const auto stamp = [](const std::string& path) -> long long {
        std::error_code error;
        const auto time = std::filesystem::last_write_time(Engine::Components::Mesh::ResolveFilePath(path), error);
        return error ? 0 : static_cast<long long>(time.time_since_epoch().count());
    };
    Engine::Brushes::BrushPreset preset;
    std::string error;
    const std::string resolved = Engine::Components::Mesh::ResolveFilePath(brushPath);
    if (!Engine::Brushes::LoadBrushPreset(resolved, preset, error)) return nullptr;
    const std::string key = brushPath + "|" + std::to_string(stamp(brushPath)) + "|" +
        std::to_string(stamp(preset.tipPath)) + "|" + std::to_string(stamp(preset.relief.mapPath));
    static std::mutex mutex;
    static std::map<std::string, std::shared_ptr<const TerrainBrushShape>> cache;
    std::lock_guard lock(mutex);
    if (const auto found = cache.find(key); found != cache.end()) return found->second;
    auto shape = FromPreset(preset);
    cache[key] = shape;
    return shape;
}
}

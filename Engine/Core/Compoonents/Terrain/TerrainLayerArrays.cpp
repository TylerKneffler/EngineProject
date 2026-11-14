#include "Core/Compoonents/Terrain/TerrainLayerArrays.h"

#include "Core/Compoonents/Materials/Material.h"
#include "Core/Compoonents/Materials/Texture.h"
#include "Core/Graphics/IGraphicsProvider.h"
#include "Core/Graphics/IGraphicsTexture.h"
#include "Core/Rendering/RenderJobs.h"

#include <stb_image.h>
#include <bc7enc.h>
#include <rgbcx.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <mutex>
#include <string>
#include <unordered_map>

namespace Engine::Components
{
namespace
{
using Engine::Graphics::GraphicsTextureFormat;

struct LayerSources
{
    std::string albedo, normal, roughness, occlusion, height;
};

// Encoded arrays, ready to upload: each slice's full mip chain, slice after
// slice, in the format of its array.
struct CpuArrays
{
    uint32_t size = 0, mips = 0, layers = 0;
    bool blockCompressed = false;
    std::vector<uint8_t> albedoHeight, normal, roughAo;
    TerrainLayerArrays::Arrays info;
};

struct Entry
{
    std::shared_future<std::shared_ptr<CpuArrays>> build;
    TerrainLayerArrays::Arrays arrays;
    const Engine::Graphics::IGraphicsProvider* provider = nullptr;
    bool ready = false, failed = false;
};

std::mutex& CacheMutex()
{
    static std::mutex mutex;
    return mutex;
}

std::unordered_map<std::string, std::shared_ptr<Entry>>& Cache()
{
    static std::unordered_map<std::string, std::shared_ptr<Entry>> cache;
    return cache;
}

float SrgbToLinear(float value)
{
    return value <= 0.04045f ? value / 12.92f
        : std::pow((value + 0.055f) / 1.055f, 2.4f);
}

float LinearToSrgb(float value)
{
    value = std::clamp(value, 0.f, 1.f);
    return value <= 0.0031308f ? value * 12.92f
        : 1.055f * std::pow(value, 1.f / 2.4f) - 0.055f;
}

uint8_t ToByte(float value)
{
    return static_cast<uint8_t>(std::lround(std::clamp(value, 0.f, 1.f) * 255.f));
}

// Float RGBA image, wrapped at the edges (terrain maps tile).
struct Image
{
    uint32_t width = 0, height = 0;
    std::vector<float> texels;
    float* At(uint32_t x, uint32_t y) { return texels.data() + (size_t(y) * width + x) * 4; }
    const float* At(uint32_t x, uint32_t y) const { return texels.data() + (size_t(y) * width + x) * 4; }
};

// Halves while at least twice the target, then bilinear to exactly size^2.
Image Resample(Image image, uint32_t size)
{
    while (image.width >= size * 2 && image.height >= size * 2)
    {
        Image half;
        half.width = image.width / 2;
        half.height = image.height / 2;
        half.texels.resize(size_t(half.width) * half.height * 4);
        for (uint32_t y = 0; y < half.height; ++y)
            for (uint32_t x = 0; x < half.width; ++x)
                for (uint32_t c = 0; c < 4; ++c)
                    half.At(x, y)[c] = 0.25f * (image.At(2 * x, 2 * y)[c] +
                        image.At(2 * x + 1, 2 * y)[c] + image.At(2 * x, 2 * y + 1)[c] +
                        image.At(2 * x + 1, 2 * y + 1)[c]);
        image = std::move(half);
    }
    if (image.width == size && image.height == size) return image;
    Image result;
    result.width = result.height = size;
    result.texels.resize(size_t(size) * size * 4);
    const auto wrap = [](int value, uint32_t extent)
    {
        const int e = static_cast<int>(extent);
        return static_cast<uint32_t>(((value % e) + e) % e);
    };
    for (uint32_t y = 0; y < size; ++y)
        for (uint32_t x = 0; x < size; ++x)
        {
            const float u = (x + 0.5f) * image.width / size - 0.5f;
            const float v = (y + 0.5f) * image.height / size - 0.5f;
            const float fu = std::floor(u), fv = std::floor(v);
            const float tu = u - fu, tv = v - fv;
            const uint32_t x0 = wrap(int(fu), image.width), x1 = wrap(int(fu) + 1, image.width);
            const uint32_t y0 = wrap(int(fv), image.height), y1 = wrap(int(fv) + 1, image.height);
            for (uint32_t c = 0; c < 4; ++c)
                result.At(x, y)[c] =
                    (image.At(x0, y0)[c] * (1 - tu) + image.At(x1, y0)[c] * tu) * (1 - tv) +
                    (image.At(x0, y1)[c] * (1 - tu) + image.At(x1, y1)[c] * tu) * tv;
        }
    return result;
}

// Decodes a map and resamples it to size^2 as RGBA floats; color maps are
// converted to linear light. Large sources are halved while converting, so
// the full-size source never exists as a float image.
bool Decode(const std::string& path, bool color, uint32_t size, Image& image)
{
    if (path.empty()) return false;
    const auto texture = Texture::DecodeDetached(path, color);
    if (!texture || texture->GetFormat() != GraphicsTextureFormat::Rgba8)
        return false;
    const auto& pixels = texture->GetPixels();
    const uint32_t width = texture->GetWidth(), height = texture->GetHeight();
    if (!width || !height || pixels.size() < size_t(width) * height * 4) return false;
    float lut[256];
    for (int i = 0; i < 256; ++i) lut[i] = color ? SrgbToLinear(i / 255.f) : i / 255.f;
    const auto value = [&](uint32_t x, uint32_t y, uint32_t c)
    {
        const uint8_t v = pixels[(size_t(y) * width + x) * 4 + c];
        return c != 3 ? lut[v] : v / 255.f;
    };
    const bool halve = width >= size * 2 && height >= size * 2;
    image.width = halve ? width / 2 : width;
    image.height = halve ? height / 2 : height;
    image.texels.resize(size_t(image.width) * image.height * 4);
    for (uint32_t y = 0; y < image.height; ++y)
        for (uint32_t x = 0; x < image.width; ++x)
            for (uint32_t c = 0; c < 4; ++c)
                image.At(x, y)[c] = halve
                    ? 0.25f * (value(2 * x, 2 * y, c) + value(2 * x + 1, 2 * y, c) +
                        value(2 * x, 2 * y + 1, c) + value(2 * x + 1, 2 * y + 1, c))
                    : value(x, y, c);
    image = Resample(std::move(image), size);
    return true;
}

// Source dimensions from the image header (PNG/JPG/TGA...). Formats stb
// cannot identify count as the cap.
uint32_t SourceSize(const std::string& path, uint32_t cap)
{
    if (path.empty()) return 0;
    int width = 0, height = 0, channels = 0;
    if (!stbi_info(path.c_str(), &width, &height, &channels)) return cap;
    return static_cast<uint32_t>(std::max(width, height));
}

// One RGBA8 mip level of each array.
struct LevelImages
{
    uint32_t size = 0;
    std::vector<uint8_t> albedoHeight, normal, roughAo;
};

// Level-0 channels in shading space, then a full mip chain.
struct Slice
{
    Image albedo;    // linear rgb, a = height
    Image normal;    // xyz unit normal, w = ao
    Image roughness; // r = GGX alpha^2
};

std::vector<LevelImages> BuildMips(Slice slice, uint32_t mips, glm::vec3& mean)
{
    std::vector<LevelImages> levels;
    Image& albedo = slice.albedo;
    Image& normal = slice.normal;
    Image& rough = slice.roughness;
    for (uint32_t mip = 0; mip < mips; ++mip)
    {
        const uint32_t size = albedo.width;
        LevelImages level;
        level.size = size;
        const size_t texels = size_t(size) * size;
        level.albedoHeight.resize(texels * 4);
        level.normal.resize(texels * 4);
        level.roughAo.resize(texels * 4);
        for (size_t i = 0; i < texels; ++i)
        {
            const float* a = &albedo.texels[i * 4];
            const float* n = &normal.texels[i * 4];
            level.albedoHeight[i * 4 + 0] = ToByte(LinearToSrgb(a[0]));
            level.albedoHeight[i * 4 + 1] = ToByte(LinearToSrgb(a[1]));
            level.albedoHeight[i * 4 + 2] = ToByte(LinearToSrgb(a[2]));
            level.albedoHeight[i * 4 + 3] = ToByte(a[3]);
            level.normal[i * 4 + 0] = ToByte(n[0] * 0.5f + 0.5f);
            level.normal[i * 4 + 1] = ToByte(n[1] * 0.5f + 0.5f);
            level.normal[i * 4 + 2] = 0;
            level.normal[i * 4 + 3] = 255;
            // Back to perceptual roughness: alpha^2 -> r = alpha^(1/2).
            level.roughAo[i * 4 + 0] = ToByte(std::pow(std::max(rough.texels[i * 4], 0.f), 0.25f));
            level.roughAo[i * 4 + 1] = ToByte(n[3]);
            level.roughAo[i * 4 + 2] = 0;
            level.roughAo[i * 4 + 3] = 255;
        }
        levels.push_back(std::move(level));
        if (size == 1)
        {
            mean = glm::vec3(albedo.texels[0], albedo.texels[1], albedo.texels[2]);
            break;
        }
        const uint32_t half = size / 2;
        Image nextAlbedo, nextNormal, nextRough;
        for (Image* image : { &nextAlbedo, &nextNormal, &nextRough })
        {
            image->width = image->height = half;
            image->texels.assign(size_t(half) * half * 4, 0.f);
        }
        for (uint32_t y = 0; y < half; ++y)
            for (uint32_t x = 0; x < half; ++x)
            {
                float sumN[4] = {}, sumA[4] = {}, sumR = 0.f;
                for (uint32_t dy = 0; dy < 2; ++dy)
                    for (uint32_t dx = 0; dx < 2; ++dx)
                    {
                        const float* a = albedo.At(2 * x + dx, 2 * y + dy);
                        const float* n = normal.At(2 * x + dx, 2 * y + dy);
                        for (uint32_t c = 0; c < 4; ++c) { sumA[c] += a[c]; sumN[c] += n[c]; }
                        sumR += rough.At(2 * x + dx, 2 * y + dy)[0];
                    }
                for (uint32_t c = 0; c < 4; ++c) nextAlbedo.At(x, y)[c] = sumA[c] * 0.25f;
                // Average unit normals; the lost length is normal variance,
                // which widens the GGX lobe (Toksvig) so bumpy layers do not
                // sparkle at distance.
                const glm::vec3 average(sumN[0] * 0.25f, sumN[1] * 0.25f, sumN[2] * 0.25f);
                const float length = std::max(glm::length(average), 1e-4f);
                const glm::vec3 unit = average / length;
                float* n = nextNormal.At(x, y);
                n[0] = unit.x; n[1] = unit.y; n[2] = unit.z; n[3] = sumN[3] * 0.25f;
                const float variance = std::clamp((1.f - length) / length, 0.f, 1.f);
                nextRough.At(x, y)[0] = std::min(sumR * 0.25f + variance, 1.f);
            }
        albedo = std::move(nextAlbedo);
        normal = std::move(nextNormal);
        rough = std::move(nextRough);
    }
    return levels;
}

enum class Block { Bc7Srgb, Bc5 };

// Appends one RGBA8 level as 4x4 blocks; small levels repeat edge texels.
void EncodeLevel(const std::vector<uint8_t>& rgba, uint32_t size, Block block,
    std::vector<uint8_t>& out)
{
    const uint32_t blocks = std::max(1u, (size + 3) / 4);
    const size_t first = out.size();
    out.resize(first + size_t(blocks) * blocks * 16);
    static std::once_flag initialized;
    std::call_once(initialized, [] { bc7enc_compress_block_init(); rgbcx::init(); });
    bc7enc_compress_block_params params;
    bc7enc_compress_block_params_init(&params);
    // Runtime speed over the last fraction of quality; TextureCook uses
    // uber level 2 offline.
    params.m_uber_level = 0;
    params.m_max_partitions = 16;
    Engine::Rendering::RenderJobs::Get().For(blocks, 4, [&](size_t by)
    {
        uint8_t pixels[64];
        for (uint32_t bx = 0; bx < blocks; ++bx)
        {
            for (uint32_t y = 0; y < 4; ++y)
                for (uint32_t x = 0; x < 4; ++x)
                {
                    const uint32_t sx = std::min<uint32_t>(bx * 4 + x, size - 1);
                    const uint32_t sy = std::min<uint32_t>(uint32_t(by) * 4 + y, size - 1);
                    std::memcpy(pixels + (y * 4 + x) * 4, &rgba[(size_t(sy) * size + sx) * 4], 4);
                }
            uint8_t* destination = out.data() + first + (by * blocks + bx) * 16;
            if (block == Block::Bc7Srgb) bc7enc_compress_block(destination, pixels, &params);
            else rgbcx::encode_bc5(destination, pixels, 0, 1, 4);
        }
    });
}

void AppendLevel(const LevelImages& level, bool blockCompressed, CpuArrays& out)
{
    if (blockCompressed)
    {
        EncodeLevel(level.albedoHeight, level.size, Block::Bc7Srgb, out.albedoHeight);
        EncodeLevel(level.normal, level.size, Block::Bc5, out.normal);
        EncodeLevel(level.roughAo, level.size, Block::Bc5, out.roughAo);
        return;
    }
    out.albedoHeight.insert(out.albedoHeight.end(), level.albedoHeight.begin(), level.albedoHeight.end());
    out.normal.insert(out.normal.end(), level.normal.begin(), level.normal.end());
    out.roughAo.insert(out.roughAo.end(), level.roughAo.begin(), level.roughAo.end());
}

// ---- Disk cache -----------------------------------------------------------
constexpr uint64_t CacheMagic = 0x3159415252414c54ull; // TLARRAY1
constexpr uint32_t CacheVersion = 1;

uint64_t Fnv(uint64_t hash, const void* data, size_t size)
{
    const auto* bytes = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; ++i) { hash ^= bytes[i]; hash *= 0x100000001b3ull; }
    return hash;
}

// Identity of the inputs: paths, sizes and write times of every source,
// plus the build settings.
uint64_t SourceSignature(const std::vector<LayerSources>& sources, bool blockCompressed, uint32_t cap)
{
    uint64_t hash = 0xcbf29ce484222325ull;
    hash = Fnv(hash, &CacheVersion, sizeof(CacheVersion));
    hash = Fnv(hash, &blockCompressed, sizeof(blockCompressed));
    hash = Fnv(hash, &cap, sizeof(cap));
    for (const auto& source : sources)
        for (const std::string* path : { &source.albedo, &source.normal,
            &source.roughness, &source.occlusion, &source.height })
        {
            hash = Fnv(hash, path->data(), path->size());
            std::error_code error;
            const auto bytes = std::filesystem::file_size(*path, error);
            const auto time = std::filesystem::last_write_time(*path, error);
            const int64_t stamp[2] = { error ? 0 : int64_t(bytes),
                error ? 0 : int64_t(time.time_since_epoch().count()) };
            hash = Fnv(hash, stamp, sizeof(stamp));
        }
    return hash;
}

std::filesystem::path CachePath(uint64_t signature)
{
    std::error_code error;
    auto directory = std::filesystem::temp_directory_path(error);
    if (error) return {};
    directory /= "EngineTerrainLayerArrays";
    char name[32];
    std::snprintf(name, sizeof(name), "%016llx.bin", static_cast<unsigned long long>(signature));
    return directory / name;
}

template<class T> bool WritePod(std::ostream& stream, const T& value)
{ return bool(stream.write(reinterpret_cast<const char*>(&value), sizeof(T))); }
template<class T> bool ReadPod(std::istream& stream, T& value)
{ return bool(stream.read(reinterpret_cast<char*>(&value), sizeof(T))); }

bool WriteBlob(std::ostream& stream, const std::vector<uint8_t>& blob)
{
    return WritePod(stream, uint64_t(blob.size())) &&
        stream.write(reinterpret_cast<const char*>(blob.data()), std::streamsize(blob.size()));
}

bool ReadBlob(std::istream& stream, std::vector<uint8_t>& blob)
{
    uint64_t size = 0;
    if (!ReadPod(stream, size) || size > (1ull << 32)) return false;
    blob.resize(size_t(size));
    return bool(stream.read(reinterpret_cast<char*>(blob.data()), std::streamsize(size)));
}

void SaveCache(const std::filesystem::path& path, uint64_t signature, const CpuArrays& arrays)
{
    if (path.empty()) return;
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    const auto temporary = path.string() + ".tmp";
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        const auto& info = arrays.info;
        const bool ok = stream && WritePod(stream, CacheMagic) && WritePod(stream, signature) &&
            WritePod(stream, arrays.size) && WritePod(stream, arrays.mips) &&
            WritePod(stream, arrays.layers) && WritePod(stream, arrays.blockCompressed) &&
            WritePod(stream, info.albedoMask) && WritePod(stream, info.normalMask) &&
            WritePod(stream, info.roughnessMask) && WritePod(stream, info.heightMask) &&
            WritePod(stream, info.meanAlbedo) && WriteBlob(stream, arrays.albedoHeight) &&
            WriteBlob(stream, arrays.normal) && WriteBlob(stream, arrays.roughAo);
        if (!ok) { stream.close(); std::filesystem::remove(temporary, error); return; }
    }
    std::filesystem::rename(temporary, path, error);
    if (error) std::filesystem::remove(temporary, error);
}

std::shared_ptr<CpuArrays> LoadCache(const std::filesystem::path& path, uint64_t signature)
{
    if (path.empty()) return nullptr;
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return nullptr;
    auto arrays = std::make_shared<CpuArrays>();
    auto& info = arrays->info;
    uint64_t magic = 0, stored = 0;
    if (!ReadPod(stream, magic) || magic != CacheMagic ||
        !ReadPod(stream, stored) || stored != signature ||
        !ReadPod(stream, arrays->size) || !ReadPod(stream, arrays->mips) ||
        !ReadPod(stream, arrays->layers) || !ReadPod(stream, arrays->blockCompressed) ||
        !ReadPod(stream, info.albedoMask) || !ReadPod(stream, info.normalMask) ||
        !ReadPod(stream, info.roughnessMask) || !ReadPod(stream, info.heightMask) ||
        !ReadPod(stream, info.meanAlbedo) || !ReadBlob(stream, arrays->albedoHeight) ||
        !ReadBlob(stream, arrays->normal) || !ReadBlob(stream, arrays->roughAo))
        return nullptr;
    info.sliceSize = arrays->size;
    return arrays;
}

std::shared_ptr<CpuArrays> Build(const std::vector<LayerSources>& sources, bool blockCompressed)
{
    if (sources.empty()) return nullptr;
    const uint32_t cap = blockCompressed ? TerrainLayerArrays::MaxSliceSizeCompressed
                                         : TerrainLayerArrays::MaxSliceSize;
    const uint64_t signature = SourceSignature(sources, blockCompressed, cap);
    const auto cachePath = CachePath(signature);
    if (auto cached = LoadCache(cachePath, signature)) return cached;

    // Slice size: the largest source, as a power of two, capped.
    uint32_t largest = 4;
    for (const auto& s : sources)
        for (const std::string* path : { &s.albedo, &s.normal, &s.roughness, &s.occlusion, &s.height })
            largest = std::max(largest, SourceSize(*path, cap));
    uint32_t size = 4;
    while (size * 2 <= std::min(largest, cap)) size *= 2;
    uint32_t mips = 1;
    while ((size >> (mips - 1)) > 1) ++mips;

    auto result = std::make_shared<CpuArrays>();
    result->size = size;
    result->mips = mips;
    result->layers = static_cast<uint32_t>(sources.size());
    result->blockCompressed = blockCompressed;
    result->info.sliceSize = size;
    // One layer at a time keeps the float working set to a single slice.
    for (size_t layer = 0; layer < sources.size(); ++layer)
    {
        const auto& s = sources[layer];
        const uint32_t bit = 1u << layer;
        Slice slice;
        for (Image* image : { &slice.albedo, &slice.normal, &slice.roughness })
        {
            image->width = image->height = size;
            image->texels.assign(size_t(size) * size * 4, 0.f);
        }
        const size_t texels = size_t(size) * size;
        for (size_t i = 0; i < texels; ++i)
        {
            float* a = &slice.albedo.texels[i * 4];
            a[0] = a[1] = a[2] = 1.f; a[3] = 0.5f;            // white, mid height
            float* n = &slice.normal.texels[i * 4];
            n[2] = 1.f; n[3] = 1.f;                            // flat, no occlusion
            slice.roughness.texels[i * 4] = 1.f;               // roughness 1
        }
        Image image;
        if (Decode(s.albedo, true, size, image))
        {
            result->info.albedoMask |= bit;
            for (size_t i = 0; i < texels; ++i)
                for (uint32_t c = 0; c < 3; ++c)
                    slice.albedo.texels[i * 4 + c] = image.texels[i * 4 + c];
        }
        if (Decode(s.height, false, size, image))
        {
            result->info.heightMask |= bit;
            for (size_t i = 0; i < texels; ++i)
                slice.albedo.texels[i * 4 + 3] = image.texels[i * 4];
        }
        if (Decode(s.normal, false, size, image))
        {
            result->info.normalMask |= bit;
            for (size_t i = 0; i < texels; ++i)
            {
                glm::vec3 n(image.texels[i * 4] * 2.f - 1.f, image.texels[i * 4 + 1] * 2.f - 1.f, 0.f);
                n.z = std::sqrt(std::max(0.f, 1.f - n.x * n.x - n.y * n.y));
                n = glm::normalize(n);
                slice.normal.texels[i * 4] = n.x;
                slice.normal.texels[i * 4 + 1] = n.y;
                slice.normal.texels[i * 4 + 2] = n.z;
            }
        }
        // Roughness in the metallic-roughness map's G channel (glTF).
        if (Decode(s.roughness, false, size, image))
        {
            result->info.roughnessMask |= bit;
            for (size_t i = 0; i < texels; ++i)
            {
                const float r = image.texels[i * 4 + 1];
                slice.roughness.texels[i * 4] = r * r * r * r; // GGX alpha^2
            }
        }
        if (Decode(s.occlusion, false, size, image))
        {
            result->info.roughnessMask |= bit;
            for (size_t i = 0; i < texels; ++i)
                slice.normal.texels[i * 4 + 3] = image.texels[i * 4];
        }
        image = {};
        const auto levels = BuildMips(std::move(slice), mips, result->info.meanAlbedo[layer]);
        for (const auto& level : levels)
            AppendLevel(level, blockCompressed, *result);
    }
    SaveCache(cachePath, signature, *result);
    return result;
}

std::string TexturePath(const std::shared_ptr<Texture>& texture)
{
    return texture ? texture->GetFilePath() : std::string();
}
}

const TerrainLayerArrays::Arrays* TerrainLayerArrays::Acquire(
    const std::vector<std::shared_ptr<Material>>& materials,
    Engine::Graphics::IGraphicsProvider* provider)
{
    if (!provider || materials.empty()) return nullptr;
    std::vector<LayerSources> sources;
    std::string key;
    for (size_t layer = 0; layer < std::min<size_t>(materials.size(), MaxLayers); ++layer)
    {
        LayerSources source;
        if (const auto* material = materials[layer].get())
        {
            source.albedo = TexturePath(material->baseColorTexture);
            source.normal = TexturePath(material->normalTexture);
            source.roughness = TexturePath(material->metallicRoughnessTexture);
            source.occlusion = TexturePath(material->occlusionTexture);
            source.height = TexturePath(material->heightTexture);
        }
        for (const std::string* path : { &source.albedo, &source.normal,
            &source.roughness, &source.occlusion, &source.height })
            key += *path + '|';
        key += ';';
        sources.push_back(std::move(source));
    }

    std::lock_guard lock(CacheMutex());
    auto& entry = Cache()[key];
    if (!entry || entry->provider != provider)
    {
        // Keyed by texture paths here; the disk cache also checks file
        // sizes and write times. Clear() rebuilds after source edits.
        entry = std::make_shared<Entry>();
        entry->provider = provider;
        auto* factory = provider->GetTextureFactory();
        const bool blockCompressed = factory && factory->SupportsBlockCompression();
        entry->build = Engine::Rendering::RenderJobs::Get().Submit(
            [sources, blockCompressed] { return Build(sources, blockCompressed); }).share();
    }
    if (entry->ready) return &entry->arrays;
    if (entry->failed) return nullptr;
    if (entry->build.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
        return nullptr;
    std::shared_ptr<CpuArrays> cpu;
    try { cpu = entry->build.get(); }
    catch (...) { entry->failed = true; return nullptr; }
    if (!cpu) { entry->failed = true; return nullptr; }
    auto* factory = provider->GetTextureFactory();
    if (!factory) return nullptr;
    const auto colorFormat = cpu->blockCompressed ? GraphicsTextureFormat::Bc7 : GraphicsTextureFormat::Rgba8;
    const auto pairFormat = cpu->blockCompressed ? GraphicsTextureFormat::Bc5 : GraphicsTextureFormat::Rgba8;
    auto albedo = factory->CreateTexture2DArray(cpu->size, cpu->size, cpu->layers,
        cpu->albedoHeight.data(), cpu->mips, colorFormat, true);
    auto normal = albedo ? factory->CreateTexture2DArray(cpu->size, cpu->size, cpu->layers,
        cpu->normal.data(), cpu->mips, pairFormat, false) : nullptr;
    auto roughAo = normal ? factory->CreateTexture2DArray(cpu->size, cpu->size, cpu->layers,
        cpu->roughAo.data(), cpu->mips, pairFormat, false) : nullptr;
    // A full upload queue returns null; the CPU copy stays for a retry.
    if (!albedo || !normal || !roughAo) return nullptr;
    entry->arrays = cpu->info;
    entry->arrays.albedoHeight = std::move(albedo);
    entry->arrays.normal = std::move(normal);
    entry->arrays.roughAo = std::move(roughAo);
    entry->ready = true;
    entry->build = {};
    return &entry->arrays;
}

void TerrainLayerArrays::Clear()
{
    std::lock_guard lock(CacheMutex());
    Cache().clear();
}
}

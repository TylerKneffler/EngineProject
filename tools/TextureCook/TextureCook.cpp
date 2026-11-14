// TextureCook: converts a source image into a block-compressed KTX2 with a
// full, usage-aware mip chain.
//
//   TextureCook [options] <source> [<output.ktx2>]
//
//   --usage color|normal|mask|height|single|auto  (default: .meta, then auto)
//       color   BC7 sRGB, mips averaged in linear light
//       normal  BC5 (XY; the shader rebuilds Z), mips average the unnormalised
//               vectors and renormalise
//       mask    BC7 linear (ORM / metallic-roughness)
//       height  BC4 from R (white = raised)
//       single  BC4 from R (any other one-channel mask)
//       auto    from the file name (_Normal, _Height/_Displacement, _Roughness...)
//   --toksvig <normal source>  mask only: widen roughness in each mip by the
//       normal variance lost at that mip (removes distant specular sparkle)
//   --roughness-channel r|g|b|a  channel holding roughness (default g, glTF)
//   --height-mips average|max     (default average)
//   --keep-size     fail instead of resampling non power-of-two sources
//   --no-zstd       write without Zstandard supercompression
//
// Non power-of-two sources are resampled to the nearest power of two so every
// mip level is a whole number of 4x4 blocks, which D3D requires of each level
// that can become the top of a streamed mip tail.
//
// The usage may also come from the source's .meta sidecar:
//   "importSettings": { "textureUsage": "normal" }

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <ktx.h>
#include <bc7enc.h>
#include <rgbcx.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace
{
enum class Usage { Auto, Color, Normal, Mask, Height, Single };

struct Image
{
    uint32_t width = 0, height = 0;
    std::vector<float> texels; // RGBA, 0..1 (sRGB-encoded for color input)
    float* At(uint32_t x, uint32_t y) { return texels.data() + (size_t(y) * width + x) * 4; }
    const float* At(uint32_t x, uint32_t y) const { return texels.data() + (size_t(y) * width + x) * 4; }
};

std::string Lower(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(),
        [](unsigned char c) { return char(std::tolower(c)); });
    return text;
}

bool ParseUsage(const std::string& name, Usage& usage)
{
    const std::string value = Lower(name);
    if (value == "auto") usage = Usage::Auto;
    else if (value == "color" || value == "albedo" || value == "basecolor") usage = Usage::Color;
    else if (value == "normal") usage = Usage::Normal;
    else if (value == "mask" || value == "orm" || value == "linear") usage = Usage::Mask;
    else if (value == "height" || value == "displacement") usage = Usage::Height;
    else if (value == "single") usage = Usage::Single;
    else return false;
    return true;
}

const char* UsageName(Usage usage)
{
    switch (usage)
    {
    case Usage::Color: return "color";
    case Usage::Normal: return "normal";
    case Usage::Mask: return "mask";
    case Usage::Height: return "height";
    case Usage::Single: return "single";
    default: return "auto";
    }
}

// Common PBR set naming (ambientCG, Poly Haven, Substance exports).
Usage GuessUsage(const fs::path& path)
{
    const std::string stem = Lower(path.stem().string());
    const auto has = [&](std::initializer_list<const char*> keys) {
        for (const char* key : keys)
            if (stem.find(key) != std::string::npos) return true;
        return false;
    };
    if (has({"normal", "_nrm", "_nor", "_norm"}) || stem.ends_with("_n")) return Usage::Normal;
    if (has({"height", "displacement", "_disp", "_bump"})) return Usage::Height;
    if (has({"roughness", "_rough", "metallic", "metalness", "_metal", "occlusion",
             "_ao", "_orm", "_arm", "_mask", "specular", "gloss"})) return Usage::Mask;
    return Usage::Color;
}

// Minimal read of importSettings.textureUsage from the .meta sidecar.
bool MetaUsage(const fs::path& source, Usage& usage)
{
    std::ifstream file(source.string() + ".meta", std::ios::binary);
    if (!file) return false;
    std::stringstream text;
    text << file.rdbuf();
    const std::string json = text.str();
    std::smatch match;
    static const std::regex pattern("\"textureUsage\"\\s*:\\s*\"([A-Za-z]+)\"");
    return std::regex_search(json, match, pattern) && ParseUsage(match[1].str(), usage);
}

bool LoadImage(const fs::path& path, Image& image, std::string& error)
{
    int width = 0, height = 0, channels = 0;
    stbi_us* data = stbi_load_16(path.string().c_str(), &width, &height, &channels, 4);
    if (!data || width <= 0 || height <= 0)
    {
        error = "cannot decode " + path.string() + ": " +
            (stbi_failure_reason() ? stbi_failure_reason() : "unknown");
        if (data) stbi_image_free(data);
        return false;
    }
    image.width = uint32_t(width);
    image.height = uint32_t(height);
    image.texels.resize(size_t(width) * height * 4);
    for (size_t i = 0; i < image.texels.size(); ++i)
        image.texels[i] = data[i] / 65535.f;
    stbi_image_free(data);
    return true;
}

float SrgbToLinear(float v)
{
    return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f);
}

float LinearToSrgb(float v)
{
    v = std::clamp(v, 0.f, 1.f);
    return v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.f / 2.4f) - 0.055f;
}

uint32_t NearestPowerOfTwo(uint32_t value)
{
    uint32_t lower = 1;
    while (lower * 2 <= value) lower *= 2;
    const uint32_t upper = lower * 2;
    // Compare in log space so 1254 -> 1024 and 1600 -> 2048.
    return std::log2(double(value)) - std::log2(double(lower)) <
        std::log2(double(upper)) - std::log2(double(value)) ? lower : upper;
}

// Area-weighted (box) resample. Exact for downscaling; for upscaling the
// footprint is narrower than a texel, which reduces to bilinear-free point
// sampling, so upscales use bilinear instead.
Image Resample(const Image& source, uint32_t width, uint32_t height)
{
    Image target;
    target.width = width;
    target.height = height;
    target.texels.assign(size_t(width) * height * 4, 0.f);
    const double sx = double(source.width) / width, sy = double(source.height) / height;
    for (uint32_t y = 0; y < height; ++y)
    for (uint32_t x = 0; x < width; ++x)
    {
        float* out = target.At(x, y);
        if (sx <= 1.0 && sy <= 1.0)
        {
            const double fx = std::clamp((x + 0.5) * sx - 0.5, 0.0, double(source.width - 1));
            const double fy = std::clamp((y + 0.5) * sy - 0.5, 0.0, double(source.height - 1));
            const uint32_t x0 = uint32_t(fx), y0 = uint32_t(fy);
            const uint32_t x1 = std::min(x0 + 1, source.width - 1), y1 = std::min(y0 + 1, source.height - 1);
            const float tx = float(fx - x0), ty = float(fy - y0);
            for (int c = 0; c < 4; ++c)
                out[c] = (source.At(x0, y0)[c] * (1 - tx) + source.At(x1, y0)[c] * tx) * (1 - ty) +
                         (source.At(x0, y1)[c] * (1 - tx) + source.At(x1, y1)[c] * tx) * ty;
            continue;
        }
        const double left = x * sx, right = (x + 1) * sx, top = y * sy, bottom = (y + 1) * sy;
        double sum[4]{}, weight = 0;
        for (uint32_t py = uint32_t(top); py < std::min<double>(std::ceil(bottom), source.height); ++py)
        {
            const double wy = std::min<double>(py + 1, bottom) - std::max<double>(py, top);
            for (uint32_t px = uint32_t(left); px < std::min<double>(std::ceil(right), source.width); ++px)
            {
                const double w = wy * (std::min<double>(px + 1, right) - std::max<double>(px, left));
                if (w <= 0) continue;
                for (int c = 0; c < 4; ++c) sum[c] += source.At(px, py)[c] * w;
                weight += w;
            }
        }
        for (int c = 0; c < 4; ++c) out[c] = float(sum[c] / std::max(weight, 1e-12));
    }
    return target;
}

// 2x2 box reduction of one level (dimensions halve, clamped at 1).
Image Reduce(const Image& source, bool maxFilter = false)
{
    Image target;
    target.width = std::max(1u, source.width / 2);
    target.height = std::max(1u, source.height / 2);
    target.texels.resize(size_t(target.width) * target.height * 4);
    for (uint32_t y = 0; y < target.height; ++y)
    for (uint32_t x = 0; x < target.width; ++x)
    {
        float* out = target.At(x, y);
        for (int c = 0; c < 4; ++c)
        {
            float sum = 0.f, peak = 0.f;
            for (uint32_t dy = 0; dy < 2; ++dy)
            for (uint32_t dx = 0; dx < 2; ++dx)
            {
                const float v = source.At(std::min(source.width - 1, x * 2 + dx),
                                          std::min(source.height - 1, y * 2 + dy))[c];
                sum += v;
                peak = std::max(peak, v);
            }
            out[c] = maxFilter ? peak : sum * 0.25f;
        }
    }
    return target;
}

// Mip chain in the space mips should be averaged in. For normals the chain
// holds unnormalised vectors in [-1,1]; their length is the Toksvig factor.
std::vector<Image> BuildChain(Image base, Usage usage, bool heightMax)
{
    if (usage == Usage::Color)
        for (size_t i = 0; i < base.texels.size(); i += 4)
            for (int c = 0; c < 3; ++c) base.texels[i + c] = SrgbToLinear(base.texels[i + c]);
    if (usage == Usage::Normal)
        for (size_t i = 0; i < base.texels.size(); i += 4)
        {
            float n[3];
            for (int c = 0; c < 3; ++c) n[c] = base.texels[i + c] * 2.f - 1.f;
            const float length = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
            for (int c = 0; c < 3; ++c) base.texels[i + c] = length > 1e-6f ? n[c] / length : (c == 2 ? 1.f : 0.f);
        }
    std::vector<Image> chain;
    chain.push_back(std::move(base));
    while (chain.back().width > 1 || chain.back().height > 1)
        chain.push_back(Reduce(chain.back(), heightMax && usage == Usage::Height));
    return chain;
}

// Karis-style Toksvig: GGX alpha^2 grows by the variance implied by the
// shortened average normal. Roughness is perceptual (alpha = r^2).
void ApplyToksvig(std::vector<Image>& mask, const std::vector<Image>& normals, int channel)
{
    for (size_t level = 1; level < mask.size(); ++level)
    {
        // Match levels by size; the normal chain may start larger or smaller.
        const Image* normal = nullptr;
        for (const auto& candidate : normals)
            if (candidate.width == mask[level].width && candidate.height == mask[level].height)
                normal = &candidate;
        if (!normal) continue;
        for (uint32_t y = 0; y < mask[level].height; ++y)
        for (uint32_t x = 0; x < mask[level].width; ++x)
        {
            const float* n = normal->At(x, y);
            const float length = std::clamp(std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]), 1e-4f, 1.f);
            const float variance = (1.f - length) / length;
            float& roughness = mask[level].At(x, y)[channel];
            const float alpha2 = std::pow(std::clamp(roughness, 0.f, 1.f), 4.f);
            const float widened = std::min(1.f, alpha2 + std::min(2.f * variance, 0.5f));
            roughness = std::pow(widened, 0.25f);
        }
    }
}

uint8_t ToByte(float v) { return uint8_t(std::clamp(v * 255.f + 0.5f, 0.f, 255.f)); }

// Final 8-bit RGBA for the encoder, in the stored (shader-visible) encoding.
std::vector<uint8_t> Encode8(const Image& level, Usage usage)
{
    std::vector<uint8_t> out(size_t(level.width) * level.height * 4);
    for (size_t i = 0, p = 0; i < level.texels.size(); i += 4, p += 4)
    {
        const float* t = level.texels.data() + i;
        if (usage == Usage::Color)
        {
            for (int c = 0; c < 3; ++c) out[p + c] = ToByte(LinearToSrgb(t[c]));
            out[p + 3] = ToByte(t[3]);
        }
        else if (usage == Usage::Normal)
        {
            const float length = std::sqrt(t[0] * t[0] + t[1] * t[1] + t[2] * t[2]);
            const float s = length > 1e-6f ? 1.f / length : 0.f;
            out[p + 0] = ToByte(t[0] * s * 0.5f + 0.5f);
            out[p + 1] = ToByte(t[1] * s * 0.5f + 0.5f);
            out[p + 2] = ToByte(length > 1e-6f ? t[2] * s * 0.5f + 0.5f : 1.f);
            out[p + 3] = 255;
        }
        else
            for (int c = 0; c < 4; ++c) out[p + c] = ToByte(t[c]);
    }
    return out;
}

enum class Block { Bc4, Bc5, Bc7 };

std::vector<uint8_t> CompressLevel(const std::vector<uint8_t>& rgba, uint32_t width,
    uint32_t height, Block block, bool perceptual)
{
    const uint32_t bw = (width + 3) / 4, bh = (height + 3) / 4;
    const uint32_t blockBytes = block == Block::Bc4 ? 8 : 16;
    std::vector<uint8_t> out(size_t(bw) * bh * blockBytes);
    bc7enc_compress_block_params params;
    bc7enc_compress_block_params_init(&params);
    if (!perceptual) bc7enc_compress_block_params_init_linear_weights(&params);
    params.m_uber_level = 2;
    params.m_max_partitions = BC7ENC_MAX_PARTITIONS;
    for (uint32_t by = 0; by < bh; ++by)
    for (uint32_t bx = 0; bx < bw; ++bx)
    {
        uint8_t pixels[16 * 4];
        for (uint32_t y = 0; y < 4; ++y)
        for (uint32_t x = 0; x < 4; ++x)
        {
            // Edge texels repeat for levels smaller than one block.
            const uint32_t sx = std::min(width - 1, bx * 4 + x), sy = std::min(height - 1, by * 4 + y);
            std::memcpy(pixels + (y * 4 + x) * 4, rgba.data() + (size_t(sy) * width + sx) * 4, 4);
        }
        uint8_t* destination = out.data() + (size_t(by) * bw + bx) * blockBytes;
        if (block == Block::Bc7) bc7enc_compress_block(destination, pixels, &params);
        else if (block == Block::Bc5) rgbcx::encode_bc5_hq(destination, pixels, 0, 1, 4);
        else rgbcx::encode_bc4_hq(destination, pixels, 4);
    }
    return out;
}

int Fail(const std::string& message)
{
    std::fprintf(stderr, "TextureCook: %s\n", message.c_str());
    return 1;
}
}

int main(int argc, char** argv)
{
    Usage usage = Usage::Auto;
    bool usageGiven = false, keepSize = false, zstd = true, heightMax = false;
    int roughnessChannel = 1;
    fs::path source, output, toksvigNormal;
    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        const auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : std::string(); };
        if (arg == "--usage") { if (!ParseUsage(next(), usage)) return Fail("unknown --usage"); usageGiven = true; }
        else if (arg == "--toksvig") toksvigNormal = next();
        else if (arg == "--roughness-channel")
        {
            const std::string c = Lower(next());
            roughnessChannel = c == "r" ? 0 : c == "g" ? 1 : c == "b" ? 2 : c == "a" ? 3 : -1;
            if (roughnessChannel < 0) return Fail("--roughness-channel takes r, g, b or a");
        }
        else if (arg == "--height-mips") heightMax = Lower(next()) == "max";
        else if (arg == "--keep-size") keepSize = true;
        else if (arg == "--no-zstd") zstd = false;
        else if (!arg.empty() && arg[0] == '-') return Fail("unknown option " + arg);
        else if (source.empty()) source = arg;
        else if (output.empty()) output = arg;
        else return Fail("too many arguments");
    }
    if (source.empty())
        return Fail("usage: TextureCook [--usage color|normal|mask|height|single|auto] "
                    "[--toksvig normal.png] [--roughness-channel g] [--height-mips average|max] "
                    "[--keep-size] [--no-zstd] <source> [<output.ktx2>]");
    if (output.empty()) output = fs::path(source).replace_extension(".ktx2");
    if (!usageGiven) MetaUsage(source, usage);
    if (usage == Usage::Auto) usage = GuessUsage(source);
    if (!toksvigNormal.empty() && usage != Usage::Mask)
        return Fail("--toksvig applies to mask (roughness) textures only");

    std::string error;
    Image base;
    if (!LoadImage(source, base, error)) return Fail(error);
    const uint32_t sourceWidth = base.width, sourceHeight = base.height;
    const uint32_t width = NearestPowerOfTwo(base.width), height = NearestPowerOfTwo(base.height);
    if (width != base.width || height != base.height)
    {
        if (keepSize) return Fail("source is not a power of two and --keep-size was given");
        base = Resample(base, width, height);
    }

    std::vector<Image> chain = BuildChain(std::move(base), usage, heightMax);
    if (!toksvigNormal.empty())
    {
        Image normal;
        if (!LoadImage(toksvigNormal, normal, error)) return Fail(error);
        if (normal.width != width || normal.height != height)
            normal = Resample(normal, NearestPowerOfTwo(normal.width), NearestPowerOfTwo(normal.height));
        ApplyToksvig(chain, BuildChain(std::move(normal), Usage::Normal, false), roughnessChannel);
    }

    const Block block = usage == Usage::Normal ? Block::Bc5
        : usage == Usage::Height || usage == Usage::Single ? Block::Bc4 : Block::Bc7;
    const uint32_t vkFormat = block == Block::Bc5 ? 141u
        : block == Block::Bc4 ? 139u : usage == Usage::Color ? 146u : 145u;

    bc7enc_compress_block_init();
    rgbcx::init();

    ktxTextureCreateInfo info{};
    info.vkFormat = vkFormat;
    info.baseWidth = width;
    info.baseHeight = height;
    info.baseDepth = 1;
    info.numDimensions = 2;
    info.numLevels = uint32_t(chain.size());
    info.numLayers = 1;
    info.numFaces = 1;
    info.isArray = KTX_FALSE;
    info.generateMipmaps = KTX_FALSE;
    ktxTexture2* texture = nullptr;
    if (ktxTexture2_Create(&info, KTX_TEXTURE_CREATE_ALLOC_STORAGE, &texture) != KTX_SUCCESS)
        return Fail("ktxTexture2_Create failed");
    struct Destroy { ktxTexture2* t; ~Destroy() { ktxTexture_Destroy(ktxTexture(t)); } } destroy{texture};

    uint64_t compressedBytes = 0;
    for (uint32_t level = 0; level < chain.size(); ++level)
    {
        const auto& image = chain[level];
        const auto blocks = CompressLevel(Encode8(image, usage), image.width, image.height,
            block, usage == Usage::Color);
        compressedBytes += blocks.size();
        if (ktxTexture_SetImageFromMemory(ktxTexture(texture), level, 0, 0,
                blocks.data(), blocks.size()) != KTX_SUCCESS)
            return Fail("ktxTexture_SetImageFromMemory failed at level " + std::to_string(level));
    }
    ktxHashList_AddKVPair(&texture->kvDataHead, KTX_WRITER_KEY,
        uint32_t(std::strlen("EngineProject TextureCook") + 1), "EngineProject TextureCook");
    if (zstd && ktxTexture2_DeflateZstd(texture, 18) != KTX_SUCCESS)
        return Fail("Zstandard supercompression failed");
    if (output.has_parent_path()) fs::create_directories(output.parent_path());
    if (ktxTexture_WriteToNamedFile(ktxTexture(texture), output.string().c_str()) != KTX_SUCCESS)
        return Fail("cannot write " + output.string());

    const uint64_t rgbaBytes = uint64_t(width) * height * 4 * 4 / 3;
    std::printf("%s -> %s  [%s, %ux%u%s, %u mips, %.1f MiB GPU vs %.1f MiB RGBA8]\n",
        source.string().c_str(), output.string().c_str(), UsageName(usage), width, height,
        (width != sourceWidth || height != sourceHeight) ? " resampled" : "",
        uint32_t(chain.size()), compressedBytes / 1048576.0, rgbaBytes / 1048576.0);
    return 0;
}

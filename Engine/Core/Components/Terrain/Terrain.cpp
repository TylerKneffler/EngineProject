#include <map>
#include <mutex>
#include <miniz.h>
#include "Core/Graphics/IGraphicsProvider.h"
#include "Core/Graphics/IGraphicsTexture.h"
#include "Terrain.h"
#include "Core/Brushes/BrushRules.h"
#include "Core/Components/Terrain/TerrainBrushShape.h"
#include "Core/Components/Terrain/TerrainObjectLayer.h"
#include "Core/Components/Terrain/TerrainRegionEdit.h"
#include "Core/Components/Terrain/TerrainSurfaceSample.h"
#include "Core/Components/Terrain/VolumeTerrain.h"
#include "Core/Components/Terrain/TerrainManager.h"
#include "Core/Scene/Object.h"
#include "Core/Scene/Scene.h"
#include "Core/Components/Mesh/Mesh.h"
#include "Core/Components/Materials/Material.h"
#include "Core/Components/Materials/Texture.h"
#include "Core/Graphics/IGraphicsProvider.h"
#include "Core/Inspection/IInspectorUi.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <type_traits>

namespace
{
template<class T> bool WriteValue(std::ofstream& stream, const T& value)
{
    static_assert(std::is_trivially_copyable_v<T>);
    stream.write(reinterpret_cast<const char*>(&value), sizeof(value));
    return static_cast<bool>(stream);
}
template<class T> bool ReadValue(std::istream& stream, T& value)
{
    static_assert(std::is_trivially_copyable_v<T>);
    stream.read(reinterpret_cast<char*>(&value), sizeof(value));
    return static_cast<bool>(stream);
}
template<class T> bool WriteVector(std::ofstream& stream, const std::vector<T>& values)
{
    static_assert(std::is_trivially_copyable_v<T>);
    const uint64_t count = values.size();
    if (!WriteValue(stream, count)) return false;
    stream.write(reinterpret_cast<const char*>(values.data()),
        static_cast<std::streamsize>(count * sizeof(T)));
    return static_cast<bool>(stream);
}
template<class T> bool ReadVector(std::ifstream& stream, std::vector<T>& values,
    uint64_t expected)
{
    static_assert(std::is_trivially_copyable_v<T>);
    uint64_t count = 0;
    if (!ReadValue(stream, count) || count != expected) return false;
    values.resize(static_cast<size_t>(count));
    stream.read(reinterpret_cast<char*>(values.data()),
        static_cast<std::streamsize>(count * sizeof(T)));
    return static_cast<bool>(stream);
}
int64_t MaterialStamp(const std::string& material)
{
    if (material.empty()) return 0;
    std::error_code error;
    const auto time = std::filesystem::last_write_time(
        Engine::Components::Mesh::ResolveFilePath(material), error);
    return error ? 0 : static_cast<int64_t>(time.time_since_epoch().count());
}
}

namespace Engine::Components
{
namespace
{
constexpr char Base64Alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string Base64Encode(const std::vector<uint8_t>& data)
{
    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);
    for (size_t i = 0; i < data.size(); i += 3)
    {
        const uint32_t a = data[i];
        const uint32_t b = i + 1 < data.size() ? data[i + 1] : 0;
        const uint32_t c = i + 2 < data.size() ? data[i + 2] : 0;
        const uint32_t triple = (a << 16) | (b << 8) | c;
        out += Base64Alphabet[(triple >> 18) & 63];
        out += Base64Alphabet[(triple >> 12) & 63];
        out += i + 1 < data.size() ? Base64Alphabet[(triple >> 6) & 63] : '=';
        out += i + 2 < data.size() ? Base64Alphabet[triple & 63] : '=';
    }
    return out;
}

bool Base64Decode(const std::string& text, std::vector<uint8_t>& out)
{
    int8_t lookup[256];
    std::fill(std::begin(lookup), std::end(lookup), int8_t(-1));
    for (int i = 0; i < 64; ++i) lookup[uint8_t(Base64Alphabet[i])] = int8_t(i);
    out.clear();
    uint32_t buffer = 0;
    int bits = 0;
    for (const char character : text)
    {
        if (character == '=' || character == ' ' || character == '\n' ||
            character == '\r' || character == '\t') continue;
        const int8_t value = lookup[uint8_t(character)];
        if (value < 0) return false;
        buffer = (buffer << 6) | uint32_t(value);
        bits += 6;
        if (bits >= 8) { bits -= 8; out.push_back(uint8_t((buffer >> bits) & 0xff)); }
    }
    return true;
}

// Paint maps serialize as 8-bit weights: a size header, then deflate.
std::string EncodePaintMap(const std::vector<float>& weights)
{
    std::vector<uint8_t> bytes(weights.size());
    for (size_t i = 0; i < weights.size(); ++i)
        bytes[i] = uint8_t(std::lround(std::clamp(weights[i], 0.f, 1.f) * 255.f));
    mz_ulong length = mz_compressBound(mz_ulong(bytes.size()));
    std::vector<uint8_t> packed(8 + length);
    const uint64_t raw = bytes.size();
    std::memcpy(packed.data(), &raw, 8);
    if (mz_compress2(packed.data() + 8, &length, bytes.data(), mz_ulong(bytes.size()),
            MZ_BEST_COMPRESSION) != MZ_OK) return {};
    packed.resize(8 + length);
    return Base64Encode(packed);
}

bool DecodePaintMap(const std::string& text, size_t expected, std::vector<float>& weights)
{
    std::vector<uint8_t> packed;
    if (!Base64Decode(text, packed) || packed.size() < 8) return false;
    uint64_t raw = 0;
    std::memcpy(&raw, packed.data(), 8);
    if (raw != expected) return false;
    std::vector<uint8_t> bytes(static_cast<size_t>(raw));
    mz_ulong length = mz_ulong(bytes.size());
    if (mz_uncompress(bytes.data(), &length, packed.data() + 8,
            mz_ulong(packed.size() - 8)) != MZ_OK || length != bytes.size()) return false;
    weights.resize(bytes.size());
    for (size_t i = 0; i < bytes.size(); ++i) weights[i] = bytes[i] / 255.f;
    return true;
}

// Bilinear sample of a vertex-aligned weight grid at fractional sample
// coordinates, clamped to the grid.
void SampleWeightGrid(const std::vector<float>& grid, int width, int height,
    size_t layerCount, float x, float y, float* out)
{
    x = std::clamp(x, 0.f, float(width - 1));
    y = std::clamp(y, 0.f, float(height - 1));
    const int x0 = std::min(int(x), width - 1), y0 = std::min(int(y), height - 1);
    const int x1 = std::min(x0 + 1, width - 1), y1 = std::min(y0 + 1, height - 1);
    const float tx = x - x0, ty = y - y0;
    const auto at = [&](int col, int row, size_t layer)
    { return grid[(size_t(row) * width + col) * layerCount + layer]; };
    for (size_t layer = 0; layer < layerCount; ++layer)
        out[layer] = (at(x0, y0, layer) * (1 - tx) + at(x1, y0, layer) * tx) * (1 - ty) +
            (at(x0, y1, layer) * (1 - tx) + at(x1, y1, layer) * tx) * ty;
}

void NormalizeSample(float* sample, size_t layerCount)
{
    float sum = 0.f;
    for (size_t l = 0; l < layerCount; ++l) sum += sample[l] = std::clamp(sample[l], 0.f, 1.f);
    if (sum < 1e-5f) { sample[0] = 1.f; return; }
    for (size_t l = 0; l < layerCount; ++l) sample[l] /= sum;
}

// One paint or erase step on a single weight sample.
bool PaintSample(float* sample, size_t layerCount, int layer, float amount, bool erase)
{
    if (amount <= 0.f) return false;
    if (erase)
    {
        // Hand the removed weight to the other layers in proportion; with
        // none left, the first other layer takes it.
        if (layerCount < 2) return false;
        const float removed = sample[layer] * amount;
        if (removed <= 0.f) return false;
        float others = 0.f;
        for (size_t l = 0; l < layerCount; ++l)
            if (int(l) != layer) others += sample[l];
        sample[layer] -= removed;
        if (others > 1e-5f)
        {
            for (size_t l = 0; l < layerCount; ++l)
                if (int(l) != layer) sample[l] += removed * sample[l] / others;
        }
        else
            sample[layer == 0 ? 1 : 0] += removed;
        return true;
    }
    const float old = sample[layer];
    for (size_t l = 0; l < layerCount; ++l) sample[l] *= 1.f - amount;
    sample[layer] += amount;
    return sample[layer] != old;
}
}

Terrain::Terrain()
{
    SetTypeName(COMPONENT_TYPE_NAME(Terrain));
    singlecomponent = true;
    layers.push_back({"", 0.f, 10.f, {.35f, .55f, .25f}});
    Resize(cellsX, cellsY, cellWidth, cellDepth);
}

bool Terrain::HasPaintMap() const
{
    return paintResolution > 0 && !layers.empty() && paintWeights.size() ==
        size_t(PaintWidth()) * PaintHeight() * layers.size();
}

void Terrain::EnsurePaintMap()
{
    if (paintResolution <= 0) { paintWeights.clear(); return; }
    while (paintResolution > 1 && (cellsX * paintResolution + 1 > MaxPaintSamples ||
        cellsY * paintResolution + 1 > MaxPaintSamples)) paintResolution /= 2;
    if (HasPaintMap() || layers.empty() ||
        weights.size() != heights.size() * layers.size()) return;
    // Seed from the vertex weights.
    const int width = PaintWidth(), height = PaintHeight();
    paintWeights.assign(size_t(width) * height * layers.size(), 0.f);
    for (int row = 0; row < height; ++row)
        for (int col = 0; col < width; ++col)
            SampleWeightGrid(weights, cellsX + 1, cellsY + 1, layers.size(),
                float(col) / paintResolution, float(row) / paintResolution,
                &paintWeights[(size_t(row) * width + col) * layers.size()]);
    m_paintTextureDirty = true;
}

void Terrain::SyncVertexWeightsFromPaint()
{
    if (!HasPaintMap()) return;
    const size_t layerCount = layers.size();
    weights.resize(heights.size() * layerCount, 0.f);
    const int width = PaintWidth();
    for (int row = 0; row <= cellsY; ++row)
        for (int col = 0; col <= cellsX; ++col)
        {
            const float* source = &paintWeights[(size_t(row * paintResolution) * width +
                size_t(col * paintResolution)) * layerCount];
            std::copy_n(source, layerCount,
                &weights[(size_t(row) * (cellsX + 1) + col) * layerCount]);
        }
}

void Terrain::SetPaintResolution(int resolution)
{
    resolution = resolution <= 0 ? 0 : resolution <= 2 ? 2 : resolution <= 4 ? 4 :
        resolution <= 8 ? 8 : 16;
    while (resolution > 1 && (cellsX * resolution + 1 > MaxPaintSamples ||
        cellsY * resolution + 1 > MaxPaintSamples)) resolution /= 2;
    if (resolution == paintResolution && (resolution == 0 || HasPaintMap())) return;
    if (resolution == 0)
    {
        SyncVertexWeightsFromPaint();
        paintResolution = 0;
        paintWeights.clear();
        m_paintTexture.reset();
        Rebuild();
        return;
    }
    const size_t layerCount = layers.size();
    const bool fromMap = HasPaintMap();
    const int oldWidth = fromMap ? PaintWidth() : cellsX + 1;
    const int oldHeight = fromMap ? PaintHeight() : cellsY + 1;
    const float oldScale = fromMap ? float(paintResolution) : 1.f;
    const std::vector<float> source = fromMap ? paintWeights : weights;
    paintResolution = resolution;
    const int width = PaintWidth(), height = PaintHeight();
    std::vector<float> next(size_t(width) * height * layerCount, 0.f);
    for (int row = 0; row < height; ++row)
        for (int col = 0; col < width; ++col)
        {
            float* sample = &next[(size_t(row) * width + col) * layerCount];
            SampleWeightGrid(source, oldWidth, oldHeight, layerCount,
                float(col) / resolution * oldScale, float(row) / resolution * oldScale, sample);
            NormalizeSample(sample, layerCount);
        }
    paintWeights = std::move(next);
    m_paintTextureDirty = true;
    Rebuild();
}

const Engine::Graphics::IGraphicsTexture* Terrain::GetPaintTexture(IGraphicsProvider* provider)
{
    if (!HasPaintMap() || !provider) return nullptr;
    if (m_paintTexture && !m_paintTextureDirty && m_paintTextureProvider == provider)
        return m_paintTexture.get();
    auto* factory = provider->GetTextureFactory();
    if (!factory) return m_paintTexture.get();
    const int width = PaintWidth(), height = PaintHeight();
    const size_t layerCount = layers.size();
    uint32_t mips = 1;
    while ((std::max(width, height) >> mips) >= 1) ++mips;
    // Slice 0: layers 0-3; slice 1: layers 4-5 (b, a unused).
    std::vector<uint8_t> bytes;
    for (int slice = 0; slice < 2; ++slice)
    {
        std::vector<float> level(size_t(width) * height * 4, 0.f);
        for (size_t i = 0; i < size_t(width) * height; ++i)
            for (int c = 0; c < 4; ++c)
            {
                const size_t layer = size_t(slice * 4 + c);
                if (layer < layerCount && layer < 6) level[i * 4 + c] = paintWeights[i * layerCount + layer];
            }
        int levelWidth = width, levelHeight = height;
        for (uint32_t mip = 0; mip < mips; ++mip)
        {
            for (float value : level)
                bytes.push_back(uint8_t(std::lround(std::clamp(value, 0.f, 1.f) * 255.f)));
            if (mip + 1 == mips) break;
            const int nextWidth = std::max(1, levelWidth / 2), nextHeight = std::max(1, levelHeight / 2);
            std::vector<float> next(size_t(nextWidth) * nextHeight * 4, 0.f);
            for (int y = 0; y < nextHeight; ++y)
                for (int x = 0; x < nextWidth; ++x)
                    for (int c = 0; c < 4; ++c)
                    {
                        float sum = 0.f;
                        for (int dy = 0; dy < 2; ++dy)
                            for (int dx = 0; dx < 2; ++dx)
                            {
                                const int sx = std::min(x * 2 + dx, levelWidth - 1);
                                const int sy = std::min(y * 2 + dy, levelHeight - 1);
                                sum += level[(size_t(sy) * levelWidth + sx) * 4 + c];
                            }
                        next[(size_t(y) * nextWidth + x) * 4 + c] = sum * .25f;
                    }
            level = std::move(next);
            levelWidth = nextWidth;
            levelHeight = nextHeight;
        }
    }
    auto texture = factory->CreateTexture2DArray(uint32_t(width), uint32_t(height), 2,
        bytes.data(), mips, Engine::Graphics::GraphicsTextureFormat::Rgba8, false);
    // A full upload queue returns null: keep the previous copy and retry.
    if (!texture) return m_paintTexture.get();
    m_paintTexture = std::move(texture);
    m_paintTextureProvider = provider;
    m_paintTextureDirty = false;
    return m_paintTexture.get();
}

void Terrain::Resize(int x, int y, float width, float depth)
{
    x = std::clamp(x, 1, 512); y = std::clamp(y, 1, 512);
    width = std::max(.001f, width); depth = std::max(.001f, depth);
    std::vector<float> next(static_cast<size_t>(x + 1) * (y + 1));
    std::vector<float> nextWeights(next.size() * layers.size(), 0.f);
    for (int row = 0; row <= std::min(y, cellsY); ++row)
        for (int col = 0; col <= std::min(x, cellsX); ++col)
            if (static_cast<size_t>(row * (cellsX + 1) + col) < heights.size())
            {
                const size_t oldIndex = static_cast<size_t>(row * (cellsX + 1) + col);
                const size_t newIndex = static_cast<size_t>(row * (x + 1) + col);
                next[newIndex] = heights[oldIndex];
                for (size_t layer = 0; layer < layers.size(); ++layer)
                    if (oldIndex * layers.size() + layer < weights.size())
                        nextWeights[newIndex * layers.size() + layer] =
                            weights[oldIndex * layers.size() + layer];
            }
    if (x != cellsX || y != cellsY) { SyncVertexWeightsFromPaint(); paintWeights.clear(); }
    cellsX = x; cellsY = y; cellWidth = width; cellDepth = depth;
    heights = std::move(next);
    weights = std::move(nextWeights);
    NormalizeWeights();
    Rebuild();
}

void Terrain::NormalizeWeights()
{
    if (layers.empty()) layers.push_back({});
    weights.resize(heights.size() * layers.size(), 0.f);
    for (size_t i = 0; i < heights.size(); ++i)
    {
        float sum = 0.f;
        for (size_t l = 0; l < layers.size(); ++l)
            sum += weights[i * layers.size() + l] =
                std::clamp(weights[i * layers.size() + l], 0.f, 1.f);
        if (sum < 0.00001f) weights[i * layers.size()] = 1.f;
        else for (size_t l = 0; l < layers.size(); ++l)
            weights[i * layers.size() + l] /= sum;
    }
}

void Terrain::AssignLayersByHeight()
{
    if (layers.empty()) return;
    weights.assign(heights.size() * layers.size(), 0.f);
    for (size_t i = 0; i < heights.size(); ++i)
    {
        size_t nearest = 0;
        float distance = INFINITY;
        for (size_t l = 0; l < layers.size(); ++l)
        {
            const float delta = std::max(layers[l].minHeight - heights[i],
                heights[i] - layers[l].maxHeight);
            if (delta < distance) { distance = delta; nearest = l; }
        }
        weights[i * layers.size() + nearest] = 1.f;
    }
    paintWeights.clear();
    Rebuild(); MarkConfigurationDirty();
}

float Terrain::BrushFalloff(float distance, float radius, float hardness, int falloff)
{
    if (radius <= 0.f || distance >= radius) return 0.f;
    if (hardness < 0.f) return 1.f - distance / radius;
    return TerrainBrushShape::Falloff(distance / radius, std::min(hardness, .999f), falloff);
}

Engine::Brushes::BrushLayerSet Terrain::ResolveRuleLayers(const Engine::Brushes::BrushRules& rules) const
{
    std::vector<std::string> materials;
    for (const Layer& layer : layers) materials.push_back(layer.material);
    return Engine::Brushes::ResolveBrushLayers(rules, materials);
}

float Terrain::RulesWeightAt(int col, int row, const std::vector<float>& heightGrid,
    const BrushOptions& options, const Engine::Brushes::BrushLayerSet& layerSet) const
{
    if (!options.rules) return 1.f;
    const auto at = [&](int c, int r) {
        c = std::clamp(c, 0, cellsX); r = std::clamp(r, 0, cellsY);
        return heightGrid[size_t(r) * (cellsX + 1) + c];
    };
    const float dx = (at(col + 1, row) - at(col - 1, row)) /
        (cellWidth * float(std::min(col + 1, cellsX) - std::max(col - 1, 0)));
    const float dz = (at(col, row + 1) - at(col, row - 1)) /
        (cellDepth * float(std::min(row + 1, cellsY) - std::max(row - 1, 0)));
    const float slope = std::atan(std::sqrt(dx * dx + dz * dz)) * 57.2957795f;
    const size_t i = size_t(row) * (cellsX + 1) + col;
    const size_t layerCount = layers.size();
    const bool painted = weights.size() == heightGrid.size() * layerCount;
    const float trail = options.trailDistance && options.rules->trailClearance > 0.f
        ? options.trailDistance(col * cellWidth, row * cellDepth, at(col, row))
        : std::numeric_limits<float>::infinity();
    return Engine::Brushes::BrushRulesWeight(*options.rules, at(col, row), slope, &layerSet,
        painted ? &weights[i * layerCount] : nullptr, painted ? layerCount : 0, trail);
}

float Terrain::BrushWeight(float dx, float dz, float radius, const BrushOptions& options,
    float* relief)
{
    if (relief) *relief = 1.f;
    if (!options.shape)
        return BrushFalloff(std::sqrt(dx * dx + dz * dz), radius, options.hardness, options.falloff);
    const TerrainBrushShape& shape = *options.shape;
    const glm::vec2 uv = TerrainBrushShape::ToShape(dx, dz, radius, options.shapeYaw);
    const float weight = shape.SampleMask(uv);
    if (relief) *relief = shape.maxHeight > 0.f
        ? std::clamp(shape.SampleHeight(uv) / shape.maxHeight, 0.f, 1.f) : 0.f;
    return std::max(weight, 0.f);
}

float Terrain::BrushReach(float radius, const BrushOptions& options)
{
    return options.shape ? radius * 1.4143f : radius;
}

bool Terrain::ApplyBrush(float x, float z, float radius, float strength,
    Brush brush, int layer, bool realignObjects, const BrushOptions& options)
{
    if (Owner && Owner->GetScene() && Owner->GetScene()->HasStarted()) return false;
    if (radius <= 0.f || strength <= 0.f || heights.empty()) return false;
    if (brush == Brush::Paint && (layer < 0 || layer >= static_cast<int>(layers.size())))
        return false;
    // The reach covers a shaped brush's turned square; outside its outline
    // the weight is 0.
    const float reach = BrushReach(radius, options);
    const std::vector<float> before = heights;
    const auto ruleLayers = options.rules ? ResolveRuleLayers(*options.rules) : Engine::Brushes::BrushLayerSet{};
    bool changed = false;
    EnsurePaintMap();
    if (brush == Brush::Paint && HasPaintMap())
    {
        // Paint at the map's resolution; the vertex weights follow in Rebuild.
        const size_t layerCount = layers.size();
        const int width = PaintWidth(), height = PaintHeight();
        const float stepX = cellWidth / paintResolution, stepZ = cellDepth / paintResolution;
        const int minCol = std::max(0, int(std::floor((x - reach) / stepX)));
        const int maxCol = std::min(width - 1, int(std::ceil((x + reach) / stepX)));
        const int minRow = std::max(0, int(std::floor((z - reach) / stepZ)));
        const int maxRow = std::min(height - 1, int(std::ceil((z + reach) / stepZ)));
        for (int row = minRow; row <= maxRow; ++row)
            for (int col = minCol; col <= maxCol; ++col)
            {
                const float dx = col * stepX - x, dz = row * stepZ - z;
                float amount = std::clamp(strength *
                    BrushWeight(dx, dz, radius, options), 0.f, 1.f);
                if (amount > 0.f && options.rules)
                    amount *= RulesWeightAt(std::clamp(int(std::lround(col * stepX / cellWidth)), 0, cellsX),
                        std::clamp(int(std::lround(row * stepZ / cellDepth)), 0, cellsY), before, options,
                        ruleLayers);
                float* sample = &paintWeights[(size_t(row) * width + col) * layerCount];
                if (PaintSample(sample, layerCount, layer, amount, options.erase))
                {
                    NormalizeSample(sample, layerCount);
                    changed = true;
                }
            }
        if (changed)
        {
            m_paintTextureDirty = true;
            Rebuild();
            MarkConfigurationDirty();
        }
        return changed;
    }
    const int firstCol = std::max(0, int(std::floor((x - reach) / cellWidth)));
    const int lastCol = std::min(cellsX, int(std::ceil((x + reach) / cellWidth)));
    const int firstRow = std::max(0, int(std::floor((z - reach) / cellDepth)));
    const int lastRow = std::min(cellsY, int(std::ceil((z + reach) / cellDepth)));
    // Backed stamp: the highest ground behind a point (back from it, against
    // the shape's facing, while still over the shape's square), from the
    // heights as they were before this dab.
    const bool backed = options.backed && options.shape && brush == Brush::Raise;
    const bool stopAtStamp = options.stopAtStamp && options.shape && brush == Brush::Raise;
    const float stampScale = options.shape
        ? radius / options.shape->halfExtent * options.stampHeightScale : 0.f;
    const glm::vec2 back = backed
        ? -glm::normalize(TerrainBrushShape::FromShape(options.shape->facing, 1.f, options.shapeYaw))
        : glm::vec2(0.f);
    const auto backingAt = [&](float px, float pz)
    {
        float highest = -std::numeric_limits<float>::infinity();
        const float step = .5f * std::min(cellWidth, cellDepth);
        for (float travelled = step;; travelled += step)
        {
            const float sx = px + back.x * travelled, sz = pz + back.y * travelled;
            const glm::vec2 uv = TerrainBrushShape::ToShape(sx - x, sz - z, radius, options.shapeYaw);
            if (std::abs(uv.x) > 1.f || std::abs(uv.y) > 1.f) break;
            float ground = 0.f;
            const bool known = options.backingHeight ? options.backingHeight(sx, sz, ground)
                : SampleTerrainSurface(before, cellsX, cellsY, cellWidth, cellDepth, sx, sz, ground);
            if (known) highest = std::max(highest, ground);
        }
        return highest;
    };
    for (int row = firstRow; row <= lastRow; ++row)
        for (int col = firstCol; col <= lastCol; ++col)
        {
            const float dx = col * cellWidth - x, dz = row * cellDepth - z;
            float relief = 1.f;
            const float weight = BrushWeight(dx, dz, radius, options, &relief);
            // Shaped Raise and Lower follow the shape's height map as well as
            // its outline: its high places move fastest.
            const bool followRelief = brush == Brush::Raise || brush == Brush::Lower;
            float amount = std::clamp(strength * weight * (followRelief ? relief : 1.f), 0.f, 1.f);
            if (amount > 0.f && options.rules) amount *= RulesWeightAt(col, row, before, options, ruleLayers);
            if (amount <= 0.f) continue;
            const size_t i = static_cast<size_t>(row * (cellsX + 1) + col);
            if (brush == Brush::Paint)
            {
                if (PaintSample(&weights[i * layers.size()], layers.size(),
                        layer, amount, options.erase))
                {
                    NormalizeSample(&weights[i * layers.size()], layers.size());
                    changed = true;
                }
            }
            else
            {
                float target = before[i];
                if (brush == Brush::Raise) target += amount;
                // Raise never lowers: a cap below the point leaves it as is.
                // At the stamp's surface the point is done.
                if (stopAtStamp)
                {
                    const glm::vec2 uv = TerrainBrushShape::ToShape(dx, dz, radius, options.shapeYaw);
                    const float surface = options.stampBase + options.shape->SampleHeight(uv) * stampScale;
                    target = std::min(target, std::max(before[i], surface));
                }
                // Nothing behind it higher than the point: the stamp has
                // nothing to build against here.
                if (backed)
                    target = std::min(target, std::max(before[i],
                        backingAt(col * cellWidth, row * cellDepth)));
                if (brush == Brush::Lower) target -= amount;
                if (brush == Brush::Flatten)
                    target = before[i] + (options.flattenHeight - before[i]) * amount;
                if (brush == Brush::Blend)
                {
                    // Smooth toward the 3x3 neighborhood mean.
                    float sum = 0.f; int count = 0;
                    for (int oy = -1; oy <= 1; ++oy)
                        for (int ox = -1; ox <= 1; ++ox)
                        {
                            const int nx = col + ox, ny = row + oy;
                            if (nx >= 0 && nx <= cellsX && ny >= 0 && ny <= cellsY)
                            { sum += before[ny * (cellsX + 1) + nx]; ++count; }
                        }
                    target = before[i] + (sum / count - before[i]) * amount;
                }
                changed |= target != heights[i]; heights[i] = target;
            }
        }
    if (changed)
    {
        // Paint keeps each touched sample normalized itself.
        Rebuild();
        MarkConfigurationDirty();
        if (brush != Brush::Paint)
            FollowSurfaceEdit(before, x - reach, z - reach, x + reach, z + reach,
                realignObjects);
    }
    return changed;
}

void Terrain::FollowSurfaceEdit(const std::vector<float>& before, float minX, float minZ,
    float maxX, float maxZ, bool realignObjects)
{
    if (!Owner || before.size() != heights.size()) return;
    for (auto* child : Owner->Children)
        if (auto* objects = child ? child->GetComponent<TerrainObjectLayer>() : nullptr)
        {
            // Sculpting moves placements vertically; orientation
            // changes only when the brush's realign option is on.
            // Generated meadow grass always hugs the ground. Any other
            // placement follows the surface only if it was resting on
            // the old surface, keeping its own offset; objects lifted
            // or sunk on purpose stay where they are.
            const bool alwaysSnap = objects->snapToGround;
            const glm::mat4 layerToCell = child->transform.GetLocalMatrix();
            const glm::mat4 cellToLayer = glm::inverse(layerToCell);
            constexpr float restingTolerance = .05f;
            bool placementsChanged = false;
            for (auto& placement : objects->placements)
            {
                glm::vec3 cell = glm::vec3(layerToCell * glm::vec4(placement.position, 1.f));
                if (cell.x < minX || cell.x > maxX || cell.z < minZ || cell.z > maxZ) continue;
                float oldHeight = cell.y, newHeight = cell.y;
                glm::vec3 oldNormal(0.f, 1.f, 0.f), newNormal(0.f, 1.f, 0.f);
                if (!SampleTerrainSurface(before, cellsX, cellsY, cellWidth,
                        cellDepth, cell.x, cell.z, oldHeight, &oldNormal) ||
                    !SampleTerrainSurface(heights, cellsX, cellsY, cellWidth,
                        cellDepth, cell.x, cell.z, newHeight, &newNormal))
                    continue;
                if (!alwaysSnap && std::abs(cell.y - oldHeight) > restingTolerance)
                    continue;
                cell.y = alwaysSnap ? newHeight : newHeight + (cell.y - oldHeight);
                const glm::vec3 position = glm::vec3(cellToLayer * glm::vec4(cell, 1.f));
                // Ground-conforming layers stay upright and use the
                // normal only to shear their base onto the slope, so
                // it always tracks the surface. With realignment on,
                // placements painted aligned to the old surface tilt
                // with it; upright ones stay upright either way.
                const glm::vec3 layerOldNormal = glm::normalize(
                    glm::vec3(cellToLayer * glm::vec4(oldNormal, 0.f)));
                const bool realign = objects->conformToGround ||
                    (realignObjects && (alwaysSnap ||
                        (placement.normal != glm::vec3(0.f, 1.f, 0.f) &&
                            glm::dot(placement.normal, layerOldNormal) > .999f)));
                const glm::vec3 normal = realign
                    ? glm::normalize(glm::vec3(cellToLayer * glm::vec4(newNormal, 0.f)))
                    : placement.normal;
                if (position != placement.position || normal != placement.normal)
                {
                    placement.position = position;
                    placement.normal = normal;
                    placementsChanged = true;
                }
            }
            if (placementsChanged) objects->MarkConfigurationDirty();
        }
}

bool Terrain::RaycastLocal(const glm::vec3& origin, const glm::vec3& direction,
    float& distance) const
{
    if (heights.size() != static_cast<size_t>(cellsX + 1) * (cellsY + 1) ||
        cellWidth <= 0.f || cellDepth <= 0.f) return false;
    const float length = glm::length(direction);
    if (!(length > 1.e-8f)) return false;
    const auto [lowest, highest] = std::minmax_element(heights.begin(), heights.end());
    const glm::vec3 minimum(0.f, *lowest - .01f, 0.f);
    const glm::vec3 maximum(cellsX * cellWidth, *highest + .01f, cellsY * cellDepth);
    float enter = 0.f, exit = std::numeric_limits<float>::max();
    for (int axis = 0; axis < 3; ++axis)
    {
        if (std::abs(direction[axis]) < 1.e-12f)
        {
            if (origin[axis] < minimum[axis] || origin[axis] > maximum[axis]) return false;
            continue;
        }
        float a = (minimum[axis] - origin[axis]) / direction[axis];
        float b = (maximum[axis] - origin[axis]) / direction[axis];
        if (a > b) std::swap(a, b);
        enter = std::max(enter, a);
        exit = std::min(exit, b);
        if (enter > exit) return false;
    }
    const auto clearance = [&](float t) {
        const glm::vec3 point = origin + direction * t;
        float height = point.y;
        SampleTerrainSurface(heights, cellsX, cellsY, cellWidth, cellDepth,
            point.x, point.z, height);
        return point.y - height;
    };
    float previousT = enter;
    if (clearance(enter) <= 0.f)
    {
        // Entering through a side wall below the surface is a hit; starting
        // underground (camera inside the bounds) is not.
        if (enter <= 0.f) return false;
        distance = enter;
        return true;
    }
    // Quarter-cell steps cannot skip a triangle; bisection then refines.
    const float step = .25f * std::min(cellWidth, cellDepth) / length;
    for (float t = enter + step;; t += step)
    {
        t = std::min(t, exit);
        if (clearance(t) <= 0.f)
        {
            float above = previousT, below = t;
            for (int iteration = 0; iteration < 24; ++iteration)
            {
                const float middle = .5f * (above + below);
                (clearance(middle) > 0.f ? above : below) = middle;
            }
            distance = below;
            return true;
        }
        if (t >= exit) return false;
        previousT = t;
    }
}

bool Terrain::ApplyBrushFromLocalRay(const glm::vec3& nearPoint,
    const glm::vec3& farPoint, float radius, float strength,
    Brush brush, int layer)
{
    float t = 0.f;
    if (!RaycastLocal(nearPoint, farPoint - nearPoint, t) || t > 1.f) return false;
    const glm::vec3 hit = nearPoint + (farPoint - nearPoint) * t;
    if (hit.x < 0.f || hit.z < 0.f ||
        hit.x > cellsX * cellWidth || hit.z > cellsY * cellDepth)
        return false;
    return ApplyBrush(hit.x, hit.z, radius, strength, brush, layer);
}

void Terrain::Rebuild()
{
    // The terrain draws and collides as a volume built from these heights
    // (VolumeTerrain; todo.md "Terrain: volume-first conversion", step 7):
    // there is no height-grid mesh. A rebuild settles the weights, the paint
    // and the materials and tells the volume the surface changed.
    if (heights.size() != static_cast<size_t>(cellsX + 1) * (cellsY + 1)) return;
    NormalizeWeights();
    EnsurePaintMap();
    SyncVertexWeightsFromPaint();
    RefreshMaterials();
    ++m_surfaceRevision;
}

void Terrain::EnsureVolume()
{
    // A terrain of its own (not a Terrain Manager's cell: the manager gives
    // its cells volumes joined into one sheet) draws and collides as a volume.
    if (!Owner || Owner->GetComponent<VolumeTerrain>()) return;
    for (auto* parent = Owner->Parent; parent; parent = parent->Parent)
        if (parent->GetComponent<TerrainManager>()) return;
    auto* volume = Owner->AddComponent<VolumeTerrain>();
    volume->fromTerrain = true;
    volume->sampleSpacing = std::clamp(std::min(cellWidth, cellDepth), .25f, 4.f);
    volume->drawDistance = std::numeric_limits<float>::max();
    volume->collisionDistance = std::numeric_limits<float>::max();
    volume->Rebuild();
}

Terrain::Prepared Terrain::TakePrepared()
{
    Prepared result;
    result.cellsX = cellsX; result.cellsY = cellsY;
    result.cellWidth = cellWidth; result.cellDepth = cellDepth;
    result.textureTiling = textureTiling;
    result.heights = std::move(heights);
    result.weights = std::move(weights);
    result.lushness = std::move(lushness);
    result.paintResolution = paintResolution;
    result.paintWeights = std::move(paintWeights);
    result.layers = std::move(layers);
    result.materials = std::move(m_materials);
    result.materialPaths = std::move(m_materialPaths);
    return result;
}

bool Terrain::InstallPrepared(Prepared prepared, IGraphicsProvider* provider)
{
    if (!Owner || prepared.heights.size() !=
        static_cast<size_t>(prepared.cellsX + 1) * (prepared.cellsY + 1)) return false;
    cellsX = prepared.cellsX; cellsY = prepared.cellsY;
    cellWidth = prepared.cellWidth; cellDepth = prepared.cellDepth;
    textureTiling = prepared.textureTiling;
    heights = std::move(prepared.heights);
    weights = std::move(prepared.weights);
    lushness = std::move(prepared.lushness);
    paintResolution = prepared.paintResolution;
    paintWeights = std::move(prepared.paintWeights);
    m_paintTextureDirty = true;
    layers = std::move(prepared.layers);
    m_materials = std::move(prepared.materials);
    m_materialPaths = std::move(prepared.materialPaths);
    RefreshMaterials();
    ++m_surfaceRevision;
    m_provider = nullptr;
    if (provider && !DeferGraphicsPreparation(provider))
        PrepareGraphicsResources(provider);
    return true;
}

void Terrain::FinalizePreparedGraphics(IGraphicsProvider* provider)
{
    if (provider && !DeferGraphicsPreparation(provider))
        PrepareGraphicsResources(provider);
}

bool Terrain::SaveCache(const std::string& path, uint64_t signature) const
{
    if (heights.empty() || heights.size() != static_cast<size_t>(cellsX + 1) * (cellsY + 1) ||
        weights.size() != heights.size() * layers.size() || layers.empty() || layers.size() > 6)
        return false;
    std::error_code error;
    const std::filesystem::path target(path);
    std::filesystem::create_directories(target.parent_path(), error);
    if (error) return false;
    const auto temporary = target.string() + ".tmp";
    std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
    if (!stream) return false;
    const uint64_t magic = 0x31484341434e5254ull; // TRNCACH1
    // 8 (2026-11-24): heights, weights and lushness only; the volume meshes
    // the cell (6 and 7 also held the height-grid mesh's appearance and LODs).
    const uint32_t version = CacheVersion;
    const uint32_t layerCount = static_cast<uint32_t>(layers.size());
    const uint8_t texturedMask = 0;
    bool valid = WriteValue(stream, magic) && WriteValue(stream, version) &&
        WriteValue(stream, signature) && WriteValue(stream, cellsX) &&
        WriteValue(stream, cellsY) && WriteValue(stream, cellWidth) &&
        WriteValue(stream, cellDepth) && WriteValue(stream, textureTiling) &&
        WriteValue(stream, texturedMask) && WriteValue(stream, layerCount);
    for (const Layer& layer : layers)
    {
        const uint32_t length = static_cast<uint32_t>(layer.material.size());
        valid &= WriteValue(stream, length) && length <= 4096;
        if (!valid) break;
        stream.write(layer.material.data(), length);
        const int64_t materialStamp = MaterialStamp(layer.material);
        valid &= static_cast<bool>(stream) && WriteValue(stream, layer.minHeight) &&
            WriteValue(stream, layer.maxHeight) && WriteValue(stream, layer.previewColor.x) &&
            WriteValue(stream, layer.previewColor.y) && WriteValue(stream, layer.previewColor.z) &&
            WriteValue(stream, materialStamp);
    }
    std::vector<float> lush(heights.size(), 0.f);
    for (size_t i = 0; i < lush.size() && i < lushness.size(); ++i) lush[i] = lushness[i];
    valid &= WriteVector(stream, heights) && WriteVector(stream, weights) && WriteVector(stream, lush);
    stream.close();
    valid &= static_cast<bool>(stream);
    bool committed = false;
    if (valid)
    {
        std::filesystem::rename(temporary, target, error);
        if (error)
        {
            error.clear();
            std::filesystem::remove(target, error);
            error.clear();
            std::filesystem::rename(temporary, target, error);
        }
        committed = !error;
    }
    if (!committed) std::filesystem::remove(temporary, error);
    return committed;
}

bool Terrain::ReadCacheHeights(const std::string& path, uint64_t signature,
    int expectedX, int expectedY, std::vector<float>& result)
{
    return ReadCacheHeightsRange(path, signature, expectedX, expectedY,
        result, 0, 0);
}

bool Terrain::ReadCacheHeightsRange(const std::string& path,
    uint64_t signature, int expectedX, int expectedY,
    std::vector<float>& result, uint64_t offset, uint64_t size,
    std::vector<float>* resultWeights, uint32_t* weightLayerCount)
{
    std::ifstream stream(path, std::ios::binary);
    if (offset) stream.seekg(static_cast<std::streamoff>(offset));
    uint64_t magic = 0, storedSignature = 0;
    uint32_t version = 0, layerCount = 0;
    uint8_t texturedMask = 0;
    int x = 0, y = 0;
    float width = 0.f, depth = 0.f, tiling = 0.f;
    if (!stream || !ReadValue(stream, magic) ||
        magic != 0x31484341434e5254ull ||
        !ReadValue(stream, version) || version != CacheVersion ||
        !ReadValue(stream, storedSignature) || storedSignature != signature ||
        !ReadValue(stream, x) || x != expectedX ||
        !ReadValue(stream, y) || y != expectedY ||
        !ReadValue(stream, width) || width <= 0.f ||
        !ReadValue(stream, depth) || depth <= 0.f ||
        !ReadValue(stream, tiling) || tiling < 1.f ||
        !ReadValue(stream, texturedMask) ||
        !ReadValue(stream, layerCount) || layerCount < 1 || layerCount > 6)
        return false;
    for (uint32_t layer = 0; layer < layerCount; ++layer)
    {
        uint32_t length = 0;
        if (!ReadValue(stream, length) || length > 4096) return false;
        std::string material(length, '\0');
        stream.read(material.data(), length);
        float ignored[5]{};
        int64_t storedStamp = 0;
        if (!stream || !stream.read(reinterpret_cast<char*>(ignored),
                sizeof(ignored)) || !ReadValue(stream, storedStamp) ||
            storedStamp != MaterialStamp(material)) return false;
    }
    const uint64_t count = static_cast<uint64_t>(x + 1) * (y + 1);
    std::vector<float> heights;
    if (!ReadVector(stream, heights, count)) return false;
    std::vector<float> weights;
    if (resultWeights && !ReadVector(stream, weights, count * layerCount))
        return false;
    if (size && static_cast<uint64_t>(stream.tellg()) > offset + size)
        return false;
    result = std::move(heights);
    if (resultWeights) *resultWeights = std::move(weights);
    if (weightLayerCount) *weightLayerCount = layerCount;
    return true;
}

bool Terrain::CacheMaterialsMatchRange(std::istream& stream,
    uint64_t signature, int expectedX, int expectedY,
    uint64_t offset, uint64_t size,
    std::map<std::string, int64_t>& materialStamps)
{
    stream.clear();
    stream.seekg(static_cast<std::streamoff>(offset));
    uint64_t magic = 0, storedSignature = 0;
    uint32_t version = 0, layerCount = 0;
    uint8_t texturedMask = 0;
    int x = 0, y = 0;
    float width = 0.f, depth = 0.f, tiling = 0.f;
    if (!stream || !ReadValue(stream, magic) ||
        magic != 0x31484341434e5254ull ||
        !ReadValue(stream, version) || version != CacheVersion ||
        !ReadValue(stream, storedSignature) || storedSignature != signature ||
        !ReadValue(stream, x) || x != expectedX ||
        !ReadValue(stream, y) || y != expectedY ||
        !ReadValue(stream, width) || width <= 0.f ||
        !ReadValue(stream, depth) || depth <= 0.f ||
        !ReadValue(stream, tiling) || tiling < 1.f ||
        !ReadValue(stream, texturedMask) ||
        !ReadValue(stream, layerCount) || layerCount < 1 || layerCount > 6)
        return false;
    for (uint32_t layer = 0; layer < layerCount; ++layer)
    {
        uint32_t length = 0;
        if (!ReadValue(stream, length) || length > 4096) return false;
        std::string material(length, '\0');
        stream.read(material.data(), length);
        float ignored[5]{};
        int64_t storedStamp = 0;
        if (!stream || !stream.read(reinterpret_cast<char*>(ignored),
                sizeof(ignored)) || !ReadValue(stream, storedStamp))
            return false;
        const auto found = materialStamps.find(material);
        const int64_t currentStamp = found == materialStamps.end()
            ? materialStamps.emplace(material, MaterialStamp(material)).first->second
            : found->second;
        if (storedStamp != currentStamp) return false;
    }
    return static_cast<uint64_t>(stream.tellg()) <= offset + size;
}

bool Terrain::LoadCache(const std::string& path, uint64_t signature,
    IGraphicsProvider* provider)
{
    return LoadCacheRange(path, signature, provider, 0, 0);
}

bool Terrain::LoadCacheRange(const std::string& path, uint64_t signature,
    IGraphicsProvider* provider, uint64_t offset, uint64_t size)
{
    if (!Owner) return false;
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return false;
    if (offset) stream.seekg(static_cast<std::streamoff>(offset));
    if (!stream) return false;
    uint64_t magic = 0, storedSignature = 0;
    uint32_t version = 0, layerCount = 0;
    uint8_t texturedMask = 0;
    int x = 0, y = 0;
    float width = 0.f, depth = 0.f, tiling = 0.f;
    if (!ReadValue(stream, magic) || magic != 0x31484341434e5254ull ||
        !ReadValue(stream, version) || version != CacheVersion ||
        !ReadValue(stream, storedSignature) || storedSignature != signature ||
        !ReadValue(stream, x) || !ReadValue(stream, y) ||
        !ReadValue(stream, width) || !ReadValue(stream, depth) ||
        !ReadValue(stream, tiling) || !ReadValue(stream, texturedMask) ||
        !ReadValue(stream, layerCount) ||
        x < 1 || x > 512 || y < 1 || y > 512 || width <= 0.f || depth <= 0.f ||
        tiling < 1.f || layerCount < 1 || layerCount > 6)
        return false;
    std::vector<Layer> cachedLayers(layerCount);
    for (Layer& layer : cachedLayers)
    {
        uint32_t length = 0;
        int64_t materialStamp = 0;
        if (!ReadValue(stream, length) || length > 4096) return false;
        layer.material.resize(length);
        stream.read(layer.material.data(), length);
        if (!stream || !ReadValue(stream, layer.minHeight) ||
            !ReadValue(stream, layer.maxHeight) ||
            !ReadValue(stream, layer.previewColor.x) ||
            !ReadValue(stream, layer.previewColor.y) ||
            !ReadValue(stream, layer.previewColor.z) ||
            !ReadValue(stream, materialStamp) ||
            materialStamp != MaterialStamp(layer.material)) return false;
    }
    const uint64_t vertexCount = static_cast<uint64_t>(x + 1) * (y + 1);
    std::vector<float> cachedHeights, cachedWeights, cachedLushness;
    if (!ReadVector(stream, cachedHeights, vertexCount) ||
        !ReadVector(stream, cachedWeights, vertexCount * layerCount) ||
        !ReadVector(stream, cachedLushness, vertexCount))
        return false;
    if (size)
    {
        if (static_cast<uint64_t>(stream.tellg()) != offset + size)
            return false;
    }
    else
    {
        char trailing = 0;
        if (stream.read(&trailing, 1) || !stream.eof()) return false;
    }
    cellsX = x; cellsY = y; cellWidth = width; cellDepth = depth;
    textureTiling = tiling;
    heights = std::move(cachedHeights);
    weights = std::move(cachedWeights);
    lushness = std::move(cachedLushness);
    layers = std::move(cachedLayers);
    m_provider = provider;
    m_materialPaths.clear(); m_materials.clear();
    Rebuild();
    return true;
}

void Terrain::RefreshMaterials()
{
    if (m_materialPaths.size() != layers.size())
    {
        m_materialPaths.resize(layers.size());
        m_materials.resize(layers.size());
    }
    for (size_t i = 0; i < layers.size(); ++i)
    {
        if (m_materialPaths[i] == layers[i].material) continue;
        m_materialPaths[i] = layers[i].material;
        m_materials[i].reset();
        if (layers[i].material.empty()) continue;
        auto material = std::make_shared<Material>();
        // Layer textures are not uploaded individually: the renderer packs
        // them into TerrainLayerArrays.
        if (material->LoadFromFile(Mesh::ResolveFilePath(layers[i].material)))
            m_materials[i] = std::move(material);
    }
}

Terrain::JsonValue Terrain::Serialize() const
{
    JsonValue out = Component::Serialize();
    out.Set("cellsX", JsonValue(cellsX)).Set("cellsY", JsonValue(cellsY))
        .Set("cellWidth", JsonValue(cellWidth)).Set("cellDepth", JsonValue(cellDepth))
        .Set("textureTiling", JsonValue(textureTiling))
        .Set("bakedAssetPath", JsonValue(bakedAssetPath))
        .Set("reliefMapping", JsonValue(reliefMapping))
        .Set("reliefShadows", JsonValue(reliefShadows));
    JsonValue h = JsonValue::MakeArray(), w = JsonValue::MakeArray(), ls = JsonValue::MakeArray();
    for (float value : heights) h.Push(JsonValue(value));
    for (float value : weights) w.Push(JsonValue(value));
    for (const Layer& layer : layers)
        ls.Push(JsonValue::MakeObject().Set("material", JsonValue(layer.material))
            .Set("minHeight", JsonValue(layer.minHeight))
            .Set("maxHeight", JsonValue(layer.maxHeight))
            .Set("color", JsonValue::MakeArray().Push(JsonValue(layer.previewColor.x))
                .Push(JsonValue(layer.previewColor.y)).Push(JsonValue(layer.previewColor.z))));
    out.Set("heights", std::move(h)).Set("weights", std::move(w))
        .Set("layers", std::move(ls));
    if (HasPaintMap())
        out.Set("paintResolution", JsonValue(paintResolution))
            .Set("paintMap", JsonValue(EncodePaintMap(paintWeights)));
    return out;
}

void Terrain::Deserialize(const JsonValue& value)
{
    Component::Deserialize(value);
    cellsX = std::clamp(value["cellsX"].AsInt(), 1, 512);
    cellsY = std::clamp(value["cellsY"].AsInt(), 1, 512);
    cellWidth = std::max(.001f, value["cellWidth"].AsFloat());
    cellDepth = std::max(.001f, value["cellDepth"].AsFloat());
    textureTiling = value.Has("textureTiling")
        ? std::max(1.f, value["textureTiling"].AsFloat()) : 4.f;
    bakedAssetPath = value.Has("bakedAssetPath")
        ? value["bakedAssetPath"].AsString() : std::string{};
    reliefMapping = !value.Has("reliefMapping") || value["reliefMapping"].AsBool();
    reliefShadows = value.Has("reliefShadows") && value["reliefShadows"].AsBool();
    heights.assign(static_cast<size_t>(cellsX + 1) * (cellsY + 1), 0.f);
    for (size_t i = 0; i < std::min(heights.size(), value["heights"].ArraySize()); ++i)
        heights[i] = value["heights"].ArrayAt(i).AsFloat();
    // Vector displacement: two floats (x, z) per vertex, or none.
    layers.clear();
    const auto& entries = value["layers"];
    for (size_t i = 0; i < entries.ArraySize(); ++i)
    {
        const auto& entry = entries.ArrayAt(i);
        Layer layer;
        layer.material = entry["material"].AsString();
        layer.minHeight = entry["minHeight"].AsFloat();
        layer.maxHeight = entry["maxHeight"].AsFloat();
        const auto& color = entry["color"];
        if (color.ArraySize() == 3)
            layer.previewColor = {color.ArrayAt(0).AsFloat(), color.ArrayAt(1).AsFloat(), color.ArrayAt(2).AsFloat()};
        layers.push_back(layer);
    }
    if (layers.empty()) layers.push_back({});
    weights.assign(heights.size() * layers.size(), 0.f);
    for (size_t i = 0; i < std::min(weights.size(), value["weights"].ArraySize()); ++i)
        weights[i] = value["weights"].ArrayAt(i).AsFloat();
    NormalizeWeights();
    paintResolution = value.Has("paintResolution")
        ? std::clamp(value["paintResolution"].AsInt(), 0, 16) : 0;
    paintWeights.clear();
    m_paintTextureDirty = true;
    // A map that does not decode to this terrain's size is reseeded from the
    // vertex weights by Rebuild.
    if (paintResolution > 0 && value.Has("paintMap"))
    {
        const size_t expected = size_t(PaintWidth()) * PaintHeight() * layers.size();
        if (DecodePaintMap(value["paintMap"].AsString(), expected, paintWeights))
            for (size_t i = 0; i < paintWeights.size(); i += layers.size())
                NormalizeSample(&paintWeights[i], layers.size());
        else
            paintWeights.clear();
    }
    Rebuild();
}

void Terrain::OnAfterDeserialize(IGraphicsProvider* provider)
{
    m_provider = nullptr;
    Rebuild();
    EnsureVolume();
    if (provider && !DeferGraphicsPreparation(provider))
        PrepareGraphicsResources(provider);
}

void Terrain::PrepareGraphicsResources(IGraphicsProvider* provider)
{
    m_provider = provider;
    EnsureVolume();
}

void Terrain::Start()
{
    if (!bakedAssetPath.empty())
        LoadCache(Mesh::ResolveFilePath(bakedAssetPath), 0, m_provider);
}

bool Terrain::DrawProperties(Engine::Core::IInspectorUi& ui)
{
    bool changed = false;
    bool geometryChanged = false;
    bool appearanceChanged = false;
    int x = cellsX, y = cellsY;
    float width = cellWidth, depth = cellDepth;
    geometryChanged |= ui.SliderInt("Cells X", &x, 1, 512);
    geometryChanged |= ui.SliderInt("Cells Y", &y, 1, 512);
    geometryChanged |= ui.DragFloat("Cell width", &width, .1f, .001f, 1000.f);
    geometryChanged |= ui.DragFloat("Cell depth", &depth, .1f, .001f, 1000.f);
    geometryChanged |= ui.DragFloat("Texture tiling", &textureTiling, .25f, 1.f, 128.f);
    changed |= ui.Checkbox("Near-camera relief (parallax)", &reliefMapping);
    changed |= ui.Checkbox("Relief shadows (costly)", &reliefShadows);
    // Paint samples per cell edge: off (vertices), 2, 4, 8 or 16.
    static constexpr int paintSteps[] = {0, 2, 4, 8, 16};
    int paintStep = 0;
    while (paintStep < 4 && paintSteps[paintStep] < paintResolution) ++paintStep;
    const int shownStep = paintStep;
    if (ui.SliderInt(paintResolution ? ("Paint detail (" + std::to_string(paintResolution) +
            " per cell)##PaintDetail").c_str() : "Paint detail (vertices)##PaintDetail",
            &paintStep, 0, 4) &&
        paintStep != shownStep)
    {
        SetPaintResolution(paintSteps[paintStep]);
        MarkConfigurationDirty();
        changed = true;
    }
    char bakePath[512]{};
    std::memcpy(bakePath, bakedAssetPath.data(),
        std::min(bakedAssetPath.size(), sizeof(bakePath) - 1));
    if (ui.InputText("Runtime baked mesh", bakePath, sizeof(bakePath)))
    { bakedAssetPath = bakePath; changed = true; }
    if (geometryChanged)
    {
        Resize(x, y, width, depth);
        MarkConfigurationDirty();
        changed = true;
    }
    if (layers.size() < 6 && ui.Button("Add material layer"))
    {
        const size_t oldCount = layers.size();
        layers.push_back({});
        std::vector<float> next(heights.size() * layers.size(), 0.f);
        for (size_t i = 0; i < heights.size(); ++i)
            for (size_t l = 0; l < oldCount; ++l)
                next[i * layers.size() + l] = weights[i * oldCount + l];
        weights = std::move(next); appearanceChanged = true;
        if (paintResolution > 0 && oldCount > 0 &&
            paintWeights.size() == size_t(PaintWidth()) * PaintHeight() * oldCount)
        {
            std::vector<float> nextPaint(paintWeights.size() / oldCount * layers.size(), 0.f);
            for (size_t i = 0; i < paintWeights.size() / oldCount; ++i)
                for (size_t l = 0; l < oldCount; ++l)
                    nextPaint[i * layers.size() + l] = paintWeights[i * oldCount + l];
            paintWeights = std::move(nextPaint);
            m_paintTextureDirty = true;
        }
    }
    for (size_t l = 0; l < layers.size(); ++l)
    {
        ui.PushId(&layers[l]);
        ui.Label(("Layer " + std::to_string(l + 1)).c_str());
        char path[512]{};
        std::memcpy(path, layers[l].material.data(),
            std::min(layers[l].material.size(), sizeof(path) - 1));
        if (ui.InputText("Material asset", path, sizeof(path)))
        { layers[l].material = path; appearanceChanged = true; }
        appearanceChanged |= ui.DragFloat("Minimum height", &layers[l].minHeight, .1f);
        appearanceChanged |= ui.DragFloat("Maximum height", &layers[l].maxHeight, .1f);
        appearanceChanged |= ui.ColorEdit3("Preview color", &layers[l].previewColor.x);
        ui.PopId();
    }
    if (ui.Button("Assign layers by height"))
    { AssignLayersByHeight(); appearanceChanged = true; }
    if (appearanceChanged) { Rebuild(); MarkConfigurationDirty(); changed = true; }
    if (ui.Button("Bake terrain for runtime"))
    {
        if (bakedAssetPath.empty())
        {
            std::string name = Owner ? Owner->name : "Terrain";
            for (char& ch : name)
                if (!std::isalnum(static_cast<unsigned char>(ch)) && ch != '-' &&
                    ch != '_') ch = '_';
            bakedAssetPath = "Assets/TerrainBakes/" + name + ".terrainmesh";
            MarkConfigurationDirty();
            changed = true;
        }
        m_bakeStatus = SaveCache(Mesh::ResolveFilePath(bakedAssetPath), 0)
            ? "Runtime terrain bake saved" : "Runtime terrain bake failed";
    }
    if (!m_bakeStatus.empty()) ui.Label(m_bakeStatus.c_str());
    return changed;
}
}


namespace Engine::Components
{
bool Terrain::WriteHeightGrid(const TerrainHeightGrid& grid, double offsetX, double offsetZ,
    bool realignObjects)
{
    if (heights.size() != static_cast<size_t>(cellsX + 1) * (cellsY + 1) ||
        grid.width <= 0 || grid.height <= 0) return false;
    const std::vector<float> before = heights;
    int minCol = cellsX + 1, minRow = cellsY + 1, maxCol = -1, maxRow = -1;
    for (int row = 0; row <= cellsY; ++row)
    {
        const double gz = (offsetZ + double(row) * cellDepth - grid.originZ) / grid.spacingZ;
        const int gridRow = int(std::lround(gz));
        if (gridRow < 0 || gridRow >= grid.height || std::abs(gz - gridRow) > 1e-3) continue;
        for (int col = 0; col <= cellsX; ++col)
        {
            const double gx = (offsetX + double(col) * cellWidth - grid.originX) / grid.spacingX;
            const int gridCol = int(std::lround(gx));
            if (gridCol < 0 || gridCol >= grid.width || std::abs(gx - gridCol) > 1e-3) continue;
            const size_t source = size_t(gridRow) * grid.width + gridCol;
            if (!grid.valid.empty() && !grid.valid[source]) continue;
            const size_t index = size_t(row) * (cellsX + 1) + col;
            bool changed = false;
            float& target = heights[index];
            if (target != grid.heights[source])
            {
                target = grid.heights[source];
                changed = true;
            }
            if (!changed) continue;
            minCol = std::min(minCol, col); maxCol = std::max(maxCol, col);
            minRow = std::min(minRow, row); maxRow = std::max(maxRow, row);
        }
    }
    if (maxCol < 0) return false;
    Rebuild();
    MarkConfigurationDirty();
    FollowSurfaceEdit(before, minCol * cellWidth - cellWidth, minRow * cellDepth - cellDepth,
        maxCol * cellWidth + cellWidth, maxRow * cellDepth + cellDepth, realignObjects);
    return true;
}

bool Terrain::EditRegion(const TerrainRegionMask& mask, const TerrainRegionOperation& operation,
    bool realignObjects)
{
    if (Owner && Owner->GetScene() && Owner->GetScene()->HasStarted()) return false;
    if (mask.Empty() || heights.size() != static_cast<size_t>(cellsX + 1) * (cellsY + 1))
        return false;
    // The whole cell is the window: operations see all of it as context.
    TerrainHeightGrid grid;
    grid.spacingX = cellWidth;
    grid.spacingZ = cellDepth;
    grid.width = cellsX + 1;
    grid.height = cellsY + 1;
    grid.heights = heights;
    grid.valid.assign(heights.size(), 1);
    if (!ApplyRegionOperation(grid, mask, operation)) return false;
    return WriteHeightGrid(grid, 0.0, 0.0, realignObjects);
}

bool Terrain::PaintRegion(const TerrainRegionMask& mask, float strength, int layer, bool erase,
    double offsetX, double offsetZ, const Engine::Brushes::BrushRules* rules,
    std::function<float(float x, float z, float height)> trailDistance)
{
    if (Owner && Owner->GetScene() && Owner->GetScene()->HasStarted()) return false;
    if (mask.Empty() || strength <= 0.f || layer < 0 ||
        layer >= static_cast<int>(layers.size()) || heights.empty()) return false;
    glm::dvec2 minimum, maximum;
    if (!mask.Bounds(minimum, maximum)) return false;
    minimum -= glm::dvec2(offsetX, offsetZ);
    maximum -= glm::dvec2(offsetX, offsetZ);
    if (maximum.x < 0.0 || maximum.y < 0.0 || minimum.x > cellsX * cellWidth ||
        minimum.y > cellsY * cellDepth) return false;
    const size_t layerCount = layers.size();
    EnsurePaintMap();
    const bool map = HasPaintMap();
    const int width = map ? PaintWidth() : cellsX + 1;
    const int height = map ? PaintHeight() : cellsY + 1;
    const double stepX = map ? double(cellWidth) / paintResolution : cellWidth;
    const double stepZ = map ? double(cellDepth) / paintResolution : cellDepth;
    std::vector<float>& target = map ? paintWeights : weights;
    if (target.size() != size_t(width) * height * layerCount) return false;
    const int minCol = std::max(0, int(std::floor(minimum.x / stepX)));
    const int maxCol = std::min(width - 1, int(std::ceil(maximum.x / stepX)));
    const int minRow = std::max(0, int(std::floor(minimum.y / stepZ)));
    const int maxRow = std::min(height - 1, int(std::ceil(maximum.y / stepZ)));
    const auto ruleLayers = rules ? ResolveRuleLayers(*rules) : Engine::Brushes::BrushLayerSet{};
    bool changed = false;
    for (int row = minRow; row <= maxRow; ++row)
        for (int col = minCol; col <= maxCol; ++col)
        {
            float amount = std::clamp(strength * mask.Weight(offsetX + col * stepX,
                offsetZ + row * stepZ), 0.f, 1.f);
            if (amount > 0.f && rules)
            {
                BrushOptions ruled;
                ruled.rules = rules;
                ruled.trailDistance = trailDistance;
                amount *= RulesWeightAt(std::clamp(int(std::lround(col * stepX / cellWidth)), 0, cellsX),
                    std::clamp(int(std::lround(row * stepZ / cellDepth)), 0, cellsY), heights, ruled,
                    ruleLayers);
            }
            float* sample = &target[(size_t(row) * width + col) * layerCount];
            if (PaintSample(sample, layerCount, layer, amount, erase))
            {
                NormalizeSample(sample, layerCount);
                changed = true;
            }
        }
    if (!changed) return false;
    if (map)
    {
        m_paintTextureDirty = true;
        Rebuild();
    }
    else
        Rebuild();
    MarkConfigurationDirty();
    return true;
}
}

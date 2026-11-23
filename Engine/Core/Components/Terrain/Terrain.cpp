#include <map>
#include <mutex>
#include <miniz.h>
#include "Core/Graphics/IGraphicsProvider.h"
#include "Core/Graphics/IGraphicsTexture.h"
#include "Terrain.h"
#include "Core/Components/Terrain/TerrainObjectLayer.h"
#include "Core/Components/Terrain/TerrainRegionEdit.h"
#include "Core/Components/Terrain/TerrainSurfaceSample.h"
#include "Core/Components/Terrain/TerrainSurfaceWinding.h"
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
// Metres a displaced grid stays inside the stamps' rock; keep equal to
// DISPLACE_INSET in tools/terrain_stamp_morph.py.
constexpr float DisplacedStampInset = 0.35f;

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
template<class T> bool ReadBoundedVector(std::ifstream& stream,
    std::vector<T>& values, uint64_t maximum)
{
    uint64_t count = 0;
    if (!ReadValue(stream, count) || count == 0 || count > maximum) return false;
    values.resize(static_cast<size_t>(count));
    stream.read(reinterpret_cast<char*>(values.data()),
        static_cast<std::streamsize>(count * sizeof(T)));
    return static_cast<bool>(stream);
}
using CachedAppearance = Engine::Components::Terrain::BakedAppearance;
static_assert(sizeof(CachedAppearance) == 24);
int64_t MaterialStamp(const std::string& material)
{
    if (material.empty()) return 0;
    std::error_code error;
    const auto time = std::filesystem::last_write_time(
        Engine::Components::Mesh::ResolveFilePath(material), error);
    return error ? 0 : static_cast<int64_t>(time.time_since_epoch().count());
}
template<class NormalAt>
glm::vec3 FilterLodNormal(int x, int y, int stride,
    int cellsX, int cellsY, NormalAt&& normalAt)
{
    const glm::vec3 original = normalAt(x, y);
    // Shared edges retain the exact detailed normal across unequal LODs.
    if (stride <= 1 || x == 0 || y == 0 ||
        x == cellsX || y == cellsY) return original;
    const int radius = stride / 2;
    glm::vec3 sum(0.f);
    for (int row = std::max(0, y - radius);
        row <= std::min(cellsY, y + radius); ++row)
        for (int col = std::max(0, x - radius);
            col <= std::min(cellsX, x + radius); ++col)
            sum += normalAt(col, row);
    return glm::dot(sum, sum) > 0.000001f
        ? glm::normalize(sum) : original;
}
void ReduceMeshForLod(std::vector<Engine::Components::Mesh::Vertex>& vertices,
    std::vector<uint32_t>& indices, int cellsX, int cellsY, int stride)
{
    if (stride <= 1 || cellsX < stride || cellsY < stride ||
        cellsX % stride || cellsY % stride) return;
    const auto full = std::move(vertices);
    vertices.clear();
    indices.clear();
    std::vector<uint32_t> remap(full.size(), UINT32_MAX);
    auto vertex = [&](int x, int y) {
        const size_t source = static_cast<size_t>(y) * (cellsX + 1) + x;
        if (remap[source] == UINT32_MAX)
        {
            remap[source] = static_cast<uint32_t>(vertices.size());
            vertices.push_back(full[source]);
            const glm::vec3 filtered = FilterLodNormal(x, y, stride,
                cellsX, cellsY, [&](int col, int row) {
                    const auto& sample = full[
                        static_cast<size_t>(row) * (cellsX + 1) + col];
                    return glm::vec3(sample.normal[0], sample.normal[1],
                        sample.normal[2]);
                });
            vertices.back().normal[0] = filtered.x;
            vertices.back().normal[1] = filtered.y;
            vertices.back().normal[2] = filtered.z;
        }
        return remap[source];
    };
    // A fan in each coarse patch retains every outer edge sample. Adjacent
    // patches share their corner vertices, and every neighboring LOD has the
    // same edge segments and height samples.
    for (int y = 0; y < cellsY; y += stride)
        for (int x = 0; x < cellsX; x += stride)
        {
            const int endX = std::min(x + stride, cellsX);
            const int endY = std::min(y + stride, cellsY);
            const int midX = x + (endX - x) / 2;
            const int midY = y + (endY - y) / 2;
            const uint32_t center = vertex(midX, midY);
            std::vector<uint32_t> boundary;
            const int leftStep = x == 0 ? 1 : endY - y;
            const int bottomStep = endY == cellsY ? 1 : endX - x;
            const int rightStep = endX == cellsX ? 1 : endY - y;
            const int topStep = y == 0 ? 1 : endX - x;
            for (int row = y; row < endY; row += leftStep)
                boundary.push_back(vertex(x, row));
            for (int col = x; col < endX; col += bottomStep)
                boundary.push_back(vertex(col, endY));
            for (int row = endY; row > y; row -= rightStep)
                boundary.push_back(vertex(endX, row));
            for (int col = endX; col > x; col -= topStep)
                boundary.push_back(vertex(col, y));
            for (size_t i = 0; i < boundary.size(); ++i)
                indices.insert(indices.end(), {center, boundary[i],
                    boundary[(i + 1) % boundary.size()]});
        }
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
    // Offsets are metres on the old grid's vertices; a new grid starts plain.
    lateralOffsets.clear();
    NormalizeWeights();
    Rebuild();
}

glm::vec3 Terrain::GridPosition(int col, int row) const
{
    col = std::clamp(col, 0, cellsX);
    row = std::clamp(row, 0, cellsY);
    const size_t i = static_cast<size_t>(row) * (cellsX + 1) + col;
    glm::vec3 position(col * cellWidth, i < heights.size() ? heights[i] : 0.f, row * cellDepth);
    if (HasLateralOffsets())
    {
        position.x += lateralOffsets[i * 2];
        position.z += lateralOffsets[i * 2 + 1];
    }
    return position;
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

float Terrain::BrushFalloff(float distance, float radius, float hardness)
{
    if (radius <= 0.f || distance >= radius) return 0.f;
    if (hardness < 0.f) return 1.f - distance / radius;
    const float inner = radius * std::min(hardness, .999f);
    if (distance <= inner) return 1.f;
    const float t = 1.f - (distance - inner) / (radius - inner);
    return t * t * (3.f - 2.f * t);
}

bool Terrain::ApplyBrush(float x, float z, float radius, float strength,
    Brush brush, int layer, bool realignObjects, const BrushOptions& options)
{
    if (Owner && Owner->GetScene() && Owner->GetScene()->HasStarted()) return false;
    if (radius <= 0.f || strength <= 0.f || heights.empty()) return false;
    if (brush == Brush::Paint && (layer < 0 || layer >= static_cast<int>(layers.size())))
        return false;
    const std::vector<float> before = heights;
    bool changed = false;
    EnsurePaintMap();
    if (brush == Brush::Paint && HasPaintMap())
    {
        // Paint at the map's resolution; the vertex weights follow in Rebuild.
        const size_t layerCount = layers.size();
        const int width = PaintWidth(), height = PaintHeight();
        const float stepX = cellWidth / paintResolution, stepZ = cellDepth / paintResolution;
        const int minCol = std::max(0, int(std::floor((x - radius) / stepX)));
        const int maxCol = std::min(width - 1, int(std::ceil((x + radius) / stepX)));
        const int minRow = std::max(0, int(std::floor((z - radius) / stepZ)));
        const int maxRow = std::min(height - 1, int(std::ceil((z + radius) / stepZ)));
        for (int row = minRow; row <= maxRow; ++row)
            for (int col = minCol; col <= maxCol; ++col)
            {
                const float dx = col * stepX - x, dz = row * stepZ - z;
                const float amount = std::clamp(strength * BrushFalloff(
                    std::sqrt(dx * dx + dz * dz), radius, options.hardness), 0.f, 1.f);
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
            RebuildRegion(int(std::floor(float(minCol) / paintResolution)),
                int(std::floor(float(minRow) / paintResolution)),
                int(std::ceil(float(maxCol) / paintResolution)),
                int(std::ceil(float(maxRow) / paintResolution)));
            MarkConfigurationDirty();
        }
        return changed;
    }
    const int firstCol = std::max(0, int(std::floor((x - radius) / cellWidth)));
    const int lastCol = std::min(cellsX, int(std::ceil((x + radius) / cellWidth)));
    const int firstRow = std::max(0, int(std::floor((z - radius) / cellDepth)));
    const int lastRow = std::min(cellsY, int(std::ceil((z + radius) / cellDepth)));
    for (int row = firstRow; row <= lastRow; ++row)
        for (int col = firstCol; col <= lastCol; ++col)
        {
            const float dx = col * cellWidth - x, dz = row * cellDepth - z;
            const float distance = std::sqrt(dx * dx + dz * dz);
            if (distance >= radius) continue;
            const float amount = std::clamp(strength *
                BrushFalloff(distance, radius, options.hardness), 0.f, 1.f);
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
        RebuildRegion(firstCol, firstRow, lastCol, lastRow);
        MarkConfigurationDirty();
        if (brush != Brush::Paint)
            FollowSurfaceEdit(before, x - radius, z - radius, x + radius, z + radius,
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
    if (!Owner || heights.size() != static_cast<size_t>(cellsX + 1) * (cellsY + 1)) return;
    m_bakedAppearance.clear();
    for (auto& lod : m_bakedLods) lod = {};
    NormalizeWeights();
    if (compactMesh && paintResolution <= 0) paintResolution = 2;
    EnsurePaintMap();
    SyncVertexWeightsFromPaint();
    RefreshMaterials();
    m_layerTextured.fill(false);
    for (size_t layer = 0; layer < std::min<size_t>(layers.size(), 6); ++layer)
        m_layerTextured[layer] = layer < m_materials.size() && m_materials[layer] &&
            m_materials[layer]->baseColorTexture &&
            std::filesystem::exists(m_materials[layer]->baseColorTexture->GetFilePath());
    Mesh* mesh = Owner->GetComponent<Mesh>();
    if (!mesh) mesh = Owner->AddComponent<Mesh>();
    std::vector<Mesh::Vertex> vertices(heights.size());
    std::vector<uint32_t> indices;
    indices.reserve(static_cast<size_t>(cellsX) * cellsY * 6);
    for (int row = 0; row <= cellsY; ++row)
        for (int col = 0; col <= cellsX; ++col)
            BuildVertex(col, row, vertices[static_cast<size_t>(row * (cellsX + 1) + col)]);
    for (int row = 0; row < cellsY; ++row)
        for (int col = 0; col < cellsX; ++col)
        {
            const uint32_t a = row * (cellsX + 1) + col, b = a + cellsX + 1;
            indices.insert(indices.end(), {a, b, a + 1, a + 1, b, b + 1});
        }
    ReduceMeshForLod(vertices, indices, cellsX, cellsY, m_meshLodStride);
    const size_t gridIndices = indices.size();
    AppendStampGeometry(vertices, indices, m_meshLodStride, heights,
        [&](size_t source, Mesh::Vertex& vertex)
        {
            Mesh::Vertex grid;
            BuildVertex(static_cast<int>(source % (cellsX + 1)),
                static_cast<int>(source / (cellsX + 1)), grid);
            std::copy_n(grid.color, 4, vertex.color);
            for (int set = 2; set < 4; ++set)
                std::copy_n(grid.uvChannels[set], 4, vertex.uvChannels[set]);
        });
    OrientTerrainSurfaceUp(vertices, indices);
    InstallCellMesh(*mesh, std::move(vertices), std::move(indices), gridIndices);
    if (m_provider && !mesh->IsReady()) mesh->OnAfterDeserialize(m_provider);
}

void Terrain::PrepareCompactPaint()
{
    if (!compactMesh) return;
    if (paintResolution <= 0) paintResolution = 2;
    EnsurePaintMap();
}

bool Terrain::InstallCellMesh(Mesh& mesh, std::vector<Mesh::Vertex>&& vertices,
    std::vector<uint32_t>&& indices, size_t gridIndices)
{
    bool installed = false;
    if (compactMesh && HasPaintMap())
    {
        Mesh::TerrainMeshData geometry;
        geometry.vertices.resize(vertices.size());
        for (size_t i = 0; i < vertices.size(); ++i)
        {
            auto& target = geometry.vertices[i];
            std::copy_n(vertices[i].pos, 3, target.pos);
            std::copy_n(vertices[i].normal, 3, target.normal);
            std::copy_n(vertices[i].uv, 2, target.uv);
            std::copy_n(vertices[i].color, 4, target.color);
        }
        vertices.clear();
        vertices.shrink_to_fit();
        geometry.indices = std::move(indices);
        installed = mesh.SetTerrainGeometry(std::move(geometry));
    }
    else
        installed = mesh.SetIndexedGeometry(std::move(vertices), std::move(indices));
    // Stamps are not collided with; a sculpt is the cell's ground.
    mesh.collisionIndexCount = stamps.empty() || Sculpt() ? 0 : gridIndices;
    mesh.occluderIndices = std::move(m_sculptOccluder);
    m_sculptOccluder.clear();
    return installed;
}

const TerrainSculpt* Terrain::Sculpt() const
{
    for (const auto& instance : stamps)
        if (instance.sculpt) return instance.sculpt.get();
    return nullptr;
}

std::vector<Mesh::Vertex> Terrain::GridVertices(const Mesh& mesh) const
{
    std::vector<Mesh::Vertex> grid;
    if (mesh.UsesTerrainVertexFormat())
    {
        const auto& compact = mesh.GetTerrainVertices();
        if (compact.size() < heights.size()) return grid;
        grid.resize(heights.size());
        for (size_t i = 0; i < grid.size(); ++i)
        {
            std::copy_n(compact[i].pos, 3, grid[i].pos);
            std::copy_n(compact[i].normal, 3, grid[i].normal);
            std::copy_n(compact[i].uv, 2, grid[i].uv);
            std::copy_n(compact[i].color, 4, grid[i].color);
            for (size_t layer = 0; layer < std::min<size_t>(layers.size(), 6); ++layer)
                if (i * layers.size() + layer < weights.size())
                    grid[i].uvChannels[2 + layer / 4][layer % 4] = weights[i * layers.size() + layer];
        }
        return grid;
    }
    if (mesh.GetVertices().size() < heights.size()) return grid;
    grid.assign(mesh.GetVertices().begin(), mesh.GetVertices().begin() + heights.size());
    return grid;
}

void Terrain::AppendStampGeometry(std::vector<Mesh::Vertex>& vertices,
    std::vector<uint32_t>& indices, int stride, const std::vector<float>& heightsSource,
    const std::function<void(size_t, Mesh::Vertex&)>& paint) const
{
    if (stamps.empty() || heightsSource.size() != static_cast<size_t>(cellsX + 1) * (cellsY + 1))
        return;
    const float width = cellsX * cellWidth, depth = cellsY * cellDepth;
    for (const auto& instance : stamps)
    {
        const TerrainSculpt* sculpt = instance.sculpt.get();
        if (!sculpt || sculpt->levels.empty()) continue;
        // The sculpt is the cell's surface: the grid's triangles go (its
        // vertices stay, unused, first in the list).
        indices.clear();
        const size_t level = std::min<size_t>(stride <= 1 ? 0 : stride <= 2 ? 1 : 2,
            sculpt->levels.size() - 1);
        const bool surfaced = instance.materialIndex >= 0 &&
            static_cast<size_t>(instance.materialIndex) < stampMaterials.size() && layers.size() <= 6;
        const float slice = static_cast<float>(layers.size() + std::max(instance.materialIndex, 0));
        std::vector<uint32_t> slots(sculpt->vertices.size(), UINT32_MAX);
        const auto& source = sculpt->levels[level];
        for (size_t t = 0; t + 2 < source.size(); t += 3)
        {
            for (size_t k = 0; k < 3; ++k)
            {
                uint32_t& slot = slots[source[t + k]];
                if (slot == UINT32_MAX)
                {
                    const auto& point = sculpt->vertices[source[t + k]];
                    Mesh::Vertex vertex{};
                    vertex.pos[0] = point.position.x; vertex.pos[1] = point.position.y;
                    vertex.pos[2] = point.position.z;
                    vertex.normal[0] = point.normal.x; vertex.normal[1] = point.normal.y;
                    vertex.normal[2] = point.normal.z;
                    vertex.uv[0] = std::clamp(point.position.x / width, 0.f, 1.f);
                    vertex.uv[1] = std::clamp(point.position.z / depth, 0.f, 1.f);
                    const int col = std::clamp(static_cast<int>(std::lround(point.position.x / cellWidth)), 0, cellsX);
                    const int row = std::clamp(static_cast<int>(std::lround(point.position.z / cellDepth)), 0, cellsY);
                    vertex.color[3] = 1.f;
                    paint(static_cast<size_t>(row) * (cellsX + 1) + col, vertex);
                    if (surfaced)
                    {
                        // Rock projected from three sides (Object.hlsl): the
                        // slice as -(slice + 1), the tiling, the scans' tint.
                        vertex.uvChannels[4][0] = sculpt->repeatsPerMetre;
                        vertex.uvChannels[4][1] = 0.f;
                        vertex.uvChannels[4][2] = -(slice + 1.f);
                        vertex.uvChannels[4][3] = std::clamp(point.weight, 0.f, 1.f);
                        vertex.uvChannels[0][2] = point.tint.x; vertex.uvChannels[0][3] = point.tint.y;
                        vertex.uvChannels[1][2] = point.tint.z;
                    }
                    slot = static_cast<uint32_t>(vertices.size());
                    vertices.push_back(vertex);
                }
                indices.push_back(slot);
            }
        }
        // The occlusion buffer's copy: the coarsest level, over the same
        // vertices (simplified levels keep a subset of the finer ones' points).
        m_sculptOccluder.clear();
        const auto& coarse = sculpt->levels.back();
        if (&coarse != &source)
            for (size_t t = 0; t + 2 < coarse.size(); t += 3)
            {
                const uint32_t a = slots[coarse[t]], b = slots[coarse[t + 1]], c = slots[coarse[t + 2]];
                if (a == UINT32_MAX || b == UINT32_MAX || c == UINT32_MAX) continue;
                m_sculptOccluder.insert(m_sculptOccluder.end(), {a, b, c});
            }
        return;
    }
    // The stamp's simplified level for this mesh level: full detail, half,
    // and quarter grid detail and coarser (tools/stamp_simplify.py). At every
    // stamp point a cliff cell held ~85,000 vertices against the grid's 1,089,
    // and the terrain's memory budget then unloaded and reloaded cells.
    const size_t wantedLevel = stride <= 1 ? 0 : stride <= 2 ? 1 : 2;
    for (const auto& instance : stamps)
    {
        const TerrainStamp* stamp = instance.stamp.get();
        if (!stamp || stamp->levels.empty()) continue;
        const auto& triangles = stamp->levels[std::min(wantedLevel, stamp->levels.size() - 1)];
        const glm::mat3 rotation = glm::mat3(instance.transform);
        // Into the rock: the patch's border tucks into the terrain behind the
        // stamp (the hillside it is set into), not onto the ground straight
        // below, which under a tall piece's top edge is tens of metres down
        // and pulled the edge into needles.
        const glm::vec3 inward = -glm::normalize(rotation * stamp->direction);
        // Stamp surface: an array slice after the painted layers, sampled at
        // the point's place on the stamp grid (Object.hlsl, uvChannels4); the
        // grid's axes carry its normals (tangent, uvChannels0.zw/1.z).
        const bool surfaced = instance.materialIndex >= 0 &&
            static_cast<size_t>(instance.materialIndex) < stampMaterials.size() && layers.size() <= 6;
        const float slice = static_cast<float>(layers.size() + std::max(instance.materialIndex, 0));
        const glm::vec3 axisU = glm::normalize(rotation * stamp->axisU);
        const glm::vec3 axisV = glm::normalize(rotation * stamp->axisV);
        std::vector<uint32_t> slots(stamp->points.size(), UINT32_MAX);
        const auto vertexAt = [&](uint32_t index)
        {
            uint32_t& slot = slots[index];
            if (slot != UINT32_MAX) return slot;
            const auto& point = stamp->points[index];
            const glm::vec3 placed = glm::vec3(instance.transform * glm::vec4(point.position, 1.f));
            const glm::vec3 placedNormal = glm::normalize(rotation * point.normal);
            const float weight = std::clamp(point.weight, 0.f, 1.f);
            float ground = 0.f;
            glm::vec3 groundNormal(0.f, 1.f, 0.f);
            SampleTerrainSurface(heightsSource, cellsX, cellsY, cellWidth, cellDepth,
                placed.x, placed.z, ground, &groundNormal);
            glm::vec3 anchor(placed.x, ground, placed.z);
            // A displaced grid already drapes over the stamp, DisplacedStampInset
            // inside its rock (tools/terrain_stamp_morph.py displace): the
            // border only steps in to meet it. Pulling it onto the height
            // field would sample the undisplaced grid and stretch it into a
            // shard.
            if (HasLateralOffsets())
            {
                anchor = placed - placedNormal * DisplacedStampInset;
                groundNormal = placedNormal;
            }
            // Points that blend: march inward until the terrain is reached.
            else if (weight < 1.f && placed.y > ground)
            {
                constexpr float Step = 1.f, Reach = 40.f;
                for (float travelled = Step; travelled <= Reach; travelled += Step)
                {
                    const glm::vec3 probe = placed + inward * travelled;
                    float height = 0.f;
                    glm::vec3 probeNormal(0.f, 1.f, 0.f);
                    SampleTerrainSurface(heightsSource, cellsX, cellsY, cellWidth, cellDepth,
                        probe.x, probe.z, height, &probeNormal);
                    if (probe.y <= height)
                    {
                        anchor = glm::vec3(probe.x, height, probe.z);
                        groundNormal = probeNormal;
                        break;
                    }
                }
            }
            // On and outside the outline the patch lies on the terrain; it
            // reaches the stamp's shape over the weight's ramp.
            const glm::vec3 position = glm::mix(anchor, placed, weight);
            glm::vec3 normal = glm::mix(groundNormal, placedNormal, weight);
            normal = glm::dot(normal, normal) > 1.e-8f ? glm::normalize(normal) : groundNormal;
            Mesh::Vertex vertex{};
            vertex.pos[0] = position.x; vertex.pos[1] = position.y; vertex.pos[2] = position.z;
            vertex.normal[0] = normal.x; vertex.normal[1] = normal.y; vertex.normal[2] = normal.z;
            // The paint map is sampled at the ground position under the point.
            vertex.uv[0] = std::clamp(position.x / width, 0.f, 1.f);
            vertex.uv[1] = std::clamp(position.z / depth, 0.f, 1.f);
            const int col = std::clamp(static_cast<int>(std::lround(position.x / cellWidth)), 0, cellsX);
            const int row = std::clamp(static_cast<int>(std::lround(position.z / cellDepth)), 0, cellsY);
            vertex.color[3] = 1.f;
            paint(static_cast<size_t>(row) * (cellsX + 1) + col, vertex);
            if (surfaced)
            {
                vertex.uvChannels[4][0] = static_cast<float>(index % stamp->width) / (stamp->width - 1);
                vertex.uvChannels[4][1] = static_cast<float>(index / stamp->width) / (stamp->height - 1);
                vertex.uvChannels[4][2] = slice;
                vertex.uvChannels[4][3] = weight;
                vertex.tangent[0] = axisU.x; vertex.tangent[1] = axisU.y;
                vertex.tangent[2] = axisU.z; vertex.tangent[3] = 1.f;
                vertex.uvChannels[0][2] = axisV.x; vertex.uvChannels[0][3] = axisV.y;
                vertex.uvChannels[1][2] = axisV.z;
            }
            slot = static_cast<uint32_t>(vertices.size());
            vertices.push_back(vertex);
            return slot;
        };
        for (size_t t = 0; t + 2 < triangles.size(); t += 3)
            indices.insert(indices.end(),
                {vertexAt(triangles[t]), vertexAt(triangles[t + 1]), vertexAt(triangles[t + 2])});
    }
}

void Terrain::BuildVertex(int col, int row, Mesh::Vertex& v) const
{
    const size_t i = static_cast<size_t>(row * (cellsX + 1) + col);
    const auto sample = [&](int sx, int sy) {
        return heights[static_cast<size_t>(std::clamp(sy, 0, cellsY) *
            (cellsX + 1) + std::clamp(sx, 0, cellsX))];
    };
    glm::vec3 normal = glm::normalize(glm::vec3(
        (sample(col - 1, row) - sample(col + 1, row)) / (2.f * cellWidth),
        1.f, (sample(col, row - 1) - sample(col, row + 1)) / (2.f * cellDepth)));
    const glm::vec3 position = GridPosition(col, row);
    if (HasLateralOffsets())
    {
        // From the displaced neighbours (central differences; one-sided at
        // the border): an undercut face's normal leans out and down.
        const glm::vec3 alongX = GridPosition(col + 1, row) - GridPosition(col - 1, row);
        const glm::vec3 alongZ = GridPosition(col, row + 1) - GridPosition(col, row - 1);
        const glm::vec3 cross = glm::cross(alongZ, alongX);
        if (glm::dot(cross, cross) > 1.e-12f) normal = glm::normalize(cross);
    }
    v = {};
    v.pos[0] = position.x; v.pos[1] = position.y; v.pos[2] = position.z;
    v.normal[0] = normal.x; v.normal[1] = normal.y; v.normal[2] = normal.z;
    v.uv[0] = static_cast<float>(col) / cellsX;
    v.uv[1] = static_cast<float>(row) / cellsY;
    glm::vec3 color(0.f);
    for (size_t l = 0; l < layers.size(); ++l)
    {
        const float weight = weights[i * layers.size() + l];
        if (l < 6)
            v.uvChannels[2 + l / 4][l % 4] = weight;
        if (l >= 6 || !m_layerTextured[l])
            color += (l < m_materials.size() && m_materials[l]
            ? m_materials[l]->diffuseColor : layers[l].previewColor) * weight;
    }
    v.color[0] = color.x; v.color[1] = color.y; v.color[2] = color.z;
    v.color[3] = 1.f;
}

void Terrain::RebuildRegion(int minCol, int minRow, int maxCol, int maxRow)
{
    Mesh* mesh = Owner ? Owner->GetComponent<Mesh>() : nullptr;
    bool materialsCurrent = m_materialPaths.size() == layers.size();
    for (size_t i = 0; materialsCurrent && i < layers.size(); ++i)
        materialsCurrent = m_materialPaths[i] == layers[i].material;
    if (!mesh || m_meshLodStride != 1 || mesh->UsesTerrainVertexFormat() || !stamps.empty() ||
        mesh->GetVertices().size() != heights.size() || !materialsCurrent ||
        weights.size() != heights.size() * layers.size())
    {
        Rebuild();
        return;
    }
    m_bakedAppearance.clear();
    for (auto& lod : m_bakedLods) lod = {};
    // Normals read one sample either side.
    minRow = std::clamp(minRow - 1, 0, cellsY);
    maxRow = std::clamp(maxRow + 1, 0, cellsY);
    minCol = std::clamp(minCol - 1, 0, cellsX);
    maxCol = std::clamp(maxCol + 1, 0, cellsX);
    if (minRow > maxRow || minCol > maxCol) return;
    SyncVertexWeightsFromPaint();
    // Whole rows form one contiguous vertex range.
    std::vector<Mesh::Vertex> vertices(mesh->GetVertices().begin() + size_t(minRow) * (cellsX + 1),
        mesh->GetVertices().begin() + size_t(maxRow + 1) * (cellsX + 1));
    for (int row = minRow; row <= maxRow; ++row)
        for (int col = minCol; col <= maxCol; ++col)
            BuildVertex(col, row, vertices[size_t(row - minRow) * (cellsX + 1) + col]);
    if (!mesh->UpdateIndexedVertexRange(size_t(minRow) * (cellsX + 1), vertices))
        Rebuild();
}

void Terrain::SetMeshLodStride(int stride)
{
    stride = stride <= 1 ? 1 : stride <= 2 ? 2 : stride <= 4 ? 4 : 8;
    while (stride > 1 && (cellsX < stride || cellsY < stride ||
        cellsX % stride || cellsY % stride)) stride /= 2;
    if (m_meshLodStride == stride) return;
    m_meshLodStride = stride;
    if (Owner && Owner->GetComponent<Mesh>() && !InstallBakedLod(stride))
        Rebuild();
}

bool Terrain::InstallBakedLod(int stride)
{
    const size_t lodIndex = stride <= 1 ? 0 : stride <= 2 ? 1 :
        stride <= 4 ? 2 : 3;
    const auto& lod = m_bakedLods[lodIndex];
    if (!Owner || m_bakedAppearance.size() != heights.size() ||
        lod.sourceVertices.empty() || lod.indices.empty()) return false;
    std::vector<Mesh::Vertex> vertices;
    vertices.reserve(lod.sourceVertices.size());
    for (uint32_t source : lod.sourceVertices)
    {
        if (source >= heights.size()) return false;
        const int col = source % (cellsX + 1);
        const int row = source / (cellsX + 1);
        Mesh::Vertex vertex{};
        const glm::vec3 position = GridPosition(col, row);
        vertex.pos[0] = position.x;
        vertex.pos[1] = position.y;
        vertex.pos[2] = position.z;
        const glm::vec3 filtered = FilterLodNormal(col, row, stride,
            cellsX, cellsY, [&](int sampleX, int sampleY) {
                const auto& sample = m_bakedAppearance[
                    static_cast<size_t>(sampleY) * (cellsX + 1) + sampleX];
                return glm::vec3(sample.normal[0], sample.normal[1],
                    sample.normal[2]);
            });
        vertex.normal[0] = filtered.x;
        vertex.normal[1] = filtered.y;
        vertex.normal[2] = filtered.z;
        vertex.uv[0] = static_cast<float>(col) / cellsX;
        vertex.uv[1] = static_cast<float>(row) / cellsY;
        std::copy_n(m_bakedAppearance[source].color, 3, vertex.color);
        vertex.color[3] = 1.f;
        for (size_t layer = 0; layer < std::min<size_t>(layers.size(), 6); ++layer)
            vertex.uvChannels[2 + layer / 4][layer % 4] =
                weights[static_cast<size_t>(source) * layers.size() + layer];
        vertices.push_back(vertex);
    }
    Mesh* mesh = Owner->GetComponent<Mesh>();
    if (!mesh) mesh = Owner->AddComponent<Mesh>();
    auto indices = lod.indices;
    const size_t gridIndices = indices.size();
    AppendStampGeometry(vertices, indices, stride, heights,
        [&](size_t source, Mesh::Vertex& vertex)
        {
            std::copy_n(m_bakedAppearance[source].color, 3, vertex.color);
            for (size_t layer = 0; layer < std::min<size_t>(layers.size(), 6); ++layer)
                vertex.uvChannels[2 + layer / 4][layer % 4] = weights[source * layers.size() + layer];
        });
    OrientTerrainSurfaceUp(vertices, indices);
    PrepareCompactPaint();
    return InstallCellMesh(*mesh, std::move(vertices), std::move(indices), gridIndices);
}

uint64_t Terrain::GetBakedCpuMemoryBytes() const
{
    uint64_t bytes = m_bakedAppearance.capacity() * sizeof(BakedAppearance);
    for (const auto& lod : m_bakedLods)
        bytes += (lod.sourceVertices.capacity() + lod.indices.capacity()) *
            sizeof(uint32_t);
    return bytes;
}

Terrain::Prepared Terrain::TakePrepared()
{
    Prepared result;
    result.cellsX = cellsX; result.cellsY = cellsY;
    result.cellWidth = cellWidth; result.cellDepth = cellDepth;
    result.textureTiling = textureTiling; result.lodStride = m_meshLodStride;
    result.heights = std::move(heights);
    result.weights = std::move(weights);
    result.paintResolution = paintResolution;
    result.paintWeights = std::move(paintWeights);
    result.layers = std::move(layers);
    result.materials = std::move(m_materials);
    result.materialPaths = std::move(m_materialPaths);
    result.bakedAppearance = std::move(m_bakedAppearance);
    result.bakedLods = std::move(m_bakedLods);
    result.bakedTextureMask = m_bakedTextureMask;
    result.stamps = std::move(stamps);
    result.stampMaterials = std::move(stampMaterials);
    result.compactMesh = compactMesh;
    if (auto* mesh = Owner ? Owner->GetComponent<Mesh>() : nullptr)
    {
        result.collisionIndexCount = mesh->collisionIndexCount;
        result.occluderIndices = mesh->occluderIndices;
        if (mesh->UsesTerrainVertexFormat())
            result.compact = mesh->TakeTerrainGeometry();
        else
        {
            result.vertices = mesh->GetVertices();
            result.indices = mesh->GetIndices();
        }
    }
    return result;
}

bool Terrain::InstallPrepared(Prepared prepared, IGraphicsProvider* provider)
{
    const bool compactPrepared = !prepared.compact.vertices.empty() && !prepared.compact.indices.empty();
    if (!Owner || (!compactPrepared && (prepared.vertices.empty() || prepared.indices.empty())))
        return false;
    cellsX = prepared.cellsX; cellsY = prepared.cellsY;
    cellWidth = prepared.cellWidth; cellDepth = prepared.cellDepth;
    textureTiling = prepared.textureTiling;
    m_meshLodStride = prepared.lodStride;
    heights = std::move(prepared.heights);
    weights = std::move(prepared.weights);
    paintResolution = prepared.paintResolution;
    paintWeights = std::move(prepared.paintWeights);
    m_paintTextureDirty = true;
    layers = std::move(prepared.layers);
    m_materials = std::move(prepared.materials);
    m_materialPaths = std::move(prepared.materialPaths);
    m_bakedAppearance = std::move(prepared.bakedAppearance);
    m_bakedLods = std::move(prepared.bakedLods);
    m_bakedTextureMask = prepared.bakedTextureMask;
    stamps = std::move(prepared.stamps);
    stampMaterials = std::move(prepared.stampMaterials);
    // The array list (layers, then stamp surfaces) follows the materials.
    RefreshMaterials();
    compactMesh = prepared.compactMesh;
    m_provider = nullptr;
    Mesh* mesh = Owner->GetComponent<Mesh>();
    if (!mesh) mesh = Owner->AddComponent<Mesh>();
    mesh->DeferGraphicsBuffers();
    OrientTerrainSurfaceUp(prepared.vertices, prepared.indices);
    const bool installed = compactPrepared
        ? mesh->SetTerrainGeometry(std::move(prepared.compact))
        : prepared.bounds
        ? mesh->SetPreparedIndexedGeometry(std::move(prepared.vertices),
            std::move(prepared.indices), *prepared.bounds)
        : mesh->SetIndexedGeometry(std::move(prepared.vertices),
            std::move(prepared.indices));
    if (!installed) return false;
    mesh->collisionIndexCount = prepared.collisionIndexCount;
    mesh->occluderIndices = std::move(prepared.occluderIndices);
    if (provider && !DeferGraphicsPreparation(provider))
        PrepareGraphicsResources(provider);
    return true;
}

void Terrain::FinalizePreparedGraphics(IGraphicsProvider* provider)
{
    if (provider && !DeferGraphicsPreparation(provider))
        PrepareGraphicsResources(provider);
}

bool Terrain::SaveCache(const std::string& path, uint64_t signature,
    bool includeBakedLods) const
{
    const auto* mesh = Owner ? Owner->GetComponent<Mesh>() : nullptr;
    // Stamp patches follow the full-resolution grid in the mesh; only the
    // grid is cached (the patches are rebuilt from the stamps).
    const size_t gridIndices = static_cast<size_t>(cellsX) * cellsY * 6;
    const std::vector<Mesh::Vertex> grid = mesh ? GridVertices(*mesh) : std::vector<Mesh::Vertex>{};
    const size_t meshVertices = mesh ? (mesh->UsesTerrainVertexFormat()
        ? mesh->GetTerrainVertices().size() : mesh->GetVertices().size()) : 0;
    if (!mesh || m_meshLodStride != 1 || grid.size() != heights.size() ||
        mesh->GetIndices().size() < gridIndices ||
        (stamps.empty() && (meshVertices != heights.size() ||
            mesh->GetIndices().size() != gridIndices)) ||
        weights.size() != heights.size() * layers.size() || layers.size() > 6)
        return false;
    std::error_code error;
    const std::filesystem::path target(path);
    std::filesystem::create_directories(target.parent_path(), error);
    if (error) return false;
    const auto temporary = target.string() + ".tmp";
    std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
    if (!stream) return false;
    const uint64_t magic = 0x31484341434e5254ull; // TRNCACH1
    const uint32_t version = includeBakedLods ? 5 : 4;
    const uint32_t layerCount = static_cast<uint32_t>(layers.size());
    uint8_t texturedMask = 0;
    for (size_t layer = 0; layer < layers.size(); ++layer)
        if (layer < m_materials.size() && m_materials[layer] &&
            m_materials[layer]->baseColorTexture &&
            std::filesystem::exists(m_materials[layer]->baseColorTexture->GetFilePath()))
            texturedMask |= static_cast<uint8_t>(1u << layer);
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
    std::vector<CachedAppearance> appearance(heights.size());
    for (size_t i = 0; i < appearance.size(); ++i)
    {
        const auto& vertex = grid[i];
        std::copy_n(vertex.normal, 3, appearance[i].normal);
        std::copy_n(vertex.color, 3, appearance[i].color);
    }
    valid &= WriteVector(stream, heights) && WriteVector(stream, weights) &&
        WriteVector(stream, appearance);
    if (includeBakedLods)
        for (int stride : {1, 2, 4, 8})
        {
            auto vertices = grid;
            std::vector<uint32_t> indices(mesh->GetIndices().begin(),
                mesh->GetIndices().begin() + gridIndices);
            ReduceMeshForLod(vertices, indices, cellsX, cellsY, stride);
            std::vector<uint32_t> sources;
            sources.reserve(vertices.size());
            for (const auto& vertex : vertices)
            {
                const int col = static_cast<int>(std::lround(
                    vertex.pos[0] / cellWidth));
                const int row = static_cast<int>(std::lround(
                    vertex.pos[2] / cellDepth));
                sources.push_back(static_cast<uint32_t>(row * (cellsX + 1) + col));
            }
            valid &= WriteVector(stream, sources) && WriteVector(stream, indices);
        }
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
        !ReadValue(stream, version) || version != 5 ||
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
        !ReadValue(stream, version) || version != 5 ||
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
        !ReadValue(stream, version) || (version != 4 && version != 5) ||
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
    std::vector<float> cachedHeights, cachedWeights;
    std::vector<CachedAppearance> appearance;
    if (!ReadVector(stream, cachedHeights, vertexCount) ||
        !ReadVector(stream, cachedWeights, vertexCount * layerCount) ||
        !ReadVector(stream, appearance, vertexCount))
        return false;
    std::array<BakedLod, 4> bakedLods;
    if (version == 5)
        for (auto& lod : bakedLods)
        {
            if (!ReadBoundedVector(stream, lod.sourceVertices, vertexCount) ||
                !ReadBoundedVector(stream, lod.indices,
                    static_cast<uint64_t>(x) * y * 6)) return false;
            for (uint32_t source : lod.sourceVertices)
                if (source >= vertexCount) return false;
            for (uint32_t index : lod.indices)
                if (index >= lod.sourceVertices.size()) return false;
        }
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
    if (version == 5)
    {
        cellsX = x; cellsY = y; cellWidth = width; cellDepth = depth;
        textureTiling = tiling;
        heights = std::move(cachedHeights);
        weights = std::move(cachedWeights);
        layers = std::move(cachedLayers);
        m_bakedAppearance = std::move(appearance);
        m_bakedLods = std::move(bakedLods);
        m_bakedTextureMask = texturedMask;
        m_provider = provider;
        m_materialPaths.clear(); m_materials.clear();
        RefreshMaterials();
        if (!InstallBakedLod(m_meshLodStride)) return false;
        if (provider)
            if (auto* mesh = Owner->GetComponent<Mesh>())
                if (!mesh->IsReady()) mesh->OnAfterDeserialize(provider);
        return true;
    }
    std::vector<Mesh::Vertex> vertices(static_cast<size_t>(vertexCount));
    m_bakedAppearance.clear();
    for (auto& lod : m_bakedLods) lod = {};
    for (int row = 0; row <= y; ++row)
        for (int col = 0; col <= x; ++col)
        {
            const size_t i = static_cast<size_t>(row * (x + 1) + col);
            auto& vertex = vertices[i];
            vertex.pos[0] = col * width;
            vertex.pos[1] = cachedHeights[i];
            vertex.pos[2] = row * depth;
            std::copy_n(appearance[i].normal, 3, vertex.normal);
            vertex.uv[0] = static_cast<float>(col) / x;
            vertex.uv[1] = static_cast<float>(row) / y;
            std::copy_n(appearance[i].color, 3, vertex.color);
            vertex.color[3] = 1.f;
            for (uint32_t layer = 0; layer < layerCount; ++layer)
                vertex.uvChannels[2 + layer / 4][layer % 4] =
                    cachedWeights[i * layerCount + layer];
        }
    std::vector<uint32_t> indices;
    indices.reserve(static_cast<size_t>(x) * y * 6);
    for (int row = 0; row < y; ++row)
        for (int col = 0; col < x; ++col)
        {
            const uint32_t a = row * (x + 1) + col, b = a + x + 1;
            indices.insert(indices.end(), {a, b, a + 1, a + 1, b, b + 1});
        }
    ReduceMeshForLod(vertices, indices, x, y, m_meshLodStride);
    // The patches need the cell's size and layers (their surfaces' array
    // slices follow the layers) before the fields below are set.
    cellsX = x; cellsY = y; cellWidth = width; cellDepth = depth;
    layers = cachedLayers;
    const size_t gridIndices = indices.size();
    if (!stamps.empty())
        AppendStampGeometry(vertices, indices, m_meshLodStride, cachedHeights,
            [&](size_t source, Mesh::Vertex& vertex)
            {
                std::copy_n(appearance[source].color, 3, vertex.color);
                for (uint32_t layer = 0; layer < layerCount && layer < 6; ++layer)
                    vertex.uvChannels[2 + layer / 4][layer % 4] = cachedWeights[source * layerCount + layer];
            });
    Mesh* mesh = Owner->GetComponent<Mesh>();
    if (!mesh) mesh = Owner->AddComponent<Mesh>();
    OrientTerrainSurfaceUp(vertices, indices);
    cellsX = x; cellsY = y; cellWidth = width; cellDepth = depth;
    textureTiling = tiling;
    heights = std::move(cachedHeights);
    weights = std::move(cachedWeights);
    layers = std::move(cachedLayers);
    PrepareCompactPaint();
    if (!InstallCellMesh(*mesh, std::move(vertices), std::move(indices), gridIndices))
        return false;
    m_provider = provider;
    m_materialPaths.clear(); m_materials.clear();
    RefreshMaterials();
    if (provider && !mesh->IsReady()) mesh->OnAfterDeserialize(provider);
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
    // Layer arrays: the painted layers, then the stamps' surfaces.
    std::vector<std::string> wanted;
    if (!stampMaterials.empty() && layers.size() <= 6)
    {
        for (const auto& layer : layers) wanted.push_back(layer.material);
        wanted.insert(wanted.end(), stampMaterials.begin(), stampMaterials.end());
    }
    if (wanted != m_arrayMaterialPaths)
    {
        m_arrayMaterialPaths = wanted;
        m_arrayMaterials.clear();
        if (!wanted.empty())
        {
            m_arrayMaterials = m_materials;
            m_arrayMaterials.resize(layers.size());
            // Shared by every cell (one load, and one set of layer arrays:
            // they are keyed by the materials' texture objects).
            static std::mutex mutex;
            static std::map<std::string, std::shared_ptr<Material>> shared;
            std::lock_guard lock(mutex);
            for (const auto& path : stampMaterials)
            {
                auto& material = shared[path];
                if (!material)
                {
                    material = std::make_shared<Material>();
                    if (!material->LoadFromFile(Mesh::ResolveFilePath(path))) material.reset();
                }
                m_arrayMaterials.push_back(material);
            }
        }
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
    if (HasLateralOffsets())
    {
        JsonValue offsets = JsonValue::MakeArray();
        for (float value : lateralOffsets) offsets.Push(JsonValue(value));
        out.Set("lateralOffsets", std::move(offsets));
    }
    if (!stampPlacements.empty())
    {
        JsonValue placed = JsonValue::MakeArray();
        for (const auto& placement : stampPlacements)
        {
            JsonValue matrix = JsonValue::MakeArray();
            for (int column = 0; column < 4; ++column)
                for (int row = 0; row < 4; ++row)
                    matrix.Push(JsonValue(placement.transform[column][row]));
            placed.Push(JsonValue::MakeObject().Set("stamp", JsonValue(placement.stamp))
                .Set("material", JsonValue(placement.material))
                .Set("transform", std::move(matrix)));
        }
        out.Set("stampPlacements", std::move(placed));
    }
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
    lateralOffsets.clear();
    if (value.Has("lateralOffsets") && value["lateralOffsets"].ArraySize() == heights.size() * 2)
    {
        lateralOffsets.resize(heights.size() * 2);
        for (size_t i = 0; i < lateralOffsets.size(); ++i)
            lateralOffsets[i] = value["lateralOffsets"].ArrayAt(i).AsFloat();
    }
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
    // Saved stamps (column-major transforms, cell space). Without any, the
    // owner's stamps stay: TerrainManager sets a streamed cell's before
    // deserializing its saved state.
    stampPlacements.clear();
    if (value.Has("stampPlacements") && value["stampPlacements"].ArraySize() > 0)
    {
        stamps.clear();
        stampMaterials.clear();
        const auto& placed = value["stampPlacements"];
        for (size_t i = 0; i < placed.ArraySize(); ++i)
        {
            const auto& entry = placed.ArrayAt(i);
            StampPlacement placement;
            placement.stamp = entry["stamp"].AsString();
            placement.material = entry.Has("material") ? entry["material"].AsString() : std::string{};
            const auto& matrix = entry["transform"];
            if (matrix.ArraySize() == 16)
                for (int column = 0; column < 4; ++column)
                    for (int row = 0; row < 4; ++row)
                        placement.transform[column][row] = matrix.ArrayAt(column * 4 + row).AsFloat();
            TerrainStampInstance instance;
            instance.stamp = TerrainStamp::Load(Mesh::ResolveFilePath(placement.stamp));
            instance.transform = placement.transform;
            if (!placement.material.empty())
            {
                auto found = std::find(stampMaterials.begin(), stampMaterials.end(), placement.material);
                if (found == stampMaterials.end())
                    found = stampMaterials.insert(stampMaterials.end(), placement.material);
                instance.materialIndex = static_cast<int>(found - stampMaterials.begin());
            }
            if (instance.stamp) stamps.push_back(std::move(instance));
            stampPlacements.push_back(std::move(placement));
        }
    }
    Rebuild();
}

void Terrain::OnAfterDeserialize(IGraphicsProvider* provider)
{
    m_provider = nullptr;
    if (Owner)
        if (auto* mesh = Owner->GetComponent<Mesh>()) mesh->DeferGraphicsBuffers();
    Rebuild();
    if (provider && !DeferGraphicsPreparation(provider))
        PrepareGraphicsResources(provider);
}

void Terrain::PrepareGraphicsResources(IGraphicsProvider* provider)
{
    m_provider = provider;
    if (Owner)
        if (auto* mesh = Owner->GetComponent<Mesh>())
            mesh->PrepareGraphicsResources(provider);
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
        m_bakeStatus = SaveCache(Mesh::ResolveFilePath(bakedAssetPath), 0, true)
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
            float& target = heights[size_t(row) * (cellsX + 1) + col];
            if (target == grid.heights[source]) continue;
            target = grid.heights[source];
            minCol = std::min(minCol, col); maxCol = std::max(maxCol, col);
            minRow = std::min(minRow, row); maxRow = std::max(maxRow, row);
        }
    }
    if (maxCol < 0) return false;
    RebuildRegion(minCol, minRow, maxCol, maxRow);
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
    double offsetX, double offsetZ)
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
    bool changed = false;
    for (int row = minRow; row <= maxRow; ++row)
        for (int col = minCol; col <= maxCol; ++col)
        {
            const float amount = std::clamp(strength * mask.Weight(offsetX + col * stepX,
                offsetZ + row * stepZ), 0.f, 1.f);
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
        RebuildRegion(int(std::floor(float(minCol) / paintResolution)),
            int(std::floor(float(minRow) / paintResolution)),
            int(std::ceil(float(maxCol) / paintResolution)),
            int(std::ceil(float(maxRow) / paintResolution)));
    }
    else
        RebuildRegion(minCol, minRow, maxCol, maxRow);
    MarkConfigurationDirty();
    return true;
}
}

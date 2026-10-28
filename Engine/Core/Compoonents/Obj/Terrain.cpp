#include "Terrain.h"
#include "Core/Object.h"
#include "Core/Scene/Scene.h"
#include "Core/Compoonents/Obj/Mesh.h"
#include "Core/Compoonents/Materials/Material.h"
#include "Core/Compoonents/Materials/Texture.h"
#include "Core/Graphics/IGraphicsProvider.h"
#include "Engine/Editor/UI/IEditorUi.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
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
Terrain::Terrain()
{
    SetTypeName(COMPONENT_TYPE_NAME(Terrain));
    singlecomponent = true;
    layers.push_back({"", 0.f, 10.f, {.35f, .55f, .25f}});
    Resize(cellsX, cellsY, cellWidth, cellDepth);
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
    Rebuild(); MarkConfigurationDirty();
}

bool Terrain::ApplyBrush(float x, float z, float radius, float strength,
    Brush brush, int layer)
{
    if (Owner && Owner->GetScene() && Owner->GetScene()->HasStarted()) return false;
    if (radius <= 0.f || strength <= 0.f || heights.empty()) return false;
    if (brush == Brush::Paint && (layer < 0 || layer >= static_cast<int>(layers.size())))
        return false;
    const std::vector<float> before = heights;
    bool changed = false;
    for (int row = 0; row <= cellsY; ++row)
        for (int col = 0; col <= cellsX; ++col)
        {
            const float dx = col * cellWidth - x, dz = row * cellDepth - z;
            const float distance = std::sqrt(dx * dx + dz * dz);
            if (distance >= radius) continue;
            const float amount = std::clamp(strength * (1.f - distance / radius), 0.f, 1.f);
            const size_t i = static_cast<size_t>(row * (cellsX + 1) + col);
            if (brush == Brush::Paint)
            {
                const float old = weights[i * layers.size() + layer];
                for (size_t l = 0; l < layers.size(); ++l)
                    weights[i * layers.size() + l] *= 1.f - amount;
                weights[i * layers.size() + layer] += amount;
                changed |= weights[i * layers.size() + layer] != old;
            }
            else
            {
                float target = before[i];
                if (brush == Brush::Raise) target += amount;
                if (brush == Brush::Lower) target -= amount;
                if (brush == Brush::Average || brush == Brush::Blend)
                {
                    float sum = 0.f; int count = 0;
                    for (int oy = -1; oy <= 1; ++oy)
                        for (int ox = -1; ox <= 1; ++ox)
                        {
                            const int nx = col + ox, ny = row + oy;
                            if (nx >= 0 && nx <= cellsX && ny >= 0 && ny <= cellsY)
                            { sum += before[ny * (cellsX + 1) + nx]; ++count; }
                        }
                    const float average = sum / count;
                    target = brush == Brush::Average ? average :
                        before[i] + (average - before[i]) * amount;
                    if (brush == Brush::Average)
                        target = before[i] + (target - before[i]) * amount;
                }
                changed |= target != heights[i]; heights[i] = target;
            }
        }
    if (changed) { NormalizeWeights(); Rebuild(); MarkConfigurationDirty(); }
    return changed;
}

bool Terrain::ApplyBrushFromLocalRay(const glm::vec3& nearPoint,
    const glm::vec3& farPoint, float radius, float strength,
    Brush brush, int layer)
{
    const glm::vec3 direction = farPoint - nearPoint;
    if (std::abs(direction.y) <= .00001f) return false;
    const float t = -nearPoint.y / direction.y;
    if (t < 0.f || t > 1.f) return false;
    const glm::vec3 hit = nearPoint + direction * t;
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
    RefreshMaterials();
    bool textured[6]{};
    for (size_t layer = 0; layer < std::min<size_t>(layers.size(), 6); ++layer)
        textured[layer] = layer < m_materials.size() && m_materials[layer] &&
            m_materials[layer]->baseColorTexture &&
            std::filesystem::exists(m_materials[layer]->baseColorTexture->GetFilePath());
    Mesh* mesh = Owner->GetComponent<Mesh>();
    if (!mesh) mesh = Owner->AddComponent<Mesh>();
    std::vector<Mesh::Vertex> vertices(heights.size());
    std::vector<uint32_t> indices;
    indices.reserve(static_cast<size_t>(cellsX) * cellsY * 6);
    for (int row = 0; row <= cellsY; ++row)
        for (int col = 0; col <= cellsX; ++col)
        {
            const size_t i = static_cast<size_t>(row * (cellsX + 1) + col);
            auto sample = [&](int sx, int sy) {
                return heights[static_cast<size_t>(std::clamp(sy, 0, cellsY) *
                    (cellsX + 1) + std::clamp(sx, 0, cellsX))];
            };
            const glm::vec3 normal = glm::normalize(glm::vec3(
                (sample(col - 1, row) - sample(col + 1, row)) / (2.f * cellWidth),
                1.f, (sample(col, row - 1) - sample(col, row + 1)) / (2.f * cellDepth)));
            auto& v = vertices[i];
            v.pos[0] = col * cellWidth; v.pos[1] = heights[i]; v.pos[2] = row * cellDepth;
            v.normal[0] = normal.x; v.normal[1] = normal.y; v.normal[2] = normal.z;
            v.uv[0] = static_cast<float>(col) / cellsX;
            v.uv[1] = static_cast<float>(row) / cellsY;
            glm::vec3 color(0.f);
            for (size_t l = 0; l < layers.size(); ++l)
            {
                const float weight = weights[i * layers.size() + l];
                if (l < 6)
                    v.uvChannels[2 + l / 4][l % 4] = weight;
                if (l >= 6 || !textured[l])
                    color += (l < m_materials.size() && m_materials[l]
                    ? m_materials[l]->diffuseColor : layers[l].previewColor) * weight;
            }
            v.color[0] = color.x; v.color[1] = color.y; v.color[2] = color.z;
            v.color[3] = 1.f;
        }
    for (int row = 0; row < cellsY; ++row)
        for (int col = 0; col < cellsX; ++col)
        {
            const uint32_t a = row * (cellsX + 1) + col, b = a + cellsX + 1;
            indices.insert(indices.end(), {a, a + 1, b, a + 1, b + 1, b});
        }
    ReduceMeshForLod(vertices, indices, cellsX, cellsY, m_meshLodStride);
    mesh->SetIndexedGeometry(std::move(vertices), std::move(indices));
    if (m_provider && !mesh->IsReady()) mesh->OnAfterDeserialize(m_provider);
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
        vertex.pos[0] = col * cellWidth;
        vertex.pos[1] = heights[source];
        vertex.pos[2] = row * cellDepth;
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
    return mesh->SetIndexedGeometry(std::move(vertices), lod.indices);
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
    result.layers = std::move(layers);
    result.materials = std::move(m_materials);
    result.materialPaths = std::move(m_materialPaths);
    result.bakedAppearance = std::move(m_bakedAppearance);
    result.bakedLods = std::move(m_bakedLods);
    result.bakedTextureMask = m_bakedTextureMask;
    if (const auto* mesh = Owner ? Owner->GetComponent<Mesh>() : nullptr)
    {
        result.vertices = mesh->GetVertices();
        result.indices = mesh->GetIndices();
    }
    return result;
}

bool Terrain::InstallPrepared(Prepared prepared, IGraphicsProvider* provider)
{
    if (!Owner || prepared.vertices.empty() || prepared.indices.empty()) return false;
    cellsX = prepared.cellsX; cellsY = prepared.cellsY;
    cellWidth = prepared.cellWidth; cellDepth = prepared.cellDepth;
    textureTiling = prepared.textureTiling;
    m_meshLodStride = prepared.lodStride;
    heights = std::move(prepared.heights);
    weights = std::move(prepared.weights);
    layers = std::move(prepared.layers);
    m_materials = std::move(prepared.materials);
    m_materialPaths = std::move(prepared.materialPaths);
    m_bakedAppearance = std::move(prepared.bakedAppearance);
    m_bakedLods = std::move(prepared.bakedLods);
    m_bakedTextureMask = prepared.bakedTextureMask;
    m_provider = provider;
    for (auto& material : m_materials)
        if (material && provider) material->PrepareTextures(provider);
    Mesh* mesh = Owner->GetComponent<Mesh>();
    if (!mesh) mesh = Owner->AddComponent<Mesh>();
    if (!mesh->SetIndexedGeometry(std::move(prepared.vertices),
        std::move(prepared.indices))) return false;
    if (provider && !mesh->IsReady()) mesh->OnAfterDeserialize(provider);
    return true;
}

void Terrain::FinalizePreparedGraphics(IGraphicsProvider* provider)
{
    m_provider = provider;
    for (auto& material : m_materials)
        if (material && provider) material->PrepareTextures(provider);
    if (auto* mesh = Owner ? Owner->GetComponent<Mesh>() : nullptr)
        if (provider && !mesh->IsReady()) mesh->OnAfterDeserialize(provider);
}

bool Terrain::SaveCache(const std::string& path, uint64_t signature,
    bool includeBakedLods) const
{
    const auto* mesh = Owner ? Owner->GetComponent<Mesh>() : nullptr;
    if (!mesh || mesh->GetVertices().size() != heights.size() ||
        mesh->GetIndices().size() != static_cast<size_t>(cellsX) * cellsY * 6 ||
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
        const auto& vertex = mesh->GetVertices()[i];
        std::copy_n(vertex.normal, 3, appearance[i].normal);
        std::copy_n(vertex.color, 3, appearance[i].color);
    }
    valid &= WriteVector(stream, heights) && WriteVector(stream, weights) &&
        WriteVector(stream, appearance);
    if (includeBakedLods)
        for (int stride : {1, 2, 4, 8})
        {
            auto vertices = mesh->GetVertices();
            auto indices = mesh->GetIndices();
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
            indices.insert(indices.end(), {a, a + 1, b, a + 1, b + 1, b});
        }
    ReduceMeshForLod(vertices, indices, x, y, m_meshLodStride);
    Mesh* mesh = Owner->GetComponent<Mesh>();
    if (!mesh) mesh = Owner->AddComponent<Mesh>();
    if (!mesh->SetIndexedGeometry(std::move(vertices), std::move(indices)))
        return false;
    cellsX = x; cellsY = y; cellWidth = width; cellDepth = depth;
    textureTiling = tiling;
    heights = std::move(cachedHeights);
    weights = std::move(cachedWeights);
    layers = std::move(cachedLayers);
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
        if (material->LoadFromFile(Mesh::ResolveFilePath(layers[i].material)))
        {
            if (m_provider) material->PrepareTextures(m_provider);
            m_materials[i] = std::move(material);
        }
    }
}

Terrain::JsonValue Terrain::Serialize() const
{
    JsonValue out = Component::Serialize();
    out.Set("cellsX", JsonValue(cellsX)).Set("cellsY", JsonValue(cellsY))
        .Set("cellWidth", JsonValue(cellWidth)).Set("cellDepth", JsonValue(cellDepth))
        .Set("textureTiling", JsonValue(textureTiling))
        .Set("bakedAssetPath", JsonValue(bakedAssetPath));
    JsonValue h = JsonValue::MakeArray(), w = JsonValue::MakeArray(), ls = JsonValue::MakeArray();
    for (float value : heights) h.Push(JsonValue(value));
    for (float value : weights) w.Push(JsonValue(value));
    for (const Layer& layer : layers)
        ls.Push(JsonValue::MakeObject().Set("material", JsonValue(layer.material))
            .Set("minHeight", JsonValue(layer.minHeight))
            .Set("maxHeight", JsonValue(layer.maxHeight))
            .Set("color", JsonValue::MakeArray().Push(JsonValue(layer.previewColor.x))
                .Push(JsonValue(layer.previewColor.y)).Push(JsonValue(layer.previewColor.z))));
    return out.Set("heights", std::move(h)).Set("weights", std::move(w))
        .Set("layers", std::move(ls));
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
    heights.assign(static_cast<size_t>(cellsX + 1) * (cellsY + 1), 0.f);
    for (size_t i = 0; i < std::min(heights.size(), value["heights"].ArraySize()); ++i)
        heights[i] = value["heights"].ArrayAt(i).AsFloat();
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
    NormalizeWeights(); Rebuild();
}

void Terrain::OnAfterDeserialize(IGraphicsProvider* provider)
{
    m_provider = provider;
    for (auto& material : m_materials)
        if (material) material->PrepareTextures(provider);
    Rebuild();
}

void Terrain::Start()
{
    if (!bakedAssetPath.empty())
        LoadCache(Mesh::ResolveFilePath(bakedAssetPath), 0, m_provider);
}

bool Terrain::DrawProperties(::Engine::Editor::IEditorUi& ui)
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

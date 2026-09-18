#include "AssetPreviewCache.h"

#include "Core/Compoonents/Materials/Material.h"
#include "Core/Compoonents/Materials/Texture.h"
#include "Core/Compoonents/Obj/Mesh.h"
#include "Core/Compoonents/Camera/Camera.h"
#include "Core/Graphics/IGraphicsProvider.h"
#include "Core/Graphics/IGraphicsTexture.h"
#include "Core/Memory/CacheStore.h"
#include "Core/Object.h"
#include "Core/Scene/Scene.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <regex>
#include <vector>
#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

namespace Engine::Editor
{
namespace
{
constexpr uint32_t PreviewSize = 128;
constexpr float Pi = 3.14159265358979323846f;
constexpr const char* PreviewCacheDomain = "Editor.AssetPreview";
constexpr uint32_t ScenePreviewMagic = 0x56525053; // "SPRV"

std::filesystem::path ScenePreviewPath(const std::string& scenePath)
{
    const std::filesystem::path scene(scenePath);
    return scene.parent_path() /
        ("." + scene.filename().string() + ".preview");
}

std::string Extension(const std::string& path)
{
    std::string extension = std::filesystem::path(path).extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
        [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    return extension;
}

float SrgbToLinear(uint8_t value)
{
    const float color = value / 255.f;
    return color <= 0.04045f
        ? color / 12.92f
        : std::pow((color + 0.055f) / 1.055f, 2.4f);
}

uint8_t LinearToSrgb(float value)
{
    value = std::clamp(value, 0.f, 1.f);
    const float encoded = value <= 0.0031308f
        ? value * 12.92f
        : 1.055f * std::pow(value, 1.f / 2.4f) - 0.055f;
    return static_cast<uint8_t>(std::clamp(encoded * 255.f + 0.5f, 0.f, 255.f));
}

glm::vec3 ToneMap(glm::vec3 color)
{
    color = glm::max(color, glm::vec3(0.f));
    return color / (glm::vec3(1.f) + color);
}

void StorePixel(std::vector<uint8_t>& pixels, uint32_t x, uint32_t y,
    glm::vec3 color, uint8_t alpha = 255)
{
    color = ToneMap(color);
    const size_t index = (static_cast<size_t>(y) * PreviewSize + x) * 4;
    pixels[index] = LinearToSrgb(color.r);
    pixels[index + 1] = LinearToSrgb(color.g);
    pixels[index + 2] = LinearToSrgb(color.b);
    pixels[index + 3] = alpha;
}

glm::vec3 SampleEnvironment(const Engine::Components::Texture& texture,
    glm::vec3 direction)
{
    direction = glm::normalize(direction);
    float u = 0.5f + std::atan2(direction.z, direction.x) / (2.f * Pi);
    const float v = std::acos(std::clamp(direction.y, -1.f, 1.f)) / Pi;
    u -= std::floor(u);
    const uint32_t x = std::min(texture.GetWidth() - 1,
        static_cast<uint32_t>(u * texture.GetWidth()));
    const uint32_t y = std::min(texture.GetHeight() - 1,
        static_cast<uint32_t>(v * texture.GetHeight()));
    const size_t pixel = static_cast<size_t>(y) * texture.GetWidth() + x;
    const auto& pixels = texture.GetPixels();
    if (texture.GetFormat() == Engine::Graphics::GraphicsTextureFormat::Rgba32Float)
    {
        glm::vec3 value{};
        std::memcpy(&value.r, pixels.data() + pixel * 16, sizeof(float));
        std::memcpy(&value.g, pixels.data() + pixel * 16 + 4, sizeof(float));
        std::memcpy(&value.b, pixels.data() + pixel * 16 + 8, sizeof(float));
        return glm::max(value, glm::vec3(0.f));
    }
    const uint8_t* source = pixels.data() + pixel * 4;
    if (texture.IsSrgb())
        return { SrgbToLinear(source[0]), SrgbToLinear(source[1]),
            SrgbToLinear(source[2]) };
    return glm::vec3(source[0], source[1], source[2]) / 255.f;
}

std::vector<uint8_t> MakeEnvironmentPreview(
    const Engine::Components::Texture& environment)
{
    std::vector<uint8_t> pixels(PreviewSize * PreviewSize * 4, 0);
    const glm::vec3 view(0.f, 0.f, 1.f);
    for (uint32_t y = 0; y < PreviewSize; ++y)
    for (uint32_t x = 0; x < PreviewSize; ++x)
    {
        const float nx = ((x + 0.5f) / PreviewSize) * 2.f - 1.f;
        const float ny = 1.f - ((y + 0.5f) / PreviewSize) * 2.f;
        const float radiusSquared = nx * nx + ny * ny;
        if (radiusSquared > 1.f)
            continue;
        const glm::vec3 normal(nx, ny, std::sqrt(1.f - radiusSquared));
        const glm::vec3 reflection = 2.f * glm::dot(normal, view) * normal - view;
        StorePixel(pixels, x, y, SampleEnvironment(environment, reflection));
    }
    return pixels;
}

std::vector<uint8_t> MakeMaterialPreview(const Engine::Components::Material& material)
{
    std::vector<uint8_t> pixels(PreviewSize * PreviewSize * 4, 0);
    const glm::vec3 view(0.f, 0.f, 1.f);
    const glm::vec3 key = glm::normalize(glm::vec3(-0.45f, 0.7f, 0.6f));
    const glm::vec3 rim = glm::normalize(glm::vec3(0.65f, 0.25f, 0.4f));
    const glm::vec3 base = glm::max(material.diffuseColor, glm::vec3(0.f));
    const float metallic = std::clamp(material.metallicFactor, 0.f, 1.f);
    const float roughness = std::clamp(material.roughnessFactor, 0.045f, 1.f);
    const glm::vec3 f0 = glm::mix(glm::vec3(0.04f), base, metallic);
    for (uint32_t y = 0; y < PreviewSize; ++y)
    for (uint32_t x = 0; x < PreviewSize; ++x)
    {
        const float nx = ((x + 0.5f) / PreviewSize) * 2.f - 1.f;
        const float ny = 1.f - ((y + 0.5f) / PreviewSize) * 2.f;
        const float radiusSquared = nx * nx + ny * ny;
        if (radiusSquared > 1.f)
            continue;
        const glm::vec3 normal(nx, ny, std::sqrt(1.f - radiusSquared));
        const float keyLight = std::max(glm::dot(normal, key), 0.f);
        const float rimLight = std::max(glm::dot(normal, rim), 0.f);
        const glm::vec3 halfVector = glm::normalize(key + view);
        const float specularPower = glm::mix(192.f, 3.f, roughness);
        const float specular = std::pow(std::max(glm::dot(normal, halfVector), 0.f),
            specularPower) * glm::mix(1.25f, 0.18f, roughness);
        const glm::vec3 reflection = 2.f * glm::dot(normal, view) * normal - view;
        const float skyBlend = std::clamp(reflection.y * 0.5f + 0.5f, 0.f, 1.f);
        const glm::vec3 studioEnvironment = glm::mix(
            glm::vec3(0.035f, 0.045f, 0.07f),
            glm::vec3(0.8f, 0.9f, 1.15f), skyBlend);
        const glm::vec3 diffuse = base * (1.f - metallic) *
            (0.08f + keyLight * 0.9f + rimLight * 0.18f);
        const glm::vec3 reflected = studioEnvironment * f0 *
            glm::mix(1.f, 0.25f, roughness) * material.reflectionStrength;
        StorePixel(pixels, x, y,
            diffuse + reflected + f0 * specular + material.emissiveColor);
    }
    return pixels;
}

glm::vec3 PreviewRotation(glm::vec3 value)
{
    constexpr float yaw = -0.65f;
    constexpr float pitch = 0.45f;
    const glm::mat3 yawRotation(
        std::cos(yaw), 0.f, -std::sin(yaw),
        0.f, 1.f, 0.f,
        std::sin(yaw), 0.f, std::cos(yaw));
    const glm::mat3 pitchRotation(
        1.f, 0.f, 0.f,
        0.f, std::cos(pitch), std::sin(pitch),
        0.f, -std::sin(pitch), std::cos(pitch));
    return pitchRotation * yawRotation * value;
}

std::vector<Engine::Model::AnimationVertex> LoadModelVertices(const std::string& path)
{
    Assimp::Importer importer;
    const aiScene* scene = importer.ReadFile(path,
        aiProcess_Triangulate | aiProcess_PreTransformVertices |
        aiProcess_JoinIdenticalVertices | aiProcess_GenSmoothNormals);
    if (!scene)
        return {};

    std::vector<Engine::Model::AnimationVertex> vertices;
    constexpr size_t triangleLimit = 100000;
    vertices.reserve(std::min<size_t>(triangleLimit * 3, 65536));
    for (unsigned meshIndex = 0; meshIndex < scene->mNumMeshes &&
        vertices.size() < triangleLimit * 3; ++meshIndex)
    {
        const aiMesh* mesh = scene->mMeshes[meshIndex];
        for (unsigned faceIndex = 0; faceIndex < mesh->mNumFaces &&
            vertices.size() < triangleLimit * 3; ++faceIndex)
        {
            const aiFace& face = mesh->mFaces[faceIndex];
            if (face.mNumIndices != 3)
                continue;
            for (unsigned corner = 0; corner < 3; ++corner)
            {
                const unsigned index = face.mIndices[corner];
                Engine::Model::AnimationVertex vertex{};
                const aiVector3D& position = mesh->mVertices[index];
                vertex.pos[0] = position.x;
                vertex.pos[1] = position.y;
                vertex.pos[2] = position.z;
                if (mesh->HasNormals())
                {
                    const aiVector3D& normal = mesh->mNormals[index];
                    vertex.normal[0] = normal.x;
                    vertex.normal[1] = normal.y;
                    vertex.normal[2] = normal.z;
                }
                vertices.push_back(vertex);
            }
        }
    }
    return vertices;
}

std::string FirstPrefabMesh(const std::string& prefabPath)
{
    std::ifstream input(prefabPath);
    if (!input)
        return {};
    const std::string contents((std::istreambuf_iterator<char>(input)), {});
    static const std::regex meshPattern(
        "\\\"file\\\"\\s*:\\s*\\\"([^\\\"]+\\.(?:mesh|obj))\\\"",
        std::regex::icase);
    std::smatch match;
    if (!std::regex_search(contents, match, meshPattern) || match.size() < 2)
        return {};
    std::string result = match[1].str();
    for (size_t position = 0;
        (position = result.find("\\\\", position)) != std::string::npos;)
        result.replace(position, 2, "\\");
    if (std::filesystem::exists(result))
        return result;
    const std::filesystem::path adjacent =
        std::filesystem::path(prefabPath).parent_path() / result;
    return std::filesystem::exists(adjacent) ? adjacent.string() : result;
}

std::vector<uint8_t> MakeMeshPreview(
    const std::vector<Engine::Model::AnimationVertex>& vertices)
{
    if (vertices.size() < 3)
        return {};
    glm::vec3 minimum(std::numeric_limits<float>::max());
    glm::vec3 maximum(std::numeric_limits<float>::lowest());
    for (const auto& vertex : vertices)
    {
        const glm::vec3 position(vertex.pos[0], vertex.pos[1], vertex.pos[2]);
        minimum = glm::min(minimum, position);
        maximum = glm::max(maximum, position);
    }
    const glm::vec3 center = (minimum + maximum) * 0.5f;
    const float extent = std::max({ maximum.x - minimum.x,
        maximum.y - minimum.y, maximum.z - minimum.z, 0.0001f });
    std::vector<uint8_t> pixels(PreviewSize * PreviewSize * 4, 0);
    std::vector<float> depth(PreviewSize * PreviewSize,
        std::numeric_limits<float>::max());
    const glm::vec3 light = glm::normalize(glm::vec3(-0.35f, 0.8f, 0.55f));

    struct Projected { float x, y, z; glm::vec3 position; };
    const auto project = [&](const Engine::Model::AnimationVertex& vertex)
    {
        const glm::vec3 rotated = PreviewRotation(
            (glm::vec3(vertex.pos[0], vertex.pos[1], vertex.pos[2]) - center) /
            extent);
        return Projected{
            (rotated.x * 0.82f + 0.5f) * PreviewSize,
            (0.5f - rotated.y * 0.82f) * PreviewSize,
            rotated.z, rotated };
    };
    for (size_t triangle = 0; triangle + 2 < vertices.size(); triangle += 3)
    {
        const Projected a = project(vertices[triangle]);
        const Projected b = project(vertices[triangle + 1]);
        const Projected c = project(vertices[triangle + 2]);
        const float area = (b.x - a.x) * (c.y - a.y) -
            (b.y - a.y) * (c.x - a.x);
        if (std::abs(area) < 0.0001f)
            continue;
        const int x0 = std::clamp(static_cast<int>(std::floor(
            std::min({ a.x, b.x, c.x }))), 0, static_cast<int>(PreviewSize) - 1);
        const int x1 = std::clamp(static_cast<int>(std::ceil(
            std::max({ a.x, b.x, c.x }))), 0, static_cast<int>(PreviewSize) - 1);
        const int y0 = std::clamp(static_cast<int>(std::floor(
            std::min({ a.y, b.y, c.y }))), 0, static_cast<int>(PreviewSize) - 1);
        const int y1 = std::clamp(static_cast<int>(std::ceil(
            std::max({ a.y, b.y, c.y }))), 0, static_cast<int>(PreviewSize) - 1);
        glm::vec3 normal = glm::cross(b.position - a.position,
            c.position - a.position);
        if (glm::dot(normal, normal) < 0.000001f)
            continue;
        normal = glm::normalize(normal);
        const float lighting = 0.22f + 0.78f * std::abs(glm::dot(normal, light));
        const glm::vec3 color = glm::mix(glm::vec3(0.18f, 0.35f, 0.58f),
            glm::vec3(0.65f, 0.82f, 1.f), lighting) * lighting;
        for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x)
        {
            const float px = x + 0.5f;
            const float py = y + 0.5f;
            const float first = ((b.x - px) * (c.y - py) -
                (b.y - py) * (c.x - px)) / area;
            const float second = ((c.x - px) * (a.y - py) -
                (c.y - py) * (a.x - px)) / area;
            const float third = 1.f - first - second;
            if (first < 0.f || second < 0.f || third < 0.f)
                continue;
            const float z = first * a.z + second * b.z + third * c.z;
            const size_t index = static_cast<size_t>(y) * PreviewSize + x;
            if (z >= depth[index])
                continue;
            depth[index] = z;
            StorePixel(pixels, static_cast<uint32_t>(x),
                static_cast<uint32_t>(y), color);
        }
    }
    return pixels;
}

std::vector<uint8_t> MakeScenePreview(Engine::Scene::Scene& scene)
{
    Engine::Components::Camera* camera = scene.FindGameCamera();
    if (!camera)
        return {};

    std::vector<uint8_t> pixels(PreviewSize * PreviewSize * 4, 255);
    for (uint32_t y = 0; y < PreviewSize; ++y)
    for (uint32_t x = 0; x < PreviewSize; ++x)
    {
        const float blend = static_cast<float>(y) / PreviewSize;
        const size_t index = (static_cast<size_t>(y) * PreviewSize + x) * 4;
        pixels[index] = static_cast<uint8_t>(25.f + blend * 12.f);
        pixels[index + 1] = static_cast<uint8_t>(31.f + blend * 14.f);
        pixels[index + 2] = static_cast<uint8_t>(43.f + blend * 17.f);
    }
    std::vector<float> depth(PreviewSize * PreviewSize,
        std::numeric_limits<float>::max());
    const glm::mat4 viewProjection = camera->GetProjectionMatrix(1.f) *
        camera->GetViewMatrix();
    const glm::vec3 light = glm::normalize(glm::vec3(-0.4f, 0.75f, -0.5f));
    size_t triangleCount = 0;
    constexpr size_t triangleLimit = 150000;

    struct Projected { float x, y, z; glm::vec3 world; };
    for (const auto& objectOwner : scene.GetObjects())
    {
        Engine::Core::Object* object = objectOwner.get();
        if (!object || !object->IsEnabledInHierarchy())
            continue;
        const auto* mesh = object->GetComponent<Engine::Components::Mesh>();
        if (!mesh || mesh->GetVertices().size() < 3)
            continue;
        const auto* material =
            object->GetComponent<Engine::Components::Material>();
        const glm::vec3 baseColor = material
            ? glm::max(material->diffuseColor, glm::vec3(0.f))
            : glm::vec3(0.45f, 0.65f, 0.9f);
        const glm::mat4 world = object->transform.GetWorldMatrixWithLayer();
        const auto project = [&](const Engine::Model::AnimationVertex& vertex,
            Projected& result)
        {
            const glm::vec4 worldPosition = world * glm::vec4(
                vertex.pos[0], vertex.pos[1], vertex.pos[2], 1.f);
            const glm::vec4 clip = viewProjection * worldPosition;
            if (clip.w <= 0.0001f)
                return false;
            const glm::vec3 ndc = glm::vec3(clip) / clip.w;
            result = {
                (ndc.x * 0.5f + 0.5f) * PreviewSize,
                (0.5f - ndc.y * 0.5f) * PreviewSize,
                ndc.z, glm::vec3(worldPosition) };
            return true;
        };

        const auto& vertices = mesh->GetVertices();
        for (size_t triangle = 0; triangle + 2 < vertices.size() &&
            triangleCount < triangleLimit; triangle += 3, ++triangleCount)
        {
            Projected a{}, b{}, c{};
            if (!project(vertices[triangle], a) ||
                !project(vertices[triangle + 1], b) ||
                !project(vertices[triangle + 2], c))
                continue;
            if ((a.z < 0.f && b.z < 0.f && c.z < 0.f) ||
                (a.z > 1.f && b.z > 1.f && c.z > 1.f))
                continue;
            const float area = (b.x - a.x) * (c.y - a.y) -
                (b.y - a.y) * (c.x - a.x);
            if (std::abs(area) < 0.0001f)
                continue;
            const int x0 = std::clamp(static_cast<int>(std::floor(
                std::min({ a.x, b.x, c.x }))), 0,
                static_cast<int>(PreviewSize) - 1);
            const int x1 = std::clamp(static_cast<int>(std::ceil(
                std::max({ a.x, b.x, c.x }))), 0,
                static_cast<int>(PreviewSize) - 1);
            const int y0 = std::clamp(static_cast<int>(std::floor(
                std::min({ a.y, b.y, c.y }))), 0,
                static_cast<int>(PreviewSize) - 1);
            const int y1 = std::clamp(static_cast<int>(std::ceil(
                std::max({ a.y, b.y, c.y }))), 0,
                static_cast<int>(PreviewSize) - 1);
            glm::vec3 normal = glm::cross(b.world - a.world, c.world - a.world);
            if (glm::dot(normal, normal) < 0.000001f)
                continue;
            normal = glm::normalize(normal);
            const float lighting = 0.25f + 0.75f *
                std::abs(glm::dot(normal, light));
            const glm::vec3 color = baseColor * lighting;
            for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x)
            {
                const float px = x + 0.5f;
                const float py = y + 0.5f;
                const float first = ((b.x - px) * (c.y - py) -
                    (b.y - py) * (c.x - px)) / area;
                const float second = ((c.x - px) * (a.y - py) -
                    (c.y - py) * (a.x - px)) / area;
                const float third = 1.f - first - second;
                if (first < 0.f || second < 0.f || third < 0.f)
                    continue;
                const float z = first * a.z + second * b.z + third * c.z;
                const size_t index = static_cast<size_t>(y) * PreviewSize + x;
                if (z < 0.f || z > 1.f || z >= depth[index])
                    continue;
                depth[index] = z;
                StorePixel(pixels, static_cast<uint32_t>(x),
                    static_cast<uint32_t>(y), color);
            }
        }
        if (triangleCount >= triangleLimit)
            break;
    }
    return pixels;
}

bool WriteScenePreview(const std::string& path,
    const std::vector<uint8_t>& pixels)
{
    if (pixels.size() != PreviewSize * PreviewSize * 4)
        return false;
    std::ofstream output(ScenePreviewPath(path), std::ios::binary | std::ios::trunc);
    if (!output)
        return false;
    const uint32_t dimensions[3] = { ScenePreviewMagic, PreviewSize, PreviewSize };
    output.write(reinterpret_cast<const char*>(dimensions), sizeof(dimensions));
    output.write(reinterpret_cast<const char*>(pixels.data()),
        static_cast<std::streamsize>(pixels.size()));
    return output.good();
}

std::vector<uint8_t> ReadScenePreview(const std::string& path)
{
    std::ifstream input(ScenePreviewPath(path), std::ios::binary);
    uint32_t dimensions[3]{};
    input.read(reinterpret_cast<char*>(dimensions), sizeof(dimensions));
    if (!input || dimensions[0] != ScenePreviewMagic ||
        dimensions[1] != PreviewSize || dimensions[2] != PreviewSize)
        return {};
    std::vector<uint8_t> pixels(PreviewSize * PreviewSize * 4);
    input.read(reinterpret_cast<char*>(pixels.data()),
        static_cast<std::streamsize>(pixels.size()));
    return input ? pixels : std::vector<uint8_t>{};
}

std::shared_ptr<Engine::Graphics::IGraphicsTexture> Upload(
    Engine::Graphics::IGraphicsProvider* provider, const std::vector<uint8_t>& pixels)
{
    if (!provider || !provider->GetTextureFactory())
        return nullptr;
    return provider->GetTextureFactory()->CreateTexture2D(
        PreviewSize, PreviewSize, pixels.data(), 1,
        Engine::Graphics::GraphicsTextureFormat::Rgba8, true);
}
}

bool AssetPreviewCache::Supports(const std::string& path)
{
    const std::string extension = Extension(path);
    static constexpr std::array<const char*, 16> supported{
        ".material", ".mat", ".hdr", ".exr", ".png", ".jpg", ".jpeg",
        ".bmp", ".dds", ".tga", ".ktx2", ".obj", ".fbx", ".mesh",
        ".prefab", ".scene" };
    return std::find(supported.begin(), supported.end(), extension) != supported.end();
}

bool AssetPreviewCache::IsCircularPreview(const std::string& path)
{
    const std::string extension = Extension(path);
    return extension == ".material" || extension == ".mat" ||
        extension == ".hdr" || extension == ".exr";
}

void* AssetPreviewCache::Get(const std::string& path,
    Engine::Graphics::IGraphicsProvider* graphicsProvider)
{
    if (!graphicsProvider || !Supports(path))
        return nullptr;
    std::error_code error;
    const auto writeTime = std::filesystem::last_write_time(path, error);
    if (error)
        return nullptr;
    Engine::Memory::CacheStore& cache = Engine::Memory::CacheStore::Get();
    const std::string key = Engine::Memory::CacheStore::PathKey(path);
    std::shared_ptr<Entry> entry = cache.Find<Entry>(PreviewCacheDomain, key);
    if (entry && entry->provider == graphicsProvider &&
        entry->writeTime == writeTime)
    {
        const auto* texture = entry->generated
            ? entry->generated.get()
            : (entry->source ? entry->source->GetGraphicsTexture() : nullptr);
        return texture ? texture->GetNativeHandle() : nullptr;
    }

    cache.Erase(PreviewCacheDomain, key);
    entry = cache.GetOrCreate<Entry>(Engine::Memory::CacheLifetime::ShortTerm,
        PreviewCacheDomain, key, [&]()
        {
            auto created = std::make_shared<Entry>();
            created->writeTime = writeTime;
            created->provider = graphicsProvider;
            const std::string extension = Extension(path);
            if (extension == ".material" || extension == ".mat")
            {
                Engine::Components::Material material;
                if (material.LoadFromFile(path))
                    created->generated = Upload(graphicsProvider,
                        MakeMaterialPreview(material));
            }
            else if (extension == ".scene")
            {
                const std::vector<uint8_t> pixels = ReadScenePreview(path);
                if (!pixels.empty())
                    created->generated = Upload(graphicsProvider, pixels);
            }
            else
            {
                const bool highDynamicRange =
                    extension == ".hdr" || extension == ".exr";
                const bool model = extension == ".obj" || extension == ".fbx" ||
                    extension == ".mesh" || extension == ".prefab";
                if (model)
                {
                    std::string meshPath = extension == ".prefab"
                        ? FirstPrefabMesh(path) : path;
                    std::vector<Engine::Model::AnimationVertex> vertices;
                    const std::string meshExtension = Extension(meshPath);
                    if (meshExtension == ".mesh" || meshExtension == ".obj")
                    {
                        try
                        {
                            Engine::Components::Mesh mesh;
                            mesh.LoadFromFile(meshPath);
                            vertices = mesh.GetVertices();
                        }
                        catch (...) {}
                    }
                    else if (!meshPath.empty())
                        vertices = LoadModelVertices(meshPath);
                    const std::vector<uint8_t> pixels = MakeMeshPreview(vertices);
                    if (!pixels.empty())
                        created->generated = Upload(graphicsProvider, pixels);
                }
                else
                {
                    created->source = Engine::Components::Texture::Acquire(
                        path, !highDynamicRange);
                    if (highDynamicRange)
                    {
                        if (created->source->Load())
                            created->generated = Upload(graphicsProvider,
                                MakeEnvironmentPreview(*created->source));
                    }
                    else
                        created->source->Prepare(graphicsProvider);
                }
            }
            return created;
        }, std::chrono::seconds(30));
    const auto* texture = entry && entry->generated
        ? entry->generated.get()
        : (entry && entry->source ? entry->source->GetGraphicsTexture() : nullptr);
    return texture ? texture->GetNativeHandle() : nullptr;
}

bool AssetPreviewCache::CaptureScene(const std::string& path,
    Engine::Scene::Scene& scene,
    Engine::Graphics::IGraphicsProvider* graphicsProvider)
{
    const std::vector<uint8_t> pixels = MakeScenePreview(scene);
    if (pixels.empty())
    {
        RemovePersistentPreview(path);
        return false;
    }
    if (!WriteScenePreview(path, pixels))
        return false;
    Engine::Memory::CacheStore::Get().Erase(PreviewCacheDomain,
        Engine::Memory::CacheStore::PathKey(path));
    if (graphicsProvider)
    {
        AssetPreviewCache previews;
        previews.Get(path, graphicsProvider);
    }
    return true;
}

void AssetPreviewCache::MovePersistentPreview(const std::string& oldPath,
    const std::string& newPath)
{
    if (Extension(oldPath) != ".scene" || Extension(newPath) != ".scene")
        return;
    std::error_code error;
    const std::filesystem::path oldPreview = ScenePreviewPath(oldPath);
    const std::filesystem::path newPreview = ScenePreviewPath(newPath);
    if (std::filesystem::exists(oldPreview, error))
    {
        std::filesystem::rename(oldPreview, newPreview, error);
        if (error)
        {
            error.clear();
            std::filesystem::copy_file(oldPreview, newPreview,
                std::filesystem::copy_options::overwrite_existing, error);
            if (!error)
                std::filesystem::remove(oldPreview, error);
        }
    }
    Engine::Memory::CacheStore::Get().Erase(PreviewCacheDomain,
        Engine::Memory::CacheStore::PathKey(oldPath));
    Engine::Memory::CacheStore::Get().Erase(PreviewCacheDomain,
        Engine::Memory::CacheStore::PathKey(newPath));
}

void AssetPreviewCache::RemovePersistentPreview(const std::string& path)
{
    if (Extension(path) != ".scene")
        return;
    std::error_code error;
    std::filesystem::remove(ScenePreviewPath(path), error);
    Engine::Memory::CacheStore::Get().Erase(PreviewCacheDomain,
        Engine::Memory::CacheStore::PathKey(path));
}

void AssetPreviewCache::Invalidate(const std::string& path)
{
    Engine::Memory::CacheStore::Get().Erase(PreviewCacheDomain,
        Engine::Memory::CacheStore::PathKey(path));
}

void AssetPreviewCache::Clear()
{
    Engine::Memory::CacheStore::Get().ClearDomain(PreviewCacheDomain);
}
}

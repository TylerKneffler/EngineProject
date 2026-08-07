#include "BakedLightingPipeline.h"
#include "Core/Scene/Scene.h"
#include "Core/Compoonents/Light.h"
#include "Core/Compoonents/Material.h"
#include "Core/Compoonents/Materials/Texture.h"
#include "Core/Compoonents/Mesh.h"
#include "Core/Rendering/Lighting/BakedLightingData.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <vector>
#include <glm/gtc/matrix_inverse.hpp>

namespace Engine::Rendering::Lighting
{
namespace
{
struct BakeLight
{
    glm::vec3 position{};
    glm::vec3 color{};
    float intensity = 0.f;
    float range = 0.f;
    float falloff = 2.f;
    Light::Type type = Light::Type::Point;
    glm::vec3 direction{ 0.f, 1.f, 0.f };
};

struct SurfaceTriangle
{
    glm::vec3 positions[3]{};
    glm::vec3 normals[3]{};
    glm::vec2 uvs[3]{};
    const Object* owner = nullptr;
};

struct Image
{
    explicit Image(uint32_t resolution)
        : width(resolution), height(resolution),
          pixels(static_cast<size_t>(resolution) * resolution),
          weights(static_cast<size_t>(resolution) * resolution) {}

    uint32_t width;
    uint32_t height;
    std::vector<glm::vec3> pixels;
    std::vector<float> weights;
    glm::vec3 irradianceSum{ 0.f };
    glm::vec3 weightedDirection{ 0.f };
    uint32_t lightingSamples = 0;

    glm::vec3 AverageIrradiance() const
    {
        return lightingSamples > 0
            ? irradianceSum / static_cast<float>(lightingSamples)
            : glm::vec3(0.f);
    }
};

std::string SafeName(std::string value)
{
    for (char& character : value)
        if (!std::isalnum(static_cast<unsigned char>(character)) &&
            character != '-' && character != '_')
            character = '_';
    while (!value.empty() && value.back() == '_')
        value.pop_back();
    return value.empty() ? "Object" : value;
}

std::string ObjectKey(const Scene& scene, const Object& object)
{
    Scene::ObjectPath path;
    scene.TryGetObjectPath(&object, path);
    std::ostringstream result;
    result << SafeName(object.name);
    for (const size_t index : path)
        result << '_' << index;
    return result.str();
}

std::string PortablePath(const std::filesystem::path& path)
{
    std::error_code error;
    const std::filesystem::path relative = std::filesystem::relative(
        std::filesystem::absolute(path, error),
        std::filesystem::current_path(), error);
    if (!error && !relative.empty() && *relative.begin() != "..")
        return relative.lexically_normal().generic_string();
    return path.lexically_normal().generic_string();
}

glm::vec3 LinearFromSrgb(const glm::vec3& value)
{
    auto convert = [](float channel)
    {
        return channel <= 0.04045f ? channel / 12.92f
            : std::pow((channel + 0.055f) / 1.055f, 2.4f);
    };
    return { convert(value.r), convert(value.g), convert(value.b) };
}

uint8_t SrgbByte(float value)
{
    value = std::max(0.f, value);
    const float encoded = value <= 0.0031308f ? value * 12.92f
        : 1.055f * std::pow(value, 1.f / 2.4f) - 0.055f;
    return static_cast<uint8_t>(std::round(
        std::clamp(encoded, 0.f, 1.f) * 255.f));
}

glm::vec3 SampleTexture(const std::shared_ptr<Texture>& texture,
    glm::vec2 uv, const glm::vec3& fallback)
{
    if (!texture || (!texture->HasPixels() && !texture->Load()) ||
        texture->GetWidth() == 0 || texture->GetHeight() == 0)
        return fallback;
    uv -= glm::floor(uv);
    const uint32_t x = std::min(texture->GetWidth() - 1,
        static_cast<uint32_t>(uv.x * texture->GetWidth()));
    const uint32_t y = std::min(texture->GetHeight() - 1,
        static_cast<uint32_t>((1.f - uv.y) * texture->GetHeight()));
    const size_t offset = (static_cast<size_t>(y) * texture->GetWidth() + x) * 4;
    const auto& pixels = texture->GetPixels();
    return LinearFromSrgb(glm::vec3(
        pixels[offset], pixels[offset + 1], pixels[offset + 2]) / 255.f);
}

bool RayIntersects(const glm::vec3& origin, const glm::vec3& direction,
    float maximumDistance, float shadowBias, const SurfaceTriangle& triangle)
{
    const glm::vec3 edge1 = triangle.positions[1] - triangle.positions[0];
    const glm::vec3 edge2 = triangle.positions[2] - triangle.positions[0];
    const glm::vec3 p = glm::cross(direction, edge2);
    const float determinant = glm::dot(edge1, p);
    if (std::abs(determinant) < 0.000001f)
        return false;
    const float inverse = 1.f / determinant;
    const glm::vec3 t = origin - triangle.positions[0];
    const float u = glm::dot(t, p) * inverse;
    if (u < 0.f || u > 1.f)
        return false;
    const glm::vec3 q = glm::cross(t, edge1);
    const float v = glm::dot(direction, q) * inverse;
    if (v < 0.f || u + v > 1.f)
        return false;
    const float distance = glm::dot(edge2, q) * inverse;
    return distance > shadowBias && distance < maximumDistance;
}

bool Occluded(const glm::vec3& position, const glm::vec3& normal,
    const glm::vec3& direction, float distance,
    float shadowBias, const std::vector<SurfaceTriangle>& geometry,
    size_t receiverTriangle)
{
    const glm::vec3 origin = position + normal * shadowBias;
    for (size_t index = 0; index < geometry.size(); ++index)
        if (index != receiverTriangle &&
            RayIntersects(origin, direction, distance, shadowBias, geometry[index]))
            return true;
    return false;
}

glm::vec3 EvaluateLighting(const glm::vec3& position, glm::vec3 normal,
    const std::vector<BakeLight>& lights,
    const std::vector<SurfaceTriangle>& geometry, size_t receiverTriangle,
    float shadowBias, glm::vec3* weightedDirection = nullptr)
{
    if (glm::dot(normal, normal) < 0.000001f)
        normal = { 0.f, 1.f, 0.f };
    normal = glm::normalize(normal);
    glm::vec3 result(0.f);
    for (const BakeLight& light : lights)
    {
        if (light.type == Light::Type::Ambient)
        {
            const float diffuse = std::max(0.f, glm::dot(normal, light.direction));
            if (!Occluded(position, normal, light.direction,
                    std::numeric_limits<float>::max(), shadowBias,
                    geometry, receiverTriangle))
            {
                const glm::vec3 contribution = light.color * light.intensity *
                    (0.15f + 0.85f * diffuse);
                result += contribution;
                if (weightedDirection)
                    *weightedDirection += light.direction *
                        (contribution.r + contribution.g + contribution.b);
            }
            continue;
        }

        const glm::vec3 toLight = light.position - position;
        const float distance = glm::length(toLight);
        if (distance <= shadowBias || distance >= light.range)
            continue;
        const glm::vec3 direction = toLight / distance;
        const float diffuse = std::max(0.f, glm::dot(normal, direction));
        if (diffuse <= 0.f || Occluded(position, normal, direction,
                distance - shadowBias, shadowBias, geometry, receiverTriangle))
            continue;
        const float rangeFactor = std::max(0.f, 1.f - distance / light.range);
        const glm::vec3 contribution = light.color * light.intensity * diffuse *
            std::pow(rangeFactor, std::max(light.falloff, 0.1f));
        result += contribution;
        if (weightedDirection)
            *weightedDirection += direction *
                (contribution.r + contribution.g + contribution.b);
    }
    return result;
}

float Edge(const glm::vec2& a, const glm::vec2& b, const glm::vec2& p)
{
    return (p.x - a.x) * (b.y - a.y) -
        (p.y - a.y) * (b.x - a.x);
}

void Dilate(Image& image, uint32_t passes)
{
    for (uint32_t pass = 0; pass < passes; ++pass)
    {
        const auto previousPixels = image.pixels;
        const auto previousWeights = image.weights;
        for (uint32_t y = 0; y < image.height; ++y)
        for (uint32_t x = 0; x < image.width; ++x)
        {
            const size_t destination = static_cast<size_t>(y) * image.width + x;
            if (previousWeights[destination] > 0.f)
                continue;
            glm::vec3 sum(0.f);
            float count = 0.f;
            for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx)
            {
                const int sx = static_cast<int>(x) + dx;
                const int sy = static_cast<int>(y) + dy;
                if (sx < 0 || sy < 0 || sx >= static_cast<int>(image.width) ||
                    sy >= static_cast<int>(image.height))
                    continue;
                const size_t source = static_cast<size_t>(sy) * image.width + sx;
                if (previousWeights[source] > 0.f)
                {
                    sum += previousPixels[source];
                    count += 1.f;
                }
            }
            if (count > 0.f)
            {
                image.pixels[destination] = sum / count;
                image.weights[destination] = 1.f;
            }
        }
    }
}

Image BakeImage(const Object& object, const Material& material,
    const std::vector<BakeLight>& lights,
    const std::vector<SurfaceTriangle>& geometry,
    const BakedLightingSettings& settings)
{
    Image image(settings.lightmapResolution);
    bool wroteTexel = false;
    for (size_t triangleIndex = 0; triangleIndex < geometry.size(); ++triangleIndex)
    {
        const SurfaceTriangle& triangle = geometry[triangleIndex];
        if (triangle.owner != &object)
            continue;
        glm::vec2 points[3];
        for (int vertex = 0; vertex < 3; ++vertex)
            points[vertex] = triangle.uvs[vertex] * glm::vec2(
                static_cast<float>(image.width - 1),
                static_cast<float>(image.height - 1));
        const float area = Edge(points[0], points[1], points[2]);
        if (std::abs(area) < 0.00001f)
            continue;
        const int minX = std::max(0, static_cast<int>(std::floor(std::min({
            points[0].x, points[1].x, points[2].x }))));
        const int maxX = std::min(static_cast<int>(image.width) - 1,
            static_cast<int>(std::ceil(std::max({
                points[0].x, points[1].x, points[2].x }))));
        const int minY = std::max(0, static_cast<int>(std::floor(std::min({
            points[0].y, points[1].y, points[2].y }))));
        const int maxY = std::min(static_cast<int>(image.height) - 1,
            static_cast<int>(std::ceil(std::max({
                points[0].y, points[1].y, points[2].y }))));
        for (int y = minY; y <= maxY; ++y)
        for (int x = minX; x <= maxX; ++x)
        {
            const glm::vec2 sample(x + 0.5f, y + 0.5f);
            const float w0 = Edge(points[1], points[2], sample) / area;
            const float w1 = Edge(points[2], points[0], sample) / area;
            const float w2 = 1.f - w0 - w1;
            if (w0 < -0.0001f || w1 < -0.0001f || w2 < -0.0001f)
                continue;
            const glm::vec3 position = triangle.positions[0] * w0 +
                triangle.positions[1] * w1 + triangle.positions[2] * w2;
            const glm::vec3 normal = triangle.normals[0] * w0 +
                triangle.normals[1] * w1 + triangle.normals[2] * w2;
            const glm::vec2 uv = triangle.uvs[0] * w0 +
                triangle.uvs[1] * w1 + triangle.uvs[2] * w2;
            const glm::vec3 albedo = material.diffuseColor *
                SampleTexture(material.baseColorTexture, uv, glm::vec3(1.f));
            const glm::vec3 sourceEmissive = settings.accumulate
                ? material.emissiveColor * SampleTexture(
                    material.emissiveTexture, uv, glm::vec3(1.f))
                : glm::vec3(0.f);
            glm::vec3 weightedDirection(0.f);
            const glm::vec3 lighting = EvaluateLighting(position, normal,
                lights, geometry, triangleIndex, settings.shadowBias,
                &weightedDirection);
            const glm::vec3 baked = sourceEmissive + albedo * lighting *
                (1.f - material.metallicFactor);
            const size_t index = static_cast<size_t>(y) * image.width + x;
            image.pixels[index] += baked;
            image.weights[index] += 1.f;
            image.irradianceSum += lighting;
            image.weightedDirection += weightedDirection;
            ++image.lightingSamples;
            wroteTexel = true;
        }
    }
    for (size_t index = 0; index < image.pixels.size(); ++index)
        if (image.weights[index] > 0.f)
            image.pixels[index] /= image.weights[index];

    if (!wroteTexel)
    {
        glm::vec3 sum(0.f);
        float count = 0.f;
        for (size_t triangleIndex = 0; triangleIndex < geometry.size(); ++triangleIndex)
        {
            const auto& triangle = geometry[triangleIndex];
            if (triangle.owner != &object)
                continue;
            for (int vertex = 0; vertex < 3; ++vertex)
            {
                glm::vec3 weightedDirection(0.f);
                const glm::vec3 lighting = EvaluateLighting(
                    triangle.positions[vertex], triangle.normals[vertex],
                    lights, geometry, triangleIndex, settings.shadowBias,
                    &weightedDirection);
                sum += material.diffuseColor * lighting *
                    (1.f - material.metallicFactor);
                image.irradianceSum += lighting;
                image.weightedDirection += weightedDirection;
                ++image.lightingSamples;
                count += 1.f;
            }
        }
        const glm::vec3 fallback = (settings.accumulate ? material.emissiveColor : glm::vec3(0.f)) +
            (count > 0.f ? sum / count : glm::vec3(0.f));
        std::fill(image.pixels.begin(), image.pixels.end(), fallback);
        std::fill(image.weights.begin(), image.weights.end(), 1.f);
    }
    Dilate(image, settings.dilationPasses);
    return image;
}

void WriteU16(std::ofstream& file, uint16_t value)
{
    file.put(static_cast<char>(value & 0xff));
    file.put(static_cast<char>((value >> 8) & 0xff));
}

void WriteU32(std::ofstream& file, uint32_t value)
{
    for (int shift = 0; shift < 32; shift += 8)
        file.put(static_cast<char>((value >> shift) & 0xff));
}

bool SaveBmp(const std::filesystem::path& path, const Image& image)
{
    std::ofstream file(path, std::ios::binary);
    if (!file)
        return false;
    const uint32_t pixelBytes = image.width * image.height * 4;
    file.put('B'); file.put('M');
    WriteU32(file, 54 + pixelBytes);
    WriteU32(file, 0); WriteU32(file, 54);
    WriteU32(file, 40); WriteU32(file, image.width);
    WriteU32(file, static_cast<uint32_t>(-static_cast<int32_t>(image.height)));
    WriteU16(file, 1); WriteU16(file, 32); WriteU32(file, 0);
    WriteU32(file, pixelBytes); WriteU32(file, 2835); WriteU32(file, 2835);
    WriteU32(file, 0); WriteU32(file, 0);
    for (const glm::vec3& pixel : image.pixels)
    {
        file.put(static_cast<char>(SrgbByte(pixel.b)));
        file.put(static_cast<char>(SrgbByte(pixel.g)));
        file.put(static_cast<char>(SrgbByte(pixel.r)));
        file.put(static_cast<char>(255));
    }
    return file.good();
}

std::vector<SurfaceTriangle> GatherGeometry(const Scene& scene)
{
    std::vector<SurfaceTriangle> result;
    for (const auto& object : scene.GetObjects())
    {
        if (!object->IsEnabledInHierarchy())
            continue;
        const Mesh* mesh = object->GetComponent<Mesh>();
        if (!mesh)
            continue;
        const glm::mat4 world = object->transform.GetWorldMatrix();
        const glm::mat3 normalMatrix = glm::inverseTranspose(glm::mat3(world));
        const auto& vertices = mesh->GetVertices();
        for (size_t offset = 0; offset + 2 < vertices.size(); offset += 3)
        {
            SurfaceTriangle triangle;
            triangle.owner = object.get();
            for (int index = 0; index < 3; ++index)
            {
                const Vertex& vertex = vertices[offset + index];
                triangle.positions[index] = glm::vec3(world * glm::vec4(
                    vertex.pos[0], vertex.pos[1], vertex.pos[2], 1.f));
                triangle.normals[index] = glm::normalize(normalMatrix * glm::vec3(
                    vertex.normal[0], vertex.normal[1], vertex.normal[2]));
                triangle.uvs[index] = { vertex.uv[0], vertex.uv[1] };
            }
            result.push_back(triangle);
        }
    }
    return result;
}

bool RestoreMaterial(Object& object, const BakedLightingData& data,
    IGraphicsProvider* graphicsProvider)
{
    Material* material = object.GetComponent<Material>();
    if (!material)
        material = object.AddComponent<Material>();
    bool restored = false;
    if (!data.originalMaterialAsset.empty())
        restored = material->LoadFromFile(data.originalMaterialAsset);
    if (!restored && !data.originalMaterialSnapshot.empty())
        restored = material->LoadFromFile(data.originalMaterialSnapshot);
    if (restored)
        material->PrepareTextures(graphicsProvider);
    return restored;
}
}

BakeResult BakedLightingPipeline::Bake(Scene& scene,
    const std::string& assetsDirectory, const std::string& sceneName,
    const BakedLightingSettings& requestedBakeSettings) const
{
    BakeResult result{};
    try
    {
        BakedLightingSettings settings = requestedBakeSettings;
        settings.lightmapResolution = std::clamp(
            settings.lightmapResolution, 32u, 2048u);
        settings.shadowBias = std::clamp(settings.shadowBias, 0.00001f, 0.1f);
        settings.dilationPasses = std::min(settings.dilationPasses, 32u);
        std::vector<BakeLight> lights;
        for (const auto& object : scene.GetObjects())
        {
            if (!object->IsEnabledInHierarchy())
                continue;
            const Light* light = object->GetComponent<Light>();
            if (!light || !light->baked || light->intensity <= 0.f)
                continue;
            if (light->GetLightType() == Light::Type::Point && light->range <= 0.f)
                continue;
            const glm::vec3 direction = light->GetLightType() == Light::Type::Ambient
                ? -glm::normalize(glm::vec3(object->transform.GetWorldMatrix()[2]))
                : glm::vec3(0.f, 1.f, 0.f);
            lights.push_back({ object->transform.GetWorldPosition(), light->color,
                light->intensity, light->range, light->falloff,
                light->GetLightType(), direction });
        }

        if (lights.empty())
        {
            result.message =
                "Lighting bake skipped: the scene has no enabled Baked lights.";
            return result;
        }

        // Bake outputs are generated cache data rather than user-authored
        // assets. Keep them under the hidden Assets/.temp tree so they remain
        // loadable by serialized material paths without cluttering the asset
        // explorer.
        const std::filesystem::path root = std::filesystem::path(assetsDirectory) /
            ".temp" / "Baked" / SafeName(sceneName);
        const std::filesystem::path materialDirectory = root / "Materials";
        const std::filesystem::path lightmapDirectory = root / "Lightmaps";
        std::filesystem::create_directories(materialDirectory);
        std::filesystem::create_directories(lightmapDirectory);

        // Re-baking always starts from the recorded source material, never from
        // a prior baked derivative.
        for (const auto& object : scene.GetObjects())
            if (const auto* data = object->GetComponent<BakedLightingData>())
                RestoreMaterial(*object, *data, scene.GetGraphicsProvider());

        const std::vector<SurfaceTriangle> geometry = GatherGeometry(scene);
        for (const auto& object : scene.GetObjects())
        {
            const Mesh* mesh = object->GetComponent<Mesh>();
            if (!mesh || !object->IsEnabledInHierarchy())
                continue;
            Material* source = object->GetComponent<Material>();
            if (!source)
                source = object->AddComponent<Material>();
            BakedLightingData* data = object->GetComponent<BakedLightingData>();
            const std::string previousOriginal = data
                ? data->originalMaterialAsset : source->GetFilePath();
            if (!data)
                data = object->AddComponent<BakedLightingData>();

            const std::string key = ObjectKey(scene, *object);
            const std::filesystem::path snapshotPath =
                materialDirectory / (key + ".source.material");
            const std::filesystem::path bakedPath =
                materialDirectory / (key + ".baked.material");
            const std::filesystem::path lightmapPath =
                lightmapDirectory / (key + ".lightmap.bmp");
            if (!source->SaveToFile(snapshotPath.string()))
                throw std::runtime_error("Could not save source material snapshot");

            const Image image = BakeImage(*object, *source, lights, geometry, settings);
            if (!SaveBmp(lightmapPath, image))
                throw std::runtime_error("Could not save baked lightmap");
            Texture::Invalidate(PortablePath(lightmapPath));

            Material baked;
            if (!baked.LoadFromFile(snapshotPath.string()))
                throw std::runtime_error("Could not create baked material");
            baked.emissiveColor = glm::vec3(1.f);
            baked.SetEmissiveTexture(PortablePath(lightmapPath));
            if (!baked.SaveToFile(bakedPath.string()))
                throw std::runtime_error("Could not save baked material asset");
            if (!source->LoadFromFile(PortablePath(bakedPath)))
                throw std::runtime_error("Could not assign baked material asset");
            source->PrepareTextures(scene.GetGraphicsProvider());

            data->irradiance = image.AverageIrradiance();
            data->directionalIrradiance = data->irradiance;
            // The generated texture is attached to this receiver's UVs, so
            // report its dominant direction in receiver-local space. This
            // keeps the inspector meaningful when the object is rotated.
            if (glm::dot(image.weightedDirection,
                    image.weightedDirection) > 0.000001f)
            {
                const glm::mat3 inverseWorld = glm::inverse(
                    glm::mat3(object->transform.GetWorldMatrix()));
                data->lightDirection = glm::normalize(inverseWorld *
                    glm::normalize(image.weightedDirection));
            }
            else
                data->lightDirection = glm::vec3(0.f, 1.f, 0.f);
            data->originalMaterialAsset = previousOriginal;
            data->originalMaterialSnapshot = PortablePath(snapshotPath);
            data->bakedMaterialAsset = PortablePath(bakedPath);
            data->bakedLightmapAsset = PortablePath(lightmapPath);
            data->valid = true;
            data->version = 3;
            ++result.receiverCount;
        }

        result.succeeded = true;
        result.bakedLightCount = static_cast<uint32_t>(lights.size());
        result.message = "Baked " + std::to_string(result.bakedLightCount) +
            " light(s) into " + std::to_string(result.receiverCount) +
            " generated material/lightmap pair(s) under " +
            PortablePath(root) + ".";
    }
    catch (const std::exception& error)
    {
        Clear(scene);
        result.message = std::string("Lighting bake failed: ") + error.what();
    }
    return result;
}

uint32_t BakedLightingPipeline::Clear(Scene& scene) const
{
    uint32_t cleared = 0;
    for (const auto& object : scene.GetObjects())
    {
        for (auto component = object->Components.begin();
            component != object->Components.end();)
        {
            auto* data = dynamic_cast<BakedLightingData*>(*component);
            if (!data)
            {
                ++component;
                continue;
            }
            RestoreMaterial(*object, *data, scene.GetGraphicsProvider());
            delete *component;
            component = object->Components.erase(component);
            ++cleared;
        }
    }
    return cleared;
}
}

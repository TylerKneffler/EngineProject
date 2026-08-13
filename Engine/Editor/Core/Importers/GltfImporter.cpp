#include "GltfImporter.h"

#include "Core/AssetRecord.h"
#include "Core/Compoonents/Material.h"
#include "Core/Compoonents/Mesh.h"
#include "Core/Compoonents/Animation/ModelAnimation.h"
#include "Core/Object.h"
#include "Core/Scene/Scene.h"
#include "Core/Serialization/SceneSerializer.h"

#include <fastgltf/core.hpp>
#include <fastgltf/tools.hpp>

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <optional>
#include <unordered_map>
#include <limits>
#include <vector>

namespace fs = std::filesystem;

namespace
{
std::string SafeName(std::string_view source, const std::string& fallback)
{
    std::string name(source);
    if (name.empty())
        name = fallback;
    for (char& c : name)
    {
        const unsigned char value = static_cast<unsigned char>(c);
        if (value < 32 || c == '<' || c == '>' || c == ':' || c == '"' ||
            c == '/' || c == '\\' || c == '|' || c == '?' || c == '*')
            c = '_';
    }
    while (!name.empty() && (name.back() == ' ' || name.back() == '.'))
        name.pop_back();
    return name.empty() ? fallback : name;
}

fs::path UniqueModelDirectory(const fs::path& assets, const std::string& base)
{
    fs::path candidate = assets / base;
    for (unsigned suffix = 2; fs::exists(candidate); ++suffix)
        candidate = assets / (base + " " + std::to_string(suffix));
    return candidate;
}

const char* MimeExtension(fastgltf::MimeType mime)
{
    switch (mime)
    {
    case fastgltf::MimeType::PNG: return ".png";
    case fastgltf::MimeType::JPEG: return ".jpg";
    case fastgltf::MimeType::DDS: return ".dds";
    case fastgltf::MimeType::KTX2: return ".ktx2";
    default: return ".bin";
    }
}

bool ExtractImage(
    const fastgltf::Asset& asset,
    const fastgltf::Image& image,
    const fs::path& output)
{
    std::vector<std::byte> bytes;
    bool found = false;
    std::visit(fastgltf::visitor {
        [&](const fastgltf::sources::Array& source)
        {
            bytes.assign(source.bytes.begin(), source.bytes.end());
            found = true;
        },
        [&](const fastgltf::sources::ByteView& source)
        {
            bytes.assign(source.bytes.begin(), source.bytes.end());
            found = true;
        },
        [&](const fastgltf::sources::BufferView& source)
        {
            const auto& view = asset.bufferViews[source.bufferViewIndex];
            const auto& buffer = asset.buffers[view.bufferIndex];
            std::visit(fastgltf::visitor {
                [&](const fastgltf::sources::Array& data)
                {
                    const size_t begin = view.byteOffset;
                    const size_t end = begin + view.byteLength;
                    if (end <= data.bytes.size())
                    {
                        bytes.assign(data.bytes.begin() + begin, data.bytes.begin() + end);
                        found = true;
                    }
                },
                [&](const fastgltf::sources::ByteView& data)
                {
                    const size_t begin = view.byteOffset;
                    const size_t end = begin + view.byteLength;
                    if (end <= data.bytes.size())
                    {
                        bytes.assign(data.bytes.begin() + begin, data.bytes.begin() + end);
                        found = true;
                    }
                },
                [](const auto&) {}
            }, buffer.data);
        },
        [&](const fastgltf::sources::URI& source)
        {
            if (!source.uri.isLocalPath())
                return;
            std::ifstream input(source.uri.fspath(), std::ios::binary);
            if (!input)
                return;
            const std::vector<char> raw{
                std::istreambuf_iterator<char>(input),
                std::istreambuf_iterator<char>()};
            bytes.resize(raw.size());
            std::transform(raw.begin(), raw.end(), bytes.begin(),
                [](char value) { return static_cast<std::byte>(
                    static_cast<unsigned char>(value)); });
            found = true;
        },
        [](const auto&) {}
    }, image.data);

    if (!found)
        return false;
    std::ofstream file(output, std::ios::binary);
    if (!file)
        return false;
    file.write(reinterpret_cast<const char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
    return file.good();
}

fastgltf::MimeType ImageMime(const fastgltf::Image& image)
{
    return std::visit(fastgltf::visitor {
        [](const fastgltf::sources::Array& source) { return source.mimeType; },
        [](const fastgltf::sources::Vector& source) { return source.mimeType; },
        [](const fastgltf::sources::ByteView& source) { return source.mimeType; },
        [](const fastgltf::sources::BufferView& source) { return source.mimeType; },
        [](const fastgltf::sources::URI& source) { return source.mimeType; },
        [](const auto&) { return fastgltf::MimeType::None; }
    }, image.data);
}

std::string ImageExtension(const fastgltf::Image& image)
{
    const char* mimeExtension = MimeExtension(ImageMime(image));
    if (*mimeExtension)
        return mimeExtension;
    return std::visit(fastgltf::visitor {
        [](const fastgltf::sources::URI& source)
        {
            std::string extension = source.uri.fspath().extension().string();
            std::transform(extension.begin(), extension.end(), extension.begin(),
                [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
            return extension;
        },
        [](const auto&) { return std::string{}; }
    }, image.data);
}

std::optional<size_t> TextureImageIndex(const fastgltf::Texture& texture)
{
    if (texture.imageIndex) return *texture.imageIndex;
    if (texture.basisuImageIndex) return *texture.basisuImageIndex;
    if (texture.ddsImageIndex) return *texture.ddsImageIndex;
    return std::nullopt;
}

std::vector<uint32_t> TriangleIndices(
    const fastgltf::Primitive& primitive,
    std::vector<uint32_t> source)
{
    if (primitive.type == fastgltf::PrimitiveType::Triangles)
        return source;

    std::vector<uint32_t> triangles;
    if (primitive.type == fastgltf::PrimitiveType::TriangleStrip)
    {
        for (size_t i = 2; i < source.size(); ++i)
        {
            if ((i & 1) == 0)
                triangles.insert(triangles.end(), { source[i - 2], source[i - 1], source[i] });
            else
                triangles.insert(triangles.end(), { source[i - 1], source[i - 2], source[i] });
        }
    }
    else if (primitive.type == fastgltf::PrimitiveType::TriangleFan)
    {
        for (size_t i = 2; i < source.size(); ++i)
            triangles.insert(triangles.end(), { source[0], source[i - 1], source[i] });
    }
    return triangles;
}

Object* AddChild(Scene& scene, Object* parent, const std::string& name)
{
    Object* child = scene.AddObject(name);
    child->Parent = parent;
    parent->Children.push_back(child);
    return child;
}

glm::vec3 QuaternionEuler(const glm::quat& q)
{
    const float sinX = 2.f * (q.w * q.x + q.y * q.z);
    const float cosX = 1.f - 2.f * (q.x * q.x + q.y * q.y);
    const float sinY = std::clamp(2.f * (q.w * q.y - q.z * q.x), -1.f, 1.f);
    const float sinZ = 2.f * (q.w * q.z + q.x * q.y);
    const float cosZ = 1.f - 2.f * (q.y * q.y + q.z * q.z);
    return { std::atan2(sinX, cosX), std::asin(sinY), std::atan2(sinZ, cosZ) };
}

struct ImportedPrimitiveRuntime
{
    std::vector<Mesh::MorphTarget> morphTargets;
};
}

GltfImportResult GltfImporter::Import(
    const std::string& sourcePath,
    const std::string& assetsDirectory)
{
    GltfImportResult result;
    fs::path output;
    fs::path normalizedAssets;
    const fs::path source = fs::path(sourcePath).lexically_normal();
    if (!fs::exists(source))
    {
        result.message = "glTF source file does not exist: " + source.string();
        return result;
    }

    try
    {
        const std::string sourceModelName = SafeName(source.stem().string(), "Model");
        normalizedAssets = fs::path(assetsDirectory).lexically_normal();
        output = UniqueModelDirectory(normalizedAssets, sourceModelName);
        const std::string modelName = output.filename().string();
        const fs::path meshesDirectory = output / "Meshes";
        const fs::path materialsDirectory = output / "Materials";
        const fs::path texturesDirectory = output / "Textures";
        fs::create_directories(meshesDirectory);
        fs::create_directories(materialsDirectory);
        fs::create_directories(texturesDirectory);

        constexpr fastgltf::Extensions extensions =
            fastgltf::Extensions::KHR_mesh_quantization |
            fastgltf::Extensions::KHR_texture_transform |
            fastgltf::Extensions::KHR_texture_basisu |
            fastgltf::Extensions::MSFT_texture_dds |
            fastgltf::Extensions::KHR_materials_emissive_strength |
            fastgltf::Extensions::KHR_materials_unlit;
        fastgltf::Parser parser(extensions);
        auto input = fastgltf::GltfDataBuffer::FromPath(source);
        if (input.error() != fastgltf::Error::None)
            throw std::runtime_error(std::string(fastgltf::getErrorMessage(input.error())));

        constexpr fastgltf::Options options =
            fastgltf::Options::LoadExternalBuffers |
            fastgltf::Options::LoadExternalImages |
            fastgltf::Options::GenerateMeshIndices |
            fastgltf::Options::DecomposeNodeMatrices;
        auto loaded = parser.loadGltf(input.get(), source.parent_path(), options);
        if (loaded.error() != fastgltf::Error::None)
            throw std::runtime_error(std::string(fastgltf::getErrorMessage(loaded.error())));
        fastgltf::Asset asset = std::move(loaded.get());

        std::vector<std::string> imagePaths(asset.images.size());
        for (size_t imageIndex = 0; imageIndex < asset.images.size(); ++imageIndex)
        {
            const auto& image = asset.images[imageIndex];
            const std::string name = SafeName(
                image.name, "Texture " + std::to_string(imageIndex + 1));
            const fs::path path = texturesDirectory /
                (name + " " + std::to_string(imageIndex + 1) +
                 ImageExtension(image));
            if (!ExtractImage(asset, image, path))
                throw std::runtime_error("Could not extract texture " + std::to_string(imageIndex));
            AssetRecord::Ensure(path, source,
                {
                    { "importer", std::string("gltf-image") },
                    { "imageIndex", static_cast<double>(imageIndex) }
                });
            imagePaths[imageIndex] = path.generic_string();
        }

        auto texturePath = [&](const auto& info) -> std::string
        {
            if (!info || info->textureIndex >= asset.textures.size())
                return {};
            const auto imageIndex = TextureImageIndex(asset.textures[info->textureIndex]);
            return imageIndex && *imageIndex < imagePaths.size() ? imagePaths[*imageIndex] : std::string{};
        };

        std::vector<std::string> materialPaths(asset.materials.size());
        for (size_t materialIndex = 0; materialIndex < asset.materials.size(); ++materialIndex)
        {
            const auto& imported = asset.materials[materialIndex];
            Material material;
            material.diffuseColor = {
                imported.pbrData.baseColorFactor.x(),
                imported.pbrData.baseColorFactor.y(),
                imported.pbrData.baseColorFactor.z()
            };
            material.emissiveColor = {
                imported.emissiveFactor.x(),
                imported.emissiveFactor.y(),
                imported.emissiveFactor.z()
            };
            material.metallicFactor = imported.pbrData.metallicFactor;
            material.roughnessFactor = imported.pbrData.roughnessFactor;
            material.baseColorAlpha = imported.pbrData.baseColorFactor.w();
            material.alphaCutoff = imported.alphaCutoff;
            material.doubleSided = imported.doubleSided;
            material.unlit = imported.unlit;
            switch (imported.alphaMode)
            {
            case fastgltf::AlphaMode::Mask: material.alphaMode = "Mask"; break;
            case fastgltf::AlphaMode::Blend: material.alphaMode = "Blend"; break;
            default: material.alphaMode = "Opaque"; break;
            }
            if (imported.normalTexture)
                material.normalScale = imported.normalTexture->scale;
            if (imported.occlusionTexture)
                material.occlusionStrength = imported.occlusionTexture->strength;
            material.SetBaseColorTexture(texturePath(imported.pbrData.baseColorTexture));
            material.SetMetallicRoughnessTexture(texturePath(imported.pbrData.metallicRoughnessTexture));
            material.SetNormalTexture(texturePath(imported.normalTexture));
            material.SetOcclusionTexture(texturePath(imported.occlusionTexture));
            material.SetEmissiveTexture(texturePath(imported.emissiveTexture));
            auto uvSet = [](const auto& info)
            {
                if (!info) return 0;
                const size_t selected = info->transform && info->transform->texCoordIndex
                    ? *info->transform->texCoordIndex : info->texCoordIndex;
                return static_cast<int>(std::min<size_t>(selected, 1));
            };
            material.baseColorUvSet = uvSet(imported.pbrData.baseColorTexture);
            material.metallicRoughnessUvSet = uvSet(imported.pbrData.metallicRoughnessTexture);
            material.normalUvSet = uvSet(imported.normalTexture);
            material.occlusionUvSet = uvSet(imported.occlusionTexture);
            material.emissiveUvSet = uvSet(imported.emissiveTexture);

            const std::string name = SafeName(
                imported.name, "Material " + std::to_string(materialIndex + 1));
            const fs::path path = materialsDirectory /
                (name + " " + std::to_string(materialIndex + 1) + ".material");
            if (!material.SaveToFile(path.string()))
                throw std::runtime_error("Could not save material: " + path.string());
            AssetRecord::Ensure(path, source,
                {
                    { "importer", std::string("gltf-material") },
                    { "materialIndex", static_cast<double>(materialIndex) }
                });
            materialPaths[materialIndex] = path.generic_string();
        }

        Material defaultMaterial;
        const fs::path defaultMaterialPath = materialsDirectory / "Default.material";
        if (!defaultMaterial.SaveToFile(defaultMaterialPath.string()))
            throw std::runtime_error("Could not save the default material");
        AssetRecord::Ensure(defaultMaterialPath, source,
            { { "importer", std::string("gltf-default-material") } });

        std::vector<std::vector<std::string>> meshPaths(asset.meshes.size());
        std::vector<std::vector<ImportedPrimitiveRuntime>> meshRuntime(asset.meshes.size());
        for (size_t meshIndex = 0; meshIndex < asset.meshes.size(); ++meshIndex)
        {
            const auto& importedMesh = asset.meshes[meshIndex];
            meshPaths[meshIndex].resize(importedMesh.primitives.size());
            meshRuntime[meshIndex].resize(importedMesh.primitives.size());
            for (size_t primitiveIndex = 0; primitiveIndex < importedMesh.primitives.size(); ++primitiveIndex)
            {
                const auto& primitive = importedMesh.primitives[primitiveIndex];
                if (primitive.type != fastgltf::PrimitiveType::Triangles &&
                    primitive.type != fastgltf::PrimitiveType::TriangleStrip &&
                    primitive.type != fastgltf::PrimitiveType::TriangleFan)
                    continue;

                const auto positionAttribute = primitive.findAttribute("POSITION");
                if (positionAttribute == primitive.attributes.end() || !primitive.indicesAccessor)
                    throw std::runtime_error("Mesh primitive is missing positions or indices");

                const auto& positionAccessor = asset.accessors[positionAttribute->accessorIndex];
                std::vector<fastgltf::math::fvec3> positions(positionAccessor.count);
                fastgltf::copyFromAccessor<fastgltf::math::fvec3>(
                    asset, positionAccessor, positions.data());

                std::vector<fastgltf::math::fvec3> normals(positionAccessor.count);
                std::vector<fastgltf::math::fvec2> texcoords(positionAccessor.count);
                std::vector<fastgltf::math::fvec2> texcoords1(positionAccessor.count);
                std::vector<fastgltf::math::fvec4> tangents(positionAccessor.count);
                std::vector<fastgltf::math::fvec4> colors(positionAccessor.count,
                    fastgltf::math::fvec4(1.f));
                std::vector<fastgltf::math::uvec4> jointData(positionAccessor.count);
                std::vector<fastgltf::math::fvec4> weightData(positionAccessor.count);
                std::vector<fastgltf::math::uvec4> jointData1(positionAccessor.count);
                std::vector<fastgltf::math::fvec4> weightData1(positionAccessor.count);
                bool hasNormals = false;
                bool hasTexcoords = false;
                bool hasTexcoords1 = false;
                bool hasTangents = false;
                bool hasColors = false;
                bool hasSkinData = false;
                bool hasSkinData1 = false;
                const auto normalAttribute = primitive.findAttribute("NORMAL");
                if (normalAttribute != primitive.attributes.end())
                {
                    const auto& normalAccessor = asset.accessors[normalAttribute->accessorIndex];
                    fastgltf::copyFromAccessor<fastgltf::math::fvec3>(
                        asset, normalAccessor, normals.data());
                    hasNormals = true;
                }
                const auto texcoordAttribute = primitive.findAttribute("TEXCOORD_0");
                if (texcoordAttribute != primitive.attributes.end())
                {
                    const auto& accessor = asset.accessors[texcoordAttribute->accessorIndex];
                    fastgltf::copyFromAccessor<fastgltf::math::fvec2>(
                        asset, accessor, texcoords.data());
                    hasTexcoords = true;
                }
                const auto texcoord1Attribute = primitive.findAttribute("TEXCOORD_1");
                if (texcoord1Attribute != primitive.attributes.end())
                {
                    fastgltf::copyFromAccessor<fastgltf::math::fvec2>(asset,
                        asset.accessors[texcoord1Attribute->accessorIndex], texcoords1.data());
                    hasTexcoords1 = true;
                }
                const auto tangentAttribute = primitive.findAttribute("TANGENT");
                if (tangentAttribute != primitive.attributes.end())
                {
                    const auto& accessor = asset.accessors[tangentAttribute->accessorIndex];
                    fastgltf::copyFromAccessor<fastgltf::math::fvec4>(
                        asset, accessor, tangents.data());
                    hasTangents = true;
                }
                const auto colorAttribute = primitive.findAttribute("COLOR_0");
                if (colorAttribute != primitive.attributes.end())
                {
                    const auto& accessor = asset.accessors[colorAttribute->accessorIndex];
                    if (accessor.type == fastgltf::AccessorType::Vec3)
                    {
                        std::vector<fastgltf::math::fvec3> rgb(positionAccessor.count);
                        fastgltf::copyFromAccessor<fastgltf::math::fvec3>(asset, accessor, rgb.data());
                        for (size_t i = 0; i < rgb.size(); ++i)
                            colors[i] = { rgb[i].x(), rgb[i].y(), rgb[i].z(), 1.f };
                    }
                    else
                        fastgltf::copyFromAccessor<fastgltf::math::fvec4>(asset, accessor, colors.data());
                    hasColors = true;
                }
                const auto jointsAttribute = primitive.findAttribute("JOINTS_0");
                const auto weightsAttribute = primitive.findAttribute("WEIGHTS_0");
                if (jointsAttribute != primitive.attributes.end() && weightsAttribute != primitive.attributes.end())
                {
                    fastgltf::copyFromAccessor<fastgltf::math::uvec4>(asset,
                        asset.accessors[jointsAttribute->accessorIndex], jointData.data());
                    fastgltf::copyFromAccessor<fastgltf::math::fvec4>(asset,
                        asset.accessors[weightsAttribute->accessorIndex], weightData.data());
                    hasSkinData = true;
                }
                const auto joints1Attribute = primitive.findAttribute("JOINTS_1");
                const auto weights1Attribute = primitive.findAttribute("WEIGHTS_1");
                if (joints1Attribute != primitive.attributes.end() && weights1Attribute != primitive.attributes.end())
                {
                    fastgltf::copyFromAccessor<fastgltf::math::uvec4>(asset,
                        asset.accessors[joints1Attribute->accessorIndex], jointData1.data());
                    fastgltf::copyFromAccessor<fastgltf::math::fvec4>(asset,
                        asset.accessors[weights1Attribute->accessorIndex], weightData1.data());
                    hasSkinData1 = true;
                }

                std::vector<std::vector<fastgltf::math::fvec3>> morphPositions(primitive.targets.size());
                std::vector<std::vector<fastgltf::math::fvec3>> morphNormals(primitive.targets.size());
                std::vector<std::vector<fastgltf::math::fvec3>> morphTangents(primitive.targets.size());
                for (size_t targetIndex = 0; targetIndex < primitive.targets.size(); ++targetIndex)
                {
                    auto copyTarget = [&](const char* semantic, auto& destination)
                    {
                        const auto attribute = primitive.findTargetAttribute(targetIndex, semantic);
                        if (attribute == primitive.targets[targetIndex].end()) return;
                        destination.resize(positionAccessor.count);
                        fastgltf::copyFromAccessor<fastgltf::math::fvec3>(asset,
                            asset.accessors[attribute->accessorIndex], destination.data());
                    };
                    copyTarget("POSITION", morphPositions[targetIndex]);
                    copyTarget("NORMAL", morphNormals[targetIndex]);
                    copyTarget("TANGENT", morphTangents[targetIndex]);
                }

                const auto& indexAccessor = asset.accessors[*primitive.indicesAccessor];
                std::vector<uint32_t> indices(indexAccessor.count);
                fastgltf::copyFromAccessor<uint32_t>(asset, indexAccessor, indices.data());
                indices = TriangleIndices(primitive, std::move(indices));

                std::vector<Vertex> vertices;
                vertices.reserve(indices.size());
                ImportedPrimitiveRuntime& runtime = meshRuntime[meshIndex][primitiveIndex];
                runtime.morphTargets.resize(primitive.targets.size());
                for (uint32_t index : indices)
                {
                    if (index >= positions.size())
                        throw std::runtime_error("Mesh index is outside the position accessor");
                    Vertex vertex{};
                    vertex.pos[0] = positions[index].x();
                    vertex.pos[1] = positions[index].y();
                    vertex.pos[2] = positions[index].z();
                    if (hasNormals && index < normals.size())
                    {
                        vertex.normal[0] = normals[index].x();
                        vertex.normal[1] = normals[index].y();
                        vertex.normal[2] = normals[index].z();
                    }
                    if (hasTexcoords && index < texcoords.size())
                    {
                        vertex.uv[0] = texcoords[index].x();
                        vertex.uv[1] = texcoords[index].y();
                    }
                    if (hasTangents && index < tangents.size())
                    {
                        vertex.tangent[0] = tangents[index].x();
                        vertex.tangent[1] = tangents[index].y();
                        vertex.tangent[2] = tangents[index].z();
                        vertex.tangent[3] = tangents[index].w();
                    }
                    if (hasTexcoords1 && index < texcoords1.size())
                    {
                        vertex.uv1[0] = texcoords1[index].x(); vertex.uv1[1] = texcoords1[index].y();
                    }
                    if (hasColors && index < colors.size())
                    {
                        vertex.color[0] = colors[index].x(); vertex.color[1] = colors[index].y();
                        vertex.color[2] = colors[index].z(); vertex.color[3] = colors[index].w();
                    }
                    if (hasSkinData)
                    {
                        const auto& importedJoints = jointData[index];
                        const auto& importedWeights = weightData[index];
                        for (size_t influence = 0; influence < 4; ++influence)
                        {
                            vertex.joints0[influence] = static_cast<float>(importedJoints[influence]);
                            vertex.weights0[influence] = importedWeights[influence];
                        }
                    }
                    if (hasSkinData1)
                    {
                        const auto& importedJoints = jointData1[index];
                        const auto& importedWeights = weightData1[index];
                        for (size_t influence = 0; influence < 4; ++influence)
                        {
                            vertex.joints1[influence] = static_cast<float>(importedJoints[influence]);
                            vertex.weights1[influence] = importedWeights[influence];
                        }
                    }
                    float totalWeight = 0.f;
                    for (size_t influence = 0; influence < 4; ++influence)
                        totalWeight += vertex.weights0[influence] + vertex.weights1[influence];
                    if (hasSkinData && totalWeight > 0.f)
                        for (size_t influence = 0; influence < 4; ++influence)
                        {
                            vertex.weights0[influence] /= totalWeight;
                            vertex.weights1[influence] /= totalWeight;
                        }
                    else if (hasSkinData)
                        vertex.weights0[0] = 1.f;
                    vertices.push_back(vertex);
                    for (size_t targetIndex = 0; targetIndex < runtime.morphTargets.size(); ++targetIndex)
                    {
                        auto expanded = [index](const auto& source) { return index < source.size() ? glm::vec3(source[index].x(), source[index].y(), source[index].z()) : glm::vec3(0.f); };
                        runtime.morphTargets[targetIndex].positions.push_back(expanded(morphPositions[targetIndex]));
                        runtime.morphTargets[targetIndex].normals.push_back(expanded(morphNormals[targetIndex]));
                        runtime.morphTargets[targetIndex].tangents.push_back(expanded(morphTangents[targetIndex]));
                    }
                }

                if (!hasNormals)
                {
                    for (size_t i = 0; i + 2 < vertices.size(); i += 3)
                    {
                        const glm::vec3 a(
                            vertices[i].pos[0], vertices[i].pos[1], vertices[i].pos[2]);
                        const glm::vec3 b(
                            vertices[i + 1].pos[0], vertices[i + 1].pos[1], vertices[i + 1].pos[2]);
                        const glm::vec3 c(
                            vertices[i + 2].pos[0], vertices[i + 2].pos[1], vertices[i + 2].pos[2]);
                        glm::vec3 normal = glm::cross(b - a, c - a);
                        if (glm::length(normal) > 0.000001f)
                            normal = glm::normalize(normal);
                        for (size_t corner = 0; corner < 3; ++corner)
                        {
                            vertices[i + corner].normal[0] = normal.x;
                            vertices[i + corner].normal[1] = normal.y;
                            vertices[i + corner].normal[2] = normal.z;
                        }
                    }
                }

                const std::string meshName = SafeName(
                    importedMesh.name, "Mesh " + std::to_string(meshIndex + 1));
                const fs::path path = meshesDirectory /
                    (meshName + " " + std::to_string(meshIndex + 1) + "_" +
                     std::to_string(primitiveIndex + 1) + ".mesh");
                if (!Mesh::SaveNativeFile(path.string(), vertices))
                    throw std::runtime_error("Could not save mesh: " + path.string());
                AssetRecord::Ensure(path, source,
                    {
                        { "importer", std::string("gltf-mesh") },
                        { "meshIndex", static_cast<double>(meshIndex) },
                        { "primitiveIndex", static_cast<double>(primitiveIndex) }
                    });
                meshPaths[meshIndex][primitiveIndex] = path.generic_string();
            }
        }

        Scene prefabScene;
        Object* prefabRoot = prefabScene.AddObject(modelName);

        // Skins are shared by every primitive that references the glTF skin.
        // Keeping them on the prefab root also gives animation and deformation
        // a single stable place from which to resolve node identities.
        for (size_t skinIndex = 0; skinIndex < asset.skins.size(); ++skinIndex)
        {
            const auto& importedSkin = asset.skins[skinIndex];
            Skeleton* skeleton = prefabRoot->AddComponent<Skeleton>();
            skeleton->skinIndex = static_cast<unsigned>(skinIndex);
            for (size_t joint : importedSkin.joints)
                skeleton->jointNodes.push_back(static_cast<unsigned>(joint));
            skeleton->inverseBindMatrices.assign(importedSkin.joints.size(), glm::mat4(1.f));
            if (importedSkin.inverseBindMatrices)
            {
                const auto& accessor = asset.accessors[*importedSkin.inverseBindMatrices];
                std::vector<fastgltf::math::fmat4x4> matrices(accessor.count);
                fastgltf::copyFromAccessor<fastgltf::math::fmat4x4>(asset, accessor, matrices.data());
                skeleton->inverseBindMatrices.resize(matrices.size());
                for (size_t matrixIndex = 0; matrixIndex < matrices.size(); ++matrixIndex)
                    for (glm::length_t column = 0; column < 4; ++column)
                        for (glm::length_t row = 0; row < 4; ++row)
                            skeleton->inverseBindMatrices[matrixIndex][column][row] = matrices[matrixIndex][column][row];
            }
        }

        for (size_t animationIndex = 0; animationIndex < asset.animations.size(); ++animationIndex)
        {
            const auto& importedAnimation = asset.animations[animationIndex];
            Animation* animation = prefabRoot->AddComponent<Animation>();
            animation->clipName = importedAnimation.name.empty()
                ? "Animation " + std::to_string(animationIndex + 1)
                : std::string(importedAnimation.name);
            for (const auto& importedChannel : importedAnimation.channels)
            {
                if (!importedChannel.nodeIndex || importedChannel.samplerIndex >= importedAnimation.samplers.size())
                    continue;
                const auto& sampler = importedAnimation.samplers[importedChannel.samplerIndex];
                const auto& inputAccessor = asset.accessors[sampler.inputAccessor];
                const auto& outputAccessor = asset.accessors[sampler.outputAccessor];
                AnimationChannel channel;
                channel.nodeIndex = static_cast<unsigned>(*importedChannel.nodeIndex);
                switch (importedChannel.path)
                {
                case fastgltf::AnimationPath::Rotation: channel.path = AnimationChannel::Path::Rotation; channel.valueWidth = 4; break;
                case fastgltf::AnimationPath::Scale: channel.path = AnimationChannel::Path::Scale; channel.valueWidth = 3; break;
                case fastgltf::AnimationPath::Weights:
                {
                    channel.path = AnimationChannel::Path::Weights;
                    const size_t splineFactor = sampler.interpolation == fastgltf::AnimationInterpolation::CubicSpline ? 3 : 1;
                    channel.valueWidth = inputAccessor.count > 0
                        ? static_cast<unsigned>(outputAccessor.count / (inputAccessor.count * splineFactor)) : 0;
                    break;
                }
                default: channel.path = AnimationChannel::Path::Translation; channel.valueWidth = 3; break;
                }
                switch (sampler.interpolation)
                {
                case fastgltf::AnimationInterpolation::Step: channel.interpolation = AnimationChannel::Interpolation::Step; break;
                case fastgltf::AnimationInterpolation::CubicSpline: channel.interpolation = AnimationChannel::Interpolation::CubicSpline; break;
                default: channel.interpolation = AnimationChannel::Interpolation::Linear; break;
                }
                channel.times.resize(inputAccessor.count);
                fastgltf::copyFromAccessor<float>(asset, inputAccessor, channel.times.data());
                if (!channel.times.empty()) animation->duration = std::max(animation->duration, channel.times.back());
                if (channel.path == AnimationChannel::Path::Weights)
                {
                    channel.values.resize(outputAccessor.count);
                    fastgltf::copyFromAccessor<float>(asset, outputAccessor, channel.values.data());
                }
                else if (channel.valueWidth == 4)
                {
                    std::vector<fastgltf::math::fvec4> values(outputAccessor.count);
                    fastgltf::copyFromAccessor<fastgltf::math::fvec4>(asset, outputAccessor, values.data());
                    channel.values.reserve(values.size() * 4);
                    for (const auto& value : values)
                        channel.values.insert(channel.values.end(), { value.x(), value.y(), value.z(), value.w() });
                }
                else
                {
                    std::vector<fastgltf::math::fvec3> values(outputAccessor.count);
                    fastgltf::copyFromAccessor<fastgltf::math::fvec3>(asset, outputAccessor, values.data());
                    channel.values.reserve(values.size() * 3);
                    for (const auto& value : values)
                        channel.values.insert(channel.values.end(), { value.x(), value.y(), value.z() });
                }
                animation->channels.push_back(std::move(channel));
            }
        }
        if (!asset.animations.empty())
        {
            AnimationManager* manager = prefabRoot->AddComponent<AnimationManager>();
            manager->clip = prefabRoot->GetComponent<Animation>()->clipName;
        }

        Model* modelComponent = prefabRoot->AddComponent<Model>();
        std::function<void(size_t, Object*)> importNode =
            [&](size_t nodeIndex, Object* parent)
        {
            const auto& node = asset.nodes[nodeIndex];
            Object* object = AddChild(prefabScene, parent,
                SafeName(node.name, "Node " + std::to_string(nodeIndex + 1)));
            modelComponent->BindNode(static_cast<unsigned>(nodeIndex), object);
            if (const auto* trs = std::get_if<fastgltf::TRS>(&node.transform))
            {
                object->transform.position = {
                    trs->translation.x(), trs->translation.y(), trs->translation.z()
                };
                object->transform.scale = {
                    trs->scale.x(), trs->scale.y(), trs->scale.z()
                };
                const glm::quat rotation(
                    trs->rotation.w(), trs->rotation.x(),
                    trs->rotation.y(), trs->rotation.z());
                object->transform.rotation = QuaternionEuler(rotation);
            }

            if (node.meshIndex && *node.meshIndex < asset.meshes.size())
            {
                const size_t meshIndex = *node.meshIndex;
                const auto& importedMesh = asset.meshes[meshIndex];
                for (size_t primitiveIndex = 0;
                    primitiveIndex < importedMesh.primitives.size(); ++primitiveIndex)
                {
                    if (meshPaths[meshIndex][primitiveIndex].empty())
                        continue;
                    Object* target = importedMesh.primitives.size() == 1
                        ? object
                        : AddChild(prefabScene, object,
                            "Primitive " + std::to_string(primitiveIndex + 1));
                    Mesh* mesh = target->AddComponent<Mesh>();
                    mesh->LoadFromFile(meshPaths[meshIndex][primitiveIndex]);

                    ImportedPrimitiveRuntime& runtime = meshRuntime[meshIndex][primitiveIndex];
                    if (!runtime.morphTargets.empty())
                    {
                        const auto& defaults = !node.weights.empty() ? node.weights : importedMesh.weights;
                        std::vector<float> weights(runtime.morphTargets.size(), 0.f);
                        for (size_t i = 0; i < std::min(defaults.size(), weights.size()); ++i)
                            weights[i] = static_cast<float>(defaults[i]);
                        mesh->SetMorphData(static_cast<unsigned>(nodeIndex),
                            runtime.morphTargets, std::move(weights));
                    }
                    if (node.skinIndex || !runtime.morphTargets.empty())
                    {
                        SkinnedMesh* deformer = target->AddComponent<SkinnedMesh>();
                        deformer->skinIndex = node.skinIndex ? static_cast<int>(*node.skinIndex) : -1;
                    }

                    const auto& primitive = importedMesh.primitives[primitiveIndex];
                    const std::string materialPath =
                        primitive.materialIndex && *primitive.materialIndex < materialPaths.size()
                        ? materialPaths[*primitive.materialIndex]
                        : defaultMaterialPath.generic_string();
                    Material* material = target->AddComponent<Material>();
                    if (!material->LoadFromFile(materialPath))
                        throw std::runtime_error("Could not load generated material");
                }
            }

            for (size_t child : node.children)
                importNode(child, object);
        };

        if (!asset.scenes.empty())
        {
            const size_t sceneIndex = asset.defaultScene.value_or(0);
            if (sceneIndex >= asset.scenes.size())
                throw std::runtime_error("Default scene index is invalid");
            for (size_t nodeIndex : asset.scenes[sceneIndex].nodeIndices)
                importNode(nodeIndex, prefabRoot);
        }
        else
        {
            std::vector<bool> isChild(asset.nodes.size(), false);
            for (const auto& node : asset.nodes)
                for (size_t child : node.children)
                    if (child < isChild.size()) isChild[child] = true;
            for (size_t nodeIndex = 0; nodeIndex < asset.nodes.size(); ++nodeIndex)
                if (!isChild[nodeIndex])
                    importNode(nodeIndex, prefabRoot);
        }

        const fs::path prefabPath = output / (modelName + ".prefab");
        if (!SceneSerializer::SavePrefab(*prefabRoot, prefabPath.string()))
            throw std::runtime_error("Could not save prefab: " + prefabPath.string());
        AssetRecord::Ensure(prefabPath, source,
            {
                { "importer", std::string("gltf") },
                { "loadExternalBuffers", true },
                { "loadExternalImages", true },
                { "generateMeshIndices", true },
                { "decomposeNodeMatrices", true }
            });

        result.success = true;
        result.prefabPath = prefabPath.generic_string();
        result.outputDirectory = output.generic_string();
        result.message = "Imported " + source.filename().string();
        return result;
    }
    catch (const std::exception& error)
    {
        std::error_code cleanupError;
        if (!output.empty() && output.parent_path().lexically_normal() == normalizedAssets)
            fs::remove_all(output, cleanupError);
        result.message = error.what();
        return result;
    }
}

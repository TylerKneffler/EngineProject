#include "GltfImporter.h"

#include "Core/Assets/AssetRecord.h"
#include "Core/Compoonents/Material.h"
#include "Core/Compoonents/Mesh.h"
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

std::optional<size_t> TextureImageIndex(const fastgltf::Texture& texture)
{
    if (texture.imageIndex) return *texture.imageIndex;
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
                 MimeExtension(ImageMime(image)));
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
        for (size_t meshIndex = 0; meshIndex < asset.meshes.size(); ++meshIndex)
        {
            const auto& importedMesh = asset.meshes[meshIndex];
            meshPaths[meshIndex].resize(importedMesh.primitives.size());
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
                std::vector<fastgltf::math::fvec4> tangents(positionAccessor.count);
                bool hasNormals = false;
                bool hasTexcoords = false;
                bool hasTangents = false;
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
                const auto tangentAttribute = primitive.findAttribute("TANGENT");
                if (tangentAttribute != primitive.attributes.end())
                {
                    const auto& accessor = asset.accessors[tangentAttribute->accessorIndex];
                    fastgltf::copyFromAccessor<fastgltf::math::fvec4>(
                        asset, accessor, tangents.data());
                    hasTangents = true;
                }

                const auto& indexAccessor = asset.accessors[*primitive.indicesAccessor];
                std::vector<uint32_t> indices(indexAccessor.count);
                fastgltf::copyFromAccessor<uint32_t>(asset, indexAccessor, indices.data());
                indices = TriangleIndices(primitive, std::move(indices));

                std::vector<Vertex> vertices;
                vertices.reserve(indices.size());
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
                    vertices.push_back(vertex);
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

        std::function<void(size_t, Object*)> importNode =
            [&](size_t nodeIndex, Object* parent)
        {
            const auto& node = asset.nodes[nodeIndex];
            Object* object = AddChild(prefabScene, parent,
                SafeName(node.name, "Node " + std::to_string(nodeIndex + 1)));
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

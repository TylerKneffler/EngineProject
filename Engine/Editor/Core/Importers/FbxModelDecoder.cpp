#include "FbxModelDecoder.h"

#include <assimp/Importer.hpp>
#include <assimp/config.h>
#include <assimp/material.h>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <functional>
#include <unordered_map>

namespace
{
glm::vec3 Vec3(const aiVector3D& value) { return { value.x, value.y, value.z }; }
glm::quat Quat(const aiQuaternion& value) { return { value.w, value.x, value.y, value.z }; }
glm::mat4 Matrix(const aiMatrix4x4& value)
{
    // aiMatrix4x4 is row-major while GLM indexes column first.
    glm::mat4 result(1.f);
    result[0] = { value.a1, value.b1, value.c1, value.d1 };
    result[1] = { value.a2, value.b2, value.c2, value.d2 };
    result[2] = { value.a3, value.b3, value.c3, value.d3 };
    result[3] = { value.a4, value.b4, value.c4, value.d4 };
    return result;
}

std::string Name(const aiString& value, const std::string& fallback)
{
    const std::string name = value.C_Str();
    return name.empty() ? fallback : name;
}

std::string TexturePath(const aiMaterial* material, aiTextureType type)
{
    aiString path;
    return material && material->GetTexture(type, 0, &path) == AI_SUCCESS
        ? std::string(path.C_Str()) : std::string{};
}

struct Influence { unsigned joint = 0; float weight = 0.f; };

void StoreInfluences(Vertex& vertex, std::vector<Influence> influences)
{
    influences.erase(std::remove_if(influences.begin(), influences.end(),
        [](const Influence& value) { return value.weight <= 0.f; }), influences.end());
    std::stable_sort(influences.begin(), influences.end(),
        [](const Influence& left, const Influence& right) { return left.weight > right.weight; });
    if (influences.size() > 8) influences.resize(8);
    float total = 0.f;
    for (const Influence& influence : influences) total += influence.weight;
    if (total <= 0.f) return;
    for (size_t index = 0; index < influences.size(); ++index)
    {
        float* joints = index < 4 ? vertex.joints0 : vertex.joints1;
        float* weights = index < 4 ? vertex.weights0 : vertex.weights1;
        const size_t slot = index & 3u;
        joints[slot] = static_cast<float>(influences[index].joint);
        weights[slot] = influences[index].weight / total;
    }
}

AnimationChannel VectorChannel(unsigned nodeIndex, AnimationChannel::Path path,
    const aiVectorKey* keys, unsigned count, double ticksPerSecond)
{
    AnimationChannel channel;
    channel.nodeIndex = nodeIndex;
    channel.path = path;
    channel.valueWidth = 3;
    channel.times.reserve(count);
    channel.values.reserve(static_cast<size_t>(count) * 3);
    for (unsigned index = 0; index < count; ++index)
    {
        channel.times.push_back(static_cast<float>(keys[index].mTime / ticksPerSecond));
        const aiVector3D& value = keys[index].mValue;
        channel.values.insert(channel.values.end(), { value.x, value.y, value.z });
    }
    return channel;
}
}

bool FbxModelDecoder::Decode(const std::string& sourcePath, ImportedModel& model,
    std::string& error)
{
    Assimp::Importer importer;
    importer.SetPropertyInteger(AI_CONFIG_PP_LBW_MAX_WEIGHTS, 8);
    const aiScene* scene = importer.ReadFile(sourcePath,
        aiProcess_Triangulate | aiProcess_JoinIdenticalVertices |
        aiProcess_GenSmoothNormals | aiProcess_CalcTangentSpace |
        aiProcess_SortByPType | aiProcess_ValidateDataStructure |
        aiProcess_LimitBoneWeights);
    if (!scene || !scene->mRootNode)
    {
        error = importer.GetErrorString();
        return false;
    }

    model = {};
    model.name = std::filesystem::path(sourcePath).stem().string();
    model.sourceFormat = "fbx";

    for (unsigned index = 0; index < scene->mNumTextures; ++index)
    {
        const aiTexture* source = scene->mTextures[index];
        // FBX normally embeds PNG/JPEG bytes (height == 0). Raw texels need an
        // image encoder and are deliberately left unresolved instead of being
        // written as a misleading image file.
        if (!source || source->mHeight != 0 || source->mWidth == 0) continue;
        ImportedTexture texture;
        texture.source = "*" + std::to_string(index);
        std::string extension = source->achFormatHint;
        if (extension.empty()) extension = "bin";
        texture.name = "Embedded " + std::to_string(index + 1) + "." + extension;
        const std::byte* begin = reinterpret_cast<const std::byte*>(source->pcData);
        texture.bytes.assign(begin, begin + source->mWidth);
        model.textures.push_back(std::move(texture));
    }

    std::unordered_map<std::string, unsigned> nodeByName;
    std::vector<unsigned> meshOwnerNode(scene->mNumMeshes, 0);
    std::function<void(const aiNode*, int)> readNode = [&](const aiNode* source, int parent)
    {
        const unsigned index = static_cast<unsigned>(model.nodes.size());
        ImportedNode node;
        node.name = Name(source->mName, "Node " + std::to_string(index + 1));
        node.parent = parent;
        aiVector3D scale, translation;
        aiQuaternion rotation;
        source->mTransformation.Decompose(scale, rotation, translation);
        node.translation = Vec3(translation);
        node.rotation = Quat(rotation);
        node.scale = Vec3(scale);
        node.primitives.assign(source->mMeshes, source->mMeshes + source->mNumMeshes);
        for (unsigned mesh = 0; mesh < source->mNumMeshes; ++mesh)
            if (source->mMeshes[mesh] < meshOwnerNode.size())
                meshOwnerNode[source->mMeshes[mesh]] = index;
        model.nodes.push_back(std::move(node));
        nodeByName.emplace(source->mName.C_Str(), index);
        for (unsigned child = 0; child < source->mNumChildren; ++child)
            readNode(source->mChildren[child], static_cast<int>(index));
    };
    readNode(scene->mRootNode, -1);

    model.materials.resize(scene->mNumMaterials);
    for (unsigned index = 0; index < scene->mNumMaterials; ++index)
    {
        const aiMaterial* source = scene->mMaterials[index];
        ImportedMaterial& material = model.materials[index];
        aiString materialName;
        if (source->Get(AI_MATKEY_NAME, materialName) == AI_SUCCESS)
            material.name = Name(materialName, "Material " + std::to_string(index + 1));
        aiColor4D color(1.f, 1.f, 1.f, 1.f);
        if (source->Get(AI_MATKEY_BASE_COLOR, color) != AI_SUCCESS)
            source->Get(AI_MATKEY_COLOR_DIFFUSE, color);
        material.baseColor = { color.r, color.g, color.b, color.a };
        aiColor3D emissive(0.f, 0.f, 0.f);
        if (source->Get(AI_MATKEY_COLOR_EMISSIVE, emissive) == AI_SUCCESS)
            material.emissiveColor = { emissive.r, emissive.g, emissive.b };
        source->Get(AI_MATKEY_METALLIC_FACTOR, material.metallic);
        source->Get(AI_MATKEY_ROUGHNESS_FACTOR, material.roughness);
        int twoSided = 0;
        source->Get(AI_MATKEY_TWOSIDED, twoSided);
        material.doubleSided = twoSided != 0;
        float opacity = material.baseColor.a;
        if (source->Get(AI_MATKEY_OPACITY, opacity) == AI_SUCCESS)
            material.baseColor.a = opacity;
        material.alphaMode = material.baseColor.a < 0.999f ? "Blend" : "Opaque";
        material.baseColorTexture = TexturePath(source, aiTextureType_BASE_COLOR);
        if (material.baseColorTexture.empty()) material.baseColorTexture = TexturePath(source, aiTextureType_DIFFUSE);
        material.metallicRoughnessTexture = TexturePath(source, aiTextureType_METALNESS);
        material.normalTexture = TexturePath(source, aiTextureType_NORMALS);
        if (material.normalTexture.empty()) material.normalTexture = TexturePath(source, aiTextureType_HEIGHT);
        material.occlusionTexture = TexturePath(source, aiTextureType_AMBIENT_OCCLUSION);
        material.emissiveTexture = TexturePath(source, aiTextureType_EMISSIVE);
    }

    model.primitives.resize(scene->mNumMeshes);
    for (unsigned meshIndex = 0; meshIndex < scene->mNumMeshes; ++meshIndex)
    {
        const aiMesh* source = scene->mMeshes[meshIndex];
        ImportedPrimitive& primitive = model.primitives[meshIndex];
        primitive.name = Name(source->mName, "Mesh " + std::to_string(meshIndex + 1));
        primitive.materialIndex = source->mMaterialIndex < scene->mNumMaterials
            ? static_cast<int>(source->mMaterialIndex) : -1;

        std::vector<std::vector<Influence>> influences(source->mNumVertices);
        if (source->HasBones())
        {
            ImportedSkin skin;
            skin.name = primitive.name + " Skeleton";
            for (unsigned boneIndex = 0; boneIndex < source->mNumBones; ++boneIndex)
            {
                const aiBone* bone = source->mBones[boneIndex];
                const auto found = nodeByName.find(bone->mName.C_Str());
                if (found == nodeByName.end()) continue;
                const unsigned paletteIndex = static_cast<unsigned>(skin.jointNodes.size());
                skin.jointNodes.push_back(found->second);
                skin.inverseBindMatrices.push_back(Matrix(bone->mOffsetMatrix));
                for (unsigned weight = 0; weight < bone->mNumWeights; ++weight)
                {
                    const aiVertexWeight& value = bone->mWeights[weight];
                    if (value.mVertexId < influences.size())
                        influences[value.mVertexId].push_back({ paletteIndex, value.mWeight });
                }
            }
            if (!skin.jointNodes.empty())
            {
                primitive.skinIndex = static_cast<int>(model.skins.size());
                model.skins.push_back(std::move(skin));
            }
        }

        primitive.morphTargets.reserve(source->mNumAnimMeshes);
        for (unsigned morphIndex = 0; morphIndex < source->mNumAnimMeshes; ++morphIndex)
        {
            const aiAnimMesh* morph = source->mAnimMeshes[morphIndex];
            MorphTargets::Target target;
            target.positions.resize(source->mNumVertices);
            if (morph->HasNormals()) target.normals.resize(source->mNumVertices);
            if (morph->HasTangentsAndBitangents()) target.tangents.resize(source->mNumVertices);
            for (unsigned vertex = 0; vertex < source->mNumVertices; ++vertex)
            {
                if (morph->HasPositions()) target.positions[vertex] = Vec3(morph->mVertices[vertex] - source->mVertices[vertex]);
                if (morph->HasNormals()) target.normals[vertex] = Vec3(morph->mNormals[vertex] - source->mNormals[vertex]);
                if (morph->HasTangentsAndBitangents()) target.tangents[vertex] = Vec3(morph->mTangents[vertex] - source->mTangents[vertex]);
            }
            primitive.morphTargets.push_back(std::move(target));
            primitive.morphWeights.push_back(static_cast<float>(morph->mWeight));
        }

        for (unsigned faceIndex = 0; faceIndex < source->mNumFaces; ++faceIndex)
        {
            const aiFace& face = source->mFaces[faceIndex];
            if (face.mNumIndices != 3) continue;
            for (unsigned corner = 0; corner < 3; ++corner)
            {
                const unsigned sourceIndex = face.mIndices[corner];
                Vertex vertex{};
                vertex.color[0] = vertex.color[1] = vertex.color[2] = vertex.color[3] = 1.f;
                const aiVector3D& position = source->mVertices[sourceIndex];
                vertex.pos[0] = position.x; vertex.pos[1] = position.y; vertex.pos[2] = position.z;
                if (source->HasNormals())
                {
                    const aiVector3D& normal = source->mNormals[sourceIndex];
                    vertex.normal[0] = normal.x; vertex.normal[1] = normal.y; vertex.normal[2] = normal.z;
                }
                if (source->HasTextureCoords(0))
                {
                    vertex.uv[0] = source->mTextureCoords[0][sourceIndex].x;
                    vertex.uv[1] = source->mTextureCoords[0][sourceIndex].y;
                }
                if (source->HasTextureCoords(1))
                {
                    vertex.uv1[0] = source->mTextureCoords[1][sourceIndex].x;
                    vertex.uv1[1] = source->mTextureCoords[1][sourceIndex].y;
                }
                if (source->HasTangentsAndBitangents())
                {
                    const aiVector3D& tangent = source->mTangents[sourceIndex];
                    vertex.tangent[0] = tangent.x; vertex.tangent[1] = tangent.y; vertex.tangent[2] = tangent.z;
                    const aiVector3D handed = source->mNormals[sourceIndex] ^ tangent;
                    vertex.tangent[3] = handed * source->mBitangents[sourceIndex] < 0.f ? -1.f : 1.f;
                }
                if (source->HasVertexColors(0))
                {
                    const aiColor4D& value = source->mColors[0][sourceIndex];
                    vertex.color[0] = value.r; vertex.color[1] = value.g;
                    vertex.color[2] = value.b; vertex.color[3] = value.a;
                }
                StoreInfluences(vertex, influences[sourceIndex]);
                primitive.vertices.push_back(vertex);
            }
        }

        // Morph arrays must follow the expanded triangle vertex stream.
        for (MorphTargets::Target& target : primitive.morphTargets)
        {
            MorphTargets::Target expanded;
            expanded.positions.reserve(primitive.vertices.size());
            if (!target.normals.empty()) expanded.normals.reserve(primitive.vertices.size());
            if (!target.tangents.empty()) expanded.tangents.reserve(primitive.vertices.size());
            for (unsigned faceIndex = 0; faceIndex < source->mNumFaces; ++faceIndex)
                for (unsigned corner = 0; corner < source->mFaces[faceIndex].mNumIndices; ++corner)
                {
                    const unsigned vertex = source->mFaces[faceIndex].mIndices[corner];
                    expanded.positions.push_back(target.positions[vertex]);
                    if (!target.normals.empty()) expanded.normals.push_back(target.normals[vertex]);
                    if (!target.tangents.empty()) expanded.tangents.push_back(target.tangents[vertex]);
                }
            target = std::move(expanded);
        }
    }

    model.animations.reserve(scene->mNumAnimations);
    for (unsigned animationIndex = 0; animationIndex < scene->mNumAnimations; ++animationIndex)
    {
        const aiAnimation* source = scene->mAnimations[animationIndex];
        ImportedAnimation animation;
        animation.name = Name(source->mName, "Animation " + std::to_string(animationIndex + 1));
        const double ticksPerSecond = source->mTicksPerSecond > 0.0 ? source->mTicksPerSecond : 25.0;
        animation.duration = static_cast<float>(source->mDuration / ticksPerSecond);
        for (unsigned channelIndex = 0; channelIndex < source->mNumChannels; ++channelIndex)
        {
            const aiNodeAnim* sourceChannel = source->mChannels[channelIndex];
            const auto found = nodeByName.find(sourceChannel->mNodeName.C_Str());
            if (found == nodeByName.end()) continue;
            if (sourceChannel->mNumPositionKeys)
                animation.channels.push_back(VectorChannel(found->second,
                    AnimationChannel::Path::Translation, sourceChannel->mPositionKeys,
                    sourceChannel->mNumPositionKeys, ticksPerSecond));
            if (sourceChannel->mNumScalingKeys)
                animation.channels.push_back(VectorChannel(found->second,
                    AnimationChannel::Path::Scale, sourceChannel->mScalingKeys,
                    sourceChannel->mNumScalingKeys, ticksPerSecond));
            if (sourceChannel->mNumRotationKeys)
            {
                AnimationChannel channel;
                channel.nodeIndex = found->second;
                channel.path = AnimationChannel::Path::Rotation;
                channel.valueWidth = 4;
                channel.times.reserve(sourceChannel->mNumRotationKeys);
                channel.values.reserve(static_cast<size_t>(sourceChannel->mNumRotationKeys) * 4);
                for (unsigned key = 0; key < sourceChannel->mNumRotationKeys; ++key)
                {
                    const aiQuatKey& value = sourceChannel->mRotationKeys[key];
                    channel.times.push_back(static_cast<float>(value.mTime / ticksPerSecond));
                    channel.values.insert(channel.values.end(),
                        { value.mValue.x, value.mValue.y, value.mValue.z, value.mValue.w });
                }
                animation.channels.push_back(std::move(channel));
            }
        }
        for (unsigned channelIndex = 0; channelIndex < source->mNumMorphMeshChannels; ++channelIndex)
        {
            const aiMeshMorphAnim* sourceChannel = source->mMorphMeshChannels[channelIndex];
            unsigned meshIndex = scene->mNumMeshes;
            for (unsigned candidate = 0; candidate < scene->mNumMeshes; ++candidate)
                if (scene->mMeshes[candidate]->mName == sourceChannel->mName)
                { meshIndex = candidate; break; }
            if (meshIndex >= scene->mNumMeshes || sourceChannel->mNumKeys == 0) continue;
            const unsigned width = scene->mMeshes[meshIndex]->mNumAnimMeshes;
            if (width == 0) continue;
            AnimationChannel channel;
            channel.nodeIndex = meshOwnerNode[meshIndex];
            channel.path = AnimationChannel::Path::Weights;
            channel.valueWidth = width;
            channel.times.reserve(sourceChannel->mNumKeys);
            channel.values.assign(static_cast<size_t>(sourceChannel->mNumKeys) * width, 0.f);
            for (unsigned keyIndex = 0; keyIndex < sourceChannel->mNumKeys; ++keyIndex)
            {
                const aiMeshMorphKey& key = sourceChannel->mKeys[keyIndex];
                channel.times.push_back(static_cast<float>(key.mTime / ticksPerSecond));
                for (unsigned valueIndex = 0; valueIndex < key.mNumValuesAndWeights; ++valueIndex)
                    if (key.mValues[valueIndex] < width)
                        channel.values[static_cast<size_t>(keyIndex) * width + key.mValues[valueIndex]] =
                            static_cast<float>(key.mWeights[valueIndex]);
            }
            animation.channels.push_back(std::move(channel));
        }
        model.animations.push_back(std::move(animation));
    }
    return true;
}

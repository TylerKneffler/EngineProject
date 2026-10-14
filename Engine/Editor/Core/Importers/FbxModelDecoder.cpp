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
#include <fstream>
#include <functional>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <limits>
#include <numeric>
#include <unordered_map>
#include <unordered_set>

namespace Engine::Editor
{
namespace
{
glm::vec3 Vec3(const aiVector3D& value) { return { value.x, value.y, value.z }; }
glm::quat Quat(const aiQuaternion& value) { return { value.w, value.x, value.y, value.z }; }
glm::quat Facing(const aiVector3D& direction, const aiVector3D& up)
{
    const glm::vec3 forward = Vec3(direction);
    const glm::vec3 sourceUp = Vec3(up);
    if (glm::dot(forward, forward) < 1e-8f || glm::dot(sourceUp, sourceUp) < 1e-8f)
        return glm::quat(1.f, 0.f, 0.f, 0.f);
    const glm::vec3 z = glm::normalize(forward);
    const glm::vec3 x = glm::cross(sourceUp, z);
    if (glm::dot(x, x) < 1e-8f)
        return glm::quat(1.f, 0.f, 0.f, 0.f);
    const glm::vec3 right = glm::normalize(x);
    const glm::vec3 adjustedUp = glm::cross(z, right);
    return glm::normalize(glm::quat_cast(glm::mat3(right, adjustedUp, z)));
}
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

void StoreInfluences(Engine::Model::AnimationVertex& vertex, std::vector<Influence> influences)
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

Engine::Model::AnimationChannel VectorChannel(unsigned nodeIndex, Engine::Model::AnimationChannel::Path path,
    const aiVectorKey* keys, unsigned count, double ticksPerSecond)
{
    Engine::Model::AnimationChannel channel;
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

bool FbxModelDecoder::Decode(const std::string& sourcePath, Engine::Model::ImportedModel& model,
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
    // FBX UnitScaleFactor expresses centimeters per source unit. Assimp
    // exposes it as float in this reader; other readers may use double.
    float sourceUnits = 1.f;
    if (scene->mMetaData && !scene->mMetaData->Get("UnitScaleFactor", sourceUnits))
    {
        double doubleUnits = 1.0;
        if (scene->mMetaData->Get("UnitScaleFactor", doubleUnits))
            sourceUnits = static_cast<float>(doubleUnits);
    }
    model.unitScaleToMeters = sourceUnits * 0.01f;
    if (!std::isfinite(model.unitScaleToMeters) || model.unitScaleToMeters <= 0.f)
    {
        error = "FBX has invalid source units";
        return false;
    }

    for (unsigned index = 0; index < scene->mNumTextures; ++index)
    {
        const aiTexture* source = scene->mTextures[index];
        // FBX normally embeds PNG/JPEG bytes (height == 0). Raw texels need an
        // image encoder and are deliberately left unresolved instead of being
        // written as a misleading image file.
        if (!source || source->mHeight != 0 || source->mWidth == 0) continue;
        Engine::Model::ImportedTexture texture;
        texture.source = "*" + std::to_string(index);
        std::string extension = source->achFormatHint;
        if (extension.empty()) extension = "bin";
        texture.name = "Embedded " + std::to_string(index + 1) + "." + extension;
        const std::byte* begin = reinterpret_cast<const std::byte*>(source->pcData);
        texture.bytes.assign(begin, begin + source->mWidth);
        model.textures.push_back(std::move(texture));
    }

    std::unordered_map<std::string, unsigned> nodeByName;
    std::unordered_map<std::string, std::vector<unsigned>> nodesByName;
    std::vector<unsigned> meshOwnerNode(scene->mNumMeshes, 0);
    std::function<void(const aiNode*, int)> readNode = [&](const aiNode* source, int parent)
    {
        const unsigned index = static_cast<unsigned>(model.nodes.size());
        Engine::Model::ImportedNode node;
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
        nodesByName[source->mName.C_Str()].push_back(index);
        for (unsigned child = 0; child < source->mNumChildren; ++child)
            readNode(source->mChildren[child], static_cast<int>(index));
    };
    readNode(scene->mRootNode, -1);

    for (unsigned index = 0; index < scene->mNumCameras; ++index)
    {
        const aiCamera* source = scene->mCameras[index];
        const auto node = nodeByName.find(source->mName.C_Str());
        if (node == nodeByName.end()) continue;
        Engine::Model::ImportedCamera camera;
        camera.nodeIndex = node->second;
        camera.position = Vec3(source->mPosition);
        camera.rotation = Facing(source->mLookAt, source->mUp);
        const float aspect = source->mAspect > 0.f ? source->mAspect : 16.f / 9.f;
        camera.verticalFov = glm::degrees(2.f * std::atan(std::tan(source->mHorizontalFOV) / aspect));
        camera.nearPlane = source->mClipPlaneNear;
        camera.farPlane = source->mClipPlaneFar;
        camera.orthographic = source->mOrthographicWidth > 0.f;
        camera.orthographicSize = camera.orthographic ? source->mOrthographicWidth / aspect : 5.f;
        model.cameras.push_back(camera);
    }

    for (unsigned index = 0; index < scene->mNumLights; ++index)
    {
        const aiLight* source = scene->mLights[index];
        const auto node = nodeByName.find(source->mName.C_Str());
        if (node == nodeByName.end()) continue;
        Engine::Model::ImportedLight light;
        light.nodeIndex = node->second;
        light.position = Vec3(source->mPosition);
        light.rotation = Facing(source->mDirection, source->mUp);
        if (source->mType == aiLightSource_DIRECTIONAL)
            light.type = Engine::Model::ImportedLight::Type::Directional;
        else if (source->mType == aiLightSource_SPOT)
            light.type = Engine::Model::ImportedLight::Type::Spot;
        else if (source->mType != aiLightSource_POINT) continue;
        const glm::vec3 diffuse(source->mColorDiffuse.r, source->mColorDiffuse.g, source->mColorDiffuse.b);
        light.intensity = std::max({ diffuse.r, diffuse.g, diffuse.b });
        light.color = light.intensity > 0.f ? diffuse / light.intensity : glm::vec3(0.f);
        // A 1% attenuation cutoff approximates Assimp's distance law in the engine's range model.
        const float constant = source->mAttenuationConstant;
        const float linear = source->mAttenuationLinear;
        const float quadratic = source->mAttenuationQuadratic;
        if (quadratic > 0.f)
            light.range = std::max(0.01f, (-linear + std::sqrt(linear * linear + 4.f * quadratic * std::max(0.f, 100.f - constant))) / (2.f * quadratic));
        else if (linear > 0.f)
            light.range = std::max(0.01f, (100.f - constant) / linear);
        light.innerConeAngle = glm::degrees(source->mAngleInnerCone);
        light.outerConeAngle = glm::degrees(source->mAngleOuterCone);
        model.lights.push_back(light);
    }

    std::vector<glm::mat4> bindWorld(model.nodes.size(), glm::mat4(1.f));
    for (size_t index = 0; index < model.nodes.size(); ++index)
    {
        const auto& node = model.nodes[index];
        const glm::mat4 local = glm::translate(glm::mat4(1.f), node.translation) *
            glm::mat4_cast(node.rotation) * glm::scale(glm::mat4(1.f), node.scale);
        bindWorld[index] = node.parent < 0 ? local :
            bindWorld[static_cast<size_t>(node.parent)] * local;
    }

    model.materials.resize(scene->mNumMaterials);
    for (unsigned index = 0; index < scene->mNumMaterials; ++index)
    {
        const aiMaterial* source = scene->mMaterials[index];
        Engine::Model::ImportedMaterial& material = model.materials[index];
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

    // An adjacent sidecar supplies explicit texture paths when an FBX omits
    // its external texture references. Paths remain relative to the FBX so
    // the writer can copy them into the imported asset directory.
    std::filesystem::path materialMapPath(sourcePath);
    materialMapPath += ".materials.tsv";
    if (std::filesystem::exists(materialMapPath))
    {
        std::ifstream materialMap(materialMapPath);
        if (!materialMap)
        {
            error = "Could not open FBX material map: " + materialMapPath.string();
            return false;
        }
        std::string line;
        size_t lineNumber = 0;
        std::unordered_set<std::string> mappedMaterials;
        while (std::getline(materialMap, line))
        {
            ++lineNumber;
            if (line.empty() || line.front() == '#') continue;
            const size_t firstTab = line.find('\t');
            const size_t secondTab = firstTab == std::string::npos ?
                std::string::npos : line.find('\t', firstTab + 1);
            if (secondTab == std::string::npos ||
                line.find('\t', secondTab + 1) != std::string::npos)
            {
                error = "Invalid FBX material map line " + std::to_string(lineNumber);
                return false;
            }
            const std::string name = line.substr(0, firstTab);
            const std::filesystem::path texture =
                line.substr(firstTab + 1, secondTab - firstTab - 1);
            const std::string alpha = line.substr(secondTab + 1);
            const auto material = std::find_if(model.materials.begin(), model.materials.end(),
                [&](const Engine::Model::ImportedMaterial& candidate)
                { return candidate.name == name; });
            if (material == model.materials.end() || texture.empty() ||
                texture.is_absolute() ||
                (alpha != "Opaque" && alpha != "Mask" && alpha != "Blend") ||
                !mappedMaterials.insert(name).second)
            {
                error = "Invalid FBX material mapping on line " +
                    std::to_string(lineNumber);
                return false;
            }
            for (const auto& part : texture)
                if (part == "..")
                {
                    error = "FBX material texture must stay beside the source";
                    return false;
                }
            std::filesystem::path resolved =
                std::filesystem::path(sourcePath).parent_path();
            for (const auto& part : texture)
            {
                const auto matching = std::find_if(
                    std::filesystem::directory_iterator(resolved),
                    std::filesystem::directory_iterator(),
                    [&](const std::filesystem::directory_entry& entry)
                    { return entry.path().filename() == part; });
                if (matching == std::filesystem::directory_iterator())
                {
                    error = "FBX material texture path has missing or incorrect case: " +
                        texture.string();
                    return false;
                }
                resolved /= part;
            }
            if (!std::filesystem::is_regular_file(resolved))
            {
                error = "FBX material texture not found: " + texture.string();
                return false;
            }
            material->baseColorTexture = texture.generic_string();
            material->alphaMode = alpha;
        }
    }

    model.primitives.resize(scene->mNumMeshes);
    std::vector<std::vector<unsigned>> morphIndexRemaps(scene->mNumMeshes);
    for (unsigned meshIndex = 0; meshIndex < scene->mNumMeshes; ++meshIndex)
    {
        const aiMesh* source = scene->mMeshes[meshIndex];
        Engine::Model::ImportedPrimitive& primitive = model.primitives[meshIndex];
        primitive.name = Name(source->mName, "Mesh " + std::to_string(meshIndex + 1));
        primitive.materialIndex = source->mMaterialIndex < scene->mNumMaterials
            ? static_cast<int>(source->mMaterialIndex) : -1;

        std::vector<std::vector<Influence>> influences(source->mNumVertices);
        if (source->HasBones())
        {
            Engine::Model::ImportedSkin skin;
            skin.name = primitive.name + " Skeleton";
            glm::mat4 referenceBind(1.f);
            for (unsigned boneIndex = 0; boneIndex < source->mNumBones; ++boneIndex)
            {
                const aiBone* bone = source->mBones[boneIndex];
                const auto found = nodesByName.find(bone->mName.C_Str());
                if (found != nodesByName.end() && found->second.size() == 1)
                {
                    referenceBind = bindWorld[found->second.front()] *
                        Matrix(bone->mOffsetMatrix);
                    break;
                }
            }
            for (unsigned boneIndex = 0; boneIndex < source->mNumBones; ++boneIndex)
            {
                const aiBone* bone = source->mBones[boneIndex];
                const auto found = nodesByName.find(bone->mName.C_Str());
                if (found == nodesByName.end()) continue;
                unsigned jointNode = found->second.front();
                float bestError = std::numeric_limits<float>::infinity();
                for (unsigned candidate : found->second)
                {
                    const glm::mat4 rest = bindWorld[candidate] *
                        Matrix(bone->mOffsetMatrix);
                    float difference = 0.f;
                    for (int column = 0; column < 4; ++column)
                        for (int row = 0; row < 4; ++row)
                            difference += std::abs(rest[column][row] -
                                referenceBind[column][row]);
                    if (difference < bestError)
                    {
                        bestError = difference;
                        jointNode = candidate;
                    }
                }
                const unsigned paletteIndex = static_cast<unsigned>(skin.jointNodes.size());
                skin.jointNodes.push_back(jointNode);
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

        // This FBX exports FacialsExtra_ with a complete skin palette but no
        // positive vertex weights. Its expression geometry is attached to the
        // head, so keep it on that existing joint and inverse bind transform.
        // Do not invent weights for other meshes: missing authoring data there
        // should remain visible to import validation.
        if (primitive.name.find("FacialsExtra_") != std::string::npos &&
            primitive.skinIndex >= 0)
        {
            const Engine::Model::ImportedSkin& skin =
                model.skins[static_cast<size_t>(primitive.skinIndex)];
            const auto head = std::find_if(skin.jointNodes.begin(), skin.jointNodes.end(),
                [&](unsigned node) { return model.nodes[node].name == "head.x"; });
            if (head != skin.jointNodes.end())
            {
                const unsigned paletteIndex = static_cast<unsigned>(
                    std::distance(skin.jointNodes.begin(), head));
                for (auto& vertexInfluences : influences)
                    if (std::none_of(vertexInfluences.begin(), vertexInfluences.end(),
                        [](const Influence& value) { return value.weight > 0.f; }))
                        vertexInfluences.push_back({ paletteIndex, 1.f });
            }
        }

        primitive.morphTargets.reserve(source->mNumAnimMeshes);
        for (unsigned morphIndex = 0; morphIndex < source->mNumAnimMeshes; ++morphIndex)
        {
            const aiAnimMesh* morph = source->mAnimMeshes[morphIndex];
            Engine::Model::MorphTarget target;
            target.name = Name(morph->mName,
                "Morph " + std::to_string(morphIndex + 1));
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
            // Assimp uses mWeight for the shape geometry's contribution to an
            // FBX blend-shape channel (often 1 for a single shape). It is not
            // the channel's initial expression weight in the imported model.
            primitive.morphWeights.push_back(0.f);
        }

        for (unsigned faceIndex = 0; faceIndex < source->mNumFaces; ++faceIndex)
        {
            const aiFace& face = source->mFaces[faceIndex];
            if (face.mNumIndices != 3) continue;
            for (unsigned corner = 0; corner < 3; ++corner)
            {
                const unsigned sourceIndex = face.mIndices[corner];
                Engine::Model::AnimationVertex vertex{};
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
                    // Assimp FBX UVs use a bottom-left origin; engine image
                    // uploads and glTF UVs use a top-left origin. Convert both
                    // sets here, leaving image pixels and other importers alone.
                    vertex.uv[0] = source->mTextureCoords[0][sourceIndex].x;
                    vertex.uv[1] = 1.f - source->mTextureCoords[0][sourceIndex].y;
                }
                if (source->HasTextureCoords(1))
                {
                    vertex.uv1[0] = source->mTextureCoords[1][sourceIndex].x;
                    vertex.uv1[1] = 1.f - source->mTextureCoords[1][sourceIndex].y;
                }
                if (source->HasTangentsAndBitangents())
                {
                    const aiVector3D& tangent = source->mTangents[sourceIndex];
                    vertex.tangent[0] = tangent.x; vertex.tangent[1] = tangent.y; vertex.tangent[2] = tangent.z;
                    const aiVector3D handed = source->mNormals[sourceIndex] ^ tangent;
                    vertex.tangent[3] = handed * source->mBitangents[sourceIndex] < 0.f ? -1.f : 1.f;
                    if (source->HasTextureCoords(0)) vertex.tangent[3] *= -1.f;
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
        for (Engine::Model::MorphTarget& target : primitive.morphTargets)
        {
            Engine::Model::MorphTarget expanded;
            expanded.name = target.name;
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
        // Assimp enumerates FBX blend-shape connections in an unstable order.
        // Keep mesh targets, their defaults, and animated weight indices in a
        // reproducible order so saved clips address the same expressions after
        // a fresh decode or import.
        auto& remap = morphIndexRemaps[meshIndex];
        remap.resize(primitive.morphTargets.size());
        std::vector<unsigned> order(remap.size());
        std::iota(order.begin(), order.end(), 0u);
        std::sort(order.begin(), order.end(), [&](unsigned a, unsigned b)
        {
            const auto& first = primitive.morphTargets[a];
            const auto& second = primitive.morphTargets[b];
            if (first.name != second.name) return first.name < second.name;
            const auto lessPosition = [](const glm::vec3& left,
                const glm::vec3& right)
            {
                if (left.x != right.x) return left.x < right.x;
                if (left.y != right.y) return left.y < right.y;
                return left.z < right.z;
            };
            return std::lexicographical_compare(first.positions.begin(),
                first.positions.end(), second.positions.begin(),
                second.positions.end(), lessPosition);
        });
        std::vector<Engine::Model::MorphTarget> sortedTargets;
        std::vector<float> sortedWeights;
        sortedTargets.reserve(order.size());
        sortedWeights.reserve(order.size());
        for (unsigned sorted = 0; sorted < order.size(); ++sorted)
        {
            const unsigned original = order[sorted];
            remap[original] = sorted;
            sortedTargets.push_back(std::move(primitive.morphTargets[original]));
            sortedWeights.push_back(primitive.morphWeights[original]);
        }
        primitive.morphTargets = std::move(sortedTargets);
        primitive.morphWeights = std::move(sortedWeights);
    }

    model.animations.reserve(scene->mNumAnimations);
    for (unsigned animationIndex = 0; animationIndex < scene->mNumAnimations; ++animationIndex)
    {
        const aiAnimation* source = scene->mAnimations[animationIndex];
        Engine::Model::ImportedAnimation animation;
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
                    Engine::Model::AnimationChannel::Path::Translation, sourceChannel->mPositionKeys,
                    sourceChannel->mNumPositionKeys, ticksPerSecond));
            if (sourceChannel->mNumScalingKeys)
                animation.channels.push_back(VectorChannel(found->second,
                    Engine::Model::AnimationChannel::Path::Scale, sourceChannel->mScalingKeys,
                    sourceChannel->mNumScalingKeys, ticksPerSecond));
            if (sourceChannel->mNumRotationKeys)
            {
                Engine::Model::AnimationChannel channel;
                channel.nodeIndex = found->second;
                channel.path = Engine::Model::AnimationChannel::Path::Rotation;
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
            Engine::Model::AnimationChannel channel;
            channel.nodeIndex = meshOwnerNode[meshIndex];
            channel.path = Engine::Model::AnimationChannel::Path::Weights;
            channel.valueWidth = width;
            channel.times.reserve(sourceChannel->mNumKeys);
            channel.values.assign(static_cast<size_t>(sourceChannel->mNumKeys) * width, 0.f);
            for (unsigned keyIndex = 0; keyIndex < sourceChannel->mNumKeys; ++keyIndex)
            {
                const aiMeshMorphKey& key = sourceChannel->mKeys[keyIndex];
                channel.times.push_back(static_cast<float>(key.mTime / ticksPerSecond));
                for (unsigned valueIndex = 0; valueIndex < key.mNumValuesAndWeights; ++valueIndex)
                    if (key.mValues[valueIndex] < width)
                        channel.values[static_cast<size_t>(keyIndex) * width +
                            morphIndexRemaps[meshIndex][key.mValues[valueIndex]]] =
                            static_cast<float>(key.mWeights[valueIndex]);
            }
            animation.channels.push_back(std::move(channel));
        }
        model.animations.push_back(std::move(animation));
    }

    // Preserve authored mesh-node transforms. A scale of 100 can convert
    // meter vertices into the skeleton's centimeter space; its rotation and
    // translation also place each mesh in the shared bind pose. Bone offsets
    // near identity do not prove that this mesh transform is redundant.

    // The runtime stores decomposed TRS nodes. Rebuild inverse binds against
    // those exact rest transforms so every joint yields the same mesh bind
    // pose, including FBX bones whose original matrices contained transform
    // inheritance that decomposition cannot represent exactly.
    std::vector<glm::mat4> runtimeWorld(model.nodes.size(), glm::mat4(1.f));
    for (size_t index = 0; index < model.nodes.size(); ++index)
    {
        const auto& node = model.nodes[index];
        const glm::mat4 local = glm::translate(glm::mat4(1.f), node.translation) *
            glm::mat4_cast(node.rotation) * glm::scale(glm::mat4(1.f), node.scale);
        runtimeWorld[index] = node.parent < 0 ? local :
            runtimeWorld[static_cast<size_t>(node.parent)] * local;
    }
    for (size_t nodeIndex = 0; nodeIndex < model.nodes.size(); ++nodeIndex)
        for (unsigned primitiveIndex : model.nodes[nodeIndex].primitives)
        {
            const int skinIndex = model.primitives[primitiveIndex].skinIndex;
            if (skinIndex < 0) continue;
            auto& skin = model.skins[static_cast<size_t>(skinIndex)];
            for (size_t joint = 0; joint < skin.jointNodes.size(); ++joint)
            {
                const glm::mat4& jointWorld = runtimeWorld[skin.jointNodes[joint]];
                const float determinant = glm::determinant(jointWorld);
                if (!std::isfinite(determinant) || std::abs(determinant) < 1e-8f)
                {
                    error = "FBX joint has a non-invertible bind transform";
                    return false;
                }
                skin.inverseBindMatrices[joint] =
                    glm::inverse(jointWorld) * runtimeWorld[nodeIndex];
            }
        }

    // Assimp presents many FBX meshes with their own copy of the same bone
    // palette. Once their runtime bind matrices are final, share identical
    // palettes without changing the per-vertex palette indices.
    std::vector<Engine::Model::ImportedSkin> distinctSkins;
    std::vector<int> remap(model.skins.size(), -1);
    for (size_t sourceIndex = 0; sourceIndex < model.skins.size(); ++sourceIndex)
    {
        const auto& sourceSkin = model.skins[sourceIndex];
        for (size_t targetIndex = 0; targetIndex < distinctSkins.size(); ++targetIndex)
        {
            const auto& targetSkin = distinctSkins[targetIndex];
            if (sourceSkin.jointNodes != targetSkin.jointNodes ||
                sourceSkin.inverseBindMatrices.size() !=
                    targetSkin.inverseBindMatrices.size()) continue;
            bool identical = true;
            for (size_t joint = 0; identical &&
                joint < sourceSkin.inverseBindMatrices.size(); ++joint)
                for (int column = 0; identical && column < 4; ++column)
                    for (int row = 0; row < 4; ++row)
                        identical &= sourceSkin.inverseBindMatrices[joint][column][row] ==
                            targetSkin.inverseBindMatrices[joint][column][row];
            if (identical)
            {
                remap[sourceIndex] = static_cast<int>(targetIndex);
                break;
            }
        }
        if (remap[sourceIndex] < 0)
        {
            remap[sourceIndex] = static_cast<int>(distinctSkins.size());
            distinctSkins.push_back(sourceSkin);
        }
    }
    for (auto& primitive : model.primitives)
        if (primitive.skinIndex >= 0)
            primitive.skinIndex = remap[static_cast<size_t>(primitive.skinIndex)];
    model.skins = std::move(distinctSkins);
    return true;
}
}

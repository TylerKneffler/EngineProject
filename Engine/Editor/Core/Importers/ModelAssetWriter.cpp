#include "ModelAssetWriter.h"
#include "Core/AnimationClipMapping.h"

#include "Core/AssetRecord.h"
#include "Core/Compoonents/Materials/Material.h"
#include "Core/Compoonents/Materials/MaterialPropertyMorph.h"
#include "Core/Compoonents/Animation/ModelAnimation.h"
#include "Core/Compoonents/Animation/BoneGroup.h"
#include "Core/Compoonents/Animation/RigConstraints.h"
#include "Core/Compoonents/Animation/RigMorphControls.h"
#include "Core/Compoonents/Physics/RigPhysicsBinding.h"
#include "Core/Compoonents/Animation/IKBone.h"
#include "Core/Compoonents/Camera/Camera.h"
#include "Core/Compoonents/Lighting/Light.h"
#include "Core/Compoonents/Physics/PrimitiveObjectCollider.h"
#include "Core/Compoonents/Physics/MeshObjectCollider.h"
#include "Core/Compoonents/Physics/RigidBody.h"
#include "Core/Compoonents/Physics/Cloth.h"
#include "Core/Model/RigAsset.h"
#include "Core/Model/PoseAsset.h"
#include "AnimationAssetWriter.h"
#include "Core/Object.h"
#include "Core/Scene/Scene.h"
#include "Core/Serialization/SceneSerializer.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <limits>
#include <optional>
#include <set>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace Engine::Editor
{
namespace fs = std::filesystem;

namespace
{
std::string SafeName(std::string name, const std::string& fallback)
{
    if (name.empty()) name = fallback;
    for (char& c : name)
    {
        const unsigned char value = static_cast<unsigned char>(c);
        if (value < 32 || c == '<' || c == '>' || c == ':' || c == '"' ||
            c == '/' || c == '\\' || c == '|' || c == '?' || c == '*') c = '_';
    }
    while (!name.empty() && (name.back() == ' ' || name.back() == '.')) name.pop_back();
    return name.empty() ? fallback : name;
}

fs::path UniqueDirectory(const fs::path& assets, const std::string& base)
{
    fs::path result = assets / base;
    for (unsigned suffix = 2; fs::exists(result); ++suffix)
        result = assets / (base + " " + std::to_string(suffix));
    return result;
}

struct IndexedPrimitiveAsset
{
    std::vector<Engine::Model::AnimationVertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<Engine::Model::MorphTarget> morphTargets;
};

template<class T>
bool CompactOffsets(const std::vector<T>& source,
    const std::vector<uint32_t>& representatives,
    const std::vector<uint32_t>& indices, std::vector<T>& result)
{
    if (source.empty()) return true;
    if (source.size() != indices.size()) return false;
    for (size_t old = 0; old < source.size(); ++old)
        if (std::memcmp(&source[old], &source[representatives[indices[old]]],
                sizeof(T)) != 0)
            return false;
    result.reserve(representatives.size());
    for (uint32_t representative : representatives)
        result.push_back(source[representative]);
    return true;
}

std::optional<IndexedPrimitiveAsset> IndexPmxPrimitive(
    const Engine::Model::ImportedPrimitive& primitive)
{
    if (primitive.vertices.size() < 6 || primitive.vertices.size() % 3)
        return std::nullopt;
    IndexedPrimitiveAsset result;
    result.vertices.reserve(primitive.vertices.size());
    result.indices.reserve(primitive.vertices.size());
    std::vector<uint32_t> representatives;
    std::unordered_map<std::string, uint32_t> byVertex;
    byVertex.reserve(primitive.vertices.size());
    for (size_t old = 0; old < primitive.vertices.size(); ++old)
    {
        const auto& vertex = primitive.vertices[old];
        std::string key(reinterpret_cast<const char*>(&vertex), sizeof(vertex));
        const auto [found, inserted] = byVertex.emplace(std::move(key),
            static_cast<uint32_t>(result.vertices.size()));
        if (inserted)
        {
            result.vertices.push_back(vertex);
            representatives.push_back(static_cast<uint32_t>(old));
        }
        result.indices.push_back(found->second);
    }
    if (result.vertices.size() == primitive.vertices.size())
        return std::nullopt;
    for (const auto& target : primitive.morphTargets)
    {
        Engine::Model::MorphTarget compact;
        compact.name = target.name;
        if (!CompactOffsets(target.positions, representatives, result.indices,
                compact.positions) ||
            !CompactOffsets(target.normals, representatives, result.indices,
                compact.normals) ||
            !CompactOffsets(target.tangents, representatives, result.indices,
                compact.tangents) ||
            !CompactOffsets(target.uv, representatives, result.indices,
                compact.uv))
            return std::nullopt;
        for (size_t channel = 0; channel < compact.uvChannels.size(); ++channel)
            if (!CompactOffsets(target.uvChannels[channel], representatives,
                    result.indices, compact.uvChannels[channel]))
                return std::nullopt;
        result.morphTargets.push_back(std::move(compact));
    }
    return result;
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

Engine::Core::Object* AddChild(Engine::Scene::Scene& scene, Engine::Core::Object* parent, const std::string& name)
{
    Engine::Core::Object* child = scene.AddObject(name);
    child->Parent = parent;
    parent->Children.push_back(child);
    return child;
}

// PMX rigid bodies are separate from render meshes. Gather rest-pose points
// across material sections, then save a small convex point cloud in body space.
std::vector<std::string> BuildWeightFittedColliderMeshes(
    const Engine::Model::ImportedModel& model, const fs::path& meshesDirectory,
    const fs::path& source)
{
    std::vector<std::string> paths(model.rigidBodies.size());
    if (model.rigidBodies.empty() || model.skins.empty()) return paths;

    std::vector<glm::mat4> nodeWorld(model.nodes.size(), glm::mat4(1.f));
    for (size_t i = 0; i < model.nodes.size(); ++i)
    {
        const auto& node = model.nodes[i];
        const glm::mat4 local = glm::translate(glm::mat4(1.f), node.translation) *
            glm::mat4_cast(node.rotation) *
            glm::scale(glm::mat4(1.f), node.scale);
        nodeWorld[i] = node.parent >= 0 &&
            static_cast<size_t>(node.parent) < i
            ? nodeWorld[static_cast<size_t>(node.parent)] * local : local;
    }
    std::vector<glm::mat4> primitiveWorld(model.primitives.size(), glm::mat4(1.f));
    for (size_t node = 0; node < model.nodes.size(); ++node)
        for (unsigned primitive : model.nodes[node].primitives)
            if (primitive < primitiveWorld.size())
                primitiveWorld[primitive] = nodeWorld[node];

    std::unordered_set<int> softBodyMaterials;
    for (const auto& softBody : model.softBodies)
        softBodyMaterials.insert(softBody.materialIndex);
    std::unordered_map<int, std::vector<glm::vec3>> pointsByBone;
    for (size_t primitiveIndex = 0; primitiveIndex < model.primitives.size(); ++primitiveIndex)
    {
        const auto& primitive = model.primitives[primitiveIndex];
        if (primitive.skinIndex < 0 ||
            softBodyMaterials.count(primitive.materialIndex) ||
            static_cast<size_t>(primitive.skinIndex) >= model.skins.size()) continue;
        const auto& joints = model.skins[static_cast<size_t>(primitive.skinIndex)].jointNodes;
        for (const auto& vertex : primitive.vertices)
        {
            int strongest = -1;
            float maximum = 0.f;
            const auto visit = [&](const float* indices, const float* weights,
                int slot, int& best, float& bestWeight)
            {
                if (weights[slot] <= bestWeight || !std::isfinite(weights[slot])) return;
                const int palette = static_cast<int>(indices[slot]);
                if (palette < 0 || static_cast<size_t>(palette) >= joints.size()) return;
                best = palette;
                bestWeight = weights[slot];
            };
            for (int slot = 0; slot < 4; ++slot)
            {
                visit(vertex.joints0, vertex.weights0, slot, strongest, maximum);
                visit(vertex.joints1, vertex.weights1, slot, strongest, maximum);
            }
            if (strongest < 0 || maximum < 0.15f) continue;
            const glm::vec3 point = glm::vec3(primitiveWorld[primitiveIndex] *
                glm::vec4(vertex.pos[0], vertex.pos[1], vertex.pos[2], 1.f));
            if (!std::isfinite(point.x) || !std::isfinite(point.y) ||
                !std::isfinite(point.z)) continue;
            pointsByBone[static_cast<int>(joints[strongest])].push_back(point);
            // A narrow shared band keeps adjacent hulls touching at blended joints.
            const auto addSecondary = [&](const float* indices, const float* weights)
            {
                for (int slot = 0; slot < 4; ++slot)
                {
                    const float weight = weights[slot];
                    const int palette = static_cast<int>(indices[slot]);
                    if (palette == strongest || weight < 0.35f ||
                        weight < maximum * 0.75f || palette < 0 ||
                        static_cast<size_t>(palette) >= joints.size()) continue;
                    pointsByBone[static_cast<int>(joints[palette])].push_back(point);
                }
            };
            addSecondary(vertex.joints0, vertex.weights0);
            addSecondary(vertex.joints1, vertex.weights1);
        }
    }

    std::vector<glm::mat4> bodyWorld(model.rigidBodies.size(), glm::mat4(1.f));
    std::unordered_map<int, std::vector<size_t>> bodiesByBone;
    for (size_t i = 0; i < model.rigidBodies.size(); ++i)
    {
        const auto& body = model.rigidBodies[i];
        const int bone = body.relatedBoneNode >= 0
            ? body.relatedBoneNode : body.parentNode;
        if (bone < 0 || static_cast<size_t>(bone) >= nodeWorld.size()) continue;
        bodiesByBone[bone].push_back(i);
        const glm::mat4 local = glm::translate(glm::mat4(1.f),
            body.localPosition) * glm::mat4_cast(body.localRotation);
        bodyWorld[i] = body.parentNode >= 0 &&
            static_cast<size_t>(body.parentNode) < nodeWorld.size()
            ? nodeWorld[static_cast<size_t>(body.parentNode)] * local : local;
    }
    for (const auto& [bone, bodyIndices] : bodiesByBone)
    {
        const auto found = pointsByBone.find(bone);
        if (found == pointsByBone.end()) continue;
        for (size_t bodyIndex : bodyIndices)
        {
            const glm::mat4 toBody = glm::inverse(bodyWorld[bodyIndex]);
            std::vector<glm::vec3> localPoints;
            localPoints.reserve(found->second.size());
            for (const glm::vec3& worldPoint : found->second)
            {
                if (bodyIndices.size() > 1)
                {
                    size_t nearest = bodyIndices.front();
                    float nearestDistance = std::numeric_limits<float>::max();
                    for (size_t candidate : bodyIndices)
                    {
                        const glm::vec3 delta = worldPoint -
                            glm::vec3(bodyWorld[candidate][3]);
                        const float distance = glm::dot(delta, delta);
                        if (distance < nearestDistance)
                        { nearest = candidate; nearestDistance = distance; }
                    }
                    if (nearest != bodyIndex) continue;
                }
                localPoints.push_back(glm::vec3(toBody * glm::vec4(worldPoint, 1.f)));
            }
            if (localPoints.size() < 4) continue;
            // Directional support points retain the convex envelope while
            // bounding the generated asset and Bullet hull build cost.
            std::set<size_t> extremes;
            for (int x = -3; x <= 3; ++x)
                for (int y = -3; y <= 3; ++y)
                    for (int z = -3; z <= 3; ++z)
                    {
                        if (!x && !y && !z) continue;
                        const glm::vec3 direction(x, y, z);
                        size_t best = 0;
                        float maximum = -std::numeric_limits<float>::max();
                        for (size_t index = 0; index < localPoints.size(); ++index)
                        {
                            const float projection = glm::dot(localPoints[index], direction);
                            if (projection > maximum)
                            { best = index; maximum = projection; }
                        }
                        extremes.insert(best);
                    }
            if (extremes.size() < 4) continue;
            const glm::vec3 first = localPoints[*extremes.begin()];
            glm::vec3 low = first, high = first;
            for (size_t index : extremes)
            { low = glm::min(low, localPoints[index]); high = glm::max(high, localPoints[index]); }
            const float extent = glm::length(high - low);
            if (extent < 1e-5f) continue;
            size_t second = *extremes.begin(), third = second;
            float farthest = 0.f, largestArea = 0.f, largestVolume = 0.f;
            for (size_t index : extremes)
            {
                const glm::vec3 delta = localPoints[index] - first;
                const float distance = glm::dot(delta, delta);
                if (distance > farthest) { second = index; farthest = distance; }
            }
            for (size_t index : extremes)
            {
                const glm::vec3 area = glm::cross(localPoints[second] - first,
                    localPoints[index] - first);
                const float size = glm::dot(area, area);
                if (size > largestArea) { third = index; largestArea = size; }
            }
            const glm::vec3 normal = glm::cross(localPoints[second] - first,
                localPoints[third] - first);
            for (size_t index : extremes)
                largestVolume = std::max(largestVolume,
                    std::abs(glm::dot(normal, localPoints[index] - first)));
            if (farthest < extent * extent * 1e-8f ||
                largestArea < std::pow(extent, 4.f) * 1e-10f ||
                largestVolume < std::pow(extent, 3.f) * 1e-6f) continue;

            std::vector<Engine::Model::AnimationVertex> vertices;
            vertices.reserve(extremes.size() + 2);
            for (size_t index : extremes)
            {
                Engine::Model::AnimationVertex vertex {};
                const glm::vec3& point = localPoints[index];
                vertex.pos[0] = point.x;
                vertex.pos[1] = point.y;
                vertex.pos[2] = point.z;
                vertex.normal[1] = 1.f;
                vertex.tangent[0] = vertex.tangent[3] = 1.f;
                vertices.push_back(vertex);
            }
            while (vertices.size() % 3u) vertices.push_back(vertices.front());
            const fs::path path = meshesDirectory /
                (SafeName(model.rigidBodies[bodyIndex].name, "Body") +
                " Collider " + std::to_string(bodyIndex) + ".mesh");
            if (!Engine::Components::Mesh::SaveNativeFile(path.string(), vertices))
                throw std::runtime_error("Could not save fitted collider: " + path.string());
            Engine::Core::AssetRecord::Ensure(path, source,
                { { "importer", std::string("pmx-weight-fitted-collider") },
                  { "bodyIndex", static_cast<double>(bodyIndex) } });
            paths[bodyIndex] = path.generic_string();
        }
    }
    return paths;
}
}

Engine::Model::ModelImportResult ModelAssetWriter::Write(const Engine::Model::ImportedModel& model,
    const std::string& sourcePath, const std::string& assetsDirectory,
    bool weightFittedColliders)
{
    Engine::Model::ModelImportResult result;
    const fs::path source = fs::path(sourcePath).lexically_normal();
    const fs::path assets = fs::path(assetsDirectory).lexically_normal();
    fs::path output;
    try
    {
        const std::string sourceName = SafeName(model.name, SafeName(source.stem().string(), "Model"));
        output = UniqueDirectory(assets, sourceName);
        const fs::path meshesDirectory = output / "Meshes";
        const fs::path materialsDirectory = output / "Materials";
        const fs::path texturesDirectory = output / "Textures";
        fs::create_directories(meshesDirectory);
        fs::create_directories(materialsDirectory);
        fs::create_directories(texturesDirectory);

        std::unordered_map<std::string, std::string> importedTextures;
        auto importTexture = [&](const std::string& value) -> std::string
        {
            if (value.empty()) return {};
            if (const auto found = importedTextures.find(value); found != importedTextures.end())
                return found->second;
            if (value.front() == '*')
            {
                const auto embedded = std::find_if(model.textures.begin(), model.textures.end(),
                    [&](const Engine::Model::ImportedTexture& texture) { return texture.source == value; });
                if (embedded == model.textures.end() || embedded->bytes.empty()) return {};
                const fs::path destination = texturesDirectory /
                    SafeName(embedded->name, "Embedded Texture.bin");
                std::ofstream file(destination, std::ios::binary);
                file.write(reinterpret_cast<const char*>(embedded->bytes.data()),
                    static_cast<std::streamsize>(embedded->bytes.size()));
                if (!file.good()) throw std::runtime_error("Could not write embedded texture");
                Engine::Core::AssetRecord::Ensure(destination, source,
                    { { "importer", std::string("model-texture") },
                      { "sourceFormat", model.sourceFormat } });
                return importedTextures.emplace(value, destination.generic_string()).first->second;
            }
            fs::path original(value);
            if (original.is_relative()) original = source.parent_path() / original;
            if (!fs::exists(original)) return {};
            // An existing project asset can be referenced directly. Copying it
            // into every imported prefab only duplicates the same image bytes.
            std::error_code pathError;
            const auto relativeToAssets = fs::weakly_canonical(original,
                pathError).lexically_relative(fs::weakly_canonical(assets));
            if (!pathError && !relativeToAssets.empty() &&
                *relativeToAssets.begin() != "..")
                return importedTextures.emplace(value,
                    original.lexically_normal().generic_string()).first->second;
            fs::path destination = texturesDirectory / original.filename();
            for (unsigned suffix = 2; fs::exists(destination); ++suffix)
                destination = texturesDirectory /
                    (original.stem().string() + " " + std::to_string(suffix) + original.extension().string());
            fs::copy_file(original, destination);
            Engine::Core::AssetRecord::Ensure(destination, source,
                { { "importer", std::string("model-texture") },
                  { "sourceFormat", model.sourceFormat } });
            return importedTextures.emplace(value, destination.generic_string()).first->second;
        };

        std::vector<std::string> materialPaths;
        materialPaths.reserve(model.materials.size() + 1);
        for (size_t index = 0; index < model.materials.size(); ++index)
        {
            const Engine::Model::ImportedMaterial& imported = model.materials[index];
            Engine::Components::Material material;
            material.diffuseColor = glm::vec3(imported.baseColor);
            material.baseColorAlpha = imported.baseColor.a;
            material.ambientColor = imported.ambientColor;
            material.specularColor = imported.specularColor;
            material.shininess = imported.shininess;
            material.specularModel = imported.specularModel;
            material.edgeColor = imported.edgeColor;
            material.edgeWidth = imported.edgeWidth;
            material.edgeEnabled = imported.edgeEnabled;
            material.sphereMode = imported.sphereMode;
            material.builtinToon = imported.builtinToon;
            material.SetSphereTexture(importTexture(imported.sphereTexture));
            material.SetToonTexture(importTexture(imported.toonTexture));
            material.emissiveColor = imported.emissiveColor;
            material.metallicFactor = imported.metallic;
            material.roughnessFactor = imported.roughness;
            material.alphaCutoff = imported.alphaCutoff;
            material.normalScale = imported.normalScale;
            material.doubleSided = imported.doubleSided;
            material.castsShadows = imported.castsShadows;
            material.unlit = imported.unlit;
            material.alphaMode = imported.alphaMode;
            material.baseColorUvSet = imported.baseColorUvSet;
            material.metallicRoughnessUvSet = imported.metallicRoughnessUvSet;
            material.normalUvSet = imported.normalUvSet;
            material.occlusionUvSet = imported.occlusionUvSet;
            material.emissiveUvSet = imported.emissiveUvSet;
            material.SetBaseColorTexture(importTexture(imported.baseColorTexture));
            material.SetMetallicRoughnessTexture(importTexture(imported.metallicRoughnessTexture));
            material.SetNormalTexture(importTexture(imported.normalTexture));
            material.SetOcclusionTexture(importTexture(imported.occlusionTexture));
            material.SetEmissiveTexture(importTexture(imported.emissiveTexture));
            const fs::path path = materialsDirectory /
                (SafeName(imported.name, "Material") + " " + std::to_string(index + 1) + ".material");
            if (!material.SaveToFile(path.string())) throw std::runtime_error("Could not save material");
            Engine::Core::AssetRecord::Ensure(path, source,
                { { "importer", std::string("model-material") },
                  { "sourceFormat", model.sourceFormat },
                  { "materialIndex", static_cast<double>(index) } });
            materialPaths.push_back(path.generic_string());
        }
        Engine::Components::Material defaultMaterial;
        const fs::path defaultMaterialPath = materialsDirectory / "Default.material";
        if (!defaultMaterial.SaveToFile(defaultMaterialPath.string()))
            throw std::runtime_error("Could not save default material");

        std::vector<std::string> meshPaths(model.primitives.size());
        std::vector<std::string> morphPaths(model.primitives.size());
        const std::vector<uint32_t> noIndices;
        for (size_t index = 0; index < model.primitives.size(); ++index)
        {
            const Engine::Model::ImportedPrimitive& primitive = model.primitives[index];
            if (primitive.vertices.empty()) continue;
            const auto indexed = model.sourceFormat == "pmx"
                ? IndexPmxPrimitive(primitive)
                : std::optional<IndexedPrimitiveAsset>{};
            const auto& vertices = indexed ? indexed->vertices : primitive.vertices;
            const auto& indices = indexed ? indexed->indices : noIndices;
            const auto& morphTargets = indexed ? indexed->morphTargets :
                primitive.morphTargets;
            const fs::path path = meshesDirectory /
                (SafeName(primitive.name, "Mesh") + " " + std::to_string(index + 1) + ".mesh");
            if (!Engine::Components::Mesh::SaveNativeFile(path.string(),
                    vertices, indices))
                throw std::runtime_error("Could not save mesh: " + path.string());
            Engine::Core::AssetRecord::Ensure(path, source,
                { { "importer", std::string("model-mesh") },
                  { "sourceFormat", model.sourceFormat },
                  { "primitiveIndex", static_cast<double>(index) } });
            meshPaths[index] = path.generic_string();
            if (!morphTargets.empty())
            {
                const fs::path morphPath = meshesDirectory /
                    (SafeName(primitive.name, "Mesh") + " " +
                     std::to_string(index + 1) + ".morph");
                if (!Engine::Components::Mesh::SaveMorphFile(
                        morphPath.string(), morphTargets))
                    throw std::runtime_error("Could not save morph data: " +
                        morphPath.string());
                morphPaths[index] = morphPath.generic_string();
                Engine::Core::AssetRecord::Ensure(morphPath, source,
                    { { "importer", std::string("model-morph") },
                      { "primitiveIndex", static_cast<double>(index) } });
            }
        }

        const std::vector<std::string> fittedColliderPaths =
            weightFittedColliders && model.sourceFormat == "pmx"
            ? BuildWeightFittedColliderMeshes(model, meshesDirectory, source)
            : std::vector<std::string>(model.rigidBodies.size());

        ::Engine::Scene::Scene scene;
        Engine::Core::Object* root = scene.AddObject(sourceName);
        root->transform.scale = glm::vec3(model.unitScaleToMeters);
        for (size_t index = 0; index < model.skins.size(); ++index)
        {
            Engine::Components::Skeleton* skeleton =
                root->AddComponent<Engine::Components::Skeleton>();
            skeleton->skinIndex = static_cast<unsigned>(index);
            skeleton->jointNodes = model.skins[index].jointNodes;
            skeleton->inverseBindMatrices = model.skins[index].inverseBindMatrices;
        }
        if (!model.skins.empty())
        {
            auto* rootSkeleton = root->GetComponent<Engine::Components::Skeleton>();
            for (const auto& skin : model.skins)
                for (unsigned node : skin.jointNodes)
                    if (std::find(rootSkeleton->rootBoneNodes.begin(),
                            rootSkeleton->rootBoneNodes.end(), node) ==
                        rootSkeleton->rootBoneNodes.end())
                        rootSkeleton->rootBoneNodes.push_back(node);
        }
        if (!model.animations.empty())
        {
            Engine::Components::AnimationManager* manager = root->AddComponent<Engine::Components::AnimationManager>();
            manager->clip = model.animations.front().name;
            manager->clips.reserve(model.animations.size());
            for (const Engine::Model::ImportedAnimation& imported : model.animations)
            {
                Engine::Model::AnimationClip animation;
                animation.clipName = imported.name;
                animation.duration = imported.duration;
                animation.channels = imported.channels;
                manager->clips.push_back(std::move(animation));
            }
        }

        ::Engine::Components::Model* modelComponent =
            root->AddComponent<::Engine::Components::Model>();
        if (!model.appendRules.empty() || !model.ikChains.empty() ||
            !model.fixedAxes.empty() || !model.localAxes.empty() ||
            !model.externalParents.empty())
        {
            auto* controller = root->AddComponent<Engine::Components::RigConstraints>();
            controller->appendRules = model.appendRules;
            controller->ikChains = model.ikChains;
            controller->fixedAxes = model.fixedAxes;
            controller->localAxes = model.localAxes;
            controller->externalParents = model.externalParents;
        }
        Engine::Components::RigMorphControls* morphControls = nullptr;
        if (!model.boneMorphs.empty() || !model.impulseMorphs.empty())
        {
            morphControls = root->AddComponent<Engine::Components::RigMorphControls>();
            morphControls->boneMorphs = model.boneMorphs;
            morphControls->impulseMorphs = model.impulseMorphs;
        }
        Engine::Components::RigPhysicsBinding* physicsBinding = nullptr;
        if (!model.physicsJoints.empty() ||
            std::any_of(model.rigidBodies.begin(), model.rigidBodies.end(),
                [](const auto& body) { return body.mode !=
                    Engine::Model::RigidBodyMotion::FollowTransform; }))
        {
            physicsBinding = root->AddComponent<Engine::Components::RigPhysicsBinding>();
            physicsBinding->physicsJoints = model.physicsJoints;
        }
        std::vector<Engine::Core::Object*> objects(model.nodes.size(), nullptr);
        std::unordered_map<int, Engine::Core::Object*> materialObjects;
        std::unordered_set<int> softBodyMaterials;
        for (const auto& soft : model.softBodies)
            softBodyMaterials.insert(soft.materialIndex);
        for (size_t index = 0; index < model.nodes.size(); ++index)
        {
            const Engine::Model::ImportedNode& imported = model.nodes[index];
            Engine::Core::Object* parent = imported.parent >= 0 && static_cast<size_t>(imported.parent) < objects.size()
                ? objects[static_cast<size_t>(imported.parent)] : root;
            Engine::Core::Object* object = AddChild(scene, parent,
                SafeName(imported.name, "Node " + std::to_string(index + 1)));
            objects[index] = object;
            modelComponent->BindNode(static_cast<unsigned>(index), object);
            object->transform.position = imported.translation;
            object->transform.rotation = QuaternionEuler(imported.rotation);
            object->transform.scale = imported.scale;

            std::unordered_map<std::string, unsigned> primitiveNameCounts;
            for (size_t slot = 0; slot < imported.primitives.size(); ++slot)
            {
                const unsigned primitiveIndex = imported.primitives[slot];
                if (primitiveIndex >= model.primitives.size() || meshPaths[primitiveIndex].empty()) continue;
                const Engine::Model::ImportedPrimitive& primitive = model.primitives[primitiveIndex];
                std::string primitiveObjectName = SafeName(primitive.name,
                    "Primitive " + std::to_string(slot + 1));
                const unsigned sameName = ++primitiveNameCounts[primitiveObjectName];
                if (sameName > 1)
                    primitiveObjectName += " " + std::to_string(sameName);
                Engine::Core::Object* target = imported.primitives.size() == 1 ? object :
                    AddChild(scene, object, primitiveObjectName);
                if (primitive.materialIndex >= 0)
                    materialObjects.emplace(primitive.materialIndex, target);
                Engine::Components::Mesh* mesh = target->AddComponent<Engine::Components::Mesh>();
                mesh->LoadFromFile(meshPaths[primitiveIndex]);
                Engine::Components::Material* material = target->AddComponent<Engine::Components::Material>();
                const std::string materialPath = primitive.materialIndex >= 0 &&
                    static_cast<size_t>(primitive.materialIndex) < materialPaths.size()
                    ? materialPaths[static_cast<size_t>(primitive.materialIndex)]
                    : defaultMaterialPath.generic_string();
                if (!material->LoadFromFile(materialPath))
                    throw std::runtime_error("Could not load generated material");
                if (primitive.materialIndex >= 0 && !model.materialMorphs.empty())
                {
                    std::vector<Engine::Model::ImportedMaterialMorph> applicable;
                    for (const auto& morph : model.materialMorphs)
                    {
                        Engine::Model::ImportedMaterialMorph selected;
                        selected.name = morph.name;
                        for (const auto& offset : morph.offsets)
                            if (offset.materialIndex == -1 ||
                                offset.materialIndex == primitive.materialIndex)
                                selected.offsets.push_back(offset);
                        if (!selected.offsets.empty())
                            applicable.push_back(std::move(selected));
                    }
                    if (!applicable.empty())
                    {
                        auto* controller = target->AddComponent<
                            Engine::Components::MaterialPropertyMorph>();
                        controller->morphs = std::move(applicable);
                    }
                }
                if (!primitive.morphTargets.empty())
                    if (!mesh->LoadMorphFile(morphPaths[primitiveIndex],
                            static_cast<unsigned>(index), primitive.morphWeights))
                        throw std::runtime_error("Could not load generated morph data");
                if ((primitive.skinIndex >= 0 || !primitive.morphTargets.empty()) &&
                    softBodyMaterials.find(primitive.materialIndex) ==
                        softBodyMaterials.end())
                {
                    Engine::Components::SkinnedMesh* deformer = target->AddComponent<Engine::Components::SkinnedMesh>();
                    deformer->skinIndex = primitive.skinIndex;
                    deformer->bindMeshToModel = primitive.bindMeshToModel;
                }
            }
        }

        for (const auto& imported : model.cameras)
        {
            if (imported.nodeIndex >= objects.size()) continue;
            Engine::Core::Object* object = AddChild(scene, objects[imported.nodeIndex], "Camera");
            object->transform.position = imported.position;
            object->transform.rotation = QuaternionEuler(imported.rotation);
            auto* camera = object->AddComponent<Engine::Components::Camera>();
            camera->fov = std::clamp(imported.verticalFov, 1.f, 179.f);
            camera->nearPlane = std::max(imported.nearPlane, 0.001f);
            camera->farPlane = std::max(imported.farPlane, camera->nearPlane + 0.001f);
            camera->orthographic = imported.orthographic;
            camera->orthographicSize = std::max(imported.orthographicSize, 0.01f);
        }
        for (const auto& imported : model.lights)
        {
            if (imported.nodeIndex >= objects.size()) continue;
            Engine::Core::Object* object = AddChild(scene, objects[imported.nodeIndex], "Light");
            object->transform.position = imported.position;
            object->transform.rotation = QuaternionEuler(imported.rotation);
            auto* light = object->AddComponent<Engine::Components::Light>();
            light->lightType = imported.type == Engine::Model::ImportedLight::Type::Directional
                ? static_cast<int>(Engine::Components::Light::Type::Directional)
                : imported.type == Engine::Model::ImportedLight::Type::Spot
                    ? static_cast<int>(Engine::Components::Light::Type::Spot)
                    : static_cast<int>(Engine::Components::Light::Type::Point);
            light->color = imported.color;
            light->intensity = imported.intensity;
            light->range = imported.range;
            light->innerConeAngle = std::clamp(imported.innerConeAngle, 0.f, 89.f);
            light->outerConeAngle = std::clamp(imported.outerConeAngle, 0.1f, 89.f);
        }

        // Mark skeletal nodes after the complete Object hierarchy exists.
        // Palette order remains on Skeleton; AnimationBone parentage follows imported
        // node ancestry, including non-joint helper nodes between bones.
        for (size_t skinIndex = 0; skinIndex < model.skins.size(); ++skinIndex)
        {
            const std::vector<unsigned>& joints =
                model.skins[skinIndex].jointNodes;
            std::unordered_map<unsigned, int> paletteByNode;
            for (size_t palette = 0; palette < joints.size(); ++palette)
                paletteByNode[joints[palette]] = static_cast<int>(palette);
            for (size_t palette = 0; palette < joints.size(); ++palette)
            {
                const unsigned nodeIndex = joints[palette];
                if (nodeIndex >= objects.size() || !objects[nodeIndex])
                    continue;
                int parentPalette = -1;
                int parent = model.nodes[nodeIndex].parent;
                while (parent >= 0 &&
                    static_cast<size_t>(parent) < model.nodes.size())
                {
                    const auto found = paletteByNode.find(
                        static_cast<unsigned>(parent));
                    if (found != paletteByNode.end())
                    {
                        parentPalette = found->second;
                        break;
                    }
                    parent = model.nodes[static_cast<size_t>(parent)].parent;
                }
                Engine::Components::AnimationBone* bone = objects[nodeIndex]
                    ->AddComponent<Engine::Components::AnimationBone>();
                bone->skinIndex = static_cast<unsigned>(skinIndex);
                bone->nodeIndex = nodeIndex;
                bone->paletteIndex = static_cast<int>(palette);
                bone->parentPaletteIndex = parentPalette;
                bone->hierarchyRoot = parentPalette < 0;
                if (bone->hierarchyRoot)
                {
                    auto* group = objects[nodeIndex]
                        ->AddComponent<Engine::Components::BoneGroup>();
                    group->skinIndex = static_cast<unsigned>(skinIndex);
                }
            }
        }

        for (unsigned nodeIndex : model.secondaryMotionNodes)
        {
            if (nodeIndex >= objects.size() || !objects[nodeIndex] ||
                !objects[nodeIndex]->GetComponent<Engine::Components::AnimationBone>())
                continue;
            auto* spring = objects[nodeIndex]->AddComponent<Engine::Components::IKBone>();
            spring->simulate = false;
            spring->wiggleEnabled = true;
        }

        std::vector<std::string> rigidNames(model.rigidBodies.size());
        for (size_t rigidIndex = 0; rigidIndex < model.rigidBodies.size();
            ++rigidIndex)
        {
            const auto& imported = model.rigidBodies[rigidIndex];
            Engine::Core::Object* parent = imported.parentNode >= 0 &&
                static_cast<size_t>(imported.parentNode) < objects.size()
                ? objects[static_cast<size_t>(imported.parentNode)] : root;
            Engine::Core::Object* object = AddChild(scene, parent,
                SafeName(imported.name, "Rigid body") + " [Body " +
                    std::to_string(rigidIndex) + "]");
            rigidNames[rigidIndex] = object->name;
            object->transform.position = imported.localPosition;
            object->transform.rotation = QuaternionEuler(imported.localRotation);
            if (!fittedColliderPaths[rigidIndex].empty())
            {
                auto* collider = object->AddComponent<
                    Engine::Components::MeshObjectCollider>();
                collider->meshPath = fittedColliderPaths[rigidIndex];
                collider->convex = true;
            }
            else
            {
                auto* collider = object->AddComponent<
                    Engine::Components::PrimitiveObjectCollider>();
                if (imported.shape == Engine::Model::RigidBodyShape::Sphere)
                {
                    collider->shape = "Sphere";
                    collider->radius = imported.size.x;
                }
                else if (imported.shape == Engine::Model::RigidBodyShape::Box)
                {
                    collider->shape = "Box";
                    collider->size = imported.size * 2.f;
                }
                else
                {
                    collider->shape = "Capsule";
                    collider->radius = imported.size.x;
                    collider->height = imported.size.y;
                }
            }
            auto* body = object->AddComponent<Engine::Components::RigidBody>();
            body->bodyType = imported.mode ==
                Engine::Model::RigidBodyMotion::FollowTransform ? "Kinematic" : "Dynamic";
            body->mass = imported.mode ==
                Engine::Model::RigidBodyMotion::FollowTransform ? 0.f :
                std::max(imported.mass, .001f);
            body->useGravity = imported.mode !=
                Engine::Model::RigidBodyMotion::FollowTransform;
            body->linearDamping = std::clamp(imported.linearDamping, 0.f, 1.f);
            body->angularDamping = std::clamp(imported.angularDamping, 0.f, 1.f);
            body->collisionLayer = 1 << imported.collisionGroup;
            body->collisionMask = (~imported.nonCollisionMask) & 0xffff;
            body->friction = imported.friction;
            body->restitution = imported.restitution;
            if (physicsBinding && imported.mode !=
                Engine::Model::RigidBodyMotion::FollowTransform &&
                imported.relatedBoneNode >= 0)
                physicsBinding->physicsBones.push_back({ object->name,
                    static_cast<unsigned>(imported.relatedBoneNode),
                    imported.mode, imported.boneOffsetPosition,
                    imported.boneOffsetRotation });
        }
        for (const auto& imported : model.softBodies)
        {
            const auto target = materialObjects.find(imported.materialIndex);
            if (target == materialObjects.end()) continue;
            auto* cloth = target->second->AddComponent<Engine::Components::Cloth>();
            cloth->topology = imported.shape == Engine::Model::SoftBodyTopology::Rope
                ? "Rope" : "TriangleMesh";
            cloth->mass = std::max(imported.mass, .001f);
            cloth->collisionMargin = std::max(imported.collisionMargin, .0001f);
            cloth->linearStiffness = std::clamp(imported.linearStiffness, 0.f, 1.f);
            cloth->bendingStiffness = std::clamp(imported.bendingStiffness, 0.f, 1.f);
            cloth->damping = std::clamp(imported.damping, 0.f, 1.f);
            cloth->drag = std::max(imported.drag, 0.f);
            cloth->friction = std::clamp(imported.friction, 0.f, 1.f);
            cloth->solverIterations = std::max(imported.solverIterations, 1);
            cloth->clusterCount = imported.clusterCount;
            cloth->clusterIterations = std::max(imported.clusterIterations, 1);
            cloth->clusterHardness = imported.clusterHardness;
            cloth->clusterImpulseSplit = imported.clusterImpulseSplit;
            cloth->collisionLayer = 1 << imported.collisionGroup;
            cloth->collisionMask = (~imported.nonCollisionMask) & 0xffff;
            cloth->pinMode = "Explicit";
            cloth->explicitPinPositions = imported.pinnedVertices;
            for (const auto& anchor : imported.anchors)
                if (anchor.bodyIndex >= 0 &&
                    static_cast<size_t>(anchor.bodyIndex) < rigidNames.size())
                    cloth->anchors.push_back({ anchor.vertexPosition,
                        rigidNames[anchor.bodyIndex], anchor.nearMode });
        }
        if (physicsBinding)
            for (auto& joint : physicsBinding->physicsJoints)
            {
                if (joint.bodyA >= 0 &&
                    static_cast<size_t>(joint.bodyA) < rigidNames.size())
                    joint.bodyAName = rigidNames[joint.bodyA];
                if (joint.bodyB >= 0 &&
                    static_cast<size_t>(joint.bodyB) < rigidNames.size())
                    joint.bodyBName = rigidNames[joint.bodyB];
            }
        if (morphControls)
            for (auto& morph : morphControls->impulseMorphs)
                for (auto& offset : morph.offsets)
                    if (offset.bodyIndex >= 0 &&
                        static_cast<size_t>(offset.bodyIndex) < rigidNames.size())
                        offset.bodyName = rigidNames[offset.bodyIndex];

        const fs::path prefabPath = output / (output.filename().string() + ".prefab");
        if (!model.skins.empty())
        {
            Engine::Model::PoseAsset restPose;
            restPose.name = "Rest pose";
            std::set<unsigned> visited;
            for (const auto& skin : model.skins)
                for (unsigned nodeIndex : skin.jointNodes)
                    if (nodeIndex < model.nodes.size() && visited.insert(nodeIndex).second)
                    {
                        const std::string path = AnimationClipMapping::NodePath(
                            *modelComponent, nodeIndex);
                        if (path.empty()) continue;
                        const auto& node = model.nodes[nodeIndex];
                        restPose.joints.push_back({ path, node.translation,
                            node.rotation, node.scale });
                    }
            if (!restPose.joints.empty())
            {
                const fs::path posePath = output /
                    (output.filename().string() + "_Rest.pose");
                if (!restPose.Save(posePath.string()))
                    throw std::runtime_error("Could not save rest pose: " +
                        posePath.string());
                Engine::Core::AssetRecord::Ensure(posePath, source,
                    { { "importer", std::string("model-pose") },
                      { "sourceFormat", model.sourceFormat } });
            }
        }
        auto* manager = root->GetComponent<Engine::Components::AnimationManager>();
        if (manager)
            for (auto& clip : manager->clips)
                for (auto& channel : clip.channels)
                    channel.targetPath = AnimationClipMapping::NodePath(
                        *modelComponent, channel.nodeIndex);
        std::vector<std::string> sharedAnimationPaths;
        std::vector<Engine::Model::RigAsset> rigSkins;
        const fs::path rigPath = output /
            (output.filename().string() + ".rig");
        for (auto* component : root->Components)
            if (auto* skeleton = dynamic_cast<Engine::Components::Skeleton*>(component))
            {
                auto rig = Engine::Model::RigAsset::Capture(scene,
                    *skeleton);
                if (auto previous = Engine::Model::RigAsset::Load(
                        rigPath.string(), skeleton->skinIndex);
                    previous && previous->joints.size() == rig.joints.size())
                {
                    bool sameNodes = true;
                    for (size_t joint = 0; joint < rig.joints.size(); ++joint)
                        sameNodes &= previous->joints[joint].nodeIndex ==
                            rig.joints[joint].nodeIndex &&
                            previous->joints[joint].name == rig.joints[joint].name;
                    if (sameNodes)
                    {
                        rig.controls = previous->controls;
                        rig.poses = previous->poses;
                        rig.symmetryPairs = previous->symmetryPairs;
                    }
                }
                rig.prefabPath = prefabPath.generic_string();
                if (manager && sharedAnimationPaths.empty())
                {
                    WriteImportedAnimations(rig, *manager, *modelComponent,
                        rigPath, prefabPath, source);
                    sharedAnimationPaths = rig.animationPaths;
                }
                else if (manager)
                    rig.animationPaths = sharedAnimationPaths;
                rigSkins.push_back(std::move(rig));
                skeleton->rigPath = rigPath.generic_string();
            }
        // Animations without a skinned mesh must still be available as .anim.
        if (manager && sharedAnimationPaths.empty())
        {
            Engine::Model::RigAsset animations;
            WriteImportedAnimations(animations, *manager, *modelComponent,
                rigPath, prefabPath, source);
            sharedAnimationPaths = animations.animationPaths;
            manager->animationPaths = sharedAnimationPaths;
        }
        for (const auto& rig : rigSkins)
        {
            auto standalone = rig;
            standalone.prefabPath.clear();
            standalone.animationPaths.clear();
            standalone.meshNames.clear();
            const auto path = output / (output.filename().string() +
                (rigSkins.size() > 1 ? "_" + std::to_string(rig.skinIndex) : "") + ".skeleton");
            if (!standalone.Save(path.string()))
                throw std::runtime_error("Could not save skeleton: " + path.string());
            Engine::Core::AssetRecord::Ensure(path, source,
                { { "importer", std::string("model-skeleton") } });
        }
        if (!rigSkins.empty())
        {
            if (!Engine::Model::RigAsset::SaveBundle(rigPath.string(), rigSkins))
                throw std::runtime_error("Could not save rig: " + rigPath.string());
            if (manager) manager->rigPaths.push_back(rigPath.generic_string());
            Engine::Core::AssetRecord::Ensure(rigPath, source,
                { { "importer", std::string("model-rig") },
                  { "skinCount", static_cast<double>(rigSkins.size()) } });
        }

        if (manager) manager->RefreshRigClips();
        if (!Engine::Serialization::SceneSerializer::SavePrefab(*root, prefabPath.string()))
            throw std::runtime_error("Could not save prefab");
        Engine::Core::AssetRecord::Ensure(prefabPath, source,
            { { "importer", std::string("model") }, { "sourceFormat", model.sourceFormat } });
        result.success = true;
        result.prefabPath = prefabPath.generic_string();
        result.outputDirectory = output.generic_string();
        result.message = "Imported " + source.filename().string();
    }
    catch (const std::exception& error)
    {
        std::error_code cleanupError;
        if (!output.empty() && output.parent_path().lexically_normal() == assets)
            fs::remove_all(output, cleanupError);
        result.message = error.what();
    }
    return result;
}
}

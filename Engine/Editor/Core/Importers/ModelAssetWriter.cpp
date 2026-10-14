#include "ModelAssetWriter.h"
#include "Core/AnimationClipMapping.h"

#include "Core/AssetRecord.h"
#include "Core/Compoonents/Materials/Material.h"
#include "Core/Compoonents/Materials/PmxMaterialMorph.h"
#include "Core/Compoonents/Animation/ModelAnimation.h"
#include "Core/Compoonents/Animation/BoneGroup.h"
#include "Core/Compoonents/Animation/PmxRigController.h"
#include "Core/Compoonents/Camera/Camera.h"
#include "Core/Compoonents/Lighting/Light.h"
#include "Core/Compoonents/Physics/PrimitiveObjectCollider.h"
#include "Core/Compoonents/Physics/RigidBody.h"
#include "Core/Compoonents/Physics/Cloth.h"
#include "Core/Model/RigAsset.h"
#include "AnimationAssetWriter.h"
#include "Core/Object.h"
#include "Core/Scene/Scene.h"
#include "Core/Serialization/SceneSerializer.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
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
}

Engine::Model::ModelImportResult ModelAssetWriter::Write(const Engine::Model::ImportedModel& model,
    const std::string& sourcePath, const std::string& assetsDirectory)
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
            material.useMmdSpecular = imported.useMmdSpecular;
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
        for (size_t index = 0; index < model.primitives.size(); ++index)
        {
            const Engine::Model::ImportedPrimitive& primitive = model.primitives[index];
            if (primitive.vertices.empty()) continue;
            const fs::path path = meshesDirectory /
                (SafeName(primitive.name, "Mesh") + " " + std::to_string(index + 1) + ".mesh");
            if (!Engine::Components::Mesh::SaveNativeFile(path.string(), primitive.vertices))
                throw std::runtime_error("Could not save mesh: " + path.string());
            Engine::Core::AssetRecord::Ensure(path, source,
                { { "importer", std::string("model-mesh") },
                  { "sourceFormat", model.sourceFormat },
                  { "primitiveIndex", static_cast<double>(index) } });
            meshPaths[index] = path.generic_string();
            if (!primitive.morphTargets.empty())
            {
                const fs::path morphPath = meshesDirectory /
                    (SafeName(primitive.name, "Mesh") + " " +
                     std::to_string(index + 1) + ".morph");
                if (!Engine::Components::Mesh::SaveMorphFile(
                        morphPath.string(), primitive.morphTargets))
                    throw std::runtime_error("Could not save morph data: " +
                        morphPath.string());
                morphPaths[index] = morphPath.generic_string();
                Engine::Core::AssetRecord::Ensure(morphPath, source,
                    { { "importer", std::string("model-morph") },
                      { "primitiveIndex", static_cast<double>(index) } });
            }
        }

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
        Engine::Components::PmxRigController* pmxController = nullptr;
        if (!model.appendRules.empty() || !model.ikChains.empty() ||
            !model.fixedAxes.empty() || !model.localAxes.empty() ||
            !model.externalParents.empty() ||
            !model.boneMorphs.empty() ||
            !model.impulseMorphs.empty() ||
            !model.physicsJoints.empty() ||
            std::any_of(model.rigidBodies.begin(), model.rigidBodies.end(),
                [](const auto& body) { return body.mode > 0; }))
        {
            auto* controller = root->AddComponent<Engine::Components::PmxRigController>();
            pmxController = controller;
            controller->appendRules = model.appendRules;
            controller->ikChains = model.ikChains;
            controller->fixedAxes = model.fixedAxes;
            controller->localAxes = model.localAxes;
            controller->externalParents = model.externalParents;
            controller->boneMorphs = model.boneMorphs;
            controller->impulseMorphs = model.impulseMorphs;
            controller->physicsJoints = model.physicsJoints;
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
                            Engine::Components::PmxMaterialMorph>();
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

        std::vector<std::string> rigidNames(model.rigidBodies.size());
        for (size_t rigidIndex = 0; rigidIndex < model.rigidBodies.size();
            ++rigidIndex)
        {
            const auto& imported = model.rigidBodies[rigidIndex];
            Engine::Core::Object* parent = imported.parentNode >= 0 &&
                static_cast<size_t>(imported.parentNode) < objects.size()
                ? objects[static_cast<size_t>(imported.parentNode)] : root;
            Engine::Core::Object* object = AddChild(scene, parent,
                SafeName(imported.name, "Rigid body") + " [PMX " +
                    std::to_string(rigidIndex) + "]");
            rigidNames[rigidIndex] = object->name;
            object->transform.position = imported.localPosition;
            object->transform.rotation = QuaternionEuler(imported.localRotation);
            auto* collider = object->AddComponent<
                Engine::Components::PrimitiveObjectCollider>();
            if (imported.shape == 0)
            {
                collider->shape = "Sphere";
                collider->radius = imported.size.x;
            }
            else if (imported.shape == 1)
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
            auto* body = object->AddComponent<Engine::Components::RigidBody>();
            body->bodyType = imported.mode == 0 ? "Kinematic" : "Dynamic";
            body->mass = imported.mode == 0 ? 0.f :
                std::max(imported.mass, .001f);
            body->useGravity = imported.mode != 0;
            body->linearDamping = std::clamp(imported.linearDamping, 0.f, 1.f);
            body->angularDamping = std::clamp(imported.angularDamping, 0.f, 1.f);
            body->collisionLayer = 1 << imported.collisionGroup;
            body->collisionMask = (~imported.nonCollisionMask) & 0xffff;
            body->friction = imported.friction;
            body->restitution = imported.restitution;
            if (pmxController && imported.mode > 0 &&
                imported.relatedBoneNode >= 0)
                pmxController->physicsBones.push_back({ object->name,
                    static_cast<unsigned>(imported.relatedBoneNode),
                    imported.mode, imported.boneOffsetPosition,
                    imported.boneOffsetRotation });
        }
        for (const auto& imported : model.softBodies)
        {
            const auto target = materialObjects.find(imported.materialIndex);
            if (target == materialObjects.end()) continue;
            auto* cloth = target->second->AddComponent<Engine::Components::Cloth>();
            cloth->mass = std::max(imported.mass, .001f);
            cloth->collisionMargin = std::max(imported.collisionMargin, .0001f);
            cloth->linearStiffness = std::clamp(imported.linearStiffness, 0.f, 1.f);
            cloth->bendingStiffness = std::clamp(imported.bendingStiffness, 0.f, 1.f);
            cloth->damping = std::clamp(imported.damping, 0.f, 1.f);
            cloth->drag = std::max(imported.drag, 0.f);
            cloth->friction = std::clamp(imported.friction, 0.f, 1.f);
            cloth->solverIterations = std::max(imported.solverIterations, 1);
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
        if (pmxController)
            for (auto& joint : pmxController->physicsJoints)
            {
                if (joint.bodyA >= 0 &&
                    static_cast<size_t>(joint.bodyA) < rigidNames.size())
                    joint.bodyAName = rigidNames[joint.bodyA];
                if (joint.bodyB >= 0 &&
                    static_cast<size_t>(joint.bodyB) < rigidNames.size())
                    joint.bodyBName = rigidNames[joint.bodyB];
            }
        if (pmxController)
            for (auto& morph : pmxController->impulseMorphs)
                for (auto& offset : morph.offsets)
                    if (offset.bodyIndex >= 0 &&
                        static_cast<size_t>(offset.bodyIndex) < rigidNames.size())
                        offset.bodyName = rigidNames[offset.bodyIndex];

        const fs::path prefabPath = output / (output.filename().string() + ".prefab");
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

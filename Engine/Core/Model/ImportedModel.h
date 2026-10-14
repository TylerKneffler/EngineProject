#pragma once

#include "Core/Model/AnimationData.h"
#include "Core/Model/MeshData.h"
#include <cstddef>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <string>
#include <vector>

namespace Engine::Model
{
struct ImportedMaterial
{
    std::string name;
    glm::vec4 baseColor { 1.f };
    glm::vec3 emissiveColor { 0.f };
    float metallic = 0.f;
    float roughness = 1.f;
    float alphaCutoff = 0.5f;
    float normalScale = 1.f;
    bool doubleSided = false;
    bool unlit = false;
    std::string alphaMode = "Opaque";
    std::string baseColorTexture;
    std::string metallicRoughnessTexture;
    std::string normalTexture;
    std::string occlusionTexture;
    std::string emissiveTexture;
    int baseColorUvSet = 0;
    int metallicRoughnessUvSet = 0;
    int normalUvSet = 0;
    int occlusionUvSet = 0;
    int emissiveUvSet = 0;
};

struct ImportedTexture
{
    std::string source;
    std::string name;
    std::vector<std::byte> bytes;
};

struct ImportedPrimitive
{
    std::string name;
    std::vector<AnimationVertex> vertices;
    std::vector<MorphTarget> morphTargets;
    std::vector<float> morphWeights;
    int materialIndex = -1;
    int skinIndex = -1;
    glm::mat4 bindMeshToModel { 1.f };
};

struct ImportedNode
{
    std::string name;
    int parent = -1;
    glm::vec3 translation { 0.f };
    glm::quat rotation { 1.f, 0.f, 0.f, 0.f };
    glm::vec3 scale { 1.f };
    std::vector<unsigned> primitives;
};

struct ImportedCamera
{
    unsigned nodeIndex = 0;
    glm::vec3 position { 0.f };
    glm::quat rotation { 1.f, 0.f, 0.f, 0.f };
    float verticalFov = 60.f;
    float nearPlane = 0.01f;
    float farPlane = 1000.f;
    bool orthographic = false;
    float orthographicSize = 5.f;
};

struct ImportedLight
{
    enum class Type { Point, Directional, Spot };
    unsigned nodeIndex = 0;
    glm::vec3 position { 0.f };
    glm::quat rotation { 1.f, 0.f, 0.f, 0.f };
    Type type = Type::Point;
    glm::vec3 color { 1.f };
    float intensity = 1.f;
    float range = 8.f;
    float innerConeAngle = 25.f;
    float outerConeAngle = 35.f;
};

struct ImportedSkin
{
    std::string name;
    std::vector<unsigned> jointNodes;
    std::vector<glm::mat4> inverseBindMatrices;
};

struct ImportedAnimation
{
    std::string name;
    float duration = 0.f;
    std::vector<AnimationChannel> channels;
};

struct ImportedModel
{
    std::string name;
    std::string sourceFormat;
    // Source-node/clip coordinates remain intact; the prefab root converts
    // their units to the engine's meters exactly once.
    float unitScaleToMeters = 1.f;
    std::vector<ImportedMaterial> materials;
    std::vector<ImportedTexture> textures;
    std::vector<ImportedPrimitive> primitives;
    std::vector<ImportedNode> nodes;
    std::vector<ImportedCamera> cameras;
    std::vector<ImportedLight> lights;
    std::vector<ImportedSkin> skins;
    std::vector<ImportedAnimation> animations;
};

struct ModelImportResult
{
    bool success = false;
    std::string prefabPath;
    std::string primaryAssetPath;
    std::string outputDirectory;
    std::string message;
};
}

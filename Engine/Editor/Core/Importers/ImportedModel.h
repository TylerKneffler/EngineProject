#pragma once

#include "Core/Compoonents/Animation/ModelAnimation.h"
#include "Core/Compoonents/Mesh.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <string>
#include <cstddef>
#include <vector>

// Canonical editor-time representation shared by every model decoder. Nothing
// here refers to glTF, FBX, Assimp, or a renderer API.
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
    // Decoder-local reference used by material texture fields (for example *0).
    std::string source;
    std::string name;
    std::vector<std::byte> bytes;
};

struct ImportedPrimitive
{
    std::string name;
    std::vector<Vertex> vertices;
    std::vector<MorphTargets::Target> morphTargets;
    std::vector<float> morphWeights;
    int materialIndex = -1;
    int skinIndex = -1;
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
    std::vector<ImportedMaterial> materials;
    std::vector<ImportedTexture> textures;
    std::vector<ImportedPrimitive> primitives;
    std::vector<ImportedNode> nodes;
    std::vector<ImportedSkin> skins;
    std::vector<ImportedAnimation> animations;
};

struct ModelImportResult
{
    bool success = false;
    std::string prefabPath;
    std::string outputDirectory;
    std::string message;
};

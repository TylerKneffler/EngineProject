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
enum class RigidBodyShape : int { Sphere = 0, Box = 1, Capsule = 2 };
enum class RigidBodyMotion : int
{
    FollowTransform = 0,
    Simulated = 1,
    SimulatedRotation = 2
};
enum class PhysicsJointKind : int
{
    SpringSixDof = 0,
    SixDof = 1,
    Point = 2,
    ConeTwist = 3,
    Slider = 4,
    Hinge = 5
};
enum class SoftBodyTopology : int { TriangleMesh = 0, Rope = 1 };

struct ImportedMaterial
{
    std::string name;
    glm::vec4 baseColor { 1.f };
    glm::vec3 emissiveColor { 0.f };
    glm::vec3 ambientColor { 0.1f };
    glm::vec3 specularColor { 1.f };
    float shininess = 32.f;
    std::string specularModel = "MetallicRoughness";
    glm::vec4 edgeColor { 0.f };
    float edgeWidth = 0.f;
    bool edgeEnabled = false;
    std::string sphereTexture;
    int sphereMode = 0;
    std::string toonTexture;
    int builtinToon = -1;
    float metallic = 0.f;
    float roughness = 1.f;
    float alphaCutoff = 0.5f;
    float normalScale = 1.f;
    bool doubleSided = false;
    bool castsShadows = true;
    bool unlit = false;
    bool useVertexColors = true;
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

struct ImportedRigidBody
{
    std::string name;
    int parentNode = -1;
    glm::vec3 localPosition { 0.f };
    glm::quat localRotation { 1.f, 0.f, 0.f, 0.f };
    RigidBodyShape shape = RigidBodyShape::Sphere;
    glm::vec3 size { 1.f };
    int collisionGroup = 0;
    int nonCollisionMask = 0;
    float friction = 0.5f;
    float restitution = 0.f;
    RigidBodyMotion mode = RigidBodyMotion::FollowTransform;
    int relatedBoneNode = -1;
    glm::vec3 boneOffsetPosition { 0.f };
    glm::quat boneOffsetRotation { 1.f, 0.f, 0.f, 0.f };
    float mass = 0.f;
    float linearDamping = 0.f;
    float angularDamping = 0.f;
};

struct ImportedPhysicsJoint
{
    std::string name;
    PhysicsJointKind type = PhysicsJointKind::SpringSixDof;
    int bodyA = -1; // Index in ImportedModel::rigidBodies.
    int bodyB = -1;
    std::string bodyAName;
    std::string bodyBName;
    glm::vec3 position { 0.f };
    glm::quat rotation { 1.f, 0.f, 0.f, 0.f };
    glm::vec3 linearLower { 0.f };
    glm::vec3 linearUpper { 0.f };
    glm::vec3 angularLower { 0.f };
    glm::vec3 angularUpper { 0.f };
    glm::vec3 linearSpring { 0.f };
    glm::vec3 angularSpring { 0.f };
    glm::vec3 frameAPosition { 0.f };
    glm::quat frameARotation { 1.f, 0.f, 0.f, 0.f };
    glm::vec3 frameBPosition { 0.f };
    glm::quat frameBRotation { 1.f, 0.f, 0.f, 0.f };
};

struct ImportedPhysicsBone
{
    std::string bodyName;
    unsigned nodeIndex = 0;
    RigidBodyMotion mode = RigidBodyMotion::Simulated;
    glm::vec3 offsetPosition { 0.f };
    glm::quat offsetRotation { 1.f, 0.f, 0.f, 0.f };
};

struct ImportedSoftBody
{
    struct Anchor
    {
        int bodyIndex = -1; // Index in ImportedModel::rigidBodies.
        glm::vec3 vertexPosition { 0.f };
        bool nearMode = false;
    };
    std::string name;
    int materialIndex = -1;
    SoftBodyTopology shape = SoftBodyTopology::TriangleMesh;
    int collisionGroup = 0;
    int nonCollisionMask = 0;
    float mass = 1.f;
    float collisionMargin = .02f;
    float linearStiffness = .8f;
    float bendingStiffness = .2f;
    float damping = .01f;
    float drag = .01f;
    float friction = .5f;
    int solverIterations = 8;
    int clusterCount = 0;
    int clusterIterations = 4;
    glm::vec3 clusterHardness { .1f, 1.f, .5f };
    glm::vec3 clusterImpulseSplit { .5f };
    std::vector<glm::vec3> pinnedVertices;
    std::vector<Anchor> anchors;
};

struct ImportedAppendRule
{
    unsigned nodeIndex = 0;
    unsigned sourceNodeIndex = 0;
    glm::vec3 sourceRestTranslation { 0.f };
    float influence = 0.f;
    int deformLayer = 0;
    int deformOrder = -1; // Stable evaluation order within the layer.
    bool afterPhysics = false;
    bool rotation = false;
    bool translation = false;
};

struct ImportedIkLink
{
    unsigned nodeIndex = 0;
    bool limited = false;
    glm::vec3 minimum { 0.f };
    glm::vec3 maximum { 0.f };
    bool fixedAxis = false;
    glm::vec3 axis { 0.f };
};

struct ImportedIkChain
{
    unsigned controllerNodeIndex = 0;
    unsigned effectorNodeIndex = 0;
    bool enabled = true;
    int iterations = 1;
    float angleLimit = 0.f;
    int deformLayer = 0;
    int deformOrder = -1; // Stable evaluation order within the layer.
    bool afterPhysics = false;
    std::vector<ImportedIkLink> links;
};

struct ImportedFixedAxis
{
    unsigned nodeIndex = 0;
    glm::vec3 axis { 0.f };
};

struct ImportedLocalAxis
{
    unsigned nodeIndex = 0;
    glm::vec3 x { 1.f, 0.f, 0.f };
    glm::vec3 z { 0.f, 0.f, 1.f };
    glm::vec3 restTranslation { 0.f };
};

struct ImportedExternalParent
{
    unsigned nodeIndex = 0;
    int key = 0;
    int deformLayer = 0;
    int deformOrder = -1;
    bool afterPhysics = false;
    std::string targetName; // Scene object bound to this key.
};

struct ImportedBoneMorphOffset
{
    unsigned nodeIndex = 0;
    glm::vec3 translation { 0.f };
    glm::quat rotation { 1.f, 0.f, 0.f, 0.f };
};

struct ImportedBoneMorph
{
    std::string name;
    float weight = 0.f;
    std::vector<ImportedBoneMorphOffset> offsets;
};

struct ImportedImpulseMorphOffset
{
    int bodyIndex = -1; // Index in ImportedModel::rigidBodies.
    std::string bodyName;
    bool local = false;
    glm::vec3 velocity { 0.f };
    glm::vec3 torque { 0.f };
};

struct ImportedImpulseMorph
{
    std::string name;
    float weight = 0.f;
    std::vector<ImportedImpulseMorphOffset> offsets;
};

struct ImportedMaterialMorphOffset
{
    int materialIndex = -1; // -1 applies to every material.
    bool additive = false;
    glm::vec4 diffuse { 0.f };
    glm::vec3 specular { 0.f };
    float shininess = 0.f;
    glm::vec3 ambient { 0.f };
    glm::vec4 textureTint { 0.f };
    glm::vec4 edgeColor { 0.f };
    float edgeWidth = 0.f;
    glm::vec4 sphereTint { 0.f };
    glm::vec4 toonTint { 0.f };
};

struct ImportedMaterialMorph
{
    std::string name;
    float weight = 0.f;
    std::vector<ImportedMaterialMorphOffset> offsets;
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
    std::vector<ImportedRigidBody> rigidBodies;
    std::vector<ImportedPhysicsJoint> physicsJoints;
    // PMX secondary-motion bones driven by inexpensive pose-following springs.
    std::vector<unsigned> secondaryMotionNodes;
    std::vector<ImportedSoftBody> softBodies;
    std::vector<ImportedAppendRule> appendRules;
    std::vector<ImportedIkChain> ikChains;
    std::vector<ImportedFixedAxis> fixedAxes;
    std::vector<ImportedLocalAxis> localAxes;
    std::vector<ImportedExternalParent> externalParents;
    std::vector<ImportedBoneMorph> boneMorphs;
    std::vector<ImportedMaterialMorph> materialMorphs;
    std::vector<ImportedImpulseMorph> impulseMorphs;
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

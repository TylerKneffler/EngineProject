#pragma once

#include "Core/Model/ImportedModel.h"
#include "Core/Model/AnimationClip.h"
#include "Core/Serialization/Json.h"
#include "Editor/Core/Importers/VmdMotionDecoder.h"
#include <glm/mat4x4.hpp>
#include <string>
#include <vector>

namespace Engine::Editor
{
// Evaluates MMD bone keys against a reviewed PMX-to-FBX map. The result is
// node-local matrices; callers may bake them into animation channels later.
class MmdPoseRetargeter
{
public:
    struct Pose
    {
        std::vector<glm::mat4> local;
        std::vector<glm::mat4> world;
        // PMX-space poses, including append/inherit transforms. IK is separate.
        std::vector<glm::mat4> sourceWorld;
        std::vector<float> ikError; // Error to the reachable target, in PMX units.
        std::vector<float> ikTargetDrift; // Distance from original VMD goal.
        std::vector<float> ikOriginalError; // End effector error to original goal.
    };
    struct FacialBake
    {
        Model::AnimationClip clip;
        std::vector<std::string> unmatchedExpressions;
        size_t matchedExpressions = 0;
        size_t matchedTargets = 0;
    };

    bool Initialize(const Model::ImportedModel& target,
        const Serialization::JsonValue& profile,
        const Serialization::JsonValue& mapping,
        const VmdMotion& motion, std::string& error);
    // Full evaluation is retained as a reference for validating IK dependency updates.
    Pose Sample(float frame, bool fullIkRebuild = false) const;
    // Samples the solved rig at 120 Hz into independent, linear FK channels.
    // The caller keeps scene tracks (camera, light, morphs) separate.
    Model::AnimationClip BakeBody(unsigned lastFrame,
        const std::string& clipName = "dance1") const;
    FacialBake BakeFacial(unsigned lastFrame,
        const std::string& clipName = "dance1 facial") const;

private:
    struct SourceBone
    {
        int parent = -1;
        int target = -1;
        std::string name;
        std::string role;
        glm::vec3 rest { 0.f };
        int inheritSource = -1;
        float inheritWeight = 0.f;
        bool inheritRotation = false;
        bool inheritTranslation = false;
        std::vector<VmdBoneFrame> keys;
    };
    struct IkLink
    {
        int bone = -1;
        bool limited = false;
        glm::vec3 minimum { 0.f };
        glm::vec3 maximum { 0.f };
    };
    struct IkChain
    {
        int controller = -1;
        int tip = -1;
        int iterations = 0;
        float stepRadians = 0.f;
        std::vector<IkLink> links;
    };
    struct FacialMesh
    {
        unsigned ownerNode = 0;
        std::string objectName;
        bool childObject = false;
        std::vector<std::string> targetNames;
        std::vector<float> defaults;
    };
    std::vector<SourceBone> m_source;
    std::vector<IkChain> m_ik;
    std::vector<VmdPropertyFrame> m_properties;
    std::vector<VmdMorphFrame> m_morphs;
    std::vector<Model::ImportedNode> m_target;
    std::vector<FacialMesh> m_facialMeshes;
    std::vector<glm::mat4> m_restLocal;
    std::vector<glm::mat4> m_restWorld;
    glm::mat4 m_axis { 1.f };
    glm::mat4 m_inverseAxis { 1.f };
    int m_rootSource = -1;
    int m_rootTarget = -1;
};
}

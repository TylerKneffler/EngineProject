#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <optional>
#include <string>
#include <vector>

namespace Engine::Model
{
// A single pose stored independently of a rig asset. Bone paths are relative
// to the model root, so node indices may differ between compatible rigs.
struct PoseAsset
{
    struct Joint
    {
        std::string path;
        glm::vec3 position { 0.f };
        glm::quat rotation { 1.f, 0.f, 0.f, 0.f };
        glm::vec3 scale { 1.f };
    };
    std::string name;
    std::vector<Joint> joints;

    bool Save(const std::string& path) const;
    static std::optional<PoseAsset> Load(const std::string& path);
};
}

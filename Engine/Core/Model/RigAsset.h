#pragma once

#include <glm/glm.hpp>
#include <optional>
#include <string>
#include <vector>

namespace Engine::Scene { class Scene; }
namespace Engine::Components { class Skeleton; }

namespace Engine::Model
{
// A stable description of one skin's bone palette and bound meshes.
// Rig-backed prefabs store their bind data here; scene snapshots may still
// carry the same data so editor undo can restore in-memory edits.
struct RigAsset
{
    struct Joint
    {
        unsigned nodeIndex = 0;
        std::string name;
        int parentNodeIndex = -1;
        glm::mat4 inverseBind { 1.f };
    };
    unsigned skinIndex = 0;
    size_t rigIndex = 0;
    std::string prefabPath;
    std::vector<Joint> joints;
    std::vector<std::string> meshNames;

    static RigAsset Capture(const Engine::Scene::Scene& scene,
        const Engine::Components::Skeleton& skeleton);
    bool Save(const std::string& path) const;
    static std::optional<RigAsset> Load(const std::string& path);
};
}

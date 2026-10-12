#pragma once

#include "AnimationClip.h"
#include <glm/glm.hpp>
#include <optional>
#include <string>
#include <vector>

namespace Engine::Components { class Model; }
namespace Engine::Model
{
// Standalone skeletal animation. Channels retain target paths so a compatible
// rig can resolve them independently of the model's original node indices.
struct AnimationClipAsset
{
    struct BaseNode
    {
        std::string path;
        std::string parent;
        glm::vec3 position{};
        glm::vec3 rotation{};
        glm::vec3 scale{ 1.f };
    };
    AnimationClip clip;
    std::vector<BaseNode> baseSkeleton;
    std::string previewPrefab;
    size_t previewRig = 0;

    static std::optional<AnimationClipAsset> Load(const std::string& path);
    static std::vector<BaseNode> CaptureBaseSkeleton(
        const Engine::Components::Model& model);
    bool Save(const std::string& path) const;
};
}

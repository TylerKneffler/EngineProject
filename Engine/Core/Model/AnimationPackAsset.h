#pragma once

#include "AnimationClip.h"
#include "Core/Serialization/Json.h"
#include <optional>
#include <string>
#include <vector>

namespace Engine::Components { class AnimationManager; }

namespace Engine::Model
{
// A portable snapshot of clips and their simultaneous playback configuration.
// Clip data is embedded, so importing the pack does not need the source rig's
// individual .anim files.
struct AnimationPackAsset
{
    struct SourceTrack
    {
        std::string kind;
        std::string json;
    };
    std::string activeClip;
    bool playing = true;
    bool looping = true;
    float speed = 1.f;
    float time = 0.f;
    std::vector<AnimationClip> clips;
    std::vector<AnimationLayer> layers;
    std::vector<std::vector<std::string>> nodeMaskPaths;
    // Source tracks retain their native keys and timeline. A later editor
    // action can extract one as a .vmdtrack or bind it to a scene component.
    std::vector<SourceTrack> sourceTracks;

    static AnimationPackAsset Capture(const Engine::Components::AnimationManager& manager);
    static std::optional<AnimationPackAsset> Load(const std::string& path,
        std::string& error);
    bool Save(const std::string& path, std::string& error) const;
    bool Apply(Engine::Components::AnimationManager& manager,
        std::string& error) const;
    bool ExtractSourceTrack(const std::string& kind, const std::string& path,
        std::string& error) const;
};
}

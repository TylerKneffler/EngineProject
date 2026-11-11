#pragma once

#include <map>
#include <string>
#include <utility>
#include <vector>

namespace Engine::Components
{
struct ObjectLodPartProfileData
{
    std::string key;
    // Ordered base-to-coarsest mesh assets for this renderer/submesh part.
    std::vector<std::string> meshes;
};

struct ObjectLodProfileData
{
    // Empty mesh chains inherit a provider's base representation.
    std::vector<std::string> meshes;
    // Empty shadow chains reuse the visible representation chain.
    std::vector<std::string> shadowMeshes;
    // Ascending world distances and descending projected-pixel boundaries.
    std::vector<float> distances;
    std::vector<float> screenThresholdsPixels;
    float hysteresis = .1f;
    bool screenSizeSelection = false;
    bool distanceOverride = false;
    bool ditherTransition = true;
    float transitionWidth = .1f;
    float shadowDistance = 0.f;
    // Keep the final resident representation unless this distance is enabled.
    bool cullAfterLastLod = false;
    float finalLodCullDistance = 200.f;
    // Additional renderer/submesh parts that share this profile's LOD policy.
    // Kept last to preserve positional aggregate initialization compatibility.
    std::vector<ObjectLodPartProfileData> parts;
};

// Supplies authored mesh representation chains without tying their source to
// terrain cells. Any component can provide alternate mesh assets for objects
// managed by a placement or prefab system.
class ObjectLodProvider
{
public:
    virtual ~ObjectLodProvider() = default;
    virtual std::map<std::string, ObjectLodProfileData>
        GetObjectLodProfiles() const = 0;

    std::map<std::string, std::vector<std::string>> GetObjectLodMeshes() const
    {
        std::map<std::string, std::vector<std::string>> result;
        for (const auto& [asset, profile] : GetObjectLodProfiles())
            result.emplace(asset, profile.meshes);
        return result;
    }
};
}

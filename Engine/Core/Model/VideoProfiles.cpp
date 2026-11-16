#include "Core/Model/VideoProfiles.h"

#include <algorithm>

namespace Engine::Model
{
void VideoProfile::Normalize()
{
    qualityPreset = std::clamp(qualityPreset, -1, 4);
    if (width == 0 || height == 0) width = height = 0;
    width = std::min(width, 16384u);
    height = std::min(height, 16384u);
    maxFps = std::clamp(maxFps, -1, 1000);
    textureBudgetMiB = textureBudgetMiB <= 0.f ? 0.f : std::clamp(textureBudgetMiB, 64.f, 65536.f);
    cpuWorkers = std::min(cpuWorkers, 128u);
    cpuLoadMs = std::clamp(cpuLoadMs, 0.f, 200.f);
    gpuLoadMs = std::clamp(gpuLoadMs, 0.f, 200.f);
}

std::vector<VideoProfile> DefaultVideoProfiles()
{
    std::vector<VideoProfile> profiles(3);
    auto& high = profiles[0];
    high.name = "High";
    high.qualityPreset = 1;

    auto& medium = profiles[1];
    medium.name = "Medium";
    medium.qualityPreset = 2;
    medium.width = 1600; medium.height = 900;
    medium.textureBudgetMiB = 1024.f;
    medium.cpuWorkers = 6;
    medium.gpuLoadMs = 4.f;

    auto& low = profiles[2];
    low.name = "Low";
    low.qualityPreset = 3;
    low.width = 1280; low.height = 720;
    low.maxFps = 30;
    low.textureBudgetMiB = 512.f;
    low.cpuWorkers = 4;
    low.cpuLoadMs = 4.f;
    low.gpuLoadMs = 10.f;
    return profiles;
}

const VideoProfile* FindVideoProfile(const std::vector<VideoProfile>& profiles, const std::string& name)
{
    if (name.empty()) return nullptr;
    const auto found = std::find_if(profiles.begin(), profiles.end(),
        [&](const VideoProfile& profile) { return profile.name == name; });
    return found == profiles.end() ? nullptr : &*found;
}
}

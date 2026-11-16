#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace Engine::Model
{
// A named bundle of video settings, plus optional hardware limits that make
// this machine behave like a weaker one, for testing how the game holds up
// on lesser hardware (and how Auto quality adapts). Project settings keep a
// list (Preferences > Video Profiles); the game applies the active one.
// Every field has a "keep" value that leaves the project or scene setting
// alone.
struct VideoProfile
{
    std::string name;

    // Video settings.
    // GraphicsQualitySettings preset: 0 Reference, 1 High, 2 Medium, 3 Low,
    // 4 Auto; -1 keeps the scene's.
    int qualityPreset = -1;
    // Game window size; 0 keeps the project's.
    uint32_t width = 0, height = 0;
    // Frame-rate limit; -1 keeps the project's, 0 is unlimited.
    int maxFps = -1;
    // Streamed texture memory pool in MiB; 0 keeps the project's.
    float textureBudgetMiB = 0.f;

    // Hardware simulation (testing only; costs real time on this machine).
    // CPU worker threads for engine jobs; 0 keeps the default (all cores
    // but two). Takes effect at game start.
    uint32_t cpuWorkers = 0;
    // Extra main-thread milliseconds per frame (a slower CPU).
    float cpuLoadMs = 0.f;
    // Extra GPU milliseconds per frame from a calibrated compute pass (a
    // slower GPU).
    float gpuLoadMs = 0.f;

    bool SimulatesHardware() const { return cpuWorkers > 0 || cpuLoadMs > 0.f || gpuLoadMs > 0.f; }
    void Normalize();
};

// High, Medium and Low. Their hardware limits are rough stand-ins for a
// mid-range and a low-end machine relative to a fast desktop; tune them per
// project.
std::vector<VideoProfile> DefaultVideoProfiles();

// The profile with this name (case-sensitive), or nullptr.
const VideoProfile* FindVideoProfile(const std::vector<VideoProfile>& profiles, const std::string& name);
}

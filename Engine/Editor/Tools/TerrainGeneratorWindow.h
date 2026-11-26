#pragma once
#include "Core/Components/Terrain/LandscapeGenerator.h"

#include <atomic>
#include <future>
#include <memory>
#include <mutex>
#include <string>

namespace Engine::Editor
{
class EditorState;

// Tools > Terrain generator: regenerates the map's base heights
// (LandscapeGenerator.h: size, relief, seed, erosion, water level, features)
// for the open scene's terrain (its TerrainManager's heightArchive), re-seats
// what stands on it, and reloads.
//
// The scene is saved first; generation runs off the main thread; then the
// scene reloads from disk (never saved in between: it would write its copies
// of the old edits under the new cell data generation). A new map size is
// applied on the reloaded scene (the manager's procedural cells; the archive
// carries the extent), saved, and reloaded once more.
class TerrainGeneratorWindow
{
public:
    void Open(EditorState& state);
    // Every frame (also polls a running generation).
    void Draw(EditorState& state);

private:
    enum class Phase { Idle, Generating, Reloading, Resizing, ReloadingResized };
    void Prefill(EditorState& state);
    void Start(EditorState& state);

    bool m_open = false;
    Phase m_phase = Phase::Idle;
    Engine::Components::LandscapeGeneratorSettings m_settings;
    Engine::Components::LandscapeGeneratorPaths m_paths;
    int m_sceneExtent = 0;          // the archive's half extent as found
    std::string m_status, m_scenePath;
    std::atomic<bool> m_cancel{false};
    std::mutex m_progressMutex;
    float m_fraction = 0.f;
    std::string m_stage;
    std::future<bool> m_job;
    std::string m_jobError;
    Engine::Components::LandscapeGeneratorReport m_report;
    int m_settleFrames = 0;
};
}

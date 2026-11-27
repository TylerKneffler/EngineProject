#pragma once

#include <atomic>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <utility>
#include <vector>

namespace Engine::Components
{
// Terrain changes that can show as a pop (.md/todo.md Step 1, "evidence
// first"): counted as they happen and taken once per frame by the game
// benchmark (frames.jsonl terrain_* fields), so a sudden change in a captured
// frame can be tied to the event that made it. Main thread only writes.
struct TerrainEvents
{
    // VolumeTerrain::SwapIn: chunks whose shown level changed, chunks whose
    // level held but whose seams (a neighbour's level) changed, and steps
    // shown (one per volume per shown step).
    static inline std::atomic<uint32_t> chunkLevelChanges{0};
    static inline std::atomic<uint32_t> chunkSeamChanges{0};
    static inline std::atomic<uint32_t> volumeStepSwaps{0};
    // TerrainManager far volume: node sets swapped in, and nodes in them.
    static inline std::atomic<uint32_t> farSwaps{0};
    static inline std::atomic<uint32_t> farNodesShown{0};
    // A loaded cell handed over from its height mesh to volume chunks.
    static inline std::atomic<uint32_t> cellHandovers{0};

    // Object representation changes (user, 2026-11-26: "keep track of loaded
    // objects -> LOD states and their changes in order to monitor how poor it
    // looks/feels"). Written to frames.jsonl as transition_* by TakeTransitions.
    // Near/far cell handover (Rendering/CellHandover.h): cross-fades started,
    // finished after their fade, and swaps done in one frame (no fade); cells
    // mid-fade; the nearest started or abrupt one, in cells from the camera's.
    static inline std::atomic<uint32_t> handoverStarted{0};
    static inline std::atomic<uint32_t> handoverFaded{0};
    static inline std::atomic<uint32_t> handoverAbrupt{0};
    static inline std::atomic<uint32_t> handoverActive{0};
    static inline std::atomic<int> handoverNearest{INT_MAX};
    // A loaded cell's objects given back to the far impostors (unload or no
    // longer ready): one frame, no fade; and the nearest, in cells.
    static inline std::atomic<uint32_t> uncovered{0};
    static inline std::atomic<int> uncoveredNearest{INT_MAX};
    // Far impostor blocks: rebuilt on the main thread, installed from a
    // worker, and blocks switching between forest clusters and single trees
    // (farClusterDistance: one frame, no fade). Near object blocks rebuilt.
    static inline std::atomic<uint32_t> farBlockRebuilds{0};
    static inline std::atomic<uint32_t> farBlockInstalls{0};
    static inline std::atomic<uint32_t> farClusterFlips{0};
    static inline std::atomic<uint32_t> nearBlockRebuilds{0};
    // The camera's smoothed speed (m/s) and the extra half-width of the
    // instanced LOD fade band it gives (SceneSettings::lodTransitionSeconds).
    static inline std::atomic<float> cameraSpeed{0.f};
    // Cells the handover list wanted this update, and how many did not fit
    // (Rendering/CellHandover.h MaximumCells).
    static inline std::atomic<uint32_t> handoverWanted{0};
    static inline std::atomic<uint32_t> handoverDropped{0};
    static inline std::atomic<float> lodFadeExtraWidth{0.f};

    // ENGINE_TRANSITION_LOG=1: one line per change of one object, for
    // .local/routes/transition_check.py (repeats and other anomalies):
    // "[Transition] f<frame> <kind> <id> d<distance in cells, -1 unknown>".
    static bool LogEnabled()
    {
        static const bool enabled = [] {
            const char* value = std::getenv("ENGINE_TRANSITION_LOG");
            return value && value[0] == '1'; }();
        return enabled;
    }
    static void Log(unsigned long long frame, const char* kind, int x, int z, int distance)
    {
        if (LogEnabled()) std::printf("[Transition] f%llu %s %d,%d d%d\n", frame, kind, x, z, distance);
    }
    static void Nearest(std::atomic<int>& slot, int cells)
    {
        int current = slot.load();
        while (cells < current && !slot.compare_exchange_weak(current, cells)) {}
    }
    // This frame's values, reset for the next (gauges are read, not reset).
    static std::vector<std::pair<const char*, double>> TakeTransitions()
    {
        const auto nearest = [](std::atomic<int>& slot) {
            const int value = slot.exchange(INT_MAX);
            return value == INT_MAX ? -1.0 : static_cast<double>(value); };
        return {
            {"handover_started", handoverStarted.exchange(0)},
            {"handover_faded", handoverFaded.exchange(0)},
            {"handover_abrupt", handoverAbrupt.exchange(0)},
            {"handover_active", handoverActive.load()},
            {"handover_nearest_cells", nearest(handoverNearest)},
            {"uncovered", uncovered.exchange(0)},
            {"uncovered_nearest_cells", nearest(uncoveredNearest)},
            {"far_block_rebuilds", farBlockRebuilds.exchange(0)},
            {"far_block_installs", farBlockInstalls.exchange(0)},
            {"far_cluster_flips", farClusterFlips.exchange(0)},
            {"near_block_rebuilds", nearBlockRebuilds.exchange(0)},
            {"handover_wanted", handoverWanted.load()},
            {"handover_dropped", handoverDropped.load()},
            {"camera_speed", cameraSpeed.load()},
            {"lod_fade_extra_m", lodFadeExtraWidth.load()},
        };
    }
};
}

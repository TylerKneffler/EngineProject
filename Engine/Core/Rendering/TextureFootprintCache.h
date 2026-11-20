#pragma once
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <glm/vec3.hpp>

namespace Engine::Rendering
{
// Per-view memory of instanced texture-footprint results. Projecting every
// placement into the camera and each shadow cascade is the expensive part of
// texture demand; streaming reacts over many frames, so each instance chunk
// is re-projected every few frames (staggered) and its last result is
// re-submitted in between. Owned by the scene; used on the scene-owner thread.
struct TextureFootprintCache
{
    // Frames between re-projections of one chunk. 8 (was 4): texture
    // streaming reacts within ~130 ms at 60 fps, and the re-projection is a
    // measurable share of camera recording on the landscape.
    static constexpr uint64_t RefreshFrames = 8;

    struct ChunkKey
    {
        const void* instanceData = nullptr;
        uint64_t revision = 0;
        uint64_t draw = 0; // hash of the draw state the projection depends on
        size_t begin = 0;
        bool operator==(const ChunkKey& other) const
        {
            return instanceData == other.instanceData && revision == other.revision &&
                draw == other.draw && begin == other.begin;
        }
    };
    struct ChunkKeyHash
    {
        size_t operator()(const ChunkKey& key) const
        {
            uint64_t hash = 1469598103934665603ull;
            const auto mix = [&](uint64_t value) { hash = (hash ^ value) * 1099511628211ull; };
            mix(uint64_t(reinterpret_cast<uintptr_t>(key.instanceData)));
            mix(key.revision);
            mix(key.draw);
            mix(uint64_t(key.begin));
            return size_t(hash);
        }
    };
    struct ChunkResult
    {
        float pixels = 0.f, coverage = 0.f;
        uint64_t due = 0, used = 0;
        // World bounds of the chunk's instances and their largest scale,
        // fixed for a key (instance revision and draw state are in it).
        bool boundsValid = false;
        glm::vec3 boundsMin{0.f}, boundsMax{0.f};
        float maxScale = 0.f;
    };
    struct View
    {
        uint64_t frame = 0, lastRequest = 0;
        std::unordered_map<ChunkKey, ChunkResult, ChunkKeyHash> chunks;
    };
    uint64_t requests = 0;
    std::unordered_map<uintptr_t, View> views;

    void Clear() { views.clear(); }
};
}

#pragma once

#include <glm/glm.hpp>
#include <cstdint>
#include <limits>
#include <memory>
#include <vector>

namespace Engine::Components
{
// Trails drawn on a TerrainManager's ground: polylines in the manager's
// space (x, z), each with its track shape. Scene objects publish them
// (TerrainTrail scripts: a SplinePath sampled into a polyline), the manager
// gathers them into one network, and the terrain shades them per pixel on
// every level (Object.hlsl TerrainTrailSample), carves them into procedural
// cells as they load, and ground cover and placement rules keep off them.
struct TerrainTrail
{
    std::vector<glm::vec2> points;
    // Half-widths from the centre line, metres: full track inside core,
    // none past edge.
    float core = 1.6f, edge = 4.2f;
    float strength = .9f;
    // Metres the track sinks into procedural cells at full share (0: none).
    float depth = .12f;
    // Paint layer the track shows, and the layers it may run over (bit mask;
    // the track fades out over the others, such as rock and snow).
    int layer = 1;
    uint32_t crossableLayers = 0xffffffffu;
    // Far tiles (no paint): the track's colour there; negative takes the
    // manager's far palette colour of the layer.
    glm::vec3 farColor{-1.f};
};

// An immutable set of trails with a bin grid over their segments, shared by
// the main thread, streaming workers (carving) and the renderer.
class TerrainTrailNetwork
{
public:
    struct Segment
    {
        glm::vec2 a{0.f}, b{0.f};
        uint32_t trail = 0;
    };
    // Side of a bin, metres (at least; larger networks take larger bins so
    // the grid stays under MaxBins); each bin lists the segments within
    // reach of it.
    static constexpr float BinSize = 16.f;
    static constexpr int MaxBins = 1 << 22;
    // Reach past a segment's edge that its bins still list, so the far
    // tiles' widened soft edge (pixel footprint) keeps its tail.
    static constexpr float BinMargin = 8.f;

    static std::shared_ptr<const TerrainTrailNetwork> Build(std::vector<TerrainTrail> trails);

    bool Empty() const { return m_segments.empty(); }
    uint64_t Signature() const { return m_signature; }
    const std::vector<TerrainTrail>& Trails() const { return m_trails; }
    const std::vector<Segment>& Segments() const { return m_segments; }

    struct Sample
    {
        float track = 0.f; // 0..1: 1 inside core, 0 past edge
        float share = 0.f; // track times the trail's strength
        float metres = std::numeric_limits<float>::infinity(); // to the nearest centre line
        int trail = -1;    // the trail giving the share
    };
    // The track at a position (manager space), as the shader shades it at
    // full resolution (no paint rules, no pixel footprint).
    Sample Evaluate(float x, float z) const;
    float Distance(float x, float z) const { return Evaluate(x, z).metres; }
    float Share(float x, float z) const { return Evaluate(x, z).share; }

    // Sinks a cell's heights (vertex grid at origin, spacing cellWidth x
    // cellDepth) by each trail's depth times its share and the crossable
    // share of the vertex's paint (weights: layerCount per vertex). True when
    // a height changed.
    bool Carve(float originX, float originZ, int countX, int countY, float cellWidth, float cellDepth,
        const std::vector<float>& weights, size_t layerCount, std::vector<float>& heights) const;
    // Whether any carving trail reaches the rectangle [low, high] (x, z).
    bool CarvesArea(glm::vec2 low, glm::vec2 high) const;

    // The renderer's copy: one RGBA32F texture of `width` texels per row.
    // Texels [0, (bins + 4) / 4): each bin's first list entry, four bins per
    // texel, row-major, with one more start after the last bin (a bin's
    // entries run to the next bin's start); then the list (segment indices, four per texel); then three texels per
    // segment: (a.x, a.y, b.x, b.y), (core, edge, strength, layer * 64 +
    // crossable mask), (far colour, 0). Integers are stored as float values.
    struct GpuLayout
    {
        std::vector<float> texels; // RGBA per texel
        uint32_t width = 0, height = 0;
        glm::vec4 grid{0.f};       // origin x, origin z, 1 / bin size, bins x
        glm::uvec4 keys{0u};       // bins z, list offset, segment offset, texture width
    };
    GpuLayout PackGpu(const glm::vec3* layerFarColors, size_t layerColorCount) const;

private:
    std::vector<TerrainTrail> m_trails;
    std::vector<Segment> m_segments;
    glm::vec2 m_origin{0.f};
    float m_binSize = BinSize;
    int m_binsX = 0, m_binsZ = 0;
    std::vector<uint32_t> m_binStart; // binsX * binsZ + 1 offsets into m_binList
    std::vector<uint32_t> m_binList;
    uint64_t m_signature = 0;
};
}

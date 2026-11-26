#pragma once

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

namespace Engine::Components
{
class Terrain;

// Where dense ground cover grows (grass, ferns, undergrowth, flowers): a
// density 0..1 per foliage layer (named like the cell's object layer that
// grows it), in 3D, so it holds on any surface: open ground, cliff faces,
// cave ceilings and the ground under an arch alike. The plants themselves
// are never stored; they are generated where the density allows, the same
// way every time (GrassField).
//
// Only edits are stored, as offsets from the layer's baseline: the density
// the component growing it gives (TerrainFoliageSource, e.g. the paint rule
// "meadow paint, facing up"), clamped to 0..1 after the offset. An offset of
// -1 clears a spot whatever its baseline, +1 fills it; a voxel never painted
// is exactly the baseline, so a stroke changes plants only where it reaches.
// Memory grows only where someone painted.
//
// Storage: voxels of VoxelSize metres in the cell's own space, voxel (i, j, k)
// centred at ((i, j, k) + 0.5) * VoxelSize, grouped in chunks of ChunkSide^3
// one-byte values. A cell's chunks live in <cellDataDirectory>/<x>_<z>.foliage:
//   u64 magic "TRNRFOL1", u32 version 1, u32 raw bytes, then zlib of:
//   u32 layer count, per layer: u32 name length, name, u32 chunk count,
//   per chunk: i16 x, i16 y, i16 z (chunk coordinates), 512 bytes (x fastest,
//   then y, then z): offset = (byte - 128) / 127.
class TerrainFoliageCell
{
public:
    static constexpr float VoxelSize = 1.f;
    static constexpr int ChunkSide = 8;
    static constexpr int ChunkVoxels = ChunkSide * ChunkSide * ChunkSide;
    using ChunkKey = std::tuple<int16_t, int16_t, int16_t>;
    using Chunk = std::array<uint8_t, ChunkVoxels>;
    struct Layer
    {
        std::map<ChunkKey, Chunk> chunks;
    };
    std::map<std::string, Layer> layers;

    bool Empty() const;
    const Layer* Find(const std::string& layer) const;

    static glm::ivec3 VoxelOf(const glm::vec3& local);
    static glm::vec3 VoxelCentre(const glm::ivec3& voxel);
    static ChunkKey ChunkOf(const glm::ivec3& voxel);
    static int IndexInChunk(const glm::ivec3& voxel);
    // A stored voxel's offset (-1..1), or none when its chunk is not stored.
    static bool Voxel(const Layer& layer, const glm::ivec3& voxel, float& offset);
    static uint8_t EncodeOffset(float offset);
    static float DecodeOffset(uint8_t stored) { return (static_cast<int>(stored) - 128) / 127.f; }

    // Density at a point of the cell's space: the baseline there plus the
    // offsets, trilinear between voxel centres (0 where none is stored).
    using Baseline = std::function<float(const glm::vec3& local)>;
    static float Sample(const Layer* layer, const glm::vec3& local, const Baseline& baseline);
    // Whether any stored chunk of the layer reaches the box (cell space).
    static bool Touches(const Layer* layer, const glm::vec3& low, const glm::vec3& high);

    // A missing file is an empty cell (true); a malformed one fails.
    static bool Load(const std::string& path, TerrainFoliageCell& cell);
    // An empty cell removes the file.
    static bool Save(const std::string& path, const TerrainFoliageCell& cell);
    uint64_t Signature() const;
};

// How a foliage brush changes the density.
struct TerrainFoliageStroke
{
    enum class Mode { Add, Erase, Set };
    Mode mode = Mode::Add;
    glm::vec3 centre{0.f};   // manager space
    float radius = 4.f;
    float strength = .5f;    // per dab, 0..1
    float target = 1.f;      // Set: the density painted toward
    float falloff = .5f;     // share of the radius that fades (0: hard edge)
    // Only voxels this close to the surface change (metres): the density
    // matters where plants stand, and a thin shell keeps a stroke on a wall
    // from reaching the ground behind it.
    float shell = 1.5f;
    // Optional: how much the stroke acts at a voxel (0..1), from the cell,
    // the voxel's centre in its space and the surface normal there (a
    // brush's rules: height and slope bands, paint layers, trails).
    std::function<float(const Terrain& cell, const glm::vec3& local, const glm::vec3& normal)> weight;
};

// The component that grows a foliage layer gives its baseline: the density
// before any edit, at a point of the cell's space with the surface normal
// there (e.g. GrassField: its paint layers, facing up).
class TerrainFoliageSource
{
public:
    virtual ~TerrainFoliageSource() = default;
    virtual float FoliageBaseline(const Terrain& cell, const glm::vec3& local, const glm::vec3& normal) const = 0;
    // The foliage layer it reads (its object layer's name unless it borrows
    // another's, as a far ring reads the meadow's).
    virtual std::string FoliageLayerName() const = 0;
};
}

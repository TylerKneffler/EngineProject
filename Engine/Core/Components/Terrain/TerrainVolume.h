#pragma once

#include "Core/Components/Mesh/Mesh.h"
#include <glm/glm.hpp>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace Engine::Components
{
// Terrain as a density volume (.md/todo.md, "Ground conforms to stamps",
// decision of 2026-11-23): a regular 3D grid of signed density, positive
// inside the ground and negative in the air, so the surface is where it
// crosses zero. Edits are unions and subtractions of shapes, which merge
// where they overlap, and any shape holds: overhangs, arches, caves. The
// surface is extracted as a mesh (MeshSurfaceNets).
//
// Storage is sparse: the grid is cut into chunks of ChunkSamples^3 samples,
// and a chunk wholly inside the ground or wholly in the air keeps one value
// instead of its samples, so a deep landscape costs memory only near its
// surface. Built offline (tools/terrain_volume.py) and read from a .volume
// file, little endian:
//   u64 magic "TRNRVOL1", u32 version, i32 size x, y, z (samples),
//   f32 spacing (metres between samples), f32 origin x, y, z (the first
//   sample's position in the object's space), f32 units per metre, then
//   version 1: x*y*z i16 densities (metres of signed distance times units
//              per metre), x fastest, then z, then y;
//   version 2: u32 chunk samples (C), then per chunk (x fastest, then z,
//              then y) u8 kind: 0 = uniform, followed by one i16; 1 = data,
//              followed by C^3 i16 (x fastest, then z, then y; samples past
//              the grid's end repeat its last).
struct TerrainVolume
{
    int sizeX = 0, sizeY = 0, sizeZ = 0;
    float spacing = 1.f;
    glm::vec3 origin{0.f};
    float unitsPerMetre = 256.f;
    int chunkSamples = 32;
    int chunksX = 0, chunksY = 0, chunksZ = 0;
    struct Chunk
    {
        int16_t uniform = 0;
        std::vector<int16_t> samples; // empty: every sample is `uniform`
    };
    std::vector<Chunk> chunks;        // x fastest, then z, then y

    // Density in metres at a sample (clamped to the grid).
    float At(int x, int y, int z) const;
    // Trilinear density in metres at a position in the object's space.
    float Sample(const glm::vec3& position) const;
    // Bytes held by the sample data.
    size_t MemoryBytes() const;
    // Loads a .volume file once per path (thread-safe); null when missing or
    // malformed.
    static std::shared_ptr<const TerrainVolume> Load(const std::string& path);
};

// The zero surface of a block of the volume as an indexed triangle mesh, by
// surface nets: one vertex in every cell the surface passes through (the
// mean of the cell's edge crossings), a quad across every sign-changing grid
// edge whose base sample lies in the block, joining the four cells around
// it, wound to face the air. Normals are the density's gradient. Positions
// are in the object's space.
//
// The block is `cells` cells from sample `first`, each cell `stride` samples
// wide (a coarser level of detail samples the grid at that stride). Blocks
// that tile the volume at one stride meet exactly. `skirt` > 0 adds a strip
// along the block's open edges, pushed that many metres into the ground, so
// blocks at different strides leave no gap where their edges disagree.
void MeshSurfaceNets(const TerrainVolume& volume, const glm::ivec3& first,
    const glm::ivec3& cells, int stride, float skirt,
    std::vector<Mesh::Vertex>& vertices, std::vector<uint32_t>& indices);
// The whole volume at full detail, no skirts.
void MeshSurfaceNets(const TerrainVolume& volume,
    std::vector<Mesh::Vertex>& vertices, std::vector<uint32_t>& indices);
}

#pragma once

#include <glm/glm.hpp>
#include <cstdint>
#include <type_traits>
#include <vector>

// Portable CPU-side vertex hierarchy shared by importers, physics and renderers:
//
// Vertex
// |-- SurfaceVertex
// |   `-- AnimationVertex
// `-- TerrainVertex
namespace Engine::Model
{
struct Vertex
{
    float pos[3];
    float normal[3];
    float uv[2];
};

// Material attributes shared by imported static and animated surfaces.
struct SurfaceVertex : Vertex
{
    float tangent[4];
    float uv1[2];
    float color[4] { 1.f, 1.f, 1.f, 1.f };
};

// Full imported-mesh stream. Animation data is kept out of the base and
// surface formats so specialized meshes do not pay for unused influences.
struct AnimationVertex : SurfaceVertex
{
    float joints0[4] {};
    float weights0[4] {};
    float joints1[4] {};
    float weights1[4] {};
};

// Terrain never uses skeletal influences, morph streams, a secondary UV set,
// or imported tangents.  Keep its persistent representation small and derive
// a stable tangent basis in the terrain vertex shader.
struct TerrainVertex : Vertex
{
    float color[4] { 1.f, 1.f, 1.f, 1.f };
};

struct TerrainMeshData
{
    std::vector<TerrainVertex> vertices;
    std::vector<uint32_t> indices;
};

static_assert(sizeof(TerrainVertex) == 48,
    "Packed terrain vertex layout must remain render-compatible");
static_assert(sizeof(Vertex) == 32,
    "Base vertex layout must remain render-compatible");
static_assert(sizeof(SurfaceVertex) == 72,
    "Surface vertex layout must remain render-compatible");
static_assert(sizeof(AnimationVertex) == 136,
    "Animation vertex layout must preserve native mesh compatibility");
static_assert(std::is_trivially_copyable_v<Vertex> &&
    std::is_trivially_copyable_v<SurfaceVertex> &&
    std::is_trivially_copyable_v<AnimationVertex> &&
    std::is_trivially_copyable_v<TerrainVertex>,
    "Vertex types must remain safe for direct graphics-buffer uploads");

struct MorphTarget
{
    std::vector<glm::vec3> positions;
    std::vector<glm::vec3> normals;
    std::vector<glm::vec3> tangents;
};
}

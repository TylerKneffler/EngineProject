#pragma once

#include <glm/glm.hpp>
#include <cstdint>
#include <array>
#include <string>
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
    // PMX stores four components for the primary and four additional UV sets.
    // The first two components of sets 0 and 1 also live in uv/uv1 for the
    // existing material pipeline. These channels retain the full source data.
    float uvChannels[5][4] {};
    // PMX SDEF uses two bone-specific radii and a shared center. The fourth
    // center component selects BDEF (0), SDEF (3), or QDEF (4).
    float sdefCenterAndMode[4] {};
    float sdefRadius0[4] {};
    float sdefRadius1[4] {};
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
static_assert(sizeof(AnimationVertex) == 264,
    "Animation vertex layout must preserve native mesh compatibility");
static_assert(std::is_trivially_copyable_v<Vertex> &&
    std::is_trivially_copyable_v<SurfaceVertex> &&
    std::is_trivially_copyable_v<AnimationVertex> &&
    std::is_trivially_copyable_v<TerrainVertex>,
    "Vertex types must remain safe for direct graphics-buffer uploads");

struct MorphTarget
{
    std::string name;
    std::vector<glm::vec3> positions;
    std::vector<glm::vec3> normals;
    std::vector<glm::vec3> tangents;
    // xy affect the primary UV set; zw affect the first additional UV set.
    std::vector<glm::vec4> uv;
    // Full four-component offsets for primary UV and additional UV1-4.
    std::array<std::vector<glm::vec4>, 5> uvChannels;
};
}

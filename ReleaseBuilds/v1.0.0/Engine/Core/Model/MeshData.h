#pragma once

#include <glm/glm.hpp>
#include <vector>

// Portable CPU-side Vertex format shared by importers, physics and renderers.
namespace Engine::Model
{
struct Vertex
{
    float pos[3];
    float normal[3];
    float uv[2];
    float tangent[4];
    float uv1[2];
    float color[4] { 1.f, 1.f, 1.f, 1.f };
    float joints0[4] {};
    float weights0[4] {};
    float joints1[4] {};
    float weights1[4] {};
};

struct MorphTarget
{
    std::vector<glm::vec3> positions;
    std::vector<glm::vec3> normals;
    std::vector<glm::vec3> tangents;
};
}


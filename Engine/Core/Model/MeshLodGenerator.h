#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "Core/Compoonents/Mesh/Mesh.h"

namespace Engine::Model
{
class MeshLodGenerator
{
public:
    struct Result { uint32_t triangles = 0; std::string path; };
    // Spatial vertex clustering for static triangle meshes. Each successive
    // LOD targets targetRatio of the preceding LOD's triangles.
    static bool Generate(const std::string& sourcePath, int lodCount,
        float targetRatio, std::vector<Result>& results, std::string& error,
        const std::string& outputSuffix = {});
private:
    static bool Simplify(const std::vector<Engine::Components::Mesh::Vertex>& vertices,
        const std::vector<uint32_t>& indices, uint32_t targetTriangles,
        std::vector<Engine::Components::Mesh::Vertex>& outputVertices,
        std::vector<uint32_t>& outputIndices);
};
}

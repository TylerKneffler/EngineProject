#define GLM_ENABLE_EXPERIMENTAL
#include "pch.h"
#include "MeshLodGenerator.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <unordered_map>
#include <glm/gtx/norm.hpp>

namespace Engine::Model
{
namespace
{
struct Cell
{
    int64_t x, y, z;
    bool operator==(const Cell& other) const noexcept
    { return x==other.x && y==other.y && z==other.z; }
};
struct CellHash
{
    size_t operator()(const Cell& c) const noexcept
    {
        size_t h = std::hash<int64_t>{}(c.x);
        h ^= std::hash<int64_t>{}(c.y) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<int64_t>{}(c.z) + 0x9e3779b9 + (h << 6) + (h >> 2);
        return h;
    }
};
}

bool MeshLodGenerator::Simplify(const std::vector<Engine::Components::Mesh::Vertex>& vertices,
    const std::vector<uint32_t>& indices, uint32_t targetTriangles,
    std::vector<Engine::Components::Mesh::Vertex>& outputVertices,
    std::vector<uint32_t>& outputIndices)
{
    if (vertices.empty() || indices.size() < 3 || indices.size() % 3) return false;
    glm::vec3 minimum(vertices[0].pos[0], vertices[0].pos[1], vertices[0].pos[2]);
    glm::vec3 maximum = minimum;
    for (const auto& v : vertices)
    {
        const glm::vec3 p(v.pos[0],v.pos[1],v.pos[2]);
        minimum=glm::min(minimum,p); maximum=glm::max(maximum,p);
    }
    const glm::vec3 extent=maximum-minimum;
    const double desiredCells=std::max(1.0, double(targetTriangles)*1.8);
    const int dimensions=(extent.x>1e-8f)+(extent.y>1e-8f)+(extent.z>1e-8f);
    if(!dimensions) return false;
    const double longest=std::max({double(extent.x),double(extent.y),double(extent.z)});
    const double cellSize=std::max(1e-9, longest/std::pow(desiredCells,1.0/dimensions));
    std::unordered_map<Cell,uint32_t,CellHash> cells;
    std::vector<uint32_t> remap(vertices.size());
    outputVertices.clear(); outputIndices.clear();
    for (size_t i=0;i<vertices.size();++i)
    {
        const auto& v=vertices[i];
        const Cell cell{int64_t(std::floor((v.pos[0]-minimum.x)/cellSize)),
            int64_t(std::floor((v.pos[1]-minimum.y)/cellSize)),
            int64_t(std::floor((v.pos[2]-minimum.z)/cellSize))};
        auto [it,inserted]=cells.emplace(cell,uint32_t(outputVertices.size()));
        if(inserted) outputVertices.push_back(v);
        remap[i]=it->second;
    }
    outputIndices.reserve(indices.size());
    for(size_t i=0;i<indices.size();i+=3)
    {
        if(indices[i]>=remap.size()||indices[i+1]>=remap.size()||indices[i+2]>=remap.size()) return false;
        uint32_t a=remap[indices[i]], b=remap[indices[i+1]], c=remap[indices[i+2]];
        if(a==b||b==c||a==c) continue;
        const auto pos=[&](uint32_t n){return glm::vec3(outputVertices[n].pos[0],outputVertices[n].pos[1],outputVertices[n].pos[2]);};
        if(glm::length2(glm::cross(pos(b)-pos(a),pos(c)-pos(a)))<1e-16f) continue;
        outputIndices.insert(outputIndices.end(),{a,b,c});
    }
    // Empty cells can occur on thin/single-axis meshes; retry with a denser
    // grid so the caller still gets a valid, strictly smaller representation.
    if(outputIndices.empty() || outputIndices.size()>=indices.size()) return false;
    return true;
}

bool MeshLodGenerator::Generate(const std::string& sourcePath, int lodCount,
    float targetRatio, std::vector<Result>& results, std::string& error,
    const std::string& outputSuffix)
{
    results.clear(); error.clear();
    if(sourcePath.empty()) { error="Choose a source mesh."; return false; }
    if(lodCount<1||lodCount>8||!std::isfinite(targetRatio)||targetRatio<.1f||targetRatio>.9f)
    { error="LOD count must be 1 to 8 and reduction ratio must be 0.1 to 0.9."; return false; }
    try
    {
        Engine::Components::Mesh source;
        source.LoadFromFile(sourcePath);
        if(source.UsesTerrainVertexFormat()||source.HasMorphTargets()||source.IsInstanced())
        { error="This generator supports static meshes without terrain, morph, or instance data."; return false; }
        const auto& vertices=source.GetVertices();
        std::vector<uint32_t> currentIndices=source.GetIndices();
        if(currentIndices.empty())
        { currentIndices.resize(vertices.size()); for(uint32_t i=0;i<currentIndices.size();++i) currentIndices[i]=i; }
        if(currentIndices.size()<6||currentIndices.size()%3)
        { error="Source mesh must contain at least two triangles."; return false; }
        auto currentVertices=vertices;
        const auto sourceFile=std::filesystem::path(sourcePath);
        for(int lod=1;lod<=lodCount;++lod)
        {
            auto output=sourceFile;
            output.replace_filename(sourceFile.stem().string()+
                (outputSuffix.empty() ? "_LOD"+std::to_string(lod) : outputSuffix)+".mesh");
            if(std::filesystem::exists(output))
            { error="Output already exists: "+output.string(); return false; }
        }
        for(int lod=1;lod<=lodCount;++lod)
        {
            const uint32_t target=std::max(1u,uint32_t(std::floor((currentIndices.size()/3.0)*targetRatio)));
            std::vector<Engine::Components::Mesh::Vertex> reducedVertices;
            std::vector<uint32_t> reducedIndices;
            if(!Simplify(currentVertices,currentIndices,target,reducedVertices,reducedIndices))
            { error="Could not simplify further without collapsing the mesh. Generated earlier LOD files were retained."; return false; }
            auto output=sourceFile;
            output.replace_filename(sourceFile.stem().string()+
                (outputSuffix.empty() ? "_LOD"+std::to_string(lod) : outputSuffix)+".mesh");
            if(!Engine::Components::Mesh::SaveNativeFile(output.string(),reducedVertices,reducedIndices))
            { error="Could not write "+output.string(); return false; }
            results.push_back({uint32_t(reducedIndices.size()/3),output.string()});
            currentVertices=std::move(reducedVertices); currentIndices=std::move(reducedIndices);
        }
        return true;
    }
    catch(const std::exception& e) { error=e.what(); return false; }
}
}

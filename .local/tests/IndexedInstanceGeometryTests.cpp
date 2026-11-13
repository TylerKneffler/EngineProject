#include "Core/Compoonents/Mesh/Mesh.h"
#include "Core/Rendering/SharedMeshBuffers.h"
#include "Core/Rendering/IndexedMeshPreparation.h"
#include "Core/Compoonents/Mesh/MeshInstanceLayerData.h"
#include <cstdlib>
#include <cstring>
#include <stdexcept>

using Engine::Components::Mesh;
using namespace Engine::Graphics;
static void require(bool condition) { if (!condition) std::abort(); }

class Buffer final : public IGraphicsBuffer
{
public:
    Usage usage;
    AccessMode access;
    std::vector<unsigned char> bytes;
    Buffer(Usage u, AccessMode a, uint64_t size, const void* data) : usage(u), access(a), bytes(size)
    { if (data) std::memcpy(bytes.data(), data, size); }
    Usage GetUsage() const override { return usage; }
    AccessMode GetAccessMode() const override { return access; }
    uint64_t GetSize() const override { return bytes.size(); }
    void* Map() override { return bytes.data(); }
    void Unmap() override {}
    void* GetNativeHandle() const override { return nullptr; }
};
class Factory final : public IGraphicsBufferFactory
{
public:
    bool failIndex = false;
    std::unique_ptr<IGraphicsBuffer> CreateBuffer(IGraphicsBuffer::Usage usage,
        IGraphicsBuffer::AccessMode access, uint64_t size, const void* data, uint32_t = 0) override
    {
        if (failIndex && usage == IGraphicsBuffer::Usage::IndexBuffer) return {};
        return std::make_unique<Buffer>(usage, access, size, data);
    }
};

int main()
{
    // Placements on opposite sides of tile boundaries share render blocks,
    // but retain every original identity and spatial ordering. Repacking after
    // erase/paint cannot mutate a previously published placement list.
    struct Placement { uint64_t id; };
    std::vector<Placement> placementStorage(8200);
    std::map<std::pair<int,int>,std::vector<const Placement*>> tiles;
    for (size_t i=0;i<placementStorage.size();++i)
    {
        placementStorage[i].id=i+1;
        tiles[{int(i/1000)-4,0}].push_back(&placementStorage[i]);
    }
    auto blocks=Engine::Components::MeshInstanceLayerData::PackPlacementBlocks(tiles);
    require(blocks.size()==3 && blocks[0].size()==4096 && blocks[1].size()==4096 && blocks[2].size()==8);
    size_t next=0;
    for (const auto& block:blocks) for (auto* placement:block)
        require(placement==&placementStorage[next++]);
    tiles.begin()->second.erase(tiles.begin()->second.begin());
    auto edited=Engine::Components::MeshInstanceLayerData::PackPlacementBlocks(tiles);
    require(edited[0][0]->id==2 && blocks[0][0]->id==1 && edited.back().size()==7);
    tiles.clear();
    require(Engine::Components::MeshInstanceLayerData::PackPlacementBlocks(tiles).empty());
    bool rejectedEmptyCapacity=false;
    try { Engine::Components::MeshInstanceLayerData::PackPlacementBlocks(tiles,0); }
    catch (const std::invalid_argument&) { rejectedEmptyCapacity=true; }
    require(rejectedEmptyCapacity);
    Factory factory;
    std::vector<Mesh::Vertex> vertices(4);
    vertices[1].pos[0] = 1; vertices[2].pos[1] = 1;
    vertices[3].pos[0] = 1; vertices[3].pos[1] = 1;
    const std::vector<uint32_t> indices{0, 1, 2, 2, 1, 3};
    std::vector<Mesh::Vertex> expanded;
    for (auto index : indices) expanded.push_back(vertices[index]);
    const auto original = expanded;
    std::vector<uint32_t> compactIndices;
    require(Engine::Rendering::IndexTriangleList(expanded, compactIndices));
    require(expanded.size() == 4 && compactIndices == indices);
    for (size_t i = 0; i < original.size(); ++i)
        require(std::memcmp(&original[i], &expanded[compactIndices[i]], sizeof(Mesh::Vertex)) == 0);
    auto seam = original;
    seam[3].uv[0] = .5f;
    compactIndices.clear();
    require(Engine::Rendering::IndexTriangleList(seam, compactIndices));
    require(seam.size() == 5 && compactIndices[2] != compactIndices[3]);
    auto unshared = vertices;
    unshared.resize(3);
    compactIndices.clear();
    require(!Engine::Rendering::IndexTriangleList(unshared, compactIndices));
    require(unshared.size() == 3 && compactIndices.empty());
    auto source = std::make_shared<Mesh>();
    require(source->SetIndexedGeometry(vertices, indices));
    auto prepared = Engine::Rendering::PrepareMeshBuffer(indices.data(), indices.size()*sizeof(uint32_t));
    auto vertexBuffer = factory.CreateStaticVertexBuffer(vertices.size()*sizeof(Mesh::Vertex), vertices.data());
    source->InstallPreparedStaticVertexBuffer(std::move(vertexBuffer), &factory, &prepared);
    require(source->IsReady() && source->GetVertexCount() == 4 && source->GetIndexCount() == 6);
    require(source->GetIndexBuffer() && source->GetIndexBuffer()->GetSize() == 24);
    require(std::memcmp(source->GetIndexBuffer()->Map(), indices.data(), 24) == 0);

    // Both publication paths must share the same geometry, including indices.
    auto second = std::make_shared<Mesh>();
    require(second->SetIndexedGeometry(vertices, indices));
    second->CreateBuffer(&factory);
    require(second->GetIndexBuffer() == source->GetIndexBuffer());
    auto instances = std::make_shared<Mesh::InstanceData>();
    instances->transforms.resize(2);
    Mesh batch;
    batch.SetInstancedGeometry(source, instances, {{0, 6, 0, 2}}, {0,0,0}, {1,1,0});
    require(batch.IsReady() && batch.GetIndexCount() == 6 && batch.GetVertexCount() == 4);
    require(batch.GetIndexBufferOwner() == source->GetIndexBufferOwner());
    require(batch.GetInstanceRanges()[0].vertexCount == 6);
    // GPU input bounds must describe one tree, not the whole placement tile.
    Mesh distantBatch;
    instances->transforms[0].rows[0] = {1,0,0,100};
    instances->transforms[0].rows[1] = {0,1,0,50};
    instances->transforms[0].rows[2] = {0,0,1,200};
    instances->transforms[1] = instances->transforms[0];
    instances->transforms[1].rows[0].w = 300;
    distantBatch.SetInstancedGeometry(source, instances, {{0,6,0,2}}, {100,50,200}, {301,51,200});
    glm::vec3 low, high;
    require(distantBatch.GetInstanceGeometryBounds(low, high));
    require(low == glm::vec3(0,0,0) && high == glm::vec3(1,1,0));
    distantBatch.foliageWindStrength = .5f;
    require(distantBatch.GetInstanceGeometryBounds(low, high));
    require(low == glm::vec3(-.75f) && high == glm::vec3(1.75f,1.75f,.75f));
    distantBatch.deformationBoundsPadding = 2;
    require(distantBatch.GetInstanceGeometryBounds(low, high));
    require(low == glm::vec3(-2) && high == glm::vec3(3,3,2));
    // Resident variants inherit the draw's wind/deformation policy even when
    // their own scalar settings are default-initialized (e.g. grass LODs).
    require(batch.GetInstanceGeometryBounds(low, high, &distantBatch));
    require(low == glm::vec3(-2) && high == glm::vec3(3,3,2));
    require(!source->GetInstanceGeometryBounds(low, high));
    bool rejected = false;
    try { batch.SetInstancedGeometry(source, instances, {{0, 9, 0, 2}}, {0,0,0}, {1,1,0}); }
    catch (const std::invalid_argument&) { rejected = true; }
    require(rejected);

    // A failed index upload must not publish an apparently ready vertex-only mesh.
    Factory failing;
    failing.failIndex = true;
    Mesh failed;
    require(failed.SetIndexedGeometry(vertices, indices));
    bool threw = false;
    try { failed.InstallPreparedStaticVertexBuffer(
        failing.CreateStaticVertexBuffer(vertices.size()*sizeof(Mesh::Vertex), vertices.data()), &failing); }
    catch (const std::runtime_error&) { threw = true; }
    require(threw && !failed.IsReady());

    Mesh nonindexed;
    vertices.resize(3);
    require(nonindexed.SetAuthoredVertices(vertices));
    nonindexed.InstallPreparedStaticVertexBuffer(
        factory.CreateStaticVertexBuffer(vertices.size()*sizeof(Mesh::Vertex), vertices.data()), &factory);
    require(nonindexed.IsReady() && !nonindexed.GetIndexBuffer());
}

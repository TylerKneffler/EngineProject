#pragma once
#include "Core/Component.h"
#include "Core/Compoonents/Obj/Mesh.h"
#include <glm/glm.hpp>
#include <cstdint>
#include <string>
#include <vector>
#include <memory>

namespace Engine::Components
{
class Material;
class Terrain : public Engine::Core::Component
{
public:
    struct Layer
    {
        std::string material;
        float minHeight = 0.f;
        float maxHeight = 10.f;
        glm::vec3 previewColor{.35f, .55f, .25f};
    };
    enum class Brush { Raise, Lower, Average, Blend, Paint };
    struct Prepared
    {
        int cellsX = 0, cellsY = 0, lodStride = 1;
        float cellWidth = 1.f, cellDepth = 1.f, textureTiling = 4.f;
        std::vector<float> heights, weights;
        std::vector<Layer> layers;
        std::vector<Mesh::Vertex> vertices;
        std::vector<uint32_t> indices;
        std::vector<std::shared_ptr<Material>> materials;
        std::vector<std::string> materialPaths;
    };
    Terrain();
    int cellsX = 32, cellsY = 32;
    float cellWidth = 1.f, cellDepth = 1.f;
    float textureTiling = 4.f;
    std::vector<float> heights;
    std::vector<Layer> layers;
    std::vector<float> weights; // one weight per vertex and layer
    void Resize(int x, int y, float width, float depth);
    void AssignLayersByHeight();
    bool ApplyBrush(float x, float z, float radius, float strength,
        Brush brush, int layer = 0);
    bool ApplyBrushFromLocalRay(const glm::vec3& nearPoint,
        const glm::vec3& farPoint, float radius, float strength,
        Brush brush, int layer = 0);
    void Rebuild();
    void SetMeshLodStride(int stride);
    int GetMeshLodStride() const { return m_meshLodStride; }
    Prepared TakePrepared();
    bool InstallPrepared(Prepared prepared, IGraphicsProvider* provider);
    void FinalizePreparedGraphics(IGraphicsProvider* provider);
    // Static generated cells can restore their CPU mesh without sampling noise
    // or rebuilding vertices. The graphics buffers are uploaded on load.
    bool SaveCache(const std::string& path, uint64_t signature) const;
    bool LoadCache(const std::string& path, uint64_t signature,
        IGraphicsProvider* provider);
    const std::vector<std::shared_ptr<Material>>& GetLayerMaterials() const { return m_materials; }
    JsonValue Serialize() const override;
    void Deserialize(const JsonValue& value) override;
    void OnAfterDeserialize(IGraphicsProvider* provider) override;
    bool DrawProperties(::Engine::Editor::IEditorUi& ui) override;
private:
    IGraphicsProvider* m_provider = nullptr;
    int m_meshLodStride = 1;
    std::vector<std::shared_ptr<Material>> m_materials;
    std::vector<std::string> m_materialPaths;
    void RefreshMaterials();
    void NormalizeWeights();
};
}

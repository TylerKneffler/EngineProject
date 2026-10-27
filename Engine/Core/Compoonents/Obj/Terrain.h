#pragma once
#include "Core/Component.h"
#include "Core/Compoonents/Obj/Mesh.h"
#include <glm/glm.hpp>
#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include <array>

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
    struct BakedAppearance
    {
        float normal[3];
        float color[3];
    };
    struct BakedLod
    {
        std::vector<uint32_t> sourceVertices;
        std::vector<uint32_t> indices;
    };
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
        std::vector<BakedAppearance> bakedAppearance;
        std::array<BakedLod, 4> bakedLods;
        uint8_t bakedTextureMask = 0;
    };
    Terrain();
    int cellsX = 32, cellsY = 32;
    float cellWidth = 1.f, cellDepth = 1.f;
    float textureTiling = 4.f;
    std::vector<float> heights;
    std::vector<Layer> layers;
    std::vector<float> weights; // one weight per vertex and layer
    std::string bakedAssetPath;
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
    uint64_t GetBakedCpuMemoryBytes() const;
    Prepared TakePrepared();
    bool InstallPrepared(Prepared prepared, IGraphicsProvider* provider);
    void FinalizePreparedGraphics(IGraphicsProvider* provider);
    // Static generated cells can restore their CPU mesh without sampling noise
    // or rebuilding vertices. The graphics buffers are uploaded on load.
    bool SaveCache(const std::string& path, uint64_t signature,
        bool includeBakedLods = false) const;
    bool LoadCache(const std::string& path, uint64_t signature,
        IGraphicsProvider* provider);
    bool LoadCacheRange(const std::string& path, uint64_t signature,
        IGraphicsProvider* provider, uint64_t offset, uint64_t size);
    static bool ReadCacheHeights(const std::string& path, uint64_t signature,
        int expectedX, int expectedY, std::vector<float>& heights);
    static bool ReadCacheHeightsRange(const std::string& path,
        uint64_t signature, int expectedX, int expectedY,
        std::vector<float>& heights, uint64_t offset, uint64_t size);
    const std::vector<std::shared_ptr<Material>>& GetLayerMaterials() const { return m_materials; }
    JsonValue Serialize() const override;
    void Deserialize(const JsonValue& value) override;
    void OnAfterDeserialize(IGraphicsProvider* provider) override;
    void Start() override;
    bool DrawProperties(::Engine::Editor::IEditorUi& ui) override;
private:
    IGraphicsProvider* m_provider = nullptr;
    int m_meshLodStride = 1;
    std::vector<std::shared_ptr<Material>> m_materials;
    std::vector<std::string> m_materialPaths;
    std::vector<BakedAppearance> m_bakedAppearance;
    std::array<BakedLod, 4> m_bakedLods;
    uint8_t m_bakedTextureMask = 0;
    std::string m_bakeStatus;
    void RefreshMaterials();
    void NormalizeWeights();
    bool InstallBakedLod(int stride);
};
}

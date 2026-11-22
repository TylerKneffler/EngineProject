#pragma once
#include "Core/Component.h"
#include "Core/Components/Terrain/TerrainStamp.h"
#include <functional>
#include "Core/Components/Mesh/Mesh.h"
#include <glm/glm.hpp>
#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include <array>
#include <iosfwd>
#include <map>
#include <optional>

namespace Engine::Graphics { class IGraphicsTexture; }

namespace Engine::Components
{
class Material;
struct TerrainRegionMask;
struct TerrainRegionOperation;
struct TerrainHeightGrid;
class Terrain : public Engine::Core::Component
{
public:
    bool RequiresUpdate() const override { return false; }
    struct Layer
    {
        std::string material;
        float minHeight = 0.f;
        float maxHeight = 10.f;
        glm::vec3 previewColor{.35f, .55f, .25f};
    };
    // Blend smooths toward the local average; Flatten pulls toward
    // BrushOptions::flattenHeight (terrain-local Y).
    enum class Brush { Raise, Lower, Blend, Paint, Flatten };
    struct BrushOptions
    {
        // Fraction of the radius at full strength before a smoothstep
        // falloff to the edge. Negative keeps the legacy linear cone.
        float hardness = -1.f;
        float flattenHeight = 0.f;
        // Paint only: remove the layer, handing its weight to the others.
        bool erase = false;
    };
    // Brush weight at a distance from the brush center (0 outside).
    static float BrushFalloff(float distance, float radius, float hardness);
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
        int paintResolution = 0;
        std::vector<float> paintWeights;
        std::vector<Layer> layers;
        std::vector<Mesh::Vertex> vertices;
        std::vector<uint32_t> indices;
        std::optional<Mesh::IndexedGeometryBounds> bounds;
        std::vector<std::shared_ptr<Material>> materials;
        std::vector<std::string> materialPaths;
        std::vector<BakedAppearance> bakedAppearance;
        std::array<BakedLod, 4> bakedLods;
        uint8_t bakedTextureMask = 0;
        std::vector<TerrainStampInstance> stamps;
        std::vector<std::string> stampMaterials;
        size_t collisionIndexCount = 0;
        std::vector<uint32_t> occluderIndices;
        bool compactMesh = false;
        // The mesh in the compact terrain format (compactMesh), instead of
        // vertices/indices.
        Mesh::TerrainMeshData compact;
    };
    Terrain();
    int cellsX = 32, cellsY = 32;
    float cellWidth = 1.f, cellDepth = 1.f;
    float textureTiling = 4.f;
    std::vector<float> heights;
    std::vector<Layer> layers;
    std::vector<float> weights; // one weight per vertex and layer
    // Placed terrain stamps (TerrainStamp): drawn as part of this cell's mesh,
    // after the height grid's vertices and triangles, at the mesh's level
    // stride. Not serialized: the terrain's owner sets them before the mesh is
    // built or loaded (TerrainManager::SetTerrainStamps).
    std::vector<TerrainStampInstance> stamps;
    // Stamps' own surface materials (tools/stamp_textures.py), packed into the
    // layer arrays after the painted layers; TerrainStampInstance::materialIndex
    // picks one. Not serialized.
    std::vector<std::string> stampMaterials;
    // Keep this cell's mesh in the compact terrain vertex format (48 bytes a
    // vertex instead of 264, on the CPU and the GPU). Those vertices carry no
    // layer weights, so the cell then always has a paint map (at least two
    // samples per quad, seeded from the vertex weights). Not serialized: the
    // owner sets it before the mesh is built or loaded
    // (TerrainManager::compactCellVertices).
    bool compactMesh = false;
    // Paint detail, in weight samples per cell edge. 0 keeps paint on the
    // vertices. Otherwise paintWeights holds one weight per layer per sample
    // on a grid aligned with the vertices, PaintWidth() x PaintHeight(), and
    // the vertex weights are resynced from it (LODs, far tiles, vertex color).
    // Serialized as 8-bit weights, deflated and base64 encoded.
    int paintResolution = 0;
    std::vector<float> paintWeights;
    // Parallax relief within 15 m of the camera, from each layer material's
    // height map and heightScale (in that layer's texture repeats).
    bool reliefMapping = true;
    // Relief self-shadowing from directional lights within 12 m. Costs about
    // as much again as the relief itself, so it is opt-in.
    bool reliefShadows = false;
    // Transient: ground cover (e.g. grass) growing on some paint layers
    // publishes its colours here, so the ground under and beyond it is tinted
    // to match. Within fadeEnd of the camera gaps between plants take the
    // darker root colour; beyond it the cover's own colour carries on.
    struct GroundCover
    {
        glm::vec3 color{0.f};      // mean linear colour of the cover
        float strength = 0.f;      // 0 disables the tint
        float rootShade = .55f;    // root colour = color * rootShade
        float fadeStart = 0.f;     // world distance where the cover fades out
        float fadeEnd = 0.f;
        uint32_t layerMask = 0;    // paint layers the cover grows on
        // The field that published this cover, and when (steady clock,
        // seconds). Several grass fields can sit on one terrain (meadow, far
        // ring, forest floor); only the owner changes or clears it, and a
        // claim that is no longer renewed expires. Never dereferenced.
        const void* owner = nullptr;
        double publishedAt = 0.0;
    };
    GroundCover groundCover;
    static constexpr int MaxPaintSamples = 2049; // per side
    int PaintWidth() const { return paintResolution > 0 ? cellsX * paintResolution + 1 : 0; }
    int PaintHeight() const { return paintResolution > 0 ? cellsY * paintResolution + 1 : 0; }
    bool HasPaintMap() const;
    // Resamples the current paint (map or vertex weights) to a new
    // resolution; 0 drops the map and keeps its vertex-sampled weights.
    void SetPaintResolution(int resolution);
    // GPU copy of the paint map: a two-slice RGBA8 array (layers 0-3, 4-5)
    // with mips, re-uploaded after edits. Null without a paint map.
    const Engine::Graphics::IGraphicsTexture* GetPaintTexture(IGraphicsProvider* provider);
    // Call after writing paintWeights directly (seam stitching), then Rebuild.
    void NotifyPaintChanged() { m_paintTextureDirty = true; }
    std::string bakedAssetPath;
    void Resize(int x, int y, float width, float depth);
    void AssignLayersByHeight();
    // Sculpting carries resting placements up/down with the surface. With
    // realignObjects, placements that were aligned to the old surface also
    // tilt to the new one; otherwise orientation is left as painted.
    bool ApplyBrush(float x, float z, float radius, float strength,
        Brush brush, int layer = 0, bool realignObjects = false,
        const BrushOptions& options = {});
    // Region editing (TerrainRegionEdit.h). Masks are in this cell's local
    // X/Z. EditRegion runs a whole-cell operation (erosion, smoothing...);
    // PaintRegion paints a layer by mask weight; offsets place the cell in
    // the mask's space (a manager passes the cell's origin).
    bool EditRegion(const TerrainRegionMask& mask, const TerrainRegionOperation& operation,
        bool realignObjects = false);
    bool PaintRegion(const TerrainRegionMask& mask, float strength, int layer, bool erase,
        double offsetX = 0.0, double offsetZ = 0.0);
    // Copies the grid's valid samples that land on this cell's vertices (the
    // grid's frame places this cell's origin at offset), rebuilds, and moves
    // resting placements with the surface. False when nothing changed.
    bool WriteHeightGrid(const TerrainHeightGrid& grid, double offsetX, double offsetZ,
        bool realignObjects);
    // Placements over the box follow a height change from before (see
    // ApplyBrush): resting ones keep their offset, grass snaps.
    void FollowSurfaceEdit(const std::vector<float>& before, float minX, float minZ,
        float maxX, float maxZ, bool realignObjects);
    bool ApplyBrushFromLocalRay(const glm::vec3& nearPoint,
        const glm::vec3& farPoint, float radius, float strength,
        Brush brush, int layer = 0);
    // Intersects a terrain-local ray with the height field (the same two
    // triangles per cell that Rebuild meshes). distance is in units of
    // direction, so an affine-transformed world ray keeps its parameter.
    bool RaycastLocal(const glm::vec3& origin, const glm::vec3& direction,
        float& distance) const;
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
        std::vector<float>& heights, uint64_t offset, uint64_t size,
        std::vector<float>* weights = nullptr,
        uint32_t* weightLayerCount = nullptr);
    static bool CacheMaterialsMatchRange(std::istream& stream,
        uint64_t signature, int expectedX, int expectedY,
        uint64_t offset, uint64_t size,
        std::map<std::string, int64_t>& materialStamps);
    const std::vector<std::shared_ptr<Material>>& GetLayerMaterials() const { return m_materials; }
    // The materials the layer arrays hold: the painted layers (at most six),
    // then stampMaterials.
    const std::vector<std::shared_ptr<Material>>& GetArrayMaterials() const
    { return m_arrayMaterials.empty() ? m_materials : m_arrayMaterials; }
    JsonValue Serialize() const override;
    void Deserialize(const JsonValue& value) override;
    void OnAfterDeserialize(IGraphicsProvider* provider) override;
    void PrepareGraphicsResources(IGraphicsProvider* provider) override;
    void Start() override;
    bool DrawProperties(Engine::Core::IInspectorUi& ui) override;
private:
    IGraphicsProvider* m_provider = nullptr;
    std::shared_ptr<Engine::Graphics::IGraphicsTexture> m_paintTexture;
    const IGraphicsProvider* m_paintTextureProvider = nullptr;
    bool m_paintTextureDirty = true;
    // Rebuilds the map from vertex weights when its size no longer matches.
    void EnsurePaintMap();
    void SyncVertexWeightsFromPaint();
    // Brush strokes: rebuild only vertex rows minRow..maxRow (plus one row
    // of normals either side) in place. Falls back to Rebuild when the mesh
    // is not a full-resolution copy of the grid (LOD stride, new materials).
    void RebuildRegion(int minCol, int minRow, int maxCol, int maxRow);
    void BuildVertex(int col, int row, Mesh::Vertex& vertex) const;
    // Installs a built cell mesh (grid, then stamp patches from gridIndices
    // on) in the compact format when compactMesh, else as is; triangle-mesh
    // collision covers the grid only.
    bool InstallCellMesh(Mesh& mesh, std::vector<Mesh::Vertex>&& vertices,
        std::vector<uint32_t>&& indices, size_t gridIndices);
    // compactMesh: a paint map from the vertex weights where there is none.
    void PrepareCompactPaint();
    // The full-resolution grid's vertices (the first heights.size() of the
    // mesh), from either vertex format. Empty when the mesh is not one.
    std::vector<Mesh::Vertex> GridVertices(const Mesh& mesh) const;
    // Appends the stamps' patches at `stride` (1 = every stamp point). Each
    // point blends from the ground under it (heightsSource, this cell's grid)
    // to its place on the stamp by its weight; `paint` fills a vertex's colour
    // and layer weights from a grid sample index.
    // A sculpted cell (TerrainStampInstance::sculpt) instead drops the grid's
    // triangles (its vertices stay first, for GridVertices) and adds the
    // sculpt's level for the stride.
    void AppendStampGeometry(std::vector<Mesh::Vertex>& vertices,
        std::vector<uint32_t>& indices, int stride,
        const std::vector<float>& heightsSource,
        const std::function<void(size_t, Mesh::Vertex&)>& paint) const;
    // The cell's sculpt, if its surface is one.
    const TerrainSculpt* Sculpt() const;
    // A sculpt's coarsest level over the vertices AppendStampGeometry last
    // built, for the mesh's occluder (InstallCellMesh takes it).
    mutable std::vector<uint32_t> m_sculptOccluder;
    // Base color map present per layer, refreshed by Rebuild.
    std::array<bool, 6> m_layerTextured{};
    int m_meshLodStride = 1;
    std::vector<std::shared_ptr<Material>> m_materials;
    std::vector<std::shared_ptr<Material>> m_arrayMaterials;
    std::vector<std::string> m_arrayMaterialPaths;
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


#pragma once
#include "Core/Component.h"
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
namespace Engine::Brushes { struct BrushRules; struct BrushLayerSet; }

namespace Engine::Components
{
class Material;
struct TerrainBrushShape;
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
        // Fraction of the radius at full strength before the falloff to the
        // edge. Negative keeps the legacy linear cone.
        float hardness = -1.f;
        // The round brush's falloff past `hardness` (a brush's: 0 hard,
        // 1 linear, 2 smooth; TerrainBrushShape::Falloff).
        int falloff = 2;
        // Where the stroke acts (a brush's terrain rules: height and slope
        // bands, paint layers it needs or avoids), judged at each point
        // before the dab; null acts everywhere. Not owned.
        const Engine::Brushes::BrushRules* rules = nullptr;
        // Metres to the nearest trail's centre line at a point of this
        // terrain (its local X, Z and height), for rules with a trail
        // clearance; unset: no trails. TerrainManager sets it per cell.
        std::function<float(float x, float z, float height)> trailDistance;
        float flattenHeight = 0.f;
        // Paint only: remove the layer, handing its weight to the others.
        bool erase = false;
        // A shaped brush (a stamp, TerrainBrushShape): its mask replaces the
        // round falloff for every brush, and Raise and Lower also scale by its
        // height map, so a stroke raises the stamp's high places fastest and
        // builds up its shape. The shape's square spans 2 x radius, turned by
        // shapeYaw (radians about +Y). Null keeps the round brush.
        std::shared_ptr<const TerrainBrushShape> shape;
        float shapeYaw = 0.f;
        // Shaped Raise only: the stamp builds only against terrain behind it
        // (back from each point, opposite the shape's facing, across its
        // footprint): a point rises at most to the highest ground found
        // there. Where the stamp hangs past a wall's top or end, or lies on
        // open ground, nothing changes.
        bool backed = false;
        // The ground heights `backed` reads, in the terrain's local X/Z;
        // false where unknown. Unset: this cell's heights before the dab.
        // TerrainManager sets one over every loaded cell, so strokes across
        // cell borders see the same ground.
        std::function<bool(float x, float z, float& height)> backingHeight;
        // Shaped Raise only: stop at the stamp's surface, stampBase plus the
        // shape's height there (scaled with the radius, so the stamp keeps its
        // proportions, times stampHeightScale). Strength is then only how fast
        // a stroke grows into the stamp; holding or overlapping strokes cannot
        // pile up past it.
        bool stopAtStamp = false;
        float stampBase = 0.f;   // terrain-local Y the stamp stands on
        float stampHeightScale = 1.f;
    };
    // Brush weight at a distance from the brush center (0 outside).
    static float BrushFalloff(float distance, float radius, float hardness, int falloff = 2);
    // How much `options.rules` let a stroke act at a vertex (1 without
    // rules): its height, slope from its neighbours, and paint, with the
    // rules' layer materials resolved for this terrain (ResolveRuleLayers).
    float RulesWeightAt(int col, int row, const std::vector<float>& heightGrid,
        const BrushOptions& options, const Engine::Brushes::BrushLayerSet& layerSet) const;
    // A brush's rule layers (material names) as this terrain's layer indices.
    Engine::Brushes::BrushLayerSet ResolveRuleLayers(const Engine::Brushes::BrushRules& rules) const;
    // Brush weight at an offset from the brush centre, round or shaped. For a
    // shaped brush, `relief` (when given) receives the shape's height there as
    // a share of its tallest point (0..1); 1 for a round brush. How far a
    // brush reaches from its centre: the radius, or the shape square's corner.
    static float BrushWeight(float dx, float dz, float radius, const BrushOptions& options,
        float* relief = nullptr);
    static float BrushReach(float radius, const BrushOptions& options);
    // A cell built off the main thread (TerrainManager streaming), handed
    // to the cell's component.
    struct Prepared
    {
        int cellsX = 0, cellsY = 0;
        float cellWidth = 1.f, cellDepth = 1.f, textureTiling = 4.f;
        std::vector<float> heights, weights, lushness;
        int paintResolution = 0;
        std::vector<float> paintWeights;
        std::vector<Layer> layers;
        std::vector<std::shared_ptr<Material>> materials;
        std::vector<std::string> materialPaths;
    };
    // The static cell cache's format (SaveCache).
    // 9 (2026-11-25): adds the erosion (flow, deposition).
    static constexpr uint32_t CacheVersion = 10;   // 10: no erosion arrays (2026-11-25)
    Terrain();
    int cellsX = 32, cellsY = 32;
    float cellWidth = 1.f, cellDepth = 1.f;
    float textureTiling = 4.f;
    std::vector<float> heights;
    std::vector<Layer> layers;
    std::vector<float> weights; // one weight per vertex and layer
    // Optional, one per vertex: how lush (darker, greener) the ground grows
    // there (the paint archive: wet valley floors under grass and forest, the
    // same tint the macro color map carries for distant ground).
    // Vertex uvChannels1.w; Object.hlsl tints the terrain by it. Kept in the
    // static cell cache; not serialized with the scene.
    std::vector<float> lushness;
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
    // rules (optional): a brush's terrain rules, judged at each point.
    bool PaintRegion(const TerrainRegionMask& mask, float strength, int layer, bool erase,
        double offsetX = 0.0, double offsetZ = 0.0,
        const Engine::Brushes::BrushRules* rules = nullptr,
        std::function<float(float x, float z, float height)> trailDistance = {});
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
    // After changing heights, weights or layers: settles weights, paint and
    // materials and bumps the surface revision, which the terrain's volume
    // (VolumeTerrain) follows. There is no height-grid mesh.
    void Rebuild();
    uint64_t GetSurfaceRevision() const { return m_surfaceRevision; }
    // A terrain that is not a Terrain Manager's cell gets a VolumeTerrain
    // (drawn and collided from its heights) if it has none.
    void EnsureVolume();
    Prepared TakePrepared();
    bool InstallPrepared(Prepared prepared, IGraphicsProvider* provider);
    void FinalizePreparedGraphics(IGraphicsProvider* provider);
    // Static generated cells restore their heights, paint and lushness
    // without sampling noise (CacheVersion).
    bool SaveCache(const std::string& path, uint64_t signature) const;
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
    // The materials the layer arrays hold: the painted layers (at most six).
    const std::vector<std::shared_ptr<Material>>& GetArrayMaterials() const { return m_materials; }
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
    uint64_t m_surfaceRevision = 1;
    std::vector<std::shared_ptr<Material>> m_materials;
    std::vector<std::string> m_materialPaths;
    std::string m_bakeStatus;
    void RefreshMaterials();
    void NormalizeWeights();
};
}


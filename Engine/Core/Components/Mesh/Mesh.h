#pragma once
#include "Core/Components/Component.h"
#include "Core/Components/PropertyMacros.h"
#include "Core/Graphics/IGraphicsBuffer.h"
#include "Core/Model/MeshData.h"
#include "Core/Rendering/MeshLodSelection.h"
#include <cstdint>
#include <algorithm>
#include <string>
#include <vector>
#include <memory>
#include <optional>
#include <initializer_list>
#include <atomic>

namespace Engine::Rendering { struct PreparedMeshBuffer; struct GpuBladeField; }
namespace Engine::Components
{
class Mesh : public Engine::Core::Component
{
public:
    using Vertex = Engine::Model::AnimationVertex;
    using TerrainVertex = Engine::Model::TerrainVertex;
    using TerrainMeshData = Engine::Model::TerrainMeshData;
    using MorphTarget = Engine::Model::MorphTarget;
    using JsonValue = Engine::Serialization::JsonValue;
    using IGraphicsBuffer = Engine::Graphics::IGraphicsBuffer;
    using IGraphicsBufferFactory = Engine::Graphics::IGraphicsBufferFactory;
    using IGraphicsProvider = Engine::Graphics::IGraphicsProvider;

    Mesh();
    bool RequiresUpdate() const override { return false; }
    ~Mesh() = default;

    // Use interpolated baked probes instead of a surface lightmap. Enable for
    // movable meshes and characters that travel through a probe volume.
    PROPERTY(Inspector,EditAnywhere,Category="Lighting")
    bool useLightProbes=false;
    // Procedural rigid meshes can opt into cascade AABB culling. Skinned and
    // shader-deformed meshes need conservative bounds before enabling this.
    bool useBoundsForShadowCulling = false;
    // Optional local-space padding for vertex or procedural deformation.
    // The wind-derived envelope remains as a conservative fallback.
    PROPERTY(Inspector,EditAnywhere,Category="Rendering",ClampMin="0")
    float deformationBoundsPadding = 0.f;
    // Explicit opt-in to the project's grass caster distance budget.
    bool usesGrassShadowBudget = false;
    // Transient provenance for transformed shared prefab geometry, whose source
    // buffer intentionally has no file path. Does not affect rendering policy.
    bool treeTimingGeometry = false;
    // Authored foliage shares the grass wind field, with restrained tree motion.
    PROPERTY(Inspector,EditAnywhere,Category="Foliage",ClampMin="0",ClampMax="1")
    float foliageWindStrength = 0.f;
    // Tree wind instead of the grass bend (.md/todo.md Foliage Phase 5): a slow,
    // gust-driven trunk bend growing with height squared, branch sway growing
    // with distance from the trunk, and leaf flutter on cooked leaf meshes.
    // Strength 0.1 bends a 20 m tree ~0.4 m at the crown in a gust.
    PROPERTY(Inspector,EditAnywhere,Category="Foliage")
    bool foliageWindTree = false;
    // Transient per-object caster range; zero means unrestricted. Evaluated on
    // the GPU so distance changes do not toggle whole instance tiles.
    float runtimeShadowDistance = 0.f;
    float runtimeShadowFadeWidth = 1.f;
    // Transient per-placement distance fade for instanced ground cover
    // (GrassField): each placement shrinks toward its root between these
    // world distances from the camera. End <= start disables it.
    // Instanced meshes with GPU LOD selection (ground cover tiles): each
    // placement takes its detail level from its own distance to the camera
    // instead of the whole tile switching together at the distance to the
    // tile's origin. Camera views only; needs full runtime visibility.
    bool perPlacementLod = false;
    // Triangle-mesh colliders use only the first collisionIndexCount indices
    // (0 = all). Terrain cells set it to their height grid's triangles, so the
    // stamp patches that follow them are drawn but not collided with.
    size_t collisionIndexCount = 0;
    // CPU-only: the triangles the software occlusion buffer rasterizes in
    // place of the drawn ones, when set (a sculpted terrain cell's coarsest
    // level; occluders need its silhouette, not its detail).
    std::vector<uint32_t> occluderIndices;
    // With occluderIndices: they use only the first occluderVertexCount
    // vertices (0 = all), so the buffer transforms no more than those.
    size_t occluderVertexCount = 0;
    float placementFadeStart = 0.f;
    float placementFadeEnd = 0.f;
    // Transient distance thinning for instanced ground cover (GrassField):
    // between these world distances the share of placements kept falls from
    // 1 to placementThinMinimum (1 disables), and survivors widen by
    // 1 / sqrt(share) so the cover stays as dense on screen.
    // Near/far cell handover role (Rendering/CellHandover.h): 1 a streamed
    // cell's own objects, 2 the far impostor blocks; 0 none. Runtime only.
    int handoverRole = 0;
    // The instanced LOD band distances (instanceLodDistances) are world
    // metres for every placement: not scaled by the layer or view LOD scale,
    // the placement's own scale, or the shadow pass. Far tree blocks and
    // their forest clusters split at TerrainManager::farClusterDistance with
    // it, so a tree and the cluster that stands for it agree on the boundary.
    bool instanceLodPlainDistance = false;
    float placementThinStart = 0.f;
    float placementThinEnd = 0.f;
    float placementThinMinimum = 1.f;
    // Thinning used as a fade-in (start > end, minimum 0): survivors keep
    // their width instead of widening.
    bool placementThinKeepWidth = false;
    // Detail levels carried by the prefab that owns this part: entry i is the
    // mesh asset used at LOD i + 1. An empty entry, or a level past the end of
    // a non-empty list, hides the part at that level (e.g. bark under an
    // impostor card). Placement systems address a level of the prefab as
    // "X.prefab#lodN"; see Core/Serialization/PrefabLod.h.
    //
    // An ordinary scene object switches levels itself: the mesh keeps LOD 1
    // and 2 of this list resident (see PrepareAuthoredLods), the sibling
    // Material supplies Material.lodMaterials for each level, and
    // gpuLod1Distance / gpuLod2Distance are the switch distances. One object,
    // one Mesh, one Material; no per-level objects or prefabs.
    std::vector<std::string> lodMeshes;
    PROPERTY(Inspector,EditAnywhere,Category="LOD")
    bool gpuLodSelection = false;
    // Opt in to pixel thresholds; distance override restores authored policy.
    bool screenLodSelection = false;
    bool distanceLodOverride = false;
    float screenLod1Pixels = 300.f;
    float screenLod2Pixels = 80.f;
    float screenLodHysteresis = .1f;
    // Fraction of each distance LOD interval used for dither overlap.
    float lodTransitionWidth = .25f;
    // Runtime policy from an ObjectLodProvider. Zero distance disables range culling.
    bool cullAfterLastLod = false;
    float finalLodCullDistance = 0.f;
    // Independent resident instance LOD bands may use different materials.
    // Transient; bands share placement records and are selected on the GPU.
    int instanceLodBand = -1;
    int fixedShadowLodBand = -1; // transient fixed shadow band; -1 follows distance
    // Transient far caster (TerrainObjectLayer::farShadowDistance): this band
    // casts even when another band is the fixed shadow band, for placements
    // beyond farShadowMinDistance (its shadow fades in where the fixed band's
    // fades out) and out to runtimeShadowDistance.
    bool farShadowCaster = false;
    float farShadowMinDistance = 0.f;
    bool dedicatedShadowProxy = false;
    bool shadowProxyOnly = false;
    glm::vec2 instanceLodDistances{0.f};
    PROPERTY(Inspector,EditAnywhere,Category="LOD")
    float gpuLod1Distance = 8.f;
    PROPERTY(Inspector,EditAnywhere,Category="LOD")
    float gpuLod2Distance = 16.f;

    // Load vertex data from a triangulated OBJ file (CPU side only).
    void LoadFromFile(const std::string& path);
    // Resolves portable Assets/... paths against bundled sandbox assets.
    static std::string ResolveFilePath(const std::string& path);
    static bool SaveNativeFile(const std::string& path, const std::vector<Vertex>& vertices);
    static bool SaveNativeFile(const std::string& path,
        const std::vector<Vertex>& vertices, const std::vector<uint32_t>& indices);
    static bool ValidateAuthoredGeometry(const std::vector<Vertex>& vertices,
        const std::vector<uint32_t>& indices);

    // Create a graphics buffer from loaded vertex data.
    // bufferFactory: creates GPU vertex buffers (API-agnostic)
    void CreateBuffer(IGraphicsBufferFactory* bufferFactory,
        Engine::Rendering::PreparedMeshBuffer* prepared = nullptr,
        Engine::Rendering::PreparedMeshBuffer* preparedIndices = nullptr);
    // Unique geometry (terrain chunks): buffers made by the caller, not
    // shared through AcquireMeshBuffer, whose one lock is held while each
    // buffer is created (a worker making a large one stalled the main
    // thread's small one by up to 25 ms).
    void InstallOwnBuffers(std::shared_ptr<IGraphicsBuffer> vertexBuffer,
        std::shared_ptr<IGraphicsBuffer> indexBuffer, IGraphicsBufferFactory* bufferFactory);
    // The same for compact terrain vertices (SetTerrainGeometry with
    // deferBuffers): the caller's buffers, not the content-cached ones.
    void InstallOwnTerrainBuffers(std::shared_ptr<IGraphicsBuffer> vertexBuffer,
        std::shared_ptr<IGraphicsBuffer> indexBuffer, IGraphicsBufferFactory* bufferFactory);
    void InstallPreparedStaticVertexBuffer(
        std::shared_ptr<IGraphicsBuffer> buffer, IGraphicsBufferFactory* bufferFactory,
        Engine::Rendering::PreparedMeshBuffer* preparedIndices = nullptr);

    // Get the underlying graphics buffer (API-agnostic)
    IGraphicsBuffer* GetGraphicsBuffer() const { return m_instanceSource ? m_instanceSource->GetGraphicsBuffer() : m_vertexBuffer.get(); }
    IGraphicsBuffer* GetIndexBuffer() const { return m_instanceSource ? m_instanceSource->GetIndexBuffer() : m_indexBuffer.get(); }
    std::shared_ptr<IGraphicsBuffer> GetVertexBufferOwner() const
    { return m_instanceSource ? m_instanceSource->GetVertexBufferOwner() : m_vertexBuffer; }
    std::shared_ptr<IGraphicsBuffer> GetIndexBufferOwner() const
    { return m_instanceSource ? m_instanceSource->GetIndexBufferOwner() : m_indexBuffer; }

    // Transient rigid instances share immutable geometry and placement buffers
    // across tiles/LODs. Rows describe a uniform-scale affine transform.
    struct InstanceTransform { glm::vec4 rows[3]; };
    struct InstanceData
    {
        inline static std::atomic<uint64_t> nextLodIdentity{1};
        const uint64_t lodIdentity=nextLodIdentity.fetch_add(1,std::memory_order_relaxed);
        uint64_t revision=1;
        uint64_t slicesRevision=1;
        uint64_t stableId=0;
        // Diagnostics (ENGINE_TRACE_LAYER): which terrain source made a shared
        // prefab block: 1 cell layer, 2 near block, 3 far block, 0 other.
        uint8_t traceSource=0;
        bool hasLodBounds=false;
        glm::vec3 lodMinimum{0},lodMaximum{0};
        // Block culling (Mesh::SetInstanceBlocks fills them once): the
        // placements' coverage scale range (InstanceCull.hlsl, length of row
        // 0, clamped to .25-4) and their largest matrix Frobenius norm, which
        // bounds each placement's radius as |local extent| * norm.
        bool hasCullStats=false;
        float coverageMinimum=1.f,coverageMaximum=1.f,maximumMatrixNorm=1.f;
        int maximumLod=0;
        std::vector<uint64_t> placementIds;
        void MarkDirty() { ++revision; }
        struct Slice
        {
            uint32_t firstInstance = 0, instanceCount = 0;
            std::unique_ptr<IGraphicsBuffer> buffer;
        };
        std::vector<InstanceTransform> transforms;
        std::vector<Slice> slices;
        uint64_t editorSelectionPlacementId = 0;
        uint64_t editorSelectionRevision = 0;
        std::unique_ptr<IGraphicsBuffer> editorSelectionBuffer;
        // Renderer scratch: the placement pool base found in the current
        // batching pass (shared part blocks are visited once per part).
        mutable uint64_t poolPass = 0;
        mutable uint32_t poolBase = 0;
        // Rendering::PersistentInstancePool's registration handle (opaque).
        mutable std::shared_ptr<void> poolHandle;
    };
    struct InstanceRange
    {
        // Draw-element offset/count: vertices for nonindexed sources, indices
        // for indexed sources. Names retain existing aggregate/source compatibility.
        uint32_t firstVertex = 0, vertexCount = 0;
        uint32_t firstInstance = 0, instanceCount = 0;
        // Ground cover: the share of the mesh's placement fade and thinning
        // distances this range's plants use (1 = all of it). Lets fine grass
        // end sooner than the shrubs in the same tile.
        float reachScale = 1.f;
        // Distance thinning (placementThin) applies to this range's plants.
        // Off for shrubs and bushes: a bush thinned away at mid range grew
        // out of the ground as the camera came toward it.
        bool thinned = true;
    };
    void SetInstancedGeometry(std::shared_ptr<Mesh> source,
        std::shared_ptr<InstanceData> instances, std::vector<InstanceRange> ranges,
        const glm::vec3& minimum, const glm::vec3& maximum);
    bool IsInstanced() const { return m_instanceSource != nullptr; }
    const Mesh* GetInstanceSource() const { return m_instanceSource.get(); }
    IGraphicsBuffer* GetInstanceBuffer(uint32_t firstInstance = 0) const
    {
        if (m_instances) for (const auto& slice : m_instances->slices)
            if (slice.firstInstance == firstInstance) return slice.buffer.get();
        return nullptr;
    }
    const std::vector<InstanceRange>& GetInstanceRanges() const { return m_instanceRanges; }
    const std::shared_ptr<InstanceData>& GetInstanceData() const { return m_instances; }

    // Shared instanced parts (one object for a prefab part across all
    // streamed cells): placements come in blocks, each its own record range
    // in the renderer's placement pool, in world space (the object keeps an
    // identity transform). minimum/maximum: the block's world bounds with
    // geometry, for culling whole blocks. GetInstanceData() is then a stub
    // that holds no placements: its revision changes with the block list and
    // maximumLod is the part's. The ranges select source geometry only.
    struct InstanceBlock
    {
        std::shared_ptr<InstanceData> data;
        glm::vec3 minimum{0.f}, maximum{0.f};
    };
    void SetInstanceBlocks(std::shared_ptr<Mesh> source, std::vector<InstanceBlock> blocks,
        std::vector<InstanceRange> ranges, int maximumLod);
    void SetInstanceBlocks(std::vector<InstanceBlock> blocks);
    bool UsesInstanceBlocks() const { return m_usesInstanceBlocks; }
    // Transient: this instanced mesh is a GPU grass blade carrier (its visible
    // list comes from the blade generation pass, Rendering/GpuGrassBlades.h).
    std::shared_ptr<Engine::Rendering::GpuBladeField> gpuBladeField;
    const std::vector<InstanceBlock>& GetInstanceBlocks() const { return m_instanceBlocks; }
    
    uint32_t GetVertexCount() const
    {
        if (m_instanceSource) return m_instanceSource->GetVertexCount();
        return static_cast<uint32_t>(m_terrainVertices.empty()
            ? m_vertices.size() : m_terrainVertices.size());
    }
    uint32_t GetIndexCount() const { return m_instanceSource ? m_instanceSource->GetIndexCount() : static_cast<uint32_t>(m_indices.size()); }
    const std::vector<Vertex>& GetVertices() const { return m_vertices; }
    const std::vector<TerrainVertex>& GetTerrainVertices() const
    {
        return m_terrainVertices;
    }
    const std::vector<uint32_t>& GetIndices() const { return m_indices; }
    uint64_t GetAuthoredGeometryRevision() const { return m_authoredGeometryRevision; }
    // Changes whenever CPU positions change without an authored revision:
    // terrain geometry replaced or taken, or deformed vertices written.
    // Drawn from one process-wide counter, so no two such changes (even in
    // different meshes) share a value.
    uint64_t GetCpuGeometryRevision() const { return m_cpuGeometryRevision; }
    float GetTextureUvWorldScale(uint32_t uvSet = 0) const;
    struct PoseSnapshot
    {
        std::vector<Vertex> vertices;
        std::vector<uint32_t> indices;
        std::vector<MorphTarget> morphTargets;
        std::vector<float> morphWeights;
        std::vector<glm::vec3> surfaceOffsets;
    };
    PoseSnapshot CapturePoseSnapshot() const;
    static std::vector<Vertex> BuildPosedVertices(
        const PoseSnapshot& snapshot, const std::vector<glm::mat4>* skinPalette,
        bool includeSurfaceOffsets = true);
    std::vector<Vertex> BuildPosedVertices(
        const std::vector<glm::mat4>* skinPalette = nullptr,
        bool includeSurfaceOffsets = true) const;
    std::vector<Vertex> BuildPortalCutTriangleStream(
        const std::vector<glm::mat4>* skinPalette = nullptr) const;
    // Posed-space displacement is transient and never enters the authored stream.
    void SetSurfaceOffsets(std::vector<glm::vec3> offsets,
        const std::vector<glm::mat4>* skinPalette);
    void ClearSurfaceOffsets();
    IGraphicsBuffer* GetPosedVertexBuffer() const { return m_posedVertexBuffer.get(); }
    bool HasSurfaceOffsets() const { return !m_surfaceOffsets.empty(); }
    uint64_t GetSurfaceOffsetsRevision() const { return m_surfaceOffsetsRevision; }
    const std::vector<glm::vec3>& GetSurfaceOffsets() const { return m_surfaceOffsets; }
    bool UsesTerrainVertexFormat() const { return !m_terrainVertices.empty(); }
    static TerrainMeshData BuildIndexedTerrain(const std::vector<Vertex>& vertices);
    // staticBuffers: device-local buffers (built once, drawn for a long
    // time: distant terrain tiles) instead of CPU-visible upload buffers
    // that suit cells rewritten often.
    // deferBuffers: create no GPU buffers here (the caller installs its own,
    // InstallOwnTerrainBuffers); the mesh is not ready until then.
    bool SetTerrainGeometry(TerrainMeshData&& geometry,
        bool retainExpandedVertices = false, bool staticBuffers = false, bool deferBuffers = false);
    TerrainMeshData TakeTerrainGeometry();
    // Returns true only when CPU vertices, bounds, and the GPU buffer changed.
    bool SetDeformedVertices(const std::vector<Vertex>& vertices);
    // Transfers ownership for newly generated meshes to avoid a full vertex copy.
    bool SetDeformedVertices(std::vector<Vertex>&& vertices);
    // Installs authored triangle-list geometry independently of runtime
    // deformation state. This validates topology, updates bounds/revision,
    // and recreates the GPU buffer when this mesh is already resident.
    bool SetAuthoredVertices(std::vector<Vertex> vertices);
    // Procedural triangle streams whose generator has already checked topology
    // and accumulated bounds. Avoids a second full CPU scan at publication.
    bool SetPreparedTriangleGeometry(std::vector<Vertex> vertices,
        const glm::vec3& minimum, const glm::vec3& maximum);
    // Exchanges static geometry and resident GPU buffers without copying or upload.
    // Returns false for meshes with morphs or runtime deformation.
    bool ExchangeStaticGeometry(Mesh& other);
    // Runtime LODs belong to this object. Installing geometry transfers ownership;
    // selecting a resident level exchanges buffers without copies or uploads.
    // These caches are transient and do not change the mesh asset's file path.
    static constexpr int MaximumRuntimeLods = 32;
    bool StoreRuntimeLod(int lod, std::unique_ptr<Mesh> geometry);
    // Takes a stored level that is neither drawn nor fading out, so its owner
    // can release its buffers later (StoreRuntimeLod releases the level it
    // replaces at once). Null when there is none or it is in use.
    std::unique_ptr<Mesh> TakeRuntimeLod(int lod);
    bool IsRuntimeLodResident(int lod) const;
    Mesh* GetRuntimeLodMesh(int lod) const
    { return !IsRuntimeLodResident(lod) ? nullptr : lod == m_activeRuntimeLod ? const_cast<Mesh*>(this) : m_runtimeLods[lod].get(); }
    bool SelectRuntimeLod(int lod);
    int GetActiveRuntimeLod() const { return m_activeRuntimeLod; }
    bool BeginRuntimeLodTransition(int lod, float durationSeconds);
    bool AdvanceRuntimeLodTransition(float deltaSeconds);
    Mesh* GetPreviousRuntimeLodMesh() const;
    glm::vec4 GetRuntimeGeometryFade(bool previous = false) const;
    // fromEnd: the visible share is the end of the dither range ([1 - v, 1))
    // instead of its start ([0, v)): a mesh fading out under one fading in
    // with the same share splits the pixels exactly (screen threshold).
    void SetRuntimeVisibility(float visibility, bool fromEnd = false);
    float GetRuntimeVisibility() const { return m_runtimeVisibility; }
    uint64_t GetRuntimeFadeRevision() const { return m_runtimeFadeRevision; }
    int ChooseRuntimeLod(float distance, std::initializer_list<float> thresholds,
        float hysteresis = 0.f) const;
    int ChooseRuntimeLod(float distance, const std::vector<float>& thresholds,
        float hysteresis = 0.f) const;
    // Opt-in visible selection. An explicit distance override always wins.
    // View must describe the actual render target; keep separate state per view.
    int ChooseScreenRuntimeLod(const glm::mat4& world,
        const Engine::Rendering::MeshLodView& view,
        const std::vector<float>& pixelThresholds, float hysteresis = .1f,
        std::optional<int> distanceOverride = std::nullopt) const;
    size_t GetResidentRuntimeLodCount() const;
    uint64_t GetRuntimeLodGpuMemoryBytes() const;
    // Releases cached alternatives, retaining the currently displayed geometry.
    void ClearRuntimeLods();
    // Back to the state before any runtime LOD was stored: the active level's
    // geometry leaves this mesh too (ClearRuntimeLods keeps it), so the
    // component can be reused for other content. The levels move to
    // `receiver` when given, so the caller decides when their buffers are
    // released (a frame in flight may still draw them); otherwise they are
    // destroyed here. False when the active geometry could not be exchanged
    // out; nothing is changed then.
    bool ResetRuntimeLods(Mesh* receiver = nullptr);
    // Updates authored vertex data without changing topology or reallocating
    // the index buffer. Used for interactive Mesh Edit transforms.
    bool UpdateAuthoredVertices(std::vector<Vertex> vertices);
    // Updates a contiguous part of an indexed surface without rebuilding its
    // topology; upload buffers receive only the changed byte range.
    bool UpdateIndexedVertexRange(size_t first, const std::vector<Vertex>& vertices);
    // Installs an authored indexed surface without expanding shared vertices.
    // Portal cuts expand only their transient CPU snapshot.
    bool SetIndexedGeometry(std::vector<Vertex> vertices,
        std::vector<uint32_t> indices);
    struct IndexedGeometryBounds
    {
        glm::vec3 minimum{}, maximum{};
    };
    static std::optional<IndexedGeometryBounds> PrepareIndexedGeometry(
        const std::vector<Vertex>& vertices,
        const std::vector<uint32_t>& indices);
    bool SetPreparedIndexedGeometry(std::vector<Vertex> vertices,
        std::vector<uint32_t> indices, IndexedGeometryBounds bounds);
    struct IndexedGeometryTiming
    {
        double validationMs = 0.0;
        double boundsMs = 0.0;
        double bufferMs = 0.0;
    };
    const IndexedGeometryTiming& GetLastIndexedGeometryTiming() const
    { return m_lastIndexedGeometryTiming; }
    // Transfers CPU vertices out of a procedural mesh that is about to be destroyed.
    std::vector<Vertex> TakeVertices();
    // Copies the runtime rendering context needed by a procedural mesh cut.
    // This does not copy vertex data or authoring/morph state.
    void InitializeRuntimeCloneFrom(const Mesh& source);
    uint32_t GetVertexStride() const
    {
        return UsesTerrainVertexFormat() ? sizeof(TerrainVertex) : sizeof(Vertex);
    }
    uint64_t GetCpuMeshMemoryBytes() const;
    uint64_t GetUploadShadowMemoryBytes() const;
    uint64_t GetGpuBufferMemoryBytes() const;
    bool     IsReady()        const { return m_ready; }
    // Transient editor-stage visibility; never serialized into the asset.
    bool IsEditorVisible() const { return m_editorVisible; }
    void SetEditorVisible(bool visible) { m_editorVisible = visible; }
    bool IsAnimationVisible() const { return m_animationVisible; }
    void SetAnimationVisible(bool visible) { m_animationVisible = visible; }
    const std::string& GetFilePath() const { return m_filePath; }
    void SetAuthoredFilePath(std::string path);
    bool HasBounds() const { return m_hasBounds; }
    // Conservative local-space envelope for vertex deformation. Bounds
    // consumers use this value without depending on the deformation source.
    float GetDeformationBoundsPadding() const
    { return std::max(std::max(0.f,deformationBoundsPadding),
        1.5f * std::max(0.f,m_boundsMax.y) * std::clamp(foliageWindStrength,0.f,1.f)); }
    glm::vec3 GetBoundsMin() const { return m_boundsMin-glm::vec3(GetDeformationBoundsPadding()); }
    glm::vec3 GetBoundsMax() const { return m_boundsMax+glm::vec3(GetDeformationBoundsPadding()); }
    // One source in placement-local space, before any instance transform.
    // GetBoundsMin/Max on an instanced mesh describe the entire batch instead.
    // Runtime LOD variants can inherit the owning mesh's deformation settings.
    bool GetInstanceGeometryBounds(glm::vec3& minimum, glm::vec3& maximum,
        const Mesh* deformationPolicy = nullptr) const;
    // Conservative local bounds for every currently resident runtime LOD.
    // Returns false when any resident representation lacks bounds.
    bool GetRuntimeLodAggregateBounds(glm::vec3& minimum,
        glm::vec3& maximum) const;
    void SetRuntimeLodBoundsEnvelope(const glm::vec3& minimum,
        const glm::vec3& maximum);
    unsigned GetMorphNodeIndex() const { return m_morphNodeIndex; }
    void SetMorphData(unsigned nodeIndex, std::vector<MorphTarget> targets,
        std::vector<float> weights);
    static bool SaveMorphFile(const std::string& path,
        const std::vector<MorphTarget>& targets);
    bool LoadMorphFile(const std::string& path, unsigned nodeIndex,
        std::vector<float> weights);
    bool SetMorphWeights(const std::vector<float>& weights);
    // Detects edits made through the mutable inspector/accessor and publishes
    // the compact weights without rebuilding the CPU vertex stream.
    void SyncMorphWeights();
    const std::vector<MorphTarget>& GetMorphTargets() const { return m_morphTargets; }
    const std::vector<float>& GetMorphWeights() const { return m_morphWeights; }
    std::vector<float>& GetMorphWeights() { return m_morphWeights; }
    uint64_t GetMorphWeightsRevision() const;
    bool HasMorphTargets() const { return !m_morphTargets.empty(); }
    void SetPortalPoseSnapshot(bool value) { m_portalPoseSnapshot = value; }
    bool IsPortalPoseSnapshot() const { return m_portalPoseSnapshot; }
    IGraphicsBuffer* GetMorphDeltaBuffer() const
    { return m_morphDeltaBuffer.get(); }
    IGraphicsBuffer* GetMorphWeightBuffer() const
    { return m_morphWeightBuffer.get(); }
    uint32_t GetMorphTargetCount() const
    { return static_cast<uint32_t>(m_morphTargets.size()); }
    uint32_t GetActiveMorphTargetCount() const
    { return m_activeMorphTargetCount; }
    uint32_t GetMorphDeltaStrideFloat4() const
    { return m_morphDeltaStrideFloat4; }

    using SliceResult = std::pair<std::vector<Vertex>, std::vector<Vertex>>;
    // Returns closed positive/negative halves. For a closed intersected mesh,
    // the cut contour is welded and capped with triangulated planar faces.
    static SliceResult SliceByPlane(const std::vector<Vertex>& vertices,
        const glm::vec3& planePoint, const glm::vec3& planeNormal);

    bool        DrawProperties(Engine::Core::IInspectorUi& ui) override;
    JsonValue   Serialize() const override;
    void        Deserialize(const JsonValue& v) override;
    void        DeserializeLegacyMorphTargets(const JsonValue& value);
    void        OnAfterDeserialize(IGraphicsProvider* graphicsProvider) override;
    void PrepareGraphicsResources(IGraphicsProvider* graphicsProvider) override;
    // Installs lodMeshes as this object's own resident levels, one level per
    // preparation pass. Ordinary objects keep LOD 0..2 resident (the renderer's
    // per-object limit); an empty entry ends the chain. Skipped when a
    // MeshLodProfile drives this mesh, and for instanced, skinned or morphing
    // meshes. Placement systems build their own bands and never call this.
    void PrepareAuthoredLods(IGraphicsProvider* graphicsProvider);
    // CPU-only preprocessing for detached meshes on a loading worker.
    void PrepareDeferredGraphicsData();
    void DeferGraphicsBuffers() { m_bufferFactory = nullptr; m_ready = false; }

private:
    static std::vector<Vertex> BuildPosedVerticesFromData(
        const std::vector<Vertex>& vertices, const std::vector<uint32_t>& indices,
        const std::vector<MorphTarget>& morphTargets,
        const std::vector<float>& morphWeights,
        const std::vector<glm::vec3>& surfaceOffsets,
        const std::vector<glm::mat4>* skinPalette, bool includeSurfaceOffsets);
    bool UsesDeferredGraphicsRetirement() const;
    void ReplaceVertexBuffer(std::shared_ptr<IGraphicsBuffer> buffer);
    void ReplaceIndexBuffer(std::shared_ptr<IGraphicsBuffer> buffer);
    void ReplacePosedVertexBuffer(std::unique_ptr<IGraphicsBuffer> buffer);
    void ReplaceMorphDeltaBuffer(std::unique_ptr<IGraphicsBuffer> buffer);
    void ReplaceMorphWeightBuffer(std::unique_ptr<IGraphicsBuffer> buffer);
    void ClearInstancing()
    {
        m_instanceSource.reset(); m_instances.reset(); m_instanceRanges.clear();
        m_instanceBlocks.clear(); m_usesInstanceBlocks = false;
    }
    std::shared_ptr<Mesh> m_instanceSource;
    std::shared_ptr<InstanceData> m_instances;
    std::vector<InstanceRange> m_instanceRanges;
    std::vector<InstanceBlock> m_instanceBlocks;
    bool m_usesInstanceBlocks = false;
    // Lazy storage keeps ordinary meshes free of per-LOD allocations. The
    // active level lives in this mesh; its cache entry is an empty holder.
    std::vector<std::unique_ptr<Mesh>> m_runtimeLods;
    glm::vec3 m_runtimeLodEnvelopeMin{0.f};
    glm::vec3 m_runtimeLodEnvelopeMax{0.f};
    bool m_hasRuntimeLodEnvelope = false;
    int m_activeRuntimeLod = -1;
    int m_previousRuntimeLod = -1;
    float m_runtimeLodProgress = 1.f;
    float m_runtimeLodDuration = 0.f;
    float m_runtimeVisibility = 1.f;
    bool m_runtimeVisibilityFromEnd = false;
    uint64_t m_runtimeFadeRevision = 0;
    // PrepareAuthoredLods state: the base file and lodMeshes it installed.
    uint64_t m_authoredLodSignature = 0;
    bool m_authoredLodsFailed = false;
    IndexedGeometryTiming m_lastIndexedGeometryTiming;
    void ObserveMorphWeights() const;
    void AdvanceMorphWeightsRevision() const;
    void CreateMorphBuffers();
    std::vector<glm::vec4> BuildMorphDeltas();
    std::vector<glm::vec4> m_preparedMorphDeltas;
    void UploadMorphWeights();
    void UpdateBounds();
    std::string m_filePath;
    std::vector<Vertex> m_vertices;
    std::vector<TerrainVertex> m_terrainVertices;
    std::vector<uint32_t> m_indices;
    uint64_t m_authoredGeometryRevision = 1;
    uint64_t m_cpuGeometryRevision = 0;
    static uint64_t NextCpuGeometryRevision();
    mutable uint64_t m_textureUvRevision = 0;
    mutable std::array<float,2> m_textureUvScale{};
    std::shared_ptr<IGraphicsBuffer> m_vertexBuffer;
    bool m_sharedVertexBuffer = false, m_sharedIndexBuffer = false;
    void MakeGeometryWritable();
    std::unique_ptr<IGraphicsBuffer> m_posedVertexBuffer;
    std::vector<glm::vec3> m_surfaceOffsets;
    uint64_t m_surfaceOffsetsRevision = 1;
    std::shared_ptr<IGraphicsBuffer> m_indexBuffer;
    std::unique_ptr<IGraphicsBuffer> m_morphDeltaBuffer;
    uint32_t m_morphDeltaStrideFloat4 = 3;
    std::unique_ptr<IGraphicsBuffer> m_morphWeightBuffer;
    IGraphicsBufferFactory* m_bufferFactory = nullptr;
    bool m_ready = false;
    bool m_editorVisible = true;
    bool m_animationVisible = true;
    bool m_hasBounds = false;
    glm::vec3 m_boundsMin{};
    glm::vec3 m_boundsMax{};
    unsigned m_morphNodeIndex = 0;
    std::vector<MorphTarget> m_morphTargets;
    std::string m_morphFilePath;
    std::vector<float> m_morphWeights;
    std::vector<glm::vec4> m_packedMorphWeights;
    uint32_t m_activeMorphTargetCount = 0;
    mutable std::vector<float> m_observedMorphWeights;
    mutable uint64_t m_morphWeightsRevision = 1;
    mutable bool m_morphWeightsObserved = false;
    bool m_portalPoseSnapshot = false;
};
}

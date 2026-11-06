#pragma once
#include "Core/component.h"
#include "Core/PropertyMacros.h"
#include "Core/Graphics/IGraphicsBuffer.h"
#include "Core/Model/MeshData.h"
#include <cstdint>
#include <algorithm>
#include <string>
#include <vector>
#include <memory>
#include <optional>
#include <initializer_list>

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
    // Runtime classification; the project grass shadow budget supplements the layer budget.
    bool grassShadowCaster = false;
    // Authored foliage shares the grass wind field, with restrained tree motion.
    PROPERTY(Inspector,EditAnywhere,Category="Foliage",ClampMin="0",ClampMax="1")
    float foliageWindStrength = 0.f;
    // Transient per-object caster range; zero means unrestricted. Evaluated on
    // the GPU so distance changes do not toggle whole instance tiles.
    float runtimeShadowDistance = 0.f;
    float runtimeShadowFadeWidth = 1.f;
    PROPERTY(Inspector,EditAnywhere,Category="LOD")
    bool gpuLodSelection = false;
    // Independent resident prefab bands may use different materials (e.g. impostors).
    // Transient; all bands share placement records and are selected on the GPU.
    int prefabLodBand = -1;
    int prefabShadowLod = -1; // transient fixed shadow band; -1 follows distance
    glm::vec2 prefabLodDistances{0.f};
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
    void CreateBuffer(IGraphicsBufferFactory* bufferFactory);

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
        uint64_t revision=1;
        uint64_t slicesRevision=1;
        uint64_t stableId=0;
        std::vector<uint64_t> placementIds;
        void MarkDirty() { ++revision; }
        struct Slice
        {
            uint32_t firstInstance = 0, instanceCount = 0;
            std::unique_ptr<IGraphicsBuffer> buffer;
        };
        std::vector<InstanceTransform> transforms;
        std::vector<Slice> slices;
    };
    struct InstanceRange
    {
        uint32_t firstVertex = 0, vertexCount = 0;
        uint32_t firstInstance = 0, instanceCount = 0;
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
    bool SetTerrainGeometry(TerrainMeshData&& geometry,
        bool retainExpandedVertices = false);
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
    bool IsRuntimeLodResident(int lod) const;
    Mesh* GetRuntimeLodMesh(int lod) const
    { return !IsRuntimeLodResident(lod) ? nullptr : lod == m_activeRuntimeLod ? const_cast<Mesh*>(this) : m_runtimeLods[lod].get(); }
    bool SelectRuntimeLod(int lod);
    int GetActiveRuntimeLod() const { return m_activeRuntimeLod; }
    bool BeginRuntimeLodTransition(int lod, float durationSeconds);
    bool AdvanceRuntimeLodTransition(float deltaSeconds);
    Mesh* GetPreviousRuntimeLodMesh() const;
    glm::vec4 GetRuntimeGeometryFade(bool previous = false) const;
    void SetRuntimeVisibility(float visibility);
    float GetRuntimeVisibility() const { return m_runtimeVisibility; }
    uint64_t GetRuntimeFadeRevision() const { return m_runtimeFadeRevision; }
    int ChooseRuntimeLod(float distance, std::initializer_list<float> thresholds,
        float hysteresis = 0.f) const;
    int ChooseRuntimeLod(float distance, const std::vector<float>& thresholds,
        float hysteresis = 0.f) const;
    size_t GetResidentRuntimeLodCount() const;
    uint64_t GetRuntimeLodGpuMemoryBytes() const;
    // Releases cached alternatives, retaining the currently displayed geometry.
    void ClearRuntimeLods();
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
    float GetFoliageWindPadding() const
    { return 1.5f * std::max(0.f,m_boundsMax.y) * std::clamp(foliageWindStrength,0.f,1.f); }
    glm::vec3 GetBoundsMin() const { return m_boundsMin-glm::vec3(GetFoliageWindPadding()); }
    glm::vec3 GetBoundsMax() const { return m_boundsMax+glm::vec3(GetFoliageWindPadding()); }
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

    bool        DrawProperties(::Engine::Editor::IEditorUi& ui) override;
    JsonValue   Serialize() const override;
    void        Deserialize(const JsonValue& v) override;
    void        DeserializeLegacyMorphTargets(const JsonValue& value);
    void        OnAfterDeserialize(IGraphicsProvider* graphicsProvider) override;
    void PrepareGraphicsResources(IGraphicsProvider* graphicsProvider) override;
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
    { m_instanceSource.reset(); m_instances.reset(); m_instanceRanges.clear(); }
    std::shared_ptr<Mesh> m_instanceSource;
    std::shared_ptr<InstanceData> m_instances;
    std::vector<InstanceRange> m_instanceRanges;
    // Lazy storage keeps ordinary meshes free of per-LOD allocations. The
    // active level lives in this mesh; its cache entry is an empty holder.
    std::vector<std::unique_ptr<Mesh>> m_runtimeLods;
    int m_activeRuntimeLod = -1;
    int m_previousRuntimeLod = -1;
    float m_runtimeLodProgress = 1.f;
    float m_runtimeLodDuration = 0.f;
    float m_runtimeVisibility = 1.f;
    uint64_t m_runtimeFadeRevision = 0;
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

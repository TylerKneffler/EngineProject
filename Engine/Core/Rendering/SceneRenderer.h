#pragma once
#include "Core/Rendering/Atmosphere/PhysicalSky.h"
#include "Core/Rendering/VegetationInteraction.h"
#include "Core/Rendering/OcclusionBuffer.h"
#include "Core/Rendering/RenderBoundsCache.h"
#include "Core/Rendering/DrawBatchPreparation.h"
#include "Core/Rendering/ShaderSourceCache.h"
#include "Core/Rendering/TextureFootprintCache.h"
#include "Core/Rendering/UploadRecordCache.h"
#include "Core/Rendering/PersistentInstanceBatches.h"
#include "Core/Rendering/InstanceTilePreparation.h"
#include "Core/Graphics/IPipelineState.h"
#include "Core/Graphics/IGraphicsBuffer.h"
#include "Core/Rendering/Lighting/Pipelines/Realtime/RealtimeLightingPipeline.h"
#include "Core/Rendering/Lighting/Pipelines/Baked/BakedLightingPipeline.h"
#include "Core/Rendering/RenderPerformance.h"
#include "Core/Rendering/RenderSpatialIndex.h"
#include "Core/Rendering/AutomaticInstancing.h"
#include "Core/Rendering/PerformanceHeat.h"
#include "Core/Rendering/SceneViewOverlays.h"
#include "Core/Scene/Scene.h"
#include "Core/Scene/SceneObserver.h"

#include <array>
#include <future>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Engine::Renderers { class UIRenderer; }

namespace Engine::Rendering
{
class SceneInstanceBatching;
class ScenePortalRendering;
class ScenePortalViewScheduling;
class SceneRendererDebugPipelines;
class SceneRendererDirectionalShadowSubmission;
class SceneRendererDrawPreparation;
class SceneRendererFramePreparation;
class SceneRendererGeometrySubmission;
class SceneRendererLighting;
class SceneRendererLodSelection;
class SceneRendererMaterialShaders;
class SceneRendererObjectPipelines;
class SceneRendererObjectRecordUpload;
class SceneRendererParticles;
class SceneRendererPipelines;
class SceneRendererResources;
class SceneRendererShadows;
class SceneRendererViewPreparation;
class SceneRendererVisibility;
class RenderSystem;
class BakedLightingData;
class EnvironmentMap;

// ---------------------------------------------------------------------------
// SceneRenderer
//
// Draws one Scene. Owns every GPU resource, cache and statistic needed to turn
// the scene's objects into frames: pipelines, upload buffers, frame items,
// shadow/occlusion/cluster caches, material shaders, LOD and instancing
// state, the scene UI renderer and the lighting pipelines.
//
// The Scene owns object memory only and does not know this class exists.
// Renderers are owned by Engine::Rendering::RenderSystem, one per Scene,
// created on first use and destroyed when the Scene is destroyed:
//
//   auto& renderer = Engine::Rendering::RenderSystem::For(scene);
//   renderer.PrepareRenderFrame();
//   renderer.Render(context, aspect, camera);
//
// The renderer reads the scene (objects, settings, transforms) and keeps its
// caches coherent through ISceneObserver notifications.
// ---------------------------------------------------------------------------
class SceneRenderer final : public Engine::Scene::ISceneObserver
{
public:
    using Scene = Engine::Scene::Scene;
    using Object = Engine::Core::Object;
    using Camera = Engine::Components::Camera;
    using IGraphicsProvider = Engine::Graphics::IGraphicsProvider;
    using IGraphicsContext = Engine::Graphics::IGraphicsContext;
    using IPipelineState = Engine::Graphics::IPipelineState;
    using IGraphicsBuffer = Engine::Graphics::IGraphicsBuffer;
    using SpatialWarpSnapshot = Scene::SpatialWarpSnapshot;
    using RetiredGraphicsResources = Scene::RetiredGraphicsResources;

    // Use RenderSystem::For(scene); a renderer is bound to one scene for life.
    SceneRenderer(Scene& scene, IGraphicsProvider* graphicsProvider);
    ~SceneRenderer() override;
    SceneRenderer(const SceneRenderer&) = delete;
    SceneRenderer& operator=(const SceneRenderer&) = delete;

    Scene& GetScene() { return m_scene; }
    const Scene& GetScene() const { return m_scene; }
    IGraphicsProvider* GetGraphicsProvider() const { return m_graphicsProvider; }

    // Adds the renderer's retired pipelines and shadow textures to resources
    // the scene retired (RenderSystem::TakeRetiredGraphicsResources).
    void TakeRetiredGraphicsResources(RetiredGraphicsResources& retired);

    // ---- ISceneObserver -------------------------------------------------------
    void OnObjectsClearing() override;
    void OnStructureChanged(Object* owner) override;
    void OnBackgroundWorkFence() override;
    void OnSceneDestroying() override;

    struct NativeRenderPacket;
    struct NativePacketRecycler;

    // Opt-in alpha-prepass experiment. Never serialized or enabled by policy.
    void SetDiagnosticOpaqueDepthEqual(bool enabled) { m_diagnosticOpaqueDepthEqual = enabled; }
    bool DiagnosticOpaqueDepthEqualReady() const { return m_diagnosticOpaqueDepthEqualReady; }
    struct PrepareTimings
    {
        double assetPublicationMs=0,texturePublicationMs=0,shaderSourcePollMs=0,shaderPipelinePublicationMs=0;
        double objectCommitMs=0,prefabInstancesMs=0,prefabSignatureMs=0,prefabSourceProbeMs=0;
        double prefabPlacementAssetsMs=0,prefabLodProfilesMs=0,prefabSourceSetMs=0;
        double prefabRegistryMs=0,prefabValidationMs=0,prefabSourceLookupMs=0;
        double prefabBuildMs=0,prefabMainBufferCreateMs=0;
        double prefabBudgetOverrunMs=0,prefabMaxLayerMs=0;
        double graphicsPreparationMs=0,nativeBuffersMs=0;
        double lookupMs = 0.0;
        double itemsMs = 0.0;
        double itemValidationMs=0, itemTexturesMs=0, itemShadersMs=0;
        double itemMeshStateMs=0, itemBoundsMs=0, itemLodMs=0, itemBuildMs=0;
        uint64_t itemShaderRequests=0, itemShaderResolutions=0;
        double boneUploadMs = 0.0;
        double capacityMs = 0.0;
        double spatialMs = 0.0;
        uint64_t boundsReused=0,boundsUpdated=0;
        uint64_t staticRenderItemsReused=0,staticRenderItemsUpdated=0;
        uint64_t instancedRenderItemsReused=0,instancedRenderItemsUpdated=0;
        double workerMs=0,mainJobMs=0,workerWaitMs=0,workerQueueMs=0;
        double transformsMs=0,transformSnapshotMs=0,transformComputeMs=0,transformCommitMs=0;
        uint64_t transformLocalUpdated=0,transformWorldUpdated=0,transformWorldReused=0,transformLevels=0;
        uint64_t transformStructureFallback=0;
        double spatialIndexMs=0,spatialCompareMs=0,spatialRefitMs=0,spatialBuildMs=0;
        uint64_t spatialActiveSlots=0,spatialRetainedSlots=0;
        uint64_t spatialChangedBounds=0,spatialReusedBounds=0,spatialDirtyLeaves=0,spatialRefitNodes=0;
        uint64_t spatialRebuilds=0,spatialQualityRebuilds=0;
    };
    struct FrameUploadStats
    {
        uint64_t bytes=0, bufferAllocations=0, allocatorGrowths=0;
    };
    FrameUploadStats GetCurrentFrameUploadStats() const
    {
        if (!m_activeFrameUploadContext) return {};
        return {m_activeFrameUploadContext->uploadedBytes,
            m_activeFrameUploadContext->bufferAllocations,
            m_activeFrameUploadContext->allocatorGrowths};
    }
    const PrepareTimings& GetLastPrepareTimings() const { return m_lastPrepareTimings; }
    using RenderCpuTimings = Engine::Rendering::RenderCpuTimings;
    const RenderCpuTimings& GetLastRenderCpuTimings() const { return m_lastRenderCpuTimings; }
    // Most expensive draws from the last Performance render mode frame.
    PerformanceHeatSnapshot GetPerformanceHeatSnapshot() const
    {
        return m_performanceHeat.GetSnapshot();
    }
    const std::array<uint64_t,3>& GetScreenLodCounts() const { return m_screenLodCounts; }
    void ResetRenderCpuTimings() { m_lastRenderCpuTimings={}; }
    bool automaticInstancing = true;
    bool gpuInstanceCulling = true;
    // Draw all objects and scene helpers (grid).
    // context: Graphics rendering context (API-agnostic command recorder)
    // aspect: Viewport aspect ratio
    // cameraOverride: if non-null, use this camera instead of the editor camera.
    // includeEditorVisuals: draws editor-only overlays such as the grid and
    // selected-object outline. Game cameras must pass false.
    // PrepareRenderFrame() performs the camera-independent scene walk once;
    // multiple views can then reuse the resulting object, light, material,
    // and skin data.
    // Editor replay packets can defer publication to the following simulation
    // commit. Object mutations still commit before rendering in either mode.
    void PrepareRenderFrame(bool publishSnapshot = true);
    std::shared_ptr<const NativeRenderPacket> TakeNativeRenderPacket();
    void RetainNativePacketMeshBuffers(const NativeRenderPacket& packet,
        Engine::Graphics::GraphicsCommandPacket& commands) const;
    bool IsRecordingNativeCommands() const { return m_recordingNativeCommands; }
    // Record queued GPU particle steps before a Vulkan render pass begins.
    void RecordGpuParticleSimulation(IGraphicsContext* context);
    void Render(IGraphicsContext* context, float aspect,
        Camera* cameraOverride = nullptr, bool includeEditorVisuals = true,
          uint32_t viewportWidth = 0u, uint32_t viewportHeight = 0u,
          bool shadowOnly = false,
          const NativeRenderPacket* nativePacket = nullptr,
          int terrainObjectDisplayLod = -1,
          int sceneViewRenderMode = -1);
    // Bytes of object records made visible to the GPU by the most recent
    // Render call. This excludes vertex, light, and bone buffers.
    uint64_t GetLastObjectDataUploadBytes() const
    {
        return m_lastObjectDataUploadBytes;
    }
    uint32_t GetLastOcclusionCulledCount() const
    {
        return m_lastOcclusionCulledCount;
    }
    void SetLastNativeOcclusionCulledCount(uint32_t count)
    { m_scene.AssertOwnerThread(); m_lastOcclusionCulledCount = count; }
    uint32_t GetLastTerrainOcclusionCulledCount() const
    {
        return m_lastTerrainOcclusionCulledCount;
    }
    uint32_t GetLastOrdinaryDrawCount() const
    {
        return m_lastOrdinaryDrawCount;
    }
    uint64_t GetLastOrdinaryTriangleCount() const
    {
        return m_lastOrdinaryTriangleCount;
    }
    uint32_t GetLastParticlePortalDrawCount() const
    {
        return m_lastParticlePortalDrawCount;
    }
    uint32_t GetLastSkinnedObjectCount() const
    {
        return m_lastSkinnedObjectCount;
    }
    uint32_t GetLastRealtimeLightCount() const { return m_frameLightCount; }
    struct ShadowDebugSnapshot
    {
        bool enabled = false;
        bool hasSelectedLight = false;
        std::string selectedLight;
        glm::vec3 directionToLight { 0.f, 1.f, 0.f };
        uint32_t atlasResolution = 0;
        uint64_t atlasAllocationGeneration = 0;
        uint32_t cascadeCount = 0;
        uint32_t refreshedCascadeMask = 0; // Tiles rendered in the latest shadow pass.
        glm::vec4 cascadeSplits { 0.f };
        std::array<glm::vec3, 4> cascadeCenters{};
        std::array<float, 4> cascadeRadii{};
        uint32_t casterCount = 0;
        uint32_t submittedCasterDrawCount = 0;
        uint32_t bakedReceiverCount = 0;
        uint32_t dynamicReceiverCount = 0;
        uint32_t portalAtlasCount = 0;
        uint32_t renderedPortalAtlasCount = 0;
        uint32_t portalAtlasResolution = 0;
    };
    const ShadowDebugSnapshot& GetShadowDebugSnapshot() const
    {
        return m_shadowDebugSnapshot;
    }
    struct ShadowCpuTiming
    {
        Engine::Rendering::InstanceStorageTimings instances;
        uint64_t explicitInstanceDraws=0, automaticDraws=0, ordinaryDraws=0, gpuGrassBatches=0;
        double scenePreparationMs = 0;
        double cascadeCullingMs = 0;
        double recordUploadMs = 0;
        double drawSubmissionMs = 0;
        double constantBindingMs = 0;
        double textureBindingMs = 0;
        double geometryDrawMs = 0;
        double submissionWallMs = 0, maxBatchMs = 0;
        uint64_t submissionThreadCycles = 0;
        double passRestoreMs = 0;
        double portalMs = 0;
        double totalMs = 0;
        // Directional cascades (atlas tiles) re-rendered this frame, as a bit
        // mask; the others were kept from earlier frames.
        uint32_t refreshedCascadeMask = 0;
        // Grid cells of the far cascade redrawn by a region refresh this
        // frame (0: none, or the whole tile when its bit is in the mask).
        uint32_t farRegionCells = 0;
    };
    const ShadowCpuTiming& GetLastShadowCpuTiming() const
    {
        return m_lastShadowCpuTiming;
    }
    // ---- View overlays (render input) ---------------------------------------
    // A host (the editor) binds overlays it owns; the pointer must outlive the
    // binding. Unbound scenes render with defaults.
    void SetViewOverlays(const Engine::Rendering::SceneViewOverlays* overlays)
    { m_viewOverlays = overlays; m_renderFramePrepared = false; }
    const Engine::Rendering::SceneViewOverlays& GetViewOverlays() const
    {
        static const Engine::Rendering::SceneViewOverlays none;
        return m_viewOverlays ? *m_viewOverlays : none;
    }
    // Call after changing bound overlays that affect prepared frame data
    // (mesh edit pose, weight paint).
    void InvalidatePreparedFrame() { m_renderFramePrepared = false; }
    void SetDistanceLightingSettings(
        const Engine::Model::DistanceLightingSettings& distanceSettings)
    {
        m_distanceLightingSettings = distanceSettings;
    }
    // Output pixels per rendered pixel (>= 1) when views render below their
    // output size and are upscaled. Detail choices (LOD coverage, the detail
    // budget, texture footprints) use output pixels so a lower render scale
    // does not also coarsen meshes and textures; rasterisation, jitter,
    // occlusion and screen-space passes keep the render size. Set before
    // Render, on the thread that calls it.
    void SetDetailResolutionScale(float scale)
    {
        m_detailResolutionScale = std::isfinite(scale) ? std::max(1.f, scale) : 1.f;
    }
    void SetRealtimeShadowSettings(
        const Engine::Model::RealtimeShadowSettings& shadowSettings)
    {
        m_realtimeShadowSettings = shadowSettings;
    }
    // Discard retained directional tiles after external caster changes, or
    // to request a complete reference refresh in rendering diagnostics.
    void InvalidateDirectionalShadowCache()
    { m_scene.AssertOwnerThread(); m_directionalShadowCacheValid=false; }
    // Pointer coordinates relative to the surface displaying the game render
    // target, used to keep embedded-view UI hit bounds aligned.
    void SetUiPointerInput(float x, float y, float viewportWidth,
        float viewportHeight, bool hovered, bool mouseDown);
    void CaptureEmbeddedUiInput(Engine::Core::NativeInputSnapshot& input) const;
    Engine::Model::BakeResult BakeLighting(
        const std::string& assetsDirectory = "Assets",
        const std::string& sceneName = "Scene",
        const Engine::Model::BakedLightingSettings& bakeSettings = {},
        const std::function<void(float)>& progress = {},
        const std::function<bool()>& cancelled = {});
    void ClearBakedLighting();
    void SetLightingBakeStatus(std::string status)
    { m_scene.AssertOwnerThread(); m_lightingBakeStatus = std::move(status); }
    const std::string& GetLightingBakeStatus() const { return m_lightingBakeStatus; }
    const Engine::Components::Texture* GetSkyboxPreviewTexture();

private:
    friend class SceneInstanceBatching;
    friend class ScenePortalRendering;
    friend class ScenePortalViewScheduling;
    friend class SceneRendererDebugPipelines;
    friend class SceneRendererDirectionalShadowSubmission;
    friend class SceneRendererDrawPreparation;
    friend class SceneRendererFramePreparation;
    friend class SceneRendererGeometrySubmission;
    friend class SceneRendererLighting;
    friend class SceneRendererLodSelection;
    friend class SceneRendererMaterialShaders;
    friend class SceneRendererObjectPipelines;
    friend class SceneRendererObjectRecordUpload;
    friend class SceneRendererParticles;
    friend class SceneRendererPipelines;
    friend class SceneRendererResources;
    friend class SceneRendererShadows;
    friend class SceneRendererViewPreparation;
    friend class SceneRendererVisibility;
    friend class RenderSystem;

    // Builds GPU resources (constant buffers, pipelines, UI renderer).
    void Initialize();

    // Scene GPU-resource lifetime the passes share (Scene keeps these because
    // components retire their own buffers there too).
    bool DefersGraphicsRetirement() const { return m_scene.m_deferGraphicsRetirement; }
    std::vector<std::unique_ptr<IGraphicsBuffer>>& RetiredSceneBuffers() { return m_scene.m_retiredRenderBuffers; }
    const Engine::Components::TransformHierarchy& SceneTransforms() const { return m_scene.m_transformHierarchy; }

    Scene& m_scene;
    IGraphicsProvider* m_graphicsProvider = nullptr;
    std::unique_ptr<IPipelineState> m_gridPipeline;
    std::unique_ptr<IGraphicsBuffer> m_gridConstantBuffer;
    void* m_gridCBMapped = nullptr;

    struct ParticleDrawSlot
    {
        struct Batch
        {
            uint32_t first = 0;
            uint32_t count = 0;
            std::shared_ptr<Engine::Components::Texture> texture;
            bool alphaBlend = false;
            const Object* owner = nullptr;
            float softness = 0.f;         // ParticleEmitter::softness
            float nearFadeDistance = 0.f; // ParticleEmitter::nearFadeDistance
        };
        struct FluidSurfaceBatch
        {
            std::unique_ptr<IGraphicsBuffer> particles;
            const Object* owner = nullptr;
            float opacity = 1.f;
            uint32_t count = 0;
        };
        std::unique_ptr<IGraphicsBuffer> vertices;
        std::unique_ptr<IGraphicsBuffer> constants;
        void* mappedVertices = nullptr;
        void* mappedConstants = nullptr;
        uint32_t vertexCapacity = 0;
        uint32_t constantCapacity = 0;
        uint32_t particleCount = 0;
        uint32_t constantCursor = 0;
        uint64_t preparedSerial = 0;
        std::vector<Batch> batches;
        std::vector<FluidSurfaceBatch> fluidSurfaces;
        // One fog record (camera position + fog0..3) per particle pass this
        // frame (main view and each portal view); reused across frames.
        std::vector<std::unique_ptr<IGraphicsBuffer>> fogBuffers;
        uint32_t fogCursor = 0;
    };
    // Height fog of the current lit camera view, packed for
    // AtmosphereFog.hlsli; zero when the view has no fog. Particles read it.
    std::array<glm::vec4, 4> m_viewAtmosphereFog{};
    std::unique_ptr<IPipelineState> m_particlePipeline;
    std::unique_ptr<IPipelineState> m_particleAlphaPipeline;
    std::unique_ptr<IPipelineState> m_particlePortalPipeline;
    std::unique_ptr<IPipelineState> m_particlePortalAlphaPipeline;
    std::unique_ptr<IPipelineState> m_particleGpuPipeline;
    std::unique_ptr<IPipelineState> m_particleGpuAlphaPipeline;
    std::unique_ptr<IPipelineState> m_particleGpuPortalPipeline;
    std::unique_ptr<IPipelineState> m_particleGpuPortalAlphaPipeline;
    std::unique_ptr<IPipelineState> m_fluidSurfacePipeline;
    std::unique_ptr<IPipelineState> m_fluidSurfacePortalPipeline;
    std::vector<ParticleDrawSlot> m_particleDrawSlots;

    std::unique_ptr<IPipelineState> m_skyboxPipeline;
    std::unique_ptr<IPipelineState> m_portalSkyboxStencilReadPipeline;
    // Volumetric clouds (Shaders/Clouds/Clouds.hlsl): a compute march at half
    // resolution into m_cloudSamples, composited over the sky by m_cloudPipeline.
    std::unique_ptr<IPipelineState> m_cloudPipeline;
    std::unique_ptr<IPipelineState> m_cloudMarchPipeline;
    std::unique_ptr<IGraphicsBuffer> m_cloudSamples;
    uint64_t m_cloudSampleCapacity = 0;
    bool m_cloudsMarched = false; // this view's march is ready to composite
    // Screen-space ambient occlusion (SceneSettings::ambientOcclusion,
    // Shaders/AmbientOcclusion): after the main view's draws, the scene depth
    // is copied into m_ambientOcclusionDepth and GTAO writes one of two
    // buffers (the other is its history). Draws bind the latest at
    // structured slot 47; m_ambientOcclusionOff (an invalid header) when
    // there is none.
    std::unique_ptr<IPipelineState> m_ambientOcclusionPipeline;
    std::unique_ptr<IGraphicsBuffer> m_ambientOcclusionDepth;
    // GPU occlusion pyramid built from the main view's depth copy after the
    // AO pass (Shaders/InstanceCull/DepthPyramid.hlsl); the next frame's
    // camera culling tests against it when the camera has barely moved
    // (SceneRendererLighting::DepthPyramidForCulling).
    std::unique_ptr<IPipelineState> m_depthPyramidPipeline;
    std::unique_ptr<IGraphicsBuffer> m_depthPyramid;
    uint64_t m_depthPyramidBytes = 0;
    bool m_depthPyramidValid = false;
    const void* m_depthPyramidCamera = nullptr;
    glm::vec3 m_depthPyramidPosition{0.f}, m_depthPyramidForward{0.f, 0.f, -1.f};
    uint32_t m_depthPyramidViewportWidth = 0, m_depthPyramidViewportHeight = 0;
    uint64_t m_depthPyramidFrameSerial = 0;
    std::array<std::unique_ptr<IGraphicsBuffer>, 2> m_ambientOcclusionBuffers;
    std::unique_ptr<IGraphicsBuffer> m_ambientOcclusionOff;
    uint64_t m_ambientOcclusionDepthBytes = 0, m_ambientOcclusionBufferBytes = 0;
    uint32_t m_ambientOcclusionLatest = 0;  // buffer written last
    bool m_ambientOcclusionReady = false;   // latest holds this view's AO
    // m_ambientOcclusionDepth holds the current main view's depth (copied
    // by the AO pass, or by soft particles when AO is off). Reset per view.
    bool m_sceneDepthCopied = false;
    // Main view extent and reverse-Z reconstruction (view depth =
    // y / (depth - x)), recorded by ComputeAmbientOcclusion; zero when the
    // view is not a perspective one.
    uint32_t m_sceneDepthWidth = 0, m_sceneDepthHeight = 0;
    glm::vec2 m_sceneDepthReconstruct{ 0.f };
    uint32_t m_ambientOcclusionFrame = 0;
    // Physical sky (SceneSettings::physicalSky): model + panorama, rebuilt on
    // a worker, and this view's atmosphere terms at the camera
    // (SceneRendererLighting::ApplyPhysicalAtmosphere). Ready is false while
    // the sky is off or still building; the HDRI sky and height fog apply then.
    Engine::Rendering::PhysicalSky m_physicalSky;
    bool m_physicalAtmosphereReady = false;
    glm::vec3 m_physicalToSun{ 0.f, 1.f, 0.f };
    glm::vec3 m_physicalSunIlluminance{ 0.f };    // after the atmosphere
    glm::vec3 m_physicalMultipleScattering{ 0.f }; // Psi_ms x sun illuminance
    std::unique_ptr<IGraphicsBuffer> m_skyboxConstantBuffer;
    void* m_skyboxCBMapped = nullptr;
    // Sun disc constants of the current view (see SceneRendererLighting::ResolveSkySun).
    glm::vec4 m_skySunDirection{ 0.f };
    glm::vec4 m_skySunRadiance{ 0.f };
    std::shared_ptr<Engine::Components::Texture> m_defaultSkyboxTexture;
    std::shared_ptr<Engine::Components::Texture> m_sceneSkyboxTexture;
    std::string m_loadedSkyboxPath;
    std::string m_environmentLightingPath;
    std::array<glm::vec4, 9> m_environmentSH{};
    std::shared_ptr<Engine::Rendering::EnvironmentMap> m_environmentMap;

    std::unique_ptr<IPipelineState> m_objectPipeline;
    std::unique_ptr<IPipelineState> m_objectDoubleSidedPipeline;
    // [doubleSided]: opaque materials with parallaxDepthOffset.
    std::array<std::unique_ptr<IPipelineState>, 2> m_objectParallaxDepthPipelines;
    std::array<std::unique_ptr<IPipelineState>, 2> m_foliageDepthPipelines;
    std::array<std::unique_ptr<IPipelineState>, 2> m_foliageEqualPipelines;
    std::unique_ptr<IPipelineState> m_objectBlendPipeline;
    std::unique_ptr<IPipelineState> m_objectBlendDoubleSidedPipeline;
    std::unique_ptr<IPipelineState> m_objectWirePipeline;
    std::unique_ptr<IPipelineState> m_objectWireDoubleSidedPipeline;
    std::unique_ptr<IPipelineState> m_objectBlendWirePipeline;
    std::unique_ptr<IPipelineState> m_objectBlendWireDoubleSidedPipeline;
    std::unique_ptr<IPipelineState> m_objectPreviewPipeline;
    std::unique_ptr<IPipelineState> m_objectPreviewDoubleSidedPipeline;
    std::unique_ptr<IPipelineState> m_objectPreviewWirePipeline;
    std::unique_ptr<IPipelineState> m_objectPreviewWireDoubleSidedPipeline;
    std::unique_ptr<IPipelineState> m_objectPortalStencilWritePipeline;
    std::unique_ptr<IPipelineState> m_objectPortalStencilIncrementPipeline;
    std::unique_ptr<IPipelineState> m_objectPortalDepthResetPipeline;
    std::unique_ptr<IPipelineState> m_objectPortalStencilReadPipeline;
    std::unique_ptr<IPipelineState> m_objectPortalStencilReadDoubleSidedPipeline;
    std::unique_ptr<IPipelineState> m_objectPortalStencilReadBlendPipeline;
    std::unique_ptr<IPipelineState> m_objectPortalStencilReadBlendDoubleSidedPipeline;
    std::unique_ptr<IPipelineState> m_objectPortalStencilReadWirePipeline;
    std::unique_ptr<IPipelineState> m_objectPortalStencilReadWireDoubleSidedPipeline;
    std::unique_ptr<IPipelineState> m_objectPortalStencilReadBlendWirePipeline;
    std::unique_ptr<IPipelineState> m_objectPortalStencilReadBlendWireDoubleSidedPipeline;
    std::unique_ptr<IPipelineState> m_objectPortalStencilReadPreviewPipeline;
    std::unique_ptr<IPipelineState> m_objectPortalStencilReadPreviewDoubleSidedPipeline;
    std::unique_ptr<IPipelineState> m_objectPortalStencilReadPreviewWirePipeline;
    std::unique_ptr<IPipelineState> m_objectPortalStencilReadPreviewWireDoubleSidedPipeline;
    std::unique_ptr<IPipelineState> m_objectSpatialDebugPipeline;
    std::unique_ptr<IPipelineState> m_objectSpatialDebugWirePipeline;
    std::unique_ptr<IPipelineState> m_objectOutlinePipeline;
    std::unique_ptr<IPipelineState> m_pmxOutlinePipeline;
    std::unique_ptr<IPipelineState> m_directionalShadowPipeline;
    std::unique_ptr<IPipelineState> m_directionalShadowDoubleSidedPipeline;
    std::unordered_map<const IPipelineState*, std::unique_ptr<IPipelineState>>
        m_terrainPipelineByBase;
    using MaterialShaderBuilder = std::function<std::unique_ptr<IPipelineState>(
        const Engine::Graphics::IShader*, const Engine::Graphics::IShader*,
        bool, const std::string&)>;
    std::unordered_map<const IPipelineState*, MaterialShaderBuilder> m_materialShaderBuilders;
    struct MaterialShaderBytecode
    {
        std::shared_ptr<Engine::Graphics::IShader> vertex, pixel;
        std::string error;
    };
    using MaterialShaderFuture = std::shared_future<MaterialShaderBytecode>;
    std::unordered_map<std::string, MaterialShaderFuture> m_materialShaderBytecode;
    // Bumped whenever m_materialShaderBytecode is cleared (pipeline rebuild).
    uint64_t m_materialShaderBytecodeEpoch = 0;
    MaterialShaderFuture QueueMaterialShaderBytecode(const std::string& path,
        bool terrain, int64_t stamp);
    Engine::Rendering::ShaderSourceCache m_materialShaderSources;
    const Engine::Rendering::ShaderSourceCache::Source& ResolveMaterialShaderSource(
        const Engine::Components::Material& material, const std::string& path);
    // Deduplicate prewarm requests per frame; source resolution survives frames.
    // Ordered by (terrain, shader, material file); the transparent comparator
    // lets each per-item lookup use string views, so only a new combination
    // allocates.
    struct PreparedMaterialShaderKey
    {
        bool terrain = false;
        std::string_view shader, file;
    };
    struct PreparedMaterialShaderSource
    {
        bool terrain = false;
        std::string shader, file;
        PreparedMaterialShaderKey View() const { return { terrain, shader, file }; }
    };
    struct PreparedMaterialShaderLess
    {
        using is_transparent = void;
        static PreparedMaterialShaderKey View(const PreparedMaterialShaderKey& key) { return key; }
        static PreparedMaterialShaderKey View(const PreparedMaterialShaderSource& key) { return key.View(); }
        template<class A, class B> bool operator()(const A& left, const B& right) const
        {
            const auto a = View(left), b = View(right);
            if (a.terrain != b.terrain) return a.terrain < b.terrain;
            if (const int order = a.shader.compare(b.shader)) return order < 0;
            return a.file < b.file;
        }
    };
    std::set<PreparedMaterialShaderSource, PreparedMaterialShaderLess> m_preparedMaterialShaderSources;
    void PrewarmMaterialShaders(const Engine::Components::Material& material,
        bool terrain);
    struct MaterialShaderPipeline
    {
        std::unique_ptr<IPipelineState> pipeline;
        // The pipeline from before a source change, still drawn with while
        // the new one compiles (draws would otherwise vanish meanwhile);
        // retired once `pipeline` is built.
        std::unique_ptr<IPipelineState> previous;
        int64_t sourceStamp = 0;
        std::string error;
        MaterialShaderFuture bytecode;
        const IPipelineState* base = nullptr;
        bool overlay = false;
        std::string blendMode;
    };
    std::unordered_map<std::string, MaterialShaderPipeline> m_materialShaderPipelines;
    // Preserve pipelines referenced by earlier queued frames after source edits.
    std::vector<std::unique_ptr<IPipelineState>> m_retiredMaterialShaderPipelines;
    IPipelineState* ResolveMaterialShaderPipeline(const Engine::Rendering::ShaderSourceCache::Source& source,
        IPipelineState* base, bool terrain, bool overlay,
        const std::string& blendMode, std::string& error);
    void PublishMaterialShaderPipelines(size_t maxPipelines = 2);
    std::shared_ptr<Engine::Graphics::IGraphicsTexture> m_directionalShadowMap;
    uint32_t m_directionalShadowResolution = 0;
    // Rows of cascade tiles in the atlas: 2 (2x2), or 3 with the refresh
    // blend, whose two spare tiles take a refreshed cascade's new image while
    // its previous one fades out (SceneRendererShadows).
    uint32_t m_directionalShadowAtlasRows = 2;
    std::array<uint32_t, 4> m_directionalShadowCascadeTile{ 0u, 1u, 2u, 3u };
    struct DirectionalShadowRefreshBlend
    {
        bool active = false;
        uint32_t cascade = 0, previousTile = 0;
        uint64_t startFrame = 0;
        glm::vec3 clipOffset{ 0.f };
        float weight = 1.f;
    };
    std::array<DirectionalShadowRefreshBlend, 2> m_directionalShadowBlends{};
    uint64_t m_directionalShadowBlendFrame = 0;
    Engine::Rendering::TextureFootprintCache m_textureFootprintCache;
    uint64_t m_directionalShadowAllocationGeneration = 0;
    bool m_directionalShadowCacheValid = false;
    Engine::Model::GraphicsQualityValues m_directionalShadowCachedQuality;
    uint64_t m_directionalShadowFrameCounter = 0;
    uint64_t m_directionalShadowLastRenderedFrame = 0;
    uint32_t m_directionalShadowCachedResolution = 0;
    uint32_t m_directionalShadowCachedCascadeCount = 1;
    std::string m_directionalShadowCachedLightKey;
    glm::vec3 m_directionalShadowCachedCameraPosition { 0.f };
    glm::vec3 m_directionalShadowCachedDirection { 0.f, 1.f, 0.f };
    std::array<glm::mat4, 4> m_directionalShadowCachedViewProjections{
        glm::mat4(1.f), glm::mat4(1.f), glm::mat4(1.f), glm::mat4(1.f) };
    std::array<glm::vec3, 4> m_directionalShadowCachedCascadeCenters{
        glm::vec3(0.f), glm::vec3(0.f), glm::vec3(0.f), glm::vec3(0.f) };
    std::array<float, 4> m_directionalShadowCachedCascadeRadii{
        0.f, 0.f, 0.f, 0.f };
    glm::vec4 m_directionalShadowCachedCascadeSplits { 0.f };
    glm::vec4 m_directionalShadowCachedCascadeData { 0.f };
    glm::vec4 m_directionalShadowCachedCameraData { 0.f };
    std::array<uint64_t, 4> m_directionalShadowCascadeLastRenderedFrame{
        0u, 0u, 0u, 0u };
    uint32_t m_directionalShadowRefreshCursor = 0;
    // Structure revision the far shadow tile was last rendered with
    // (SceneRendererShadows): it refreshes when objects stream in or out.
    uint64_t m_directionalShadowFarStructureRevision = 0;
    // Shadow content each cached cascade was last rendered with: a signature
    // of the casters (and their instance blocks) inside its light-space
    // square (SceneRendererShadows). A stepped cascade whose casters changed
    // since (trees streamed in or out, a near/far block hand-off) refreshes,
    // instead of keeping their old shadows until the camera crosses a step.
    std::array<uint64_t, 4> m_directionalShadowCascadeContent{ 0u, 0u, 0u, 0u };
    uint32_t m_directionalShadowContentCursor = 0;
public:
    // The far cascade's caster signatures per region of a grid over its tile
    // (SceneRendererShadows): the regions whose casters changed since they
    // were drawn are redrawn alone, within a few frames.
    static constexpr uint32_t kFarShadowRegionGrid = 8u;
    using FarShadowRegionContent = std::array<uint64_t, kFarShadowRegionGrid * kFarShadowRegionGrid>;
private:
    FarShadowRegionContent m_directionalShadowFarRegionContent{};
    bool m_directionalShadowFarRegionContentValid = false;
    uint64_t m_directionalShadowFarRegionCheckFrame = 0;
    struct PortalShadowCacheEntry
    {
        uint64_t key = 0;
        const Engine::Components::Camera* ownerCamera = nullptr;
        const Engine::Components::SpatialManipulator* targetChart = nullptr;
        const Object* contentScopeRoot = nullptr;
        glm::mat4 view { 1.f };
        glm::vec3 cameraPosition { 0.f };
        uint32_t depth = 0;
        float screenArea = 0.f;
        uint64_t touchedGeneration = 0;
        std::shared_ptr<Engine::Graphics::IGraphicsTexture> atlas;
        uint32_t atlasResolution = 0;
        std::unique_ptr<IGraphicsBuffer> constantBuffer;
        void* constantMapped = nullptr;
        std::unique_ptr<IGraphicsBuffer> objectBuffer;
        void* objectMapped = nullptr;
        uint32_t bufferCapacity = 0;
        std::array<glm::mat4, 4> viewProjections{
            glm::mat4(1.f), glm::mat4(1.f), glm::mat4(1.f), glm::mat4(1.f) };
        glm::vec4 cascadeSplits { 0.f };
        glm::vec4 cascadeData { 0.f };
        glm::vec4 cameraData { 0.f };
        bool rendered = false;
        bool selected = false;
    };
    std::vector<PortalShadowCacheEntry> m_portalShadowCache;
    uint64_t m_portalShadowGeneration = 0;
    std::vector<std::shared_ptr<Engine::Graphics::IGraphicsTexture>>
        m_retiredShadowTextures;
    std::unique_ptr<IGraphicsBuffer> m_objectConstantBuffer;
    void* m_objectCBMapped = nullptr;
    std::unique_ptr<IGraphicsBuffer> m_objectDataBuffer;
    void* m_objectDataMapped = nullptr;
    Engine::Rendering::UploadRecordCache m_objectUploadCache,m_shadowUploadCache;
    std::unique_ptr<IGraphicsBuffer> m_shadowConstantBuffer;
    void* m_shadowCBMapped = nullptr;
    std::unique_ptr<IGraphicsBuffer> m_shadowObjectDataBuffer;
    void* m_shadowObjectDataMapped = nullptr;
    std::unique_ptr<IGraphicsBuffer> m_portalObjectDataBuffer;
    void* m_portalObjectDataMapped = nullptr;
    std::unique_ptr<IGraphicsBuffer> m_lightDataBuffer;
    void* m_lightDataMapped = nullptr;
    std::unique_ptr<IGraphicsBuffer> m_boneDataBuffer;
    void* m_boneDataMapped = nullptr;
    // Capacity growth can occur while prior command buffers are still in
    // flight. Game rendering moves these into fence-tagged retirement batches.
    std::unique_ptr<IGraphicsBuffer> m_emptyMorphDeltaBuffer;
    // GPU copies of camera occlusion buffers for the instance cull, one ring
    // per prepared frame slot (several views may upload in one frame), and
    // an empty one (level count 0) bound when a dispatch has none.
    struct OcclusionPyramidRing
    {
        uint64_t serial = UINT64_MAX;
        size_t used = 0;
        std::vector<std::unique_ptr<IGraphicsBuffer>> buffers;
    };
    std::vector<OcclusionPyramidRing> m_occlusionPyramidRings;
    std::unique_ptr<IGraphicsBuffer> m_occlusionPyramidEmpty;
    // Last camera occlusion buffer, reused while camera and terrain are still.
    Engine::Rendering::OcclusionBufferCache m_occlusionBufferCache;
    std::unique_ptr<IGraphicsBuffer> m_emptyMorphWeightBuffer;
    std::shared_ptr<Engine::Graphics::IGraphicsTexture> m_brdfIntegrationLut;
    std::unique_ptr<IGraphicsBuffer> m_clusterHeaderBuffer;
    std::unique_ptr<IGraphicsBuffer> m_clusterLightIndexBuffer;
    void* m_clusterHeaderMapped = nullptr;
    void* m_clusterLightIndexMapped = nullptr;
    uint32_t m_clusterHeaderCapacity = 0;
    uint32_t m_clusterLightIndexCapacity = 0;
    std::vector<glm::uvec2> m_clusterHeaderScratch;
    std::vector<uint32_t> m_clusterLightIndexScratch;
    std::unique_ptr<IGraphicsBuffer> m_portalApertureBuffer;
    void* m_portalApertureMapped = nullptr;
    struct NativeFrameBuffers
    {
        std::unique_ptr<IGraphicsBuffer> grid, skybox, lights, bones;
        std::unique_ptr<IGraphicsBuffer> clusterHeaders, clusterIndices;
        std::unique_ptr<IGraphicsBuffer> portalAperture;
        void* gridMapped = nullptr;
        void* skyboxMapped = nullptr;
        void* lightsMapped = nullptr;
        void* bonesMapped = nullptr;
        void* clusterHeadersMapped = nullptr;
        void* clusterIndicesMapped = nullptr;
        void* portalApertureMapped = nullptr;
        uint32_t boneCapacity = 0;
        uint32_t clusterHeaderCapacity = 0;
        uint32_t clusterIndexCapacity = 0;
    };
    std::unique_ptr<Engine::Renderers::UIRenderer> m_uiRenderer;
    Engine::Rendering::RealtimeLightingPipeline m_realtimeLightingPipeline;
    Engine::Rendering::BakedLightingPipeline m_bakedLightingPipeline;
    std::string m_lightingBakeStatus = "Lighting has not been baked.";

    void BuildGridPipeline();
    void BuildParticlePipeline();
    void RenderParticles(IGraphicsContext* context,
        const glm::mat4& viewProjection, const glm::mat4& view,
        const Object* contentScopeRoot = nullptr,
        const glm::vec4& clipPlane = glm::vec4(0.f),
        bool portalView = false);
    void BuildSkyboxPipeline();
    void BuildObjectPipeline();
    void EnsureObjectRenderCapacity(uint32_t requiredObjects);
    void EnsureSkinPaletteCapacity(uint32_t requiredMatrices);
    const Engine::Components::Texture* ResolveSkyboxTexture();
    void UpdateEnvironmentLighting(const Engine::Components::Texture* texture);
    std::shared_ptr<Engine::Rendering::EnvironmentMap> ResolveReflectionEnvironment(
        const Engine::Components::Material& material);

    struct FrameRenderItem
    {
        uint32_t objectSlot=0;
        bool previousLod=false;
        int gpuLod = -1, gpuMaximumLod = 0;
        glm::vec2 gpuLodDistances{0};
        Object* object = nullptr;
        Engine::Components::Mesh* mesh = nullptr;
        Engine::Components::Sprite* sprite = nullptr;
        Engine::Components::Material* material = nullptr;
        Engine::Components::SpatialManipulator* spatialManipulator = nullptr;
        Engine::Components::SkinnedMesh* skinnedMesh = nullptr;
        const Engine::Rendering::BakedLightingData* bakedLighting = nullptr;
        IGraphicsBuffer* spriteVertexBuffer = nullptr;
        const Engine::Components::Texture* spriteTexture = nullptr;
        // When a mesh crosses a nonlinear warp boundary, this buffer holds
        // the per-vertex mapped positions, normals and tangents.  It is a
        // renderer-owned view of the authored mesh, never the mesh itself.
        IGraphicsBuffer* warpedVertexBuffer = nullptr;
        glm::mat4 world{1.f};
        glm::mat4 authoredWorld{1.f};
        glm::vec3 boundsMinimum{0.f}, boundsMaximum{0.f};
        bool hasBounds = false;
        bool hasMorphTargets = false;
        bool hasSurfaceOffsets = false;
        bool castsShadows = true;
        bool terrainSurface = false;
        bool terrainCell = false;
        bool completeTerrainFootprint = false;
        // Non-zero only for a portal-split chart instance. Object.hlsl clips
        // against this world-space plane without modifying the mesh buffer.
        glm::vec4 traversalClipPlane{0.f};
        glm::vec4 geometryFade{0.f, 1.f, 0.f, 0.f};
        glm::vec2 shadowCasterFade{0.f,1.f};
        // The portal endpoint whose coordinate chart owns world and the clip
        // plane. Portal views remap only instances in their near-side chart.
        const Engine::Components::SpatialManipulator* traversalChartPortal = nullptr;
        glm::vec2 spriteWorldSize{1.f};
        glm::vec4 spriteUvRect{0.f, 0.f, 1.f, 1.f};
        uint32_t skinPaletteOffset = 0;
        uint32_t skinJointCount = 0;
        int sortingLayer = 0;
        bool belongsToPreview = false;
        bool blended = false;
    };

    std::vector<FrameRenderItem> m_frameRenderItems;
    // Compact reverse map from stable render-object slots to the frame items
    // that use them. Several LOD or portal items may share a slot.
    std::vector<uint32_t> m_frameItemsByRenderSlotOffsets;
    std::vector<uint32_t> m_frameItemsByRenderSlot;
    struct StaticRenderItemCache
    {
        FrameRenderItem item;
        uint64_t worldRevision = 0;
        uint64_t meshRevision = 0;
        uint64_t geometryRevision = 0;
        bool matrixLayerEnabled = false;
        glm::mat4 matrixLayer{1.f};
        const void* instanceData = nullptr;
        uint64_t instanceRevision = 0;
        const Engine::Components::Mesh* instanceSource = nullptr;
        uint64_t sourceGeometryRevision = 0;
    };
    std::unordered_map<Object*, StaticRenderItemCache> m_staticRenderItemCache;
    using RenderObjectKey=std::tuple<uintptr_t,int,uintptr_t,bool>;
    std::map<RenderObjectKey,uint32_t> m_renderObjectSlots;
    std::vector<uint32_t> m_freeRenderObjectSlots;
    uint32_t m_renderObjectSlotCount=0;
    uint64_t m_renderObjectSlotsRevision=UINT64_MAX;

    struct FrameComponentLookup
    {
        Object* object = nullptr;
        Engine::Components::Mesh* mesh = nullptr;
        Engine::Components::Sprite* sprite = nullptr;
        Engine::Components::Material* material = nullptr;
        Engine::Components::SpatialManipulator* spatialManipulator = nullptr;
        Engine::Components::SkinnedMesh* skinnedMesh = nullptr;
        const Engine::Rendering::BakedLightingData* bakedLighting = nullptr;
        // Resolved with the rest so per-frame item preparation avoids two
        // dynamic_cast scans per object.
        Engine::Components::Terrain* terrain = nullptr;
        const Engine::Components::TerrainBatchAppearance* terrainBatch = nullptr;
    };
    std::vector<FrameComponentLookup> m_frameComponentLookupScratch;
    struct OcclusionBufferResult
    {
        std::shared_ptr<const Engine::Rendering::OcclusionBuffer> buffer;
        bool reused = false;
        double jobMilliseconds = 0.0;
        uint64_t meshOccluders = 0;
    };
    // Rasterizes the frame's terrain (and optional mesh occluders) into a
    // software occlusion buffer for one camera view. Runs on a job.
    OcclusionBufferResult BuildCameraOcclusionBuffer(
        const std::vector<FrameRenderItem>& frameRenderItems, const glm::mat4& viewProjection,
        glm::vec2 projectionScale, float aspect);
    // Started by a camera's shadow pass, adopted by its color pass when the
    // view matches exactly. Holds a reference to the frame items: joined by
    // every color pass, by frame preparation and by FenceBackgroundWork.
    struct EarlyCameraOcclusion
    {
        std::future<OcclusionBufferResult> job;
        const void* camera = nullptr;
        const std::vector<FrameRenderItem>* items = nullptr;
        uint64_t serial = 0;
        uint32_t width = 0, height = 0;
        glm::mat4 viewProjection{ 1.f };
    } m_earlyCameraOcclusion;
    void JoinEarlyCameraOcclusion();
    Engine::Rendering::RenderSpatialIndex m_renderSpatialIndex;
    Engine::Rendering::RenderBoundsCache m_worldBoundsCache;
    // Spatial index positions follow persistent draw slots, not the compact
    // frame draw order, which changes during LOD fades and visibility changes.
    std::vector<Engine::Rendering::RenderBounds> m_spatialSlotBounds;
    std::vector<uint8_t> m_spatialSlotSeen;
    RenderCpuTimings m_lastRenderCpuTimings;
    Engine::Rendering::AutomaticInstanceCache m_automaticInstanceCache;
    std::array<Engine::Rendering::DrawBatchPreparation,2> m_drawBatchPreparation;
    Engine::Rendering::PersistentInstancePool m_persistentInstances;
    Engine::Rendering::PersistentInstanceBatches m_persistentBatches;
    Engine::Rendering::InstanceTilePreparation m_instanceTilePreparation;
    // Camera/view, geometry identity, placement, domain. Never shared with shadows.
    std::map<std::array<uint64_t,4>,std::pair<int,uint64_t>> m_screenLodHistory;
    std::array<uint64_t,3> m_screenLodCounts{};
    // Per instanced tile (InstanceData::lodIdentity): the range of its
    // placements' LOD scales (ScreenCoverage.h), so band skipping stays
    // conservative when scaled placements keep a band further out. Rebuilt
    // when the tile's revision changes; entries unused for a while expire.
    struct PlacementLodScaleRange
    {
        uint64_t revision = 0, serial = 0;
        float minimum = 1.f, maximum = 1.f;
    };
    std::unordered_map<uint64_t, PlacementLodScaleRange> m_placementLodScaleRanges;
    // Directional light indices cover every cluster and do not depend on
    // camera pose or light color. Buffer identities separate GPU frame slots.
    std::map<std::pair<const void*,const void*>,std::array<uint32_t,5>> m_directionalClusterLayouts;
    uint64_t m_instanceViewSerial=UINT64_MAX;
    std::map<std::pair<const void*,bool>,uint32_t> m_instanceViewUses;
    struct RenderPassBuffers
    {
        std::unique_ptr<IGraphicsBuffer> constants, objects, shadowConstants, shadowObjects;
        Engine::Rendering::UploadRecordCache objectCache,shadowCache;
        void* constantsMapped=nullptr; void* objectsMapped=nullptr;
        void* shadowConstantsMapped=nullptr; void* shadowObjectsMapped=nullptr;
        uint32_t capacity=0;
    };
    struct RenderPassBufferSlot
    {
        uint64_t serial=UINT64_MAX;
        size_t cursor=0;
        std::vector<std::unique_ptr<RenderPassBuffers>> passes;
    };
    // All dynamic scene uploads for one renderer frame slot share this owner.
    // The renderer only returns the slot after its completion fence retires.
    struct FrameUploadContext
    {
        uint64_t serial=UINT64_MAX;
        NativeFrameBuffers native;
        RenderPassBufferSlot renderPasses;
        uint64_t uploadedBytes=0;
        uint64_t bufferAllocations=0;
        uint64_t allocatorGrowths=0;
    };
    std::vector<std::unique_ptr<FrameUploadContext>> m_frameUploadContexts;
    FrameUploadContext* m_activeFrameUploadContext=nullptr;
    NativeFrameBuffers* m_activeNativeFrameBuffers = nullptr;
    RenderPassBuffers* m_activeRenderPassBuffers=nullptr;
    void SelectNativeFrameBuffers();
    void RecordFrameUploadBytes(uint64_t bytes)
    { if (m_activeFrameUploadContext) m_activeFrameUploadContext->uploadedBytes += bytes; }
    void SelectRenderPassBuffers(uint32_t requiredObjectSlots);
    struct CachedRenderMaterial
    {
        std::vector<uint8_t> data;
        std::array<const Engine::Graphics::IGraphicsTexture*,30> textures{};
        uint64_t revision=0;
        uint64_t frame=0;
        uint32_t alphaMode=0;
        bool doubleSided=false;
    };
    std::unordered_map<const Engine::Components::Material*,std::unordered_map<uint32_t,CachedRenderMaterial>> m_renderMaterialCache;
    uint64_t m_materialPreparationSerial=0, m_materialCacheStructureRevision=0;
    std::unique_ptr<Engine::Graphics::IPipelineState> m_instanceCullPipeline;
    bool m_instanceCullAttempted = false;
    // GPU grass blade generation (Shaders/GrassBlades/GrassBlades.hlsl).
    std::unique_ptr<Engine::Graphics::IPipelineState> m_grassBladePipeline;
    bool m_grassBladeAttempted = false;
    // Hardware simulation's synthetic GPU load (Shaders/GpuLoad).
    std::unique_ptr<Engine::Graphics::IPipelineState> m_gpuLoadPipeline;
    std::unique_ptr<IGraphicsBuffer> m_gpuLoadSeeds, m_gpuLoadSink;
    bool m_gpuLoadAttempted = false;
    bool m_frameLookupFull=true;
    std::unordered_set<Object*> m_frameLookupDirty;
    std::unordered_map<Object*,size_t> m_frameLookupIndices;
    std::vector<size_t> m_frameLookupFree;
    uint64_t m_frameLookupRevision = 0;
    bool m_frameLookupEditPose = false;
    std::vector<Engine::Components::SpatialManipulator*>
        m_frameSpatialManipulatorScratch;

    struct ViewRenderItem
    {
        const FrameRenderItem* source = nullptr;
        float cameraDistanceSquared = 0.f;
        float lightingDistanceSquared = 0.f;
        float worldDepth = 0.f;
        int sortingLayer = 0;
        bool blended = false;
    };
    std::vector<ViewRenderItem> m_viewRenderItemScratch;

    struct WarpedRenderMesh
    {
        std::unique_ptr<IGraphicsBuffer> vertexBuffer;
        std::vector<Engine::Model::AnimationVertex> vertices;
        glm::mat4 authoredWorld { 1.f };
        glm::vec3 mappedOrigin { 0.f };
        uint64_t meshRevision = 0;
        uint64_t warpRevision = 0;
        bool evaluated = false;
        bool affectedByWarp = false;
    };
    // Object ownership remains in m_objects; this cache only owns transient
    // GPU upload buffers. Entries are replaced when topology changes and are
    // pruned as objects leave the scene.
    std::unordered_map<const Object*, WarpedRenderMesh> m_warpedRenderMeshes;
    uint32_t m_frameLightCount = 0;
    Engine::Model::RealtimeShadowSettings m_realtimeShadowSettings;
    // Temporal anti-aliasing per camera: the previous frame's unjittered
    // view-projection (for reprojection), the jitter sequence position and
    // the viewport the history belongs to.
    struct TemporalViewState
    {
        glm::dmat4 previousViewProjection{ 1.0 };
        uint32_t frame = 0;
        uint32_t width = 0, height = 0;
        bool valid = false;
    };
    std::unordered_map<const void*, TemporalViewState> m_temporalViews;
    Engine::Model::DistanceLightingSettings m_distanceLightingSettings;
    // SetDetailResolutionScale.
    float m_detailResolutionScale = 1.f;
    uint64_t m_lastObjectDataUploadBytes = 0;
    uint32_t m_lastOcclusionCulledCount = 0;
    uint32_t m_lastTerrainOcclusionCulledCount = 0;
    uint32_t m_lastOrdinaryDrawCount = 0;
    uint64_t m_lastOrdinaryTriangleCount = 0;
    uint32_t m_lastParticlePortalDrawCount = 0;
    uint32_t m_lastSkinnedObjectCount = 0;
    ShadowDebugSnapshot m_shadowDebugSnapshot;
    ShadowCpuTiming m_lastShadowCpuTiming{};
    bool m_renderFramePrepared = false;
    std::vector<Engine::Rendering::VegetationBendCapsule> m_frameGrassCapsules;
    bool m_nativeRenderPacketTaken = false;
    std::shared_ptr<NativePacketRecycler> m_nativePacketRecycler;
    bool m_recordingNativeCommands = false;
    bool m_diagnosticOpaqueDepthEqual = false;
    bool m_diagnosticOpaqueDepthEqualReady = true;
    PrepareTimings m_lastPrepareTimings{};
    size_t m_prefabPreparationCursor = 0;
    // No object layer had prefab work left after the last frame's pass: the
    // next pass only needs to cycle the layers' periodic re-checks, on a
    // quarter of the budget.
    bool m_prefabPreparationSettled = false;
    const Engine::Rendering::SceneViewOverlays* m_viewOverlays = nullptr;
    PerformanceHeatMap m_performanceHeat;

    // Long-range terrain alone can contribute 289 patch meshes. Truncating
    // ordinary views at 64 draws left square background holes even though the
    // chunks and their GPU buffers had been generated successfully.
    static constexpr uint32_t kInitialObjectCapacity = 512;
    static constexpr uint32_t kInitialSkinnedObjectCapacity = 64;
    static constexpr uint32_t kMaxSpatialObjects = 64;
    // The editor diagnostic expands a linked 8-point portal pair into point
    // markers, boundary bars, correspondence bars, and plane normals. Keep
    // enough transient space for every logical spatial object; it is never
    // used by runtime portal rendering.
    static constexpr uint32_t kMaxSpatialVerticesPerObject = 1024;
    // Recorded DX12/Vulkan draws consume upload-buffer contents later, when
    // the command list executes. Every portal view therefore owns immutable
    // aperture, depth-reset, and connected-scene slots for the whole frame.
    static constexpr uint32_t kMaxPortalRenderViews = 64;
    // Sky constant ring: slot 0 main sky, 1..kMaxPortalRenderViews portal
    // skies, then the volumetric cloud pass.
    static constexpr uint32_t kCloudConstantSlot = kMaxPortalRenderViews + 1u;
    static constexpr uint32_t kMaxSpatialDebugDraws =
        kMaxSpatialObjects * kMaxSpatialVerticesPerObject / 24;
    static constexpr uint32_t kInitialBoneMatrixCapacity = kInitialSkinnedObjectCapacity * 256;
    static constexpr uint32_t kMaxLights =
        Engine::Model::MaxRealtimeLights;
    static constexpr uint32_t kCBStride = 256;
    uint32_t m_objectCapacity = kInitialObjectCapacity;
    uint32_t m_boneMatrixCapacity = kInitialBoneMatrixCapacity;
};
}

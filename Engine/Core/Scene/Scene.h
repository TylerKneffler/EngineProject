#pragma once
#include "Core/Rendering/RenderBoundsCache.h"
#include "Core/Rendering/DrawBatchPreparation.h"
#include "Core/Rendering/ShaderSourceCache.h"
#include "Core/Compoonents/TransformHierarchy.h"
#include "Core/Scene/ComponentRegistry.h"
#include "Core/Rendering/PersistentInstanceBatches.h"
#include "Core/Rendering/InstanceTilePreparation.h"
#include "Core/Compoonents/SpatialUpdate.h"
#include <glm/gtc/quaternion.hpp>
#include "Core/Object.h"
#include "Core/Graphics/IPipelineState.h"
#include "Core/Graphics/IGraphicsBuffer.h"
#include "Core/Rendering/Lighting/Pipelines/Realtime/RealtimeLightingPipeline.h"
#include "Core/Rendering/Lighting/Pipelines/Baked/BakedLightingPipeline.h"
#include "Core/Model/SceneSettings.h"
#include "Core/Model/MeshData.h"
#include "Core/Model/LightingData.h"
#include "Core/Model/UIData.h"
#include "Core/Time/SimulationClock.h"
#include "Core/Threading/JobScheduler.h"
#include "Core/Rendering/GrassPerformance.h"
#include "Core/Rendering/RenderPerformance.h"
#include "Core/Rendering/RenderSpatialIndex.h"
#include "Core/Rendering/AutomaticInstancing.h"
#include "Core/Physics/Physics.h"
#include "Core/Scene/Spatial/WarpVolume.h"
#include <glm/glm.hpp>
#include <array>
#include <atomic>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <stdexcept>
#include <unordered_map>
#include <any>
#include <typeindex>
#include <unordered_set>
#include <set>
#include <vector>

namespace Engine::Audio { class Audio; }
namespace Engine::Core { struct NativeInputSnapshot; }
namespace Engine::Components
{
    class Camera;
    class Texture;
    class Mesh;
    class Sprite;
    class Material;
    class SpatialManipulator;
    class SkinnedMesh;
    class ParticleEmitter;
}
namespace Engine::Rendering
{
    class BakedLightingData;
    class EnvironmentMap;
}
namespace Engine::Renderers { class UIRenderer; }
namespace Engine::Graphics
{
    class IGraphicsProvider;
    class IGraphicsContext;
    class GraphicsCommandPacket;
    class IGraphicsTexture;
    class IShader;
}

// Forward declarations
// ---------------------------------------------------------------------------
// Scene
//
// Owns all game objects plus the scene's Physics and Audio runtime state.
// Start()/Update() provide the common runtime path used by the standalone game
// and Editor Play mode; Init()/Render() manage API-neutral rendering resources.
//
// ---- Typical editor usage ----
//
//   auto graphicsProvider = renderer->GetGraphicsProvider();
//   scene.Init(graphicsProvider);
//
//   // inside SceneView drawFn:
//   scene.Render(graphicsContext);
// ---------------------------------------------------------------------------

namespace Engine::Scene
{
class Scene
{
public:
    using Object = Engine::Core::Object;
    using Camera = Engine::Components::Camera;
    using SceneSettings = Engine::Model::SceneSettings;
    using IGraphicsProvider = Engine::Graphics::IGraphicsProvider;
    using IGraphicsContext = Engine::Graphics::IGraphicsContext;
    using IPipelineState = Engine::Graphics::IPipelineState;
    using IGraphicsBuffer = Engine::Graphics::IGraphicsBuffer;

    using ObjectPath = std::vector<std::size_t>;
    using ObjectId = uint64_t;
    enum class ObjectPlacement { Before, AsChild, After };

    // Value-only view of the committed scene. A reader may retain a snapshot
    // after objects are removed; it must never dereference live scene state.
    struct SceneSnapshot
    {
        struct ComponentInfo
        {
            std::string type;
            uint64_t configurationRevision = 0;
            std::shared_ptr<const Engine::Serialization::JsonValue> authoredState;
        };
        struct ObjectInfo
        {
            ObjectId id = 0, parentId = 0;
            std::string name;
            bool enabled = true;
            glm::dvec3 position { 0.0 };
            glm::vec3 rotation { 0.f }, scale { 1.f };
            std::vector<ComponentInfo> components;
        };
        uint64_t generation = 0, structureRevision = 0;
        std::vector<ObjectInfo> objects;
    };
    struct NativeRenderPacket;

    // Every subsystem asks for spatial coordinates through this contract.
    // Physics bodies still store their stable Euclidean simulation chart;
    // Physics and raycast callers use this mapping when presenting/querying
    // nonlinear spaces rather than silently using the render-only transform.
    enum class SpatialQueryDomain : uint8_t
    {
        Rendering,
        Physics,
        Raycast,
        Audio,
        Camera,
        Gameplay
    };
    struct SpatialQuery
    {
        SpatialQueryDomain domain = SpatialQueryDomain::Gameplay;
        const Object* excludedOwner = nullptr;
        bool includeWarpVolumes = true;
    };
    struct SpatialQuerySample
    {
        glm::vec3 point { 0.f };
        glm::mat3 jacobian { 1.f };
        bool affectedByWarpVolume = false;
    };
    struct SpatialWarpSnapshot
    {
        struct Volume
        {
            const Object* owner = nullptr; // Identity only; never dereferenced by jobs.
            Spatial::WarpVolumeDefinition definition;
            glm::mat4 world { 1.f };
            int priority = 0;
            ObjectPath path;
        };
        std::vector<Volume> volumes;
        SpatialQuerySample Sample(const glm::vec3& worldPoint,
            const Object* excludedOwner = nullptr) const;
    };
    struct SpatialRay
    {
        glm::vec3 origin { 0.f };
        glm::vec3 direction { 0.f, 0.f, 1.f };
    };
    struct SpatialRayPathSettings
    {
        float curvatureTolerance = 0.01f;
        float maximumSourceStep = 1.f;
        uint32_t maximumSegments = 64u;
        uint32_t maximumSubdivisionDepth = 8u;
    };
    struct SpatialRayPathSegment
    {
        glm::vec3 start { 0.f };
        glm::vec3 end { 0.f };
        float sourceDistanceStart = 0.f;
        float sourceDistanceEnd = 0.f;
    };
    // A straight segment in one spatial chart.  A portal hit ends a segment;
    // the following segment starts at the connected endpoint in its chart.
    struct PortalRaySegment
    {
        SpatialRay ray;
        float maxDistance = 0.f;
        const Object* enteredPortal = nullptr;
        // Null means the legacy scene-wide chart. Otherwise geometry/query
        // candidates must belong to this root hierarchy.
        const Object* contentScopeRoot = nullptr;
    };
    struct PortalRaycastHit
    {
        Engine::Physics::Physics::RaycastHit physicsHit;
        float distance = 0.f;
        uint32_t portalHops = 0;
    };

    using PhysicsAdapterFactory = std::function<
        std::unique_ptr<Engine::Physics::IPhysicsAdapter>(Scene&)>;
    Scene();
    // Selects the scene's physics implementation after scene members exist.
    explicit Scene(PhysicsAdapterFactory physicsAdapterFactory);
    ~Scene();

    // Runtime lifecycle shared by the standalone game and Editor Play mode.
    void Start();
    bool HasStarted() const { return m_hasStarted; }
    void Update(float deltaTime);
    // Advance explicit edit-mode previews without running gameplay callbacks.
    // Live scene phases use the same owner-thread dependency scheduler as Update().
    bool AdvanceEditorPreviews(float deltaTime);
    bool UpdateSpatialScripts(bool editor=false) { return m_spatialUpdates.Tick(*this,editor); }
    const auto& GetSpatialUpdateScheduler() const { return m_spatialUpdates; }
    struct UpdateTimings
    {
        double objectsMs = 0.0;
        double poseMs = 0.0;
        double physicsMs = 0.0;
        double postPhysicsMs = 0.0;
        double constraintsMs = 0.0, audioMs = 0.0;
    };
    void SetUpdateProfilingEnabled(bool enabled) { m_updateProfilingEnabled = enabled; }
    const UpdateTimings& GetLastUpdateTimings() const { return m_lastUpdateTimings; }
    void ResetGrassPerformance() { m_grassPerformance = {}; m_grassGpuResources.clear(); }
    bool AccountGrassGpuResource(const void* resource)
    { return resource && m_grassGpuResources.insert(resource).second; }
    const Engine::Rendering::GrassPerformance& GetGrassPerformance() const
    { return m_grassPerformance; }
    void AddGrassPerformance(const Engine::Rendering::GrassPerformance& value)
    {
        m_grassPerformance.totalMs += value.totalMs;
        m_grassPerformance.scanMs += value.scanMs;
        m_grassPerformance.generationMs += value.generationMs;
        m_grassPerformance.uploadMs += value.uploadMs;
        m_grassPerformance.placementsScanned += value.placementsScanned;
        m_grassPerformance.tilesBuilt += value.tilesBuilt;
        m_grassPerformance.lodSwitches += value.lodSwitches;
        m_grassPerformance.pending += value.pending;
        m_grassPerformance.residentLods += value.residentLods;
        m_grassPerformance.gpuBytes += value.gpuBytes;
        m_grassPerformance.instances += value.instances;
        m_grassPerformance.instanceUploadBytes += value.instanceUploadBytes;
        m_grassPerformance.activeTiles+=value.activeTiles;
        m_grassPerformance.transitionTiles+=value.transitionTiles;
        m_grassPerformance.fadingTiles+=value.fadingTiles;
    }
    struct PrepareTimings
    {
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
    const PrepareTimings& GetLastPrepareTimings() const { return m_lastPrepareTimings; }
    using RenderCpuTimings = Engine::Rendering::RenderCpuTimings;
    const RenderCpuTimings& GetLastRenderCpuTimings() const { return m_lastRenderCpuTimings; }
    void ResetRenderCpuTimings() { m_lastRenderCpuTimings={}; }
    bool automaticInstancing = true;
    bool gpuInstanceCulling = true;
    // Re-evaluate animation-facing solvers after a manually sampled pose.
    void ApplyPoseConstraints();
    // Advances exactly one configured fixed frame. Offline exporters use this
    // instead of supplying or sampling wall-clock deltas.
    void UpdateFixedFrame();
    void SetFixedTimeStep(double seconds) { AssertOwnerThread(); m_clock.SetFixedStep(seconds); }
    void UseVariableTimeStep() { AssertOwnerThread(); m_clock.UseVariableStep(); }
    float GetDeltaTime() const { return m_deltaTime; }
    // CPU particle storage is capped across all emitters. Existing particles
    // keep their slots in scene object order; new spawns are dropped at budget.
    // The default matches the current particle renderer's 100k draw limit.
    void SetParticleBudget(size_t capacity);
    size_t GetParticleBudget() const { return m_particleBudget; }
    size_t GetParticleLiveCount() const;
    size_t GetParticleCapacityFor(
        const Engine::Components::ParticleEmitter* emitter) const;
    double GetElapsedTime() const { return m_clock.GetElapsedTime(); }
    uint64_t GetFrameIndex() const { return m_clock.GetFrameIndex(); }
    // Manual render loops can advance the shared streaming/upload frame
    // budget without running the scene's component and physics updates.
    void AdvanceManualFrame(float deltaTime)
    { AssertOwnerThread(); m_deltaTime = m_clock.Advance(deltaTime); }
    const Engine::Time::SimulationClock& GetClock() const { return m_clock; }
    Engine::Physics::Physics& GetPhysics() { return *m_physics; }
    const Engine::Physics::Physics& GetPhysics() const { return *m_physics; }
    Engine::Audio::Audio& GetAudio() { return *m_audio; }
    const Engine::Audio::Audio& GetAudio() const { return *m_audio; }

    // --- Editor Settings ---
    Object editorCamera; // not used by the game runtime, used for editor scene view navigation

    // Build GPU resources (grid vertex buffer, PSO).
    // Must be called once before the first Render() call.
    void Init(IGraphicsProvider* graphicsProvider);

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
    void QueueGraphicsPreparation()
    { AssertOwnerThread(); m_graphicsPreparationPending = true; }
    void ProcessGraphicsPreparation(IGraphicsProvider* graphicsProvider);
    std::shared_ptr<const NativeRenderPacket> TakeNativeRenderPacket();
    void RetainNativePacketMeshBuffers(const NativeRenderPacket& packet,
        Engine::Graphics::GraphicsCommandPacket& commands) const;
    // Native UI layout writes computed component rectangles on the scene owner.
    // The render worker reads this prepared layout until the frame completes.
    void PrepareNativeUiLayout(float viewportAspect);
    const std::vector<Engine::Model::UITextLayout>& GetPreparedNativeUiLayout() const
    { return m_preparedNativeUiLayout; }
    bool IsRecordingNativeCommands() const { return m_recordingNativeCommands; }
    // Record queued GPU particle steps before a Vulkan render pass begins.
    void RecordGpuParticleSimulation(IGraphicsContext* context);
    struct RetiredGraphicsResources
    {
        std::vector<std::unique_ptr<IGraphicsBuffer>> buffers;
        std::vector<std::shared_ptr<IGraphicsBuffer>> sharedBuffers;
        std::vector<std::unique_ptr<IPipelineState>> pipelines;
        std::vector<std::shared_ptr<Engine::Graphics::IGraphicsTexture>> textures;
        bool Empty() const
        { return buffers.empty() && sharedBuffers.empty() && pipelines.empty() && textures.empty(); }
    };
    // Called by the render owner after EndFrame while the simulation handoff
    // is blocked. The caller keeps these resources until the GPU frame fence.
    RetiredGraphicsResources TakeRetiredGraphicsResources();
    // Mesh components can replace buffers while their object remains live.
    // Native frame handoff serializes these calls with TakeRetiredGraphicsResources.
    void RetireGraphicsBuffer(std::unique_ptr<IGraphicsBuffer> buffer);
    void RetireGraphicsBuffer(std::shared_ptr<IGraphicsBuffer> buffer);
    void SetDeferredGraphicsRetirement(bool enabled)
    { AssertOwnerThread(); m_deferGraphicsRetirement = enabled; }
    bool IsDeferredGraphicsRetirementEnabled() const
    { AssertOwnerThread(); return m_deferGraphicsRetirement; }
    void SetBeforeGpuResourceDestruction(std::function<void()> waitForGpu)
    { AssertOwnerThread(); m_beforeGpuResourceDestruction = std::move(waitForGpu); }
    void WaitForGpuResourceDestruction()
    { AssertOwnerThread(); if (m_beforeGpuResourceDestruction) m_beforeGpuResourceDestruction(); }
    void Render(IGraphicsContext* context, float aspect,
        Camera* cameraOverride = nullptr, bool includeEditorVisuals = true,
          uint32_t viewportWidth = 0u, uint32_t viewportHeight = 0u,
          bool shadowOnly = false,
          const NativeRenderPacket* nativePacket = nullptr);
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
    { AssertOwnerThread(); m_lastOcclusionCulledCount = count; }
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
    };
    const ShadowCpuTiming& GetLastShadowCpuTiming() const
    {
        return m_lastShadowCpuTiming;
    }
    void SetSelectedObject(Object* obj) { m_selectedObject = obj; }
    Object* GetSelectedObject() const { return m_selectedObject; }
    void SetEditorSelectedMesh(Engine::Components::Mesh* mesh)
    { m_editorSelectedMesh = mesh; }
    Engine::Components::Mesh* GetEditorSelectedMesh() const
    { return m_editorSelectedMesh; }
    void SetEditorMeshEditPose(bool enabled)
    {
        if (m_editorMeshEditPose == enabled) return;
        m_editorMeshEditPose = enabled;
        m_renderFramePrepared = false;
    }
    bool IsEditorMeshEditPose() const { return m_editorMeshEditPose; }
    // Viewport preference only; leave individual rig visibility and saved assets intact.
    void SetEditorBonesVisible(bool visible) { m_editorBonesVisible = visible; }
    bool AreEditorBonesVisible() const { return m_editorBonesVisible; }
    void SetEditorWeightPaint(Engine::Components::Mesh* mesh, int bone)
    {
        if (m_editorWeightPaintMesh == mesh && m_editorWeightPaintBone == bone)
            return;
        m_editorWeightPaintMesh = mesh;
        m_editorWeightPaintBone = bone;
        m_renderFramePrepared = false;
    }
    void SetPreviewObject(Object* obj) { m_previewObject = obj; }
    void SetEditorMode2D(bool enabled);
    bool IsEditorMode2D() const { return m_editorMode2D; }
    void SetDistanceLightingSettings(
        const Engine::Model::DistanceLightingSettings& distanceSettings)
    {
        m_distanceLightingSettings = distanceSettings;
    }
    void SetRealtimeShadowSettings(
        const Engine::Model::RealtimeShadowSettings& shadowSettings)
    {
        m_realtimeShadowSettings = shadowSettings;
    }
    // Pointer coordinates relative to the surface displaying the game render
    // target, used to keep embedded-view UI hit bounds aligned.
    void SetUiPointerInput(float x, float y, float viewportWidth,
        float viewportHeight, bool hovered, bool mouseDown);
    void CaptureEmbeddedUiInput(Engine::Core::NativeInputSnapshot& input) const;

    // Embedded gameplay must acquire input from its viewport, rather than
    // treating every editor panel as part of the game window.
    void SetGameplayInputEnabled(bool enabled)
    {
        m_gameplayInputManaged = true;
        m_gameplayInputEnabled = enabled;
    }
    bool IsGameplayInputEnabled() const { return m_gameplayInputEnabled; }
    bool IsGameplayInputManaged() const { return m_gameplayInputManaged; }
    void SetGameplayInputAnchor(const glm::vec2& anchor) { m_gameplayInputAnchor = anchor; }
    const glm::vec2& GetGameplayInputAnchor() const { return m_gameplayInputAnchor; }

    // Returns the first active Camera component found on a scene game object.
    // The editor camera is deliberately excluded so GameView cannot silently
    // render from the Scene view's navigation camera.
    Camera* FindGameCamera();

    // Maps the ordinary Euclidean scene chart through active spatial volumes.
    // The sample Jacobian carries nonlinear orientation/scale to consumers.
    SpatialQuerySample SampleSpatialPoint(const glm::vec3& worldPoint,
        const SpatialQuery& query = {}) const;
    SpatialWarpSnapshot CaptureSpatialWarpSnapshot() const;
    glm::vec3 MapSpatialPoint(const glm::vec3& worldPoint,
        const SpatialQuery& query = {}) const;
    // Numerically inverts the fully composed spatial mapping. This is local:
    // callers should use it only where the active charts are nonsingular and
    // do not fold multiple source points onto the same mapped point.
    bool TryUnmapSpatialPoint(const glm::vec3& mappedPoint,
        glm::vec3& worldPoint, const SpatialQuery& query = {}) const;
    glm::mat4 MapSpatialMatrix(const glm::mat4& worldMatrix,
        const SpatialQuery& query = {}) const;
    SpatialRay MapSpatialRay(const SpatialRay& ray,
        const SpatialQuery& query = {}) const;
    std::vector<SpatialRayPathSegment> TraceSpatialRayPath(
        const SpatialRay& ray, float maxDistance,
        const SpatialQuery& query = { SpatialQueryDomain::Raycast },
        const SpatialRayPathSettings& settings = {}) const;
    // Trace a finite physical-space ray across portal apertures.  This is the
    // common primitive for gameplay queries and editor picking; callers test
    // ordinary geometry against each returned segment in order.  Nonlinear
    // volume integration remains a separate concern from discrete portals.
    std::vector<PortalRaySegment> TracePortalRay(const SpatialRay& ray,
        float maxDistance, uint32_t maxPortalHops = 8u) const;
    bool RaycastPortals(const SpatialRay& ray, float maxDistance,
        PortalRaycastHit& hit, uint32_t maxPortalHops = 8u,
        uint32_t collisionMask = ~0u) const;
    static bool IsObjectInSpatialRegion(const Object* object,
        const Object* contentScopeRoot);

    // Compatibility names for existing gameplay code. New code should state
    // its spatial intent with MapSpatialPoint/MapSpatialMatrix.
    glm::vec3 WarpWorldPoint(const glm::vec3& worldPoint,
        const Object* excludedOwner = nullptr) const;
    glm::mat4 WarpWorldMatrix(const glm::mat4& worldMatrix,
        const Object* excludedOwner = nullptr) const;

    // Move the editor camera to frame the given object, keeping a comfortable
    // viewing distance and looking directly at its world-space origin.
    // Pass nullptr to reset to the default startup position.
    void FocusEditorCamera(Object* obj);
    void UpdateEditorCameraFollow();
    void StopEditorCameraFollow();

    // Object management
    // Legacy editor and script callbacks run on the scene owner. Their direct
    // public-field edits form a serial compatibility path and are published
    // at the next update or render-preparation commit. Workers use the queue.
    void AssertOwnerThread() const
    {
        if (std::this_thread::get_id() != m_ownerThread)
            throw std::logic_error("Live scene mutation requires the scene owner thread");
    }
    bool IsOwnerThread() const
    { return std::this_thread::get_id() == m_ownerThread; }
    void FenceBackgroundWork();
    void WaitForSceneJobs();
    // Call only while the current owner is idle and no scene readers are active.
    // The native game transfers at startup/shutdown; editor Play transfers at
    // recorded-frame boundaries, with live UI excluded until the tick joins.
    void TransferOwnerTo(std::thread::id nextOwner)
    {
        AssertOwnerThread();
        if (nextOwner == std::thread::id{})
            throw std::logic_error("Scene owner thread cannot be empty");
        m_ownerThread = nextOwner;
    }
    Object* AddObject();                     // create an empty Object owned by this scene
    Object* AddObject(const std::string& name);
    // Includes objects queued for an end-of-frame runtime spawn.
    Object* FindObjectByName(const std::string& name);
    const Object* FindObjectByName(const std::string& name) const;
    // Safe from component callbacks: destruction is committed after the
    // current scene update finishes, so the calling component remains valid
    // until its Update() returns.
    void    RequestRemoveObject(Object* obj);
    void    RemoveObject(Object* obj);
    // Edit previews queue removals while traversing objects, just like scripts.
    // Call only after that traversal; runtime Update commits automatically.
    void CommitPendingObjectChanges();
    // A worker must carry the generation captured when its scene work was
    // scheduled. The owner fences tracked jobs before replacing or destroying
    // objects, so no callback from a reloadable module survives that boundary.
    using SceneGeneration = uint64_t;
    SceneGeneration CaptureSceneGeneration() const
    { return m_sceneGeneration.load(std::memory_order_acquire); }
    Engine::Threading::JobScheduler::Handle ScheduleSceneJob(
        std::function<void(SceneGeneration)> work);
    void FenceSceneGeneration();
    std::shared_ptr<const SceneSnapshot> GetPublishedSceneSnapshot() const
    { return std::atomic_load_explicit(&m_publishedSceneSnapshot, std::memory_order_acquire); }
    double GetLastSceneSnapshotPublishMs() const { return m_lastSnapshotPublishMs; }
    // Workers pass the generation captured when their job was scheduled.
    // Commands execute in FIFO order on the owner at the next commit boundary;
    // commands from an earlier Play, scene, or module generation are dropped.
    ObjectId QueueAddObject(SceneGeneration generation, std::string name);
    ObjectId QueueAddObject(std::string name)
    { AssertOwnerThread(); return QueueAddObject(CaptureSceneGeneration(), std::move(name)); }
    void QueueRemoveObject(SceneGeneration generation, ObjectId id);
    void QueueRemoveObject(ObjectId id)
    { AssertOwnerThread(); QueueRemoveObject(CaptureSceneGeneration(), id); }
    void QueueMoveObject(SceneGeneration generation, ObjectId id,
        ObjectId target, ObjectPlacement placement);
    void QueueMoveObject(ObjectId id, ObjectId target, ObjectPlacement placement)
    { AssertOwnerThread(); QueueMoveObject(CaptureSceneGeneration(), id, target, placement); }
    // Field writes may change name, enabled state, or local transform. Use
    // QueueMoveObject for hierarchy edits and QueueAddComponent for structure.
    void QueueObjectFieldWrite(SceneGeneration generation, ObjectId id,
        std::function<void(Object&)> write);
    void QueueObjectFieldWrite(ObjectId id, std::function<void(Object&)> write)
    { AssertOwnerThread(); QueueObjectFieldWrite(CaptureSceneGeneration(), id, std::move(write)); }
    template<typename T> void QueueAddComponent(SceneGeneration generation, ObjectId id)
    {
        QueueSceneMutation(generation, [id](Scene& scene)
        {
            if (auto* object = scene.FindObjectById(id))
                object->AddComponent<T>();
        });
    }
    template<typename T> void QueueAddComponent(ObjectId id)
    { AssertOwnerThread(); QueueAddComponent<T>(CaptureSceneGeneration(), id); }
    template<typename T> void QueueComponentWrite(SceneGeneration generation,
        ObjectId id, std::function<void(T&)> write)
    {
        QueueSceneMutation(generation, [id, write = std::move(write)](Scene& scene)
        {
            if (auto* object = scene.FindObjectById(id))
                if (auto* component = object->GetComponent<T>())
                {
                    write(*component);
                    component->MarkConfigurationDirty();
                }
        });
    }
    template<typename T> void QueueComponentWrite(ObjectId id, std::function<void(T&)> write)
    { AssertOwnerThread(); QueueComponentWrite<T>(CaptureSceneGeneration(), id, std::move(write)); }
    // Live object lookup is scene-owner only. Workers use IDs in the snapshot.
    ObjectId GetObjectId(const Object* object) const;
    Object* FindObjectById(ObjectId id) const;
    void    ClearObjects();                  // remove all objects and reset selection
    const std::vector<std::unique_ptr<Object>>& GetObjects() const { return m_objects; }
    bool TryGetObjectPath(const Object* object, ObjectPath& path) const;
    Object* FindObjectByPath(const ObjectPath& path) const;
    bool MoveObject(Object* object, Object* target, ObjectPlacement placement);
    // Serial owner path for runtime hierarchy edits that preserve local TRS.
    // The result is published at the next update/render-preparation boundary.
    bool SetParentLocal(Object* object, Object* parent);
    // Invalidates caches that retain object or component pointers. Object and
    // component creation call this automatically; hierarchy editors should
    // use MoveObject so cached animation bindings stay coherent.
    uint64_t GetStructureRevision() const { return m_structureRevision; }
    template<typename T> const std::vector<T*>& GetComponents() const
    { return m_componentRegistry.Get<T>(m_objects); }
    template<class T> uint64_t GetComponentRevision() const { return m_componentRegistry.Revision<T>(m_objects); }
    uint64_t GetPhysicsActivityRevision() const { return m_physicsActivityRevision; }
    void NotifyPhysicsActivityChanged() { ++m_physicsActivityRevision; }
    bool IsCommittedObject(Object* owner) const { return m_componentRegistry.Contains(owner); }
    void NotifyStructureChanged(Object* owner=nullptr,bool removed=false)
    {
        AssertOwnerThread();
        if (++m_structureRevision == 0) ++m_structureRevision;
        if(!owner) { m_componentRegistry.Reconcile(m_objects); m_frameLookupFull=true; m_updateRegistryFull=true; }
        else
        {
            if(removed) m_componentRegistry.Remove(owner); else m_componentRegistry.Changed(owner);
            m_frameLookupDirty.insert(owner); m_updateRegistryDirty.insert(owner);
        }
    }
    void NotifyComponentRemoved(Engine::Core::Component* component)
    {
        AssertOwnerThread();
        m_componentSnapshotCache.erase(component);
        m_componentRegistry.Remove(component);
        if(++m_structureRevision==0) ++m_structureRevision;
        if(component && component->Owner)
        { m_frameLookupDirty.insert(component->Owner); m_updateRegistryDirty.insert(component->Owner); }
    }

    // Serialization — delegates to SceneSerializer.
    // Save writes the scene to a scene XML file.
    // Load clears the scene and repopulates it from the file.  Mesh GPU
    // buffers are created automatically using the graphics provider stored by Init().
    bool Save(const std::string& path) const;
    bool Load(const std::string& path);
    std::string SaveToString() const;
    std::string SaveSnapshotToString() const;
    // Editor Play restore: skips serializing the runtime graph for rollback.
    bool RestoreTrustedSnapshot(const Engine::Serialization::JsonValue& snapshot);
    bool LoadFromString(const std::string& source);
    bool LoadFromValue(const Engine::Serialization::JsonValue& document);

    Engine::Model::BakeResult BakeLighting(
        const std::string& assetsDirectory = "Assets",
        const std::string& sceneName = "Scene",
        const Engine::Model::BakedLightingSettings& bakeSettings = {},
        const std::function<void(float)>& progress = {},
        const std::function<bool()>& cancelled = {});
    void ClearBakedLighting();
    void SetLightingBakeStatus(std::string status)
    { AssertOwnerThread(); m_lightingBakeStatus = std::move(status); }
    const std::string& GetLightingBakeStatus() const { return m_lightingBakeStatus; }

    SceneSettings settings;

    IGraphicsProvider* GetGraphicsProvider() const { return m_graphicsProvider; }
    const Engine::Components::Texture* GetSkyboxPreviewTexture();

private:
    bool m_gameplayInputManaged = false;
    bool m_gameplayInputEnabled = true;
    glm::vec2 m_gameplayInputAnchor { 0.f };
    // ---- Rendering resources (kept API-agnostic) ----
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
    };
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
    std::unique_ptr<IGraphicsBuffer> m_skyboxConstantBuffer;
    void* m_skyboxCBMapped = nullptr;
    std::shared_ptr<Engine::Components::Texture> m_defaultSkyboxTexture;
    std::shared_ptr<Engine::Components::Texture> m_sceneSkyboxTexture;
    std::string m_loadedSkyboxPath;
    std::string m_environmentLightingPath;
    std::array<glm::vec4, 9> m_environmentSH{};
    std::shared_ptr<Engine::Rendering::EnvironmentMap> m_environmentMap;

    std::unique_ptr<IPipelineState> m_objectPipeline;
    std::unique_ptr<IPipelineState> m_objectDoubleSidedPipeline;
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
    MaterialShaderFuture QueueMaterialShaderBytecode(const std::string& path,
        bool terrain, int64_t stamp);
    Engine::Rendering::ShaderSourceCache m_materialShaderSources;
    const Engine::Rendering::ShaderSourceCache::Source& ResolveMaterialShaderSource(
        const Engine::Components::Material& material, const std::string& path);
    // Deduplicate prewarm requests per frame; source resolution survives frames.
    std::set<std::tuple<std::string, std::string, bool>> m_preparedMaterialShaderSources;
    void PrewarmMaterialShaders(const Engine::Components::Material& material,
        bool terrain);
    struct MaterialShaderPipeline
    {
        std::unique_ptr<IPipelineState> pipeline;
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
    uint64_t m_directionalShadowAllocationGeneration = 0;
    bool m_directionalShadowCacheValid = false;
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
    std::vector<std::unique_ptr<IGraphicsBuffer>> m_retiredRenderBuffers;
    std::vector<std::shared_ptr<IGraphicsBuffer>> m_retiredSharedRenderBuffers;
    bool m_deferGraphicsRetirement = false;
    std::function<void()> m_beforeGpuResourceDestruction;
    std::unique_ptr<IGraphicsBuffer> m_emptyMorphDeltaBuffer;
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
    std::vector<std::unique_ptr<NativeFrameBuffers>> m_nativeFrameBuffers;
    NativeFrameBuffers* m_activeNativeFrameBuffers = nullptr;
    void SelectNativeFrameBuffers();
    std::unique_ptr<Engine::Renderers::UIRenderer> m_uiRenderer;
    std::vector<Engine::Model::UITextLayout> m_preparedNativeUiLayout;
    std::unique_ptr<Engine::Physics::Physics> m_physics;
    std::unique_ptr<Engine::Audio::Audio> m_audio;

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
    };
    std::vector<FrameComponentLookup> m_frameComponentLookupScratch;
    Engine::Rendering::RenderSpatialIndex m_renderSpatialIndex;
    Engine::Rendering::RenderBoundsCache m_worldBoundsCache;
    // Spatial index positions follow persistent draw slots, not the compact
    // frame draw order, which changes during LOD fades and visibility changes.
    std::vector<Engine::Rendering::RenderBounds> m_spatialSlotBounds;
    std::vector<uint8_t> m_spatialSlotSeen;
    Engine::Components::TransformHierarchy m_transformHierarchy;
    uint64_t m_transformPreparationRevision=UINT64_MAX;
    std::vector<Engine::Components::Transform*> m_transformPreparationScratch;
    RenderCpuTimings m_lastRenderCpuTimings;
    Engine::Rendering::AutomaticInstanceCache m_automaticInstanceCache;
    std::array<Engine::Rendering::DrawBatchPreparation,2> m_drawBatchPreparation;
    Engine::Rendering::PersistentInstancePool m_persistentInstances;
    Engine::Rendering::PersistentInstanceBatches m_persistentBatches;
    Engine::Rendering::InstanceTilePreparation m_instanceTilePreparation;
    // Directional light indices cover every cluster and do not depend on
    // camera pose or light color. Buffer identities separate GPU frame slots.
    std::map<std::pair<const void*,const void*>,std::array<uint32_t,5>> m_directionalClusterLayouts;
    uint64_t m_instanceViewSerial=UINT64_MAX;
    std::map<std::pair<const void*,bool>,uint32_t> m_instanceViewUses;
    struct RenderPassBuffers
    {
        std::unique_ptr<IGraphicsBuffer> constants, objects, shadowConstants, shadowObjects;
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
    std::vector<RenderPassBufferSlot> m_renderPassBufferSlots;
    RenderPassBuffers* m_activeRenderPassBuffers=nullptr;
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
    bool m_frameLookupFull=true,m_updateRegistryFull=true;
    std::unordered_set<Object*> m_frameLookupDirty,m_updateRegistryDirty;
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
    Engine::Model::DistanceLightingSettings m_distanceLightingSettings;
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
    bool m_nativeRenderPacketTaken = false;
    bool m_recordingNativeCommands = false;

    // ---- Object list ----
    std::vector<std::unique_ptr<Object>> m_objects;
    std::thread::id m_ownerThread = std::this_thread::get_id();
    std::vector<std::unique_ptr<Object>> m_pendingObjectAdditions;
    std::vector<Object*> m_pendingObjectRemovals;
    std::atomic<ObjectId> m_nextObjectId { 1 };
    std::unordered_map<const Object*, ObjectId> m_objectIds;
    std::unordered_map<ObjectId, Object*> m_objectsById;
    struct CachedComponentSnapshot
    {
        uint64_t revision = 0;
        std::shared_ptr<const Engine::Serialization::JsonValue> state;
    };
    std::unordered_map<const Engine::Core::Component*, CachedComponentSnapshot>
        m_componentSnapshotCache;
    mutable std::mutex m_queuedSceneMutationsMutex;
    struct QueuedSceneMutation
    {
        SceneGeneration generation;
        std::function<void(Scene&)> apply;
    };
    std::vector<QueuedSceneMutation> m_queuedSceneMutations;
    std::atomic<SceneGeneration> m_sceneGeneration{1};
    std::vector<Engine::Threading::JobScheduler::Handle> m_sceneJobs;
    std::shared_ptr<const SceneSnapshot> m_publishedSceneSnapshot;
    bool m_suppressSceneSnapshotPublication = false;
    uint64_t m_snapshotGeneration = 0;
    double m_lastSnapshotPublishMs = 0;
    bool m_isCommitting = false;
    bool m_isFlushingRemovals = false;
    bool QueueSceneMutation(SceneGeneration generation,
        std::function<void(Scene&)> mutation);
    void PublishSceneSnapshot();
    bool m_isUpdating = false;
    bool m_updateProfilingEnabled = false;
    UpdateTimings m_lastUpdateTimings{};
    Engine::Rendering::GrassPerformance m_grassPerformance{};
    std::unordered_set<const void*> m_grassGpuResources;
    PrepareTimings m_lastPrepareTimings{};
    bool m_hasStarted = false;
    bool m_graphicsPreparationPending = false;
    uint64_t m_lifecycleRevision = 0;
    size_t m_particleBudget = 100000;
    float m_deltaTime = 0.f;
    Engine::Time::SimulationClock m_clock;
    uint64_t m_structureRevision = 1;
    mutable Engine::Scene::ComponentRegistry m_componentRegistry;
    uint64_t m_physicsActivityRevision=0;
    uint64_t m_updateRegistryRevision = 0;
    std::vector<Object*> m_updateObjects, m_poseObjects;
    Engine::Components::SpatialUpdateScheduler m_spatialUpdates;
    void RefreshUpdateRegistry();
    Object* AddObjectWithId(ObjectId id);
    void FlushPendingObjectAdditions();
    void FlushPendingObjectRemovals();
    void EnforceParticleBudget();
    Object* m_selectedObject = nullptr;
    Engine::Components::Mesh* m_editorSelectedMesh = nullptr;
    bool m_editorMeshEditPose = false;
    bool m_editorBonesVisible = true;
    Engine::Components::Mesh* m_editorWeightPaintMesh = nullptr;
    int m_editorWeightPaintBone = -1;
    Object* m_previewObject = nullptr;
    bool m_editorMode2D = false;
    bool m_editorCameraModeInitialized = false;
    Object* m_editorCameraFollowTarget = nullptr;
    glm::vec3 m_editorCameraFollowPosition { 0.f };
    glm::quat m_editorCameraFollowRotation { 1.f, 0.f, 0.f, 0.f };

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

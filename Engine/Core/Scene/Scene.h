#pragma once
#include "Core/Components/TransformHierarchy.h"
#include "Core/Scene/ComponentRegistry.h"
#include "Core/Components/SpatialUpdate.h"
#include <glm/gtc/quaternion.hpp>
#include "Core/Scene/Object.h"
#include "Core/Graphics/IPipelineState.h"
#include "Core/Graphics/IGraphicsBuffer.h"
#include "Core/Model/SceneSettings.h"
#include "Core/Model/MeshData.h"
#include "Core/Model/LightingData.h"
#include "Core/Model/UIData.h"
#include "Core/Time/SimulationClock.h"
#include "Core/Threading/JobScheduler.h"
#include "Core/Physics/Physics.h"
#include "Core/Scene/Spatial/WarpVolume.h"
#include "Core/Scene/SceneObserver.h"
#include "Core/Scene/SceneCounters.h"
#include <glm/glm.hpp>
#include <array>
#include <atomic>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
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
    class Terrain;
    class TerrainBatchAppearance;
}
namespace Engine::Graphics
{
    class IGraphicsProvider;
    class IGraphicsContext;
    class GraphicsCommandPacket;
    class IGraphicsTexture;
    class IShader;
}

// ---------------------------------------------------------------------------
// Scene
//
// Owns object memory: game objects, their hierarchy and identity, the update
// index, structure events and snapshots, plus the physics world. Game drives
// the lifecycle; rendering is not part of the scene.
//
// Engine::Rendering::RenderSystem owns one SceneRenderer per scene, created on
// first use and released when the scene is destroyed. The renderer reads the
// scene and follows its changes through ISceneObserver:
//
//   scene.Init(graphicsProvider);   // provider for component GPU resources
//   auto& renderer = Engine::Rendering::RenderSystem::For(scene);
//   renderer.Render(graphicsContext, aspect);
// ---------------------------------------------------------------------------

namespace Engine::Runtime { class Game; }

namespace Engine::Rendering { class SceneRenderer; }

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

    // ---- Lifecycle ------------------------------------------------------------
    // Engine::Runtime::Game drives the lifecycle (Start/Update/fixed step) and
    // owns the simulation clock. A scene with no Game bound (editor edit mode,
    // asset previews, importers) is never ticked; its time reads return zero.
    Engine::Runtime::Game* GetGame() const { return m_game; }
    // Called by Game on construction/destruction only.
    void BindGame(Engine::Runtime::Game* game) { m_game = game; }
    // True once Game::Start ran; objects added afterwards start on commit.
    bool HasStarted() const { return m_hasStarted; }
    void MarkStarted() { m_hasStarted = true; }

    // ---- Simulation-step support (used by Game::Update) ----------------------
    // Marks the update pass: structural edits are deferred to the next commit.
    void BeginUpdatePass();
    void EndUpdatePass() { m_isUpdating = false; }
    bool IsInUpdatePass() const { return m_isUpdating; }
    // Objects that have components needing Update / pose evaluation, refreshed
    // from the structure revision.
    const std::vector<Object*>& GetUpdateObjects();
    void PrepareTransformHierarchy();
    // Whether the per-frame transform passes snapshot this object. Static
    // leaves with no components (spline control points: 4.4k on the
    // landscape's trails) are left to the lazy transform path.
    static bool PreparesTransform(const Object& object);
    void ClearPreparedNativeUiLayout() { m_preparedNativeUiLayout.clear(); }

    bool UpdateSpatialScripts(bool editor=false) { return m_spatialUpdates.Tick(*this,editor); }
    const auto& GetSpatialUpdateScheduler() const { return m_spatialUpdates; }
    // Per-frame diagnostic counters published by scripts and plugins.
    SceneCounters& GetCounters() { return m_counters; }
    const SceneCounters& GetCounters() const { return m_counters; }
    // Diagnostics flag shared by Game update timings and render timings.
    void SetUpdateProfilingEnabled(bool enabled) { m_updateProfilingEnabled = enabled; }
    bool IsUpdateProfilingEnabled() const { return m_updateProfilingEnabled; }
    // Re-evaluate animation-facing solvers after a manually sampled pose.
    // Evaluates pose/constraint components on the pose index. Game::Update runs
    // it each frame; editor pose tools call it directly in edit mode.
    void ApplyPoseConstraints();
    // ---- Time (read-through to the bound Game's clock; zero without one) ------
    float GetDeltaTime() const;
    // CPU particle storage is capped across all emitters. Existing particles
    // keep their slots in scene object order; new spawns are dropped at budget.
    // The default matches the current particle renderer's 100k draw limit.
    void SetParticleBudget(size_t capacity);
    // Trims emitters to the budget in scene order (Game runs it each update).
    void EnforceParticleBudget();
    size_t GetParticleBudget() const { return m_particleBudget; }
    size_t GetParticleLiveCount() const;
    size_t GetParticleCapacityFor(
        const Engine::Components::ParticleEmitter* emitter) const;
    double GetElapsedTime() const { return GetClock().GetElapsedTime(); }
    uint64_t GetFrameIndex() const { return GetClock().GetFrameIndex(); }
    const Engine::Time::SimulationClock& GetClock() const;
    Engine::Physics::Physics& GetPhysics() { return *m_physics; }
    const Engine::Physics::Physics& GetPhysics() const { return *m_physics; }

    // --- Editor Settings ---
    // Fallback viewer: rendered from when no game camera is active, the audio
    // listener fallback, and the editor's navigation camera. Moves out of Scene
    // when the renderer takes its view camera as input (refactor plan 4.4).
    Object editorCamera;

    // Stores the graphics provider components use for their GPU resources
    // and configures the fallback camera. Call before rendering the scene.
    void Init(IGraphicsProvider* graphicsProvider);

    void QueueGraphicsPreparation(double budgetMilliseconds = 0)
    {
        AssertOwnerThread();
        m_graphicsPreparationPending = true;
        m_graphicsPreparationRestart = true;
        if (budgetMilliseconds > 0) m_graphicsPreparationBudgetMs = budgetMilliseconds;
    }
    bool HasPendingGraphicsPreparation() const { return m_graphicsPreparationPending; }
    void ProcessGraphicsPreparation(IGraphicsProvider* graphicsProvider);
    // Native UI layout writes computed component rectangles on the scene owner.
    // The render worker reads this prepared layout until the frame completes.
    void PrepareNativeUiLayout(float viewportAspect);
    const std::vector<Engine::Model::UITextLayout>& GetPreparedNativeUiLayout() const
    { return m_preparedNativeUiLayout; }
    struct RetiredGraphicsResources
    {
        // Detached built-in render-only leaves retain their full mesh/texture
        // ownership graph until the submission fence completes.
        std::vector<std::unique_ptr<Object>> objects;
        std::vector<std::unique_ptr<IGraphicsBuffer>> buffers;
        std::vector<std::shared_ptr<IGraphicsBuffer>> sharedBuffers;
        std::vector<std::unique_ptr<IPipelineState>> pipelines;
        std::vector<std::shared_ptr<Engine::Graphics::IGraphicsTexture>> textures;
        bool Empty() const
        { return objects.empty() && buffers.empty() && sharedBuffers.empty() && pipelines.empty() && textures.empty(); }
    };
    // Called by the render owner after EndFrame while the simulation handoff
    // is blocked. The caller keeps these resources until the GPU frame fence.
    RetiredGraphicsResources TakeRetiredGraphicsResources();
    // Mesh components can replace buffers while their object remains live.
    // Native frame handoff serializes these calls with TakeRetiredGraphicsResources.
    void RetireGraphicsBuffer(std::unique_ptr<IGraphicsBuffer> buffer);
    void RetireGraphicsBuffer(std::shared_ptr<IGraphicsBuffer> buffer);
    // An object that is not in the scene and only carries render resources a
    // submitted frame may still draw (a recycled grass tile's old meshes). It
    // is released with the retired render leaves, behind the frame fence;
    // without deferred retirement the GPU is waited for first, as for a
    // removed object.
    void RetireRenderObject(std::unique_ptr<Object> detached);
    void SetDeferredGraphicsRetirement(bool enabled)
    { AssertOwnerThread(); m_deferGraphicsRetirement = enabled; }
    bool IsDeferredGraphicsRetirementEnabled() const
    { AssertOwnerThread(); return m_deferGraphicsRetirement; }
    void SetBeforeGpuResourceDestruction(std::function<void()> waitForGpu)
    { AssertOwnerThread(); m_beforeGpuResourceDestruction = std::move(waitForGpu); }
    void WaitForGpuResourceDestruction()
    { AssertOwnerThread(); if (m_beforeGpuResourceDestruction) m_beforeGpuResourceDestruction(); }

    // ---- Structure observers ---------------------------------------------------
    void AddObserver(ISceneObserver* observer);
    void RemoveObserver(ISceneObserver* observer);

    // 2D scenes use an orthographic fallback camera and 2D render paths.
    // Kept in sync with settings.dimension.
    void SetMode2D(bool enabled);
    bool IsMode2D() const { return m_mode2D; }

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
    // Keep value snapshots stable while an editor incrementally restores a
    // document, then publish the completed graph once at the commit point.
    void BeginIncrementalRestore();
    void EndIncrementalRestore();
    // A worker must carry the generation captured when its scene work was
    // scheduled. The owner fences tracked jobs before replacing or destroying
    // objects, so no callback from a reloadable module survives that boundary.
    using SceneGeneration = uint64_t;
    SceneGeneration CaptureSceneGeneration() const
    { return m_sceneGeneration.load(std::memory_order_acquire); }
    Engine::Threading::JobScheduler::Handle ScheduleSceneJob(
        std::function<void(SceneGeneration)> work);
    void FenceSceneGeneration();
    // The standalone game has no reader for per-object snapshot contents
    // (only the editor does). Disabling them publishes snapshots without the
    // object list, saving a full-scene compare/rebuild every commit.
    void SetSceneSnapshotObjectsEnabled(bool enabled) { m_sceneSnapshotObjects = enabled; }
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
        if(!owner) { m_componentRegistry.Reconcile(m_objects); m_updateRegistryFull=true; }
        else
        {
            if(removed) m_componentRegistry.Remove(owner); else m_componentRegistry.Changed(owner);
            m_updateRegistryDirty.insert(owner);
        }
        for (ISceneObserver* observer : m_observers) observer->OnStructureChanged(owner);
    }
    void NotifyComponentRemoved(Engine::Core::Component* component)
    {
        AssertOwnerThread();
        m_componentSnapshotCache.erase(component);
        m_componentRegistry.Remove(component);
        if(++m_structureRevision==0) ++m_structureRevision;
        if(component && component->Owner)
        {
            m_updateRegistryDirty.insert(component->Owner);
            for (ISceneObserver* observer : m_observers) observer->OnStructureChanged(component->Owner);
        }
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
    bool RestoreTrustedSnapshot(const Engine::Serialization::JsonValue& snapshot,
        bool publishSceneSnapshot = true, bool retainPrefabPrototypes = false);
    size_t GetRestoreRetainedPrefabPrototypeCount() const { return m_restorePrefabPrototypes.size(); }
    bool LoadFromString(const std::string& source);
    bool LoadFromValue(const Engine::Serialization::JsonValue& document);


    SceneSettings settings;

    IGraphicsProvider* GetGraphicsProvider() const { return m_graphicsProvider; }

    // Runtime state a component system keeps per scene and owns itself
    // (Components::SharedPrefabParts). Never serialized.
    std::shared_ptr<void> sharedPrefabParts;
    // Likewise for GPU grass blades (Assets/Grass/GrassBlades).
    std::shared_ptr<void> grassBlades;

private:
    // Reads object memory and the transform hierarchy while preparing frames.
    friend class Engine::Rendering::SceneRenderer;

    bool m_gameplayInputManaged = false;
    bool m_gameplayInputEnabled = true;
    glm::vec2 m_gameplayInputAnchor { 0.f };
    // ---- Component GPU-resource lifetime (kept API-agnostic) ----
    // Components create resources through this provider. Replaced buffers and
    // removed render-only objects wait here for the host's frame fence.
    IGraphicsProvider* m_graphicsProvider = nullptr;
    std::vector<std::unique_ptr<Object>> m_retiredRenderObjects;
    std::vector<std::unique_ptr<IGraphicsBuffer>> m_retiredRenderBuffers;
    std::vector<std::shared_ptr<IGraphicsBuffer>> m_retiredSharedRenderBuffers;
    bool m_deferGraphicsRetirement = false;
    std::function<void()> m_beforeGpuResourceDestruction;
    std::vector<Engine::Model::UITextLayout> m_preparedNativeUiLayout;
    std::unique_ptr<Engine::Physics::Physics> m_physics;

    Engine::Components::TransformHierarchy m_transformHierarchy;
    uint64_t m_transformPreparationRevision=UINT64_MAX;
    std::vector<Engine::Components::Transform*> m_transformPreparationScratch;
    bool m_updateRegistryFull=true;
    std::unordered_set<Object*> m_updateRegistryDirty;

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
    bool m_sceneSnapshotObjects = true;
    uint64_t m_snapshotGeneration = 0;
    double m_lastSnapshotPublishMs = 0;
    bool m_isCommitting = false;
    bool m_isFlushingRemovals = false;
    bool QueueSceneMutation(SceneGeneration generation,
        std::function<void(Scene&)> mutation);
    void PublishSceneSnapshot();
    bool m_isUpdating = false;
    bool m_updateProfilingEnabled = false;
    bool m_hasStarted = false;
    bool m_graphicsPreparationPending = false;
    bool m_graphicsPreparationRestart = false;
    double m_graphicsPreparationBudgetMs = 0;
    std::vector<std::pair<ObjectId, Engine::Core::Component*>> m_graphicsPreparationItems;
    size_t m_graphicsPreparationCursor = 0;
    uint64_t m_lifecycleRevision = 0;
    size_t m_particleBudget = 100000;
    Engine::Runtime::Game* m_game = nullptr;
    uint64_t m_structureRevision = 1;
    mutable Engine::Scene::ComponentRegistry m_componentRegistry;
    uint64_t m_physicsActivityRevision=0;
    uint64_t m_updateRegistryRevision = 0;
    std::vector<Object*> m_updateObjects, m_poseObjects;
    Engine::Components::SpatialUpdateScheduler m_spatialUpdates;
    SceneCounters m_counters;
    // Keeps immutable prefab prototypes alive while a trusted play restore
    // recreates its layers. Released once those layers finish preparing.
    std::vector<std::shared_ptr<void>> m_restorePrefabPrototypes;
    void RefreshUpdateRegistry();
    Object* AddObjectWithId(ObjectId id);
    void FlushPendingObjectAdditions();
    void FlushPendingObjectRemovals();
    std::vector<ISceneObserver*> m_observers;
    bool m_mode2D = false;
    bool m_fallbackCameraConfigured = false;
};
}

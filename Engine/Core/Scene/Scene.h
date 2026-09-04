#pragma once
#include "Core/Object.h"
#include "Core/Graphics/IPipelineState.h"
#include "Core/Graphics/IGraphicsBuffer.h"
#include "Core/Rendering/Lighting/Pipelines/Realtime/RealtimeLightingPipeline.h"
#include "Core/Rendering/Lighting/Pipelines/Baked/BakedLightingPipeline.h"
#include "Core/Model/SceneSettings.h"
#include "Core/Model/MeshData.h"
#include <glm/glm.hpp>
#include <array>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace Engine::Physics { class Physics; }
namespace Engine::Audio { class Audio; }
namespace Engine::Components
{
    class Camera;
    class Texture;
    class Mesh;
    class Sprite;
    class Material;
}
namespace Engine::Rendering { class BakedLightingData; }
namespace Engine::Renderers { class UIRenderer; }
namespace Engine::Graphics { class IGraphicsProvider; class IGraphicsContext; }

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
    enum class ObjectPlacement { Before, AsChild, After };

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
    struct SpatialRay
    {
        glm::vec3 origin { 0.f };
        glm::vec3 direction { 0.f, 0.f, 1.f };
    };
    // A straight segment in one spatial chart.  A portal hit ends a segment;
    // the following segment starts at the connected endpoint in its chart.
    struct PortalRaySegment
    {
        SpatialRay ray;
        float maxDistance = 0.f;
        const Object* enteredPortal = nullptr;
    };

    Scene();
    ~Scene();

    // Runtime lifecycle shared by the standalone game and Editor Play mode.
    void Start();
    void Update(float deltaTime);
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
    void PrepareRenderFrame();
    void Render(IGraphicsContext* context, float aspect,
        Camera* cameraOverride = nullptr, bool includeEditorVisuals = true,
        uint32_t viewportWidth = 0u, uint32_t viewportHeight = 0u);
    void SetSelectedObject(Object* obj) { m_selectedObject = obj; }
    Object* GetSelectedObject() const { return m_selectedObject; }
    void SetPreviewObject(Object* obj) { m_previewObject = obj; }
    void SetEditorMode2D(bool enabled);
    bool IsEditorMode2D() const { return m_editorMode2D; }
    // Pointer coordinates relative to the surface displaying the game render
    // target, used to keep embedded-view UI hit bounds aligned.
    void SetUiPointerInput(float x, float y, float viewportWidth,
        float viewportHeight, bool hovered, bool mouseDown);

    // Returns the first active Camera component found on a scene game object.
    // The editor camera is deliberately excluded so GameView cannot silently
    // render from the Scene view's navigation camera.
    Camera* FindGameCamera();

    // Maps the ordinary Euclidean scene chart through active spatial volumes.
    // The sample Jacobian carries nonlinear orientation/scale to consumers.
    SpatialQuerySample SampleSpatialPoint(const glm::vec3& worldPoint,
        const SpatialQuery& query = {}) const;
    glm::vec3 MapSpatialPoint(const glm::vec3& worldPoint,
        const SpatialQuery& query = {}) const;
    glm::mat4 MapSpatialMatrix(const glm::mat4& worldMatrix,
        const SpatialQuery& query = {}) const;
    SpatialRay MapSpatialRay(const SpatialRay& ray,
        const SpatialQuery& query = {}) const;
    // Trace a finite physical-space ray across portal apertures.  This is the
    // common primitive for gameplay queries and editor picking; callers test
    // ordinary geometry against each returned segment in order.  Nonlinear
    // volume integration remains a separate concern from discrete portals.
    std::vector<PortalRaySegment> TracePortalRay(const SpatialRay& ray,
        float maxDistance, uint32_t maxPortalHops = 8u) const;

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

    // Object management
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
    void    ClearObjects();                  // remove all objects and reset selection
    const std::vector<std::unique_ptr<Object>>& GetObjects() const { return m_objects; }
    bool TryGetObjectPath(const Object* object, ObjectPath& path) const;
    Object* FindObjectByPath(const ObjectPath& path) const;
    bool MoveObject(Object* object, Object* target, ObjectPlacement placement);

    // Serialization — delegates to SceneSerializer.
    // Save writes the scene to a scene XML file.
    // Load clears the scene and repopulates it from the file.  Mesh GPU
    // buffers are created automatically using the graphics provider stored by Init().
    bool Save(const std::string& path) const;
    bool Load(const std::string& path);
    std::string SaveToString() const;
    bool LoadFromString(const std::string& source);

    Engine::Model::BakeResult BakeLighting(
        const std::string& assetsDirectory = "Assets",
        const std::string& sceneName = "Scene",
        const Engine::Model::BakedLightingSettings& bakeSettings = {});
    void ClearBakedLighting();
    const std::string& GetLightingBakeStatus() const { return m_lightingBakeStatus; }

    SceneSettings settings;

    IGraphicsProvider* GetGraphicsProvider() const { return m_graphicsProvider; }
    const Engine::Components::Texture* GetSkyboxPreviewTexture();

private:
    // ---- Rendering resources (kept API-agnostic) ----
    IGraphicsProvider* m_graphicsProvider = nullptr;
    std::unique_ptr<IPipelineState> m_gridPipeline;
    std::unique_ptr<IGraphicsBuffer> m_gridConstantBuffer;
    void* m_gridCBMapped = nullptr;

    std::unique_ptr<IPipelineState> m_skyboxPipeline;
    std::unique_ptr<IPipelineState> m_portalSkyboxStencilReadPipeline;
    std::unique_ptr<IGraphicsBuffer> m_skyboxConstantBuffer;
    void* m_skyboxCBMapped = nullptr;
    std::shared_ptr<Engine::Components::Texture> m_defaultSkyboxTexture;
    std::shared_ptr<Engine::Components::Texture> m_sceneSkyboxTexture;
    std::string m_loadedSkyboxPath;
    std::string m_environmentLightingPath;
    std::array<glm::vec4, 9> m_environmentSH{};

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
    std::unique_ptr<IGraphicsBuffer> m_objectConstantBuffer;
    void* m_objectCBMapped = nullptr;
    std::unique_ptr<IGraphicsBuffer> m_objectDataBuffer;
    void* m_objectDataMapped = nullptr;
    std::unique_ptr<IGraphicsBuffer> m_lightDataBuffer;
    void* m_lightDataMapped = nullptr;
    std::unique_ptr<IGraphicsBuffer> m_boneDataBuffer;
    void* m_boneDataMapped = nullptr;
    std::unique_ptr<IGraphicsBuffer> m_portalApertureBuffer;
    void* m_portalApertureMapped = nullptr;
    std::unique_ptr<Engine::Renderers::UIRenderer> m_uiRenderer;
    std::unique_ptr<Engine::Physics::Physics> m_physics;
    std::unique_ptr<Engine::Audio::Audio> m_audio;

    Engine::Rendering::RealtimeLightingPipeline m_realtimeLightingPipeline;
    Engine::Rendering::BakedLightingPipeline m_bakedLightingPipeline;
    std::string m_lightingBakeStatus = "Lighting has not been baked.";

    void BuildGridPipeline();
    void BuildSkyboxPipeline();
    void BuildObjectPipeline();
    const Engine::Components::Texture* ResolveSkyboxTexture();
    void UpdateEnvironmentLighting(const Engine::Components::Texture* texture);
    std::shared_ptr<const std::array<glm::vec4, 9>> ResolveReflectionEnvironment(
        const Engine::Components::Material& material);

    struct FrameRenderItem
    {
        Object* object = nullptr;
        Engine::Components::Mesh* mesh = nullptr;
        Engine::Components::Sprite* sprite = nullptr;
        Engine::Components::Material* material = nullptr;
        const Engine::Rendering::BakedLightingData* bakedLighting = nullptr;
        IGraphicsBuffer* spriteVertexBuffer = nullptr;
        const Engine::Components::Texture* spriteTexture = nullptr;
        // When a mesh crosses a nonlinear warp boundary, this buffer holds
        // the per-vertex mapped positions, normals and tangents.  It is a
        // renderer-owned view of the authored mesh, never the mesh itself.
        IGraphicsBuffer* warpedVertexBuffer = nullptr;
        glm::mat4 world{1.f};
        // Non-zero only for a portal-split chart instance. Object.hlsl clips
        // against this world-space plane without modifying the mesh buffer.
        glm::vec4 traversalClipPlane{0.f};
        glm::vec2 spriteWorldSize{1.f};
        glm::vec4 spriteUvRect{0.f, 0.f, 1.f, 1.f};
        uint32_t skinPaletteOffset = 0;
        uint32_t skinJointCount = 0;
        int sortingLayer = 0;
        bool belongsToPreview = false;
        bool blended = false;
    };

    std::vector<FrameRenderItem> m_frameRenderItems;

    struct WarpedRenderMesh
    {
        std::unique_ptr<IGraphicsBuffer> vertexBuffer;
        std::vector<Engine::Model::Vertex> vertices;
    };
    // Object ownership remains in m_objects; this cache only owns transient
    // GPU upload buffers. Entries are replaced when topology changes and are
    // pruned as objects leave the scene.
    std::unordered_map<const Object*, WarpedRenderMesh> m_warpedRenderMeshes;
    uint32_t m_frameLightCount = 0;
    bool m_renderFramePrepared = false;

    // ---- Object list ----
    std::vector<std::unique_ptr<Object>> m_objects;
    std::vector<std::unique_ptr<Object>> m_pendingObjectAdditions;
    std::vector<Object*> m_pendingObjectRemovals;
    bool m_isUpdating = false;
    bool m_hasStarted = false;
    void FlushPendingObjectAdditions();
    void FlushPendingObjectRemovals();
    Object* m_selectedObject = nullptr;
    Object* m_previewObject = nullptr;
    bool m_editorMode2D = false;
    bool m_editorCameraModeInitialized = false;

    static constexpr uint32_t kMaxObjects = 64;
    // The editor diagnostic expands a linked 8-point portal pair into point
    // markers, boundary bars, correspondence bars, and plane normals. Keep
    // enough transient space for every logical spatial object; it is never
    // used by runtime portal rendering.
    static constexpr uint32_t kMaxSpatialVerticesPerObject = 1024;
    static constexpr uint32_t kMaxBonesPerObject = 256;
    static constexpr uint32_t kMaxLights =
        Engine::Model::MaxRealtimeLights;
    static constexpr uint32_t kCBStride = 256;
};
}

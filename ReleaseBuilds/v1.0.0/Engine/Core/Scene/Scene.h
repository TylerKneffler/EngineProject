#pragma once
#include "Core/Object.h"
#include "Core/Graphics/IPipelineState.h"
#include "Core/Graphics/IGraphicsBuffer.h"
#include "Core/Rendering/Lighting/Pipelines/Realtime/RealtimeLightingPipeline.h"
#include "Core/Rendering/Lighting/Pipelines/Baked/BakedLightingPipeline.h"
#include "Core/Serialization/Json.h"
#include <glm/glm.hpp>
#include <string>

// Forward declarations
class IGraphicsProvider;
class IGraphicsContext;
class IShaderCompiler;
class IPipelineStateFactory;
class IGraphicsBufferFactory;
class Texture;

// ---------------------------------------------------------------------------
// Scene
//
// Owns all game objects for one scene and manages scene-wide rendering
// helpers such as the editor grid.  Only the editor uses Init()/Render();
// the game runtime will call Start()/Update() instead.
//
// ---- Typical editor usage ----
//
//   auto graphicsProvider = renderer->GetGraphicsProvider();
//   scene.Init(graphicsProvider);
//
//   // inside SceneView drawFn:
//   scene.Render(graphicsContext);
// ---------------------------------------------------------------------------

struct SceneSettings
{
    // Grid
    bool  showGrid        = true;
    int   gridHalfSize    = 10;          // legacy — kept for serialization compat
    float gridCellSize    = 1.f;         // spacing between lines (world units)
    float gridOpacity     = 0.4f;        // 0 = invisible, 1 = fully opaque
    float gridFadeDistance = 80.f;       // world units; grid fades to 0 at this distance
    glm::vec3 gridColor        = glm::vec3(0.45f, 0.45f, 0.45f);
    glm::vec3 gridOriginColor  = glm::vec3(0.30f, 0.50f, 0.80f); // X/Z axis lines

    // Ambient light
    glm::vec3 ambientColor = glm::vec3(0.12f, 0.12f, 0.12f);

    // Optional equirectangular panorama. Empty uses the bundled editor sky.
    std::string skyboxTexture;

    JsonValue Serialize() const;
    void Deserialize(const JsonValue& value);
};

class Scene
{
public:
    using ObjectPath = std::vector<std::size_t>;
    enum class ObjectPlacement { Before, AsChild, After };

    Scene()  = default;
    ~Scene() = default;

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
    void Render(IGraphicsContext* context, float aspect,
        Camera* cameraOverride = nullptr, bool includeEditorVisuals = true);
    void SetSelectedObject(Object* obj) { m_selectedObject = obj; }
    Object* GetSelectedObject() const { return m_selectedObject; }
    void SetPreviewObject(Object* obj) { m_previewObject = obj; }

    // Returns the first active Camera component found on a scene game object.
    // The editor camera is deliberately excluded so GameView cannot silently
    // render from the Scene view's navigation camera.
    Camera* FindGameCamera();

    // Move the editor camera to frame the given object, keeping a comfortable
    // viewing distance and looking directly at its world-space origin.
    // Pass nullptr to reset to the default startup position.
    void FocusEditorCamera(Object* obj);

    // Object management
    Object* AddObject();                     // create an empty Object owned by this scene
    Object* AddObject(const std::string& name);
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

    Engine::Rendering::Lighting::BakeResult BakeLighting();
    void ClearBakedLighting();
    const std::string& GetLightingBakeStatus() const { return m_lightingBakeStatus; }

    SceneSettings settings;

    IGraphicsProvider* GetGraphicsProvider() const { return m_graphicsProvider; }
    const Texture* GetSkyboxPreviewTexture();

private:
    // ---- Rendering resources (kept API-agnostic) ----
    IGraphicsProvider* m_graphicsProvider = nullptr;
    std::unique_ptr<IPipelineState> m_gridPipeline;
    std::unique_ptr<IGraphicsBuffer> m_gridConstantBuffer;
    void* m_gridCBMapped = nullptr;

    std::unique_ptr<IPipelineState> m_skyboxPipeline;
    std::unique_ptr<IGraphicsBuffer> m_skyboxConstantBuffer;
    void* m_skyboxCBMapped = nullptr;
    std::shared_ptr<Texture> m_defaultSkyboxTexture;
    std::shared_ptr<Texture> m_sceneSkyboxTexture;
    std::string m_loadedSkyboxPath;

    std::unique_ptr<IPipelineState> m_objectPipeline;
    std::unique_ptr<IPipelineState> m_objectPreviewPipeline;
    std::unique_ptr<IPipelineState> m_objectOutlinePipeline;
    std::unique_ptr<IGraphicsBuffer> m_objectConstantBuffer;
    void* m_objectCBMapped = nullptr;
    std::unique_ptr<IGraphicsBuffer> m_objectDataBuffer;
    void* m_objectDataMapped = nullptr;
    std::unique_ptr<IGraphicsBuffer> m_lightDataBuffer;
    void* m_lightDataMapped = nullptr;

    Engine::Rendering::Lighting::RealtimeLightingPipeline m_realtimeLightingPipeline;
    Engine::Rendering::Lighting::BakedLightingPipeline m_bakedLightingPipeline;
    std::string m_lightingBakeStatus = "Lighting has not been baked.";

    void BuildGridPipeline();
    void BuildSkyboxPipeline();
    void BuildObjectPipeline();
    const Texture* ResolveSkyboxTexture();

    // ---- Object list ----
    std::vector<std::unique_ptr<Object>> m_objects;
    Object* m_selectedObject = nullptr;
    Object* m_previewObject = nullptr;

    static constexpr uint32_t kMaxObjects = 64;
    static constexpr uint32_t kMaxLights =
        Engine::Rendering::Lighting::MaxRealtimeLights;
    static constexpr uint32_t kCBStride = 256;
};

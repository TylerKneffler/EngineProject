#pragma once
#include "Core/Object.h"
#include "Core/Graphics/IPipelineState.h"
#include "Core/Graphics/IGraphicsBuffer.h"
#include <glm/glm.hpp>

// Forward declarations
class IGraphicsProvider;
class IGraphicsContext;
class IShaderCompiler;
class IPipelineStateFactory;
class IGraphicsBufferFactory;

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
};

class Scene
{
public:
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
    // cameraOverride: if non-null, use this camera instead of the normal selection.
    void Render(IGraphicsContext* context, float aspect, Camera* cameraOverride = nullptr);
    void SetSelectedObject(Object* obj) { m_selectedObject = obj; }

    // Returns the first Camera component found on any scene game object,
    // or the editor camera as a fallback. Used by GameView.
    Camera* FindGameCamera();

    // Object management
    Object* AddObject();                     // create an empty Object owned by this scene
    Object* AddObject(const std::string& name);
    void    RemoveObject(Object* obj);
    void    ClearObjects();                  // remove all objects and reset selection
    const std::vector<std::unique_ptr<Object>>& GetObjects() const { return m_objects; }

    // Serialization — delegates to SceneSerializer.
    // Save writes the scene to a .scene JSON file.
    // Load clears the scene and repopulates it from the file.  Mesh GPU
    // buffers are created automatically using the graphics provider stored by Init().
    bool Save(const std::string& path) const;
    bool Load(const std::string& path);

    SceneSettings settings;

private:
    // ---- Rendering resources (kept API-agnostic) ----
    IGraphicsProvider* m_graphicsProvider = nullptr;
    std::unique_ptr<IPipelineState> m_gridPipeline;
    std::unique_ptr<IGraphicsBuffer> m_gridConstantBuffer;
    void* m_gridCBMapped = nullptr;

    std::unique_ptr<IPipelineState> m_objectPipeline;
    std::unique_ptr<IGraphicsBuffer> m_objectConstantBuffer;
    void* m_objectCBMapped = nullptr;

    void BuildGridPipeline();
    void BuildObjectPipeline();

    // ---- Object list ----
    std::vector<std::unique_ptr<Object>> m_objects;
    Object* m_selectedObject = nullptr;

    static constexpr uint32_t kMaxObjects = 64;
    static constexpr uint32_t kCBStride = 256;
};

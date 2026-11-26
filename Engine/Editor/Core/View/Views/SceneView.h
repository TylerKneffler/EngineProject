#pragma once
#include <algorithm>
#include <set>
#include <string>
#include "View/View.h"
#include "Engine/Editor/UI/IEditorUi.h"
#include "Core/Scene/Scene.h"
#include "Core/Rendering/SceneRenderer.h"
#include "Core/Components/Terrain/Terrain.h"
#include "Core/Components/Terrain/TerrainRegionEdit.h"
#include "Core/Brushes/BrushPreset.h"
#include "Core/ProjectLoader.h"
#include "Engine/Editor/Core/Gizmos/EditorGizmoSystem.h"
#include "Scene/SceneViewportToolbar.h"
#include "Core/Serialization/PreparedPrefab.h"
#include "Engine/Editor/Core/View/Templates/EditModes/ViewportModeTemplate.h"

namespace Engine::Editor
{
// ---------------------------------------------------------------------------
// SceneView — editor Scene panel
//
// Extends View with scene-camera orbit/pan/zoom controls driven by mouse
// input captured inside the Scene panel. Supports aspect ratio
// constraints with letterboxing/pillarboxing visualization.
//
// ---- Per-frame call order ----
//   sceneViewport.Init(device, w, h, srvCpu, srvGpu, &scene, projectSettings);
//   sceneViewport.Render(cmdList, mainRtv, drawFn);   // inherited from View
//   sceneViewport.DrawPanel();                        // overridden here
// ---------------------------------------------------------------------------
class SceneView : public View
{
public:
    SceneView();
    ~SceneView();

    // Calls View::Init then stores the scene pointer and aspect ratio settings.
    // scene and settings must outlive this viewport.
    // device: opaque renderer device handle (cast internally to ID3D12Device*)
    // srvCpu/srvGpu: opaque descriptor handles (cast internally from void*)
    void Init(void* device,
              uint32_t width, uint32_t height,
              void* srvCpu,
              void* srvGpu,
              uint32_t srvSlotIndex,
              Engine::Scene::Scene* scene,
              const Engine::Model::ProjectSettings& settings);

    // Issues the editor-camera scene draw into cmd each frame.
    // cmd: opaque graphics command list handle (cast internally to ID3D12GraphicsCommandList*)
    void Render3D(void* cmd) override;
    void RenderShadow3D(void* cmd) override;
    void RecordRenderCommands(Engine::Graphics::GraphicsCommandPacket& shadow,
        Engine::Graphics::GraphicsCommandPacket& color,
        const Engine::Rendering::SceneRenderer::NativeRenderPacket* packet, RecordingTimings* timings = nullptr);

    // Shows the offscreen texture with letterboxing/pillarboxing based on
    // aspect ratio settings, captures mouse input, and drives the
    // scene's editorCamera with FPS-style look / pan / zoom.
    void DrawPanel(IEditorUi& ui) override;
    Engine::Scene::Scene* GetScene() const { return m_scene; }
    void SetScene(Engine::Scene::Scene* scene);
    bool ContainsScreenPoint(float x, float y) const
    { return m_viewportScreenMax.x > m_viewportScreenMin.x &&
        x >= m_viewportScreenMin.x && x < m_viewportScreenMax.x &&
        y >= m_viewportScreenMin.y && y < m_viewportScreenMax.y; }
    void SetEditMode(SceneEditMode mode) { m_editMode = mode; }
    SceneEditMode GetEditMode() const { return m_editMode; }
    EditorTransformTool GetTransformTool() const
    { return m_toolbar.GetTransformTool(); }
    void SetTransformTool(EditorTransformTool tool)
    { m_toolbar.SetTransformTool(tool); }
    void SetPoseEditing(bool enabled) { m_gizmos.SetPoseEditing(enabled); }
    void SetModelManipulation(bool enabled) { m_modelManipulation = enabled; }
    void SetModelSelectionMode(int mode)
    { m_modelSelectionMode = std::clamp(mode, 0, 3); }
    const std::vector<Engine::Core::Object*>& ModelChangedBones() const
    { return m_gizmos.ModelChangedBones(); }
    bool ModelDragEnded() const { return m_gizmos.ModelDragEnded(); }
    void SetPoseTranslationHandler(std::function<bool(Engine::Core::Object*,
        const glm::vec3&)> handler)
    { m_gizmos.SetPoseTranslationHandler(std::move(handler)); }
    void SetBoneTransformSettings(int pivotMode, const glm::vec3& customPivot,
        float translationSnap, float rotationSnapDegrees, float scaleSnap)
    {
        m_gizmos.SetBoneTransformSettings(pivotMode, customPivot,
            translationSnap, rotationSnapDegrees, scaleSnap);
    }
    void DrawPoseToolbar(const std::function<bool(float)>& next,
        bool animationEditor = false, bool inlineOptions = false);
    void DrawTerrainToolbar(const std::function<bool(float)>& next);
    // Select tool controls: shape, feather, operation, its settings, Apply.
    void DrawTerrainRegionToolbar(const std::function<bool(float)>& next);
    int GetTerrainObjectDisplayLod() const { return m_terrainObjectDisplayLod; }
    void SetTerrainObjectDisplayLod(int lod)
    { m_terrainObjectDisplayLod = std::clamp(lod, -1, 15); }
    int GetSceneRenderMode() const { return m_sceneRenderMode; }
    void SetSceneRenderMode(int mode) { m_sceneRenderMode = std::clamp(mode, 0, 5); }
    bool UseGlobalToolbar = false;
    bool UseGlobalBoneVisibility = true;
    void SetDocumentPath(std::string path) { m_documentPath = std::move(path); }
    void RequestFocusOnNextDraw() { m_focusOnNextDraw = true; }
    std::function<void(const std::string&)> OnAssetDropped;
    std::function<void(const std::string&, Engine::Core::Object*)> OnAssetAssigned;
    std::function<Engine::Core::Object*(const std::string&)> OnAssetPreviewRequested;
    std::function<Engine::Core::Object*(Engine::Scene::Scene*, Engine::Serialization::PreparedPrefab&)> OnPreparedAssetPreviewRequested;
    bool HasPendingPrefabPreparation() const { return m_prefabPreparation.valid() || m_prefabDropPending; }
    std::function<void(Engine::Core::Object*)> OnAssetPreviewCancelled;
    std::function<void(Engine::Core::Object*, const std::string&)> OnAssetPreviewCommitted;
    std::function<void(Engine::Core::Object*)> OnObjectSelected;
    std::function<void(Engine::Core::Object*)> OnObjectCreated;
    std::function<void()> OnDeleteSelectionRequested;
    std::function<void(bool)> OnGizmoInteraction;
    std::function<void()> OnKeyModelControllersRequested;
    std::function<void()> OnModelControllerEdited;
    std::function<bool(IEditorUi&)> OnDrawDocumentTools;
    std::function<void(IEditorUi&)> OnDrawBottomPanel;
    std::function<bool(IEditorUi&, const EditorUiViewportInput&,
        EditorTransformTool)> OnMeshViewportInput;
    std::function<bool(IEditorUi&, const EditorUiViewportInput&)> OnDrawRigControls;
    std::function<void(IEditorUi&)> OnDocumentDrop;
    std::function<bool(const Engine::Core::Object*)> CanSelectObject;
    bool AllowObjectCreation = true;
    bool AllowAssetDrops = true;
    bool AllowObjectTransform = true;
    float BottomPanelHeight = 420.f;

protected:
    float EffectiveRenderScale() const override;
private:
    void CancelPrefabPreview();

    // Calculates the game viewport size and position based on aspect ratio mode
    Engine::Scene::Scene* m_scene = nullptr; // non-owning; set via Init()
    EditorUiVec2 m_viewportScreenMin{}, m_viewportScreenMax{};
    std::string m_documentPath;
    
    // Aspect ratio settings
    Engine::Model::ProjectSettings::AspectRatioMode m_aspectRatioMode = Engine::Model::ProjectSettings::AspectRatioMode::Locked;
    float m_gameAspectRatio = 1.777f;  // 16:9
    uint32_t m_gameWindowWidth = 1920;
    uint32_t m_gameWindowHeight = 1080;
    glm::vec4 m_letterboxColor{0.f, 0.f, 0.f, 1.f};

    // Computed viewport aspect ratio
    float m_aspect = 1.0f;
    Engine::Core::Object* m_prefabPreview = nullptr;
    Engine::Core::Object* m_previousSelectedObject = nullptr;
    std::string m_prefabPreviewPath;
    bool m_prefabPreviewHasPlacement = false;
    std::future<std::unique_ptr<Engine::Serialization::PreparedPrefab>> m_prefabPreparation;
    std::shared_ptr<std::atomic_bool> m_prefabPreparationCanceled;
    glm::vec3 m_prefabPlacement{};
    bool m_prefabDropPending = false;
    Engine::Scene::Scene::SceneGeneration m_prefabPreparationGeneration = 0;
    std::string m_prefabPreparationError;
    void PollPrefabPreparation();
    void CommitPrefabPreview();
    bool m_focusOnNextDraw = false;
    SceneEditMode m_editMode = SceneEditMode::Object;
    // Toolbar tool index: Raise, Lower, Smooth, Flatten, Paint, Objects,
    // then the region-operation brushes (Erode, Weather, Noise, Terrace),
    // which run a TerrainRegionOperation under the brush every frame, and
    // Select, which draws a region and applies a heavy operation on commit.
    // Shift inverts the tool while held (raise/lower, paint/erase layer,
    // place/erase objects, noise sign).
    static constexpr int kSmoothBrush = 2;
    static constexpr int kFlattenBrush = 3;
    static constexpr int kPaintBrush = 4;
    static constexpr int kObjectBrush = 5;
    static constexpr int kErodeBrush = 6;
    static constexpr int kWeatherBrush = 7;
    static constexpr int kNoiseBrush = 8;
    static constexpr int kTerraceBrush = 9;
    static constexpr int kSelectTool = 10;
    // Stamp: Raise with a stamp's shape (required): held, it raises the
    // ground under the stamp's outline, its high places fastest; Shift lowers.
    static constexpr int kStampTool = 11;
    // Volume sculpting (volume edits, TerrainManager::AddVolumeEdit): Rock
    // adds rock along the stroke, Carve cuts it away (Shift swaps them),
    // Ledge levels to the height where the stroke began, cutting above and
    // filling below. They aim at the terrain field (overhangs, holes) and
    // need a Terrain Manager.
    static constexpr int kRockTool = 12;
    static constexpr int kCarveTool = 13;
    static constexpr int kLedgeTool = 14;
    bool VolumeTerrainTool() const { return m_terrainBrush >= kRockTool && m_terrainBrush <= kLedgeTool; }
    bool m_volumeStroke = false;
    glm::dvec3 m_volumeLast{0.0};
    float m_volumeLedgeHeight = 0.f;
    // The terrain brush (a .brush asset) for Raise, Lower, Smooth, Flatten,
    // Paint, Objects and Stamp, or empty for the round brush: its tip shapes
    // the stroke (TerrainBrushShape), its relief is what Stamp builds, its
    // rules limit where strokes act, its pressure and scatter settings vary
    // each dab, and a Place brush's placement drives the Objects tool.
    std::string m_terrainBrushPath;
    Engine::Brushes::BrushPreset m_terrainBrushPreset;
    bool m_terrainBrushLoaded = false;
    std::string m_terrainBrushError;
    long long m_terrainBrushStamp = 0;
    // The round brush's falloff past the hardness (0 hard, 1 linear, 2 smooth).
    int m_terrainFalloff = 2;
    float m_terrainShapeYawDegrees = 0.f;
    // Stamp: build only against terrain behind the shape (BrushOptions::backed).
    bool m_terrainStampBacked = true;
    // Stamp: grows to its own surface (BrushOptions::stopAtStamp), heights times this.
    float m_terrainStampHeightScale = 1.f;
    std::vector<std::string> m_terrainBrushAssets;
    bool ShapedTerrainTool() const
    { return m_terrainBrush <= kObjectBrush || m_terrainBrush == kStampTool; }
    void DrawTerrainShapeToolbar(const std::function<bool(float)>& next);
    // Loads the brush (empty path: the round brush). applySettings: also take
    // its hardness, falloff, rotation, relief's backing, and its tool and
    // paint layer (a Paint brush switches to Paint on its layer, a Place
    // brush to Objects, a relief brush to Stamp).
    void SelectTerrainBrush(const std::string& path, bool applySettings);
    const Engine::Brushes::BrushRules* TerrainBrushRules() const
    {
        return m_terrainBrushLoaded && m_terrainBrushPreset.terrain.rules.Any()
            ? &m_terrainBrushPreset.terrain.rules : nullptr;
    }
    // A Foliage brush (a foliage section): the Paint tool paints foliage
    // density (TerrainFoliage.h) instead of a paint layer.
    bool TerrainBrushFoliage() const
    { return m_terrainBrushLoaded && m_terrainBrushPreset.terrain.foliage.Any(); }
    bool TerrainBrushPlaces() const
    { return m_terrainBrushLoaded && m_terrainBrushPreset.terrain.placement.Any(); }
    // A dab's pen pressure (radius and strength factors, from the brush's
    // pressure settings) and its centre moved by the brush's scatter.
    float TerrainBrushPressureSize() const;
    float TerrainBrushPressureStrength() const;
    glm::dvec3 ScatterTerrainDab(const glm::dvec3& local, float radius);
    // Settings of the region-operation brushes (one set, read per tool).
    Engine::Components::TerrainRegionOperation m_brushOperation;
    uint32_t m_brushSeed = 1;
    // Select tool. Shapes: 0 rectangle, 1 ellipse, 2 lasso (drag), 3 polygon
    // (click points; click the first point, double-click or Enter closes).
    int m_regionShape = 0;
    float m_regionFeather = 4.f;
    Engine::Components::TerrainRegionMask m_regionMask;   // committed selection
    bool m_regionDrawing = false;
    glm::dvec2 m_regionStart{0.0}, m_regionLast{0.0};
    std::vector<glm::dvec2> m_regionPoints;
    glm::vec2 m_regionLastScreen{0.f};
    // Operations: see kRegionOperations in SceneView.cpp.
    int m_regionOperation = 0;
    static constexpr int kRegionPaint = 7;
    static constexpr int kRegionPlace = 8;
    static Engine::Components::TerrainRegionOperation::Kind RegionOperationKind(int operation);
    // Settings widgets for one operation (index into kRegionOperations);
    // brush mode hides settings that only make sense for a committed region.
    static void DrawRegionOperationSettings(Engine::Components::TerrainRegionOperation& op,
        int operation, bool brush);
    static int BrushOperationKind(int tool)
    { return tool == kErodeBrush ? 0 : tool == kWeatherBrush ? 1 : tool == kNoiseBrush ? 3 : 4; }
    Engine::Components::TerrainRegionOperation m_regionSettings;
    bool m_regionApplyRequested = false;
    std::string m_regionStatus;
    void ClearRegionSelection()
    { m_regionMask = {};
      m_regionDrawing = false; m_regionPoints.clear(); }
    int m_terrainBrush = 0;
    // Fraction of the radius at full strength before the smooth falloff.
    float m_terrainHardness = .5f;
    // Flatten target: the surface height where the stroke started.
    float m_terrainFlattenHeight = 0.f;
    bool m_terrainFlattenSampled = false;
    int m_terrainObjectCount = 1;
    bool m_terrainObjectRandomYaw = true;
    float m_terrainObjectScaleMin = 1.f;
    float m_terrainObjectScaleMax = 1.f;
    uint32_t m_terrainRandom = 0x9e3779b9u;
    bool m_terrainObjectStrokeReported = false;
    // A toolbar setting that edits terrain data (paint detail) is reported
    // by the next viewport frame as a one-frame edit, i.e. one undo step.
    bool m_terrainSettingChanged = false;
    int m_terrainObjectDisplayLod = -1;
    int m_sceneRenderMode = 0;
    int m_terrainLayer = 0;
    float m_terrainRadius = 3.f;
    float m_terrainStrength = .25f;
    bool m_editTerrainCells = false;
    std::string m_terrainObjectPrefab;
    // Objects tool: the object layer painted into (and thinned); empty: the
    // layer already holding the prefab, else "Objects".
    std::string m_terrainObjectLayer;
    bool m_terrainAlignObjects = true;
    // Sculpt brushes: tilt surface-aligned placements to the reshaped ground.
    bool m_terrainRealignObjects = false;
    float m_terrainObjectSpacing = 2.f;
    glm::vec3 m_lastTerrainObjectPosition{0.f};
    bool m_terrainObjectStroke = false;
    // True while a sculpt/paint stroke or cell edit is changing terrain, so
    // the edit is reported like a gizmo drag (undo + unsaved changes).
    bool m_terrainEditActive = false;
    // Runtime-only proxy standing in for a selected TerrainObjectLayer placement.
    Engine::Scene::Scene::ObjectId m_placementProxy = 0;
    void SynchronizePlacementProxy();
    // Editor-only visibility of terrain object layers (trees, grass, rocks...),
    // keyed by layer name so it covers every streamed cell. Applied only while
    // terrain tools are active; leaving them shows everything again.
    std::set<std::string> m_hiddenTerrainLayers;
    bool m_hideAllTerrainLayers = false;
    bool m_terrainLayersHiddenApplied = false;
    void ApplyTerrainLayerVisibility(Engine::Core::Object* terrainRoot);
    bool m_modelManipulation = false;
    int m_modelSelectionMode = 2;
    int m_modelAction = 0;
    bool m_modelScrollDepth = false;
    int m_modelScope = 1;
    EditorGizmoSystem m_gizmos;
    SceneViewportToolbar m_toolbar;
};
}

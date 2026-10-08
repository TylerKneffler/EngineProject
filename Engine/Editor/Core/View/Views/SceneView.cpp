
#include "SceneView.h"
#include "Scene/SceneCameraController.h"
#include "Scene/ScenePlacementAndPicking.h"
#include "Engine/Editor/Core/View/Templates/EditModes/Object/ObjectViewportTemplate.h"
#include "Engine/Editor/Core/View/Templates/EditModes/Mesh/MeshViewportTemplate.h"
#include "Engine/Editor/Core/View/Templates/EditModes/Skeleton/SkeletonViewportTemplate.h"
#include "Engine/Editor/Core/PrimitiveObjectFactory.h"
#include "Engine/Editor/Core/View/Templates/Common/AssetPathTemplate.h"
#include "Engine/Editor/UI/IEditorUi.h"
#include "Core/Scene/Scene.h"
#include "Core/Compoonents/Camera/Camera.h"
#include "Core/Compoonents/Obj/Sprite.h"
#include "Core/Compoonents/Sprite/SpriteAnimationManager.h"
#include "Core/Compoonents/Lighting/LightProbe.h"
#include "Core/Graphics/IGraphicsContext.h"
#include "Core/Graphics/IGraphicsProvider.h"
#include "../Focus/WindowFocusHandler.h"

namespace Engine::Editor
{


// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------
SceneView::SceneView()
{
    // Scene view keeps cursor visible (for editor UI interaction)
    SetCursorBehaviorOnFocus(CursorBehaviorOnFocus::Visible);
}

SceneView::~SceneView()
{
    CancelPrefabPreview();
}

// ---------------------------------------------------------------------------
// Init — store the scene pointer, then delegate resource creation to View.
// ---------------------------------------------------------------------------
void SceneView::Init(void* device,
                         uint32_t width, uint32_t height,
                         void* srvCpu,
                         void* srvGpu,
                         uint32_t srvSlotIndex,
                         Engine::Scene::Scene* scene,
                         const Engine::Model::ProjectSettings& settings)
{
    m_scene = scene;
    m_aspectRatioMode = settings.aspectRatioMode;
    m_gameAspectRatio = settings.gameAspectRatio;
    m_gameWindowWidth = settings.gameWindowWidth;
    m_gameWindowHeight = settings.gameWindowHeight;
    m_letterboxColor = settings.letterboxColor;
    
    View::Init(device, width, height, srvCpu, srvGpu, srvSlotIndex);
}

// ---------------------------------------------------------------------------
// DrawPanel
// ---------------------------------------------------------------------------
void SceneView::DrawPanel(IEditorUi& ui)
{
    if (m_focusOnNextDraw)
    {
        ui.FocusWindow(m_title.c_str());
        m_focusOnNextDraw = false;
    }

    const bool mode2D = m_scene && m_scene->IsEditorMode2D();
    SetClearColor(mode2D ? 0.28f : 0.0f,
        mode2D ? 0.30f : 0.0f,
        mode2D ? 0.34f : 0.502f);

    // Remove inner padding so the texture fills the panel edge-to-edge.
    const bool windowVisible = ui.BeginWindow(m_title.c_str(), &m_open, true);
    SetPanelVisible(windowVisible);
    if (!m_documentPath.empty())
        ui.WindowTitleTooltip(m_documentPath.c_str());
    if (ui.IsWindowFocused() && OnFocused) OnFocused();
    if (!windowVisible)
    {
        CancelPrefabPreview();
        ui.EndWindow();
        return;
    }
    if (AllowObjectTransform && ui.DeleteShortcutPressed() &&
        OnDeleteSelectionRequested)
        OnDeleteSelectionRequested();

    float panDX = 0.f, panDY = 0.f;
    float orbitDX = 0.f, orbitDY = 0.f;
    float zoom = 0.f;
    float dolly = 0.f;

    const float targetAspect = m_aspectRatioMode == Engine::Model::ProjectSettings::AspectRatioMode::Free ? 0.f :
        (m_aspectRatioMode == Engine::Model::ProjectSettings::AspectRatioMode::Locked ? m_gameAspectRatio :
         static_cast<float>(m_gameWindowWidth) / static_cast<float>(m_gameWindowHeight));
    const auto input = ui.Viewport(GetUiTextureHandle(), targetAspect,
        {m_letterboxColor.r,m_letterboxColor.g,m_letterboxColor.b,m_letterboxColor.a});
    panDX = input.keyPanDX;
    panDY = input.keyPanDY;
    dolly = input.keyDolly;
    bool overlayConsumedClick = false;
    m_toolbar.SetSceneToolsVisible(!UseGlobalToolbar);
    switch (m_editMode)
    {
    case SceneEditMode::Mesh:
        overlayConsumedClick = MeshViewportTemplate::Draw(ui, input,
            m_scene, m_toolbar);
        break;
    case SceneEditMode::Skeleton:
        overlayConsumedClick = SkeletonViewportTemplate::Draw(ui, input,
            m_scene, m_toolbar, AllowObjectTransform);
        break;
    default:
        overlayConsumedClick = ObjectViewportTemplate::Draw(ui, input,
            m_scene, m_toolbar, AllowObjectTransform);
        break;
    }
    const EditorUiContextMenuResult createMenu = AllowObjectCreation
        ? ui.ContextMenu(this, "Create", nullptr, true)
        : EditorUiContextMenuResult{};
    if (m_scene && (createMenu.addRequested ||
        !createMenu.primitive3D.empty() || createMenu.addSpriteRequested ||
        createMenu.addLightProbeRequested ||
        createMenu.addLightProbeGroupRequested))
    {
        Engine::Core::Object* created = nullptr;
        glm::vec3 placement(0.f);
        const bool hasPlacement = m_scene &&
            ScenePlacementAndPicking::FindPrefabPlacement(
                *m_scene, m_prefabPreview,
                input.mousePosInViewport, input.available, placement);
        try
        {
            if (!createMenu.primitive3D.empty())
                created = CreatePrimitiveObject(*m_scene, createMenu.primitive3D);
            else if (createMenu.addSpriteRequested)
            {
                created = m_scene->AddObject("Sprite");
                Engine::Components::SpriteAnimationManager* manager =
                    created->AddComponent<Engine::Components::SpriteAnimationManager>();
                Engine::Components::Sprite* sprite =
                    created->AddComponent<Engine::Components::Sprite>();
                sprite->SetAnimationManager(manager);
            }
            else if(createMenu.addLightProbeRequested)
            {
                created=m_scene->AddObject("Light Probe");
                created->AddComponent<Engine::Components::LightProbe>();
            }
            else if(createMenu.addLightProbeGroupRequested)
            {
                created=m_scene->AddObject("Light Probe Volume");
                created->AddComponent<Engine::Components::LightProbeGroup>();
            }
            else
                created = m_scene->AddObject("GameObject");
        }
        catch (const std::exception& error)
        {
            if (created)
            {
                m_scene->RemoveObject(created);
                created = nullptr;
            }
            OutputDebugStringA(("[SceneView] Create object failed: " +
                std::string(error.what()) + "\n").c_str());
        }
        if (created)
        {
            if (hasPlacement)
                created->transform.position = placement;
            if (OnObjectCreated)
                OnObjectCreated(created);
        }
    }
    EditorUiViewportInput gizmoInput = input;
    const EditorTransformTool transformTool = m_toolbar.GetTransformTool();
    if (overlayConsumedClick || transformTool == EditorTransformTool::Hand)
        gizmoInput.leftClicked = false;
    m_gizmos.SetBoneEditing(m_editMode == SceneEditMode::Skeleton &&
        AllowObjectTransform);
    const EditorGizmoResult gizmoResult = m_scene && AllowObjectTransform
        ? m_gizmos.DrawAndHandle(*m_scene, ui, gizmoInput, transformTool)
        : EditorGizmoResult{};
    if (OnGizmoInteraction)
        OnGizmoInteraction(gizmoResult.transformDragging ||
            m_toolbar.IsTransformDragging());
    if (gizmoResult.selectionRequested && OnObjectSelected &&
        (!CanSelectObject || CanSelectObject(gizmoResult.selectedObject)))
        OnObjectSelected(gizmoResult.selectedObject);
    bool prefabDragObserved = false;
    if (AllowAssetDrops && ui.BeginDragDropTarget())
    {
        const EditorUiDragDropPayloadResult payload =
            ui.InspectDragDropPayload("ENGINE_ASSET_PATH");
        if (payload.data && payload.size > 0)
        {
            const char* bytes = static_cast<const char*>(payload.data);
            size_t length = 0;
            while (length < payload.size && bytes[length] != '\0')
                ++length;
            const std::string path(bytes, length);
            const std::string extension =
                Engine::Editor::LowerAssetExtension(path);
            if (extension == ".prefab")
            {
                prefabDragObserved = true;
                if ((!m_prefabPreview || m_prefabPreviewPath != path) &&
                    OnAssetPreviewRequested)
                {
                    CancelPrefabPreview();
                    m_prefabPreview = OnAssetPreviewRequested(path);
                    m_prefabPreviewPath = m_prefabPreview ? path : std::string{};
                    m_prefabPreviewHasPlacement = false;
                    if (m_scene)
                        m_scene->SetPreviewObject(m_prefabPreview);
                }
                glm::vec3 placement{};
                if (m_prefabPreview && m_scene &&
                    ScenePlacementAndPicking::FindPrefabPlacement(
                        *m_scene, m_prefabPreview,
                        input.mousePosInViewport, input.available, placement))
                {
                    m_prefabPreview->transform.position = placement;
                    m_prefabPreviewHasPlacement = true;
                }
                if (payload.delivered && m_prefabPreview &&
                    m_prefabPreviewHasPlacement)
                {
                    Engine::Core::Object* placed = m_prefabPreview;
                    const std::string placedPath = m_prefabPreviewPath;
                    m_prefabPreview = nullptr;
                    m_prefabPreviewPath.clear();
                    m_prefabPreviewHasPlacement = false;
                    if (m_scene)
                        m_scene->SetPreviewObject(nullptr);
                    if (OnAssetPreviewCommitted)
                        OnAssetPreviewCommitted(placed, placedPath);
                }
            }
            else if (AllowAssetDrops && payload.delivered && OnAssetDropped)
                OnAssetDropped(path);
        }
        ui.EndDragDropTarget();
    }
    if (!prefabDragObserved)
        CancelPrefabPreview();
    bool selectSceneObject = false;
    EditorUiVec2 size = input.available;
    if (size.x > 0.f && size.y > 0.f)
    {
        // Update computed aspect ratio
        m_aspect = size.x / size.y;

        // Calculate game viewport with aspect ratio constraints
        EditorUiVec2 viewportSize = size, viewportPos{};

        // Draw letterbox/pillarbox background (if needed)
        if (viewportSize.x < size.x || viewportSize.y < size.y)
        {
            // Draw background in letterbox color
        }

        // Position cursor at viewport location and draw the game texture

        // Check for mouse input on the viewport
        if (input.hovered || input.viewportDragActive ||
            input.rightDown || input.middleDown ||
            input.zoomDragDown)
        {
            if (input.leftClicked && !overlayConsumedClick &&
                transformTool != EditorTransformTool::Hand &&
                !gizmoResult.consumedClick &&
                !input.rightDown && !input.middleDown && OnObjectSelected)
                selectSceneObject = true;

            EditorUiVec2 d = input.mouseDelta;

            // Right-button drag → pan.
            if (input.rightDown)
            {
                panDX += d.x;
                panDY += d.y;
            }

            // Middle-button drag → orbit.
            if (input.middleDown)
            {
                orbitDX = d.x;
                orbitDY = d.y;
            }

            // Hand mode grabs the view plane directly with the left mouse.
            if (transformTool == EditorTransformTool::Hand &&
                input.leftDown && !overlayConsumedClick)
            {
                panDX += d.x;
                panDY += d.y;
            }

            // Scroll wheel → zoom.
            zoom = input.mouseWheel;
            // Alt+Ctrl+left drag provides a trackpad-friendly dolly gesture.
            if (input.zoomDragDown)
                zoom += -d.y * 0.05f;
        }
    }

    const bool documentToolsConsumedClick = !UseGlobalToolbar && OnDrawDocumentTools
        ? OnDrawDocumentTools(ui) : false;
    EditorUiViewportInput meshInput = input;
    if (documentToolsConsumedClick || overlayConsumedClick)
        meshInput.leftClicked = false;
    const bool meshConsumedClick = OnMeshViewportInput
        ? OnMeshViewportInput(ui, meshInput, transformTool) : false;
    if (selectSceneObject && !documentToolsConsumedClick &&
        !meshConsumedClick && AllowObjectTransform)
        OnObjectSelected(ScenePlacementAndPicking::PickObjectInViewport(
            *m_scene, input.mousePosInViewport, input.available));
    ui.EndWindow();

    if (m_scene)
        SceneCameraController::Apply(*m_scene,
            panDX, panDY, orbitDX, orbitDY, zoom, dolly);
}

void SceneView::CancelPrefabPreview()
{
    if (m_scene)
        m_scene->SetPreviewObject(nullptr);
    if (m_prefabPreview && OnAssetPreviewCancelled)
        OnAssetPreviewCancelled(m_prefabPreview);
    m_prefabPreview = nullptr;
    m_prefabPreviewPath.clear();
    m_prefabPreviewHasPlacement = false;
}

// ---------------------------------------------------------------------------
// Render3D
// ---------------------------------------------------------------------------
void SceneView::Render3D(void* cmd)
{
    if (m_scene)
    {
        m_scene->UpdateEditorCameraFollow();
        auto* factory = m_scene->GetGraphicsProvider()->GetContextFactory();
        factory->SetCommandBuffer(cmd);
        auto ctx = factory->CreateContext();
        m_scene->Render(ctx.get(), m_aspect, nullptr, true,
            GetWidth(), GetHeight());
    }
}

void SceneView::RenderShadow3D(void* cmd)
{
    if (!m_scene || !m_scene->GetGraphicsProvider()) return;
    auto* factory = m_scene->GetGraphicsProvider()->GetContextFactory();
    factory->SetCommandBuffer(cmd);
    auto ctx = factory->CreateContext();
    if (ctx)
        m_scene->Render(ctx.get(), m_aspect, nullptr, false,
            GetWidth(), GetHeight(), true);
}
}

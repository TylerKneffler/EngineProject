
#include "SceneView.h"
#include "Core/Component.h"
#include "Editor/Core/View/EditorViewportSettings.h"
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
#include "Core/Compoonents/Animation/AnimationBone.h"
#include "Core/Compoonents/Animation/Model.h"
#include "Core/Compoonents/Animation/AnimationManager.h"
#include "Core/Compoonents/Obj/Mesh.h"
#include "Core/Compoonents/Obj/Terrain.h"
#include "Core/Compoonents/Obj/TerrainManager.h"
#include <set>
#include "Core/Compoonents/Camera/Camera.h"
#include "Core/Compoonents/Transform.h"
#include "Core/Compoonents/Obj/Sprite.h"
#include "Core/Compoonents/Particles/ParticleEmitter.h"
#include "Core/Compoonents/Sprite/SpriteAnimationManager.h"
#include "Core/Compoonents/Lighting/LightProbe.h"
#include "Core/Graphics/IGraphicsContext.h"
#include "Core/Graphics/IGraphicsProvider.h"
#include "../Focus/WindowFocusHandler.h"
#include "imgui.h"
#include <algorithm>
#include <glm/gtc/matrix_inverse.hpp>

namespace Engine::Editor
{
namespace
{
Engine::Core::Object* AssetDropTarget(Engine::Core::Object* hovered,
    const std::string& extension)
{
    if (!hovered) return nullptr;
    if (extension == ".material" || extension == ".mat")
        return hovered->GetComponent<Engine::Components::Mesh>() ? hovered : nullptr;
    if (extension == ".anim" || extension == ".animation" ||
        extension == ".animpack")
    {
        // The visible mesh can be several levels below its controller.
        for (auto* current = hovered; current; current = current->Parent)
            if (current->GetComponent<Engine::Components::AnimationManager>())
                return current;
        for (auto* current = hovered; current; current = current->Parent)
            if (current->GetComponent<Engine::Components::Model>())
                return current;
    }
    return nullptr;
}

void DrawAssetDropHighlight(IEditorUi& ui, const Engine::Scene::Scene& scene,
    const Engine::Core::Object& target, const EditorUiVec2& viewport)
{
    const auto* camera = scene.editorCamera.GetComponent<Engine::Components::Camera>();
    if (!camera || viewport.x <= 1.f || viewport.y <= 1.f) return;
    const glm::mat4 vp = camera->GetProjectionMatrix(viewport.x / viewport.y) *
        camera->GetViewMatrix();
    const EditorUiColor color{ 1.f, .75f, .12f, 1.f };
    for (const auto& object : scene.GetObjects())
    {
        const auto* current = object.get();
        bool descendant = false;
        for (auto* ancestor = current; ancestor; ancestor = ancestor->Parent)
            if (ancestor == &target) { descendant = true; break; }
        if (!descendant) continue;
        const auto* mesh = current->GetComponent<Engine::Components::Mesh>();
        if (!mesh || !mesh->HasBounds()) continue;
        const glm::vec3 minimum = mesh->GetBoundsMin();
        const glm::vec3 maximum = mesh->GetBoundsMax();
        const glm::mat4 world = current->transform.GetWorldMatrix();
        EditorUiVec2 points[8]{};
        bool visible[8]{};
        for (int corner = 0; corner < 8; ++corner)
        {
            const glm::vec3 local{
                corner & 1 ? maximum.x : minimum.x,
                corner & 2 ? maximum.y : minimum.y,
                corner & 4 ? maximum.z : minimum.z };
            const glm::vec4 clip = vp * world * glm::vec4(local, 1.f);
            if (clip.w <= 0.f) continue;
            const glm::vec3 ndc = glm::vec3(clip) / clip.w;
            if (ndc.z < 0.f || ndc.z > 1.f) continue;
            points[corner] = { (ndc.x * .5f + .5f) * viewport.x,
                (.5f - ndc.y * .5f) * viewport.y };
            visible[corner] = true;
        }
        constexpr int edges[12][2] = {
            {0,1},{1,3},{3,2},{2,0}, {4,5},{5,7},{7,6},{6,4},
            {0,4},{1,5},{2,6},{3,7} };
        for (const auto& edge : edges)
            if (visible[edge[0]] && visible[edge[1]])
                ui.DrawViewportLine(points[edge[0]], points[edge[1]], color, 2.5f);
    }
}
}


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
    if (m_scene) EditorViewportSettings::Apply(*m_scene);
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
        m_viewportScreenMin = m_viewportScreenMax = {};
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

    if (OnDrawBottomPanel)
    {
        const float available = ImGui::GetContentRegionAvail().y;
        BottomPanelHeight = std::clamp(BottomPanelHeight, 130.f,
            std::max(130.f, available - 120.f));
        ImGui::BeginChild("##SceneViewportArea",
            { 0.f, std::max(100.f, available - BottomPanelHeight - 8.f) },
            false, ImGuiWindowFlags_NoScrollbar |
                ImGuiWindowFlags_NoScrollWithMouse);
    }

    const float targetAspect = m_aspectRatioMode == Engine::Model::ProjectSettings::AspectRatioMode::Free ? 0.f :
        (m_aspectRatioMode == Engine::Model::ProjectSettings::AspectRatioMode::Locked ? m_gameAspectRatio :
         static_cast<float>(m_gameWindowWidth) / static_cast<float>(m_gameWindowHeight));
    const auto input = ui.Viewport(GetUiTextureHandle(), targetAspect,
        {m_letterboxColor.r,m_letterboxColor.g,m_letterboxColor.b,m_letterboxColor.a});
    m_viewportScreenMin = input.screenMin;
    m_viewportScreenMax = input.screenMax;
    bool terrainConsumedClick = false;
    static bool editTerrainCells = false;
    auto* terrain = m_scene && m_scene->GetSelectedObject()
        ? m_scene->GetSelectedObject()->GetComponent<Engine::Components::Terrain>()
        : nullptr;
    auto* manager = m_scene && m_scene->GetSelectedObject()
        ? m_scene->GetSelectedObject()->GetComponent<Engine::Components::TerrainManager>()
        : nullptr;
    if (!manager && m_scene && m_scene->GetSelectedObject() &&
        m_scene->GetSelectedObject()->Parent)
        manager = m_scene->GetSelectedObject()->Parent->GetComponent<Engine::Components::TerrainManager>();
    if (terrain || manager)
    {
        static int brush = 0, layer = 0;
        static float radius = 3.f, strength = .25f;
        ImGui::SetNextWindowPos({input.screenMin.x + 8.f, input.screenMin.y + 48.f}, ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(.92f);
        ImGui::Begin(("Terrain tools##" + m_title).c_str(), nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings);
        const char* modes[] = {"Raise", "Lower", "Average", "Blend", "Paint"};
        ImGui::Combo("Brush", &brush, modes, 5);
        ImGui::DragFloat("Radius", &radius, .05f, .05f, 100.f);
        ImGui::DragFloat("Strength", &strength, .01f, .001f, 2.f);
        if (brush == 4 && terrain)
        {
            layer = std::clamp(layer, 0, static_cast<int>(terrain->layers.size()) - 1);
            ImGui::SliderInt("Layer", &layer, 0,
                std::max(0, static_cast<int>(terrain->layers.size()) - 1));
        }
        terrainConsumedClick = ImGui::IsWindowHovered();
        ImGui::End();
        if (input.hovered && input.leftDown && !terrainConsumedClick &&
            !(manager && editTerrainCells) &&
            input.available.x > 0.f && input.available.y > 0.f)
        {
            auto* camera = m_scene->editorCamera.GetComponent<Engine::Components::Camera>();
            if (camera)
            {
                const glm::mat4 inverse = glm::inverse(camera->GetProjectionMatrix(
                    input.available.x / input.available.y) * camera->GetViewMatrix());
                const float nx = 2.f * input.mousePosInViewport.x / input.available.x - 1.f;
                const float ny = 1.f - 2.f * input.mousePosInViewport.y / input.available.y;
                glm::vec4 nearPoint = inverse * glm::vec4(nx, ny, 0.f, 1.f);
                glm::vec4 farPoint = inverse * glm::vec4(nx, ny, 1.f, 1.f);
                nearPoint /= nearPoint.w; farPoint /= farPoint.w;
                const glm::mat4 worldToLocal = glm::inverse(
                    (manager ? manager->Owner : m_scene->GetSelectedObject())->transform.GetWorldMatrix());
                const glm::vec3 a = glm::vec3(worldToLocal * nearPoint);
                const glm::vec3 b = glm::vec3(worldToLocal * farPoint);
                if (manager)
                {
                    const float dy = b.y - a.y;
                    if (std::abs(dy) > .00001f)
                    {
                        const float t = -a.y / dy;
                        if (t >= 0.f && t <= 1.f)
                        {
                            const glm::vec3 hit = a + (b - a) * t;
                            terrainConsumedClick = manager->ApplyBrush(hit.x, hit.z,
                                radius, strength * ImGui::GetIO().DeltaTime * 30.f,
                                static_cast<Engine::Components::Terrain::Brush>(brush), layer);
                        }
                    }
                }
                else terrainConsumedClick = terrain->ApplyBrushFromLocalRay(a, b,
                    radius, strength * ImGui::GetIO().DeltaTime * 30.f,
                    static_cast<Engine::Components::Terrain::Brush>(brush), layer);
            }
        }
    }
    if (manager && input.available.x > 0.f && input.available.y > 0.f)
    {
        ImGui::SetNextWindowPos({input.screenMin.x + 8.f, input.screenMin.y + 170.f}, ImGuiCond_Always);
        ImGui::Begin(("Terrain cells##" + m_title).c_str(), nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize |
            ImGuiWindowFlags_NoSavedSettings);
        ImGui::Checkbox("Edit terrain cells", &editTerrainCells);
        ImGui::End();
        if (editTerrainCells)
        {
            auto* camera = m_scene->editorCamera.GetComponent<Engine::Components::Camera>();
            if (camera)
            {
                const glm::mat4 viewProjection = camera->GetProjectionMatrix(
                    input.available.x / input.available.y) * camera->GetViewMatrix() *
                    manager->Owner->transform.GetWorldMatrix();
                auto cells = manager->GetCells();
                if (cells.empty()) cells.push_back({0, 0});
                std::set<std::pair<int, int>> candidates(cells.begin(), cells.end());
                for (const auto& cell : cells)
                    for (const auto offset : {std::pair{-1, 0}, {1, 0}, {0, -1}, {0, 1}})
                        candidates.insert({cell.first + offset.first, cell.second + offset.second});
                for (const auto& cell : candidates)
                {
                    const float x = cell.first * manager->cellsX * manager->cellWidth;
                    const float z = cell.second * manager->cellsY * manager->cellDepth;
                    const float w = manager->cellsX * manager->cellWidth;
                    const float h = manager->cellsY * manager->cellDepth;
                    ImVec2 corners[4];
                    bool visible = true;
                    for (int i = 0; i < 4; ++i)
                    {
                        const glm::vec4 clip = viewProjection * glm::vec4(
                            x + ((i == 1 || i == 2) ? w : 0.f), 0.f,
                            z + (i >= 2 ? h : 0.f), 1.f);
                        if (clip.w <= 0.f) { visible = false; break; }
                        corners[i] = {input.screenMin.x + (clip.x / clip.w + 1.f) * input.available.x * .5f,
                            input.screenMin.y + (1.f - clip.y / clip.w) * input.available.y * .5f};
                    }
                    if (!visible) continue;
                    const bool exists = manager->HasCell(cell.first, cell.second);
                    ImGui::GetForegroundDrawList()->AddQuad(corners[0], corners[1], corners[2], corners[3],
                        exists ? IM_COL32(255, 95, 70, 220) : IM_COL32(70, 220, 120, 220), 3.f);
                    const ImVec2 minimum{std::min(corners[0].x, corners[2].x), std::min(corners[0].y, corners[2].y)};
                    const ImVec2 maximum{std::max(corners[0].x, corners[2].x), std::max(corners[0].y, corners[2].y)};
                    if (input.hovered && ImGui::IsMouseClicked(0) &&
                        ImGui::IsMouseHoveringRect(minimum, maximum))
                    {
                        if (exists) manager->RemoveCell(cell.first, cell.second);
                        else manager->AddCell(cell.first, cell.second);
                        terrainConsumedClick = true;
                        break;
                    }
                }
            }
        }
    }
    bool modelHeaderConsumedClick = false;
    if (input.available.x > (UseGlobalToolbar ? 685.f : 450.f))
    {
        ImGui::SetNextWindowPos({ input.screenMin.x +
            (UseGlobalToolbar ? 246.f : 8.f), input.screenMin.y + 6.f },
            ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(.92f);
        ImGui::Begin(("Model Manipulator##" + m_title).c_str(), nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoScrollbar |
            ImGuiWindowFlags_AlwaysAutoResize);
        ImGui::Checkbox("Pose", &m_modelManipulation);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Drag a limb in the active camera view. Hold Shift to move in depth.");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90.f);
        const char* selections[] = { "Mesh", "Bones", "Hybrid", "Points" };
        ImGui::Combo("##PoseSelection", &m_modelSelectionMode,
            selections, 4);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Blue: move joint. Green: aim segment. Pink: twist segment. Ctrl-click blue points for direct grouped movement.");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(75.f);
        const char* actions[] = { "Move", "Rotate" };
        ImGui::Combo("##PoseAction", &m_modelAction, actions, 2);
        ImGui::SameLine();
        ImGui::Checkbox("Scroll depth", &m_modelScrollDepth);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("While dragging, scroll up to pull toward the camera or down to push away.");
        if (m_modelManipulation)
        {
            const char* scopes[] = { "Selected", "Body Part", "Full Body" };
            ImGui::SetNextItemWidth(110.f);
            ImGui::Combo("Scope", &m_modelScope, scopes, 3);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Selected moves one joint; Body Part solves a limb; Full Body solves the hierarchy with pinned effectors.");
            auto* selected = m_scene ? m_scene->GetSelectedObject() : nullptr;
            if (selected && selected->GetComponent<Engine::Components::AnimationBone>())
            {
                auto* controller = selected->GetComponent<
                    Engine::Components::AnimationBone>();
                bool controllerActive = m_gizmos.IsModelControllerActive(selected);
                ImGui::SameLine();
                if (ImGui::Checkbox("Active", &controllerActive))
                {
                    m_gizmos.SetModelControllerActive(selected, controllerActive);
                    if (OnModelControllerEdited) OnModelControllerEdited();
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Active controllers retain a soft target during Full Body posing. Fixed pins remain hard targets.");
                bool pinPosition = m_gizmos.IsModelPinned(selected, false);
                bool pinRotation = m_gizmos.IsModelPinned(selected, true);
                ImGui::SameLine();
                ImGui::BeginDisabled(!selected->Parent ||
                    !selected->Parent->Parent ||
                    !selected->Parent->GetComponent<Engine::Components::AnimationBone>() ||
                    !selected->Parent->Parent->GetComponent<Engine::Components::AnimationBone>());
                if (ImGui::Checkbox("Pin position", &pinPosition))
                {
                    m_gizmos.SetModelPin(selected, false, pinPosition);
                    if (OnModelControllerEdited) OnModelControllerEdited();
                }
                ImGui::EndDisabled();
                ImGui::SameLine();
                if (ImGui::Checkbox("Pin rotation", &pinRotation))
                {
                    m_gizmos.SetModelPin(selected, true, pinRotation);
                    if (OnModelControllerEdited) OnModelControllerEdited();
                }
                ImGui::SameLine();
                if (ImGui::Button("Controller settings"))
                    ImGui::OpenPopup("##ControllerSettings");
                if (ImGui::BeginPopup("##ControllerSettings"))
                {
                    bool changed = false;
                    const char* modes[] = {"Follow scope", "IK", "FK"};
                    changed |= ImGui::Combo("Solve mode",
                        &controller->poseControllerMode, modes, 3);
                    int scopeChoice = controller->poseControllerScope + 1;
                    const char* controllerScopes[] = {"Follow viewport", "Selected",
                        "Body Part", "Full Body"};
                    if (ImGui::Combo("Hierarchy scope", &scopeChoice,
                            controllerScopes, 4))
                    {
                        controller->poseControllerScope = scopeChoice - 1;
                        changed = true;
                    }
                    changed |= ImGui::SliderInt("Chain ancestors",
                        &controller->poseControllerChainDepth, 1, 32);
                    if (ImGui::Checkbox("Face target",
                            &controller->poseFacingEnabled))
                    {
                        if (controller->poseFacingEnabled)
                        {
                            const glm::vec3 axis = glm::dot(
                                controller->poseFacingAxis,
                                controller->poseFacingAxis) > 1e-8f
                                ? glm::normalize(controller->poseFacingAxis)
                                : glm::vec3(0.f, 0.f, 1.f);
                            controller->poseFacingTarget =
                                selected->transform.GetWorldPosition() +
                                glm::vec3(selected->transform.GetWorldMatrix() *
                                    glm::vec4(axis, 0.f));
                        }
                        changed = true;
                    }
                    changed |= ImGui::DragFloat3("Facing target (world)",
                        &controller->poseFacingTarget.x, .01f);
                    changed |= ImGui::DragFloat3("Local forward axis",
                        &controller->poseFacingAxis.x, .01f);
                    changed |= ImGui::SliderFloat("Facing weight",
                        &controller->poseFacingWeight, 0.f, 1.f);
                    ImGui::Text("Connection length: %.1f%% of rest",
                        controller->PoseLengthRatio() * 100.f);
                    if (ImGui::Button("Capture current length"))
                    {
                        controller->CapturePoseRestOffset();
                        changed = true;
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Restore rest length") &&
                        controller->RestorePoseLength())
                    {
                        selected->transform.NotifyEditorTransformChanged(
                            Engine::Components::Transform::EditorPosition);
                        changed = true;
                    }
                    if (ImGui::Button("Aim now") &&
                        m_gizmos.ApplyModelFacing(selected))
                        changed = true;
                    ImGui::TextWrapped("FK moves this point directly. IK solves "
                        "its ancestor chain. Body Part uses Chain ancestors; "
                        "Full Body also respects other active points and pins.");
                    if (changed)
                    {
                        controller->MarkConfigurationDirty();
                        if (OnModelControllerEdited)
                            OnModelControllerEdited();
                    }
                    ImGui::EndPopup();
                }
            }
            if (selected && m_scene && AllowObjectCreation)
            {
                Engine::Components::Model* model = nullptr;
                for (auto* ancestor = selected; ancestor;
                    ancestor = ancestor->Parent)
                    if ((model = ancestor->GetComponent<
                            Engine::Components::Model>())) break;
                ImGui::SameLine();
                if (ImGui::Button("Add control point"))
                {
                    auto* point = m_scene->AddObject("Control Point");
                    if (m_scene->MoveObject(point, selected,
                            Engine::Scene::Scene::ObjectPlacement::AsChild))
                    {
                        point->transform.position = {0.f, .15f, 0.f};
                        point->transform.rotation = {0.f, 0.f, 0.f};
                        point->transform.scale = {1.f, 1.f, 1.f};
                        const unsigned node = model
                            ? static_cast<unsigned>(model->GetNodeCount())
                            : static_cast<unsigned>(-1);
                        if (model)
                            model->BindNode(node, point);
                        auto* controller = point->AddComponent<
                            Engine::Components::AnimationBone>();
                        if (const auto* parentBone = selected->GetComponent<
                                Engine::Components::AnimationBone>())
                            controller->skinIndex = parentBone->skinIndex;
                        controller->nodeIndex = node;
                        controller->poseControlOnly = true;
                        controller->poseControllerTarget =
                            point->transform.GetWorldPosition();
                        m_scene->SetSelectedObject(point);
                        if (OnObjectCreated) OnObjectCreated(point);
                    }
                    else m_scene->RemoveObject(point);
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip(model
                        ? "Add a model-bound point without skinning weights. "
                          "Attach it to a hand, finger, prop, or another node."
                        : "Add a scene-only point under this object. "
                          "Model-bound points can also be keyed in animations.");
            }
            ImGui::SameLine();
            if (ImGui::Button("Clear pins") && m_scene)
            {
                m_gizmos.ClearModelPins(*m_scene);
                if (OnModelControllerEdited) OnModelControllerEdited();
            }
            if (OnKeyModelControllersRequested)
            {
                ImGui::SameLine();
                if (ImGui::Button("Key controllers"))
                    OnKeyModelControllersRequested();
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Key active controllers and fixed pins at the animation playhead.");
            }
        }
        modelHeaderConsumedClick = ImGui::IsWindowHovered() &&
            ImGui::IsMouseClicked(ImGuiMouseButton_Left);
        ImGui::End();
    }
    panDX = input.keyPanDX;
    panDY = input.keyPanDY;
    dolly = input.keyDolly;
    bool overlayConsumedClick = false;
    switch (m_editMode)
    {
    case SceneEditMode::Mesh:
        overlayConsumedClick = MeshViewportTemplate::Draw(ui, input,
            m_scene, m_toolbar);
        break;
    case SceneEditMode::Skeleton:
        overlayConsumedClick = SkeletonViewportTemplate::Draw(ui, input,
            m_scene, m_toolbar);
        break;
    default:
        overlayConsumedClick = ObjectViewportTemplate::Draw(ui, input,
            m_scene, m_toolbar, AllowObjectTransform);
        break;
    }
    if (OnDrawRigControls)
        overlayConsumedClick = OnDrawRigControls(ui, input) || overlayConsumedClick;
    const EditorUiContextMenuResult createMenu = AllowObjectCreation
        ? ui.ContextMenu(this, "Create", nullptr, true)
        : EditorUiContextMenuResult{};
    if (m_scene && (createMenu.addRequested ||
        !createMenu.primitive3D.empty() || createMenu.addSpriteRequested ||
        createMenu.addParticleEmitterRequested ||
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
            else if (createMenu.addParticleEmitterRequested)
            {
                created = m_scene->AddObject("Particle Emitter");
                created->AddComponent<Engine::Components::ParticleEmitter>();
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
    if (overlayConsumedClick || modelHeaderConsumedClick || terrainConsumedClick ||
        (transformTool == EditorTransformTool::Hand && !m_modelManipulation))
        gizmoInput.leftClicked = false;
    gizmoInput.modelDepthDown = ImGui::GetIO().KeyShift;
    gizmoInput.modelGroupSelectDown = ImGui::GetIO().KeyCtrl;
    m_gizmos.SetModelManipulation(m_modelManipulation,
        m_modelSelectionMode, m_modelAction == 1);
    m_gizmos.SetModelScrollDepth(m_modelScrollDepth);
    m_gizmos.SetModelScope(m_modelScope);
    m_gizmos.SetModelPickFilter(CanSelectObject);
    m_gizmos.SetBoneEditing(m_editMode == SceneEditMode::Skeleton &&
        AllowObjectTransform);
    const EditorGizmoResult gizmoResult = m_scene && AllowObjectTransform
        ? m_gizmos.DrawAndHandle(*m_scene, ui, gizmoInput,
            m_modelManipulation ? EditorTransformTool::Hand : transformTool)
        : EditorGizmoResult{};
    if (gizmoResult.selectionRequested && OnObjectSelected &&
        (!CanSelectObject || CanSelectObject(gizmoResult.selectedObject)))
        OnObjectSelected(gizmoResult.selectedObject);
    if (OnGizmoInteraction)
        OnGizmoInteraction(gizmoResult.transformDragging ||
            m_toolbar.IsTransformDragging());
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
            else
            {
                Engine::Core::Object* target = nullptr;
                if (m_scene && (extension == ".material" || extension == ".mat" ||
                    extension == ".anim" || extension == ".animation" ||
                    extension == ".animpack"))
                {
                    auto* hovered = ScenePlacementAndPicking::PickObjectInViewport(
                        *m_scene, input.mousePosInViewport, input.available, true);
                    target = AssetDropTarget(hovered, extension);
                    if (target)
                    {
                        DrawAssetDropHighlight(ui, *m_scene, *target, input.available);
                        ui.DrawViewportText({ input.mousePosInViewport.x + 14.f,
                            input.mousePosInViewport.y + 18.f }, target->name.c_str(),
                            { 1.f, .75f, .12f, 1.f });
                    }
                }
                if (payload.delivered)
                {
                    if (target && OnAssetAssigned)
                        OnAssetAssigned(path, target);
                    else if (OnAssetDropped)
                        OnAssetDropped(path);
                }
            }
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
                !modelHeaderConsumedClick &&
                transformTool != EditorTransformTool::Hand &&
                !m_modelManipulation &&
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
            zoom = m_modelManipulation && m_modelScrollDepth &&
                m_gizmos.IsModelDragging() ? 0.f : input.mouseWheel;
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
    {
        Engine::Core::Object* bone = Engine::Core::Component::HasEditorReferencePick()
            ? ScenePlacementAndPicking::PickBoneInViewport(*m_scene,
                input.mousePosInViewport, input.available) : nullptr;
        OnObjectSelected(bone ? bone :
            ScenePlacementAndPicking::PickObjectInViewport(*m_scene,
                input.mousePosInViewport, input.available));
    }
    if (OnDrawBottomPanel)
    {
        ImGui::EndChild();
        ImGui::InvisibleButton("##AnimationPanelSplitter", { -1.f, 7.f });
        if (ImGui::IsItemHovered() || ImGui::IsItemActive())
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
        if (ImGui::IsItemActive())
            BottomPanelHeight -= ImGui::GetIO().MouseDelta.y;
        ImGui::BeginChild("##SceneBottomPanel", { 0.f, 0.f }, false);
        OnDrawBottomPanel(ui);
        ImGui::EndChild();
    }
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

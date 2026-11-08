
#include "SceneView.h"
#include "Core/Graphics/GraphicsCommandPacket.h"
#include "Core/Component.h"
#include "Core/Rendering/RenderJobs.h"
#include "Editor/Core/View/EditorViewportSettings.h"
#include "Scene/SceneCameraController.h"
#include "Scene/ScenePlacementAndPicking.h"
#include "Scene/SurfacePlacement.h"
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
#include "Core/Compoonents/Obj/TerrainObjectLayer.h"
#include "Core/Compoonents/Obj/TerrainManager.h"
#include <set>
#include <filesystem>
#include <vector>
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
#include <cmath>
#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/quaternion.hpp>

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
    ScenePlacementAndPicking::ClearCaches();
}

// ---------------------------------------------------------------------------
// Init — store the scene pointer, then delegate resource creation to View.
// ---------------------------------------------------------------------------
void SceneView::SetScene(Engine::Scene::Scene* scene)
{
    if (m_scene == scene) return;
    CancelPrefabPreview();
    ScenePlacementAndPicking::ClearCaches();
    m_previousSelectedObject = nullptr;
    // Gestures and model pins contain pointers into the previous graph.
    m_gizmos.ResetInteraction();
    const auto tool = m_toolbar.GetTransformTool();
    m_toolbar = SceneViewportToolbar{};
    m_toolbar.SetTransformTool(tool);
    m_scene = scene;
}

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
// Pose controls shared by the main scene and animation toolbars.
// ---------------------------------------------------------------------------
void SceneView::DrawPoseToolbar(const std::function<bool(float)>& next,
    bool animationEditor, bool inlineOptions)
{
    if (m_scene && m_scene->IsEditorMode2D()) return;
    ImGui::PushID(this);
    auto* selected = m_scene ? m_scene->GetSelectedObject() : nullptr;
    auto* controller = selected
        ? selected->GetComponent<Engine::Components::AnimationBone>() : nullptr;
    if (!animationEditor && next(ImGui::CalcTextSize("Pose").x +
            ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x))
        ImGui::Checkbox("Pose", &m_modelManipulation);
    if (next(130.f))
    {
        ImGui::SetNextItemWidth(130.f);
        const char* scopes[]{"Scope: Selected", "Scope: Body Part", "Scope: Full Body"};
        int scope = controller && controller->poseControllerScope >= 0
            ? controller->poseControllerScope : m_modelScope;
        if (ImGui::Combo("##PoseScope", &scope, scopes, 3))
        {
            m_modelScope = scope;
            if (controller && controller->poseControllerScope >= 0)
            {
                controller->poseControllerScope = scope;
                controller->MarkConfigurationDirty();
                if (OnModelControllerEdited) OnModelControllerEdited();
            }
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Selected moves one joint; Body Part solves a limb; Full Body respects active controllers and pins.");
    }
    if (next(105.f))
    {
        ImGui::BeginDisabled(!controller);
        ImGui::SetNextItemWidth(105.f);
        const char* modes[]{"Solve: Auto", "Solve: IK", "Solve: FK"};
        int mode = controller ? controller->poseControllerMode : 0;
        if (ImGui::Combo("##PoseSolve", &mode, modes, 3) && controller)
        {
            controller->poseControllerMode = mode;
            controller->MarkConfigurationDirty();
            if (OnModelControllerEdited) OnModelControllerEdited();
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip(controller ? "Solve mode for the selected bone. Auto follows the viewport scope."
                : "Select a bone to choose IK or FK.");
    }
    if (next(100.f))
    {
        ImGui::SetNextItemWidth(100.f);
        const char* selections[]{"Select: Mesh", "Select: Bones", "Select: Hybrid", "Select: Points"};
        ImGui::Combo("##PoseSelection", &m_modelSelectionMode, selections, 4);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Choose what can be picked in the viewport.");
    }
    if (next(80.f))
    {
        ImGui::SetNextItemWidth(80.f);
        const char* actions[]{"Move", "Rotate"};
        ImGui::Combo("##PoseAction", &m_modelAction, actions, 2);
    }
    if (!inlineOptions && next(100.f) && ImGui::Button("Pose options"))
        ImGui::OpenPopup("##PoseSettings");
    const bool optionsPopup = !inlineOptions && ImGui::BeginPopup("##PoseSettings");
    if (inlineOptions || optionsPopup)
    {
        if (animationEditor) ImGui::Checkbox("Enable posing", &m_modelManipulation);
        ImGui::Checkbox("Scroll adjusts depth", &m_modelScrollDepth);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("While dragging, scroll to pull toward the camera or push away.");
        if (selected && selected->GetComponent<Engine::Components::AnimationBone>())
        {
            bool controllerActive = m_gizmos.IsModelControllerActive(selected);
            if (ImGui::Checkbox("Active", &controllerActive))
            {
                m_gizmos.SetModelControllerActive(selected, controllerActive);
                if (OnModelControllerEdited) OnModelControllerEdited();
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Active controllers retain a soft target during Full Body posing. Fixed pins remain hard targets.");
            bool pinPosition = m_gizmos.IsModelPinned(selected, false);
            bool pinRotation = m_gizmos.IsModelPinned(selected, true);
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
            if (ImGui::Checkbox("Pin rotation", &pinRotation))
            {
                m_gizmos.SetModelPin(selected, true, pinRotation);
                if (OnModelControllerEdited) OnModelControllerEdited();
            }
            if (ImGui::CollapsingHeader("Controller details"))
            {
                bool changed = false;
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
            }
        }
        if (selected && m_scene && AllowObjectCreation)
        {
            Engine::Components::Model* model = nullptr;
            for (auto* ancestor = selected; ancestor;
                ancestor = ancestor->Parent)
                if ((model = ancestor->GetComponent<
                        Engine::Components::Model>())) break;
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
                    auto* newController = point->AddComponent<
                        Engine::Components::AnimationBone>();
                    if (const auto* parentBone = selected->GetComponent<
                            Engine::Components::AnimationBone>())
                        newController->skinIndex = parentBone->skinIndex;
                    newController->nodeIndex = node;
                    newController->poseControlOnly = true;
                    newController->poseControllerTarget =
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
        if (ImGui::Button("Clear pins") && m_scene)
        {
            m_gizmos.ClearModelPins(*m_scene);
            if (OnModelControllerEdited) OnModelControllerEdited();
        }
        if (OnKeyModelControllersRequested)
        {
            if (ImGui::Button("Key controllers"))
                OnKeyModelControllersRequested();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Key active controllers and fixed pins at the animation playhead.");
        }
        if (optionsPopup) ImGui::EndPopup();
    }
    ImGui::PopID();
}

void SceneView::DrawTerrainToolbar(const std::function<bool(float)>& next)
{
    if (m_editMode != SceneEditMode::Terrain || !m_scene) return;
    auto* selected = m_scene->GetSelectedObject();
    auto* terrain = selected
        ? selected->GetComponent<Engine::Components::Terrain>() : nullptr;
    Engine::Components::TerrainManager* manager = nullptr;
    for (auto* object = selected; object && !manager; object = object->Parent)
        manager = object->GetComponent<Engine::Components::TerrainManager>();

    ImGui::PushID(this);
    if (!terrain && !manager)
    {
        if (next(225.f))
            ImGui::TextDisabled("Select Terrain or Terrain Manager");
        ImGui::PopID();
        return;
    }
    const char* modes[] = {"Raise", "Lower", "Average", "Blend", "Paint", "Objects"};
    if (next(112.f))
    {
        ImGui::SetNextItemWidth(112.f);
        ImGui::Combo("##TerrainBrush", &m_terrainBrush, modes, 6);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Terrain brush");
    }
    if (m_terrainBrush != 5 && next(120.f))
    {
        ImGui::SetNextItemWidth(120.f);
        ImGui::DragFloat("##TerrainRadius", &m_terrainRadius,
            .05f, .05f, 100.f, "Radius %.1f");
    }
    if (m_terrainBrush != 5 && next(135.f))
    {
        ImGui::SetNextItemWidth(135.f);
        ImGui::DragFloat("##TerrainStrength", &m_terrainStrength,
            .01f, .001f, 2.f, "Strength %.2f");
    }
    if (m_terrainBrush == 4 && terrain)
    {
        m_terrainLayer = std::clamp(m_terrainLayer, 0,
            std::max(0, static_cast<int>(terrain->layers.size()) - 1));
        if (next(100.f))
        {
            ImGui::SetNextItemWidth(100.f);
            ImGui::SliderInt("##TerrainLayer", &m_terrainLayer, 0,
                std::max(0, static_cast<int>(terrain->layers.size()) - 1),
                "Layer %d");
        }
    }
    if (m_terrainBrush == 5)
    {
        if (next(210.f))
        {
            const std::string label = m_terrainObjectPrefab.empty()
                ? "Drop prefab here##ObjectBrush" :
                std::filesystem::path(m_terrainObjectPrefab).filename().string() + "##ObjectBrush";
            ImGui::Button(label.c_str(), {210.f, 0.f});
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Drag a prefab from Assets here, then drag on terrain to paint objects.");
            if (ImGui::BeginDragDropTarget())
            {
                if (const auto* payload = ImGui::AcceptDragDropPayload("ENGINE_ASSET_PATH"))
                {
                    const auto* data = static_cast<const char*>(payload->Data);
                    size_t length = 0;
                    while (length < static_cast<size_t>(payload->DataSize) && data[length]) ++length;
                    const std::string path(data, length);
                    if (LowerAssetExtension(path) == ".prefab") m_terrainObjectPrefab = path;
                }
                ImGui::EndDragDropTarget();
            }
        }
        if (next(145.f)) ImGui::Checkbox("Align to surface", &m_terrainAlignObjects);
        if (next(135.f))
        {
            ImGui::SetNextItemWidth(135.f);
            ImGui::DragFloat("##ObjectSpacing", &m_terrainObjectSpacing,
                .05f, .1f, 100.f, "Spacing %.1f");
        }
    }
    if (m_terrainBrush != 5 && manager && next(125.f))
        ImGui::Checkbox("Edit cells", &m_editTerrainCells);
    ImGui::PopID();
}

void SceneView::DrawPanel(IEditorUi& ui)
{
    if (m_scene && UseGlobalBoneVisibility) EditorViewportSettings::Apply(*m_scene);
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
    const bool windowVisible = ui.BeginViewportWindow(m_title.c_str(), &m_open);
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
    if (m_scene && (input.hovered || input.viewportDragActive))
        ScenePlacementAndPicking::PrepareSkinnedGeometry(*m_scene);
    RequestRenderSize(input.available.x, input.available.y);
    m_viewportScreenMin = input.screenMin;
    m_viewportScreenMax = input.screenMax;
    if (m_scene && !m_scene->HasStarted())
    {
        const glm::dvec3 viewer =
            m_scene->editorCamera.transform.GetWorldPositionD();
        std::vector<Engine::Components::TerrainManager*> terrainWorlds;
        for (const auto& object : m_scene->GetObjects())
            if (object)
                if (auto* world = object->GetComponent<Engine::Components::TerrainManager>();
                    world && world->asyncStreaming)
                    terrainWorlds.push_back(world);
        for (auto* world : terrainWorlds)
        {
            const glm::dvec3 local = glm::dvec3(
                glm::inverse(world->Owner->transform.GetWorldMatrixD()) *
                glm::dvec4(viewer, 1.0));
            world->UpdateStreaming(
                static_cast<int>(std::floor(local.x /
                    (world->cellsX * world->cellWidth))),
                static_cast<int>(std::floor(local.z /
                    (world->cellsY * world->cellDepth))));
        }
    }
    bool terrainConsumedClick = false;
    auto* selected = m_scene ? m_scene->GetSelectedObject() : nullptr;
    auto* terrain = selected
        ? selected->GetComponent<Engine::Components::Terrain>() : nullptr;
    Engine::Components::TerrainManager* manager = nullptr;
    for (auto* object = selected; object && !manager; object = object->Parent)
        manager = object->GetComponent<Engine::Components::TerrainManager>();
    if (selected != m_previousSelectedObject)
    {
        if ((terrain || manager) && m_editMode == SceneEditMode::Object)
            m_editMode = SceneEditMode::Terrain;
        m_previousSelectedObject = selected;
    }
    const bool objectBrush = (terrain || manager) &&
        m_editMode == SceneEditMode::Terrain && m_terrainBrush == 5;
    if (!objectBrush || !input.leftDown || !input.hovered ||
        input.rightDown || input.middleDown)
        m_terrainObjectStroke = false;
    if (objectBrush)
    {
        // Reserve the stroke even on a miss, so painting cannot select its own objects.
        terrainConsumedClick = input.hovered && (input.leftDown || input.leftClicked);
        glm::vec3 position{}, normal{};
        if (input.hovered && !input.rightDown && !input.middleDown &&
            ScenePlacementAndPicking::FindTerrainPlacement(*m_scene,
                manager ? manager->Owner : selected,
                input.mousePosInViewport, input.available, position, normal))
        {
            ui.DrawViewportCircle(input.mousePosInViewport, 6.f, {1.f, .8f, .15f, 1.f}, false, 2.f);
            const bool spaced = !m_terrainObjectStroke ||
                glm::distance(position, m_lastTerrainObjectPosition) >=
                    std::max(.1f, m_terrainObjectSpacing);
            if (AllowObjectCreation && input.leftDown && !m_terrainObjectPrefab.empty() && spaced)
            {
                auto* cell=manager ? nullptr:terrain;
                if (manager)
                    for (auto* candidate:m_scene->GetComponents<Engine::Components::Terrain>())
                    {
                        if (candidate->Owner->GetComponentInParent<Engine::Components::TerrainManager>()!=manager) continue;
                        const auto local=glm::vec3(glm::inverse(candidate->Owner->transform.GetWorldMatrix())*glm::vec4(position,1));
                        if (local.x>=0 && local.z>=0 && local.x<=candidate->cellsX*candidate->cellWidth &&
                            local.z<=candidate->cellsY*candidate->cellDepth) { cell=candidate; break; }
                    }
                if (cell)
                {
                    Engine::Components::TerrainObjectLayer* layer=nullptr;
                    for (auto* child:cell->Owner->Children)
                        if (auto* candidate=child->GetComponent<Engine::Components::TerrainObjectLayer>();
                            candidate && (candidate->layerName=="Objects" ||
                            (!candidate->placements.empty() && LowerAssetExtension(candidate->placements.front().asset)==".prefab")))
                        { layer=candidate; break; }
                    if (!layer)
                    {
                        auto* child=m_scene->AddObject("Objects");
                        m_scene->MoveObject(child,cell->Owner,Engine::Scene::Scene::ObjectPlacement::AsChild);
                        child->transform.position={0,0,0}; child->transform.rotation={0,0,0};
                        child->transform.scale={1,1,1};
                        layer=child->AddComponent<Engine::Components::TerrainObjectLayer>();
                    }
                    Engine::Components::TerrainObjectLayer::Placement placement;
                    placement.asset=m_terrainObjectPrefab;
                    const auto inverse=glm::inverse(layer->Owner->transform.GetWorldMatrix());
                    placement.position=glm::vec3(inverse*glm::vec4(position,1));
                    placement.normal=m_terrainAlignObjects ? glm::normalize(glm::vec3(inverse*glm::vec4(normal,0))):glm::vec3(0,1,0);
                    layer->placements.push_back(placement); layer->MarkConfigurationDirty();
                    if (OnObjectCreated) OnObjectCreated(selected);
                    // Keep terrain selected for the rest of the brush stroke.
                    m_scene->SetSelectedObject(selected);
                    if (OnObjectSelected) OnObjectSelected(selected);
                    m_lastTerrainObjectPosition = position;
                    m_terrainObjectStroke = true;
                }
            }
        }
    }
    if ((terrain || manager) && m_editMode == SceneEditMode::Terrain && !objectBrush)
    {
        if (input.hovered && input.leftDown &&
            !(manager && m_editTerrainCells) &&
            input.available.x > 0.f && input.available.y > 0.f)
        {
            auto* camera = m_scene->editorCamera.GetComponent<Engine::Components::Camera>();
            if (camera)
            {
                const glm::dmat4 inverse = glm::inverse(camera->GetProjectionMatrixD(
                    static_cast<double>(input.available.x) / input.available.y) *
                    camera->GetViewMatrixD());
                const double nx = 2.0 * input.mousePosInViewport.x / input.available.x - 1.0;
                const double ny = 1.0 - 2.0 * input.mousePosInViewport.y / input.available.y;
                glm::dvec4 nearPoint = inverse * glm::dvec4(nx, ny, 0.0, 1.0);
                glm::dvec4 farPoint = inverse * glm::dvec4(nx, ny, 1.0, 1.0);
                nearPoint /= nearPoint.w; farPoint /= farPoint.w;
                const glm::dmat4 worldToLocal = glm::inverse(
                    (manager ? manager->Owner : m_scene->GetSelectedObject())->transform.GetWorldMatrixD());
                const glm::dvec3 a = glm::dvec3(worldToLocal * nearPoint);
                const glm::dvec3 b = glm::dvec3(worldToLocal * farPoint);
                if (manager)
                {
                    const double dy = b.y - a.y;
                    if (std::abs(dy) > .00001f)
                    {
                        const double t = -a.y / dy;
                        if (t >= 0.f && t <= 1.f)
                        {
                            const glm::dvec3 hit = a + (b - a) * t;
                            terrainConsumedClick = manager->ApplyBrush(hit.x, hit.z,
                                m_terrainRadius, m_terrainStrength * ImGui::GetIO().DeltaTime * 30.f,
                                static_cast<Engine::Components::Terrain::Brush>(m_terrainBrush), m_terrainLayer);
                        }
                    }
                }
                else terrainConsumedClick = terrain->ApplyBrushFromLocalRay(
                    glm::vec3(a), glm::vec3(b),
                    m_terrainRadius, m_terrainStrength * ImGui::GetIO().DeltaTime * 30.f,
                    static_cast<Engine::Components::Terrain::Brush>(m_terrainBrush), m_terrainLayer);
            }
        }
    }
    if (manager && m_editMode == SceneEditMode::Terrain && !objectBrush &&
        input.available.x > 0.f && input.available.y > 0.f)
    {
        if (m_editTerrainCells)
        {
            auto* camera = m_scene->editorCamera.GetComponent<Engine::Components::Camera>();
            if (camera)
            {
                const glm::dmat4 viewProjection = camera->GetProjectionMatrixD(
                    static_cast<double>(input.available.x) / input.available.y) *
                    camera->GetViewMatrixD() *
                    manager->Owner->transform.GetWorldMatrixD();
                auto cells = manager->GetCells();
                if (cells.empty()) cells.push_back({0, 0});
                std::set<std::pair<int, int>> candidates(cells.begin(), cells.end());
                for (const auto& cell : cells)
                    for (const auto offset : {std::pair{-1, 0}, {1, 0}, {0, -1}, {0, 1}})
                        candidates.insert({cell.first + offset.first, cell.second + offset.second});
                for (const auto& cell : candidates)
                {
                    const double x = static_cast<double>(cell.first) * manager->cellsX * manager->cellWidth;
                    const double z = static_cast<double>(cell.second) * manager->cellsY * manager->cellDepth;
                    const double w = static_cast<double>(manager->cellsX) * manager->cellWidth;
                    const double h = static_cast<double>(manager->cellsY) * manager->cellDepth;
                    ImVec2 corners[4];
                    bool visible = true;
                    for (int i = 0; i < 4; ++i)
                    {
                        const glm::dvec4 clip = viewProjection * glm::dvec4(
                            x + ((i == 1 || i == 2) ? w : 0.0), 0.0,
                            z + (i >= 2 ? h : 0.0), 1.0);
                        if (clip.w <= 0.f) { visible = false; break; }
                        corners[i] = {static_cast<float>(input.screenMin.x +
                                (clip.x / clip.w + 1.0) * input.available.x * .5),
                            static_cast<float>(input.screenMin.y +
                                (1.0 - clip.y / clip.w) * input.available.y * .5)};
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
    if (OnDrawRigControls && m_scene && m_scene->AreEditorBonesVisible())
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
                Engine::Components::SpriteAnimationManager* animationManager =
                    created->AddComponent<Engine::Components::SpriteAnimationManager>();
                Engine::Components::Sprite* sprite =
                    created->AddComponent<Engine::Components::Sprite>();
                sprite->SetAnimationManager(animationManager);
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
    if (overlayConsumedClick || terrainConsumedClick ||
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
    if (m_prefabDropPending && ImGui::IsKeyPressed(ImGuiKey_Escape)) CancelPrefabPreview();
    if (m_prefabDropPending) PollPrefabPreparation();
    if (m_prefabDropPending)
        ui.DrawViewportText({14.f, 18.f}, "Loading placed prefab...", {1.f, .85f, .3f, 1.f});
    if (!m_prefabPreparationError.empty())
        ui.DrawViewportText({14.f, 40.f}, m_prefabPreparationError.c_str(), {1.f, .4f, .3f, 1.f});
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
                if (m_prefabPreviewPath != path &&
                    (OnPreparedAssetPreviewRequested || OnAssetPreviewRequested))
                {
                    CancelPrefabPreview();
                    m_prefabPreviewPath = path;
                    m_prefabPreparationError.clear();
                    m_prefabPreparationGeneration = m_scene ? m_scene->CaptureSceneGeneration() : 0;
                    m_prefabPreparationCanceled = std::make_shared<std::atomic_bool>(false);
                    m_prefabPreparation = Engine::Rendering::RenderJobs::Get().Submit(
                        [path, canceled = m_prefabPreparationCanceled] {
                            return Engine::Serialization::PreparedPrefab::Load(path, canceled);
                        });
                }
                PollPrefabPreparation();
                glm::vec3 placement{};
                if (!m_prefabDropPending && m_scene &&
                    ScenePlacementAndPicking::FindPrefabPlacement(
                        *m_scene, m_prefabPreview,
                        input.mousePosInViewport, input.available, placement))
                {
                    m_prefabPlacement = placement;
                    if (m_prefabPreview) m_prefabPreview->transform.position = placement;
                    m_prefabPreviewHasPlacement = true;
                }
                if (payload.delivered && m_prefabPreviewHasPlacement &&
                    (m_prefabPreview || m_prefabPreparation.valid()))
                {
                    m_prefabDropPending = true;
                    if (m_prefabPreview) CommitPrefabPreview();
                }
                if (m_prefabPreparation.valid())
                    ui.DrawViewportText({input.mousePosInViewport.x + 14.f,
                        input.mousePosInViewport.y + 18.f}, "Loading prefab...", {1.f, .85f, .3f, 1.f});

            }
            else
            {
                Engine::Core::Object* target = nullptr;
                if (m_scene && (extension == ".material" || extension == ".mat" ||
                    extension == ".anim" || extension == ".animation" ||
                    extension == ".animpack"))
                {
                    auto* hovered = payload.delivered
                        ? ScenePlacementAndPicking::PickObjectInViewport(
                            *m_scene, input.mousePosInViewport, input.available, true)
                        : ScenePlacementAndPicking::PickObjectHoverAsync(
                            *m_scene, input.mousePosInViewport, input.available);
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
    if (!prefabDragObserved && !m_prefabDropPending)
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
            if (input.leftClicked && !overlayConsumedClick && !terrainConsumedClick &&
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
        if (OnDocumentDrop) OnDocumentDrop(ui);
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
    if (OnDocumentDrop) OnDocumentDrop(ui);
    ui.EndWindow();

    if (m_scene)
    {
        SceneCameraController::Apply(*m_scene,
            panDX, panDY, orbitDX, orbitDY, zoom, dolly);
        m_scene->UpdateEditorCameraFollow();
        const glm::dvec3 cameraPosition =
            m_scene->editorCamera.transform.GetWorldPositionD();
        std::vector<Engine::Components::TerrainManager*> terrainManagers;
        for (const auto& object : m_scene->GetObjects())
            if (auto* terrainManager =
                    object->GetComponent<Engine::Components::TerrainManager>())
                terrainManagers.push_back(terrainManager);
        for (auto* terrainManager : terrainManagers)
            terrainManager->UpdateEditorStreaming(cameraPosition,
                m_scene->editorCamera.GetComponent<Engine::Components::Camera>(),
                static_cast<int>(GetHeight()),
                GetHeight() > 0.f ? GetWidth() / GetHeight() : 1.f);
    }
}

void SceneView::PollPrefabPreparation()
{
    if (m_scene && (m_prefabPreparation.valid() || m_prefabPreview) &&
        m_scene->CaptureSceneGeneration() != m_prefabPreparationGeneration)
    {
        // A reload already destroyed the previous scene's preview object.
        m_prefabPreview = nullptr;
        CancelPrefabPreview();
        return;
    }
    if (!m_prefabPreparation.valid() ||
        m_prefabPreparation.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
        return;
    try
    {
        auto prepared = m_prefabPreparation.get();
        if (!prepared || !m_scene) return;
        m_prefabPreview = OnPreparedAssetPreviewRequested
            ? OnPreparedAssetPreviewRequested(m_scene, *prepared)
            : OnAssetPreviewRequested ? OnAssetPreviewRequested(prepared->path) : nullptr;
        if (!m_prefabPreview) throw std::runtime_error("Could not create prefab: " + prepared->path);
        if (m_prefabPreviewHasPlacement)
            m_prefabPreview->transform.position = m_prefabPlacement;
        m_scene->SetPreviewObject(m_prefabPreview);
        if (m_prefabDropPending) CommitPrefabPreview();
    }
    catch (const std::exception& error)
    {
        m_prefabPreparationError = error.what();
        m_prefabDropPending = false;
    }
}

void SceneView::CommitPrefabPreview()
{
    auto* placed = m_prefabPreview;
    const auto path = m_prefabPreviewPath;
    m_prefabPreview = nullptr;
    m_prefabPreviewPath.clear();
    m_prefabPreviewHasPlacement = false;
    m_prefabDropPending = false;
    m_prefabPreparationCanceled.reset();
    if (m_scene) m_scene->SetPreviewObject(nullptr);
    if (placed && OnAssetPreviewCommitted) OnAssetPreviewCommitted(placed, path);
}

void SceneView::CancelPrefabPreview()
{
    if (m_prefabPreparationCanceled) m_prefabPreparationCanceled->store(true);
    m_prefabPreparation = {};
    m_prefabPreparationCanceled.reset();
    m_prefabDropPending = false;
    if (m_scene)
        m_scene->SetPreviewObject(nullptr);
    if (m_prefabPreview && OnAssetPreviewCancelled && m_scene &&
        m_scene->CaptureSceneGeneration() == m_prefabPreparationGeneration)
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

void SceneView::RecordRenderCommands(Engine::Graphics::GraphicsCommandPacket& shadow,
    Engine::Graphics::GraphicsCommandPacket& color,
    const Engine::Scene::Scene::NativeRenderPacket* packet)
{
    if (!m_scene) return;
    m_scene->UpdateEditorCameraFollow();
    m_scene->PrepareNativeUiLayout(m_aspect);
    m_scene->Render(&shadow, m_aspect, nullptr, false, GetWidth(), GetHeight(), true, packet);
    m_scene->Render(&color, m_aspect, nullptr, true, GetWidth(), GetHeight(), false, packet);
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

float Engine::Editor::SceneView::EffectiveRenderScale() const
{
    return m_scene ? std::clamp(m_scene->settings.renderScale * m_scene->settings.graphicsQuality.Resolve().renderScale, .5f, 1.f) : 1.f;
}

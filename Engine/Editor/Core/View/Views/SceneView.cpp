#include "Engine/Editor/State/EditorViewState.h"
#include "Core/Rendering/RenderSystem.h"
#include "Core/Rendering/SceneRenderer.h"

#include "Core/Inspection/ComponentInspector.h"
#include "SceneView.h"
#include <chrono>
#include "Core/Graphics/GraphicsCommandPacket.h"
#include "Core/Component.h"
#include "Core/Rendering/RenderJobs.h"
#include "Editor/Core/View/EditorViewportSettings.h"
#include "Scene/SceneCameraController.h"
#include "Scene/ScenePlacementAndPicking.h"
#include "Scene/SceneTerrainTools.h"
#include "Scene/SurfacePlacement.h"
#include "Engine/Editor/Core/View/Templates/EditModes/Object/ObjectViewportTemplate.h"
#include "Engine/Editor/Core/View/Templates/EditModes/Mesh/MeshViewportTemplate.h"
#include "Engine/Editor/Core/View/Templates/EditModes/Skeleton/SkeletonViewportTemplate.h"
#include "Engine/Editor/Core/PrimitiveObjectFactory.h"
#include "Engine/Editor/Core/View/Templates/Common/AssetPathTemplate.h"
#include "Engine/Editor/UI/IEditorUi.h"
#include "Core/Scene/Scene.h"
#include "Core/Components/Camera/Camera.h"
#include "Core/Components/Animation/AnimationBone.h"
#include "Core/Components/Animation/Model.h"
#include "Core/Components/Animation/AnimationManager.h"
#include "Core/Components/Mesh/Mesh.h"
#include "Core/Components/Terrain/Terrain.h"
#include "Core/Components/Terrain/TerrainBrushShape.h"
#include "Core/Components/Terrain/TerrainPlacementBrush.h"
#include "Core/Components/Terrain/TerrainFoliageBrush.h"
#include "Engine/Editor/Core/Paint/PaintInput.h"
#include "Core/Components/Terrain/TerrainObjectLayer.h"
#include "Core/Components/Terrain/TerrainManager.h"
#include "Core/Components/Terrain/TerrainPlacementHandle.h"
#include <set>
#include <filesystem>
#include <vector>
#include "Core/Components/Camera/Camera.h"
#include "Core/Components/Transform.h"
#include "Core/Components/Sprite/Sprite.h"
#include "Core/Components/Particles/ParticleEmitter.h"
#include "Core/Components/Sprite/SpriteAnimationManager.h"
#include "Core/Components/Lighting/LightProbe.h"
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
// Init â€” store the scene pointer, then delegate resource creation to View.
// ---------------------------------------------------------------------------
void SceneView::SetScene(Engine::Scene::Scene* scene)
{
    if (m_scene == scene) return;
    CancelPrefabPreview();
    ScenePlacementAndPicking::ClearCaches();
    m_previousSelectedObject = nullptr;
    m_terrainObjectDisplayLod = -1;
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
    if (m_scene && m_scene->IsMode2D()) return;
    ImGui::PushID(this);
    auto* selected = m_scene ? Engine::Editor::EditorViewState::For(*m_scene).selectedObject : nullptr;
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
                    Engine::Editor::EditorViewState::For(*m_scene).SelectObject(point);
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

namespace
{
// Select-tool operations, in m_regionOperation order.
constexpr const char* kRegionOperations[] = {"Hydraulic erosion", "Thermal erosion", "Smooth",
    "Noise", "Terrace", "Flatten", "Raise / lower", "Paint layer", "Place objects"};
constexpr const char* kRegionShapes[] = {"Rectangle", "Ellipse", "Lasso", "Polygon"};
}

Engine::Components::TerrainRegionOperation::Kind SceneView::RegionOperationKind(int operation)
{
    using Kind = Engine::Components::TerrainRegionOperation::Kind;
    static constexpr Kind kinds[] = {Kind::Hydraulic, Kind::Thermal, Kind::Smooth, Kind::Noise,
        Kind::Terrace, Kind::Flatten, Kind::Raise, Kind::Raise};
    return kinds[std::clamp(operation, 0, int(IM_ARRAYSIZE(kinds)) - 1)];
}

void SceneView::DrawRegionOperationSettings(Engine::Components::TerrainRegionOperation& op,
    int operation, bool brush)
{
    ImGui::PushItemWidth(170.f);
    if (!brush && operation != kRegionPaint)
    {
        ImGui::SliderFloat("Blend", &op.strength, 0.f, 1.f, "%.2f");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("How much of the result replaces the terrain.");
    }
    switch (operation)
    {
    case 0:
        if (!brush)
        {
            ImGui::DragFloat("Droplets / m2", &op.dropletDensity, .01f, .01f, 20.f, "%.2f");
            ImGui::DragInt("Max droplets", &op.maxDroplets, 1000.f, 1000, 4000000);
        }
        ImGui::DragInt("Droplet lifetime", &op.maxSteps, .2f, 4, 400);
        ImGui::SliderFloat("Erosion rate", &op.erodeSpeed, .01f, 1.f, "%.2f");
        ImGui::SliderFloat("Deposition rate", &op.depositSpeed, .01f, 1.f, "%.2f");
        ImGui::DragFloat("Capacity", &op.capacity, .05f, .1f, 32.f, "%.2f");
        ImGui::SliderFloat("Inertia", &op.inertia, 0.f, .95f, "%.2f");
        ImGui::SliderFloat("Evaporation", &op.evaporation, .001f, .2f, "%.3f");
        ImGui::SliderInt("Erosion brush (samples)", &op.erosionRadius, 1, 8);
        ImGui::DragFloat("Gentleness", &op.erosionUnit, .5f, 1.f, 1000.f, "%.0f");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Height unit of the droplet rules, in grid spacings.\n"
                "Lower carves deeper channels per droplet; 60 suits mountain-scale relief,\n"
                "5-20 suits small hand-made hills.");
        break;
    case 1:
        ImGui::SliderFloat("Talus angle", &op.talusDegrees, 5.f, 80.f, "%.0f deg");
        if (!brush) ImGui::SliderInt("Iterations", &op.iterations, 1, 200);
        break;
    case 2:
        ImGui::SliderInt("Iterations", &op.iterations, 1, 200);
        break;
    case 3:
        ImGui::DragFloat("Scale (m)", &op.noiseScale, .1f, .5f, 4000.f, "%.1f");
        ImGui::DragFloat("Amplitude (m)", &op.noiseAmplitude, .05f, 0.f, 500.f, "%.2f");
        {
            int seed = int(op.seed);
            if (ImGui::DragInt("Seed", &seed, 1.f, 0, 1 << 20)) op.seed = uint32_t(std::max(0, seed));
        }
        break;
    case 4:
        ImGui::DragFloat("Step height (m)", &op.terraceStep, .05f, .05f, 500.f, "%.2f");
        ImGui::SliderFloat("Flatness", &op.terraceSharpness, 0.f, .95f, "%.2f");
        break;
    case 5:
        ImGui::DragFloat("Height", &op.height, .05f, -10000.f, 10000.f, "%.2f");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Ctrl+click the terrain to pick a height.");
        break;
    case 6:
        ImGui::DragFloat("Amount (m)", &op.amount, .05f, -10000.f, 10000.f, "%.2f");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Negative lowers.");
        break;
    case kRegionPaint:
        ImGui::SliderFloat("Opacity", &op.strength, 0.f, 1.f, "%.2f");
        ImGui::TextDisabled("Layer: toolbar combo. Shift+Apply erases it.");
        break;
    case kRegionPlace:
        ImGui::SliderFloat("Density share", &op.strength, 0.f, 1.f, "%.2f");
        ImGui::TextDisabled("The brush (a Place brush) and the Objects tool's layer.\n"
            "Shift+Apply thins its assets instead.");
        break;
    default: break;
    }
    ImGui::PopItemWidth();
}

void SceneView::DrawTerrainRegionToolbar(const std::function<bool(float)>& next)
{
    if (next(105.f))
    {
        ImGui::SetNextItemWidth(105.f);
        if (ImGui::Combo("##RegionShape", &m_regionShape, kRegionShapes, IM_ARRAYSIZE(kRegionShapes)))
            m_regionDrawing = false;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Selection shape.\n"
                "Rectangle / Ellipse: drag corner to corner.\n"
                "Lasso: drag to draw the outline.\n"
                "Polygon: click points; click the first point, double-click or Enter closes.\n"
                "Backspace removes the last point, Esc clears the selection.");
    }
    if (next(120.f))
    {
        ImGui::SetNextItemWidth(120.f);
        if (ImGui::DragFloat("##RegionFeather", &m_regionFeather, .1f, 0.f, 1000.f, "Feather %.1f"))
            m_regionMask.feather = std::max(0.f, m_regionFeather);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Soft edge inside the selection, in metres.");
    }
    if (next(150.f))
    {
        ImGui::SetNextItemWidth(150.f);
        ImGui::Combo("##RegionOperation", &m_regionOperation, kRegionOperations,
            IM_ARRAYSIZE(kRegionOperations));
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Operation applied to the selection by Apply.");
    }
    if (next(80.f))
    {
        if (ImGui::Button("Settings##RegionOperation")) ImGui::OpenPopup("RegionOperationSettings");
        if (ImGui::BeginPopup("RegionOperationSettings"))
        {
            DrawRegionOperationSettings(m_regionSettings, m_regionOperation, false);
            ImGui::EndPopup();
        }
    }
    const bool hasSelection = !m_regionMask.Empty();
    if (next(70.f))
    {
        ImGui::BeginDisabled(!hasSelection);
        if (ImGui::Button("Apply##Region")) m_regionApplyRequested = true;
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip(hasSelection ? "Run the operation on the selection (one undo step).\n"
                "Enter over the viewport does the same." : "Draw a selection on the terrain first.");
    }
    if (next(60.f))
    {
        ImGui::BeginDisabled(!hasSelection && !m_regionDrawing);
        if (ImGui::Button("Clear##Region")) ClearRegionSelection();
        ImGui::EndDisabled();
    }
    if (!m_regionStatus.empty() && next(170.f)) ImGui::TextDisabled("%s", m_regionStatus.c_str());
}

namespace
{
long long BrushFileStamp(const std::string& path)
{
    std::error_code error;
    const auto time = std::filesystem::last_write_time(Engine::Components::Mesh::ResolveFilePath(path), error);
    return error ? 0 : static_cast<long long>(time.time_since_epoch().count());
}
}

void SceneView::SelectTerrainBrush(const std::string& path, bool applySettings)
{
    m_terrainBrushPath = path;
    m_terrainBrushLoaded = false;
    m_terrainBrushError.clear();
    m_terrainBrushStamp = 0;
    if (path.empty()) return;
    Engine::Brushes::BrushPreset preset;
    if (!Engine::Brushes::LoadBrushPreset(Engine::Components::Mesh::ResolveFilePath(path), preset,
            m_terrainBrushError))
        return;
    m_terrainBrushPreset = std::move(preset);
    m_terrainBrushLoaded = true;
    m_terrainBrushStamp = BrushFileStamp(path);
    if (!applySettings) return;
    const auto& brush = m_terrainBrushPreset;
    m_terrainHardness = brush.hardness;
    m_terrainFalloff = brush.falloff;
    m_terrainShapeYawDegrees = brush.rotation;
    if (brush.relief.Present()) m_terrainStampBacked = brush.relief.backed;
    using Engine::Brushes::BrushTool;
    switch (brush.terrain.tool)
    {
    case BrushTool::Paint:
        m_terrainBrush = kPaintBrush;
        break;
    case BrushTool::Place:
        m_terrainBrush = kObjectBrush;
        break;
    case BrushTool::Foliage:
        m_terrainBrush = kPaintBrush;
        break;
    case BrushTool::Sculpt:
        if (brush.relief.Present()) m_terrainBrush = kStampTool;
        else if (m_terrainBrush > kFlattenBrush && m_terrainBrush != kStampTool) m_terrainBrush = 0;
        break;
    default: break;
    }
}

float SceneView::TerrainBrushPressureSize() const
{
    return m_terrainBrushLoaded ? Paint::PressureScale(m_terrainBrushPreset.pressureSize) : 1.f;
}

float SceneView::TerrainBrushPressureStrength() const
{
    return m_terrainBrushLoaded ? Paint::PressureScale(m_terrainBrushPreset.pressureStrength) : 1.f;
}

glm::dvec3 SceneView::ScatterTerrainDab(const glm::dvec3& local, float radius)
{
    if (!m_terrainBrushLoaded || m_terrainBrushPreset.scatter <= 0.f) return local;
    const auto next = [this] {
        m_terrainRandom ^= m_terrainRandom << 13; m_terrainRandom ^= m_terrainRandom >> 17;
        m_terrainRandom ^= m_terrainRandom << 5;
        return (m_terrainRandom & 0xffffffu) / double(0x1000000);
    };
    const double distance = m_terrainBrushPreset.scatter * radius * std::sqrt(next());
    const double angle = 6.283185307179586 * next();
    return {local.x + std::cos(angle) * distance, local.y, local.z + std::sin(angle) * distance};
}

void SceneView::DrawTerrainShapeToolbar(const std::function<bool(float)>& next)
{
    // Brushes (.brush) anywhere under Assets; any other can be dropped on the
    // combo. The Stamp tool lists those with a relief.
    const auto scanBrushes = [this]
    {
        m_terrainBrushAssets.clear();
        std::error_code error;
        const std::filesystem::path assets = Engine::Components::Mesh::ResolveFilePath("Assets");
        for (std::filesystem::recursive_directory_iterator it(assets, error), end;
             !error && it != end; it.increment(error))
            if (it->path().extension() == ".brush")
                m_terrainBrushAssets.push_back("Assets/" +
                    std::filesystem::relative(it->path(), assets, error).generic_string());
        std::sort(m_terrainBrushAssets.begin(), m_terrainBrushAssets.end());
    };
    const auto hasRelief = [](const std::string& path) {
        Engine::Brushes::BrushPreset preset;
        std::string error;
        return Engine::Brushes::LoadBrushPreset(Engine::Components::Mesh::ResolveFilePath(path), preset, error) &&
            preset.relief.Present();
    };
    // An edited brush (inspector, file) is read again; its settings stay.
    if (!m_terrainBrushPath.empty() && BrushFileStamp(m_terrainBrushPath) != m_terrainBrushStamp)
        SelectTerrainBrush(m_terrainBrushPath, false);
    const bool stampTool = m_terrainBrush == kStampTool;
    if (stampTool && !(m_terrainBrushLoaded && m_terrainBrushPreset.relief.Present()))
    {
        if (m_terrainBrushAssets.empty()) scanBrushes();
        for (const auto& path : m_terrainBrushAssets)
            if (hasRelief(path)) { SelectTerrainBrush(path, true); break; }
    }
    const auto brushName = [](const std::string& path)
    { return path.empty() ? std::string("Round") : std::filesystem::path(path).stem().string(); };
    if (next(170.f))
    {
        ImGui::SetNextItemWidth(170.f);
        if (ImGui::BeginCombo("##TerrainShape", ("Brush " + brushName(m_terrainBrushPath)).c_str()))
        {
            if (ImGui::IsWindowAppearing()) scanBrushes();
            if (!stampTool && ImGui::Selectable("Round", m_terrainBrushPath.empty()))
                SelectTerrainBrush({}, false);
            for (const auto& path : m_terrainBrushAssets)
            {
                if (stampTool && !hasRelief(path)) continue;
                if (ImGui::Selectable(brushName(path).c_str(), path == m_terrainBrushPath))
                    SelectTerrainBrush(path, true);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", path.c_str());
            }
            ImGui::EndCombo();
        }
        if (ImGui::BeginDragDropTarget())
        {
            if (const auto* payload = ImGui::AcceptDragDropPayload("ENGINE_ASSET_PATH"))
            {
                const auto* data = static_cast<const char*>(payload->Data);
                size_t length = 0;
                while (length < static_cast<size_t>(payload->DataSize) && data[length]) ++length;
                const std::string path(data, length);
                if (LowerAssetExtension(path) == ".brush") SelectTerrainBrush(path, true);
            }
            ImGui::EndDragDropTarget();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Terrain brush (.brush asset; drop one here). Its tip is the brush's\n"
                "outline, a relief is what Raise, Lower and Stamp build up, its rules limit\n"
                "where it acts (height, slope, paint layers), pen pressure and scatter vary\n"
                "each dab, and a Place brush puts down its assets with the Objects tool.\n"
                "Choosing one takes its hardness, falloff, rotation and tool.\n"
                "Edit brushes in the Inspector; Photoshop .abr packs import as brushes.");
    }
    if (!m_terrainBrushError.empty() && next(200.f))
        ImGui::TextColored({1.f, .4f, .35f, 1.f}, "%s", m_terrainBrushError.c_str());
    if (const auto* rules = TerrainBrushRules(); rules && next(60.f))
    {
        ImGui::TextDisabled("Rules");
        if (ImGui::IsItemHovered())
        {
            std::string text = "This brush acts only where its rules allow:";
            if (rules->minHeight > -1e30f || rules->maxHeight < 1e30f)
                text += "\nheight " + (rules->minHeight > -1e30f ? std::to_string(int(rules->minHeight)) : std::string("-")) +
                    " .. " + (rules->maxHeight < 1e30f ? std::to_string(int(rules->maxHeight)) : std::string("-")) + " m";
            if (rules->minSlope > 0.f || rules->maxSlope < 90.f)
                text += "\nslope " + std::to_string(int(rules->minSlope)) + " .. " +
                    std::to_string(int(rules->maxSlope)) + " degrees";
            const auto names = [](const std::vector<std::string>& layers) {
                std::string list;
                for (const auto& layer : layers)
                    list += (list.empty() ? "" : ", ") + std::filesystem::path(layer).stem().string();
                return list;
            };
            if (!rules->onLayers.empty()) text += "\non " + names(rules->onLayers);
            if (!rules->avoidLayers.empty()) text += "\noff " + names(rules->avoidLayers);
            ImGui::SetTooltip("%s", text.c_str());
        }
    }
    const auto shape = m_terrainBrushPath.empty() ? nullptr
        : Engine::Components::TerrainBrushShape::Load(m_terrainBrushPath);
    if (!shape)
    {
        // The round brush: its falloff past the hardness.
        if (next(90.f))
        {
            static constexpr const char* falloffs[] = {"Hard", "Linear", "Smooth"};
            ImGui::SetNextItemWidth(90.f);
            ImGui::Combo("##TerrainFalloff", &m_terrainFalloff, falloffs, IM_ARRAYSIZE(falloffs));
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Falloff past the hardness.");
        }
        return;
    }
    if (next(110.f))
    {
        ImGui::SetNextItemWidth(110.f);
        ImGui::DragFloat("##TerrainShapeYaw", &m_terrainShapeYawDegrees, 1.f, -360.f, 360.f,
            "Turn %.0f deg");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Turns the brush shape about the vertical.");
    }
    if (stampTool && next(120.f))
    {
        ImGui::SetNextItemWidth(120.f);
        ImGui::DragFloat("##StampHeight", &m_terrainStampHeightScale, .01f, .05f, 10.f, "Height x%.2f");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("A stroke grows the brush's relief up to its own surface, standing on the\n"
                "ground where the stroke began, and stops there. Its heights scale with the\n"
                "radius (the shape keeps its proportions); this multiplies them.");
    }
    if (stampTool && next(140.f))
    {
        ImGui::Checkbox("Against terrain", &m_terrainStampBacked);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("The relief builds only against terrain behind it (back from its face):\n"
                "each point rises at most to the ground behind it, so the part hanging past\n"
                "a wall's top or end, or lying on open ground, changes nothing.");
    }
}

void SceneView::DrawTerrainToolbar(const std::function<bool(float)>& next)
{
    if (m_editMode != SceneEditMode::Terrain || !m_scene) return;
    auto* selected = Engine::Editor::EditorViewState::For(*m_scene).selectedObject;
    auto* terrain = selected
        ? selected->GetComponent<Engine::Components::Terrain>() : nullptr;
    // Only a Terrain cell or a TerrainManager is a terrain-editing target. A
    // cell edits through its manager; objects placed on the terrain (trees,
    // grass batches, placement proxies) must not activate terrain tools.
    Engine::Components::TerrainManager* manager = selected
        ? selected->GetComponent<Engine::Components::TerrainManager>() : nullptr;
    for (auto* object = terrain ? selected->Parent : nullptr; object && !manager;
         object = object->Parent)
        manager = object->GetComponent<Engine::Components::TerrainManager>();

    ImGui::PushID(this);
    if (!terrain && !manager)
    {
        if (next(225.f))
            ImGui::TextDisabled("Select Terrain or Terrain Manager");
        ImGui::PopID();
        return;
    }
    const char* modes[] = {"Raise", "Lower", "Smooth", "Flatten", "Paint", "Objects",
        "Erode", "Weather", "Noise", "Terrace", "Select", "Stamp", "Rock", "Carve", "Ledge"};
    if (next(112.f))
    {
        ImGui::SetNextItemWidth(112.f);
        if (ImGui::Combo("##TerrainBrush", &m_terrainBrush, modes, IM_ARRAYSIZE(modes)))
            m_regionDrawing = false;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Terrain tool. Hold Shift to invert it while painting:\n"
                "Raise <-> Lower, Paint -> erase the layer, Objects -> erase objects,\n"
                "Noise -> negative noise.\n"
                "Flatten levels to the height where the stroke starts.\n"
                "Erode: hydraulic erosion under the brush (droplets carve and settle).\n"
                "Weather: thermal erosion, steep slopes slump to the talus angle.\n"
                "Select: draw a rectangle, ellipse, lasso or polygon, then Apply an\n"
                "operation (erosion, smoothing, paint...) to the whole region at once.\n"
                "Stamp: a raise brush shaped like a terrain stamp; holding it grows the\n"
                "stamp's shape out of the ground and stops at it (Shift lowers).\n"
                "Rock / Carve: sculpt the volume along the stroke (overhangs, arches,\n"
                "caves); Shift swaps them. Ledge: level to the height where the stroke\n"
                "began, cutting above and filling below. These need a Terrain Manager\n"
                "and are kept as volume edits (undo restores them).");
    }
    const bool selectTool = m_terrainBrush == kSelectTool;
    // Objects and Select's "Place objects": the brush and the object layer.
    const bool placesObjects = m_terrainBrush == kObjectBrush ||
        (selectTool && m_regionOperation == kRegionPlace);
    if (ShapedTerrainTool() || (selectTool && m_regionOperation == kRegionPlace)) DrawTerrainShapeToolbar(next);
    if (selectTool) DrawTerrainRegionToolbar(next);
    if (!selectTool && next(120.f))
    {
        ImGui::SetNextItemWidth(120.f);
        ImGui::DragFloat("##TerrainRadius", &m_terrainRadius,
            .05f, .05f, 100.f, "Radius %.1f");
    }
    const bool placing = m_terrainBrush == kObjectBrush && TerrainBrushPlaces();
    if ((m_terrainBrush != kObjectBrush || placing) && !selectTool && !VolumeTerrainTool() && next(135.f))
    {
        ImGui::SetNextItemWidth(135.f);
        ImGui::DragFloat("##TerrainStrength", &m_terrainStrength,
            .01f, .001f, placing ? 1.f : 2.f, "Strength %.2f");
        if (placing && ImGui::IsItemHovered())
            ImGui::SetTooltip("Share of the brush's density placed by each dab (and of the placements\n"
                "thinned when erasing with Shift).");
    }
    if ((m_terrainBrush != kObjectBrush || placing) && !selectTool &&
        !(ShapedTerrainTool() && !m_terrainBrushPath.empty() &&
          Engine::Components::TerrainBrushShape::Load(m_terrainBrushPath)) && next(130.f))
    {
        ImGui::SetNextItemWidth(130.f);
        ImGui::SliderFloat("##TerrainHardness", &m_terrainHardness, 0.f, 1.f, "Hardness %.2f");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Share of the radius at full strength; the rest fades out.");
    }
    if (m_terrainBrush >= kErodeBrush && m_terrainBrush <= kTerraceBrush && next(80.f))
    {
        if (ImGui::Button("Settings##BrushOperation")) ImGui::OpenPopup("BrushOperationSettings");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Settings of this brush");
        if (ImGui::BeginPopup("BrushOperationSettings"))
        {
            DrawRegionOperationSettings(m_brushOperation, BrushOperationKind(m_terrainBrush), true);
            ImGui::EndPopup();
        }
    }
    if (m_terrainBrush == kPaintBrush || (selectTool && m_regionOperation == kRegionPaint))
    {
        // Layer names come from the selected cell, or any loaded cell of the
        // manager (cells of one manager share their layer list).
        const Engine::Components::Terrain* layerSource = terrain;
        if (!layerSource && manager)
            for (const auto& cell : manager->GetCells())
                if ((layerSource = manager->GetLoadedCell(cell.first, cell.second))) break;
        const int layerCount = layerSource ? static_cast<int>(layerSource->layers.size()) : 0;
        m_terrainLayer = std::clamp(m_terrainLayer, 0, std::max(0, layerCount - 1));
        if (m_terrainBrush == kPaintBrush && TerrainBrushFoliage())
        {
            // A Foliage brush paints its own foliage layers.
            std::string layers;
            for (const auto& layer : m_terrainBrushPreset.terrain.foliage.layers)
                layers += (layers.empty() ? "" : ", ") + layer.layer;
            if (next(220.f))
            {
                ImGui::TextUnformatted(("Foliage: " + layers).c_str());
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("This brush paints where ground cover grows (any surface: ground, "
                        "cliff faces, ceilings). Shift erases. Edit its layers and amounts in "
                        "the brush's Foliage section.");
            }
        }
        else if (next(170.f))
        {
            const auto layerName = [&](int index) {
                if (!layerSource || index >= layerCount) return std::to_string(index);
                const auto& material = layerSource->layers[static_cast<size_t>(index)].material;
                return std::to_string(index) + "  " + (material.empty() ? std::string("(no material)")
                    : std::filesystem::path(material).stem().string());
            };
            ImGui::SetNextItemWidth(170.f);
            if (ImGui::BeginCombo("##TerrainLayer", layerName(m_terrainLayer).c_str()))
            {
                for (int index = 0; index < layerCount; ++index)
                {
                    const auto& color = layerSource->layers[static_cast<size_t>(index)].previewColor;
                    ImGui::ColorButton(("##LayerColor" + std::to_string(index)).c_str(),
                        {color.r, color.g, color.b, 1.f}, ImGuiColorEditFlags_NoTooltip, {14.f, 14.f});
                    ImGui::SameLine();
                    if (ImGui::Selectable(layerName(index).c_str(), index == m_terrainLayer))
                        m_terrainLayer = index;
                }
                ImGui::EndCombo();
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Paint layer (Shift erases it)");
        }
        // Paint detail: weight samples per cell edge. A single terrain is
        // resampled now; a manager's cells adopt it on their next stroke.
        if (next(130.f))
        {
            static constexpr int steps[] = {0, 2, 4, 8, 16};
            static constexpr const char* names[] = {"Detail: vertices", "Detail: 2 / cell",
                "Detail: 4 / cell", "Detail: 8 / cell", "Detail: 16 / cell"};
            const int current = manager ? manager->paintResolution
                : (terrain ? terrain->paintResolution : 0);
            int index = 0;
            while (index < 4 && steps[index] < current) ++index;
            ImGui::SetNextItemWidth(130.f);
            if (ImGui::Combo("##PaintDetail", &index, names, IM_ARRAYSIZE(names)) &&
                steps[index] != current)
            {
                if (manager) { manager->paintResolution = steps[index]; manager->MarkConfigurationDirty(); }
                else if (terrain) { terrain->SetPaintResolution(steps[index]); terrain->MarkConfigurationDirty(); }
                m_terrainSettingChanged = true;
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Paint resolution, independent of the mesh: weight samples per cell edge.\n"
                    "Manager cells adopt it the next time a stroke touches them\n"
                    "(procedural cells keep vertex paint).");
        }
    }
    if (placesObjects && next(170.f))
    {
        // The object layer to paint into: any the manager or the loaded
        // cells have, or a new name.
        std::set<std::string> names{"Objects"};
        if (manager) names.insert(manager->GetObjectLayerNames().begin(), manager->GetObjectLayerNames().end());
        std::vector<Engine::Core::Object*> pending{manager ? manager->Owner : selected};
        while (!pending.empty())
        {
            auto* object = pending.back(); pending.pop_back();
            if (!object) continue;
            if (auto* layer = object->GetComponent<Engine::Components::TerrainObjectLayer>())
            { if (!layer->conformToGround && !layer->snapToGround) names.insert(layer->layerName); continue; }
            for (auto* child : object->Children) pending.push_back(child);
        }
        const std::string label = "Layer " + (m_terrainObjectLayer.empty()
            ? std::string(placing ? "Objects" : "(prefab's)") : m_terrainObjectLayer);
        ImGui::SetNextItemWidth(170.f);
        if (ImGui::BeginCombo("##ObjectLayer", label.c_str()))
        {
            if (ImGui::Selectable(placing ? "Objects (default)" : "(the prefab's layer)", m_terrainObjectLayer.empty()))
                m_terrainObjectLayer.clear();
            for (const auto& name : names)
                if (ImGui::Selectable(name.c_str(), name == m_terrainObjectLayer)) m_terrainObjectLayer = name;
            ImGui::Separator();
            static char newLayer[96] = "";
            ImGui::SetNextItemWidth(140.f);
            if (ImGui::InputTextWithHint("##NewObjectLayer", "New layer", newLayer, sizeof(newLayer),
                    ImGuiInputTextFlags_EnterReturnsTrue) && newLayer[0])
            {
                m_terrainObjectLayer = newLayer;
                newLayer[0] = 0;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Object layer the brush places into and erases from (made in each cell\n"
                "when missing). Type a name and press Enter for a new one.");
    }
    if (placesObjects && next(70.f))
    {
        // How the chosen layer draws, for every cell's layer of that name.
        const std::string name = m_terrainObjectLayer.empty() ? std::string("Objects") : m_terrainObjectLayer;
        if (ImGui::Button("Layer...##ObjectLayerSettings")) ImGui::OpenPopup("ObjectLayerSettings");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Settings of the \"%s\" layer: view, detail and shadow distances.\n"
                "They apply to that layer in every cell, painted ones included.", name.c_str());
        if (ImGui::BeginPopup("ObjectLayerSettings"))
        {
            // Current: the manager's settings for the name, else a loaded
            // layer of that name, else the defaults.
            Engine::Components::TerrainObjectLayer::Settings settings;
            const Engine::Components::TerrainObjectLayer::Settings* stored =
                manager ? manager->GetObjectLayerSettings(name) : nullptr;
            if (stored) settings = *stored;
            else
            {
                std::vector<Engine::Core::Object*> pending{manager ? manager->Owner : selected};
                bool found = false;
                while (!pending.empty() && !found)
                {
                    auto* object = pending.back(); pending.pop_back();
                    if (!object) continue;
                    if (auto* layer = object->GetComponent<Engine::Components::TerrainObjectLayer>())
                    {
                        if (layer->layerName == name)
                        { settings = Engine::Components::TerrainObjectLayer::Settings::From(*layer); found = true; }
                        continue;
                    }
                    for (auto* child : object->Children) pending.push_back(child);
                }
            }
            ImGui::TextDisabled("%s", name.c_str());
            bool changed = false;
            ImGui::PushItemWidth(170.f);
            changed |= ImGui::DragFloat("View distance (m)", &settings.viewDistance, 1.f, 1.f, 100000.f, "%.0f");
            changed |= ImGui::DragFloat("Detail 1 from (m)", &settings.lod1Distance, .5f, 0.f, settings.viewDistance, "%.0f");
            changed |= ImGui::DragFloat("Detail 2 from (m)", &settings.lod2Distance, .5f, settings.lod1Distance,
                settings.viewDistance, "%.0f");
            changed |= ImGui::DragFloat("Shadows to (m)", &settings.shadowDistance, .5f, 0.f, settings.viewDistance, "%.0f");
            changed |= ImGui::DragFloat("Far shadows to (m)", &settings.farShadowDistance, 1.f, 0.f,
                settings.viewDistance, "%.0f");
            ImGui::PopItemWidth();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Beyond the shadow distance, the last detail level (a tree's impostor)\n"
                    "keeps casting out to this distance. 0: off.");
            if (changed)
            {
                if (manager) manager->SetObjectLayerSettings(name, settings);
                else if (terrain && terrain->Owner)
                    for (auto* child : terrain->Owner->Children)
                        if (auto* layer = child ? child->GetComponent<Engine::Components::TerrainObjectLayer>() : nullptr)
                            if (layer->layerName == name) settings.ApplyTo(*layer);
                m_terrainSettingChanged = true;
            }
            ImGui::EndPopup();
        }
    }
    if (placing && next(220.f))
    {
        const auto& place = m_terrainBrushPreset.terrain.placement;
        ImGui::TextDisabled("%zu asset%s, %.2f / m2, %.1f m apart", place.assets.size(),
            place.assets.size() == 1 ? "" : "s", place.density, place.minSpacing);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("The Place brush's placement (edit it in the brush's Inspector).\n"
                "Drag to place; Shift-drag thins its assets. Choose the Round brush\n"
                "to drop a single prefab instead.");
    }
    if (m_terrainBrush == kObjectBrush && !placing)
    {
        if (next(210.f))
        {
            const std::string label = m_terrainObjectPrefab.empty()
                ? "Drop prefab here##ObjectBrush" :
                std::filesystem::path(m_terrainObjectPrefab).filename().string() + "##ObjectBrush";
            ImGui::Button(label.c_str(), {210.f, 0.f});
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Drag a prefab from Assets here, then drag on terrain to paint objects.\n"
                    "Shift-drag erases this prefab (or, with no prefab, any painted object).\n"
                    "Right-click to clear.");
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) m_terrainObjectPrefab.clear();
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
        if (next(90.f))
        {
            ImGui::SetNextItemWidth(90.f);
            ImGui::DragInt("##ObjectCount", &m_terrainObjectCount, .1f, 1, 64, "Count %d");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Objects per dab, scattered inside the radius (1 = at the cursor).");
        }
        if (next(135.f))
        {
            ImGui::SetNextItemWidth(135.f);
            ImGui::DragFloat("##ObjectSpacing", &m_terrainObjectSpacing,
                .05f, .1f, 100.f, "Spacing %.1f");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Distance the cursor travels between dabs.");
        }
        if (next(150.f))
        {
            ImGui::SetNextItemWidth(150.f);
            ImGui::DragFloatRange2("##ObjectScale", &m_terrainObjectScaleMin, &m_terrainObjectScaleMax,
                .01f, .05f, 10.f, "Scale %.2f", "%.2f");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Random scale range for painted objects.");
        }
        if (next(110.f)) ImGui::Checkbox("Random yaw", &m_terrainObjectRandomYaw);
        if (next(145.f)) ImGui::Checkbox("Align to surface", &m_terrainAlignObjects);
    }
    if (m_terrainBrush != kObjectBrush && m_terrainBrush != kPaintBrush && next(135.f))
    {
        ImGui::Checkbox("Realign objects", &m_terrainRealignObjects);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Objects resting on the terrain always follow its height.\n"
                "When on, objects painted aligned to the surface also tilt to the new slope;\n"
                "when off, they keep the orientation they were painted with.");
    }
    if (m_terrainBrush != kObjectBrush && !selectTool && manager && next(125.f))
        ImGui::Checkbox("Edit cells", &m_editTerrainCells);
    if (next(110.f))
    {
        ImGui::Checkbox("Hide objects", &m_hideAllTerrainLayers);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Hide every object layer (trees, grass, rocks...) for a clean sculpting view.\nShortcut: H over the viewport.");
    }
    if (next(70.f))
    {
        const std::string label = m_hiddenTerrainLayers.empty()
            ? std::string("Layers##TerrainLayerVisibility")
            : "Layers (" + std::to_string(m_hiddenTerrainLayers.size()) +
                ")##TerrainLayerVisibility";
        if (ImGui::Button(label.c_str()))
            ImGui::OpenPopup("TerrainLayerVisibility");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Show or hide individual object layers");
        if (ImGui::BeginPopup("TerrainLayerVisibility"))
        {
            // Names come from the loaded cells, so streamed and procedural
            // layers are listed alongside the manager's authored ones.
            std::set<std::string> names(m_hiddenTerrainLayers);
            if (manager)
                names.insert(manager->GetObjectLayerNames().begin(),
                    manager->GetObjectLayerNames().end());
            std::vector<Engine::Core::Object*> pending{manager ? manager->Owner : selected};
            while (!pending.empty())
            {
                auto* object = pending.back(); pending.pop_back();
                if (!object) continue;
                if (auto* layer = object->GetComponent<Engine::Components::TerrainObjectLayer>())
                { names.insert(layer->layerName.empty() ? object->name : layer->layerName); continue; }
                for (auto* child : object->Children) pending.push_back(child);
            }
            if (names.empty()) ImGui::TextDisabled("No object layers");
            if (m_hideAllTerrainLayers) ImGui::TextDisabled("All layers hidden by \"Hide objects\"");
            for (const auto& name : names)
            {
                bool visible = !m_hiddenTerrainLayers.count(name);
                if (ImGui::Checkbox(name.c_str(), &visible))
                {
                    if (visible) m_hiddenTerrainLayers.erase(name);
                    else m_hiddenTerrainLayers.insert(name);
                }
            }
            if (!names.empty())
            {
                ImGui::Separator();
                if (ImGui::Button("Show all")) { m_hiddenTerrainLayers.clear(); m_hideAllTerrainLayers = false; }
                ImGui::SameLine();
                if (ImGui::Button("Hide all")) m_hiddenTerrainLayers = names;
            }
            ImGui::EndPopup();
        }
    }
    ImGui::PopID();
}

void SceneView::ApplyTerrainLayerVisibility(Engine::Core::Object* terrainRoot)
{
    if (!m_scene) return;
    const bool anyHidden = terrainRoot &&
        (m_hideAllTerrainLayers || !m_hiddenTerrainLayers.empty());
    if (!anyHidden)
    {
        // Leaving terrain tools (or showing everything) restores every layer
        // this view hid, including layers under a previously selected terrain.
        if (m_terrainLayersHiddenApplied)
            for (const auto& object : m_scene->GetObjects())
                if (object && object->editorHidden &&
                    object->GetComponent<Engine::Components::TerrainObjectLayer>())
                    object->editorHidden = false;
        m_terrainLayersHiddenApplied = false;
        return;
    }
    m_terrainLayersHiddenApplied = true;
    // Re-applied every frame so newly streamed cells pick up the state.
    std::vector<Engine::Core::Object*> pending{terrainRoot};
    while (!pending.empty())
    {
        auto* object = pending.back(); pending.pop_back();
        if (!object) continue;
        if (auto* layer = object->GetComponent<Engine::Components::TerrainObjectLayer>())
        {
            const std::string& name = layer->layerName.empty() ? object->name : layer->layerName;
            object->editorHidden = m_hideAllTerrainLayers || m_hiddenTerrainLayers.count(name) > 0;
            continue;
        }
        for (auto* child : object->Children) pending.push_back(child);
    }
}

void SceneView::SynchronizePlacementProxy()
{
    if (!m_scene) { m_placementProxy = 0; return; }
    using Handle = Engine::Components::TerrainPlacementHandle;
    auto* selected = Engine::Editor::EditorViewState::For(*m_scene).selectedObject;
    auto* tracked = m_placementProxy ? m_scene->FindObjectById(m_placementProxy) : nullptr;
    if (selected && selected != tracked && selected->GetComponent<Handle>())
    {
        if (tracked) m_scene->RequestRemoveObject(tracked);
        tracked = selected;
        m_placementProxy = m_scene->GetObjectId(selected);
    }
    if (!tracked) { m_placementProxy = 0; return; }
    // The proxy exists only while selected; the placement record is the data.
    if (selected != tracked)
    {
        m_scene->RequestRemoveObject(tracked);
        m_placementProxy = 0;
        return;
    }
    auto* handle = tracked->GetComponent<Handle>();
    if (!handle || handle->Synchronize() == Handle::SyncResult::PlacementMissing)
    {
        m_scene->RequestRemoveObject(tracked);
        m_placementProxy = 0;
        if (OnObjectSelected) OnObjectSelected(nullptr);
    }
}

void SceneView::DrawPanel(IEditorUi& ui)
{
    if (m_scene && UseGlobalBoneVisibility) EditorViewportSettings::Apply(*m_scene);
    if (m_focusOnNextDraw)
    {
        ui.FocusWindow(m_title.c_str());
        m_focusOnNextDraw = false;
    }

    const bool mode2D = m_scene && m_scene->IsMode2D();
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
    bool terrainConsumedClick = false;
    // A terrain edit spans the frames the left button is held; once released
    // the stroke ends and the editor captures one undo step for it.
    if (!input.leftDown) m_terrainEditActive = false;
    auto* selected = m_scene ? Engine::Editor::EditorViewState::For(*m_scene).selectedObject : nullptr;
    auto* terrain = selected
        ? selected->GetComponent<Engine::Components::Terrain>() : nullptr;
    // Only a Terrain cell or a TerrainManager is a terrain-editing target. A
    // cell edits through its manager; objects placed on the terrain (trees,
    // grass batches, placement proxies) must not activate terrain tools.
    Engine::Components::TerrainManager* manager = selected
        ? selected->GetComponent<Engine::Components::TerrainManager>() : nullptr;
    for (auto* object = terrain ? selected->Parent : nullptr; object && !manager;
         object = object->Parent)
        manager = object->GetComponent<Engine::Components::TerrainManager>();
    if (selected != m_previousSelectedObject)
    {
        if ((terrain || manager) && m_editMode == SceneEditMode::Object)
            m_editMode = SceneEditMode::Terrain;
        m_previousSelectedObject = selected;
    }
    const bool terrainTools = (terrain || manager) && m_editMode == SceneEditMode::Terrain;
    if (terrainTools && input.hovered && !ImGui::GetIO().WantTextInput &&
        ImGui::IsKeyPressed(ImGuiKey_H, false))
        m_hideAllTerrainLayers = !m_hideAllTerrainLayers;
    ApplyTerrainLayerVisibility(terrainTools
        ? (manager ? manager->Owner : selected) : nullptr);
    const bool objectBrush = terrainTools && m_terrainBrush == kObjectBrush;
    const bool invert = ImGui::GetIO().KeyShift;
    SceneTerrainTools::Target terrainTarget;
    terrainTarget.manager = manager;
    terrainTarget.terrain = manager ? nullptr : terrain;
    terrainTarget.root = manager ? manager->Owner : selected;
    // World view-projection of the editor camera, for the brush ring.
    glm::dmat4 terrainViewProjection(1.0);
    bool terrainRingVisible = false;
    if (terrainTools && m_scene && input.available.x > 0.f && input.available.y > 0.f)
        if (auto* camera = m_scene->editorCamera.GetComponent<Engine::Components::Camera>())
        {
            terrainViewProjection = camera->GetProjectionMatrixD(
                static_cast<double>(input.available.x) / input.available.y) *
                camera->GetViewMatrixD();
            terrainRingVisible = true;
        }
    const auto drawRing = [&](const glm::dvec3& localCenter, float hardness, uint32_t color)
    {
        if (!terrainRingVisible) return;
        SceneTerrainTools::DrawBrushRing(terrainTarget, localCenter, m_terrainRadius,
            hardness, terrainViewProjection, {input.screenMin.x, input.screenMin.y},
            {input.available.x, input.available.y}, color);
    };
    if (!input.leftDown)
    {
        m_terrainFlattenSampled = false;
        m_terrainObjectStrokeReported = false;
    }
    if (m_terrainSettingChanged) { m_terrainEditActive = true; m_terrainSettingChanged = false; }
    if (!objectBrush || !input.leftDown || !input.hovered ||
        input.rightDown || input.middleDown)
        m_terrainObjectStroke = false;
    if (objectBrush)
    {
        // Reserve the stroke even on a miss, so painting cannot select its own objects.
        terrainConsumedClick = input.hovered && (input.leftDown || input.leftClicked);
        glm::vec3 position{}, normal{};
        if (input.hovered && !input.rightDown && !input.middleDown &&
            ScenePlacementAndPicking::FindTerrainPlacement(*m_scene, terrainTarget.root,
                input.mousePosInViewport, input.available, position, normal))
        {
            const glm::dvec3 local = SceneTerrainTools::ToLocal(terrainTarget, glm::dvec3(position));
            const uint32_t ringColor = invert ? IM_COL32(255, 80, 60, 230) : IM_COL32(255, 205, 40, 230);
            const auto placeShape = TerrainBrushPlaces() && !m_terrainBrushPath.empty()
                ? Engine::Components::TerrainBrushShape::Load(m_terrainBrushPath) : nullptr;
            const float shapeYaw = glm::radians(m_terrainShapeYawDegrees);
            if (placeShape && terrainRingVisible)
            {
                std::vector<glm::dvec2> outline;
                for (const auto& point : placeShape->outline)
                {
                    const glm::vec2 offset = Engine::Components::TerrainBrushShape::FromShape(
                        point, m_terrainRadius, shapeYaw);
                    outline.push_back({local.x + offset.x, local.z + offset.y});
                }
                SceneTerrainTools::DrawDrapedPolyline(terrainTarget, outline, true,
                    terrainViewProjection, {input.screenMin.x, input.screenMin.y},
                    {input.available.x, input.available.y}, ringColor);
            }
            else
                drawRing(local, TerrainBrushPlaces() ? m_terrainHardness : 0.f, ringColor);
            int changed = 0;
            if (AllowObjectCreation && input.leftDown && TerrainBrushPlaces())
            {
                // A Place brush: a dab each time the cursor has moved the
                // brush's spacing (a share of its diameter).
                const float radius = m_terrainRadius * TerrainBrushPressureSize();
                const float spacing = std::max(.1f, m_terrainBrushPreset.spacing * 2.f * radius);
                if (!m_terrainObjectStroke || glm::distance(position, m_lastTerrainObjectPosition) >= spacing)
                {
                    Engine::Components::TerrainPlacementDab dab;
                    dab.center = ScatterTerrainDab(local, radius);
                    dab.radius = radius;
                    dab.strength = std::clamp(m_terrainStrength * TerrainBrushPressureStrength(), 0.f, 1.f);
                    dab.erase = invert;
                    dab.layer = m_terrainObjectLayer;
                    dab.shape = placeShape;
                    dab.yaw = shapeYaw;
                    dab.hardness = m_terrainHardness;
                    dab.falloff = m_terrainFalloff;
                    changed = Engine::Components::ApplyPlacementBrush(*m_scene, manager,
                        manager ? nullptr : terrain, m_terrainBrushPreset, dab, m_terrainRandom);
                    m_lastTerrainObjectPosition = position;
                    m_terrainObjectStroke = true;
                }
            }
            else if (AllowObjectCreation && input.leftDown && invert)
                changed = SceneTerrainTools::Erase(terrainTarget, local, m_terrainRadius,
                    m_terrainObjectPrefab);
            else if (AllowObjectCreation && input.leftDown && !m_terrainObjectPrefab.empty() &&
                (!m_terrainObjectStroke || glm::distance(position, m_lastTerrainObjectPosition) >=
                    std::max(.1f, m_terrainObjectSpacing)))
            {
                SceneTerrainTools::ScatterSettings settings;
                settings.prefab = m_terrainObjectPrefab;
                settings.count = std::clamp(m_terrainObjectCount, 1, 64);
                settings.alignToSurface = m_terrainAlignObjects;
                settings.randomYaw = m_terrainObjectRandomYaw;
                settings.minScale = m_terrainObjectScaleMin;
                settings.maxScale = m_terrainObjectScaleMax;
                settings.layer = m_terrainObjectLayer;
                changed = SceneTerrainTools::Scatter(*m_scene, terrainTarget, local,
                    m_terrainRadius, settings, m_terrainRandom);
                m_lastTerrainObjectPosition = position;
                m_terrainObjectStroke = true;
            }
            if (changed > 0 && manager)
            {
                // The macro colour map's canopy follows the placements.
                const double spanX = double(manager->cellsX) * manager->cellWidth;
                const double spanZ = double(manager->cellsY) * manager->cellDepth;
                manager->MarkMacroCellsDirty({int(std::floor(local.x / spanX)), int(std::floor(local.z / spanZ))});
            }
            if (changed > 0)
            {
                // One undo step per stroke: the edit stays "in progress" while
                // the button is held, like a sculpt stroke.
                m_terrainEditActive = true;
                if (!m_terrainObjectStrokeReported && OnObjectCreated) OnObjectCreated(selected);
                m_terrainObjectStrokeReported = true;
                // Keep terrain selected for the rest of the brush stroke.
                Engine::Editor::EditorViewState::For(*m_scene).SelectObject(selected);
            }
        }
    }
    // A Foliage brush on the Paint tool: foliage density on whatever surface
    // the cursor meets (the terrain field: open ground, faces, ceilings).
    const bool foliageBrush = m_terrainBrush == kPaintBrush && TerrainBrushFoliage();
    if (manager && foliageBrush && m_editMode == SceneEditMode::Terrain && !(m_editTerrainCells))
    {
        glm::dvec3 hit{};
        glm::vec3 normal(0.f, 1.f, 0.f);
        if (input.hovered && !input.rightDown && !input.middleDown &&
            ScenePlacementAndPicking::FindFieldSurfaceHit(*m_scene, *manager,
                input.mousePosInViewport, input.available, hit, normal))
        {
            const glm::dvec3 local = SceneTerrainTools::ToLocal(terrainTarget, hit);
            drawRing(local, m_terrainHardness, invert ? IM_COL32(255, 80, 60, 230) : IM_COL32(90, 220, 90, 230));
            if (input.leftDown)
            {
                terrainConsumedClick = true;
                Engine::Components::TerrainFoliageDab dab;
                dab.radius = m_terrainRadius * TerrainBrushPressureSize();
                dab.centre = glm::vec3(ScatterTerrainDab(local, dab.radius));
                dab.centre.y = static_cast<float>(local.y);
                dab.strength = std::clamp(m_terrainStrength * TerrainBrushPressureStrength() *
                    ImGui::GetIO().DeltaTime * 30.f, 0.f, 1.f);
                dab.erase = invert;
                dab.hardness = m_terrainHardness;
                const bool changed = Engine::Components::ApplyFoliageBrush(*manager, m_terrainBrushPreset, dab);
                m_terrainEditActive = m_terrainEditActive || changed;
            }
        }
    }
    if ((terrain || manager) && m_editMode == SceneEditMode::Terrain && !objectBrush && !foliageBrush &&
        ShapedTerrainTool() && !(manager && m_editTerrainCells))
    {
        using TerrainBrush = Engine::Components::Terrain::Brush;
        const bool stampTool = m_terrainBrush == kStampTool;
        const auto shape = m_terrainBrushPath.empty() ? nullptr :
            Engine::Components::TerrainBrushShape::Load(m_terrainBrushPath);
        glm::dvec3 hit{};
        if (input.hovered && !input.rightDown && !input.middleDown &&
            ScenePlacementAndPicking::FindTerrainSurfaceHit(*m_scene, terrainTarget.root,
                input.mousePosInViewport, input.available, hit))
        {
            const glm::dvec3 local = SceneTerrainTools::ToLocal(terrainTarget, hit);
            static constexpr Engine::Components::Terrain::Brush toolBrush[] = {
                Engine::Components::Terrain::Brush::Raise,
                Engine::Components::Terrain::Brush::Lower,
                Engine::Components::Terrain::Brush::Blend,
                Engine::Components::Terrain::Brush::Flatten,
                Engine::Components::Terrain::Brush::Paint};
            // Stamp: Raise with the stamp's shape and height map.
            auto brush = stampTool ? TerrainBrush::Raise
                : toolBrush[std::clamp(m_terrainBrush, 0, kPaintBrush)];
            if (invert && brush == Engine::Components::Terrain::Brush::Raise)
                brush = Engine::Components::Terrain::Brush::Lower;
            else if (invert && brush == Engine::Components::Terrain::Brush::Lower)
                brush = Engine::Components::Terrain::Brush::Raise;
            const bool erase = invert && brush == Engine::Components::Terrain::Brush::Paint;
            uint32_t color = IM_COL32(110, 230, 120, 230);
            if (brush == Engine::Components::Terrain::Brush::Lower) color = IM_COL32(255, 140, 70, 230);
            if (brush == Engine::Components::Terrain::Brush::Blend) color = IM_COL32(110, 210, 255, 230);
            if (brush == Engine::Components::Terrain::Brush::Flatten) color = IM_COL32(150, 150, 255, 230);
            if (erase) color = IM_COL32(255, 80, 60, 230);
            const float shapeYaw = glm::radians(m_terrainShapeYawDegrees);
            if (shape && terrainRingVisible)
            {
                // The shape's outline, turned and sized as the brush applies it.
                std::vector<glm::dvec2> outline;
                for (const auto& point : shape->outline)
                {
                    const glm::vec2 offset = Engine::Components::TerrainBrushShape::FromShape(
                        point, m_terrainRadius, shapeYaw);
                    outline.push_back({local.x + offset.x, local.z + offset.y});
                }
                SceneTerrainTools::DrawDrapedPolyline(terrainTarget, outline, true,
                    terrainViewProjection, {input.screenMin.x, input.screenMin.y},
                    {input.available.x, input.available.y}, color);
            }
            else if (!stampTool)
                drawRing(local, m_terrainHardness, color);
            if (input.leftDown && !(stampTool && !shape))
            {
                // A press on the terrain belongs to the stroke even where the
                // brush changes nothing, so it cannot select objects underneath.
                terrainConsumedClick = true;
                if (!m_terrainFlattenSampled)
                {
                    m_terrainFlattenHeight = static_cast<float>(local.y);
                    m_terrainFlattenSampled = true;
                }
                Engine::Components::Terrain::BrushOptions options;
                options.hardness = m_terrainHardness;
                options.falloff = m_terrainFalloff;
                options.rules = TerrainBrushRules();
                options.flattenHeight = m_terrainFlattenHeight;
                options.erase = erase;
                options.shape = shape;
                options.shapeYaw = shapeYaw;
                options.backed = stampTool && m_terrainStampBacked;
                // The stamp stands on the ground where the stroke began and
                // grows into its own shape, no further.
                options.stopAtStamp = stampTool;
                options.stampBase = m_terrainFlattenHeight;
                options.stampHeightScale = m_terrainStampHeightScale;
                // Pen pressure and the brush's scatter vary each dab.
                const float radius = m_terrainRadius * TerrainBrushPressureSize();
                const float strength = m_terrainStrength * TerrainBrushPressureStrength() *
                    ImGui::GetIO().DeltaTime * 30.f;
                const glm::dvec3 centre = ScatterTerrainDab(local, radius);
                const bool changed = manager
                    ? manager->ApplyBrush(centre.x, centre.z, radius, strength,
                        brush, m_terrainLayer, m_terrainRealignObjects, options)
                    : terrain->ApplyBrush(static_cast<float>(centre.x),
                        static_cast<float>(centre.z), radius, strength,
                        brush, m_terrainLayer, m_terrainRealignObjects, options);
                m_terrainEditActive = m_terrainEditActive || changed;
            }
        }
    }
    // Volume sculpting: a stroke lays volume edits along its path, one each
    // time the brush has moved a third of its radius across the ground
    // (holding still adds nothing more), as capsules from the last dab so a
    // stroke is a few smooth edits rather than a string of spheres.
    if (!input.leftDown) m_volumeStroke = false;
    if (terrainTools && manager && VolumeTerrainTool())
    {
        glm::dvec3 hit{};
        glm::vec3 normal(0.f, 1.f, 0.f);
        if (input.hovered && !input.rightDown && !input.middleDown &&
            ScenePlacementAndPicking::FindFieldSurfaceHit(*m_scene, *manager,
                input.mousePosInViewport, input.available, hit, normal))
        {
            const glm::dvec3 local = SceneTerrainTools::ToLocal(terrainTarget, hit);
            const bool ledge = m_terrainBrush == kLedgeTool;
            const bool carve = !ledge && ((m_terrainBrush == kCarveTool) != invert);
            drawRing(local, m_terrainHardness, ledge ? IM_COL32(150, 150, 255, 230)
                : carve ? IM_COL32(255, 120, 70, 230) : IM_COL32(200, 160, 110, 230));
            if (input.leftDown)
            {
                terrainConsumedClick = true;
                const float radius = std::clamp(m_terrainRadius, .25f, 60.f);
                const bool first = !m_volumeStroke;
                if (first)
                {
                    m_volumeStroke = true;
                    m_volumeLast = local;
                    m_volumeLedgeHeight = static_cast<float>(local.y);
                }
                const double across = glm::length(glm::dvec2(local.x - m_volumeLast.x, local.z - m_volumeLast.z));
                if (first || across >= std::max(.33 * radius, .25))
                {
                    using Edit = Engine::Components::TerrainVolumeEdit;
                    const float blend = .5f + radius * .6f * (1.f - std::clamp(m_terrainHardness, 0.f, 1.f));
                    bool changed = false;
                    if (ledge)
                    {
                        Edit cut;
                        cut.shape = Edit::Shape::Box;
                        cut.subtract = true;
                        cut.a = {float(local.x), m_volumeLedgeHeight + radius, float(local.z)};
                        cut.b = {radius, radius, radius};
                        cut.radius = radius * .25f;
                        cut.blend = blend;
                        Edit fill = cut;
                        fill.subtract = false;
                        fill.a.y = m_volumeLedgeHeight - radius * .5f;
                        fill.b.y = radius * .5f;
                        changed = manager->AddVolumeEdit(cut, true);
                        changed = manager->AddVolumeEdit(fill, true) || changed;
                    }
                    else
                    {
                        Edit edit;
                        edit.shape = Edit::Shape::Capsule;
                        edit.subtract = carve;
                        edit.a = glm::vec3(first ? local : m_volumeLast);
                        edit.b = glm::vec3(local);
                        edit.radius = radius;
                        edit.blend = blend;
                        changed = manager->AddVolumeEdit(edit, true);
                    }
                    m_volumeLast = local;
                    m_terrainEditActive = m_terrainEditActive || changed;
                }
            }
        }
    }
    // Region-operation brushes: each frame the button is held runs a small
    // operation under the brush through the same cross-cell window as the
    // Select tool, so erosion flows over cell borders without seams.
    const bool operationBrush = terrainTools && m_terrainBrush >= kErodeBrush &&
        m_terrainBrush <= kTerraceBrush;
    if (operationBrush)
    {
        glm::dvec3 hit{};
        if (input.hovered && !input.rightDown && !input.middleDown &&
            ScenePlacementAndPicking::FindTerrainSurfaceHit(*m_scene, terrainTarget.root,
                input.mousePosInViewport, input.available, hit))
        {
            const glm::dvec3 local = SceneTerrainTools::ToLocal(terrainTarget, hit);
            static constexpr uint32_t colors[] = {IM_COL32(90, 170, 255, 230),
                IM_COL32(210, 175, 120, 230), IM_COL32(220, 120, 255, 230),
                IM_COL32(255, 220, 110, 230)};
            drawRing(local, m_terrainHardness, colors[m_terrainBrush - kErodeBrush]);
            if (input.leftDown)
            {
                terrainConsumedClick = true;
                auto op = m_brushOperation;
                op.kind = RegionOperationKind(BrushOperationKind(m_terrainBrush));
                const float rate = std::clamp(m_terrainStrength * ImGui::GetIO().DeltaTime * 30.f,
                    0.f, 4.f);
                const float spacing = manager ? std::max(manager->cellWidth, manager->cellDepth)
                    : std::max(terrain->cellWidth, terrain->cellDepth);
                op.strength = 1.f;
                switch (m_terrainBrush)
                {
                case kErodeBrush:
                    // About half a droplet per covered sample per frame at
                    // strength 1, new droplets every frame.
                    op.dropletDensity = .5f * rate / std::max(1e-4f, spacing * spacing);
                    op.seed = m_brushSeed++;
                    break;
                case kWeatherBrush:
                    op.iterations = 1;
                    op.strength = std::min(1.f, rate);
                    break;
                case kNoiseBrush:
                    op.noiseAmplitude = m_brushOperation.noiseAmplitude * .1f * rate *
                        (invert ? -1.f : 1.f);
                    break;
                default:
                    op.strength = std::min(1.f, .2f * rate);
                    break;
                }
                const auto mask = Engine::Components::TerrainRegionMask::Circle(
                    {local.x, local.z}, m_terrainRadius, m_terrainHardness);
                const bool changed = manager
                    ? manager->EditRegion(mask, op, m_terrainRealignObjects)
                    : terrain->EditRegion(mask, op, m_terrainRealignObjects);
                m_terrainEditActive = m_terrainEditActive || changed;
            }
        }
    }
    // Select tool: draw a region, then Apply (toolbar or Enter) runs the
    // chosen operation once over all of it as a single undo step.
    const bool regionTool = terrainTools && m_terrainBrush == kSelectTool;
    if (!regionTool) m_regionDrawing = false;
    if (regionTool)
    {
        const auto& io = ImGui::GetIO();
        glm::dvec3 hit{};
        glm::dvec3 local{};
        const bool overTerrain = input.hovered && !input.rightDown && !input.middleDown &&
            ScenePlacementAndPicking::FindTerrainSurfaceHit(*m_scene, terrainTarget.root,
                input.mousePosInViewport, input.available, hit);
        if (overTerrain) local = SceneTerrainTools::ToLocal(terrainTarget, hit);
        const glm::dvec2 point(local.x, local.z);
        const glm::vec2 mouse(io.MousePos.x, io.MousePos.y);
        const glm::vec2 screenMin(input.screenMin.x, input.screenMin.y);
        const glm::vec2 screenSize(input.available.x, input.available.y);
        const auto commitPolygon = [&] {
            if (m_regionPoints.size() >= 3)
                m_regionMask = Engine::Components::TerrainRegionMask::Polygon(m_regionPoints,
                    m_regionFeather);
            m_regionPoints.clear();
            m_regionDrawing = false;
        };
        if (input.hovered && !io.WantTextInput)
        {
            if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) ClearRegionSelection();
            if (m_regionShape == 3 && m_regionDrawing && !m_regionPoints.empty() &&
                ImGui::IsKeyPressed(ImGuiKey_Backspace, false))
            {
                m_regionPoints.pop_back();
                m_regionDrawing = !m_regionPoints.empty();
            }
            if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
                ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false))
            {
                if (m_regionShape == 3 && m_regionDrawing) commitPolygon();
                else if (!m_regionMask.Empty()) m_regionApplyRequested = true;
            }
        }
        if (overTerrain && io.KeyCtrl && input.leftClicked)
            // Ctrl+click: pick the Flatten height from the surface.
            m_regionSettings.height = float(local.y);
        else if (input.hovered && !io.KeyCtrl)
        {
            if (m_regionShape <= 1)
            {
                if (input.leftDown && !m_regionDrawing && overTerrain)
                {
                    m_regionStart = m_regionLast = point;
                    m_regionDrawing = true;
                }
                if (m_regionDrawing && overTerrain) m_regionLast = point;
                if (m_regionDrawing && !input.leftDown)
                {
                    auto mask = Engine::Components::TerrainRegionMask::Box(m_regionStart,
                        m_regionLast, m_regionShape == 1, m_regionFeather);
                    // A click without a drag clears the selection.
                    m_regionMask = mask.Empty() ? Engine::Components::TerrainRegionMask{} : mask;
                    m_regionDrawing = false;
                }
            }
            else if (m_regionShape == 2)
            {
                if (input.leftDown && !m_regionDrawing && overTerrain)
                {
                    m_regionPoints = {point};
                    m_regionLastScreen = mouse;
                    m_regionDrawing = true;
                }
                else if (m_regionDrawing && input.leftDown && overTerrain &&
                    glm::distance(mouse, m_regionLastScreen) >= 6.f)
                {
                    m_regionPoints.push_back(point);
                    m_regionLastScreen = mouse;
                }
                if (m_regionDrawing && !input.leftDown)
                {
                    if (m_regionPoints.size() < 3) m_regionMask = {};
                    commitPolygon();
                }
            }
            else if (input.leftClicked && overTerrain)
            {
                glm::vec2 first;
                const bool closes = m_regionDrawing && m_regionPoints.size() >= 3 &&
                    (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) ||
                        (SceneTerrainTools::ProjectSurface(terrainTarget, m_regionPoints.front(),
                            terrainViewProjection, screenMin, screenSize, first) &&
                            glm::distance(first, mouse) <= 10.f));
                if (closes) commitPolygon();
                else
                {
                    if (!m_regionDrawing) { m_regionPoints.clear(); m_regionDrawing = true; }
                    m_regionPoints.push_back(point);
                }
            }
        }
        terrainConsumedClick = terrainConsumedClick ||
            (input.hovered && (input.leftDown || input.leftClicked));
        if (terrainRingVisible)
        {
            const uint32_t selectionColor = IM_COL32(80, 200, 255, 230);
            const uint32_t draftColor = IM_COL32(255, 230, 120, 230);
            if (!m_regionMask.Empty())
                SceneTerrainTools::DrawRegionHighlight(terrainTarget, m_regionMask,
                    terrainViewProjection, screenMin, screenSize, selectionColor);
            if (m_regionDrawing && m_regionShape <= 1)
            {
                const auto draft = Engine::Components::TerrainRegionMask::Box(m_regionStart,
                    m_regionLast, m_regionShape == 1, m_regionFeather);
                if (!draft.Empty())
                    SceneTerrainTools::DrawRegionHighlight(terrainTarget, draft,
                        terrainViewProjection, screenMin, screenSize, draftColor);
            }
            else if (m_regionDrawing && !m_regionPoints.empty())
            {
                auto line = m_regionPoints;
                if (m_regionShape == 3 && overTerrain) line.push_back(point);
                SceneTerrainTools::DrawDrapedPolyline(terrainTarget, line, false,
                    terrainViewProjection, screenMin, screenSize, draftColor, 2.f);
                glm::vec2 first;
                if (m_regionShape == 3 && SceneTerrainTools::ProjectSurface(terrainTarget,
                        m_regionPoints.front(), terrainViewProjection, screenMin, screenSize, first))
                    ImGui::GetForegroundDrawList()->AddCircle({first.x, first.y}, 6.f,
                        draftColor, 0, 2.f);
            }
        }
    }
    if (m_regionApplyRequested)
    {
        m_regionApplyRequested = false;
        if (terrainTools && !m_regionMask.Empty())
        {
            const auto begin = std::chrono::steady_clock::now();
            m_regionMask.feather = std::max(0.f, m_regionFeather);
            bool changed = false;
            if (m_regionOperation == kRegionPlace)
            {
                if (!TerrainBrushPlaces())
                    m_regionStatus = "Choose a Place brush (Brush combo) first";
                else
                    changed = Engine::Components::ApplyPlacementRegion(*m_scene, manager,
                        manager ? nullptr : terrain, m_terrainBrushPreset, m_regionMask,
                        m_regionSettings.strength, m_terrainObjectLayer, invert, m_terrainRandom) > 0;
            }
            else if (m_regionOperation == kRegionPaint)
                changed = manager
                    ? manager->PaintRegion(m_regionMask, m_regionSettings.strength, m_terrainLayer, invert,
                        TerrainBrushRules())
                    : terrain->PaintRegion(m_regionMask, m_regionSettings.strength, m_terrainLayer, invert,
                        0.0, 0.0, TerrainBrushRules());
            else
            {
                auto op = m_regionSettings;
                op.kind = RegionOperationKind(m_regionOperation);
                changed = manager ? manager->EditRegion(m_regionMask, op, m_terrainRealignObjects)
                    : terrain->EditRegion(m_regionMask, op, m_terrainRealignObjects);
                // Repeated erosion applies fresh droplets.
                if (op.kind == Engine::Components::TerrainRegionOperation::Kind::Hydraulic)
                    ++m_regionSettings.seed;
            }
            const double ms = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - begin).count();
            char status[96];
            std::snprintf(status, sizeof(status), changed ? "Applied in %.0f ms" :
                "No change (%.0f ms)", ms);
            m_regionStatus = status;
            // Reported as a one-frame edit: one undo step.
            if (changed) m_terrainEditActive = true;
        }
    }
    if (manager && m_editMode == SceneEditMode::Terrain && !objectBrush && !regionTool &&
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
                        m_terrainEditActive = true;
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
    if (OnDrawRigControls && m_scene && Engine::Editor::EditorViewState::For(*m_scene).bonesVisible)
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
        OnGizmoInteraction(gizmoResult.transformDragging || m_terrainEditActive ||
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

            // Right-button drag â†’ pan.
            if (input.rightDown)
            {
                panDX += d.x;
                panDY += d.y;
            }

            // Middle-button drag â†’ orbit.
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

            // Scroll wheel â†’ zoom.
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
        Engine::Core::Object* bone = Engine::Core::ComponentInspector::HasReferencePick()
            ? ScenePlacementAndPicking::PickBoneInViewport(*m_scene,
                input.mousePosInViewport, input.available) : nullptr;
        uint64_t pickedPlacementId = 0;
        Engine::Core::Object* pickedObject = bone ? bone :
            ScenePlacementAndPicking::PickObjectInViewport(*m_scene,
                input.mousePosInViewport, input.available, false,
                &pickedPlacementId);
        // Instanced layer batches draw many placements; select the one that
        // was hit through its proxy so the gizmo and outline act on it alone.
        auto* placementLayer = pickedObject && pickedPlacementId
            ? pickedObject->GetComponentInParent<
                Engine::Components::TerrainObjectLayer>() : nullptr;
        if (placementLayer)
            if (auto* proxy = Engine::Components::TerrainPlacementHandle::Acquire(
                    *m_scene, placementLayer->Owner, pickedPlacementId))
                pickedObject = proxy;
        OnObjectSelected(pickedObject);
    }
    SynchronizePlacementProxy();
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
        Engine::Editor::EditorViewState::For(*m_scene).UpdateCameraFollow();
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
        Engine::Editor::EditorViewState::For(*m_scene).previewObject = m_prefabPreview;
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
    if (m_scene) Engine::Editor::EditorViewState::For(*m_scene).previewObject = nullptr;
    if (placed && OnAssetPreviewCommitted) OnAssetPreviewCommitted(placed, path);
}

void SceneView::CancelPrefabPreview()
{
    if (m_prefabPreparationCanceled) m_prefabPreparationCanceled->store(true);
    m_prefabPreparation = {};
    m_prefabPreparationCanceled.reset();
    m_prefabDropPending = false;
    if (m_scene)
        Engine::Editor::EditorViewState::For(*m_scene).previewObject = nullptr;
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
        Engine::Editor::EditorViewState::For(*m_scene).UpdateCameraFollow();
        auto* factory = m_scene->GetGraphicsProvider()->GetContextFactory();
        factory->SetCommandBuffer(cmd);
        auto ctx = factory->CreateContext();
        Engine::Rendering::RenderSystem::For(*m_scene).Render(ctx.get(), m_aspect, nullptr, true,
            GetWidth(), GetHeight(), false, nullptr,
            m_terrainObjectDisplayLod, m_sceneRenderMode);
    }
}

void SceneView::RecordRenderCommands(Engine::Graphics::GraphicsCommandPacket& shadow,
    Engine::Graphics::GraphicsCommandPacket& color,
    const Engine::Rendering::SceneRenderer::NativeRenderPacket* packet, RecordingTimings* timings)
{
    using Clock=std::chrono::steady_clock;
    const auto started=Clock::now();
    if (!m_scene) return;
    Engine::Editor::EditorViewState::For(*m_scene).UpdateCameraFollow();
    m_scene->PrepareNativeUiLayout(m_aspect);
    const auto setupEnd=Clock::now();
    Engine::Rendering::RenderSystem::For(*m_scene).Render(&shadow, m_aspect, nullptr, false, GetWidth(), GetHeight(), true, packet);
    const auto shadowEnd=Clock::now();
    Engine::Rendering::RenderSystem::For(*m_scene).Render(&color, m_aspect, nullptr, true, GetWidth(), GetHeight(), false,
        packet, m_terrainObjectDisplayLod, m_sceneRenderMode);
    if(timings) {
        timings->setupMs=std::chrono::duration<double,std::milli>(setupEnd-started).count();
        timings->shadowMs=std::chrono::duration<double,std::milli>(shadowEnd-setupEnd).count();
        timings->colorMs=std::chrono::duration<double,std::milli>(Clock::now()-shadowEnd).count();
    }
}

void SceneView::RenderShadow3D(void* cmd)
{
    if (!m_scene || !m_scene->GetGraphicsProvider()) return;
    auto* factory = m_scene->GetGraphicsProvider()->GetContextFactory();
    factory->SetCommandBuffer(cmd);
    auto ctx = factory->CreateContext();
    if (ctx)
        Engine::Rendering::RenderSystem::For(*m_scene).Render(ctx.get(), m_aspect, nullptr, false,
            GetWidth(), GetHeight(), true);
}
}

float Engine::Editor::SceneView::EffectiveRenderScale() const
{
    return m_scene ? std::clamp(m_scene->settings.renderScale * m_scene->settings.graphicsQuality.Resolve().renderScale, .5f, 1.f) : 1.f;
}

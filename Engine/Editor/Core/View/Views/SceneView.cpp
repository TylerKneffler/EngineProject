
#include "SceneView.h"
#include "Engine/Editor/Core/PrimitiveObjectFactory.h"
#include "Engine/Editor/Core/View/Templates/Common/AssetPathTemplate.h"
#include "Engine/Editor/UI/IEditorUi.h"
#include "Core/Scene/Scene.h"
#include "Core/Compoonents/Camera.h"
#include "Core/Compoonents/Mesh.h"
#include "Core/Compoonents/Sprite.h"
#include "Core/Compoonents/Sprite/SpriteAnimationManager.h"
#include "Core/Graphics/IGraphicsContext.h"
#include "Core/Graphics/IGraphicsProvider.h"
#include <cfloat>
#include <cmath>
#include <limits>
#include <glm/gtc/matrix_inverse.hpp>
#include <glm/geometric.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <filesystem>
#include <algorithm>
#include <cctype>

namespace Engine::Editor
{
namespace
{
bool BuildViewportRay(const Engine::Scene::Scene& scene, const EditorUiVec2& mousePos,
    const EditorUiVec2& viewportSize, glm::vec3& origin, glm::vec3& direction)
{
    if (viewportSize.x <= 1.f || viewportSize.y <= 1.f)
        return false;
    const Engine::Components::Camera* camera = scene.editorCamera.GetComponent<Engine::Components::Camera>();
    if (!camera)
        return false;
    const glm::mat4 inverseViewProjection = glm::inverse(
        camera->GetProjectionMatrix(viewportSize.x / viewportSize.y) *
        camera->GetViewMatrix());
    const float ndcX = 2.f * mousePos.x / viewportSize.x - 1.f;
    const float ndcY = 1.f - 2.f * mousePos.y / viewportSize.y;
    glm::vec4 nearPoint = inverseViewProjection * glm::vec4(ndcX, ndcY, 0.f, 1.f);
    glm::vec4 farPoint = inverseViewProjection * glm::vec4(ndcX, ndcY, 1.f, 1.f);
    if (std::abs(nearPoint.w) < 0.000001f || std::abs(farPoint.w) < 0.000001f)
        return false;
    origin = glm::vec3(nearPoint) / nearPoint.w;
    const glm::vec3 target = glm::vec3(farPoint) / farPoint.w;
    direction = glm::normalize(target - origin);
    return true;
}

bool IsInObjectHierarchy(const Engine::Core::Object* object, const Engine::Core::Object* root)
{
    for (const Engine::Core::Object* current = object; current; current = current->Parent)
        if (current == root)
            return true;
    return false;
}

bool IntersectMeshBounds(const Engine::Core::Object& object, const Engine::Components::Mesh& mesh,
    const glm::vec3& rayOrigin, const glm::vec3& rayDirection, float& distance)
{
    if (!mesh.HasBounds())
        return false;
    const glm::mat4 inverseWorld = glm::inverse(object.transform.GetWorldMatrix());
    const glm::vec3 localOrigin = glm::vec3(inverseWorld * glm::vec4(rayOrigin, 1.f));
    const glm::vec3 localDirection = glm::vec3(inverseWorld * glm::vec4(rayDirection, 0.f));
    float nearDistance = 0.f;
    float farDistance = FLT_MAX;
    for (int axis = 0; axis < 3; ++axis)
    {
        if (std::abs(localDirection[axis]) < 0.000001f)
        {
            if (localOrigin[axis] < mesh.GetBoundsMin()[axis] ||
                localOrigin[axis] > mesh.GetBoundsMax()[axis])
                return false;
            continue;
        }
        float first = (mesh.GetBoundsMin()[axis] - localOrigin[axis]) /
            localDirection[axis];
        float second = (mesh.GetBoundsMax()[axis] - localOrigin[axis]) /
            localDirection[axis];
        if (first > second)
            std::swap(first, second);
        nearDistance = glm::max(nearDistance, first);
        farDistance = glm::min(farDistance, second);
        if (nearDistance > farDistance)
            return false;
    }
    distance = nearDistance > 0.f ? nearDistance : farDistance;
    return distance > 0.f;
}

bool Contains(EditorUiVec2 point, EditorUiVec2 center, float radius)
{
    const float x = point.x - center.x;
    const float y = point.y - center.y;
    return x * x + y * y <= radius * radius;
}

void DrawToolIcon(IEditorUi& ui, EditorTransformTool tool,
    EditorUiVec2 center, EditorUiColor color, float scale = 1.f)
{
    if (tool == EditorTransformTool::Translate)
    {
        ui.DrawViewportLine({center.x - 10.f * scale, center.y},
            {center.x + 10.f * scale, center.y}, color, 2.f * scale);
        ui.DrawViewportLine({center.x, center.y - 10.f * scale},
            {center.x, center.y + 10.f * scale}, color, 2.f * scale);
        ui.DrawViewportTriangle({center.x + 12.f * scale, center.y},
            {center.x + 7.f * scale, center.y - 4.f * scale},
            {center.x + 7.f * scale, center.y + 4.f * scale}, color);
        ui.DrawViewportTriangle({center.x - 12.f * scale, center.y},
            {center.x - 7.f * scale, center.y - 4.f * scale},
            {center.x - 7.f * scale, center.y + 4.f * scale}, color);
        ui.DrawViewportTriangle({center.x, center.y - 12.f * scale},
            {center.x - 4.f * scale, center.y - 7.f * scale},
            {center.x + 4.f * scale, center.y - 7.f * scale}, color);
        ui.DrawViewportTriangle({center.x, center.y + 12.f * scale},
            {center.x - 4.f * scale, center.y + 7.f * scale},
            {center.x + 4.f * scale, center.y + 7.f * scale}, color);
    }
    else if (tool == EditorTransformTool::Rotate)
    {
        ui.DrawViewportCircle(center, 10.f * scale, color, false, 2.f * scale);
        ui.DrawViewportTriangle({center.x + 11.f * scale, center.y - 2.f * scale},
            {center.x + 5.f * scale, center.y - 7.f * scale},
            {center.x + 12.f * scale, center.y - 9.f * scale}, color);
    }
    else if (tool == EditorTransformTool::Scale)
    {
        ui.DrawViewportLine({center.x - 8.f * scale, center.y + 8.f * scale},
            {center.x + 8.f * scale, center.y - 8.f * scale}, color, 3.f * scale);
        ui.DrawViewportCircle({center.x - 9.f * scale, center.y + 9.f * scale}, 4.f * scale,
            color, true);
        ui.DrawViewportCircle({center.x + 9.f * scale, center.y - 9.f * scale}, 4.f * scale,
            color, true);
    }
    else
    {
        ui.DrawViewportCircle({center.x, center.y + 4.f * scale}, 7.f * scale,
            color, false, 2.f * scale);
        for (int finger = -2; finger <= 2; ++finger)
            ui.DrawViewportLine({center.x + finger * 3.f * scale, center.y + 2.f * scale},
                {center.x + finger * 3.f * scale,
                    center.y + (-9.f + std::abs(finger)) * scale},
                color, 2.f * scale);
    }
}
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
    const bool mode2D = m_scene && m_scene->IsEditorMode2D();
    SetClearColor(mode2D ? 0.28f : 0.0f,
        mode2D ? 0.30f : 0.0f,
        mode2D ? 0.34f : 0.502f);

    // Remove inner padding so the texture fills the panel edge-to-edge.
    const bool windowVisible = ui.BeginWindow(m_title.c_str(), &m_open, true);
    if (ui.IsWindowFocused() && OnFocused) OnFocused();
    if (!windowVisible)
    {
        CancelPrefabPreview();
        ui.EndWindow();
        return;
    }

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
    const bool toolbarConsumedClick = DrawTransformToolbar(ui, input);
    const EditorUiContextMenuResult createMenu =
        ui.ContextMenu(this, "Create", nullptr, true);
    if (m_scene && (createMenu.addRequested ||
        !createMenu.primitive3D.empty() || createMenu.addSpriteRequested))
    {
        Engine::Core::Object* created = nullptr;
        glm::vec3 placement(0.f);
        const bool hasPlacement = FindPrefabPlacement(
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
    if (toolbarConsumedClick || m_transformTool == EditorTransformTool::Hand)
        gizmoInput.leftClicked = false;
    const EditorGizmoResult gizmoResult = m_scene
        ? m_gizmos.DrawAndHandle(*m_scene, ui, gizmoInput, m_transformTool)
        : EditorGizmoResult{};
    if (OnGizmoInteraction)
        OnGizmoInteraction(gizmoResult.transformDragging);
    if (gizmoResult.selectionRequested && OnObjectSelected)
        OnObjectSelected(gizmoResult.selectedObject);
    bool prefabDragObserved = false;
    if (ui.BeginDragDropTarget())
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
                if (m_prefabPreview && FindPrefabPlacement(
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
            else if (payload.delivered && OnAssetDropped)
                OnAssetDropped(path);
        }
        ui.EndDragDropTarget();
    }
    if (!prefabDragObserved)
        CancelPrefabPreview();
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
            if (input.leftClicked && !toolbarConsumedClick &&
                m_transformTool != EditorTransformTool::Hand &&
                !gizmoResult.consumedClick &&
                !input.rightDown && !input.middleDown && OnObjectSelected)
                OnObjectSelected(PickObjectInViewport(input.mousePosInViewport, input.available));

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
            if (m_transformTool == EditorTransformTool::Hand &&
                input.leftDown && !toolbarConsumedClick)
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

    ui.EndWindow();

    ApplyCameraControls(panDX, panDY, orbitDX, orbitDY, zoom, dolly);
}

bool SceneView::DrawTransformToolbar(IEditorUi& ui,
    const EditorUiViewportInput& input)
{
    if (input.available.x < 28.f || input.available.y < 28.f)
        return false;

    constexpr float scale = 1.f / 3.f;
    constexpr float radius = 18.f * scale;
    constexpr float spacing = 44.f * scale;
    constexpr float padding = 8.f;
    const EditorUiVec2 mainCenter{padding + radius, padding + radius};
    bool consumed = false;
    if (input.rawLeftClicked && Contains(
        input.mousePosInViewport, mainCenter, radius))
    {
        m_transformToolbarExpanded = !m_transformToolbarExpanded;
        consumed = true;
    }

    const bool mainHovered = Contains(
        input.mousePosInViewport, mainCenter, radius);
    ui.DrawViewportCircle(mainCenter, radius,
        mainHovered ? EditorUiColor{0.22f, 0.25f, 0.31f, 0.98f}
                    : EditorUiColor{0.10f, 0.12f, 0.16f, 0.94f}, true);
    ui.DrawViewportCircle(mainCenter, radius,
        {0.72f, 0.78f, 0.90f, 0.9f}, false, 1.5f * scale);
    DrawToolIcon(ui, m_transformTool, mainCenter,
        {0.92f, 0.95f, 1.f, 1.f}, scale);

    if (!m_transformToolbarExpanded)
        return consumed;

    constexpr EditorTransformTool tools[] = {
        EditorTransformTool::Translate,
        EditorTransformTool::Rotate,
        EditorTransformTool::Scale,
        EditorTransformTool::Hand
    };
    for (int index = 0; index < 4; ++index)
    {
        const EditorUiVec2 center{mainCenter.x, mainCenter.y +
            spacing * static_cast<float>(index + 1)};
        if (center.y + radius > input.available.y)
            break;
        const bool hovered = Contains(input.mousePosInViewport, center, radius);
        if (input.rawLeftClicked && hovered)
        {
            m_transformTool = tools[index];
            m_transformToolbarExpanded = false;
            consumed = true;
        }
        const bool selected = tools[index] == m_transformTool;
        ui.DrawViewportCircle(center, radius,
            selected ? EditorUiColor{0.18f, 0.38f, 0.68f, 0.98f}
                     : hovered ? EditorUiColor{0.22f, 0.25f, 0.31f, 0.98f}
                               : EditorUiColor{0.10f, 0.12f, 0.16f, 0.94f}, true);
        ui.DrawViewportCircle(center, radius,
            selected ? EditorUiColor{0.45f, 0.72f, 1.f, 1.f}
                     : EditorUiColor{0.62f, 0.68f, 0.78f, 0.9f}, false,
            1.5f * scale);
        DrawToolIcon(ui, tools[index], center,
            {0.92f, 0.95f, 1.f, 1.f}, scale);
    }
    return consumed;
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

bool SceneView::FindPrefabPlacement(const EditorUiVec2& mousePos,
    const EditorUiVec2& viewportSize, glm::vec3& placement) const
{
    if (!m_scene)
        return false;
    glm::vec3 rayOrigin{}, rayDirection{};
    if (!BuildViewportRay(*m_scene, mousePos, viewportSize, rayOrigin, rayDirection))
        return false;

    float bestDistance = FLT_MAX;
    bool found = false;
    for (const auto& objectPointer : m_scene->GetObjects())
    {
        Engine::Core::Object* object = objectPointer.get();
        if (!object || !object->IsEnabledInHierarchy() ||
            !object->GetComponent<Engine::Components::Mesh>() || IsInObjectHierarchy(object, m_prefabPreview))
            continue;
        Engine::Components::Mesh* mesh = object->GetComponent<Engine::Components::Mesh>();
        float distance = 0.f;
        if (mesh && IntersectMeshBounds(
            *object, *mesh, rayOrigin, rayDirection, distance) &&
            distance < bestDistance)
        {
            bestDistance = distance;
            placement = rayOrigin + rayDirection * distance;
            found = true;
        }
    }

    if (m_scene->IsEditorMode2D())
    {
        if (std::abs(rayDirection.z) <= 0.000001f)
            return found;
        const float canvasDistance = -rayOrigin.z / rayDirection.z;
        if (canvasDistance > 0.f && canvasDistance < bestDistance)
        {
            placement = rayOrigin + rayDirection * canvasDistance;
            placement.z = 0.f;
            return true;
        }
        return found;
    }

    // The 3D editor grid is the XZ placement plane. Prefer an object surface when
    // it is in front of the grid intersection; otherwise use the exact ground
    // intersection without snapping.
    if (std::abs(rayDirection.y) > 0.000001f)
    {
        const float gridDistance = -rayOrigin.y / rayDirection.y;
        if (gridDistance > 0.f && gridDistance < bestDistance)
        {
            placement = rayOrigin + rayDirection * gridDistance;
            placement.y = 0.f;
            found = true;
        }
    }
    return found;
}

Engine::Core::Object* SceneView::PickObjectInViewport(const EditorUiVec2& mousePos, const EditorUiVec2& viewportSize) const
{
    if (!m_scene || viewportSize.x <= 1.f || viewportSize.y <= 1.f)
        return nullptr;

    Engine::Components::Camera* cam = m_scene->editorCamera.GetComponent<Engine::Components::Camera>();
    if (!cam)
        return nullptr;

    const glm::mat4 view = cam->GetViewMatrix();
    const glm::mat4 proj = cam->GetProjectionMatrix(viewportSize.x / viewportSize.y);
    const glm::mat4 invVP = glm::inverse(proj * view);

    const float ndcX = (2.f * mousePos.x / viewportSize.x) - 1.f;
    const float ndcY = 1.f - (2.f * mousePos.y / viewportSize.y);

    glm::vec4 nearH = invVP * glm::vec4(ndcX, ndcY, 0.f, 1.f);
    glm::vec4 farH  = invVP * glm::vec4(ndcX, ndcY, 1.f, 1.f);
    if (nearH.w == 0.f || farH.w == 0.f)
        return nullptr;

    const glm::vec3 rayOrigin = glm::vec3(nearH) / nearH.w;
    const glm::vec3 rayTarget = glm::vec3(farH) / farH.w;
    const glm::vec3 rayDir = glm::normalize(rayTarget - rayOrigin);

    if (m_scene->IsEditorMode2D())
    {
        Engine::Core::Object* frontmost = nullptr;
        int frontLayer = std::numeric_limits<int>::min();
        float frontZ = -FLT_MAX;
        for (const auto& objPtr : m_scene->GetObjects())
        {
            Engine::Core::Object* obj = objPtr.get();
            Engine::Components::Sprite* sprite = obj && obj->IsEnabledInHierarchy()
                ? obj->GetComponent<Engine::Components::Sprite>() : nullptr;
            if (!sprite || std::abs(rayDir.z) < 0.000001f)
                continue;
            glm::mat4 world = obj->transform.GetWorldMatrix();
            world[3].z = 0.f;
            const glm::vec2 spriteSize = sprite->GetWorldSize();
            world = world * glm::scale(glm::mat4(1.f),
                glm::vec3(spriteSize, 1.f));
            const glm::mat4 inverseWorld = glm::inverse(world);
            const glm::vec3 localOrigin = glm::vec3(
                inverseWorld * glm::vec4(rayOrigin, 1.f));
            const glm::vec3 localDirection = glm::vec3(
                inverseWorld * glm::vec4(rayDir, 0.f));
            if (std::abs(localDirection.z) < 0.000001f)
                continue;
            const float distance = -localOrigin.z / localDirection.z;
            if (distance < 0.f)
                continue;
            const glm::vec3 hit = localOrigin + localDirection * distance;
            if (std::abs(hit.x) > 0.5f || std::abs(hit.y) > 0.5f)
                continue;
            const float z = obj->transform.GetWorldPosition().z;
            if (!frontmost || sprite->sortingLayer > frontLayer ||
                (sprite->sortingLayer == frontLayer && z >= frontZ))
            {
                frontmost = obj;
                frontLayer = sprite->sortingLayer;
                frontZ = z;
            }
        }
        if (frontmost)
            return frontmost;

        Engine::Core::Object* closestMesh = nullptr;
        float closestDistance = FLT_MAX;
        for (const auto& objPtr : m_scene->GetObjects())
        {
            Engine::Core::Object* obj = objPtr.get();
            Engine::Components::Mesh* mesh = obj && obj->IsEnabledInHierarchy()
                ? obj->GetComponent<Engine::Components::Mesh>() : nullptr;
            float distance = 0.f;
            if (mesh && IntersectMeshBounds(*obj, *mesh,
                rayOrigin, rayDir, distance) && distance < closestDistance)
            {
                closestDistance = distance;
                closestMesh = obj;
            }
        }
        return closestMesh;
    }

    Engine::Core::Object* best = nullptr;
    float bestT = FLT_MAX;

    for (const auto& objPtr : m_scene->GetObjects())
    {
        Engine::Core::Object* obj = objPtr.get();
        if (!obj || !obj->IsEnabledInHierarchy() ||
            (!obj->GetComponent<Engine::Components::Mesh>() && !obj->GetComponent<Engine::Components::Sprite>()))
            continue;

        const glm::vec3 center = obj->transform.GetWorldPosition();
        const float radius = glm::max(glm::max(obj->transform.scale.x, obj->transform.scale.y), obj->transform.scale.z) * 0.75f;
        const glm::vec3 oc = rayOrigin - center;
        const float a = glm::dot(rayDir, rayDir);
        const float b = 2.f * glm::dot(oc, rayDir);
        const float c = glm::dot(oc, oc) - (radius * radius);
        const float disc = b * b - 4.f * a * c;
        if (disc < 0.f)
            continue;

        const float sqrtDisc = std::sqrt(disc);
        const float t0 = (-b - sqrtDisc) / (2.f * a);
        const float t1 = (-b + sqrtDisc) / (2.f * a);
        float t = t0 > 0.f ? t0 : t1;
        if (t > 0.f && t < bestT)
        {
            bestT = t;
            best = obj;
        }
    }

    return best;
}

// ---------------------------------------------------------------------------
// CalculateGameViewport
// ---------------------------------------------------------------------------
static void CalculateGameViewport(EditorUiVec2 availableSize, EditorUiVec2& outViewportSize, EditorUiVec2& outViewportPos,
    Engine::Model::ProjectSettings::AspectRatioMode mode, float lockedAspect, uint32_t windowWidth, uint32_t windowHeight)
{
    outViewportPos = {0.f, 0.f};

    switch (mode)
    {
        case Engine::Model::ProjectSettings::AspectRatioMode::Free:
        {
            // Free aspect: use full available size
            outViewportSize = availableSize;
            break;
        }

        case Engine::Model::ProjectSettings::AspectRatioMode::Locked:
        {
            // Locked aspect ratio: fit to aspect while maintaining ratio
            float availableAspect = availableSize.x / availableSize.y;

            if (availableAspect > lockedAspect)
            {
                // Panel is wider than game aspect: pillarbox (vertical bars)
                outViewportSize.y = availableSize.y;
                outViewportSize.x = availableSize.y * lockedAspect;
                outViewportPos.x = (availableSize.x - outViewportSize.x) * 0.5f;
            }
            else
            {
                // Panel is narrower than game aspect: letterbox (horizontal bars)
                outViewportSize.x = availableSize.x;
                outViewportSize.y = availableSize.x / lockedAspect;
                outViewportPos.y = (availableSize.y - outViewportSize.y) * 0.5f;
            }
            break;
        }

        case Engine::Model::ProjectSettings::AspectRatioMode::Hardcoded:
        {
            // Hardcoded size: fit to exact window size, scale to fit
            float gameAspect = static_cast<float>(windowWidth) / static_cast<float>(windowHeight);
            float availableAspect = availableSize.x / availableSize.y;

            if (availableAspect > gameAspect)
            {
                // Panel is wider: pillarbox
                outViewportSize.y = availableSize.y;
                outViewportSize.x = availableSize.y * gameAspect;
                outViewportPos.x = (availableSize.x - outViewportSize.x) * 0.5f;
            }
            else
            {
                // Panel is narrower: letterbox
                outViewportSize.x = availableSize.x;
                outViewportSize.y = availableSize.x / gameAspect;
                outViewportPos.y = (availableSize.y - outViewportSize.y) * 0.5f;
            }
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// ApplyCameraControls
// ---------------------------------------------------------------------------
void SceneView::ApplyCameraControls(float panDX, float panDY,
                                        float orbitDX, float orbitDY,
                                        float zoom, float dolly)
{
    if (!m_scene) return;
    if (panDX == 0.f && panDY == 0.f && orbitDX == 0.f &&
        orbitDY == 0.f && zoom == 0.f && dolly == 0.f) return;

    Engine::Components::Camera*    cam = m_scene->editorCamera.GetComponent<Engine::Components::Camera>();
    assert(cam && "Scene editorCamera must have a Camera component");
    glm::vec3& pos = m_scene->editorCamera.transform.position;

    if (m_scene->IsEditorMode2D())
    {
        const float dragX = panDX + orbitDX;
        const float dragY = panDY + orbitDY;
        const float speed = glm::max(cam->orthographicSize, 0.01f) * 0.002f;
        const glm::vec3 pan(-dragX * speed, dragY * speed, 0.f);
        pos += pan;
        cam->target += pan;
        const float zoomInput = zoom + dolly * 0.01f;
        if (zoomInput != 0.f)
            cam->orthographicSize = std::clamp(
                cam->orthographicSize * std::pow(0.85f, zoomInput),
                0.05f, 10000.f);
        return;
    }

    glm::vec3 eye     = pos;
    glm::vec3 target  = cam->target;
    glm::vec3 forward = glm::normalize(target - eye);
    const glm::vec3 right  = glm::normalize(glm::cross(cam->up, forward));
    const glm::vec3 realUp = glm::normalize(glm::cross(forward, right));

    // Pan — move camera and target together along the view plane,
    //        opposite the drag direction (grab-the-world feel).
    if (panDX != 0.f || panDY != 0.f)
    {
        const float dist = glm::length(target - eye);
        float speed = dist * 0.002f;
        const glm::vec3 pan = right * (-panDX * speed) + realUp * (panDY * speed);
        pos += pan;
        cam->target += pan;
        eye = pos;
        target = cam->target;
    }

    // Arrow-key dolly translates both the camera and its orbit target along
    // the look direction, rather than moving vertically in the view plane.
    if (dolly != 0.f)
    {
        const float speed = std::max(glm::length(target - eye), 0.05f) * 0.002f;
        const glm::vec3 movement = forward * (dolly * speed);
        pos += movement;
        cam->target += movement;
        eye = pos;
        target = cam->target;
    }

    // Orbit — rotate camera around target.
    //         Horizontal drag yaws around world Y; vertical drag pitches around right.
    if (orbitDX != 0.f || orbitDY != 0.f)
    {
        constexpr float kSensitivity = 0.005f;
        glm::vec3 arm = eye - target;
        arm = glm::mat3(glm::rotate(glm::mat4(1.f), orbitDX * kSensitivity,
                                    glm::vec3(0.f, 1.f, 0.f))) * arm;
        arm = glm::mat3(glm::rotate(glm::mat4(1.f), orbitDY * kSensitivity, right)) * arm;
        pos = target + arm;
        eye = pos;
    }

    // Zoom — move eye along the forward vector, scaled by distance so the
    //         step shrinks as the camera approaches the target (Unity/Blender feel).
    if (zoom != 0.f)
    {
        constexpr float kZoomFactor  = 0.1f;
        constexpr float kMinDistance = 0.05f;
        const float dist = glm::length(target - eye);
        float step   = zoom * dist * kZoomFactor;
        // Clamp so we never overshoot the target or go below the minimum distance.
        step = std::min(step, dist - kMinDistance);
        forward = glm::normalize(target - eye);
        pos += forward * step;
    }
}

// ---------------------------------------------------------------------------
// Render3D
// ---------------------------------------------------------------------------
void SceneView::Render3D(void* cmd)
{
    if (m_scene)
    {
        auto* factory = m_scene->GetGraphicsProvider()->GetContextFactory();
        factory->SetCommandBuffer(cmd);
        auto ctx = factory->CreateContext();
        m_scene->Render(ctx.get(), m_aspect);
    }
}
}

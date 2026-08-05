
#include "SceneView.h"
#include "Engine/Editor/UI/IEditorUi.h"
#include "Core/Scene/Scene.h"
#include "Core/Compoonents/Camera.h"
#include "Core/Compoonents/Mesh.h"
#include "Core/Graphics/IGraphicsContext.h"
#include "Core/Graphics/IGraphicsProvider.h"
#include <cfloat>
#include <cmath>
#include <glm/gtc/matrix_inverse.hpp>
#include <glm/geometric.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <filesystem>
#include <algorithm>
#include <cctype>

namespace
{
bool BuildViewportRay(const Scene& scene, const EditorUiVec2& mousePos,
    const EditorUiVec2& viewportSize, glm::vec3& origin, glm::vec3& direction)
{
    if (viewportSize.x <= 1.f || viewportSize.y <= 1.f)
        return false;
    const Camera* camera = scene.editorCamera.GetComponent<Camera>();
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

bool IsInObjectHierarchy(const Object* object, const Object* root)
{
    for (const Object* current = object; current; current = current->Parent)
        if (current == root)
            return true;
    return false;
}

bool IntersectMeshBounds(const Object& object, const Mesh& mesh,
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
                         Scene* scene,
                         const ProjectSettings& settings)
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
    // Remove inner padding so the texture fills the panel edge-to-edge.
    if (!ui.BeginWindow(m_title.c_str(), &m_open, true))
    {
        CancelPrefabPreview();
        ui.EndWindow();
        return;
    }

    float panDX = 0.f, panDY = 0.f;
    float orbitDX = 0.f, orbitDY = 0.f;
    float zoom = 0.f;

    const float targetAspect = m_aspectRatioMode == ProjectSettings::AspectRatioMode::Free ? 0.f :
        (m_aspectRatioMode == ProjectSettings::AspectRatioMode::Locked ? m_gameAspectRatio :
         static_cast<float>(m_gameWindowWidth) / static_cast<float>(m_gameWindowHeight));
    const auto input = ui.Viewport(GetUiTextureHandle(), targetAspect,
        {m_letterboxColor.r,m_letterboxColor.g,m_letterboxColor.b,m_letterboxColor.a});
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
            std::string extension = std::filesystem::path(path).extension().string();
            std::transform(extension.begin(), extension.end(), extension.begin(),
                [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
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
                    Object* placed = m_prefabPreview;
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
        if (input.hovered)
        {
            if (input.leftClicked && !input.rightDown && !input.middleDown && OnObjectSelected)
                OnObjectSelected(PickObjectInViewport(input.mousePosInViewport, input.available));

            EditorUiVec2 d = input.mouseDelta;

            // Right-button drag → pan.
            if (input.rightDown)
            {
                panDX = d.x;
                panDY = d.y;
            }

            // Middle-button drag → orbit.
            if (input.middleDown)
            {
                orbitDX = d.x;
                orbitDY = d.y;
            }

            // Scroll wheel → zoom.
            zoom = input.mouseWheel;
        }
    }

    ui.EndWindow();

    ApplyCameraControls(panDX, panDY, orbitDX, orbitDY, zoom);
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
        Object* object = objectPointer.get();
        if (!object || !object->IsEnabledInHierarchy() ||
            !object->GetComponent<Mesh>() || IsInObjectHierarchy(object, m_prefabPreview))
            continue;
        Mesh* mesh = object->GetComponent<Mesh>();
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

    // The editor grid is the XZ placement plane. Prefer an object surface when
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

Object* SceneView::PickObjectInViewport(const EditorUiVec2& mousePos, const EditorUiVec2& viewportSize) const
{
    if (!m_scene || viewportSize.x <= 1.f || viewportSize.y <= 1.f)
        return nullptr;

    Camera* cam = m_scene->editorCamera.GetComponent<Camera>();
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

    Object* best = nullptr;
    float bestT = FLT_MAX;

    for (const auto& objPtr : m_scene->GetObjects())
    {
        Object* obj = objPtr.get();
        if (!obj || !obj->IsEnabledInHierarchy() || !obj->GetComponent<Mesh>())
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
    ProjectSettings::AspectRatioMode mode, float lockedAspect, uint32_t windowWidth, uint32_t windowHeight)
{
    outViewportPos = {0.f, 0.f};

    switch (mode)
    {
        case ProjectSettings::AspectRatioMode::Free:
        {
            // Free aspect: use full available size
            outViewportSize = availableSize;
            break;
        }

        case ProjectSettings::AspectRatioMode::Locked:
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

        case ProjectSettings::AspectRatioMode::Hardcoded:
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
                                        float zoom)
{
    if (!m_scene) return;
    if (panDX == 0.f && panDY == 0.f && orbitDX == 0.f && orbitDY == 0.f && zoom == 0.f) return;

    Camera*    cam = m_scene->editorCamera.GetComponent<Camera>();
    assert(cam && "Scene editorCamera must have a Camera component");
    glm::vec3& pos = m_scene->editorCamera.transform.position;

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

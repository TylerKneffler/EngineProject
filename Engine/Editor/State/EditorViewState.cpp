#include "pch.h"
#include "Core/Rendering/RenderSystem.h"
#include "Core/Rendering/SceneRenderer.h"
#include "Engine/Editor/State/EditorViewState.h"
#include "Core/Scene/Object.h"
#include "Core/Scene/Scene.h"
#include "Core/Components/Camera/Camera.h"
#include "Core/Components/Mesh/Mesh.h"
#include "Core/Components/Particles/ParticleEmitter.h"
#include "Core/Components/Terrain/TerrainPlacementHandle.h"
#include <algorithm>
#include <cmath>
#include <memory>
#include <glm/trigonometric.hpp>

namespace Engine::Editor
{
namespace
{
std::unordered_map<const Engine::Scene::Scene*, std::unique_ptr<EditorViewState>>& Registry()
{
    static std::unordered_map<const Engine::Scene::Scene*,
        std::unique_ptr<EditorViewState>> states;
    return states;
}

glm::quat WorldRotation(const Engine::Core::Object& object)
{
    glm::mat3 basis(object.transform.GetWorldMatrix());
    for (int column = 0; column < 3; ++column)
    {
        const float length = glm::length(basis[column]);
        if (length > 0.0001f)
            basis[column] /= length;
    }
    return glm::normalize(glm::quat_cast(basis));
}

struct FocusBounds
{
    glm::vec3 minimum { 0.f };
    glm::vec3 maximum { 0.f };
    bool hasPoints = false;

    void Include(const glm::vec3& point)
    {
        if (!std::isfinite(point.x) || !std::isfinite(point.y) ||
            !std::isfinite(point.z))
            return;
        if (!hasPoints)
        {
            minimum = maximum = point;
            hasPoints = true;
            return;
        }
        minimum = glm::min(minimum, point);
        maximum = glm::max(maximum, point);
    }
};

void IncludeFocusBounds(const Engine::Core::Object& object,
    FocusBounds& meshes, FocusBounds& origins)
{
    const glm::mat4 world = object.transform.GetWorldMatrix();
    origins.Include(glm::vec3(world[3]));
    if (const auto* mesh = object.GetComponent<Engine::Components::Mesh>();
        mesh && mesh->HasBounds())
    {
        const glm::vec3 low = mesh->GetBoundsMin();
        const glm::vec3 high = mesh->GetBoundsMax();
        for (int x = 0; x < 2; ++x)
            for (int y = 0; y < 2; ++y)
                for (int z = 0; z < 2; ++z)
                    meshes.Include(glm::vec3(world * glm::vec4(
                        x ? high.x : low.x, y ? high.y : low.y,
                        z ? high.z : low.z, 1.f)));
    }
    if (const auto* emitter = object.GetComponent<Engine::Components::ParticleEmitter>())
    {
        const auto bounds = emitter->GetWorldBounds(world);
        if (bounds.valid)
        {
            meshes.Include(bounds.minimum);
            meshes.Include(bounds.maximum);
        }
    }
    for (const Engine::Core::Object* child : object.Children)
        if (child)
            IncludeFocusBounds(*child, meshes, origins);
}
}

void EditorViewState::FocusCamera(Object* obj)
{
    m_cameraFollowTarget = obj;
    Engine::Components::Camera* cam = m_scene->editorCamera.GetComponent<Engine::Components::Camera>();
    if (!cam)
        return;

    const glm::vec3 objectPos =
        obj ? obj->transform.GetWorldPosition() : glm::vec3(0.f);
    FocusBounds meshes, origins;
    if (obj)
        IncludeFocusBounds(*obj, meshes, origins);
    const FocusBounds& bounds = meshes.hasPoints ? meshes : origins;
    const glm::vec3 targetPos = bounds.hasPoints
        ? (bounds.minimum + bounds.maximum) * 0.5f : objectPos;
    m_cameraFollowPosition = objectPos;
    if (obj)
        m_cameraFollowRotation = WorldRotation(*obj);
    if (m_scene->IsMode2D())
    {
        m_scene->editorCamera.transform.position.x = targetPos.x;
        m_scene->editorCamera.transform.position.y = targetPos.y;
        m_scene->editorCamera.transform.position.z = -10.f;
        cam->target = { targetPos.x, targetPos.y, 0.f };
        return;
    }
    const float radius = meshes.hasPoints
        ? glm::length((bounds.maximum - bounds.minimum) * 0.5f)
        : std::max(glm::length((bounds.maximum - bounds.minimum) * 0.5f), 0.5f);
    const float halfFov = glm::radians(std::clamp(cam->fov, 1.f, 179.f)) * 0.5f;
    const float distance = std::max(
        radius * 1.2f / std::sin(halfFov), cam->nearPlane * 4.f);

    const glm::vec3& eye = m_scene->editorCamera.transform.position;
    const glm::vec3 oldTarget = cam->target;
    glm::vec3 oldDir = eye - oldTarget;
    float oldLen = glm::length(oldDir);
    if (oldLen < 0.001f)
    {
        oldDir = glm::vec3(0.f, 1.5f, 1.f);
        oldLen = glm::length(oldDir);
    }

    m_scene->editorCamera.transform.position = targetPos + oldDir * (distance / oldLen);
    cam->target = targetPos;
}

void EditorViewState::UpdateCameraFollow()
{
    if (!m_cameraFollowTarget)
        return;

    const glm::vec3 current = m_cameraFollowTarget->transform.GetWorldPosition();
    const glm::quat rotation = WorldRotation(*m_cameraFollowTarget);
    const glm::quat change = rotation * glm::inverse(m_cameraFollowRotation);
    m_scene->editorCamera.transform.position = current +
        change * (glm::vec3(m_scene->editorCamera.transform.position) - m_cameraFollowPosition);
    if (Engine::Components::Camera* cam = m_scene->editorCamera.GetComponent<Engine::Components::Camera>())
    {
        cam->target = current + change * (cam->target - m_cameraFollowPosition);
        cam->up = change * cam->up;
    }
    m_cameraFollowPosition = current;
    m_cameraFollowRotation = rotation;
}

EditorViewState& EditorViewState::For(Scene& scene)
{
    auto& states = Registry();
    auto it = states.find(&scene);
    if (it == states.end())
    {
        auto state = std::make_unique<EditorViewState>(scene);
        Engine::Rendering::RenderSystem::For(scene).SetViewOverlays(state.get());
        scene.AddObserver(state.get());
        it = states.emplace(&scene, std::move(state)).first;
    }
    return *it->second;
}

EditorViewState* EditorViewState::Find(const Scene* scene)
{
    auto& states = Registry();
    const auto it = states.find(scene);
    return it != states.end() ? it->second.get() : nullptr;
}

void EditorViewState::SelectObject(Object* object)
{
    selectedObject = object;
    selectedPlacementId = 0;
    selectedPlacementLayer = nullptr;
    if (const auto* handle = object
            ? object->GetComponent<Engine::Components::TerrainPlacementHandle>() : nullptr;
        handle && handle->placementId != 0 && object->Parent)
    {
        selectedPlacementId = handle->placementId;
        selectedPlacementLayer = object->Parent;
    }
}

void EditorViewState::SetMeshEditPose(bool enabled)
{
    if (meshEditPose == enabled) return;
    meshEditPose = enabled;
    Engine::Rendering::RenderSystem::For(*m_scene).InvalidatePreparedFrame();
}

void EditorViewState::SetWeightPaint(Mesh* mesh, int bone)
{
    if (weightPaintMesh == mesh && weightPaintBone == bone)
        return;
    weightPaintMesh = mesh;
    weightPaintBone = bone;
    Engine::Rendering::RenderSystem::For(*m_scene).InvalidatePreparedFrame();
}

uint8_t EditorViewState::OverlaysOf(const Object* object)
{
    const EditorViewState* state = object ? Find(object->GetScene()) : nullptr;
    if (!state) return 0;
    const auto it = state->overlays.find(object);
    return it != state->overlays.end() ? it->second : 0;
}

uint8_t* EditorViewState::MutableOverlays(Object* object)
{
    Scene* scene = object ? object->GetScene() : nullptr;
    if (!scene)
    {
        static uint8_t scratch = 0;
        scratch = 0;
        return &scratch;
    }
    return &For(*scene).overlays[object];
}

void EditorViewState::OnObjectsRemoving(const std::vector<Object*>& objects)
{
    const auto removed = [&objects](const Object* object)
    {
        return object && std::find(objects.begin(), objects.end(), object)
            != objects.end();
    };
    if (removed(selectedObject))
    {
        selectedObject = nullptr;
        selectedPlacementId = 0;
        selectedPlacementLayer = nullptr;
    }
    if (removed(previewObject))
        previewObject = nullptr;
    if (removed(m_cameraFollowTarget))
        StopCameraFollow();
    if (!overlays.empty())
        for (const Object* object : objects)
            overlays.erase(object);
    // Matches the scene's previous behavior: any removal drops the mesh
    // selection, which may belong to a removed object's component.
    selectedMesh = nullptr;
}

void EditorViewState::OnObjectsClearing()
{
    selectedObject = nullptr;
    selectedPlacementId = 0;
    selectedPlacementLayer = nullptr;
    selectedMesh = nullptr;
    previewObject = nullptr;
    m_cameraFollowTarget = nullptr;
    overlays.clear();
}

void EditorViewState::OnSceneDestroying()
{
    // Releases this object; nothing may touch members afterwards.
    Registry().erase(m_scene);
}
}

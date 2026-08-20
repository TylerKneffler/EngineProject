#include "ScenePlacementAndPicking.h"

#include "Core/Compoonents/Camera.h"
#include "Core/Compoonents/Mesh.h"
#include "Core/Compoonents/Sprite.h"
#include "Core/Scene/Scene.h"
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <limits>
#include <glm/ext/matrix_transform.hpp>
#include <glm/geometric.hpp>
#include <glm/gtc/matrix_inverse.hpp>

namespace Engine::Editor
{
namespace
{
bool BuildViewportRay(const Engine::Scene::Scene& scene,
    const EditorUiVec2& mousePos, const EditorUiVec2& viewportSize,
    glm::vec3& origin, glm::vec3& direction)
{
    if (viewportSize.x <= 1.f || viewportSize.y <= 1.f)
        return false;
    const Engine::Components::Camera* camera =
        scene.editorCamera.GetComponent<Engine::Components::Camera>();
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

bool IsInObjectHierarchy(const Engine::Core::Object* object,
    const Engine::Core::Object* root)
{
    for (const Engine::Core::Object* current = object; current;
        current = current->Parent)
    {
        if (current == root)
            return true;
    }
    return false;
}

bool IntersectMeshBounds(const Engine::Core::Object& object,
    const Engine::Components::Mesh& mesh, const glm::vec3& rayOrigin,
    const glm::vec3& rayDirection, float& distance)
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
            {
                return false;
            }
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

bool ScenePlacementAndPicking::FindPrefabPlacement(
    const Engine::Scene::Scene& scene, const Engine::Core::Object* prefabPreview,
    const EditorUiVec2& mousePos, const EditorUiVec2& viewportSize,
    glm::vec3& placement)
{
    glm::vec3 rayOrigin{};
    glm::vec3 rayDirection{};
    if (!BuildViewportRay(scene, mousePos, viewportSize, rayOrigin, rayDirection))
        return false;

    float bestDistance = FLT_MAX;
    bool found = false;
    for (const auto& objectPointer : scene.GetObjects())
    {
        Engine::Core::Object* object = objectPointer.get();
        if (!object || !object->IsEnabledInHierarchy() ||
            !object->GetComponent<Engine::Components::Mesh>() ||
            IsInObjectHierarchy(object, prefabPreview))
        {
            continue;
        }

        Engine::Components::Mesh* mesh =
            object->GetComponent<Engine::Components::Mesh>();
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

    if (scene.IsEditorMode2D())
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

Engine::Core::Object* ScenePlacementAndPicking::PickObjectInViewport(
    const Engine::Scene::Scene& scene, const EditorUiVec2& mousePos,
    const EditorUiVec2& viewportSize)
{
    if (viewportSize.x <= 1.f || viewportSize.y <= 1.f)
        return nullptr;

    const Engine::Components::Camera* cam =
        scene.editorCamera.GetComponent<Engine::Components::Camera>();
    if (!cam)
        return nullptr;

    const glm::mat4 view = cam->GetViewMatrix();
    const glm::mat4 proj = cam->GetProjectionMatrix(viewportSize.x / viewportSize.y);
    const glm::mat4 invVP = glm::inverse(proj * view);

    const float ndcX = (2.f * mousePos.x / viewportSize.x) - 1.f;
    const float ndcY = 1.f - (2.f * mousePos.y / viewportSize.y);

    glm::vec4 nearH = invVP * glm::vec4(ndcX, ndcY, 0.f, 1.f);
    glm::vec4 farH = invVP * glm::vec4(ndcX, ndcY, 1.f, 1.f);
    if (nearH.w == 0.f || farH.w == 0.f)
        return nullptr;

    const glm::vec3 rayOrigin = glm::vec3(nearH) / nearH.w;
    const glm::vec3 rayTarget = glm::vec3(farH) / farH.w;
    const glm::vec3 rayDir = glm::normalize(rayTarget - rayOrigin);

    if (scene.IsEditorMode2D())
    {
        Engine::Core::Object* frontmost = nullptr;
        int frontLayer = std::numeric_limits<int>::min();
        float frontZ = -FLT_MAX;
        for (const auto& objPtr : scene.GetObjects())
        {
            Engine::Core::Object* obj = objPtr.get();
            Engine::Components::Sprite* sprite = obj && obj->IsEnabledInHierarchy()
                ? obj->GetComponent<Engine::Components::Sprite>() : nullptr;
            if (!sprite || std::abs(rayDir.z) < 0.000001f)
                continue;

            glm::mat4 world = obj->transform.GetWorldMatrix();
            world[3].z = 0.f;
            const glm::vec2 spriteSize = sprite->GetWorldSize();
            world = world * glm::scale(glm::mat4(1.f), glm::vec3(spriteSize, 1.f));
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
        for (const auto& objPtr : scene.GetObjects())
        {
            Engine::Core::Object* obj = objPtr.get();
            Engine::Components::Mesh* mesh = obj && obj->IsEnabledInHierarchy()
                ? obj->GetComponent<Engine::Components::Mesh>() : nullptr;
            float distance = 0.f;
            if (mesh && IntersectMeshBounds(*obj, *mesh, rayOrigin,
                rayDir, distance) && distance < closestDistance)
            {
                closestDistance = distance;
                closestMesh = obj;
            }
        }
        return closestMesh;
    }

    Engine::Core::Object* best = nullptr;
    float bestDistance = FLT_MAX;
    for (const auto& objPtr : scene.GetObjects())
    {
        Engine::Core::Object* obj = objPtr.get();
        if (!obj || !obj->IsEnabledInHierarchy() ||
            (!obj->GetComponent<Engine::Components::Mesh>() &&
            !obj->GetComponent<Engine::Components::Sprite>()))
        {
            continue;
        }

        const glm::vec3 center = obj->transform.GetWorldPosition();
        const float radius = glm::max(
            glm::max(obj->transform.scale.x, obj->transform.scale.y),
            obj->transform.scale.z) * 0.75f;
        const glm::vec3 oc = rayOrigin - center;
        const float a = glm::dot(rayDir, rayDir);
        const float b = 2.f * glm::dot(oc, rayDir);
        const float c = glm::dot(oc, oc) - (radius * radius);
        const float discriminant = b * b - 4.f * a * c;
        if (discriminant < 0.f)
            continue;

        const float sqrtDiscriminant = std::sqrt(discriminant);
        const float t0 = (-b - sqrtDiscriminant) / (2.f * a);
        const float t1 = (-b + sqrtDiscriminant) / (2.f * a);
        float distance = t0 > 0.f ? t0 : t1;
        if (distance > 0.f && distance < bestDistance)
        {
            bestDistance = distance;
            best = obj;
        }
    }

    return best;
}
}

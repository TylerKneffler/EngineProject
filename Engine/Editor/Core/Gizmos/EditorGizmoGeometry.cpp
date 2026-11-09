#include "EditorGizmoSystem.h"
#include "Editor/Core/View/EditorViewportSettings.h"
#include "Core/Scene/Scene.h"
#include "Core/Object.h"
#include "Core/Compoonents/Camera/Camera.h"
#include "Core/Compoonents/Lighting/Light.h"
#include "Core/Compoonents/Lighting/LightProbe.h"
#include "Core/Compoonents/Mesh/Mesh.h"
#include "Core/Compoonents/Sprite/Sprite.h"
#include "Core/Compoonents/Particles/ParticleEmitter.h"
#include "Core/Compoonents/Physics/RigidBody.h"
#include "Core/Compoonents/Physics/Collider.h"
#include "Core/Compoonents/Physics/PrimitiveObjectCollider.h"
#include "Core/Compoonents/Physics/MeshObjectCollider.h"
#include "Core/Compoonents/Animation/Skeleton.h"
#include "Core/Compoonents/Animation/BoneGroup.h"
#include "Core/Compoonents/Animation/AnimationBone.h"
#include "Core/Compoonents/Animation/Model.h"
#include "Core/Compoonents/Animation/TwoBoneIK.h"
#include "Core/Compoonents/Animation/ChainIK.h"
#include "Editor/Core/View/Views/Scene/ScenePlacementAndPicking.h"
#include <algorithm>
#include <cmath>
#include <cfloat>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include "EditorGizmoGeometry.h"

namespace Engine::Editor::GizmoGeometry
{
const Engine::Components::Skeleton& DisplaySkeleton(
    const Engine::Components::Skeleton& skeleton)
{
    if (auto* model = skeleton.ResolveModel(); model && model->Owner)
        if (auto* root = model->Owner->GetComponent<
                Engine::Components::Skeleton>();
            root && !root->rootBoneNodes.empty())
            return *root;
    return skeleton;
}

EditorUiVec2 Add(EditorUiVec2 a, EditorUiVec2 b)
{
    return { a.x + b.x, a.y + b.y };
}

EditorUiVec2 Subtract(EditorUiVec2 a, EditorUiVec2 b)
{
    return { a.x - b.x, a.y - b.y };
}

EditorUiVec2 Multiply(EditorUiVec2 value, float scale)
{
    return { value.x * scale, value.y * scale };
}

float Dot(EditorUiVec2 a, EditorUiVec2 b)
{
    return a.x * b.x + a.y * b.y;
}

float Length(EditorUiVec2 value)
{
    return std::sqrt(Dot(value, value));
}

int DragDirection(float value)
{
    return value > 1e-5f ? 1 : value < -1e-5f ? -1 : 0;
}

glm::mat3 WorldRotation(const Engine::Core::Object& object)
{
    const glm::vec3& rotation = object.transform.rotation;
    const glm::mat4 local =
        glm::rotate(glm::mat4(1.f), rotation.z, { 0.f, 0.f, 1.f }) *
        glm::rotate(glm::mat4(1.f), rotation.y, { 0.f, 1.f, 0.f }) *
        glm::rotate(glm::mat4(1.f), rotation.x, { 1.f, 0.f, 0.f });
    return object.Parent
        ? WorldRotation(*object.Parent) * glm::mat3(local)
        : glm::mat3(local);
}

glm::vec3 WorldPosition(Engine::Core::Object* object)
{
    return object ? object->transform.GetWorldPosition() : glm::vec3(0.f);
}

EditorUiColor PoseConnectionColor(float ratio)
{
    if (ratio > 1.025f) return {.98f, .42f, .3f, 1.f};
    if (ratio < .975f) return {.38f, .62f, 1.f, 1.f};
    return {.38f, .75f, .4f, 1.f};
}

bool ControllerFacingRotation(const Engine::Core::Object& joint,
    const Engine::Components::AnimationBone& controller,
    glm::quat& rotation)
{
    if (!controller.poseFacingEnabled ||
        controller.poseFacingWeight <= 0.f ||
        glm::dot(controller.poseFacingAxis,
            controller.poseFacingAxis) < 1e-8f) return false;
    const glm::vec3 direction = controller.poseFacingTarget -
        joint.transform.GetWorldPosition();
    if (glm::dot(direction, direction) < 1e-8f) return false;
    const glm::vec3 forward = glm::normalize(WorldRotation(joint) *
        glm::normalize(controller.poseFacingAxis));
    const glm::quat delta(forward, glm::normalize(direction));
    const glm::quat weighted = glm::slerp(
        glm::quat(1.f, 0.f, 0.f, 0.f), delta,
        std::clamp(controller.poseFacingWeight, 0.f, 1.f));
    rotation = glm::normalize(weighted *
        glm::quat_cast(WorldRotation(joint)));
    return true;
}

bool ApplyControllerFacing(Engine::Core::Object& joint,
    const Engine::Components::AnimationBone& controller)
{
    glm::quat desired;
    if (!ControllerFacingRotation(joint, controller, desired))
        return false;
    const glm::quat parent = joint.Parent
        ? glm::quat_cast(WorldRotation(*joint.Parent))
        : glm::quat(1.f, 0.f, 0.f, 0.f);
    joint.transform.rotation = glm::eulerAngles(glm::normalize(
        glm::inverse(parent) * desired));
    return true;
}

bool ProjectPoint(const glm::mat4& viewProjection, const glm::vec3& world,
    const EditorUiVec2& viewport, EditorUiVec2& screen)
{
    const glm::vec4 clip = viewProjection * glm::vec4(world, 1.f);
    if (clip.w <= 0.0001f)
        return false;
    const glm::vec3 ndc = glm::vec3(clip) / clip.w;
    if (ndc.z < 0.f || ndc.z > 1.f)
        return false;
    screen = {
        (ndc.x * 0.5f + 0.5f) * viewport.x,
        (0.5f - ndc.y * 0.5f) * viewport.y
    };
    return true;
}

bool RotationPlaneDirection(const glm::mat4& inverseViewProjection,
    const EditorUiVec2& mouse, const EditorUiVec2& viewport,
    const glm::vec3& pivot, const glm::vec3& axis,
    glm::vec3& direction)
{
    if (viewport.x <= 1.f || viewport.y <= 1.f) return false;
    const float x = 2.f * mouse.x / viewport.x - 1.f;
    const float y = 1.f - 2.f * mouse.y / viewport.y;
    const glm::vec4 nearClip = inverseViewProjection * glm::vec4(x, y, 0.f, 1.f);
    const glm::vec4 farClip = inverseViewProjection * glm::vec4(x, y, 1.f, 1.f);
    if (std::abs(nearClip.w) < 1e-6f || std::abs(farClip.w) < 1e-6f)
        return false;
    const glm::vec3 origin = glm::vec3(nearClip) / nearClip.w;
    const glm::vec3 ray = glm::normalize(glm::vec3(farClip) / farClip.w - origin);
    const float denominator = glm::dot(ray, axis);
    // Edge-on rings have an unstable plane intersection. Their projected
    // tangent remains available as the drag fallback.
    if (std::abs(denominator) < .08f) return false;
    const float distance = glm::dot(pivot - origin, axis) / denominator;
    if (distance <= 0.f) return false;
    direction = origin + ray * distance - pivot;
    const float length = glm::length(direction);
    if (!std::isfinite(length) || length < 1e-5f) return false;
    direction /= length;
    return true;
}

bool ViewPlanePoint(const glm::mat4& inverseViewProjection,
    EditorUiVec2 mouse, EditorUiVec2 viewport, glm::vec3 pivot,
    glm::vec3 normal, glm::vec3& point)
{
    if (viewport.x <= 1.f || viewport.y <= 1.f) return false;
    const float x = 2.f * mouse.x / viewport.x - 1.f;
    const float y = 1.f - 2.f * mouse.y / viewport.y;
    const glm::vec4 nearClip = inverseViewProjection * glm::vec4(x, y, 0.f, 1.f);
    const glm::vec4 farClip = inverseViewProjection * glm::vec4(x, y, 1.f, 1.f);
    if (std::abs(nearClip.w) < 1e-6f || std::abs(farClip.w) < 1e-6f)
        return false;
    const glm::vec3 origin = glm::vec3(nearClip) / nearClip.w;
    const glm::vec3 ray = glm::normalize(glm::vec3(farClip) / farClip.w - origin);
    const float denominator = glm::dot(ray, normal);
    if (std::abs(denominator) < 1e-5f) return false;
    const float distance = glm::dot(pivot - origin, normal) / denominator;
    if (distance <= 0.f) return false;
    point = origin + ray * distance;
    return true;
}

float DistanceToSegment(EditorUiVec2 point, EditorUiVec2 start,
    EditorUiVec2 end, float& parameter)
{
    const EditorUiVec2 segment = Subtract(end, start);
    const float lengthSquared = Dot(segment, segment);
    parameter = lengthSquared > 0.0001f
        ? std::clamp(Dot(Subtract(point, start), segment) / lengthSquared, 0.f, 1.f)
        : 0.f;
    return Length(Subtract(point, Add(start, Multiply(segment, parameter))));
}

void DrawSpotGizmo(IEditorUi& ui, const glm::mat4& viewProjection,
    const EditorUiVec2& viewport, const Engine::Core::Object& object,
    const Engine::Components::Light& light);

void DrawWorldBox(IEditorUi& ui, const glm::mat4& viewProjection,
    EditorUiVec2 viewport, const glm::mat4& world,
    glm::vec3 minimum, glm::vec3 maximum, EditorUiColor color)
{
    const glm::vec3 corners[8] = {
        {minimum.x,minimum.y,minimum.z},{maximum.x,minimum.y,minimum.z},
        {maximum.x,maximum.y,minimum.z},{minimum.x,maximum.y,minimum.z},
        {minimum.x,minimum.y,maximum.z},{maximum.x,minimum.y,maximum.z},
        {maximum.x,maximum.y,maximum.z},{minimum.x,maximum.y,maximum.z}
    };
    EditorUiVec2 projected[8]{};
    bool visible[8]{};
    for (int i=0;i<8;++i)
        visible[i]=ProjectPoint(viewProjection,
            glm::vec3(world*glm::vec4(corners[i],1.f)),viewport,projected[i]);
    constexpr int edges[12][2] = {
        {0,1},{1,2},{2,3},{3,0},{4,5},{5,6},{6,7},{7,4},
        {0,4},{1,5},{2,6},{3,7}
    };
    for (const auto& edge:edges)
        if (visible[edge[0]] && visible[edge[1]])
            ui.DrawViewportLine(projected[edge[0]],projected[edge[1]],color,1.5f);
}

void DrawWorldLine(IEditorUi& ui, const glm::mat4& viewProjection,
    EditorUiVec2 viewport, glm::vec3 a, glm::vec3 b, EditorUiColor color)
{
    EditorUiVec2 start{}, end{};
    if (ProjectPoint(viewProjection, a, viewport, start) &&
        ProjectPoint(viewProjection, b, viewport, end))
        ui.DrawViewportLine(start, end, color, 1.25f);
}

void DrawColliderRing(IEditorUi& ui, const glm::mat4& viewProjection,
    EditorUiVec2 viewport, const glm::mat4& world, float radius,
    float height, int plane, EditorUiColor color)
{
    glm::vec3 previous{};
    for (int step = 0; step <= 32; ++step)
    {
        const float angle = 6.28318530718f * step / 32.f;
        glm::vec3 point(0.f);
        point[(plane + 1) % 3] = std::cos(angle) * radius;
        point[(plane + 2) % 3] = std::sin(angle) * radius;
        point[plane] = height;
        const glm::vec3 current = glm::vec3(world * glm::vec4(point, 1.f));
        if (step) DrawWorldLine(ui, viewProjection, viewport, previous, current, color);
        previous = current;
    }
}

void DrawColliderOverlay(IEditorUi& ui, const glm::mat4& viewProjection,
    EditorUiVec2 viewport, const Engine::Core::Object& object,
    const Engine::Components::Collider& collider)
{
    if (!collider.collisionEnabled) return;
    const glm::mat4 world = object.transform.GetWorldMatrix() *
        glm::translate(glm::mat4(1.f), collider.center);
    const EditorUiColor color{.3f, 1.f, .62f, .85f};
    if (const auto* primitive = dynamic_cast<const Engine::Components::PrimitiveObjectCollider*>(&collider))
    {
        const float radius = std::max(primitive->radius, .001f);
        if (primitive->shape == "Sphere" || primitive->shape == "Circle")
        {
            for (int plane = 0; plane < 3; ++plane)
                DrawColliderRing(ui, viewProjection, viewport, world,
                    radius, 0.f, plane, color);
        }
        else if (primitive->shape == "Capsule" || primitive->shape == "Cylinder")
        {
            const float half = std::max(primitive->height, 0.f) * .5f;
            DrawColliderRing(ui, viewProjection, viewport, world, radius, -half, 1, color);
            DrawColliderRing(ui, viewProjection, viewport, world, radius, half, 1, color);
            for (int side = 0; side < 4; ++side)
            {
                const float angle = 1.57079632679f * side;
                const glm::vec3 radial(std::cos(angle) * radius, 0.f,
                    std::sin(angle) * radius);
                DrawWorldLine(ui, viewProjection, viewport,
                    glm::vec3(world * glm::vec4(radial + glm::vec3(0.f, -half, 0.f), 1.f)),
                    glm::vec3(world * glm::vec4(radial + glm::vec3(0.f, half, 0.f), 1.f)), color);
            }
        }
        else
        {
            const glm::vec3 half = glm::max(glm::abs(primitive->size) * .5f,
                glm::vec3(.001f));
            DrawWorldBox(ui, viewProjection, viewport, world, -half, half, color);
        }
    }
    else if (dynamic_cast<const Engine::Components::MeshObjectCollider*>(&collider))
    {
        const auto* mesh = object.GetComponent<Engine::Components::Mesh>();
        if (!mesh) return;
        const auto& vertices = mesh->GetVertices();
        const auto& indices = mesh->GetIndices();
        if (vertices.empty())
        {
            if (mesh->HasBounds()) DrawWorldBox(ui, viewProjection, viewport,
                world, mesh->GetBoundsMin(), mesh->GetBoundsMax(), color);
            return;
        }
        const size_t triangleCount = indices.empty() ? vertices.size() / 3 : indices.size() / 3;
        const size_t stride = std::max<size_t>(1, (triangleCount + 999) / 1000);
        for (size_t triangle = 0; triangle < triangleCount; triangle += stride)
        {
            glm::vec3 points[3];
            bool valid = true;
            for (int corner = 0; corner < 3; ++corner)
            {
                const size_t index = indices.empty() ? triangle * 3 + corner :
                    indices[triangle * 3 + corner];
                if (index >= vertices.size()) { valid = false; break; }
                const auto& vertex = vertices[index];
                points[corner] = glm::vec3(world * glm::vec4(
                    vertex.pos[0], vertex.pos[1], vertex.pos[2], 1.f));
            }
            if (!valid) continue;
            for (int edge = 0; edge < 3; ++edge)
                DrawWorldLine(ui, viewProjection, viewport,
                    points[edge], points[(edge + 1) % 3], color);
        }
    }
}

void DrawMeshNormals(IEditorUi& ui, const glm::mat4& viewProjection,
    EditorUiVec2 viewport, const Engine::Core::Object& object,
    const Engine::Components::Mesh& mesh)
{
    const auto& vertices = mesh.GetVertices();
    const auto& terrain = mesh.GetTerrainVertices();
    const size_t count = terrain.empty() ? vertices.size() : terrain.size();
    if (!count) return;
    const glm::mat4 world = object.transform.GetWorldMatrix();
    if (std::abs(glm::determinant(glm::mat3(world))) < 1e-8f) return;
    const glm::mat3 normalMatrix = glm::transpose(glm::inverse(glm::mat3(world)));
    const float length = mesh.HasBounds() ? std::clamp(
        glm::length(mesh.GetBoundsMax() - mesh.GetBoundsMin()) * .035f,
        .025f, .5f) : .1f;
    const size_t stride = std::max<size_t>(1, (count + 511) / 512);
    const EditorUiColor color{.22f, .82f, 1.f, .9f};
    for (size_t index = 0; index < count; index += stride)
    {
        const auto& vertex = terrain.empty()
            ? static_cast<const Engine::Model::Vertex&>(vertices[index])
            : static_cast<const Engine::Model::Vertex&>(terrain[index]);
        const glm::vec3 normal = normalMatrix * glm::vec3(
            vertex.normal[0], vertex.normal[1], vertex.normal[2]);
        if (glm::dot(normal, normal) < 1e-8f) continue;
        const glm::vec3 start = glm::vec3(world * glm::vec4(
            vertex.pos[0], vertex.pos[1], vertex.pos[2], 1.f));
        DrawWorldLine(ui, viewProjection, viewport, start,
            start + glm::normalize(normal) * length, color);
    }
}

void DrawLightVolume(IEditorUi& ui, const glm::mat4& viewProjection,
    EditorUiVec2 viewport, const Engine::Core::Object& object,
    const Engine::Components::Light& light)
{
    if (light.GetLightType()==Engine::Components::Light::Type::Spot)
    {
        DrawSpotGizmo(ui,viewProjection,viewport,object,light);
        return;
    }
    const glm::vec3 center=glm::vec3(object.transform.GetWorldMatrix()[3]);
    const EditorUiColor color{1.f,.72f,.16f,.85f};
    if (light.GetLightType()==Engine::Components::Light::Type::Directional)
    {
        const glm::vec3 direction=glm::normalize(
            glm::vec3(object.transform.GetWorldMatrix()[2]));
        EditorUiVec2 start{},end{};
        if (ProjectPoint(viewProjection,center,viewport,start) &&
            ProjectPoint(viewProjection,center+direction*2.f,viewport,end))
            ui.DrawViewportLine(start,end,color,2.f);
        return;
    }
    const float radius=std::max(light.range,.01f);
    for (int plane=0;plane<3;++plane)
    {
        EditorUiVec2 previous{};
        bool previousVisible=false;
        for (int step=0;step<=64;++step)
        {
            const float angle=6.28318530718f*step/64.f;
            glm::vec3 offset(0.f);
            offset[(plane+1)%3]=std::cos(angle)*radius;
            offset[(plane+2)%3]=std::sin(angle)*radius;
            EditorUiVec2 current{};
            const bool visible=ProjectPoint(viewProjection,center+offset,
                viewport,current);
            if (visible && previousVisible)
                ui.DrawViewportLine(previous,current,color,1.25f);
            previous=current;
            previousVisible=visible;
        }
    }
}

void DrawCameraFrustum(IEditorUi& ui, const glm::mat4& viewProjection,
    EditorUiVec2 viewport, const Engine::Components::Camera& camera)
{
    const float aspect=viewport.x/std::max(viewport.y,1.f);
    const glm::mat4 inverse=glm::inverse(
        camera.GetProjectionMatrix(aspect)*camera.GetViewMatrix());
    const glm::vec2 corners[4]={{-1.f,-1.f},{1.f,-1.f},
        {1.f,1.f},{-1.f,1.f}};
    EditorUiVec2 screen[8]{};
    bool visible[8]{};
    for (int plane=0;plane<2;++plane)
        for (int i=0;i<4;++i)
        {
            const glm::vec4 point=inverse*glm::vec4(corners[i],
                plane?1.f:0.f,1.f);
            if (std::abs(point.w)<1e-6f) continue;
            visible[plane*4+i]=ProjectPoint(viewProjection,
                glm::vec3(point)/point.w,viewport,screen[plane*4+i]);
        }
    constexpr int edges[12][2] = {
        {0,1},{1,2},{2,3},{3,0},{4,5},{5,6},{6,7},{7,4},
        {0,4},{1,5},{2,6},{3,7}
    };
    const EditorUiColor color{.25f,.82f,1.f,.8f};
    for (const auto& edge:edges)
        if (visible[edge[0]] && visible[edge[1]])
            ui.DrawViewportLine(screen[edge[0]],screen[edge[1]],color,1.5f);
}

void DrawLightIcon(IEditorUi& ui, EditorUiVec2 center, bool selected)
{
    if (selected)
        ui.DrawViewportCircle(center, 13.f, { 1.f, 1.f, 1.f, 0.9f }, false, 2.f);
    const EditorUiVec2 bulb{ center.x, center.y - 2.f };
    ui.DrawViewportCircle(bulb, 6.f, kOutline, true);
    ui.DrawViewportCircle(bulb, 4.5f, { 1.f, 0.78f, 0.18f, 1.f }, true);
    ui.DrawViewportLine({ center.x - 4.f, center.y + 4.f },
        { center.x + 4.f, center.y + 4.f }, kOutline, 3.f);
    ui.DrawViewportLine({ center.x - 3.f, center.y + 7.f },
        { center.x + 3.f, center.y + 7.f }, { 1.f, 0.78f, 0.18f, 1.f }, 2.f);
    for (int ray = 0; ray < 8; ++ray)
    {
        const float angle = static_cast<float>(ray) * 0.785398163f;
        const EditorUiVec2 direction{ std::cos(angle), std::sin(angle) };
        ui.DrawViewportLine(Add(bulb, Multiply(direction, 8.f)),
            Add(bulb, Multiply(direction, 11.f)),
            { 1.f, 0.78f, 0.18f, 0.95f }, 2.f);
    }
}

void DrawSpotGizmo(IEditorUi& ui, const glm::mat4& viewProjection,
    const EditorUiVec2& viewport, const Engine::Core::Object& object,
    const Engine::Components::Light& light)
{
    const glm::mat4 world = object.transform.GetWorldMatrix();
    const glm::vec3 origin(world[3]);
    const glm::vec3 direction = glm::normalize(glm::vec3(world[2]));
    const glm::vec3 right = glm::normalize(glm::vec3(world[0]));
    const glm::vec3 up = glm::normalize(glm::vec3(world[1]));
    const float length = std::max(light.range, 0.01f);
    const float radius = std::tan(glm::radians(std::clamp(
        light.outerConeAngle, 0.1f, 89.f))) * length;
    const glm::vec3 center = origin + direction * length;
    const glm::vec3 rim[4] = { center + right * radius, center + up * radius,
        center - right * radius, center - up * radius };
    EditorUiVec2 originScreen{}, rimScreen[4]{};
    if (!ProjectPoint(viewProjection, origin, viewport, originScreen)) return;
    bool visible[4]{};
    for (int index = 0; index < 4; ++index)
    {
        visible[index] = ProjectPoint(viewProjection, rim[index], viewport,
            rimScreen[index]);
        if (visible[index])
            ui.DrawViewportLine(originScreen, rimScreen[index],
                { 1.f, 0.72f, 0.16f, 0.8f }, 1.25f);
    }
    for (int index = 0; index < 4; ++index)
        if (visible[index] && visible[(index + 1) % 4])
            ui.DrawViewportLine(rimScreen[index], rimScreen[(index + 1) % 4],
                { 1.f, 0.72f, 0.16f, 0.8f }, 1.25f);
}

void DrawCameraIcon(IEditorUi& ui, EditorUiVec2 center, bool selected)
{
    const EditorUiColor color{ 0.25f, 0.82f, 1.f, 1.f };
    if (selected)
        ui.DrawViewportCircle(center, 14.f, { 1.f, 1.f, 1.f, 0.9f }, false, 2.f);
    const EditorUiVec2 a{ center.x - 8.f, center.y - 6.f };
    const EditorUiVec2 b{ center.x + 4.f, center.y - 6.f };
    const EditorUiVec2 c{ center.x + 4.f, center.y + 6.f };
    const EditorUiVec2 d{ center.x - 8.f, center.y + 6.f };
    ui.DrawViewportLine(a, b, kOutline, 5.f);
    ui.DrawViewportLine(b, c, kOutline, 5.f);
    ui.DrawViewportLine(c, d, kOutline, 5.f);
    ui.DrawViewportLine(d, a, kOutline, 5.f);
    ui.DrawViewportLine(a, b, color, 2.f);
    ui.DrawViewportLine(b, c, color, 2.f);
    ui.DrawViewportLine(c, d, color, 2.f);
    ui.DrawViewportLine(d, a, color, 2.f);
    ui.DrawViewportTriangle({ center.x + 5.f, center.y - 5.f },
        { center.x + 12.f, center.y - 9.f },
        { center.x + 12.f, center.y + 9.f }, kOutline);
    ui.DrawViewportTriangle({ center.x + 4.f, center.y - 3.f },
        { center.x + 10.f, center.y - 6.f },
        { center.x + 10.f, center.y + 6.f }, color);
}

void DrawProbeIcon(IEditorUi& ui,EditorUiVec2 center,bool selected,bool volume)
{
    const EditorUiColor color=volume
        ?EditorUiColor{.75f,.42f,1.f,1.f}
        :EditorUiColor{1.f,.78f,.18f,1.f};
    if(selected)
        ui.DrawViewportCircle(center,12.f,{1.f,1.f,1.f,.9f},false,2.f);
    ui.DrawViewportCircle(center,volume?7.f:6.f,kOutline,true);
    ui.DrawViewportCircle(center,volume?5.f:4.f,color,true);
    ui.DrawViewportLine({center.x-10.f,center.y},
        {center.x+10.f,center.y},color,1.5f);
    ui.DrawViewportLine({center.x,center.y-10.f},
        {center.x,center.y+10.f},color,1.5f);
}

void DrawBoxHandle(IEditorUi& ui,EditorUiVec2 center,float half,
    EditorUiColor color)
{
    const EditorUiVec2 topLeft{center.x-half,center.y-half};
    const EditorUiVec2 topRight{center.x+half,center.y-half};
    const EditorUiVec2 bottomRight{center.x+half,center.y+half};
    const EditorUiVec2 bottomLeft{center.x-half,center.y+half};
    ui.DrawViewportTriangle(topLeft,topRight,bottomRight,kOutline);
    ui.DrawViewportTriangle(topLeft,bottomRight,bottomLeft,kOutline);
    const float inset=1.5f;
    ui.DrawViewportTriangle({topLeft.x+inset,topLeft.y+inset},
        {topRight.x-inset,topRight.y+inset},
        {bottomRight.x-inset,bottomRight.y-inset},color);
    ui.DrawViewportTriangle({topLeft.x+inset,topLeft.y+inset},
        {bottomRight.x-inset,bottomRight.y-inset},
        {bottomLeft.x+inset,bottomLeft.y-inset},color);
}

bool SceneContains(const Engine::Scene::Scene& scene, const Engine::Core::Object* object)
{
    for (const auto& candidate : scene.GetObjects())
        if (candidate.get() == object)
            return true;
    return false;
}

void DrawBoneShape(IEditorUi& ui, EditorUiVec2 root, EditorUiVec2 tip,
    bool selected, float opacity)
{
    const EditorUiVec2 difference = Subtract(tip, root);
    const float length = Length(difference);
    if (length < 1.f) return;
    const EditorUiVec2 direction = Multiply(difference, 1.f / length);
    const EditorUiVec2 perpendicular{ -direction.y, direction.x };
    const float width = std::clamp(length * 0.13f, 3.f, 11.f);
    const EditorUiVec2 shoulder = Add(root,
        Multiply(direction, std::clamp(length * 0.22f, 5.f, 22.f)));
    const EditorUiVec2 first = Add(shoulder, Multiply(perpendicular, width));
    const EditorUiVec2 second = Subtract(shoulder, Multiply(perpendicular, width));
    EditorUiColor fill = selected
        ? EditorUiColor{ 1.f, 0.72f, 0.16f, 0.75f }
        : EditorUiColor{ 0.35f, 0.78f, 1.f, 0.58f };
    EditorUiColor line = selected
        ? kHoverColor : EditorUiColor{ 0.45f, 0.86f, 1.f, 1.f };
    EditorUiColor outline = kOutline;
    fill.a *= opacity;
    line.a *= opacity;
    outline.a *= opacity;
    ui.DrawViewportTriangle(root, first, tip, fill);
    ui.DrawViewportTriangle(root, tip, second, fill);
    ui.DrawViewportLine(root, first, outline, 4.f);
    ui.DrawViewportLine(first, tip, outline, 4.f);
    ui.DrawViewportLine(tip, second, outline, 4.f);
    ui.DrawViewportLine(second, root, outline, 4.f);
    ui.DrawViewportLine(root, first, line, 1.5f);
    ui.DrawViewportLine(first, tip, line, 1.5f);
    ui.DrawViewportLine(tip, second, line, 1.5f);
    ui.DrawViewportLine(second, root, line, 1.5f);
}
} // namespace Engine::Editor::GizmoGeometry

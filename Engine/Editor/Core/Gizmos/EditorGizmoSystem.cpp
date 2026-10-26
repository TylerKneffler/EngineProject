#include "EditorGizmoSystem.h"
#include "Editor/Core/View/EditorViewportSettings.h"
#include "Core/Scene/Scene.h"
#include "Core/Object.h"
#include "Core/Compoonents/Camera/Camera.h"
#include "Core/Compoonents/Lighting/Light.h"
#include "Core/Compoonents/Lighting/LightProbe.h"
#include "Core/Compoonents/Obj/Mesh.h"
#include "Core/Compoonents/Obj/Sprite.h"
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

namespace Engine::Editor
{
namespace
{
constexpr EditorUiColor kXColor{ 0.95f, 0.20f, 0.18f, 1.f };
constexpr EditorUiColor kYColor{ 0.25f, 0.85f, 0.30f, 1.f };
constexpr EditorUiColor kZColor{ 0.22f, 0.48f, 1.f, 1.f };
constexpr EditorUiColor kHoverColor{ 1.f, 0.82f, 0.16f, 1.f };
constexpr EditorUiColor kOutline{ 0.04f, 0.04f, 0.05f, 0.92f };

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
    bool selected, float opacity = 1.f)
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
}

bool EditorGizmoSystem::IsModelPinned(
    const Engine::Core::Object* joint, bool rotation) const
{
    const auto found = std::find_if(m_modelPins.begin(), m_modelPins.end(),
        [joint](const ModelPin& pin) { return pin.joint == joint; });
    const auto* bone = joint ? joint->GetComponent<
        Engine::Components::AnimationBone>() : nullptr;
    if (bone) return rotation ? bone->posePinRotation :
        bone->posePinPosition;
    return found != m_modelPins.end() &&
        (rotation ? found->rotation : found->position);
}

bool EditorGizmoSystem::IsModelControllerActive(
    const Engine::Core::Object* joint) const
{
    const auto* bone = joint ? joint->GetComponent<
        Engine::Components::AnimationBone>() : nullptr;
    return bone && bone->poseControllerActive;
}

void EditorGizmoSystem::SetModelControllerActive(
    Engine::Core::Object* joint, bool active)
{
    auto* bone = joint ? joint->GetComponent<
        Engine::Components::AnimationBone>() : nullptr;
    if (!bone || bone->poseControllerActive == active) return;
    bone->poseControllerActive = active;
    if (active) bone->poseControllerTarget = WorldPosition(joint);
    bone->MarkConfigurationDirty();
}

bool EditorGizmoSystem::ApplyModelFacing(Engine::Core::Object* joint)
{
    auto* bone = joint ? joint->GetComponent<
        Engine::Components::AnimationBone>() : nullptr;
    if (!bone || !ApplyControllerFacing(*joint, *bone)) return false;
    joint->transform.NotifyEditorTransformChanged(
        Engine::Components::Transform::EditorRotation);
    return true;
}

void EditorGizmoSystem::SetModelPin(
    Engine::Core::Object* joint, bool rotation, bool enabled)
{
    if (!joint) return;
    auto found = std::find_if(m_modelPins.begin(), m_modelPins.end(),
        [joint](const ModelPin& pin) { return pin.joint == joint; });
    if (found != m_modelPins.end())
        if (const auto* bone = joint->GetComponent<
                Engine::Components::AnimationBone>())
        {
            found->position = bone->posePinPosition;
            found->rotation = bone->posePinRotation;
            found->worldPosition = bone->posePinTarget;
            found->worldRotation = glm::quat(bone->posePinRotationEuler);
            found->pole = bone->posePinPole;
        }
    if (found == m_modelPins.end())
    {
        const auto* bone = joint->GetComponent<
            Engine::Components::AnimationBone>();
        if (!enabled && (!bone || (!bone->posePinPosition &&
                !bone->posePinRotation))) return;
        ModelPin pin;
        pin.joint = joint;
        pin.position = bone && bone->posePinPosition;
        pin.rotation = bone && bone->posePinRotation;
        pin.worldPosition = pin.position ? bone->posePinTarget :
            WorldPosition(joint);
        pin.worldRotation = pin.rotation ?
            glm::quat(bone->posePinRotationEuler) :
            glm::quat_cast(WorldRotation(*joint));
        pin.pole = pin.position ? bone->posePinPole : glm::vec3(0.f);
        if (!pin.position && joint->Parent && joint->Parent->Parent)
            pin.pole = WorldPosition(joint->Parent) * 2.f -
                (WorldPosition(joint->Parent->Parent) + pin.worldPosition) * .5f;
        m_modelPins.push_back(pin);
        found = std::prev(m_modelPins.end());
    }
    if (rotation)
    {
        if (enabled && !found->rotation)
            found->worldRotation = glm::quat_cast(WorldRotation(*joint));
        found->rotation = enabled;
    }
    else
    {
        const bool validChain = joint->Parent && joint->Parent->Parent &&
            joint->Parent->GetComponent<Engine::Components::AnimationBone>() &&
            joint->Parent->Parent->GetComponent<Engine::Components::AnimationBone>();
        if (enabled && !found->position && validChain)
        {
            found->worldPosition = WorldPosition(joint);
            found->pole = WorldPosition(joint->Parent) * 2.f -
                (WorldPosition(joint->Parent->Parent) +
                    found->worldPosition) * .5f;
        }
        found->position = enabled && validChain;
    }
    if (auto* bone = joint->GetComponent<
            Engine::Components::AnimationBone>())
    {
        bone->posePinPosition = found->position;
        bone->posePinRotation = found->rotation;
        bone->posePinTarget = found->worldPosition;
        bone->posePinRotationEuler = glm::eulerAngles(
            found->worldRotation);
        bone->posePinPole = found->pole;
        bone->MarkConfigurationDirty();
    }
    if (!found->position && !found->rotation)
        m_modelPins.erase(found);
}

void EditorGizmoSystem::ClearModelPins(Engine::Scene::Scene& scene)
{
    for (const auto& owner : scene.GetObjects())
        if (auto* bone = owner->GetComponent<
                Engine::Components::AnimationBone>();
            bone && (bone->posePinPosition || bone->posePinRotation))
        {
            bone->posePinPosition = false;
            bone->posePinRotation = false;
            bone->MarkConfigurationDirty();
        }
    m_modelPins.clear();
}

void EditorGizmoSystem::DrawSkeletonOverlay(
    Engine::Scene::Scene& scene,
    const Engine::Components::Skeleton& skeleton, IEditorUi& ui,
    const EditorUiViewportInput& input, int selectedPaletteIndex)
{
    const auto& displaySkeleton = DisplaySkeleton(skeleton);
    if (!scene.AreEditorBonesVisible() ||
        (!displaySkeleton.showBones &&
            (!skeleton.Owner || !(skeleton.Owner->editorOverlays &
                Engine::Core::Object::BoneOverlay)) &&
            (!displaySkeleton.Owner || !(displaySkeleton.Owner->editorOverlays &
                Engine::Core::Object::BoneOverlay))))
        return;
    const auto* camera = scene.editorCamera.GetComponent<
        Engine::Components::Camera>();
    if (!camera || input.available.x <= 1.f || input.available.y <= 1.f)
        return;
    const auto& joints = skeleton.ResolveJoints();
    if (joints.empty()) return;
    const glm::mat4 viewProjection = camera->GetProjectionMatrix(
        input.available.x / input.available.y) * camera->GetViewMatrix();
    const std::unordered_set<Engine::Core::Object*> jointSet(
        joints.begin(), joints.end());

    // Draw the selected segment last so its normal opacity stays legible
    // where several projected bones overlap.
    for (int pass = 0; pass < 2; ++pass)
        for (size_t index = 0; index < joints.size(); ++index)
        {
            auto* joint = joints[index];
            if (!joint) continue;
            const bool selected = static_cast<int>(index) ==
                selectedPaletteIndex;
            if (selected != (pass == 1)) continue;
            const float opacity = selected ? 1.f : .22f;
            EditorUiVec2 tip{};
            if (!ProjectPoint(viewProjection, WorldPosition(joint),
                    input.available, tip)) continue;
            auto* parent = joint->Parent;
            while (parent && jointSet.find(parent) == jointSet.end())
                parent = parent->Parent;
            if (parent)
            {
                EditorUiVec2 root{};
                if (!ProjectPoint(viewProjection, WorldPosition(parent),
                        input.available, root)) continue;
                DrawBoneShape(ui, root, tip, selected, opacity);
            }
            else
            {
                EditorUiColor outline = kOutline;
                EditorUiColor fill = selected ? kHoverColor :
                    EditorUiColor{ .45f, .86f, 1.f, 1.f };
                outline.a *= opacity;
                fill.a *= opacity;
                ui.DrawViewportCircle(tip, selected ? 7.f : 5.f,
                    outline, true);
                ui.DrawViewportCircle(tip, selected ? 5.f : 3.5f,
                    fill, true);
            }
        }
}

EditorGizmoResult EditorGizmoSystem::DrawAndHandle(
    Engine::Scene::Scene& scene, IEditorUi& ui,
    const EditorUiViewportInput& input, EditorTransformTool tool)
{
    EditorGizmoResult result{};
    const Engine::Components::Camera* camera = scene.editorCamera.GetComponent<Engine::Components::Camera>();
    if (!camera || input.available.x <= 1.f || input.available.y <= 1.f)
        return result;

    if (m_dragObject && (!SceneContains(scene, m_dragObject) || !input.leftDown))
    {
        m_dragObject = nullptr;
        m_dragAxis = -1;
    }

    const glm::mat4 viewProjection =
        camera->GetProjectionMatrix(input.available.x / input.available.y) *
        camera->GetViewMatrix();
    const glm::mat4 inverseViewProjection = glm::inverse(viewProjection);
    m_modelDragEnded = false;
    m_modelPins.erase(std::remove_if(m_modelPins.begin(), m_modelPins.end(),
        [&](const ModelPin& pin) {
            if (!SceneContains(scene, pin.joint)) return true;
            const auto* bone = pin.joint->GetComponent<
                Engine::Components::AnimationBone>();
            return bone && !bone->posePinPosition &&
                !bone->posePinRotation;
        }),
        m_modelPins.end());
    for (ModelPin& pin : m_modelPins)
        if (const auto* bone = pin.joint->GetComponent<
                Engine::Components::AnimationBone>())
        {
            pin.position = bone->posePinPosition;
            pin.rotation = bone->posePinRotation;
            pin.worldPosition = bone->posePinTarget;
            pin.worldRotation = glm::quat(bone->posePinRotationEuler);
            pin.pole = bone->posePinPole;
        }
    for (const auto& owner : scene.GetObjects())
    {
        auto* joint = owner.get();
        const auto* bone = joint ? joint->GetComponent<
            Engine::Components::AnimationBone>() : nullptr;
        if (!bone || (!bone->posePinPosition &&
                !bone->posePinRotation)) continue;
        if (std::find_if(m_modelPins.begin(), m_modelPins.end(),
                [joint](const ModelPin& pin) {
                    return pin.joint == joint;
                }) != m_modelPins.end()) continue;
        ModelPin pin;
        pin.joint = joint;
        pin.position = bone->posePinPosition;
        pin.rotation = bone->posePinRotation;
        pin.worldPosition = bone->posePinTarget;
        pin.worldRotation = glm::quat(bone->posePinRotationEuler);
        pin.pole = bone->posePinPole;
        m_modelPins.push_back(pin);
    }
    m_modelSelectedPoints.erase(std::remove_if(m_modelSelectedPoints.begin(),
        m_modelSelectedPoints.end(), [&](Engine::Core::Object* joint) {
            return !SceneContains(scene, joint);
        }), m_modelSelectedPoints.end());
    for (const ModelPin& pin : m_modelPins)
    {
        EditorUiVec2 point{};
        if (!scene.AreEditorBonesVisible()) continue;
        const glm::vec3 marker = pin.position ? pin.worldPosition
            : WorldPosition(pin.joint);
        if (!ProjectPoint(viewProjection, marker,
                input.available, point)) continue;
        ui.DrawViewportCircle(point, 7.f,
            pin.position ? EditorUiColor{1.f, .36f, .24f, .95f}
                : EditorUiColor{.65f, .42f, 1.f, .95f}, false, 2.f);
        ui.DrawViewportText(Add(point, {9.f, -8.f}),
            pin.position && pin.rotation ? "P/R" :
                pin.position ? "P" : "R",
            {1.f, 1.f, 1.f, .95f});
    }
    if (m_modelDragObject && (!SceneContains(scene, m_modelDragObject) ||
            !input.leftDown || !m_modelManipulation))
    {
        m_modelDragEnded = !input.leftDown;
        m_modelDragObject = nullptr;
        m_modelPosePreferences.clear();
    }
    if (m_modelDragObject)
    {
        auto* joint = m_modelDragObject;
        const EditorUiVec2 mouseMotion = Subtract(
            input.mousePosInViewport, m_modelStartMouse);
        if (!m_modelDidMove && Length(mouseMotion) < .5f &&
            input.mouseWheel == 0.f)
        {
            result.transformDragging = true;
            result.consumedClick = true;
            return result;
        }
        m_modelDidMove = true;
        const auto markChanged = [&](Engine::Core::Object* bone) {
            if (bone && std::find(m_modelChangedBones.begin(),
                    m_modelChangedBones.end(), bone) == m_modelChangedBones.end())
                m_modelChangedBones.push_back(bone);
        };
        const glm::mat4 cameraWorld = scene.editorCamera.transform.GetWorldMatrix();
        const glm::vec3 forward = -glm::normalize(glm::vec3(cameraWorld[2]));
        const glm::vec3 oldPosition = joint->transform.position;
        const glm::vec3 oldRotation = joint->transform.rotation;
        if (m_modelScrollDepthEnabled && !m_modelDragRotate &&
            input.mouseWheel != 0.f)
        {
            const float cameraDistance = glm::length(m_modelStartWorld -
                glm::vec3(cameraWorld[3]));
            m_modelScrollDepth -= input.mouseWheel *
                std::max(cameraDistance * .08f, .02f);
        }
        bool fullBodyHandled = false;
        if (m_modelDragTwist)
        {
            glm::vec3 hit{};
            if (ViewPlanePoint(inverseViewProjection,
                    input.mousePosInViewport, input.available,
                    m_modelStartHandleWorld, forward, hit))
            {
                const glm::vec3 initial = m_modelStartHandleWorld -
                    m_modelTwistBaseWorld;
                glm::vec3 desired = m_modelStartHandleWorld +
                    hit - m_modelPlaneHit - m_modelTwistBaseWorld;
                desired -= m_modelTwistAxis *
                    glm::dot(desired, m_modelTwistAxis);
                if (glm::dot(initial, initial) > 1e-8f &&
                    glm::dot(desired, desired) > 1e-8f)
                {
                    const float angle = std::atan2(glm::dot(m_modelTwistAxis,
                        glm::cross(initial, desired)),
                        glm::dot(initial, desired));
                    const glm::quat parent = joint->Parent
                        ? glm::quat_cast(WorldRotation(*joint->Parent))
                        : glm::quat(1.f, 0.f, 0.f, 0.f);
                    const glm::quat delta = glm::angleAxis(angle,
                        m_modelTwistAxis);
                    joint->transform.rotation = glm::eulerAngles(
                        glm::normalize(glm::inverse(parent) * delta * parent *
                            glm::quat(m_modelStartRotation)));
                }
            }
        }
        else if (m_modelDragDirection)
        {
            glm::vec3 hit{};
            if (ViewPlanePoint(inverseViewProjection,
                    input.mousePosInViewport, input.available,
                    m_modelStartHandleWorld, forward, hit))
            {
                const glm::vec3 initial = m_modelStartHandleWorld -
                    m_modelStartWorld;
                const glm::vec3 desired = m_modelStartHandleWorld +
                    hit - m_modelPlaneHit - m_modelStartWorld;
                if (glm::dot(initial, initial) > 1e-8f &&
                    glm::dot(desired, desired) > 1e-8f)
                {
                    const glm::quat parent = joint->Parent
                        ? glm::quat_cast(WorldRotation(*joint->Parent))
                        : glm::quat(1.f, 0.f, 0.f, 0.f);
                    const glm::quat delta = glm::quat(
                        glm::normalize(initial), glm::normalize(desired));
                    joint->transform.rotation = glm::eulerAngles(
                        glm::normalize(glm::inverse(parent) * delta * parent *
                            glm::quat(m_modelStartRotation)));
                }
            }
        }
        else if (m_modelDragRotate)
        {
            const float angle = (input.mousePosInViewport.x - m_modelStartMouse.x) * .01f;
            const glm::quat parent = joint->Parent
                ? glm::quat_cast(WorldRotation(*joint->Parent))
                : glm::quat(1.f, 0.f, 0.f, 0.f);
            const glm::quat delta = glm::angleAxis(angle, forward);
            joint->transform.rotation = glm::eulerAngles(glm::normalize(
                glm::inverse(parent) * delta * parent *
                glm::quat(m_modelStartRotation)));
        }
        else
        {
            glm::vec3 target = m_modelStartWorld;
            if (input.modelDepthDown)
            {
                const float depth = glm::length(m_modelStartWorld -
                    glm::vec3(cameraWorld[3]));
                target += forward * ((m_modelStartMouse.y -
                    input.mousePosInViewport.y) * depth *
                    2.f / input.available.y);
            }
            else
            {
                glm::vec3 hit{};
                if (ViewPlanePoint(inverseViewProjection,
                        input.mousePosInViewport, input.available,
                        m_modelStartWorld, forward, hit))
                    target += hit - m_modelPlaneHit;
            }
            target += forward * m_modelScrollDepth;
            if (auto* draggedBone = joint->GetComponent<
                    Engine::Components::AnimationBone>();
                draggedBone && draggedBone->poseControllerActive)
            {
                draggedBone->poseControllerTarget = target;
                draggedBone->MarkConfigurationDirty();
            }
            bool groupHandled = false;
            if (!m_modelDragRotate && m_modelDragGroup.size() > 1)
            {
                const glm::vec3 delta = target - m_modelStartWorld;
                for (const auto& [member, startWorld] : m_modelDragGroup)
                {
                    if (!SceneContains(scene, member) ||
                        IsModelPinned(member, false)) continue;
                    if (auto* memberBone = member->GetComponent<
                            Engine::Components::AnimationBone>();
                        memberBone && memberBone->poseControllerActive)
                    {
                        memberBone->poseControllerTarget = startWorld + delta;
                        memberBone->MarkConfigurationDirty();
                    }
                    bool selectedAncestor = false;
                    for (auto* ancestor = member->Parent; ancestor;
                        ancestor = ancestor->Parent)
                        if (std::find_if(m_modelDragGroup.begin(),
                                m_modelDragGroup.end(), [&](const auto& entry) {
                                    return entry.first == ancestor &&
                                        !IsModelPinned(ancestor, false);
                                }) != m_modelDragGroup.end())
                        { selectedAncestor = true; break; }
                    if (selectedAncestor) continue;
                    const glm::mat4 parentWorld = member->Parent
                        ? member->Parent->transform.GetWorldMatrix()
                        : glm::mat4(1.f);
                    if (std::abs(glm::determinant(parentWorld)) <= 1e-8f)
                        continue;
                    const glm::vec3 local = glm::vec3(glm::inverse(
                        parentWorld) * glm::vec4(startWorld + delta, 1.f));
                    if (member->transform.position != local)
                    {
                        member->transform.position = local;
                        if (auto* point = member != m_modelRoot
                                ? member->GetComponent<
                                    Engine::Components::AnimationBone>()
                                : nullptr)
                            point->RestorePoseLength();
                        member->transform.NotifyEditorTransformChanged(
                            Engine::Components::Transform::EditorPosition);
                        markChanged(member);
                    }
                }
                groupHandled = true;
            }
            if (!groupHandled && m_modelEffectiveScope == 2 && m_modelRoot)
            {
                const bool draggingRoot = m_modelRoot == joint;
                if (draggingRoot)
                {
                    const glm::mat4 parentWorld = joint->Parent
                        ? joint->Parent->transform.GetWorldMatrix()
                        : glm::mat4(1.f);
                    if (std::abs(glm::determinant(parentWorld)) > 1e-8f)
                    {
                        joint->transform.position = glm::vec3(
                            glm::inverse(parentWorld) * glm::vec4(target, 1.f));
                        if (m_modelSupportRadius > 0.f)
                            Engine::Components::ChainIK::ConstrainRootBalance(
                                *joint, {m_modelSupportCenter,
                                    m_modelSupportRadius});
                        joint->transform.NotifyEditorTransformChanged(
                            Engine::Components::Transform::EditorPosition);
                        markChanged(joint);
                    }
                }
                const auto chainTo = [&](Engine::Core::Object* tip) {
                    std::vector<Engine::Core::Object*> chain;
                    for (auto* current = tip; current;
                        current = current->Parent)
                    {
                        chain.push_back(current);
                        if (current == m_modelRoot) break;
                    }
                    if (chain.empty() || chain.back() != m_modelRoot)
                        chain.clear();
                    else std::reverse(chain.begin(), chain.end());
                    return chain;
                };
                std::vector<Engine::Components::ChainIK::Effector> effectors;
                if (!draggingRoot)
                {
                    Engine::Components::ChainIK::Effector selectedEffector;
                    selectedEffector.chain = chainTo(joint);
                    selectedEffector.target = target;
                    selectedEffector.positionWeight = 1.f;
                    if (selectedEffector.chain.size() >= 2)
                        effectors.push_back(std::move(selectedEffector));
                }
                for (const ModelPin& pin : m_modelPins)
                {
                    if ((!pin.position && !pin.rotation) ||
                        pin.joint == joint) continue;
                    Engine::Components::ChainIK::Effector pinnedEffector;
                    pinnedEffector.chain = chainTo(pin.joint);
                    if (pinnedEffector.chain.size() < 2) continue;
                    pinnedEffector.target = pin.position
                        ? pin.worldPosition : WorldPosition(pin.joint);
                    pinnedEffector.positionWeight = pin.position ? 1.f : 0.f;
                    pinnedEffector.rotationWeight = pin.rotation ? 1.f : 0.f;
                    pinnedEffector.rotation = pin.worldRotation;
                    pinnedEffector.pole = pin.pole;
                    pinnedEffector.usePole = pin.position;
                    pinnedEffector.fixed = true;
                    effectors.push_back(std::move(pinnedEffector));
                }
                for (const auto& owner : scene.GetObjects())
                {
                    auto* active = owner.get();
                    if (!active || active == joint ||
                        IsModelPinned(active, false)) continue;
                    const auto* bone = active->GetComponent<
                        Engine::Components::AnimationBone>();
                    if (!bone || !bone->poseControllerActive) continue;
                    Engine::Components::ChainIK::Effector activeEffector;
                    activeEffector.chain = chainTo(active);
                    if (activeEffector.chain.size() < 2) continue;
                    activeEffector.target = bone->poseControllerTarget;
                    activeEffector.positionWeight = .4f;
                    glm::quat facing;
                    if (ControllerFacingRotation(*active, *bone, facing))
                    {
                        activeEffector.rotation = facing;
                        activeEffector.rotationWeight =
                            std::clamp(bone->poseFacingWeight, 0.f, 1.f);
                    }
                    effectors.push_back(std::move(activeEffector));
                }
                std::vector<Engine::Components::ChainIK::PosePreference>
                    preferences;
                preferences.reserve(m_modelPosePreferences.size());
                for (const auto& preference : m_modelPosePreferences)
                    preferences.push_back({preference.joint,
                        preference.localRotation, preference.stiffness});
                const Engine::Components::ChainIK::BalanceConstraint balance{
                    m_modelSupportCenter, m_modelSupportRadius};
                const bool solved = !effectors.empty() &&
                    Engine::Components::ChainIK::SolveMultiple(effectors,
                        24, .001f, draggingRoot ? 0.f : .35f,
                        45.f, &preferences,
                        m_modelSupportRadius > 0.f ? &balance : nullptr);
                fullBodyHandled = draggingRoot || solved;
                if (fullBodyHandled)
                {
                    // Store the animation editor's limb target after the
                    // shared solve has moved the root and pinned effectors.
                    if (!draggingRoot && m_poseEditing &&
                        m_poseTranslationHandler)
                        m_poseTranslationHandler(joint, target);
                    for (const auto& effector : effectors)
                        for (auto* bone : effector.chain)
                        {
                            bone->transform.NotifyEditorTransformChanged(
                                static_cast<uint8_t>(
                                    Engine::Components::Transform::EditorRotation |
                                    (bone == m_modelRoot
                                        ? Engine::Components::Transform::EditorPosition : 0)));
                            markChanged(bone);
                        }
                }
            }
            bool bodyPartHandled = false;
            if (!groupHandled && !fullBodyHandled &&
                m_modelEffectiveScope == 1 &&
                (m_modelControllerMode == 1 ||
                    m_modelChainDepth != 2 ||
                    !m_poseEditing || !m_poseTranslationHandler))
            {
                Engine::Components::ChainIK::Effector effector;
                for (auto* current = joint; current &&
                    effector.chain.size() <=
                        static_cast<size_t>(m_modelChainDepth);
                    current = current->Parent)
                {
                    effector.chain.insert(effector.chain.begin(), current);
                    if (current == m_modelRoot) break;
                }
                if (effector.chain.size() >= 2)
                {
                    effector.target = target;
                    std::vector<Engine::Components::ChainIK::PosePreference>
                        preferences;
                    for (const auto& preference : m_modelPosePreferences)
                        preferences.push_back({preference.joint,
                            preference.localRotation, preference.stiffness});
                    bodyPartHandled = Engine::Components::ChainIK::
                        SolveMultiple({effector}, 24, .001f, 0.f, 45.f,
                            &preferences);
                    if (bodyPartHandled)
                        for (auto* member : effector.chain)
                        {
                            member->transform.NotifyEditorTransformChanged(
                                Engine::Components::Transform::EditorRotation);
                            markChanged(member);
                        }
                }
            }
            const bool poseHandled = !groupHandled && !fullBodyHandled &&
                !bodyPartHandled && m_modelEffectiveScope > 0 &&
                m_poseEditing &&
                m_poseTranslationHandler &&
                m_poseTranslationHandler(joint, target);
            const bool ikHandled = !groupHandled && !fullBodyHandled &&
                !bodyPartHandled && !poseHandled &&
                !m_poseEditing &&
                m_modelEffectiveScope > 0 &&
                m_modelDragIK && joint->Parent && joint->Parent->Parent &&
                Engine::Components::TwoBoneIK::Blend(
                    *joint->Parent->Parent, *joint->Parent, *joint,
                    target, m_modelPole, m_modelTipRotation, 1.f);
            if (ikHandled)
                for (auto* bone : {joint->Parent->Parent, joint->Parent, joint})
                {
                    bone->transform.NotifyEditorTransformChanged(
                        Engine::Components::Transform::EditorRotation);
                    markChanged(bone);
                }
            if (poseHandled && joint->Parent && joint->Parent->Parent)
                for (auto* bone : {joint->Parent->Parent, joint->Parent, joint})
                    markChanged(bone);
            if (!groupHandled && !fullBodyHandled && !bodyPartHandled &&
                !poseHandled && !ikHandled)
            {
                const glm::mat4 parentWorld = joint->Parent
                    ? joint->Parent->transform.GetWorldMatrix()
                    : glm::mat4(1.f);
                if (std::abs(glm::determinant(parentWorld)) > 1e-8f)
                    joint->transform.position = glm::vec3(
                        glm::inverse(parentWorld) * glm::vec4(target, 1.f));
            }
            if (auto* controller = joint->GetComponent<
                    Engine::Components::AnimationBone>();
                controller && ApplyControllerFacing(*joint, *controller))
            {
                joint->transform.NotifyEditorTransformChanged(
                    Engine::Components::Transform::EditorRotation);
                markChanged(joint);
            }
            if (auto* controller = joint->GetComponent<
                    Engine::Components::AnimationBone>();
                controller && joint != m_modelRoot &&
                controller->RestorePoseLength())
            {
                joint->transform.NotifyEditorTransformChanged(
                    Engine::Components::Transform::EditorPosition);
                markChanged(joint);
            }
        }
        for (const ModelPin& pin : m_modelPins)
        {
            auto* pinned = pin.joint;
            if (!pinned || pinned == joint) continue;
            if (!fullBodyHandled && pin.position && pinned->Parent &&
                pinned->Parent->Parent &&
                Engine::Components::TwoBoneIK::Solve(
                    *pinned->Parent->Parent, *pinned->Parent, *pinned,
                    pin.worldPosition, pin.pole, 1.f, 0.f))
            {
                for (auto* bone : {pinned->Parent->Parent,
                        pinned->Parent, pinned})
                {
                    bone->transform.NotifyEditorTransformChanged(
                        Engine::Components::Transform::EditorRotation);
                    markChanged(bone);
                }
            }
            if (pin.rotation)
            {
                const glm::quat parent = pinned->Parent
                    ? glm::quat_cast(WorldRotation(*pinned->Parent))
                    : glm::quat(1.f, 0.f, 0.f, 0.f);
                pinned->transform.rotation = glm::eulerAngles(glm::normalize(
                    glm::inverse(parent) * pin.worldRotation));
                pinned->transform.NotifyEditorTransformChanged(
                    Engine::Components::Transform::EditorRotation);
                markChanged(pinned);
            }
        }
        if (joint->transform.position != oldPosition ||
            joint->transform.rotation != oldRotation)
        {
            joint->transform.NotifyEditorTransformChanged(m_modelDragRotate
                ? Engine::Components::Transform::EditorRotation
                : Engine::Components::Transform::EditorPosition);
            markChanged(joint);
        }
        result.transformDragging = true;
        result.consumedClick = true;
        return result;
    }
    // Pinned hierarchy overlays are visual aids and do not take input.
    for (const auto& objectPointer : scene.GetObjects())
    {
        Engine::Core::Object* object = objectPointer.get();
        if (!object || !object->IsEnabledInHierarchy()) continue;
        const uint8_t overlays = object->editorOverlays;
        if (!(overlays & (Engine::Core::Object::PositionOverlay |
            Engine::Core::Object::RotationOverlay |
            Engine::Core::Object::ScaleOverlay))) continue;
        const glm::vec3 center = WorldPosition(object);
        EditorUiVec2 origin{};
        if (!ProjectPoint(viewProjection, center, input.available, origin)) continue;
        const glm::vec3 cameraPosition =
            glm::vec3(scene.editorCamera.transform.GetWorldMatrix()[3]);
        const float axisScale = std::clamp(glm::length(center-cameraPosition)*.18f,
            .35f,8.f);
        const glm::mat3 rotation = WorldRotation(*object);
        const glm::vec3 axes[3] = { rotation[0],rotation[1],rotation[2] };
        const EditorUiColor colors[3] = { kXColor,kYColor,kZColor };
        for (int axis=0;axis<3;++axis)
        {
            EditorUiVec2 end{};
            if ((overlays & Engine::Core::Object::PositionOverlay) &&
                ProjectPoint(viewProjection,center+axes[axis]*axisScale,
                    input.available,end))
            {
                ui.DrawViewportLine(origin,end,colors[axis],2.f);
                const EditorUiVec2 direction=Subtract(end,origin);
                const float length=Length(direction);
                if (length>7.f)
                {
                    const EditorUiVec2 unit=Multiply(direction,1.f/length);
                    const EditorUiVec2 side{-unit.y,unit.x};
                    ui.DrawViewportTriangle(end,
                        Add(Subtract(end,Multiply(unit,10.f)),Multiply(side,4.f)),
                        Subtract(Subtract(end,Multiply(unit,10.f)),Multiply(side,4.f)),
                        colors[axis]);
                }
            }
            if ((overlays & Engine::Core::Object::ScaleOverlay) &&
                ProjectPoint(viewProjection,center+axes[axis]*axisScale*.8f,
                    input.available,end))
            {
                ui.DrawViewportLine(origin,end,colors[axis],2.f);
                DrawBoxHandle(ui,end,5.f,colors[axis]);
            }
            if (overlays & Engine::Core::Object::RotationOverlay)
            {
                EditorUiVec2 previous{};
                bool previousVisible=false;
                for (int segment=0;segment<=48;++segment)
                {
                    const float angle=6.28318530718f*segment/48.f;
                    EditorUiVec2 current{};
                    const bool visible=ProjectPoint(viewProjection,
                        center+(axes[(axis+1)%3]*std::cos(angle)+
                            axes[(axis+2)%3]*std::sin(angle))*axisScale*.78f,
                        input.available,current);
                    if (previousVisible && visible)
                        ui.DrawViewportLine(previous,current,colors[axis],1.5f);
                    previous=current;
                    previousVisible=visible;
                }
            }
        }
    }
    for (const auto& objectPointer : scene.GetObjects())
    {
        Engine::Core::Object* object=objectPointer.get();
        if (!object || !object->IsEnabledInHierarchy()) continue;
        const uint8_t overlays=object->editorOverlays;
        if (object == scene.GetSelectedObject())
            if (const auto* emitter=object->GetComponent<Engine::Components::ParticleEmitter>())
            {
                const glm::mat4 world=object->transform.GetWorldMatrix();
                const EditorUiColor shapeColor{1.f,.72f,.22f,.85f};
                if (emitter->shape == static_cast<int>(
                        Engine::Components::ParticleEmitter::Shape::Box))
                {
                    const glm::vec3 extents=glm::abs(emitter->boxExtents);
                    DrawWorldBox(ui,viewProjection,input.available,world,
                        -extents,extents,shapeColor);
                }
                else if (emitter->shape == static_cast<int>(
                        Engine::Components::ParticleEmitter::Shape::Sphere))
                {
                    const float radius=std::max(0.f,emitter->sphereRadius);
                    for (int axis=0;axis<3;++axis)
                        DrawColliderRing(ui,viewProjection,input.available,
                            world,radius,0.f,axis,shapeColor);
                }
                else
                {
                    const float radius=std::clamp(emitter->size,.1f,.5f);
                    for (int axis=0;axis<3;++axis)
                    {
                        glm::vec3 end(0.f);
                        end[axis]=radius;
                        DrawWorldLine(ui,viewProjection,input.available,
                            glm::vec3(world*glm::vec4(-end,1.f)),
                            glm::vec3(world*glm::vec4(end,1.f)),shapeColor);
                    }
                }
            }
        if (EditorViewportSettings::AreCollidersVisible())
            for (const auto* component : object->Components)
                if (const auto* collider = dynamic_cast<const Engine::Components::Collider*>(component))
                    DrawColliderOverlay(ui, viewProjection, input.available,
                        *object, *collider);
        if (EditorViewportSettings::AreMeshNormalsVisible())
            if (const auto* mesh = object->GetComponent<Engine::Components::Mesh>();
                mesh && mesh->IsEditorVisible())
                DrawMeshNormals(ui, viewProjection, input.available, *object, *mesh);
        if ((overlays & Engine::Core::Object::BoundsOverlay) ||
            EditorViewportSettings::AreBoundsVisible())
        {
            const glm::mat4 world=object->transform.GetWorldMatrix();
            const EditorUiColor color{.58f,.78f,1.f,.85f};
            if (const auto* mesh=object->GetComponent<Engine::Components::Mesh>();
                mesh && mesh->HasBounds())
                DrawWorldBox(ui,viewProjection,input.available,world,
                    mesh->GetBoundsMin(),mesh->GetBoundsMax(),color);
            if (const auto* sprite=object->GetComponent<Engine::Components::Sprite>())
            {
                const glm::vec2 half=sprite->GetWorldSize()*.5f;
                DrawWorldBox(ui,viewProjection,input.available,world,
                    {-half.x,-half.y,0.f},{half.x,half.y,0.f},color);
            }
            if (const auto* emitter=object->GetComponent<Engine::Components::ParticleEmitter>())
            {
                const auto bounds=emitter->GetWorldBounds(world);
                if (bounds.valid)
                    DrawWorldBox(ui,viewProjection,input.available,glm::mat4(1.f),
                        bounds.minimum,bounds.maximum,color);
            }
            if (const auto* probes=object->GetComponent<
                    Engine::Components::LightProbeGroup>())
                DrawWorldBox(ui,viewProjection,input.available,world,
                    -glm::abs(probes->size)*.5f,
                    glm::abs(probes->size)*.5f,color);
        }
        if ((overlays & Engine::Core::Object::LightVolumeOverlay) ||
            EditorViewportSettings::AreLightVolumesVisible())
            if (const auto* light=object->GetComponent<Engine::Components::Light>())
                DrawLightVolume(ui,viewProjection,input.available,*object,*light);
        if ((overlays & Engine::Core::Object::CameraFrustumOverlay) ||
            EditorViewportSettings::AreCameraFrustumsVisible())
            if (const auto* sceneCamera=object->GetComponent<Engine::Components::Camera>())
                DrawCameraFrustum(ui,viewProjection,input.available,*sceneCamera);
    }
    Engine::Core::Object* selected = scene.GetSelectedObject();
    const auto* selectedGroup = Engine::Components::BoneGroup::ForSelection(selected);
    const Engine::Components::Skeleton* selectedSkeleton = nullptr;
    if (m_boneEditing && selected)
        for (const auto& owner : scene.GetObjects())
        {
            if (!owner || selectedSkeleton) continue;
            for (auto* component : owner->Components)
                if (auto* skeleton = dynamic_cast<Engine::Components::Skeleton*>(component))
                {
                    const auto& joints = skeleton->ResolveJoints();
                    if (std::find(joints.begin(), joints.end(), selected) != joints.end())
                    { selectedSkeleton = skeleton; break; }
                }
        }
    const bool editingBone = selectedSkeleton != nullptr;
    glm::vec3 transformPivot = WorldPosition(selected);
    if (editingBone && m_bonePivotMode == 1 && selected->Parent)
        transformPivot = WorldPosition(selected->Parent);
    else if (editingBone && m_bonePivotMode == 2 && selectedSkeleton->GetHierarchyRoot())
        transformPivot = WorldPosition(selectedSkeleton->GetHierarchyRoot());
    else if (editingBone && m_bonePivotMode == 3)
        transformPivot = m_boneCustomPivot;
    Engine::Core::Object* selectedPrefabRoot = selected
        ? selected->GetPrefabInstanceRoot() : nullptr;

    Engine::Core::Object* boneHit = nullptr;
    Engine::Core::Object* jointHit = nullptr;
    float jointHitDistance = FLT_MAX;
    float boneHitDistance = FLT_MAX;
    std::set<std::pair<Engine::Core::Object*, Engine::Core::Object*>> drawnBones;
    std::unordered_set<Engine::Core::Object*> drawnRoots;
    std::unordered_set<Engine::Core::Object*> visibleSkeletonJoints;
    struct ProjectedJoint
    {
        EditorUiVec2 screen{};
        bool visible = false;
    };
    std::unordered_map<Engine::Core::Object*, ProjectedJoint> projectedJoints;
    const auto projectJoint = [&](Engine::Core::Object* joint)
        -> const ProjectedJoint&
    {
        auto [entry, inserted] = projectedJoints.try_emplace(joint);
        if (inserted)
            entry->second.visible = ProjectPoint(viewProjection,
                WorldPosition(joint), input.available, entry->second.screen);
        return entry->second;
    };
    for (const auto& ownerPointer : scene.GetObjects())
    {
        Engine::Core::Object* owner = ownerPointer.get();
        if (!owner || !owner->IsEnabledInHierarchy()) continue;
        for (Engine::Core::Component* component : owner->Components)
        {
            auto* skeleton = dynamic_cast<Engine::Components::Skeleton*>(component);
            if (!scene.AreEditorBonesVisible() || !skeleton) continue;
            const auto& displaySkeleton = DisplaySkeleton(*skeleton);
            const uint8_t displayOverlays = displaySkeleton.Owner
                ? displaySkeleton.Owner->editorOverlays : 0;
            const bool drawBones = displaySkeleton.showBones ||
                (owner->editorOverlays & Engine::Core::Object::BoneOverlay) ||
                (displayOverlays & Engine::Core::Object::BoneOverlay);
            const bool drawJoints = displaySkeleton.showBones ||
                (owner->editorOverlays & Engine::Core::Object::SkeletonOverlay) ||
                (displayOverlays & Engine::Core::Object::SkeletonOverlay);
            if (!drawBones && !drawJoints) continue;
            const auto* rootSkeleton = owner->GetComponent<Engine::Components::Skeleton>();
            if (rootSkeleton != skeleton && rootSkeleton &&
                !rootSkeleton->rootBoneNodes.empty()) continue;
            std::unordered_set<Engine::Core::Object*> joints;
            const auto& displayNodes = skeleton->rootBoneNodes.empty()
                ? skeleton->jointNodes : skeleton->rootBoneNodes;
            for (unsigned node : displayNodes)
            {
                Engine::Core::Object* joint = skeleton->FindNode(node);
                if (joint)
                {
                    if (selectedGroup && !selectedGroup->Contains(joint)) continue;
                    joints.insert(joint);
                    visibleSkeletonJoints.insert(joint);
                }
            }
            for (Engine::Core::Object* joint : joints)
            {
                Engine::Core::Object* parentJoint = joint->Parent;
                while (parentJoint && joints.find(parentJoint) == joints.end())
                    parentJoint = parentJoint->Parent;

                const auto& tip = projectJoint(joint);
                if (!tip.visible) continue;
                const EditorUiVec2 tipScreen = tip.screen;

                float hitDistance = Length(Subtract(
                    input.mousePosInViewport, tipScreen));
                if (drawBones &&
                    hitDistance <= 9.f && hitDistance < boneHitDistance)
                {
                    boneHit = joint;
                    boneHitDistance = hitDistance;
                }
                if (drawJoints &&
                    hitDistance <= 9.f && hitDistance < jointHitDistance)
                {
                    jointHit = joint;
                    jointHitDistance = hitDistance;
                }

                if (!parentJoint)
                {
                    if (drawJoints &&
                        drawnRoots.insert(joint).second)
                    {
                        ui.DrawViewportCircle(tipScreen, joint == selected ? 7.f : 5.f,
                            kOutline, true);
                        ui.DrawViewportCircle(tipScreen, joint == selected ? 5.f : 3.5f,
                            joint == selected ? kHoverColor :
                                EditorUiColor{ 0.45f, 0.86f, 1.f, 1.f }, true);
                    }
                    continue;
                }

                if (!drawnBones.insert({ parentJoint, joint }).second)
                    continue;
                const auto& root = projectJoint(parentJoint);
                if (!root.visible) continue;
                const EditorUiVec2 rootScreen = root.screen;
                if (drawBones)
                    DrawBoneShape(ui, rootScreen, tipScreen,
                        joint == selected || parentJoint == selected);
                if (drawJoints)
                {
                    ui.DrawViewportCircle(rootScreen, 4.f, kOutline, true);
                    ui.DrawViewportCircle(rootScreen, 2.5f,
                        parentJoint == selected ? kHoverColor :
                            EditorUiColor{ 0.55f, 0.9f, 1.f, 1.f }, true);
                }

                float parameter = 0.f;
                hitDistance = DistanceToSegment(input.mousePosInViewport,
                    rootScreen, tipScreen, parameter);
                if (drawBones &&
                    hitDistance <= 7.f && hitDistance < boneHitDistance)
                {
                    boneHit = m_poseEditing ? parentJoint : joint;
                    boneHitDistance = hitDistance;
                }
            }
        }
    }

    // Point selection uses the resolved skeleton rather than unrelated scene
    // objects, and follows the viewport's bone-gizmo visibility control.
    Engine::Core::Object* pointHit = nullptr;
    bool pointHitDirection = false;
    bool pointHitTwist = false;
    glm::vec3 pointHitWorld{};
    glm::vec3 pointHitTwistAxis{};
    glm::vec3 pointHitTwistBase{};
    float pointHitDistance = FLT_MAX;
    if (scene.AreEditorBonesVisible() && m_modelManipulation && m_modelSelectionMode == 3)
    {
        std::unordered_set<Engine::Core::Object*> drawnPoints;
        for (const auto& ownerPointer : scene.GetObjects())
        {
            auto* owner = ownerPointer.get();
            if (!owner || !owner->IsEnabledInHierarchy()) continue;
            for (auto* component : owner->Components)
            {
                auto* skeleton = dynamic_cast<Engine::Components::Skeleton*>(component);
                if (!skeleton) continue;
                std::vector<Engine::Core::Object*> pointJoints =
                    skeleton->ResolveJoints();
                for (const auto& candidate : scene.GetObjects())
                {
                    auto* point = candidate.get();
                    const auto* controller = point ? point->GetComponent<
                        Engine::Components::AnimationBone>() : nullptr;
                    if (!controller || !controller->poseControlOnly ||
                        controller->skinIndex != skeleton->skinIndex) continue;
                    for (auto* ancestor = point->Parent; ancestor;
                        ancestor = ancestor->Parent)
                        if (std::find(pointJoints.begin(), pointJoints.end(),
                                ancestor) != pointJoints.end())
                        { pointJoints.push_back(point); break; }
                }
                for (auto* joint : pointJoints)
                {
                    if (!joint || !drawnPoints.insert(joint).second ||
                        (selectedGroup && !selectedGroup->Contains(joint)) ||
                        (m_modelPickFilter && !m_modelPickFilter(joint)))
                        continue;
                    EditorUiVec2 screen{};
                    if (!ProjectPoint(viewProjection, WorldPosition(joint),
                            input.available, screen)) continue;
                    const auto* connection = joint->GetComponent<
                        Engine::Components::AnimationBone>();
                    if (joint->Parent && connection)
                    {
                        EditorUiVec2 parentScreen{};
                        if (ProjectPoint(viewProjection,
                                WorldPosition(joint->Parent), input.available,
                                parentScreen))
                            ui.DrawViewportLine(parentScreen, screen,
                                PoseConnectionColor(
                                    connection->PoseLengthRatio()), 2.f);
                    }
                    const float distance = Length(Subtract(
                        input.mousePosInViewport, screen));
                    if (distance <= 10.f && distance < pointHitDistance)
                    {
                        pointHit = joint;
                        pointHitDirection = false;
                        pointHitTwist = false;
                        pointHitWorld = WorldPosition(joint);
                        pointHitDistance = distance;
                    }
                    const bool selectedPoint = joint == selected ||
                        std::find(m_modelSelectedPoints.begin(),
                            m_modelSelectedPoints.end(), joint) !=
                                m_modelSelectedPoints.end();
                    const bool active = IsModelControllerActive(joint);
                    ui.DrawViewportCircle(screen, selectedPoint ? 7.f : 5.5f,
                        kOutline, true);
                    ui.DrawViewportCircle(screen, selectedPoint ? 5.f : 3.5f,
                        selectedPoint ? kHoverColor : active
                            ? EditorUiColor{.28f, .78f, 1.f, 1.f}
                            : EditorUiColor{.36f, .75f, .4f, 1.f}, true);
                    // Dragging the midpoint rotates the parent toward the
                    // pointer while the endpoint remains a position target.
                    auto* parent = joint->Parent;
                    if (!parent || !parent->GetComponent<
                            Engine::Components::AnimationBone>() ||
                        glm::dot(WorldPosition(joint) - WorldPosition(parent),
                            WorldPosition(joint) - WorldPosition(parent)) < 1e-8f)
                        continue;
                    const glm::vec3 parentWorld = WorldPosition(parent);
                    const glm::vec3 childWorld = WorldPosition(joint);
                    const glm::vec3 axis = glm::normalize(childWorld -
                        parentWorld);
                    const glm::vec3 handleWorld = glm::mix(parentWorld,
                        childWorld, .5f);
                    EditorUiVec2 handleScreen{};
                    if (!ProjectPoint(viewProjection, handleWorld,
                            input.available, handleScreen)) continue;
                    ui.DrawViewportCircle(handleScreen, 5.f, kOutline, true);
                    ui.DrawViewportCircle(handleScreen, 3.f,
                        parent == selected ? kHoverColor :
                            EditorUiColor{.45f, 1.f, .58f, 1.f}, true);
                    if (m_modelPickFilter && !m_modelPickFilter(parent))
                        continue;
                    const float handleDistance = Length(Subtract(
                        input.mousePosInViewport, handleScreen));
                    if (handleDistance <= 8.f &&
                        handleDistance < pointHitDistance)
                    {
                        pointHit = parent;
                        pointHitDirection = true;
                        pointHitTwist = false;
                        pointHitWorld = handleWorld;
                        pointHitDistance = handleDistance;
                    }
                    const glm::mat3 orientation = WorldRotation(*parent);
                    glm::vec3 side = orientation[0] - axis *
                        glm::dot(orientation[0], axis);
                    if (glm::dot(side, side) < 1e-6f)
                        side = orientation[1] - axis *
                            glm::dot(orientation[1], axis);
                    if (glm::dot(side, side) < 1e-6f) continue;
                    const glm::vec3 twistWorld = handleWorld +
                        glm::normalize(side) *
                            (.3f * glm::length(childWorld - parentWorld));
                    EditorUiVec2 twistScreen{};
                    if (!ProjectPoint(viewProjection, twistWorld,
                            input.available, twistScreen)) continue;
                    ui.DrawViewportCircle(twistScreen, 5.f, kOutline, true);
                    ui.DrawViewportCircle(twistScreen, 3.f,
                        parent == selected ? kHoverColor :
                            EditorUiColor{1.f, .55f, .85f, 1.f}, true);
                    const float twistDistance = Length(Subtract(
                        input.mousePosInViewport, twistScreen));
                    if (twistDistance <= 8.f &&
                        twistDistance < pointHitDistance)
                    {
                        pointHit = parent;
                        pointHitDirection = false;
                        pointHitTwist = true;
                        pointHitWorld = twistWorld;
                        pointHitTwistAxis = axis;
                        pointHitTwistBase = handleWorld;
                        pointHitDistance = twistDistance;
                    }
                }
            }
        }
        // A control-only point may be attached to a prop or a non-humanoid
        // model node outside the skinning-joint list.
        for (const auto& candidate : scene.GetObjects())
        {
            auto* point = candidate.get();
            const auto* controller = point ? point->GetComponent<
                Engine::Components::AnimationBone>() : nullptr;
            if (!controller || !controller->poseControlOnly ||
                !drawnPoints.insert(point).second ||
                (selectedGroup && !selectedGroup->Contains(point)) ||
                (m_modelPickFilter && !m_modelPickFilter(point))) continue;
            EditorUiVec2 screen{};
            if (!ProjectPoint(viewProjection, WorldPosition(point),
                    input.available, screen)) continue;
            if (point->Parent)
            {
                EditorUiVec2 parentScreen{};
                if (ProjectPoint(viewProjection, WorldPosition(point->Parent),
                        input.available, parentScreen))
                    ui.DrawViewportLine(parentScreen, screen,
                        PoseConnectionColor(controller->PoseLengthRatio()),
                        2.f);
            }
            const float distance = Length(Subtract(
                input.mousePosInViewport, screen));
            if (distance <= 10.f && distance < pointHitDistance)
            {
                pointHit = point;
                pointHitDirection = false;
                pointHitTwist = false;
                pointHitWorld = WorldPosition(point);
                pointHitDistance = distance;
            }
            ui.DrawViewportCircle(screen, point == selected ? 7.f : 5.5f,
                kOutline, true);
            ui.DrawViewportCircle(screen, point == selected ? 5.f : 3.5f,
                point == selected ? kHoverColor :
                    controller->poseControllerActive
                        ? EditorUiColor{.28f, .78f, 1.f, 1.f}
                        : EditorUiColor{.36f, .75f, .4f, 1.f}, true);
        }
    }

    if (m_poseEditing && jointHit) boneHit = jointHit;
    if (m_modelManipulation && input.hovered && input.leftClicked &&
        !input.rightDown && !input.middleDown)
    {
        Engine::Core::Object* meshHit = m_modelSelectionMode != 1 &&
            m_modelSelectionMode != 3
            ? ScenePlacementAndPicking::PickSkinnedBoneInViewport(scene,
                input.mousePosInViewport, input.available) : nullptr;
        Engine::Core::Object* candidate = m_modelSelectionMode == 0
            ? meshHit : m_modelSelectionMode == 1
                ? boneHit : m_modelSelectionMode == 3
                    ? pointHit : (boneHit ? boneHit : meshHit);
        if (!candidate && m_modelSelectionMode != 0 &&
            m_modelSelectionMode != 3)
            candidate = ScenePlacementAndPicking::PickBoneInViewport(scene,
                input.mousePosInViewport, input.available);
        if (candidate && m_modelPickFilter && !m_modelPickFilter(candidate))
            candidate = nullptr;
        if (candidate)
        {
            const bool directionHandle = m_modelSelectionMode == 3 &&
                pointHitDirection;
            const bool twistHandle = m_modelSelectionMode == 3 &&
                pointHitTwist;
            if (m_modelSelectionMode == 3 && !directionHandle &&
                !twistHandle &&
                input.modelGroupSelectDown)
            {
                const auto found = std::find(m_modelSelectedPoints.begin(),
                    m_modelSelectedPoints.end(), candidate);
                if (found == m_modelSelectedPoints.end())
                    m_modelSelectedPoints.push_back(candidate);
                else m_modelSelectedPoints.erase(found);
                result.selectedObject = candidate;
                result.selectionRequested = candidate != selected;
                result.consumedClick = true;
                return result;
            }
            if (m_modelSelectionMode == 3 && !directionHandle &&
                !twistHandle &&
                std::find(m_modelSelectedPoints.begin(),
                    m_modelSelectedPoints.end(), candidate) ==
                        m_modelSelectedPoints.end())
                m_modelSelectedPoints.assign(1, candidate);
            if (IsModelPinned(candidate,
                    m_modelRotate || directionHandle || twistHandle))
            {
                result.selectedObject = candidate;
                result.selectionRequested = candidate != selected;
                result.consumedClick = true;
                return result;
            }
            const glm::vec3 forward = -glm::normalize(glm::vec3(
                scene.editorCamera.transform.GetWorldMatrix()[2]));
            glm::vec3 planeHit{};
            const glm::vec3 world = directionHandle || twistHandle ? pointHitWorld
                : WorldPosition(candidate);
            if (ViewPlanePoint(inverseViewProjection,
                    input.mousePosInViewport, input.available,
                    world, forward, planeHit))
            {
                m_modelDragObject = candidate;
                m_modelDragGroup.clear();
                if (m_modelSelectionMode == 3 && !directionHandle &&
                    !twistHandle &&
                    !m_modelRotate)
                    for (auto* member : m_modelSelectedPoints)
                        if (SceneContains(scene, member))
                        {
                            SetModelControllerActive(member, true);
                            m_modelDragGroup.emplace_back(member,
                                WorldPosition(member));
                        }
                m_modelStartWorld = WorldPosition(candidate);
                if (auto* point = candidate->GetComponent<
                        Engine::Components::AnimationBone>();
                    point && !point->poseRestOffsetConfigured)
                    point->CapturePoseRestOffset();
                for (const auto& [member, startWorld] : m_modelDragGroup)
                    if (auto* point = member->GetComponent<
                            Engine::Components::AnimationBone>();
                        point && !point->poseRestOffsetConfigured)
                        point->CapturePoseRestOffset();
                m_modelStartHandleWorld = world;
                m_modelStartRotation = candidate->transform.rotation;
                m_modelPlaneHit = planeHit;
                m_modelStartMouse = input.mousePosInViewport;
                m_modelDragRotate = m_modelRotate || directionHandle ||
                    twistHandle;
                const auto* controller = candidate->GetComponent<
                    Engine::Components::AnimationBone>();
                m_modelControllerMode = controller
                    ? std::clamp(controller->poseControllerMode, 0, 2) : 0;
                m_modelEffectiveScope = controller
                    ? controller->EffectivePoseScope(m_modelScope)
                    : m_modelScope;
                m_modelChainDepth = controller
                    ? std::clamp(controller->poseControllerChainDepth, 1, 32)
                    : 2;
                if (m_modelSelectionMode == 3 && !m_modelDragRotate)
                    SetModelControllerActive(candidate, true);
                m_modelDragDirection = directionHandle;
                m_modelDragTwist = twistHandle;
                m_modelTwistAxis = pointHitTwistAxis;
                m_modelTwistBaseWorld = pointHitTwistBase;
                m_modelDidMove = false;
                m_modelChangedBones.clear();
                m_modelScrollDepth = 0.f;
                m_modelDragIK = false;
                m_modelRoot = nullptr;
                m_modelSupportRadius = 0.f;
                if (!m_modelDragRotate)
                {
                    for (const auto& owner : scene.GetObjects())
                        for (auto* component : owner->Components)
                            if (auto* skeleton = dynamic_cast<
                                    Engine::Components::Skeleton*>(component))
                            {
                                const auto& joints = skeleton->ResolveJoints();
                                const auto contains = [&](Engine::Core::Object* bone) {
                                    return std::find(joints.begin(), joints.end(), bone)
                                        != joints.end();
                                };
                                auto* hierarchyRoot = candidate;
                                if (!contains(candidate))
                                {
                                    const auto* pointController =
                                        candidate->GetComponent<
                                            Engine::Components::AnimationBone>();
                                    if (!pointController ||
                                        !pointController->poseControlOnly) continue;
                                    while (hierarchyRoot &&
                                        !contains(hierarchyRoot))
                                        hierarchyRoot = hierarchyRoot->Parent;
                                    if (!hierarchyRoot) continue;
                                }
                                while (hierarchyRoot->Parent &&
                                    contains(hierarchyRoot->Parent))
                                    hierarchyRoot = hierarchyRoot->Parent;
                                m_modelRoot = hierarchyRoot;
                                if (!candidate->Parent ||
                                    !candidate->Parent->Parent ||
                                    !contains(candidate->Parent) ||
                                    !contains(candidate->Parent->Parent)) continue;
                                const bool leaf = std::none_of(joints.begin(),
                                    joints.end(), [&](Engine::Core::Object* bone) {
                                        return bone && bone->Parent == candidate;
                                    });
                                if (!leaf) continue;
                                const glm::vec3 root = WorldPosition(candidate->Parent->Parent);
                                const glm::vec3 end = WorldPosition(candidate);
                                m_modelPole = WorldPosition(candidate->Parent) * 2.f -
                                    (root + end) * .5f;
                                m_modelTipRotation = glm::quat_cast(
                                    WorldRotation(*candidate));
                                m_modelDragIK = m_modelEffectiveScope > 0;
                                break;
                            }
                }
                if (!m_modelRoot && controller &&
                    controller->poseControlOnly)
                    for (auto* ancestor = candidate->Parent; ancestor;
                        ancestor = ancestor->Parent)
                        if (ancestor->GetComponent<
                                Engine::Components::Model>())
                        { m_modelRoot = ancestor; break; }
                if (m_modelRoot && m_modelEffectiveScope == 2)
                {
                    glm::vec3 supportSum(0.f);
                    float supportDistance = 0.f;
                    int supportCount = 0;
                    const glm::vec3 rootStart = WorldPosition(m_modelRoot);
                    for (const ModelPin& pin : m_modelPins)
                    {
                        if (!pin.position || !pin.joint) continue;
                        auto* ancestor = pin.joint;
                        while (ancestor && ancestor != m_modelRoot)
                            ancestor = ancestor->Parent;
                        if (!ancestor) continue;
                        supportSum += pin.worldPosition;
                        supportDistance += glm::distance(rootStart,
                            pin.worldPosition);
                        ++supportCount;
                    }
                    if (supportCount > 0)
                    {
                        m_modelSupportCenter = supportSum /
                            static_cast<float>(supportCount);
                        const glm::vec2 horizontal(rootStart.x -
                            m_modelSupportCenter.x, rootStart.z -
                            m_modelSupportCenter.z);
                        m_modelSupportRadius = glm::length(horizontal) +
                            std::max(.05f, .2f * supportDistance /
                                static_cast<float>(supportCount));
                    }
                }
                m_modelPosePreferences.clear();
                if (m_modelRoot)
                    for (const auto& owner : scene.GetObjects())
                    {
                        auto* bone = owner.get();
                        if (!bone || (bone != m_modelRoot &&
                                !bone->GetComponent<
                                    Engine::Components::AnimationBone>()))
                            continue;
                        int depth = 0;
                        auto* ancestor = bone;
                        while (ancestor && ancestor != m_modelRoot)
                        { ancestor = ancestor->Parent; ++depth; }
                        if (!ancestor) continue;
                        m_modelPosePreferences.push_back({bone,
                            glm::quat(bone->transform.rotation),
                            depth == 0 ? .12f : depth <= 2 ? .08f : .04f});
                    }
                result.selectedObject = candidate;
                result.selectionRequested = candidate != selected;
                result.transformDragging = true;
                result.consumedClick = true;
                return result;
            }
        }
    }
    Engine::Core::Object* iconHit = nullptr;
    float iconHitDistance = FLT_MAX;
    for (const auto& objectPointer : scene.GetObjects())
    {
        Engine::Core::Object* object = objectPointer.get();
        if (!object || !object->IsEnabledInHierarchy())
            continue;
        const auto* light = object->GetComponent<Engine::Components::Light>();
        const bool hasLight = light != nullptr;
        const bool hasCamera = object->GetComponent<Engine::Components::Camera>() != nullptr;
        const auto* probe=object->GetComponent<Engine::Components::LightProbe>();
        const auto* probeGroup=object->GetComponent<
            Engine::Components::LightProbeGroup>();
        if (!hasLight && !hasCamera && !probe && !probeGroup)
            continue;

        EditorUiVec2 center{};
        if (!ProjectPoint(viewProjection, WorldPosition(object),
            input.available, center))
            continue;
        if (hasLight)
        {
            DrawLightIcon(ui, center, object == selected);
            if (object == selected &&
                !(object->editorOverlays & Engine::Core::Object::LightVolumeOverlay) &&
                !EditorViewportSettings::AreLightVolumesVisible() &&
                light->GetLightType() ==
                Engine::Components::Light::Type::Spot)
                DrawSpotGizmo(ui, viewProjection, input.available, *object,
                    *light);
        }
        else if(hasCamera)
            DrawCameraIcon(ui, center, object == selected);
        else
            DrawProbeIcon(ui,center,object==selected,probeGroup!=nullptr);

        if(probeGroup&&object==selected)
        {
            const glm::mat4 world=object->transform.GetWorldMatrix();
            const glm::vec3 half=glm::max(glm::abs(probeGroup->size)*.5f,
                glm::vec3(.005f));
            const glm::vec3 corners[8]={{-half.x,-half.y,-half.z},
                {half.x,-half.y,-half.z},{half.x,half.y,-half.z},
                {-half.x,half.y,-half.z},{-half.x,-half.y,half.z},
                {half.x,-half.y,half.z},{half.x,half.y,half.z},
                {-half.x,half.y,half.z}};
            EditorUiVec2 screens[8]{};
            bool visible[8]{};
            for(int corner=0;corner<8;++corner)
                visible[corner]=ProjectPoint(viewProjection,
                    glm::vec3(world*glm::vec4(corners[corner],1.f)),
                    input.available,screens[corner]);
            constexpr int edges[12][2]={{0,1},{1,2},{2,3},{3,0},
                {4,5},{5,6},{6,7},{7,4},{0,4},{1,5},{2,6},{3,7}};
            for(const auto& edge:edges)
                if(visible[edge[0]]&&visible[edge[1]])
                    ui.DrawViewportLine(screens[edge[0]],screens[edge[1]],
                        {.75f,.42f,1.f,.9f},1.5f);

            const int stepX=std::max(1,probeGroup->countX/8);
            const int stepY=std::max(1,probeGroup->countY/8);
            const int stepZ=std::max(1,probeGroup->countZ/8);
            for(int z=0;z<probeGroup->countZ;z+=stepZ)
            for(int y=0;y<probeGroup->countY;y+=stepY)
            for(int x=0;x<probeGroup->countX;x+=stepX)
            {
                EditorUiVec2 sampleScreen{};
                if(ProjectPoint(viewProjection,glm::vec3(world*glm::vec4(
                    probeGroup->LocalSamplePosition(x,y,z),1.f)),
                    input.available,sampleScreen))
                    ui.DrawViewportCircle(sampleScreen,2.25f,
                        probeGroup->valid?EditorUiColor{1.f,.78f,.18f,.9f}:
                        EditorUiColor{.55f,.42f,.65f,.8f},true);
            }
        }

        const float distance = Length(Subtract(input.mousePosInViewport, center));
        if (distance <= 14.f && distance < iconHitDistance)
        {
            iconHit = object;
            iconHitDistance = distance;
        }
    }

    const bool selectedTransformEditable = selected &&
        (!selectedPrefabRoot || selected == selectedPrefabRoot ||
            visibleSkeletonJoints.find(selected) != visibleSkeletonJoints.end() ||
            editingBone);
    int hoveredAxis = -1;
    EditorUiVec2 hoveredDragDirection{};
    EditorUiVec2 originScreen{};
    EditorUiVec2 axisEnds[3]{};
    float axisScale = 0.f;
    bool axisVisible[3]{};
    if (selectedTransformEditable && tool != EditorTransformTool::Hand &&
        ProjectPoint(viewProjection,
        transformPivot, input.available, originScreen))
    {
        const glm::vec3 cameraPosition =
            glm::vec3(scene.editorCamera.transform.GetWorldMatrix()[3]);
        const glm::vec3 selectedPosition = transformPivot;
        axisScale = std::clamp(glm::length(
            selectedPosition - cameraPosition) * 0.18f,
            0.35f, 8.f);
        const glm::mat3 rotation = WorldRotation(*selected);
        const glm::vec3 axes[3] = {
            rotation[0], rotation[1], rotation[2]
        };
        float closestDistance = FLT_MAX;
        if(tool!=EditorTransformTool::Rotate)
        {
            for (int axis = 0; axis < 3; ++axis)
            {
                axisVisible[axis] = ProjectPoint(viewProjection,
                    selectedPosition + axes[axis] * axisScale,
                    input.available, axisEnds[axis]);
                if (!axisVisible[axis])
                    continue;
                float parameter = 0.f;
                const float distance = DistanceToSegment(input.mousePosInViewport,
                    originScreen, axisEnds[axis], parameter);
                if (parameter >= 0.18f && distance <= 8.f && distance < closestDistance)
                {
                    hoveredAxis = axis;
                    closestDistance = distance;
                    const EditorUiVec2 direction=Subtract(axisEnds[axis],originScreen);
                    const float length=Length(direction);
                    if(length>0.001f)
                        hoveredDragDirection=Multiply(direction,1.f/length);
                }
            }
        }

        const EditorUiColor colors[3] = { kXColor, kYColor, kZColor };
        const char* labels[3] = { "X", "Y", "Z" };
        ui.DrawViewportCircle(originScreen, 4.f, kOutline, true);
        if(tool==EditorTransformTool::Rotate)
        {
            constexpr int segments=64;
            for(int axis=0;axis<3;++axis)
            {
                const glm::vec3 basisA=axes[(axis+1)%3];
                const glm::vec3 basisB=axes[(axis+2)%3];
                EditorUiVec2 previous{};
                bool previousVisible=false;
                for(int segment=0;segment<=segments;++segment)
                {
                    constexpr float twoPi=6.28318530718f;
                    const float angle=twoPi*static_cast<float>(segment)/segments;
                    EditorUiVec2 current{};
                    const bool currentVisible=ProjectPoint(viewProjection,
                        selectedPosition+(basisA*std::cos(angle)+
                            basisB*std::sin(angle))*axisScale*.78f,
                        input.available,current);
                    if(previousVisible&&currentVisible)
                    {
                        float parameter=0.f;
                        const float distance=DistanceToSegment(
                            input.mousePosInViewport,previous,current,parameter);
                        if(distance<=7.f&&distance<closestDistance)
                        {
                            hoveredAxis=axis;
                            closestDistance=distance;
                            const EditorUiVec2 tangent=Subtract(current,previous);
                            const float tangentLength=Length(tangent);
                            if(tangentLength>.001f)
                                hoveredDragDirection=Multiply(tangent,
                                    1.f/tangentLength);
                        }
                    }
                    previous=current;
                    previousVisible=currentVisible;
                }
            }
            for(int axis=0;axis<3;++axis)
            {
                const glm::vec3 basisA=axes[(axis+1)%3];
                const glm::vec3 basisB=axes[(axis+2)%3];
                const EditorUiColor color=axis==hoveredAxis||axis==m_dragAxis
                    ?kHoverColor:colors[axis];
                EditorUiVec2 previous{};
                bool previousVisible=false;
                for(int segment=0;segment<=segments;++segment)
                {
                    constexpr float twoPi=6.28318530718f;
                    const float angle=twoPi*static_cast<float>(segment)/segments;
                    EditorUiVec2 current{};
                    const bool currentVisible=ProjectPoint(viewProjection,
                        selectedPosition+(basisA*std::cos(angle)+
                            basisB*std::sin(angle))*axisScale*.78f,
                        input.available,current);
                    if(previousVisible&&currentVisible)
                    {
                        ui.DrawViewportLine(previous,current,kOutline,5.f);
                        ui.DrawViewportLine(previous,current,color,
                            axis==hoveredAxis?3.f:2.f);
                    }
                    previous=current;
                    previousVisible=currentVisible;
                }
            }
        }
        else
        {
            for (int axis = 0; axis < 3; ++axis)
            {
                if (!axisVisible[axis])
                    continue;
                const EditorUiVec2 difference = Subtract(axisEnds[axis], originScreen);
                const float screenLength = Length(difference);
                if (screenLength < 7.f)
                    continue;
                const EditorUiVec2 direction = Multiply(difference, 1.f / screenLength);
                const EditorUiVec2 perpendicular{ -direction.y, direction.x };
                const EditorUiColor color = axis == hoveredAxis || axis == m_dragAxis
                    ? kHoverColor : colors[axis];
                ui.DrawViewportLine(originScreen, axisEnds[axis], kOutline, 6.f);
                ui.DrawViewportLine(originScreen, axisEnds[axis], color, 3.f);
                if (tool == EditorTransformTool::Translate)
                    ui.DrawViewportTriangle(axisEnds[axis],
                        Add(Subtract(axisEnds[axis], Multiply(direction, 11.f)),
                            Multiply(perpendicular, 5.f)),
                        Subtract(Subtract(axisEnds[axis], Multiply(direction, 11.f)),
                            Multiply(perpendicular, 5.f)), color);
                else
                    DrawBoxHandle(ui,axisEnds[axis],6.f,color);
                ui.DrawViewportText(Add(axisEnds[axis], Multiply(perpendicular, 7.f)),
                    labels[axis], color);
            }
        }
    }

    if (m_dragObject && input.leftDown)
    {
        const EditorUiVec2 pointerDelta = Subtract(
            input.mousePosInViewport, m_dragLastMouse);
        int pointerDirection = DragDirection(Dot(pointerDelta,
            m_dragScreenDirection));
        glm::vec3 currentPlaneDirection{};
        const bool currentPlaneValid = m_dragTool == EditorTransformTool::Rotate &&
            m_dragPlaneValid && RotationPlaneDirection(
                inverseViewProjection, input.mousePosInViewport,
                input.available, m_dragPivotWorld, m_dragWorldAxis,
                currentPlaneDirection);
        if (currentPlaneValid)
            pointerDirection = DragDirection(std::atan2(glm::dot(
                m_dragWorldAxis, glm::cross(m_dragPlaneDirection,
                    currentPlaneDirection)),
                glm::dot(m_dragPlaneDirection, currentPlaneDirection)));
        const auto& current = m_dragObject->transform;
        // A solver or constraint may have replaced the requested pose since
        // the previous frame. Rebase at its accepted pose, keep the pointer
        // free, and resume as soon as the drag reverses direction.
        const bool corrected = m_dragHasWritten && (
            (m_dragTool == EditorTransformTool::Translate &&
                glm::distance(current.position,
                    m_dragLastWrittenPosition) > 1e-4f) ||
            (m_dragTool == EditorTransformTool::Rotate &&
                (glm::distance(current.rotation,
                    m_dragLastWrittenRotation) > 1e-4f ||
                    (m_dragWasBone && m_bonePivotMode != 0 &&
                        glm::distance(current.position,
                            m_dragLastWrittenPosition) > 1e-4f))) ||
            (m_dragTool == EditorTransformTool::Scale &&
                (glm::distance(current.scale,
                    m_dragLastWrittenScale) > 1e-4f ||
                    (m_dragWasBone && m_bonePivotMode != 0 &&
                        glm::distance(current.position,
                            m_dragLastWrittenPosition) > 1e-4f))));
        if (corrected)
        {
            m_dragBlockedDirection = m_dragLastAttemptDirection;
            m_dragHasWritten = false;
            m_dragStartLocalPosition = current.position;
            m_dragStartLocalRotation = current.rotation;
            m_dragStartLocalScale = current.scale;
            m_dragStartWorldPosition = WorldPosition(m_dragObject);
            m_dragStartMouse = input.mousePosInViewport;
            m_dragLastMouse = input.mousePosInViewport;
            m_dragAccumulatedAngle = 0.f;
            m_dragWorldAxis = WorldRotation(*m_dragObject)[m_dragAxis];
            m_dragPlaneValid = m_dragTool == EditorTransformTool::Rotate &&
                RotationPlaneDirection(inverseViewProjection,
                    input.mousePosInViewport, input.available,
                    m_dragPivotWorld, m_dragWorldAxis, m_dragPlaneDirection);
            result.transformDragging = true;
            result.consumedClick = true;
            return result;
        }
        if (m_dragBlockedDirection != 0 &&
            (pointerDirection == 0 ||
                pointerDirection == m_dragBlockedDirection))
        {
            m_dragStartLocalPosition = current.position;
            m_dragStartLocalRotation = current.rotation;
            m_dragStartLocalScale = current.scale;
            m_dragStartWorldPosition = WorldPosition(m_dragObject);
            m_dragStartMouse = input.mousePosInViewport;
            m_dragLastMouse = input.mousePosInViewport;
            if (currentPlaneValid)
                m_dragPlaneDirection = currentPlaneDirection;
            else if (m_dragTool == EditorTransformTool::Rotate)
                m_dragPlaneValid = false;
            result.transformDragging = true;
            result.consumedClick = true;
            return result;
        }
        if (m_dragBlockedDirection != 0)
        {
            m_dragBlockedDirection = 0;
            m_dragStartLocalPosition = current.position;
            m_dragStartLocalRotation = current.rotation;
            m_dragStartLocalScale = current.scale;
            m_dragStartWorldPosition = WorldPosition(m_dragObject);
            m_dragStartMouse = m_dragLastMouse;
            m_dragAccumulatedAngle = 0.f;
        }
        const glm::vec3 previousPosition = m_dragObject->transform.position;
        const glm::vec3 previousRotation = m_dragObject->transform.rotation;
        const glm::vec3 previousScale = m_dragObject->transform.scale;
        const float pixels = Dot(Subtract(
            input.mousePosInViewport, m_dragStartMouse), m_dragScreenDirection);
        if (m_dragTool == EditorTransformTool::Translate)
        {
            float distance = pixels * m_dragWorldUnitsPerPixel;
            if (m_dragWasBone && m_boneTranslationSnap > 0.f)
                distance = std::round(distance / m_boneTranslationSnap) *
                    m_boneTranslationSnap;
            const glm::vec3 worldDelta =
                m_dragWorldAxis * distance;
            glm::vec3 localDelta = worldDelta;
            if (m_dragObject->Parent)
            {
                const glm::mat4 parentWorld =
                    m_dragObject->Parent->transform.GetWorldMatrix();
                if (std::abs(glm::determinant(parentWorld)) > 0.000001f)
                    localDelta = glm::vec3(glm::inverse(parentWorld) *
                        glm::vec4(worldDelta, 0.f));
            }
            if (!m_poseEditing || !m_poseTranslationHandler ||
                !m_poseTranslationHandler(m_dragObject,
                    m_dragStartWorldPosition + worldDelta))
                m_dragObject->transform.position =
                    m_dragStartLocalPosition + localDelta;
        }
        else if (m_dragTool == EditorTransformTool::Rotate)
        {
            float angle = pixels * 0.01f;
            glm::vec3 planeDirection{};
            if (m_dragPlaneValid && RotationPlaneDirection(
                    inverseViewProjection, input.mousePosInViewport,
                    input.available, m_dragPivotWorld, m_dragWorldAxis,
                    planeDirection))
            {
                const float step = std::atan2(glm::dot(m_dragWorldAxis,
                    glm::cross(m_dragPlaneDirection, planeDirection)),
                    glm::dot(m_dragPlaneDirection, planeDirection));
                m_dragAccumulatedAngle += step;
                m_dragPlaneDirection = planeDirection;
                angle = m_dragAccumulatedAngle;
            }
            else
            {
                m_dragPlaneValid = false;
                m_dragAccumulatedAngle += Dot(Subtract(
                    input.mousePosInViewport, m_dragLastMouse),
                    m_dragScreenDirection) * .01f;
                angle = m_dragAccumulatedAngle;
            }
            m_dragLastMouse = input.mousePosInViewport;
            if (m_dragWasBone && m_boneRotationSnap > 0.f)
            {
                const float step = glm::radians(m_boneRotationSnap);
                angle = std::round(angle / step) * step;
            }
            const glm::quat parentOrientation = m_dragObject->Parent
                ? glm::quat_cast(WorldRotation(*m_dragObject->Parent))
                : glm::quat(1.f, 0.f, 0.f, 0.f);
            const glm::quat worldDelta = glm::angleAxis(angle,
                glm::normalize(m_dragWorldAxis));
            const glm::quat startLocal = glm::quat(m_dragStartLocalRotation);
            m_dragObject->transform.rotation = glm::eulerAngles(
                glm::normalize(glm::inverse(parentOrientation) * worldDelta *
                    parentOrientation * startLocal));
            if (m_dragWasBone && m_bonePivotMode != 0)
            {
                const glm::vec3 rotated = m_dragPivotWorld +
                    glm::vec3(glm::rotate(glm::mat4(1.f), angle,
                        m_dragWorldAxis) * glm::vec4(
                            m_dragStartWorldPosition - m_dragPivotWorld, 0.f));
                const glm::mat4 parentWorld = m_dragObject->Parent
                    ? m_dragObject->Parent->transform.GetWorldMatrix()
                    : glm::mat4(1.f);
                if (std::abs(glm::determinant(parentWorld)) > 0.000001f)
                    m_dragObject->transform.position = glm::vec3(
                        glm::inverse(parentWorld) * glm::vec4(rotated, 1.f));
            }
        }
        else if (m_dragTool == EditorTransformTool::Scale)
        {
            float change = pixels * 0.01f;
            if (m_dragWasBone && m_boneScaleSnap > 0.f)
                change = std::round(change / m_boneScaleSnap) * m_boneScaleSnap;
            const float requestedScale =
                m_dragStartLocalScale[m_dragAxis] + change;
            m_dragObject->transform.scale[m_dragAxis] =
                std::max(0.001f, requestedScale);
            if (requestedScale < 0.001f)
                m_dragBlockedDirection = -1;
            if (m_dragWasBone && m_bonePivotMode != 0 &&
                std::abs(m_dragStartLocalScale[m_dragAxis]) > 0.000001f)
            {
                const glm::vec3 offset = m_dragStartWorldPosition -
                    m_dragPivotWorld;
                const float ratio = m_dragObject->transform.scale[m_dragAxis] /
                    m_dragStartLocalScale[m_dragAxis];
                const glm::vec3 newWorld = m_dragStartWorldPosition +
                    m_dragWorldAxis * glm::dot(offset, m_dragWorldAxis) *
                    (ratio - 1.f);
                const glm::mat4 parentWorld = m_dragObject->Parent
                    ? m_dragObject->Parent->transform.GetWorldMatrix()
                    : glm::mat4(1.f);
                if (std::abs(glm::determinant(parentWorld)) > 0.000001f)
                    m_dragObject->transform.position = glm::vec3(
                        glm::inverse(parentWorld) * glm::vec4(newWorld, 1.f));
            }
        }
        const uint8_t channel = m_dragTool == EditorTransformTool::Translate
            ? Engine::Components::Transform::EditorPosition
            : m_dragTool == EditorTransformTool::Rotate
                ? static_cast<uint8_t>(Engine::Components::Transform::EditorRotation |
                    (m_dragWasBone && m_bonePivotMode != 0
                        ? Engine::Components::Transform::EditorPosition : 0))
                : static_cast<uint8_t>(Engine::Components::Transform::EditorScale |
                    (m_dragWasBone && m_bonePivotMode != 0
                        ? Engine::Components::Transform::EditorPosition : 0));
        if (m_dragObject->transform.position != previousPosition ||
            m_dragObject->transform.rotation != previousRotation ||
            m_dragObject->transform.scale != previousScale)
        {
            m_dragObject->transform.NotifyEditorTransformChanged(channel);
            if (auto* body =
                m_dragObject->GetComponent<Engine::Components::RigidBody>())
                body->NotifyEditorTransformChanged();
        }
        m_dragLastWrittenPosition = m_dragObject->transform.position;
        m_dragLastWrittenRotation = m_dragObject->transform.rotation;
        m_dragLastWrittenScale = m_dragObject->transform.scale;
        m_dragHasWritten = true;
        if (pointerDirection != 0)
            m_dragLastAttemptDirection = pointerDirection;
        if (m_dragBlockedDirection != 0)
        {
            m_dragStartLocalPosition = m_dragObject->transform.position;
            m_dragStartLocalRotation = m_dragObject->transform.rotation;
            m_dragStartLocalScale = m_dragObject->transform.scale;
            m_dragStartWorldPosition = WorldPosition(m_dragObject);
            m_dragStartMouse = input.mousePosInViewport;
            m_dragAccumulatedAngle = 0.f;
        }
        m_dragLastMouse = input.mousePosInViewport;
        result.transformDragging = true;
        result.consumedClick = true;
    }
    else if (input.hovered && input.leftClicked && selected && hoveredAxis >= 0)
    {
        const float screenLength = Length(hoveredDragDirection);
        if (screenLength >= .9f)
        {
            const glm::mat3 rotation = WorldRotation(*selected);
            const glm::vec3 axes[3] = {
                rotation[0], rotation[1], rotation[2]
            };
            m_dragObject = selected;
            m_dragWasBone = editingBone;
            m_dragPivotWorld = transformPivot;
            m_dragStartWorldPosition = WorldPosition(selected);
            m_dragAxis = hoveredAxis;
            m_dragStartLocalPosition = selected->transform.position;
            m_dragStartLocalRotation = selected->transform.rotation;
            m_dragStartLocalScale = selected->transform.scale;
            m_dragWorldAxis = axes[hoveredAxis];
            m_dragAccumulatedAngle = 0.f;
            m_dragPlaneValid = tool == EditorTransformTool::Rotate &&
                RotationPlaneDirection(inverseViewProjection,
                    input.mousePosInViewport, input.available,
                    transformPivot, m_dragWorldAxis, m_dragPlaneDirection);
            m_dragStartMouse = input.mousePosInViewport;
            m_dragLastMouse = input.mousePosInViewport;
            m_dragScreenDirection = Multiply(hoveredDragDirection,
                1.f / screenLength);
            const float projectedAxisLength=tool==EditorTransformTool::Rotate
                ?1.f:Length(Subtract(axisEnds[hoveredAxis],originScreen));
            m_dragWorldUnitsPerPixel = tool==EditorTransformTool::Rotate
                ?0.f:axisScale/std::max(projectedAxisLength,1.f);
            m_dragTool = tool;
            m_dragHasWritten = false;
            m_dragBlockedDirection = 0;
            m_dragLastAttemptDirection = 0;
            result.transformDragging = true;
            result.consumedClick = true;
        }
    }
    else if (input.hovered && input.leftClicked && iconHit)
    {
        result.selectedObject = iconHit;
        result.selectionRequested = true;
        result.consumedClick = true;
    }
    else if (input.hovered && input.leftClicked && boneHit)
    {
        result.selectedObject = boneHit;
        result.selectionRequested = true;
        result.consumedClick = true;
    }

    return result;
}
}

#include "EditorGizmoSystem.h"
#include "Core/Scene/Scene.h"
#include "Core/Object.h"
#include "Core/Compoonents/Camera/Camera.h"
#include "Core/Compoonents/Lighting/Light.h"
#include "Core/Compoonents/Lighting/LightProbe.h"
#include "Core/Compoonents/Obj/Mesh.h"
#include "Core/Compoonents/Obj/Sprite.h"
#include "Core/Compoonents/Physics/RigidBody.h"
#include "Core/Compoonents/Animation/Skeleton.h"
#include "Core/Compoonents/Animation/BoneGroup.h"
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

void EditorGizmoSystem::DrawSkeletonOverlay(
    Engine::Scene::Scene& scene,
    const Engine::Components::Skeleton& skeleton, IEditorUi& ui,
    const EditorUiViewportInput& input, int selectedPaletteIndex)
{
    if (!scene.AreEditorBonesVisible()) return;
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
        if (overlays & Engine::Core::Object::BoundsOverlay)
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
            if (const auto* probes=object->GetComponent<
                    Engine::Components::LightProbeGroup>())
                DrawWorldBox(ui,viewProjection,input.available,world,
                    -glm::abs(probes->size)*.5f,
                    glm::abs(probes->size)*.5f,color);
        }
        if (overlays & Engine::Core::Object::LightVolumeOverlay)
            if (const auto* light=object->GetComponent<Engine::Components::Light>())
                DrawLightVolume(ui,viewProjection,input.available,*object,*light);
        if (overlays & Engine::Core::Object::CameraFrustumOverlay)
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
            if (!scene.AreEditorBonesVisible() || !skeleton ||
                !(owner->editorOverlays & (Engine::Core::Object::BoneOverlay |
                    Engine::Core::Object::SkeletonOverlay))) continue;
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
                if ((owner->editorOverlays & Engine::Core::Object::BoneOverlay) &&
                    hitDistance <= 9.f && hitDistance < boneHitDistance)
                {
                    boneHit = joint;
                    boneHitDistance = hitDistance;
                }
                if ((owner->editorOverlays & Engine::Core::Object::SkeletonOverlay) &&
                    hitDistance <= 9.f && hitDistance < jointHitDistance)
                {
                    jointHit = joint;
                    jointHitDistance = hitDistance;
                }

                if (!parentJoint)
                {
                    if ((owner->editorOverlays & Engine::Core::Object::SkeletonOverlay) &&
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
                if (owner->editorOverlays & Engine::Core::Object::BoneOverlay)
                    DrawBoneShape(ui, rootScreen, tipScreen,
                        joint == selected || parentJoint == selected);
                if (owner->editorOverlays & Engine::Core::Object::SkeletonOverlay)
                {
                    ui.DrawViewportCircle(rootScreen, 4.f, kOutline, true);
                    ui.DrawViewportCircle(rootScreen, 2.5f,
                        parentJoint == selected ? kHoverColor :
                            EditorUiColor{ 0.55f, 0.9f, 1.f, 1.f }, true);
                }

                float parameter = 0.f;
                hitDistance = DistanceToSegment(input.mousePosInViewport,
                    rootScreen, tipScreen, parameter);
                if ((owner->editorOverlays & Engine::Core::Object::BoneOverlay) &&
                    hitDistance <= 7.f && hitDistance < boneHitDistance)
                {
                    boneHit = m_poseEditing ? parentJoint : joint;
                    boneHitDistance = hitDistance;
                }
            }
        }
    }

    if (m_poseEditing && jointHit) boneHit = jointHit;
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

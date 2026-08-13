#include "EditorGizmoSystem.h"
#include "Core/Scene/Scene.h"
#include "Core/Object.h"
#include "Core/Compoonents/Camera.h"
#include "Core/Compoonents/Light.h"
#include "Core/Compoonents/Animation/Skeleton.h"
#include <algorithm>
#include <cmath>
#include <cfloat>
#include <set>
#include <unordered_set>
#include <glm/gtc/matrix_inverse.hpp>

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

bool SceneContains(const Scene& scene, const Object* object)
{
    for (const auto& candidate : scene.GetObjects())
        if (candidate.get() == object)
            return true;
    return false;
}

void DrawBoneShape(IEditorUi& ui, EditorUiVec2 root, EditorUiVec2 tip,
    bool selected)
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
    const EditorUiColor fill = selected
        ? EditorUiColor{ 1.f, 0.72f, 0.16f, 0.75f }
        : EditorUiColor{ 0.35f, 0.78f, 1.f, 0.58f };
    const EditorUiColor line = selected
        ? kHoverColor : EditorUiColor{ 0.45f, 0.86f, 1.f, 1.f };
    ui.DrawViewportTriangle(root, first, tip, fill);
    ui.DrawViewportTriangle(root, tip, second, fill);
    ui.DrawViewportLine(root, first, kOutline, 4.f);
    ui.DrawViewportLine(first, tip, kOutline, 4.f);
    ui.DrawViewportLine(tip, second, kOutline, 4.f);
    ui.DrawViewportLine(second, root, kOutline, 4.f);
    ui.DrawViewportLine(root, first, line, 1.5f);
    ui.DrawViewportLine(first, tip, line, 1.5f);
    ui.DrawViewportLine(tip, second, line, 1.5f);
    ui.DrawViewportLine(second, root, line, 1.5f);
}
}

EditorGizmoResult EditorGizmoSystem::DrawAndHandle(
    Scene& scene, IEditorUi& ui, const EditorUiViewportInput& input)
{
    EditorGizmoResult result{};
    const Camera* camera = scene.editorCamera.GetComponent<Camera>();
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
    Object* selected = scene.GetSelectedObject();
    Object* selectedPrefabRoot = selected
        ? selected->GetPrefabInstanceRoot() : nullptr;

    Object* boneHit = nullptr;
    float boneHitDistance = FLT_MAX;
    std::set<std::pair<Object*, Object*>> drawnBones;
    std::unordered_set<Object*> drawnRoots;
    std::unordered_set<Object*> visibleSkeletonJoints;
    for (const auto& ownerPointer : scene.GetObjects())
    {
        Object* owner = ownerPointer.get();
        if (!owner || !owner->IsEnabledInHierarchy()) continue;
        for (Component* component : owner->Components)
        {
            auto* skeleton = dynamic_cast<Skeleton*>(component);
            if (!skeleton || !skeleton->showBones) continue;
            std::unordered_set<Object*> joints;
            for (Object* joint : skeleton->ResolveJoints())
                if (joint)
                {
                    joints.insert(joint);
                    visibleSkeletonJoints.insert(joint);
                }
            for (Object* joint : joints)
            {
                Object* parentJoint = joint->Parent;
                while (parentJoint && joints.find(parentJoint) == joints.end())
                    parentJoint = parentJoint->Parent;

                EditorUiVec2 tipScreen{};
                if (!ProjectPoint(viewProjection,
                    joint->transform.GetWorldPosition(), input.available,
                    tipScreen))
                    continue;

                float hitDistance = Length(Subtract(
                    input.mousePosInViewport, tipScreen));
                if (hitDistance <= 9.f && hitDistance < boneHitDistance)
                {
                    boneHit = joint;
                    boneHitDistance = hitDistance;
                }

                if (!parentJoint)
                {
                    if (drawnRoots.insert(joint).second)
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
                EditorUiVec2 rootScreen{};
                if (!ProjectPoint(viewProjection,
                    parentJoint->transform.GetWorldPosition(), input.available,
                    rootScreen))
                    continue;
                DrawBoneShape(ui, rootScreen, tipScreen,
                    joint == selected || parentJoint == selected);
                ui.DrawViewportCircle(rootScreen, 4.f, kOutline, true);
                ui.DrawViewportCircle(rootScreen, 2.5f,
                    parentJoint == selected ? kHoverColor :
                        EditorUiColor{ 0.55f, 0.9f, 1.f, 1.f }, true);

                float parameter = 0.f;
                hitDistance = DistanceToSegment(input.mousePosInViewport,
                    rootScreen, tipScreen, parameter);
                if (hitDistance <= 7.f && hitDistance < boneHitDistance)
                {
                    boneHit = joint;
                    boneHitDistance = hitDistance;
                }
            }
        }
    }

    Object* iconHit = nullptr;
    float iconHitDistance = FLT_MAX;
    for (const auto& objectPointer : scene.GetObjects())
    {
        Object* object = objectPointer.get();
        if (!object || !object->IsEnabledInHierarchy())
            continue;
        const bool hasLight = object->GetComponent<Light>() != nullptr;
        const bool hasCamera = object->GetComponent<Camera>() != nullptr;
        if (!hasLight && !hasCamera)
            continue;

        EditorUiVec2 center{};
        if (!ProjectPoint(viewProjection, object->transform.GetWorldPosition(),
            input.available, center))
            continue;
        if (hasLight)
            DrawLightIcon(ui, center, object == selected);
        else
            DrawCameraIcon(ui, center, object == selected);

        const float distance = Length(Subtract(input.mousePosInViewport, center));
        if (distance <= 14.f && distance < iconHitDistance)
        {
            iconHit = object;
            iconHitDistance = distance;
        }
    }

    const bool selectedTransformEditable = selected &&
        (!selectedPrefabRoot || selected == selectedPrefabRoot ||
            visibleSkeletonJoints.find(selected) != visibleSkeletonJoints.end());
    int hoveredAxis = -1;
    EditorUiVec2 originScreen{};
    EditorUiVec2 axisEnds[3]{};
    float axisScale = 0.f;
    bool axisVisible[3]{};
    if (selectedTransformEditable && ProjectPoint(viewProjection,
        selected->transform.GetWorldPosition(), input.available, originScreen))
    {
        const glm::vec3 cameraPosition =
            glm::vec3(scene.editorCamera.transform.GetWorldMatrix()[3]);
        axisScale = std::clamp(glm::length(
            selected->transform.GetWorldPosition() - cameraPosition) * 0.18f,
            0.35f, 8.f);
        const glm::vec3 axes[3] = {
            { 1.f, 0.f, 0.f }, { 0.f, 1.f, 0.f }, { 0.f, 0.f, 1.f }
        };
        float closestDistance = FLT_MAX;
        for (int axis = 0; axis < 3; ++axis)
        {
            axisVisible[axis] = ProjectPoint(viewProjection,
                selected->transform.GetWorldPosition() + axes[axis] * axisScale,
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
            }
        }

        const EditorUiColor colors[3] = { kXColor, kYColor, kZColor };
        const char* labels[3] = { "X", "Y", "Z" };
        ui.DrawViewportCircle(originScreen, 4.f, kOutline, true);
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
            ui.DrawViewportTriangle(axisEnds[axis],
                Add(Subtract(axisEnds[axis], Multiply(direction, 11.f)),
                    Multiply(perpendicular, 5.f)),
                Subtract(Subtract(axisEnds[axis], Multiply(direction, 11.f)),
                    Multiply(perpendicular, 5.f)), color);
            ui.DrawViewportText(Add(axisEnds[axis], Multiply(perpendicular, 7.f)),
                labels[axis], color);
        }
    }

    if (m_dragObject && input.leftDown)
    {
        const float pixels = Dot(Subtract(
            input.mousePosInViewport, m_dragStartMouse), m_dragScreenDirection);
        const glm::vec3 worldDelta =
            m_dragWorldAxis * pixels * m_dragWorldUnitsPerPixel;
        glm::vec3 localDelta = worldDelta;
        if (m_dragObject->Parent)
        {
            const glm::mat4 parentWorld =
                m_dragObject->Parent->transform.GetWorldMatrix();
            if (std::abs(glm::determinant(parentWorld)) > 0.000001f)
                localDelta = glm::vec3(glm::inverse(parentWorld) *
                    glm::vec4(worldDelta, 0.f));
        }
        m_dragObject->transform.position = m_dragStartLocalPosition + localDelta;
        result.transformDragging = true;
        result.consumedClick = true;
    }
    else if (input.hovered && input.leftClicked && selected && hoveredAxis >= 0)
    {
        const EditorUiVec2 screenAxis = Subtract(axisEnds[hoveredAxis], originScreen);
        const float screenLength = Length(screenAxis);
        if (screenLength >= 7.f)
        {
            const glm::vec3 axes[3] = {
                { 1.f, 0.f, 0.f }, { 0.f, 1.f, 0.f }, { 0.f, 0.f, 1.f }
            };
            m_dragObject = selected;
            m_dragAxis = hoveredAxis;
            m_dragStartLocalPosition = selected->transform.position;
            m_dragWorldAxis = axes[hoveredAxis];
            m_dragStartMouse = input.mousePosInViewport;
            m_dragScreenDirection = Multiply(screenAxis, 1.f / screenLength);
            m_dragWorldUnitsPerPixel = axisScale / screenLength;
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

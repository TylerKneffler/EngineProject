#include "SpatialManipulator.h"
#include "Core/Math/FormulaExpression.h"
#include "Core/Object.h"
#include "Core/Compoonents/Physics/Collider.h"
#include "Core/Compoonents/Materials/Material.h"
#include "Core/Physics/Physics.h"
#include "Core/Scene/Scene.h"
#include "Engine/Editor/UI/IEditorUi.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <glm/gtc/quaternion.hpp>
#include <unordered_set>

namespace Engine::Components
{
namespace
{
float Clamp01(float value)
{
    return std::max(0.f, std::min(1.f, value));
}

glm::vec3 SafeNormalize(const glm::vec3& value, const glm::vec3& fallback)
{
    const float lengthSquared = glm::dot(value, value);
    if (lengthSquared <= 1e-8f)
        return fallback;
    return value / std::sqrt(lengthSquared);
}

bool MatricesNearlyEqual(const glm::mat4& first, const glm::mat4& second,
    float epsilon = 1e-5f)
{
    for (int column = 0; column < 4; ++column)
    {
        for (int row = 0; row < 4; ++row)
        {
            if (std::abs(first[column][row] - second[column][row]) > epsilon)
                return false;
        }
    }
    return true;
}

void BuildBasis(const glm::vec3& normal, glm::vec3& tangent,
    glm::vec3& bitangent, glm::vec3& normalized)
{
    normalized = SafeNormalize(normal, glm::vec3(0.f, 0.f, 1.f));
    const glm::vec3 reference = std::abs(normalized.z) < 0.999f
        ? glm::vec3(0.f, 0.f, 1.f)
        : glm::vec3(0.f, 1.f, 0.f);
    tangent = SafeNormalize(glm::cross(reference, normalized), glm::vec3(1.f, 0.f, 0.f));
    bitangent = SafeNormalize(glm::cross(normalized, tangent), glm::vec3(0.f, 1.f, 0.f));
}

glm::mat4 BuildTransformMatrix(const glm::vec3& translation,
    const glm::vec3& rotation,
    const glm::vec3& scale)
{
    const glm::mat4 translationMatrix = glm::translate(glm::mat4(1.f), translation);
    const glm::mat4 rx = glm::rotate(glm::mat4(1.f), rotation.x, { 1.f, 0.f, 0.f });
    const glm::mat4 ry = glm::rotate(glm::mat4(1.f), rotation.y, { 0.f, 1.f, 0.f });
    const glm::mat4 rz = glm::rotate(glm::mat4(1.f), rotation.z, { 0.f, 0.f, 1.f });
    const glm::mat4 scaleMatrix = glm::scale(glm::mat4(1.f), scale);
    return translationMatrix * rz * ry * rx * scaleMatrix;
}

glm::vec3 ComputeCenter(const std::vector<glm::vec3>& points)
{
    if (points.empty())
        return glm::vec3(0.f);
    glm::vec3 sum(0.f);
    for (const glm::vec3& point : points)
        sum += point;
    return sum / static_cast<float>(points.size());
}

void VisitObjectTree(Engine::Core::Object* object,
    const std::function<void(Engine::Core::Object*)>& visitor)
{
    if (!object)
        return;
    visitor(object);
    for (Engine::Core::Object* child : object->Children)
        VisitObjectTree(child, visitor);
}

}

SpatialManipulator::SpatialManipulator()
{
    SetTypeName(COMPONENT_TYPE_NAME(SpatialManipulator));
    RegisterField("enabled", enabled);
    RegisterField("position", position);
    RegisterField("rotation", rotation);
    RegisterField("scale", scale);
    RegisterField("connectionMode", connectionMode);
    RegisterField("matrixOverlayScopeRoot", matrixOverlayScopeRoot);
    RegisterField("matrixOverlayIncludeChildren", matrixOverlayIncludeChildren);
    RegisterField("matrixOverlayPriority", matrixOverlayPriority);
    RegisterField("definesWarpVolume", definesWarpVolume);
    RegisterField("warpPriority", warpPriority);
    RegisterField("warpVolumeShape", warpVolumeShape);
    RegisterField("warpVolumeSize", warpVolumeSize);
    RegisterField("warpVolumeRadius", warpVolumeRadius);
    RegisterField("warpBoundaryFalloff", warpBoundaryFalloff);
    RegisterField("applyTraversalScale", applyTraversalScale);
    RegisterField("persistTraversalScaleOnExit", persistTraversalScaleOnExit);
    RegisterField("spaceWarpType", spaceWarpType);
    RegisterField("spiralAxis", spiralAxis);
    RegisterField("spiralRadiansPerUnit", spiralRadiansPerUnit);
    RegisterField("formulaX", formulaX);
    RegisterField("formulaY", formulaY);
    RegisterField("formulaZ", formulaZ);
    RegisterField("formulaA", formulaA);
    RegisterField("formulaB", formulaB);
    RegisterField("formulaC", formulaC);
    RegisterField("formulaD", formulaD);
    RegisterField("portalPoint", portalPoint);
    RegisterField("portalNormal", portalNormal);
    RegisterField("portalPointCount", portalPointCount);
    RegisterField("portalShapePoint0", portalShapePoint0);
    RegisterField("portalShapePoint1", portalShapePoint1);
    RegisterField("portalShapePoint2", portalShapePoint2);
    RegisterField("portalShapePoint3", portalShapePoint3);
    RegisterField("portalShapePoint4", portalShapePoint4);
    RegisterField("portalShapePoint5", portalShapePoint5);
    RegisterField("portalShapePoint6", portalShapePoint6);
    RegisterField("portalShapePoint7", portalShapePoint7);
    RegisterField("autoMatchPortalPointCount", autoMatchPortalPointCount);
    RegisterField("deformMeshOnTraversal", deformMeshOnTraversal);
    RegisterField("traversalBlendDistance", traversalBlendDistance);
    RegisterField("deformationStrength", deformationStrength);
    RegisterField("portalEdgeHalfWidth", portalEdgeHalfWidth);
    RegisterField("portalEdgeHalfDepth", portalEdgeHalfDepth);
    RegisterField("portalCollisionCutUpdateDistance", portalCollisionCutUpdateDistance);
    RegisterField("materializeSplitOnDisconnect", materializeSplitOnDisconnect);
    RegisterField("portalTraversalPriority", portalTraversalPriority);
    RegisterField("meshReference", meshReference);
    RegisterField("traversalTriggerBodyReference", traversalTriggerBodyReference);
    RegisterField("targetManipulator", targetManipulator);
}

glm::mat4 SpatialManipulator::GetOverlayMatrix() const
{
    return BuildTransformMatrix(position, rotation, scale);
}

bool SpatialManipulator::ContainsWorldPoint(const glm::vec3& worldPoint) const
{
    if (!enabled || !definesWarpVolume || !Owner)
        return false;

    const glm::vec3 localPoint = glm::vec3(glm::inverse(
        Owner->transform.GetWorldMatrix()) * glm::vec4(worldPoint, 1.f));
    switch (static_cast<WarpVolumeShape>(warpVolumeShape))
    {
    case WarpVolumeShape::Box:
    {
        const glm::vec3 halfSize = glm::max(glm::abs(warpVolumeSize) * 0.5f,
            glm::vec3(0.0001f));
        return std::abs(localPoint.x) <= halfSize.x &&
            std::abs(localPoint.y) <= halfSize.y &&
            std::abs(localPoint.z) <= halfSize.z;
    }
    case WarpVolumeShape::Sphere:
    {
        const float radius = std::max(0.001f, std::abs(warpVolumeRadius));
        return glm::dot(localPoint, localPoint) <= radius * radius;
    }
    case WarpVolumeShape::Infinite:
    default:
        return true;
    }
}

glm::vec3 SpatialManipulator::MapWorldPointThroughVolume(
    const glm::vec3& worldPoint) const
{
    if (!ContainsWorldPoint(worldPoint))
        return worldPoint;

    const glm::mat4 volumeWorld = Owner->transform.GetWorldMatrix();
    const glm::vec3 localPoint = glm::vec3(glm::inverse(volumeWorld) *
        glm::vec4(worldPoint, 1.f));
    const SpaceWarpType warpType = static_cast<SpaceWarpType>(spaceWarpType);
    glm::vec3 mappedLocal = localPoint;
    if (warpType == SpaceWarpType::Formula)
    {
        const Engine::Math::FormulaVariables variables {
            localPoint.x, localPoint.y, localPoint.z,
            formulaA, formulaB, formulaC, formulaD
        };
        double mappedX = 0.0;
        double mappedY = 0.0;
        double mappedZ = 0.0;
        if (!Engine::Math::EvaluateFormula(formulaX, variables, mappedX) ||
            !Engine::Math::EvaluateFormula(formulaY, variables, mappedY) ||
            !Engine::Math::EvaluateFormula(formulaZ, variables, mappedZ))
            return worldPoint;
        mappedLocal = glm::vec3(GetOverlayMatrix() * glm::vec4(
            static_cast<float>(mappedX), static_cast<float>(mappedY),
            static_cast<float>(mappedZ), 1.f));
        if (!std::isfinite(mappedLocal.x) || !std::isfinite(mappedLocal.y) ||
            !std::isfinite(mappedLocal.z))
            return worldPoint;
    }
    else
    {
        mappedLocal = glm::vec3(GetOverlayMatrix() * glm::vec4(localPoint, 1.f));
    }

    if (warpType == SpaceWarpType::Spiral)
    {
        const glm::vec3 axis = SafeNormalize(spiralAxis, glm::vec3(0.f, 1.f, 0.f));
        const float angle = glm::dot(localPoint, axis) * spiralRadiansPerUnit;
        mappedLocal = glm::vec3(glm::rotate(glm::mat4(1.f), angle, axis) *
            glm::vec4(mappedLocal, 1.f));
    }

    const float falloff = std::max(0.f, warpBoundaryFalloff);
    if (falloff > 0.f)
    {
        float boundaryDistance = falloff;
        switch (static_cast<WarpVolumeShape>(warpVolumeShape))
        {
        case WarpVolumeShape::Box:
        {
            const glm::vec3 halfSize = glm::max(glm::abs(warpVolumeSize) * 0.5f,
                glm::vec3(0.0001f));
            const glm::vec3 remaining = halfSize - glm::abs(localPoint);
            boundaryDistance = std::min(remaining.x,
                std::min(remaining.y, remaining.z));
            break;
        }
        case WarpVolumeShape::Sphere:
            boundaryDistance = std::max(0.f, std::abs(warpVolumeRadius) -
                glm::length(localPoint));
            break;
        case WarpVolumeShape::Infinite:
        default:
            break;
        }
        mappedLocal = glm::mix(localPoint, mappedLocal,
            Clamp01(boundaryDistance / falloff));
    }

    return glm::vec3(volumeWorld * glm::vec4(mappedLocal, 1.f));
}

int SpatialManipulator::GetClampedPortalPointCount() const
{
    return std::max(3, std::min(kMaxPortalShapePoints, portalPointCount));
}

glm::vec3 SpatialManipulator::GetPortalShapePoint(int index) const
{
    switch (index)
    {
    case 0: return portalShapePoint0;
    case 1: return portalShapePoint1;
    case 2: return portalShapePoint2;
    case 3: return portalShapePoint3;
    case 4: return portalShapePoint4;
    case 5: return portalShapePoint5;
    case 6: return portalShapePoint6;
    case 7: return portalShapePoint7;
    default: return portalShapePoint0;
    }
}

void SpatialManipulator::SetPortalShapePoint(int index, const glm::vec3& value)
{
    switch (index)
    {
    case 0: portalShapePoint0 = value; break;
    case 1: portalShapePoint1 = value; break;
    case 2: portalShapePoint2 = value; break;
    case 3: portalShapePoint3 = value; break;
    case 4: portalShapePoint4 = value; break;
    case 5: portalShapePoint5 = value; break;
    case 6: portalShapePoint6 = value; break;
    case 7: portalShapePoint7 = value; break;
    default: break;
    }
}

std::vector<glm::vec3> SpatialManipulator::GetPortalShapePoints() const
{
    const int count = GetClampedPortalPointCount();
    std::vector<glm::vec3> points;
    points.reserve(static_cast<size_t>(count));
    for (int index = 0; index < count; ++index)
        points.push_back(GetPortalShapePoint(index));
    return points;
}

std::vector<glm::vec3> SpatialManipulator::GetWorldPortalShapePoints() const
{
    const std::vector<glm::vec3> localShape = GetPortalShapePoints();
    if (localShape.empty())
        return {};

    glm::vec3 localTangent(1.f, 0.f, 0.f);
    glm::vec3 localBitangent(0.f, 1.f, 0.f);
    glm::vec3 localNormal(0.f, 0.f, 1.f);
    BuildBasis(portalNormal, localTangent, localBitangent, localNormal);

    glm::mat4 worldMatrix(1.f);
    if (Owner)
        worldMatrix = Owner->transform.GetWorldMatrix();

    std::vector<glm::vec3> worldPoints;
    worldPoints.reserve(localShape.size());
    for (const glm::vec3& shapePoint : localShape)
    {
        const glm::vec3 localPoint = portalPoint +
            localTangent * shapePoint.x +
            localBitangent * shapePoint.y +
            localNormal * shapePoint.z;
        worldPoints.push_back(glm::vec3(worldMatrix * glm::vec4(localPoint, 1.f)));
    }
    return worldPoints;
}

std::vector<glm::vec3> SpatialManipulator::GetRenderWorldPortalShapePoints() const
{
    std::vector<glm::vec3> points = GetWorldPortalShapePoints();
    if (!Owner || !Owner->GetScene())
        return points;

    const Engine::Scene::Scene::SpatialQuery query {
        Engine::Scene::Scene::SpatialQueryDomain::Rendering, Owner };
    for (glm::vec3& point : points)
        point = Owner->GetScene()->MapSpatialPoint(point, query);
    return points;
}

bool SpatialManipulator::HasCompatiblePortalShapeWith(const SpatialManipulator& target) const
{
    return GetClampedPortalPointCount() == target.GetClampedPortalPointCount() &&
        IsValidPortalAperture() && target.IsValidPortalAperture();
}

bool SpatialManipulator::IsValidPortalAperture(float tolerance) const
{
    const std::vector<glm::vec3> points = GetWorldPortalShapePoints();
    if (points.size() < 3)
        return false;

    glm::vec3 tangent(1.f, 0.f, 0.f);
    glm::vec3 bitangent(0.f, 1.f, 0.f);
    glm::vec3 normal(0.f, 0.f, 1.f);
    BuildBasis(portalNormal, tangent, bitangent, normal);
    glm::mat4 ownerWorld(1.f);
    if (Owner)
        ownerWorld = Owner->transform.GetWorldMatrix();
    const glm::mat3 ownerLinear(ownerWorld);
    normal = SafeNormalize(glm::transpose(glm::inverse(ownerLinear)) * normal,
        glm::vec3(0.f, 0.f, 1.f));
    tangent = ownerLinear * tangent;
    tangent -= normal * glm::dot(tangent, normal);
    tangent = SafeNormalize(tangent, glm::vec3(1.f, 0.f, 0.f));
    bitangent = SafeNormalize(glm::cross(normal, tangent), glm::vec3(0.f, 1.f, 0.f));

    const glm::vec3 anchor = Owner
        ? glm::vec3(ownerWorld * glm::vec4(portalPoint, 1.f))
        : portalPoint;
    const float epsilon = std::max(1e-6f, tolerance);
    std::vector<glm::vec2> projected;
    projected.reserve(points.size());
    for (const glm::vec3& point : points)
    {
        const glm::vec3 relative = point - anchor;
        if (std::abs(glm::dot(relative, normal)) > epsilon)
            return false;
        projected.emplace_back(glm::dot(relative, tangent),
            glm::dot(relative, bitangent));
    }

    float winding = 0.f;
    for (size_t index = 0; index < projected.size(); ++index)
    {
        const glm::vec2& a = projected[index];
        const glm::vec2& b = projected[(index + 1u) % projected.size()];
        const glm::vec2& c = projected[(index + 2u) % projected.size()];
        const glm::vec2 edge = b - a;
        if (glm::dot(edge, edge) <= epsilon * epsilon)
            return false;
        const float turn = edge.x * (c.y - b.y) - edge.y * (c.x - b.x);
        if (std::abs(turn) <= epsilon)
            return false;
        if (winding == 0.f)
            winding = turn;
        else if (turn * winding <= 0.f)
            return false;
    }
    return true;
}

bool SpatialManipulator::IsWorldPointInsidePortalAperture(
    const glm::vec3& worldPoint, float margin) const
{
    if (!IsValidPortalAperture())
        return false;

    const glm::mat4 frame = GetPortalWorldFrame();
    const glm::vec3 tangent(frame[0]);
    const glm::vec3 bitangent(frame[1]);
    const glm::vec3 normal(frame[2]);
    const glm::vec3 anchor(frame[3]);
    const glm::vec3 relative = worldPoint - anchor;
    if (std::abs(glm::dot(relative, normal)) > std::max(0.0005f, margin))
        return false;

    const glm::vec2 point(glm::dot(relative, tangent),
        glm::dot(relative, bitangent));
    const std::vector<glm::vec3> worldPoints = GetWorldPortalShapePoints();
    float winding = 0.f;
    for (size_t index = 0; index < worldPoints.size(); ++index)
    {
        const glm::vec3 aWorld = worldPoints[index] - anchor;
        const glm::vec3 bWorld = worldPoints[(index + 1u) % worldPoints.size()] - anchor;
        const glm::vec2 a(glm::dot(aWorld, tangent), glm::dot(aWorld, bitangent));
        const glm::vec2 b(glm::dot(bWorld, tangent), glm::dot(bWorld, bitangent));
        const glm::vec2 edge = b - a;
        const glm::vec2 offset = point - a;
        const float cross = edge.x * offset.y - edge.y * offset.x;
        if (index == 0u)
            winding = cross >= 0.f ? 1.f : -1.f;
        if (winding * cross < -std::max(0.f, margin) * glm::length(edge))
            return false;
    }
    return true;
}

glm::mat4 SpatialManipulator::GetPortalWorldFrame() const
{
    glm::vec3 localTangent(1.f, 0.f, 0.f);
    glm::vec3 localBitangent(0.f, 1.f, 0.f);
    glm::vec3 localNormal(0.f, 0.f, 1.f);
    BuildBasis(portalNormal, localTangent, localBitangent, localNormal);

    glm::mat4 ownerWorld(1.f);
    if (Owner)
        ownerWorld = Owner->transform.GetWorldMatrix();

    const glm::mat3 ownerLinear(ownerWorld);
    const glm::mat3 normalMatrix = glm::transpose(glm::inverse(ownerLinear));
    const glm::vec3 normal = SafeNormalize(normalMatrix * localNormal,
        glm::vec3(0.f, 0.f, 1.f));

    glm::vec3 tangentCandidate = ownerLinear * localTangent;
    tangentCandidate -= normal * glm::dot(tangentCandidate, normal);
    glm::vec3 tangent = SafeNormalize(tangentCandidate, glm::vec3(0.f));
    if (glm::dot(tangent, tangent) <= 1e-8f)
    {
        glm::vec3 fallbackBitangent(0.f, 1.f, 0.f);
        glm::vec3 fallbackNormal(0.f, 0.f, 1.f);
        BuildBasis(normal, tangent, fallbackBitangent, fallbackNormal);
    }
    const glm::vec3 bitangent = SafeNormalize(glm::cross(normal, tangent),
        glm::vec3(0.f, 1.f, 0.f));

    glm::mat4 frame(1.f);
    frame[0] = glm::vec4(tangent, 0.f);
    frame[1] = glm::vec4(bitangent, 0.f);
    frame[2] = glm::vec4(normal, 0.f);
    // The portal point is the single canonical anchor for the aperture,
    // traversal plane, and source-to-target mapping. Shape vertices describe
    // the opening around that anchor; their centroid must not silently move
    // the physical crossing plane.
    frame[3] = glm::vec4(glm::vec3(ownerWorld * glm::vec4(portalPoint, 1.f)), 1.f);
    return frame;
}

glm::mat4 SpatialManipulator::GetRenderPortalWorldFrame() const
{
    const glm::mat4 worldFrame = GetPortalWorldFrame();
    if (!Owner || !Owner->GetScene())
        return worldFrame;

    const Engine::Scene::Scene::SpatialQuery query {
        Engine::Scene::Scene::SpatialQueryDomain::Rendering, Owner };
    const Engine::Scene::Scene::SpatialQuerySample sample =
        Owner->GetScene()->SampleSpatialPoint(glm::vec3(worldFrame[3]), query);
    const glm::mat3& jacobian = sample.jacobian;
    const glm::vec3 rawTangent(worldFrame[0]);
    const glm::vec3 rawNormal(worldFrame[2]);

    glm::vec3 normal = rawNormal;
    const float determinant = glm::determinant(jacobian);
    if (std::isfinite(determinant) && std::abs(determinant) > 1e-7f)
    {
        normal = glm::transpose(glm::inverse(jacobian)) * rawNormal;
    }
    normal = SafeNormalize(normal, rawNormal);

    glm::vec3 tangent = jacobian * rawTangent;
    tangent -= normal * glm::dot(tangent, normal);
    tangent = SafeNormalize(tangent, rawTangent);
    const glm::vec3 bitangent = SafeNormalize(glm::cross(normal, tangent),
        glm::vec3(worldFrame[1]));

    glm::mat4 renderFrame(1.f);
    renderFrame[0] = glm::vec4(tangent, 0.f);
    renderFrame[1] = glm::vec4(bitangent, 0.f);
    renderFrame[2] = glm::vec4(normal, 0.f);
    renderFrame[3] = glm::vec4(sample.point, 1.f);
    return renderFrame;
}

glm::mat4 SpatialManipulator::GetPortalWorldTransformTo(
    const SpatialManipulator& target) const
{
    const glm::mat4 sourceFrame = GetPortalWorldFrame();
    const glm::mat4 targetFrame = target.GetPortalWorldFrame();

    // Crossing a portal reverses portal-local depth and horizontal handedness.
    // Without this half-turn, the virtual camera lands on the wrong side of
    // the target and looks away from the connected scene, so reverse views and
    // recursive source/target reflections disappear. Scale/shear stay excluded.
    glm::mat4 crossing(1.f);
    crossing[0][0] = -1.f;
    crossing[2][2] = -1.f;
    return targetFrame * crossing * glm::inverse(sourceFrame);
}

glm::mat4 SpatialManipulator::GetRenderPortalWorldTransformTo(
    const SpatialManipulator& target) const
{
    const glm::mat4 sourceFrame = GetRenderPortalWorldFrame();
    const glm::mat4 targetFrame = target.GetRenderPortalWorldFrame();
    glm::mat4 crossing(1.f);
    crossing[0][0] = -1.f;
    crossing[2][2] = -1.f;
    return targetFrame * crossing * glm::inverse(sourceFrame);
}

glm::vec3 SpatialManipulator::MapWorldPointThroughPortalShape(const glm::vec3& point,
    const SpatialManipulator& target) const
{
    if (!HasCompatiblePortalShapeWith(target))
        return point;
    return glm::vec3(GetPortalWorldTransformTo(target) * glm::vec4(point, 1.f));
}

glm::vec3 SpatialManipulator::MapRenderWorldPointThroughPortalShape(
    const glm::vec3& point, const SpatialManipulator& target) const
{
    if (!HasCompatiblePortalShapeWith(target))
        return point;
    return glm::vec3(GetRenderPortalWorldTransformTo(target) *
        glm::vec4(point, 1.f));
}

bool SpatialManipulator::EnsurePointCountCompatibility(SpatialManipulator* target)
{
    if (!target)
        return false;

    const int thisCount = GetClampedPortalPointCount();
    const int targetCount = target->GetClampedPortalPointCount();
    if (thisCount == targetCount)
        return true;

    if (!autoMatchPortalPointCount && !target->autoMatchPortalPointCount)
        return false;

    if (thisCount < targetCount)
    {
        const glm::vec3 fill = GetPortalShapePoint(std::max(0, thisCount - 1));
        for (int index = thisCount; index < targetCount; ++index)
            SetPortalShapePoint(index, fill);
        portalPointCount = targetCount;
    }
    else
    {
        const glm::vec3 fill = target->GetPortalShapePoint(std::max(0, targetCount - 1));
        for (int index = targetCount; index < thisCount; ++index)
            target->SetPortalShapePoint(index, fill);
        target->portalPointCount = thisCount;
    }

    return true;
}

RigidBody* SpatialManipulator::ResolveTraversalTriggerBody() const
{
    if (!Owner)
        return nullptr;
    if (traversalTriggerBodyReference.IsAssigned())
        return Engine::Core::ResolveComponentReference<RigidBody>(Owner,
            traversalTriggerBodyReference);
    return Owner->GetComponent<RigidBody>();
}

Mesh* SpatialManipulator::ResolveMeshForObject(Engine::Core::Object* object) const
{
    if (!object)
        return nullptr;
    if (meshReference.IsAssigned())
    {
        if (Mesh* referenced = Engine::Core::ResolveComponentReference<Mesh>(object,
            meshReference))
            return referenced;
    }
    return object->GetComponent<Mesh>();
}

float SpatialManipulator::ComputeSignedDistanceToPortalPlane(
    const glm::vec3& worldPoint) const
{
    if (!Owner)
        return 0.f;
    const glm::mat4 ownerWorld = Owner->transform.GetWorldMatrix();
    const glm::vec3 portalWorldPoint = glm::vec3(ownerWorld *
        glm::vec4(portalPoint, 1.f));
    const glm::mat3 normalMatrix = glm::transpose(glm::inverse(
        glm::mat3(ownerWorld)));
    const glm::vec3 portalWorldNormal = SafeNormalize(normalMatrix *
        SafeNormalize(portalNormal, glm::vec3(0.f, 0.f, 1.f)),
        glm::vec3(0.f, 0.f, 1.f));
    return glm::dot(worldPoint - portalWorldPoint, portalWorldNormal);
}

void SpatialManipulator::ResetTraversalMeshDeformation(RigidBody* traversingBody)
{
    if (!traversingBody)
        return;

    auto it = m_traversalStates.find(traversingBody);
    if (it == m_traversalStates.end())
        return;

    // A split body owns two independent collision pieces. Remove the remote
    // proxy before rebuilding the local dynamic body: the proxy filter holds
    // the native owner pointer and must never observe the replaced body.
    if (Owner && Owner->GetScene())
        Owner->GetScene()->GetPhysics().RemovePortalMeshCollider(&it->second);
    traversingBody->ClearPortalLocalMeshCollider(&it->second);

    it->second.meshDeformed = false;
    it->second.hasCollisionCut = false;
    it->second.postTeleportVisual = false;
    it->second.lastMesh = nullptr;
    it->second.baseVertices.clear();
    it->second.localMeshVertices.clear();
    it->second.remoteMeshVertices.clear();
    it->second.localCollisionVertices.clear();
    it->second.localChartPortal = nullptr;
    it->second.remoteChartPortal = nullptr;
}

void SpatialManipulator::ResetTraversalMeshDeformation()
{
    for (const auto& pair : m_traversalStates)
        ResetTraversalMeshDeformation(const_cast<RigidBody*>(pair.first));
    m_traversalStates.clear();
}

void SpatialManipulator::AppendTraversalRenderInstances(
    std::vector<TraversalRenderInstance>& output) const
{
    for (const auto& pair : m_traversalStates)
    {
        const RigidBody* body = pair.first;
        const TraversalState& state = pair.second;
        if (!body || !body->Owner || !state.meshDeformed || !state.lastMesh)
            continue;

        glm::mat4 localWorld = body->Owner->transform.GetWorldMatrixWithLayer();
        if (state.postTeleportVisual)
        {
            // The physical owner is now in the target chart. Reconstruct its
            // source-chart transform for the trailing half; applying the
            // forward transform below then returns the leading half to the
            // physical target chart.
            localWorld = glm::inverse(state.remoteRenderWorldTransform) *
                localWorld;
        }
        output.push_back({ body->Owner, const_cast<Mesh*>(state.lastMesh),
            localWorld, state.localRenderClipPlane, state.localChartPortal,
            false });
        output.push_back({ body->Owner, const_cast<Mesh*>(state.lastMesh),
            state.remoteRenderWorldTransform * localWorld,
            state.remoteRenderClipPlane, state.remoteChartPortal, true });
    }
}

void SpatialManipulator::ApplyTraversalMeshDeformation(SpatialManipulator* target,
    RigidBody* traversingBody, bool mapPositiveHalf)
{
    if (!deformMeshOnTraversal || !target || !Owner || !target->Owner ||
        !traversingBody || !traversingBody->Owner)
    {
        ResetTraversalMeshDeformation(traversingBody);
        return;
    }
    if (!HasCompatiblePortalShapeWith(*target))
    {
        ResetTraversalMeshDeformation(traversingBody);
        return;
    }

    Mesh* mesh = ResolveMeshForObject(traversingBody->Owner);
    if (!mesh)
    {
        ResetTraversalMeshDeformation(traversingBody);
        return;
    }

    const std::vector<Mesh::Vertex>& currentVertices = mesh->GetVertices();
    if (currentVertices.empty())
    {
        ResetTraversalMeshDeformation(traversingBody);
        return;
    }

    TraversalState& state = m_traversalStates[traversingBody];
    state.localChartPortal = this;
    state.remoteChartPortal = target;
    if (state.lastMesh != mesh)
    {
        if (Owner && Owner->GetScene())
            Owner->GetScene()->GetPhysics().RemovePortalMeshCollider(&state);
        traversingBody->ClearPortalLocalMeshCollider(&state);
        state.lastMesh = mesh;
        state.baseVertices = currentVertices;
        state.meshDeformed = false;
    }
    else if (!state.meshDeformed)
    {
        state.baseVertices = currentVertices;
    }

    const glm::mat4 bodyWorld = traversingBody->Owner->transform.GetWorldMatrix();
    const glm::mat4 bodyWorldInverse = glm::inverse(bodyWorld);
    const glm::mat4 portalWorldTransform = GetPortalWorldTransformTo(*target);

    const glm::mat4 ownerWorld = Owner->transform.GetWorldMatrix();
    const glm::vec3 sourceWorldPortalPoint = glm::vec3(ownerWorld *
        glm::vec4(portalPoint, 1.f));
    const glm::mat3 normalMatrix = glm::transpose(glm::inverse(
        glm::mat3(ownerWorld)));
    const glm::vec3 sourceWorldPortalNormal = SafeNormalize(normalMatrix *
        SafeNormalize(portalNormal, glm::vec3(0.f, 0.f, 1.f)),
        glm::vec3(0.f, 0.f, 1.f));

    const glm::mat3 bodyLinear(bodyWorld);
    const glm::vec3 localPlanePoint = glm::vec3(bodyWorldInverse *
        glm::vec4(sourceWorldPortalPoint, 1.f));
    const glm::vec3 localPlaneNormal = SafeNormalize(
        glm::transpose(bodyLinear) * sourceWorldPortalNormal,
        glm::vec3(0.f, 0.f, 1.f));

    // Render the two halves directly from the original mesh buffer. The
    // fragment shader clips each chart in world space, eliminating the old
    // per-frame combined-mesh upload and preserving independent transforms,
    // sort positions, shadow paths, and portal views.
    const glm::mat4 renderPortalWorldTransform =
        GetRenderPortalWorldTransformTo(*target);
    const glm::mat4 renderSourceFrame = GetRenderPortalWorldFrame();
    const glm::vec3 renderSourcePoint(renderSourceFrame[3]);
    // GetRenderPortalWorldFrame already contains an orthonormal world-space
    // basis.  Applying its inverse-transpose to a *world* +Z vector mixes
    // coordinate spaces and produces an unrelated clip normal whenever a
    // portal is rotated.  That made both chart instances disappear or overlap
    // for rotated portals.  Start with the authored world normal, then map the
    // complete plane through the source-to-target transform below.
    const glm::vec3 renderSourceNormal = SafeNormalize(
        glm::vec3(renderSourceFrame[2]), glm::vec3(0.f, 0.f, 1.f));
    const glm::vec3 renderRemoteNormal = SafeNormalize(
        glm::transpose(glm::inverse(glm::mat3(renderPortalWorldTransform))) *
            renderSourceNormal,
        glm::vec3(0.f, 0.f, 1.f));
    const glm::vec3 localClipNormal = mapPositiveHalf
        ? -renderSourceNormal : renderSourceNormal;
    const glm::vec3 remoteClipNormal = mapPositiveHalf
        ? renderRemoteNormal : -renderRemoteNormal;
    state.remoteRenderWorldTransform = renderPortalWorldTransform;
    state.localRenderClipPlane = glm::vec4(localClipNormal,
        -glm::dot(localClipNormal, renderSourcePoint));
    const glm::vec3 renderRemotePoint = glm::vec3(
        renderPortalWorldTransform * glm::vec4(renderSourcePoint, 1.f));
    state.remoteRenderClipPlane = glm::vec4(remoteClipNormal,
        -glm::dot(remoteClipNormal, renderRemotePoint));
    state.meshDeformed = true;

    // A moving cut normally changes only a small amount between physics ticks.
    // Keep the local convex hull and remote Bvh proxy alive until the cut has
    // moved a meaningful distance, changed direction, or its portal mapping
    // changes. Rendering remains exact every frame because it is GPU-clipped.
    const float cutUpdateDistance = std::max(0.001f,
        portalCollisionCutUpdateDistance);
    const bool planeMoved = glm::length(localPlanePoint -
        state.lastCollisionPlanePoint) >= cutUpdateDistance ||
        glm::dot(localPlaneNormal, state.lastCollisionPlaneNormal) < 0.9995f;
    const bool needsCollisionCut = !state.hasCollisionCut ||
        state.mapPositiveHalf != mapPositiveHalf || planeMoved ||
        !MatricesNearlyEqual(state.collisionRemoteWorldTransform,
            portalWorldTransform);
    if (!needsCollisionCut)
        return;

    auto [positiveHalf, negativeHalf] = Mesh::SliceByPlane(
        state.baseVertices, localPlanePoint, localPlaneNormal);
    if (positiveHalf.empty() || negativeHalf.empty())
    {
        ResetTraversalMeshDeformation(traversingBody);
        return;
    }

    // Express the complete source-to-target mapping in the mesh's local
    // frame. This remains correct when the body or one of its parents has
    // rotation/non-uniform scale, unlike applying a world rotation to normals
    // and then attempting to undo it piecemeal.
    const glm::mat4 localToRemoteLocal = bodyWorldInverse *
        portalWorldTransform * bodyWorld;
    const glm::mat3 remoteNormalMatrix = glm::transpose(glm::inverse(
        glm::mat3(localToRemoteLocal)));

    // Negative-to-positive motion exposes the positive half through the
    // target; positive-to-negative motion exposes the negative half. Keeping
    // this selection directional prevents reverse crossings from stretching
    // the trailing (wrong) side of an asymmetric aperture.
    std::vector<Mesh::Vertex>& remoteHalf = mapPositiveHalf ? positiveHalf : negativeHalf;
    std::vector<Mesh::Vertex>& localHalf = mapPositiveHalf ? negativeHalf : positiveHalf;
    std::vector<glm::vec3> localVertices;
    localVertices.reserve(localHalf.size());
    for (const Mesh::Vertex& vertex : localHalf)
        localVertices.emplace_back(vertex.pos[0], vertex.pos[1], vertex.pos[2]);
    std::vector<glm::vec3> remoteWorldVertices;
    remoteWorldVertices.reserve(remoteHalf.size());
    for (Mesh::Vertex& vertex : remoteHalf)
    {
        const glm::vec3 localPosition(vertex.pos[0], vertex.pos[1], vertex.pos[2]);
        const glm::vec3 mappedLocal = glm::vec3(localToRemoteLocal *
            glm::vec4(localPosition, 1.f));
        vertex.pos[0] = mappedLocal.x;
        vertex.pos[1] = mappedLocal.y;
        vertex.pos[2] = mappedLocal.z;
        remoteWorldVertices.push_back(glm::vec3(bodyWorld *
            glm::vec4(mappedLocal, 1.f)));

        const glm::vec3 localNormal(vertex.normal[0], vertex.normal[1],
            vertex.normal[2]);
        const glm::vec3 mappedLocalNormal = SafeNormalize(
            remoteNormalMatrix * localNormal,
            glm::vec3(0.f, 0.f, 1.f));
        vertex.normal[0] = mappedLocalNormal.x;
        vertex.normal[1] = mappedLocalNormal.y;
        vertex.normal[2] = mappedLocalNormal.z;
    }

    state.localMeshVertices = localHalf;
    state.remoteMeshVertices = remoteHalf;
    state.localCollisionVertices = localVertices;
    state.remoteLinearTransform = glm::mat3(portalWorldTransform);
    state.collisionRemoteWorldTransform = portalWorldTransform;
    state.lastCollisionPlanePoint = localPlanePoint;
    state.lastCollisionPlaneNormal = localPlaneNormal;
    state.hasCollisionCut = true;
    state.mapPositiveHalf = mapPositiveHalf;
    // Keep the dynamic owner restricted to its local clipped triangles. The
    // remote triangles are retained as a separate target-space Bvh instance;
    // neither is rebuilt until the bounded cut update above requires it.
    traversingBody->SetPortalLocalMeshCollider(&state, localVertices);
    if (Owner && Owner->GetScene())
    {
        Owner->GetScene()->GetPhysics().SetPortalMeshCollider(&state,
            *traversingBody, remoteWorldVertices);
    }
}

void SpatialManipulator::MaterializeTraversalMeshSplits(
    SpatialManipulator* /*target*/)
{
    if (!materializeSplitOnDisconnect || !Owner || !Owner->GetScene())
        return;

    Engine::Scene::Scene& scene = *Owner->GetScene();

    for (auto& pair : m_traversalStates)
    {
        RigidBody* localBody = const_cast<RigidBody*>(pair.first);
        TraversalState& state = pair.second;
        if (!localBody || !localBody->Owner || !state.meshDeformed ||
            state.localMeshVertices.empty() || state.remoteMeshVertices.empty() ||
            state.localCollisionVertices.size() < 3u)
        {
            continue;
        }

        Mesh* localMesh = ResolveMeshForObject(localBody->Owner);
        if (!localMesh)
            continue;

        // Keep the local object as the local cut. Its runtime collider is
        // re-keyed to the object so ResetTraversalMeshDeformation cannot
        // restore the old, whole-object collision hull.
        localMesh->SetDeformedVertices(state.localMeshVertices);
        localBody->SetPortalLocalMeshCollider(localBody,
            state.localCollisionVertices);

        Engine::Core::Object* remoteObject = scene.AddObject(
            localBody->Owner->name + " (Portal Fragment)");
        remoteObject->Parent = localBody->Owner->Parent;
        if (remoteObject->Parent)
            remoteObject->Parent->Children.push_back(remoteObject);
        remoteObject->transform.position = localBody->Owner->transform.position;
        remoteObject->transform.rotation = localBody->Owner->transform.rotation;
        remoteObject->transform.scale = localBody->Owner->transform.scale;

        // During traversal the remote half is kept in the source object's
        // local frame so one GPU instance can draw it through the mapping.
        // Once materialized it needs its own target-frame owner transform;
        // convert vertices back to ordinary object-local coordinates first.
        const glm::mat4 localWorld =
            localBody->Owner->transform.GetWorldMatrix();
        const glm::mat4 remoteWorld =
            state.collisionRemoteWorldTransform * localWorld;
        const glm::mat4 sourceToRemoteLocal = glm::inverse(remoteWorld) *
            localWorld;
        const glm::mat3 restoreRemoteNormal = glm::transpose(
            glm::inverse(glm::mat3(sourceToRemoteLocal)));
        std::vector<Mesh::Vertex> remoteLocalVertices =
            state.remoteMeshVertices;
        for (Mesh::Vertex& vertex : remoteLocalVertices)
        {
            const glm::vec3 mappedPosition(vertex.pos[0], vertex.pos[1],
                vertex.pos[2]);
            const glm::vec3 localPosition = glm::vec3(sourceToRemoteLocal *
                glm::vec4(mappedPosition, 1.f));
            vertex.pos[0] = localPosition.x;
            vertex.pos[1] = localPosition.y;
            vertex.pos[2] = localPosition.z;

            const glm::vec3 mappedNormal(vertex.normal[0], vertex.normal[1],
                vertex.normal[2]);
            const glm::vec3 localNormal = SafeNormalize(
                restoreRemoteNormal * mappedNormal, glm::vec3(0.f, 0.f, 1.f));
            vertex.normal[0] = localNormal.x;
            vertex.normal[1] = localNormal.y;
            vertex.normal[2] = localNormal.z;
        }

        Mesh* remoteMesh = remoteObject->AddComponent<Mesh>();
        remoteMesh->InitializeRuntimeCloneFrom(*localMesh);
        remoteMesh->SetDeformedVertices(remoteLocalVertices);
        if (const Material* localMaterial =
                localBody->Owner->GetComponent<Material>())
        {
            Material* remoteMaterial = remoteObject->AddComponent<Material>();
            remoteMaterial->Deserialize(localMaterial->Serialize());
        }
        MeshObjectCollider* remoteCollider =
            remoteObject->AddComponent<MeshObjectCollider>();
        remoteCollider->convex = true;

        RigidBody* remoteBody = remoteObject->AddComponent<RigidBody>();
        remoteBody->bodyType = localBody->bodyType;
        remoteBody->mass = std::max(0.001f, localBody->mass * 0.5f);
        localBody->mass = std::max(0.001f, localBody->mass * 0.5f);
        remoteBody->useGravity = localBody->useGravity;
        remoteBody->gravityScale = localBody->gravityScale;
        remoteBody->linearDamping = localBody->linearDamping;
        remoteBody->angularDamping = localBody->angularDamping;
        remoteBody->friction = localBody->friction;
        remoteBody->restitution = localBody->restitution;
        remoteBody->continuousCollision = localBody->continuousCollision;
        remoteBody->freezePositionX = localBody->freezePositionX;
        remoteBody->freezePositionY = localBody->freezePositionY;
        remoteBody->freezePositionZ = localBody->freezePositionZ;
        remoteBody->freezeRotationX = localBody->freezeRotationX;
        remoteBody->freezeRotationY = localBody->freezeRotationY;
        remoteBody->freezeRotationZ = localBody->freezeRotationZ;
        remoteBody->collisionLayer = localBody->collisionLayer;
        remoteBody->collisionMask = localBody->collisionMask;
        remoteBody->collisionIdentifier = localBody->collisionIdentifier;
        remoteBody->frictionBehaviors = localBody->frictionBehaviors;

        glm::mat3 remoteWorldBasis(remoteWorld);
        for (int column = 0; column < 3; ++column)
            remoteWorldBasis[column] = SafeNormalize(remoteWorldBasis[column],
                glm::vec3(column == 0 ? 1.f : 0.f,
                    column == 1 ? 1.f : 0.f,
                    column == 2 ? 1.f : 0.f));
        const glm::quat remoteWorldRotation = glm::quat_cast(remoteWorldBasis);
        remoteBody->SetWorldPose(glm::vec3(remoteWorld[3]), remoteWorldRotation);
        remoteBody->SetLinearVelocity(state.remoteLinearTransform *
            localBody->GetLinearVelocity());
        remoteBody->SetAngularVelocity(state.remoteLinearTransform *
            localBody->GetAngularVelocity());

        // Subsequent reset removes only the temporary remote proxy and this
        // traversal bookkeeping; it intentionally leaves the two fragments.
        state.meshDeformed = false;
        state.baseVertices.clear();
        state.lastMesh = nullptr;
    }
}

void SpatialManipulator::UpdateTriggerTraversal(SpatialManipulator* target,
    std::unordered_set<const RigidBody*>* claimedBodies)
{
    if (!target || !Owner || !Owner->GetScene() ||
        !HasCompatiblePortalShapeWith(*target))
    {
        ResetTraversalMeshDeformation();
        return;
    }

    RigidBody* triggerBody = ResolveTraversalTriggerBody();
    std::unordered_set<const RigidBody*> activeBodies;
    for (const auto& sceneObject : Owner->GetScene()->GetObjects())
    {
        if (!sceneObject)
            continue;

        RigidBody* body = sceneObject->GetComponent<RigidBody>();
        if (!body || !body->Owner || body->Owner == Owner || body->isTrigger)
            continue;

        activeBodies.insert(body);
        TraversalState& state = m_traversalStates[body];
        const glm::vec3 bodyWorldPosition = body->Owner->transform.GetWorldPosition();
        const float currentSignedDistance = ComputeSignedDistanceToPortalPlane(
            bodyWorldPosition);
        const bool remainsInTrigger = triggerBody && triggerBody->IsOverlapping(body);
        if (!state.hasPreviousWorldPosition)
        {
            state.previousWorldPosition = bodyWorldPosition;
            state.hasPreviousWorldPosition = true;
        }
        const float previousSignedDistance = ComputeSignedDistanceToPortalPlane(
            state.previousWorldPosition);
        // A physical teleport occurs when the body anchor crosses. Continue
        // rendering its source/target chart pair afterwards until the body
        // has cleared the target aperture, otherwise the trailing half pops
        // away exactly when the anchor crosses the plane.
        const float deformationDistance = std::max(0.001f,
            traversalBlendDistance * std::max(0.f, deformationStrength));
        const glm::vec3 planePoint = bodyWorldPosition -
            currentSignedDistance * glm::vec3(GetPortalWorldFrame()[2]);
        if (state.postTeleportVisual)
        {
            const bool remainsInVisualTransition =
                std::abs(currentSignedDistance) <= deformationDistance &&
                IsWorldPointInsidePortalAperture(planePoint, 0.001f);
            if (!remainsInVisualTransition)
                ResetTraversalMeshDeformation(body);
            else
            {
                state.previousWorldPosition = bodyWorldPosition;
                continue;
            }
        }
        // Crossing and re-arm thresholds intentionally differ. A body must
        // first establish a stable side, cross the plane, and then move well
        // clear before another traversal is possible. This suppresses contact
        // jitter and stale trigger-overlap samples after SetWorldPose.
        constexpr float kCrossingDistance = 0.001f;
        constexpr float kRearmDistance = 0.025f;
        using TraversalPhase = TraversalState::Phase;

        bool crossedPortalPlane = false;
        switch (state.phase)
        {
        case TraversalPhase::Uninitialized:
            if (currentSignedDistance <= -kRearmDistance)
                state.phase = TraversalPhase::ArmedNegative;
            else if (currentSignedDistance >= kRearmDistance)
                state.phase = TraversalPhase::ArmedPositive;
            break;
        case TraversalPhase::ArmedNegative:
            crossedPortalPlane = previousSignedDistance <= kCrossingDistance &&
                currentSignedDistance >= kCrossingDistance;
            break;
        case TraversalPhase::ArmedPositive:
            crossedPortalPlane = previousSignedDistance >= -kCrossingDistance &&
                currentSignedDistance <= -kCrossingDistance;
            break;
        case TraversalPhase::Cooldown:
            if (state.waitForOverlapExit && remainsInTrigger)
                break;
            state.waitForOverlapExit = false;
            if (currentSignedDistance <= -kRearmDistance)
                state.phase = TraversalPhase::ArmedNegative;
            else if (currentSignedDistance >= kRearmDistance)
                state.phase = TraversalPhase::ArmedPositive;
            break;
        }

        if (crossedPortalPlane && claimedBodies &&
            claimedBodies->find(body) != claimedBodies->end())
        {
            // A higher-priority aperture already consumed this swept motion.
            // Keep this portal's state current, but do not split or remap it.
            crossedPortalPlane = false;
        }

        if (crossedPortalPlane)
        {
            const float distanceDelta = currentSignedDistance - previousSignedDistance;
            const float crossingT = std::abs(distanceDelta) > 1e-8f
                ? Clamp01(-previousSignedDistance / distanceDelta) : 0.5f;
            const glm::vec3 crossingPoint = glm::mix(
                state.previousWorldPosition, bodyWorldPosition, crossingT);
            // The trigger is merely a broad-phase optimization. The swept
            // crossing must land in the actual logical aperture.
            if (!IsWorldPointInsidePortalAperture(crossingPoint, 0.001f))
                crossedPortalPlane = false;
        }

        if (crossedPortalPlane)
        {
            // A body can cross the anchor plane between two physics ticks,
            // before a prior frame had an opportunity to create its visual
            // split. Create one now so high-speed traversal retains the same
            // continuous hand-off as ordinary motion.
            if (!state.meshDeformed && deformMeshOnTraversal)
                ApplyTraversalMeshDeformation(target, body,
                    currentSignedDistance > previousSignedDistance);
            const TraversalState departingVisual = state;
            const bool retainVisualSplit = departingVisual.meshDeformed &&
                departingVisual.lastMesh;
            ResetTraversalMeshDeformation(body);
            const glm::mat4 portalTransform = GetPortalWorldTransformTo(*target);
            const glm::mat3 portalLinear(portalTransform);
            glm::mat3 portalRotation(1.f);
            for (int column = 0; column < 3; ++column)
                portalRotation[column] = SafeNormalize(portalLinear[column],
                    glm::vec3(column == 0, column == 1, column == 2));

            const glm::mat4 bodyWorld = body->Owner->transform.GetWorldMatrix();
            glm::mat3 bodyWorldRotation(1.f);
            for (int column = 0; column < 3; ++column)
                bodyWorldRotation[column] = SafeNormalize(glm::vec3(bodyWorld[column]),
                    glm::vec3(column == 0, column == 1, column == 2));

            const glm::vec3 mappedPosition = glm::vec3(portalTransform *
                glm::vec4(bodyWorldPosition, 1.f));
            const glm::quat mappedRotation = glm::normalize(glm::quat_cast(
                portalRotation * bodyWorldRotation));
            const glm::vec3 mappedLinearVelocity = portalLinear *
                body->GetLinearVelocity();
            const glm::vec3 mappedAngularVelocity = portalRotation *
                body->GetAngularVelocity();

            body->SetWorldPose(mappedPosition, mappedRotation);
            body->SetLinearVelocity(mappedLinearVelocity);
            body->SetAngularVelocity(mappedAngularVelocity);

            // Both ends suppress this body until it has moved away from the
            // destination plane. This also prevents a reciprocal target from
            // sending it straight back during the same traversal sequence.
            state.phase = TraversalPhase::Cooldown;
            state.waitForOverlapExit = true;
            state.previousWorldPosition = mappedPosition;
            state.hasPreviousWorldPosition = true;
            TraversalState& targetState = target->m_traversalStates[body];
            if (retainVisualSplit)
            {
                // Transfer only GPU visual state. Collision split instances
                // owned by the source were removed above when the physical
                // body was placed in the target chart.
                targetState.lastMesh = departingVisual.lastMesh;
                targetState.remoteRenderWorldTransform =
                    departingVisual.remoteRenderWorldTransform;
                targetState.localRenderClipPlane =
                    departingVisual.localRenderClipPlane;
                targetState.remoteRenderClipPlane =
                    departingVisual.remoteRenderClipPlane;
                targetState.localChartPortal =
                    departingVisual.localChartPortal;
                targetState.remoteChartPortal =
                    departingVisual.remoteChartPortal;
                targetState.meshDeformed = true;
                targetState.hasCollisionCut = false;
                targetState.postTeleportVisual = true;
            }
            targetState.phase = TraversalPhase::Cooldown;
            targetState.waitForOverlapExit = false;
            targetState.previousWorldPosition = mappedPosition;
            targetState.hasPreviousWorldPosition = true;
            if (claimedBodies)
                claimedBodies->insert(body);
        }

        // A destination portal remains overlapped immediately after a
        // teleport. Its cooldown state must never slice the same mesh again;
        // otherwise the two endpoints repeatedly cut each other's generated
        // vertices and the mesh grows without bound.
        // Use the authored traversal window instead of an implicit one-unit
        // threshold. Strength scales the distance at which visual splitting
        // begins while retaining a small finite minimum.
        const bool intersectsAperture =
            std::abs(currentSignedDistance) <= deformationDistance &&
            IsWorldPointInsidePortalAperture(planePoint, 0.001f);
        const float signedMotion = currentSignedDistance - previousSignedDistance;
        const bool isArmed = state.phase == TraversalPhase::ArmedNegative ||
            state.phase == TraversalPhase::ArmedPositive;
        const bool movingTowardPortal =
            (state.phase == TraversalPhase::ArmedNegative && signedMotion > 1e-5f) ||
            (state.phase == TraversalPhase::ArmedPositive && signedMotion < -1e-5f);
        // A portal is a spatial boundary, not a movement-only trigger. A
        // stationary body that already straddles the aperture needs the same
        // two chart instances and local/remote collision pieces as a moving
        // traverser. SliceByPlane below is the exact mesh intersection test;
        // this predicate merely keeps the work scoped to the aperture.
        const bool stationaryAtPortal = std::abs(signedMotion) <= 1e-5f;
        const bool shouldMaintainSplit = movingTowardPortal || stationaryAtPortal;
        const bool claimedByHigherPriorityPortal = claimedBodies &&
            claimedBodies->find(body) != claimedBodies->end();
        if (!crossedPortalPlane && intersectsAperture && isArmed &&
            shouldMaintainSplit && !claimedByHigherPriorityPortal)
        {
            // With no directional sweep, retain the anchor's side locally
            // and map the opposite half. This makes stationary cuts stable
            // rather than choosing a side based on floating-point noise.
            const bool mapPositiveHalf = movingTowardPortal
                ? signedMotion > 0.f
                : currentSignedDistance <= 0.f;
            ApplyTraversalMeshDeformation(target, body, mapPositiveHalf);
            // Mesh deformation is an exclusive, frame-local view of a body.
            // Without this claim two coincident apertures can each cache the
            // other's already split vertices, leaving the mesh stretched when
            // either side later restores its stale snapshot.
            if (claimedBodies)
                claimedBodies->insert(body);
        }
        else if (!intersectsAperture || !shouldMaintainSplit)
            ResetTraversalMeshDeformation(body);

        if (!crossedPortalPlane)
            state.previousWorldPosition = bodyWorldPosition;
    }

    for (auto it = m_traversalStates.begin(); it != m_traversalStates.end();)
    {
        if (activeBodies.find(it->first) == activeBodies.end())
        {
            ResetTraversalMeshDeformation(const_cast<RigidBody*>(it->first));
            it = m_traversalStates.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

void SpatialManipulator::ApplyToOwner()
{
    if (!Owner)
        return;

    auto& layer = Owner->transform.matrixLayer;
    layer.enabled = enabled;
    layer.SetLocalToLayer(GetOverlayMatrix());
    layer.portalPoint = portalPoint;
    layer.portalNormal = glm::normalize(portalNormal);

    if (connectionMode == static_cast<int>(ConnectionMode::Portal) ||
        connectionMode == static_cast<int>(ConnectionMode::LinkedPortal))
    {
        layer.connection.boundaryPoint = portalPoint;
        layer.connection.boundaryNormal = glm::normalize(portalNormal);
    }
    else
    {
        layer.connection.enabled = false;
    }
}

void SpatialManipulator::ClearSpatialWarpState(SpatialManipulator* target)
{
    MaterializeTraversalMeshSplits(target);
    ResetTraversalMeshDeformation();
    ClearOwnedMatrixOverlayState();

    if (Owner && Owner->GetScene())
        Owner->GetScene()->GetPhysics().RemovePortalApertureCollider(this);

    // Portal state is endpoint-local. Scoped overlays are owned separately,
    // so do not erase an unrelated higher-priority overlay on either root.
    if (Owner && !Owner->transform.matrixLayer.overlayOwner)
        Owner->transform.matrixLayer = MatrixLayer {};

    // Connection setup writes reciprocal warp state onto the target transform,
    // so disabling either endpoint must remove that state as well.
    if (target)
    {
        target->MaterializeTraversalMeshSplits(this);
        target->ResetTraversalMeshDeformation();
        target->ClearOwnedMatrixOverlayState();
        if (target->Owner && target->Owner->GetScene())
            target->Owner->GetScene()->GetPhysics().RemovePortalApertureCollider(target);
        if (target->Owner && !target->Owner->transform.matrixLayer.overlayOwner)
            target->Owner->transform.matrixLayer = MatrixLayer {};
    }
}

SpatialManipulator* SpatialManipulator::FindReciprocalManipulator() const
{
    if (!Owner || !Owner->GetScene())
        return nullptr;

    for (const auto& object : Owner->GetScene()->GetObjects())
    {
        if (!object)
            continue;
        for (Engine::Core::Component* component : object->Components)
        {
            auto* candidate = dynamic_cast<SpatialManipulator*>(component);
            if (candidate && candidate != this && candidate->ResolveTarget() == this)
                return candidate;
        }
    }
    return nullptr;
}

void SpatialManipulator::ConnectToTarget(SpatialManipulator* target)
{
    if (!target || target == this)
        return;

    if (ResolveTarget() != target || target->ResolveTarget() != this)
    {
        Disconnect();
        target->Disconnect();
    }

    EnsurePointCountCompatibility(target);
    targetManipulator = Engine::Core::CaptureComponentReference(target, "SpatialManipulator");
    target->targetManipulator = Engine::Core::CaptureComponentReference(
        this, "SpatialManipulator");
}

void SpatialManipulator::Disconnect()
{
    SpatialManipulator* target = ResolveTarget();
    if (!target)
        target = FindReciprocalManipulator();

    targetManipulator.Clear();
    ClearSpatialWarpState(target);

    if (target)
    {
        if (target->ResolveTarget() == this)
            target->targetManipulator.Clear();
        target->ResetTraversalMeshDeformation();
    }
}

void SpatialManipulator::Disabled()
{
    SpatialManipulator* target = ResolveTarget();
    if (!target)
        target = FindReciprocalManipulator();
    ClearSpatialWarpState(target);
}

void SpatialManipulator::OnDestroy()
{
    Disconnect();
}

bool SpatialManipulator::HasTarget() const
{
    return targetManipulator.IsAssigned();
}

SpatialManipulator* SpatialManipulator::ResolveTarget() const
{
    if (!Owner || !HasTarget())
        return nullptr;
    return Engine::Core::ResolveComponentReference<SpatialManipulator>(Owner, targetManipulator);
}

void SpatialManipulator::ConnectPortalPoints(const glm::vec3& localPoint,
    const glm::vec3& localNormal,
    SpatialManipulator* other)
{
    if (!other)
        return;

    portalPoint = localPoint;
    portalNormal = localNormal;
    other->portalPoint = localPoint;
    other->portalNormal = localNormal;

    if (Owner && other->Owner)
    {
        Owner->transform.matrixLayer.connection.boundaryPoint = localPoint;
        Owner->transform.matrixLayer.connection.boundaryNormal = glm::normalize(localNormal);
        other->Owner->transform.matrixLayer.connection.boundaryPoint = localPoint;
        other->Owner->transform.matrixLayer.connection.boundaryNormal = glm::normalize(localNormal);
    }
}

void SpatialManipulator::ApplyPortalConnection(SpatialManipulator* target)
{
    if (!target || !Owner || !target->Owner)
        return;

    EnsurePointCountCompatibility(target);
    if (!HasCompatiblePortalShapeWith(*target))
        return;

    // Older serialized scenes may only store the source reference. Repair the
    // harmless missing reciprocal link so both visible apertures render as
    // portals and either endpoint can tear the connection down safely.
    if (!target->HasTarget())
        target->targetManipulator = Engine::Core::CaptureComponentReference(
            this, "SpatialManipulator");

    const glm::mat4 sourceToTarget = GetPortalWorldTransformTo(*target);
    const glm::mat4 targetToSource = glm::inverse(sourceToTarget);
    const glm::mat4 sourceFrame = GetPortalWorldFrame();
    const glm::mat4 targetFrame = target->GetPortalWorldFrame();

    Owner->transform.matrixLayer.connection.enabled = true;
    Owner->transform.matrixLayer.connection.boundaryPoint = glm::vec3(sourceFrame[3]);
    Owner->transform.matrixLayer.connection.boundaryNormal = glm::vec3(sourceFrame[2]);
    Owner->transform.matrixLayer.connection.localToRemote = sourceToTarget;
    Owner->transform.matrixLayer.connection.remoteToLocal = targetToSource;

    target->Owner->transform.matrixLayer.connection.enabled = true;
    target->Owner->transform.matrixLayer.connection.boundaryPoint = glm::vec3(targetFrame[3]);
    target->Owner->transform.matrixLayer.connection.boundaryNormal = glm::vec3(targetFrame[2]);
    target->Owner->transform.matrixLayer.connection.localToRemote = targetToSource;
    target->Owner->transform.matrixLayer.connection.remoteToLocal = sourceToTarget;

    // Each aperture contributes only a solid rim, never a visual or physical
    // centre face. Bullet then resolves an oversized split body against the
    // actual polygon boundary before post-physics traversal evaluates it.
    Owner->GetScene()->GetPhysics().SetPortalApertureCollider(this,
        GetWorldPortalShapePoints(), glm::vec3(sourceFrame[2]),
        portalEdgeHalfWidth, portalEdgeHalfDepth);
    target->Owner->GetScene()->GetPhysics().SetPortalApertureCollider(target,
        target->GetWorldPortalShapePoints(), glm::vec3(targetFrame[2]),
        target->portalEdgeHalfWidth, target->portalEdgeHalfDepth);
}

void SpatialManipulator::ApplyMatrixConnection(SpatialManipulator* target)
{
    if (!target || !Owner || !target->Owner)
        return;
    if (!IsMatrixOverlayAuthority(target))
        return;

    ClearOwnedMatrixOverlayState();
    const glm::mat4 sourceToTarget = target->GetOverlayMatrix() *
        glm::inverse(GetOverlayMatrix());
    const glm::mat4 targetToSource = glm::inverse(sourceToTarget);
    const int priority = std::max(matrixOverlayPriority,
        target->matrixOverlayPriority);
    const std::string ownerKey = GetStableSceneKey();

    const auto applyScope = [&](const SpatialManipulator& endpoint,
        const glm::mat4& transform)
    {
        for (Engine::Core::Object* object : endpoint.ResolveMatrixOverlayScope())
        {
            if (!object)
                continue;
            MatrixLayer& layer = object->transform.matrixLayer;
            const bool replace = layer.overlayOwner == this ||
                !layer.enabled || !layer.overlayOwner ||
                priority > layer.overlayPriority ||
                (priority == layer.overlayPriority &&
                    ownerKey < layer.overlayOwnerKey);
            if (!replace)
                continue;
            layer = MatrixLayer {};
            layer.SetLocalToLayer(transform);
            layer.overlayOwner = this;
            layer.overlayPriority = priority;
            layer.overlayOwnerKey = ownerKey;
            m_matrixOverlayObjects.push_back(object);
        }
    };

    // Source and target scopes are mapped by inverse relative transforms.
    // The link itself owns this state; carrier meshes are never special.
    applyScope(*this, sourceToTarget);
    applyScope(*target, targetToSource);
}

void SpatialManipulator::ClearOwnedMatrixOverlayState()
{
    for (Engine::Core::Object* object : m_matrixOverlayObjects)
    {
        if (object && object->transform.matrixLayer.overlayOwner == this)
            object->transform.matrixLayer = MatrixLayer {};
    }
    m_matrixOverlayObjects.clear();
}

std::vector<Engine::Core::Object*>
SpatialManipulator::ResolveMatrixOverlayScope() const
{
    std::vector<Engine::Core::Object*> objects;
    if (!Owner)
        return objects;
    Engine::Core::Object* root = Owner;
    if (matrixOverlayScopeRoot.IsAssigned())
    {
        Transform* transform = Engine::Core::ResolveComponentReference<Transform>(
            Owner, matrixOverlayScopeRoot);
        if (!transform || !transform->Owner)
            return objects;
        root = transform->Owner;
    }
    if (matrixOverlayIncludeChildren)
    {
        VisitObjectTree(root, [&](Engine::Core::Object* object)
        {
            objects.push_back(object);
        });
    }
    else
    {
        objects.push_back(root);
    }
    return objects;
}

bool SpatialManipulator::IsMatrixOverlayAuthority(
    const SpatialManipulator* target) const
{
    if (!target)
        return false;
    return GetStableSceneKey() < target->GetStableSceneKey();
}

std::string SpatialManipulator::GetStableSceneKey() const
{
    if (!Owner || !Owner->GetScene())
        return "~";
    Engine::Scene::Scene::ObjectPath path;
    if (!Owner->GetScene()->TryGetObjectPath(Owner, path))
        return "~";
    std::string key;
    for (const size_t index : path)
        key += std::to_string(index) + "/";
    return key;
}

void SpatialManipulator::Update()
{
    if (!enabled)
    {
        ClearSpatialWarpState(ResolveTarget());
        return;
    }

    if (definesWarpVolume)
    {
        ResetTraversalMeshDeformation();
        UpdateWarpVolumeTraversalScale();
        if (Owner)
            Owner->transform.matrixLayer = MatrixLayer {};
        return;
    }

    const auto mode = static_cast<ConnectionMode>(connectionMode);
    if (mode == ConnectionMode::MatrixOverlay)
    {
        SpatialManipulator* target = ResolveTarget();
        if (!target || !target->enabled)
        {
            ClearOwnedMatrixOverlayState();
            if (!target)
                ApplyToOwner();
            else
                ClearSpatialWarpState(target);
            return;
        }
        ApplyMatrixConnection(target);
        return;
    }

    ClearOwnedMatrixOverlayState();

    ApplyToOwner();

    if (SpatialManipulator* target = ResolveTarget())
    {
        if (!target->enabled)
        {
            ClearSpatialWarpState(target);
            return;
        }

        switch (mode)
        {
        case ConnectionMode::Portal:
        case ConnectionMode::LinkedPortal:
            ApplyPortalConnection(target);
            break;
        case ConnectionMode::None:
        default:
            ClearSpatialWarpState(target);
            break;
        }
    }
    else
    {
        ClearSpatialWarpState();
    }
}

void SpatialManipulator::UpdateWarpVolumeTraversalScale()
{
    if (!applyTraversalScale || !Owner || !Owner->GetScene())
    {
        m_warpVolumeTraversalStates.clear();
        return;
    }

    Engine::Scene::Scene& scene = *Owner->GetScene();
    const glm::mat4 volumeWorld = Owner->transform.GetWorldMatrix();
    const glm::mat4 inverseVolumeWorld = glm::inverse(volumeWorld);
    const glm::vec3 localXAxis = SafeNormalize(
        glm::vec3(volumeWorld[0]), glm::vec3(1.f, 0.f, 0.f));
    const glm::vec3 localYAxis = SafeNormalize(
        glm::vec3(volumeWorld[1]), glm::vec3(0.f, 1.f, 0.f));
    std::unordered_set<const Engine::Core::Object*> liveObjects;

    for (const std::unique_ptr<Engine::Core::Object>& root : scene.GetObjects())
    {
        VisitObjectTree(root.get(), [&](Engine::Core::Object* object)
        {
            if (!object || object == Owner || !object->enabled ||
                !object->GetComponent<RigidBody>())
                return;

            liveObjects.insert(object);
            const glm::vec3 worldPosition = object->transform.GetWorldPosition();
            const glm::vec3 localPosition = glm::vec3(inverseVolumeWorld *
                glm::vec4(worldPosition, 1.f));
            const bool inside = ContainsWorldPoint(worldPosition);
            WarpVolumeTraversalState& state = m_warpVolumeTraversalStates[object];

            if (!state.hasPreviousPosition)
            {
                state.authoredScale = object->transform.scale;
                state.persistedScale = state.authoredScale;
                state.previousLocalPosition = localPosition;
                state.hasPreviousPosition = true;
                return;
            }

            if (!state.completed && !state.active && inside)
            {
                state.active = true;
                state.enteredFromNegativeZ = localPosition.z >=
                    state.previousLocalPosition.z;
            }

            if (!state.completed && state.active && inside &&
                state.enteredFromNegativeZ)
            {
                const Engine::Scene::Scene::SpatialQuerySample sample =
                    scene.SampleSpatialPoint(worldPosition,
                        { Engine::Scene::Scene::SpatialQueryDomain::Gameplay, object });
                const float xScale = glm::length(sample.jacobian * localXAxis);
                const float yScale = glm::length(sample.jacobian * localYAxis);
                const float transverseScale = std::sqrt(std::max(0.f, xScale * yScale));

                if (std::isfinite(transverseScale) && transverseScale > 1e-5f)
                {
                    state.persistedScale = state.authoredScale / transverseScale;
                    object->transform.scale = state.persistedScale;
                }
            }
            else if (!state.completed && state.active && !inside)
            {
                if (!state.enteredFromNegativeZ || !persistTraversalScaleOnExit)
                    object->transform.scale = state.authoredScale;
                else
                {
                    // Sample the positive boundary rather than retaining the
                    // last simulation tick inside it. That makes the carried
                    // scale independent of physics frame rate.
                    glm::vec3 exitLocalPosition = localPosition;
                    if (static_cast<WarpVolumeShape>(warpVolumeShape) ==
                        WarpVolumeShape::Box)
                    {
                        exitLocalPosition.z = std::abs(warpVolumeSize.z) * 0.5f;
                    }
                    const glm::vec3 exitWorldPosition = glm::vec3(volumeWorld *
                        glm::vec4(exitLocalPosition, 1.f));
                    const Engine::Scene::Scene::SpatialQuerySample exitSample =
                        scene.SampleSpatialPoint(exitWorldPosition,
                            { Engine::Scene::Scene::SpatialQueryDomain::Gameplay, object });
                    const float exitXScale = glm::length(exitSample.jacobian * localXAxis);
                    const float exitYScale = glm::length(exitSample.jacobian * localYAxis);
                    const float exitTransverseScale = std::sqrt(std::max(0.f,
                        exitXScale * exitYScale));
                    if (std::isfinite(exitTransverseScale) && exitTransverseScale > 1e-5f)
                        state.persistedScale = state.authoredScale / exitTransverseScale;
                    object->transform.scale = state.persistedScale;
                }

                state.active = false;
                state.completed = true;
            }

            state.previousLocalPosition = localPosition;
        });
    }

    for (auto it = m_warpVolumeTraversalStates.begin();
        it != m_warpVolumeTraversalStates.end();)
    {
        if (liveObjects.find(it->first) == liveObjects.end())
            it = m_warpVolumeTraversalStates.erase(it);
        else
            ++it;
    }
}

void SpatialManipulator::PostPhysicsUpdate(
    std::unordered_set<const RigidBody*>* claimedBodies)
{
    if (!enabled || definesWarpVolume)
    {
        ResetTraversalMeshDeformation();
        return;
    }

    const auto mode = static_cast<ConnectionMode>(connectionMode);
    if (mode != ConnectionMode::Portal && mode != ConnectionMode::LinkedPortal)
    {
        ResetTraversalMeshDeformation();
        return;
    }

    SpatialManipulator* target = ResolveTarget();
    if (!target || !target->enabled)
    {
        ResetTraversalMeshDeformation();
        return;
    }
    UpdateTriggerTraversal(target, claimedBodies);
}

bool SpatialManipulator::DrawProperties(::Engine::Editor::IEditorUi& ui)
{
    bool changed = false;

    changed = ui.Checkbox("Enabled", &enabled) || changed;
    changed = ui.DragFloat3("Position", &position.x, 0.05f) || changed;
    changed = ui.DragFloat3("Rotation", &rotation.x, 0.05f) || changed;
    changed = ui.DragFloat3("Scale", &scale.x, 0.05f, 0.01f, 10.f) || changed;

    static const char* modes[] = { "None", "Matrix Overlay", "Portal", "Linked Portal" };
    int mode = connectionMode;
    if (ui.Combo("Connection Mode", &mode, modes, 4))
    {
        connectionMode = mode;
        changed = true;
    }
    if (static_cast<ConnectionMode>(connectionMode) ==
        ConnectionMode::MatrixOverlay)
    {
        changed = ui.Checkbox("Overlay Scope Includes Children",
            &matrixOverlayIncludeChildren) || changed;
        float overlayPriority = static_cast<float>(matrixOverlayPriority);
        if (ui.DragFloat("Overlay Priority", &overlayPriority, 1.f,
            -100000.f, 100000.f))
        {
            matrixOverlayPriority = static_cast<int>(std::round(overlayPriority));
            changed = true;
        }
        const char* scopeLabel = matrixOverlayScopeRoot.IsAssigned()
            ? matrixOverlayScopeRoot.objectName.c_str()
            : "Owner (default)";
        ui.ValueLabel("Overlay Scope Root", scopeLabel);
        if (ui.BeginDragDropTarget())
        {
            size_t payloadSize = 0;
            const void* payload = ui.AcceptDragDropPayload(
                "ENGINE_COMPONENT_REORDER", &payloadSize);
            if (payload && payloadSize == sizeof(Engine::Core::Component*))
            {
                auto* component = *static_cast<Engine::Core::Component* const*>(
                    payload);
                if (auto* transform = dynamic_cast<Transform*>(component))
                {
                    matrixOverlayScopeRoot =
                        Engine::Core::CaptureComponentReference(transform,
                            "Transform");
                    changed = true;
                }
            }
            ui.EndDragDropTarget();
        }
        if (matrixOverlayScopeRoot.IsAssigned())
        {
            ui.SameLine();
            if (ui.Button("Clear Overlay Scope"))
            {
                matrixOverlayScopeRoot.Clear();
                changed = true;
            }
        }
    }

    changed = ui.Checkbox("Defines Warp Volume", &definesWarpVolume) || changed;
    if (definesWarpVolume)
    {
        float priority = static_cast<float>(warpPriority);
        if (ui.DragFloat("Warp Priority", &priority, 1.f, -100000.f, 100000.f))
        {
            warpPriority = static_cast<int>(std::round(priority));
            changed = true;
        }
        static const char* volumeShapes[] = { "Infinite", "Box", "Sphere" };
        changed = ui.Combo("Warp Volume Shape", &warpVolumeShape,
            volumeShapes, 3) || changed;
        if (static_cast<WarpVolumeShape>(warpVolumeShape) == WarpVolumeShape::Box)
            changed = ui.DragFloat3("Warp Volume Size", &warpVolumeSize.x,
                0.1f, 0.001f, 100000.f) || changed;
        else if (static_cast<WarpVolumeShape>(warpVolumeShape) == WarpVolumeShape::Sphere)
            changed = ui.DragFloat("Warp Volume Radius", &warpVolumeRadius,
                0.1f, 0.001f, 100000.f) || changed;
        if (static_cast<WarpVolumeShape>(warpVolumeShape) != WarpVolumeShape::Infinite)
            changed = ui.DragFloat("Warp Boundary Falloff", &warpBoundaryFalloff,
                0.05f, 0.f, 100000.f) || changed;

        changed = ui.Checkbox("Apply Traversal Scale", &applyTraversalScale) || changed;
        if (applyTraversalScale)
        {
            changed = ui.Checkbox("Persist Scale On Positive-Z Exit",
                &persistTraversalScaleOnExit) || changed;
            ui.DisabledLabel("Dynamic bodies use the warp Jacobian's local X/Y scale.");
        }

        static const char* warpTypes[] = { "Affine", "Spiral", "Formula" };
        changed = ui.Combo("Space Warp Type", &spaceWarpType,
            warpTypes, 3) || changed;
        if (static_cast<SpaceWarpType>(spaceWarpType) == SpaceWarpType::Spiral)
        {
            changed = ui.DragFloat3("Spiral Axis", &spiralAxis.x, 0.05f) || changed;
            changed = ui.DragFloat("Spiral Radians Per Unit",
                &spiralRadiansPerUnit, 0.01f, -100.f, 100.f) || changed;
        }
        else if (static_cast<SpaceWarpType>(spaceWarpType) == SpaceWarpType::Formula)
        {
            const auto editFormula = [&](const char* label, std::string& formula)
            {
                char buffer[512] = {};
                std::snprintf(buffer, sizeof(buffer), "%s", formula.c_str());
                if (!ui.InputText(label, buffer, sizeof(buffer))) return false;
                formula = buffer;
                return true;
            };
            changed = editFormula("Formula X", formulaX) || changed;
            changed = editFormula("Formula Y", formulaY) || changed;
            changed = editFormula("Formula Z", formulaZ) || changed;
            changed = ui.DragFloat("Formula a", &formulaA, 0.01f) || changed;
            changed = ui.DragFloat("Formula b", &formulaB, 0.01f) || changed;
            changed = ui.DragFloat("Formula c", &formulaC, 0.01f) || changed;
            changed = ui.DragFloat("Formula d", &formulaD, 0.01f) || changed;
            ui.DisabledLabel("Variables: x y z a b c d r rho theta phi pi e");
            ui.DisabledLabel("Functions: sin cos tan sqrt abs exp log min max pow atan2 clamp mix");
        }
    }

    changed = ui.DragFloat3("Portal Point", &portalPoint.x, 0.05f) || changed;
    changed = ui.DragFloat3("Portal Normal", &portalNormal.x, 0.05f) || changed;

    float pointCount = static_cast<float>(portalPointCount);
    if (ui.DragFloat("Portal Point Count", &pointCount, 1.f,
        3.f, static_cast<float>(kMaxPortalShapePoints)))
    {
        portalPointCount = std::max(3, std::min(kMaxPortalShapePoints,
            static_cast<int>(std::round(pointCount))));
        changed = true;
    }

    for (int index = 0; index < GetClampedPortalPointCount(); ++index)
    {
        glm::vec3 shapePoint = GetPortalShapePoint(index);
        char label[64] = {};
        std::snprintf(label, sizeof(label), "Portal Shape Point %d", index);
        if (ui.DragFloat3(label, &shapePoint.x, 0.05f))
        {
            SetPortalShapePoint(index, shapePoint);
            changed = true;
        }
    }

    changed = ui.Checkbox("Auto Match Portal Point Count", &autoMatchPortalPointCount) || changed;
    changed = ui.Checkbox("Split Mesh While Crossing", &deformMeshOnTraversal) || changed;
    changed = ui.DragFloat("Portal Collision Cut Update Distance",
        &portalCollisionCutUpdateDistance, 0.005f, 0.001f, 1.f) || changed;
    float traversalPriority = static_cast<float>(portalTraversalPriority);
    if (ui.DragFloat("Portal Traversal Priority", &traversalPriority,
        1.f, -100000.f, 100000.f))
    {
        portalTraversalPriority = static_cast<int>(std::round(traversalPriority));
        changed = true;
    }

    return changed;
}
}

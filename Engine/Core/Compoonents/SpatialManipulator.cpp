#include "SpatialManipulator.h"
#include "Core/Math/FormulaExpression.h"
#include "Core/Object.h"
#include "Core/Scene/Scene.h"
#include "Engine/Editor/UI/IEditorUi.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
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

}

SpatialManipulator::SpatialManipulator()
{
    SetTypeName(COMPONENT_TYPE_NAME(SpatialManipulator));
    RegisterField("enabled", enabled);
    RegisterField("position", position);
    RegisterField("rotation", rotation);
    RegisterField("scale", scale);
    RegisterField("connectionMode", connectionMode);
    RegisterField("definesWarpVolume", definesWarpVolume);
    RegisterField("warpVolumeShape", warpVolumeShape);
    RegisterField("warpVolumeSize", warpVolumeSize);
    RegisterField("warpVolumeRadius", warpVolumeRadius);
    RegisterField("warpBoundaryFalloff", warpBoundaryFalloff);
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

glm::mat4 SpatialManipulator::GetPortalWorldTransformTo(
    const SpatialManipulator& target) const
{
    const glm::mat4 sourceFrame = GetPortalWorldFrame();
    const glm::mat4 targetFrame = target.GetPortalWorldFrame();

    // A normal portal is a rigid half-turn in portal-frame coordinates.
    // Aperture shape controls only the visible opening; it must not silently
    // scale or shear the connected space. Explicit scale warps belong to
    // spatial volumes or matrix overlays.
    glm::mat4 crossing(1.f);
    crossing[0][0] = -1.f;
    crossing[1][1] = 1.f;
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

void SpatialManipulator::ResetTraversalMeshDeformation(const RigidBody* traversingBody)
{
    if (!traversingBody)
        return;

    auto it = m_traversalStates.find(traversingBody);
    if (it == m_traversalStates.end())
        return;

    if (traversingBody->Owner && it->second.meshDeformed)
    {
        if (Mesh* mesh = ResolveMeshForObject(traversingBody->Owner))
            if (!it->second.baseVertices.empty())
                mesh->SetDeformedVertices(it->second.baseVertices);
    }

    it->second.meshDeformed = false;
    it->second.lastMesh = nullptr;
    it->second.baseVertices.clear();
}

void SpatialManipulator::ResetTraversalMeshDeformation()
{
    for (const auto& pair : m_traversalStates)
        ResetTraversalMeshDeformation(pair.first);
    m_traversalStates.clear();
}

void SpatialManipulator::ApplyTraversalMeshDeformation(SpatialManipulator* target,
    RigidBody* traversingBody)
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
    if (state.lastMesh != mesh)
    {
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
    auto [positiveHalf, negativeHalf] = Mesh::SliceByPlane(
        state.baseVertices, localPlanePoint, localPlaneNormal);
    if (positiveHalf.empty() || negativeHalf.empty())
    {
        ResetTraversalMeshDeformation(traversingBody);
        return;
    }

    const glm::mat4 portalTransform = GetPortalWorldTransformTo(*target);
    const glm::mat3 portalRotation(portalTransform);
    const glm::mat3 worldNormalMatrix = glm::transpose(glm::inverse(bodyLinear));
    for (Mesh::Vertex& vertex : positiveHalf)
    {
        const glm::vec3 localPosition(vertex.pos[0], vertex.pos[1], vertex.pos[2]);
        const glm::vec3 mappedWorld = glm::vec3(portalTransform *
            bodyWorld * glm::vec4(localPosition, 1.f));
        const glm::vec3 mappedLocal = glm::vec3(bodyWorldInverse *
            glm::vec4(mappedWorld, 1.f));
        vertex.pos[0] = mappedLocal.x;
        vertex.pos[1] = mappedLocal.y;
        vertex.pos[2] = mappedLocal.z;

        const glm::vec3 localNormal(vertex.normal[0], vertex.normal[1],
            vertex.normal[2]);
        const glm::vec3 mappedWorldNormal = SafeNormalize(portalRotation *
            (worldNormalMatrix * localNormal), glm::vec3(0.f, 0.f, 1.f));
        const glm::vec3 mappedLocalNormal = SafeNormalize(
            glm::transpose(bodyLinear) * mappedWorldNormal,
            glm::vec3(0.f, 0.f, 1.f));
        vertex.normal[0] = mappedLocalNormal.x;
        vertex.normal[1] = mappedLocalNormal.y;
        vertex.normal[2] = mappedLocalNormal.z;
    }

    negativeHalf.insert(negativeHalf.end(), positiveHalf.begin(),
        positiveHalf.end());
    if (mesh->SetDeformedVertices(negativeHalf))
        state.meshDeformed = true;
}

void SpatialManipulator::UpdateTriggerTraversal(SpatialManipulator* target)
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
            targetState.phase = TraversalPhase::Cooldown;
            targetState.waitForOverlapExit = false;
            targetState.previousWorldPosition = mappedPosition;
            targetState.hasPreviousWorldPosition = true;
        }

        // A destination portal remains overlapped immediately after a
        // teleport. Its cooldown state must never slice the same mesh again;
        // otherwise the two endpoints repeatedly cut each other's generated
        // vertices and the mesh grows without bound.
        const glm::vec3 planePoint = bodyWorldPosition -
            currentSignedDistance * glm::vec3(GetPortalWorldFrame()[2]);
        const bool intersectsAperture = std::abs(currentSignedDistance) <= 1.f &&
            IsWorldPointInsidePortalAperture(planePoint, 0.001f);
        if (!crossedPortalPlane && intersectsAperture &&
            (state.phase == TraversalPhase::ArmedNegative ||
             state.phase == TraversalPhase::ArmedPositive))
            ApplyTraversalMeshDeformation(target, body);
        else if (!intersectsAperture)
            ResetTraversalMeshDeformation(body);

        if (!crossedPortalPlane)
            state.previousWorldPosition = bodyWorldPosition;
    }

    for (auto it = m_traversalStates.begin(); it != m_traversalStates.end();)
    {
        if (activeBodies.find(it->first) == activeBodies.end())
        {
            ResetTraversalMeshDeformation(it->first);
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
    ResetTraversalMeshDeformation();

    if (Owner)
        Owner->transform.matrixLayer = MatrixLayer {};

    // Connection setup writes reciprocal warp state onto the target transform,
    // so disabling either endpoint must remove that state as well.
    if (target && target->Owner)
        target->Owner->transform.matrixLayer = MatrixLayer {};
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
}

void SpatialManipulator::ApplyMatrixConnection(SpatialManipulator* target)
{
    if (!target || !Owner || !target->Owner)
        return;

    Owner->transform.matrixLayer.connection = MatrixLayerConnection {};
    target->Owner->transform.matrixLayer.connection = MatrixLayerConnection {};

    const glm::mat4 targetMatrix = target->GetOverlayMatrix();
    Owner->transform.matrixLayer.enabled = enabled;
    Owner->transform.matrixLayer.localToLayer = targetMatrix;
    Owner->transform.matrixLayer.layerToLocal = glm::inverse(targetMatrix);

    target->Owner->transform.matrixLayer.enabled = target->enabled;
    target->Owner->transform.matrixLayer.localToLayer = GetOverlayMatrix();
    target->Owner->transform.matrixLayer.layerToLocal = glm::inverse(GetOverlayMatrix());
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
        if (Owner)
            Owner->transform.matrixLayer = MatrixLayer {};
        return;
    }

    ApplyToOwner();

    if (SpatialManipulator* target = ResolveTarget())
    {
        if (!target->enabled)
        {
            ClearSpatialWarpState(target);
            return;
        }

        switch (static_cast<ConnectionMode>(connectionMode))
        {
        case ConnectionMode::MatrixOverlay:
            ApplyMatrixConnection(target);
            break;
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
        ResetTraversalMeshDeformation();
    }
}

void SpatialManipulator::PostPhysicsUpdate()
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
    UpdateTriggerTraversal(target);
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

    changed = ui.Checkbox("Defines Warp Volume", &definesWarpVolume) || changed;
    if (definesWarpVolume)
    {
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

    return changed;
}
}

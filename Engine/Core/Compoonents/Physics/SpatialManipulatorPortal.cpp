#include "SpatialManipulator.h"

#include "Core/Object.h"
#include "Core/Scene/Scene.h"
#include "Core/Scene/Spatial/PortalAperture.h"
#include "Core/Scene/Spatial/PortalMapping.h"
#include <algorithm>
#include <cmath>

namespace Engine::Components
{
namespace
{
glm::vec3 SafeNormalize(const glm::vec3& value, const glm::vec3& fallback)
{
    const float lengthSquared = glm::dot(value, value);
    return lengthSquared <= 1e-8f ? fallback :
        value / std::sqrt(lengthSquared);
}
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
    glm::mat4 worldMatrix(1.f);
    if (Owner)
        worldMatrix = Owner->transform.GetWorldMatrix();
    return Engine::Scene::Spatial::PortalAperture::BuildWorldPoints(
        localShape, worldMatrix, portalPoint, portalNormal);
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
    return Engine::Scene::Spatial::PortalAperture::IsValid(
        GetWorldPortalShapePoints(), GetPortalWorldFrame(), tolerance);
}

bool SpatialManipulator::IsWorldPointInsidePortalAperture(
    const glm::vec3& worldPoint, float margin) const
{
    return Engine::Scene::Spatial::PortalAperture::Contains(worldPoint,
        GetWorldPortalShapePoints(), GetPortalWorldFrame(), margin);
}

glm::mat4 SpatialManipulator::GetPortalWorldFrame() const
{
    glm::mat4 ownerWorld(1.f);
    if (Owner)
        ownerWorld = Owner->transform.GetWorldMatrix();
    return Engine::Scene::Spatial::PortalAperture::BuildFrame(ownerWorld,
        portalPoint, portalNormal);
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
    return Engine::Scene::Spatial::PortalMapping::BuildAffineTransform(
        GetWorldPortalShapePoints(),
        target.GetWorldPortalShapePoints(), GetPortalWorldFrame(),
        target.GetPortalWorldFrame());
}

float SpatialManipulator::GetPortalScaleRatioTo(
    const SpatialManipulator& target) const
{
    return Engine::Scene::Spatial::PortalMapping::ComputeScaleRatio(
        GetWorldPortalShapePoints(), target.GetWorldPortalShapePoints());
}

glm::mat4 SpatialManipulator::GetRenderPortalWorldTransformTo(
    const SpatialManipulator& target) const
{
    return Engine::Scene::Spatial::PortalMapping::BuildAffineTransform(
        GetRenderWorldPortalShapePoints(),
        target.GetRenderWorldPortalShapePoints(), GetRenderPortalWorldFrame(),
        target.GetRenderPortalWorldFrame());
}

float SpatialManipulator::GetRenderPortalScaleRatioTo(
    const SpatialManipulator& target) const
{
    return Engine::Scene::Spatial::PortalMapping::ComputeScaleRatio(
        GetRenderWorldPortalShapePoints(),
        target.GetRenderWorldPortalShapePoints());
}

bool SpatialManipulator::UsesPiecewisePortalWarpTo(
    const SpatialManipulator& target) const
{
    if (!HasCompatiblePortalShapeWith(target))
        return false;
    const Engine::Scene::Spatial::PortalMapping mapping(
        GetWorldPortalShapePoints(), target.GetWorldPortalShapePoints(),
        GetPortalWorldFrame(), target.GetPortalWorldFrame());
    return mapping.IsPiecewise();
}

glm::vec3 SpatialManipulator::MapWorldPointThroughPortalShape(const glm::vec3& point,
    const SpatialManipulator& target) const
{
    if (!HasCompatiblePortalShapeWith(target))
        return point;
    return Engine::Scene::Spatial::PortalMapping(
        GetWorldPortalShapePoints(), target.GetWorldPortalShapePoints(),
        GetPortalWorldFrame(), target.GetPortalWorldFrame()).MapPoint(point);
}

glm::vec3 SpatialManipulator::MapWorldDirectionThroughPortalShape(
    const glm::vec3& origin, const glm::vec3& direction,
    const SpatialManipulator& target) const
{
    if (!HasCompatiblePortalShapeWith(target))
        return direction;
    return Engine::Scene::Spatial::PortalMapping(GetWorldPortalShapePoints(),
        target.GetWorldPortalShapePoints(), GetPortalWorldFrame(),
        target.GetPortalWorldFrame()).MapDirection(origin, direction);
}

glm::vec3 SpatialManipulator::MapWorldNormalThroughPortalShape(
    const glm::vec3& origin, const glm::vec3& normal,
    const SpatialManipulator& target) const
{
    if (!HasCompatiblePortalShapeWith(target))
        return normal;
    return Engine::Scene::Spatial::PortalMapping(GetWorldPortalShapePoints(),
        target.GetWorldPortalShapePoints(), GetPortalWorldFrame(),
        target.GetPortalWorldFrame()).MapNormal(origin, normal);
}

glm::vec3 SpatialManipulator::MapRenderWorldPointThroughPortalShape(
    const glm::vec3& point, const SpatialManipulator& target) const
{
    if (!HasCompatiblePortalShapeWith(target))
        return point;
    return Engine::Scene::Spatial::PortalMapping(
        GetRenderWorldPortalShapePoints(),
        target.GetRenderWorldPortalShapePoints(), GetRenderPortalWorldFrame(),
        target.GetRenderPortalWorldFrame()).MapPoint(point);
}

glm::vec3 SpatialManipulator::MapRenderWorldDirectionThroughPortalShape(
    const glm::vec3& origin, const glm::vec3& direction,
    const SpatialManipulator& target) const
{
    if (!HasCompatiblePortalShapeWith(target))
        return direction;
    return Engine::Scene::Spatial::PortalMapping(
        GetRenderWorldPortalShapePoints(),
        target.GetRenderWorldPortalShapePoints(), GetRenderPortalWorldFrame(),
        target.GetRenderPortalWorldFrame()).MapDirection(origin, direction);
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
}

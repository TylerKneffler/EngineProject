#include "SpatialManipulator.h"
#include "Core/Object.h"
#include "Engine/Editor/UI/IEditorUi.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
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

float ComputeAverageRadius(const std::vector<glm::vec3>& points,
    const glm::vec3& center, const glm::vec3& tangent,
    const glm::vec3& bitangent)
{
    if (points.empty())
        return 1.f;
    float sum = 0.f;
    for (const glm::vec3& point : points)
    {
        const glm::vec3 delta = point - center;
        const glm::vec2 uv(glm::dot(delta, tangent), glm::dot(delta, bitangent));
        sum += glm::length(uv);
    }
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
        GetClampedPortalPointCount() >= 3;
}

glm::mat4 SpatialManipulator::GetPortalWorldFrame(float& averageRadius) const
{
    const std::vector<glm::vec3> worldPoints = GetWorldPortalShapePoints();
    const glm::vec3 center = ComputeCenter(worldPoints);

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

    averageRadius = std::max(1e-4f, ComputeAverageRadius(worldPoints,
        center, tangent, bitangent));

    glm::mat4 frame(1.f);
    frame[0] = glm::vec4(tangent, 0.f);
    frame[1] = glm::vec4(bitangent, 0.f);
    frame[2] = glm::vec4(normal, 0.f);
    frame[3] = glm::vec4(center, 1.f);
    return frame;
}

glm::mat4 SpatialManipulator::GetPortalWorldTransformTo(
    const SpatialManipulator& target) const
{
    float sourceRadius = 1.f;
    float targetRadius = 1.f;
    const glm::mat4 sourceFrame = GetPortalWorldFrame(sourceRadius);
    const glm::mat4 targetFrame = target.GetPortalWorldFrame(targetRadius);
    const float scaleRatio = targetRadius / sourceRadius;

    // Crossing a portal is a half-turn in portal-frame coordinates. Flipping
    // tangent and normal while retaining bitangent has positive determinant,
    // so orientations remain right-handed instead of becoming reflections.
    glm::mat4 crossing(1.f);
    crossing[0][0] = -scaleRatio;
    crossing[1][1] = scaleRatio;
    crossing[2][2] = -scaleRatio;
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
            if (it->second.baseVertices.size() == mesh->GetVertices().size())
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
    if (state.lastMesh != mesh ||
        state.baseVertices.size() != currentVertices.size())
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

    std::vector<Mesh::Vertex> deformed = state.baseVertices;
    bool hasDeformation = false;
    const float blendDistance = std::max(0.001f, traversalBlendDistance);
    const float strength = std::max(0.f, deformationStrength);

    for (size_t index = 0; index < deformed.size(); ++index)
    {
        const Mesh::Vertex& baseVertex = state.baseVertices[index];
        const glm::vec3 localVertex(baseVertex.pos[0], baseVertex.pos[1], baseVertex.pos[2]);
        const glm::vec3 worldVertex = glm::vec3(bodyWorld * glm::vec4(localVertex, 1.f));

        const float signedDistance = glm::dot(worldVertex - sourceWorldPortalPoint,
            sourceWorldPortalNormal);
        if (signedDistance <= 0.f)
            continue;

        const float blend = Clamp01(signedDistance / blendDistance) * strength;
        const glm::vec3 mappedWorld = MapWorldPointThroughPortalShape(worldVertex, *target);
        const glm::vec3 blendedWorld = glm::mix(worldVertex, mappedWorld, blend);
        const glm::vec3 blendedLocal = glm::vec3(bodyWorldInverse *
            glm::vec4(blendedWorld, 1.f));

        deformed[index].pos[0] = blendedLocal.x;
        deformed[index].pos[1] = blendedLocal.y;
        deformed[index].pos[2] = blendedLocal.z;
        hasDeformation = true;
    }

    if (hasDeformation)
    {
        mesh->SetDeformedVertices(deformed);
        state.meshDeformed = true;
    }
    else
    {
        ResetTraversalMeshDeformation(traversingBody);
    }
}

void SpatialManipulator::UpdateTriggerTraversal(SpatialManipulator* target)
{
    if (!target)
    {
        ResetTraversalMeshDeformation();
        return;
    }

    RigidBody* triggerBody = ResolveTraversalTriggerBody();
    if (!triggerBody)
    {
        ResetTraversalMeshDeformation();
        return;
    }

    std::unordered_set<const RigidBody*> activeBodies;
    for (RigidBody* body : triggerBody->GetOverlappingBodies())
    {
        if (!body || !body->Owner || body->Owner == Owner)
            continue;

        activeBodies.insert(body);
        TraversalState& state = m_traversalStates[body];
        const glm::vec3 bodyWorldPosition = body->Owner->transform.GetWorldPosition();
        const float currentSignedDistance = ComputeSignedDistanceToPortalPlane(
            bodyWorldPosition);
        constexpr float kPortalPlaneEpsilon = 1e-4f;
        const bool isAwayFromPortalPlane =
            std::abs(currentSignedDistance) > kPortalPlaneEpsilon;
        const bool crossedPortalPlane = state.hasLastSignedDistance &&
            isAwayFromPortalPlane &&
            ((state.lastSignedDistance < -kPortalPlaneEpsilon &&
                 currentSignedDistance > kPortalPlaneEpsilon) ||
                (state.lastSignedDistance > kPortalPlaneEpsilon &&
                    currentSignedDistance < -kPortalPlaneEpsilon));

        if (crossedPortalPlane)
        {
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
        }

        if (currentSignedDistance > 0.f)
            ApplyTraversalMeshDeformation(target, body);
        else
            ResetTraversalMeshDeformation(body);

        // Keep the last unambiguous side while the body is within the plane's
        // dead zone. This makes an exact-on-plane frame part of a crossing,
        // rather than treating it as a new starting side.
        if (isAwayFromPortalPlane)
        {
            state.lastSignedDistance = currentSignedDistance;
            state.hasLastSignedDistance = true;
        }
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

void SpatialManipulator::ConnectToTarget(SpatialManipulator* target)
{
    if (!target || target == this)
        return;

    EnsurePointCountCompatibility(target);
    targetManipulator = Engine::Core::CaptureComponentReference(target, "SpatialManipulator");
}

void SpatialManipulator::Disconnect()
{
    targetManipulator.Clear();
    if (Owner)
        Owner->transform.matrixLayer.connection.enabled = false;
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

    const glm::mat4 sourceToTarget = GetPortalWorldTransformTo(*target);
    const glm::mat4 targetToSource = glm::inverse(sourceToTarget);
    float sourceRadius = 1.f;
    float targetRadius = 1.f;
    const glm::mat4 sourceFrame = GetPortalWorldFrame(sourceRadius);
    const glm::mat4 targetFrame = target->GetPortalWorldFrame(targetRadius);

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
            UpdateTriggerTraversal(target);
            break;
        case ConnectionMode::None:
        default:
            ResetTraversalMeshDeformation();
            break;
        }
    }
    else
    {
        ResetTraversalMeshDeformation();
    }
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
    changed = ui.Checkbox("Deform Mesh On Traversal", &deformMeshOnTraversal) || changed;
    changed = ui.DragFloat("Traversal Blend Distance", &traversalBlendDistance,
        0.05f, 0.001f, 1000.f) || changed;
    changed = ui.DragFloat("Deformation Strength", &deformationStrength,
        0.05f, 0.f, 2.f) || changed;

    return changed;
}
}

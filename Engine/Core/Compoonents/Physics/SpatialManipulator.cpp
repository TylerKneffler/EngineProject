#include "SpatialManipulator.h"
#include "Core/Object.h"
#include "Core/Compoonents/Physics/Collider.h"
#include "Core/Physics/Physics.h"
#include "Core/Scene/Scene.h"
#include "Core/Scene/Spatial/WarpVolume.h"
#include "Engine/Editor/UI/IEditorUi.h"
#include <algorithm>
#include <cmath>
#include <functional>

namespace Engine::Components
{
namespace
{
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

Engine::Scene::Spatial::WarpVolumeDefinition BuildWarpVolumeDefinition(
    const SpatialManipulator& component)
{
    return {
        component.enabled,
        component.definesWarpVolume,
        static_cast<Engine::Scene::Spatial::WarpVolumeShape>(
            component.warpVolumeShape),
        component.warpVolumeSize,
        component.warpVolumeRadius,
        component.warpBoundaryFalloff,
        static_cast<Engine::Scene::Spatial::SpaceWarpType>(
            component.spaceWarpType),
        component.position,
        component.rotation,
        component.scale,
        component.spiralAxis,
        component.spiralRadiansPerUnit,
        component.formulaX,
        component.formulaY,
        component.formulaZ,
        component.formulaA,
        component.formulaB,
        component.formulaC,
        component.formulaD
    };
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
    return Engine::Scene::Spatial::WarpVolume::BuildOverlayMatrix(
        BuildWarpVolumeDefinition(*this));
}

bool SpatialManipulator::ContainsWorldPoint(const glm::vec3& worldPoint) const
{
    return Owner && Engine::Scene::Spatial::WarpVolume::Contains(
        BuildWarpVolumeDefinition(*this), Owner->transform.GetWorldMatrix(),
        worldPoint);
}

glm::vec3 SpatialManipulator::MapWorldPointThroughVolume(
    const glm::vec3& worldPoint) const
{
    if (!Owner)
        return worldPoint;
    return Engine::Scene::Spatial::WarpVolume::MapPoint(
        BuildWarpVolumeDefinition(*this), Owner->transform.GetWorldMatrix(),
        worldPoint);
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

    if (changed)
        MarkConfigurationDirty();
    return changed;
}
}

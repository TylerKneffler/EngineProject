#include "IKBone.h"

#include "Core/Compoonents/Animation/AnimationBone.h"
#include "Core/Compoonents/Animation/Skeleton.h"
#include "Core/Compoonents/Physics/Collider.h"
#include "Core/Object.h"
#include "Core/Physics/Internal/PhysicsInternal.h"
#include "Core/Scene/Scene.h"
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/matrix_decompose.hpp>
#include <glm/gtx/quaternion.hpp>
#include <algorithm>
#include <cctype>
#include <memory>

namespace Engine::Components
{
namespace
{
std::string Lower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char character)
        {
            return static_cast<char>(std::tolower(character));
        });
    return value;
}

glm::quat MatrixRotation(const glm::mat4& matrix)
{
    glm::mat3 basis(1.f);
    for (int column = 0; column < 3; ++column)
    {
        const glm::vec3 axis(matrix[column]);
        const float axisLength = glm::length(axis);
        if (axisLength > 0.00001f)
            basis[column] = axis / axisLength;
    }
    if (glm::determinant(basis) < 0.f)
        basis[0] = -basis[0];
    return glm::normalize(glm::quat_cast(basis));
}

btTransform RigidTransform(const glm::vec3& position, const glm::quat& rotation)
{
    btTransform result;
    result.setIdentity();
    result.setOrigin(Engine::Physics::ToBullet(position));
    result.setRotation(btQuaternion(rotation.x, rotation.y, rotation.z,
        rotation.w));
    return result;
}

glm::mat4 GlmTransform(const btTransform& transform)
{
    const btQuaternion value = transform.getRotation();
    const glm::quat rotation(value.w(), value.x(), value.y(), value.z());
    return glm::translate(glm::mat4(1.f),
        Engine::Physics::ToGlm(transform.getOrigin())) *
        glm::mat4_cast(rotation);
}
}

struct IKBone::Impl
{
    btDynamicsWorld* world = nullptr;
    std::unique_ptr<btCollisionShape> shape;
    std::unique_ptr<btDefaultMotionState> motionState;
    std::unique_ptr<btRigidBody> body;
    std::unique_ptr<btTypedConstraint> constraint;
    IKBone* constraintParent = nullptr;
    btTransform bodyToBone;
    const PrimitiveObjectCollider* collider = nullptr;
    uint64_t colliderRevision = 0;
};

IKBone::IKBone() : m_impl(new Impl())
{
    SetTypeName(COMPONENT_TYPE_NAME(IKBone));
    singlecomponent = true;
    RegisterField("simulate", simulate, "IK Bone");
    RegisterField("activationDelay", activationDelay, "IK Bone");
    RegisterField("weight", weight, "IK Bone");
    RegisterField("colliderReference", colliderReference, "IK Bone | Collider");
    RegisterField("mass", mass, "IK Bone | Body");
    RegisterField("friction", friction, "IK Bone | Body");
    RegisterField("linearDamping", linearDamping, "IK Bone | Body");
    RegisterField("angularDamping", angularDamping, "IK Bone | Body");
    RegisterField("initialLinearVelocity", initialLinearVelocity,
        "IK Bone | Body");
    RegisterField("initialAngularVelocity", initialAngularVelocity,
        "IK Bone | Body");
    RegisterField("connectToParent", connectToParent,
        "IK Bone | Joint");
    RegisterField("jointType", jointType, "IK Bone | Joint");
    RegisterField("swingLimit", swingLimit, "IK Bone | Joint");
    RegisterField("twistLimit", twistLimit, "IK Bone | Joint");
    RegisterField("hingeLimit", hingeLimit, "IK Bone | Joint");
    RegisterField("springStiffness", springStiffness,
        "IK Bone | Joint | Spring");
    RegisterField("springDamping", springDamping,
        "IK Bone | Joint | Spring");
    RegisterField("collideWithParent", collideWithParent,
        "IK Bone | Joint");
}

IKBone::~IKBone()
{
    DestroyConstraint();
    DestroyBody();
    delete m_impl;
}

void IKBone::Start()
{
    m_elapsed = 0.f;
}

void IKBone::AdvanceActivation(float deltaTime)
{
    if (simulate && !IsSimulating())
        m_elapsed += std::max(deltaTime, 0.f);
}

bool IKBone::WantsSimulation() const
{
    if (!simulate || !Owner || !Owner->IsEnabledInHierarchy() ||
        m_elapsed < std::max(activationDelay, 0.f)) return false;
    const AnimationBone* bone = Owner->GetComponent<AnimationBone>();
    for (Object* ancestor = Owner; ancestor; ancestor = ancestor->Parent)
        if (auto* skeleton = ancestor->GetComponent<Skeleton>();
            skeleton && bone && skeleton->skinIndex == bone->skinIndex)
            return !skeleton->UsesWholeMeshCollider();
    return true;
}

Engine::Core::Object* IKBone::FindSegmentChild(
    const std::string& childName) const
{
    if (!Owner)
        return nullptr;
    AnimationBone* hierarchyBone = Owner->GetComponent<AnimationBone>();
    if (!hierarchyBone)
        return nullptr;
    for (AnimationBone* child : hierarchyBone->GetChildBones())
        if (child && child->Owner &&
            ((!childName.empty() && child->Owner->name == childName) ||
                (childName.empty() && child->Owner->GetComponent<IKBone>())))
            return child->Owner;
    return nullptr;
}

IKBone* IKBone::FindParentIKBone() const
{
    AnimationBone* hierarchyBone = Owner
        ? Owner->GetComponent<AnimationBone>() : nullptr;
    for (AnimationBone* parent = hierarchyBone
            ? hierarchyBone->GetParentBone() : nullptr;
        parent; parent = parent->GetParentBone())
        if (parent->Owner)
            if (IKBone* bone = parent->Owner->GetComponent<IKBone>())
                return bone;
    return nullptr;
}

bool IKBone::EnsureBody()
{
    if (!WantsSimulation() || !Owner ||
        !Owner->GetScene() || !Owner->GetComponent<AnimationBone>())
        return IsSimulating();

    PrimitiveObjectCollider* collider = colliderReference.IsAssigned()
        ? Engine::Core::ResolveComponentReference<PrimitiveObjectCollider>(
            Owner, colliderReference) : nullptr;
    if (!colliderReference.IsAssigned())
        for (Component* component : Owner->Components)
            if (auto* candidate = dynamic_cast<PrimitiveObjectCollider*>(component);
                candidate && candidate->collisionEnabled)
            {
                collider = candidate;
                break;
            }
    if (!collider || collider->Owner != Owner || !collider->collisionEnabled)
    {
        if (IsSimulating()) DestroyBody();
        return false;
    }
    if (IsSimulating() && m_impl->collider == collider &&
        m_impl->colliderRevision == collider->GetConfigurationRevision())
        return true;
    if (IsSimulating()) DestroyBody();

    const glm::mat4 boneMatrix = Owner->transform.GetWorldMatrix();
    const glm::vec3 bonePosition(boneMatrix[3]);
    const glm::quat boneRotation = MatrixRotation(boneMatrix);
    Engine::Core::Object* segmentChild = collider->alignToBoneChild
        ? FindSegmentChild(collider->childBone) : nullptr;
    glm::vec3 direction = boneRotation * glm::vec3(1.f, 0.f, 0.f);
    float segmentLength = std::max(collider->height, 0.f);
    if (segmentChild)
    {
        const glm::vec3 childPosition = segmentChild->transform.GetWorldPosition();
        const glm::vec3 delta = childPosition - bonePosition;
        if (glm::length(delta) > 0.00001f)
        {
            direction = glm::normalize(delta);
            if (segmentLength <= 0.f)
                segmentLength = glm::length(delta);
        }
    }

    const std::string shape = Lower(collider->shape);
    const float bodyRadius = std::max(collider->radius, 0.001f);
    const bool sphere = shape == "sphere" || shape == "circle" ||
        (shape == "capsule" && segmentLength <= 0.001f);
    const bool alongChild = collider->alignToBoneChild && !sphere &&
        (shape == "capsule" || shape == "cylinder");
    const glm::vec3 center = bonePosition +
        boneRotation * collider->center +
        (alongChild ? direction * segmentLength * 0.5f : glm::vec3(0.f));
    const glm::quat bodyRotation = alongChild ? glm::normalize(
        glm::rotation(glm::vec3(0.f, 1.f, 0.f), direction)) : boneRotation;
    const btTransform bodyWorld = RigidTransform(center, bodyRotation);
    const btTransform boneWorld = RigidTransform(bonePosition, boneRotation);

    if (sphere)
        m_impl->shape = std::make_unique<btSphereShape>(bodyRadius);
    else if (shape == "cylinder")
        m_impl->shape = std::make_unique<btCylinderShape>(btVector3(
            bodyRadius, std::max(segmentLength, 0.001f) * 0.5f, bodyRadius));
    else if (shape == "box" || shape == "cube")
    {
        const glm::vec3 halfSize = glm::max(glm::abs(collider->size) * 0.5f,
            glm::vec3(0.0005f));
        m_impl->shape = std::make_unique<btBoxShape>(
            Engine::Physics::ToBullet(halfSize));
    }
    else
        m_impl->shape = std::make_unique<btCapsuleShape>(bodyRadius,
            std::max(segmentLength - bodyRadius * 2.f, 0.001f));
    btVector3 inertia(0.f, 0.f, 0.f);
    const float bodyMass = std::max(mass, 0.001f);
    m_impl->shape->calculateLocalInertia(bodyMass, inertia);
    m_impl->motionState = std::make_unique<btDefaultMotionState>(bodyWorld);
    btRigidBody::btRigidBodyConstructionInfo info(bodyMass,
        m_impl->motionState.get(), m_impl->shape.get(), inertia);
    m_impl->body = std::make_unique<btRigidBody>(info);
    m_impl->body->setDamping(std::clamp(linearDamping, 0.f, 1.f),
        std::clamp(angularDamping, 0.f, 1.f));
    m_impl->body->setFriction(std::clamp(friction, 0.f, 1.f));
    m_impl->body->setLinearVelocity(
        Engine::Physics::ToBullet(initialLinearVelocity));
    m_impl->body->setAngularVelocity(
        Engine::Physics::ToBullet(initialAngularVelocity));
    m_impl->body->setActivationState(DISABLE_DEACTIVATION);
    m_impl->bodyToBone = bodyWorld.inverse() * boneWorld;
    m_impl->world = Engine::Physics::StateFor(Owner->GetScene()).world.get();
    m_impl->world->addRigidBody(m_impl->body.get());
    m_impl->collider = collider;
    m_impl->colliderRevision = collider->GetConfigurationRevision();
    return true;
}

bool IKBone::EnsureConstraint()
{
    if (m_impl->constraint || !m_impl->body || !connectToParent)
        return static_cast<bool>(m_impl->constraint) || !connectToParent;
    IKBone* parent = FindParentIKBone();
    if (!parent || !parent->m_impl->body || parent->m_impl->world != m_impl->world)
        return false;

    const glm::mat4 jointMatrix = Owner->transform.GetWorldMatrix();
    const btTransform jointWorld = RigidTransform(glm::vec3(jointMatrix[3]),
        MatrixRotation(jointMatrix));
    const btTransform frameA = parent->m_impl->body->getWorldTransform().inverse()
        * jointWorld;
    const btTransform frameB = m_impl->body->getWorldTransform().inverse()
        * jointWorld;
    const std::string type = Lower(jointType);
    if (type == "fixed")
    {
        m_impl->constraint = std::make_unique<btFixedConstraint>(
            *parent->m_impl->body, *m_impl->body, frameA, frameB);
    }
    else if (type == "hinge")
    {
        auto hinge = std::make_unique<btHingeConstraint>(
            *parent->m_impl->body, *m_impl->body, frameA, frameB);
        const float limit = std::clamp(hingeLimit, 0.f, 3.14159f);
        hinge->setLimit(-limit, limit);
        m_impl->constraint = std::move(hinge);
    }
    else if (type == "spring")
    {
        auto spring = std::make_unique<btGeneric6DofSpring2Constraint>(
            *parent->m_impl->body, *m_impl->body, frameA, frameB);
        // The joint anchors may not translate. Axis 3 is twist and axes 4/5
        // are swing in the joint frame, matching the ConeTwist controls.
        spring->setLinearLowerLimit(btVector3(0.f, 0.f, 0.f));
        spring->setLinearUpperLimit(btVector3(0.f, 0.f, 0.f));
        const float swing = std::clamp(swingLimit, 0.f, 3.14159f);
        const float twist = std::clamp(twistLimit, 0.f, 3.14159f);
        spring->setAngularLowerLimit(btVector3(-twist, -swing, -swing));
        spring->setAngularUpperLimit(btVector3(twist, swing, swing));
        for (int axis = 3; axis < 6; ++axis)
        {
            spring->enableSpring(axis, true);
            spring->setStiffness(axis, std::max(springStiffness, 0.f));
            spring->setDamping(axis,
                std::clamp(springDamping, 0.f, 1.f));
        }
        // The authored orientation at activation is the rest pose.
        spring->setEquilibriumPoint();
        m_impl->constraint = std::move(spring);
    }
    else
    {
        auto cone = std::make_unique<btConeTwistConstraint>(
            *parent->m_impl->body, *m_impl->body, frameA, frameB);
        cone->setLimit(std::clamp(swingLimit, 0.f, 3.14159f),
            std::clamp(swingLimit, 0.f, 3.14159f),
            std::clamp(twistLimit, 0.f, 3.14159f));
        m_impl->constraint = std::move(cone);
    }
    m_impl->constraintParent = parent;
    m_impl->world->addConstraint(m_impl->constraint.get(),
        !collideWithParent);
    return true;
}

void IKBone::RemoveInvalidConstraint()
{
    if (m_impl->constraint && (!WantsSimulation() ||
        !m_impl->constraintParent ||
        !m_impl->constraintParent->WantsSimulation()))
        DestroyConstraint(true);
}

bool IKBone::HasManualEditInHierarchy() const
{
    for (const Object* object = Owner; object; object = object->Parent)
        if (object->transform.HasEditorOverride(
                object == Owner
                    ? Transform::EditorPosition | Transform::EditorRotation
                    : Transform::EditorAll))
            return true;
    return false;
}

void IKBone::SyncBodyFromBone()
{
    if (!m_impl->body || !Owner || !HasManualEditInHierarchy())
        return;
    const glm::mat4 boneMatrix = Owner->transform.GetWorldMatrix();
    const btTransform simulatedBoneWorld =
        m_impl->body->getWorldTransform() * m_impl->bodyToBone;
    bool inheritedOverride = false;
    for (const Object* ancestor = Owner->Parent; ancestor;
        ancestor = ancestor->Parent)
        inheritedOverride = inheritedOverride ||
            ancestor->transform.HasEditorOverride();
    const bool overridePosition = inheritedOverride ||
        Owner->transform.HasEditorOverride(Transform::EditorPosition);
    const bool overrideRotation = inheritedOverride ||
        Owner->transform.HasEditorOverride(Transform::EditorRotation);
    const btQuaternion simulatedRotation = simulatedBoneWorld.getRotation();
    const glm::quat simulatedGlmRotation(simulatedRotation.w(),
        simulatedRotation.x(), simulatedRotation.y(), simulatedRotation.z());
    const btTransform boneWorld = RigidTransform(
        overridePosition ? glm::vec3(boneMatrix[3])
            : Engine::Physics::ToGlm(simulatedBoneWorld.getOrigin()),
        overrideRotation ? MatrixRotation(boneMatrix) : simulatedGlmRotation);
    const btTransform bodyWorld = boneWorld * m_impl->bodyToBone.inverse();
    m_impl->body->setWorldTransform(bodyWorld);
    m_impl->body->setInterpolationWorldTransform(bodyWorld);
    if (m_impl->motionState)
        m_impl->motionState->setWorldTransform(bodyWorld);
    if (overridePosition)
        m_impl->body->setLinearVelocity(btVector3(0.f, 0.f, 0.f));
    if (overrideRotation)
        m_impl->body->setAngularVelocity(btVector3(0.f, 0.f, 0.f));
    m_impl->body->activate(true);
    if (m_impl->world)
        m_impl->world->updateSingleAabb(m_impl->body.get());
}

void IKBone::SyncBoneFromBody()
{
    if (!m_impl->body || !Owner)
        return;
    const bool overridePosition = Owner->transform.HasEditorOverride(
        Transform::EditorPosition);
    const bool overrideRotation = Owner->transform.HasEditorOverride(
        Transform::EditorRotation);
    const float influence = std::clamp(weight, 0.f, 1.f);
    if (influence <= 0.f)
        return;
    const btTransform boneWorld = m_impl->body->getWorldTransform() *
        m_impl->bodyToBone;
    const glm::mat4 desiredWorld = GlmTransform(boneWorld);
    // The first simulated joint carries the whole character. Keep the rig
    // origin with that joint so the object transform, root joint, and mesh
    // travel with the ragdoll instead of remaining at the spawn position.
    AnimationBone* hierarchyBone = Owner->GetComponent<AnimationBone>();
    AnimationBone* parentBone = hierarchyBone
        ? hierarchyBone->GetParentBone() : nullptr;
    if (hierarchyBone &&
        (hierarchyBone->hierarchyRoot ||
            (parentBone && parentBone->hierarchyRoot)) &&
        !FindParentIKBone())
    {
        for (Object* ancestor = Owner->Parent; ancestor;
            ancestor = ancestor->Parent)
        {
            if (ancestor->transform.HasEditorOverride(Transform::EditorPosition))
                break;
            if (auto* skeleton = ancestor->GetComponent<Skeleton>();
                skeleton && skeleton->skinIndex == hierarchyBone->skinIndex)
            {
                const glm::vec3 currentBonePosition =
                    Owner->transform.GetWorldPosition();
                const glm::vec3 rootWorldPosition =
                    ancestor->transform.GetWorldPosition();
                const glm::vec3 desiredRootWorldPosition =
                    rootWorldPosition +
                    (glm::vec3(desiredWorld[3]) - currentBonePosition) * influence;
                const glm::mat4 rootParentWorld = ancestor->Parent
                    ? ancestor->Parent->transform.GetWorldMatrix()
                    : glm::mat4(1.f);
                ancestor->transform.position = glm::vec3(
                    glm::inverse(rootParentWorld) *
                    glm::vec4(desiredRootWorldPosition, 1.f));
                ancestor->transform.MarkDirty();
                break;
            }
        }
    }
    const glm::mat4 parentWorld = Owner->Parent
        ? Owner->Parent->transform.GetWorldMatrix() : glm::mat4(1.f);
    const glm::mat4 local = glm::inverse(parentWorld) * desiredWorld;
    glm::vec3 scale, translation, skew;
    glm::vec4 perspective;
    glm::quat rotation;
    if (glm::decompose(local, scale, rotation, translation, skew, perspective))
    {
        const glm::quat animationRotation = glm::normalize(
            glm::quat(Owner->transform.rotation));
        const glm::quat simulatedRotation = glm::normalize(rotation);
        if (!overridePosition)
            Owner->transform.position = glm::mix(
                Owner->transform.position, translation, influence);
        if (!overrideRotation)
            Owner->transform.rotation = glm::eulerAngles(glm::normalize(
                glm::slerp(animationRotation, simulatedRotation, influence)));
        Owner->transform.MarkDirty();
    }
}

bool IKBone::IsSimulating() const
{
    return m_impl && static_cast<bool>(m_impl->body);
}

void IKBone::DestroyConstraint(bool removeFromWorld)
{
    if (!m_impl)
        return;
    if (removeFromWorld && m_impl->world && m_impl->constraint)
        m_impl->world->removeConstraint(m_impl->constraint.get());
    m_impl->constraint.reset();
    m_impl->constraintParent = nullptr;
}

void IKBone::DestroyBody(bool removeFromWorld)
{
    if (!m_impl)
        return;
    DestroyConstraint(removeFromWorld);
    // A child's joint is owned by the child, but references this body. Remove
    // those joints before releasing the body when one bone is disabled alone.
    if (removeFromWorld && Owner && Owner->GetScene())
        for (const auto& object : Owner->GetScene()->GetObjects())
            for (Engine::Core::Component* component : object->Components)
                if (auto* child = dynamic_cast<IKBone*>(component);
                    child && child->m_impl->constraintParent == this)
                    child->DestroyConstraint(true);
    if (removeFromWorld && m_impl->world && m_impl->body)
        m_impl->world->removeRigidBody(m_impl->body.get());
    m_impl->body.reset();
    m_impl->motionState.reset();
    m_impl->shape.reset();
    m_impl->collider = nullptr;
    m_impl->colliderRevision = 0;
    m_impl->world = nullptr;
}

void IKBone::ResetSimulation()
{
    DestroyConstraint();
    DestroyBody();
    m_elapsed = 0.f;
}

void IKBone::Disabled()
{
    DestroyConstraint();
    DestroyBody();
}

void IKBone::OnDestroy()
{
    DestroyConstraint();
    DestroyBody();
}
}

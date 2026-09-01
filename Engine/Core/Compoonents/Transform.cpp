#include "Transform.h"
#include "Core/Object.h"
#include "Core/Scene/Scene.h"
#include <glm/gtc/matrix_transform.hpp>

namespace Engine::Components
{
Transform::Transform()
{
    SetTypeName(COMPONENT_TYPE_NAME(Transform));
    RegisterField("position", position);
    RegisterField("rotation", rotation);
    RegisterField("scale", scale);
}

Transform::Transform(const Transform& other)
    : Transform()
{
    position = other.position;
    rotation = other.rotation;
    scale = other.scale;
}

Transform& Transform::operator=(const Transform& other)
{
    if (this == &other)
        return *this;

    // Keep this instance's registered field callbacks and owner. They must
    // continue to reference this Transform rather than the copied source.
    position = other.position;
    rotation = other.rotation;
    scale = other.scale;
    MarkDirty();
    return *this;
}

namespace
{
Engine::Serialization::JsonValue JVec3(const glm::vec3& v)
{
    return Engine::Serialization::JsonValue::MakeArray()
        .Push(Engine::Serialization::JsonValue(v.x))
        .Push(Engine::Serialization::JsonValue(v.y))
        .Push(Engine::Serialization::JsonValue(v.z));
}

glm::vec3 Vec3From(const Engine::Serialization::JsonValue& v, const glm::vec3& def)
{
    if (!v.IsArray() || v.ArraySize() < 3)
        return def;
    return { v.ArrayAt(0).AsFloat(), v.ArrayAt(1).AsFloat(), v.ArrayAt(2).AsFloat() };
}
}

glm::vec3 Transform::GetLocalPosition() const
{
    return position;
}

glm::vec3 Transform::GetWorldPosition() const
{
    return glm::vec3(GetWorldMatrix()[3]);
}

glm::mat4 Transform::GetLocalMatrixWithLayer() const
{
    const glm::mat4 base = GetLocalMatrix();
    if (!matrixLayer.enabled)
        return base;
    return matrixLayer.localToLayer * base;
}

glm::mat4 Transform::GetWorldMatrixWithLayer() const
{
    glm::mat4 world = GetWorldMatrix();
    if (matrixLayer.enabled)
        world = matrixLayer.localToLayer * world;
    if (Owner && Owner->GetScene())
        world = Owner->GetScene()->MapSpatialMatrix(world,
            { Engine::Scene::Scene::SpatialQueryDomain::Rendering, Owner });
    return world;
}

glm::vec3 Transform::ApplyLocalMatrixLayer(const glm::vec3& point) const
{
    return matrixLayer.TransformPoint(point);
}

glm::vec3 Transform::InverseApplyLocalMatrixLayer(const glm::vec3& point) const
{
    return matrixLayer.InverseTransformPoint(point);
}

glm::vec3 Transform::ApplyWorldMatrixLayer(const glm::vec3& point) const
{
    const glm::vec3 local = point;
    const glm::vec3 layered = ApplyLocalMatrixLayer(local);
    return GetWorldPosition() + (layered - GetLocalPosition());
}

glm::vec3 Transform::InverseApplyWorldMatrixLayer(const glm::vec3& point) const
{
    const glm::vec3 relative = point - GetWorldPosition();
    return InverseApplyLocalMatrixLayer(relative + GetLocalPosition());
}

void Transform::AdvanceRevision(uint64_t& revision)
{
    if (++revision == 0)
        ++revision;
}

void Transform::MarkDirty()
{
    AdvanceRevision(m_localRevision);
    m_localCacheInitialized = false;
}

void Transform::UpdateLocalCache() const
{
    const bool valuesChanged = m_localCacheInitialized &&
        (position != m_cachedPosition || rotation != m_cachedRotation ||
            scale != m_cachedScale);
    if (valuesChanged)
        AdvanceRevision(m_localRevision);

    if (m_localCacheInitialized && !valuesChanged)
        return;

    glm::mat4 t  = glm::translate(glm::mat4(1.f), position);
    glm::mat4 rx = glm::rotate(glm::mat4(1.f), rotation.x, { 1.f, 0.f, 0.f });
    glm::mat4 ry = glm::rotate(glm::mat4(1.f), rotation.y, { 0.f, 1.f, 0.f });
    glm::mat4 rz = glm::rotate(glm::mat4(1.f), rotation.z, { 0.f, 0.f, 1.f });
    glm::mat4 s  = glm::scale(glm::mat4(1.f), scale);
    m_cachedLocalMatrix = t * rz * ry * rx * s;
    m_cachedPosition = position;
    m_cachedRotation = rotation;
    m_cachedScale = scale;
    m_localCacheInitialized = true;
}

glm::mat4 Transform::GetLocalMatrix() const
{
    UpdateLocalCache();
    return m_cachedLocalMatrix;
}

uint64_t Transform::GetLocalRevision() const
{
    UpdateLocalCache();
    return m_localRevision;
}

void Transform::UpdateWorldCache() const
{
    UpdateLocalCache();

    const Engine::Core::Object* parent = Owner ? Owner->Parent : nullptr;
    glm::mat4 parentWorld(1.f);
    uint64_t parentRevision = 0;
    if (parent)
    {
        parentWorld = parent->transform.GetWorldMatrix();
        // GetWorldMatrix() has already validated the complete ancestor chain.
        // Transform instances may inspect each other's private cache state.
        parentRevision = parent->transform.m_worldRevision;
    }

    const bool dependenciesChanged = m_worldCacheInitialized &&
        (m_cachedLocalRevisionForWorld != m_localRevision ||
            m_cachedParent != parent ||
            m_cachedParentWorldRevision != parentRevision);
    if (m_worldCacheInitialized && !dependenciesChanged)
        return;

    m_cachedWorldMatrix = parentWorld * m_cachedLocalMatrix;
    m_cachedLocalRevisionForWorld = m_localRevision;
    m_cachedParent = parent;
    m_cachedParentWorldRevision = parentRevision;
    if (dependenciesChanged)
        AdvanceRevision(m_worldRevision);
    m_worldCacheInitialized = true;
}

glm::mat4 Transform::GetWorldMatrix() const
{
    UpdateWorldCache();
    return m_cachedWorldMatrix;
}

uint64_t Transform::GetWorldRevision() const
{
    UpdateWorldCache();
    return m_worldRevision;
}
}

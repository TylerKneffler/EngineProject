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
    return glm::vec3(position);
}

glm::vec3 Transform::GetWorldPosition() const
{
    return glm::vec3(GetWorldMatrix()[3]);
}

glm::dvec3 Transform::GetWorldPositionD() const
{
    return glm::dvec3(GetWorldMatrixD()[3]);
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
    // Matrix layers live in world space at this boundary.  Treating the
    // input as an offset from this transform's position silently drops the
    // layer's rotation and scale (and is wrong for a rotated parent).
    return matrixLayer.TransformPoint(point);
}

glm::vec3 Transform::InverseApplyWorldMatrixLayer(const glm::vec3& point) const
{
    return matrixLayer.InverseTransformPoint(point);
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

void Transform::NotifyEditorTransformChanged(uint8_t channels)
{
    MarkDirty();
    m_editorOverrideChannels |= channels;
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

    glm::dmat4 t  = glm::translate(glm::dmat4(1.0), position);
    glm::dmat4 rxD = glm::rotate(glm::dmat4(1.0),
        static_cast<double>(rotation.x), glm::dvec3(1.0, 0.0, 0.0));
    glm::dmat4 ryD = glm::rotate(glm::dmat4(1.0),
        static_cast<double>(rotation.y), glm::dvec3(0.0, 1.0, 0.0));
    glm::dmat4 rzD = glm::rotate(glm::dmat4(1.0),
        static_cast<double>(rotation.z), glm::dvec3(0.0, 0.0, 1.0));
    glm::dmat4 sD = glm::scale(glm::dmat4(1.0), glm::dvec3(scale));
    m_cachedLocalMatrixD = t * rzD * ryD * rxD * sD;
    m_cachedLocalMatrix = glm::mat4(m_cachedLocalMatrixD);
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
    glm::dmat4 parentWorld(1.0);
    uint64_t parentRevision = 0;
    if (parent)
    {
        parentWorld = parent->transform.GetWorldMatrixD();
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

    m_cachedWorldMatrixD = parentWorld * m_cachedLocalMatrixD;
    m_cachedWorldMatrix = glm::mat4(m_cachedWorldMatrixD);
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

glm::dmat4 Transform::GetWorldMatrixD() const
{
    UpdateWorldCache();
    return m_cachedWorldMatrixD;
}

uint64_t Transform::GetWorldRevision() const
{
    UpdateWorldCache();
    return m_worldRevision;
}
}
